#include "app/decode_jobs.hpp"

#include "core/utf8.hpp"
#include "tools/docs/docs.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <span>
#include <system_error>

namespace app {

auto dropped_picture(const std::string& path) -> core::Result<tools::ImageOut> {
    const auto w = core::utf8::to_wide(path);
    if (!w) {
        return core::make_error(core::Code::InvalidArgument, "not a file name");
    }
    const std::filesystem::path file{*w};
    std::error_code ec;
    const auto size = std::filesystem::file_size(file, ec);
    if (ec) {
        return core::make_error(core::Code::NotFound, "the file cannot be read");
    }
    if (size > kMaxDroppedPicture) { // never read whole: a "picture" may be gigabytes
        return core::make_error(core::Code::InvalidArgument,
                                std::format("too large to attach ({} MB, at most {} MB)", size >> 20, kMaxDroppedPicture >> 20));
    }
    std::ifstream in{file, std::ios::binary};
    const std::string bytes{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    auto picture = tools::image::decode(std::as_bytes(std::span{bytes.data(), bytes.size()}), tools::image::kHighSide);
    if (!picture) {
        return std::unexpected{picture.error()};
    }
    const auto slash = path.find_last_of("\\/");
    auto made = tools::image::for_model(*picture, true, slash == std::string::npos ? path : path.substr(slash + 1));
    if (made) {
        made->path = path;
    }
    return made;
}

auto extension_of(std::string_view path) -> std::string {
    const auto dot = path.find_last_of('.');
    if (dot == std::string_view::npos || path.find_first_of("\\/", dot) != std::string_view::npos) {
        return {};
    }
    std::string ext{path.substr(dot + 1)};
    std::ranges::transform(ext, ext.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
    return ext;
}

auto file_details(const std::string& path) -> std::string {
    const auto w = core::utf8::to_wide(path);
    if (!w) {
        return {};
    }
    std::error_code ec;
    const auto size = std::filesystem::file_size(std::filesystem::path{*w}, ec);
    if (ec) {
        return {};
    }
    const auto ext = extension_of(path);
    int pages = 0;
    int sheets = 0;
    if (ext == "pdf") {
        if (const auto n = tools::docs::pdf_page_count(*w)) {
            pages = *n;
        }
    } else if ((ext == "xlsx" || ext == "xlsm") && size <= tools::docs::kMaxDocument) {
        std::ifstream in{std::filesystem::path{*w}, std::ios::binary};
        const std::string bytes{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
        if (auto zip = tools::docs::ZipReader::open(std::as_bytes(std::span{bytes.data(), bytes.size()}))) {
            if (const auto names = tools::docs::xlsx_sheet_names(*zip)) {
                sheets = static_cast<int>(names->size());
            }
        }
    }
    return details_line(ext, size, 0, 0, pages, sheets);
}

} // namespace app
