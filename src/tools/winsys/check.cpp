#include "tools/winsys/check.hpp"

#include "core/utf8.hpp"
#include "security/paths.hpp"

#include <windows.h>

#include <algorithm>
#include <filesystem>

namespace tools::winsys {
namespace {

void note(std::vector<std::wstring>& list, std::wstring what) {
    if (!what.empty() && std::ranges::find(list, what) == list.end()) {
        list.push_back(std::move(what));
    }
}

auto wide(std::string_view s) -> std::wstring {
    return core::utf8::to_wide(s).value_or(std::wstring{});
}

// The files steps 1–3 name (and the processes' at step 8): each once, as written.
auto files_of(const Snapshot& s) -> std::vector<std::wstring> {
    std::vector<std::wstring> out;
    const auto add = [&](const std::wstring& f) {
        if (!f.empty() && std::ranges::find(out, f) == out.end()) {
            out.push_back(f);
        }
    };
    for (const auto& e : s.startup) {
        add(e.file);
    }
    for (const auto& v : s.services) {
        add(v.file);
    }
    for (const auto& t : s.tasks) {
        add(t.file);
    }
    return out;
}

// What was learned of `file`, into the snapshot; false: it was known already.
auto learn(const Sources& src, Snapshot& s, const std::wstring& file) -> bool {
    if (file.empty() || s.files.contains(file)) {
        return false;
    }
    FileFacts f;
    f.exists = src.exists ? src.exists(file) : true;
    if (f.exists && src.signature) {
        f.signature = src.signature(file);
    }
    s.files.emplace(file, std::move(f));
    return true;
}

auto report_of(const CheckState& state, std::optional<Stopped> stopped) -> CheckReport {
    const auto& s = state.snapshot;
    CheckReport r;
    r.stages = state.stages;
    r.findings = judge(s);
    for (const auto& st : state.stages) {
        r.places_checked += st.places;
    }
    r.places_checked += state.deeper_places;
    r.not_read = state.not_read;
    r.defender_on = s.defender.read && s.defender.present && s.defender.on;
    r.stopped = stopped;
    r.verdict = verdict(r.findings, r.places_checked, stopped);
    r.deeper = state.deeper;
    return r;
}

} // namespace

auto run_check(const Sources& src, CheckState& state, int from, std::stop_token stop,
               const std::function<void(const CheckProgress&)>& progress) -> CheckReport {
    constexpr int kSteps = static_cast<int>(kStages.size());
    from = std::clamp(from, 1, kSteps);
    // On from a step: what came before it must have been read; else afresh.
    const bool can_continue = state.stages.size() == kStages.size() &&
                              std::all_of(state.stages.begin(), state.stages.begin() + (from - 1),
                                          [](const Stage& s) { return s.state == StageState::Done; });
    if (from == 1 || !can_continue) {
        state = CheckState{};
        for (const auto name : kStages) {
            state.stages.push_back({std::string{name}, 0, StageState::Waiting});
        }
        from = 1;
    }
    for (int k = from; k <= kSteps; ++k) {
        state.stages[static_cast<std::size_t>(k - 1)] = {std::string{kStages[static_cast<std::size_t>(k - 1)]}, 0, StageState::Waiting};
    }
    auto& s = state.snapshot;
    s.places = src.places;
    std::optional<Stopped> stopped;
    for (int k = from; k <= kSteps; ++k) {
        auto& stage = state.stages[static_cast<std::size_t>(k - 1)];
        if (stop.stop_requested()) {
            stage.state = StageState::Stopped;
            stopped = Stopped{k, kSteps};
            break;
        }
        if (progress) {
            progress({.step = k, .name = stage.name});
        }
        switch (k) {
        case 1: {
            auto l = src.startup ? src.startup() : StartupList{};
            s.startup = std::move(l.entries);
            stage.places = s.startup.size();
            for (auto& n : l.not_read) {
                note(state.not_read, std::move(n));
            }
            break;
        }
        case 2:
            s.services = src.services ? src.services() : std::vector<ServiceInfo>{};
            stage.places = s.services.size();
            break;
        case 3: {
            auto l = src.tasks ? src.tasks() : TaskList{};
            s.tasks = std::move(l.tasks);
            stage.places = s.tasks.size();
            for (const auto& n : l.not_read) {
                note(state.not_read, L"Task Scheduler › " + wide(n));
            }
            break;
        }
        case 4: {
            auto l = src.wmi ? src.wmi() : WmiList{};
            s.wmi = std::move(l.subscriptions);
            stage.places = l.read ? std::max<std::size_t>(s.wmi.size(), 1) : 0; // the namespace itself, when read
            for (auto& n : l.not_read) {
                note(state.not_read, std::move(n));
            }
            if (!l.read && l.not_read.empty()) {
                note(state.not_read, L"WMI root\\subscription");
            }
            break;
        }
        case 5:
            s.files.clear();
            for (const auto& f : files_of(s)) {
                stage.places += learn(src, s, f) ? 1 : 0;
            }
            break;
        case 6:
            s.connections = src.connections ? src.connections() : std::vector<Connection>{};
            s.net = src.net ? src.net() : NetSettings{};
            stage.places = s.connections.size() + 2; // the connections, the hosts file, the proxy settings
            break;
        case 7:
            s.defender = src.defender ? src.defender() : DefenderStatus{};
            stage.places = s.defender.read ? 1 : 0;
            if (!s.defender.read) {
                note(state.not_read, L"Microsoft Defender: " + wide(s.defender.why_not));
            } else if (!s.defender.threats_read) {
                note(state.not_read, L"Defender's threat history (needs administrator rights)");
            }
            break;
        case 8:
            s.processes = src.idle ? src.idle(stop,
                                              [&](int second, int of) {
                                                  if (progress && of > 0) {
                                                      progress({.step = k, .name = stage.name, .within = static_cast<double>(second) / of,
                                                                .left = of - second});
                                                  }
                                              })
                                   : std::vector<Sample>{};
            stage.places = s.processes.size();
            for (const auto& p : s.processes) {
                stage.places += learn(src, s, p.path) ? 1 : 0;
            }
            break;
        default: break;
        }
        // Stopped while it read: this step is not done, what it read is not trusted whole.
        if (stop.stop_requested()) {
            stage.state = StageState::Stopped;
            stage.places = 0;
            stopped = Stopped{k, kSteps};
            break;
        }
        stage.state = StageState::Done;
    }
    return report_of(state, stopped);
}

auto deepen(const Sources& src, CheckState& state, const reader::Data& d) -> CheckReport {
    auto& s = state.snapshot;
    std::size_t added = 0;
    const auto drop = [&](std::wstring_view starting) {
        std::erase_if(state.not_read, [&](const std::wstring& n) { return n.starts_with(starting); });
    };
    if (d.tasks_read) {
        for (const auto& t : d.tasks) {
            if (std::ranges::any_of(s.tasks, [&](const TaskInfo& k) { return k.path == t.path; })) {
                continue;
            }
            const auto command = wide(t.command);
            s.tasks.push_back({.path = t.path, .account = t.account, .command = command, .file = file_of(command), .enabled = t.enabled});
            added += 1 + (learn(src, s, s.tasks.back().file) ? 1 : 0);
        }
        drop(L"Task Scheduler › ");
    }
    if (d.threats_read) {
        s.defender.threats.clear();
        for (const auto& t : d.threats) {
            s.defender.threats.push_back({.name = t.name, .path = t.path, .resolved = t.resolved, .at = t.at});
        }
        s.defender.threats_read = true;
        added += d.threats.size() + 1;
        drop(L"Defender's threat history");
    }
    if (d.exclusions_read) {
        s.defender.exclusions = d.exclusions;
        added += 1;
    }
    // The processes whose image the user could not see: their paths, and those files' facts.
    for (auto& p : s.processes) {
        if (!p.path.empty()) {
            continue;
        }
        const auto it = std::ranges::find(d.processes, p.pid, &reader::Process::pid);
        if (it != d.processes.end() && !it->path.empty()) {
            p.path = wide(it->path);
            added += learn(src, s, p.path) ? 1 : 0;
        }
    }
    for (const auto& n : d.not_read) {
        note(state.not_read, L"Even with administrator rights: " + wide(n));
    }
    state.deeper_places += added;
    state.deeper = true;
    return report_of(state, std::nullopt);
}

auto real_sources() -> Sources {
    const auto& roots = security::paths::system_roots();
    wchar_t temp[MAX_PATH + 1]{};
    const DWORD n = GetTempPathW(MAX_PATH + 1, temp);
    std::wstring temp_dir{temp, n};
    while (!temp_dir.empty() && temp_dir.back() == L'\\') {
        temp_dir.pop_back();
    }
    return {
        .startup = [] { return startup_entries(context().registry, context().folders); },
        .services = [] { return services(); },
        .tasks = [] { return context().read_tasks(); },
        .wmi = [] { return context().read_wmi(); },
        .signature = [](const std::wstring& f) -> std::optional<Signature> { return signature(std::filesystem::path{f}); },
        .exists = [](const std::wstring& f) {
            std::error_code ec;
            return std::filesystem::exists(std::filesystem::path{f}, ec);
        },
        .connections = [] { return connections(); },
        .net = [] { return net_settings(); },
        .defender = [] { return defender_status(); },
        .idle = [](std::stop_token stop, const std::function<void(int, int)>& tick) {
            return sample_processes(std::chrono::seconds{30}, stop, tick);
        },
        .places = {.windir = roots.system_root,
                   .program_files = roots.program_files,
                   .program_data = roots.program_data,
                   .profile = roots.profile,
                   .temp = temp_dir,
                   .users_public = roots.users + L"\\Public"},
    };
}

} // namespace tools::winsys
