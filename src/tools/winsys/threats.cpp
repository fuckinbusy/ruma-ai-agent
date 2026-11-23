#include "tools/winsys/threats.hpp"

#include "core/utf8.hpp"

#include <algorithm>
#include <array>
#include <cwctype>
#include <format>
#include <numeric>

namespace tools::winsys {
namespace {

constexpr std::string_view kDisable = "Disable autostart\xE2\x80\xA6";
constexpr std::string_view kQuarantine = "Quarantine\xE2\x80\xA6";
constexpr std::string_view kEnd = "End process\xE2\x80\xA6";
constexpr std::string_view kYourself = "How to fix it yourself";
constexpr std::string_view kSecurity = "Open Windows Security";

auto utf8(std::wstring_view s) -> std::string {
    return core::utf8::to_utf8(s).value_or("?");
}

auto lower(std::wstring_view s) -> std::wstring {
    std::wstring out{s};
    std::ranges::transform(out, out.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return out;
}

auto trim(std::wstring_view s) -> std::wstring_view {
    while (!s.empty() && (s.front() == L' ' || s.front() == L'"')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == L' ' || s.back() == L'"')) {
        s.remove_suffix(1);
    }
    return s;
}

// `path` is inside `dir` (any case).
auto under(std::wstring_view path, std::wstring_view dir) -> bool {
    if (dir.empty() || path.size() <= dir.size()) {
        return false;
    }
    return lower(path.substr(0, dir.size())) == lower(dir) && (path[dir.size()] == L'\\' || dir.ends_with(L'\\'));
}

auto parent_of(std::wstring_view path) -> std::wstring_view {
    const auto slash = path.find_last_of(L'\\');
    return slash == std::wstring_view::npos ? std::wstring_view{} : path.substr(0, slash);
}

auto leaf(std::wstring_view path) -> std::wstring {
    const auto slash = path.find_last_of(L'\\');
    return lower(slash == std::wstring_view::npos ? path : path.substr(slash + 1));
}

auto same(std::wstring_view a, std::wstring_view b) -> bool {
    return lower(a) == lower(b);
}

// Something that starts or runs a program, as the rules see it.
struct Thing {
    std::string kind; // "autostart", "service", "scheduled task", "process"
    std::string name;
    std::wstring file;
    std::wstring command;
    std::string where;  // a key, a folder, Task Scheduler
    bool registry = false;
    std::vector<std::string> actions;
};

auto starting_actions(Place p) -> std::vector<std::string> {
    switch (p) {
    case Place::Run:
    case Place::Run32:
    case Place::StartupFolder:
    case Place::CommonStartupFolder: return {std::string{kDisable}, std::string{kQuarantine}};
    default: return {std::string{kYourself}};
    }
}

struct Judge {
    const Snapshot& s;
    std::vector<Finding> out;

    [[nodiscard]] auto facts_of(const Thing& t) const -> std::vector<Fact> {
        std::vector<Fact> f;
        if (!t.file.empty()) {
            f.push_back({"Path", utf8(t.file), false});
            if (const auto it = s.files.find(t.file); it != s.files.end() && it->second.signature) {
                const auto& sig = *it->second.signature;
                f.push_back({"Publisher", sig.signed_ok ? sig.publisher : std::string{"not signed"}, false});
            }
        }
        if (!t.command.empty() && t.command != t.file) {
            f.push_back({"Command", utf8(t.command), false});
        }
        if (!t.where.empty()) {
            f.push_back({t.registry ? "Registry" : "Where", t.where, t.registry});
        }
        return f;
    }

    void add(Level level, std::string rule, std::string title, std::vector<std::string> why, const Thing& t, std::vector<std::string> actions) {
        out.push_back({level, std::move(rule), std::move(title), std::move(why), facts_of(t), std::move(actions)});
    }

    // nullopt: not read; true: a valid signature.
    [[nodiscard]] auto signed_ok(const std::wstring& file) const -> std::optional<bool> {
        const auto it = s.files.find(file);
        if (it == s.files.end() || !it->second.signature) {
            return std::nullopt;
        }
        return it->second.signature->signed_ok;
    }

