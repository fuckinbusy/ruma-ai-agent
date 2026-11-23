#include "setup/view/painter.hpp"

#include "core/hash.hpp"
#include "ui/theme.hpp"
#include "ui/work.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <tuple>

namespace setup::view {
namespace {

using ::ui::Face;
using ::ui::layout::Box;
using ::ui::render::Color;

// `c` at `share` of its own alpha.
auto faint(Color c, float share) noexcept -> Color {
    c.a = static_cast<std::uint8_t>(static_cast<float>(c.a) * std::clamp(share, 0.0f, 1.0f) + 0.5f);
    return c;
}

// design/ui_installer/installer.html :root.
constexpr Color kCloseHover{196, 43, 28, 255};   // color-ok: Windows 11's own caption close button under the mouse

constexpr char32_t kIconMinimize = 0xE921;
constexpr char32_t kIconClose = 0xE8BB;
constexpr char32_t kIconFolder = 0xE8B7;
constexpr char32_t kIconKeyboard = 0xE765;
constexpr char32_t kIconPower = 0xE7E8;
constexpr char32_t kIconDesktop = 0xE7F4;
constexpr char32_t kIconBack = 0xE76B;
constexpr char32_t kIconTranslate = 0xE8C1;
constexpr char32_t kIconPhoto = 0xEB9F;
constexpr char32_t kIconCheck = 0xE73E;

auto id_of(std::string_view name) -> ::ui::Id {
    return core::hash::fnv1a64(name, core::hash::fnv1a64("setup/view"));
}

auto all(float r) -> ::ui::render::Corners {
    return {r, r, r, r};
}

auto rect(const Box& b) -> ::ui::render::Rect {
    return {b.x, b.y, b.w, b.h};
}

auto megabytes(std::uint64_t bytes) -> std::uint64_t {
    return (bytes + (1ull << 20) - 1) >> 20;
}

// "C:" of a path; "" when it has no drive.
auto drive_of(const std::filesystem::path& p) -> std::string {
    const auto root = p.root_name().native();
    std::string out;
    for (const wchar_t c : root)
        out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    return out;
}

auto narrow(const std::filesystem::path& p) -> std::string {
    const auto u8 = p.u8string();
    return {reinterpret_cast<const char*>(u8.data()), u8.size()};
}

auto step_doing(Step s) -> std::string_view {
    switch (s) {
    case Step::Files: return "Copying files";
    case Step::Shortcuts: return "Creating shortcuts";
    case Step::Settings: return "Saving settings";
    }
    return "";
}

} // namespace

auto Painter::place_of(Screen s) -> Place {
    switch (s) {
    case Screen::Options: return {75.0f, 0.42f};
    case Screen::Installing:
    case Screen::Removing: return {180.0f, 0.66f};
    default: return {190.0f, 1.0f};
    }
}

void Painter::update(const State& s, double now) {
    const auto place = place_of(s.screen);
    const float want_d = kOrbBody * place.scale;
    if (!placed_ || last_ < 0) {
        orb_y_spring_.snap(place.y);
        orb_d_spring_.snap(want_d);
        placed_ = true;
        shown_screen_ = s.screen; // first seen: the screen and its progress as they are
        progress_.snap(s.progress);
    } else {
        // Between screens the orb goes to its place and its size on smooth (the map's unnamed row;
        // it glided by an exponential tail), a frame's time at most a step.
        const auto dt = static_cast<float>(std::clamp(now - last_, 0.0, static_cast<double>(::ui::motion::kMaxFrameDt)));
        orb_y_spring_.set(place.y, ::ui::motion::kSmooth);
        orb_d_spring_.set(want_d, ::ui::motion::kSmooth);
        orb_y_spring_.step(dt);
        orb_d_spring_.step(dt);
        tick_ += dt;
        if (s.screen != shown_screen_) { // another screen: the one it replaces is kept as a picture, going
            leaving_ = s;
            leaving_->screen = shown_screen_;
            shown_screen_ = s.screen;
            screen_at_ = tick_ - dt; // it changed within this frame
            progress_.snap(s.progress); // a screen that comes shows its progress as it is: it counts from there
        }
        progress_.set(s.progress, ::ui::motion::kSmooth);
        progress_.step(dt);
    }
    {
        namespace mt = ::ui::motion;
        const double since = tick_ - screen_at_;
        const double in_since = since - std::chrono::duration<double>(mt::installer::kScreenInAt).count();
        screen_motion_.out = leaving_ ? 1.0f - mt::response(mt::kMicro, since) : 0.0f;
        screen_motion_.in = mt::response(mt::kQuick, in_since);
        screen_motion_.lift = reduced_ ? 0.0f : mt::installer::kScreenLift * (1.0f - mt::response(mt::kBase, in_since));
        if (leaving_ && screen_motion_.out <= 0.0f) {
            leaving_.reset(); // gone
        }
    }
    orb_y_ = orb_y_spring_.x;
    orb_d_ = orb_d_spring_.x;
    last_ = now;
    now_ = now;
    const bool working = s.screen == Screen::Installing || s.screen == Screen::Removing;
    orb_->set_mood(working ? ::ui::Orb::Mood::Thinking : ::ui::Orb::Mood::Idle);
    orb_->update(now);
}

void Painter::render_orb(double now) {
    orb_->render(now);
}

void Painter::text(Face face, float size, std::string_view t, float x, float baseline, Color c) {
    texts_.emplace_back(t);
    ui_->draw_text(face, ui_->shape(face, t, size), size, {x, baseline}, c);
}

void Painter::centred(Face face, float size, std::string_view t, float cx, float top, float height, Color c) {
    const float w = width(face, size, t);
    text(face, size, t, cx - w / 2, ui_->centred_baseline(face, size, top, height), c);
}

auto Painter::width(Face face, float size, std::string_view t) -> float {
    return ::ui::work::text_width(*ui_, face, size, t);
}

void Painter::target(std::string_view name, const Box& box) {
    targets_.push_back({std::string{name}, box});
}

auto Painter::button(std::string_view name, const Box& box, std::string_view label, bool primary, bool enabled,
                     bool crit) -> bool {
    target(name, box);
    const auto style = crit ? ::ui::work::ButtonStyle::Crit
                            : primary ? ::ui::work::ButtonStyle::Primary : ::ui::work::ButtonStyle::Normal;
    const bool clicked = ::ui::work::button(*ui_, id_of(name), box, "", style, box.h < 40, enabled);
    const bool small = box.h < 40;
    const float size = small ? 12.5f : 14.0f;
    const Color ink = primary && !crit ? ::ui::th().on_accent : ::ui::th().text;
    const Color shown = enabled ? ink : Color{ink.r, ink.g, ink.b, 110};
    const float pad = small ? 12.0f : primary ? 26.0f : 20.0f;
    text(Face::Semibold, size, label, box.x + pad, ui_->centred_baseline(Face::Semibold, size, box.y, box.h), shown);
    return clicked && enabled;
}

// The screen's main button at the footer's right (`right` its right edge), with its Enter key.
auto Painter::main_button(std::string_view name, float right, float y, std::string_view label, bool enabled, bool crit)
    -> std::pair<bool, float> {
    const float label_w = width(Face::Semibold, 14.0f, label);
    const float kbd_w = std::max(26.0f, width(Face::Semibold, 11.5f, "Enter") + 12.0f);
    const float w = 26.0f + label_w + 8.0f + kbd_w + 26.0f;
    const Box box{right - w, y, w, 44.0f};
    const bool clicked = button(name, box, label, true, enabled, crit);
    const Box kbd{box.x + 26.0f + label_w + 8.0f, y + 11.0f, kbd_w, 22.0f};
    auto& r = ui_->renderer();
    const Color edge = crit ? ::ui::th().crit_line : faint(::ui::th().on_accent, 0.15f); // on the soft crit, on the send gradient
    r.rect(rect(kbd), {.radius = all(6.0f), .border = edge, .border_width = 1.0f});
    const Color ink = crit ? ::ui::th().crit : faint(::ui::th().on_accent, 0.7f);
    centred(Face::Semibold, 11.5f, "Enter", kbd.x + kbd.w / 2, kbd.y, kbd.h, ink);
    return {clicked, box.x};
}

void Painter::draw_window(const Box& p) {
    auto& r = ui_->renderer();
    // .win: 0 40px 90px -20px, 0 12px 30px -10px — in the theme's shadow colours
    r.shadow({p.x + 20, p.y + 40 + 20, p.w - 40, p.h - 40}, kRadius, 90.0f, ::ui::th().shadow[0].color);
    r.shadow({p.x + 10, p.y + 12 + 10, p.w - 20, p.h - 20}, kRadius, 30.0f, ::ui::th().shadow[1].color);
    r.rect(rect(p), {.fill = ::ui::th().surface, .radius = all(kRadius), .border = ::ui::th().line, .border_width = 1.0f});
    // .glow: a soft violet behind the orb (radial 124,110,255 .16 → 80,120,255 .05).
    r.push_clip({p.x + 1, p.y + 1, p.w - 2, p.h - kFooter});
    const float cy = p.y + orb_y_;
    r.shadow({p.x + p.w / 2 - 150, cy - 110, 300, 220}, 110.0f, 150.0f, ::ui::th().accent_soft_hi);
    r.pop_clip();
}

void Painter::draw_top(const Box& p, Reaction& r) {
    text(Face::Text, 12.5f, "WinAiAgent Setup", p.x + 18, ui_->centred_baseline(Face::Text, 12.5f, p.y, kTopBar), ::ui::th().text_3);
    const Box close{p.x + p.w - 6 - 36, p.y + 7, 36, 30};
    const Box minimize{close.x - 36, p.y + 7, 36, 30};
    drag_ = {p.x, p.y, minimize.x - p.x, kTopBar};
    auto& rr = ui_->renderer();
    for (const auto& [name, box, icon] : {std::tuple{"minimize", minimize, kIconMinimize}, std::tuple{"close", close, kIconClose}}) {
        target(name, box);
        const auto hit = ui_->interact(id_of(name), box, false);
        const float h = ui_->hover(id_of(name), hit.hovered);
        const bool x = std::string_view{name} == "close";
        if (h > 0.01f) {
            const Color fill = x ? Color{kCloseHover.r, kCloseHover.g, kCloseHover.b, static_cast<std::uint8_t>(255 * h)}
                                 : faint(::ui::th().w2, h);
            rr.rect(rect(box), {.fill = fill, .radius = all(7.0f)});
        }
        const Color ink = h > 0.5f ? ::ui::th().text : ::ui::th().text_3;
        ui_->draw_icon(icon, 10.0f, {box.x + box.w / 2, box.y + box.h / 2}, ink);
        if (hit.clicked) {
            if (x)
                r.action = Action::Escape;
            else
                minimize_ = true;
        }
    }
}

void Painter::draw_footer_line(const Box& p) {
    auto& r = ui_->renderer();
    const float y = p.y + p.h - kFooter;
    r.rect({p.x + 1, y, p.w - 2, kFooter - 1}, {.fill = ::ui::th().w1, .radius = {0, 0, kRadius - 1, kRadius - 1}});
    r.rect({p.x + 1, y, p.w - 2, 1}, {.fill = ::ui::th().line});
}

void Painter::draw_orb(const Box& p) {
    const float d = orb_d_ * kOrbQuad; // the texture holds the body with its halo around it
    const float cx = p.x + p.w / 2;
    const float cy = p.y + orb_y_;
    ui_->renderer().image({cx - d / 2, cy - d / 2, d, d}, ::ui::Orb::uv(), orb_->texture(), ::ui::kOnPicture); // its own colours
}

void Painter::draw_ring(const Box& p, float progress) {
    auto& r = ui_->renderer();
    const ::ui::render::Point c{p.x + p.w / 2, p.y + 180.0f};
    constexpr float kR = 156.0f;
    const float to = 2 * std::numbers::pi_v<float> * std::clamp(progress, 0.0f, 1.0f);
    r.arc(c, kR, 3.0f, 0, 2 * std::numbers::pi_v<float>, ::ui::th().w2);
    if (to > 0.001f) {
        r.arc(c, kR, 7.0f, 0, to, ::ui::th().accent_soft_hi);   // its soft glow
        r.arc(c, kR, 3.0f, 0, to, ::ui::th().accent);
        const ::ui::render::Point head{c.x + kR * std::sin(to), c.y - kR * std::cos(to)};
        r.rect({head.x - 5, head.y - 5, 10, 10}, {.fill = ::ui::th().text, .radius = all(5.0f)}); // its head
    }
}

void Painter::chips(const Box& p, float top) {
    struct Chip {
        char32_t icon;
        std::string_view label;
    };
    constexpr Chip items[] = {{kIconFolder, "Files & system"}, {kIconTranslate, "Translation"}, {kIconPhoto, "Images & docs"}};
    float total = 0;
    float widths[3]{};
    for (int i = 0; i < 3; ++i) {
        widths[i] = 12 + 14 + 7 + width(Face::Text, 12.5f, items[i].label) + 12;
        total += widths[i] + (i > 0 ? 8.0f : 0.0f);
    }
    float x = p.x + (p.w - total) / 2;
    auto& r = ui_->renderer();
    for (int i = 0; i < 3; ++i) {
        const Box b{x, top, widths[i], 27.0f};
        r.rect(rect(b), {.fill = ::ui::th().w1, .radius = all(13.5f), .border = ::ui::th().line, .border_width = 1.0f});
        ui_->draw_icon(items[i].icon, 13.0f, {b.x + 12 + 7, b.y + b.h / 2}, ::ui::th().accent);
        text(Face::Text, 12.5f, items[i].label, b.x + 12 + 14 + 7, ui_->centred_baseline(Face::Text, 12.5f, b.y, b.h), ::ui::th().text_2);
        x += widths[i] + 8;
    }
}

// "Ctrl+Alt+Space" as keys: small (.kbd) on Options, big (.bk) with a "+" between on Done.
void Painter::keys(std::string_view hotkey, float cx, float top, float height, bool big) {
    std::vector<std::string_view> parts;
    for (std::size_t at = 0; at <= hotkey.size();) {
        const auto plus = hotkey.find('+', at);
        parts.push_back(hotkey.substr(at, plus == std::string_view::npos ? std::string_view::npos : plus - at));
        if (plus == std::string_view::npos)
            break;
        at = plus + 1;
    }
    const float size = big ? 17.0f : 11.5f;
    const float pad = big ? 18.0f : 6.0f;
    const float min_w = big ? 64.0f : 26.0f;
    const float h = big ? 52.0f : 22.0f;
    const float gap = big ? 10.0f + width(Face::Text, 18.0f, "+") + 10.0f : 5.0f;
    std::vector<float> widths;
    float total = 0;
    for (const auto part : parts) {
        widths.push_back(std::max(min_w, width(Face::Semibold, size, part) + 2 * pad));
        total += widths.back();
    }
    total += gap * static_cast<float>(parts.size() - 1);
    float x = big ? cx - total / 2 : cx - total; // small keys end at cx
    const float y = top + (height - h) / 2;
    auto& r = ui_->renderer();
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const Box b{x, y, widths[i], h};
        if (big) {
            r.shadow({b.x, b.y + 8, b.w, b.h}, 12.0f, 20.0f, ::ui::th().pop_shadow[0].color);
            r.rect(rect(b), {.fill = ::ui::th().w3, .fill_to = ::ui::th().w1,
                             .gradient_angle = std::numbers::pi_v<float> / 2, .radius = all(12.0f),
                             .border = ::ui::th().w4, .border_width = 1.0f});
        } else {
            r.rect(rect(b), {.radius = all(6.0f), .border = ::ui::th().w4, .border_width = 1.0f});
        }
        centred(Face::Semibold, size, parts[i], b.x + b.w / 2, b.y, b.h, big ? ::ui::th().text : ::ui::th().text_2);
        x += widths[i];
        if (big && i + 1 < parts.size())
            centred(Face::Text, 18.0f, "+", x + gap / 2, b.y, b.h, ::ui::th().text_3);
        x += gap;
    }
}

