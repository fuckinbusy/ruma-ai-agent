#pragma once

// app::sheet::Painter — draws the sheet for a Model, frame by frame, and turns clicks into the
// same Reactions keys give. The panel's shape follows the view with the mockup's spring curve
// (width, height, radius .6s), and switching views fades the old layer out (.22s) and the new
// one in (.35s after .22s), as the mockup's .L layers do. Geometry comes from ui::theme.
//
// Per frame: update() -> render_orb() -> [renderer.begin, ui.begin_frame] -> draw() -> [end].

#include "app/answer.hpp"
#include "app/motion.hpp"
#include "app/scenes/accordion.hpp"
#include "app/scenes/approval.hpp"
#include "app/scenes/fold.hpp"
#include "app/scenes/send.hpp"
#include "app/scenes/sign.hpp"
#include "app/scenes/stream.hpp"
#include "app/sheet_model.hpp"
#include "theme/contrast.hpp"
#include "ui/anim.hpp"
#include "ui/orb.hpp"
#include "ui/render/target.hpp"
#include "ui/render/texture.hpp"
#include "ui/text/paragraph.hpp"
#include "ui/ui.hpp"
#include "ui/widgets.hpp"
#include "ui/table.hpp"
#include "ui/work.hpp"

#include <array>
#include <concepts>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace app::sheet {

// The dim window (MS 11) for a dim of `dim` (0 clear .. 1): shown while any of it is there — hidden
// only at none — its layered alpha `most` at 1.
struct DimWindow {
    bool shown = false;
    std::uint8_t alpha = 0;
};
[[nodiscard]] constexpr auto dim_window(float dim, std::uint8_t most) noexcept -> DimWindow {
    if (!(dim > 0.002f)) {
        return {};
    }
    const float a = (dim > 1.0f ? 1.0f : dim) * static_cast<float>(most);
    const auto byte = static_cast<std::uint8_t>(a + 0.5f);
    return {.shown = true, .alpha = byte == 0 ? std::uint8_t{1} : byte};
}

// What a light under the mouse draws with (MS 14: on quick, never on in a frame): a colour between
// two, and a colour at a share of its own opacity.
[[nodiscard]] inline auto blend(ui::render::Color a, ui::render::Color b, float t) noexcept -> ui::render::Color {
    const auto part = [t](std::uint8_t from, std::uint8_t to) {
        return static_cast<std::uint8_t>(static_cast<float>(from) + (static_cast<float>(to) - static_cast<float>(from)) * t + 0.5f);
    };
    return {part(a.r, b.r), part(a.g, b.g), part(a.b, b.b), part(a.a, b.a)};
}
// `base` with the mouse on it: the first step of the ink ladder (w1) laid over it at `lit` of its
// strength — over a card, a row, anything (the designer: a light, not another solid colour).
[[nodiscard]] inline auto under_mouse(ui::render::Color base, float lit) noexcept -> ui::render::Color {
    auto top = ui::th().w1;
    top.a = static_cast<std::uint8_t>(std::lround(static_cast<float>(top.a) * std::clamp(lit, 0.0f, 1.0f)));
    return ::theme::layer(top, base);
}

[[nodiscard]] inline auto faint(ui::render::Color c, float share) noexcept -> ui::render::Color {
    c.a = static_cast<std::uint8_t>(static_cast<float>(c.a) * share + 0.5f);
    return c;
}

struct Shape {
    float width = 0;
    float height = 0;
    float radius = 0;
};

// The monitor's work area the sheet hangs in (logical pixels).
struct Room {
    float w = 1.0e9f;
    float h = 1.0e9f;
};

// Phase 8 (mockup-spec § 1): the bar and a one-column conversation are 860 wide.
inline constexpr float kOneColumn = 860.0f;
// Below the bar, the answer's body (and the permission card's, until it has its own layout).
inline constexpr float kAnswerBody = 380.0f;
// A quick answer's one section under the bar (the mockup's sheet of 222 px less its 90 px row).
inline constexpr float kQuickBody = 132.0f;
// The steps screen's body (the mockup's sheet of 738 px less its row).
inline constexpr float kStepsBody = 648.0f;
// The permission screen's body (612 px less the row).
inline constexpr float kPermissionBody = 522.0f;
// The change log's body (604 px less the row), its footer included.
inline constexpr float kLogBody = 514.0f;
// The system report's body (552 px less the row).
inline constexpr float kReportBody = 462.0f;
// The background (6_background), in the open sheet's strip: its cards kCardGap apart.
inline constexpr float kTasksWidth = 540.0f;
inline constexpr float kCardGap = 10.0f;
// Segoe Fluent Icons: the paperclip, a lock (phase 10, screen 17).
inline constexpr char32_t kIconAttach = 0xE723;
inline constexpr char32_t kIconRoute = 0xE8AB; // a request sent to another model than the one chosen
inline constexpr char32_t kIconLock = 0xE72E;
// The detailed mode (7_chat_terminal): 1400 wide (the monitor allows less), 780 tall.
inline constexpr float kTerminalWidth = 1400.0f;
inline constexpr float kTerminalBody = 690.0f;
inline constexpr float kDropGap = 16.0f;
// What a view leaves of its monitor: on each side and below (the mockup is drawn for 1920 x 1200;
// at 1920 x 1080 and 150 % a monitor is 1280 x 720 logical pixels).
inline constexpr float kSideRoom = 24.0f;
inline constexpr float kBottomRoom = 48.0f;

// The panel at rest in each view (logical pixels), no bigger than `room` allows.
auto shape_of(View v, Room room = {}) -> Shape;

// Room around the panel inside the window (left, right, bottom) for the resting handle's glow.
inline constexpr float kMargin = 48.0f;

// Why the window should draw (nothing moves while the agent does nothing).
//   None:    nothing changes.
//   Caret:   only the field's caret blinks: one frame at each flip (next_caret_flip); the orb
//            stands still (Windows' animations off).
//   OrbIdle: the open sheet's orb moves, smoothly (at the caret's flips it
//            jumped like a lag): the shell paces its frames, the orb alone on a kept frame.
//   Full:    the agent works or something animates (shape, layer, hover, a widget's motion).
enum class FrameNeed : std::uint8_t { None, Caret, OrbIdle, Full };

// `s` on one line if it fits `width`, otherwise cut at the last space that fits (the rest is the
// second line, which label() cuts with "…" if it is still too long). Views of `s`.
[[nodiscard]] auto two_lines(ui::Ui& ui, ui::Face face, float size, std::string_view s, float width)
    -> std::pair<std::string_view, std::string_view>;

// An edit's diff on its permission card (phase 12 § 1): `lines` from the preview's first hunk on,
// the first `shown` of them drawn and `hidden` more counted, the diff's own "… N more lines" tail
// included. No hunk: empty.
struct CardDiff {
    std::string_view lines;
    std::size_t shown = 0;
    std::size_t hidden = 0;
};
inline constexpr std::size_t kCardDiffLines = 12;
[[nodiscard]] auto card_diff(std::string_view preview) -> CardDiff;
template <class S>
    requires std::same_as<S, std::string>
auto card_diff(S&&) -> CardDiff = delete; // a temporary string: `lines` would point into it, dead

// A clickable place, by name ("send", "stop", "collapse", "field", "action:<id>"): for tests and,
// later, accessibility.
struct Target {
    std::string name;
    ui::layout::Box box;
};

class Painter {
public:
    Painter(ui::Ui& ui, ui::Fonts& fonts, ui::Orb& orb) : ui_(&ui), fonts_(&fonts), orb_(&orb) {}

    // Moves the animations to `now` towards the model's view and takes in new answer text;
    // returns this frame's panel shape.
    auto update(const Model& m, double now) -> Shape;
    // Renders the orb into its texture; binds the orb's framebuffer, so call it before the
    // frame's renderer.begin().
    void render_orb(double now);

    // Phase 9: the open sheet with nothing to do: only its orb moves. A full frame is drawn into a
    // kept target (`w` x `h` physical) between begin_keep (before the renderer begins) and
    // end_keep (after it ends) — the one draw of that frame, the sheet's orb left out of it (user,
    // 2026-10-15: a second, inert draw of it lost the hover and faded it out). Then, until
    // something changes or the caret is due, draw_kept puts that frame and the orb at `now` into
    // the bound framebuffer (the caller begins and ends the renderer); the orb moved on by
    // orb_step — update() and render_orb()'s part, before the renderer begins.
    void orb_step(double now);
    void begin_keep(int w, int h);
    void end_keep() noexcept;
    [[nodiscard]] auto draw_kept(double now) -> bool;
    void drop_kept() noexcept { kept_ready_ = false; }
    // A frame of the orb alone will do at `now`: a frame is kept and the caret does not flip
    // between the full frame at `last_full` and now.
    [[nodiscard]] auto orb_frame_due(double last_full, double now) const noexcept -> bool;
    // Where the last full frame drew its orbs (logical pixels).
    [[nodiscard]] auto orb_boxes() const -> std::vector<ui::layout::Box>;
    // Draws the panel in `panel` (logical pixels) between ui.begin_frame and ui.end_frame.
    auto draw(Model& m, const ui::layout::Box& panel) -> Reaction;
    // Esc (responsive text): it closes the right button's menu, else clears a selection, before it
    // reaches the sheet; true when it did.
    auto take_escape() -> bool;
    // Ctrl+C and Ctrl+A go to the field (final review I5): the last press was in it, or the user typed
    // since; a press on the sheet's text takes them back.
    [[nodiscard]] auto keys_to_field() const noexcept -> bool { return keys_to_field_; }
    void typed() noexcept { keys_to_field_ = true; }

