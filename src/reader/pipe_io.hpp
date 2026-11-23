#pragma once

// agent-reader.exe's only file it opens: the agent's pipe, for writing. Anything else — a file, a
// pipe of another name — is refused here, before Windows is asked.

#include <windows.h>

#include <string>

namespace reader {

// The pipe `name` (the agent's prefix and 32 hex digits), opened to write; INVALID_HANDLE_VALUE
// when the name is not such a pipe or no one serves it.
auto open_pipe(const std::wstring& name) -> HANDLE;

// Whether `name` is the agent's kind of pipe name.
auto agent_pipe_name(const std::wstring& name) -> bool;

} // namespace reader
