#pragma once

#include <cstdint>
#include <vector>

namespace net {

// Plain transform types so the servers don't have to depend on the client's
// math library. The client converts these to Filament's types on arrival.
struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Quat {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

// One replicated entity. Velocities travel with the transform so the client can
// extrapolate forward from the newest snapshot instead of lagging behind to
// interpolate between two old ones.
struct EntityState {
    uint32_t id = 0;
    // What the client should render. See net::EntityType.
    uint8_t type = 0;
    Vec3 position;
    Quat rotation;
    Vec3 velocity;         // units/second
    Vec3 angularVelocity;  // radians/second, per axis
    // For player entities: the last input sequence the server applied. Zero for
    // anything the client doesn't control.
    uint32_t lastInputSequence = 0;
};

struct Snapshot {
    uint32_t tick = 0;
    uint64_t serverTimeMs = 0;
    std::vector<EntityState> entities;
};

// Encoding and decoding live together so the two ends can't drift apart: a
// change to one is a compile error in the other.
void encodeSnapshot(const Snapshot& snapshot, std::vector<uint8_t>& out);
bool decodeSnapshot(const uint8_t* data, size_t size, Snapshot* out);

// Every UDP datagram starts with [magic:u32][type:u16][sequence:u32]. The magic
// discards stray traffic before parsing; the sequence lets the receiver drop
// datagrams that arrive out of order, which UDP allows.
struct UdpHeader {
    uint16_t type = 0;
    uint32_t sequence = 0;
};

void writeUdpHeader(std::vector<uint8_t>& out, uint16_t type, uint32_t sequence);
bool readUdpHeader(const uint8_t* data, size_t size, UdpHeader* out, size_t* bodyOffset);

}  // namespace net
