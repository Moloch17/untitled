#include "terrain/heightfield.h"

#include <cmath>
#include <cstdint>

namespace terrain {

namespace {

// Fixed seed: the terrain must be identical on every client and the server.
constexpr uint32_t kSeed = 0x9E3779B9u;

// Feature size of the largest octave, in metres.
// Broad enough that the tall amplitude reads as hills and valleys rather
// than noise.
constexpr float kBaseWavelength = 340.0f;
constexpr int kOctaves = 5;
constexpr float kLacunarity = 2.03f;  // slightly off 2 to avoid grid alignment
constexpr float kGain = 0.5f;

// Deterministic hash -> [0,1). Integer maths only, so it can't drift between
// compilers the way a float hash based on sin() would.
float hash(int32_t x, int32_t y) {
    uint32_t h = kSeed;
    h ^= static_cast<uint32_t>(x) * 0x85EBCA6Bu;
    h ^= static_cast<uint32_t>(y) * 0xC2B2AE35u;
    h ^= h >> 15;
    h *= 0x2545F491u;
    h ^= h >> 13;
    h *= 0x3B9ACB93u;
    h ^= h >> 16;
    return static_cast<float>(h & 0x00FFFFFFu) / static_cast<float>(0x01000000u);
}

// Quintic fade. The cubic smoothstep is only C1 continuous: its second
// derivative jumps at every lattice line, and curvature discontinuities show up
// directly in shading as faint creases along a grid. This is the standard
// improved-Perlin fade and is C2.
float fade(float t) {
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

// Value noise: random values on a lattice, smoothly interpolated.
float valueNoise(float x, float y) {
    const float floorX = std::floor(x);
    const float floorY = std::floor(y);
    const auto cellX = static_cast<int32_t>(floorX);
    const auto cellY = static_cast<int32_t>(floorY);

    const float fx = fade(x - floorX);
    const float fy = fade(y - floorY);

    const float topLeft = hash(cellX, cellY);
    const float topRight = hash(cellX + 1, cellY);
    const float bottomLeft = hash(cellX, cellY + 1);
    const float bottomRight = hash(cellX + 1, cellY + 1);

    const float top = topLeft + (topRight - topLeft) * fx;
    const float bottom = bottomLeft + (bottomRight - bottomLeft) * fx;
    return top + (bottom - top) * fy;
}


// Raw fractal height before smoothing.
float rawHeight(float x, float z) {
    // Fractal noise: a few octaves of decreasing size and strength, which is
    // what gives both broad hills and smaller undulation on their slopes.
    float frequency = 1.0f / kBaseWavelength;
    float amplitude = 1.0f;
    float total = 0.0f;
    float normalisation = 0.0f;

    for (int octave = 0; octave < kOctaves; ++octave) {
        total += valueNoise(x * frequency, z * frequency) * amplitude;
        normalisation += amplitude;
        frequency *= kLacunarity;
        amplitude *= kGain;
    }

    // Centre on zero so roughly half the terrain sits below the origin's level.
    const float unit = (total / normalisation) * 2.0f - 1.0f;
    return unit * kAmplitude * 0.5f;
}

}  // namespace

float heightAt(float x, float z) {
    // A small smoothing kernel over the raw field. The finest octave carries
    // detail the 2 metre mesh can't represent, and unrepresentable detail shows
    // up as faceting and as shadow acne on slopes. Averaging at the mesh's own
    // scale removes exactly the part the triangles can't express.
    //
    // This lives inside heightAt so the server stands characters on precisely
    // the surface the client draws -- smoothing only one of them would put
    // players slightly through the ground.
    constexpr float kSmoothRadius = kWorldSize / static_cast<float>(kMeshResolution) * 0.5f;

    const float centre = rawHeight(x, z);
    const float left = rawHeight(x - kSmoothRadius, z);
    const float right = rawHeight(x + kSmoothRadius, z);
    const float back = rawHeight(x, z - kSmoothRadius);
    const float front = rawHeight(x, z + kSmoothRadius);

    // Centre-weighted so hills keep their shape rather than flattening.
    return centre * 0.5f + (left + right + back + front) * 0.125f;
}

void normalAt(float x, float z, float* outX, float* outY, float* outZ) {
    // Central differences. The step matches the mesh spacing so the collision
    // normal agrees with the shading normal.
    constexpr float kStep = kWorldSize / static_cast<float>(kMeshResolution);
    const float dx = heightAt(x + kStep, z) - heightAt(x - kStep, z);
    const float dz = heightAt(x, z + kStep) - heightAt(x, z - kStep);

    // Cross product of the two tangents, normalised.
    float nx = -dx;
    float ny = 2.0f * kStep;
    float nz = -dz;
    const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
    *outX = nx / length;
    *outY = ny / length;
    *outZ = nz / length;
}

}  // namespace terrain
