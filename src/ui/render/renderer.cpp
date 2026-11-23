#include "ui/render/renderer.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace ui::render {

// One primitive as the vertex shader sees it (per-instance attributes, divisor 1). Logical
// pixels; the shader applies the DPI scale.
struct Instance {
    float rect[4];        // x, y, w, h
    float radius[4];      // tl, tr, br, bl
    float clip[4];        // x, y, w, h
    float uv[4];          // u0, v0, u1, v1
    float params[4];      // border width | half width, gradient angle | arc start, blur, arc end
    std::uint32_t colors[4]; // fill, fill_to, border (RGBA8, straight alpha), kind
};
static_assert(sizeof(Instance) == 96);

namespace {

// kSegment keeps its endpoints in `uv` (logical pixels); kArc its radius in radius[0].
enum Kind : std::uint32_t { kShape = 0, kShadow = 1, kGlyph = 2, kImage = 3, kSegment = 4, kArc = 5 };

constexpr float kNoClip = 1.0e9f;
constexpr Rect kEverything{-kNoClip, -kNoClip, 2 * kNoClip, 2 * kNoClip};

constexpr const char* kVertex = R"(#version 450 core
layout(location = 0) in vec4 a_rect;
layout(location = 1) in vec4 a_radius;
layout(location = 2) in vec4 a_clip;
layout(location = 3) in vec4 a_uv;
layout(location = 4) in vec4 a_params;
layout(location = 5) in uvec4 a_colors;

uniform vec2 u_viewport; // physical pixels
uniform float u_scale;

out vec2 v_pos;
flat out vec4 v_rect;
flat out vec4 v_radius;
flat out vec4 v_uv;
flat out vec4 v_params;
flat out uvec4 v_colors;

void main() {
    uint kind = a_colors.w;
    vec4 rect = a_rect * u_scale;
    vec4 params = vec4(a_params.x * u_scale, a_params.y, a_params.z * u_scale, a_params.w);
    // Shapes and arcs get a 1 px margin for the anti-aliased edge, shadows and segments their blur
    // extent too; glyphs and images cover exactly their rectangle.
    float pad = (kind == 0u || kind == 5u) ? 1.0 : ((kind == 1u || kind == 4u) ? params.z * 1.5 + 1.0 : 0.0);
    vec2 lo = rect.xy - pad;
    vec2 hi = rect.xy + rect.zw + pad;
    vec4 clip = a_clip * u_scale;
    lo = max(lo, clip.xy);
    hi = max(min(hi, clip.xy + clip.zw), lo);
    vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    vec2 p = mix(lo, hi, corner);

    v_pos = p;
    v_rect = rect;
    v_radius = a_radius * u_scale;
    v_uv = kind == 4u ? a_uv * u_scale : a_uv; // a segment's endpoints are coordinates
    v_params = params;
    v_colors = a_colors;
    gl_Position = vec4(p.x / u_viewport.x * 2.0 - 1.0, 1.0 - p.y / u_viewport.y * 2.0, 0.0, 1.0);
})";

constexpr const char* kFragment = R"(#version 450 core
in vec2 v_pos;
flat in vec4 v_rect;
flat in vec4 v_radius;
flat in vec4 v_uv;
flat in vec4 v_params;
flat in uvec4 v_colors;

layout(binding = 0) uniform sampler2D u_atlas;
layout(binding = 1) uniform sampler2D u_image;
uniform float u_text_contrast;

out vec4 o_color; // premultiplied

// Signed distance to a box with per-corner radii (tl, tr, br, bl); y grows downwards.
float sd_round_box(vec2 p, vec2 half_size, vec4 radii) {
    vec4 r = min(radii, vec4(min(half_size.x, half_size.y)));
    float rr = p.x < 0.0 ? (p.y < 0.0 ? r.x : r.w) : (p.y < 0.0 ? r.y : r.z);
    vec2 q = abs(p) - half_size + rr;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - rr;
}

float erf_approx(float x) {
    float s = sign(x);
    float a = abs(x);
    float t = 1.0 + (0.278393 + (0.230389 + 0.078108 * (a * a)) * a) * a;
    t *= t;
    return s - s / (t * t);
}

