#pragma once

// ui::render::Renderer — the batch renderer. Every primitive is one 96-byte instance written
// straight into a persistent-mapped ring buffer (3 segments, each guarded by a fence) and drawn
// as an instanced quad by one uber-shader: SDF rounded rectangle (per-corner radius, border,
// linear gradient), soft shadow, glyph (R8 atlas coverage), image (RGBA texture), segment
// (capsule, optionally blurred) and arc.
//
// Coordinates are logical pixels, top-left origin; begin() takes the DPI scale. Clipping is
// geometric (each quad is cut to the clip rectangle in the vertex shader), so clip changes never
// break a batch; only a change of image texture or glyph atlas does.
//
// Between begin() and end() the renderer owns the GL state (program, vertex array, blending,
// texture units 0 and 1). The frame allocates nothing: there is no command list to grow.

#include "core/error.hpp"
#include "theme/color.hpp"
#include "ui/gl/objects.hpp"
#include "ui/gl/program_cache.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace ui::render {

// sRGB colour with straight (not premultiplied) alpha: the theme module's (it owns every colour).
using Color = theme::Color;

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
};

struct Point {
    float x = 0, y = 0;
};

struct Corners {
    float tl = 0, tr = 0, br = 0, bl = 0;
};

// Texture coordinates of a sub-rectangle, (u0, v0) at the quad's top-left corner.
struct Uv {
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
};

struct RectStyle {
    Color fill{};
    std::optional<Color> fill_to; // set: linear gradient from fill to fill_to
    float gradient_angle = 0.0f;  // radians; 0 runs left to right, pi/2 top to bottom
    Corners radius{};
    Color border{};
    float border_width = 0.0f;    // drawn inside the rectangle
};

struct FrameStats {
    std::uint32_t draw_calls = 0;
    std::uint32_t instances = 0;
};

struct Instance; // GPU layout, renderer.cpp

class Renderer {
public:
    static auto create(gl::ProgramCache& cache) -> core::Result<Renderer>;
    ~Renderer();
    Renderer(Renderer&& other) noexcept;
    Renderer& operator=(Renderer&& other) noexcept;
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Draws into the currently bound framebuffer of `width` x `height` physical pixels.
    void begin(int width, int height, float scale = 1.0f);
    void rect(const Rect& r, const RectStyle& style);
    void shadow(const Rect& r, float radius, float blur, Color color);
    void glyph(const Rect& r, const Uv& uv, Color color);
    void image(const Rect& r, const Uv& uv, GLuint texture, Color tint);
    // A line from `a` to `b`, `width` wide with round ends (graphs, dashes of a path); `blur` > 0
    // softens it into a glow of about that many pixels.
    void segment(Point a, Point b, float width, Color color, float blur = 0.0f);
    // The path through `points` drawn from its start over `fraction` (0..1) of its length: a mark
    // that draws itself (MOTION_SPEC scene 06: the check). Segments, `width` wide, round ends.
    void stroke(std::span<const Point> points, float width, Color color, float fraction);
    // A ring of `radius` (to the middle of its stroke) and `width`, from angle `from` to `to`
    // (radians, 0 at 12 o'clock, clockwise, 0 <= from <= to <= 2 pi), round ends.
    void arc(Point center, float radius, float width, float from, float to, Color color);
    // The clip is intersected with the current one. At most kMaxClipDepth levels.
    void push_clip(const Rect& r);
    void pop_clip();
    void end();

    // R8 coverage texture sampled by glyph(); kept across frames.
    void set_glyph_atlas(GLuint texture);
    // Multiplies the alpha of everything drawn after it (layers fading in and out). begin()
    // resets it to 1.
    void set_opacity(float opacity) noexcept { opacity_ = opacity < 0 ? 0 : (opacity > 1 ? 1 : opacity); }
    [[nodiscard]] auto opacity() const noexcept -> float { return opacity_; }
    // Glyph coverage is sharpened like DirectWrite's enhanced contrast, a * (k + 1) / (a * k + 1):
    // FreeType's plain coverage makes light text on the dark sheet thinner and softer than native
    // Windows text (spikes/text-compare). Kept across frames.
    void set_text_contrast(float k) noexcept { text_contrast_ = k < 0 ? 0 : k; }
    static constexpr float kTextContrast = 1.0f;
    // The frame begun, in logical pixels (begin()'s size over its scale).
    [[nodiscard]] auto logical_width() const noexcept -> float { return view_w_; }
    [[nodiscard]] auto logical_height() const noexcept -> float { return view_h_; }

    [[nodiscard]] auto last_frame() const noexcept -> const FrameStats& { return last_; }
    // Instances per ring segment: a frame larger than this is split into several draw calls.
    [[nodiscard]] static constexpr auto capacity() noexcept -> std::uint32_t {
        return kSegmentInstances;
    }

    static constexpr std::uint32_t kSegments = 3;
    static constexpr std::uint32_t kSegmentInstances = 16384;
    static constexpr int kMaxClipDepth = 32;

private:
    Renderer() = default;
    void push(const Instance& instance);
    void flush();
    void next_segment();
    void bind_image(GLuint texture);

    gl::Program program_;
    gl::VertexArray vao_;
    gl::Buffer buffer_;
    Instance* mapped_ = nullptr;
    std::array<gl::Sync, kSegments> fences_;
    GLint u_viewport_ = -1;
    GLint u_scale_ = -1;
    GLint u_text_contrast_ = -1;
    float text_contrast_ = kTextContrast;
    float view_w_ = 0, view_h_ = 0;

    std::uint32_t segment_ = 0;
    std::uint32_t used_ = 0;        // instances written into the current segment
    std::uint32_t batch_first_ = 0; // first instance of the batch not drawn yet
    GLuint image_ = 0;
    GLuint atlas_ = 0;
    bool in_frame_ = false;

    std::array<Rect, kMaxClipDepth + 1> clips_{};
    int clip_depth_ = 0;
    int clip_overflow_ = 0; // pushes beyond kMaxClipDepth, popped first
    float opacity_ = 1.0f;

    FrameStats frame_{};
    FrameStats last_{};
};

} // namespace ui::render
