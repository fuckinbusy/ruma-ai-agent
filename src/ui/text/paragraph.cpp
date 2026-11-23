#include "ui/text/paragraph.hpp"

#include <algorithm>

namespace ui::text {

Paragraph::Paragraph(std::span<FontSet* const> faces, float px) : px_(px) {
    for (FontSet* f : faces) {
        if (face_count_ < kMaxFaces) {
            faces_[face_count_++] = f;
        }
    }
}

// Appends pending_ + text to `out` up to the last complete character; keeps the rest pending.
void Paragraph::take_complete(std::string_view text, std::string& out) {
    pending_.append(text);
    std::size_t end = pending_.size();
    // A lead byte in the last 3 bytes whose sequence runs past the end: hold it back.
    for (std::size_t back = 1; back <= 3 && back <= pending_.size(); ++back) {
        const auto c = static_cast<unsigned char>(pending_[pending_.size() - back]);
        if ((c & 0xC0) == 0x80) {
            continue; // continuation byte
        }
        const std::size_t length = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        if (length > back) {
            end = pending_.size() - back;
        }
        break;
    }
    out.append(pending_, 0, end);
    pending_.erase(0, end);
}

void Paragraph::shape_from(std::size_t from) {
    for (std::size_t i = 0; i < runs_.size(); ++i) {
        const std::size_t run_end = i + 1 < runs_.size() ? runs_[i + 1].start : text_.size();
        const std::size_t begin = std::max<std::size_t>(runs_[i].start, from);
        if (begin < run_end) {
            shape_append(*faces_[runs_[i].face], text_, begin, run_end, px_, shaped_,
                         runs_[i].face);
        }
    }
    shaped_bytes_ += text_.size() - from;
}

void Paragraph::set_text(std::string_view text) {
    text_.clear();
    pending_.clear();
    runs_.clear();
    shaped_ = {};
    lines_.clear();
    append(text, 0);
}

// Lines before the last one ended where a character already in the text overflowed, so new text
// cannot move their breaks: only the last line is shaped and wrapped again.
void Paragraph::append(std::string_view text, std::uint8_t face) {
    face = std::min<std::uint8_t>(face, static_cast<std::uint8_t>(face_count_ - 1)); // an unknown face: the last one
    const std::size_t before = text_.size();
    take_complete(text, text_);
    if (runs_.empty() || runs_.back().face != face) {
        if (!runs_.empty() && runs_.back().start == before) {
            runs_.back().face = face; // the previous run got no text: reuse it
        } else {
            runs_.push_back({static_cast<std::uint32_t>(before), face});
        }
    }
    if (lines_.empty()) { // the first text
        shaped_ = {};
        shape_from(0);
        wrap(shaped_, text_, width_, lines_);
        return;
    }
    if (text_.size() == before) {
        return; // nothing complete arrived
    }
    const Line last = lines_.back();
    lines_.pop_back();

    for (std::size_t i = last.first_glyph; i < shaped_.glyphs.size(); ++i) {
        shaped_.width -= shaped_.glyphs[i].x_advance;
    }
    shaped_.glyphs.resize(last.first_glyph);
    shape_from(last.first_byte);
    wrap_from(shaped_, text_, width_, {last.first_byte, last.first_glyph}, lines_);
}

void Paragraph::set_width(float max_width) {
    width_ = max_width;
    if (!lines_.empty()) {
        wrap(shaped_, text_, width_, lines_);
    }
}

} // namespace ui::text
