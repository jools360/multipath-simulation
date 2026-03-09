#include "GNSSAlmanac.h"

#include <fstream>
#include <sstream>
#include <iostream>
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// WGS-84 constants
static constexpr double WGS84_A = 6378137.0;              // semi-major axis (m)
static constexpr double WGS84_F = 1.0 / 298.257223563;    // flattening
static constexpr double WGS84_E2 = 2.0 * WGS84_F - WGS84_F * WGS84_F; // eccentricity squared
static constexpr double MU = 3.986005e14;                  // Earth gravitational parameter (m^3/s^2)
static constexpr double OMEGA_E_DOT = 7.2921151467e-5;    // Earth rotation rate (rad/s)
static constexpr int GPS_LEAP_SECONDS = 18;                // UTC to GPS offset (valid from Jan 2017)

// GPS epoch: January 6, 1980
static constexpr int GPS_EPOCH_JD = 2444244; // Julian Day Number for Jan 6, 1980 (integer part)

static double degToRad(double deg) { return deg * M_PI / 180.0; }
static double radToDeg(double rad) { return rad * 180.0 / M_PI; }

// Trim whitespace from both ends
static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

bool parseYumaAlmanac(const std::string& filename, std::vector<AlmanacEntry>& almanac) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Cannot open almanac file: " << filename << std::endl;
        return false;
    }

    almanac.clear();
    AlmanacEntry current;
    bool inEntry = false;
    int fieldCount = 0;

    std::string line;
    while (std::getline(file, line)) {
        std::string trimmed = trim(line);
        if (trimmed.empty()) continue;

        // Check for header line (starts with "********")
        if (trimmed.find("********") != std::string::npos) {
            if (inEntry && fieldCount >= 12) {
                almanac.push_back(current);
            }
            current = AlmanacEntry{};
            inEntry = true;
            fieldCount = 0;
            continue;
        }

        if (!inEntry) continue;

        // Parse key:value pairs
        size_t colonPos = trimmed.find(':');
        if (colonPos == std::string::npos) continue;

        std::string key = trim(trimmed.substr(0, colonPos));
        std::string value = trim(trimmed.substr(colonPos + 1));

        // Convert key to lowercase for flexible matching
        std::string keyLower = key;
        std::transform(keyLower.begin(), keyLower.end(), keyLower.begin(), ::tolower);

        try {
            if (keyLower == "id") {
                current.prn = std::stoi(value);
                fieldCount++;
            } else if (keyLower == "health") {
                current.health = std::stoi(value);
                fieldCount++;
            } else if (keyLower.find("eccentricity") != std::string::npos) {
                current.eccentricity = std::stod(value);
                fieldCount++;
            } else if (keyLower.find("time of applicability") != std::string::npos) {
                current.toa = std::stod(value);
                fieldCount++;
            } else if (keyLower.find("orbital inclination") != std::string::npos) {
                current.inclination = std::stod(value);
                fieldCount++;
            } else if (keyLower.find("rate of right ascen") != std::string::npos) {
                current.rateOfRightAscen = std::stod(value);
                fieldCount++;
            } else if (keyLower.find("sqrt(a)") != std::string::npos ||
                       keyLower.find("sqrt a") != std::string::npos) {
                current.sqrtA = std::stod(value);
                fieldCount++;
            } else if (keyLower.find("right ascen at week") != std::string::npos) {
                current.rightAscenAtWeek = std::stod(value);
                fieldCount++;
            } else if (keyLower.find("argument of perigee") != std::string::npos) {
                current.argOfPerigee = std::stod(value);
                fieldCount++;
            } else if (keyLower.find("mean anom") != std::string::npos) {
                current.meanAnom = std::stod(value);
                fieldCount++;
            } else if (keyLower.find("af0") != std::string::npos) {
                current.af0 = std::stod(value);
                fieldCount++;
            } else if (keyLower.find("af1") != std::string::npos) {
                current.af1 = std::stod(value);
                fieldCount++;
            } else if (keyLower == "week") {
                current.week = std::stoi(value);
                fieldCount++;
            }
        } catch (...) {
            std::cerr << "Warning: failed to parse almanac field: " << key << " = " << value << std::endl;
        }
    }

    // Don't forget the last entry
    if (inEntry && fieldCount >= 12) {
        almanac.push_back(current);
    }

    std::cout << "Loaded " << almanac.size() << " satellites from almanac" << std::endl;
    return !almanac.empty();
}

// Julian Day Number from calendar date (noon-based)
static double julianDayNumber(int year, int month, int day) {
    if (month <= 2) { year--; month += 12; }
    int A = year / 100;
    int B = 2 - A + A / 4;
    return (int)(365.25 * (year + 4716)) + (int)(30.6001 * (month + 1)) + day + B - 1524.5;
}

GPSTime utcToGPSTime(int year, int month, int day, int hour, int minute, double second) {
    double jd = julianDayNumber(year, month, day);
    double dayFrac = (hour + minute / 60.0 + second / 3600.0) / 24.0;
    double jdFull = jd + dayFrac;

    // Days since GPS epoch
    double daysSinceEpoch = jdFull - 2444244.5;

    // Add leap seconds (GPS time is ahead of UTC)
    double gpsSec = daysSinceEpoch * 86400.0 + GPS_LEAP_SECONDS;

    GPSTime result;
    result.week = (int)(gpsSec / 604800.0);
    result.tow = gpsSec - result.week * 604800.0;
    return result;
}

