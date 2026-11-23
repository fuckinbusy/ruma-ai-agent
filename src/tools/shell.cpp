#include "tools/shell.hpp"

#include "core/json.hpp"
#include "core/utf8.hpp"
#include "core/win.hpp"
#include "tools/os.hpp"
#include "tools/shell_jobs.hpp"
#include "tools/winsys/signature.hpp"

#include <glaze/glaze.hpp>

#include <windows.h>
#include <tlhelp32.h>

#include <array>
#include <filesystem>
#include <format>
#include <optional>

namespace tools::shell::args {

struct Run {
    std::string command;
    std::optional<int> timeout_s;
    std::optional<bool> background;
};
struct Kill {
    std::uint32_t pid = 0;
};

} // namespace tools::shell::args

namespace tools::shell::out {

struct Run {
    int code = 0;
    std::string stdout_text; // "stdout" in the JSON
    std::string stderr_text; // "stderr"
    double duration_ms = 0;
    bool timed_out = false;
    bool cpu_limited = false;
    bool stopped = false;
    std::uint32_t pid = 0;
};
struct Killed {
    std::uint32_t pid = 0;
    std::string name;
};

} // namespace tools::shell::out

template <>
struct glz::meta<tools::shell::out::Run> {
    using T = tools::shell::out::Run;
    static constexpr auto value = glz::object("code", &T::code, "stdout", &T::stdout_text, "stderr", &T::stderr_text,
                                              "duration_ms", &T::duration_ms, "timed_out", &T::timed_out,
                                              "cpu_limited", &T::cpu_limited, "stopped", &T::stopped, "pid", &T::pid);
};
template <>
struct glz::json_schema<tools::shell::args::Run> {
    schema command{.description = "A PowerShell command. The user sees it in full and decides."};
    schema timeout_s{.description = "Seconds before it is ended with everything it started (1-600, default 60)."};
    schema background{.description = "Run it in the background (a build, tests, a server): you get its job number at once; "
                                      "read it with shell.job, end it with shell.stop. At most 3 at once, up to 2 hours. "
                                      "Default false."};
};
template <>
struct glz::json_schema<tools::shell::args::Kill> {
    schema pid{.description = "The process id (from process.list)."};
};

