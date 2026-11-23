#include "app/journal.hpp"

#include "app/history.hpp"
#include "core/json.hpp"
#include "core/utf8.hpp"

#include <glaze/glaze.hpp>

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <format>
#include <fstream>
#include <map>
#include <utility>

namespace app::journal {

namespace detail {

// The line as JSON: the kind by name.
struct ChangeLine {
    std::string kind;
    std::string from;
    std::string to;
    bool undoable = false;
    bool undone = false;
    std::optional<std::string> backup; // Edited: the copy of what the file held
    std::optional<std::string> after;  // Edited: the file's hash right after the edit
};

struct SpotLine {
    std::string path;
    int x = 0;
    int y = 0;
};

struct EntryLine {
    std::uint64_t task = 0;
    std::string title;
    std::string started;
    std::vector<ChangeLine> effects;
    std::vector<SpotLine> spots; // absent in the lines of phase 5's first days
};

} // namespace detail

namespace {

constexpr std::array<std::pair<tools::EffectKind, std::string_view>, 17> kKinds{{
    {tools::EffectKind::Created, "created"},
    {tools::EffectKind::Moved, "moved"},
    {tools::EffectKind::Renamed, "renamed"},
    {tools::EffectKind::Trashed, "trashed"},
    {tools::EffectKind::Restored, "restored"},
    {tools::EffectKind::Deleted, "deleted"},
    {tools::EffectKind::Overwritten, "overwritten"},
    {tools::EffectKind::Killed, "killed"},
    {tools::EffectKind::Ran, "ran"},
    {tools::EffectKind::Edited, "edited"},
    {tools::EffectKind::StartupOff, "startup-off"},
    {tools::EffectKind::TaskOff, "task-off"},
    {tools::EffectKind::WmiTaken, "wmi-taken"},
    {tools::EffectKind::SwitchedOn, "switched-on"},
    {tools::EffectKind::Uninstalled, "uninstalled"},
    {tools::EffectKind::Quarantined, "quarantined"},
    {tools::EffectKind::Released, "released"},
}};

auto kind_of(std::string_view name) -> std::optional<tools::EffectKind> {
    for (const auto& [k, n] : kKinds) {
        if (n == name) {
            return k;
        }
    }
    return std::nullopt;
}

auto utf8(const std::wstring& w) -> std::string {
    return core::utf8::to_utf8(w).value_or(std::string{});
}

auto paths_call(std::string_view tool, const std::string& path) -> ToolCall {
    return {std::string{tool}, R"({"paths":[)" + history::json_string(path) + "]}"};
}

// `path` is `folder` or inside it.
auto inside(std::string_view path, std::string_view folder) -> bool {
    if (folder.empty() || path.size() <= folder.size() || !path.starts_with(folder)) {
        return false;
    }
    return path[folder.size()] == '\\' || path[folder.size()] == '/';
}

} // namespace

auto kind_name(tools::EffectKind k) -> std::string_view {
    for (const auto& [kind, name] : kKinds) {
        if (kind == k) {
            return name;
        }
    }
    return "created";
}

auto change_of(const tools::Effect& e) -> Change {
    return {.kind = e.kind, .from = utf8(e.from), .to = utf8(e.to), .undoable = e.undoable};
}

auto inverse(const Change& c) -> std::optional<ToolCall> {
    switch (c.kind) {
    case tools::EffectKind::Created:
    case tools::EffectKind::Restored:
        return paths_call("fs.trash", c.to);
    case tools::EffectKind::Moved:
    case tools::EffectKind::Renamed:
        return ToolCall{"fs.move", R"({"moves":[{"from":)" + history::json_string(c.to) + R"(,"to":)" +
                                       history::json_string(c.from) + "}]}"};
    case tools::EffectKind::Trashed:
        return paths_call("fs.restore", c.to);
    case tools::EffectKind::Edited:
        if (c.backup.empty()) {
            return std::nullopt; // its copy was not kept
        }
        return ToolCall{"fs.revert", R"({"path":)" + history::json_string(c.to) + R"(,"backup":)" + history::json_string(c.backup) +
                                         R"(,"after":)" + history::json_string(c.after) + "}"};
    // A program off at startup (phase 13a): back on by the agent's own tools.
    case tools::EffectKind::StartupOff:
        return ToolCall{"startup.enable", R"({"entry":)" + history::json_string(c.to) + "}"};
    case tools::EffectKind::TaskOff:
        return ToolCall{"task.enable", R"({"path":)" + history::json_string(c.to) + "}"};
    case tools::EffectKind::WmiTaken:
        return ToolCall{"wmi.restore", R"({"mof":)" + history::json_string(c.to) + R"(,"filter":)" + history::json_string(c.from) + "}"};
    case tools::EffectKind::Quarantined: // back from the agent's quarantine (phase 13a § 5.4)
        return ToolCall{"file.release", R"({"dir":)" + history::json_string(c.to) + "}"};
    case tools::EffectKind::Deleted:
    case tools::EffectKind::Overwritten:
    case tools::EffectKind::Killed:
    case tools::EffectKind::Ran:
    case tools::EffectKind::SwitchedOn:
    case tools::EffectKind::Uninstalled:
    case tools::EffectKind::Released:
        break;
    }
    return std::nullopt;
}

auto inverse(const tools::Effect& e) -> std::optional<ToolCall> {
    return inverse(change_of(e));
}

auto undo_order(const Entry& e, std::optional<std::size_t> which) -> std::vector<std::size_t> {
    const auto can = [&](std::size_t i) { return e.changes[i].undoable && !e.changes[i].undone && inverse(e.changes[i]); };
    std::vector<std::size_t> out;
    if (!which) {
        for (std::size_t i = e.changes.size(); i-- > 0;) {
            if (can(i)) {
                out.push_back(i);
            }
        }
        return out;
    }
    if (*which >= e.changes.size() || !can(*which)) {
        return out;
    }
    const auto& c = e.changes[*which];
    if (c.kind == tools::EffectKind::Created) {
        for (std::size_t i = e.changes.size(); i-- > *which + 1;) {
            if (can(i) && inside(e.changes[i].to, c.to)) {
                out.push_back(i);
            }
        }
    }
    out.push_back(*which);
    return out;
}

auto to_json(const Entry& e) -> std::string {
    detail::EntryLine line{.task = e.task, .title = e.title, .started = e.started, .effects = {}};
    line.effects.reserve(e.changes.size());
    for (const auto& c : e.changes) {
        detail::ChangeLine l{std::string{kind_name(c.kind)}, c.from, c.to, c.undoable, c.undone, {}, {}};
        if (!c.backup.empty()) {
            l.backup = c.backup;
            l.after = c.after;
        }
        line.effects.push_back(std::move(l));
    }
    for (const auto& s : e.spots) {
        line.spots.push_back({s.path, s.x, s.y});
    }
    std::string out;
    if (core::json::write(line, out)) {
        return {};
    }
    return out;
}

auto parse(std::string_view text) -> std::optional<Entry> {
    detail::EntryLine line;
    if (glz::read<glz::opts{.error_on_unknown_keys = false}>(line, text)) {
        return std::nullopt;
    }
    Entry e{.task = line.task, .title = std::move(line.title), .started = std::move(line.started), .changes = {}};
    for (auto& c : line.effects) {
        const auto kind = kind_of(c.kind);
        if (!kind) {
            return std::nullopt;
        }
        e.changes.push_back({*kind, std::move(c.from), std::move(c.to), c.undoable, c.undone, c.backup.value_or(std::string{}),
                             c.after.value_or(std::string{})});
    }
    for (auto& s : line.spots) {
        e.spots.push_back({std::move(s.path), s.x, s.y});
    }
    return e;
}

auto append(const std::filesystem::path& file, const Entry& e) -> core::Result<void> {
    const auto line = to_json(e);
    if (line.empty()) {
        return core::make_error(core::Code::Internal, "the journal line could not be made");
    }
    // A line cut short (the process died while writing it) ends here, not inside this one.
    bool cut = false;
    if (std::ifstream in{file, std::ios::binary | std::ios::ate}; in && in.tellg() > 0) {
        in.seekg(-1, std::ios::end);
        cut = in.get() != '\n';
    }
    std::ofstream out(file, std::ios::binary | std::ios::app);
    if (cut) {
        out << '\n';
    }
    out << line << '\n';
    out.flush();
    if (!out) {
        return core::make_error(core::Code::Io, "the journal could not be written");
    }
    return {};
}

namespace {

// "2026-10-12T09:47:56Z" -> that moment.
auto moment_of(std::string_view utc) -> std::optional<SYSTEMTIME> {
    SYSTEMTIME t{};
    unsigned y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    const std::string text{utc};
    if (std::sscanf(text.c_str(), "%4u-%2u-%2uT%2u:%2u:%2uZ", &y, &mo, &d, &h, &mi, &s) != 6) {
        return std::nullopt;
    }
    t.wYear = static_cast<WORD>(y);
    t.wMonth = static_cast<WORD>(mo);
    t.wDay = static_cast<WORD>(d);
    t.wHour = static_cast<WORD>(h);
    t.wMinute = static_cast<WORD>(mi);
    t.wSecond = static_cast<WORD>(s);
    return t;
}

auto local_of(const SYSTEMTIME& utc) -> SYSTEMTIME {
    SYSTEMTIME local{};
    if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) {
        return utc;
    }
    return local;
}

auto day_number(const SYSTEMTIME& t) -> int {
    using namespace std::chrono;
    return static_cast<int>(
        sys_days{year{t.wYear} / month{t.wMonth} / day{t.wDay}}.time_since_epoch().count());
}

auto utc_of(std::chrono::system_clock::time_point now) -> SYSTEMTIME {
    using namespace std::chrono;
    const auto days = floor<std::chrono::days>(now);
    const year_month_day ymd{days};
    const hh_mm_ss hms{floor<seconds>(now - days)};
    SYSTEMTIME t{};
    t.wYear = static_cast<WORD>(static_cast<int>(ymd.year()));
    t.wMonth = static_cast<WORD>(static_cast<unsigned>(ymd.month()));
    t.wDay = static_cast<WORD>(static_cast<unsigned>(ymd.day()));
    t.wHour = static_cast<WORD>(hms.hours().count());
    t.wMinute = static_cast<WORD>(hms.minutes().count());
    t.wSecond = static_cast<WORD>(hms.seconds().count());
    return t;
}

auto parent_of(std::string_view path) -> std::string_view {
    const auto cut = path.find_last_of("\\/");
    return cut == std::string_view::npos ? std::string_view{} : path.substr(0, cut);
}

auto name_of(std::string_view path) -> std::string_view {
    const auto cut = path.find_last_of("\\/");
    return cut == std::string_view::npos ? path : path.substr(cut + 1);
}

// The longest folder both are in (whole names only).
auto common_folder(std::string_view a, std::string_view b) -> std::string_view {
    while (!a.empty() && !(b.size() >= a.size() && b.starts_with(a) && (b.size() == a.size() || b[a.size()] == '\\' || b[a.size()] == '/'))) {
        a = parent_of(a);
    }
    return a;
}

auto components_after(std::string_view path, std::string_view root) -> int {
    int n = 0;
    for (std::size_t i = root.size(); i < path.size(); ++i) {
        n += path[i] == '\\' || path[i] == '/' ? 1 : 0;
    }
    return n;
}

auto less_folded(std::string_view a, std::string_view b) -> bool {
    const auto fold = [](char c) {
        c = c == '/' ? '\\' : c;
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    };
    return std::ranges::lexicographical_compare(a, b, [&](char x, char y) {
        // A separator sorts first: a folder comes right before what is in it.
        const char fx = fold(x), fy = fold(y);
        if (fx == '\\' || fy == '\\') {
            return fx == '\\' && fy != '\\';
        }
        return static_cast<unsigned char>(fx) < static_cast<unsigned char>(fy);
    });
}

auto quoted(std::string_view s) -> std::string {
    return "\xE2\x80\x9C" + std::string{s} + "\xE2\x80\x9D";
}

auto task_of(const Entry& e, int today) -> events::LogTask {
    events::LogTask t{.task = e.task, .title = e.title, .when = {}, .days_ago = 0, .summary = {}, .kind = events::Kind::Read,
                      .changes = e.changes.size(), .undoable = false, .rows = {}};
    if (const auto at = moment_of(e.started)) {
        const auto local = local_of(*at);
        t.when = std::format("{:02}:{:02}", local.wHour, local.wMinute);
        t.days_ago = std::max(0, today - day_number(local));
    }
    std::size_t can = 0, undone = 0, trashed = 0;
    bool system = false, deleted = false;
    for (const auto& c : e.changes) {
        const bool takes = c.undoable && inverse(c).has_value();
        can += takes && !c.undone ? 1 : 0;
        undone += takes && c.undone ? 1 : 0;
        trashed += c.kind == tools::EffectKind::Trashed ? 1 : 0;
        system = system || c.kind == tools::EffectKind::Killed || c.kind == tools::EffectKind::Ran || c.kind == tools::EffectKind::StartupOff ||
                 c.kind == tools::EffectKind::TaskOff || c.kind == tools::EffectKind::WmiTaken || c.kind == tools::EffectKind::Uninstalled ||
                 c.kind == tools::EffectKind::Quarantined;
        deleted = deleted || c.kind == tools::EffectKind::Trashed || c.kind == tools::EffectKind::Deleted;
    }
    t.undoable = can > 0;
    if (e.changes.empty()) {
        t.summary = "no changes";
        return t;
    }
    t.kind = system ? events::Kind::System : deleted ? events::Kind::Delete : events::Kind::Change;
    if (can == 0 && undone > 0) {
        t.summary = "undone";
    } else if (can == 0) {
        t.summary = "cannot be undone";
    } else if (trashed == e.changes.size()) {
        t.summary = std::format("{} in the Recycle Bin", trashed);
    } else {
        t.summary = std::format("{} {}", e.changes.size(), e.changes.size() == 1 ? "change" : "changes");
    }

    // The tree: the folder every path is in, then each change under its folders.
    // A process, a command, a startup entry: named, not a path in the tree.
    const auto path_of = [](const Change& c) -> std::string_view {
        switch (c.kind) {
        case tools::EffectKind::Killed:
        case tools::EffectKind::Ran:
        case tools::EffectKind::StartupOff:
        case tools::EffectKind::TaskOff:
        case tools::EffectKind::WmiTaken:
        case tools::EffectKind::SwitchedOn:
        case tools::EffectKind::Uninstalled:
        case tools::EffectKind::Quarantined:
        case tools::EffectKind::Released: return {};
        default: return c.to;
        }
    };
    std::optional<std::string_view> root;
    for (const auto& c : e.changes) {
        if (path_of(c).empty()) {
            continue;
        }
        for (const auto p : {parent_of(c.to), (c.kind == tools::EffectKind::Moved ? parent_of(c.from) : parent_of(c.to))}) {
            root = root ? common_folder(*root, p) : p;
        }
    }
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < e.changes.size(); ++i) {
        order.push_back(i);
    }
    std::ranges::stable_sort(order, [&](std::size_t a, std::size_t b) { return less_folded(path_of(e.changes[a]), path_of(e.changes[b])); });
    const auto is_folder = [&](const Change& c) {
        return std::ranges::any_of(e.changes, [&](const Change& o) { return inside(o.to, c.to); });
    };
    std::vector<std::string_view> shown; // folders with a row
    if (root) {
        t.rows.push_back({.op = events::LogOp::Context, .name = std::string{name_of(*root)}, .note = {}, .depth = 0, .folder = true,
                          .change = std::nullopt, .undone = false});
        shown.push_back(*root);
    }
    for (const auto i : order) {
        const auto& c = e.changes[i];
        const auto path = path_of(c);
        events::LogRow row{.op = events::LogOp::Context, .name = std::string{name_of(path)}, .note = {}, .depth = 0,
                           .folder = false, .change = std::nullopt, .undone = c.undone};
        if (!path.empty() && root) {
            // The folders between the root and this one that have no row yet.
            std::vector<std::string_view> missing;
            for (auto p = parent_of(path); p.size() > root->size(); p = parent_of(p)) {
                if (std::ranges::find(shown, p) == shown.end()) {
                    missing.push_back(p);
                }
            }
            for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
                t.rows.push_back({.op = events::LogOp::Context, .name = std::string{name_of(*it)}, .note = {},
                                  .depth = components_after(*it, *root), .folder = true, .change = std::nullopt, .undone = false});
                shown.push_back(*it);
            }
            row.depth = components_after(path, *root);
        }
        switch (c.kind) {
        case tools::EffectKind::Created:
            row.op = events::LogOp::Created;
            row.folder = is_folder(c);
            row.note = row.folder ? "folder" : "";
            break;
        case tools::EffectKind::Restored:
            row.op = events::LogOp::Created;
            row.note = "restored";
            break;
        case tools::EffectKind::Moved:
            row.op = events::LogOp::Moved;
            row.folder = is_folder(c);
            if (root && parent_of(c.from) != *root && parent_of(c.from) != parent_of(c.to)) {
                row.note = "from " + quoted(name_of(parent_of(c.from)));
            }
            break;
        case tools::EffectKind::Renamed:
            row.op = events::LogOp::Renamed;
            row.note = "\xE2\x86\x90 " + quoted(name_of(c.from));
            break;
        case tools::EffectKind::Trashed:
            row.op = events::LogOp::Deleted;
            row.note = "in the Recycle Bin";
            break;
        case tools::EffectKind::Deleted:
            row.op = events::LogOp::Deleted;
            row.note = "deleted for good";
            break;
        case tools::EffectKind::Overwritten:
            row.op = events::LogOp::Renamed;
            row.note = "overwritten";
            break;
        case tools::EffectKind::Killed:
            row.op = events::LogOp::Deleted;
            row.name = c.from;
            row.note = "closed";
            break;
        case tools::EffectKind::Ran:
            row.name = c.from;
            row.note = "ran";
            break;
        case tools::EffectKind::StartupOff:
            row.name = c.from;
            row.note = "off at startup";
            break;
        case tools::EffectKind::TaskOff:
            row.name = c.from;
            row.note = "scheduled task off";
            break;
        case tools::EffectKind::WmiTaken:
            row.name = c.from;
            row.note = "WMI subscription taken off";
            break;
        case tools::EffectKind::SwitchedOn:
            row.name = c.from;
            row.note = "back on";
            break;
        case tools::EffectKind::Uninstalled:
            row.op = events::LogOp::Deleted;
            row.name = c.from;
            row.note = "uninstalled";
            break;
        case tools::EffectKind::Quarantined:
            row.op = events::LogOp::Moved;
            row.name = std::string{name_of(c.from)};
            row.note = "in quarantine";
            break;
        case tools::EffectKind::Released:
            row.name = std::string{name_of(c.from)};
            row.note = "back from quarantine";
            break;
        }
        if (row.folder) {
            shown.push_back(path);
        }
        if (c.undoable && !c.undone && inverse(c)) {
            row.change = i;
        }
        t.rows.push_back(std::move(row));
    }
    return t;
}

} // namespace

