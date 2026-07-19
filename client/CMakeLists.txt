# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------

# GLFW: no OpenGL loader, no docs/tests/examples. On Linux we build both the
# Wayland and X11 backends so a single binary runs on either session type; GLFW
# picks at runtime (override with GLFW_PLATFORM / XDG_SESSION_TYPE).
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
if (UNIX AND NOT APPLE)
    set(GLFW_BUILD_WAYLAND ON CACHE BOOL "" FORCE)
    set(GLFW_BUILD_X11 ON CACHE BOOL "" FORCE)
endif()

# Filament: Vulkan only, and skip everything we don't ship.
set(FILAMENT_SUPPORTS_VULKAN ON CACHE BOOL "" FORCE)
set(FILAMENT_SUPPORTS_OPENGL OFF CACHE BOOL "" FORCE)
set(FILAMENT_SKIP_SAMPLES ON CACHE BOOL "" FORCE)
set(FILAMENT_SKIP_SDL2 ON CACHE BOOL "" FORCE)
set(FILAMENT_BUILD_TESTING OFF CACHE BOOL "" FORCE)
if (UNIX AND NOT APPLE)
    set(FILAMENT_SUPPORTS_WAYLAND ON CACHE BOOL "" FORCE)
    # Filament only builds against libc++ on Linux (its material compiler relies
    # on libc++'s transitive includes), and it links the C++ runtime statically,
    # so the client below has to use the same standard library.
    set(USE_STATIC_LIBCXX ON CACHE BOOL "" FORCE)
    # Filament's Vulkan platform is a compile-time choice: Wayland and X11 are
    # mutually exclusive there (see VulkanPlatformLinux.cpp), so with Wayland on
    # these must be off. GLFW still handles X11 windowing, but the Vulkan
    # surface path requires a Wayland session.
    set(FILAMENT_SUPPORTS_XCB OFF CACHE BOOL "" FORCE)
    set(FILAMENT_SUPPORTS_XLIB OFF CACHE BOOL "" FORCE)
endif()

add_subdirectory(deps/glfw EXCLUDE_FROM_ALL)
add_subdirectory(deps/filament EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# Materials
#
# Each .mat is compiled to a .filamat by Filament's matc, then resgen packs the
# results into an object file so the shaders are embedded in the binary rather
# than loaded from disk at runtime.
# ---------------------------------------------------------------------------

set(MATERIAL_SRCS materials/lit.mat materials/ui.mat)

set(GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(MATERIAL_DIR ${GENERATED_DIR}/material)
set(RESOURCE_DIR ${GENERATED_DIR}/resources)
file(MAKE_DIRECTORY ${MATERIAL_DIR} ${RESOURCE_DIR})

set(RESOURCE_BINS)
foreach (mat_src ${MATERIAL_SRCS})
    get_filename_component(localname "${mat_src}" NAME_WE)
    get_filename_component(fullname "${mat_src}" ABSOLUTE)
    set(output_path "${MATERIAL_DIR}/${localname}.filamat")
    add_custom_command(
        OUTPUT ${output_path}
        COMMAND matc -a vulkan -p desktop -o ${output_path} ${fullname}
        MAIN_DEPENDENCY ${mat_src}
        DEPENDS matc
        COMMENT "Compiling material ${mat_src}"
    )
    list(APPEND RESOURCE_BINS ${output_path})
endforeach()

# The UI font is embedded in the executable the same way, so the client has no
# runtime asset paths to resolve.
list(APPEND RESOURCE_BINS ${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/roboto_medium.ttf)

# Defines RESGEN_OUTPUTS / RESGEN_FLAGS / RESGEN_SOURCE. The archive name has to
# be unique across the whole link, hence the "client" prefix.
get_resgen_vars(${RESOURCE_DIR} clientmaterials)

add_custom_command(
    OUTPUT ${RESGEN_OUTPUTS}
    COMMAND resgen ${RESGEN_FLAGS} ${RESOURCE_BINS}
    DEPENDS resgen ${RESOURCE_BINS}
    COMMENT "Packing materials"
)

if (DEFINED RESGEN_SOURCE_FLAGS)
    set_source_files_properties(${RESGEN_SOURCE} PROPERTIES COMPILE_FLAGS ${RESGEN_SOURCE_FLAGS})
endif()

# ---------------------------------------------------------------------------
# Client
# ---------------------------------------------------------------------------

add_executable(client
    src/main.cpp
    src/application.cpp
    src/native_window.cpp
    src/camera_controller.cpp
    src/mesh.cpp
    src/scene.cpp
    src/net_client.cpp
    src/prediction.cpp
    src/screens.cpp
    src/ui/font.cpp
    src/ui/ui_renderer.cpp
    src/ui/widgets.cpp
    ${RESGEN_SOURCE}
)

target_include_directories(client PRIVATE src ${RESOURCE_DIR} deps/stb)

target_link_libraries(client PRIVATE
    net_client
    gamesim_client
    glfw
    filament
    backend
    filabridge
    filaflat
    ibl
    utils
    geometry
    smol-v
    bluevk
)

if (WIN32)
    # Console-less release builds; keep stdout attached in debug.
    set_target_properties(client PROPERTIES WIN32_EXECUTABLE $<CONFIG:Release>)
    if (MSVC)
        # A GUI-subsystem executable makes MSVC look for WinMain, but the entry
        # point is an ordinary main(). Point the linker at the C runtime's
        # startup instead of renaming it.
        target_link_options(client PRIVATE $<$<CONFIG:Release>:/ENTRY:mainCRTStartup>)
    endif()
elseif (UNIX AND NOT APPLE)
    target_compile_definitions(client PRIVATE FILAMENT_SUPPORTS_WAYLAND)
    # Match the C++ runtime Filament was built against.
    target_compile_options(client PRIVATE -stdlib=libc++)
    target_link_options(client PRIVATE -stdlib=libc++)
    target_link_libraries(client PRIVATE dl pthread)
endif()
