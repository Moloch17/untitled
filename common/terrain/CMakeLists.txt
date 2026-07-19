# Procedural terrain, shared by the world server and the client. Compiled twice
# for the same reason as net and gamesim: the client links libc++ and the
# servers link libstdc++.
set(TERRAIN_SOURCES src/heightfield.cpp)

add_library(terrain STATIC ${TERRAIN_SOURCES})
target_include_directories(terrain PUBLIC include)

add_library(terrain_client STATIC ${TERRAIN_SOURCES})
target_include_directories(terrain_client PUBLIC include)
if (UNIX AND NOT APPLE)
    target_compile_options(terrain_client PUBLIC -stdlib=libc++)
    target_link_options(terrain_client PUBLIC -stdlib=libc++)
endif()