    // Where installers rarely put what starts with Windows; empty: none of those.
    [[nodiscard]] auto user_place(std::wstring_view file) const -> std::string {
        const auto& p = s.places;
        if (under(file, p.temp)) {
            return "Temp";
        }
        if (under(file, p.profile + L"\\AppData")) {
            return "AppData";
        }
        if (under(file, p.program_data)) {
            return "ProgramData";
        }
        if (under(file, p.users_public)) {
            return "the Public folder";
        }
        if (!p.profile.empty() && same(parent_of(file), p.profile)) {
            return "the profile folder itself";
        }
        return {};
    }

    [[nodiscard]] auto in_program_files(std::wstring_view file) const -> bool {
        return std::ranges::any_of(s.places.program_files, [&](const std::wstring& d) { return under(file, d); });
    }

    // Rules 1, 9: an unsigned program where it starts from.
    void unsigned_starters(const std::vector<Thing>& things) {
        for (const auto& t : things) {
            if (t.file.empty() || signed_ok(t.file) != false) {
                continue;
            }
            if (const auto place = user_place(t.file); !place.empty()) {
                add(Level::Suspicious, "unsigned-in-user-place", std::format("{} starts an unsigned program from {}", t.name, place),
                    {std::format("It starts by itself ({})", t.kind), "Its program has no valid signature",
                     std::format("It lives in {}, where installed programs rarely start from", place)},
                    t, t.actions);
            } else if (in_program_files(t.file)) {
                add(Level::WorthALook, "unsigned-program-files", std::format("{}'s program is not signed", t.name),
                    {std::format("It starts by itself ({})", t.kind), "Its program in Program Files has no valid signature: many small vendors "
                                                                     "do not sign, but it cannot be told apart from a changed file"},
                    t, t.actions);
            }
        }
    }

    // Rule 2: a Windows file's name elsewhere.
    void system_names(const std::vector<Thing>& things) {
        static constexpr std::array<std::wstring_view, 13> kNames{L"svchost.exe", L"lsass.exe",  L"csrss.exe",    L"explorer.exe", L"winlogon.exe",
                                                                  L"services.exe", L"smss.exe",  L"wininit.exe",  L"spoolsv.exe",  L"taskhostw.exe",
                                                                  L"dwm.exe",      L"conhost.exe", L"rundll32.exe"};
        const auto& w = s.places.windir;
        for (const auto& t : things) {
            if (t.file.empty()) {
                continue;
            }
            const auto name = leaf(t.file);
            if (std::ranges::find(kNames, name) == kNames.end()) {
                continue;
            }
            const auto dir = parent_of(t.file);
            if (same(dir, w) || same(dir, w + L"\\System32") || same(dir, w + L"\\SysWOW64")) {
                continue;
            }
            add(Level::Suspicious, "system-name-elsewhere", std::format("{} outside Windows' folders", utf8(name)),
                {std::format("{} is the name of a file of Windows", utf8(name)),
                 std::format("This one is in {}, not in System32 or SysWOW64", utf8(dir))},
                t, t.kind == "process" ? std::vector<std::string>{std::string{kEnd}, std::string{kQuarantine}} : t.actions);
        }
    }

    // Rule 3: a mining pool.
    void mining(const std::vector<Thing>& things) {
        static constexpr std::array<std::uint16_t, 6> kPorts{3333, 4444, 5555, 7777, 14444, 45700};
        for (const auto& c : s.connections) {
            const bool loopback = c.remote.starts_with("127.") || c.remote == "::1";
            if (c.remote_port == 0 || loopback || std::ranges::find(kPorts, c.remote_port) == kPorts.end()) {
                continue;
            }
            const Thing t{.kind = "process", .name = c.process, .where = std::format("{}:{}", c.remote, c.remote_port)};
            add(Level::Suspicious, "mining-pool", std::format("{} talks to a mining pool's port", c.process),
                {std::format("It is connected to {}:{}", c.remote, c.remote_port),
                 std::format("Port {} is a common port of cryptocurrency mining pools", c.remote_port)},
                t, {std::string{kEnd}});
        }
        for (const auto& t : things) {
            if (lower(t.command).find(L"stratum+tcp") != std::wstring::npos) {
                add(Level::Suspicious, "mining-pool", std::format("{} names a mining pool", t.name),
                    {"Its command line has stratum+tcp://, the mining pools' protocol"}, t, t.actions);
            }
        }
    }

