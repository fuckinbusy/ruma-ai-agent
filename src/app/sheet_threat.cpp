// The threat check's report in the data panel (phase 13a § 5.3, design/ui_system 05b–05g): the
// verdict in words with its counts, the findings by level — the suspicious alone in red — one
// opened at a time with its reasons, its facts (a registry key read-only) and only its own actions,
// then what was checked, step by step, and what was not without administrator rights. Every line
// is the report's own (present made them): nothing is made in a frame.

#include "app/sheet_view.hpp"

#include "core/hash.hpp"
#include "ui/theme.hpp"
#include "ui/work.hpp"

#include <algorithm>
#include <array>
#include <cmath>
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
constexpr float kVerdict = 84;
constexpr float kGroup = 32;
constexpr float kRow = 54;
constexpr float kWhy = 22;
constexpr float kFact = 22;
constexpr float kActions = 46;
constexpr float kStage = 30;
constexpr char32_t kIconWarn = 0xE7BA;
constexpr char32_t kIconOk = 0xE73E;
constexpr char32_t kIconCross = 0xE711;
constexpr char32_t kIconChevronRight = 0xE76C;
constexpr char32_t kIconShield = 0xEA18;
constexpr char32_t kIconBug = 0xEBE8;

auto id_of(std::string_view name, std::uint64_t n = 0) -> ui::Id {
    return core::hash::fnv1a64(name, core::hash::fnv1a64("app/sheet")) * 1099511628211ull + n;
}

template <std::size_t N, class... Args>
auto text(std::array<char, N>& buf, std::format_string<Args...> f, Args&&... args) -> std::string_view {
    const auto r = std::format_to_n(buf.data(), buf.size(), f, std::forward<Args>(args)...);
    return {buf.data(), static_cast<std::size_t>(r.out - buf.data())};
}

auto level_color(int level) -> Color {
    return level == 0 ? ui::th().crit : level == 1 ? ui::th().warn : ui::th().ok;
}
auto level_soft(int level) -> Color {
    return level == 0 ? ui::th().crit_soft : level == 1 ? ui::th().warn_soft : ui::th().ok_soft;
}

// A fact's value in at most two lines of `width`, broken anywhere (a path has no spaces).
auto two(ui::Ui& ui, Face face, float size, std::string_view s, float width) -> std::pair<std::string_view, std::string_view> {
    if (work::text_width(ui, face, size, s) <= width) {
        return {s, {}};
    }
    std::size_t cut = 0;
    for (std::size_t i = 1; i < s.size(); ++i) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) {
            continue;
        }
        if (work::text_width(ui, face, size, s.substr(0, i)) > width) {
            break;
        }
        cut = i;
    }
    return cut == 0 ? std::pair{s, std::string_view{}} : std::pair{s.substr(0, cut), s.substr(cut)};
}

// "1 175": its thousands apart, into `buf`.
template <std::size_t N>
auto grouped(std::array<char, N>& buf, std::size_t n) -> std::string_view {
    std::array<char, 24> digits{};
    const auto d = std::format_to_n(digits.data(), digits.size(), "{}", n);
    const auto len = static_cast<std::size_t>(d.out - digits.data());
    std::size_t o = 0;
    for (std::size_t i = 0; i < len && o + 1 < N; ++i) {
        if (i > 0 && (len - i) % 3 == 0) {
            buf[o++] = ' ';
        }
        buf[o++] = digits[i];
    }
    return {buf.data(), o};
}

namespace vm = ui::motion::verdict;
// The verdict's coming (MS § 7): the block's room, its words and their lift, its three counts
// (+0, +1, +2), a stage's mark and its showing.
const ui::Id kVerdictRoomTrack = id_of("threat:verdict-room");
const ui::Id kVerdictWordsTrack = id_of("threat:verdict-words");
const ui::Id kVerdictLiftTrack = id_of("threat:verdict-lift");
const ui::Id kVerdictCountTrack = id_of("threat:verdict-count");
const ui::Id kStageMarkTrack = id_of("threat:stage-mark");
const ui::Id kStageMarkInTrack = id_of("threat:stage-mark-in");

auto mono(std::string_view key) -> bool {
    return key == "Path" || key == "Registry" || key == "Command";
}

} // namespace

