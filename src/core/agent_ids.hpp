#pragma once

// The names by which the agent is known to Windows and to its installer: one place for both sides
// (app and setup), so the installer closes, registers and removes exactly what the agent uses.

#include <windows.h>

#include <string_view>

namespace core::agent_ids {

// app::instance's single-instance mutex and the sheet's window class.
inline constexpr std::wstring_view kMutex = L"Local\\WinAiAgent.Agent.SingleInstance";
inline constexpr std::wstring_view kWindowClass = L"WinAiAgent.Sheet";

// Posted to the sheet's window: save and exit, as "Exit" in the tray menu does (the installer
// asks before it updates or removes the program).
inline constexpr UINT kExitMessage = WM_APP + 9;

// Notifications: the app's id and its activator (app/notify.cpp registers both).
inline constexpr std::wstring_view kAumid = L"WinAiAgent.Agent";
inline constexpr std::wstring_view kActivatorClsid = L"{3F6B2C1D-8E4A-4B7F-A2D9-5C1E7B3A9D64}";
inline constexpr GUID kActivatorGuid{0x3f6b2c1d, 0x8e4a, 0x4b7f, {0xa2, 0xd9, 0x5c, 0x1e, 0x7b, 0x3a, 0x9d, 0x64}};

// Start-menu shortcuts: the installer's, and the one an agent without an installer makes itself.
inline constexpr std::wstring_view kShortcut = L"WinAiAgent.lnk";
inline constexpr std::wstring_view kBuildShortcut = L"Agent.lnk";

} // namespace core::agent_ids
