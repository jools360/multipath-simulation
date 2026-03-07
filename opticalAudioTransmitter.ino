// Teensy 4.1 — QPSK I/Q over S/PDIF (pin 14), 48 kHz / 16-bit
// REDUCED PAYLOAD VERSION: Strips constant bytes before transmission
// - Constant header (bytes 0-7) and FIZ status (bytes 60-63) are stripped
// - Only variable bytes (8-59) + CRC (64-65) = 54 bytes are transmitted
// - Receiver reconstructs the full 66-byte message
//
// - Payload can come from USB CDC or Serial1 (VIPS: 66 bytes full message)
// - Flashes LED on Pin 3 upon successful decode
// - Pin 2 goes HIGH at start-of-frame, LOW 1 ms later (non-blocking)

#include <Arduino.h>
#define AUDIO_SAMPLE_RATE_EXACT 48000.0
#include <Audio.h>
#include "output_spdif3.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <SPI.h>

// ---------- SPI / DAC pins ----------
static const int DAC_CS_PIN = 10;
static SPISettings dacSPI(20000000, MSBFIRST, SPI_MODE0);

// ---------- Timer for steady 48 kS/s DAC writes ----------
IntervalTimer dacTimer;

// ---------- Constants ----------
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

uint8_t pindex = 0;
uint8_t ledCount = 0;

static const float FS    = 48000.0f;
static const float RS    = 16000.0f;
static const int   SPS   = 3;
static const float ALPHA = 0.35f;
static const int   SPAN  = 5;
static const int   TAPS  = 2*SPAN*SPS + 1; // 31

static const int   PRE_SYMS = 16;

// ============ REDUCED PAYLOAD CONSTANTS ============
// Full incoming message size
static const int FULL_PAYLOAD_BYTES = 66;

// Constant header bytes (0-7): Sync word + Byte count + Mask flags
static const uint8_t CONST_HEADER[] = { 0x24, 0xD9, 0x42, 0x00, 0x46, 0x24, 0x00, 0x00 };
static const int CONST_HEADER_LEN = 8;

// Constant FIZ status bytes (60-63)
static const uint8_t CONST_FIZ_STATUS[] = { 0x06, 0x01, 0x02, 0x00 };
static const int CONST_FIZ_STATUS_LEN = 4;
static const int CONST_FIZ_STATUS_OFFSET = 60;

// Variable data: bytes 8-59 = 52 bytes
static const int VAR_DATA_OFFSET = 8;
static const int VAR_DATA_LEN = 52;

// CRC: bytes 64-65 = 2 bytes
static const int CRC_OFFSET = 64;
static const int CRC_LEN = 2;

// Reduced payload = variable data + CRC = 54 bytes
static const int REDUCED_PAYLOAD_BYTES = VAR_DATA_LEN + CRC_LEN; // 54

// SOF bytes for detecting incoming FULL messages (first 3 bytes)
static const uint8_t SOF_BYTES[] = { 0x24, 0xD9, 0x42 };
static const int SOF_LEN = 3;

// What we actually transmit over QPSK (reduced)
static const int PAYLOAD_BYTES = REDUCED_PAYLOAD_BYTES; // 54 bytes transmitted

static const int TOTAL_SYMS    = PRE_SYMS + (PAYLOAD_BYTES*8)/2;
static const int UPLEN         = TOTAL_SYMS * SPS;
static const int OUTLEN        = UPLEN + (TAPS - 1);

static const float TARGET_HZ     = 60.0f;
static const int   FRAME_SAMPLES = (int)lroundf(FS / TARGET_HZ);
static const uint32_t FRAME_US   = (uint32_t)(1000000.0f / TARGET_HZ);

static const int   GUARD_SILENCE = 0;

// ---------- Trigger pin ----------
static const int TRIG_PIN = 2;
volatile bool     trig_on     = false;
volatile uint32_t trig_due_us = 0;

