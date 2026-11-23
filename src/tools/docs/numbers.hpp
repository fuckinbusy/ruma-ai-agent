#pragma once

// Numbers as they are written in a table (tools::docs): "12.5", "-3", "1.5 MB".

#include <array>
#include <charconv>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace tools::docs {

// A whole plain number: digits, one '.', a sign; spaces around. "1e3", "1,5" and "12 kg" are not.
inline auto number_of(std::string_view s) -> std::optional<double> {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
    if (!s.empty() && s.front() == '+') s.remove_prefix(1);
    if (s.empty() || s.find_first_not_of("-.0123456789") != std::string_view::npos) {
        return std::nullopt;
    }
    double v = 0;
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || p != s.data() + s.size()) {
        return std::nullopt;
    }
    return v;
}

// "1.5 MB", "512 KB", "3 GB", "900 B" (1024 to the next unit) -> megabytes.
inline auto megabytes_of(std::string_view s) -> std::optional<double> {
    const auto space = s.find_last_of(' ');
    if (space == std::string_view::npos) {
        return std::nullopt;
    }
    const auto value = number_of(s.substr(0, space));
    const auto unit = s.substr(space + 1);
    if (!value) {
        return std::nullopt;
    }
    if (unit == "B") return *value / (1024.0 * 1024.0);
    if (unit == "KB") return *value / 1024.0;
    if (unit == "MB") return *value;
    if (unit == "GB") return *value * 1024.0;
    if (unit == "TB") return *value * 1024.0 * 1024.0;
    return std::nullopt;
}

// Money as it is written (phase 10): a number with its symbol (¥ $ € £ ₽) before or after it, spaces
// (plain, no-break, narrow) and thousands separators in it, a sign before it all. "1.234,56 €" and
// "$1,234.56" both read: the last of ',' and '.' is the decimal point; a lone ',' before two digits
// at the end is one too.
struct Money {
    double value = 0;
    std::string_view symbol; // one of kCurrencies
};
inline constexpr std::array<std::string_view, 5> kCurrencies{"\xC2\xA5", "$", "\xE2\x82\xAC", "\xC2\xA3", "\xE2\x82\xBD"};

// The currency a cell's number format shows (phase 10): "[$€-407]#,##0.00" -> "€", "\"¥\"#,##0" -> "¥",
// "$#,##0.00" -> "$" — the first symbol where it stands; a locale's tag "[$-409]" (a date's, say) is
// none, and the text of other brackets ([Red], [h]) is skipped.
inline auto currency_in_format(std::string_view code) -> std::string_view {
    for (std::size_t i = 0; i < code.size();) {
        if (code[i] == '[') {
            const auto end = code.find(']', i);
            if (end == std::string_view::npos) {
                return {};
            }
            if (code.substr(i, 2) == "[$") { // [$SYMBOL-LCID]: the symbol up to its '-'
                const auto inner = code.substr(i + 2, end - i - 2);
                const auto symbol = inner.substr(0, inner.find('-'));
                for (const auto c : kCurrencies) {
                    if (symbol == c) {
                        return c;
                    }
                }
            }
            i = end + 1;
            continue;
        }
        for (const auto c : kCurrencies) {
            if (code.substr(i).starts_with(c)) {
                return c;
            }
        }
        ++i;
    }
    return {};
}

namespace detail {

inline constexpr std::array<std::string_view, 3> kSpaces{" ", "\xC2\xA0", "\xE2\x80\xAF"};

inline void trim_spaces(std::string_view& v) {
    for (bool again = true; again;) {
        again = false;
        for (const auto space : kSpaces) {
            if (v.starts_with(space)) {
                v.remove_prefix(space.size());
                again = true;
            }
            if (v.ends_with(space)) {
                v.remove_suffix(space.size());
                again = true;
            }
        }
    }
}

} // namespace detail

inline auto money_of(std::string_view s) -> std::optional<Money> {
    detail::trim_spaces(s);
    bool negative = false;
    if (s.starts_with("-")) {
        negative = true;
        s.remove_prefix(1);
        detail::trim_spaces(s);
    }
    std::string_view symbol;
    for (const auto c : kCurrencies) {
        if (s.starts_with(c)) {
            symbol = c;
            s.remove_prefix(c.size());
            break;
        }
        if (s.ends_with(c)) {
            symbol = c;
            s.remove_suffix(c.size());
            break;
        }
    }
    detail::trim_spaces(s);
    if (symbol.empty() || s.empty()) {
        return std::nullopt;
    }
    if (s.starts_with("-")) {
        negative = !negative;
        s.remove_prefix(1);
    }
    std::string digits; // the spaces of any kind go
    while (!s.empty()) {
        bool space = false;
        for (const auto sp : detail::kSpaces) {
            if (s.starts_with(sp)) {
                s.remove_prefix(sp.size());
                space = true;
                break;
            }
        }
        if (!space) {
            digits += s.front();
            s.remove_prefix(1);
        }
    }
    const auto comma = digits.rfind(',');
    const auto dot = digits.rfind('.');
    const bool comma_decimal = comma != std::string::npos &&
                               ((dot != std::string::npos && comma > dot) ||
                                (dot == std::string::npos && digits.find(',') == comma && digits.size() - comma == 3));
    std::string plain;
    for (const char c : digits) {
        if (c == '.') {
            if (!comma_decimal) {
                plain += '.';
            }
        } else if (c == ',') {
            if (comma_decimal) {
                plain += '.';
            }
        } else {
            plain += c;
        }
    }
    const auto v = number_of(plain);
    if (!v || plain.find('-') != std::string::npos) {
        return std::nullopt;
    }
    return Money{.value = negative ? -*v : *v, .symbol = symbol};
}

