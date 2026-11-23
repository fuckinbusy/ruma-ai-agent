#include "app/scene_run.hpp"

namespace app::scene_run {
namespace {

// 01 the summon, 02 Esc on the bar, 15 Esc 130 ms into a summon, 04 the field's row growing a line
// and emptied, 14 the light under the mouse and a press.
constexpr Step kBar[] = {
    {Do::Open, 0.5},
    {Do::Esc, 1.2},
    {Do::Open, 1.0},
    {Do::Esc, 0.13},
    {Do::Open, 1.0},
    {Do::Hover, 0.9, "model:chip"},
    {Do::Hover, 0.5, "-"},
    {Do::Type, 0.4, "Compare the three largest folders on drive D with last month's sizes and tell me which of them grew the most since then"},
    {Do::Hover, 0.8, "send"},
    {Do::Hover, 0.5, "-"},
    {Do::Esc, 0.6},
    {Do::Esc, 0.6},
    {Do::End, 1.2},
};

// 05 Enter, 06 the steps, 07 the answer streaming and its end, 14 a button pressed, 03 Esc on the
// expanded window.
constexpr Step kAsk[] = {
    {Do::Open, 0.5},
    {Do::Type, 0.8, "Why is my computer slow?"},
    {Do::Enter, 0.4},
    {Do::Click, 0.8, "action:close"},
    {Do::Settle, 1.0},
    {Do::Esc, 0.2},
    {Do::End, 1.4},
};

// 12 the window folding into the handle while the agent works, 13 the handle clicked back into the
// window, 03 at the end.
constexpr Step kFold[] = {
    {Do::Open, 0.5},
    {Do::Type, 0.8, "Sort my desktop"},
    {Do::Enter, 0.4},
    {Do::Esc, 0.9},
    {Do::Click, 0.9, "sign"},
    {Do::Settle, 1.0},
    {Do::Hotkey, 0.2},
    {Do::End, 1.4},
};

// 08 a table's rows coming and sorted again (a click on its head), a row's light (14). The report
// reads the machine (nothing is changed) and goes on living: its steps wait by the clock.
constexpr Step kPanel[] = {
    {Do::Open, 0.5},
    {Do::Type, 0.8, "Why is my computer slow?"},
    {Do::Enter, 0.4},
    {Do::Click, 2.0, "panel:table@60,14"},
    {Do::Click, 1.2, "panel:table@60,14"},
    {Do::Hover, 1.2, "row:1"},
    {Do::Hover, 0.6, "-"},
    {Do::Esc, 0.8},
    {Do::End, 1.4},
};

// 11 a question asked and answered, twice (the screen around dims for its seconds; the files are
// made-up ones in the run's own folder), 10 an earlier turn opened and closed.
constexpr Step kApprove[] = {
    {Do::Open, 0.5},
    {Do::Type, 0.8, "Clean up temp files and delete old installers from Downloads"},
    {Do::Enter, 0.4},
    {Do::Click, 1.2, "action:allow"},
    {Do::Click, 1.5, "action:allow"},
    {Do::Settle, 1.2},
    {Do::Type, 0.3, "Hello there"},
    {Do::Enter, 0.4},
    {Do::Settle, 1.2},
    {Do::Click, 0.4, "earlier:0"},
    {Do::Click, 1.2, "earlier:0"},
    {Do::Settle, 0.8},
    {Do::Esc, 0.2},
    {Do::End, 1.4},
};

// The hidden agent's sign (UI update, stage 3; design/ui_presence): the capsule under the mouse and
// its going 0.6 s after; the window out of the capsule and back into the handle; closed while the
// agent works — the wide handle, then its dot when the answer is there; the capsule on that.
constexpr Step kHandle[] = {
    {Do::Open, 0.5},
    {Do::Esc, 1.2},
    {Do::Hover, 1.4, "sign"},
    {Do::Hover, 1.4, "-"},
    {Do::Hover, 1.6, "sign"},
    {Do::Click, 1.0, "sign"},
    {Do::Esc, 1.4},
    {Do::Open, 1.2},
    {Do::Type, 0.9, "Why is my computer slow?"},
    {Do::Enter, 0.4},
    {Do::Esc, 0.7},
    {Do::Settle, 1.2},
    {Do::Hover, 0.6, "sign"},
    {Do::Hover, 1.4, "-"},
    {Do::Open, 1.4},
    {Do::Esc, 1.4},
    {Do::End, 1.4},
};

// The model menu (UI update, stage 4b; MODEL_PICKER_SPEC § 12, 5): opened from the chip, a search
// typed and cleared, Pro chosen by a click, the menu again and Max chosen in its foot, closed, then
// opened and closed 100 ms into its opening, the sheet closed. No scene of MS § 6 of its own.
constexpr Step kMenu[] = {
    {Do::Open, 0.5},
    {Do::Click, 1.0, "model:chip"},
    {Do::Type, 0.8, "pro"},
    {Do::Esc, 1.0},
    {Do::Hover, 0.6, "menu:row:1"},
    {Do::Click, 0.9, "menu:row:1"},
    {Do::Click, 1.0, "model:chip"},
    {Do::Click, 0.8, "menu:level:3"},
    {Do::Click, 0.8, "model:chip"},
    {Do::Click, 1.0, "model:chip"},
    {Do::Click, 0.1, "model:chip"},
    {Do::Esc, 1.0},
    {Do::End, 1.0},
};

constexpr Script kScripts[] = {
    {"handle", {12, 13, 0, 0, 0, 0}, kHandle, false},
    {"bar", {1, 2, 4, 14, 15, 0}, kBar, false},
    {"ask", {3, 5, 6, 7, 14, 0}, kAsk, false},
    {"fold", {12, 13, 3, 0, 0, 0}, kFold, false},
    {"panel", {8, 14, 0, 0, 0, 0}, kPanel, true},
    {"approve", {10, 11, 0, 0, 0, 0}, kApprove, true},
    {"menu", {0, 0, 0, 0, 0, 0}, kMenu, false},
};

} // namespace

// 09 (the panel's tabs): the simulated agent makes one data tab at most, so there is no second tab
// to click. Its motion is held by the view's and the widget's tests and a hand check.
constexpr int kNotPlayed[] = {9};

auto not_played() noexcept -> std::span<const int> {
    return kNotPlayed;
}

auto all() noexcept -> std::span<const Script> {
    return kScripts;
}

auto find(std::string_view name) noexcept -> const Script* {
    for (const auto& s : kScripts) {
        if (s.name == name) {
            return &s;
        }
    }
    return nullptr;
}

} // namespace app::scene_run
