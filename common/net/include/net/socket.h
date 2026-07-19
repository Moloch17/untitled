#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace net {

#if defined(_WIN32)
using SocketHandle = uintptr_t;
#else
using SocketHandle = int;
#endif

constexpr SocketHandle kInvalidSocket = static_cast<SocketHandle>(-1);

// Call once at startup. No-op on POSIX; initialises Winsock on Windows.
bool initSockets();
void shutdownSockets();

// Last socket error as a human-readable string, for logging.
std::string lastSocketError();

// An IPv4 address/port pair, kept as a plain value so it can be used as a map
// key for tracking UDP peers.
struct Address {
    uint32_t ip = 0;  // host byte order
    uint16_t port = 0;

    bool operator==(const Address& other) const { return ip == other.ip && port == other.port; }
    bool operator<(const Address& other) const {
        return ip != other.ip ? ip < other.ip : port < other.port;
    }

    std::string toString() const;
    static bool resolve(const std::string& host, uint16_t port, Address* out);
};

// Non-blocking TCP socket. All calls report "would block" as success with zero
// bytes so a single-threaded poll loop can drive many connections.
class TcpSocket {
public:
    TcpSocket() = default;
    explicit TcpSocket(SocketHandle handle) : mHandle(handle) {}
    ~TcpSocket();

    TcpSocket(TcpSocket&& other) noexcept;
    TcpSocket& operator=(TcpSocket&& other) noexcept;
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    // Binds and starts listening. Sets SO_REUSEADDR so a restarted server
    // doesn't fail on a lingering TIME_WAIT socket.
    bool listen(uint16_t port, int backlog = 64);

    // Returns an invalid socket when there's nothing pending.
    TcpSocket accept(Address* peer = nullptr);

    // Starts a non-blocking connect. Completion is reported by the socket
    // becoming writable; check connectResult() then.
    bool connect(const Address& address);
    bool connectResult();

    // Returns false on a closed or broken connection. `bytes` is 0 when the
    // call would have blocked.
    bool receive(uint8_t* buffer, size_t size, size_t* bytes);
    bool send(const uint8_t* data, size_t size, size_t* bytes);

    bool valid() const { return mHandle != kInvalidSocket; }
    SocketHandle handle() const { return mHandle; }
    void close();

private:
    SocketHandle mHandle = kInvalidSocket;
};

// Non-blocking UDP socket.
class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();

    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    // Port 0 asks the OS for an ephemeral port, which is what clients want.
    bool open(uint16_t port);

    // Returns false only on a real error; sets `size` to 0 when no datagram is
    // waiting.
    bool receiveFrom(uint8_t* buffer, size_t capacity, size_t* size, Address* from);
    bool sendTo(const uint8_t* data, size_t size, const Address& to);

    bool valid() const { return mHandle != kInvalidSocket; }
    SocketHandle handle() const { return mHandle; }
    void close();

private:
    SocketHandle mHandle = kInvalidSocket;
};

// Waits until any of the given sockets is readable, or the timeout expires.
// Returns the number of ready sockets.
int waitReadable(const std::vector<SocketHandle>& sockets, int timeoutMs,
        std::vector<bool>* readable);

// Same, for writability. A non-blocking connect reports completion by becoming
// writable, so this is how a caller polls one without blocking a frame.
int waitWritable(const std::vector<SocketHandle>& sockets, int timeoutMs,
        std::vector<bool>* writable);

}  // namespace net
