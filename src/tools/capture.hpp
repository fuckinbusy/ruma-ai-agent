#pragma once

// The real screen.capture (phase 9), behind tools::os::Backend::capture: GDI only.

#include "tools/os.hpp"

namespace tools::os::detail {

// The monitor under the mouse, a monitor by its number or all of them (BitBlt of the screen), or a
// window (PrintWindow, so a window behind others still comes out whole) cut to the bounds DWM
// draws. `active`: the window that was in front before the agent opened (an HWND), null: the
// foreground one. `agent`: the agent's window on the screen, outlined where it is (phase 10).
auto capture(const CaptureSpec& spec, void* active, std::array<long, 4> agent) -> core::Result<Captured>;
// The clipboard: text, else files, else a picture ("PNG", then CF_DIBV5 / CF_DIB through WIC).
auto read_clipboard() -> core::Result<ClipboardContent>;

} // namespace tools::os::detail
