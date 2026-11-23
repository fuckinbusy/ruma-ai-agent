#pragma once

// ui::gl::Context — an OpenGL 4.5 core context with an 8-bit alpha framebuffer on a window
// (per-pixel transparency through DWM, docs/spikes/r1-acrylic-gl.md). Functions come from glad.
// ui::gl::HiddenSurface — a context on a window that is never shown: for off-screen rendering,
// snapshots and tests.

#include "core/error.hpp"
#include "core/mem/handle.hpp"

#include <glad/glad.h>
#include <glad/glad_wgl.h>

#include <string>

namespace ui::gl {

struct WindowTraits {
    using type = HWND;
    static auto invalid() noexcept -> HWND { return nullptr; }
    static void close(HWND h) noexcept { DestroyWindow(h); }
};
using Window = core::Handle<WindowTraits>;

class Context {
public:
    // The window class must use CS_OWNDC. Makes the context current and loads GL functions.
    static auto create(HWND hwnd, bool debug) -> core::Result<Context>;
    ~Context();
    Context(Context&& other) noexcept;
    Context& operator=(Context&& other) noexcept;
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    void make_current() const noexcept;
    void swap_buffers() const noexcept;
    // Another window this context draws into (its class must use CS_OWNDC): it gets the same
    // pixel format; its DC comes back. One context, several windows, no shared lists to keep.
    [[nodiscard]] auto adopt(HWND other) const -> core::Result<HDC>;
    void make_current(HDC other) const noexcept;
    [[nodiscard]] auto dc() const noexcept -> HDC { return dc_; }
    // "vendor | renderer | version": identifies the driver (program cache key).
    [[nodiscard]] auto driver_id() const noexcept -> const std::string& { return driver_id_; }

private:
    Context() = default;
    void destroy() noexcept;

    HWND hwnd_ = nullptr;
    HDC dc_ = nullptr;
    HGLRC rc_ = nullptr;
    std::string driver_id_;
};

class HiddenSurface {
public:
    static auto create(bool debug = false) -> core::Result<HiddenSurface>;
    [[nodiscard]] auto context() noexcept -> Context& { return context_; }

private:
    HiddenSurface(Window window, Context context)
        : window_(std::move(window)), context_(std::move(context)) {}
    Window window_;   // declared first: destroyed after the context
    Context context_;
};

} // namespace ui::gl
