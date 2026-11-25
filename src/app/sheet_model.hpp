#pragma once

// app::sheet — the sheet's state, independent of the window and of drawing: whether it is open,
// what the agent is doing, the text field, the question, the answer so far, the offered buttons.
// User actions return the command to send to the agent; agent events update the state.

#include "app/events.hpp"
#include "app/model_choice.hpp"
#include "app/settings.hpp"
#include "app/spend.hpp"
#include "app/picker.hpp"
#include "app/terminal.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace app::sheet {

// A text field; the caret is a byte offset on a UTF-8 character boundary. The bar's field holds
// lines (phase 10, screens 27-31); the key and the filter are one line.
struct Edit {
    std::string text;
    std::size_t caret = 0;
    // A selection (responsive text): from here to the caret; kNoAnchor: none.
    static constexpr std::size_t kNoAnchor = static_cast<std::size_t>(-1);
    std::size_t anchor = kNoAnchor;
};

// Its selection's bytes [first, second) (none: both the caret).
[[nodiscard]] auto selection(const Edit& e) -> std::pair<std::size_t, std::size_t>;
// Removes the selected text; false when there was none.
auto erase_selection(Edit& e) -> bool;
// A move of the caret (`step`): with shift it selects from where it was, without it drops the selection.
void extend(Edit& e, bool shift, void (*step)(Edit&));

// At the caret. One line: line breaks and tabs become spaces. `lines`: CRLF and CR become '\n',
// a tab a space.
void insert(Edit& e, std::string_view text, bool lines = false);
void backspace(Edit& e);
void del(Edit& e);
void left(Edit& e);
void right(Edit& e);
void home(Edit& e);
void end(Edit& e);
void word_left(Edit& e);
void word_right(Edit& e);
// Deletes the spaces before the caret and the word before them (Ctrl+Backspace).
void backspace_word(Edit& e);

enum class Activity : std::uint8_t {
    Idle,      // nothing asked yet, or stopped before any answer
    Thinking,  // working, no answer text yet (or acting on a pressed button)
    Answering, // answer text: streaming, or complete and waiting with its buttons
    Done,      // finished (or failed): the result bar
};

// What the sheet shows.
// Rest: closed — the handle at the screen's edge, whatever goes on behind it.
// Terminal: the detailed mode (7_chat_terminal), the conversation and plan beside the agent's terminal.
enum class View : std::uint8_t { Rest, Input, Thinking, Answering, Done, Permission, Quick, Steps, Log, Report, Terminal, Settings };

// Stage 4d-1 (SETTINGS_SPEC): settings are a mode of the agent's own window, a section at a time.
enum class Section : std::uint8_t { General, Appearance, Models, Roles, RulesMemory, Privacy, Shortcuts };
struct Settings {
    bool open = false;
    Section section = Section::General; // the one shown, and the one the next opening shows
    bool back_to_chat = false;          // a conversation was there: closing goes back to it
    // What the sections show, as the shell made it from config.json (settings::pages, in Section's
    // order), and the two lists' sets in Model::sets (0: none yet).
    std::array<std::vector<settings::Item>, settings::kPages> pages;
    std::uint32_t memory_set = 0, private_set = 0;
    bool key_open = false; // stage 4d-2: Models' key field (Model::key), checked as the bar's was
    bool key_asked = false; // its key is being checked: the verdict is Models', even if the field went
    spend::Month spend;    // stage 4d-2: this month's spending, Models' last line (the shell keeps it)
};

// The change log's segments: today's tasks, the last seven days', all.
enum class LogRange : std::uint8_t { Today, Week, All };

// A question the user has answered (MS 11): what was asked, what they said and what was ticked.
// Its card shrinks into a summary row that stays in the turn; the view keeps its objects' panel
// while that moves. Gone with the next question or task.
struct Answered {
    events::Permission asked;
    events::Choice choice = events::Choice::Deny;
    std::vector<bool> ticks;
};

