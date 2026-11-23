#include "core/utf8.hpp"

#include <windows.h>

#include <array>
#include <cstring>

namespace core::utf8 {
namespace {

// ---- Validation DFA ---------------------------------------------------------------------
// States are stored as bit offsets (6 bits each) so one transition is a load, a shift and a
// mask: next = (kTable[byte] >> state) & 63. Nine states fit in 54 bits.
enum State : std::uint32_t {
    kAccept = 0,   // between characters
    kReject = 1,   // sink
    kTail1 = 2,    // one continuation byte (80..BF) left
    kTail2 = 3,    // two left
    kAfterE0 = 4,  // E0: next must be A0..BF (no overlongs)
    kAfterED = 5,  // ED: next must be 80..9F (no surrogates)
    kTail3 = 6,    // three left (after F1..F3)
    kAfterF0 = 7,  // F0: next must be 90..BF
    kAfterF4 = 8,  // F4: next must be 80..8F (<= U+10FFFF)
    kStateCount = 9,
};

constexpr auto next_state(std::uint32_t state, std::uint32_t b) -> std::uint32_t {
    const auto in = [b](std::uint32_t lo, std::uint32_t hi) { return b >= lo && b <= hi; };
    switch (state) {
    case kAccept:
        if (b <= 0x7F) return kAccept;
        if (in(0xC2, 0xDF)) return kTail1;
        if (b == 0xE0) return kAfterE0;
        if (b == 0xED) return kAfterED;
        if (in(0xE1, 0xEF)) return kTail2;
        if (b == 0xF0) return kAfterF0;
        if (in(0xF1, 0xF3)) return kTail3;
        if (b == 0xF4) return kAfterF4;
        return kReject;
    case kTail1: return in(0x80, 0xBF) ? kAccept : kReject;
    case kTail2: return in(0x80, 0xBF) ? kTail1 : kReject;
    case kAfterE0: return in(0xA0, 0xBF) ? kTail1 : kReject;
    case kAfterED: return in(0x80, 0x9F) ? kTail1 : kReject;
    case kTail3: return in(0x80, 0xBF) ? kTail2 : kReject;
    case kAfterF0: return in(0x90, 0xBF) ? kTail2 : kReject;
    case kAfterF4: return in(0x80, 0x8F) ? kTail2 : kReject;
    default: return kReject;
    }
}

constexpr auto build_table() -> std::array<std::uint64_t, 256> {
    std::array<std::uint64_t, 256> table{};
    for (std::uint32_t b = 0; b < 256; ++b) {
        std::uint64_t row = 0;
        for (std::uint32_t s = 0; s < kStateCount; ++s) {
            row |= std::uint64_t{next_state(s, b) * 6} << (s * 6);
        }
        table[b] = row;
    }
    return table;
}

constexpr auto kTable = build_table();

constexpr std::uint64_t kAcceptOffset = kAccept * 6;
constexpr std::uint64_t kRejectOffset = kReject * 6;

auto load64(const unsigned char* p) noexcept -> std::uint64_t {
    std::uint64_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

} // namespace

auto decode(std::string_view s, std::size_t pos) noexcept -> Decoded {
    const auto* p = reinterpret_cast<const unsigned char*>(s.data()) + pos;
    const std::size_t left = s.size() - pos;
    const std::uint32_t b0 = p[0];
    if (b0 < 0x80) {
        return {b0, 1, true};
    }

    std::uint32_t need = 0;
    std::uint32_t lo = 0x80, hi = 0xBF; // allowed range of the second byte
    char32_t cp = 0;
    if (b0 >= 0xC2 && b0 <= 0xDF) {
        need = 1;
        cp = b0 & 0x1F;
    } else if (b0 >= 0xE0 && b0 <= 0xEF) {
        need = 2;
        cp = b0 & 0x0F;
        if (b0 == 0xE0) lo = 0xA0;
        if (b0 == 0xED) hi = 0x9F;
    } else if (b0 >= 0xF0 && b0 <= 0xF4) {
        need = 3;
        cp = b0 & 0x07;
        if (b0 == 0xF0) lo = 0x90;
        if (b0 == 0xF4) hi = 0x8F;
    } else {
        return {kReplacement, 1, false};
    }

    std::uint8_t consumed = 1;
    for (std::uint32_t i = 0; i < need; ++i) {
        if (consumed >= left) {
            return {kReplacement, consumed, false}; // truncated: skip the valid prefix
        }
        const std::uint32_t b = p[consumed];
        const std::uint32_t min = i == 0 ? lo : 0x80;
        const std::uint32_t max = i == 0 ? hi : 0xBF;
        if (b < min || b > max) {
            return {kReplacement, consumed, false};
        }
        cp = (cp << 6) | (b & 0x3F);
        ++consumed;
    }
    return {cp, consumed, true};
}

auto is_valid(std::string_view s) noexcept -> bool {
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const auto* end = p + s.size();
    std::uint64_t state = kAcceptOffset;
    while (p < end) {
        // ASCII fast path: only between characters, 8 bytes at a time.
        if (state == kAcceptOffset) {
            while (end - p >= 8 && (load64(p) & 0x8080808080808080ull) == 0) {
                p += 8;
            }
            if (p == end) {
                break;
            }
        }
        state = (kTable[*p++] >> state) & 63;
        if (state == kRejectOffset) {
            return false;
        }
    }
    return state == kAcceptOffset;
}

auto count_code_points(std::string_view s) noexcept -> std::size_t {
    std::size_t count = 0;
    for (std::size_t pos = 0; pos < s.size(); pos += decode(s, pos).length) {
        ++count;
    }
    return count;
}

auto encode(char32_t cp, char (&out)[4]) noexcept -> std::size_t {
    if (cp < 0x80) {
        out[0] = static_cast<char>(cp);
        return 1;
    }
    if (cp < 0x800) {
        out[0] = static_cast<char>(0xC0 | (cp >> 6));
        out[1] = static_cast<char>(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp >= 0xD800 && cp <= 0xDFFF) {
        return 0;
    }
    if (cp < 0x10000) {
        out[0] = static_cast<char>(0xE0 | (cp >> 12));
        out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp <= 0x10FFFF) {
        out[0] = static_cast<char>(0xF0 | (cp >> 18));
        out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[3] = static_cast<char>(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

auto to_wide(std::string_view s) -> core::Result<std::wstring> {
    if (s.empty()) {
        return std::wstring{};
    }
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                                      static_cast<int>(s.size()), nullptr, 0);
    if (n == 0) {
        return core::make_error(core::Code::Parse, "invalid UTF-8 in text for a Win32 call");
    }
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()),
                        out.data(), n);
    return out;
}

auto to_utf8(std::wstring_view w) -> core::Result<std::string> {
    if (w.empty()) {
        return std::string{};
    }
    const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(),
                                      static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n == 0) {
        return core::make_error(core::Code::Parse, "invalid UTF-16 (unpaired surrogate)");
    }
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), static_cast<int>(w.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

} // namespace core::utf8
