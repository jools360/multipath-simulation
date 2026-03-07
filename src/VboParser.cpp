#include "VboParser.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <filesystem>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static std::string toLower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), ::tolower);
    return r;
}

static std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

double vboTimeToSeconds(double vboTime) {
    int hh = (int)(vboTime / 10000.0);
    int mm = (int)((vboTime - hh * 10000.0) / 100.0);
    double ss = vboTime - hh * 10000.0 - mm * 100.0;
    return hh * 3600.0 + mm * 60.0 + ss;
}

std::string buildVideoFilename(const VboFile& vbo, int aviFileIndex) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%04d", aviFileIndex);
    return vbo.sourceDir + "/" + vbo.aviBaseName + buf + "." + vbo.aviExtension;
}

void gpsToLocalMeters(double lat, double lon, double refLat, double refLon,
                      double& eastMeters, double& northMeters) {
    double latRad = (refLat / 60.0) * M_PI / 180.0;
    northMeters = (lat - refLat) * 1852.0;
    eastMeters = (lon - refLon) * 1852.0 * cos(latRad);
}

bool parseVboFile(const std::string& path, VboFile& out) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "Cannot open VBO file: " << path << std::endl;
        return false;
    }

    out = VboFile{};

    // Store source directory
    std::filesystem::path p(path);
    out.sourceDir = p.parent_path().string();

    std::string line;
    std::string section;
    std::vector<std::string> columnNames;

    // Column indices
    int colSats = -1, colTime = -1, colLat = -1, colLong = -1;
    int colVelocity = -1, colHeading = -1, colHeight = -1;
    int colSamplePeriod = -1, colSolutionType = -1;
    int colAviFileIndex = -1, colAviTime = -1;

    while (std::getline(file, line)) {
        std::string trimmed = trim(line);
        if (trimmed.empty()) continue;

        // Section headers
        if (trimmed.front() == '[' && trimmed.back() == ']') {
            section = toLower(trimmed.substr(1, trimmed.size() - 2));
            continue;
        }

        if (section == "avi") {
            if (out.aviBaseName.empty())
                out.aviBaseName = trimmed;
            else if (out.aviExtension.empty())
                out.aviExtension = trimmed;
        }
        else if (section == "laptiming") {
            std::istringstream iss(trimmed);
            std::string type;
            iss >> type;
            if (toLower(type) == "start") {
                double lon1, lat1, lon2, lat2;
                iss >> lon1 >> lat1 >> lon2 >> lat2;
                // Negate longitude (VBO inverts sign)
                out.sfLon1 = -lon1;
                out.sfLat1 = lat1;
                out.sfLon2 = -lon2;
                out.sfLat2 = lat2;
                out.hasLaptiming = true;
            }
        }
        else if (section == "column names") {
            std::istringstream iss(trimmed);
            std::string col;
            while (iss >> col) {
                columnNames.push_back(toLower(col));
            }
            for (int i = 0; i < (int)columnNames.size(); i++) {
                const auto& name = columnNames[i];
                if (name == "sats" || name == "satellites") colSats = i;
                else if (name == "time") colTime = i;
                else if (name == "lat" || name == "latitude") colLat = i;
                else if (name == "long" || name == "longitude") colLong = i;
                else if (name.find("velocity") != std::string::npos) colVelocity = i;
                else if (name == "heading") colHeading = i;
                else if (name == "height") colHeight = i;
                else if (name == "tsample" || name == "sampleperiod") colSamplePeriod = i;
                else if (name == "solution_type" || name == "solutiontype") colSolutionType = i;
                else if (name == "avifileindex") colAviFileIndex = i;
                else if (name == "avitime" || name == "avisynctime") colAviTime = i;
            }
        }
        else if (section == "data") {
            std::istringstream iss(trimmed);
            std::vector<std::string> tokens;
            std::string token;
            while (iss >> token) tokens.push_back(token);

            if ((int)tokens.size() < (int)columnNames.size()) continue;

            auto getD = [&](int col) -> double {
                if (col < 0 || col >= (int)tokens.size()) return 0.0;
                try { return std::stod(tokens[col]); } catch (...) { return 0.0; }
            };
            auto getI = [&](int col) -> int {
                if (col < 0 || col >= (int)tokens.size()) return 0;
                try { return std::stoi(tokens[col]); } catch (...) { return 0; }
            };

            VboSample s;
            s.satellites = getI(colSats);
            s.time = getD(colTime);
            s.timeSeconds = vboTimeToSeconds(s.time);
            s.latitude = getD(colLat);
            s.longitude = -getD(colLong);  // Negate: VBO inverts longitude sign
            s.velocity = getD(colVelocity);
            s.heading = getD(colHeading);
            s.height = getD(colHeight);
            s.samplePeriod = getD(colSamplePeriod);
            s.solutionType = getI(colSolutionType);
            s.aviFileIndex = getI(colAviFileIndex);
            s.aviTime = getD(colAviTime);

            out.samples.push_back(s);
        }
    }

    if (!out.samples.empty() && out.samples[0].samplePeriod > 0) {
        out.sampleRate = 1.0 / out.samples[0].samplePeriod;
    }

    std::cout << "Parsed VBO: " << out.samples.size() << " samples at "
              << out.sampleRate << " Hz" << std::endl;
    if (out.hasLaptiming) {
        std::cout << "  Laptiming S/F: (" << out.sfLon1 << ", " << out.sfLat1 << ")" << std::endl;
    }
    return !out.samples.empty();
}