void Painter::draw_welcome(const State& s, const Box& p, Reaction& r) {
    const bool update = s.screen == Screen::Update;
    const float cx = p.x + p.w / 2;
    centred(Face::Display, 32.0f, "WinAiAgent", cx, p.y + 384 + part(1), 37, ::ui::th().text);
    const auto sub = update ? std::format("Version {} is installed \xE2\x80\x94 this updates it to {}", s.installed_version, s.version)
                            : std::string{"An AI agent that lives at the top of your screen"};
    centred(Face::Text, 15.0f, sub, cx, p.y + 437 + part(2), 22.5f, ::ui::th().text_2);
    chips(p, p.y + 488 + part(3));
    const auto meta = !s.note.empty() ? s.note
                      : update        ? std::string{"Your settings, history and key stay as they are"}
                                      : std::format("Version {} \xC2\xB7 {} MB \xC2\xB7 {}", s.version, megabytes(s.needed), s.system);
    centred(Face::Text, 12.0f, meta, cx, p.y + 536 + part(4), 18, s.note.empty() ? ::ui::th().text_4 : ::ui::th().text_2);

    const float fy = p.y + p.h - kFooter + part(5); // the foot: its line, its words and its buttons together
    const float by = fy + 20;
    // "By installing you accept the License": the link opens the license.
    const std::string_view lead = "By installing you accept the ";
    const float base = ui_->centred_baseline(Face::Text, 12.5f, fy, kFooter);
    text(Face::Text, 12.5f, lead, p.x + 24, base, ::ui::th().text_3);
    const float lx = p.x + 24 + width(Face::Text, 12.5f, lead);
    const Box link{lx, fy + 30, width(Face::Text, 12.5f, "License"), 24};
    target("license", link);
    const auto hit = ui_->interact(id_of("license"), link, false);
    if (hit.hovered)
        ui_->want_cursor(::ui::Cursor::Hand);
    text(Face::Text, 12.5f, "License", lx, base, ::ui::th().accent);
    if (hit.clicked)
        r.action = Action::License;

    const auto [install, left] = main_button("primary", p.x + p.w - 22, by, update ? "Update" : "Install");
    if (install)
        r.action = Action::Enter;
    if (!update) {
        const float w = 20 + width(Face::Semibold, 14.0f, "Options") + 20;
        if (button("options", {left - 10 - w, by, w, 44}, "Options", false))
            r.action = Action::Options;
    }
}

