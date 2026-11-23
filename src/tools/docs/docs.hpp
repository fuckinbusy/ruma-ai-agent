#pragma once

// tools::docs — documents read and written on this machine (phase 9): ZIP (miniz) for the Office
// Open XML files, a small XML reader of our own, the PDF filter and renderer Windows has.

#include "core/error.hpp"
#include "tools/image.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tools::docs {

inline constexpr std::size_t kMaxPart = 16u << 20;     // one part of a ZIP, inflated (a long contract's body)
inline constexpr std::size_t kMaxDocument = 100u << 20; // a document file
inline constexpr std::size_t kMaxInflated = 64u << 20;  // all one reader inflates, its parts together

// ---- ZIP

class ZipReader {
public:
    static auto open(std::span<const std::byte> zip) -> core::Result<ZipReader>;
    ZipReader(ZipReader&&) noexcept;
    auto operator=(ZipReader&&) noexcept -> ZipReader&;
    ~ZipReader();

    // A part by its name ("word/document.xml"). Refused before inflating when its stated size is
    // past `max` or would take what this reader has inflated past kMaxInflated (a ZIP bomb, one
    // part asked for again and again), or when the name climbs (".."), is absolute or uses '\'.
    auto read(std::string_view name, std::size_t max = kMaxPart) -> core::Result<std::string>;
    // The first `bytes` of a part, however large it is (a sheet's <dimension> before its cells);
    // empty when there is no such part (phase 10).
    [[nodiscard]] auto head(std::string_view name, std::size_t bytes) -> std::string;
    [[nodiscard]] auto names() const -> std::vector<std::string>;

private:
    struct State;
    explicit ZipReader(std::unique_ptr<State> s);
    std::unique_ptr<State> state_;
};

class ZipWriter {
public:
    void add(std::string_view name, std::string_view data);
    auto finish() -> core::Result<std::string>;

private:
    std::vector<std::pair<std::string, std::string>> parts_;
};

// ---- XML: a pull reader of elements, attributes and text (the five entities and &#…;). A
// DOCTYPE (and with it any entity of its own) is an error: no expansion, no billion laughs.

class Xml {
public:
    enum class Event : std::uint8_t { Start, End, Text, Done, Error };
    explicit Xml(std::string_view text) : text_(text) {}

    auto next() -> Event;
    // The element's name without its prefix ("w:t" -> "t"), for Start and End.
    [[nodiscard]] auto name() const -> std::string_view { return name_; }
    // An attribute of the element just started, by its name without the prefix; empty if none.
    // A prefixed name ("r:id") matches exactly — an element may have both "id" and "r:id".
    [[nodiscard]] auto attribute(std::string_view name) const -> std::string;
    // Text, entities resolved.
    [[nodiscard]] auto text() const -> const std::string& { return value_; }
    [[nodiscard]] auto error() const -> const std::string& { return error_; }

private:
    std::string_view text_;
    std::size_t at_ = 0;
    std::string_view name_;
    std::string_view attributes_;
    std::string value_;
    std::string error_;
    bool closing_ = false; // a self-closing element owes its End
};

// ---- Office Open XML

struct Sheet {
    std::string name;
    std::vector<std::vector<std::string>> rows; // the first is the header when there is one
    bool skipped = false; // past the limit: not read, the other sheets are (phase 10)
    std::string facts;    // then its size: "1 048 576 rows × 16 384 columns"
};

// Paragraphs, "Heading N" as #, lists as -, tables as Markdown tables.
auto docx_markdown(ZipReader& zip) -> core::Result<std::string>;
// Each sheet's cells: shared strings, numbers, dates by their style (YYYY-MM-DD), formulas'
// values as saved; at most `max_rows` a sheet.
auto xlsx_sheets(ZipReader& zip, std::size_t max_rows) -> core::Result<std::vector<Sheet>>;
// The sheets' names, in order, from the workbook alone (no cell is read).
auto xlsx_sheet_names(ZipReader& zip) -> core::Result<std::vector<std::string>>;
// Each slide's text, in order.
auto pptx_slides(ZipReader& zip) -> core::Result<std::vector<std::string>>;

// ---- CSV (RFC 4180: quotes, "", line breaks in quotes; ',' or ';' as the first line uses)

auto csv_rows(std::string_view text, std::size_t max_rows) -> std::vector<std::vector<std::string>>;

// ---- Writing (doc.write, the panel's XLSX and CSV)

enum class ColumnKind : std::uint8_t { Text, Number, Size, Money, Percent, Date };
struct Table {
    std::vector<std::pair<std::string, ColumnKind>> columns;
    std::vector<std::vector<std::string>> rows;
};
// What a column's values read as: "1.5 MB" Size, "$12" or "¥ 3 200.00" Money, "45%" Percent,
// "12.5" Number, "2026-09-01" or "01.09.2026" Date (phase 10).
auto kind_of(std::span<const std::string> values) -> ColumnKind;

// A Word document: headings 1-3, paragraphs with bold, italic, code and links, lists two levels
// deep, tables, quotes, rules (Normal in Calibri 11, Heading 1-3).
auto docx_from_markdown(std::string_view markdown) -> core::Result<std::string>;
// One sheet, its header bold: sizes in MB ("<title> (MB)"), money as $0.00, percents as 0%; a
// cell that is not a number is text, never a formula.
auto xlsx_from_table(const Table& table) -> core::Result<std::string>;
// UTF-8 with a BOM, CRLF, each cell through csv_cell.
auto csv_from_table(const Table& table) -> std::string;
// One CSV cell: a text starting with = + - @ that is not a number gets a ' first (no formula when
// a spreadsheet opens it); quoted when it holds , " or a line break.
auto csv_cell(std::string_view cell) -> std::string;

// ---- PDF, through Windows

// The text layer, by the PDF filter Windows has (no page numbers: the filter gives none).
auto pdf_text(const std::wstring& path) -> core::Result<std::string>;
auto pdf_page_count(const std::wstring& path) -> core::Result<int>;
// The same text piece by piece as the filter gives it: one a page (an empty one for a scanned page;
// phase 9's spike) — the caller checks the count against the pages (phase 10).
auto pdf_pages(const std::wstring& path) -> core::Result<std::vector<std::string>>;
// One page (0-based) as a picture at `dpi`, its longer side at most kMaxRenderSide.
inline constexpr int kMaxRenderSide = 4096;
inline constexpr double kMaxPageMeters = 3.0; // a longer side past it: not drawn, kind "page_too_large" (phase 10)
auto pdf_render(const std::wstring& path, int page, int dpi) -> core::Result<Picture>;

} // namespace tools::docs
