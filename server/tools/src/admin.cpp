// Administrative command-line tool.
//
//   admin account create <name> <password>
//   admin account delete <name>          (asks for confirmation)
//
// This is a separate process from the servers on purpose. The confirmation
// prompt blocks on stdin, and a prompt has no business anywhere near a server's
// tick loop -- keeping it here makes blocking the simulation structurally
// impossible. The servers only ever see an asynchronous request.
//
// Authenticated with ADMIN_SECRET, which must match the auth server's.

#include <cstdio>
#include <cstring>
#include <iostream>
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
            "usage:\n"
            "  admin account create <name> <password>\n"
            "  admin account delete <name>\n"
            "\n"
            "environment:\n"
            "  ADMIN_SECRET       shared secret, must match the auth server\n"
            "  AUTH_HOST          auth server host (default 127.0.0.1)\n"
            "  AUTH_SERVER_PORT   auth server port (default 7001)\n");
    return 2;
}

bool awaitMessage(MessageStream& stream, MessageStream::Message* out, int timeoutMs) {
    std::vector<SocketHandle> handles{stream.socket().handle()};
    std::vector<bool> ready;
    for (int elapsed = 0; elapsed < timeoutMs; elapsed += 50) {
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

// Requires a full "yes"/"y"; anything else, including a bare Enter, aborts.
bool confirm(const std::string& prompt) {
    std::cout << prompt << " [y/N]: " << std::flush;
    std::string answer;
    if (!std::getline(std::cin, answer)) {
        return false;
    }
    for (char& c : answer) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return answer == "y" || answer == "yes";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3 || std::strcmp(argv[1], "account") != 0) {
        return usage();
    }

    const std::string action = argv[2];
    const bool creating = action == "create";
    const bool deleting = action == "delete";
    if ((creating && argc != 5) || (deleting && argc != 4) || (!creating && !deleting)) {
        return usage();
    }

    const std::string username = argv[3];
    const std::string password = creating ? argv[4] : "";

    const std::string secret = serverutil::envString("ADMIN_SECRET", "");
    if (secret.empty()) {
        std::fprintf(stderr, "ADMIN_SECRET is not set\n");
        return 1;
    }

    if (deleting) {
        std::cout << "This permanently deletes account '" << username
                  << "' and all of its sessions.\n";
        if (!confirm("Are you sure?")) {
            std::cout << "Aborted.\n";
            return 1;
        }
    }

    if (!initSockets()) {
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
        std::fprintf(stderr, "cannot reach auth server at %s\n", address.toString().c_str());
        return 1;
    }
    std::vector<SocketHandle> handles{socket.handle()};
    std::vector<bool> ready;
    waitReadable(handles, 2000, &ready);
    if (!socket.connectResult()) {
        std::fprintf(stderr, "cannot reach auth server at %s\n", address.toString().c_str());
        return 1;
    }

    MessageStream stream(std::move(socket));

    std::vector<uint8_t> payload;
    ByteWriter writer(payload);
    writer.string(secret);
    writer.string(username);
    if (creating) {
        writer.string(password);
    }
    stream.send(static_cast<uint16_t>(creating ? AuthMessage::AdminAccountCreateRequest
                                               : AuthMessage::AdminAccountDeleteRequest),
            payload);

    MessageStream::Message message;
    // Argon2id hashing is deliberately slow, so allow a generous window.
    if (!awaitMessage(stream, &message, 20000)) {
        std::fprintf(stderr, "no response from auth server\n");
        return 1;
    }

    ByteReader reader(message.payload.data(), message.payload.size());
    const auto result = static_cast<AuthResult>(reader.u8());

    if (result == AuthResult::Success) {
        std::printf("account %s: '%s'\n", creating ? "created" : "deleted", username.c_str());
    } else {
        std::fprintf(stderr, "account %s failed: %s\n", creating ? "create" : "delete",
                toString(result));
    }

    shutdownSockets();
    return result == AuthResult::Success ? 0 : 1;
}
