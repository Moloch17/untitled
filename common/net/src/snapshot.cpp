#include "net/snapshot.h"

#include "net/byte_stream.h"
#include "net/protocol.h"

namespace net {

void encodeSnapshot(const Snapshot& snapshot, std::vector<uint8_t>& out) {
    ByteWriter writer(out);
    writer.u32(snapshot.tick);
    writer.u64(snapshot.serverTimeMs);
    writer.u16(static_cast<uint16_t>(snapshot.entities.size()));

    for (const EntityState& entity : snapshot.entities) {
        writer.u32(entity.id);
        writer.u8(entity.type);
        writer.f32(entity.position.x);
        writer.f32(entity.position.y);
        writer.f32(entity.position.z);
        writer.f32(entity.rotation.x);
        writer.f32(entity.rotation.y);
        writer.f32(entity.rotation.z);
        writer.f32(entity.rotation.w);
        writer.f32(entity.velocity.x);
        writer.f32(entity.velocity.y);
        writer.f32(entity.velocity.z);
        writer.f32(entity.angularVelocity.x);
        writer.f32(entity.angularVelocity.y);
        writer.f32(entity.angularVelocity.z);
        writer.u32(entity.lastInputSequence);
    }
}

bool decodeSnapshot(const uint8_t* data, size_t size, Snapshot* out) {
    ByteReader reader(data, size);
    out->tick = reader.u32();
    out->serverTimeMs = reader.u64();
    const uint16_t count = reader.u16();

    if (reader.failed() || count > kMaxEntitiesPerSnapshot) {
        return false;
    }

    out->entities.clear();
    out->entities.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        EntityState entity;
        entity.id = reader.u32();
        entity.type = reader.u8();
        entity.position.x = reader.f32();
        entity.position.y = reader.f32();
        entity.position.z = reader.f32();
        entity.rotation.x = reader.f32();
        entity.rotation.y = reader.f32();
        entity.rotation.z = reader.f32();
        entity.rotation.w = reader.f32();
        entity.velocity.x = reader.f32();
        entity.velocity.y = reader.f32();
        entity.velocity.z = reader.f32();
        entity.angularVelocity.x = reader.f32();
        entity.angularVelocity.y = reader.f32();
        entity.angularVelocity.z = reader.f32();
        entity.lastInputSequence = reader.u32();
        if (reader.failed()) {
            return false;  // truncated: discard the whole snapshot
        }
        out->entities.push_back(entity);
    }
    return true;
}

void writeUdpHeader(std::vector<uint8_t>& out, uint16_t type, uint32_t sequence) {
    ByteWriter writer(out);
    writer.u32(kUdpMagic);
    writer.u16(type);
    writer.u32(sequence);
}

bool readUdpHeader(const uint8_t* data, size_t size, UdpHeader* out, size_t* bodyOffset) {
    ByteReader reader(data, size);
    if (reader.u32() != kUdpMagic) {
        return false;
    }
    out->type = reader.u16();
    out->sequence = reader.u32();
    if (reader.failed()) {
        return false;
    }
    *bodyOffset = sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint32_t);
    return true;
}

}  // namespace net
