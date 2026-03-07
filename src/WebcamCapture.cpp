#include "WebcamCapture.h"
#include "FilamentApp.h"

#include <filament/Texture.h>
#include <filament/TextureSampler.h>
#include <filament/RenderableManager.h>

#include <iostream>

#ifdef _WIN32
#include <dshow.h>
#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "ole32.lib")

// ISampleGrabber and friends were removed from modern Windows SDK (qedit.h).
// Define the minimal interfaces we need manually.
DEFINE_GUID(CLSID_SampleGrabber, 0xC1F400A0, 0x3F08, 0x11D3,
    0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37);
DEFINE_GUID(CLSID_NullRenderer, 0xC1F400A4, 0x3F08, 0x11D3,
    0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37);
DEFINE_GUID(IID_ISampleGrabber, 0x6B652FFF, 0x11FE, 0x4FCE,
    0x92, 0xAD, 0x02, 0x66, 0xB5, 0xD7, 0xC7, 0x8F);
DEFINE_GUID(IID_ISampleGrabberCB, 0x0579154A, 0x2B53, 0x4994,
    0xB0, 0xD0, 0xE7, 0x73, 0x14, 0x8E, 0xFF, 0x85);

// Minimal ISampleGrabberCB interface
MIDL_INTERFACE("0579154A-2B53-4994-B0D0-E773148EFF85")
ISampleGrabberCB : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE SampleCB(double SampleTime, IMediaSample* pSample) = 0;
    virtual HRESULT STDMETHODCALLTYPE BufferCB(double SampleTime, BYTE* pBuffer, long BufferLen) = 0;
};

// Minimal ISampleGrabber interface
MIDL_INTERFACE("6B652FFF-11FE-4FCE-92AD-0266B5D7C78F")
ISampleGrabber : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL OneShot) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE* pType) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE* pType) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL BufferThem) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long* pBufferSize, long* pBuffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample** ppSample) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCallback(ISampleGrabberCB* pCallback, long WhichMethodToCallback) = 0;
};

// DirectShow SampleGrabber callback — receives decoded BGR frames on streaming thread
class SampleGrabberCallback : public ISampleGrabberCB {
public:
    // Shared state pointers (set by initWebcam)
    std::vector<uint8_t>* frameData = nullptr;
    std::mutex* frameMutex = nullptr;
    std::atomic<bool>* newFrameFlag = nullptr;
    CameraTrackingData* trackingData = nullptr;
    std::mutex* trackingMutex = nullptr;
    CameraTrackingData* webcamTracking = nullptr;
    std::deque<CameraTrackingData>* trackingBuffer = nullptr;
    float* trackingDelayMs = nullptr;
    std::atomic<int64_t>* videoFrameUTC_us = nullptr;
    std::atomic<int>* cbCount = nullptr;
    HANDLE frameEvent = NULL;
    bool bottomUp = true;
    int width = 0;
    int height = 0;

    LONG refCount = 1;
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_ISampleGrabberCB) {
            *ppv = static_cast<ISampleGrabberCB*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refCount); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG r = InterlockedDecrement(&refCount);
        if (r == 0) delete this;
        return r;
    }

    STDMETHODIMP SampleCB(double, IMediaSample*) override { return E_NOTIMPL; }

    STDMETHODIMP BufferCB(double sampleTime, BYTE* pBuffer, long bufferLen) override {
        if (!frameData || !frameMutex || !newFrameFlag) return S_OK;
        size_t expected = (size_t)width * height * 3;
        if ((size_t)bufferLen < expected) return S_OK;

        // Snapshot tracking data, applying delay to compensate for video latency
        CameraTrackingData trackSnapshot;
        if (trackingMutex && trackingData) {
            std::lock_guard<std::mutex> lock(*trackingMutex);
            float delayMs = trackingDelayMs ? *trackingDelayMs : 0.0f;
            if (delayMs > 0.0f && trackingBuffer && !trackingBuffer->empty()) {
                auto nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                int64_t targetUs = nowUs - (int64_t)(delayMs * 1000.0f);
                trackSnapshot = trackingBuffer->front();
                for (auto& s : *trackingBuffer) {
                    if (s.localTimeUs <= targetUs) {
                        trackSnapshot = s;
                    } else {
                        break;
                    }
                }
            } else {
                trackSnapshot = *trackingData;
            }
        }

        {
            std::lock_guard<std::mutex> lock(*frameMutex);
            if (bottomUp) {
                size_t rowBytes = (size_t)width * 3;
                for (int y = 0; y < height; y++) {
                    memcpy(frameData->data() + y * rowBytes,
                           pBuffer + (height - 1 - y) * rowBytes, rowBytes);
                }
            } else {
                memcpy(frameData->data(), pBuffer, expected);
            }
            if (webcamTracking) *webcamTracking = trackSnapshot;
            if (videoFrameUTC_us) {
                auto utcNow = std::chrono::system_clock::now();
                auto us = std::chrono::duration_cast<std::chrono::microseconds>(utcNow.time_since_epoch()).count();
                videoFrameUTC_us->store(us, std::memory_order_relaxed);
            }
            *newFrameFlag = true;
        }
        if (cbCount) (*cbCount)++;
        if (frameEvent) SetEvent(frameEvent);
        return S_OK;
    }
};

