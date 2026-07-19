#include "net/socket.h"

#include <cstdio>
#include <cstring>

#if defined(_WIN32)
    #include <winsock2.h>
    #include <ws2tcpip.h>
    using socklen_t = int;
    #define NET_ERRNO WSAGetLastError()
    #define NET_WOULDBLOCK WSAEWOULDBLOCK
    #define NET_INPROGRESS WSAEWOULDBLOCK
    // Winsock's codes are distinct from errno's: <errno.h> also defines
    // ECONNREFUSED, with a different value, so comparing against that one would
    // silently never match.
    #define NET_CONNREFUSED WSAECONNREFUSED
#else
    #include <arpa/inet.h>
    #include <errno.h>
    #include <fcntl.h>
    #include <netdb.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <poll.h>
    #include <sys/socket.h>
    #include <unistd.h>
    #define NET_ERRNO errno
    #define NET_WOULDBLOCK EWOULDBLOCK
    #define NET_INPROGRESS EINPROGRESS
    #define NET_CONNREFUSED ECONNREFUSED
#endif

namespace net {

namespace {

int closeHandle(SocketHandle handle) {
#if defined(_WIN32)
    return ::closesocket(handle);
#else
    return ::close(handle);
#endif
}

bool setNonBlocking(SocketHandle handle) {
#if defined(_WIN32)
    u_long mode = 1;
    return ::ioctlsocket(handle, FIONBIO, &mode) == 0;
#else
    const int flags = ::fcntl(handle, F_GETFL, 0);
    return flags != -1 && ::fcntl(handle, F_SETFL, flags | O_NONBLOCK) != -1;
#endif
}

bool wouldBlock() {
    const int error = NET_ERRNO;
    return error == NET_WOULDBLOCK
#if !defined(_WIN32)
            || error == EAGAIN
#endif
            ;
}

sockaddr_in toSockaddr(const Address& address) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(address.port);
    addr.sin_addr.s_addr = htonl(address.ip);
    return addr;
}

Address fromSockaddr(const sockaddr_in& addr) {
    Address out;
    out.ip = ntohl(addr.sin_addr.s_addr);
    out.port = ntohs(addr.sin_port);
    return out;
}

}  // namespace

bool initSockets() {
#if defined(_WIN32)
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
    return true;
#endif
}

void shutdownSockets() {
#if defined(_WIN32)
    WSACleanup();
#endif
}

std::string lastSocketError() {
#if defined(_WIN32)
    char buffer[256];
    std::snprintf(buffer, sizeof buffer, "winsock error %d", WSAGetLastError());
    return buffer;
#else
    return std::strerror(errno);
#endif
}

std::string Address::toString() const {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%u.%u.%u.%u:%u", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
            (ip >> 8) & 0xFF, ip & 0xFF, port);
    return buffer;
}

bool Address::resolve(const std::string& host, uint16_t port, Address* out) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* results = nullptr;
    if (::getaddrinfo(host.c_str(), nullptr, &hints, &results) != 0 || !results) {
        return false;
    }

    const auto* addr = reinterpret_cast<const sockaddr_in*>(results->ai_addr);
    out->ip = ntohl(addr->sin_addr.s_addr);
    out->port = port;
    ::freeaddrinfo(results);
    return true;
}

// ---------------------------------------------------------------------------
// TcpSocket
// ---------------------------------------------------------------------------

TcpSocket::~TcpSocket() { close(); }

TcpSocket::TcpSocket(TcpSocket&& other) noexcept : mHandle(other.mHandle) {
    other.mHandle = kInvalidSocket;
}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
    if (this != &other) {
        close();
        mHandle = other.mHandle;
        other.mHandle = kInvalidSocket;
    }
    return *this;
}

void TcpSocket::close() {
    if (mHandle != kInvalidSocket) {
        closeHandle(mHandle);
        mHandle = kInvalidSocket;
    }
}

bool TcpSocket::listen(uint16_t port, int backlog) {
    close();
    mHandle = ::socket(AF_INET, SOCK_STREAM, 0);
    if (mHandle == kInvalidSocket) {
        return false;
    }

    // Without this, restarting the server fails for as long as the previous
    // listening socket sits in TIME_WAIT.
    int reuse = 1;
    ::setsockopt(mHandle, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
            sizeof reuse);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (::bind(mHandle, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0
            || ::listen(mHandle, backlog) != 0 || !setNonBlocking(mHandle)) {
        close();
        return false;
    }
    return true;
}

TcpSocket TcpSocket::accept(Address* peer) {
    sockaddr_in addr{};
    socklen_t length = sizeof addr;
    const SocketHandle handle = ::accept(mHandle, reinterpret_cast<sockaddr*>(&addr), &length);
    if (handle == kInvalidSocket) {
        return TcpSocket();
    }
    if (peer) {
        *peer = fromSockaddr(addr);
    }
    setNonBlocking(handle);

    // Control messages are small and latency-sensitive; don't let Nagle delay
    // them waiting for more data.
    int noDelay = 1;
    ::setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay),
            sizeof noDelay);
    return TcpSocket(handle);
}

bool TcpSocket::connect(const Address& address) {
    close();
    mHandle = ::socket(AF_INET, SOCK_STREAM, 0);
    if (mHandle == kInvalidSocket) {
        return false;
    }
    setNonBlocking(mHandle);

    int noDelay = 1;
    ::setsockopt(mHandle, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay),
            sizeof noDelay);

    const sockaddr_in addr = toSockaddr(address);
    if (::connect(mHandle, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
        const int error = NET_ERRNO;
        if (error != NET_INPROGRESS && error != NET_WOULDBLOCK) {
            close();
            return false;
        }
    }
    return true;
}

