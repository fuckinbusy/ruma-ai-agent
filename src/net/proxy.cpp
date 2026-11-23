#include "net/proxy.hpp"

#include "core/log.hpp"
#include "core/utf8.hpp"

#include <windows.h>
#include <winhttp.h>

#include <cctype>

namespace net {
namespace {

auto lower(char c) -> char {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

// `pattern` with * (any run) and ? (one character) against the whole of `text`, any case.
auto matches(std::string_view pattern, std::string_view text) -> bool {
    std::size_t p = 0, t = 0, star = std::string_view::npos, back = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || lower(pattern[p]) == lower(text[t]))) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            back = t;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            t = ++back;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }
    return p == pattern.size();
}

// The host of `url`, without user, port or brackets.
auto host_of(std::string_view url) -> std::string_view {
    if (const auto scheme = url.find("://"); scheme != std::string_view::npos) {
        url.remove_prefix(scheme + 3);
    }
    url = url.substr(0, url.find_first_of("/?#"));
    if (const auto at = url.rfind('@'); at != std::string_view::npos) {
        url.remove_prefix(at + 1);
    }
    if (url.starts_with('[')) {
        return url.substr(1, url.find(']') == std::string_view::npos ? std::string_view::npos : url.find(']') - 1);
    }
    return url.substr(0, url.find(':'));
}

auto trim(std::string_view s) -> std::string_view {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    return s;
}

// WinHTTP's strings are the caller's to GlobalFree.
struct Owned {
    LPWSTR p = nullptr;
    Owned() = default;
    Owned(const Owned&) = delete;
    auto operator=(const Owned&) -> Owned& = delete;
    ~Owned() {
        if (p != nullptr) {
            GlobalFree(p); // mem-ok: WinHttpGetIEProxyConfigForCurrentUser's strings; only GlobalFree frees them
        }
    }
};

} // namespace

auto system_settings() -> ProxySettings {
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG config{};
    if (!WinHttpGetIEProxyConfigForCurrentUser(&config)) {
        return {};
    }
    Owned pac, proxy, bypass;
    pac.p = config.lpszAutoConfigUrl;
    proxy.p = config.lpszProxy;
    bypass.p = config.lpszProxyBypass;
    return ProxySettings{.proxy = proxy.p != nullptr ? proxy_of_list(proxy.p) : std::string{},
                         .bypass = bypass.p != nullptr ? bypass_of_list(bypass.p) : std::vector<std::string>{},
                         .pac = pac.p != nullptr};
}

auto proxy_for(std::string_view url, const ProxySettings& s) -> std::string {
    if (s.proxy.empty() || s.pac) {
        return {};
    }
    const std::string_view host = host_of(url);
    for (const auto& entry : s.bypass) {
        if (entry == "<local>" ? host.find('.') == std::string_view::npos : matches(entry, host)) {
            return {};
        }
    }
    return s.proxy;
}

auto settings_of(std::string_view choice, const ProxySettings& system) -> ProxySettings {
    if (choice == "none") {
        return {};
    }
    if (choice.starts_with("http://")) {
        return ProxySettings{.proxy = std::string{choice}, .bypass = system.bypass, .pac = false};
    }
    if (system.pac) {
        WA_WARN("net", "Windows' proxy settings name a PAC script: it is not read, requests go direct "
                       "(network.proxy in config.json can name a proxy)");
    }
    return system;
}

auto follows_system(std::string_view choice) -> bool {
    return choice != "none" && !choice.starts_with("http://");
}

auto proxy_of_list(std::wstring_view list) -> std::string {
    const std::string text = core::utf8::to_utf8(list).value_or(std::string{});
    if (text.find('=') == std::string::npos) {
        return std::string{trim(text)};
    }
    std::string http, https;
    std::string_view rest = text;
    while (!rest.empty()) {
        const auto end = rest.find_first_of("; ");
        const std::string_view item = rest.substr(0, end);
        rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
        if (item.starts_with("https=")) {
            https = item.substr(6);
        } else if (item.starts_with("http=")) {
            http = item.substr(5);
        }
    }
    return !https.empty() ? https : http;
}

auto bypass_of_list(std::wstring_view list) -> std::vector<std::string> {
    const std::string text = core::utf8::to_utf8(list).value_or(std::string{});
    std::vector<std::string> out;
    std::string_view rest = text;
    while (!rest.empty()) {
        const auto end = rest.find(';');
        const std::string_view item = trim(rest.substr(0, end));
        rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
        if (!item.empty()) {
            out.emplace_back(item);
        }
    }
    return out;
}

} // namespace net
