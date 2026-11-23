#include "app/sizes.hpp"

#include <array>
#include <charconv>
#include <format>
#include <iterator>

namespace app::sizes {
namespace {

auto digit(char c) -> bool {
    return c >= '0' && c <= '9';
}

auto upper(char c) -> char {
    return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
}

} // namespace

auto text(std::uint64_t bytes) -> std::string {
    if (bytes < 1024) {
        return std::format("{} B", bytes);
    }
    constexpr std::array<const char*, 4> kUnits{"KB", "MB", "GB", "TB"};
    double v = static_cast<double>(bytes) / 1024.0;
    std::size_t u = 0;
    while (v >= 1024.0 && u + 1 < kUnits.size()) {
        v /= 1024.0;
        ++u;
    }
    return v < 10 ? std::format("{:.1f} {}", v, kUnits[u]) : std::format("{:.0f} {}", v, kUnits[u]);
}

auto parse(std::string_view cell) -> std::optional<double> {
    while (!cell.empty() && cell.front() == ' ') {
        cell.remove_prefix(1);
    }
    while (!cell.empty() && cell.back() == ' ') {
        cell.remove_suffix(1);
    }
    std::size_t i = 0;
    const std::size_t n = cell.size();
    bool negative = false;
    if (i < n && (cell[i] == '+' || cell[i] == '-')) {
        negative = cell[i] == '-';
        ++i;
    }
    if (i < n && cell[i] == '$') {
        ++i;
    }
    std::string number;
    while (i < n && digit(cell[i])) {
        number += cell[i++];
    }
    if (number.empty()) {
        return std::nullopt;
    }
    // "1 284": a space before exactly three digits is a thousands' space.
    while (i + 3 < n && cell[i] == ' ' && digit(cell[i + 1]) && digit(cell[i + 2]) && digit(cell[i + 3]) &&
           (i + 4 == n || !digit(cell[i + 4]))) {
        number.append(cell.substr(i + 1, 3));
        i += 4;
    }
    if (i < n && cell[i] == '.') {
        number += '.';
        ++i;
        while (i < n && digit(cell[i])) {
            number += cell[i++];
        }
    }
    double value = 0;
    if (std::from_chars(number.data(), number.data() + number.size(), value).ec != std::errc{}) {
        return std::nullopt;
    }
    while (i < n && cell[i] == ' ') {
        ++i;
    }
    std::string unit;
    for (; i < n; ++i) {
        unit += upper(cell[i]);
    }
    constexpr std::array<std::pair<std::string_view, double>, 7> kUnits{
        {{"", 1.0}, {"%", 1.0}, {"B", 1.0}, {"KB", 1024.0}, {"MB", 1048576.0}, {"GB", 1073741824.0}, {"TB", 1099511627776.0}}};
    for (const auto& [name, factor] : kUnits) {
        if (unit == name) {
            return (negative ? -value : value) * factor;
        }
    }
    return std::nullopt;
}

} // namespace app::sizes
