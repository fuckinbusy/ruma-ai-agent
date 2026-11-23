#pragma once

// app::monitor — the machine's numbers over the last minute (docs/plans/phase-5.md, Task 10):
// once a second the processor (GetSystemTimes), memory (GlobalMemoryStatusEx) and the system
// drive, kept 60 points each; the graphics card (PDH "GPU Engine", ~1 ms a reading) only while
// someone asks — the report on screen, a watcher on it. Watchers (Task 11) hear when a metric
// goes over their line. The sheet at rest is never woken by it: the owner is told of a reading
// only while it asks (set_notify).

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace app::monitor {

enum class Metric : std::uint8_t { Cpu, Memory, Gpu, Disk };
inline constexpr std::size_t kPoints = 60;

struct Reading {
    std::optional<float> cpu; // % of all processors since the last reading; none at the first
    std::uint64_t memory_used = 0;
    std::uint64_t memory_total = 0;
    std::optional<float> gpu; // % of the busiest adapter; none unless asked (and at its first)
    std::uint64_t disk_used = 0; // the system drive
    std::uint64_t disk_total = 0;
};

// The processor's temperature: a value from administrator rights, else why there is none.
enum class TemperatureState : std::uint8_t { Value, NeedsAdmin, NotAvailable };
struct Temperature {
    TemperatureState state = TemperatureState::NotAvailable;
    float celsius = 0;
};
// "52 °C", "temp needs admin", "no temperature" (as short as the report's tile needs).
auto temperature_text(const Temperature& t) -> std::string;

// What ROOT\WMI MSAcpi_ThermalZoneTemperature answered: its result and the hottest zone.
struct WmiAnswer {
    long hr = 0; // HRESULT: S_OK, WBEM_E_NOT_SUPPORTED, WBEM_E_ACCESS_DENIED…
    std::optional<float> celsius;
};
auto wmi_temperature() -> WmiAnswer;
// "Needs administrator rights" only when Windows refused without them; "not supported" (most
// laptops' firmware) is no temperature at all, with rights or without.
auto temperature_of(const WmiAnswer& answer, bool elevated) -> Temperature;

class Source {
public:
    virtual ~Source() = default;
    virtual auto read(bool gpu) -> Reading = 0;
    virtual auto temperature() -> Temperature = 0;
};
// This computer. `elevated`: the agent runs as administrator (the only way to a temperature).
auto machine(bool elevated) -> std::unique_ptr<Source>;

struct Snapshot {
    Reading latest;
    std::array<std::vector<float>, 4> series; // by Metric, oldest first, in percent
    std::size_t samples = 0;
};

class Monitor {
public:
    explicit Monitor(std::unique_ptr<Source> source);
    ~Monitor();
    Monitor(const Monitor&) = delete;
    auto operator=(const Monitor&) -> Monitor& = delete;
    auto operator=(Monitor&&) -> Monitor& = delete;

    // Reads every `every` on a thread of its own; after a reading, `sampled` while set_notify is on.
    void start(std::function<void()> sampled, std::chrono::milliseconds every = std::chrono::seconds{1});
    void stop();
    // One reading now (start() calls it; tests call it themselves); `gpu_too`: the graphics card
    // this once, whatever set_gpu says (the report being made).
    void sample(bool gpu_too = false);
    void set_gpu(bool on) noexcept { gpu_ = on; }
    void set_notify(bool on) noexcept { notify_ = on; }

    // Oldest first, in percent. Not for another thread while started: snapshot() is.
    [[nodiscard]] auto series(Metric m) const -> std::span<const float>;
    [[nodiscard]] auto snapshot() const -> Snapshot;
    [[nodiscard]] auto temperature() -> Temperature;

    // `fire` when `metric` goes over `above` — once, until it is back under it.
    auto watch(Metric metric, float above, std::function<void(float)> fire) -> std::uint64_t;
    void unwatch(std::uint64_t id);

private:
    struct Ring {
        std::array<float, kPoints> points{};
        std::size_t size = 0;
        void push(float v);
    };
    struct Watch {
        std::uint64_t id = 0;
        Metric metric = Metric::Memory;
        float above = 0;
        bool over = false;
        std::function<void(float)> fire;
    };

    std::unique_ptr<Source> source_;
    std::mutex read_mutex_; // one reading at a time
    mutable std::mutex mutex_;
    std::array<Ring, 4> rings_;
    Reading latest_;
    std::size_t samples_ = 0;
    std::vector<Watch> watches_;
    std::uint64_t next_watch_ = 1;
    std::atomic<bool> gpu_{false};
    std::atomic<bool> notify_{false};
    std::condition_variable_any wake_;
    std::jthread thread_; // last: stopped first
};

} // namespace app::monitor
