#pragma once

// The agent's side of agent-reader.exe (phase 13a § 6): a pipe with a random name that only the
// user and administrators may open, the helper started with it, its JSON read back. Started only
// by the user's own press (Check deeper); the model can suggest it, never start it.

#include "core/error.hpp"
#include "tools/winsys/reader_data.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace tools::winsys::reader {

// Starts `exe` with `args`; the process handle (the caller closes it).
using Launch = std::function<core::Result<void*>(const std::wstring& exe, const std::wstring& args)>;

// `exe` can be changed by no one but administrators: neither the file nor its folder is writable
// with this user's token (AccessCheck). Otherwise another program of the user could put its own
// helper there, and the UAC prompt would raise it (final review C2).
auto protected_location(const std::filesystem::path& exe) -> core::Result<void>;

// ShellExecuteExW "runas": Windows' own UAC prompt. Behind the fence: a test that reaches it ends.
auto launch_elevated(const std::wstring& exe, const std::wstring& args) -> core::Result<void*>;
// The same program started as the user, its manifest's administrator set aside (Windows'
// RunAsInvoker layer): the tests' way to run the helper without a prompt.
auto launch_as_invoker(const std::wstring& exe, const std::wstring& args) -> core::Result<void*>;

// One run: the pipe made, `exe` launched with it, the client checked to be `exe` itself, its JSON
// read (at most `limit_ms`, then a little for the helper to end itself).
auto run(const std::filesystem::path& exe, const Launch& launch, std::uint32_t limit_ms = kLimitMs) -> core::Result<Data>;

} // namespace tools::winsys::reader
