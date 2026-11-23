// The permission screen of design/interaction-topbar (2_permission): the policy asks, and the
// card shows what exactly — the kind of action, how much, one row per file with a tick, "to the
// Recycle Bin" or (only in advanced mode) "permanently", "don't ask again for this folder" where
// the policy allows it — and beside it why: the access levels, the one that asks marked, each
// changeable only towards stricter. The facts are the policy's (canonical paths), never the
// agent's own words.

#include "app/sheet_view.hpp"

#include "core/hash.hpp"
#include "ui/theme.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <span>
#include <string>
#include <utility>

namespace app::sheet {
namespace {

namespace theme = ui::theme;
namespace widgets = ui::widgets;
namespace work = ui::work;
namespace wt = ui::theme::work;
using ui::Face;
using ui::layout::Box;
using ui::render::Color;

auto id_of(std::string_view name) -> ui::Id {
    return core::hash::fnv1a64(name, core::hash::fnv1a64("app/sheet"));
}

auto id_of(std::string_view name, std::size_t n) -> ui::Id {
    return core::hash::fnv1a64(name, core::hash::fnv1a64("app/sheet")) * 1099511628211ull + n;
}

template <std::size_t N, class... Args>
auto text(std::array<char, N>& buf, std::format_string<Args...> f, Args&&... args) -> std::string_view {
    const auto r = std::format_to_n(buf.data(), buf.size(), f, std::forward<Args>(args)...);
    return {buf.data(), static_cast<std::size_t>(r.out - buf.data())};
}

auto with_alpha(Color c, float k) -> Color {
    c.a = static_cast<std::uint8_t>(std::lround(static_cast<float>(c.a) * std::clamp(k, 0.0f, 1.0f)));
    return c;
}

// An edit's diff in its preview (fs.edit, fs.revert: phase 12 § 1): from its first hunk on, or none.
auto diff_of(std::string_view preview) -> std::string_view {
    for (std::size_t at = 0; at < preview.size();) {
        if (preview.substr(at).starts_with("@@ ")) {
            return preview.substr(at);
        }
        const auto end = preview.find('\n', at);
        if (end == std::string_view::npos) {
            break;
        }
        at = end + 1;
    }
    return {};
}

auto line_count(std::string_view s) -> std::size_t {
    const auto n = static_cast<std::size_t>(std::ranges::count(s, '\n'));
    return n + (!s.empty() && s.back() != '\n' ? 1 : 0);
}

// The diff's own tail past its 200 lines ("… N more lines\n"): N, and the diff without it.
auto cut_tail(std::string_view& diff) -> std::size_t {
    constexpr std::string_view mark = "\xE2\x80\xA6 ";
    constexpr std::string_view words = " more lines\n";
    if (!diff.ends_with(words)) {
        return 0;
    }
    const auto start = diff.rfind('\n', diff.size() - words.size()) + 1; // npos + 1 == 0
    const auto line = diff.substr(start);
    if (!line.starts_with(mark) || line.size() <= mark.size() + words.size()) {
        return 0;
    }
    std::size_t n = 0;
    for (const char c : line.substr(mark.size(), line.size() - mark.size() - words.size())) {
        if (c < '0' || c > '9') {
            return 0;
        }
        n = n * 10 + static_cast<std::size_t>(c - '0');
    }
    diff = diff.substr(0, start);
    return n;
}

inline constexpr float kDiffLine = 18.0f;

// A card's texts in full (a path cut at the card's edge hid what is deleted,
// a command cut at it hid what would run): they wrap, the card grows.
auto place_style(ui::Ui& ui) -> work::TextBlockStyle { // its lines as a label's were
    return {.face = Face::Text, .size = 12.5f, .color = ui::th().text_3, .line = 1.0f, .min_line_h = ui.line_height(Face::Text, 12.5f)};
}
auto fact_style() -> work::TextBlockStyle {
    return {.face = Face::Text, .size = 12.5f, .color = ui::th().text_2, .min_line_h = 20.0f};
}
auto command_style() -> work::TextBlockStyle {
    return {.face = Face::Mono, .size = 12.0f, .color = ui::th().text_2, .min_line_h = 20.0f};
}
auto diff_style() -> work::TextBlockStyle {
    return {.face = Face::Mono, .size = 12.0f, .color = ui::th().text_2, .min_line_h = 18.0f};
}

auto fact_style(const events::Permission& p) -> work::TextBlockStyle {
    return p.risk == "execute" ? command_style() : fact_style();
}

auto title_style(ui::Ui& ui, float size) -> work::TextBlockStyle {
    return {.face = Face::Display, .size = size, .color = ui::th().text, .line = 1.0f, .min_line_h = ui.line_height(Face::Display, size)};
}

// The place under the title: one line at least (the title's head keeps its height).
auto place_height(ui::Ui& ui, std::string_view place, float width) -> float {
    return std::max(work::text_block_height(ui, place, place_style(ui), width), ui.line_height(Face::Text, 12.5f));
}

// The facts (a command, a process, the clipboard), all of them, one under the other.
auto facts_height(ui::Ui& ui, const events::Permission& p, float width) -> float {
    float h = 4;
    for (const auto& fact : p.what) {
        h += work::text_block_height(ui, fact, fact_style(p), width);
    }
    return h;
}

auto kind_of(events::Kind k) -> work::Kind {
    switch (k) {
    case events::Kind::Read: return work::Kind::Read;
    case events::Kind::Change: return work::Kind::Change;
    case events::Kind::Delete: return work::Kind::Delete;
    case events::Kind::System: return work::Kind::System;
    case events::Kind::Ok: return work::Kind::Ok;
    }
    return work::Kind::Change;
}

auto icon_of(events::Kind k) -> char32_t {
    switch (k) {
    case events::Kind::Read: return work::kIconSearch;
    case events::Kind::Change: return work::kIconMove;
    case events::Kind::Delete: return work::kIconTrash;
    case events::Kind::System:
    case events::Kind::Ok: break;
    }
    return work::kIconSystem;
}

namespace am = ui::motion::approval;

auto lerp(Color a, Color b, float t) -> Color {
    const auto mix = [t](std::uint8_t from, std::uint8_t to) {
        return static_cast<std::uint8_t>(std::lround(static_cast<float>(from) + (static_cast<float>(to) - static_cast<float>(from)) * t));
    };
    return {mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b), mix(a.a, b.a)};
}

constexpr float kSide = 320.0f;     // .cols 1fr 320px
constexpr std::size_t kFirstRows = 3; // the list shows three until "show all"
constexpr float kLevelsHeight = 330.0f; // why it asks: its title, four levels, the shield's note
constexpr float kAllowedLine = 18.0f;

// A system action's card (phase 13a, design/ui_system 06a): three rows — a caption column of 78,
// then the text in up to two lines; how to undo it after its sign, green when it can be, yellow
// when not.
constexpr float kCaption = 78;
constexpr float kRowLine = 18;
constexpr float kRowGap = 10;
constexpr float kUndoSign = 20;
constexpr char32_t kIconUndo = 0xE7A7;
constexpr char32_t kIconWarn = 0xE7BA;
constexpr std::array<std::pair<std::string_view, std::string_view>, 3> kCaptions{{{"WHAT", "HAPPENS"}, {"AFFECTS", ""}, {"HOW TO", "UNDO"}}};
constexpr std::array<std::string_view, 3> kRowTargets{"card:happens", "card:affects", "card:undo"};

struct Explained {
    std::array<std::string_view, 3> text;
    std::array<work::TextBlockStyle, 3> style{};
    std::array<float, 3> width{}; // each row's text
    std::array<float, 3> heights{};
    float total = 0;
};

// The three rows in full, each as tall as its text (a path in mono breaks after its separators).
auto explained(ui::Ui& ui, std::string_view happens, std::string_view affects, std::string_view undo, float width) -> Explained {
    Explained e;
    const float text_w = width - kCaption;
    e.text = {happens, affects, undo};
    e.width = {text_w, text_w, text_w - kUndoSign};
    e.style[0] = {.face = Face::Text, .size = 13.0f, .color = ui::th().text_2, .line = 1.0f, .min_line_h = kRowLine};
    e.style[1] = {.face = Face::Mono, .size = 12.0f, .color = ui::th().text_2, .line = 1.0f, .min_line_h = kRowLine};
    e.style[2] = e.style[0];
    for (std::size_t i = 0; i < 3; ++i) {
        e.heights[i] = std::max(28.0f, work::text_block_height(ui, e.text[i], e.style[i], e.width[i])) + kRowGap;
        e.total += e.heights[i];
    }
    return e;
}

// Under the note: the commands' starts this task allowed (phase 12 § 7), with their title.
auto allowed_height(const events::Permission& p) -> float {
    return p.task_allowed.empty() ? 0.0f : 16 + 22 + kAllowedLine * static_cast<float>(p.task_allowed.size()) + 8;
}

} // namespace

