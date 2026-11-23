#include "app/sheet_view.hpp"

#include "app/sheet_handle.hpp"

#include "core/hash.hpp"
#include "ui/theme.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <numbers>
#include <span>
#include <string>

namespace app::sheet {
namespace {

namespace theme = ui::theme;
namespace widgets = ui::widgets;
namespace work = ui::work;
namespace wt = ui::theme::work;
using ui::Face;
using ui::layout::Box;

auto id_of(std::string_view name) -> ui::Id {
    return core::hash::fnv1a64(name, core::hash::fnv1a64("app/sheet"));
}

constexpr std::string_view kBarPlaceholder = "Ask anything\xE2\x80\xA6";
constexpr std::string_view kRefinePlaceholder = "Refine or add to the task\xE2\x80\xA6";
constexpr std::string_view kFollowUpPlaceholder = "Refine or ask a new question\xE2\x80\xA6";
constexpr std::string_view kPermissionPlaceholder = "Or say what to change\xE2\x80\xA6";
constexpr std::string_view kAdvancedPlaceholder = "Type ALLOW, or ask something else\xE2\x80\xA6";
constexpr std::string_view kThinking = "Thinking\xE2\x80\xA6";
constexpr std::string_view kUnsafe = "Unsafe mode"; // an advanced flag of config.json is on

// .ph: rgba(242,243,247,.42)

// A view's layer changing (ui::motion::layers): the one that goes, on quick; the one that comes,
// on base from kInAt — `t` seconds after the change.
const double kLayerInAt = std::chrono::duration<double>(ui::motion::layers::kInAt).count();
// When both have rested: the change is over.
const double kLayerChange = kLayerInAt + ui::motion::settle_seconds(ui::motion::kBase);

auto fade_out(double t) -> float {
    return 1.0f - ui::motion::response(ui::motion::kQuick, t);
}

auto fade_in(double t) -> float {
    return ui::motion::response(ui::motion::kBase, t - kLayerInAt);
}

// .borb: the same orb in every open view.
auto orb_target(View v) -> float {
    return v == View::Rest ? 0.0f : wt::bar::orb;
}

auto rect(const Box& b) -> ui::render::Rect {
    return {b.x, b.y, b.w, b.h};
}

auto right(const Box& b) -> float {
    return b.x + b.w;
}

// Where the row's content starts after the orb: padding 18, the orb 68 with -8 px on each side,
// the row's gap.
constexpr float kFieldLeft = wt::bar::pad_left + wt::bar::orb + 2 * wt::bar::orb_margin + wt::bar::gap;
// The orb's centre in the row.
constexpr float kOrbX = wt::bar::pad_left + wt::bar::orb_margin + wt::bar::orb / 2;
constexpr float kOrbY = wt::bar::height / 2; // the first line's middle, however many the field has
constexpr ui::anim::Id kRowTrack = 21;         // the field's row height (phase 10)
constexpr ui::anim::Id kGoneTrack = 25;        // the field's text going, emptied (MS 04)
constexpr ui::anim::Id kHintBackTrack = 26;    // the hint coming back after it
constexpr ui::anim::Id kSelTrack = 27;         // the field's selection (micro both ways)
constexpr ui::anim::Id kDropZoneTrack = 28;    // the drop zone, files over the window (quick)
constexpr ui::anim::Id kRailTrack = 29;        // the progress rail's share (smooth; MS 05)
constexpr ui::anim::Id kRailAlphaTrack = 38;   // the rail going when the work is done (smooth; MS 06)
constexpr ui::anim::Id kStopShareTrack = 39;   // "stop" turning back into the arrow (base; MS 06)
constexpr ui::anim::Id kStatusTurnTrack = 40;  // the status chip's new words over its old (base; MS 06, 07)
constexpr ui::anim::Id kStatusWidthTrack = 41; // …and its room
constexpr double kSendGrowAfter = std::chrono::duration<double>(ui::motion::send::kGrowAt).count();
constexpr double kSendWithin = 0.1; // a task begun this soon after the field was emptied is a send
constexpr double kCharInSeconds = std::chrono::duration<double>(ui::motion::stream::kCharIn).count(); // a streamed character's fade
constexpr double kHintBackAfter = std::chrono::duration<double>(ui::motion::field::kHintBackAt).count();
// A change of theme, 0 .. 1 (UI update, stage 1, task 4): each frame gives the theme module how far.
const ui::anim::Id kThemeTrack = core::hash::fnv1a64("app/sheet:theme-change");
constexpr char32_t kIconFold = 0xE73F;          // the editor's ⤡ (BackToWindow)
constexpr char32_t kIconInfo = 0xE946;          // the long paste's notice
// A pasted text's card: a card on a card (card_2) laid on the surface — it lies over what is under it.
auto paste_card() -> ui::render::Color {
    return ::theme::over(ui::th().card_2, ui::th().surface);
}

} // namespace

auto shape_of(View v, Room room) -> Shape {
    if (v == View::Rest) {
        return {ui::motion::sign::kZoneWidth, ui::motion::sign::kZoneHeight, 0.0f}; // the sign's zone: what takes the mouse
    }
    Shape s{wt::bar::width, wt::bar::height, ui::th().radius.window};
    if (v == View::Input || v == View::Quick) {
        s.width = kOneColumn; // as wide as the conversation it opens into (phase 8)
    }
    if (v == View::Permission) {
        s.height += kPermissionBody;
    } else if (v == View::Answering) {
        s.height += kAnswerBody;
    } else if (v == View::Quick) {
        s.height += kQuickBody;
    } else if (v == View::Steps) {
        s.height += kStepsBody;
    } else if (v == View::Log) {
        s.height += kLogBody;
    } else if (v == View::Report) {
        s.height += kReportBody;
    } else if (v == View::Terminal) {
        s.width = kTerminalWidth;
        s.height += kTerminalBody;
    }
    // Review Focus 1: never past the monitor; the body scrolls instead.
    s.width = std::max(1.0f, std::min(s.width, room.w - 2 * kSideRoom));
    s.height = std::max(1.0f, std::min(s.height, room.h - kBottomRoom));
    return s;
}

auto Painter::update(const Model& m, double now) -> Shape {
    now_ = now;
    mnow_ = clock_.at(now);
    step_sign(m);
    if (m.attachments.empty() && chip_seen_next_ != 0) { // sent or cleared: attached again, they come in again
        chip_seen_.fill({});
        chip_seen_next_ = 0;
    }
    const View v = view(m);
    const bool conversation = conversation_shown(m, v);
    if (first_) {
        first_ = false;
        shown_ = previous_ = v;
    } else if (v != shown_ && conversation && conv_shown_) {
        shown_ = previous_ = v; // the same conversation, another state of its turn: no crossfade
    } else if (v != shown_) {
        previous_ = shown_;
        shown_ = v;
        changed_at_ = mnow_;
        done_since_ = now;
        done_pressed_ = false;
    }

    anim_.begin_frame(now);
    // A change of theme moves before anything is painted: the whole frame has one theme.
    if (::theme::changing()) {
        ::theme::progress(anim_.to(kThemeTrack, 1.0f, ui::motion::kQuick));
    }
    wants_theme_frame_ = false;
    Shape target = conversation ? conversation_shape(m) : shape_of(v, room_);
    const bool was_conversation = conv_shown_;
    conv_shown_ = conversation;
    if (strip_wanted(m, v)) { // the background's strip under the content, still never past the monitor
        target.height = std::min(target.height + strip_height(m), std::max(1.0f, room_.h - kBottomRoom));
    }
    if (const float row = attach_row(m, v); row > 0) { // the attachments over the line (phase 10)
        target.height = std::min(target.height + row, std::max(1.0f, room_.h - kBottomRoom));
    }
    target_ = target;
    // The window's motion (phase 8, app::motion): a glass view opens from the strip with the seam
    // and the panel's spring, grows from its bottom, and closes back up; the window keeps its open
    // size until the close has ended, and only then is the strip drawn (no strip at the panel's
    // size: the user's "big colourful square").
    const bool glass_view = v != View::Rest;
    if (glass_view && !motion_open_) {
        if (!reduced_motion_ && !arrive_next_ && changed_at_ == mnow_) {
            // Every open comes out of the edge's sign (UI update, stage 3): from the handle, or its
            // capsule where that is. Mid-fold, from where the shape is, to the window.
            if (!folding_) {
                fold_.handle = handle_shape();
                fold_.handle_orb = kHandleOrb;
                fold_.scene.snap_handle();
            }
            fold_.unfold = true;
            fold_.scene.unfold();
            fold_.last = now;
            motion_.open_at_rest(mnow_, target.width, wt::bar::height);
        } else if (arrive_next_) { // after an install: the installer's window became this bar and is held under it
            motion_.arrive(mnow_, wt::bar::height);
        } else {
            motion_.open(mnow_, target.width, wt::bar::height);
        }
        arrive_next_ = false;
        motion_open_ = true;
        motion_height_ = wt::bar::height;
    } else if (!glass_view && motion_open_) {
        if (!reduced_motion_) {
            // Every close goes into the edge's sign: from the window as it is now; mid-unfold, from
            // where the shape is, to the handle.
            if (!folding_) {
                fold_.window = shape_;
                fold_.window_orb = orb_size_;
                fold_.scene.snap_window();
            }
            fold_.unfold = false;
            fold_.scene.fold();
            fold_.last = now;
            motion_.cut();
        } else {
            motion_.close(mnow_);
        }
        motion_open_ = false;
    }
    if (glass_view) {
        last_open_view_ = v;
    }
    sync_field(m);                          // emptied just now? (a send: update_send asks)
    update_send(m, now, was_conversation); // Enter's scene (MS 05)
    step_disclosures(now);                 // what opens under its row (MS 10)
    update_approval(m, now);               // a question asked and answered (MS 11)
    // MS 05: after Enter the window begins to grow at +60 ms.
    const bool grow_waits = !send_.done() && send_.elapsed() < kSendGrowAfter && !reduced_motion_; // (moving() keeps the frames coming)
    if (motion_open_ && std::abs(target.height - motion_height_) > 0.5f && !grow_waits) {
        // A conversation's parts come in by Enter's scene (send_part).
        motion_.expand(mnow_, target.height);
        motion_height_ = target.height;
    }
    redraw_due_ = false;
    const bool was_moving = moving_window_;
    frame_ = motion_.at(mnow_);
    // The field's row (phase 10, screens 27-31): 30 px more a line of the field, at most kFieldLines;
    // its own 120 ms both ways (180 when the field was emptied: a send), on top of the window's motion
    // — the glass, the shape and the fold all take it from the frame's height.
    // Its lines are counted where the field is drawn, at its real width (draw_row): a view whose
    // row is narrower or wider would otherwise bump the row for a frame or two.
    const bool with_field = field_view(m, v);
    sync_field(m); // its text as it is now, at the last width (the keys' caret moves use it too)
    if (with_field) { // at once; a view whose row is another width counts again when it is drawn, in this frame
        count_row_lines(m);
    }
    row_h_ = anim_.geometry(kRowTrack, with_field ? row_to_ : wt::bar::height, row_token_);
    // While the row moves the window holds the larger of where it was and where it goes (the user:
    // a window resized every frame jerked); the panel moves inside it.
    row_hold_ = row_h_ == row_to_ ? row_to_ : std::max({row_hold_, row_h_, row_to_});
    const float base = frame_.height;
    motion_clip_ = base; // the curtain before the row's growth: draw_moving lays out on top of it
    // Closing (Esc on a field of several lines): the row's lines go on their own spring as the
    // curtain closes — dropped at once they took the window's height with them in one frame.
    const bool closing = v == View::Rest && !frame_.window_hidden;
    if (closing) {
        row_hold_ = row_h_; // nothing is held for a row that is leaving
    }
    const auto grown = [&](float row) { // it grows down, never past the monitor
        if ((!with_field && !closing) || row <= wt::bar::height) {
            return base;
        }
        // With the conversation open the window grows with it to 85 % of the screen at most; past
        // that the answer's area gives way, never the row (screen 30).
        const float most = conversation ? std::max(base, kConversationShare * room_.h) : 1.0e9f;
        return std::min({base + row - wt::bar::height, most, std::max(1.0f, room_.h - kBottomRoom)});
    };
    frame_.height = grown(row_h_);
    held_h_ = grown(row_hold_);
    if (paste_bottom_ > frame_.height && m.open) { // a pasted text's card under the mouse: room for it
        frame_.height = std::min(paste_bottom_, std::max(1.0f, room_.h - kBottomRoom));
    }
    if (now > fold_.last && fold_.last > -1.0e8) {
        fold_.scene.step(static_cast<float>(now - fold_.last)); // a frame's limit at most
    }
    fold_.last = std::max(fold_.last, now);
    const bool was_folding = folding_;
    folding_ = !fold_.scene.done();
    if (folding_ && fold_.unfold &&
        fold_.scene.near_window(std::max(std::abs(fold_.window.width - fold_.handle.width), std::abs(fold_.window.height - fold_.handle.height)))) {
        fold_.scene.snap_window(); // within a pixel and slow: the window's own drawing takes over, and its clicks
        folding_ = false;
    }
    moving_window_ = motion_open_ || !frame_.window_hidden || folding_;
    if (was_moving && !moving_window_) {
        // After a fold into the handle the handle is there already, whole: nothing fades in.
        hidden_at_ = was_folding && v == View::Rest ? -1.0e9 : mnow_;
    }
    if (moving_window_) {
        if (motion_open_) { // view to view while open: the width follows; the height is the motion's
            open_width_ = anim_.geometry(1, target.width, ui::motion::kSmooth); // a size: smooth, never past its end
            open_radius_ = anim_.geometry(3, target.radius, ui::motion::kSmooth);
        }
        shape_ = {open_width_, frame_.height, open_radius_};
        orb_size_ = wt::bar::orb * frame_.orb_scale;
    } else { // the sign's zone: its own springs (ids of their own)
        // MS 12's second stage and § 7 "Tongue": the width and corners on base, a height (the
        // preview's) on smooth — critically damped: an overshoot took the strip's height through
        // nothing (two empty frames, the screen recording of 2026-11-09).
        if (was_moving) {
            // The window's motion has just let go of the shape: its height goes on from where the
            // curtain left it, its width and corners from the shape's own (a handle with its card
            // dropped while the close still ran stood whole in a frame: agent.exe, 2026-11-09).
            // A fold ends on the handle itself.
            anim_.place(11, target.width);
            anim_.place(12, was_folding ? target.height : shape_.height);
            anim_.place(13, target.radius);
        }
        shape_.width = anim_.geometry(11, target.width, ui::motion::kBase);
        shape_.height = anim_.geometry(12, target.height, ui::motion::kSmooth);
        shape_.radius = anim_.geometry(13, target.radius, ui::motion::kBase);
        orb_size_ = anim_.geometry(14, orb_target(v), ui::motion::kBase);
        if (v == View::Rest) { // the sign's capsule is wider and taller than its zone: the shape holds it, at once
            const auto sf = sign_.frame();
            shape_.width = std::max(shape_.width, sf.width + 2 * ui::motion::sign::kHalo);
            shape_.height = std::max(shape_.height, sf.capsule > 0.001f ? ui::motion::sign::kTop + sf.height + 2.0f : 0.0f);
        }
    }
    if (folding_) { // one shape between the two ends: the window (as its motion has it now) and the handle
        if (fold_.unfold) {
            fold_.window = shape_;
            fold_.window_orb = orb_size_;
        } else {
            fold_.handle = handle_shape();
            fold_.handle_orb = kHandleOrb;
        }
        const float s = fold_.scene.frame().shape; // 0 the window, 1 the handle (morph: ~1 % past)
        const auto mix = [s](float a, float b) { return a + (b - a) * s; };
        shape_ = {mix(fold_.window.width, fold_.handle.width), mix(fold_.window.height, fold_.handle.height),
                  mix(fold_.window.radius, fold_.handle.radius)};
        orb_size_ = mix(fold_.window_orb, fold_.handle_orb);
    }
    anim_.end_frame();

    ui::Orb::Mood mood = ui::Orb::Mood::Idle;
    if (!m.open && presence_of(m).state == PresenceState::Working) {
        mood = ui::Orb::Mood::Thinking; // the capsule's orb thinks while a task works behind the handle
    } else if (m.activity == Activity::Thinking) {
        mood = ui::Orb::Mood::Thinking;
    } else if (m.activity == Activity::Answering && m.streaming) {
        mood = ui::Orb::Mood::Answering;
    }
    orb_->set_mood(mood);
    // design/ui_model: the material and energy of the model choice shown — the picker's hover, else
    // what the next request goes with (Flash while a picture is in the input with Pro chosen).
    const ModelChoice shown = m.preview.value_or(sent_with(m));
    const auto target_look = ui::orb_look::look_of(model_index(shown.model), level_for(model_of(shown), shown.level));
    if (!look_snapped_) {
        look_motion_.snap(target_look); // the first frame: the choice as it is, not a change to it
        look_snapped_ = true;
    }
    look_motion_.set(target_look, now, reduced_motion_);
    look_frame_ = look_motion_.at(now);
    orb_->set_look(look_frame_);
    orb_->set_energy(ui::orb_look::energy_at(look_frame_.look.level));
    hint_up_ = hint_shown(m);
    if (m.answer.size() > answer_seen_) {
        orb_->pulse(); // the mockup pulses the orb on every streamed token
    }
    answer_seen_ = m.answer.size();
    orb_->update(now);
    sync_answer(m); // every frame: a token's fade starts when it arrives, not when first drawn
    // The window: the row's held height over the panel's - only while the open panel is the window's
    // own motion (a fold or the strip size their windows as before).
    window_h_ = moving_window_ && !folding_ ? std::max(shape_.height, shape_.height + held_h_ - frame_.height) : shape_.height;
    if (hint_up_ && moving_window_ && !folding_) { // the hint under the bar (design/ui_model, screen 4)
        window_h_ = std::max(window_h_, shape_.height + kHintRoom);
    }
    // The model menu under the bar (stage 4b): the window holds it, and its shadow, while it is
    // open or fading; its height changes only as the menu opens and closes, not a frame.
    if ((m.menu.open || menu_alpha_ > 0.0f) && m.open) {
        window_h_ = std::max(window_h_, row_h_ + 8 + menu_height(m) + 28);
    }
    // A tooltip under the bar (cut off by the window's edge): its room from
    // when it is asked, through its 500 ms wait, while it shows.
    // Only one with no room above its widget (the bar's, at the screen's top): another turns up
    // inside the glass (ui::Ui::tip_bottom says 0).
    if (moving_window_ && !folding_) {
        if (const float tip = ui_->tip_bottom(); tip > 0) {
            window_h_ = std::max(window_h_, tip - glass_.y + 4);
        }
    }
    return shape_;
}

void Painter::render_orb(double now) {
    if (orb_size_ <= 1.0f && sign_.frame().capsule > 0.001f) {
        orb_->render(now); // the capsule's orb: the window's own, drawn while the capsule is there
    }
    // Stills are the orb in a theme's light: after a change of theme has ended they are made again
    // (during it they stay — a still made half-way would be neither theme's).
    if (!::theme::changing() && stills_theme_ != ::theme::current().name) {
        avatar_stills_ = {};
        stills_theme_ = ::theme::current().name;
    }
    if (orb_size_ > 1.0f) {
        orb_->render(now);
        // The answers' mark, once each (alike since the orb has no material of a model's own; an answer shows the
        // model that made it); without the small orb, the bar's first frame.
        for (std::size_t i = 0; i < avatar_stills_.size(); ++i) {
            if (avatar_stills_[i]) {
                continue;
            }
            if (card_orb_ != nullptr) {
                card_orb_->set_params({.spike = 0.35f, .speed = 0.5f, .hue = 0.0f, .energy = 0.0f, .phase = 2.4f});
                card_orb_->set_zoom(1.0f);
                card_orb_->set_look({.look = ui::orb_look::look_of(static_cast<int>(i), 1)});
                card_orb_->render(2.4);
                if (auto still = card_orb_->still()) {
                    avatar_stills_[i].emplace(std::move(*still));
                }
            } else if (auto still = orb_->still()) {
                avatar_stills_[i].emplace(std::move(*still));
            }
        }
    }
}

void Painter::avatar_image(const Box& box, int model) {
    if (const auto& still = avatar_stills_[static_cast<std::size_t>(std::clamp(model, 0, static_cast<int>(avatar_stills_.size()) - 1))]) {
        ui_->renderer().image({box.x, box.y, box.w, box.h}, ui::Orb::uv(), still->texture(), {255, 255, 255, 255});
    }
}

void Painter::orb_step(double now) {
    orb_->update(now);
    render_orb(now);
}

auto Painter::animating() const noexcept -> bool {
    const bool transition = mnow_ - changed_at_ < kLayerChange;
    return anim_.active() || transition || ::theme::changing() || wants_theme_frame_ || (shown_ == View::Rest && !sign_.done());
}

auto Painter::frame_need(const Model& m, double now) const noexcept -> FrameNeed {
    (void)now;
    if (animating() || moving() || ui_->wants_frame() || m.streaming) {
        return FrameNeed::Full; // a widget's own motion (a spinner, a sweep) asks through the Ui
    }
    if (shown_ == View::Rest) { // the handle stands still, whatever goes on behind it: the agent's events draw it
        return FrameNeed::None;
    }
    // The open sheet's orb moves whatever the rest setting (that is the resting strip's).
    return reduced_motion_ ? FrameNeed::Caret : FrameNeed::OrbIdle;
}

auto Painter::window_hidden() const noexcept -> bool {
    return !moving_window_;
}

void Painter::set_reduced_motion(bool on) noexcept {
    motion_.reduced(on);
    reduced_motion_ = on;
    anim_.set_reduced(on);              // the window's sizes and places
    ui_->animator().set_reduced(on);   // the widgets': presses, tooltips, tables, tabs
}

void Painter::change_theme(const ::theme::Theme& to) noexcept {
    if (reduced_motion_) {
        ::theme::set(to);
        wants_theme_frame_ = true;
        return;
    }
    const bool going = ::theme::changing();
    const std::string_view going_to = going ? ::theme::target().name : std::string_view{};
    ::theme::begin_change(to); // the theme it is, or already goes to, is no new change
    if (::theme::changing() && (!going || ::theme::target().name != going_to)) {
        anim_.place(kThemeTrack, 0.0f); // a new change starts from the colours on the screen
        wants_theme_frame_ = true;
    }
}

auto Painter::next_caret_flip(double now) const noexcept -> double {
    const double half = std::chrono::duration<double>(ui::motion::caret::kBlink).count() / 2.0; // steps(2): on half the cycle, off the other
    const double since = std::max(0.0, now - edited_at_);
    return edited_at_ + (std::floor(since / half) + 1.0) * half;
}

// What the sign says now, to its scene; the scene a frame on (the motion clock: a stall moves it a
// frame's limit). With Windows' animation effects off, and on its first frame, it is there at once.
auto Painter::handle_shape() const -> Shape {
    const auto sf = sign_.frame();
    return {sf.width, ui::motion::sign::kTop + sf.height, sf.radius};
}

void Painter::draw_handle(const Model& m, const Box& zone, float opacity) {
    const auto sf = sign_.frame();
    const Presence says = presence_of(m);
    const auto& st = *sign_theme_;
    sign_capsule_w_ = sign_capsule_width(*ui_, says.word, says.line, shortcut_);
    draw_sign(*ui_, zone,
              {.theme = sign_theme_,
               .frame = sf,
               .dot = sign_dot_,
               .opacity = opacity,
               .word = says.word,
               .line = says.line,
               .shortcut = shortcut_,
               .word_color = says.state == PresenceState::Done       ? st.ok
                             : says.state == PresenceState::Decision ? st.warn
                             : says.state == PresenceState::Failed   ? st.crit
                             : says.state == PresenceState::Working  ? st.accent
                                                                     : st.text_3,
               .orb = sf.capsule > 0.001f ? orb_->texture() : 0u});
    handle_drawn_w_ = sf.capsule > 0.001f ? 0.0f : sf.width;
}

void Painter::step_sign(const Model& m) {
    const Presence says = presence_of(m);
    const scenes::SignTarget to{.working = says.state == PresenceState::Working,
                                .progress = says.progress,
                                .dot = says.dot,
                                .warn = says.state == PresenceState::Decision,
                                .hovered = sign_hovered_ && !m.open,
                                .capsule_width = sign_capsule_w_};
    if (says.dot) { // kept while the dot goes: it fades in the colour it had
        const auto& th = *sign_theme_;
        sign_dot_ = says.state == PresenceState::Decision ? th.warn : says.state == PresenceState::Failed ? th.crit : th.ok;
    }
    if (sign_at_ < -1.0e8) {
        sign_.snap(to);
    } else {
        sign_.set(to, mnow_);
        sign_.step(static_cast<float>(mnow_ - sign_at_), mnow_);
        if (reduced_motion_) {
            sign_.settle(); // no motion; what waits (the capsule's 0.6 s) still waits
        }
    }
    sign_at_ = mnow_;
}

auto Painter::rest_frame_due(double now) const noexcept -> bool {
    (void)now;
    return false; // the handle stands still: nothing to draw at the resting tick
}

auto Painter::rest_motion() const noexcept -> bool {
    return false;
}

auto Painter::moving() const noexcept -> bool {
    return anim_.active() || mnow_ - changed_at_ < kLayerChange || folding_ || // (the window and the edge's sign as one shape)
           std::abs(shown_content_ - content_) > 0.25f || now_ - last_token_at_ < kCharInSeconds || stream_.flowing() ||
           redraw_due_ || // (text held back as an unfinished word is not motion: stream_.flowing() says)
           !motion_.settled(mnow_) || // the window's own motion: a grow after a turn too
           !send_.done() || // Enter's scene (MS 05)
           !disclosures_.empty() || // something opening or closing under its row (MS 10)
           !approval_.done() ||     // a question coming or answered, the dim with it (MS 11)
           look_motion_.moving() || hint_up_ || // design/ui_model: the orb's change, the hint
           mnow_ - hidden_at_ < ui::motion::settle_seconds(ui::motion::kQuick); // the strip fading in after a close — even when the close's end
                                         // was first seen after a pause in frames (the tray menu's loop)
}

void Painter::add_target(std::string_view name, const Box& box) {
    // The names of earlier frames are written over: a long one costs no allocation per frame.
    if (target_count_ < targets_.size()) {
        targets_[target_count_].name.assign(name);
        targets_[target_count_].box = box;
    } else {
        targets_.push_back({std::string{name}, box});
    }
    ++target_count_;
}

auto Painter::action_name(std::string_view id) -> std::string_view {
    action_name_.assign("action:");
    action_name_.append(id);
    return action_name_;
}

void Painter::orb_image(const Box& box) {
    if (orb_count_ < orb_draws_.size()) {
        orb_draws_[orb_count_++] = {box, ui_->renderer().opacity()};
    }
    if (!hide_orbs_) {
        ui_->renderer().image({box.x, box.y, box.w, box.h}, ui::Orb::uv(), orb_->texture(), {255, 255, 255, 255});
    }
}

auto Painter::orb_boxes() const -> std::vector<Box> {
    std::vector<Box> out;
    for (std::size_t i = 0; i < orb_count_; ++i) {
        out.push_back(orb_draws_[i].box);
    }
    return out;
}

void Painter::begin_keep(int w, int h) {
    kept_ready_ = false;
    if (!kept_ || kept_->width() != w || kept_->height() != h) {
        auto made = ui::render::Target::create(w, h);
        if (!made) {
            kept_.reset();
            glBindFramebuffer(GL_FRAMEBUFFER, 0); // drawn as usual: nothing kept
            return;
        }
        kept_.emplace(std::move(*made));
    }
    kept_->bind();
    kept_->clear();
    hide_orbs_ = true;
}

void Painter::end_keep() noexcept {
    if (hide_orbs_ && kept_) {
        kept_orbs_ = orb_draws_; // where the frame just drawn put the orb
        kept_orb_count_ = orb_count_;
        kept_ready_ = true;
    }
    hide_orbs_ = false;
}

auto Painter::draw_kept(double now) -> bool {
    (void)now; // the orb's texture was drawn for it by render_orb(now)
    if (!kept_ready_ || !kept_) {
        return false;
    }
    auto& renderer = ui_->renderer();
    kept_->copy_to_bound(); // exactly as drawn: a blit, not a premultiplying draw (before any of the renderer's)
    for (std::size_t i = 0; i < kept_orb_count_; ++i) {
        renderer.set_opacity(kept_orbs_[i].opacity);
        const auto& b = kept_orbs_[i].box;
        renderer.image({b.x, b.y, b.w, b.h}, ui::Orb::uv(), orb_->texture(), {255, 255, 255, 255});
    }
    renderer.set_opacity(1.0f);
    return true;
}

auto Painter::orb_frame_due(double last_full, double now) const noexcept -> bool {
    return kept_ready_ && now < next_caret_flip(last_full);
}

auto Painter::draw(Model& m, const Box& panel) -> Reaction {
    if (m.export_xlsx && !panel_shown(m)) {
        m.export_xlsx = false; // Ctrl+Shift+E with no table shown: nothing, and nothing later
    }
    target_count_ = 0;
    solid_count_ = 0;
    orb_count_ = 0;
    links_drawn_ = 0;
    tables_drawn_ = 0;
    model_chip_ = {}; // drawn again by the bar that shows it
    table_pick_.reset();
    table_picked_ = nullptr;
    output_lines_drawn_ = 0;
    handle_drawn_w_ = 0;
    sign_hovered_ = false; // the rest's frame says again; an open window's never
    Reaction r;
    auto& renderer = ui_->renderer();
    const double t = mnow_ - changed_at_;
    const bool transition = t < kLayerChange;
    const bool changed = transition && previous_ != shown_;
    const float radius = std::min(shape_.radius, corner_cap_);

    if (folding_) { // the window and the handle as one shape: nothing takes a click meanwhile
        draw_fold(m, panel);
        renderer.set_opacity(1.0f);
        return r;
    }
    if (moving_window_) { // a glass view: open, opening or closing (app::motion)
        draw_moving(m, panel, radius, changed, t, r);
        renderer.set_opacity(1.0f);
        draw_context_menu(panel, r);
        return r;
    }

    // The sign: the handle at the top centre, at its own size (its zone, and its capsule while it
    // is there, take the mouse). A glass view's opening and closing are the fold above; after a
    // close at once (Windows' animations off) the handle comes in on quick.
    renderer.set_opacity(ui::motion::response(ui::motion::kQuick, mnow_ - hidden_at_));
    const Box zone = sign_zone(panel.x + panel.w / 2, panel.y);
    const auto sf = sign_.frame();
    draw_handle(m, zone, 1.0f);
    const Box body = sign_body(zone, sf);
    const float left = std::min(zone.x, body.x);
    const Box hit{left, zone.y, std::max(zone.x + zone.w, body.x + body.w) - left, std::max(zone.h, ui::motion::sign::kTop + body.h + 2.0f)};
    add_target("sign", hit);
    add_solid(hit);
    if (sf.capsule > 0.5f) {
        add_target("sign:capsule", body);
        add_target("sign:key", sign_key(zone, sf, *ui_, shortcut_));
    }
    const auto over = ui_->interact(id_of("sign"), hit, false);
    sign_hovered_ = over.hovered; // the next frame's: the capsule comes, or its 0.6 s begin
    if (over.clicked) {
        r.open = true; // as the shortcut: the sheet, on what the sign says
    }
    renderer.set_opacity(1.0f);
    draw_context_menu(panel, r);
    return r;
}

void Painter::draw_context_menu(const Box& panel, Reaction& r) {
    // The right button over the sheet (responsive text): Copy what is selected, Select all there.
    const auto& in = ui_->input();
    if (in.mouse_pressed && !field_pressed_) {
        keys_to_field_ = false; // a press elsewhere in the sheet: Ctrl+C and Ctrl+A are its text's
    }
    field_pressed_ = false;
    const bool over = in.mouse_x >= panel.x && in.mouse_x < panel.x + panel.w && in.mouse_y >= panel.y && in.mouse_y < panel.y + panel.h;
    if (in.right_pressed && over && shown_ != View::Rest) {
        menu_open_ = true;
        menu_at_ = {in.mouse_x, in.mouse_y};
    }
    if (!menu_open_) {
        return;
    }
    static constexpr std::array<std::string_view, 2> kItems{"Copy", "Select all"};
    const std::array<bool, 2> enabled{ui_->selection().active(), true};
    std::array<Box, 2> boxes{};
    const auto picked = work::context_menu(*ui_, id_of("menu"), menu_at_, kItems, enabled, menu_open_, boxes);
    add_target("menu:copy", boxes[0]);
    add_target("menu:select-all", boxes[1]);
    if (picked == 0 && ui_->copy_selection(menu_text_)) {
        r.clipboard = menu_text_;
    } else if (picked == 1) {
        ui_->select_all_at(menu_at_.x, menu_at_.y);
    }
}

auto Painter::take_escape() -> bool {
    if (menu_open_) {
        menu_open_ = false;
        return true;
    }
    if (ui_->selection().active()) {
        ui_->clear_selection();
        return true;
    }
    return false;
}

void Painter::draw_fold(Model& m, const Box& panel) {
    auto& renderer = ui_->renderer();
    const auto f = fold_.scene.frame();
    // The shape: flat on top (the window's edge), rounded under, its ring on three sides.
    const float rad = std::min(16.0f, panel.h / 2);
    // The window's shadow goes with the shape, and comes with it: never there or gone in a frame.
    renderer.set_opacity(std::clamp(1.0f - f.shape, 0.0f, 1.0f));
    work::sheet_shadow(*ui_, panel, rad);
    renderer.set_opacity(1.0f - f.handle_alpha); // into the handle: the shape gives way to the sign's own look
    renderer.rect({panel.x, panel.y - rad, panel.w, panel.h + rad},
                  {.fill = work::surface(*ui_, ui::th().surface), .radius = {0, 0, rad, rad}, .border = ui::th().line, .border_width = 1});
    const View bar = fold_.unfold ? shown_ : last_open_view_;
    const Shape bar_shape = fold_.unfold ? Shape{open_width_, frame_.height, 0} : fold_.window;
    const auto keep = target_count_;
    Reaction ignored;
    ui_->set_inert(true);
    renderer.push_clip(rect(panel));
    // The bar's content, as it was (or as it will be), from the shape's left: its row and what is
    // under it each at its own opacity (MS 13: the row from +260 ms, the columns from +320 coming
    // down 10 px).
    if (f.window_alpha > 0.001f || f.columns_alpha > 0.001f) {
        // Folding, the front is empty already: the task the close sent behind the handle is drawn in
        // its place while it fades, and given back at once.
        const auto behind = fold_.unfold || m.closed_task == 0 ? m.background.end()
                                                               : std::ranges::find(m.background, m.closed_task, &TaskState::task);
        if (behind != m.background.end()) {
            std::swap(static_cast<TaskState&>(m), *behind);
        }
        const Box laid{panel.x, panel.y, bar_shape.width, bar_shape.height};
        renderer.push_clip(rect({panel.x, panel.y, panel.w, row_h_}));
        renderer.set_opacity(f.window_alpha);
        draw_layer(bar, m, laid, ignored);
        renderer.pop_clip();
        if (f.columns_alpha > 0.001f && panel.h > row_h_) {
            renderer.push_clip(rect({panel.x, panel.y + row_h_, panel.w, panel.h - row_h_}));
            renderer.set_opacity(f.columns_alpha);
            draw_layer(bar, m, {laid.x, laid.y + f.columns_dy, laid.w, laid.h}, ignored);
            renderer.pop_clip();
        }
        if (behind != m.background.end()) {
            std::swap(static_cast<TaskState&>(m), *behind);
        }
    }
    renderer.pop_clip();
    ui_->set_inert(false);
    target_count_ = keep;
    solid_count_ = 0;
    // The orb flies from the bar's place towards the handle's with the shape, its size too, and is gone by half the way.
    const float at_bar_x = panel.x + kOrbX;
    const float at_bar_y = panel.y + kOrbY;
    if (f.handle_alpha > 0.001f) { // the sign itself, over the shape's last fifth (outside the shape's clip: its zone is wider)
        renderer.set_opacity(1.0f);
        draw_handle(m, sign_zone(panel.x + panel.w / 2, panel.y), f.handle_alpha);
    }
    const float at_handle_x = panel.x + panel.w / 2;
    const float at_handle_y = panel.y + ui::motion::sign::kTop + ui::motion::sign::kHeight / 2;
    const float k = f.shape;
    const float cx = at_bar_x + (at_handle_x - at_bar_x) * k;
    const float cy = at_bar_y + (at_handle_y - at_bar_y) * k;
    renderer.set_opacity(f.orb_alpha);
    if (orb_size_ > 1.0f && f.orb_alpha > 0.001f) {
        orb_image({cx - orb_size_ / 2, cy - orb_size_ / 2, orb_size_, orb_size_});
    }
}

auto Painter::panel_color() const -> ui::render::Color {
    return work::surface(*ui_, ui::th().panel);
}

void Painter::panel_ground(const Box& panel) {
    ui_->renderer().rect({panel.x, panel.y, panel.w, panel.h}, {.fill = panel_color(), .radius = bottom_corners(panel)}); // the window's corner stays clear
}

auto Painter::surface_at(float y) const -> ui::render::Color {
    // As work::sheet lays it: the top's colour at the glass's top, the surface at its bottom.
    const float k = glass_.h > 1.0f ? std::clamp((y - glass_.y) / glass_.h, 0.0f, 1.0f) : 1.0f;
    return work::surface(*ui_, blend(ui::th().surface_top, ui::th().surface, k));
}

void Painter::draw_moving(Model& m, const Box& panel, float radius, bool changed, double t, Reaction& r) {
    auto& renderer = ui_->renderer();
    // The curtain (motion rework, MS scenes 01-03): the glass is the panel's visible height; the
    // content is laid out once at its full height (MS § 5.4) and shifted inside it.
    const Box glass{panel.x, panel.y, panel.w, frame_.height};
    const float base = 1.0f;
    renderer.set_opacity(base);
    // The theme's shadow holds the window (in both themes). The window is a
    // canvas with kMargin round the panel (motion rework, task 5), so the shadow fades out inside
    // it; on 2026-10-15 the window was the panel itself and cut it into a sharp dark rectangle.
    work::sheet(*ui_, glass, radius, true);
    glass_ = glass;
    glass_radius_ = radius;

    // The content moves with the panel; while it closes it is the last open view, fading first
    // and taking no clicks.
    const bool closing = !motion_open_;
    const View v = closing ? last_open_view_ : shown_;
    const float content_alpha = base;
    // After Enter the row stays and its parts change by the scene (MS 05): no crossfade of layers.
    const bool sending = !send_.done();
    const bool layer_change = changed && !closing && !sending && previous_ != View::Rest;
    renderer.push_clip(rect(glass));
    // Closing, the content keeps the size it had while the curtain closes over it (MS § 5.4);
    // growing, it is laid out at the glass (at the height it grows to, a conversation's scroll to
    // its card was reckoned against a view it did not have yet).
    const float laid = closing ? std::max(glass.h, frame_.layout_h + (glass.h - motion_clip_)) : glass.h;
    const float dy = frame_.content_dy;
    const float strip = strip_wanted(m, v) ? std::min(strip_height(m), std::max(0.0f, laid - row_h_)) : 0.0f;
    // The attachments' row over the line (phase 10): the view is drawn under it, as it always is.
    const float above = std::min(attach_row(m, v), std::max(0.0f, laid - strip - row_h_));
    if (above > 0) {
        renderer.set_opacity(content_alpha * frame_.field_alpha);
        ui_->set_inert(closing);
        const auto keep = target_count_;
        draw_attach_row(m, {panel.x, glass.y + dy, panel.w, above});
        ui_->set_inert(false);
        if (closing) {
            target_count_ = keep;
        }
    }
    const Box content{panel.x, glass.y + dy + above, panel.w, laid - strip - above};
    if (layer_change && fade_out(t) > 0) {
        const auto keep = target_count_;
        Reaction ignored;
        ui_->set_inert(true);
        renderer.set_opacity(content_alpha * fade_out(t));
        draw_layer(previous_, m, content, ignored);
        ui_->set_inert(false);
        target_count_ = keep;
    }
    {
        const auto keep = target_count_;
        Reaction leaving;
        ui_->set_inert(closing);
        const float in = content_alpha * (layer_change ? fade_in(t) : 1.0f);
        if (frame_.field_alpha == frame_.content_alpha) {
            renderer.set_opacity(in * frame_.field_alpha);
            draw_layer(v, m, content, closing ? leaving : r);
        } else {
            // MS 03: the columns fade first, the bar's row 40 ms later — the view drawn twice, its
            // row and what is under it each clipped to its part, each at its own opacity.
            const Box row_part{glass.x, content.y, glass.w, row_h_};
            const Box under{glass.x, content.y + row_h_, glass.w, std::max(0.0f, glass.y + glass.h - content.y - row_h_)};
            renderer.push_clip(rect(row_part));
            renderer.set_opacity(in * frame_.field_alpha);
            draw_layer(v, m, content, closing ? leaving : r);
            renderer.pop_clip();
            Reaction twice;
            ui_->set_inert(true);
            const auto drawn = target_count_;
            renderer.push_clip(rect(under));
            renderer.set_opacity(in * frame_.content_alpha);
            draw_layer(v, m, content, twice);
            renderer.pop_clip();
            target_count_ = drawn;
            ui_->set_inert(closing);
        }
        ui_->set_inert(false);
        if (closing) {
            target_count_ = keep;
        }
    }
    if (strip > 0 && !closing && !r.command && !r.close) {
        renderer.set_opacity(content_alpha);
        draw_strip(m, {panel.x, glass.y + glass.h - strip, panel.w, strip}, r);
    }
    if (!closing) { // a pasted text's card over all of it (phase 10, screen 31)
        renderer.set_opacity(content_alpha);
        draw_paste_preview(m, glass);
    } else {
        paste_bottom_ = 0;
    }
    renderer.pop_clip();

    if (orb_size_ > 1.0f && frame_.orb_alpha > 0.0f) { // MS 01: in from +30 ms, .72 -> 1; MS 02: out, to .8
        renderer.push_clip(rect(glass));
        renderer.set_opacity(base * frame_.orb_alpha * frame_.field_alpha);
        const float s = orb_size_;
        const Box orb_box{panel.x + kOrbX - s / 2, glass.y + dy + above + kOrbY - s / 2, s, s};
        orb_image(orb_box);
        renderer.pop_clip();
        if (!closing) { // design/ui_model: a click opens the picker, the wheel steps thinking
            draw_orb_controls(m, orb_box, r);
        }
    }
    if (!closing) {
        renderer.set_opacity(base);
        draw_model_hint(m, glass);
        renderer.set_opacity(base);
        draw_menu(m, glass.y + dy + above, panel.x + panel.w, r); // over all of the frame (stage 4b)
    }
    // Files over the window (the shell's drop target): over all of it; in and out on quick (MS § 7).
    if (const float zone = anim_.to(kDropZoneTrack, m.drag_over && !closing ? 1.0f : 0.0f, ui::motion::kQuick); zone > 0.001f && !closing) {
        renderer.set_opacity(base * zone);
        const auto keep = target_count_;
        draw_drop_zone(glass);
        if (!m.drag_over) {
            target_count_ = keep; // going: nothing to drop on
        }
        renderer.set_opacity(base);
    }
}

void Painter::draw_layer(View v, Model& m, const Box& panel, Reaction& r) {
    if (conversation_shown(m, v)) {
        draw_conversation(m, panel, r);
        return;
    }
    switch (v) {
    case View::Input: draw_input(m, panel, r); break;
    case View::Thinking: draw_thinking(m, panel, r); break;
    case View::Done: draw_done(m, panel, r); break;
    case View::Answering: draw_answer(m, panel, r); break;
    case View::Permission: draw_permission(m, panel, r); break;
    case View::Quick: draw_quick(m, panel, r); break;
    case View::Steps: draw_steps(m, panel, r); break;
    case View::Log: draw_log(m, panel, r); break;
    case View::Report: draw_report(m, panel, r); break;
    case View::Terminal: draw_terminal(m, panel, r); break;
    case View::Rest: break;
    }
}

void Painter::disclose(std::uint64_t key, bool open) {
    auto it = std::ranges::find(disclosures_, key, &Disclosure::key);
    if (it == disclosures_.end()) {
        disclosures_.push_back({.key = key, .scene = {}}); // at a click, never in a frame of its own
        it = disclosures_.end() - 1;
        it->scene.snap(!open);
    }
    if (reduced_motion_) {
        it->scene.snap(open);
    } else if (open) {
        it->scene.open();
    } else {
        it->scene.close();
    }
    redraw_due_ = true;
}

auto Painter::disclosed(std::uint64_t key, bool open) const -> scenes::AccordionFrame {
    if (const auto it = std::ranges::find(disclosures_, key, &Disclosure::key); it != disclosures_.end()) {
        return it->scene.frame();
    }
    return open ? scenes::AccordionFrame{.room = 1, .body_alpha = 1, .body_dy = 0, .chevron = 90, .ground = 1} : scenes::AccordionFrame{};
}

void Painter::step_disclosures(double now) {
    if (now > disclosures_last_) {
        const auto dt = static_cast<float>(std::min(now - disclosures_last_, static_cast<double>(ui::motion::kMaxFrameDt)));
        for (auto& d : disclosures_) {
            d.scene.step(dt);
        }
        disclosures_last_ = now;
    }
    std::erase_if(disclosures_, [](const Disclosure& d) { return d.scene.done(); });
}

namespace {

// How long the answered question's rows take to go (MS 11): the last ticked one starts
// kRowsCloseAt + its turn after the answer, then closes on smooth.
auto rows_out_seconds(const Answered& a) -> double {
    namespace am = ui::motion::approval;
    const auto seconds = [](std::chrono::milliseconds ms) { return std::chrono::duration<double>(ms).count(); };
    if (a.choice == events::Choice::Deny || a.asked.items.empty()) {
        return 0.0;
    }
    const auto ticked = static_cast<std::size_t>(std::ranges::count(a.ticks, true));
    return seconds(am::kRowsCloseAt) + seconds(am::kRowStagger) * static_cast<double>(std::min(ticked, ui::motion::kStaggerMaxItems)) +
           seconds(ui::motion::hold::kRowsGoing);
}

} // namespace

void Painter::update_approval(const Model& m, double now) {
    if (now > approval_last_ && approval_last_ > -1.0e8) {
        approval_.step(static_cast<float>(std::min(now - approval_last_, static_cast<double>(ui::motion::kMaxFrameDt))));
    }
    approval_last_ = std::max(approval_last_, now);
    const std::uint64_t asking = wants_dim(m) ? m.permission->id + 1 : 0; // the model's rule: open, and asking
    const std::uint64_t answered = m.open && m.answered ? m.answered->asked.id + 1 : 0;
    if (asking != 0) {
        if (asked_id_ != asking || asked_answered_) { // a question comes
            asked_id_ = asking;
            asked_answered_ = false;
            if (reduced_motion_) {
                approval_.ask_reduced();
            } else {
                approval_.ask();
            }
        }
    } else if (asked_id_ != 0 && !asked_answered_) { // it is gone
        if (answered == asked_id_) { // answered: its card into its summary row
            asked_answered_ = true;
            if (reduced_motion_) {
                approval_.answer_reduced();
            } else {
                approval_.answer();
            }
        } else { // withdrawn, or the sheet shut: nothing to show for it
            asked_id_ = 0;
            if (reduced_motion_) {
                approval_.leave_reduced();
            } else {
                approval_.leave();
            }
        }
    } else if (asked_answered_ && answered != asked_id_) { // its summary row went with its turn
        asked_id_ = 0;
        asked_answered_ = false;
        approval_.leave();
    }
    if (approval_going(m)) {
        redraw_due_ = true; // its rows' turns come on frames
    }
    approval_frame_ = approval_.frame();
}

auto Painter::approval_going(const Model& m) const -> bool {
    if (!asked_answered_ || !m.answered || m.answered->asked.id + 1 != asked_id_) {
        return false;
    }
    return !approval_.done() || approval_.elapsed() < rows_out_seconds(*m.answered);
}

auto Painter::card_look(const events::Permission& p, bool going) const -> scenes::ApprovalFrame {
    if (asked_id_ == p.id + 1 && asked_answered_ == going) {
        return approval_frame_;
    }
    return going ? scenes::ApprovalFrame{.dim = 0, .room = 1, .contents = 0, .lift = 0, .shrink = 1, .tone = 1, .summary = 1, .status = 2}
                 : scenes::ApprovalFrame{.dim = 1, .room = 1, .contents = 1, .lift = 0, .shrink = 0, .tone = 0, .summary = 0, .status = 1};
}

void Painter::update_send(const Model& m, double now, bool was_conversation) {
    if (m.task != sent_task_ || !send_seen_) {
        // A task begun as the field was emptied: Enter (or the button). Any other new task — one of
        // the history, the handle's, the first one seen — finds the scene at rest.
        const bool sent = send_seen_ && m.open && working(m) && !m.question.empty() && now - gone_at_ <= kSendWithin;
        sent_task_ = m.task;
        send_seen_ = true;
        if (sent) {
            send_from_ = field_box_;
            send_fresh_ = !was_conversation;
            send_.reduced(reduced_motion_);
            send_.start();
            anim_.place(kGoneTrack, 0.0f);     // the text flies; it does not fade where it was (MS 04's clear)
            anim_.place(kHintBackTrack, 1.0f); // the new hint comes by the scene
        } else {
            send_.snap();
        }
        anim_.place(kRailTrack, 0.0f); // a new task's rail starts from nothing
        anim_.place(kRailAlphaTrack, 1.0f); // …and is there (Enter's scene brings it in)
        send_last_ = now;
    }
    if (now > send_last_) {
        send_.step(static_cast<float>(now - send_last_));
        send_last_ = now;
    }
    send_frame_ = send_.frame();
    question_drawn_ = false; // the turn says where its bubble's text is, each frame it draws it
}

void Painter::draw_rail(const Model& m, const Box& rail, bool busy) {
    // MS 05: the rail comes in after Enter; its share only grows (the model's), each new one on
    // smooth. MS 06: the work done, it fills and goes on smooth — never off in a frame.
    const bool live = !ui_->inert();
    const float there = live ? anim_.to(kRailAlphaTrack, busy ? 1.0f : 0.0f, ui::motion::kSmooth) : busy ? 1.0f : 0.0f;
    if (live) {
        row_motion_.rail = there;
    }
    if (!busy && there <= 0.004f) {
        return;
    }
    float share = m.progress;
    if (share >= 0) {
        share = anim_.geometry(kRailTrack, busy ? std::clamp(share, 0.0f, 1.0f) : 1.0f, ui::motion::kSmooth, 0.0f);
    }
    if (busy) {
        add_target("rail", rail);
        if (share >= 0) {
            add_target("rail:fill", {rail.x, rail.y, rail.w * share, rail.h});
        }
    }
    auto& rr = ui_->renderer();
    const float o = rr.opacity();
    rr.set_opacity(o * send_frame_.rail * there);
    work::bar_progress(*ui_, rail, share);
    rr.set_opacity(o);
}

void Painter::field_selection(const ui::text::Paragraph& para, const ui::text::Line& line, std::pair<std::size_t, std::size_t> sel,
                              float x, float y) {
    const auto [from, to] = sel;
    if (from == to || to <= line.first_byte || from > line.end_byte) {
        return;
    }
    const auto& glyphs = para.shaped().glyphs;
    float pen = x, x0 = -1, x1 = -1;
    for (std::uint32_t g = line.first_glyph; g < line.end_glyph && g < glyphs.size(); ++g) {
        if (x0 < 0 && glyphs[g].cluster >= from) {
            x0 = pen;
        }
        if (x1 < 0 && glyphs[g].cluster >= to) {
            x1 = pen;
        }
        pen += glyphs[g].x_advance / ui_->scale();
    }
    x0 = x0 < 0 ? pen : x0;
    x1 = x1 < 0 ? pen + (to > line.end_byte ? 4.0f : 0.0f) : x1;
    if (x1 > x0) {
        ui_->renderer().rect({x0, y + 3, x1 - x0, kFieldLine - 6}, {.fill = ui::th().selection});
    }
}

void Painter::draw_field_text(const Edit& e, std::string_view placeholder, float placeholder_size, float size,
                              const Box& box, bool bright) {
    if (e.text != field_text_ || e.caret != field_caret_) {
        field_text_ = e.text;
        field_caret_ = e.caret;
        edited_at_ = now_; // the caret stays lit while typing
        field_follow_ = true; // and in view again, whatever the wheel did
    }
    auto& renderer = ui_->renderer();
    renderer.push_clip(rect(box));
    float caret_x = 0;
    float caret_y = box.y;
    const float o = renderer.opacity();
    if (e.text.empty()) {
        field_scroll_ = 0;
        // MS 04: the text emptied goes on micro, as it was laid out, its selection with it.
        if (const float gone = anim_.to(kGoneTrack, 0.0f, ui::motion::kMicro); gone > 0.001f && gone_para_) {
            renderer.set_opacity(o * gone);
            const auto& lines = gone_para_->lines();
            for (std::size_t i = 0; i < lines.size(); ++i) {
                const float y = box.y + static_cast<float>(i) * kFieldLine - gone_scroll_;
                if (y + kFieldLine < box.y || y > box.y + box.h) {
                    continue;
                }
                field_selection(*gone_para_, lines[i], gone_sel_, box.x, y);
                ui_->draw_line(*gone_para_, lines[i], {box.x, ui_->centred_baseline(Face::Text, size, y, kFieldLine)}, ui::th().text);
            }
            renderer.set_opacity(o);
        }
        if (!placeholder.empty()) {
            // Opening, the hint comes in 220–400 ms from 10 px to the left (app::motion); emptied,
            // it comes back after the text on quick (MS 04: +60).
            const float back = anim_.to(kHintBackTrack, now_ - gone_at_ < kHintBackAfter ? 0.0f : 1.0f, ui::motion::kQuick);
            renderer.set_opacity(o * frame_.hint_alpha * back * send_frame_.hint); // after Enter: MS 05's new hint
            work::caption(*ui_, {box.x + frame_.hint_x, box.y, box.w, kFieldLine}, Face::Text, placeholder_size, placeholder,
                        bright ? ui::th().text : ui::th().text_4);
            renderer.set_opacity(o);
        }
    } else {
        // Its lines, 30 px apart, scrolled so that the caret's line is in the field.
        const auto [at, x] = caret_place(e.caret);
        const auto& lines = field_para_->lines();
        const float top = static_cast<float>(at) * kFieldLine;
        if (field_follow_ && top < field_scroll_) {
            field_scroll_ = top;
        }
        if (field_follow_ && top + kFieldLine > field_scroll_ + box.h) {
            field_scroll_ = top + kFieldLine - box.h;
        }
        field_scroll_ = std::clamp(field_scroll_, 0.0f, std::max(0.0f, static_cast<float>(lines.size()) * kFieldLine - box.h));
        const auto field_key = ui_->next_key(); // its lines: one text
        // Its selection on micro both ways (MS 04): lit in, and out where it last was.
        const auto now_sel = selection(e);
        if (now_sel.first != now_sel.second) {
            last_sel_ = now_sel;
        }
        const float lit = anim_.to(kSelTrack, now_sel.first != now_sel.second ? 1.0f : 0.0f, ui::motion::kMicro);
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const float y = box.y + static_cast<float>(i) * kFieldLine - field_scroll_;
            if (y + kFieldLine < box.y || y > box.y + box.h) {
                continue;
            }
            // Its selection, lit behind the glyphs (responsive text).
            if (lit > 0.001f) {
                renderer.set_opacity(o * lit);
                field_selection(*field_para_, lines[i], last_sel_, box.x, y);
                renderer.set_opacity(o);
            }
            ui_->draw_line(*field_para_, lines[i], {box.x, ui_->centred_baseline(Face::Text, size, y, kFieldLine)}, ui::th().text,
                           {.key = field_key});
        }
        caret_x = x;
        caret_y = box.y + top - field_scroll_;
    }
    const float caret_h = std::round(size + 5); // .caret 26 px for the 21 px text
    widgets::caret(*ui_, {box.x + caret_x, caret_y + (kFieldLine - caret_h) / 2, theme::caret::input_width, caret_h}, edited_at_);
    renderer.pop_clip();
}