vec4 premultiply(vec4 c) { return vec4(c.rgb * c.a, c.a); }

void main() {
    uint kind = v_colors.w;
    vec4 fill = unpackUnorm4x8(v_colors.x);
    vec2 half_size = v_rect.zw * 0.5;
    vec2 p = v_pos - (v_rect.xy + half_size);

    if (kind == 0u) {
        float d = sd_round_box(p, half_size, v_radius);
        float coverage = clamp(0.5 - d, 0.0, 1.0);
        vec2 dir = vec2(cos(v_params.y), sin(v_params.y));
        float extent = abs(dir.x) * half_size.x + abs(dir.y) * half_size.y;
        float t = clamp(dot(p, dir) / max(2.0 * extent, 1e-6) + 0.5, 0.0, 1.0);
        vec4 color = mix(fill, unpackUnorm4x8(v_colors.y), t);
        float border = v_params.x;
        if (border > 0.0) {
            float inner = sd_round_box(p, max(half_size - border, vec2(0.0)),
                                       max(v_radius - border, vec4(0.0)));
            color = mix(unpackUnorm4x8(v_colors.z), color, clamp(0.5 - inner, 0.0, 1.0));
        }
        o_color = premultiply(color) * coverage;
    } else if (kind == 1u) {
        float d = sd_round_box(p, half_size, v_radius);
        float sigma = max(v_params.z * 0.5, 1e-3);
        float coverage = 0.5 - 0.5 * erf_approx(d / (sigma * 1.4142136));
        o_color = premultiply(fill) * coverage;
    } else if (kind == 4u) {
        vec2 pa = v_pos - v_uv.xy;
        vec2 ba = v_uv.zw - v_uv.xy;
        float h = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-6), 0.0, 1.0);
        float d = length(pa - ba * h) - v_params.x;
        float coverage = v_params.z > 0.0
            ? 0.5 - 0.5 * erf_approx(d / (max(v_params.z * 0.5, 1e-3) * 1.4142136))
            : clamp(0.5 - d, 0.0, 1.0);
        o_color = premultiply(fill) * coverage;
    } else if (kind == 5u) {
        const float tau = 6.2831853;
        float r = v_radius.x;
        float a = atan(p.x, -p.y); // 0 at 12 o'clock, clockwise (y grows downwards)
        if (a < 0.0) {
            a += tau;
        }
        float from = v_params.y;
        float to = v_params.w;
        float d;
        if (a >= from && a <= to) {
            d = abs(length(p) - r) - v_params.x;
        } else {
            vec2 e0 = r * vec2(sin(from), -cos(from));
            vec2 e1 = r * vec2(sin(to), -cos(to));
            d = min(length(p - e0), length(p - e1)) - v_params.x;
        }
        o_color = premultiply(fill) * clamp(0.5 - d, 0.0, 1.0);
    } else {
        vec2 local = (v_pos - v_rect.xy) / max(v_rect.zw, vec2(1e-6));
        vec2 uv = mix(v_uv.xy, v_uv.zw, local);
        if (kind == 2u) {
            float a = texture(u_atlas, uv).r;
            a = a * (u_text_contrast + 1.0) / (a * u_text_contrast + 1.0);
            o_color = premultiply(fill) * a;
        } else {
            o_color = premultiply(texture(u_image, uv) * fill);
        }
    }
})";

constexpr auto pack(Color c) noexcept -> std::uint32_t {
    return static_cast<std::uint32_t>(c.r) | static_cast<std::uint32_t>(c.g) << 8 |
           static_cast<std::uint32_t>(c.b) << 16 | static_cast<std::uint32_t>(c.a) << 24;
}

auto intersect(const Rect& a, const Rect& b) noexcept -> Rect {
    const float x0 = std::max(a.x, b.x);
    const float y0 = std::max(a.y, b.y);
    const float x1 = std::min(a.x + a.w, b.x + b.w);
    const float y1 = std::min(a.y + a.h, b.y + b.h);
    return {x0, y0, std::max(x1 - x0, 0.0f), std::max(y1 - y0, 0.0f)};
}

