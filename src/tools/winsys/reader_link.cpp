#include "tools/winsys/reader_link.hpp"

#include "core/win.hpp"
#include "tools/os.hpp"

#include <glaze/glaze.hpp>

#include <windows.h>
#include <bcrypt.h>
#include <aclapi.h>
#include <sddl.h>
#include <shellapi.h>

#include <array>
#include <chrono>
#include <format>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace tools::winsys::reader {
namespace {

struct Handle {
    HANDLE h = nullptr;
    explicit Handle(HANDLE x = nullptr) : h(x) {}
    Handle(const Handle&) = delete;
    auto operator=(const Handle&) -> Handle& = delete;
    ~Handle() {
        if (h != nullptr && h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
        }
    }
};

auto random_hex() -> std::wstring {
    std::array<unsigned char, 16> bytes{};
    BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    std::wstring out;
    for (const auto b : bytes) {
        out += std::format(L"{:02x}", b);
    }
    return out;
}

// The user's own SID as text ("S-1-5-21-…"): the pipe's ACL names it.
auto user_sid() -> std::wstring {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return {};
    }
    const Handle t{token};
    DWORD n = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &n);
    std::vector<std::byte> buf(n);
    wchar_t* text = nullptr;
    if (n == 0 || !GetTokenInformation(token, TokenUser, buf.data(), n, &n) ||
        !ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &text)) {
        return {};
    }
    const core::win::LocalPtr<wchar_t*> owned{text};
    return std::wstring{text};
}

// What the helper's exit code says, when it ended without handing anything over.
auto exit_words(DWORD code) -> std::string {
    switch (code) {
    case kExitUsage: return "the helper did not accept how it was started";
    case kExitNoAgent: return "the helper found no agent's pipe";
    case kExitNotAgent: return "the helper refused the pipe: not served by agent.exe beside it";
    case kExitTimeLimit: return "the helper reached its 5-minute limit";
    default: return std::format("the helper ended with code {}", code);
    }
}

// One overlapped call on `pipe` finished: the bytes it moved, or its error — WAIT_TIMEOUT when the
// deadline came first, ERROR_PROCESS_ABORTED when the process `also` ended first.
struct Io {
    DWORD moved = 0;
    DWORD error = ERROR_SUCCESS;
};

auto finish(HANDLE pipe, OVERLAPPED& ov, std::chrono::steady_clock::time_point deadline, HANDLE also = nullptr) -> Io {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    const std::array<HANDLE, 2> waits{ov.hEvent, also};
    const DWORD w = WaitForMultipleObjects(also != nullptr ? 2 : 1, waits.data(), FALSE, left > 0 ? static_cast<DWORD>(left) : 0);
    if (w != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &ov);
        DWORD ignored = 0;
        GetOverlappedResult(pipe, &ov, &ignored, TRUE);
        return {.moved = 0, .error = w == WAIT_OBJECT_0 + 1 ? static_cast<DWORD>(ERROR_PROCESS_ABORTED) : static_cast<DWORD>(WAIT_TIMEOUT)};
    }
    Io io;
    if (!GetOverlappedResult(pipe, &ov, &io.moved, FALSE)) {
        io.error = GetLastError();
    }
    return io;
}

} // namespace

