#pragma once

// tools::os — the only place where the agent's tools change the machine (docs/plans/phase-4.md
// § The machine's safety during development). Everything that writes, creates, moves, deletes, starts
// or ends something goes through a Backend:
//
//   - win32(): the real calls, behind the fence;
//   - Recorder: notes what it was asked and does nothing (tests whose target is a system object,
//     tool-runner --dry-run).
//
// The fence (fence::enable) is for tests, --smoke and tool-runner --fence: once on, any
// operation on an object outside its root ends the process at once (exit code 3) instead of
// running. It does not depend on the policy being right: it is the second wall.

#include "core/error.hpp"
#include "tools/image.hpp"

#include <cstdint>
#include <filesystem>
#include <array>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace tools::winsys {
class RegistryView;
struct StartupEntry;
struct WmiSubscription;
} // namespace tools::winsys

namespace tools::os {

inline constexpr std::size_t kMaxStream = 30 * 1024; // stdout and stderr each, kept for the model

struct ShellResult {
    int code = 0;
    std::string out, err;    // the first kMaxStream bytes of each
    bool out_cut = false, err_cut = false;
    bool timed_out = false;  // ended by the timeout, with everything it started
    bool cpu_limited = false; // ended because it used up its processor time
    bool stopped = false;     // ended by the stop button
    double duration_ms = 0;
    std::uint32_t pid = 0;   // the shell's process id
};

// screen.capture (phase 9): the monitor under the mouse, a window by a part of its title, or the
// window that was in front before the agent opened.
enum class CaptureTarget : std::uint8_t { Screen, Window, Active, Monitor, All };
// What to take (phase 10, screen 19: also a monitor by its number, or all of them).
struct CaptureSpec {
    CaptureTarget target = CaptureTarget::Screen;
    std::wstring window; // a part of its title (Window)
    int monitor = 0;     // from 1, left to right (Monitor)
    void* hwnd = nullptr; // Active: this window (the one window.look checked); none: the hook's
};
struct Captured {
    Picture picture;
    std::string title; // "screen", or the window's title
    // Phase 10 (screen 19): where it came from ("Monitor 1", "All monitors", the window's title),
    // the monitors there are, the open windows' titles (to pick one), and where the agent's own
    // window is on it, in its pixels (w 0: not on it).
    std::string source;
    int monitors = 1;
    std::vector<std::string> windows;
    int hole_x = 0, hole_y = 0, hole_w = 0, hole_h = 0;
};

// What the user copied: text, files, or a picture (the first there is, in that order).
struct ClipboardContent {
    std::optional<std::string> text; // at most 20 000 characters
    std::optional<std::vector<std::string>> files;
    std::optional<Picture> picture;
};

// A command's output as it comes (from the pipe readers' threads) and the stop button: a stop
// ends the command with everything it started.
struct ShellIo {
    std::function<void(std::string_view)> output;
    std::stop_token stop;
};

enum class WriteMode : std::uint8_t {
    CreateOnly, // fails when the file exists
    Overwrite,  // atomic: written next to it (".<name>.wa-tmp"), then moved over it
    Append,
};

// What Defender's scan of a file said (phase 13a § 5.4): MpCmdRun's exit code (2: a threat), and its words.
struct DefenderScan {
    int code = 0;
    bool threat = false;
    std::string output;
};

// Paths are final paths (canonical, no \\?\ prefix); the backend adds the long form itself.
class Backend {
public:
    virtual ~Backend() = default;
    // A folder and its missing parents.
    virtual auto make_dir(const std::wstring& path) -> core::Result<void> = 0;
    virtual auto write_file(const std::wstring& path, std::string_view data, WriteMode mode) -> core::Result<void> = 0;
    // A file or a folder; fails when `to` exists unless `replace`.
    virtual auto move(const std::wstring& from, const std::wstring& to, bool replace) -> core::Result<void> = 0;
    // One file.
    virtual auto copy_file(const std::wstring& from, const std::wstring& to, bool replace) -> core::Result<void> = 0;
    // To the Recycle Bin, never further: refused on a drive that would delete at once; checked to
    // be in the bin afterwards.
    virtual auto trash(const std::wstring& path) -> core::Result<void> = 0;
    // The newest item trashed from `original`, back where it was; never over something there.
    virtual auto restore(const std::wstring& original) -> core::Result<void> = 0;
    // For good (fs.delete_permanent, advanced only): a folder with what is in it; a link inside
    // is removed as the link, never followed.
    virtual auto remove(const std::wstring& path) -> core::Result<void> = 0;
    virtual auto set_clipboard(std::string_view text) -> core::Result<void> = 0;
    // A URL in the browser, a file in its program, a folder in Explorer.
    virtual auto open(const std::wstring& target) -> core::Result<void> = 0;
    // A PowerShell window of the user's own in `folder` ("Take over" in the terminal): the user's
    // action, the agent only opens it.
    virtual auto open_terminal(const std::wstring& folder) -> core::Result<void> = 0;
    // PowerShell 7 (pwsh.exe) if there is one, else Windows PowerShell, with -NoProfile
    // -NonInteractive -ExecutionPolicy Bypass -Command; in a Job Object (killed with everything it
    // started on the timeout or when the job closes; memory, process and processor-time limits,
    // `cpu_s` seconds of user time for everything it starts); no window, no input, never elevated.
    virtual auto run_shell(const std::wstring& command, const std::wstring& working_dir, int timeout_s, int cpu_s,
                           const ShellIo& io)
        -> core::Result<ShellResult> = 0;
    // Asks its windows to close, then ends it after 2 s.
    virtual auto kill(std::uint32_t pid) -> core::Result<void> = 0;
    // A picture of the screen or a window, with the agent's own windows hidden from it
    // (with_exclusion). Never shown to anyone by itself: screen.capture asks first.
    virtual auto capture(const CaptureSpec& spec) -> core::Result<Captured> = 0;
    // clipboard.read and Ctrl+V (phase 9: pictures too: PNG, then a DIB).
    virtual auto read_clipboard() -> core::Result<ClipboardContent> = 0;