    // The panel's shape or a layer crossfade moves (not the orb, not the caret).
    [[nodiscard]] auto animating() const noexcept -> bool;
    // What the window needs after the frame just drawn at `now` (FrameNeed).
    [[nodiscard]] auto frame_need(const Model& m, double now) const noexcept -> FrameNeed;
    // When the field's caret next turns on or off (seconds, the same clock as `now`).
    [[nodiscard]] auto next_caret_flip(double now) const noexcept -> double;
    // The window's open motion has ended in its closed look: the shell may switch the window to
    // its resting styles now, not before (phase 8: no flash while the panel leaves).
    [[nodiscard]] auto window_hidden() const noexcept -> bool;
    // The resting strip's width drawn in the last frame (0: not drawn) — for tests.
    [[nodiscard]] auto handle_drawn_width() const noexcept -> float { return handle_drawn_w_; }
    // design/ui_model: the bar's model chip as last drawn — its box, its words or only the dot and
    // bars (a narrow bar), a picture sending Pro's request to Flash — and the orb's look now.
    struct ModelChipShown {
        ui::layout::Box box;
        bool words = false;
        bool images = false;
        std::string_view name;     // the model's short name (ai::catalog)
        std::string_view level;    // "Off" … "Max": the level it is sent
        std::string_view monogram; // its provider's
        bool once = false;         // a choice for the next message only
    };
    [[nodiscard]] auto model_chip() const noexcept -> const ModelChipShown& { return model_chip_; }
    [[nodiscard]] auto look() const noexcept -> const ui::orb_look::Frame& { return look_frame_; }
    // The hint under the orb as drawn (MS § 7: in on quick, out on micro).
    [[nodiscard]] auto hint_opacity() const noexcept -> float { return hint_alpha_; }
    // A step's row as last drawn (MS 06): its spinner's share, its done circle, its check's stroke.
    struct StepMotion {
        float run = 0;
        float done = 0;
        float check = 0;
    };
    [[nodiscard]] auto step_motion(std::uint64_t id) const noexcept -> StepMotion;
    // The live block folding into its line when the work is done (MS 06).
    struct ActionsFold {
        float list = 1;         // the steps' rows
        float live_words = 1;   // "Working · step k of n"
        float folded_words = 0; // "3 actions · …"
    };
    [[nodiscard]] auto actions_fold() const noexcept -> ActionsFold { return actions_fold_; }
    // The conversation's scroll (pixels down from its top) and its viewport as last drawn.
    [[nodiscard]] auto conversation_scroll() const noexcept -> float { return conv_scroll_.offset; }
    [[nodiscard]] auto conversation_viewport() const noexcept -> ui::layout::Box { return conv_view_; }
    [[nodiscard]] auto conversation_content() const noexcept -> float { return conv_content_; }
    // What follows the answer (places, files, buttons, cost, hints) opening after its last character (MS 07).
    struct TailMotion {
        float share = 1; // of its height
        float alpha = 1;
    };
    [[nodiscard]] auto tail_motion() const noexcept -> TailMotion { return tail_motion_; }
    // The data panel changing its content (MS 09): the old one going aside, the new one coming.
    struct PanelMotion {
        float out = 0;       // the old content's opacity
        float out_shift = 0; // …and how far aside it is (px; left is under 0)
        float in = 1;        // the new content's opacity
        float in_shift = 0;
    };
    [[nodiscard]] auto panel_motion() const noexcept -> PanelMotion { return panel_motion_; }
    // Seconds of motion since the shown layer last changed (its crossfade's age).
    [[nodiscard]] auto layer_age() const noexcept -> double { return mnow_ - changed_at_; }
    // A folder of the tree opened or left (MS § 7): the list it left going, the new one coming.
    [[nodiscard]] auto space_motion() const noexcept -> PanelMotion { return space_motion_; }
    // The row as the work ends (MS 06): the rail going, "stop" turning back into the arrow.
    struct RowMotion {
        float rail = 0; // the rail's opacity
        float stop = 0; // how much of "stop" is still there (1: stop, 0: the arrow)
        float status = 1; // the status chip's new words over its old ones (MS 07: base)
    };
    [[nodiscard]] auto row_motion() const noexcept -> RowMotion { return row_motion_; }
    // Enter's scene this frame (MS 05; at rest between sends).
    [[nodiscard]] auto send_frame() const noexcept -> const scenes::SendFrame& { return send_frame_; }
    // The answer's cost line (design/ui_model): its badge ("Pro · Max", "Flash (images) · High") and
    // the line after it, as last drawn.
    [[nodiscard]] auto cost_text() const noexcept -> std::string_view { return cost_; } // the line under the answer, after its model
    // The small orb the answers' marks are drawn from (none: the bar's first frame).
    void set_card_orb(ui::Orb* orb) noexcept {
        card_orb_ = orb;
        avatar_stills_ = {}; // made again with it
    }
    // Windows' animation effects off: the window only fades (120 ms).
    void set_reduced_motion(bool on) noexcept;
    // Another theme (chosen in the tray, or Windows' mode changed under "as Windows"): it comes on
    // quick, each frame moving it; with Windows' animations off, at once. The theme it already goes
    // to is no new change.
    void change_theme(const ::theme::Theme& to) noexcept;
    // Where the shape is going (motion rework, task 5): the view's own shape with its strip and
    // attachments' rows, as update() last aimed. The shell's region takes it at a transition's start.
    [[nodiscard]] auto target_shape() const noexcept -> Shape { return target_; }
    // The fold's parts this frame (MS scenes 12, 13): the contents' opacities and the shape's spring.
    [[nodiscard]] auto fold_frame() const -> scenes::FoldFrame { return fold_.scene.frame(); }
    // MS § 7 "Threat check · Verdict", as drawn this frame: the block's room (0..1), its words'
    // opacity and lift (px), its three counts on their way from 0, a stage's mark's size (.9 → 1).
    struct ThreatMotion {
        float room = 1;
        float words = 1;
        float lift = 0;
        std::array<float, 3> counts{};
        float mark = 1;
    };
    [[nodiscard]] auto threat_motion() const noexcept -> const ThreatMotion& { return threat_motion_; }
    // MS § 7 "Translation": a translation card on its way in, as drawn this frame — its room (0..1)
    // and each of its blocks' opacity and lift (px), from the top.
    struct CardComing {
        float room = 1;
        std::array<float, 6> alpha{1, 1, 1, 1, 1, 1};
        std::array<float, 6> lift{};
    };
    [[nodiscard]] auto card_coming() const noexcept -> const CardComing& { return card_coming_; }
    // MS § 7 "Suggestions after an answer": the suggestion chips' opacities as drawn this frame, in order.
    [[nodiscard]] auto suggestions_in() const noexcept -> const std::array<float, 8>& { return suggest_in_; }
    // The next opening is the arrival after an install (docs/specs/motion-rework.md § 3.8): the
    // installer holds this very bar on the screen until ours is shown, so ours comes whole at once
    // — its glass and its orb — and only the row's content fades in.
    void arrive_next() noexcept { arrive_next_ = true; }
    // MS 11: how dim the screen around is now (0 clear .. 1) — the shell's dim window takes it —
    // and the question's scene as drawn this frame.
    [[nodiscard]] auto dim() const noexcept -> float { return approval_frame_.dim; }
    [[nodiscard]] auto approval_frame() const noexcept -> const scenes::ApprovalFrame& { return approval_frame_; }
    // The field's row (phase 10, screens 27-31): 90 px and 30 more a line of the field, at most
    // kFieldLines; its height now (logical px) — it moves in 120 ms, 180 when the field was emptied.
    static constexpr std::size_t kFieldLines = 6;
    static constexpr float kFieldLine = 30.0f;
    static constexpr float kFieldGutter = 12.0f; // past kFieldLines: room for the scrollbar
    static constexpr float kFieldBar = 5.0f;
    static constexpr float kEditorButton = 40.0f; // its ⤢ (.ib38: 40 x 40)
    static constexpr std::size_t kEditorLines = 15; // the editor (screen 29): lines before it scrolls
    static constexpr float kConversationShare = 0.85f; // the row grows a conversation's window to this much of the screen
    static constexpr std::size_t kQuestionLines = 4;  // a sent question shows this many, then Show more (screen 30)
    // A pasted text's card shows its start: at most this many bytes are shaped (final review I2: a log
    // of megabytes was shaped whole, and wrapped again every frame).
    static constexpr std::size_t kCardText = 4096;
    [[nodiscard]] auto paste_card_text() const noexcept -> std::string_view {
        return paste_para_ ? paste_para_->text() : std::string_view{};
    }
    // The question's Show more / Show less as last drawn (empty: a short question).
    [[nodiscard]] auto question_link() const noexcept -> std::string_view { return question_link_; }
    // What the key's row said in the last frame (the waiting line, "Checking…", or why not kept).
    [[nodiscard]] auto key_line() const noexcept -> std::string_view { return key_line_; }
    static constexpr float kEditorFoot = 56.0f;    // and its foot
    static constexpr float kNotice = 46.0f;        // a long paste's notice under the row (screen 31)
    [[nodiscard]] auto row_height() const noexcept -> float { return row_h_; }
    // The window's height for the shape update() returned: the row's larger height while it moves -
    // the shell resizes the window before a frame is shown, so a window resized every frame of the
    // row's 120 ms jerked (the user); it takes its new height once and the panel moves inside it.
    [[nodiscard]] auto window_height() const noexcept -> float { return window_h_; }
    // The field's lines as last laid out (physical px) and its width (logical px) — for tests.
    [[nodiscard]] auto field_lines() const noexcept -> std::span<const ui::text::Line>;
    [[nodiscard]] auto field_width() const noexcept -> float { return field_w_; }
    // Past kFieldLines (screen 28): pixels the text is scrolled up, "3 lines above" as last drawn
    // (empty: none), and the counter as last drawn ("468 chars"; empty: under 400).
    [[nodiscard]] auto field_scroll() const noexcept -> float { return field_scroll_; }
    [[nodiscard]] auto field_hint() const noexcept -> std::string_view { return {hint_buf_.data(), hint_len_}; }
    [[nodiscard]] auto counter() const noexcept -> std::string_view { return counter_; }
    // Up/Down in the bar's field: the caret to the line above or below at about the same x; false on
    // its first or last line (Up and Down are the history's there) or when the field does not take keys.
    auto caret_line(Model& m, bool down) -> bool;
    // Home/End: the caret to the start or end of its line as shown; false when the field does not take keys.
    auto caret_edge(Model& m, bool end) -> bool;
    // The caret's line and x (logical px) as drawn: at a wrap inside a word, End and Up/Down that land on
    // a line's end keep it there, not at the next line's start (final review I1).
    auto caret_at(const Model& m) -> std::pair<std::size_t, float>;
    // The drop's size now (logical px) — for tests.
    [[nodiscard]] auto orb_size() const noexcept -> float { return orb_size_; }
    // The panel's shape or a layer crossfade moves (full frame rate); otherwise only the orb and
    // the caret do, which 60 frames a second carry.
    [[nodiscard]] auto moving() const noexcept -> bool;
    // The orb is recreated when the sheet moves to a monitor with another scale.
    void set_orb(ui::Orb& orb) noexcept {
        orb_ = &orb;
        avatar_stills_ = {}; // taken again
    }
    // The monitor the sheet hangs on: views are cut to it (shape_of).
    void set_room(Room room) noexcept { room_ = room; }
    // The panel's drawn corners round at most this much (logical pixels): the open window's
    // own rounding by DWM, which also clips the backdrop.
    void set_corner_cap(float radius) noexcept { corner_cap_ = radius; }
    [[nodiscard]] auto targets() const noexcept -> std::span<const Target> { return {targets_.data(), target_count_}; }
    // The shapes that take the mouse while the sheet is closed: the sign's zone and its capsule;
    // the shell's window region is made of them. Empty in every other view.
    [[nodiscard]] auto solid() const noexcept -> std::span<const ui::layout::Box> { return {solid_.data(), solid_count_}; }
    // Bytes of the answer shown so far: the text is revealed letter by letter behind what has
    // arrived, a little each frame (smooth at the monitor's rate).
    [[nodiscard]] auto revealed() const noexcept -> std::size_t { return shown_bytes_; }
    // The answer's blocks as drawn (tests read their text).
    [[nodiscard]] auto answer_doc() const noexcept -> const answer::Doc* { return doc_ ? &*doc_ : nullptr; }
    // The Activity's output lines drawn in the last frame.
    [[nodiscard]] auto output_lines_drawn() const noexcept -> std::size_t { return output_lines_drawn_; }
    // The Activity tab's summary line as last drawn ("2 commands · 0.55 s · Read-only — …").
    [[nodiscard]] auto activity_summary() const noexcept -> std::string_view { return activity_summary_; }
    // The data panel's table as last drawn (its rows shown, its scroll); none: no panel.
    [[nodiscard]] auto panel_table() const noexcept -> const ui::work::TableState* { return panel_table_; }
    // The bar's status pill as last drawn ("Done · 0.6 s", "Step 2 of 3 · 0:01"); empty: none.
    [[nodiscard]] auto status() const noexcept -> std::string_view { return status_; }
    // The refusals' "Nothing broke." line as last drawn (phase 10).
    [[nodiscard]] auto safe_note() const noexcept -> std::string_view { return safe_note_; }