auto Painter::draw_question(const Model& m, float right, float y, float width) -> float {
    const float scale = ui_->scale();
    const float ts = ui_->text_scale();
    constexpr float kPadX = 14.0f;
    constexpr float kPadY = 9.0f;
    if (!question_ || question_text_ != m.question || question_scale_ != scale * ts) {
        question_.emplace(fonts_->text, wt::type::bubble * ts * scale);
        question_->set_text(m.question);
        question_text_ = m.question;
        question_scale_ = scale * ts;
    }
    question_->set_width((width * theme::bubble::max_width - 2 * kPadX - 2) * scale);
    float w = 0;
    for (const auto& l : question_->lines()) {
        w = std::max(w, l.width / scale);
    }
    const float line = ui_->line_height(Face::Text, wt::type::bubble);
    const float bw = w + 2 * kPadX + 2;
    const float bh = static_cast<float>(question_->lines().size()) * line + 2 * kPadY + 2;
    const Box bubble{right - bw, y, bw, bh};
    work::bubble(*ui_, bubble);
    const auto& sized = fonts_->text.sized(0, wt::type::bubble * ts * scale);
    const auto question_key = ui_->next_key(); // its lines: one text
    for (std::size_t i = 0; i < question_->lines().size(); ++i) {
        const float top = bubble.y + 1 + kPadY + static_cast<float>(i) * line;
        const float base = top + (line - (sized.ascent + sized.descent) / scale) / 2 + sized.ascent / scale;
        ui_->draw_line(*question_, question_->lines()[i], {bubble.x + 1 + kPadX, base}, ui::th().text, {.key = question_key});
    }
    return bh;
}

auto Painter::card_text(std::string_view prefix, std::size_t fact, const Box& at, std::string_view s, const work::TextBlockStyle& style)
    -> float {
    const float h = work::text_block(*ui_, at, s, style);
    const auto& lines = ui_->paragraph(style.face, s, style.size, at.w).lines();
    const float step = lines.empty() ? h : h / static_cast<float>(lines.size());
    std::array<char, 96> buf{};
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::size_t to = i + 1 < lines.size() ? lines[i + 1].first_byte : s.size();
        add_target(text(buf, "{}{}:{}:{}", prefix, fact, lines[i].first_byte, to), {at.x, at.y + static_cast<float>(i) * step, at.w, step});
    }
    return h;
}

auto Painter::card_frame(std::uint64_t id, float x, float y, float w, float natural, float foot, float room) -> CardFrame {
    if (card_scrolled_ != id) {
        card_scrolled_ = id;
        card_scroll_ = {};
    }
    CardFrame f;
    f.scrolled = natural > room && room > foot + 80; // too short a room: the conversation scrolls it whole
    const float h = f.scrolled ? room : natural;
    f.shown = {x, y, w, h};
    f.content_y = y;
    f.foot_y = y + h - foot;
    f.view = {x, y, w, h - foot};
    return f;
}

void Painter::open_card(CardFrame& f, float natural, float foot) {
    if (f.scrolled) {
        f.content_y = work::scroll_view(*ui_, card_scroll_, f.view, natural - foot).y;
    }
}

void Painter::close_card(const CardFrame& f, ui::render::Corners corners) {
    if (f.scrolled) {
        work::end_scroll(*ui_, card_scroll_, f.view, work::surface(*ui_), corners);
    }
}

