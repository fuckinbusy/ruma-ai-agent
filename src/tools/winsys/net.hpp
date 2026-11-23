#pragma once

// tools::winsys — who talks to the network (each connection and listening port with its process)
// and how the network is set (hosts, proxy, DNS): phase 13a § 1, § 5.1 stage 6. Read only.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tools::winsys {

struct Connection {
    std::string proto; // tcp, udp
    std::string local, remote; // addresses
    std::uint16_t local_port = 0, remote_port = 0;
    std::string state;  // listening, established, time wait, … (udp: empty)
    std::uint32_t pid = 0;
    std::string process;
};

[[nodiscard]] auto connections() -> std::vector<Connection>;

struct NetSettings {
    std::string hosts_extra;            // the hosts file's lines past comments and localhost
    std::optional<std::string> proxy;   // the user's WinINet proxy, when on
    std::optional<std::string> winhttp_proxy;
    std::vector<std::string> dns;       // the adapters' DNS servers
};

[[nodiscard]] auto net_settings() -> NetSettings;
// A hosts file's lines that do something beyond localhost, comments and blank lines dropped.
[[nodiscard]] auto hosts_extra(std::string_view hosts) -> std::string;

} // namespace tools::winsys