    // The done bar goes back to the field after this long without the mouse over it (the
    // mockup shows it 3.4 s; a little longer, for its Undo button).
    static constexpr double kDoneHold = std::chrono::duration<double>(ui::motion::hold::kDoneBar).count();

    // At rest the handle's white drifts along it and its glow breathes, once per kRestCycle
    // seconds (a very slow animation). rest_motion(): the shell should
    // draw resting frames at its low rate.
    // The theme the hidden agent's sign is drawn in (theme::sign_theme: Windows' own mode, as the
    // taskbar) — not the window's.
    void set_sign_theme(const ::theme::Theme& th) noexcept { sign_theme_ = &th; }
    // The shortcut that opens the sheet, as the capsule says it (the one actually registered).
    void set_shortcut(std::string_view keys) { shortcut_.assign(keys); }
    void set_rest_motion(bool on) noexcept { rest_motion_on_ = on; }
    // Stage 4d-2: the agent paused (the tray's Pause agent): its hidden sign fades to a third.
    void set_paused(bool on) noexcept { paused_ = on; }
    // The resting strip's drifting light is gone with the strip (UI update, stage 3): the handle
    // stands still, so the shell's resting tick never has a frame due. (The setting and these
    // two go in the stage's task 7.)
    [[nodiscard]] auto rest_frame_due(double now) const noexcept -> bool;
    [[nodiscard]] auto rest_motion() const noexcept -> bool;

private:
    void draw_layer(View v, Model& m, const ui::layout::Box& panel, Reaction& r);
    void draw_input(Model& m, const ui::layout::Box& panel, Reaction& r);
    void draw_thinking(Model& m, const ui::layout::Box& panel, Reaction& r);
    void draw_done(Model& m, const ui::layout::Box& panel, Reaction& r);
    void draw_answer(Model& m, const ui::layout::Box& panel, Reaction& r);
    // The policy's question: its facts as text (permission_text), Allow / the folder for the
    // session / Deny; the word's field in advanced mode.
    void draw_permission(Model& m, const ui::layout::Box& panel, Reaction& r);
    // A one-line text field: the text at `size` (or the placeholder at `placeholder_size`) and the
    // caret, scrolled to keep the caret in view; `box` is the text area.
    // The bytes `sel` of `line` lit behind its glyphs, the line's top at `y`.
    void field_selection(const ui::text::Paragraph& para, const ui::text::Line& line, std::pair<std::size_t, std::size_t> sel, float x,
                         float y);
    void draw_field_text(const Edit& e, std::string_view placeholder, float placeholder_size, float size,
                         const ui::layout::Box& box, bool bright = false);
    // The top row of every open view (.row): the field with its placeholder and, right-aligned,
    // the chips, the collapse button and the round send or stop button.
    struct RowChip {
        std::string_view label;
        ui::work::Tone tone;
        std::string_view target; // its name in targets()
    };
    enum class RowButton : std::uint8_t { Send, Stop, None };
    // `bright`: the placeholder looks typed (a quick answer's row shows its question). With
    // RowButton::None, `reserve` pixels at the right are left to the caller's own controls.
    void draw_row(Model& m, const ui::layout::Box& panel, Reaction& r, std::string_view placeholder,
                  float placeholder_size, std::span<const RowChip> chips, RowButton button, bool collapse,
                  bool bright = false, float reserve = 0, bool expand = false);
    // design/ui_model: the bar's model chip with its right edge at `right` (its tooltip, its click:
    // the picker); returns its left edge. The line of the model's colour along a bar's bottom edge,
    // the orb's click and wheel, the hint under the bar and the ring of a model change.
    auto draw_model_chip(Model& m, float right, float cy, float row_w) -> float;
    void draw_model_edge(const Model& m, const ui::layout::Box& panel);
    void draw_orb_controls(Model& m, const ui::layout::Box& orb, Reaction& r);
    void draw_model_hint(const Model& m, const ui::layout::Box& glass);
    [[nodiscard]] auto hint_shown(const Model& m) const noexcept -> bool;
    // The detailed mode (sheet_terminal.cpp): the conversation and the plan on the left, the
    // agent's terminal on the right with its tabs, Take over, its foot.
    void draw_terminal(Model& m, const ui::layout::Box& panel, Reaction& r);
    // Its parts (phase 8: the plan in the turn, the terminal in the panel): the plan card's
    // height; the processes' tabs, Take over, the output, its foot.
    auto draw_plan(Model& m, float x, float y, float w) -> float;
    void draw_terminal_pane(Model& m, const ui::layout::Box& pane, Reaction& r);
    float terminal_scroll_ = 0;    // lines scrolled back from the newest
    std::uint64_t task_seen_ = 0;  // the task in front, and when it was first drawn ("Working · 2:14")
    double task_seen_at_ = 0;
    // The change log (sheet_log.cpp): the tasks on the left, the picked one's changes as a tree
    // with an Undo per line, the footer's "Undo last".
    void draw_log(Model& m, const ui::layout::Box& panel, Reaction& r);
    // The system report (sheet_report.cpp): the question and the sentence, four tiles on the
    // left; what uses memory and what can be done on the right.
    void draw_report(Model& m, const ui::layout::Box& panel, Reaction& r);
    // The screen question (sheet_screen.cpp, phase 10, screen 19): its card, its panel.
    auto screen_card(Model& m, float x, float y0, float w, bool measure, Reaction& r) -> float;
    void draw_capture_panel(Model& m, const ui::layout::Box& panel, Reaction& r);
    bool capture_menu_ = false; // the windows to take instead, listed
    // A PDF in the panel (sheet_document.cpp, phase 10, screen 22): its page map, parts, page text.
    void draw_document(Model& m, const events::DataSet& set, const ui::layout::Box& body, Reaction& r);
    // What the tools refused, in the turn (phase 10, screen 25): "Nothing broke." and a card each;
    // a thumbnail whose file is not a picture by its name becomes one.
    auto draw_refusals(Model& m, float x, float y0, float w, bool measure) -> float;
    // "Looked at" over the answer (phase 13a § 2, design/ui_system 03a-03c): the window read, a
    // picture of it, or a sensitive one's lock and why; its height.
    auto draw_looked(Model& m, float x, float y0, float w, bool measure) -> float;
    // Stage 4c (sheet_route.cpp, screens 05 and 06): the blue notes of where the turn went instead,
    // over its answer; the divider of a model the user changed to; a provider's failure as its card
    // with the turn's buttons in it. Each returns its height.
    auto draw_routes(std::span<const std::string> routes, float x, float y0, float w, bool measure) -> float;
    auto draw_switched(std::string_view name, float x, float y0, float w, bool measure) -> float;
    // Stage 4d-1 (sheet_settings.cpp): the settings mode — the header, the sections' column, a section;
    // its shape: 1240 wide, as tall as the section (measured as it is drawn), never past the room.
    void draw_settings(Model& m, const ui::layout::Box& panel, Reaction& r);
    // Stage 4d-2 (sheet_settings_models.cpp): Models — the provider's card, its key's field and models,
    // the month's spending; Roles — the three roles and their models. Each returns its height.
    auto draw_models_section(Model& m, float x, float y0, float w, Reaction& r) -> float;
    auto draw_roles_section(Model& m, float x, float y0, float w, Reaction& r) -> float;
    bool paused_ = false; // stage 4d-2: the sign at a third
    std::array<ui::layout::Box, 3> settings_role_boxes_{}; // Roles' model boxes: the menu opens under the one asked
    [[nodiscard]] auto settings_shape(const Model& m) const -> Shape;
    float settings_content_ = 0;        // the section's height under the header, as last drawn
    Section settings_shown_ = Section::General; // the section drawn last: a change brings the new one in
    int settings_open_item_ = -1;       // the item whose list is open (a dropdown), -1: none
    ui::work::Scroll settings_scroll_;  // the section's own scroll: the header and the column stay
    auto draw_failure_card(Model& m, float x, float y0, float w, bool measure, Reaction& r) -> float;
    // The files the turn made (phase 10, screen 24): a card each; their Preview in the panel, the
    // document on a white page (the answer's Markdown drawn with paper_ ink).
    auto draw_created(Model& m, float x, float y0, float w, bool measure, Reaction& r) -> float;
    // The files and folders the answer names (phase 10, after a hand check): a row each, once it
    // is written — Open, Show in folder, Copy path ("Copied" a moment after).
    auto draw_places(Model& m, float x, float y0, float w, bool measure, Reaction& r) -> float;
    std::size_t place_copied_ = static_cast<std::size_t>(-1);
    double place_copied_at_ = -1.0e9;
    void draw_preview(Model& m, const events::DataSet& set, const ui::layout::Box& body, Reaction& r);
    std::optional<answer::Doc> preview_doc_;
    ui::work::Scroll preview_scroll_;     // its page, scrolled in its frame
    std::uint32_t preview_scroll_set_ = 0; // the Preview it is for: another starts at its top
    bool paper_ = false; // draw_markdown: dark ink on a white page
    bool countdown_warm_ = false; // the preview's "hides in N s": its digits shaped and drawn once
    void refuse_picture(Model& m, Reaction& r);
    std::string safe_note_;
    // A picture's text and converted pictures in the panel (sheet_tools.cpp, phase 10, screens 20-21).
    void draw_text_read(const events::DataSet& set, const ui::layout::Box& body, Reaction& r);
    void draw_converted(const events::DataSet& set, const ui::layout::Box& body, Reaction& r);
    // What takes the space (sheet_space.cpp, phase 13a, 02b–02c): a drive's bar with its names, the
    // way back with Folders | Largest files and Show in Explorer, from `y`. Returns where the table
    // goes, or a negative number when the set was made again (the next frame draws it).
    auto draw_space_head(Model& m, const events::DataSet& set, const ui::layout::Box& body, float y, Reaction& r) -> float;
    // A step of the tree: the set made again at `path`, its table from the top.
    void space_step(Model& m, std::uint32_t set, std::vector<std::uint32_t> path, bool files);
    // The threat report (sheet_threat.cpp, phase 13a, 05b–05g): its verdict and counts, the findings
    // by level (one opened at a time: its reasons, facts, actions), what was checked.
    void draw_threat(Model& m, const events::DataSet& set, const ui::layout::Box& body, Reaction& r);
    ui::work::Scroll threat_scroll_;
    int threat_open_ = -1;          // the finding opened; -1: none
    std::uint32_t threat_set_ = 0;  // the report it belongs to
    double threat_shown_at_ = -1.0e9; // when this report was first drawn: its verdict comes from then (MS § 7)
    ThreatMotion threat_motion_{};

