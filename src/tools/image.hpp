#pragma once

// tools::image — pictures through Windows' own codecs (WIC, phase 9): any format with a codec in
// (JPEG, PNG, GIF, BMP, TIFF, ICO; WebP and HEIC when Windows has them), turned by its EXIF
// orientation, made smaller, written back; and what the model is shown of one.

#include "core/error.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace tools {

// Pixels, top row first, 4 bytes each: blue, green, red, alpha (not premultiplied).
struct Picture {
    int width = 0, height = 0;
    std::vector<std::uint8_t> bgra;
    int source_width = 0, source_height = 0; // the file's own pixels, turned as shown (0: these)
    int turned = 0; // clockwise degrees its EXIF orientation turned it by at decoding (phase 10)
};

enum class ImageFormat : std::uint8_t { Jpeg, Png, Bmp, Tiff, Gif };

// A picture as the model gets it, and as the window shows it.
struct ImageOut {
    std::string mime;  // image/png or image/jpeg
    std::string bytes; // encoded
    std::string label; // "photo.jpg", "screen"
    std::string path;  // the file it came from (UTF-8); empty for the screen or the clipboard
    int width = 0, height = 0; // what the model sees
    Picture thumb;             // for the window: at most 240 wide and 120 high
    int source_width = 0, source_height = 0; // the file's own pixels (an attachment's details)
};

namespace image {

inline constexpr std::size_t kMaxPixels = 40'000'000; // decoded: 160 MB at most, a larger one is scaled at decoding
inline constexpr int kLowSide = 1024;                 // the model's picture, `detail: low` (the default)
inline constexpr int kHighSide = 2048;                // `detail: high`
inline constexpr std::size_t kMaxSent = 4u << 20;     // encoded, for the model

// The first frame, turned by its EXIF orientation. With `max_side` > 0 (or past kMaxPixels) it is
// scaled down while decoding, so a huge file never lies in memory whole.
auto decode(std::span<const std::byte> file, int max_side = 0) -> core::Result<Picture>;
// Never larger; the longer side at most `max_side` (Fant).
auto fit(const Picture& picture, int max_side) -> Picture;
// Within `width` x `height`, its proportions kept (never larger).
auto fit_box(const Picture& picture, int width, int height) -> Picture;
// Turned clockwise by 90, 180 or 270 degrees.
auto rotate(const Picture& picture, int degrees) -> Picture;
// `quality` 1..100 for JPEG; GIF is written with a palette of its own 256 colours.
auto encode(const Picture& picture, ImageFormat format, int quality = 85) -> core::Result<std::string>;
// For the model: fitted to kLowSide or kHighSide; PNG when it has few colours (a screenshot, a
// diagram), else JPEG q85; under kMaxSent (quality, then size, lowered); with its thumbnail.
auto for_model(const Picture& picture, bool high, std::string label, std::size_t max_bytes = kMaxSent) -> core::Result<ImageOut>;

} // namespace image
} // namespace tools
