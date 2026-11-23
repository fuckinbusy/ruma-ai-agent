#pragma once

// ui::theme — design tokens taken from the chosen mockup, design/agent-placements.html
// (placement 2, "sheet"). Each value names its CSS source; tests/ui/theme_test.cpp reads the
// mockup and checks the values against it. Sizes are logical pixels (CSS px).

#include "theme/current.hpp"
#include "ui/render/renderer.hpp"

namespace ui {

// The theme of this frame (the theme module's): where every colour of the interface comes from.
[[nodiscard]] inline auto th() noexcept -> const ::theme::Theme& {
    return ::theme::current();
}

} // namespace ui

namespace ui {

// Corners of `r` all round, and a card's (the theme's radius: the designer's 14).
[[nodiscard]] inline auto corners(float r) noexcept -> render::Corners {
    return {r, r, r, r};
}
[[nodiscard]] inline auto card_corners() noexcept -> render::Corners {
    return corners(th().radius.card);
}

// Over a picture (a thumbnail's label on its own dark scrim, the zoom mark on a picture under the
// mouse): the picture is not the interface, so these are the same whatever the theme.
inline constexpr render::Color kOnPicture{255, 255, 255, 255}; // color-ok: over a picture, in both themes
inline constexpr render::Color kPictureScrim{0, 0, 0, 140};    // color-ok: over a picture, in both themes

} // namespace ui