// ---------- Status LED (Using Pin 3, as Pin 13 is SPI SCK) ----------
static const int STATUS_LED_PIN = 3;
static unsigned long led_turn_off_at = 0;

// ---------- Per-channel adjustable delays (signed) ----------
volatile int DELAY_I_SAMP = 0;
volatile int DELAY_Q_SAMP = 0;

// ---------- TX gate ----------
static volatile bool g_tx_active = false;

// ---------- RRC taps ----------
static float rrc[TAPS];

// ---------- Double frame buffers + swap state ----------
static int16_t frameI_A[FRAME_SAMPLES];
static int16_t frameQ_A[FRAME_SAMPLES];
static int16_t frameI_B[FRAME_SAMPLES];
static int16_t frameQ_B[FRAME_SAMPLES];
static volatile const int16_t* nextI = nullptr;
static volatile const int16_t* nextQ = nullptr;
static volatile bool           swapPending = false;
static bool                    useA = true;

// ---------- USB ingest ----------
static volatile bool g_usb_mode = true;
// Store the REDUCED payload for transmission
static uint8_t       g_extPayload[REDUCED_PAYLOAD_BYTES];
static volatile bool g_have_extPayload = false;

// ---------- Payload bit capture (optional) ----------
static uint8_t g_payload_Ibits[(PAYLOAD_BYTES*8)/2];
static uint8_t g_payload_Qbits[(PAYLOAD_BYTES*8)/2];

// ---------- DAC ring buffer (I/Q interleaved by index) ----------
#define RB_LOG2 11
#define RB_SIZE (1u << RB_LOG2)
#define RB_MASK (RB_SIZE - 1u)
static int16_t rbI[RB_SIZE];
static int16_t rbQ[RB_SIZE];
static volatile uint32_t rb_wr = 0;
static volatile uint32_t rb_rd = 0;
static volatile int16_t  lastI = 0, lastQ = 0;

static inline bool rb_push(int16_t si, int16_t sq) {
  uint32_t wr = rb_wr;
  uint32_t next = (wr + 1) & RB_MASK;
  if (next == rb_rd) {
    rb_rd = (rb_rd + 1) & RB_MASK;
  }
  rbI[wr] = si;
  rbQ[wr] = sq;
  rb_wr = next;
  return true;
}

// ---------- Simple LFSR scrambler (symmetric) ----------
static uint16_t lfsr16_next(uint16_t s) {
  uint16_t feedback = ((s >> 15) ^ (s >> 13) ^ (s >> 12) ^ (s >> 10)) & 0x1u;
  return (uint16_t)((s << 1) | feedback);
}

static void scramble_payload_inplace(uint8_t* buf, int len) {
  uint16_t s = 0xACE1u;
  for (int i = 0; i < len; ++i) {
    s = lfsr16_next(s);
    buf[i] ^= (uint8_t)(s & 0xFFu);
  }
}

static inline bool rb_pop(int16_t &si, int16_t &sq) {
  uint32_t rd = rb_rd;
  if (rd == rb_wr) return false;
  si = rbI[rd];
  sq = rbQ[rd];
  rb_rd = (rd + 1) & RB_MASK;
  return true;
}

// ---------- Utils ----------
static void make_rrc(double a, int sps, int span, float *h) {
  const int N = span * sps;
  double E = 0.0; int p = 0;
  for (int n = -N; n <= N; ++n) {
    double t = (double)n / sps, v;
    if (fabs(t) < 1e-12) {
      v = (1.0 - a) + 4.0 * a / M_PI;
    } else if (a > 0 && fabs(fabs(t) - 1.0 / (4.0 * a)) < 1e-12) {
      v = (a / sqrt(2.0)) * ((1.0 + 2.0 / M_PI) * sin(M_PI / (4.0 * a))
          + (1.0 - 2.0 / M_PI) * cos(M_PI / (4.0 * a)));
    } else {
      double num = sin(M_PI * t * (1.0 - a)) + 4.0 * a * t * cos(M_PI * t * (1.0 + a));
      double den = M_PI * t * (1.0 - pow(4.0 * a * t, 2.0));
      v = num / den;
    }
    h[p++] = (float)v; E += v*v;
  }
  float sc = (float)(1.0 / sqrt(E));
  for (int i = 0; i < TAPS; ++i) h[i] *= sc;
}

