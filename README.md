# untitled

Game client and dedicated servers. Builds on Linux and Windows.

## Get the source

The renderer and windowing library are submodules, so clone recursively:

    git clone --recurse-submodules <url>
    cd untitled

If you already cloned without that flag, configuring will fetch them for you.

## Linux

Install the toolchain and libraries:

    # Arch
    sudo pacman -S --needed clang cmake ninja git libc++ libc++abi \
        wayland libxkbcommon libx11 libxrandr libxinerama libxcursor libxi \
        libxcomposite libxxf86vm glu postgresql-libs libsodium

    # Debian / Ubuntu
    sudo apt install clang cmake ninja-build git libc++-dev libc++abi-dev \
        libwayland-dev libwayland-bin libxkbcommon-dev libx11-dev libxrandr-dev \
        libxinerama-dev libxcursor-dev libxi-dev libxcomposite-dev \
        libxxf86vm-dev libglu1-mesa-dev libpq-dev libsodium-dev

    # Fedora
    sudo dnf install clang cmake ninja-build git libcxx-devel \
        wayland-devel libxkbcommon-devel libX11-devel libXrandr-devel \
        libXinerama-devel libXcursor-devel libXi-devel libXcomposite-devel \
        libXxf86vm-devel mesa-libGLU-devel libpq-devel libsodium-devel

Then build:

    cmake --preset linux
    cmake --build --preset linux

Binaries land in `build/bin/`. The first build takes a while -- it compiles
Filament from source.

Configuring checks every dependency up front and, if any are missing, prints a
single install command for your distribution rather than failing partway
through.

## Windows

Install [Visual Studio 2022](https://visualstudio.microsoft.com/) with the
"Desktop development with C++" workload, plus
[CMake](https://cmake.org/download/) and [Git](https://git-scm.com/download/win).

The third-party libraries come from [vcpkg](https://vcpkg.io):

    git clone https://github.com/microsoft/vcpkg C:\vcpkg
    C:\vcpkg\bootstrap-vcpkg.bat
    setx VCPKG_ROOT C:\vcpkg

Open a new terminal so `VCPKG_ROOT` is set, then build:

    cmake --preset windows
    cmake --build --preset windows

`vcpkg.json` lists what gets fetched, so this needs no manual package
installation. Binaries land in `build\bin\Release\`.

## Build presets

| Preset | What it builds |
| --- | --- |
| `linux` | Everything, Release |
| `linux-debug` | Everything, Debug |
| `linux-client` | Client only -- skips the servers, so Postgres and libsodium aren't needed |
| `windows` | Everything, via vcpkg |
| `windows-client` | Client only -- no vcpkg required |

To build a single target:

    cmake --build --preset linux --target client

Targets: `client`, `dbserver`, `authserver`, `worldserver`, and the command-line
tools `admin`, `authcli`, `worldcli`.

## Running

### 1. Start the servers

The servers need a PostgreSQL database. Docker brings up that and all three
servers:

    docker compose up

Your terminal is attached to the world server, which offers a console prompt.
The other services run alongside it but their output is kept out of the way;
see them with `docker compose logs dbserver` and so on. Use `docker compose up
-d` if you would rather not have the prompt.

On the very first run this creates the database and applies
`server/sql/schema.sql`. The data lives in `data/postgres` on the host, which is
**not** in the repository, so a fresh clone always starts with an empty database
and no accounts.

Rebuilt a server? A running container keeps using the binary it started with, so
restart it to pick up the new one:

    docker compose restart dbserver authserver worldserver

### 2. Create an account

At the world server console:

    > account create yourname yourpassword 2

The trailing number is the permission level: 0 player, 1 game master, 2 admin.
Leave it off for an ordinary player account.

Commands typed at this console need no account of their own. Reaching the
server's stdin already means control of the process, so asking it to
authenticate would guard nothing. It is also the *only* way to create the first
account -- there is no network path to it and no seeded account with a known
password. A database with no admin says so in the auth server's log and points
at this command.

### 3. Run the client

The client needs a Vulkan driver (`vulkan-radeon`, `vulkan-intel`, or the
NVIDIA driver) and, on Linux, a Wayland session:

    ./build/bin/client

It opens on a login screen and connects to `127.0.0.1:7001` by default. Sign in
with the account from step 2. To point it at a server elsewhere:

    AUTH_HOST=192.0.2.10 AUTH_SERVER_PORT=7001 ./build/bin/client

If the servers aren't running, the login screen reports `cannot reach auth
server` rather than failing silently.

## The server console

Typed at the world server (`docker compose up`), or remotely with
`./build/bin/console`:

    account create <name> <password> [level]   0 player, 1 gm, 2 admin
    account delete <name>
    account level <name> <0|1|2>
    time                                       show the world clock
    time set <HH:MM>                           set the clock (24 hour)
    time speed <multiplier>                    1 = real time, 24 = 1 hour day
    time reset                                 real time, at the real time
    latency set <milliseconds>                 simulated lag, 0 turns it off
    status                                     clock, players, tick, latency
    help

`time speed` multiplies how fast the world clock runs; changing it never makes
the clock jump, because the new rate applies from the moment it is set.
`latency set` delays packets in both directions on the world server, so 150 is
roughly a 300ms round trip for everyone connected, capped at 2000.

### Administering from another machine

The standalone console talks to the servers over TCP:

    ./serve.sh                             # start the servers, then a prompt
    ./build/bin/console                    # against servers already running
    ./build/bin/console time set 20:30     # one command and exit

Unlike the server's own stdin, this signs in with an account and every command
is authorised against that account's permission level. Account management needs
admin (2); the clock and latency need game master (1) or higher. The servers log
which account performed each action, and refusals name the account refused.

For scripting, `CONSOLE_USER` and `CONSOLE_PASSWORD` skip the prompt. Without a
terminal and without those set, it exits rather than waiting on a prompt nobody
can answer.

### Checking the servers without the client

Two command-line tools exercise the same paths, which is quicker than launching
the game when you're changing server code:

    ./build/bin/authcli login <name> <password>       # auth only
    ./build/bin/worldcli <name> <password> 5          # login, join, snapshots
    ./build/bin/worldcli <name> <password> 5 move     # ...while walking forward
    ./build/bin/worldcli <name> <password> 5 jump     # ...while jumping

### Running a server outside Docker

Each server is a plain executable configured by environment variables. It needs
a PostgreSQL you provide yourself, with `server/sql/schema.sql` already applied:

    PGHOST=localhost PGDATABASE=untitled PGUSER=untitled PGPASSWORD=... \
        ./build/bin/dbserver
    DB_SERVER_HOST=localhost ./build/bin/authserver
    DB_SERVER_HOST=localhost ./build/bin/worldserver

Ports default to 7000 (dbserver), 7001 (authserver) and 7002/7003 TCP/UDP
(worldserver).

The world clock follows the server's local wall-clock time, so in-game noon is
real noon. Compose mounts `/etc/localtime` into the world server for that;
without it a container runs on UTC. `TIME_SPEED` (or `DAY_LENGTH_SECONDS`) sets
an accelerated cycle from startup, which is what you want when working on the
lighting. See `compose.yaml` for every variable.

## Updating dependencies

Filament and GLFW are pinned to specific commits and never move on their own.
To update one deliberately:

    cd client/deps/glfw
    git fetch origin
    git checkout <commit-or-tag>
    cd ../../..
    git add client/deps/glfw
    git commit -m "Update GLFW to <version>"

Avoid `git submodule update --remote`, which moves them to their upstream branch
tips without recording your intent.
