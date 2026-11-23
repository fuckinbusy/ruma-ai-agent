#pragma once

// theme::Color — a colour as the renderer takes it: 8 bits a channel, straight (not premultiplied)
// alpha. It lives here because the theme module owns every colour of the agent; the renderer's
// ui::render::Color is this type.

#include <cstdint>

namespace theme {

struct Color {
    std::uint8_t r = 0, g = 0, b = 0, a = 0;
};

} // namespace theme
