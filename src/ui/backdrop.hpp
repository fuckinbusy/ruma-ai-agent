#pragma once

// ui::backdrop — the window's composition (spike R1). The GL framebuffer's alpha reaches DWM
// through blur-behind with an empty region: per-pixel alpha, no blur. Phase 8: no acrylic on
// the open window either: the panel draws the mockup's .965
// surface itself, and switching DWM's blur was one source of the flash when closing.

#include <windows.h>

namespace ui::backdrop {

// Once, after creating the window: per-pixel alpha, no DWM rounding (and so no DWM shadow around
// the resting handle), dark mode.
void prepare(HWND hwnd);

} // namespace ui::backdrop