void Painter::draw_levels(Model& m, const events::Permission& p, const Box& column, Reaction& r) {
    float y = column.y + 2;
    work::caption(*ui_, {column.x + 2, y, column.w, 16}, Face::Semibold, 12.0f, "WHY I'M ASKING", ui::th().text_3);
    y += 16 + 10;
    struct Level {
        std::string_view key;
        events::Kind kind;
        char32_t icon;
        std::string_view title;
        std::string_view sub;
    };
    static constexpr std::array<Level, 4> kLevels{{
        {"read", events::Kind::Read, work::kIconSearch, "Reading", "files, processes, the network"},
        {"change", events::Kind::Change, work::kIconMove, "Creating, moving", "folders, renames"},
        {"delete", events::Kind::Delete, work::kIconTrash, "Deleting", "files and folders"},
        {"system", events::Kind::System, work::kIconSystem, "The system", "processes, services, the registry"},
    }};
    static constexpr std::array<std::string_view, 2> kRead{"No questions", "Ask"};
    static constexpr std::array<std::string_view, 2> kChange{"Once per folder", "Always"};
    static constexpr std::array<std::string_view, 1> kAlways{"Always"};
    using Items = std::span<const std::string_view>;
    const auto items_of = [](std::string_view key) -> Items {
        return key == "read" ? Items{kRead} : key == "change" ? Items{kChange} : Items{kAlways};
    };
    // .lvl: padding 11 14, the 30 px icon, the title over its line or two, the list on the right.
    const float th = ui_->line_height(Face::Semibold, 13.0f);
    const float sh = ui_->line_height(Face::Text, 11.5f);
    std::array<Box, 4> boxes{};    // the lists
    std::array<float, 4> heights{}; // the rows
    std::array<std::pair<std::string_view, std::string_view>, 4> subs{};
    float total = 0;
    for (std::size_t i = 0; i < kLevels.size(); ++i) {
        const auto dd = work::dropdown_size(*ui_, items_of(kLevels[i].key));
        boxes[i] = {column.x + column.w - 14 - dd.w, 0, dd.w, dd.h};
        const float tw = std::max(0.0f, boxes[i].x - 12 - (column.x + 14 + 30 + 12));
        subs[i] = two_lines(*ui_, Face::Text, 11.5f, kLevels[i].sub, tw);
        heights[i] = 2 * 11 + std::max(30.0f, th + sh * (subs[i].second.empty() ? 1.0f : 2.0f));
        total += heights[i];
    }
    const Box card{column.x, y, column.w, total};
    work::card(*ui_, card);
    float ry = card.y;
    for (std::size_t i = 0; i < kLevels.size(); ++i) {
        const auto& l = kLevels[i];
        const Box line{card.x, ry, card.w, heights[i]};
        ry += heights[i];
        if (i > 0) {
            ui_->renderer().rect({line.x, line.y, line.w, 1}, {.fill = ui::th().line});
        }
        if (l.kind == p.kind) { // .lvl.hit: the level this question comes from
            ui_->renderer().rect({line.x + 1, line.y + 1, line.w - 2, line.h - 2},
                                 {.fill = work::kind_ground(kind_of(l.kind)), .radius = {11, 11, 11, 11}});
        }
        work::kind_icon(*ui_, {line.x + 14, line.y + (line.h - 30) / 2, 30, 30}, kind_of(l.kind), l.icon);
        boxes[i].y = line.y + (line.h - boxes[i].h) / 2;
        const float tx = line.x + 14 + 30 + 12;
        const float tw = std::max(0.0f, boxes[i].x - 12 - tx);
        const float lines = subs[i].second.empty() ? 1.0f : 2.0f;
        const float top = line.y + (line.h - th - sh * lines) / 2;
        work::caption(*ui_, {tx, top, tw, th}, Face::Semibold, 13.0f, l.title, ui::th().text);
        work::caption(*ui_, {tx, top + th, tw, sh}, Face::Text, 11.5f, subs[i].first, ui::th().text_3);
        if (!subs[i].second.empty()) {
            work::caption(*ui_, {tx, top + th + sh, tw, sh}, Face::Text, 11.5f, subs[i].second, ui::th().text_3);
        }
    }
    // The lists after the rows, the last first: an open one lies over the rows below it.
    std::array<char, 64> buf{};
    for (std::size_t n = kLevels.size(); n-- > 0;) {
        const auto& l = kLevels[n];
        const Box& box = boxes[n];
        add_target(text(buf, "level:{}", l.key), box);
        // Deletes and the system are always asked: their one item is shown, never changed.
        const bool read = l.key == "read";
        const bool change = l.key == "change";
        const Items items = items_of(l.key);
        const int selected = read ? (m.levels.read_asks ? 1 : 0) : change ? (m.levels.change_per_folder ? 0 : 1) : 0;
        const auto picked = work::dropdown(*ui_, id_of("level", n), box, items, selected, level_open_[n], read || change);
        if (level_open_[n]) {
            for (std::size_t i = 0; i < items.size(); ++i) {
                add_target(text(buf, "level:{}:{}", l.key, i),
                           {box.x, box.y + box.h + 4 + box.h * static_cast<float>(i), box.w, box.h});
            }
        }
        if (picked) {
            events::Levels levels = m.levels;
            if (read) {
                levels.read_asks = *picked == 1;
            } else {
                levels.change_per_folder = *picked == 0;
            }
            m.levels = levels;
            r.levels = levels;
        }
    }
    // The shield's note, in its line or two.
    y = card.y + card.h + 12;
    const float nh = ui_->line_height(Face::Text, 12.5f);
    const auto note = two_lines(*ui_, Face::Text, 12.5f, "Windows and Program Files are always protected, even with permission",
                                column.w - 28 - 4);
    ui_->draw_icon(work::kIconShield, 13.0f, {column.x + 12, y + nh / 2}, ui::th().ok);
    work::caption(*ui_, {column.x + 28, y, column.w - 28, nh}, Face::Text, 12.5f, note.first, ui::th().text_3);
    work::caption(*ui_, {column.x + 28, y + nh, column.w - 28, nh}, Face::Text, 12.5f, note.second, ui::th().text_3);
    // The commands' starts this task allowed (phase 12 § 7): run without a question until it ends.
    if (!p.task_allowed.empty()) {
        y += 2 * nh + 16;
        const float lh = kAllowedLine;
        const Box list{column.x, y, column.w, 16 + 6 + lh * static_cast<float>(p.task_allowed.size())};
        work::caption(*ui_, {column.x + 2, y, column.w, 16}, Face::Semibold, 12.0f, "ALLOWED FOR THIS TASK", ui::th().text_3);
        for (std::size_t i = 0; i < p.task_allowed.size(); ++i) {
            work::caption(*ui_, {column.x + 2, y + 22 + lh * static_cast<float>(i), column.w - 4, lh}, Face::Mono, 12.0f,
                        p.task_allowed[i], ui::th().text_2);
        }
        add_target("why:allowed", list);
    }
}

