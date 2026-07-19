// Command-line exerciser for the auth path, used to test the servers without
// running the game client.
//
//   authcli register <user> <pass>
//   authcli login <user> <pass>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <net/byte_stream.h>
#include <net/framing.h>
#include <net/protocol.h>
#include <net/socket.h>
#include <serverutil/config.h>

using namespace net;

namespace {

int usage() {
    std::fprintf(stderr,
            "usage: authcli <register|login> <username> <password>\n"
            "  AUTH_HOST / AUTH_SERVER_PORT override the target (default 127.0.0.1:7001)\n");
    return 2;
}

// Blocks until one message arrives or the timeout expires.
bool awaitMessage(MessageStream& stream, MessageStream::Message* out, int timeoutMs) {
    std::vector<SocketHandle> handles{stream.socket().handle()};
    std::vector<bool> ready;
    const int deadlineSlices = timeoutMs / 50;
    for (int i = 0; i < deadlineSlices; ++i) {
        if (stream.next(out)) {
            return true;
        }
        waitReadable(handles, 50, &ready);
        if (!stream.pump()) {
            return false;
        }
    }
    return stream.next(out);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        return usage();
    }

    const std::string command = argv[1];
    const std::string username = argv[2];
    const std::string password = argv[3];
    if (command != "register" && command != "login") {
        return usage();
    }

    if (!initSockets()) {
        std::fprintf(stderr, "socket init failed\n");
        return 1;
    }

    const std::string host = serverutil::envString("AUTH_HOST", "127.0.0.1");
    const uint16_t port = serverutil::envPort("AUTH_SERVER_PORT", 7001);

    Address address;
    if (!Address::resolve(host, port, &address)) {
        std::fprintf(stderr, "cannot resolve %s\n", host.c_str());
        return 1;
    }

    TcpSocket socket;
    if (!socket.connect(address)) {
        std::fprintf(stderr, "connect to %s failed: %s\n", address.toString().c_str(),
                lastSocketError().c_str());
        return 1;
    }

    std::vector<SocketHandle> handles{socket.handle()};
    std::vector<bool> ready;
    waitReadable(handles, 2000, &ready);
    if (!socket.connectResult()) {
        std::fprintf(stderr, "connect to %s failed\n", address.toString().c_str());
        return 1;
    }

    MessageStream stream(std::move(socket));

    std::vector<uint8_t> payload;
    ByteWriter writer(payload);
    writer.u32(kProtocolVersion);
    writer.string(username);
    writer.string(password);
    stream.send(static_cast<uint16_t>(command == "login" ? AuthMessage::LoginRequest
                                                         : AuthMessage::RegisterRequest),
            payload);

    MessageStream::Message message;
    if (!awaitMessage(stream, &message, 10000)) {
        std::fprintf(stderr, "no response from auth server\n");
        return 1;
    }

    ByteReader reader(message.payload.data(), message.payload.size());
    const auto result = static_cast<AuthResult>(reader.u8());

    if (command == "register") {
        std::printf("register: %s\n", toString(result));
        return result == AuthResult::Success ? 0 : 1;
    }

    const std::string token = reader.string();
    const std::string worldHost = reader.string();
    const uint16_t worldPort = reader.u16();
    const uint64_t accountId = reader.u64();

    if (reader.failed()) {
        std::fprintf(stderr, "malformed response\n");
        return 1;
    }

    std::printf("login: %s\n", toString(result));
    if (result == AuthResult::Success) {
        std::printf("  account id : %llu\n", static_cast<unsigned long long>(accountId));
        std::printf("  token      : %s\n", token.c_str());
        std::printf("  world      : %s:%u\n", worldHost.c_str(), worldPort);
    }

    shutdownSockets();
    return result == AuthResult::Success ? 0 : 1;
}