    // Rule 4: busy while idle, hidden and unsigned.
    void busy_idle() {
        const auto mean = [](const std::vector<float>& v) {
            return v.empty() ? 0.0f : std::accumulate(v.begin(), v.end(), 0.0f) / static_cast<float>(v.size());
        };
        for (const auto& p : s.processes) {
            const float cpu = mean(p.cpu);
            const float gpu = mean(p.gpu);
            if (!p.windowless || (cpu <= 50 && gpu <= 50) || p.path.empty() || signed_ok(p.path) != false) {
                continue;
            }
            const bool graphics = gpu > cpu;
            const Thing t{.kind = "process", .name = p.name, .file = p.path};
            add(Level::Suspicious, "busy-when-idle",
                std::format("{} keeps the {} busy while idle", p.name, graphics ? "graphics card" : "processor"),
                {std::format("{:.0f}% of the {} on average while nothing was being done", graphics ? gpu : cpu, graphics ? "graphics card" : "processor"),
                 "It has no window", "Its program has no valid signature"},
                t, {std::string{kEnd}, std::string{kQuarantine}});
        }
    }

    // Rule 5: WMI running something.
    void wmi() {
        for (const auto& w : s.wmi) {
            const bool command = w.consumer_class == "CommandLineEventConsumer";
            if (!command && w.consumer_class != "ActiveScriptEventConsumer") {
                continue;
            }
            const Thing t{.kind = "WMI subscription", .name = w.filter, .command = w.command, .where = "WMI root\\subscription \xE2\x80\xBA " + w.consumer};
            add(Level::Suspicious, "wmi-runs-something", std::format("A WMI subscription runs {}", command ? "a command" : "a script"),
                {"WMI starts it by itself on an event, unseen: a way programs stay after they are removed",
                 std::format("Its filter: {}", w.query.empty() ? w.filter : w.query)},
                t, {std::string{kDisable}});
        }
    }

    // Rules 6, 7: IFEO and Winlogon.
    void deep_places() {
        const std::wstring userinit = lower(s.places.windir + L"\\system32\\userinit.exe");
        for (const auto& e : s.startup) {
            const Thing t{.kind = std::string{place_name(e.place)}, .name = utf8(e.name), .file = e.file, .command = e.command,
                          .where = utf8(e.key_or_folder), .registry = true};
            if (e.place == Place::Ifeo) {
                const bool debugger = lower(e.key_or_folder).find(L"image file execution options") != std::wstring::npos;
                if (debugger) {
                    add(Level::Suspicious, "ifeo-debugger", std::format("{} is opened through another program", utf8(e.name)),
                        {std::format("Image File Execution Options sends {} to {} instead", utf8(e.name), utf8(e.command)),
                         "A way to replace a program, or to run something whenever it starts"},
                        t, {std::string{kYourself}});
                } else {
                    add(Level::WorthALook, "silent-exit-monitor", std::format("Something runs when {} ends", utf8(e.name)),
                        {std::format("SilentProcessExit starts {} when it closes", utf8(e.command))}, t, {std::string{kYourself}});
                }
            } else if (e.place == Place::Winlogon) {
                const auto name = lower(e.name);
                auto value = lower(trim(e.command));
                if (name == L"shell" && value != L"explorer.exe") {
                    add(Level::Suspicious, "winlogon-changed", "Winlogon's Shell is changed",
                        {"Windows' own is explorer.exe", std::format("This one is {}", utf8(e.command)),
                         "A wrong Shell starts something at every sign-in"},
                        t, {std::string{kYourself}});
                } else if (name == L"userinit") {
                    while (!value.empty() && (value.back() == L',' || value.back() == L' ')) {
                        value.pop_back();
                    }
                    if (value != userinit) {
                        add(Level::Suspicious, "winlogon-changed", "Winlogon's Userinit is changed",
                            {std::format("Windows' own is {},", utf8(s.places.windir + L"\\system32\\userinit.exe")),
                             std::format("This one is {}", utf8(e.command))},
                            t, {std::string{kYourself}});
                    }
                }
            }
        }
    }

