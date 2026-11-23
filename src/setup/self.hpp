#pragma once

// setup::self — the setup program's own business: one setup per session, which mode a start is
// (install with the package inside; uninstall without it, first from a copy in %TEMP% because
// uninstall.exe can't remove itself), and its temporary folders: its DLLs and GL program cache
// live there for the run. It removes only folders it names itself under %TEMP%.

#include "core/win.hpp"

#include <windows.h>

#include <filesystem>
#include <optional>
#include <span>
#include <string_view>

namespace setup::self {

inline constexpr std::wstring_view kSingleName = L"Local\\WinAiAgent.Setup";
inline constexpr std::wstring_view kWindowClass = L"WinAiAgent.Setup";
inline constexpr std::wstring_view kFromTemp = L"--from-temp=";
inline constexpr std::wstring_view kNoGl = L"--no-gl";
inline constexpr std::wstring_view kSmoke = L"--smoke";

// Held while the setup runs; nothing when another setup holds it.
auto acquire_single(std::wstring_view name = kSingleName) -> std::optional<core::win::KernelHandle>;

struct Startup {
    bool uninstall = false;
    bool relaunch = false;          // uninstall.exe from the program's folder: go on from a copy in %TEMP%
    bool no_gl = false;             // --no-gl: the plain Windows dialogs, as without OpenGL 4.5
    bool smoke = false;             // --smoke: the window's whole way, never shown, nothing installed or removed
    std::filesystem::path app_dir;  // what uninstall removes
};

auto startup(std::span<const std::wstring_view> args, bool has_payload, const std::filesystem::path& exe_dir) -> Startup;

// The click-through layer for the appearance and the launch: its monitor one row short. A topmost
// window exactly the monitor's size is taken for a fullscreen game — its frames reach the screen past
// DWM and its transparent pixels show black.
[[nodiscard]] inline auto layer_rect(const RECT& monitor) -> RECT {
    return {monitor.left, monitor.top, monitor.right, monitor.bottom - 1};
}

// The setup's own DLLs travel as resources of type DLL named FREETYPE_DLL (the resource compiler keeps
// quotes in a quoted name, and a bare name can't hold a dot): a file name and its resource name.
auto dll_resource_name(std::wstring_view file) -> std::wstring;
auto dll_file_name(std::wstring_view resource) -> std::wstring;

// The setup runs as an administrator (Program Files): raised by another account than the desktop's
// (a standard user typed an administrator's password), its HKCU and Start menu would be that
// account's, not the user's — refused then. False when it can't tell.
auto elevated_by_another_account() -> bool;

// An event the agent sets or waits on: its DACL the desktop user's and the administrators', its label
// medium — an elevated setup's own objects are high-integrity, and the unelevated agent could not set
// them. Opened when it is there.
auto event_for_agent(const std::wstring& name) -> core::win::KernelHandle;

// `exe args` started in `dir` as the desktop's user, unelevated, through the shell (Explorer's
// ShellExecute): the elevated setup never starts the agent elevated. False when it could not.
auto launch_as_desktop_user(const std::filesystem::path& exe, const std::wstring& args, const std::filesystem::path& dir) -> bool;

// %TEMP%\WinAiAgent<kind>-<pid>
auto own_temp(std::wstring_view kind, DWORD pid) -> std::filesystem::path;
// A folder named WinAiAgent<letters>-<digits> directly in %TEMP%: the only kind the setup removes.
auto is_own_temp(const std::filesystem::path& dir) -> bool;
// Removes `dir` and what it holds, never following a link — only when it is a folder named
// WinAiAgent<letters>-<digits> directly in %TEMP%. False for anything else, and when it couldn't.
auto remove_own_temp(const std::filesystem::path& dir) -> bool;

} // namespace setup::self
