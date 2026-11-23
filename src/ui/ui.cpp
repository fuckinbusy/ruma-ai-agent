#include "ui/ui.hpp"

#include "ui/theme.hpp"

#include "core/hash.hpp"
#include "core/utf8.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace ui {
namespace {


// The tooltip's own tracks ("tip" and a number: apart from any widget's id).
constexpr anim::Id kTipAlpha = 0x7469700000000001ull;
constexpr anim::Id kTipX = 0x7469700000000002ull;
constexpr anim::Id kTipY = 0x7469700000000003ull;

} // namespace

auto Fonts::system(FontLibrary& library) -> core::Result<Fonts> {
    auto text = text::FontSet::system_ui(library, 400.0f, 10.5f);
    auto semibold = text::FontSet::system_ui(library, 600.0f, 10.5f);
    auto display = text::FontSet::system_ui(library, 600.0f, 36.0f);
    auto icons = text::FontSet::system_icons(library);
    auto mono = text::FontSet::system_mono(library);
    for (const auto* set : {&text, &semibold, &display, &icons, &mono}) {
        if (!*set) {
            return std::unexpected{set->error()};
        }
    }
    Fonts fonts{std::move(*text), std::move(*semibold), std::move(*display), std::move(*icons), std::move(*mono)};
    fonts.library = &library;
    return fonts;
}

auto Fonts::get(Face face) noexcept -> text::FontSet& {
    switch (face) {
    case Face::Semibold: return semibold;
    case Face::Display: return display;
    case Face::Icons: return icons;
    case Face::Mono: return mono;
    case Face::Italic:
    case Face::SemiboldItalic: {
        const bool bold = face == Face::SemiboldItalic;
        auto& set = bold ? semibold_italic : italic;
        bool& tried = bold ? semibold_italic_tried : italic_tried;
        if (!set && !tried && library != nullptr) {
            tried = true;
            if (auto loaded = text::FontSet::system_italic(*library, bold)) {
                set.emplace(std::move(*loaded));
            }
        }
        return set ? *set : (bold ? semibold : text);
    }
    case Face::Text: break;
    }
    return text;
}

void Ui::begin_frame(double now_seconds, float scale, const Input& input) {
    now_ = now_seconds;
    scale_ = scale;
    input_ = input;
    requested_ = false;
    inert_ = false;
    disabled_ = false;
    hit_depth_ = 0;
    cursor_now_ = Cursor::Arrow;
    tip_asked_ = 0;
    if (input.mouse_pressed) {
        focus_visible_ = false; // the mouse: no focus ring (screens 26–27)
    }
    focus_order_.clear();
    anim_.begin_frame(now_seconds);
    atlas_->next_frame();
    text_map_.begin_frame();
    ++frames_;
    pane_depth_ = 0;
    selectable_ = true;
    order_ = 0;
    provider_count_ = 0;
    panes_seen_count_ = 0;
    cell_row_ = cell_col_ = -1;
}

auto Ui::take_copied(std::string& out) -> bool {
    if (!copied_ready_) {
        return false;
    }
    copied_ready_ = false;
    out = copied_;
    return true;
}

void Ui::begin_overlay(const layout::Box& box) noexcept {
    if (overlays_now_count_ < overlays_now_.size()) {
        overlays_now_[overlays_now_count_++] = box;
    }
    in_overlay_ = true;
}

auto Ui::under_overlay(float x, float y) const noexcept -> bool {
    for (std::size_t i = 0; i < overlays_last_count_; ++i) {
        const auto& b = overlays_last_[i];
        if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) {
            return true;
        }
    }
    return false;
}

auto Ui::copy_selection(std::string& out) const -> bool {
    out.clear();
    if (selection_.whole()) { // all of a pane: its provider's text when it has one (a table's rows)
        for (std::size_t i = 0; i < provider_count_; ++i) {
            if (providers_[i].pane == selection_.pane()) {
                providers_[i].text(providers_[i].context, out);
                return !out.empty();
            }
        }
    }
    selection_.copy(text_map_, out);
    return !out.empty();
}

