# Dependency preflight.
#
# Every check reports into one consolidated message rather than failing at the
# first problem, so a fresh contributor gets a single list of what to install
# instead of discovering them one build at a time.

set(MISSING_ARCH "")
set(MISSING_DEBIAN "")
set(MISSING_FEDORA "")
set(MISSING_NOTES "")

function(untitled_require condition arch debian fedora)
    if (NOT ${condition})
        set(MISSING_ARCH "${MISSING_ARCH};${arch}" PARENT_SCOPE)
        set(MISSING_DEBIAN "${MISSING_DEBIAN};${debian}" PARENT_SCOPE)
        set(MISSING_FEDORA "${MISSING_FEDORA};${fedora}" PARENT_SCOPE)
    endif()
endfunction()

# --------------------------------------------------------------------------
# Vendored dependencies
#
# Filament and GLFW are submodules. A fresh clone without --recurse-submodules
# leaves them as empty directories, which would otherwise fail as a confusing
# "add_subdirectory: directory does not contain CMakeLists.txt".
# --------------------------------------------------------------------------
if (NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/client/deps/filament/CMakeLists.txt
        OR NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/client/deps/glfw/CMakeLists.txt)
    find_package(Git QUIET)
    if (GIT_FOUND AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/.git)
        message(STATUS "Fetching submodules (Filament, GLFW) -- this takes a few minutes")
        execute_process(
            COMMAND ${GIT_EXECUTABLE} submodule update --init --recursive
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            RESULT_VARIABLE submodule_result)
        if (NOT submodule_result EQUAL 0)
            message(FATAL_ERROR
                "Failed to fetch submodules. Run this by hand:\n"
                "  git submodule update --init --recursive")
        endif()
    else()
        message(FATAL_ERROR
            "The Filament and GLFW submodules are missing, and this isn't a git\n"
            "checkout so they can't be fetched automatically. Clone with:\n"
            "  git clone --recurse-submodules <url>")
    endif()
endif()

# --------------------------------------------------------------------------
# Toolchain
# --------------------------------------------------------------------------
if (UNIX AND NOT APPLE)
    if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR
            "Filament requires clang on Linux (GCC is unsupported upstream).\n"
            "Use the preset, which selects it for you:\n"
            "  cmake --preset default\n"
            "or configure manually:\n"
            "  cmake -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++")
    endif()

    # Filament links libc++ statically on Linux, so everything linking against
    # it has to agree on that standard library.
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS "-stdlib=libc++")
    check_cxx_source_compiles("#include <cstring>\nint main() { return 0; }" HAVE_LIBCXX)
    unset(CMAKE_REQUIRED_FLAGS)
    untitled_require(HAVE_LIBCXX "libc++ libc++abi" "libc++-dev libc++abi-dev" "libcxx-devel")

    # --------------------------------------------------------------------
    # Client: windowing and Vulkan
    # --------------------------------------------------------------------
    find_package(PkgConfig REQUIRED)

    pkg_check_modules(WAYLAND wayland-client)
    untitled_require(WAYLAND_FOUND "wayland" "libwayland-dev" "wayland-devel")

    pkg_check_modules(WAYLAND_SCANNER wayland-scanner)
    untitled_require(WAYLAND_SCANNER_FOUND "wayland" "libwayland-bin" "wayland-devel")

    # wayland-protocols is deliberately not required: GLFW ships the protocol
    # XML it needs in deps/wayland and generates from those.

    pkg_check_modules(XKBCOMMON xkbcommon)
    untitled_require(XKBCOMMON_FOUND "libxkbcommon" "libxkbcommon-dev" "libxkbcommon-devel")

    pkg_check_modules(X11 x11)
    untitled_require(X11_FOUND "libx11" "libx11-dev" "libX11-devel")

    pkg_check_modules(XRANDR xrandr)
    untitled_require(XRANDR_FOUND "libxrandr" "libxrandr-dev" "libXrandr-devel")

    pkg_check_modules(XINERAMA xinerama)
    untitled_require(XINERAMA_FOUND "libxinerama" "libxinerama-dev" "libXinerama-devel")

    pkg_check_modules(XCURSOR xcursor)
    untitled_require(XCURSOR_FOUND "libxcursor" "libxcursor-dev" "libXcursor-devel")

    pkg_check_modules(XI xi)
    untitled_require(XI_FOUND "libxi" "libxi-dev" "libXi-devel")

    # Filament additionally wants these two and GLU.
    pkg_check_modules(XCOMPOSITE xcomposite)
    untitled_require(XCOMPOSITE_FOUND
        "libxcomposite" "libxcomposite-dev" "libXcomposite-devel")

    pkg_check_modules(XXF86VM xxf86vm)
    untitled_require(XXF86VM_FOUND "libxxf86vm" "libxxf86vm-dev" "libXxf86vm-devel")

    pkg_check_modules(GLU glu)
    untitled_require(GLU_FOUND "glu" "libglu1-mesa-dev" "mesa-libGLU-devel")

    # The Vulkan SDK is deliberately not required either: Filament vendors its
    # own headers (libs/bluevk) and loads the loader at run time. What you need
    # is a *driver* when running the client -- vulkan-radeon, vulkan-intel or
    # nvidia -- which is a runtime concern, not a build one.

    # --------------------------------------------------------------------
    # Servers. Optional: a contributor working only on the client shouldn't
    # need Postgres headers installed.
    # --------------------------------------------------------------------
    if (UNTITLED_BUILD_SERVERS)
        pkg_check_modules(LIBPQ libpq)
        untitled_require(LIBPQ_FOUND "postgresql-libs" "libpq-dev" "libpq-devel")

        pkg_check_modules(SODIUM libsodium)
        untitled_require(SODIUM_FOUND "libsodium" "libsodium-dev" "libsodium-devel")

        if (NOT LIBPQ_FOUND OR NOT SODIUM_FOUND)
            set(MISSING_NOTES
                "\nThe last two are only needed for the dedicated servers. To build just\nthe client instead, configure with -DUNTITLED_BUILD_SERVERS=OFF")
        endif()
    endif()