#endif // _WIN32

void webcam_shutdown(FilamentApp* app) {
#ifdef _WIN32
    app->mWebcamEnabled = false;
    if (app->mDSControl) {
        app->mDSControl->Stop();
        app->mDSControl->Release();
        app->mDSControl = nullptr;
    }
    if (app->mDSSampleGrabber) { app->mDSSampleGrabber->Release(); app->mDSSampleGrabber = nullptr; }
    if (app->mDSNullFilter && app->mDSGraph) { app->mDSGraph->RemoveFilter(app->mDSNullFilter); app->mDSNullFilter->Release(); app->mDSNullFilter = nullptr; }
    if (app->mDSGrabberFilter && app->mDSGraph) { app->mDSGraph->RemoveFilter(app->mDSGrabberFilter); app->mDSGrabberFilter->Release(); app->mDSGrabberFilter = nullptr; }
    if (app->mDSSourceFilter && app->mDSGraph) { app->mDSGraph->RemoveFilter(app->mDSSourceFilter); app->mDSSourceFilter->Release(); app->mDSSourceFilter = nullptr; }
    if (app->mDSCapture) { app->mDSCapture->Release(); app->mDSCapture = nullptr; }
    if (app->mDSGraph) { app->mDSGraph->Release(); app->mDSGraph = nullptr; }
    if (app->mDSCallback) { app->mDSCallback->Release(); app->mDSCallback = nullptr; }
    if (app->mWebcamFrameEvent) { CloseHandle(app->mWebcamFrameEvent); app->mWebcamFrameEvent = NULL; }
    app->mConnectedVideoDeviceName.clear();
#endif
}

