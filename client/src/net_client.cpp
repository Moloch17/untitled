#include "net_client.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

#include <net/byte_stream.h>

using namespace net;

namespace game {

namespace {

// The UDP hello is repeated until snapshots arrive: it is a single datagram and
// may simply be lost.
constexpr double kHelloRetrySeconds = 0.25;

}  // namespace

bool NetClient::init() {
    if (const char* value = std::getenv("NET_FAKE_LATENCY_MS")) {
        mFakeLatency = std::atof(value) / 1000.0;
    }
    if (const char* value = std::getenv("NET_FAKE_LOSS_PERCENT")) {
        mFakeLoss = std::atof(value) / 100.0;
    }
    return initSockets();
}

void NetClient::shutdown() {
    disconnect();
    shutdownSockets();
}

void NetClient::fail(const std::string& message) {
    mError = message;
    mState = State::Failed;
    mAuth.reset();
    mWorld.reset();
    mPendingAuthSocket.close();
    mPendingWorldSocket.close();
    mUdp.close();
}

void NetClient::login(const std::string& host, uint16_t port, const std::string& username,
        const std::string& password) {
    disconnect();
    mError.clear();
    mUsername = username;
    mPassword = password;
    mEntities.clear();
    mSnapshotsReceived = 0;

    Address address;
    if (!Address::resolve(host, port, &address)) {
        fail("cannot resolve " + host);
        return;
    }
    if (!mPendingAuthSocket.connect(address)) {
        fail("cannot reach auth server");
        return;
    }
    mState = State::ConnectingAuth;
}

void NetClient::poll(double now) {
    mLastPollTime = now;
    std::vector<bool> ready;

    switch (mState) {
        case State::ConnectingAuth: {
            // A non-blocking connect completes by becoming writable. Zero
            // timeout: this is a poll, not a wait.
            std::vector<SocketHandle> handles{mPendingAuthSocket.handle()};
            if (waitWritable(handles, 0, &ready) > 0 && ready[0]) {
                if (!mPendingAuthSocket.connectResult()) {
                    fail("cannot reach auth server");
                    return;
                }
                mAuth = std::make_unique<MessageStream>(std::move(mPendingAuthSocket));

                std::vector<uint8_t> payload;
                ByteWriter writer(payload);
                writer.u32(kProtocolVersion);
                writer.string(mUsername);
                writer.string(mPassword);
                mAuth->send(static_cast<uint16_t>(AuthMessage::LoginRequest), payload);

                // The password is not needed again; don't keep it in memory.
                mPassword.clear();
                mState = State::AwaitingLogin;
            }
            break;
        }

        case State::AwaitingLogin:
            pumpAuth();
            break;

        case State::ConnectingWorld: {
            std::vector<SocketHandle> handles{mPendingWorldSocket.handle()};
            if (waitWritable(handles, 0, &ready) > 0 && ready[0]) {
                if (!mPendingWorldSocket.connectResult()) {
                    fail("cannot reach world server");
                    return;
                }
                mWorld = std::make_unique<MessageStream>(std::move(mPendingWorldSocket));

                std::vector<uint8_t> payload;
                ByteWriter writer(payload);
                writer.u32(kProtocolVersion);
                writer.string(mToken);
                mWorld->send(static_cast<uint16_t>(WorldMessage::JoinRequest), payload);
                mState = State::AwaitingJoin;
            }
            break;
        }

        case State::AwaitingJoin:
        case State::InWorld:
            pumpWorld(now);
            break;

        default:
            break;
    }
}

void NetClient::pumpAuth() {
    if (!mAuth || !mAuth->pump()) {
        fail("auth server closed the connection");
        return;
    }

    MessageStream::Message message;
    while (mAuth->next(&message)) {
        if (message.type != static_cast<uint16_t>(AuthMessage::LoginResponse)) {
            continue;
        }

        ByteReader reader(message.payload.data(), message.payload.size());
        const auto result = static_cast<AuthResult>(reader.u8());
        mToken = reader.string();
        mWorldHost = reader.string();
        mWorldPort = reader.u16();
        reader.u64();  // account id

        if (reader.failed()) {
            fail("malformed login response");
            return;
        }
        if (result != AuthResult::Success) {
            fail(toString(result));
            return;
        }

        // Auth is done; the token is all the world server needs.
        mAuth.reset();

        Address address;
        if (!Address::resolve(mWorldHost, mWorldPort, &address)) {
            fail("cannot resolve world server");
            return;
        }
        if (!mPendingWorldSocket.connect(address)) {
            fail("cannot reach world server");
            return;
        }
        mState = State::ConnectingWorld;
        return;
    }
}

void NetClient::sendUdpHello() {
    std::vector<uint8_t> packet;
    writeUdpHeader(packet, static_cast<uint16_t>(WorldMessage::ClientHello), 0);
    ByteWriter writer(packet);
    writer.u64(mPlayerId);
    writer.string(mToken);
    mUdp.sendTo(packet.data(), packet.size(), mUdpTarget);
}

void NetClient::pumpWorld(double now) {
    if (!mWorld || !mWorld->pump()) {
        fail("world server closed the connection");
        return;
    }

    MessageStream::Message message;
    while (mWorld->next(&message)) {
        switch (static_cast<WorldMessage>(message.type)) {
            case WorldMessage::JoinResponse: {
                ByteReader reader(message.payload.data(), message.payload.size());
                const auto result = static_cast<JoinResult>(reader.u8());
                mPlayerId = reader.u64();
                const uint16_t udpPort = reader.u16();
                mUsername = reader.string();
                mOwnEntityId = reader.u32();

                if (reader.failed()) {
                    fail("malformed join response");
                    return;
                }
                if (result != JoinResult::Success) {
                    fail(result == JoinResult::InvalidSession ? "session rejected"
                                                              : "could not join the world");
                    return;
                }

                // Ephemeral port: the server learns our address from the hello.
                if (!mUdp.open(0)) {
                    fail("cannot open a UDP socket");
                    return;
                }
                if (!Address::resolve(mWorldHost, udpPort, &mUdpTarget)) {
                    fail("cannot resolve the world server's UDP address");
                    return;
                }

                mState = State::InWorld;
                sendUdpHello();
                mLastHelloAt = now;
                mLastSnapshotAt = now;
                break;
            }

            case WorldMessage::Disconnect:
                fail("disconnected by the server");
                return;

            default:
                break;
        }
    }

    if (mState != State::InWorld) {
        return;
    }

    // Keep announcing ourselves until the stream starts; a lost hello would
    // otherwise leave us connected but never receiving anything.
    if (mSnapshotsReceived == 0 && now - mLastHelloAt > kHelloRetrySeconds) {
        sendUdpHello();
        mLastHelloAt = now;
    }

    uint8_t buffer[2048];
    for (;;) {
        size_t size = 0;
        Address from;
        if (!mUdp.receiveFrom(buffer, sizeof buffer, &size, &from) || size == 0) {
            break;
        }

        UdpHeader header;
        size_t bodyOffset = 0;
        if (!readUdpHeader(buffer, size, &header, &bodyOffset)) {
            continue;
        }
        if (static_cast<WorldMessage>(header.type) != WorldMessage::Snapshot) {
            continue;
        }
        // UDP may deliver out of order; an older snapshot is worse than none.
        if (mSnapshotsReceived > 0 && header.sequence <= mLastSnapshotSequence) {
            continue;
        }

        if (mFakeLatency > 0.0) {
            mDelayedIncoming.push_back({now + mFakeLatency,
                std::vector<uint8_t>(buffer + bodyOffset, buffer + size)});
            mLastSnapshotSequence = header.sequence;
            continue;
        }

        applySnapshotPacket(buffer + bodyOffset, size - bodyOffset, header.sequence, now);
    }

    flushDelayed(now);
}

void NetClient::applySnapshotPacket(const uint8_t* data, size_t size, uint32_t sequence,
        double now) {
    Snapshot snapshot;
    if (!decodeSnapshot(data, size, &snapshot)) {
        return;
    }

    mTimeOfDay = snapshot.timeOfDay;

    for (const EntityState& entity : snapshot.entities) {
        Replicated& replicated = mEntities[entity.id];
        replicated.state = entity;
        replicated.receivedAt = now;
    }

    mLastSnapshotSequence = sequence;
    mLastSnapshotAt = now;
    ++mSnapshotsReceived;
}

void NetClient::flushDelayed(double now) {
    while (!mDelayedOutgoing.empty() && mDelayedOutgoing.front().dueAt <= now) {
        const auto& packet = mDelayedOutgoing.front();
        mUdp.sendTo(packet.data.data(), packet.data.size(), mUdpTarget);
        mDelayedOutgoing.pop_front();
    }
    while (!mDelayedIncoming.empty() && mDelayedIncoming.front().dueAt <= now) {
        const auto& packet = mDelayedIncoming.front();
        applySnapshotPacket(packet.data.data(), packet.data.size(), mLastSnapshotSequence, now);
        mDelayedIncoming.pop_front();
    }
}

void NetClient::sendInput(float moveForward, float moveRight, float yaw, bool jump, bool sprint,
        uint32_t sequence) {
    if (mState != State::InWorld || !mUdp.valid()) {
        return;
    }

    uint8_t buttons = 0;
    if (jump) {
        buttons |= kInputJump;
    }
    if (sprint) {
        buttons |= kInputSprint;
    }

    std::vector<uint8_t> packet;
    writeUdpHeader(packet, static_cast<uint16_t>(WorldMessage::ClientInput), sequence);
    ByteWriter writer(packet);
    writer.u64(mPlayerId);
    // The sequence comes from the prediction layer: it is what the server
    // echoes back so the client knows what has been accounted for.
    writer.u32(sequence);
    writer.f32(moveForward);
    writer.f32(moveRight);
    writer.f32(yaw);
    writer.u8(buttons);
    // Simulated loss. A dropped input is a real divergence: the server never
    // applies it, so the client's prediction is genuinely wrong and has to be
    // rolled back.
    if (mFakeLoss > 0.0 && (std::rand() / static_cast<double>(RAND_MAX)) < mFakeLoss) {
        return;
    }

    if (mFakeLatency > 0.0) {
        mDelayedOutgoing.push_back({mLastPollTime + mFakeLatency, std::move(packet)});
        return;
    }
    // Fire and forget: a lost input packet is replaced by the next one 16ms
    // later, which is cheaper than retransmitting a stale intent.
    mUdp.sendTo(packet.data(), packet.size(), mUdpTarget);
}

void NetClient::logout() {
    if (mWorld && mState == State::InWorld) {
        std::vector<uint8_t> empty;
        mWorld->send(static_cast<uint16_t>(WorldMessage::LogoutRequest), empty);
        // Best-effort flush: the socket is about to close either way, and the
        // server also treats a dropped connection as a logout.
        mWorld->flush();
    }
    disconnect();
}

void NetClient::disconnect() {
    mAuth.reset();
    mWorld.reset();
    mPendingAuthSocket.close();
    mPendingWorldSocket.close();
    mUdp.close();
    mEntities.clear();
    mDelayedOutgoing.clear();
    mDelayedIncoming.clear();
    mToken.clear();
    mPassword.clear();
    mPlayerId = 0;
    mOwnEntityId = 0;
    mInputSequence = 1;
    mSnapshotsReceived = 0;
    mLastSnapshotSequence = 0;
    mState = State::Idle;
}

}  // namespace game
