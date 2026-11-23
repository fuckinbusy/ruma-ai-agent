#include "ui/text/atlas.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <climits>
#include <ranges>
#include <cmath>
#include <cstring>
#include <utility>

namespace ui::text {

auto ShelfPacker::insert(int w, int h, std::uint64_t frame) -> std::optional<Slot> {
    if (w > width_ || h > height_ || w <= 0 || h <= 0) {
        return std::nullopt;
    }
    const int shelf_height = (h + 3) & ~3;
    const int tolerance = std::max(4, h / 4);

    int chosen = -1;
    int best_waste = INT_MAX;
    for (int i = 0; i < static_cast<int>(shelves_.size()); ++i) {
        const Shelf& s = shelves_[static_cast<std::size_t>(i)];
        const int waste = s.height - h;
        if (waste >= 0 && waste <= tolerance && width_ - s.used >= w && waste < best_waste) {
            chosen = i;
            best_waste = waste;
        }
    }
    if (chosen < 0 && next_y_ + shelf_height <= height_) {
        shelves_.push_back({next_y_, shelf_height, 0, frame});
        next_y_ += shelf_height;
        chosen = static_cast<int>(shelves_.size()) - 1;
    }
    if (chosen < 0) { // full: reuse the least recently used shelf not used in this frame
        for (int i = 0; i < static_cast<int>(shelves_.size()); ++i) {
            const Shelf& s = shelves_[static_cast<std::size_t>(i)];
            if (s.height < h || s.last_used >= frame) {
                continue;
            }
            if (chosen < 0 || s.last_used < shelves_[static_cast<std::size_t>(chosen)].last_used) {
                chosen = i;
            }
        }
        if (chosen < 0) {
            return std::nullopt;
        }
        shelves_[static_cast<std::size_t>(chosen)].used = 0;
        evicted_.push_back(chosen);
    }
    Shelf& s = shelves_[static_cast<std::size_t>(chosen)];
    const Slot slot{s.used, s.y, w, h, chosen};
    s.used += w;
    s.last_used = frame;
    return slot;
}

void ShelfPacker::touch(int shelf, std::uint64_t frame) {
    auto& s = shelves_[static_cast<std::size_t>(shelf)];
    s.last_used = std::max(s.last_used, frame);
}

auto ShelfPacker::take_evicted() -> std::vector<int> {
    return std::exchange(evicted_, {});
}

void ShelfPacker::reset() {
    shelves_.clear();
    evicted_.clear();
    next_y_ = 0;
}

auto GlyphAtlas::create(int size) -> core::Result<GlyphAtlas> {
    if (size <= 0) {
        return core::make_error(core::Code::InvalidArgument, "glyph atlas of zero size");
    }
    GlyphAtlas atlas{size};
    GLuint texture = 0;
    glCreateTextures(GL_TEXTURE_2D, 1, &texture);
    atlas.texture_ = gl::Texture{texture};
    glTextureStorage2D(texture, 1, GL_R8, size, size);
    const std::uint8_t zero = 0;
    glClearTexImage(texture, 0, GL_RED, GL_UNSIGNED_BYTE, &zero);
    // Glyph quads sit on whole physical pixels: nearest sampling reproduces the bitmap exactly.
    glTextureParameteri(texture, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTextureParameteri(texture, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTextureParameteri(texture, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(texture, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return atlas;
}

void GlyphAtlas::clear() {
    entries_.clear();
    failed_.clear();
    sets_.fill(0);
    packer_.reset();
    const std::uint8_t zero = 0;
    glClearTexImage(texture_.get(), 0, GL_RED, GL_UNSIGNED_BYTE, &zero);
}

auto GlyphAtlas::get(FontSet& fonts, FontIndex font, float px, std::uint32_t glyph)
    -> const Entry* {
    std::size_t set = 0;
    while (set < sets_.size() && sets_[set] != 0 && sets_[set] != fonts.id()) {
        ++set;
    }
    if (set == sets_.size()) { // sets come and go (DPI or font changes): start over
        WA_WARN("text", "glyph atlas: more than {} font sets over time, clearing", kMaxFontSets);
        clear();
        set = 0;
    }
    sets_[set] = fonts.id();
    // set 4 bits | font 8 bits | size in 1/64 px 20 bits | glyph 32 bits
    const auto size26 = static_cast<std::uint64_t>(px * 64.0f) & 0xFFFFF;
    const std::uint64_t key = (static_cast<std::uint64_t>(set) << 60) |
                              (static_cast<std::uint64_t>(font) << 52) | (size26 << 32) |
                              static_cast<std::uint64_t>(glyph);
    if (auto it = entries_.find(key); it != entries_.end()) {
        if (it->second.shelf >= 0) {
            packer_.touch(it->second.shelf, frame_);
        }
        return &it->second;
    }
    if (std::ranges::find(failed_, key) != failed_.end()) {
        return nullptr; // already found no room this frame: no FreeType work, no log line
    }

    FT_GlyphSlot slot = fonts.render_glyph(font, px, glyph);
    ++stats_.rendered;
    if (slot == nullptr) {
        return nullptr;
    }
    const FT_Bitmap& bitmap = slot->bitmap;
    Entry entry;
    entry.left = static_cast<float>(slot->bitmap_left);
    entry.top = static_cast<float>(slot->bitmap_top);
    if (bitmap.width == 0 || bitmap.rows == 0 || bitmap.pixel_mode != FT_PIXEL_MODE_GRAY ||
        bitmap.pitch < 0) {
        return &entries_.insert_or_assign(key, entry).first->second; // blank: nothing to draw
    }

    const int w = static_cast<int>(bitmap.width);
    const int h = static_cast<int>(bitmap.rows);
    // One pixel of empty border keeps neighbours out of reach of the sampler.
    const auto packed = packer_.insert(w + 2, h + 2, frame_);
    if (!packed) {
        if (failed_.empty()) {
            WA_WARN("text", "glyph atlas full this frame: glyph {} at {} px not drawn", glyph, px);
        }
        failed_.push_back(key);
        return nullptr;
    }
    for (const int shelf : packer_.take_evicted()) {
        std::erase_if(entries_, [shelf](const auto& item) { return item.second.shelf == shelf; });
        ++stats_.evicted_shelves;
    }

    const auto padded_w = static_cast<std::size_t>(w + 2);
    upload_.assign(padded_w * static_cast<std::size_t>(h + 2), 0);
    for (int row = 0; row < h; ++row) {
        std::memcpy(upload_.data() + (static_cast<std::size_t>(row) + 1) * padded_w + 1,
                    bitmap.buffer + static_cast<std::ptrdiff_t>(row) * bitmap.pitch,
                    static_cast<std::size_t>(w));
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTextureSubImage2D(texture_.get(), 0, packed->x, packed->y, w + 2, h + 2, GL_RED,
                        GL_UNSIGNED_BYTE, upload_.data());
    ++stats_.rasterized;

    const float s = static_cast<float>(size_);
    entry.uv = {static_cast<float>(packed->x + 1) / s, static_cast<float>(packed->y + 1) / s,
                static_cast<float>(packed->x + 1 + w) / s, static_cast<float>(packed->y + 1 + h) / s};
    entry.w = static_cast<float>(w);
    entry.h = static_cast<float>(h);
    entry.shelf = packed->shelf;
    return &entries_.insert_or_assign(key, entry).first->second;
}

namespace {

// Each glyph is drawn from faces[glyph.face] (the last face if there are fewer).
void draw_range(render::Renderer& renderer, GlyphAtlas& atlas, std::span<FontSet* const> faces,
                const Shaped& shaped, std::uint32_t first, std::uint32_t end, float px, Point baseline,
                render::Color color, GlyphColor color_of, const void* context, float scale) {
    renderer.set_glyph_atlas(atlas.texture());
    float pen = baseline.x * scale;
    const float base = std::round(baseline.y * scale);
    for (std::uint32_t i = first; i < end; ++i) {
        const Glyph& g = shaped.glyphs[i];
        FontSet& fonts = *faces[std::min<std::size_t>(g.face, faces.size() - 1)];
        const auto* e = atlas.get(fonts, g.font, px, g.id);
        if (e != nullptr && e->shelf >= 0) {
            const float x = std::round(pen + g.x_offset) + e->left;
            const float y = base - std::round(g.y_offset) - e->top;
            const render::Color c =
                color_of != nullptr ? color_of(context, i, (pen - baseline.x * scale) / scale)
                                    : color;
            renderer.glyph({x / scale, y / scale, e->w / scale, e->h / scale}, e->uv, c);
        }
        pen += g.x_advance;
    }
}

} // namespace

void draw(render::Renderer& renderer, GlyphAtlas& atlas, FontSet& fonts, const Shaped& shaped,
          float px, Point baseline, render::Color color, float scale) {
    FontSet* const one[] = {&fonts};
    draw_range(renderer, atlas, one, shaped, 0, static_cast<std::uint32_t>(shaped.glyphs.size()), px,
               baseline, color, nullptr, nullptr, scale);
}

void draw(render::Renderer& renderer, GlyphAtlas& atlas, FontSet& fonts, const Shaped& shaped,
          const Line& line, float px, Point baseline, render::Color color, float scale) {
    FontSet* const one[] = {&fonts};
    draw_range(renderer, atlas, one, shaped, line.first_glyph, line.end_glyph, px, baseline, color,
               nullptr, nullptr, scale);
}

void draw(render::Renderer& renderer, GlyphAtlas& atlas, FontSet& fonts, const Shaped& shaped,
          std::uint32_t first, std::uint32_t end, float px, Point baseline, GlyphColor color,
          const void* context, float scale) {
    FontSet* const one[] = {&fonts};
    draw_range(renderer, atlas, one, shaped, first, end, px, baseline, {}, color, context, scale);
}

void draw_zoomed(render::Renderer& renderer, GlyphAtlas& atlas, FontSet& fonts, const Shaped& shaped,
                 const Line& line, float px, Point baseline, render::Color color, float scale, float zoom) {
    renderer.set_glyph_atlas(atlas.texture());
    float pen = 0; // physical pixels of the shaped size, from the baseline's start
    for (std::uint32_t i = line.first_glyph; i < line.end_glyph; ++i) {
        const Glyph& g = shaped.glyphs[i];
        const auto* e = atlas.get(fonts, g.font, px, g.id);
        if (e != nullptr && e->shelf >= 0) {
            const float x = baseline.x + (pen + g.x_offset + static_cast<float>(e->left)) * zoom / scale;
            const float y = baseline.y - (g.y_offset + static_cast<float>(e->top)) * zoom / scale;
            renderer.glyph({x, y, static_cast<float>(e->w) * zoom / scale, static_cast<float>(e->h) * zoom / scale}, e->uv, color);
        }
        pen += g.x_advance;
    }
}

void draw(render::Renderer& renderer, GlyphAtlas& atlas, std::span<FontSet* const> faces,
          const Shaped& shaped, std::uint32_t first, std::uint32_t end, float px, Point baseline,
          GlyphColor color, const void* context, float scale) {
    draw_range(renderer, atlas, faces, shaped, first, end, px, baseline, {}, color, context, scale);
}

} // namespace ui::text
