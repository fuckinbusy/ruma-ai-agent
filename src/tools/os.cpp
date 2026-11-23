#include "tools/os.hpp"

#include "core/utf8.hpp"
#include "core/win.hpp"
#include "security/paths.hpp"
#include "tools/capture.hpp"
#include "tools/process_os.hpp"
#include "tools/recycle.hpp"
#include "tools/winsys/change.hpp"
#include "tools/winsys/registry.hpp"
#include "tools/winsys/startup.hpp"
#include "tools/winsys/wmi.hpp"

#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <format>
#include <mutex>
#include <regex>
#include <system_error>

namespace tools::os {
namespace {

auto utf8(std::wstring_view w) -> std::string {
    auto s = core::utf8::to_utf8(w);
    return s ? std::move(*s) : std::string{"?"};
}

std::mutex g_fence_mutex;
std::atomic<bool> g_fence_on{false};
std::wstring g_fence_resolved; // the root as the file system names it
std::wstring g_fence_logical;  // and as it was given, collapsed

// Loud and final: nothing after this runs, and no CRT dialog waits for a click (std::abort
// shows one in Debug).
[[noreturn]] void refuse(std::string_view operation, std::wstring_view path, std::string_view why) {
    std::fprintf(stderr, "[fence] %.*s refused: %s: %.*s (the fence root is %s)\n", static_cast<int>(operation.size()),
                 operation.data(), utf8(path).c_str(), static_cast<int>(why.size()), why.data(),
                 utf8(g_fence_resolved).c_str());
    std::fflush(stderr);
    OutputDebugStringA("[fence] an operation outside the fence root was refused; the process ends\n");
    TerminateProcess(GetCurrentProcess(), 3);
    for (;;) {
        Sleep(INFINITE);
    }
}

auto long_form(const std::wstring& path) -> std::wstring {
    return security::paths::long_form(path);
}

// A name next to `path` that nothing can have taken in advance: the write opens it with
// CREATE_NEW, so a link planted at a guessable name is never written through.
auto temporary_for(const std::wstring& path) -> std::wstring {
    static std::atomic<std::uint64_t> counter{0};
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const std::uint64_t id = static_cast<std::uint64_t>(now.QuadPart) * 1'000'003 + GetCurrentProcessId() +
                             counter.fetch_add(1, std::memory_order_relaxed);
    const auto slash = path.find_last_of(L'\\');
    return path.substr(0, slash + 1) + std::format(L".wa-{:016x}.tmp", id);
}

auto write_all(HANDLE h, std::string_view data) -> bool {
    while (!data.empty()) {
        DWORD n = 0;
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(data.size(), 1u << 30));
        if (!WriteFile(h, data.data(), chunk, &n, nullptr) || n == 0) {
            return false;
        }
        data.remove_prefix(n);
    }
    return FlushFileBuffers(h) != FALSE;
}

class Win32 final : public Backend {
public:
    auto make_dir(const std::wstring& path) -> core::Result<void> override {
        fence::check_path("make_dir", path);
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path{long_form(path)}, ec);
        if (ec) {
            return core::make_error(core::Code::Io, "creating " + utf8(path) + ": " + ec.message());
        }
        return {};
    }

