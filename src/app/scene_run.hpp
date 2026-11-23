#pragma once

// app::scene_run — the acceptance scripts of the motion rework (docs/specs/motion-rework.md § 9.4):
// what agent.exe --smoke --show --scene=<name> plays, on the simulated agent's test data, while
// scripts/motion-record.ps1 records the agent's region of the screen. A script is a table of steps;
// the shell plays it through the same paths the user's keys and mouse take (Shell::scene_step).

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace app::scene_run {

enum class Do : std::uint8_t {
    Open,   // the hotkey: the sheet opens (never activated: the user's window keeps the keyboard)
    Hotkey, // the hotkey again: open if closed, close if open
    Type,   // `text` into the field, a character at a time as the keyboard gives them
    Enter,
    Esc,
    Press,  // the answer's button with the id `text`, once it is offered
    Click,  // the mouse pressed and released on the target named `text`, once it is drawn ("name@dx,dy":
            // that far from its top left corner, not at its middle)
    Hover,  // the mouse moved onto the target named `text` ("-": away from the sheet)
    Settle, // waits until the agent has nothing to do and nothing moves
    End,    // the run passes
};

struct Step {
    Do what;
    // Seconds after the step before it was done; for Press and Click also after their target is
    // there, for Settle after everything is at rest.
    double after = 0;
    std::string_view text{};
};

struct Script {
    std::string_view name;
    std::array<int, 6> scenes; // the scenes of MS § 6 it plays (0: unused)
    std::span<const Step> steps;
    // It needs the agent's tools: the run is given --m5=<a folder of made-up files under %TEMP%>,
    // fenced. Without it the simulated agent has no tools and plays its plain scenarios.
    bool sandbox = false;
};

[[nodiscard]] auto all() noexcept -> std::span<const Script>;
// The scenes of MS § 6 no script can play on the simulated agent (each with its reason there).
[[nodiscard]] auto not_played() noexcept -> std::span<const int>;
[[nodiscard]] auto find(std::string_view name) noexcept -> const Script*;

} // namespace app::scene_run
