// The conversation's word on its models (UI update, stage 4c; the designer's screens 05 and 06):
// the blue route note over an answer that another model made, the divider of a model the user
// changed to, and a provider's failure as one card. Every text was made when its event came;
// nothing here allocates in a frame.

#include "app/sheet_view.hpp"

#include "core/hash.hpp"
#include "ui/theme.hpp"
#include "ui/work.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>

namespace app::sheet {
namespace {

namespace work = ui::work;
using ui::Face;
using ui::layout::Box;
using ui::render::Color;

auto id_of(std::string_view name, std::size_t n = 0) -> ui::Id {
    return core::hash::fnv1a64(name, core::hash::fnv1a64("app/sheet")) * 1099511628211ull + n;
}

auto with_alpha(Color c, float k) -> Color {
    c.a = static_cast<std::uint8_t>(std::lround(static_cast<float>(c.a) * std::clamp(k, 0.0f, 1.0f)));
    return c;
}

template <class... Args>
auto text(std::array<char, 64>& buf, std::format_string<Args...> f, Args&&... args) -> std::string_view {
    const auto r = std::format_to_n(buf.data(), buf.size(), f, std::forward<Args>(args)...);
    return {buf.data(), static_cast<std::size_t>(r.out - buf.data())};
}

// The route note (.route): padding 10 14, the route icon 16, text 13.5 that wraps.
constexpr float kRoutePadY = 10, kRoutePadX = 14, kRouteIcon = 16, kRouteGap = 10, kRouteLine = 20;
auto route_style() -> work::TextBlockStyle {
    return {.face = Face::Text, .size = 13.5f, .color = ui::th().text_2, .line = 1.4f, .min_line_h = 0};
}

// The failure card (.ecard): padding 16 18, radius 18; the tag, the icon 34, the title, the body.
constexpr float kCardPadY = 16, kCardPadX = 18, kCardIcon = 34, kCardMax = 560;
auto body_style() -> work::TextBlockStyle {
    return {.face = Face::Text, .size = 13.5f, .color = ui::th().text_2, .line = 1.5f, .min_line_h = 0};
}

// Segoe Fluent Icons: a key, a clock, the globe, a card, a warning.
constexpr char32_t kIconKey = 0xE8D7, kIconClock = 0xE823, kIconGlobe = 0xE774, kIconPayment = 0xE8C7, kIconWarning = 0xE7BA;

struct Look {
    char32_t icon;
    Color ground, ink;
};
auto look_of(events::CardKind k) -> Look {
    const auto& t = ui::th();
    switch (k) {
    case events::CardKind::NoKey:
    case events::CardKind::KeyRejected: return {kIconKey, t.crit_soft, t.crit};
    case events::CardKind::Busy: return {kIconClock, t.warn_soft, t.warn};
    case events::CardKind::Balance: return {kIconPayment, t.warn_soft, t.warn};
    case events::CardKind::NoAnswer: return {kIconClock, t.card_2, t.text_2};
    case events::CardKind::Offline: return {kIconGlobe, t.card_2, t.text_2};
    case events::CardKind::Error: break;
    }
    return {kIconWarning, with_alpha(t.accent_2, 0.14f), t.accent_2};
}

} // namespace

auto Painter::draw_routes(std::span<const std::string> routes, float x, float y0, float w, bool measure) -> float {
    auto& renderer = ui_->renderer();
    const auto style = route_style();
    const float tw = std::max(0.0f, w - kRoutePadX - kRouteIcon - kRouteGap - kRoutePadX);
    std::array<char, 64> buf{};
    float y = y0;
    for (std::size_t i = 0; i < routes.size(); ++i) {
        const float th = work::text_block_height(*ui_, routes[i], style, tw);
        const float h = std::max(th, kRouteLine) + 2 * kRoutePadY;
        if (!measure) {
            const Box box{x, y, w, h};
            renderer.rect({box.x, box.y, box.w, box.h}, {.fill = with_alpha(ui::th().accent_2, 0.07f),
                                                         .radius = {12, 12, 12, 12},
                                                         .border = with_alpha(ui::th().accent_2, 0.26f),
                                                         .border_width = 1});
            const float first = std::min(th, kRouteLine); // the icon by the first line
            ui_->draw_icon(kIconRoute, kRouteIcon, {x + kRoutePadX + kRouteIcon / 2, y + kRoutePadY + std::max(first, kRouteLine) / 2},
                           ui::th().accent_2);
            const float ty = y + kRoutePadY + (th < kRouteLine ? (kRouteLine - th) / 2 : 0.0f);
            (void)work::text_block(*ui_, {x + kRoutePadX + kRouteIcon + kRouteGap, ty, tw, th}, routes[i], style);
            add_target(text(buf, "route:{}", i), box);
        }
        y += h + 8;
    }
    return routes.empty() ? 0.0f : y - y0 + 6;
}

auto Painter::draw_switched(std::string_view name, float x, float y0, float w, bool measure) -> float {
    if (name.empty()) {
        return 0;
    }
    constexpr float kLine = 18, kGap = 12;
    constexpr std::string_view kSaid = "You switched to ";
    if (!measure) {
        auto& renderer = ui_->renderer();
        const float said_w = work::text_width(*ui_, Face::Text, 12.5f, kSaid);
        const float name_w = work::text_width(*ui_, Face::Semibold, 12.5f, name);
        const float whole = said_w + name_w;
        const float left = x + std::max(0.0f, (w - whole) / 2);
        const float cy = y0 + kLine / 2;
        if (left - kGap > x) {
            renderer.rect({x, cy, left - kGap - x, 1}, {.fill = ui::th().line});
        }
        if (left + whole + kGap < x + w) {
            renderer.rect({left + whole + kGap, cy, x + w - (left + whole + kGap), 1}, {.fill = ui::th().line});
        }
        work::caption(*ui_, {left, y0, said_w + 1, kLine}, Face::Text, 12.5f, kSaid, ui::th().text_3);
        work::caption(*ui_, {left + said_w, y0, name_w + 1, kLine}, Face::Semibold, 12.5f, name, ui::th().text_2);
        add_target("switched", {left, y0, whole, kLine});
    }
    return kLine + 18;
}

auto Painter::draw_failure_card(Model& m, float x, float y0, float w0, bool measure, Reaction& r) -> float {
    const auto& c = m.card;
    const float w = std::min(w0, kCardMax);
    const float inner = w - 2 * kCardPadX;
    const auto body = body_style();
    const float body_h = c.body.empty() ? 0.0f : work::text_block_height(*ui_, c.body, body, inner);
    float buttons_h = 0;
    for (const auto& a : m.done_actions) {
        buttons_h = std::max(buttons_h, work::button_size(*ui_, a.label).h);
    }
    constexpr float kTag = 14, kTagGap = 10;
    const float body_part = c.body.empty() ? 14.0f : 12 + body_h + 14;
    const float h = kCardPadY + kTag + kTagGap + kCardIcon + body_part + buttons_h + kCardPadY;
    if (measure) {
        return h + 10;
    }
    auto& renderer = ui_->renderer();
    const Box card{x, y0, w, h};
    add_target("turn:card", card);
    renderer.rect({card.x, card.y, card.w, card.h}, {.fill = ui::th().card, .radius = {18, 18, 18, 18}, .border = ui::th().line_2, .border_width = 1});
    float y = y0 + kCardPadY;
    std::array<char, 32> upper{}; // the tag in capitals, on the stack
    const std::size_t n = std::min(c.tag.size(), upper.size());
    for (std::size_t i = 0; i < n; ++i) {
        const char ch = c.tag[i];
        upper[i] = ch >= 'a' && ch <= 'z' ? static_cast<char>(ch - 'a' + 'A') : ch;
    }
    work::caption(*ui_, {x + kCardPadX, y, inner, kTag}, Face::Semibold, 10.5f, {upper.data(), n}, ui::th().text_4);
    y += kTag + kTagGap;
    const Look look = look_of(c.kind);
    renderer.rect({x + kCardPadX, y, kCardIcon, kCardIcon}, {.fill = look.ground, .radius = {10, 10, 10, 10}});
    ui_->draw_icon(look.icon, 17.0f, {x + kCardPadX + kCardIcon / 2, y + kCardIcon / 2}, look.ink);
    const float tx = x + kCardPadX + kCardIcon + 11;
    const float tw = std::max(0.0f, x + w - kCardPadX - tx);
    work::caption(*ui_, {tx, y - 1, tw, 20}, Face::Semibold, 15.5f, c.title, ui::th().text);
    if (!c.detail.empty()) {
        work::caption(*ui_, {tx, y + 19, tw, 16}, Face::Text, 12.5f, c.detail, ui::th().text_3);
    }
    y += kCardIcon;
    if (!c.body.empty()) {
        (void)work::text_block(*ui_, {x + kCardPadX, y + 12, inner, body_h}, c.body, body);
    }
    y += body_part;
    float bx = x + kCardPadX;
    std::array<char, 64> buf{};
    for (std::size_t i = 0; i < m.done_actions.size(); ++i) {
        const auto& a = m.done_actions[i];
        const auto size = work::button_size(*ui_, a.label);
        const Box box{bx, y, size.w, size.h};
        add_target(text(buf, "action:{}", a.id), box);
        if (work::button(*ui_, id_of("done:", core::hash::fnv1a64(a.id, 0)), box, a.label,
                         a.primary ? work::ButtonStyle::Primary : work::ButtonStyle::Normal) &&
            !r.command) {
            const std::string id = a.id;
            r.command = press(m, id);
            return h + 10;
        }
        bx += size.w + 8;
    }
    return h + 10;
}

} // namespace app::sheet
