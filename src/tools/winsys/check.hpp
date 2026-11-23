#pragma once

// tools::winsys::run_check — the threat check (phase 13a § 5.1): eight steps over the collectors,
// each place read counted, what needs an administrator named, the rules' findings and the verdict.
// A stop keeps what was read; "Continue from step N" goes on from there with it.

#include "tools/winsys/reader_data.hpp"
#include "tools/winsys/threats.hpp"

#include <array>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace tools::winsys {

inline constexpr std::array<std::string_view, 8> kStages{"Autostart places", "Services and drivers", "Scheduled tasks",
                                                         "WMI subscriptions", "Signatures and paths", "Network",
                                                         "Microsoft Defender", "Load while idle"};

// Where the check reads from: Windows (real_sources), or a test's own.
struct Sources {
    std::function<StartupList()> startup;
    std::function<std::vector<ServiceInfo>()> services;
    std::function<TaskList()> tasks;
    std::function<WmiList()> wmi;
    std::function<std::optional<Signature>(const std::wstring&)> signature;
    std::function<bool(const std::wstring&)> exists;
    std::function<std::vector<Connection>()> connections;
    std::function<NetSettings()> net;
    std::function<DefenderStatus()> defender;
    // 30 s of the processes' load; `tick` after each second, as sample_processes says it.
    std::function<std::vector<Sample>(std::stop_token, const std::function<void(int second, int of)>& tick)> idle;
    Places places;
};

// How far the check has come: the step as it starts (`within` 0), and during one whose length is
// known (the load while idle) its share done and the seconds left.
struct CheckProgress {
    int step = 0;
    std::string_view name;
    double within = 0;
    std::optional<int> left;
};

enum class StageState : std::uint8_t { Waiting, Done, Stopped };

struct Stage {
    std::string name;
    std::size_t places = 0;
    StageState state = StageState::Waiting;
};

// What a check has read so far, kept between a stop and its continuation.
struct CheckState {
    Snapshot snapshot;
    std::vector<Stage> stages;
    std::vector<std::wstring> not_read;
    std::size_t deeper_places = 0; // what the helper added (phase 13a § 6)
    bool deeper = false;
};

struct CheckReport {
    std::vector<Stage> stages;
    std::vector<Finding> findings;
    std::size_t places_checked = 0;
    std::vector<std::wstring> not_read;
    bool defender_on = false;
    std::optional<Stopped> stopped;
    std::string verdict;
    bool deeper = false; // the helper's facts are in it
};

// From step `from` (1: afresh; 2–8: on with what `state` holds of the steps before). `progress`:
// each step as it starts, and each second of the load while idle.
auto run_check(const Sources& sources, CheckState& state, int from, std::stop_token stop,
               const std::function<void(const CheckProgress&)>& progress) -> CheckReport;

// The helper's facts (agent-reader.exe) into a finished check: the tasks, history, exclusions and
// process images the user could not read; judged again with them. What even the helper could not
// read stays in not_read, said so.
auto deepen(const Sources& sources, CheckState& state, const reader::Data& data) -> CheckReport;

// Windows' own: the context's registry, folders, tasks and WMI; the rest read here.
[[nodiscard]] auto real_sources() -> Sources;

} // namespace tools::winsys
