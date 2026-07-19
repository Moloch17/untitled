// World server: owns the simulation and replicates it to connected players.
//
// TCP handles joining and leaving -- events that must not be lost. UDP carries
// the snapshot stream at the tick rate. A player is only sent snapshots once
// they've bound a UDP address with a ClientHello matching their session.

#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <csignal>
#include <deque>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <net/byte_stream.h>
#include <net/protocol.h>
#include <net/snapshot.h>
#include <net/socket.h>
#include <serverutil/config.h>
#include <serverutil/db_client.h>
#include <serverutil/log.h>
#include <serverutil/tcp_server.h>

#include "simulation.h"

using namespace net;
using serverutil::Log;
using serverutil::TcpServer;

namespace {

volatile std::sig_atomic_t gRunning = 1;
void onSignal(int) { gRunning = 0; }

// Named gLog rather than log: box3d pulls in <math.h>, whose ::log() would
// otherwise make every call ambiguous.
const Log gLog("world");

// A player in the world. Created on a successful join, destroyed on logout or
// TCP disconnect.
struct Player {
    uint64_t playerId = 0;
    uint64_t accountId = 0;
    std::string username;
    std::string token;  // kept to match the UDP hello against
    TcpServer::ConnectionId connection = 0;

    bool udpBound = false;
    Address udpAddress;
    uint32_t outgoingSequence = 0;
    std::chrono::steady_clock::time_point lastUdpPacket;
};

// Packets held back to simulate a slow link. Applied in both directions, so a
// setting of 100ms costs roughly 200ms round trip -- what a player would
// actually feel.
struct DelayedPacket {
    double dueAt = 0.0;
    Address address;
    std::vector<uint8_t> data;
};

struct DelayedInput {
    double dueAt = 0.0;
    uint64_t playerId = 0;
    world::PlayerInput input;
};

double monotonicSeconds() {
    return std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
}

uint64_t nowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
}

// Compares without an early exit, so timing can't reveal how much of a token
// was correct.
bool constantTimeEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    unsigned char difference = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        difference |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return difference == 0;
}

void sendJoinResponse(TcpServer& server, TcpServer::ConnectionId id, JoinResult result,
        uint64_t playerId, uint16_t udpPort, const std::string& username, uint32_t entityId) {
    std::vector<uint8_t> payload;
    ByteWriter writer(payload);
    writer.u8(static_cast<uint8_t>(result));
    writer.u64(playerId);
    writer.u16(udpPort);
    writer.string(username);
    // Which replicated entity is this player's own body.
    writer.u32(entityId);
    server.send(id, static_cast<uint16_t>(WorldMessage::JoinResponse), payload);
}

}  // namespace