// Simple XML attribute extractor
static std::string getAttr(const std::string& line, const std::string& attr) {
    std::string search = attr + "=\"";
    auto pos = line.find(search);
    if (pos == std::string::npos) return "";
    pos += search.size();
    auto end = line.find('"', pos);
    if (end == std::string::npos) return "";
    return line.substr(pos, end - pos);
}

static void parseMinMax(const std::string& val, double& a, double& b) {
    auto comma = val.find(',');
    if (comma == std::string::npos) return;
    try {
        a = std::stod(val.substr(0, comma));
        b = std::stod(val.substr(comma + 1));
    } catch (...) {}
}

bool loadCircuitDatabase(const std::string& xmlPath, std::vector<CircuitInfo>& circuits) {
    std::ifstream file(xmlPath);
    if (!file.is_open()) {
        std::cerr << "Cannot open circuit database: " << xmlPath << std::endl;
        return false;
    }

    circuits.clear();
    std::string line;
    std::string currentCountry;
    CircuitInfo currentCircuit;
    bool inCircuit = false;

    while (std::getline(file, line)) {
        std::string trimmed = trim(line);

        if (trimmed.find("<country ") != std::string::npos) {
            currentCountry = getAttr(trimmed, "name");
        }
        else if (trimmed.find("</country>") != std::string::npos) {
            currentCountry.clear();
        }
        else if (trimmed.find("<circuit ") != std::string::npos) {
            currentCircuit = CircuitInfo{};
            currentCircuit.country = currentCountry;
            currentCircuit.name = getAttr(trimmed, "name");

            std::string minVal = getAttr(trimmed, "min");
            std::string maxVal = getAttr(trimmed, "max");
            parseMinMax(minVal, currentCircuit.minLon, currentCircuit.minLat);
            parseMinMax(maxVal, currentCircuit.maxLon, currentCircuit.maxLat);

            std::string lenStr = getAttr(trimmed, "length");
            if (!lenStr.empty()) {
                try { currentCircuit.length = std::stoi(lenStr); } catch (...) {}
            }
            std::string gwStr = getAttr(trimmed, "gatewidth");
            if (!gwStr.empty()) {
                try { currentCircuit.gateWidth = std::stod(gwStr); } catch (...) {}
            }

            inCircuit = true;

            // Check if circuit tag is self-closing or has splitinfo on same line
            if (trimmed.find("/>") != std::string::npos) {
                inCircuit = false;
            }
        }
        else if (trimmed.find("<startFinish ") != std::string::npos && inCircuit) {
            std::string lonStr = getAttr(trimmed, "long");
            std::string latStr = getAttr(trimmed, "lat");
            std::string oldLonStr = getAttr(trimmed, "oldlong");
            std::string oldLatStr = getAttr(trimmed, "oldlat");

            try {
                if (!lonStr.empty()) currentCircuit.sfLon = std::stod(lonStr);
                if (!latStr.empty()) currentCircuit.sfLat = std::stod(latStr);
                if (!oldLonStr.empty()) currentCircuit.sfOldLon = std::stod(oldLonStr);
                if (!oldLatStr.empty()) currentCircuit.sfOldLat = std::stod(oldLatStr);
            } catch (...) {}
        }
        else if (trimmed.find("</circuit>") != std::string::npos) {
            if (inCircuit && currentCircuit.sfLon != 0) {
                circuits.push_back(currentCircuit);
            }
            inCircuit = false;
        }
    }

    std::cout << "Loaded " << circuits.size() << " circuits from database" << std::endl;
    return !circuits.empty();
}