void Painter::draw_editor_foot(Model& m, const Box& panel, Reaction& r) {
    auto& renderer = ui_->renderer();
    const Box foot{panel.x, panel.y + row_h_ - kEditorFoot, panel.w, kEditorFoot};
    add_target("editor:foot", foot);
    renderer.rect({foot.x, foot.y, foot.w, 1}, {.fill = ui::th().line});
    const float cy = foot.y + foot.h / 2;
    // Right to left: send, fold (⤡), the counter - always, here.
    const float b = wt::bar::send;
    const Box send{foot.x + foot.w - 16 - b, cy - b / 2, b, b};
    if (work::send_button(*ui_, id_of("send"), send, can_send(m))) {
        r.command = submit(m);
    }
    add_target("send", send);
    const Box fold{send.x - 8 - kEditorButton, cy - kEditorButton / 2, kEditorButton, kEditorButton};
    if (work::icon_button(*ui_, id_of("editor:collapse"), fold, kIconFold)) {
        m.editor = false;
    }
    add_target("editor:collapse", fold);
    const std::size_t chars = char_count(m.input.text);
    counter_ = counter_text(chars, counter_buf_, true);
    const float cw = work::text_width(*ui_, Face::Text, 12.5f, counter_);
    const Box count{fold.x - 14 - cw, cy - 10, cw, 20};
    // (its colour turns on quick — the map's unnamed "Character counter and its warn colour")
    const float count_warn = std::clamp(anim_.to(id_of("counter:warn"), counter_warn(chars) ? 1.0f : 0.0f, ui::motion::kQuick), 0.0f, 1.0f);
    work::caption(*ui_, count, Face::Text, 12.5f, counter_, blend(ui::th().text_3, ui::th().warn, count_warn));
    add_target("counter", count);
    // Left: what the keys do - [Enter] send · [Shift][Enter] new line · [Esc] collapse.
    const float kh = ui_->line_height(Face::Text, 11.5f) + 2;
    float x = foot.x + 22;
    const auto key = [&](std::string_view k) {
        const float w = work::text_width(*ui_, Face::Text, 11.5f, k) + 12;
        renderer.rect({x, cy - kh / 2, w, kh}, {.radius = {5, 5, 5, 5}, .border = ui::th().line_2, .border_width = 1});
        work::caption(*ui_, {x + 6, cy - kh / 2, w - 6, kh}, Face::Text, 11.5f, k, ui::th().text_2);
        x += w + 4;
    };
    const auto said = [&](std::string_view s, float gap) {
        x += 2;
        x += work::caption(*ui_, {x, cy - 10, std::max(0.0f, count.x - 14 - x), 20}, Face::Text, 12.5f, s, ui::th().text_3) + gap;
    };
    key("Enter");
    said("send", 14);
    key("Shift");
    key("Enter");
    said("new line", 14);
    key("Esc");
    said("collapse", 0);
    add_target("editor:keys", {foot.x + 22, cy - 10, std::max(0.0f, x - (foot.x + 22)), 20});
}

