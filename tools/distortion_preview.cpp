/*
 * Lens Distortion Preview
 *
 * Standalone tool that loads an OpenCV-format lens calibration JSON file
 * and visualises the distortion effect on a grid pattern.
 *
 * Usage: distortion_preview.exe [calibration.json]
 *        (defaults to RED_Komodo_calibration.json next to the exe)
 *
 * Keys:
 *   +/-  : Adjust distortion strength (multiplier)
 *   G    : Toggle grid / checkerboard
 *   R    : Reset strength to 1.0
 *   ESC  : Quit
 */

#ifdef _WIN32
#include <windows.h>
#endif

#include <opencv2/opencv.hpp>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <cmath>

// Minimal JSON value parser (avoids needing nlohmann on the tool path)
// Only handles the subset we need: objects, strings, numbers, arrays
#include <nlohmann/json.hpp>

using namespace cv;
using json = nlohmann::json;

struct LensCalibration {
    std::string cameraName;
    double fx, fy, cx, cy;          // pixel-space camera matrix
    double k1, k2, k3, p1, p2;     // distortion coefficients
    int imageWidth, imageHeight;
};

bool loadCalibration(const std::string& path, LensCalibration& cal) {
    std::ifstream in(path);
    if (!in) {
        std::cerr << "Cannot open: " << path << std::endl;
        return false;
    }
    json j;
    try { in >> j; }
    catch (const json::parse_error& e) {
        std::cerr << "JSON parse error: " << e.what() << std::endl;
        return false;
    }

    cal.cameraName = j.value("camera", "Unknown");
    cal.imageWidth  = 1920;
    cal.imageHeight = 1080;

    if (j.contains("image_dimensions_px")) {
        cal.imageWidth  = j["image_dimensions_px"].value("width", 1920);
        cal.imageHeight = j["image_dimensions_px"].value("height", 1080);
    }

    if (j.contains("camera_matrix")) {
        auto& cm = j["camera_matrix"];
        cal.fx = cm.value("fx", (double)cal.imageWidth);
        cal.fy = cm.value("fy", (double)cal.imageHeight);
        cal.cx = cm.value("cx", cal.imageWidth / 2.0);
        cal.cy = cm.value("cy", cal.imageHeight / 2.0);
    } else {
        cal.fx = cal.imageWidth;
        cal.fy = cal.imageHeight;
        cal.cx = cal.imageWidth / 2.0;
        cal.cy = cal.imageHeight / 2.0;
    }

    if (j.contains("distortion_coefficients")) {
        auto& dc = j["distortion_coefficients"];
        cal.k1 = dc.value("k1", 0.0);
        cal.k2 = dc.value("k2", 0.0);
        cal.k3 = dc.value("k3", 0.0);
        cal.p1 = dc.value("p1", 0.0);
        cal.p2 = dc.value("p2", 0.0);
    }

    return true;
}

Mat makeGrid(int width, int height, int spacing, bool checkerboard) {
    Mat img(height, width, CV_8UC3, Scalar(255, 255, 255));

    if (checkerboard) {
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int cx = x / spacing;
                int cy = y / spacing;
                if ((cx + cy) % 2 == 0) {
                    img.at<Vec3b>(y, x) = Vec3b(200, 200, 200);
                }
            }
        }
    }

    // Draw grid lines
    for (int x = 0; x < width; x += spacing) {
        line(img, Point(x, 0), Point(x, height - 1), Scalar(0, 0, 0), 1);
    }
    for (int y = 0; y < height; y += spacing) {
        line(img, Point(0, y), Point(width - 1, y), Scalar(0, 0, 0), 1);
    }

    // Draw thicker lines at center
    line(img, Point(width / 2, 0), Point(width / 2, height - 1), Scalar(0, 0, 200), 2);
    line(img, Point(0, height / 2), Point(width - 1, height / 2), Scalar(0, 0, 200), 2);

    // Draw crosshair at principal point
    return img;
}

void buildDistortionMaps(const LensCalibration& cal, double strength,
                         Mat& mapX, Mat& mapY)
{
    int w = cal.imageWidth;
    int h = cal.imageHeight;
    mapX.create(h, w, CV_32FC1);
    mapY.create(h, w, CV_32FC1);

    double k1 = cal.k1 * strength;
    double k2 = cal.k2 * strength;
    double k3 = cal.k3 * strength;
    double p1 = cal.p1 * strength;
    double p2 = cal.p2 * strength;

    // For each pixel in the DISTORTED output, find the UNDISTORTED source pixel.
    // Use cv::undistortPoints for accurate inversion.
    // Build a grid of all pixel coords, undistort them, use as remap source.

    Mat distCoeffs = (Mat_<double>(1, 5) << k1, k2, p1, p2, k3);
    Mat cameraMat = (Mat_<double>(3, 3) <<
        cal.fx, 0, cal.cx,
        0, cal.fy, cal.cy,
        0, 0, 1);

    // Create array of all distorted pixel positions
    std::vector<Point2f> distortedPts(w * h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            distortedPts[y * w + x] = Point2f((float)x, (float)y);

    // undistortPoints: distorted pixel → undistorted normalised → re-project with same camera
    std::vector<Point2f> undistortedPts;
    cv::undistortPoints(distortedPts, undistortedPts, cameraMat, distCoeffs,
                        noArray(), cameraMat);

    // Fill remap tables
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int idx = y * w + x;
            mapX.at<float>(y, x) = undistortedPts[idx].x;
            mapY.at<float>(y, x) = undistortedPts[idx].y;
        }
    }
}

