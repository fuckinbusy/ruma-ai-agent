#pragma once

// tools::winsys::services — Windows' services, looked at only (phase 13a, rule 1): the manager is
// opened to enumerate and each service to query its configuration; nothing here can change one.

#include <string>
#include <vector>

namespace tools::winsys {

struct ServiceInfo {
    std::string name;
    std::string display;
    std::string state;      // running, stopped, start pending, stop pending, paused, …
    std::string start_type; // automatic, automatic (delayed), manual, disabled, boot, system
    std::wstring command;   // its binary path as configured
    std::wstring file;      // file_of(command)
    std::string account;
    bool driver = false;
    std::wstring dll;       // a service inside svchost: its Parameters\ServiceDll, expanded
};

[[nodiscard]] auto services() -> std::vector<ServiceInfo>;

} // namespace tools::winsys
