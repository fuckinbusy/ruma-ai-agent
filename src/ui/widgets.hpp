#pragma once

// ui::widgets — the sheet's pieces from design/agent-placements.html, as immediate-mode free
// functions: each draws into the box the layout gave it and returns what the user did with it.
// Metrics and colours come from ui::theme. Streamed text fading in asks the Ui for the next frame
// while it moves; the caret asks for none (the shell wakes at its flips).

#include "ui/layout.hpp"
#include "ui/text/paragraph.hpp"
#include "ui/ui.hpp"

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace ui::widgets {

// Segoe Fluent Icons code points for the mockup's SVG icons.
inline constexpr char32_t kIconUp = 0xE74A;        // send: arrow up
inline constexpr char32_t kIconChevronUp = 0xE70E; // collapse
inline constexpr char32_t kIconCheck = 0xE73E;     // done badge

// The resting sheet: a glowing violet-white-cyan bar on the top edge.
// The resting strip: violet to white to cyan, the white at `highlight` (0..1 of its width), its
// glow at `glow` strength.
void handle(Ui& ui, const layout::Box& box, float radius, float highlight = 0.5f, float glow = 1.0f);
// The sheet body hanging from the top edge, bottom corners rounded.
void panel(Ui& ui, const layout::Box& box, float radius);

auto send_button(Ui& ui, Id id, const layout::Box& box) -> bool;
auto icon_button(Ui& ui, Id id, const layout::Box& box, char32_t icon) -> bool;
auto pill_button(Ui& ui, Id id, const layout::Box& box, std::string_view label, bool primary)
    -> bool;
auto pill_size(Ui& ui, std::string_view label) -> layout::Size;

void chip(Ui& ui, const layout::Box& box, std::string_view label);
auto chip_size(Ui& ui, std::string_view label) -> layout::Size;
void ok_badge(Ui& ui, const layout::Box& box);

// The capsule input at the bottom of the answer card, with its placeholder. Only `hit` takes the
// mouse: the button inside the field is registered after it and must get its own presses.
auto field(Ui& ui, Id id, const layout::Box& box, const layout::Box& hit, std::string_view placeholder)
    -> Interaction;
// The user's question: background shape (draw the text inside with the bubble paddings).
void bubble(Ui& ui, const layout::Box& box);
// A blinking caret: visible during the first half of each second since `since`. `stream` draws
// the answer's chrome block caret instead of the input's thin white bar.
void caret(Ui& ui, const layout::Box& box, double since, bool stream = false);

// When each part of a streamed answer arrived: tokens fade in from their arrival time.
class StreamTimes {
public:
    void mark(std::uint32_t end_byte, double time) {
        if (marks_.capacity() == 0) {
            marks_.reserve(512); // a long answer's tokens: no growth while it prints
        }
        marks_.push_back({end_byte, time});
    }
    void clear() noexcept { marks_.clear(); }
    // Arrival time of the token holding `byte`; very old when unknown.
    [[nodiscard]] auto time_of(std::uint32_t byte) const noexcept -> double;

private:
    struct Mark {
        std::uint32_t end;
        double time;
    };
    std::vector<Mark> marks_;
};

// How a streamed paragraph looks: a colour per face (Glyph::face) and the line height as a
// multiple of its size.
struct StreamStyle {
    std::array<render::Color, text::Paragraph::kMaxFaces> colors;
    float line;
};
// .resp: prose, bold white.
[[nodiscard]] auto answer_style() noexcept -> StreamStyle;

// A streamed answer paragraph (shaped at its size x the Ui scale), with the chrome caret after
// the last character while `with_caret`.
void stream_text(Ui& ui, const layout::Box& box, const text::Paragraph& paragraph,
                 const StreamTimes& times, bool with_caret, const StreamStyle& style = answer_style());

} // namespace ui::widgets
