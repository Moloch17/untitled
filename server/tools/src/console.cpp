// Interactive server console.
//
//   console                      -- prompt
//   console "time set 20:30"     -- run one command and exit
//
// This is a separate process on purpose. A prompt blocks on stdin, and stdin
// has no business anywhere near a 60Hz tick loop, so the console talks to the
// servers over TCP exactly as any other client does. Blocking here cannot stall
// the simulation.
//
// Account commands go to the auth server, which owns password hashing; world
// commands go to the world server, which owns the clock. Both authorise against
// the permission level of the account you log in as -- there is no shared
// secret, and the servers log which account performed each action.

#include <cstdio>

#if defined(_WIN32)
    #include <io.h>
    #define isatty _isatty
    #define fileno _fileno
#else
    #include <unistd.h>
#endif
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <net/byte_stream.h>
#include <net/framing.h>
#include <net/protocol.h>
#include <net/socket.h>
#include <serverutil/config.h>

using namespace net;

namespace {

std::string gToken;       // this console session
std::string gUser;
std::string gPassword;    // kept to re-authenticate when the session expires
uint8_t gLevel = 0;
std::string gAuthHost;
uint16_t gAuthPort = 0;
std::string gWorldHost;
uint16_t gWorldPort = 0;

bool connectTo(const std::string& host, uint16_t port, TcpSocket* socket) {
    Address address;
    if (!Address::resolve(host, port, &address) || !socket->connect(address)) {
        return false;
    }
    std::vector<SocketHandle> handles{socket->handle()};
    std::vector<bool> ready;
    waitReadable(handles, 3000, &ready);
    return socket->connectResult();
}

// Sends one request and waits for the reply. Connections are per-command:
// the console is idle almost all of the time, and holding sockets open just to
// save a millisecond would mean reconnect logic for no benefit.
bool request(const std::string& host, uint16_t port, uint16_t type,
        const std::vector<uint8_t>& payload, MessageStream::Message* reply) {
    TcpSocket socket;
    if (!connectTo(host, port, &socket)) {
        std::printf("  cannot reach %s:%u\n", host.c_str(), port);
        return false;
    }
    MessageStream stream(std::move(socket));
    stream.send(type, payload);

    std::vector<SocketHandle> handles{stream.socket().handle()};
    std::vector<bool> ready;
    for (int waited = 0; waited < 20000; waited += 50) {
        if (stream.next(reply)) {
            return true;
        }
        waitReadable(handles, 50, &ready);
        if (!stream.pump()) {
            break;
        }
    }
    std::printf("  no response\n");
    return false;
}

bool login();

void accountCreate(const std::string& name, const std::string& password, uint8_t level) {
    std::vector<uint8_t> payload;
    ByteWriter writer(payload);
    writer.string(gToken);
    writer.string(name);
    writer.string(password);
    writer.u8(level);

    MessageStream::Message reply;
    if (!request(gAuthHost, gAuthPort,
                static_cast<uint16_t>(AuthMessage::AdminAccountCreateRequest), payload, &reply)) {
        return;
    }
    ByteReader reader(reply.payload.data(), reply.payload.size());
    const auto result = static_cast<AuthResult>(reader.u8());
    if (result != AuthResult::Success) {
        std::printf("  failed: %s\n", toString(result));
        return;
    }
    std::printf("  created account '%s'\n", name.c_str());
}

void accountDelete(const std::string& name, bool assumeYes) {
    if (!assumeYes) {
        std::printf("  permanently delete account '%s' and its sessions? [y/N]: ", name.c_str());
        std::fflush(stdout);
        std::string answer;
        if (!std::getline(std::cin, answer) || (answer != "y" && answer != "Y"
                    && answer != "yes")) {
            std::printf("  aborted\n");
            return;
        }
    }

    std::vector<uint8_t> payload;
    ByteWriter writer(payload);
    writer.string(gToken);
    writer.string(name);

    MessageStream::Message reply;
    if (!request(gAuthHost, gAuthPort,
                static_cast<uint16_t>(AuthMessage::AdminAccountDeleteRequest), payload, &reply)) {
        return;
    }
    ByteReader reader(reply.payload.data(), reply.payload.size());
    const auto result = static_cast<AuthResult>(reader.u8());
    std::printf(result == AuthResult::Success ? "  deleted account '%s'\n" : "  failed: %s\n",
            result == AuthResult::Success ? name.c_str() : toString(result));
}

void accountSetLevel(const std::string& name, uint8_t level) {
    std::vector<uint8_t> payload;
    ByteWriter writer(payload);
    writer.string(gToken);
    writer.string(name);
    writer.u8(level);

    MessageStream::Message reply;
    if (!request(gAuthHost, gAuthPort,
                static_cast<uint16_t>(AuthMessage::AdminSetLevelRequest), payload, &reply)) {
        return;
    }
    ByteReader reader(reply.payload.data(), reply.payload.size());
    const auto result = static_cast<AuthResult>(reader.u8());
    if (result == AuthResult::Success) {
        static const char* names[] = {"player", "game master", "admin"};
        std::printf("  '%s' is now %s (%u)\n", name.c_str(), names[level < 3 ? level : 0], level);
    } else {
        std::printf("  failed: %s\n", toString(result));
    }
}

void latencyCommand(bool set, uint32_t milliseconds) {
    std::vector<uint8_t> payload;
    ByteWriter writer(payload);
    writer.string(gToken);
    writer.u32(milliseconds);

    MessageStream::Message reply;
    if (!request(gWorldHost, gWorldPort,
                static_cast<uint16_t>(WorldMessage::AdminSetLatencyRequest), payload, &reply)) {
        return;
    }
    ByteReader reader(reply.payload.data(), reply.payload.size());
    const uint8_t result = reader.u8();
    const uint32_t current = reader.u32();
    if (result == 2) {
        std::printf("  refused: your account lacks permission (game master or higher)\n");
        return;
    }
    if (result != 0) {
        std::printf("  refused: session expired -- restart the console\n");
        return;
    }
    (void) set;
    if (current == 0) {
        std::printf("  simulated latency off\n");
    } else {
        std::printf("  simulated latency %u ms each way (about %u ms round trip)\n", current,
                current * 2);
    }
}

void printClock(float timeOfDay, float speed) {
    const int minutes = static_cast<int>(timeOfDay * 24.0f * 60.0f + 0.5f) % (24 * 60);
    std::printf("  world clock is %02d:%02d", minutes / 60, minutes % 60);
    if (speed == 1.0f) {
        std::printf(" (real time)\n");
    } else if (speed == 0.0f) {
        std::printf(" (frozen)\n");
    } else {
        // A day takes 24h / speed.
        const float dayMinutes = 24.0f * 60.0f / speed;
        if (dayMinutes >= 1.0f) {
            std::printf(" (%.4gx real time, %.0f minute day)\n", speed, dayMinutes);
        } else {
            std::printf(" (%.4gx real time, %.0f second day)\n", speed, dayMinutes * 60.0f);
        }
    }
}

// 24 hour HH:MM. A bare fraction is also accepted, which is what the wire
// format carries and is occasionally handy for scripting.
bool parseTimeOfDay(const std::string& text, float* out) {
    const size_t colon = text.find(':');
    if (colon == std::string::npos) {
        char* end = nullptr;
        const float value = std::strtof(text.c_str(), &end);
        if (end == text.c_str() || value < 0.0f || value >= 1.0f) {
            return false;
        }
        *out = value;
        return true;
    }
    const int hours = std::atoi(text.substr(0, colon).c_str());
    const int minutes = std::atoi(text.substr(colon + 1).c_str());
    if (hours < 0 || hours > 23 || minutes < 0 || minutes > 59) {
        return false;
    }
    *out = static_cast<float>(hours * 60 + minutes) / (24.0f * 60.0f);
    return true;
}

void timeCommand(TimeCommand command, float value) {
    std::vector<uint8_t> payload;
    ByteWriter writer(payload);
    writer.string(gToken);
    writer.u8(static_cast<uint8_t>(command));
    writer.f32(value);

    MessageStream::Message reply;
    if (!request(gWorldHost, gWorldPort,
                static_cast<uint16_t>(WorldMessage::AdminSetTimeRequest), payload, &reply)) {
        return;
    }
    ByteReader reader(reply.payload.data(), reply.payload.size());
    const uint8_t result = reader.u8();
    const float now = reader.f32();
    const float speed = reader.f32();
    if (result == 2) {
        std::printf("  refused: your account lacks permission (game master or higher)\n");
        return;
    }
    if (result != 0) {
        std::printf("  refused: session expired -- restart the console\n");
        return;
    }
    printClock(now, speed);
}

void status() {
    std::vector<uint8_t> payload;
    ByteWriter writer(payload);
    writer.string(gToken);

    MessageStream::Message reply;
    if (!request(gWorldHost, gWorldPort,
                static_cast<uint16_t>(WorldMessage::AdminStatusRequest), payload, &reply)) {
        return;
    }
    ByteReader reader(reply.payload.data(), reply.payload.size());
    const uint8_t result = reader.u8();
    const float timeOfDay = reader.f32();
    const float speed = reader.f32();
    const uint16_t players = reader.u16();
    const uint32_t tick = reader.u32();
    const uint32_t latencyMs = reader.u32();
    if (result == 2) {
        std::printf("  refused: your account lacks permission (game master or higher)\n");
        return;
    }
    if (result != 0) {
        std::printf("  refused: session expired -- restart the console\n");
        return;
    }
    printClock(timeOfDay, speed);
    std::printf("  players online: %u\n  tick: %u\n", players, tick);
    if (latencyMs > 0) {
        std::printf("  simulated latency: %u ms each way\n", latencyMs);
    }
}

// Authenticates and stores the session token used by every command below.
bool login() {
    std::vector<uint8_t> payload;
    ByteWriter writer(payload);
    writer.u32(kProtocolVersion);
    writer.string(gUser);
    writer.string(gPassword);

    MessageStream::Message reply;
    if (!request(gAuthHost, gAuthPort, static_cast<uint16_t>(AuthMessage::LoginRequest), payload,
                &reply)) {
        return false;
    }
    ByteReader reader(reply.payload.data(), reply.payload.size());
    const auto result = static_cast<AuthResult>(reader.u8());
    gToken = reader.string();
    reader.string();  // world host
    reader.u16();     // world port
    reader.u64();     // account id
    gLevel = reader.u8();

    if (result != AuthResult::Success) {
        std::printf("login failed: %s\n", toString(result));
        return false;
    }
    static const char* names[] = {"player", "game master", "admin"};
    std::printf("signed in as %s (%s)\n", gUser.c_str(), names[gLevel < 3 ? gLevel : 0]);
    if (gLevel < static_cast<uint8_t>(PermissionLevel::GameMaster)) {
        std::printf("note: this account has no privileges; commands will be refused.\n");
    }
    return true;
}

void help() {
    std::printf(
            "  account create <name> <password> [level]   create an account\n"
            "  account delete <name> [--yes]              delete an account\n"
            "  account level <name> <0|1|2>               0 player, 1 gm, 2 admin\n"
            "  time                               show the world clock\n"
            "  time set <HH:MM>                   set the clock (24 hour)\n"
            "  time speed <multiplier>            1 = real time, 24 = 1 hour day\n"
            "  time reset                         real time, at the real time of day\n"
            "  latency                            show simulated latency\n"
            "  latency set <milliseconds>         0 turns it off\n"
            "  status                             clock, players online, tick\n"
            "  help                               this list\n"
            "  quit                               leave the console\n");
}

// Returns false when the console should exit.
bool runCommand(const std::string& line) {
    std::istringstream stream(line);
    std::vector<std::string> words;
    for (std::string word; stream >> word;) {
        words.push_back(word);
    }
    if (words.empty()) {
        return true;
    }

    const std::string& command = words[0];
    if (command == "quit" || command == "exit") {
        return false;
    }
    if (command == "help" || command == "?") {
        help();
    } else if (command == "status") {
        status();
    } else if (command == "time") {
        if (words.size() == 1) {
            status();
        } else if (words[1] == "reset") {
            timeCommand(TimeCommand::Reset, 0.0f);
        } else if (words[1] == "set" && words.size() >= 3) {
            float timeOfDay = 0.0f;
            if (!parseTimeOfDay(words[2], &timeOfDay)) {
                std::printf("  expected HH:MM in 24 hour form, e.g. 'time set 20:30'\n");
            } else {
                timeCommand(TimeCommand::Set, timeOfDay);
            }
        } else if (words[1] == "speed" && words.size() >= 3) {
            char* end = nullptr;
            const float speed = std::strtof(words[2].c_str(), &end);
            if (end == words[2].c_str() || speed < 0.0f) {
                std::printf("  expected a multiplier, e.g. 'time speed 24' for a one hour day\n");
            } else {
                timeCommand(TimeCommand::Speed, speed);
            }
        } else {
            std::printf("  usage: time\n"
                        "         time set <HH:MM>\n"
                        "         time speed <multiplier>\n"
                        "         time reset\n");
        }
    } else if (command == "latency") {
        if (words.size() == 1) {
            // Reading it back is a set to whatever it already is, which the
            // server answers with the current value.
            status();
        } else if (words[1] == "set" && words.size() >= 3) {
            latencyCommand(true, static_cast<uint32_t>(std::atoi(words[2].c_str())));
        } else {
            std::printf("  usage: latency | latency set <milliseconds>\n");
        }
    } else if (command == "account") {
        const bool assumeYes = words.size() > 0 && words.back() == "--yes";
        if (words.size() >= 4 && words[1] == "create") {
            const uint8_t level = words.size() >= 5
                    ? static_cast<uint8_t>(std::atoi(words[4].c_str()))
                    : 0;
            accountCreate(words[2], words[3], level);
        } else if (words.size() >= 3 && words[1] == "delete") {
            accountDelete(words[2], assumeYes);
        } else if (words.size() >= 4 && words[1] == "level") {
            accountSetLevel(words[2], static_cast<uint8_t>(std::atoi(words[3].c_str())));
        } else {
            std::printf("  usage: account create <name> <password> [level]\n"
                        "         account delete <name>\n"
                        "         account level <name> <0|1|2>\n");
        }
    } else {
        std::printf("  unknown command '%s' -- try 'help'\n", command.c_str());
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (!initSockets()) {
        return 1;
    }

    gAuthHost = serverutil::envString("AUTH_HOST", "127.0.0.1");
    gAuthPort = serverutil::envPort("AUTH_SERVER_PORT", 7001);
    gWorldHost = serverutil::envString("WORLD_HOST", "127.0.0.1");
    gWorldPort = serverutil::envPort("WORLD_SERVER_PORT", 7002);

    // Credentials may come from the environment (for scripts) or the prompt.
    gUser = serverutil::envString("CONSOLE_USER", "");
    gPassword = serverutil::envString("CONSOLE_PASSWORD", "");
    // Fail fast rather than blocking on a prompt nobody can answer: without a
    // terminal and without credentials in the environment, there is no way to
    // sign in, and hanging there looks like the server is unreachable.
    if ((gUser.empty() || gPassword.empty()) && !isatty(fileno(stdin))) {
        std::fprintf(stderr,
                "no terminal to prompt on; set CONSOLE_USER and CONSOLE_PASSWORD\n");
        return 1;
    }

    if (gUser.empty()) {
        std::printf("account: ");
        std::fflush(stdout);
        if (!std::getline(std::cin, gUser)) {
            return 1;
        }
    }
    if (gPassword.empty()) {
        // No terminal echo control here; this is a local admin tool, and
        // CONSOLE_PASSWORD exists for anything scripted.
        std::printf("password: ");
        std::fflush(stdout);
        if (!std::getline(std::cin, gPassword)) {
            return 1;
        }
    }

    if (!login()) {
        shutdownSockets();
        return 1;
    }

    // One-shot mode, for scripts: everything after argv[0] is the command.
    if (argc > 1) {
        std::string line;
        for (int i = 1; i < argc; ++i) {
            line += (i > 1 ? " " : "");
            line += argv[i];
        }
        runCommand(line);
        shutdownSockets();
        return 0;
    }

    std::printf("untitled server console. 'help' for commands, 'quit' to leave.\n");
    status();

    for (;;) {
        std::printf("> ");
        std::fflush(stdout);
        std::string line;
        if (!std::getline(std::cin, line)) {
            break;  // EOF, e.g. piped input or Ctrl-D
        }
        if (!runCommand(line)) {
            break;
        }
    }

    shutdownSockets();
    return 0;
}
