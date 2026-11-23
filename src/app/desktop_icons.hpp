#pragma once

// app::desktop_icons — where the desktop's icons stand (docs/spikes/phase-5.md: IShellWindows ->
// the desktop's IFolderView2; ~25 ms for 30 icons, so never on the UI thread). It reads; it
// moves an icon only to put it back where it stood (place_icons, an undo of the desktop's sorting).

#include "app/journal.hpp"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace app::desktop_icons {

// Where each path's icon stands, in the desktop view's own coordinates (UTF-8 paths); none
// for a path not on the desktop.
auto icon_spots(std::span<const std::string> paths) -> std::vector<std::optional<journal::Spot>>;
// The icons of these paths back where they stood (an undo of the desktop's sorting); the files
// may reappear a moment after their move back, so it looks again for up to a second. With the
// desktop's "Auto arrange icons" on Windows places them itself: nothing is moved. How many placed.
auto place_icons(std::span<const journal::Spot> spots) -> std::size_t;

} // namespace app::desktop_icons