void Painter::draw_options(const State& s, const Box& p, Reaction& r) {
    const float cx = p.x + p.w / 2;
    centred(Face::Display, 22.0f, "Options", cx, p.y + 122, 25, ::ui::th().text);
    centred(Face::Text, 13.0f, "All of this can be changed later in Settings", cx, p.y + 152, 18, ::ui::th().text_3);

    auto& rr = ui_->renderer();
    const Box card{p.x + 24, p.y + 212, p.w - 48, 4 * 58.0f + 3};
    rr.rect(rect(card), {.fill = ::ui::th().w1, .radius = all(14.0f), .border = ::ui::th().line, .border_width = 1.0f});
    const auto row = [&](int i, char32_t icon, std::string_view title, std::string_view desc, bool mono) {
        const float y = card.y + static_cast<float>(i) * 58.0f + static_cast<float>(i);
        if (i > 0)
            rr.rect({card.x + 1, y - 1, card.w - 2, 1}, {.fill = ::ui::th().line});
        const Box ic{card.x + 14, y + 14, 30, 30};
        rr.rect(rect(ic), {.fill = ::ui::th().accent_soft, .radius = all(8.0f)});
        ui_->draw_icon(icon, 15.0f, {ic.x + 15, ic.y + 15}, ::ui::th().accent);
        const float tx = ic.x + 30 + 12;
        text(Face::Semibold, 13.5f, title, tx, y + 26, ::ui::th().text);
        const float room = card.x + card.w - 14 - 150 - tx;
        texts_.emplace_back(desc);
        ::ui::work::label(*ui_, {tx, y + 32, room, 16}, mono ? Face::Mono : Face::Text, mono ? 11.5f : 12.0f, desc, ::ui::th().text_3);
        return y;
    };
    const auto change = [&](std::string_view name, float y, Action a) {
        const float w = 12 + width(Face::Semibold, 12.5f, "Change") + 12;
        const Box b{card.x + card.w - 14 - w, y + 14, w, 30};
        if (button(name, b, "Change", false))
            r.action = a;
        return b.x;
    };
    const auto toggle = [&](std::string_view name, float y, bool on, Action a) {
        const Box b{card.x + card.w - 14 - 40, y + 18, 40, 22};
        target(name, b);
        if (::ui::work::toggle(*ui_, id_of(name), b, on))
            r.action = a;
    };

    const auto dir = narrow(s.choices.dir);
    float y = row(0, kIconFolder, "Install location", dir, true);
    {   // Program Files, for everyone (installer-program-files.md): no Change.
        constexpr std::string_view kEveryone = "For everyone on this computer";
        const float w = width(Face::Text, 12.0f, kEveryone);
        text(Face::Text, 12.0f, kEveryone, card.x + card.w - 14 - w, y + 34, ::ui::th().text_3);
    }
    y = row(1, kIconKeyboard, "Hotkey to call the agent", "Press it anywhere \xE2\x80\x94 the bar drops from the top", false);
    const float kx = change("hotkey", y, Action::ChangeHotkey);
    keys(s.choices.hotkey, kx - 6, y + 14, 30, false);
    y = row(2, kIconPower, "Start with Windows", "Runs quietly in the background, ~40 MB RAM", false);
    toggle("autostart", y, s.choices.autostart, Action::ToggleAutostart);
    y = row(3, kIconDesktop, "Desktop shortcut", "The hotkey is usually enough", false);
    toggle("desktop", y, s.choices.desktop, Action::ToggleDesktop);

    if (!s.folder.problem.empty())
        centred(Face::Semibold, 13.0f, s.folder.problem, cx, p.y + 500, 20, ::ui::th().crit);

    // .disk: what it needs against what is free.
    const float dy = p.y + 542;
    const auto needs = std::format("Needs {} MB", megabytes(s.needed));
    const auto free = std::format("{} GB free on {}", s.folder.free >> 30, drive_of(s.choices.dir));
    const float nw = width(Face::Text, 12.0f, needs);
    const float fw = width(Face::Text, 12.0f, free);
    text(Face::Text, 12.0f, needs, p.x + 24, ui_->centred_baseline(Face::Text, 12.0f, dy, 16), ::ui::th().text_3);
    text(Face::Text, 12.0f, free, p.x + p.w - 24 - fw, ui_->centred_baseline(Face::Text, 12.0f, dy, 16), ::ui::th().text_3);
    const Box bar{p.x + 24 + nw + 12, dy + 5, p.w - 48 - nw - fw - 24, 6};
    rr.rect(rect(bar), {.fill = ::ui::th().w2, .radius = all(3.0f)});
    if (s.folder.total > 0) {
        const float used = static_cast<float>(s.folder.total - std::min(s.folder.free, s.folder.total)) /
                           static_cast<float>(s.folder.total);
        const float mine = static_cast<float>(s.needed) / static_cast<float>(s.folder.total);
        rr.rect({bar.x, bar.y, bar.w * used, bar.h}, {.fill = ::ui::th().w4, .radius = {3, 0, 0, 3}});
        rr.rect({bar.x + bar.w * used, bar.y, std::max(3.0f, bar.w * mine), bar.h}, {.fill = ::ui::th().accent});
    }

    const float by = p.y + p.h - kFooter + 20;
    const float bw = 20 + 16 + 8 + width(Face::Semibold, 14.0f, "Back") + 20;
    const Box back{p.x + 24, by, bw, 44};
    target("back", back);
    const bool back_clicked = ::ui::work::button(*ui_, id_of("back"), back, "", ::ui::work::ButtonStyle::Normal);
    ui_->draw_icon(kIconBack, 12.0f, {back.x + 20 + 8, back.y + 22}, ::ui::th().text);
    text(Face::Semibold, 14.0f, "Back", back.x + 20 + 16 + 8, ui_->centred_baseline(Face::Semibold, 14.0f, back.y, 44), ::ui::th().text);
    if (back_clicked)
        r.action = Action::Back;
    if (main_button("primary", p.x + p.w - 22, by, "Install", can_install(s)).first)
        r.action = Action::Enter;
}