int main(int argc, char* argv[]) {
    // Find calibration file
    std::string calibPath;
    if (argc > 1) {
        calibPath = argv[1];
    } else {
        // Try next to exe, then current dir
        std::vector<std::string> candidates = {
            "RED_Komodo_calibration.json",
            "../RED_Komodo_calibration.json",
        };
#ifdef _WIN32
        char exePath[MAX_PATH];
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        std::string exeDir(exePath);
        size_t slash = exeDir.find_last_of("\\/");
        if (slash != std::string::npos) {
            exeDir = exeDir.substr(0, slash + 1);
            candidates.insert(candidates.begin(), exeDir + "RED_Komodo_calibration.json");
        }
#endif
        for (auto& c : candidates) {
            if (std::ifstream(c).good()) { calibPath = c; break; }
        }
        if (calibPath.empty()) {
            std::cerr << "Usage: distortion_preview [calibration.json]" << std::endl;
            return 1;
        }
    }

    LensCalibration cal;
    if (!loadCalibration(calibPath, cal)) return 1;

    std::cout << "Camera: " << cal.cameraName << std::endl;
    std::cout << "Image:  " << cal.imageWidth << "x" << cal.imageHeight << std::endl;
    std::cout << "fx=" << cal.fx << " fy=" << cal.fy
              << " cx=" << cal.cx << " cy=" << cal.cy << std::endl;
    std::cout << "k1=" << cal.k1 << " k2=" << cal.k2 << " k3=" << cal.k3
              << " p1=" << cal.p1 << " p2=" << cal.p2 << std::endl;
    std::cout << std::endl;
    std::cout << "Keys: +/- adjust strength, G toggle grid, R reset, ESC quit" << std::endl;

    double strength = 1.0;
    bool checkerboard = false;
    int gridSpacing = 60;

    // Display at half resolution for usability
    int dispW = cal.imageWidth / 2;
    int dispH = cal.imageHeight / 2;

    namedWindow("Distortion Preview", WINDOW_AUTOSIZE);

    while (true) {
        // Build grid at full calibration resolution
        Mat grid = makeGrid(cal.imageWidth, cal.imageHeight, gridSpacing, checkerboard);

        // Draw principal point crosshair
        int ppx = (int)cal.cx, ppy = (int)cal.cy;
        line(grid, Point(ppx - 20, ppy), Point(ppx + 20, ppy), Scalar(200, 0, 0), 2);
        line(grid, Point(ppx, ppy - 20), Point(ppx, ppy + 20), Scalar(200, 0, 0), 2);

        // Build distortion maps and apply
        Mat mapX, mapY;
        buildDistortionMaps(cal, strength, mapX, mapY);

        Mat distorted;
        remap(grid, distorted, mapX, mapY, INTER_LINEAR, BORDER_CONSTANT, Scalar(40, 40, 40));

        // Draw principal point on distorted too
        line(distorted, Point(ppx - 20, ppy), Point(ppx + 20, ppy), Scalar(200, 0, 0), 2);
        line(distorted, Point(ppx, ppy - 20), Point(ppx, ppy + 20), Scalar(200, 0, 0), 2);

        // Add labels
        putText(grid, "Undistorted", Point(20, 40), FONT_HERSHEY_SIMPLEX, 1.0, Scalar(0, 0, 200), 2);
        char label[128];
        snprintf(label, sizeof(label), "Distorted (x%.2f)", strength);
        putText(distorted, label, Point(20, 40), FONT_HERSHEY_SIMPLEX, 1.0, Scalar(0, 0, 200), 2);

        // Put camera name
        putText(distorted, cal.cameraName.c_str(), Point(20, 80),
                FONT_HERSHEY_SIMPLEX, 0.7, Scalar(0, 150, 0), 2);

        // Side by side
        Mat sideBySide;
        hconcat(grid, distorted, sideBySide);

        // Resize for display
        Mat display;
        resize(sideBySide, display, Size(dispW * 2, dispH));

        imshow("Distortion Preview", display);

        int key = waitKey(0) & 0xFF;
        if (key == 27) break;  // ESC
        else if (key == '+' || key == '=') { strength += 0.1; std::cout << "Strength: " << strength << std::endl; }
        else if (key == '-' || key == '_') { strength = std::max(0.0, strength - 0.1); std::cout << "Strength: " << strength << std::endl; }
        else if (key == 'g' || key == 'G') { checkerboard = !checkerboard; }
        else if (key == 'r' || key == 'R') { strength = 1.0; std::cout << "Strength: " << strength << std::endl; }
    }

    destroyAllWindows();
    return 0;
}
