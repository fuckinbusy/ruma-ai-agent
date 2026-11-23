#pragma once

// tools::winsys — is a window sensitive, so the agent neither looks at it nor reads it (phase 13a
// § 2.1): a password manager, Windows' sign-in, a private browser window, a bank's title, a window
// that keeps itself out of captures, a password field in focus, or the user's own list. Checked when
// the window is read, never before.

#include "tools/winsys/private_rules.hpp"

#include <windows.h>

#include <span>
#include <string>

namespace tools::winsys {

struct WindowFacts {
    std::string program;  // the image's file name
    std::string title;
    bool capture_excluded = false; // SetWindowDisplayAffinity other than WDA_NONE
    bool password_focused = false; // its thread's focus is a password field
};

struct Sensitivity {
    bool sensitive = false;
    std::string reason; // "it is a password manager", "it is on your private windows list", …
};

[[nodiscard]] auto facts_of(HWND w) -> WindowFacts;
[[nodiscard]] auto sensitivity_of(const WindowFacts& facts, std::span<const PrivateRule> rules) -> Sensitivity;

} // namespace tools::winsys
