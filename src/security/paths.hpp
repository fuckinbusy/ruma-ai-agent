#pragma once

// security::paths — what a path really names and which zone it is in (03-SECURITY § Path zones).
//
// canonical() turns whatever a tool call says into the one path the file system means:
//   1. relative to the task's working folder; "C:relative" (drive-relative) is refused;
//   2. `/` -> `\`, `\\?\`, `\\.\C:\` and `\\localhost\C$` forms unwrapped, `.`/`..` collapsed and
//      trailing dots and spaces dropped per component — before the file system is asked;
//   3. the longest existing part opened and asked for its final path (junctions, symbolic
//      links, 8.3 names, case all resolved by the file system itself); the rest appended;
//   4. alternate data streams (`file:stream`) and devices (CON, \\.\PhysicalDrive0,
//      \\?\GLOBALROOT\...) flagged; UNC, network and removable drives flagged.
// zone_of() then reads the zone off the canonical path, with roots collected at start from
// SHGetKnownFolderPath and the environment (never hard-coded): the stricter of the zones of the
// resolved and the logical path (a junction from User into System is System; a file the system
// redirects — an MSIX parent's virtualised %LOCALAPPDATA% — keeps the zone of its name).

#include "core/error.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace security::paths {

enum class Zone : std::uint8_t { System, Secrets, AppSelf, User, Other };
auto zone_name(Zone z) -> std::string_view;

struct Canonical {
    std::wstring path;    // resolved by the file system ("C:\Windows\System32", "\\server\share\x"), no long prefix
    std::wstring logical; // as named, collapsed and unwrapped, before the file system was asked
    bool exists = false; // the whole path exists
    bool ads = false;    // it named an alternate data stream (stripped from `path`)
    bool device = false; // a device or a kernel namespace path, not a file
    bool unc = false;    // \\server\share
    bool remote = false; // a network or substituted network drive
    bool removable = false;

    // The \\?\ form for Win32 calls (no MAX_PATH limit).
    [[nodiscard]] auto long_form() const -> std::wstring;
};

auto canonical(std::wstring_view input, const std::filesystem::path& working_dir) -> core::Result<Canonical>;

// The \\?\ form of a final path for Win32 calls: "\\?\UNC\server\share\…" for a share,
// "\\?\C:\…" for a drive; a path already in long form as it is. The only place it is built.
auto long_form(std::wstring_view path) -> std::wstring;

// Where things are on this machine.
struct Roots {
    std::wstring system_root;   // %SystemRoot%
    std::wstring system_drive;  // "C:"
    std::vector<std::wstring> program_files;
    std::wstring program_data;  // %ProgramData%
    std::wstring profile;       // %USERPROFILE%
    std::wstring users;         // C:\Users
    std::wstring local_app_data;
    std::wstring roaming_app_data;
    std::vector<std::wstring> agent_dirs; // %LOCALAPPDATA%\Agent, and <program>\data in a portable build
    std::vector<std::wstring> secret_dirs;
};

// For the current user; the agent's folders are %LOCALAPPDATA%\Agent and, in a portable build,
// the data folder beside `exe` (core::win::data_dir_beside). Resolved (see resolve()).
auto make_roots(const std::filesystem::path& exe) -> Roots;
// For this program.
auto make_roots() -> Roots;
// Every root through canonical(): a root behind a junction or a redirection (an MSIX-packaged
// parent's virtualised %LOCALAPPDATA%) must match the final paths canonical() gives, or a path
// could slip past its zone.
void resolve(Roots& roots);
// Collected once.
auto system_roots() -> const Roots&;

auto zone_of(const Canonical& path, const Roots& roots) -> Zone;
// The stricter of two zones: Secrets > System > AppSelf > Other > User.
auto stricter(Zone a, Zone b) -> Zone;

// Case-insensitive: `path` is `root` or inside it.
auto is_within(std::wstring_view path, std::wstring_view root) -> bool;

} // namespace security::paths
