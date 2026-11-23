#pragma once

// The document tools' functions, for the table in all.cpp (phase 9).

#include "tools/tool.hpp"

#include <optional>
#include <set>

namespace tools::documents {

auto read_schema() -> std::string;
auto read_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request>;
auto read_run(const security::Permit& permit, std::string_view args) -> Outcome;

// Which pages of a PDF of `pages` pages a doc.read takes (phase 10): the ones asked for, or a part of
// kPartPages (1 when null; past the end, the last). Without a map (`pieces`, the filter's, are not
// its pages) the text is read whole as before and `note` says why.
inline constexpr int kPartPages = 12;
struct PdfPlan {
    bool mapped = true;
    std::string note;
    std::vector<int> pages;
    int part = 0;  // 0: by pages asked for
    int parts = 0;
    std::optional<int> next_part;
};
auto pdf_plan(int pages, std::size_t pieces, std::optional<int> part, const std::optional<std::set<int>>& wanted) -> PdfPlan;

auto write_schema() -> std::string;
auto write_request(std::string_view args, const std::filesystem::path& wd) -> core::Result<security::Request>;
auto write_run(const security::Permit& permit, std::string_view args) -> Outcome;

} // namespace tools::documents
