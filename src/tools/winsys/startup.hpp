#pragma once

// tools::winsys — what starts a program with Windows, as the registry and the startup folders hold
// it (phase 13a § 1, § 5.1). Read only; a place the user cannot read is named, never guessed.

#include "tools/winsys/apps.hpp"
#include "tools/winsys/registry.hpp"
#include "tools/winsys/services.hpp"
#include "tools/winsys/tasks.hpp"
#include "tools/winsys/wmi.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tools::winsys {

enum class Place : std::uint8_t {
    Run, RunOnce, Run32, StartupFolder, CommonStartupFolder, Task, Service, Wmi,
    Winlogon, Ifeo, AppInit, ActiveSetup, Lsa, BootExecute, ShellObjects, Policies
};
[[nodiscard]] auto place_name(Place p) -> std::string_view;

struct StartupEntry {
    std::wstring name;    // the value's name, the shortcut's file name, the package's
    std::wstring command; // as written
    std::wstring file;    // the program it runs (file_of)
    Place place = Place::Run;
    bool enabled = true;  // StartupApproved's first byte odd: switched off (as Task Manager does)
    bool machine_wide = false;
    std::optional<int> impact_ms; // from Diagnostics-Performance, when the helper read it
    std::wstring key_or_folder;   // "HKLM\Software\…\Run", or the folder
};

struct StartupList {
    std::vector<StartupEntry> entries;
    std::vector<std::wstring> not_read; // places that could not be read (rights)
};

struct Folders {
    std::filesystem::path user_startup;   // FOLDERID_Startup
    std::filesystem::path common_startup; // FOLDERID_CommonStartup
    static auto real() -> Folders;
};

auto startup_entries(const RegistryView& reg, const Folders& folders) -> StartupList;

// The program a command line runs: the quoted path, else up to its ".exe" (spaces allowed); the DLL
// rundll32 loads; environment variables expanded. Empty when there is none.
[[nodiscard]] auto file_of(std::wstring_view command) -> std::wstring;

struct Sources;    // check.hpp
namespace reader {
struct Data; // reader_data.hpp
}
struct CheckState; // check.hpp

// What the tools read through: the real registry and folders, or a test's (as os::use does).
struct Context {
    RegistryView registry = RegistryView::real();
    Folders folders = Folders::real();
    std::filesystem::path private_file; // the user's private windows (data\private.json); empty: none
    // The scheduled tasks and the WMI subscriptions as they are now: Windows', or a test's.
    std::function<TaskList()> read_tasks = &tasks;
    std::function<WmiList()> read_wmi = &wmi_subscriptions;
    // data\quarantine: a WMI subscription switched off is kept in its wmi\ as MOF; empty: none can be.
    std::filesystem::path quarantine;
    // The Store's packages (Windows', or a test's); how long app.uninstall waits for an uninstaller.
    std::function<std::vector<App>()> read_store = &store_apps;
    std::chrono::milliseconds uninstall_wait{30 * 60 * 1000};
    // How often disk.usage reports its tree so far (tests: as often as it can).
    std::chrono::milliseconds progress_every{250};
    // The threat check (§ 5): where it reads from (none: Windows) and what it read so far, kept for
    // "Continue from step N" (none: nothing kept between calls).
    std::shared_ptr<const Sources> check_sources;
    std::shared_ptr<CheckState> check_state;
    // The services as they are now (file.quarantine looks whether one runs the file): Windows', or a test's.
    std::function<std::vector<ServiceInfo>()> read_services = &services;
    // What agent-reader.exe read at the user's last Check deeper (phase 13a § 6); none: never.
    std::function<std::shared_ptr<const reader::Data>()> deeper_data;
};
[[nodiscard]] auto context() -> const Context&;
// The private windows file of the context in use; none set: none (the real context is not made for it).
[[nodiscard]] auto private_file() -> std::filesystem::path;
void use_context(const Context* c); // nullptr: the real ones
class ContextScope {
public:
    explicit ContextScope(const Context& c) { use_context(&c); }
    ~ContextScope() { use_context(nullptr); }
    ContextScope(const ContextScope&) = delete;
    ContextScope& operator=(const ContextScope&) = delete;
};

} // namespace tools::winsys
