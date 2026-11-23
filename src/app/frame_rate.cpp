#include "app/frame_rate.hpp"

#include <algorithm>
#include <cmath>

namespace app::frame_rate {

auto frame_interval(int monitor_hz, config::FrameRate rate) noexcept -> int {
    int wanted = 0;
    switch (rate) {
    case config::FrameRate::Monitor: return 1;
    case config::FrameRate::Hz120: wanted = 120; break;
    case config::FrameRate::Hz60: wanted = 60; break;
    case config::FrameRate::Hz30: wanted = 30; break;
    }
    if (monitor_hz <= 0) {
        return 1;
    }
    return std::max(1, static_cast<int>(std::lround(static_cast<double>(monitor_hz) / wanted)));
}

auto idle_orb_due(double since) noexcept -> bool {
    return since >= kIdleOrbFrame - 1.0e-4;
}

} // namespace app::frame_rate
