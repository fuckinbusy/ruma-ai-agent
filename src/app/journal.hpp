#pragma once

// app::journal — everything the agent changed, task by task (docs/plans/phase-5.md, Task 9):
// changes.jsonl in the data folder, a line per task each time its changes grow — the last line
// of a task is the task, so a crash loses at most the call in flight. A task that only read is
// kept too ("read only · no changes").
//
// Undo goes back through the tools and the policy (the cards ask as usual): the journal names
// the calls that take a change back, it grants nothing.

#include "app/events.hpp"
#include "core/error.hpp"
#include "tools/tool.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace app::journal {

struct Change {
    tools::EffectKind kind = tools::EffectKind::Created;
    std::string from; // UTF-8; Moved/Renamed: where it was; Killed: the program; Ran: the command
    std::string to;   // where it is now (Trashed: where it was)
    bool undoable = false;
    bool undone = false;
    std::string backup; // Edited (phase 12): the copy of what the file held, in <data>\undo
    std::string after;  // Edited: the file's hash right after the edit (changed since: the undo asks)
};

// Where a desktop icon stood before the task moved its file (the desktop view's own
// coordinates): an undo puts it back there, not in the first free cell.
struct Spot {
    std::string path; // UTF-8, where the file was
    int x = 0;
    int y = 0;
};

struct Entry {
    std::uint64_t task = 0;
    std::string title;   // the user's words
    std::string started; // UTC, "2026-10-12T09:47:56Z"
    std::vector<Change> changes;
    std::vector<Spot> spots;
};

// A tool call by name, its arguments as JSON.
struct ToolCall {
    std::string name;
    std::string args;
};

auto change_of(const tools::Effect& e) -> Change;
// The call that takes a change back: Created -> fs.trash, Moved/Renamed -> fs.move back (never
// over what is there now), Trashed -> fs.restore, Restored -> fs.trash. Deleted, Overwritten,
// Killed and Ran cannot be undone: none.
auto inverse(const Change& c) -> std::optional<ToolCall>;
auto inverse(const tools::Effect& e) -> std::optional<ToolCall>;

// Which changes undo change `which` (the whole task: nullopt), in the order to run them: newest
// first, those undone or not undoable left out. A created folder takes the changes inside it
// with it (its files go back before it goes to the Recycle Bin).
auto undo_order(const Entry& e, std::optional<std::size_t> which) -> std::vector<std::size_t>;

// One line, no newline: {"task":…,"title":…,"started":…,"effects":[{"kind":"moved",…}]}
auto to_json(const Entry& e) -> std::string;
auto parse(std::string_view line) -> std::optional<Entry>;
// Appends `e` as a line (the directory must exist).
auto append(const std::filesystem::path& file, const Entry& e) -> core::Result<void>;
// The tasks in `file` in the order they began, each as its last line says; broken lines are
// skipped. A missing file is an empty journal.
auto read(const std::filesystem::path& file) -> std::vector<Entry>;

auto kind_name(tools::EffectKind k) -> std::string_view; // "created", "moved", …

// The change log screen's tasks, newest first: each with its local time against `now`, its
// summary and its changes as a tree under the folder they share (a folder before what is in it).
auto log_of(const std::vector<Entry>& entries, std::chrono::system_clock::time_point now) -> events::Log;

} // namespace app::journal
