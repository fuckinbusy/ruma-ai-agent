#include "ui/orb.hpp"

#include "ui/theme.hpp"

#include <algorithm>
#include <cmath>

namespace ui {
namespace {

constexpr const char* kVertex = R"(#version 450 core
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
})";

// design/agent-placements.html <script id="fs">, ported to GLSL 4.5. Changes: the output is
// straight alpha (the renderer premultiplies), the .orbc CSS mask is applied here, and the
// raymarch step count is a uniform.
constexpr const char* kFragment = R"(#version 450 core
uniform vec2 uRes;
uniform float uTime, uPhase, uSpike, uEnergy, uHue;
uniform int uSteps;
uniform float uZoom; // 1: the agent's framing; below 1: closer (the installer)
out vec4 o_color;

const vec3 VIOLET = vec3(.49,.36,1.);
const vec3 CYAN   = vec3(.13,.83,.93);
const vec3 PINK   = vec3(1.,.31,.64);
float hash(vec3 p){ p = fract(p*.3183099 + .1); p *= 17.; return fract(p.x*p.y*p.z*(p.x+p.y+p.z)); }
float noise(vec3 x){
  vec3 i = floor(x), f = fract(x); f = f*f*(3.-2.*f);
  return mix(mix(mix(hash(i),hash(i+vec3(1,0,0)),f.x), mix(hash(i+vec3(0,1,0)),hash(i+vec3(1,1,0)),f.x),f.y),
             mix(mix(hash(i+vec3(0,0,1)),hash(i+vec3(1,0,1)),f.x), mix(hash(i+vec3(0,1,1)),hash(i+vec3(1,1,1)),f.x),f.y), f.z);
}
float smin(float a, float b, float k){ float h = clamp(.5+.5*(b-a)/k,0.,1.); return mix(b,a,h) - k*h*(1.-h); }
vec3 hueShift(vec3 c, float a){ const vec3 k = vec3(.57735); float ca = cos(a); return c*ca + cross(k,c)*sin(a) + k*dot(k,c)*(1.-ca); }
vec3 env(vec3 d){
  vec3 c = vec3(.010,.010,.018) + vec3(.035,.035,.06)*(d.y*.5+.5);
  c += vec3(1.)    * 1.7 * smoothstep(.80,.96, dot(d, normalize(vec3(-.45,.75,.55))));
  c += VIOLET      * 1.5 * pow(max(dot(d, normalize(vec3(-1.,.15,.55))),0.), 5.);
  c += CYAN        * 1.4 * pow(max(dot(d, normalize(vec3(1.,-.05,.5))),0.), 5.);
  c += PINK        * .9  * pow(max(dot(d, normalize(vec3(.2,-1.,.45))),0.), 5.);
  c += vec3(.85,.9,1.) * .55 * smoothstep(.035,.0, abs(d.y-.18)) * smoothstep(-.1,.7,d.z);
  return max(hueShift(c, uHue), 0.);
}
float map(vec3 p){
  float t = uPhase;
  float d = length(p) - (.50 - .06*uSpike + .05*uEnergy*sin(uTime*11.));
  float orb = .36 + .20*uSpike;
  for (int i = 0; i < 5; i++){
    float fi = float(i);
    vec3 c = vec3(sin(t*(.7+fi*.23) + fi*2.1), sin(t*(.9+fi*.17) + fi*1.3), cos(t*(.6+fi*.31) + fi*.7)) * orb;
    float r = .19 + .025*fi + .07*uEnergy*max(0., sin(uTime*10. + fi*1.3));
    d = smin(d, length(p - c) - r, .30);
  }
  d += .012*(noise(p*6. + uTime) - .5);
  return d * .8;
}
vec3 shade(vec3 p, vec3 n, vec3 rd){
  float f = .55 + .45*pow(1.-max(dot(n,-rd),0.), 5.);
  vec3 tint = hueShift(vec3(.92,.9,1.), uHue);
  vec3 r = reflect(rd,n);
  vec3 sky = mix(vec3(.05,.05,.08), hueShift(vec3(.55,.5,.9),uHue), smoothstep(-.02,.5,r.y)) * smoothstep(-.03,.0,r.y);
  sky += hueShift(vec3(.2,.6,.75),uHue) * .6 * exp(-abs(r.y+.02)*25.);
  return (env(r) + sky*.8) * tint * mix(.8, 1.05, f);
}
vec3 calcN(vec3 p){
  const vec2 e = vec2(.0015,-.0015);
  return normalize(e.xyy*map(p+e.xyy) + e.yyx*map(p+e.yyx) + e.yxy*map(p+e.yxy) + e.xxx*map(p+e.xxx));
}
void main(){
  vec2 uv = (gl_FragCoord.xy - .5*uRes) / uRes.y * 1.6 * uZoom;
  vec3 ro = vec3(0,0,3.6), rd = normalize(vec3(uv, -1.6));
  float rl = length(uv);
  float b = dot(ro,rd), c = dot(ro,ro) - 1.2*1.2, h = b*b - c;
  bool hit = false; float mind = 9.; vec3 col = vec3(0.); float a = 0.;
  if (h > 0.){
    float t = -b - sqrt(h), tmax = -b + sqrt(h);
    for (int i = 0; i < uSteps; i++){
      vec3 p = ro + rd*t; float d = map(p); mind = min(mind, d);
      if (d < .0008){ hit = true; break; }
      t += d; if (t > tmax) break;
    }
    if (hit){ vec3 p = ro + rd*t; col = shade(p, calcN(p), rd); col = 1. - exp(-col*1.25); col = pow(col, vec3(.92)); a = 1.; }
  }
  if (!hit){
    vec3 gc = hueShift(mix(VIOLET, vec3(.75,.78,1.), .45 + .25*sin(uTime*.5)), uHue);
    float halo = exp(-max(rl - .35, 0.)*4.2) * (.16 + .10*uSpike + .22*uEnergy);
    float rim = exp(-mind*18.) * .25 * step(mind, 1.);
    a = clamp(halo*.7 + rim, 0., 1.);
    col = gc; // the mockup's premultiplied gc * a, as straight alpha
  }
  // .orbc { mask: radial-gradient(closest-side, #000 70%, transparent) }
  float r = length(gl_FragCoord.xy - .5*uRes) / (.5*min(uRes.x, uRes.y));
  float mask = clamp((1. - r) / .3, 0., 1.);
  o_color = vec4(max(col, 0.), a * mask);
})";

