#pragma once

struct GLFWwindow;

namespace game {

// Returns the platform handle Filament expects for Engine::createSwapChain():
//   Windows  -> HWND
//   Wayland  -> pointer to a { wl_display*, wl_surface*, width, height } struct
//
// The Wayland struct is read by Filament when the swap chain is created, so
// pass the current window size and re-create the swap chain on resize.
void* getNativeWindow(GLFWwindow* window, int width, int height);

}  // namespace game
