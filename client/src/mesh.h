#pragma once

#include <filament/Box.h>
#include <filament/Engine.h>
#include <filament/IndexBuffer.h>
#include <filament/VertexBuffer.h>
#include <math/vec3.h>

namespace game {

// A vertex/index buffer pair plus the bounds Filament needs for culling. The
// buffers are owned by the Engine; call destroy() before tearing it down.
struct Mesh {
    filament::VertexBuffer* vertexBuffer = nullptr;
    filament::IndexBuffer* indexBuffer = nullptr;
    filament::Box boundingBox;

    void destroy(filament::Engine* engine);
};

// Axis-aligned cube of the given edge length, centred on the origin.
Mesh createCube(filament::Engine* engine, float size);

// Flat plane in the XZ axis facing +Y, centred on the origin.
Mesh createPlane(filament::Engine* engine, float size);

// Sphere, for the sun and moon: a disc from any angle without billboarding.
Mesh createSphere(filament::Engine* engine, float radius, int segments = 24, int rings = 12);

// Capsule aligned to the Y axis, centred on the origin: a cylinder of
// 2*halfHeight capped with hemispheres. Matches the server's player collider.
Mesh createCapsule(filament::Engine* engine, float radius, float halfHeight, int segments = 16,
        int rings = 6);

}  // namespace game