int main() {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
#if !defined(_WIN32)
    std::signal(SIGPIPE, SIG_IGN);
#endif

    if (!initSockets()) {
        gLog.error("failed to initialise sockets");
        return 1;
    }

    const uint16_t tcpPort = serverutil::envPort("WORLD_SERVER_PORT", 7002);
    const uint16_t udpPort = serverutil::envPort("WORLD_UDP_PORT", 7003);
    const std::string dbHost = serverutil::envString("DB_SERVER_HOST", "dbserver");
    const uint16_t dbPort = serverutil::envPort("DB_SERVER_PORT", 7000);
    const uint32_t udpTimeoutSeconds = serverutil::envUint("UDP_TIMEOUT_SECONDS", 30);


    serverutil::DbClient dbClient;
    for (int attempt = 1; gRunning; ++attempt) {
        if (dbClient.connect(dbHost, dbPort)) {
            break;
        }
        gLog.warn("database server connection attempt %d failed", attempt);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (!gRunning) {
        return 0;
    }
    gLog.info("connected to database server at %s:%u", dbHost.c_str(), dbPort);

    TcpServer server;
    if (!server.listen(tcpPort)) {
        gLog.error("failed to listen on TCP port %u: %s", tcpPort, lastSocketError().c_str());
        return 1;
    }

    UdpSocket udp;
    if (!udp.open(udpPort)) {
        gLog.error("failed to bind UDP port %u: %s", udpPort, lastSocketError().c_str());
        return 1;
    }
    gLog.info("listening on TCP %u / UDP %u, ticking at %d Hz", tcpPort, udpPort, kServerTickHz);

    // The clock runs at TIME_SPEED times real time; 1 means in-game noon is
    // real noon. DAY_LENGTH_SECONDS is kept as a convenience and converted,
    // since "a one minute day" is easier to think about while testing.
    float timeSpeed = 1.0f;
    const uint32_t dayLength = serverutil::envUint("DAY_LENGTH_SECONDS", 0);
    if (dayLength > 0) {
        timeSpeed = 86400.0f / static_cast<float>(dayLength);
    } else if (const char* value = std::getenv("TIME_SPEED")) {
        timeSpeed = static_cast<float>(std::atof(value));
    }

    world::Simulation simulation;
    simulation.init(timeSpeed);
    gLog.info("world clock at %.4gx real time (%.0f second day)", simulation.timeSpeed(),
            86400.0f / simulation.timeSpeed());

    // Simulated latency, set from the console. Zero means send and apply
    // immediately, which is the normal path.
    uint32_t simulatedLatencyMs = 0;
    std::deque<DelayedPacket> delayedOutgoing;
    std::deque<DelayedInput> delayedInputs;

    std::map<TcpServer::ConnectionId, Player> playersByConnection;
    std::map<uint64_t, TcpServer::ConnectionId> connectionByPlayer;
    uint64_t nextPlayerId = 1;

    const auto findPlayerByUdp = [&](const Address& from) -> Player* {
        for (auto& [connection, player] : playersByConnection) {
            if (player.udpBound && player.udpAddress == from) {
                return &player;
            }
        }
        return nullptr;
    };

    const auto removePlayer = [&](TcpServer::ConnectionId id, const char* reason) {
        auto it = playersByConnection.find(id);
        if (it == playersByConnection.end()) {
            return;
        }
        gLog.info("player '%s' left (%s), %zu remaining", it->second.username.c_str(), reason,
                playersByConnection.size() - 1);
        simulation.removePlayer(it->second.playerId);
        connectionByPlayer.erase(it->second.playerId);
        playersByConnection.erase(it);
    };

    server.callbacks.onDisconnect = [&](TcpServer::ConnectionId id) {
        removePlayer(id, "connection closed");
    };

    server.callbacks.onMessage = [&](TcpServer::ConnectionId id, uint16_t type,
                                          const std::vector<uint8_t>& data) {
        ByteReader reader(data.data(), data.size());

        switch (static_cast<WorldMessage>(type)) {
            case WorldMessage::JoinRequest: {
                const uint32_t version = reader.u32();
                const std::string token = reader.string();
                if (reader.failed()) {
                    sendJoinResponse(server, id, JoinResult::ServerError, 0, 0, "", 0);
                    return;
                }
                if (version != kProtocolVersion) {
                    sendJoinResponse(server, id, JoinResult::VersionMismatch, 0, 0, "", 0);
                    return;
                }
                if (playersByConnection.count(id)) {
                    return;  // already joined; ignore duplicates
                }

                // The token is only meaningful because the auth server put it
                // in the database; this server never sees a password.
                dbClient.lookupSession(token,
                        [&, id, token](bool ok, const serverutil::DbClient::SessionLookup&
                                                        session) {
                            if (!ok) {
                                gLog.error("session lookup failed");
                                sendJoinResponse(server, id, JoinResult::ServerError, 0, 0, "", 0);
                                return;
                            }
                            if (!session.found) {
                                gLog.warn("rejected join with invalid or expired session");
                                sendJoinResponse(server, id, JoinResult::InvalidSession, 0, 0, "", 0);
                                return;
                            }

                            Player player;
                            player.playerId = nextPlayerId++;
                            player.accountId = session.accountId;
                            player.username = session.username;
                            player.token = token;
                            player.connection = id;
                            player.lastUdpPacket = std::chrono::steady_clock::now();

                            connectionByPlayer[player.playerId] = id;
                            const uint64_t playerId = player.playerId;
                            const std::string username = player.username;
                            playersByConnection.emplace(id, std::move(player));

                            // Give the player a physics body in the world.
                            const uint32_t entityId = simulation.addPlayer(playerId);

                            gLog.info("player '%s' joined as player %llu (entity %u, account %llu)",
                                    username.c_str(), static_cast<unsigned long long>(playerId),
                                    entityId,
                                    static_cast<unsigned long long>(session.accountId));
                            sendJoinResponse(server, id, JoinResult::Success, playerId, udpPort,
                                    username, entityId);

                            // One-time redemption: the token has done its job,
                            // so remove it. A stolen token can't be replayed to
                            // open a second session.
                            dbClient.deleteSession(token, [](bool, DbResult) {});
                        });
                break;
            }

            case WorldMessage::AdminSetLatencyRequest: {
                const std::string token = reader.string();
                const uint32_t milliseconds = reader.u32();

                const auto reply = [&server, id, &simulatedLatencyMs](uint8_t result) {
                    std::vector<uint8_t> payload;
                    ByteWriter writer(payload);
                    writer.u8(result);
                    writer.u32(simulatedLatencyMs);
                    server.send(id, static_cast<uint16_t>(WorldMessage::AdminSetLatencyResponse),
                            payload);
                };
                if (reader.failed()) {
                    reply(1);
                    return;
                }

                dbClient.lookupSession(token,
                        [&simulatedLatencyMs, milliseconds, reply](bool ok,
                                const serverutil::DbClient::SessionLookup& session) {
                            if (!ok || !session.found) {
                                reply(1);
                                return;
                            }
                            if (session.username == kBootstrapAccount
                                    || session.permissionLevel
                                            < static_cast<uint8_t>(PermissionLevel::GameMaster)) {
                                reply(2);
                                return;
                            }
                            // Capped so a typo can't make the world unplayable
                            // for everyone connected.
                            simulatedLatencyMs = std::min(milliseconds, 2000u);
                            gLog.info("%s set simulated latency to %u ms",
                                    session.username.c_str(), simulatedLatencyMs);
                            reply(0);
                        });
                break;
            }

            case WorldMessage::AdminSetTimeRequest:
            case WorldMessage::AdminStatusRequest: {
                // Authorised the same way as the auth server's commands: the
                // caller's session names an account, and that account's level
                // decides. Controlling the world clock is a game-master power.
                const auto command = static_cast<WorldMessage>(type);
                const std::string token = reader.string();
                const auto timeCommand = command == WorldMessage::AdminSetTimeRequest
                        ? static_cast<TimeCommand>(reader.u8())
                        : TimeCommand::Set;
                const float value =
                        command == WorldMessage::AdminSetTimeRequest ? reader.f32() : 0.0f;

                const WorldMessage responseType = command == WorldMessage::AdminSetTimeRequest
                        ? WorldMessage::AdminSetTimeResponse
                        : WorldMessage::AdminStatusResponse;

                const auto reply = [&server, id, responseType, &simulation,
                                           &simulatedLatencyMs](uint8_t result) {
                    std::vector<uint8_t> payload;
                    ByteWriter writer(payload);
                    writer.u8(result);
                    writer.f32(simulation.timeOfDay());
                    writer.f32(simulation.timeSpeed());
                    if (responseType == WorldMessage::AdminStatusResponse) {
                        writer.u16(static_cast<uint16_t>(simulation.playerCount()));
                        writer.u32(simulation.tick());
                        writer.u32(simulatedLatencyMs);
                    }
                    server.send(id, static_cast<uint16_t>(responseType), payload);
                };

                if (reader.failed()) {
                    reply(1);
                    return;
                }

                dbClient.lookupSession(token,
                        [&simulation, command, timeCommand, value, reply](bool ok,
                                const serverutil::DbClient::SessionLookup& session) {
                            if (!ok || !session.found) {
                                reply(1);
                                return;
                            }
                            // The bootstrap account is admin-level but exists
                            // only to create the first real admin.
                            if (session.username == kBootstrapAccount) {
                                gLog.warn("the bootstrap account may only create an account");
                                reply(2);
                                return;
                            }
                            if (session.permissionLevel
                                    < static_cast<uint8_t>(PermissionLevel::GameMaster)) {
                                gLog.warn("'%s' (level %u) attempted a world command",
                                        session.username.c_str(), session.permissionLevel);
                                reply(2);
                                return;
                            }
                            if (command == WorldMessage::AdminSetTimeRequest) {
                                switch (timeCommand) {
                                    case TimeCommand::Reset:
                                        simulation.resetClock();
                                        gLog.info("%s reset the world clock to real time",
                                                session.username.c_str());
                                        break;
                                    case TimeCommand::Speed: {
                                        // Clamped: zero freezes the clock, and
                                        // the upper bound keeps a typo from
                                        // making the sun strobe.
                                        const float speed = std::clamp(value, 0.0f, 20000.0f);
                                        simulation.setTimeSpeed(speed);
                                        gLog.info("%s set the world clock to %.4gx real time",
                                                session.username.c_str(), speed);
                                        break;
                                    }
                                    case TimeCommand::Set:
                                    default:
                                        simulation.setTimeOfDay(value);
                                        gLog.info("%s set the world clock to %.4f",
                                                session.username.c_str(), value);
                                        break;
                                }
                            }
                            reply(0);
                        });
                break;
            }

            case WorldMessage::LogoutRequest: {
                std::vector<uint8_t> payload;
                server.send(id, static_cast<uint16_t>(WorldMessage::LogoutResponse), payload);
                removePlayer(id, "logout");
                break;
            }

            default:
                gLog.warn("unknown TCP message type %u", type);
                break;
        }
    };

    // Fixed-timestep loop. Snapshot sends are driven by the accumulator rather
    // than by how long the previous frame took, so the tick rate stays honest.
    const double tickSeconds = 1.0 / kServerTickHz;
    auto previous = std::chrono::steady_clock::now();
    double accumulator = 0.0;
    uint8_t datagram[2048];

    while (gRunning) {
        // Poll TCP briefly so joins are handled promptly without stalling ticks.
        server.poll(1);
        if (!dbClient.poll()) {
            gLog.warn("lost connection to database server, reconnecting");
            while (gRunning && !dbClient.connect(dbHost, dbPort)) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }

        // Drain inbound UDP.
        for (;;) {
            size_t size = 0;
            Address from;
            if (!udp.receiveFrom(datagram, sizeof datagram, &size, &from) || size == 0) {
                break;
            }

            UdpHeader header;
            size_t bodyOffset = 0;
            if (!readUdpHeader(datagram, size, &header, &bodyOffset)) {
                continue;  // not ours
            }

            if (static_cast<WorldMessage>(header.type) == WorldMessage::ClientHello) {
                ByteReader body(datagram + bodyOffset, size - bodyOffset);
                const uint64_t playerId = body.u64();
                const std::string token = body.string();
                if (body.failed()) {
                    continue;
                }

                auto connectionIt = connectionByPlayer.find(playerId);
                if (connectionIt == connectionByPlayer.end()) {
                    continue;
                }
                auto playerIt = playersByConnection.find(connectionIt->second);
                if (playerIt == playersByConnection.end()) {
                    continue;
                }

                // Anyone can send us a datagram claiming a player id, so the
                // token has to match before we start streaming to that address.
                if (!constantTimeEquals(playerIt->second.token, token)) {
                    gLog.warn("rejected UDP hello for player %llu with a bad token",
                            static_cast<unsigned long long>(playerId));
                    continue;
                }

                Player& player = playerIt->second;
                if (!player.udpBound || !(player.udpAddress == from)) {
                    gLog.info("player '%s' bound UDP address %s", player.username.c_str(),
                            from.toString().c_str());
                }
                player.udpBound = true;
                player.udpAddress = from;
                player.lastUdpPacket = std::chrono::steady_clock::now();
                continue;
            }

            if (Player* player = findPlayerByUdp(from)) {
                player->lastUdpPacket = std::chrono::steady_clock::now();

                if (static_cast<WorldMessage>(header.type) == WorldMessage::ClientInput) {
                    ByteReader body(datagram + bodyOffset, size - bodyOffset);
                    const uint64_t claimedId = body.u64();
                    world::PlayerInput input;
                    input.sequence = body.u32();
                    input.moveForward = body.f32();
                    input.moveRight = body.f32();
                    input.yaw = body.f32();
                    const uint8_t buttons = body.u8();

                    // The datagram's source address already identifies the
                    // player; the id in the body must agree, or it's spoofed.
                    if (body.failed() || claimedId != player->playerId) {
                        continue;
                    }

                    // Clamp: a modified client must not be able to ask for more
                    // than full-stick movement.
                    input.moveForward = std::clamp(input.moveForward, -1.0f, 1.0f);
                    input.moveRight = std::clamp(input.moveRight, -1.0f, 1.0f);
                    input.jump = (buttons & kInputJump) != 0;
                    input.sprint = (buttons & kInputSprint) != 0;

                    if (simulatedLatencyMs > 0) {
                        delayedInputs.push_back({monotonicSeconds()
                                        + simulatedLatencyMs / 1000.0,
                                player->playerId, input});
                    } else {
                        simulation.setInput(player->playerId, input);
                    }
                }
            }
        }

        const auto now = std::chrono::steady_clock::now();
        accumulator += std::chrono::duration<double>(now - previous).count();
        previous = now;

        // Cap catch-up so a stalled process doesn't then burn CPU replaying a
        // huge backlog of ticks.
        if (accumulator > 0.25) {
            accumulator = 0.25;
        }

        while (accumulator >= tickSeconds) {
            accumulator -= tickSeconds;
            simulation.step(static_cast<float>(tickSeconds));

            Snapshot snapshot;
            simulation.buildSnapshot(&snapshot);
            snapshot.serverTimeMs = nowMs();

            std::vector<uint8_t> body;
            encodeSnapshot(snapshot, body);

            for (auto& [connection, player] : playersByConnection) {
                if (!player.udpBound) {
                    continue;
                }
                std::vector<uint8_t> packet;
                writeUdpHeader(packet, static_cast<uint16_t>(WorldMessage::Snapshot),
                        player.outgoingSequence++);
                packet.insert(packet.end(), body.begin(), body.end());

                if (simulatedLatencyMs > 0) {
                    delayedOutgoing.push_back({monotonicSeconds() + simulatedLatencyMs / 1000.0,
                        player.udpAddress, std::move(packet)});
                } else {
                    udp.sendTo(packet.data(), packet.size(), player.udpAddress);
                }
            }
        }

        // Release anything the simulated link has held long enough.
        {
            const double now = monotonicSeconds();
            while (!delayedInputs.empty() && delayedInputs.front().dueAt <= now) {
                simulation.setInput(delayedInputs.front().playerId, delayedInputs.front().input);
                delayedInputs.pop_front();
            }
            while (!delayedOutgoing.empty() && delayedOutgoing.front().dueAt <= now) {
                const DelayedPacket& packet = delayedOutgoing.front();
                udp.sendTo(packet.data.data(), packet.data.size(), packet.address);
                delayedOutgoing.pop_front();
            }
        }

        // Drop players whose UDP has gone silent; their TCP socket may be a
        // half-open connection that will never report an error.
        const auto deadline = std::chrono::steady_clock::now()
                - std::chrono::seconds(udpTimeoutSeconds);
        std::vector<TcpServer::ConnectionId> timedOut;
        for (const auto& [connection, player] : playersByConnection) {
            if (player.udpBound && player.lastUdpPacket < deadline) {
                timedOut.push_back(connection);
            }
        }
        for (TcpServer::ConnectionId connection : timedOut) {
            removePlayer(connection, "UDP timeout");
            server.closeConnection(connection);
        }
    }

    gLog.info("shutting down");
    simulation.shutdown();
    shutdownSockets();
    return 0;
}
