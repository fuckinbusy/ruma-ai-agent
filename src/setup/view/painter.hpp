#pragma once

// setup::view::Painter — the installer's window drawn from a setup::State (design/ui_installer:
// 03_welcome … 06_done, with the spec's changes: no crash reports, no "Open the agent now" and no
// Launch). The same glass and chrome orb as the agent: ui::Orb, ui::work's buttons and switches.
// Immediate mode: draw() returns what was clicked as a setup::Action; the window turns keys into
// actions itself.

#include "setup/flow.hpp"
#include "setup/view/motion.hpp"
#include "ui/layout.hpp"
#include "ui/motion.hpp"
#include "ui/orb.hpp"
#include "ui/ui.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace setup::view {

struct Target {
    std::string name;
    ::ui::layout::Box box;
};

struct Reaction {
    std::optional<Action> action;
};

class Painter {
public:
    // The window (README § Window): 560 x 660, radius 22, the top 44 and the footer 84; around it the
    // room its shadow (0 40px 90px -20px) takes.
    static constexpr float kWidth = 560.0f;
    static constexpr float kHeight = 660.0f;
    static constexpr float kRadius = 22.0f;
    static constexpr float kTopBar = 44.0f;
    static constexpr float kFooter = 84.0f;
    static constexpr float kMarginSide = 72.0f;
    static constexpr float kMarginTop = 32.0f;
    static constexpr float kMarginBottom = 112.0f;

    // The orb is framed close (ui::Orb::set_zoom): its body is 58 % of the texture, so a 240 px orb
    // wants a texture about 420 px (times the DPI scale) across.
    static constexpr float kOrbQuad = kOrbZoom / kBodyShare; // the drawn texture against the body's size

    Painter(::ui::Ui& ui, ::ui::Orb& orb) : ui_{&ui}, orb_{&orb} { orb.set_zoom(kOrbZoom); }

    // Moves the orb toward the screen's place (τ 110 ms) and sets its mood.
    void update(const State& s, double now);
    // The orb's texture for this frame (binds its framebuffer: rebind the target after).
    void render_orb(double now);
    auto draw(const State& s, const ::ui::layout::Box& panel) -> Reaction;
    // The appearance and the launch (motion.hpp), in the overlay's coordinates: `window` is where
    // the window stands. Nothing else is drawn — no dimming of what is behind.
    void draw_intro(const State& s, const IntroFrame& f, const ::ui::layout::Box& window);
    void draw_launch(const State& s, const LaunchFrame& f, const ::ui::layout::Box& window);

    // The top bar's minimize button was clicked in the last draw (the window minimizes).
    [[nodiscard]] auto minimize_clicked() const noexcept -> bool { return minimize_; }
    // Where the window may be dragged: the top bar without its buttons (window coordinates).
    [[nodiscard]] auto drag_box() const noexcept -> const ::ui::layout::Box& { return drag_; }
    // The last draw's clickable boxes by name, and every text it wrote (tests, the gallery).
    [[nodiscard]] auto targets() const noexcept -> const std::vector<Target>& { return targets_; }
    [[nodiscard]] auto texts() const noexcept -> const std::vector<std::string>& { return texts_; }
    // A screen changing, as drawn this frame: how much of the one that goes is left, how much of
    // the one that comes is there, how far under its place that one still is.
    struct ScreenMotion {
        float out = 0;
        float in = 1;
        float lift = 0;
    };
    [[nodiscard]] auto screen_motion() const noexcept -> const ScreenMotion& { return screen_motion_; }
    // The progress as drawn: on its way to the state's on smooth.
    [[nodiscard]] auto shown_progress() const noexcept -> float { return progress_.x; }
    // Windows' animation effects off: nothing shifts, opacity only.
    void set_reduced(bool on) noexcept { reduced_ = on; }
    // The orb's place in the panel (its centre and drawn size), for the animations.
    [[nodiscard]] auto orb_centre_y() const noexcept -> float { return orb_y_; }
    [[nodiscard]] auto orb_size() const noexcept -> float { return orb_d_; }

private:
    struct Place {
        float y;     // the orb's centre from the window's top
        float scale; // 1.0 ≈ 240 px across
    };
    static auto place_of(Screen s) -> Place;

    void text(::ui::Face face, float size, std::string_view t, float x, float baseline, ::ui::render::Color c);
    void centred(::ui::Face face, float size, std::string_view t, float cx, float top, float height,
                 ::ui::render::Color c);
    auto width(::ui::Face face, float size, std::string_view t) -> float;
    auto button(std::string_view name, const ::ui::layout::Box& box, std::string_view label, bool primary,
                bool enabled = true, bool crit = false) -> bool;
    auto main_button(std::string_view name, float right, float y, std::string_view label, bool enabled = true,
                     bool crit = false) -> std::pair<bool, float>;
    void target(std::string_view name, const ::ui::layout::Box& box);

    void draw_window(const ::ui::layout::Box& p);
    void draw_content(const State& s, const ::ui::layout::Box& p, Reaction& r);
    void draw_screen(const State& s, const ::ui::layout::Box& p, Reaction& r); // the screen's own part, under the top bar
    // The content's part `k` of the appearance (motion.hpp: IntroFrame::parts): sets its opacity and
    // returns how far under its place it still is. Outside the appearance: as it is, 0.
    auto part(std::size_t k) -> float;
    const std::array<Part, ::ui::motion::installer::kContentParts>* parts_ = nullptr;
    void draw_glass(const ::ui::layout::Box& box, float radius_top, float radius_bottom, float alpha, float chrome, float rim_alpha,
                    float rim_degrees);
    void orb_at(::ui::render::Point centre, float quad, float alpha);
    void draw_top(const ::ui::layout::Box& p, Reaction& r);
    void draw_footer_line(const ::ui::layout::Box& p);
    void draw_orb(const ::ui::layout::Box& p);
    void draw_ring(const ::ui::layout::Box& p, float progress);
    void draw_welcome(const State& s, const ::ui::layout::Box& p, Reaction& r);
    void draw_options(const State& s, const ::ui::layout::Box& p, Reaction& r);
    void draw_working(const State& s, const ::ui::layout::Box& p, Reaction& r);
    void draw_done(const State& s, const ::ui::layout::Box& p, Reaction& r);
    void draw_message(const State& s, const ::ui::layout::Box& p, Reaction& r);
    void chips(const ::ui::layout::Box& p, float top);
    void keys(std::string_view hotkey, float cx, float top, float height, bool big);

    ::ui::Ui* ui_;
    ::ui::Orb* orb_;
    double now_ = 0;
    double last_ = -1;
    float orb_y_ = 190.0f;
    float orb_d_ = 240.0f;
    ::ui::motion::Spring orb_y_spring_{.range = 200.0f}; // their ways on smooth (orb_y_, orb_d_ are their values)
    ::ui::motion::Spring orb_d_spring_{.range = 200.0f};
    bool placed_ = false;
    // The screens' motion: its clock (a frame's time at most a step), the progress on smooth, the
    // screen shown and when it came, and the one it took the place of — a copy of the state made at
    // the change, drawn going.
    double tick_ = 0;
    ::ui::motion::Spring progress_{};
    Screen shown_screen_ = Screen::Welcome;
    double screen_at_ = -1.0e9;
    std::optional<State> leaving_;
    ScreenMotion screen_motion_{};
    bool reduced_ = false;
    bool minimize_ = false;
    ::ui::layout::Box drag_{};
    std::vector<Target> targets_;
    std::vector<std::string> texts_;
};

} // namespace setup::view