void webcam_init(FilamentApp* app, int deviceIndex) {
#ifdef _WIN32
    std::cout << "Initializing webcam (DirectShow) device " << deviceIndex << "..." << std::endl;

    if (!app->mCOMInitialized) {
        HRESULT hrCom = CoInitializeEx(NULL, COINIT_MULTITHREADED);
        if (FAILED(hrCom) && hrCom != RPC_E_CHANGED_MODE) {
            std::cerr << "COM init failed: 0x" << std::hex << hrCom << std::dec << std::endl;
            return;
        }
        app->mCOMInitialized = true;
    }
    HRESULT hr;

    // Create filter graph
    hr = CoCreateInstance(CLSID_FilterGraph, NULL, CLSCTX_INPROC_SERVER,
                          IID_IGraphBuilder, (void**)&app->mDSGraph);
    if (FAILED(hr)) { std::cerr << "Failed to create FilterGraph" << std::endl; return; }

    hr = CoCreateInstance(CLSID_CaptureGraphBuilder2, NULL, CLSCTX_INPROC_SERVER,
                          IID_ICaptureGraphBuilder2, (void**)&app->mDSCapture);
    if (FAILED(hr)) { std::cerr << "Failed to create CaptureGraphBuilder2" << std::endl; return; }
    app->mDSCapture->SetFiltergraph(app->mDSGraph);

    // Enumerate video capture devices
    ICreateDevEnum* devEnum = nullptr;
    hr = CoCreateInstance(CLSID_SystemDeviceEnum, NULL, CLSCTX_INPROC_SERVER,
                          IID_ICreateDevEnum, (void**)&devEnum);
    if (FAILED(hr)) { std::cerr << "Failed to create device enumerator" << std::endl; return; }

    IEnumMoniker* enumMoniker = nullptr;
    hr = devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &enumMoniker, 0);
    devEnum->Release();
    if (hr != S_OK || !enumMoniker) {
        std::cerr << "No video capture devices found" << std::endl;
        return;
    }

    // Select device by index
    IMoniker* moniker = nullptr;
    int devIdx = 0;
    while (enumMoniker->Next(1, &moniker, NULL) == S_OK) {
        IPropertyBag* propBag = nullptr;
        if (SUCCEEDED(moniker->BindToStorage(0, 0, IID_IPropertyBag, (void**)&propBag))) {
            VARIANT varName;
            VariantInit(&varName);
            if (SUCCEEDED(propBag->Read(L"FriendlyName", &varName, 0))) {
                std::wcout << L"  Device " << devIdx << L": " << varName.bstrVal << std::endl;
            }
            VariantClear(&varName);
            propBag->Release();
        }
        if (devIdx == deviceIndex) break;
        moniker->Release();
        moniker = nullptr;
        devIdx++;
    }
    enumMoniker->Release();

    if (!moniker) {
        std::cerr << "Camera index " << deviceIndex << " not found" << std::endl;
        return;
    }

    // Bind moniker to source filter
    hr = moniker->BindToObject(0, 0, IID_IBaseFilter, (void**)&app->mDSSourceFilter);
    moniker->Release();
    if (FAILED(hr)) { std::cerr << "Failed to bind camera device" << std::endl; return; }

    app->mDSGraph->AddFilter(app->mDSSourceFilter, L"VideoCapture");

    // Create SampleGrabber filter
    hr = CoCreateInstance(CLSID_SampleGrabber, NULL, CLSCTX_INPROC_SERVER,
                          IID_IBaseFilter, (void**)&app->mDSGrabberFilter);
    if (FAILED(hr)) { std::cerr << "Failed to create SampleGrabber" << std::endl; return; }
    app->mDSGraph->AddFilter(app->mDSGrabberFilter, L"SampleGrabber");

    // Get ISampleGrabber interface
    hr = app->mDSGrabberFilter->QueryInterface(IID_ISampleGrabber, (void**)&app->mDSSampleGrabber);
    if (FAILED(hr)) { std::cerr << "Failed to get ISampleGrabber" << std::endl; return; }

    // Configure grabber to accept RGB24
    AM_MEDIA_TYPE mt = {};
    mt.majortype = MEDIATYPE_Video;
    mt.subtype = MEDIASUBTYPE_RGB24;
    app->mDSSampleGrabber->SetMediaType(&mt);

    // Create NullRenderer (sink)
    hr = CoCreateInstance(CLSID_NullRenderer, NULL, CLSCTX_INPROC_SERVER,
                          IID_IBaseFilter, (void**)&app->mDSNullFilter);
    if (FAILED(hr)) { std::cerr << "Failed to create NullRenderer" << std::endl; return; }
    app->mDSGraph->AddFilter(app->mDSNullFilter, L"NullRenderer");

    // Try to configure capture pin for 1920x1080 MJPG
    IAMStreamConfig* streamConfig = nullptr;
    hr = app->mDSCapture->FindInterface(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video,
                                    app->mDSSourceFilter, IID_IAMStreamConfig, (void**)&streamConfig);
    if (SUCCEEDED(hr) && streamConfig) {
        int count = 0, size = 0;
        streamConfig->GetNumberOfCapabilities(&count, &size);
        std::vector<BYTE> capsBuf(size);

        std::cout << "  Available capture modes:" << std::endl;
        for (int i = 0; i < count; i++) {
            AM_MEDIA_TYPE* pmt = nullptr;
            if (SUCCEEDED(streamConfig->GetStreamCaps(i, &pmt, capsBuf.data()))) {
                if (pmt->formattype == FORMAT_VideoInfo && pmt->cbFormat >= sizeof(VIDEOINFOHEADER)) {
                    VIDEOINFOHEADER* vih = (VIDEOINFOHEADER*)pmt->pbFormat;
                    BITMAPINFOHEADER& bmi = vih->bmiHeader;
                    double fps = (vih->AvgTimePerFrame > 0) ? 10000000.0 / vih->AvgTimePerFrame : 0;
                    char fourcc[5] = {};
                    memcpy(fourcc, &bmi.biCompression, 4);
                    double maxFps = 0, minFps = 0;
                    if (size >= (int)sizeof(VIDEO_STREAM_CONFIG_CAPS)) {
                        VIDEO_STREAM_CONFIG_CAPS* caps = (VIDEO_STREAM_CONFIG_CAPS*)capsBuf.data();
                        if (caps->MinFrameInterval > 0)
                            maxFps = 10000000.0 / caps->MinFrameInterval;
                        if (caps->MaxFrameInterval > 0)
                            minFps = 10000000.0 / caps->MaxFrameInterval;
                    }
                    std::cout << "    [" << i << "] " << bmi.biWidth << "x" << abs(bmi.biHeight)
                              << " " << fourcc << " " << fps << "fps"
                              << " (range: " << minFps << "-" << maxFps << "fps)" << std::endl;
                }
                if (pmt->cbFormat) CoTaskMemFree(pmt->pbFormat);
                if (pmt) CoTaskMemFree(pmt);
            }
        }

        bool configured = false;
        // First try MJPG 1920x1080 60fps
        for (int i = 0; i < count && !configured; i++) {
            AM_MEDIA_TYPE* pmt = nullptr;
            if (SUCCEEDED(streamConfig->GetStreamCaps(i, &pmt, capsBuf.data()))) {
                if (pmt->formattype == FORMAT_VideoInfo && pmt->cbFormat >= sizeof(VIDEOINFOHEADER)) {
                    VIDEOINFOHEADER* vih = (VIDEOINFOHEADER*)pmt->pbFormat;
                    BITMAPINFOHEADER& bmi = vih->bmiHeader;
                    if (bmi.biWidth == 1920 && abs(bmi.biHeight) == 1080 &&
                        pmt->subtype == MEDIASUBTYPE_MJPG) {
                        vih->AvgTimePerFrame = 166667;
                        std::cout << "  Setting capture: 1920x1080 MJPG 60fps" << std::endl;
                        streamConfig->SetFormat(pmt);
                        configured = true;
                    }
                }
                if (pmt->cbFormat) CoTaskMemFree(pmt->pbFormat);
                if (pmt) CoTaskMemFree(pmt);
            }
        }
        // Fallback: any 1920x1080
        for (int i = 0; i < count && !configured; i++) {
            AM_MEDIA_TYPE* pmt = nullptr;
            if (SUCCEEDED(streamConfig->GetStreamCaps(i, &pmt, capsBuf.data()))) {
                if (pmt->formattype == FORMAT_VideoInfo && pmt->cbFormat >= sizeof(VIDEOINFOHEADER)) {
                    VIDEOINFOHEADER* vih = (VIDEOINFOHEADER*)pmt->pbFormat;
                    BITMAPINFOHEADER& bmi = vih->bmiHeader;
                    if (bmi.biWidth == 1920 && abs(bmi.biHeight) == 1080) {
                        std::cout << "  Setting capture: 1920x1080 (any codec)" << std::endl;
                        streamConfig->SetFormat(pmt);
                        configured = true;
                    }
                }
                if (pmt->cbFormat) CoTaskMemFree(pmt->pbFormat);
                if (pmt) CoTaskMemFree(pmt);
            }
        }
        streamConfig->Release();
    }

    // Query and set camera exposure
    IAMCameraControl* camCtrl = nullptr;
    hr = app->mDSSourceFilter->QueryInterface(IID_IAMCameraControl, (void**)&camCtrl);
    if (SUCCEEDED(hr) && camCtrl) {
        long minExp, maxExp, step, defExp, flags;
        hr = camCtrl->GetRange(CameraControl_Exposure, &minExp, &maxExp, &step, &defExp, &flags);
        if (SUCCEEDED(hr)) {
            app->mCamExposureMin = minExp;
            app->mCamExposureMax = maxExp;
            app->mCamExposureStep = step;
            app->mCamExposureAvailable = true;
            std::cout << "  Exposure range: " << minExp << " to " << maxExp
                      << " step=" << step << " default=" << defExp
                      << " flags=" << flags << std::endl;
        }
        long curExp, curFlags;
        hr = camCtrl->Get(CameraControl_Exposure, &curExp, &curFlags);
        if (SUCCEEDED(hr)) {
            app->mCamExposure = curExp;
            app->mCamExposureAuto = (curFlags & CameraControl_Flags_Auto) != 0;
            std::cout << "  Current exposure: " << curExp
                      << (curFlags & CameraControl_Flags_Auto ? " (auto)" : " (manual)") << std::endl;
        }
        // Apply auto or manual exposure based on default setting
        if (app->mCamExposureAuto) {
            hr = camCtrl->Set(CameraControl_Exposure, app->mCamExposure, CameraControl_Flags_Auto);
            if (SUCCEEDED(hr)) {
                std::cout << "  Set exposure to auto" << std::endl;
            }
        } else {
            hr = camCtrl->Set(CameraControl_Exposure, app->mCamExposure, CameraControl_Flags_Manual);
            if (SUCCEEDED(hr)) {
                std::cout << "  Set exposure to " << app->mCamExposure << " (manual) for high fps" << std::endl;
            } else {
                std::cerr << "  Failed to set exposure: 0x" << std::hex << hr << std::dec << std::endl;
            }
        }
        camCtrl->Release();
    }

    // Build the graph: Source -> SampleGrabber -> NullRenderer
    hr = app->mDSCapture->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video,
                                   app->mDSSourceFilter, app->mDSGrabberFilter, app->mDSNullFilter);
    if (FAILED(hr)) {
        std::cerr << "Failed to render capture stream: 0x" << std::hex << hr << std::dec << std::endl;
        return;
    }

    // Check actual connected format
    AM_MEDIA_TYPE connectedMT = {};
    hr = app->mDSSampleGrabber->GetConnectedMediaType(&connectedMT);
    if (SUCCEEDED(hr) && connectedMT.formattype == FORMAT_VideoInfo &&
        connectedMT.cbFormat >= sizeof(VIDEOINFOHEADER)) {
        VIDEOINFOHEADER* vih = (VIDEOINFOHEADER*)connectedMT.pbFormat;
        app->mWebcamWidth = vih->bmiHeader.biWidth;
        app->mWebcamHeight = abs(vih->bmiHeader.biHeight);
        bool bottomUp = (vih->bmiHeader.biHeight > 0);
        double connectedFps = (vih->AvgTimePerFrame > 0) ? 10000000.0 / vih->AvgTimePerFrame : 0;
        app->mWebcamFps = (connectedFps > 0) ? (int)(connectedFps + 0.5) : 30;
        std::cout << "  Connected format: " << app->mWebcamWidth << "x" << app->mWebcamHeight
                  << (bottomUp ? " bottom-up" : " top-down")
                  << " " << connectedFps << "fps" << std::endl;

        app->mDSCallback = new SampleGrabberCallback();
        app->mDSCallback->bottomUp = bottomUp;
    } else {
        app->mDSCallback = new SampleGrabberCallback();
    }
    if (connectedMT.cbFormat) CoTaskMemFree(connectedMT.pbFormat);

    std::cout << "Webcam resolution: " << app->mWebcamWidth << "x" << app->mWebcamHeight << std::endl;

    // Initialize shared frame buffer
    size_t bufSize = app->mWebcamWidth * app->mWebcamHeight * 3;
    app->mWebcamFrameData.resize(bufSize);

    // Create auto-reset event
    app->mWebcamFrameEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    // Configure callback pointers
    app->mDSCallback->frameData = &app->mWebcamFrameData;
    app->mDSCallback->frameMutex = &app->mWebcamMutex;
    app->mDSCallback->newFrameFlag = &app->mWebcamNewFrame;
    app->mDSCallback->trackingData = &app->mTrackingData;
    app->mDSCallback->trackingMutex = &app->mTrackingMutex;
    app->mDSCallback->webcamTracking = &app->mWebcamTracking;
    app->mDSCallback->trackingBuffer = &app->mTrackingBuffer;
    app->mDSCallback->trackingDelayMs = &app->mTrackingDelayMs;
    app->mDSCallback->videoFrameUTC_us = &app->mLastVideoFrameUTC_us;
    app->mDSCallback->cbCount = &app->mWebcamCBCount;
    app->mDSCallback->frameEvent = app->mWebcamFrameEvent;
    app->mDSCallback->width = app->mWebcamWidth;
    app->mDSCallback->height = app->mWebcamHeight;

    // Set callback (method 1 = BufferCB)
    app->mDSSampleGrabber->SetBufferSamples(FALSE);
    app->mDSSampleGrabber->SetOneShot(FALSE);
    app->mDSSampleGrabber->SetCallback(app->mDSCallback, 1);

    // Create or recreate webcam texture
    if (app->mWebcamTexture) {
        app->mEngine->destroy(app->mWebcamTexture);
        app->mWebcamTexture = nullptr;
    }
    app->mWebcamTexture = Texture::Builder()
        .width(app->mWebcamWidth)
        .height(app->mWebcamHeight)
        .levels(1)
        .format(Texture::InternalFormat::RGB8)
        .sampler(Texture::Sampler::SAMPLER_2D)
        .build(*app->mEngine);

    // Create background quad (only on first init)
    if (!app->mBackgroundMI) {
        webcam_createBackgroundQuad(app);
    }
    // Rebind texture to material
    if (app->mBackgroundMI) {
        TextureSampler sampler;
        sampler.setMagFilter(TextureSampler::MagFilter::LINEAR);
        sampler.setMinFilter(TextureSampler::MinFilter::LINEAR);
        app->mBackgroundMI->setParameter("videoTexture", app->mWebcamTexture, sampler);
    }

    // Increase SampleGrabber allocator buffers
    {
        IPin* grabberIn = nullptr;
        IEnumPins* enumPins = nullptr;
        app->mDSGrabberFilter->EnumPins(&enumPins);
        if (enumPins) {
            while (enumPins->Next(1, &grabberIn, NULL) == S_OK) {
                PIN_DIRECTION dir;
                grabberIn->QueryDirection(&dir);
                if (dir == PINDIR_INPUT) break;
                grabberIn->Release();
                grabberIn = nullptr;
            }
            enumPins->Release();
        }
        if (grabberIn) {
            IMemInputPin* memPin = nullptr;
            grabberIn->QueryInterface(IID_IMemInputPin, (void**)&memPin);
            if (memPin) {
                IMemAllocator* alloc = nullptr;
                memPin->GetAllocator(&alloc);
                if (alloc) {
                    ALLOCATOR_PROPERTIES props = {}, actual = {};
                    alloc->GetProperties(&props);
                    std::cout << "  Allocator buffers before: " << props.cBuffers << std::endl;
                    if (props.cBuffers < 4) {
                        props.cBuffers = 4;
                        HRESULT hrAlloc = alloc->Decommit();
                        std::cout << "  Decommit: 0x" << std::hex << hrAlloc << std::dec << std::endl;
                        hrAlloc = alloc->SetProperties(&props, &actual);
                        std::cout << "  SetProperties: 0x" << std::hex << hrAlloc << std::dec
                                  << " -> " << actual.cBuffers << " buffers" << std::endl;
                        hrAlloc = alloc->Commit();
                        std::cout << "  Commit: 0x" << std::hex << hrAlloc << std::dec << std::endl;
                    } else {
                        std::cout << "  Allocator already >= 4 buffers" << std::endl;
                    }
                    alloc->Release();
                }
                memPin->Release();
            }
            grabberIn->Release();
        }
    }

    // Disable graph reference clock
    IMediaFilter* mediaFilter = nullptr;
    hr = app->mDSGraph->QueryInterface(IID_IMediaFilter, (void**)&mediaFilter);
    if (SUCCEEDED(hr) && mediaFilter) {
        HRESULT hrSync = mediaFilter->SetSyncSource(NULL);
        mediaFilter->Release();
        std::cout << "  SetSyncSource(NULL): 0x" << std::hex << hrSync << std::dec
                  << (SUCCEEDED(hrSync) ? " (OK - freerunning)" : " (FAILED)") << std::endl;
    }

    // Start the graph
    hr = app->mDSGraph->QueryInterface(IID_IMediaControl, (void**)&app->mDSControl);
    if (SUCCEEDED(hr)) {
        hr = app->mDSControl->Run();
        if (FAILED(hr)) {
            std::cerr << "Failed to start capture graph: 0x" << std::hex << hr << std::dec << std::endl;
            return;
        }
    }

    app->mWebcamEnabled = true;

    if (deviceIndex >= 0 && deviceIndex < (int)app->mVideoDevices.size()) {
        app->mConnectedVideoDeviceName = app->mVideoDevices[deviceIndex];
    } else {
        app->mConnectedVideoDeviceName = "Device " + std::to_string(deviceIndex);
    }

    std::cout << "Webcam initialized successfully (DirectShow)" << std::endl;
