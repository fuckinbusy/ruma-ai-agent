#include "security/sha256.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <cstdlib>

#pragma comment(lib, "bcrypt.lib")

namespace security {

auto sha256(std::string_view data) -> Digest {
    Digest d{};
    // BCRYPT_SHA256_ALG_HANDLE: a pseudo-handle, nothing to open or close.
    const NTSTATUS s = BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
                                  reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                                  static_cast<ULONG>(data.size()), d.data(), static_cast<ULONG>(d.size()));
    if (!BCRYPT_SUCCESS(s)) {
        std::abort(); // never fails for these arguments; a wrong hash would break the chain silently
    }
    return d;
}

auto hex(const Digest& d) -> std::string {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(d.size() * 2);
    for (const std::uint8_t b : d) {
        out += kDigits[b >> 4];
        out += kDigits[b & 0xF];
    }
    return out;
}

} // namespace security
