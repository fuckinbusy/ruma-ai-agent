#pragma once

// What agent-reader.exe reads with administrator rights and hands to the agent over its pipe
// (phase 13a § 6), as JSON: plain structs, written by the helper and read by the agent with the
// same names. Only facts: the agent judges them (threats.cpp), the helper never does.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tools::winsys::reader {

// Diagnostics-Performance, event 100: one boot, how long it took.
struct Boot {
    std::int64_t at = 0;       // seconds since 1970, UTC
    std::uint32_t boot_ms = 0;
};
// Diagnostics-Performance, event 101: a program that slowed the start, by how much.
struct SlowStart {
    std::string name, file;    // its friendly name, its file
    std::uint32_t total_ms = 0, degradation_ms = 0;
    std::int64_t at = 0;
};
// Defender's history of threats (MSFT_MpThreatDetection with its MSFT_MpThreat name).
struct Threat {
    std::string name, path;
    bool resolved = false;
    std::int64_t at = 0;
};
// A task of the Task Scheduler that runs as SYSTEM (or another account than the user's).
struct Task {
    std::string path, account, command;
    bool enabled = true;
};
// A process the user could not look into: its image path, read with the administrator's token.
struct Process {
    std::uint32_t pid = 0;
    std::string name, path; // path empty: not readable even so (protected)
};
// A system folder closed to the user: its size.
struct Folder {
    std::string path;
    std::uint64_t bytes = 0;
    std::uint32_t files = 0, not_read = 0; // not_read: entries even the administrator cannot open
};

struct Data {
    int version = 1;
    bool elevated = false; // the helper's own token: false only in the tests' run
    std::vector<Boot> boots;
    std::vector<SlowStart> slow_starts;
    bool events_read = false;
    std::vector<Threat> threats;
    bool threats_read = false;
    std::vector<Task> tasks;
    bool tasks_read = false;
    std::vector<Process> processes;
    std::vector<Folder> folders;
    std::vector<std::string> exclusions; // Defender's exclusions: "Paths: C:\x", "Processes: y.exe"
    bool exclusions_read = false;
    std::vector<std::string> not_read;   // what even the helper could not read, in words
};

// The pipe's name: this prefix and 32 hex digits, made by the agent for one run.
inline constexpr std::wstring_view kPipePrefix = L"\\\\.\\pipe\\WinAiAgent-reader-";
// The helper's lifetime at most, whatever happens (§ 6: no more than 5 minutes).
inline constexpr std::uint32_t kLimitMs = 5 * 60 * 1000;

// The helper's exit codes.
inline constexpr int kExitDone = 0;
inline constexpr int kExitUsage = 1;
inline constexpr int kExitNoAgent = 2;     // no pipe: no agent waiting
inline constexpr int kExitNotAgent = 3;    // the pipe is served by another program
inline constexpr int kExitTimeLimit = 4;   // the limit came first

} // namespace tools::winsys::reader
