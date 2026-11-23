#pragma once

// ui::text::GlyphAtlas — rasterised glyphs in one R8 texture (2048x2048 by default), packed in
// shelves. When it is full, the least recently used shelf that no glyph of the current frame
// sits on is cleared and reused. ui::text::draw puts shaped text on the renderer.

#include "core/error.hpp"
#include "ui/gl/objects.hpp"
#include "ui/render/renderer.hpp"
#include "ui/text/fonts.hpp"
#include "ui/text/shape.hpp"
#include "ui/text/wrap.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace ui::text {

// Shelf packing with LRU eviction of whole shelves; pure CPU bookkeeping.
class ShelfPacker {
public:
    struct Slot {
        int x = 0, y = 0, w = 0, h = 0;
        int shelf = 0;
    };

    ShelfPacker(int width, int height) : width_(width), height_(height) {}

    // Marks the shelf used in `frame`. A slot in a shelf used in the current frame is never
    // evicted, so slots handed out during a frame stay valid until it ends.
    auto insert(int w, int h, std::uint64_t frame) -> std::optional<Slot>;
    void touch(int shelf, std::uint64_t frame);
    // Shelves cleared for reuse since the last call: their old slots are gone.
    auto take_evicted() -> std::vector<int>;
    void reset();
    // Share of the height given to shelves, 0..1 (the debug window's "atlas fill").
    [[nodiscard]] auto fill() const noexcept -> double {
        return height_ > 0 ? static_cast<double>(next_y_) / static_cast<double>(height_) : 0.0;
    }

private:
    struct Shelf {
        int y = 0;
        int height = 0;
        int used = 0; // width taken from the left
        std::uint64_t last_used = 0;
    };
    std::vector<Shelf> shelves_;
    std::vector<int> evicted_;
    int width_;
    int height_;
    int next_y_ = 0;
};

class GlyphAtlas {
public:
    struct Entry {
        render::Uv uv;
        float w = 0, h = 0;       // bitmap size, pixels
        float left = 0, top = 0;  // bitmap offset from the pen position (top is up)
        int shelf = -1;           // -1: blank glyph (a space), nothing to draw
    };
    struct Stats {
        std::uint64_t rendered = 0;   // FreeType renders
        std::uint64_t rasterized = 0; // glyphs uploaded into the atlas
        std::uint64_t evicted_shelves = 0;
    };

    static constexpr int kMaxFontSets = 16;

    static auto create(int size = 2048) -> core::Result<GlyphAtlas>;

    // Rasterises on the first request. Null when the glyph cannot be rendered or the atlas has
    // no room left this frame. Valid until a later frame evicts its shelf.
    // Up to kMaxFontSets font sets share the atlas, known by FontSet::id(); a further set
    // clears the atlas and starts over (a font set replaced by a new one is a new set).
    auto get(FontSet& fonts, FontIndex font, float px, std::uint32_t glyph) -> const Entry*;
    void next_frame() noexcept {
        ++frame_;
        failed_.clear(); // glyphs that found no room may fit after this frame's shelves age
    }

    [[nodiscard]] auto texture() const noexcept -> GLuint { return texture_.get(); }
    [[nodiscard]] auto stats() const noexcept -> const Stats& { return stats_; }
    [[nodiscard]] auto fill() const noexcept -> double { return packer_.fill(); }

private:
    explicit GlyphAtlas(int size) : size_(size), packer_(size, size) {}

    void clear();

    std::array<std::uint64_t, kMaxFontSets> sets_{}; // FontSet ids, 0 = free
    std::vector<std::uint64_t> failed_;               // keys with no room this frame
    int size_;
    gl::Texture texture_;
    ShelfPacker packer_;
    std::unordered_map<std::uint64_t, Entry> entries_;
    std::vector<std::uint8_t> upload_; // padded bitmap staging, reused
    std::uint64_t frame_ = 1;
    Stats stats_;
};

struct Point {
    float x = 0, y = 0;
};

// Draws shaped glyphs with the pen starting at `baseline` (logical pixels). `px` is the size
// the text was shaped at, in physical pixels (= logical size x scale). Glyphs are snapped to
// physical pixels so they stay crisp.
void draw(render::Renderer& renderer, GlyphAtlas& atlas, FontSet& fonts, const Shaped& shaped,
          float px, Point baseline, render::Color color, float scale = 1.0f);
void draw(render::Renderer& renderer, GlyphAtlas& atlas, FontSet& fonts, const Shaped& shaped,
          const Line& line, float px, Point baseline, render::Color color, float scale = 1.0f);

// A line drawn `zoom` times its shaped size from `baseline`, its glyphs as rasterized (nothing shaped
// or rasterized again, nothing snapped): text in flight between two sizes (MOTION_SPEC scene 05).
void draw_zoomed(render::Renderer& renderer, GlyphAtlas& atlas, FontSet& fonts, const Shaped& shaped,
                 const Line& line, float px, Point baseline, render::Color color, float scale, float zoom);

// Colour per glyph: `glyph` indexes shaped.glyphs, `x` is the pen offset from the start of the
// range in logical pixels (shimmer gradients, tokens fading in).
using GlyphColor = auto (*)(const void* context, std::uint32_t glyph, float x) -> render::Color;
void draw(render::Renderer& renderer, GlyphAtlas& atlas, FontSet& fonts, const Shaped& shaped,
          std::uint32_t first, std::uint32_t end, float px, Point baseline, GlyphColor color,
          const void* context, float scale = 1.0f);
// The same for text in several faces (a Paragraph's faces()): glyph.face picks the set.
void draw(render::Renderer& renderer, GlyphAtlas& atlas, std::span<FontSet* const> faces,
          const Shaped& shaped, std::uint32_t first, std::uint32_t end, float px, Point baseline,
          GlyphColor color, const void* context, float scale = 1.0f);

} // namespace ui::text