// design/ui_model/orb-demo.html <script id="fs">, ported to GLSL 4.5 as the classic one above: the
// same framing (uZoom, the bounding sphere, the steps uniform) and the same .orbc mask; straight
// alpha out. uDrops is the drops split off the core, uLevel the glow (a corona of sparks at 3); the
// light is the theme's, whatever the model. The halo and the corona are in the ray's units, as the demo's.
constexpr const char* kFragmentModel = R"(#version 450 core
uniform vec2 uRes;
uniform float uTime, uPhase, uSpike, uEnergy, uDrops, uLevel;
uniform int uSteps;
uniform float uZoom;
// The theme's light (design/ui_palette). uAmb: how much of the surroundings lights the dark side
// (a light theme).
uniform vec3 uL1, uL2, uL3, uTint, uGlow;
uniform float uAmb;
out vec4 o_color;

float hash(vec3 p){ p = fract(p*.3183099 + .1); p *= 17.; return fract(p.x*p.y*p.z*(p.x+p.y+p.z)); }
float noise(vec3 x){
  vec3 i = floor(x), f = fract(x); f = f*f*(3.-2.*f);
  return mix(mix(mix(hash(i),hash(i+vec3(1,0,0)),f.x), mix(hash(i+vec3(0,1,0)),hash(i+vec3(1,1,0)),f.x),f.y),
             mix(mix(hash(i+vec3(0,0,1)),hash(i+vec3(1,0,1)),f.x), mix(hash(i+vec3(0,1,1)),hash(i+vec3(1,1,1)),f.x),f.y), f.z);
}
float smin(float a, float b, float k){ float h = clamp(.5+.5*(b-a)/k,0.,1.); return mix(b,a,h) - k*h*(1.-h); }
vec3 L1(){ return uL1; }
vec3 L2(){ return uL2; }
vec3 L3(){ return uL3; }
vec3 T(){ return uTint; }
vec3 env(vec3 d){
  vec3 c = vec3(.010,.010,.018) + vec3(.035,.035,.06)*(d.y*.5+.5) + uAmb*mix(T(), vec3(1.), .4)*(d.y*.35+.55);
  c += vec3(1.) * 1.7 * smoothstep(.80,.96, dot(d, normalize(vec3(-.45,.75,.55))));
  c += L1() * 1.5 * pow(max(dot(d, normalize(vec3(-1.,.15,.55))),0.), 5.);
  c += L2() * 1.4 * pow(max(dot(d, normalize(vec3(1.,-.05,.5))),0.), 5.);
  c += L3() * .9  * pow(max(dot(d, normalize(vec3(.2,-1.,.45))),0.), 5.);
  c += vec3(.85,.9,1.) * .55 * smoothstep(.035,.0, abs(d.y-.18)) * smoothstep(-.1,.7,d.z);
  return c;
}
float map(vec3 p){
  float t = uPhase;
  float solo = 1. - smoothstep(0., 2., uDrops);
  float d = length(p) - (.46 + .12*solo - .06*uSpike + .05*uEnergy*sin(uTime*11.));
  float orb = .46 + .16*uSpike;
  for (int i = 0; i < 5; i++){
    float fi = float(i);
    float s = clamp(uDrops - fi, 0., 1.);
    if (s <= 0.) continue;
    vec3 c = vec3(sin(t*(.7+fi*.23) + fi*2.1), sin(t*(.9+fi*.17) + fi*1.3), cos(t*(.6+fi*.31) + fi*.7)) * orb * mix(.35, 1., s);
    float r = (.145 + .018*fi) * mix(.55, 1., s) + .07*uEnergy*max(0., sin(uTime*10. + fi*1.3));
    d = mix(d, smin(d, length(p - c) - r, .22), s);
  }
  d += .012*(noise(p*6. + uTime) - .5);
  return d * .8;
}
vec3 shade(vec3 p, vec3 n, vec3 rd){
  float f = .55 + .45*pow(1.-max(dot(n,-rd),0.), 5.);
  vec3 tint = T();
  vec3 r = reflect(rd,n);
  vec3 sky = mix(vec3(.05,.05,.08), mix(uL1,uL2,.5)*.75, smoothstep(-.02,.5,r.y)) * smoothstep(-.03,.0,r.y);
  sky += uL2 * .5 * exp(-abs(r.y+.02)*25.);
  return (env(r) + sky*.8) * tint * mix(.8, 1.05, f);
}
vec3 calcN(vec3 p){
  const vec2 e = vec2(.0015,-.0015);
  return normalize(e.xyy*map(p+e.xyy) + e.yyx*map(p+e.yyx) + e.yxy*map(p+e.yxy) + e.xxx*map(p+e.xxx));
}
void main(){
  vec2 uv = (gl_FragCoord.xy - .5*uRes) / uRes.y * 1.6 * uZoom;
  vec3 ro = vec3(0,0,3.6), rd = normalize(vec3(uv, -1.6));
  float rl = length(uv);
  float b = dot(ro,rd), c = dot(ro,ro) - 1.25*1.25, h = b*b - c;
  bool hit = false; float mind = 9.; vec3 col = vec3(0.); float a = 0.;
  if (h > 0.){
    float t = -b - sqrt(h), tmax = -b + sqrt(h);
    for (int i = 0; i < uSteps; i++){
      vec3 p = ro + rd*t; float d = map(p); mind = min(mind, d);
      if (d < .0008){ hit = true; break; }
      t += d; if (t > tmax) break;
    }
    if (hit){ vec3 p = ro + rd*t; col = shade(p, calcN(p), rd); col = 1. - exp(-col*1.25); col = pow(col, vec3(.92)); a = 1.; }
  }
  if (!hit){
    vec3 gc = uGlow * (.9 + .1*sin(uTime*.5));
    float halo = exp(-max(rl - .23, 0.)*6.4) * (.12 + .035*uLevel + .10*uSpike + .22*uEnergy);
    float rim = exp(-mind*18.) * .25 * step(mind, 1.);
    float lv3 = smoothstep(2.,3.,uLevel);
    float ang = atan(uv.y, uv.x);
    float sp = pow(.5+.5*cos(ang*7. - uPhase*1.9), 26.) + .7*pow(.5+.5*cos(ang*4. + uPhase*1.3 + 1.7), 34.);
    float band = exp(-abs(rl - (.445 + .007*sin(uTime*2.3)))*110.);
    float cor = lv3 * (sp*band*1.1 + band*.10);
    float glow = clamp(halo*.7 + rim, 0., 1.);
    a = clamp(glow + cor, 0., 1.);
    // The demo's premultiplied colour, as straight alpha.
    col = (gc * glow + mix(uGlow, vec3(1.), .6) * cor) / max(a, 1e-4);
  }
  float r = length(gl_FragCoord.xy - .5*uRes) / (.5*min(uRes.x, uRes.y));
  float mask = clamp((1. - r) / .3, 0., 1.);
  o_color = vec4(max(col, 0.), a * mask);
})";