void Painter::draw_working(const State& s, const Box& p, Reaction& r) {
    const bool removing = s.screen == Screen::Removing;
    draw_ring(p, progress_.x); // the ring and the percent: the progress on its way (smooth) — the number counts
    const float cx = p.x + p.w / 2;
    std::array<char, 8> pct_buf{};
    const auto pct_end = std::format_to_n(pct_buf.data(), pct_buf.size(), "{}",
                                          static_cast<int>(std::lround(std::clamp(progress_.x, 0.0f, 1.0f) * 100)));
    const std::string_view pct{pct_buf.data(), static_cast<std::size_t>(pct_end.out - pct_buf.data())};
    const float pw = width(Face::Display, 40.0f, pct);
    const float sw = width(Face::Display, 20.0f, "%");
    const float px = cx - (pw + 2 + sw) / 2;
    const float base = ui_->centred_baseline(Face::Display, 40.0f, p.y + 400, 40);
    text(Face::Display, 40.0f, pct, px, base, ::ui::th().text);
    text(Face::Display, 20.0f, "%", px + pw + 2, base, ::ui::th().text_3);
    centred(Face::Text, 13.5f, removing ? std::string_view{"Removing WinAiAgent"} : step_doing(s.step), cx, p.y + 450, 20, ::ui::th().text_2);

    if (!removing) { // .steps: done ✓, now turning, next a dot
        constexpr std::pair<Step, std::string_view> steps[] = {{Step::Files, "Files"}, {Step::Shortcuts, "Shortcuts"}, {Step::Settings, "Settings"}};
        float widths[3]{};
        float total = 0;
        for (int i = 0; i < 3; ++i) {
            widths[i] = 11 + 14 + 7 + width(Face::Text, 12.5f, steps[i].second) + 11;
            total += widths[i] + (i > 0 ? 6.0f : 0.0f);
        }
        float x = cx - total / 2;
        auto& rr = ui_->renderer();
        for (int i = 0; i < 3; ++i) {
            const auto [step, label] = steps[i];
            const bool done = step < s.step;
            const bool run = step == s.step;
            const Box b{x, p.y + 512, widths[i], 27};
            // As a step of scene 06: its light on quick; done, the spinner goes on quick as the green
            // mark comes — from kDoneFrom of its size on base — and the next chip lights.
            namespace mt = ::ui::motion;
            auto& anim = ui_->animator();
            const ::ui::Id chip = id_of("setup:step") + static_cast<::ui::Id>(i) * 8;
            const float lit = std::clamp(anim.to(chip, run ? 1.0f : 0.0f, mt::kQuick), 0.0f, 1.0f);
            const float marked = std::clamp(anim.to(chip + 1, done ? 1.0f : 0.0f, mt::kQuick), 0.0f, 1.0f);
            const float grown = anim.to(chip + 2, done ? 1.0f : mt::installer::kDoneFrom, mt::kBase);
            const float opacity = rr.opacity();
            rr.rect(rect(b), {.fill = faint(::ui::th().w1, 1.0f - lit), .radius = all(13.5f)});
            if (lit > 0.004f) {
                rr.rect(rect(b), {.fill = faint(::ui::th().accent_soft, lit), .radius = all(13.5f), .border = faint(::ui::th().accent_line, lit),
                                  .border_width = 1.0f});
            }
            const ::ui::render::Point dot{b.x + 11 + 7, b.y + b.h / 2};
            if (const float waits = (1.0f - lit) * (1.0f - marked); waits > 0.004f) { // not its turn yet: a ring
                rr.set_opacity(opacity * waits);
                rr.rect({dot.x - 3.5f, dot.y - 3.5f, 7, 7}, {.radius = all(3.5f), .border = ::ui::th().text_4, .border_width = 1.5f});
            }
            if (lit > 0.004f) {
                rr.set_opacity(opacity * lit);
                ::ui::work::spinner(*ui_, dot, 13.0f, ::ui::th().accent);
            }
            if (marked > 0.004f) {
                rr.set_opacity(opacity * marked);
                const float half = 7.0f * grown;
                rr.rect({dot.x - half, dot.y - half, 2 * half, 2 * half}, {.fill = ::ui::th().ok, .radius = all(half)});
                ui_->draw_icon(kIconCheck, 9.0f, {dot.x, dot.y}, ::ui::th().on_accent, grown);
            }
            rr.set_opacity(opacity);
            const auto between = [](Color a, Color to, float share) {
                const auto ch = [share](std::uint8_t from, std::uint8_t end) {
                    return static_cast<std::uint8_t>(std::lround(static_cast<float>(from) + (static_cast<float>(end) - static_cast<float>(from)) * share));
                };
                return Color{ch(a.r, to.r), ch(a.g, to.g), ch(a.b, to.b), ch(a.a, to.a)};
            };
            text(Face::Text, 12.5f, label, b.x + 11 + 14 + 7, ui_->centred_baseline(Face::Text, 12.5f, b.y, b.h),
                 between(between(::ui::th().text_3, ::ui::th().text_2, marked), ::ui::th().text, lit));
            x += widths[i] + 6;
        }
    }

    const float fy = p.y + p.h - kFooter;
    const float by = fy + 20;
    const float base_f = ui_->centred_baseline(Face::Text, 12.5f, fy, kFooter);
    if (removing)
        return; // removal runs to its end: nothing to press
    if (s.asking_cancel) {
        text(Face::Text, 13.0f, "Cancel the installation?", p.x + 24, base_f, ::ui::th().text_2);
        const float yes_w = 20 + width(Face::Semibold, 14.0f, "Yes, cancel") + 20;
        const Box yes{p.x + p.w - 22 - yes_w, by, yes_w, 44};
        if (button("yes", yes, "Yes, cancel", false, true, true))
            r.action = Action::Yes;
        const float no_w = 20 + width(Face::Semibold, 14.0f, "Keep installing") + 20;
        if (button("no", {yes.x - 10 - no_w, by, no_w, 44}, "Keep installing", false))
            r.action = Action::No;
        return;
    }
    if (s.eta_seconds >= 0)
        text(Face::Text, 12.5f, std::format("About {} s left", static_cast<int>(std::ceil(s.eta_seconds))), p.x + 24, base_f, ::ui::th().text_3);
    const float w = 20 + width(Face::Semibold, 14.0f, "Cancel") + 20;
    if (button("cancel", {p.x + p.w - 22 - w, by, w, 44}, "Cancel", false))
        r.action = Action::Cancel;
}