void Painter::draw_paste_notice(Model& m, const Box& panel) {
    auto& renderer = ui_->renderer();
    const Box note{panel.x, panel.y + row_h_ - kNotice, panel.w, kNotice};
    add_target("notice", note);
    renderer.rect({note.x, note.y, note.w, 1}, {.fill = ui::th().line});
    const float cy = note.y + note.h / 2;
    // Right to left: Undo [Ctrl+Z], Paste as text [Ctrl+Shift+V] - each puts the text in the field.
    const float kh = ui_->line_height(Face::Text, 11.0f) + 2;
    float x = note.x + note.w - 18;
    const auto link = [&](std::string_view name, std::string_view word, std::string_view key, ui::render::Color color) {
        const float kw = work::text_width(*ui_, Face::Text, 11.0f, key) + 10;
        const float ww = work::text_width(*ui_, Face::Text, 12.5f, word);
        const Box box{x - kw - 4 - ww, cy - 12, ww + 4 + kw, 24};
        const auto in = ui_->interact(id_of(name), box, false);
        work::caption(*ui_, {box.x, box.y, ww, box.h}, Face::Text, 12.5f, word, blend(color, ui::th().text, lit(id_of(name), in.hovered)));
        renderer.rect({box.x + ww + 4, cy - kh / 2, kw, kh}, {.radius = {4, 4, 4, 4}, .border = ui::th().line_2, .border_width = 1});
        work::caption(*ui_, {box.x + ww + 4 + 5, cy - kh / 2, kw - 5, kh}, Face::Text, 11.0f, key, ui::th().text_3);
        add_target(name, box);
        x = box.x - 16;
        if (in.clicked) {
            (void)paste_as_text(m);
        }
    };
    link("notice:undo", "Undo", "Ctrl+Z", ui::th().text_3);
    link("notice:as_text", "Paste as text", "Ctrl+Shift+V", ui::th().accent);
    // Left, under the text: what happened.
    const float lx = panel.x + 84;
    ui_->draw_icon(kIconInfo, 15.0f, {lx + 7.5f, cy}, ui::th().accent);
    work::caption(*ui_, {lx + 15 + 10, cy - 10, std::max(0.0f, x - (lx + 25)), 20}, Face::Text, 12.5f,
                "Long text was added as an attachment so the field stays readable.", ui::th().text_2);
}

