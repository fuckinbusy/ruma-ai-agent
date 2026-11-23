#pragma once

// tools::winsys::apps — the programs installed for the user and the machine (phase 13a § 1, § 4):
// the three Uninstall branches, and Store packages when asked. Read only.

#include "tools/winsys/registry.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace tools::winsys {

struct App {
    std::string name, version, publisher, date;
    std::wstring uninstall;        // UninstallString as written
    std::wstring quiet_uninstall;  // never used (no silent uninstalls), kept to say it is there
    std::optional<std::uint64_t> size; // bytes; none when the program does not say
    bool store = false;
    bool system_component = false;
    bool msi = false;
    std::wstring msi_code;         // {GUID}
    bool machine_wide = false;
    std::wstring key;              // where its entry is ("HKLM\…\Uninstall\…"), for the check after an uninstall
    std::wstring location;         // InstallLocation, when it says
};

// Entries without a DisplayName are left out (updates and parts, as Settings does).
[[nodiscard]] auto apps(const RegistryView& reg, bool with_store) -> std::vector<App>;
// The Store's packages for this user, frameworks and parts left out.
[[nodiscard]] auto store_apps() -> std::vector<App>;

// What app.uninstall starts (phase 13a § 4): an MSI by msiexec.exe /x {code} (its own window); any
// other its UninstallString as written, split into the program and its arguments. Never the
// QuietUninstallString.
[[nodiscard]] auto uninstall_command(const App& app) -> std::pair<std::wstring, std::wstring>;

// Waits for `pid` and every process it started (and they started) to end, at most `timeout`, or
// until `stop`. True: none of them runs. 0: nothing to wait for.
[[nodiscard]] auto wait_for_tree(std::uint32_t pid, std::stop_token stop, std::chrono::milliseconds timeout) -> bool;

} // namespace tools::winsys
