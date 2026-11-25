#include "app/tray_icon.hpp"
#include "core/product.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace app::tray_icon {
namespace {

struct BitmapTraits {
    using type = HBITMAP;
    static auto invalid() noexcept -> HBITMAP { return nullptr; }
    static void close(HBITMAP h) noexcept { DeleteObject(h); }
};
using Bitmap = core::Handle<BitmapTraits>;

} // namespace

auto tip(const sheet::ModelChoice& choice) -> std::wstring {
    const std::string_view level = sheet::thinking_name(choice.level);
    const std::string_view name = sheet::model_of(choice).short_name; // the catalog's ASCII
    return std::wstring{core::product::kNameW} + L" · " + std::wstring{name.begin(), name.end()} + L" · " + std::wstring{level.begin(), level.end()};
}

auto make(std::span<const std::uint8_t> rgba, int size) -> core::Result<Icon> {
    if (size <= 0 || rgba.size() < static_cast<std::size_t>(size) * static_cast<std::size_t>(size) * 4) {
        return core::make_error(core::Code::InvalidArgument, "the icon's frame is smaller than its size");
    }
    BITMAPV5HEADER h{};
    h.bV5Size = sizeof(h);
    h.bV5Width = size;
    h.bV5Height = -size; // top down
    h.bV5Planes = 1;
    h.bV5BitCount = 32;
    h.bV5Compression = BI_BITFIELDS;
    h.bV5RedMask = 0x00FF0000;
    h.bV5GreenMask = 0x0000FF00;
    h.bV5BlueMask = 0x000000FF;
    h.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    const Bitmap color{CreateDIBSection(dc, reinterpret_cast<const BITMAPINFO*>(&h), DIB_RGB_COLORS, &bits, nullptr, 0)};
    ReleaseDC(nullptr, dc);
    if (!color || bits == nullptr) {
        return core::os_error(GetLastError(), "the icon's bitmap");
    }
    auto* out = static_cast<std::uint8_t*>(bits);
    for (std::size_t i = 0; i < static_cast<std::size_t>(size) * static_cast<std::size_t>(size); ++i) {
        out[i * 4 + 0] = rgba[i * 4 + 2]; // BGRA, straight alpha (a 32-bit icon's)
        out[i * 4 + 1] = rgba[i * 4 + 1];
        out[i * 4 + 2] = rgba[i * 4 + 0];
        out[i * 4 + 3] = rgba[i * 4 + 3];
    }
    const Bitmap mask{CreateBitmap(size, size, 1, 1, nullptr)}; // the alpha says it all
    ICONINFO info{TRUE, 0, 0, mask.get(), color.get()};
    HICON icon = CreateIconIndirect(&info); // copies both bitmaps: theirs go when they do
    if (icon == nullptr) {
        return core::os_error(GetLastError(), "the icon");
    }
    return Icon{icon};
}

auto shrink(std::span<const std::uint8_t> rgba, int from, int to) -> std::vector<std::uint8_t> {
    std::vector<std::uint8_t> out;
    shrink_into(rgba, from, to, out);
    return out;
}

void shrink_into(std::span<const std::uint8_t> rgba, int from, int to, std::vector<std::uint8_t>& out) {
    const auto side = static_cast<std::size_t>(std::max(to, 0));
    out.assign(side * side * 4, 0); // into its room: nothing allocated once it is there
    if (from <= 0 || to <= 0 || rgba.size() < static_cast<std::size_t>(from) * static_cast<std::size_t>(from) * 4) {
        return;
    }
    for (int y = 0; y < to; ++y) {
        const int y0 = y * from / to;
        const int y1 = std::max(y0 + 1, (y + 1) * from / to);
        for (int x = 0; x < to; ++x) {
            const int x0 = x * from / to;
            const int x1 = std::max(x0 + 1, (x + 1) * from / to);
            double r = 0, g = 0, b = 0, a = 0;
            int n = 0;
            for (int sy = y0; sy < y1; ++sy) {
                for (int sx = x0; sx < x1; ++sx) {
                    const auto* p = &rgba[static_cast<std::size_t>(sy * from + sx) * 4];
                    const double pa = p[3];
                    r += p[0] * pa;
                    g += p[1] * pa;
                    b += p[2] * pa;
                    a += pa;
                    ++n;
                }
            }
            auto* q = &out[static_cast<std::size_t>(y * to + x) * 4];
            if (a > 0) {
                q[0] = static_cast<std::uint8_t>(std::lround(r / a));
                q[1] = static_cast<std::uint8_t>(std::lround(g / a));
                q[2] = static_cast<std::uint8_t>(std::lround(b / a));
            }
            q[3] = static_cast<std::uint8_t>(std::lround(a / n));
        }
    }
}

auto pose(int frame) noexcept -> Pose {
    if (frame == kStill) {
        return {};
    }
    // One period of the spike's breath; the body turns a little and back, so the last frame meets the first.
    const float a = 6.2831853f * static_cast<float>(frame) / static_cast<float>(kBreath);
    return {.spike = 0.45f + 0.25f * std::sin(a), .phase = 2.4f + 0.35f * std::sin(a)};
}

void Animator::start(const sheet::ModelChoice& choice, int size) {
    // Kept as built even when it fails: the shell does not render it again every pass.
    for_ = choice;
    size_ = size;
    shown_ = kStill - 1;
    failed_ = false;
    breath_.clear();
    breath_.reserve(kBreath); // its room now: growing it allocates nothing
    still_ = {};
    frame_.reserve(static_cast<std::size_t>(size) * static_cast<std::size_t>(size) * 4);
}

auto Animator::keep_still() -> core::Result<void> {
    auto still = make(frame_, size_);
    if (!still) {
        failed_ = true;
        return std::unexpected(still.error());
    }
    still_ = std::move(*still);
    return {};
}

auto Animator::keep_breath() -> core::Result<void> {
    auto icon = make(frame_, size_);
    if (!icon) {
        failed_ = true; // the still stays, nothing tried again every pass
        return std::unexpected(icon.error());
    }
    breath_.push_back(std::move(*icon));
    return {};
}

auto Animator::built_for(const sheet::ModelChoice& choice, int size) const noexcept -> bool {
    return for_ && *for_ == choice && size_ == size;
}

auto Animator::due(double now, bool working) noexcept -> HICON {
    if (!still_) {
        return nullptr;
    }
    // While working: the breath's frame of the moment, a new one every 0.1 s (the still until the
    // breath is whole); at rest: the still, once.
    const int frame =
        working && breathing() ? static_cast<int>(static_cast<std::int64_t>(std::floor(now * 10.0)) % kBreath) : kStill;
    if (frame == shown_) {
        return nullptr;
    }
    shown_ = frame;
    return frame == kStill ? still_.get() : breath_[static_cast<std::size_t>(frame)].get();
}

} // namespace app::tray_icon