    // MS 10: what opens or closes under its row (a finding, an earlier turn) — a scene each, made
    // at the click and let go at rest; a thing with no scene is at rest as it is.
    struct Disclosure {
        std::uint64_t key = 0;
        scenes::Accordion scene;
    };
    std::vector<Disclosure> disclosures_;
    double disclosures_last_ = -1.0e9;
    // At the click: `key` goes to `open` from the other state (or from where it is on its way).
    void disclose(std::uint64_t key, bool open);
    // This frame's look of `key`, which is `open` when nothing moves it.
    [[nodiscard]] auto disclosed(std::uint64_t key, bool open) const -> scenes::AccordionFrame;
    // Steps every scene to `now`; those at rest are let go.
    void step_disclosures(double now);
    int ocr_lit_ = -1;        // the line lit, by its box or its row; -1: none (the frame's, from the mouse)
    std::string ocr_text_;    // the lines, joined: Copy and .txt
    void draw_step_row(const events::Step& s, const ui::layout::Box& row, std::string_view prefix);
    // The model's plan (phase 12 § 3) over the steps: a row per item; its height.
    auto draw_checklist(const events::Checklist& c, float x, float y, float w, bool measure) -> float;
    // The model's question (phase 12 § 4): its options, the recommended first; Other, Skip; its height.
    auto draw_question_card(Model& m, float x, float y, float w, bool measure, Reaction& r) -> float;
    [[nodiscard]] auto drop_height(const Drop& d, float width) -> float;
    // `s` on one line if it fits, else cut at the last space that fits (the rest on a second line,
    // cut with "…" by the label).
    auto wrap2(ui::Face face, float size, std::string_view s, float width) -> std::pair<std::string_view, std::string_view>;
    // The background's rows in the open sheet's strip (sheet_background.cpp): the tasks still listed
    // (Pause, Stop), the watchers (their switch).
    void draw_task_rows(Model& m, const ui::layout::Box& rows, Reaction& r);
    // The window folding into the handle, or coming out of it (scenes::Fold): the two ends — the
    // window's shape and orb, the handle's — and the scene whose morph spring puts the shape
    // between them; while it lasts it is the window's motion and its drawing. Turned round
    // mid-way, the spring goes back from where it is.
    struct Folding {
        Shape window{};
        float window_orb = 0;
        Shape handle{};
        float handle_orb = 0;
        scenes::Fold scene;
        double last = -1.0e9; // the scene stepped to here
        bool unfold = false;
    };
    Folding fold_{};
    bool folding_ = false;
    void draw_fold(Model& m, const ui::layout::Box& panel);
    // Cards from `y` in a column `width` wide (questions first, `max` of them, "+N more"): where they end.
    auto draw_cards(Model& m, float x, float y, float width, std::size_t max, Reaction& r) -> float;
    [[nodiscard]] auto cards_height(const Model& m, float width, std::size_t max) -> float;
    // The sheet open with something in the background: its strip under the view's content.
    [[nodiscard]] auto strip_wanted(const Model& m, View v) const -> bool;
    [[nodiscard]] auto strip_height(const Model& m) -> float;
    void draw_strip(Model& m, const ui::layout::Box& strip, Reaction& r);
    float orb_x_ = 0, orb_y_ = 0; // the orb's centre in the panel when a view puts it elsewhere
    float log_scroll_ = 0;  // the task list, pixels down
    float tasks_scroll_ = 0; // the background list's rows, pixels down
    float tree_scroll_ = 0; // the tree
    std::uint64_t tree_task_ = 0; // the tree shown: a new one starts at its top
    // A short answer in the bar: the value, the bar of parts with its legend, one action.
    void draw_quick(Model& m, const ui::layout::Box& panel, Reaction& r);
    // The steps screen (sheet_steps.cpp): the question, the agent's words, the step cards, the
    // summary beside them.
    void draw_steps(Model& m, const ui::layout::Box& panel, Reaction& r);
    // One step card at `y` (its whole width `w`); returns its height. `measure`: nothing drawn.
    auto draw_step(const events::Step& s, float x, float y, float w, bool measure) -> float;
    void draw_summary(const events::Summary& s, const ui::layout::Box& column);
    [[nodiscard]] auto step_open(const events::Step& s) const -> bool;
    void sync_answer(const Model& m);
    // The answer's blocks (sheet_markdown.cpp) from the top of `box`, drawn where `visible` shows
    // them; `caret` after the last block's text. A web link clicked goes to r.open_url.
    void draw_markdown(const ui::layout::Box& box, const ui::layout::Box& visible, bool caret, Reaction& r);
    // Any answer's blocks (an earlier turn's has no times: all shown).
    void draw_markdown(const answer::Doc& doc, std::span<const ui::widgets::StreamTimes> times, const ui::layout::Box& box,
                       const ui::layout::Box& visible, bool caret, Reaction& r);
    std::size_t links_drawn_ = 0; // the frame's links, numbered across its answers ("md:link:N")
    std::size_t tables_drawn_ = 0; // the frame's table cards (a table in the panel), numbered the same way
    std::optional<std::size_t> table_pick_; // the card clicked this frame, by that number
    const answer::Block* table_picked_ = nullptr; // and its block (the frame's doc holds it)
    // A card of the answer just drawn (its numbers from `first`) clicked: the answer's set it
    // stands for, among `data`, to the panel — or, before that set has come (the answer still
    // written, stopped, cut short), a set made from the card's own rows: a card always opens its
    // table ("if there is a button, it does something").
    void open_answer_table(Model& m, const std::vector<std::uint32_t>& data, std::size_t first);
    static constexpr std::uint32_t kCardTable = 0xFFFFFF00u; // that set's id: past the agent's and the shell's
    void draw_table_card(const answer::Block& b, const ui::layout::Box& card, std::size_t n);
    ui::widgets::StreamTimes shown_times_; // no times: a block all shown

