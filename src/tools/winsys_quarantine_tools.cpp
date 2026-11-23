#include "tools/winsys_tools.hpp"

#include "core/json.hpp"
#include "core/utf8.hpp"
#include "security/paths.hpp"
#include "tools/os.hpp"
#include "tools/winsys/check.hpp"
#include "tools/winsys/defender.hpp"
#include "tools/winsys/signature.hpp"
#include "tools/winsys/startup.hpp"

#include <glaze/glaze.hpp>

#include <windows.h>

#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cwctype>
#include <format>
#include <fstream>
#include <optional>

// The agent's own quarantine (phase 13a § 5.4): a file moved into data\quarantine\<id>\ with its
// meta.json (where it came from, its hash, when, why), its execute right denied to everyone, and
// the autostart entries and tasks that start it switched off by the same yes. Undo (file.release)
// takes the rule off, puts it back and turns them on; quarantine.delete removes it for good.
// Never a file Windows, the agent or a service needs (the policy); never one that runs (end it first).

namespace tools::winsys::args {

struct Quarantine {
    std::string path;
    std::optional<std::string> why; // the finding's reason, kept in meta.json
};
struct Kept {
    std::string dir; // its folder in the quarantine
};
struct Scan {
    std::string path;
};

} // namespace tools::winsys::args

namespace tools::winsys::out {

struct Quarantined {
    std::string file, kept_in;
    std::vector<std::string> switched_off;
    std::string note = "It cannot run there. Undo in the log puts it back and turns its autostart on.";
    std::vector<std::string> not_switched_off; // "\UpdTask (Access is denied.)": still on, said so
};
struct Released {
    std::string file, back_to, note;
};
struct DeletedForGood {
    std::string file;
    std::string note = "Deleted from the quarantine for good.";
};
struct Meta {
    std::string from, sha256, why;
    std::int64_t at = 0;
};
struct Scanned {
    std::string file, verdict, threat, note;
};

} // namespace tools::winsys::out

