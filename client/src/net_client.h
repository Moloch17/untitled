#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <net/framing.h>
#include <net/protocol.h>
#include <net/snapshot.h>
#include <net/socket.h>

namespace game {

// Client-side networking: logs in against the auth server, joins the world with
// the returned token, then receives the UDP snapshot stream.
//
// Every step is non-blocking and driven by poll(), which is called once a
// frame. Nothing here may ever wait on a socket -- a stalled frame is a dropped
// frame.
class NetClient {
public:
    enum class State {
        Idle,
        ConnectingAuth,
        AwaitingLogin,
        ConnectingWorld,
        AwaitingJoin,
        InWorld,
        Failed,
    };

    // A replicated entity as last reported, plus when it arrived, so the caller
    // can extrapolate forward from it.
    struct Replicated {
        net::EntityState state;
        double receivedAt = 0.0;
    };

    bool init();
    void shutdown();

    // Starts the login sequence. Returns immediately.
    void login(const std::string& host, uint16_t port, const std::string& username,
            const std::string& password);

    // Advances the state machine. `now` is a monotonically increasing time in
    // seconds, used to timestamp snapshots.
    void poll(double now);

    // Sends this frame's movement intent. Cheap enough to call every frame:
    // the server uses the newest one it has at each tick.
    void sendInput(float moveForward, float moveRight, float yaw, bool jump, bool sprint,
            uint32_t sequence);

    // Tells the world server we're leaving, then drops every connection.
    void logout();
    // Drops connections without notifying, for shutdown paths.
    void disconnect();

    State state() const { return mState; }
    const std::string& error() const { return mError; }
    const std::string& username() const { return mUsername; }
    uint64_t playerId() const { return mPlayerId; }
    // The replicated entity that is this client's own character.
    uint32_t ownEntityId() const { return mOwnEntityId; }

    const std::map<uint32_t, Replicated>& entities() const { return mEntities; }
    // Seconds since the newest snapshot arrived; the extrapolation window.
    double timeSinceSnapshot(double now) const { return now - mLastSnapshotAt; }
    uint32_t snapshotsReceived() const { return mSnapshotsReceived; }
    // Server-owned world clock from the newest snapshot, in [0,1). At 60
    // snapshots a second this is smooth enough to drive the sky directly, so
    // the client keeps no clock of its own to drift out of step.
    float timeOfDay() const { return mTimeOfDay; }
    // Simulated one-way latency in seconds, from NET_FAKE_LATENCY_MS. Zero
    // disables the whole mechanism.
    double fakeLatency() const { return mFakeLatency; }

private:
    void fail(const std::string& message);
    void pumpAuth();
    void pumpWorld(double now);
    void sendUdpHello();
    void flushDelayed(double now);
    void applySnapshotPacket(const uint8_t* data, size_t size, uint32_t sequence, double now);

    State mState = State::Idle;
    std::string mError;

    std::unique_ptr<net::MessageStream> mAuth;
    std::unique_ptr<net::MessageStream> mWorld;
    net::TcpSocket mPendingAuthSocket;
    net::TcpSocket mPendingWorldSocket;
    net::UdpSocket mUdp;
    net::Address mUdpTarget;

    std::string mUsername;
    std::string mPassword;
    std::string mToken;
    std::string mWorldHost;
    uint16_t mWorldPort = 0;
    uint64_t mPlayerId = 0;
    uint32_t mOwnEntityId = 0;
    uint32_t mInputSequence = 1;

    // Packets held back to simulate latency, so prediction and reconciliation
    // can be exercised on a machine where the server is a millisecond away.
    struct DelayedPacket {
        double dueAt = 0.0;
        std::vector<uint8_t> data;
    };
    std::deque<DelayedPacket> mDelayedOutgoing;
    std::deque<DelayedPacket> mDelayedIncoming;
    double mFakeLatency = 0.0;
    // Fraction of outgoing input packets to drop, from NET_FAKE_LOSS_PERCENT.
    double mFakeLoss = 0.0;
    double mLastPollTime = 0.0;

    std::map<uint32_t, Replicated> mEntities;
    float mTimeOfDay = 0.0f;
    double mLastSnapshotAt = 0.0;
    double mLastHelloAt = 0.0;
    uint32_t mLastSnapshotSequence = 0;
    uint32_t mSnapshotsReceived = 0;
};

}  // namespace game