// Solve Kepler's equation: M = E - e*sin(E) for E
static double solveKepler(double M, double e, int maxIter = 10) {
    double E = M;  // initial guess
    for (int i = 0; i < maxIter; i++) {
        double dE = (M - E + e * sin(E)) / (1.0 - e * cos(E));
        E += dE;
        if (fabs(dE) < 1e-12) break;
    }
    return E;
}

// Convert geodetic (lat, lon, alt) to ECEF
static void geodeticToECEF(double latRad, double lonRad, double alt,
                            double& x, double& y, double& z) {
    double sinLat = sin(latRad);
    double cosLat = cos(latRad);
    double sinLon = sin(lonRad);
    double cosLon = cos(lonRad);

    double N = WGS84_A / sqrt(1.0 - WGS84_E2 * sinLat * sinLat);

    x = (N + alt) * cosLat * cosLon;
    y = (N + alt) * cosLat * sinLon;
    z = (N * (1.0 - WGS84_E2) + alt) * sinLat;
}

// Convert ECEF difference to ENU (East, North, Up) at reference position
static void ecefToENU(double dx, double dy, double dz,
                       double refLatRad, double refLonRad,
                       double& east, double& north, double& up) {
    double sinLat = sin(refLatRad);
    double cosLat = cos(refLatRad);
    double sinLon = sin(refLonRad);
    double cosLon = cos(refLonRad);

    east  = -sinLon * dx + cosLon * dy;
    north = -sinLat * cosLon * dx - sinLat * sinLon * dy + cosLat * dz;
    up    =  cosLat * cosLon * dx + cosLat * sinLon * dy + sinLat * dz;
}

void computeSatellitePositions(
    const std::vector<AlmanacEntry>& almanac,
    const GPSTime& gpsTime,
    double receiverLatDeg, double receiverLonDeg, double receiverAltM,
    double elevationMaskDeg,
    std::vector<SatellitePosition>& positions)
{
    positions.clear();
    positions.reserve(almanac.size());

    double recLatRad = degToRad(receiverLatDeg);
    double recLonRad = degToRad(receiverLonDeg);

    // Receiver ECEF position
    double recX, recY, recZ;
    geodeticToECEF(recLatRad, recLonRad, receiverAltM, recX, recY, recZ);

    double elevMaskRad = degToRad(elevationMaskDeg);

    for (const auto& alm : almanac) {
        SatellitePosition pos;
        pos.prn = alm.prn;
        pos.healthy = (alm.health == 0);

        // Semi-major axis
        double a = alm.sqrtA * alm.sqrtA;
        if (a < 1e6) { // sanity check
            positions.push_back(pos);
            continue;
        }

        // Mean motion
        double n = sqrt(MU / (a * a * a));

        // Time from almanac epoch
        double tk = (gpsTime.week - alm.week) * 604800.0 + (gpsTime.tow - alm.toa);
        // Normalize to +/- half week
        while (tk >  302400.0) tk -= 604800.0;
        while (tk < -302400.0) tk += 604800.0;

        // Mean anomaly
        double Mk = alm.meanAnom + n * tk;

        // Solve Kepler's equation
        double Ek = solveKepler(Mk, alm.eccentricity);

        // True anomaly
        double sinV = sqrt(1.0 - alm.eccentricity * alm.eccentricity) * sin(Ek) /
                       (1.0 - alm.eccentricity * cos(Ek));
        double cosV = (cos(Ek) - alm.eccentricity) /
                       (1.0 - alm.eccentricity * cos(Ek));
        double vk = atan2(sinV, cosV);

        // Argument of latitude
        double phik = vk + alm.argOfPerigee;

        // Radius
        double rk = a * (1.0 - alm.eccentricity * cos(Ek));

        // Position in orbital plane
        double xp = rk * cos(phik);
        double yp = rk * sin(phik);

        // Corrected longitude of ascending node
        double omegak = alm.rightAscenAtWeek +
                        (alm.rateOfRightAscen - OMEGA_E_DOT) * tk -
                        OMEGA_E_DOT * alm.toa;

        // ECEF coordinates
        double cosOmega = cos(omegak);
        double sinOmega = sin(omegak);
        double cosI = cos(alm.inclination);
        double sinI = sin(alm.inclination);

        double satX = xp * cosOmega - yp * cosI * sinOmega;
        double satY = xp * sinOmega + yp * cosI * cosOmega;
        double satZ = yp * sinI;

        // Delta ECEF
        double dx = satX - recX;
        double dy = satY - recY;
        double dz = satZ - recZ;

        // Convert to ENU
        double east, north, up;
        ecefToENU(dx, dy, dz, recLatRad, recLonRad, east, north, up);

        // Azimuth and elevation
        double horizDist = sqrt(east * east + north * north);
        pos.elevationRad = atan2(up, horizDist);
        pos.azimuthRad = atan2(east, north);
        if (pos.azimuthRad < 0) pos.azimuthRad += 2.0 * M_PI;

        pos.elevationDeg = radToDeg(pos.elevationRad);
        pos.azimuthDeg = radToDeg(pos.azimuthRad);

        // Visibility check
        pos.visible = (pos.elevationRad >= elevMaskRad) && pos.healthy;

        positions.push_back(pos);
    }
}