void Ui::pane_text(Id pane, PaneText text, const void* context) noexcept {
    if (provider_count_ < providers_.size()) {
        providers_[provider_count_++] = {pane, text, context};
    }
}

namespace {

// A word's bytes: what a double click selects (letters, digits, any non-ASCII).
auto in_word(char c) noexcept -> bool {
    constexpr std::string_view kBreaks = " \t\r\n.,;:!?()[]{}<>\"'|\\/=&*+`~";
    return kBreaks.find(c) == std::string_view::npos;
}

} // namespace

auto Ui::pos_in_pane(Id pane, float x, float y) const -> std::optional<TextPos> {
    // The line under the mouse, else the nearest one above or below; on it, the run under x.
    const TextRun* best = nullptr;
    float best_dy = 0;
    for (const auto& run : text_map_.runs()) {
        if (run.key.pane != pane || pane == 0) {
            continue;
        }
        const float dy = y < run.box.y ? run.box.y - y : y >= run.box.y + run.box.h ? y - (run.box.y + run.box.h) + 0.001f : 0.0f;
        if (best == nullptr || dy < best_dy - 0.5f || (std::abs(dy - best_dy) <= 0.5f && run.box.x <= x && run.box.x > best->box.x) ||
            (std::abs(dy - best_dy) <= 0.5f && best->box.x > x && run.box.x < best->box.x)) {
            best = &run;
            best_dy = dy;
        }
    }
    if (best == nullptr) {
        return std::nullopt;
    }
    const std::uint32_t end = best->first_byte + best->text_length;
    if (best_dy > 0) { // off its line: above it, its start; below it, its end
        return TextPos{best->key, y < best->box.y ? best->first_byte : end};
    }
    return TextPos{best->key, text_map_.byte_at(*best, x)};
}

void Ui::select_all_at(float x, float y) {
    selection_.select_all(pane_at(x, y));
}

auto Ui::pane_at(float x, float y) const -> Id {
    // The pane of the text nearest the point (its own, under it), within 200 px.
    Id pane = 0;
    float nearest = 1.0e9f;
    for (const auto& run : text_map_.runs()) {
        if (run.key.pane == 0) {
            continue;
        }
        const auto& b = run.box;
        const float dx = std::max({b.x - x, 0.0f, x - (b.x + b.w)});
        const float dy = std::max({b.y - y, 0.0f, y - (b.y + b.h)});
        if (dx + dy < nearest) {
            nearest = dx + dy;
            pane = run.key.pane;
        }
    }
    return nearest < 200 ? pane : 0;
}

