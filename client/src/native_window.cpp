#include "native_window.h"

#include <cstdint>
#include <cstdio>

#include <GLFW/glfw3.h>

#if defined(_WIN32)
    #define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__linux__)
    #define GLFW_EXPOSE_NATIVE_WAYLAND
#endif
#include <GLFW/glfw3native.h>

namespace game {

void* getNativeWindow(GLFWwindow* window, int width, int height) {
#if defined(_WIN32)
    (void) width;
    (void) height;
    return (void*) glfwGetWin32Window(window);
#elif defined(__linux__)
    if (glfwGetPlatform() != GLFW_PLATFORM_WAYLAND) {
        fprintf(stderr, "This build only supports Wayland sessions on Linux.\n");
        return nullptr;
    }

    // Filament keeps the pointer we hand it and dereferences it later, so this
    // has to outlive the call. One window per process, so a static is fine.
    static struct {
        struct wl_display* display;
        struct wl_surface* surface;
        uint32_t width;
        uint32_t height;
    } wayland;

    wayland.display = glfwGetWaylandDisplay();
    wayland.surface = glfwGetWaylandWindow(window);
    wayland.width = static_cast<uint32_t>(width);
    wayland.height = static_cast<uint32_t>(height);
    return (void*) &wayland;
#else
    #error "Unsupported platform"
#endif
}

}  // namespace game
