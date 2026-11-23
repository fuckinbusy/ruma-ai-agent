#pragma once

// app::events — what flows between the UI thread and the agent thread. Commands go to the agent,
// events come back; both carry the task id, so an event of a task the user already stopped or
// replaced is recognised and dropped. Plain data, moved through core::Mailbox.

#include "tools/image.hpp"
#include "tools/winsys/disk.hpp"
#include "ui/table_data.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <string>
#include <variant>
#include <vector>

namespace app::events {

// A button the agent offers ("Yes, close", "Undo"); `id` comes back in Act.
struct Action {
    std::string id;
    std::string label;
    bool primary = false;
};

// What an action does, by colour: read, change (create, move, write), delete, the system
// (processes, services, the registry); Ok for something done well.
enum class Kind : std::uint8_t { Read, Change, Delete, System, Ok };
// How a value is coloured.
enum class Tone : std::uint8_t { Plain, Accent, Ok, Warn, Crit };
enum class StepState : std::uint8_t { Wait, Run, Done, Failed, Skipped };

struct FileItem {
    std::string name;   // "blender-4.2.1-windows-x64.msi"
    std::string ext;    // "MSI"
    std::string meta;   // "340 MB · 2 mo"
    std::uint64_t bytes = 0;
};

// ---- UI -> agent
struct Start {
    std::uint64_t task = 0;
    std::string text;
    std::uint64_t replaces = 0; // a task whose waiting question this request makes a no (phase 5)
    bool fresh = false;         // "New chat": not a follow-up of the last conversation (phase 8)
    std::vector<tools::ImageOut> images; // pictures the user pasted or dropped: in the request itself (phase 9)
    // "Only for the next request" (design/ui_model): this request's model ("deepseek-v4-pro") and
    // thinking ("max"); empty: the agent's setting.
    std::string model;
    std::string thinking;
};
struct Cancel {
    std::uint64_t task = 0;
};
struct Act {
    std::uint64_t task = 0;
    std::string action;
};
// The user's answer to a Permission — only ever from a click or a key in the UI thread.
// Retake (phase 10, screen 19): not an answer yet — the screen taken again first, the same question.
// AllowTask (phase 12 § 7): yes, and the command's start (Permission::task_scope) for the task.
enum class Choice : std::uint8_t { Deny, Allow, AllowFolder, Retake, AllowTask };
struct Answer {
    std::uint64_t task = 0;
    std::uint64_t id = 0; // the Permission's
    Choice choice = Choice::Deny;
    std::vector<std::size_t> keep; // the items left ticked (indices); empty: all of them
    bool permanent = false;        // "Delete permanently" instead of the Recycle Bin
    // With Retake: how — the same again, a monitor (from 1), all of them, a window by its title.
    enum class Take : std::uint8_t { Same, Monitor, All, Window };
    Take take = Take::Same;
    int monitor = 0;
    std::string window;
};
// Words typed into the bar while the task works: they go to that task (phase 5).
struct Refine {
    std::uint64_t task = 0;
    std::string text;
};
// The access levels the user set on the card (03-SECURITY § Access levels): from the next call on.
struct Levels {
    bool read_asks = false;
    bool change_per_folder = true;
};
// The change log's undo: the changes of journal task `target` (one, or all), as a new task
// `task` that asks as any does.
struct Undo {
    std::uint64_t task = 0;
    std::uint64_t target = 0;
    std::optional<std::size_t> change;
};
// A task in the background (phase 5): held before the next object of its batch, then let go on;
// a watcher switched off and on.
struct Pause {
    std::uint64_t task = 0;
};
struct Resume {
    std::uint64_t task = 0;
};
struct Toggle {
    std::uint64_t task = 0;
    bool on = true;
};
// The user's reply to the model's Question (phase 12 § 4, its ui.ask): the options chosen (their
// labels), their own words ("Other"), or none — skipped, or a new message of theirs instead.
struct Reply {
    std::uint64_t task = 0;
    std::uint64_t id = 0; // the Question's
    std::vector<std::string> chosen;
    std::string other;
    bool answered = true;
};
// Forget what the agent remembered (phase 12 § 6): one entry (the line's Undo, the list's Forget),
// or all of it ("Forget everything").
// Rules and memory's list (phase 13a): a private windows rule's row has this plus the rule's id as
// its action's number (memory entries have their own ids, below it).
inline constexpr double kPrivateRow = 1.0e12;
struct Forget {
    std::uint64_t id = 0;
    bool all = false;
    bool private_window = false; // `id` is a private windows rule's (phase 13a)
};
// A private window the user added in Settings › Rules and memory (phase 13a § 2.1): a program, or
// words in a window's title ("Title" and "Site").
struct AddPrivate {
    std::string program;
    std::string words;
};
using Command = std::variant<Start, Cancel, Act, Answer, Refine, Levels, Undo, Pause, Resume, Toggle, Reply, Forget, AddPrivate>;

// ---- agent -> UI
struct Thinking {
    std::string step; // "Analyzing the load"
};
struct Delta {
    std::string text; // the next piece of the answer (markdown-lite)
};
struct Actions {
    std::vector<Action> actions; // the answer is complete; these buttons follow it
};
struct Finished {
    std::string title;  // "OBS closed"
    std::string detail; // "freed 1.1 GB of RAM"
    std::vector<Action> actions;
};
struct Failed {
    std::string message;
    std::vector<Action> actions; // phase 7: "Retry" after the network broke, "Add key" without a key
    std::string partial;         // what the failed turn had said (the end of the answer): Retry takes it off
};
// The policy asks before a tool call (03-SECURITY): what the card shows are the facts the
// policy decided on — its reason, the canonical paths or the full command — never the model's
// description. The agent waits for an Answer with this id.
// A picture the model was shown (phase 9: image.view, the screen, the clipboard): its thumbnail
// in the turn; a click opens `path` (empty: the screen or the clipboard, nothing to open).
struct Picture {
    std::string label, path;
    int width = 0, height = 0; // what the model saw
    int thumb_w = 0, thumb_h = 0;
    std::vector<std::uint8_t> thumb_rgba; // thumb_w x thumb_h, top row first, not premultiplied
    std::uint64_t id = 0; // given by the model as it takes the picture: the window's texture is found by it
    // Phase 10 (screen 18): its own pixels and format for its tooltip ("1600 × 1120 · PNG"), and
    // whose it is — the user's own (attached, pasted) sits in their message, the rest in the actions.
    int source_w = 0, source_h = 0;
    std::string format;
    bool mine = false;
};
// A picture as the tools made it, as the window shows it: its thumbnail from BGRA to RGBA.
inline auto picture_of(const tools::ImageOut& image) -> Picture {
    Picture p{.label = image.label, .path = image.path, .width = image.width, .height = image.height,
              .thumb_w = image.thumb.width, .thumb_h = image.thumb.height, .thumb_rgba = image.thumb.bgra, .id = 0,
              .source_w = image.source_width > 0 ? image.source_width : image.width,
              .source_h = image.source_height > 0 ? image.source_height : image.height,
              .format = image.mime == "image/png" ? "PNG" : image.mime == "image/jpeg" ? "JPEG" : std::string{}, .mine = false};
    for (std::size_t i = 0; i + 3 < p.thumb_rgba.size(); i += 4) {
        std::swap(p.thumb_rgba[i], p.thumb_rgba[i + 2]);
    }
    return p;
}

// A screen question's capture as its panel shows it (phase 10, screen 19).
struct Capture {
    Picture preview;                       // at most 864 x 432: exactly what goes, larger
    std::string source;                    // "Monitor 1", "All monitors", a window's title
    int monitors = 1;
    std::vector<std::string> windows;      // the open windows' titles, to take one instead
    float hole_x = 0, hole_y = 0, hole_w = 0, hole_h = 0; // the agent's window on it (0..1; w 0: none)
    int width = 0, height = 0;             // the picture's own pixels
    std::string format;                    // "PNG"
    std::size_t bytes = 0;                 // as it would be sent
    int sent_w = 0, sent_h = 0;            // as the model would get it
    std::string taken_at;                  // "14:12:30"
};

struct Permission {
    std::uint64_t id = 0;
    std::string tool;
    std::string reason;
    std::vector<std::string> what; // canonical paths, or the full command
    std::string preview;           // a diff, a list of moves
    std::string zone;
    std::string risk;
    bool folder = false;   // "Don't ask again for this folder" may be offered
    // shell.run (phase 12 § 7): "Allow “npm test” for this task" may be offered (the start; empty:
    // not), and the starts this task allowed already, for "Why I'm asking".
    std::string task_scope;
    std::vector<std::string> task_allowed;
    bool warning = false;  // the agent runs as administrator: asked in red
    bool advanced = false; // possible only by an advanced flag: the word ALLOW first
    // The card of design/interaction-topbar (phase 5): what kind of action, its title and where,
    // one row per object (the policy's paths, with their sizes), the folder's name.
    Kind kind = Kind::Change;
    std::string title;            // "Delete 3 files · 440 MB"
    std::string where;            // "C:\Users\Anna\Downloads"
    std::vector<FileItem> items;  // one per object, in the call's order
    std::string folder_name;      // "Downloads"
    bool offers_permanent = false; // "Delete permanently" is possible (advanced mode has the tool)
    std::optional<Picture> picture; // screen.capture: what the model gets on a yes (phase 9)
    std::optional<Capture> capture; // and its panel (phase 10)
    // Phase 13a (design/ui_system 06a): the card of an action on the system — what happens, what it
    // affects, how to undo it (green when it can be, yellow when not) — and its buttons' own words
    // ("Turn off at startup", "Keep"); empty: the plain card, Allow and Deny.
    std::string happens;
    std::string affects;
    std::string undo;
    bool undo_ok = true;
    std::string allow_label;
    std::string deny_label;
    // Phase 13a (06b): the programs the question may be narrowed to, one picked (the first until the
    // user picks); each with its own title, words and button. The answer's keep is the one picked.
    struct Pick {
        std::string name, detail, size;
        std::string title, happens, affects, allow;
    };
    std::vector<Pick> picks;
};

// ---- the work as it goes (phase 5, design/interaction-topbar)

struct Fact {
    std::string key;
    std::string value;
    Tone tone = Tone::Plain;
};

// One action of the task as a card under the bar; a Step with an id already shown replaces it.
struct Step {
    std::uint32_t id = 0;
    Kind kind = Kind::Read;
    StepState state = StepState::Run;
    std::string title;  // "Deleting temp files to the Recycle Bin"
    std::string detail; // "AppData\Local\Temp\vs_setup_3f2a.tmp"
    std::string result; // "1 284 files · 3.1 GB", or "63%"
    std::vector<FileItem> files; // the first few; `more_files` after them
    std::uint32_t more_files = 0;
    float progress = -1;          // 0..1; -1: none
    std::string progress_text;    // "812 of 1 284 · 1.9 GB"
    std::string eta;              // "~20 s left"
    std::string command;          // the full command it runs, if any
    std::string command_label;    // "PowerShell"
    std::vector<std::string> notes; // warnings: "Skipped 14 files — in use by Chrome, Steam"
    std::string tag;              // a waiting step's note: "will ask permission"
    std::string tool;             // what runs it, monospaced: "fs.list", "Get-ChildItem" (phase 10)
    double since = -1;            // the model's clock when it began (set by the model, not the agent)
    std::string local;            // done on this PC, nothing sent: the line under the answer says it (phase 10)
    bool rejected = false;        // its file refused on purpose (a ZIP bomb, nothing of it readable): not "failed" (phase 10)
    std::string skipped;          // read, but not all of it: "a skipped page" (phase 10)
    std::string file;             // the file it read, when it read one: "I read only plan_A0.pdf" (phase 10)
};

// A file doc.write made (phase 10, screen 24): its card in the turn — Open, Show in folder, what
// of the text it kept — and its Preview in the panel.
struct Created {
    std::string path;   // "C:\Users\…\Documents\Study\L5.docx"
    std::string name;   // "L5.docx"
    std::string type;   // "DOCX"
    std::string size;   // "18 KB"
    std::string folder; // "Documents\Study"
    std::vector<std::string> kept; // "Headings", "Lists", "Money as numbers", "Formula-safe: =, +, @ escaped"
    std::uint32_t preview = 0;     // its Preview's set
};

// A file or folder the answer names that is on the disk (phase 10, after a hand check): a row
// under the answer with Open, Show in folder and Copy path.
struct Place {
    std::string path;   // "C:\Users\…\Documents\Study\report.docx", as the answer wrote it
    std::string name;   // "report.docx"
    std::string where;  // its folder: "~\Documents\Study"
    bool folder = false;
};
struct Places {
    std::vector<Place> places; // in the answer's order, each once
};

// A file, or a part of one, the tools refused (phase 10, screen 25): its card in the turn.
struct Refusal {
    std::string kind;   // "zip_bomb", "sheet_too_large", "page_too_large", "not_an_image", "too_large"
    std::string label;  // its pill: "ZIP bomb"
    std::string title;  // "report_final.xlsx — not opened"
    std::string line;   // in mono: where it is, or its size ("1 048 576 rows × 16 384 columns")
    std::string advice; // what it means and what to do
    bool whole = true;  // nothing of the file was read: it counts in "N files rejected"
};

// The task at a glance, beside its steps.
struct Summary {
    std::string value;  // "1.9"
    std::string of;     // "/ 5.5 GB"
    std::string label;  // "freed"
    float fraction = -1; // the ring; -1: no ring
    std::vector<Fact> facts;
};

// The line along the bar's bottom edge: 0..1, or -1 while it cannot be told (a running sweep).
struct Progress {
    float fraction = -1;
};

struct Segment {
    std::string label;
    double share = 0; // of the whole, 0..1; its colour is the theme's series, by its place
};

// A short answer in the bar itself: a big value, a stacked bar, one action.
struct Quick {
    std::string value; // "100 GB"
    std::string unit;  // "free of 512"
    std::string sub;   // "Disk C: · NVMe · 80% used"
    std::vector<Segment> segments;
    std::string chip;  // in the bar: how long it took, "0.4 s"
    std::optional<Action> action;
};

// The system report (4_system_report): one sentence, four tiles with their last minute, what
// uses memory, what can be done.
struct Tile {
    std::string name;   // "Memory"
    std::string value;  // "13.8", "34%"
    std::string unit;   // "of 16 GB", "52 °C", "needs administrator rights"
    std::string status; // "high", "normal"
    Tone tone = Tone::Ok;
    std::vector<float> series; // percent, oldest first, up to a minute
    std::optional<float> line; // a dashed line at this percent (memory: 90)
};
struct ProcRow {
    std::uint32_t pid = 0; // the biggest of its processes
    std::string name;      // "chrome.exe"
    std::string note;      // "12 processes"
    float cpu = 0;         // percent of the machine
    std::uint64_t memory = 0;
    bool closable = false; // the user's own program: "Close" (process.kill, asked)
};
struct Advice {
    std::string action; // the Act it sends: "close:1234"
    std::string title;  // "Close OBS Studio"
    std::string sub;    // "it uses 0.8 GB"
    std::string gain;   // "+0.8 GB"
    Kind kind = Kind::System;
};
struct Report {
    std::string headline; // markdown-lite: **bold**
    std::vector<Tile> tiles;
    std::vector<ProcRow> processes;
    std::size_t process_count = 0;
    std::vector<Advice> advice;
    std::size_t readings = 0; // the monitor's readings it has taken in (report::refresh); 0: as made
};

// The detailed mode (7_chat_terminal): the task's plan as mini steps, and what its processes
// print, by tab ("PowerShell · agent"); a command's line names the plan step that ran it.
enum class PlanState : std::uint8_t { Done, Run, Wait };
struct PlanItem {
    std::string title;
    PlanState state = PlanState::Wait;
    std::string note; // "4 812", "64%"
};
struct Plan {
    std::vector<PlanItem> items;
};
struct Output {
    std::uint32_t tab = 1;
    std::string tab_label; // "PowerShell · agent"
    std::string text;      // as it came; whole lines end with '\n'
    bool command = false;  // the agent's own line ("PS C:\…> …")
    int step = -1;         // the plan step it belongs to
};

// A watcher began (it lives in the background until switched off), and what it saw.
struct Watching {
    std::string title;  // "Watching memory"
    std::string detail; // "I'll tell you if it is over 90%"
    bool on = true;
};
struct Alert {
    std::string title;  // "Memory is at 93%"
    std::string detail; // "over the 90% you set"
};

// What the task cost (phase 7): the model's usage of all its turns, priced (ai::cost_usd). A
// small line under the answer: "$0.0004 · 92% cached".
struct Spent {
    double usd = 0;
    int prompt = 0;     // input tokens of all turns
    int completion = 0;
    int cache_hit = 0;  // of the input, read from DeepSeek's cache
    bool vision = false; // pro chosen, a picture in the request: it went to flash (phase 9)
    // design/ui_model: what the answer was made with — the model ("deepseek-flash" with a picture),
    // the thinking ("max") — and the model chosen ("deepseek-v4-pro"). Empty: not said.
    std::string model;
    std::string thinking;
    std::string chosen;
};

// ---- phase 8: what a task found, as data for the panel, and what it ran

// What a data set is, for its tab's icon and its panel's tools.
enum class SetKind : std::uint8_t { Files, Matches, Processes, Table, Metrics, Approval, Translation, Terms, Terminal, Document, Text, Converted, Preview, Memory, Space, Threat };

// The threat check's report as the panel draws it (phase 13a, 05b–05g): every line made when the
// report comes, none in a frame.
struct ThreatFact {
    std::string key, value;
    bool read_only = false; // a registry key: looked at only
};
struct ThreatRow {
    int level = 0;          // 0 suspicious, 1 worth a look
    std::string title;
    std::string sub;        // its reasons in one line
    std::vector<std::string> why;
    std::vector<ThreatFact> facts;
    std::vector<std::string> actions;
};
struct ThreatStage {
    std::string name;
    std::string count;      // "612 files"
    int state = 0;          // 0 waiting, 1 done, 2 stopped
};
struct ThreatView {
    std::string verdict;    // "1 thing looks suspicious, 2 are worth a look"
    std::string caveat;     // its honest second line
    std::size_t suspicious = 0, worth = 0, fine = 0;
    std::vector<ThreatRow> rows; // the suspicious first
    std::vector<ThreatStage> stages;
    std::vector<std::string> not_read;
    std::string checked;    // "682 places"
    std::optional<int> stopped_at;
    bool defender_on = true;
    bool deeper = false;    // the helper's facts are in it (phase 13a § 6): no Check deeper again
};

// A PDF read by its pages (phase 10, screen 22): the pages read so far, each text or a scan read
// on this PC, with its text; no map when Windows' filter did not give its pages (`note` says why).
struct PageText {
    int page = 0;
    bool scan = false;
    std::string text;
};
struct Pages {
    int count = 0;
    int part_size = 12;
    bool mapped = true;
    std::string note;
    std::vector<PageText> read; // by page number
};

// A picture's text read on this PC (phase 10, screen 20): the picture, its lines with their boxes
// (parts of the picture, 0..1) and their doubtful words (bytes of the line), the profile's languages.
struct TextLine {
    std::string text;
    float x = 0, y = 0, w = 0, h = 0;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> doubtful; // [from, from + size) of `text`
};
struct LanguageChip {
    std::string name;      // its own name: "Russkiy" for Russian, in Russian
    bool used = false;     // the one it read with
    bool installed = false; // Windows can read it
};
struct TextRead {
    Picture picture;          // its id given by the model as it takes the set
    std::vector<TextLine> lines;
    std::vector<LanguageChip> languages;
    std::string language;     // the tag it read with: "ru"
    std::string file;         // "lesson_scan.jpg"
};

// What image.convert made (phase 10, screen 21), each picture said before and after.
struct ConvertedRow {
    std::string from, from_format, before; // "IMG_2041.jpg", "JPEG", "4032×3024 · 8.4 MB"
    std::string to, to_format, after;      // "… · rotated 90°", "… · already that size"
    std::string saved;                     // "−89%"
    Picture thumb;                         // the new file's
};
struct ConvertedSet {
    std::string heading; // "6 images → JPEG · 1600 px"
    std::string source;  // "image.convert · quality 85"
    std::string folder;  // where they went: Open folder
    std::string total;   // "6 files · 52.2 MB → 6.1 MB"
    std::vector<ConvertedRow> rows;
};

// A piece of a drive's bar (phase 13a, 02b): a folder, "Other" or "Free", its size as written and
// its share of the drive.
struct SpaceSlice {
    std::string name;
    std::string size;
    float share = 0;
    bool free = false;
};

struct DataSet {
    std::uint32_t id = 0;     // the task's own numbering; the same id appended to is the same set
    std::string title;        // "Downloads"
    std::string source;       // the source chip: "fs.list C:\Users\…\Downloads"
    std::string snapshot;     // "as of 14:32"
    std::string location;     // the folder it lists (Open in Explorer); empty: none
    std::vector<ui::work::Column> columns;
    ui::work::DataRows rows;
    bool complete = false;    // all its rows are in: it can be sorted
    std::string footer;       // "9 files · 177.8 MB"
    SetKind kind = SetKind::Table;
    std::optional<Pages> pages; // a Document's (phase 10)
    std::string book;           // a workbook's sheet: the book's file name, one tab for its sheets (phase 10)
    std::optional<TextRead> text_read;     // a Text set's (phase 10)
    std::optional<ConvertedSet> converted; // a Converted set's (phase 10)
    std::string preview;                   // a Preview's Markdown: the document as written (phase 10)
    bool read_only = false;                // looked at only: a read-only badge in its title (phase 13a: the services)
    // A Space set (phase 13a § 4): the whole count, the folder shown (indices down from its root) and
    // whether its folders or the largest files of all of it.
    std::shared_ptr<const tools::winsys::DiskScan> space;
    std::vector<std::uint32_t> space_path;
    bool space_files = false;
    std::vector<std::string> space_crumbs;  // "C:", "Users", … down to the folder shown
    std::vector<SpaceSlice> space_bar;      // a drive's top: its largest folders, Other, Free (02b)
    std::shared_ptr<const ThreatView> threat; // a Threat set's report (phase 13a § 5)
};
// The source of a set made from a table of the model's answer (too big for the text).
inline constexpr std::string_view kAnswerSource = "the answer";
// A data set, whole or (append) its next rows.
struct Data {
    DataSet set;
    bool append = false;
};

// One call of the task (the Activity tab): a command and what it printed, or a step inside the
// agent that ran nothing.
struct ActivityEntry {
    int number = 0;
    std::string title;   // "Read the disk's state"
    std::string purpose; // why
    std::string command; // "Get-PhysicalDisk | …"
    std::string output;
    int exit_code = 0;
    double seconds = 0;
    bool internal = false; // inside the agent: no command
    bool changed = false;  // it changed something on the machine
    std::uint32_t set = 0; // the data set it made (its "Open table"); 0: none
};
struct Activity {
    ActivityEntry entry;
};

// The agent took a request: as a follow-up of the conversation before it, or as a new one (the
// last ended long ago, or "New chat"). A new one ends the sheet's session.
struct Conversation {
    bool continues = true;
};

// ---- phase 8: translation cards (spec § 5.6), as the model's show.* tools gave them

struct Example {
    std::string text;          // in the language learnt
    std::string transcription; // pinyin, IPA, or empty
    std::string translation;   // in the user's language
    std::string highlight;     // the word as it stands in `text`
};
struct Word {
    std::string source, target; // ISO codes: the word's language, the user's
    std::string word, transcription, meaning, reference, level;
    std::vector<Example> examples;
    struct Common {
        std::string word, transcription, meaning;
    };
    std::vector<Common> common;
};
struct Phrase {
    std::string source, target;
    std::string main;  // the way to say it
    std::string where; // where it fits
    struct Variant {
        std::string tone, text; // "informal", "formal"
    };
    std::vector<Variant> variants;
    std::string why; // only with a nuance
    std::vector<Example> examples;
};
struct TextTranslation {
    std::string source, target, style; // "business", "neutral", "casual"
    struct Pair {
        std::string original, translation, transcription;
    };
    std::vector<Pair> pairs;
    std::vector<Word::Common> terms;
    bool copied = false; // the translation is on the clipboard (no question: the user's own request)
};

// What the user might ask next, as chips under the answer (the model's ui.suggest).
struct Suggestions {
    std::vector<std::string> prompts; // at most 3, each at most 60 characters
};

// The model's plan of the task (phase 12 § 3, its plan.update): sent whole each time, at most 12
// items; drawn over the steps, its count is the "step N of M".
enum class ItemState : std::uint8_t { Pending, Active, Done, Skipped };
struct ChecklistItem {
    std::string title;
    ItemState state = ItemState::Pending;
    auto operator==(const ChecklistItem&) const -> bool = default;
};
struct Checklist {
    std::vector<ChecklistItem> items;
    auto operator==(const Checklist&) const -> bool = default;
};

// The model's question with options (phase 12 § 4, its ui.ask): 2 to 4, the recommended one (its
// index) shown first and marked; several may be chosen when `multi`. The task waits for the Reply.
struct QuestionOption {
    std::string label;       // at most 40 characters
    std::string description; // at most 120
    auto operator==(const QuestionOption&) const -> bool = default;
};
struct Question {
    std::uint64_t id = 0;
    std::string question;
    std::vector<QuestionOption> options;
    std::optional<std::size_t> recommended; // an index into `options`
    bool multi = false;
};

// The agent remembered something (phase 12 § 6, memory.add): never asked, but seen — a quiet line
// under the answer, with Undo.
struct Remembered {
    std::uint64_t id = 0;
    std::string text;
    bool private_window = false; // a private windows rule (phase 13a), not a memory entry
};

// What the agent looked at in the user's window (phase 13a § 2): over the answer, always — the
// window's program and title and how (its text, or a picture of it); a sensitive one only that it
// was not looked at, and why.
struct Looked {
    std::string program, title;
    bool text = false;
    bool picture = false;
    bool sensitive = false;
    std::string reason;
};

// How much of the history's limit the last turn's prompt took (phase 12 § 10): past 50 % the
// conversation's header says it.
struct Context {
    int percent = 0;
};

// A background command of the task ended (phase 12 § 8): with the sheet closed, a notification.
struct JobDone {
    std::uint64_t job = 0;
    std::string command;
    int code = 0;
    bool stopped = false;   // ended by shell.stop or a cancel
    bool timed_out = false; // ended by the 2-hour limit
};

// How a provider stands after its last answer (stage 4b, the model menu): its rows are dimmed with
// "Add key" or "Update key", or marked busy or offline. Told by the error's kind, never its words.
enum class Trouble : std::uint8_t { None, NoKey, KeyRejected, Balance, Busy, Offline };
struct ProviderState {
    std::string provider; // the catalog's id
    Trouble trouble = Trouble::None;
};

using AgentEvent =
    std::variant<Thinking, Delta, Actions, Finished, Failed, Permission, Step, Summary, Progress, Quick, Report, Watching, Alert,
                 Plan, Output, Spent, Data, Activity, Conversation, Suggestions, Word, Phrase, TextTranslation, Picture,
                 Refusal, Created, Places, Checklist, Question, JobDone, Remembered, Context, Looked, ProviderState>;

// ---- the change log (phase 5, 5_change_log): made from changes.jsonl by journal::log_of

// A line of a task's tree, as a diff: + created, − deleted, → moved, ✎ renamed.
enum class LogOp : std::uint8_t { Context, Created, Deleted, Moved, Renamed };
struct LogRow {
    LogOp op = LogOp::Context;
    std::string name;  // "Documents", "report.pdf"
    std::string note;  // "folder", "← “old name”", "in the Recycle Bin"
    int depth = 0;
    bool folder = false;
    std::optional<std::size_t> change; // the journal change its Undo takes back; none: no Undo
    bool undone = false;
};
struct LogTask {
    std::uint64_t task = 0;
    std::string title;
    std::string when;    // local "16:24"
    int days_ago = 0;    // 0: today (local)
    std::string summary; // "11 changes", "3 in the Recycle Bin", "cannot be undone", "no changes"
    Kind kind = Kind::Read; // its dot: Change, Delete, System; Read: nothing changed
    std::size_t changes = 0;
    bool undoable = false; // something of it can still be taken back
    std::vector<LogRow> rows;
};
// Newest first.
struct Log {
    std::vector<LogTask> tasks;
};

struct Event {
    std::uint64_t task = 0;
    AgentEvent event;
};

} // namespace app::events
