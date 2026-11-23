#include "reader/pipe_io.hpp"

#include "tools/winsys/reader_data.hpp"

#include <algorithm>

namespace reader {

auto agent_pipe_name(const std::wstring& name) -> bool {
    constexpr std::wstring_view prefix = tools::winsys::reader::kPipePrefix;
    if (name.size() != prefix.size() + 32 || std::wstring_view{name}.substr(0, prefix.size()) != prefix) {
        return false;
    }
    return std::all_of(name.begin() + static_cast<std::ptrdiff_t>(prefix.size()), name.end(),
                       [](wchar_t c) { return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'); });
}

auto open_pipe(const std::wstring& name) -> HANDLE {
    if (!agent_pipe_name(name)) {
        SetLastError(ERROR_INVALID_NAME);
        return INVALID_HANDLE_VALUE;
    }
    // Write only, and anonymous to the server: whoever serves the pipe cannot act with this
    // administrator's token (impersonation), even before the helper checks who it is.
    return CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS, nullptr);
}

} // namespace reader