#endif
}

void webcam_createBackgroundQuad(FilamentApp* app) {
    static TexturedVertex bgVerts[4] = {
        {{-1.0f,  1.0f, 0.0f}, {0.0f, 1.0f}, {0, 0, 0, 32767}},
        {{ 1.0f,  1.0f, 0.0f}, {1.0f, 1.0f}, {0, 0, 0, 32767}},
        {{ 1.0f, -1.0f, 0.0f}, {1.0f, 0.0f}, {0, 0, 0, 32767}},
        {{-1.0f, -1.0f, 0.0f}, {0.0f, 0.0f}, {0, 0, 0, 32767}},
    };

    static uint16_t bgIndices[6] = {0, 3, 2, 0, 2, 1};

    app->mBackgroundVB = VertexBuffer::Builder()
        .vertexCount(4)
        .bufferCount(1)
        .attribute(VertexAttribute::POSITION, 0, VertexBuffer::AttributeType::FLOAT3,
                   offsetof(TexturedVertex, position), sizeof(TexturedVertex))
        .attribute(VertexAttribute::UV0, 0, VertexBuffer::AttributeType::FLOAT2,
                   offsetof(TexturedVertex, uv), sizeof(TexturedVertex))
        .attribute(VertexAttribute::TANGENTS, 0, VertexBuffer::AttributeType::SHORT4,
                   offsetof(TexturedVertex, tangents), sizeof(TexturedVertex))
        .normalized(VertexAttribute::TANGENTS)
        .build(*app->mEngine);

    app->mBackgroundVB->setBufferAt(*app->mEngine, 0,
        VertexBuffer::BufferDescriptor(bgVerts, sizeof(bgVerts)));

    app->mBackgroundIB = IndexBuffer::Builder()
        .indexCount(6)
        .bufferType(IndexBuffer::IndexType::USHORT)
        .build(*app->mEngine);

    app->mBackgroundIB->setBuffer(*app->mEngine,
        IndexBuffer::BufferDescriptor(bgIndices, sizeof(bgIndices)));

    app->mBackgroundMaterial = app->loadMaterial("background.filamat");
    if (app->mBackgroundMaterial) {
        app->mBackgroundMI = app->mBackgroundMaterial->createInstance();
        TextureSampler sampler;
        sampler.setMagFilter(TextureSampler::MagFilter::LINEAR);
        sampler.setMinFilter(TextureSampler::MinFilter::LINEAR);
        app->mBackgroundMI->setParameter("videoTexture", app->mWebcamTexture, sampler);
    } else {
        std::cerr << "ERROR: Failed to load background material" << std::endl;
        return;
    }

    EntityManager& em = EntityManager::get();
    app->mBackgroundEntity = em.create();

    RenderableManager::Builder(1)
        .boundingBox({{-1, -1, -1}, {1, 1, 1}})
        .geometry(0, RenderableManager::PrimitiveType::TRIANGLES,
                  app->mBackgroundVB, app->mBackgroundIB, 0, 6)
        .material(0, app->mBackgroundMI)
        .culling(false)
        .receiveShadows(false)
        .castShadows(false)
        .build(*app->mEngine, app->mBackgroundEntity);

    app->mBackgroundScene = app->mEngine->createScene();
    app->mBackgroundScene->addEntity(app->mBackgroundEntity);

    app->mBackgroundCameraEntity = em.create();
    app->mBackgroundCamera = app->mEngine->createCamera(app->mBackgroundCameraEntity);
    app->mBackgroundCamera->setProjection(Camera::Projection::ORTHO, -1, 1, -1, 1, 0, 10);
    app->mBackgroundCamera->lookAt({0, 0, 1}, {0, 0, 0}, {0, 1, 0});

    app->mBackgroundView = app->mEngine->createView();
    app->mBackgroundView->setScene(app->mBackgroundScene);
    app->mBackgroundView->setCamera(app->mBackgroundCamera);
    app->mBackgroundView->setViewport({0, 0, WINDOW_WIDTH, WINDOW_HEIGHT});
    app->mBackgroundView->setPostProcessingEnabled(false);
    app->mBackgroundView->setShadowingEnabled(false);

    std::cout << "Background view created (separate scene + ortho camera)" << std::endl;
}

