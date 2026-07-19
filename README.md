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

The client needs a Vulkan driver (`vulkan-radeon`, `vulkan-intel`, or the
NVIDIA driver) and, on Linux, a Wayland session:

    ./build/bin/client

The servers need a PostgreSQL database. The quickest way to get one, plus all
three servers, is Docker:

    docker compose up -d

That runs the host-built binaries in containers with `build/bin` and
`data/postgres` bind-mounted, so both stay visible on the host. Rebuilt a
server? Restart it to pick up the new binary:

    docker compose restart dbserver authserver worldserver

To run a server directly instead, point it at your own PostgreSQL:

    PGHOST=localhost PGDATABASE=untitled PGUSER=untitled PGPASSWORD=... \
        ./build/bin/dbserver

`server/sql/schema.sql` creates the tables. Create an account with:

    export ADMIN_SECRET=...        # must match the auth server's
    ./build/bin/admin account create <name> <password>

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