static void finalize_into_buffers(const double* yi, const double* yq,
                                  int16_t* outI, int16_t* outQ) {
  double peak = 1e-12;
  for (int n = 0; n < OUTLEN; ++n) {
    double mag = yi[n]*yi[n] + yq[n]*yq[n];
    if (mag > peak) peak = mag;
  }
  peak = sqrt(peak);
  const double sc = 32767.0 / peak;
  for (int n = 0; n < FRAME_SAMPLES; ++n) { outI[n] = 0; outQ[n] = 0; }

  int g = GUARD_SILENCE;
  if (g < 0) g = 0;
  if (g > FRAME_SAMPLES) g = FRAME_SAMPLES;
  int maxWrite = FRAME_SAMPLES - g;
  if (maxWrite > OUTLEN) maxWrite = OUTLEN;
  for (int n = 0; n < maxWrite; ++n) {
    long Ii = lround(yi[n]*sc);
    long Qq = lround(yq[n]*sc);
    if (Ii < -32767) Ii=-32767; if (Ii > 32767) Ii=32767;
    if (Qq < -32767) Qq=-32767; if (Qq > 32767) Qq=32767;
    outI[g + n] = (int16_t)Ii;
    outQ[g + n] = (int16_t)Qq;
  }
}

// Build QPSK frame - payload here is the REDUCED 54-byte payload
static void build_qpsk_into(const uint8_t* payload, int16_t* destI, int16_t* destQ) {
  static double sI[TOTAL_SYMS];
  static double sQ[TOTAL_SYMS];
  int pos = 0;
  for (int k = 0; k < 8; ++k) {
    sI[pos]=+1.0; sQ[pos]=+1.0; pos++;
    sI[pos]=-1.0; sQ[pos]=-1.0; pos++;
  }

  const double ISQRT2 = 0.7071067811865476;
  int paySymIdx = 0;
  for (int i = 0; i < PAYLOAD_BYTES; ++i) {
    uint8_t b = payload[i];    
    for (int k = 7; k >= 0; k -= 2) {
      uint8_t b0 = (b >> k) & 1u;
      uint8_t b1 = (b >> (k-1)) & 1u;
      uint8_t idx = (uint8_t)(b0 * 2u + b1);
      if (pos >= PRE_SYMS && paySymIdx < (int)((PAYLOAD_BYTES*8)/2)) {
        g_payload_Ibits[paySymIdx] = b0;
        g_payload_Qbits[paySymIdx] = b1;
        ++paySymIdx;
      }

      double si = (idx == 0 || idx == 2) ? +ISQRT2 : -ISQRT2;
      double sq = (idx == 0 || idx == 1) ? +ISQRT2 : -ISQRT2;
      sI[pos] = si; sQ[pos] = sq; pos++;
    }
  }

  static double upI[UPLEN], upQ[UPLEN];
  for (int n=0; n<UPLEN; ++n) { upI[n]=0.0; upQ[n]=0.0; }
  for (int k=0; k<TOTAL_SYMS; ++k) { upI[k*SPS]=sI[k]; upQ[k*SPS]=sQ[k]; }

  static double yi[OUTLEN], yq[OUTLEN];
  for (int n=0; n<OUTLEN; ++n) {
    double accI=0.0, accQ=0.0;
    for (int k=0; k<TAPS; ++k) {
      int xi = n - k;
      if (xi >= 0 && xi < UPLEN) { accI += rrc[k]*upI[xi]; accQ += rrc[k]*upQ[xi]; }
    }
    yi[n]=accI; yq[n]=accQ;
  }

  finalize_into_buffers(yi, yq, destI, destQ);
}