    // Phase 8 (sheet_conversation.cpp): the conversation — the bar with its status, the column's
    // header (History, New chat), the turn in front (its question, actions, answer, data cards,
    // chips), the earlier turns folded under it — one column 860 wide (content at most 720).
    [[nodiscard]] auto conversation_shown(const Model& m, View v) const -> bool;
    [[nodiscard]] auto conversation_shape(const Model& m) const -> Shape;
    void draw_conversation(Model& m, const ui::layout::Box& panel, Reaction& r);
    void draw_conversation_row(Model& m, const ui::layout::Box& panel, Reaction& r);
    // The column under the bar; `measure`: only its height.
    auto draw_column(Model& m, const ui::layout::Box& column, Reaction& r) -> void;
    auto draw_turn(Model& m, float x, float y, float w, const ui::layout::Box& visible, bool measure, Reaction& r) -> float;
    // What the model was shown (phase 9): up to 3 thumbnails in a row, "+N" past them; a click
    // opens the picture's file. Returns the row's height (0: none).
    // Phase 10 (screen 18): the user's own pictures in their message, 120 x 84 with their names
    // (returns the row's height); what the model looked at, 26 x 26 from x, centred on cy, 4 apart
    // (returns their width) — under the mouse a ring, a magnifier, its name and size, a click opens
    // a picture's file.
    auto draw_mine(std::span<const events::Picture> pictures, float x, float y, float w, bool measure, Reaction& r) -> float;
    auto draw_seen(std::span<const events::Picture> pictures, float x, float cy, bool measure, Reaction& r) -> float;
    [[nodiscard]] static auto seen_width(std::span<const events::Picture> pictures) -> float;
    // A thumbnail's texture, made the first time it is drawn; the 24 drawn last are kept.
    auto picture_texture(const events::Picture& p) -> const ui::render::Texture*;
    // The attachments as chips from `x` in the input row (phase 9), up to `limit`; returns where
    // the field may start.
    // The attachments' row over the line (phase 10, screen 17): 64 high while there are any in a
    // view with the field, else 0.
    [[nodiscard]] auto attach_row(const Model& m, View v) const -> float;
    void draw_attach_row(Model& m, const ui::layout::Box& row);
    // Files dragged over the window: "Release to attach" over the glass.
    void draw_drop_zone(const ui::layout::Box& glass);
    float attach_scroll_ = 0; // the attachments' row, pixels to the right
    // § 7 "Attachment chip": when each chip was first seen (its content comes at +60 ms), a few at a
    // time, no allocation; the chip just removed, copied: it fades on micro, then gives its width
    // back on smooth, the chips after it sliding along.
    std::array<std::pair<std::uint64_t, double>, 32> chip_seen_{};
    std::size_t chip_seen_next_ = 0;
    std::optional<Attachment> going_chip_;
    std::size_t going_chip_index_ = 0;
    float going_chip_w_ = 0;
    double going_chip_at_ = -1.0e9;
    struct PictureTexture {
        std::uint64_t picture = 0; // the events::Picture::id it was made from
        ui::render::Texture texture;
        std::uint64_t drawn = 0; // the frame it was last drawn in
    };
    std::vector<PictureTexture> picture_textures_;
    std::uint64_t picture_frame_ = 0;
    // The orbs a frame drew (at most 4: the bar's, the agent's marks), for the kept frames.
    struct OrbDraw {
        ui::layout::Box box;
        float opacity = 1;
    };
    void orb_image(const ui::layout::Box& box);
    // The right button's menu over the sheet: Copy, Select all.
    void draw_context_menu(const ui::layout::Box& panel, Reaction& r);
    bool menu_open_ = false;
    ui::render::Point menu_at_{};
    std::string menu_text_; // Copy's text, its room kept
    std::array<OrbDraw, 4> orb_draws_{};
    std::size_t orb_count_ = 0;
    bool hide_orbs_ = false;
    // The agent's mark beside its answers: a still of the orb, drawn as any picture of the frame
    // (under what covers it, in kept frames) — never the live orb again.
    std::array<std::optional<ui::render::Target>, 2> avatar_stills_; // the answers' mark: Flash's, Pro's
    ui::Orb* card_orb_ = nullptr; // the small orb the marks are made from
    // The theme whose light the stills were made in (its name; the built-in themes' names live for
    // the process): another one, once its change has ended, makes them again.
    std::string_view stills_theme_;
    void avatar_image(const ui::layout::Box& box, int model); // a model's place in the catalog
    std::optional<ui::render::Target> kept_;
    std::array<OrbDraw, 4> kept_orbs_{};
    std::size_t kept_orb_count_ = 0;
    float kept_scale_ = 1;
    bool kept_ready_ = false;
    auto draw_earlier(Model& m, float x, float y, float w, const ui::layout::Box& visible, bool measure, Reaction& r) -> float;
    void draw_history(Model& m, const ui::layout::Box& column, Reaction& r);
    auto earlier_doc(const Turn& t, float width) -> answer::Doc&;
    std::string status_;              // the bar's pill, its buffer kept
    std::string joined_;              // the actions line's step names, its buffer kept
    std::string cost_;                // "$0.0004 · 92% cached", made when the cost changes
    std::string cost_model_;               // the model the line was made for
    double cost_usd_ = -1;
    ui::work::Scroll conv_scroll_;
    float conv_content_ = 0;          // the column's content height as last drawn
    bool redraw_due_ = false;         // draw changed what the next frame shows (a height, a tab)
    // The model picker's part `index` (the cards 0, then what is under them): what follows is
    // drawn at its opacity over cascade_base_ (WindowMotion::part).
    void cascade(int index);
    float cascade_base_ = 1.0f; // the renderer's opacity before the conversation
    bool conv_shown_ = false;         // the last update showed the conversation
    bool conv_follow_ = true;          // keep the running step in view, until the wheel moves
    // MS 07: the answer followed while its turn is live; scrolled up, "New" says there is more.
    bool conv_new_ = false;
    TailMotion tail_motion_{};
    // MS 09: what the panel showed last (a set's id, or its turn's Activity) and where its tab was;
    // what it showed before that, while it goes.
    PanelMotion panel_motion_{};
    // MS § 7 "Into a folder": the rows and the table's state it left (copied at the click), the set
    // they were of, which way it went (+1: deeper) and where the table was last drawn.
    PanelMotion space_motion_{.out = 0, .out_shift = 0, .in = 1, .in_shift = 0};
    // This frame's motion of the step (at rest unless `set` is the one that stepped).
    auto space_stepping(const events::DataSet& set) -> PanelMotion;
    // The list it left, drawn in `table` as a picture.
    void draw_space_left(const events::DataSet& set, const ui::layout::Box& table, float opacity);
    ui::work::DataRows space_old_rows_;
    ui::work::TableState space_old_state_;
    std::uint32_t space_old_set_ = 0;
    float space_way_ = 1;
    ui::layout::Box space_table_box_{};
    std::uint64_t panel_key_ = 0;      // 0: nothing yet
    std::uint64_t panel_old_key_ = 0;
    int panel_tab_ = 0;
    float panel_way_ = 1;              // +1: to a tab on the right (the old goes left)
    double panel_changed_at_ = -1.0e9;
    // "Remembered" lines (MS § 7): when each was first seen (its green's 4 s), a few, no allocation;
    // the one Undo took, its words copied: it goes, then its room closes.
    std::array<std::pair<std::uint64_t, double>, 8> notes_seen_{};
    std::size_t notes_seen_next_ = 0;
    std::array<char, 256> gone_note_{};
    std::size_t gone_note_n_ = 0;
    std::size_t gone_note_place_ = 0;
    double gone_note_at_ = -1.0e9;
    double tail_at_ = -1.0e9;   // when the answer was first all shown and ended (under -1e8: not yet)
    float tail_full_ = 0;       // its whole height, as this frame measured it
    // The status chip's last words and tone (copied: no allocation), going out as the new come.
    std::array<char, 64> status_old_{};
    std::size_t status_old_n_ = 0;
    std::array<char, 64> status_last_{};
    std::size_t status_last_n_ = 0;
    ui::work::Tone status_tone_ = ui::work::Tone::Plain;
    ui::work::Tone status_old_tone_ = ui::work::Tone::Plain;
    bool status_seen_ = false;
    double conv_live_at_ = -1.0e9;
    float answer_bottom_ = -1.0e9f;    // the answer's bottom this frame (none: far above)
    std::uint64_t answer_h_task_ = 0;  // the task whose answer's height the spring holds
    std::uint64_t follow_task_ = 0;
    bool history_open_ = false;
    ui::layout::Box history_button_{}; // a press there is the button's, not "elsewhere"
    std::uint64_t turn_task_ = 0;     // the turn in front, and when it was first seen (its "0:01")
    double turn_seen_at_ = 0;
    struct EarlierDoc {
        std::uint64_t task = 0;
        answer::Doc doc;
    };
    std::vector<EarlierDoc> earlier_docs_; // the unfolded earlier turns' answers

