#include "tools/docs/docs.hpp"

#include "markdown/markdown.hpp"
#include "tools/docs/numbers.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <set>

namespace tools::docs {
namespace {

constexpr std::string_view kXmlHead = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)";
constexpr std::string_view kW = R"(xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main" )"
                                R"(xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships")";
constexpr std::string_view kRel = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";

auto escaped(std::string_view s) -> std::string {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default:
            if (static_cast<unsigned char>(c) >= 0x20 || c == '\t' || c == '\n') {
                out += c; // other control characters are not XML
            }
        }
    }
    return out;
}

auto run(std::string_view text, bool bold, bool italic, bool code, bool link) -> std::string {
    std::string props;
    if (bold) props += "<w:b/>";
    if (italic) props += "<w:i/>";
    if (code) props += R"(<w:rFonts w:ascii="Consolas" w:hAnsi="Consolas" w:cs="Consolas"/>)";
    if (link) props += R"(<w:color w:val="0563C1"/><w:u w:val="single"/>)";
    return std::format(R"(<w:r>{}<w:t xml:space="preserve">{}</w:t></w:r>)", props.empty() ? "" : "<w:rPr>" + props + "</w:rPr>", escaped(text));
}

struct Writer {
    std::string body;
    std::vector<std::string> links; // hyperlink targets: rId100, rId101…

    // A block's text in runs, by its inline styles (they may overlap: a bold link).
    auto runs(const markdown::Block& b) -> std::string {
        std::set<std::uint32_t> cuts{0, static_cast<std::uint32_t>(b.text.size())};
        for (const auto& in : b.inlines) {
            cuts.insert(std::min<std::uint32_t>(in.begin, static_cast<std::uint32_t>(b.text.size())));
            cuts.insert(std::min<std::uint32_t>(in.end, static_cast<std::uint32_t>(b.text.size())));
        }
        std::string out;
        for (auto it = cuts.begin(); std::next(it) != cuts.end(); ++it) {
            const auto from = *it, to = *std::next(it);
            bool bold = false, italic = false, code = false;
            std::string href;
            for (const auto& in : b.inlines) {
                if (in.begin <= from && to <= in.end) {
                    bold = bold || in.bold;
                    italic = italic || in.italic;
                    code = code || in.code;
                    if (in.link) href = in.href;
                }
            }
            const auto piece = std::string_view{b.text}.substr(from, to - from);
            if (href.empty() || !(href.starts_with("http://") || href.starts_with("https://") || href.starts_with("mailto:"))) {
                out += run(piece, bold, italic, code, !href.empty());
            } else {
                links.push_back(href);
                out += std::format(R"(<w:hyperlink r:id="rId{}">{}</w:hyperlink>)", 99 + links.size(), run(piece, bold, italic, code, true));
            }
        }
        return out;
    }

    void paragraph(std::string_view props, std::string_view runs_xml) {
        body += std::format("<w:p>{}{}</w:p>", props.empty() ? "" : "<w:pPr>" + std::string{props} + "</w:pPr>", runs_xml);
    }

    void table(const std::vector<std::vector<std::string>>& cells) {
        body += R"(<w:tbl><w:tblPr><w:tblW w:w="0" w:type="auto"/><w:tblBorders>)";
        for (const char* side : {"top", "left", "bottom", "right", "insideH", "insideV"}) {
            body += std::format(R"(<w:{} w:val="single" w:sz="4" w:space="0" w:color="BFBFBF"/>)", side);
        }
        body += "</w:tblBorders></w:tblPr>";
        for (std::size_t r = 0; r < cells.size(); ++r) {
            body += "<w:tr>";
            for (const auto& cell : cells[r]) {
                body += "<w:tc><w:p>" + run(cell, r == 0, false, false, false) + "</w:p></w:tc>";
            }
            body += "</w:tr>";
        }
        body += "</w:tbl>";
    }
};

