# Shared character simulation. Compiled twice for the same reason as `net`: the
# client links libc++ and the servers link libstdc++.
set(GAMESIM_SOURCES src/character.cpp)

add_library(gamesim STATIC ${GAMESIM_SOURCES})
target_include_directories(gamesim PUBLIC include)
target_link_libraries(gamesim PUBLIC box3d terrain)

add_library(gamesim_client STATIC ${GAMESIM_SOURCES})
target_include_directories(gamesim_client PUBLIC include)
target_link_libraries(gamesim_client PUBLIC box3d terrain_client)
if (UNIX AND NOT APPLE)
    target_compile_options(gamesim_client PUBLIC -stdlib=libc++)
    target_link_options(gamesim_client PUBLIC -stdlib=libc++)
endif()
