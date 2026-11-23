#include "ui/selection.hpp"

#include <algorithm>
#include <array>

namespace ui {
namespace {

// A text's covered bytes [first, second) between `lo` and `hi`, for a run of `key` at [start, end).
auto cover(const TextKey& key, std::uint32_t start, std::uint32_t end, const TextPos& lo, const TextPos& hi)
    -> std::pair<std::uint32_t, std::uint32_t> {
    if (key < lo.key || hi.key < key) {
        return {0, 0};
    }
    const std::uint32_t from = key == lo.key ? std::max(start, lo.byte) : start;
    const std::uint32_t to = key == hi.key ? std::min(end, hi.byte) : end;
    return from < to ? std::pair{from, to} : std::pair<std::uint32_t, std::uint32_t>{0, 0};
}

} // namespace

void Selection::set(TextPos anchor, TextPos focus) noexcept {
    pane_ = anchor.key.pane;
    anchor_ = anchor;
    focus_ = focus.key.pane == pane_ ? focus : anchor;
    whole_ = false;
    kept_count_ = 0;
}

void Selection::extend(TextPos focus) noexcept {
    if (pane_ != 0 && focus.key.pane == pane_) {
        focus_ = focus;
    }
}

void Selection::select_all(Id pane) noexcept {
    pane_ = pane;
    anchor_ = focus_ = {};
    whole_ = pane != 0;
    kept_count_ = 0;
}

void Selection::clear() noexcept {
    pane_ = 0;
    anchor_ = focus_ = {};
    whole_ = false;
    kept_count_ = 0;
}

auto Selection::range() const noexcept -> std::pair<TextPos, TextPos> {
    return anchor_ < focus_ ? std::pair{anchor_, focus_} : std::pair{focus_, anchor_};
}

auto Selection::covered(const TextRun& run) const noexcept -> std::pair<std::uint32_t, std::uint32_t> {
    if (!active() || run.key.pane != pane_) {
        return {0, 0};
    }
    const std::uint32_t end = run.first_byte + run.text_length;
    if (whole_) {
        return run.text_length == 0 ? std::pair<std::uint32_t, std::uint32_t>{0, 0} : std::pair{run.first_byte, end};
    }
    const auto [lo, hi] = range();
    return cover(run.key, run.first_byte, end, lo, hi);
}

void Selection::seen(const TextMap& map) {
    if (!active()) {
        return;
    }
    // A text drawn now has its lines as drawn now: its kept lines of another wrap go (final review I3:
    // a paragraph wrapped again at a new width was copied with its old lines beside the new ones).
    std::size_t kept = 0;
    for (std::size_t i = 0; i < kept_count_; ++i) {
        const auto& k = kept_[i];
        const bool drawn = std::any_of(map.runs().begin(), map.runs().end(), [&](const TextRun& r) { return r.key == k.key; });
        const bool same_line = std::any_of(map.runs().begin(), map.runs().end(),
                                           [&](const TextRun& r) { return r.key == k.key && r.first_byte == k.first_byte; });
        if (!drawn || same_line) {
            if (kept != i) {
                std::swap(kept_[kept], kept_[i]); // its string's room goes along
            }
            ++kept;
        }
    }
    kept_count_ = kept;
    for (const auto& run : map.runs()) {
        const auto [from, to] = covered(run);
        if (from == to) {
            continue;
        }
        auto it = std::find_if(kept_.begin(), kept_.begin() + static_cast<std::ptrdiff_t>(kept_count_),
                               [&](const Kept& k) { return k.key == run.key && k.first_byte == run.first_byte; });
        if (it == kept_.begin() + static_cast<std::ptrdiff_t>(kept_count_)) {
            if (kept_count_ == kept_.size()) {
                kept_.emplace_back();
            }
            it = kept_.begin() + static_cast<std::ptrdiff_t>(kept_count_++);
        }
        it->key = run.key;
        it->first_byte = run.first_byte;
        it->soft_break_after = run.soft_break_after;
        it->row = run.row;
        it->col = run.col;
        it->text.assign(map.text(run)); // its room kept: no allocation once it has grown
    }
}

void Selection::copy(const TextMap& map, std::string& out) const {
    out.clear();
    if (!active()) {
        return;
    }
    // The pieces in reading order: this frame's runs, then the kept ones not drawn now.
    struct Piece {
        TextKey key;
        std::uint32_t first_byte;
        std::string_view text;
        bool soft;
        int row;
        std::string_view full; // a cut one's whole text: copied when all it shows is covered
    };
    std::vector<Piece> pieces;
    pieces.reserve(map.runs().size() + kept_count_);
    for (const auto& run : map.runs()) {
        if (run.key.pane == pane_ && covered(run).first != covered(run).second) {
            pieces.push_back({run.key, run.first_byte, map.text(run), run.soft_break_after, run.row, run.cut ? map.full(run) : std::string_view{}});
        }
    }
    const std::size_t drawn = pieces.size();
    for (std::size_t i = 0; i < kept_count_; ++i) {
        const auto& k = kept_[i];
        const bool shown = std::any_of(pieces.begin(), pieces.begin() + static_cast<std::ptrdiff_t>(drawn),
                                       [&](const Piece& p) { return p.key == k.key && p.first_byte == k.first_byte; });
        if (!shown) {
            pieces.push_back({k.key, k.first_byte, k.text, k.soft_break_after, k.row, {}});
        }
    }
    std::ranges::sort(pieces, [](const Piece& a, const Piece& b) { return a.key != b.key ? a.key < b.key : a.first_byte < b.first_byte; });
    const auto [lo, hi] = range();
    const Piece* before = nullptr;
    for (const auto& p : pieces) {
        const auto end = p.first_byte + static_cast<std::uint32_t>(p.text.size());
        const auto [from, to] = whole_ ? std::pair{p.first_byte, end} : cover(p.key, p.first_byte, end, lo, hi);
        if (from == to) {
            continue;
        }
        if (before != nullptr) {
            if (before->key == p.key) {
                out += before->soft ? "" : "\n"; // wrapped on screen: its source's own spaces are in the run
            } else {
                out += before->row >= 0 && before->row == p.row ? "\t" : "\n";
            }
        }
        if (!p.full.empty() && from == p.first_byte && to == end) {
            out.append(p.full); // shown cut with "…": all of it was selected, all of it is copied
        } else {
            out.append(p.text.substr(from - p.first_byte, to - from));
        }
        before = &p;
    }
}

} // namespace ui
