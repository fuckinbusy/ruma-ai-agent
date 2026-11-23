#pragma once

// ui::gl::ProgramCache — builds GL programs and keeps their driver binaries on disk, so later
// starts skip compilation (one small program costs ~60 ms to compile, docs/spikes/r1).
// Key: hash of the driver id and both sources; a driver update or an edited shader misses.

#include "core/error.hpp"
#include "ui/gl/objects.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace ui::gl {

class ProgramCache {
public:
    struct Stats {
        std::uint32_t compiled = 0; // built from source
        std::uint32_t loaded = 0;   // restored from a cached binary
    };

    ProgramCache(std::filesystem::path directory, std::string driver_id);

    // `name` is used in error messages only.
    auto build(std::string_view name, std::string_view vertex, std::string_view fragment)
        -> core::Result<Program>;

    [[nodiscard]] auto stats() const noexcept -> const Stats& { return stats_; }
    // False when the driver offers no program binary formats: everything is compiled.
    [[nodiscard]] auto binary_supported() const noexcept -> bool { return binary_supported_; }

private:
    auto load(const std::filesystem::path& file) -> Program;
    void store(const std::filesystem::path& file, GLuint program);

    std::filesystem::path directory_;
    std::string driver_id_;
    bool binary_supported_ = false;
    Stats stats_;
};

} // namespace ui::gl