void Painter::draw_paste_preview(Model& m, const Box& glass) {
    paste_bottom_ = 0;
    if (paste_preview_ >= m.attachments.size() || m.attachments[paste_preview_].text.empty()) {
        paste_preview_ = static_cast<std::size_t>(-1);
        return;
    }
    const auto& a = m.attachments[paste_preview_];
    constexpr float kW = 520, kHead = 38, kBody = 150, kFoot = 46, kGap = 12;
    // Under its chip, moved in from the panel's right edge (a chip far to the right).
    const float x = std::max(glass.x + kGap, std::min(paste_chip_.x, glass.x + glass.w - kGap - kW));
    const Box card{x, paste_chip_.y + paste_chip_.h + kGap, kW, kHead + kBody + kFoot};
    // It stays while the mouse is on the chip, on the card, or on its way between them.
    const auto& in = ui_->input();
    const float left = std::min(card.x, paste_chip_.x);
    const float right = std::max(card.x + card.w, paste_chip_.x + paste_chip_.w);
    const Box way{left, paste_chip_.y, right - left, card.y + card.h - paste_chip_.y};
    if (!(in.mouse_x >= way.x && in.mouse_x < way.x + way.w && in.mouse_y >= way.y && in.mouse_y < way.y + way.h)) {
        paste_preview_ = static_cast<std::size_t>(-1);
        return;
    }
    paste_bottom_ = card.y + card.h + kGap - glass.y;
    auto& renderer = ui_->renderer();
    renderer.rect({card.x, card.y, card.w, card.h},
                  {.fill = paste_card(), .radius = ui::card_corners(), .border = ui::th().line, .border_width = 1});
    add_target("paste:card", card);
    // Its head: what it is.
    const float tw = work::caption(*ui_, {card.x + 14, card.y, 200, kHead}, Face::Semibold, 12.5f, a.label, ui::th().text);
    work::caption(*ui_, {card.x + 14 + tw + 8, card.y, card.w - 36 - tw, kHead}, Face::Text, 12.0f, a.meta, ui::th().text_3);
    renderer.rect({card.x, card.y + kHead, card.w, 1}, {.fill = ui::th().line});
    // Its start, fading out.
    const float px = 13.0f * ui_->scale();
    if (!paste_para_ || paste_px_ != px) {
        paste_para_.emplace(fonts_->text, px);
        paste_px_ = px;
    }
    std::string_view head = a.text; // its start only: a log of megabytes is not shaped whole
    if (head.size() > kCardText) {
        std::size_t n = kCardText;
        while (n > 0 && (static_cast<unsigned char>(head[n]) & 0xC0) == 0x80) {
            --n;
        }
        head = head.substr(0, n);
    }
    if (paste_para_->text() != head) {
        paste_para_->set_text(head);
        paste_wrap_ = -1;
    }
    if (paste_wrap_ != card.w) { // wrapped again only when its width changes
        paste_para_->set_width((card.w - 28) * ui_->scale());
        paste_wrap_ = card.w;
    }
    const Box body{card.x + 14, card.y + kHead + 12, card.w - 28, kBody - 12};
    const float line = 13.0f * 1.55f;
    renderer.push_clip({body.x, body.y, body.w, body.h});
    const auto paste_key = ui_->next_key(); // its lines: one text
    for (std::size_t i = 0; i < paste_para_->lines().size(); ++i) {
        const float top = body.y + static_cast<float>(i) * line;
        if (top > body.y + body.h) {
            break;
        }
        ui_->draw_line(*paste_para_, paste_para_->lines()[i], {body.x, ui_->centred_baseline(Face::Text, 13.0f, top, line)},
                       ui::th().text_2, {.key = paste_key});
    }
    renderer.pop_clip();
    ui::render::Color clear = paste_card();
    clear.a = 0;
    renderer.rect({card.x + 1, body.y + body.h * 0.6f, card.w - 2, body.h * 0.4f},
                  {.fill = clear, .fill_to = paste_card(), .gradient_angle = std::numbers::pi_v<float> / 2});
    // Its foot: Open in editor, Paste as text.
    const float fy = card.y + kHead + kBody;
    renderer.rect({card.x, fy, card.w, 1}, {.fill = ui::th().line});
    const auto edit_size = work::button_size(*ui_, "Open in editor", true);
    const auto text_size = work::button_size(*ui_, "Paste as text", true);
    const Box edit{card.x + 14, fy + (kFoot - edit_size.h) / 2, edit_size.w, edit_size.h};
    const Box as_text{edit.x + edit.w + 8, edit.y, text_size.w, text_size.h};
    add_target("paste:card:edit", edit);
    add_target("paste:card:text", as_text);
    const std::size_t which = paste_preview_;
    if (work::button(*ui_, id_of("paste:edit"), edit, "Open in editor", work::ButtonStyle::Normal, true)) {
        open_pasted(m, which);
        paste_preview_ = static_cast<std::size_t>(-1);
    } else if (work::button(*ui_, id_of("paste:text"), as_text, "Paste as text", work::ButtonStyle::Normal, true)) {
        (void)paste_chip_as_text(m, which);
        paste_preview_ = static_cast<std::size_t>(-1);
    }
}

