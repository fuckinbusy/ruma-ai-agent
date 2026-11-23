#include "ui/backdrop.hpp"

#include "core/mem/handle.hpp"

#include <dwmapi.h>

namespace ui::backdrop {
namespace {

struct RegionTraits {
    using type = HRGN;
    static auto invalid() noexcept -> HRGN { return nullptr; }
    static void close(HRGN r) noexcept { DeleteObject(r); }
};

} // namespace

void prepare(HWND hwnd) {
    core::Handle<RegionTraits> empty{CreateRectRgn(0, 0, -1, -1)};
    DWM_BLURBEHIND bb{};
    bb.dwFlags = DWM_BB_ENABLE | DWM_BB_BLURREGION;
    bb.fEnable = TRUE;
    bb.hRgnBlur = empty.get();
    DwmEnableBlurBehindWindow(hwnd, &bb);
    const DWORD corner = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
}

} // namespace ui::backdrop