void Painter::draw_permission(Model& m, const Box& panel, Reaction& r) {
    if (!m.permission) {
        return; // the card fading out after its answer
    }
    // The row: the field takes a new request, which replaces the asking task,
    // or the word ALLOW where it is needed.
    const auto& asked = *m.permission;
    const bool word = asked.advanced || (m.permanent && asked.offers_permanent);
    std::array<RowChip, 2> chips{{{asked.advanced ? "Needs permission \xC2\xB7 advanced mode" : "Needs permission",
                                   work::Tone::Warn, "chip"},
                                  {"Unsafe mode", work::Tone::Warn, "unsafe"}}};
    draw_row(m, panel, r, word ? "Type ALLOW to allow it, or say what to change\xE2\x80\xA6" : "Or say what to change\xE2\x80\xA6",
             wt::type::placeholder_small, std::span{chips.data(), m.unsafe ? 2u : 1u}, RowButton::Send, false);
    if (!m.permission) {
        return; // a new request took its place
    }
    const auto& p = *m.permission;
    const Box body{panel.x + wt::body::pad_x, panel.y + row_h_ + wt::body::pad_top, panel.w - 2 * wt::body::pad_x,
                   std::max(0.0f, panel.h - row_h_ - wt::body::pad_top - 16)};
    if (body.w <= 0 || body.h <= 0) {
        return;
    }
    const bool side = body.w > kSide + 360;
    const Box left{body.x, body.y, side ? body.w - kSide - wt::body::column_gap : body.w, body.h};
    auto& renderer = ui_->renderer();
    renderer.push_clip({left.x - 4, left.y, left.w + 8, left.h});
    float y = left.y;
    y += draw_question(m, left.x + left.w, y, left.w) + theme::bubble::margin_bottom;
    // The agent's words before the question, at most a few lines.
    if (doc_) {
        doc_->set_width(left.w);
        const float line = theme::type::prose * ui_->text_scale() * theme::type::prose_line;
        const auto& blocks = doc_->blocks();
        for (std::size_t i = 0; i < blocks.size() && i < 2; ++i) {
            const float h = static_cast<float>(std::min<std::size_t>(blocks[i].paragraph.lines().size(), 3)) * line;
            widgets::stream_text(*ui_, {left.x, y, left.w, h}, blocks[i].paragraph, times_[i], false);
            y += h + 8;
        }
        y += 4;
    }

    // .perm: the card, tinted by its kind (red for a delete).
    const Color k = work::kind_color(kind_of(p.kind));
    const bool files = !p.items.empty();
    const std::size_t shown = files ? (m.all_shown ? p.items.size() : std::min(p.items.size(), kFirstRows)) : 0;
    // .perm-h: padding 14 16 12, the 16 px title over the 12.5 px place, both in full.
    const float tx = left.x + 16 + 30 + 12;
    const float tw = left.x + left.w - 16 - tx;
    const std::string_view title = p.title.empty() ? std::string_view{p.reason} : std::string_view{p.title};
    const auto title_look = title_style(*ui_, 16.0f);
    const float th = work::text_block_height(*ui_, title, title_look, tw);
    const std::string_view place = p.where.empty() ? std::string_view{p.zone} : p.where;
    const float head_h = th + place_height(*ui_, place, tw);
    const float head = 14 + std::max(30.0f, head_h) + 12;
    // .list: rows of 30 px, 4 apart; the links are a row of their own.
    const float list = files ? static_cast<float>(shown) * 34 + (p.items.size() > 1 ? 34.0f : 0.0f) : facts_height(*ui_, p, left.w - 32);
    const bool del = p.kind == events::Kind::Delete && p.tool != "fs.delete_permanent";
    const float oh = work::option_height(*ui_); // two lines at the user's text size
    const float options = del ? 10.0f + oh + 2 : 0.0f;
    const float warn = p.warning ? 22.0f : 0.0f;
    const float foot = 12 + work::button_size(*ui_, "Allow").h + 16;
    const float natural = head + list + options + warn + foot;
    // Taller than the column's room: all but the buttons scrolls in the card.
    auto frame = card_frame(p.id, left.x, y, left.w, natural, foot, left.y + left.h - y);
    renderer.rect({frame.shown.x, frame.shown.y, frame.shown.w, frame.shown.h},
                  {.fill = with_alpha(k, 0.055f), .radius = ui::card_corners(), .border = with_alpha(k, 0.28f), .border_width = 1});
    add_target("card", frame.shown);
    open_card(frame, natural, foot);
    const Box card{left.x, frame.content_y, left.w, natural};
    const float hc = card.y + 14 + std::max(30.0f, head_h) / 2; // the head's middle
    work::kind_icon(*ui_, {card.x + 16, hc - 15, 30, 30}, kind_of(p.kind), icon_of(p.kind));
    (void)work::text_block(*ui_, {tx, hc - head_h / 2, tw, th}, title, title_look);
    std::array<char, 64> buf{};
    // The place's lines, one under the other; each a target "card:where:<i>:<from>:<to>" naming its
    // part of the text, for the tests.
    (void)card_text("card:where:", 0, {tx, hc - head_h / 2 + th, tw, 0}, place, place_style(*ui_));
    float ly = card.y + head;
    if (files) {
        // One row per file: its tick, its badge, its name, its size. A click flips the tick.
        for (std::size_t i = 0; i < shown; ++i) {
            const Box row{card.x + 16, ly, card.w - 32, wt::file_row::height};
            const bool on = i < m.ticks.size() && m.ticks[i];
            const auto hit = ui_->interact(id_of("file", i), row);
            add_target(text(buf, "file:{}", i), row);
            if (hit.clicked) {
                tick(m, i);
            }
            renderer.rect({row.x + 2, row.y + 7, 16, 16},
                          on ? ui::render::RectStyle{.fill = ui::th().accent, .radius = {4, 4, 4, 4}}
                             : ui::render::RectStyle{.fill = {}, .radius = {4, 4, 4, 4}, .border = ui::th().text_4,
                                                     .border_width = 1.5f});
            if (on) {
                ui_->draw_icon(widgets::kIconCheck, 11.0f, {row.x + 10, row.y + 15}, ui::th().on_accent);
            }
            const float was = renderer.opacity();
            renderer.set_opacity(was * (on ? 1.0f : 0.45f)); // a file left out
            work::file_row(*ui_, {row.x + 26, row.y, row.w - 26, row.h}, p.items[i].ext,
                           p.items[i].name, p.items[i].meta);
            renderer.set_opacity(was);
            ly += 34;
        }
        if (p.items.size() > 1) {
            // "Show all N · untick all"
            float lx = card.x + 16 + 9;
            const float lh = 20;
            if (p.items.size() > kFirstRows && !m.all_shown) {
                const auto label = text(buf, "Show all {}", p.items.size());
                const float w = work::text_width(*ui_, Face::Text, 12.5f, label);
                const Box link{lx, ly + 5, w, lh};
                if (ui_->interact(id_of("show-all"), link).clicked) {
                    m.all_shown = true;
                }
                add_target("show-all", link);
                work::caption(*ui_, link, Face::Text, 12.5f, label, ui::th().accent);
                lx += w;
                work::caption(*ui_, {lx, ly + 5, 20, lh}, Face::Text, 12.5f, " \xC2\xB7 ", ui::th().accent);
                lx += work::text_width(*ui_, Face::Text, 12.5f, " \xC2\xB7 ");
            }
            const bool any = std::ranges::any_of(m.ticks, [](bool t) { return t; });
            const std::string_view toggle = any ? "untick all" : "tick all";
            const float w = work::text_width(*ui_, Face::Text, 12.5f, toggle);
            const Box link{lx, ly + 5, w, lh};
            if (ui_->interact(id_of("tick-all"), link).clicked) {
                tick_all(m, !any);
            }
            add_target("tick-all", link);
            work::caption(*ui_, link, Face::Text, 12.5f, toggle, ui::th().accent);
            ly += 34;
        }
    } else {
        // A command, a process, the clipboard: the facts in full, one under the other (a command in mono).
        for (std::size_t i = 0; i < p.what.size(); ++i) {
            ly += card_text("card:what:", i, {card.x + 16, ly, card.w - 32, 0}, p.what[i], fact_style(p));
        }
        ly += 4;
    }
    if (del) {
        // To the Recycle Bin (the default), or — only where advanced mode has the tool — for good.
        const float ow = (card.w - 32 - 6) / 2;
        const Box trash{card.x + 16, ly + 10, ow, oh};
        if (work::option(*ui_, id_of("option:trash"), trash, "Move to Recycle Bin", "Can be restored for 30 days", !m.permanent)) {
            choose_permanent(m, false);
        }
        add_target("option:trash", trash);
        if (p.offers_permanent) {
            const Box forever{trash.x + ow + 6, trash.y, ow, oh};
            if (work::option(*ui_, id_of("option:permanent"), forever, "Delete permanently", "Cannot be undone", m.permanent)) {
                choose_permanent(m, true);
            }
            add_target("option:permanent", forever);
        }
        ly += options;
    }
    if (p.warning) {
        work::caption(*ui_, {card.x + 16, ly + 2, card.w - 32, 18}, Face::Semibold, 12.5f, "The agent runs as administrator",
                    ui::th().warn);
        ly += warn;
    }
    close_card(frame, {});
    // The footer: "don't ask again" where it is offered, Deny, Allow — under the scrolled part.
    const auto allow_size = work::button_size(*ui_, "Allow");
    const float by = frame.foot_y + 12;
    float bx = card.x + card.w - 16 - allow_size.w - 10; // .btn.pri padding 8 26: a little wider
    const bool can_allow = (m.ticks.empty() || std::ranges::any_of(m.ticks, [](bool t) { return t; })) &&
                           (!word || m.input.text == kAllowWord);
    const Box allow{bx, by, allow_size.w + 10, allow_size.h};
    add_target("action:allow", allow);
    if (work::button(*ui_, id_of("action:allow"), allow, "Allow", work::ButtonStyle::Primary, false, can_allow)) {
        r.command = answer(m, events::Choice::Allow);
    }
    const auto deny_size = work::button_size(*ui_, "Deny");
    bx -= deny_size.w + 8;
    const Box deny{bx, by, deny_size.w, deny_size.h};
    add_target("action:deny", deny);
    if (m.permission && work::button(*ui_, id_of("action:deny"), deny, "Deny")) {
        r.command = answer(m, events::Choice::Deny);
    }
    if (m.permission && p.folder) {
        const auto label = text(buf, "Don't ask again for \xE2\x80\x9C{}\xE2\x80\x9D", p.folder_name.empty() ? "this folder" : p.folder_name);
        const Box box{card.x + 16, by, std::max(0.0f, bx - 12 - (card.x + 16)), deny_size.h};
        if (work::checkbox(*ui_, id_of("folder"), box, label, m.folder_tick)) {
            tick_folder(m);
        }
        add_target("folder", box);
    }
    renderer.pop_clip();
    if (side && m.permission) {
        draw_levels(m, *m.permission, {left.x + left.w + wt::body::column_gap, body.y, kSide, body.h}, r);
    }
}

