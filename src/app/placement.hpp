#pragma once

// app::placement — where the sheet's window goes on a monitor: hanging from the top edge,
// centred; at rest with room around the panel for the handle's glow, open as tight as the panel
// (see Frame). Pure geometry, physical pixels out.

#include "app/sheet_view.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <span>

namespace app::placement {

struct Monitor {
    RECT rect{};       // the monitor's full area (the sheet hangs over the taskbar side too)
    float scale = 1.0f; // effective DPI / 96
};

struct Placement {
    RECT window{}; // screen coordinates
    RECT panel{};  // window-relative
    int radius = 0;
    float scale = 1.0f;
};

struct LogicalPoint {
    float x = 0, y = 0;
};

// Glow: the resting handle, with kMargin of room for its glow. Tight: the open sheet, whose
// window is the panel itself, because the DWM backdrop covers the whole window rectangle whatever
// its region (docs/spikes/region-backdrop.md); DWM rounds the corners, and the window goes
// kDwmCorner above the monitor so that only the bottom ones show.
enum class Frame { Glow, Tight };

// DWMWCP_ROUND's corner radius (Windows 11 guidelines), logical pixels.
inline constexpr float kDwmCorner = 8.0f;

inline auto place(const sheet::Shape& shape, const Monitor& m, Frame frame = Frame::Glow) -> Placement {
    const auto px = [&](float logical) {
        return static_cast<int>(std::lround(std::max(logical, 0.0f) * m.scale));
    };
    const int panel_w = std::max(px(shape.width), 1);
    const int panel_h = std::max(px(shape.height), 1);
    if (frame == Frame::Tight) {
        const int lift = px(kDwmCorner);
        const int x = m.rect.left + (m.rect.right - m.rect.left - panel_w) / 2;
        Placement p;
        p.window = {x, m.rect.top - lift, x + panel_w, m.rect.top + panel_h};
        p.panel = {0, lift, panel_w, lift + panel_h};
        p.radius = std::min(px(shape.radius), px(kDwmCorner));
        p.scale = m.scale;
        return p;
    }
    const float margin = sheet::kMargin;
    const int side = px(margin);
    const int w = panel_w + 2 * side;
    const int h = panel_h + px(margin);
    const int x = m.rect.left + (m.rect.right - m.rect.left - w) / 2;
    Placement p;
    p.window = {x, m.rect.top, x + w, m.rect.top + h};
    p.panel = {side, 0, side + panel_w, panel_h};
    p.radius = px(shape.radius);
    p.scale = m.scale;
    return p;
}

// The agent's window for a monitor (motion rework, task 5; made once at the
// agent's largest size and redrawn — never resized in a frame, MOTION_SPEC § 5.3). As wide as the
// widest sheet with the glow's margins, as tall as the tallest, hanging from the top edge with the
// corner's lift above it, centred. Never the monitor's size: Windows takes a topmost window of
// exactly that for a fullscreen one (the installer's black screen, 2026-10-16). `panel` holds the
// content's origin: (0, the lift). It depends on the monitor alone: a shape never moves it.
inline auto canvas(const Monitor& m) -> Placement {
    const float s = m.scale > 0 ? m.scale : 1.0f;
    const int mw = m.rect.right - m.rect.left;
    const int mh = m.rect.bottom - m.rect.top;
    const float room_w = static_cast<float>(mw) / s;
    const float room_h = static_cast<float>(mh) / s;
    const float widest = std::min(sheet::kTerminalWidth, room_w - 2 * sheet::kSideRoom);
    const float tallest = std::max(1.0f, room_h - sheet::kBottomRoom);
    const int lift = static_cast<int>(std::lround(kDwmCorner * s));
    const int w = std::min(static_cast<int>(std::lround((widest + 2 * sheet::kMargin) * s)), mw - 2);
    const int h = std::min(lift + static_cast<int>(std::lround((tallest + sheet::kMargin) * s)), lift + mh - 1);
    const int x = m.rect.left + (mw - w) / 2;
    Placement p;
    p.window = {x, m.rect.top - lift, x + w, m.rect.top - lift + h};
    p.panel = {0, lift, w, h};
    p.scale = s;
    return p;
}

// Where a shape is drawn inside the canvas, in its logical pixels: centred, at the top.
inline auto panel_in(const Placement& canvas, const sheet::Shape& shape) -> ui::layout::Box {
    const float w = static_cast<float>(canvas.window.right - canvas.window.left) / canvas.scale;
    return {(w - shape.width) / 2, static_cast<float>(canvas.panel.top) / canvas.scale, shape.width, shape.height};
}

// The window's region (phase 10): the shapes that take the mouse — the sign's zone, its capsule
// — in window pixels, rounded outward; a click anywhere else reaches the desktop (SetWindowRgn:
// WM_NCHITTEST's HTTRANSPARENT passes clicks to the same thread's windows only). More boxes than
// it holds: the last rectangle takes the rest.
struct Region {
    std::array<RECT, 8> rects{};
    std::size_t count = 0;
};

inline auto region_of(const Placement& p, std::span<const ui::layout::Box> boxes) -> Region {
    Region r;
    const int w = p.window.right - p.window.left;
    const int h = p.window.bottom - p.window.top;
    for (const auto& b : boxes) {
        const RECT rect{std::clamp(static_cast<int>(std::floor(b.x * p.scale)), 0, w), std::clamp(static_cast<int>(std::floor(b.y * p.scale)), 0, h),
                        std::clamp(static_cast<int>(std::ceil((b.x + b.w) * p.scale)), 0, w),
                        std::clamp(static_cast<int>(std::ceil((b.y + b.h) * p.scale)), 0, h)};
        if (r.count < r.rects.size()) {
            r.rects[r.count++] = rect;
        } else {
            auto& last = r.rects.back();
            last = {std::min(last.left, rect.left), std::min(last.top, rect.top), std::max(last.right, rect.right),
                    std::max(last.bottom, rect.bottom)};
        }
    }
    return r;
}

// A press at `p` (logical px in the window) with the sheet open is beside it — in its shadow's
// room — when it is on neither the sheet as it is drawn now nor where it is going (a window still
// coming out of the handle is a few pixels; one shrinking is still what the user sees).
inline auto beside_sheet(LogicalPoint p, const ui::layout::Box& now, const ui::layout::Box& target) -> bool {
    const auto on = [&](const ui::layout::Box& b) { return p.x >= b.x && p.x < b.x + b.w && p.y >= b.y && p.y < b.y + b.h; };
    return !on(now) && !on(target);
}

inline auto same(const Region& a, const Region& b) -> bool {
    return a.count == b.count && std::equal(a.rects.begin(), a.rects.begin() + static_cast<std::ptrdiff_t>(a.count), b.rects.begin(),
                                            [](const RECT& x, const RECT& y) {
                                                return x.left == y.left && x.top == y.top && x.right == y.right && x.bottom == y.bottom;
                                            });
}

// `rect` lies inside one of the region's rectangles.
inline auto covers(const Region& r, const RECT& rect) -> bool {
    for (std::size_t i = 0; i < r.count; ++i) {
        const RECT& o = r.rects[i];
        if (rect.left >= o.left && rect.top >= o.top && rect.right <= o.right && rect.bottom <= o.bottom) {
            return true;
        }
    }
    return false;
}

// The region to set this frame, if any (motion rework, task 5; MOTION_SPEC § 5.3: twice a
// transition, not every frame). While something moves, the region only grows: to what was, what
// is and what will be (`target`) at once, when what is drawn now or the target is not inside it.
// At rest: exactly `wanted`. Nothing when the region already is what it should be.
inline auto next_region(const Region& current, bool on, const Region& wanted, const Region& target, bool moving)
    -> std::optional<Region> {
    if (!moving) {
        if (on && same(current, wanted)) {
            return std::nullopt;
        }
        return wanted;
    }
    const auto inside = [&](const Region& r) {
        for (std::size_t i = 0; i < r.count; ++i) {
            if (!covers(current, r.rects[i])) {
                return false;
            }
        }
        return true;
    };
    if (on && inside(wanted) && inside(target)) {
        return std::nullopt;
    }
    Region grown = on ? current : Region{};
    const auto add = [&](const RECT& rect) {
        if (grown.count < grown.rects.size()) {
            grown.rects[grown.count++] = rect;
        } else {
            auto& last = grown.rects.back();
            last = {std::min(last.left, rect.left), std::min(last.top, rect.top), std::max(last.right, rect.right),
                    std::max(last.bottom, rect.bottom)};
        }
    };
    for (const Region* r : {&wanted, &target}) {
        for (std::size_t i = 0; i < r->count; ++i) {
            if (!covers(grown, r->rects[i])) {
                add(r->rects[i]);
            }
        }
    }
    return grown;
}

inline auto to_logical(const Placement& p, int x, int y) -> LogicalPoint {
    return {static_cast<float>(x) / p.scale, static_cast<float>(y) / p.scale};
}

} // namespace app::placement
