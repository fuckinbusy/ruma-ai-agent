#pragma once

// ui::text::FontSet — a fallback chain of fonts: the first font that has a character draws it.
// Files are memory-mapped (msyh.ttc is 20 MB; only touched pages are read). HarfBuzz shapes with
// its own OpenType tables (hb-ot), FreeType rasterises; both see the same variation axes, so
// glyph ids and metrics agree. One HarfBuzz font and one FT_Size per (font, pixel size).

#include "core/error.hpp"
#include "ui/font_library.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct hb_font_t;
struct hb_buffer_t;

namespace ui::text {

using FontIndex = std::uint8_t; // position in the chain

// A font at one pixel size. Metrics in pixels; descent is positive (below the baseline).
struct SizedFont {
    hb_font_t* hb = nullptr;
    FT_Size ft_size = nullptr;
    float px = 0;
    float ascent = 0;
    float descent = 0;
    float line_height = 0;
};

class FontSet {
public:
    struct Source {
        std::filesystem::path file;
        int face_index = 0;          // face inside a .ttc collection
        float weight = 0;            // 'wght' axis of a variable font; 0 keeps the default
        float optical_size = 0;      // 'opsz' axis; 0 keeps the default
    };

    FontSet();
    // Fonts that fail to open are skipped with a warning; an empty chain is an error. The set
    // keeps the library alive (either may be destroyed first). One thread uses a set.
    static auto create(FontLibrary& library, std::span<const Source> sources)
        -> core::Result<FontSet>;
    // The mockups' UI font, Segoe UI Variable Text (Windows 11), then Segoe UI, Segoe UI
    // Emoji, Segoe UI Symbol and Microsoft YaHei from %WINDIR%\Fonts.
    // weight/optical_size are the variable font's axes: 400/10.5 is "Segoe UI Variable Text",
    // optical size 36 is "... Display" (the mockups' --font-d).
    static auto system_ui(FontLibrary& library, float weight = 400.0f,
                          float optical_size = 10.5f) -> core::Result<FontSet>;
    // Segoe Fluent Icons, the Windows 11 system icon font (glyphs by code point, U+E700...).
    static auto system_icons(FontLibrary& library) -> core::Result<FontSet>;
    // --mono: Cascadia Mono where Windows has it, else Consolas, with the UI fonts after them.
    static auto system_mono(FontLibrary& library) -> core::Result<FontSet>;
    // Segoe UI Italic (or Semibold Italic) with the UI chain's fallbacks: Markdown's emphasis.
    static auto system_italic(FontLibrary& library, bool semibold) -> core::Result<FontSet>;
    ~FontSet();
    FontSet(FontSet&& other) noexcept;
    FontSet& operator=(FontSet&& other) noexcept;
    FontSet(const FontSet&) = delete;
    FontSet& operator=(const FontSet&) = delete;

    [[nodiscard]] auto size() const noexcept -> FontIndex;
    [[nodiscard]] auto family(FontIndex font) const -> std::string_view;
    [[nodiscard]] auto has_glyph(FontIndex font, char32_t cp) const -> bool;
    // The first font in the chain that has `cp`; 0 when none has it (draws .notdef).
    [[nodiscard]] auto resolve(char32_t cp) const -> FontIndex;
    // Opens the size on first use; the reference stays valid for the FontSet's lifetime.
    auto sized(FontIndex font, float px) -> const SizedFont&;
    // Loads and renders a glyph (8-bit coverage, light hinting) into the face's glyph slot;
    // valid until the next call. Null on failure.
    auto render_glyph(FontIndex font, float px, std::uint32_t glyph) -> FT_GlyphSlot;

    // A reusable HarfBuzz buffer for the shaper (one FontSet is used by one thread).
    [[nodiscard]] auto scratch_buffer() noexcept -> hb_buffer_t*;
    // Unique per set content for the process lifetime (never reused like an address): caches
    // such as the glyph atlas key by it.
    [[nodiscard]] auto id() const noexcept -> std::uint64_t { return id_; }

private:
    struct Font;
    static auto open(FontLibrary& library, const Source& source)
        -> core::Result<std::unique_ptr<Font>>;
    std::vector<std::unique_ptr<Font>> fonts_;
    hb_buffer_t* buffer_ = nullptr;
    std::uint64_t id_;
};

} // namespace ui::text