void Painter::draw_done(const State& s, const Box& p, Reaction& r) {
    const float cx = p.x + p.w / 2;
    auto& rr = ui_->renderer();
    // .okp: the green pill.
    const auto took = std::format("Installed in {:.1f} s", s.took_seconds);
    const float tw = 12 + 14 + 7 + width(Face::Text, 12.5f, took) + 12;
    const Box pill{cx - tw / 2, p.y + 336, tw, 25};
    rr.rect(rect(pill), {.fill = ::ui::th().ok_soft, .radius = all(12.5f), .border = ::ui::th().ok_line, .border_width = 1.0f});
    const ::ui::render::Point dot{pill.x + 12 + 7, pill.y + 12.5f};
    rr.rect({dot.x - 7, dot.y - 7, 14, 14}, {.fill = ::ui::th().ok, .radius = all(7.0f)});
    ui_->draw_icon(kIconCheck, 9.0f, {dot.x, dot.y}, ::ui::th().on_accent);
    text(Face::Text, 12.5f, took, pill.x + 12 + 14 + 7, ui_->centred_baseline(Face::Text, 12.5f, pill.y, pill.h), ::ui::th().ok);

    centred(Face::Display, 32.0f, "All set", cx, p.y + 384, 37, ::ui::th().text);
    centred(Face::Text, 15.0f, "Call the agent from anywhere with", cx, p.y + 437, 22.5f, ::ui::th().text_2);
    keys(s.choices.hotkey, cx, p.y + 480, 52, true);

    // The agent starts only when Launch is pressed: the launch's animation, then
    // the agent's own window. Closing the window leaves it for the next sign-in.
    const float fy = p.y + p.h - kFooter;
    if (main_button("launch", p.x + p.w - 22, fy + 20, "Launch").first)
        r.action = Action::Enter;
}

