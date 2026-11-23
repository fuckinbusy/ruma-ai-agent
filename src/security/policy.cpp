#include "security/permit.hpp"
#include "security/policy.hpp"
#include "security/shellguard.hpp"

#include "core/utf8.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cwctype>
#include <format>

namespace security {
namespace {

using paths::Zone;

auto rank(Decision d) -> int {
    return static_cast<int>(d); // Auto < Ask < Deny < HardDeny
}

auto utf8(std::wstring_view w) -> std::string {
    auto s = core::utf8::to_utf8(w);
    return s ? std::move(*s) : std::string{"?"};
}

auto changes(Risk r) -> bool {
    return r == Risk::Write || r == Risk::RenameMove || r == Risk::Delete;
}

auto lower(std::string s) -> std::string {
    std::ranges::transform(s, s.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return s;
}

// A web address as the user would write it: lower case, no "http://", "https://" or "www.".
auto plain_web(std::string s) -> std::string {
    s = lower(std::move(s));
    for (const std::string_view drop : {"https://", "http://", "www."}) {
        for (auto at = s.find(drop); at != std::string::npos; at = s.find(drop, at)) {
            s.erase(at, drop.size());
        }
    }
    return s;
}

// A character a site's name or an address goes on with ("youtube.com" in "notyoutube.company" is
// not a site of its own).
auto web_char(char c) -> bool {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_';
}

// The system's own processes: ending one breaks the session or the machine.
auto system_process(std::string name) -> bool {
    name = lower(std::move(name));
    if (name.ends_with(".exe")) {
        name.resize(name.size() - 4);
    }
    static constexpr std::array<std::string_view, 18> kSystem{
        "system", "idle", "registry", "secure system", "memory compression", "smss", "csrss", "wininit", "winlogon",
        "services", "lsass", "lsaiso", "svchost", "explorer", "dwm", "fontdrvhost", "sihost", "msmpeng"};
    return std::ranges::find(kSystem, name) != kSystem.end();
}

// The strictest zone of the protected folders inside `folder` (User when none).
auto inner_zone(std::wstring_view folder, const paths::Roots& roots) -> Zone {
    Zone z = Zone::User;
    const auto inside = [&](const std::wstring& root) { return !root.empty() && paths::is_within(root, folder); };
    if (std::ranges::any_of(roots.secret_dirs, inside)) {
        z = paths::stricter(z, Zone::Secrets);
    }
    if (std::ranges::any_of(roots.agent_dirs, inside)) {
        z = paths::stricter(z, Zone::AppSelf);
    }
    if (inside(roots.system_root) || std::ranges::any_of(roots.program_files, inside) ||
        inside(roots.program_data + L"\\Microsoft")) {
        z = paths::stricter(z, Zone::System);
    }
    return z;
}

// Builds the ruling: a stricter decision replaces the reason, an equal question adds to it.
struct Builder {
    Ruling& out;
    void raise(Decision d, std::string why, bool advanced = false) {
        if (rank(d) > rank(out.decision)) {
            out.decision = d;
            out.reason = std::move(why);
            out.advanced = advanced;
        } else if (d == out.decision && d == Decision::Ask) {
            out.reason += out.reason.empty() ? why : "; " + why;
            out.advanced = out.advanced || advanced;
        }
    }
};

// A whole drive, the user's profile or a folder holding it (C:\Users).
auto whole(const std::wstring& path, const paths::Roots& roots) -> bool {
    const bool drive = path.size() <= 3 && path.size() >= 2 && path[1] == L':';
    return drive || paths::is_within(roots.profile, path);
}

auto lower_ascii(std::string_view s) -> std::string {
    std::string out{s};
    std::ranges::transform(out, out.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
    return out;
}

// shell.run (phase 12 § 7): the shell guard's class, the paths the command names through the
// zones, a start this task allowed.
void decide_command(Builder& b, const Request& rq, const Settings& s, const Session& session, const paths::Roots& roots) {
    using shellguard::Use;
    Ruling& out = b.out;
    const auto v = shellguard::classify(rq.command);
    Decision d = v.decision;
    if (d == Decision::Ask && !v.prefix.empty() && session.allowed_command(v.prefix)) {
        d = Decision::Auto;
        out.by_session = true;
    }
    b.raise(d, v.reason);
    if (d == Decision::Ask || out.by_session) {
        out.task_prefix = v.prefix; // offered; or, by the session, named in the reason
    }
    if (d == Decision::Auto && out.zone != Zone::User) {
        b.raise(Decision::Ask, "runs in a folder outside the user's own");
    }
    if (d == Decision::Auto && !out.by_session && s.read_asks) {
        b.raise(Decision::Ask, "reads: the access level asks about reading");
    }
    for (const auto& p : v.paths) {
        const auto c = paths::canonical(p.path, rq.working_dir);
        if (!c) {
            if (p.use == Use::Read || p.use == Use::List || (p.use == Use::Run && out.by_session)) {
                b.raise(Decision::Ask, "names a path the shell guard cannot check: " + utf8(p.path));
            }
            continue;
        }
        const Zone zone = paths::zone_of(*c, roots);
        const std::string where = utf8(c->path);
        switch (p.use) {
        case Use::Change:
            if (zone == Zone::System || zone == Zone::Secrets) {
                b.raise(Decision::HardDeny, std::format("changes {}: {}", where, zone == Zone::System ? "Windows' own" : "a secret"));
            } else if (zone == Zone::AppSelf) {
                b.raise(Decision::Deny, "changes the agent's own files: " + where);
            }
            if (p.tree && whole(c->path, roots)) {
                b.raise(Decision::HardDeny, "deletes a whole drive or the user's profile: " + where);
            }
            break;
        case Use::Read:
        case Use::List:
            if (zone == Zone::Secrets) {
                b.raise(Decision::Deny, "reads a secret: " + where);
            } else if (zone == Zone::AppSelf) {
                b.raise(Decision::Deny, "reads the agent's own files: " + where);
            } else if (zone == Zone::Other) {
                b.raise(Decision::Ask, "reads " + where + " on a network or removable drive");
            }
            // A listing of everything below: the names inside a secret or the agent's folder (final review S9).
            if (p.use == Use::List && p.tree) {
                if (const Zone inner = inner_zone(c->path, roots); inner == Zone::Secrets || inner == Zone::AppSelf) {
                    b.raise(Decision::Ask, std::format("lists every name below {}, {} inside", where,
                                                       inner == Zone::Secrets ? "secrets" : "the agent's own files"));
                }
            }
            break;
        case Use::Run:
            if (out.by_session && zone != Zone::User) {
                b.raise(Decision::Ask, std::format("names {}: the task allowed \xE2\x80\x9C{}\xE2\x80\x9D, not that path", where, v.prefix));
            }
            break;
        }
    }
}

// One path, one cell of 03-SECURITY § The decision matrix.
void decide_path(Builder& b, const Request& rq, const paths::Canonical& c, Zone zone, const Settings& s,
                 const Session& session, const paths::Roots& roots) {
    const std::string where = utf8(c.path);
    if (rq.risk == Risk::Read) {
        switch (zone) {
        case Zone::System: return; // auto
        case Zone::User:
            if (s.read_asks) { // the access level "Ask" for reads
                b.raise(Decision::Ask, "reads " + where);
            }
            return;
        case Zone::Secrets: {
            // The flag is for the user's own secrets: never the agent's (its key) or the
            // registry hives — by the path as resolved or as named (an MSIX-packaged parent puts
            // the agent's files in its LocalCache: named inside the agent's folder, resolved outside).
            const auto own_at = [&](const std::wstring& p) {
                return !p.empty() &&
                       (std::ranges::any_of(roots.agent_dirs, [&](const std::wstring& d) { return paths::is_within(p, d); }) ||
                        paths::is_within(p, roots.system_root + L"\\System32\\config"));
            };
            const bool own = own_at(c.path) || own_at(c.logical);
            if (s.allow_secrets_read && !c.device && !own) {
                b.raise(Decision::Ask, where + " holds secrets: readable only because advanced.allow_secrets_read is on",
                        true);
            } else {
                b.raise(Decision::Deny, where + " holds secrets (keys, passwords) or is a device: the agent does not read it");
            }
            return;
        }
        case Zone::AppSelf: b.raise(Decision::Deny, where + " is the agent's own data: tools do not touch it"); return;
        case Zone::Other: b.raise(Decision::Ask, where + " is on a network or removable drive"); return;
        }
        return;
    }

    if (c.ads) {
        b.raise(Decision::Deny, where + ": alternate data streams are never written");
        return;
    }
    const char* verb = rq.risk == Risk::Write ? "writes" : rq.risk == Risk::RenameMove ? "moves" : "deletes";
    switch (zone) {
    case Zone::System:
        if (paths::is_within(c.path, roots.program_data + L"\\Microsoft\\Windows\\Start Menu")) {
            b.raise(Decision::Ask, std::format("{} {} (the Start Menu)", verb, where));
        } else if (s.allow_system_writes && !s.agent_elevated) {
            b.raise(Decision::Ask, std::format("{} {}: a system folder, changed only because advanced.allow_system_writes is on", verb, where), true);
        } else {
            b.raise(Decision::HardDeny, std::format("{} is a system folder: the agent never changes it", where));
        }
        return;
    case Zone::Secrets: b.raise(Decision::HardDeny, where + " holds secrets or is a device: the agent never changes it"); return;
    case Zone::AppSelf: b.raise(Decision::Deny, where + " is the agent's own data: tools do not touch it"); return;
    case Zone::User:
        if ((rq.risk == Risk::Write || rq.risk == Risk::RenameMove) && s.change_per_folder && session.allowed(c)) {
            return; // auto: the user allowed this folder for the session (creating and moving)
        }
        b.raise(Decision::Ask, rq.risk == Risk::Delete && !rq.permanent ? std::format("moves {} to the Recycle Bin", where)
                                                                       : std::format("{} {}", verb, where));
        return;
    case Zone::Other:
        b.raise(Decision::Ask, std::format("{} {} on a network or removable drive", verb, where));
        return;
    }
}

// What opening a file with its program can run by itself (05-AI-AGENT § system.open: "ask, except
// files of the User zone and a web page the user named" — a program in Downloads is still a program).
// An allow-list (review 6): only these open without a question — documents, pictures, sound and
// video, with no macros. Anything else, known to run something or not known at all, is asked.
auto runs_something(std::wstring_view path) -> bool {
    const auto dot = path.find_last_of(L".\\");
    if (dot == std::wstring_view::npos || path[dot] != L'.') {
        return true; // no extension: unknown, asked about
    }
    std::wstring ext{path.substr(dot + 1)};
    std::ranges::transform(ext, ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    static constexpr std::array<std::wstring_view, 34> kDocuments{
        L"pdf",  L"txt", L"md",  L"rtf", L"doc",  L"docx", L"xls", L"xlsx", L"ppt",  L"pptx", L"odt", L"ods",
        L"odp",  L"csv", L"jpg", L"jpeg", L"png", L"gif",  L"bmp", L"webp", L"heic", L"tif",  L"tiff", L"mp3",
        L"wav",  L"flac", L"m4a", L"ogg", L"mp4", L"mkv",  L"mov", L"avi",  L"webm", L"epub"};
    return std::ranges::find(kDocuments, ext) == kDocuments.end();
}

void decide_open(Builder& b, const Request& rq, const Session& session, const paths::Roots& roots) {
    Ruling& out = b.out;
    if (rq.paths.empty()) {
        const std::string url = lower(rq.command);
        if (url.starts_with("https://") || url.starts_with("http://")) {
            if (session.named(rq.command)) {
                return; // auto: the page the user named, in the browser
            }
            // The model's own page (a search made of the user's words, say): it goes to the web.
            b.raise(Decision::Ask, "opens " + rq.command + " in the browser");
            return;
        }
        b.raise(rq.command.empty() ? Decision::Deny : Decision::Ask,
                rq.command.empty() ? std::string{"nothing to open"} : "opens " + rq.command);
        return;
    }
    if (rq.paths.size() != 1) {
        b.raise(Decision::Deny, "system.open opens one thing at a time");
        return;
    }
    auto c = paths::canonical(rq.paths[0], rq.working_dir);
    if (!c) {
        b.raise(Decision::Deny, std::format("{}: not a path the agent can use ({})", utf8(rq.paths[0]), c.error().message));
        return;
    }
    const Zone zone = paths::zone_of(*c, roots);
    const std::string where = utf8(c->path);
    out.zone = zone;
    out.paths.push_back(*c);
    out.zones.push_back(zone);
    const DWORD attrs = c->exists ? GetFileAttributesW((L"\\\\?\\" + c->path).c_str()) : INVALID_FILE_ATTRIBUTES;
    const bool folder = attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    switch (zone) {
    case Zone::Secrets:
    case Zone::AppSelf: b.raise(Decision::Deny, where + " is not shown: it holds secrets or the agent's own data"); return;
    case Zone::User:
        if (folder || !runs_something(c->path)) {
            return; // auto: the user's document in its program, or a folder in Explorer
        }
        b.raise(Decision::Ask, "runs " + where);
        return;
    case Zone::System:
    case Zone::Other: b.raise(Decision::Ask, (runs_something(c->path) && !folder ? "runs " : "opens ") + where); return;
    }
}

// startup.disable and its way back (phase 13a § 3, rules 1, 2 and 5): only Run, the startup folders,
// scheduled tasks and WMI subscriptions are switched; Winlogon, IFEO, AppInit, LSA, BootExecute
// and services never, not even with a yes; the whole computer's only by an agent running as
// administrator. Asked every time: nothing allows it for a task or a session.
void decide_startup(Builder& b, const Request& rq, const Settings& s) {
    static constexpr std::array<std::string_view, 6> kNever{"Winlogon",     "Image File Execution Options", "AppInit DLLs",
                                                            "LSA packages", "BootExecute",                  "service"};
    static constexpr std::array<std::string_view, 6> kSwitched{"run",           "run (32-bit)",   "startup folder",
                                                               "startup folder (all users)", "scheduled task", "WMI subscription"};
    const std::string& name = rq.entry;
    if (rq.place == "service") {
        b.raise(Decision::HardDeny,
                std::format("services are only looked at: the agent never starts, stops or changes one. To stop \xE2\x80\x9C{}\xE2\x80\x9D "
                            "starting yourself: open Services (services.msc), find it and set its Startup type to Manual or Disabled",
                            name));
        return;
    }
    if (std::ranges::find(kNever, rq.place) != kNever.end()) {
        b.raise(Decision::HardDeny,
                std::format("{} is never changed by the agent, not even with your yes: a mistake there can keep Windows from "
                            "starting or signing in. To remove \xE2\x80\x9C{}\xE2\x80\x9D yourself: open Registry Editor (regedit) at {}, "
                            "make sure it is not Windows' own, and remove it; or run a full scan in Windows Security",
                            rq.place, name, rq.where));
        return;
    }
    if (std::ranges::find(kSwitched, rq.place) == kSwitched.end()) {
        b.raise(Decision::Deny, std::format("{} is not something the agent switches off. To remove \xE2\x80\x9C{}\xE2\x80\x9D yourself: "
                                            "open Registry Editor (regedit) at {} and remove it",
                                            rq.place.empty() ? std::string{"an unknown place"} : rq.place, name, rq.where));
        return;
    }
    if (rq.place == "scheduled task" && lower(name).starts_with("\\microsoft\\windows\\")) {
        b.raise(Decision::Deny, std::format("{} is a task of Windows itself: the agent leaves it on", name));
        return;
    }
    if (rq.machine_wide && !s.agent_elevated) {
        const char* how = rq.place == "scheduled task"     ? "in Task Scheduler (taskschd.msc)"
                          : rq.place == "WMI subscription" ? "from PowerShell run as administrator"
                                                           : "in Task Manager \xE2\x80\xBA Startup apps";
        b.raise(Decision::Deny,
                std::format("\xE2\x80\x9C{}\xE2\x80\x9D is the whole computer's and the agent does not run as administrator: only you can, {}",
                            name, how));
        return;
    }
    b.raise(Decision::Ask, rq.back_on ? std::format("turns \xE2\x80\x9C{}\xE2\x80\x9D back on at startup", name)
                                      : std::format("turns \xE2\x80\x9C{}\xE2\x80\x9D off at startup", name));
}

// The agent's quarantine (phase 13a § 5.4, rule 4): a file in, back, or out for good. Never Windows'
// own (its folder, Microsoft's signature), the agent's own, secrets, a file off the local disks,
// nor one Winlogon, IFEO, AppInit, LSA or a service needs; the whole computer's (Program Files)
// only by an agent running as administrator. Asked every time.
void decide_quarantine(Builder& b, const Request& rq, const Settings& s, const paths::Roots& roots, Ruling& out) {
    if (rq.tool == "quarantine.delete") {
        b.raise(Decision::Ask, std::format("deletes \xE2\x80\x9C{}\xE2\x80\x9D from the quarantine for good: it cannot be undone", rq.entry));
        return;
    }
    if (rq.tool == "file.release") {
        b.raise(Decision::Ask, std::format("puts \xE2\x80\x9C{}\xE2\x80\x9D back where it was, able to run again", rq.entry));
        return;
    }
    if (rq.paths.size() != 1) {
        b.raise(Decision::Deny, "one file at a time");
        return;
    }
    const auto c = paths::canonical(rq.paths[0], rq.working_dir);
    if (!c || !c->exists || c->device || c->ads) {
        b.raise(Decision::Deny, std::format("{}: not a file the agent can quarantine", utf8(rq.paths[0])));
        return;
    }
    const auto zone = paths::zone_of(*c, roots);
    out.zone = zone;
    out.paths.push_back(*c);
    out.zones.push_back(zone);
    const std::string where = utf8(c->path);
    // Defender's own verdict: whatever it finds, Defender itself moves, so it is asked every time.
    if (rq.tool == "defender.scan") {
        b.raise(Decision::Ask, std::format("asks Microsoft Defender to scan {}; if it finds a threat, Defender moves the file "
                                           "into its own quarantine",
                                           where));
        return;
    }
    if (!roots.system_root.empty() && paths::is_within(c->path, roots.system_root)) {
        b.raise(Decision::HardDeny, where + " is in Windows' folder: the agent never moves Windows' files");
    } else if (rq.microsoft_signed) {
        b.raise(Decision::HardDeny, where + " is signed by Microsoft: the agent never moves Windows' files");
    } else if (zone == paths::Zone::AppSelf) {
        b.raise(Decision::HardDeny, where + " is the agent's own");
    } else if (zone == paths::Zone::Secrets) {
        b.raise(Decision::HardDeny, where + " holds secrets or is a device: the agent never moves it");
    } else if (zone == paths::Zone::Other && (c->unc || c->remote || c->removable)) {
        b.raise(Decision::HardDeny, where + " is not on a local disk");
    } else if (zone == paths::Zone::Other && !roots.users.empty() && paths::is_within(c->path, roots.users) &&
               !paths::is_within(c->path, roots.profile)) {
        b.raise(Decision::HardDeny, where + " belongs to another user of this computer: the agent works for you only");
    } else if (!rq.place.empty()) {
        b.raise(Decision::HardDeny,
                std::format("{} points to {}: without it Windows may not start or sign in, so the agent leaves it. To remove it "
                            "yourself: first take it out of {} (Registry Editor or Services), restart, then delete the file",
                            rq.place, where, rq.place));
    } else if ((zone == paths::Zone::System || rq.machine_wide) && !s.agent_elevated) {
        b.raise(Decision::Deny, std::format("{} belongs to the whole computer: only an agent running as administrator can quarantine it; "
                                            "or remove it yourself", where));
    } else {
        b.raise(Decision::Ask, rq.along.empty() ? std::format("moves {} into the agent's quarantine", where)
                                                : std::format("moves {} into the agent's quarantine and switches off {} autostart {}", where,
                                                              rq.along.size(), rq.along.size() == 1 ? "entry" : "entries"));
    }
}

} // namespace

auto risk_name(Risk r) -> std::string_view {
    switch (r) {
    case Risk::Read: return "read";
    case Risk::Write: return "write";
    case Risk::RenameMove: return "rename/move";
    case Risk::Delete: return "delete";
    case Risk::Execute: return "execute";
    case Risk::ProcessKill: return "process.kill";
    case Risk::Network: return "network";
    case Risk::Clipboard: return "clipboard";
    case Risk::Screen: return "screen";
    case Risk::Look: return "look";
    case Risk::Startup: return "startup";
    case Risk::Uninstall: return "uninstall";
    case Risk::Quarantine: return "quarantine";
    }
    return "?";
}

auto decision_name(Decision d) -> std::string_view {
    switch (d) {
    case Decision::Auto: return "auto";
    case Decision::Ask: return "ask";
    case Decision::Deny: return "deny";
    case Decision::HardDeny: return "hard-deny";
    }
    return "?";
}

// `parent` is `folder` itself, or a folder this task made inside it — not any other subfolder
// (review 8: a profile allowed once is not its startup folder or its shell profile).
auto Session::in_folder(const std::wstring& parent, const std::wstring& folder) const -> bool {
    const auto same = [](const std::wstring& a, const std::wstring& b) { return paths::is_within(a, b) && paths::is_within(b, a); };
    if (same(parent, folder)) {
        return true;
    }
    return paths::is_within(parent, folder) &&
           std::ranges::any_of(created_, [&](const std::wstring& made) { return same(parent, made); });
}

auto Session::allow_folder(const Ruling& ruling) -> bool {
    const Risk risk = ruling.request.risk;
    if ((risk != Risk::Write && risk != Risk::RenameMove) || !ruling.per_folder ||
        rank(ruling.decision) > rank(Decision::Ask) || ruling.paths.empty() ||
        !std::ranges::all_of(ruling.zones, [](Zone z) { return z == Zone::User; })) {
        return false; // never a delete; never when every change is to be asked
    }
    const std::wstring folder = std::filesystem::path{ruling.paths.front().path}.parent_path().wstring();
    for (const auto& c : ruling.paths) {
        if (!in_folder(std::filesystem::path{c.path}.parent_path().wstring(), folder)) {
            return false; // not one folder
        }
    }
    if (std::ranges::none_of(folders_, [&](const std::wstring& f) { return paths::is_within(folder, f) && paths::is_within(f, folder); })) {
        folders_.push_back(folder);
    }
    return true;
}

auto Session::allowed(const paths::Canonical& path) const -> bool {
    const std::wstring parent = std::filesystem::path{path.path}.parent_path().wstring();
    return std::ranges::any_of(folders_, [&](const std::wstring& f) { return in_folder(parent, f); });
}

void Session::heard(std::string_view words) {
    heard_ += ' ';
    heard_ += plain_web(std::string{words});
}

auto Session::named(std::string_view url) const -> bool {
    std::string u = plain_web(std::string{url});
    while (!u.empty() && u.back() == '/') {
        u.pop_back();
    }
    if (u.empty()) {
        return false;
    }
    for (auto at = heard_.find(u); at != std::string::npos; at = heard_.find(u, at + 1)) {
        const std::size_t end = at + u.size();
        const bool starts = at == 0 || (!web_char(heard_[at - 1]) && heard_[at - 1] != '.');
        const bool ends = end == heard_.size() || !web_char(heard_[end]);
        if (starts && ends) {
            return true;
        }
    }
    return false;
}

void Session::created(std::wstring folder) {
    while (!folder.empty() && (folder.back() == L'\\' || folder.back() == L'/')) {
        folder.pop_back();
    }
    if (!folder.empty()) {
        created_.push_back(std::move(folder));
    }
}

auto Session::allow_command(const Ruling& ruling) -> bool {
    if (ruling.request.risk != Risk::Execute || ruling.decision != Decision::Ask || ruling.task_prefix.empty()) {
        return false;
    }
    if (!allowed_command(ruling.task_prefix)) {
        commands_.push_back(ruling.task_prefix);
    }
    return true;
}

auto Session::allowed_command(std::string_view prefix) const -> bool {
    const auto wanted = lower_ascii(prefix);
    return std::ranges::any_of(commands_, [&](const std::string& c) { return lower_ascii(c) == wanted; });
}

void Session::note(const Request& request, bool confirmed) {
    if (confirmed) { // the user saw this one: the limits count from here
        calls_ = 0;
        files_ = 0;
        bytes_ = 0;
        return;
    }
    ++calls_;
    if (changes(request.risk)) {
        // A folder counts with what is in it (review 10).
        files_ += std::max<std::uint64_t>(request.paths.size(), request.tree_objects);
        bytes_ += request.bytes;
    }
}

auto decide(const Request& rq, const Settings& s, const Session& session, const paths::Roots& roots) -> Ruling {
    Ruling out{.decision = Decision::Auto, .per_folder = s.change_per_folder, .request = rq};
    Builder b{out};

    switch (rq.risk) {
    case Risk::Read:
    case Risk::Write:
    case Risk::RenameMove:
    case Risk::Delete: {
        // A read of no file (system.info, process.list) is a read; a change needs its object.
        // guarded_fs opens nothing outside a Permit's paths, so a path-less Permit reads no file.
        if (rq.paths.empty() && rq.risk != Risk::Read) {
            b.raise(Decision::Deny, std::format("{} names no path", rq.tool));
        }
        for (const auto& p : rq.paths) {
            auto c = paths::canonical(p, rq.working_dir);
            if (!c) {
                b.raise(Decision::Deny, std::format("{}: not a path the agent can use ({})", utf8(p), c.error().message));
                continue;
            }
            Zone zone = paths::zone_of(*c, roots);
            // Moving or deleting a folder moves or deletes what is in it: a folder holding a
            // protected one (%LOCALAPPDATA% holds the agent's data and the browsers' passwords)
            // is judged by the strictest inside it.
            if (rq.risk == Risk::RenameMove || rq.risk == Risk::Delete) {
                zone = paths::stricter(zone, inner_zone(c->path, roots));
            }
            decide_path(b, rq, *c, zone, s, session, roots);
            out.zone = out.zone ? paths::stricter(*out.zone, zone) : zone;
            out.paths.push_back(std::move(*c));
            out.zones.push_back(zone);
        }
        // A question about many paths: how many (the card lists them; the reasons, one per
        // path, would only repeat them). A stricter decision keeps its own reason.
        // Only where the reasons say nothing more than the verb: all in the user's folders, or
        // all on a share (which the summary keeps saying); a Start Menu path keeps its own.
        const bool all_user = std::ranges::all_of(out.zones, [](Zone z) { return z == Zone::User; });
        const bool all_other = std::ranges::all_of(out.zones, [](Zone z) { return z == Zone::Other; });
        if (out.decision == Decision::Ask && rq.paths.size() > 1 && !out.advanced && (all_user || all_other) &&
            out.zones.size() == rq.paths.size()) {
            const char* what = rq.risk == Risk::Read         ? "reads"
                               : rq.risk == Risk::Write      ? "writes"
                               : rq.risk == Risk::RenameMove ? "moves"
                               : rq.permanent                ? "deletes for good"
                                                             : "moves to the Recycle Bin";
            out.reason = std::format("{} {} paths{}", what, rq.paths.size(), all_other ? " on a network or removable drive" : "");
        }
        if (rq.risk == Risk::Delete && rq.permanent) {
            if (!s.allow_permanent_delete) {
                b.raise(Decision::Deny, "permanent delete is off (advanced.allow_permanent_delete): use the Recycle Bin");
            } else {
                b.raise(Decision::Ask, "deletes permanently, not to the Recycle Bin", true);
            }
        }
        break;
    }
    case Risk::Execute:
        if (rq.opens) {
            decide_open(b, rq, session, roots);
            break;
        }
        if (auto wd = paths::canonical(rq.working_dir.wstring(), rq.working_dir)) {
            out.zone = paths::zone_of(*wd, roots);
        }
        if (rq.command.empty()) {
            b.raise(Decision::Deny, "no command");
        } else {
            decide_command(b, rq, s, session, roots);
        }
        break;
    case Risk::ProcessKill: {
        // Phase 13a § 5.4: a Windows name is Windows' own when its file is in Windows' folder or
        // signed by Microsoft (or where it is cannot be told) — never ended; elsewhere and unsigned
        // it is a look-alike, asked about. One running as SYSTEM: only by an agent running as administrator.
        const bool in_windows = !roots.system_root.empty() && !rq.process_path.empty() && paths::is_within(rq.process_path, roots.system_root);
        const bool windows_own = rq.process_path.empty() || in_windows || rq.microsoft_signed;
        if (rq.pid == 0 || rq.pid == 4 || (system_process(rq.process_name) && windows_own)) {
            b.raise(Decision::HardDeny, std::format("{} (pid {}) is part of Windows: the agent never ends it",
                                                    rq.process_name, rq.pid));
        } else if (system_process(rq.process_name)) {
            b.raise(Decision::Ask, std::format("ends {} (pid {}): it has a Windows name but is not Windows' own \xE2\x80\x94 {}, not signed by "
                                               "Microsoft", rq.process_name, rq.pid, utf8(rq.process_path)));
        } else if (rq.process_system && windows_own) { // Windows' own, or one it cannot be told is not
            b.raise(Decision::HardDeny, std::format("{} (pid {}) is part of Windows: the agent never ends it", rq.process_name, rq.pid));
        } else if (rq.process_system && !s.agent_elevated) {
            b.raise(Decision::Deny, std::format("{} (pid {}) runs as SYSTEM or as a service: only an agent running as administrator "
                                                "can end it; or end it yourself in Task Manager", rq.process_name, rq.pid));
        } else if (rq.pid == GetCurrentProcessId()) {
            b.raise(Decision::Deny, "that is the agent itself");
        } else {
            b.raise(Decision::Ask, std::format("ends {} (pid {})", rq.process_name, rq.pid));
        }
        break;
    }
    case Risk::Network: b.raise(Decision::Deny, "the agent has no network tools"); break;
    case Risk::Clipboard: b.raise(Decision::Ask, "uses the clipboard (it may hold passwords)"); break;
    case Risk::Screen: b.raise(Decision::Ask, "shows the model what is on the screen (it may be anything open there)"); break;
    case Risk::Look: break; // the user's window, unasked but always shown; a sensitive one never read (phase 13a rule 7)
    case Risk::Startup: decide_startup(b, rq, s); break;
    case Risk::Uninstall:
        // Phase 13a § 4: Windows' parts and the agent itself never; any other program asked every
        // time — its uninstaller asks for administrator rights itself (its own UAC) when it needs them.
        if (rq.place == "program") {
            b.raise(Decision::Ask, std::format("removes {} with its own uninstaller: it cannot be undone", rq.entry));
        } else if (rq.place == "the agent itself") {
            b.raise(Decision::HardDeny, "that is the agent itself: remove it yourself in Settings \xE2\x80\xBA Apps, or with its installer");
        } else {
            b.raise(Decision::HardDeny, std::format("{} is a part of Windows ({}): the agent never removes it", rq.entry,
                                                    rq.place.empty() ? std::string{"unknown"} : rq.place));
        }
        break;
    case Risk::Quarantine: decide_quarantine(b, rq, s, roots, out); break;
    }

    // Running as administrator: every change and every command asked, in red.
    if (s.agent_elevated && (changes(rq.risk) || rq.risk == Risk::Execute || rq.risk == Risk::ProcessKill ||
                             rq.risk == Risk::Startup || rq.risk == Risk::Uninstall || rq.risk == Risk::Quarantine)) {
        out.warning = true;
        b.raise(Decision::Ask, "the agent runs as administrator");
    }

    // A call that must ask (an undo over changes made since): never softer, asked whatever allows it.
    if (!rq.must_ask.empty()) {
        b.raise(Decision::Ask, rq.must_ask);
    }

    // Blast radius, since the last confirmation.
    if (session.calls() >= s.max_calls) {
        b.raise(Decision::Ask, std::format("{} tool calls in this task: continue?", session.calls()));
    }
    if (changes(rq.risk)) {
        if (session.files() + rq.paths.size() > s.max_files) {
            b.raise(Decision::Ask, std::format("more than {} files changed in this task without a confirmation", s.max_files));
        }
        if (session.bytes() + rq.bytes > s.max_bytes) {
            b.raise(Decision::Ask, std::format("more than {} MB written in this task without a confirmation", s.max_bytes >> 20));
        }
        if (rq.risk == Risk::Delete && rq.paths.size() > s.max_trash_batch) {
            b.raise(Decision::Ask, std::format("{} objects at once (more than {})", rq.paths.size(), s.max_trash_batch));
        }
        if (rq.tree_objects > s.max_tree) {
            b.raise(Decision::Ask, std::format("{} objects inside (more than {})", rq.tree_objects, s.max_tree));
        }
    }
    if (out.decision == Decision::Auto) {
        out.reason = out.by_session
                         ? std::format("{}: allowed for this task (\xE2\x80\x9C{}\xE2\x80\x9D)", risk_name(rq.risk), out.task_prefix)
                         : std::format("{}: allowed by the policy", risk_name(rq.risk));
    }
    // A start offered for the task only on a plain question; "by the session" only when it ran so.
    if (out.decision != Decision::Ask || out.warning || out.advanced) {
        out.task_prefix.clear();
    }
    if (out.decision != Decision::Auto) {
        out.by_session = false;
    }
    return out;
}

auto grant(const Ruling& ruling, const Request& request, bool user_confirmed, Session& session) -> std::optional<Permit> {
    if (!(ruling.request == request)) {
        return std::nullopt; // decided for something else
    }
    switch (ruling.decision) {
    case Decision::HardDeny:
    case Decision::Deny: return std::nullopt;
    case Decision::Ask:
        if (!user_confirmed) {
            return std::nullopt;
        }
        break;
    case Decision::Auto: break;
    }
    const bool by_user = ruling.decision == Decision::Ask;
    session.note(request, by_user);
    return Permit{request, ruling.paths, ruling.zones, by_user};
}

} // namespace security
