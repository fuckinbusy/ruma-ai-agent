#include "tools/doc_tools.hpp"

#include "core/json.hpp"
#include "core/utf8.hpp"
#include "tools/docs/docs.hpp"
#include "tools/fs.hpp"
#include "tools/guarded_fs.hpp"
#include "tools/ocr.hpp"
#include "tools/os.hpp"

#include <glaze/glaze.hpp>

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <format>
#include <optional>
#include <set>

namespace tools::documents::args {

struct Read {
    std::string path;
    std::optional<int> offset;    // in characters
    std::optional<int> max_chars; // 60 000 if null
    std::optional<std::string> pages;
    std::optional<std::string> language; // a scan's language for OCR; null: the user's
    std::optional<int> part;             // a PDF's part of 12 pages, from 1; null: the first
};

struct TableArg {
    std::vector<std::string> columns;
    std::vector<std::vector<std::string>> rows;
};
struct Write {
    std::string path;
    std::string format; // docx, xlsx, csv, md
    std::optional<std::string> markdown;
    std::optional<TableArg> table;
};

} // namespace tools::documents::args

namespace tools::documents::out {

struct SheetSize {
    std::string name;
    std::size_t rows = 0;
};
struct Read {
    std::string format;
    std::string text;
    std::size_t total_chars = 0;
    std::size_t offset = 0;
    std::optional<std::size_t> next_offset; // where the next part starts; absent at the end
    std::optional<int> pages;               // a PDF's pages, a presentation's slides
    bool ocr = false;                        // a scan: read from the page pictures
    std::optional<std::vector<SheetSize>> sheets;
    // A PDF by its parts of 12 pages (phase 10): this one, how many, the next; the pages read from
    // their pictures here; false: no page map (the note says why), its text read whole.
    std::optional<int> part;
    std::optional<int> parts;
    std::optional<int> next_part;
    std::optional<std::vector<int>> scanned_pages;
    std::optional<std::string> note;
    std::optional<std::vector<Refusal>> skipped; // a sheet, a page past the limit: left out, the rest read (phase 10)
};

} // namespace tools::documents::out

template <>
struct glz::json_schema<tools::documents::args::Read> {
    schema path{.description = "The document: PDF (scans too), DOCX, XLSX, PPTX, CSV, TXT or MD."};
    schema offset{.description = "Where to start, in characters (from next_offset of the part before); null: the beginning."};
    schema max_chars{.description = "At most this many characters (60000 if null)."};
    schema pages{.description = "\"3\", \"1-5\", \"2,4-6\": a PDF's pages read from their pictures (a scan, or scanned pages its "
                                  "text lacks), a presentation's slides, a workbook's sheets; null: all (a scan's first 10 pages)."};
    schema language{.description = "A scan's language as a tag (\"en-US\", \"ru\"); null: the user's own."};
    schema part{.description = "A PDF is read in parts of 12 pages: which part, from 1 (next_part of the one before); null: the first. "
                               "Stop when you have the answer. Cite its pages as [p. 7](page:7)."};
};

template <>
struct glz::json_schema<tools::documents::args::Write> {
    schema path{.description = "The new file. An existing one is never written over."};
    schema format{.description = "docx (from markdown), xlsx or csv (from table), md (the markdown as it is).",
                  .enumeration = std::vector<std::string_view>{"docx", "xlsx", "csv", "md"}};
    schema markdown{.description = "For docx and md: the text in Markdown (headings, lists, tables, bold, links); else null."};
    schema table{.description = "For xlsx and csv: {columns: [names], rows: [[cells]]}; sizes like \"1.5 MB\", \"$12\", \"45%\" keep "
                                "their meaning; else null."};
};