// ---------- MCP4922 helpers ----------
static inline uint16_t i16_to_mcp12(int16_t s) {
  uint16_t u = (uint16_t)(s + 32768);
  uint32_t v = ((uint32_t)u * 4095u + 32767u) / 65535u;
  if (v > 4095u) v = 4095u;
  return (uint16_t)v;
}
static inline uint16_t mcp4922_word(bool chanB, uint16_t code12) {
  return (uint16_t)((chanB ? 1u : 0u) << 15) |
         (0u << 14) | (1u << 13) | (1u << 12) | (code12 & 0x0FFFu);
}

static inline void mcp4922_write_pair(uint16_t codeA, uint16_t codeB) {
  SPI.beginTransaction(dacSPI);
  digitalWriteFast(DAC_CS_PIN, LOW);
  SPI.transfer16(mcp4922_word(false, codeA));
  digitalWriteFast(DAC_CS_PIN, HIGH);
  digitalWriteFast(DAC_CS_PIN, LOW);
  SPI.transfer16(mcp4922_word(true,  codeB));
  digitalWriteFast(DAC_CS_PIN, HIGH);
  SPI.endTransaction();
}

// ---------- Steady 48 kHz DAC ISR ----------
void dac_isr() {
  int16_t si, sq;
  if (!rb_pop(si, sq)) { si = lastI; sq = lastQ; }
  lastI = si; lastQ = sq;
  uint16_t codeA = i16_to_mcp12(si);
  uint16_t codeB = i16_to_mcp12(sq);
  mcp4922_write_pair(codeA, codeB);
}

// ---------- Audio player ----------
class AudioPlayIQ : public AudioStream {
public:
  AudioPlayIQ() : AudioStream(0, NULL) {}
  void begin(const int16_t* iBuf, const int16_t* qBuf, int totalLen) {
    AudioNoInterrupts();
    i_ptr = iBuf; q_ptr = qBuf; len = totalLen; idx = 0;
    AudioInterrupts();
  }

virtual void update(void) {
  audio_block_t *bi = allocate();
  audio_block_t *bq = allocate();
  if (!bi || !bq) { if (bi) release(bi); if (bq) release(bq); return; }

  int local_idx = idx;
  int dI, dQ;
  __disable_irq(); dI = DELAY_I_SAMP; dQ = DELAY_Q_SAMP; __enable_irq();

  if (trig_on && (int32_t)(micros() - trig_due_us) >= 0) {
    digitalWriteFast(TRIG_PIN, LOW);
    trig_on = false;
  }

  for (int n = 0; n < AUDIO_BLOCK_SAMPLES; ++n) {
    if (swapPending && local_idx == 0) {
      i_ptr = (const int16_t*)nextI;
      q_ptr = (const int16_t*)nextQ;
      swapPending = false;
      g_tx_active = true; 
    }

    if (g_tx_active) {
      if (local_idx == 0 && !trig_on) {
        digitalWriteFast(TRIG_PIN, HIGH);
        trig_on = true;
        trig_due_us = micros() + 1000;
      }

      int i_idx = local_idx + dI;
      int q_idx = local_idx + dQ;
      int16_t si = (i_idx >= 0 && i_idx < len) ? i_ptr[i_idx] : 0;
      int16_t sq = (q_idx >= 0 && q_idx < len) ? q_ptr[q_idx] : 0;

      bi->data[n] = si;
      bq->data[n] = sq;
      rb_push(si, sq);

      if (++local_idx >= len) {
        local_idx   = 0;
        g_tx_active = false;
      }
    } else {
      bi->data[n] = 0;
      bq->data[n] = 0;
      rb_push(0, 0);
    }

    if (trig_on && (int32_t)(micros() - trig_due_us) >= 0) {
      digitalWriteFast(TRIG_PIN, LOW);
      trig_on = false;
    }
  }

  transmit(bi, 0); transmit(bq, 1);
  release(bi); release(bq);
  idx = local_idx;
}

private:
  const int16_t* i_ptr = nullptr;
  const int16_t* q_ptr = nullptr;
  int len = 0;
  volatile int idx = 0;
};

