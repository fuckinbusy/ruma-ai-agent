#pragma once

// theme — the one module that owns the agent's colours and everything else of a theme: the light
// of the orb, the radii, the shadows (UI update, stage 1; one module with a
// clear API, so that themes can be added later without touching the code that paints).
//
//   theme::Theme      every token of one theme (a plain value: it can be copied, mixed, compared)
//   theme::builtin()  the themes that come with the agent — Dark and Light; their values are in
//                     builtin.cpp and nowhere else
//   theme::find(name) a built-in theme by its name
//   theme::color_tokens()  name <-> field, for whatever goes through every colour (tests, and
//                     later a theme read from a file)
//   theme/contrast.hpp  WCAG contrast, laying a see-through colour on its ground, mixing
//   theme/current.hpp   the theme of this frame, which the painting code reads
//
// Painting code never writes a colour in numbers: it asks the current theme for a token.

#include "theme/color.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

namespace theme {

// A vertical gradient, top to bottom.
struct Gradient {
    Color top, bottom;
};

// One layer of a drop shadow, as CSS writes it: 0 <y> <blur> <spread> <colour> (logical px).
struct Shadow {
    float y = 0, blur = 0, spread = 0;
    Color color;
};

// The orb's light (its shader's uL1, uL2, uL3, uTint, uGlow, uAmb): three lights, the tint of its
// body, its glow, and how much of the surroundings lights its dark side (a light theme).
struct OrbLight {
    Color l1, l2, l3, tint, glow;
    float amb = 0;
};

struct Radii {
    float window = 24, card = 14, button = 10, label = 99;
};

struct Theme {
    // The colours, one after another (color_tokens() names each; the test counts them).
    Color surface;     // the window, at its bottom
    Color surface_top; // ...and at its top: a little lighter
    Color panel;       // the data panel beside the conversation
    Color card;        // a card on the window
    Color card_2;      // a card on a card, a raised control
    Color line;        // a hairline
    Color line_2;      // a stronger one
    Color text;        // what is read
    Color text_2;      // second
    Color text_3;      // captions
    Color text_4;      // hints: only for labels of little weight
    Color accent;      // the active tab, the send button, what is chosen — nothing else
    Color accent_2;    // routes and notes that inform
    Color accent_soft; // the ground of what is chosen
    Color accent_line; // ...and its line
    Color on_accent;   // words and icons on the accent — and on any solid colour (a check on the ok disc)
    Color ok;
    Color warn;
    Color crit;
    Color you;         // the user's own message: its ground
    Color you_line;    // ...and its line
    // States and derived colours (the designer's three rules: an ink ladder for hover, press,
    // choice and tracks; soft ground, ground under the mouse and line for every coloured token;
    // colour means risk, not category).
    Color w1; // the ink ladder: the text colour at four strengths, over any surface — a row under the mouse
    Color w2; // ...pressed or chosen; a button at rest; a track
    Color w3; // ...a button under the mouse; a monogram's ground
    Color w4; // ...a neutral bar on its track
    Color scroll; // a scrollbar's thumb
    Color scroll_hi; // ...under the mouse
    Color sunk; // below the surface: a code block, a command's output, the terminal
    Color pop; // above it: a menu, a list, a tooltip (its line is line_2, its shadow pop_shadow)
    Color selection; // selected text
    Color scrim; // over the screen around a question
    Color accent_soft_hi; // the ground of what is chosen, under the mouse
    Color ok_soft; // a status's soft ground (the status itself is the text and the icon on it)
    Color ok_soft_hi; // ...under the mouse
    Color ok_line; // ...and its line
    Color warn_soft;
    Color warn_soft_hi;
    Color warn_line;
    Color crit_soft;
    Color crit_soft_hi;
    Color crit_line;
    Color c1; // a chart's series, in this order everywhere
    Color c2;
    Color c3;
    Color c4;
    Color c_rest; // ...and the rest: the fifth and further go here
    Color f_folder; // a folder
    Color f_folder_line; // ...its inner line (a light theme; none on a dark one)
    Color f_doc; // a text document's badge
    Color f_sheet; // a spreadsheet's badge
    Color f_page; // a file's sheet of paper
    // The sign of the hidden agent (design/ui_presence): its bar, and a halo 1.5 px round it of the
    // opposite lightness — seen on any window and wallpaper.
    Color sign_bar;
    Color sign_halo;

    Gradient send;                // the send button
    std::array<Shadow, 2> shadow; // the window's: a wide one and a near one
    std::array<Shadow, 2> pop_shadow; // a pop's
    OrbLight orb;
    Radii radius;

    std::string_view name; // "Dark", "Light"
    bool light = false;    // a light ground: what lights up on a dark theme darkens here
};

inline constexpr std::size_t kColorTokens = 53;

struct ColorToken {
    std::string_view name; // as a theme file will spell it: "surface", "text_2"
    Color Theme::* field;
};

[[nodiscard]] auto color_tokens() noexcept -> std::span<const ColorToken>;

// The themes that come with the agent: Dark first.
[[nodiscard]] auto builtin() noexcept -> std::span<const Theme>;
[[nodiscard]] auto dark() noexcept -> const Theme&;
[[nodiscard]] auto light() noexcept -> const Theme&;
// A built-in theme by name, whatever its letters' case; nullptr when there is none.
[[nodiscard]] auto find(std::string_view name) noexcept -> const Theme*;

} // namespace theme
