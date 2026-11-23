#include "markdown/markdown.hpp"

#include <md4c.h>

#include <algorithm>
#include <array>
#include <cctype>

namespace markdown {
namespace {

// md4c's events turned into flat blocks: list items with their depth, quotes and paragraphs
// by themselves, a table's cells as plain text.
class Builder {
public:
    explicit Builder(std::vector<Block>& out) : out_(&out) {}

    static auto enter_block(MD_BLOCKTYPE type, void* detail, void* self) -> int {
        static_cast<Builder*>(self)->enter(type, detail);
        return 0;
    }
    static auto leave_block(MD_BLOCKTYPE type, void* detail, void* self) -> int {
        static_cast<Builder*>(self)->leave(type, detail);
        return 0;
    }
    static auto enter_span(MD_SPANTYPE type, void* detail, void* self) -> int {
        static_cast<Builder*>(self)->span(type, detail, true);
        return 0;
    }
    static auto leave_span(MD_SPANTYPE type, void* detail, void* self) -> int {
        static_cast<Builder*>(self)->span(type, detail, false);
        return 0;
    }
    static auto text(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* self) -> int {
        static_cast<Builder*>(self)->add_text(type, {text, size});
        return 0;
    }

private:
    struct List {
        bool ordered = false;
        int next = 1;
    };

    void flush() {
        if (!open_) {
            return;
        }
        if (cur_.kind == Kind::Code) {
            while (!cur_.text.empty() && cur_.text.back() == '\n') {
                cur_.text.pop_back();
            }
        }
        out_->push_back(std::move(cur_));
        cur_ = {};
        open_ = false;
    }
    void start(Kind kind) {
        flush();
        cur_ = {};
        cur_.kind = kind;
        open_ = true;
    }

    void enter(MD_BLOCKTYPE type, void* detail) {
        switch (type) {
        case MD_BLOCK_QUOTE: ++quote_; break;
        case MD_BLOCK_UL:
            flush(); // an item's own text ends where its nested list begins
            lists_.push_back({false, 1});
            break;
        case MD_BLOCK_OL:
            flush();
            lists_.push_back({true, static_cast<int>(static_cast<MD_BLOCK_OL_DETAIL*>(detail)->start)});
            break;
        case MD_BLOCK_LI:
            start(Kind::ListItem);
            cur_.depth = std::max(0, static_cast<int>(lists_.size()) - 1);
            if (!lists_.empty()) {
                cur_.ordered = lists_.back().ordered;
                cur_.number = cur_.ordered ? lists_.back().next++ : 0;
            }
            break;
        case MD_BLOCK_P:
            if (open_ && cur_.kind == Kind::ListItem) { // a paragraph of a list item: its text
                if (!cur_.text.empty()) {
                    cur_.text += ' ';
                }
            } else {
                start(quote_ > 0 ? Kind::Quote : Kind::Paragraph);
            }
            break;
        case MD_BLOCK_H:
            start(Kind::Heading);
            cur_.level = static_cast<int>(static_cast<MD_BLOCK_H_DETAIL*>(detail)->level);
            break;
        case MD_BLOCK_CODE: start(Kind::Code); break;
        case MD_BLOCK_HR:
            start(Kind::Rule);
            flush();
            break;
        case MD_BLOCK_TABLE: start(Kind::Table); break;
        case MD_BLOCK_TR: cur_.cells.emplace_back(); break;
        case MD_BLOCK_TH:
        case MD_BLOCK_TD: {
            if (cur_.cells.empty()) {
                cur_.cells.emplace_back();
            }
            cur_.cells.back().emplace_back();
            if (type == MD_BLOCK_TH) {
                const auto a = static_cast<MD_BLOCK_TD_DETAIL*>(detail)->align;
                cur_.align.push_back(a == MD_ALIGN_RIGHT ? 'r' : a == MD_ALIGN_CENTER ? 'c' : 'l');
            }
            break;
        }
        default: break;
        }
    }

    void leave(MD_BLOCKTYPE type, void*) {
        switch (type) {
        case MD_BLOCK_QUOTE: quote_ = std::max(0, quote_ - 1); break;
        case MD_BLOCK_UL:
        case MD_BLOCK_OL:
            if (!lists_.empty()) {
                lists_.pop_back();
            }
            break;
        case MD_BLOCK_LI: flush(); break;
        case MD_BLOCK_P:
            if (!(open_ && cur_.kind == Kind::ListItem)) {
                flush();
            }
            break;
        case MD_BLOCK_H:
        case MD_BLOCK_CODE:
        case MD_BLOCK_TABLE: flush(); break;
        default: break;
        }
    }

