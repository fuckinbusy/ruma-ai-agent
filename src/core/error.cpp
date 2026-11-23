#include "core/error.hpp"

#include <windows.h>

#include <array>
#include <format>

namespace core {

auto make_error(Code code, std::string message, std::source_location where)
    -> std::unexpected<Error> {
    return std::unexpected<Error>{Error{code, std::move(message), 0, where}};
}

auto os_error(std::uint32_t win32_error, std::string_view context, std::source_location where)
    -> std::unexpected<Error> {
    Code code = Code::Io;
    switch (win32_error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
    case ERROR_BAD_NETPATH:
        code = Code::NotFound;
        break;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        code = Code::AccessDenied;
        break;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        code = Code::AlreadyExists;
        break;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        code = Code::OutOfMemory;
        break;
    case ERROR_OPERATION_ABORTED:
    case ERROR_CANCELLED:
        code = Code::Cancelled;
        break;
    case ERROR_TIMEOUT:
    case WAIT_TIMEOUT:
        code = Code::Timeout;
        break;
    default:
        break;
    }
    return std::unexpected<Error>{Error{
        code, std::format("{}: {} (win32 {})", context, system_message(win32_error), win32_error),
        win32_error, where}};
}

auto system_message(std::uint32_t win32_error) -> std::string {
    std::array<wchar_t, 512> wide{};
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, win32_error, 0, wide.data(),
                                   static_cast<DWORD>(wide.size()), nullptr);
    DWORD len = n;
    while (len > 0 && (wide[len - 1] == L'\r' || wide[len - 1] == L'\n' || wide[len - 1] == L' ' ||
                       wide[len - 1] == L'.')) {
        --len;
    }
    if (len == 0) {
        return std::format("unknown error {}", win32_error);
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(len), nullptr,
                                          0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(len), out.data(), bytes,
                        nullptr, nullptr);
    return out;
}

auto code_name(Code code) -> std::string_view {
    switch (code) {
    case Code::Ok: return "ok";
    case Code::InvalidArgument: return "invalid_argument";
    case Code::NotFound: return "not_found";
    case Code::AccessDenied: return "access_denied";
    case Code::AlreadyExists: return "already_exists";
    case Code::Io: return "io";
    case Code::Parse: return "parse";
    case Code::Net: return "net";
    case Code::Api: return "api";
    case Code::Policy: return "policy";
    case Code::Cancelled: return "cancelled";
    case Code::Timeout: return "timeout";
    case Code::OutOfMemory: return "out_of_memory";
    case Code::Internal: return "internal";
    }
    return "unknown";
}

} // namespace core
