#pragma once

// ui::work's table data (phase 8): what a table shows, apart from drawing it (ui/table.hpp) — the
// agent's data sets carry it (app::events::DataSet).

#include <cstdint>
#include <string>
#include <vector>

namespace ui::work {

enum class ColumnKind : std::uint8_t {
    Text,    // 13.5, the first in the text colour, cut with "…"
    Mono,    // a PID: mono 12.5, dim
    Number,  // right, tabular figures
    Size,    // its value, then a bar to the column's largest
    Date,    // right, 13, dim
    Percent, // its value, then a bar to 100
    Icon,    // the cell's text is a type for type_icon
    Action,  // the cell's text is a small button shown under the mouse; a row's badge stands there instead
    Spark,   // the row's seconds (DataRows::series) as a line, 108 x 22 (phase 13a)
};

struct Column {
    std::string title;
    ColumnKind kind = ColumnKind::Text;
    float width = 0;     // logical pixels…
    bool grow = false;   // …or a share of what the fixed ones leave
    bool sortable = false;
    std::string tag;     // what it was read as, after its title: "DATE", "¥" (phase 10)
};

struct Cell {
    std::string text;   // as shown
    double number = 0;  // what it sorts and measures by: bytes, a percent, a date's seconds
};

struct DataRows {
    std::size_t columns = 0;
    std::vector<Cell> cells;               // row by row
    std::vector<int> group;                // per row, or empty: a divider where it changes…
    std::vector<std::string> group_titles; // …titled by its group's entry ("IDLE — 0% CPU")
    std::vector<std::uint8_t> faded;       // per row, or empty: 1 draws it at .45
    std::string total;                     // a line under the rows ("9 files · 177.8 MB")
    bool dense = false;                    // rows 36 high
    // Phase 13a (design/ui_system): per row, or empty.
    std::vector<std::vector<float>> series; // a Spark column's points, oldest first
    int hot_row = -1;                       // the row holding the load: its spark yellow
    std::vector<std::string> notes;         // a dim line under the row's first text ("Rendering · blender.exe")
    std::vector<std::string> badges;        // a quiet badge in the action column instead of its button ("system")

    [[nodiscard]] auto rows() const noexcept -> std::size_t { return columns == 0 ? 0 : cells.size() / columns; }
    [[nodiscard]] auto at(std::size_t row, std::size_t column) const noexcept -> const Cell& {
        return cells[row * columns + column];
    }
};

} // namespace ui::work