constexpr std::string_view kStyles =
    R"(<w:styles xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">)"
    R"(<w:docDefaults><w:rPrDefault><w:rPr><w:rFonts w:ascii="Calibri" w:hAnsi="Calibri" w:cs="Calibri" w:eastAsia="Calibri"/>)"
    R"(<w:sz w:val="22"/><w:szCs w:val="22"/></w:rPr></w:rPrDefault><w:pPrDefault><w:pPr><w:spacing w:after="160" w:line="259" w:lineRule="auto"/>)"
    R"(</w:pPr></w:pPrDefault></w:docDefaults>)"
    R"(<w:style w:type="paragraph" w:default="1" w:styleId="Normal"><w:name w:val="Normal"/></w:style>)"
    R"(<w:style w:type="paragraph" w:styleId="Heading1"><w:name w:val="heading 1"/><w:basedOn w:val="Normal"/><w:next w:val="Normal"/>)"
    R"(<w:pPr><w:keepNext/><w:spacing w:before="240"/><w:outlineLvl w:val="0"/></w:pPr><w:rPr><w:b/><w:sz w:val="32"/></w:rPr></w:style>)"
    R"(<w:style w:type="paragraph" w:styleId="Heading2"><w:name w:val="heading 2"/><w:basedOn w:val="Normal"/><w:next w:val="Normal"/>)"
    R"(<w:pPr><w:keepNext/><w:spacing w:before="200"/><w:outlineLvl w:val="1"/></w:pPr><w:rPr><w:b/><w:sz w:val="28"/></w:rPr></w:style>)"
    R"(<w:style w:type="paragraph" w:styleId="Heading3"><w:name w:val="heading 3"/><w:basedOn w:val="Normal"/><w:next w:val="Normal"/>)"
    R"(<w:pPr><w:keepNext/><w:spacing w:before="160"/><w:outlineLvl w:val="2"/></w:pPr><w:rPr><w:b/><w:sz w:val="24"/></w:rPr></w:style>)"
    R"(<w:style w:type="paragraph" w:styleId="Quote"><w:name w:val="Quote"/><w:basedOn w:val="Normal"/>)"
    R"(<w:pPr><w:ind w:left="720"/></w:pPr><w:rPr><w:i/><w:color w:val="595959"/></w:rPr></w:style>)"
    R"(</w:styles>)";

auto numbering() -> std::string {
    std::string out = R"(<w:numbering xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">)";
    for (int id = 0; id < 2; ++id) {
        out += std::format(R"(<w:abstractNum w:abstractNumId="{}">)", id);
        for (int level = 0; level < 2; ++level) {
            const bool bullet = id == 0;
            out += std::format(R"(<w:lvl w:ilvl="{}"><w:start w:val="1"/><w:numFmt w:val="{}"/><w:lvlText w:val="{}"/><w:lvlJc w:val="left"/>)"
                               R"(<w:pPr><w:ind w:left="{}" w:hanging="360"/></w:pPr></w:lvl>)",
                               level, bullet ? "bullet" : "decimal", bullet ? (level == 0 ? "\xE2\x80\xA2" : "\xE2\x97\xA6") : std::format("%{}.", level + 1),
                               720 * (level + 1));
        }
        out += "</w:abstractNum>";
    }
    out += R"(<w:num w:numId="1"><w:abstractNumId w:val="0"/></w:num><w:num w:numId="2"><w:abstractNumId w:val="1"/></w:num></w:numbering>)";
    return out;
}

} // namespace

auto docx_from_markdown(std::string_view md) -> core::Result<std::string> {
    markdown::Document doc;
    doc.sync(md, true);
    Writer w;
    for (const auto& b : doc.blocks()) {
        switch (b.kind) {
        case markdown::Kind::Heading:
            w.paragraph(std::format(R"(<w:pStyle w:val="Heading{}"/>)", std::clamp(b.level, 1, 3)), w.runs(b));
            break;
        case markdown::Kind::ListItem:
            w.paragraph(std::format(R"(<w:numPr><w:ilvl w:val="{}"/><w:numId w:val="{}"/></w:numPr>)", std::min(b.depth, 1), b.ordered ? 2 : 1),
                        w.runs(b));
            break;
        case markdown::Kind::Code: {
            std::size_t from = 0;
            while (from <= b.text.size()) {
                const auto nl = b.text.find('\n', from);
                const auto line = std::string_view{b.text}.substr(from, nl == std::string::npos ? std::string::npos : nl - from);
                w.paragraph(R"(<w:spacing w:after="0"/>)", run(line, false, false, true, false));
                if (nl == std::string::npos) {
                    break;
                }
                from = nl + 1;
            }
            break;
        }
        case markdown::Kind::Quote: w.paragraph(R"(<w:pStyle w:val="Quote"/>)", w.runs(b)); break;
        case markdown::Kind::Rule:
            w.paragraph(R"(<w:pBdr><w:bottom w:val="single" w:sz="6" w:space="1" w:color="BFBFBF"/></w:pBdr>)", "");
            break;
        case markdown::Kind::Table: w.table(b.cells); break;
        case markdown::Kind::Paragraph: w.paragraph("", w.runs(b)); break;
        }
    }
    std::string rels = std::format(R"({}<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">)"
                                   R"(<Relationship Id="rId1" Type="{}/styles" Target="styles.xml"/>)"
                                   R"(<Relationship Id="rId2" Type="{}/numbering" Target="numbering.xml"/>)",
                                   kXmlHead, kRel, kRel);
    for (std::size_t i = 0; i < w.links.size(); ++i) {
        rels += std::format(R"(<Relationship Id="rId{}" Type="{}/hyperlink" Target="{}" TargetMode="External"/>)", 100 + i, kRel, escaped(w.links[i]));
    }
    rels += "</Relationships>";
    ZipWriter zip;
    zip.add("[Content_Types].xml",
            std::format(R"({}<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">)"
                        R"(<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>)"
                        R"(<Default Extension="xml" ContentType="application/xml"/>)"
                        R"(<Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>)"
                        R"(<Override PartName="/word/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml"/>)"
                        R"(<Override PartName="/word/numbering.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.numbering+xml"/>)"
                        R"(</Types>)",
                        kXmlHead));
    zip.add("_rels/.rels", std::format(R"({}<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">)"
                                       R"(<Relationship Id="rId1" Type="{}/officeDocument" Target="word/document.xml"/></Relationships>)",
                                       kXmlHead, kRel));
    zip.add("word/_rels/document.xml.rels", rels);
    zip.add("word/document.xml", std::format(R"({}<w:document {}><w:body>{}<w:sectPr><w:pgSz w:w="11906" w:h="16838"/>)"
                                             R"(<w:pgMar w:top="1134" w:right="1134" w:bottom="1134" w:left="1134" w:header="708" w:footer="708" w:gutter="0"/>)"
                                             R"(</w:sectPr></w:body></w:document>)",
                                             kXmlHead, kW, w.body));
    zip.add("word/styles.xml", std::string{kXmlHead} + std::string{kStyles});
    zip.add("word/numbering.xml", std::string{kXmlHead} + numbering());
    return zip.finish();
}

