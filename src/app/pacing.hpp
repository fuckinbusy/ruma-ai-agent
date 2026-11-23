#pragma once

// app::pacing — how long the shell's idle loop may sleep before it has something to do.

#include "core/win.hpp"

#include <windows.h>

#include <algorithm>
#include <optional>

namespace app::pacing {

// Milliseconds for MsgWaitForMultipleObjects; INFINITE: until a message comes.
//   resting: the resting motion runs; it looks again `look_every` after the last look
//            (`since_look` seconds ago), at once when that is overdue.
//   paused:  the resting motion is on but paused (a fullscreen app): look again in a second.
//   deadline_left: a measuring smoke run's time left at rest; overdue waits 0 (review I-1).
inline auto idle_wait_ms(bool resting, bool paused, double since_look, double look_every,
                         std::optional<double> deadline_left) -> DWORD {
    std::optional<double> wait;
    if (resting) {
        wait = std::max(look_every - since_look, 0.0);
    } else if (paused) {
        wait = 1.0;
    }
    if (deadline_left) {
        const double left = std::max(*deadline_left, 0.0);
        wait = wait ? std::min(*wait, left) : left;
    }
    return wait ? static_cast<DWORD>(*wait * 1000.0) : INFINITE;
}

// The wait between two frames that only the orb, the caret or a running line move. A timeout
// of MsgWaitForMultipleObjects ends on the system's timer tick (15.6 ms unless some program
// asked for finer): a 12 ms wait took 15.6, and "60 frames a second" came 40 to 60 times a
// second, unevenly — the running line stuttered. A high-resolution waitable timer keeps it.
class Timer {
public:
    Timer()
        : timer_{CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS)} {}

    // Up to `seconds`; a message for the thread's windows ends it sooner.
    void wait(double seconds) {
        LARGE_INTEGER due{};
        due.QuadPart = -static_cast<LONGLONG>(std::max(seconds, 0.0) * 1.0e7); // relative, in 100 ns
        if (timer_ && SetWaitableTimer(timer_.get(), &due, 0, nullptr, nullptr, FALSE)) {
            HANDLE h = timer_.get();
            MsgWaitForMultipleObjects(1, &h, FALSE, INFINITE, QS_ALLINPUT);
            CancelWaitableTimer(h);
        } else { // no high-resolution timer (before Windows 10 1803): the tick it is
            MsgWaitForMultipleObjects(0, nullptr, FALSE, static_cast<DWORD>(seconds * 1000.0), QS_ALLINPUT);
        }
    }

private:
    core::win::KernelHandle timer_;
};

} // namespace app::pacing
