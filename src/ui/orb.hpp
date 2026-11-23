#pragma once

// ui::Orb — the agent's liquid chrome orb (variant B), ported from the sheet mockup's own shader
// (design/agent-placements.html, <script id="fs">: liquid-orbs.html's v_chrome adapted to sit on
// the sheet: transparent outside the body, a halo and rim, phase accumulated so speed changes do
// not jump). Raymarched into its own square texture; the .orbc mask
// radial-gradient(closest-side, #000 70%, transparent) is applied in the shader.
//
// Parameters follow the mood on orb-token springs (motion rework, MOTION_SPEC § 4); a streamed
// token's pulse raises the energy's target, which fades (ui/motion_tokens.hpp, namespace orb).
//
// Two styles. Classic: the violet, cyan and pink chrome of the sheet and installer mockups (the
// installer keeps it). Model (design/ui_model, orb-demo.html): the same chrome whose material is the
// model and whose drops and glow are the thinking level (ui::orb_look); its moods change the motion
// only, never the colour.

#include "core/error.hpp"
#include "ui/motion.hpp"
#include "ui/orb_look.hpp"
#include "ui/gl/objects.hpp"
#include "ui/gl/program_cache.hpp"
#include "ui/render/renderer.hpp"
#include "ui/render/target.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace ui {

class Orb {
public:
    enum class Mood : std::uint8_t { Idle, Thinking, Answering };
    enum class Style : std::uint8_t { Classic, Model };

    struct Params {
        float spike = 0.05f;
        float speed = 0.35f;
        float hue = 0.0f;
        float energy = 0.0f;
        float phase = 0.0f;
    };

    // `size`: texture side in physical pixels; the sheet draws an 80 px orb from 2x (160).
    static auto create(gl::ProgramCache& cache, int size, Style style = Style::Classic) -> core::Result<Orb>;

    void set_mood(Mood mood) noexcept { mood_ = mood; }
    // The model's style: the material, drops and glow (and a model change's extra spike) for the
    // next render. The classic style has none of them: ignored.
    void set_look(const orb_look::Frame& frame) noexcept { look_ = frame; }
    // The model's style: the speeds and the thinking spike of the level chosen (the moods' targets).
    void set_energy(const orb_look::Energy& energy) noexcept { energy_look_ = energy; }
    [[nodiscard]] auto style() const noexcept -> Style { return style_; }
    // Sets the parameters directly, at rest: deterministic snapshots (gallery, tests).
    void set_params(const Params& params) noexcept;
    void pulse(float strength = 1.0f) noexcept;
    // How much of the scene the texture frames: 1 the agent's, below 1 closer (the installer's big
    // orb: the body larger in the same texture).
    void set_zoom(float zoom) noexcept { zoom_ = zoom > 0 ? zoom : 1.0f; }
    // Advances the parameters to `now` (seconds). The first call only records the time.
    void update(double now) noexcept;
    // Draws into the orb's texture. Binds the orb's framebuffer: rebind your target afterwards.
    void render(double now);

    [[nodiscard]] auto texture() const noexcept -> GLuint { return target_.texture(); }
    // The texture's rows run bottom to top (GL): draw it with these coordinates.
    [[nodiscard]] static constexpr auto uv() noexcept -> render::Uv { return {0, 1, 1, 0}; }
    [[nodiscard]] auto size() const noexcept -> int { return target_.width(); }
    [[nodiscard]] auto params() const noexcept -> const Params& { return params_; }
    // A still of the orb as last rendered, in a texture of its own sampled as the orb's (draw it
    // with uv()): the agent's mark beside its answers. Binds a framebuffer: rebind yours after.
    [[nodiscard]] auto still() const -> core::Result<render::Target>;
    // Straight-alpha RGBA, rows top to bottom (tests, snapshots).
    [[nodiscard]] auto read() const -> std::vector<std::uint8_t> { return target_.read(); }
    void read_into(std::vector<std::uint8_t>& out) const { target_.read_into(out); }

    static constexpr int kMaxSteps = 120; // raymarch steps, as in the mockup

private:
    Orb(gl::Program program, gl::VertexArray vao, render::Target target, Style style)
        : program_(std::move(program)), vao_(std::move(vao)), target_(std::move(target)), style_(style) {}

    gl::Program program_;
    gl::VertexArray vao_;
    render::Target target_;
    Style style_ = Style::Classic;
    struct Uniforms {
        GLint res, time, phase, spike, energy, hue, steps, zoom, drops, level, l1, l2, l3, tint, glow, amb;
    } u_{};
    Mood mood_ = Mood::Idle;
    Params params_;
    // The mood's parameters and the energy, as springs (motion rework): params_ is what they say.
    motion::Spring spike_{.x = 0.05f, .v = 0, .target = 0.05f, .tok = motion::kOrb};
    motion::Spring speed_{.x = 0.35f, .v = 0, .target = 0.35f, .tok = motion::kOrb};
    motion::Spring hue_{.x = 0, .v = 0, .target = 0, .tok = motion::kOrb};
    motion::Spring energy_{.x = 0, .v = 0, .target = 0, .tok = motion::kQuick};
    float energy_target_ = 0;
    orb_look::Frame look_;
    orb_look::Energy energy_look_;
    float zoom_ = 1.0f;
    std::optional<double> last_;
    std::optional<double> epoch_; // the first frame's time: uTime counts from it
};

} // namespace ui
