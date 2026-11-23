#pragma once

// tools::jobs — shell commands in the background (phase 12 § 8): shell.run with background:true
// starts one and answers its number at once; shell.job reads what it printed since a position
// and whether it ended (its exit code); shell.stop ends it with everything it started. Each runs
// as shell.run does (os::Backend::run_shell: the Job Object, the fence), on a thread of its own,
// for at most 2 hours; at most 3 at once per task; the registry's end ends them all.

#include "core/error.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>

namespace tools::jobs {

inline constexpr std::size_t kMaxAtOnce = 3;          // per task
inline constexpr int kMaxSeconds = 2 * 60 * 60;       // then it is ended
inline constexpr std::size_t kMaxKept = 4u << 20;     // output kept; past it cut, with a mark
inline constexpr std::size_t kMaxRead = 64 * 1024;    // one read's output
inline constexpr std::size_t kKeptEnded = 16;         // ended commands kept to be read; the oldest go

struct State {
    std::uint64_t id = 0;
    std::string command;
    bool running = true;
    int code = 0;           // when it ended
    bool timed_out = false; // ended by the 2-hour limit
    bool stopped = false;   // ended by shell.stop, a cancel or the agent's end
    bool cut = false;       // more than kMaxKept printed: the rest was not kept
    std::string output;     // from the read's `since`, at most kMaxRead
    std::size_t next = 0;   // where the next read starts
};

class Registry {
public:
    explicit Registry(int max_seconds = kMaxSeconds, std::size_t kept_ended = kKeptEnded);
    ~Registry(); // ends every command, waits for them
    Registry(const Registry&) = delete;
    auto operator=(const Registry&) -> Registry& = delete;

    // Called when a command ends, on its thread (not after the registry's end began).
    void on_finished(std::function<void(std::uint64_t task, const State&)> f);
    auto start(std::uint64_t task, std::wstring command, std::wstring working_dir) -> core::Result<std::uint64_t>;
    // A command of `task`: its state, its output from `since`.
    [[nodiscard]] auto read(std::uint64_t task, std::uint64_t job, std::size_t since) const -> core::Result<State>;
    auto stop(std::uint64_t task, std::uint64_t job) -> core::Result<void>;
    // The task's commands asked to end (the task cancelled); they end shortly, never waited for
    // here: the caller may hold a lock their on_finished takes.
    void stop_task(std::uint64_t task);
    // No more on_finished; every command ended and waited for. The destructor's, and the owner's
    // before what on_finished uses goes.
    void end();
    [[nodiscard]] auto running(std::uint64_t task) const -> std::size_t;

private:
    struct Job {
        std::uint64_t task = 0;
        State state;
        std::string output; // all of it kept, up to kMaxKept
        std::jthread thread; // its stop token ends the command
        std::atomic<bool> done{false}; // its thread past everything it does: joined at once
    };
    void run(Job& job, std::wstring command, std::wstring working_dir, std::stop_token stop);
    [[nodiscard]] auto running_locked(std::uint64_t task) const -> std::size_t;

    int max_seconds_;
    std::size_t kept_ended_;
    mutable std::mutex mutex_;
    std::map<std::uint64_t, std::unique_ptr<Job>> jobs_;
    std::uint64_t last_ = 0;
    std::function<void(std::uint64_t, const State&)> finished_;
    bool ending_ = false;
};

} // namespace tools::jobs
