#include "serverutil/db_client.h"

#include <net/byte_stream.h>

using namespace net;

namespace serverutil {

bool DbClient::connect(const std::string& host, uint16_t port) {
    Address address;
    if (!Address::resolve(host, port, &address)) {
        return false;
    }

    TcpSocket socket;
    if (!socket.connect(address)) {
        return false;
    }

    // The connect is non-blocking, but these are inter-server links on a
    // container network established once at startup, so waiting briefly for
    // completion keeps the callers simple.
    std::vector<SocketHandle> handles{socket.handle()};
    std::vector<bool> ready;
    waitReadable(handles, 2000, &ready);
    if (!socket.connectResult()) {
        return false;
    }

    mStream = std::make_unique<MessageStream>(std::move(socket));
    mConnected = true;
    return true;
}

net::SocketHandle DbClient::handle() const {
    return mStream ? mStream->socket().handle() : kInvalidSocket;
}

void DbClient::registerHandler(uint32_t requestId, Handler handler) {
    mPending.emplace(requestId, std::move(handler));
}

void DbClient::failAllPending() {
    // Move first: a callback may enqueue new work, which must not be dropped by
    // the clear below.
    auto pending = std::move(mPending);
    mPending.clear();
    for (auto& [id, handler] : pending) {
        handler(false, 0, {});
    }
}

bool DbClient::poll() {
    if (!mConnected || !mStream) {
        return false;
    }
    if (!mStream->pump()) {
        mConnected = false;
        failAllPending();
        return false;
    }

    MessageStream::Message message;
    while (mStream->next(&message)) {
        // Every reply starts with the request id it answers.
        ByteReader reader(message.payload.data(), message.payload.size());
        const uint32_t requestId = reader.u32();
        if (reader.failed()) {
            continue;
        }

        auto it = mPending.find(requestId);
        if (it == mPending.end()) {
            continue;  // duplicate or unsolicited reply
        }
        Handler handler = std::move(it->second);
        mPending.erase(it);
        handler(true, message.type, message.payload);
    }
    return true;
}

void DbClient::lookupAccount(const std::string& username,
        std::function<void(bool, const AccountLookup&)> callback) {
    if (!mConnected) {
        callback(false, {});
        return;
    }

    const uint32_t requestId = nextRequestId();
    std::vector<uint8_t> payload;
    ByteWriter writer(payload);
    writer.u32(requestId);
    writer.string(username);
    mStream->send(static_cast<uint16_t>(DbMessage::AccountLookupRequest), payload);

    registerHandler(requestId,
            [callback](bool ok, uint16_t type, const std::vector<uint8_t>& data) {
                if (!ok || type != static_cast<uint16_t>(DbMessage::AccountLookupResponse)) {
                    callback(false, {});
                    return;
                }
                ByteReader reader(data.data(), data.size());
                reader.u32();  // request id
                AccountLookup result;
                result.found = reader.u8() != 0;
                result.accountId = reader.u64();
                result.passwordHash = reader.string();
                callback(!reader.failed(), result);
            });
}

void DbClient::createAccount(const std::string& username, const std::string& passwordHash,
        std::function<void(bool, DbResult, uint64_t)> callback) {
    if (!mConnected) {
        callback(false, DbResult::Error, 0);
        return;
    }

    const uint32_t requestId = nextRequestId();
    std::vector<uint8_t> payload;
    ByteWriter writer(payload);
    writer.u32(requestId);
    writer.string(username);
    writer.string(passwordHash);
    mStream->send(static_cast<uint16_t>(DbMessage::AccountCreateRequest), payload);

    registerHandler(requestId,
            [callback](bool ok, uint16_t type, const std::vector<uint8_t>& data) {
                if (!ok || type != static_cast<uint16_t>(DbMessage::AccountCreateResponse)) {
                    callback(false, DbResult::Error, 0);
                    return;
                }
                ByteReader reader(data.data(), data.size());
                reader.u32();
                const auto result = static_cast<DbResult>(reader.u8());
                const uint64_t accountId = reader.u64();
                callback(!reader.failed(), result, accountId);
            });
}

void DbClient::deleteAccount(const std::string& username,
        std::function<void(bool, DbResult)> callback) {
    if (!mConnected) {
        callback(false, DbResult::Error);
        return;
    }

    const uint32_t requestId = nextRequestId();
    std::vector<uint8_t> payload;
    ByteWriter writer(payload);
    writer.u32(requestId);
    writer.string(username);
    mStream->send(static_cast<uint16_t>(DbMessage::AccountDeleteRequest), payload);

    registerHandler(requestId,
            [callback](bool ok, uint16_t type, const std::vector<uint8_t>& data) {
                if (!ok || type != static_cast<uint16_t>(DbMessage::AccountDeleteResponse)) {
                    callback(false, DbResult::Error);
                    return;
                }
                ByteReader reader(data.data(), data.size());
                reader.u32();
                const auto result = static_cast<DbResult>(reader.u8());
                callback(!reader.failed(), result);
            });
}

void DbClient::createSession(uint64_t accountId, const std::string& token, uint32_t ttlSeconds,
        std::function<void(bool, DbResult)> callback) {
    if (!mConnected) {
        callback(false, DbResult::Error);
        return;
    }

    const uint32_t requestId = nextRequestId();
    std::vector<uint8_t> payload;
    ByteWriter writer(payload);
    writer.u32(requestId);
    writer.u64(accountId);
    writer.string(token);
    writer.u32(ttlSeconds);
    mStream->send(static_cast<uint16_t>(DbMessage::SessionCreateRequest), payload);

    registerHandler(requestId,
            [callback](bool ok, uint16_t type, const std::vector<uint8_t>& data) {
                if (!ok || type != static_cast<uint16_t>(DbMessage::SessionCreateResponse)) {
                    callback(false, DbResult::Error);
                    return;
                }
                ByteReader reader(data.data(), data.size());
                reader.u32();
                const auto result = static_cast<DbResult>(reader.u8());
                callback(!reader.failed(), result);
            });
}

void DbClient::lookupSession(const std::string& token,
        std::function<void(bool, const SessionLookup&)> callback) {
    if (!mConnected) {
        callback(false, {});
        return;
    }

    const uint32_t requestId = nextRequestId();
    std::vector<uint8_t> payload;
    ByteWriter writer(payload);
    writer.u32(requestId);
    writer.string(token);
    mStream->send(static_cast<uint16_t>(DbMessage::SessionLookupRequest), payload);

    registerHandler(requestId,
            [callback](bool ok, uint16_t type, const std::vector<uint8_t>& data) {
                if (!ok || type != static_cast<uint16_t>(DbMessage::SessionLookupResponse)) {
                    callback(false, {});
                    return;
                }
                ByteReader reader(data.data(), data.size());
                reader.u32();
                SessionLookup result;
                result.found = reader.u8() != 0;
                result.accountId = reader.u64();
                result.username = reader.string();
                callback(!reader.failed(), result);
            });
}

void DbClient::deleteSession(const std::string& token,
        std::function<void(bool, DbResult)> callback) {
    if (!mConnected) {
        callback(false, DbResult::Error);
        return;
    }

    const uint32_t requestId = nextRequestId();
    std::vector<uint8_t> payload;
    ByteWriter writer(payload);
    writer.u32(requestId);
    writer.string(token);
    mStream->send(static_cast<uint16_t>(DbMessage::SessionDeleteRequest), payload);

    registerHandler(requestId,
            [callback](bool ok, uint16_t type, const std::vector<uint8_t>& data) {
                if (!ok || type != static_cast<uint16_t>(DbMessage::SessionDeleteResponse)) {
                    callback(false, DbResult::Error);
                    return;
                }
                ByteReader reader(data.data(), data.size());
                reader.u32();
                const auto result = static_cast<DbResult>(reader.u8());
                callback(!reader.failed(), result);
            });
}

}  // namespace serverutil
