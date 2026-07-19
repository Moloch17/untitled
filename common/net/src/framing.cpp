#include "net/framing.h"

#include <cstring>

#include "net/byte_stream.h"

namespace net {

namespace {

constexpr size_t kHeaderSize = sizeof(uint32_t) + sizeof(uint16_t);

uint32_t readU32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8)
            | (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

uint16_t readU16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0] | (data[1] << 8));
}

}  // namespace

bool MessageStream::pump() {
    uint8_t chunk[4096];
    for (;;) {
        size_t received = 0;
        if (!mSocket.receive(chunk, sizeof chunk, &received)) {
            return false;
        }
        if (received == 0) {
            return true;  // nothing more available right now
        }
        mReadBuffer.insert(mReadBuffer.end(), chunk, chunk + received);

        // A peer that keeps sending without us consuming would grow this
        // unboundedly; next() drains it, but reject a single oversized claim.
        if (mReadBuffer.size() >= kHeaderSize
                && readU32(mReadBuffer.data()) > kMaxPayloadSize) {
            return false;
        }
    }
}

bool MessageStream::next(Message* out) {
    if (mReadBuffer.size() < kHeaderSize) {
        return false;
    }

    const uint32_t payloadSize = readU32(mReadBuffer.data());
    if (payloadSize > kMaxPayloadSize) {
        return false;
    }
    if (mReadBuffer.size() < kHeaderSize + payloadSize) {
        return false;  // still arriving
    }

    out->type = readU16(mReadBuffer.data() + sizeof(uint32_t));
    out->payload.assign(mReadBuffer.begin() + kHeaderSize,
            mReadBuffer.begin() + kHeaderSize + payloadSize);
    mReadBuffer.erase(mReadBuffer.begin(), mReadBuffer.begin() + kHeaderSize + payloadSize);
    return true;
}

void MessageStream::send(uint16_t type, const std::vector<uint8_t>& payload) {
    ByteWriter writer(mWriteBuffer);
    writer.u32(static_cast<uint32_t>(payload.size()));
    writer.u16(type);
    if (!payload.empty()) {
        writer.raw(payload.data(), payload.size());
    }
    flush();
}

bool MessageStream::flush() {
    while (mWriteOffset < mWriteBuffer.size()) {
        size_t sent = 0;
        if (!mSocket.send(mWriteBuffer.data() + mWriteOffset, mWriteBuffer.size() - mWriteOffset,
                    &sent)) {
            return false;
        }
        if (sent == 0) {
            return true;  // socket full; retry when it drains
        }
        mWriteOffset += sent;
    }

    mWriteBuffer.clear();
    mWriteOffset = 0;
    return true;
}

}  // namespace net