    // Rule 8 and Defender's state.
    void defender() {
        const auto& d = s.defender;
        for (const auto& th : d.threats) {
            if (th.resolved) {
                continue;
            }
            const Thing t{.kind = "file", .name = th.name, .file = core::utf8::to_wide(th.path).value_or(L"")};
            add(Level::Suspicious, "defender-threat", std::format("Defender found {} and it is still there", th.name),
                {"Microsoft Defender named it a threat", "Its history says it was not removed"}, t, {std::string{kSecurity}});
        }
        // Read by the helper only: what Defender never scans. Malware adds such exclusions to hide.
        for (const auto& x : d.exclusions) {
            const auto colon = x.find(": ");
            const std::string what = colon == std::string::npos ? x : x.substr(colon + 2);
            const std::string kind = colon == std::string::npos ? std::string{"Paths"} : x.substr(0, colon);
            const Thing t{.kind = "Defender exclusion", .name = what, .where = "Microsoft Defender \xE2\x80\xBA Exclusions \xE2\x80\xBA " + kind,
                          .registry = true};
            add(Level::WorthALook, "defender-exclusion", std::format("Defender never scans {}", what),
                {kind == "Processes" ? "Microsoft Defender has an exclusion for this program: what it opens is not checked"
                 : kind == "Extensions" ? "Microsoft Defender has an exclusion for this kind of file: such files are never checked"
                                        : "Microsoft Defender has an exclusion for it: files there are never checked",
                 "Fine if you or your work added it; malware adds such exclusions to hide"},
                t, {std::string{kSecurity}});
        }
        if (d.read && (!d.present || !d.on || !d.up_to_date)) {
            const Thing t{.kind = "Defender", .name = "Microsoft Defender"};
            const char* title = !d.present ? "Microsoft Defender is not there" : !d.on ? "Defender's real-time protection is off"
                                                                                     : "Defender's signatures are out of date";
            add(Level::WorthALook, "defender-off", title,
                {!d.present ? "Another antivirus may stand in for it; if none does, nothing watches files as they come"
                 : !d.on    ? "Files are not checked as they are opened"
                            : "Its signatures are more than 7 days old: newer threats are not known to it"},
                t, {std::string{kSecurity}});
        }
    }