    // Phase 8 (sheet_panel.cpp): the data panel right of the conversation — the tabs of the
    // shown turn's sets, the set's header with its tools (filter, Copy, CSV, Explorer), its
    // tiles, its table, its footer. Two columns 1240 wide (470 + 1 + 769); a narrow monitor
    // keeps the conversation at 360 and the panel at 480 at least.
    [[nodiscard]] auto panel_set(const Model& m) const -> const events::DataSet*;
    // The conversation's width in a window `width` wide with the panel.
    [[nodiscard]] static auto conversation_width(float width) -> float;
    void draw_panel(Model& m, const ui::layout::Box& panel, Reaction& r);
    auto table_state(std::uint32_t set) -> ui::work::TableState&;
    std::vector<std::pair<std::uint32_t, ui::work::TableState>> tables_; // per data set
    const ui::work::TableState* panel_table_ = nullptr;
    std::string panel_text_; // Copy's and CSV's text, made on the click
    float panel_content_ = 0; // a question's panel: how tall its list and levels are
    bool tabs_more_open_ = false; // the panel's "N more": its list of the tabs with no room
    // Phase 8 (sheet_permission.cpp): a question in two columns — its card in the turn, its
    // objects with their ticks and why it asks in the panel.
    // `going`: the question was answered (Model::answered) — its card shrinking into its summary
    // row, its panel's rows going; nothing of them takes a click (MS 11).
    auto approval_card(Model& m, const events::Permission& p, bool going, float x, float y, float w, bool measure, Reaction& r) -> float;
    void draw_approval_panel(Model& m, const events::Permission& p, bool going, const ui::layout::Box& panel, Reaction& r);
    std::array<float, 8> suggest_in_{}; // MS § 7: each suggestion chip's opacity on its way in, as drawn
    // MS § 7 "Translation": which card is shown (what it says, of which task) and when it came; this
    // frame's look of it (coming(): a new key starts it).
    std::uint64_t card_key_ = 0;
    double card_came_at_ = -1.0e9;
    CardComing card_coming_{};
    auto coming(std::uint64_t key) -> CardComing;
    // MS § 7 "Looked at": which task's lines these are, how many of them were seen, when each came
    // (long ago: it was there when the turn was first drawn).
    std::uint64_t looked_task_ = 0;
    std::size_t looked_seen_ = 0;
    std::array<double, 8> looked_at_{};
    // MS 14: a thing's light under the mouse, 0..1 on quick both ways, by its id.
    [[nodiscard]] auto lit(std::uint64_t id, bool hovered) -> float { return std::clamp(ui_->hover(id, hovered), 0.0f, 1.0f); }
    // MS 11 (app::scenes::Approval): the question asked, then answered. `asked_id_`: the question
    // the scene is of (its id + 1; 0 none); `asked_answered_`: the scene runs its answer.
    scenes::Approval approval_;
    scenes::ApprovalFrame approval_frame_{};
    double approval_last_ = -1.0e9;
    std::uint64_t asked_id_ = 0;
    bool asked_answered_ = false;
    void update_approval(const Model& m, double now);
    // The answered question's panel is still shown: its card shrinks, its rows go (MS 11).
    [[nodiscard]] auto approval_going(const Model& m) const -> bool;
    // This frame's look of a question's card: on its way by the scene, or at rest as it is.
    [[nodiscard]] auto card_look(const events::Permission& p, bool going) const -> scenes::ApprovalFrame;
    // A card's text in full (responsive text, 2026-10-29): wrapped at `at.w`, each line a target
    // "<prefix><fact>:<from>:<to>" naming its part of the text. Returns its height.
    auto card_text(std::string_view prefix, std::size_t fact, const ui::layout::Box& at, std::string_view text,
                   const ui::work::TextBlockStyle& style) -> float;
    // A card taller than its room: all but its foot (the decision buttons) scrolls in it, the foot
    // stays under (the spec: the buttons always seen and enabled). `natural`: its whole height.
    struct CardFrame {
        ui::layout::Box shown;   // the card as drawn (its background, its target)
        float content_y = 0;     // where its content starts, scrolled
        float foot_y = 0;        // where its foot starts
        bool scrolled = false;
        ui::layout::Box view{};  // the scrolled part's viewport
    };
    auto card_frame(std::uint64_t id, float x, float y, float w, float natural, float foot, float room) -> CardFrame;
    // After its background: the scrolled part begins (content_y set) and ends (its fade, its bar).
    void open_card(CardFrame& frame, float natural, float foot);
    void close_card(const CardFrame& frame, ui::render::Corners corners);
    ui::work::Scroll card_scroll_;     // a card's text past its room
    std::uint64_t card_scrolled_ = 0;  // the question it belongs to
    float card_room_ = 1.0e9f;         // the conversation's height: a card is never taller
    ui::layout::Box conv_view_{};      // the conversation's viewport this frame (its card comes into it)
    // Phase 8 (sheet_translate.cpp): the translation cards — a word or a phrase in the turn (920
    // wide), a text's pairs and terms in the panel.
    void ask_again(Model& m, std::string_view request, Reaction& r);
    auto card_header(Model& m, std::string_view source, std::string_view target, std::string_view pill, std::string_view swap_text,
                     float x, float y, float w, Reaction& r) -> float;
    auto examples_block(Model& m, std::string_view title, std::span<const events::Example> examples, float x, float y, float w,
                        bool measure) -> float;
    auto card_footer(Model& m, std::string_view copy_label, std::string_view copy, std::string_view with, bool shorter, float x,
                     float y, float w, Reaction& r) -> float;
    auto draw_word_card(Model& m, float x, float y, float w, bool measure, Reaction& r) -> float;
    auto draw_phrase_card(Model& m, float x, float y, float w, bool measure, Reaction& r) -> float;
    auto draw_text_turn(Model& m, float x, float y, float w, bool measure, Reaction& r) -> float;
    void draw_text_panel(Model& m, const ui::layout::Box& panel, Reaction& r);
    void sync_pairs(const events::TextTranslation& t, float original_w, float translation_w);
    [[nodiscard]] static auto translation_width(const Model& m) -> float;
    std::string tr_request_; // a card's next request, its buffer kept
    std::string tr_with_;    // "学习 (xuéxí)"
    int tr_tab_ = 0;         // the text's panel: 0 Translation, 1 Terms
    struct Pair {
        ui::text::Paragraph original;
        ui::text::Paragraph translation;
    };
    std::vector<Pair> pairs_; // the text's sentences, wrapped once
    std::uint64_t pairs_key_ = 0;
    // The panel is there: a data set, or a turn's Activity, to show.
    [[nodiscard]] auto panel_shown(const Model& m) const -> bool;
    // The system report's programs as a set of the panel (made again when the report changes).
    [[nodiscard]] auto has_report(const Model& m) const -> bool;
    auto report_set(const Model& m) -> const events::DataSet&;
    void draw_set_body(Model& m, const events::DataSet& set, const ui::layout::Box& body, Reaction& r);
    static constexpr std::uint32_t kReportSet = 0xFFFFFFFFu;
    events::DataSet report_set_;
    std::uint64_t report_key_ = 0;
    // MS § 7: which of the monitor's readings the tiles show and when it came — their lines slide
    // through its second.
    std::size_t report_readings_ = 0;
    double report_reading_at_ = -1.0e9;
    // The Activity tab (sheet_activity.cpp): the summary, the failed calls first, each with its
    // command and its output (folded; twelve lines; Show all).
    void draw_activity(Model& m, std::span<const events::ActivityEntry> entries, const ui::layout::Box& box, Reaction& r);
    auto activity_entry(const events::ActivityEntry& e, std::uint64_t task, float x, float y, float w,
                        const ui::layout::Box& visible, bool measure) -> float;
    auto header_pills(const events::ActivityEntry& e) -> float;
    std::uint32_t open_set_ = 0; // a step's "Open table" pressed this frame: its set
    std::string activity_summary_;
    std::size_t output_lines_drawn_ = 0; // this frame's output lines (only those in view)
    ui::work::Scroll activity_scroll_;
    ui::work::Scroll approval_scroll_;    // a question's panel: its objects, then why it asks
    std::uint64_t approval_scrolled_ = 0; // the question it belongs to
    std::uint64_t activity_scroll_task_ = 0;
    std::vector<std::uint32_t> activity_order_;
    struct Output {
        std::uint64_t task = 0;
        int number = 0;
        std::size_t size = 0;              // the output's bytes when its lines were found
        std::vector<std::uint32_t> starts; // each line's first byte
        bool open = false;
        bool all = false;
    };
    std::vector<Output> outputs_;
    auto output_of(std::uint64_t task, const events::ActivityEntry& e) -> Output&;
    void draw_table(const answer::Block& b, const ui::layout::Box& table, const ui::layout::Box& visible,
                    const ui::widgets::StreamTimes& times);
    void add_target(std::string_view name, const ui::layout::Box& box);
    // "action:<id>" — an action button's name, for its target and its id. Written into one string
    // kept between frames: a long id costs no allocation a frame. Good until the next call.
    auto action_name(std::string_view id) -> std::string_view;
    std::string action_name_;

