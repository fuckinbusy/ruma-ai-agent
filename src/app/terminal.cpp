#include "app/terminal.hpp"

#include <algorithm>
#include <array>

namespace app::terminal {
namespace {

auto is_space(char c) -> bool {
    return c == ' ' || c == '\t';
}

auto is_letter(char c) -> bool {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

auto is_number(std::string_view w) -> bool {
    return !w.empty() && std::ranges::all_of(w, [](char c) { return (c >= '0' && c <= '9') || c == '.' || c == ',' || c == '/'; }) &&
           w.front() >= '0' && w.front() <= '9';
}

// A path or a pattern: a backslash, a home, a wildcard, a dot first.
auto is_path(std::string_view w) -> bool {
    return w.find('\\') != std::string_view::npos || w.find('*') != std::string_view::npos || w.front() == '~' ||
           (w.front() == '.' && w.size() > 1 && w[1] != '.');
}

class Inker {
public:
    explicit Inker(std::string_view line) : line_(line) {}

    void add(std::size_t begin, std::size_t end, Ink ink) {
        if (end > begin) {
            out.push_back({static_cast<std::uint32_t>(begin), static_cast<std::uint32_t>(end), ink});
        }
    }
    // From `at`: a run of spaces, or a word (a quoted string whole); its end.
    auto next(std::size_t at) const -> std::size_t {
        if (is_space(line_[at])) {
            while (at < line_.size() && is_space(line_[at])) {
                ++at;
            }
            return at;
        }
        if (line_[at] == '"' || line_[at] == '\'') {
            const auto close = line_.find(line_[at], at + 1);
            return close == std::string_view::npos ? line_.size() : close + 1;
        }
        while (at < line_.size() && !is_space(line_[at])) {
            ++at;
        }
        return at;
    }

    std::vector<Token> out;

private:
    std::string_view line_;
};

} // namespace

auto tokenize(std::string_view line) -> std::vector<Token> {
    Inker k{line};
    if (line.empty()) {
        return {};
    }
    std::size_t at = 0;
    bool command = false; // a line the agent typed: after its prompt
    if (line.starts_with("PS ")) {
        if (const auto gt = line.find('>'); gt != std::string_view::npos) {
            k.add(0, gt + 1, Ink::Prompt);
            at = gt + 1;
            command = true;
        }
    }
    bool expect_command = command;
    bool first = true;
    // An output line's label (dim): its words up to "word:" (Count    : 4812, found so far: …).
    std::size_t label_end = 0;
    if (!command) {
        for (auto colon = line.find(':'); colon != std::string_view::npos; colon = line.find(':', colon + 1)) {
            if (colon + 1 == line.size() || is_space(line[colon + 1])) {
                label_end = colon + 1;
                break;
            }
        }
    }
    const auto separator = [](std::string_view w) {
        return w == "\xE2\x89\xA1" || w == "\xE2\x80\x94" || w == "\xE2\x86\x92" || w == "|" || w == "=";
    };
    while (at < line.size()) {
        const auto end = k.next(at);
        const std::string_view w = line.substr(at, end - at);
        if (is_space(w.front())) {
            k.add(at, end, Ink::Plain);
        } else if (w.front() == '#') {
            k.add(at, line.size(), Ink::Dim); // a comment: the rest of the line
            break;
        } else if (command) {
            if (w == "|" || w == "=" || w == ";") {
                k.add(at, end, Ink::Dim);
                expect_command = true;
            } else if (w.front() == '"' || w.front() == '\'') {
                k.add(at, end, Ink::String);
            } else if (w.front() == '-' || w.front() == '$') {
                k.add(at, end, Ink::Dim); // a parameter, an operator, a variable
            } else if (expect_command && is_letter(w.front())) {
                k.add(at, end, Ink::Command);
                expect_command = false;
            } else if (is_path(w)) {
                k.add(at, end, Ink::String);
            } else {
                k.add(at, end, Ink::Plain);
            }
        } else if (first && (w == "skip" || w == "warning" || w == "WARNING:" || w == "error" || w == "ERROR:")) {
            k.add(at, end, Ink::Warn);
        } else if (at < label_end || separator(w) || (first && label_end == 0 && !is_number(w))) {
            k.add(at, end, Ink::Dim); // the line's label, a separator
        } else if (is_number(w)) {
            k.add(at, end, Ink::Value);
        } else {
            k.add(at, end, Ink::Plain);
        }
        if (!is_space(w.front())) {
            first = false;
        }
        at = end;
    }
    return std::move(k.out);
}

void Buffer::append(std::string_view text, int step) {
    while (!text.empty()) {
        const auto nl = text.find('\n');
        std::string_view piece = text.substr(0, nl);
        if (!piece.empty() && piece.back() == '\r') {
            piece.remove_suffix(1);
        }
        if (open_ && !lines_.empty()) {
            lines_.back().text += piece;
        } else {
            lines_.push_back({std::string{piece}, {}, step});
            if (lines_.size() > kMaxLines) {
                lines_.pop_front(); // the oldest go
            }
        }
        lines_.back().tokens = tokenize(lines_.back().text);
        open_ = nl == std::string_view::npos;
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
    }
}

} // namespace app::terminal
