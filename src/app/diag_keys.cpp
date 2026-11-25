#include "app/diag_keys.hpp"

#include "app/fullscreen.hpp"

#include "core/product.hpp"
#include "core/utf8.hpp"

#include <windows.h>

#include <shellapi.h>

#include <chrono>
#include <format>
#include <fstream>
#include <string>
#include <string_view>

namespace app::diag {
namespace {

struct State {
    std::ofstream out;
    std::chrono::steady_clock::time_point start;
    double seconds = 30;
    int hotkeys = 0, hooked = 0, raw = 0;
    std::wstring last_foreground;
};
State* g = nullptr; // the hook procedure has no context pointer

void line(std::string_view text) {
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g->start).count();
    g->out << std::format("{:8.3f}  {}\n", t, text);
    g->out.flush();
}

auto mods() -> std::string {
    std::string m;
    m += GetAsyncKeyState(VK_CONTROL) < 0 ? "Ctrl+" : "";
    m += GetAsyncKeyState(VK_MENU) < 0 ? "Alt+" : "";
    m += GetAsyncKeyState(VK_SHIFT) < 0 ? "Shift+" : "";
    m += GetAsyncKeyState(VK_LWIN) < 0 || GetAsyncKeyState(VK_RWIN) < 0 ? "Win+" : "";
    return m + "Space";
}

// Only a shortcut-like Space is logged: plain typing (its rhythm included) stays out of the log.
auto with_modifier() -> bool {
    return mods() != "Space";
}

auto utf8(std::wstring_view w) -> std::string {
    auto s = core::utf8::to_utf8(w);
    return s ? *s : std::string{"?"};
}

// The foreground window's program and whether it runs elevated (a higher-integrity window
// keeps low-level hooks of normal programs from seeing its input).
auto foreground() -> std::wstring {
    HWND w = GetForegroundWindow();
    if (w == nullptr) {
        return L"(none)";
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    std::wstring name = L"pid " + std::to_wstring(pid);
    std::wstring elevated = L"elevation unknown";
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t path[MAX_PATH]{};
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(p, 0, path, &size)) {
            name = std::filesystem::path{path}.filename().wstring();
        }
        HANDLE token = nullptr;
        if (OpenProcessToken(p, TOKEN_QUERY, &token)) {
            TOKEN_ELEVATION e{};
            DWORD got = 0;
            if (GetTokenInformation(token, TokenElevation, &e, sizeof(e), &got)) {
                elevated = e.TokenIsElevated != 0 ? L"ELEVATED" : L"not elevated";
            }
            CloseHandle(token);
        } else {
            elevated = L"token not readable (often: ELEVATED)";
        }
        CloseHandle(p);
    }
    return name + L", " + elevated;
}

auto notification_state() -> std::string_view {
    QUERY_USER_NOTIFICATION_STATE s{};
    if (FAILED(SHQueryUserNotificationState(&s))) {
        return "?";
    }
    switch (s) {
    case QUNS_NOT_PRESENT: return "not present (locked / screen saver)";
    case QUNS_BUSY: return "busy (fullscreen app)";
    case QUNS_RUNNING_D3D_FULL_SCREEN: return "D3D fullscreen";
    case QUNS_PRESENTATION_MODE: return "presentation mode";
    case QUNS_ACCEPTS_NOTIFICATIONS: return "normal";
    case QUNS_QUIET_TIME: return "quiet time";
    case QUNS_APP: return "a Store app fullscreen";
    }
    return "?";
}

