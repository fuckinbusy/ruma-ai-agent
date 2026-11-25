#pragma once

// setup::flow — the installer's screens without a window (design/ui_installer, docs/specs/installer.md):
// which screen shows, what a button or key does there, and what the program must do about it
// (a Command for the window: start the work, pick a folder, quit). The painter draws a State;
// the window turns its clicks and keys into Actions.

#include "setup/model.hpp"
#include "setup/steps.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace setup {

enum class Screen : std::uint8_t {
    Welcome,    // first install
    Options,
    Installing, // install or update at work
    Done,
    Installed,  // the same or a newer version is there: nothing to do
    Update,     // an older version is there
    Failed,     // the work stopped on an error; everything was undone
    Confirm,    // uninstall.exe: "Remove Ruma?"
    Removing,
    Removed,
};

enum class Action : std::uint8_t {
    Enter,  // the screen's main button
    Escape, // the window's close
    Options,
    Back,
    ChangeFolder,
    ChangeHotkey,
    ToggleAutostart,
    ToggleDesktop,
    Cancel, // the Installing footer's
    Yes,    // to "Cancel the installation?"
    No,
    Close,
    License,
};

enum class Command : std::uint8_t { None, Install, Update, CancelInstall, Launch, Uninstall, License, Quit };

enum class Outcome : std::uint8_t { Done, Cancelled, Failed };

// The agent's four hotkeys, as its tray menu offers them.
inline constexpr std::string_view kHotkeys[] = {"Ctrl+Space", "Ctrl+Shift+Space", "Ctrl+Alt+Space", "Alt+Shift+Space"};

struct State {
    Screen screen = Screen::Welcome;
    Choices choices;
    FolderCheck folder;            // of choices.dir
    std::string version;           // this installer's
    std::string system;            // "Windows 11", for the Welcome screen's line
    std::string installed_version; // what is there (Update, Installed, Confirm)
    std::filesystem::path installed_dir;
    std::uint64_t needed = 0;      // bytes the program takes
    // Installing / Removing:
    float progress = 0;            // 0..1
    Step step = Step::Files;
    double eta_seconds = -1;       // below 0: not known yet
    double took_seconds = 0;       // Done: "Installed in 6.2 s"
    bool asking_cancel = false;    // "Cancel the installation?" in the footer
    std::string note;              // a line under the title: why it failed, or that Cancel left nothing
    bool uninstalling = false;     // uninstall.exe: Failed means nothing was removed
};

auto first_screen(Relation relation, bool uninstaller) -> Screen;
// The chosen folder takes the install (writable, enough space, not too deep).
auto can_install(const State& s) -> bool;
auto press(State& s, Action a) -> Command;
// The work in the background ended.
void finished(State& s, Outcome outcome, std::string_view why = {});

} // namespace setup
