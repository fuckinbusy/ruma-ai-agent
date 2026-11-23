#pragma once

// tools::winsys::defender_status — is Microsoft Defender there, on and up to date, and what it
// found (phase 13a § 5.1 stage 7): WMI root\Microsoft\Windows\Defender, ExecQuery only.

#include <cstdint>
#include <string>
#include <vector>

namespace tools::winsys {

struct Threat {
    std::string name;
    std::string path;
    bool resolved = false;
    std::int64_t at = 0;
};

struct DefenderStatus {
    bool read = false;
    std::string why_not;     // when it could not be read
    bool present = false;
    bool on = false;         // real-time protection
    bool up_to_date = false; // signatures under 7 days old
    std::int64_t signatures_at = 0;
    bool threats_read = false; // the history usually needs administrator rights
    std::vector<Threat> threats;
    std::vector<std::string> exclusions; // "Paths: C:\x" — administrators only: the helper reads them
};

[[nodiscard]] auto defender_status() -> DefenderStatus;

} // namespace tools::winsys
