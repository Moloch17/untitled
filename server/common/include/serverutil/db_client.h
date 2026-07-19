#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include <net/framing.h>
#include <net/protocol.h>
#include <net/socket.h>

namespace serverutil {

// Client side of the database server RPC, used by the auth and world servers.
//
// Calls are asynchronous: each carries a request id, and the reply invokes the
// callback registered for it. That keeps the single-threaded event loop from
// blocking on Postgres while other clients wait.
class DbClient {
public:
    struct AccountLookup {
        bool found = false;
        uint64_t accountId = 0;
        std::string passwordHash;
    };

    struct SessionLookup {
        bool found = false;
        uint64_t accountId = 0;
        std::string username;
    };

    bool connect(const std::string& host, uint16_t port);
    bool connected() const { return mConnected; }

    // Pumps the socket and dispatches replies. Returns false if the connection
    // dropped, in which case pending callbacks are invoked with a failure.
    bool poll();

    net::SocketHandle handle() const;

    void lookupAccount(const std::string& username,
            std::function<void(bool ok, const AccountLookup&)> callback);
    void createAccount(const std::string& username, const std::string& passwordHash,
            std::function<void(bool ok, net::DbResult, uint64_t accountId)> callback);
    void deleteAccount(const std::string& username,
            std::function<void(bool ok, net::DbResult)> callback);
    void createSession(uint64_t accountId, const std::string& token, uint32_t ttlSeconds,
            std::function<void(bool ok, net::DbResult)> callback);
    void lookupSession(const std::string& token,
            std::function<void(bool ok, const SessionLookup&)> callback);
    void deleteSession(const std::string& token,
            std::function<void(bool ok, net::DbResult)> callback);

private:
    using Handler = std::function<void(bool ok, uint16_t type, const std::vector<uint8_t>&)>;

    uint32_t nextRequestId() { return mNextRequestId++; }
    void registerHandler(uint32_t requestId, Handler handler);
    void failAllPending();

    std::unique_ptr<net::MessageStream> mStream;
    std::map<uint32_t, Handler> mPending;
    uint32_t mNextRequestId = 1;
    bool mConnected = false;
};

}  // namespace serverutil