// ---------- Audio graph ----------
AudioPlayIQ       iqSource;
AudioOutputSPDIF3 spdifOut;
AudioConnection   patchCord1(iqSource, 0, spdifOut, 0);
AudioConnection   patchCord2(iqSource, 1, spdifOut, 1);

// ---------- Unified Decoder Logic ----------
// Receive FULL 66-byte messages, then strip constant bytes
static enum { SEEK_SOF, COLLECT } g_decState = SEEK_SOF;
static uint8_t g_rxBuf[FULL_PAYLOAD_BYTES];  // Buffer for FULL message
static int     g_rxCount = 0;
static int     g_sofIndex = 0;

// Extract reduced payload from full message
// Takes 66-byte full message, outputs 54-byte reduced payload
static void extract_reduced_payload(const uint8_t* fullMsg, uint8_t* reducedPayload) {
  // Copy variable data bytes 8-59 (52 bytes)
  memcpy(reducedPayload, &fullMsg[VAR_DATA_OFFSET], VAR_DATA_LEN);
  // Copy CRC bytes 64-65 (2 bytes)
  memcpy(&reducedPayload[VAR_DATA_LEN], &fullMsg[CRC_OFFSET], CRC_LEN);
}

// Process incoming byte - looking for FULL 66-byte messages
static void process_byte(uint8_t c) {
  // if (c<16) Serial.print("0");
  // Serial.print(c,HEX);
  // Serial.print(" ");
  // if (pindex++ >= 65) {
  //   pindex = 0;
  //   Serial.println("");
  // }
 
  if (g_decState == SEEK_SOF) {
    if (c == SOF_BYTES[g_sofIndex]) {
      g_rxBuf[g_sofIndex] = (uint8_t)c;
      g_sofIndex++;
      g_rxCount = g_sofIndex;
      if (g_sofIndex >= SOF_LEN) {
        g_decState = COLLECT;
      }
    } else {
      if (c == SOF_BYTES[0]) {
        g_rxBuf[0] = (uint8_t)c;
        g_sofIndex = 1;
        g_rxCount  = 1;
      } else {
        g_sofIndex = 0;
        g_rxCount  = 0;
      }
    }
  } else { // COLLECT
    g_rxBuf[g_rxCount++] = (uint8_t)c;
    if (g_rxCount >= FULL_PAYLOAD_BYTES) {
      // Full message received - extract reduced payload
      uint8_t reducedPayload[REDUCED_PAYLOAD_BYTES];
      extract_reduced_payload(g_rxBuf, reducedPayload);
      
      __disable_irq();
      memcpy((void*)g_extPayload, reducedPayload, REDUCED_PAYLOAD_BYTES);
      g_have_extPayload = true;
      __enable_irq();

      if (ledCount++ > 1) {
        digitalWriteFast(STATUS_LED_PIN, HIGH);
        led_turn_off_at = millis() + 10;
        ledCount = 0;
      }

      g_decState = SEEK_SOF;
      g_sofIndex = 0;
      g_rxCount  = 0;
    }
  }
}

// ---------- Unified Input Handler ----------
static void ingest_all_inputs() {
  if (!g_usb_mode) return;

  while (Serial1.available()) {
    process_byte(Serial1.read());
  }

  while (Serial.available()) {
    int c = Serial.peek();
    if (g_decState == COLLECT || g_sofIndex > 0 || c == SOF_BYTES[0]) {
      process_byte(Serial.read()); 
    } else {
      break; 
    }
  }
}

