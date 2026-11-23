#pragma once

// tools::winsys — what crashed lately, from the System and Application logs (phase 13a § 1):
// programs that crashed or hung, services that died, unclean shutdowns; grouped and counted.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace tools::winsys {

struct RawEvent {
    std::string provider;
    std::uint32_t id = 0;
    std::uint8_t level = 0; // 1 critical, 2 error, 3 warning
    std::int64_t at = 0;    // seconds since 1970, UTC
    std::string message;
};

struct EventGroup {
    std::string provider;
    std::uint32_t id = 0;
    std::uint8_t level = 0;
    std::size_t count = 0;
    std::int64_t last = 0;
    std::string message; // the newest one's
};

struct EventReport {
    bool read = false;
    std::vector<EventGroup> groups; // the most often first; at most 50
    std::size_t total = 0;          // events in the period
    std::vector<std::string> not_read;
};

// The groups of `raw` within `days` before `now`.
[[nodiscard]] auto summarize(std::span<const RawEvent> raw, int days, std::int64_t now) -> EventReport;
// The live logs: Application Error 1000 and 1002, Windows Error Reporting 1001, Service Control
// Manager 7031 / 7034, Kernel-Power 41, EventLog 6008; `days` 1..30.
[[nodiscard]] auto crashes(int days) -> EventReport;

} // namespace tools::winsys
