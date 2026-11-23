#include "ui/text/fonts.hpp"

#include "core/fs.hpp"
#include "core/log.hpp"
#include "core/utf8.hpp"
#include "core/win.hpp"

#include <hb-ot.h>
#include <hb.h>

#include FT_MULTIPLE_MASTERS_H
#include FT_SIZES_H
#include FT_MODULE_H

#include <algorithm>
#include <array>
#include <atomic>
#include <utility>
#include <vector>

namespace ui::text {

// One font file (or one face of a collection), mapped read-only, with its sizes.
struct FontSet::Font {
    FT_Library library = nullptr; // referenced: done after the face
    core::win::FileHandle file;
    core::win::KernelHandle mapping;
    const void* view = nullptr;
    std::size_t bytes = 0;
    FT_Face face = nullptr;
    hb_blob_t* blob = nullptr;
    hb_face_t* hb_face = nullptr;
    std::vector<hb_variation_t> variations;
    std::string family;
    std::vector<std::unique_ptr<SizedFont>> sizes; // stable addresses for sized()

    Font() = default;
    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;
    // HarfBuzz and FreeType read the mapped bytes: both go before the view is unmapped; the
    // file and mapping handles close after this body.
    ~Font() {
        for (auto& s : sizes) {
            hb_font_destroy(s->hb); // FT sizes go with the face
        }
        hb_face_destroy(hb_face);
        hb_blob_destroy(blob);
        if (face != nullptr) {
            FT_Done_Face(face);
        }
        if (library != nullptr) {
            FT_Done_Library(library); // drops our reference; the last one frees it
        }
        if (view != nullptr) {
            UnmapViewOfFile(view);
        }
    }
};

namespace {

// Sets the variation axes we care about on a FreeType face, keeping the others at default.
void apply_variations(FT_Library library, FT_Face face, const FontSet::Source& source) {
    if (!FT_HAS_MULTIPLE_MASTERS(face)) {
        return;
    }
    FT_MM_Var* mm = nullptr;
    if (FT_Get_MM_Var(face, &mm) != 0) {
        return;
    }
    std::array<FT_Fixed, 16> coords{};
    const auto n = std::min<FT_UInt>(mm->num_axis, static_cast<FT_UInt>(coords.size()));
    for (FT_UInt i = 0; i < n; ++i) {
        const auto& axis = mm->axis[i];
        float value = static_cast<float>(axis.def) / 65536.0f;
        if (axis.tag == FT_MAKE_TAG('w', 'g', 'h', 't') && source.weight > 0) {
            value = source.weight;
        } else if (axis.tag == FT_MAKE_TAG('o', 'p', 's', 'z') && source.optical_size > 0) {
            value = source.optical_size;
        }
        value = std::clamp(value, static_cast<float>(axis.minimum) / 65536.0f,
                           static_cast<float>(axis.maximum) / 65536.0f);
        coords[i] = static_cast<FT_Fixed>(value * 65536.0f);
    }
    FT_Set_Var_Design_Coordinates(face, n, coords.data());
    FT_Done_MM_Var(library, mm);
}

} // namespace

auto FontSet::open(FontLibrary& library, const Source& source)
    -> core::Result<std::unique_ptr<Font>> {
    auto font = std::make_unique<Font>();
    font->file = core::win::FileHandle{
        CreateFileW(core::fs::os_path(source.file).c_str(), GENERIC_READ, FILE_SHARE_READ,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (!font->file) {
        return core::os_error(GetLastError(), "open font " + source.file.string());
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(font->file.get(), &size) || size.QuadPart <= 0) {
        return core::make_error(core::Code::Io, "font file is empty: " + source.file.string());
    }
    font->bytes = static_cast<std::size_t>(size.QuadPart);
    font->mapping = core::win::KernelHandle{
        CreateFileMappingW(font->file.get(), nullptr, PAGE_READONLY, 0, 0, nullptr)};
    if (!font->mapping) {
        return core::os_error(GetLastError(), "map font " + source.file.string());
    }
    font->view = MapViewOfFile(font->mapping.get(), FILE_MAP_READ, 0, 0, 0);
    if (font->view == nullptr) {
        return core::os_error(GetLastError(), "map font " + source.file.string());
    }

    if (const FT_Error e = FT_New_Memory_Face(
            library.get(), static_cast<const FT_Byte*>(font->view),
            static_cast<FT_Long>(font->bytes), source.face_index, &font->face);
        e != 0) {
        return core::make_error(core::Code::Parse,
                                "FreeType cannot read " + source.file.string() + ": " +
                                    (FT_Error_String(e) ? FT_Error_String(e) : "unknown error"));
    }
    FT_Reference_Library(library.get());
    font->library = library.get();
    apply_variations(library.get(), font->face, source);
    font->family = font->face->family_name ? font->face->family_name : "";

    font->blob = hb_blob_create(static_cast<const char*>(font->view),
                                static_cast<unsigned>(font->bytes), HB_MEMORY_MODE_READONLY,
                                nullptr, nullptr);
    font->hb_face = hb_face_create(font->blob, static_cast<unsigned>(source.face_index));
    if (source.weight > 0) {
        font->variations.push_back({HB_TAG('w', 'g', 'h', 't'), source.weight});
    }
    if (source.optical_size > 0) {
        font->variations.push_back({HB_TAG('o', 'p', 's', 'z'), source.optical_size});
    }
    return font;
}

namespace {
std::atomic<std::uint64_t> g_next_font_set_id{1};
} // namespace

FontSet::FontSet() : id_(g_next_font_set_id.fetch_add(1, std::memory_order_relaxed)) {}

auto FontSet::create(FontLibrary& library, std::span<const Source> sources)
    -> core::Result<FontSet> {
    FontSet set;
    for (const auto& source : sources) {
        auto font = open(library, source);
        if (!font) {
            WA_WARN("text", "font skipped: {}", font.error().message);
            continue;
        }
        if (set.fonts_.size() == 255) {
            break;
        }
        set.fonts_.push_back(std::move(*font));
    }
    if (set.fonts_.empty()) {
        return core::make_error(core::Code::NotFound, "no usable font in the fallback chain");
    }
    return set;
}

namespace {

auto fonts_directory() -> core::Result<std::filesystem::path> {
    wchar_t windows[MAX_PATH + 1] = {};
    const UINT length = GetWindowsDirectoryW(windows, MAX_PATH);
    if (length == 0 || length > MAX_PATH) {
        return core::os_error(GetLastError(), "GetWindowsDirectoryW");
    }
    return std::filesystem::path{windows} / L"Fonts";
}

} // namespace

auto FontSet::system_icons(FontLibrary& library) -> core::Result<FontSet> {
    auto fonts = fonts_directory();
    if (!fonts) {
        return std::unexpected{fonts.error()};
    }
    const Source chain[] = {{*fonts / L"SegoeIcons.ttf"}};
    return create(library, chain);
}

auto FontSet::system_ui(FontLibrary& library, float weight, float optical_size)
    -> core::Result<FontSet> {
    auto dir = fonts_directory();
    if (!dir) {
        return std::unexpected{dir.error()};
    }
    const auto& fonts = *dir;
    // The fallbacks have no weight axis: bold CJK falls back to regular YaHei.
    const Source chain[] = {
        {fonts / L"SegUIVar.ttf", 0, weight, optical_size},
        {fonts / L"segoeui.ttf"},
        {fonts / L"seguiemj.ttf"}, // before Symbol, which has monochrome copies of many emoji
        {fonts / L"seguisym.ttf"},
        {fonts / L"msyh.ttc", 0},
    };
    return create(library, chain);
}

auto FontSet::system_italic(FontLibrary& library, bool semibold) -> core::Result<FontSet> {
    auto dir = fonts_directory();
    if (!dir) {
        return std::unexpected{dir.error()};
    }
    const auto& fonts = *dir;
    // Segoe UI Variable has no italic: the static Segoe UI ones, then the UI chain's fallbacks.
    const Source chain[] = {
        {fonts / (semibold ? L"seguisbi.ttf" : L"segoeuii.ttf")},
        {fonts / L"seguiemj.ttf"},
        {fonts / L"seguisym.ttf"},
        {fonts / L"msyh.ttc", 0},
    };
    return create(library, chain);
}

auto FontSet::system_mono(FontLibrary& library) -> core::Result<FontSet> {
    auto dir = fonts_directory();
    if (!dir) {
        return std::unexpected{dir.error()};
    }
    // Only fonts that are there: a missing one would be a warning on every start.
    std::vector<Source> chain;
    for (const wchar_t* name : {L"CascadiaMono.ttf", L"consola.ttf", L"segoeui.ttf", L"seguisym.ttf", L"msyh.ttc"}) {
        auto path = *dir / name;
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            chain.push_back({std::move(path)});
        }
    }
    return create(library, chain);
}

FontSet::~FontSet() {
    hb_buffer_destroy(buffer_); // accepts null
}

// A moved-to set is new content: it takes the source's id, and the emptied source gets a fresh one.
FontSet::FontSet(FontSet&& other) noexcept
    : fonts_(std::move(other.fonts_)), buffer_(std::exchange(other.buffer_, nullptr)),
      id_(std::exchange(other.id_, g_next_font_set_id.fetch_add(1, std::memory_order_relaxed))) {}

FontSet& FontSet::operator=(FontSet&& other) noexcept {
    if (this != &other) {
        hb_buffer_destroy(buffer_);
        fonts_ = std::move(other.fonts_);
        buffer_ = std::exchange(other.buffer_, nullptr);
        id_ = std::exchange(other.id_, g_next_font_set_id.fetch_add(1, std::memory_order_relaxed));
    }
    return *this;
}

auto FontSet::size() const noexcept -> FontIndex {
    return static_cast<FontIndex>(fonts_.size());
}

auto FontSet::family(FontIndex font) const -> std::string_view {
    return fonts_[font]->family;
}

auto FontSet::has_glyph(FontIndex font, char32_t cp) const -> bool {
    return FT_Get_Char_Index(fonts_[font]->face, cp) != 0;
}

auto FontSet::resolve(char32_t cp) const -> FontIndex {
    for (FontIndex i = 0; i < size(); ++i) {
        if (has_glyph(i, cp)) {
            return i;
        }
    }
    return 0;
}

auto FontSet::sized(FontIndex index, float px) -> const SizedFont& {
    Font& font = *fonts_[index];
    for (const auto& s : font.sizes) {
        if (s->px == px) {
            return *s;
        }
    }
    auto s = std::make_unique<SizedFont>();
    s->px = px;
    s->hb = hb_font_create(font.hb_face);
    const int scale = static_cast<int>(px * 64.0f);
    hb_font_set_scale(s->hb, scale, scale);
    if (!font.variations.empty()) {
        hb_font_set_variations(s->hb, font.variations.data(),
                               static_cast<unsigned>(font.variations.size()));
    }
    hb_font_extents_t extents{};
    hb_font_get_h_extents(s->hb, &extents);
    s->ascent = static_cast<float>(extents.ascender) / 64.0f;
    s->descent = static_cast<float>(-extents.descender) / 64.0f;
    s->line_height = s->ascent + s->descent + static_cast<float>(extents.line_gap) / 64.0f;

    if (FT_New_Size(font.face, &s->ft_size) == 0) {
        FT_Activate_Size(s->ft_size);
        FT_Set_Char_Size(font.face, 0, static_cast<FT_F26Dot6>(scale), 72, 72);
    } else {
        s->ft_size = nullptr;
    }
    font.sizes.push_back(std::move(s));
    return *font.sizes.back();
}

auto FontSet::render_glyph(FontIndex index, float px, std::uint32_t glyph) -> FT_GlyphSlot {
    const SizedFont& s = sized(index, px);
    if (s.ft_size == nullptr) {
        return nullptr;
    }
    FT_Face face = fonts_[index]->face;
    FT_Activate_Size(s.ft_size);
    if (FT_Load_Glyph(face, glyph, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) != 0) {
        return nullptr;
    }
    return face->glyph;
}

auto FontSet::scratch_buffer() noexcept -> hb_buffer_t* {
    if (buffer_ == nullptr) {
        buffer_ = hb_buffer_create();
    }
    return buffer_;
}

} // namespace ui::text
