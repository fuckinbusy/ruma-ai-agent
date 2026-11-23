#include "tools/tool.hpp"

#include "core/json.hpp"
#include "core/utf8.hpp"
#include "tools/doc_tools.hpp"
#include "tools/fs.hpp"
#include "tools/fs_edit.hpp"
#include "tools/fs_write.hpp"
#include "tools/image_tools.hpp"
#include "tools/shell.hpp"
#include "tools/system.hpp"
#include "tools/trash.hpp"
#include "tools/winsys_tools.hpp"

#include <glaze/glaze.hpp>

#include <array>
#include <chrono>
#include <thread>

namespace tools {
namespace {

using security::Risk;

const std::array kAll{
    Tool{"fs.list", "List a folder's files and subfolders (with sizes and dates).", Risk::Read, &fs::list_schema,
         &fs::list_request, &fs::list_run},
    Tool{"fs.read", "Read a text file (or a range of its lines). Binary files give only their size.", Risk::Read,
         &fs::read_schema, &fs::read_request, &fs::read_run},
    Tool{"fs.stat", "Size, dates, attributes and type (file, folder, junction, symlink) of a path.", Risk::Read,
         &fs::stat_schema, &fs::stat_request, &fs::stat_run},
    Tool{"fs.search", "Find files in a folder and its subfolders by name pattern and/or by the text they contain.",
         Risk::Read, &fs::search_schema, &fs::search_request, &fs::search_run},
    Tool{"fs.write", "Write a text file: a new one, or overwrite or append to an existing one.", Risk::Write,
         &fs::write_schema, &fs::write_request, &fs::write_run, &fs::write_preview},
    Tool{"fs.edit",
         "Change an existing text file: replace exact pieces of it (all or none). Prefer it to rewriting a file with fs.write.",
         Risk::Write, &fs::edit_schema, &fs::edit_request, &fs::edit_run, &fs::edit_preview},
    Tool{"fs.revert", "Put back what a file held before an edit (the agent's Undo).", Risk::Write, &fs::revert_schema,
         &fs::revert_request, &fs::revert_run, &fs::revert_preview},
    Tool{"fs.mkdir", "Create a folder (and its missing parents).", Risk::Write, &fs::mkdir_schema, &fs::mkdir_request,
         &fs::mkdir_run},
    Tool{"fs.move", "Move or rename files and folders, several at once.", Risk::RenameMove, &fs::move_schema,
         &fs::move_request, &fs::move_run, &fs::move_preview, &fs::move_narrow},
    Tool{"fs.copy", "Copy files or whole folders, several at once.", Risk::Write, &fs::copy_schema, &fs::copy_request,
         &fs::copy_run, &fs::copy_preview, &fs::copy_narrow},
    Tool{"fs.trash", "Move files or folders to the Recycle Bin (never deleted for good). fs.restore puts them back.",
         Risk::Delete, &trash::schema, &trash::trash_request, &trash::trash_run, &trash::trash_preview, &trash::narrow},
    Tool{"fs.restore", "Put files or folders moved to the Recycle Bin back where they were (give the original paths).",
         Risk::Write, &trash::schema, &trash::restore_request, &trash::restore_run},
    Tool{"system.info", "The computer now: processor and its load, memory, disks with free space, graphics cards, "
                        "uptime, Windows version, DPI.",
         Risk::Read, &sys::no_args_schema, &sys::info_request, &sys::info_run},
    Tool{"process.list", "Running programs with their memory and processor use, the biggest first.", Risk::Read,
         &sys::processes_schema, &sys::processes_request, &sys::processes_run},
    Tool{"window.active", "The window the user is working in: its title and program.", Risk::Read,
         &sys::no_args_schema, &sys::window_request, &sys::window_run},
    Tool{"startup.list", "What starts with Windows: Run keys, startup folders, Winlogon, IFEO, AppInit, Active Setup, LSA, "
                         "BootExecute — each with its program, where it is written and whether it is switched on.",
         Risk::Read, &winsys::empty_schema, &winsys::startup_request, &winsys::startup_run},
    Tool{"service.list", "Windows' services with their state, start type, program and account. Looked at only: no tool can "
                         "start, stop or change a service.",
         Risk::Read, &winsys::empty_schema, &winsys::services_request, &winsys::services_run},
    Tool{"task.list", "The scheduled tasks the user can see: path, triggers, what each runs, as whom, switched on or off.",
         Risk::Read, &winsys::empty_schema, &winsys::tasks_request, &winsys::tasks_run},
    Tool{"file.signature", "Whether a file is signed, by whom, whether it is Microsoft's, and its SHA-256.", Risk::Read,
         &winsys::signature_schema, &winsys::signature_request, &winsys::signature_run},
    Tool{"process.sample", "What each program uses over some seconds (processor and graphics card, a point a second; memory): "
                           "the busiest 50, with their files and whether they have a window. seconds 1-60, 10 if null.",
         Risk::Read, &winsys::sample_schema, &winsys::sample_request, &winsys::sample_run},
    Tool{"net.connections", "Who talks to the network: each connection and listening port with its program; the hosts file "
                            "beyond localhost, the proxy, the DNS servers.",
         Risk::Read, &winsys::empty_schema, &winsys::connections_request, &winsys::connections_run},
    Tool{"eventlog.query", "What crashed lately, from Windows' logs: programs that crashed or hung, services that died, "
                           "unclean shutdowns — grouped and counted. days 1-30, 7 if null.",
         Risk::Read, &winsys::events_schema, &winsys::events_request, &winsys::events_run},
    Tool{"window.look", "Look at the window the user was working in before they opened you: as text (its buttons, fields, "
                        "labels — first choice) or as a picture (when text is not enough: graphics, a canvas, a video). The "
                        "user sees that you looked. A sensitive window (passwords, banking, private browsing) is never shown "
                        "to you: then say so plainly.",
         Risk::Look, &winsys::look_schema, &winsys::look_request, &winsys::look_run},
    Tool{"disk.usage", "What takes the space under a folder (the system drive if path is null): its folders by size two "
                       "levels down, its largest files, the drive's size and free space. Counts for tens of seconds on a whole "
                       "drive, with progress the user sees; the user sees the whole tree in the panel. Only looks: to clean up, "
                       "use fs.trash on a separate request.",
         Risk::Read, &winsys::disk_schema, &winsys::disk_request, &winsys::disk_run},
    Tool{"system.check",
         "Check the computer for threats: what starts with Windows (Run, startup folders, Winlogon, IFEO, AppInit, LSA, "
         "BootExecute, policies), services and drivers, scheduled tasks, WMI subscriptions, the signatures of their files and "
         "of running programs, the network (connections, hosts, proxy, DNS), Microsoft Defender, and the load while idle (30 s). "
         "Fixed rules mark what looks suspicious or is worth a look, each with its reasons. Tell the user the verdict as it is "
         "given — never \"no viruses\". from_step 2-8 continues a check that was stopped.",
         Risk::Read, &winsys::check_schema, &winsys::check_request, &winsys::check_run},
    Tool{"file.quarantine",
         "Move a suspicious file into the agent's own quarantine, where it cannot run; the autostart entries and tasks that "
         "start it are switched off by the same yes. why: the finding's reason. Asked every time; Undo in the log puts it "
         "back. Never Windows' files, Microsoft-signed ones, the agent's own, or a file Winlogon, IFEO, AppInit, LSA or a "
         "service needs. A running file: end it with process.kill first.",
         Risk::Quarantine, &winsys::quarantine_schema, &winsys::quarantine_request, &winsys::quarantine_run},
    Tool{"file.release", "Put a file back from the agent's quarantine (the agent's Undo).", Risk::Quarantine, &winsys::release_schema,
         &winsys::release_request, &winsys::release_run},
    Tool{"quarantine.delete", "Delete a file from the agent's quarantine for good, by its folder there. Asked; cannot be undone.",
         Risk::Quarantine, &winsys::release_schema, &winsys::quarantine_delete_request, &winsys::quarantine_delete_run},
    Tool{"defender.scan",
         "Ask Microsoft Defender to scan one file and say its verdict. Asked every time: a threat it finds, Defender moves into "
         "its own quarantine (restored in Windows Security > Protection history). Not available when Defender is off.",
         Risk::Quarantine, &winsys::defender_scan_schema, &winsys::defender_scan_request, &winsys::defender_scan_run},
    Tool{"app.uninstall",
         "Remove a program with its own uninstaller, in its usual window (never silently). name as apps.list gives it; version "
         "when two share the name — if more than one matches you get the list: ask the user which. Asked every time; it "
         "cannot be undone. Waits for the uninstaller, then says \"removed\", \"still installed\" or \"the uninstaller is "
         "still open\": say exactly that. A Store app: Settings opens on its page. Windows' parts and the agent itself never.",
         Risk::Uninstall, &winsys::uninstall_schema, &winsys::uninstall_request, &winsys::uninstall_run, nullptr,
         &winsys::uninstall_narrow},
    Tool{"startup.disable",
         "Switch a program off at startup, the way Task Manager does — nothing is deleted, the user can turn it back on. "
         "place and name as startup.list gives them (where too when two share a name); a scheduled task: place "
         "\"scheduled task\", name its path from task.list; a WMI subscription: place \"WMI subscription\", name its filter. "
         "Asked every time. Winlogon, IFEO, AppInit, LSA, BootExecute and services are never changed: tell the user how "
         "to do it themselves.",
         Risk::Startup, &winsys::disable_schema, &winsys::disable_request, &winsys::disable_run},
    Tool{"startup.enable", "Turn an entry switched off by startup.disable back on (the agent's Undo).", Risk::Startup,
         &winsys::enable_schema, &winsys::enable_request, &winsys::enable_run},
    Tool{"task.enable", "Turn a scheduled task switched off by startup.disable back on (the agent's Undo).", Risk::Startup,
         &winsys::task_enable_schema, &winsys::task_enable_request, &winsys::task_enable_run},
    Tool{"wmi.restore", "Put back a WMI subscription startup.disable took off, from its MOF (the agent's Undo).", Risk::Startup,
         &winsys::wmi_restore_schema, &winsys::wmi_restore_request, &winsys::wmi_restore_run},
    Tool{"apps.list","The installed programs: name, version, publisher, date, size (none when the program does not say), "
                      "Store apps too unless store is false.",
         Risk::Read, &winsys::apps_schema, &winsys::apps_request, &winsys::apps_run},
    Tool{"clipboard.read", "What the user copied: text or a list of files. Asked about every time.", Risk::Clipboard,
         &sys::no_args_schema, &sys::clipboard_request, &sys::clipboard_run},
    Tool{"clipboard.write", "Put text on the clipboard. Asked about every time.", Risk::Clipboard,
         &sys::clipboard_write_schema, &sys::clipboard_write_request, &sys::clipboard_write_run,
         &sys::clipboard_write_preview},
    Tool{"system.open", "Open a web page in the browser, a document in its program, or a folder in Explorer.",
         Risk::Execute, &sys::open_schema, &sys::open_request, &sys::open_run},
    Tool{"shell.run", "Run a PowerShell command and get its output and exit code. Only reading commands (Get-ChildItem, "
                      "Get-Content, Select-String, git status|log|diff...) run unasked; any other is shown to the user in "
                      "full, who decides. Never hide what a command does.",
         Risk::Execute, &shell::run_schema, &shell::run_request, &shell::run_run},
    Tool{"process.kill", "End a running program (asked; Windows' own processes never).", Risk::ProcessKill,
         &shell::kill_schema, &shell::kill_request, &shell::kill_run},
    Tool{"image.view", "Look at a picture file (a photo, a screenshot, a scan, a diagram): you are shown it in the next "
                       "message. detail \"high\" for small text or fine detail.",
         Risk::Read, &pictures::view_schema, &pictures::view_request, &pictures::view_run},
    Tool{"image.ocr", "Read the text in a picture (a scan, a photo of a page, a screenshot) on this machine, exactly; "
                      "nothing is sent. Better than looking for long or exact text. Words Windows may have misread come back "
                      "as \"doubtful\": say so.",
         Risk::Read, &pictures::ocr_schema, &pictures::ocr_request, &pictures::ocr_run},
    Tool{"image.convert", "Make smaller, convert or turn pictures (JPEG, PNG, BMP, TIFF, GIF), several at once. The "
                          "originals are never changed: new files go to a folder \"converted\" beside them (or the folder asked), "
                          "without their EXIF or location.",
         Risk::Write, &pictures::convert_schema, &pictures::convert_request, &pictures::convert_run},
    Tool{"doc.read", "Read a document: PDF (a scan is read by OCR on this machine), DOCX, XLSX (its sheets also go to the "
                     "user's data panel), PPTX, CSV, TXT, MD. A long one comes in parts: ask again from next_offset.",
         Risk::Read, &documents::read_schema, &documents::read_request, &documents::read_run},
    Tool{"doc.write", "Create a document: DOCX from Markdown, XLSX or CSV from a table, or MD. Never writes over an "
                      "existing file.",
         Risk::Write, &documents::write_schema, &documents::write_request, &documents::write_run},
    Tool{"screen.capture", "Look at the user's screen or one window: you are shown it in the next message. The user sees "
                           "the picture and says yes or no every time.",
         Risk::Screen, &pictures::capture_schema, &pictures::capture_request, &pictures::capture_run, nullptr, nullptr,
         &pictures::capture_prepare, &pictures::capture_run_prepared, &pictures::capture_prepare_again},
    Tool{"fs.delete_permanent", "Delete files or folders FOR GOOD, not to the Recycle Bin. Only in advanced mode; "
                                "always asked. Prefer fs.trash.",
         Risk::Delete, &trash::schema, &trash::delete_request, &trash::delete_run, &trash::delete_preview, &trash::narrow},
};

// Tools that exist only when an advanced flag is on.
auto hidden(const Tool& t, const security::Settings& settings) -> bool {
    return t.name == "fs.delete_permanent" && !settings.allow_permanent_delete;
}

auto json_string(std::string_view s) -> std::string {
    auto j = core::json::write(s);
    return j ? std::move(*j) : std::string{"\"?\""};
}

} // namespace

auto all() -> std::span<const Tool> {
    return kAll;
}

auto available(const security::Settings& settings) -> std::vector<const Tool*> {
    std::vector<const Tool*> out;
    for (const auto& t : kAll) {
        if (!hidden(t, settings) && !internal(t.name)) {
            out.push_back(&t);
        }
    }
    return out;
}

auto find(std::string_view name, const security::Settings& settings) -> const Tool* {
    for (const auto& t : kAll) {
        if (t.name == name && !hidden(t, settings)) {
            return &t;
        }
    }
    return nullptr;
}

// Found by name for the agent's own calls (Tools::undo), never offered to the model (available) nor
// run for it (Tools::run, final review E1).
auto internal(std::string_view name) -> bool {
    return name == "fs.revert" || name == "startup.enable" || name == "task.enable" || name == "wmi.restore" || name == "file.release";
}

auto wrap(std::string_view tool, std::wstring_view path, std::string_view data_json, bool truncated) -> std::string {
    auto p = core::utf8::to_utf8(path);
    std::string out = R"({"source":)" + json_string(tool);
    if (!path.empty()) {
        out += R"(,"path":)" + json_string(p ? *p : std::string{"?"});
    }
    out += R"(,"data":)";
    out += data_json;
    if (truncated) {
        out += R"(,"truncated":true)";
    }
    out += '}';
    return out;
}

namespace {
// A refusal's kind and facts after the error (phase 10), when it has them.
auto kind_json(const core::Error& error) -> std::string {
    if (error.kind.empty()) {
        return {};
    }
    return R"(,"kind":)" + json_string(error.kind) + R"(,"facts":)" + json_string(error.facts);
}
} // namespace

auto failure(std::string_view tool, const core::Error& error) -> Outcome {
    return {error.code == core::Code::Policy ? 3 : 2,
            R"({"source":)" + json_string(tool) + R"(,"error":)" + json_string(error.message) + kind_json(error) + "}"};
}

namespace {
const Control kNoControl{};
thread_local const Control* g_control = nullptr;
} // namespace

auto control() noexcept -> const Control& {
    return g_control != nullptr ? *g_control : kNoControl;
}

ControlScope::ControlScope(const Control& c) noexcept : previous_(g_control) {
    g_control = &c;
}

ControlScope::~ControlScope() {
    g_control = previous_;
}

auto proceed() -> bool {
    const Control& c = control();
    while (c.paused != nullptr && c.paused->load(std::memory_order_acquire) && !c.stop.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    return !c.stop.stop_requested();
}

auto now() -> std::chrono::steady_clock::time_point {
    const Control& c = control();
    return c.clock ? c.clock() : std::chrono::steady_clock::now();
}

void report(std::uint64_t done, std::uint64_t total, std::wstring_view current, std::uint64_t bytes,
            std::optional<std::chrono::seconds> left) {
    if (const Control& c = control(); c.progress) {
        c.progress({done, total, bytes, std::wstring{current}, left});
    }
}

auto stopped_error() -> core::Error {
    return {core::Code::Cancelled, "stopped by the user"};
}

auto partial(std::string_view tool, const core::Error& error, std::string_view done_json, std::wstring_view failed,
             std::span<const std::wstring> not_started) -> Outcome {
    const auto utf8 = [](std::wstring_view w) {
        auto s = core::utf8::to_utf8(w);
        return s ? std::move(*s) : std::string{"?"};
    };
    std::string out = R"({"source":)" + json_string(tool) + R"(,"error":)" + json_string(error.message) + kind_json(error) +
                      R"(,"done":)" + std::string{done_json.empty() ? "[]" : done_json} + R"(,"failed":)" +
                      json_string(utf8(failed)) + R"(,"not_started":[)";
    const std::size_t listed = std::min(not_started.size(), kMaxPartialList);
    for (std::size_t i = 0; i < listed; ++i) {
        out += (i == 0 ? "" : ",") + json_string(utf8(not_started[i]));
    }
    out += ']';
    if (listed < not_started.size()) {
        out += R"(,"truncated":true)";
    }
    out += '}';
    return {error.code == core::Code::Policy ? 3 : 2, std::move(out)};
}

} // namespace tools
