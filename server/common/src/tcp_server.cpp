#include "serverutil/tcp_server.h"

namespace serverutil {

bool TcpServer::listen(uint16_t port, int backlog) {
    return mListener.listen(port, backlog);
}

void TcpServer::poll(int timeoutMs) {
    // Poll the listener alongside every live connection, so an idle server
    // sleeps instead of spinning.
    std::vector<net::SocketHandle> handles;
    std::vector<ConnectionId> ids;
    handles.push_back(mListener.handle());
    for (const auto& [id, connection] : mConnections) {
        handles.push_back(connection.stream->socket().handle());
        ids.push_back(id);
    }

    std::vector<bool> readable;
    net::waitReadable(handles, timeoutMs, &readable);

    if (!readable.empty() && readable[0]) {
        // Drain the accept queue: poll() only reports readiness once, so a
        // burst of connections must all be taken now.
        for (;;) {
            net::Address peer;
            net::TcpSocket socket = mListener.accept(&peer);
            if (!socket.valid()) {
                break;
            }
            const ConnectionId id = mNextId++;
            Connection connection;
            connection.stream = std::make_unique<net::MessageStream>(std::move(socket));
            connection.peer = peer;
            mConnections.emplace(id, std::move(connection));
            if (callbacks.onConnect) {
                callbacks.onConnect(id, peer);
            }
        }
    }

    for (size_t i = 0; i < ids.size(); ++i) {
        if (!readable[i + 1]) {
            continue;
        }
        const ConnectionId id = ids[i];
        auto it = mConnections.find(id);
        if (it == mConnections.end()) {
            continue;  // closed by a callback earlier in this pass
        }

        net::MessageStream& stream = *it->second.stream;
        if (!stream.pump()) {
            closeConnection(id);
            continue;
        }

        net::MessageStream::Message message;
        while (stream.next(&message)) {
            if (callbacks.onMessage) {
                callbacks.onMessage(id, message.type, message.payload);
            }
            // The handler may have closed this connection.
            if (mConnections.find(id) == mConnections.end()) {
                break;
            }
        }
    }

    // Deferred so a connection is never destroyed while its own callback is
    // still on the stack.
    for (ConnectionId id : mClosing) {
        auto it = mConnections.find(id);
        if (it != mConnections.end()) {
            if (callbacks.onDisconnect) {
                callbacks.onDisconnect(id);
            }
            mConnections.erase(it);
        }
    }
    mClosing.clear();
}

void TcpServer::send(ConnectionId id, uint16_t type, const std::vector<uint8_t>& payload) {
    auto it = mConnections.find(id);
    if (it == mConnections.end()) {
        return;
    }
    it->second.stream->send(type, payload);
}

void TcpServer::closeConnection(ConnectionId id) {
    mClosing.push_back(id);
}

const net::Address* TcpServer::peer(ConnectionId id) const {
    auto it = mConnections.find(id);
    return it == mConnections.end() ? nullptr : &it->second.peer;
}

}  // namespace serverutil
