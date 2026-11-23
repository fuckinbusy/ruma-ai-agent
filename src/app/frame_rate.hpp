#pragma once

// app::frame_rate — the animations' frame rate (motion rework, task 4; chosen,
// not 60 for good). Kept by the vertical sync — a swap every n-th sync, never a Sleep (MOTION_SPEC
// § 5.2). Motion steps by a frame's own time, so it looks the same at any of them.

#include "app/config.hpp"

namespace app::frame_rate {

// The open idle sheet's orb at most this often.
inline constexpr double kIdleOrbFrame = 1.0 / 60.0;

// The swap interval for a monitor of `monitor_hz` at `rate`: the nearest whole divisor, never a
// rate above the monitor's; 1 when the monitor does not say.
[[nodiscard]] auto frame_interval(int monitor_hz, config::FrameRate rate) noexcept -> int;

// The idle orb's next frame is due `since` seconds after its last one.
[[nodiscard]] auto idle_orb_due(double since) noexcept -> bool;

} // namespace app::frame_rate
