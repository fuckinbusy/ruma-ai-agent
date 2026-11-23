#include "ui/gl/program_cache.hpp"

#include "core/fs.hpp"
#include "core/hash.hpp"
#include "core/log.hpp"

#include <cstring>
#include <format>
#include <string>
#include <system_error>
#include <vector>

namespace ui::gl {
namespace {

constexpr std::uint32_t kMagic = 0x42504157; // "WAPB"
constexpr std::uint32_t kVersion = 1;

struct FileHeader {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t format;
    std::uint32_t size;
};

auto shader_log(GLuint shader) -> std::string {
    GLint length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
    std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
    glGetShaderInfoLog(shader, length, nullptr, log.data());
    return log;
}

auto program_log(GLuint program) -> std::string {
    GLint length = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
    std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
    glGetProgramInfoLog(program, length, nullptr, log.data());
    return log;
}

auto compile(GLenum type, std::string_view source, std::string_view name)
    -> core::Result<Shader> {
    Shader shader{glCreateShader(type)};
    const char* text = source.data();
    const auto length = static_cast<GLint>(source.size());
    glShaderSource(shader.get(), 1, &text, &length);
    glCompileShader(shader.get());
    GLint ok = 0;
    glGetShaderiv(shader.get(), GL_COMPILE_STATUS, &ok);
    if (!ok) {
        return core::make_error(core::Code::Internal,
                                std::format("shader '{}' ({}) failed to compile: {}", name,
                                            type == GL_VERTEX_SHADER ? "vertex" : "fragment",
                                            shader_log(shader.get())));
    }
    return shader;
}

} // namespace

ProgramCache::ProgramCache(std::filesystem::path directory, std::string driver_id)
    : directory_(std::move(directory)), driver_id_(std::move(driver_id)) {
    GLint formats = 0;
    glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &formats);
    binary_supported_ = formats > 0;
}

auto ProgramCache::build(std::string_view name, std::string_view vertex,
                         std::string_view fragment) -> core::Result<Program> {
    std::uint64_t key = core::hash::fnv1a64(driver_id_);
    key = core::hash::fnv1a64(std::string_view{"\0", 1}, key);
    key = core::hash::fnv1a64(vertex, key);
    key = core::hash::fnv1a64(std::string_view{"\0", 1}, key);
    key = core::hash::fnv1a64(fragment, key);
    const auto file = directory_ / std::format("{:016x}.bin", key);

    if (binary_supported_) {
        if (Program cached = load(file)) {
            ++stats_.loaded;
            return cached;
        }
    }

    auto vs = compile(GL_VERTEX_SHADER, vertex, name);
    if (!vs) {
        return std::unexpected{vs.error()};
    }
    auto fs = compile(GL_FRAGMENT_SHADER, fragment, name);
    if (!fs) {
        return std::unexpected{fs.error()};
    }
    Program program{glCreateProgram()};
    glProgramParameteri(program.get(), GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
    glAttachShader(program.get(), vs->get());
    glAttachShader(program.get(), fs->get());
    glLinkProgram(program.get());
    glDetachShader(program.get(), vs->get());
    glDetachShader(program.get(), fs->get());
    GLint ok = 0;
    glGetProgramiv(program.get(), GL_LINK_STATUS, &ok);
    if (!ok) {
        return core::make_error(core::Code::Internal,
                                std::format("program '{}' failed to link: {}", name,
                                            program_log(program.get())));
    }
    ++stats_.compiled;
    if (binary_supported_) {
        store(file, program.get());
    }
    return program;
}

auto ProgramCache::load(const std::filesystem::path& file) -> Program {
    auto data = core::fs::read_file(file, 64u << 20);
    if (!data || data->size() < sizeof(FileHeader)) {
        return {};
    }
    FileHeader header{};
    std::memcpy(&header, data->data(), sizeof(header));
    if (header.magic != kMagic || header.version != kVersion ||
        header.size != data->size() - sizeof(FileHeader)) {
        return {};
    }
    Program program{glCreateProgram()};
    glProgramBinary(program.get(), header.format, data->data() + sizeof(FileHeader),
                    static_cast<GLsizei>(header.size));
    GLint ok = 0;
    glGetProgramiv(program.get(), GL_LINK_STATUS, &ok);
    if (!ok) {
        return {}; // stale or foreign binary: the caller compiles and overwrites it
    }
    return program;
}

void ProgramCache::store(const std::filesystem::path& file, GLuint program) {
    GLint length = 0;
    glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &length);
    if (length <= 0) {
        return;
    }
    std::string bytes(sizeof(FileHeader) + static_cast<std::size_t>(length), '\0');
    GLenum format = 0;
    GLsizei written = 0;
    glGetProgramBinary(program, length, &written, &format, bytes.data() + sizeof(FileHeader));
    const FileHeader header{kMagic, kVersion, format, static_cast<std::uint32_t>(written)};
    std::memcpy(bytes.data(), &header, sizeof(header));
    bytes.resize(sizeof(FileHeader) + static_cast<std::size_t>(written));

    std::error_code ec;
    std::filesystem::create_directories(directory_, ec);
    if (auto saved = core::fs::write_file_atomic(file, bytes); !saved) {
        WA_WARN("gl", "program cache not saved: {}", saved.error().message);
    }
}

} // namespace ui::gl