    void span(MD_SPANTYPE type, void* detail, bool enter) {
        const int d = enter ? 1 : -1;
        switch (type) {
        case MD_SPAN_STRONG: bold_ += d; break;
        case MD_SPAN_EM: italic_ += d; break;
        case MD_SPAN_CODE: code_ += d; break;
        case MD_SPAN_A:
            link_ += d;
            if (enter) {
                const auto& href = static_cast<MD_SPAN_A_DETAIL*>(detail)->href;
                href_.assign(href.text, href.size);
            } else if (link_ == 0) {
                href_.clear();
            }
            break;
        default: break;
        }
    }

    void add_text(MD_TEXTTYPE type, std::string_view text) {
        if (!open_) {
            start(quote_ > 0 ? Kind::Quote : Kind::Paragraph);
        }
        std::string piece;
        switch (type) {
        case MD_TEXT_SOFTBR: piece = "\n"; break; // a chat's line break (GitHub's comments do the same)
        case MD_TEXT_BR: piece = "\n"; break;
        case MD_TEXT_NULLCHAR: piece = "\xEF\xBF\xBD"; break;
        case MD_TEXT_ENTITY: piece = entity(text); break;
        default: piece.assign(text); break;
        }
        if (cur_.kind == Kind::Table) {
            if (!cur_.cells.empty() && !cur_.cells.back().empty()) {
                cur_.cells.back().back() += piece;
            }
            return;
        }
        const auto begin = static_cast<std::uint32_t>(cur_.text.size());
        cur_.text += piece;
        const auto end = static_cast<std::uint32_t>(cur_.text.size());
        if (cur_.kind == Kind::Code || (bold_ == 0 && italic_ == 0 && code_ == 0 && link_ == 0)) {
            return;
        }
        Inline in{.begin = begin, .end = end, .bold = bold_ > 0, .italic = italic_ > 0, .code = code_ > 0,
                  .link = link_ > 0, .href = link_ > 0 ? href_ : std::string{}};
        auto& list = cur_.inlines;
        if (!list.empty() && list.back().end == begin && list.back().bold == in.bold && list.back().italic == in.italic &&
            list.back().code == in.code && list.back().link == in.link && list.back().href == in.href) {
            list.back().end = end; // one range for a span's text that came in pieces
        } else {
            list.push_back(std::move(in));
        }
    }

