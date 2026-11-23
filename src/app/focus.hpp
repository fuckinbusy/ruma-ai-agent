#pragma once

// app::focus — whose window took the focus from the sheet (shell.cpp, WM_ACTIVATE).

#include <windows.h>

namespace app::focus {

// `w` is owned by `owner`, directly or through its own owners: the sheet's own dialog (a
// CSV's Save as, its overwrite question), not another program. No window: not ours.
[[nodiscard]] inline auto owned_by(HWND w, HWND owner) noexcept -> bool {
    for (int depth = 0; w != nullptr && depth < 16; ++depth) { // an owner chain is short; a loop is not one
        w = GetWindow(w, GW_OWNER);
        if (w == owner) {
            return true;
        }
    }
    return false;
}

} // namespace app::focus
