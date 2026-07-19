# Wire format and socket primitives shared by the client and all three servers.
#
# The client links libc++ (Filament requires it) while the servers use the
# toolchain's default libstdc++. std::string and std::vector cross this
# library's interface, and their layouts differ between the two standard
# libraries, so the sources are compiled twice rather than linked into both.
set(NET_SOURCES
    src/socket.cpp
    src/framing.cpp
    src/protocol.cpp
    src/snapshot.cpp
)

# For the servers.
add_library(net STATIC ${NET_SOURCES})
target_include_directories(net PUBLIC include)

# For the client. Same sources, libc++ ABI.
add_library(net_client STATIC ${NET_SOURCES})
target_include_directories(net_client PUBLIC include)
if (UNIX AND NOT APPLE)
    target_compile_options(net_client PUBLIC -stdlib=libc++)
    target_link_options(net_client PUBLIC -stdlib=libc++)
endif()

if (WIN32)
    target_link_libraries(net PUBLIC ws2_32)
    target_link_libraries(net_client PUBLIC ws2_32)
endif()
