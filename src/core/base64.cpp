#include "core/base64.hpp"

#include <cstdint>
#include <string_view>

namespace core {

auto base64(std::span<const std::byte> data) -> std::string {
    static constexpr std::string_view kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= data.size(); i += 3) {
        const auto n = (std::to_integer<std::uint32_t>(data[i]) << 16) | (std::to_integer<std::uint32_t>(data[i + 1]) << 8) |
                       std::to_integer<std::uint32_t>(data[i + 2]);
        out += kAlphabet[(n >> 18) & 63];
        out += kAlphabet[(n >> 12) & 63];
        out += kAlphabet[(n >> 6) & 63];
        out += kAlphabet[n & 63];
    }
    if (const auto rest = data.size() - i; rest > 0) {
        auto n = std::to_integer<std::uint32_t>(data[i]) << 16;
        if (rest == 2) {
            n |= std::to_integer<std::uint32_t>(data[i + 1]) << 8;
        }
        out += kAlphabet[(n >> 18) & 63];
        out += kAlphabet[(n >> 12) & 63];
        out += rest == 2 ? kAlphabet[(n >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

} // namespace core
