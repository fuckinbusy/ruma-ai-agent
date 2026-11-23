#pragma once

// ui::FontLibrary — RAII owner of a FreeType library whose memory goes through core::mem
// (tag "ui/freetype"), following the module-boundary rule in docs/01-ARCHITECTURE.md.
// Font sets opened with it hold a reference to the library (FT_Reference_Library), so the two may
// be destroyed in any order. Like FreeType itself, it is used from one thread (the UI thread).

#include "core/error.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SYSTEM_H

#include <memory>

namespace ui {

class FontLibrary {
public:
    static auto create() -> core::Result<FontLibrary>;
    ~FontLibrary();
    FontLibrary(FontLibrary&& other) noexcept;
    FontLibrary& operator=(FontLibrary&& other) noexcept;
    FontLibrary(const FontLibrary&) = delete;
    FontLibrary& operator=(const FontLibrary&) = delete;

    [[nodiscard]] auto get() const noexcept -> FT_Library { return library_; }

private:
    FontLibrary() = default;
    FT_Library library_ = nullptr;
};

} // namespace ui
