#pragma once

// setup::install / update / uninstall (docs/specs/installer.md § What the install does, § Removal).
// Install records everything it makes in the journal (install.json) and undoes it all, in reverse,
// on any failure or Cancel. Uninstall removes what the journal lists, the data folder, and what the
// agent itself wrote outside its folder — never anything that isn't the program's.

#include "core/error.hpp"
#include "setup/journal.hpp"
#include "setup/model.hpp"
#include "setup/payload.hpp"
#include "setup/system.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>

namespace setup {

// The Apps entry is the machine's (the program in Program Files, docs/specs/installer-program-files.md):
// "HKLM\\" before a key names the machine's branch, everywhere System takes a key, and in the journal.
inline constexpr std::wstring_view kUninstallKey = L"HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\WinAiAgent";
// The entry of the earlier install for the current user only (%LOCALAPPDATA%): replaced by it.
inline constexpr std::wstring_view kUserUninstallKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\WinAiAgent";
inline constexpr std::wstring_view kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
inline constexpr std::wstring_view kRunValue = L"WinAiAgent";
// What the agent writes while it runs (app/gpu.cpp, app/notify.cpp; Windows keeps the AUMID's
// notification settings).
inline constexpr std::wstring_view kGpuKey = L"Software\\Microsoft\\DirectX\\UserGpuPreferences";
inline constexpr std::wstring_view kNotificationsKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Notifications\\Settings\\WinAiAgent.Agent";

enum class Step : std::uint8_t { Files, Shortcuts, Settings };

struct Watch {
    Progress progress;                // the files' bytes
    std::function<void(Step)> step;   // each step as it starts
};

struct Choices {
    std::filesystem::path dir; // the install folder itself (install_dir of the user's choice)
    std::string hotkey = "Ctrl+Space";
    bool autostart = true;
    bool desktop = false;
};

// The current user's Start menu programs folder, desktop, and the agent's own folder of theirs
// (%LOCALAPPDATA%\Ruma: its data in data\, apart from the program in Program Files); tests
// use folders of their own.
struct Places {
    std::filesystem::path start_menu;
    std::filesystem::path desktop;
    std::filesystem::path user_dir;
    static auto for_user() -> Places;
};

struct Installed {
    std::filesystem::path dir;
    std::string version;
};

// The installed program, from its Uninstall entry; nothing when there is none.
auto installed(System& sys) -> std::optional<Installed>;

auto install(System& sys, const Places& places, const Choices& choices, std::span<const std::byte> zip,
             const Watch& watch, const std::stop_token& stop) -> core::Result<Journal>;

// The program's files replaced in place; data, settings, shortcuts and autostart stay as they are. An
// install from before the rename moves to update_dir(dir), with the same shortcuts and autostart.
auto update(System& sys, const Places& places, const std::filesystem::path& dir, std::span<const std::byte> zip,
            const Watch& watch, const std::stop_token& stop) -> core::Result<Journal>;

// The program is installed in `dir`: its journal names the folder, or the Apps entry does. Only then
// does uninstall remove anything (uninstall.exe beside a build of the agent removes nothing).
auto installed_here(System& sys, const std::filesystem::path& dir) -> bool;

auto uninstall(System& sys, const Places& places, const std::filesystem::path& dir, const Progress& progress)
    -> core::Result<void>;

} // namespace setup