// True when `r` grown by `pad` on every side misses `clip`: the instance would draw nothing.
auto culled(const Rect& r, float pad, const Rect& clip) noexcept -> bool {
    return r.x - pad >= clip.x + clip.w || r.y - pad >= clip.y + clip.h ||
           r.x + r.w + pad <= clip.x || r.y + r.h + pad <= clip.y || clip.w <= 0 ||
           clip.h <= 0;
}

void wait(gl::Sync& fence) {
    if (!fence) {
        return;
    }
    for (;;) {
        const GLenum result =
            glClientWaitSync(fence.get(), GL_SYNC_FLUSH_COMMANDS_BIT, 1'000'000'000);
        if (result != GL_TIMEOUT_EXPIRED) {
            break;
        }
        WA_WARN("render", "GPU has not released a ring segment for 1 s, still waiting");
    }
    fence.reset();
}

} // namespace

auto Renderer::create(gl::ProgramCache& cache) -> core::Result<Renderer> {
    auto program = cache.build("render", kVertex, kFragment);
    if (!program) {
        return std::unexpected{program.error()};
    }
    Renderer r;
    r.program_ = std::move(*program);
    r.u_viewport_ = glGetUniformLocation(r.program_.get(), "u_viewport");
    r.u_scale_ = glGetUniformLocation(r.program_.get(), "u_scale");
    r.u_text_contrast_ = glGetUniformLocation(r.program_.get(), "u_text_contrast");

    constexpr GLsizeiptr kBytes =
        static_cast<GLsizeiptr>(sizeof(Instance)) * kSegments * kSegmentInstances;
    constexpr GLbitfield kFlags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    GLuint buffer = 0;
    glCreateBuffers(1, &buffer);
    r.buffer_ = gl::Buffer{buffer};
    glNamedBufferStorage(buffer, kBytes, nullptr, kFlags);
    r.mapped_ = static_cast<Instance*>(glMapNamedBufferRange(buffer, 0, kBytes, kFlags));
    if (r.mapped_ == nullptr) {
        return core::make_error(core::Code::Internal, "render: persistent mapping failed");
    }

    GLuint vao = 0;
    glCreateVertexArrays(1, &vao);
    r.vao_ = gl::VertexArray{vao};
    glVertexArrayVertexBuffer(vao, 0, buffer, 0, sizeof(Instance));
    glVertexArrayBindingDivisor(vao, 0, 1);
    const GLuint float_offsets[] = {offsetof(Instance, rect), offsetof(Instance, radius),
                                    offsetof(Instance, clip), offsetof(Instance, uv),
                                    offsetof(Instance, params)};
    for (GLuint i = 0; i < 5; ++i) {
        glEnableVertexArrayAttrib(vao, i);
        glVertexArrayAttribFormat(vao, i, 4, GL_FLOAT, GL_FALSE, float_offsets[i]);
        glVertexArrayAttribBinding(vao, i, 0);
    }
    glEnableVertexArrayAttrib(vao, 5);
    glVertexArrayAttribIFormat(vao, 5, 4, GL_UNSIGNED_INT, offsetof(Instance, colors));
    glVertexArrayAttribBinding(vao, 5, 0);
    return r;
}

// Deleting a still-mapped buffer is legal GL, but the AMD driver then keeps two worker threads
// alive after the context is gone (measured 2026-10-10: +2 threads per context). Unmap first.
Renderer::~Renderer() {
    if (mapped_ != nullptr) {
        glUnmapNamedBuffer(buffer_.get());
    }
}

Renderer::Renderer(Renderer&& other) noexcept {
    *this = std::move(other);
}

Renderer& Renderer::operator=(Renderer&& other) noexcept {
    if (this != &other) {
        if (mapped_ != nullptr) { // see ~Renderer: never delete a mapped buffer
            glUnmapNamedBuffer(buffer_.get());
        }
        program_ = std::move(other.program_);
        vao_ = std::move(other.vao_);
        buffer_ = std::move(other.buffer_);
        mapped_ = std::exchange(other.mapped_, nullptr);
        fences_ = std::move(other.fences_);
        u_viewport_ = other.u_viewport_;
        u_scale_ = other.u_scale_;
        u_text_contrast_ = other.u_text_contrast_;
        text_contrast_ = other.text_contrast_;
        segment_ = other.segment_;
        used_ = other.used_;
        batch_first_ = other.batch_first_;
        image_ = other.image_;
        atlas_ = other.atlas_;
        in_frame_ = std::exchange(other.in_frame_, false);
        opacity_ = other.opacity_;
        clips_ = other.clips_;
        clip_depth_ = other.clip_depth_;
        clip_overflow_ = other.clip_overflow_;
        frame_ = other.frame_;
        last_ = other.last_;
    }
    return *this;
}

