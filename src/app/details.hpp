#pragma once

// app::details_line — what helps to tell an attachment is the right file (phase 10, screen 17):
// "PNG · 1920×1200 · 1.4 MB", "48 pages · 2.3 MB", "3 sheets · 41 KB", "DOCX · 120 KB". Parts that
// are 0 are left out: a picture's pixels, a PDF's pages, a workbook's sheets, the size.

#include <cstdint>
#include <string>
#include <string_view>

namespace app {

[[nodiscard]] auto details_line(std::string_view ext, std::uint64_t bytes, int width, int height, int pages, int sheets) -> std::string;

} // namespace app