// Installed, Failed, Confirm, Removed: the orb, a title, a line, a quiet line, the buttons.
void Painter::draw_message(const State& s, const Box& p, Reaction& r) {
    const float cx = p.x + p.w / 2;
    std::string_view title;
    std::string sub;
    std::string meta;
    std::string_view primary = "Close";
    bool crit = false;
    bool keep = false;
    switch (s.screen) {
    case Screen::Installed:
        title = "Already installed";
        sub = std::format("WinAiAgent {} is already installed on this computer", s.installed_version);
        meta = narrow(s.installed_dir);
        break;
    case Screen::Failed:
        title = s.uninstalling ? std::string_view{"Nothing was removed"} : std::string_view{"Not installed"};
        sub = s.note;
        meta = s.uninstalling ? std::string{} : std::string{"Everything it had done was undone"};
        break;
    case Screen::Confirm:
        title = "Remove WinAiAgent?";
        sub = "The program, its settings, your history and the API key";
        meta = narrow(s.installed_dir);
        primary = "Remove";
        crit = true;
        keep = true;
        break;
    default: // Removed
        title = s.note.empty() ? std::string_view{"Removed"} : std::string_view{"Not everything was removed"};
        sub = s.note.empty() ? std::string{"Nothing of WinAiAgent is left on this computer"} : s.note;
        meta = s.note.empty() ? std::string{} : std::string{"Run the uninstaller again to finish"};
        break;
    }
    centred(Face::Display, 32.0f, title, cx, p.y + 384, 37, ::ui::th().text);
    centred(Face::Text, 15.0f, sub, cx, p.y + 437, 22.5f, ::ui::th().text_2);
    if (!meta.empty())
        centred(s.screen == Screen::Failed || s.screen == Screen::Removed ? Face::Text : Face::Mono, 12.0f, meta, cx,
                p.y + 474, 18, ::ui::th().text_4);

    const float by = p.y + p.h - kFooter + 20;
    const auto [pressed, left] = main_button("primary", p.x + p.w - 22, by, primary, true, crit);
    if (pressed)
        r.action = Action::Enter;
    if (keep) {
        const float w = 20 + width(Face::Semibold, 14.0f, "Keep it") + 20;
        if (button("keep", {left - 10 - w, by, w, 44}, "Keep it", false))
            r.action = Action::Close;
    }
}

