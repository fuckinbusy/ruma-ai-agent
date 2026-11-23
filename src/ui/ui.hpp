#pragma once

// ui::Ui — the per-window immediate-mode context: one frame's input, time and DPI scale, the
// interaction state that outlives frames (pressed widget, keyboard focus), text and icon
// drawing through the shared atlas, and the animator. Widgets are free functions taking a Ui&.
//
// Interaction: interact(id, box) reports hover/held/clicked for the box laid out this frame.
// The widget pressed first captures the mouse until release; a click is a press and a release
// on the same widget. Tab / Shift+Tab move keyboard focus in drawing order; Enter or Space
// (Input::activate) clicks the focused widget.

#include "core/error.hpp"
#include "ui/anim.hpp"
#include "ui/font_library.hpp"
#include "ui/layout.hpp"
#include "ui/render/renderer.hpp"
#include "ui/text/atlas.hpp"
#include "ui/text/fonts.hpp"
#include "ui/text/paragraph.hpp"
#include "ui/text/shape.hpp"
#include "ui/selection.hpp"
#include "ui/text_map.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ui {

// Widget identity, stable across frames (phase 3: a hash of the widget's path). 0 means "no
// widget" (nothing pressed, nothing focused) and must not be used as an id.
using Id = std::uint64_t;

// Window input for one frame, logical pixels.
struct Input {
    float mouse_x = -1.0e9f;
    float mouse_y = -1.0e9f;
    bool mouse_down = false;
    bool mouse_pressed = false;  // went down during this frame
    bool mouse_released = false; // went up during this frame
    bool tab = false;
    bool shift = false;
    bool activate = false; // Enter or Space
    float wheel = 0;       // wheel movement this frame, in lines (positive: away from the user)
    float notches = 0;     // the same in notches: 1 a wheel's click, a touchpad's move a part of one
    // Responsive text (selection): a press's clicks in a row (2 a double click, 3 a triple), the
    // right button pressed this frame, Ctrl+A.
    int clicks = 1;
    bool right_pressed = false;
    bool select_all = false;
    bool copy = false; // Ctrl+C: the selection's text made at this frame's end (take_copied)
};

struct Interaction {
    bool hovered = false;
    bool held = false;
    bool clicked = false;
    bool focused = false;
    bool focus_ring = false; // focused and reached by the keyboard: the ring is drawn
};

// The mouse cursor the frame wants over the window (WM_SETCURSOR).
enum class Cursor : std::uint8_t { Arrow, Hand, IBeam };

// The faces of the mockup: --font (Segoe UI Variable Text) regular and semibold, --font-d
// (Display) semibold, and Segoe Fluent Icons for the glyph icons.
// Italic and SemiboldItalic (Markdown's emphasis, phase 8) are loaded on first use.
enum class Face : std::uint8_t { Text, Semibold, Display, Icons, Mono, Italic, SemiboldItalic };

struct Fonts {
    text::FontSet text;
    text::FontSet semibold;
    text::FontSet display;
    text::FontSet icons;
    text::FontSet mono; // --mono: commands and the terminal
    // Loaded the first time they are asked for (the start does not open them): Segoe UI's
    // italics; a missing file falls back to the upright face, once.
    FontLibrary* library = nullptr;
    std::optional<text::FontSet> italic;
    std::optional<text::FontSet> semibold_italic;
    bool italic_tried = false;
    bool semibold_italic_tried = false;

    static auto system(FontLibrary& library) -> core::Result<Fonts>;
    auto get(Face face) noexcept -> text::FontSet&;
};

class Ui {
public:
    Ui(render::Renderer& renderer, text::GlyphAtlas& atlas, Fonts& fonts)
        : renderer_(&renderer), atlas_(&atlas), fonts_(&fonts) {}

    void begin_frame(double now_seconds, float scale, const Input& input);
    void end_frame();