namespace tools::shell {
namespace {

using security::Risk;

auto utf8(std::wstring_view w) -> std::string {
    auto s = core::utf8::to_utf8(w);
    return s ? std::move(*s) : std::string{"?"};
}

template <class T>
auto parse(std::string_view json) -> core::Result<T> {
    T value{};
    if (const auto ec = glz::read<glz::opts{.error_on_unknown_keys = true, .error_on_missing_keys = true}>(value, json)) {
        return core::make_error(core::Code::InvalidArgument, "arguments: " + glz::format_error(ec, json));
    }
    return value;
}

// Invalid UTF-8 (a program writing in another code page) as U+FFFD: the result stays JSON.
auto clean(std::string_view s) -> std::string {
    if (core::utf8::is_valid(s)) {
        return std::string{s};
    }
    std::string out;
    for (std::size_t i = 0; i < s.size();) {
        const auto d = core::utf8::decode(s, i);
        if (d.valid) {
            out.append(s.substr(i, d.length));
        } else {
            out += "\xEF\xBF\xBD";
        }
        i += d.length;
    }
    return out;
}

// Cuts at a UTF-8 boundary.
void cut(std::string& s, std::size_t n) {
    if (s.size() <= n) {
        return;
    }
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) {
        --n;
    }
    s.resize(n);
}

struct ProcessInfo {
    std::string name;
    bool system = false;
    std::uint64_t started = 0; // creation time (FILETIME); 0 when it cannot be read
    std::wstring path;         // its file (phase 13a § 5.4); empty when it cannot be read
};

auto creation_time(HANDLE process) -> std::uint64_t {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) {
        return 0;
    }
    return (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}

// The image name, and whether it runs as SYSTEM, a service or in session 0. What cannot be
// looked into counts as system.
auto describe(std::uint32_t pid) -> std::optional<ProcessInfo> {
    core::win::FileHandle snap{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
    PROCESSENTRY32W e{sizeof(e)};
    std::optional<ProcessInfo> info;
    for (BOOL ok = snap ? Process32FirstW(snap.get(), &e) : FALSE; ok; ok = Process32NextW(snap.get(), &e)) {
        if (e.th32ProcessID == pid) {
            info = ProcessInfo{utf8(e.szExeFile)};
            break;
        }
    }
    if (!info) {
        return std::nullopt;
    }
    DWORD session = 0;
    if (!ProcessIdToSessionId(pid, &session) || session == 0) {
        info->system = true;
        return info;
    }
    core::win::KernelHandle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
    if (process) {
        info->started = creation_time(process.get());
        std::array<wchar_t, 1024> image{};
        DWORD n = static_cast<DWORD>(image.size());
        if (QueryFullProcessImageNameW(process.get(), 0, image.data(), &n)) {
            info->path.assign(image.data(), n);
        }
    }
    HANDLE token = nullptr;
    if (!process || !OpenProcessToken(process.get(), TOKEN_QUERY, &token)) {
        info->system = true;
        return info;
    }
    core::win::KernelHandle token_guard{token};
    alignas(TOKEN_USER) unsigned char buf[256]{};
    DWORD size = 0;
    if (!GetTokenInformation(token, TokenUser, buf, sizeof(buf), &size)) {
        info->system = true;
        return info;
    }
    const PSID sid = reinterpret_cast<TOKEN_USER*>(buf)->User.Sid;
    info->system = IsWellKnownSid(sid, WinLocalSystemSid) || IsWellKnownSid(sid, WinLocalServiceSid) ||
                   IsWellKnownSid(sid, WinNetworkServiceSid);
    return info;
}

} // namespace

// shell.run

auto run_schema() -> std::string {
    auto s = glz::write_json_schema<args::Run>();
    return s ? std::move(*s) : std::string{"{}"};
}

auto run_request(std::string_view json, const std::filesystem::path& wd) -> core::Result<security::Request> {
    auto a = parse<args::Run>(json);
    if (!a) {
        return std::unexpected{a.error()};
    }
    if (a->command.empty() || a->command.find('\0') != std::string::npos) {
        return core::make_error(core::Code::InvalidArgument, "arguments: \"command\" is empty");
    }
    return security::Request{.tool = "shell.run", .risk = Risk::Execute, .working_dir = wd, .command = a->command,
                             .background = a->background.value_or(false)};
}

auto run_run(const security::Permit& permit, std::string_view json) -> Outcome {
    const auto a = parse<args::Run>(json);
    if (!a) {
        return failure("shell.run", a.error());
    }
    const auto& decided = permit.request();
    if (permit.risk() != Risk::Execute || decided.opens || decided.command.empty()) {
        return failure("shell.run", core::Error{core::Code::Policy, "a command needs its own permission"});
    }
    auto command = core::utf8::to_wide(decided.command); // what the user saw, not the arguments again
    if (!command) {
        return failure("shell.run", core::Error{core::Code::InvalidArgument, "the command is not UTF-8"});
    }
    if (decided.background) { // phase 12 § 8: started, its number at once; its output read with shell.job
        if (control().jobs == nullptr) {
            return failure("shell.run", core::Error{core::Code::InvalidArgument, "background commands are not available here"});
        }
        const auto job = control().jobs->start(control().task, *command, decided.working_dir.wstring());
        if (!job) {
            return failure("shell.run", job.error());
        }
        Outcome o{0, wrap("shell.run", L"",
                          std::format(R"({{"job":{},"running":true,"note":"read it with shell.job, end it with shell.stop"}})", *job),
                          false)};
        o.effects.push_back({.kind = EffectKind::Ran, .from = *command, .undoable = false});
        return o;
    }
    const int timeout = std::clamp(a->timeout_s.value_or(60), 1, 600);
    // Processor time for everything it starts: no more than its wall time, so a command that
    // starts many busy processes ends early rather than taking every core for the whole timeout.
    // Its output goes to the terminal as it comes; the stop button ends it.
    auto r = os::current().run_shell(*command, decided.working_dir.wstring(), timeout, timeout,
                                     {.output = control().output, .stop = control().stop});
    if (!r) {
        return failure("shell.run", r.error());
    }
    out::Run data{.code = r->code, .stdout_text = clean(r->out), .stderr_text = clean(r->err),
                  .duration_ms = r->duration_ms, .timed_out = r->timed_out, .cpu_limited = r->cpu_limited,
                  .stopped = r->stopped, .pid = r->pid};
    bool truncated = r->out_cut || r->err_cut;
    for (;;) {
        auto body = core::json::write(data);
        auto wrapped = wrap("shell.run", L"", body ? *body : std::string{"null"}, truncated);
        if (wrapped.size() <= kMaxOutput || (data.stdout_text.empty() && data.stderr_text.empty())) {
            // What a command changed is not known: it is in the log, and it cannot be undone.
            Outcome o{0, std::move(wrapped)};
            o.effects.push_back({.kind = EffectKind::Ran, .from = *command, .undoable = false});
            return o;
        }
        truncated = true; // escaping made it bigger: cut both by the excess
        const double keep = static_cast<double>(kMaxOutput) / static_cast<double>(wrapped.size()) * 0.95;
        cut(data.stdout_text, static_cast<std::size_t>(static_cast<double>(data.stdout_text.size()) * keep));
        cut(data.stderr_text, static_cast<std::size_t>(static_cast<double>(data.stderr_text.size()) * keep));
    }
}

// process.kill

auto kill_schema() -> std::string {
    auto s = glz::write_json_schema<args::Kill>();
    return s ? std::move(*s) : std::string{"{}"};
}

auto kill_request(std::string_view json, const std::filesystem::path& wd) -> core::Result<security::Request> {
    auto a = parse<args::Kill>(json);
    if (!a) {
        return std::unexpected{a.error()};
    }
    security::Request r{.tool = "process.kill", .risk = Risk::ProcessKill, .working_dir = wd, .pid = a->pid};
    if (a->pid == 0 || a->pid == 4) {
        r.process_name = a->pid == 0 ? "Idle" : "System";
        r.process_system = true;
        return r;
    }
    const auto info = describe(a->pid);
    if (!info) {
        return core::make_error(core::Code::NotFound, "no process with id " + std::to_string(a->pid));
    }
    r.process_name = info->name;
    r.process_system = info->system;
    r.process_started = info->started;
    r.process_path = info->path; // where its file is and who signed it: Windows' own or a look-alike (phase 13a)
    r.microsoft_signed = !info->path.empty() && winsys::signature(std::filesystem::path{info->path}).microsoft;
    return r;
}

auto kill_run(const security::Permit& permit, std::string_view) -> Outcome {
    const auto& decided = permit.request();
    if (permit.risk() != Risk::ProcessKill) {
        return failure("process.kill", core::Error{core::Code::Policy, "ending a process needs its own permission"});
    }
    // The id may have been reused since the decision: the very process (same program, same start),
    // or nothing. A start time that cannot be read is no match.
    const auto now = describe(decided.pid);
    // Its file too, at the moment it ends (phase 13a § 5.4): the same file, still not Microsoft's when asked as not.
    if (!now || now->name != decided.process_name || (now->system && !decided.process_system) || now->started == 0 ||
        now->started != decided.process_started || now->path != decided.process_path ||
        (!decided.microsoft_signed && !now->path.empty() && winsys::signature(std::filesystem::path{now->path}).microsoft)) {
        return failure("process.kill", core::Error{core::Code::Policy, "process " + std::to_string(decided.pid) +
                                                                           " is not the one the permission was given for"});
    }
    if (auto done = os::current().kill(decided.pid); !done) {
        return failure("process.kill", done.error());
    }
    auto body = core::json::write(out::Killed{decided.pid, decided.process_name});
    Outcome o{0, wrap("process.kill", L"", body ? *body : std::string{"null"}, false)};
    o.effects.push_back({.kind = EffectKind::Killed, .from = core::utf8::to_wide(decided.process_name).value_or(L"?"),
                         .undoable = false});
    return o;
}

} // namespace tools::shell
