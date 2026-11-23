#include "setup/flow.hpp"

#include <algorithm>
#include <iterator>

namespace setup {
namespace {

auto start(State& s, Command c) -> Command {
    s.screen = Screen::Installing;
    s.progress = 0;
    s.step = Step::Files;
    s.eta_seconds = -1;
    s.asking_cancel = false;
    s.note.clear();
    return c;
}

void next_hotkey(State& s) {
    const auto* at = std::ranges::find(kHotkeys, s.choices.hotkey);
    const auto i = at == std::end(kHotkeys) ? 0 : (std::distance(std::begin(kHotkeys), at) + 1) % std::size(kHotkeys);
    s.choices.hotkey = std::string{kHotkeys[i]};
}

} // namespace

auto first_screen(Relation relation, bool uninstaller) -> Screen {
    if (uninstaller)
        return Screen::Confirm;
    switch (relation) {
    case Relation::NotInstalled: return Screen::Welcome;
    case Relation::Older: return Screen::Update;
    case Relation::SameOrNewer: return Screen::Installed;
    }
    return Screen::Welcome;
}

auto can_install(const State& s) -> bool {
    return s.folder.writable && s.folder.problem.empty();
}

auto press(State& s, Action a) -> Command {
    if (a == Action::License)
        return Command::License;
    switch (s.screen) {
    case Screen::Welcome:
    case Screen::Options:
        switch (a) {
        case Action::Enter:
            if (!can_install(s)) {
                s.screen = Screen::Options; // the problem is shown where the folder can be changed
                return Command::None;
            }
            return start(s, Command::Install);
        case Action::Options: s.screen = Screen::Options; return Command::None;
        case Action::Back: s.screen = Screen::Welcome; return Command::None;
        case Action::ChangeFolder: return Command::None; // Program Files only: no other place
        case Action::ChangeHotkey: next_hotkey(s); return Command::None;
        case Action::ToggleAutostart: s.choices.autostart = !s.choices.autostart; return Command::None;
        case Action::ToggleDesktop: s.choices.desktop = !s.choices.desktop; return Command::None;
        case Action::Escape:
        case Action::Close: return Command::Quit;
        default: return Command::None;
        }
    case Screen::Update:
        if (a == Action::Enter)
            return start(s, Command::Update);
        return a == Action::Escape || a == Action::Close ? Command::Quit : Command::None;
    case Screen::Installing:
        if (s.asking_cancel) {
            if (a == Action::Yes) {
                s.asking_cancel = false;
                return Command::CancelInstall;
            }
            if (a == Action::No || a == Action::Escape)
                s.asking_cancel = false;
            return Command::None;
        }
        if (a == Action::Escape || a == Action::Cancel || a == Action::Close)
            s.asking_cancel = true;
        return Command::None;
    case Screen::Done:
        if (a == Action::Enter)
            return Command::Launch;
        return a == Action::Escape || a == Action::Close ? Command::Quit : Command::None;
    case Screen::Confirm:
        if (a == Action::Enter) {
            s.screen = Screen::Removing;
            s.progress = 0;
            return Command::Uninstall;
        }
        return a == Action::Escape || a == Action::Close ? Command::Quit : Command::None;
    case Screen::Removing: return Command::None; // removal runs to its end
    case Screen::Installed:
    case Screen::Failed:
    case Screen::Removed:
        return a == Action::Enter || a == Action::Escape || a == Action::Close ? Command::Quit : Command::None;
    }
    return Command::None;
}

void finished(State& s, Outcome outcome, std::string_view why) {
    s.asking_cancel = false;
    const bool removing = s.screen == Screen::Removing;
    switch (outcome) {
    case Outcome::Done:
        s.progress = 1;
        s.screen = removing ? Screen::Removed : Screen::Done;
        s.note.clear();
        break;
    case Outcome::Cancelled:
        s.screen = s.installed_version.empty() ? Screen::Welcome : Screen::Update;
        s.note = "Cancelled. Nothing was left behind.";
        break;
    case Outcome::Failed:
        s.screen = removing ? Screen::Removed : Screen::Failed;
        s.note = std::string{why};
        break;
    }
}

} // namespace setup
