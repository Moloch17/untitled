#pragma once

#include <cstdint>
#include <vector>

#include "net/socket.h"

namespace net {

// TCP is a byte stream with no message boundaries, so each message is prefixed
// with its length: [u32 payloadSize][u16 type][payload].
//
// MessageStream buffers partial reads until a whole message has arrived and
// queues writes that the socket couldn't accept immediately.
class MessageStream {
public:
    struct Message {
        uint16_t type = 0;
        std::vector<uint8_t> payload;
    };

    explicit MessageStream(TcpSocket socket) : mSocket(std::move(socket)) {}

    // Drains the socket into the receive buffer. Returns false when the peer
    // disconnected or sent something malformed.
    bool pump();

    // Pops one complete message, if any is buffered.
    bool next(Message* out);

    // Queues a message and tries to flush immediately.
    void send(uint16_t type, const std::vector<uint8_t>& payload);

    // Writes as much queued data as the socket accepts.
    bool flush();

    bool hasPendingWrites() const { return mWriteOffset < mWriteBuffer.size(); }
    TcpSocket& socket() { return mSocket; }

    // Guards against a peer claiming an absurd payload size.
    static constexpr uint32_t kMaxPayloadSize = 1 << 20;

private:
    TcpSocket mSocket;
    std::vector<uint8_t> mReadBuffer;
    std::vector<uint8_t> mWriteBuffer;
    size_t mWriteOffset = 0;
};

}  // namespace net