namespace tools::documents {
namespace {

constexpr std::size_t kDefaultChars = 60'000;
constexpr std::size_t kBytesOut = 48'000;   // the text's bytes in one answer: within 64 KB once escaped
constexpr std::size_t kMaxRows = 5000;      // a sheet's rows, for the panel
constexpr int kScanDpi = 200;
constexpr int kScanPagesFirst = 10;

template <class T>
auto parse(std::string_view json) -> core::Result<T> {
    T value{};
    if (const auto ec = glz::read<glz::opts{.error_on_unknown_keys = true, .error_on_missing_keys = true}>(value, json)) {
        return core::make_error(core::Code::InvalidArgument, "arguments: " + glz::format_error(ec, json));
    }
    return value;
}

auto utf8(std::wstring_view w) -> std::string {
    auto s = core::utf8::to_utf8(w);
    return s ? std::move(*s) : std::string{"?"};
}

auto chars_of(std::string_view s) -> std::size_t {
    return static_cast<std::size_t>(std::ranges::count_if(s, [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

// The byte where character `n` starts.
auto byte_of(std::string_view s, std::size_t n) -> std::size_t {
    std::size_t seen = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
            if (seen == n) {
                return i;
            }
            ++seen;
        }
    }
    return s.size();
}

// "3", "1-5", "2,4-6" -> 1-based numbers.
auto pages_of(std::string_view spec) -> core::Result<std::set<int>> {
    std::set<int> out;
    while (!spec.empty()) {
        const auto comma = spec.find(',');
        auto part = spec.substr(0, comma);
        spec = comma == std::string_view::npos ? std::string_view{} : spec.substr(comma + 1);
        while (!part.empty() && part.front() == ' ') part.remove_prefix(1);
        while (!part.empty() && part.back() == ' ') part.remove_suffix(1);
        const auto dash = part.find('-');
        int from = 0, to = 0;
        const auto a = part.substr(0, dash);
        const auto b = dash == std::string_view::npos ? a : part.substr(dash + 1);
        const bool ok = std::from_chars(a.data(), a.data() + a.size(), from).ec == std::errc{} &&
                        std::from_chars(b.data(), b.data() + b.size(), to).ec == std::errc{} && from >= 1 && to >= from && to - from < 1000;
        if (!ok) {
            return core::make_error(core::Code::InvalidArgument, "arguments: \"pages\" is like \"3\", \"1-5\" or \"2,4-6\"");
        }
        for (int i = from; i <= to; ++i) {
            out.insert(i);
        }
    }
    return out;
}

auto table_text(const std::vector<std::vector<std::string>>& rows) -> std::string {
    std::string out;
    for (std::size_t r = 0; r < rows.size(); ++r) {
        out += "|";
        for (const auto& cell : rows[r]) {
            std::string c = cell;
            std::ranges::replace(c, '\n', ' ');
            std::ranges::replace(c, '|', '/');
            out += " " + c + " |";
        }
        out += "\n";
        if (r == 0) {
            out += "|";
            for (std::size_t i = 0; i < rows[r].size(); ++i) {
                out += "---|";
            }
            out += "\n";
        }
    }
    return out;
}

struct Read {
    std::string format;
    std::string text;
    std::optional<int> pages;
    bool ocr = false;
    std::vector<DataTable> tables;
    std::optional<PdfPlan> plan;           // a PDF by its pages (phase 10)
    std::optional<DocumentPages> document;
    std::vector<Refusal> skipped;          // phase 10
};

auto unsupported(std::string_view ext) -> core::Error {
    return core::Error{core::Code::InvalidArgument,
                       std::format("the {} format is not supported (save it as .docx, .xlsx or .pptx)", ext)};
}

constexpr std::size_t kTextLayer = 20; // fewer characters on a page: no text layer, a scan

auto read_pdf(const std::wstring& path, const std::optional<std::set<int>>& wanted, std::string_view language, std::optional<int> part)
    -> core::Result<Read> {
    Read out{.format = "pdf"};
    auto count = docs::pdf_page_count(path);
    if (!count) {
        return std::unexpected{count.error()};
    }
    out.pages = *count;
    // Phase 10: by its pages, in parts of 12, a page without a text layer read from its picture here.
    if (auto pieces = docs::pdf_pages(path)) {
        auto plan = pdf_plan(*count, pieces->size(), part, wanted);
        if (plan.mapped) {
            DocumentPages doc{.path = path, .pages = *count, .part_size = kPartPages, .mapped = true, .note = {}, .read = {}};
            for (std::size_t i = 0; i < plan.pages.size(); ++i) {
                if (!proceed()) {
                    return std::unexpected{stopped_error()};
                }
                const int p = plan.pages[i];
                PageRead page{.page = p, .scan = false, .text = std::move((*pieces)[static_cast<std::size_t>(p - 1)])};
                if (chars_of(page.text) < kTextLayer) {
                    auto picture = docs::pdf_render(path, p - 1, kScanDpi);
                    if (!picture && picture.error().kind == "page_too_large") { // left out, the others read (phase 10)
                        out.skipped.push_back({.kind = picture.error().kind, .what = std::format("page {}", p), .facts = picture.error().facts});
                        report(i + 1, plan.pages.size(), path);
                        continue;
                    }
                    if (!picture) {
                        return std::unexpected{picture.error()};
                    }
                    auto read = ocr::recognize(*picture, language);
                    if (!read) {
                        return std::unexpected{read.error()};
                    }
                    page.scan = true;
                    page.text.clear();
                    for (const auto& line : read->lines) {
                        page.text += line.text + "\n";
                    }
                    out.ocr = true;
                }
                out.text += std::format("{}--- page {} ---\n{}\n", out.text.empty() ? "" : "\n", p, page.text);
                doc.read.push_back(std::move(page));
                report(i + 1, plan.pages.size(), path);
            }
            out.document = std::move(doc);
            out.plan = std::move(plan);
            return out;
        }
        out.document = DocumentPages{.path = path, .pages = *count, .part_size = kPartPages, .mapped = false, .note = plan.note, .read = {}};
        out.plan = std::move(plan);
    }
    auto text = docs::pdf_text(path);
    out.text = text ? std::move(*text) : std::string{};
    if (!wanted && chars_of(out.text) >= static_cast<std::size_t>(50 * std::max(1, *count))) {
        return out;
    }
    // A scan, or pages asked for (a scanned page in a document with text): drawn and read here.
    std::vector<int> pages;
    if (wanted) {
        for (const int p : *wanted) {
            if (p <= *count) {
                pages.push_back(p);
            }
        }
    } else {
        for (int p = 1; p <= std::min(*count, kScanPagesFirst); ++p) {
            pages.push_back(p);
        }
    }
    out.ocr = true;
    out.text.clear();
    for (std::size_t i = 0; i < pages.size(); ++i) {
        if (!proceed()) {
            return std::unexpected{stopped_error()};
        }
        auto picture = docs::pdf_render(path, pages[i] - 1, kScanDpi);
        if (!picture && picture.error().kind == "page_too_large") { // left out, the others read (phase 10)
            out.skipped.push_back({.kind = picture.error().kind, .what = std::format("page {}", pages[i]), .facts = picture.error().facts});
            report(i + 1, pages.size(), path);
            continue;
        }
        if (!picture) {
            return std::unexpected{picture.error()};
        }
        auto read = ocr::recognize(*picture, language);
        if (!read) {
            return std::unexpected{read.error()};
        }
        out.text += std::format("{}--- page {} ---\n", out.text.empty() ? "" : "\n", pages[i]);
        for (const auto& line : read->lines) {
            out.text += line.text + "\n";
        }
        report(i + 1, pages.size(), path);
    }
    return out;
}

} // namespace

auto write_schema() -> std::string {
    auto s = glz::write_json_schema<args::Write>();
    return s ? std::move(*s) : std::string{"{}"};
}

auto write_request(std::string_view json, const std::filesystem::path& wd) -> core::Result<security::Request> {
    auto a = parse<args::Write>(json);
    if (!a) {
        return std::unexpected{a.error()};
    }
    if (a->format != "docx" && a->format != "xlsx" && a->format != "csv" && a->format != "md") {
        return core::make_error(core::Code::InvalidArgument, "arguments: \"format\" is docx, xlsx, csv or md");
    }
    auto path = core::utf8::to_wide(a->path);
    if (!path || path->empty()) {
        return core::make_error(core::Code::InvalidArgument, "arguments: \"path\" is empty or not UTF-8");
    }
    auto ext = std::filesystem::path{*path}.extension().string();
    std::ranges::transform(ext, ext.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    if (ext != "." + a->format) {
        return core::make_error(core::Code::InvalidArgument, std::format("arguments: a {} file's name ends in .{}", a->format, a->format));
    }
    return security::Request{.tool = "doc.write", .risk = security::Risk::Write, .paths = {std::move(*path)}, .working_dir = wd};
}

auto write_run(const security::Permit& permit, std::string_view json) -> Outcome {
    const auto a = parse<args::Write>(json);
    if (!a) {
        return failure("doc.write", a.error());
    }
    auto to = guarded_fs::target(permit, 0);
    if (!to) {
        return failure("doc.write", to.error());
    }
    if (GetFileAttributesW(security::paths::long_form(*to).c_str()) != INVALID_FILE_ATTRIBUTES) {
        return failure("doc.write", core::Error{core::Code::AlreadyExists, utf8(*to) + " exists: choose another name (it is never written over)"});
    }
    core::Result<std::string> bytes = core::make_error(core::Code::InvalidArgument, "nothing to write");
    if (a->format == "docx" || a->format == "md") {
        if (!a->markdown || a->markdown->empty()) {
            return failure("doc.write", core::Error{core::Code::InvalidArgument, "a docx or md file needs \"markdown\""});
        }
        bytes = a->format == "md" ? core::Result<std::string>{*a->markdown} : docs::docx_from_markdown(*a->markdown);
    } else {
        if (!a->table || a->table->columns.empty()) {
            return failure("doc.write", core::Error{core::Code::InvalidArgument, "an xlsx or csv file needs \"table\" with its columns"});
        }
        docs::Table t{.columns = {}, .rows = a->table->rows};
        for (std::size_t c = 0; c < a->table->columns.size(); ++c) {
            std::vector<std::string> values;
            for (const auto& row : t.rows) {
                values.push_back(c < row.size() ? row[c] : std::string{});
            }
            t.columns.emplace_back(a->table->columns[c], docs::kind_of(values));
        }
        bytes = a->format == "xlsx" ? docs::xlsx_from_table(t) : core::Result<std::string>{docs::csv_from_table(t)};
    }
    if (!bytes) {
        return failure("doc.write", bytes.error());
    }
    if (auto written = os::current().write_file(*to, *bytes, os::WriteMode::CreateOnly); !written) {
        return failure("doc.write", written.error());
    }
    const auto done = std::format(R"({{"written":{},"bytes":{}}})", core::json::write(utf8(*to)).value_or("\"\""), bytes->size());
    Outcome o{0, wrap("doc.write", *to, done, false), {{.kind = EffectKind::Created, .from = {}, .to = *to, .undoable = true}}};
    return o;
}

auto read_schema() -> std::string {
    auto s = glz::write_json_schema<args::Read>();
    return s ? std::move(*s) : std::string{"{}"};
}

auto read_request(std::string_view json, const std::filesystem::path& wd) -> core::Result<security::Request> {
    auto a = parse<args::Read>(json);
    if (!a) {
        return std::unexpected{a.error()};
    }
    if ((a->offset && *a->offset < 0) || (a->max_chars && *a->max_chars < 1)) {
        return core::make_error(core::Code::InvalidArgument, "arguments: \"offset\" is 0 or more, \"max_chars\" 1 or more");
    }
    if (a->pages) {
        if (auto p = pages_of(*a->pages); !p) {
            return std::unexpected{p.error()};
        }
    }
    auto path = core::utf8::to_wide(a->path);
    if (!path || path->empty()) {
        return core::make_error(core::Code::InvalidArgument, "arguments: \"path\" is empty or not UTF-8");
    }
    return security::Request{.tool = "doc.read", .risk = security::Risk::Read, .paths = {std::move(*path)}, .working_dir = wd};
}

auto read_run(const security::Permit& permit, std::string_view json) -> Outcome {
    const auto a = parse<args::Read>(json);
    if (!a) {
        return failure("doc.read", a.error());
    }
    std::optional<std::set<int>> wanted;
    if (a->pages) {
        wanted = *pages_of(*a->pages);
    }
    auto file = guarded_fs::open_read(permit, permit.paths().empty() ? std::wstring{} : permit.paths()[0].path);
    if (!file) {
        return failure("doc.read", file.error());
    }
    if (file->directory) {
        return failure("doc.read", core::Error{core::Code::InvalidArgument, utf8(file->path) + " is a folder"});
    }
    auto ext = std::filesystem::path{file->path}.extension().string();
    std::ranges::transform(ext, ext.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    if (ext == ".doc" || ext == ".xls" || ext == ".ppt" || ext == ".odt" || ext == ".ods" || ext == ".odp" || ext == ".rtf") {
        return failure("doc.read", unsupported(ext));
    }
    auto loaded = [&]() -> core::Result<Read> {
        if (ext == ".pdf") {
            return read_pdf(file->path, wanted, a->language.value_or(""), a->part);
        }
        auto bytes = guarded_fs::read(*file, docs::kMaxDocument);
        if (!bytes) {
            return std::unexpected{bytes.error()};
        }
        if (bytes->size > bytes->data.size()) {
            return core::make_error(core::Code::InvalidArgument, "the document is larger than 100 MB");
        }
        const auto leaf = utf8(std::filesystem::path{file->path}.filename().wstring());
        if (ext == ".txt" || ext == ".md" || ext == ".csv") {
            auto text = fs::text_of(bytes->data);
            if (!text) {
                return core::make_error(core::Code::InvalidArgument, "not a text file");
            }
            if (ext != ".csv") {
                return Read{.format = ext.substr(1), .text = std::move(*text)};
            }
            auto rows = docs::csv_rows(*text, kMaxRows);
            Read out{.format = "csv", .text = table_text(rows)};
            out.tables.push_back({.name = leaf, .rows = std::move(rows)});
            return out;
        }
        if (ext != ".docx" && ext != ".xlsx" && ext != ".pptx") {
            return core::make_error(core::Code::InvalidArgument, "not a document doc.read reads (fs.read reads text files)");
        }
        auto zip = docs::ZipReader::open(std::as_bytes(std::span{bytes->data.data(), bytes->data.size()}));
        if (!zip) {
            return std::unexpected{zip.error()};
        }
        if (ext == ".docx") {
            auto md = docs::docx_markdown(*zip);
            if (!md) {
                return std::unexpected{md.error()};
            }
            return Read{.format = "docx", .text = std::move(*md)};
        }
        if (ext == ".pptx") {
            auto slides = docs::pptx_slides(*zip);
            if (!slides) {
                return std::unexpected{slides.error()};
            }
            Read out{.format = "pptx", .pages = static_cast<int>(slides->size())};
            for (std::size_t i = 0; i < slides->size(); ++i) {
                if (!wanted || wanted->contains(static_cast<int>(i) + 1)) {
                    out.text += std::format("{}## Slide {}\n\n{}\n", out.text.empty() ? "" : "\n", i + 1, (*slides)[i]);
                }
            }
            return out;
        }
        auto sheets = docs::xlsx_sheets(*zip, kMaxRows);
        if (!sheets) {
            return std::unexpected{sheets.error()};
        }
        Read out{.format = "xlsx"};
        for (std::size_t i = 0; i < sheets->size(); ++i) {
            if (wanted && !wanted->contains(static_cast<int>(i) + 1)) {
                continue;
            }
            auto& s = (*sheets)[i];
            if (s.skipped) { // past the limit: said, not read (phase 10)
                out.text += std::format("{}## {}\n\n(not read: {})\n", out.text.empty() ? "" : "\n", s.name, s.facts);
                out.skipped.push_back({.kind = "sheet_too_large", .what = std::format("sheet \xC2\xAB{}\xC2\xBB", s.name), .facts = std::move(s.facts)});
                continue;
            }
            out.text += std::format("{}## {}\n\n{}", out.text.empty() ? "" : "\n", s.name, table_text(s.rows));
            out.tables.push_back({.name = std::move(s.name), .rows = std::move(s.rows)});
        }
        return out;
    }();
    if (!loaded) {
        return failure("doc.read", loaded.error());
    }
    // This part: from `offset`, at most max_chars characters and kBytesOut bytes.
    const std::string& text = loaded->text;
    out::Read data{.format = loaded->format, .total_chars = chars_of(text), .pages = loaded->pages, .ocr = loaded->ocr};
    if (loaded->plan) { // a PDF by its parts (phase 10)
        const auto& plan = *loaded->plan;
        if (plan.part > 0 && plan.mapped) {
            data.part = plan.part;
            data.parts = plan.parts;
            data.next_part = plan.next_part;
        }
        if (!plan.note.empty()) {
            data.note = plan.note;
        }
        if (loaded->document) {
            std::vector<int> scanned;
            for (const auto& p : loaded->document->read) {
                if (p.scan) {
                    scanned.push_back(p.page);
                }
            }
            if (!scanned.empty()) {
                data.scanned_pages = std::move(scanned);
            }
        }
    }
    data.offset = std::min<std::size_t>(static_cast<std::size_t>(a->offset.value_or(0)), data.total_chars);
    const std::size_t from = byte_of(text, data.offset);
    const std::size_t want = a->max_chars ? static_cast<std::size_t>(*a->max_chars) : kDefaultChars;
    std::size_t to = byte_of(std::string_view{text}.substr(from), want) + from;
    if (to - from > kBytesOut) {
        to = from + kBytesOut;
        while (to > from && (static_cast<unsigned char>(text[to]) & 0xC0) == 0x80) {
            --to; // never half a character
        }
    }
    data.text = text.substr(from, to - from);
    if (to < text.size()) {
        data.next_offset = data.offset + chars_of(data.text);
    }
    if (!loaded->tables.empty() || loaded->format == "xlsx") {
        std::vector<out::SheetSize> sizes;
        for (const auto& t : loaded->tables) {
            sizes.push_back({.name = t.name, .rows = t.rows.size()});
        }
        data.sheets = std::move(sizes);
    }
    if (!loaded->skipped.empty()) {
        data.skipped = loaded->skipped;
    }
    auto data_json = core::json::write(data);
    Outcome o{0, wrap("doc.read", file->path, data_json ? *data_json : std::string{"{}"}, data.next_offset.has_value()), {}};
    o.tables = std::move(loaded->tables);
    o.document = std::move(loaded->document);
    o.skipped = std::move(loaded->skipped);
    return o;
}

auto pdf_plan(int pages, std::size_t pieces, std::optional<int> part, const std::optional<std::set<int>>& wanted) -> PdfPlan {
    PdfPlan plan;
    plan.parts = std::max(1, (pages + kPartPages - 1) / kPartPages);
    if (pieces != static_cast<std::size_t>(pages)) {
        plan.mapped = false;
        plan.note = std::format("Windows' PDF filter gave {} pieces for {} pages: no page map, the text is read whole", pieces, pages);
        return plan;
    }
    if (wanted) {
        for (const int p : *wanted) {
            if (p >= 1 && p <= pages) {
                plan.pages.push_back(p);
            }
        }
        return plan;
    }
    plan.part = std::clamp(part.value_or(1), 1, plan.parts);
    for (int p = (plan.part - 1) * kPartPages + 1; p <= std::min(pages, plan.part * kPartPages); ++p) {
        plan.pages.push_back(p);
    }
    if (plan.part < plan.parts) {
        plan.next_part = plan.part + 1;
    }
    return plan;
}

} // namespace tools::documents
