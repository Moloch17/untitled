// Database server: the only process that talks to Postgres.
//
// Auth and world connect over TCP and issue RPCs; this translates them to
// parameterised SQL. Centralising it means credentials live in one place and
// the other services never construct SQL at all.

#include <chrono>
#include <csignal>
#include <thread>
#include <string>
#include <vector>

#include <net/byte_stream.h>
#include <net/protocol.h>
#include <serverutil/config.h>
#include <serverutil/log.h>
#include <serverutil/tcp_server.h>

#include "database.h"

using namespace net;
using serverutil::Log;

namespace {

volatile std::sig_atomic_t gRunning = 1;
void onSignal(int) { gRunning = 0; }

const Log log("db");

std::string buildConnInfo() {
    // Assembled from parts so the password never has to be embedded in a URL
    // in compose.yaml or a shell history.
    return "host=" + serverutil::envString("PGHOST", "postgres")
            + " port=" + std::to_string(serverutil::envPort("PGPORT", 5432))
            + " dbname=" + serverutil::envString("PGDATABASE", "untitled")
            + " user=" + serverutil::envString("PGUSER", "untitled")
            + " password=" + serverutil::envString("PGPASSWORD", "untitled")
            + " connect_timeout=5";
}

}  // namespace

int main() {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
#if !defined(_WIN32)
    // A client vanishing mid-write must not kill the process.
    std::signal(SIGPIPE, SIG_IGN);
#endif

    if (!initSockets()) {
        log.error("failed to initialise sockets");
        return 1;
    }

    const uint16_t port = serverutil::envPort("DB_SERVER_PORT", 7000);

    db::Database database;
    // Postgres may still be starting when this container comes up, so retry
    // rather than exiting and relying on the restart policy.
    for (int attempt = 1; gRunning; ++attempt) {
        if (database.connect(buildConnInfo())) {
            break;
        }
        log.warn("postgres connection attempt %d failed: %s", attempt,
                database.lastError().c_str());
        for (int i = 0; i < 20 && gRunning; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    if (!gRunning) {
        return 0;
    }
    if (!database.migrate()) {
        log.error("schema migration failed: %s", database.lastError().c_str());
        return 1;
    }
    log.info("connected to postgres");

    serverutil::TcpServer server;
    if (!server.listen(port)) {
        log.error("failed to listen on port %u: %s", port, lastSocketError().c_str());
        return 1;
    }
    log.info("listening on port %u", port);

    server.callbacks.onConnect = [](serverutil::TcpServer::ConnectionId id,
                                         const Address& peer) {
        log.info("service connected (conn %llu from %s)", static_cast<unsigned long long>(id),
                peer.toString().c_str());
    };
    server.callbacks.onDisconnect = [](serverutil::TcpServer::ConnectionId id) {
        log.info("service disconnected (conn %llu)", static_cast<unsigned long long>(id));
    };

    server.callbacks.onMessage = [&](serverutil::TcpServer::ConnectionId id, uint16_t type,
                                          const std::vector<uint8_t>& data) {
        ByteReader reader(data.data(), data.size());
        const uint32_t requestId = reader.u32();
        if (reader.failed()) {
            log.warn("dropping malformed request from conn %llu",
                    static_cast<unsigned long long>(id));
            return;
        }

        std::vector<uint8_t> payload;
        ByteWriter writer(payload);

        switch (static_cast<DbMessage>(type)) {
            case DbMessage::AccountLookupRequest: {
                const std::string username = reader.string();
                db::Account account;
                bool found = false;
                const bool ok = !reader.failed()
                        && database.findAccount(username, &account, &found);
                if (!ok) {
                    log.error("account lookup failed: %s", database.lastError().c_str());
                }
                writer.u32(requestId);
                writer.u8(ok && found ? 1 : 0);
                writer.u64(account.id);
                writer.string(account.passwordHash);
                writer.u8(account.permissionLevel);
                server.send(id, static_cast<uint16_t>(DbMessage::AccountLookupResponse), payload);
                break;
            }

            case DbMessage::AdminExistsRequest: {
                const std::string excluding = reader.string();
                bool exists = false;
                const bool ok = !reader.failed()
                        && database.hasAdminExcluding(excluding, &exists);
                writer.u32(requestId);
                writer.u8(ok && exists ? 1 : 0);
                server.send(id, static_cast<uint16_t>(DbMessage::AdminExistsResponse), payload);
                break;
            }

            case DbMessage::AccountSetLevelRequest: {
                const std::string username = reader.string();
                const uint8_t level = reader.u8();
                bool found = false;
                const bool ok = !reader.failed()
                        && database.setPermissionLevel(username, level, &found);
                if (ok && found) {
                    log.info("set '%s' permission level to %u", username.c_str(), level);
                }
                writer.u32(requestId);
                writer.u8(static_cast<uint8_t>(!ok ? DbResult::Error
                                : !found            ? DbResult::NotFound
                                                    : DbResult::Ok));
                server.send(id, static_cast<uint16_t>(DbMessage::AccountSetLevelResponse),
                        payload);
                break;
            }

            case DbMessage::AccountCreateWithLevelRequest:
            case DbMessage::AccountCreateRequest: {
                const std::string username = reader.string();
                const std::string passwordHash = reader.string();
                const uint8_t level = static_cast<DbMessage>(type)
                                == DbMessage::AccountCreateWithLevelRequest
                        ? reader.u8()
                        : 0;
                uint64_t accountId = 0;
                bool conflict = false;
                const bool ok = !reader.failed()
                        && database.createAccount(username, passwordHash, &accountId, &conflict);
                if (!ok) {
                    log.error("account create failed: %s", database.lastError().c_str());
                } else if (!conflict) {
                    if (level != 0) {
                        bool found = false;
                        database.setPermissionLevel(username, level, &found);
                    }
                    log.info("created account '%s' (id %llu, level %u)", username.c_str(),
                            static_cast<unsigned long long>(accountId), level);
                }
                writer.u32(requestId);
                writer.u8(static_cast<uint8_t>(!ok       ? DbResult::Error
                                : conflict               ? DbResult::Conflict
                                                         : DbResult::Ok));
                writer.u64(accountId);
                server.send(id, static_cast<uint16_t>(DbMessage::AccountCreateResponse), payload);
                break;
            }

            case DbMessage::AccountDeleteRequest: {
                const std::string username = reader.string();
                bool found = false;
                const bool ok = !reader.failed() && database.deleteAccount(username, &found);
                if (!ok) {
                    log.error("account delete failed: %s", database.lastError().c_str());
                } else if (found) {
                    log.info("deleted account '%s'", username.c_str());
                }
                writer.u32(requestId);
                writer.u8(static_cast<uint8_t>(!ok  ? DbResult::Error
                                : !found            ? DbResult::NotFound
                                                    : DbResult::Ok));
                server.send(id, static_cast<uint16_t>(DbMessage::AccountDeleteResponse), payload);
                break;
            }

            case DbMessage::SessionCreateRequest: {
                const uint64_t accountId = reader.u64();
                const std::string token = reader.string();
                const uint32_t ttl = reader.u32();
                const bool ok = !reader.failed()
                        && database.createSession(accountId, token, ttl)
                        && database.touchLastLogin(accountId);
                if (!ok) {
                    log.error("session create failed: %s", database.lastError().c_str());
                }
                writer.u32(requestId);
                writer.u8(static_cast<uint8_t>(ok ? DbResult::Ok : DbResult::Error));
                server.send(id, static_cast<uint16_t>(DbMessage::SessionCreateResponse), payload);
                break;
            }

            case DbMessage::SessionLookupRequest: {
                const std::string token = reader.string();
                db::Session session;
                bool found = false;
                const bool ok = !reader.failed()
                        && database.lookupSession(token, &session, &found);
                if (!ok) {
                    log.error("session lookup failed: %s", database.lastError().c_str());
                }
                writer.u32(requestId);
                writer.u8(ok && found ? 1 : 0);
                writer.u64(session.accountId);
                writer.string(session.username);
                writer.u8(session.permissionLevel);
                server.send(id, static_cast<uint16_t>(DbMessage::SessionLookupResponse), payload);
                break;
            }

            case DbMessage::SessionDeleteRequest: {
                const std::string token = reader.string();
                const bool ok = !reader.failed() && database.deleteSession(token);
                writer.u32(requestId);
                writer.u8(static_cast<uint8_t>(ok ? DbResult::Ok : DbResult::Error));
                server.send(id, static_cast<uint16_t>(DbMessage::SessionDeleteResponse), payload);
                break;
            }

            default:
                log.warn("unknown message type %u from conn %llu", type,
                        static_cast<unsigned long long>(id));
                break;
        }
    };

    // Expired sessions are filtered out by every query anyway; this just stops
    // the table growing forever.
    int ticksUntilPurge = 0;
    while (gRunning) {
        server.poll(200);
        if (--ticksUntilPurge <= 0) {
            ticksUntilPurge = 5 * 60 * 5;  // ~5 minutes at 200ms
            int removed = 0;
            if (database.purgeExpiredSessions(&removed) && removed > 0) {
                log.info("purged %d expired session(s)", removed);
            }
        }
    }

    log.info("shutting down");
    shutdownSockets();
    return 0;
}
