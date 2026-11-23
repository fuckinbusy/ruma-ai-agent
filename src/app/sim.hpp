#pragma once

// app::sim — the simulated agent of phase 3: plays the mockups' scenarios with realistic timing so
// the whole path UI <-> state <-> events runs without a network or a model.
//
// Player is the scenario engine, pure: commands and time go in, timed events come out (tests run
// it with a fake clock). Agent runs a Player on its own thread and posts events to the UI.

#include "app/agent_tools.hpp"
#include "app/events.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace app::sim {

class Player {
public:
    // `speed` divides every delay (tests and demos); `seed` makes the token pace reproducible.
    explicit Player(std::uint64_t seed = 1, double speed = 1.0) : random_(seed), speed_(speed) {}

    void command(const events::Command& command, double now);
    // Moves the events due by `now` into `out` (cleared first); returns when the next event is
    // due, or infinity when nothing is scheduled.
    auto advance(double now, std::vector<events::Event>& out) -> double;

    // The scenario a request plays (by keywords); exposed for the tray's "Try" menu later.
    static auto scenario_for(std::string_view request) -> std::string_view;
    // The scenario task `task` plays, empty when it plays none (the agent's own programs).
    [[nodiscard]] auto scenario_of(std::uint64_t task) const -> std::string_view;

private:
    // Several tasks at once (phase 5): each event knows its task, the queue is in time order.
    struct Due {
        double time;
        std::uint64_t task;
        events::AgentEvent event;
    };
    void schedule(std::uint64_t task, std::string_view script, double now);
    void drop(std::uint64_t task);

    std::mt19937_64 random_;
    double speed_;
    std::vector<std::pair<std::uint64_t, std::string>> scenarios_; // by task
    std::deque<Due> queue_;
};

// The agent. With a Host, some requests run real tools (phase 4): "why is my computer slow"
// reads system.info and process.list; "create a folder X" runs fs.mkdir, and its Undo fs.trash —
// every question through the policy, every answer from the user's click. Each such task runs on
// a thread of its own (phase 5): several at once, each paused, resumed and stopped by itself; a
// watcher lives on the monitor until switched off. The rest plays the scripted scenarios.
class Agent {
public:
    using Post = std::function<void(events::Event)>;

    explicit Agent(Post post, double speed = 1.0, std::optional<agent::Host> host = std::nullopt);
    ~Agent();
    Agent(const Agent&) = delete;
    Agent& operator=(const Agent&) = delete;

    // From the UI thread; never blocks for long.
    void send(events::Command command);
    // The largest task id in the journal (0 without tools): the sheet's ids start after it.
    [[nodiscard]] auto last_task() const -> std::uint64_t;

private:
    // A task that runs tools, on its own thread.
    struct Job {
        std::uint64_t task = 0;
        std::string scenario;
        std::vector<std::pair<std::uint32_t, std::string>> closable; // "slow": the programs its report offers to close
        std::vector<events::Answer> answers; // under mutex_
        bool cancelled = false;              // under mutex_: stopped, or its question replaced
        std::atomic<bool> paused{false};     // held before the next object of a batch
        std::stop_token stop;                // its thread's
        std::jthread thread;
        // A watcher: what it watches and its place on the monitor while on.
        std::optional<std::uint64_t> watch;
        monitor::Metric metric = monitor::Metric::Memory;
        float above = 90;
        std::string what; // "memory"
        auto control() -> tools::Control;
    };

    void run(std::stop_token stop);
    void dispatch(events::Command c, double now);
    // Whether `c` runs tools (on a task's thread, waiting for answers there).
    [[nodiscard]] auto is_program(const events::Command& c, const Job* job) const -> bool;
    auto job_of(std::uint64_t task) -> Job*; // with mutex_ held
    void start(Job& job, events::Command command);
    void stop_job(Job& job);
    void run_program(Job& job, const events::Command& c);
    // The system report on the machine's own numbers (`said`: what was just done, before the
    // sentence); Close of a program it offered (process.kill, asked), then the report again.
    void slow(Job& job, std::string said = {});
    void close_program(Job& job, std::uint32_t pid);
    // "How much space is left on C?": the drive's own numbers as a quick answer.
    void space(Job& job);
    void make_folder(Job& job, std::string_view request);
    void sort_desktop(Job& job);
    // Takes back journal task `target`'s changes (one, or all) through the tools, asking as the job.
    void undo(Job& job, std::uint64_t target, std::optional<std::size_t> change);
    // "Watch memory and warn me above 90%": a watch on the monitor; an Alert each time it goes over.
    void watch(Job& job, std::string_view request);
    void switch_watch(Job& job, bool on);
    // Posts the question and waits for its answer; the task stopped, its question replaced or the
    // agent's end is no answer.
    auto ask(Job& job, const events::Permission& p) -> std::optional<events::Answer>;
    // A tool call of the job: asked through it, held and stopped by it.
    // `progress`: after each object of a batch (the desktop's highlight follows the files).
    // `output`: a command's output as it comes (the terminal's tab).
    auto call(Job& job, std::string_view name, std::string_view args, std::function<void(const tools::ProgressInfo&)> progress = {},
              std::function<void(std::string_view)> output = {}) -> tools::Called;
    // "Count to 5 in the terminal": a real command in the agent's PowerShell tab, its output streamed.
    void count(Job& job, std::string_view request);
    // "Clean up temp files and delete old installers from Downloads": its steps as cards, a card
    // with the files before each delete (to the Recycle Bin), a summary, Undo.
    void cleanup(Job& job);
    // "Find duplicate photos in Pictures": played (no tools) — the plan and the terminal's lines.
    void duplicates(Job& job);
    // Waits while the user holds the task (Pause, Take over).
    void wait_paused(Job& job);
    [[nodiscard]] auto over(const Job& job) -> bool;
    // Waits `seconds` / speed, or less when stopped.
    void pause(Job& job, double seconds);

    Post post_;
    double speed_;
    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    std::vector<events::Command> inbox_;
    Player player_;
    std::optional<agent::Tools> tools_;
    std::vector<std::unique_ptr<Job>> jobs_; // under mutex_; a job stays while the agent lives
    std::jthread thread_;                    // last: started after everything it uses
};

} // namespace app::sim
