#pragma once

#include <cstdint>
#include <string>

struct pg_conn;

namespace db {

// Mirrors net::PermissionLevel.
struct Account {
    uint64_t id = 0;
    std::string username;
    std::string passwordHash;
    uint8_t permissionLevel = 0;
};

struct Session {
    uint64_t accountId = 0;
    std::string username;
    uint8_t permissionLevel = 0;
};

// Thin libpq wrapper. Every query is parameterised -- values are never pasted
// into SQL text, so a username like "'; DROP TABLE accounts; --" is just an
// ordinary string that fails to match.
//
// This is the only place in the project that talks to Postgres.
class Database {
public:
    ~Database();

    // conninfo is a libpq connection string, e.g.
    // "host=postgres port=5432 dbname=untitled user=untitled password=..."
    bool connect(const std::string& conninfo);
    // Brings an existing database up to date. schema.sql only runs on a fresh
    // volume, so additive changes have to be applied here too.
    bool migrate();
    void disconnect();

    // Reconnects if the connection dropped. Returns false if it's still down.
    bool ensureConnected();

    bool findAccount(const std::string& username, Account* out, bool* found);
    bool createAccount(const std::string& username, const std::string& passwordHash,
            uint64_t* accountId, bool* conflict);
    bool touchLastLogin(uint64_t accountId);
    // Sessions are removed with the account by the FK's ON DELETE CASCADE.
    bool deleteAccount(const std::string& username, bool* found);
    bool setPermissionLevel(const std::string& username, uint8_t level, bool* found);
    // True if any account has admin rights; used to decide whether the
    // bootstrap account is needed.
    bool hasAdmin(bool* exists);

    bool createSession(uint64_t accountId, const std::string& token, uint32_t ttlSeconds);
    bool lookupSession(const std::string& token, Session* out, bool* found);
    bool deleteSession(const std::string& token);
    // Housekeeping so expired rows don't accumulate.
    bool purgeExpiredSessions(int* removed);

    const std::string& lastError() const { return mLastError; }

private:
    pg_conn* mConnection = nullptr;
    std::string mConninfo;
    std::string mLastError;
};

}  // namespace db
