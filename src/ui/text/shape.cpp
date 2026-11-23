#include "ui/text/shape.hpp"

#include "core/hash.hpp"
#include "core/utf8.hpp"

#include <hb.h>

#include <bit>
#include <optional>

namespace ui::text {
namespace {

// Characters that belong to whatever script surrounds them: they stay in the current run when
// its font has them, so "你好, world" does not split the comma into another font.
auto is_common(char32_t cp) noexcept -> bool {
    if (cp < 0x80) {
        const char32_t lower = cp | 0x20;
        return !(lower >= 'a' && lower <= 'z');
    }
    return cp == 0xA0 || (cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x2000 && cp <= 0x206F) ||
           (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0x1F3FB && cp <= 0x1F3FF) ||
           (cp >= 0xE0100 && cp <= 0xE01EF);
}

void shape_run(FontSet& fonts, std::string_view text, std::size_t begin, std::size_t end,
               FontIndex font, float px, Shaped& out, std::uint8_t face, bool tabular) {
    if (begin == end) {
        return;
    }
    hb_buffer_t* buffer = fonts.scratch_buffer();
    hb_buffer_clear_contents(buffer);
    // The whole text is passed as context; only [begin, end) is shaped.
    hb_buffer_add_utf8(buffer, text.data(), static_cast<int>(text.size()),
                       static_cast<unsigned>(begin), static_cast<int>(end - begin));
    hb_buffer_guess_segment_properties(buffer);
    // Right-to-left scripts are not supported yet; shaping them left to right keeps the glyphs in
    // text order (clusters non-decreasing), which wrapping and drawing rely on.
    hb_buffer_set_direction(buffer, HB_DIRECTION_LTR);
    hb_font_t* hb = fonts.sized(font, px).hb;
    static const hb_feature_t kTabular{HB_TAG('t', 'n', 'u', 'm'), 1, HB_FEATURE_GLOBAL_START, HB_FEATURE_GLOBAL_END};
    hb_shape(hb, buffer, tabular ? &kTabular : nullptr, tabular ? 1u : 0u);

    unsigned count = 0;
    const hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buffer, &count);
    const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buffer, &count);
    hb_codepoint_t space = 0;
    const bool has_space = hb_font_get_nominal_glyph(hb, ' ', &space) != 0;
    for (unsigned i = 0; i < count; ++i) {
        float advance = static_cast<float>(pos[i].x_advance) / 64.0f;
        hb_codepoint_t id = info[i].codepoint;
        if (text[info[i].cluster] == '\t' && has_space) { // fonts have no tab glyph: 4 spaces
            id = space;
            advance = 4.0f * static_cast<float>(hb_font_get_glyph_h_advance(hb, space)) / 64.0f;
        }
        out.glyphs.push_back({id, face, info[i].cluster, advance,
                              static_cast<float>(pos[i].x_offset) / 64.0f,
                              static_cast<float>(pos[i].y_offset) / 64.0f, font});
        out.width += advance;
    }
}

} // namespace

void shape_append(FontSet& fonts, std::string_view text, std::size_t from, float px,
                  Shaped& out) {
    shape_append(fonts, text, from, text.size(), px, out, 0);
}

void shape_append(FontSet& fonts, std::string_view text, std::size_t from, std::size_t end,
                  float px, Shaped& out, std::uint8_t face, bool tabular, bool one_line) {
    const SizedFont& primary = fonts.sized(0, px);
    out.ascent = primary.ascent;
    out.descent = primary.descent;
    out.line_height = primary.line_height;

    std::size_t run_begin = from;
    std::optional<FontIndex> run_font;
    std::size_t pos = from;
    while (pos < end) {
        const auto d = core::utf8::decode(text, pos);
        if (d.cp == '\n' || d.cp == '\r') { // line breaks are the wrapper's, not glyphs
            if (run_font) {
                shape_run(fonts, text, run_begin, pos, *run_font, px, out, face, tabular);
            }
            // In one line of text (a label) a break reads as a space; "\r\n" as one.
            if (hb_codepoint_t space = 0; one_line && d.cp == '\n' && hb_font_get_nominal_glyph(primary.hb, ' ', &space) != 0) {
                const float advance = static_cast<float>(hb_font_get_glyph_h_advance(primary.hb, space)) / 64.0f;
                out.glyphs.push_back({space, face, static_cast<std::uint32_t>(pos), advance, 0.0f, 0.0f, 0});
                out.width += advance;
            }
            pos += d.length;
            run_begin = pos;
            run_font.reset();
            continue;
        }
        const FontIndex font = (run_font && is_common(d.cp) && fonts.has_glyph(*run_font, d.cp))
                                   ? *run_font
                                   : fonts.resolve(d.cp);
        if (run_font && font != *run_font) {
            shape_run(fonts, text, run_begin, pos, *run_font, px, out, face, tabular);
            run_begin = pos;
        }
        run_font = font;
        pos += d.length;
    }
    if (run_font) {
        shape_run(fonts, text, run_begin, pos, *run_font, px, out, face, tabular);
    }
}

