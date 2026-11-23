#include "ui/render/texture.hpp"

namespace ui::render {

auto Texture::rgba(int width, int height, std::span<const std::uint8_t> pixels) -> core::Result<Texture> {
    if (width <= 0 || height <= 0 || pixels.size() != static_cast<std::size_t>(width) * height * 4) {
        return core::make_error(core::Code::InvalidArgument, "a texture's pixels do not match its size");
    }
    GLuint id = 0;
    glCreateTextures(GL_TEXTURE_2D, 1, &id);
    if (id == 0) {
        return core::make_error(core::Code::Io, "no texture for the picture");
    }
    Texture t;
    t.texture_ = gl::Texture{id};
    t.width_ = width;
    t.height_ = height;
    glTextureStorage2D(id, 1, GL_RGBA8, width, height);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTextureSubImage2D(id, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glTextureParameteri(id, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(id, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(id, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(id, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

} // namespace ui::render
