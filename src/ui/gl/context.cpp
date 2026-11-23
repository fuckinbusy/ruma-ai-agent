#include "ui/gl/context.hpp"

#include "core/log.hpp"

#include <format>
#include <utility>

namespace ui::gl {
namespace {

constexpr wchar_t kHiddenClass[] = L"wa_gl_hidden";

void register_class(const wchar_t* name) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = name;
    RegisterClassExW(&wc); // a second registration fails harmlessly
}

auto make_hidden_window() -> Window {
    register_class(kHiddenClass);
    return Window{CreateWindowExW(WS_EX_TOOLWINDOW, kHiddenClass, L"", WS_POPUP, 0, 0, 1, 1,
                                  nullptr, nullptr, GetModuleHandleW(nullptr), nullptr)};
}

struct LegacyContextTraits {
    using type = HGLRC;
    static auto invalid() noexcept -> HGLRC { return nullptr; }
    static void close(HGLRC rc) noexcept {
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(rc);
    }
};

// WGL extension entry points need a current context, and a window's pixel format can be set
// once — so a throwaway window with a legacy context loads them, once per process.
auto load_wgl_extensions() -> core::Result<void> {
    static bool loaded = false;
    if (loaded) {
        return {};
    }
    const Window dummy = make_hidden_window();
    if (!dummy) {
        return core::os_error(GetLastError(), "creating a window for WGL setup");
    }
    HDC dc = GetDC(dummy.get());
    PIXELFORMATDESCRIPTOR pfd{sizeof(pfd), 1};
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    const int format = ChoosePixelFormat(dc, &pfd);
    if (format == 0 || !SetPixelFormat(dc, format, &pfd)) {
        return core::os_error(GetLastError(), "setting a pixel format for WGL setup");
    }
    const core::Handle<LegacyContextTraits> legacy{wglCreateContext(dc)};
    if (!legacy || !wglMakeCurrent(dc, legacy.get())) {
        return core::os_error(GetLastError(), "creating a legacy GL context for WGL setup");
    }
    if (!gladLoadWGL(dc) || !GLAD_WGL_ARB_pixel_format || !GLAD_WGL_ARB_create_context) {
        return core::make_error(core::Code::Internal,
                                "the GL driver lacks WGL_ARB_pixel_format/create_context");
    }
    loaded = true;
    return {};
}

void APIENTRY debug_callback(GLenum, GLenum type, GLuint id, GLenum severity, GLsizei length,
                             const GLchar* message, const void*) {
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) {
        return;
    }
    const bool error = type == GL_DEBUG_TYPE_ERROR || severity == GL_DEBUG_SEVERITY_HIGH;
    const std::string_view text{message, static_cast<std::size_t>(length)};
    if (error) {
        WA_ERROR("gl", "driver {}: {}", id, text);
    } else {
        WA_WARN("gl", "driver {}: {}", id, text);
    }
}

} // namespace

