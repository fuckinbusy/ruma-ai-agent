#pragma once

// tools::winsys::read_window_text — a window's text as UI Automation sees it (phase 13a § 2): each
// element's role, name and value, a line each, in the tree's order; password fields never read.

#include "core/error.hpp"

#include <windows.h>

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>

namespace tools::winsys {

inline constexpr std::size_t kMaxWindowText = 8192;

[[nodiscard]] auto read_window_text(HWND w, std::size_t max_bytes = kMaxWindowText) -> core::Result<std::string>;

// Whether the focus is a password field (UI Automation's IsPassword: a browser's too) inside `w`'s
// process. The focus is the foreground's: asked when the sheet opens, before it takes it. Waited
// for `limit` at most: nothing when the program did not say by then (a busy or hung one held the
// sheet's thread ~11 s, twice, 2026-10-30). The agent's own windows are never asked (false): their
// thread is the caller's, and UI Automation would wait for it for good.
[[nodiscard]] auto focused_password(HWND w, std::chrono::milliseconds limit) -> std::optional<bool>;

} // namespace tools::winsys
