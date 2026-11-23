#include "app/answer.hpp"

#include "ui/theme.hpp"

#include <algorithm>
#include <array>

namespace app::answer {
namespace {

using markdown::Kind;

auto face_of(const markdown::Inline& in) noexcept -> std::uint8_t {
    if (in.link) {
        return kLink;
    }
    if (in.code) {
        return kCode;
    }
    if (in.bold && in.italic) {
        return kBoldItalic;
    }
    return in.bold ? kBold : in.italic ? kItalic : kRegular;
}

// Appends the block's text from byte `from` on, each piece in its face.
void append_runs(ui::text::Paragraph& p, const markdown::Block& b, std::size_t from) {
    const std::string_view text = b.text;
    std::size_t at = from;
    for (const auto& in : b.inlines) {
        if (in.end <= at) {
            continue;
        }
        if (in.begin > at) {
            p.append(text.substr(at, in.begin - at), kRegular);
            at = in.begin;
        }
        p.append(text.substr(at, in.end - at), face_of(in));
        at = in.end;
    }
    if (at < text.size()) {
        p.append(text.substr(at), kRegular);
    }
}

// `b` is `a` grown: the same kind of block, its text continued, the styles of a's text unchanged.
auto grown(const markdown::Block& a, const markdown::Block& b) -> bool {
    if (a.kind != b.kind || a.level != b.level || a.depth != b.depth || a.ordered != b.ordered ||
        a.number != b.number || a.kind == Kind::Table || a.kind == Kind::Rule || !b.text.starts_with(a.text)) {
        return false;
    }
    const auto n = static_cast<std::uint32_t>(a.text.size());
    std::size_t j = 0;
    for (const auto& in : b.inlines) {
        if (in.begin >= n) {
            break;
        }
        if (j >= a.inlines.size()) {
            return false;
        }
        const auto& old = a.inlines[j++];
        if (old.begin != in.begin || old.end != std::min(in.end, n) || face_of(old) != face_of(in) || old.href != in.href) {
            return false;
        }
    }
    return j == a.inlines.size();
}

// "158 GB", "-3", "$12", "64%": a cell that starts with a number.
auto numeric(std::string_view s) noexcept -> bool {
    std::size_t i = 0;
    while (i < s.size() && (s[i] == '+' || s[i] == '-' || s[i] == '$' || s[i] == '~')) {
        ++i;
    }
    return i < s.size() && s[i] >= '0' && s[i] <= '9';
}

auto links_of(const markdown::Block& b) -> std::vector<Link> {
    std::vector<Link> links;
    for (const auto& in : b.inlines) {
        if (!in.link) {
            continue;
        }
        if (!links.empty() && links.back().end == in.begin && links.back().href == in.href) {
            links.back().end = in.end; // a bold word inside the link
        } else {
            links.push_back({in.begin, in.end, in.href});
        }
    }
    return links;
}

} // namespace

void Doc::reset() {
    seen_.clear();
    finished_ = false;
    md_ = {};
    blocks_.clear();
}

auto Doc::size_for(Kind kind, int level) const noexcept -> float {
    if (kind == Kind::Heading) {
        return size_ * (level <= 1 ? 18.0f : level == 2 ? 16.0f : 14.5f) / 14.5f;
    }
    return kind == Kind::Code ? size_ * 12.5f / 14.5f : size_;
}

auto Doc::size_of(const Block& b) const noexcept -> float {
    return size_for(b.kind, b.level);
}

auto Doc::line_of(const Block& b) noexcept -> float {
    switch (b.kind) {
    case Kind::Heading: return 1.3f;
    case Kind::Code: return 1.55f;
    default: return ui::theme::type::prose_line;
    }
}

auto Doc::indent(const Block& b) noexcept -> float {
    if (b.kind == Kind::ListItem) {
        return static_cast<float>(b.depth) * kNestIndent + (b.ordered ? kNumberIndent : kItemIndent);
    }
    return b.kind == Kind::Quote ? kQuoteIndent : 0.0f;
}

auto Doc::height(const Block& b) const noexcept -> float {
    switch (b.kind) {
    case Kind::Rule: return 1.0f;
    case Kind::Table:
        if (b.panel) {
            return kPanelCard;
        }
        return b.cells.empty() ? 0.0f : kHeaderRow + kRow * static_cast<float>(b.cells.size() - 1) + 2;
    default: break;
    }
    const float lines = static_cast<float>(b.paragraph.lines().size()) * size_of(b) * line_of(b);
    return b.kind == Kind::Code ? lines + 2 * kCodePadY + 2 : lines;
}

auto Doc::gap(const Block& above, const Block& below) noexcept -> float {
    if (above.kind == Kind::ListItem && below.kind == Kind::ListItem) {
        return 3.0f;
    }
    if (below.kind == Kind::Heading) {
        return 16.0f;
    }
    return above.kind == Kind::Heading ? 6.0f : 10.0f;
}

auto Doc::height() const noexcept -> float {
    float h = 0;
    for (std::size_t i = 0; i < blocks_.size(); ++i) {
        h += (i > 0 ? gap(blocks_[i - 1], blocks_[i]) : 0.0f) + height(blocks_[i]);
    }
    return h;
}

void Doc::apply_width(Block& b) const {
    const float w = b.kind == Kind::Code ? code_width_ - 2 * kCodePadX - 2 : width_ - indent(b);
    b.paragraph.set_width((w > 1.0f ? w : 1.0f) * scale_);
}

void Doc::set_width(float width, float code_width) {
    if (width == width_ && code_width == code_width_) {
        return;
    }
    width_ = width;
    code_width_ = code_width;
    for (auto& b : blocks_) {
        apply_width(b);
    }
}

auto Doc::faces_for(const markdown::Block& source) const -> Faces {
    bool italic = false;
    bool bold_italic = false;
    for (const auto& in : source.inlines) {
        italic = italic || (in.italic && !in.bold);
        bold_italic = bold_italic || (in.italic && in.bold);
    }
    auto& f = *fonts_;
    // Italics load the first time a block has them.
    Faces faces{};
    if (source.kind == Kind::Code) {
        faces.fill(&f.mono);
    } else if (source.kind == Kind::Heading) {
        auto* slanted = italic || bold_italic ? &f.get(ui::Face::SemiboldItalic) : &f.display;
        faces = {&f.display, &f.display, slanted, slanted, &f.mono, &f.display};
    } else {
        faces = {&f.text, &f.semibold, italic ? &f.get(ui::Face::Italic) : &f.text,
                 bold_italic ? &f.get(ui::Face::SemiboldItalic) : &f.semibold, &f.mono, &f.text};
    }
    return faces;
}

auto Doc::make(const markdown::Block& source) -> Block {
    const auto faces = faces_for(source);
    Block b{.kind = source.kind,
            .level = source.level,
            .depth = source.depth,
            .ordered = source.ordered,
            .number = source.number,
            .paragraph = ui::text::Paragraph{faces, size_for(source.kind, source.level) * scale_},
            .links = links_of(source),
            .cells = source.cells,
            .align = source.align,
            .panel = !whole_tables_ && source.kind == Kind::Table && markdown::to_panel(source),
            .source = source};
    if (b.kind == Kind::Table) {
        // A column of numbers is set right, whatever its delimiter row said.
        for (std::size_t c = 0; c < b.align.size(); ++c) {
            bool numbers = b.cells.size() > 1;
            for (std::size_t r = 1; r < b.cells.size() && numbers; ++r) {
                numbers = c >= b.cells[r].size() || b.cells[r][c].empty() || numeric(b.cells[r][c]);
            }
            if (numbers && b.align[c] == 'l') {
                b.align[c] = 'r';
            }
        }
    } else if (b.kind != Kind::Rule) {
        append_runs(b.paragraph, source, 0);
    }
    apply_width(b);
    return b;
}

void Doc::sync(std::string_view answer, bool finished) {
    if (answer == seen_ && finished == finished_) {
        return; // a steady frame
    }
    if (!answer.starts_with(seen_)) {
        reset(); // not a continuation: a new answer
    }
    seen_.assign(answer);
    finished_ = finished;
    md_.sync(answer, finished);
    const auto src = md_.blocks();
    if (blocks_.size() > src.size()) {
        blocks_.erase(blocks_.begin() + static_cast<std::ptrdiff_t>(src.size()), blocks_.end());
    }
    for (std::size_t i = 0; i < src.size(); ++i) {
        if (i >= blocks_.size()) {
            blocks_.push_back(make(src[i]));
            continue;
        }
        auto& b = blocks_[i];
        if (b.source == src[i]) {
            continue;
        }
        // Grown, and its faces still do: italics that came only now need a paragraph with them.
        if (grown(b.source, src[i]) && std::ranges::equal(b.paragraph.faces(), faces_for(src[i]))) {
            append_runs(b.paragraph, src[i], b.source.text.size());
            b.links = links_of(src[i]);
            b.source = src[i];
        } else {
            b = make(src[i]);
        }
    }
}

} // namespace app::answer
