#pragma once

// app::tray_menu — the tray's menu in the agent's look (design/ui_model screen 8), as data: its rows
// for the config and the settings menu (app::settings), what a click on each is, where it opens.
// Its window is the shell's. Logical pixels, 372 wide; a row's `id` is the settings item's (the
// shell applies it as before) or kOpenId / kQuitId / kDebugId; 0: the row folds a list open.

#include "app/config.hpp"
#include "app/scenes/accordion.hpp"
#include "app/settings.hpp"
#include "ui/layout.hpp"
#include "ui/ui.hpp"

#include <windows.h>

#include <array>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace app::tray_menu {

enum class Kind : std::uint8_t { Header, Section, Model, Thinking, Budget, BudgetValue, Item, Settings, SettingsItem, Separator, Theme };

struct Row {
    Kind kind = Kind::Item;
    std::string label;
    std::string detail;     // the header's badge, a model's line, a value, a key
    std::string state;      // the header's "ready" / "working"
    unsigned id = 0;
    ui::layout::Box box;
    bool checked = false;
    int model = 0;          // a model row's place in the catalog (its orb's still), the header's
    int level = 0;          // a thinking step's (its drops: 0 2 4 5)
    bool open = false;      // the budget's or the settings' list shown
    char32_t icon = 0;      // an item's icon (Segoe Fluent / MDL2); the budget draws its "$"
    // Unfolding: the row's box with every list in it open, the list it is
    // in (none, the settings', the budget's), how much of it shows, its chevron's turn (› 0, ˅ 1).
    enum class In : std::uint8_t { None, Settings, Budget };
    ui::layout::Box full;
    In in = In::None;
    float alpha = 1.0f;
    float turn = 0.0f;
};

// How much of a list shows now: its room (0 shut .. 1 open), its rows' opacity, its chevron's turn
// (› 0 .. ˅ 1).
struct Shown {
    float room = 0;
    float alpha = 0;
    float turn = 0;
};
// A list's unfolding, as MS 10 (app::scenes::Accordion): the room on smooth, the rows 80 ms later,
// the chevron on base; shut, the rows go first. Asked the other way midway, it goes back from where
// it is. Each call steps it to `now` — a frame's time at most (a stall is one step, MS § 5.1).
struct Unfold {
    void set(bool open, double now) noexcept;
    [[nodiscard]] auto shown(double now) noexcept -> Shown;
    [[nodiscard]] auto moving(double now) noexcept -> bool;
    [[nodiscard]] auto open() const noexcept -> bool { return open_; }

private:
    void step(double now) noexcept;
    scenes::Accordion scene_;
    double last_ = -1.0e9;
    bool open_ = false;
};

inline constexpr float kWidth = 372.0f;
inline constexpr unsigned kOpenId = 1;  // the shell's "Open agent"
inline constexpr unsigned kQuitId = 2;  // "Quit"
inline constexpr unsigned kDebugId = 3; // "Debug window (F12)", debug builds
inline constexpr unsigned kPauseId = 4; // "Pause agent" / "Resume agent" (stage 4d-2)

// `room`: the work area's height (logical pixels). A list open that would take the menu past it
// folds the Model and Thinking sections away (the header's badge still says the choice), and if
// that is not enough the budget's list closes for the settings'.
// `paused` (stage 4d-2): the agent paused — Resume agent, and the header says so.
auto layout(const config::Config& c, std::span<const settings::Item> items, bool working, bool settings_open, bool budget_open,
            bool debug, float room = std::numeric_limits<float>::infinity(), bool paused = false) -> std::vector<Row>;
[[nodiscard]] auto height(const std::vector<Row>& rows) -> float;
// Its height with its lists all the way open.
[[nodiscard]] auto full_height(const std::vector<Row>& rows) -> float;
// Its window's height: the tallest the menu gets, either list open or shut, so the window is never
// resized while it shows (resized, it showed its last frame moved).
[[nodiscard]] auto window_height(const config::Config& c, std::span<const settings::Item> items, bool working, bool debug,
                                 float room = std::numeric_limits<float>::infinity()) -> float;
// The rows as the lists show now (0..1 each): a list's rows grow and fade in, the rows under it
// move with it. In place, nothing allocated: every frame of an unfolding.
void unfold(std::vector<Row>& rows, Shown settings, Shown budget) noexcept;
// The same with one number for a list's room, rows and chevron.
void unfold(std::vector<Row>& rows, float settings, float budget) noexcept;
// The row under (x, y), its index; a section, the header's empty part or a separator is none.
[[nodiscard]] auto hit(const std::vector<Row>& rows, float x, float y) -> std::optional<std::size_t>;
// Its header's "ready" / "working" as the agent is now: every frame while it is open, in place.
void live(std::vector<Row>& rows, bool working) noexcept;
// The keys (the review's deferred item): the row ↑ (-1) or ↓ (+1) from `from` — line by line, on
// the thinking line the one chosen, around at the ends; none yet: the first or the last.
[[nodiscard]] auto step(const std::vector<Row>& rows, std::optional<std::size_t> from, int dir) -> std::optional<std::size_t>;
// ← (-1) or → (+1): the thinking step next to the one chosen; none past Off or Max.
[[nodiscard]] auto level_step(const std::vector<Row>& rows, int dir) -> std::optional<std::size_t>;
// A right click on the icon while the menu is open: its press takes the menu's focus (it goes),
// its release asks for the menu again. Shown again only if the click's press (`pressed_at`) is not
// within kReclick of the menu going by focus (`lost_at`; else -1e9), however long it was held.
inline constexpr double kReclick = 0.4;
[[nodiscard]] auto reopens(double lost_at, double pressed_at) noexcept -> bool;
// Where the menu (`menu`, physical pixels) opens: next to the tray icon on the taskbar's side, all
// of it in the work area of the icon's monitor.
[[nodiscard]] auto place(RECT icon, RECT work, SIZE menu) -> POINT;
// Unfolding: the window placed for the menu `full` tall (physical pixels), the glass `shown` tall
// in it — its top in the window, where place() would put a menu of its height (the edge it keeps
// fixed: the bottom over a bottom taskbar, the top under a top one, the icon's middle by a side one).
[[nodiscard]] auto glass_top(RECT icon, RECT work, LONG width, LONG full, LONG shown) -> LONG;

// Draws the menu, `width` x `height` logical pixels from (0, 0), between ui.begin_frame and
// ui.end_frame: its glass, its rows, the one under the mouse lit. `orbs`: Flash's and Pro's stills
// (ui::Orb's, drawn with its uv) — none, no orb.
void draw(ui::Ui& ui, const std::vector<Row>& rows, std::optional<std::size_t> hover, std::array<GLuint, 2> orbs, float width,
          float height, ui::text::Point origin = {}, GLuint live_orb = 0);

} // namespace app::tray_menu