// One task as the sheet shows it: the one in front is the Model itself, the others wait in
// Model::background with the same state.
struct TaskState {
    Activity activity = Activity::Idle;
    std::uint64_t task = 0; // the task's id; 0 = none yet
    std::string question;
    std::string step;       // thinking: what the agent is doing
    std::string answer;     // markdown-lite
    bool streaming = false;
    // Asked (or a button pressed) and not yet ended: the one "it works" (phase 10). The activity
    // says what is shown; a question, its answer, a long step after the words keep it running.
    bool running = false;
    std::vector<events::Action> actions; // under the answer
    std::string done_title;
    std::string done_detail;
    std::vector<events::Action> done_actions;
    bool failed = false;
    std::string failed_partial; // the failed turn's words at the answer's end: Retry takes them off
    events::Card card;          // stage 4c: the failure as its card (empty title: the plain line)
    std::vector<std::string> routes; // stage 4c: each route once, made when it came (never in a frame)
    std::string switched_to;    // stage 4c: asked with another model than the last request: its full name
    std::optional<events::Permission> permission; // a question waiting for the user
    std::optional<Answered> answered;             // the last one answered: its summary row in the turn
    std::vector<events::Step> steps;              // in the order they first came
    std::optional<events::Summary> summary;
    float progress = -1;                          // the bar's bottom line; -1: none
    std::optional<events::Quick> quick;           // a short answer in the bar
    bool quick_more = false;                      // Tab: the whole answer instead of the quick one
    std::optional<events::Report> report;         // the system report (its sentence is the answer)
    std::shared_ptr<const events::ThreatView> threat; // its threat check's report (phase 13a): the handle's words when it ends behind
    std::optional<events::Spent> spent;           // what the answer cost (phase 7), under it
    std::string_view asked_model;                 // the model the request went to (a catalog id): its mark while it works
    bool paused = false;                          // held by the user (Pause in the list)
    // The detailed mode: the plan, and what its processes printed by tab.
    std::optional<events::Plan> plan;
    struct Tab {
        std::uint32_t id = 0;
        std::string label;
        terminal::Buffer buffer;
    };
    std::vector<Tab> tabs;
    // Phase 8: its data sets (ids in Model::sets), what it ran, when it was asked and answered.
    std::vector<std::uint32_t> data;
    std::vector<events::ActivityEntry> activity_log;
    std::vector<std::string> suggestions; // the model's next prompts, as chips
    // A translation card (phase 8, the model's show.* tools): a word, a phrase, or a text.
    std::optional<events::Word> word;
    std::optional<events::Phrase> phrase;
    std::optional<events::TextTranslation> text_translation;
    std::vector<events::Picture> pictures; // what the model was shown, as thumbnails (phase 9)
    std::vector<events::Refusal> refusals; // what the tools refused, a card each (phase 10)
    std::vector<events::Created> created;  // the files this turn made, a card each (phase 10)
    std::vector<events::Place> places;     // the files and folders the answer names, a row each (none of `created`)
    std::string time;      // "14:32": Model::clock when it was asked
    double started_at = 0; // Model::now then…
    double ended_at = -1;  // …and when its answer was complete
    // Phase 12 § 9: when the model's present thought began (none: it is not thinking now) and
    // whether "Answer now" was pressed for it.
    std::optional<double> thinking_since;
    bool answer_pressed = false;
    std::optional<events::Checklist> checklist; // the model's plan (phase 12 § 3), its last state
    std::optional<events::Question> query;      // the model's question waiting for the user (phase 12 § 4)
    std::vector<events::Remembered> remembered; // what this turn remembered: a line each, with Undo (phase 12 § 6)
    std::vector<events::Looked> looked;         // the user's window, looked at or not (phase 13a § 2): a line each
};

// An earlier turn of the session (phase 8): the conversation column shows it folded, a click
// unfolds it and shows its data.
struct Turn {
    std::uint64_t task = 0;
    std::string question;
    std::string time;
    std::string answer;
    std::vector<events::Step> steps;
    std::vector<std::uint32_t> data;
    std::vector<events::ActivityEntry> activity;
    std::vector<events::Picture> pictures;
    double seconds = 0; // from its question to its answer
    std::vector<std::string> routes; // stage 4c: where its turns went instead, as said above its answer
    std::string switched_to;         // stage 4c: the model the user changed to before it (its full name)
    bool collapsed = true;
    std::optional<events::Checklist> checklist; // its plan's last state (phase 12 § 3)
};

// A card that drops from the tab (6_background): a background task's question, its result, or
// what a watcher saw.
enum class DropKind : std::uint8_t { Question, Result, Alert };
struct Drop {
    std::uint64_t task = 0;
    DropKind kind = DropKind::Result;
    std::string title; // "Needs a decision", "Downloads cleaned up", "Memory is at 93%"
    std::string text;
    std::vector<events::Action> actions; // a result's buttons (the task's own)
    bool failed = false; // a result that did not work: the handle is red (phase 10)
    std::string more;    // an answer's preview: the line after its first sentence (screen 14)
    bool answer = false; // an answer complete behind the handle: Open answer, Copy
    std::uint64_t made = 0; // Model::drops_made when it was made: which card it is (its notification's)
    bool notify = false;    // a notification even without a fullscreen app (a background command ended, phase 12 § 8)
    std::size_t red = 0;    // a threat check's: the text's first bytes in red (phase 13a, 07a)
    bool quiet = false;     // …and stopped part way: no dot
};

struct Watcher {
    std::uint64_t task = 0;
    std::string title; // "Watching memory"
    std::string detail;
    bool on = true;
};

// The hint under the orb after a quick change (design/ui_model, screen 4): "Thinking Low → High",
// "DeepSeek Pro", "Max from the next request"; at a limit it shakes in a yellow frame.
struct ChoiceHint {
    std::string text;
    double at = -1.0e9; // when it was given (the model's clock)
    bool limit = false;
};

// A chip in the input (phase 9): a pasted picture (no path), or a dropped file (a picture among
// them decoded in the background); a refused one says why and is not sent.
struct Attachment {
    std::string label, path;
    std::optional<tools::ImageOut> image;
    std::string refused;
    std::optional<events::Picture> thumb; // the chip's picture, RGBA
    // Phase 10 (screen 17): what tells it is the right file ("PNG · 1920×1200 · 1.4 MB", "48 pages
    // · 2.3 MB"; a refused one: "Won't be sent · secrets"), and when Ctrl+V put it there (its
    // "Ctrl+V" mark for 2 s; never pasted: far in the past).
    std::string meta;
    double pasted_at = -1.0e9;
    std::string text; // a long paste (phase 10, screen 31): "Pasted text", its lines go with the request
};