    auto write_file(const std::wstring& path, std::string_view data, WriteMode mode) -> core::Result<void> override {
        fence::check_path("write_file", path);
        if (mode == WriteMode::Overwrite) {
            std::wstring tmp;
            core::win::FileHandle h;
            for (int tries = 0; tries < 4 && !h; ++tries) { // CREATE_NEW: a taken name is tried again
                tmp = temporary_for(path);
                fence::check_path("write_file", tmp);
                h.reset(CreateFileW(long_form(tmp).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr));
            }
            if (!h) {
                return core::os_error(GetLastError(), "writing " + utf8(path) + " (its temporary copy)");
            }
            // On the disk before it takes the file's place: ReplaceFileW has no write-through of its own
            // (REPLACEFILE_WRITE_THROUGH is documented as unsupported; final review E4).
            const bool written = write_all(h.get(), data) && FlushFileBuffers(h.get()) != FALSE;
            const DWORD write_error = GetLastError();
            h.reset(); // closed before the move
            if (!written) {
                DeleteFileW(long_form(tmp).c_str());
                return core::os_error(write_error, "writing " + utf8(path));
            }
            // Over an existing file: ReplaceFileW keeps its attributes, creation time and
            // permissions (phase 12: an edited file stays the file it was). A new one: a move.
            const bool existing = GetFileAttributesW(long_form(path).c_str()) != INVALID_FILE_ATTRIBUTES;
            const bool replaced = existing ? ReplaceFileW(long_form(path).c_str(), long_form(tmp).c_str(), nullptr,
                                                          REPLACEFILE_IGNORE_MERGE_ERRORS | REPLACEFILE_IGNORE_ACL_ERRORS, nullptr,
                                                          nullptr) != FALSE
                                           : MoveFileExW(long_form(tmp).c_str(), long_form(path).c_str(),
                                                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
            if (!replaced) {
                const DWORD e = GetLastError();
                DeleteFileW(long_form(tmp).c_str());
                return core::os_error(e, "replacing " + utf8(path));
            }
            return {};
        }
        const bool append = mode == WriteMode::Append;
        core::win::FileHandle h{CreateFileW(long_form(path).c_str(), append ? FILE_APPEND_DATA : GENERIC_WRITE, 0, nullptr,
                                            append ? OPEN_ALWAYS : CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (!h) {
            const DWORD e = GetLastError();
            if (e == ERROR_FILE_EXISTS) {
                return core::make_error(core::Code::AlreadyExists, utf8(path) + " already exists (use \"mode\":\"overwrite\")");
            }
            return core::os_error(e, "writing " + utf8(path));
        }
        if (!write_all(h.get(), data)) {
            return core::os_error(GetLastError(), "writing " + utf8(path));
        }
        return {};
    }

    auto move(const std::wstring& from, const std::wstring& to, bool replace) -> core::Result<void> override {
        fence::check_path("move", from);
        fence::check_path("move", to);
        const DWORD flags = MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0);
        if (!MoveFileExW(long_form(from).c_str(), long_form(to).c_str(), flags)) {
            const DWORD e = GetLastError();
            if (e == ERROR_ALREADY_EXISTS || e == ERROR_FILE_EXISTS) {
                return core::make_error(core::Code::AlreadyExists, utf8(to) + " already exists");
            }
            return core::os_error(e, "moving " + utf8(from) + " to " + utf8(to));
        }
        return {};
    }

    auto copy_file(const std::wstring& from, const std::wstring& to, bool replace) -> core::Result<void> override {
        fence::check_path("copy_file", to);
        BOOL cancel = FALSE;
        if (!CopyFileExW(long_form(from).c_str(), long_form(to).c_str(), nullptr, nullptr, &cancel,
                         replace ? 0 : COPY_FILE_FAIL_IF_EXISTS)) {
            const DWORD e = GetLastError();
            if (e == ERROR_FILE_EXISTS) {
                return core::make_error(core::Code::AlreadyExists, utf8(to) + " already exists");
            }
            return core::os_error(e, "copying " + utf8(from) + " to " + utf8(to));
        }
        return {};
    }

    auto trash(const std::wstring& path) -> core::Result<void> override {
        fence::check_path("trash", path);
        if (auto ok = recycle::usable(path); !ok) {
            return ok;
        }
        if (auto sent = recycle::send(path); !sent) {
            return sent;
        }
        if (GetFileAttributesW(long_form(path).c_str()) != INVALID_FILE_ATTRIBUTES) {
            return core::make_error(core::Code::Io, utf8(path) + " is still there");
        }
        if (!recycle::find(path)) {
            return core::make_error(core::Code::Io, utf8(path) + " is gone but not found in the Recycle Bin");
        }
        return {};
    }

    auto restore(const std::wstring& original) -> core::Result<void> override {
        fence::check_path("restore", original);
        const auto item = recycle::find(original);
        if (!item) {
            return core::make_error(core::Code::NotFound, utf8(original) + " is not in the Recycle Bin");
        }
        fence::check_recycled("restore", item->data);
        fence::check_recycled("restore", item->info);
        if (GetFileAttributesW(long_form(original).c_str()) != INVALID_FILE_ATTRIBUTES) {
            return core::make_error(core::Code::AlreadyExists, utf8(original) + " exists: it is not put back over it");
        }
        if (!MoveFileExW(long_form(item->data).c_str(), long_form(original).c_str(), MOVEFILE_WRITE_THROUGH)) {
            return core::os_error(GetLastError(), "putting " + utf8(original) + " back");
        }
        DeleteFileW(long_form(item->info).c_str()); // its note in the bin: the item is no longer there
        return {};
    }

    auto remove(const std::wstring& path) -> core::Result<void> override {
        fence::check_path("remove", path);
        const DWORD attrs = GetFileAttributesW(long_form(path).c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            return core::make_error(core::Code::NotFound, utf8(path) + " does not exist");
        }
        if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0 && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
            if (auto inner = remove_tree(path); !inner) {
                return inner;
            }
            return {};
        }
        return remove_one(path, attrs);
    }

    auto set_clipboard(std::string_view text) -> core::Result<void> override {
        fence::check_desktop("set_clipboard");
        auto wide = core::utf8::to_wide(text);
        if (!wide) {
            return core::make_error(core::Code::InvalidArgument, "the text is not UTF-8");
        }
        bool open = false;
        for (int i = 0; i < 10 && !(open = OpenClipboard(nullptr) != FALSE); ++i) {
            Sleep(20);
        }
        if (!open) {
            return core::make_error(core::Code::Io, "the clipboard is busy");
        }
        EmptyClipboard();
        const std::size_t bytes = (wide->size() + 1) * sizeof(wchar_t);
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes); // mem-ok: the clipboard takes GlobalAlloc memory and owns it
        bool done = false;
        if (mem != nullptr) {
            if (void* p = GlobalLock(mem)) {
                std::memcpy(p, wide->c_str(), bytes);
                GlobalUnlock(mem);
                done = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
            }
            if (!done) {
                GlobalFree(mem); // mem-ok: not taken by the clipboard
            }
        }
        CloseClipboard();
        if (!done) {
            return core::make_error(core::Code::Io, "the clipboard did not take the text");
        }
        return {};
    }

