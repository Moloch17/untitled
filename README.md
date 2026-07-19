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

The servers need a PostgreSQL database. The quickest way to get one, plus all
three servers, is Docker:

    docker compose up -d

Wait a few seconds for the database to report healthy:

    docker compose ps

On the very first run this creates the database and applies
`server/sql/schema.sql`, which lives in `data/postgres` on the host. That
directory is **not** in the repository, so a fresh clone always starts with an
empty database and no accounts -- you cannot log in until you create one.

Rebuilt a server? A running container keeps using the binary it started with,
so restart it to pick up the new one:

    docker compose restart dbserver authserver worldserver

### 2. Create an account

Account management goes through the `admin` tool, which authenticates with a
shared secret that must match the auth server's. Compose sets it to
`change-me-in-dot-env` unless you override it:

    export ADMIN_SECRET=change-me-in-dot-env
    ./build/bin/admin account create <name> <password>

To use your own secret and database password, create a `.env` file next to
`compose.yaml` -- Compose reads it automatically:

    ADMIN_SECRET=something-private
    POSTGRES_PASSWORD=something-private

then `docker compose up -d` again, and export the same `ADMIN_SECRET` in the
shell you run `admin` from.

Deleting an account asks for confirmation and takes its sessions with it:

    ./build/bin/admin account delete <name>

### 3. Run the client

The client needs a Vulkan driver (`vulkan-radeon`, `vulkan-intel`, or the
NVIDIA driver) and, on Linux, a Wayland session:

    ./build/bin/client

It opens on a login screen and connects to `127.0.0.1:7001` by default. Sign in
with the account from step 2. To point it at a server elsewhere:

    AUTH_HOST=192.0.2.10 AUTH_SERVER_PORT=7001 ./build/bin/client

If the servers aren't running, the login screen reports `cannot reach auth
server` rather than failing silently.

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
    DB_SERVER_HOST=localhost ADMIN_SECRET=... ./build/bin/authserver
    DB_SERVER_HOST=localhost ./build/bin/worldserver

Ports default to 7000 (dbserver), 7001 (authserver) and 7002/7003 TCP/UDP
(worldserver); see `compose.yaml` for every variable.

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