// Phase 8 (task 13d): the question in the conversation — the card with what it is, its options
// and its buttons — while its objects with their ticks and why it asks are in the panel.
auto Painter::approval_card(Model& m, const events::Permission& p, bool going, float x, float y0, float w, bool measure, Reaction& r)
    -> float {
    if (p.capture && !going) { // the screen (phase 10): its own card
        return screen_card(m, x, y0, w, measure, r);
    }
    auto& renderer = ui_->renderer();
    std::array<char, 64> buf{};
    // MS 11: coming, its room grows and its contents come a little later from below; answered, the
    // contents go, it shrinks into its summary row and takes the answer's colour.
    const auto scene = card_look(p, going);
    const bool bare = p.capture.has_value(); // the screen's card answered: its summary row only
    const bool files = !p.items.empty();
    const bool word = p.advanced || (m.permanent && p.offers_permanent);
    const Color k = work::kind_color(kind_of(p.kind));
    // The head: the title and the place in full, as many lines as they take.
    const float tx = x + 16 + 30 + 12;
    const float tw = x + w - 16 - tx;
    const std::string_view place = p.where.empty() ? std::string_view{p.zone} : p.where;
    const bool rows = !p.happens.empty() && !files; // a system action's three rows (06a)
    // Several versions (06b): the one picked gives the title, the rows and the button.
    const events::Permission::Pick* chosen = p.picks.empty() ? nullptr : &p.picks[std::min(m.pick, p.picks.size() - 1)];
    const std::string_view title_text = chosen != nullptr ? std::string_view{chosen->title} : std::string_view{p.title};
    const std::string_view title = title_text.empty() ? std::string_view{p.reason} : title_text;
    const auto title_look = title_style(*ui_, 15.5f);
    const float th = work::text_block_height(*ui_, title, title_look, tw);
    const float head_h = th + place_height(*ui_, place, tw);
    const float head = 14 + std::max(30.0f, head_h) + 12;
    const Explained e = rows ? explained(*ui_, chosen != nullptr ? std::string_view{chosen->happens} : std::string_view{p.happens},
                                         chosen != nullptr ? std::string_view{chosen->affects} : std::string_view{p.affects}, p.undo, w - 32)
                             : Explained{};
    constexpr float kPickH = 46;
    const float choices = p.picks.empty() ? 0.0f : static_cast<float>(p.picks.size()) * (kPickH + 6) + 6;
    const float facts = files ? 24.0f : rows ? e.total + choices : facts_height(*ui_, p, w - 32);
    const bool del = p.kind == events::Kind::Delete && p.tool != "fs.delete_permanent";
    const float oh = work::option_height(*ui_); // two lines at the user's text size
    const float options = del ? (p.offers_permanent ? 2 * oh + 6 : oh) + 10 : 0.0f;
    const float warn = p.warning ? 22.0f : 0.0f;
    const float tick = p.folder || !p.task_scope.empty() ? 30.0f : 0.0f;
    const std::string_view allow_label = chosen != nullptr      ? std::string_view{chosen->allow}
                                         : p.allow_label.empty() ? std::string_view{"Allow"}
                                                                 : std::string_view{p.allow_label};
    const std::string_view deny_label = p.deny_label.empty() ? std::string_view{"Deny"} : std::string_view{p.deny_label};
    const auto allow_size = work::button_size(*ui_, allow_label);
    const float foot = 12 + allow_size.h + 16;
    const float shot = p.picture ? static_cast<float>(p.picture->thumb_h) + 12 : 0.0f; // screen.capture: what a yes sends
    const auto [diff, shown, hidden] = card_diff(p.preview);
    const float diff_w = w - 32 - 16;
    float changes = 0;
    if (!diff.empty()) {
        std::string_view rest = diff;
        for (std::size_t i = 0; i < shown; ++i) {
            const auto end = rest.find('\n');
            changes += work::text_block_height(*ui_, rest.substr(0, end), diff_style(), diff_w);
            rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
        }
        changes += (hidden > 0 ? kDiffLine : 0.0f) + 16;
    }
    const float natural = bare ? am::kSummaryRow : head + facts + shot + changes + options + warn + tick + foot;
    // Taller than the conversation: all but the buttons scrolls in the card (the spec, 2026-10-29).
    const bool scrolls = natural > card_room_ && card_room_ > foot + 80;
    const float whole = scrolls ? card_room_ : natural;
    // As much of it as there is now: growing in, whole, or on its way to the summary row.
    const float h = (whole + (am::kSummaryRow - whole) * scene.shrink) * scene.room;
    const float gap = 12 * scene.room;
    if (measure) {
        return h + gap;
    }
    auto frame = card_frame(p.id, x, y0, w, natural, foot, card_room_);
    frame.shown.h = h;
    // Its buttons come into view, as the running step does, until the wheel takes over.
    if (const float under = frame.shown.y + frame.shown.h + 12 - (conv_view_.y + conv_view_.h);
        !going && conv_follow_ && under > 0.5f && conv_view_.h > 0) {
        work::scroll_to(conv_scroll_, conv_scroll_.offset + under);
        ui_->request_frame();
    }
    // Its colour: the question's kind, turning to the answer's — green allowed, quiet denied.
    const Color answer_tone = going && m.answered && m.answered->choice == events::Choice::Deny ? ui::th().text_3 : ui::th().ok;
    const Color tone = lerp(k, answer_tone, scene.tone);
    if (h > 1.0f) {
        const float corner = std::min(14.0f, h / 2);
        renderer.rect({frame.shown.x, frame.shown.y, frame.shown.w, frame.shown.h},
                      {.fill = with_alpha(tone, 0.055f), .radius = {corner, corner, corner, corner}, .border = with_alpha(tone, 0.32f),
                       .border_width = 1});
    }
    add_target("card", frame.shown);
    // The summary row it becomes: a sign, what was said, what it was about.
    if (going && scene.summary > 0.004f && h > 1.0f) {
        const bool denied = m.answered && m.answered->choice == events::Choice::Deny;
        const float was = renderer.opacity();
        renderer.push_clip({frame.shown.x, frame.shown.y, frame.shown.w, frame.shown.h});
        renderer.set_opacity(was * scene.summary);
        const float cy = frame.shown.y + frame.shown.h - am::kSummaryRow / 2;
        renderer.rect({x + 16, cy - 11, 22, 22}, {.fill = with_alpha(tone, 0.14f), .radius = {11, 11, 11, 11}});
        ui_->draw_icon(denied ? work::kIconClose : widgets::kIconCheck, 11.0f, {x + 27, cy}, tone);
        const std::string_view said = denied ? "Denied" : "Allowed";
        const float sw = work::caption(*ui_, {x + 48, cy - 11, w - 64, 22}, Face::Semibold, 14.0f, said, ui::th().text);
        work::caption(*ui_, {x + 48 + sw + 8, cy - 11, std::max(0.0f, w - 72 - sw), 22}, Face::Text, 13.0f, title, ui::th().text_3);
        add_target("card:summary", {x, cy - am::kSummaryRow / 2, w, am::kSummaryRow});
        renderer.set_opacity(was);
        renderer.pop_clip();
    }
    if (bare || scene.contents <= 0.004f || h <= 1.0f) {
        return h + gap; // nothing of its contents to draw
    }
    // Its contents in the room it has: coming up from kLift px under; answered, a picture going.
    const float contents_opacity = renderer.opacity();
    const bool contents_inert = ui_->inert();
    const auto targets_kept = target_count_;
    renderer.push_clip({frame.shown.x, frame.shown.y, frame.shown.w, frame.shown.h});
    renderer.set_opacity(contents_opacity * scene.contents);
    ui_->set_inert(contents_inert || going || scene.contents < 0.5f);
    const auto leave_contents = [&] {
        ui_->set_inert(contents_inert);
        renderer.set_opacity(contents_opacity);
        renderer.pop_clip();
        if (going) {
            target_count_ = targets_kept;
        }
    };
    frame.content_y += scene.lift;
    frame.foot_y += scene.lift;
    open_card(frame, natural, foot);
    const Box card{x, frame.content_y, w, natural};
    const float hc = card.y + 14 + std::max(30.0f, head_h) / 2;
    work::kind_icon(*ui_, {card.x + 16, hc - 15, 30, 30}, kind_of(p.kind), icon_of(p.kind));
    (void)work::text_block(*ui_, {tx, hc - head_h / 2, tw, th}, title, title_look);
    // The place's lines, one under the other; each a target "card:where:<i>:<from>:<to>" naming its
    // part of the text, for the tests.
    (void)card_text("card:where:", 0, {tx, hc - head_h / 2 + th, tw, 0}, place, place_style(*ui_));
    float ly = card.y + head;
    if (files) {
        const auto picked = static_cast<std::size_t>(std::ranges::count(m.ticks, true));
        work::caption(*ui_, {card.x + 16, ly, card.w - 32, 20}, Face::Text, 13.5f,
                    text(buf, "{} of {} ticked \xE2\x80\x94 the list is beside", picked, p.items.size()), ui::th().text_2);
    } else if (rows) {
        float ry = ly;
        // The versions: each a radio with its line and size; a click picks it.
        for (std::size_t i = 0; i < p.picks.size(); ++i) {
            const auto& pk = p.picks[i];
            const Box b{card.x + 16, ry, card.w - 32, kPickH};
            add_target(text(buf, "pick:{}", i), b);
            if (work::option(*ui_, id_of("pick", i), b, pk.name, pk.detail, i == std::min(m.pick, p.picks.size() - 1))) {
                pick_choice(m, i);
                redraw_due_ = true;
            }
            const float sw = work::text_width(*ui_, Face::Text, 12.0f, pk.size);
            work::caption(*ui_, {b.x + b.w - 12 - sw, b.y + 6, sw, 18}, Face::Text, 12.0f, pk.size, ui::th().text_3);
            ry += kPickH + 6;
        }
        if (!p.picks.empty()) {
            ry += 6;
        }
        for (std::size_t i = 0; i < 3; ++i) {
            const Box row{card.x + 16, ry, card.w - 32, e.heights[i] - kRowGap};
            add_target(kRowTargets[i], row);
            work::caption(*ui_, {row.x, row.y + 2, kCaption - 6, 13}, Face::Semibold, 10.5f, kCaptions[i].first, ui::th().text_3);
            if (!kCaptions[i].second.empty()) {
                work::caption(*ui_, {row.x, row.y + 15, kCaption - 6, 13}, Face::Semibold, 10.5f, kCaptions[i].second, ui::th().text_3);
            }
            float vx = row.x + kCaption;
            auto look = e.style[i];
            if (i == 2) {
                look.color = p.undo_ok ? ui::th().ok : ui::th().warn;
                ui_->draw_icon(p.undo_ok ? kIconUndo : kIconWarn, 12.0f, {vx + 6, row.y + kRowLine / 2}, look.color);
                if (!p.undo_ok) {
                    add_target("card:undo:warn", {vx, row.y, 12, kRowLine});
                }
                vx += kUndoSign;
            }
            (void)card_text(text(buf, "{}:", kRowTargets[i]), 0, {vx, row.y, e.width[i], 0}, e.text[i], look);
            ry += e.heights[i];
        }
    } else {
        // A command, a process, the clipboard: the facts in full, one under the other (a command in mono).
        float fy = ly;
        for (std::size_t i = 0; i < p.what.size(); ++i) {
            fy += card_text("card:what:", i, {card.x + 16, fy, card.w - 32, 0}, p.what[i], fact_style(p));
        }
    }
    ly += facts;
    if (p.picture) {
        const Box picture{card.x + 16, ly, static_cast<float>(p.picture->thumb_w), static_cast<float>(p.picture->thumb_h)};
        add_target("card:picture", picture);
        if (const auto* t = picture_texture(*p.picture)) {
            renderer.image({picture.x, picture.y, picture.w, picture.h}, {}, t->id(), {255, 255, 255, 255});
        }
        renderer.rect({picture.x, picture.y, picture.w, picture.h}, {.border = ui::th().line, .border_width = 1});
        ly += shot;
    }
    if (!diff.empty()) {
        // The diff, in mono: removed lines red, added green, the hunks' headers dim.
        const Box block{card.x + 16, ly + 4, card.w - 32, changes - 8};
        renderer.rect({block.x, block.y, block.w, block.h}, {.fill = ui::th().sunk, .radius = {8, 8, 8, 8}});
        add_target("card:diff", block);
        std::string_view rest = diff;
        float dy = block.y + 4;
        for (std::size_t i = 0; i < shown; ++i) {
            const auto end = rest.find('\n');
            const auto line = rest.substr(0, end);
            rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
            auto look = diff_style();
            look.color = line.starts_with("@@")  ? ui::th().text_3
                         : line.starts_with('-') ? ui::th().crit
                         : line.starts_with('+') ? ui::th().ok
                                                 : ui::th().text_2;
            dy += work::text_block(*ui_, {block.x + 8, dy, diff_w, 0}, line, look);
        }
        if (hidden > 0) {
            const Box more{block.x + 8, dy, block.w - 16, kDiffLine};
            work::caption(*ui_, more, Face::Text, 12.0f, text(buf, "\xE2\x80\xA6 {} more lines", hidden), ui::th().text_3);
            add_target("card:diff:more", more);
        }
        ly += changes;
    }
    if (del) {
        // To the Recycle Bin (the default), or — only where advanced mode has the tool — for good.
        const Box trash{card.x + 16, ly + 10, card.w - 32, oh};
        if (work::option(*ui_, id_of("option:trash"), trash, "Move to Recycle Bin", "Can be restored for 30 days", !m.permanent)) {
            choose_permanent(m, false);
        }
        add_target("option:trash", trash);
        if (p.offers_permanent) {
            const Box forever{trash.x, trash.y + oh + 6, trash.w, oh};
            if (work::option(*ui_, id_of("option:permanent"), forever, "Delete permanently", "Cannot be undone", m.permanent)) {
                choose_permanent(m, true);
            }
            add_target("option:permanent", forever);
        }
        ly += options;
    }
    if (p.warning) {
        work::caption(*ui_, {card.x + 16, ly + 2, card.w - 32, 18}, Face::Semibold, 12.5f, "The agent runs as administrator",
                    ui::th().warn);
        ly += warn;
    }
    if (p.folder) {
        const auto label = text(buf, "Don't ask again for \xE2\x80\x9C{}\xE2\x80\x9D", p.folder_name.empty() ? "this folder" : p.folder_name);
        const Box box{card.x + 16, ly + 6, card.w - 32, 22};
        if (work::checkbox(*ui_, id_of("folder"), box, label, m.folder_tick)) {
            tick_folder(m);
        }
        add_target("folder", box);
        ly += tick;
    } else if (!p.task_scope.empty()) { // a command's start (phase 12 § 7)
        std::array<char, 256> long_buf{};
        const auto label = text(long_buf, "Allow \xE2\x80\x9C{}\xE2\x80\x9D for this task", p.task_scope);
        const Box box{card.x + 16, ly + 6, card.w - 32, 22};
        if (work::checkbox(*ui_, id_of("task"), box, label, m.folder_tick)) {
            tick_folder(m);
        }
        add_target("task", box);
        ly += tick;
    }
    close_card(frame, {});
    // Deny on the left, Allow on the right: under the scrolled part, always seen.
    const float by = frame.foot_y + 12;
    const bool can_allow = (m.ticks.empty() || std::ranges::any_of(m.ticks, [](bool t) { return t; })) &&
                           (!word || m.input.text == kAllowWord);
    const auto deny_size = work::button_size(*ui_, deny_label);
    const Box deny{card.x + 16, by, deny_size.w, deny_size.h};
    add_target("action:deny", deny);
    if (work::button(*ui_, id_of("action:deny"), deny, deny_label) && !r.command) {
        r.command = answer(m, events::Choice::Deny); // (`p` is the model's no more: nothing of it is read below)
        leave_contents();
        return h + gap;
    }
    const Box allow{card.x + card.w - 16 - allow_size.w - 10, by, allow_size.w + 10, allow_size.h};
    add_target("action:allow", allow);
    if (work::button(*ui_, id_of("action:allow"), allow, allow_label, work::ButtonStyle::Primary, false, can_allow) && !r.command) {
        r.command = answer(m, events::Choice::Allow);
    }
    leave_contents();
    return h + gap;
}