    auto open(const std::wstring& target) -> core::Result<void> override {
        fence::check_desktop("open");
        SHELLEXECUTEINFOW info{sizeof(info)};
        info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        info.lpVerb = L"open";
        info.lpFile = target.c_str();
        info.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&info)) {
            return core::os_error(GetLastError(), "opening " + utf8(target));
        }
        return {};
    }

    auto open_terminal(const std::wstring& folder) -> core::Result<void> override {
        fence::check_desktop("open_terminal");
        // Windows Terminal if there is one, else Windows PowerShell; never elevated, its own window.
        for (const wchar_t* program : {L"wt.exe", L"powershell.exe"}) {
            const std::wstring args = std::wstring_view{program} == L"wt.exe" ? L"-d \"" + folder + L"\"" : L"-NoExit";
            SHELLEXECUTEINFOW info{sizeof(info)};
            info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
            info.lpVerb = L"open";
            info.lpFile = program;
            info.lpParameters = args.c_str();
            info.lpDirectory = folder.c_str();
            info.nShow = SW_SHOWNORMAL;
            if (ShellExecuteExW(&info)) {
                return {};
            }
        }
        return core::os_error(GetLastError(), "opening a terminal in " + utf8(folder));
    }

    auto run_shell(const std::wstring& command, const std::wstring& working_dir, int timeout_s, int cpu_s,
                   const ShellIo& io) -> core::Result<ShellResult> override {
        fence::check_command("run_shell", command);
        if (!working_dir.empty()) {
            fence::check_path("run_shell", working_dir);
        }
        return detail::run_shell(command, working_dir, timeout_s, cpu_s, io);
    }

    auto kill(std::uint32_t pid) -> core::Result<void> override {
        fence::check_process("kill", pid);
        return detail::kill(pid);
    }

    auto read_clipboard() -> core::Result<ClipboardContent> override {
        fence::check_desktop("read_clipboard");
        return detail::read_clipboard();
    }

    auto capture(const CaptureSpec& spec) -> core::Result<Captured> override {
        fence::check_desktop("capture");
        const auto& hooks = capture_hooks();
        return with_exclusion([&] {
            void* active = spec.hwnd != nullptr ? spec.hwnd : hooks.active ? hooks.active() : nullptr;
            return detail::capture(spec, active, hooks.agent ? hooks.agent() : std::array<long, 4>{});
        });
    }

    auto set_startup_approved(const winsys::RegistryView& reg, const winsys::StartupEntry& entry, bool enabled)
        -> core::Result<void> override {
        fence::check_registry("set_startup_approved", reg);
        return winsys::change::set_startup_approved(reg, entry, enabled);
    }

    auto set_task_enabled(std::wstring_view path, bool enabled) -> core::Result<void> override {
        fence::check_system("set_task_enabled");
        return winsys::change::set_task_enabled(path, enabled);
    }

    auto take_wmi_subscription(const winsys::WmiSubscription& s) -> core::Result<std::string> override {
        fence::check_system("take_wmi_subscription");
        return winsys::change::take_wmi_subscription(s);
    }

    auto put_wmi_subscription(std::string_view mof) -> core::Result<void> override {
        fence::check_system("put_wmi_subscription");
        return winsys::change::put_wmi_subscription(mof);
    }

    auto launch_uninstaller(std::wstring_view program, std::wstring_view arguments) -> core::Result<std::uint32_t> override {
        fence::check_system("launch_uninstaller");
        return winsys::change::launch_uninstaller(program, arguments);
    }

    auto deny_execute(const std::wstring& path, bool deny) -> core::Result<void> override {
        fence::check_path("deny_execute", path);
        return winsys::change::deny_execute(path, deny);
    }

    auto defender_scan(const std::wstring& path) -> core::Result<DefenderScan> override {
        fence::check_system("defender_scan");
        return winsys::change::defender_scan(path);
    }

