# Dedicated servers. Headless, and built on Linux and Windows alike.
#
# Dependencies are located by find_package rather than pkg-config so the same
# CMake works on Windows, where vcpkg provides them (see vcpkg.json) and
# pkg-config generally isn't present.

find_package(PostgreSQL REQUIRED)

# libsodium ships no official CMake config, so try the packaged forms in turn:
# vcpkg's config package, then pkg-config, then a plain library search.
find_package(unofficial-sodium CONFIG QUIET)
if (TARGET unofficial-sodium::sodium)
    set(SODIUM_TARGET unofficial-sodium::sodium)
else()
    find_package(PkgConfig QUIET)
    if (PkgConfig_FOUND)
        pkg_check_modules(SODIUM IMPORTED_TARGET libsodium)
    endif()
    if (TARGET PkgConfig::SODIUM)
        set(SODIUM_TARGET PkgConfig::SODIUM)
    else()
        find_path(SODIUM_INCLUDE_DIR sodium.h)
        find_library(SODIUM_LIBRARY NAMES sodium libsodium)
        if (NOT SODIUM_INCLUDE_DIR OR NOT SODIUM_LIBRARY)
            message(FATAL_ERROR
                "libsodium not found (needed for password hashing).\n"
                "  Arch:    sudo pacman -S libsodium\n"
                "  Debian:  sudo apt install libsodium-dev\n"
                "  Windows: configure with the vcpkg preset, which fetches it")
        endif()
        add_library(sodium_fallback INTERFACE)
        target_include_directories(sodium_fallback INTERFACE ${SODIUM_INCLUDE_DIR})
        target_link_libraries(sodium_fallback INTERFACE ${SODIUM_LIBRARY})
        set(SODIUM_TARGET sodium_fallback)
    endif()
endif()

# Shared server plumbing: poll loop, logging, config, database RPC client.
add_library(serverutil STATIC
    common/src/tcp_server.cpp
    common/src/db_client.cpp
)
target_include_directories(serverutil PUBLIC common/include)
target_link_libraries(serverutil PUBLIC net)

# The only target that links libpq.
add_executable(dbserver
    dbserver/src/main.cpp
    dbserver/src/database.cpp
)
target_link_libraries(dbserver PRIVATE serverutil PostgreSQL::PostgreSQL)

add_executable(worldserver
    worldserver/src/main.cpp
    worldserver/src/simulation.cpp
    worldserver/src/console.cpp
)
# libsodium here too: the console can create accounts, which means hashing.
target_link_libraries(worldserver PRIVATE serverutil gamesim ${SODIUM_TARGET})

add_executable(authserver authserver/src/main.cpp)
target_link_libraries(authserver PRIVATE serverutil ${SODIUM_TARGET})

# Interactive server console: account management and world clock control.
add_executable(console tools/src/console.cpp)
target_link_libraries(console PRIVATE serverutil)

# Test tool for exercising auth without the game client.
add_executable(authcli tools/src/authcli.cpp)
target_link_libraries(authcli PRIVATE serverutil)

# Drives login -> join -> UDP snapshots end to end.
add_executable(worldcli tools/src/worldcli.cpp)
target_link_libraries(worldcli PRIVATE serverutil)