auto Painter::draw(const State& s, const Box& panel) -> Reaction {
    Reaction r;
    targets_.clear();
    texts_.clear();
    minimize_ = false;
    draw_window(panel);
    draw_orb(panel);
    draw_content(s, panel, r);
    return r;
}

// A membrane or a melting window: the glass in `box` with its radius, the chrome over it.
auto Painter::part(std::size_t k) -> float {
    if (parts_ == nullptr) {
        return 0.0f;
    }
    const Part& p = (*parts_)[std::min(k, parts_->size() - 1)];
    ui_->renderer().set_opacity(p.alpha);
    return p.dy;
}

void Painter::draw_glass(const Box& box, float radius_top, float radius_bottom, float alpha, float chrome, float rim_alpha,
                         float rim_degrees) {
    auto& r = ui_->renderer();
    const float radius = std::max(radius_top, radius_bottom);
    r.shadow({box.x + box.w * 0.04f, box.y + 40 + box.h * 0.03f, box.w * 0.92f, box.h * 0.94f}, radius, 90.0f, faint(::ui::th().shadow[0].color, alpha));
    r.rect(rect(box), {.fill = faint(::ui::th().surface, alpha),
                       .radius = {radius_top, radius_top, radius_bottom, radius_bottom},
                       .border = faint(::ui::th().line, alpha), .border_width = 1.0f});
    if (chrome > 0.001f) { // .chromeov: a light top, a dark horizon, a light bottom — lit as the orb (the theme's orb light)
        const float half = box.h / 2;
        const float k = chrome * alpha;
        constexpr float kDown = std::numbers::pi_v<float> / 2;
        r.rect({box.x, box.y, box.w, half}, {.fill = faint(::ui::th().orb.tint, k), .fill_to = faint(::ui::th().surface, k), .gradient_angle = kDown,
                                             .radius = {radius, radius, 0, 0}});
        r.rect({box.x, box.y + half, box.w, box.h - half}, {.fill = faint(::ui::th().surface, k), .fill_to = faint(::ui::th().orb.tint, 0.9f * k),
                                                            .gradient_angle = kDown, .radius = {0, 0, radius, radius}});
        r.shadow({box.x + box.w * 0.08f, box.y + box.h * 0.45f, box.w * 0.25f, box.h * 0.2f}, box.w * 0.1f, box.w * 0.25f, faint(::ui::th().orb.l1, 0.5f * k));
        r.shadow({box.x + box.w * 0.7f, box.y + box.h * 0.4f, box.w * 0.22f, box.h * 0.2f}, box.w * 0.1f, box.w * 0.25f, faint(::ui::th().orb.l2, 0.4f * k));
    }
    if (rim_alpha > 0.001f) { // .rim: a highlight running along the edge
        const float rad = rim_degrees * std::numbers::pi_v<float> / 180.0f;
        const float cx = box.x + box.w / 2;
        const float cy = box.y + box.h / 2;
        const ::ui::render::Point head{cx + box.w / 2 * std::sin(rad), cy - box.h / 2 * std::cos(rad)};
        const ::ui::render::Point tail{cx + box.w / 2 * std::sin(rad - 0.5f), cy - box.h / 2 * std::cos(rad - 0.5f)};
        r.segment(tail, head, 2.0f, faint(::ui::th().orb.tint, 0.8f * rim_alpha * alpha), 6.0f);
    }
}

