#pragma once

// security::shellguard — a shell.run command's class before it runs (03-SECURITY § Shell
// commands, phase 12 § 7). A simplified reading of PowerShell: strings, `;`, `|`, `&&`, `||`,
// `$( )`, `( )`, `{ }`, `&`, the backtick as an escape. Every part of the command is classed:
//   hard-deny — a pattern no click allows (format, diskpart, -EncodedCommand, a download run...);
//   ask, every time — a pattern that changes, installs or reaches the network (Remove-Item,
//                     redirections, git push, npm install, Invoke-WebRequest...);
//   auto — only reading, from the allow-list (Get-ChildItem, Get-Content, git status|log|diff...);
//   ask — everything else, with the full command; the user may allow its start for the task.
// A construct it cannot read is asked, never auto. The paths it names go to the policy's zones:
// a command that changes something in the Windows folder is refused there (decide()).

#include "security/policy.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace security::shellguard {

// What the command does with a path it names.
enum class Use : std::uint8_t {
    Read,   // an allow-listed reading command: auto, unless the path's zone says otherwise
    List,   // an allow-listed listing (Get-ChildItem): names only
    Change, // writes, moves or deletes it
    Run,    // a command of the "everything else" class: it may do anything with it
};

struct Path {
    std::wstring path; // as written, environment variables and ~ expanded; a wildcard's folder
    Use use = Use::Run;
    bool tree = false; // a recursive delete (Remove-Item -Recurse, rd /s)
};

struct Verdict {
    Decision decision = Decision::Ask;
    std::string reason; // English: for the card, the model and the audit
    // The command and its subcommand ("git log", "npm test", "cmake --build"): what "Allow for
    // this task" allows. Empty unless the only parts asked are of the "everything else" class,
    // with this one start, read in full (an install, a delete, a network command never offer it).
    std::string prefix;
    std::vector<Path> paths;
};

[[nodiscard]] auto classify(std::string_view command) -> Verdict;

} // namespace security::shellguard