private:
    static auto remove_one(const std::wstring& path, DWORD attrs) -> core::Result<void> {
        if ((attrs & FILE_ATTRIBUTE_READONLY) != 0) {
            SetFileAttributesW(long_form(path).c_str(), attrs & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY));
        }
        const bool dir = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0; // a folder link: removed as the link
        if (!(dir ? RemoveDirectoryW(long_form(path).c_str()) : DeleteFileW(long_form(path).c_str()))) {
            return core::os_error(GetLastError(), "deleting " + utf8(path));
        }
        return {};
    }

    static auto remove_tree(const std::wstring& dir) -> core::Result<void> {
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileExW(long_form(dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                    nullptr, 0);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                const std::wstring_view name{fd.cFileName};
                if (name == L"." || name == L"..") {
                    continue;
                }
                const std::wstring child = dir + L"\\" + fd.cFileName;
                const bool real_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                                      (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
                auto done = real_dir ? remove_tree(child) : remove_one(child, fd.dwFileAttributes);
                if (!done) {
                    FindClose(h);
                    return done;
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        return remove_one(dir, GetFileAttributesW(long_form(dir).c_str()));
    }
};

auto mode_name(WriteMode m) -> const char* {
    switch (m) {
    case WriteMode::CreateOnly: return "create_only";
    case WriteMode::Overwrite: return "overwrite";
    case WriteMode::Append: return "append";
    }
    return "?";
}

Backend* g_current = nullptr;

// The only commands a fenced run may start. Built when the fence goes on (before a test run's
// leak epoch): std::wregex keeps its state on the heap for the process.
auto harmless_commands() -> const std::vector<std::wregex>& {
    static const std::vector<std::wregex> forms{
        std::wregex{LR"(Start-Sleep -Seconds \d{1,4})"},
        std::wregex{LR"(Write-Output '[A-Za-z0-9 .,!?-]{0,200}')"},
        std::wregex{LR"(Write-Output \('x' \* \d{1,7}\))"},
        std::wregex{LR"(\[Console\]::Out\.Write\('[A-Za-z0-9 .,!?-]{0,200}'\))"},
        // prints control characters, as a coloured PowerShell does (the JSON escape's test)
        std::wregex{LR"(\[Console\]::Out\.Write\(\[string\]\[char\]27 \+ '\[32mgreen' \+ \[char\]27 \+ '\[0m' \+ \[char\]7\))"},
        std::wregex{LR"(exit \d{1,3})"},
        std::wregex{LR"(& pwsh -NoProfile -Command 'Start-Sleep -Seconds \d{1,4}')"},
        std::wregex{LR"(while\(\$true\)\{\})"}, // only burns the processor (the CPU limit's test)
        // prints numbers slowly (the streamed output's test)
        std::wregex{LR"(1\.\.\d{1,3} \| % \{ Write-Output \$_; Start-Sleep -m \d{1,4} \})"},
    };
    return forms;
}

} // namespace

auto win32() -> Backend& {
    static Win32 backend;
    return backend;
}

namespace {

auto hooks_store() -> CaptureHooks& {
    static CaptureHooks hooks;
    return hooks;
}

auto target_name(CaptureTarget t) -> std::string_view {
    switch (t) {
    case CaptureTarget::Screen: return "screen";
    case CaptureTarget::Window: return "window";
    case CaptureTarget::Active: return "active";
    case CaptureTarget::Monitor: return "monitor";
    case CaptureTarget::All: return "all";
    }
    return "?";
}

} // namespace

void set_capture_hooks(CaptureHooks hooks) {
    hooks_store() = std::move(hooks);
}

auto capture_hooks() -> const CaptureHooks& {
    return hooks_store();
}

auto with_exclusion(const std::function<core::Result<Captured>()>& shoot) -> core::Result<Captured> {
    struct Hidden {
        Hidden() {
            if (hooks_store().exclude) {
                hooks_store().exclude(true);
            }
        }
        ~Hidden() {
            if (hooks_store().exclude) {
                hooks_store().exclude(false);
            }
        }
        Hidden(const Hidden&) = delete;
        auto operator=(const Hidden&) -> Hidden& = delete;
    } hidden;
    return shoot();
}

auto Recorder::capture(const CaptureSpec& spec) -> core::Result<Captured> {
    return with_exclusion([&]() -> core::Result<Captured> {
        std::string call = "capture " + std::string{target_name(spec.target)};
        captured_window_ = spec.hwnd;
        if (spec.target == CaptureTarget::Monitor) {
            call += " " + std::to_string(spec.monitor);
        } else if (spec.target == CaptureTarget::Window) {
            call += " " + utf8(spec.window);
        }
        calls_.push_back(std::move(call));
        if (!capture_error_.empty()) {
            return core::make_error(core::Code::Io, capture_error_);
        }
        Captured c{.picture = {.width = 64, .height = 32, .bgra = std::vector<std::uint8_t>(64u * 32 * 4, 0x90)},
                   .title = "screen", .source = "Monitor 1", .monitors = 2, .windows = {"Probe window", "Notepad"},
                   .hole_x = 16, .hole_y = 0, .hole_w = 32, .hole_h = 4};
        switch (spec.target) {
        case CaptureTarget::Screen: break;
        case CaptureTarget::Monitor: c.source = "Monitor " + std::to_string(spec.monitor); break;
        case CaptureTarget::All: c.source = "All monitors"; break;
        case CaptureTarget::Window: c.title = c.source = utf8(spec.window); break;
        case CaptureTarget::Active: c.title = c.source = "Probe window"; break;
        }
        return c;
    });
}

auto Recorder::read_clipboard() -> core::Result<ClipboardContent> {
    calls_.push_back("read_clipboard");
    return clipboard_;
}

auto FolderBin::read_clipboard() -> core::Result<ClipboardContent> {
    return core::make_error(core::Code::Policy, "not with a folder bin");
}

auto FolderBin::capture(const CaptureSpec&) -> core::Result<Captured> {
    return core::make_error(core::Code::Policy, "not with a folder bin");
}

auto Recorder::make_dir(const std::wstring& path) -> core::Result<void> {
    calls_.push_back("make_dir " + utf8(path));
    return {};
}

auto Recorder::write_file(const std::wstring& path, std::string_view data, WriteMode mode) -> core::Result<void> {
    calls_.push_back(std::string{"write_file "} + mode_name(mode) + " " + std::to_string(data.size()) + " " + utf8(path));
    if (!write_error_.empty()) {
        return core::make_error(core::Code::Io, write_error_);
    }
    return {};
}

auto Recorder::move(const std::wstring& from, const std::wstring& to, bool replace) -> core::Result<void> {
    calls_.push_back(std::string{"move "} + (replace ? "replace " : "") + utf8(from) + " -> " + utf8(to));
    return {};
}

auto Recorder::copy_file(const std::wstring& from, const std::wstring& to, bool replace) -> core::Result<void> {
    calls_.push_back(std::string{"copy_file "} + (replace ? "replace " : "") + utf8(from) + " -> " + utf8(to));
    return {};
}

auto Recorder::trash(const std::wstring& path) -> core::Result<void> {
    calls_.push_back("trash " + utf8(path));
    return {};
}

auto Recorder::restore(const std::wstring& original) -> core::Result<void> {
    calls_.push_back("restore " + utf8(original));
    return {};
}

auto Recorder::remove(const std::wstring& path) -> core::Result<void> {
    calls_.push_back("remove " + utf8(path));
    return {};
}

auto Recorder::set_clipboard(std::string_view text) -> core::Result<void> {
    calls_.push_back("set_clipboard " + std::to_string(text.size()) + " bytes");
    return {};
}

auto Recorder::open(const std::wstring& target) -> core::Result<void> {
    calls_.push_back("open " + utf8(target));
    return {};
}

auto Recorder::open_terminal(const std::wstring& folder) -> core::Result<void> {
    calls_.push_back("terminal " + utf8(folder));
    return {};
}

auto Recorder::run_shell(const std::wstring& command, const std::wstring&, int timeout_s, int cpu_s, const ShellIo&)
    -> core::Result<ShellResult> {
    calls_.push_back(std::format("run_shell timeout={} cpu={} ", timeout_s, cpu_s) + utf8(command));
    return ShellResult{};
}

auto Recorder::kill(std::uint32_t pid) -> core::Result<void> {
    calls_.push_back("kill " + std::to_string(pid));
    return {};
}

auto Recorder::set_startup_approved(const winsys::RegistryView&, const winsys::StartupEntry& entry, bool enabled) -> core::Result<void> {
    calls_.push_back("startup_approved " + utf8(entry.key_or_folder) + " " + utf8(entry.name) + (enabled ? " on" : " off"));
    return {};
}

auto Recorder::set_task_enabled(std::wstring_view path, bool enabled) -> core::Result<void> {
    calls_.push_back("task_enabled " + utf8(path) + (enabled ? " on" : " off"));
    if (!task_error_.empty()) {
        return core::make_error(core::Code::AccessDenied, task_error_);
    }
    return {};
}

auto Recorder::take_wmi_subscription(const winsys::WmiSubscription& s) -> core::Result<std::string> {
    calls_.push_back("take_wmi " + s.filter + " " + s.consumer);
    return std::format("// recorded: the filter {} bound to the {} {}\n", s.filter, s.consumer_class, s.consumer);
}

auto Recorder::put_wmi_subscription(std::string_view mof) -> core::Result<void> {
    calls_.push_back("put_wmi " + std::string{mof});
    return {};
}

auto Recorder::launch_uninstaller(std::wstring_view program, std::wstring_view arguments) -> core::Result<std::uint32_t> {
    calls_.push_back("launch_uninstaller " + utf8(program) + " |" + (arguments.empty() ? std::string{} : " " + utf8(arguments)));
    if (on_uninstall_) {
        on_uninstall_();
    }
    return uninstall_pid_;
}

auto FolderBin::launch_uninstaller(std::wstring_view program, std::wstring_view arguments) -> core::Result<std::uint32_t> {
    return win32().launch_uninstaller(program, arguments);
}

auto Recorder::deny_execute(const std::wstring& path, bool deny) -> core::Result<void> {
    calls_.push_back("deny_execute " + utf8(path) + (deny ? " on" : " off"));
    return {};
}

auto FolderBin::deny_execute(const std::wstring& path, bool deny) -> core::Result<void> {
    return win32().deny_execute(path, deny);
}

auto Recorder::defender_scan(const std::wstring& path) -> core::Result<DefenderScan> {
    calls_.push_back("defender_scan " + utf8(path));
    return scan_;
}

auto FolderBin::defender_scan(const std::wstring& path) -> core::Result<DefenderScan> {
    return win32().defender_scan(path);
}

auto FolderBin::set_startup_approved(const winsys::RegistryView& reg, const winsys::StartupEntry& entry, bool enabled)
    -> core::Result<void> {
    return win32().set_startup_approved(reg, entry, enabled);
}

auto FolderBin::set_task_enabled(std::wstring_view path, bool enabled) -> core::Result<void> {
    return win32().set_task_enabled(path, enabled);
}

auto FolderBin::take_wmi_subscription(const winsys::WmiSubscription& s) -> core::Result<std::string> {
    return win32().take_wmi_subscription(s);
}

auto FolderBin::put_wmi_subscription(std::string_view mof) -> core::Result<void> {
    return win32().put_wmi_subscription(mof);
}

auto FolderBin::make_dir(const std::wstring& path) -> core::Result<void> {
    return win32().make_dir(path);
}

auto FolderBin::write_file(const std::wstring& path, std::string_view data, WriteMode mode) -> core::Result<void> {
    return win32().write_file(path, data, mode);
}

auto FolderBin::move(const std::wstring& from, const std::wstring& to, bool replace) -> core::Result<void> {
    return win32().move(from, to, replace);
}

auto FolderBin::copy_file(const std::wstring& from, const std::wstring& to, bool replace) -> core::Result<void> {
    return win32().copy_file(from, to, replace);
}

auto FolderBin::trash(const std::wstring& path) -> core::Result<void> {
    if (auto made = win32().make_dir(dir_.wstring()); !made) {
        return made;
    }
    const auto lies = (dir_ / (std::to_wstring(++count_) + L"-" + std::filesystem::path{path}.filename().wstring())).wstring();
    if (auto moved = win32().move(path, lies, false); !moved) {
        return moved;
    }
    binned_.emplace_back(path, lies);
    return {};
}

auto FolderBin::restore(const std::wstring& original) -> core::Result<void> {
    for (auto it = binned_.rbegin(); it != binned_.rend(); ++it) { // the newest trashed from there
        if (it->first == original) {
            if (auto moved = win32().move(it->second, original, false); !moved) {
                return moved;
            }
            binned_.erase(std::next(it).base());
            return {};
        }
    }
    return core::make_error(core::Code::NotFound, "nothing trashed from " + utf8(original));
}

auto FolderBin::remove(const std::wstring& path) -> core::Result<void> {
    return core::make_error(core::Code::Policy, "not with a folder bin: " + utf8(path));
}

auto FolderBin::set_clipboard(std::string_view) -> core::Result<void> {
    return core::make_error(core::Code::Policy, "not with a folder bin");
}

auto FolderBin::open(const std::wstring&) -> core::Result<void> {
    return core::make_error(core::Code::Policy, "not with a folder bin");
}

auto FolderBin::open_terminal(const std::wstring&) -> core::Result<void> {
    return core::make_error(core::Code::Policy, "not with a folder bin");
}

auto FolderBin::run_shell(const std::wstring&, const std::wstring&, int, int, const ShellIo&) -> core::Result<ShellResult> {
    return core::make_error(core::Code::Policy, "not with a folder bin");
}

auto FolderBin::kill(std::uint32_t) -> core::Result<void> {
    return core::make_error(core::Code::Policy, "not with a folder bin");
}

auto FolderBin::holds(const std::filesystem::path& original) const -> bool {
    return std::ranges::any_of(binned_, [&](const auto& b) { return b.first == original.wstring(); });
}

auto current() -> Backend& {
    return g_current != nullptr ? *g_current : win32();
}

void use(Backend* backend) {
    g_current = backend;
}

namespace fence {

void enable(const std::filesystem::path& root) {
    auto c = security::paths::canonical(root.wstring(), root);
    std::scoped_lock lock{g_fence_mutex};
    if (!c || !c->exists || c->device) {
        refuse("fence::enable", root.wstring(), "the root must be an existing folder");
    }
    if (g_fence_on.load() && !security::paths::is_within(c->path, g_fence_resolved)) {
        refuse("fence::enable", root.wstring(), "the fence is already on with another root");
    }
    if (!g_fence_on.load()) {
        g_fence_resolved = c->path;
        g_fence_logical = c->logical;
        (void)harmless_commands();
        g_fence_on.store(true);
    }
}

auto enabled() -> bool {
    return g_fence_on.load();
}

auto root() -> std::optional<std::wstring> {
    if (!g_fence_on.load()) {
        return std::nullopt;
    }
    return g_fence_resolved;
}

void check_path(std::string_view operation, std::wstring_view path) {
    if (!g_fence_on.load()) {
        return;
    }
    const auto c = security::paths::canonical(path, g_fence_resolved);
    if (!c) {
        refuse(operation, path, "not a path the fence can place (" + c.error().message + ")");
    }
    if (c->device) {
        refuse(operation, path, "a device, not a file");
    }
    // Both: what the name says and what the file system resolves it to (a link inside the root
    // pointing out, or a link outside pointing in, is outside).
    const bool resolved_in = security::paths::is_within(c->path, g_fence_resolved);
    const bool named_in = security::paths::is_within(c->logical, g_fence_logical) ||
                          security::paths::is_within(c->logical, g_fence_resolved);
    if (!resolved_in || !named_in) {
        refuse(operation, path, "outside the fence root");
    }
}

void check_command(std::string_view operation, std::wstring_view command) {
    if (!g_fence_on.load()) {
        return;
    }
    for (const auto& form : harmless_commands()) {
        if (std::regex_match(command.begin(), command.end(), form)) {
            return;
        }
    }
    refuse(operation, command, "not one of the fence's harmless commands");
}

void check_process(std::string_view operation, std::uint32_t pid) {
    if (!g_fence_on.load()) {
        return;
    }
    core::win::FileHandle snap{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
    PROCESSENTRY32W e{sizeof(e)};
    bool ours = false;
    for (BOOL ok = snap ? Process32FirstW(snap.get(), &e) : FALSE; ok; ok = Process32NextW(snap.get(), &e)) {
        if (e.th32ProcessID == pid) {
            ours = e.th32ParentProcessID == GetCurrentProcessId();
            break;
        }
    }
    if (!ours) {
        refuse(operation, std::to_wstring(pid), "not a process this process started");
    }
}

void check_desktop(std::string_view operation) {
    if (g_fence_on.load()) {
        refuse(operation, L"", "the clipboard and the desktop are off limits behind the fence");
    }
}

void check_registry(std::string_view operation, const winsys::RegistryView& reg) {
    if (g_fence_on.load() && !reg.is_test()) {
        refuse(operation, L"", "the real registry is off limits behind the fence (a test's branch only)");
    }
}

void check_system(std::string_view operation) {
    if (g_fence_on.load()) {
        refuse(operation, L"", "the Task Scheduler and WMI are off limits behind the fence");
    }
}

void check_recycled(std::string_view operation, std::wstring_view path) {
    if (!g_fence_on.load()) {
        return;
    }
    const auto bin = recycle::folder_for(path);
    if (!bin || !security::paths::is_within(path, *bin) || path.find(L"\\..") != std::wstring_view::npos) {
        refuse(operation, path, "not in this user's Recycle Bin");
    }
}

} // namespace fence

} // namespace tools::os