    ui::Ui* ui_;
    ui::Fonts* fonts_;
    ui::Orb* orb_;
    ui::anim::Animator anim_; // the panel's shape and the orb's size
    Shape shape_;
    Shape target_; // target_shape()
    float orb_size_ = 0;
    float corner_cap_ = 1.0e9f;
    Room room_;
    double now_ = 0;
    View shown_ = View::Rest;
    View previous_ = View::Rest;
    double changed_at_ = -1.0e9; // the first view shows without a transition (motion clock)
    // The time of motion drawn by its age (MS § 5.1): the frames' time, a stall moving it a frame's
    // limit — the layer's change, the strip after a close, the window's motion and its parts, a
    // panel's change, the handle's words, a looked-at row are stamped and aged on it.
    ui::motion::Clock clock_;
    double mnow_ = 0;
    bool first_ = true;

    // The answer card.
    std::optional<answer::Doc> doc_;
    float doc_scale_ = 0;
    std::vector<ui::widgets::StreamTimes> times_; // per block: when its text arrived
    std::vector<std::size_t> block_sizes_;
    std::optional<ui::text::Paragraph> question_;
    std::string question_text_;
    float question_scale_ = 0;
    float scroll_ = 0; // pixels scrolled back from the bottom of the answer
    // The answer's height as shown: follows the real height smoothly, so a new line lifts the
    // text over ~0.2 s instead of jumping (a jump read as text that did not come out smoothly).
    float shown_content_ = 0;
    ui::motion::Spring shown_content_spring_{}; // its height's way on quick (its value is shown_content_)
    float content_ = 0;
    double last_draw_ = 0;
    double last_token_at_ = -1.0e9;
    std::size_t answer_seen_ = 0;
    scenes::Stream stream_;           // the answer's smoother (MS § 6)
    std::uint64_t stream_task_ = 0;   // the task it is of
    std::size_t shown_bytes_ = 0;
    Edit masked_; // the key's dots, its buffer kept between frames
    double last_reveal_ = 0;
    double done_since_ = 0; // the done bar shown, or last under the mouse
    bool rest_motion_on_ = true;
    bool reduced_motion_ = false; // Windows' animations off: the idle orb stands still
    bool wants_theme_frame_ = false; // another theme since the last frame: one is drawn, whatever else rests
    // The hidden agent's sign (UI update, stage 3): its scene, the theme it is drawn in (Windows'
    // own mode), its dot's colour (kept while the dot goes), the motion clock at its last step.
    scenes::Sign sign_;
    const ::theme::Theme* sign_theme_ = &::theme::dark();
    ::theme::Color sign_dot_{};
    // Its capsule: the mouse was in the sign's zone (or on the capsule) at the last frame; the
    // capsule's width for what it says, measured at the last frame; the shortcut it names.
    bool sign_hovered_ = false;
    float sign_capsule_w_ = 0;
    std::string shortcut_ = "Ctrl+Space";
    double sign_at_ = -1.0e9;
    void step_sign(const Model& m);
    // The sign as it is now, at `zone`, at this opacity (the rest's frame and a fold's end).
    void draw_handle(const Model& m, const ui::layout::Box& zone, float opacity);
    // The fold's end at the edge: the sign's own shape now (the handle, or its capsule).
    [[nodiscard]] auto handle_shape() const -> Shape;
    // The orb's size as the window's shape reaches the handle (it is gone by half the way).
    static constexpr float kHandleOrb = 30.0f;
    float handle_drawn_w_ = 0; // this frame's handle (its bar's width), 0 if none
    // The window's motion (app::motion): opening, growing, closing; the strip only once closed.
    motion::WindowMotion motion_;
    motion::MotionFrame frame_;
    bool motion_open_ = false;     // opened and not yet closing
    float motion_height_ = 0;      // the height last asked of the motion
    bool arrive_next_ = false;     // the next opening: at once, after an install (arrive_next())
    float open_width_ = 0;         // the open panel's width, kept while it closes
    double hidden_at_ = -1.0e9;    // when the close ended: the strip fades in from there
    bool moving_window_ = false;   // the glass is shown: open, or still closing
    float open_radius_ = 0;
    // The panel as last drawn and its bottom corners' radius: a scrolled view down to its bottom
    // fades with the same rounding (bottom_corners), or its square fade fills them (phase 10).
    ui::layout::Box glass_{};
    float glass_radius_ = 0;
    [[nodiscard]] auto bottom_corners(const ui::layout::Box& viewport) const -> ui::render::Corners {
        const bool bottom = std::abs(viewport.y + viewport.h - (glass_.y + glass_.h)) < 1.5f;
        const bool left = std::abs(viewport.x - glass_.x) < 1.5f;
        const bool right = std::abs(viewport.x + viewport.w - (glass_.x + glass_.w)) < 1.5f;
        return {0, 0, bottom && right ? glass_radius_ : 0.0f, bottom && left ? glass_radius_ : 0.0f};
    }
    // The data panel's ground under `panel` (the theme's panel colour, as see-through as the window),
    // the window's corner kept clear; and that colour, for what fades into it.
    void panel_ground(const ui::layout::Box& panel);
    [[nodiscard]] auto panel_color() const -> ui::render::Color;
    // The window's own colour at height `y` (lighter at its top): what a fade over it is made of.
    [[nodiscard]] auto surface_at(float y) const -> ui::render::Color;
    View last_open_view_ = View::Input; // what the closing panel still shows
    float motion_clip_ = 0;        // the scene's curtain this frame, before the field's row grows it
    void draw_moving(Model& m, const ui::layout::Box& panel, float radius, bool changed, double t, Reaction& r);
    bool done_pressed_ = false;

    // The steps screen: Brief or Detailed (for the session), the steps opened or closed by a click
    // (the running one is open unless closed), the scroll of the cards.
    bool brief_ = false;
    std::vector<std::uint32_t> toggled_; // steps whose open state the user flipped
    float steps_scroll_ = 0;             // pixels scrolled down from the top of the cards
    bool steps_follow_ = true;           // keep the running step in view, until the wheel moves
    std::uint64_t steps_task_ = 0;

    // The permission screen (sheet_permission.cpp): the question, the card with its files, the
    // access levels beside it; the levels' lists open or closed.
    auto draw_question(const Model& m, float right, float y, float width) -> float;
    void draw_levels(Model& m, const events::Permission& p, const ui::layout::Box& column, Reaction& r);
    std::array<bool, 4> level_open_{};