struct Model : TaskState {
    bool open = false;
    Edit input;          // the bar's field, or the answer card's footer field
    bool editor = false; // the field opened as the editor (phase 10, screen 29: Ctrl+E or its ⤢)
    // A long paste made a chip (phase 10, screen 31): the field as it was then. Its notice stands
    // while the field is so and the chip is there.
    std::optional<std::string> pasted_field;
    std::vector<Attachment> attachments;
    std::string attach_note; // "At most 6 attachments", until one goes
    bool drag_over = false;  // files dragged over the window (the shell's drop target): the drop zone
    bool retaking = false;   // the screen question's picture is being taken again (phase 10)
    std::uint64_t pictures_taken = 0; // the last events::Picture::id given
    std::uint64_t closed_task = 0; // the task the closing sent behind the handle: the opening brings it back
    bool unsafe = false; // an advanced flag of config.json is on: the sheet says so
    std::vector<TaskState> background; // tasks still working behind the one in front
    std::vector<std::string> recent;   // questions asked, oldest first (Up and Down recall them)
    std::size_t recall = 0;            // how far back Up went; 0: the field is the user's own
    // The permission card's choices, fresh for each question: the items ticked, "delete
    // permanently", "don't ask again for this folder" (or a command's "for this task": one tick,
    // never both offered), the whole list shown.
    std::vector<bool> ticks;
    bool permanent = false;
    bool folder_tick = false;
    bool all_shown = false;
    std::size_t pick = 0; // the version picked on its card (phase 13a, 06b)
    // The question card's (phase 12 § 4): the options ticked when several may be chosen, and the
    // field taking the user's own answer ("Other").
    std::vector<bool> picks;
    bool answering_other = false;
    events::Levels levels; // the access levels as config.json has them (the card shows and sets them)
    double think_seconds = 60; // config.json limits.think_seconds: a thought this long gets "Answer now"
    int context = 0;           // the conversation's history, % of its limit (phase 12 § 10): said past 50
    // The change log (tray -> Change log…): shown while no task is in front; the field searches it.
    std::optional<events::Log> log;
    LogRange log_range = LogRange::Today;
    std::uint64_t log_task = 0; // the task picked on the left; 0 or hidden: the first shown
    // The background (phase 5): cards dropped from the tab, newest last; the watchers; the mouse
    // over the tab (its list open).
    std::vector<Drop> drops;
    std::uint64_t drops_made = 0; // one more for each card dropped: the view shows a new one's preview
    std::vector<Watcher> watchers;
    std::uint64_t last_task = 0; // the newest task id given: a task brought back to the front keeps its own
    // The detailed mode: open or folded, the tab shown, the plan step whose command is under the mouse.
    bool expanded = false;
    std::size_t tab_shown = 0;
    int hovered_step = -1;
    std::string agent_usage; // the terminal's foot: "Agent CPU 6% · RAM 140 MB" (the shell measures it)
    // The DeepSeek key's field (phase 7): the bar's field shows dots; the key is never in `input`,
    // in `recent` or in the log. Enter hands it to the shell (key_entered) and forgets it.
    bool key_mode = false;
    Edit key;
    std::optional<std::string> key_entered;
    // no agent without a key, and a key kept only once DeepSeek takes it.
    bool has_key = true;      // the shell says at start (secrets.bin); false: the sheet opens on the key
    bool key_checking = false; // DeepSeek is being asked (the row says so; Enter waits)
    std::string key_error;     // why the last key wasn't kept: its form, DeepSeek's no, no answer
    // Phase 8: the session — the earlier turns, newest first (the turn in front is the TaskState
    // itself) — the data sets its turns found (ids unique in the run: the agent numbers them),
    // the one the panel shows (0: none).
    std::vector<Turn> session;
    std::vector<events::DataSet> sets;
    std::uint32_t active_set = 0;
    std::uint32_t doc_page = 0; // the Document set's page shown (0: its first read; phase 10)
    bool export_xlsx = false;   // Ctrl+Shift+E: the panel's table to XLSX, at its next frame (phase 10)
    bool fresh_chat = false; // "New chat": the next question starts a new conversation
    std::uint64_t activity_task = 0; // the panel shows this turn's Activity (0: a data set)
    bool transcription = true;       // translation cards show pinyin / IPA (a switch, no request)
    Edit filter;             // the panel's filter of the set it shows…
    // Private windows' own field (Rules and memory, phase 13a): what it is (0 App, 1 Title, 2 Site),
    // typed while it has the focus.
    Edit private_add;
    bool private_focus = false;
    int private_kind = 0;
    bool filter_focus = false; // …which takes the typing while it has the focus
    double now = 0;          // the shell's clock, seconds: a turn's duration
    std::string clock;       // "14:32", the shell's once a minute: a turn's time
    // The model and thinking level (design/ui_model): the setting (config.json), a change for the
    // next request only, the menu's hover of a level (the orb shows it, nothing is kept).
    ModelChoice choice;
    std::optional<ModelChoice> next_only;
    Settings settings; // stage 4d-1: the settings mode
    ai::routing::Roles roles = ai::routing::default_roles(); // stage 4c: config.json's ai.roles
    std::vector<std::string> disabled; // stage 4d-2: config.json's ai.disabled — not in the menu
    std::string last_chosen; // the model chosen for the last request of this conversation
    std::optional<ModelChoice> preview;
    std::string_view previous_model; // the one chosen before (a catalog id): Ctrl+Shift+M goes back to it
    ChoiceHint choice_hint;
    double budget_usd = 0.10; // config.json's budget per task (the tray sets it)
    // The model menu (stage 4b, MODEL_PICKER_SPEC): over the bar, the view under it unchanged.
    struct Menu {
        bool open = false;
        Edit query;               // the search: the typing goes here while it is open
        std::size_t active = 0;   // the one lit row, over all its groups' rows in order
        bool by_keys = false;     // lit by the keys (its ring), else by the mouse
        std::vector<picker::Group> groups; // made again when the query or a provider's state changes
        std::optional<ai::routing::Need> role; // stage 4d-2: opened for a role of Roles (its choice is that role's)
    };
    Menu menu;
    // The last state of each provider the agent said (ProviderState), for the menu's rows.
    std::vector<events::ProviderState> providers;
    // The session's last answers' prices, newest last (at most kAnswerCosts): the menu's tooltip.
    struct AnswerCost {
        std::string model; // the catalog id of the model that answered
        double usd = 0;
    };
    std::vector<AnswerCost> answer_costs;
};

