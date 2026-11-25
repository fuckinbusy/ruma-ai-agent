// The settings' Models and Roles sections (UI update, stage 4d-2; the designer's 02 and 04): the
// provider's card with its key and models, the month's spending; the three roles with their models
// and the budget per task. Every text is made on the stack; nothing here allocates in a frame.

#include "app/picker.hpp"
#include "app/sheet_view.hpp"

#include "core/hash.hpp"
#include "ui/theme.hpp"
#include "ui/work.hpp"

#include <algorithm>
#include <array>
#include <format>

namespace app::sheet {
namespace {

namespace work = ui::work;
using ui::Face;
using ui::layout::Box;
using ui::render::Color;

auto id_of(std::string_view name, std::size_t n = 0) -> ui::Id {
    return core::hash::fnv1a64(name, core::hash::fnv1a64("app/sheet/settings/models")) * 1099511628211ull + n;
}

template <std::size_t N, class... Args>
auto said(std::array<char, N>& buf, std::format_string<Args...> f, Args&&... args) -> std::string_view {
    const auto r = std::format_to_n(buf.data(), static_cast<std::ptrdiff_t>(buf.size()), f, std::forward<Args>(args)...);
    return {buf.data(), static_cast<std::size_t>(r.out - buf.data())};
}

// Segoe Fluent Icons: an eye, a wrench, a bulb (as the model menu's), the chevron down.
constexpr char32_t kIconEye = 0xE7B3, kIconTools = 0xE90F, kIconThink = 0xEA80, kIconChevron = 0xE70D;
// Roles' icons: a chat, a brain-ish lightbulb, a picture.
constexpr char32_t kIconChat = 0xE8BD, kIconHard = 0xEA80, kIconPicture = 0xEB9F;

constexpr float kCardPad = 16, kMono = 34, kModelRow = 42;

void monogram(ui::Ui& ui, const Box& b, std::string_view mono, Color ground, Color ink) {
    ui.renderer().rect({b.x, b.y, b.w, b.h}, {.fill = ground, .radius = {9, 9, 9, 9}});
    const float mw = work::text_width(ui, Face::Semibold, 12.0f, mono);
    work::caption(ui, {b.x + (b.w - mw) / 2, b.y, mw + 1, b.h}, Face::Semibold, 12.0f, mono, ink);
}

} // namespace

auto Painter::draw_models_section(Model& m, float x, float y0, float w, Reaction& r) -> float {
    auto& renderer = ui_->renderer();
    const auto& provider = ai::catalog::default_provider();
    const auto trouble = provider_trouble(m, provider.id);
    const auto& items = m.settings.pages[static_cast<std::size_t>(Section::Models)];
    std::size_t on = 0;
    for (const auto& item : items) {
        on += item.checked ? 1 : 0;
    }
    float y = y0;

    // The provider's card (.pcard): its monogram, name and line, its state, its key's button.
    const float field_h = m.settings.key_open ? 34 + 10 + 18 + 12 : 0.0f;
    const float card_h = kCardPad + kMono + kCardPad + field_h + 28 + static_cast<float>(items.size()) * kModelRow + 8;
    renderer.rect({x, y, w, card_h}, {.fill = ui::th().card, .radius = {16, 16, 16, 16}, .border = ui::th().line, .border_width = 1});
    add_target("settings:provider", {x, y, w, card_h});
    const Box mono{x + kCardPad, y + kCardPad, kMono, kMono};
    monogram(*ui_, mono, provider.monogram, ui::th().w3, ui::th().text_2);
    const float tx = mono.x + kMono + 14;
    work::caption(*ui_, {tx, mono.y - 1, w * 0.5f, 20}, Face::Semibold, 15.0f, provider.name, ui::th().text);
    std::array<char, 96> line{};
    const std::string_view sub = m.has_key ? said(line, "Key saved, encrypted \xC2\xB7 {} of {} models on", on, items.size())
                                           : said(line, "Add a key to use {}'s models", provider.name);
    work::caption(*ui_, {tx, mono.y + 19, w * 0.55f, 16}, Face::Text, 12.5f, sub, ui::th().text_3);

    const bool rejected = trouble == events::Trouble::KeyRejected;
    const std::string_view action = !m.has_key ? "Add key" : rejected ? "Update key" : "Edit key";
    const auto size = work::button_size(*ui_, action);
    const Box edit{x + w - kCardPad - size.w, mono.y + (kMono - size.h) / 2, size.w, size.h};
    add_target("settings:key:edit", edit);
    if (!m.settings.key_open &&
        work::button(*ui_, id_of("key:edit"), edit, action, rejected || !m.has_key ? work::ButtonStyle::Primary : work::ButtonStyle::Normal) &&
        !r.command) {
        edit_key(m);
    }
    const std::string_view state = !m.has_key ? "No key" : rejected ? "Key rejected" : trouble == events::Trouble::Busy ? "Busy" : "Connected";
    const auto tone = !m.has_key ? work::Tone::Plain : rejected ? work::Tone::Crit : trouble == events::Trouble::Busy ? work::Tone::Warn : work::Tone::Ok;
    const auto chip = work::chip_size(*ui_, state);
    const Box state_box{edit.x - 10 - chip.w, mono.y + (kMono - chip.h) / 2, chip.w, chip.h};
    work::chip(*ui_, state_box, state, tone);
    add_target("settings:provider:state", state_box);
    y += kCardPad + kMono + kCardPad;

    // The key's field (stage 4d-2): dots for what is typed, Check and Cancel, the line under it.
    if (m.settings.key_open) {
        const Box field{x + kCardPad, y, w - 2 * kCardPad - 180, 34};
        renderer.rect({field.x, field.y, field.w, field.h}, {.fill = ui::th().w1, .radius = {8, 8, 8, 8}, .border = ui::th().accent, .border_width = 1});
        add_target("settings:key:field", field);
        std::array<char, 3 * 64> dots{};
        std::size_t n = 0;
        for (std::size_t i = 0; i < m.key.text.size() && n + 3 <= dots.size(); ++i) {
            if ((static_cast<unsigned char>(m.key.text[i]) & 0xC0) == 0x80) {
                continue; // a character's tail byte: one dot a character
            }
            dots[n++] = '\xE2';
            dots[n++] = '\x80';
            dots[n++] = '\xA2';
        }
        const std::string_view shown = n == 0 ? std::string_view{"Paste the key\xE2\x80\xA6"} : std::string_view{dots.data(), n};
        const float dw = work::caption(*ui_, {field.x + 12, field.y, field.w - 24, field.h}, Face::Text, 13.0f, shown,
                                       n == 0 ? ui::th().text_4 : ui::th().text);
        renderer.rect({field.x + 12 + (n == 0 ? 0.0f : dw) + 1, field.y + 9, 1.5f, field.h - 18}, {.fill = ui::th().text});
        const auto check_size = work::button_size(*ui_, "Check");
        const Box check{field.x + field.w + 10, field.y + (field.h - check_size.h) / 2, check_size.w, check_size.h};
        add_target("settings:key:check", check);
        if (work::button(*ui_, id_of("key:check"), check, "Check", work::ButtonStyle::Primary, false, !m.key_checking) && !r.command) {
            (void)submit(m);
        }
        const auto cancel_size = work::button_size(*ui_, "Cancel");
        const Box cancel{check.x + check.w + 8, check.y, cancel_size.w, cancel_size.h};
        add_target("settings:key:cancel", cancel);
        if (work::button(*ui_, id_of("key:cancel"), cancel, "Cancel") && !r.command) {
            m.settings.key_open = false;
            m.key = {};
            m.key_error.clear();
        }
        std::array<char, 96> checking{};
        const std::string_view note = m.key_checking        ? said(checking, "Checking the key with {}\xE2\x80\xA6", provider.name)
                                      : !m.key_error.empty() ? std::string_view{m.key_error}
                                                             : std::string_view{"Stored encrypted on this PC, never in plain files"};
        work::caption(*ui_, {field.x, field.y + field.h + 8, w - 2 * kCardPad, 18}, Face::Text, 12.5f, note,
                      m.key_error.empty() ? ui::th().text_3 : ui::th().crit);
        y += field_h;
    }

    // Its models (.mlist): the name, what it can do, its context and price, on or off in the menu.
    renderer.rect({x, y, w, 1}, {.fill = ui::th().line});
    const float c_can = x + w * 0.58f, c_ctx = x + w * 0.72f, c_cost = x + w * 0.82f;
    for (const auto& [label, at] : {std::pair{"MODEL", x + kCardPad + 48}, std::pair{"CAN DO", c_can}, std::pair{"CONTEXT", c_ctx},
                                    std::pair{"COST", c_cost}}) {
        work::caption(*ui_, {at, y + 6, 120, 18}, Face::Semibold, 10.5f, label, ui::th().text_4);
    }
    y += 28;
    const auto all = ai::catalog::models();
    std::array<char, 64> buf{};
    for (std::size_t i = 0; i < items.size() && i < all.size(); ++i) {
        const auto& model = all[i];
        const auto& item = items[i];
        const float cy = y + kModelRow / 2;
        work::caption(*ui_, {x + kCardPad + 48, y, c_can - x - kCardPad - 60, kModelRow}, Face::Semibold, 13.5f, model.short_name,
                      item.checked ? ui::th().text : ui::th().text_3);
        float ix = c_can;
        for (const auto& [can, icon] : {std::pair{model.vision, kIconEye}, std::pair{model.tools, kIconTools}, std::pair{model.thinking, kIconThink}}) {
            if (can == ai::catalog::Tri::Yes) {
                ui_->draw_icon(icon, 14.0f, {ix + 7, cy}, ui::th().text_3);
                ix += 22;
            }
        }
        work::caption(*ui_, {c_ctx, y, 80, kModelRow}, Face::Text, 12.5f, picker::context_text(model.context_tokens, buf), ui::th().text_3);
        work::caption(*ui_, {c_cost, y, 60, kModelRow}, Face::Text, 12.5f, picker::price_tier(model), ui::th().text_3);
        const Box sw{x + w - kCardPad - 40, cy - 11, 40, 22};
        std::array<char, 64> name{};
        add_target(said(name, "settings:item:{}", item.id), sw);
        if (work::toggle(*ui_, id_of("model", item.id), sw, item.checked) && !r.command) {
            r.setting = item.id;
        }
        y += kModelRow;
    }
    y += 8 + 14;

    // The month (.usage): what it cost, its tokens, a bar by provider.
    const auto total = spend::total(m.settings.spend);
    const float uh = 66;
    renderer.rect({x, y, w, uh}, {.fill = ui::th().card, .radius = {16, 16, 16, 16}, .border = ui::th().line, .border_width = 1});
    add_target("settings:spend", {x, y, w, uh});
    std::array<char, 32> usd{};
    std::array<char, 32> tokens{};
    const float uw = work::caption(*ui_, {x + kCardPad, y + 12, 140, 24}, Face::Semibold, 18.0f, said(usd, "${:.2f}", total.usd), ui::th().text);
    work::caption(*ui_, {x + kCardPad, y + 38, 140, 16}, Face::Text, 12.0f, "this month", ui::th().text_3);
    const float tk = x + kCardPad + std::max(uw, 80.0f) + 24;
    const double t = static_cast<double>(total.tokens);
    const float tw = work::caption(*ui_, {tk, y + 12, 140, 24}, Face::Semibold, 18.0f,
                                   t >= 1e6 ? said(tokens, "{:.1f} M", t / 1e6) : t >= 1e3 ? said(tokens, "{:.0f} K", t / 1e3) : said(tokens, "{}", total.tokens),
                                   ui::th().text);
    work::caption(*ui_, {tk, y + 38, 140, 16}, Face::Text, 12.0f, "tokens", ui::th().text_3);
    const float bx = tk + std::max(tw, 60.0f) + 24;
    const float bw = std::max(0.0f, x + w - kCardPad - bx);
    renderer.rect({bx, y + 20, bw, 6}, {.fill = ui::th().w2, .radius = {3, 3, 3, 3}});
    if (total.usd > 0) {
        renderer.rect({bx, y + 20, bw, 6}, {.fill = ui::th().c1, .radius = {3, 3, 3, 3}}); // one provider: the whole bar
    }
    std::array<char, 64> legend{};
    const auto it = std::ranges::find(m.settings.spend.providers, provider.id, &spend::Total::provider);
    renderer.rect({bx, y + 40, 8, 8}, {.fill = ui::th().c1, .radius = {2, 2, 2, 2}});
    work::caption(*ui_, {bx + 13, y + 34, bw - 13, 20}, Face::Text, 12.0f,
                  said(legend, "{} ${:.2f}", provider.name, it != m.settings.spend.providers.end() ? it->usd : 0.0), ui::th().text_3);
    y += uh;
    return y - y0;
}

auto Painter::draw_roles_section(Model& m, float x, float y0, float w, Reaction& r) -> float {
    auto& renderer = ui_->renderer();
    float y = y0;
    struct RoleRow {
        char32_t icon;
        std::string_view title, when, need;
        const ai::routing::Role* role;
        ai::routing::Need kind;
    };
    const std::array<RoleRow, 3> rows{{
        {kIconChat, "Everyday chat and tasks", "Questions, files, system checks", {}, &m.roles.chat, ai::routing::Need::Chat},
        {kIconHard, "Hard problems", "When you pick High or Max thinking", {}, &m.roles.hard, ai::routing::Need::Hard},
        {kIconPicture, "Images and screenshots", "A picture in the conversation", "needs vision", &m.roles.images, ai::routing::Need::Images},
    }};
    const auto& chosen = model_of(effective(m));
    constexpr float kRole = 84;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];
        renderer.rect({x, y, w, kRole - 10}, {.fill = ui::th().card, .radius = {14, 14, 14, 14}, .border = ui::th().line, .border_width = 1});
        const Box icon{x + 14, y + 16, 34, 34};
        renderer.rect({icon.x, icon.y, icon.w, icon.h}, {.fill = ui::th().w2, .radius = {10, 10, 10, 10}});
        ui_->draw_icon(row.icon, 16.0f, {icon.x + 17, icon.y + 17}, ui::th().text_2);
        work::caption(*ui_, {icon.x + 48, y + 14, w * 0.45f, 20}, Face::Semibold, 14.0f, row.title, ui::th().text);
        work::caption(*ui_, {icon.x + 48, y + 34, w * 0.45f, 16}, Face::Text, 12.5f, row.when, ui::th().text_3);
        if (!row.need.empty()) {
            work::caption(*ui_, {icon.x + 48, y + 51, w * 0.45f, 16}, Face::Semibold, 11.0f, row.need, ui::th().accent_2);
        }
        // Its model, as a request of that kind would go now (the role's own, or the one chosen).
        const auto* first = row.kind == ai::routing::Need::Chat || row.role->model.empty() ? &chosen : ai::catalog::model(row.role->model);
        const auto& model = first != nullptr ? *first : chosen;
        const auto& by = ai::catalog::provider_of(model);
        const float mx = x + w * 0.52f;
        const Box pick{mx, y + 12, w - (mx - x) - 14, 30};
        renderer.rect({pick.x, pick.y, pick.w, pick.h}, {.fill = ui::th().w1, .radius = {8, 8, 8, 8}, .border = ui::th().line, .border_width = 1});
        monogram(*ui_, {pick.x + 5, pick.y + 5, 20, 20}, by.monogram, ui::th().w3, ui::th().text_2);
        work::caption(*ui_, {pick.x + 32, pick.y, pick.w - 60, pick.h}, Face::Semibold, 13.0f, model.full_name, ui::th().text);
        ui_->draw_icon(kIconChevron, 11.0f, {pick.x + pick.w - 14, pick.y + pick.h / 2}, ui::th().text_3);
        std::array<char, 64> name{};
        add_target(said(name, "settings:role:{}", i), pick);
        settings_role_boxes_[i] = pick;
        if (ui_->interact(id_of("role", i), pick, false).clicked && !r.command) { // the same model menu, for this role
            if (m.menu.open) {
                close_menu(m);
            } else if (row.kind == ai::routing::Need::Chat) {
                open_menu(m); // Everyday chat's model is the conversation's
            } else {
                open_role_menu(m, row.kind);
            }
        }
        // Behind it: the models that stand in, in order (shown, not set, in 4d-2).
        std::array<char, 160> behind{};
        std::size_t n = 0;
        const auto put = [&](std::string_view s) {
            const auto k = std::min(s.size(), behind.size() - n);
            std::copy_n(s.data(), k, behind.data() + n);
            n += k;
        };
        put("if unavailable");
        std::size_t stands = 0;
        for (const auto& id : row.role->fallbacks) {
            const auto* f = ai::catalog::model(id);
            if (f == nullptr || f == &model || std::ranges::find(m.disabled, f->id) != m.disabled.end()) {
                continue;
            }
            put(" \xE2\x86\x92 ");
            put(f->short_name);
            ++stands;
        }
        if (stands == 0) {
            put(" \xE2\x86\x92 none");
        }
        work::caption(*ui_, {pick.x + 4, pick.y + pick.h + 6, pick.w, 16}, Face::Text, 12.0f, {behind.data(), n}, ui::th().text_4);
        y += kRole;
    }
    return y - y0;
}

} // namespace app::sheet