auto shape(FontSet& fonts, std::string_view text, float px, bool tabular) -> Shaped {
    Shaped out;
    shape_append(fonts, text, 0, text.size(), px, out, 0, tabular, true);
    return out;
}

ShapeCache::ShapeCache() {
    entries_.reserve(1024); // its buckets: a busy screen's texts never make it rehash
    spare_.reserve(kSpare);
    constexpr std::size_t kWarm = 16;
    std::unordered_map<std::uint64_t, Entry> made;
    for (std::uint64_t i = 0; i < kWarm; ++i) {
        auto& e = made[i];
        e.text.reserve(kRoom);
        e.shaped.glyphs.reserve(kRoom);
    }
    for (std::uint64_t i = 0; i < kWarm; ++i) {
        spare_.push_back(made.extract(i));
    }
}

auto ShapeCache::get(FontSet& fonts, std::string_view text, float px, bool tabular) -> const Shaped& {
    const auto fonts_id = reinterpret_cast<std::uintptr_t>(&fonts);
    const auto px_bits = std::bit_cast<std::uint32_t>(px);
    std::uint64_t key = core::hash::fnv1a64(text);
    key = core::hash::fnv1a64(
        std::string_view{reinterpret_cast<const char*>(&px_bits), sizeof(px_bits)}, key);
    key = core::hash::fnv1a64(
        std::string_view{reinterpret_cast<const char*>(&fonts_id), sizeof(fonts_id)}, key);
    key ^= tabular ? 0x9E3779B97F4A7C15ull : 0;

    Entry* e = nullptr;
    if (auto it = entries_.find(key); it != entries_.end()) {
        e = &it->second;
        if (e->text == text && e->px == px && e->fonts == &fonts && e->tabular == tabular) {
            e->last_used = frame_;
            ++stats_.hits;
            return e->shaped;
        }
        // A hash collision simply replaces the older entry.
    } else if (!spare_.empty()) {
        // An evicted entry's node, its string and its glyphs' room kept: a text that changes
        // every second (a clock) allocates nothing once warm.
        auto node = std::move(spare_.back());
        spare_.pop_back();
        node.key() = key;
        e = &entries_.insert(std::move(node)).position->second;
    } else {
        // Made with the warm spares' room: evicted later, it serves a longer text (a clock's next
        // second) without growing - a one-character entry's node once did, now and then (phase 10).
        e = &entries_[key];
        e->text.reserve(kRoom);
        e->shaped.glyphs.reserve(kRoom);
    }
    ++stats_.shaped;
    e->text.assign(text);
    e->fonts = &fonts;
    e->px = px;
    e->tabular = tabular;
    e->last_used = frame_;
    e->shaped.glyphs.clear();
    e->shaped.width = 0;
    e->shaped.ascent = 0;
    e->shaped.descent = 0;
    e->shaped.line_height = 0;
    shape_append(fonts, text, 0, text.size(), px, e->shaped, 0, tabular, true);
    return e->shaped;
}

void ShapeCache::end_frame() {
    ++frame_;
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (frame_ - it->second.last_used <= static_cast<std::uint64_t>(kKeepFrames)) {
            ++it;
            continue;
        }
        auto next = std::next(it);
        if (spare_.size() < kSpare) {
            spare_.push_back(entries_.extract(it));
        } else {
            entries_.erase(it);
        }
        it = next;
    }
}

} // namespace ui::text