void Renderer::begin(int width, int height, float scale) {
    view_w_ = static_cast<float>(width) / (scale > 0 ? scale : 1.0f);
    view_h_ = static_cast<float>(height) / (scale > 0 ? scale : 1.0f);
    wait(fences_[segment_]);
    used_ = 0;
    batch_first_ = 0;
    frame_ = {};
    clips_[0] = kEverything;
    clip_depth_ = 0;
    clip_overflow_ = 0;
    opacity_ = 1.0f;
    image_ = 0;
    in_frame_ = true;

    glUseProgram(program_.get());
    glProgramUniform2f(program_.get(), u_viewport_, static_cast<float>(width),
                       static_cast<float>(height));
    glProgramUniform1f(program_.get(), u_scale_, scale);
    glProgramUniform1f(program_.get(), u_text_contrast_, text_contrast_);
    glBindVertexArray(vao_.get());
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBindTextureUnit(0, atlas_);
    glBindTextureUnit(1, 0);
}

void Renderer::rect(const Rect& r, const RectStyle& style) {
    const Rect& clip = clips_[clip_depth_];
    if (culled(r, 1.0f, clip)) {
        return;
    }
    push({{r.x, r.y, r.w, r.h},
          {style.radius.tl, style.radius.tr, style.radius.br, style.radius.bl},
          {clip.x, clip.y, clip.w, clip.h},
          {0, 0, 0, 0},
          {style.border_width, style.gradient_angle, 0, 0},
          {pack(style.fill), pack(style.fill_to.value_or(style.fill)), pack(style.border),
           kShape}});
}

void Renderer::shadow(const Rect& r, float radius, float blur, Color color) {
    const Rect& clip = clips_[clip_depth_];
    if (culled(r, blur * 1.5f + 1.0f, clip)) {
        return;
    }
    push({{r.x, r.y, r.w, r.h},
          {radius, radius, radius, radius},
          {clip.x, clip.y, clip.w, clip.h},
          {0, 0, 0, 0},
          {0, 0, blur, 0},
          {pack(color), 0, 0, kShadow}});
}

void Renderer::glyph(const Rect& r, const Uv& uv, Color color) {
    const Rect& clip = clips_[clip_depth_];
    if (culled(r, 0.0f, clip)) {
        return;
    }
    push({{r.x, r.y, r.w, r.h},
          {0, 0, 0, 0},
          {clip.x, clip.y, clip.w, clip.h},
          {uv.u0, uv.v0, uv.u1, uv.v1},
          {0, 0, 0, 0},
          {pack(color), 0, 0, kGlyph}});
}

void Renderer::image(const Rect& r, const Uv& uv, GLuint texture, Color tint) {
    const Rect& clip = clips_[clip_depth_];
    if (culled(r, 0.0f, clip)) {
        return;
    }
    bind_image(texture);
    push({{r.x, r.y, r.w, r.h},
          {0, 0, 0, 0},
          {clip.x, clip.y, clip.w, clip.h},
          {uv.u0, uv.v0, uv.u1, uv.v1},
          {0, 0, 0, 0},
          {pack(tint), 0, 0, kImage}});
}

void Renderer::segment(Point a, Point b, float width, Color color, float blur) {
    const Rect& clip = clips_[clip_depth_];
    const float half = width * 0.5f;
    const Rect box{std::min(a.x, b.x) - half, std::min(a.y, b.y) - half, std::abs(b.x - a.x) + width,
                   std::abs(b.y - a.y) + width};
    if (culled(box, blur * 1.5f + 1.0f, clip)) {
        return;
    }
    push({{box.x, box.y, box.w, box.h},
          {0, 0, 0, 0},
          {clip.x, clip.y, clip.w, clip.h},
          {a.x, a.y, b.x, b.y},
          {half, 0, blur, 0},
          {pack(color), 0, 0, kSegment}});
}

