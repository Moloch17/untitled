// Auth server: verifies credentials and hands out short-lived session tokens.
//
// It never touches Postgres directly -- account rows come from the database
// server. Password hashing and token generation happen here so the database
// server stays a dumb data layer.

#include <chrono>
#include <csignal>
#include <thread>
#include <ctime>
#include <string>
#include <vector>

#include <sodium.h>

#include <net/byte_stream.h>
#include <net/protocol.h>
#include <serverutil/config.h>
#include <serverutil/db_client.h>
#include <serverutil/log.h>
#include <serverutil/tcp_server.h>

using namespace net;
using serverutil::Log;

namespace {

volatile std::sig_atomic_t gRunning = 1;
void onSignal(int) { gRunning = 0; }

const Log log("auth");

constexpr size_t kTokenBytes = 32;
constexpr size_t kMinPasswordLength = 6;
constexpr size_t kMaxUsernameLength = 32;

// Random, unguessable, and long enough that brute-forcing is hopeless.
std::string generateToken() {
    unsigned char raw[kTokenBytes];
    randombytes_buf(raw, sizeof raw);

    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(kTokenBytes * 2);
    for (unsigned char byte : raw) {
        out.push_back(hex[byte >> 4]);
        out.push_back(hex[byte & 0x0F]);
    }
    return out;
}

// Argon2id with libsodium's interactive parameters: deliberately slow, so a
// stolen database is expensive to attack offline.
bool hashPassword(const std::string& password, std::string* out) {
    char hashed[crypto_pwhash_STRBYTES];
    if (crypto_pwhash_str(hashed, password.c_str(), password.size(),
                crypto_pwhash_OPSLIMIT_INTERACTIVE, crypto_pwhash_MEMLIMIT_INTERACTIVE)
            != 0) {
        return false;  // out of memory
    }
    *out = hashed;
    return true;
}

bool verifyPassword(const std::string& hash, const std::string& password) {
    return crypto_pwhash_str_verify(hash.c_str(), password.c_str(), password.size()) == 0;
}

bool validUsername(const std::string& username) {
    if (username.empty() || username.size() > kMaxUsernameLength) {
        return false;
    }
    for (char c : username) {
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

void sendLoginResponse(serverutil::TcpServer& server, serverutil::TcpServer::ConnectionId id,
        AuthResult result, const std::string& token, const std::string& worldHost,
        uint16_t worldPort, uint64_t accountId, uint8_t permissionLevel) {
    std::vector<uint8_t> payload;
    ByteWriter writer(payload);
    writer.u8(static_cast<uint8_t>(result));
    writer.string(token);
    writer.string(worldHost);
    writer.u16(worldPort);
    writer.u64(accountId);
    writer.u8(permissionLevel);
    server.send(id, static_cast<uint16_t>(AuthMessage::LoginResponse), payload);
}

void sendAdminResponse(serverutil::TcpServer& server, serverutil::TcpServer::ConnectionId id,
        AuthMessage type, AuthResult result) {
    std::vector<uint8_t> payload;
    ByteWriter writer(payload);
    writer.u8(static_cast<uint8_t>(result));
    server.send(id, static_cast<uint16_t>(type), payload);
}

void sendRegisterResponse(serverutil::TcpServer& server, serverutil::TcpServer::ConnectionId id,
        AuthResult result) {
    std::vector<uint8_t> payload;
    ByteWriter writer(payload);
    writer.u8(static_cast<uint8_t>(result));
    server.send(id, static_cast<uint16_t>(AuthMessage::RegisterResponse), payload);
}

}  // namespace

int main() {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
#if !defined(_WIN32)
    std::signal(SIGPIPE, SIG_IGN);
#endif

    if (sodium_init() < 0) {
        log.error("failed to initialise libsodium");
        return 1;
    }
    if (!initSockets()) {
        log.error("failed to initialise sockets");
        return 1;
    }

    const uint16_t listenPort = serverutil::envPort("AUTH_SERVER_PORT", 7001);
    const std::string dbHost = serverutil::envString("DB_SERVER_HOST", "dbserver");
    const uint16_t dbPort = serverutil::envPort("DB_SERVER_PORT", 7000);
    // Handed to the client on success -- the address it should use, which is
    // not necessarily the one the world server binds inside its container.
    const std::string worldHost = serverutil::envString("WORLD_PUBLIC_HOST", "127.0.0.1");
    const uint16_t worldPort = serverutil::envPort("WORLD_SERVER_PORT", 7002);
    const uint32_t sessionTtl = serverutil::envUint("SESSION_TTL_SECONDS", 120);

    // Optional first-run bootstrap: BOOTSTRAP_ADMIN=name:password gives that
    // account admin rights if -- and only if -- no admin exists yet. Without
    // it a fresh database has no way to grant the first privilege, since every
    // privileged action requires an already-privileged account.
    const std::string bootstrap = serverutil::envString("BOOTSTRAP_ADMIN", "");

    serverutil::DbClient dbClient;
    for (int attempt = 1; gRunning; ++attempt) {
        if (dbClient.connect(dbHost, dbPort)) {
            break;
        }
        log.warn("database server connection attempt %d failed", attempt);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (!gRunning) {
        return 0;
    }
    log.info("connected to database server at %s:%u", dbHost.c_str(), dbPort);

    // Apply the bootstrap before serving. It is a no-op once any admin exists,
    // so leaving it set is harmless.
    if (!bootstrap.empty()) {
        const size_t colon = bootstrap.find(':');
        if (colon == std::string::npos) {
            log.warn("BOOTSTRAP_ADMIN should look like name:password");
        } else {
            const std::string name = bootstrap.substr(0, colon);
            const std::string password = bootstrap.substr(colon + 1);
            std::string hash;
            if (validUsername(name) && password.size() >= kMinPasswordLength
                    && hashPassword(password, &hash)) {
                // Create if missing, then promote. Both are idempotent, so a
                // restart with the same value changes nothing.
                dbClient.createAccountWithLevel(name, hash,
                        static_cast<uint8_t>(PermissionLevel::Admin),
                        [](bool, DbResult, uint64_t) {});
                dbClient.setPermissionLevel(name, static_cast<uint8_t>(PermissionLevel::Admin),
                        [name](bool ok, DbResult result) {
                            if (ok && result == DbResult::Ok) {
                                log.info("bootstrap: '%s' has admin rights", name.c_str());
                            }
                        });
                for (int i = 0; i < 40; ++i) {
                    dbClient.poll();
                    std::this_thread::sleep_for(std::chrono::milliseconds(25));
                }
            } else {
                log.warn("BOOTSTRAP_ADMIN rejected: bad username or short password");
            }
        }
    }

    serverutil::TcpServer server;
    if (!server.listen(listenPort)) {
        log.error("failed to listen on port %u: %s", listenPort, lastSocketError().c_str());
        return 1;
    }
    log.info("listening on port %u", listenPort);

    server.callbacks.onConnect = [](serverutil::TcpServer::ConnectionId id, const Address& peer) {
        log.info("client connected (conn %llu from %s)", static_cast<unsigned long long>(id),
                peer.toString().c_str());
    };

    server.callbacks.onMessage = [&](serverutil::TcpServer::ConnectionId id, uint16_t type,
                                          const std::vector<uint8_t>& data) {
        ByteReader reader(data.data(), data.size());

        switch (static_cast<AuthMessage>(type)) {
            case AuthMessage::LoginRequest: {
                const uint32_t version = reader.u32();
                const std::string username = reader.string();
                const std::string password = reader.string();
                if (reader.failed()) {
                    sendLoginResponse(server, id, AuthResult::MalformedRequest, "", "", 0, 0, 0);
                    return;
                }
                if (version != kProtocolVersion) {
                    log.warn("rejecting client with protocol version %u (expected %u)", version,
                            kProtocolVersion);
                    sendLoginResponse(server, id, AuthResult::VersionMismatch, "", "", 0, 0, 0);
                    return;
                }

                log.info("login attempt for '%s'", username.c_str());
                dbClient.lookupAccount(username,
                        [&server, id, password, username, worldHost, worldPort, sessionTtl,
                                &dbClient](bool ok, const serverutil::DbClient::AccountLookup&
                                                            account) {
                            if (!ok) {
                                log.error("account lookup failed for '%s'", username.c_str());
                                sendLoginResponse(server, id, AuthResult::ServerError, "", "", 0, 0, 0);
                                return;
                            }

                            // Same response for "no such user" and "wrong
                            // password", so the reply can't be used to
                            // enumerate valid usernames.
                            if (!account.found || !verifyPassword(account.passwordHash, password)) {
                                log.info("login failed for '%s'", username.c_str());
                                sendLoginResponse(server, id, AuthResult::InvalidCredentials, "", "", 0, 0, 0);
                                return;
                            }

                            const std::string token = generateToken();
                            dbClient.createSession(account.accountId, token, sessionTtl,
                                    [&server, id, token, worldHost, worldPort, username,
                                            accountId = account.accountId,
                                            level = account.permissionLevel](bool sessionOk,
                                            DbResult result) {
                                        if (!sessionOk || result != DbResult::Ok) {
                                            log.error("failed to create session for '%s'",
                                                    username.c_str());
                                            sendLoginResponse(server, id, AuthResult::ServerError,
                                                    "", "", 0, 0, 0);
                                            return;
                                        }
                                        log.info("login succeeded for '%s' (account %llu)",
                                                username.c_str(),
                                                static_cast<unsigned long long>(accountId));
                                        sendLoginResponse(server, id, AuthResult::Success, token,
                                                worldHost, worldPort, accountId, level);
                                    });
                        });
                break;
            }

            case AuthMessage::RegisterRequest: {
                const uint32_t version = reader.u32();
                const std::string username = reader.string();
                const std::string password = reader.string();
                if (reader.failed()) {
                    sendRegisterResponse(server, id, AuthResult::MalformedRequest);
                    return;
                }
                if (version != kProtocolVersion) {
                    sendRegisterResponse(server, id, AuthResult::VersionMismatch);
                    return;
                }
                if (!validUsername(username) || password.size() < kMinPasswordLength) {
                    sendRegisterResponse(server, id, AuthResult::MalformedRequest);
                    return;
                }

                std::string hash;
                if (!hashPassword(password, &hash)) {
                    log.error("password hashing failed");
                    sendRegisterResponse(server, id, AuthResult::ServerError);
                    return;
                }

                dbClient.createAccount(username, hash,
                        [&server, id, username](bool ok, DbResult result, uint64_t accountId) {
                            if (!ok) {
                                sendRegisterResponse(server, id, AuthResult::ServerError);
                                return;
                            }
                            if (result == DbResult::Conflict) {
                                sendRegisterResponse(server, id, AuthResult::AccountExists);
                                return;
                            }
                            log.info("registered '%s' as account %llu", username.c_str(),
                                    static_cast<unsigned long long>(accountId));
                            sendRegisterResponse(server, id, AuthResult::Success);
                        });
                break;
            }

            case AuthMessage::AdminAccountCreateRequest:
            case AuthMessage::AdminAccountDeleteRequest:
            case AuthMessage::AdminSetLevelRequest: {
                // Every privileged command is authorised the same way: resolve
                // the caller's session to an account and check that account's
                // permission level. No shared secret exists to leak, and the
                // log names who did what.
                const auto command = static_cast<AuthMessage>(type);
                const std::string token = reader.string();
                const std::string target = reader.string();
                const std::string password =
                        command == AuthMessage::AdminAccountCreateRequest ? reader.string() : "";
                const uint8_t level = (command == AuthMessage::AdminAccountCreateRequest
                                              || command == AuthMessage::AdminSetLevelRequest)
                        ? reader.u8()
                        : 0;

                const AuthMessage responseType =
                        command == AuthMessage::AdminAccountCreateRequest
                        ? AuthMessage::AdminAccountCreateResponse
                        : (command == AuthMessage::AdminAccountDeleteRequest
                                          ? AuthMessage::AdminAccountDeleteResponse
                                          : AuthMessage::AdminSetLevelResponse);

                if (reader.failed() || target.empty()) {
                    sendAdminResponse(server, id, responseType, AuthResult::MalformedRequest);
                    return;
                }

                dbClient.lookupSession(token,
                        [&server, &dbClient, id, command, responseType, target, password, level](
                                bool ok, const serverutil::DbClient::SessionLookup& session) {
                            if (!ok) {
                                sendAdminResponse(server, id, responseType,
                                        AuthResult::ServerError);
                                return;
                            }
                            if (!session.found) {
                                sendAdminResponse(server, id, responseType,
                                        AuthResult::SessionExpired);
                                return;
                            }
                            // Account management is admin-only.
                            if (session.permissionLevel
                                    < static_cast<uint8_t>(PermissionLevel::Admin)) {
                                log.warn("'%s' (level %u) attempted an admin command",
                                        session.username.c_str(), session.permissionLevel);
                                sendAdminResponse(server, id, responseType,
                                        AuthResult::InsufficientPermission);
                                return;
                            }

                            const std::string actor = session.username;
                            if (command == AuthMessage::AdminAccountCreateRequest) {
                                if (!validUsername(target) || password.size() < kMinPasswordLength
                                        || level > static_cast<uint8_t>(PermissionLevel::Admin)) {
                                    sendAdminResponse(server, id, responseType,
                                            AuthResult::MalformedRequest);
                                    return;
                                }
                                std::string hash;
                                if (!hashPassword(password, &hash)) {
                                    sendAdminResponse(server, id, responseType,
                                            AuthResult::ServerError);
                                    return;
                                }
                                dbClient.createAccountWithLevel(target, hash, level,
                                        [&server, id, responseType, target, level, actor](
                                                bool created, DbResult result, uint64_t) {
                                            if (!created) {
                                                sendAdminResponse(server, id, responseType,
                                                        AuthResult::ServerError);
                                                return;
                                            }
                                            if (result == DbResult::Conflict) {
                                                sendAdminResponse(server, id, responseType,
                                                        AuthResult::AccountExists);
                                                return;
                                            }
                                            log.info("%s created account '%s' at level %u",
                                                    actor.c_str(), target.c_str(), level);
                                            sendAdminResponse(server, id, responseType,
                                                    AuthResult::Success);
                                        });
                            } else if (command == AuthMessage::AdminAccountDeleteRequest) {
                                dbClient.deleteAccount(target,
                                        [&server, id, responseType, target, actor](
                                                bool deleted, DbResult result) {
                                            if (!deleted) {
                                                sendAdminResponse(server, id, responseType,
                                                        AuthResult::ServerError);
                                                return;
                                            }
                                            if (result == DbResult::NotFound) {
                                                sendAdminResponse(server, id, responseType,
                                                        AuthResult::NoSuchAccount);
                                                return;
                                            }
                                            log.info("%s deleted account '%s'", actor.c_str(),
                                                    target.c_str());
                                            sendAdminResponse(server, id, responseType,
                                                    AuthResult::Success);
                                        });
                            } else {
                                if (level > static_cast<uint8_t>(PermissionLevel::Admin)) {
                                    sendAdminResponse(server, id, responseType,
                                            AuthResult::MalformedRequest);
                                    return;
                                }
                                dbClient.setPermissionLevel(target, level,
                                        [&server, id, responseType, target, level, actor](
                                                bool set, DbResult result) {
                                            if (!set) {
                                                sendAdminResponse(server, id, responseType,
                                                        AuthResult::ServerError);
                                                return;
                                            }
                                            if (result == DbResult::NotFound) {
                                                sendAdminResponse(server, id, responseType,
                                                        AuthResult::NoSuchAccount);
                                                return;
                                            }
                                            log.info("%s set '%s' to level %u", actor.c_str(),
                                                    target.c_str(), level);
                                            sendAdminResponse(server, id, responseType,
                                                    AuthResult::Success);
                                        });
                            }
                        });
                break;
            }

            default:
                log.warn("unknown message type %u", type);
                break;
        }
    };

    while (gRunning) {
        server.poll(50);
        if (!dbClient.poll()) {
            log.warn("lost connection to database server, reconnecting");
            while (gRunning && !dbClient.connect(dbHost, dbPort)) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    }

    log.info("shutting down");
    shutdownSockets();
    return 0;
}