    // Phase 13a (§ 3): a program off or on at startup, the way Task Manager does it — the entry's
    // value in Explorer\StartupApproved\Run, Run32 or StartupFolder (02…: on, 03… and when: off);
    // the Run value and the shortcut stay. The only registry write there is.
    virtual auto set_startup_approved(const winsys::RegistryView& reg, const winsys::StartupEntry& entry, bool enabled)
        -> core::Result<void> = 0;
    // A scheduled task switched off or on (IRegisteredTask::put_Enabled), never deleted.
    virtual auto set_task_enabled(std::wstring_view path, bool enabled) -> core::Result<void> = 0;
    // A WMI subscription's filter, consumer and binding taken off root\subscription; what they were,
    // as MOF, to be put back by put_wmi_subscription.
    virtual auto take_wmi_subscription(const winsys::WmiSubscription& s) -> core::Result<std::string> = 0;
    virtual auto put_wmi_subscription(std::string_view mof) -> core::Result<void> = 0;
    // Phase 13a (§ 4): a program's own uninstaller in its usual window (ShellExecuteEx: its own UAC
    // when it asks for one), never a quiet one. The started process's id (0: none to wait for).
    virtual auto launch_uninstaller(std::wstring_view program, std::wstring_view arguments) -> core::Result<std::uint32_t> = 0;
    // A file in the agent's quarantine (§ 5.4): a deny-execute rule for everyone on it, or taken off.
    virtual auto deny_execute(const std::wstring& path, bool deny) -> core::Result<void> = 0;
    // Microsoft Defender's own scan of one file (MpCmdRun -Scan -ScanType 3 -File): it decides and
    // moves a threat into its own quarantine itself.
    virtual auto defender_scan(const std::wstring& path) -> core::Result<DefenderScan> = 0;
};

// What the agent's window gives the capture: hiding its own windows from it (on, then off), and
// the window that was in front before it opened (an HWND; null: the foreground one).
struct CaptureHooks {
    std::function<void(bool hide)> exclude;
    std::function<void*()> active;
    // Where the agent's window is on the screen (left, top, right, bottom; all 0: nowhere) — it is
    // not in the picture, the panel outlines its place (phase 10).
    std::function<std::array<long, 4>()> agent;
    // Whether a password field had the focus in `active`'s window when the sheet opened (UI
    // Automation's IsPassword: a browser's field too). Its focus is the sheet's after that.
    std::function<bool()> password_at_open;
};
// Not thread-safe: set it before tools run.
void set_capture_hooks(CaptureHooks hooks);
auto capture_hooks() -> const CaptureHooks&;
// `shoot` with the agent's windows hidden from capture: exclude(true) before, exclude(false)
// after — also when it fails.
auto with_exclusion(const std::function<core::Result<Captured>()>& shoot) -> core::Result<Captured>;

// The real one.
auto win32() -> Backend&;

// Notes each call ("make_dir C:\x"), changes nothing, reports success.
class Recorder final : public Backend {
public:
    auto make_dir(const std::wstring& path) -> core::Result<void> override;
    auto write_file(const std::wstring& path, std::string_view data, WriteMode mode) -> core::Result<void> override;
    void fail_write_file(std::string why) { write_error_ = std::move(why); } // then write_file fails with it
    auto move(const std::wstring& from, const std::wstring& to, bool replace) -> core::Result<void> override;
    auto copy_file(const std::wstring& from, const std::wstring& to, bool replace) -> core::Result<void> override;
    auto trash(const std::wstring& path) -> core::Result<void> override;
    auto restore(const std::wstring& original) -> core::Result<void> override;
    auto remove(const std::wstring& path) -> core::Result<void> override;
    auto set_clipboard(std::string_view text) -> core::Result<void> override;
    auto open(const std::wstring& target) -> core::Result<void> override;
    auto open_terminal(const std::wstring& folder) -> core::Result<void> override;
    auto run_shell(const std::wstring& command, const std::wstring& working_dir, int timeout_s, int cpu_s, const ShellIo& io)
        -> core::Result<ShellResult> override;
    auto kill(std::uint32_t pid) -> core::Result<void> override;
    // A grey 64 x 32 picture through with_exclusion ("capture monitor 2", "capture window Notepad"):
    // two monitors, two windows, the agent at x 16..48 of its top; or, after fail_capture(why), that
    // error (empty: succeed again).
    auto capture(const CaptureSpec& spec) -> core::Result<Captured> override;
    void fail_capture(std::string why) { capture_error_ = std::move(why); }
    [[nodiscard]] auto captured_window() const -> void* { return captured_window_; } // the last Active spec's hwnd
    // What read_clipboard gives (nothing by default).
    auto read_clipboard() -> core::Result<ClipboardContent> override;
    void hold_clipboard(ClipboardContent content) { clipboard_ = std::move(content); }
    // "startup_approved <where> <name> off", "task_enabled <path> on", "take_wmi <filter> <consumer>"
    // (its MOF: a line naming both), "put_wmi <mof>".
    auto set_startup_approved(const winsys::RegistryView& reg, const winsys::StartupEntry& entry, bool enabled)
        -> core::Result<void> override;
    auto set_task_enabled(std::wstring_view path, bool enabled) -> core::Result<void> override;
    void fail_task_enabled(std::string why) { task_error_ = std::move(why); } // then set_task_enabled fails with it
    auto take_wmi_subscription(const winsys::WmiSubscription& s) -> core::Result<std::string> override;
    auto put_wmi_subscription(std::string_view mof) -> core::Result<void> override;
    // "launch_uninstaller <program> | <arguments>"; then `on_launch` (a test's stand-in for what the
    // uninstaller does), and the process id given (0: none).
    auto launch_uninstaller(std::wstring_view program, std::wstring_view arguments) -> core::Result<std::uint32_t> override;
    auto deny_execute(const std::wstring& path, bool deny) -> core::Result<void> override; // "deny_execute <path> on|off"
    // "defender_scan <path>"; what answer_scan set (a clean file by default).
    auto defender_scan(const std::wstring& path) -> core::Result<DefenderScan> override;
    void answer_scan(DefenderScan s) { scan_ = std::move(s); }
    void answer_uninstall(std::uint32_t pid, std::function<void()> on_launch) {
        uninstall_pid_ = pid;
        on_uninstall_ = std::move(on_launch);
    }
    [[nodiscard]] auto calls() const -> const std::vector<std::string>& { return calls_; }

private:
    std::vector<std::string> calls_;
    std::string capture_error_;
    void* captured_window_ = nullptr;
    ClipboardContent clipboard_;
    std::uint32_t uninstall_pid_ = 0;
    std::function<void()> on_uninstall_;
    DefenderScan scan_;
    std::string task_error_;
    std::string write_error_;
};

// The real calls, except the Recycle Bin: a trashed object is moved into `dir` and restored from
// there (tests and the --m4 smoke run, inside their fence: the user's bin is never touched).
class FolderBin final : public Backend {
public:
    explicit FolderBin(std::filesystem::path dir) : dir_(std::move(dir)) {}
    auto make_dir(const std::wstring& path) -> core::Result<void> override;
    auto write_file(const std::wstring& path, std::string_view data, WriteMode mode) -> core::Result<void> override;
    auto move(const std::wstring& from, const std::wstring& to, bool replace) -> core::Result<void> override;
    auto copy_file(const std::wstring& from, const std::wstring& to, bool replace) -> core::Result<void> override;
    auto trash(const std::wstring& path) -> core::Result<void> override;
    auto restore(const std::wstring& original) -> core::Result<void> override;
    auto remove(const std::wstring& path) -> core::Result<void> override;
    auto set_clipboard(std::string_view text) -> core::Result<void> override;
    auto open(const std::wstring& target) -> core::Result<void> override;
    auto open_terminal(const std::wstring& folder) -> core::Result<void> override;
    auto run_shell(const std::wstring& command, const std::wstring& working_dir, int timeout_s, int cpu_s, const ShellIo& io)
        -> core::Result<ShellResult> override;
    auto kill(std::uint32_t pid) -> core::Result<void> override;
    auto capture(const CaptureSpec& spec) -> core::Result<Captured> override;
    auto read_clipboard() -> core::Result<ClipboardContent> override;
    auto set_startup_approved(const winsys::RegistryView& reg, const winsys::StartupEntry& entry, bool enabled)
        -> core::Result<void> override;
    auto set_task_enabled(std::wstring_view path, bool enabled) -> core::Result<void> override;
    auto take_wmi_subscription(const winsys::WmiSubscription& s) -> core::Result<std::string> override;
    auto put_wmi_subscription(std::string_view mof) -> core::Result<void> override;
    auto launch_uninstaller(std::wstring_view program, std::wstring_view arguments) -> core::Result<std::uint32_t> override;
    auto deny_execute(const std::wstring& path, bool deny) -> core::Result<void> override;
    auto defender_scan(const std::wstring& path) -> core::Result<DefenderScan> override;
    // `original` was trashed and is still in the folder.
    [[nodiscard]] auto holds(const std::filesystem::path& original) const -> bool;

private:
    std::filesystem::path dir_;
    std::vector<std::pair<std::wstring, std::wstring>> binned_; // original -> where it lies, oldest first
    int count_ = 0;
};

// What tools use: win32() unless another is put in.
auto current() -> Backend&;
// nullptr: win32() again. Not thread-safe: set it before tools run.
void use(Backend* backend);

namespace fence {

// Process-wide, once: a second call with another root ends the process. `root` must exist.
void enable(const std::filesystem::path& root);
[[nodiscard]] auto enabled() -> bool;
// The root as the file system names it.
[[nodiscard]] auto root() -> std::optional<std::wstring>;
// With the fence on: returns only when `path` (resolved by the file system, junctions and all)
// is inside the root; otherwise prints why and ends the process with exit code 3.
void check_path(std::string_view operation, std::wstring_view path);
// With the fence on: returns only when `path` is inside this user's Recycle Bin (restore takes
// its items from there, outside any root); otherwise ends the process with exit code 3.
void check_recycled(std::string_view operation, std::wstring_view path);
// With the fence on, what has no path to place — the clipboard, opening a program or a page on
// the desktop — is refused outright (tests use a Recorder for them).
void check_desktop(std::string_view operation);
// With the fence on: a command runs only if it is exactly one of the fence's harmless forms
// (Start-Sleep -Seconds N, Write-Output '…', Write-Output ('x' * N), [Console]::Out.Write('…'),
// exit N, and a nested pwsh that only sleeps); anything else ends the process.
void check_command(std::string_view operation, std::wstring_view command);
// With the fence on: only a process this process started may be ended.
void check_process(std::string_view operation, std::uint32_t pid);
// With the fence on: a registry write only in the tests' own branch (a test's RegistryView).
void check_registry(std::string_view operation, const winsys::RegistryView& reg);
// With the fence on, Windows' own settings — the Task Scheduler, WMI — are refused outright (tests
// use a Recorder for them).
void check_system(std::string_view operation);

} // namespace fence

} // namespace tools::os
