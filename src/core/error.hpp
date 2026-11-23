#pragma once

// Errors are values: every operation that can fail returns core::Result<T>.

#include <cstdint>
#include <expected>
#include <source_location>
#include <string>
#include <string_view>

namespace core {

enum class Code : std::uint8_t {
    Ok,
    InvalidArgument,
    NotFound,
    AccessDenied,
    AlreadyExists,
    Io,
    Parse,
    Net,
    Api,
    Policy,
    Cancelled,
    Timeout,
    OutOfMemory,
    Internal,
};

struct Error {
    Code code = Code::Internal;
    std::string message;
    std::uint32_t os_error = 0; // Win32 error code when the failure came from the OS
    std::source_location where;
    // What a tool refused, for its card (phase 10, screen 25): "zip_bomb", "sheet_too_large",
    // "page_too_large", "not_an_image"; empty: a plain error. `facts` says it in numbers.
    std::string kind;
    std::string facts;
};

template <class T>
using Result = std::expected<T, Error>;

auto make_error(Code code, std::string message,
                std::source_location where = std::source_location::current())
    -> std::unexpected<Error>;

// Maps a Win32 error to a Code and builds "<context>: <system text> (win32 <n>)".
auto os_error(std::uint32_t win32_error, std::string_view context,
              std::source_location where = std::source_location::current())
    -> std::unexpected<Error>;

// The system's description of a Win32 error, UTF-8, without trailing whitespace.
auto system_message(std::uint32_t win32_error) -> std::string;

auto code_name(Code code) -> std::string_view;

} // namespace core