auto Painter::field_byte_at(const Box& box, float x, float y) const -> std::size_t {
    // The edge between two characters nearest the point, on the line under it (the last frame's lines).
    if (!field_para_ || field_para_->lines().empty()) {
        return 0;
    }
    const auto& lines = field_para_->lines();
    const auto row = static_cast<std::ptrdiff_t>(std::floor((y - box.y + field_scroll_) / kFieldLine));
    const auto& line = lines[static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(row, 0, static_cast<std::ptrdiff_t>(lines.size()) - 1))];
    const auto& glyphs = field_para_->shaped().glyphs;
    const float scale = ui_->scale();
    float pen = box.x;
    for (std::uint32_t g = line.first_glyph; g < line.end_glyph && g < glyphs.size(); ++g) {
        const float advance = glyphs[g].x_advance / scale;
        if (x < pen + advance / 2) {
            return std::max<std::size_t>(glyphs[g].cluster, line.first_byte);
        }
        pen += advance;
    }
    return line.end_byte;
}

void Painter::select_in_field(Model& m, const Box& box, bool held) {
    // The field by the mouse (responsive text): a press puts the caret there, a drag selects, a double
    // click a word; shift+click extends. Its own: the press starts no selection of the sheet's text.
    if (!field_keys(m) || m.key_mode) {
        field_selecting_ = false;
        return;
    }
    const auto& in = ui_->input();
    auto& e = m.input;
    if (held && in.mouse_pressed) {
        ui_->claim_press();
        ui_->clear_selection(); // the keys are the field's now: its Ctrl+C, its Ctrl+A (final review I5)
        keys_to_field_ = true;
        field_pressed_ = true;
        const auto at = std::min(field_byte_at(box, in.mouse_x, in.mouse_y), e.text.size());
        if (in.clicks >= 2) { // a word: between the spaces around the press
            std::size_t from = at, to = at;
            while (from > 0 && e.text[from - 1] != ' ' && e.text[from - 1] != '\n') {
                --from;
            }
            while (to < e.text.size() && e.text[to] != ' ' && e.text[to] != '\n') {
                ++to;
            }
            e.anchor = from;
            e.caret = to;
            field_selecting_ = false;
        } else {
            if (!in.shift || e.anchor == Edit::kNoAnchor) {
                e.anchor = in.shift ? e.caret : at;
            }
            e.caret = at;
            field_selecting_ = true;
        }
        edited_at_ = now_;
    } else if (held && field_selecting_ && in.mouse_down) {
        e.caret = std::min(field_byte_at(box, in.mouse_x, in.mouse_y), e.text.size());
        field_follow_ = true;
    }
    if (!in.mouse_down) {
        field_selecting_ = false;
        if (e.anchor == e.caret) {
            e.anchor = Edit::kNoAnchor;
        }
    }
}

void Painter::drag_field_bar(const Model& m, const Box& box) {
    // The field's scrollbar by the mouse (responsive text), as every scroll bar: before the field
    // takes the press for its caret. Its lines as the last frame laid them out.
    if (!field_over_ || m.input.text.empty() || !field_para_) {
        field_grab_ = -1;
        return;
    }
    const float content = static_cast<float>(field_para_->lines().size()) * kFieldLine;
    const float h = std::max(24.0f, box.h * box.h / content);
    const float most = std::max(1.0f, content - box.h);
    const float y = box.y + (box.h - h) * field_scroll_ / most;
    const auto hit = ui_->interact(id_of("field:bar"), {box.x + box.w - 14, box.y, 14 + 4, box.h}, false);
    const auto& in = ui_->input();
    if (hit.held && in.mouse_pressed) {
        ui_->claim_press();
        field_grab_ = in.mouse_y >= y && in.mouse_y < y + h ? in.mouse_y - y : h / 2; // the track: its middle comes there
    }
    if (hit.held && field_grab_ >= 0 && box.h > h) {
        field_scroll_ = std::clamp((in.mouse_y - field_grab_ - box.y) / (box.h - h) * most, 0.0f, std::max(0.0f, content - box.h));
        field_follow_ = false;
    } else if (!hit.held) {
        field_grab_ = -1;
    }
}

void Painter::draw_field_scroll(Model& m, const Box& box) {
    hint_len_ = 0;
    if (!field_over_ || m.input.text.empty()) {
        return;
    }
    auto& renderer = ui_->renderer();
    const float content = static_cast<float>(field_para_->lines().size()) * kFieldLine;
    // The top fade while scrolled: one line, the row's surface over the text's top.
    if (field_scroll_ > 2) {
        const ui::render::Color ground = surface_at(box.y + kFieldLine / 2);
        ui::render::Color clear = ground;
        clear.a = 0;
        renderer.rect({box.x - 4, box.y, box.w + 4, kFieldLine},
                      {.fill = ground, .fill_to = clear, .gradient_angle = std::numbers::pi_v<float> / 2});
    }
    // "↑ 3 lines above": in the row's top padding at the text's right; its click, to the start.
    if (const auto n = static_cast<int>(std::lround(field_scroll_ / kFieldLine)); n > 0) {
        const auto said = std::format_to_n(hint_buf_.data(), hint_buf_.size(), "{} {} above", n, n == 1 ? "line" : "lines");
        hint_len_ = static_cast<std::size_t>(said.out - hint_buf_.data());
        const std::string_view number{hint_buf_.data(), static_cast<std::size_t>(std::formatted_size("{}", n))};
        const std::string_view rest{hint_buf_.data() + number.size(), hint_len_ - number.size()};
        const float nw = work::text_width(*ui_, Face::Semibold, 12.0f, number);
        const float rw = work::text_width(*ui_, Face::Text, 12.0f, rest);
        const Box pill{box.x + box.w - (6 + 12 + 5 + nw + rw + 9), box.y - 25, 6 + 12 + 5 + nw + rw + 9, 20};
        const auto in = ui_->interact(id_of("field:above"), pill, false);
        renderer.rect({pill.x, pill.y, pill.w, pill.h}, {.fill = blend(ui::th().w2, ui::th().w3, lit(id_of("field:above"), in.hovered)),
                                                         .radius = {10, 10, 10, 10}, .border = ui::th().line, .border_width = 1});
        ui_->draw_icon(widgets::kIconChevronUp, 12.0f, {pill.x + 6 + 6, pill.y + pill.h / 2}, ui::th().text_2);
        work::caption(*ui_, {pill.x + 6 + 12 + 5, pill.y, nw, pill.h}, Face::Semibold, 12.0f, number, ui::th().text);
        work::caption(*ui_, {pill.x + 6 + 12 + 5 + nw, pill.y, rw, pill.h}, Face::Text, 12.0f, rest, ui::th().text_2);
        add_target("field:above", pill);
        if (in.clicked) {
            m.input.caret = 0; // to the start: the caret too, or it would pull the text back
            m.input.anchor = Edit::kNoAnchor;
            field_scroll_ = 0;
        }
    }
    // The scrollbar: 5 px at the text column's right, its length the share shown (drag_field_bar).
    const float h = std::max(24.0f, box.h * box.h / content);
    const float y = box.y + (box.h - h) * field_scroll_ / std::max(1.0f, content - box.h);
    const Box bar{box.x + box.w - kFieldBar, y, kFieldBar, h};
    renderer.rect({bar.x, bar.y, bar.w, bar.h}, {.fill = ui::th().scroll, .radius = {3, 3, 3, 3}});
    add_target("field:bar", bar);
}

auto Painter::field_view(const Model& m, View v) const -> bool {
    switch (v) {
    case View::Input:
    case View::Thinking:
    case View::Answering:
    case View::Permission:
    case View::Quick:
    case View::Steps:
    case View::Log:
    case View::Report:
    case View::Terminal: return true;
    default: return conversation_shown(m, v);
    }
}

auto Painter::field_keys(const Model& m) const -> bool {
    return m.open && !m.key_mode && !m.filter_focus && view(m) != View::Log && field_view(m, view(m));
}

auto Painter::shown_field(const Model& m) -> const Edit& {
    if (m.key_mode) {
        mask_into(m.key, masked_);
        return masked_;
    }
    return m.input;
}

void Painter::sync_field(const Model& m) {
    const float px = wt::type::typed * ui_->scale();
    if (!field_para_ || field_px_ != px) {
        field_para_.emplace(fonts_->text, px);
        field_px_ = px;
        field_wrap_ = -1;
    }
    if (const auto& e = shown_field(m); field_para_->text() != e.text) {
        if (e.text.empty() && !field_para_->text().empty()) {
            // Emptied (MS 04): its text goes as it was laid out, the hint comes back after it.
            gone_sel_ = last_sel_;
            gone_scroll_ = field_scroll_;
            std::swap(field_para_, gone_para_);
            if (!field_para_ || field_px_ != px) {
                field_para_.emplace(fonts_->text, px);
            }
            gone_at_ = now_;
            anim_.place(kGoneTrack, 1.0f);
            anim_.place(kHintBackTrack, 0.0f);
            anim_.place(kSelTrack, 0.0f);
        }
        field_para_->set_text(e.text);
        field_wrap_ = -1;
        end_caret_ = kNoCaret; // another text: the bytes mean other places
    }
    if (field_w_ <= 0) { // before the first row is drawn: the input bar's
        field_w_ = kOneColumn - kFieldLeft - wt::bar::pad_right - wt::bar::send - 10 - 40 - 10 - wt::bar::gap;
    }
    if (const std::size_t most = editor_open(m) ? kEditorLines : kFieldLines; most != field_most_) {
        field_most_ = most;
        field_wrap_ = -1; // over or not again
    }
    if (field_wrap_ != field_w_) {
        field_para_->set_width(field_w_ * ui_->scale());
        field_over_ = field_para_->lines().size() > field_most_;
        if (field_over_) { // beside its scrollbar (screen 28): the text 12 px narrower
            field_para_->set_width((field_w_ - kFieldGutter) * ui_->scale());
        }
        field_wrap_ = field_w_;
    }
}

auto Painter::editor_open(const Model& m) const -> bool {
    return editor_shown(m); // the model's own rule: Esc and the view agree (final review)
}

auto Painter::notice_open(const Model& m) const -> bool {
    return paste_notice(m);
}

auto Painter::count_row_lines(const Model& m) -> bool {
    const bool editor = editor_open(m); // up to 15 lines over its 56 px foot (screen 29)
    const std::size_t lines = std::clamp<std::size_t>(field_para_->lines().size(), 1, editor ? kEditorLines : kFieldLines);
    const float foot = editor ? kEditorFoot : notice_open(m) ? kNotice : 0.0f; // or the long paste's notice (screen 31)
    const float to = wt::bar::height + kFieldLine * static_cast<float>(lines - 1) + foot;
    if (to == row_to_) {
        return false;
    }
    // MS 04: a line more or less on quick; the field emptied (a send, a clear) folds back on smooth.
    row_token_ = to < row_to_ && shown_field(m).text.empty() ? ui::motion::kSmooth : ui::motion::kQuick;
    row_to_ = to;
    return true;
}

auto Painter::field_lines() const noexcept -> std::span<const ui::text::Line> {
    return field_para_ ? std::span<const ui::text::Line>{field_para_->lines()} : std::span<const ui::text::Line>{};
}

auto Painter::caret_place(std::size_t caret) const -> std::pair<std::size_t, float> {
    const auto& lines = field_para_->lines();
    const auto& glyphs = field_para_->shaped().glyphs;
    const auto text = field_para_->text();
    std::size_t at = 0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].first_byte <= caret) {
            at = i;
        }
    }
    if (lines.empty()) {
        return {0, 0.0f};
    }
    // At a wrap with no space (a word broken between characters, CJK) a line's end is the next one's
    // start: put there by End or Up/Down, the caret stays at the end of its line.
    if (caret == end_caret_ && at > 0 && lines[at].first_byte == caret && lines[at - 1].end_byte == caret) {
        --at;
    }
    // Its glyphs before the caret, the spaces hanging at its end too; never the '\n' that ends it.
    const std::size_t end = at + 1 < lines.size() ? lines[at + 1].first_glyph : glyphs.size();
    float x = 0;
    for (auto g = lines[at].first_glyph; g < end && glyphs[g].cluster < caret && text[glyphs[g].cluster] != '\n'; ++g) {
        x += glyphs[g].x_advance;
    }
    return {at, x / ui_->scale()};
}

