#include "app/details.hpp"

#include <algorithm>
#include <format>

namespace app {
namespace {

// "900 B", "41 KB", "2.3 MB", "3.0 GB".
auto size_text(std::uint64_t b) -> std::string {
    const auto d = static_cast<double>(b);
    if (b < 1024) {
        return std::format("{} B", b);
    }
    if (b < (std::uint64_t{1} << 20)) {
        return std::format("{:.0f} KB", d / 1024.0);
    }
    if (b < (std::uint64_t{1} << 30)) {
        return std::format("{:.1f} MB", d / 1048576.0);
    }
    return std::format("{:.1f} GB", d / 1073741824.0);
}

// "png" -> "PNG"; the photo's ".jpg" says JPEG, as the format is named.
auto kind_text(std::string_view ext) -> std::string {
    std::string up{ext};
    std::ranges::transform(up, up.begin(), [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; });
    return up == "JPG" ? "JPEG" : up == "TIF" ? "TIFF" : up;
}

} // namespace

auto details_line(std::string_view ext, std::uint64_t bytes, int width, int height, int pages, int sheets) -> std::string {
    std::string out;
    const auto add = [&](const std::string& part) {
        if (!out.empty()) {
            out += " \xC2\xB7 ";
        }
        out += part;
    };
    if (pages > 0) {
        add(std::format("{} page{}", pages, pages == 1 ? "" : "s"));
    } else if (sheets > 0) {
        add(std::format("{} sheet{}", sheets, sheets == 1 ? "" : "s"));
    } else if (!ext.empty()) {
        add(kind_text(ext));
    }
    if (width > 0 && height > 0) {
        add(std::format("{}\xC3\x97{}", width, height));
    }
    if (bytes > 0) {
        add(size_text(bytes));
    }
    return out;
}

} // namespace app
