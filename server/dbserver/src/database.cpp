#include "database.h"

#include <cstdio>
#include <cstdlib>

#include <libpq-fe.h>

namespace db {

namespace {

// Owns a PGresult so early returns can't leak it.
class Result {
public:
    explicit Result(PGresult* result) : mResult(result) {}
    ~Result() { PQclear(mResult); }
    Result(const Result&) = delete;
    Result& operator=(const Result&) = delete;

    PGresult* get() const { return mResult; }
    ExecStatusType status() const { return PQresultStatus(mResult); }
    int rows() const { return PQntuples(mResult); }
    const char* value(int row, int column) const { return PQgetvalue(mResult, row, column); }

private:
    PGresult* mResult;
};

uint64_t toUint64(const char* text) {
    return text ? std::strtoull(text, nullptr, 10) : 0;
}

}  // namespace

Database::~Database() { disconnect(); }

bool Database::connect(const std::string& conninfo) {
    disconnect();
    mConninfo = conninfo;
    mConnection = PQconnectdb(conninfo.c_str());
    if (PQstatus(mConnection) != CONNECTION_OK) {
        mLastError = PQerrorMessage(mConnection);
        PQfinish(mConnection);
        mConnection = nullptr;
        return false;
    }
    return true;
}

void Database::disconnect() {
    if (mConnection) {
        PQfinish(mConnection);
        mConnection = nullptr;
    }
}

bool Database::ensureConnected() {
    if (mConnection && PQstatus(mConnection) == CONNECTION_OK) {
        return true;
    }
    if (mConnection) {
        // Postgres restarting shouldn't take the database server down with it.
        PQreset(mConnection);
        if (PQstatus(mConnection) == CONNECTION_OK) {
            return true;
        }
    }
    return connect(mConninfo);
}

bool Database::findAccount(const std::string& username, Account* out, bool* found) {
    *found = false;
    if (!ensureConnected()) {
        return false;
    }

    const char* values[] = {username.c_str()};
    const Result result(PQexecParams(mConnection,
            "SELECT id, username, password_hash FROM accounts WHERE lower(username) = lower($1)",
            1, nullptr, values, nullptr, nullptr, 0));

    if (result.status() != PGRES_TUPLES_OK) {
        mLastError = PQerrorMessage(mConnection);
        return false;
    }
    if (result.rows() == 0) {
        return true;  // query succeeded, no such account
    }

    out->id = toUint64(result.value(0, 0));
    out->username = result.value(0, 1);
    out->passwordHash = result.value(0, 2);
    *found = true;
    return true;
}

bool Database::createAccount(const std::string& username, const std::string& passwordHash,
        uint64_t* accountId, bool* conflict) {
    *conflict = false;
    if (!ensureConnected()) {
        return false;
    }

    const char* values[] = {username.c_str(), passwordHash.c_str()};
    // ON CONFLICT lets the unique index decide, which avoids the race a
    // check-then-insert would have.
    const Result result(PQexecParams(mConnection,
            "INSERT INTO accounts (username, password_hash) VALUES ($1, $2) "
            "ON CONFLICT DO NOTHING RETURNING id",
            2, nullptr, values, nullptr, nullptr, 0));

    if (result.status() != PGRES_TUPLES_OK) {
        mLastError = PQerrorMessage(mConnection);
        return false;
    }
    if (result.rows() == 0) {
        *conflict = true;
        return true;
    }

    *accountId = toUint64(result.value(0, 0));
    return true;
}

bool Database::touchLastLogin(uint64_t accountId) {
    if (!ensureConnected()) {
        return false;
    }
    const std::string id = std::to_string(accountId);
    const char* values[] = {id.c_str()};
    const Result result(PQexecParams(mConnection,
            "UPDATE accounts SET last_login_at = now() WHERE id = $1", 1, nullptr, values,
            nullptr, nullptr, 0));
    if (result.status() != PGRES_COMMAND_OK) {
        mLastError = PQerrorMessage(mConnection);
        return false;
    }
    return true;
}

bool Database::deleteAccount(const std::string& username, bool* found) {
    *found = false;
    if (!ensureConnected()) {
        return false;
    }

    const char* values[] = {username.c_str()};
    const Result result(PQexecParams(mConnection,
            "DELETE FROM accounts WHERE lower(username) = lower($1) RETURNING id", 1, nullptr,
            values, nullptr, nullptr, 0));

    if (result.status() != PGRES_TUPLES_OK) {
        mLastError = PQerrorMessage(mConnection);
        return false;
    }
    *found = result.rows() > 0;
    return true;
}

bool Database::createSession(uint64_t accountId, const std::string& token, uint32_t ttlSeconds) {
    if (!ensureConnected()) {
        return false;
    }

    const std::string id = std::to_string(accountId);
    const std::string ttl = std::to_string(ttlSeconds);
    const char* values[] = {token.c_str(), id.c_str(), ttl.c_str()};
    const Result result(PQexecParams(mConnection,
            "INSERT INTO sessions (token, account_id, expires_at) "
            "VALUES ($1, $2, now() + ($3 || ' seconds')::interval)",
            3, nullptr, values, nullptr, nullptr, 0));

    if (result.status() != PGRES_COMMAND_OK) {
        mLastError = PQerrorMessage(mConnection);
        return false;
    }
    return true;
}

bool Database::lookupSession(const std::string& token, Session* out, bool* found) {
    *found = false;
    if (!ensureConnected()) {
        return false;
    }

    const char* values[] = {token.c_str()};
    // Expiry is enforced in the query so a stale row can never authorise a join.
    const Result result(PQexecParams(mConnection,
            "SELECT s.account_id, a.username FROM sessions s "
            "JOIN accounts a ON a.id = s.account_id "
            "WHERE s.token = $1 AND s.expires_at > now()",
            1, nullptr, values, nullptr, nullptr, 0));

    if (result.status() != PGRES_TUPLES_OK) {
        mLastError = PQerrorMessage(mConnection);
        return false;
    }
    if (result.rows() == 0) {
        return true;
    }

    out->accountId = toUint64(result.value(0, 0));
    out->username = result.value(0, 1);
    *found = true;
    return true;
}

bool Database::deleteSession(const std::string& token) {
    if (!ensureConnected()) {
        return false;
    }
    const char* values[] = {token.c_str()};
    const Result result(PQexecParams(mConnection, "DELETE FROM sessions WHERE token = $1", 1,
            nullptr, values, nullptr, nullptr, 0));
    if (result.status() != PGRES_COMMAND_OK) {
        mLastError = PQerrorMessage(mConnection);
        return false;
    }
    return true;
}

bool Database::purgeExpiredSessions(int* removed) {
    *removed = 0;
    if (!ensureConnected()) {
        return false;
    }
    const Result result(PQexec(mConnection, "DELETE FROM sessions WHERE expires_at <= now()"));
    if (result.status() != PGRES_COMMAND_OK) {
        mLastError = PQerrorMessage(mConnection);
        return false;
    }
    const char* affected = PQcmdTuples(result.get());
    *removed = affected && *affected ? std::atoi(affected) : 0;
    return true;
}

}  // namespace db