    // Rules 10–14.
    void worth_a_look(const std::vector<Thing>& tasks) {
        for (const auto& e : s.startup) {
            if (e.file.empty()) {
                continue;
            }
            if (const auto it = s.files.find(e.file); it != s.files.end() && !it->second.exists) {
                const Thing t{.kind = std::string{place_name(e.place)}, .name = utf8(e.name), .file = e.file, .where = utf8(e.key_or_folder),
                              .registry = !e.key_or_folder.starts_with(L"C:") && e.key_or_folder.starts_with(L"HK")};
                add(Level::WorthALook, "missing-file", std::format("{} starts a file that is not there", utf8(e.name)),
                    {"Its program was removed, but its autostart entry stayed"}, t, starting_actions(e.place));
            }
        }
        if (!s.net.hosts_extra.empty()) {
            const Thing t{.kind = "hosts", .name = "hosts", .where = utf8(s.places.windir + L"\\System32\\drivers\\etc\\hosts")};
            add(Level::WorthALook, "hosts", "The hosts file sends some names elsewhere",
                {"It holds lines beyond Windows' own: " + s.net.hosts_extra.substr(0, 200)}, t, {std::string{kYourself}});
        }
        for (const auto* proxy : {&s.net.proxy, &s.net.winhttp_proxy}) {
            if (proxy->has_value()) {
                const Thing t{.kind = "proxy", .name = "proxy", .where = **proxy};
                add(Level::WorthALook, "proxy", "A proxy is set",
                    {std::format("Web traffic goes through {}", **proxy), "Fine if you or your work set it; otherwise it can watch what you browse"},
                    t, {std::string{kYourself}});
            }
        }
        for (const auto& t : tasks) {
            const auto c = lower(t.command);
            const bool encoded = c.find(L"powershell") != std::wstring::npos &&
                                 (c.find(L" -enc") != std::wstring::npos || c.find(L" -encodedcommand") != std::wstring::npos ||
                                  c.find(L" -e ") != std::wstring::npos);
            const bool script_host = c.find(L"wscript") != std::wstring::npos || c.find(L"mshta") != std::wstring::npos;
            const bool rundll_profile = c.find(L"rundll32") != std::wstring::npos && !s.places.profile.empty() &&
                                        c.find(lower(s.places.profile) + L"\\") != std::wstring::npos;
            if (encoded || script_host || rundll_profile) {
                add(Level::WorthALook, "script-task", std::format("The task {} runs a script", t.name),
                    {encoded         ? "It runs PowerShell with an encoded command: what it does cannot be read as written"
                     : script_host ? "It runs a script through wscript or mshta"
                                   : "It loads a DLL from your profile through rundll32"},
                    t, t.actions);
            }
        }
    }
};

} // namespace

auto judge(const Snapshot& s) -> std::vector<Finding> {
    std::vector<Thing> starters; // what starts by itself: autostart, services, tasks
    std::vector<Thing> tasks;
    for (const auto& e : s.startup) {
        if (e.place == Place::Ifeo || e.place == Place::Winlogon) {
            continue; // their own rules
        }
        starters.push_back({.kind = std::string{place_name(e.place)}, .name = utf8(e.name), .file = e.file, .command = e.command,
                            .where = utf8(e.key_or_folder), .registry = e.key_or_folder.starts_with(L"HK"), .actions = starting_actions(e.place)});
    }
    for (const auto& v : s.services) {
        starters.push_back({.kind = "service", .name = v.display.empty() ? v.name : v.display, .file = v.file, .command = v.command,
                            .where = "Services \xE2\x80\xBA " + v.name, .actions = {std::string{kYourself}}});
    }
    for (const auto& t : s.tasks) {
        Thing x{.kind = "scheduled task", .name = t.path, .file = t.file, .command = t.command, .where = "Task Scheduler \xE2\x80\xBA " + t.path,
                .actions = {std::string{kDisable}, std::string{kQuarantine}}};
        starters.push_back(x);
        tasks.push_back(std::move(x));
    }
    std::vector<Thing> running;
    for (const auto& p : s.processes) {
        running.push_back({.kind = "process", .name = p.name, .file = p.path, .where = std::format("process {}", p.pid),
                           .actions = {std::string{kEnd}, std::string{kQuarantine}}});
    }
    Judge j{s, {}};
    j.unsigned_starters(starters);
    auto everything = starters;
    everything.insert(everything.end(), running.begin(), running.end());
    j.system_names(everything);
    j.mining(starters);
    j.busy_idle();
    j.wmi();
    j.deep_places();
    j.defender();
    j.worth_a_look(tasks);
    std::ranges::stable_sort(j.out, {}, &Finding::level); // the suspicious first
    return std::move(j.out);
}

auto verdict(const std::vector<Finding>& findings, std::size_t places, std::optional<Stopped> stopped) -> std::string {
    if (stopped) {
        return std::format("Stopped at step {} of {} \xE2\x80\x94 partial result", stopped->at, stopped->of);
    }
    const auto bad = static_cast<std::size_t>(std::ranges::count(findings, Level::Suspicious, &Finding::level));
    const auto look = static_cast<std::size_t>(std::ranges::count(findings, Level::WorthALook, &Finding::level));
    if (bad == 0 && look == 0) {
        return std::format("Nothing suspicious found in {} places. That's not a guarantee \xE2\x80\x94 it means nothing I checked looks wrong.",
                           places);
    }
    const auto suspicious = std::format("{} {} suspicious", bad, bad == 1 ? "thing looks" : "things look");
    if (look == 0) {
        return suspicious;
    }
    if (bad == 0) {
        return std::format("Nothing suspicious; {} {} worth a look", look, look == 1 ? "thing is" : "things are");
    }
    return std::format("{}, {} {} worth a look", suspicious, look, look == 1 ? "is" : "are");
}

} // namespace tools::winsys
