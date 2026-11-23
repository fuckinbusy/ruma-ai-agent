#include "core/log.hpp"
#include "core/secret_like.hpp"

#include "core/mem/mem.hpp"
#include "core/queue.hpp"
#include "core/version.hpp"
#include "core/win.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <format>
#include <mutex>
#include <semaphore>
#include <thread>

namespace core::log {
namespace {

constexpr std::string_view kEllipsis = "\xE2\x80\xA6"; // U+2026 "…"

struct Record {
    std::uint64_t filetime; // UTC, 100 ns since 1601
    const char* file;       // __FILE__ literal of the call site, or nullptr
    std::uint32_t line;
    std::uint32_t thread_id;
    Level level;
    std::uint8_t category_len;
    std::uint16_t text_len;
    char category[kMaxCategory];
    char text[kMaxText];
};

std::atomic<Logger*> g_logger{nullptr};
std::atomic<int> g_active_writers{0}; // callers inside submit(); the deleter waits for zero
std::atomic<std::uint8_t> g_min_level{static_cast<std::uint8_t>(Level::Info)};
std::atomic<std::uint64_t> g_dropped{0};
std::atomic<std::uint64_t> g_recent_version{0}; // recent_version()
std::atomic<std::uint64_t> g_writer_wakeups{0};

auto level_name(Level level) noexcept -> const char* {
    switch (level) {
    case Level::Trace: return "TRACE";
    case Level::Debug: return "DEBUG";
    case Level::Info: return "INFO";
    case Level::Warn: return "WARN";
    case Level::Error: return "ERROR";
    }
    return "?";
}

auto basename(const char* path) noexcept -> const char* {
    const char* name = path;
    for (const char* p = path; *p != '\0'; ++p) {
        if (*p == '\\' || *p == '/') {
            name = p + 1;
        }
    }
    return name;
}

auto to_filetime(std::uint64_t value) noexcept -> FILETIME {
    const ULARGE_INTEGER u{.QuadPart = value};
    return FILETIME{u.LowPart, u.HighPart};
}

// UTC FILETIME to local wall-clock time (current time-zone rules, DST included).
auto local_time(std::uint64_t utc_filetime) noexcept -> SYSTEMTIME {
    const FILETIME ft = to_filetime(utc_filetime);
    SYSTEMTIME utc{}, local{};
    FileTimeToSystemTime(&ft, &utc);
    if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) {
        local = utc;
    }
    return local;
}

auto date_key(const SYSTEMTIME& t) noexcept -> std::uint32_t {
    return static_cast<std::uint32_t>(t.wYear) * 10000u + t.wMonth * 100u + t.wDay;
}

// Drops a UTF-8 sequence cut off at the end of s[0, n).
auto trim_partial_utf8(const char* s, std::size_t n) noexcept -> std::size_t {
    for (std::size_t back = 1; back <= 4 && back <= n; ++back) {
        const auto c = static_cast<unsigned char>(s[n - back]);
        if ((c & 0xC0) == 0x80) {
            continue; // continuation byte: keep looking for the lead
        }
        const std::size_t need = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        return back >= need ? n : n - back;
    }
    return n;
}

} // namespace

class Logger {
public:
    Logger(const Config& config, core::win::FileHandle file)
        : queue_(config.queue_capacity), file_(std::move(file)),
          debug_output_(config.debug_output), recent_(config.recent_capacity), thread_([this] { run(); }) {}