void webcam_setupGradientBackground(FilamentApp* app) {
    // Create a gradient texture: dark gray at bottom to lighter gray at top
    const int w = 2;
    const int h = 256;

    // Create texture if needed (or reuse existing if already gradient-sized)
    if (app->mWebcamTexture) {
        app->mEngine->destroy(app->mWebcamTexture);
        app->mWebcamTexture = nullptr;
    }
    app->mWebcamTexture = Texture::Builder()
        .width(w)
        .height(h)
        .levels(1)
        .format(Texture::InternalFormat::RGB8)
        .sampler(Texture::Sampler::SAMPLER_2D)
        .build(*app->mEngine);

    // Create background quad infrastructure if it doesn't exist
    if (!app->mBackgroundMI) {
        webcam_createBackgroundQuad(app);
    }

    // Generate gradient: row 0 = bottom of screen (dark), row h-1 = top (lighter)
    // Background material swaps B/R (it expects BGR webcam data), so we store as BGR
    size_t dataSize = (size_t)w * h * 3;
    uint8_t* pixels = new uint8_t[dataSize];
    for (int y = 0; y < h; y++) {
        // UV0 maps y=0 to top of texture. Row 0 in texture = top of screen.
        // We want top = lighter, bottom = darker.
        // y=0 is top row → lighter, y=h-1 is bottom row → darker
        uint8_t gray = (uint8_t)(40 + (y * 80) / (h - 1));  // range 40 (top) to 120 (bottom)
        // Invert so top is lighter: top (y=0) = 120, bottom (y=h-1) = 40
        gray = (uint8_t)(120 - (y * 80) / (h - 1));
        for (int x = 0; x < w; x++) {
            int idx = (y * w + x) * 3;
            pixels[idx + 0] = gray;  // B (becomes R after material swap)
            pixels[idx + 1] = gray;  // G
            pixels[idx + 2] = gray;  // R (becomes B after material swap)
        }
    }

    Texture::PixelBufferDescriptor pbd(
        pixels, dataSize,
        Texture::Format::RGB, Texture::Type::UBYTE,
        [](void* buffer, size_t, void*) { delete[] static_cast<uint8_t*>(buffer); }
    );
    app->mWebcamTexture->setImage(*app->mEngine, 0, std::move(pbd));

    // Rebind texture to material
    if (app->mBackgroundMI) {
        TextureSampler sampler;
        sampler.setMagFilter(TextureSampler::MagFilter::LINEAR);
        sampler.setMinFilter(TextureSampler::MinFilter::LINEAR);
        app->mBackgroundMI->setParameter("videoTexture", app->mWebcamTexture, sampler);
    }

    app->mUseGradientBg = true;
    std::cout << "Gradient background enabled" << std::endl;
}

