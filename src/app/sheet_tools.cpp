// What was done on this PC, in the panel (phase 10, design/ui_remastered screens 20-21): a
// picture's text — the picture with each line's box (orange where a word is doubtful), the lines
// numbered, a line under the mouse lighting its box and a box its line; Copy and .txt — and
// pictures converted — three promises, each picture before and after and what it saved, the
// folder they went to.

#include "app/sheet_view.hpp"

#include "core/hash.hpp"
#include "ui/theme.hpp"
#include "ui/work.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <string_view>

namespace app::sheet {
namespace {

namespace work = ui::work;
namespace wt = ui::theme::work;
using ui::Face;
using ui::layout::Box;
using Color = ui::render::Color;

constexpr float kPadX = 22;
constexpr float kTool = 30;
constexpr float kLine = 31;  // a line of the text's list
constexpr float kRow = 43;   // a converted picture's row
constexpr char32_t kIconCopy = 0xE8C8;
constexpr char32_t kIconSave = 0xE74E;
constexpr char32_t kIconFolder = 0xE838;
constexpr char32_t kIconCheck = 0xE73E;
constexpr char32_t kIconTerminal = 0xE756;

auto id_of(std::string_view name, std::uint64_t n = 0) -> ui::Id {
    return core::hash::fnv1a64(name, core::hash::fnv1a64("app/sheet")) * 1099511628211ull + n;
}

template <std::size_t N, class... Args>
auto text(std::array<char, N>& buf, std::format_string<Args...> f, Args&&... args) -> std::string_view {
    const auto r = std::format_to_n(buf.data(), buf.size(), f, std::forward<Args>(args)...);
    return {buf.data(), static_cast<std::size_t>(r.out - buf.data())};
}

auto inside(const Box& b, float x, float y) -> bool {
    return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}

// `picture` within `box`, its proportions kept, in its middle.
auto contained(const events::Picture& p, const Box& box) -> Box {
    const float pw = static_cast<float>(std::max(1, p.width > 0 ? p.width : p.thumb_w));
    const float ph = static_cast<float>(std::max(1, p.height > 0 ? p.height : p.thumb_h));
    const float k = std::min(box.w / pw, box.h / ph);
    const float w = pw * k;
    const float h = ph * k;
    return {box.x + (box.w - w) / 2, box.y + (box.h - h) / 2, w, h};
}

} // namespace

void Painter::draw_text_read(const events::DataSet& set, const Box& body, Reaction& r) {
    auto& renderer = ui_->renderer();
    const auto& read = *set.text_read;
    std::array<char, 48> name{};
    const float x = body.x + kPadX;
    const float w = std::max(0.0f, body.w - 2 * kPadX);
    float y = body.y + 18;
    const auto& in = ui_->input();

    // The lines, joined: what Copy and .txt give.
    ocr_text_.clear();
    for (const auto& l : read.lines) {
        ocr_text_ += l.text;
        ocr_text_ += '\n';
    }

    // The header: the file, where it was read; the profile's languages, Copy, .txt on the right.
    float right = x + w;
    {
        const float tw = work::text_width(*ui_, Face::Semibold, 12.0f, ".txt") + 10 + 14 + 12;
        const Box txt{right - tw, y - 2, tw, kTool};
        right = txt.x - 6;
        const auto hit = ui_->interact(id_of("ocr:txt"), txt, false);
        renderer.rect({txt.x, txt.y, txt.w, txt.h}, {.fill = blend(ui::th().w1, ui::th().w2, lit(id_of("ocr:txt"), hit.hovered)), .radius = {8, 8, 8, 8},
                                                       .border = ui::th().line, .border_width = 1});
        ui_->draw_icon(kIconSave, 13.0f, {txt.x + 10 + 7, txt.y + txt.h / 2}, ui::th().text_2);
        work::caption(*ui_, {txt.x + 10 + 14 + 6, txt.y, tw - 30, txt.h}, Face::Semibold, 12.0f, ".txt", ui::th().text_2);
        add_target("ocr:txt", txt);
        if (hit.clicked) {
            const auto dot = read.file.rfind('.');
            r.save = Reaction::SaveFile{(dot == std::string::npos ? read.file : read.file.substr(0, dot)) + ".txt", ocr_text_};
        }
        const Box copy{right - kTool, y - 2, kTool, kTool};
        right = copy.x - 10;
        add_target("ocr:copy", copy);
        if (work::icon_button(*ui_, id_of("ocr:copy"), copy, kIconCopy)) {
            r.clipboard = ocr_text_;
        }
        for (std::size_t i = read.languages.size(); i-- > 0;) {
            const auto& lang = read.languages[i];
            const float lw = work::text_width(*ui_, Face::Text, 11.5f, lang.name) + 18;
            const Box chip{right - lw, y + 2, lw, 22};
            right = chip.x - 6;
            const Color ink = lang.used ? ui::th().text : lang.installed ? ui::th().text_3 : ui::th().text_4;
            renderer.rect({chip.x, chip.y, chip.w, chip.h}, {.fill = lang.used ? ui::th().accent_soft : ui::th().w1, .radius = {11, 11, 11, 11},
                                                             .border = lang.used ? ui::th().accent_line : ui::th().line, .border_width = 1});
            work::caption(*ui_, {chip.x + 9, chip.y, lw - 9, chip.h}, Face::Text, 11.5f, lang.name, ink);
            add_target(text(name, "ocr:lang:{}", i), chip);
            if (ui_->interact(id_of("ocr:lang", i), chip, false).hovered) {
                ui_->tooltip(id_of("ocr:lang", i), chip, lang.used ? "Read in this language" : lang.installed ? "In your profile" : "Not installed",
                             lang.installed ? "Windows reads one language at a time: ask again to read in another."
                                            : "Add it in Settings \xC2\xB7 Time & language to read it.");
            }
        }
    }
    const float th = ui_->line_height(Face::Display, 18.0f);
    work::caption(*ui_, {x, y, std::max(0.0f, right - x - 8), th}, Face::Display, 18.0f, read.file, ui::th().text);
    y += th + 6;
    {
        const float sw = std::min(work::text_width(*ui_, Face::Mono, 12.0f, set.source) + 36, w);
        const Box chip{x, y, sw, 22};
        renderer.rect({chip.x, chip.y, chip.w, chip.h}, {.fill = ui::th().w1, .radius = {7, 7, 7, 7}, .border = ui::th().line, .border_width = 1});
        ui_->draw_icon(kIconTerminal, 13.0f, {chip.x + 15, chip.y + 11}, ui::th().text_3);
        work::caption(*ui_, {chip.x + 27, chip.y, chip.w - 36, chip.h}, Face::Mono, 12.0f, set.source, ui::th().text_2);
    }
    y += 22 + 16;

    // The picture on the left, the lines on the right.
    const float room = std::max(120.0f, body.y + body.h - y - 16);
    const float pw = std::floor(w * 0.46f);
    const Box card{x, y, pw, room};
    const Box list{x + pw + 14, y, w - pw - 14, room};
    const Box shot = contained(read.picture, {card.x + 12, card.y + 12, card.w - 24, card.h - 24});
    const auto box_of = [&](const events::TextLine& l) {
        return Box{shot.x + l.x * shot.w - 3, shot.y + l.y * shot.h - 3, l.w * shot.w + 6, l.h * shot.h + 6};
    };
    const std::size_t fits = static_cast<std::size_t>(std::max(0.0f, (list.h - 8) / kLine));
    const std::size_t shown = read.lines.size() <= fits ? read.lines.size() : (fits > 0 ? fits - 1 : 0); // the last row: "+N more"
    const auto row_of = [&](std::size_t i) { return Box{list.x + 1, list.y + 4 + static_cast<float>(i) * kLine, list.w - 2, kLine}; };

    // The line lit: by its box on the picture, or its row in the list.
    ocr_lit_ = -1;
    for (std::size_t i = 0; i < read.lines.size() && ocr_lit_ < 0; ++i) {
        if ((i < shown && inside(row_of(i), in.mouse_x, in.mouse_y)) || inside(box_of(read.lines[i]), in.mouse_x, in.mouse_y)) {
            ocr_lit_ = static_cast<int>(i);
        }
    }

    renderer.rect({card.x, card.y, card.w, card.h}, {.fill = ui::th().w1, .radius = ui::card_corners(), .border = ui::th().line, .border_width = 1});
    add_target("ocr:picture", card);
    if (const auto* tex = read.picture.thumb_rgba.empty() ? nullptr : picture_texture(read.picture)) {
        renderer.image({shot.x, shot.y, shot.w, shot.h}, {}, tex->id(), {255, 255, 255, 255});
    }
    for (std::size_t i = 0; i < read.lines.size(); ++i) {
        const auto& l = read.lines[i];
        const Box b = box_of(l);
        const bool sure = l.doubtful.empty(); // a doubtful word, a line with one: the warn
        const Color edge = sure ? ui::th().accent : ui::th().warn;
        const bool lit = ocr_lit_ == static_cast<int>(i);
        renderer.rect({b.x, b.y, b.w, b.h}, {.fill = sure ? (lit ? ui::th().accent_soft_hi : ui::th().accent_soft) : (lit ? ui::th().warn_soft_hi : ui::th().warn_soft), .radius = {3, 3, 3, 3},
                                             .border = lit ? ui::th().text : edge, .border_width = lit ? 2.0f : 1.5f});
        add_target(text(name, "ocr:box:{}", i), b);
    }

    renderer.rect({list.x, list.y, list.w, list.h}, {.fill = ui::th().w1, .radius = ui::card_corners(), .border = ui::th().line, .border_width = 1});
    renderer.push_clip({list.x, list.y, list.w, list.h});
    for (std::size_t i = 0; i < shown; ++i) {
        const auto& l = read.lines[i];
        const Box row = row_of(i);
        if (ocr_lit_ == static_cast<int>(i)) {
            renderer.rect({row.x, row.y, row.w, row.h}, {.fill = ui::th().w2});
        }
        if (i > 0) {
            renderer.rect({row.x + 12, row.y, row.w - 24, 1}, {.fill = ui::th().line});
        }
        work::caption(*ui_, {row.x + 12, row.y, 24, row.h}, Face::Text, 11.5f, text(name, "{}", i + 1), ui::th().text_3);
        const float tx = row.x + 40;
        work::caption(*ui_, {tx, row.y, std::max(0.0f, row.x + row.w - 12 - tx), row.h}, Face::Text, 13.5f, l.text,
                    ui::th().text);
        add_target(text(name, "ocr:line:{}", i), row);
        // A doubtful word: a wavy orange line under it.
        for (std::size_t k = 0; k < l.doubtful.size(); ++k) {
            const auto [from, size] = l.doubtful[k];
            if (from + size > l.text.size()) {
                continue;
            }
            const float wx = tx + work::text_width(*ui_, Face::Text, 13.5f, std::string_view{l.text}.substr(0, from));
            const float ww = work::text_width(*ui_, Face::Text, 13.5f, std::string_view{l.text}.substr(from, size));
            const float base = row.y + row.h / 2 + 8;
            constexpr std::array<float, 4> kWave{0, 1, 2, 1};
            for (float s = 0; s < ww; s += 1.5f) {
                renderer.rect({wx + s, base + kWave[static_cast<std::size_t>(s / 1.5f) % 4], 1.5f, 1.5f}, {.fill = ui::th().warn});
            }
            add_target(text(name, "ocr:doubt:{}:{}", i, k), {wx, base - 1, ww, 5});
        }
    }
    if (shown < read.lines.size()) {
        const Box more = row_of(shown);
        work::caption(*ui_, {more.x + 40, more.y, more.w - 52, more.h}, Face::Text, 12.5f,
                    text(name, "+{} more \xC2\xB7 Copy gives all", read.lines.size() - shown), ui::th().text_3);
        add_target("ocr:more", more);
    }
    renderer.pop_clip();
}

void Painter::draw_converted(const events::DataSet& set, const Box& body, Reaction& r) {
    auto& renderer = ui_->renderer();
    const auto& conv = *set.converted;
    std::array<char, 48> name{};
    const float x = body.x + kPadX;
    const float w = std::max(0.0f, body.w - 2 * kPadX);
    float y = body.y + 18;

    // The header: what was done; Open folder on the right.
    const float ow = work::text_width(*ui_, Face::Semibold, 12.0f, "Open folder") + 10 + 14 + 6 + 12;
    const Box open{x + w - ow, y - 2, ow, kTool};
    {
        const auto hit = ui_->interact(id_of("conv:open"), open, false);
        renderer.rect({open.x, open.y, open.w, open.h}, {.fill = blend(ui::th().w1, ui::th().w2, lit(id_of("conv:open"), hit.hovered)), .radius = {8, 8, 8, 8},
                                                         .border = ui::th().line, .border_width = 1});
        ui_->draw_icon(kIconFolder, 13.0f, {open.x + 10 + 7, open.y + open.h / 2}, ui::th().text_2);
        work::caption(*ui_, {open.x + 10 + 14 + 6, open.y, ow - 30, open.h}, Face::Semibold, 12.0f, "Open folder", ui::th().text_2);
        add_target("conv:open", open);
        if (hit.clicked) {
            r.open_folder = conv.folder;
        }
    }
    const float th = ui_->line_height(Face::Display, 18.0f);
    work::caption(*ui_, {x, y, std::max(0.0f, open.x - 12 - x), th}, Face::Display, 18.0f, conv.heading, ui::th().text);
    y += th + 6;
    {
        const float sw = std::min(work::text_width(*ui_, Face::Mono, 12.0f, conv.source) + 36, w);
        const Box chip{x, y, sw, 22};
        renderer.rect({chip.x, chip.y, chip.w, chip.h}, {.fill = ui::th().w1, .radius = {7, 7, 7, 7}, .border = ui::th().line, .border_width = 1});
        ui_->draw_icon(kIconTerminal, 13.0f, {chip.x + 15, chip.y + 11}, ui::th().text_3);
        work::caption(*ui_, {chip.x + 27, chip.y, chip.w - 36, chip.h}, Face::Mono, 12.0f, conv.source, ui::th().text_2);
    }
    y += 22 + 12;

    // Three promises, true of every conversion: new files, the originals as they were, no EXIF.
    {
        float px = x;
        constexpr std::array<std::string_view, 3> kPromises{"New files only", "Originals untouched", "EXIF & location removed"};
        for (std::size_t i = 0; i < kPromises.size(); ++i) {
            const float lw = work::text_width(*ui_, Face::Text, 11.5f, kPromises[i]);
            const Box pill{px, y, 10 + 12 + 6 + lw + 10, 22};
            renderer.rect({pill.x, pill.y, pill.w, pill.h},
                          {.fill = ui::th().ok_soft, .radius = {11, 11, 11, 11}, .border = ui::th().ok_line, .border_width = 1});
            ui_->draw_icon(kIconCheck, 11.0f, {pill.x + 10 + 6, pill.y + pill.h / 2}, ui::th().ok);
            work::caption(*ui_, {pill.x + 10 + 12 + 6, pill.y, lw + 2, pill.h}, Face::Text, 11.5f, kPromises[i], ui::th().text_2);
            add_target(text(name, "conv:promise:{}", i), pill);
            px += pill.w + 8;
        }
    }
    y += 22 + 14;

    // The table: before, after, saved.
    const float foot = 28;
    const float room = std::max(kRow + 30, body.y + body.h - y - foot - 12);
    const float head = 30;
    const std::size_t fits = static_cast<std::size_t>(std::max(0.0f, (room - head) / kRow));
    const std::size_t shown = conv.rows.size() <= fits ? conv.rows.size() : (fits > 0 ? fits - 1 : 0);
    const float table_h = head + static_cast<float>(shown + (shown < conv.rows.size() ? 1 : 0)) * kRow;
    const Box table{x, y, w, table_h};
    renderer.rect({table.x, table.y, table.w, table.h}, {.fill = ui::th().w1, .radius = ui::card_corners(), .border = ui::th().line, .border_width = 1});
    const float before_x = table.x + 14 + 34 + 12;
    const float saved_w = 64;
    const float after_x = table.x + (table.w - saved_w) / 2 + 20;
    work::caption(*ui_, {before_x, table.y, 120, head}, Face::Text, 12.0f, "Before", ui::th().text_3);
    work::caption(*ui_, {after_x, table.y, 120, head}, Face::Text, 12.0f, "After", ui::th().text_3);
    {
        const float sw = work::text_width(*ui_, Face::Text, 12.0f, "Saved");
        work::caption(*ui_, {table.x + table.w - 14 - sw, table.y, sw, head}, Face::Text, 12.0f, "Saved", ui::th().text_3);
    }
    const auto side = [&](float sx, float sy, float sw, std::string_view file, std::string_view format, std::string_view meta) {
        work::caption(*ui_, {sx, sy + 5, sw, 18}, Face::Text, 13.0f, file, ui::th().text);
        const float bw = work::text_width(*ui_, Face::Mono, 10.5f, format) + 12;
        renderer.rect({sx, sy + 24, bw, 15}, {.fill = ui::th().w2, .radius = {4, 4, 4, 4}});
        work::caption(*ui_, {sx + 6, sy + 24, bw - 6, 15}, Face::Mono, 10.5f, format, ui::th().text_2);
        work::caption(*ui_, {sx + bw + 7, sy + 22, std::max(0.0f, sw - bw - 7), 18}, Face::Text, 11.5f, meta, ui::th().text_3);
    };
    for (std::size_t i = 0; i < shown; ++i) {
        const auto& row = conv.rows[i];
        const Box b{table.x + 1, table.y + head + static_cast<float>(i) * kRow, table.w - 2, kRow};
        renderer.rect({b.x + 12, b.y, b.w - 24, 1}, {.fill = ui::th().line});
        if (const float light = lit(id_of("conv:row", i), ui_->interact(id_of("conv:row", i), b, false).hovered); light > 0.004f) {
            renderer.rect({b.x, b.y + 1, b.w, b.h - 1}, {.fill = faint(ui::th().w1, light)});
        }
        const Box thumb{table.x + 14, b.y + (kRow - 34) / 2, 34, 34};
        renderer.rect({thumb.x, thumb.y, thumb.w, thumb.h}, {.fill = ui::th().w2, .radius = {6, 6, 6, 6}});
        if (const auto* tex = row.thumb.thumb_rgba.empty() ? nullptr : picture_texture(row.thumb)) {
            renderer.image({thumb.x, thumb.y, thumb.w, thumb.h}, {}, tex->id(), {255, 255, 255, 255});
        }
        // Its edge: a white scan is not lost on a light theme's paper.
        renderer.rect({thumb.x, thumb.y, thumb.w, thumb.h}, {.fill = {}, .radius = {6, 6, 6, 6}, .border = ui::th().line_2, .border_width = 1});
        side(before_x, b.y, after_x - before_x - 12, row.from, row.from_format, row.before);
        side(after_x, b.y, table.x + table.w - 14 - saved_w - after_x, row.to, row.to_format, row.after);
        const float sw = work::text_width(*ui_, Face::Semibold, 12.5f, row.saved);
        const Box saved{table.x + table.w - 14 - sw, b.y, sw, b.h};
        work::caption(*ui_, saved, Face::Semibold, 12.5f, row.saved, row.saved.starts_with('+') ? ui::th().warn : ui::th().ok);
        add_target(text(name, "conv:saved:{}", i), saved);
        add_target(text(name, "conv:row:{}", i), b);
    }
    if (shown < conv.rows.size()) {
        const float my = table.y + head + static_cast<float>(shown) * kRow;
        renderer.rect({table.x + 13, my, table.w - 26, 1}, {.fill = ui::th().line});
        work::caption(*ui_, {before_x, my, table.w - 80, kRow}, Face::Text, 12.5f, text(name, "+{} more", conv.rows.size() - shown), ui::th().text_3);
        add_target("conv:more", {table.x, my, table.w, kRow});
    }
    y += table_h + 10;

    // The total and where they went: "…\Pictures\converted".
    const float tw = work::caption(*ui_, {x, y, w, 18}, Face::Text, 12.0f, conv.total, ui::th().text_2);
    add_target("conv:total", {x, y, tw, 18});
    std::string_view folder = conv.folder;
    if (const auto last = folder.rfind('\\'); last != std::string_view::npos && last > 0) {
        if (const auto before = folder.rfind('\\', last - 1); before != std::string_view::npos && before > 0) {
            folder = folder.substr(before);
        }
    }
    const float fx = x + tw + 14;
    const float fw = work::caption(*ui_, {fx + (folder.size() < conv.folder.size() ? 12 : 0), y, std::max(0.0f, x + w - fx - 12), 18}, Face::Text,
                                 12.0f, folder, ui::th().text_3);
    if (folder.size() < conv.folder.size()) {
        work::caption(*ui_, {fx, y, 12, 18}, Face::Text, 12.0f, "\xE2\x80\xA6", ui::th().text_3);
    }
    add_target("conv:folder", {fx, y, fw + 12, 18});
}

void Painter::draw_preview(Model& m, const events::DataSet& set, const Box& body, Reaction& r) {
    auto& renderer = ui_->renderer();
    const float x = body.x + kPadX;
    const float w = std::max(0.0f, body.w - 2 * kPadX);
    float y = body.y + 18;
    // Its file's buttons, as on its card: Open, Show in folder.
    const auto file = std::ranges::find(m.created, set.id, &events::Created::preview);
    float right = x + w;
    if (file != m.created.end()) {
        const auto fs = work::button_size(*ui_, "Show in folder", true);
        const Box folder{right - fs.w - 20, y - 2, fs.w + 20, fs.h};
        const auto fhit = ui_->interact(id_of("preview:folder"), folder, false);
        renderer.rect({folder.x, folder.y, folder.w, folder.h}, {.fill = blend(ui::th().w1, ui::th().w2, lit(id_of("preview:folder"), fhit.hovered)), .radius = {7, 7, 7, 7},
                                                                 .border = ui::th().line, .border_width = 1});
        ui_->draw_icon(kIconFolder, 12.0f, {folder.x + 10 + 6, folder.y + folder.h / 2}, ui::th().text_2);
        work::caption(*ui_, {folder.x + 28, folder.y, folder.w - 34, folder.h}, Face::Semibold, 12.0f, "Show in folder", ui::th().text_2);
        add_target("preview:folder", folder);
        if (fhit.clicked) {
            r.show_in_folder = file->path;
        }
        const auto os = work::button_size(*ui_, "Open", true);
        const Box open{folder.x - 8 - os.w, y - 2, os.w, os.h};
        add_target("preview:open", open);
        if (work::button(*ui_, id_of("preview:open"), open, "Open", work::ButtonStyle::Primary, true)) {
            r.open_created = file->path;
        }
        right = open.x - 12;
    }
    const float th = ui_->line_height(Face::Display, 18.0f);
    work::caption(*ui_, {x, y, std::max(0.0f, right - x), th}, Face::Display, 18.0f, "Preview", ui::th().text);
    y += th + 6;
    {
        const float sw = std::min(work::text_width(*ui_, Face::Mono, 12.0f, set.source) + 36, w);
        const Box chip{x, y, sw, 22};
        renderer.rect({chip.x, chip.y, chip.w, chip.h}, {.fill = ui::th().w1, .radius = {7, 7, 7, 7}, .border = ui::th().line, .border_width = 1});
        ui_->draw_icon(kIconTerminal, 13.0f, {chip.x + 15, chip.y + 11}, ui::th().text_3);
        work::caption(*ui_, {chip.x + 27, chip.y, chip.w - 36, chip.h}, Face::Mono, 12.0f, set.source, ui::th().text_2);
    }
    y += 22 + 14;

    // The page: white, the document on it as written; it scrolls inside its frame (the wheel), the
    // frame's edge over it and the foot fading into the frame while more is below.
    const Box frame{x, y, w, std::max(80.0f, body.y + body.h - y - 16)};
    constexpr float kFrameRadius = 12;
    renderer.rect({frame.x, frame.y, frame.w, frame.h}, {.fill = ui::th().panel, .radius = {kFrameRadius, kFrameRadius, kFrameRadius, kFrameRadius}});
    add_target("preview:frame", frame);
    if (!preview_doc_) {
        preview_doc_.emplace(*fonts_, ui::theme::type::prose * ui_->text_scale(), ui_->scale());
        preview_doc_->keep_tables_whole(); // the page is the document: its tables on it, whole
    }
    if (preview_scroll_set_ != set.id) { // another document: from its top
        preview_scroll_ = {};
        preview_scroll_set_ = set.id;
    }
    preview_doc_->sync(set.preview, true);
    const float pw = std::min(frame.w - 64, 560.0f);
    const float inner = std::max(40.0f, pw - 76);
    preview_doc_->set_width(inner, inner);
    const Box viewport{frame.x + 1, frame.y + 1, frame.w - 2, frame.h - 2};
    const float page_h = 34 + preview_doc_->height() + 48;
    const Box content = work::scroll_view(*ui_, preview_scroll_, viewport, 17 + page_h + 18);
    const Box page{frame.x + (frame.w - pw) / 2, content.y + 17, pw, page_h};
    // The page: the stronger line and a soft shadow (the designer's answer on the palette, 9) — on a
    // dark theme its white edge does not glare, on a light one it is not lost.
    const auto& soft = ui::th().pop_shadow[1]; // the near layer of a pop's shadow
    renderer.shadow({page.x, page.y + soft.y, page.w, page.h}, 3.0f, soft.blur, soft.color);
    renderer.rect({page.x, page.y, page.w, page.h}, {.fill = {255, 255, 255, 255}, .radius = {3, 3, 3, 3}});
    renderer.rect({page.x, page.y, page.w, page.h}, {.fill = {}, .radius = {3, 3, 3, 3}, .border = ui::th().line_2, .border_width = 1}); // over the paper
    add_target("preview:page", page);
    const Box text{page.x + 38, page.y + 34, inner, preview_doc_->height()};
    paper_ = true;
    draw_markdown(*preview_doc_, {}, text, viewport, false, r);
    paper_ = false;
    work::end_scroll(*ui_, preview_scroll_, viewport, ui::th().panel, {0, 0, kFrameRadius - 1, kFrameRadius - 1});
    renderer.rect({frame.x, frame.y, frame.w, frame.h},
                  {.fill = {}, .radius = {kFrameRadius, kFrameRadius, kFrameRadius, kFrameRadius}, .border = ui::th().line, .border_width = 1});
}

} // namespace app::sheet
