#pragma once

// core::product — the product's name, from one place: set(WA_PRODUCT ...) in CMakeLists.txt, through the
// generated wa_version.h (the resource compiler reads it too). Everything a user sees takes it from here
// (2026-11-25: the agent is Ruma). The names Windows keeps for the agent's own working — its mutex, window
// classes, notifications' id, the Apps entry's key, the key file's entropy — stay as they were
// (core/agent_ids.hpp, security/secrets.hpp): an update finds the earlier agent by them, and the saved key
// decrypts by them.

#include "wa_version.h"

#include <string_view>

namespace core::product {

inline constexpr std::string_view kName = WA_PRODUCT_TEXT;
inline constexpr std::wstring_view kNameW = WA_PRODUCT_WIDE;
// The folders named after it: Program Files\<it> (the program), %LOCALAPPDATA%\<it> (the data), and the
// Start menu's shortcut.
inline constexpr std::wstring_view kFolder = WA_PRODUCT_WIDE;
inline constexpr std::wstring_view kShortcut = WA_PRODUCT_WIDE L".lnk";
// Before the rename: what an update moves from (data, program, shortcut), never made any more.
inline constexpr std::wstring_view kOldFolder = L"WinAiAgent";
inline constexpr std::wstring_view kOldShortcut = L"WinAiAgent.lnk";

} // namespace core::product