LRESULT CALLBACK hook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        if (k->vkCode == VK_SPACE && (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN) && with_modifier()) {
            ++g->hooked;
            line(std::format("low-level hook: {} down{}", mods(), (k->flags & LLKHF_INJECTED) != 0 ? " (injected)" : ""));
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

LRESULT CALLBACK proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_HOTKEY:
        ++g->hotkeys;
        line(std::format("WM_HOTKEY id {} -> {}; foreground: {}", wp,
                         fullscreen::blocks_now() ? "the agent would NOT open (a fullscreen app in front)"
                                                  : "the agent would open",
                         utf8(foreground())));
        return 0;
    case WM_INPUT: {
        RAWINPUT ri{};
        UINT size = sizeof(ri);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) !=
                static_cast<UINT>(-1) &&
            ri.header.dwType == RIM_TYPEKEYBOARD && ri.data.keyboard.VKey == VK_SPACE &&
            (ri.data.keyboard.Flags & RI_KEY_BREAK) == 0 && with_modifier()) {
            ++g->raw;
            line(std::format("raw input: {} down; foreground: {}", mods(), utf8(foreground())));
        }
        break; // DefWindowProc cleans the input up
    }
    case WM_TIMER: {
        const auto fg = foreground();
        if (fg != g->last_foreground) {
            g->last_foreground = fg;
            line(std::format("foreground: {}; notification state: {}", utf8(fg), notification_state()));
        }
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g->start).count();
        if (t >= g->seconds) {
            PostQuitMessage(0);
        }
        return 0;
    }
    default: break;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

} // namespace

auto keys(double seconds, const std::filesystem::path& log, bool box) -> int {
    State state;
    state.seconds = seconds;
    state.start = std::chrono::steady_clock::now();
    std::error_code ec;
    std::filesystem::create_directories(log.parent_path(), ec);
    state.out.open(log, std::ios::binary | std::ios::trunc);
    if (!state.out) {
        return 2;
    }
    g = &state;
    line(std::format("keys diagnostic for {:.0f} s: press Ctrl+Space (and Ctrl+Shift+Space) a few times now",
                     seconds));

    const HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"WinAiAgent.KeysDiag";
    RegisterClassExW(&wc);
    HWND w = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst,
                             nullptr);

    const struct {
        int id;
        UINT mods;
        const char* name;
    } shortcuts[] = {{1, MOD_CONTROL, "Ctrl+Space"},
                     {2, MOD_CONTROL | MOD_SHIFT, "Ctrl+Shift+Space"},
                     {3, MOD_CONTROL | MOD_ALT, "Ctrl+Alt+Space"}};
    for (const auto& s : shortcuts) {
        if (RegisterHotKey(w, s.id, s.mods | MOD_NOREPEAT, VK_SPACE)) {
            line(std::format("RegisterHotKey {}: ok (id {})", s.name, s.id));
        } else {
            const DWORD e = GetLastError();
            line(std::format("RegisterHotKey {}: FAILED, error {}{}", s.name, e,
                             e == ERROR_HOTKEY_ALREADY_REGISTERED ? " (taken by another program, or the agent runs)" : ""));
        }
    }
    HHOOK h = SetWindowsHookExW(WH_KEYBOARD_LL, hook, inst, 0);
    line(h != nullptr ? "low-level keyboard hook: installed" : std::format("low-level keyboard hook: FAILED, error {}", GetLastError()));
    RAWINPUTDEVICE device{0x01, 0x06, RIDEV_INPUTSINK, w}; // generic desktop, keyboard
    line(RegisterRawInputDevices(&device, 1, sizeof(device)) ? "raw input: registered"
                                                             : std::format("raw input: FAILED, error {}", GetLastError()));
    state.last_foreground = foreground();
    line(std::format("foreground: {}; notification state: {}", utf8(state.last_foreground), notification_state()));
    SetTimer(w, 1, 250, nullptr);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    line(std::format("done: Space with modifiers seen {} times by raw input, {} by the low-level hook; WM_HOTKEY {} times",
                     state.raw, state.hooked, state.hotkeys));
    if (h != nullptr) {
        UnhookWindowsHookEx(h);
    }
    for (const auto& s : shortcuts) {
        UnregisterHotKey(w, s.id);
    }
    DestroyWindow(w);
    g = nullptr;
    state.out.close();
    if (box) {
        const std::wstring text = L"Keys diagnostic done.\nThe log is at:\n" + log.wstring();
        MessageBoxW(nullptr, text.c_str(), WA_PRODUCT_WIDE, MB_OK | MB_ICONINFORMATION);
    }
    return 0;
}

} // namespace app::diag