// An earlier turn unfolded (its first data set to the panel), or folded again.
void open_turn(Model& m, std::size_t turn);
// A data set to the panel (a card or a tab clicked): its rows unfiltered.
void show_set(Model& m, std::uint32_t id);
// A Space set (phase 13a § 4) at another folder of its count (`path`: indices down from its top) or
// in its other view, made again in place: the same id and tab, the count's own time.
void space_show(Model& m, std::uint32_t set, std::vector<std::uint32_t> path, bool files);
// A turn's Activity to the panel (its actions line or the Activity tab clicked).
void show_activity(Model& m, std::uint64_t task);
// The panel's cross: no set nor Activity in it, one column again (a set's card shows it again).
void hide_panel(Model& m);
// A page link of the answer ("p. 7", page:7) clicked: the newest Document of the turn in front to
// the panel, on that page (phase 10).
void show_page(Model& m, std::uint32_t page);
// The translation cards' transcriptions on or off (the Pinyin switch): nothing asked of the agent.
void toggle_transcription(Model& m);
// The panel's filter takes the typing (a click on it), or gives it back (Esc, a click elsewhere).
void focus_filter(Model& m, bool on);
void focus_private(Model& m, bool on);
// The field's rule for the agent, the field emptied; nothing typed: none.
auto add_private(Model& m) -> std::optional<events::Command>;
// "New chat": the session empties, the questions asked stay for Up; a working task is stopped.
auto new_chat(Model& m) -> std::optional<events::Command>;

// The key's field, open: what is typed or pasted goes there (tray "DeepSeek API key…", or a
// card's "Add key").
void ask_key(Model& m);
// The first open after the installer (--after-install): the key's field when there is no key yet,
// otherwise the plain field.
void after_install(Model& m, bool has_key);
// The key wasn't kept: DeepSeek refused it, or couldn't be asked. The field stays, `why` under it.
void key_rejected(Model& m, std::string_view why);
// What the field shows of `e`: a dot for each character, the caret where it was.
auto masked(const Edit& e) -> Edit;
// The same into `out`, keeping its buffer: what a frame uses (no allocation once it fits).
void mask_into(const Edit& e, Edit& out);
// The shell saved the key (DPAPI) or could not: said on the done bar.
void key_saved(Model& m, bool ok, std::string_view error);

// After apply(): a question of the task in front, the sheet closed — open it with its card. A
// background task's question never does (Review Focus 4): it waits in its card.
[[nodiscard]] auto opens_sheet(const Model& m, const events::Event& e) -> bool;
// The task in front asks, and the sheet could not open for it (a fullscreen app is in front):
// the task goes behind the tab with its question as a card — never answered, never lost.
// False when nothing in front asks.
auto send_back(Model& m) -> bool;
// The task has a plan or a terminal to show in the detailed mode.
[[nodiscard]] auto has_detail(const TaskState& t) -> bool;
void expand(Model& m, bool on);
// Anything to show with the sheet closed: tasks in the background, watchers, cards.
[[nodiscard]] auto has_background(const Model& m) -> bool;
// The background tasks' progress together, 0..1; -1: none can say.
[[nodiscard]] auto overall_progress(const Model& m) -> float;
// The list's buttons: Pause (then Resume), Stop, a watcher's switch.
auto pause_task(Model& m, std::uint64_t task) -> std::optional<events::Command>;
auto cancel_task(Model& m, std::uint64_t task) -> std::optional<events::Command>;
auto toggle_watch(Model& m, std::uint64_t task) -> std::optional<events::Command>;
// A watcher switched off goes from the list by its x: its task is cancelled. Nothing for one on.
auto remove_watch(Model& m, std::uint64_t task) -> std::optional<events::Command>;
// What the tab counts: background tasks still at work (or asking, or paused) and watchers on.
[[nodiscard]] auto active_count(const Model& m) -> std::size_t;
// A background task the list shows: one at work, asking or paused. A finished one is only its card.
[[nodiscard]] auto listed(const TaskState& t) -> bool;