struct MoodTargets {
    float spike, speed, hue;
};

// ORB_STATES in the mockup (the classic style).
constexpr auto targets(Orb::Mood mood) noexcept -> MoodTargets {
    switch (mood) {
    case Orb::Mood::Thinking: return {1.0f, 1.6f, 0.32f};
    case Orb::Mood::Answering: return {0.55f, 0.8f, -0.22f};
    case Orb::Mood::Idle: break;
    }
    return {0.05f, 0.35f, 0.0f};
}

// The model's style: the level's speeds and spike (design/ui_model README); the colour never moves.
auto model_targets(Orb::Mood mood, const orb_look::Energy& e) noexcept -> MoodTargets {
    switch (mood) {
    case Orb::Mood::Thinking: return {e.think_spike, e.think_speed, 0.0f};
    case Orb::Mood::Answering: return {0.55f, std::max(0.8f, e.rest_speed), 0.0f};
    case Orb::Mood::Idle: break;
    }
    return {0.05f, e.rest_speed, 0.0f};
}

} // namespace

auto Orb::create(gl::ProgramCache& cache, int size, Style style) -> core::Result<Orb> {
    auto program = style == Style::Model ? cache.build("orb-model", kVertex, kFragmentModel) : cache.build("orb", kVertex, kFragment);
    if (!program) {
        return std::unexpected{program.error()};
    }
    int levels = 1;
    while ((size >> levels) > 0) {
        ++levels;
    }
    auto target = render::Target::create(size, size, levels);
    if (!target) {
        return std::unexpected{target.error()};
    }
    // Drawn several times smaller than rendered (supersampling): trilinear sampling of the mip
    // levels averages the texels a screen pixel covers.
    glTextureParameteri(target->texture(), GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTextureParameteri(target->texture(), GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(target->texture(), GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(target->texture(), GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GLuint vao = 0;
    glCreateVertexArrays(1, &vao);
    Orb orb{std::move(*program), gl::VertexArray{vao}, std::move(*target), style};
    const GLuint p = orb.program_.get();
    orb.u_ = {glGetUniformLocation(p, "uRes"),   glGetUniformLocation(p, "uTime"),
              glGetUniformLocation(p, "uPhase"), glGetUniformLocation(p, "uSpike"),
              glGetUniformLocation(p, "uEnergy"), glGetUniformLocation(p, "uHue"),
              glGetUniformLocation(p, "uSteps"),  glGetUniformLocation(p, "uZoom"),
              glGetUniformLocation(p, "uDrops"),
              glGetUniformLocation(p, "uLevel"),  glGetUniformLocation(p, "uL1"),
              glGetUniformLocation(p, "uL2"),     glGetUniformLocation(p, "uL3"),
              glGetUniformLocation(p, "uTint"),   glGetUniformLocation(p, "uGlow"),
              glGetUniformLocation(p, "uAmb")}; // a style's missing uniforms are -1: their sets do nothing
    return orb;
}

void Orb::pulse(float strength) noexcept {
    energy_target_ = std::min(motion::orb::kPulseMost, energy_target_ + motion::orb::kPulseEnergy * strength);
}

void Orb::set_params(const Params& params) noexcept {
    params_ = params;
    spike_.snap(params.spike);
    speed_.snap(params.speed);
    hue_.snap(params.hue);
    energy_.snap(params.energy);
    energy_target_ = params.energy;
}

// The mood's parameters on orb-token springs and the energy on a quick one (motion rework, MS § 4):
// the same at any frame rate, a frame's step held to kMaxFrameDt (a stalled window does not leap).
void Orb::update(double now) noexcept {
    if (!epoch_) {
        epoch_ = now;
    }
    if (!last_) {
        last_ = now;
        return;
    }
    const auto dt = static_cast<float>(std::clamp(now - *last_, 0.0, static_cast<double>(motion::kMaxFrameDt)));
    last_ = now;
    const MoodTargets t = style_ == Style::Model ? model_targets(mood_, energy_look_) : targets(mood_);
    spike_.set(t.spike, motion::kOrb);
    speed_.set(t.speed, motion::kOrb);
    hue_.set(t.hue, motion::kOrb);
    energy_target_ *= std::exp(-dt * motion::orb::kPulseFade);
    energy_.set(energy_target_, motion::kQuick);
    spike_.step(dt);
    speed_.step(dt);
    hue_.step(dt);
    energy_.step(dt);
    params_.spike = spike_.x;
    params_.speed = speed_.x;
    params_.hue = hue_.x;
    params_.energy = energy_.x;
    params_.phase += (motion::orb::kPhaseBase + params_.speed * motion::orb::kPhasePerSpeed) * dt;
}

auto Orb::still() const -> core::Result<render::Target> {
    const int size = target_.width();
    int levels = 1;
    while ((size >> levels) > 0) {
        ++levels;
    }
    auto made = render::Target::create(size, size, levels);
    if (!made) {
        return std::unexpected{made.error()};
    }
    glTextureParameteri(made->texture(), GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTextureParameteri(made->texture(), GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(made->texture(), GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(made->texture(), GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    made->bind();
    target_.copy_to_bound();
    made->generate_mipmaps();
    return made;
}

void Orb::render(double now) {
    const GLuint p = program_.get();
    const auto side = static_cast<float>(target_.width());
    target_.bind();
    target_.clear();
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(p);
    glProgramUniform2f(p, u_.res, side, side);
    // From the orb's first frame, not from the system's start: a float of days of seconds steps
    // the shader's noise (motion-gap-map risk 50).
    if (!epoch_) {
        epoch_ = now;
    }
    glProgramUniform1f(p, u_.time, static_cast<float>(now - *epoch_));
    glProgramUniform1f(p, u_.phase, params_.phase);
    const bool model = style_ == Style::Model;
    glProgramUniform1f(p, u_.spike, params_.spike);
    glProgramUniform1f(p, u_.energy, params_.energy);
    glProgramUniform1f(p, u_.hue, params_.hue);
    glProgramUniform1i(p, u_.steps, kMaxSteps);
    glProgramUniform1f(p, u_.zoom, zoom_);
    if (model) {
        glProgramUniform1f(p, u_.drops, look_.look.drops);
        glProgramUniform1f(p, u_.level, look_.look.level);
        // The theme of this frame (a change of theme moves the orb with the rest).
        const auto& o = th().orb;
        const auto rgb = [&](GLint at, ::theme::Color c) {
            glProgramUniform3f(p, at, static_cast<float>(c.r) / 255.0f, static_cast<float>(c.g) / 255.0f, static_cast<float>(c.b) / 255.0f);
        };
        rgb(u_.l1, o.l1);
        rgb(u_.l2, o.l2);
        rgb(u_.l3, o.l3);
        rgb(u_.tint, o.tint);
        rgb(u_.glow, o.glow);
        glProgramUniform1f(p, u_.amb, o.amb);
    }
    glBindVertexArray(vao_.get());
    glDrawArrays(GL_TRIANGLES, 0, 3);
    target_.generate_mipmaps();
}

} // namespace ui