auto log_of(const std::vector<Entry>& entries, std::chrono::system_clock::time_point now) -> events::Log {
    const int today = day_number(local_of(utc_of(now)));
    events::Log log;
    log.tasks.reserve(entries.size());
    for (const auto& e : entries) {
        log.tasks.push_back(task_of(e, today));
    }
    // Newest first: by when each began, then by id.
    std::vector<std::size_t> order(entries.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::ranges::sort(order, [&](std::size_t a, std::size_t b) {
        return entries[a].started != entries[b].started ? entries[a].started > entries[b].started : entries[a].task > entries[b].task;
    });
    events::Log sorted;
    sorted.tasks.reserve(order.size());
    for (const auto i : order) {
        sorted.tasks.push_back(std::move(log.tasks[i]));
    }
    return sorted;
}

auto read(const std::filesystem::path& file) -> std::vector<Entry> {
    std::ifstream in(file, std::ios::binary);
    std::vector<Entry> out;
    std::map<std::uint64_t, std::size_t> at; // task -> its place in `out`
    for (std::string l; std::getline(in, l);) {
        if (!l.empty() && l.back() == '\r') {
            l.pop_back();
        }
        auto e = parse(l);
        if (!e) {
            continue;
        }
        if (const auto it = at.find(e->task); it != at.end()) {
            out[it->second] = std::move(*e);
        } else {
            at.emplace(e->task, out.size());
            out.push_back(std::move(*e));
        }
    }
    return out;
}

} // namespace app::journal