// The handle (phase 10, design/ui_remastered screens 11–16): the sheet closed while the agent has
// something — one state by priority: a decision waits > it failed > an answer is ready > it works >
// only watchers. Its words are views of the model's strings (no allocation per frame).
// Alert: what a watcher saw (phase 10, final review) - yellow, its own words, never "Answer ready".
enum class BehindState : std::uint8_t { None, Working, Watchers, Answer, Decision, Failed, Alert };
struct Behind {
    BehindState state = BehindState::None;
    std::uint64_t task = 0;     // the task it is about (0: watchers)
    std::string_view text;      // the request, the question's title, or what failed
    std::size_t step = 0;       // working: the running step (1-based; 0: none yet)…
    std::size_t steps = 0;      // …of the steps begun
    double started_at = 0;      // working: Model::now when it was asked
    float progress = -1;        // working: the rail, 0..1; -1: no fraction
    std::size_t others = 0;     // the eye's count: other background tasks at work and watchers on
    // A threat check's answer (phase 13a, 07a): its own key ("Check done", "Check stopped"), the
    // text's first bytes in red ("1 suspicious"), and quiet — no dot — when it stopped part way.
    std::string_view key;
    std::size_t red = 0;
    bool quiet = false;
};
[[nodiscard]] auto behind_of(const Model& m) -> Behind;

// The hidden agent's sign (UI update, stage 3; design/ui_presence, variant A): the handle at the
// top edge shows one state, its capsule under the mouse one word and one line. The priority is
// behind_of's: a decision > a failure > an answer > work > watchers. What a watcher saw holds the
// sign as a decision does (the warn's colour: it does not go by itself). Views of the model's
// strings: nothing allocated.
enum class PresenceState : std::uint8_t { Rest, Working, Done, Decision, Failed };
struct Presence {
    PresenceState state = PresenceState::Rest;
    std::uint64_t task = 0;   // the task it is about (0: none, or watchers)
    float progress = -1;      // working: 0..1; -1: no fraction known
    std::string_view word;    // "Working", "Answer ready", "Needs a decision", "Failed", "Watching", "Noticed"; none at rest
    std::string_view line;    // the request, the question, what failed; at rest: what the field says
    bool dot = false;         // the 6 px dot beside the bar (done, a decision, a failure)
};
[[nodiscard]] auto presence_of(const Model& m) -> Presence;

void show_log(Model& m, events::Log log);
void close_log(Model& m);
// Within the range, and the field's words (any case) in its title or one of its rows' names.
[[nodiscard]] auto log_visible(const Model& m, const events::LogTask& t) -> bool;
[[nodiscard]] auto log_selected(const Model& m) -> const events::LogTask*;
// An undo from the log: a task of its own (its questions and its result as any task's), then
// the log again.
auto undo(Model& m, std::uint64_t target, std::optional<std::size_t> change) -> std::optional<events::Command>;
// Ctrl+Z, "Undo last": the newest task that can still be taken back, whole.
auto undo_last(Model& m) -> std::optional<events::Command>;

// The card's choices; each a no-op where the question does not offer it.
void tick(Model& m, std::size_t item);
void tick_all(Model& m, bool on);
void choose_permanent(Model& m, bool on);
void tick_folder(Model& m);
// One of the card's versions picked (06b); past its list: nothing.
void pick_choice(Model& m, std::size_t i);

// A question asked before (the history read at start): Up reaches it after this session's.
void remember(Model& m, std::string_view question);
// The ids the earlier runs used (the journal's): this run's tasks come after them, so a task
// never joins an old one's changes.
void resume_tasks(Model& m, std::uint64_t used);

// The task works: its result is still to come (phase 10: the one answer, for every view).
auto working(const TaskState& t) -> bool;

// Any task works, in front or behind (the tray's icon and menu): a finished one waiting in the
// background as a card is not work.
[[nodiscard]] auto busy(const Model& m) -> bool;

// The model and thinking level (design/ui_model). What the next request goes with: the change for
// it alone, or the setting.
[[nodiscard]] auto effective(const Model& m) -> ModelChoice;
// Stage 4c: the next request goes to another model than the one chosen — by its role (a picture in
// the input, High or Max thinking) — the chip shows that one with the route mark.
[[nodiscard]] auto images_elsewhere(const Model& m) -> bool;
// What the next request is sent with: `effective`, its role's model when that is another.
[[nodiscard]] auto sent_with(const Model& m) -> ModelChoice;
// The model menu (stage 4b): open on the model chosen, its search empty; closed, its search cleared.
void open_menu(Model& m);
void close_menu(Model& m);
// Its rows, group after group: how many, and the i-th (null past the last).
[[nodiscard]] auto menu_rows(const Model& m) -> std::size_t;
[[nodiscard]] auto menu_row(const Model& m, std::size_t i) -> const picker::Row*;
// The mouse over a row: it is the lit one (the keys' ring goes).
void hover_row(Model& m, std::size_t i);
// About what an answer of `model` costs (MODEL_PICKER_SPEC § 5): the mean of its last 20 answers
// this session; with none, 3 000 tokens in (none from the cache) and 600 out.
inline constexpr std::size_t kAnswerCosts = 80;
[[nodiscard]] auto answer_cost(const Model& m, const ai::catalog::Model& model) -> double;
// How a provider stands (the chip's dot, the menu's group): no key in the keys file, else the last
// state the agent said of it.
[[nodiscard]] auto provider_trouble(const Model& m, std::string_view provider) -> events::Trouble;

