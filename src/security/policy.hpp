#pragma once

// security::policy — whether a tool call runs, is asked about or is refused (03-SECURITY § The decision
// matrix). The policy decides, never the model: a question is answered only by a click in the
// UI thread (grant() in permit.hpp), and nothing changes the machine without a Permit.
//
// decide() canonicalizes every path itself — a tool cannot hand it a path it has not seen — and
// the ruling carries those canonical paths: they are what the question shows and what the Permit
// is valid for.

#include "security/paths.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace security {

// Screen (phase 9): the screen or a window shown to the model — asked every time, never remembered.
// Startup (phase 13a § 3): a program off or back on at startup — asked every time, never allowed for
// a task or a session. Uninstall (§ 4): a program removed by its own uninstaller, the same.
// Quarantine (§ 5.4): a file into the agent's quarantine, back, or deleted from it for good, the same.
enum class Risk : std::uint8_t { Read, Write, RenameMove, Delete, Execute, ProcessKill, Network, Clipboard, Screen, Look, Startup, Uninstall, Quarantine };
enum class Decision : std::uint8_t { Auto, Ask, Deny, HardDeny };
auto risk_name(Risk r) -> std::string_view;
auto decision_name(Decision d) -> std::string_view;

// What a tool call is about to do, with the parameters it will really use.
// How far a tool counts the objects inside a folder before it stops: enough for any limit a
// person would set, and short enough not to walk a whole disk for one question.
inline constexpr std::uint64_t kCountObjectsUpTo = 100'000;

// One of the things a call may be narrowed to on its card (phase 13a, 06b: a program's versions):
// its name and version, a line about it, what it affects.
struct Choice {
    std::string name;
    std::string version;
    std::string detail; // "installed 8 Aug 2024 · in use"
    std::string where;  // its folder and size
    auto operator==(const Choice&) const -> bool = default;
};

struct Request {
    std::string tool;
    Risk risk = Risk::Read;
    std::vector<std::wstring> paths;   // as the call names them; canonicalized by decide()
    std::filesystem::path working_dir; // the task's folder: relative paths are inside it
    std::string command;               // Execute: the full command
    std::uint32_t pid = 0;             // ProcessKill
    std::string process_name;          // ProcessKill: the image name ("notepad.exe")
    bool process_system = false;       // ProcessKill: runs as SYSTEM or as a service
    std::uint64_t process_started = 0; // ProcessKill: its creation time (FILETIME): a reused id is another process
    std::wstring process_path;         // ProcessKill: its file (phase 13a § 5.4); empty: not known
    bool microsoft_signed = false;     // ProcessKill: its file is signed by Microsoft
    std::uint64_t bytes = 0;           // Write: bytes to be written
    std::uint64_t tree_objects = 0;    // RenameMove/Delete: objects inside the folders it moves or deletes
    bool permanent = false;            // Delete: not to the Recycle Bin (fs.delete_permanent)
    bool opens = false;                // Execute: system.open — `command` a URL, or `paths` one file
    bool background = false;           // Execute: shell.run in the background (phase 12 § 8): the card says so
    std::string must_ask;              // a reason this call is asked whatever else allows it (phase 12: an undo over later changes)
    // Startup (phase 13a § 3): where the entry is written ("run", "scheduled task", "Winlogon", as
    // startup.list names it), its name (a task's path), the key or folder it is in, and whether it
    // is the whole computer's (HKLM, the common folder, a task of another account, WMI).
    // Uninstall (§ 4): place "program", "system component", "Windows component" or "the agent
    // itself"; entry its name and version; where its folder and size, for the card.
    std::string place;
    std::string entry;
    std::string where;
    bool machine_wide = false;
    bool back_on = false;              // Startup: switched back on (the Undo), not off
    std::string version;               // Uninstall: the program's version
    // Uninstall (06b): the programs the call may be narrowed to, the first asked about until the
    // user picks; empty: the call names one.
    std::vector<Choice> choices;
    // Quarantine (§ 5.4): the autostart entries switched off by the same yes, as the card names them;
    // `place` names what points to the file that may not lose it ("Winlogon", "service"), empty: none.
    std::vector<std::string> along;