    ~Logger() {
        stopping_.store(true, std::memory_order_seq_cst);
        wake_.release();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // Wake-up protocol (Dekker): the producer publishes the record, fences, reads sleeping_;
    // the writer sets sleeping_, fences, re-checks the queue. The two seq_cst fences guarantee
    // at least one side sees the other's store, so the writer can sleep without a timeout.
    auto push(Record&& record) -> bool {
        if (!queue_.try_push(std::move(record))) {
            return false;
        }
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (sleeping_.load(std::memory_order_relaxed) &&
            sleeping_.exchange(false, std::memory_order_seq_cst)) {
            wake_.release();
        }
        return true;
    }

    auto kept(std::size_t max) -> std::vector<Entry> {
        std::scoped_lock lock{recent_mutex_};
        const std::size_t n = std::min(max, recent_size_);
        std::vector<Entry> out;
        out.reserve(n);
        for (std::size_t i = recent_size_ - n; i < recent_size_; ++i) {
            const Record& r = recent_[(recent_next_ + recent_.size() - recent_size_ + i) % recent_.size()];
            const SYSTEMTIME t = local_time(r.filetime);
            out.push_back({std::format("{:02}:{:02}:{:02}.{:03}", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds),
                           r.level, std::string{r.category, r.category_len}, std::string{r.text, r.text_len}});
        }
        return out;
    }

private:
    void keep(const Record& r) {
        if (recent_.empty()) {
            return;
        }
        std::scoped_lock lock{recent_mutex_};
        recent_[recent_next_] = r;
        recent_next_ = (recent_next_ + 1) % recent_.size();
        recent_size_ = std::min(recent_size_ + 1, recent_.size());
        g_recent_version.fetch_add(1, std::memory_order_release); // under the lock: seen with the record
    }

    void run() {
        core::mem::TagScope tag{core::mem::tag_id("log/writer")};
        write_header();
        Record record;
        for (;;) {
            while (queue_.try_pop(record)) {
                append(record);
            }
            flush();
            if (stopping_.load(std::memory_order_seq_cst)) {
                // Producers are gone (the deleter waited for them): drain and stop. Do not
                // "simplify" this to a plain break: a record pushed after the drain loop above
                // saw an empty queue but before stopping_ was set would be lost. The race is
                // too narrow for a deterministic test (a mutation removing this survived).
                if (!queue_.try_pop(record)) {
                    break;
                }
                append(record);
                continue;
            }
            sleeping_.store(true, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_seq_cst);
            if (queue_.try_pop(record)) { // pushed between the drain and going to sleep
                sleeping_.store(false, std::memory_order_relaxed);
                append(record);
                continue;
            }
            wake_.acquire(); // no timeout: an idle logger costs nothing
            g_writer_wakeups.fetch_add(1, std::memory_order_relaxed);
            sleeping_.store(false, std::memory_order_relaxed);
        }
        flush();
    }

    // "===== Agent 0.0.1 | 2026-10-10 11:41:03 UTC+03:00 | pid 12345 | Release ====="
    void write_header() {
        FILETIME now_ft{};
        GetSystemTimePreciseAsFileTime(&now_ft);
        const std::uint64_t now =
            ULARGE_INTEGER{.LowPart = now_ft.dwLowDateTime, .HighPart = now_ft.dwHighDateTime}
                .QuadPart;
        const SYSTEMTIME local = local_time(now);
        FILETIME local_ft{};
        SystemTimeToFileTime(&local, &local_ft);
        const auto local_value =
            ULARGE_INTEGER{.LowPart = local_ft.dwLowDateTime, .HighPart = local_ft.dwHighDateTime}
                .QuadPart;
        // Offset in minutes, rounded: the conversion drops sub-millisecond precision.
        const auto offset_minutes = static_cast<int>(
            (static_cast<std::int64_t>(local_value) - static_cast<std::int64_t>(now) +
             (local_value >= now ? 300'000'000 : -300'000'000)) /
            600'000'000);
        const int offset_abs = offset_minutes < 0 ? -offset_minutes : offset_minutes;
#ifdef NDEBUG
        constexpr const char* kBuild = "Release";
#else
        constexpr const char* kBuild = "Debug";
#endif
        const std::string_view version = core::version();
        const int n = std::snprintf(
            staging_.data(), staging_.size(),
            "===== Agent %.*s | %04u-%02u-%02u %02u:%02u:%02u UTC%c%02d:%02d | pid %lu | %s "
            "=====\n",
            static_cast<int>(version.size()), version.data(), local.wYear, local.wMonth,
            local.wDay, local.wHour, local.wMinute, local.wSecond, offset_minutes < 0 ? '-' : '+',
            offset_abs / 60, offset_abs % 60, GetCurrentProcessId(), kBuild);
        last_date_ = date_key(local);
        emit(static_cast<std::size_t>(std::max(n, 0)));
    }

    // One record: "HH:MM:SS.mmm  LEVEL  category    tTHREAD  message  (file:line)"; lines of a
    // multi-line message continue under the message column.
    void append(const Record& r) {
        keep(r);
        const SYSTEMTIME t = local_time(r.filetime);
        std::size_t len = 0;
        const auto room = [&] { return staging_.size() - len; };
        if (date_key(t) != last_date_) {
            last_date_ = date_key(t);
            len += static_cast<std::size_t>(std::max(
                std::snprintf(staging_.data(), staging_.size(), "----- %04u-%02u-%02u -----\n",
                              t.wYear, t.wMonth, t.wDay),
                0));
        }
        const std::size_t line_start = len;
        len += static_cast<std::size_t>(std::max(
            std::snprintf(staging_.data() + len, room(), "%02u:%02u:%02u.%03u  %-5s  %-10.*s  t%-6lu  ",
                          t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, level_name(r.level),
                          static_cast<int>(r.category_len), r.category,
                          static_cast<unsigned long>(r.thread_id)),
            0));
        const std::size_t indent = len - line_start;

        const auto put = [&](const char* s, std::size_t n) {
            n = std::min(n, room() - 1); // keep one byte for the terminating zero
            std::memcpy(staging_.data() + len, s, n);
            len += n;
        };
        std::string_view text{r.text, r.text_len};
        bool first = true;
        while (!text.empty() || first) {
            const std::size_t nl = text.find('\n');
            std::string_view segment = text.substr(0, nl);
            if (segment.ends_with('\r')) {
                segment.remove_suffix(1);
            }
            if (!first) {
                put("\n", 1);
                for (std::size_t i = 0; i < indent && room() > 1; ++i) {
                    staging_[len++] = ' ';
                }
            }
            put(segment.data(), segment.size());
            if (first && r.file != nullptr) {
                len += static_cast<std::size_t>(std::max(
                    std::snprintf(staging_.data() + len, room(), "  (%s:%u)", basename(r.file),
                                  static_cast<unsigned>(r.line)),
                    0));
            }
            first = false;
            if (nl == std::string_view::npos) {
                break;
            }
            text.remove_prefix(nl + 1);
        }
        put("\n", 1);
        emit(len);
    }

    // Moves staging_[0, n) into the file batch (and the debugger output if enabled).
    void emit(std::size_t n) {
        if (n == 0) {
            return;
        }
        if (batch_.size() - batch_len_ < n) {
            flush();
        }
        std::memcpy(batch_.data() + batch_len_, staging_.data(), n);
        batch_len_ += n;
        if (debug_output_) {
            staging_[std::min(n, staging_.size() - 1)] = '\0';
            OutputDebugStringA(staging_.data());
        }
    }

    void flush() {
        if (batch_len_ == 0) {
            return;
        }
        DWORD written = 0;
        WriteFile(file_.get(), batch_.data(), static_cast<DWORD>(batch_len_), &written, nullptr);
        batch_len_ = 0;
    }

    MpmcQueue<Record> queue_;
    core::win::FileHandle file_;
    bool debug_output_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> sleeping_{false};
    std::counting_semaphore<> wake_{0};
    std::array<char, 64 * 1024> batch_{};   // written by the writer thread only
    std::size_t batch_len_ = 0;
    std::array<char, 32 * 1024> staging_{}; // one formatted record (fits a 480-line message)
    std::uint32_t last_date_ = 0;           // yyyymmdd of the last written line
    std::vector<Record> recent_;            // ring of the newest records, for recent()
    std::size_t recent_next_ = 0;
    std::size_t recent_size_ = 0;
    std::mutex recent_mutex_;
    std::jthread thread_; // last: starts after every other member exists
};

namespace {

void submit(Level level, std::string_view category, std::string_view text, bool truncated,
            const char* file, std::uint32_t line) noexcept {
    // seq_cst on both sides of this handshake with the deleter (store-buffering freedom is
    // only guaranteed for seq_cst by the C++ model; on x86 it costs nothing extra).
    g_active_writers.fetch_add(1, std::memory_order_seq_cst);
    Logger* logger = g_logger.load(std::memory_order_seq_cst);
    if (logger != nullptr) {
        Record r;
        FILETIME ft;
        GetSystemTimePreciseAsFileTime(&ft);
        r.filetime = ULARGE_INTEGER{.LowPart = ft.dwLowDateTime, .HighPart = ft.dwHighDateTime}
                         .QuadPart;
        r.file = file;
        r.line = line;
        r.thread_id = GetCurrentThreadId();
        r.level = level;
        r.category_len = static_cast<std::uint8_t>(std::min(category.size(), kMaxCategory));
        std::memcpy(r.category, category.data(), r.category_len);

        const std::size_t room = kMaxText - (truncated ? kEllipsis.size() : 0);
        std::size_t n = mask_secrets(text, {r.text, room});
        n = trim_partial_utf8(r.text, n);
        if (truncated) {
            std::memcpy(r.text + n, kEllipsis.data(), kEllipsis.size());
            n += kEllipsis.size();
        }
        r.text_len = static_cast<std::uint16_t>(n);
        if (!logger->push(std::move(r))) {
            g_dropped.fetch_add(1, std::memory_order_relaxed);
        }
    }
    g_active_writers.fetch_sub(1, std::memory_order_seq_cst);
}

} // namespace

void LoggerDeleter::operator()(Logger* logger) const noexcept {
    if (logger == nullptr) {
        return;
    }
    Logger* expected = logger;
    g_logger.compare_exchange_strong(expected, nullptr, std::memory_order_seq_cst);
    g_recent_version.fetch_add(1, std::memory_order_release); // recent() is empty from now on
    while (g_active_writers.load(std::memory_order_seq_cst) != 0) {
        std::this_thread::yield();
    }
    std::unique_ptr<Logger> owned{logger}; // stops the writer after it drained the queue
}

auto start(const Config& config) -> core::Result<LoggerPtr> {
    // Refuse before touching the file system; the CAS below still settles a race.
    if (g_logger.load(std::memory_order_seq_cst) != nullptr) {
        return core::make_error(core::Code::AlreadyExists, "a logger is already running");
    }
    core::win::FileHandle file{CreateFileW(
        config.file.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (!file) {
        return core::os_error(GetLastError(), "opening the log file");
    }
    auto logger = std::make_unique<Logger>(config, std::move(file));
    Logger* expected = nullptr;
    if (!g_logger.compare_exchange_strong(expected, logger.get(), std::memory_order_seq_cst)) {
        return core::make_error(core::Code::AlreadyExists, "a logger is already running");
    }
    g_min_level.store(static_cast<std::uint8_t>(config.min_level), std::memory_order_relaxed);
    g_recent_version.fetch_add(1, std::memory_order_release); // another logger's records are gone
    return LoggerPtr{logger.release()};
}

auto recent_version() noexcept -> std::uint64_t {
    return g_recent_version.load(std::memory_order_acquire);
}

auto recent(std::size_t max) -> std::vector<Entry> {
    // The same handshake as submit(): the deleter waits until no caller holds the logger.
    g_active_writers.fetch_add(1, std::memory_order_seq_cst);
    std::vector<Entry> out;
    if (Logger* logger = g_logger.load(std::memory_order_seq_cst)) {
        out = logger->kept(max);
    }
    g_active_writers.fetch_sub(1, std::memory_order_seq_cst);
    return out;
}

auto enabled(Level level) noexcept -> bool {
    return static_cast<std::uint8_t>(level) >= g_min_level.load(std::memory_order_relaxed) &&
           g_logger.load(std::memory_order_relaxed) != nullptr;
}

void write(Level level, std::string_view category, std::string_view text) noexcept {
    if (!enabled(level)) {
        return;
    }
    const bool truncated = text.size() > kMaxText;
    submit(level, category, truncated ? text.substr(0, kMaxText) : text, truncated, nullptr, 0);
}

auto writer_wakeups() noexcept -> std::uint64_t {
    return g_writer_wakeups.load(std::memory_order_relaxed);
}

auto dropped() noexcept -> std::uint64_t {
    return g_dropped.load(std::memory_order_relaxed);
}

void detail::write_formatted(Level level, std::string_view category, const char* text,
                             std::size_t full_size, const char* file,
                             std::uint32_t line) noexcept {
    const bool truncated = full_size > kMaxText;
    submit(level, category, std::string_view{text, truncated ? kMaxText : full_size}, truncated,
           file, line);
}

auto mask_secrets(std::string_view in, std::span<char> out) noexcept -> std::size_t {
    std::size_t o = 0;
    const auto put = [&](char c) {
        if (o < out.size()) {
            out[o++] = c;
        }
    };
    const auto put_str = [&](std::string_view s) {
        for (char c : s) {
            put(c);
        }
    };

    // What core::secret_like finds (phase 12 § 6: the patterns memory.add refuses): its name kept
    // (sk-, password=, Authorization: Bearer), the secret itself "***".
    std::size_t i = 0;
    while (i < in.size() && o < out.size()) {
        if (const auto found = secret_like::at(in, i)) {
            put_str(in.substr(i, found->value - i));
            if (found->end > found->value) {
                put_str("***");
            }
            i = found->end;
            continue;
        }
        put(in[i++]);
    }
    return o;
}

} // namespace core::log