void Ui::handle_selection() {
    const auto& in = input_;
    // Its pane gone (a card answered, the sheet closed): the selection with it.
    if (selection_.active() && std::find(panes_seen_.begin(), panes_seen_.begin() + static_cast<std::ptrdiff_t>(panes_seen_count_),
                                         selection_.pane()) == panes_seen_.begin() + static_cast<std::ptrdiff_t>(panes_seen_count_)) {
        selection_.clear();
    }
    const auto under = [&]() -> std::optional<TextPos> {
        if (under_overlay(in.mouse_x, in.mouse_y)) {
            return std::nullopt; // a menu over the text: its, not the text's
        }
        for (const auto& run : text_map_.runs()) {
            const auto& b = run.box;
            if (run.key.pane != 0 && in.mouse_x >= b.x && in.mouse_x < b.x + b.w && in.mouse_y >= b.y && in.mouse_y < b.y + b.h) {
                return TextPos{run.key, text_map_.byte_at(run, in.mouse_x)};
            }
        }
        return std::nullopt;
    };
    if (in.select_all) {
        select_all_at(in.mouse_x, in.mouse_y);
    }
    if (in.mouse_pressed && press_claimed_) {
        press_at_.reset(); // a scroll bar's thumb: no selection begins, none ends
    } else if (in.mouse_pressed) {
        const auto hit = under();
        if (in.shift && hit && selection_.active() && !selection_.whole() && hit->key.pane == selection_.pane()) {
            selection_.extend(*hit);
            selecting_ = true;
            active_ = 0;
        } else if (hit && in.clicks >= 2) {
            // A word (a double click), its whole text (a triple one).
            std::uint32_t from = hit->byte, to = hit->byte;
            for (const auto& run : text_map_.runs()) {
                if (run.key != hit->key) {
                    continue;
                }
                const std::uint32_t end = run.first_byte + run.text_length;
                if (in.clicks >= 3) {
                    from = std::min(from, run.first_byte);
                    to = std::max(to, end);
                } else if (hit->byte >= run.first_byte && hit->byte <= end) {
                    const auto text = text_map_.text(run);
                    std::uint32_t l = hit->byte - run.first_byte, r = l;
                    while (l > 0 && in_word(text[l - 1])) {
                        --l;
                    }
                    while (r < text.size() && in_word(text[r])) {
                        ++r;
                    }
                    from = run.first_byte + l;
                    to = run.first_byte + r;
                }
            }
            selection_.set({hit->key, from}, {hit->key, to});
            press_at_.reset();
            selecting_ = false;
        } else if (!hit && active_ != 0) {
            press_at_.reset(); // a button, a menu item: it acts on the selection (Copy), it keeps it
        } else {
            selection_.clear(); // a press elsewhere, or the start of a new one
            press_at_ = hit;
            press_x_ = in.mouse_x;
            press_y_ = in.mouse_y;
            selecting_ = false;
        }
    }
    if ((in.mouse_down || in.mouse_released) && press_at_ && !selecting_ &&
        std::hypot(in.mouse_x - press_x_, in.mouse_y - press_y_) > 4.0f) {
        selecting_ = true; // a drag, not a click: the widget pressed clicks nothing
        selection_.set(*press_at_, *press_at_);
        active_ = 0;
    }
    if (selecting_ && (in.mouse_down || in.mouse_released)) {
        if (const auto at = pos_in_pane(selection_.pane(), in.mouse_x, in.mouse_y)) {
            selection_.extend(*at);
        }
    }
    if (in.mouse_released || !in.mouse_down) {
        selecting_ = false;
        press_at_.reset();
        press_claimed_ = false;
    }
    selection_.seen(text_map_);
    // Over selectable text and nothing that clicks: the I-beam.
    if (cursor_now_ == Cursor::Arrow && (under() || selecting_)) {
        cursor_now_ = Cursor::IBeam;
    }
}

void Ui::push_pane(Id pane) noexcept {
    if (pane_depth_ < panes_.size()) {
        panes_[pane_depth_] = pane;
    }
    ++pane_depth_;
    if (panes_seen_count_ < panes_seen_.size() &&
        std::find(panes_seen_.begin(), panes_seen_.begin() + static_cast<std::ptrdiff_t>(panes_seen_count_), pane) ==
            panes_seen_.begin() + static_cast<std::ptrdiff_t>(panes_seen_count_)) {
        panes_seen_[panes_seen_count_++] = pane;
    }
}

void Ui::pop_pane() noexcept {
    if (pane_depth_ > 0) {
        --pane_depth_;
    }
}

void Ui::note_text(TextRun run, std::string_view text, std::string_view full, const TextFrom& from) {
    run.key = from.key.order != 0 ? from.key : next_key();
    run.scale = scale_;
    run.soft_break_after = run.soft_break_after || from.soft_break_after;
    run.row = from.row >= 0 ? from.row : cell_row_;
    run.col = from.col >= 0 ? from.col : cell_col_;
    run.text_length = static_cast<std::uint32_t>(text.size());
    // Selected (as the last frame left the selection): lit over its glyphs.
    if (const auto [a, b] = selection_.covered(run); a != b) {
        const float x0 = text_map_.x_of(run, a);
        const float x1 = std::max(text_map_.x_of(run, b), x0 + 3.0f);
        renderer_->rect({x0, run.box.y, x1 - x0, run.box.h}, {.fill = th().selection});
    }
    text_map_.add(run, text, full.empty() ? from.full : full);
}

