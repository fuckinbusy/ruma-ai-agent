#pragma once

// app::present — the screens of phase 5 made from what the tools returned, for the real model
// (phase 7; the model writes text, the numbers on screen are the tools').
// One Task per task, on the task's thread; each call gives the events to post:
// - every tool call is a Step card (Run, then Done or Failed), its title from its arguments;
// - system.info and process.list read: the system report (report::make_report);
// - a question about space answered from system.info, nothing changed: the drive's quick answer;
// - shell.run: the plan and the terminal's output;
// - the answer complete: its buttons and what it cost.

#include "ai/types.hpp"
#include "app/agent_tools.hpp"
#include "app/events.hpp"
#include "app/memory.hpp"
#include "markdown/markdown.hpp"
#include "tools/call.hpp"
#include "tools/shell_jobs.hpp"
#include "tools/winsys/private_rules.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace app::present {

// A table of the answer as the data panel shows it (titled "Table", source events::kAnswerSource):
// the set Task::answered makes, and what a table card makes from its own rows before that set has
// come (the answer still being written, stopped, cut short).
auto answer_table(const markdown::Block& table) -> events::DataSet;

// A step's title from the call's own arguments, never the model's words: "List Desktop",
// "Read notes.txt", "Run: Get-ChildItem". Arguments that do not parse: the tool's name.
auto step_title(std::string_view tool, std::string_view args) -> std::string;

// The model's ui.suggest (phase 8): its prompts as chips — at most 3, each 1 to 60 characters;
// anything else is refused whole. The schema says the limits in words: strict mode refuses
// maxItems/maxLength, and one refused keyword fails every request.
inline constexpr std::string_view kSuggest = "ui.suggest";
inline constexpr std::string_view kSuggestDescription =
    "Offer the user up to three short follow-up requests as buttons under your answer (each at most 60 "
    "characters, in the user's language). Use it when a next step is likely; the user may ignore them.";
inline constexpr std::string_view kSuggestSchema =
    R"({"type":"object","properties":{"prompts":{"type":"array","items":{"type":"string"},)"
    R"("description":"At most three follow-up requests, each at most 60 characters, most likely first."}},)"
    R"("required":["prompts"],"additionalProperties":false})";
auto suggestions(std::string_view args) -> std::optional<events::Suggestions>;

// Stage 4d-2: the model's ui.settings — open the settings on a section when the user asks where a
// setting is; it changes nothing. Its section, or nullopt for anything else (refused whole).
inline constexpr std::string_view kSettings = "ui.settings";
inline constexpr std::string_view kSettingsDescription =
    "Open the agent's own settings on a section, only when the user asks to see or change a setting of the agent "
    "(its theme, model, API key, roles, rules and memory, private windows, shortcuts). It changes nothing itself: "
    "the user does. Say in one line what you opened.";
inline constexpr std::string_view kSettingsSchema =
    R"({"type":"object","properties":{"section":{"type":"string","enum":["general","appearance","models","roles",)"
    R"("rules_and_memory","privacy","shortcuts"]}},"required":["section"],"additionalProperties":false})";
auto settings_section(std::string_view args) -> std::optional<std::string>;

// The model's plan.update (phase 12 § 3): its plan whole each time — at most 12 items, each a
// title and a state; a call with more, an empty title or another state is refused whole (why).
inline constexpr std::string_view kPlanUpdate = "plan.update";
inline constexpr std::string_view kPlanUpdateDescription =
    "Show the user your plan for this task as a checklist, and keep it current: send the whole plan each time (at most "
    "12 items, in the user's language), one item active while you work on it, done when it is done. Use it for any task "
    "of three actions or more; end with an item \"Check\" (verify what you changed) before you say it is done.";
inline constexpr std::string_view kPlanUpdateSchema =
    R"({"type":"object","properties":{"items":{"type":"array","items":{"type":"object","properties":)"
    R"({"title":{"type":"string","description":"What this step does, briefly."},)"
    R"("state":{"type":"string","enum":["pending","active","done","skipped"]}},)"
    R"("required":["title","state"],"additionalProperties":false},)"
    R"("description":"The whole plan, in order: at most 12 items."}},)"
    R"("required":["items"],"additionalProperties":false})";
