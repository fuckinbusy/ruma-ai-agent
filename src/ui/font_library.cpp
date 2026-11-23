#include "ui/font_library.hpp"

#include "core/mem/mem.hpp"

#include FT_MODULE_H

#include <utility>

namespace ui {
namespace {

auto freetype_tag() -> core::mem::TagId {
    static const core::mem::TagId tag = core::mem::tag_id("ui/freetype");
    return tag;
}

void* ft_alloc(FT_Memory, long size) {
    core::mem::TagScope scope{freetype_tag()};
    return core::mem::allocate(static_cast<std::size_t>(size));
}

void ft_free(FT_Memory, void* block) {
    core::mem::deallocate(block);
}

void* ft_realloc(FT_Memory, long, long new_size, void* block) {
    core::mem::TagScope scope{freetype_tag()};
    return core::mem::reallocate(block, static_cast<std::size_t>(new_size));
}

// FreeType keeps this pointer for the library's whole life, which may outlast the FontLibrary
// object (font sets hold references): a static, stateless record.
FT_MemoryRec_ g_memory{nullptr, ft_alloc, ft_free, ft_realloc};

} // namespace

auto FontLibrary::create() -> core::Result<FontLibrary> {
    FontLibrary result;
    if (const FT_Error e = FT_New_Library(&g_memory, &result.library_); e != 0) {
        return core::make_error(core::Code::Internal,
                                std::string{"FreeType initialization failed: "} +
                                    (FT_Error_String(e) ? FT_Error_String(e) : "unknown error"));
    }
    FT_Add_Default_Modules(result.library_);
    FT_Set_Default_Properties(result.library_);
    return result;
}

FontLibrary::~FontLibrary() {
    if (library_ != nullptr) {
        FT_Done_Library(library_);
    }
}

FontLibrary::FontLibrary(FontLibrary&& other) noexcept
    : library_(std::exchange(other.library_, nullptr)) {}

FontLibrary& FontLibrary::operator=(FontLibrary&& other) noexcept {
    if (this != &other) {
        if (library_ != nullptr) {
            FT_Done_Library(library_);
        }
        library_ = std::exchange(other.library_, nullptr);
    }
    return *this;
}

} // namespace ui