void Painter::draw_threat(Model& m, const events::DataSet& set, const Box& body, Reaction& r) {
    auto& renderer = ui_->renderer();
    const auto& v = *set.threat;
    std::array<char, 64> buf{};
    if (threat_set_ != set.id) { // another report: nothing open, from the top
        threat_set_ = set.id;
        threat_open_ = -1;
        threat_scroll_ = {};
        if (!ui_->inert() && !reduced_motion_) { // its verdict comes (MS § 7), once: a report made again is the same set
            threat_shown_at_ = now_;
            anim_.place(kVerdictRoomTrack, 0.0f);
            anim_.place(kVerdictWordsTrack, 0.0f);
            anim_.place(kVerdictLiftTrack, vm::kLift);
            for (ui::Id i = 0; i < 3; ++i) {
                anim_.place(kVerdictCountTrack + i, 0.0f);
            }
            anim_.place(kStageMarkTrack, vm::kMarkFrom);
            anim_.place(kStageMarkInTrack, 0.0f);
        }
    }
    // MS § 7 "Verdict": kAt after the report is there its block grows on smooth, its words come on
    // quick kContentsAt later from kLift px under, its counts from 0 on smooth one after another; a
    // stage's mark with the words, from .9 of its size.
    ThreatMotion verdict;
    {
        const auto seconds = [](std::chrono::milliseconds ms) { return std::chrono::duration<double>(ms).count(); };
        const double age = now_ - threat_shown_at_;
        const double words_at = seconds(vm::kAt) + seconds(vm::kContentsAt);
        const bool block_due = age >= seconds(vm::kAt) - 1e-9;
        const bool words_due = age >= words_at - 1e-9;
        verdict.room = std::clamp(anim_.to(kVerdictRoomTrack, block_due ? 1.0f : 0.0f, ui::motion::kSmooth), 0.0f, 1.0f);
        verdict.words = std::clamp(anim_.to(kVerdictWordsTrack, words_due ? 1.0f : 0.0f, ui::motion::kQuick), 0.0f, 1.0f);
        verdict.lift = anim_.to(kVerdictLiftTrack, words_due ? 0.0f : vm::kLift, ui::motion::kBase);
        const std::array<std::size_t, 3> counts{v.suspicious, v.worth, v.fine};
        for (std::size_t i = 0; i < 3; ++i) {
            const bool due = age >= words_at + static_cast<double>(i) * seconds(vm::kCountStagger) - 1e-9;
            verdict.counts[i] = anim_.to(kVerdictCountTrack + i, due ? static_cast<float>(counts[i]) : 0.0f, ui::motion::kSmooth);
        }
        verdict.mark = anim_.to(kStageMarkTrack, words_due ? 1.0f : vm::kMarkFrom, ui::motion::kBase);
        if (age < words_at + 2 * seconds(vm::kCountStagger) + 0.05) {
            redraw_due_ = true; // their turns come on frames
        }
        if (!ui_->inert()) {
            threat_motion_ = verdict;
        }
    }
    const float mark_in = std::clamp(anim_.to(kStageMarkInTrack, verdict.mark > vm::kMarkFrom ? 1.0f : 0.0f, ui::motion::kQuick), 0.0f, 1.0f);
    const float verdict_h = (kVerdict + 12) * verdict.room;
    const float x = body.x + kPadX;
    const float w = body.w - 2 * kPadX;
    // A finding's opening (MS 10), by its place in this report.
    const auto fold_key = [&](std::size_t row) { return id_of("threat:fold", static_cast<std::uint64_t>(set.id) * 4096 + row); };

    // The header: its title, its chip and time.
    const float top = body.y + 18;
    work::caption(*ui_, {x, top, w, 24}, Face::Display, 18.0f, set.title, ui::th().text);
    const float my = top + 24 + 7;
    const float sw = std::min(work::text_width(*ui_, Face::Mono, 12.0f, set.source) + 36, w);
    renderer.rect({x, my, sw, 22}, {.fill = ui::th().w1, .radius = {7, 7, 7, 7}, .border = ui::th().line, .border_width = 1});
    ui_->draw_icon(kIconShield, 12.0f, {x + 15, my + 11}, ui::th().text_3);
    work::caption(*ui_, {x + 27, my, sw - 36, 22}, Face::Mono, 12.0f, set.source, ui::th().text_2);
    work::caption(*ui_, {x + sw + 10, my, 120, 22}, Face::Text, 12.5f, set.snapshot, ui::th().text_3);
    const float y0 = my + 22 + 14;

    // A finding's facts block, as drawn: a line each, a path's second line, a read-only badge.
    const float fact_w = w - 56 - 16 - 12 - 96 - 12;
    const auto facts_h = [&](const events::ThreatRow& row) {
        if (row.facts.empty()) {
            return 0.0f;
        }
        float h = 12;
        for (const auto& fact : row.facts) {
            h += kFact;
            if (!two(*ui_, mono(fact.key) ? Face::Mono : Face::Text, 12.5f, fact.value, fact_w).second.empty()) {
                h += kFact - 4;
            }
            if (fact.read_only) {
                h += 18;
            }
        }
        return h;
    };
    const auto open_h = [&](const events::ThreatRow& row) {
        const float facts = facts_h(row);
        return static_cast<float>(row.why.size()) * kWhy + 12 + (facts > 0 ? facts + 12 : 0.0f) + kActions;
    };
    // The content's height, for its scroll.
    const auto rows_h = [&] {
        float h = 0;
        for (int level = 0; level < 2; ++level) {
            const bool any = std::ranges::any_of(v.rows, [&](const events::ThreatRow& row) { return row.level == level; });
            if (any) {
                h += kGroup;
            }
        }
        for (std::size_t i = 0; i < v.rows.size(); ++i) {
            h += kRow;
            // MS 10: its room as it is now — opening, open, or still closing.
            if (const float room = disclosed(fold_key(i), static_cast<int>(i) == threat_open_).room; room > 0.001f) {
                h += open_h(v.rows[i]) * room;
            }
        }
        return h + kGroup; // look fine
    }();
    const float defender_h = v.defender_on ? 0.0f : 44.0f;
    // Check deeper (phase 13a § 6): under what was not read, for a finished report; once deeper, a line.
    const bool can_deepen = !v.deeper && !v.stopped_at && !v.not_read.empty();
    const float deeper_h = v.deeper ? 34.0f : can_deepen ? 46.0f : 0.0f;
    const float checked_h = 30 + std::ceil(static_cast<float>(v.stages.size()) / 2.0f) * kStage + 16 +
                            (v.not_read.empty() ? 0.0f : 24.0f * static_cast<float>(v.not_read.size()) + 6) + deeper_h;
    const float content = verdict_h + defender_h + rows_h + 12 + checked_h + 20;
    const Box view{body.x, y0, body.w, std::max(0.0f, body.y + body.h - y0)};
    threat_scroll_.foot = work::corner_foot(bottom_corners(view).br);
    const Box in = work::scroll_view(*ui_, threat_scroll_, view, content);
    float y = in.y;

    // The verdict, its honest line, its counts; stopped: the step to go on from.
    {
        const bool found = v.suspicious + v.worth > 0;
        const bool stopped = v.stopped_at.has_value();
        const Color tone = stopped ? ui::th().text_2 : found ? ui::th().warn : ui::th().ok;
        const Box box{x, y + verdict.lift, w, kVerdict};       // its words' place: kLift px under, coming
        const Box room{x, y, w, std::min(kVerdict, verdict_h)}; // as much of the block as there is now
        add_target("threat:verdict", room);
        if (room.h > 1.0f) {
            renderer.rect({room.x, room.y, room.w, room.h},
                          {.fill = stopped ? ui::th().w1 : found ? ui::th().warn_soft : ui::th().ok_soft,
                           .radius = ui::card_corners(),
                           .border = stopped ? ui::th().line : found ? ui::th().warn_line : ui::th().ok_line,
                           .border_width = 1});
        }
        // Its words in that room: coming on quick, from kLift px under.
        const float verdict_opacity = renderer.opacity();
        const bool verdict_inert = ui_->inert();
        renderer.push_clip({room.x, room.y, room.w, room.h});
        renderer.set_opacity(verdict_opacity * verdict.words);
        ui_->set_inert(verdict_inert || verdict.words < 0.5f);
        renderer.rect({box.x + 14, box.y + 22, 40, 40}, {.fill = stopped ? ui::th().w2 : found ? ui::th().warn_soft_hi : ui::th().ok_soft_hi, .radius = {10, 10, 10, 10}});
        ui_->draw_icon(found || stopped ? kIconWarn : kIconOk, 17.0f, {box.x + 34, box.y + 42}, tone);
        // The counts: each column as wide as its number or its name takes at the user's text size,
        // kCountGap apart (they were 75 px whatever their words took: "worth a look look fine").
        constexpr float kCountGap = 18.0f;
        const std::array<std::size_t, 3> counts{static_cast<std::size_t>(std::lround(std::max(0.0f, verdict.counts[0]))),
                                                static_cast<std::size_t>(std::lround(std::max(0.0f, verdict.counts[1]))),
                                                static_cast<std::size_t>(std::lround(std::max(0.0f, verdict.counts[2])))};
        const std::array<std::string_view, 3> names{"suspicious", "worth a look", "look fine"};
        const std::array<std::size_t, 3> finals{v.suspicious, v.worth, v.fine}; // a column keeps its width while its number counts up
        std::array<float, 3> count_w{};
        float counts_w = stopped ? 190.0f : 2 * kCountGap;
        for (std::size_t i = 0; !stopped && i < 3; ++i) {
            count_w[i] = std::ceil(std::max(work::text_width(*ui_, Face::Semibold, 20.0f, grouped(buf, finals[i])),
                                            work::text_width(*ui_, Face::Text, 12.0f, names[i]))) + 1;
            counts_w += count_w[i];
        }
        const float tx = box.x + 14 + 40 + 14;
        const float tw = box.w - (tx - box.x) - counts_w - (stopped ? 12.0f : 14.0f + 20.0f); // stopped: its button, as before
        work::caption(*ui_, {tx, box.y + 14, tw, 22}, Face::Semibold, 16.5f, v.verdict, ui::th().text);
        const auto [c1, c2] = two_lines(*ui_, Face::Text, 13.0f, v.caveat, tw);
        work::caption(*ui_, {tx, box.y + 38, tw, 18}, Face::Text, 13.0f, c1, ui::th().text_2);
        work::caption(*ui_, {tx, box.y + 56, tw, 18}, Face::Text, 13.0f, c2, ui::th().text_2);
        if (stopped) {
            const auto label = text(buf, "Continue from step {}", *v.stopped_at);
            const auto size = work::button_size(*ui_, label);
            const Box go{box.x + box.w - 14 - size.w - 10, box.y + (box.h - size.h) / 2, size.w + 10, size.h};
            add_target("threat:continue", go);
            if (work::button(*ui_, id_of("threat:continue"), go, label, work::ButtonStyle::Primary) && !r.command) {
                r.command = press(m, text(buf, "check_from:{}", *v.stopped_at));
            }
        } else {
            // Each on its way from 0 (MS § 7), written whole.
            float cx = box.x + box.w - 14 - counts_w;
            for (std::size_t i = 0; i < 3; ++i) {
                const float cw = count_w[i];
                const Box c{cx, box.y + 16, cw, 52};
                add_target(text(buf, "threat:count:{}", i), c);
                work::caption(*ui_, {c.x, c.y, cw, 26}, Face::Semibold, 20.0f, grouped(buf, counts[i]), level_color(static_cast<int>(i)));
                work::caption(*ui_, {c.x, c.y + 28, cw, 18}, Face::Text, 12.0f, names[i], ui::th().text_3);
                cx += cw + kCountGap;
            }
        }
        ui_->set_inert(verdict_inert);
        renderer.set_opacity(verdict_opacity);
        renderer.pop_clip();
        y += verdict_h;
    }

    // Defender off (05f): what that leaves out.
    if (!v.defender_on) {
        const Box b{x, y, w, 34};
        add_target("threat:no-defender", b);
        renderer.rect({b.x, b.y, b.w, b.h}, {.fill = ui::th().warn_soft, .radius = {9, 9, 9, 9},
                                              .border = ui::th().warn_line, .border_width = 1});
        ui_->draw_icon(kIconWarn, 13.0f, {b.x + 16, b.y + 17}, ui::th().warn);
        work::caption(*ui_, {b.x + 32, b.y, b.w - 40, b.h}, Face::Text, 12.5f,
                    "Microsoft Defender is off: its scan and its quarantine are not there. The agent's own checks still ran.",
                    ui::th().text_2);
        y += 44;
    }

    // The findings, by level; one opened.
    const Box card{x, y, w, rows_h};
    renderer.rect({card.x, card.y, card.w, card.h}, {.fill = ui::th().w1, .radius = ui::card_corners(), .border = ui::th().line, .border_width = 1});
    static constexpr std::array<std::string_view, 3> kGroups{"SUSPICIOUS", "WORTH A LOOK", "LOOK FINE"};
    const auto group = [&](int level, std::size_t count) {
        const Box g{x, y, w, kGroup};
        add_target(text(buf, "threat:group:{}", level), g);
        renderer.rect({g.x + 14, g.y + kGroup / 2 - 4, 8, 8}, {.fill = level_color(level), .radius = {4, 4, 4, 4}});
        const float lw = work::text_width(*ui_, Face::Semibold, 11.0f, kGroups[static_cast<std::size_t>(level)]);
        work::caption(*ui_, {g.x + 30, g.y, lw, kGroup}, Face::Semibold, 11.0f, kGroups[static_cast<std::size_t>(level)], level_color(level));
        const auto n = grouped(buf, count);
        const float nw = work::text_width(*ui_, Face::Text, 11.0f, n) + 12;
        renderer.rect({g.x + 30 + lw + 8, g.y + kGroup / 2 - 9, nw, 18}, {.fill = ui::th().w2, .radius = {9, 9, 9, 9}});
        work::caption(*ui_, {g.x + 30 + lw + 14, g.y, nw, kGroup}, Face::Text, 11.0f, n, ui::th().text_2);
        y += kGroup;
    };
    for (int level = 0; level < 2; ++level) {
        const auto count = static_cast<std::size_t>(std::ranges::count(v.rows, level, &events::ThreatRow::level));
        if (count == 0) {
            continue;
        }
        group(level, count);
        for (std::size_t i = 0; i < v.rows.size(); ++i) {
            const auto& row = v.rows[i];
            if (row.level != level) {
                continue;
            }
            const bool open = static_cast<int>(i) == threat_open_;
            const Color tone = level_color(level);
            // MS 10: its room grows on smooth, its body comes 80 ms later from above, its chevron
            // turns; closing, the body goes first.
            const auto fold = disclosed(fold_key(i), open);
            const float shown = fold.room > 0.001f ? open_h(row) * fold.room : 0.0f;
            if (fold.ground > 0.004f) { // .fnd opened: a faint red ground and a 2 px bar at its left
                renderer.rect({x + 1, y, w - 2, kRow + shown}, {.fill = faint(level_soft(level), fold.ground)});
                renderer.rect({x + 1, y, 2, kRow + shown}, {.fill = faint(tone, fold.ground)});
            }
            const Box head{x, y, w, kRow};
            add_target(text(buf, "threat:row:{}", i), head);
            const auto hit = ui_->interact(id_of("threat:row", i), head, false);
            const float h = ui_->hover(id_of("threat:row", i), hit.hovered);
            if (h > 0.01f && !open) {
                renderer.rect({head.x + 1, head.y, head.w - 2, head.h}, {.fill = faint(ui::th().w1, h)});
            }
            renderer.rect({head.x + 14, head.y + 12, 30, 30}, {.fill = level_soft(level), .radius = {8, 8, 8, 8}});
            ui_->draw_icon(level == 0 ? kIconBug : kIconWarn, 14.0f, {head.x + 29, head.y + 27}, tone);
            work::caption(*ui_, {head.x + 56, head.y + 9, head.w - 56 - 40, 20}, Face::Semibold, 13.5f, row.title, ui::th().text);
            work::caption(*ui_, {head.x + 56, head.y + 29, head.w - 56 - 40, 18}, Face::Text, 12.0f, row.sub, ui::th().text_3);
            work::chevron(*ui_, {head.x + head.w - 20, head.y + kRow / 2}, 11.0f, fold.chevron, ui::th().text_3);
            if (hit.clicked) {
                if (!open && threat_open_ >= 0) {
                    disclose(fold_key(static_cast<std::size_t>(threat_open_)), false); // the one that was open closes as this comes
                }
                disclose(fold_key(i), !open);
                threat_open_ = open ? -1 : static_cast<int>(i);
                redraw_due_ = true;
            }
            y += kRow;
            if (shown <= 0.0f) {
                continue;
            }
            // Its body in the room it has now; one closing takes no click and names no target.
            const float body_end = y + shown;
            const float body_opacity = renderer.opacity();
            const bool was_inert = ui_->inert();
            const auto targets_kept = target_count_;
            renderer.push_clip({x, y, w, shown});
            renderer.set_opacity(body_opacity * fold.body_alpha);
            ui_->set_inert(was_inert || !open);
            y += fold.body_dy;
            // Why: a line each.
            for (std::size_t k = 0; k < row.why.size(); ++k) {
                const Box line{x + 56, y, w - 56 - 20, kWhy};
                add_target(text(buf, "threat:why:{}", k), line);
                ui_->draw_icon(kIconWarn, 12.0f, {line.x - 14, line.y + kWhy / 2}, tone);
                work::caption(*ui_, {line.x, line.y, line.w, kWhy}, Face::Text, 13.5f, row.why[k], ui::th().text_2);
                y += kWhy;
            }
            y += 12;
            // The facts: a key's column of 96, a path in mono broken anywhere, a registry key read-only.
            if (!row.facts.empty()) {
                const Box facts{x + 56, y, w - 56 - 16, facts_h(row)};
                add_target("threat:facts", facts);
                renderer.rect({facts.x, facts.y, facts.w, facts.h}, {.fill = ui::th().sunk, .radius = {9, 9, 9, 9}});
                float fy = facts.y + 6;
                for (const auto& fact : row.facts) {
                    work::caption(*ui_, {facts.x + 12, fy, 90, kFact}, Face::Text, 12.0f, fact.key, ui::th().text_3);
                    const Face face = mono(fact.key) ? Face::Mono : Face::Text;
                    const float vw = fact_w;
                    const auto [l1, l2] = two(*ui_, face, 12.5f, fact.value, vw);
                    work::caption(*ui_, {facts.x + 12 + 96, fy, vw, kFact}, face, 12.5f, l1,
                                fact.value == "not signed" ? ui::th().crit : ui::th().text);
                    if (!l2.empty()) {
                        work::caption(*ui_, {facts.x + 12 + 96, fy + kFact - 4, vw, kFact}, face, 12.5f, l2, ui::th().text);
                        fy += kFact - 4;
                    }
                    if (fact.read_only) {
                        constexpr std::string_view kRo = "registry \xC2\xB7 read-only";
                        const float bw = work::text_width(*ui_, Face::Text, 11.0f, kRo) + 30;
                        const Box ro{facts.x + 12 + 96, fy + kFact, bw, 18};
                        add_target("threat:read-only", ro);
                        renderer.rect({ro.x, ro.y, ro.w, ro.h}, {.fill = ui::th().w1, .radius = {9, 9, 9, 9}, .border = ui::th().line, .border_width = 1});
                        ui_->draw_icon(kIconLock, 10.0f, {ro.x + 11, ro.y + 9}, ui::th().text_3);
                        work::caption(*ui_, {ro.x + 20, ro.y, bw - 24, 18}, Face::Text, 11.0f, kRo, ui::th().text_3);
                        fy += 18;
                    }
                    fy += kFact;
                }
                y += facts.h + 12;
            }
            // Its own actions, the first in front; each asks first.
            float ax = x + 56;
            for (std::size_t k = 0; k < row.actions.size(); ++k) {
                const auto size = work::button_size(*ui_, row.actions[k], true);
                const Box b{ax, y + 4, size.w + 10, size.h};
                add_target(text(buf, "threat:action:{}", k), b);
                if (work::button(*ui_, id_of("threat:action", i * 16 + k), b, row.actions[k],
                                 k == 0 ? work::ButtonStyle::Primary : work::ButtonStyle::Normal, true) &&
                    !r.command) {
                    r.command = press(m, text(buf, "finding:{}:{}", i, k));
                }
                ax += b.w + 8;
            }
            work::caption(*ui_, {ax + 8, y + 4, std::max(0.0f, x + w - 14 - ax - 8), 28}, Face::Text, 12.0f, "each one asks first",
                        ui::th().text_3);
            ui_->set_inert(was_inert);
            renderer.set_opacity(body_opacity);
            renderer.pop_clip();
            if (!open) {
                target_count_ = targets_kept;
            }
            y = body_end;
        }
    }
    group(2, v.fine);
    y += 12;

    // What was checked: the steps with their numbers; what was not, without administrator rights.
    work::caption(*ui_, {x, y, w, 20}, Face::Semibold, 11.0f, text(buf, "WHAT I CHECKED \xC2\xB7 {}", v.checked), ui::th().text_3);
    y += 30;
    const float half = (w - 12) / 2;
    for (std::size_t k = 0; k < v.stages.size(); ++k) {
        const auto& s = v.stages[k];
        const Box b{x + static_cast<float>(k % 2) * (half + 12), y + static_cast<float>(k / 2) * kStage, half, kStage};
        add_target(text(buf, "threat:stage:{}", k), b);
        const Color c = s.state == 1 ? ui::th().ok : s.state == 2 ? ui::th().crit : ui::th().text_3;
        ui_->draw_icon(s.state == 1 ? kIconOk : s.state == 2 ? kIconCross : kIconChevronRight, 11.0f, {b.x + 9, b.y + kStage / 2}, c);
        work::caption(*ui_, {b.x + 24, b.y, b.w - 24 - 110, kStage}, Face::Text, 13.0f, s.name, s.state == 0 ? ui::th().text_3 : ui::th().text);
        const float cw = work::text_width(*ui_, Face::Text, 12.0f, s.count);
        if (verdict.mark >= 0.999f && mark_in >= 0.999f) {
            work::caption(*ui_, {b.x + b.w - cw, b.y, cw, kStage}, Face::Text, 12.0f, s.count, ui::th().text_3);
        } else if (mark_in > 0.004f && !s.count.empty()) {
            // MS § 7 "Finding mark": coming, .9 → 1 of its size about its middle, shown on quick.
            const float zoom = verdict.mark;
            const float was = renderer.opacity();
            renderer.set_opacity(was * mark_in);
            const float base = ui_->centred_baseline(Face::Text, 12.0f, b.y, kStage) - 12.0f * 0.35f * (1.0f - zoom);
            ui_->draw_text_zoomed(Face::Text, ui_->shape(Face::Text, s.count, 12.0f), 12.0f,
                                  {b.x + b.w - cw + cw * (1.0f - zoom) / 2, base}, ui::th().text_3, zoom);
            renderer.set_opacity(was);
        }
    }
    y += std::ceil(static_cast<float>(v.stages.size()) / 2.0f) * kStage + 16;
    if (!v.not_read.empty()) {
        add_target("threat:not-read", {x, y, w, 24.0f * static_cast<float>(v.not_read.size())});
        for (const auto& n : v.not_read) {
            ui_->draw_icon(kIconLock, 11.0f, {x + 9, y + 12}, ui::th().text_3);
            work::caption(*ui_, {x + 24, y, w - 24, 24}, Face::Text, 12.5f, n, ui::th().text_3);
            y += 24;
        }
        y += 6;
    }
    if (can_deepen) {
        constexpr std::string_view label = "Check deeper";
        const auto size = work::button_size(*ui_, label);
        const Box go{x, y + 6, size.w + 10, size.h};
        add_target("threat:deeper", go);
        if (work::button(*ui_, id_of("threat:deeper"), go, label, work::ButtonStyle::Normal) && !r.command) {
            r.command = press(m, text(buf, "deeper:{}", set.id));
        }
        work::caption(*ui_, {go.x + go.w + 12, go.y, w - go.w - 12, go.h}, Face::Text, 12.5f,
                    "Reads these as administrator, after Windows asks you. It changes nothing.", ui::th().text_3);
    } else if (v.deeper) {
        const Box done{x, y + 6, w, 24};
        add_target("threat:deeper-done", done);
        ui_->draw_icon(kIconOk, 11.0f, {done.x + 9, done.y + 12}, ui::th().ok);
        work::caption(*ui_, {done.x + 24, done.y, done.w - 24, done.h}, Face::Text, 12.5f,
                    "Checked deeper with administrator rights: what is above was read too.", ui::th().text_2);
    }
    work::end_scroll(*ui_, threat_scroll_, view, panel_color(), bottom_corners(view));
}

} // namespace app::sheet
