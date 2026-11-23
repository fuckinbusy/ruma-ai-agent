#pragma once

// app::report — the system report and the disk's quick answer, made from what system.info and
// process.list read and from the monitor's last minute (docs/plans/phase-5.md, Task 10). Pure:
// the numbers come in, the screen's words go out.

#include "app/events.hpp"
#include "app/monitor.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace app::report {

// What system.info said.
struct Machine {
    double cpu_percent = 0;
    std::uint64_t memory_total = 0;
    std::uint64_t memory_available = 0;
    std::uint64_t disk_total = 0; // the system drive
    std::uint64_t disk_free = 0;
    std::string gpu_name;
};

// One line of process.list.
struct Proc {
    std::uint32_t pid = 0;
    std::string name;
    std::uint64_t memory = 0;
    double cpu = 0;
    bool readable = true;
};

// The tiles (Memory, Processor, Graphics, Disk C:), the top five programs by memory (a
// program's processes together; `self`, Windows' own and a many-process program not closable by
// one Close), one piece of advice per program worth closing, and the sentence.
auto make_report(const Machine& m, std::span<const Proc> processes, std::size_t process_count,
                 const monitor::Snapshot& minute, const monitor::Temperature& temperature, std::uint32_t self) -> events::Report;

// The report for the model (responsive text; the program's English first line
// over a Russian answer, its processor number not the model's): `tool_json`, the result of the call
// that made the report, with "report_summary" — its sentence as plain words and each tile's name,
// value, unit and state — for the model to begin its answer with, in the user's language.
auto with_report_summary(std::string_view tool_json, const events::Report& r) -> std::string;

// While the report is on screen: the tiles' lines, and the processor's, memory's and graphics
// card's values, from the monitor's newest reading (none yet: unchanged).
void refresh(events::Report& r, const monitor::Snapshot& now);

// "How much space is left on C?": free of total, the used share as the bar; `seconds` it took.
auto disk_quick(const Machine& m, double seconds) -> events::Quick;

// A graphics card's name as its tile has room for: "AMD Radeon(TM) 780M" -> "Radeon 780M",
// "NVIDIA GeForce RTX 4060" -> "RTX 4060" (the maker and the marks go, the model stays).
auto gpu_short(std::string_view name) -> std::string;

// system.info's answer (its JSON, as the tool returned it: {"source", "data": {...}}) as the
// report takes it: Windows' own drive, the first real graphics card. None: not system.info's.
auto machine_from_json(std::string_view system_info) -> std::optional<Machine>;

} // namespace app::report
