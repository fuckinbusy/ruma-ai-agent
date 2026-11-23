#pragma once

// core::crash — no modal box for a failure nobody watches. A Debug runtime check (an assert in
// the standard library, a corrupted stack) normally opens a dialog on the desktop and waits for
// a click; a crash opens Windows' error report. For tests and the agent's --smoke runs both are
// printed to stderr and the process ends with code 3 instead (2026-10-11: a stack check and a
// string_view assert each stopped a run behind a dialog on the user's screen).

namespace core::crash {

inline constexpr int kFailedCheck = 3;

// Process-wide; call it first thing in main (or from a static initializer).
void quiet_failures();

} // namespace core::crash
