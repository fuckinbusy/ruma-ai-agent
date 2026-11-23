#pragma once

// theme — arithmetic on colours and themes: WCAG 2's contrast ratio (what the palette's table is
// checked by), a see-through colour laid on its ground, and mixing two colours or two whole
// themes (a change of theme goes through the mix).

#include "theme/theme.hpp"

namespace theme {

// `top` laid on an opaque `ground`: the colour that is seen.
[[nodiscard]] auto over(Color top, Color ground) noexcept -> Color;
// `top` laid on `under` when either may be see-through (source-over): the one colour that looks the
// same as the two drawn one after the other. A light under the mouse on a card is this.
[[nodiscard]] auto layer(Color top, Color under) noexcept -> Color;
// WCAG 2 relative luminance of an opaque colour, 0 (black) .. 1 (white).
[[nodiscard]] auto luminance(Color c) noexcept -> double;
// WCAG 2 contrast ratio, 1 .. 21, of `text` (laid on the ground first when it is see-through) on
// an opaque `ground`. 4.5 is AA for text.
[[nodiscard]] auto contrast(Color text, Color ground) noexcept -> double;

// From `a` (t = 0) to `b` (t = 1), each channel on its own; t outside 0..1 is held at the ends.
[[nodiscard]] auto mix(Color a, Color b, float t) noexcept -> Color;
// Every token of the two themes mixed. The name and `light` are those of the theme it is nearer to.
[[nodiscard]] auto mix(const Theme& a, const Theme& b, float t) noexcept -> Theme;

} // namespace theme
