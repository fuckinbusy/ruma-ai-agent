#pragma once

// The hidden agent's sign as it is painted (UI update, stage 3; design/ui_presence, variant A ·
// Handle): a two-tone bar hanging 3 px under the top edge — the bar and a halo of the opposite
// lightness round it — with the progress of the work filled in the accent and a dot of its state.
// The painter gives the frame of its scene (app::scenes::Sign) and the theme the sign is drawn in:
// Windows' own mode, not the window's theme.

#include "app/scenes/sign.hpp"
#include "theme/theme.hpp"
#include "ui/layout.hpp"
#include "ui/ui.hpp"

#include <string_view>

namespace app::sheet {

struct SignLook {
    const ::theme::Theme* theme = nullptr; // the sign's (theme::sign_theme)
    scenes::SignFrame frame;
    ::theme::Color dot;                    // the dot's colour: the state's (ok, warn, crit)
    float opacity = 1;                     // the whole sign's (it fades in after a close)
    // The capsule's content (frame.capsule > 0): one word in its state's colour, one line, the
    // shortcut that opens the sheet, the orb's picture (0: none).
    std::string_view word;
    std::string_view line;
    std::string_view shortcut;
    ::theme::Color word_color;
    unsigned orb = 0;
};

// The zone that takes the mouse (kZoneWidth x kZoneHeight), centred at `centre_x`, from `top` down.
[[nodiscard]] auto sign_zone(float centre_x, float top) noexcept -> ui::layout::Box;
// The sign's body this frame: centred in the zone, kTop under its top.
[[nodiscard]] auto sign_body(const ui::layout::Box& zone, const scenes::SignFrame& f) noexcept -> ui::layout::Box;
// Its dot's box (kDot at full size), kDotGap right of the body.
[[nodiscard]] auto sign_dot(const ui::layout::Box& zone, const scenes::SignFrame& f) noexcept -> ui::layout::Box;

// The capsule's width for what it says: the orb, the word, the line (cut when long), the shortcut's
// key — never under kCapsuleMin, never over kCapsuleMost.
[[nodiscard]] auto sign_capsule_width(ui::Ui& ui, std::string_view word, std::string_view line, std::string_view shortcut) -> float;
// The shortcut's key in the capsule: at its right end.
[[nodiscard]] auto sign_key(const ui::layout::Box& zone, const scenes::SignFrame& f, ui::Ui& ui, std::string_view shortcut) -> ui::layout::Box;

void draw_sign(ui::Ui& ui, const ui::layout::Box& zone, const SignLook& look);

} // namespace app::sheet
