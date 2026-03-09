#pragma once

#include <vector>
#include <string>

struct AlmanacEntry {
    int prn = 0;
    int health = 0;
    double eccentricity = 0;
    double toa = 0;              // Time of Applicability (seconds)
    double inclination = 0;      // Full orbital inclination (radians)
    double rateOfRightAscen = 0; // rad/s (OMEGA DOT)
    double sqrtA = 0;            // sqrt(semi-major axis) m^0.5
    double rightAscenAtWeek = 0; // OMEGA_0 (radians)
    double argOfPerigee = 0;     // omega (radians)
    double meanAnom = 0;         // M_0 (radians)
    double af0 = 0;              // clock correction (seconds)
    double af1 = 0;              // clock correction rate (s/s)
    int week = 0;                // GPS week number
};

struct SatellitePosition {
    int prn = 0;
    double azimuthDeg = 0;     // degrees, 0=N, 90=E, 180=S, 270=W
    double elevationDeg = 0;   // degrees, 0=horizon, 90=zenith
    double azimuthRad = 0;
    double elevationRad = 0;
    bool healthy = true;
    bool visible = false;       // above elevation mask
};

struct GPSTime {
    int week = 0;
    double tow = 0;  // Time of Week in seconds (0 = midnight Sat/Sun)
};

// Parse a YUMA-format GPS almanac file
bool parseYumaAlmanac(const std::string& filename, std::vector<AlmanacEntry>& almanac);

// Convert UTC date/time to GPS week and TOW
// Includes 18 leap seconds (valid from Jan 2017 onwards)
GPSTime utcToGPSTime(int year, int month, int day, int hour, int minute, double second);

// Compute satellite azimuth/elevation from receiver position using almanac orbital parameters
void computeSatellitePositions(
    const std::vector<AlmanacEntry>& almanac,
    const GPSTime& gpsTime,
    double receiverLatDeg, double receiverLonDeg, double receiverAltM,
    double elevationMaskDeg,
    std::vector<SatellitePosition>& positions);
