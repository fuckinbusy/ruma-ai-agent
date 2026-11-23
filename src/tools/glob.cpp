#include "tools/glob.hpp"

#include "core/utf8.hpp"

#include <windows.h>

#include <algorithm>
#include <optional>
#include <span>

namespace tools::glob {
namespace {

// Lower case, `\` as `/` when `slashes` (a path, a name pattern; never a .gitignore line, where
// `\` escapes the next character).
auto folded(std::string_view s, bool slashes) -> std::wstring {
    auto w = core::utf8::to_wide(s);
    std::wstring out = w ? std::move(*w) : std::wstring{};
    if (!out.empty()) {
        CharLowerBuffW(out.data(), static_cast<DWORD>(out.size()));
    }
    if (slashes) {
        std::ranges::replace(out, L'\\', L'/');
    }
    return out;
}

auto split(std::wstring_view s) -> std::vector<std::wstring_view> {
    std::vector<std::wstring_view> out;
    std::size_t at = 0;
    for (;;) {
        const auto cut = s.find(L'/', at);
        out.push_back(s.substr(at, cut == std::wstring_view::npos ? std::wstring_view::npos : cut - at));
        if (cut == std::wstring_view::npos) {
            return out;
        }
        at = cut + 1;
    }
}

// `[...]` at p[i]: whether `c` is in it, and where the class ends (i stays when it never closes).
auto in_class(std::wstring_view p, std::size_t& i, wchar_t c) -> std::optional<bool> {
    std::size_t k = i + 1;
    const bool negated = k < p.size() && (p[k] == L'!' || p[k] == L'^');
    k += negated ? 1 : 0;
    bool found = false;
    for (bool first = true; k < p.size() && (first || p[k] != L']'); first = false) {
        wchar_t lo = p[k] == L'\\' && k + 1 < p.size() ? p[++k] : p[k];
        wchar_t hi = lo;
        if (k + 2 < p.size() && p[k + 1] == L'-' && p[k + 2] != L']') {
            k += 2;
            hi = p[k] == L'\\' && k + 1 < p.size() ? p[++k] : p[k];
        }
        found = found || (c >= lo && c <= hi);
        ++k;
    }
    if (k >= p.size()) {
        return std::nullopt; // no `]`: a plain `[`
    }
    i = k;
    return found != negated;
}

// One name against one name's pattern: `*`, `?`, `[...]`, `\x`.
auto name_match(std::wstring_view p, std::wstring_view s) -> bool {
    std::size_t i = 0, j = 0;
    std::size_t star = std::wstring_view::npos, resume = 0;
    while (j < s.size()) {
        if (i < p.size() && p[i] == L'*') {
            while (i < p.size() && p[i] == L'*') {
                ++i;
            }
            star = i;
            resume = j;
            continue;
        }
        bool one = false;
        std::size_t next = i + 1;
        if (i < p.size()) {
            if (p[i] == L'?') {
                one = true;
            } else if (p[i] == L'[') {
                std::size_t end = i;
                if (const auto in = in_class(p, end, s[j])) {
                    one = *in;
                    next = end + 1;
                } else {
                    one = s[j] == L'[';
                }
            } else if (p[i] == L'\\' && i + 1 < p.size()) {
                one = p[i + 1] == s[j];
                next = i + 2;
            } else {
                one = p[i] == s[j];
            }
        }
        if (one) {
            i = next;
            ++j;
        } else if (star != std::wstring_view::npos) {
            i = star;
            j = ++resume;
        } else {
            return false;
        }
    }
    while (i < p.size() && p[i] == L'*') {
        ++i;
    }
    return i == p.size();
}

// Names against names' patterns; `**` any names, none included, but a trailing one at least one.
auto names_match(std::span<const std::wstring_view> p, std::span<const std::wstring_view> s) -> bool {
    if (p.empty()) {
        return s.empty();
    }
    if (p[0] == L"**") {
        if (p.size() == 1) {
            return !s.empty();
        }
        for (std::size_t k = 0; k <= s.size(); ++k) {
            if (names_match(p.subspan(1), s.subspan(k))) {
                return true;
            }
        }
        return false;
    }
    return !s.empty() && name_match(p[0], s[0]) && names_match(p.subspan(1), s.subspan(1));
}

auto path_match(std::wstring_view pattern, std::wstring_view path) -> bool {
    const auto p = split(pattern);
    const auto s = split(path);
    return names_match(p, s);
}

auto depth_of(std::wstring_view folder) -> std::size_t {
    return folder.empty() ? 0 : static_cast<std::size_t>(std::ranges::count(folder, L'/')) + 1;
}

} // namespace

auto glob_match(std::string_view pattern, std::string_view relative_path) -> bool {
    return path_match(folded(pattern, true), folded(relative_path, true));
}

void Ignore::add(std::string_view folder, std::string_view text) {
    std::wstring dir = folded(folder, true);
    while (!dir.empty() && dir.back() == L'/') {
        dir.pop_back();
    }
    const auto all = folded(text, false);
    std::size_t at = 0;
    while (at <= all.size()) {
        const auto cut = all.find(L'\n', at);
        std::wstring line = all.substr(at, cut == std::wstring::npos ? std::wstring::npos : cut - at);
        at = cut == std::wstring::npos ? all.size() + 1 : cut + 1;
        if (!line.empty() && line.back() == L'\r') {
            line.pop_back();
        }
        // Trailing spaces go, unless the last one is escaped.
        while (!line.empty() && line.back() == L' ' && !(line.size() >= 2 && line[line.size() - 2] == L'\\')) {
            line.pop_back();
        }
        if (line.empty() || line.front() == L'#') {
            continue;
        }
        Rule r{.folder = dir, .depth = depth_of(dir)};
        if (line.front() == L'!') {
            r.negate = true;
            line.erase(0, 1);
        } else if (line.starts_with(L"\\#") || line.starts_with(L"\\!")) {
            line.erase(0, 1);
        }
        if (!line.empty() && line.back() == L'/') {
            r.dir_only = true;
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        // A slash at the start or in the middle anchors the pattern to its folder; none: any depth.
        if (line.find(L'/') == std::wstring::npos) {
            line = L"**/" + line;
        } else if (line.front() == L'/') {
            line.erase(0, 1);
        }
        r.pattern = std::move(line);
        rules_.push_back(std::move(r));
    }
}

auto Ignore::ignored(std::string_view relative, bool dir) const -> bool {
    const auto path = folded(relative, true);
    for (const auto name : split(path)) {
        if (name == L".git" || name == L"node_modules") {
            return true;
        }
    }
    // The matching rule of the deepest folder, its last line.
    const Rule* last = nullptr;
    for (const auto& r : rules_) {
        if (r.dir_only && !dir) {
            continue;
        }
        std::wstring_view inside = path;
        if (!r.folder.empty()) {
            if (!(inside.starts_with(r.folder) && inside.size() > r.folder.size() && inside[r.folder.size()] == L'/')) {
                continue;
            }
            inside.remove_prefix(r.folder.size() + 1);
        }
        if (path_match(r.pattern, inside) && (last == nullptr || r.depth >= last->depth)) {
            last = &r;
        }
    }
    return last != nullptr && !last->negate;
}

} // namespace tools::glob
