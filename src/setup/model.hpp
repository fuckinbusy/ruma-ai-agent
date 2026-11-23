#pragma once

// setup: the installer's decisions that need no window — where it installs, whether the chosen
// folder takes it, and what the installed version means for this installer (docs/specs/installer.md).

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace setup {

// The folder the program goes to: `<chosen>\WinAiAgent`, or `chosen` itself when it is already
// called that (any case). A trailing separator is ignored.
auto install_dir(const std::filesystem::path& chosen) -> std::filesystem::path;

struct FolderCheck {
    bool writable = false;
    std::uint64_t free = 0;   // bytes free on the folder's disk
    std::uint64_t needed = 0; // what the install takes
    std::uint64_t total = 0;  // the disk's size (the Options screen's bar)
    std::string problem;      // for the screen; empty when the folder takes the install
};

// Whether `dir` (or, while it doesn't exist, its nearest existing parent) takes `needed` bytes:
// writes and removes a probe file, reads the disk's free space. Creates nothing that stays.
auto check_folder(const std::filesystem::path& dir, std::uint64_t needed) -> FolderCheck;

// The folder exists and holds anything while it isn't the program's own (no install.json): a first
// install never moves into someone else's files (a clone of the project is called WinAiAgent too).
auto holds_others(const std::filesystem::path& dir) -> bool;

// The longest install folder accepted. A shortcut can't point past MAX_PATH (260), Windows starts
// programs from deeper folders only with a machine-wide setting, and the program's data lies a few
// levels below it (data\logs\…): 150 leaves room for those.
inline constexpr std::size_t kMaxDirLength = 150;

enum class Relation : std::uint8_t { NotInstalled, Older, SameOrNewer };

// The installed version (from the Uninstall entry) against this installer's: x.y.z compared by
// numbers. A version that can't be read is Older — the installer updates it.
auto relate(std::optional<std::string_view> installed, std::string_view mine) -> Relation;

} // namespace setup