void Ui::draw_line(const text::Paragraph& paragraph, const text::Line& line, text::Point baseline, render::Color color,
                   const TextFrom& from) {
    text::draw(*renderer_, *atlas_, paragraph.fonts(), paragraph.shaped(), line, paragraph.px(), baseline, color, scale_);
    const float ascent = paragraph.fonts().sized(0, paragraph.px()).ascent / scale_;
    const float height = paragraph.fonts().sized(0, paragraph.px()).line_height / scale_;
    TextRun run;
    run.box = {baseline.x, baseline.y - ascent, line.width / scale_, height};
    run.baseline = baseline.y;
    run.first_byte = line.first_byte;
    run.shaped = &paragraph.shaped();
    run.first_glyph = line.first_glyph;
    run.end_glyph = line.end_glyph;
    // Wrapped here (the next line goes on after its spaces), not ended by the text's own line end.
    const auto text = paragraph.text();
    std::size_t next = line.end_byte;
    while (next < text.size() && (text[next] == ' ' || text[next] == '\t')) {
        ++next;
    }
    run.soft_break_after = next < text.size() && text[next] != '\n' && text[next] != '\r';
    // A line keeps the spaces it hangs (wrapped, before its line end, at the text's end): copied,
    // the text is its source byte for byte.
    const std::size_t end = next;
    note_text(run, text.substr(line.first_byte, end - line.first_byte), {}, from);
}

void Ui::draw_line_zoomed(const text::Paragraph& paragraph, const text::Line& line, text::Point baseline, render::Color color, float zoom) {
    text::draw_zoomed(*renderer_, *atlas_, paragraph.fonts(), paragraph.shaped(), line, paragraph.px(), baseline, color, scale_, zoom);
}

auto Ui::wheel_for(const void* who, const layout::Box& viewport) noexcept -> float {
    if (wheel_views_now_ < wheel_views_[now_list_].size()) {
        wheel_views_[now_list_][wheel_views_now_++] = {who, viewport};
    }
    const bool under = input_.mouse_x >= viewport.x && input_.mouse_x < viewport.x + viewport.w && input_.mouse_y >= viewport.y &&
                       input_.mouse_y < viewport.y + viewport.h;
    if (!under) {
        return 0;
    }
    if (!wheel_owner_known_) { // the innermost of the last frame's views under the mouse now
        wheel_owner_known_ = true;
        wheel_owner_ = nullptr;
        const auto& before = wheel_views_[1 - now_list_];
        for (std::size_t i = wheel_views_before_; i-- > 0;) {
            const auto& [view, box] = before[i];
            if (input_.mouse_x >= box.x && input_.mouse_x < box.x + box.w && input_.mouse_y >= box.y && input_.mouse_y < box.y + box.h) {
                wheel_owner_ = view;
                break;
            }
        }
    }
    // None of them (a view new in this frame): each one under the mouse.
    return wheel_owner_ == nullptr || wheel_owner_ == who ? input_.wheel : 0.0f;
}

