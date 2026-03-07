#pragma once

#include <vector>
#include <cmath>
#include <cstdint>

// Vertex structure with position and packed tangent quaternion
struct Vertex {
    float position[3];
    int16_t tangents[4];  // Quaternion-encoded tangent frame (SHORT4)
};

class ConeGenerator {
public:
    // Generate a cone mesh
    // baseRadius: radius of the cone base
    // height: height of the cone
    // segments: number of segments around the base (more = smoother)
    static void generate(
        float baseRadius,
        float height,
        int segments,
        std::vector<Vertex>& vertices,
        std::vector<uint16_t>& indices
    );

private:
    // Pack a normal vector into a quaternion (SHORT4)
    static void packTangentFrame(float nx, float ny, float nz, int16_t* out);
};