auto Painter::caret_line(Model& m, bool down) -> bool {
    if (!field_keys(m)) {
        return false;
    }
    sync_field(m);
    const auto [at, x] = caret_place(m.input.caret);
    const auto& lines = field_para_->lines();
    if (down ? at + 1 >= lines.size() : at == 0) {
        return false; // the history's
    }
    // On the other line, the place nearest the caret's x.
    const auto& to = lines[down ? at + 1 : at - 1];
    const auto& glyphs = field_para_->shaped().glyphs;
    std::size_t best = to.first_byte;
    float best_d = std::abs(x);
    float acc = 0;
    for (auto g = to.first_glyph; g < to.end_glyph; ++g) {
        acc += glyphs[g].x_advance / ui_->scale();
        const std::size_t after = g + 1 < to.end_glyph ? glyphs[g + 1].cluster : to.end_byte;
        if (std::abs(acc - x) < best_d) {
            best_d = std::abs(acc - x);
            best = after;
        }
    }
    m.input.caret = best;
    end_caret_ = best == to.end_byte ? best : kNoCaret; // landed on its end: drawn there
    return true;
}

auto Painter::caret_at(const Model& m) -> std::pair<std::size_t, float> {
    sync_field(m);
    return caret_place(m.input.caret);
}

auto Painter::caret_edge(Model& m, bool end) -> bool {
    if (!field_keys(m)) {
        return false;
    }
    sync_field(m);
    const auto& line = field_para_->lines()[caret_place(m.input.caret).first];
    m.input.caret = end ? line.end_byte : line.first_byte;
    end_caret_ = end ? line.end_byte : kNoCaret;
    return true;
}

void Painter::draw_row(Model& m, const Box& panel, Reaction& r, std::string_view placeholder, float placeholder_size,
                       std::span<const RowChip> chips, RowButton button, bool collapse, bool bright, float reserve, bool expand_button) {
    // The editor (screen 29): no right group in the row - its send, fold and counter are in its
    // foot - and the field as wide as the row.
    const bool editor = editor_open(m) && button == RowButton::Send;
    const bool notice = !editor && notice_open(m);
    const float foot = editor ? kEditorFoot : notice ? kNotice : 0.0f;
    counter_ = {};
    if (notice) {
        draw_paste_notice(m, panel);
    }
    if (editor) {
        draw_editor_foot(m, panel, r);
        if (r.command) {
            return; // sent: the model has moved on
        }
        chips = {};
        button = RowButton::None;
        collapse = expand_button = false;
        reserve = -wt::bar::gap; // the field to the row's right padding
    }
    // Its controls by the field's last line (phase 10: the row grows down with the field).
    const float cy = panel.y + row_h_ - foot - wt::bar::height / 2;
    if (panel.h > row_h_ + 1) { // .row.sep: a line under the row when a body hangs below it
        ui_->renderer().rect({panel.x, panel.y + row_h_ - 1, panel.w, 1}, {.fill = ui::th().line});
    }
    // Right to left: the round button, the collapse button, the chips (.ml, 10 px apart).
    const float b = wt::bar::send;
    const Box round{right(panel) - wt::bar::pad_right - b, cy - b / 2, b, b};
    if (button == RowButton::Send) {
        // Opening, the button springs in from 260 ms (scale .6 → 1, app::motion).
        const float o = ui_->renderer().opacity();
        const float bs = b * frame_.button_scale;
        // MS 06: the work done, "stop" turns back into the arrow on base — the arrow takes its
        // press at once, "stop" is a picture going.
        const float stop_left = ui_->inert() ? 0.0f : anim_.to(kStopShareTrack, 0.0f, ui::motion::kBase);
        if (!ui_->inert()) {
            row_motion_.stop = stop_left;
        }
        ui_->renderer().set_opacity(o * frame_.button_alpha * (1.0f - stop_left));
        if (work::send_button(*ui_, id_of("send"), {round.x + (b - bs) / 2, round.y + (b - bs) / 2, bs, bs}, can_send(m))) {
            r.command = submit(m);
        }
        if (stop_left > 0.004f) {
            const float k = ui::motion::send::kStopScaleFrom + (1.0f - ui::motion::send::kStopScaleFrom) * stop_left;
            ui_->set_inert(true);
            ui_->renderer().set_opacity(o * stop_left);
            (void)work::stop_button(*ui_, id_of("stop:going"), {round.x + b * (1 - k) / 2, round.y + b * (1 - k) / 2, b * k, b * k}, stop_left);
            ui_->set_inert(false);
        }
        ui_->renderer().set_opacity(o);
        add_target("send", round);
    } else if (button == RowButton::Stop) {
        if (!ui_->inert()) {
            anim_.place(kStopShareTrack, 1.0f); // "stop" whole: the work done, it goes from here
            row_motion_.stop = 1.0f;
        }
        // MS 05: the button goes down and comes back; the arrow goes as "stop" grows out of it.
        const auto& sf = send_frame_;
        auto& rr = ui_->renderer();
        const float o = rr.opacity();
        const auto sized = [&](float k) { return Box{round.x + b * (1 - k) / 2, round.y + b * (1 - k) / 2, b * k, b * k}; };
        if (sf.arrow > 0.001f) { // the arrow it was, going: a picture, nothing to press
            const bool was_inert = ui_->inert();
            ui_->set_inert(true);
            rr.set_opacity(o * sf.arrow);
            (void)work::send_button(*ui_, id_of("send:going"), sized(sf.press), true);
            ui_->set_inert(was_inert);
        }
        rr.set_opacity(o * sf.stop);
        const float grown = ui::motion::send::kStopScaleFrom + (1.0f - ui::motion::send::kStopScaleFrom) * sf.stop;
        if (work::stop_button(*ui_, id_of("stop"), sized(sf.press * grown), sf.ring)) {
            r.command = stop(m);
        }
        rr.set_opacity(o);
        add_target("stop", round);
    }
    // RowButton::None: the caller draws its own controls in the `reserve` at the right.
    float x = button == RowButton::None ? right(panel) - wt::bar::pad_right - reserve - 10 : round.x - 10;
    if (button != RowButton::None && !m.key_mode) { // design/ui_model: the model chip, 12 px before the button
        const float o = ui_->renderer().opacity(); // MS 01: the pill comes in with the button
        ui_->renderer().set_opacity(o * frame_.button_alpha);
        x = draw_model_chip(m, round.x - 12, cy, panel.w) - 10;
        ui_->renderer().set_opacity(o);
    }
    if (panel.h <= row_h_ + 1 && !m.key_mode) { // the bar alone: the model's colour along its bottom edge
        draw_model_edge(m, panel);
    }
    if (button == RowButton::Send && !m.key_mode) {
        // From the field's third line its ⤢ (screen 27): the editor, Ctrl+E; left of it the counter
        // from 400 characters, yellow from 7 000.
        if (field_para_ && field_para_->lines().size() >= 3 && !m.editor) {
            const Box box{x - kEditorButton, cy - kEditorButton / 2, kEditorButton, kEditorButton};
            if (work::icon_button(*ui_, id_of("editor"), box, work::kIconExpand)) {
                m.editor = true;
            }
            if (const auto& in = ui_->input();
                in.mouse_x >= box.x && in.mouse_x < box.x + box.w && in.mouse_y >= box.y && in.mouse_y < box.y + box.h) {
                ui_->tooltip(id_of("editor"), box, "Open editor", "See the whole text at once", "Ctrl+E");
            }
            add_target("editor", box);
            x = box.x - 10;
        }
        const std::size_t chars = char_count(m.input.text);
        counter_ = counter_text(chars, counter_buf_);
        if (!counter_.empty()) {
            const float w = work::text_width(*ui_, Face::Text, 12.5f, counter_);
            const Box box{x - w, cy - 10, w, 20};
            work::caption(*ui_, box, Face::Text, 12.5f, counter_, counter_warn(chars) ? ui::th().warn : ui::th().text_3);
            add_target("counter", box);
            x = box.x - 10;
        }
    }
    if (button != RowButton::None && !m.key_mode) { // the paperclip (phase 10): Windows' file picker
        const Box clip{x - 40, cy - 20, 40, 40};
        if (work::icon_button(*ui_, id_of("attach"), clip, kIconAttach)) {
            r.attach = true;
        }
        if (const auto& in = ui_->input(); in.mouse_x >= clip.x && in.mouse_x < clip.x + clip.w && in.mouse_y >= clip.y &&
                                           in.mouse_y < clip.y + clip.h) {
            ui_->tooltip(id_of("attach"), clip, "Attach", "Ctrl+V \xE2\x80\x94 paste a screenshot"); // asked only while under the mouse
        }
        add_target("attach", clip);
        x = clip.x - 10;
    }
    if (collapse) {
        const float ib = wt::bar::icon_button;
        const Box box{x - ib, cy - ib / 2, ib, ib};
        if (work::icon_button(*ui_, id_of("collapse"), box, widgets::kIconChevronUp)) {
            r.close = true;
        }
        add_target("collapse", box);
        x = box.x - 10;
    }
    if (expand_button) {
        // The detailed mode: open, or folded back.
        const float ib = wt::bar::icon_button;
        const Box box{x - ib, cy - ib / 2, ib, ib};
        if (work::icon_button(*ui_, id_of("expand"), box, work::kIconExpand)) {
            expand(m, !m.expanded);
        }
        add_target("expand", box);
        x = box.x - 10;
    }
    for (const auto& chip : chips) {
        const auto s = work::chip_size(*ui_, chip.label);
        const float w = std::min(s.w, std::max(0.0f, x - (panel.x + kFieldLeft) - 120)); // the field keeps some room
        float turned = 1.0f;
        float room = w;
        const bool status = chip.target == "status" && !ui_->inert();
        if (status) {
            // MS 06, 07: the status turns — "Step 2 of 3", "Writing" into "Done" — its new words over
            // its old ones on base, its room following; the same tone's words (a clock ticking) just change.
            if (status_seen_ && chip.tone != status_tone_) {
                status_old_ = status_last_;
                status_old_n_ = status_last_n_;
                status_old_tone_ = status_tone_;
                anim_.place(kStatusTurnTrack, 0.0f);
            }
            status_seen_ = true;
            status_tone_ = chip.tone;
            status_last_n_ = std::min(chip.label.size(), status_last_.size());
            std::copy_n(chip.label.data(), status_last_n_, status_last_.data());
            turned = anim_.to(kStatusTurnTrack, 1.0f, ui::motion::kBase);
            room = anim_.geometry(kStatusWidthTrack, w, ui::motion::kBase);
            row_motion_.status = turned;
        }
        const Box box{x - room, cy - s.h / 2, room, s.h};
        // MS 05: after Enter the status comes in from a little to the right.
        const float o = ui_->renderer().opacity();
        if (status && turned < 0.996f && status_old_n_ > 0) { // the old words going, at the same right edge
            const std::string_view old{status_old_.data(), status_old_n_};
            const float ow = work::chip_size(*ui_, old).w;
            ui_->renderer().set_opacity(o * send_frame_.chip * (1.0f - turned));
            work::chip(*ui_, {x - ow, box.y, ow, box.h}, old, status_old_tone_);
        }
        ui_->renderer().set_opacity(o * send_frame_.chip * turned);
        work::chip(*ui_, {x - w + send_frame_.chip_dx, box.y, w, box.h}, chip.label, chip.tone);
        ui_->renderer().set_opacity(o);
        add_target(chip.target, box);
        x = box.x - 10;
    }
    // The field: under the row's 30 px top padding, its lines 30 px each (phase 10, screen 27).
    const float x0 = panel.x + kFieldLeft;
    const Box field{x0, panel.y + (wt::bar::height - kFieldLine) / 2, std::max(0.0f, x + 10 - wt::bar::gap - x0),
                    std::max(kFieldLine, row_h_ - foot - (wt::bar::height - kFieldLine))};
    if (!ui_->inert()) {
        drag_field_bar(m, field);
    }
    const auto field_hit = ui_->interact(id_of("field"), field, false);
    if (field_hit.hovered) {
        ui_->want_cursor(ui::Cursor::IBeam); // over text: the I-beam (screens 26–27)
    }
    if (!ui_->inert()) {
        select_in_field(m, field, field_hit.held);
    }
    add_target("field", field);
    if (!ui_->inert()) { // the front layer's row, not one fading out: its width, its lines, the row's height
        field_box_ = field;
        field_w_ = field.w;
        sync_field(m); // laid out at this width before it is drawn
        if (count_row_lines(m)) { // another view's row: it moves from this frame's time
            row_h_ = anim_.geometry(kRowTrack, row_to_, row_token_);
        }
        // Past six lines the wheel over the field scrolls its text (screen 28), until the caret moves.
        if (const auto& in = ui_->input(); field_over_ && in.wheel != 0 && in.mouse_x >= field.x && in.mouse_x < field.x + field.w &&
                                           in.mouse_y >= field.y && in.mouse_y < field.y + field.h) {
            field_scroll_ -= in.wheel * kFieldLine;
            field_follow_ = false;
        }
    }
    draw_field_text(shown_field(m), placeholder, placeholder_size, wt::type::typed, field, bright);
    if (!ui_->inert()) {
        draw_field_scroll(m, field);
    }
}

// design/ui_model: the model and the thinking level on view.
namespace {

constexpr float kNarrowRow = 760.0f;                       // a narrower bar: the chip's dot and bars only
constexpr double kHintHold = std::chrono::duration<double>(ui::motion::model::kHintHold).count();
constexpr double kHintOut = std::chrono::duration<double>(ui::motion::model::kHintOut).count();
constexpr double kHintShake = 0.2;
constexpr double kBarUpStep = std::chrono::duration<double>(ui::motion::model::kBarUpStep).count();
constexpr double kBarDownStep = std::chrono::duration<double>(ui::motion::model::kBarDownStep).count();
// The chip's and the hint's tracks (MS § 7).
constexpr ui::anim::Id kChipProTrack = 30, kChipNameTrack = 31, kChipLevelTrack = 32, kChipBarTrack = 33 /* ..35 */;
constexpr ui::anim::Id kHintAlphaTrack = 36, kHintRiseTrack = 37;

// The chosen model's ink: the accent, Flash or Pro alike (the designer: the two models have no hue of
// their own any more; a model is told by its name — and, from stage 4, by its provider's monogram).
auto model_ink(float alpha = 1.0f) -> ui::render::Color {
    auto c = ui::th().accent;
    c.a = static_cast<std::uint8_t>(std::lround(255.0f * std::clamp(alpha, 0.0f, 1.0f)));
    return c;
}

auto mix_ink(ui::render::Color a, ui::render::Color b, float t) -> ui::render::Color {
    const auto ch = [t](std::uint8_t x, std::uint8_t y) {
        return static_cast<std::uint8_t>(std::clamp(static_cast<float>(x) + (static_cast<float>(y) - static_cast<float>(x)) * t, 0.0f, 255.0f) + 0.5f);
    };
    return {ch(a.r, b.r), ch(a.g, b.g), ch(a.b, b.b), ch(a.a, b.a)};
}

} // namespace

