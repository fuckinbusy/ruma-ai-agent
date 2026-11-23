#pragma once

// ui::work::table — the data panel's table (phase 8; mockup-spec § 3, Table): a header 34 high
// that stays on top and sorts by a column once the data is complete (stable: equal values keep
// their order), rows 40 high (36 dense) of which only those in view are drawn, a size or a
// percent as its value and a #C3CBFF bar, a row's action under the mouse, group dividers, a total
// line under it all.

#include "ui/layout.hpp"
#include "ui/table_data.hpp"
#include "ui/ui.hpp"
#include "ui/work.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ui::work {

struct TableState {
    Scroll scroll;
    int sort_column = -1;
    bool descending = true;
    int hover = -1;                  // the row under the mouse (an index into DataRows), or -1
    std::vector<std::uint32_t> order; // the rows as shown, top to bottom
    std::size_t first = 0;           // the rows drawn last frame: order[first, end)
    std::size_t end = 0;
    // What the order and the layout were made for.
    std::size_t built_rows = 0;
    int built_column = -2;
    bool built_descending = true;
    std::vector<float> tops;   // each shown row's top in the content (groups move them)
    std::vector<double> most;  // per column, the largest number (a size's bar)
    // Only the rows with these words in a cell (any case); empty: all.
    std::string filter;
    std::string built_filter;
    // What the last frame drew from (Ctrl+A copies its rows: table_text).
    const DataRows* copy_rows = nullptr;
    std::span<const Column> copy_columns;
    // Motion (MOTION_SPEC scene 08). When each row came (seconds; rows of one arrival kStagger apart,
    // kStaggerMaxItems at most; grown only as rows come), each row's top before the last re-sort and
    // the spring that takes them to the new ones (FLIP: 0 the old places .. 1 the new), and whether
    // the table has waited for data (its arriving rows then come over skeletons).
    std::vector<double> came;
    std::vector<float> from_top;
    std::vector<std::uint8_t> was_shown; // by row: shown before the filter changed (its FLIP; made at the change)
    motion::Spring flip{.x = 1, .v = 0, .target = 1, .tok = motion::kSmooth};
    double flip_at = -1;
    bool waited = false;
    // A row that left (MOTION_SPEC § 7 "Forget"): its cells as they were, where it stood — it goes on
    // micro, then its room closes on smooth and the rows under it rise. The data loses it at once.
    std::vector<Cell> left_cells;
    float left_top = 0;         // its top in the content
    std::size_t left_place = 0; // its place in the order: the rows from here on were under it
    double left_at = -1.0e9;
    bool leaving = false;
    bool leave_pending = false; // left since the last frame: its time starts with the next
};

// A bar where something will come (MS § 7 "Skeleton"): a faint ground and, while `lit` (still
// waited for), a light running over it once in table::kSkeletonSweep; at `opacity`. It asks for
// frames while lit.
void skeleton_bar(Ui& ui, const layout::Box& bar, float opacity, bool lit);

// A table's rows as shown, its text columns (not an icon, a spark nor a button) separated by tabs,
// one row a line: what Ctrl+A over it copies. `state` is a TableState.
void table_text(const void* state, std::string& out);

struct TableEvent {
    int action_row = -1; // a row's action pressed: its index in DataRows
    bool sorted = false; // the header sorted it again
    int open_row = -1;   // `openable`: a row clicked (phase 10)
    int more_row = -1;   // `openable`: its "..." clicked
};

// Row `row` of `rows` is about to be taken out of the data (Forget): the table keeps it as it is
// drawn and lets it go (see TableState::left_cells). Call it before the data loses the row.
void table_leave(TableState& s, const DataRows& rows, std::size_t row);

// `openable` (phase 10, screen 26): a row is clicked to open it, its "..." at the right end under
// the mouse; a row under the mouse has a 2 px accent bar at its left, pressed an accent tint.
auto table(Ui& ui, Id id, const layout::Box& box, std::span<const Column> columns, const DataRows& rows, TableState& s,
           bool complete, bool openable = false) -> TableEvent;

} // namespace ui::work