auto checklist_of(std::string_view args) -> core::Result<events::Checklist>;

// The model's ui.ask (phase 12 § 4): a question with 2 to 4 options (a label of at most 40
// characters, a description of at most 120), the recommended one's number (from 1) or null, and
// whether several may be chosen; anything else refused whole (why). Its id is the caller's.
inline constexpr std::string_view kAsk = "ui.ask";
inline constexpr std::string_view kAskDescription =
    "Ask the user to choose, when the choice changes the result and cannot be worked out from the request: 2 to 4 options "
    "(label at most 40 characters, description at most 120, in the user's language), the one you recommend by its number. "
    "The user may also answer in their own words. Never ask what your tools can find out.";
inline constexpr std::string_view kAskSchema =
    R"({"type":"object","properties":{"question":{"type":"string"},)"
    R"("options":{"type":"array","items":{"type":"object","properties":{"label":{"type":"string"},"description":{"type":"string"}},)"
    R"("required":["label","description"],"additionalProperties":false},"description":"2 to 4 options."},)"
    R"("recommended":{"anyOf":[{"type":"integer"},{"type":"null"}],"description":"The number of the option you recommend (from 1), or null."},)"
    R"("multi":{"anyOf":[{"type":"boolean"},{"type":"null"}],"description":"Several options may be chosen."}},)"
    R"("required":["question","options","recommended","multi"],"additionalProperties":false})";
auto question_of(std::string_view args) -> core::Result<events::Question>;
// What the model gets back: {"chosen":[...],"other":"..."}, or {"answered":false}.
auto reply_json(const events::Reply& r) -> std::string;

// A background command's reading and its end (phase 12 § 8): the agent's own, never asked — the
// command was allowed when it started.
inline constexpr std::string_view kShellJob = "shell.job";
inline constexpr std::string_view kShellJobDescription =
    "A background command (shell.run with background): whether it still runs, its exit code when it ended, and what it "
    "printed from position `since` on (at most 64 KB; give the `next` you got to read on).";
inline constexpr std::string_view kShellJobSchema =
    R"({"type":"object","properties":{"job":{"type":"integer"},)"
    R"("since":{"anyOf":[{"type":"integer"},{"type":"null"}],"description":"Where to read from; null: the start."}},)"
    R"("required":["job","since"],"additionalProperties":false})";
inline constexpr std::string_view kShellStop = "shell.stop";
inline constexpr std::string_view kShellStopDescription = "End a background command with everything it started.";
inline constexpr std::string_view kShellStopSchema =
    R"({"type":"object","properties":{"job":{"type":"integer"}},"required":["job"],"additionalProperties":false})";
// What the agent remembers about the user (phase 12 § 6): its own, never asked, but shown.
inline constexpr std::string_view kMemoryAdd = "memory.add";
inline constexpr std::string_view kMemoryAddDescription =
    "Remember a lasting preference or fact about the user for later conversations (at most 200 characters; never a secret).";
inline constexpr std::string_view kMemoryAddSchema =
    R"({"type":"object","properties":{"text":{"type":"string"}},"required":["text"],"additionalProperties":false})";
inline constexpr std::string_view kMemoryForget = "memory.forget";
inline constexpr std::string_view kMemoryForgetDescription = "Forget a remembered entry by its id (the user's wish, or when it is no longer true).";
inline constexpr std::string_view kMemoryForgetSchema =
    R"({"type":"object","properties":{"id":{"type":"integer"}},"required":["id"],"additionalProperties":false})";

// private.add (phase 13a § 2.1): a window the agent is never to look at, at the user's word.
inline constexpr std::string_view kPrivateAdd = "private.add";
inline constexpr std::string_view kPrivateAddDescription =
    "Never look at certain windows again, when the user asks: a program (\"Telegram.exe\") or words in a window's title "
    "(a bank's name). The user sees it with Undo; only the user can take one back.";
