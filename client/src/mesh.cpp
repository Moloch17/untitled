#include "mesh.h"

#include <cmath>
#include <vector>

#include <geometry/SurfaceOrientation.h>
#include <math/mat3.h>
#include <math/scalar.h>
#include <math/quat.h>

using namespace filament;
using namespace filament::math;

namespace game {

namespace {

// Interleaved layout handed to the GPU: position, then the tangent frame as a
// quaternion (Filament derives the normal from it rather than taking one).
struct Vertex {
    float3 position;
    quatf tangents;
};

// Builds the buffers from parallel position/normal arrays. Normals are only
// used to derive the tangent frames; they aren't uploaded directly.
Mesh build(Engine* engine,
        const std::vector<float3>& positions,
        const std::vector<float3>& normals,
        const std::vector<uint16_t>& indices) {
    const size_t vertexCount = positions.size();

    std::vector<quatf> quats(vertexCount);
    auto* orientation = geometry::SurfaceOrientation::Builder()
            .vertexCount(vertexCount)
            .normals(normals.data())
            .build();
    orientation->getQuats(quats.data(), vertexCount);
    delete orientation;

    std::vector<Vertex> vertices(vertexCount);
    float3 minCorner = positions[0];
    float3 maxCorner = positions[0];
    for (size_t i = 0; i < vertexCount; ++i) {
        vertices[i] = {positions[i], quats[i]};
        minCorner = min(minCorner, positions[i]);
        maxCorner = max(maxCorner, positions[i]);
    }

    Mesh mesh;
    mesh.vertexBuffer = VertexBuffer::Builder()
            .vertexCount(static_cast<uint32_t>(vertexCount))
            .bufferCount(1)
            .attribute(VertexAttribute::POSITION, 0, VertexBuffer::AttributeType::FLOAT3,
                    offsetof(Vertex, position), sizeof(Vertex))
            .attribute(VertexAttribute::TANGENTS, 0, VertexBuffer::AttributeType::FLOAT4,
                    offsetof(Vertex, tangents), sizeof(Vertex))
            .build(*engine);

    // The staging copies must outlive the upload, so hand ownership of a heap
    // copy to Filament and free it from the completion callback.
    auto* vertexData = new std::vector<Vertex>(std::move(vertices));
    mesh.vertexBuffer->setBufferAt(*engine, 0,
            VertexBuffer::BufferDescriptor(vertexData->data(), vertexData->size() * sizeof(Vertex),
                    [](void*, size_t, void* user) {
                        delete static_cast<std::vector<Vertex>*>(user);
                    },
                    vertexData));

    mesh.indexBuffer = IndexBuffer::Builder()
            .indexCount(static_cast<uint32_t>(indices.size()))
            .bufferType(IndexBuffer::IndexType::USHORT)
            .build(*engine);

    auto* indexData = new std::vector<uint16_t>(indices);
    mesh.indexBuffer->setBuffer(*engine,
            IndexBuffer::BufferDescriptor(indexData->data(), indexData->size() * sizeof(uint16_t),
                    [](void*, size_t, void* user) {
                        delete static_cast<std::vector<uint16_t>*>(user);
                    },
                    indexData));

    mesh.boundingBox = Box().set(minCorner, maxCorner);
    return mesh;
}

}  // namespace

void Mesh::destroy(Engine* engine) {
    engine->destroy(vertexBuffer);
    engine->destroy(indexBuffer);
    vertexBuffer = nullptr;
    indexBuffer = nullptr;
}

Mesh createCube(Engine* engine, float size) {
    const float h = size * 0.5f;

    // Faces don't share vertices: each corner needs a different normal per face
    // to keep the edges hard.
    const float3 faceNormals[6] = {
        {0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0},
    };
    const float3 faceCorners[6][4] = {
        {{-h, -h, h}, {h, -h, h}, {h, h, h}, {-h, h, h}},        // +Z
        {{h, -h, -h}, {-h, -h, -h}, {-h, h, -h}, {h, h, -h}},    // -Z
        {{h, -h, h}, {h, -h, -h}, {h, h, -h}, {h, h, h}},        // +X
        {{-h, -h, -h}, {-h, -h, h}, {-h, h, h}, {-h, h, -h}},    // -X
        {{-h, h, h}, {h, h, h}, {h, h, -h}, {-h, h, -h}},        // +Y
        {{-h, -h, -h}, {h, -h, -h}, {h, -h, h}, {-h, -h, h}},    // -Y
    };

    std::vector<float3> positions;
    std::vector<float3> normals;
    std::vector<uint16_t> indices;
    for (uint16_t face = 0; face < 6; ++face) {
        for (int corner = 0; corner < 4; ++corner) {
            positions.push_back(faceCorners[face][corner]);
            normals.push_back(faceNormals[face]);
        }
        const uint16_t base = face * 4;
        for (uint16_t offset : {0, 1, 2, 2, 3, 0}) {
            indices.push_back(base + offset);
        }
    }

    return build(engine, positions, normals, indices);
}

Mesh createCapsule(Engine* engine, float radius, float halfHeight, int segments, int rings) {
    std::vector<float3> positions;
    std::vector<float3> normals;
    std::vector<uint16_t> indices;

    // Built as a grid of rings from the top pole to the bottom. The two
    // hemispheres share the cylinder's rings, so the surface is continuous and
    // the normals stay smooth across the seam.
    const int totalRings = rings * 2 + 2;
    for (int ring = 0; ring < totalRings; ++ring) {
        // Latitude runs from +pi/2 (top pole) to -pi/2 (bottom pole). The
        // middle two rings sit at the equator and are separated only by the
        // cylinder's height, which is what turns the sphere into a capsule.
        const bool topHalf = ring <= rings;
        const int ringInHalf = topHalf ? ring : ring - rings - 1;
        const float t = static_cast<float>(ringInHalf) / static_cast<float>(rings);
        const float latitude = topHalf ? (F_PI * 0.5f) * (1.0f - t) : -(F_PI * 0.5f) * t;

        const float ringRadius = std::cos(latitude) * radius;
        const float ringY = std::sin(latitude) * radius + (topHalf ? halfHeight : -halfHeight);

        for (int segment = 0; segment <= segments; ++segment) {
            const float longitude = 2.0f * F_PI * static_cast<float>(segment)
                    / static_cast<float>(segments);
            const float x = std::cos(longitude);
            const float z = std::sin(longitude);

            positions.push_back({x * ringRadius, ringY, z * ringRadius});
            // The normal is the hemisphere's, which on the cylinder section is
            // simply horizontal.
            normals.push_back(normalize(float3{x * std::cos(latitude), std::sin(latitude),
                    z * std::cos(latitude)}));
        }
    }

    const int columns = segments + 1;
    for (int ring = 0; ring + 1 < totalRings; ++ring) {
        for (int segment = 0; segment < segments; ++segment) {
            const uint16_t a = static_cast<uint16_t>(ring * columns + segment);
            const uint16_t b = static_cast<uint16_t>(a + 1);
            const uint16_t c = static_cast<uint16_t>((ring + 1) * columns + segment);
            const uint16_t d = static_cast<uint16_t>(c + 1);

            indices.push_back(a);
            indices.push_back(c);
            indices.push_back(b);
            indices.push_back(b);
            indices.push_back(c);
            indices.push_back(d);
        }
    }

    return build(engine, positions, normals, indices);
}

Mesh createPlane(Engine* engine, float size) {
    const float h = size * 0.5f;
    const std::vector<float3> positions = {
        {-h, 0, -h}, {-h, 0, h}, {h, 0, h}, {h, 0, -h},
    };
    const std::vector<float3> normals(4, float3{0, 1, 0});
    const std::vector<uint16_t> indices = {0, 1, 2, 2, 3, 0};

    return build(engine, positions, normals, indices);
}

}  // namespace game
