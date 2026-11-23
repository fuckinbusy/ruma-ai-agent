#pragma once

// tools — the agent's tools (05-AI-AGENT § Tools: the protocol): a static set, each one a risk
// class, a JSON Schema of its arguments and two functions:
//
//   request(args, working folder) -> what it is about to do, for security::decide();
//   run(permit, args)             -> the result, for the model.
//
// run() acts only on what the Permit names (its canonical paths, never the raw arguments) and
// only through guarded_fs / tools::os. The result is wrapped with its provenance —
// {"source":"fs.read","path":"…","data":…} — and is at most 64 KB, "truncated":true when cut
// (03-SECURITY § Prompt-injection).

#include "core/error.hpp"
#include "security/permit.hpp"
#include "tools/image.hpp"
#include "tools/ocr.hpp"
#include "tools/winsys/disk.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace tools::jobs {
class Registry; // tools/shell_jobs.hpp
} // namespace tools::jobs

namespace tools::winsys {
struct CheckReport; // tools/winsys/check.hpp
} // namespace tools::winsys

namespace tools {

inline constexpr std::size_t kMaxOutput = 64 * 1024;

// What a call changed, for the change log and its undo (phase 5): a created folder or file, a
// move or a rename (from -> to), a trip to the Recycle Bin and back, and what cannot be taken
// back — a permanent delete, an overwrite, an ended process, a command that ran. An edit (phase
// 12, fs.edit) goes back: what the file held is kept with it.
//
// Phase 13a (§ 3): a program off at startup — its Run value or shortcut (StartupOff), a scheduled
// task (TaskOff), a WMI subscription kept as MOF (WmiTaken) — and one of them back on (SwitchedOn,
// its Undo's own effect).
enum class EffectKind : std::uint8_t {
    Created, Moved, Renamed, Trashed, Restored, Deleted, Overwritten, Killed, Ran, Edited,
    StartupOff, TaskOff, WmiTaken, SwitchedOn,
    Uninstalled, // a program removed by its own uninstaller (§ 4): no way back
    Quarantined, // a file moved into the agent's quarantine (§ 5.4): from its place to its folder there
    Released     // …and put back (its Undo's own effect)
};
struct Effect {
    EffectKind kind = EffectKind::Created;
    // Moved/Renamed: where it was; Killed: the program; Ran: the command; StartupOff, TaskOff,
    // WmiTaken, SwitchedOn: the entry's name as the user knows it.
    std::wstring from;
    // where it is now (Created, Moved, Renamed, Trashed: the original path, Restored); StartupOff:
    // "<place>\n<where>\n<name>"; TaskOff: the task's path; WmiTaken: the MOF file; SwitchedOn: the
    // `to` of what it switched back on.
    std::wstring to;
    bool undoable = false;
    std::string before; // Edited: the file's bytes before it (the agent keeps them for Undo)
};

// A table a tool read (phase 9: a workbook's sheet, a CSV): for the data panel, beside the JSON.
struct DataTable {
    std::string name;
    std::vector<std::vector<std::string>> rows; // the first is the header
};

// A PDF read by its pages, for the panel's map (phase 10, screen 22): which pages this read had,
// each as text or as a scan read here, and its text.
struct PageRead {
    int page = 0;
    bool scan = false;
    std::string text;
};
struct DocumentPages {
    std::wstring path;
    int pages = 0;
    int part_size = 12;
    bool mapped = true; // the filter's pieces are its pages; false: no map, `note` says why
    std::string note;
    std::vector<PageRead> read;
};

// A picture's text read here, for the panel (phase 10, screen 20): the picture smaller, its lines
// with their boxes in its own pixels, and the profile's languages.
struct OcrPage {
    std::wstring path;
    Picture picture; // at most 640 on a side
    int width = 0, height = 0; // the picture's own pixels: the boxes' frame
    OcrText text;
    std::vector<ProfileLanguage> languages;
};

// What image.convert made of each picture (phase 10, screen 21): before and after.
struct ConvertedFile {
    std::wstring from, to;
    std::string from_format, to_format; // "JPEG", "PNG"
    int from_w = 0, from_h = 0;         // the file's own pixels, as stored
    int to_w = 0, to_h = 0;
    std::uint64_t from_bytes = 0, to_bytes = 0;
    int rotated = 0;      // clockwise degrees: its EXIF turn and the one asked, together
    bool already = false; // within max_side already: not made smaller (never larger)
    Picture thumb;        // the result, at most 80 on a side
};
struct Conversion {
    std::string format; // as asked: "jpeg"; empty: each kept its own
    int max_side = 0;   // 0: the size as it was
    int quality = 85;
    std::wstring folder; // where the new files went (the first's)
    std::vector<ConvertedFile> files;
};

// What a call left out and why (phase 10, screen 25): a sheet or a page past the limit; the rest was read.
struct Refusal {
    std::string kind;  // "sheet_too_large", "page_too_large"
    std::string what;  // "sheet «Data»", "page 2"
    std::string facts; // "1 048 576 rows × 16 384 columns", "page size 200 × 200 m"
};

struct Outcome {
    int code = 0;     // 0 done; 2 failed (arguments, not found, I/O); 3 refused (policy, guard)
    std::string json; // the wrapped result, or {"source":…,"error":…}
    std::vector<Effect> effects; // what was done, also when a batch stopped part way
    std::vector<ImageOut> images; // pictures for the model (phase 9): a user message after the results
    std::vector<DataTable> tables; // for the panel: too large for the JSON the model gets (phase 9)
    std::optional<DocumentPages> document; // a PDF's pages, for the panel (phase 10)
    std::optional<OcrPage> ocr;            // image.ocr, for the panel (phase 10)
    std::optional<Conversion> conversion;  // image.convert, for the panel (phase 10)
    std::vector<Refusal> skipped;          // left out, the rest read (phase 10)
    std::shared_ptr<const winsys::DiskScan> disk; // disk.usage's whole tree, for the panel (phase 13a)
    std::shared_ptr<const winsys::CheckReport> check; // system.check's report, for the panel (phase 13a)
};

// How far a batch has come.
struct ProgressInfo {
    std::uint64_t done = 0;  // objects finished
    std::uint64_t total = 0;
    std::uint64_t bytes = 0;
    std::wstring current;    // the one just finished
    std::optional<std::chrono::seconds> left; // a wait whose length is known: what is left of it
    std::shared_ptr<const winsys::DiskScan> disk; // disk.usage: the tree so far, counting (phase 13a)
};

// The running call's controls (tools::call sets them for its thread): the stop button and the
// pause, ahead of each object of a batch; progress after each; a command's output as it comes.
struct Control {
    std::stop_token stop;
    const std::atomic<bool>* paused = nullptr;
    std::function<void(const ProgressInfo&)> progress;
    std::function<void(std::string_view)> output; // from another thread
    std::function<std::chrono::steady_clock::time_point()> clock; // a test's own; none: the steady clock
    jobs::Registry* jobs = nullptr; // shell.run background: where its commands run (phase 12 § 8)
    std::uint64_t task = 0;          // …and whose they are
    bool agents_own = false;         // the agent's own call (an Undo): its internal tools are found
};

// The controls of the call running on this thread; none outside a call.
[[nodiscard]] auto control() noexcept -> const Control&;
class ControlScope {
public:
    explicit ControlScope(const Control& c) noexcept;
    ~ControlScope();
    ControlScope(const ControlScope&) = delete;
    auto operator=(const ControlScope&) -> ControlScope& = delete;

private:
    const Control* previous_;
};
// Ahead of each object of a batch: waits while paused; false once the call is stopped.
[[nodiscard]] auto proceed() -> bool;
// The call's clock: the steady clock, or a test's own (Control::clock).
[[nodiscard]] auto now() -> std::chrono::steady_clock::time_point;
// After each object of a batch.
void report(std::uint64_t done, std::uint64_t total, std::wstring_view current, std::uint64_t bytes = 0,
            std::optional<std::chrono::seconds> left = std::nullopt);
// The error of a batch the user stopped.
[[nodiscard]] auto stopped_error() -> core::Error;

// What a tool made before the question (phase 9, screen.capture: the picture the user sees on
// the card and the model gets only after a yes).
struct Prepared {
    std::optional<ImageOut> image;
    std::string title;
    // Phase 10 (screen 19), screen.capture: a larger picture for the panel (at most 864 x 432),
    // where it came from, the monitors and windows there are, and where the agent's window is on
    // it (parts of the picture, 0..1; w 0: not on it).
    Picture preview;
    std::string source;
    int monitors = 1;
    std::vector<std::string> windows;
    float hole_x = 0, hole_y = 0, hole_w = 0, hole_h = 0;
};

// The user's "take it again" on a screen question (phase 10, screen 19): another monitor, all of
// them, a window — or the same again ("Refresh", after closing what should not be seen).
struct Retake {
    enum class What : std::uint8_t { Same, Monitor, All, Window };
    What what = What::Same;
    int monitor = 0;    // from 1
    std::string window; // its title
};

struct Tool {
    std::string_view name;
    std::string_view description;
    security::Risk risk;
    auto (*schema)() -> std::string;
    auto (*request)(std::string_view args, const std::filesystem::path& working_dir) -> core::Result<security::Request>;
    auto (*run)(const security::Permit& permit, std::string_view args) -> Outcome;
    // What the question card shows besides the paths (a diff, a list of moves); nullptr: nothing.
    auto (*preview)(std::string_view args, const security::Request& request) -> std::string = nullptr;
    // The same call with only the objects `keep` names (indices into its list of paths or pairs):
    // what the user left ticked on the card. An index past the list is an error. nullptr: the
    // call is one object and cannot be narrowed.
    auto (*narrow)(std::string_view args, std::span<const std::size_t> keep) -> core::Result<std::string> = nullptr;
    // Made before the policy decides (the question shows it); a failure ends the call before any
    // question. With it, run_prepared runs instead of run, with what was made.
    auto (*prepare)(std::string_view args) -> core::Result<Prepared> = nullptr;
    auto (*run_prepared)(const security::Permit& permit, std::string_view args, Prepared&& prepared) -> Outcome = nullptr;
    // Made again as the user asks before answering (phase 10): another of what `prepare` makes.
    auto (*prepare_again)(std::string_view args, const Retake& retake) -> core::Result<Prepared> = nullptr;
};

// Every tool there is (schemas, tests).
auto all() -> std::span<const Tool>;
// What the model may use with these settings: fs.delete_permanent only with
// advanced.allow_permanent_delete.
auto available(const security::Settings& settings) -> std::vector<const Tool*>;
auto find(std::string_view name, const security::Settings& settings = {}) -> const Tool*;
// The agent's own tools (an Undo's fs.revert): never the model's to see or to call.
[[nodiscard]] auto internal(std::string_view name) -> bool;

// The envelope around `data_json` (a JSON value already within the budget).
auto wrap(std::string_view tool, std::wstring_view path, std::string_view data_json, bool truncated) -> std::string;
// {"source":tool,"error":message}; code 3 for core::Code::Policy, else 2.
auto failure(std::string_view tool, const core::Error& error) -> Outcome;
// A batch that stopped part way: what is done stays done, so the model is told exactly —
// {"source","error","done":<done_json>,"failed":path,"not_started":[paths]}; codes as failure().
// `not_started` is cut to kMaxPartialList paths ("truncated":true).
inline constexpr std::size_t kMaxPartialList = 200;
auto partial(std::string_view tool, const core::Error& error, std::string_view done_json, std::wstring_view failed,
             std::span<const std::wstring> not_started) -> Outcome;

} // namespace tools