auto xlsx_from_table(const Table& table) -> core::Result<std::string> {
    // Styles: 0 plain, 1 bold (the header), 2 "0.0" (MB), 3 "$0.00", 4 "0%".
    const auto column_name = [](std::size_t c) {
        std::string name;
        for (std::size_t n = c + 1; n > 0; n = (n - 1) / 26) {
            name.insert(name.begin(), static_cast<char>('A' + (n - 1) % 26));
        }
        return name;
    };
    const auto text_cell = [](const std::string& ref, std::string_view text, int style) {
        return std::format(R"(<c r="{}" t="inlineStr"{}><is><t xml:space="preserve">{}</t></is></c>)", ref,
                           style > 0 ? std::format(R"( s="{}")", style) : std::string{}, escaped(text));
    };
    const auto shown = [](double v) { return std::format("{}", v); };
    std::string cols = "<cols>";
    for (std::size_t c = 0; c < table.columns.size(); ++c) {
        std::size_t width = table.columns[c].first.size() + 6;
        for (const auto& row : table.rows) {
            width = std::max(width, c < row.size() ? row[c].size() + 2 : std::size_t{0});
        }
        cols += std::format(R"(<col min="{}" max="{}" width="{}" customWidth="1"/>)", c + 1, c + 1, std::min<std::size_t>(width, 60));
    }
    cols += "</cols>";
    std::string rows = R"(<row r="1">)";
    for (std::size_t c = 0; c < table.columns.size(); ++c) {
        const auto& [title, kind] = table.columns[c];
        rows += text_cell(column_name(c) + "1", kind == ColumnKind::Size ? title + " (MB)" : title, 1);
    }
    rows += "</row>";
    for (std::size_t r = 0; r < table.rows.size(); ++r) {
        const auto line = std::to_string(r + 2);
        rows += std::format(R"(<row r="{}">)", line);
        for (std::size_t c = 0; c < table.columns.size(); ++c) {
            const std::string value = c < table.rows[r].size() ? table.rows[r][c] : std::string{};
            const auto ref = column_name(c) + line;
            std::optional<double> number;
            int style = 0;
            switch (table.columns[c].second) {
            case ColumnKind::Number: number = number_of(value); break;
            case ColumnKind::Size:
                if (const auto mb = megabytes_of(value)) {
                    number = std::round(*mb * 10.0) / 10.0;
                    style = 2;
                }
                break;
            case ColumnKind::Money: // a number, its symbol in its format (phase 10: ¥ € £ ₽ too)
                if (const auto m = money_of(value)) {
                    number = m->value;
                    const auto at = std::ranges::find(kCurrencies, m->symbol) - kCurrencies.begin();
                    style = at == 1 ? 3 : 6 + static_cast<int>(at > 1 ? at - 1 : at);
                }
                break;
            case ColumnKind::Date: // a date, as Excel counts it (phase 10)
                if (const auto d = date_of(value)) {
                    number = *d;
                    style = 5;
                }
                break;
            case ColumnKind::Percent:
                if (!value.empty() && value.back() == '%') {
                    if (const auto p = number_of(std::string_view{value}.substr(0, value.size() - 1))) {
                        number = *p / 100.0;
                        style = 4;
                    }
                }
                break;
            case ColumnKind::Text: break;
            }
            if (value.empty()) {
                continue;
            }
            rows += number ? std::format(R"(<c r="{}"{}><v>{}</v></c>)", ref, style > 0 ? std::format(R"( s="{}")", style) : std::string{},
                                         shown(*number))
                           : text_cell(ref, value, 0); // text, never a formula
        }
        rows += "</row>";
    }
    ZipWriter zip;
    zip.add("[Content_Types].xml",
            std::format(R"({}<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">)"
                        R"(<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>)"
                        R"(<Default Extension="xml" ContentType="application/xml"/>)"
                        R"(<Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/>)"
                        R"(<Override PartName="/xl/worksheets/sheet1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>)"
                        R"(<Override PartName="/xl/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml"/>)"
                        R"(</Types>)",
                        kXmlHead));
    zip.add("_rels/.rels", std::format(R"({}<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">)"
                                       R"(<Relationship Id="rId1" Type="{}/officeDocument" Target="xl/workbook.xml"/></Relationships>)",
                                       kXmlHead, kRel));
    zip.add("xl/workbook.xml",
            std::format(R"({}<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" xmlns:r="{}">)"
                        R"(<sheets><sheet name="Sheet1" sheetId="1" r:id="rId1"/></sheets></workbook>)",
                        kXmlHead, kRel));
    zip.add("xl/_rels/workbook.xml.rels",
            std::format(R"({}<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">)"
                        R"(<Relationship Id="rId1" Type="{}/worksheet" Target="worksheets/sheet1.xml"/>)"
                        R"(<Relationship Id="rId2" Type="{}/styles" Target="styles.xml"/></Relationships>)",
                        kXmlHead, kRel, kRel));
    zip.add("xl/worksheets/sheet1.xml",
            std::format(R"({}<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">{}<sheetData>{}</sheetData></worksheet>)",
                        kXmlHead, cols, rows));
    zip.add("xl/styles.xml",
            std::format(R"({}<styleSheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">)"
                        R"(<numFmts count="8"><numFmt numFmtId="164" formatCode="0.0"/><numFmt numFmtId="165" formatCode="$0.00"/><numFmt numFmtId="166" formatCode="0%"/>)"
                        R"(<numFmt numFmtId="167" formatCode="yyyy-mm-dd"/>)"
                        R"(<numFmt numFmtId="168" formatCode="&quot;)" "\xC2\xA5" R"(&quot; #,##0.00"/>)"
                        R"(<numFmt numFmtId="169" formatCode="#,##0.00 &quot;)" "\xE2\x82\xAC" R"(&quot;"/>)"
                        R"(<numFmt numFmtId="170" formatCode="&quot;)" "\xC2\xA3" R"(&quot;#,##0.00"/>)"
                        R"(<numFmt numFmtId="171" formatCode="#,##0.00 &quot;)" "\xE2\x82\xBD" R"(&quot;"/></numFmts>)"
                        R"(<fonts count="2"><font><sz val="11"/><name val="Calibri"/></font><font><b/><sz val="11"/><name val="Calibri"/></font></fonts>)"
                        R"(<fills count="2"><fill><patternFill patternType="none"/></fill><fill><patternFill patternType="gray125"/></fill></fills>)"
                        R"(<borders count="1"><border><left/><right/><top/><bottom/><diagonal/></border></borders>)"
                        R"(<cellStyleXfs count="1"><xf numFmtId="0" fontId="0" fillId="0" borderId="0"/></cellStyleXfs>)"
                        R"(<cellXfs count="10"><xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0"/>)"
                        R"(<xf numFmtId="0" fontId="1" fillId="0" borderId="0" xfId="0" applyFont="1"/>)"
                        R"(<xf numFmtId="164" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/>)"
                        R"(<xf numFmtId="165" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/>)"
                        R"(<xf numFmtId="166" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/>)"
                        R"(<xf numFmtId="167" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/>)"
                        R"(<xf numFmtId="168" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/>)"
                        R"(<xf numFmtId="169" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/>)"
                        R"(<xf numFmtId="170" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/>)"
                        R"(<xf numFmtId="171" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/></cellXfs>)"
                        R"(<cellStyles count="1"><cellStyle name="Normal" xfId="0" builtinId="0"/></cellStyles></styleSheet>)",
                        kXmlHead));
    return zip.finish();
}

} // namespace tools::docs
