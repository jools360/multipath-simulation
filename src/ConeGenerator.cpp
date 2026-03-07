#include "ConeGenerator.h"
#include <cmath>
#include <algorithm>

// Pack a normal into a tangent-frame quaternion
// This creates a quaternion that represents a rotation from +Z to the normal
void ConeGenerator::packTangentFrame(float nx, float ny, float nz, int16_t* out) {
    // Normalize the normal
    float len = std::sqrt(nx*nx + ny*ny + nz*nz);
    if (len > 0.0001f) {
        nx /= len;
        ny /= len;
        nz /= len;
    }

    // Generate a tangent perpendicular to the normal
    float tx, ty, tz;
    if (std::abs(ny) < 0.999f) {
        // Cross product with Y axis
        tx = nz;
        ty = 0.0f;
        tz = -nx;
    } else {
        // Cross product with X axis
        tx = 0.0f;
        ty = -nz;
        tz = ny;
    }

    // Normalize tangent
    len = std::sqrt(tx*tx + ty*ty + tz*tz);
    if (len > 0.0001f) {
        tx /= len;
        ty /= len;
        tz /= len;
    }

    // Bitangent = normal cross tangent
    float bx = ny * tz - nz * ty;
    float by = nz * tx - nx * tz;
    float bz = nx * ty - ny * tx;

    // Convert TBN matrix to quaternion
    // The TBN matrix is [T B N] where columns are tangent, bitangent, normal
    float trace = tx + by + nz;
    float qx, qy, qz, qw;

    if (trace > 0.0f) {
        float s = 0.5f / std::sqrt(trace + 1.0f);
        qw = 0.25f / s;
        qx = (bz - ny) * s;
        qy = (nx - tz) * s;
        qz = (ty - bx) * s;
    } else if (tx > by && tx > nz) {
        float s = 2.0f * std::sqrt(1.0f + tx - by - nz);
        qw = (bz - ny) / s;
        qx = 0.25f * s;
        qy = (bx + ty) / s;
        qz = (nx + tz) / s;
    } else if (by > nz) {
        float s = 2.0f * std::sqrt(1.0f + by - tx - nz);
        qw = (nx - tz) / s;
        qx = (bx + ty) / s;
        qy = 0.25f * s;
        qz = (ny + bz) / s;
    } else {
        float s = 2.0f * std::sqrt(1.0f + nz - tx - by);
        qw = (ty - bx) / s;
        qx = (nx + tz) / s;
        qy = (ny + bz) / s;
        qz = 0.25f * s;
    }

    // Normalize quaternion
    len = std::sqrt(qx*qx + qy*qy + qz*qz + qw*qw);
    if (len > 0.0001f) {
        qx /= len;
        qy /= len;
        qz /= len;
        qw /= len;
    }

    // Ensure w is positive (quaternion sign convention)
    if (qw < 0) {
        qx = -qx;
        qy = -qy;
        qz = -qz;
        qw = -qw;
    }

    // Pack to SHORT4 (-32767 to 32767)
    const float scale = 32767.0f;
    out[0] = static_cast<int16_t>(std::clamp(qx * scale, -32767.0f, 32767.0f));
    out[1] = static_cast<int16_t>(std::clamp(qy * scale, -32767.0f, 32767.0f));
    out[2] = static_cast<int16_t>(std::clamp(qz * scale, -32767.0f, 32767.0f));
    out[3] = static_cast<int16_t>(std::clamp(qw * scale, -32767.0f, 32767.0f));
}

void ConeGenerator::generate(
    float baseRadius,
    float height,
    int segments,
    std::vector<Vertex>& vertices,
    std::vector<uint16_t>& indices
) {
    vertices.clear();
    indices.clear();

    const float PI = 3.14159265358979323846f;

    // Calculate the slant height and normal angle for the cone sides
    float slantHeight = std::sqrt(baseRadius * baseRadius + height * height);
    float normalY = baseRadius / slantHeight;
    float normalXZScale = height / slantHeight;

    // Generate side vertices
    for (int i = 0; i <= segments; ++i) {
        float angle = 2.0f * PI * i / segments;
        float cosA = std::cos(angle);
        float sinA = std::sin(angle);

        // Normal for this segment (pointing outward and up along cone surface)
        float nx = cosA * normalXZScale;
        float ny = normalY;
        float nz = sinA * normalXZScale;

        // Normalize
        float len = std::sqrt(nx*nx + ny*ny + nz*nz);
        nx /= len;
        ny /= len;
        nz /= len;

        // Base vertex
        Vertex baseVertex;
        baseVertex.position[0] = baseRadius * cosA;
        baseVertex.position[1] = 0.0f;
        baseVertex.position[2] = baseRadius * sinA;
        packTangentFrame(nx, ny, nz, baseVertex.tangents);
        vertices.push_back(baseVertex);

        // Apex vertex (same normal for smooth shading along this edge)
        Vertex apexVertex;
        apexVertex.position[0] = 0.0f;
        apexVertex.position[1] = height;
        apexVertex.position[2] = 0.0f;
        packTangentFrame(nx, ny, nz, apexVertex.tangents);
        vertices.push_back(apexVertex);
    }

    // Generate side indices (triangle fan from apex)
    for (int i = 0; i < segments; ++i) {
        uint16_t base1 = i * 2;         // Current base vertex
        uint16_t apex1 = i * 2 + 1;     // Current apex vertex
        uint16_t base2 = (i + 1) * 2;   // Next base vertex

        // Triangle: apex, base2, base1 (reversed winding)
        indices.push_back(apex1);
        indices.push_back(base2);
        indices.push_back(base1);
    }

    // Generate base cap
    // Center vertex for base
    uint16_t baseCenterIdx = static_cast<uint16_t>(vertices.size());
    Vertex baseCenter;
    baseCenter.position[0] = 0.0f;
    baseCenter.position[1] = 0.0f;
    baseCenter.position[2] = 0.0f;
    packTangentFrame(0.0f, -1.0f, 0.0f, baseCenter.tangents);  // Pointing down
    vertices.push_back(baseCenter);

    // Base rim vertices (with downward normal)
    uint16_t baseRimStart = static_cast<uint16_t>(vertices.size());
    for (int i = 0; i <= segments; ++i) {
        float angle = 2.0f * PI * i / segments;

        Vertex rimVertex;
        rimVertex.position[0] = baseRadius * std::cos(angle);
        rimVertex.position[1] = 0.0f;
        rimVertex.position[2] = baseRadius * std::sin(angle);
        packTangentFrame(0.0f, -1.0f, 0.0f, rimVertex.tangents);
        vertices.push_back(rimVertex);
    }

    // Base cap indices (triangle fan)
    for (int i = 0; i < segments; ++i) {
        indices.push_back(baseCenterIdx);
        indices.push_back(baseRimStart + i);
        indices.push_back(baseRimStart + i + 1);
    }
}
