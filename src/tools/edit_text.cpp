#include "tools/edit_text.hpp"

#include "core/utf8.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <vector>

namespace tools::edit {
namespace {

constexpr std::string_view kBom8 = "\xEF\xBB\xBF";
constexpr std::string_view kBom16 = "\xFF\xFE";
constexpr std::size_t kSniff = 8 * 1024; // a NUL in these: binary

// Every line break is CRLF (and there is one).
auto all_crlf(std::string_view s) -> bool {
    bool any = false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n') {
            if (i == 0 || s[i - 1] != '\r') {
                return false;
            }
            any = true;
        }
    }
    return any;
}

auto replace_all(std::string s, std::string_view from, std::string_view to) -> std::string {
    std::string out;
    out.reserve(s.size());
    std::size_t at = 0;
    for (std::size_t hit; (hit = s.find(from, at)) != std::string::npos; at = hit + from.size()) {
        out.append(s, at, hit - at);
        out += to;
    }
    out.append(s, at);
    return out;
}

auto from_1251(std::string_view bytes) -> core::Result<std::string> {
    if (bytes.empty()) {
        return std::string{};
    }
    const int n = MultiByteToWideChar(1251, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(1251, 0, bytes.data(), static_cast<int>(bytes.size()), w.data(), n);
    return core::utf8::to_utf8(w);
}

auto to_1251(std::string_view utf8) -> core::Result<std::string> {
    auto w = core::utf8::to_wide(utf8);
    if (!w) {
        return std::unexpected{w.error()};
    }
    if (w->empty()) {
        return std::string{};
    }
    BOOL lost = FALSE;
    const int n = WideCharToMultiByte(1251, WC_NO_BEST_FIT_CHARS, w->data(), static_cast<int>(w->size()), nullptr, 0, nullptr, &lost);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(1251, WC_NO_BEST_FIT_CHARS, w->data(), static_cast<int>(w->size()), out.data(), n, nullptr, &lost);
    if (lost) {
        return core::make_error(core::Code::InvalidArgument,
                                "the file is Windows-1251 and the new text has characters it cannot hold: use fs.write to rewrite it");
    }
    return out;
}

auto lines_of(std::string_view text) -> std::vector<std::string_view> {
    std::vector<std::string_view> out;
    while (!text.empty()) {
        const auto nl = text.find('\n');
        out.push_back(text.substr(0, nl));
        if (nl == std::string_view::npos) {
            break;
        }
        text.remove_prefix(nl + 1);
    }
    return out;
}

enum class Op : std::uint8_t { Same, Del, Add };
struct Step {
    Op op;
    std::size_t a; // its line in before (Same, Del)
    std::size_t b; // its line in after (Same, Add)
};

// The edit script: the common head and tail kept, the middle by longest common subsequence (up
// to 2000 x 2000 lines; past that, all of it out, then all of it in).
auto script(const std::vector<std::string_view>& a, const std::vector<std::string_view>& b) -> std::vector<Step> {
    std::size_t head = 0;
    while (head < a.size() && head < b.size() && a[head] == b[head]) {
        ++head;
    }
    std::size_t tail = 0;
    while (tail < a.size() - head && tail < b.size() - head && a[a.size() - 1 - tail] == b[b.size() - 1 - tail]) {
        ++tail;
    }
    std::vector<Step> out;
    for (std::size_t i = 0; i < head; ++i) {
        out.push_back({Op::Same, i, i});
    }
    const std::size_t na = a.size() - head - tail;
    const std::size_t nb = b.size() - head - tail;
    if (na <= 2000 && nb <= 2000) {
        std::vector<std::vector<std::uint16_t>> l(na + 1, std::vector<std::uint16_t>(nb + 1, 0));
        for (std::size_t i = na; i-- > 0;) {
            for (std::size_t j = nb; j-- > 0;) {
                l[i][j] = a[head + i] == b[head + j] ? static_cast<std::uint16_t>(l[i + 1][j + 1] + 1) : std::max(l[i + 1][j], l[i][j + 1]);
            }
        }
        std::size_t i = 0, j = 0;
        while (i < na || j < nb) {
            if (i < na && j < nb && a[head + i] == b[head + j]) {
                out.push_back({Op::Same, head + i++, head + j++});
            } else if (i < na && (j == nb || l[i + 1][j] >= l[i][j + 1])) {
                out.push_back({Op::Del, head + i++, 0}); // what goes first, then what comes
            } else {
                out.push_back({Op::Add, 0, head + j++});
            }
        }
    } else {
        for (std::size_t i = 0; i < na; ++i) {
            out.push_back({Op::Del, head + i, 0});
        }
        for (std::size_t j = 0; j < nb; ++j) {
            out.push_back({Op::Add, 0, head + j});
        }
    }
    for (std::size_t k = 0; k < tail; ++k) {
        out.push_back({Op::Same, a.size() - tail + k, b.size() - tail + k});
    }
    return out;
}

} // namespace

