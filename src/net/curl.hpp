#pragma once

// net::curl — libcurl's handles as RAII owners, and its process-wide state (01-ARCHITECTURE § net).
// libcurl.dll is delay-loaded (02-LIBRARIES): nothing here touches it until global() is called,
// so a process that never makes a request never loads it.

#include "core/error.hpp"
#include "core/mem/handle.hpp"

#include <curl/curl.h>

#include <string>

namespace net::curl {

struct EasyTraits {
    using type = CURL*;
    static auto invalid() noexcept -> CURL* { return nullptr; }
    static void close(CURL* h) noexcept { curl_easy_cleanup(h); }
};
struct MultiTraits {
    using type = CURLM*;
    static auto invalid() noexcept -> CURLM* { return nullptr; }
    static void close(CURLM* h) noexcept { curl_multi_cleanup(h); }
};
struct SlistTraits {
    using type = curl_slist*;
    static auto invalid() noexcept -> curl_slist* { return nullptr; }
    static void close(curl_slist* h) noexcept { curl_slist_free_all(h); }
};

using Easy = core::Handle<EasyTraits>;
using Multi = core::Handle<MultiTraits>;
using Slist = core::Handle<SlistTraits>;

// curl_global_init_mem with core::mem's functions (tag "net/curl"), once per process, from any
// thread. curl_global_cleanup is never called: the state lives as long as the process, and the
// order in which delay-loaded DLLs are unloaded at exit is not ours to rely on.
auto global() -> core::Result<void>;

// What this libcurl was built with.
struct Features {
    bool ssl = false;
    bool schannel = false;
    bool http2 = false;
    std::string version; // "8.22.0"
};
auto features() -> Features;

} // namespace net::curl
