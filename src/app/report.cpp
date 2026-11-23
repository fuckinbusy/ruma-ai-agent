#include "app/report.hpp"

#include "core/json.hpp"
#include "core/utf8.hpp"

#include <glaze/glaze.hpp>
#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <vector>

namespace app::report {
namespace {

// system.info's answer as the report takes it: Windows' drive, the first real graphics card.
auto machine_of(const glz::generic& d) -> Machine {
    Machine m;
    const auto number = [](const glz::generic& j, const char* key) {
        return j.is_object() && j.contains(key) && j[key].is_number() ? j[key].get_number() : 0.0;
    };
    m.cpu_percent = number(d["cpu"], "load_percent");
    m.memory_total = static_cast<std::uint64_t>(number(d["memory"], "total"));
    m.memory_available = static_cast<std::uint64_t>(number(d["memory"], "available"));
    wchar_t windows[MAX_PATH + 1]{};
    GetWindowsDirectoryW(windows, MAX_PATH + 1);
    const auto system_drive = core::utf8::to_utf8(std::wstring_view{windows, 2}).value_or("C:");
    if (d.contains("disks") && d["disks"].is_array()) {
        for (const auto& disk : d["disks"].get_array()) {
            if (disk.contains("drive") && disk["drive"].is_string() && disk["drive"].get_string() == system_drive) {
                m.disk_total = static_cast<std::uint64_t>(number(disk, "total"));
                m.disk_free = static_cast<std::uint64_t>(number(disk, "free"));
            }
        }
    }
    if (d.contains("gpus") && d["gpus"].is_array()) {
        for (const auto& g : d["gpus"].get_array()) {
            if (g.contains("name") && g["name"].is_string() && g["name"].get_string().find("Basic Render") == std::string::npos) {
                m.gpu_name = g["name"].get_string();
                break;
            }
        }
    }
    return m;
}

constexpr double kGb = 1024.0 * 1024 * 1024;
constexpr float kHigh = 85.0f;       // memory or processor this busy is the problem
constexpr float kMemoryLine = 90.0f; // the memory tile's dashed line
constexpr double kLowDisk = 10.0;    // percent free

// Windows' own programs: never offered to close (process.kill refuses them anyway).
constexpr std::array<std::string_view, 16> kSystem{
    "system",       "registry",     "smss.exe",      "csrss.exe",     "wininit.exe",  "services.exe",
    "lsass.exe",    "winlogon.exe", "svchost.exe",   "dwm.exe",       "explorer.exe", "msmpeng.exe",
    "fontdrvhost.exe", "sihost.exe", "memory compression", "secure system"};

auto lower(std::string_view s) -> std::string {
    std::string out{s};
    std::ranges::transform(out, out.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
    return out;
}

auto gb(std::uint64_t bytes) -> double {
    return static_cast<double>(bytes) / kGb;
}

// "1.9 GB", "640 MB".
auto size_text(std::uint64_t bytes) -> std::string {
    const double g = gb(bytes);
    return g >= 0.95 ? std::format("{:.1f} GB", g) : std::format("{:.0f} MB", g * 1024.0);
}

auto percent(std::uint64_t part, std::uint64_t whole) -> float {
    return whole == 0 ? 0.0f : static_cast<float>(100.0 * static_cast<double>(part) / static_cast<double>(whole));
}

auto series_or(const monitor::Snapshot& minute, monitor::Metric m, std::optional<float> now) -> std::vector<float> {
    auto s = minute.series[static_cast<std::size_t>(m)];
    if (s.empty() && now) {
        s.push_back(*now); // no minute yet: the one reading
    }
    return s;
}

} // namespace

namespace detail { // not an unnamed namespace: glaze reflects their members by name

struct SummaryTile {
    std::string name, value, unit, state;
};
struct Summary {
    std::string sentence; // the program's own words, its **bold** gone: what the tiles say
    std::vector<SummaryTile> tiles;
    std::string say = "The window shows these tiles. Begin the answer with what they say, in the user's language, "
                      "with these numbers; then go on.";
};

} // namespace detail

auto with_report_summary(std::string_view tool_json, const events::Report& r) -> std::string {
    detail::Summary s;
    for (const char c : r.headline) {
        if (c != '*') {
            s.sentence += c;
        }
    }
    for (const auto& t : r.tiles) {
        s.tiles.push_back({t.name, t.value, t.unit, t.status});
    }
    const auto summary = core::json::write(s).value_or(std::string{"{}"});
    // Into the result's object, before its closing brace; a result of another shape: beside it.
    auto end = tool_json.find_last_not_of(" \t\r\n");
    if (end != std::string_view::npos && tool_json[end] == '}' && tool_json.find_first_not_of(" \t\r\n") != std::string_view::npos &&
        tool_json[tool_json.find_first_not_of(" \t\r\n")] == '{') {
        std::string out{tool_json.substr(0, end)};
        const bool empty = out.find_last_not_of(" \t\r\n{") == std::string::npos || out.back() == '{';
        out += empty ? "\"report_summary\":" : ",\"report_summary\":";
        out += summary;
        out += '}';
        return out;
    }
    return std::format("{{\"result\":{},\"report_summary\":{}}}", tool_json.empty() ? std::string_view{"null"} : tool_json, summary);
}

auto make_report(const Machine& m, std::span<const Proc> processes, std::size_t process_count,
                 const monitor::Snapshot& minute, const monitor::Temperature& temperature, std::uint32_t self) -> events::Report {
    events::Report r;
    r.process_count = process_count;

    // A program's processes together, biggest first.
    struct Group {
        std::string name;
        std::string key;
        std::uint64_t memory = 0;
        double cpu = 0;
        std::size_t count = 0;
        std::uint32_t pid = 0;
        std::uint64_t biggest = 0;
        bool readable = true;
        bool self = false;
    };
    std::vector<Group> groups;
    for (const auto& p : processes) {
        const auto key = lower(p.name);
        auto it = std::ranges::find(groups, key, &Group::key);
        if (it == groups.end()) {
            groups.push_back({.name = p.name, .key = key});
            it = groups.end() - 1;
        }
        it->memory += p.memory;
        it->cpu += p.cpu;
        ++it->count;
        it->readable = it->readable && p.readable;
        it->self = it->self || p.pid == self;
        if (p.memory >= it->biggest) {
            it->biggest = p.memory;
            it->pid = p.pid;
        }
    }
    std::ranges::stable_sort(groups, [](const Group& a, const Group& b) { return a.memory > b.memory; });
    const auto closable = [](const Group& g) {
        return g.count == 1 && g.readable && !g.self && std::ranges::find(kSystem, std::string_view{g.key}) == kSystem.end();
    };
    for (std::size_t i = 0; i < groups.size() && r.processes.size() < 5; ++i) {
        const auto& g = groups[i];
        r.processes.push_back({.pid = g.pid,
                               .name = g.name,
                               .note = g.count > 1 ? std::format("{} processes", g.count) : std::string{},
                               .cpu = static_cast<float>(g.cpu),
                               .memory = g.memory,
                               .closable = closable(g)});
    }

    // The tiles.
    const std::uint64_t used = m.memory_total - std::min(m.memory_total, m.memory_available);
    const float memory_p = percent(used, m.memory_total);
    const float cpu_p = minute.latest.cpu.value_or(static_cast<float>(m.cpu_percent));
    const std::uint64_t disk_used = m.disk_total - std::min(m.disk_total, m.disk_free);
    const double free_p = m.disk_total == 0 ? 100.0 : 100.0 * static_cast<double>(m.disk_free) / static_cast<double>(m.disk_total);
    const bool memory_high = memory_p >= kHigh;
    const bool cpu_high = cpu_p >= kHigh;
    const bool disk_low = free_p < kLowDisk;
    const auto gpu = minute.latest.gpu;
    const bool gpu_high = gpu && *gpu >= kHigh;
    r.tiles.push_back({.name = "Memory",
                       .value = std::format("{:.1f}", gb(used)),
                       .unit = std::format("of {:.0f} GB", std::round(gb(m.memory_total))),
                       .status = memory_high ? "high" : "normal",
                       .tone = memory_high ? events::Tone::Warn : events::Tone::Ok,
                       .series = series_or(minute, monitor::Metric::Memory, memory_p),
                       .line = kMemoryLine});
    r.tiles.push_back({.name = "Processor",
                       .value = std::format("{:.0f}%", cpu_p),
                       .unit = monitor::temperature_text(temperature),
                       .status = cpu_high ? "busy" : "normal",
                       .tone = cpu_high ? events::Tone::Warn : events::Tone::Ok,
                       .series = series_or(minute, monitor::Metric::Cpu, cpu_p),
                       .line = std::nullopt});
    r.tiles.push_back({.name = "Graphics",
                       .value = gpu ? std::format("{:.0f}%", *gpu) : std::string{"\xE2\x80\x94"},
                       .unit = gpu_short(m.gpu_name),
                       .status = !gpu ? "not measured" : gpu_high ? "busy" : "normal",
                       .tone = !gpu ? events::Tone::Plain : gpu_high ? events::Tone::Warn : events::Tone::Ok,
                       .series = series_or(minute, monitor::Metric::Gpu, gpu),
                       .line = std::nullopt});
    r.tiles.push_back({.name = "Disk C:",
                       .value = std::format("{:.0f}", gb(disk_used)),
                       .unit = std::format("of {:.0f} GB", gb(m.disk_total)),
                       .status = disk_low ? "low space" : "normal",
                       .tone = disk_low ? events::Tone::Warn : events::Tone::Ok,
                       .series = series_or(minute, monitor::Metric::Disk, percent(disk_used, m.disk_total)),
                       .line = std::nullopt});

    // What can be done: close a program that holds a lot, one Close at a time.
    for (const auto& g : groups) {
        if (r.advice.size() >= 2) {
            break;
        }
        if (closable(g) && gb(g.memory) >= 0.3) {
            r.advice.push_back({.action = std::format("close:{}", g.pid),
                                .title = "Close " + g.name,
                                .sub = std::format("it uses {} of memory", size_text(g.memory)),
                                .gain = "+" + size_text(g.memory),
                                .kind = events::Kind::System});
        }
    }

    // The sentence.
    std::string rest;
    const auto fine = [&](std::vector<std::string_view> parts) {
        if (parts.empty()) {
            return std::string{};
        }
        std::string s = " The ";
        for (std::size_t i = 0; i < parts.size(); ++i) {
            s += i == 0 ? "" : i + 1 == parts.size() ? " and " : ", ";
            s += parts[i];
        }
        return s + (parts.size() == 1 ? " is fine." : " are fine.");
    };
    if (memory_high) {
        std::string who;
        if (groups.size() >= 2 && used > 0) {
            const double share = 100.0 * static_cast<double>(groups[0].memory + groups[1].memory) / static_cast<double>(used);
            who = std::format(": {} and {} hold {:.0f}% of it", groups[0].name, groups[1].name, share);
        } else if (!groups.empty()) {
            who = std::format(": {} holds the most", groups[0].name);
        }
        std::vector<std::string_view> ok;
        if (!cpu_high) {
            ok.push_back("processor");
        }
        if (gpu && !gpu_high) {
            ok.push_back("graphics");
        }
        if (!disk_low) {
            ok.push_back("disk");
        }
        r.headline = std::format("Not enough **memory** \xE2\x80\x94 {:.0f}% used{}.", memory_p, who) + fine(ok);
    } else if (cpu_high) {
        auto busiest = groups;
        std::ranges::stable_sort(busiest, [](const Group& a, const Group& b) { return a.cpu > b.cpu; });
        r.headline = std::format("The **processor** is busy \xE2\x80\x94 {:.0f}%{}.", cpu_p,
                                 busiest.empty() ? std::string{} : std::format(": {} uses the most", busiest[0].name)) +
                     fine(disk_low ? std::vector<std::string_view>{"memory"} : std::vector<std::string_view>{"memory", "disk"});
    } else if (disk_low) {
        r.headline = std::format("The **disk** is nearly full \xE2\x80\x94 {} free.", size_text(m.disk_free)) +
                     fine({"memory", "processor"});
    } else {
        r.headline = std::format("Nothing is overloaded: memory is {:.0f}% used, the processor at {:.0f}%.", memory_p, cpu_p);
    }
    return r;
}

void refresh(events::Report& r, const monitor::Snapshot& now) {
    // Each part only when the monitor has it: before its first reading nothing changes.
    const auto take = [&](events::Tile& t, monitor::Metric m) {
        const auto& s = now.series[static_cast<std::size_t>(m)];
        if (!s.empty()) {
            t.series = s;
        }
    };
    if (now.samples != 0) {
        r.readings = now.samples; // a reading more: the tiles' lines move on (MS § 7)
    }
    for (auto& t : r.tiles) {
        if (t.name == "Memory") {
            take(t, monitor::Metric::Memory);
            if (now.latest.memory_total > 0) {
                t.value = std::format("{:.1f}", gb(now.latest.memory_used));
            }
        } else if (t.name == "Processor") {
            take(t, monitor::Metric::Cpu);
            if (now.latest.cpu) {
                t.value = std::format("{:.0f}%", *now.latest.cpu);
            }
        } else if (t.name == "Graphics") {
            take(t, monitor::Metric::Gpu);
            if (now.latest.gpu) {
                t.value = std::format("{:.0f}%", *now.latest.gpu);
            }
        } else if (t.name == "Disk C:") {
            take(t, monitor::Metric::Disk);
        }
    }
}

auto disk_quick(const Machine& m, double seconds) -> events::Quick {
    const std::uint64_t used = m.disk_total - std::min(m.disk_total, m.disk_free);
    events::Quick q;
    q.value = std::format("{:.0f} GB", gb(m.disk_free));
    q.unit = std::format("free of {:.0f}", gb(m.disk_total));
    q.sub = std::format("Disk C: \xC2\xB7 {:.0f}% used", percent(used, m.disk_total));
    q.segments.push_back({std::format("Used {:.0f} GB", gb(used)),
                          m.disk_total == 0 ? 0.0 : static_cast<double>(used) / static_cast<double>(m.disk_total)});
    q.chip = std::format("{:.1f} s", seconds);
    return q;
}

auto gpu_short(std::string_view name) -> std::string {
    std::string s{name};
    for (const std::string_view mark : {"(TM)", "(tm)", "(R)", "(r)"}) {
        for (auto at = s.find(mark); at != std::string::npos; at = s.find(mark)) {
            s.erase(at, mark.size());
        }
    }
    // The maker and the word "Graphics" go; the model stays.
    std::string out;
    for (std::size_t pos = 0; pos < s.size();) {
        const auto end = std::min(s.find(' ', pos), s.size());
        const std::string_view word{s.data() + pos, end - pos};
        if (!word.empty() && word != "NVIDIA" && word != "GeForce" && word != "AMD" && word != "Intel" && word != "Graphics") {
            out += out.empty() ? "" : " ";
            out += word;
        }
        pos = end + 1;
    }
    return out;
}

auto machine_from_json(std::string_view system_info) -> std::optional<Machine> {
    glz::generic j;
    if (glz::read_json(j, system_info) || !j.is_object() || !j.contains("data") || !j["data"].is_object()) {
        return std::nullopt;
    }
    return machine_of(j["data"]);
}

} // namespace app::report