auto protected_location(const std::filesystem::path& exe) -> core::Result<void> {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(exe, ec)) {
        return core::make_error(core::Code::NotFound, "agent-reader.exe is not beside the agent");
    }
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &token)) {
        return core::os_error(GetLastError(), "reading this user's token");
    }
    const Handle own{token};
    HANDLE copy = nullptr;
    if (!DuplicateToken(token, SecurityImpersonation, &copy)) {
        return core::os_error(GetLastError(), "reading this user's token");
    }
    const Handle as_user{copy};
    // What this user may do to `path`, by its access list (the owner's implicit rights counted).
    const auto may = [&](const std::filesystem::path& path, DWORD changing) {
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                  nullptr, nullptr, nullptr, nullptr, &sd) != ERROR_SUCCESS) {
            return true; // not readable: not known to be safe
        }
        const core::win::LocalPtr<PSECURITY_DESCRIPTOR> owned{sd};
        GENERIC_MAPPING map{FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS};
        PRIVILEGE_SET privileges{};
        DWORD size = sizeof(privileges);
        DWORD granted = 0;
        BOOL ok = FALSE;
        const BOOL checked = AccessCheck(sd, as_user.h, MAXIMUM_ALLOWED, &map, &privileges, &size, &granted, &ok);
        return !checked || (granted & changing) != 0;
    };
    constexpr DWORD kFile = FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE | WRITE_DAC | WRITE_OWNER;
    constexpr DWORD kFolder = FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER;
    const auto folder = exe.parent_path();
    // The file, its folder (a file put beside it, it swapped), the folder above (the folder renamed away).
    if (may(exe, kFile) || may(folder, kFolder) || (folder.has_parent_path() && may(folder.parent_path(), FILE_DELETE_CHILD | WRITE_DAC | WRITE_OWNER))) {
        return core::make_error(core::Code::Policy,
                                "agent-reader.exe lies where your programs can change it: Check deeper works in the installed agent "
                                "(Program Files)");
    }
    return {};
}

auto launch_elevated(const std::wstring& exe, const std::wstring& args) -> core::Result<void*> {
    // Never from a test: the UAC prompt is the user's alone.
    os::fence::check_system("reader_elevated");
    // Only a helper no program of the user could have put there (final review C2).
    if (auto safe = protected_location(exe); !safe) {
        return std::unexpected{safe.error()};
    }
    SHELLEXECUTEINFOW sei{.cbSize = sizeof(SHELLEXECUTEINFOW)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || sei.hProcess == nullptr) {
        const DWORD e = GetLastError();
        if (e == ERROR_CANCELLED) {
            return core::make_error(core::Code::Cancelled, "the Windows prompt was declined: nothing more was read");
        }
        return core::os_error(e, "starting agent-reader.exe");
    }
    return static_cast<void*>(sei.hProcess);
}

auto launch_as_invoker(const std::wstring& exe, const std::wstring& args) -> core::Result<void*> {
    // This process's environment, with Windows' layer that sets a manifest's administrator aside.
    std::wstring block;
    if (wchar_t* env = GetEnvironmentStringsW(); env != nullptr) {
        for (const wchar_t* p = env; *p != L'\0'; p += std::wcslen(p) + 1) {
            if (_wcsnicmp(p, L"__COMPAT_LAYER=", 15) != 0) {
                block.append(p).push_back(L'\0');
            }
        }
        FreeEnvironmentStringsW(env);
    }
    block.append(L"__COMPAT_LAYER=RunAsInvoker").push_back(L'\0');
    block.push_back(L'\0');
    std::wstring line = L"\"" + exe + L"\" " + args;
    STARTUPINFOW si{.cb = sizeof(STARTUPINFOW)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, block.data(),
                        nullptr, &si, &pi)) {
        return core::os_error(GetLastError(), "starting agent-reader.exe as the user");
    }
    CloseHandle(pi.hThread);
    return static_cast<void*>(pi.hProcess);
}