bool detectCircuit(const VboFile& vbo, const std::vector<CircuitInfo>& circuits,
                   CircuitInfo& matched) {
    if (vbo.samples.empty() || circuits.empty()) return false;

    // Compute average position from first 500 valid samples
    double sumLat = 0, sumLon = 0;
    int count = 0;
    for (const auto& s : vbo.samples) {
        if (s.satellites < 4) continue;
        sumLat += s.latitude;
        sumLon += s.longitude;
        count++;
        if (count >= 500) break;
    }
    if (count == 0) return false;

    double avgLat = sumLat / count;
    double avgLon = sumLon / count;

    std::cout << "Average GPS position: lat=" << avgLat << " lon=" << avgLon
              << " (minutes)" << std::endl;

    // Find circuit whose bounding box contains this point
    double bestDist = 1e18;
    int bestIdx = -1;

    for (int i = 0; i < (int)circuits.size(); i++) {
        const auto& c = circuits[i];
        double cMinLon = std::min(c.minLon, c.maxLon);
        double cMaxLon = std::max(c.minLon, c.maxLon);
        double cMinLat = std::min(c.minLat, c.maxLat);
        double cMaxLat = std::max(c.minLat, c.maxLat);

        // Check bounding box containment
        if (avgLon >= cMinLon && avgLon <= cMaxLon &&
            avgLat >= cMinLat && avgLat <= cMaxLat) {
            // Prefer smaller bounding boxes (more specific)
            double area = (cMaxLon - cMinLon) * (cMaxLat - cMinLat);
            if (area < bestDist) {
                bestDist = area;
                bestIdx = i;
            }
        }
    }

    // Fallback: find nearest circuit by start/finish distance
    if (bestIdx < 0) {
        for (int i = 0; i < (int)circuits.size(); i++) {
            double dLat = avgLat - circuits[i].sfLat;
            double dLon = avgLon - circuits[i].sfLon;
            double dist = dLat * dLat + dLon * dLon;
            if (dist < bestDist) {
                bestDist = dist;
                bestIdx = i;
            }
        }
        // Only accept if within ~5km (about 2.7 minutes at mid-latitudes)
        if (bestDist > 2.7 * 2.7) {
            std::cerr << "No matching circuit found in database" << std::endl;
            return false;
        }
    }

    if (bestIdx >= 0) {
        matched = circuits[bestIdx];
        std::cout << "Detected circuit: " << matched.name << " (" << matched.country << ")"
                  << std::endl;
        return true;
    }
    return false;
}

// 2D cross product of vectors (p2-p1) and (p3-p1)
static double cross2d(double x1, double y1, double x2, double y2, double x3, double y3) {
    return (x2 - x1) * (y3 - y1) - (y2 - y1) * (x3 - x1);
}

