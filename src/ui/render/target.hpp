#pragma once

// ui::render::Target — an off-screen RGBA8 framebuffer: render into it, read pixels back, save a
// PNG snapshot (UI tests, ui-gallery). Top-left origin. The pixels are whatever the producer
// wrote: premultiplied from the Renderer, straight alpha from the Orb.

#include "core/error.hpp"
#include "ui/gl/objects.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace ui::render {

class Target {
public:
    // `levels` > 1 allocates mip levels (generate them with generate_mipmaps()).
    static auto create(int width, int height, int levels = 1) -> core::Result<Target>;
    void generate_mipmaps() const;

    // Binds the framebuffer for drawing and sets the viewport to cover it.
    void bind() const;
    // Fills with transparent black.
    void clear() const;

    // Its pixels as they are (no blending, no filtering) into the framebuffer bound for drawing,
    // at its origin: a kept frame put back (phase 9).
    void copy_to_bound() const;

    [[nodiscard]] auto pixel(int x, int y) const -> std::array<std::uint8_t, 4>;
    // Rows top to bottom, 4 bytes per pixel.
    [[nodiscard]] auto read() const -> std::vector<std::uint8_t>;
    // The same into `out`, its room kept: a second read of this size allocates nothing.
    void read_into(std::vector<std::uint8_t>& out) const;
    auto save_png(const std::filesystem::path& file) const -> core::Result<void>;

    [[nodiscard]] auto width() const noexcept -> int { return width_; }
    [[nodiscard]] auto height() const noexcept -> int { return height_; }
    [[nodiscard]] auto texture() const noexcept -> GLuint { return color_.get(); }

private:
    Target() = default;

    gl::Texture color_;
    gl::Framebuffer framebuffer_;
    int width_ = 0;
    int height_ = 0;
};

} // namespace ui::render
