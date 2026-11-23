#pragma once

// RAII owners for OpenGL objects (core::Handle traits). Destroy them while their context is
// current.

#include "core/mem/handle.hpp"

#include <glad/glad.h>

namespace ui::gl {

struct ProgramTraits {
    using type = GLuint;
    static constexpr auto invalid() noexcept -> GLuint { return 0; }
    static void close(GLuint id) noexcept { glDeleteProgram(id); }
};
struct ShaderTraits {
    using type = GLuint;
    static constexpr auto invalid() noexcept -> GLuint { return 0; }
    static void close(GLuint id) noexcept { glDeleteShader(id); }
};
struct BufferTraits {
    using type = GLuint;
    static constexpr auto invalid() noexcept -> GLuint { return 0; }
    static void close(GLuint id) noexcept { glDeleteBuffers(1, &id); }
};
struct TextureTraits {
    using type = GLuint;
    static constexpr auto invalid() noexcept -> GLuint { return 0; }
    static void close(GLuint id) noexcept { glDeleteTextures(1, &id); }
};
struct FramebufferTraits {
    using type = GLuint;
    static constexpr auto invalid() noexcept -> GLuint { return 0; }
    static void close(GLuint id) noexcept { glDeleteFramebuffers(1, &id); }
};
struct VertexArrayTraits {
    using type = GLuint;
    static constexpr auto invalid() noexcept -> GLuint { return 0; }
    static void close(GLuint id) noexcept { glDeleteVertexArrays(1, &id); }
};
struct SyncTraits {
    using type = GLsync;
    static auto invalid() noexcept -> GLsync { return nullptr; }
    static void close(GLsync s) noexcept { glDeleteSync(s); }
};

using Program = core::Handle<ProgramTraits>;
using Shader = core::Handle<ShaderTraits>;
using Buffer = core::Handle<BufferTraits>;
using Texture = core::Handle<TextureTraits>;
using Framebuffer = core::Handle<FramebufferTraits>;
using VertexArray = core::Handle<VertexArrayTraits>;
using Sync = core::Handle<SyncTraits>;

} // namespace ui::gl
