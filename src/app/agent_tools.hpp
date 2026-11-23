#pragma once

// app::agent — the agent thread's side of the tools: one tool call through tools::call, a
// question turned into events::Permission for the UI and its answer waited for. The agent never
// answers a question itself: `ask` returns what the user clicked, or nothing (no answer, the
// task stopped), which is a no.

#include "app/config.hpp"
#include "app/events.hpp"
#include "app/journal.hpp"
#include "app/monitor.hpp"
#include "security/audit.hpp"
#include "security/policy.hpp"
#include "tools/call.hpp"
#include "tools/shell_jobs.hpp"
#include "tools/winsys/check.hpp"
#include "tools/winsys/reader_data.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace app::agent {

struct Host {
    std::filesystem::path desktop;  // where "on the desktop" is
    std::filesystem::path audit;    // audit.jsonl
    security::Settings settings;    // config.json's advanced flags and limits, and elevation
    // "Sort my desktop" on real tools (M4: tests and the smoke run, on a copy); otherwise played.
    bool sort_desktop = false;
    std::filesystem::path journal; // changes.jsonl; empty: no journal (and nothing to undo)
    monitor::Monitor* monitor = nullptr; // the machine's last minute for the report; none: one reading
    // "Clean up temp files and delete old installers from Downloads": the folders it works in
    // (the user's own in a real run; a copy's in tests and the --m5 smoke run). Empty: played.
    std::filesystem::path temp;
    std::filesystem::path downloads;
    std::filesystem::path documents; // named in the model's prompt: the user's in a real run; empty: not named
    // The desktop's icons (a real run: app::desktop_icons; tests: their own; none on a copy):
    // where each path's icon stands, and putting icons back where they stood (how many it placed).
    std::function<std::vector<std::optional<journal::Spot>>(std::span<const std::string> paths)> icon_spots;
    std::function<std::size_t(std::span<const journal::Spot> spots)> place_icons;
    // The threat check's sources: Windows' own when none (tests: their own machine).
    std::shared_ptr<const tools::winsys::Sources> check_sources;
    // Where the autostart is read and switched: Windows' own (tests: their sandbox branch and folders).
    tools::winsys::RegistryView registry = tools::winsys::RegistryView::real();
    tools::winsys::Folders folders = tools::winsys::Folders::real();
    // Check deeper (phase 13a § 6): agent-reader.exe through the UAC prompt (a real run), a test's
    // facts; none: the button does nothing. Only the user's press calls it — never a tool.
    std::function<core::Result<tools::winsys::reader::Data>()> read_deeper;
};

// What an undo did: changes taken back, those that failed (the first one's reason), whether
// the user said no to a question (the undo stops there).
struct Undone {
    std::size_t done = 0;
    std::size_t failed = 0;
    bool refused = false;
    std::string first_error;
};

// A real run's host: the user's own Desktop, %TEMP% and Downloads, the audit and the journal in
// `data_dir`. Every scenario of the mockups runs on real tools there, each change after its card.
auto user_host(const std::filesystem::path& data_dir, security::Settings settings, monitor::Monitor* monitor) -> Host;

using Ask = std::function<std::optional<events::Answer>(const events::Permission&)>;

// config.json's "advanced" and "limits" as the policy takes them.
auto settings_of(const config::Advanced& advanced, const config::Limits& limits, const config::Access& access,
                 bool elevated) -> security::Settings;
// config.json's "access" as the permission card shows it, and back (the card's change is saved).
auto levels_of(const config::Access& access) -> events::Levels;
auto access_of(const events::Levels& levels) -> config::Access;
// This process runs with an administrator's token (UAC elevated).
auto process_elevated() -> bool;

// The audit screen's text (markdown-lite): whether the chain holds, then the records newest
// first — when, the tool, what it was for, who decided and how it ended.
auto audit_text(const std::vector<security::AuditRecord>& records, const core::Result<std::size_t>& verified) -> std::string;
// The same records as a table for the panel (phase 8): when, the tool, its target, who decided
// and how it ended; the newest first.
auto audit_set(const std::vector<security::AuditRecord>& records) -> events::DataSet;

