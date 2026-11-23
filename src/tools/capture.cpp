#include "tools/capture.hpp"

#include "core/utf8.hpp"

#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <format>

#pragma comment(lib, "dwmapi.lib")

namespace tools::os::detail {
namespace {

// A 32-bit top-down DIB with a memory DC to draw into.
class Canvas {
public:
    Canvas(int w, int h) : w_(w), h_(h) {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = w;
        info.bmiHeader.biHeight = -h; // top row first
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        dc_ = CreateCompatibleDC(nullptr);
        bitmap_ = dc_ != nullptr ? CreateDIBSection(dc_, &info, DIB_RGB_COLORS, &bits_, nullptr, 0) : nullptr;
        if (bitmap_ != nullptr) {
            old_ = SelectObject(dc_, bitmap_);
        }
    }
    ~Canvas() {
        if (old_ != nullptr) {
            SelectObject(dc_, old_);
        }
        if (bitmap_ != nullptr) {
            DeleteObject(bitmap_);
        }
        if (dc_ != nullptr) {
            DeleteDC(dc_);
        }
    }
    Canvas(const Canvas&) = delete;
    auto operator=(const Canvas&) -> Canvas& = delete;

    [[nodiscard]] auto ok() const -> bool { return bits_ != nullptr; }
    [[nodiscard]] auto dc() const -> HDC { return dc_; }
    // The part (x, y, w, h) of it, opaque.
    [[nodiscard]] auto picture(int x, int y, int w, int h) const -> Picture {
        GdiFlush();
        x = std::clamp(x, 0, w_);
        y = std::clamp(y, 0, h_);
        w = std::clamp(w, 0, w_ - x);
        h = std::clamp(h, 0, h_ - y);
        Picture p{.width = w, .height = h, .bgra = std::vector<std::uint8_t>(static_cast<std::size_t>(w) * h * 4)};
        const auto* src = static_cast<const std::uint8_t*>(bits_);
        for (int row = 0; row < h; ++row) {
            std::copy_n(src + (static_cast<std::size_t>(y + row) * w_ + x) * 4, static_cast<std::size_t>(w) * 4,
                        p.bgra.data() + static_cast<std::size_t>(row) * w * 4);
        }
        for (std::size_t i = 3; i < p.bgra.size(); i += 4) {
            p.bgra[i] = 255; // GDI leaves alpha as it likes
        }
        return p;
    }

private:
    int w_, h_;
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ old_ = nullptr;
    void* bits_ = nullptr;
};

auto title_of(HWND w) -> std::wstring {
    std::wstring title(static_cast<std::size_t>(GetWindowTextLengthW(w)) + 1, L'\0');
    title.resize(static_cast<std::size_t>(GetWindowTextW(w, title.data(), static_cast<int>(title.size()))));
    return title;
}

auto lower(std::wstring s) -> std::wstring {
    std::ranges::transform(s, s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

// A visible window with a title containing `part` (case aside), the front-most first.
auto window_titled(std::wstring_view part) -> HWND {
    struct Search {
        std::wstring part;
        HWND found = nullptr;
    } search{lower(std::wstring{part}), nullptr};
    EnumWindows(
        [](HWND w, LPARAM data) -> BOOL {
            auto& s = *reinterpret_cast<Search*>(data);
            if (!IsWindowVisible(w) || (GetWindowLongPtrW(w, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0) {
                return TRUE;
            }
            if (lower(title_of(w)).find(s.part) != std::wstring::npos) {
                s.found = w;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.found;
}

auto utf8(std::wstring_view w) -> std::string {
    auto s = core::utf8::to_utf8(w);
    return s ? std::move(*s) : std::string{"?"};
}

// The monitors, left to right (then top to bottom): "Monitor 1" is the leftmost.
auto monitor_rects() -> std::vector<RECT> {
    std::vector<RECT> rects;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR m, HDC, LPRECT, LPARAM data) -> BOOL {
            MONITORINFO info{.cbSize = sizeof(MONITORINFO)};
            if (GetMonitorInfoW(m, &info)) {
                reinterpret_cast<std::vector<RECT>*>(data)->push_back(info.rcMonitor);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&rects));
    std::ranges::sort(rects, [](const RECT& a, const RECT& b) { return a.left != b.left ? a.left < b.left : a.top < b.top; });
    return rects;
}

// A part of the screen (virtual-screen coordinates).
auto shoot(const RECT& r, std::string source) -> core::Result<Captured> {
    const int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) {
        return core::make_error(core::Code::Io, "the screen cannot be found");
    }
    Canvas canvas{w, h};
    HDC screen = GetDC(nullptr);
    const bool drawn = canvas.ok() && screen != nullptr && BitBlt(canvas.dc(), 0, 0, w, h, screen, r.left, r.top, SRCCOPY | CAPTUREBLT);
    if (screen != nullptr) {
        ReleaseDC(nullptr, screen);
    }
    if (!drawn) {
        return core::make_error(core::Code::Io, "the screen could not be captured");
    }
    return Captured{.picture = canvas.picture(0, 0, w, h), .title = "screen", .source = std::move(source)};
}

// The open windows one could show instead (visible, titled, not a tool's, not cloaked), front first.
auto window_titles() -> std::vector<std::string> {
    std::vector<std::string> titles;
    EnumWindows(
        [](HWND w, LPARAM data) -> BOOL {
            auto& out = *reinterpret_cast<std::vector<std::string>*>(data);
            if (out.size() >= 30 || !IsWindowVisible(w) || (GetWindowLongPtrW(w, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0) {
                return TRUE;
            }
            DWORD cloaked = 0;
            DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
            const auto title = title_of(w);
            if (cloaked == 0 && !title.empty() && title != L"Program Manager") {
                out.push_back(utf8(title));
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&titles));
    return titles;
}

auto window(HWND w, RECT* area = nullptr) -> core::Result<Captured> {
    if (w == nullptr || !IsWindow(w)) {
        return core::make_error(core::Code::NotFound, "that window is not open");
    }
    if (IsIconic(w)) {
        return core::make_error(core::Code::Io, "the window is minimized: it has nothing to show");
    }
    RECT whole{};
    GetWindowRect(w, &whole);
    RECT seen = whole; // without the invisible resize borders
    DwmGetWindowAttribute(w, DWMWA_EXTENDED_FRAME_BOUNDS, &seen, sizeof seen);
    const int ww = whole.right - whole.left, wh = whole.bottom - whole.top;
    if (ww <= 0 || wh <= 0) {
        return core::make_error(core::Code::Io, "the window has no size");
    }
    Canvas canvas{ww, wh};
    if (!canvas.ok() || !PrintWindow(w, canvas.dc(), PW_RENDERFULLCONTENT)) {
        return core::make_error(core::Code::Io, "the window could not be captured (it may run as administrator)");
    }
    const auto title = title_of(w);
    if (area != nullptr) {
        *area = seen;
    }
    auto named = title.empty() ? std::string{"window"} : utf8(title);
    return Captured{.picture = canvas.picture(seen.left - whole.left, seen.top - whole.top, seen.right - seen.left, seen.bottom - seen.top),
                    .title = named, .source = named};
}

// A DIB on the clipboard as a .bmp file, for WIC's BMP decoder (it knows every header and layout).
auto bmp_of(const std::uint8_t* dib, std::size_t size) -> std::string {
    if (size < sizeof(BITMAPINFOHEADER)) {
        return {};
    }
    BITMAPINFOHEADER h{};
    std::memcpy(&h, dib, sizeof h);
    const std::size_t masks = h.biCompression == BI_BITFIELDS && h.biSize == sizeof(BITMAPINFOHEADER) ? 12 : 0;
    const std::size_t colours = (h.biClrUsed != 0 ? h.biClrUsed : (h.biBitCount <= 8 ? (1u << h.biBitCount) : 0)) * 4;
    BITMAPFILEHEADER f{};
    f.bfType = 0x4D42; // "BM"
    f.bfSize = static_cast<DWORD>(sizeof f + size);
    f.bfOffBits = static_cast<DWORD>(sizeof f + h.biSize + masks + colours);
    std::string out(sizeof f + size, '\0');
    std::memcpy(out.data(), &f, sizeof f);
    std::memcpy(out.data() + sizeof f, dib, size);
    return out;
}

auto clipboard_bytes(UINT format) -> std::string {
    HANDLE h = GetClipboardData(format);
    if (h == nullptr) {
        return {};
    }
    const auto* p = static_cast<const std::uint8_t*>(GlobalLock(h));
    if (p == nullptr) {
        return {};
    }
    std::string out(reinterpret_cast<const char*>(p), GlobalSize(h));
    GlobalUnlock(h);
    return out;
}

} // namespace

auto read_clipboard() -> core::Result<ClipboardContent> {
    bool open = false;
    for (int i = 0; i < 10 && !(open = OpenClipboard(nullptr) != FALSE); ++i) {
        Sleep(20); // another program holds it
    }
    if (!open) {
        return core::make_error(core::Code::Io, "the clipboard is busy");
    }
    ClipboardContent out;
    std::string picture_file; // decoded after the clipboard is closed again
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
            out.text = utf8(std::wstring_view{p}.substr(0, 20000)); // within the 64 KB result, even escaped
            GlobalUnlock(h);
        }
    } else if (HANDLE drop = GetClipboardData(CF_HDROP)) {
        auto* hdrop = static_cast<HDROP>(drop);
        std::vector<std::string> names;
        const UINT n = DragQueryFileW(hdrop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n && i < 500; ++i) {
            wchar_t name[MAX_PATH * 2]{};
            DragQueryFileW(hdrop, i, name, static_cast<UINT>(std::size(name)));
            names.push_back(utf8(name));
        }
        out.files = std::move(names);
    } else if (const UINT png = RegisterClipboardFormatW(L"PNG"); IsClipboardFormatAvailable(png)) {
        picture_file = clipboard_bytes(png);
    } else {
        for (const UINT dib : {static_cast<UINT>(CF_DIBV5), static_cast<UINT>(CF_DIB)}) {
            if (IsClipboardFormatAvailable(dib)) {
                const auto bytes = clipboard_bytes(dib);
                picture_file = bmp_of(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
                break;
            }
        }
    }
    CloseClipboard();
    if (!picture_file.empty()) {
        auto picture = image::decode(std::as_bytes(std::span{picture_file.data(), picture_file.size()}));
        if (!picture) {
            return std::unexpected{picture.error()};
        }
        out.picture = std::move(*picture);
    }
    return out;
}

auto capture(const CaptureSpec& spec, void* active, std::array<long, 4> agent) -> core::Result<Captured> {
    DwmFlush(); // the agent's windows are hidden from capture by now
    const auto monitors = monitor_rects();
    RECT area{};
    core::Result<Captured> shot = core::make_error(core::Code::InvalidArgument, "nothing to capture");
    switch (spec.target) {
    case CaptureTarget::Screen: {
        POINT at{};
        GetCursorPos(&at);
        MONITORINFO info{.cbSize = sizeof(MONITORINFO)};
        if (!GetMonitorInfoW(MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST), &info)) {
            return core::make_error(core::Code::Io, "the screen cannot be found");
        }
        area = info.rcMonitor;
        const auto it = std::ranges::find_if(monitors, [&](const RECT& r) { return r.left == area.left && r.top == area.top; });
        shot = shoot(area, std::format("Monitor {}", it == monitors.end() ? 1 : it - monitors.begin() + 1));
        break;
    }
    case CaptureTarget::Monitor:
        if (spec.monitor < 1 || static_cast<std::size_t>(spec.monitor) > monitors.size()) {
            return core::make_error(core::Code::NotFound, std::format("there is no monitor {}", spec.monitor));
        }
        area = monitors[static_cast<std::size_t>(spec.monitor - 1)];
        shot = shoot(area, std::format("Monitor {}", spec.monitor));
        break;
    case CaptureTarget::All:
        area = {GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
                GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
        shot = shoot(area, "All monitors");
        break;
    case CaptureTarget::Active: shot = window(active != nullptr ? static_cast<HWND>(active) : GetForegroundWindow(), &area); break;
    case CaptureTarget::Window: {
        HWND w = window_titled(spec.window);
        if (w == nullptr) {
            return core::make_error(core::Code::NotFound, std::format("no open window's title contains \"{}\"", utf8(spec.window)));
        }
        shot = window(w, &area);
        break;
    }
    }
    if (!shot) {
        return shot;
    }
    shot->monitors = std::max(1, static_cast<int>(monitors.size()));
    shot->windows = window_titles();
    // Where the agent's window is on it (it is not in the picture): its part inside the area.
    const long l = std::max(agent[0], area.left), t = std::max(agent[1], area.top);
    const long r = std::min(agent[2], area.right), b = std::min(agent[3], area.bottom);
    if (r > l && b > t) {
        shot->hole_x = static_cast<int>(l - area.left);
        shot->hole_y = static_cast<int>(t - area.top);
        shot->hole_w = static_cast<int>(r - l);
        shot->hole_h = static_cast<int>(b - t);
    }
    return shot;
}

} // namespace tools::os::detail