// ---------- Serial control ----------
static void handleSerialCommands() {
  static char buf[48]; static int p = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      buf[p] = 0;
      p = 0; if (!buf[0]) continue;
      if (strncmp(buf, "dI=", 3) == 0) {
        int v = atoi(buf + 3);
        __disable_irq(); DELAY_I_SAMP = v; __enable_irq();
        Serial.print("Set DELAY_I_SAMP = "); Serial.println(v);
      } else if (strncmp(buf, "dQ=", 3) == 0) {
        int v = atoi(buf + 3);
        __disable_irq(); DELAY_Q_SAMP = v; __enable_irq();
        Serial.print("Set DELAY_Q_SAMP = "); Serial.println(v);
      } else if (strcmp(buf, "show") == 0) {
        __disable_irq();
        int di = DELAY_I_SAMP, dq = DELAY_Q_SAMP; __enable_irq();
        Serial.print("Delays: I="); Serial.print(di); Serial.print("  Q="); Serial.println(dq);
        Serial.print("Full payload: "); Serial.print(FULL_PAYLOAD_BYTES);
        Serial.print(" -> Reduced: "); Serial.println(REDUCED_PAYLOAD_BYTES);
      } else if (strcmp(buf, "usb=on") == 0) {
        g_usb_mode = true;
        Serial.println("USB ingest: ON");
      } else if (strcmp(buf, "usb=off") == 0) {
        g_usb_mode = false;
        Serial.println("USB ingest: OFF");
      } else {
        Serial.println("Commands: dI=±N  |  dQ=±N  |  show  |  usb=on  |  usb=off");
      }
    } else if (p < (int)sizeof(buf) - 1) {
      buf[p++] = c;
    }
  }
}

// ---------- Default payload (reduced format) ----------
static void make_default_payload(uint8_t* outMsg) {
  // Fill with test pattern
  for (int i = 0; i < REDUCED_PAYLOAD_BYTES; ++i) {
    outMsg[i] = 0x41;
  }
}

static uint16_t checksum16_crc(const uint8_t* data, int len) {
    uint16_t crc = 0;
    const uint16_t poly = 0x1021;

    for (int i = 0; i < len; ++i) {
        crc ^= (uint16_t)data[i] << 8;

        for (uint8_t bit = 0; bit < 8; bit++) {
            if (crc & 0x8000)
                crc = (crc << 1) ^ poly;
            else
                crc <<= 1;

            crc &= 0xFFFF;
        }
    }

    return crc;
}