// The question as the card shows it: the policy's reason, canonical paths ("from -> to" for a
// move or a copy), the command, the process.
auto permission_of(const security::Ruling& ruling, std::string_view preview, std::uint64_t id,
                   const security::Session& session, const security::Settings& settings = {}) -> events::Permission;

// The tools as the agent's tasks use them, each task on a thread of its own (phase 5): a policy
// session per task; the settings, the journal and the audit shared, under their locks.
class Tools {
public:
    explicit Tools(Host host);
    // The undo folder it set (tools::fs::set_undo_root) is let go with it.
    ~Tools();
    Tools(const Tools&) = delete;
    auto operator=(const Tools&) -> Tools& = delete;

    // A task starts over: its limits and the folders allowed for it.
    void new_task(std::uint64_t task);
    // A new task in the journal (it is kept even if it changes nothing), then new_task(); its title
    // is the user's request, heard().
    void begin(std::uint64_t task, std::string title);
    // The user's words for `task` (its request, a line typed while it runs): a web page they name
    // opens without a question (security::Session::named).
    void heard(std::uint64_t task, std::string_view words);
    // The access levels the user set on the card, from the next call on.
    void set_levels(const events::Levels& levels);
    // One call through the policy; what it changed goes to the journal under `task`. `control`:
    // the task's stop and pause (between the objects of a batch).
    auto run(std::uint64_t task, std::string_view name, std::string_view args, const Ask& ask,
             const tools::Control& control = {}) -> tools::Called;
    // Takes back the changes of journal task `target` (one, or all: nullopt) by the calls
    // journal::inverse names, newest first, each asked as any call is (`task` is the asking
    // task, in a fresh session). Not journaled as changes of their own; the journal marks what
    // was undone.
    auto undo(std::uint64_t task, std::uint64_t target, std::optional<std::size_t> which, const Ask& ask,
              const tools::Control& control = {}) -> Undone;
    // "Take over": the user's own terminal in the task's folder. The user's action, so no question;
    // written in the audit as theirs.
    auto take_over(std::uint64_t task) -> core::Result<void>;
    // Something of journal task `task` can still be taken back.
    [[nodiscard]] auto undoable(std::uint64_t task) const -> bool;
    // Where the desktop's icons stood before task `task` moved their files (the Host's icon_spots):
    // in its journal entry, for an undo to put them back.
    void remember_spots(std::uint64_t task, std::span<const std::string> paths);
    // The largest task id in the journal: the earlier runs' tasks and this one's.
    [[nodiscard]] auto last_task() const -> std::uint64_t;
    // The journal as this process knows it (read at start, then kept up to date): a copy.
    [[nodiscard]] auto journal() const -> std::vector<journal::Entry>;
    [[nodiscard]] auto host() const noexcept -> const Host& { return host_; }
    // The tasks' background commands (phase 12 § 8): shell.run starts them here; they end with
    // these tools (the agent closing).
    [[nodiscard]] auto jobs() noexcept -> tools::jobs::Registry& { return jobs_; }

private:
    auto call(std::uint64_t task, std::string_view name, std::string_view args, const Ask& ask,
              const tools::Control& control) -> tools::Called;
    // Both with mutex_ held.
    auto entry(std::uint64_t task) -> journal::Entry&;
    void save(const journal::Entry& e);

    Host host_;
    mutable std::mutex mutex_;                           // sessions_, host_.settings, journal_
    std::map<std::uint64_t, security::Session> sessions_; // by task
    std::optional<security::AuditLog> audit_;            // locks itself
    std::atomic<std::uint64_t> asked_{0};
    std::vector<journal::Entry> journal_;
    tools::jobs::Registry jobs_; // last: its commands end before the rest goes
};

} // namespace app::agent
