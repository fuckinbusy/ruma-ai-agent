#pragma once

// app::notify — Windows notifications for the tab's cards while a fullscreen app is in front
// (docs/plans/phase-5.md, Task 12; the way the spike found: a Start menu shortcut "Agent.lnk"
// carrying the app's id and its activator, docs/spikes/phase-5.md). A notification's button is
// the card's button; a policy question only opens its card — it is never answered from a
// notification.

#include "app/sheet_model.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace app::notify {

struct Button {
    std::string label;     // "Undo"
    std::string arguments; // "drop:12:undo": what press_of reads back
};

struct Note {
    std::uint64_t id = 0; // the card's task
    std::string title;
    std::string text;
    std::vector<Button> buttons;
};

class Notifier {
public:
    virtual ~Notifier() = default;
    virtual void show(const Note& note) = 0;
    virtual void hide(std::uint64_t id) = 0;
};

// Notes what it was asked (tests and the smoke run: nothing reaches Windows).
class RecordingNotifier final : public Notifier {
public:
    void show(const Note& note) override { shown.push_back(note); }
    void hide(std::uint64_t id) override { hidden.push_back(id); }
    std::vector<Note> shown;
    std::vector<std::uint64_t> hidden;
};

// The real one. Nothing is written until the first notification: then the Start menu shortcut
// (%APPDATA%\Microsoft\Windows\Start Menu\Programs\Agent.lnk, the app's id and its activator) and
// HKCU\Software\Classes\CLSID\{activator}\LocalServer32 (Windows starts the agent for a click
// when it is not running). `clicked` gets a button's arguments, on a COM thread.
class WindowsNotifier final : public Notifier {
public:
    explicit WindowsNotifier(std::function<void(std::wstring)> clicked);
    ~WindowsNotifier() override;
    WindowsNotifier(const WindowsNotifier&) = delete;
    auto operator=(const WindowsNotifier&) -> WindowsNotifier& = delete;
    void show(const Note& note) override;
    void hide(std::uint64_t id) override;

private:
    auto ready() -> bool;
    bool tried_ = false;
    bool ready_ = false;
};

// The card as a notification: a result's own buttons, a question's Review only, an alert none.
auto note_of(const sheet::Drop& d) -> Note;
// "drop:<task>:<action>".
// The Start-menu shortcut that carries the app's id: the installer's "Ruma.lnk" when the
// agent was installed (install.json beside agent.exe) — the agent then makes none of its own —
// otherwise a build's "Agent.lnk".
auto shortcut_name(const std::filesystem::path& exe_dir) -> std::wstring_view;

auto arguments(std::uint64_t task, std::string_view action) -> std::string;
// A button's arguments back to the card's task and button; anything else: none.
auto press_of(std::string_view arguments) -> std::optional<std::pair<std::uint64_t, std::string>>;
// The toast's XML (ToastGeneric: the title, the text, the buttons), every text escaped.
auto toast_xml(const Note& note) -> std::string;

// Which cards are notifications now: while a fullscreen app is in front every new card is shown
// once; a card that went away takes its notification with it.
class Courier {
public:
    void update(const sheet::Model& m, bool fullscreen, Notifier& n);

private:
    std::vector<std::pair<std::uint64_t, std::uint64_t>> shown_; // the cards (task, made) that are notifications
};

} // namespace app::notify