    auto interact(Id id, const layout::Box& box, bool focusable = true) -> Interaction;
    // While inert, interact() reports nothing and registers nothing: a layer fading out is drawn
    // but cannot be clicked or focused.
    void set_inert(bool inert) noexcept { inert_ = inert; }
    [[nodiscard]] auto inert() const noexcept -> bool { return inert_; }
    // While pushed, interact() takes the mouse only inside `box` (and the boxes pushed before):
    // a scrolled view's content outside it is clipped away and lies under other widgets.
    void push_hit_clip(const layout::Box& box) noexcept;
    void pop_hit_clip() noexcept;
    // A layer over the rest (a menu, a list; final review I1, I2): what lies under its box gets no
    // press — the box as the last frame drew it — save the layer's own widgets, between begin and end.
    void begin_overlay(const layout::Box& box) noexcept;
    void end_overlay() noexcept { in_overlay_ = false; }
    // The wheel for a scrolled view (`who` names it across frames): the innermost view under the
    // mouse takes it, as the last frame laid them out (a card's text inside the conversation);
    // with none under it then, each view under it now.
    auto wheel_for(const void* who, const layout::Box& viewport) noexcept -> float;
    // Typing goes to the text field: no button keeps the keyboard focus.
    void clear_focus() noexcept { focus_ = 0; }
    // 0..1, fading towards 1 while `on`: in on quick, out on micro (motion rework).
    auto fade(Id id, bool on) -> float;
    // A hover's 0..1 (quick both ways), a press's depth 0..1 (down on micro, back on base) — MS
    // scene 14; a tooltip (after 500 ms of hover; at once for a neighbour), the cursor wanted, a
    // disabled stretch (no mouse, the arrow).
    auto hover(Id id, bool on) -> float;
    auto press(Id id, bool held) -> float;
    void tooltip(Id id, const layout::Box& anchor, std::string_view title, std::string_view sub = {}, std::string_view key = {});
    [[nodiscard]] auto tooltip_shown() const noexcept -> Id { return tip_shown_; }
    // After end_frame(): the tooltip drawn in that frame, if any.
    [[nodiscard]] auto tip_box() const noexcept -> std::optional<layout::Box> { return tip_drawn_; }
    [[nodiscard]] auto tip_opacity() const noexcept -> float { return tip_alpha_; }
    // After end_frame(): the bottom of the tooltip asked in that frame (waiting or shown) as it is
    // placed under its widget, when it has no room above it either; 0: none asked, or it can turn
    // up. A window too short for it grows by it (the bar's, at the screen's top).
    [[nodiscard]] auto tip_bottom() -> float;
    void want_cursor(Cursor c) noexcept { cursor_now_ = c; }
    // After end_frame(): the cursor the last frame wanted.
    [[nodiscard]] auto cursor() const noexcept -> Cursor { return cursor_; }
    void set_disabled(bool on) noexcept { disabled_ = on; }
    [[nodiscard]] auto disabled() const noexcept -> bool { return disabled_; }
    // A widget that changes every frame (spinner, caret) asks for the next one.
    void request_frame() noexcept { requested_ = true; }
    // After end_frame(): whether anything still moves and the window should draw again.
    [[nodiscard]] auto wants_frame() const noexcept -> bool { return wants_frame_; }

    // Text in logical sizes; shaping happens at size x scale and is cached.
    // `tabular`: figures of one width (numbers in columns).
    auto shape(Face face, std::string_view text, float size, bool tabular = false) -> const text::Shaped&;
    // Line metrics in logical pixels for a face and size.
    auto line_height(Face face, float size) -> float;
    auto ascent(Face face, float size) -> float;
    auto descent(Face face, float size) -> float;
    void draw_text(Face face, const text::Shaped& shaped, float size, text::Point baseline,
                   render::Color color);
    // `text` in `face` at `size`, wrapped at `width` (logical pixels; its lines' widths are
    // physical): shaped once and kept while it is asked for (the least recently asked one makes
    // room, never one asked in this frame: then there is one more); a new width wraps it again
    // without shaping. A steady frame finds them all and allocates nothing.
    static constexpr std::size_t kParagraphs = 96;
    auto paragraph(Face face, std::string_view text, float size, float width) -> const text::Paragraph&;

