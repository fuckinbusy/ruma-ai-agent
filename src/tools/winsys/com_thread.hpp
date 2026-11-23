#pragma once

// tools::winsys::on_own_mta — COM work (WMI, the Task Scheduler) on a short-lived thread of its own,
// in a multithreaded apartment it opens and closes: whatever apartment the caller's thread is in,
// COM's worker threads end with it instead of living on in the caller's.

#include <objbase.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace tools::winsys {

template <class F>
auto on_own_mta(F&& f) -> decltype(f()) {
    decltype(f()) out{};
    std::jthread t{[&] {
        const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        out = std::forward<F>(f)();
        if (com) {
            CoUninitialize();
        }
    }};
    t.join();
    return out;
}

// The same, waited for `limit` at most: nothing when it has not ended by then. Its thread is not
// waited for: it ends by itself later and its result is dropped, so `f` owns what it uses (taken
// by value: nothing of the caller's stack outlives the wait).
template <class F>
auto on_own_mta_within(std::chrono::milliseconds limit, F f) -> std::optional<decltype(f())> {
    using R = decltype(f());
    struct State {
        std::mutex m;
        std::condition_variable ended;
        std::optional<R> out;
    };
    auto s = std::make_shared<State>();
    std::thread{[s, f = std::move(f)]() mutable {
        const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        R r = f();
        if (com) {
            CoUninitialize();
        }
        {
            const std::lock_guard lock{s->m};
            s->out = std::move(r);
        }
        s->ended.notify_one();
    }}.detach();
    std::unique_lock lock{s->m};
    s->ended.wait_for(lock, limit, [&] { return s->out.has_value(); });
    return s->out;
}

} // namespace tools::winsys