inline constexpr std::string_view kPrivateAddSchema =
    R"({"type":"object","properties":{"program":{"type":["string","null"],"description":"The program's file name, or null."},)"
    R"("words":{"type":["string","null"],"description":"Words in the window's title, or null."}},)"
    R"("required":["program","words"],"additionalProperties":false})";

using events::kPrivateRow;

// "Rules and memory…" (phase 12 § 6) as the data panel's list: the rules (Open, its action 0),
// each entry (Forget, its id), everything (Forget all, -1) when anything is remembered; then
// PRIVATE WINDOWS (phase 13a): the built-in signs, always on (no action, -2), and the user's rules
// (Forget, kPrivateRow + id).
auto memory_set(std::span<const memory::Entry> entries, std::span<const tools::winsys::PrivateRule> rules = {},
                bool private_windows = true) -> events::DataSet;
// Stage 4d-1: the settings' Privacy list — the private windows alone (the built-in signs, the user's
// to forget), its field to add one over it (SetKind::Private).
auto private_set(std::span<const tools::winsys::PrivateRule> rules) -> events::DataSet;

// {"job":1,"command":"...","running":true,...,"output":"...","next":N}.
auto job_json(const tools::jobs::State& s) -> std::string;

// The translation cards' tools (phase 8, spec § 5.6): the model gives a card's content, the
// agent draws it and answers {"shown":true}; show.text's translation goes to the clipboard.
inline constexpr std::string_view kShowWord = "show.word";
inline constexpr std::string_view kShowPhrase = "show.phrase";
inline constexpr std::string_view kShowText = "show.text";
extern const std::string_view kShowWordDescription, kShowWordSchema;
extern const std::string_view kShowPhraseDescription, kShowPhraseSchema;
extern const std::string_view kShowTextDescription, kShowTextSchema;
// Every field there (strict), or nothing.
auto word_of(std::string_view args) -> std::optional<events::Word>;
auto phrase_of(std::string_view args) -> std::optional<events::Phrase>;
auto text_of(std::string_view args) -> std::optional<events::TextTranslation>;
// The translation as one text: its sentences, spaced unless the language writes none (zh, ja).
auto copied_text(const events::TextTranslation& t) -> std::string;
// Its length in bytes, counted without building it (a frame's clipboard chip).
auto copied_size(const events::TextTranslation& t) -> std::size_t;

// A picture the model is shown, as the window's thumbnail event (phase 9): its pixels turned
// from BGRA to RGBA.
auto picture_event(const tools::ImageOut& image) -> events::Picture;
// A screen question's capture for its panel (phase 10): the larger preview, where it came from,
// what else could be taken, the agent's place on it, sizes, when.
auto capture_event(const tools::Prepared& prepared) -> events::Capture;
// A call that only shows something: no step, no activity of the task.
[[nodiscard]] auto shows(std::string_view tool) -> bool;

// The files and folders an answer names (phase 10, after a hand check: a path in the text could
// not be opened or copied). A path is a drive's — "C:\…" or "C:/…", never a network one — and
// ends where the disk says: of the ways to read it (to the line's end, or cut at a space or a
// mark after its last backslash), the longest that `probe` finds. `probe`: nothing when it is not
// there (or may not be shown), else whether it is a folder. Each once, at most kMaxPlaces.
inline constexpr std::size_t kMaxPlaces = 6;
inline constexpr std::size_t kMaxProbes = 64; // the disk asked at most this often an answer, each reading once
using Probe = std::function<std::optional<bool>(const std::string& path)>;
auto places_in(std::string_view text, const Probe& probe) -> std::vector<events::Place>;
// The disk, as places_in asks it: there, and not in the secrets zone (security::paths).
auto on_disk(const std::string& path) -> std::optional<bool>;