// Stage 4c: where `model`'s pictures go, by the Images role (the menu's tooltip); null: none of
// the role sees them (they would go as their names).
[[nodiscard]] auto images_go_to(const Model& m, const ai::catalog::Model& model) -> const ai::catalog::Model*;
// Stage 4c: a route as the conversation says it, from the catalog's names ("": an id it lost).
[[nodiscard]] auto route_text(const events::Routed& r) -> std::string;

auto view(const Model& m) -> View;
auto view_name(View v) -> std::string_view; // "rest", "input", … for the debug window and the snapshot

void open(Model& m);
void close(Model& m);
void toggle(Model& m);
// Typed text (WM_CHAR, paste); on the done bar it starts a new question.
void type(Model& m, std::string_view text);
// Enter or the send button: a non-blank field becomes a new task.
auto submit(Model& m) -> std::optional<events::Command>;
// Whether submit() would send anything: the send button is dim and takes no click otherwise (phase 10).
[[nodiscard]] auto can_send(const Model& m) -> bool;
// Enter while the task works: what the field holds goes to that task (events::Refine).
auto refine(Model& m) -> std::optional<events::Command>;
// The stop button: cancels the current task if it is still working.
auto stop(Model& m) -> std::optional<events::Command>;
// The done bar's time is up, or it was clicked: back to the empty field. Elsewhere a no-op.
void dismiss(Model& m);
// A button under the answer or on the done bar.
auto press(Model& m, std::string_view action) -> std::optional<events::Command>;
// An event of the agent; events of other tasks are ignored.
void apply(Model& m, const events::Event& e);
// A button of the permission card. AllowFolder, AllowTask only where offered; an advanced-mode
// question needs the word ALLOW typed in the field first.
auto answer(Model& m, events::Choice choice) -> std::optional<events::Command>;
// The screen question's "take it again" (phase 10, screen 19): another monitor, all, a window, or the
// same again; the question stays, the panel says it is being taken until the new one comes.
auto retake(Model& m, events::Answer::Take take, int monitor, std::string window) -> std::optional<events::Command>;
// "Answer now" (phase 12 § 9): while the model thinks, once per thought.
auto answer_now(Model& m) -> std::optional<events::Command>;
// The question card (phase 12 § 4): an option clicked — the reply, or with several to choose a
// tick; Send with the ticked ones (in their order); Skip; Other (the field takes the user's own
// answer, which refine() then sends). Each nothing without a question.
auto choose(Model& m, std::size_t option) -> std::optional<events::Command>;
auto send_choices(Model& m) -> std::optional<events::Command>;
auto skip_question(Model& m) -> std::optional<events::Command>;
void answer_other(Model& m);
// "Remembered: …"'s Undo (phase 12 § 6): the line goes, the agent forgets it.
auto forget_remembered(Model& m, std::uint64_t id, bool private_window = false) -> std::optional<events::Command>;
// Rules and memory's list in the panel (phase 12 § 6): in place of the last one, shown.
void show_memory(Model& m, events::DataSet set);
// Its row's button: Forget — the row goes, the agent forgets it.
auto forget_row(Model& m, std::uint32_t set, std::size_t row) -> std::optional<events::Command>;

// The cost line: "$0.0004 · 92% cached"; under a hundredth of a cent "<$0.0001"; a dollar and
// more with cents. No input tokens: the price alone.
auto spent_text(const events::Spent& s) -> std::string;
// Its parts (phase 10: the conversation draws flash's pill between them): "$0.0006", "58% cached" ("" with no prompt).
auto spent_price(const events::Spent& s) -> std::string;
auto spent_cached(const events::Spent& s) -> std::string;
// The card's text, markdown-lite: the reason in bold, each path or the command as a list item,
// then the preview.
auto permission_text(const events::Permission& p) -> std::string;
inline constexpr std::string_view kAllowWord = "ALLOW";
// A report of the agent itself (the audit screen) as an answer card, shown when the sheet opens:
// a turn of the shell's own (the audit from the tray): the turn in front goes to the session, `set`
// (if any) to the panel. Refused while a task works or asks.
auto show_report(Model& m, std::string_view title, std::string text, std::optional<events::DataSet> set = std::nullopt) -> bool;

