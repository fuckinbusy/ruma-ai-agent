#pragma once

// app::instance — one agent per user session. The first process to create the named mutex is
// the agent; a later one asks it to open the sheet (app window message) and exits.

#include "core/error.hpp"
#include "core/win.hpp"

#include <string_view>

namespace app::instance {

inline constexpr wchar_t kName[] = L"Local\\WinAiAgent.Agent.SingleInstance";

class Instance {
public:
    explicit Instance(core::win::KernelHandle mutex, bool first)
        : mutex_(std::move(mutex)), first_(first) {}
    // True for the process that created the mutex.
    [[nodiscard]] auto first() const noexcept -> bool { return first_; }

private:
    core::win::KernelHandle mutex_; // held for the process lifetime
    bool first_;
};

auto acquire(std::wstring_view name = kName) -> core::Result<Instance>;

} // namespace app::instance
