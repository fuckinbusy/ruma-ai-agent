#pragma once

// The process calls of tools::os's real backend (os.cpp checks the fence first).

#include "tools/os.hpp"

namespace tools::os::detail {

auto run_shell(const std::wstring& command, const std::wstring& working_dir, int timeout_s, int cpu_s, const ShellIo& io)
    -> core::Result<ShellResult>;
auto kill(std::uint32_t pid) -> core::Result<void>;

} // namespace tools::os::detail
