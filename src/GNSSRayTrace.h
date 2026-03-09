#pragma once

#include <vector>
#include <cstdint>
#include <cmath>
#include <math/vec3.h>

namespace gnss {

using Vec3 = filament::math::float3;

struct Triangle {
    Vec3 v0, v1, v2;
    Vec3 normal;
};

struct RayHit {
    bool hit = false;
    float t = 0;           // distance along ray
    Vec3 point = {0,0,0};  // world-space hit point
    Vec3 normal = {0,0,0}; // surface normal at hit
    int triIndex = -1;     // triangle index
};

struct BVHNode {
    Vec3 bmin, bmax;
    int leftFirst = 0;     // leaf: first index in mTriIndices; inner: left child node index
    int count = 0;          // >0 = leaf node with this many triangles
};

class BVH {
public:
    void build(const std::vector<Triangle>& triangles);
    RayHit trace(Vec3 origin, Vec3 direction, float tMax = 1e30f) const;
    const std::vector<Triangle>& triangles() const { return mTriangles; }
    int triangleCount() const { return (int)mTriangles.size(); }

private:
    std::vector<BVHNode> mNodes;
    std::vector<int> mTriIndices;
    std::vector<Triangle> mTriangles;
    int mNodeCount = 0;

    void buildRecursive(int nodeIdx, int start, int end);
    void intersectBVH(int nodeIdx, Vec3 origin, Vec3 invDir, float& tBest, RayHit& bestHit) const;
};

// Extract all triangles from a GLB (glTF binary) file loaded into memory.
// Handles node transform hierarchy, multiple meshes, and common vertex/index formats.
bool extractMeshFromGLB(const uint8_t* data, size_t size, std::vector<Triangle>& triangles);

} // namespace gnss
