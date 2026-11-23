#include "tools/winsys/net.hpp"

#include "core/utf8.hpp"
#include "core/win.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <tlhelp32.h>
#include <winhttp.h>

#include <fstream>
#include <map>
#include <sstream>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")

namespace tools::winsys {
namespace {

auto state_name(DWORD s) -> std::string {
    switch (s) {
    case MIB_TCP_STATE_LISTEN: return "listening";
    case MIB_TCP_STATE_ESTAB: return "established";
    case MIB_TCP_STATE_TIME_WAIT: return "time wait";
    case MIB_TCP_STATE_CLOSE_WAIT: return "close wait";
    case MIB_TCP_STATE_SYN_SENT: return "connecting";
    case MIB_TCP_STATE_SYN_RCVD: return "accepting";
    case MIB_TCP_STATE_FIN_WAIT1:
    case MIB_TCP_STATE_FIN_WAIT2:
    case MIB_TCP_STATE_CLOSING:
    case MIB_TCP_STATE_LAST_ACK: return "closing";
    default: return "closed";
    }
}

auto address(int family, const void* addr) -> std::string {
    char buf[INET6_ADDRSTRLEN]{};
    return inet_ntop(family, addr, buf, sizeof(buf)) != nullptr ? std::string{buf} : std::string{};
}

auto process_names() -> std::map<std::uint32_t, std::string> {
    std::map<std::uint32_t, std::string> out;
    const core::win::FileHandle snap{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
    if (!snap) {
        return out;
    }
    PROCESSENTRY32W e{sizeof(e)};
    for (BOOL ok = Process32FirstW(snap.get(), &e); ok; ok = Process32NextW(snap.get(), &e)) {
        out[e.th32ProcessID] = core::utf8::to_utf8(e.szExeFile).value_or("");
    }
    return out;
}

auto table(DWORD (*get)(PVOID, PDWORD, BOOL, ULONG, int, ULONG), ULONG family, int kind) -> std::vector<std::byte> {
    DWORD size = 0;
    get(nullptr, &size, FALSE, family, kind, 0);
    std::vector<std::byte> buf(size + 4096);
    size = static_cast<DWORD>(buf.size());
    if (get(buf.data(), &size, FALSE, family, kind, 0) != NO_ERROR) {
        buf.clear();
    }
    return buf;
}

auto tcp_get(PVOID t, PDWORD s, BOOL o, ULONG f, int k, ULONG r) -> DWORD {
    return GetExtendedTcpTable(t, s, o, f, static_cast<TCP_TABLE_CLASS>(k), r);
}
auto udp_get(PVOID t, PDWORD s, BOOL o, ULONG f, int k, ULONG r) -> DWORD {
    return GetExtendedUdpTable(t, s, o, f, static_cast<UDP_TABLE_CLASS>(k), r);
}

auto trim(std::string_view s) -> std::string_view {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

} // namespace

auto connections() -> std::vector<Connection> {
    std::vector<Connection> out;
    const auto names = process_names();
    const auto name_of = [&](std::uint32_t pid) {
        const auto it = names.find(pid);
        return it == names.end() ? std::string{} : it->second;
    };
    if (auto b = table(&tcp_get, AF_INET, TCP_TABLE_OWNER_PID_ALL); !b.empty()) {
        const auto* t = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(b.data());
        for (DWORD i = 0; i < t->dwNumEntries; ++i) {
            const auto& r = t->table[i];
            out.push_back({.proto = "tcp", .local = address(AF_INET, &r.dwLocalAddr), .remote = address(AF_INET, &r.dwRemoteAddr),
                           .local_port = ntohs(static_cast<u_short>(r.dwLocalPort)), .remote_port = ntohs(static_cast<u_short>(r.dwRemotePort)),
                           .state = state_name(r.dwState), .pid = r.dwOwningPid, .process = name_of(r.dwOwningPid)});
        }
    }
    if (auto b = table(&tcp_get, AF_INET6, TCP_TABLE_OWNER_PID_ALL); !b.empty()) {
        const auto* t = reinterpret_cast<const MIB_TCP6TABLE_OWNER_PID*>(b.data());
        for (DWORD i = 0; i < t->dwNumEntries; ++i) {
            const auto& r = t->table[i];
            out.push_back({.proto = "tcp", .local = address(AF_INET6, r.ucLocalAddr), .remote = address(AF_INET6, r.ucRemoteAddr),
                           .local_port = ntohs(static_cast<u_short>(r.dwLocalPort)), .remote_port = ntohs(static_cast<u_short>(r.dwRemotePort)),
                           .state = state_name(r.dwState), .pid = r.dwOwningPid, .process = name_of(r.dwOwningPid)});
        }
    }
    if (auto b = table(&udp_get, AF_INET, UDP_TABLE_OWNER_PID); !b.empty()) {
        const auto* t = reinterpret_cast<const MIB_UDPTABLE_OWNER_PID*>(b.data());
        for (DWORD i = 0; i < t->dwNumEntries; ++i) {
            const auto& r = t->table[i];
            out.push_back({.proto = "udp", .local = address(AF_INET, &r.dwLocalAddr), .remote = {},
                           .local_port = ntohs(static_cast<u_short>(r.dwLocalPort)), .remote_port = 0, .state = {}, .pid = r.dwOwningPid,
                           .process = name_of(r.dwOwningPid)});
        }
    }
    return out;
}

auto hosts_extra(std::string_view hosts) -> std::string {
    std::string out;
    std::size_t at = 0;
    while (at < hosts.size()) {
        auto nl = hosts.find('\n', at);
        if (nl == std::string_view::npos) {
            nl = hosts.size();
        }
        auto line = hosts.substr(at, nl - at);
        at = nl + 1;
        if (const auto hash = line.find('#'); hash != std::string_view::npos) {
            line = line.substr(0, hash);
        }
        line = trim(line);
        if (line.empty()) {
            continue;
        }
        // Only localhost names on it: what every hosts file has.
        std::istringstream words{std::string{line}};
        std::string ip, name;
        words >> ip;
        bool only_localhost = true;
        while (words >> name) {
            only_localhost = only_localhost && name == "localhost";
        }
        if (only_localhost) {
            continue;
        }
        out += (out.empty() ? "" : "\n") + std::string{line};
    }
    return out;
}

auto net_settings() -> NetSettings {
    NetSettings s;
    wchar_t windir[MAX_PATH]{};
    if (GetWindowsDirectoryW(windir, MAX_PATH) > 0) {
        std::ifstream f{std::wstring{windir} + L"\\System32\\drivers\\etc\\hosts", std::ios::binary};
        std::ostringstream text;
        text << f.rdbuf();
        s.hosts_extra = hosts_extra(text.str());
    }
    DWORD on = 0, size = sizeof(on);
    constexpr const wchar_t* kInternet = L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings";
    if (RegGetValueW(HKEY_CURRENT_USER, kInternet, L"ProxyEnable", RRF_RT_REG_DWORD, nullptr, &on, &size) == ERROR_SUCCESS && on != 0) {
        wchar_t server[512]{};
        DWORD bytes = sizeof(server);
        if (RegGetValueW(HKEY_CURRENT_USER, kInternet, L"ProxyServer", RRF_RT_REG_SZ, nullptr, server, &bytes) == ERROR_SUCCESS) {
            s.proxy = core::utf8::to_utf8(server).value_or("");
        }
    }
    WINHTTP_PROXY_INFO wp{};
    if (WinHttpGetDefaultProxyConfiguration(&wp)) {
        const core::win::GlobalPtr<LPWSTR> proxy{wp.lpszProxy}, bypass{wp.lpszProxyBypass};
        if (wp.dwAccessType == WINHTTP_ACCESS_TYPE_NAMED_PROXY && wp.lpszProxy != nullptr) {
            s.winhttp_proxy = core::utf8::to_utf8(wp.lpszProxy).value_or("");
        }
    }
    ULONG bytes = 0;
    GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, nullptr, nullptr, &bytes);
    std::vector<std::byte> buf(bytes + 1024);
    bytes = static_cast<ULONG>(buf.size());
    auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, nullptr, a, &bytes) == NO_ERROR) {
        for (; a != nullptr; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp) {
                continue;
            }
            for (auto* d = a->FirstDnsServerAddress; d != nullptr; d = d->Next) {
                const auto* sa = d->Address.lpSockaddr;
                const auto text = sa->sa_family == AF_INET ? address(AF_INET, &reinterpret_cast<const sockaddr_in*>(sa)->sin_addr)
                                                           : address(AF_INET6, &reinterpret_cast<const sockaddr_in6*>(sa)->sin6_addr);
                if (!text.empty() && std::ranges::find(s.dns, text) == s.dns.end()) {
                    s.dns.push_back(text);
                }
            }
        }
    }
    return s;
}

} // namespace tools::winsys
