#include "ui/render/target.hpp"

#include "core/fs.hpp"
#include "core/mem/mem.hpp"

#include <algorithm>
#include <cstring>
#include <string>

namespace {

auto png_tag() -> core::mem::TagId {
    static const core::mem::TagId tag = core::mem::tag_id("ui/png");
    return tag;
}

void* png_alloc(std::size_t size) {
    core::mem::TagScope scope{png_tag()};
    return core::mem::allocate(size);
}

} // namespace

// stb_image_write's buffers go through core::mem like every other C library's (09-BUILD).
#define STBIW_MALLOC(size) png_alloc(size)
#define STBIW_REALLOC(p, size) core::mem::reallocate(p, size)
#define STBIW_FREE(p) core::mem::deallocate(p)
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include <stb_image_write.h>

namespace ui::render {

auto Target::create(int width, int height, int levels) -> core::Result<Target> {
    if (width <= 0 || height <= 0) {
        return core::make_error(core::Code::InvalidArgument, "render target of zero size");
    }
    Target t;
    t.width_ = width;
    t.height_ = height;
    GLuint texture = 0;
    glCreateTextures(GL_TEXTURE_2D, 1, &texture);
    t.color_ = gl::Texture{texture};
    glTextureStorage2D(texture, levels < 1 ? 1 : levels, GL_RGBA8, width, height);
    GLuint framebuffer = 0;
    glCreateFramebuffers(1, &framebuffer);
    t.framebuffer_ = gl::Framebuffer{framebuffer};
    glNamedFramebufferTexture(framebuffer, GL_COLOR_ATTACHMENT0, texture, 0);
    if (glCheckNamedFramebufferStatus(framebuffer, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        return core::make_error(core::Code::Internal, "render target framebuffer incomplete");
    }
    return t;
}

void Target::generate_mipmaps() const {
    glGenerateTextureMipmap(color_.get());
}

void Target::bind() const {
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_.get());
    glViewport(0, 0, width_, height_);
}

void Target::copy_to_bound() const {
    GLint into = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &into);
    glBlitNamedFramebuffer(framebuffer_.get(), static_cast<GLuint>(into), 0, 0, width_, height_, 0, 0, width_, height_, GL_COLOR_BUFFER_BIT,
                           GL_NEAREST);
}

void Target::clear() const {
    constexpr float kTransparent[4] = {0, 0, 0, 0};
    glClearNamedFramebufferfv(framebuffer_.get(), GL_COLOR, 0, kTransparent);
}

auto Target::pixel(int x, int y) const -> std::array<std::uint8_t, 4> {
    std::array<std::uint8_t, 4> rgba{};
    // GL rows run bottom to top.
    glGetTextureSubImage(color_.get(), 0, x, height_ - 1 - y, 0, 1, 1, 1, GL_RGBA,
                         GL_UNSIGNED_BYTE, static_cast<GLsizei>(rgba.size()), rgba.data());
    return rgba;
}

auto Target::read() const -> std::vector<std::uint8_t> {
    std::vector<std::uint8_t> rows;
    read_into(rows);
    return rows;
}

void Target::read_into(std::vector<std::uint8_t>& out) const {
    const auto row = static_cast<std::size_t>(width_) * 4;
    const auto h = static_cast<std::size_t>(height_);
    out.resize(row * h); // its room kept from the last read: nothing allocated again
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTextureImage(color_.get(), 0, GL_RGBA, GL_UNSIGNED_BYTE, static_cast<GLsizei>(out.size()), out.data());
    for (std::size_t y = 0; y < h / 2; ++y) { // GL's rows bottom to top: turned in place
        std::swap_ranges(out.begin() + static_cast<std::ptrdiff_t>(y * row), out.begin() + static_cast<std::ptrdiff_t>((y + 1) * row),
                         out.begin() + static_cast<std::ptrdiff_t>((h - 1 - y) * row));
    }
}

auto Target::save_png(const std::filesystem::path& file) const -> core::Result<void> {
    const auto pixels = read();
    std::string png;
    const auto append = [](void* context, void* data, int size) {
        static_cast<std::string*>(context)->append(static_cast<const char*>(data),
                                                   static_cast<std::size_t>(size));
    };
    if (stbi_write_png_to_func(append, &png, width_, height_, 4, pixels.data(), width_ * 4) == 0) {
        return core::make_error(core::Code::Internal, "PNG encoding failed");
    }
    return core::fs::write_file_atomic(file, png);
}

} // namespace ui::render