// What a key or a click asks of the window and the agent.
struct Reaction {
    std::optional<events::Command> command;
    bool close = false; // collapse the sheet
    std::optional<events::Levels> levels; // an access level changed on the card: config.json and the agent
    bool open = false;                    // open the sheet (a background task brought to the front)
    bool show_log = false;                // the change log (the list's "Log")
    bool attach = false;                  // the paperclip: Windows' file picker (phase 10)
    std::string open_url;                 // a link in the answer clicked: http(s) only, for the browser
    std::uint32_t page = 0;               // a page link (page:7) clicked: that page of the document (phase 10)
    // The data panel's tools (phase 8): text for the clipboard, a file to save where the user
    // picks, a folder to open in Explorer.
    struct SaveFile {
        std::string name; // the name offered
        std::string text;
    };
    std::string clipboard;
    std::optional<SaveFile> save;
    std::string open_folder;
    std::string open_file; // a picture's thumbnail clicked: its file in its program (phase 9)
    std::string not_a_picture; // a thumbnail clicked whose file is not a picture by its name: a card, never run (phase 10)
    std::string open_created;  // a file this turn made, in its program (phase 10): the shell opens only those
    std::string show_in_folder; // a file selected in Explorer (phase 10)
    std::string open_listed;   // a file listed in the panel, clicked: in its program, a program only shown (phase 10)
    std::string show_listed;   // a listed file's "...": selected in Explorer (phase 10)
    // Rules and memory (phase 12 § 6): agent.md opened in its editor; "Forget all" (the shell asks first).
    bool open_rules = false;
    bool forget_all = false;
    // A place the answer names (phase 10, after a hand check): opened — a folder in Explorer, a
    // program only shown — or selected in Explorer. Its Copy path is `clipboard`.
    std::string open_place;
    std::string show_place;
    // The model choice changed (design/ui_model): config.json and the agent's settings. A change for
    // the next request only goes with its Start instead.
    std::optional<ModelChoice> choose;
    std::optional<double> budget; // the picker's budget per task chosen: config.json and the agent
    // Stage 4d-1: a settings item chosen in the settings mode — the shell applies it as the tray's.
    std::optional<unsigned> setting;
    // Stage 4d-2: a role's model chosen in the model menu opened for it (Roles): config.json's ai.roles.
    struct RoleChoice {
        ai::routing::Need need = ai::routing::Need::Hard;
        std::string model;
    };
    std::optional<RoleChoice> role;
};

// Stage 4d-1: the settings mode. Opened on `section`, or the last one; closed, back to the
// conversation if one was there before, else the window goes (its handle: Reaction::close). The
// agent works on behind it; a question for the user comes over it and it comes back after.
void open_settings(Model& m, std::optional<Section> section = std::nullopt);
auto close_settings(Model& m) -> Reaction;
void show_section(Model& m, Section s);
// Stage 4d-2: Add key / Update key — the settings on Models with the key's field (the bar's field
// stays for a first run with no key at all).
void edit_key(Model& m);
// Stage 4d-2: the model menu for a role of Roles (Hard, Images): a choice writes that role
// (Reaction::role), not the conversation's model.
void open_role_menu(Model& m, ai::routing::Need need);
// ui.settings' section ids ("models", "rules_and_memory"…): the section; nullopt: none of them.
[[nodiscard]] auto section_of(std::string_view id) -> std::optional<Section>;
[[nodiscard]] auto section_name(Section s) -> std::string_view;
// The Rules and memory and Privacy sections' lists, made anew by the shell (opening, a rule kept or
// forgotten): they replace the ones before in Model::sets; the data panel is not touched.
void settings_lists(Model& m, events::DataSet memory, events::DataSet private_windows);

// A model choice made (the picker): for the next request only, or the setting (Reaction::choose).
auto set_choice(Model& m, ModelChoice choice, bool next_only, double now) -> Reaction;
// A menu's row chosen (a click, Enter; Alt+Enter: `once`, for the next message only): its model
// with the level kept, the menu closed. A dimmed row does its action instead (Add key, Update key:
// the key's field) or nothing (offline).
auto pick(Model& m, std::size_t i, bool once) -> Reaction;
// Ctrl+Shift+M: back to the model chosen before (none yet: the catalog's next), the level kept. A
// change for the next request alone changes that one.
auto toggle_model(Model& m, double now) -> Reaction;
// The wheel over the orb, Ctrl+[ and Ctrl+]: `delta` levels; past Off or Max, nothing but the hint.
auto step_level(Model& m, int delta, double now) -> Reaction;

// A card's button: "review" (the task to the front, its whole card), "skip" (its question's no),
// "close" (the card goes; a finished task with it), "copy" (its first sentence to the clipboard,
// the card stays: phase 10), or one of a result's own buttons (the task to the front, the button
// pressed).
auto drop_press(Model& m, std::uint64_t task, std::string_view action) -> Reaction;

// A question's answer from the handle's card that changes nothing (screen 15): "Keep all" for a
// delete, "Don't send" for a screen shot, "Skip" otherwise — each "skip", a Deny. The yes is given
// only in the open sheet.
[[nodiscard]] auto safe_answer(const Model& m, std::uint64_t task) -> std::string_view;
// What the card says of it: "Nothing is deleted yet. I'm paused until you decide."
[[nodiscard]] auto decision_note(const Model& m, std::uint64_t task) -> std::string_view;

// The screen dims behind the sheet while it asks (as UAC does): open, with a question in front.
[[nodiscard]] auto wants_dim(const Model& m) -> bool;