namespace tools::winsys {
namespace {

auto utf8(std::wstring_view s) -> std::string {
    return core::utf8::to_utf8(s).value_or("?");
}

auto wide(std::string_view s) -> std::wstring {
    return core::utf8::to_wide(s).value_or(std::wstring{});
}

auto lower(std::wstring_view s) -> std::wstring {
    std::wstring out{s};
    std::ranges::transform(out, out.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return out;
}

auto same(std::wstring_view a, std::wstring_view b) -> bool {
    return !a.empty() && lower(a) == lower(b);
}

template <class T>
auto read_args(std::string_view json) -> core::Result<T> {
    T a{};
    if (const auto ec = glz::read<glz::opts{.error_on_unknown_keys = true}>(a, json)) {
        return core::make_error(core::Code::InvalidArgument, "arguments: " + glz::format_error(ec, json));
    }
    return a;
}

template <class T>
auto said(std::string_view tool, const T& data, std::vector<Effect> effects = {}) -> Outcome {
    auto json = core::json::write(data);
    Outcome o{0, wrap(tool, L"", json ? *json : std::string{R"("?")"}, false)};
    o.effects = std::move(effects);
    return o;
}

auto size_text(std::uint64_t bytes) -> std::string {
    constexpr std::array<const char*, 5> kUnits{"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(bytes);
    std::size_t u = 0;
    while (v >= 1024.0 && u + 1 < kUnits.size()) {
        v /= 1024.0;
        ++u;
    }
    return v < 10.0 && u > 0 ? std::format("{:.1f} {}", v, kUnits[u]) : std::format("{:.0f} {}", v, kUnits[u]);
}

auto expanded(const std::wstring& s) -> std::wstring {
    const DWORD n = ExpandEnvironmentStringsW(s.c_str(), nullptr, 0);
    if (n == 0) {
        return s;
    }
    std::wstring out(n, L'\0');
    ExpandEnvironmentStringsW(s.c_str(), out.data(), n);
    out.resize(wcsnlen(out.c_str(), out.size()));
    return out;
}

// What starts the file, and what may not lose it.
struct Pointing {
    std::vector<StartupEntry> entries; // Run, Run32, the startup folders: switched off with it
    std::vector<TaskInfo> tasks;       // scheduled tasks: switched off with it
    std::vector<WmiSubscription> wmi;  // WMI subscriptions that run it: taken off and kept with it
    std::string needed_by;             // Winlogon, IFEO, AppInit, LSA, BootExecute or a service: no quarantine
};

auto pointing_to(const std::wstring& file) -> Pointing {
    const auto& ctx = context();
    Pointing p;
    // As written, and as Windows reads it: %VARIABLES% expanded (REG_EXPAND_SZ values).
    const auto names = [&](const std::wstring& command, const std::wstring& f) {
        const auto has = [&](const std::wstring& c) { return !c.empty() && lower(c).find(lower(file)) != std::wstring::npos; };
        return same(f, file) || has(command) || (command.find(L'%') != std::wstring::npos && has(expanded(command)));
    };
    for (const auto& e : startup_entries(ctx.registry, ctx.folders).entries) {
        if (!names(e.command, e.file)) {
            continue;
        }
        switch (e.place) {
        case Place::Run:
        case Place::Run32:
        case Place::StartupFolder:
        case Place::CommonStartupFolder:
            if (e.enabled) {
                p.entries.push_back(e);
            }
            break;
        case Place::Winlogon:
        case Place::Ifeo:
        case Place::AppInit:
        case Place::Lsa:
        case Place::BootExecute:
            if (p.needed_by.empty()) {
                p.needed_by = std::string{place_name(e.place)};
            }
            break;
        default: break;
        }
    }
    for (const auto& t : ctx.read_tasks().tasks) {
        if (t.enabled && names(t.command, t.file)) {
            p.tasks.push_back(t);
        }
    }
    if (ctx.read_wmi) {
        for (const auto& w : ctx.read_wmi().subscriptions) {
            if (names(w.command, {})) {
                p.wmi.push_back(w);
            }
        }
    }
    if (p.needed_by.empty() && ctx.read_services) {
        for (const auto& v : ctx.read_services()) {
            if (names(v.command, v.file) || same(v.dll, file)) { // svchost's services: the DLL it loads
                p.needed_by = "the service \xE2\x80\x9C" + (v.display.empty() ? v.name : v.display) + "\xE2\x80\x9D";
                break;
            }
        }
    }
    return p;
}

// A process running from the file: its id; 0: none.
auto running_from(const std::wstring& file) -> std::uint32_t {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return 0;
    }
    std::uint32_t found = 0;
    PROCESSENTRY32W e{.dwSize = sizeof(PROCESSENTRY32W)};
    for (BOOL ok = Process32FirstW(snap, &e); ok && found == 0; ok = Process32NextW(snap, &e)) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, e.th32ProcessID);
        if (h == nullptr) {
            continue;
        }
        std::array<wchar_t, 1024> image{};
        DWORD n = static_cast<DWORD>(image.size());
        if (QueryFullProcessImageNameW(h, 0, image.data(), &n) && same(std::wstring_view{image.data(), n}, file)) {
            found = e.th32ProcessID;
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    return found;
}

auto entry_id(const StartupEntry& e) -> std::wstring {
    return wide(place_name(e.place)) + L"\n" + e.key_or_folder + L"\n" + e.name;
}

auto along_of(const Pointing& p) -> std::vector<std::string> {
    std::vector<std::string> out;
    for (const auto& e : p.entries) {
        out.push_back(std::format("{} \xE2\x80\xBA {}", place_name(e.place), utf8(e.name)));
    }
    for (const auto& t : p.tasks) {
        out.push_back("scheduled task \xE2\x80\xBA " + t.path);
    }
    for (const auto& w : p.wmi) {
        out.push_back("WMI subscription \xE2\x80\xBA " + w.consumer);
    }
    return out;
}

// `dir` is a folder of the quarantine (never the quarantine itself, nothing outside it).
auto in_quarantine(const std::filesystem::path& dir) -> bool {
    const auto& q = context().quarantine;
    if (q.empty() || dir.empty()) {
        return false;
    }
    const auto a = lower(dir.lexically_normal().wstring());
    const auto b = lower(q.lexically_normal().wstring());
    return a.size() > b.size() + 1 && a.starts_with(b) && (a[b.size()] == L'\\' || a[b.size()] == L'/');
}

auto meta_of(const std::filesystem::path& dir) -> std::optional<out::Meta> {
    std::error_code ec;
    const auto size = std::filesystem::file_size(dir / "meta.json", ec);
    if (ec || size > 64 * 1024) {
        return std::nullopt;
    }
    std::string text(static_cast<std::size_t>(size), '\0');
    if (FILE* f = _wfopen((dir / "meta.json").c_str(), L"rb"); f != nullptr) {
        const auto got = std::fread(text.data(), 1, text.size(), f);
        std::fclose(f);
        text.resize(got);
    }
    out::Meta m;
    if (glz::read<glz::opts{.error_on_unknown_keys = false}>(m, text)) {
        return std::nullopt;
    }
    return m;
}

auto kept_request(std::string_view tool, std::string_view json, const std::filesystem::path& wd)
    -> core::Result<std::pair<security::Request, out::Meta>> {
    const auto a = read_args<args::Kept>(json);
    if (!a) {
        return std::unexpected{a.error()};
    }
    const std::filesystem::path dir{wide(a->dir)};
    const auto meta = in_quarantine(dir) ? meta_of(dir) : std::nullopt;
    if (!meta) {
        return core::make_error(core::Code::InvalidArgument, "not a folder of the agent's quarantine: " + a->dir);
    }
    const auto from = std::filesystem::path{wide(meta->from)};
    return std::pair{security::Request{.tool = std::string{tool}, .risk = security::Risk::Quarantine, .working_dir = wd,
                                       .entry = utf8(from.filename().wstring()), .where = meta->from},
                     *meta};
}

} // namespace

// file.quarantine

auto quarantine_schema() -> std::string {
    auto s = glz::write_json_schema<args::Quarantine>();
    return s ? std::move(*s) : std::string{"{}"};
}

auto quarantine_request(std::string_view json, const std::filesystem::path& wd) -> core::Result<security::Request> {
    const auto a = read_args<args::Quarantine>(json);
    if (!a) {
        return std::unexpected{a.error()};
    }
    // As the file system names it: what points to it and the run's own check see the same path.
    const auto c = security::paths::canonical(wide(a->path), wd);
    std::error_code ec;
    if (a->path.empty() || !c || !c->exists || !std::filesystem::is_regular_file(c->path, ec)) {
        return core::make_error(core::Code::NotFound, "no file " + a->path);
    }
    const std::wstring path = c->path;
    const bool microsoft = signature(std::filesystem::path{path}).microsoft;
    // Windows' own is refused by the policy whatever runs it: asked about running only what may be quarantined.
    if (!microsoft) {
        if (const auto pid = running_from(path); pid != 0) {
            return core::make_error(core::Code::InvalidArgument,
                                    std::format("{} is running (process {}): end it first with process.kill, then quarantine it", a->path, pid));
        }
    }
    const auto p = pointing_to(path);
    const auto bytes = std::filesystem::file_size(path, ec);
    security::Request rq{.tool = "file.quarantine", .risk = security::Risk::Quarantine, .paths = {path}, .working_dir = wd,
                         .place = p.needed_by, .entry = utf8(std::filesystem::path{path}.filename().wstring()),
                         .where = utf8(path) + (ec ? std::string{} : " \xC2\xB7 " + size_text(bytes)),
                         // Started for every user, by another account or by WMI: the whole computer's.
                         .machine_wide = std::ranges::any_of(p.entries, &StartupEntry::machine_wide) || !p.wmi.empty() ||
                                         std::ranges::any_of(p.tasks, [](const TaskInfo& t) { return !t.account.empty() && !own_account(t.account); })};
    rq.microsoft_signed = microsoft;
    rq.along = along_of(p);
    return rq;
}

auto quarantine_run(const security::Permit& permit, std::string_view json) -> Outcome {
    constexpr std::string_view tool = "file.quarantine";
    const auto a = read_args<args::Quarantine>(json);
    if (!a) {
        return failure(tool, a.error());
    }
    if (permit.risk() != security::Risk::Quarantine || permit.paths().size() != 1) {
        return failure(tool, core::Error{core::Code::Policy, "a quarantine needs its own permission"});
    }
    const std::wstring path = permit.paths().front().path;
    if (running_from(path) != 0) {
        return failure(tool, core::Error{core::Code::Policy, "it started running since it was asked about: end it first"});
    }
    const auto& ctx = context();
    if (ctx.quarantine.empty()) {
        return failure(tool, core::Error{core::Code::Internal, "there is no quarantine folder"});
    }
    const auto p = pointing_to(path);
    if (!p.needed_by.empty() || along_of(p) != permit.request().along) {
        return failure(tool, core::Error{core::Code::Policy, "what starts it changed since it was asked about: ask again"});
    }
    static std::atomic<int> n{0};
    const auto when = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const auto dir = ctx.quarantine / std::format("{}-{}", when, n.fetch_add(1));
    const auto name = std::filesystem::path{path}.filename();
    const auto kept = dir / name;
    const auto sha = signature(std::filesystem::path{path}).sha256;
    auto& os = os::current();
    if (auto made = os.make_dir(dir.wstring()); !made) {
        return failure(tool, made.error());
    }
    if (auto moved = os.move(path, kept.wstring(), false); !moved) {
        return failure(tool, moved.error());
    }
    const auto meta = core::json::write(out::Meta{.from = utf8(path), .sha256 = sha, .why = a->why.value_or(""), .at = when});
    // Without its meta.json the file could never be put back (file.release reads it): undone at once.
    if (auto kept_meta = os.write_file((dir / "meta.json").wstring(), meta ? *meta : std::string{"{}"}, os::WriteMode::CreateOnly);
        !kept_meta) {
        (void)os.move(kept.wstring(), path, false);
        (void)os.remove(dir.wstring());
        return failure(tool, kept_meta.error());
    }
    if (auto denied = os.deny_execute(kept.wstring(), true); !denied) {
        (void)os.move(kept.wstring(), path, false); // not locked: back where it was, nothing half done
        return failure(tool, denied.error());
    }
    std::vector<Effect> effects{
        Effect{.kind = EffectKind::Quarantined, .from = path, .to = dir.wstring(), .undoable = true, .before = {}}};
    out::Quarantined said_out{.file = utf8(path), .kept_in = utf8(dir.wstring()), .switched_off = {}};
    // What starts it, off by the same yes; what would not go off is said, with why.
    const auto failed = [&](std::string what, const core::Error& e) { said_out.not_switched_off.push_back(what + " (" + e.message + ")"); };
    for (const auto& e : p.entries) {
        if (auto off = os.set_startup_approved(ctx.registry, e, false); off) {
            effects.push_back({.kind = EffectKind::StartupOff, .from = e.name, .to = entry_id(e), .undoable = true, .before = {}});
            said_out.switched_off.push_back(utf8(e.name));
        } else {
            failed(utf8(e.name), off.error());
        }
    }
    for (const auto& t : p.tasks) {
        if (auto off = os.set_task_enabled(wide(t.path), false); off) {
            effects.push_back({.kind = EffectKind::TaskOff, .from = wide(t.path), .to = wide(t.path), .undoable = true, .before = {}});
            said_out.switched_off.push_back(t.path);
        } else {
            failed(t.path, off.error());
        }
    }
    for (const auto& w : p.wmi) {
        // Taken off and kept as its MOF beside the quarantine's files: wmi.restore puts it back (Undo).
        const auto mof_dir = ctx.quarantine / "wmi";
        const auto file = mof_dir / std::format("{}-{}.mof", when, n.fetch_add(1));
        std::error_code ec;
        std::filesystem::create_directories(mof_dir, ec);
        auto mof = os.take_wmi_subscription(w);
        if (!mof) {
            failed("WMI subscription " + w.consumer, mof.error());
            continue;
        }
        std::ofstream out(file, std::ios::binary);
        out << *mof;
        out.close();
        if (!out) {
            (void)os.put_wmi_subscription(*mof); // not kept: put back at once, nothing lost
            failed("WMI subscription " + w.consumer, core::Error{core::Code::Io, "it could not be kept, so it was put back"});
            continue;
        }
        effects.push_back({.kind = EffectKind::WmiTaken, .from = wide(w.filter), .to = file.wstring(), .undoable = true, .before = {}});
        said_out.switched_off.push_back("WMI subscription " + w.consumer);
    }
    return said(tool, said_out, std::move(effects));
}

// file.release — back from the quarantine (the agent's Undo)

auto release_schema() -> std::string {
    auto s = glz::write_json_schema<args::Kept>();
    return s ? std::move(*s) : std::string{"{}"};
}

auto release_request(std::string_view json, const std::filesystem::path& wd) -> core::Result<security::Request> {
    auto r = kept_request("file.release", json, wd);
    if (!r) {
        return std::unexpected{r.error()};
    }
    return std::move(r->first);
}

auto release_run(const security::Permit& permit, std::string_view json) -> Outcome {
    constexpr std::string_view tool = "file.release";
    const auto r = kept_request(tool, json, {});
    if (!r) {
        return failure(tool, r.error());
    }
    if (permit.risk() != security::Risk::Quarantine || permit.request().where != r->second.from) {
        return failure(tool, core::Error{core::Code::Policy, "putting back needs its own permission"});
    }
    const auto a = read_args<args::Kept>(json);
    const std::filesystem::path dir{wide(a->dir)};
    const std::filesystem::path from{wide(r->second.from)};
    const auto kept = dir / from.filename();
    auto& os = os::current();
    if (auto allowed = os.deny_execute(kept.wstring(), false); !allowed) {
        return failure(tool, allowed.error());
    }
    // Its place taken since: beside it, " (restored)".
    auto target = from;
    std::error_code ec;
    if (std::filesystem::exists(target, ec)) {
        target = from.parent_path() / (from.stem().wstring() + L" (restored)" + from.extension().wstring());
    }
    if (auto moved = os.move(kept.wstring(), target.wstring(), false); !moved) {
        return failure(tool, moved.error());
    }
    (void)os.remove(dir.wstring());
    return said(tool, out::Released{.file = r->second.from, .back_to = utf8(target.wstring()),
                                     .note = target == from ? "Back where it was, able to run again."
                                                            : "Its place was taken: put beside it, named \xE2\x80\x9C(restored)\xE2\x80\x9D."},
                {Effect{.kind = EffectKind::Released, .from = target.wstring(), .to = dir.wstring(), .undoable = false, .before = {}}});
}

// quarantine.delete — out of the quarantine for good

auto quarantine_delete_request(std::string_view json, const std::filesystem::path& wd) -> core::Result<security::Request> {
    auto r = kept_request("quarantine.delete", json, wd);
    if (!r) {
        return std::unexpected{r.error()};
    }
    return std::move(r->first);
}

auto quarantine_delete_run(const security::Permit& permit, std::string_view json) -> Outcome {
    constexpr std::string_view tool = "quarantine.delete";
    const auto r = kept_request(tool, json, {});
    if (!r) {
        return failure(tool, r.error());
    }
    if (permit.risk() != security::Risk::Quarantine || permit.request().where != r->second.from) {
        return failure(tool, core::Error{core::Code::Policy, "deleting for good needs its own permission"});
    }
    const auto a = read_args<args::Kept>(json);
    const std::filesystem::path dir{wide(a->dir)};
    if (auto gone = os::current().remove(dir.wstring()); !gone) {
        return failure(tool, gone.error());
    }
    return said(tool, out::DeletedForGood{.file = r->second.from},
                {Effect{.kind = EffectKind::Deleted, .from = wide(r->second.from), .to = dir.wstring(), .undoable = false, .before = {}}});
}

// defender.scan — Microsoft Defender's own verdict on one file (asked: a threat it finds, it moves)

auto defender_scan_schema() -> std::string {
    auto s = glz::write_json_schema<args::Scan>();
    return s ? std::move(*s) : std::string{"{}"};
}

auto defender_scan_request(std::string_view json, const std::filesystem::path& wd) -> core::Result<security::Request> {
    const auto a = read_args<args::Scan>(json);
    if (!a) {
        return std::unexpected{a.error()};
    }
    const std::wstring path = wide(a->path);
    std::error_code ec;
    if (path.empty() || !std::filesystem::is_regular_file(path, ec)) {
        return core::make_error(core::Code::NotFound, "no file " + a->path);
    }
    const auto& ctx = context();
    const DefenderStatus d = ctx.check_sources && ctx.check_sources->defender ? ctx.check_sources->defender() : defender_status();
    if (!d.read || !d.present || !d.on) {
        return core::make_error(core::Code::InvalidArgument,
                                "Microsoft Defender is off or not there (" + (d.read ? std::string{"not running"} : d.why_not) +
                                    "): it cannot scan the file. Turn it on in Windows Security, or use another antivirus.");
    }
    return security::Request{.tool = "defender.scan", .risk = security::Risk::Quarantine, .paths = {path}, .working_dir = wd,
                             .entry = utf8(std::filesystem::path{path}.filename().wstring()), .where = a->path};
}

auto defender_scan_run(const security::Permit& permit, std::string_view json) -> Outcome {
    constexpr std::string_view tool = "defender.scan";
    const auto a = read_args<args::Scan>(json);
    if (!a) {
        return failure(tool, a.error());
    }
    if (permit.risk() != security::Risk::Quarantine || permit.paths().size() != 1) {
        return failure(tool, core::Error{core::Code::Policy, "a Defender scan needs its own permission"});
    }
    const auto scan = os::current().defender_scan(permit.paths().front().path);
    if (!scan) {
        return failure(tool, scan.error());
    }
    out::Scanned o{.file = a->path, .verdict = {}, .threat = {}, .note = {}};
    if (scan->threat) {
        // MpCmdRun says "Threat <name> identified." — the name, when it says it.
        if (const auto at = scan->output.find("Threat "); at != std::string::npos) {
            const auto from = at + 7;
            const auto to = scan->output.find_first_of(" \r\n", from);
            o.threat = scan->output.substr(from, to == std::string::npos ? std::string::npos : to - from);
        }
        o.verdict = "Defender found a threat" + (o.threat.empty() ? std::string{} : ": " + o.threat);
        o.note = "Defender handles the file itself (its own quarantine). See or restore it in Windows Security \xE2\x80\xBA Protection history.";
    } else if (scan->code == 0) {
        o.verdict = "Defender found no threat in it";
        o.note = "A clean scan is Defender's word, not a guarantee: a new threat may be unknown to it yet.";
    } else {
        return failure(tool, core::Error{core::Code::Api, std::format("Defender's scan ended with code {}: {}", scan->code,
                                                                     scan->output.substr(0, 400))});
    }
    return said(tool, o);
}

} // namespace tools::winsys
