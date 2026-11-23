#include "net/curl.hpp"

#include "core/mem/mem.hpp"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>

namespace net::curl {
namespace {

// curl's memory goes through core::mem under its own tag (01-ARCHITECTURE § Memory, rule 2): a
// leak in it shows by name, and the leak checks see it like ours.
auto tag() -> core::mem::TagId {
    static const core::mem::TagId t = core::mem::tag_id("net/curl");
    return t;
}

auto c_malloc(std::size_t size) -> void* {
    const core::mem::TagScope scope{tag()};
    return core::mem::allocate(size);
}

void c_free(void* p) {
    core::mem::deallocate(p);
}

auto c_realloc(void* p, std::size_t size) -> void* {
    const core::mem::TagScope scope{tag()};
    return core::mem::reallocate(p, size);
}

auto c_strdup(const char* s) -> char* {
    const std::size_t n = std::strlen(s) + 1;
    auto* p = static_cast<char*>(c_malloc(n));
    if (p != nullptr) {
        std::memcpy(p, s, n);
    }
    return p;
}

auto c_calloc(std::size_t count, std::size_t size) -> void* {
    if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
        return nullptr;
    }
    void* p = c_malloc(count * size);
    if (p != nullptr) {
        std::memset(p, 0, count * size);
    }
    return p;
}

} // namespace

auto global() -> core::Result<void> {
    // Once, by whichever thread comes first (the others wait). libcurl first, the way the
    // delay-load helper looks for it (and its nghttp2 and zlib): missing, that is an error here
    // instead of the delay-load exception (0xC06D007E) that would end the process at the first
    // call — __HrLoadAllImportsForDll raises it too. Loaded, the helper finds it; it stays.
    static const core::Result<void> ready = []() -> core::Result<void> {
        if (LoadLibraryExW(WA_CURL_DLL, nullptr, 0) == nullptr) {
            return core::os_error(GetLastError(), WA_CURL_DLL_NAME " could not be loaded (or what it needs: nghttp2, zlib)");
        }
        const CURLcode code = curl_global_init_mem(CURL_GLOBAL_DEFAULT, c_malloc, c_free, c_realloc, c_strdup, c_calloc);
        if (code != CURLE_OK) {
            return core::make_error(core::Code::Net, std::string{"curl_global_init: "} + curl_easy_strerror(code));
        }
        return {};
    }();
    return ready;
}

auto features() -> Features {
    const curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
    if (info == nullptr) {
        return {};
    }
    const std::string_view tls = info->ssl_version != nullptr ? info->ssl_version : "";
    return {.ssl = (info->features & CURL_VERSION_SSL) != 0,
            .schannel = tls.find("Schannel") != std::string_view::npos,
            .http2 = (info->features & CURL_VERSION_HTTP2) != 0,
            .version = info->version != nullptr ? info->version : ""};
}

} // namespace net::curl