auto Context::create(HWND hwnd, bool debug) -> core::Result<Context> {
    if (auto loaded = load_wgl_extensions(); !loaded) {
        return std::unexpected{loaded.error()};
    }
    Context ctx;
    ctx.hwnd_ = hwnd;
    ctx.dc_ = GetDC(hwnd);

    const int attribs[] = {
        WGL_DRAW_TO_WINDOW_ARB, GL_TRUE,
        WGL_SUPPORT_OPENGL_ARB, GL_TRUE,
        WGL_DOUBLE_BUFFER_ARB, GL_TRUE,
        WGL_ACCELERATION_ARB, WGL_FULL_ACCELERATION_ARB,
        WGL_PIXEL_TYPE_ARB, WGL_TYPE_RGBA_ARB,
        WGL_COLOR_BITS_ARB, 32,
        WGL_ALPHA_BITS_ARB, 8,
        WGL_DEPTH_BITS_ARB, 0,
        WGL_STENCIL_BITS_ARB, 0,
        0,
    };
    int format = 0;
    UINT count = 0;
    if (!wglChoosePixelFormatARB(ctx.dc_, attribs, nullptr, 1, &format, &count) || count == 0) {
        return core::make_error(core::Code::Internal, "no RGBA8 pixel format with alpha");
    }
    PIXELFORMATDESCRIPTOR pfd{};
    DescribePixelFormat(ctx.dc_, format, sizeof(pfd), &pfd);
    if (!SetPixelFormat(ctx.dc_, format, &pfd)) {
        return core::os_error(GetLastError(), "setting the window pixel format");
    }
    const int context_attribs[] = {
        WGL_CONTEXT_MAJOR_VERSION_ARB, 4,
        WGL_CONTEXT_MINOR_VERSION_ARB, 5,
        WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
        WGL_CONTEXT_FLAGS_ARB, debug ? WGL_CONTEXT_DEBUG_BIT_ARB : 0,
        0,
    };
    ctx.rc_ = wglCreateContextAttribsARB(ctx.dc_, nullptr, context_attribs);
    if (ctx.rc_ == nullptr) {
        return core::make_error(core::Code::Internal, "OpenGL 4.5 core context not available");
    }
    ctx.make_current();
    if (!gladLoadGL()) {
        return core::make_error(core::Code::Internal, "loading OpenGL functions failed");
    }
    const auto gl_string = [](GLenum name) {
        const auto* s = reinterpret_cast<const char*>(glGetString(name));
        return s != nullptr ? s : "?"; // null on a lost context
    };
    ctx.driver_id_ = std::format("{} | {} | {}", gl_string(GL_VENDOR), gl_string(GL_RENDERER),
                                 gl_string(GL_VERSION));
    if (debug) {
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(debug_callback, nullptr);
    }
    WA_INFO("gl", "context ready: {}", ctx.driver_id_);
    return ctx;
}

void Context::destroy() noexcept {
    if (rc_ != nullptr) {
        if (wglGetCurrentContext() == rc_) {
            wglMakeCurrent(nullptr, nullptr);
        }
        wglDeleteContext(rc_);
        rc_ = nullptr;
    }
    if (dc_ != nullptr) {
        ReleaseDC(hwnd_, dc_); // no-op for CS_OWNDC windows, correct either way
        dc_ = nullptr;
    }
}

Context::~Context() {
    destroy();
}

Context::Context(Context&& other) noexcept
    : hwnd_(std::exchange(other.hwnd_, nullptr)), dc_(std::exchange(other.dc_, nullptr)),
      rc_(std::exchange(other.rc_, nullptr)), driver_id_(std::move(other.driver_id_)) {}

Context& Context::operator=(Context&& other) noexcept {
    if (this != &other) {
        destroy();
        hwnd_ = std::exchange(other.hwnd_, nullptr);
        dc_ = std::exchange(other.dc_, nullptr);
        rc_ = std::exchange(other.rc_, nullptr);
        driver_id_ = std::move(other.driver_id_);
    }
    return *this;
}

void Context::make_current() const noexcept {
    wglMakeCurrent(dc_, rc_);
}

void Context::swap_buffers() const noexcept {
    SwapBuffers(dc_);
}

auto Context::adopt(HWND other) const -> core::Result<HDC> {
    HDC dc = GetDC(other); // CS_OWNDC: the window keeps it
    if (dc == nullptr) {
        return core::os_error(GetLastError(), "getting the second window's DC");
    }
    const int format = GetPixelFormat(dc_);
    PIXELFORMATDESCRIPTOR pfd{};
    if (format == 0 || DescribePixelFormat(dc_, format, sizeof(pfd), &pfd) == 0 ||
        (GetPixelFormat(dc) != format && !SetPixelFormat(dc, format, &pfd))) {
        return core::os_error(GetLastError(), "setting the second window's pixel format");
    }
    return dc;
}

void Context::make_current(HDC other) const noexcept {
    wglMakeCurrent(other, rc_);
}

auto HiddenSurface::create(bool debug) -> core::Result<HiddenSurface> {
    Window window = make_hidden_window();
    if (!window) {
        return core::os_error(GetLastError(), "creating a hidden window");
    }
    auto context = Context::create(window.get(), debug);
    if (!context) {
        return std::unexpected{context.error()};
    }
    return HiddenSurface{std::move(window), std::move(*context)};
}

} // namespace ui::gl
