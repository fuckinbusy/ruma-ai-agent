#pragma once

// core::log — asynchronous logger. Callers format into a fixed stack buffer and push a fixed-size
// record into a lock-free queue; a background thread writes lines to the file. Logging never
// blocks and never allocates: a full queue drops the record and counts it.
//
//   auto logger = core::log::start({.file = dir / "agent.log"});   // RAII: flush + stop
//   WA_INFO("net", "connected in {} ms", ms);
//
// File format (local time; the message column is aligned, continuation lines too):
//   ===== Agent 0.0.1 | 2026-10-10 11:41:03 UTC+03:00 | pid 12345 | Release =====
//   11:41:03.123  INFO   net         t1234   connected in 42 ms  (http.cpp:88)
//   ----- 2026-10-11 -----                    (when the date changes)
//
// Cost: WA_TRACE / WA_DEBUG / ... below WA_LOG_COMPILE_LEVEL compile to nothing (arguments
// are not evaluated). Release builds keep INFO and above by default (CMake option
// WA_LOG_RELEASE_LEVEL); a kept level filtered at run time costs one relaxed atomic load.

#include "core/error.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace core::log {

enum class Level : std::uint8_t { Trace, Debug, Info, Warn, Error };

inline constexpr std::size_t kMaxText = 480;     // bytes of message text per record
inline constexpr std::size_t kMaxCategory = 23;  // bytes of category name

struct Config {
    std::filesystem::path file;
    Level min_level = Level::Info;
    bool debug_output = false;         // also OutputDebugStringA each line
    std::size_t queue_capacity = 1024; // records (~0.5 KiB each)
    // The newest records kept in memory for recent() (the debug window); 0: none. Filled by the
    // writer thread, so the callers' hot path stays as it is.
    std::size_t recent_capacity = 0;
};

// One kept record, as the debug window shows it.
struct Entry {
    std::string time; // local "HH:MM:SS.mmm"
    Level level = Level::Info;
    std::string category;
    std::string text;
};
// The newest `max` kept records, oldest first; empty without a logger or with recent_capacity 0.
auto recent(std::size_t max) -> std::vector<Entry>;
// Moves whenever what recent() returns may have changed: a record kept, a logger started or
// stopped. Cheap: the debug window fetches recent() again only when it moved.
auto recent_version() noexcept -> std::uint64_t;

class Logger;
struct LoggerDeleter {
    void operator()(Logger* logger) const noexcept; // unpublishes, flushes everything, joins
};
using LoggerPtr = std::unique_ptr<Logger, LoggerDeleter>;

// Opens (appends to) the file, starts the writer thread and installs it as the process logger.
// Fails with AlreadyExists while another logger is installed.
auto start(const Config& config) -> core::Result<LoggerPtr>;

auto enabled(Level level) noexcept -> bool;
// No-op when no logger is installed. Text longer than kMaxText is cut at a UTF-8 boundary.
void write(Level level, std::string_view category, std::string_view text) noexcept;
// Records dropped because the queue was full, since process start.
auto dropped() noexcept -> std::uint64_t;
// Times the writer thread woke from sleep, since process start (idle cost diagnostics).
auto writer_wakeups() noexcept -> std::uint64_t;

// Replaces what core::secret_like finds (API keys, Authorization header values, password= and
// token= values, card numbers) with "***", their names kept. Writes at most out.size() bytes;
// returns the number written.
auto mask_secrets(std::string_view in, std::span<char> out) noexcept -> std::size_t;

namespace detail {
// Used by WA_LOG: `full_size` is the untruncated formatted length (format_to_n result);
// `file` must be a string literal (__FILE__): only the pointer is stored.
void write_formatted(Level level, std::string_view category, const char* text,
                     std::size_t full_size, const char* file, std::uint32_t line) noexcept;
} // namespace detail

} // namespace core::log

#define WA_LOG(level, category, ...)                                                               \
    do {                                                                                           \
        const ::core::log::Level wa_log_level_ = (level);                                          \
        if (::core::log::enabled(wa_log_level_)) {                                                 \
            char wa_log_text_[::core::log::kMaxText];                                              \
            const auto wa_log_result_ =                                                            \
                std::format_to_n(wa_log_text_, sizeof(wa_log_text_), __VA_ARGS__);                \
            ::core::log::detail::write_formatted(wa_log_level_, category, wa_log_text_,            \
                                                 static_cast<std::size_t>(wa_log_result_.size),    \
                                                 __FILE__, __LINE__);                              \
        }                                                                                          \
    } while (false)

// 0 = trace, 1 = debug, 2 = info, 3 = warn, 4 = error. Set by CMake (Debug: 0, Release: 2).
#ifndef WA_LOG_COMPILE_LEVEL
#define WA_LOG_COMPILE_LEVEL 0
#endif

#if WA_LOG_COMPILE_LEVEL <= 0
#define WA_TRACE(category, ...) WA_LOG(::core::log::Level::Trace, category, __VA_ARGS__)
#else
#define WA_TRACE(category, ...) ((void)0)
#endif
#if WA_LOG_COMPILE_LEVEL <= 1
#define WA_DEBUG(category, ...) WA_LOG(::core::log::Level::Debug, category, __VA_ARGS__)
#else
#define WA_DEBUG(category, ...) ((void)0)
#endif
#if WA_LOG_COMPILE_LEVEL <= 2
#define WA_INFO(category, ...) WA_LOG(::core::log::Level::Info, category, __VA_ARGS__)
#else
#define WA_INFO(category, ...) ((void)0)
#endif
#if WA_LOG_COMPILE_LEVEL <= 3
#define WA_WARN(category, ...) WA_LOG(::core::log::Level::Warn, category, __VA_ARGS__)
#else
#define WA_WARN(category, ...) ((void)0)
#endif
#define WA_ERROR(category, ...) WA_LOG(::core::log::Level::Error, category, __VA_ARGS__)
