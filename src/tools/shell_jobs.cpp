#include "tools/shell_jobs.hpp"

#include "core/utf8.hpp"
#include "tools/os.hpp"

#include <algorithm>
#include <format>
#include <vector>

namespace tools::jobs {
namespace {

// `n` bytes into `s`, moved back to a character's start (never in the middle of one).
auto boundary(std::string_view s, std::size_t n) -> std::size_t {
    n = std::min(n, s.size());
    while (n > 0 && n < s.size() && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) {
        --n;
    }
    return n;
}

} // namespace

Registry::Registry(int max_seconds, std::size_t kept_ended) : max_seconds_(max_seconds), kept_ended_(kept_ended) {}

Registry::~Registry() {
    end();
}

void Registry::end() {
    std::vector<std::unique_ptr<Job>> ending;
    {
        const std::scoped_lock lock{mutex_};
        ending_ = true;
        finished_ = nullptr;
        for (auto& [id, job] : jobs_) {
            job->thread.request_stop();
            ending.push_back(std::move(job));
        }
        jobs_.clear();
    }
    ending.clear(); // each thread joined: its command ended with everything it started
}

void Registry::on_finished(std::function<void(std::uint64_t, const State&)> f) {
    const std::scoped_lock lock{mutex_};
    finished_ = std::move(f);
}

auto Registry::start(std::uint64_t task, std::wstring command, std::wstring working_dir) -> core::Result<std::uint64_t> {
    std::vector<std::unique_ptr<Job>> dropped; // joined after the lock: their threads have done all they do
    const std::scoped_lock lock{mutex_};
    // Ended commands past kept_ended_, the oldest first (final review J1): their output and thread go.
    std::size_t ended = static_cast<std::size_t>(std::ranges::count_if(jobs_, [](const auto& kv) { return kv.second->done.load(); }));
    for (auto it = jobs_.begin(); it != jobs_.end() && ended > kept_ended_;) {
        if (it->second->done) {
            dropped.push_back(std::move(it->second));
            it = jobs_.erase(it);
            --ended;
        } else {
            ++it;
        }
    }
    if (ending_) {
        return core::make_error(core::Code::Cancelled, "the agent is closing");
    }
    if (running_locked(task) >= kMaxAtOnce) {
        return core::make_error(core::Code::Policy, std::format("at most {} background commands at once: wait for one, or end it "
                                                                "with shell.stop",
                                                                kMaxAtOnce));
    }
    auto job = std::make_unique<Job>();
    job->task = task;
    job->state.id = ++last_;
    auto text = core::utf8::to_utf8(command);
    job->state.command = text ? std::move(*text) : std::string{"?"};
    Job& j = *job;
    jobs_.emplace(j.state.id, std::move(job));
    j.thread = std::jthread{[this, &j, command = std::move(command), working_dir = std::move(working_dir)](std::stop_token stop) mutable {
        run(j, std::move(command), std::move(working_dir), stop);
        j.done = true;
    }};
    return j.state.id;
}

void Registry::run(Job& job, std::wstring command, std::wstring working_dir, std::stop_token stop) {
    const auto keep = [this, &job](std::string_view text) {
        const std::scoped_lock lock{mutex_};
        const std::size_t room = kMaxKept - std::min(kMaxKept, job.output.size());
        if (text.size() > room) {
            job.output.append(text.substr(0, boundary(text, room)));
            if (!job.state.cut) {
                job.state.cut = true;
                job.output += "\n[more output was not kept]\n";
            }
            return;
        }
        job.output.append(text);
    };
    // Its processor time no more than its wall time, as shell.run's.
    auto r = os::current().run_shell(command, working_dir, max_seconds_, max_seconds_, {.output = keep, .stop = stop});
    std::function<void(std::uint64_t, const State&)> finished;
    State ended;
    {
        const std::scoped_lock lock{mutex_};
        job.state.running = false;
        if (r) {
            job.state.code = r->code;
            job.state.timed_out = r->timed_out;
            job.state.stopped = r->stopped || stop.stop_requested();
            if (job.output.empty()) { // a backend that gives it only at the end
                job.output = r->out + r->err;
            }
        } else {
            job.state.code = -1;
            job.output += std::format("\n[{}]\n", r.error().message);
        }
        ended = job.state;
        if (!ending_) {
            finished = finished_;
        }
    }
    if (finished) {
        finished(job.task, ended);
    }
}

auto Registry::read(std::uint64_t task, std::uint64_t job, std::size_t since) const -> core::Result<State> {
    const std::scoped_lock lock{mutex_};
    const auto it = jobs_.find(job);
    if (it == jobs_.end() || it->second->task != task) {
        return core::make_error(core::Code::NotFound, std::format("no background command {} in this task", job));
    }
    const auto& j = *it->second;
    State s = j.state;
    const std::size_t from = boundary(j.output, std::min(since, j.output.size()));
    const std::size_t to = j.output.size() - from > kMaxRead ? boundary(j.output, from + kMaxRead) : j.output.size();
    s.output = j.output.substr(from, to - from);
    s.next = to;
    return s;
}

auto Registry::stop(std::uint64_t task, std::uint64_t job) -> core::Result<void> {
    const std::scoped_lock lock{mutex_};
    const auto it = jobs_.find(job);
    if (it == jobs_.end() || it->second->task != task) {
        return core::make_error(core::Code::NotFound, std::format("no background command {} in this task", job));
    }
    it->second->thread.request_stop(); // its command ends with everything it started
    return {};
}

void Registry::stop_task(std::uint64_t task) {
    const std::scoped_lock lock{mutex_};
    for (auto& [id, job] : jobs_) {
        if (job->task == task) {
            job->thread.request_stop();
        }
    }
}

auto Registry::running(std::uint64_t task) const -> std::size_t {
    const std::scoped_lock lock{mutex_};
    return running_locked(task);
}

auto Registry::running_locked(std::uint64_t task) const -> std::size_t {
    std::size_t n = 0;
    for (const auto& [id, job] : jobs_) {
        n += job->task == task && job->state.running ? 1 : 0;
    }
    return n;
}

} // namespace tools::jobs