void Painter::orb_at(::ui::render::Point centre, float quad, float alpha) {
    ui_->renderer().image({centre.x - quad / 2, centre.y - quad / 2, quad, quad}, ::ui::Orb::uv(), orb_->texture(),
                          faint(::ui::kOnPicture, alpha)); // the orb's own colours at `alpha`
}

void Painter::draw_intro(const State& s, const IntroFrame& f, const Box& window) {
    targets_.clear();
    texts_.clear();
    auto& r = ui_->renderer();
    for (const auto& w : f.waves) { // .shock: a thin bright ring and its glow
        if (w.alpha <= 0.001f)
            continue;
        constexpr float kTurn = 2 * std::numbers::pi_v<float>;
        r.arc(f.centre, w.radius, 10.0f, 0, kTurn, faint(::ui::th().orb.glow, 0.18f * w.alpha));
        r.arc(f.centre, w.radius, 1.5f, 0, kTurn, faint(::ui::th().orb.tint, 0.8f * w.alpha));
    }
    if (f.window_alpha > 0.001f) {
        draw_glass(f.window, f.window_radius, f.window_radius, f.window_alpha, f.chrome, f.rim_alpha, f.rim_degrees);
        if (f.parts.front().alpha > 0.001f) {
            // The first screen by its parts (MS § 7): each on its own turn, cut by the membrane as it is now.
            Reaction ignored;
            parts_ = &f.parts;
            r.push_clip(rect(f.window));
            draw_content(s, window, ignored);
            r.pop_clip();
            parts_ = nullptr;
            r.set_opacity(1.0f);
        }
    }
    orb_at(f.orb_centre, f.orb_body * kOrbQuad, f.orb_alpha);
}

void Painter::draw_launch(const State& s, const LaunchFrame& f, const Box& window) {
    targets_.clear();
    texts_.clear();
    auto& r = ui_->renderer();
    if (f.window_alpha > 0.001f) {
        draw_glass(f.window, f.radius_top, f.radius_bottom, f.window_alpha, 0.0f, f.rim_alpha, f.rim_degrees);
        if (f.content_alpha > 0.001f) { // the last screen going, cut by the shape as it is now
            r.set_opacity(f.content_alpha * f.window_alpha);
            Reaction ignored;
            r.push_clip(rect(f.window));
            draw_content(s, window, ignored);
            r.pop_clip();
            r.set_opacity(1.0f);
        }
    }
    if (f.orb_alpha > 0.001f && f.orb_quad > 0.5f)
        orb_at(f.orb_centre, f.orb_quad, f.orb_alpha);
}

void Painter::draw_content(const State& s, const Box& panel, Reaction& r) {
    draw_top({panel.x, panel.y + part(0), panel.w, panel.h}, r);
    draw_footer_line({panel.x, panel.y + part(5), panel.w, panel.h});
    if (parts_ != nullptr) { // the appearance: the first screen by its parts
        draw_screen(s, panel, r);
        return;
    }
    // A screen changing (the map's unnamed row): the one that goes is a picture, on micro; the one
    // that comes takes the clicks from its first frame, on quick, up from below on base.
    auto& renderer = ui_->renderer();
    const float opacity = renderer.opacity();
    if (leaving_ && screen_motion_.out > 0.004f) {
        const auto targets_kept = targets_.size();
        const bool was_inert = ui_->inert();
        Reaction ignored;
        ui_->set_inert(true);
        renderer.set_opacity(opacity * screen_motion_.out);
        draw_screen(*leaving_, panel, ignored);
        ui_->set_inert(was_inert);
        targets_.resize(targets_kept);
    }
    renderer.set_opacity(opacity * screen_motion_.in);
    draw_screen(s, {panel.x, panel.y + screen_motion_.lift, panel.w, panel.h}, r);
    renderer.set_opacity(opacity);
}

void Painter::draw_screen(const State& s, const Box& panel, Reaction& r) {
    switch (s.screen) {
    case Screen::Welcome:
    case Screen::Update: draw_welcome(s, panel, r); break;
    case Screen::Options: draw_options(s, panel, r); break;
    case Screen::Installing:
    case Screen::Removing: draw_working(s, panel, r); break;
    case Screen::Done: draw_done(s, panel, r); break;
    default: draw_message(s, panel, r); break;
    }
}

} // namespace setup::view