class Task {
public:
    // `host`: where the desktop is and the monitor for the report; none: no highlight, one reading.
    Task(std::uint64_t task, std::string request, const agent::Host* host);

    auto started(const ai::ToolCall& call) -> std::vector<events::AgentEvent>;
    auto finished(const ai::ToolCall& call, const tools::Called& called) -> std::vector<events::AgentEvent>;
    auto progress(const ai::ToolCall& call, const tools::ProgressInfo& p) -> std::vector<events::AgentEvent>;
    auto output(const ai::ToolCall& call, std::string_view text) -> std::vector<events::AgentEvent>;
    // The model's answer is complete: the quick answer when it is one, the buttons, the cost, and
    // the tables and places in `whole` (all the answer's words on screen; empty: `text`).
    auto answered(std::string_view text, const ai::Usage& usage, double usd, std::string_view whole = {}) -> std::vector<events::AgentEvent>;

    // A follow-up in the same conversation: what `before` read of system.info is this task's too
    // (the model answers from it; the drive's quick answer shows its numbers).
    void carry(const Task& before);

    // A program the report offered to close, by its pid (its Close names it to the model).
    [[nodiscard]] auto program(std::uint32_t pid) const -> std::optional<std::string>;
    // A program of the Programs tab, by its button's number: "Blender 4.2 LTS 4.2.1" (phase 13a).
    [[nodiscard]] auto app(std::uint32_t row) const -> std::optional<std::string>;
    // A threat finding's action, by its row and button: "Quarantine: svchost.exe outside Windows' folders."
    [[nodiscard]] auto finding(std::uint32_t row, std::uint32_t action) const -> std::optional<std::string>;
    // Check deeper (phase 13a § 6): the last threat report's set `id` again, `r` in it; none when
    // `id` is not that report's.
    auto deepened(const tools::winsys::CheckReport& r, std::uint32_t id) -> std::optional<events::DataSet>;
    // The last Startup programs set's id (0: none), and that set again from a new startup.list
    // (after Check deeper: the helper's timing in it); none when `id` is not it.
    [[nodiscard]] auto startup_set_id() const -> std::uint32_t { return startup_set_; }
    auto startup_again(const tools::Called& called, std::uint32_t id) -> std::optional<events::DataSet>;
    // A step inside the agent that ran no tool (Check deeper's), as an Activity entry in its order.
    auto inside(std::string title, std::string output, bool failed) -> events::Activity;

private:
    std::uint64_t task_;
    std::string request_;
    const agent::Host* host_;
    std::chrono::steady_clock::time_point began_; // the quick answer's "0.4 s"
    std::uint32_t next_step_ = 1;
    std::map<std::string, std::uint32_t> step_of_; // by call id
    std::string system_info_, process_list_;       // their JSON, once read
    bool reported_ = false;
    bool changed_ = false;                          // a tool that changes things ran
    std::vector<std::pair<std::uint32_t, std::string>> closable_;
    std::vector<std::string> uninstallable_;        // the Programs tab's, by its buttons' numbers
    std::shared_ptr<const events::ThreatView> threat_; // the last threat check's, for its findings' buttons
    std::uint32_t threat_set_ = 0;                  // its set's id in the panel
    std::uint32_t startup_set_ = 0;                 // the last Startup programs set's id (its Check deeper)
    std::map<std::string, std::uint32_t> disk_set_of_; // disk.usage's set while it counts, by call id
    std::vector<events::PlanItem> plan_;            // shell.run commands
    std::map<std::string, int> plan_of_;            // by call id
    std::map<std::wstring, std::uint32_t> document_of_; // a PDF's Document set, by its path (phase 10)
    // A document doc.write made, its Preview set by its path (lower case): a later tool that writes
    // that file (a script's new file moved over it) shows in the Preview as the file is (phase 10).
    std::map<std::wstring, std::uint32_t> preview_of_;
    int calls_ = 0;                                 // the Activity's numbering
    std::map<std::string, std::chrono::steady_clock::time_point> began_of_; // by call id
};

} // namespace app::present
