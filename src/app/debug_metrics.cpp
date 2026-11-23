#include "app/debug_metrics.hpp"

#include "core/log.hpp"
#include "core/mem/mem.hpp"

#include <algorithm>
#include <format>
#include <span>

namespace app::debug {
namespace {

using sheet::view_name;

auto level_name(core::log::Level l) -> std::string_view {
    switch (l) {
    case core::log::Level::Trace: return "TRACE";
    case core::log::Level::Debug: return "DEBUG";
    case core::log::Level::Info: return "INFO";
    case core::log::Level::Warn: return "WARN";
    case core::log::Level::Error: return "ERROR";
    }
    return "?";
}

// Reorders `v`.
auto percentile(std::span<double> v, double p) -> double {
    if (v.empty()) {
        return 0;
    }
    const auto k = static_cast<std::size_t>(p * static_cast<double>(v.size() - 1) + 0.5);
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
    return v[k];
}

} // namespace

auto reason_name(FrameReason r) -> std::string_view {
    switch (r) {
    case FrameReason::Rest: return "rest";
    case FrameReason::RestMotion: return "resting motion";
    case FrameReason::Animation: return "animation";
    case FrameReason::Streaming: return "streaming";
    case FrameReason::IdleOpen: return "idle open";
    }
    return "?";
}

void Metrics::add(const FrameSample& f) {
    ring_[next_] = f;
    next_ = (next_ + 1) % kFrames;
    size_ = std::min(size_ + 1, kFrames);
}

auto Metrics::frames() const -> std::vector<FrameSample> {
    std::vector<FrameSample> out;
    out.reserve(size_);
    for (std::size_t i = 0; i < size_; ++i) {
        out.push_back(frame(i));
    }
    return out;
}

auto memory_now(double t) noexcept -> MemorySample {
    const auto snap = core::mem::snapshot();
    MemorySample s{.t = t, .allocations = core::mem::total_allocations()};
    for (std::size_t i = 0; i < snap.live_bytes.size(); ++i) {
        s.live_bytes += snap.live_bytes[i];
        s.live_blocks += snap.live_blocks[i];
    }
    return s;
}

void Metrics::add_memory(const MemorySample& s) {
    memory_[memory_next_] = s;
    memory_next_ = (memory_next_ + 1) % kMemorySamples;
    memory_size_ = std::min(memory_size_ + 1, kMemorySamples);
}

auto Metrics::memory_summary(double now, double window) const noexcept -> MemorySummary {
    MemorySummary s;
    for (std::size_t i = 0; i < memory_size_; ++i) {
        const auto& m = memory(i);
        if (m.t <= now - window || m.t > now) {
            continue;
        }
        if (s.samples == 0) {
            s.first = m;
        } else if (m.t > s.last.t) {
            s.allocs_per_s_max = std::max(s.allocs_per_s_max,
                                          static_cast<double>(m.allocations - s.last.allocations) / (m.t - s.last.t));
        }
        s.last = m;
        s.peak_live_bytes = std::max(s.peak_live_bytes, m.live_bytes);
        ++s.samples;
    }
    if (s.last.t > s.first.t) {
        s.allocs_per_s = static_cast<double>(s.last.allocations - s.first.allocations) / (s.last.t - s.first.t);
    }
    return s;
}

void Metrics::mark_startup(std::string_view stage, double ms) {
    startup_.push_back({std::string{stage}, ms});
}

void Metrics::note_open_refused(std::string reason, double t) {
    refused_ = Refusal{std::move(reason), t};
}

auto Metrics::summary(double now, double window) const -> Summary {
    Summary s;
    std::array<double, kFrames> intervals; // on the stack: the debug window asks 30 times a second
    std::size_t n = 0;
    double first = 0, last = 0, work = 0, swap = 0;
    for (std::size_t i = 0; i < size_; ++i) {
        const auto& f = frame(i);
        if (f.t <= now - window || f.t > now) {
            continue;
        }
        if (s.frames == 0) {
            first = f.t;
        } else {
            intervals[n++] = (f.t - last) * 1000.0;
        }
        last = f.t;
        ++s.frames;
        work += f.work_ms;
        swap += f.swap_ms;
        s.work_max_ms = std::max(s.work_max_ms, f.work_ms);
        s.swap_max_ms = std::max(s.swap_max_ms, f.swap_ms);
        s.last_reason = f.reason;
        s.draw_calls = f.draw_calls;
        s.instances = f.instances;
    }
    if (s.frames == 0) {
        return s;
    }
    s.fps = last > first ? static_cast<double>(s.frames) / (last - first) : 0;
    const std::span<double> shown{intervals.data(), n};
    s.p50_ms = percentile(shown, 0.50);
    s.p95_ms = percentile(shown, 0.95);
    s.max_ms = shown.empty() ? 0 : *std::ranges::max_element(shown);
    s.work_avg_ms = work / static_cast<double>(s.frames);
    s.swap_avg_ms = swap / static_cast<double>(s.frames);
    return s;
}

auto snapshot_text(const SnapshotInput& in) -> std::string {
    std::string out;
    const auto line = [&](std::string_view text) {
        out += text;
        out += '\n';
    };
    const auto s = in.metrics.summary(in.now, 5.0);
    line("== frames (last 5 s)");
    line(std::format("frames: {}, fps: {:.1f}, interval p50: {:.2f} ms, p95: {:.2f} ms, max: {:.2f} ms", s.frames,
                     s.fps, s.p50_ms, s.p95_ms, s.max_ms));
    line(std::format("reason: {}", reason_name(s.last_reason)));
    line(std::format("work: avg {:.3f} ms, max {:.3f} ms; swap: avg {:.3f} ms, max {:.3f} ms", s.work_avg_ms,
                     s.work_max_ms, s.swap_avg_ms, s.swap_max_ms));
    line(std::format("draw calls: {}, instances: {} (newest frame)", s.draw_calls, s.instances));

    line("== startup");
    for (const auto& st : in.metrics.startup()) {
        line(std::format("{}: {:.1f} ms", st.name, st.ms));
    }

    line("== sheet");
    const auto& m = in.model;
    line(std::format("view: {}, open: {}, task: {}, answer: {} bytes, streaming: {}", view_name(sheet::view(m)),
                     m.open, m.task, m.answer.size(), m.streaming));
    if (const auto& r = in.metrics.open_refused()) {
        line(std::format("open refused: {} (at {:.1f} s)", r->reason, r->t));
    } else {
        line("open refused: never");
    }

    line("== settings");
    const auto& c = in.config;
    line(std::format("hotkey: {}{}", c.hotkey, in.live_hotkey ? std::format(" (live: {})", *in.live_hotkey) : ""));
    line(std::format("over_fullscreen: {}, text_scale: {:.3f}, rest_animation: {}", c.over_fullscreen, c.text_scale,
                     c.rest_animation));
    line(std::format("advanced: system writes {}, secrets read {}, permanent delete {}", c.advanced.allow_system_writes,
                     c.advanced.allow_secrets_read, c.advanced.allow_permanent_delete));

    line("== memory (core::mem, live)");
    for (const auto& t : core::mem::stats()) {
        if (t.live_blocks != 0 || t.live_bytes != 0) {
            line(std::format("{}: {} bytes in {} blocks ({} allocations so far)", t.name, t.live_bytes, t.live_blocks,
                             t.total_allocs));
        }
    }
    if (in.atlas_fill >= 0) {
        line(std::format("atlas: {:.0f}%", in.atlas_fill * 100.0));
    }
    line(std::format("log records dropped: {}", core::log::dropped()));

    line("== memory over time (last 60 s)");
    if (const auto mem = in.metrics.memory_summary(in.now, 60.0); mem.samples >= 2) {
        line(std::format("live now: {} bytes in {} blocks; {:.1f} s before: {} bytes in {} blocks; peak {} bytes",
                         mem.last.live_bytes, mem.last.live_blocks, mem.last.t - mem.first.t, mem.first.live_bytes,
                         mem.first.live_blocks, mem.peak_live_bytes));
        line(std::format("allocations: {:.0f} a second on average, {:.0f} at most. The count of allocations only "
                         "grows (each is counted when made, freed or not); a leak shows as live memory growing",
                         mem.allocs_per_s, mem.allocs_per_s_max));
    } else {
        line("not enough samples yet (one a second while the agent is awake)");
    }

    line("== log (newest 30)");
    for (const auto& e : core::log::recent(30)) {
        line(std::format("{}  {:<5}  {:<10}  {}", e.time, level_name(e.level), e.category, e.text));
    }
    return out;
}

} // namespace app::debug
