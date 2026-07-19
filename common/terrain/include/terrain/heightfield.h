#pragma once

namespace terrain {

// Procedural terrain height, shared by the world server and the client.
//
// This has to be one implementation used by both. The server resolves where
// characters stand from it and the client builds the visible mesh from it; if
// the two disagreed by even a little, players would float or sink and
// prediction would fight a correction every tick.
//
// It is a pure function of position with a fixed seed -- no state, no assets to
// load, and identical results on every machine.

// Metres across. The mesh is built once at this size, so it is bounded by what
// is reasonable to upload rather than by the maths.
constexpr float kWorldSize = 1024.0f;
constexpr float kHalfWorld = kWorldSize * 0.5f;

// Quads per side of the visual mesh: 2 metre spacing.
constexpr int kMeshResolution = 512;

// Peak-to-trough height of the large features. Generous on purpose: gentle
// terrain reads as flat once atmospheric haze takes the distance.
constexpr float kAmplitude = 110.0f;

// Ground height at a world position.
float heightAt(float x, float z);

// Surface normal, from the slope of the height function.
void normalAt(float x, float z, float* outX, float* outY, float* outZ);

}  // namespace terrain
