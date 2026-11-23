#include "app/zorder.hpp"

#include "core/utf8.hpp"
#include "core/win.hpp"

#include <dwmapi.h>

#include <array>
#include <filesystem>
#include <format>

namespace app::zorder {

namespace {

auto cloaked(HWND w) -> bool {
    DWORD c = 0;
    return SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_CLOAKED, &c, sizeof(c))) && c != 0;
}

} // namespace

auto covering(HWND self, const RECT& area) -> HWND {
    for (HWND w = GetWindow(self, GW_HWNDPREV); w != nullptr; w = GetWindow(w, GW_HWNDPREV)) {
        if (IsWindowVisible(w) == FALSE || IsIconic(w) != FALSE || cloaked(w)) {
            continue;
        }
        RECT r{};
        RECT meet{};
        if (GetWindowRect(w, &r) != FALSE && IntersectRect(&meet, &r, &area) != FALSE) {
            return w;
        }
    }
    return nullptr;
}

auto describe(HWND w) -> std::string {
    std::array<wchar_t, 128> cls{};
    const int n = GetClassNameW(w, cls.data(), static_cast<int>(cls.size()));
    std::wstring program = L"?";
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (const core::win::KernelHandle p{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)}) {
        std::array<wchar_t, MAX_PATH> path{};
        DWORD size = static_cast<DWORD>(path.size());
        if (QueryFullProcessImageNameW(p.get(), 0, path.data(), &size) != FALSE) {
            program = std::filesystem::path{std::wstring_view{path.data(), size}}.filename().wstring(); // its name, never its folder
        }
    }
    return std::format("class {}, {}", core::utf8::to_utf8(std::wstring_view{cls.data(), static_cast<std::size_t>(std::max(n, 0))}).value_or("?"),
                       core::utf8::to_utf8(program).value_or("?"));
}

} // namespace app::zorder
