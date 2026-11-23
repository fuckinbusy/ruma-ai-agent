// The model menu (UI update, stage 4b; MODEL_PICKER_SPEC §§ 1–6, 10): a pop under the bar's row,
// over the frame — its search, a group for each provider, a row for each model, its thinking foot,
// and a row's tooltip to its left. Its state is sheet::Model::menu; here it is only drawn and
// clicked. Nothing is allocated in its frames: its words are written into buffers on the stack.

#include "app/sheet_view.hpp"

#include "core/hash.hpp"
#include "ui/theme.hpp"

#include <algorithm>
#include <array>
#include <format>

namespace app::sheet {
namespace {

namespace work = ui::work;
using ui::Face;
using ui::layout::Box;
using ui::render::Color;

auto id_of(std::string_view name) -> ui::Id {
    return core::hash::fnv1a64(name, core::hash::fnv1a64("app/sheet"));
}

auto rect(const Box& b) -> ui::render::Rect {
    return {b.x, b.y, b.w, b.h};
}

auto all(float r) -> ui::render::Corners {
    return {r, r, r, r};
}

auto inside(const Box& b, float x, float y) -> bool {
    return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}

auto said(std::span<char> buf, auto made) -> std::string_view {
    return {buf.data(), static_cast<std::size_t>(made.out - buf.data())};
}

// MODEL_PICKER_SPEC § 1, in its px.
constexpr float kWidth = 520, kPad = 8, kRadius = 16, kSearchH = 40, kAfterSearch = 4;
constexpr float kHeadH = 26; // 10 above, 11 px caps, 5 below
constexpr float kRowH = 42, kRowRadius = 10, kMono = 28, kIcon = 14, kIconGap = 5, kContextW = 46, kPriceW = 30, kGap = 10;
constexpr float kFootH = 55;  // 8 apart, its line, 10 above, the switch 33, 4 below
constexpr float kEmptyH = 72; // "No models match …"
constexpr float kMostH = 640;
constexpr float kTipW = 300, kTipPadX = 12, kTipPadY = 9, kTipLine = 12.5f * 1.45f;
constexpr double kTipWait = 0.5;
constexpr std::size_t kNamedRows = 16; // rows with a target's name (the catalog has fewer)

constexpr std::array<std::string_view, kNamedRows> kRowNames{
    "menu:row:0", "menu:row:1", "menu:row:2",  "menu:row:3",  "menu:row:4",  "menu:row:5",  "menu:row:6",  "menu:row:7",
    "menu:row:8", "menu:row:9", "menu:row:10", "menu:row:11", "menu:row:12", "menu:row:13", "menu:row:14", "menu:row:15"};
constexpr std::array<std::string_view, kNamedRows> kActionNames{
    "menu:action:0", "menu:action:1", "menu:action:2",  "menu:action:3",  "menu:action:4",  "menu:action:5",  "menu:action:6",  "menu:action:7",
    "menu:action:8", "menu:action:9", "menu:action:10", "menu:action:11", "menu:action:12", "menu:action:13", "menu:action:14", "menu:action:15"};
constexpr std::array<std::string_view, 4> kLevelNames{"menu:level:0", "menu:level:1", "menu:level:2", "menu:level:3"};
constexpr std::array<std::string_view, 4> kLevelWords{"Off", "Low", "High", "Max"};

constexpr char32_t kIconSearch = 0xE721, kIconEye = 0xE7B3, kIconTools = 0xE90F, kIconThink = 0xEA80;

constexpr ui::anim::Id kMenuAlphaTrack = 60, kMenuShiftTrack = 61, kMenuLitTrack = 62, kMenuHeightTrack = 63, kMenuLevelTrack = 64,
                       kMenuTipTrack = 65, kMenuTipYTrack = 66;

auto row_name(std::size_t i) -> std::string_view {
    return i < kRowNames.size() ? kRowNames[i] : std::string_view{"menu:row"};
}

auto upper(std::string_view s, std::span<char> buf) -> std::string_view {
    const std::size_t n = std::min(s.size(), buf.size());
    for (std::size_t i = 0; i < n; ++i) {
        const char c = s[i];
        buf[i] = c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
    }
    return {buf.data(), n};
}

auto status_color(events::Trouble t, bool searching) -> Color {
    if (searching) {
        return ui::th().text_4;
    }
    switch (t) {
    case events::Trouble::KeyRejected:
    case events::Trouble::Balance: return ui::th().crit;
    case events::Trouble::Busy: return ui::th().warn;
    case events::Trouble::Offline:
    case events::Trouble::NoKey:
    case events::Trouble::None: break;
    }
    return ui::th().text_3;
}

} // namespace

auto Painter::menu_height(const Model& m) const -> float {
    float list = 0;
    for (const auto& g : m.menu.groups) {
        list += kHeadH + static_cast<float>(g.rows.size()) * kRowH;
    }
    if (m.menu.groups.empty()) {
        list = kEmptyH;
    }
    const float whole = kPad + kSearchH + kAfterSearch + list + kFootH + kPad;
    const float room = room_.h > 0 ? room_.h - (row_h_ + 8) - 16 : kMostH;
    return std::max(kPad + kSearchH + kFootH + kPad, std::min({whole, kMostH, room}));
}

void Painter::draw_menu(Model& m, float row_top, float right, Reaction& r) {
    auto& rr = ui_->renderer();
    const auto& in = ui_->input();
    const bool open = m.menu.open && m.open && !m.key_mode;
    // § 10: in on quick, a 6 px rise on base; out on micro. Windows' effects off: only the fade.
    const float alpha = anim_.to(kMenuAlphaTrack, open ? 1.0f : 0.0f, open ? ui::motion::kQuick : ui::motion::kMicro);
    const float shift = reduced_motion_ ? 0.0f : anim_.to(kMenuShiftTrack, open ? 0.0f : -6.0f, ui::motion::kBase);
    if (!open && alpha < 0.002f) {
        anim_.place(kMenuShiftTrack, -6.0f); // the next opening rises again
        menu_alpha_ = 0;
        menu_tip_up_ = false;
        menu_tip_row_ = static_cast<std::size_t>(-1);
        return;
    }
    if (menu_alpha_ <= 0.0f) { // a fresh opening: the lit plate, the height and the level stand where they are
        anim_.place(kMenuHeightTrack, menu_height(m));
        menu_place_ = true;
        menu_mouse_x_ = in.mouse_x; // a mouse resting where a row comes lights nothing (review M1)
        menu_mouse_y_ = in.mouse_y;
    }
    menu_alpha_ = alpha;
    // A press outside it closes it (§ 9); the chip and the orb toggle it themselves.
    if (open && in.mouse_pressed && !inside(menu_box_, in.mouse_x, in.mouse_y) && !inside(model_chip_.box, in.mouse_x, in.mouse_y) &&
        !inside(orb_controls_, in.mouse_x, in.mouse_y)) {
        close_menu(m);
        return;
    }
    const float h = anim_.geometry(kMenuHeightTrack, menu_height(m), ui::motion::kBase);
    const Box menu{right - 18 - kWidth, row_top + row_h_ + 8 + shift, kWidth, h};
    menu_box_ = menu;
    const float o = rr.opacity();
    rr.set_opacity(o * alpha);
    ui_->set_inert(!open);
    ui_->begin_overlay(menu);
    work::pop_surface(*ui_, menu, kRadius);
    if (open) {
        add_target("menu", menu);
    }
    rr.push_clip(rect(menu));

    // The search (§ 1): sunk, the icon 15, the text 14, Ctrl M at its right.
    const Box search{menu.x + kPad, menu.y + kPad, menu.w - 2 * kPad, kSearchH};
    rr.rect(rect(search), {.fill = ui::th().sunk, .radius = all(10), .border = ui::th().line, .border_width = 1});
    ui_->draw_icon(kIconSearch, 15.0f, {search.x + 12 + 7.5f, search.y + kSearchH / 2}, ui::th().text_3);
    const float tx = search.x + 12 + 15 + 10;
    const bool empty_query = m.menu.query.text.empty();
    const float typed = work::caption(*ui_, {tx, search.y, search.w - (tx - search.x) - 70, kSearchH}, Face::Text, 14.0f,
                                    empty_query ? std::string_view{"Search models or providers\xE2\x80\xA6"} : std::string_view{m.menu.query.text},
                                    empty_query ? ui::th().text_4 : ui::th().text);
    if (!empty_query) { // the caret after what is typed, steady; empty, the placeholder says where typing goes
        rr.rect({tx + typed + 1, search.y + 11, 1.5f, kSearchH - 22}, {.fill = ui::th().accent});
    }
    // Ctrl M as a key's cap at its right.
    const float kw = work::text_width(*ui_, Face::Text, 11.0f, "Ctrl M");
    const Box cap{search.x + search.w - 10 - kw - 12, search.y + (kSearchH - 20) / 2, kw + 12, 20};
    rr.rect(rect(cap), {.fill = {}, .radius = all(5), .border = ui::th().line_2, .border_width = 1});
    work::caption(*ui_, {cap.x + 6, cap.y, kw + 1, cap.h}, Face::Text, 11.0f, "Ctrl M", ui::th().text_3);
    if (open) {
        add_target("menu:search", search);
    }

    // The list: a group's header, its rows; the one lit row's plate slides between them (§ 10).
    const float list_top = search.y + kSearchH + kAfterSearch;
    const float foot_top = menu.y + menu.h - kPad - kFootH;
    rr.push_clip({menu.x, list_top, menu.w, std::max(0.0f, foot_top - list_top)});
    const std::string_view chosen = effective(m).model;
    const bool searching = !empty_query;
    const bool moved = in.mouse_x != menu_mouse_x_ || in.mouse_y != menu_mouse_y_;
    menu_mouse_x_ = in.mouse_x;
    menu_mouse_y_ = in.mouse_y;
    float y = list_top;
    std::size_t index = 0;
    Box lit_row{};
    const ai::catalog::Model* lit_model = nullptr;
    // Where the lit row is, before any row is drawn: its plate goes under them.
    {
        float ly = list_top;
        std::size_t li = 0;
        for (const auto& g : m.menu.groups) {
            ly += kHeadH;
            for (const auto& row : g.rows) {
                if (li == m.menu.active) {
                    lit_row = {menu.x + kPad, ly, menu.w - 2 * kPad, kRowH};
                    lit_model = row.model;
                }
                ly += kRowH;
                ++li;
            }
        }
    }
    if (lit_model != nullptr) {
        if (menu_place_) {
            anim_.place(kMenuLitTrack, lit_row.y);
        }
        const float ly = anim_.geometry(kMenuLitTrack, lit_row.y, ui::motion::kQuick);
        const Box plate{lit_row.x, ly, lit_row.w, kRowH};
        if (lit_model->id != chosen) { // the chosen one's own ground lights instead (§ 3)
            rr.rect(rect(plate), {.fill = ui::th().w1, .radius = all(kRowRadius),
                                  .border = m.menu.by_keys ? ui::th().accent : Color{}, .border_width = m.menu.by_keys ? 1.5f : 0.0f});
        }
        if (open) {
            add_target("menu:lit", plate);
        }
    }
    for (const auto& g : m.menu.groups) {
        // The header (§ 1, § 4): the provider in caps, its state at the right.
        std::array<char, 64> caps{};
        const Box head{menu.x + kPad + 10, y, menu.w - 2 * kPad - 20, kHeadH};
        work::caption(*ui_, {head.x, head.y + 10, head.w / 2, 13}, Face::Semibold, 11.0f, upper(g.provider->name, caps), ui::th().text_4);
        std::array<char, 32> status_buf{};
        if (const auto status = picker::status_text(g, searching, status_buf); !status.empty()) {
            const Color c = status_color(g.trouble, searching);
            const float sw = work::text_width(*ui_, Face::Text, 11.0f, status);
            work::caption(*ui_, {head.x + head.w - sw, head.y + 10, sw + 1, 13}, Face::Text, 11.0f, status, c);
            if (!searching) {
                rr.rect({head.x + head.w - sw - 11, head.y + 14, 6, 6}, {.fill = c, .radius = all(3)});
            }
        }
        y += kHeadH;
        for (const auto& row : g.rows) {
            const Box box{menu.x + kPad, y, menu.w - 2 * kPad, kRowH};
            const ui::Id id = id_of(row_name(index));
            const auto hit = ui_->interact(id, box, false);
            if (open && hit.hovered && moved) {
                hover_row(m, index); // the mouse lights the row it moves to; the keys' row stays otherwise
            }
            const bool selected = row.model->id == chosen;
            const bool active = index == m.menu.active;
            if (selected) {
                rr.rect(rect(box), {.fill = active ? ui::th().accent_soft_hi : ui::th().accent_soft, .radius = all(kRowRadius),
                                    .border = ui::th().accent_line, .border_width = 1});
            }
            const float ro = rr.opacity();
            if (row.dimmed) {
                rr.set_opacity(ro * 0.55f);
            }
            // [monogram] name … capabilities, context, price (§ 2).
            const Box mono{box.x + 8, box.y + (kRowH - kMono) / 2, kMono, kMono};
            rr.rect(rect(mono), {.fill = selected ? ui::th().accent : ui::th().w3, .radius = all(8)});
            const auto& provider = *g.provider;
            const float mw = work::text_width(*ui_, Face::Semibold, 11.0f, provider.monogram);
            work::caption(*ui_, {mono.x + (kMono - mw) / 2, mono.y, mw + 1, kMono}, Face::Semibold, 11.0f, provider.monogram,
                        selected ? ui::th().on_accent : ui::th().text);
            const float right_x = box.x + box.w - 10;
            float tail = right_x; // where the name must end
            if (row.dimmed) {
                if (!row.action.empty()) { // its action, a link of the accent at the right (§ 3)
                    const float aw = work::text_width(*ui_, Face::Semibold, 12.5f, row.action);
                    const Box link{right_x - aw, box.y, aw + 1, kRowH};
                    work::caption(*ui_, link, Face::Semibold, 12.5f, row.action, ui::th().accent);
                    if (open && index < kActionNames.size()) {
                        add_target(kActionNames[index], link);
                    }
                    tail = link.x - kGap;
                }
            } else {
                const std::string_view price = picker::price_tier(*row.model);
                const bool is_free = price == "free";
                const float pw = work::text_width(*ui_, is_free ? Face::Text : Face::Mono, 11.0f, price);
                work::caption(*ui_, {right_x - pw, box.y, pw + 1, kRowH}, is_free ? Face::Text : Face::Mono, 11.0f, price,
                            is_free ? ui::th().ok : ui::th().text_3);
                std::array<char, 16> ctx_buf{};
                const auto ctx = picker::context_text(row.model->context_tokens, ctx_buf);
                const float cw = work::text_width(*ui_, Face::Semibold, 11.5f, ctx);
                const float ctx_right = right_x - kPriceW - kGap;
                work::caption(*ui_, {ctx_right - cw, box.y, cw + 1, kRowH}, Face::Semibold, 11.5f, ctx, ui::th().text_4);
                // Eye, wrench, lamp, in this order, pressed to the right; none for what it cannot (§ 2).
                float ix = ctx_right - kContextW - kGap;
                const std::array<std::pair<ai::catalog::Tri, char32_t>, 3> caps_of{
                    {{row.model->thinking, kIconThink}, {row.model->tools, kIconTools}, {row.model->vision, kIconEye}}};
                for (const auto& [can, icon] : caps_of) {
                    if (can == ai::catalog::Tri::No) {
                        continue;
                    }
                    if (can == ai::catalog::Tri::Unknown) {
                        rr.rect({ix - kIcon, box.y + (kRowH - kIcon) / 2, kIcon, kIcon},
                                {.fill = {}, .radius = all(3), .border = ui::th().line_2, .border_width = 1});
                        work::caption(*ui_, {ix - kIcon + 4, box.y, kIcon, kRowH}, Face::Text, 10.0f, "?", ui::th().text_4);
                    } else {
                        ui_->draw_icon(icon, kIcon, {ix - kIcon / 2, box.y + kRowH / 2}, ui::th().text_3);
                    }
                    ix -= kIcon + kIconGap;
                }
                tail = ix - kGap + kIconGap;
            }
            // The name, the search's match in the accent (§ 8).
            const float nx = mono.x + kMono + kGap;
            const Box name_box{nx, box.y, std::max(0.0f, tail - nx), kRowH};
            const std::string_view name = row.model->short_name;
            const Color ink = selected ? ui::th().accent : ui::th().text;
            if (row.match_len == 0 || row.match_from + row.match_len > name.size()) {
                work::caption(*ui_, name_box, Face::Semibold, 14.0f, name, ink);
            } else {
                const auto before = name.substr(0, row.match_from);
                const auto match = name.substr(row.match_from, row.match_len);
                const auto after = name.substr(row.match_from + row.match_len);
                float x = nx;
                x += before.empty() ? 0.0f : work::caption(*ui_, {x, box.y, name_box.w, kRowH}, Face::Semibold, 14.0f, before, ink);
                x += work::caption(*ui_, {x, box.y, std::max(0.0f, nx + name_box.w - x), kRowH}, Face::Semibold, 14.0f, match, ui::th().accent);
                if (!after.empty()) {
                    work::caption(*ui_, {x, box.y, std::max(0.0f, nx + name_box.w - x), kRowH}, Face::Semibold, 14.0f, after, ink);
                }
            }
            rr.set_opacity(ro);
            if (open) {
                add_target(row_name(index), box);
                if (hit.clicked) {
                    const auto got = pick(m, index, false);
                    if (got.choose) {
                        r.choose = got.choose;
                    }
                }
            }
            y += kRowH;
            ++index;
        }
    }
    if (m.menu.groups.empty()) { // § 8: nothing found
        ui_->draw_icon(kIconSearch, 18.0f, {menu.x + menu.w / 2, list_top + 22}, ui::th().text_4);
        std::array<char, 160> buf{};
        const auto said_none = said(buf, std::format_to_n(buf.data(), static_cast<std::ptrdiff_t>(buf.size()),
                                                          "No models match \xE2\x80\x9C{}\xE2\x80\x9D", m.menu.query.text));
        const float w = std::min(menu.w - 40, work::text_width(*ui_, Face::Text, 13.0f, said_none));
        work::caption(*ui_, {menu.x + (menu.w - w) / 2, list_top + 38, w + 1, 22}, Face::Text, 13.0f, said_none, ui::th().text_3);
    }
    rr.pop_clip();

    // The foot (§ 1, § 6): THINKING and its switch; a level the model does not take dimmed and struck.
    rr.rect({menu.x + kPad, foot_top + 8, menu.w - 2 * kPad, 1}, {.fill = ui::th().line});
    const float fy = foot_top + 8 + 1 + 10;
    work::caption(*ui_, {menu.x + kPad + 10, fy, 80, 33}, Face::Semibold, 11.0f, "THINKING", ui::th().text_4);
    const ModelChoice now_choice = effective(m);
    const auto& now_model = model_of(now_choice);
    std::array<float, 4> item_w{};
    float track_w = 6;
    for (std::size_t i = 0; i < kLevelWords.size(); ++i) {
        item_w[i] = work::text_width(*ui_, Face::Semibold, 12.5f, kLevelWords[i]) + 22;
        track_w += item_w[i];
    }
    const Box track{menu.x + kPad + 10 + 80, fy, track_w, 33};
    rr.rect(rect(track), {.fill = ui::th().sunk, .radius = all(10), .border = ui::th().line, .border_width = 1});
    const int shown_level = level_for(now_model, now_choice.level);
    float ix = track.x + 3;
    float chosen_x = ix;
    for (std::size_t i = 0; i < kLevelWords.size(); ++i) {
        if (static_cast<int>(i) == shown_level) {
            chosen_x = ix;
        }
        ix += item_w[i];
    }
    if (menu_place_) {
        anim_.place(kMenuLevelTrack, chosen_x);
        menu_place_ = false;
    }
    const float plate_x = anim_.geometry(kMenuLevelTrack, chosen_x, ui::motion::kBase);
    rr.rect({plate_x, track.y + 3, item_w[static_cast<std::size_t>(std::clamp(shown_level, 0, 3))], 27}, {.fill = ui::th().accent, .radius = all(7)});
    ix = track.x + 3;
    bool previewing = false;
    for (std::size_t i = 0; i < kLevelWords.size(); ++i) {
        const Box item{ix, track.y + 3, item_w[i], 27};
        const bool takes = ai::catalog::has_level(now_model, static_cast<ai::Thinking>(i));
        const auto hit = ui_->interact(id_of(kLevelNames[i]), item, false);
        const float lo = rr.opacity();
        if (!takes) {
            rr.set_opacity(lo * 0.45f);
        }
        const float lw = item_w[i] - 22;
        work::caption(*ui_, {item.x + 11, item.y, lw + 1, 27}, Face::Semibold, 12.5f, kLevelWords[i],
                    static_cast<int>(i) == shown_level ? ui::th().on_accent : ui::th().text_2);
        if (!takes) {
            rr.rect({item.x + 9, item.y + 13.5f, lw + 4, 1}, {.fill = ui::th().line_2});
        }
        rr.set_opacity(lo);
        if (open) {
            add_target(kLevelNames[i], item);
            if (hit.hovered && takes) { // the orb shows it; leaving brings it back (§ 6)
                m.preview = ModelChoice{now_choice.model, static_cast<int>(i)};
                previewing = true;
            }
            if (hit.clicked && takes) {
                ModelChoice c = now_choice;
                c.level = static_cast<int>(i);
                const auto got = set_choice(m, c, m.next_only.has_value(), now_);
                if (got.choose) {
                    r.choose = got.choose;
                }
            }
        }
        ix += item_w[i];
    }
    if (!previewing && open) {
        m.preview.reset();
    }
    if (now_model.thinking == ai::catalog::Tri::No) { // § 6: why nothing is lit but Off
        std::array<char, 96> why{};
        const auto text = said(why, std::format_to_n(why.data(), static_cast<std::ptrdiff_t>(why.size()), "{} can't think", now_model.short_name));
        const float ww = work::text_width(*ui_, Face::Text, 11.5f, text);
        work::caption(*ui_, {menu.x + menu.w - kPad - 10 - ww, fy, ww + 1, 33}, Face::Text, 11.5f, text, ui::th().text_4);
    }
    rr.pop_clip();
    ui_->end_overlay();
    ui_->set_inert(false);

    // The lit row's tooltip (§ 5): after 500 ms on it, mouse or keys; from row to row without a new wait.
    if (open && lit_model != nullptr) {
        if (menu_tip_row_ != m.menu.active) {
            menu_tip_row_ = m.menu.active;
            menu_tip_from_ = menu_tip_up_ ? now_ - kTipWait : now_;
        }
        const bool over = inside(menu, in.mouse_x, in.mouse_y) || m.menu.by_keys;
        if (over && now_ - menu_tip_from_ >= kTipWait) {
            if (!menu_tip_up_) { // it comes in at its row, from nothing
                anim_.place(kMenuTipYTrack, lit_row.y);
                anim_.place(kMenuTipTrack, 0.0f);
            }
            menu_tip_up_ = true;
            draw_menu_tip(m, *lit_model, lit_row, menu);
        } else {
            menu_tip_up_ = false;
            if (over) {
                redraw_due_ = true; // its wait passes on frames
            } else {
                menu_tip_from_ = now_; // the wait starts when the mouse comes in (or the keys move)
            }
        }
    }
    rr.set_opacity(o);
}

void Painter::draw_menu_tip(const Model& m, const ai::catalog::Model& model, const Box& row, const Box& menu) {
    auto& rr = ui_->renderer();
    // Its lines: the full name; what it can; what it cannot and where that goes instead; the context
    // and about what an answer costs.
    struct Line {
        std::string_view text;
        Face face;
        Color color;
        char32_t icon;
    };
    std::array<Line, 7> lines{};
    std::size_t n = 0;
    lines[n++] = {model.full_name, Face::Semibold, ui::th().text, 0};
    if (model.vision == ai::catalog::Tri::Yes) {
        lines[n++] = {"Sees images", Face::Text, ui::th().text_2, kIconEye};
    }
    if (model.tools == ai::catalog::Tri::Yes) {
        lines[n++] = {"Runs commands and tools", Face::Text, ui::th().text_2, kIconTools};
    }
    if (model.thinking == ai::catalog::Tri::Yes) {
        lines[n++] = {"Thinks before answering", Face::Text, ui::th().text_2, kIconThink};
    }
    std::array<char, 128> instead{};
    if (model.vision == ai::catalog::Tri::No) {
        const auto* to = ai::catalog::beside(model).vision;
        lines[n++] = to != nullptr ? Line{said(instead, std::format_to_n(instead.data(), static_cast<std::ptrdiff_t>(instead.size()),
                                                                        "Can't see images \xE2\x86\x92 they go to {}", to->full_name)),
                                          Face::Text, ui::th().warn, 0}
                                   : Line{"Can't see images", Face::Text, ui::th().text_3, 0};
    } else if (model.vision == ai::catalog::Tri::Unknown) {
        lines[n++] = {"Image support unknown \xE2\x80\x94 I'll check before relying on it", Face::Text, ui::th().text_3, 0};
    }
    std::array<char, 16> ctx_buf{};
    std::array<char, 96> last{};
    const double usd = answer_cost(m, model);
    const auto ctx = picker::context_text(model.context_tokens, ctx_buf);
    lines[n++] = {said(last, usd < 0.001 ? std::format_to_n(last.data(), static_cast<std::ptrdiff_t>(last.size()),
                                                            "{} context \xC2\xB7 about ${:.4f} per answer", ctx, usd)
                                         : std::format_to_n(last.data(), static_cast<std::ptrdiff_t>(last.size()),
                                                            "{} context \xC2\xB7 about ${:.3f} per answer", ctx, usd)),
                  Face::Text, ui::th().text_2, 0};
    float w = 0;
    for (std::size_t i = 0; i < n; ++i) {
        w = std::max(w, work::text_width(*ui_, lines[i].face, 12.5f, lines[i].text) + (lines[i].icon != 0 ? 20.0f : 0.0f));
    }
    w = std::min(kTipW, w + 2 * kTipPadX);
    const float h = 2 * kTipPadY + static_cast<float>(n) * kTipLine;
    // Left of the menu, its top at the row's; no room there: to its right. It moves to the next
    // row on quick, and never covers the menu.
    float x = menu.x - 8 - w;
    if (x < glass_.x + 4) {
        x = menu.x + menu.w + 8;
    }
    const float y = anim_.geometry(kMenuTipYTrack, row.y, ui::motion::kQuick);
    const float alpha = anim_.to(kMenuTipTrack, 1.0f, ui::motion::kQuick);
    const float o = rr.opacity();
    rr.set_opacity(o * alpha);
    const Box tip{x, y, w, h};
    work::pop_surface(*ui_, tip, 10);
    float ly = tip.y + kTipPadY;
    for (std::size_t i = 0; i < n; ++i) {
        float lx = tip.x + kTipPadX;
        if (lines[i].icon != 0) {
            ui_->draw_icon(lines[i].icon, 13.0f, {lx + 6.5f, ly + kTipLine / 2}, ui::th().text_3);
            lx += 20;
        }
        work::caption(*ui_, {lx, ly, tip.x + tip.w - kTipPadX - lx, kTipLine}, lines[i].face, 12.5f, lines[i].text, lines[i].color);
        ly += kTipLine;
    }
    rr.set_opacity(o);
    add_target("menu:tip", tip);
}

} // namespace app::sheet
