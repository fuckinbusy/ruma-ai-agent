#pragma once

// tools::winsys::tasks — the scheduled tasks the user can see, read through the Task Scheduler's
// COM interfaces with GetFolder, GetTasks and get_Definition only (phase 13a § 1).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tools::winsys {

struct TaskInfo {
    std::string path;     // "\Microsoft\Windows\Defrag\ScheduledDefrag"
    std::string author;
    std::string account;  // who it runs as
    std::wstring command; // the first exec action: path and arguments
    std::wstring file;
    std::string triggers; // "at logon", "daily", "at startup", "on idle", … joined
    bool enabled = true;
    bool at_logon_or_startup = false;
    std::optional<std::int64_t> last_run; // seconds since 1970, UTC
};

struct TaskList {
    std::vector<TaskInfo> tasks;
    std::vector<std::string> not_read; // folders the user cannot read
};

[[nodiscard]] auto tasks() -> TaskList;

// A task's account is this user's ("me", "PC\me"): the user may switch it off without administrator
// rights. SYSTEM, a service account, a group, another user, or none said: not.
[[nodiscard]] auto own_account(std::string_view account) -> bool;

} // namespace tools::winsys
