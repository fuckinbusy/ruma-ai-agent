// The settings mode of the agent's window (UI update, stage 4d-1; SETTINGS_SPEC, the designer's 02
// and 04): a header instead of the bar, the sections' column, a section's items on the right. The
// items are the tray's settings (app::settings::pages): a choice is that item's id, applied by the
// shell as the tray's. Nothing here allocates in a frame.

#include "app/sheet_view.hpp"

#include "core/hash.hpp"
#include "ui/anim.hpp"
#include "ui/theme.hpp"
#include "ui/work.hpp"

#include <algorithm>
#include <array>
#include <format>

namespace app::sheet {
namespace {

namespace work = ui::work;
namespace wt = ui::theme::work;
using ui::Face;
using ui::layout::Box;

auto id_of(std::string_view name, std::size_t n = 0) -> ui::Id {
    return core::hash::fnv1a64(name, core::hash::fnv1a64("app/sheet/settings")) * 1099511628211ull + n;
}

template <class... Args>
auto text(std::array<char, 64>& buf, std::format_string<Args...> f, Args&&... args) -> std::string_view {
    const auto r = std::format_to_n(buf.data(), buf.size(), f, std::forward<Args>(args)...);
    return {buf.data(), static_cast<std::size_t>(r.out - buf.data())};
}

// The layout (.awin, .snav, .smain).
constexpr float kSettingsWidth = 1240; // the bar's width (SETTINGS_SPEC § 3)
constexpr float kSettingsFieldLeft = wt::bar::pad_left + wt::bar::orb + 2 * wt::bar::orb_margin + wt::bar::gap; // right of the orb
constexpr float kPadX = 22;            // the data panel's own padding round its list
constexpr float kNav = 230, kNavItem = 38, kNavGap = 2, kNavPadTop = 14, kNavPadX = 10;
constexpr float kMainPadTop = 24, kMainPadX = 30, kLeadMax = 760;
constexpr float kItemRow = 56; // a setting: its name left, its control right
constexpr float kListMin = 420; // Rules and memory, Privacy: the list's room at least
constexpr std::size_t kSections = 7;

// Segoe Fluent Icons: Setting, Color, Book, Lock, Keyboard.
// Segoe Fluent Icons: Setting, Color, a cube (Models), a route (Roles), Book, Lock, Keyboard.
constexpr std::array<char32_t, kSections> kNavIcons{0xE713, 0xE790, 0xF158, 0xE8AB, 0xE82D, 0xE72E, 0xE765};
constexpr std::array<std::string_view, kSections> kLeads{
    "How the agent starts and where it shows.",
    "How the agent looks. A change shows at once.",
    "The provider the agent works with, its key and its models. The key is checked before it is kept.",
    "Which model does what. When a model can't do something or doesn't answer, the agent moves to the next one "
    "and tells you in the chat.",
    "Your rules for the agent, and what it remembers about you. It never remembers a secret.",
    "The windows the agent never looks at, and the logs of what it did.",
    "The key that shows the agent, and the keys inside its window.",
};
// Shortcuts: the window's own keys, said, not set (SETTINGS_SPEC § 2; the model menu, stage 4b).
constexpr std::array<std::pair<std::string_view, std::string_view>, 4> kKeys{{
    {"Ctrl+M", "The model menu"},
    {"Ctrl+Shift+M", "Back to the model before"},
    {"Ctrl+,", "Settings"},
    {"Esc", "Close what is on top, then the window"},
}};

auto section_at(std::size_t i) -> Section {
    return static_cast<Section>(i);
}

// An item's choices as labels, on the stack: the controls take a span of views.
struct Labels {
    std::array<std::string_view, 16> at{};
    std::size_t n = 0;
    int checked = -1;
    [[nodiscard]] auto span() const -> std::span<const std::string_view> { return {at.data(), n}; }
};
auto labels_of(const settings::Item& item) -> Labels {
    Labels l;
    for (const auto& c : item.children) {
        if (l.n == l.at.size()) {
            break;
        }
        if (c.checked) {
            l.checked = static_cast<int>(l.n);
        }
        l.at[l.n++] = c.label;
    }
    return l;
}

} // namespace

auto Painter::settings_shape(const Model& m) const -> Shape {
    (void)m;
    const float height = wt::bar::height + std::max(settings_content_, 360.0f);
    return {std::min(kSettingsWidth, room_.w - 2 * kSideRoom), std::min(height, room_.h - kBottomRoom), ui::th().radius.window};
}

void Painter::draw_settings(Model& m, const Box& panel, Reaction& r) {
    auto& renderer = ui_->renderer();
    std::array<char, 64> buf{};
    const auto section = static_cast<std::size_t>(m.settings.section);
    const float bar = wt::bar::height;
    const float cy = panel.y + bar / 2;

    // The header (.sbar): ‹ Chat, "Settings · <section>", and on the right the note, Esc, the cross.
    renderer.rect({panel.x, panel.y + bar - 1, panel.w, 1}, {.fill = ui::th().line});
    constexpr std::string_view kChat = "\xE2\x80\xB9 Chat";
    const auto chat_size = work::button_size(*ui_, kChat);
    const Box chat{panel.x + kSettingsFieldLeft, cy - chat_size.h / 2, chat_size.w, chat_size.h};
    add_target("settings:chat", chat);
    if (work::button(*ui_, id_of("chat"), chat, kChat) && !r.command) {
        const auto closed = close_settings(m);
        r.close = closed.close;
        return;
    }
    const float tx = chat.x + chat.w + 16;
    const float title_w = work::caption(*ui_, {tx, panel.y, 200, bar}, Face::Semibold, 20.0f, "Settings", ui::th().text);
    const float name_w = work::caption(*ui_, {tx + title_w + 6, panel.y + 2, 260, bar}, Face::Text, 14.0f,
                                       text(buf, "\xC2\xB7 {}", section_name(m.settings.section)), ui::th().text_3);
    add_target("settings:title", {tx, cy - 12, title_w + 6 + name_w, 24});
    const float ib = wt::bar::icon_button;
    const Box close{panel.x + panel.w - wt::bar::pad_right - ib, cy - ib / 2, ib, ib};
    add_target("settings:close", close);
    if (work::icon_button(*ui_, id_of("close"), close, work::kIconClose) && !r.command) {
        const auto closed = close_settings(m);
        r.close = closed.close;
        return;
    }
    const float esc_w = work::text_width(*ui_, Face::Semibold, 11.0f, "Esc") + 12;
    const Box esc{close.x - 12 - esc_w, cy - 10, esc_w, 20};
    renderer.rect({esc.x, esc.y, esc.w, esc.h}, {.fill = ui::th().w1, .radius = {5, 5, 5, 5}, .border = ui::th().line, .border_width = 1});
    work::caption(*ui_, {esc.x + 6, esc.y, esc.w - 6, esc.h}, Face::Semibold, 11.0f, "Esc", ui::th().text_3);
    constexpr std::string_view kStays = "Stays open until you close it";
    const float stays_w = work::text_width(*ui_, Face::Text, 12.5f, kStays);
    work::caption(*ui_, {esc.x - 10 - stays_w, panel.y, stays_w + 1, bar}, Face::Text, 12.5f, kStays, ui::th().text_4);

    // The column (.snav): the panel's ground, the chosen one's soft ground sliding to it on base.
    const Box nav{panel.x, panel.y + bar, kNav, std::max(0.0f, panel.h - bar)};
    renderer.rect({nav.x, nav.y, nav.w, nav.h}, {.fill = ui::th().panel, .radius = {0, 0, 0, ui::th().radius.window}});
    renderer.rect({nav.x + nav.w - 1, nav.y, 1, nav.h}, {.fill = ui::th().line});
    const auto item_box = [&](std::size_t i) {
        return Box{nav.x + kNavPadX, nav.y + kNavPadTop + static_cast<float>(i) * (kNavItem + kNavGap), nav.w - 2 * kNavPadX, kNavItem};
    };
    const float lit_y = anim_.to(id_of("nav:lit"), item_box(section).y, ui::motion::kBase);
    renderer.rect({item_box(0).x, lit_y, item_box(0).w, kNavItem}, {.fill = ui::th().accent_soft, .radius = {9, 9, 9, 9}});
    renderer.rect({item_box(0).x, lit_y + 8, 2, kNavItem - 16}, {.fill = ui::th().accent, .radius = {1, 1, 1, 1}});
    for (std::size_t i = 0; i < kSections; ++i) {
        const Box b = item_box(i);
        const bool on = i == section;
        const auto hit = ui_->interact(id_of("nav", i), b);
        if (!on && hit.hovered) {
            renderer.rect({b.x, b.y, b.w, b.h}, {.fill = ui::th().w1, .radius = {9, 9, 9, 9}});
        }
        ui_->draw_icon(kNavIcons[i], 16.0f, {b.x + 12 + 8, b.y + b.h / 2}, on ? ui::th().accent : ui::th().text_3);
        work::caption(*ui_, {b.x + 12 + 16 + 11, b.y, b.w - 40, b.h}, Face::Text, 14.0f, section_name(section_at(i)),
                      on ? ui::th().text : ui::th().text_2);
        add_target(text(buf, "settings:nav:{}", i), b);
        if (hit.clicked && !on) {
            show_section(m, section_at(i));
        }
    }

    // The section (.smain): on a change the old one is gone at once, the new one comes in on quick from
    // 8 px below (SETTINGS_SPEC § 6).
    if (settings_shown_ != m.settings.section) {
        settings_shown_ = m.settings.section;
        anim_.place(id_of("section:in"), 0.0f);
        settings_open_item_ = -1;
        settings_scroll_ = {}; // a new section from its top
    }
    const float in = anim_.to(id_of("section:in"), 1.0f, ui::motion::kQuick);
    const float dy = 8.0f * (1.0f - in);
    const float o = renderer.opacity();
    renderer.set_opacity(o * in);
    // The right side scrolls when the section is taller than the room (final review I4).
    const Box viewport{nav.x + nav.w, nav.y, std::max(0.0f, panel.x + panel.w - nav.x - nav.w), nav.h};
    const Box content = work::scroll_view(*ui_, settings_scroll_, viewport, settings_content_);
    const Box main{content.x + kMainPadX, content.y + kMainPadTop + dy, std::max(0.0f, viewport.w - 2 * kMainPadX),
                   std::max(0.0f, nav.h - kMainPadTop)};
    float y = main.y;
    add_target("settings:section", {main.x, y, main.w, 28});
    work::caption(*ui_, {main.x, y, main.w, 28}, Face::Semibold, 22.0f, section_name(m.settings.section), ui::th().text);
    y += 28 + 6;
    const work::TextBlockStyle lead{.face = Face::Text, .size = 14.0f, .color = ui::th().text_2, .line = 1.5f, .min_line_h = 0};
    y += work::text_block(*ui_, {main.x, y, std::min(main.w, kLeadMax), 0}, kLeads[section], lead) + 18;

    if (m.settings.section == Section::Models) { // its card and the month (stage 4d-2)
        y += draw_models_section(m, main.x, y, main.w, r) + 18;
    } else if (m.settings.section == Section::Roles) {
        y += draw_roles_section(m, main.x, y, main.w, r) + 8;
    }
    const std::span<const settings::Item> items = m.settings.section == Section::Models
                                                      ? std::span<const settings::Item>{} // Models' are in its card
                                                      : std::span<const settings::Item>{m.settings.pages[section]};
    // Items from the last up: an open list is drawn after the rows under it, over them.
    const float items_top = y;
    for (std::size_t k = items.size(); k-- > 0;) {
        const auto& item = items[k];
        const Box row{main.x, items_top + static_cast<float>(k) * kItemRow, main.w, kItemRow};
        if (k + 1 < items.size()) {
            renderer.rect({row.x, row.y + row.h - 1, row.w, 1}, {.fill = ui::th().line});
        }
        const float rcy = row.y + row.h / 2;
        if (item.children.empty()) {
            if (item.label.ends_with("\xE2\x80\xA6")) { // an action: Change log…, Audit log…
                const auto size = work::button_size(*ui_, item.label);
                const Box b{row.x, rcy - size.h / 2, size.w, size.h};
                add_target(text(buf, "settings:item:{}", item.id), b);
                if (work::button(*ui_, id_of("item", item.id), b, item.label) && !r.command) {
                    r.setting = item.id;
                }
                continue;
            }
            work::caption(*ui_, {row.x, row.y, row.w - 60, row.h}, Face::Text, 14.0f, item.label, ui::th().text);
            const Box sw{row.x + row.w - 40, rcy - 11, 40, 22};
            add_target(text(buf, "settings:item:{}", item.id), sw);
            if (work::toggle(*ui_, id_of("item", item.id), sw, item.checked) && !r.command) {
                r.setting = item.id;
            }
            continue;
        }
        work::caption(*ui_, {row.x, row.y, row.w * 0.45f, row.h}, Face::Text, 14.0f, item.label, ui::th().text);
        const auto labels = labels_of(item);
        const auto seg = work::segmented_size(*ui_, labels.span());
        if (labels.n <= 5 && seg.w <= row.w * 0.55f) {
            const Box b{row.x + row.w - seg.w, rcy - seg.h / 2, seg.w, seg.h};
            float x = b.x + wt::segment::pad;
            for (std::size_t c = 0; c < labels.n; ++c) { // each choice's place, as segmented() lays them out
                const float w = 2 * wt::segment::item_pad_x + work::text_width(*ui_, Face::Text, wt::type::segment, labels.at[c]);
                add_target(text(buf, "settings:item:{}", item.children[c].id), {x, b.y + wt::segment::pad, w, b.h - 2 * wt::segment::pad});
                x += w + 2.0f;
            }
            const int picked = work::segmented(*ui_, id_of("item", item.children.front().id), b, labels.span(), labels.checked);
            if (picked != labels.checked && picked >= 0 && !r.command) {
                r.setting = item.children[static_cast<std::size_t>(picked)].id;
            }
            continue;
        }
        const auto dd = work::dropdown_size(*ui_, labels.span());
        const Box b{row.x + row.w - dd.w, rcy - dd.h / 2, dd.w, dd.h};
        add_target(text(buf, "settings:item:{}", item.children.front().id), b);
        bool open = settings_open_item_ == static_cast<int>(k);
        if (const auto picked = work::dropdown(*ui_, id_of("item", item.children.front().id), b, labels.span(), labels.checked, open);
            picked && !r.command) {
            r.setting = item.children[static_cast<std::size_t>(*picked)].id;
        }
        settings_open_item_ = open ? static_cast<int>(k) : settings_open_item_ == static_cast<int>(k) ? -1 : settings_open_item_;
    }
    y = items_top + static_cast<float>(items.size()) * kItemRow;

    // The lists: Rules and memory's, the private windows (Privacy) — the data panel's own drawing.
    const auto list_id = m.settings.section == Section::RulesMemory ? m.settings.memory_set
                       : m.settings.section == Section::Privacy     ? m.settings.private_set
                                                                    : 0u;
    if (list_id != 0) {
        if (const auto it = std::ranges::find(m.sets, list_id, &events::DataSet::id); it != m.sets.end()) {
            y += items.empty() ? 0.0f : 18.0f;
            const Box list{main.x - kPadX, y, main.w + 2 * kPadX, kListMin};
            add_target("settings:list", list);
            draw_set_body(m, *it, list, r);
            y += kListMin;
        }
    }
    if (m.settings.section == Section::Shortcuts) {
        y += 18;
        for (const auto& [keys, what] : kKeys) {
            const float kw = work::text_width(*ui_, Face::Semibold, 12.0f, keys) + 14;
            renderer.rect({main.x, y + 7, kw, 22}, {.fill = ui::th().w1, .radius = {6, 6, 6, 6}, .border = ui::th().line, .border_width = 1});
            work::caption(*ui_, {main.x + 7, y + 7, kw - 7, 22}, Face::Semibold, 12.0f, keys, ui::th().text_2);
            work::caption(*ui_, {main.x + 150, y, main.w - 150, 36}, Face::Text, 14.0f, what, ui::th().text_2);
            y += 36;
        }
    }
    settings_content_ = (y - dy) - content.y + kMainPadTop;
    work::end_scroll(*ui_, settings_scroll_, viewport);
    renderer.set_opacity(o);
}

} // namespace app::sheet