void Ui::end_frame() {
    now_list_ = 1 - now_list_;
    wheel_views_before_ = wheel_views_now_;
    wheel_views_now_ = 0;
    wheel_owner_known_ = false;
    if (input_.tab && !focus_order_.empty()) {
        const auto it = std::ranges::find(focus_order_, focus_);
        const auto n = static_cast<std::ptrdiff_t>(focus_order_.size());
        if (it == focus_order_.end()) {
            focus_ = input_.shift ? focus_order_.back() : focus_order_.front();
        } else {
            const auto i = it - focus_order_.begin();
            focus_ = focus_order_[static_cast<std::size_t>((i + (input_.shift ? n - 1 : 1)) % n)];
        }
        focus_visible_ = true;
    }
    handle_selection();
    if (input_.copy) { // Ctrl+C: made while this frame's data is the data drawn (final review I7)
        copied_ready_ = copy_selection(copied_);
    }
    tip_last_ = tip_asked_;
    tip_drawn_.reset();
    // The tooltip: after its delay of hover on one widget; at once when one was shown a moment ago
    // (MS § 7: in on quick, 4 px on base, out on micro, its place moving to a neighbour on base).
    using seconds = std::chrono::duration<double>;
    const bool visible = tip_drawn_was_;
    if (tip_asked_ == 0) {
        if (tip_shown_ != 0) {
            tip_gone_at_ = now_;
        }
        tip_pending_ = 0;
        tip_shown_ = 0;
    } else {
        if (tip_asked_ != tip_pending_) {
            tip_pending_ = tip_asked_;
            tip_since_ = now_;
        }
        const bool neighbour = tip_shown_ != 0 || now_ - tip_gone_at_ <= seconds(motion::tooltip::kNeighbourFor).count();
        if (tip_shown_ == tip_asked_ || neighbour || now_ - tip_since_ >= seconds(motion::tooltip::kDelay).count() - 1e-6) {
            tip_shown_ = tip_asked_;
        } else {
            requested_ = true; // its timer runs on frames, only while the mouse rests on it
        }
    }
    if (tip_shown_ != 0 && !visible) {
        anim_.place(kTipAlpha, 0.0f); // from nothing: it comes in
    }
    tip_alpha_ = anim_.to(kTipAlpha, tip_shown_ != 0 ? 1.0f : 0.0f, tip_shown_ != 0 ? motion::kQuick : motion::kMicro);
    tip_drawn_was_ = false;
    if (tip_shown_ != 0 || (visible && tip_alpha_ > 0.001f)) { // shown (from its first, clear frame), or going
        draw_tooltip(!visible);
        tip_drawn_was_ = true;
    }
    cursor_ = cursor_now_;
    if (input_.mouse_released) {
        active_ = 0;
    }
    anim_.end_frame();
    shapes_.end_frame();
    overlays_last_ = overlays_now_; // the next frame's presses under them are theirs
    overlays_last_count_ = overlays_now_count_;
    overlays_now_count_ = 0;
    in_overlay_ = false;
    wants_frame_ = requested_ || anim_.active();
}

