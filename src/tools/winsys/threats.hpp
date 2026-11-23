#pragma once

// tools::winsys::judge — the threat check's rules (phase 13a § 5.2): what the collectors read, and
// what of it looks suspicious or is worth a look, each finding with its reasons in words, its facts
// and the actions that fit it. Pure: the same snapshot, the same findings. The model may raise a
// finding's level, never lower it — the rules are the floor.

#include "tools/winsys/defender.hpp"
#include "tools/winsys/net.hpp"
#include "tools/winsys/sample.hpp"
#include "tools/winsys/services.hpp"
#include "tools/winsys/signature.hpp"
#include "tools/winsys/startup.hpp"
#include "tools/winsys/tasks.hpp"
#include "tools/winsys/wmi.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace tools::winsys {

enum class Level : std::uint8_t { Suspicious, WorthALook, Fine };

// What the rules know of the user's folders.
struct Places {
    std::wstring windir;                     // C:\Windows
    std::vector<std::wstring> program_files; // C:\Program Files, … (x86)
    std::wstring program_data;
    std::wstring profile;
    std::wstring temp;
    std::wstring users_public;
};

// What was learned of a file a rule looks at.
struct FileFacts {
    bool exists = true;
    std::optional<Signature> signature; // none: not read
};

struct Snapshot {
    std::vector<StartupEntry> startup;
    std::vector<ServiceInfo> services;
    std::vector<TaskInfo> tasks;
    std::vector<WmiSubscription> wmi;
    std::vector<Sample> processes; // over the idle sample
    std::vector<Connection> connections;
    NetSettings net;
    DefenderStatus defender;
    std::map<std::wstring, FileFacts> files; // by the path as the entries name it
    Places places;
};

struct Fact {
    std::string key;
    std::string value;
    bool read_only = false; // a registry key: looked at only
};

struct Finding {
    Level level = Level::Fine;
    std::string rule;              // "unsigned-in-user-place"
    std::string title;             // "svchost.exe outside System32"
    std::vector<std::string> why;  // the reasons, in words for the report
    std::vector<Fact> facts;       // path, publisher, where it is written
    std::vector<std::string> actions; // what can be done to it: "Disable autostart…", "End process…", …
};

[[nodiscard]] auto judge(const Snapshot& s) -> std::vector<Finding>;

struct Stopped {
    int at = 0; // the step it stopped at, from 1
    int of = 0;
};
// The words over the report (§ 5.3): never "no viruses"; nothing found is "nothing I checked looks wrong".
[[nodiscard]] auto verdict(const std::vector<Finding>& findings, std::size_t places, std::optional<Stopped> stopped) -> std::string;

} // namespace tools::winsys
