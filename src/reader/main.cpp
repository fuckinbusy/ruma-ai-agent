// agent-reader.exe (phase 13a § 6): started by the agent through Windows' UAC prompt, only when
// the user pressed Check deeper. It opens the agent's pipe, checks that agent.exe from its own
// folder serves it, reads (collect.cpp) and writes one JSON, and ends — within 5 minutes whatever
// happens. It changes nothing: its import table is what the tests read.
//
//   agent-reader.exe --pipe \\.\pipe\WinAiAgent-reader-<32 hex> [--limit-ms N]
//
// --limit-ms may only shorten the 5 minutes (the tests' way to see the limit).

#include "core/json.hpp"
#include "reader/collect.hpp"
#include "reader/pipe_io.hpp"
#include "tools/winsys/reader_data.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <string>
#include <string_view>
#include <thread>

namespace {

namespace rd = tools::winsys::reader;

// The pipe's server is agent.exe beside this program, by the image of the process that serves it.
auto served_by_agent(HANDLE pipe) -> bool {
    ULONG pid = 0;
    if (!GetNamedPipeServerProcessId(pipe, &pid) || pid == 0) {
        return false;
    }
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (p == nullptr) {
        return false;
    }
    std::array<wchar_t, 32768> image{};
    DWORD n = static_cast<DWORD>(image.size());
    const BOOL got = QueryFullProcessImageNameW(p, 0, image.data(), &n);
    CloseHandle(p);
    if (!got) {
        return false;
    }
    std::array<wchar_t, 32768> self{};
    const DWORD m = GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size()));
    if (m == 0 || m >= self.size()) {
        return false;
    }
    std::wstring agent{self.data(), m};
    agent = agent.substr(0, agent.find_last_of(L'\\') + 1) + L"agent.exe";
    return CompareStringOrdinal(image.data(), static_cast<int>(n), agent.c_str(), static_cast<int>(agent.size()), TRUE) == CSTR_EQUAL;
}

auto write_all(HANDLE pipe, std::string_view data) -> bool {
    while (!data.empty()) {
        DWORD put = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(data.size(), 64 * 1024));
        if (!WriteFile(pipe, data.data(), chunk, &put, nullptr) || put == 0) {
            return false;
        }
        data.remove_prefix(put);
    }
    return true;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring pipe_name;
    std::uint32_t limit = rd::kLimitMs;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::wstring_view key{argv[i]};
        if (key == L"--pipe") {
            pipe_name = argv[i + 1];
        } else if (key == L"--limit-ms") {
            limit = std::min<std::uint32_t>(limit, static_cast<std::uint32_t>(std::wcstoul(argv[i + 1], nullptr, 10)));
        } else {
            return rd::kExitUsage;
        }
    }
    if (argc % 2 == 0 || !reader::agent_pipe_name(pipe_name)) {
        return rd::kExitUsage;
    }
    // The limit, whatever the rest is doing: the whole process ends with it.
    std::thread{[limit] {
        Sleep(limit);
        ExitProcess(static_cast<UINT>(rd::kExitTimeLimit));
    }}.detach();

    HANDLE pipe = reader::open_pipe(pipe_name);
    if (pipe == INVALID_HANDLE_VALUE) {
        return rd::kExitNoAgent;
    }
    if (!served_by_agent(pipe)) {
        CloseHandle(pipe);
        return rd::kExitNotAgent;
    }
    const auto data = reader::collect();
    const auto json = core::json::write(data);
    const bool sent = json.has_value() && write_all(pipe, *json);
    FlushFileBuffers(pipe);
    CloseHandle(pipe);
    return sent ? rd::kExitDone : rd::kExitNoAgent;
}
