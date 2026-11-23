#pragma once

// ui::render::Texture — a picture uploaded from memory (phase 9: the thumbnails of what the model
// was shown). Straight RGBA in, rows top to bottom, held as it is (Renderer::image premultiplies
// what it samples); drawn right side up with the default Uv. Destroy it with its context current.

#include "core/error.hpp"
#include "ui/gl/objects.hpp"

#include <cstdint>
#include <span>

namespace ui::render {

class Texture {
public:
    // `pixels`: width x height x 4 bytes, straight (not premultiplied) alpha.
    static auto rgba(int width, int height, std::span<const std::uint8_t> pixels) -> core::Result<Texture>;

    [[nodiscard]] auto id() const noexcept -> GLuint { return texture_.get(); }
    [[nodiscard]] auto width() const noexcept -> int { return width_; }
    [[nodiscard]] auto height() const noexcept -> int { return height_; }

private:
    gl::Texture texture_;
    int width_ = 0, height_ = 0;
};

} // namespace ui::render
