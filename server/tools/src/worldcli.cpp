// End-to-end exerciser: logs in against the auth server, joins the world with
// the returned token, binds a UDP address and prints the snapshots that arrive.
//
//   worldcli <username> <password> [seconds]
//
// Used to verify replication without running the game client.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <net/byte_stream.h>
#include <net/framing.h>
#include <net/protocol.h>
#include <net/snapshot.h>
#include <net/socket.h>
#include <serverutil/config.h>

using namespace net;

namespace {

bool awaitMessage(MessageStream& stream, MessageStream::Message* out, int timeoutMs) {
    std::vector<SocketHandle> handles{stream.socket().handle()};
    std::vector<bool> ready;
    for (int elapsed = 0; elapsed < timeoutMs; elapsed += 50) {
        if (stream.next(out)) {
            return true;
        }
        waitReadable(handles, 50, &ready);
        if (!stream.pump()) {
            return false;
        }
    }
    return stream.next(out);
}

bool connectTo(const std::string& host, uint16_t port, TcpSocket* socket) {
    Address address;
    if (!Address::resolve(host, port, &address)) {
        std::fprintf(stderr, "cannot resolve %s\n", host.c_str());
        return false;
    }
    if (!socket->connect(address)) {
        std::fprintf(stderr, "connect to %s failed\n", address.toString().c_str());
        return false;
    }
    std::vector<SocketHandle> handles{socket->handle()};
    std::vector<bool> ready;
    waitReadable(handles, 2000, &ready);
    if (!socket->connectResult()) {
        std::fprintf(stderr, "connect to %s failed\n", address.toString().c_str());
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: worldcli <username> <password> [seconds]\n");
        return 2;
    }

    const std::string username = argv[1];
    const std::string password = argv[2];
    const int seconds = argc > 3 ? std::atoi(argv[3]) : 3;

    if (!initSockets()) {
        return 1;
    }

    // --- 1. Log in -------------------------------------------------------
    const std::string authHost = serverutil::envString("AUTH_HOST", "127.0.0.1");
    const uint16_t authPort = serverutil::envPort("AUTH_SERVER_PORT", 7001);

    TcpSocket authSocket;
    if (!connectTo(authHost, authPort, &authSocket)) {
        return 1;
    }
    MessageStream auth(std::move(authSocket));

    {
        std::vector<uint8_t> payload;
        ByteWriter writer(payload);
        writer.u32(kProtocolVersion);
        writer.string(username);
        writer.string(password);
        auth.send(static_cast<uint16_t>(AuthMessage::LoginRequest), payload);
    }

    MessageStream::Message message;
    if (!awaitMessage(auth, &message, 10000)) {
        std::fprintf(stderr, "no login response\n");
        return 1;
    }

    ByteReader loginReader(message.payload.data(), message.payload.size());
    const auto authResult = static_cast<AuthResult>(loginReader.u8());
    const std::string token = loginReader.string();
    const std::string worldHost = loginReader.string();
    const uint16_t worldPort = loginReader.u16();
    if (authResult != AuthResult::Success) {
        std::fprintf(stderr, "login failed: %s\n", toString(authResult));
        return 1;
    }
    std::printf("login ok, world at %s:%u\n", worldHost.c_str(), worldPort);

    // --- 2. Join the world over TCP --------------------------------------
    TcpSocket worldSocket;
    if (!connectTo(worldHost, worldPort, &worldSocket)) {
        return 1;
    }
    MessageStream world(std::move(worldSocket));

    {
        std::vector<uint8_t> payload;
        ByteWriter writer(payload);
        writer.u32(kProtocolVersion);
        writer.string(token);
        world.send(static_cast<uint16_t>(WorldMessage::JoinRequest), payload);
    }

    if (!awaitMessage(world, &message, 10000)) {
        std::fprintf(stderr, "no join response\n");
        return 1;
    }

    ByteReader joinReader(message.payload.data(), message.payload.size());
    const auto joinResult = static_cast<JoinResult>(joinReader.u8());
    const uint64_t playerId = joinReader.u64();
    const uint16_t udpPort = joinReader.u16();
    const std::string worldUsername = joinReader.string();
    const uint32_t ownEntityId = joinReader.u32();
    if (joinResult != JoinResult::Success) {
        std::fprintf(stderr, "join failed (result %u)\n", static_cast<unsigned>(joinResult));
        return 1;
    }
    std::printf("joined as player %llu ('%s'), entity %u, udp port %u\n",
            static_cast<unsigned long long>(playerId), worldUsername.c_str(), ownEntityId,
            udpPort);

    // --- 3. Bind a UDP address and read snapshots ------------------------
    UdpSocket udp;
    if (!udp.open(0)) {
        std::fprintf(stderr, "failed to open UDP socket\n");
        return 1;
    }

    Address udpTarget;
    if (!Address::resolve(worldHost, udpPort, &udpTarget)) {
        return 1;
    }

    std::vector<uint8_t> hello;
    writeUdpHeader(hello, static_cast<uint16_t>(WorldMessage::ClientHello), 0);
    ByteWriter helloWriter(hello);
    helloWriter.u64(playerId);
    helloWriter.string(token);
    udp.sendTo(hello.data(), hello.size(), udpTarget);
    std::printf("sent UDP hello, listening for snapshots...\n\n");

    // Optional movement script, so the server's authoritative character
    // physics can be exercised without the game client.
    const std::string action = argc > 4 ? argv[4] : "";
    uint32_t inputSequence = 1;
    const auto sendInput = [&](float forward, float right, float yaw, bool jump) {
        std::vector<uint8_t> packet;
        writeUdpHeader(packet, static_cast<uint16_t>(WorldMessage::ClientInput), inputSequence);
        ByteWriter writer(packet);
        writer.u64(playerId);
        writer.u32(inputSequence++);
        writer.f32(forward);
        writer.f32(right);
        writer.f32(yaw);
        writer.u8(jump ? kInputJump : 0);
        udp.sendTo(packet.data(), packet.size(), udpTarget);
    };

    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::seconds(seconds);
    uint8_t buffer[2048];
    int received = 0;
    uint32_t lastSequence = 0;
    int outOfOrder = 0;
    uint32_t firstTick = 0;
    uint32_t lastTick = 0;

    while (std::chrono::steady_clock::now() < deadline) {
        std::vector<SocketHandle> handles{udp.handle()};
        std::vector<bool> ready;
        waitReadable(handles, 16, &ready);

        const double elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - started).count();
        if (action == "move") {
            sendInput(1.0f, 0.0f, 0.0f, false);  // walk forward
        } else if (action == "jump") {
            // Stand still, then jump once after a second so the arc is clear.
            sendInput(0.0f, 0.0f, 0.0f, elapsed > 1.0 && elapsed < 1.1);
        }

        for (;;) {
            size_t size = 0;
            Address from;
            if (!udp.receiveFrom(buffer, sizeof buffer, &size, &from) || size == 0) {
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

            Snapshot snapshot;
            if (!decodeSnapshot(buffer + bodyOffset, size - bodyOffset, &snapshot)) {
                std::fprintf(stderr, "malformed snapshot\n");
                continue;
            }

            if (received > 0 && header.sequence < lastSequence) {
                ++outOfOrder;
            }
            lastSequence = header.sequence;
            if (received == 0) {
                firstTick = snapshot.tick;
            }
            lastTick = snapshot.tick;

            // Print a sample rather than every packet at 60Hz.
            if (received % 15 == 0 && !action.empty()) {
                for (const EntityState& p : snapshot.entities) {
                    if (p.id != ownEntityId) {
                        continue;
                    }
                    std::printf("t=%5.2f  player pos=(%+.2f, %+.3f, %+.2f)  vel=(%+.2f, %+.2f, "
                                "%+.2f)\n",
                            elapsed, p.position.x, p.position.y, p.position.z, p.velocity.x,
                            p.velocity.y, p.velocity.z);
                }
            } else if (received % 30 == 0 && action.empty() && !snapshot.entities.empty()) {
                // Summarise everyone in the snapshot, so a second client shows up.
                std::printf("  entities=%zu:", snapshot.entities.size());
                for (const EntityState& p : snapshot.entities) {
                    std::printf(" [%u %s (%+.1f,%+.2f,%+.1f)]", p.id,
                            p.type == static_cast<uint8_t>(EntityType::Player) ? "player" : "cube",
                            p.position.x, p.position.y, p.position.z);
                }
                std::printf("\n");
                const EntityState& e = snapshot.entities[0];
                // Yaw back out of the quaternion, purely so the output is
                // readable.
                const double yaw = 2.0 * std::atan2(e.rotation.y, e.rotation.w);
                std::printf("tick %6u  entity %u  pos=(%+.3f, %.3f, %+.3f)  "
                            "velX=%+.3f  yaw=%+.3f rad\n",
                        snapshot.tick, e.id, e.position.x, e.position.y, e.position.z,
                        e.velocity.x, yaw);
            }
            ++received;
        }
    }

    std::printf("\nreceived %d snapshots over %ds (~%d/s), ticks %u..%u, %d out of order\n",
            received, seconds, received / (seconds > 0 ? seconds : 1), firstTick, lastTick,
            outOfOrder);

    // --- 4. Log out ------------------------------------------------------
    std::vector<uint8_t> empty;
    world.send(static_cast<uint16_t>(WorldMessage::LogoutRequest), empty);
    if (awaitMessage(world, &message, 3000)) {
        std::printf("logged out\n");
    }

    shutdownSockets();
    return received > 0 ? 0 : 1;
}