// ---------- Frame builder / queue ----------
static void rebuild_and_queue() {
  int16_t* dstI = useA ? frameI_A : frameI_B;
  int16_t* dstQ = useA ? frameQ_A : frameQ_B;

  uint8_t payload[REDUCED_PAYLOAD_BYTES];
  if (g_usb_mode && g_have_extPayload) {
    __disable_irq();
    memcpy(payload, (const void*)g_extPayload, REDUCED_PAYLOAD_BYTES);
    g_have_extPayload = false; 
    __enable_irq();
  } else {
    make_default_payload(payload);
  }

  // Reconstruct full message temporarily to recalculate CRC with constant FIZ bytes
  uint8_t fullMsg[66];
  // Header (0-7)
  fullMsg[0] = 0x24;
  fullMsg[1] = 0xD9;
  fullMsg[2] = 0x42;
  fullMsg[3] = 0x00;
  fullMsg[4] = 0x46;
  fullMsg[5] = 0x24;
  fullMsg[6] = 0x00;
  fullMsg[7] = 0x00;
  // Variable data (8-59) from reduced payload (indices 0-51)
  memcpy(&fullMsg[8], payload, 52);
  // Constant FIZ status (60-63)
  fullMsg[60] = 0x06;
  fullMsg[61] = 0x01;
  fullMsg[62] = 0x02;
  fullMsg[63] = 0x00;
  // Recalculate CRC
  uint16_t crc = checksum16_crc(fullMsg, 64);
  payload[52] = (uint8_t)(crc >> 8);    // MSB
  payload[53] = (uint8_t)(crc & 0xFF);  // LSB

  // Scramble the reduced payload
  scramble_payload_inplace(payload, REDUCED_PAYLOAD_BYTES);

  build_qpsk_into(payload, dstI, dstQ);

  AudioNoInterrupts();
  nextI = dstI; nextQ = dstQ;
  swapPending = true;
  AudioInterrupts();
  useA = !useA;
}
static void print_first_byte_symbols(uint8_t b) {
  const float ISQRT2 = 0.7071067811865476f;
  Serial.print("First payload byte = 0x"); Serial.println(b, HEX);
  for (int k = 7, s = 0; k >= 0; k -= 2, ++s) {
    uint8_t ibit = (b >> k) & 1u;
    uint8_t qbit = (b >> (k-1)) & 1u;
    uint8_t idx  = (uint8_t)(ibit*2u + qbit);
    float si = (idx == 0 || idx == 2) ? +ISQRT2 : -ISQRT2;
    float sq = (idx == 0 || idx == 1) ? +ISQRT2 : -ISQRT2;
    Serial.print("sym"); Serial.print(s);
    Serial.print(": I="); Serial.print(si, 6);
    Serial.print(", Q="); Serial.print(sq, 6);
    Serial.print("  (bits I,Q="); Serial.print(ibit); Serial.print(','); Serial.print(qbit); Serial.println(')');
  }
}

void setup() {
  if (OUTLEN > FRAME_SAMPLES) {
    Serial.print(OUTLEN);
    Serial.println(" - WARNING: OUTLEN exceeds FRAME_SAMPLES.");
  }
  Serial.print("Frame samples: ");
  Serial.println(FRAME_SAMPLES);
  Serial.print("OUTLEN: ");
  Serial.println(OUTLEN);

  Serial.begin(115200);
  Serial1.begin(115200); 
  randomSeed(analogRead(A0));
  while (!Serial && millis() < 3000) {}

  Serial.println("AirPixel XT Audio transmitter (REDUCED PAYLOAD)");
  Serial.print("Full payload: "); Serial.print(FULL_PAYLOAD_BYTES);
  Serial.print(" bytes -> Reduced: "); Serial.print(REDUCED_PAYLOAD_BYTES);
  Serial.println(" bytes");

  pinMode(TRIG_PIN, OUTPUT);
  digitalWriteFast(TRIG_PIN, LOW);

  pinMode(DAC_CS_PIN, OUTPUT);
  digitalWriteFast(DAC_CS_PIN, HIGH);
  
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWriteFast(STATUS_LED_PIN, LOW);

  SPI.begin();

  AudioMemory(64);
  make_rrc(ALPHA, SPS, SPAN, rrc);

  uint8_t p0[REDUCED_PAYLOAD_BYTES];
  make_default_payload(p0);

  uint8_t first_unscrambled = p0[0];
  scramble_payload_inplace(p0, REDUCED_PAYLOAD_BYTES);

  build_qpsk_into(p0, frameI_A, frameQ_A);
  print_first_byte_symbols(first_unscrambled);

  iqSource.begin(frameI_A, frameQ_A, FRAME_SAMPLES);
  nextI = frameI_A; nextQ = frameQ_A; swapPending = false;

  dacTimer.begin(dac_isr, 1000000.0f / FS);
  Serial.println("QPSK TX + MCP4922 mirror ready (REDUCED MODE).");
}

void loop() {
  ingest_all_inputs();
  //handleSerialCommands();

  if (led_turn_off_at > 0 && millis() >= led_turn_off_at) {
    digitalWriteFast(STATUS_LED_PIN, LOW);
    led_turn_off_at = 0; 
  }

  if (g_usb_mode && g_have_extPayload) {
    rebuild_and_queue();
  }
}
