#pragma once

// app::fullscreen — is a fullscreen app in front, so that the sheet must not pop over it?
// Windows' SHQueryUserNotificationState answers BUSY whenever some app claims fullscreen — a
// meeting app sharing the screen does so the whole time, with an ordinary window in front (keys
// diagnostic, 2026-10-11). BUSY counts only when the window in front really covers its monitor;
// exclusive Direct3D and presentation mode always count.

#include <windows.h>

#include <shellapi.h>

namespace app::fullscreen {

// `front`: the foreground window's rectangle; `monitor`: its monitor's; `shell`: the foreground
// is the desktop or the taskbar (which cover the screen without being an app).
inline auto blocks(QUERY_USER_NOTIFICATION_STATE state, const RECT& front, const RECT& monitor, bool shell) -> bool {
    switch (state) {
    case QUNS_RUNNING_D3D_FULL_SCREEN:
    case QUNS_PRESENTATION_MODE: return true;
    case QUNS_BUSY:
        return !shell && front.left <= monitor.left && front.top <= monitor.top && front.right >= monitor.right &&
               front.bottom >= monitor.bottom;
    default: return false;
    }
}

// The handle hides under a fullscreen app on its own monitor only (phase 10: a video on the other
// monitor leaves it be); presentation mode is everywhere.
inline auto blocks_here(QUERY_USER_NOTIFICATION_STATE state, const RECT& front, const RECT& monitor, bool shell,
                        bool same_monitor) -> bool {
    return blocks(state, front, monitor, shell) && (same_monitor || state == QUNS_PRESENTATION_MODE);
}

// The same for the window in front right now.
auto blocks_now() -> bool;
// And for the monitor `mine` (the window's).
auto blocks_on(HMONITOR mine) -> bool;

} // namespace app::fullscreen
