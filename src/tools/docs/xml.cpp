#include "tools/docs/docs.hpp"

#include <charconv>

namespace tools::docs {
namespace {

auto local(std::string_view name) -> std::string_view {
    const auto colon = name.find(':');
    return colon == std::string_view::npos ? name : name.substr(colon + 1);
}

void put_utf8(std::string& out, std::uint32_t c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x110000) {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

// `raw` with its entities resolved; false on one it does not know.
auto resolved(std::string_view raw, std::string& out) -> bool {
    out.clear();
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] != '&') {
            out += raw[i];
            continue;
        }
        const auto semi = raw.find(';', i);
        if (semi == std::string_view::npos) {
            return false;
        }
        const auto name = raw.substr(i + 1, semi - i - 1);
        if (name == "amp") out += '&';
        else if (name == "lt") out += '<';
        else if (name == "gt") out += '>';
        else if (name == "quot") out += '"';
        else if (name == "apos") out += '\'';
        else if (name.size() > 1 && name[0] == '#') {
            std::uint32_t code = 0;
            const bool hex = name[1] == 'x' || name[1] == 'X';
            const auto digits = name.substr(hex ? 2 : 1);
            const auto [p, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), code, hex ? 16 : 10);
            if (ec != std::errc{} || p != digits.data() + digits.size()) {
                return false;
            }
            put_utf8(out, code);
        } else {
            return false; // no entity of its own: there is no DOCTYPE to define one
        }
        i = semi;
    }
    return true;
}

} // namespace

auto Xml::next() -> Event {
    if (closing_) {
        closing_ = false;
        return Event::End;
    }
    for (;;) {
        if (at_ >= text_.size()) {
            return Event::Done;
        }
        if (text_[at_] != '<') {
            const auto end = text_.find('<', at_);
            const auto raw = text_.substr(at_, end == std::string_view::npos ? std::string_view::npos : end - at_);
            at_ = end == std::string_view::npos ? text_.size() : end;
            if (!resolved(raw, value_)) {
                error_ = "an unknown entity";
                return Event::Error;
            }
            return Event::Text;
        }
        const auto rest = text_.substr(at_);
        if (rest.starts_with("<?")) {
            const auto end = text_.find("?>", at_);
            at_ = end == std::string_view::npos ? text_.size() : end + 2;
            continue;
        }
        if (rest.starts_with("<!--")) {
            const auto end = text_.find("-->", at_);
            at_ = end == std::string_view::npos ? text_.size() : end + 3;
            continue;
        }
        if (rest.starts_with("<![CDATA[")) {
            const auto end = text_.find("]]>", at_);
            if (end == std::string_view::npos) {
                error_ = "an unclosed CDATA";
                return Event::Error;
            }
            value_.assign(text_.substr(at_ + 9, end - at_ - 9));
            at_ = end + 3;
            return Event::Text;
        }
        if (rest.starts_with("<!")) {
            error_ = "a DOCTYPE is not read (its entities could expand without end)";
            return Event::Error;
        }
        const auto close = text_.find('>', at_);
        if (close == std::string_view::npos) {
            error_ = "an unclosed tag";
            return Event::Error;
        }
        auto inside = text_.substr(at_ + 1, close - at_ - 1);
        at_ = close + 1;
        if (inside.starts_with('/')) {
            name_ = local(inside.substr(1));
            while (!name_.empty() && (name_.back() == ' ' || name_.back() == '\t' || name_.back() == '\r' || name_.back() == '\n')) {
                name_.remove_suffix(1);
            }
            return Event::End;
        }
        if (inside.ends_with('/')) {
            inside.remove_suffix(1);
            closing_ = true;
        }
        const auto space = inside.find_first_of(" \t\r\n");
        name_ = local(inside.substr(0, space));
        attributes_ = space == std::string_view::npos ? std::string_view{} : inside.substr(space);
        return Event::Start;
    }
}

auto Xml::attribute(std::string_view name) const -> std::string {
    std::size_t i = 0;
    while (i < attributes_.size()) {
        while (i < attributes_.size() && (attributes_[i] == ' ' || attributes_[i] == '\t' || attributes_[i] == '\r' || attributes_[i] == '\n')) {
            ++i;
        }
        const auto eq = attributes_.find('=', i);
        if (eq == std::string_view::npos || eq + 1 >= attributes_.size()) {
            return {};
        }
        auto key = attributes_.substr(i, eq - i);
        while (!key.empty() && key.back() == ' ') {
            key.remove_suffix(1);
        }
        std::size_t q = eq + 1;
        while (q < attributes_.size() && attributes_[q] == ' ') {
            ++q;
        }
        const char quote = attributes_[q];
        const auto end = attributes_.find(quote, q + 1);
        if ((quote != '"' && quote != '\'') || end == std::string_view::npos) {
            return {};
        }
        if (name.find(':') != std::string_view::npos ? key == name : local(key) == name) { // "r:id" exactly, else by the local name
            std::string out;
            return resolved(attributes_.substr(q + 1, end - q - 1), out) ? out : std::string{};
        }
        i = end + 1;
    }
    return {};
}

} // namespace tools::docs