bool webcam_updateFrame(FilamentApp* app) {
    if (!app->mWebcamEnabled || !app->mWebcamNewFrame) return false;

    size_t dataSize = app->mWebcamWidth * app->mWebcamHeight * 3;
    uint8_t* uploadData = new uint8_t[dataSize];

    {
        std::lock_guard<std::mutex> lock(app->mWebcamMutex);
        memcpy(uploadData, app->mWebcamFrameData.data(), dataSize);
        app->mWebcamTrackingDisplay = app->mWebcamTracking;
        app->mWebcamNewFrame = false;
    }

    Texture::PixelBufferDescriptor pbd(
        uploadData,
        dataSize,
        Texture::Format::RGB,
        Texture::Type::UBYTE,
        [](void* buffer, size_t, void*) { delete[] static_cast<uint8_t*>(buffer); }
    );

    app->mWebcamTexture->setImage(*app->mEngine, 0, std::move(pbd));
    return true;
}

void webcam_setCameraExposure(FilamentApp* app, long value, bool autoExp) {
#ifdef _WIN32
    if (!app->mDSSourceFilter) return;
    IAMCameraControl* camCtrl = nullptr;
    HRESULT hr = app->mDSSourceFilter->QueryInterface(IID_IAMCameraControl, (void**)&camCtrl);
    if (SUCCEEDED(hr) && camCtrl) {
        long flags = autoExp ? CameraControl_Flags_Auto : CameraControl_Flags_Manual;
        hr = camCtrl->Set(CameraControl_Exposure, value, flags);
        if (SUCCEEDED(hr)) {
            app->mCamExposure = value;
            app->mCamExposureAuto = autoExp;
        }
        camCtrl->Release();
    }
#endif
}

