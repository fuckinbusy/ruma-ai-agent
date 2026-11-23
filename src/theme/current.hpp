#pragma once

// theme — the theme of this frame: what the painting code reads. One for the process, read and set
// on the UI thread only.
//
//   theme::current()        every painter's source of colour
//   theme::set(th)          this theme, at once
//   theme::begin_change(to) a change that takes its time: what is on the screen now is kept as its
//                           start (a change begun during a change goes on from the colours it has)
//   theme::progress(t)      how far the change is, 0 .. 1 — the owner of the clock says (the app's
//                           spring on the quick token); at 1 the theme is exactly `to`
//
// The module knows nothing of time: the one who draws frames moves the change. Reading a token and
// moving a change allocate nothing.

#include "theme/theme.hpp"

namespace theme {

[[nodiscard]] auto current() noexcept -> const Theme&;
void set(const Theme& th) noexcept;

void begin_change(const Theme& to) noexcept;
void progress(float t) noexcept;
// True from begin_change() until progress() has reached 1 (or set() cut it short).
[[nodiscard]] auto changing() noexcept -> bool;
// The theme a change is going to; the current one when nothing changes.
[[nodiscard]] auto target() noexcept -> const Theme&;

} // namespace theme