// Check if line segments (a1,a2) and (b1,b2) intersect
static bool segmentsIntersect(double ax1, double ay1, double ax2, double ay2,
                               double bx1, double by1, double bx2, double by2) {
    double d1 = cross2d(bx1, by1, bx2, by2, ax1, ay1);
    double d2 = cross2d(bx1, by1, bx2, by2, ax2, ay2);
    double d3 = cross2d(ax1, ay1, ax2, ay2, bx1, by1);
    double d4 = cross2d(ax1, ay1, ax2, ay2, bx2, by2);

    if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) &&
        ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0))) {
        return true;
    }
    return false;
}

std::vector<LapInfo> detectLaps(const VboFile& vbo, double sfLon1, double sfLat1,
                                double sfLon2, double sfLat2, double gateWidth) {
    std::vector<LapInfo> laps;
    if (vbo.samples.size() < 2) return laps;

    // The start/finish line is perpendicular to the direction from old to new point,
    // passing through the new point. Create a gate segment.
    double dirLon = sfLon1 - sfLon2;
    double dirLat = sfLat1 - sfLat2;
    double dirLen = sqrt(dirLon * dirLon + dirLat * dirLat);
    if (dirLen < 1e-10) return laps;

    // Perpendicular direction (rotated 90 degrees)
    double perpLon = -dirLat / dirLen;
    double perpLat = dirLon / dirLen;

    // Gate width in minutes (approximate)
    double latRad = (sfLat1 / 60.0) * M_PI / 180.0;
    double metersPerMinLat = 1852.0;
    double halfGateMinutes = (gateWidth / 2.0) / metersPerMinLat;

    // Gate segment endpoints
    double gx1 = sfLon1 + perpLon * halfGateMinutes;
    double gy1 = sfLat1 + perpLat * halfGateMinutes;
    double gx2 = sfLon1 - perpLon * halfGateMinutes;
    double gy2 = sfLat1 - perpLat * halfGateMinutes;

    // Find crossings
    std::vector<int> crossingIndices;
    for (int i = 1; i < (int)vbo.samples.size(); i++) {
        const auto& s0 = vbo.samples[i - 1];
        const auto& s1 = vbo.samples[i];

        // Skip samples with poor GPS
        if (s0.satellites < 4 || s1.satellites < 4) continue;
        // Skip very low speed (stationary)
        if (s0.velocity < 5.0) continue;

        if (segmentsIntersect(s0.longitude, s0.latitude, s1.longitude, s1.latitude,
                              gx1, gy1, gx2, gy2)) {
            // Only accept crossings in the correct direction (same direction as travel)
            double travelDot = (s1.longitude - s0.longitude) * dirLon +
                               (s1.latitude - s0.latitude) * dirLat;
            if (travelDot > 0) {
                // Avoid duplicate crossings (require minimum 10 seconds between)
                if (crossingIndices.empty() ||
                    (s1.timeSeconds - vbo.samples[crossingIndices.back()].timeSeconds) > 10.0) {
                    crossingIndices.push_back(i);
                }
            }
        }
    }

    // Create laps from consecutive crossings
    for (int i = 1; i < (int)crossingIndices.size(); i++) {
        LapInfo lap;
        lap.lapNumber = i;
        lap.startIdx = crossingIndices[i - 1];
        lap.endIdx = crossingIndices[i];
        lap.startAviTime = vbo.samples[lap.startIdx].aviTime;
        lap.endAviTime = vbo.samples[lap.endIdx].aviTime;
        lap.lapTimeSeconds = vbo.samples[lap.endIdx].timeSeconds -
                             vbo.samples[lap.startIdx].timeSeconds;
        laps.push_back(lap);
    }

    if (!laps.empty()) {
        std::cout << "Detected " << laps.size() << " laps:" << std::endl;
        for (const auto& lap : laps) {
            int mins = (int)(lap.lapTimeSeconds / 60.0);
            double secs = lap.lapTimeSeconds - mins * 60.0;
            printf("  Lap %d: %d:%06.3f\n", lap.lapNumber, mins, secs);
        }
    }

    return laps;
}