    static auto entity(std::string_view e) -> std::string {
        static constexpr std::array<std::pair<std::string_view, std::string_view>, 6> kNamed{
            {{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&#39;", "'"}, {"&nbsp;", "\xC2\xA0"}}};
        for (const auto& [name, value] : kNamed) {
            if (e == name) {
                return std::string{value};
            }
        }
        return std::string{e};
    }

    std::vector<Block>* out_;
    Block cur_;
    bool open_ = false;
    std::vector<List> lists_;
    int quote_ = 0;
    int bold_ = 0;
    int italic_ = 0;
    int code_ = 0;
    int link_ = 0;
    std::string href_;

public:
    ~Builder() { flush(); }
    Builder(const Builder&) = delete;
    auto operator=(const Builder&) -> Builder& = delete;
};

auto parse(std::string_view text, std::vector<Block>& out) -> void {
    if (text.empty()) {
        return;
    }
    Builder builder{out};
    MD_PARSER parser{};
    parser.abi_version = 0;
    parser.flags = MD_DIALECT_GITHUB | MD_FLAG_NOHTML;
    parser.enter_block = &Builder::enter_block;
    parser.leave_block = &Builder::leave_block;
    parser.enter_span = &Builder::enter_span;
    parser.leave_span = &Builder::leave_span;
    parser.text = &Builder::text;
    (void)md_parse(text.data(), static_cast<MD_SIZE>(text.size()), &parser, &builder);
}

auto is_fence(std::string_view line) -> bool {
    std::size_t i = 0;
    while (i < line.size() && i < 3 && line[i] == ' ') {
        ++i;
    }
    return line.substr(i).starts_with("```") || line.substr(i).starts_with("~~~");
}

auto trimmed(std::string_view s) -> std::string_view {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

// "|---|:--:|" and the like: a table's delimiter row.
auto is_delimiter_row(std::string_view line) -> bool {
    line = trimmed(line);
    if (line.empty() || line.find('-') == std::string_view::npos) {
        return false;
    }
    return std::ranges::all_of(line, [](char c) { return c == '|' || c == '-' || c == ':' || c == ' ' || c == '\t'; });
}

// Where the tail's complete blocks end: after the last blank line outside a fence whose next
// line starts at the margin (not a list item's continuation), or 0.
auto stable_cut(std::string_view tail) -> std::size_t {
    std::size_t cut = 0;
    bool fence = false;
    bool blank_before = false;
    std::size_t pos = 0;
    while (pos < tail.size()) {
        const auto nl = tail.find('\n', pos);
        if (nl == std::string_view::npos) {
            break; // the last line is not complete: never a cut after it
        }
        const auto line = tail.substr(pos, nl - pos);
        const bool blank = trimmed(line).empty();
        if (!fence && blank_before && !blank && line.front() != ' ' && line.front() != '\t') {
            cut = pos;
        }
        if (is_fence(line)) {
            fence = !fence;
        }
        blank_before = blank && !fence;
        pos = nl + 1;
    }
    // A line still coming after a blank one begins a new block: the blocks before it are complete.
    if (pos < tail.size() && !fence && blank_before && tail[pos] != ' ' && tail[pos] != '\t') {
        cut = pos;
    }
    return cut;
}

// The provisional tail without its markup that is not closed yet.
auto visible(std::string_view tail) -> std::string_view {
    // Lines, with where each starts.
    std::vector<std::pair<std::size_t, std::string_view>> lines;
    for (std::size_t pos = 0; pos < tail.size();) {
        const auto nl = tail.find('\n', pos);
        const auto end = nl == std::string_view::npos ? tail.size() : nl;
        lines.emplace_back(pos, tail.substr(pos, end - pos));
        pos = nl == std::string_view::npos ? tail.size() : nl + 1;
    }
    if (lines.empty()) {
        return tail;
    }
    const bool complete_last = tail.back() == '\n';
    // The trailing lines that start with '|': a table that may not be complete.
    std::size_t first_row = lines.size();
    while (first_row > 0 && trimmed(lines[first_row - 1].second).starts_with('|')) {
        --first_row;
    }
    if (first_row < lines.size()) {
        // A line still coming is half a row (or half the delimiter row): not shown.
        const std::size_t rows = lines.size() - first_row - (complete_last ? 0 : 1);
        if (rows < 2 || !is_delimiter_row(lines[first_row + 1].second)) {
            return tail.substr(0, lines[first_row].first); // no delimiter row yet: not a table yet
        }
        return complete_last ? tail : tail.substr(0, lines.back().first);
    }
    if (complete_last) {
        return tail;
    }
    // The last line is still coming.
    const auto [start, last] = lines.back();
    const auto t = trimmed(last);
    if (!t.empty() && std::ranges::all_of(t, [](char c) { return c == '-' || c == '=' || c == ' '; })) {
        return tail.substr(0, start); // it may underline the line above
    }
    if (!t.empty() && t.size() < 3 && std::ranges::all_of(t, [](char c) { return c == '`' || c == '~'; })) {
        return tail.substr(0, start); // a fence being typed
    }
    std::size_t keep = last.size();
    const auto cut_at = [&](std::size_t at) { keep = std::min(keep, at); };
    // An unclosed "**", then a single '*' or '_', then '`'.
    std::size_t doubles = 0;
    std::size_t last_double = std::string_view::npos;
    for (std::size_t i = 0; i + 1 < last.size(); ++i) {
        if (last[i] == '*' && last[i + 1] == '*') {
            ++doubles;
            last_double = i;
            ++i;
        }
    }
    if (doubles % 2 == 1) {
        cut_at(last_double);
    }
    for (const char mark : {'*', '_', '`'}) {
        std::size_t count = 0;
        std::size_t last_mark = std::string_view::npos;
        for (std::size_t i = 0; i < keep; ++i) {
            if (last[i] != mark) {
                continue;
            }
            if (mark == '*' && i + 1 < last.size() && last[i + 1] == '*') {
                ++i; // a "**" pair, counted above
                continue;
            }
            if (mark == '_' && i > 0 && std::isalnum(static_cast<unsigned char>(last[i - 1])) != 0) {
                continue; // snake_case is not emphasis
            }
            ++count;
            last_mark = i;
        }
        if (count % 2 == 1) {
            cut_at(last_mark);
        }
    }
    // A link not complete: "[text](http…" without its ')'.
    if (const auto open = last.rfind('['); open != std::string_view::npos && open < keep) {
        const auto close = last.find(')', open);
        if (close == std::string_view::npos) {
            cut_at(open);
        }
    }
    return tail.substr(0, start + keep);
}

// " word" or " a few words" — or, after a text that ended with its space, "word" or "a few words" —
// perhaps with one space after: each word after one space, nothing md4c could make markup of — no
// line break, emphasis, code, link, autolink (":", "@", "/", "www."), entity, HTML, escape, strike,
// heading, table or a space too many.
auto plain_words(std::string_view delta, bool after_space) noexcept -> bool {
    if (delta.empty() || (delta.front() == ' ') == after_space || (!after_space && delta.size() < 2)) {
        return false;
    }
    if (delta.size() >= 2 && delta[delta.size() - 1] == ' ' && delta[delta.size() - 2] == ' ') {
        return false;
    }
    char before = after_space ? ' ' : '\0';
    for (const char c : delta) {
        if (c == ' ' && before == ' ') {
            return false;
        }
        switch (c) {
        case '\n': case '\r': case '\t': case '*': case '_': case '`': case '[': case ']': case '<': case '>': case '&':
        case '\\': case '~': case '#': case '|': case ':': case '@': case '/':
            return false;
        default: break;
        }
        before = c;
    }
    for (std::size_t i = 0; i + 4 <= delta.size(); ++i) { // www. in any case
        if ((delta[i] | 0x20) == 'w' && (delta[i + 1] | 0x20) == 'w' && (delta[i + 2] | 0x20) == 'w' && delta[i + 3] == '.') {
            return false;
        }
    }
    return true;
}

// The text's last line begins with a letter: not with a list's or a quote's mark still empty (md4c
// keeps "1.", "+", ">" right under a paragraph in it until the mark's first word comes), not with
// a space (a line still coming after a soft break), not with what may be a heading's underline.
auto last_line_is_words(std::string_view text) noexcept -> bool {
    const auto nl = text.rfind('\n');
    const auto line = nl == std::string_view::npos ? text : text.substr(nl + 1);
    if (line.empty()) {
        return false;
    }
    const auto c = static_cast<unsigned char>(line.front());
    return c >= 0x80 || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

} // namespace

void Document::sync(std::string_view text, bool finished) {
    if (!text.starts_with(text_)) { // not a continuation: from the start
        text_.clear();
        stable_ = 0;
        stable_blocks_ = 0;
        blocks_.clear();
        whole_ = false;
    } else if (const bool after_space = !text_.empty() && text_.back() == ' ';
               !finished && whole_ && text.size() > text_.size() && blocks_.size() > stable_blocks_ &&
               blocks_.back().kind == Kind::Paragraph && last_line_is_words(text_) && text_.back() != '\r' && text_.back() != '\t' &&
               !(after_space && text_.size() >= 2 && text_[text_.size() - 2] == ' ') &&
               plain_words(text.substr(text_.size()), after_space)) {
        // Plain words after the last paragraph, all of it shown: they are its text, as a parse of
        // the whole would make them — no md4c run (the motion review's Important 6: a printing
        // answer allocated every frame). A space at the end is not the paragraph's (md4c drops it).
        auto delta = text.substr(text_.size());
        text_.append(delta);
        if (delta.back() == ' ') {
            delta.remove_suffix(1);
        }
        if (after_space) {
            blocks_.back().text.push_back(' ');
        }
        blocks_.back().text.append(delta);
        return;
    }
    text_.assign(text);
    blocks_.resize(stable_blocks_);
    const std::string_view tail = std::string_view{text_}.substr(stable_);
    if (const auto cut = stable_cut(tail); cut > 0) {
        parse(tail.substr(0, cut), blocks_);
        parsed_bytes_ += cut;
        stable_ += cut;
        stable_blocks_ = blocks_.size();
    }
    const auto tail_now = std::string_view{text_}.substr(stable_);
    const auto rest = finished ? tail_now : visible(tail_now); // nothing more will close it
    parse(rest, blocks_);
    parsed_bytes_ += rest.size();
    whole_ = !finished && rest.size() == tail_now.size();
}

auto to_panel(const Block& table) -> bool {
    if (table.kind != Kind::Table || table.cells.empty()) {
        return false;
    }
    std::size_t cols = 0;
    for (const auto& row : table.cells) {
        cols = std::max(cols, row.size());
    }
    return table.cells.size() - 1 > 5 || cols > 3;
}

} // namespace markdown
