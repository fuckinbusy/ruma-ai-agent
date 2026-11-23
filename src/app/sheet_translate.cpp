// The translation cards (phase 8, docs/specs/phase-8-ui-remaster.md § 5.6; mockup-spec § 5): a
// word or a phrase as one card in the turn, 920 wide; a text in two columns — the turn says it is
// on the clipboard, the panel has its sentence pairs and its key terms. What changes a card (⇄,
// a style, a common word, Shorter) is the next request in the conversation; the transcriptions'
// switch asks nothing; Copy and With pinyin are the user's own (the clipboard, no question).

#include "app/sheet_view.hpp"

#include "app/present.hpp"

#include "core/hash.hpp"
#include "ui/theme.hpp"
#include "ui/work.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <iterator>

namespace app::sheet {
namespace {

namespace work = ui::work;
namespace wt = ui::theme::work;
using ui::Face;
using ui::layout::Box;
using ui::render::Color;

auto id_of(std::string_view name, std::size_t n = 0) -> ui::Id {
    return core::hash::fnv1a64(name, core::hash::fnv1a64("app/sheet/translate")) * 1099511628211ull + n;
}

template <class... Args>
auto text(std::array<char, 64>& buf, std::format_string<Args...> f, Args&&... args) -> std::string_view {
    const auto r = std::format_to_n(buf.data(), buf.size(), f, std::forward<Args>(args)...);
    return {buf.data(), static_cast<std::size_t>(r.out - buf.data())};
}

constexpr float kCardW = 920.0f;   // a word or a phrase (mockup-spec § 1)
constexpr float kHeader = 48.0f;
constexpr float kFooter = 52.0f;
constexpr char32_t kIconSwap = 0xE8AB;
constexpr char32_t kIconCopy = 0xE8C8;
constexpr char32_t kIconBulb = 0xEA80;

auto language(std::string_view code) -> std::string_view {
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 12> kNames{{{"zh", "Chinese"},
                                                                                           {"en", "English"},
                                                                                           {"de", "German"},
                                                                                           {"ru", "Russian"},
                                                                                           {"fr", "French"},
                                                                                           {"es", "Spanish"},
                                                                                           {"ja", "Japanese"},
                                                                                           {"ko", "Korean"},
                                                                                           {"it", "Italian"},
                                                                                           {"pt", "Portuguese"},
                                                                                           {"uk", "Ukrainian"},
                                                                                           {"pl", "Polish"}}};
    for (const auto& [c, name] : kNames) {
        if (c == code) {
            return name;
        }
    }
    return code;
}

auto ideographic(std::string_view code) -> bool {
    return code == "zh" || code == "ja" || code == "ko";
}

} // namespace

// The next request of the conversation, from a card's button.
void Painter::ask_again(Model& m, std::string_view request, Reaction& r) {
    if (r.command) {
        return;
    }
    m.input.text.assign(request);
    m.input.caret = m.input.text.size();
    m.input.anchor = Edit::kNoAnchor; // a new text: no selection of the old one
    r.command = submit(m);
}

auto Painter::card_header(Model& m, std::string_view source, std::string_view target, std::string_view pill, std::string_view swap_text,
                          float x, float y, float w, Reaction& r) -> float {
    auto& renderer = ui_->renderer();
    std::array<char, 64> buf{};
    renderer.rect({x + 1, y + kHeader - 1, w - 2, 1}, {.fill = ui::th().line});
    float cx = x + 16;
    const auto lang = [&](std::string_view code) {
        std::array<char, 8> up{};
        const std::size_t n = std::min(code.size(), up.size());
        std::ranges::transform(code.substr(0, n), up.begin(), [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c; });
        const std::string_view upper{up.data(), n};
        const float cw = work::text_width(*ui_, Face::Semibold, 10.5f, upper) + 12;
        renderer.rect({cx, y + (kHeader - 18) / 2, cw, 18}, {.fill = ui::th().w2, .radius = {5, 5, 5, 5}});
        work::caption(*ui_, {cx + 6, y + (kHeader - 18) / 2, cw - 6, 18}, Face::Semibold, 10.5f, upper, ui::th().text_3);
        cx += cw + 8;
        cx += work::caption(*ui_, {cx, y, 160, kHeader}, Face::Semibold, 13.5f, language(code), ui::th().text) + 10;
    };
    lang(source);
    const Box swap{cx, y + (kHeader - 30) / 2, 30, 30};
    add_target("tr:swap", swap);
    const auto hit = ui_->interact(id_of("swap"), swap, false);
    renderer.rect({swap.x, swap.y, swap.w, swap.h},
                  {.fill = blend(ui::th().w1, ui::th().w2, lit(id_of("swap"), hit.hovered)), .radius = {8, 8, 8, 8}});
    ui_->draw_icon(kIconSwap, 15.0f, {swap.x + 15, swap.y + 15}, ui::th().text_3);
    cx += 30 + 10;
    lang(target);
    if (!pill.empty()) {
        const float pw = work::text_width(*ui_, Face::Text, 12.0f, pill) + 20;
        const Box p{x + w - 16 - pw, y + (kHeader - 22) / 2, pw, 22};
        renderer.rect({p.x, p.y, p.w, p.h}, {.fill = ui::th().accent_soft, .radius = {11, 11, 11, 11}});
        work::caption(*ui_, {p.x + 10, p.y, pw - 10, p.h}, Face::Text, 12.0f, pill, ui::th().accent);
    }
    if (hit.clicked) {
        // ⇄: the same thing asked the other way round.
        tr_request_ = std::format("Translate \xE2\x80\x9C{}\xE2\x80\x9D from {} into {}", swap_text, language(target), language(source));
        ask_again(m, tr_request_, r);
    }
    (void)buf;
    return kHeader;
}

auto Painter::examples_block(Model& m, std::string_view title, std::span<const events::Example> examples, float x, float y, float w,
                             bool measure) -> float {
    if (examples.empty()) {
        return 0;
    }
    auto& renderer = ui_->renderer();
    std::array<char, 64> buf{};
    const bool readings = m.transcription;
    const float row = 9 + 26 + (readings ? 18.0f : 0.0f) + 3 + 19 + 9;
    const float h = 14 + 14 + 6 + static_cast<float>(examples.size()) * row;
    if (measure) {
        return h;
    }
    renderer.rect({x + 1, y, w - 2, 1}, {.fill = ui::th().line});
    work::caption(*ui_, {x + 22, y + 14, w - 44, 14}, Face::Semibold, 11.0f, title, ui::th().text_3);
    float ry = y + 14 + 14 + 6;
    for (std::size_t i = 0; i < examples.size(); ++i) {
        const auto& e = examples[i];
        const Box box{x + 22, ry, w - 44, row};
        add_target(text(buf, "tr:example:{}", i), box);
        if (i > 0) {
            renderer.rect({box.x, box.y, box.w, 1}, {.fill = ui::th().w1});
        }
        work::caption(*ui_, {box.x, ry + 9, 20, 26}, Face::Text, 12.0f, text(buf, "{}", i + 1), ui::th().text_4);
        const float tx = box.x + 32;
        const float tw = box.w - 32;
        // The word in its sentence, marked.
        if (const auto at = e.text.find(e.highlight); !e.highlight.empty() && at != std::string::npos) {
            const float before = work::text_width(*ui_, Face::Text, 18.0f, std::string_view{e.text}.substr(0, at));
            const float mark = work::text_width(*ui_, Face::Text, 18.0f, e.highlight);
            renderer.rect({tx + before - 3, ry + 9 + 2, mark + 6, 22}, {.fill = ui::th().accent_soft_hi, .radius = {4, 4, 4, 4}});
            renderer.rect({tx + before - 3, ry + 9 + 22, mark + 6, 2}, {.fill = ui::th().accent_line});
        }
        work::caption(*ui_, {tx, ry + 9, tw, 26}, Face::Text, 18.0f, e.text, ui::th().text);
        float ly = ry + 9 + 26;
        if (readings && !e.transcription.empty()) {
            work::caption(*ui_, {tx, ly, tw, 18}, Face::Text, 12.5f, e.transcription, ui::th().text_3);
        }
        ly += readings ? 18.0f : 0.0f;
        work::caption(*ui_, {tx, ly + 3, tw, 19}, Face::Text, 13.5f, e.translation, ui::th().text_2);
        ry += row;
    }
    return h;
}

auto Painter::card_footer(Model& m, std::string_view copy_label, std::string_view copy, std::string_view with, bool shorter, float x,
                          float y, float w, Reaction& r) -> float {
    auto& renderer = ui_->renderer();
    renderer.rect({x + 1, y, w - 2, kFooter - 1}, {.fill = ui::th().w1, .radius = {0, 0, 14, 14}});
    renderer.rect({x + 1, y, w - 2, 1}, {.fill = ui::th().line});
    float bx = x + 16;
    const auto button = [&](std::string_view target, std::string_view label) {
        const auto size = work::button_size(*ui_, label, true);
        const Box b{bx, y + (kFooter - size.h) / 2, size.w, size.h};
        add_target(target, b);
        bx += size.w + 8;
        return work::button(*ui_, id_of(target), b, label, work::ButtonStyle::Normal, true);
    };
    if (button("tr:copy", copy_label)) {
        r.clipboard.assign(copy);
    }
    if (!with.empty() && button("tr:with_transcription", "With pinyin")) {
        r.clipboard.assign(with);
    }
    if (shorter && button("tr:shorter", "Shorter")) {
        tr_request_ = std::format("A shorter way to say \xE2\x80\x9C{}\xE2\x80\x9D", copy);
        ask_again(m, tr_request_, r);
    }
    if (m.spent) {
        if (cost_usd_ != m.spent->usd || cost_.empty()) {
            cost_ = spent_text(*m.spent);
            cost_usd_ = m.spent->usd;
        }
        const float cw = work::text_width(*ui_, Face::Text, 12.0f, cost_);
        work::caption(*ui_, {x + w - 16 - cw, y, cw, kFooter}, Face::Text, 12.0f, cost_, ui::th().text_4);
    }
    return kFooter;
}

auto Painter::coming(std::uint64_t key) -> CardComing {
    // MS § 7 "Translation": as an answer opens — the room on smooth, the blocks on quick one
    // kBlockStagger after another, each up from kLift px on base. A card seen before is as it is.
    namespace tm = ui::motion::translate;
    if (key != card_key_) {
        card_key_ = key;
        card_came_at_ = reduced_motion_ ? -1.0e9 : now_;
    }
    const double age = now_ - card_came_at_;
    CardComing c;
    if (age > 5.0) {
        return c;
    }
    const ui::Id id = id_of("card:coming", key);
    const double stagger = std::chrono::duration<double>(tm::kBlockStagger).count();
    c.room = std::clamp(anim_.to(id, 1.0f, ui::motion::kSmooth, 0.0f), 0.0f, 1.0f);
    for (std::size_t i = 0; i < c.alpha.size(); ++i) {
        const bool due = age >= stagger * static_cast<double>(i) - 1e-9;
        c.alpha[i] = std::clamp(anim_.to(id + 1 + i, due ? 1.0f : 0.0f, ui::motion::kQuick, 0.0f), 0.0f, 1.0f);
        c.lift[i] = anim_.to(id + 16 + i, due ? 0.0f : tm::kLift, ui::motion::kBase, tm::kLift);
        if (!due) {
            redraw_due_ = true; // its turn comes on a frame
        }
    }
    return c;
}

auto Painter::draw_word_card(Model& m, float x, float y0, float w, bool measure, Reaction& r) -> float {
    const auto& word = *m.word;
    auto& renderer = ui_->renderer();
    std::array<char, 64> buf{};
    const CardComing in = coming(core::hash::fnv1a64(word.word, m.task) | 1u);
    if (!measure && !ui_->inert()) {
        card_coming_ = in;
    }
    const float card_opacity = renderer.opacity();
    // A block's turn: its opacity, and how far under its place it still is.
    const auto block = [&](std::size_t i) {
        renderer.set_opacity(card_opacity * in.alpha[i]);
        return in.lift[i];
    };
    const bool tile = ideographic(word.source);
    const bool readings = m.transcription && !word.transcription.empty();
    // The text column: the word itself when it has no tile (40), its reading, meaning, reference.
    const float hero = 20 + std::max(tile ? 74.0f * 1.15f + 22 : 0.0f, (tile ? 0.0f : 40.0f) + (readings ? 32.0f : 0.0f) + 3 + 28 + 8 + 18) + 20;
    const float common_rows = static_cast<float>((word.common.size() + 1) / 2);
    const float common = word.common.empty() ? 0.0f : 14 + 14 + 6 + common_rows * 48 + 6;
    const float examples = examples_block(m, "EXAMPLES", word.examples, x, 0, w, true);
    const float h = kHeader + hero + examples + common + kFooter;
    if (measure) {
        return (h + 12) * in.room;
    }
    const Box card{x, y0, w, h * in.room}; // as much of it as there is now
    add_target("tr:card", card);
    if (card.h <= 1.0f) {
        return (h + 12) * in.room;
    }
    const float corner = std::min(14.0f, card.h / 2);
    renderer.rect({card.x, card.y, card.w, card.h}, {.fill = ui::th().w1, .radius = {corner, corner, corner, corner}, .border = ui::th().line, .border_width = 1});
    renderer.push_clip({card.x, card.y, card.w, card.h});
    float y = y0 + card_header(m, word.source, word.target, word.level, word.meaning, x, y0 + block(0), w, r);
    // The hero: the word (a character in its tile), its reading, its meaning, its reference.
    const float hero_lift = block(1);
    y += hero_lift;
    float tx = x + 22;
    if (tile) {
        const float gw = work::text_width(*ui_, Face::Text, 74.0f, word.word) + 28;
        const Box g{x + 22, y + 20, gw, 74.0f * 1.15f + 22};
        add_target("tr:word", g);
        renderer.rect({g.x, g.y, g.w, g.h}, {.fill = ui::th().w1, .radius = {14, 14, 14, 14}, .border = ui::th().line, .border_width = 1});
        work::caption(*ui_, {g.x + 14, g.y + 10, gw - 14, 74.0f * 1.15f}, Face::Text, 74.0f, word.word, ui::th().text);
        tx = g.x + g.w + 22;
    }
    float ty = y + 20;
    const float tw = x + w - 22 - tx;
    if (!tile) {
        add_target("tr:word", {tx, ty, tw, 40});
        work::caption(*ui_, {tx, ty, tw, 40}, Face::Display, 34.0f, word.word, ui::th().text);
        ty += 40;
    }
    if (readings) {
        work::caption(*ui_, {tx, ty, tw, 32}, Face::Text, 25.0f, word.transcription, ui::th().accent);
        ty += 32;
    }
    work::caption(*ui_, {tx, ty + 3, tw, 28}, Face::Display, 21.0f, word.meaning, ui::th().text);
    ty += 3 + 28 + 8;
    add_target("tr:reference", {tx, ty, tw, 18});
    work::caption(*ui_, {tx, ty, tw, 18}, Face::Text, 13.0f, word.reference, ui::th().text_3);
    add_target("tr:hero", {x, y, w, hero});
    y += hero - hero_lift;
    y += examples_block(m, "EXAMPLES", word.examples, x, y + block(2), w, false);
    if (!word.common.empty()) {
        // Common words, two to a row: each is the next request.
        const float common_lift = block(3);
        y += common_lift;
        renderer.rect({x + 1, y, w - 2, 1}, {.fill = ui::th().line});
        work::caption(*ui_, {x + 22, y + 14, w - 44, 14}, Face::Semibold, 11.0f, "COMMON WORDS", ui::th().text_3);
        const float iw = (w - 44 - 8) / 2;
        for (std::size_t i = 0; i < word.common.size(); ++i) {
            const auto& c = word.common[i];
            const Box item{x + 22 + static_cast<float>(i % 2) * (iw + 8), y + 14 + 14 + 6 + static_cast<float>(i / 2) * 48, iw, 40};
            add_target(text(buf, "tr:common:{}", i), item);
            const auto hit = ui_->interact(id_of("common", i), item, false);
            renderer.rect({item.x, item.y, item.w, item.h},
                          {.fill = blend(ui::th().w1, ui::th().w2, lit(id_of("common", i), hit.hovered)), .radius = {10, 10, 10, 10},
                           .border = ui::th().line, .border_width = 1});
            const float ww = work::caption(*ui_, {item.x + 12, item.y, item.w / 2, item.h}, Face::Text, 17.0f, c.word, ui::th().text);
            add_target(text(buf, "tr:common:{}:word", i), {item.x + 12, item.y, ww, item.h});
            float after = item.x + 12 + ww + 10; // the meaning starts no sooner (drawn over the word)
            if (m.transcription && !c.transcription.empty()) {
                after += work::caption(*ui_, {after, item.y, item.w / 3, item.h}, Face::Text, 12.5f, c.transcription, ui::th().accent) + 10;
            }
            // Right-aligned in what is left, cut with an ellipsis: never out of its box.
            const float room = std::max(0.0f, item.x + item.w - 12 - after);
            const float mw = std::min(work::text_width(*ui_, Face::Text, 13.0f, c.meaning), room);
            add_target(text(buf, "tr:common:{}:meaning", i), {item.x + item.w - 12 - mw, item.y, mw, item.h});
            work::caption(*ui_, {item.x + item.w - 12 - mw, item.y, mw, item.h}, Face::Text, 13.0f, c.meaning, ui::th().text_2);
            if (hit.clicked) {
                tr_request_ = std::format("What does \xE2\x80\x9C{}\xE2\x80\x9D mean?", c.word);
                ask_again(m, tr_request_, r);
            }
        }
        y += common - common_lift;
    }
    tr_with_.clear();
    if (!word.transcription.empty() && word.source.starts_with("zh")) { // pinyin is Chinese's
        std::format_to(std::back_inserter(tr_with_), "{} ({})", word.word, word.transcription);
    }
    (void)card_footer(m, text(buf, "Copy {}", word.word), word.word, tr_with_, false, x, y + block(4), w, r);
    renderer.set_opacity(card_opacity);
    renderer.pop_clip();
    return (h + 12) * in.room;
}

auto Painter::draw_phrase_card(Model& m, float x, float y0, float w, bool measure, Reaction& r) -> float {
    const auto& p = *m.phrase;
    auto& renderer = ui_->renderer();
    std::array<char, 64> buf{};
    const CardComing in = coming(core::hash::fnv1a64(p.main, m.task) | 1u);
    if (!measure && !ui_->inert()) {
        card_coming_ = in;
    }
    const float card_opacity = renderer.opacity();
    const auto block = [&](std::size_t i) { // a block's turn: its opacity, and how far under its place it still is
        renderer.set_opacity(card_opacity * in.alpha[i]);
        return in.lift[i];
    };
    const float main_h = 20 + 36 + 7 + 20 + 16;
    const float tones = p.variants.empty() ? 0.0f : 14 + 14 + 6 + static_cast<float>(p.variants.size()) * 44 + 6;
    const float why = p.why.empty() ? 0.0f : 12 + 46 + 12;
    const float examples = examples_block(m, "HOW IT'S USED", p.examples, x, 0, w, true);
    const float h = kHeader + main_h + tones + why + examples + kFooter;
    if (measure) {
        return (h + 12) * in.room;
    }
    const Box card{x, y0, w, h * in.room}; // as much of it as there is now
    add_target("tr:card", card);
    if (card.h <= 1.0f) {
        return (h + 12) * in.room;
    }
    const float corner = std::min(14.0f, card.h / 2);
    renderer.rect({card.x, card.y, card.w, card.h}, {.fill = ui::th().w1, .radius = {corner, corner, corner, corner}, .border = ui::th().line, .border_width = 1});
    renderer.push_clip({card.x, card.y, card.w, card.h});
    const auto pill = p.variants.empty() ? std::string_view{} : text(buf, "{} variants", p.variants.size() + 1);
    float y = y0 + card_header(m, p.source, p.target, pill, p.main, x, y0 + block(0), w, r);
    const float main_lift = block(1);
    y += main_lift;
    add_target("tr:main", {x + 22, y + 20, w - 44, 36});
    work::caption(*ui_, {x + 22, y + 20, w - 44, 36}, Face::Display, 26.0f, p.main, ui::th().text);
    work::caption(*ui_, {x + 22, y + 20 + 36 + 7, w - 44, 20}, Face::Text, 13.5f, p.where, ui::th().text_3);
    y += main_h - main_lift;
    if (!p.variants.empty()) {
        const float tones_lift = block(2);
        y += tones_lift;
        renderer.rect({x + 1, y, w - 2, 1}, {.fill = ui::th().line});
        work::caption(*ui_, {x + 22, y + 14, w - 44, 14}, Face::Semibold, 11.0f, "OTHER TONE", ui::th().text_3);
        for (std::size_t i = 0; i < p.variants.size(); ++i) {
            const auto& v = p.variants[i];
            const Box row{x + 22, y + 14 + 14 + 6 + static_cast<float>(i) * 44, w - 44, 44};
            add_target(text(buf, "tr:variant:{}", i), row);
            const float pw = std::min(104.0f, work::text_width(*ui_, Face::Text, 12.0f, v.tone) + 20);
            renderer.rect({row.x, row.y + 11, pw, 22}, {.fill = ui::th().w2, .radius = {11, 11, 11, 11}});
            work::caption(*ui_, {row.x + 10, row.y + 11, pw - 10, 22}, Face::Text, 12.0f, v.tone, ui::th().text_2);
            work::caption(*ui_, {row.x + 116, row.y, row.w - 116 - 42, row.h}, Face::Text, 15.5f, v.text, ui::th().text);
            const Box copy{row.x + row.w - 30, row.y + 7, 30, 30};
            add_target(text(buf, "tr:variant:{}:copy", i), copy);
            if (work::icon_button(*ui_, id_of("variant", i), copy, kIconCopy)) {
                r.clipboard.assign(v.text);
            }
        }
        y += tones - tones_lift;
    }
    if (!p.why.empty()) {
        const Box box{x + 22, y + 12 + block(3), w - 44, 46};
        add_target("tr:why", box);
        renderer.rect({box.x, box.y, box.w, box.h}, {.fill = ui::th().accent_soft, .radius = ui::card_corners(),
                                                      .border = ui::th().accent_line, .border_width = 1});
        ui_->draw_icon(kIconBulb, 17.0f, {box.x + 14 + 8.5f, box.y + box.h / 2}, ui::th().accent);
        work::caption(*ui_, {box.x + 42, box.y, box.w - 56, box.h}, Face::Text, 13.5f, p.why, ui::th().text_2);
        y += why;
    }
    y += examples_block(m, "HOW IT'S USED", p.examples, x, y + block(4), w, false);
    (void)card_footer(m, "Copy", p.main, {}, true, x, y + block(5), w, r);
    renderer.set_opacity(card_opacity);
    renderer.pop_clip();
    return (h + 12) * in.room;
}

auto Painter::draw_text_turn(Model& m, float x, float y0, float w, bool measure, Reaction& r) -> float {
    const auto& t = *m.text_translation;
    auto& renderer = ui_->renderer();
    std::array<char, 64> buf{};
    const float h = (t.copied ? 30.0f : 0.0f) + 54 + 8;
    // It opens as the word's and the phrase's cards do (MS § 7): its room on smooth, itself on quick.
    const CardComing in = coming(core::hash::fnv1a64(t.source, m.task * 31 + t.pairs.size()) | 1u);
    if (measure) {
        return (h + 8) * in.room;
    }
    if (h * in.room <= 1.0f) {
        return (h + 8) * in.room;
    }
    const float card_opacity = renderer.opacity();
    renderer.push_clip({x, y0, w, h * in.room});
    renderer.set_opacity(card_opacity * in.alpha[0]);
    float y = y0 + in.lift[0];
    if (t.copied) {
        const std::string_view label = "Copied to clipboard";
        const float cw = work::text_width(*ui_, Face::Text, 12.5f, label) + 22 + 18;
        const Box chip{x, y, cw, 26};
        add_target("tr:copied", chip);
        renderer.rect({chip.x, chip.y, chip.w, chip.h}, {.fill = ui::th().ok_soft, .radius = {13, 13, 13, 13},
                                                         .border = ui::th().ok_line, .border_width = 1});
        ui_->draw_icon(0xE73E, 11.0f, {chip.x + 14, chip.y + 13}, ui::th().ok);
        work::caption(*ui_, {chip.x + 26, chip.y, cw - 26, chip.h}, Face::Text, 12.5f, label, ui::th().ok);
        y += 30;
    }
    // The card of the translation in the panel.
    const Box card{x, y, w, 54};
    add_target("tr:card", card);
    if (ui_->interact(id_of("text-card"), card, false).clicked) {
        tr_tab_ = 0;
    }
    renderer.rect({card.x, card.y, card.w, card.h}, {.fill = ui::th().accent_soft, .radius = ui::card_corners(),
                                                     .border = ui::th().accent_line, .border_width = 1});
    work::caption(*ui_, {card.x + 14, card.y + 9, card.w - 28, 18}, Face::Semibold, 13.5f,
                text(buf, "{} \xE2\x86\x92 {} translation", language(t.source), language(t.target)), ui::th().text);
    work::caption(*ui_, {card.x + 14, card.y + 28, card.w - 28, 17}, Face::Text, 12.5f,
                text(buf, "{} sentences \xC2\xB7 {}", t.pairs.size(), t.style), ui::th().text_3);
    renderer.set_opacity(card_opacity);
    renderer.pop_clip();
    (void)r;
    return (h + 8) * in.room;
}

void Painter::sync_pairs(const events::TextTranslation& t, float original_w, float translation_w) {
    const float scale = ui_->scale();
    const float ts = ui_->text_scale();
    std::uint64_t key = t.pairs.size() * 1099511628211ull + static_cast<std::uint64_t>(original_w * 10) * 31 +
                        static_cast<std::uint64_t>(translation_w * 10) + static_cast<std::uint64_t>(scale * ts * 100);
    for (const auto& p : t.pairs) {
        key = key * 131 + core::hash::fnv1a64(p.translation);
    }
    if (key == pairs_key_) {
        return;
    }
    pairs_key_ = key;
    pairs_.clear();
    for (const auto& p : t.pairs) {
        Pair made{ui::text::Paragraph{fonts_->text, 14.5f * ts * scale}, ui::text::Paragraph{fonts_->text, 19.0f * ts * scale}};
        made.original.set_text(p.original);
        made.original.set_width(original_w * scale);
        made.translation.set_text(p.translation);
        made.translation.set_width(translation_w * scale);
        pairs_.push_back(std::move(made));
    }
}

void Painter::draw_text_panel(Model& m, const Box& panel, Reaction& r) {
    const auto& t = *m.text_translation;
    auto& renderer = ui_->renderer();
    const float scale = ui_->scale();
    std::array<char, 64> buf{};
    add_target("panel", panel);
    panel_ground(panel);
    renderer.rect({panel.x - 1, panel.y, 1, panel.h}, {.fill = ui::th().line});
    // The tabs: Translation N · Terms N.
    std::array<work::Tab, 2> tabs{{{"Translation", 0xE8C1, static_cast<int>(t.pairs.size()), false},
                                   {"Terms", 0xE82D, static_cast<int>(t.terms.size()), false}}};
    int active = tr_tab_;
    const Box body = work::tabs(*ui_, id_of("tabs"), panel, tabs, active, -1);
    {
        float x = panel.x + 8;
        for (std::size_t i = 0; i < tabs.size(); ++i) {
            const auto count = text(buf, "{}", tabs[i].count);
            const float w = 13 + 15 + 8 + work::text_width(*ui_, Face::Text, 13.0f, tabs[i].label) + 13 + 8 + 14 +
                            work::text_width(*ui_, Face::Text, 11.5f, count);
            add_target(i == 0 ? "tab:translation" : "tab:terms", {x, panel.y, w, 46});
            x += w + 2;
        }
    }
    tr_tab_ = active;
    // The header: Source → Target, the clipboard; the style, the readings' switch, Copy.
    const float top = body.y + 18;
    work::caption(*ui_, {body.x + 22, top, 300, 24}, Face::Display, 18.0f,
                text(buf, "{} \xE2\x86\x92 {}", language(t.source), language(t.target)), ui::th().text);
    if (t.copied) {
        work::caption(*ui_, {body.x + 22, top + 24 + 7, 300, 22}, Face::Text, 12.5f,
                    text(buf, "Clipboard \xC2\xB7 {} chars", present::copied_size(t)), ui::th().text_3);
    }
    float tx = body.x + body.w - 22;
    const auto copy_size = work::button_size(*ui_, "Copy", true);
    const Box copy{tx - copy_size.w, top - 2, copy_size.w, copy_size.h};
    add_target("tr:copy", copy);
    if (work::button(*ui_, id_of("copy"), copy, "Copy", work::ButtonStyle::Normal, true)) {
        r.clipboard = present::copied_text(t);
    }
    tx = copy.x - 14;
    // The readings' switch, where the translation has readings (pinyin, IPA).
    if (std::ranges::any_of(t.pairs, [](const events::TextTranslation::Pair& p) { return !p.transcription.empty(); })) {
        const Box toggle{tx - 32, top + 2, 32, 18};
        add_target("tr:transcription", toggle);
        if (work::toggle(*ui_, id_of("readings"), toggle, m.transcription)) {
            toggle_transcription(m); // a switch: nothing asked of the agent
        }
        tx = toggle.x - 8;
        const std::string_view name = ideographic(t.target) ? "Pinyin" : "Reading";
        const float pw = work::text_width(*ui_, Face::Text, 12.5f, name);
        work::caption(*ui_, {tx - pw, top, pw, 22}, Face::Text, 12.5f, name, ui::th().text_2);
        tx -= pw + 16;
    }
    static constexpr std::array<std::string_view, 3> kStyles{"Business", "Neutral", "Casual"};
    static constexpr std::array<std::string_view, 3> kNames{"business", "neutral", "casual"};
    const auto seg = work::segmented_size(*ui_, kStyles);
    const Box styles{tx - seg.w, top - 2, seg.w, seg.h};
    int current = 1;
    for (std::size_t i = 0; i < kNames.size(); ++i) {
        current = kNames[i] == t.style ? static_cast<int>(i) : current;
    }
    {
        float sx = styles.x + 3;
        for (std::size_t i = 0; i < kStyles.size(); ++i) {
            const float iw = 2 * wt::segment::item_pad_x + work::text_width(*ui_, Face::Text, wt::type::segment, kStyles[i]);
            add_target(text(buf, "tr:style:{}", kNames[i]), {sx, styles.y, iw, styles.h});
            sx += iw + 2;
        }
    }
    if (const int picked = work::segmented(*ui_, id_of("style"), styles, kStyles, current); picked != current) {
        tr_request_ = std::format("Translate it again in a {} style", kNames[static_cast<std::size_t>(picked)]);
        ask_again(m, tr_request_, r);
    }
    float y = top + 24 + 7 + 22 + 14;
    const float x = body.x + 22;
    const float w = body.w - 44;
    if (tr_tab_ == 0) {
        // The pairs: number, the original, the translation and its reading.
        const float ow = (w - 32 - 22 - 18 * 2) / 2.25f;
        const float trw = ow * 1.25f;
        sync_pairs(t, ow, trw);
        renderer.rect({x, y, w, 30}, {.fill = ui::th().w1, .radius = {12, 12, 0, 0}});
        work::caption(*ui_, {x + 16, y, 22, 30}, Face::Text, 12.0f, "#", ui::th().text_3);
        work::caption(*ui_, {x + 16 + 22 + 18, y, ow, 30}, Face::Text, 12.0f, "Original", ui::th().text_3);
        work::caption(*ui_, {x + 16 + 22 + 18 + ow + 18, y, trw, 30}, Face::Text, 12.0f, "Translation", ui::th().text_3);
        y += 30;
        const float o_line = 14.5f * 1.5f;
        const float t_line = 19.0f * 1.45f;
        for (std::size_t i = 0; i < t.pairs.size() && i < pairs_.size(); ++i) {
            const auto& pr = pairs_[i];
            const bool reading = m.transcription && !t.pairs[i].transcription.empty();
            const float oh = static_cast<float>(pr.original.lines().size()) * o_line;
            const float th = static_cast<float>(pr.translation.lines().size()) * t_line + (reading ? 3 + 18.0f : 0.0f);
            const Box row{x, y, w, 13 + std::max(oh, th) + 13};
            if (row.y > body.y + body.h) {
                break;
            }
            add_target(text(buf, "tr:pair:{}", i), row);
            const bool hover = ui_->interact(id_of("pair", i), row, false).hovered;
            if (hover) {
                renderer.rect({row.x, row.y, row.w, row.h}, {.fill = ui::th().accent_soft});
            }
            renderer.rect({row.x, row.y, row.w, 1}, {.fill = ui::th().line});
            work::caption(*ui_, {x + 16, y + 13, 22, o_line}, Face::Text, 12.0f, text(buf, "{}", i + 1), ui::th().text_4);
            const auto lines = [&](const ui::text::Paragraph& p, float px, float lx, float ly, float line, Color c) {
                const auto& sized = fonts_->text.sized(0, px * scale);
                const auto key = ui_->next_key(); // its lines: one text
                for (std::size_t k = 0; k < p.lines().size(); ++k) {
                    const float base = ly + static_cast<float>(k) * line + (line - (sized.ascent + sized.descent) / scale) / 2 +
                                       sized.ascent / scale;
                    ui_->draw_line(p, p.lines()[k], {lx, base}, c, {.key = key});
                }
            };
            const float ox = x + 16 + 22 + 18;
            const float tx2 = ox + ow + 18;
            lines(pr.original, 14.5f * ui_->text_scale(), ox, y + 13, o_line, hover ? ui::th().text : ui::th().text_2);
            lines(pr.translation, 19.0f * ui_->text_scale(), tx2, y + 13, t_line, ui::th().text);
            if (reading) {
                const Box rb{tx2, y + 13 + static_cast<float>(pr.translation.lines().size()) * t_line + 3, trw, 18};
                add_target(text(buf, "tr:pair:{}:transcription", i), rb);
                work::caption(*ui_, rb, Face::Text, 12.5f, t.pairs[i].transcription, ui::th().text_3);
            }
            y += row.h;
        }
    } else {
        // The key terms, as chips that wrap.
        float cx = x;
        for (std::size_t i = 0; i < t.terms.size(); ++i) {
            const auto& term = t.terms[i];
            const float cw = 11 + work::text_width(*ui_, Face::Text, 16.0f, term.word) + 8 +
                             (m.transcription ? work::text_width(*ui_, Face::Text, 12.0f, term.transcription) + 8 : 0.0f) +
                             work::text_width(*ui_, Face::Text, 13.0f, term.meaning) + 11;
            if (cx > x && cx + cw > x + w) {
                cx = x;
                y += 44;
            }
            const Box chip{cx, y, std::min(cw, w), 36};
            add_target(text(buf, "tr:term:{}", i), chip);
            renderer.rect({chip.x, chip.y, chip.w, chip.h}, {.fill = ui::th().w1, .radius = {10, 10, 10, 10}, .border = ui::th().line, .border_width = 1});
            float lx = chip.x + 11;
            lx += work::caption(*ui_, {lx, chip.y, chip.w, chip.h}, Face::Text, 16.0f, term.word, ui::th().text) + 8;
            if (m.transcription) {
                lx += work::caption(*ui_, {lx, chip.y, chip.w, chip.h}, Face::Text, 12.0f, term.transcription, ui::th().accent) + 8;
            }
            work::caption(*ui_, {lx, chip.y, std::max(0.0f, chip.x + chip.w - lx), chip.h}, Face::Text, 13.0f, term.meaning, ui::th().text_2);
            cx += cw + 8;
        }
    }
    // The footer.
    const float fy = body.y + body.h - 44;
    renderer.rect({body.x, fy, body.w, 1}, {.fill = ui::th().line});
    work::caption(*ui_, {body.x + 22, fy, body.w - 44, 44}, Face::Text, 12.5f,
                t.copied ? text(buf, "{} sentences \xC2\xB7 the translation is in the clipboard", t.pairs.size())
                         : text(buf, "{} sentences", t.pairs.size()),
                ui::th().text_3);
}

auto Painter::translation_width(const Model& m) -> float {
    return (m.word || m.phrase) ? kCardW : 0.0f;
}

} // namespace app::sheet