bool TcpSocket::connectResult() {
    int error = 0;
    socklen_t length = sizeof error;
    if (::getsockopt(mHandle, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length)
            != 0) {
        return false;
    }
    return error == 0;
}

bool TcpSocket::receive(uint8_t* buffer, size_t size, size_t* bytes) {
    *bytes = 0;
    const auto received = ::recv(mHandle, reinterpret_cast<char*>(buffer),
            static_cast<int>(size), 0);
    if (received > 0) {
        *bytes = static_cast<size_t>(received);
        return true;
    }
    if (received == 0) {
        return false;  // orderly shutdown by the peer
    }
    return wouldBlock();
}

bool TcpSocket::send(const uint8_t* data, size_t size, size_t* bytes) {
    *bytes = 0;
    const auto sent = ::send(mHandle, reinterpret_cast<const char*>(data),
            static_cast<int>(size),
#if defined(MSG_NOSIGNAL)
            MSG_NOSIGNAL  // a disconnected peer must not kill the process
#else
            0
#endif
    );
    if (sent >= 0) {
        *bytes = static_cast<size_t>(sent);
        return true;
    }
    return wouldBlock();
}

// ---------------------------------------------------------------------------
// UdpSocket
// ---------------------------------------------------------------------------

UdpSocket::~UdpSocket() { close(); }

UdpSocket::UdpSocket(UdpSocket&& other) noexcept : mHandle(other.mHandle) {
    other.mHandle = kInvalidSocket;
}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept {
    if (this != &other) {
        close();
        mHandle = other.mHandle;
        other.mHandle = kInvalidSocket;
    }
    return *this;
}

void UdpSocket::close() {
    if (mHandle != kInvalidSocket) {
        closeHandle(mHandle);
        mHandle = kInvalidSocket;
    }
}

bool UdpSocket::open(uint16_t port) {
    close();
    mHandle = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (mHandle == kInvalidSocket) {
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (::bind(mHandle, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0
            || !setNonBlocking(mHandle)) {
        close();
        return false;
    }
    return true;
}

bool UdpSocket::receiveFrom(uint8_t* buffer, size_t capacity, size_t* size, Address* from) {
    *size = 0;
    sockaddr_in addr{};
    socklen_t length = sizeof addr;
    const auto received = ::recvfrom(mHandle, reinterpret_cast<char*>(buffer),
            static_cast<int>(capacity), 0, reinterpret_cast<sockaddr*>(&addr), &length);
    if (received >= 0) {
        *size = static_cast<size_t>(received);
        if (from) {
            *from = fromSockaddr(addr);
        }
        return true;
    }
    // An ICMP "port unreachable" from a peer that went away surfaces here on
    // some platforms; it must not take the socket down.
    return wouldBlock() || NET_ERRNO == NET_CONNREFUSED;
}

bool UdpSocket::sendTo(const uint8_t* data, size_t size, const Address& to) {
    const sockaddr_in addr = toSockaddr(to);
    const auto sent = ::sendto(mHandle, reinterpret_cast<const char*>(data),
            static_cast<int>(size), 0, reinterpret_cast<const sockaddr*>(&addr), sizeof addr);
    return sent >= 0 || wouldBlock();
}

// ---------------------------------------------------------------------------

namespace {

int waitFor(const std::vector<SocketHandle>& sockets, int timeoutMs, std::vector<bool>* ready,
        bool forWrite) {
    ready->assign(sockets.size(), false);
    if (sockets.empty()) {
        return 0;
    }

#if defined(_WIN32)
    fd_set set;
    fd_set exceptions;
    FD_ZERO(&set);
    FD_ZERO(&exceptions);
    for (SocketHandle handle : sockets) {
        FD_SET(handle, &set);
        FD_SET(handle, &exceptions);
    }
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    // Winsock reports a *failed* non-blocking connect in the exception set
    // rather than the write set, so a caller polling only for writability would
    // wait for a completion that never arrives.
    const int count = ::select(0, forWrite ? nullptr : &set, forWrite ? &set : nullptr,
            forWrite ? &exceptions : nullptr, timeoutMs < 0 ? nullptr : &tv);
    if (count > 0) {
        for (size_t i = 0; i < sockets.size(); ++i) {
            (*ready)[i] = FD_ISSET(sockets[i], &set) != 0
                    || (forWrite && FD_ISSET(sockets[i], &exceptions) != 0);
        }
    }
    return count;
#else
    std::vector<pollfd> fds(sockets.size());
    for (size_t i = 0; i < sockets.size(); ++i) {
        fds[i].fd = sockets[i];
        fds[i].events = forWrite ? POLLOUT : POLLIN;
    }
    const int count = ::poll(fds.data(), fds.size(), timeoutMs);
    if (count > 0) {
        const short mask = static_cast<short>((forWrite ? POLLOUT : POLLIN) | POLLHUP | POLLERR);
        for (size_t i = 0; i < sockets.size(); ++i) {
            (*ready)[i] = (fds[i].revents & mask) != 0;
        }
    }
    return count;
#endif
}

}  // namespace

int waitReadable(const std::vector<SocketHandle>& sockets, int timeoutMs,
        std::vector<bool>* readable) {
    return waitFor(sockets, timeoutMs, readable, /*forWrite=*/false);
}

int waitWritable(const std::vector<SocketHandle>& sockets, int timeoutMs,
        std::vector<bool>* writable) {
    return waitFor(sockets, timeoutMs, writable, /*forWrite=*/true);
}

}  // namespace net
