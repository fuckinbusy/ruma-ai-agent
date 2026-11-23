#pragma once

// net::proxy — the proxy Windows' Internet settings name, and the addresses that go around it
// (01-ARCHITECTURE § net). The user's machine has one on with a bypass list (127.*, localhost,
// <local>, …): the tests' mock server on 127.0.0.1 must be reached directly, the model's API
// through the proxy. A PAC script (AutoConfigURL) is not read: direct, said in the log.

#include <string>
#include <string_view>
#include <vector>

namespace net {

struct ProxySettings {
    std::string proxy;               // "host:port"; empty: none
    std::vector<std::string> bypass; // ProxyOverride's entries: names with * and ?, "<local>"
    bool pac = false;                // only a PAC script is set
};

// WinHttpGetIEProxyConfigForCurrentUser: the current user's settings. Nothing found: no proxy.
auto system_settings() -> ProxySettings;

// config.json's network.proxy over the system's settings: "none" — no proxy; "http://host:port"
// — that one, the system's bypass list kept; "system" — the system's (a PAC script: a warning in
// the log, then direct).
auto settings_of(std::string_view choice, const ProxySettings& system) -> ProxySettings;
// network.proxy as the network takes it: "system" (or anything not a choice of its own) - the
// system's settings read for each request; "none" or "http://host:port" - fixed.
auto follows_system(std::string_view choice) -> bool;

// The proxy for `url` ("" — direct): none, or the host matches a bypass entry (case-insensitive,
// the whole host; "<local>" — a name without a dot). The host is taken without user and port.
auto proxy_for(std::string_view url, const ProxySettings& s) -> std::string;

// Windows' proxy string: "host:port" for every scheme, or "http=h:p;https=h:p;…" (https wanted,
// then http; other schemes are not ours). And its bypass list, ";"-separated.
auto proxy_of_list(std::wstring_view list) -> std::string;
auto bypass_of_list(std::wstring_view list) -> std::vector<std::string>;

} // namespace net