auto run(const std::filesystem::path& exe, const Launch& launch, std::uint32_t limit_ms) -> core::Result<Data> {
    using clock = std::chrono::steady_clock;
    limit_ms = std::min(limit_ms, kLimitMs);
    const std::wstring sid = user_sid();
    if (sid.empty()) {
        return core::os_error(GetLastError(), "reading the user's SID");
    }
    // Only this user and the administrators may open it; one instance; never from the network.
    const std::wstring sddl = L"D:P(A;;GA;;;" + sid + L")(A;;GA;;;BA)";
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) {
        return core::os_error(GetLastError(), "the pipe's access list");
    }
    core::win::LocalPtr<PSECURITY_DESCRIPTOR> owned_sd{sd};
    SECURITY_ATTRIBUTES sa{.nLength = sizeof(SECURITY_ATTRIBUTES), .lpSecurityDescriptor = sd, .bInheritHandle = FALSE};
    const std::wstring name = std::wstring{kPipePrefix} + random_hex();
    const Handle pipe{CreateNamedPipeW(name.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 0, 64 * 1024, 0, &sa)};
    const DWORD made = GetLastError();
    owned_sd.reset(); // the pipe has its rights: the list is not needed while the helper runs
    if (pipe.h == INVALID_HANDLE_VALUE) {
        return core::os_error(made, "making the helper's pipe");
    }
    auto launched = launch(exe.wstring(), std::format(L"--pipe {} --limit-ms {}", name, limit_ms));
    if (!launched) {
        return std::unexpected{launched.error()};
    }
    const Handle process{static_cast<HANDLE>(*launched)};
    const auto deadline = clock::now() + std::chrono::milliseconds{limit_ms} + std::chrono::seconds{5};
    const auto ended = [&]() -> std::string {
        DWORD code = STILL_ACTIVE;
        if (WaitForSingleObject(process.h, 2000) == WAIT_OBJECT_0) {
            GetExitCodeProcess(process.h, &code);
        }
        return code == STILL_ACTIVE ? std::string{"the helper did not answer in time"} : exit_words(code);
    };

    const Handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    OVERLAPPED ov{};
    ov.hEvent = event.h;
    if (!ConnectNamedPipe(pipe.h, &ov)) {
        const DWORD e = GetLastError();
        if (e == ERROR_IO_PENDING) {
            if (finish(pipe.h, ov, deadline, process.h).error != ERROR_SUCCESS) {
                return core::make_error(core::Code::Api, ended());
            }
        } else if (e != ERROR_PIPE_CONNECTED) {
            return core::os_error(e, "waiting for the helper");
        }
    }
    // The one who connected is the helper this run started, by its process id.
    ULONG client = 0;
    if (!GetNamedPipeClientProcessId(pipe.h, &client) || client != GetProcessId(process.h)) {
        return core::make_error(core::Code::Policy, "another program opened the helper's pipe: nothing was read from it");
    }
    std::string text;
    std::array<char, 64 * 1024> buf{};
    for (;;) {
        ResetEvent(event.h);
        ov = OVERLAPPED{};
        ov.hEvent = event.h;
        DWORD got = 0;
        Io io;
        if (ReadFile(pipe.h, buf.data(), static_cast<DWORD>(buf.size()), &got, &ov)) {
            io.moved = got;
        } else if (const DWORD e = GetLastError(); e == ERROR_IO_PENDING) {
            io = finish(pipe.h, ov, deadline);
        } else {
            io.error = e;
        }
        if (io.error == ERROR_BROKEN_PIPE) {
            break; // the helper wrote all and closed its end
        }
        if (io.error == WAIT_TIMEOUT) {
            return core::make_error(core::Code::Timeout, ended());
        }
        if (io.error != ERROR_SUCCESS) {
            return core::os_error(io.error, "reading the helper's answer");
        }
        text.append(buf.data(), io.moved);
        if (text.size() > 64 * 1024 * 1024) {
            return core::make_error(core::Code::Api, "the helper's answer is too long");
        }
    }
    if (text.empty()) {
        return core::make_error(core::Code::Api, ended());
    }
    Data d;
    if (const auto ec = glz::read<glz::opts{.error_on_unknown_keys = false}>(d, text)) {
        return core::make_error(core::Code::Parse, "the helper's answer is not what the agent reads: " + glz::format_error(ec, text));
    }
    WaitForSingleObject(process.h, 5000);
    return d;
}

} // namespace tools::winsys::reader