void Painter::draw_approval_panel(Model& m, const events::Permission& p, bool going, const Box& panel, Reaction& r) {
    if (p.capture) { // the screen (phase 10): what will be sent, larger
        if (!going) {
            draw_capture_panel(m, panel, r);
        }
        return;
    }
    auto& renderer = ui_->renderer();
    std::array<char, 64> buf{};
    // Answered (MS 11): a picture of it — nothing to press, no targets — its ticked rows going.
    const bool was_inert = ui_->inert();
    const auto targets_kept = target_count_;
    ui_->set_inert(was_inert || going);
    const std::vector<bool>& ticks = going && m.answered ? m.answered->ticks : m.ticks;
    const bool rows_go = going && m.answered && m.answered->choice != events::Choice::Deny;
    std::array<std::pair<std::size_t, Box>, 16> going_rows{}; // the rows as they go, named for the tests below
    std::size_t going_count = 0;
    const double since = going ? approval_.elapsed() : 0.0;
    add_target("panel", panel);
    panel_ground(panel);
    renderer.rect({panel.x - 1, panel.y, 1, panel.h}, {.fill = ui::th().line});
    const float x = panel.x + 22;
    const float w = std::max(0.0f, panel.w - 44);
    // Its content scrolls: the objects (all of them after Show all), then why it asks.
    const std::size_t shown = m.all_shown ? p.items.size() : std::min<std::size_t>(p.items.size(), 12);
    const float content = 18 + (p.items.empty() ? 0.0f : 36 + 34 * static_cast<float>(shown) + (p.items.size() > 1 ? 34 : 0) + 38) +
                          kLevelsHeight + allowed_height(p);
    redraw_due_ = redraw_due_ || std::abs(content - panel_content_) > 0.5f;
    panel_content_ = content; // the window grows to show it whole, as far as it can
    if (approval_scrolled_ != p.id) {
        approval_scrolled_ = p.id;
        approval_scroll_ = {};
    }
    approval_scroll_.foot = work::corner_foot(bottom_corners(panel).br);
    const Box at = work::scroll_view(*ui_, approval_scroll_, panel, content);
    float y = at.y + 18;
    if (!p.items.empty()) {
        // The objects, each with its tick: a file left unticked is left out of the answer.
        const auto picked = static_cast<std::size_t>(std::ranges::count(ticks, true));
        const std::string_view title = p.kind == events::Kind::Delete ? "To delete" : "To change";
        const float tw = work::caption(*ui_, {x, y, w, 24}, Face::Display, 18.0f, title, ui::th().text);
        work::caption(*ui_, {x + tw + 10, y, 120, 24}, Face::Text, 13.0f, text(buf, "{}/{}", picked, p.items.size()), ui::th().text_3);
        y += 24 + 12;
        std::size_t turn = 0; // which of the ticked rows this is: its place in the stagger
        going_count = 0;
        const float list_opacity = renderer.opacity();
        for (std::size_t i = 0; i < shown; ++i) {
            const bool on = i < ticks.size() && ticks[i];
            // MS 11: allowed, the rows it was allowed for go one after another — quick, then their room on smooth.
            float row_alpha = 1.0f;
            float row_room = 1.0f;
            if (rows_go && on) {
                const auto seconds = [](std::chrono::milliseconds ms) { return std::chrono::duration<double>(ms).count(); };
                const double wait = seconds(am::kRowStagger) * static_cast<double>(std::min(turn, ui::motion::kStaggerMaxItems));
                ++turn;
                row_alpha = std::clamp(
                    anim_.to(id_of("approval:row-alpha", i), since >= seconds(am::kRowsOutAt) + wait ? 0.0f : 1.0f, ui::motion::kQuick), 0.0f, 1.0f);
                row_room = std::clamp(
                    anim_.to(id_of("approval:row-room", i), since >= seconds(am::kRowsCloseAt) + wait ? 0.0f : 1.0f, ui::motion::kSmooth), 0.0f, 1.0f);
            }
            const float row_h = 34 * row_room;
            if (row_h <= 0.5f) {
                continue;
            }
            const Box row{x, y, w, wt::file_row::height};
            if (going && going_count < going_rows.size()) {
                going_rows[going_count++] = {i, {row.x, row.y, row.w, row_h}};
            }
            renderer.push_clip({row.x, row.y, row.w, row_h});
            renderer.set_opacity(list_opacity * row_alpha);
            const auto hit = ui_->interact(id_of("file", i), row);
            add_target(text(buf, "file:{}", i), row);
            if (hit.clicked) {
                tick(m, i);
            }
            renderer.rect({row.x + 2, row.y + 7, 16, 16},
                          on ? ui::render::RectStyle{.fill = ui::th().accent, .radius = {4, 4, 4, 4}}
                             : ui::render::RectStyle{.fill = {}, .radius = {4, 4, 4, 4}, .border = ui::th().text_4,
                                                     .border_width = 1.5f});
            if (on) {
                ui_->draw_icon(widgets::kIconCheck, 11.0f, {row.x + 10, row.y + 15}, ui::th().on_accent);
            }
            const float was = renderer.opacity();
            renderer.set_opacity(was * (on ? 1.0f : 0.45f)); // a file left out
            work::file_row(*ui_, {row.x + 26, row.y, row.w - 26, row.h}, p.items[i].ext, p.items[i].name,
                           p.items[i].meta);
            renderer.set_opacity(list_opacity);
            renderer.pop_clip();
            y += row_h;
        }
        if (p.items.size() > 1) {
            float lx = x + 9;
            if (shown < p.items.size()) {
                const auto label = text(buf, "Show all {}", p.items.size());
                const float lw = work::text_width(*ui_, Face::Text, 12.5f, label);
                const Box link{lx, y + 5, lw, 20};
                if (ui_->interact(id_of("show-all"), link).clicked) {
                    m.all_shown = true;
                }
                add_target("show-all", link);
                work::caption(*ui_, link, Face::Text, 12.5f, label, ui::th().accent);
                lx += lw + work::text_width(*ui_, Face::Text, 12.5f, " \xC2\xB7 ");
            }
            const bool any = std::ranges::any_of(ticks, [](bool t) { return t; });
            const std::string_view toggle = any ? "untick all" : "tick all";
            const Box link{lx, y + 5, work::text_width(*ui_, Face::Text, 12.5f, toggle), 20};
            if (ui_->interact(id_of("tick-all"), link).clicked) {
                tick_all(m, !any);
            }
            add_target("tick-all", link);
            work::caption(*ui_, link, Face::Text, 12.5f, toggle, ui::th().accent);
            y += 34;
        }
        work::caption(*ui_, {x, y, w, 20}, Face::Text, 12.5f, text(buf, "{} of {} selected", picked, p.items.size()), ui::th().text_3);
        y += 20 + 18;
    }
    // Why it asks: the access levels, the one that asks marked, each changeable towards stricter.
    if (going || m.permission) { // (a click in the list above may have answered it)
        draw_levels(m, going ? p : *m.permission, {x, y, std::min(w, 420.0f), kLevelsHeight}, r);
    }
    work::end_scroll(*ui_, approval_scroll_, panel, panel_color(), bottom_corners(panel));
    ui_->set_inert(was_inert);
    if (going) {
        target_count_ = targets_kept;
        add_target("panel", panel); // still there, as a picture: "going:file:<i>" its rows
        for (std::size_t k = 0; k < going_count; ++k) {
            add_target(text(buf, "going:file:{}", going_rows[k].first), going_rows[k].second);
        }
    }
}

auto two_lines(ui::Ui& ui, Face face, float size, std::string_view s, float width) -> std::pair<std::string_view, std::string_view> {
    if (work::text_width(ui, face, size, s) <= width) {
        return {s, {}};
    }
    std::size_t cut = std::string_view::npos;
    for (auto i = s.find(' '); i != std::string_view::npos; i = s.find(' ', i + 1)) {
        if (work::text_width(ui, face, size, s.substr(0, i)) > width) {
            break;
        }
        cut = i;
    }
    if (cut == std::string_view::npos) {
        return {s, {}};
    }
    return {s.substr(0, cut), s.substr(cut + 1)};
}

auto card_diff(std::string_view preview) -> CardDiff {
    auto lines = diff_of(preview);
    const std::size_t beyond = cut_tail(lines);
    const std::size_t total = line_count(lines);
    const std::size_t shown = std::min(total, kCardDiffLines);
    return {.lines = lines, .shown = shown, .hidden = total - shown + beyond};
}

} // namespace app::sheet
