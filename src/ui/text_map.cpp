#include "ui/text_map.hpp"

#include <algorithm>

namespace ui {

void TextMap::begin_frame() {
    runs_.clear();
    chars_.clear();
}

void TextMap::add(const TextRun& run, std::string_view text, std::string_view full) {
    TextRun r = run;
    r.text_at = static_cast<std::uint32_t>(chars_.size());
    r.text_length = static_cast<std::uint32_t>(text.size());
    chars_.append(text);
    r.full_at = static_cast<std::uint32_t>(chars_.size());
    r.full_length = static_cast<std::uint32_t>(full.size());
    chars_.append(full);
    runs_.push_back(r);
}

auto TextMap::text(const TextRun& run) const -> std::string_view {
    return std::string_view{chars_}.substr(run.text_at, run.text_length);
}

auto TextMap::full(const TextRun& run) const -> std::string_view {
    return run.full_length == 0 ? text(run) : std::string_view{chars_}.substr(run.full_at, run.full_length);
}

auto TextMap::x_of(const TextRun& run, std::uint32_t byte) const -> float {
    float x = run.box.x;
    if (run.shaped == nullptr) {
        return x;
    }
    const auto& glyphs = run.shaped->glyphs;
    for (std::uint32_t g = run.first_glyph; g < run.end_glyph && g < glyphs.size(); ++g) {
        if (glyphs[g].cluster >= byte) {
            return x;
        }
        x += glyphs[g].x_advance / run.scale;
    }
    return x;
}

auto TextMap::byte_at(const TextRun& run, float x) const -> std::uint32_t {
    const auto end = run.first_byte + run.text_length;
    if (run.shaped == nullptr) {
        return x < run.box.x ? run.first_byte : end;
    }
    float pen = run.box.x;
    const auto& glyphs = run.shaped->glyphs;
    std::uint32_t g = run.first_glyph;
    while (g < run.end_glyph && g < glyphs.size()) {
        // A cluster's glyphs together (a ligature, a mark on its letter).
        const auto cluster = glyphs[g].cluster;
        float advance = 0;
        std::uint32_t next = g;
        while (next < run.end_glyph && next < glyphs.size() && glyphs[next].cluster == cluster) {
            advance += glyphs[next].x_advance / run.scale;
            ++next;
        }
        if (x < pen + advance / 2) {
            return std::max(cluster, run.first_byte);
        }
        pen += advance;
        g = next;
    }
    return end;
}

auto TextMap::at(float x, float y) const -> std::optional<TextPos> {
    for (const auto& run : runs_) {
        const auto& b = run.box;
        if (y < b.y || y >= b.y + b.h || x < b.x - 2 || x > b.x + b.w + 2) {
            continue;
        }
        return TextPos{run.key, byte_at(run, x)};
    }
    return std::nullopt;
}

} // namespace ui