    // The text field: its lines laid out at its width (phase 10), scrolled to keep the caret in view.
    std::string field_text_;
    std::size_t field_caret_ = 0;
    double edited_at_ = 0;
    float field_scroll_ = 0; // lines scrolled up, in pixels
    float field_grab_ = -1;  // its scrollbar held: where in it the mouse took it
    std::optional<ui::text::Paragraph> field_para_;
    // The field's text as it was when it was emptied (MS 04: it goes on micro, the hint comes back
    // at +60 on quick): the paragraph itself, swapped out — nothing shaped again.
    std::optional<ui::text::Paragraph> gone_para_;
    std::pair<std::size_t, std::size_t> gone_sel_{};  // its selection then
    std::pair<std::size_t, std::size_t> last_sel_{};  // the field's last selection (it fades on micro)
    float gone_scroll_ = 0;
    double gone_at_ = -1.0e9;
    float field_px_ = 0;      // its text size x scale
    float field_w_ = 0;       // its width as last drawn (logical)
    float field_wrap_ = -1;   // the width it was wrapped at
    static constexpr std::size_t kNoCaret = static_cast<std::size_t>(-1);
    std::size_t end_caret_ = kNoCaret; // a caret End or Up/Down put at a line's end: drawn there
    bool field_over_ = false; // past kFieldLines: wrapped 12 px narrower, beside its scrollbar
    std::size_t field_most_ = kFieldLines; // its lines before it scrolls: kEditorLines in the editor
    std::uint64_t question_open_ = 0; // the task whose long question is shown whole (Show more)
    std::string_view question_link_;
    std::string_view key_line_; // one of key_texts_, or the model's key_error (lives while the model does)
    // The key's row, said with the provider's name from the catalog: made at the first key row, kept.
    std::array<std::string, 3> key_texts_;
    [[nodiscard]] auto editor_open(const Model& m) const -> bool;
    // The long paste's notice under the row (screen 31), and its chip's card under the mouse.
    [[nodiscard]] auto notice_open(const Model& m) const -> bool;
    void draw_paste_notice(Model& m, const ui::layout::Box& panel);
    void draw_paste_preview(Model& m, const ui::layout::Box& glass);
    std::size_t paste_preview_ = static_cast<std::size_t>(-1); // the chip whose card is up
    ui::layout::Box paste_chip_{};
    float paste_bottom_ = 0; // the card's bottom under the panel's top (logical): the window makes room
    std::optional<ui::text::Paragraph> paste_para_;
    float paste_px_ = 0;
    float paste_wrap_ = -1;
    // The editor's foot (screen 29): its keys, the counter, fold and send.
    void draw_editor_foot(Model& m, const ui::layout::Box& panel, Reaction& r);
    bool field_follow_ = true; // the scroll follows the caret, until the wheel takes it
    std::array<char, 32> hint_buf_{};
    std::size_t hint_len_ = 0;
    CounterText counter_buf_{};
    std::string_view counter_;
    // Past kFieldLines: the top fade, "N lines above" and the scrollbar over the field `box`.
    void draw_field_scroll(Model& m, const ui::layout::Box& box);
    void drag_field_bar(const Model& m, const ui::layout::Box& box);
    void select_in_field(Model& m, const ui::layout::Box& box, bool held);
    [[nodiscard]] auto field_byte_at(const ui::layout::Box& box, float x, float y) const -> std::size_t;
    bool field_selecting_ = false; // the field's text dragged over by the mouse
    bool keys_to_field_ = false;   // keys_to_field()
    bool field_pressed_ = false;   // this frame's press was the field's
    // The row's height from the field's lines now; true when it changed.
    auto count_row_lines(const Model& m) -> bool;
    float row_h_ = 90.0f;     // the row's height now
    float row_to_ = 90.0f;    // where it is going…
    ui::motion::MotionToken row_token_ = ui::motion::kQuick; // …on this: MS 04, quick; emptied, smooth
    float row_hold_ = 90.0f;  // the larger of where it was and where it goes, while it moves
    float window_h_ = 0;      // window_height()
    // design/ui_model: the orb's look moving to the model choice shown, the chip as drawn, the room
    // the hint under the bar takes below the panel (the window grows for it while it shows).
    ui::orb_look::Motion look_motion_;
    ui::orb_look::Frame look_frame_;
    bool look_snapped_ = false;
    bool hint_up_ = false;
    // MS 06: the steps' rows seen lately — the state each was last drawn in and when it was first
    // seen done (its check starts kCheckAt after); a few, no allocation. One first seen done is old.
    struct StepSeen {
        std::uint64_t key = 0; // the row's list and its step's id
        std::uint64_t id = 0;
        events::StepState state = events::StepState::Wait;
        double done_at = -1.0e9;
        StepMotion motion{};
    };
    std::array<StepSeen, 48> steps_seen_{};
    std::size_t steps_seen_next_ = 0;
    // The live block's fold (MS 06): the task it is of, whether it was at work, when the work ended.
    std::uint64_t actions_task_ = 0;
    bool actions_busy_ = false;
    double actions_done_at_ = -1.0e9;
    ActionsFold actions_fold_{};
    // Enter (MS 05, app::scenes::Send): started when a task begins with the field just emptied;
    // any other new task finds it at rest. The text flies from where the field had it to where the
    // turn draws its bubble's first line this frame.
    scenes::Send send_;
    scenes::SendFrame send_frame_{};
    double send_last_ = -1.0e9;
    std::uint64_t sent_task_ = 0;
    bool send_seen_ = false;   // a first task seen: the next new one may be a send
    bool send_fresh_ = false;  // the conversation was not shown before it: its head and panel come in
    ui::layout::Box field_box_{};    // the field as last drawn
    ui::layout::Box send_from_{};    // …when Enter came
    ui::layout::Box question_at_{};  // the bubble's first line this frame: x, top, its width, a line's height
    bool question_drawn_ = false;
    void update_send(const Model& m, double now, bool was_conversation);
    void draw_send_fly();
    // `busy`: at work — the rail is there; the work done, it fills and goes on smooth (MS 06).
    void draw_rail(const Model& m, const ui::layout::Box& rail, bool busy = true);
    RowMotion row_motion_{};
    // A part of the conversation in Enter's scene: 0 the column's head, 3 the data panel — its
    // opacity set over cascade_base_; returns how far below its place it is.
    auto send_part(int index) -> float;
    float hint_alpha_ = 0;         // as drawn
    double hint_seen_at_ = -1.0e9; // the hint's own time when it was last drawn: another one is new
    // The chip's bars (MS § 7): the level they showed, and from which and when they last changed —
    // up they light one by one, down they go from the top.
    int chip_level_ = -1;
    std::string_view chip_name_, chip_name_from_; // the chip's name and the one it crosses from (catalog text)
    int chip_level_from_ = 0;
    double chip_level_at_ = -1.0e9;
    // The wheel over the orb and over the picker's scale, in notches not yet stepped: a level a
    // whole notch, so a touchpad's small moves add up instead of each taking a level.
    float orb_notches_ = 0;
    float scale_notches_ = 0;
    [[nodiscard]] auto notch_step(float& kept, bool over) -> int;
    ModelChipShown model_chip_;
    // The model menu (sheet_menu.cpp, stage 4b; MODEL_PICKER_SPEC): a pop under the bar, over the
    // frame. `row_top`: the bar's row's top in the window; `right`: its right edge.
    void draw_menu(Model& m, float row_top, float right, Reaction& r);
    // Its height for its rows (the list cut, never scrolled: the catalog's few models fit).
    [[nodiscard]] auto menu_height(const Model& m) const -> float;
    void draw_menu_tip(const Model& m, const ai::catalog::Model& model, const ui::layout::Box& row, const ui::layout::Box& menu);
    ui::layout::Box menu_box_{};   // as last drawn (its overlay; a press outside closes it)
    float menu_alpha_ = 0;          // 0 closed: the window keeps its room while it fades
    float menu_mouse_x_ = -1.0e9f;  // the mouse at the last frame: a row lit by the mouse only when it moves
    float menu_mouse_y_ = -1.0e9f;
    std::size_t menu_tip_row_ = static_cast<std::size_t>(-1); // the row the tooltip waits on or shows
    double menu_tip_from_ = 0;      // since when that row is lit
    bool menu_tip_up_ = false;      // shown: the next row's comes without the wait
    bool menu_place_ = false;       // its tracks stand at their places this frame (a fresh opening)
    ui::layout::Box orb_controls_{}; // the bar's orb as last drawn: a press on it toggles the menu itself
    static constexpr float kHintRoom = 44.0f; // 8 above, 28 the hint, 8 below
    float held_h_ = 0;        // the frame's height with the row held
    [[nodiscard]] auto field_view(const Model& m, View v) const -> bool;
    [[nodiscard]] auto field_keys(const Model& m) const -> bool;
    auto shown_field(const Model& m) -> const Edit&;
    void sync_field(const Model& m);
    // The caret's line and its x (logical px) in the laid-out field.
    [[nodiscard]] auto caret_place(std::size_t caret) const -> std::pair<std::size_t, float>;

    std::vector<Target> targets_; // the first target_count_ are this frame's
    std::size_t target_count_ = 0;
    std::array<ui::layout::Box, 8> solid_{}; // the first solid_count_ are this frame's
    std::size_t solid_count_ = 0;
    void add_solid(const ui::layout::Box& b) noexcept {
        if (solid_count_ < solid_.size()) {
            solid_[solid_count_++] = b;
        }
    }
};

} // namespace app::sheet
