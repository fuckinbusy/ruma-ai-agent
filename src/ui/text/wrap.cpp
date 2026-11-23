#include "ui/text/wrap.hpp"

#include "core/utf8.hpp"

namespace ui::text {
namespace {

auto is_space(char32_t cp) noexcept -> bool {
    return cp == ' ' || cp == '\t' || cp == 0x3000;
}

auto is_cjk(char32_t cp) noexcept -> bool {
    return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFF00 && cp <= 0xFFEF) || (cp >= 0x20000 && cp <= 0x3FFFF);
}

// Closing punctuation never starts a line (kinsoku, minimal set), nor does a mark that belongs
// to the character before it (combining marks, variation selectors, ZWJ, skin tones).
auto no_break_before(char32_t cp) noexcept -> bool {
    if ((cp >= 0x0300 && cp <= 0x036F) || cp == 0x3099 || cp == 0x309A || cp == 0x200D ||
        (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0x1F3FB && cp <= 0x1F3FF) ||
        (cp >= 0xE0100 && cp <= 0xE01EF)) {
        return true;
    }
    switch (cp) {
    case 0x3001:
    case 0x3002:
    case 0xFF0C:
    case 0xFF0E:
    case 0xFF1A:
    case 0xFF1B:
    case 0xFF01:
    case 0xFF1F:
    case 0xFF09:
    case 0x300D:
    case 0x300F:
    case 0x3011:
    case 0x30FC:
        return true;
    default:
        return false;
    }
}

// Inside a word wider than the line (a path, a command): it breaks after one of these, or before
// a '-', rather than between any two characters.
auto separates(char32_t cp) noexcept -> bool {
    switch (cp) {
    case '\\':
    case '/':
    case '|':
    case ';':
    case ',':
    case '=':
    case '&':
        return true;
    default:
        return false;
    }
}

} // namespace

void wrap(const Shaped& shaped, std::string_view text, float max_width, std::vector<Line>& out) {
    out.clear();
    wrap_from(shaped, text, max_width, {}, out);
}

void wrap_from(const Shaped& shaped, std::string_view text, float max_width, LineStart start,
               std::vector<Line>& out) {
    const auto& glyphs = shaped.glyphs;
    const auto glyph_count = static_cast<std::uint32_t>(glyphs.size());

    LineStart line = start;       // where the current line starts
    LineStart content_end = line; // after its last non-hanging character
    float x = 0;                  // pen position, hanging spaces included
    float content_x = 0;          // width up to content_end
    bool has_break = false;       // a break opportunity on the current line
    LineStart break_end{}, break_next{};
    float break_end_x = 0, break_next_x = 0;
    bool space_since_content = false;
    char32_t previous = 0;
    bool switch_hyphen = false; // `previous` is a '-' that starts a word (a command's "-First"): no break after it
    // The last place inside the current word to break it if it is wider than the line.
    bool has_soft = false;
    LineStart soft_end{}, soft_next{};
    float soft_end_x = 0, soft_next_x = 0;

    const auto emit = [&](LineStart first, LineStart end, float width) {
        out.push_back({first.glyph, end.glyph, first.byte, end.byte, width});
    };
    const auto start_line = [&](LineStart at) {
        line = at;
        content_end = at;
        x = 0;
        content_x = 0;
        has_break = false;
        space_since_content = false;
        previous = 0;
        switch_hyphen = false;
        has_soft = false;
    };
    // Moves the text from `from` to the end of the content to a new line (it starts there).
    const auto move_to_new_line = [&](LineStart from, float from_x) {
        const LineStart moved_end = content_end;
        const float moved_x = content_x - from_x;
        const char32_t moved_previous = previous;
        const bool moved_switch = switch_hyphen;
        start_line(from);
        if (moved_end.byte > from.byte) {
            content_end = moved_end;
            content_x = moved_x;
            x = moved_x;
            previous = moved_previous;
            switch_hyphen = moved_switch;
        }
    };

    std::uint32_t gi = start.glyph;
    std::size_t pos = start.byte;
    while (pos < text.size()) {
        const auto d = core::utf8::decode(text, pos);
        const auto byte = static_cast<std::uint32_t>(pos);
        while (gi < glyph_count && glyphs[gi].cluster < byte) {
            ++gi; // glyphs of a cluster that began earlier (ligatures) were counted there
        }
        const LineStart here{byte, gi};
        float advance = 0;
        while (gi < glyph_count && glyphs[gi].cluster == byte) {
            advance += glyphs[gi].x_advance;
            ++gi;
        }
        const LineStart after{static_cast<std::uint32_t>(pos + d.length), gi};
        pos += d.length;

        if (d.cp == '\n') {
            emit(line, content_end, content_x);
            start_line(after);
            continue;
        }
        if (d.cp == '\r') {
            continue;
        }
        bool has_content = content_end.byte != line.byte;
        if (has_content && is_space(d.cp)) { // hangs if the line breaks here
            x += advance;
            space_since_content = true;
            previous = d.cp;
            switch_hyphen = false;
            continue;
        }
        const bool starts_word = !has_content || space_since_content;
        if (has_content) {
            const bool opportunity =
                !no_break_before(d.cp) &&
                (space_since_content || (previous == '-' && !switch_hyphen) || is_cjk(d.cp) || is_cjk(previous));
            if (opportunity) {
                has_break = true;
                break_end = content_end;
                break_end_x = content_x;
                break_next = here;
                break_next_x = x;
            } else if (!no_break_before(d.cp) && (separates(previous) || (d.cp == '-' && previous != '-'))) {
                has_soft = true;
                soft_end = content_end;
                soft_end_x = content_x;
                soft_next = here;
                soft_next_x = x;
            }
            if (x + advance > max_width && has_break) {
                emit(line, break_end, break_end_x);
                // Text between the break and `here` (the start of the current word, if the
                // break was earlier) moves to the new line; no spaces follow it, or the break
                // would have been at `here`. A separator inside it stays a place to break.
                const bool soft = has_soft && soft_next.byte > break_next.byte;
                const LineStart kept_end = soft_end, kept_next = soft_next;
                const float kept_end_x = soft_end_x - break_next_x, kept_next_x = soft_next_x - break_next_x;
                move_to_new_line(break_next, break_next_x);
                if (soft) {
                    has_soft = true;
                    soft_end = kept_end;
                    soft_next = kept_next;
                    soft_end_x = kept_end_x;
                    soft_next_x = kept_next_x;
                }
                has_content = content_end.byte != line.byte;
            }
            if (has_content && x + advance > max_width && has_soft) { // a path or a command: at its separator
                emit(line, soft_end, soft_end_x);
                move_to_new_line(soft_next, soft_next_x);
                has_content = content_end.byte != line.byte;
            }
            if (has_content && x + advance > max_width) { // one word wider than the line
                emit(line, content_end, content_x);
                start_line(here);
            }
        }
        x += advance;
        content_x = x;
        content_end = after;
        space_since_content = false;
        previous = d.cp;
        switch_hyphen = d.cp == '-' && starts_word;
    }
    emit(line, content_end, content_x);
}

} // namespace ui::text
