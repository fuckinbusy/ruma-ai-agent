#include "app/fullscreen.hpp"

#include <string_view>

namespace app::fullscreen {

namespace {

// `mine` null: any monitor.
auto blocks_for(HMONITOR mine) -> bool {
    QUERY_USER_NOTIFICATION_STATE state{};
    if (FAILED(SHQueryUserNotificationState(&state))) {
        return false;
    }
    HWND front = GetForegroundWindow();
    RECT rect{};
    MONITORINFO mi{sizeof(mi)};
    const HMONITOR theirs = front != nullptr ? MonitorFromWindow(front, MONITOR_DEFAULTTONEAREST) : nullptr;
    if (front == nullptr || !GetWindowRect(front, &rect) || !GetMonitorInfoW(theirs, &mi)) {
        return state == QUNS_RUNNING_D3D_FULL_SCREEN || state == QUNS_PRESENTATION_MODE;
    }
    wchar_t cls[64]{};
    GetClassNameW(front, cls, 64);
    const std::wstring_view name{cls};
    const bool shell = front == GetShellWindow() || front == GetDesktopWindow() || name == L"WorkerW" ||
                       name == L"Progman" || name == L"Shell_TrayWnd";
    return blocks_here(state, rect, mi.rcMonitor, shell, mine == nullptr || mine == theirs);
}

} // namespace

auto blocks_now() -> bool {
    return blocks_for(nullptr);
}

auto blocks_on(HMONITOR mine) -> bool {
    return blocks_for(mine);
}

} // namespace app::fullscreen