auto Painter::draw_model_chip(Model& m, float right_x, float cy, float row_w) -> float {
    // MODEL_PICKER_SPEC § 7: the provider's monogram, the model's short name and three bars of the
    // level; a request that goes to another model (a picture, a model that does not see) shows that
    // one and a route mark; "once" while a choice is for the next message only; a model without
    // levels has no bars and is dimmed; a narrow bar keeps the monogram and the bars.
    const ModelChoice c = sent_with(m);
    const auto& model = model_of(c);
    const auto& provider = ai::catalog::provider_of(model);
    const bool routed = images_elsewhere(m);
    const bool once = m.next_only.has_value();
    const bool trouble = provider_trouble(m, provider.id) != events::Trouble::None; // a 7 px dot after the name
    const bool words = row_w >= kNarrowRow;
    const bool levels = model.thinking != ai::catalog::Tri::No && model.levels != 0;
    const int level = level_for(model, c.level);
    const std::string_view name = model.short_name;
    constexpr float kH = 32, kPadL = 6, kPadR = 12, kMono = 22, kGap = 7, kRoute = 13;
    constexpr float kBars = 3 * 3 + 2 * 2; // three bars, 3 px wide, 2 apart
    // MS § 7: the name crosses on quick, the width follows its words on base. Only the front layer's
    // chip moves the tracks (a layer fading out may be another row, another width).
    const bool live = !ui_->inert();
    if (live && name != chip_name_) {
        chip_name_from_ = chip_name_.empty() ? name : chip_name_;
        chip_name_ = name;
        anim_.place(kChipProTrack, chip_name_from_ == name ? 1.0f : 0.0f);
    }
    const float crossed = live ? anim_.to(kChipProTrack, 1.0f, ui::motion::kQuick) : 1.0f;
    const float name_own = words ? work::text_width(*ui_, Face::Semibold, 13.0f, name) : 0.0f;
    const float once_own = words && once ? work::text_width(*ui_, Face::Text, 10.5f, "once") : 0.0f;
    const float extra_own = (trouble && words ? 6 + 7 : 0.0f) + (routed && words ? 6 + kRoute : 0.0f) + (once_own > 0 ? 6 + once_own : 0.0f);
    const float name_w = live ? anim_.geometry(kChipNameTrack, name_own, ui::motion::kBase) : name_own;
    const float extra_w = live ? anim_.geometry(kChipLevelTrack, extra_own, ui::motion::kBase) : extra_own;
    // Its bars (MS § 7): up they light one by one, down they go from the top.
    if (live && level != chip_level_) {
        chip_level_from_ = chip_level_ < 0 ? level : chip_level_;
        chip_level_at_ = chip_level_ < 0 ? -1.0e9 : now_;
        chip_level_ = level;
    }
    float w = kPadL + kMono + kPadR;
    if (words) {
        w += kGap + name_w + extra_w;
    }
    if (levels) {
        w += kGap + kBars;
    }
    const Box box{right_x - w, cy - kH / 2, w, kH};
    const auto in = ui_->interact(id_of("model:chip"), box, false);
    auto& rr = ui_->renderer();
    const float hot = lit(id_of("model:chip"), in.hovered);
    const float r = kH / 2;
    if (m.menu.open) { // the menu open: a ring of the accent with its soft halo
        rr.rect({box.x - 4, box.y - 4, box.w + 8, box.h + 8}, {.fill = {}, .radius = {r + 4, r + 4, r + 4, r + 4}, .border = ui::th().accent_soft, .border_width = 4});
    }
    rr.rect(rect(box), {.fill = blend(ui::th().card, ui::th().w2, hot), .radius = {r, r, r, r},
                        .border = m.menu.open ? ui::th().accent : blend(ui::th().line, ui::th().line_2, hot), .border_width = m.menu.open ? 1.5f : 1.0f});
    const float o = rr.opacity();
    if (!levels) {
        rr.set_opacity(o * 0.75f); // a model without levels: dimmed
    }
    float x = box.x + kPadL;
    const Box mono{x, cy - kMono / 2, kMono, kMono};
    rr.rect(rect(mono), {.fill = ui::th().w3, .radius = {7, 7, 7, 7}});
    const float mw = work::text_width(*ui_, Face::Semibold, 10.5f, provider.monogram);
    work::caption(*ui_, {mono.x + (kMono - mw) / 2, mono.y, mw + 1, kMono}, Face::Semibold, 10.5f, provider.monogram, ui::th().text);
    add_target("model:mono", mono);
    x += kMono;
    if (words) {
        x += kGap;
        // The name crossing: the one going out as the other comes (quick).
        rr.push_clip({x, box.y, name_w + 1, kH});
        if (crossed < 0.996f && chip_name_from_ != name) {
            rr.set_opacity(o * (levels ? 1.0f : 0.75f) * (1.0f - crossed));
            work::caption(*ui_, {x, box.y, 120, kH}, Face::Semibold, 13.0f, chip_name_from_, ui::th().text);
        }
        rr.set_opacity(o * (levels ? 1.0f : 0.75f) * crossed);
        work::caption(*ui_, {x, box.y, name_own + 1, kH}, Face::Semibold, 13.0f, name, ui::th().text);
        rr.set_opacity(o * (levels ? 1.0f : 0.75f));
        rr.pop_clip();
        x += name_w;
        rr.push_clip({x, box.y, extra_w + 1, kH}); // its room follows it on base
        float ex = x;
        if (trouble) { // its provider: no key, the key rejected, busy, offline
            ex += 6;
            const Box dot{ex, cy - 3.5f, 7, 7};
            rr.rect(rect(dot), {.fill = ui::th().warn, .radius = {3.5f, 3.5f, 3.5f, 3.5f}});
            add_target("model:trouble", dot);
            ex += 7;
        }
        if (routed) { // this request goes to another model than the one chosen
            ex += 6;
            ui_->draw_icon(kIconRoute, 12.0f, {ex + kRoute / 2, cy}, ui::th().accent_2);
            add_target("model:route", {ex, cy - kRoute / 2, kRoute, kRoute});
            ex += kRoute;
        }
        if (once) {
            ex += 6;
            work::caption(*ui_, {ex, box.y, once_own + 1, kH}, Face::Text, 10.5f, "once", ui::th().text_3);
            add_target("model:once", {ex, box.y, once_own, kH});
        }
        rr.pop_clip();
        x += extra_w;
    }
    if (levels) {
        x += kGap;
        constexpr std::array<float, 3> kHeights{5.0f, 8.5f, 12.0f};
        constexpr std::array<std::string_view, 3> kNames{"model:bar:0", "model:bar:1", "model:bar:2"};
        const bool up = level > chip_level_from_;
        const double since = now_ - chip_level_at_;
        for (std::size_t i = 0; i < kHeights.size(); ++i) {
            const int n = static_cast<int>(i);
            const bool on = n < level;
            float light = on ? 1.0f : 0.0f;
            if (live) {
                // Its turn: the first to light is the lowest new one, the first to go the top one.
                const double turn = up ? (n - chip_level_from_) * kBarUpStep : (chip_level_from_ - 1 - n) * kBarDownStep;
                const bool due = since >= turn;
                if (!due) {
                    redraw_due_ = true; // its turn comes on frames
                }
                light = anim_.to(kChipBarTrack + i, due ? light : (n < chip_level_from_ ? 1.0f : 0.0f), up ? ui::motion::kBase : ui::motion::kQuick);
            }
            const Box bar{x + static_cast<float>(i) * 5.0f, cy + 6 - kHeights[i], 3, kHeights[i]};
            rr.rect(rect(bar), {.fill = mix_ink(ui::th().line_2, ui::th().accent, light), .radius = {1, 1, 1, 1}});
            add_target(kNames[i], bar);
        }
    }
    rr.set_opacity(o);
    if (in.hovered) {
        std::array<char, 160> head{}; // on the stack: std::format allocates every frame the chip is under the mouse
        std::array<char, 200> line{};
        const auto said = [](auto& buf, auto made) { return std::string_view{buf.data(), static_cast<std::size_t>(made.out - buf.data())}; };
        if (routed) {
            const auto& chosen = model_of(effective(m));
            ui_->tooltip(id_of("model:chip"), box, said(head, std::format_to_n(head.data(), head.size(), "This request goes to {}", model.full_name)),
                         said(line, std::format_to_n(line.data(), line.size(),
                                                     "{} doesn't see pictures. Your model stays {} \xE2\x80\x94 the next request without a picture goes to it.",
                                                     chosen.short_name, chosen.short_name)));
        } else {
            ui_->tooltip(id_of("model:chip"), box,
                         said(head, std::format_to_n(head.data(), head.size(), "{} \xC2\xB7 Thinking {}", model.full_name, thinking_name(level))),
                         "Change \xE2\x80\x94 click \xC2\xB7 Ctrl+M model \xC2\xB7 Ctrl+[ ] level");
        }
    }
    if (in.clicked) {
        m.menu.open ? close_menu(m) : open_menu(m);
        ui_->clear_focus(); // Enter is the menu's, not a button's focused before (review I2)
    }
    add_target("model:chip", box);
    model_chip_ = {.box = box, .words = words, .images = routed, .name = name, .level = thinking_name(level), .monogram = provider.monogram, .once = once};
    return box.x;
}

void Painter::draw_model_edge(const Model& /*m*/, const Box& panel) { // the accent, whichever model
    const float inset = ui::th().radius.window;
    ui_->renderer().rect({panel.x + inset, panel.y + row_h_ - 2, std::max(0.0f, panel.w - 2 * inset), 1.5f},
                         {.fill = model_ink(0.75f)});
}

auto Painter::notch_step(float& kept, bool over) -> int {
    if (!over) {
        kept = 0;
        return 0;
    }
    kept += ui_->input().notches;
    if (std::abs(kept) < 1.0f) {
        return 0;
    }
    const int step = kept > 0 ? 1 : -1; // one level a frame, however fast the wheel
    kept = 0;
    return step;
}

void Painter::draw_orb_controls(Model& m, const Box& orb, Reaction& r) {
    orb_controls_ = orb;
    const auto in = ui_->interact(id_of("model:orb"), orb, false);
    add_target("model:orb", orb);
    if (in.clicked) {
        m.menu.open ? close_menu(m) : open_menu(m);
        ui_->clear_focus();
    }
    if (const int notch = notch_step(orb_notches_, in.hovered); notch != 0) { // up: more thinking
        const auto step = step_level(m, notch, now_);
        if (step.choose) {
            r.choose = step.choose;
        }
    }
    if (look_frame_.ring_alpha > 0.001f) { // a model change: one ring, in the accent (MS § 7)
        const float s = orb.w * look_frame_.ring;
        const float cx = orb.x + orb.w / 2;
        const float cy = orb.y + orb.h / 2;
        ui_->renderer().rect({cx - s / 2, cy - s / 2, s, s},
                             {.radius = {s / 2, s / 2, s / 2, s / 2}, .border = model_ink(look_frame_.ring_alpha),
                              .border_width = 2.0f});
    }
}

auto Painter::hint_shown(const Model& m) const noexcept -> bool {
    const double t = now_ - m.choice_hint.at;
    return m.open && t >= 0 && t < kHintHold + kHintOut;
}

void Painter::draw_model_hint(const Model& m, const Box& glass) {
    if (!hint_up_) {
        hint_alpha_ = 0;
        return;
    }
    const auto& hint = m.choice_hint;
    const double t = now_ - hint.at;
    // MS § 7: in on quick coming 6 px on base, held 1.2 s, out on micro. Another hint while one is
    // up (the next notch) takes its place as it is.
    if (hint.at != hint_seen_at_) {
        hint_seen_at_ = hint.at;
        if (hint_alpha_ < 0.01f) {
            anim_.place(kHintAlphaTrack, 0.0f);
            anim_.place(kHintRiseTrack, ui::motion::model::kHintRise);
        }
    }
    const bool held = t < kHintHold;
    const float a = anim_.to(kHintAlphaTrack, held ? 1.0f : 0.0f, held ? ui::motion::kQuick : ui::motion::kMicro);
    const float dy = anim_.geometry(kHintRiseTrack, 0.0f, ui::motion::kBase);
    const float dx = hint.limit && t < kHintShake
                         ? 4.0f * static_cast<float>(std::sin(2.0 * std::numbers::pi * t / 0.07) * (1.0 - t / kHintShake))
                         : 0.0f;
    const float text_w = work::text_width(*ui_, Face::Text, 12.5f, hint.text);
    const Box box{glass.x + 12 + dx, glass.y + glass.h + 8 + dy, 12 + 7 + 8 + text_w + 12, 28};
    auto& rr = ui_->renderer();
    const float o = rr.opacity();
    hint_alpha_ = std::clamp(a, 0.0f, 1.0f);
    rr.set_opacity(o * hint_alpha_);
    rr.rect(rect(box), {.fill = ui::th().pop, .radius = {14, 14, 14, 14},
                        .border = hint.limit ? ui::th().warn : ui::th().line, .border_width = hint.limit ? 1.5f : 1.0f});
    // The model the request goes to: Flash while a picture is in with Pro chosen, as the orb and the chip.
    rr.rect({box.x + 12, box.y + 14 - 3.5f, 7, 7}, {.fill = model_ink(), .radius = {3.5f, 3.5f, 3.5f, 3.5f}});
    work::caption(*ui_, {box.x + 12 + 7 + 8, box.y, text_w + 1, box.h}, Face::Text, 12.5f, hint.text, ui::th().text);
    rr.set_opacity(o);
    add_target("model:hint", box);
}

void Painter::draw_quick(Model& m, const Box& panel, Reaction& r) {
    if (!m.quick) {
        return; // fading out after Esc
    }
    // The field is empty for the next question; this one stays over its answer.
    std::array<RowChip, 1> chip{{{m.quick->chip, work::Tone::Ok, "chip"}}};
    draw_row(m, panel, r, kBarPlaceholder, wt::type::placeholder, std::span{chip.data(), m.quick->chip.empty() ? 0u : 1u},
             RowButton::Send, false, true);
    if (!m.quick) {
        return; // a new question took its place
    }
    const auto& q = *m.quick;
    // .qa: padding 0 6 inside the body's 0 18; the value block 250 wide, the parts, the action.
    const Box section{panel.x + 24, panel.y + row_h_, panel.w - 48, std::max(0.0f, panel.h - row_h_)};
    const float qh = ui_->line_height(Face::Text, 12.5f);
    const Box asked{section.x, section.y + 10, section.w, qh};
    work::caption(*ui_, asked, Face::Text, 12.5f, m.question, ui::th().text_2);
    add_target("quick:question", asked);
    const float cy = section.y + section.h / 2 - 2; // the question over it, the hint line at the bottom
    float right_edge = section.x + section.w;
    if (q.action) {
        const auto size = work::button_size(*ui_, q.action->label);
        const Box box{right_edge - size.w, cy - size.h / 2, size.w, size.h};
        const std::string_view name = action_name(q.action->id);
        add_target(name, box);
        if (work::button(*ui_, id_of(name), box, q.action->label)) {
            const std::string id = q.action->id;
            r.command = press(m, id);
            return;
        }
        right_edge = box.x - 18;
    }
    // The value (30 px) with its unit (15 px), the line under it (13 px).
    const float value_h = ui_->line_height(Face::Display, wt::type::quick_value);
    const float sub_h = q.sub.empty() ? 0.0f : ui_->line_height(Face::Text, 13.0f) + 4;
    const float top = cy - (value_h + sub_h) / 2;
    const float vw = work::caption(*ui_, {section.x, top, 250, value_h}, Face::Display, wt::type::quick_value, q.value,
                                 ui::th().text);
    add_target("quick:value", {section.x, top, vw, value_h});
    if (!q.unit.empty()) {
        const float unit_x = section.x + vw + 6;
        const float base = ui_->centred_baseline(Face::Display, wt::type::quick_value, top, value_h);
        const auto& unit = ui_->shape(Face::Text, q.unit, wt::type::quick_unit);
        ui_->draw_text(Face::Text, unit, wt::type::quick_unit, {unit_x, base}, ui::th().text_3);
    }
    if (!q.sub.empty()) {
        work::caption(*ui_, {section.x, top + value_h + 4, 250, sub_h - 4}, Face::Text, 13.0f, q.sub, ui::th().text_3);
    }
    // The parts: a 12 px bar and its legend under it (12 px, squares of 8).
    const float mx = section.x + 250 + 18;
    const float mw = right_edge - mx;
    if (!q.segments.empty() && mw > 40) {
        std::array<work::Segment, 8> parts{};
        const std::size_t n = std::min(q.segments.size(), parts.size());
        // The theme's series, in their order everywhere; the fifth piece and further are "the rest".
        const std::array<ui::render::Color, 4> series{ui::th().c1, ui::th().c2, ui::th().c3, ui::th().c4};
        for (std::size_t i = 0; i < n; ++i) {
            parts[i] = {static_cast<float>(q.segments[i].share), i < series.size() ? series[i] : ui::th().c_rest};
        }
        const float legend_h = ui_->line_height(Face::Text, 12.0f);
        const float block = wt::quick::stack + 8 + legend_h;
        const float by = cy - block / 2;
        work::stacked_bar(*ui_, {mx, by, mw, wt::quick::stack}, std::span{parts.data(), n});
        float lx = mx;
        const float ly = by + wt::quick::stack + 8;
        for (std::size_t i = 0; i < n && lx < mx + mw; ++i) {
            ui_->renderer().rect({lx, ly + (legend_h - 8) / 2, 8, 8}, {.fill = parts[i].color, .radius = {2, 2, 2, 2}});
            lx += 14;
            lx += work::caption(*ui_, {lx, ly, std::max(0.0f, mx + mw - lx), legend_h}, Face::Text, 12.0f,
                              q.segments[i].label, ui::th().text_3) +
                  14;
        }
    }
    // .hint: what the keys do here (the mockup has it under the bar; the window ends with the panel).
    work::caption(*ui_, {section.x, section.y + section.h - 24, section.w, 18}, Face::Text, 12.5f,
                "Esc \xE2\x80\x94 close \xC2\xB7 Tab \xE2\x80\x94 more \xC2\xB7 \xE2\x86\x91 \xE2\x80\x94 history",
                ui::th().text_3);
}

