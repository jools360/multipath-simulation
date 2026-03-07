#pragma once

#include <string>
#include <vector>
#include <cstdint>

struct VboSample {
    int satellites = 0;
    double time = 0;          // hhmmss.sss raw value
    double timeSeconds = 0;   // seconds from midnight (computed)
    double latitude = 0;      // minutes (positive = North)
    double longitude = 0;     // minutes (sign corrected: negative = West)
    double velocity = 0;      // km/h
    double heading = 0;       // degrees, 0=North, clockwise
    double height = 0;        // meters
    int solutionType = 0;
    int aviFileIndex = 0;
    double aviTime = 0;       // milliseconds into video
    double samplePeriod = 0;  // seconds
};

struct VboFile {
    std::vector<VboSample> samples;
    std::string aviBaseName;    // e.g. "VBOX0006_"
    std::string aviExtension;   // e.g. "mp4"
    std::string sourceDir;      // directory containing the VBO file

    // From [laptiming] section (sign-corrected longitude)
    double sfLon1 = 0, sfLat1 = 0;
    double sfLon2 = 0, sfLat2 = 0;
    bool hasLaptiming = false;

    double sampleRate = 0;      // Hz
};

struct CircuitInfo {
    std::string country;
    std::string name;
    double minLon = 0, minLat = 0;
    double maxLon = 0, maxLat = 0;
    double sfLon = 0, sfLat = 0;          // start/finish point
    double sfOldLon = 0, sfOldLat = 0;    // reference point (defines gate direction)
    double gateWidth = 25.0;
    int length = 0;
};

struct LapInfo {
    int lapNumber = 0;
    int startIdx = 0;
    int endIdx = 0;
    double lapTimeSeconds = 0;
    double startAviTime = 0;    // ms
    double endAviTime = 0;      // ms
};

// Parse a VBO file
bool parseVboFile(const std::string& path, VboFile& out);

// Parse the circuit database XML
bool loadCircuitDatabase(const std::string& xmlPath, std::vector<CircuitInfo>& circuits);

// Find the circuit matching the GPS data
bool detectCircuit(const VboFile& vbo, const std::vector<CircuitInfo>& circuits,
                   CircuitInfo& matched);

// Detect laps from start/finish line crossings
std::vector<LapInfo> detectLaps(const VboFile& vbo, double sfLon1, double sfLat1,
                                double sfLon2, double sfLat2, double gateWidth);

// Convert VBO time (hhmmss.sss) to seconds from midnight
double vboTimeToSeconds(double vboTime);

// Build video filename from VBO data and file index
std::string buildVideoFilename(const VboFile& vbo, int aviFileIndex);

// Convert lat/lon in minutes to local meters from a reference point
void gpsToLocalMeters(double lat, double lon, double refLat, double refLon,
                      double& eastMeters, double& northMeters);
