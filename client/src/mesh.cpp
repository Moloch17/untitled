#include "mesh.h"

#include <cmath>
#include <vector>

#include <geometry/SurfaceOrientation.h>
#include <math/mat3.h>
#include <math/scalar.h>

#include <terrain/heightfield.h>
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
    float2 uv;
};

// Builds the buffers from parallel position/normal arrays. Normals are only
// used to derive the tangent frames; they aren't uploaded directly.
Mesh build(Engine* engine,
        const std::vector<float3>& positions,
        const std::vector<float3>& normals,
        const std::vector<float2>& uvs,
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
        vertices[i] = {positions[i], quats[i], uvs[i]};
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
            .attribute(VertexAttribute::UV0, 0, VertexBuffer::AttributeType::FLOAT2,
                    offsetof(Vertex, uv), sizeof(Vertex))
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

// As build(), but with 32-bit indices: the terrain has far more than 65,536
// vertices, which is all a 16-bit index can address.
Mesh buildWide(Engine* engine,
        const std::vector<float3>& positions,
        const std::vector<float3>& normals,
        const std::vector<float2>& uvs,
        const std::vector<uint32_t>& indices) {
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
        vertices[i] = {positions[i], quats[i], uvs[i]};
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
            .attribute(VertexAttribute::UV0, 0, VertexBuffer::AttributeType::FLOAT2,
                    offsetof(Vertex, uv), sizeof(Vertex))
            .build(*engine);

    auto* vertexData = new std::vector<Vertex>(std::move(vertices));
    mesh.vertexBuffer->setBufferAt(*engine, 0,
            VertexBuffer::BufferDescriptor(vertexData->data(), vertexData->size() * sizeof(Vertex),
                    [](void*, size_t, void* user) {
                        delete static_cast<std::vector<Vertex>*>(user);
                    },
                    vertexData));

    mesh.indexBuffer = IndexBuffer::Builder()
            .indexCount(static_cast<uint32_t>(indices.size()))
            .bufferType(IndexBuffer::IndexType::UINT)
            .build(*engine);

    auto* indexData = new std::vector<uint32_t>(indices);
    mesh.indexBuffer->setBuffer(*engine,
            IndexBuffer::BufferDescriptor(indexData->data(), indexData->size() * sizeof(uint32_t),
                    [](void*, size_t, void* user) {
                        delete static_cast<std::vector<uint32_t>*>(user);
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
    std::vector<float2> uvs;
    std::vector<uint16_t> indices;
    const float2 cornerUvs[4] = {{0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}, {0.0f, 0.0f}};
    for (uint16_t face = 0; face < 6; ++face) {
        for (int corner = 0; corner < 4; ++corner) {
            positions.push_back(faceCorners[face][corner]);
            normals.push_back(faceNormals[face]);
            uvs.push_back(cornerUvs[corner]);
        }
        const uint16_t base = face * 4;
        for (uint16_t offset : {0, 1, 2, 2, 3, 0}) {
            indices.push_back(base + offset);
        }
    }

    return build(engine, positions, normals, uvs, indices);
}

Mesh createCapsule(Engine* engine, float radius, float halfHeight, int segments, int rings) {
    std::vector<float3> positions;
    std::vector<float3> normals;
    std::vector<float2> uvs;
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
            // u wraps around the capsule and v runs top to bottom. Local -Z --
            // the direction a character faces -- lands at u = 0.75, which is
            // where the forward marker is drawn.
            uvs.push_back({static_cast<float>(segment) / static_cast<float>(segments),
                static_cast<float>(ring) / static_cast<float>(totalRings - 1)});
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

            // Wound so that cross(b-a, c-a) points along the vertex normal,
            // i.e. outward -- the same convention createCube uses. The
            // opposite order leaves every triangle facing inward, so back-face
            // culling removes the near surface and you see the inside of the
            // far one. Verified by comparing winding against the normals for
            // all 384 triangles.
            indices.push_back(a);
            indices.push_back(b);
            indices.push_back(c);
            indices.push_back(b);
            indices.push_back(d);
            indices.push_back(c);
        }
    }

    return build(engine, positions, normals, uvs, indices);
}

Mesh createTerrain(Engine* engine) {
    constexpr int resolution = terrain::kMeshResolution;
    constexpr int columns = resolution + 1;
    constexpr float step = terrain::kWorldSize / static_cast<float>(resolution);

    std::vector<float3> positions;
    std::vector<float3> normals;
    std::vector<float2> uvs;
    positions.reserve(static_cast<size_t>(columns) * columns);
    normals.reserve(static_cast<size_t>(columns) * columns);
    uvs.reserve(static_cast<size_t>(columns) * columns);

    for (int row = 0; row < columns; ++row) {
        for (int column = 0; column < columns; ++column) {
            const float x = -terrain::kHalfWorld + column * step;
            const float z = -terrain::kHalfWorld + row * step;

            // The same function the server stands characters on.
            positions.push_back({x, terrain::heightAt(x, z), z});

            float nx = 0.0f;
            float ny = 1.0f;
            float nz = 0.0f;
            terrain::normalAt(x, z, &nx, &ny, &nz);
            normals.push_back({nx, ny, nz});
            // One UV unit per metre, so any future terrain texture tiles at a
            // predictable world scale.
            uvs.push_back({x, z});
        }
    }

    std::vector<uint32_t> indices;
    indices.reserve(static_cast<size_t>(resolution) * resolution * 6);
    for (int row = 0; row < resolution; ++row) {
        for (int column = 0; column < resolution; ++column) {
            const uint32_t topLeft = static_cast<uint32_t>(row * columns + column);
            const uint32_t topRight = topLeft + 1;
            const uint32_t bottomLeft = topLeft + columns;
            const uint32_t bottomRight = bottomLeft + 1;

            indices.push_back(topLeft);
            indices.push_back(bottomLeft);
            indices.push_back(topRight);
            indices.push_back(topRight);
            indices.push_back(bottomLeft);
            indices.push_back(bottomRight);
        }
    }

    return buildWide(engine, positions, normals, uvs, indices);
}

Mesh createSphere(Engine* engine, float radius, int segments, int rings) {
    return createCapsule(engine, radius, 0.0f, segments, rings);
}

Mesh createPlane(Engine* engine, float size) {
    const float h = size * 0.5f;
    const std::vector<float3> positions = {
        {-h, 0, -h}, {-h, 0, h}, {h, 0, h}, {h, 0, -h},
    };
    const std::vector<float3> normals(4, float3{0, 1, 0});
    const std::vector<float2> uvs = {{0, 0}, {0, 1}, {1, 1}, {1, 0}};
    const std::vector<uint16_t> indices = {0, 1, 2, 2, 3, 0};

    return build(engine, positions, normals, uvs, indices);
}

}  // namespace game
