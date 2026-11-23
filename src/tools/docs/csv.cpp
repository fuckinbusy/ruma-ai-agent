#include "tools/docs/docs.hpp"

#include "tools/docs/numbers.hpp"

namespace tools::docs {

auto csv_cell(std::string_view cell) -> std::string {
    std::string text{cell};
    // A spreadsheet would run it as a formula (OWASP's six: = + - @, a tab, a carriage return).
    if (!text.empty() && std::string_view{"=+-@\t\r"}.find(text[0]) != std::string_view::npos && !number_of(text)) {
        text.insert(text.begin(), '\'');
    }
    if (text.find_first_of(",\"\r\n") == std::string::npos) {
        return text;
    }
    std::string out = "\"";
    for (const char c : text) {
        out += c;
        if (c == '"') {
            out += '"';
        }
    }
    return out + "\"";
}

auto csv_from_table(const Table& table) -> std::string {
    std::string out = "\xEF\xBB\xBF"; // UTF-8's BOM: Excel reads the file as UTF-8 then
    const auto line = [&](auto&& cell_of, std::size_t n) {
        for (std::size_t c = 0; c < n; ++c) {
            out += (c > 0 ? "," : "") + csv_cell(cell_of(c));
        }
        out += "\r\n";
    };
    const std::size_t n = table.columns.size();
    line([&](std::size_t c) -> std::string_view { return table.columns[c].first; }, n);
    for (const auto& row : table.rows) {
        line([&](std::size_t c) -> std::string_view { return c < row.size() ? std::string_view{row[c]} : std::string_view{}; }, n);
    }
    return out;
}

auto kind_of(std::span<const std::string> values) -> ColumnKind {
    bool any = false, sizes = true, money = true, percents = true, numbers = true, dates = true;
    std::string_view currency; // one currency a column: yen, dollars and euros added up would mean nothing
    for (const auto& v : values) {
        if (v.empty()) {
            continue;
        }
        any = true;
        sizes = sizes && megabytes_of(v).has_value();
        if (const auto m = money ? money_of(v) : std::nullopt) {
            money = currency.empty() || currency == m->symbol;
            currency = m->symbol;
        } else {
            money = false;
        }
        dates = dates && date_of(v).has_value();
        percents = percents && v.back() == '%' && number_of(std::string_view{v}.substr(0, v.size() - 1)).has_value();
        // "007" is a code, not seven: as a number its zeros would be lost (phase 9's deferred item).
        const bool code = v.size() > 1 && v[0] == '0' && v.find_first_not_of("0123456789") == std::string::npos;
        numbers = numbers && !code && number_of(v).has_value();
    }
    if (!any) return ColumnKind::Text;
    if (numbers) return ColumnKind::Number;
    if (sizes) return ColumnKind::Size;
    if (money) return ColumnKind::Money;
    if (percents) return ColumnKind::Percent;
    if (dates) return ColumnKind::Date;
    return ColumnKind::Text;
}

} // namespace tools::docs
