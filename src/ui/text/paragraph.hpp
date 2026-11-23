#pragma once

// ui::text::Paragraph — a block of text that grows at the end (a streamed answer). Appending
// re-shapes only from the start of the last line; a new width re-wraps without re-shaping.
// Text can come in several faces (up to kMaxFaces: regular, bold, italic, code…): each appended
// piece names its face, and the glyphs carry it (Glyph::face) for drawing with faces().

#include "ui/text/shape.hpp"
#include "ui/text/wrap.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ui::text {

class Paragraph {
public:
    static constexpr std::size_t kMaxFaces = 6;

    Paragraph(FontSet& fonts, float px) : faces_{&fonts, &fonts}, face_count_(2), px_(px) {}
    Paragraph(FontSet& regular, FontSet& bold, float px) : faces_{&regular, &bold}, face_count_(2), px_(px) {}
    // Faces by index (at most kMaxFaces; the rest are ignored).
    Paragraph(std::span<FontSet* const> faces, float px);

    // An incomplete UTF-8 sequence at the end (a token boundary inside a character) is held
    // back until the rest arrives; text() does not include it.
    void set_text(std::string_view text);
    void append(std::string_view text, std::uint8_t face = 0);
    void set_width(float max_width);

    [[nodiscard]] auto text() const noexcept -> std::string_view { return text_; }
    [[nodiscard]] auto fonts() const noexcept -> FontSet& { return *faces_[0]; }
    [[nodiscard]] auto faces() const noexcept -> std::span<FontSet* const> { return {faces_.data(), face_count_}; }
    [[nodiscard]] auto px() const noexcept -> float { return px_; }
    [[nodiscard]] auto shaped() const noexcept -> const Shaped& { return shaped_; }
    [[nodiscard]] auto lines() const noexcept -> const std::vector<Line>& { return lines_; }
    // Bytes passed to the shaper so far: what the streaming tests and the profiler watch.
    [[nodiscard]] auto shaped_bytes() const noexcept -> std::uint64_t { return shaped_bytes_; }

private:
    struct Run {
        std::uint32_t start; // byte offset where this face begins
        std::uint8_t face;
    };
    void take_complete(std::string_view text, std::string& out);
    // Shapes text_[from, end of text) run by run onto shaped_.
    void shape_from(std::size_t from);

    std::array<FontSet*, kMaxFaces> faces_{};
    std::size_t face_count_ = 0;
    float px_;
    float width_ = std::numeric_limits<float>::infinity();
    std::string text_;
    std::string pending_; // the start of a character whose remaining bytes have not arrived
    std::vector<Run> runs_;
    Shaped shaped_;
    std::vector<Line> lines_;
    std::uint64_t shaped_bytes_ = 0;
};

} // namespace ui::text