auto decode(std::string_view bytes) -> core::Result<Text> {
    Text t;
    if (bytes.starts_with(kBom16)) {
        const auto body = bytes.substr(kBom16.size());
        if (body.size() % 2 != 0) {
            return core::make_error(core::Code::InvalidArgument, "the file says UTF-16 but has an odd number of bytes");
        }
        std::wstring w(body.size() / 2, L'\0');
        std::copy_n(body.data(), body.size(), reinterpret_cast<char*>(w.data()));
        auto u = core::utf8::to_utf8(w);
        if (!u) {
            return core::make_error(core::Code::InvalidArgument, "the file's UTF-16 text is broken");
        }
        t.encoding = Encoding::Utf16Le;
        t.utf8 = std::move(*u);
    } else {
        std::string_view body = bytes;
        t.encoding = Encoding::Utf8;
        if (body.starts_with(kBom8)) {
            body.remove_prefix(kBom8.size());
            t.encoding = Encoding::Utf8Bom;
        }
        if (body.substr(0, kSniff).find('\0') != std::string_view::npos) {
            return core::make_error(core::Code::InvalidArgument, "the file is binary: fs.edit changes text files only");
        }
        if (core::utf8::to_wide(body) || body.empty()) {
            t.utf8 = std::string{body};
        } else if (t.encoding == Encoding::Utf8Bom) {
            return core::make_error(core::Code::InvalidArgument, "the file says UTF-8 but is not");
        } else {
            auto u = from_1251(body);
            if (!u) {
                return std::unexpected{u.error()};
            }
            t.encoding = Encoding::Cp1251;
            t.utf8 = std::move(*u);
        }
    }
    t.crlf = all_crlf(t.utf8);
    if (t.crlf) {
        t.utf8 = replace_all(std::move(t.utf8), "\r\n", "\n");
    }
    return t;
}

auto encode(const Text& text) -> core::Result<std::string> {
    const std::string body = text.crlf ? replace_all(text.utf8, "\n", "\r\n") : text.utf8;
    switch (text.encoding) {
    case Encoding::Utf8: return body;
    case Encoding::Utf8Bom: return std::string{kBom8} + body;
    case Encoding::Cp1251: return to_1251(body);
    case Encoding::Utf16Le: {
        auto w = core::utf8::to_wide(body);
        if (!w && !body.empty()) {
            return std::unexpected{w.error()};
        }
        std::string out{kBom16};
        if (w) {
            out.append(reinterpret_cast<const char*>(w->data()), w->size() * sizeof(wchar_t));
        }
        return out;
    }
    }
    return core::make_error(core::Code::Internal, "an unknown encoding");
}

auto apply(std::string_view text, std::span<const Edit> edits) -> core::Result<std::string> {
    std::string s{text};
    for (std::size_t n = 0; n < edits.size(); ++n) {
        const auto& e = edits[n];
        if (e.old_text.empty()) {
            return core::make_error(core::Code::InvalidArgument, std::format("edit {}: its old text is empty", n + 1));
        }
        std::size_t count = 0;
        for (std::size_t at = s.find(e.old_text); at != std::string::npos; at = s.find(e.old_text, at + e.old_text.size())) {
            ++count;
        }
        if (count == 0) {
            return core::make_error(core::Code::InvalidArgument,
                                    std::format("edit {}: not found (the old text must be exactly as in the file); nothing was changed", n + 1));
        }
        if (count > 1 && !e.all) {
            return core::make_error(core::Code::InvalidArgument,
                                    std::format("edit {}: found {} times; give more of the text around it, or all: true; nothing was changed",
                                                n + 1, count));
        }
        s = e.all ? replace_all(std::move(s), e.old_text, e.new_text) : s.replace(s.find(e.old_text), e.old_text.size(), e.new_text);
    }
    return s;
}

auto unified_diff(std::string_view before, std::string_view after, int context, int max_lines) -> std::string {
    const auto a = lines_of(before);
    const auto b = lines_of(after);
    const auto steps = script(a, b);
    const auto c = static_cast<std::size_t>(std::max(context, 0));
    std::string out;
    int shown = 0, hidden = 0;
    const auto line = [&](std::string text) {
        if (shown < max_lines) {
            out += text;
            ++shown;
        } else {
            ++hidden;
        }
    };
    std::size_t k = 0;
    while (k < steps.size()) {
        while (k < steps.size() && steps[k].op == Op::Same) {
            ++k;
        }
        if (k == steps.size()) {
            break;
        }
        // A hunk: from `context` lines before this change to `context` after the last change
        // closer than 2 x context to the one before it.
        const std::size_t first = k >= c ? k - c : 0;
        std::size_t last = k;
        for (std::size_t m = k; m < steps.size(); ++m) {
            if (steps[m].op != Op::Same) {
                last = m;
            } else if (m - last > 2 * c) {
                break;
            }
        }
        const std::size_t end = std::min(steps.size(), last + c + 1);
        std::size_t a0 = 0, b0 = 0, na = 0, nb = 0;
        bool a_set = false, b_set = false;
        for (std::size_t m = first; m < end; ++m) {
            if (steps[m].op != Op::Add) {
                a0 = a_set ? a0 : steps[m].a;
                a_set = true;
                ++na;
            }
            if (steps[m].op != Op::Del) {
                b0 = b_set ? b0 : steps[m].b;
                b_set = true;
                ++nb;
            }
        }
        // Lines start at 1; an empty side is at the line before it.
        const auto start = [&](bool set, std::size_t at, std::size_t n) { return set ? at + 1 : (n == 0 ? 0 : at); };
        line(std::format("@@ -{},{} +{},{} @@\n", start(a_set, a0, na), na, start(b_set, b0, nb), nb));
        for (std::size_t m = first; m < end; ++m) {
            const auto& s = steps[m];
            const std::string_view text = s.op == Op::Add ? b[s.b] : a[s.a];
            line(std::format("{}{}\n", s.op == Op::Same ? ' ' : s.op == Op::Del ? '-' : '+', text));
        }
        k = end;
    }
    if (hidden > 0) {
        out += std::format("\xE2\x80\xA6 {} more lines\n", hidden);
    }
    return out;
}

} // namespace tools::edit