    // The frame's texts (text_map.hpp), emptied by begin_frame: what a selection is made of.
    [[nodiscard]] auto text_map() noexcept -> TextMap& { return text_map_; }
    [[nodiscard]] auto text_map() const noexcept -> const TextMap& { return text_map_; }
    // The pane texts are drawn in (the conversation, the data panel, a card over them): a selection
    // stays in one. 0 (none pushed): the texts are not selectable.
    void push_pane(Id pane) noexcept;
    void pop_pane() noexcept;
    [[nodiscard]] auto pane() const noexcept -> Id { return pane_depth_ == 0 ? 0 : panes_[std::min(pane_depth_, panes_.size()) - 1]; }
    // While off, texts drawn are not selectable (a button's or a tab's word): their key's pane is 0.
    void set_selectable(bool on) noexcept { selectable_ = on; }
    [[nodiscard]] auto selectable() const noexcept -> bool { return selectable_; }
    // The next text's key in reading order: the pane's (0 while not selectable), the next order.
    // A text not selectable (a button's word, a layer fading out) takes no order: it never moves the others'.
    auto next_key() noexcept -> TextKey {
        return selectable_ && !inert_ && pane() != 0 ? TextKey{pane(), ++order_} : TextKey{};
    }
    // Where the next texts' orders start (final review C1): a screen gives each block it may skip
    // (out of view) or draw differently (under the mouse) a base of its own — a turn, an answer's
    // block, an Activity entry, a table's row — so a text's key stays the same whatever was drawn
    // before it, and a selection stays on its characters. Bases grow in reading order.
    void order_base(std::uint64_t base) noexcept { order_ = base; }
    // Puts a drawn line into the map: `run`'s key is `from`'s when it has one, else the next; its
    // scale the frame's.
    void note_text(TextRun run, std::string_view text, std::string_view full, const TextFrom& from);
    // The text selected with the mouse (selection.hpp): a press on text and a drag of more than
    // 4 px selects (the press then clicks nothing), a double click a word, a triple its text;
    // shift extends; a press elsewhere clears it. Handled at end_frame on that frame's map.
    [[nodiscard]] auto selection() const noexcept -> const Selection& { return selection_; }
    void clear_selection() noexcept { selection_.clear(); }
    // A selection is being dragged (a scrolled view under it scrolls when the mouse is past it).
    [[nodiscard]] auto selecting() const noexcept -> bool { return selecting_; }
    // This press is a widget's drag (a scroll bar's thumb): it starts no selection, clears none.
    void claim_press() noexcept { press_claimed_ = true; }
    // Ctrl+A, the menu's Select all: all of the pane at (x, y), as the last frame drew it.
    void select_all_at(float x, float y);
    // The selectable pane whose text is nearest (x, y), within 200 px; 0: none.
    [[nodiscard]] auto pane_at(float x, float y) const -> Id;
    // Its text (Ctrl+C), from the last frame: false when nothing is selected. Only within a frame or
    // right after it (a pane's text reads the data that frame drew): the shell asks with Input::copy.
    auto copy_selection(std::string& out) const -> bool;
    // The text Input::copy asked for, made at that frame's end; once (final review I7).
    auto take_copied(std::string& out) -> bool;
    // A pane's whole text when it is more than its drawn lines (a table's rows): Ctrl+A copies it.
    // Asked for again every frame; `context` lives until the next frame.
    using PaneText = void (*)(const void* context, std::string& out);
    void pane_text(Id pane, PaneText text, const void* context) noexcept;
    // The cell the next texts belong to (a table's row and column; -1: none).
    void set_cell(int row, int col) noexcept {
        cell_row_ = row;
        cell_col_ = col;
    }
    // A paragraph's line drawn and put into the map (the conversation's question, the field).
    void draw_line(const text::Paragraph& paragraph, const text::Line& line, text::Point baseline, render::Color color,
                   const TextFrom& from = {});
    // The line `zoom` times its size from `baseline`: a picture of text in flight — not selectable,
    // not noted as text (MOTION_SPEC scene 05).
    void draw_line_zoomed(const text::Paragraph& paragraph, const text::Line& line, text::Point baseline, render::Color color, float zoom);
    // The same for a shaped run: a small mark coming in at .9 of its size (MS § 7).
    void draw_text_zoomed(Face face, const text::Shaped& shaped, float size, text::Point baseline, render::Color color, float zoom);
    // Baseline that centres a line of `face` at `size` vertically in [top, top + height).
    auto centred_baseline(Face face, float size, float top, float height) -> float;
    // One icon glyph (Segoe Fluent Icons code point) with its ink centred on `centre`.
    // `zoom` scales the drawn glyph, not its size: an icon that grows and shrinks (a button
    // springing in) is rasterized and shaped once, not at every size it passes.
    void draw_icon(char32_t icon, float size, text::Point centre, render::Color color, float zoom = 1.0f);

