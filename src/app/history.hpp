#pragma once

// app::history — the conversations, kept as JSON Lines in %LOCALAPPDATA%\Agent\history\
// YYYY-MM.jsonl: one line per finished step (an answer, a done bar, a failure, a stop). The
// writer appends on its own thread: the UI thread never waits for the disk.

#include "app/events.hpp"
#include "app/sheet_model.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace app::history {

struct Record {
    std::string time; // UTC, "2026-10-10T20:31:05Z"
    std::uint64_t task = 0;
    std::string kind; // "answer", "done", "failed", "stopped"
    std::string question;
    std::string text; // the answer, or the done bar's title and detail
    std::vector<events::ChecklistItem> plan; // the turn's plan, its last state (phase 12 § 3); none: no key
};

// A JSON string literal: quotes, backslashes and control characters escaped; valid UTF-8 kept
// as it is, a broken byte becomes U+FFFD (a strict reader would reject the line).
auto json_string(std::string_view s) -> std::string;
// One line, no newline: {"time":...,"task":...,"kind":...,"question":...,"text":...}
auto to_json(const Record& r) -> std::string;
auto now_utc() -> std::string;

// After the model has applied `e`: the record worth keeping, if any (answers with their
// buttons, done bars, failures of the current task).
auto record_of(const sheet::Model& m, const events::Event& e, std::string time) -> std::optional<Record>;
// The user stopped task `stopped_task` (the model already moved on): what had been answered so
// far, unless that answer was recorded already (`answered_task`: the last task whose answer was,
// e.g. a button's action being stopped).
auto stopped(const sheet::Model& m, std::uint64_t stopped_task, std::uint64_t answered_task, std::string time)
    -> std::optional<Record>;

// The questions of the last `max` tasks in `dir`'s month files, oldest first (one per task;
// unreadable lines skipped): what Up brings back in the bar. Reads only the two newest files'
// last 256 KB each.
auto recent_questions(const std::filesystem::path& dir, std::size_t max) -> std::vector<std::string>;

class Writer {
public:
    explicit Writer(std::filesystem::path dir);
    ~Writer(); // writes what is queued, then stops
    Writer(const Writer&) = delete;
    auto operator=(const Writer&) -> Writer& = delete;

    void add(Record r);

private:
    void run(const std::stop_token& stop);

    std::filesystem::path dir_;
    std::mutex mutex_;
    std::condition_variable_any ready_;
    std::deque<Record> queue_;
    std::jthread thread_; // last: stopped and joined first
};

} // namespace app::history