enum class Key : std::uint8_t { Enter, Escape, Backspace, Delete, Left, Right, Home, End, Tab, Up, Down, PageUp, PageDown };
// A key in the sheet; `ctrl` makes Backspace, Left and Right work on words; Shift+Enter is a new
// line in the bar's field (phase 10); Alt+Enter in the model menu chooses for the next message only.
auto key(Model& m, Key k, bool ctrl, bool shift = false, bool alt = false) -> Reaction;

// The field's counter (phase 10, screen 27): characters (code points), shown from 400 — "468 chars",
// "7 001 chars" (a no-break space between thousands) — and yellow from 7 000.
inline constexpr std::size_t kCounterFrom = 400;
inline constexpr std::size_t kCounterWarn = 7000;
[[nodiscard]] auto char_count(std::string_view text) -> std::size_t;
// `always`: the editor's (screen 29), there from the first character.
// Its text's room: the largest count (20 digits, six no-break spaces of 2 bytes, " chars") fits.
using CounterText = std::array<char, 48>;
[[nodiscard]] auto counter_text(std::size_t chars, CounterText& buf, bool always = false) -> std::string_view;
// Ctrl+E (screen 29): the field opened as the editor, or folded back; only where the editor can be
// shown (the field takes text, no task works, not the log).
void toggle_editor(Model& m);
// The editor is up: its flag, where it can be shown. Esc folds it only then (final review).
[[nodiscard]] auto editor_shown(const Model& m) -> bool;

// Ctrl+V of text (screen 31): more than kPasteLines lines or kPasteChars characters become a
// "Pasted text" chip ("64 lines · 4 812 chars"), the field left for the question - true; less goes
// into the field. Ctrl+Shift+V types it whatever its length.
// The conversation is what `v` shows (the turns under the row); the chips' row over the row is
// shown (the input, or a conversation) - a long paste makes a chip only there (final review).
[[nodiscard]] auto conversation_view(const Model& m, View v) -> bool;
[[nodiscard]] auto attachments_shown(const Model& m) -> bool;
inline constexpr std::size_t kPasteLines = 20;
inline constexpr std::size_t kPasteChars = 1500;
// Sent with no question, a pasted text's first line is the question shown, at most this many bytes
// and "…" (the model gets it whole).
inline constexpr std::size_t kPastedQuestion = 200;
auto paste(Model& m, std::string_view text, double now = -1.0e9) -> bool;
// The field the keys go to (the bar's, the filter, Private windows'; not the key's): its selected
// text (Ctrl+C), it cut (Ctrl+X: the text removed, returned), all of it selected (Ctrl+A).
[[nodiscard]] auto field_selected(const Model& m) -> std::string_view;
auto cut_field(Model& m) -> std::string;
void select_all_field(Model& m);
// The notice under the row: the last paste became a chip and the field is as it was then.
[[nodiscard]] auto paste_notice(const Model& m) -> bool;
// Its Undo (Ctrl+Z) and Paste as text: the newest chip's text into the field at the caret; false
// with no notice.
auto paste_as_text(Model& m) -> bool;
// A chip clicked: its text into the field, the editor open (screen 31: "a click opens it in the editor");
// its card's Paste as text: the text into the field only. False: not a pasted text.
void open_pasted(Model& m, std::size_t index);
auto paste_chip_as_text(Model& m, std::size_t index) -> bool;
[[nodiscard]] inline auto counter_warn(std::size_t chars) -> bool { return chars >= kCounterWarn; }

// Attachments in the input (phase 9), at most kMaxAttachments; past them attach_note says so.
inline constexpr std::size_t kMaxAttachments = 6;
// A picture pasted with Ctrl+V at `now` (the view's clock): it goes in the next request itself.
void paste_picture(Model& m, tools::ImageOut image, double now = -1.0e9);
// Files dropped on the window: a chip each; `refusal` says why one may not go (a secrets file),
// empty when it may. On sending, each is named in the request ("Attached: <path>").
void drop_files(Model& m, std::span<const std::string> paths, const std::function<std::string(const std::string&)>& refusal);
// A dropped picture, decoded in the background: it goes as a picture too, its path still named.
void attach_image(Model& m, std::string_view path, tools::ImageOut image);
void remove_attachment(Model& m, std::size_t index);
// A dropped file's details, read in the background, on its chip (a refused one keeps why).
void set_details(Model& m, std::string_view path, std::string details);
// A file named as a picture (.jpg, .png, … in any case): decoded when dropped, opened by a click
// on its thumbnail — never a program whose contents happen to be a picture.
[[nodiscard]] auto picture_file(std::string_view path) -> bool;
// A file Windows would run when opened (a program, a script, a shortcut): never opened, only shown (phase 10).
[[nodiscard]] auto program_file(std::string_view path) -> bool;
// A file a click may open in its program (phase 10, final review): documents and media by their
// extension, never one that runs code or carries macros; the rest is only shown in its folder.
[[nodiscard]] auto opens_safely(std::string_view path) -> bool;
// The place of the turn in front with exactly this path; none: the shell does nothing with it.
[[nodiscard]] auto placed(const Model& m, std::string_view path) -> const events::Place*;

} // namespace app::sheet