void Painter::draw_input(Model& m, const Box& panel, Reaction& r) {
    if (m.key_mode) {
        // The DeepSeek key (phase 7): the same bar, its text dots, said what it is for. The user
        // (2026-10-16): DeepSeek checks it before it is kept — the row says it is asked, or why the
        // key wasn't kept; without a key there is no Esc past it.
        const bool refused = !m.key_error.empty();
        std::array<RowChip, 1> chip{{{refused ? "Not kept" : m.key_checking ? "Checking" : "API key",
                                      refused ? work::Tone::Crit : work::Tone::Accent, "key"}}};
        key_line_ = m.key_checking ? std::string_view{"Checking the key with DeepSeek\xE2\x80\xA6"}
                    : refused      ? std::string_view{m.key_error}
                    : m.has_key    ? std::string_view{"Paste your DeepSeek API key \xE2\x80\x94 Enter checks it, Esc cancels"}
                                   : std::string_view{"Paste your DeepSeek API key \xE2\x80\x94 Enter checks it"};
        draw_row(m, panel, r, key_line_, wt::type::placeholder, std::span{chip.data(), 1u}, RowButton::Send, false);
        return;
    }
    std::array<RowChip, 1> unsafe{{{kUnsafe, work::Tone::Warn, "unsafe"}}};
    draw_row(m, panel, r, kBarPlaceholder, wt::type::placeholder, std::span{unsafe.data(), m.unsafe ? 1u : 0u},
             RowButton::Send, false);
}

void Painter::draw_thinking(Model& m, const Box& panel, Reaction& r) {
    // The bar stays a field (refine on the go); a chip says what the agent is doing.
    std::array<RowChip, 2> chips{{{m.step.empty() ? kThinking : std::string_view{m.step}, work::Tone::Accent, "chip"},
                                  {kUnsafe, work::Tone::Warn, "unsafe"}}};
    draw_row(m, panel, r, kRefinePlaceholder, wt::type::placeholder_small, std::span{chips.data(), m.unsafe ? 2u : 1u},
             RowButton::Stop, false, false, 0, has_detail(m));
    // .rprog on the row's bottom edge, inside the rounded corners.
    const float inset = shape_.radius * 0.6f;
    auto& renderer = ui_->renderer();
    const float y = panel.y + std::min(panel.h, row_h_) - wt::bar::progress;
    renderer.push_clip({panel.x + inset, panel.y, panel.w - 2 * inset, panel.h});
    draw_rail(m, {panel.x, y, panel.w, wt::bar::progress});
    renderer.pop_clip();
}

void Painter::draw_done(Model& m, const Box& panel, Reaction& r) {
    const float cy = panel.y + wt::bar::height / 2;
    float x = panel.x + kFieldLeft;
    if (!m.failed) {
        const float badge = theme::button::ok_badge;
        widgets::ok_badge(*ui_, {x, cy - badge / 2, badge, badge});
        x += badge + 10;
    }
    // The buttons, right-aligned.
    float bx = right(panel) - wt::bar::pad_right;
    // By index, not over a copy (a copy allocates every frame): a press changes the model, so the
    // size is checked on every step and nothing of the pressed action is used after press().
    for (std::size_t n = 0; n < m.done_actions.size(); ++n) {
        const auto& a = m.done_actions[m.done_actions.size() - 1 - n];
        const auto size = work::button_size(*ui_, a.label);
        bx -= size.w;
        const Box box{bx, cy - size.h / 2, size.w, size.h};
        const std::string_view name = action_name(a.id);
        add_target(name, box);
        if (work::button(*ui_, id_of(name), box, a.label,
                         a.primary ? work::ButtonStyle::Primary : work::ButtonStyle::Normal)) {
            const std::string id = a.id;
            r.command = press(m, id);
        }
        bx -= 8;
    }
    if (has_detail(m)) {
        // The task's terminal and plan stay there to read after it ends.
        const float ib = wt::bar::icon_button;
        const Box box{bx - ib, cy - ib / 2, ib, ib};
        if (work::icon_button(*ui_, id_of("expand"), box, work::kIconExpand)) {
            expand(m, true);
        }
        add_target("expand", box);
        bx = box.x - 8;
    }
    // Gone by itself after kDoneHold without the mouse over it, or with a click on the bar.
    if (shown_ == View::Done && !r.command) {
        const auto& in = ui_->input();
        const bool over = in.mouse_x >= panel.x && in.mouse_x < right(panel) && in.mouse_y >= panel.y &&
                          in.mouse_y < panel.y + panel.h;
        if (over) {
            done_since_ = now_;
        }
        done_pressed_ = in.mouse_pressed ? over : done_pressed_;
        const bool clicked = done_pressed_ && in.mouse_released && over;
        if (in.mouse_released) {
            done_pressed_ = false;
        }
        if (clicked || now_ - done_since_ > kDoneHold) {
            dismiss(m);
            return;
        }
    }
    // .rt: the title (17 px, Display semibold) over its detail (13.5 px).
    const float room = std::max(0.0f, bx - wt::bar::gap - x);
    const Face face = m.failed ? Face::Text : Face::Display;
    const float title_h = ui_->line_height(face, wt::type::title);
    const float detail_h = m.done_detail.empty() ? 0.0f : ui_->line_height(Face::Text, wt::type::sub) + 3;
    const float top = cy - (title_h + detail_h) / 2;
    work::caption(*ui_, {x, top, room, title_h}, face, wt::type::title, m.done_title, ui::th().text);
    if (!m.done_detail.empty()) {
        work::caption(*ui_, {x, top + title_h + 3, room, detail_h - 3}, Face::Text, wt::type::sub, m.done_detail,
                    ui::th().text_3);
    }
}

void Painter::sync_answer(const Model& m) {
    const float scale = ui_->scale();
    if (!doc_ || doc_scale_ != scale * ui_->text_scale()) {
        doc_.emplace(*fonts_, theme::type::prose * ui_->text_scale(), scale);
        doc_scale_ = scale * ui_->text_scale();
    }
    // The smoother (MS § 6 "Streaming", app::scenes::Stream): tokens come in bursts, the text comes
    // out evenly, whole words only. Each block marks how far it has grown per frame, so every
    // letter comes in from the frame it was shown (widgets::stream_text). An answer that is whole
    // when its task is first seen (the history, a turn restored) is there at once.
    const double dt = std::clamp(now_ - last_reveal_, 0.0, 1.0);
    last_reveal_ = now_;
    if (m.task != stream_task_) {
        stream_task_ = m.task;
        stream_.reset();
        if (!m.streaming && !m.answer.empty()) {
            stream_.show_all(m.answer);
        }
    }
    stream_.step(m.answer, !m.streaming, static_cast<float>(dt));
    const std::size_t shown = stream_.shown();
    shown_bytes_ = shown;
    // Shown whole and no more coming: markup left open is text after all.
    doc_->sync(std::string_view{m.answer}.substr(0, shown), shown == m.answer.size() && !m.streaming);
    const auto& blocks = doc_->blocks();
    times_.resize(blocks.size());
    block_sizes_.resize(blocks.size(), 0);
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        const std::size_t size = blocks[i].size();
        if (size < block_sizes_[i]) { // a new answer reused this slot
            times_[i].clear();
            block_sizes_[i] = 0;
        }
        if (size > block_sizes_[i]) {
            times_[i].mark(static_cast<std::uint32_t>(size), now_);
            block_sizes_[i] = size;
            last_token_at_ = now_;
        }
    }
    if (m.answer.empty()) {
        scroll_ = 0;
    }
}

void Painter::draw_answer(Model& m, const Box& panel, Reaction& r) {
    auto& renderer = ui_->renderer();
    const float scale = ui_->scale();

    // The row: a field for the next words, what is going on, collapse, send or stop.
    const bool working = sheet::working(m);
    std::array<RowChip, 2> chips{{{"Answering", work::Tone::Accent, "chip"}, {kUnsafe, work::Tone::Warn, "unsafe"}}};
    const std::size_t first = m.streaming ? 0 : 1;
    const std::size_t count = (m.streaming ? 1 : 0) + (m.unsafe ? 1 : 0);
    draw_row(m, panel, r, kFollowUpPlaceholder, wt::type::placeholder_small, std::span{chips.data() + first, count},
             working ? RowButton::Stop : RowButton::Send, true);

    // .sbody: the question and the answer, anchored to the bottom, scrolled with the wheel.
    const Box body{panel.x + wt::body::pad_x, panel.y + row_h_ + wt::body::pad_top, panel.w - 2 * wt::body::pad_x,
                   panel.h - row_h_ - wt::body::pad_top - 16};
    if (body.h <= 0 || body.w <= 0) {
        return;
    }
    sync_answer(m);
    // Prose at most 62 characters wide (max-width 62ch); code blocks take the body's width.
    doc_->set_width(std::min(body.w, 62 * work::text_width(*ui_, Face::Text, theme::type::prose, "0")), body.w);
    const float ts = ui_->text_scale();
    const float line = theme::type::prose * ts * theme::type::prose_line;

    // .bubble: up to 85 % of the body wide, wrapped; padding 9 14.
    constexpr float kBubblePadX = 14.0f;
    constexpr float kBubblePadY = 9.0f;
    const float bubble_text_max = body.w * theme::bubble::max_width - 2 * kBubblePadX - 2;
    if (!question_ || question_text_ != m.question || question_scale_ != scale * ts) {
        question_.emplace(fonts_->text, wt::type::bubble * ts * scale);
        question_->set_text(m.question);
        question_text_ = m.question;
        question_scale_ = scale * ts;
    }
    question_->set_width(bubble_text_max * scale);
    float question_w = 0;
    for (const auto& l : question_->lines()) {
        question_w = std::max(question_w, l.width / scale);
    }
    const float q_line = ui_->line_height(Face::Text, wt::type::bubble);
    const float q_w = question_w + 2 * kBubblePadX + 2;
    const float q_h = static_cast<float>(question_->lines().size()) * q_line + 2 * kBubblePadY + 2;

    const float words = doc_->height();
    float content = q_h + theme::bubble::margin_bottom + words;
    content += 10; // the last block's margin
    // The buttons follow the text once it is all shown.
    const bool has_actions = shown_bytes_ == m.answer.size() && !m.actions.empty();
    float pill_h = 0;
    for (const auto& a : has_actions ? m.actions : std::span<const events::Action>{}) {
        pill_h = std::max(pill_h, work::button_size(*ui_, a.label).h);
    }
    if (has_actions) {
        content += 4 + pill_h;
    }
    // What it cost, under it all once the answer is all shown: "$0.0004 · 92% cached" (phase 7).
    const bool has_cost = shown_bytes_ == m.answer.size() && m.spent.has_value() && !m.streaming;
    const float cost_h = ui_->line_height(Face::Text, 11.5f);
    if (has_cost) {
        content += 6 + cost_h;
    }

    const auto& in = ui_->input();
    if (in.wheel != 0 && in.mouse_x >= body.x && in.mouse_x < right(body) && in.mouse_y >= body.y &&
        in.mouse_y < body.y + body.h) {
        scroll_ += in.wheel * line;
    }
    scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, content - body.h));

    // The shown height eases towards the real one (time constant ~70 ms); a shorter answer (a new
    // question) or the first frame snaps.
    content_ = content;
    const double dt = std::max(now_ - last_draw_, 0.0); // exact for any frame interval
    last_draw_ = now_;
    if (shown_content_ <= 0 || content < shown_content_ - 1.0f) {
        shown_content_ = content;
        shown_content_spring_.snap(content);
    } else {
        // A paragraph's height: on quick, from where it is and at the speed it has (MS 07).
        shown_content_spring_.x = shown_content_;
        shown_content_spring_.range = std::max(1.0f, std::abs(content - shown_content_));
        shown_content_spring_.set(content, ui::motion::kQuick);
        shown_content_spring_.step(static_cast<float>(dt));
        shown_content_ = shown_content_spring_.x;
        if (std::abs(content - shown_content_) < 0.25f) {
            shown_content_ = content;
            shown_content_spring_.snap(content);
        }
    }
    float y = body.y + body.h - shown_content_ + scroll_;
    add_target("answer", {body.x, y, body.w, content});
    renderer.push_clip(rect(body));
    const Box bubble{right(body) - q_w, y, q_w, q_h};
    work::bubble(*ui_, bubble);
    const auto& q_sized = fonts_->text.sized(0, wt::type::bubble * ts * scale);
    const auto question_key = ui_->next_key(); // its lines: one text
    for (std::size_t i = 0; i < question_->lines().size(); ++i) {
        const float top = bubble.y + 1 + kBubblePadY + static_cast<float>(i) * q_line;
        const float base = top + (q_line - (q_sized.ascent + q_sized.descent) / scale) / 2 + q_sized.ascent / scale;
        ui_->draw_line(*question_, question_->lines()[i], {bubble.x + 1 + kBubblePadX, base}, ui::th().text, {.key = question_key});
    }
    y += q_h + theme::bubble::margin_bottom;

    draw_markdown({body.x, y, body.w, words}, body, m.streaming, r);
    y += words + 10;
    if (has_actions) {
        y += 4;
        float x = body.x;
        // By index, as in draw_done: a press changes m.actions.
        for (std::size_t i = 0; i < m.actions.size(); ++i) {
            const auto& a = m.actions[i];
            const auto size = work::button_size(*ui_, a.label);
            const Box box{x, y, size.w, size.h};
            const std::string_view name = action_name(a.id);
            add_target(name, box);
            if (work::button(*ui_, id_of(name), box, a.label,
                             a.primary ? work::ButtonStyle::Primary : work::ButtonStyle::Normal)) {
                const std::string id = a.id;
                r.command = press(m, id);
            }
            x += size.w + 8;
        }
        y += pill_h;
    }
    if (has_cost) {
        y += 6;
        const Box cost{body.x, y, body.w, cost_h};
        add_target("answer:cost", cost);
        work::caption(*ui_, cost, Face::Text, 11.5f, spent_text(*m.spent), ui::th().text_3);
    }
    renderer.pop_clip();
}

} // namespace app::sheet