void Ui::push_hit_clip(const layout::Box& box) noexcept {
    layout::Box b = box;
    if (hit_depth_ > 0) {
        const auto& under = hit_clips_[std::min(hit_depth_, hit_clips_.size()) - 1];
        const float x0 = std::max(b.x, under.x);
        const float y0 = std::max(b.y, under.y);
        const float x1 = std::min(b.x + b.w, under.x + under.w);
        const float y1 = std::min(b.y + b.h, under.y + under.h);
        b = {x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
    }
    if (hit_depth_ < hit_clips_.size()) {
        hit_clips_[hit_depth_] = b;
    }
    ++hit_depth_;
}

void Ui::pop_hit_clip() noexcept {
    if (hit_depth_ > 0) {
        --hit_depth_;
    }
}

auto Ui::interact(Id id, const layout::Box& box, bool focusable) -> Interaction {
    if (inert_ || disabled_) {
        return {}; // disabled: no hover, no press, the arrow
    }
    const auto within = [&](const layout::Box& b) {
        return input_.mouse_x >= b.x && input_.mouse_x < b.x + b.w && input_.mouse_y >= b.y && input_.mouse_y < b.y + b.h;
    };
    const bool inside = within(box) && (hit_depth_ == 0 || within(hit_clips_[std::min(hit_depth_, hit_clips_.size()) - 1])) &&
                        (in_overlay_ || !under_overlay(input_.mouse_x, input_.mouse_y)); // a menu over it takes the mouse
    if (focusable) {
        focus_order_.push_back(id);
    }
    if (input_.mouse_pressed && inside && active_ == 0) {
        active_ = id;
        if (focusable) {
            focus_ = id;
        }
    }
    Interaction r;
    r.hovered = inside && (active_ == 0 || active_ == id);
    r.held = active_ == id && input_.mouse_down;
    r.clicked = active_ == id && input_.mouse_released && inside;
    r.focused = focusable && focus_ == id;
    r.focus_ring = r.focused && focus_visible_;
    if (r.hovered && cursor_now_ == Cursor::Arrow) {
        cursor_now_ = Cursor::Hand; // what can be clicked: the hand (a field asks for its I-beam itself)
    }
    if (r.focused && input_.activate) {
        r.clicked = true;
    }
    return r;
}

// Coming on quick, going on micro: leaving is a token faster than arriving (MS § 2.4). A fade seen
// the first time comes from nothing.
auto Ui::fade(Id id, bool on) -> float {
    return anim_.to(id, on ? 1.0f : 0.0f, on ? motion::kQuick : motion::kMicro, 0.0f);
}

// MS scene 14: the highlight on quick both ways.
auto Ui::hover(Id id, bool on) -> float {
    return anim_.to(id ^ 0x486F766572ull, on ? 1.0f : 0.0f, motion::kQuick, 0.0f);
}

// MS scene 14: down on micro, back on base.
auto Ui::press(Id id, bool held) -> float {
    return anim_.geometry(id ^ 0x5072657373ull, held ? 1.0f : 0.0f, held ? motion::kMicro : motion::kBase, 0.0f); // a scale
}

void Ui::tooltip(Id id, const layout::Box& anchor, std::string_view title, std::string_view sub, std::string_view key) {
    if (inert_ || disabled_) {
        return;
    }
    tip_asked_ = id;
    tip_anchor_ = anchor;
    tip_title_.assign(title);
    tip_sub_.assign(sub);
    tip_key_.assign(key);
}

void Ui::draw_tooltip(bool fresh) {
    // A tooltip is a pop (the theme's layer above the surface), smaller: its title 12.5 px, a small line
    // under it 11.5 px (text_3: text_4 is under AA on a dark pop), a key badge after the title;
    // 8 px under the widget, centred on it, kept inside the window (above it if no room below).
    const render::Color kBack = th().pop;
    const render::Color kRing = th().line_2;
    const render::Color kTitle = th().text;
    const render::Color kSmall = th().text_3;
    const render::Color kKey = th().text_2;
    const float kw = tip_key_.empty() ? 0.0f : shape(Face::Text, tip_key_, 11.0f).width / scale_ + 10 + 6;
    const float sw = tip_sub_.empty() ? 0.0f : shape(Face::Text, tip_sub_, 11.5f).width / scale_;
    const float lh = line_height(Face::Text, 12.5f);
    const float sh = tip_sub_.empty() ? 0.0f : line_height(Face::Text, 11.5f) + 2;
    // A long title (a caption's whole text, a path) on as many lines as it takes.
    const auto& title = paragraph(Face::Text, tip_title_, 12.5f, tip_title_width());
    float tw = 0;
    for (const auto& line : title.lines()) {
        tw = std::max(tw, line.width / scale_);
    }
    const float w = std::max(tw + kw, sw) + 22;
    const float h = tip_height();
    const float vw = renderer_->logical_width();
    const float vh = renderer_->logical_height();
    float x = tip_anchor_.x + tip_anchor_.w / 2 - w / 2;
    x = std::clamp(x, 4.0f, std::max(4.0f, vw - w - 4));
    float y = tip_anchor_.y + tip_anchor_.h + 8;
    bool above = false;
    if (y + h > vh - 4 && tip_anchor_.y - 8 - h >= 4) {
        y = tip_anchor_.y - 8 - h;
        above = true;
    }
    if (fresh) { // from nothing: at its place, kRise px further from its widget, and it comes
        anim_.place(kTipX, x);
        anim_.place(kTipY, y + (above ? -motion::tooltip::kRise : motion::tooltip::kRise));
    }
    x = anim_.geometry(kTipX, x, motion::kBase); // to a neighbour: its place moves there
    y = anim_.geometry(kTipY, y, motion::kBase);
    const float was = renderer_->opacity();
    renderer_->set_opacity(was * tip_alpha_);
    tip_drawn_ = layout::Box{x, y, w, h};
    renderer_->rect({x, y, w, h}, {.fill = kBack, .radius = {9, 9, 9, 9}, .border = kRing, .border_width = 1});
    for (std::size_t i = 0; i < title.lines().size(); ++i) {
        const float top = y + 8 + static_cast<float>(i) * lh;
        text::draw(*renderer_, *atlas_, title.fonts(), title.shaped(), title.lines()[i], title.px(), {x + 11, centred_baseline(Face::Text, 12.5f, top, lh)},
                   kTitle, scale_);
    }
    if (!tip_key_.empty()) {
        const float kx = x + 11 + tw + 6;
        const float kh = line_height(Face::Text, 11.0f) + 2;
        renderer_->rect({kx, y + 8 + (lh - kh) / 2, kw - 6, kh}, {.fill = {}, .radius = {4, 4, 4, 4}, .border = {255, 255, 255, 51}, .border_width = 1});
        draw_text(Face::Text, shape(Face::Text, tip_key_, 11.0f), 11.0f, {kx + 5, centred_baseline(Face::Text, 11.0f, y + 8 + (lh - kh) / 2, kh)}, kKey);
    }
    if (!tip_sub_.empty()) {
        const float under = y + 8 + lh * static_cast<float>(std::max<std::size_t>(title.lines().size(), 1));
        draw_text(Face::Text, shape(Face::Text, tip_sub_, 11.5f), 11.5f, {x + 11, centred_baseline(Face::Text, 11.5f, under + 2, sh - 2)}, kSmall);
    }
    renderer_->set_opacity(was);
}

auto Ui::tip_title_width() const noexcept -> float {
    // At most 420 wide, and never wider than the window allows.
    return std::clamp(renderer_->logical_width() - 8 - 22, 40.0f, 420.0f);
}

auto Ui::tip_height() -> float {
    const float lh = line_height(Face::Text, 12.5f);
    const float sh = tip_sub_.empty() ? 0.0f : line_height(Face::Text, 11.5f) + 2;
    const auto lines = paragraph(Face::Text, tip_title_, 12.5f, tip_title_width()).lines().size();
    return lh * static_cast<float>(std::max<std::size_t>(lines, 1)) + sh + 16;
}

auto Ui::tip_bottom() -> float {
    if (tip_last_ == 0) {
        return 0.0f;
    }
    const float h = tip_height();
    if (tip_anchor_.y - 8 - h >= 4) {
        return 0.0f; // room above its widget: draw_tooltip turns it up there when below is short
    }
    return tip_anchor_.y + tip_anchor_.h + 8 + h;
}

auto Ui::shape(Face face, std::string_view text, float size, bool tabular) -> const text::Shaped& {
    return shapes_.get(fonts_->get(face), text, px(face, size), tabular);
}

auto Ui::line_height(Face face, float size) -> float {
    return fonts_->get(face).sized(0, px(face, size)).line_height / scale_;
}

auto Ui::ascent(Face face, float size) -> float {
    return fonts_->get(face).sized(0, px(face, size)).ascent / scale_;
}

auto Ui::descent(Face face, float size) -> float {
    return fonts_->get(face).sized(0, px(face, size)).descent / scale_;
}

void Ui::draw_text(Face face, const text::Shaped& shaped, float size, text::Point baseline,
                   render::Color color) {
    text::draw(*renderer_, *atlas_, fonts_->get(face), shaped, px(face, size), baseline, color,
               scale_);
}

void Ui::draw_text_zoomed(Face face, const text::Shaped& shaped, float size, text::Point baseline, render::Color color, float zoom) {
    const text::Line whole{.first_glyph = 0,
                           .end_glyph = static_cast<std::uint32_t>(shaped.glyphs.size()),
                           .first_byte = 0,
                           .end_byte = 0,
                           .width = shaped.width};
    text::draw_zoomed(*renderer_, *atlas_, fonts_->get(face), shaped, whole, px(face, size), baseline, color, scale_, zoom);
}

auto Ui::paragraph(Face face, std::string_view text, float size, float width) -> const text::Paragraph& {
    const float at = px(face, size);
    std::uint64_t key = core::hash::fnv1a64(text);
    key = key * 1099511628211ull ^ static_cast<std::uint64_t>(face);
    key = key * 1099511628211ull ^ std::bit_cast<std::uint32_t>(at);
    const float physical = width * scale_;
    if (wrapped_.empty()) {
        for (std::size_t i = 0; i < kParagraphs; ++i) {
            wrapped_.push_back(std::make_unique<Wrapped>());
        }
    }
    Wrapped* slot = nullptr;
    Wrapped* found = nullptr;
    for (auto& p : wrapped_) {
        auto& w = *p;
        if (w.paragraph && w.key == key && w.paragraph->px() == at && &w.paragraph->fonts() == &fonts_->get(face) &&
            w.paragraph->text() == text) {
            found = &w;
            break;
        }
        if (w.frame != frames_ && (slot == nullptr || w.asked < slot->asked)) {
            slot = &w; // the least recent of those this frame has not asked for: its texts are drawn
        }
    }
    if (found != nullptr) {
        slot = found;
    } else if (slot == nullptr) {
        wrapped_.push_back(std::make_unique<Wrapped>()); // this frame asks for more than there are: one more, kept
        slot = wrapped_.back().get();
    }
    slot->frame = frames_;
    if (slot->key != key || !slot->paragraph || slot->paragraph->text() != text) {
        slot->key = key;
        slot->paragraph.emplace(fonts_->get(face), at);
        slot->paragraph->set_width(physical);
        slot->paragraph->set_text(text);
        slot->width = physical;
    } else if (slot->width != physical) {
        slot->paragraph->set_width(physical);
        slot->width = physical;
    }
    slot->asked = ++asks_;
    return *slot->paragraph;
}

auto Ui::centred_baseline(Face face, float size, float top, float height) -> float {
    const float a = ascent(face, size);
    return top + (height - (a + descent(face, size))) / 2.0f + a;
}

void Ui::draw_icon(char32_t icon, float size, text::Point centre, render::Color color, float zoom) {
    char utf8[4];
    const auto length = core::utf8::encode(icon, utf8);
    const auto& shaped = shape(Face::Icons, std::string_view{utf8, length}, size);
    if (shaped.glyphs.empty()) {
        return;
    }
    const auto& g = shaped.glyphs[0];
    const float icon_px = px(Face::Icons, size);
    const auto* e = atlas_->get(fonts_->icons, g.font, icon_px, g.id);
    if (e == nullptr || e->shelf < 0) {
        return;
    }
    renderer_->set_glyph_atlas(atlas_->texture());
    if (zoom != 1.0f) {
        const float w = static_cast<float>(e->w) * zoom;
        const float h = static_cast<float>(e->h) * zoom;
        renderer_->glyph({centre.x - w / 2 / scale_, centre.y - h / 2 / scale_, w / scale_, h / scale_}, e->uv, color);
        return;
    }
    const float x = std::round(centre.x * scale_ - e->w / 2.0f);
    const float y = std::round(centre.y * scale_ - e->h / 2.0f);
    renderer_->glyph({x / scale_, y / scale_, e->w / scale_, e->h / scale_}, e->uv, color);
}

} // namespace ui
