#include "app/agent_tools.hpp"

#include "app/desktop_icons.hpp"
#include "app/history.hpp"
#include "app/present.hpp"
#include "app/sizes.hpp"
#include "core/log.hpp"
#include "core/utf8.hpp"
#include "core/win.hpp"
#include "tools/fs_edit.hpp"
#include "tools/os.hpp"
#include "tools/winsys/reader_link.hpp"

#include <glaze/glaze.hpp>

#include <windows.h>

#include <knownfolders.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <format>
#include <fstream>
#include <iterator>

namespace app::agent {
namespace {

auto utf8(std::wstring_view w) -> std::string {
    auto s = core::utf8::to_utf8(w);
    return s ? std::move(*s) : std::string{"?"};
}

} // namespace

auto user_host(const std::filesystem::path& data_dir, security::Settings settings, monitor::Monitor* monitor) -> Host {
    return Host{.desktop = core::win::known_folder(FOLDERID_Desktop).value_or(data_dir),
                .audit = data_dir / L"audit.jsonl",
                .settings = settings,
                .sort_desktop = true,
                .journal = data_dir / L"changes.jsonl",
                .monitor = monitor,
                .temp = std::filesystem::temp_directory_path(),
                .downloads = core::win::known_folder(FOLDERID_Downloads).value_or(std::filesystem::path{}),
                .documents = core::win::known_folder(FOLDERID_Documents).value_or(std::filesystem::path{}),
                .icon_spots = desktop_icons::icon_spots,
                .place_icons = desktop_icons::place_icons,
                .check_sources = nullptr,
                // agent-reader.exe beside agent.exe, through Windows' UAC prompt (phase 13a § 6).
                .read_deeper = [] {
                    return tools::winsys::reader::run(core::win::module_path().parent_path() / L"agent-reader.exe",
                                                      &tools::winsys::reader::launch_elevated);
                }};
}

auto settings_of(const config::Advanced& advanced, const config::Limits& limits, const config::Access& access,
                 bool elevated) -> security::Settings {
    return {.allow_system_writes = advanced.allow_system_writes,
            .allow_secrets_read = advanced.allow_secrets_read,
            .allow_permanent_delete = advanced.allow_permanent_delete,
            .max_files = limits.max_files,
            .max_bytes = static_cast<std::uint64_t>(limits.max_megabytes) << 20,
            .max_trash_batch = limits.max_trash_batch,
            .max_tree = limits.max_tree,
            .max_calls = limits.max_calls,
            .agent_elevated = elevated,
            .read_asks = access.read == config::ReadLevel::Ask,
            .change_per_folder = access.change == config::ChangeLevel::Folder};
}

auto process_elevated() -> bool {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_ELEVATION e{};
    DWORD got = 0;
    const bool elevated = GetTokenInformation(token, TokenElevation, &e, sizeof(e), &got) && e.TokenIsElevated != 0;
    CloseHandle(token);
    return elevated;
}

namespace {

// The record's object, from its arguments: a path, the first of the paths, the command, the pid.
auto target_of(std::string_view args) -> std::string {
    glz::generic a;
    if (glz::read_json(a, args) || !a.is_object()) {
        return {};
    }
    if (a.contains("path") && a["path"].is_string()) {
        return a["path"].get_string();
    }
    for (const char* list : {"paths", "moves", "copies"}) {
        if (a.contains(list) && a[list].is_array() && !a[list].get_array().empty()) {
            const auto& all = a[list].get_array();
            const auto& first = all[0];
            std::string one = first.is_string() ? first.get_string()
                              : first.is_object() && first.contains("from") ? first["from"].get_string()
                                                                             : std::string{};
            return all.size() > 1 ? std::format("{} and {} more", one, all.size() - 1) : one;
        }
    }
    if (a.contains("command") && a["command"].is_string()) {
        return a["command"].get_string();
    }
    if (a.contains("target") && a["target"].is_string()) {
        return a["target"].get_string();
    }
    if (a.contains("pid") && a["pid"].is_number()) {
        return std::format("process {}", static_cast<long long>(a["pid"].get_number()));
    }
    return {};
}

} // namespace

namespace {

// Who decided a record and how it ended, in a few words.
auto decided(const security::AuditRecord& r) -> std::string {
    std::string how;
    if (r.decision == "auto") {
        how = "allowed by the policy";
    } else if (r.decision == "ask") {
        how = r.result_code == 3 ? "refused by you" : "allowed by you";
    } else {
        how = "refused (" + r.decision + ")";
    }
    if (r.result_code == 2) {
        how += ", failed";
    }
    return how;
}

// "2026-10-11T12:00:09.000Z" as seconds, for sorting by time.
auto seconds_of(std::string_view ts) -> double {
    const auto at = [&](std::size_t from, std::size_t len) {
        int v = 0;
        if (from + len <= ts.size()) {
            (void)std::from_chars(ts.data() + from, ts.data() + from + len, v);
        }
        return v;
    };
    const std::chrono::sys_days day = std::chrono::year_month_day{std::chrono::year{at(0, 4)},
                                                                  std::chrono::month{static_cast<unsigned>(at(5, 2))},
                                                                  std::chrono::day{static_cast<unsigned>(at(8, 2))}};
    return static_cast<double>(day.time_since_epoch().count()) * 86400.0 + at(11, 2) * 3600.0 + at(14, 2) * 60.0 + at(17, 2);
}

} // namespace

auto audit_set(const std::vector<security::AuditRecord>& records) -> events::DataSet {
    events::DataSet s{.title = "Audit log", .source = "audit.jsonl", .complete = true, .kind = events::SetKind::Table};
    s.columns = {{"When", ui::work::ColumnKind::Date, 150, false, true},
                 {"Tool", ui::work::ColumnKind::Mono, 110, false, true},
                 {"What", ui::work::ColumnKind::Text, 0, true, true},
                 {"Decided", ui::work::ColumnKind::Text, 170, false, true}};
    s.rows.columns = s.columns.size();
    for (auto it = records.rbegin(); it != records.rend(); ++it) { // the newest first
        std::string when = it->ts.substr(0, std::min<std::size_t>(it->ts.size(), 19));
        std::ranges::replace(when, 'T', ' ');
        s.rows.cells.push_back({std::move(when), seconds_of(it->ts)});
        s.rows.cells.push_back({it->tool, 0});
        s.rows.cells.push_back({target_of(it->args), 0});
        s.rows.cells.push_back({decided(*it), 0});
    }
    s.footer = std::format("{} {}", records.size(), records.size() == 1 ? "record" : "records");
    return s;
}

auto audit_text(const std::vector<security::AuditRecord>& records, const core::Result<std::size_t>& verified) -> std::string {
    if (records.empty() && verified && *verified == 0) {
        return "No tool has run yet. Every tool call will be listed here, with who allowed it.";
    }
    std::string out = verified ? std::format("**{} records** \xE2\x80\x94 the chain holds: nothing was changed or removed\n", *verified)
                               : "**The audit log was changed or damaged** \xE2\x80\x94 " + verified.error().message + "\n";
    for (auto it = records.rbegin(); it != records.rend(); ++it) {
        const auto& r = *it;
        std::string when = r.ts.substr(0, std::min<std::size_t>(r.ts.size(), 19));
        std::ranges::replace(when, 'T', ' ');
        const std::string how = decided(r);
        const std::string target = target_of(r.args);
        out += std::format("- **{}** {}{}{} \xE2\x80\x94 {}\n", r.tool, target, target.empty() ? "" : " ", "(" + when + ")", how);
    }
    return out;
}

namespace {

auto kind_of(security::Risk r) -> events::Kind {
    switch (r) {
    case security::Risk::Read: return events::Kind::Read;
    case security::Risk::Write:
    case security::Risk::RenameMove: return events::Kind::Change;
    case security::Risk::Delete: return events::Kind::Delete;
    case security::Risk::Execute:
    case security::Risk::ProcessKill:
    case security::Risk::Network:
    case security::Risk::Clipboard:
    case security::Risk::Screen:
    case security::Risk::Look:
    case security::Risk::Startup:
    case security::Risk::Uninstall:
    case security::Risk::Quarantine: break;
    }
    return events::Kind::System;
}

// One row of the card: the object's name, its extension, its size (a folder says so).
auto item_of(const security::paths::Canonical& c) -> events::FileItem {
    const std::filesystem::path p{c.path};
    events::FileItem item{.name = utf8(p.filename().wstring())};
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (GetFileAttributesExW(security::paths::long_form(c.path).c_str(), GetFileExInfoStandard, &fa)) {
        if ((fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            item.meta = "folder";
            item.ext = "DIR";
            return item;
        }
        item.bytes = (static_cast<std::uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
        item.meta = sizes::text(item.bytes);
    }
    std::string ext = utf8(p.extension().wstring());
    if (!ext.empty()) {
        ext.erase(0, 1);
    }
    std::ranges::transform(ext, ext.begin(), [](char ch) { return static_cast<char>(std::toupper(static_cast<unsigned char>(ch))); });
    item.ext = ext.size() <= 4 ? ext : ext.substr(0, 4);
    return item;
}

// A task's edit copies (<undo>\<task>\) none of which changed for 30 days: deleted when the agent
// starts, so the data folder does not grow without end (final review E3). Their Undo then says the
// copy is gone.
void forget_old_copies(const std::filesystem::path& undo) {
    constexpr auto kKept = std::chrono::hours{24 * 30};
    const auto line = std::filesystem::file_time_type::clock::now() - kKept;
    const std::filesystem::directory_iterator end;
    std::error_code ec;
    for (std::filesystem::directory_iterator task{undo, ec}; !ec && task != end; task.increment(ec)) {
        std::error_code e;
        if (!task->is_directory(e)) {
            continue;
        }
        bool recent = false;
        for (std::filesystem::directory_iterator copy{task->path(), e}; !e && copy != end; copy.increment(e)) {
            std::error_code t;
            const auto when = copy->last_write_time(t);
            recent = recent || t || when > line; // a time it cannot tell: kept
        }
        if (!recent && !e) {
            std::filesystem::remove_all(task->path(), e);
        }
    }
}

} // namespace

auto permission_of(const security::Ruling& ruling, std::string_view preview, std::uint64_t id,
                   const security::Session& session, const security::Settings& settings) -> events::Permission {
    const auto& rq = ruling.request;
    events::Permission p{.id = id,
                         .tool = rq.tool,
                         .reason = ruling.reason,
                         .preview = std::string{preview},
                         .zone = ruling.zone ? std::string{security::paths::zone_name(*ruling.zone)} : std::string{},
                         .risk = std::string{security::risk_name(rq.risk)},
                         .warning = ruling.warning,
                         .advanced = ruling.advanced};
    const bool pairs = rq.tool == "fs.move" || rq.tool == "fs.copy";
    for (std::size_t i = 0; i < ruling.paths.size(); ++i) {
        if (pairs && i + 1 < ruling.paths.size()) {
            p.what.push_back(utf8(ruling.paths[i].path) + " -> " + utf8(ruling.paths[i + 1].path));
            ++i;
        } else {
            p.what.push_back(utf8(ruling.paths[i].path));
        }
    }
    if (!rq.command.empty()) {
        p.what.push_back(rq.command);
    }
    if (rq.background) { // phase 12 § 8: said on the card
        p.what.push_back("In the background, up to 2 hours; it ends when the agent closes");
    }
    if (rq.risk == security::Risk::ProcessKill) {
        p.what.push_back(std::format("{} (process {})", rq.process_name, rq.pid));
    }
    security::Session probe = session; // only asks whether the folder could be allowed
    p.folder = probe.allow_folder(ruling);
    p.task_scope = ruling.task_prefix; // a command's start, for the task (phase 12 § 7)
    p.task_allowed = session.commands();

    // The card (design/interaction-topbar, 2_permission): one row per object — the sources of a
    // move or a copy — with the policy's own paths.
    p.kind = kind_of(rq.risk);
    // A file for the quarantine is no list to tick: its card says its three rows (06a, card 3).
    for (std::size_t i = 0; rq.risk != security::Risk::Quarantine && i < ruling.paths.size(); i += pairs ? 2 : 1) {
        p.items.push_back(item_of(ruling.paths[i]));
    }
    std::uint64_t total = 0;
    for (const auto& item : p.items) {
        total += item.bytes;
    }
    if (!ruling.paths.empty()) {
        const std::filesystem::path parent = std::filesystem::path{ruling.paths.front().path}.parent_path();
        p.where = utf8(parent.wstring());
        p.folder_name = utf8(parent.filename().wstring());
    }
    const std::size_t n = p.items.size();
    const std::string count = std::format("{} {}", n, n == 1 ? "file" : "files");
    const std::string size = total > 0 ? " \xC2\xB7 " + sizes::text(total) : std::string{};
    switch (rq.risk) {
    case security::Risk::Delete: p.title = (rq.permanent ? "Delete for good " : "Delete ") + count + size; break;
    case security::Risk::RenameMove: p.title = "Move " + count + size; break;
    case security::Risk::Write: p.title = (n == 1 ? "Write " + (p.items.empty() ? std::string{} : p.items[0].name) : "Write " + count); break;
    case security::Risk::Read: p.title = "Read " + count; break;
    case security::Risk::Execute: p.title = rq.opens ? "Open " + (p.items.empty() ? rq.command : p.items[0].name) : "Run a command"; break;
    case security::Risk::ProcessKill: p.title = "End " + rq.process_name + "?"; break;
    case security::Risk::Clipboard: p.title = "Use the clipboard"; break;
    case security::Risk::Network: p.title = "Go online"; break;
    case security::Risk::Screen: p.title = "Show the screen to the model"; break;
    case security::Risk::Look: p.title = "Look at your window"; break;
    case security::Risk::Startup:
    case security::Risk::Uninstall:
    case security::Risk::Quarantine: break; // below
    }
    p.offers_permanent = rq.tool == "fs.trash" && settings.allow_permanent_delete;

    // Phase 13a (design/ui_system 06a): an action on the system says what happens, what it affects
    // and how to undo it, on buttons of its own words.
    if (rq.risk == security::Risk::Startup) {
        const bool task = rq.place == "scheduled task";
        const bool wmi = rq.place == "WMI subscription";
        const auto slash = rq.entry.find_last_of('\\');
        const std::string name = task && slash != std::string::npos ? rq.entry.substr(slash + 1) : rq.entry;
        p.affects = task  ? "Task Scheduler \xE2\x80\xBA " + rq.entry
                    : wmi ? "WMI root\\subscription \xE2\x80\xBA " + name
                          : rq.where + " \xE2\x86\x92 " + name;
        if (rq.back_on) {
            p.title = "Turn " + name + " back on at startup?";
            p.where = "Undo a switched-off autostart entry";
            p.happens = name + " starts with Windows again.";
            p.undo = "Turn it off again whenever you like.";
            p.allow_label = "Turn back on";
            p.deny_label = "Keep off";
        } else {
            p.title = "Turn off " + name + " at startup?";
            p.where = task ? "Switch off a scheduled task" : wmi ? "Take off a WMI subscription" : "Disable an autostart entry";
            p.happens = task ? "The task " + name + " won't run until it is switched back on. Nothing is deleted."
                        : wmi ? "The subscription " + name + " is taken off and kept, to be put back. Nothing runs from it meanwhile."
                              : name + " won't start with Windows. It still works when you open it yourself. Nothing is deleted.";
            p.undo = task  ? "Undo in the log switches it back on."
                     : wmi ? "Undo in the log puts it back as it was."
                           : "Undo in the log turns it back on. The entry is kept.";
            p.allow_label = "Turn off at startup";
            p.deny_label = "Keep";
        }
    } else if (rq.risk == security::Risk::Uninstall) {
        p.title = "Uninstall " + rq.entry + "?";
        p.where = "Remove a program with its own uninstaller";
        p.happens = rq.entry + "'s own uninstaller opens and removes the program. Your own files aren't touched.";
        p.affects = rq.where;
        p.undo = "Can't be undone \xE2\x80\x94 only by installing it again.";
        p.undo_ok = false;
        p.allow_label = "Uninstall\xE2\x80\xA6";
        p.deny_label = "Cancel";
        // Several versions (06b): each its own title, words and button; the others named as kept.
        for (const auto& c : rq.choices) {
            std::string others;
            for (const auto& o : rq.choices) {
                if (!(o == c)) {
                    others += (others.empty() ? "" : ", ") + o.name;
                }
            }
            const auto dot = c.where.rfind(" \xC2\xB7 ");
            p.picks.push_back({.name = c.name,
                               .detail = c.detail,
                               .size = dot == std::string::npos ? std::string{} : c.where.substr(dot + 4),
                               .title = "Uninstall " + c.name + "?",
                               .happens = c.name + "'s own uninstaller removes it. Your files and " + others + " aren't touched.",
                               .affects = c.where,
                               .allow = "Uninstall " + c.name + "\xE2\x80\xA6"});
        }
    } else if (rq.risk == security::Risk::Quarantine) {
        // The agent's quarantine (06a, card 3): in, back, or out for good.
        if (rq.tool == "quarantine.delete") {
            p.title = "Delete " + rq.entry + " for good?";
            p.where = "Remove a file from the quarantine";
            p.happens = rq.entry + " is deleted from the agent's quarantine.";
            p.affects = rq.where;
            p.undo = "Can't be undone.";
            p.undo_ok = false;
            p.allow_label = "Delete for good";
            p.deny_label = "Keep";
        } else if (rq.tool == "file.release") {
            p.title = "Put " + rq.entry + " back?";
            p.where = "Return a file from the quarantine";
            p.happens = rq.entry + " goes back where it was and can run again.";
            p.affects = rq.where;
            p.undo = "Quarantine it again whenever you like.";
            p.allow_label = "Put back";
            p.deny_label = "Keep in quarantine";
        } else if (rq.tool == "defender.scan") {
            p.title = "Scan " + rq.entry + " with Defender?";
            p.where = "Ask Microsoft Defender about a file";
            p.happens = "Microsoft Defender scans the file. If it finds a threat, Defender moves it into its own quarantine.";
            p.affects = rq.where;
            p.undo = "Restore it in Windows Security \xE2\x80\xBA Protection history.";
            p.allow_label = "Scan";
            p.deny_label = "Cancel";
        } else {
            p.title = "Quarantine " + rq.entry + "?";
            p.where = "Move a file into the agent's quarantine";
            p.happens = rq.along.empty() ? "The file moves into the agent's quarantine, where it can't run."
                                         : std::format("The file moves into the agent's quarantine, where it can't run. {} pointing to it {} switched off.",
                                                       rq.along.size() == 1 ? std::string{"Its autostart entry"} : std::format("{} autostart entries", rq.along.size()),
                                                       rq.along.size() == 1 ? "is" : "are");
            p.affects = rq.where;
            for (const auto& a : rq.along) {
                p.affects += " \xC2\xB7 " + a;
            }
            p.undo = "Undo in the log puts it back and turns its autostart on.";
            p.allow_label = "Quarantine";
            p.deny_label = "Cancel";
        }
    } else if (rq.risk == security::Risk::ProcessKill) {
        p.where = "Stop a process";
        p.happens = rq.process_name + " closes right away. Unsaved changes in it are lost.";
        p.affects = std::format("1 process \xC2\xB7 {} \xC2\xB7 PID {}", rq.process_name, rq.pid);
        p.undo = "Start it again. Unsaved changes can't be brought back.";
        p.undo_ok = false;
        p.allow_label = "End process";
        p.deny_label = "Cancel";
    }
    return p;
}

auto levels_of(const config::Access& access) -> events::Levels {
    return {.read_asks = access.read == config::ReadLevel::Ask, .change_per_folder = access.change == config::ChangeLevel::Folder};
}

auto access_of(const events::Levels& levels) -> config::Access {
    return {.read = levels.read_asks ? config::ReadLevel::Ask : config::ReadLevel::Auto,
            .change = levels.change_per_folder ? config::ChangeLevel::Folder : config::ChangeLevel::Always};
}

void Tools::set_levels(const events::Levels& levels) {
    std::scoped_lock lock{mutex_};
    host_.settings.read_asks = levels.read_asks;
    host_.settings.change_per_folder = levels.change_per_folder;
}

Tools::Tools(Host host) : host_(std::move(host)) {
    if (!host_.audit.empty()) {
        if (auto a = security::AuditLog::open(host_.audit)) {
            audit_.emplace(std::move(*a));
        } else {
            WA_WARN("agent", "the audit cannot be written ({}): tool calls are refused", a.error().message);
        }
    }
    if (!host_.journal.empty()) {
        journal_ = journal::read(host_.journal);
        tools::fs::set_undo_root(host_.journal.parent_path() / "undo"); // an edit's copy, beside the journal
        forget_old_copies(host_.journal.parent_path() / "undo");
    }
}

Tools::~Tools() {
    if (!host_.journal.empty()) {
        tools::fs::set_undo_root({});
    }
}

void Tools::new_task(std::uint64_t task) {
    std::scoped_lock lock{mutex_};
    sessions_[task] = {};
}

void Tools::heard(std::uint64_t task, std::string_view words) {
    std::scoped_lock lock{mutex_};
    sessions_[task].heard(words);
}

void Tools::begin(std::uint64_t task, std::string title) {
    new_task(task);
    heard(task, title);
    std::scoped_lock lock{mutex_};
    auto& e = entry(task);
    e.title = std::move(title);
    save(e);
}

auto Tools::take_over(std::uint64_t task) -> core::Result<void> {
    const auto folder = host_.desktop.wstring();
    if (audit_) {
        auto written = audit_->append({.task_id = task,
                                       .tool = "take_over",
                                       .args = R"({"folder":)" + history::json_string(core::utf8::to_utf8(folder).value_or("")) + "}",
                                       .zone = "User",
                                       .decision = "user",
                                       .decided_by = "user"});
        if (!written) {
            return written; // nothing happens unaudited
        }
    }
    return tools::os::current().open_terminal(folder);
}

auto Tools::undoable(std::uint64_t task) const -> bool {
    std::scoped_lock lock{mutex_};
    const auto found = std::ranges::find(journal_, task, &journal::Entry::task);
    return found != journal_.end() && !journal::undo_order(*found, std::nullopt).empty();
}

void Tools::remember_spots(std::uint64_t task, std::span<const std::string> paths) {
    if (!host_.icon_spots || paths.empty()) {
        return;
    }
    const auto spots = host_.icon_spots(paths); // the desktop is read without the lock
    std::scoped_lock lock{mutex_};
    auto& e = entry(task);
    for (const auto& s : spots) {
        if (s) {
            e.spots.push_back(*s);
        }
    }
    save(e);
}

auto Tools::last_task() const -> std::uint64_t {
    std::scoped_lock lock{mutex_};
    std::uint64_t last = 0;
    for (const auto& e : journal_) {
        last = std::max(last, e.task);
    }
    return last;
}

auto Tools::journal() const -> std::vector<journal::Entry> {
    std::scoped_lock lock{mutex_};
    return journal_;
}

auto Tools::entry(std::uint64_t task) -> journal::Entry& {
    for (auto& e : journal_) {
        if (e.task == task) {
            return e;
        }
    }
    return journal_.emplace_back(journal::Entry{.task = task, .title = {}, .started = history::now_utc(), .changes = {}});
}

void Tools::save(const journal::Entry& e) {
    if (host_.journal.empty()) {
        return;
    }
    if (auto r = journal::append(host_.journal, e); !r) {
        WA_WARN("agent", "changes.jsonl: {}", r.error().message);
    }
}

namespace {

// An edit's copy of what the file held, in <data>\undo\<task>\<n>.bin, and the file's hash right
// after it: what its Undo puts back, and how it knows the file changed since. No copy kept: the
// change has no way back (journal::inverse gives none), said in the log.
void keep_copy(std::uint64_t task, std::size_t n, const tools::Effect& effect, journal::Change& change) {
    const auto root = tools::fs::undo_root();
    if (root.empty()) {
        return;
    }
    const auto folder = root / std::to_string(task);
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const auto file = folder / std::format("{}.bin", n);
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(effect.before.data(), static_cast<std::streamsize>(effect.before.size()));
        if (!out) {
            WA_WARN("agent", "an edit's copy could not be kept in {}: it cannot be undone", file.string());
            return;
        }
    }
    std::ifstream now(std::filesystem::path{security::paths::long_form(effect.to)}, std::ios::binary);
    const std::string after{std::istreambuf_iterator<char>{now}, std::istreambuf_iterator<char>{}};
    change.backup = core::utf8::to_utf8(file.wstring()).value_or(std::string{});
    change.after = tools::fs::content_hash(after);
}

} // namespace

auto Tools::run(std::uint64_t task, std::string_view name, std::string_view args, const Ask& ask, const tools::Control& control)
    -> tools::Called {
    auto called = call(task, name, args, ask, control);
    if (!host_.journal.empty() && !called.outcome.effects.empty()) {
        std::scoped_lock lock{mutex_};
        auto& e = entry(task);
        for (const auto& effect : called.outcome.effects) {
            auto change = journal::change_of(effect);
            if (effect.kind == tools::EffectKind::Edited && effect.undoable) {
                keep_copy(task, e.changes.size(), effect, change);
            }
            e.changes.push_back(std::move(change));
        }
        save(e);
    }
    return called;
}

namespace {

// The effect of the call that took change `c` back.
auto takes_back(const tools::Effect& effect, const journal::Change& c) -> bool {
    const auto from = core::utf8::to_utf8(effect.from).value_or(std::string{});
    const auto to = core::utf8::to_utf8(effect.to).value_or(std::string{});
    switch (c.kind) {
    case tools::EffectKind::Moved:
    case tools::EffectKind::Renamed:
        return (effect.kind == tools::EffectKind::Moved || effect.kind == tools::EffectKind::Renamed) && from == c.to &&
               to == c.from;
    case tools::EffectKind::Created:
    case tools::EffectKind::Restored:
        return effect.kind == tools::EffectKind::Trashed && to == c.to;
    case tools::EffectKind::Trashed:
        return effect.kind == tools::EffectKind::Restored && to == c.to;
    case tools::EffectKind::Edited: // fs.revert's own edit of the same file
        return effect.kind == tools::EffectKind::Edited && to == c.to;
    case tools::EffectKind::StartupOff: // back on, or found on or gone already (phase 13a)
    case tools::EffectKind::TaskOff:
    case tools::EffectKind::WmiTaken:
        return effect.kind == tools::EffectKind::SwitchedOn && to == c.to;
    case tools::EffectKind::Quarantined: // back from the quarantine
        return effect.kind == tools::EffectKind::Released && to == c.to;
    default:
        return false;
    }
}

// The tool's own words for why it failed.
auto error_of(const tools::Outcome& o) -> std::string {
    glz::generic j;
    if (!glz::read_json(j, o.json) && j.is_object() && j.contains("error") && j["error"].is_string()) {
        return j["error"].get_string();
    }
    return "the call failed";
}

// A folder that holds anything (after its own files went back: things the task did not put there).
auto holds_anything(const std::string& path) -> bool {
    const auto p = core::utf8::to_wide(path).value_or(std::wstring{});
    std::error_code ec;
    return std::filesystem::is_directory(p, ec) && !std::filesystem::is_empty(p, ec);
}

} // namespace

auto Tools::undo(std::uint64_t task, std::uint64_t target, std::optional<std::size_t> which, const Ask& ask,
                 const tools::Control& control) -> Undone {
    Undone u;
    new_task(task); // an undo is asked afresh: no folder allowed before carries over
    journal::Entry e; // a copy: the calls below wait for the user, the journal stays free meanwhile
    {
        std::scoped_lock lock{mutex_};
        const auto found = std::ranges::find(journal_, target, &journal::Entry::task);
        if (found == journal_.end()) {
            return u;
        }
        e = *found;
    }
    const auto order = journal::undo_order(e, which);
    tools::Control own = control; // an Undo's calls are the agent's own: fs.revert among them
    own.agents_own = true;
    std::vector<journal::Spot> back; // the icons of the files that went back, where they stood
    const auto fail = [&](std::string why) {
        ++u.failed;
        if (u.first_error.empty()) {
            u.first_error = std::move(why);
        }
    };
    // Changes taken back by the same tool one after another go in one call: one question.
    for (std::size_t k = 0; k < order.size();) {
        const auto tool = journal::inverse(e.changes[order[k]])->name;
        // fs.move, fs.trash and fs.restore take many at once; any other goes back alone, with its own
        // arguments (an edit's copy and hash, a startup entry, a task, a subscription's MOF).
        const bool alone = tool != "fs.move" && tool != "fs.trash" && tool != "fs.restore";
        std::vector<std::size_t> batch;
        std::size_t next = k;
        for (; next < order.size() && journal::inverse(e.changes[order[next]])->name == tool && (!alone || batch.empty()); ++next) {
            const auto& c = e.changes[order[next]];
            if (c.kind == tools::EffectKind::Created && holds_anything(c.to)) {
                fail(std::format("{} holds files the task did not put there: it stays", c.to));
                continue;
            }
            batch.push_back(order[next]);
        }
        k = next;
        if (batch.empty()) {
            continue;
        }
        std::string args;
        if (alone) {
            args = journal::inverse(e.changes[batch[0]])->args;
        } else if (tool == "fs.move") {
            args = R"({"moves":[)";
            for (std::size_t i = 0; i < batch.size(); ++i) {
                const auto& c = e.changes[batch[i]];
                args += std::format(R"({}{{"from":{},"to":{}}})", i == 0 ? "" : ",", history::json_string(c.to),
                                    history::json_string(c.from));
            }
        } else {
            args = R"({"paths":[)";
            for (std::size_t i = 0; i < batch.size(); ++i) {
                args += (i == 0 ? "" : ",") + history::json_string(e.changes[batch[i]].to);
            }
        }
        if (!alone) {
            args += "]}";
        }
        const auto called = call(task, tool, args, ask, own);
        std::vector<std::size_t> taken;
        for (const auto& effect : called.outcome.effects) {
            for (const auto i : batch) {
                if (!e.changes[i].undone && takes_back(effect, e.changes[i])) {
                    e.changes[i].undone = true;
                    taken.push_back(i);
                    if (const auto s = std::ranges::find(e.spots, e.changes[i].from, &journal::Spot::path); s != e.spots.end()) {
                        back.push_back(*s);
                    }
                    break;
                }
            }
        }
        if (!taken.empty()) {
            std::scoped_lock lock{mutex_};
            auto& now = entry(target);
            for (const auto i : taken) {
                if (i < now.changes.size()) {
                    now.changes[i].undone = true;
                }
            }
            save(now);
        }
        u.done += taken.size();
        if (called.asked && !called.allowed) {
            u.refused = true;
            break;
        }
        for (std::size_t i = taken.size(); i < batch.size(); ++i) {
            fail(called.outcome.code != 0 ? error_of(called.outcome) : "not taken back");
        }
    }
    if (!back.empty() && host_.place_icons) {
        (void)host_.place_icons(back); // where they stood, not the first free cells
    }
    return u;
}

auto Tools::call(std::uint64_t task, std::string_view name, std::string_view args, const Ask& ask,
                 const tools::Control& control) -> tools::Called {
    if (!host_.audit.empty() && !audit_) {
        tools::Called refused;
        refused.outcome = tools::failure(name, core::Error{core::Code::Policy, "the audit cannot be written: no tool runs"});
        return refused;
    }
    // This task's session (a map node: it stays where it is while other tasks add theirs) and the
    // settings as they are now; one thread runs a task's calls.
    security::Session* session = nullptr;
    security::Settings settings;
    {
        std::scoped_lock lock{mutex_};
        session = &sessions_[task];
        settings = host_.settings;
    }
    const auto confirm = [&](const security::Ruling& ruling, std::string_view preview, const tools::Prepared* prepared) -> tools::Consent {
        auto question = permission_of(ruling, preview, ++asked_, *session, settings);
        if (prepared != nullptr && prepared->image) { // the picture itself, on the card: what a yes sends
            question.picture = present::picture_event(*prepared->image);
            if (prepared->title != "screen") {
                question.title = "Show \xE2\x80\x9C" + prepared->title + "\xE2\x80\x9D to the model";
            }
            if (!prepared->source.empty()) { // its panel: where it came from, what else could be taken (phase 10)
                question.capture = present::capture_event(*prepared);
            }
        }
        const auto answer = ask(std::move(question));
        if (answer && answer->choice == events::Choice::Retake) { // not an answer yet: taken again first
            tools::Consent again{false};
            again.retake = tools::Retake{.what = static_cast<tools::Retake::What>(answer->take), .monitor = answer->monitor,
                                         .window = answer->window};
            return again;
        }
        if (!answer || answer->choice == events::Choice::Deny) {
            return false;
        }
        // A file left out stays asked: with ticks cleared the folder is not allowed, only this call.
        if (answer->choice == events::Choice::AllowFolder && answer->keep.empty() && !session->allow_folder(ruling)) {
            return false; // not a folder that can be allowed: a click on nothing
        }
        if (answer->choice == events::Choice::AllowTask && !session->allow_command(ruling)) {
            return false; // no start was offered
        }
        tools::Consent consent{true};
        consent.keep = answer->keep;
        consent.permanent = answer->permanent;
        return consent;
    };
    tools::Control with_jobs = control; // shell.run in the background starts its command here (phase 12 § 8)
    with_jobs.jobs = &jobs_;
    with_jobs.task = task;
    auto called = tools::call(name, args, host_.desktop, settings, *session, confirm, audit_ ? &*audit_ : nullptr, task, with_jobs);
    WA_INFO("agent", "{} -> {} ({}), code {}", name, security::decision_name(called.ruling.decision),
            called.asked ? "asked" : "policy", called.outcome.code);
    // Folders this task made: moving into them is moving within the folder they were made in.
    for (const auto& e : called.outcome.effects) {
        if (e.kind == tools::EffectKind::Created) {
            std::error_code ec;
            if (std::filesystem::is_directory(e.to, ec)) {
                session->created(e.to);
            }
        }
    }
    return called;
}

} // namespace app::agent
