cmake_minimum_required(VERSION 3.21)

project(untitled LANGUAGES C CXX)

# resgen embeds compiled materials with .incbin, which needs an assembler. On
# Windows it emits a C file instead (MASM has no .incbin), so ASM is only
# required elsewhere -- requiring it there would fail configuration for nothing.
if (NOT WIN32)
    enable_language(ASM)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# clangd picks this up from ./build automatically.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Filament on Linux is only supported under clang, and it links the C++ runtime
# statically. Anything that links against it has to agree on that ABI, so bail
# early with a useful message rather than at link time.
option(UNTITLED_BUILD_CLIENT "Build the game client" ON)
option(UNTITLED_BUILD_SERVERS "Build the dedicated servers" ON)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(Dependencies)

add_subdirectory(common/net)

# Physics, used by the world server's authoritative simulation. The client never
# runs physics -- it renders what the server sends.
set(BOX3D_SAMPLES OFF CACHE BOOL "" FORCE)
set(BOX3D_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(BOX3D_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(BOX3D_VALIDATE OFF CACHE BOOL "" FORCE)
add_subdirectory(common/box3d EXCLUDE_FROM_ALL)
add_subdirectory(common/terrain)
add_subdirectory(common/gamesim)
if (UNTITLED_BUILD_CLIENT)
    add_subdirectory(client)
endif()

# The servers build and run on Linux and Windows alike.
if (UNTITLED_BUILD_SERVERS)
    add_subdirectory(server)
endif()
