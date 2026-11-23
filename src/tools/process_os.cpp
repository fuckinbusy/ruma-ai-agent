#include "tools/process_os.hpp"

#include "core/utf8.hpp"
#include "core/win.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <thread>
#include <vector>

namespace tools::os::detail {
namespace {

auto utf8(std::wstring_view w) -> std::string {
    auto s = core::utf8::to_utf8(w);
    return s ? std::move(*s) : std::string{"?"};
}

auto find_shell() -> std::wstring {
    wchar_t buf[MAX_PATH]{};
    if (SearchPathW(nullptr, L"pwsh.exe", nullptr, MAX_PATH, buf, nullptr) != 0) {
        return buf;
    }
    wchar_t sys[MAX_PATH]{};
    GetSystemDirectoryW(sys, MAX_PATH);
    return std::wstring{sys} + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
}

// One argument as CommandLineToArgvW reads it back.
auto quote(std::wstring_view s) -> std::wstring {
    std::wstring out = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t c : s) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
        } else {
            out.append(backslashes, L'\\');
        }
        backslashes = 0;
        out += c;
    }
    out.append(backslashes * 2, L'\\');
    out += L'"';
    return out;
}

// Reads a pipe to its end; keeps the first `max` bytes and hands every piece to `output` as it
// comes (the terminal shows all of it, the model the kept part).
void drain(HANDLE pipe, std::string& out, bool& cut, std::size_t max, const std::function<void(std::string_view)>& output) {
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(pipe, buf, sizeof(buf), &n, nullptr) && n > 0) {
        if (output) {
            output(std::string_view{buf, n});
        }
        const std::size_t room = max > out.size() ? max - out.size() : 0;
        out.append(buf, std::min<std::size_t>(room, n));
        if (n > room) {
            cut = true;
        }
    }
}

struct Pipe {
    core::win::KernelHandle read, write;
};

auto make_pipe() -> std::optional<Pipe> {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE r = nullptr;
    HANDLE w = nullptr;
    if (!CreatePipe(&r, &w, &sa, 0)) {
        return std::nullopt;
    }
    SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0); // only the write end goes to the child
    return Pipe{core::win::KernelHandle{r}, core::win::KernelHandle{w}};
}

} // namespace

auto run_shell(const std::wstring& command, const std::wstring& working_dir, int timeout_s, int cpu_s, const ShellIo& io)
    -> core::Result<ShellResult> {
    const auto started = std::chrono::steady_clock::now();
    core::win::KernelHandle job{CreateJobObjectW(nullptr, nullptr)};
    if (!job) {
        return core::os_error(GetLastError(), "creating the job for the command");
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION |
                                              JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_JOB_MEMORY |
                                              JOB_OBJECT_LIMIT_JOB_TIME;
    limits.BasicLimitInformation.ActiveProcessLimit = 32;
    // User time of all its processes together, in 100 ns units; at the limit Windows ends them all.
    const LONGLONG cpu_limit = static_cast<LONGLONG>(std::clamp(cpu_s, 1, 3600)) * 10'000'000;
    limits.BasicLimitInformation.PerJobUserTimeLimit.QuadPart = cpu_limit;
    limits.JobMemoryLimit = 2ull << 30;
    if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        return core::os_error(GetLastError(), "limiting the job for the command");
    }

    auto out = make_pipe();
    auto err = make_pipe();
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    core::win::FileHandle nul{CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr)};
    if (!out || !err || !nul) {
        return core::make_error(core::Code::Io, "creating the command's pipes");
    }

    // Only these three handles are inherited, nothing else of the agent's.
    HANDLE inherit[] = {nul.get(), out->write.get(), err->write.get()};
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<unsigned char> attr_buf(size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &size) ||
        !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr, nullptr)) {
        return core::os_error(GetLastError(), "preparing the command");
    }
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul.get();
    si.StartupInfo.hStdOutput = out->write.get();
    si.StartupInfo.hStdError = err->write.get();
    si.lpAttributeList = attrs;

    // UTF-8 out of PowerShell whatever the console's code page.
    const std::wstring script = L"[Console]::OutputEncoding = [System.Text.Encoding]::UTF8; " + command;
    std::wstring cmdline = quote(find_shell()) + L" -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command " + quote(script);
    PROCESS_INFORMATION pi{};
    const BOOL created = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                                        CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT |
                                            CREATE_UNICODE_ENVIRONMENT,
                                        nullptr, working_dir.empty() ? nullptr : working_dir.c_str(), &si.StartupInfo, &pi);
    const DWORD create_error = GetLastError();
    DeleteProcThreadAttributeList(attrs);
    if (!created) {
        return core::os_error(create_error, "starting the shell");
    }
    core::win::KernelHandle process{pi.hProcess};
    core::win::KernelHandle thread{pi.hThread};
    if (!AssignProcessToJobObject(job.get(), process.get())) {
        TerminateProcess(process.get(), 1);
        return core::os_error(GetLastError(), "putting the shell in its job");
    }
    ResumeThread(thread.get());
    out->write.reset(); // the child has its copies: the pipes end when the job's processes do
    err->write.reset();
    nul.reset();

    ShellResult r;
    r.pid = pi.dwProcessId;
    std::thread out_reader{[&] { drain(out->read.get(), r.out, r.out_cut, kMaxStream, io.output); }};
    std::thread err_reader{[&] { drain(err->read.get(), r.err, r.err_cut, kMaxStream, io.output); }};
    // In slices, so the stop button is seen within 100 ms.
    const auto deadline = started + std::chrono::seconds{std::clamp(timeout_s, 1, 3600)};
    for (;;) {
        if (WaitForSingleObject(process.get(), 100) != WAIT_TIMEOUT) {
            break;
        }
        if (io.stop.stop_requested()) {
            r.stopped = true;
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            r.timed_out = true;
            break;
        }
    }
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION used{};
    if (!r.timed_out && !r.stopped &&
        QueryInformationJobObject(job.get(), JobObjectBasicAccountingInformation, &used, sizeof(used), nullptr) &&
        used.TotalUserTime.QuadPart >= cpu_limit) {
        r.cpu_limited = true;
    }
    // Whatever is still running — the shell after a timeout, or what it left behind — ends now.
    TerminateJobObject(job.get(), 1);
    WaitForSingleObject(process.get(), 5000);
    out_reader.join();
    err_reader.join();
    DWORD code = 0;
    GetExitCodeProcess(process.get(), &code);
    r.code = static_cast<int>(code);
    r.duration_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    return r;
}

auto kill(std::uint32_t pid) -> core::Result<void> {
    core::win::KernelHandle h{OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
    if (!h) {
        return core::os_error(GetLastError(), "opening process " + std::to_string(pid));
    }
    struct Find {
        DWORD pid;
        int posted = 0;
    } find{pid};
    EnumWindows(
        [](HWND w, LPARAM p) -> BOOL {
            auto* f = reinterpret_cast<Find*>(p);
            DWORD owner = 0;
            GetWindowThreadProcessId(w, &owner);
            if (owner == f->pid && IsWindowVisible(w)) {
                PostMessageW(w, WM_CLOSE, 0, 0);
                ++f->posted;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&find));
    if (find.posted > 0 && WaitForSingleObject(h.get(), 2000) == WAIT_OBJECT_0) {
        return {};
    }
    if (!TerminateProcess(h.get(), 1)) {
        return core::os_error(GetLastError(), "ending process " + std::to_string(pid));
    }
    WaitForSingleObject(h.get(), 5000);
    return {};
}

} // namespace tools::os::detail