void Renderer::stroke(std::span<const Point> points, float width, Color color, float fraction) {
    if (points.size() < 2 || fraction <= 0) {
        return;
    }
    const auto length = [](Point a, Point b) { return std::hypot(b.x - a.x, b.y - a.y); };
    float whole = 0;
    for (std::size_t i = 1; i < points.size(); ++i) {
        whole += length(points[i - 1], points[i]);
    }
    float left = whole * std::min(fraction, 1.0f);
    for (std::size_t i = 1; i < points.size() && left > 0; ++i) {
        const Point a = points[i - 1];
        const Point b = points[i];
        const float l = length(a, b);
        if (l <= 0) {
            continue;
        }
        const float k = std::min(1.0f, left / l);
        segment(a, {a.x + (b.x - a.x) * k, a.y + (b.y - a.y) * k}, width, color);
        left -= l;
    }
}

void Renderer::arc(Point center, float radius, float width, float from, float to, Color color) {
    const Rect& clip = clips_[clip_depth_];
    const float outer = radius + width * 0.5f;
    const Rect box{center.x - outer, center.y - outer, 2 * outer, 2 * outer};
    if (culled(box, 1.0f, clip) || to <= from) {
        return;
    }
    push({{box.x, box.y, box.w, box.h},
          {radius, radius, radius, radius},
          {clip.x, clip.y, clip.w, clip.h},
          {0, 0, 0, 0},
          {width * 0.5f, from, 0, to},
          {pack(color), 0, 0, kArc}});
}

void Renderer::push_clip(const Rect& r) {
    if (clip_depth_ == kMaxClipDepth) {
        ++clip_overflow_; // keeps push/pop balanced; deeper clips are ignored
        WA_ERROR("render", "clip stack deeper than {}", kMaxClipDepth);
        return;
    }
    clips_[clip_depth_ + 1] = intersect(clips_[clip_depth_], r);
    ++clip_depth_;
}

void Renderer::pop_clip() {
    if (clip_overflow_ > 0) {
        --clip_overflow_;
    } else if (clip_depth_ > 0) {
        --clip_depth_;
    }
}

void Renderer::end() {
    flush();
    fences_[segment_].reset(glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0));
    segment_ = (segment_ + 1) % kSegments;
    in_frame_ = false;
    last_ = frame_;
}

void Renderer::set_glyph_atlas(GLuint texture) {
    if (texture == atlas_) {
        return;
    }
    if (in_frame_) {
        flush();
        glBindTextureUnit(0, texture);
    }
    atlas_ = texture;
}

void Renderer::push(const Instance& instance) {
    if (used_ == kSegmentInstances) {
        next_segment();
    }
    Instance& slot = mapped_[segment_ * kSegmentInstances + used_];
    slot = instance;
    if (opacity_ < 1.0f) { // the alpha byte of fill, fill_to and border
        for (int i = 0; i < 3; ++i) {
            const std::uint32_t c = instance.colors[i];
            const auto a = static_cast<std::uint32_t>(static_cast<float>(c >> 24) * opacity_ + 0.5f);
            slot.colors[i] = (c & 0x00FFFFFFu) | (a << 24);
        }
    }
    ++used_;
    ++frame_.instances;
}

void Renderer::flush() {
    const std::uint32_t count = used_ - batch_first_;
    if (count == 0) {
        return;
    }
    glDrawArraysInstancedBaseInstance(GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(count),
                                      segment_ * kSegmentInstances + batch_first_);
    ++frame_.draw_calls;
    batch_first_ = used_;
}

// The frame outgrew its segment: draw what it holds, fence it and continue in the next one.
void Renderer::next_segment() {
    flush();
    fences_[segment_].reset(glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0));
    segment_ = (segment_ + 1) % kSegments;
    wait(fences_[segment_]);
    used_ = 0;
    batch_first_ = 0;
}

void Renderer::bind_image(GLuint texture) {
    if (texture == image_) {
        return;
    }
    if (image_ != 0) {
        flush(); // instances so far sample the previous image
    }
    glBindTextureUnit(1, texture);
    image_ = texture;
}

} // namespace ui::render
