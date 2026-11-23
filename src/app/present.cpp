#include "app/present.hpp"

#include "markdown/markdown.hpp"
#include "app/report.hpp"
#include "app/sizes.hpp"
#include "app/space.hpp"
#include "core/json.hpp"
#include "core/utf8.hpp"
#include "security/paths.hpp"
#include "tools/docs/docs.hpp"
#include "tools/docs/numbers.hpp"
#include "tools/tool.hpp"
#include "tools/winsys/check.hpp"

#include <glaze/glaze.hpp>
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <numeric>
#include <span>

namespace app::present {

namespace {

constexpr std::string_view kTerminal = "PowerShell \xC2\xB7 agent";

auto parsed(std::string_view json) -> std::optional<glz::generic> {
    glz::generic j;
    if (glz::read_json(j, json) || !j.is_object()) {
        return std::nullopt;
    }
    return j;
}

auto text(const glz::generic& j, const char* key) -> std::string {
    return j.is_object() && j.contains(key) && j[key].is_string() ? j[key].get_string() : std::string{};
}

// A path's last part: "C:\Users\user\Desktop" -> "Desktop".
auto leaf(std::string_view path) -> std::string {
    while (!path.empty() && (path.back() == '\\' || path.back() == '/')) {
        path.remove_suffix(1);
    }
    const auto at = path.find_last_of("\\/");
    return std::string{at == std::string_view::npos ? path : path.substr(at + 1)};
}

auto parent(std::string_view path) -> std::string {
    const auto at = path.find_last_of("\\/");
    return at == std::string_view::npos ? std::string{} : std::string{path.substr(0, at)};
}

// The paths of a list argument: fs.trash's "paths", fs.move's "moves" (`side`: "from" or "to").
auto paths_of(const glz::generic& j, const char* list, const char* side = nullptr) -> std::vector<std::string> {
    std::vector<std::string> out;
    if (j.is_object() && j.contains(list) && j[list].is_array()) {
        for (const auto& p : j[list].get_array()) {
            if (side == nullptr && p.is_string()) {
                out.push_back(p.get_string());
            } else if (side != nullptr) {
                out.push_back(text(p, side));
            }
        }
    }
    return out;
}

auto items(std::size_t n, std::string_view one, const std::string& first) -> std::string {
    return n == 1 ? std::string{one} + " " + leaf(first) : std::format("{} {} items", one, n);
}

auto kind_of(std::string_view tool) -> events::Kind {
    if (tool == "fs.delete_permanent") {
        return events::Kind::Delete; // hidden from find() unless advanced: a delete all the same
    }
    const auto* t = tools::find(tool);
    if (t == nullptr) {
        return events::Kind::Read;
    }
    switch (t->risk) {
    case security::Risk::Read:
    case security::Risk::Clipboard:
    case security::Risk::Screen:
    case security::Risk::Look: return events::Kind::Read;
    case security::Risk::Startup: return events::Kind::System;
    case security::Risk::Write:
    case security::Risk::RenameMove: return events::Kind::Change;
    case security::Risk::Delete: return events::Kind::Delete;
    default: return events::Kind::System;
    }
}

auto changes(std::string_view tool) -> bool {
    return kind_of(tool) != events::Kind::Read;
}

auto about_space(std::string_view request) -> bool {
    std::string low{request};
    std::ranges::transform(low, low.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
    for (const std::string_view word : {"space", "disk", "drive", "storage"}) {
        if (low.find(word) != std::string::npos) {
            return true;
        }
    }
    return false;
}

auto error_of(const tools::Called& c) -> std::string {
    if (c.asked && !c.allowed) {
        return "You said no";
    }
    if (c.outcome.code == 3 && !c.ruling.reason.empty()) {
        return c.ruling.reason;
    }
    if (const auto j = parsed(c.outcome.json); j && !text(*j, "error").empty()) {
        return text(*j, "error");
    }
    return "It did not work";
}

// ---- phase 8: data sets, numbered across the run (the sheet keeps a session's sets together)

std::atomic<std::uint32_t> g_last_set{0};

auto number(const glz::generic& j, const char* key) -> double {
    return j.is_object() && j.contains(key) && j[key].is_number() ? j[key].get_number() : 0.0;
}

auto plural(std::size_t n, std::string_view one, std::string_view many) -> std::string {
    return std::format("{} {}", n, n == 1 ? one : many);
}

// "report.PDF" -> "pdf"; none: empty.
auto extension(std::string_view name) -> std::string {
    const auto dot = name.find_last_of('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 == name.size()) {
        return {};
    }
    std::string ext{name.substr(dot + 1)};
    std::ranges::transform(ext, ext.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
    return ext;
}

// "2026-10-01T12:00:00Z" as seconds since 1970 (UTC), for sorting; not one: 0.
auto seconds_of(std::string_view iso) -> double {
    const auto field = [&](std::size_t at, std::size_t len) {
        int v = -1;
        if (at + len <= iso.size()) {
            (void)std::from_chars(iso.data() + at, iso.data() + at + len, v);
        }
        return v;
    };
    const int y = field(0, 4), mo = field(5, 2), d = field(8, 2), h = field(11, 2), mi = field(14, 2), s = field(17, 2);
    if (y < 0 || mo < 1 || d < 1) {
        return 0.0;
    }
    const std::chrono::sys_days day = std::chrono::year_month_day{std::chrono::year{y}, std::chrono::month{static_cast<unsigned>(mo)},
                                                                  std::chrono::day{static_cast<unsigned>(d)}};
    return static_cast<double>(day.time_since_epoch().count()) * 86400.0 + std::max(h, 0) * 3600.0 + std::max(mi, 0) * 60.0 +
           std::max(s, 0);
}

auto data_of(std::string_view outcome, const char* list) -> std::optional<glz::generic> {
    auto j = parsed(outcome);
    if (!j || !j->contains("data") || !(*j)["data"].is_object() || !(*j)["data"].contains(list) ||
        !(*j)["data"][list].is_array()) {
        return std::nullopt;
    }
    return j;
}

// fs.list: each entry's type, name, size and date, the largest first.
auto files_set(std::string_view outcome) -> std::optional<events::DataSet> {
    const auto j = data_of(outcome, "entries");
    if (!j) {
        return std::nullopt;
    }
    const auto& data = (*j)["data"];
    const auto& entries = data["entries"].get_array();
    std::vector<const glz::generic*> order;
    for (const auto& e : entries) {
        order.push_back(&e);
    }
    std::ranges::stable_sort(order, std::greater{}, [](const glz::generic* e) { return number(*e, "size"); });
    const std::string path = text(*j, "path");
    events::DataSet s{.id = ++g_last_set,
                      .title = leaf(path),
                      .source = "fs.list " + path,
                      .location = path,
                      .columns = {{"", ui::work::ColumnKind::Icon, 22, false, false},
                                  {"Name", ui::work::ColumnKind::Text, 0, true, true},
                                  {"Size", ui::work::ColumnKind::Size, 176, false, true},
                                  {"Modified", ui::work::ColumnKind::Date, 104, false, true}},
                      .complete = true,
                      .kind = events::SetKind::Files};
    s.rows.columns = 4;
    std::size_t files = 0, folders = 0;
    std::uint64_t total = 0;
    for (const auto* e : order) {
        const std::string name = text(*e, "name");
        const std::string type = text(*e, "type");
        const bool folder = type == "dir" || type == "junction";
        const auto size = static_cast<std::uint64_t>(number(*e, "size"));
        const std::string modified = text(*e, "modified");
        (folder ? folders : files) += 1;
        total += size;
        s.rows.cells.push_back({folder ? std::string{"folder"} : extension(name), 0});
        s.rows.cells.push_back({name, 0});
        s.rows.cells.push_back({folder ? std::string{} : sizes::text(size), static_cast<double>(size)});
        s.rows.cells.push_back({modified.substr(0, std::min<std::size_t>(10, modified.size())), seconds_of(modified)});
    }
    s.footer = plural(files, "file", "files") + " \xC2\xB7 " + plural(folders, "folder", "folders") + " \xC2\xB7 " + sizes::text(total);
    if (const auto listed = static_cast<std::size_t>(number(data, "listed")); listed > entries.size()) {
        s.footer += std::format(" \xC2\xB7 {} more not listed", listed - entries.size());
    }
    return s;
}

// fs.search (phase 12 § 2): each line found (its file, the file's folder in full — what a click
// opens —, the line, its text), or each file, or each file's count.
auto matches_set(std::string_view outcome) -> std::optional<events::DataSet> {
    const auto j = data_of(outcome, "matches");
    if (!j) {
        return std::nullopt;
    }
    const auto& data = (*j)["data"];
    const std::string path = text(*j, "path");
    const auto& matches = data["matches"].get_array();
    const auto any = [&](const char* key) { return std::ranges::any_of(matches, [&](const auto& m) { return m.contains(key); }); };
    const bool lines = any("line");
    const bool counts = !lines && any("count");
    events::DataSet s{.id = ++g_last_set,
                      .title = "Found in " + leaf(path),
                      .source = "fs.search " + path,
                      .location = path,
                      .columns = {{"", ui::work::ColumnKind::Icon, 22, false, false},
                                  {"Name", ui::work::ColumnKind::Text, 0, true, true},
                                  {"Folder", ui::work::ColumnKind::Text, 0, true, true}},
                      .complete = true,
                      .kind = events::SetKind::Matches};
    if (lines) {
        s.columns.push_back({"Line", ui::work::ColumnKind::Number, 56, false, true});
        s.columns.push_back({"Text", ui::work::ColumnKind::Text, 0, true, false});
    } else if (counts) {
        s.columns.push_back({"Lines", ui::work::ColumnKind::Number, 56, false, true});
    }
    s.rows.columns = s.columns.size();
    // The tool's paths are relative to the folder searched; an absolute one stays as it is.
    const auto folder_of = [&](const std::string& found) {
        const bool absolute = found.starts_with("\\\\") || (found.size() > 1 && found[1] == ':');
        const std::string up = parent(found);
        return absolute ? up : up.empty() ? path : path + (path.ends_with('\\') ? "" : "\\") + up;
    };
    for (const auto& m : matches) {
        const std::string found = text(m, "path");
        s.rows.cells.push_back({extension(leaf(found)), 0});
        s.rows.cells.push_back({leaf(found), 0});
        s.rows.cells.push_back({folder_of(found), 0});
        if (lines) {
            const double line = number(m, "line");
            s.rows.cells.push_back({line > 0 ? std::format("{:.0f}", line) : std::string{}, line});
            s.rows.cells.push_back({text(m, "text"), 0});
        } else if (counts) {
            const double count = number(m, "count");
            s.rows.cells.push_back({std::format("{:.0f}", count), count});
        }
    }
    s.footer = std::format("{} found \xC2\xB7 {} files looked at", matches.size(),
                           static_cast<std::size_t>(number(data, "files_seen")));
    if (const auto large = static_cast<std::size_t>(number(data, "skipped_large")); large > 0) {
        s.footer += std::format(" \xC2\xB7 {} over 20 MB skipped", large);
    }
    if (data.contains("stopped_at")) {
        s.footer += std::format(" \xC2\xB7 stopped after 20 s, at file {}", static_cast<std::size_t>(number(data, "stopped_at")));
    }
    return s;
}

// process.list: the busiest first. Its processor column is a quarter second's reading (the user,
// 2026-10-29: every row grey under "IDLE — 0% CPU", nothing a person could read from it): said so in
// its title and its footer, no row greyed or grouped by it.
auto processes_set(std::string_view outcome) -> std::optional<events::DataSet> {
    const auto j = data_of(outcome, "processes");
    if (!j) {
        return std::nullopt;
    }
    const auto& data = (*j)["data"];
    std::vector<const glz::generic*> order;
    for (const auto& p : data["processes"].get_array()) {
        order.push_back(&p);
    }
    std::ranges::stable_sort(order, [](const glz::generic* a, const glz::generic* b) {
        const double ca = number(*a, "cpu_percent");
        const double cb = number(*b, "cpu_percent");
        return ca != cb ? ca > cb : number(*a, "memory") > number(*b, "memory");
    });
    events::DataSet s{.id = ++g_last_set,
                      .title = "Processes",
                      .source = "process.list",
                      .columns = {{"", ui::work::ColumnKind::Icon, 20, false, false},
                                  {"Name", ui::work::ColumnKind::Text, 0, true, true},
                                  {"PID", ui::work::ColumnKind::Mono, 60, false, true},
                                  {"CPU now", ui::work::ColumnKind::Percent, 168, false, true},
                                  {"Memory", ui::work::ColumnKind::Number, 76, false, true}},
                      .complete = true,
                      .kind = events::SetKind::Processes};
    s.rows.columns = 5;
    for (const auto* p : order) {
        const std::string name = text(*p, "name");
        const double cpu = number(*p, "cpu_percent");
        const double memory = number(*p, "memory");
        const double pid = number(*p, "pid");
        s.rows.cells.push_back({"process:" + name, 0});
        s.rows.cells.push_back({name, 0});
        s.rows.cells.push_back({std::format("{:.0f}", pid), pid});
        s.rows.cells.push_back({std::format("{:.1f}%", cpu), cpu});
        s.rows.cells.push_back({sizes::text(static_cast<std::uint64_t>(memory)), memory});
    }
    const auto total = data.contains("total") && data["total"].is_number() ? static_cast<std::size_t>(data["total"].get_number())
                                                                           : order.size();
    s.footer = plural(total, "process", "processes") + " \xC2\xB7 processor over a quarter second";
    return s;
}

// ---- phase 13a: what slows it (design/ui_system 01a-01e)

// Seconds since 1970 (UTC) as the user's local "2026-10-28 14:05".
// Through Windows' own zone, not std::chrono::current_zone(): that one loads the time zone
// database once and keeps it for the process.
auto local_when(std::int64_t utc) -> std::string {
    const auto ticks = static_cast<std::uint64_t>(utc + 11'644'473'600LL) * 10'000'000ULL; // 1601, in 100 ns
    const FILETIME ft{.dwLowDateTime = static_cast<DWORD>(ticks), .dwHighDateTime = static_cast<DWORD>(ticks >> 32)};
    SYSTEMTIME u{}, local{};
    if (!FileTimeToSystemTime(&ft, &u)) {
        return {};
    }
    const bool zoned = SystemTimeToTzSpecificLocalTime(nullptr, &u, &local) != FALSE;
    const SYSTEMTIME& t = zoned ? local : u;
    return std::format("{:04}-{:02}-{:02} {:02}:{:02}{}", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, zoned ? "" : " UTC");
}

auto windows_file(std::string_view path) -> bool {
    std::string low{path};
    std::ranges::transform(low, low.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return low.starts_with("c:\\windows\\");
}

// process.sample: each program's last seconds as a spark, the one keeping the processor busy in
// yellow (from 25 % on average), Windows' own with a "system" badge where Stop would be.
auto sample_set(std::string_view outcome) -> std::optional<events::DataSet> {
    const auto j = data_of(outcome, "processes");
    if (!j) {
        return std::nullopt;
    }
    const auto& data = (*j)["data"];
    const auto seconds = static_cast<int>(number(data, "seconds"));
    events::DataSet s{.id = ++g_last_set,
                      .title = std::format("Processes \xC2\xB7 last {} s", seconds),
                      .source = "process.sample",
                      .columns = {{"", ui::work::ColumnKind::Icon, 20, false, false},
                                  {"Program", ui::work::ColumnKind::Text, 0, true, true},
                                  {std::format("Last {} s", seconds), ui::work::ColumnKind::Spark, 120, false, false},
                                  {"CPU", ui::work::ColumnKind::Number, 60, false, true},
                                  {"Memory", ui::work::ColumnKind::Number, 76, false, true},
                                  {"", ui::work::ColumnKind::Action, 110, false, false}},
                      .complete = true,
                      .kind = events::SetKind::Processes};
    s.rows.columns = 6;
    double hottest = 25.0;
    for (const auto& p : data["processes"].get_array()) {
        std::vector<float> cpu;
        if (p.contains("cpu") && p["cpu"].is_array()) {
            for (const auto& v : p["cpu"].get_array()) {
                cpu.push_back(v.is_number() ? static_cast<float>(v.get_number()) : 0.0f);
            }
        }
        const double mean = cpu.empty() ? 0.0 : std::accumulate(cpu.begin(), cpu.end(), 0.0) / static_cast<double>(cpu.size());
        const std::string name = text(p, "name");
        const std::string path = text(p, "path");
        const bool system = windows_file(path);
        const auto row = s.rows.rows();
        if (mean >= hottest) {
            hottest = mean;
            s.rows.hot_row = static_cast<int>(row);
        }
        const double memory = number(p, "memory");
        s.rows.cells.push_back({"process:" + name, 0});
        s.rows.cells.push_back({name, 0});
        s.rows.cells.push_back({{}, mean});
        s.rows.cells.push_back({std::format("{:.1f}%", mean), mean});
        s.rows.cells.push_back({sizes::text(static_cast<std::uint64_t>(memory)), memory});
        // "Stop": "End process…" did not fit its button; the card says it in full.
        s.rows.cells.push_back({system ? std::string{} : std::string{"Stop"}, number(p, "pid")});
        s.rows.series.push_back(std::move(cpu));
        const bool background = p.contains("windowless") && p["windowless"].is_boolean() && p["windowless"].get_boolean();
        s.rows.notes.push_back(leaf(path.empty() ? name : path) + (background ? " \xC2\xB7 in the background" : ""));
        s.rows.badges.emplace_back(system ? "system" : "");
    }
    s.footer = std::format("{} \xC2\xB7 the busiest first", plural(s.rows.rows(), "program", "programs"));
    return s;
}

// startup.list: what starts with Windows — where it is written under its name, its impact on the
// boot (only the helper reads it: "unknown" and a Check deeper button without), on or off; the
// last boot in the footer once the helper read it. `id`: the same set again (after Check deeper).
auto startup_set(std::string_view outcome, std::uint32_t id = 0) -> std::optional<events::DataSet> {
    const auto j = data_of(outcome, "entries");
    if (!j) {
        return std::nullopt;
    }
    const auto& data = (*j)["data"];
    const bool deeper = data.contains("deeper") && data["deeper"].is_boolean() && data["deeper"].get_boolean();
    events::DataSet s{.id = id != 0 ? id : ++g_last_set,
                      .title = "Startup programs",
                      .source = "startup.list",
                      .columns = {{"Program", ui::work::ColumnKind::Text, 0, true, true},
                                  {"Where", ui::work::ColumnKind::Text, 150, false, true},
                                  {"Impact", ui::work::ColumnKind::Text, 150, false, true},
                                  {"Status", ui::work::ColumnKind::Text, 70, false, true}},
                      .complete = true,
                      .kind = events::SetKind::Table};
    if (!deeper) {
        s.columns.push_back({"", ui::work::ColumnKind::Action, 120, false, false});
    }
    s.rows.columns = deeper ? 4 : 5;
    for (const auto& e : (*j)["data"]["entries"].get_array()) {
        const bool on = !e.contains("enabled") || !e["enabled"].is_boolean() || e["enabled"].get_boolean();
        const double ms = e.contains("impact_ms") && e["impact_ms"].is_number() ? e["impact_ms"].get_number() : -1.0;
        s.rows.cells.push_back({text(e, "name"), 0});
        s.rows.cells.push_back({text(e, "place"), 0});
        s.rows.cells.push_back({ms < 0    ? std::string{"unknown"}
                                : ms == 0 ? std::string{"none recorded"} // not in the boot log: it slowed nothing it recorded
                                          : std::format("{} \xC2\xB7 {:.1f} s", ms >= 3000 ? "High" : ms >= 1000 ? "Medium" : "Low", ms / 1000.0),
                                ms});
        s.rows.cells.push_back({on ? "On" : "Off", on ? 1.0 : 0.0});
        if (!deeper) {
            s.rows.cells.push_back({ms < 0 ? std::string{"Check deeper"} : std::string{}, static_cast<double>(s.id)});
        }
        s.rows.notes.push_back(text(e, "file"));
    }
    s.footer = plural(s.rows.rows(), "program starts with Windows", "programs start with Windows");
    if (data.contains("last_boot_ms") && data["last_boot_ms"].is_number()) {
        s.footer += std::format(" \xC2\xB7 last boot {:.0f} s", data["last_boot_ms"].get_number() / 1000.0);
    }
    return s;
}

// service.list: looked at only (phase 13a rule 1) — no action column, a read-only badge in its title.
auto services_set(std::string_view outcome) -> std::optional<events::DataSet> {
    const auto j = data_of(outcome, "services");
    if (!j) {
        return std::nullopt;
    }
    events::DataSet s{.id = ++g_last_set,
                      .title = "Services",
                      .source = "service.list",
                      .columns = {{"Service", ui::work::ColumnKind::Text, 0, true, true},
                                  {"State", ui::work::ColumnKind::Text, 90, false, true},
                                  {"Start", ui::work::ColumnKind::Text, 140, false, true},
                                  {"Account", ui::work::ColumnKind::Text, 120, false, true}},
                      .complete = true,
                      .kind = events::SetKind::Table,
                      .read_only = true};
    s.rows.columns = 4;
    s.rows.dense = true;
    for (const auto& v : (*j)["data"]["services"].get_array()) {
        s.rows.cells.push_back({text(v, "display"), 0});
        s.rows.cells.push_back({text(v, "state"), 0});
        s.rows.cells.push_back({text(v, "start_type"), 0});
        s.rows.cells.push_back({text(v, "account"), 0});
        s.rows.notes.push_back(text(v, "name"));
    }
    s.footer = plural(s.rows.rows(), "service", "services") + " \xC2\xB7 looked at only: the agent cannot start, stop or change one";
    return s;
}

// eventlog.query: what crashed — a group a row, its newest message under it, how many times.
// apps.list (phase 13a, 02d): a program a row, the largest first — its initial in a square, its
// publisher under it, version, when installed, size (a dash when it does not say), Uninstall… (a
// badge for Windows' own parts). `named` gets each row's program for its button ("Blender 4.2 LTS 4.2.1").
auto apps_set(std::string_view outcome, std::vector<std::string>& named) -> std::optional<events::DataSet> {
    const auto j = data_of(outcome, "apps");
    if (!j) {
        return std::nullopt;
    }
    struct Row {
        const glz::generic* app;
        double size;
    };
    std::vector<Row> rows;
    for (const auto& a : (*j)["data"]["apps"].get_array()) {
        rows.push_back({&a, a.contains("size") && a["size"].is_number() ? a["size"].get_number() : -1.0});
    }
    std::ranges::stable_sort(rows, std::greater{}, &Row::size);
    events::DataSet s{.id = ++g_last_set,
                      .title = "Installed programs",
                      .source = "Uninstall keys (HKLM, HKCU) \xC2\xB7 read-only",
                      .columns = {{"", ui::work::ColumnKind::Icon, 34, false, false},
                                  {"Program", ui::work::ColumnKind::Text, 0, true, true},
                                  {"Version", ui::work::ColumnKind::Mono, 96, false, true},
                                  {"Installed", ui::work::ColumnKind::Date, 104, false, true},
                                  {"Size", ui::work::ColumnKind::Size, 150, false, true},
                                  {"", ui::work::ColumnKind::Action, 104, false, false}},
                      .complete = true,
                      .kind = events::SetKind::Table};
    s.rows.columns = s.columns.size();
    named.clear();
    std::size_t unsized = 0;
    for (const auto& r : rows) {
        const auto& a = *r.app;
        const auto name = text(a, "name");
        const auto version = text(a, "version");
        const auto date = text(a, "date"); // "20240808"
        std::string when;
        double day = 0;
        if (date.size() == 8) {
            const std::string iso = std::format("{}-{}-{}", date.substr(0, 4), date.substr(4, 2), date.substr(6, 2));
            day = seconds_of(iso);
            if (day > 0) {
                // The month by name from a table: chrono's "%b" goes through a locale, which keeps memory for the process.
                static constexpr std::array<std::string_view, 12> kMonths{"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
                const std::chrono::sys_days d{std::chrono::days{static_cast<int>(day / 86400.0)}};
                const std::chrono::year_month_day ymd{d};
                when = std::format("{} {} {}", static_cast<unsigned>(ymd.day()), kMonths[static_cast<unsigned>(ymd.month()) - 1],
                                   static_cast<int>(ymd.year()));
            }
        }
        const bool system = a.contains("system_component") && a["system_component"].is_boolean() && a["system_component"].get_boolean();
        s.rows.cells.push_back({"process:" + name, 0});
        s.rows.cells.push_back({name, 0});
        s.rows.cells.push_back({version, 0});
        s.rows.cells.push_back({when, day});
        if (r.size < 0) {
            ++unsized;
            s.rows.cells.push_back({"\xE2\x80\x94", 0});
        } else {
            s.rows.cells.push_back({sizes::text(static_cast<std::uint64_t>(r.size)), r.size});
        }
        s.rows.cells.push_back(system ? ui::work::Cell{"", -1} : ui::work::Cell{"Uninstall\xE2\x80\xA6", static_cast<double>(named.size())});
        s.rows.notes.push_back(text(a, "publisher"));
        s.rows.badges.push_back(system ? "system" : "");
        named.push_back(version.empty() ? name : name + " " + version);
    }
    s.footer = plural(rows.size(), "program", "programs");
    if (unsized > 0) {
        s.footer += std::format(" \xC2\xB7 {} do not say their size", unsized);
    }
    return s;
}

// system.check (phase 13a, 05b–05g): the report's every line for the panel, made once.
auto threat_view(const tools::winsys::CheckReport& r) -> events::ThreatView {
    using tools::winsys::Level;
    events::ThreatView v;
    v.verdict = r.verdict;
    v.suspicious = static_cast<std::size_t>(std::ranges::count(r.findings, Level::Suspicious, &tools::winsys::Finding::level));
    v.worth = static_cast<std::size_t>(std::ranges::count(r.findings, Level::WorthALook, &tools::winsys::Finding::level));
    v.fine = r.places_checked > r.findings.size() ? r.places_checked - r.findings.size() : 0;
    if (r.stopped) {
        v.stopped_at = r.stopped->at;
        v.caveat = std::format("Steps {}\xE2\x80\x93{} were not done: what they look at is not in this result.", r.stopped->at, r.stopped->of);
    } else if (r.findings.empty()) {
        const auto cut = v.verdict.find(". ");
        if (cut != std::string::npos) {
            v.caveat = v.verdict.substr(cut + 2);
            v.verdict.resize(cut + 1);
        }
    } else {
        v.caveat = "That's not a guarantee \xE2\x80\x94 it means nothing else I checked looks wrong.";
    }
    // Three or more of one rule (05d): one row with their count; opened, their names.
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 11> kMany{{
        {"unsigned-in-user-place", "unsigned programs start from your folders"},
        {"system-name-elsewhere", "Windows file names outside Windows' folders"},
        {"mining-pool", "connections to mining pools"},
        {"busy-when-idle", "hidden programs keep the PC busy while idle"},
        {"wmi-runs-something", "WMI subscriptions run something"},
        {"ifeo-debugger", "programs are opened through another"},
        {"winlogon-changed", "Winlogon values are changed"},
        {"defender-threat", "threats Defender found are still there"},
        {"unsigned-program-files", "programs are not signed"},
        {"missing-file", "autostart entries start missing files"},
        {"script-task", "tasks run scripts"},
    }};
    const auto same_kind = [&](const tools::winsys::Finding& a, const tools::winsys::Finding& b) { return a.rule == b.rule && a.level == b.level; };
    std::vector<bool> taken(r.findings.size(), false);
    for (std::size_t i = 0; i < r.findings.size(); ++i) {
        if (taken[i]) {
            continue;
        }
        const auto& f = r.findings[i];
        std::vector<std::size_t> kind;
        for (std::size_t j = i; j < r.findings.size(); ++j) {
            if (same_kind(f, r.findings[j])) {
                kind.push_back(j);
            }
        }
        if (kind.size() >= 3) {
            const auto words = std::ranges::find(kMany, std::string_view{f.rule}, &std::pair<std::string_view, std::string_view>::first);
            events::ThreatRow row{.level = f.level == Level::Suspicious ? 0 : 1,
                                  .title = std::format("{} {}", kind.size(), words != kMany.end() ? words->second : std::string_view{"findings of one kind"}),
                                  .sub = f.why.empty() ? std::string{} : f.why.front(),
                                  .why = {},
                                  .facts = {},
                                  .actions = {}};
            for (const auto j : kind) {
                row.why.push_back(r.findings[j].title);
                taken[j] = true;
            }
            v.rows.push_back(std::move(row));
            continue;
        }
        taken[i] = true;
        events::ThreatRow row{.level = f.level == Level::Suspicious ? 0 : 1, .title = f.title, .sub = {}, .why = f.why, .facts = {},
                              .actions = f.actions};
        for (std::size_t k = 0; k < f.why.size() && k < 3; ++k) {
            row.sub += (k == 0 ? "" : " \xC2\xB7 ") + f.why[k];
        }
        for (const auto& x : f.facts) {
            row.facts.push_back({x.key, x.value, x.read_only});
        }
        v.rows.push_back(std::move(row));
    }
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 8> kUnits{
        {{"place", "places"}, {"service", "services"}, {"task", "tasks"}, {"subscription", "subscriptions"},
         {"file", "files"}, {"place", "places"}, {"check", "checks"}, {"process", "processes"}}};
    for (std::size_t i = 0; i < r.stages.size(); ++i) {
        const auto& s = r.stages[i];
        const auto [one, many] = kUnits[std::min(i, kUnits.size() - 1)];
        v.stages.push_back({s.name, std::format("{} {}", s.places, s.places == 1 ? one : many),
                            s.state == tools::winsys::StageState::Done ? 1 : s.state == tools::winsys::StageState::Stopped ? 2 : 0});
    }
    for (const auto& n : r.not_read) {
        v.not_read.push_back(core::utf8::to_utf8(n).value_or("?"));
    }
    v.checked = std::format("{} places", r.places_checked);
    v.defender_on = r.defender_on;
    v.deeper = r.deeper;
    return v;
}

// The panel's set of a threat report, by its id: a new one, or the same one again (Check deeper).
auto threat_set(std::shared_ptr<const events::ThreatView> view, std::size_t checks, std::uint32_t id) -> events::DataSet {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    events::DataSet set{.id = id,
                        .title = "Threat check",
                        .source = std::format("{} checks \xC2\xB7 read-only{}", checks, view->deeper ? " \xC2\xB7 with administrator rights" : ""),
                        .snapshot = std::format("{:02}:{:02}:{:02}", now.wHour, now.wMinute, now.wSecond),
                        .complete = true,
                        .footer = view->verdict,
                        .kind = events::SetKind::Threat};
    set.threat = std::move(view);
    return set;
}

auto events_set(std::string_view outcome) -> std::optional<events::DataSet> {
    const auto j = data_of(outcome, "groups");
    if (!j) {
        return std::nullopt;
    }
    const auto& data = (*j)["data"];
    events::DataSet s{.id = ++g_last_set,
                      .title = "Crashes and slow boots",
                      .source = "eventlog.query",
                      .columns = {{"What", ui::work::ColumnKind::Text, 0, true, true},
                                  {"Times", ui::work::ColumnKind::Number, 60, false, true},
                                  {"Last", ui::work::ColumnKind::Date, 130, false, true}},
                      .complete = true,
                      .kind = events::SetKind::Table};
    s.rows.columns = 3;
    for (const auto& g : data["groups"].get_array()) {
        const double count = number(g, "count");
        const double last = number(g, "last");
        s.rows.cells.push_back({std::format("{} {}", text(g, "provider"), static_cast<long long>(number(g, "id"))), 0});
        s.rows.cells.push_back({std::format("{}", static_cast<long long>(count)), count});
        s.rows.cells.push_back({local_when(static_cast<std::int64_t>(last)), last});
        s.rows.notes.push_back(text(g, "message"));
    }
    s.footer = std::format("{} of {} events \xC2\xB7 last {} days", s.rows.rows(), static_cast<long long>(number(data, "total")),
                           static_cast<long long>(number(data, "days")));
    return s;
}

// A table of the model's answer too big for the text: its cells; a column of numbers ("3 MB",
// "12%") sorts by them.
// Rows of text as a table for the panel: the first row its header; a column whose every cell
// reads as a number (a size, a price, a count) sorts as numbers.
// "¥ 6 840.00": a sum with its symbol, thousands apart.
auto money_text(std::string_view symbol, double v) -> std::string {
    const auto cents = static_cast<long long>(std::llround(std::abs(v) * 100.0));
    std::string whole = std::to_string(cents / 100);
    for (auto at = static_cast<std::ptrdiff_t>(whole.size()) - 3; at > 0; at -= 3) {
        whole.insert(static_cast<std::size_t>(at), " ");
    }
    return std::format("{}{} {}.{:02}", v < 0 ? "-" : "", symbol, whole, cents % 100);
}

// "01 Sep 2026" from a date as Excel counts it.
auto date_shown(double serial) -> std::string {
    using namespace std::chrono;
    const year_month_day ymd{sys_days{year{1899} / December / 30} + days{static_cast<int>(std::floor(serial))}};
    static constexpr std::array<std::string_view, 12> kMonths{"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    return std::format("{:02} {} {}", static_cast<unsigned>(ymd.day()), kMonths[static_cast<unsigned>(ymd.month()) - 1], static_cast<int>(ymd.year()));
}

auto rows_set(const std::vector<std::vector<std::string>>& cells, std::string title, std::string source) -> events::DataSet {
    events::DataSet s{.id = ++g_last_set, .title = std::move(title), .source = std::move(source), .complete = true,
                      .kind = events::SetKind::Table};
    std::size_t cols = 0;
    for (const auto& row : cells) {
        cols = std::max(cols, row.size());
    }
    s.rows.columns = cols;
    // Phase 10: a column of dates sorts as dates ("DATE"), of money as numbers (its symbol), right.
    std::vector<tools::docs::ColumnKind> read(cols, tools::docs::ColumnKind::Text);
    std::optional<std::size_t> money_column;
    std::string_view symbol;
    for (std::size_t c = 0; c < cols; ++c) {
        std::vector<std::string> values;
        for (std::size_t r = 1; r < cells.size(); ++r) {
            values.push_back(c < cells[r].size() ? cells[r][c] : std::string{});
        }
        read[c] = cells.size() > 1 ? tools::docs::kind_of(values) : tools::docs::ColumnKind::Text;
        bool numbers = cells.size() > 1;
        for (std::size_t r = 1; r < cells.size() && numbers; ++r) {
            numbers = c < cells[r].size() && sizes::parse(cells[r][c]).has_value();
        }
        const std::string header = !cells.empty() && c < cells[0].size() ? cells[0][c] : std::string{};
        if (read[c] == tools::docs::ColumnKind::Date) {
            s.columns.push_back(ui::work::Column{header, ui::work::ColumnKind::Date, 118, false, true, "DATE"});
        } else if (read[c] == tools::docs::ColumnKind::Money) {
            std::string_view sym;
            for (const auto& v : values) {
                if (const auto m = tools::docs::money_of(v)) {
                    sym = m->symbol;
                    break;
                }
            }
            if (!money_column) {
                money_column = c;
                symbol = sym;
            }
            s.columns.push_back(ui::work::Column{header, ui::work::ColumnKind::Number, 112, false, true, std::string{sym}});
        } else {
            s.columns.push_back(numbers ? ui::work::Column{header, ui::work::ColumnKind::Number, 100, false, true, {}}
                                        : ui::work::Column{header, ui::work::ColumnKind::Text, 0, true, true, {}});
        }
    }
    double sum = 0;
    for (std::size_t r = 1; r < cells.size(); ++r) {
        for (std::size_t c = 0; c < cols; ++c) {
            const std::string cell = c < cells[r].size() ? cells[r][c] : std::string{};
            if (read[c] == tools::docs::ColumnKind::Date) {
                const auto d = tools::docs::date_of(cell);
                s.rows.cells.push_back({d ? date_shown(*d) : cell, d.value_or(0.0)});
            } else if (read[c] == tools::docs::ColumnKind::Money) {
                const auto m = tools::docs::money_of(cell);
                s.rows.cells.push_back({cell, m ? m->value : 0.0});
                sum += c == money_column && m ? m->value : 0.0;
            } else {
                s.rows.cells.push_back({cell, sizes::parse(cell).value_or(0.0)});
            }
        }
    }
    s.footer = plural(cells.empty() ? 0 : cells.size() - 1, "row", "rows");
    if (money_column) { // "24 rows · ¥ 6 840.00"
        s.footer += " \xC2\xB7 " + money_text(symbol, sum);
    }
    return s;
}

auto table_set(const markdown::Block& b, std::string_view heading) -> events::DataSet {
    return rows_set(b.cells, heading.empty() ? std::string{"Table"} : std::string{heading}, std::string{events::kAnswerSource});
}

// What a call ran, as the Activity tab shows it: a command's own text, or the tool and its target.
auto command_of(const ai::ToolCall& call, const std::optional<glz::generic>& args) -> std::string {
    if (args && call.name == "shell.run") {
        return text(*args, "command");
    }
    if (args && !text(*args, "path").empty()) {
        return call.name + " " + text(*args, "path");
    }
    return call.name;
}

} // namespace

auto answer_table(const markdown::Block& table) -> events::DataSet {
    return table_set(table, {});
}

auto checklist_of(std::string_view args) -> core::Result<events::Checklist> {
    const auto bad = [](std::string why) { return core::make_error(core::Code::InvalidArgument, "plan.update: " + std::move(why)); };
    const auto j = parsed(args);
    if (!j || !j->is_object() || !j->contains("items") || !(*j)["items"].is_array()) {
        return bad("give {\"items\":[{\"title\":...,\"state\":...}]}");
    }
    const auto& list = (*j)["items"].get_array();
    if (list.empty() || list.size() > 12) {
        return bad(std::format("1 to 12 items, not {}", list.size()));
    }
    events::Checklist plan;
    for (const auto& item : list) {
        const std::string title = text(item, "title");
        const std::string state = text(item, "state");
        if (title.empty()) {
            return bad("every item needs a title");
        }
        events::ItemState s{};
        if (state == "pending") {
            s = events::ItemState::Pending;
        } else if (state == "active") {
            s = events::ItemState::Active;
        } else if (state == "done") {
            s = events::ItemState::Done;
        } else if (state == "skipped") {
            s = events::ItemState::Skipped;
        } else {
            return bad(std::format("\"{}\" is not a state: pending, active, done or skipped", state));
        }
        plan.items.push_back({title, s});
    }
    return plan;
}

auto question_of(std::string_view args) -> core::Result<events::Question> {
    const auto bad = [](std::string why) { return core::make_error(core::Code::InvalidArgument, "ui.ask: " + std::move(why)); };
    const auto chars = [](std::string_view s) {
        return static_cast<std::size_t>(std::ranges::count_if(s, [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
    };
    const auto j = parsed(args);
    if (!j || !j->is_object() || !j->contains("options") || !(*j)["options"].is_array()) {
        return bad("give {\"question\":...,\"options\":[{\"label\":...,\"description\":...}],\"recommended\":...,\"multi\":...}");
    }
    events::Question q{.question = text(*j, "question")};
    if (q.question.empty()) {
        return bad("the question is empty");
    }
    const auto& list = (*j)["options"].get_array();
    if (list.size() < 2 || list.size() > 4) {
        return bad(std::format("2 to 4 options, not {}", list.size()));
    }
    for (const auto& o : list) {
        events::QuestionOption option{.label = text(o, "label"), .description = text(o, "description")};
        if (option.label.empty()) {
            return bad("every option needs a label");
        }
        if (chars(option.label) > 40) {
            return bad(std::format("\"{}\": a label is at most 40 characters", option.label));
        }
        if (chars(option.description) > 120) {
            return bad(std::format("\"{}\": a description is at most 120 characters", option.label));
        }
        q.options.push_back(std::move(option));
    }
    if (j->contains("recommended") && (*j)["recommended"].is_number()) {
        const double n = (*j)["recommended"].get_number();
        if (n < 1 || n > static_cast<double>(q.options.size()) || n != std::floor(n)) {
            return bad(std::format("recommended is an option's number, 1 to {}, or null", q.options.size()));
        }
        q.recommended = static_cast<std::size_t>(n) - 1;
    }
    q.multi = j->contains("multi") && (*j)["multi"].is_boolean() && (*j)["multi"].get_boolean();
    return q;
}

auto reply_json(const events::Reply& r) -> std::string {
    if (!r.answered) {
        return R"({"answered":false})";
    }
    const auto quoted = [](std::string_view s) {
        auto w = core::json::write(s);
        return w ? std::move(*w) : std::string{"\"\""};
    };
    std::string out = R"({"chosen":[)";
    for (std::size_t i = 0; i < r.chosen.size(); ++i) {
        out += (i == 0 ? "" : ",") + quoted(r.chosen[i]);
    }
    return out + R"(],"other":)" + quoted(r.other) + "}";
}

auto memory_set(std::span<const memory::Entry> entries, std::span<const tools::winsys::PrivateRule> rules) -> events::DataSet {
    events::DataSet s{.id = ++g_last_set,
                      .title = "Rules and memory",
                      .source = "memory.json",
                      .location = {},
                      .columns = {{"What", ui::work::ColumnKind::Text, 0, true, false},
                                  {"Date", ui::work::ColumnKind::Text, 104, false, false},
                                  {"", ui::work::ColumnKind::Action, 96, false, false}},
                      .complete = true,
                      .kind = events::SetKind::Memory};
    s.rows.columns = 3;
    const auto row = [&](std::string what, std::string date, std::string action, double id) {
        s.rows.cells.push_back({std::move(what), 0});
        s.rows.cells.push_back({std::move(date), 0});
        s.rows.cells.push_back({std::move(action), id});
    };
    s.rows.group_titles = {"", "PRIVATE WINDOWS"};
    row("Your rules (agent.md)", {}, "Open", 0);
    for (const auto& e : entries) {
        row(e.text, e.date, "Forget", static_cast<double>(e.id));
    }
    if (!entries.empty()) {
        row("Everything remembered", {}, "Forget all", -1);
    }
    s.rows.group.assign(s.rows.rows(), 0);
    // Phase 13a § 2.1: the windows the agent never looks at — its own signs, then the user's.
    for (const char* sign : {"Password managers", "Windows' sign-in and permission windows", "Private browser windows",
                             "Windows that keep themselves out of captures", "Windows with a password field in focus",
                             "Windows whose title looks like a bank's"}) {
        row(sign, "always on", {}, -2);
        s.rows.group.push_back(1);
    }
    for (const auto& r : rules) {
        row(tools::winsys::describe(r), r.date, "Forget", kPrivateRow + static_cast<double>(r.id));
        s.rows.group.push_back(1);
    }
    s.footer = entries.empty() ? std::string{"Nothing remembered yet"}
                               : std::format("{} remembered \xC2\xB7 at most {}", entries.size(), memory::kMaxEntries);
    return s;
}

auto job_json(const tools::jobs::State& s) -> std::string {
    const auto quoted = [](std::string_view v) {
        auto w = core::json::write(v);
        return w ? std::move(*w) : std::string{"\"\""};
    };
    return std::format(R"({{"job":{},"command":{},"running":{},"code":{},"timed_out":{},"stopped":{},"cut":{},"output":{},"next":{}}})",
                       s.id, quoted(s.command), s.running, s.running ? std::string{"null"} : std::to_string(s.code), s.timed_out,
                       s.stopped, s.cut, quoted(s.output), s.next);
}

auto suggestions(std::string_view args) -> std::optional<events::Suggestions> {
    const auto j = parsed(args);
    if (!j || !j->is_object() || j->get_object().size() != 1 || !j->contains("prompts") || !(*j)["prompts"].is_array()) {
        return std::nullopt;
    }
    const auto& list = (*j)["prompts"].get_array();
    if (list.size() > 3) {
        return std::nullopt;
    }
    events::Suggestions s;
    for (const auto& p : list) {
        if (!p.is_string()) {
            return std::nullopt;
        }
        const auto& prompt = p.get_string();
        const auto chars = std::ranges::count_if(prompt, [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; });
        if (chars == 0 || chars > 60) {
            return std::nullopt;
        }
        s.prompts.push_back(prompt);
    }
    return s;
}

// ---- the translation cards' tools: strict schemas (every field required, nothing else)

#define WA_EXAMPLE                                                                                                              \
    R"({"type":"array","description":"Three sentences (no more) in the language learnt, the word in each.","items":{"type":"object","properties":{)" \
    R"("text":{"type":"string"},"transcription":{"type":"string","description":"Pinyin with tone marks, IPA, or empty."},)"    \
    R"("translation":{"type":"string","description":"In the user's language."},)"                                             \
    R"("highlight":{"type":"string","description":"The word as it stands in text."}},)"                                        \
    R"("required":["text","transcription","translation","highlight"],"additionalProperties":false}})"
#define WA_TERM                                                                                                                 \
    R"({"type":"object","properties":{"word":{"type":"string"},"transcription":{"type":"string"},"meaning":{"type":"string"}},)" \
    R"("required":["word","transcription","meaning"],"additionalProperties":false})"
#define WA_LANGS                                                                                                                \
    R"("source":{"type":"string","description":"ISO code of the language translated from: zh, en, de..."},)"                   \
    R"("target":{"type":"string","description":"ISO code of the language translated to."},)"

const std::string_view kShowWordDescription =
    "Show a translation card for one word or character of another language that the user asked to translate or asked the "
    "meaning of: its reading, meaning, a short reference, a level, three examples and common words with it. Never for a "
    "question about a thing, a name or an event: answer those in text. Use it instead of writing the card as text; then "
    "answer in one short line.";
const std::string_view kShowWordSchema =
    R"({"type":"object","properties":{)" WA_LANGS
    R"("word":{"type":"string"},"transcription":{"type":"string"},"meaning":{"type":"string"},)"
    R"("reference":{"type":"string","description":"Part of speech, radical and strokes; or gender and forms."},)"
    R"("level":{"type":"string","description":"A level such as A2 or B1; empty if none."},)"
    R"("examples":)" WA_EXAMPLE R"(,"common":{"type":"array","description":"Up to six common words with it.","items":)" WA_TERM R"(}},)"
    R"("required":["source","target","word","transcription","meaning","reference","level","examples","common"],"additionalProperties":false})";
const std::string_view kShowPhraseDescription =
    "Show a card for a phrase the user wants to say in another language: the main way, where it fits, up to three other "
    "tones, why when there is a nuance (else empty), three examples. Use it instead of writing the card as text.";
const std::string_view kShowPhraseSchema =
    R"({"type":"object","properties":{)" WA_LANGS
    R"("main":{"type":"string"},"where":{"type":"string","description":"Where it fits, in the user's language."},)"
    R"("variants":{"type":"array","description":"Up to three other tones.","items":{"type":"object","properties":{"tone":{"type":"string"},"text":{"type":"string"}},)"
    R"("required":["tone","text"],"additionalProperties":false}},"why":{"type":"string"},"examples":)" WA_EXAMPLE R"(},)"
    R"("required":["source","target","main","where","variants","why","examples"],"additionalProperties":false})";
const std::string_view kShowTextDescription =
    "Show a text the user asked to translate, sentence by sentence with its key terms. The translation is put on the user's clipboard, unless the result says "
    "on_clipboard false (then the user copies it from the card). "
    "Use it instead of writing the translation as text; then answer in one or two short sentences.";
const std::string_view kShowTextSchema =
    R"({"type":"object","properties":{)" WA_LANGS
    R"("style":{"type":"string","enum":["business","neutral","casual"]},)"
    R"("pairs":{"type":"array","items":{"type":"object","properties":{"original":{"type":"string"},"translation":{"type":"string"},)"
    R"("transcription":{"type":"string"}},"required":["original","translation","transcription"],"additionalProperties":false}},)"
    R"("terms":{"type":"array","description":"Up to eight key terms.","items":)" WA_TERM R"(}},)"
    R"("required":["source","target","style","pairs","terms"],"additionalProperties":false})";

#undef WA_EXAMPLE
#undef WA_TERM
#undef WA_LANGS

namespace {

// Every key is a string in `j`.
auto strings(const glz::generic& j, std::initializer_list<const char*> keys) -> bool {
    return j.is_object() && std::ranges::all_of(keys, [&](const char* k) { return j.contains(k) && j[k].is_string(); });
}

auto list(const glz::generic& j, const char* key) -> const glz::generic::array_t* {
    return j.is_object() && j.contains(key) && j[key].is_array() ? &j[key].get_array() : nullptr;
}

auto examples_of(const glz::generic& j, std::vector<events::Example>& out) -> bool {
    const auto* items = list(j, "examples");
    if (items == nullptr || items->size() > 3) {
        return false;
    }
    for (const auto& e : *items) {
        if (!strings(e, {"text", "transcription", "translation", "highlight"})) {
            return false;
        }
        out.push_back({text(e, "text"), text(e, "transcription"), text(e, "translation"), text(e, "highlight")});
    }
    return true;
}

auto terms_of(const glz::generic& j, const char* key, std::vector<events::Word::Common>& out) -> bool {
    const auto* items = list(j, key);
    if (items == nullptr) {
        return false;
    }
    for (const auto& e : *items) {
        if (!strings(e, {"word", "transcription", "meaning"})) {
            return false;
        }
        out.push_back({text(e, "word"), text(e, "transcription"), text(e, "meaning")});
    }
    return true;
}

} // namespace

auto word_of(std::string_view args) -> std::optional<events::Word> {
    const auto j = parsed(args);
    if (!j || !strings(*j, {"source", "target", "word", "transcription", "meaning", "reference", "level"})) {
        return std::nullopt;
    }
    events::Word w{.source = text(*j, "source"), .target = text(*j, "target"), .word = text(*j, "word"),
                   .transcription = text(*j, "transcription"), .meaning = text(*j, "meaning"),
                   .reference = text(*j, "reference"), .level = text(*j, "level")};
    if (w.word.empty() || !examples_of(*j, w.examples) || !terms_of(*j, "common", w.common)) {
        return std::nullopt;
    }
    return w;
}

auto phrase_of(std::string_view args) -> std::optional<events::Phrase> {
    const auto j = parsed(args);
    if (!j || !strings(*j, {"source", "target", "main", "where", "why"})) {
        return std::nullopt;
    }
    events::Phrase p{.source = text(*j, "source"), .target = text(*j, "target"), .main = text(*j, "main"),
                     .where = text(*j, "where"), .why = text(*j, "why")};
    const auto* variants = list(*j, "variants");
    if (p.main.empty() || variants == nullptr || variants->size() > 3 || !examples_of(*j, p.examples)) {
        return std::nullopt;
    }
    for (const auto& v : *variants) {
        if (!strings(v, {"tone", "text"})) {
            return std::nullopt;
        }
        p.variants.push_back({text(v, "tone"), text(v, "text")});
    }
    return p;
}

auto text_of(std::string_view args) -> std::optional<events::TextTranslation> {
    const auto j = parsed(args);
    if (!j || !strings(*j, {"source", "target", "style"})) {
        return std::nullopt;
    }
    events::TextTranslation t{.source = text(*j, "source"), .target = text(*j, "target"), .style = text(*j, "style")};
    const auto* pairs = list(*j, "pairs");
    if (pairs == nullptr || pairs->empty() || !terms_of(*j, "terms", t.terms)) {
        return std::nullopt;
    }
    for (const auto& p : *pairs) {
        if (!strings(p, {"original", "translation", "transcription"})) {
            return std::nullopt;
        }
        t.pairs.push_back({text(p, "original"), text(p, "translation"), text(p, "transcription")});
    }
    return t;
}

namespace {

auto spaced(const events::TextTranslation& t) -> bool {
    return t.target != "zh" && t.target != "ja";
}

} // namespace

auto picture_event(const tools::ImageOut& image) -> events::Picture {
    return events::picture_of(image);
}

auto capture_event(const tools::Prepared& prepared) -> events::Capture {
    events::Capture c;
    c.preview = {.label = prepared.title, .path = {}, .width = prepared.preview.width, .height = prepared.preview.height,
                 .thumb_w = prepared.preview.width, .thumb_h = prepared.preview.height, .thumb_rgba = prepared.preview.bgra};
    for (std::size_t i = 0; i + 3 < c.preview.thumb_rgba.size(); i += 4) {
        std::swap(c.preview.thumb_rgba[i], c.preview.thumb_rgba[i + 2]); // BGRA -> RGBA
    }
    c.source = prepared.source;
    c.monitors = prepared.monitors;
    c.windows = prepared.windows;
    c.hole_x = prepared.hole_x;
    c.hole_y = prepared.hole_y;
    c.hole_w = prepared.hole_w;
    c.hole_h = prepared.hole_h;
    if (prepared.image) {
        c.width = prepared.image->source_width > 0 ? prepared.image->source_width : prepared.image->width;
        c.height = prepared.image->source_height > 0 ? prepared.image->source_height : prepared.image->height;
        c.format = prepared.image->mime == "image/png" ? "PNG" : "JPEG";
        c.bytes = prepared.image->bytes.size();
        c.sent_w = prepared.image->width;
        c.sent_h = prepared.image->height;
    }
    SYSTEMTIME now{};
    GetLocalTime(&now);
    c.taken_at = std::format("{:02}:{:02}:{:02}", now.wHour, now.wMinute, now.wSecond);
    return c;
}

auto copied_size(const events::TextTranslation& t) -> std::size_t {
    std::size_t n = 0;
    for (const auto& p : t.pairs) {
        n += (n > 0 && spaced(t) ? 1 : 0) + p.translation.size();
    }
    return n;
}

auto copied_text(const events::TextTranslation& t) -> std::string {
    const bool gap = spaced(t);
    std::string out;
    for (const auto& p : t.pairs) {
        if (!out.empty() && gap) {
            out += ' ';
        }
        out += p.translation;
    }
    return out;
}

auto shows(std::string_view tool) -> bool {
    return tool == kSuggest || tool == kShowWord || tool == kShowPhrase || tool == kShowText || tool == kPlanUpdate || tool == kAsk ||
           tool == kShellJob || tool == kShellStop || tool == kMemoryAdd || tool == kMemoryForget;
}

auto step_title(std::string_view tool, std::string_view args) -> std::string {
    const auto j = parsed(args);
    if (!j) {
        return std::string{tool};
    }
    const std::string path = text(*j, "path");
    if (tool == "fs.list") return "List " + leaf(path);
    if (tool == "fs.read") return "Read " + leaf(path);
    if (tool == "fs.stat") return "Look at " + leaf(path);
    if (tool == "fs.search") return "Search " + leaf(path);
    if (tool == "fs.write") return "Write " + leaf(path);
    if (tool == "fs.mkdir") return "Create folder " + leaf(path);
    if (tool == "fs.move") {
        const auto from = paths_of(*j, "moves", "from");
        return from.empty() ? std::string{"Move"} : items(from.size(), "Move", from.front());
    }
    if (tool == "fs.copy") {
        const auto from = paths_of(*j, "copies", "from");
        return from.empty() ? std::string{"Copy"} : items(from.size(), "Copy", from.front());
    }
    if (tool == "fs.trash" || tool == "fs.restore" || tool == "fs.delete_permanent") {
        const auto paths = paths_of(*j, "paths");
        const std::string_view verb = tool == "fs.trash" ? "Recycle" : tool == "fs.restore" ? "Restore" : "Delete for good";
        return paths.empty() ? std::string{verb} : items(paths.size(), verb, paths.front());
    }
    if (tool == "system.info") return "Read the computer's numbers";
    if (tool == "process.list") return "List running programs";
    if (tool == "window.active") return "See the active window";
    if (tool == "clipboard.read") return "Read the clipboard";
    if (tool == "clipboard.write") return "Put text on the clipboard";
    if (tool == "system.open") return "Open " + leaf(text(*j, "target"));
    if (tool == "image.view") return "Look at " + leaf(path);
    if (tool == "doc.read") return "Read " + leaf(path);
    if (tool == "doc.write") return "Create " + leaf(path);
    if (tool == "image.ocr") return "Read text in " + leaf(path);
    if (tool == "image.convert") {
        const auto pictures = paths_of(*j, "paths");
        return pictures.size() == 1 ? "Convert " + leaf(pictures.front()) : std::format("Convert {} pictures", pictures.size());
    }
    if (tool == "screen.capture") {
        const auto target = text(*j, "target");
        return target == "window" ? "Capture " + text(*j, "window") : target == "active" ? "Capture the active window" : "Capture the screen";
    }
    if (tool == "shell.run") {
        std::string command = text(*j, "command");
        if (command.size() > 80) {
            command = command.substr(0, 77) + "...";
        }
        return "Run: " + command;
    }
    if (tool == "process.kill") {
        return j->contains("pid") && (*j)["pid"].is_number() ? std::format("End process {:.0f}", (*j)["pid"].get_number())
                                                              : std::string{"End a process"};
    }
    if (tool == "watch.add") return "Watch " + text(*j, "what");
    return std::string{tool};
}

Task::Task(std::uint64_t task, std::string request, const agent::Host* host)
    : task_(task), request_(std::move(request)), host_(host), began_(std::chrono::steady_clock::now()) {}

auto Task::started(const ai::ToolCall& call) -> std::vector<events::AgentEvent> {
    std::vector<events::AgentEvent> out;
    if (shows(call.name)) {
        return out; // the model's chips: no step of the task
    }
    began_of_[call.id] = std::chrono::steady_clock::now();
    const std::uint32_t id = next_step_++;
    step_of_[call.id] = id;
    const std::string title = step_title(call.name, call.arguments);
    out.emplace_back(events::Thinking{title});
    const auto j = parsed(call.arguments);
    // What runs it, monospaced in the live block: the tool, or a command's first word.
    std::string tool = call.name;
    if (call.name == "shell.run" && j && j->contains("command") && (*j)["command"].is_string()) {
        const auto& command = (*j)["command"].get_string();
        const auto start = command.find_first_not_of(" \t");
        const auto end = start == std::string::npos ? std::string::npos : command.find_first_of(" \t|;", start);
        if (start != std::string::npos) {
            tool = command.substr(start, end == std::string::npos ? std::string::npos : end - start);
        }
    }
    out.emplace_back(events::Step{.id = id, .kind = kind_of(call.name), .state = events::StepState::Run, .title = title, .tool = std::move(tool)});
    if (call.name == "shell.run" && j) {
        plan_of_[call.id] = static_cast<int>(plan_.size());
        plan_.push_back({.title = title, .state = events::PlanState::Run, .note = {}});
        out.emplace_back(events::Plan{plan_});
        out.emplace_back(events::Output{.tab = 1,
                                        .tab_label = std::string{kTerminal},
                                        .text = "PS> " + text(*j, "command") + "\n",
                                        .command = true,
                                        .step = plan_of_[call.id]});
    }
    return out;
}

namespace {

// A tool's picture for the window: BGRA to RGBA.
auto rgba_of(const tools::Picture& p, int own_w, int own_h) -> events::Picture {
    events::Picture out{.label = {}, .path = {}, .width = own_w, .height = own_h, .thumb_w = p.width, .thumb_h = p.height, .thumb_rgba = p.bgra};
    for (std::size_t i = 0; i + 3 < out.thumb_rgba.size(); i += 4) {
        std::swap(out.thumb_rgba[i], out.thumb_rgba[i + 2]);
    }
    return out;
}

auto primary_of(std::string_view tag) -> std::string_view {
    return tag.substr(0, tag.find('-'));
}

// image.ocr for the Text panel (phase 10, screen 20).
auto text_set(const tools::OcrPage& page) -> events::DataSet {
    const auto name = core::utf8::to_utf8(page.path).value_or(std::string{});
    events::DataSet s{.id = ++g_last_set, .title = "Recognized text", .source = "image.ocr \xC2\xB7 Windows OCR", .complete = true,
                      .kind = events::SetKind::Text};
    const auto n = page.text.lines.size();
    s.footer = std::format("{} {} \xC2\xB7 {}", n, n == 1 ? "line" : "lines", primary_of(page.text.language));
    events::TextRead read;
    read.picture = rgba_of(page.picture, page.width, page.height);
    read.picture.label = leaf(name);
    read.picture.path = name;
    read.file = leaf(name);
    read.language = page.text.language;
    const float w = static_cast<float>(std::max(1, page.width));
    const float h = static_cast<float>(std::max(1, page.height));
    for (const auto& line : page.text.lines) {
        events::TextLine l{.text = line.text, .x = static_cast<float>(line.box.x) / w, .y = static_cast<float>(line.box.y) / h,
                           .w = static_cast<float>(line.box.w) / w, .h = static_cast<float>(line.box.h) / h, .doubtful = {}};
        // Each word where it is in the line, in order: the doubtful ones' bytes.
        std::size_t at = 0;
        for (const auto& word : line.words) {
            const auto found = line.text.find(word.text, at);
            if (found == std::string::npos) {
                continue;
            }
            if (word.doubtful) {
                l.doubtful.emplace_back(static_cast<std::uint32_t>(found), static_cast<std::uint32_t>(word.text.size()));
            }
            at = found + word.text.size();
        }
        read.lines.push_back(std::move(l));
    }
    for (const auto& lang : page.languages) {
        read.languages.push_back({.name = lang.name,
                                  .used = lang.tag == page.text.language || primary_of(lang.tag) == primary_of(page.text.language),
                                  .installed = lang.installed});
    }
    s.text_read = std::move(read);
    return s;
}

auto megabytes(std::uint64_t bytes) -> std::string {
    if (bytes < 100 * 1024) {
        return std::format("{} KB", (bytes + 1023) / 1024);
    }
    return std::format("{:.1f} MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
}

// "−89%", "+12%": what a smaller file saved, or what a larger one cost.
auto saved_of(std::uint64_t before, std::uint64_t after) -> std::string {
    if (before == 0) {
        return {};
    }
    const auto change = std::lround((static_cast<double>(after) / static_cast<double>(before) - 1.0) * 100.0);
    return change < 0 ? std::format("\xE2\x88\x92{}%", -change) : std::format("+{}%", change);
}

// image.convert for the Converted panel (phase 10, screen 21).
auto converted_set(const tools::Conversion& c) -> events::DataSet {
    events::DataSet s{.id = ++g_last_set, .title = "Converted images", .source = "image.convert", .complete = true,
                      .kind = events::SetKind::Converted};
    events::ConvertedSet v;
    const auto n = c.files.size();
    std::string format = c.format;
    std::ranges::transform(format, format.begin(), [](char ch) { return static_cast<char>(std::toupper(static_cast<unsigned char>(ch))); });
    v.heading = std::format("{} {}", n, n == 1 ? "image" : "images");
    if (!format.empty()) {
        v.heading += " \xE2\x86\x92 " + format;
    }
    if (c.max_side > 0) {
        v.heading += std::format(" \xC2\xB7 {} px", c.max_side);
    }
    const bool jpeg = std::ranges::any_of(c.files, [](const tools::ConvertedFile& f) { return f.to_format == "JPEG"; });
    v.source = jpeg ? std::format("image.convert \xC2\xB7 quality {}", c.quality) : std::string{"image.convert"};
    v.folder = core::utf8::to_utf8(c.folder).value_or(std::string{});
    std::uint64_t before = 0;
    std::uint64_t after = 0;
    for (const auto& f : c.files) {
        events::ConvertedRow row;
        row.from = leaf(core::utf8::to_utf8(f.from).value_or(std::string{}));
        row.to = leaf(core::utf8::to_utf8(f.to).value_or(std::string{}));
        row.from_format = f.from_format;
        row.to_format = f.to_format;
        row.before = std::format("{}\xC3\x97{} \xC2\xB7 {}", f.from_w, f.from_h, megabytes(f.from_bytes));
        row.after = std::format("{}\xC3\x97{} \xC2\xB7 {}", f.to_w, f.to_h, megabytes(f.to_bytes));
        if (f.rotated != 0) {
            row.after += std::format(" \xC2\xB7 rotated {}\xC2\xB0", f.rotated);
        }
        if (f.already) {
            row.after += " \xC2\xB7 already that size";
        }
        row.saved = saved_of(f.from_bytes, f.to_bytes);
        row.thumb = rgba_of(f.thumb, f.to_w, f.to_h);
        before += f.from_bytes;
        after += f.to_bytes;
        v.rows.push_back(std::move(row));
    }
    v.total = std::format("{} {} \xC2\xB7 {} \xE2\x86\x92 {}", n, n == 1 ? "file" : "files", megabytes(before), megabytes(after));
    s.footer = std::format("{} {} \xC2\xB7 {}", n, n == 1 ? "file" : "files", saved_of(before, after));
    s.converted = std::move(v);
    return s;
}

} // namespace

namespace {

// "C:\Users\<name>\Downloads\a.xlsx" -> "~\Downloads\a.xlsx": the user's own folder as ~.
auto home_short(std::string_view path) -> std::string {
    if (path.size() > 9 && path[1] == ':' && path.substr(2, 7) == "\\Users\\") {
        if (const auto after = path.find('\\', 9); after != std::string_view::npos) {
            return "~" + std::string{path.substr(after)};
        }
    }
    return std::string{path};
}

// A refusal's card (phase 10, screen 25): its pill, what happened to which file, its line in mono,
// what it means and what to do — the kind's own words around the tool's facts.
auto refusal_card(std::string_view kind, std::string_view file, std::string_view what, std::string_view facts, std::string_view path,
                  std::string_view error) -> events::Refusal {
    events::Refusal c{.kind = std::string{kind}, .label = {}, .title = {}, .line = std::string{facts}, .advice = {}, .whole = true};
    if (kind == "zip_bomb") {
        c.label = "ZIP bomb";
        c.title = std::format("{} \xE2\x80\x94 not opened", file);
        c.line = home_short(path);
        c.advice = std::format("It {}. Files like this are made on purpose to freeze programs \xE2\x80\x94 don't open it in Excel either.", facts);
    } else if (kind == "sheet_too_large") {
        c.label = "Sheet too large";
        c.title = std::format("{} \xE2\x80\x94 {} not read", file, what);
        c.advice = "That's more than can be read at once. Save the range you need into a separate file and I'll read it.";
    } else if (kind == "page_too_large") {
        c.label = "Page too large";
        c.title = std::format("{} \xE2\x80\x94 {} skipped", file, what);
        c.advice = "A page this big can't be drawn for text recognition. The other pages were read.";
    } else if (kind == "not_an_image") {
        c.label = "Not an image";
        c.title = std::format("{} \xE2\x80\x94 not opened", file);
        c.advice = "Only picture files open as pictures: .png .jpg .jpeg .gif .bmp .tiff .webp.";
    } else {
        c.label = "Too large";
        c.title = std::format("{} \xE2\x80\x94 not read", file);
        c.advice = std::string{error};
    }
    return c;
}

// A path to compare: Windows' names are any case.
auto lowered(std::wstring_view path) -> std::wstring {
    std::wstring out{path};
    std::ranges::transform(out, out.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return out;
}

// A document as Markdown, read from the file as it is now: a DOCX's text, an MD as it is; else nothing.
auto document_markdown(const std::wstring& path) -> std::optional<std::string> {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > tools::docs::kMaxDocument) {
        return std::nullopt;
    }
    std::ifstream in{std::filesystem::path{path}, std::ios::binary};
    const std::string bytes{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    const auto ext = lowered(std::filesystem::path{path}.extension().wstring());
    if (ext == L".md") {
        return bytes;
    }
    if (ext != L".docx") {
        return std::nullopt;
    }
    auto zip = tools::docs::ZipReader::open(std::as_bytes(std::span{bytes.data(), bytes.size()}));
    if (!zip) {
        return std::nullopt;
    }
    auto md = tools::docs::docx_markdown(*zip);
    return md ? std::optional<std::string>{std::move(*md)} : std::nullopt;
}

// What of a Markdown text a Word file keeps, as the card's chips: only what the text has.
auto kept_of_markdown(std::string_view md) -> std::vector<std::string> {
    bool headings = false, lists = false, table = false, emphasis = false, links = false;
    for (std::size_t at = 0; at < md.size();) {
        const auto end = std::min(md.find('\n', at), md.size());
        auto line = md.substr(at, end - at);
        at = end + 1;
        while (!line.empty() && line.front() == ' ') {
            line.remove_prefix(1);
        }
        headings = headings || line.starts_with('#');
        lists = lists || line.starts_with("- ") || line.starts_with("* ") ||
                (!line.empty() && std::isdigit(static_cast<unsigned char>(line.front())) && line.find(". ") != std::string_view::npos &&
                 line.find(". ") <= 3);
        table = table || line.starts_with('|');
        const auto body = line.starts_with("* ") ? line.substr(2) : line;
        emphasis = emphasis || body.find("**") != std::string_view::npos || body.find("__") != std::string_view::npos ||
                   std::ranges::count(body, '*') >= 2 || std::ranges::count(body, '_') >= 2;
        links = links || line.find("](") != std::string_view::npos;
    }
    std::vector<std::string> out;
    for (const auto& [has, name] : {std::pair{headings, "Headings"}, std::pair{lists, "Lists"}, std::pair{table, "Table"},
                                    std::pair{emphasis, "Bold \xC2\xB7 italic"}, std::pair{links, "Links"}}) {
        if (has) {
            out.emplace_back(name);
        }
    }
    return out;
}

} // namespace

auto Task::finished(const ai::ToolCall& call, const tools::Called& called) -> std::vector<events::AgentEvent> {
    std::vector<events::AgentEvent> out;
    if (shows(call.name)) {
        return out;
    }
    const bool ok = called.outcome.code == 0;
    changed_ = changed_ || (ok && changes(call.name));
    events::Step step{.id = step_of_.contains(call.id) ? step_of_[call.id] : next_step_++,
                      .kind = kind_of(call.name),
                      .state = ok ? events::StepState::Done : events::StepState::Failed,
                      .title = step_title(call.name, call.arguments)};
    if (!ok) {
        step.detail = error_of(called);
    }
    // Done on this PC, nothing of it sent: OCR, conversion, a PDF's scanned pages (phase 10).
    if (ok && call.name == "image.ocr") {
        step.local = "The image was not sent anywhere \xE2\x80\x94 Windows recognized it on this PC";
    } else if (ok && call.name == "image.convert") {
        step.local = "The images were not sent anywhere \xE2\x80\x94 converted on this PC";
    } else if (ok && called.outcome.document && std::ranges::any_of(called.outcome.document->read, &tools::PageRead::scan)) {
        step.local = "The scanned pages were not sent anywhere \xE2\x80\x94 Windows recognized them on this PC";
    }
    // Refused on purpose (phase 10, screen 25): a card each; the step "rejected", not "failed".
    std::vector<events::Refusal> cards;
    {
        const auto args = parsed(call.arguments);
        const auto path = args ? text(*args, "path") : std::string{};
        const auto file = leaf(path);
        if (const auto result = ok ? std::nullopt : parsed(called.outcome.json); result && result->contains("kind")) {
            cards.push_back(refusal_card(text(*result, "kind"), file, {}, text(*result, "facts"), path, text(*result, "error")));
            step.rejected = true;
        }
        if (ok && !called.outcome.skipped.empty()) {
            const bool nothing = called.outcome.tables.empty() && (!called.outcome.document || called.outcome.document->read.empty());
            for (const auto& s : called.outcome.skipped) {
                cards.push_back(refusal_card(s.kind, file, s.what, s.facts, path, {}));
                cards.back().whole = nothing;
            }
            if (nothing) {
                step.rejected = true;
            } else {
                const auto n = called.outcome.skipped.size();
                const bool pages = called.outcome.skipped.front().kind == "page_too_large";
                step.skipped = n == 1 ? std::format("a skipped {}", pages ? "page" : "sheet") : std::format("{} skipped {}", n, pages ? "pages" : "sheets");
            }
        }
        if (ok && !step.rejected && !file.empty() && (call.name == "doc.read" || call.name == "image.view" || call.name == "image.ocr")) {
            step.file = file;
        }
    }
    out.emplace_back(std::move(step));
    for (auto& c : cards) {
        out.emplace_back(std::move(c));
    }
    // The user's window (phase 13a § 2): always shown that it was looked at, or that it was not.
    if (const auto result = ok && call.name == "window.look" ? parsed(called.outcome.json) : std::nullopt;
        result && result->contains("data") && (*result)["data"].is_object()) {
        const auto& d = (*result)["data"];
        if (d.contains("sensitive") && d["sensitive"].is_boolean() && d["sensitive"].get_boolean()) {
            out.emplace_back(events::Looked{.sensitive = true, .reason = text(d, "reason")});
        } else if (d.contains("window") && d["window"].is_object()) {
            const bool picture = d.contains("picture") && d["picture"].is_boolean() && d["picture"].get_boolean();
            out.emplace_back(events::Looked{.program = text(d["window"], "program"), .title = text(d["window"], "title"), .text = !picture,
                                            .picture = picture});
        }
    }

    if (const auto p = plan_of_.find(call.id); p != plan_of_.end()) {
        plan_[static_cast<std::size_t>(p->second)].state = events::PlanState::Done;
        plan_[static_cast<std::size_t>(p->second)].note = ok ? std::string{} : std::string{"failed"};
        out.emplace_back(events::Plan{plan_});
    }

    const auto j = parsed(call.arguments);
    if (ok && call.name == "system.info") {
        system_info_ = called.outcome.json;
    } else if (ok && call.name == "process.list") {
        process_list_ = called.outcome.json;
    }
    if (!reported_ && !system_info_.empty() && !process_list_.empty()) {
        const auto machine = report::machine_from_json(system_info_);
        const auto list = parsed(process_list_);
        if (machine && list && list->contains("data") && (*list)["data"].is_object()) {
            const auto& data = (*list)["data"];
            std::vector<report::Proc> procs;
            if (data.contains("processes") && data["processes"].is_array()) {
                for (const auto& p : data["processes"].get_array()) {
                    const auto number = [&](const char* key) {
                        return p.contains(key) && p[key].is_number() ? p[key].get_number() : 0.0;
                    };
                    procs.push_back({.pid = static_cast<std::uint32_t>(number("pid")),
                                     .name = text(p, "name"),
                                     .memory = static_cast<std::uint64_t>(number("memory")),
                                     .cpu = number("cpu_percent"),
                                     .readable = !(p.contains("readable") && p["readable"].is_boolean()) || p["readable"].get_boolean()});
                }
            }
            const auto count = data.contains("total") && data["total"].is_number() ? static_cast<std::size_t>(data["total"].get_number())
                                                                                   : procs.size();
            monitor::Snapshot minute;
            monitor::Temperature temperature;
            if (host_ != nullptr && host_->monitor != nullptr) {
                minute = host_->monitor->snapshot();
                temperature = host_->monitor->temperature();
            }
            auto r = report::make_report(*machine, procs, count, minute, temperature, GetCurrentProcessId());
            closable_.clear();
            for (const auto& p : r.processes) {
                if (p.closable) {
                    closable_.emplace_back(p.pid, p.name);
                }
            }
            reported_ = true;
            out.emplace_back(std::move(r));
        }
    }

    // Phase 8: what it found, as a data set for the panel; the call as an Activity entry.
    if (ok) {
        std::optional<events::DataSet> set;
        if (call.name == "fs.list") {
            set = files_set(called.outcome.json);
        } else if (call.name == "fs.search") {
            set = matches_set(called.outcome.json);
        } else if (call.name == "process.list" &&
                   std::ranges::none_of(out, [](const auto& e) { return std::holds_alternative<events::Report>(e); })) {
            set = processes_set(called.outcome.json); // a report made here has them: no second table
        } else if (call.name == "process.sample") {
            set = sample_set(called.outcome.json); // phase 13a: what slows it, over time
            // Its Stop buttons name their programs to the model, as the report's do.
            for (std::size_t r = 0; set && r < set->rows.rows(); ++r) {
                const auto& action = set->rows.at(r, set->rows.columns - 1);
                const auto pid = static_cast<std::uint32_t>(action.number);
                if (!action.text.empty() && std::ranges::none_of(closable_, [&](const auto& c) { return c.first == pid; })) {
                    closable_.emplace_back(pid, set->rows.at(r, 1).text); // its Program column
                }
            }
        } else if (call.name == "startup.list") {
            set = startup_set(called.outcome.json);
            if (set) {
                startup_set_ = set->id;
            }
        } else if (call.name == "service.list") {
            set = services_set(called.outcome.json);
        } else if (call.name == "eventlog.query") {
            set = events_set(called.outcome.json);
        } else if (call.name == "system.check" && called.outcome.check) { // the threat report (phase 13a § 5)
            threat_ = std::make_shared<const events::ThreatView>(threat_view(*called.outcome.check));
            threat_set_ = ++g_last_set;
            set = threat_set(threat_, called.outcome.check->stages.size(), threat_set_);
        } else if (call.name == "apps.list") {
            set = apps_set(called.outcome.json, uninstallable_);
        } else if (call.name == "disk.usage" && called.outcome.disk) { // what takes the space: the whole tree (phase 13a § 4)
            SYSTEMTIME now{};
            GetLocalTime(&now);
            const auto counted = disk_set_of_.find(call.id); // the set its counting showed: the same one, whole
            const std::uint32_t id = counted != disk_set_of_.end() ? counted->second : ++g_last_set;
            set = space::set_of(called.outcome.disk, {}, space::View::Folders, id,
                                std::format("{:02}:{:02}:{:02}", now.wHour, now.wMinute, now.wSecond));
        }
        if (set) {
            out.emplace_back(events::Data{std::move(*set), false});
        }
        // doc.read: a PDF by its pages — one Document set for its file, its next parts added (phase 10).
        if (const auto& doc = called.outcome.document) {
            auto& id = document_of_[doc->path];
            const bool append = id != 0;
            if (!append) {
                id = ++g_last_set;
            }
            const auto name = core::utf8::to_utf8(doc->path).value_or(std::string{});
            events::DataSet s{.id = id, .title = leaf(name), .source = std::format("doc.read \xC2\xB7 {} pages", doc->pages), .complete = true,
                              .kind = events::SetKind::Document};
            s.pages = events::Pages{.count = doc->pages, .part_size = doc->part_size, .mapped = doc->mapped, .note = doc->note, .read = {}};
            for (const auto& p : doc->read) {
                s.pages->read.push_back({.page = p.page, .scan = p.scan, .text = p.text});
            }
            // Its card's line ("48 pages · text + scans"); the model's merge keeps what it read before.
            const bool scans = std::ranges::any_of(doc->read, &tools::PageRead::scan);
            s.footer = std::format("{} pages \xC2\xB7 {}", doc->pages, scans ? "text + scans" : "text");
            out.emplace_back(events::Data{std::move(s), append});
        }
        // doc.write: the file's card and its Preview (phase 10, screen 24).
        if (const auto result = call.name == "doc.write" && j ? parsed(called.outcome.json) : std::nullopt; result && result->contains("data")) {
            const auto& data = (*result)["data"];
            events::Created card;
            card.path = text(data, "written");
            card.name = leaf(card.path);
            const auto format = text(*j, "format");
            card.type = format;
            std::ranges::transform(card.type, card.type.begin(), [](char ch) { return static_cast<char>(std::toupper(static_cast<unsigned char>(ch))); });
            const auto bytes = static_cast<std::uint64_t>(number(data, "bytes"));
            card.size = bytes < 1024 * 1024 ? std::format("{} KB", std::max<std::uint64_t>(1, (bytes + 512) / 1024))
                                             : std::format("{:.1f} MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
            auto folder = home_short(parent(card.path));
            card.folder = folder.starts_with("~\\") ? folder.substr(2) : folder;
            const auto source = "doc.write \xE2\x86\x92 " + card.type;
            if (format == "docx" || format == "md") {
                const auto md = text(*j, "markdown");
                card.kept = kept_of_markdown(md);
                events::DataSet page{.id = ++g_last_set, .title = card.name, .source = source, .complete = true, .kind = events::SetKind::Preview};
                page.preview = md;
                card.preview = page.id;
                if (const auto wide = core::utf8::to_wide(card.path)) {
                    preview_of_[lowered(*wide)] = page.id;
                }
                out.emplace_back(events::Data{std::move(page), false});
            } else if (j->contains("table") && (*j)["table"].is_object()) {
                // The table as written: its rows in the panel, its columns read as the writer read them.
                const auto& table = (*j)["table"];
                std::vector<std::vector<std::string>> rows(1);
                if (table.contains("columns") && table["columns"].is_array()) {
                    for (const auto& c : table["columns"].get_array()) {
                        rows[0].push_back(c.is_string() ? c.get_string() : std::string{});
                    }
                }
                if (table.contains("rows") && table["rows"].is_array()) {
                    for (const auto& r : table["rows"].get_array()) {
                        auto& row = rows.emplace_back();
                        if (r.is_array()) {
                            for (const auto& cell : r.get_array()) {
                                row.push_back(cell.is_string() ? cell.get_string() : std::string{});
                            }
                        }
                    }
                }
                if (format == "xlsx") {
                    bool money = false, dates = false;
                    for (std::size_t c = 0; c < rows[0].size(); ++c) {
                        std::vector<std::string> values;
                        for (std::size_t r = 1; r < rows.size(); ++r) {
                            values.push_back(c < rows[r].size() ? rows[r][c] : std::string{});
                        }
                        const auto kind = tools::docs::kind_of(values);
                        money = money || kind == tools::docs::ColumnKind::Money;
                        dates = dates || kind == tools::docs::ColumnKind::Date;
                    }
                    if (money) {
                        card.kept.emplace_back("Money as numbers");
                    }
                    if (dates) {
                        card.kept.emplace_back("Dates as dates");
                    }
                    card.kept.emplace_back("No formulas");
                } else {
                    card.kept = {"UTF-8", "Formula-safe: =, +, @ escaped"};
                }
                auto written = rows_set(rows, card.name, source);
                card.preview = written.id;
                out.emplace_back(events::Data{std::move(written), false});
            }
            out.emplace_back(std::move(card));
        }
        // A document with a Preview written again by any tool: its Preview as the file is now.
        for (const auto& effect : called.outcome.effects) {
            const bool writes = effect.kind == tools::EffectKind::Created || effect.kind == tools::EffectKind::Moved ||
                                effect.kind == tools::EffectKind::Renamed || effect.kind == tools::EffectKind::Overwritten;
            const auto it = writes && call.name != "doc.write" ? preview_of_.find(lowered(effect.to)) : preview_of_.end();
            if (it == preview_of_.end()) {
                continue;
            }
            if (auto md = document_markdown(effect.to)) {
                const auto name = leaf(core::utf8::to_utf8(effect.to).value_or(std::string{}));
                events::DataSet page{.id = it->second, .title = name, .source = call.name + " \xE2\x86\x92 the file as it is now", .complete = true,
                                     .kind = events::SetKind::Preview};
                page.preview = std::move(*md);
                out.emplace_back(events::Data{std::move(page), false});
            }
        }
        // image.ocr and image.convert: their panels (phase 10, screens 20-21).
        if (const auto& page = called.outcome.ocr) {
            out.emplace_back(events::Data{text_set(*page), false});
        }
        if (const auto& conversion = called.outcome.conversion) {
            out.emplace_back(events::Data{converted_set(*conversion), false});
        }
        // doc.read: a workbook's sheets, a CSV — each a table of its own in the panel (phase 9).
        for (const auto& table : called.outcome.tables) {
            const auto book = leaf(j ? text(*j, "path") : std::string{});
            auto sheet = rows_set(table.rows, table.name, "doc.read " + book);
            const auto ext = book.size() > 5 ? book.substr(book.size() - 5) : std::string{};
            if (ext == ".xlsx" || ext == ".xlsm") {
                sheet.book = book; // its sheets: one tab, a switch (phase 10)
            }
            out.emplace_back(events::Data{std::move(sheet), false});
        }
    }
    events::ActivityEntry entry{.number = ++calls_,
                                .title = step_title(call.name, call.arguments),
                                .purpose = {},
                                .command = command_of(call, j),
                                .output = {},
                                .exit_code = called.outcome.code,
                                .seconds = 0,
                                .internal = false,
                                .changed = ok && changes(call.name)};
    if (const auto began = began_of_.find(call.id); began != began_of_.end()) {
        entry.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began->second).count();
    }
    if (const auto run = parsed(called.outcome.json); ok && call.name == "shell.run" && run && run->contains("data")) {
        const auto& data = (*run)["data"];
        entry.exit_code = static_cast<int>(number(data, "code"));
        entry.output = text(data, "stdout") + text(data, "stderr");
        entry.seconds = number(data, "duration_ms") / 1000.0;
    }
    // The set this call made, for its entry's "Open table" (responsive text): the last one it gave.
    for (const auto& e : out) {
        if (const auto* d = std::get_if<events::Data>(&e); d != nullptr && !d->append) {
            entry.set = d->set.id;
        }
    }
    out.emplace_back(events::Activity{std::move(entry)});
    return out;
}

auto Task::progress(const ai::ToolCall& call, const tools::ProgressInfo& p) -> std::vector<events::AgentEvent> {
    std::vector<events::AgentEvent> out;
    if (p.total != 0) {
        out.emplace_back(events::Progress{static_cast<float>(p.done) / static_cast<float>(p.total)});
    }
    // system.check's steps on its own step: the bar, which step, the seconds left of the load while
    // idle (30 s of a bar standing at 7/8 looked like a hang, 2026-10-30).
    if (call.name == "system.check" && p.total != 0) {
        if (const auto it = step_of_.find(call.id); it != step_of_.end()) {
            out.emplace_back(events::Step{.id = it->second,
                                          .kind = kind_of(call.name),
                                          .state = events::StepState::Run,
                                          .title = step_title(call.name, call.arguments),
                                          .progress = static_cast<float>(p.done) / static_cast<float>(p.total),
                                          .progress_text = core::utf8::to_utf8(p.current).value_or(""),
                                          .eta = p.left ? std::format("{} s left", p.left->count()) : std::string{}});
        }
    }
    // disk.usage's tree so far: the panel grows while it counts, one set for the call.
    if (p.disk) {
        auto& id = disk_set_of_[call.id];
        if (id == 0) {
            id = ++g_last_set;
        }
        SYSTEMTIME now{};
        GetLocalTime(&now);
        out.emplace_back(events::Data{space::set_of(p.disk, {}, space::View::Folders, id,
                                                    std::format("{:02}:{:02}:{:02}", now.wHour, now.wMinute, now.wSecond)),
                                      false});
    }
    return out;
}

auto Task::output(const ai::ToolCall& call, std::string_view text) -> std::vector<events::AgentEvent> {
    const auto p = plan_of_.find(call.id);
    return {events::Output{.tab = 1,
                           .tab_label = std::string{kTerminal},
                           .text = std::string{text},
                           .command = false,
                           .step = p == plan_of_.end() ? -1 : p->second}};
}

namespace {

auto word_char(char c) -> bool {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// "C:\" or "C:/" at i, not inside a word ("http://" is not one).
auto drive_at(std::string_view t, std::size_t i) -> bool {
    return i + 2 < t.size() && ((t[i] >= 'a' && t[i] <= 'z') || (t[i] >= 'A' && t[i] <= 'Z')) && t[i + 1] == ':' &&
           (t[i + 2] == '\\' || t[i + 2] == '/') && (i == 0 || !word_char(t[i - 1]));
}

// Where a path's text surely ends: the line's end, a character no path has, the next path.
auto path_end(std::string_view t, std::size_t i) -> std::size_t {
    constexpr std::size_t kLongest = 1024;
    std::size_t k = i + 3;
    for (; k < t.size() && k - i < kLongest; ++k) {
        const char c = t[k];
        if (c == '\n' || c == '\r' || c == '\t' || c == '`' || c == '<' || c == '>' || c == '"' || c == '|' || c == '*' || c == '?' ||
            c == ':' || drive_at(t, k)) {
            break;
        }
    }
    return k;
}

// A place a reading may stop before, after the path's last backslash: a space, a mark, the start
// of « » “ ” — ….
auto cut_at(char c) -> bool {
    return c == ' ' || c == ',' || c == ';' || c == ')' || c == ']' || c == '\'' || c == '.' || c == '!' || c == '\xC2' || c == '\xE2';
}

// The path's own spelling: slashes as backslashes, a doubled backslash single.
auto spelled(std::string_view raw) -> std::string {
    std::string out;
    for (char c : raw) {
        c = c == '/' ? '\\' : c;
        if (c == '\\' && !out.empty() && out.back() == '\\') {
            continue;
        }
        out += c;
    }
    return out;
}

// What Windows reads of a name anyway: no spaces or dots at its end (nor a folder's backslash).
auto trimmed(std::string s) -> std::string {
    while (s.size() > 3 && (s.back() == ' ' || s.back() == '.' || s.back() == '\\')) {
        s.pop_back();
    }
    return s;
}

auto same_path(std::string_view a, std::string_view b) -> bool {
    return std::ranges::equal(a, b, [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
}

} // namespace

auto places_in(std::string_view text, const Probe& probe) -> std::vector<events::Place> {
    constexpr std::size_t kCuts = 8; // a name with more spaces than this is not looked for
    std::vector<events::Place> out;
    // The disk is asked kMaxProbes times at most an answer, each reading once (final review).
    std::vector<std::pair<std::string, std::optional<bool>>> asked;
    const auto look = [&](const std::string& path) -> std::optional<bool> {
        if (const auto it = std::ranges::find_if(asked, [&](const auto& a) { return same_path(a.first, path); }); it != asked.end()) {
            return it->second;
        }
        if (asked.size() >= kMaxProbes) {
            return std::nullopt;
        }
        asked.emplace_back(path, probe(path));
        return asked.back().second;
    };
    std::size_t i = 0;
    while (i < text.size() && out.size() < kMaxPlaces) {
        if (!drive_at(text, i)) {
            ++i;
            continue;
        }
        const std::size_t end = path_end(text, i);
        const std::string_view raw = text.substr(i, end - i);
        const auto last = raw.find_last_of("\\/");
        // The readings, longest first: to the end, then cut at each of the first kCuts marks after
        // the last backslash.
        std::vector<std::size_t> ends;
        for (std::size_t k = last + 1; k < raw.size() && ends.size() < kCuts; ++k) {
            if (cut_at(raw[k])) {
                ends.push_back(k);
            }
        }
        ends.push_back(raw.size());
        std::ranges::sort(ends, std::greater{});
        std::optional<std::pair<std::string, bool>> found;
        std::size_t used = 3;
        for (const auto k : ends) {
            const auto path = trimmed(spelled(raw.substr(0, k)));
            if (path.size() <= 3) {
                continue; // a drive alone ("free on C:\") is not a place
            }
            if (const auto folder = look(path)) {
                found.emplace(path, *folder);
                used = k;
                break;
            }
        }
        if (found && std::ranges::none_of(out, [&](const events::Place& p) { return same_path(p.path, found->first); })) {
            const auto at = found->first.find_last_of('\\');
            out.push_back({.path = found->first,
                           .name = found->first.substr(at + 1),
                           .where = home_short(found->first.substr(0, at)),
                           .folder = found->second});
        }
        i += std::max<std::size_t>(used, 3);
    }
    return out;
}

auto on_disk(const std::string& path) -> std::optional<bool> {
    const auto w = core::utf8::to_wide(path);
    if (!w || w->size() < 4) {
        return std::nullopt;
    }
    // A network drive is not looked at: it may hang, and it is not this PC's; nor an optical drive,
    // which may spin up for seconds (on the agent's thread, before the answer's buttons).
    const std::wstring root = w->substr(0, 3);
    if (const UINT type = GetDriveTypeW(root.c_str());
        type == DRIVE_REMOTE || type == DRIVE_CDROM || type == DRIVE_NO_ROOT_DIR || type == DRIVE_UNKNOWN) {
        return std::nullopt;
    }
    const DWORD attrs = GetFileAttributesW(w->c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return std::nullopt;
    }
    // Where it really is: not behind a link to a share, not in the secrets zone.
    const auto c = security::paths::canonical(*w, std::filesystem::path{root});
    if (!c || c->unc || c->remote || security::paths::zone_of(*c, security::paths::system_roots()) == security::paths::Zone::Secrets) {
        return std::nullopt;
    }
    return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

auto Task::answered(std::string_view said, const ai::Usage& usage, double usd, std::string_view whole) -> std::vector<events::AgentEvent> {
    std::vector<events::AgentEvent> out;
    // A table of the answer too big for the text goes to the panel, under the heading before it —
    // from every turn's words, as the screen shows them: its card there opens this set (a table said
    // before a last turn had a card and no set, a click on it did nothing).
    const std::string_view shown = whole.empty() ? said : whole;
    markdown::Document doc;
    doc.sync(shown, true);
    std::string heading;
    for (const auto& b : doc.blocks()) {
        if (b.kind == markdown::Kind::Heading) {
            heading = b.text;
        } else if (b.kind == markdown::Kind::Table && markdown::to_panel(b)) {
            out.emplace_back(events::Data{table_set(b, heading), false});
        }
    }
    if (!changed_ && !reported_ && !system_info_.empty() && about_space(request_)) {
        if (const auto machine = report::machine_from_json(system_info_); machine && machine->disk_total > 0) {
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began_).count();
            out.emplace_back(report::disk_quick(*machine, seconds));
        }
    }
    // The files and folders it names, each a row with its buttons: the user cannot copy the text.
    if (auto places = places_in(shown, on_disk); !places.empty()) {
        out.emplace_back(events::Places{std::move(places)});
    }
    out.emplace_back(events::Actions{});
    out.emplace_back(events::Spent{.usd = usd, .prompt = usage.prompt, .completion = usage.completion, .cache_hit = usage.cache_hit});
    return out;
}

void Task::carry(const Task& before) {
    if (system_info_.empty()) {
        system_info_ = before.system_info_;
    }
    preview_of_.insert(before.preview_of_.begin(), before.preview_of_.end()); // the conversation's documents
}

auto Task::deepened(const tools::winsys::CheckReport& r, std::uint32_t id) -> std::optional<events::DataSet> {
    if (threat_ == nullptr || id != threat_set_) {
        return std::nullopt;
    }
    threat_ = std::make_shared<const events::ThreatView>(threat_view(r));
    return threat_set(threat_, r.stages.size(), id);
}

auto Task::startup_again(const tools::Called& called, std::uint32_t id) -> std::optional<events::DataSet> {
    if (id == 0 || id != startup_set_ || called.outcome.code != 0) {
        return std::nullopt;
    }
    return startup_set(called.outcome.json, id);
}

auto Task::inside(std::string title, std::string output, bool failed) -> events::Activity {
    return events::Activity{events::ActivityEntry{.number = ++calls_, .title = std::move(title), .purpose = {}, .command = {},
                                                  .output = std::move(output), .exit_code = failed ? 1 : 0, .seconds = 0,
                                                  .internal = true, .changed = false}};
}

auto Task::finding(std::uint32_t row, std::uint32_t action) const -> std::optional<std::string> {
    if (!threat_ || row >= threat_->rows.size() || action >= threat_->rows[row].actions.size()) {
        return std::nullopt;
    }
    std::string what = threat_->rows[row].actions[action];
    if (what.ends_with("\xE2\x80\xA6")) {
        what.resize(what.size() - 3); // "Quarantine…" -> "Quarantine"
    }
    return std::format("{}: {}.", what, threat_->rows[row].title);
}

auto Task::app(std::uint32_t row) const -> std::optional<std::string> {
    return row < uninstallable_.size() ? std::optional<std::string>{uninstallable_[row]} : std::nullopt;
}

auto Task::program(std::uint32_t pid) const -> std::optional<std::string> {
    const auto it = std::ranges::find(closable_, pid, &std::pair<std::uint32_t, std::string>::first);
    return it == closable_.end() ? std::nullopt : std::optional<std::string>{it->second};
}

} // namespace app::present