    [[nodiscard]] auto renderer() noexcept -> render::Renderer& { return *renderer_; }
    [[nodiscard]] auto atlas() noexcept -> text::GlyphAtlas& { return *atlas_; }
    [[nodiscard]] auto fonts() noexcept -> Fonts& { return *fonts_; }
    [[nodiscard]] auto animator() noexcept -> anim::Animator& { return anim_; }
    [[nodiscard]] auto now() const noexcept -> double { return now_; }
    [[nodiscard]] auto scale() const noexcept -> float { return scale_; }
    // Text (not icons) is shaped at size x scale x text scale: the user's text size setting.
    void set_text_scale(float s) noexcept { text_scale_ = s > 0 ? s : 1.0f; }
    [[nodiscard]] auto text_scale() const noexcept -> float { return text_scale_; }
    // The panels' surface opacity (the "Window transparency" setting): its fill's alpha.
    void set_surface_alpha(std::uint8_t a) noexcept { surface_alpha_ = a; }
    [[nodiscard]] auto surface_alpha() const noexcept -> std::uint8_t { return surface_alpha_; }
    [[nodiscard]] auto input() const noexcept -> const Input& { return input_; }
    [[nodiscard]] auto focus() const noexcept -> Id { return focus_; }

private:
    render::Renderer* renderer_;
    text::GlyphAtlas* atlas_;
    Fonts* fonts_;
    text::ShapeCache shapes_;
    struct Wrapped {
        std::uint64_t key = 0; // the text's hash, its face and size
        std::uint64_t asked = 0; // the last ask's number (the least recent makes room)
        std::uint64_t frame = 0; // the frame it was last asked in: never given up in it
        float width = -1;      // physical
        std::optional<text::Paragraph> paragraph;
    };
    // At least kParagraphs; more when one frame asks for more (final review C2: a long Activity), kept.
    std::vector<std::unique_ptr<Wrapped>> wrapped_;
    std::uint64_t asks_ = 0;
    std::uint64_t frames_ = 0;
    TextMap text_map_;
    std::array<Id, 8> panes_{};
    std::size_t pane_depth_ = 0;
    bool selectable_ = true;
    std::uint64_t order_ = 0;
    Selection selection_;
    int cell_row_ = -1;
    int cell_col_ = -1;
    struct Provider {
        Id pane = 0;
        PaneText text = nullptr;
        const void* context = nullptr;
    };
    std::array<Provider, 8> providers_{};
    std::size_t provider_count_ = 0;
    std::array<Id, 16> panes_seen_{}; // this frame's panes: a selection's gone with its pane
    std::size_t panes_seen_count_ = 0;
    // A press on selectable text, not yet a drag: where it was and what it pressed.
    std::optional<TextPos> press_at_;
    float press_x_ = 0;
    float press_y_ = 0;
    bool selecting_ = false;
    bool press_claimed_ = false;
    std::string copied_; // take_copied's
    bool copied_ready_ = false;
    std::array<layout::Box, 4> overlays_now_{};  // this frame's layers
    std::array<layout::Box, 4> overlays_last_{}; // the last frame's: what is under them gets no press
    std::size_t overlays_now_count_ = 0;
    std::size_t overlays_last_count_ = 0;
    bool in_overlay_ = false;
    [[nodiscard]] auto under_overlay(float x, float y) const noexcept -> bool;
    void handle_selection();
    // The character under (x, y) in `pane`: on its line, or the nearest line's end.
    [[nodiscard]] auto pos_in_pane(Id pane, float x, float y) const -> std::optional<TextPos>;
    // The scrolled views asked for the wheel, this frame's and the last one's, outer before inner.
    std::array<std::array<std::pair<const void*, layout::Box>, 16>, 2> wheel_views_{};
    std::size_t now_list_ = 0;
    std::size_t wheel_views_now_ = 0;
    std::size_t wheel_views_before_ = 0;
    const void* wheel_owner_ = nullptr; // this frame's: the innermost of the last frame's under the mouse
    bool wheel_owner_known_ = false;
    anim::Animator anim_;
    Input input_;
    double now_ = 0;
    float scale_ = 1.0f;
    float text_scale_ = 1.0f;
    std::uint8_t surface_alpha_ = 255; // opaque: the setting's default
    // Physical pixels for a text size.
    [[nodiscard]] auto px(Face face, float size) const noexcept -> float {
        return size * scale_ * (face == Face::Icons ? 1.0f : text_scale_);
    }
    Id active_ = 0; // pressed widget, holds the mouse until release
    Id focus_ = 0;
    std::vector<Id> focus_order_; // focusable widgets of this frame, in drawing order
    bool requested_ = false;
    bool wants_frame_ = false;
    bool inert_ = false;
    bool disabled_ = false;
    bool focus_visible_ = false; // Tab moved the focus since the last mouse press
    Cursor cursor_now_ = Cursor::Arrow;
    Cursor cursor_ = Cursor::Arrow;
    // The tooltip: asked this frame (id 0: none), hovered since, shown; its words kept (their
    // buffers reused: no allocation once they have room).
    Id tip_asked_ = 0;
    Id tip_pending_ = 0;
    Id tip_shown_ = 0;
    double tip_since_ = 0;
    double tip_gone_at_ = -1.0e9; // when the last shown one went: a neighbour shows at once
    layout::Box tip_anchor_{};
    std::string tip_title_, tip_sub_, tip_key_;
    Id tip_last_ = 0;                        // the one asked in the last frame (tip_bottom)
    std::optional<layout::Box> tip_drawn_;   // tip_box()
    float tip_alpha_ = 0.0f;                 // its opacity as drawn (tip_opacity())
    bool tip_drawn_was_ = false;             // drawn in the last frame (going: as it was)
    [[nodiscard]] auto tip_height() -> float;
    [[nodiscard]] auto tip_title_width() const noexcept -> float;
    // `fresh`: none was in sight — it comes in at its place, kRise px off.
    void draw_tooltip(bool fresh);
    std::array<layout::Box, 8> hit_clips_{}; // each the intersection with the ones under it
    std::size_t hit_depth_ = 0;              // pushes; past the array, the top one repeats
};

} // namespace ui
