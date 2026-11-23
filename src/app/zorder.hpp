#pragma once

// app::zorder — what stands over the resting strip (it sometimes vanished
// and came back after an open and a close; nothing in the log said why). Read once a second at
// rest and logged when it changes: the window over it, by its class and program — never its title
// (the user's words).

#include <windows.h>

#include <string>

namespace app::zorder {

// The first window above `self` in the z-order — shown, not minimised, not cloaked — whose
// rectangle meets `area` (screen pixels); nullptr when none does.
[[nodiscard]] auto covering(HWND self, const RECT& area) -> HWND;

// "class <name>, <program>.exe" for the log.
[[nodiscard]] auto describe(HWND w) -> std::string;

} // namespace app::zorder