    auto operator==(const Request&) const -> bool = default;
};

// config.json "advanced" and "limits", and how the agent runs.
struct Settings {
    bool allow_system_writes = false;
    bool allow_secrets_read = false;
    bool allow_permanent_delete = false;
    std::uint32_t max_files = 50;            // files changed without a confirmation in between
    std::uint64_t max_bytes = 200ull << 20;  // bytes written without a confirmation in between
    std::uint32_t max_trash_batch = 100;
    std::uint32_t max_tree = 1000;
    std::uint32_t max_calls = 25;            // tool calls without a confirmation in between
    bool agent_elevated = false;             // the agent itself runs as administrator
    // Access levels (03-SECURITY § Access levels), only ever stricter than the matrix: reads in
    // the user's folders asked too; creating and moving allowed once per folder, or never.
    bool read_asks = false;
    bool change_per_folder = true;
};

struct Ruling {
    Decision decision = Decision::Deny;
    std::string reason;                // English, for the question card, the model and the audit
    std::optional<paths::Zone> zone;   // the strictest zone of the paths (or of the working folder)
    std::vector<paths::Canonical> paths;
    std::vector<paths::Zone> zones;    // one per path
    bool advanced = false;             // possible only by an advanced flag: the UI asks for the word first
    bool warning = false;              // asked in red: the agent runs as administrator
    bool per_folder = true;            // Settings::change_per_folder when it was decided
    // shell.run (phase 12 § 7): the command's start the user may allow for this task ("npm test"),
    // when it is asked and may be; and whether an allowed start made it auto (audited "session").
    std::string task_prefix;
    bool by_session = false;
    Request request;                   // what was decided: grant() takes no other
};

// One task's state: folders allowed for the session and what was done since the last click.
class Session {
public:
    // "Allow in this folder for this session": for a write or a move in User whose paths are all
    // in one folder (or in folders this task made inside it), never a delete; not at all when
    // the access level is "always".
    auto allow_folder(const Ruling& ruling) -> bool;
    // That folder itself, or a folder this task made inside one allowed.
    [[nodiscard]] auto allowed(const paths::Canonical& path) const -> bool;
    // A folder this task made (fs.mkdir's effect), as its final path.
    void created(std::wstring folder);
    // "Allow `npm test` for this task" (phase 12 § 7): the ruling's start, when it offered one.
    auto allow_command(const Ruling& ruling) -> bool;
    [[nodiscard]] auto allowed_command(std::string_view prefix) const -> bool;
    [[nodiscard]] auto commands() const noexcept -> const std::vector<std::string>& { return commands_; } // as allowed
    // The user's own words in this task (its request, what was typed while it ran). A web page
    // they name opens without a question; any other the model makes up is asked (the user,
    // 2026-10-16: it opened searches made of a test's words).
    void heard(std::string_view words);
    // `url` (http or https) is one the user wrote: without its scheme, "www." and a trailing "/",
    // it stands in their words where a word ends (a site or the start of an address they gave).
    [[nodiscard]] auto named(std::string_view url) const -> bool;

    // A granted call: counted towards the limits; a confirmation starts the count over.
    void note(const Request& request, bool confirmed);
    [[nodiscard]] auto calls() const noexcept -> std::uint32_t { return calls_; }
    [[nodiscard]] auto files() const noexcept -> std::uint64_t { return files_; }
    [[nodiscard]] auto bytes() const noexcept -> std::uint64_t { return bytes_; }

private:
    [[nodiscard]] auto in_folder(const std::wstring& parent, const std::wstring& folder) const -> bool;

    std::vector<std::wstring> folders_;
    std::vector<std::wstring> created_;
    std::vector<std::string> commands_;
    std::string heard_; // the user's words: lower case, without "http://", "https://" and "www."
    std::uint32_t calls_ = 0;
    std::uint64_t files_ = 0;
    std::uint64_t bytes_ = 0;
};

auto decide(const Request& request, const Settings& settings, const Session& session,
            const paths::Roots& roots = paths::system_roots()) -> Ruling;

} // namespace security
