#pragma once

// app::tray_icon — the tray icon is the orb (design/ui_model screen 8): silver for Flash, gold for
// Pro, breathing while the agent works; its tip says the model and thinking. The shell renders the
// orb's breath once per choice; this keeps it as icons and says which is due — at rest the still
// once, while working at most 10 a second, and nothing allocated for a frame.

#include "app/model_choice.hpp"
#include "core/error.hpp"
#include "core/mem/handle.hpp"

#include <windows.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace app::tray_icon {

struct IconTraits {
    using type = HICON;
    static auto invalid() noexcept -> HICON { return nullptr; }
    static void close(HICON h) noexcept { DestroyIcon(h); }
};
using Icon = core::Handle<IconTraits>;

// L"WinAiAgent · Flash · Low".
[[nodiscard]] auto tip(const sheet::ModelChoice& choice) -> std::wstring;

// An icon of a `size` x `size` straight-alpha RGBA frame (rows top to bottom).
[[nodiscard]] auto make(std::span<const std::uint8_t> rgba, int size) -> core::Result<Icon>;

// A `from` x `from` RGBA frame shrunk to `to` x `to` by averaging blocks, the colour weighed by alpha
// (a see-through texel adds none). `from` a multiple of `to` reads best; any works.
[[nodiscard]] auto shrink(std::span<const std::uint8_t> rgba, int from, int to) -> std::vector<std::uint8_t>;
// The same into `out`, its room kept: nothing allocated once it is there.
void shrink_into(std::span<const std::uint8_t> rgba, int from, int to, std::vector<std::uint8_t>& out);

// The orb's breath: kBreath frames, one every 0.1 s while working (2.8 s, the orb's spike's own
// period), and its still. A pose is the orb's spike and phase for frame `i` (kStill: at rest).
inline constexpr int kBreath = 28;
inline constexpr int kStill = -1;
struct Pose {
    float spike = 0.2f;
    float phase = 2.4f;
};
[[nodiscard]] auto pose(int frame) noexcept -> Pose;

// The icons of a choice: its still made at once (`build`, one render), its breath a pose a call
// (`grow`, one a main-loop pass: all 28 at once took 13 ms on the UI thread, a dropped frame as
// the model's ring began — the review). Then only shown: while working the breath's frame of the
// moment (the still until the breath is whole), at rest the still once. `paint(pose, out)` puts
// the orb at `pose` into `out` as a size x size RGBA frame, its room kept: nothing allocated after
// the build.
class Animator {
public:
    template <class Paint>
    auto build(const sheet::ModelChoice& choice, int size, Paint&& paint) -> core::Result<void> {
        start(choice, size);
        paint(pose(kStill), frame_);
        return keep_still();
    }
    template <class Paint>
    auto grow(Paint&& paint) -> core::Result<void> {
        if (breathing() || failed_ || !for_) {
            return {};
        }
        paint(pose(static_cast<int>(breath_.size())), frame_);
        return keep_breath();
    }
    [[nodiscard]] auto breathing() const noexcept -> bool { return breath_.size() == static_cast<std::size_t>(kBreath); }
    [[nodiscard]] auto built_for(const sheet::ModelChoice& choice, int size) const noexcept -> bool;
    // The icon to show at `now`, or none: nothing new is due.
    [[nodiscard]] auto due(double now, bool working) noexcept -> HICON;
    // The tray lost what was shown (added again): the next due() shows an icon.
    void reshow() noexcept { shown_ = kStill - 1; }

private:
    void start(const sheet::ModelChoice& choice, int size);
    auto keep_still() -> core::Result<void>;
    auto keep_breath() -> core::Result<void>;

    std::vector<std::uint8_t> frame_; // paint's, its room kept
    std::vector<Icon> breath_;
    Icon still_;
    std::optional<sheet::ModelChoice> for_;
    int size_ = 0;
    int shown_ = kStill - 1; // none yet
    bool failed_ = false;    // a pose could not be made an icon: the still stays
};

} // namespace app::tray_icon
