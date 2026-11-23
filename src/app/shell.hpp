#pragma once

// app::shell — the agent process: settings, the sheet's window and its GL renderer, the
// simulated agent, the tray icon and the frame loop. One UI thread; the agent posts events
// through a mailbox and wakes the loop with WM_APP_WAKE.

#include <optional>
#include <string>
#include <string_view>

namespace app::shell {

struct ScreenPoint {
    int x = 0;
    int y = 0;
};

struct Options {
    bool open_at_start = false; // --open
    double sim_speed = 1.0;     // --sim-speed=N
    // --smoke: the whole agent process without showing anything: never-shown window, no hotkey,
    // no tray; asks the slow-computer question, presses "Yes, close", exits 0 when done.
    bool smoke = false;
    // --show (with --smoke): the window is shown, never activated, so that a probe can look at
    // what DWM composes on screen (spikes/region --agent).
    bool show = false;
    // --rest-seconds=N (with --smoke): after closing, stay at rest N seconds and log what the
    // resting handle costs (frames, CPU). --rest-animation=on|off: the slow motion at rest (and
    // the open idle orb) whatever config.json says; not given: as it says.
    double rest_seconds = 0;
    std::optional<bool> rest_animation;
    // --headless-measure: a smoke run without the scenario: the handle at rest for
    // rest_seconds (10 unless given), logging the first frame's time and the rest's cost.
    bool measure = false;
    // --open-idle (with --headless-measure): the sheet open with nothing to do, measured as the
    // rest is (phase 8: only the caret's flips and, with the rest motion, the orb).
    bool open_idle = false;
    // --snapshot=<file> (with --smoke or --headless-measure): when the run ends, the debug
    // snapshot (app::debug::snapshot_text) goes to <file>.
    std::wstring snapshot;
    // --debug-capture=<dir> (Debug builds, with --smoke): the debug window's four tabs as PNGs
    // when the run ends, drawn off-screen.
    std::wstring debug_capture;
    // --m4=<dir> (with --smoke): milestone M4 on a copy of a desktop made in <dir>, which must be
    // inside %TEMP%: "Sort my desktop" on real tools behind a fence on <dir>, every question
    // answered Allow by the run itself (only here), the layout checked, Undo, the audit verified.
    std::wstring m4;
    // --m5=<dir> (with --smoke): milestone M5 on copies of Temp, Downloads and Desktop made in <dir>
    // (inside %TEMP%, behind a fence on it): "Clean up temp files and delete old installers from
    // Downloads" on real tools, the installers' card with one file left out, the result and the
    // journal checked, the task undone, the audit verified.
    std::wstring m5;
    // Phase 7: the real agent (app::model) is the default. --sim: the simulated one instead (as
    // --smoke, --m4 and --m5 have it). --replay=<cassette>: the real agent on a recording (no
    // network); with --smoke it asks "Why is my computer slow?" and waits for the report and its
    // cost. --m7=<dir> (inside %TEMP%): the real agent with DeepSeek on a copy of a desktop in
    // <dir>, behind a fence on it — the live scenarios of phase 7.
    bool sim = false;
    std::wstring replay;
    std::wstring m7;
    // --after-install=<x>,<y> (the installer's launch, physical pixels: its window's centre): the
    // sheet opens, with its usual opening, on the monitor with that point — asking for the key when
    // there is none.
    std::optional<ScreenPoint> after_install;
    // --go-event=<name> (with --after-install): the installer's event, set when its orb has gone into
    // the strip; the sheet opens then. The agent starts with the flight, so it is up by then. No such
    // event (the installer is gone): it opens at once.
    std::wstring go_event;
    // --ready-event=<name> (with --after-install): the installer's event, set once the open window's
    // first frame is on the screen — the installer quits then.
    std::wstring ready_event;
    // --smoke-exit (with --smoke): the run asks itself to exit as the installer does; it passes
    // when the exit message ends it.
    bool smoke_exit = false;
    // --scene=<name> (with --smoke --show): an acceptance script of the motion rework is played
    // instead of the smoke scenario (app::scene_run), for the screen recording.
    std::string scene;
    // --frame-rate=<120|60|30> (with --smoke): the animations' frame rate whatever config.json says.
    int frame_rate = 0;
    // --reduced-motion (with --smoke): as with Windows' animation effects off, whatever Windows says
    // (the user's setting is never touched).
    bool reduced_motion = false;
};

inline constexpr std::wstring_view kReadyArg = L"--ready-event=";
inline constexpr std::wstring_view kGoArg = L"--go-event=";

// "<x>,<y>": two whole numbers; anything else is nothing.
auto parse_point(std::wstring_view text) -> std::optional<ScreenPoint>;

// Runs until the user exits; returns the process exit code.
auto run(const Options& options) -> int;

} // namespace app::shell
