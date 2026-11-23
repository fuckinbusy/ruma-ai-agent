#pragma once

// app::diag — `agent.exe --diag-keys[=seconds]`: why the shortcut does not reach the agent while
// another program (e.g. a meeting app sharing the screen) runs. For `seconds` (30 by default) it
// listens at three levels at once and logs where Ctrl+Space is still seen:
//   RegisterHotKey (WM_HOTKEY; is the combination taken?), a low-level keyboard hook (does the
//   key reach hooks at all?), Raw Input (the lowest level an application sees),
// with the foreground window's program and whether it runs elevated. Only Space pressed with
// Ctrl, Alt, Shift or Win is logged — no other keystrokes, not plain typing. The log goes to
// %LOCALAPPDATA%\Agent\logs\keys-diag.log.

#include <filesystem>

namespace app::diag {

// `box`: say where the log is when done (off for a run under a script).
auto keys(double seconds, const std::filesystem::path& log, bool box = true) -> int;

} // namespace app::diag
