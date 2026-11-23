#pragma once

// app::debug — what the agent is doing, frame by frame, for the debug window (F12, Debug builds)
// and for a text snapshot anyone can read (`--snapshot=<file>`, every build): frame timing per
// stage and why frames come at their rate, startup stages, why the sheet last refused to open,
// the sheet's state, settings, memory per tag and the newest log lines.

#include "app/config.hpp"
#include "app/sheet_model.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace app::debug {

// Why the shell drew this frame at its rate.
enum class FrameReason : std::uint8_t {
    Rest,       // a frame at rest (a message, a setting changed)
    RestMotion, // the resting strip's slow drift
    Animation,  // the sheet's shape or layers move: monitor rate
    Streaming,  // the answer's text is revealed: monitor rate
    IdleOpen,   // the open sheet with nothing to do: the caret's flip, the slow orb
};
auto reason_name(FrameReason r) -> std::string_view;

struct FrameSample {
    double t = 0;         // seconds, the shell's clock
    double update_ms = 0; // Painter::update
    double draw_ms = 0;   // Painter::draw and the renderer's end
    double work_ms = 0;   // the whole frame until the swap
    double swap_ms = 0;   // SwapBuffers (vsync wait included)
    std::uint32_t draw_calls = 0;
    std::uint32_t instances = 0;
    FrameReason reason = FrameReason::Rest;
};

struct Stage {
    std::string name;
    double ms = 0; // since the process started
};

// The whole process's memory at one moment (all core::mem tags summed).
struct MemorySample {
    double t = 0;
    std::int64_t live_bytes = 0;
    std::int64_t live_blocks = 0;
    std::uint64_t allocations = 0; // made so far: only grows, freed or not
};
// Now, at time `t` (the shell's clock); allocates nothing.
auto memory_now(double t) noexcept -> MemorySample;

class Metrics {
public:
    static constexpr std::size_t kFrames = 2048;

    static constexpr std::size_t kMemorySamples = 600; // 10 minutes at one a second

    void add(const FrameSample& f);
    // Oldest first.
    [[nodiscard]] auto frames() const -> std::vector<FrameSample>;
    // The same in place, without a copy: frame(0) is the oldest.
    [[nodiscard]] auto frame_count() const noexcept -> std::size_t { return size_; }
    [[nodiscard]] auto frame(std::size_t i) const noexcept -> const FrameSample& {
        return ring_[(next_ + kFrames - size_ + i) % kFrames];
    }

    // Memory over time: the shell adds one a second while it is awake.
    void add_memory(const MemorySample& s);
    [[nodiscard]] auto memory_count() const noexcept -> std::size_t { return memory_size_; }
    [[nodiscard]] auto memory(std::size_t i) const noexcept -> const MemorySample& {
        return memory_[(memory_next_ + kMemorySamples - memory_size_ + i) % kMemorySamples];
    }
    // Samples in (now - window, now]: the first and the last, the peak, and the allocation rate
    // (on average and over the busiest second between two samples).
    struct MemorySummary {
        std::size_t samples = 0;
        MemorySample first, last;
        std::int64_t peak_live_bytes = 0;
        double allocs_per_s = 0, allocs_per_s_max = 0;
    };
    [[nodiscard]] auto memory_summary(double now, double window) const noexcept -> MemorySummary;

    void mark_startup(std::string_view stage, double ms);
    [[nodiscard]] auto startup() const noexcept -> const std::vector<Stage>& { return startup_; }

    void note_open_refused(std::string reason, double t);
    struct Refusal {
        std::string reason;
        double t = 0;
    };
    [[nodiscard]] auto open_refused() const noexcept -> const std::optional<Refusal>& { return refused_; }

    // Frames drawn in (now - window, now]: count, rate, and the intervals between them.
    struct Summary {
        std::size_t frames = 0;
        double fps = 0;
        double p50_ms = 0, p95_ms = 0, max_ms = 0; // interval between frames
        double work_avg_ms = 0, work_max_ms = 0;
        double swap_avg_ms = 0, swap_max_ms = 0;
        FrameReason last_reason = FrameReason::Rest;
        std::uint32_t draw_calls = 0, instances = 0; // the newest frame's
    };
    [[nodiscard]] auto summary(double now, double window) const -> Summary;

private:
    std::array<FrameSample, kFrames> ring_{};
    std::size_t next_ = 0;
    std::size_t size_ = 0;
    std::array<MemorySample, kMemorySamples> memory_{};
    std::size_t memory_next_ = 0;
    std::size_t memory_size_ = 0;
    std::vector<Stage> startup_;
    std::optional<Refusal> refused_;
};

struct SnapshotInput {
    const Metrics& metrics;
    const sheet::Model& model;
    const config::Config& config;
    std::optional<std::string_view> live_hotkey;
    double now = 0;
    double atlas_fill = -1; // 0..1; < 0: unknown
};
// Plain text, one "== section" after another; meant to be read by a person or by Claude.
auto snapshot_text(const SnapshotInput& in) -> std::string;

} // namespace app::debug