std::vector<std::string> webcam_enumerateDevices() {
    std::vector<std::string> devices;
#ifdef _WIN32
    bool weInitedCOM = false;
    HRESULT hrCom = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(hrCom)) {
        weInitedCOM = true;
    } else if (hrCom != RPC_E_CHANGED_MODE && hrCom != S_FALSE) {
        return devices;
    }

    ICreateDevEnum* devEnum = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum, NULL, CLSCTX_INPROC_SERVER,
                                  IID_ICreateDevEnum, (void**)&devEnum);
    if (SUCCEEDED(hr)) {
        IEnumMoniker* enumMoniker = nullptr;
        hr = devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &enumMoniker, 0);
        if (hr == S_OK && enumMoniker) {
            IMoniker* moniker = nullptr;
            while (enumMoniker->Next(1, &moniker, NULL) == S_OK) {
                IPropertyBag* propBag = nullptr;
                if (SUCCEEDED(moniker->BindToStorage(0, 0, IID_IPropertyBag, (void**)&propBag))) {
                    VARIANT varName;
                    VariantInit(&varName);
                    if (SUCCEEDED(propBag->Read(L"FriendlyName", &varName, 0))) {
                        int len = WideCharToMultiByte(CP_UTF8, 0, varName.bstrVal, -1, NULL, 0, NULL, NULL);
                        std::string name(len - 1, '\0');
                        WideCharToMultiByte(CP_UTF8, 0, varName.bstrVal, -1, &name[0], len, NULL, NULL);
                        devices.push_back(name);
                    }
                    VariantClear(&varName);
                    propBag->Release();
                }
                moniker->Release();
            }
            enumMoniker->Release();
        }
        devEnum->Release();
    }

    if (weInitedCOM) CoUninitialize();
#endif
    return devices;
}
