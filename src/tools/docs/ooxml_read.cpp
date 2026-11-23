#include "tools/docs/docs.hpp"
#include "tools/docs/numbers.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <format>
#include <map>
#include <optional>
#include <set>

namespace tools::docs {
namespace {

using Event = Xml::Event;

auto trimmed(std::string s) -> std::string {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    s.erase(0, first);
    s.erase(s.find_last_not_of(" \t\r\n") + 1);
    return s;
}

auto parse_error(std::string_view part, const Xml& x) -> core::Error {
    return core::Error{core::Code::Parse, std::format("{}: {}", part, x.error())};
}

// "Heading2" / "heading 2" -> 2; "Title" -> 1; else 0.
auto heading_level(std::string_view style) -> int {
    std::string s{style};
    std::ranges::transform(s, s.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    std::erase(s, ' ');
    if (s == "title") {
        return 1;
    }
    if (s.starts_with("heading") && s.size() == 8 && s[7] >= '1' && s[7] <= '6') {
        return s[7] - '0';
    }
    return 0;
}

auto table_markdown(const std::vector<std::vector<std::string>>& rows) -> std::string {
    std::size_t cols = 0;
    for (const auto& r : rows) {
        cols = std::max(cols, r.size());
    }
    if (cols == 0) {
        return {};
    }
    const auto line = [&](const std::vector<std::string>& r) {
        std::string out = "|";
        for (std::size_t c = 0; c < cols; ++c) {
            std::string cell = c < r.size() ? r[c] : std::string{};
            std::ranges::replace(cell, '\n', ' ');
            std::string escaped;
            for (const char ch : cell) {
                escaped += ch == '|' ? std::string{"\\|"} : std::string(1, ch);
            }
            out += " " + escaped + " |";
        }
        return out;
    };
    std::string out = line(rows[0]) + "\n|";
    for (std::size_t c = 0; c < cols; ++c) {
        out += "---|";
    }
    for (std::size_t i = 1; i < rows.size(); ++i) {
        out += "\n" + line(rows[i]);
    }
    return out;
}

// A relationships part: Id -> the target's name inside the archive, from `base` ("xl/").
auto relationships(ZipReader& zip, std::string_view part, std::string_view base) -> core::Result<std::map<std::string, std::string>> {
    auto text = zip.read(part);
    if (!text) {
        return std::unexpected{text.error()};
    }
    std::map<std::string, std::string> out;
    Xml x{*text};
    for (auto e = x.next(); e != Event::Done; e = x.next()) {
        if (e == Event::Error) {
            return std::unexpected{parse_error(part, x)};
        }
        if (e == Event::Start && x.name() == "Relationship") {
            const auto target = x.attribute("Target");
            out[x.attribute("Id")] = target.starts_with('/') ? target.substr(1) : std::string{base} + target;
        }
    }
    return out;
}

// Excel's own limits: columns A..XFD; the cells of a sheet read into rows of equal width.
constexpr std::size_t kMaxColumns = 16384;
constexpr std::size_t kMaxCells = 2'000'000;

// "BC12" -> 54 (0-based column of BC); past XFD: none.
auto column_of(std::string_view ref) -> std::optional<std::size_t> {
    std::size_t col = 0;
    for (const char c : ref) {
        if (c < 'A' || c > 'Z') {
            break;
        }
        col = col * 26 + static_cast<std::size_t>(c - 'A' + 1);
        if (col > kMaxColumns) {
            return std::nullopt;
        }
    }
    return col == 0 ? 0 : col - 1;
}

// "1 048 576": thousands apart.
auto grouped(std::size_t n) -> std::string {
    auto s = std::to_string(n);
    for (auto i = static_cast<std::ptrdiff_t>(s.size()) - 3; i > 0; i -= 3) {
        s.insert(static_cast<std::size_t>(i), 1, ' ');
    }
    return s;
}

// A sheet's size as it says it is (<dimension ref="A1:XFD1048576"> before its cells), in the first
// 4 KB of its part: "1 048 576 rows × 16 384 columns"; empty when it does not say.
auto stated_size(ZipReader& zip, std::string_view part) -> std::string {
    const auto head = zip.head(part, 4096);
    const auto at = head.find("<dimension");
    const auto ref = at == std::string::npos ? std::string::npos : head.find("ref=\"", at);
    if (ref == std::string::npos) {
        return {};
    }
    const auto end = head.find('"', ref + 5);
    const std::string_view r = std::string_view{head}.substr(ref + 5, end == std::string::npos ? 0 : end - ref - 5);
    const auto colon = r.find(':');
    const auto last = colon == std::string_view::npos ? r : r.substr(colon + 1);
    const auto digits = last.find_first_of("0123456789");
    const auto col = column_of(last.substr(0, digits));
    std::size_t rows = 0;
    if (digits != std::string_view::npos) {
        std::from_chars(last.data() + digits, last.data() + last.size(), rows);
    }
    if (!col || rows == 0) {
        return {};
    }
    return std::format("{} rows \xC3\x97 {} columns", grouped(rows), grouped(*col + 1));
}

// A part a document may lack: missing is nothing, any other failure (too large, damaged) is the
// document's error — never cells read as empty.
auto optional_part(ZipReader& zip, std::string_view name) -> core::Result<std::string> {
    auto text = zip.read(name);
    if (!text && text.error().code == core::Code::NotFound) {
        return std::string{};
    }
    return text;
}

// A date number format: built-in 14-22, or a custom code with d, m or y outside quotes and brackets.
auto date_code(std::string_view code) -> bool {
    bool quoted = false, bracket = false;
    for (const char c : code) {
        if (c == '"') quoted = !quoted;
        else if (c == '[') bracket = true;
        else if (c == ']') bracket = false;
        else if (!quoted && !bracket && (c == 'd' || c == 'm' || c == 'y' || c == 'D' || c == 'M' || c == 'Y')) return true;
    }
    return false;
}

auto date_text(double serial) -> std::string {
    using namespace std::chrono;
    const auto day = static_cast<int>(std::floor(serial));
    const sys_days epoch = sys_days{year{1899} / December / 30};
    const year_month_day ymd{epoch + days{day}};
    std::string out = std::format("{:04}-{:02}-{:02}", static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month()),
                                  static_cast<unsigned>(ymd.day()));
    const auto minutes = static_cast<int>(std::lround((serial - day) * 24 * 60));
    if (minutes > 0) {
        out += std::format(" {:02}:{:02}", minutes / 60 % 24, minutes % 60);
    }
    return out;
}

} // namespace

auto docx_markdown(ZipReader& zip) -> core::Result<std::string> {
    auto text = zip.read("word/document.xml");
    if (!text) {
        return std::unexpected{text.error()};
    }
    std::vector<std::string> blocks;
    std::string paragraph, style;
    bool listed = false, in_text = false;
    int tables = 0; // depth: a table in a table is read into the outer one's cell
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string cell;
    Xml x{*text};
    for (auto e = x.next(); e != Event::Done; e = x.next()) {
        if (e == Event::Error) {
            return std::unexpected{parse_error("word/document.xml", x)};
        }
        const auto n = x.name();
        if (e == Event::Start) {
            if (n == "p") { paragraph.clear(); style.clear(); listed = false; }
            else if (n == "pStyle") style = x.attribute("val");
            else if (n == "numPr") listed = true;
            else if (n == "t") in_text = true;
            else if (n == "tab") paragraph += '\t';
            else if (n == "br") paragraph += ' ';
            else if (n == "tbl" && ++tables == 1) rows.clear();
            else if (n == "tr" && tables == 1) row.clear();
            else if (n == "tc" && tables == 1) cell.clear();
        } else if (e == Event::Text && in_text) {
            paragraph += x.text();
        } else if (e == Event::End) {
            if (n == "t") {
                in_text = false;
            } else if (n == "p") {
                auto words = trimmed(paragraph);
                if (tables > 0) {
                    cell += (cell.empty() || words.empty() ? "" : " ") + words;
                } else if (!words.empty()) {
                    const int level = heading_level(style);
                    blocks.push_back(level > 0 ? std::string(static_cast<std::size_t>(level), '#') + " " + words : listed ? "- " + words : words);
                }
            } else if (n == "tc" && tables == 1) {
                row.push_back(trimmed(cell));
            } else if (n == "tr" && tables == 1) {
                rows.push_back(std::move(row));
                row.clear();
            } else if (n == "tbl" && --tables == 0) {
                if (auto t = table_markdown(rows); !t.empty()) {
                    blocks.push_back(std::move(t));
                }
            }
        }
    }
    std::string out;
    for (const auto& b : blocks) {
        out += (out.empty() ? "" : "\n\n") + b;
    }
    return out;
}

auto xlsx_sheet_names(ZipReader& zip) -> core::Result<std::vector<std::string>> {
    auto workbook = zip.read("xl/workbook.xml");
    if (!workbook) {
        return std::unexpected{workbook.error()};
    }
    std::vector<std::string> names;
    Xml x{*workbook};
    for (auto e = x.next(); e != Event::Done; e = x.next()) {
        if (e == Event::Error) {
            return std::unexpected{parse_error("xl/workbook.xml", x)};
        }
        if (e == Event::Start && x.name() == "sheet") {
            names.push_back(x.attribute("name"));
        }
    }
    return names;
}

auto xlsx_sheets(ZipReader& zip, std::size_t max_rows) -> core::Result<std::vector<Sheet>> {
    auto rels = relationships(zip, "xl/_rels/workbook.xml.rels", "xl/");
    if (!rels) {
        return std::unexpected{rels.error()};
    }
    auto workbook = zip.read("xl/workbook.xml");
    if (!workbook) {
        return std::unexpected{workbook.error()};
    }
    std::vector<std::pair<std::string, std::string>> listed; // name, part (each part once)
    std::set<std::string> seen;
    {
        Xml x{*workbook};
        for (auto e = x.next(); e != Event::Done; e = x.next()) {
            if (e == Event::Error) {
                return std::unexpected{parse_error("xl/workbook.xml", x)};
            }
            if (e == Event::Start && x.name() == "sheet") {
                if (const auto it = rels->find(x.attribute("r:id")); it != rels->end() && seen.insert(it->second).second) {
                    listed.emplace_back(x.attribute("name"), it->second);
                }
            }
        }
    }
    std::vector<std::string> shared;
    auto sst = optional_part(zip, "xl/sharedStrings.xml");
    if (!sst) {
        return std::unexpected{sst.error()};
    }
    {
        Xml x{*sst};
        bool in_text = false;
        for (auto e = sst->empty() ? Event::Done : x.next(); e != Event::Done; e = x.next()) {
            if (e == Event::Error) return std::unexpected{parse_error("xl/sharedStrings.xml", x)};
            if (e == Event::Start && x.name() == "si") shared.emplace_back();
            else if (e == Event::Start && x.name() == "t") in_text = true;
            else if (e == Event::End && x.name() == "t") in_text = false;
            else if (e == Event::Text && in_text && !shared.empty()) shared.back() += x.text();
        }
    }
    std::vector<bool> dates;            // per cell style: a date format
    std::vector<std::string_view> money; // per cell style: a currency's symbol in its format (phase 10), else empty
    auto styles = optional_part(zip, "xl/styles.xml");
    if (!styles) {
        return std::unexpected{styles.error()};
    }
    {
        std::set<std::string> custom_dates;
        std::map<std::string, std::string_view> currencies; // numFmtId -> its symbol
        bool in_xfs = false;
        Xml x{*styles};
        for (auto e = styles->empty() ? Event::Done : x.next(); e != Event::Done; e = x.next()) {
            if (e == Event::Error) {
                return std::unexpected{parse_error("xl/styles.xml", x)};
            }
            if (e == Event::Start && x.name() == "numFmt") {
                const auto code = x.attribute("formatCode");
                if (const auto c = currency_in_format(code); !c.empty()) { // where it stands, a locale's tag no currency
                    currencies[x.attribute("numFmtId")] = c;
                }
                if (!currencies.contains(x.attribute("numFmtId")) && date_code(code)) {
                    custom_dates.insert(x.attribute("numFmtId"));
                }
            } else if (e == Event::Start && x.name() == "cellXfs") {
                in_xfs = true;
            } else if (e == Event::End && x.name() == "cellXfs") {
                in_xfs = false;
            } else if (e == Event::Start && in_xfs && x.name() == "xf") {
                const auto id = x.attribute("numFmtId");
                int n = -1;
                std::from_chars(id.data(), id.data() + id.size(), n);
                dates.push_back((n >= 14 && n <= 22) || custom_dates.contains(id));
                const auto c = currencies.find(id);
                money.push_back(c == currencies.end() ? std::string_view{} : c->second);
            }
        }
    }
    std::vector<Sheet> out;
    for (const auto& [name, part] : listed) {
        auto text = zip.read(part);
        if (!text && text.error().kind == "too_large") { // past the limit: this sheet is left out, the others read (phase 10)
            const auto said = stated_size(zip, part);
            out.push_back({.name = name, .rows = {}, .skipped = true, .facts = said.empty() ? text.error().facts : said});
            continue;
        }
        if (!text) {
            return std::unexpected{text.error()};
        }
        Sheet sheet{.name = name, .rows = {}};
        std::size_t width = 0;
        std::size_t row_index = 0, col = 0;
        std::string type, value, inline_text;
        int style = -1;
        bool in_value = false, in_inline = false, stop = false;
        Xml x{*text};
        for (auto e = x.next(); e != Event::Done && !stop; e = x.next()) {
            if (e == Event::Error) {
                return std::unexpected{parse_error(part, x)};
            }
            const auto n = x.name();
            if (e == Event::Start && n == "row") {
                const auto r = x.attribute("r");
                std::size_t number = 0;
                std::from_chars(r.data(), r.data() + r.size(), number);
                row_index = number > 0 ? number - 1 : sheet.rows.size();
                if (row_index >= max_rows) {
                    stop = true;
                    continue;
                }
                if (sheet.rows.size() <= row_index) {
                    sheet.rows.resize(row_index + 1);
                }
                col = 0;
            } else if (e == Event::Start && n == "c") {
                const auto ref = x.attribute("r");
                if (!ref.empty()) {
                    const auto at = column_of(ref);
                    if (!at) {
                        return core::make_error(core::Code::Parse, std::format("{}: a cell past column XFD ({})", part, ref));
                    }
                    col = *at;
                }
                type = x.attribute("t");
                const auto s = x.attribute("s");
                style = -1;
                std::from_chars(s.data(), s.data() + s.size(), style);
                value.clear();
                inline_text.clear();
            } else if (e == Event::Start && n == "v") {
                in_value = true;
            } else if (e == Event::End && n == "v") {
                in_value = false;
            } else if (e == Event::Start && n == "t") {
                in_inline = true;
            } else if (e == Event::End && n == "t") {
                in_inline = false;
            } else if (e == Event::Text) {
                if (in_value) value += x.text();
                else if (in_inline) inline_text += x.text();
            } else if (e == Event::End && n == "c" && row_index < sheet.rows.size()) {
                std::string shown;
                if (type == "s") {
                    std::size_t k = 0;
                    std::from_chars(value.data(), value.data() + value.size(), k);
                    shown = k < shared.size() ? shared[k] : std::string{};
                } else if (type == "inlineStr") {
                    shown = inline_text;
                } else if (type == "b") {
                    shown = value == "1" ? "TRUE" : "FALSE";
                } else if (type.empty() && style >= 0 && static_cast<std::size_t>(style) < dates.size() && dates[static_cast<std::size_t>(style)] &&
                           !value.empty()) {
                    double serial = 0;
                    std::from_chars(value.data(), value.data() + value.size(), serial);
                    shown = date_text(serial);
                } else if (type.empty() && style >= 0 && static_cast<std::size_t>(style) < money.size() &&
                           !money[static_cast<std::size_t>(style)].empty() && !value.empty()) {
                    double amount = 0;
                    std::from_chars(value.data(), value.data() + value.size(), amount);
                    shown = std::format("{} {:.2f}", money[static_cast<std::size_t>(style)], amount); // "¥ 3200.00"
                } else {
                    shown = value;
                }
                if (sheet.rows.size() * std::max(width, col + 1) > kMaxCells) { // read as rows of equal width: left out (phase 10)
                    const auto said = stated_size(zip, part);
                    sheet.facts = said.empty() ? std::format("{} rows \xC3\x97 {} columns", grouped(sheet.rows.size()), grouped(std::max(width, col + 1)))
                                               : said;
                    sheet.skipped = true;
                    sheet.rows.clear();
                    width = 0;
                    stop = true;
                    continue;
                }
                auto& cells = sheet.rows[row_index];
                if (cells.size() <= col) {
                    cells.resize(col + 1);
                }
                cells[col] = std::move(shown);
                width = std::max(width, col + 1);
                ++col;
            }
        }
        for (auto& r : sheet.rows) {
            r.resize(width);
        }
        if (sheet.skipped) {
            sheet.rows = {}; // its memory given back now, not with the book
        }
        out.push_back(std::move(sheet));
    }
    return out;
}

auto pptx_slides(ZipReader& zip) -> core::Result<std::vector<std::string>> {
    auto rels = relationships(zip, "ppt/_rels/presentation.xml.rels", "ppt/");
    if (!rels) {
        return std::unexpected{rels.error()};
    }
    auto presentation = zip.read("ppt/presentation.xml");
    if (!presentation) {
        return std::unexpected{presentation.error()};
    }
    std::vector<std::string> parts; // each slide once, however often it is listed
    std::set<std::string> seen;
    {
        Xml x{*presentation};
        for (auto e = x.next(); e != Event::Done; e = x.next()) {
            if (e == Event::Error) {
                return std::unexpected{parse_error("ppt/presentation.xml", x)};
            }
            if (e == Event::Start && x.name() == "sldId") {
                if (const auto it = rels->find(x.attribute("r:id")); it != rels->end() && seen.insert(it->second).second) {
                    parts.push_back(it->second);
                }
            }
        }
    }
    std::vector<std::string> out;
    for (const auto& part : parts) {
        auto text = zip.read(part);
        if (!text) {
            return std::unexpected{text.error()};
        }
        std::string slide, paragraph;
        bool in_text = false;
        Xml x{*text};
        for (auto e = x.next(); e != Event::Done; e = x.next()) {
            if (e == Event::Error) {
                return std::unexpected{parse_error(part, x)};
            }
            if (e == Event::Start && x.name() == "p") {
                paragraph.clear();
            } else if (e == Event::Start && x.name() == "t") {
                in_text = true;
            } else if (e == Event::End && x.name() == "t") {
                in_text = false;
            } else if (e == Event::Text && in_text) {
                paragraph += x.text();
            } else if (e == Event::End && x.name() == "p") {
                if (auto words = trimmed(paragraph); !words.empty()) {
                    slide += (slide.empty() ? "" : "\n") + words;
                }
            }
        }
        out.push_back(std::move(slide));
    }
    return out;
}

auto csv_rows(std::string_view text, std::size_t max_rows) -> std::vector<std::vector<std::string>> {
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.remove_prefix(3);
    }
    // The separator the first line uses most: ',' or ';' (Excel in many languages writes ';').
    std::size_t commas = 0, semicolons = 0;
    bool quoted = false;
    for (const char c : text) {
        if (c == '"') quoted = !quoted;
        else if (!quoted && c == '\n') break;
        else if (!quoted && c == ',') ++commas;
        else if (!quoted && c == ';') ++semicolons;
    }
    const char sep = semicolons > commas ? ';' : ',';
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string cell;
    quoted = false;
    for (std::size_t i = 0; i < text.size() && rows.size() < max_rows; ++i) {
        const char c = text[i];
        if (quoted) {
            if (c == '"' && i + 1 < text.size() && text[i + 1] == '"') {
                cell += '"';
                ++i;
            } else if (c == '"') {
                quoted = false;
            } else {
                cell += c;
            }
        } else if (c == '"') {
            quoted = true;
        } else if (c == sep) {
            row.push_back(std::move(cell));
            cell.clear();
        } else if (c == '\n' || c == '\r') {
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
                ++i;
            }
            row.push_back(std::move(cell));
            cell.clear();
            rows.push_back(std::move(row));
            row.clear();
        } else {
            cell += c;
        }
    }
    if ((!cell.empty() || !row.empty()) && rows.size() < max_rows) {
        row.push_back(std::move(cell));
        rows.push_back(std::move(row));
    }
    return rows;
}

} // namespace tools::docs