endif()

# --------------------------------------------------------------------------
# Windows
#
# There is no system package manager, so the third-party libraries come from
# vcpkg (see vcpkg.json). The presets wire up its toolchain; without it, the
# server dependencies simply won't be found.
# --------------------------------------------------------------------------
if (WIN32)
    if (UNTITLED_BUILD_SERVERS AND NOT DEFINED CMAKE_TOOLCHAIN_FILE
            AND NOT DEFINED ENV{VCPKG_ROOT})
        message(WARNING
            "Building the servers on Windows needs libpq and libsodium, which come\n"
            "from vcpkg. Either use the preset:\n"
            "  cmake --preset windows\n"
            "(set VCPKG_ROOT first), or build only the client with\n"
            "  -DUNTITLED_BUILD_SERVERS=OFF")
    endif()
endif()

# --------------------------------------------------------------------------
# One consolidated report
# --------------------------------------------------------------------------
if (MISSING_ARCH)
    # Several checks map to the same distro package; list each once.
    list(REMOVE_DUPLICATES MISSING_ARCH)
    list(REMOVE_DUPLICATES MISSING_DEBIAN)
    list(REMOVE_DUPLICATES MISSING_FEDORA)
    list(REMOVE_ITEM MISSING_ARCH "")
    list(REMOVE_ITEM MISSING_DEBIAN "")
    list(REMOVE_ITEM MISSING_FEDORA "")
    string(REPLACE ";" " " arch_packages "${MISSING_ARCH}")
    string(REPLACE ";" " " debian_packages "${MISSING_DEBIAN}")
    string(REPLACE ";" " " fedora_packages "${MISSING_FEDORA}")
    message(FATAL_ERROR
        "Missing build dependencies. Install them with one of:\n"
        "  Arch:    sudo pacman -S --needed ${arch_packages}\n"
        "  Debian:  sudo apt install ${debian_packages}\n"
        "  Fedora:  sudo dnf install ${fedora_packages}\n"
        "${MISSING_NOTES}")
endif()
