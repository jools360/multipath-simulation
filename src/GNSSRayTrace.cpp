// Workaround for nlohmann/json assert_invariant collision with Filament's macro
#ifdef assert_invariant
#undef assert_invariant
#endif
#include <nlohmann/json.hpp>
#include <utils/debug.h>

#include "GNSSRayTrace.h"

#include <iostream>
#include <algorithm>
#include <cstring>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

using json = nlohmann::json;

namespace gnss {

// ============================================================
// Moller-Trumbore ray-triangle intersection
// ============================================================
static bool intersectTriangle(Vec3 origin, Vec3 dir, const Triangle& tri,
                               float& t, float& u, float& v) {
    Vec3 e1 = tri.v1 - tri.v0;
    Vec3 e2 = tri.v2 - tri.v0;
    Vec3 h = cross(dir, e2);
    float a = dot(e1, h);
    if (fabsf(a) < 1e-8f) return false;

    float f = 1.0f / a;
    Vec3 s = origin - tri.v0;
    u = f * dot(s, h);
    if (u < 0.0f || u > 1.0f) return false;

    Vec3 q = cross(s, e1);
    v = f * dot(dir, q);
    if (v < 0.0f || u + v > 1.0f) return false;

    t = f * dot(e2, q);
    return t > 1e-4f;
}

// ============================================================
// BVH
// ============================================================
void BVH::build(const std::vector<Triangle>& triangles) {
    mTriangles = triangles;
    int n = (int)mTriangles.size();
    if (n == 0) return;

    mTriIndices.resize(n);
    for (int i = 0; i < n; i++) mTriIndices[i] = i;

    mNodes.resize(2 * n + 1); // upper bound for binary tree
    mNodeCount = 1; // root = node 0

    buildRecursive(0, 0, n);

    mNodes.resize(mNodeCount);
    std::cout << "BVH built: " << mNodeCount << " nodes, " << n << " triangles" << std::endl;
}

void BVH::buildRecursive(int nodeIdx, int start, int end) {
    BVHNode& node = mNodes[nodeIdx];

    // Compute bounding box
    node.bmin = {1e30f, 1e30f, 1e30f};
    node.bmax = {-1e30f, -1e30f, -1e30f};
    for (int i = start; i < end; i++) {
        const auto& tri = mTriangles[mTriIndices[i]];
        for (const auto& v : {tri.v0, tri.v1, tri.v2}) {
            node.bmin = min(node.bmin, v);
            node.bmax = max(node.bmax, v);
        }
    }

    int count = end - start;
    if (count <= 4) {
        node.leftFirst = start;
        node.count = count;
        return;
    }

    // Split on longest axis using median
    Vec3 extent = node.bmax - node.bmin;
    int axis = 0;
    if (extent.y > extent.x) axis = 1;
    if (extent.z > extent[axis]) axis = 2;

    int mid = (start + end) / 2;
    std::nth_element(
        mTriIndices.begin() + start,
        mTriIndices.begin() + mid,
        mTriIndices.begin() + end,
        [&](int a, int b) {
            Vec3 ca = (mTriangles[a].v0 + mTriangles[a].v1 + mTriangles[a].v2) * (1.0f / 3.0f);
            Vec3 cb = (mTriangles[b].v0 + mTriangles[b].v1 + mTriangles[b].v2) * (1.0f / 3.0f);
            return ca[axis] < cb[axis];
        }
    );

    int leftIdx = mNodeCount;
    mNodeCount += 2;

    node.leftFirst = leftIdx;
    node.count = 0;

    buildRecursive(leftIdx, start, mid);
    buildRecursive(leftIdx + 1, mid, end);
}

void BVH::intersectBVH(int nodeIdx, Vec3 origin, Vec3 invDir,
                         float& tBest, RayHit& bestHit) const {
    const BVHNode& node = mNodes[nodeIdx];

    // Slab test for AABB
    float tx1 = (node.bmin.x - origin.x) * invDir.x;
    float tx2 = (node.bmax.x - origin.x) * invDir.x;
    float tmin = std::min(tx1, tx2);
    float tmax = std::max(tx1, tx2);

    float ty1 = (node.bmin.y - origin.y) * invDir.y;
    float ty2 = (node.bmax.y - origin.y) * invDir.y;
    tmin = std::max(tmin, std::min(ty1, ty2));
    tmax = std::min(tmax, std::max(ty1, ty2));

    float tz1 = (node.bmin.z - origin.z) * invDir.z;
    float tz2 = (node.bmax.z - origin.z) * invDir.z;
    tmin = std::max(tmin, std::min(tz1, tz2));
    tmax = std::min(tmax, std::max(tz1, tz2));

    if (tmax < std::max(tmin, 0.0f) || tmin >= tBest) return;

    if (node.count > 0) {
        // Leaf - test triangles
        for (int i = node.leftFirst; i < node.leftFirst + node.count; i++) {
            int triIdx = mTriIndices[i];
            float t, u, v;
            if (intersectTriangle(origin, Vec3{1.0f/invDir.x, 1.0f/invDir.y, 1.0f/invDir.z},
                                  mTriangles[triIdx], t, u, v)) {
                if (t < tBest) {
                    tBest = t;
                    bestHit.hit = true;
                    bestHit.t = t;
                    bestHit.triIndex = triIdx;
                    bestHit.normal = mTriangles[triIdx].normal;
                    bestHit.point = origin + Vec3{1.0f/invDir.x, 1.0f/invDir.y, 1.0f/invDir.z} * t;
                }
            }
        }
    } else {
        // Inner node - recurse into children
        intersectBVH(node.leftFirst, origin, invDir, tBest, bestHit);
        intersectBVH(node.leftFirst + 1, origin, invDir, tBest, bestHit);
    }
}

RayHit BVH::trace(Vec3 origin, Vec3 direction, float tMax) const {
    RayHit hit;
    if (mNodes.empty()) return hit;

    // Compute inverse direction (handle zeros)
    Vec3 invDir = {
        fabsf(direction.x) > 1e-8f ? 1.0f / direction.x : (direction.x >= 0 ? 1e30f : -1e30f),
        fabsf(direction.y) > 1e-8f ? 1.0f / direction.y : (direction.y >= 0 ? 1e30f : -1e30f),
        fabsf(direction.z) > 1e-8f ? 1.0f / direction.z : (direction.z >= 0 ? 1e30f : -1e30f)
    };

    float tBest = tMax;
    intersectBVH(0, origin, invDir, tBest, hit);
    return hit;
}

// ============================================================
// GLB mesh extraction using nlohmann/json
// ============================================================

// 4x4 matrix multiply (column-major, like glTF)
struct Mat4 {
    double m[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

    static Mat4 identity() {
        Mat4 r;
        return r;
    }

    Vec3 transformPoint(Vec3 p) const {
        double x = m[0]*p.x + m[4]*p.y + m[8]*p.z  + m[12];
        double y = m[1]*p.x + m[5]*p.y + m[9]*p.z  + m[13];
        double z = m[2]*p.x + m[6]*p.y + m[10]*p.z + m[14];
        return {(float)x, (float)y, (float)z};
    }

    Vec3 transformNormal(Vec3 n) const {
        // For normal transformation, use the inverse transpose of the upper 3x3
        // For rigid transforms (no non-uniform scaling), just using the upper 3x3 is fine
        double x = m[0]*n.x + m[4]*n.y + m[8]*n.z;
        double y = m[1]*n.x + m[5]*n.y + m[9]*n.z;
        double z = m[2]*n.x + m[6]*n.y + m[10]*n.z;
        float len = (float)sqrt(x*x + y*y + z*z);
        if (len > 1e-8f) { x /= len; y /= len; z /= len; }
        return {(float)x, (float)y, (float)z};
    }

    Mat4 operator*(const Mat4& b) const {
        Mat4 r;
        for (int col = 0; col < 4; col++) {
            for (int row = 0; row < 4; row++) {
                r.m[col*4+row] = 0;
                for (int k = 0; k < 4; k++) {
                    r.m[col*4+row] += m[k*4+row] * b.m[col*4+k];
                }
            }
        }
        return r;
    }
};

// Build a transform matrix from glTF TRS components
static Mat4 buildTRSMatrix(const double* t, const double* r, const double* s) {
    // Quaternion to rotation matrix
    double qx = r[0], qy = r[1], qz = r[2], qw = r[3];
    double xx = qx*qx, yy = qy*qy, zz = qz*qz;
    double xy = qx*qy, xz = qx*qz, yz = qy*qz;
    double wx = qw*qx, wy = qw*qy, wz = qw*qz;

    Mat4 m;
    m.m[0]  = (1.0 - 2.0*(yy+zz)) * s[0];
    m.m[1]  = (2.0*(xy+wz)) * s[0];
    m.m[2]  = (2.0*(xz-wy)) * s[0];
    m.m[3]  = 0;

    m.m[4]  = (2.0*(xy-wz)) * s[1];
    m.m[5]  = (1.0 - 2.0*(xx+zz)) * s[1];
    m.m[6]  = (2.0*(yz+wx)) * s[1];
    m.m[7]  = 0;

    m.m[8]  = (2.0*(xz+wy)) * s[2];
    m.m[9]  = (2.0*(yz-wx)) * s[2];
    m.m[10] = (1.0 - 2.0*(xx+yy)) * s[2];
    m.m[11] = 0;

    m.m[12] = t[0];
    m.m[13] = t[1];
    m.m[14] = t[2];
    m.m[15] = 1;
    return m;
}

// Get local transform for a glTF node
static Mat4 getNodeTransform(const json& node) {
    if (node.contains("matrix")) {
        Mat4 m;
        const auto& mat = node["matrix"];
        for (int i = 0; i < 16; i++) m.m[i] = mat[i].get<double>();
        return m;
    }

    double t[3] = {0, 0, 0};
    double r[4] = {0, 0, 0, 1};
    double s[3] = {1, 1, 1};

    if (node.contains("translation")) {
        const auto& v = node["translation"];
        t[0] = v[0]; t[1] = v[1]; t[2] = v[2];
    }
    if (node.contains("rotation")) {
        const auto& v = node["rotation"];
        r[0] = v[0]; r[1] = v[1]; r[2] = v[2]; r[3] = v[3];
    }
    if (node.contains("scale")) {
        const auto& v = node["scale"];
        s[0] = v[0]; s[1] = v[1]; s[2] = v[2];
    }
    return buildTRSMatrix(t, r, s);
}

// Read float data from an accessor
static bool readFloatAccessor(const json& j, int accessorIdx, const uint8_t* binData,
                               std::vector<float>& out, int expectedComponents) {
    const auto& accessor = j["accessors"][accessorIdx];
    int count = accessor["count"].get<int>();
    int componentType = accessor["componentType"].get<int>();

    int bufferViewIdx = accessor.value("bufferView", -1);
    int accessorOffset = accessor.value("byteOffset", 0);

    if (bufferViewIdx < 0) return false;

    const auto& bv = j["bufferViews"][bufferViewIdx];
    int bvOffset = bv.value("byteOffset", 0);
    int bvStride = bv.value("byteStride", 0);

    if (componentType != 5126) { // 5126 = FLOAT
        std::cerr << "Unsupported position component type: " << componentType << std::endl;
        return false;
    }

    int stride = bvStride > 0 ? bvStride : (expectedComponents * 4);
    out.resize(count * expectedComponents);

    const uint8_t* base = binData + bvOffset + accessorOffset;
    for (int i = 0; i < count; i++) {
        const float* src = (const float*)(base + i * stride);
        for (int c = 0; c < expectedComponents; c++) {
            out[i * expectedComponents + c] = src[c];
        }
    }
    return true;
}

// Read index data from an accessor
static bool readIndexAccessor(const json& j, int accessorIdx, const uint8_t* binData,
                               std::vector<uint32_t>& out) {
    const auto& accessor = j["accessors"][accessorIdx];
    int count = accessor["count"].get<int>();
    int componentType = accessor["componentType"].get<int>();

    int bufferViewIdx = accessor.value("bufferView", -1);
    int accessorOffset = accessor.value("byteOffset", 0);

    if (bufferViewIdx < 0) return false;

    const auto& bv = j["bufferViews"][bufferViewIdx];
    int bvOffset = bv.value("byteOffset", 0);

    out.resize(count);
    const uint8_t* base = binData + bvOffset + accessorOffset;

    if (componentType == 5121) { // UNSIGNED_BYTE
        for (int i = 0; i < count; i++) out[i] = base[i];
    } else if (componentType == 5123) { // UNSIGNED_SHORT
        const uint16_t* src = (const uint16_t*)base;
        for (int i = 0; i < count; i++) out[i] = src[i];
    } else if (componentType == 5125) { // UNSIGNED_INT
        const uint32_t* src = (const uint32_t*)base;
        for (int i = 0; i < count; i++) out[i] = src[i];
    } else {
        std::cerr << "Unsupported index component type: " << componentType << std::endl;
        return false;
    }
    return true;
}

// Recursively process nodes and extract mesh triangles
static void processNode(const json& j, int nodeIdx, const Mat4& parentTransform,
                         const uint8_t* binData, std::vector<Triangle>& triangles) {
    const auto& node = j["nodes"][nodeIdx];
    Mat4 localTransform = getNodeTransform(node);
    Mat4 worldTransform = parentTransform * localTransform;

    // If this node has a mesh, extract its triangles
    if (node.contains("mesh")) {
        int meshIdx = node["mesh"].get<int>();
        const auto& mesh = j["meshes"][meshIdx];
        const auto& primitives = mesh["primitives"];

        for (const auto& prim : primitives) {
            // Only handle TRIANGLES mode (4) or default (unspecified = triangles)
            int mode = prim.value("mode", 4);
            if (mode != 4) continue;

            // Get position accessor
            if (!prim.contains("attributes") || !prim["attributes"].contains("POSITION")) continue;
            int posAccessorIdx = prim["attributes"]["POSITION"].get<int>();

            std::vector<float> positions;
            if (!readFloatAccessor(j, posAccessorIdx, binData, positions, 3)) continue;
            int vertexCount = (int)(positions.size() / 3);

            if (prim.contains("indices")) {
                // Indexed geometry
                int idxAccessorIdx = prim["indices"].get<int>();
                std::vector<uint32_t> indices;
                if (!readIndexAccessor(j, idxAccessorIdx, binData, indices)) continue;

                for (size_t i = 0; i + 2 < indices.size(); i += 3) {
                    uint32_t i0 = indices[i], i1 = indices[i+1], i2 = indices[i+2];
                    if (i0 >= (uint32_t)vertexCount || i1 >= (uint32_t)vertexCount ||
                        i2 >= (uint32_t)vertexCount) continue;

                    Triangle tri;
                    tri.v0 = worldTransform.transformPoint({positions[i0*3], positions[i0*3+1], positions[i0*3+2]});
                    tri.v1 = worldTransform.transformPoint({positions[i1*3], positions[i1*3+1], positions[i1*3+2]});
                    tri.v2 = worldTransform.transformPoint({positions[i2*3], positions[i2*3+1], positions[i2*3+2]});

                    Vec3 e1 = tri.v1 - tri.v0;
                    Vec3 e2 = tri.v2 - tri.v0;
                    tri.normal = normalize(cross(e1, e2));

                    // Skip degenerate triangles
                    float area = length(cross(e1, e2)) * 0.5f;
                    if (area > 1e-6f) {
                        triangles.push_back(tri);
                    }
                }
            } else {
                // Non-indexed: every 3 vertices form a triangle
                for (int i = 0; i + 2 < vertexCount; i += 3) {
                    Triangle tri;
                    tri.v0 = worldTransform.transformPoint({positions[i*3], positions[i*3+1], positions[i*3+2]});
                    tri.v1 = worldTransform.transformPoint({positions[(i+1)*3], positions[(i+1)*3+1], positions[(i+1)*3+2]});
                    tri.v2 = worldTransform.transformPoint({positions[(i+2)*3], positions[(i+2)*3+1], positions[(i+2)*3+2]});

                    Vec3 e1 = tri.v1 - tri.v0;
                    Vec3 e2 = tri.v2 - tri.v0;
                    tri.normal = normalize(cross(e1, e2));

                    float area = length(cross(e1, e2)) * 0.5f;
                    if (area > 1e-6f) {
                        triangles.push_back(tri);
                    }
                }
            }
        }
    }

    // Process children
    if (node.contains("children")) {
        for (const auto& childIdx : node["children"]) {
            processNode(j, childIdx.get<int>(), worldTransform, binData, triangles);
        }
    }
}

bool extractMeshFromGLB(const uint8_t* data, size_t size, std::vector<Triangle>& triangles) {
    triangles.clear();

    // Parse GLB header
    if (size < 12) {
        std::cerr << "GLB file too small" << std::endl;
        return false;
    }

    uint32_t magic, version, totalLength;
    memcpy(&magic, data, 4);
    memcpy(&version, data + 4, 4);
    memcpy(&totalLength, data + 8, 4);

    if (magic != 0x46546C67) { // "glTF"
        std::cerr << "Not a valid GLB file (bad magic)" << std::endl;
        return false;
    }
    if (version != 2) {
        std::cerr << "Unsupported glTF version: " << version << std::endl;
        return false;
    }

    // Parse JSON chunk
    if (size < 20) return false;
    uint32_t jsonLen, jsonType;
    memcpy(&jsonLen, data + 12, 4);
    memcpy(&jsonType, data + 16, 4);

    if (jsonType != 0x4E4F534A) { // "JSON"
        std::cerr << "First GLB chunk is not JSON" << std::endl;
        return false;
    }

    std::string jsonStr((const char*)(data + 20), jsonLen);
    json j;
    try {
        j = json::parse(jsonStr);
    } catch (const json::parse_error& e) {
        std::cerr << "Failed to parse GLB JSON: " << e.what() << std::endl;
        return false;
    }

    // Parse BIN chunk
    size_t binChunkStart = 20 + jsonLen;
    if (binChunkStart + 8 > size) {
        std::cerr << "No BIN chunk in GLB" << std::endl;
        return false;
    }

    uint32_t binLen, binType;
    memcpy(&binLen, data + binChunkStart, 4);
    memcpy(&binType, data + binChunkStart + 4, 4);

    if (binType != 0x004E4942) { // "BIN\0"
        std::cerr << "Second GLB chunk is not BIN" << std::endl;
        return false;
    }

    const uint8_t* binData = data + binChunkStart + 8;

    // Get the default scene (or scene 0)
    int sceneIdx = j.value("scene", 0);
    if (!j.contains("scenes") || sceneIdx >= (int)j["scenes"].size()) {
        std::cerr << "No scenes in GLB" << std::endl;
        return false;
    }

    const auto& scene = j["scenes"][sceneIdx];
    if (!scene.contains("nodes")) return false;

    Mat4 identity = Mat4::identity();
    for (const auto& nodeIdx : scene["nodes"]) {
        processNode(j, nodeIdx.get<int>(), identity, binData, triangles);
    }

    std::cout << "Extracted " << triangles.size() << " triangles from GLB" << std::endl;
    return !triangles.empty();
}

} // namespace gnss