// A date as Excel counts it, days since 1899-12-30 (phase 10): "2026-09-01" (a time after it
// aside), "01.09.2026", "01/09/2026" (the day first, unless the second cannot be a month),
// "01 Sep 2026", "1 September 2026", "Sep 1, 2026". Never a date that does not exist.
inline auto date_of(std::string_view s) -> std::optional<double> {
    using namespace std::chrono;
    const auto serial = [](int y, int m, int d) -> std::optional<double> {
        if (y < 1900 || y > 9999 || m < 1 || m > 12 || d < 1 || d > 31) {
            return std::nullopt;
        }
        const year_month_day ymd{year{y}, month{static_cast<unsigned>(m)}, day{static_cast<unsigned>(d)}};
        if (!ymd.ok()) {
            return std::nullopt;
        }
        return static_cast<double>((sys_days{ymd} - sys_days{year{1899} / December / 30}).count());
    };
    const auto number = [](std::string_view v) -> std::optional<int> {
        int n = 0;
        if (v.empty() || v.size() > 4) {
            return std::nullopt;
        }
        const auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), n);
        return ec == std::errc{} && p == v.data() + v.size() ? std::optional<int>{n} : std::nullopt;
    };
    const auto month_of = [](std::string_view name) -> int {
        static constexpr std::array<std::string_view, 12> kMonths{"jan", "feb", "mar", "apr", "may", "jun",
                                                                  "jul", "aug", "sep", "oct", "nov", "dec"};
        if (name.size() < 3) {
            return 0;
        }
        std::array<char, 3> head{};
        for (std::size_t i = 0; i < 3; ++i) {
            head[i] = static_cast<char>(name[i] >= 'A' && name[i] <= 'Z' ? name[i] - 'A' + 'a' : name[i]);
        }
        for (std::size_t i = 0; i < kMonths.size(); ++i) {
            if (kMonths[i] == std::string_view{head.data(), 3}) {
                return static_cast<int>(i) + 1;
            }
        }
        return 0;
    };
    detail::trim_spaces(s);
    // ISO: 2026-09-01, a time after it.
    if (s.size() >= 10 && s[4] == '-' && s[7] == '-' && (s.size() == 10 || s[10] == 'T' || s[10] == ' ')) {
        const auto y = number(s.substr(0, 4)), m = number(s.substr(5, 2)), d = number(s.substr(8, 2));
        return y && m && d ? serial(*y, *m, *d) : std::nullopt;
    }
    // 01.09.2026, 01/09/2026: the day first unless the second cannot be a month.
    for (const char sep : {'.', '/'}) {
        const auto a = s.find(sep);
        const auto b = a == std::string_view::npos ? a : s.find(sep, a + 1);
        if (b != std::string_view::npos && s.find(sep, b + 1) == std::string_view::npos) {
            const auto p = number(s.substr(0, a)), q = number(s.substr(a + 1, b - a - 1)), y = number(s.substr(b + 1));
            if (!p || !q || !y || s.size() - b - 1 != 4) {
                return std::nullopt;
            }
            return *q > 12 ? serial(*y, *p, *q) : serial(*y, *q, *p);
        }
    }
    // 01 Sep 2026, 1 September 2026, Sep 1, 2026: three words.
    std::array<std::string_view, 3> words{};
    std::size_t n = 0;
    for (std::size_t i = 0; i < s.size();) {
        while (i < s.size() && (s[i] == ' ' || s[i] == ',')) {
            ++i;
        }
        if (i >= s.size()) {
            break;
        }
        if (n == words.size()) {
            return std::nullopt;
        }
        const auto j = s.find_first_of(" ,", i);
        words[n++] = s.substr(i, j == std::string_view::npos ? s.size() - i : j - i);
        i = j == std::string_view::npos ? s.size() : j;
    }
    if (n != 3) {
        return std::nullopt;
    }
    const auto y = number(words[2]);
    if (!y) {
        return std::nullopt;
    }
    if (const auto d = number(words[0]); d && month_of(words[1]) > 0) {
        return serial(*y, month_of(words[1]), *d);
    }
    if (const auto d = number(words[1]); d && month_of(words[0]) > 0) {
        return serial(*y, month_of(words[0]), *d);
    }
    return std::nullopt;
}

} // namespace tools::docs