namespace ui::theme {

// Font sizes (CSS font-size) and the faces they use.
namespace type {
inline constexpr float typed = 17.0f;       // #sheet .typed
inline constexpr float sub = 13.5f;         // #sheet .sub
inline constexpr float title = 14.5f;       // .tx b, 600
inline constexpr float header = 15.0f;      // .hd b, 600 --font-d
inline constexpr float shimmer = 14.5f;     // .shim, 600 --font-d
inline constexpr float chip = 11.5f;        // .chip
inline constexpr float button = 12.5f;      // .pbtn, 600
inline constexpr float answer = 14.0f;      // .resp
inline constexpr float answer_line = 1.6f;  // .resp line-height
inline constexpr float prose = 14.5f;       // the answer's prose (phase 8, ui_remastered)…
inline constexpr float prose_line = 1.62f;  // …and its line height
inline constexpr float bubble = 13.5f;      // .bubble
inline constexpr float placeholder = 13.5f; // .ft .ph
inline constexpr float icon = 14.0f;        // .icb svg
} // namespace type

namespace sheet {
inline constexpr float width = 780.0f;          // --w in input/thinking/answering/done
inline constexpr float bar_height = 74.0f;      // --h input/thinking/done
inline constexpr float answer_height = 380.0f;  // --h answering
inline constexpr float bar_radius = 22.0f;      // --r input/thinking/done
inline constexpr float answer_radius = 26.0f;   // --r answering
inline constexpr float handle_width = 150.0f;   // rest --w
inline constexpr float handle_height = 5.0f;    // rest --h, --r 5
inline constexpr float pad_left = 76.0f;        // --pl
inline constexpr float pad_right = 18.0f;       // --pr
inline constexpr float gap = 10.0f;             // .L-* gap
inline constexpr float orb_x = 40.0f;           // --ox
inline constexpr float orb_y = 37.0f;           // --oy
inline constexpr float header_height = 74.0f;   // --hh answering
inline constexpr float header_left = 78.0f;     // --hl answering
inline constexpr float header_right = 14.0f;    // .hd right
inline constexpr float body_inset = 24.0f;      // .bd left/right
inline constexpr float footer_space = 68.0f;    // .bd bottom (--bb default)
inline constexpr float shadow_y = 24.0f;        // box-shadow 0 24px 60px -16px
inline constexpr float shadow_blur = 60.0f;
inline constexpr float shadow_spread = -16.0f;
} // namespace sheet

namespace field { // .ft: the capsule input at the bottom of the answer card
inline constexpr float inset = 14.0f;  // left/right/bottom
inline constexpr float height = 42.0f;
inline constexpr float radius = 21.0f;
inline constexpr float pad_left = 16.0f;
inline constexpr float pad_right = 5.0f;
} // namespace field

namespace bubble { // .bubble
inline constexpr float pad_y = 8.0f;
inline constexpr float pad_x = 13.0f;
inline constexpr float radius = 15.0f;
inline constexpr float tail_radius = 4.0f; // bottom-right
inline constexpr float max_width = 0.85f;  // of the body width
inline constexpr float margin_bottom = 12.0f;
} // namespace bubble

namespace chip { // .chip
inline constexpr float pad_y = 4.0f;
inline constexpr float pad_x = 9.0f;
inline constexpr float gap = 6.0f;
inline constexpr float dot = 6.0f;
inline constexpr float dot_glow = 8.0f;
} // namespace chip

namespace button {
inline constexpr float send = 34.0f;        // .send
inline constexpr float send_icon = 16.0f;   // .send svg
inline constexpr float stop = 32.0f;        // .stopb
inline constexpr float stop_square = 10.0f; // .stopb span
inline constexpr float stop_square_radius = 2.5f;
inline constexpr float stop_ring_inset = 3.0f; // .stopb::before inset:-3px, padding 2px
inline constexpr float stop_ring_width = 2.0f;
inline constexpr float icon = 30.0f;        // .icb
inline constexpr float icon_radius = 8.0f;
inline constexpr float pill_pad_y = 6.0f;   // .pbtn
inline constexpr float pill_pad_x = 13.0f;
inline constexpr float ok_badge = 18.0f;    // .okc
} // namespace button

namespace caret {
inline constexpr float input_width = 2.0f;   // .caret
inline constexpr float input_height = 18.0f;
inline constexpr float stream_width = 8.0f;  // .scaret
inline constexpr float stream_height = 14.0f;
inline constexpr float stream_radius = 2.0f;
} // namespace caret

namespace progress { // .bar
inline constexpr float height = 2.0f;
inline constexpr float highlight = 0.35f; // width share of .bar i
} // namespace progress

// The agent's work screens (phase 5): the top bar, steps, the permission card, the report, the
// log, background tasks, the terminal — from design/interaction-topbar/agent-topbar-interaction.html.
// tests/ui/work_theme_test.cpp reads that mockup and checks these values against it.
namespace work {

namespace bar { // .sheet and its .row
inline constexpr float width = 972.0f;   // const W
inline constexpr float height = 90.0f;   // .row height
inline constexpr float gap = 14.0f;      // .row gap
inline constexpr float pad_left = 18.0f; // .row padding 0 22px 0 18px
inline constexpr float pad_right = 22.0f;
inline constexpr float orb = 68.0f;       // .borb
inline constexpr float orb_margin = -8.0f; // .borb margin 0 -8px
inline constexpr float send = 44.0f;      // .send44
inline constexpr float send_icon = 19.0f; // .send44 svg
inline constexpr float stop = 44.0f;      // .stop44
inline constexpr float stop_square = 13.0f;        // .stop44 span
inline constexpr float stop_square_radius = 3.0f;
inline constexpr float icon_button = 38.0f;        // .ib
inline constexpr float icon_button_radius = 10.0f;
inline constexpr float icon = 18.0f;               // .ib svg
inline constexpr float progress = 2.0f;            // .rprog
} // namespace bar

namespace body { // .sbody, .cols
inline constexpr float top = 90.0f;
inline constexpr float pad_top = 20.0f;
inline constexpr float pad_x = 24.0f;
inline constexpr float column_gap = 20.0f;
} // namespace body

namespace type {
inline constexpr float typed = 21.0f;             // .typed 400
inline constexpr float placeholder = 22.0f;       // .ph 300
inline constexpr float placeholder_small = 19.0f; // .ph.sm
inline constexpr float title = 17.0f;             // .rt b 600 --font-d
inline constexpr float sub = 13.5f;               // .rt span
inline constexpr float chip = 13.0f;              // .chip
inline constexpr float message = 14.5f;           // .msg
inline constexpr float message_line = 1.55f;
inline constexpr float bubble = 14.0f;            // .bubble
inline constexpr float button = 13.0f;            // .btn 600
inline constexpr float button_small = 12.0f;      // .btn.sm
inline constexpr float step_title = 13.5f;        // .st b 600
inline constexpr float step_sub = 12.0f;          // .st span
inline constexpr float step_result = 12.5f;       // .sr
inline constexpr float file = 12.5f;              // .frow
inline constexpr float tag = 11.5f;               // .tag
inline constexpr float option = 13.0f;            // .opt
inline constexpr float segment = 12.0f;           // .seg
inline constexpr float kv = 13.0f;                // .kv
inline constexpr float mono = 12.0f;              // .cmd --mono
inline constexpr float tile_value = 26.0f;        // .tile .v 600 --font-d
inline constexpr float quick_value = 30.0f;       // .qa .big2 600 --font-d
inline constexpr float quick_unit = 15.0f;        // .qa .big2 small
} // namespace type

namespace chip { // .chip
inline constexpr float pad_y = 5.0f;
inline constexpr float pad_x = 11.0f;
inline constexpr float gap = 7.0f;
inline constexpr float dot = 7.0f;
} // namespace chip

namespace kind_icon { // .kic and its .badge
inline constexpr float size = 30.0f;
inline constexpr float radius = 8.0f;
inline constexpr float icon = 16.0f;
inline constexpr float badge = 14.0f;
inline constexpr float badge_offset = -4.0f; // right/bottom -4px
inline constexpr float tint = 0.14f;  // background color-mix 14%
inline constexpr float ring = 0.24f;  // inset 1px color-mix 24%
} // namespace kind_icon

namespace step { // .step, .sh, .sb, .steps
inline constexpr float min_height = 54.0f;
inline constexpr float gap = 12.0f;
inline constexpr float pad_y = 10.0f;
inline constexpr float pad_x = 14.0f;
inline constexpr float body_left = 56.0f;
inline constexpr float body_gap = 6.0f;
inline constexpr float list_gap = 8.0f;
} // namespace step

namespace file_row { // .frow, .fic
inline constexpr float height = 30.0f;
inline constexpr float radius = 7.0f;
inline constexpr float gap = 10.0f;
inline constexpr float pad_x = 9.0f;
inline constexpr float icon_w = 18.0f;
inline constexpr float icon_h = 20.0f;
} // namespace file_row

namespace progress { // .prog
inline constexpr float height = 4.0f;
inline constexpr float radius = 2.0f;
} // namespace progress

namespace button { // .btn, .btn.sm (corners: the theme's)
inline constexpr float pad_y = 7.0f;
inline constexpr float pad_x = 15.0f;
inline constexpr float small_pad_y = 5.0f;
inline constexpr float small_pad_x = 11.0f;
} // namespace button

namespace segment { // .seg
inline constexpr float pad = 2.0f;
inline constexpr float radius = 7.0f;
inline constexpr float item_pad_y = 3.0f;
inline constexpr float item_pad_x = 10.0f;
inline constexpr float item_radius = 5.0f;
} // namespace segment

namespace option { // .opt, .opt .r
inline constexpr float radius = 8.0f;
inline constexpr float gap = 10.0f;
inline constexpr float pad_y = 9.0f;
inline constexpr float pad_x = 11.0f;
inline constexpr float radio = 16.0f;
inline constexpr float radio_dot = 5.0f; // .opt.on .r inset 5px: the ring that fills to a dot
} // namespace option

namespace checkbox { // .cb
inline constexpr float size = 16.0f;
inline constexpr float radius = 4.0f;
inline constexpr float gap = 9.0f;
} // namespace checkbox

namespace dropdown { // .dd
inline constexpr float radius = 6.0f;
inline constexpr float pad_y = 5.0f;
inline constexpr float pad_x = 9.0f;
} // namespace dropdown

namespace tile { // .tile (corners: a card's)
inline constexpr float spark = 40.0f;
} // namespace tile

namespace donut { // .donut: a 36-unit viewBox, r 15, stroke 4
inline constexpr float size = 76.0f;
inline constexpr float radius = 76.0f * 15.0f / 36.0f;
inline constexpr float stroke = 76.0f * 4.0f / 36.0f;
} // namespace donut

namespace toggle { // .sw
inline constexpr float width = 40.0f;
inline constexpr float height = 20.0f;
inline constexpr float knob = 12.0f;
} // namespace toggle

namespace tag { // .tag
inline constexpr float pad_y = 2.0f;
inline constexpr float pad_x = 9.0f;
} // namespace tag

namespace kv { // .kv
inline constexpr float pad_y = 8.0f;
} // namespace kv

namespace quick { // .qa .stack
inline constexpr float stack = 12.0f;
} // namespace quick

} // namespace work

} // namespace ui::theme
