#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include <net/framing.h>
#include <net/socket.h>

namespace serverutil {

// Single-threaded, poll-driven TCP server shared by all three services.
//
// One thread means no locking around game or session state, and at this scale
// poll() handles far more connections than we need. Connections are identified
// by an id that is never reused, so a stale id can't address a new client.
class TcpServer {
public:
    using ConnectionId = uint64_t;

    struct Callbacks {
        std::function<void(ConnectionId, const net::Address&)> onConnect;
        std::function<void(ConnectionId, uint16_t type, const std::vector<uint8_t>&)> onMessage;
        std::function<void(ConnectionId)> onDisconnect;
    };

    bool listen(uint16_t port, int backlog = 64);

    // Accepts new connections and dispatches whatever arrived. Blocks up to
    // timeoutMs waiting for activity.
    void poll(int timeoutMs);

    void send(ConnectionId id, uint16_t type, const std::vector<uint8_t>& payload);
    void closeConnection(ConnectionId id);

    const net::Address* peer(ConnectionId id) const;
    size_t connectionCount() const { return mConnections.size(); }

    Callbacks callbacks;

private:
    struct Connection {
        std::unique_ptr<net::MessageStream> stream;
        net::Address peer;
    };

    net::TcpSocket mListener;
    std::map<ConnectionId, Connection> mConnections;
    std::vector<ConnectionId> mClosing;
    ConnectionId mNextId = 1;
};

}  // namespace serverutil
