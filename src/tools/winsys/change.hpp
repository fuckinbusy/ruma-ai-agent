#pragma once

// tools::winsys::change — the real calls behind tools::os::win32()'s startup methods (phase 13a § 3):
// a StartupApproved value, a scheduled task's Enabled, a WMI subscription taken off and put back.
// Nothing else calls them: tools go through tools::os::current(), behind the fence.

#include "core/error.hpp"
#include "tools/os.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace tools::winsys {

class RegistryView;
struct StartupEntry;
struct WmiSubscription;

namespace change {

// 12 bytes, as Task Manager writes them: 02 00 00 00 and zeros (on), or 03 00 00 00 and the time it
// was switched off (FILETIME, UTC).
auto set_startup_approved(const RegistryView& reg, const StartupEntry& entry, bool enabled) -> core::Result<void>;
auto set_task_enabled(std::wstring_view path, bool enabled) -> core::Result<void>;
auto take_wmi_subscription(const WmiSubscription& s) -> core::Result<std::string>;
auto put_wmi_subscription(std::string_view mof) -> core::Result<void>;
// ShellExecuteExW, shown normally: the uninstaller's own window and, when it asks, its own UAC.
auto launch_uninstaller(std::wstring_view program, std::wstring_view arguments) -> core::Result<std::uint32_t>;
// An explicit deny of FILE_EXECUTE for Everyone on the file's DACL (deny), or every such deny taken off.
auto deny_execute(const std::wstring& path, bool deny) -> core::Result<void>;
// MpCmdRun.exe -Scan -ScanType 3 -File <path>, without a window, at most 10 minutes: its exit code and words.
auto defender_scan(const std::wstring& path) -> core::Result<os::DefenderScan>;

// `line` without a window, its output (64 KB at most) read on a thread of its own; ended at
// `limit_ms` (timed_out). Only the output pipe is handed to it.
struct RunOutput {
    int code = 0;
    std::string output;
    bool timed_out = false;
};
auto run_with_output(std::wstring line, std::uint32_t limit_ms) -> core::Result<RunOutput>;

// Where StartupApproved keeps an entry of this place ("Run", "Run32", "StartupFolder"); empty: a
// place it does not keep (only those three are switched off this way).
[[nodiscard]] auto approved_key(const StartupEntry& entry) -> std::wstring_view;

} // namespace change
} // namespace tools::winsys
