#pragma once

// ui::work — the pieces of the agent's work screens (design/interaction-topbar): the kind icon of
// an action with its state badge, chips, segmented switch, checkbox, radio option, drop-down,
// buttons, progress bar, sparkline, donut, stacked bar, file row, tag, key-value row, card,
// switch. Immediate-mode free functions like ui::widgets, metrics and colours from
// ui::theme::work. Each draws into the box the caller laid out and returns what the user did.

#include "ui/anim.hpp"
#include "ui/layout.hpp"
#include "ui/motion.hpp"
#include "ui/render/renderer.hpp"
#include "ui/text_map.hpp"
#include "ui/theme.hpp"
#include "ui/ui.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace ui::work {

// What an action does, by colour (mockup [data-k]): read blue, change yellow, delete red,
// the system violet; ok green for what is done well.
enum class Kind : std::uint8_t { Read, Change, Delete, System, Ok };
// The state of a step on its icon (.badge): done, running (a turning ring), waiting.
enum class Badge : std::uint8_t { None, Done, Run, Wait };
// A chip's dot and a value's colour.
enum class Tone : std::uint8_t { Accent, Ok, Warn, Crit, Plain };
enum class ButtonStyle : std::uint8_t { Normal, Primary, Crit };

[[nodiscard]] auto kind_color(Kind k) noexcept -> render::Color;
// The soft ground under a kind's icon or tag: neutral for a read, the accent's for the system, the
// status's for a change and a delete (colour means risk, not category).
[[nodiscard]] auto kind_ground(Kind k) noexcept -> render::Color;
// A neutral mark on a file's sheet of paper (f_page, light in both themes' own way): ink that reads on it.
[[nodiscard]] auto page_ink() noexcept -> render::Color;
// A file type's mark (its letters in capitals: "DOCX", "EXE"): its ground, laid on the page, and
// the letters' ink. A text document and a spreadsheet keep their hue; every other type is neutral.
struct TagInk {
    render::Color ground;
    render::Color ink;
};
[[nodiscard]] auto tag_ink(std::string_view tag) noexcept -> TagInk;
// What is above the surface — a menu, a list, a tooltip: the theme's pop with its line and shadow.
void pop_surface(Ui& ui, const layout::Box& box, float radius);
[[nodiscard]] auto tone_color(Tone t) noexcept -> render::Color;

// Segoe Fluent Icons code points for the mockup's SVG icons.
inline constexpr char32_t kIconSearch = 0xE721;
inline constexpr char32_t kIconTrash = 0xE74D;
inline constexpr char32_t kIconMove = 0xE8DE;     // move to folder
inline constexpr char32_t kIconSystem = 0xE950;   // a component: processes, services
inline constexpr char32_t kIconShield = 0xEA18;
inline constexpr char32_t kIconChevronRight = 0xE76C;
inline constexpr char32_t kIconChevronDown = 0xE70D;
inline constexpr char32_t kIconWarning = 0xE7BA;
inline constexpr char32_t kIconUndo = 0xE7A7;
inline constexpr char32_t kIconClose = 0xE711;
inline constexpr char32_t kIconPause = 0xE769;
inline constexpr char32_t kIconPlay = 0xE768;
inline constexpr char32_t kIconEye = 0xE7B3;
inline constexpr char32_t kIconZip = 0xF012;
inline constexpr char32_t kIconTerminal = 0xE756;
inline constexpr char32_t kIconHand = 0xE7A8;     // take over
inline constexpr char32_t kIconExpand = 0xE740;
inline constexpr char32_t kIconDisk = 0xEDA2;

// .kic: a tinted square with the icon in the kind's colour, the badge at its corner.
// `id` (not 0): the badge moves between its states as MOTION_SPEC scene 06 has a step — it wakes
// from its dot on quick, its spinner goes as the done circle comes on base, the check draws itself
// (tracks id, id + 1, id + 2). Without one it is drawn as it is.
void kind_icon(Ui& ui, const layout::Box& box, Kind kind, char32_t icon, Badge badge = Badge::None, Id id = 0);

// .chip: a capsule with a glowing dot (none for Plain) and its label.
void chip(Ui& ui, const layout::Box& box, std::string_view label, Tone tone);
[[nodiscard]] auto chip_size(Ui& ui, std::string_view label) -> layout::Size;

// .btn / .btn.sm; returns a click.
auto button(Ui& ui, Id id, const layout::Box& box, std::string_view label, ButtonStyle style = ButtonStyle::Normal,
            bool small = false, bool enabled = true) -> bool;
[[nodiscard]] auto button_size(Ui& ui, std::string_view label, bool small = false) -> layout::Size;

// .seg: one of several labels; returns the one clicked, or `selected`.
auto segmented(Ui& ui, Id id, const layout::Box& box, std::span<const std::string_view> labels, int selected) -> int;
// One of several chosen (a segment, a switch of two): the ground under the chosen one — where it
// is now on its way to `on`, sliding on kBase (its place kept from `origin_x`, so a control that
// moves takes its ground with it) — and how much a thing is the chosen one, 0..1 on kQuick, for
// its words' colour.
[[nodiscard]] auto chosen_ground(Ui& ui, Id id, float origin_x, const layout::Box& on) -> layout::Box;
[[nodiscard]] auto chosen(Ui& ui, Id id, bool on) -> float;
[[nodiscard]] auto segmented_size(Ui& ui, std::span<const std::string_view> labels) -> layout::Size;

// .cb: returns a click (the caller flips its value).
auto checkbox(Ui& ui, Id id, const layout::Box& box, std::string_view label, bool checked) -> bool;
// .opt: a radio with a title and a smaller line under it; returns a click.
auto option(Ui& ui, Id id, const layout::Box& box, std::string_view title, std::string_view sub, bool on) -> bool;
// An option's height for its two lines at the current text size, with its padding above and below.
[[nodiscard]] auto option_height(Ui& ui) -> float;
// .dd: the current item and a chevron; a click opens the list under it (the caller keeps `open`),
// a click on an item returns its index and closes it. `enabled` false: shown, not changed.
auto dropdown(Ui& ui, Id id, const layout::Box& box, std::span<const std::string_view> items, int selected,
              bool& open, bool enabled = true) -> std::optional<int>;
[[nodiscard]] auto dropdown_size(Ui& ui, std::span<const std::string_view> items) -> layout::Size;

// .sw: returns a click.
auto toggle(Ui& ui, Id id, const layout::Box& box, bool on) -> bool;

// .prog: a 4 px track filled to `fraction` (0..1) in `color`.
void progress_bar(Ui& ui, const layout::Box& box, float fraction, render::Color color);
// .spark: values 0..100 left to right as a line with a faint area under it; a dashed line at
// `warn_line` when given. MS § 7 "Inline chart": `slide` 0..1 is how far the newest point has
// come in from beyond the right edge (1: at the edge, at rest) — the data move without a jump when
// a point is added. `rolling`: the oldest point was dropped for it (a whole minute), so the line
// goes off the left as it comes; otherwise the line gathers itself to make room.
void sparkline(Ui& ui, const layout::Box& box, std::span<const float> values, render::Color color,
               std::optional<float> warn_line = std::nullopt, float slide = 1.0f, bool rolling = false);
// A chevron drawn as a mark that turns (MS 10): › at 0 degrees, ˅ at 90, `size` as an icon's.
void chevron(Ui& ui, render::Point centre, float size, float degrees, render::Color color);
// .donut: a ring filled clockwise from the top to `fraction`.
void donut(Ui& ui, const layout::Box& box, float fraction);

struct Segment {
    float share = 0; // of the whole bar, 0..1
    render::Color color{};
};
// .stack: segments left to right with 2 px gaps, on a faint track.
// `id` (not 0): its segments grow in on smooth, each kSegmentStagger after the one before, and a
// changed share goes there on smooth (MOTION_SPEC § 7 "Disk bar"). Without one: as given.
void stacked_bar(Ui& ui, const layout::Box& box, std::span<const Segment> segments, Id id = 0);

// .frow: a file badge with its extension (in capitals; its colour as a type icon's mark), the name
// (cut with an ellipsis) and the meta on the right.
void file_row(Ui& ui, const layout::Box& box, std::string_view ext, std::string_view name, std::string_view meta);
// .tag: a small capsule in the kind's colour.
void tag(Ui& ui, const layout::Box& box, std::string_view label, Kind kind);
[[nodiscard]] auto tag_size(Ui& ui, std::string_view label) -> layout::Size;
// .kv: a key on the left, its value on the right, a line above.
void kv_row(Ui& ui, const layout::Box& box, std::string_view key, std::string_view value, Tone tone = Tone::Plain);
// .card: a faint rounded panel with a 1 px line.
void card(Ui& ui, const layout::Box& box); // its corners: the theme's card radius

// .sheet: the top bar's panel hanging from the screen edge, bottom corners rounded, with its
// stroke, its top highlight and (`shadow`) its shadow outside it. The window's region must give
// the shadow its room (sheet_shadow_reach): with the panel alone it shows only in the corners, as
// the window's sharp rectangle (2026-10-15, and again 2026-11-19).
void sheet(Ui& ui, const layout::Box& box, float radius, bool shadow = true);
// How far the sheet's shadow goes past it (the theme's layers): to each side, and under it. The
// window's region gives it this room, so the shadow fades out by itself and is never cut.
struct ShadowReach {
    float side = 0;
    float bottom = 0;
};
[[nodiscard]] auto sheet_shadow_reach() -> ShadowReach;
// The sheet's shadow alone (a shape that is not the sheet yet: the fold), at the renderer's opacity.
void sheet_shadow(Ui& ui, const layout::Box& box, float radius);
// A panel's surface colour at the user's transparency (Ui::surface_alpha): the sheet's, or `c`'s
// own colour (the handle's, a card's).
[[nodiscard]] auto surface(const Ui& ui, render::Color c = ui::th().surface) -> render::Color;
// .send44 / .stop44: the bar's round buttons; return a click. The stop ring turns.
// `enabled` false: nothing to send - dim, no hover, no click (phase 10, screen 26).
auto send_button(Ui& ui, Id id, const layout::Box& box, bool enabled = true) -> bool;
// `ring`: its turning ring's opacity (MOTION_SPEC scene 05: it comes after the square).
auto stop_button(Ui& ui, Id id, const layout::Box& box, float ring = 1.0f) -> bool;
// .ib: a square icon button (pause, hide, expand, close); `filled` is .ib.f.
auto icon_button(Ui& ui, Id id, const layout::Box& box, char32_t icon, bool filled = false, bool enabled = true) -> bool;
// Phase 10 (screens 26-27): `box` pressed to `k` of its size about its middle.
[[nodiscard]] auto squeezed(const layout::Box& box, float k) -> layout::Box;
// The keyboard's ring around `box`: a 2 px dark gap, then 2 px #AEB9FF (screen 26).
// MS 14: with its widget's id it comes on base from 1.06 of its size and goes on micro; `on`: focused by the keyboard.
void focus_ring(Ui& ui, Id id, const layout::Box& box, float radius, bool on);
// .rprog: the 2 px line on the bar's bottom edge, filled to `fraction` with a glowing end; a
// fraction below 0 runs a highlight along it (work that cannot be measured).
void bar_progress(Ui& ui, const layout::Box& box, float fraction);
// .bubble: the user's words, a light box with a sharper bottom-right corner.
void bubble(Ui& ui, const layout::Box& box);

// A view that its content scrolls in (phase 8; mockup-spec § 1, Scroll): the wheel moves it 40 px
// a line (the system's lines per notch are in Input::wheel), scroll_to glides on a spring
// {360, 34}; a 5 px bar 4 px from the right while the content is longer than the view; the
// bottom 80 px fade into the surface while more is below.
struct Scroll {
    float offset = 0;  // pixels scrolled down from the content's top
    float content = 0; // its height, as the last frame drew it
    motion::Spring glide{.tok = motion::kSmooth, .range = 100.0f}; // MOTION_SPEC § 3: scrolling on smooth
    bool gliding = false;
    double at = -1.0; // the last frame
    float grab = -1;  // the thumb held: where in it the mouse took it (below 0: not held)
    float lit = 0;    // under the mouse: 0..1, the thumb wider
    bool moved_by_user = false; // its bar pressed or dragged, or a selection scrolled it, this frame
    float foot = 0;    // its bar ends this far above the view's bottom: a view down in a rounded corner (corner_foot)
};
// The room a scroll bar leaves at the bottom of a view that ends in a corner of `radius`: where the
// corner's curve is at the bar's outer edge, and 2 px.
[[nodiscard]] auto corner_foot(float radius) -> float;
// Clips to `viewport` and returns where the content goes (its top moved up by the offset); call
// end_scroll after drawing it.
auto scroll_view(Ui& ui, Scroll& s, const layout::Box& viewport, float content_height) -> layout::Box;
// `fade`: the bottom fade's corners - a viewport whose bottom is the panel's keeps its rounding.
void end_scroll(Ui& ui, const Scroll& s, const layout::Box& viewport,
                render::Color surface = ui::th().surface, render::Corners fade = {});
// Glides to `y` (clamped to the content by the next scroll_view); the wheel stops the glide.
void scroll_to(Scroll& s, float y);

// The data panel's tabs (phase 8; mockup-spec § 3): a row 46 high, each tab its icon, label and
// count; the active one in the text colour, underlined in the accent; a loading one turns a
// spinner for its icon and shows "N…". The tab at `pinned_right` (Activity; -1: none) sits at the
// row's right edge. A click makes a tab `active`. Returns the room under the row.
struct Tab {
    std::string_view label;
    char32_t icon = 0;
    int count = -1;       // its badge; below 0: none
    bool loading = false;
};
// `boxes` (as many as the tabs, or empty): where each was drawn. `reserve_right`: the row's end
// left free (a button of the caller's), the pinned tab before it.
// `more` (responsive text): the tabs with no room go under an "N more" button at the row's end, a
// list of them; one chosen there becomes `active`. Without it they are not drawn.
struct TabsMore {
    bool open = false;               // its list
    bool defer = false;              // the list drawn later, over what is under the row (tabs_more_list)
    layout::Box button{};            // where it was drawn (none: no tab went under it)
    std::span<layout::Box> items{};  // per tab (as many as the tabs): its row in the list, when listed
    // Filled by tabs(): the tabs under it, where its list goes.
    std::array<int, 64> hidden{};
    std::size_t hidden_count = 0;
    Id list_id = 0;
    render::Point at{};
};
auto tabs(Ui& ui, Id id, const layout::Box& box, std::span<const Tab> tabs, int& active, int pinned_right,
          std::span<layout::Box> boxes = {}, float reserve_right = 0, TabsMore* more = nullptr) -> layout::Box;
// The "N more" list of a `defer`red TabsMore, drawn once what lies under it is (final review I2): the
// tab chosen in it.
auto tabs_more_list(Ui& ui, std::span<const Tab> tabs, TabsMore& more) -> std::optional<int>;

// A row's type, centred in `box` (mockup-spec § 3): "folder"; "process:<name>", a square in the
// program's colour with its initial; anything else a file's extension, a page with its tag (PDF,
// DOCX, XLSX, EXE, MSI, ZIP in their colours, others grey).
void type_icon(Ui& ui, const layout::Box& box, std::string_view type);
// A program's colour, the same for the same name.
[[nodiscard]] auto app_color(std::string_view name) noexcept -> render::Color;

// A metric tile (mockup-spec § 3): the label and its state in a pill of its tone, the value
// large with its unit, a line under it, the last minute as a sparkline (0..100).
struct Metric {
    std::string_view label;
    std::string_view state;
    Tone tone = Tone::Ok;
    std::string_view value;
    std::string_view unit;
    std::string_view sub;
    std::span<const float> series;
    std::optional<float> line; // a dashed line at this percent
    float slide = 1.0f;        // the series' newest point on its way in (sparkline)
    bool rolling = false;
};
// With an id (MS § 7 "Metric tiles"): a new value is counted to on kSmooth, the state's colour
// turns on kQuick. First shown, it is as it is.
void metric_tile(Ui& ui, const layout::Box& box, const Metric& m, Id id = 0);
// The number a value begins with ("34%" → 34, "13.8" → 13.8); none when it begins with no number.
[[nodiscard]] auto number_of(std::string_view value) noexcept -> std::optional<float>;
// `value` with its number replaced by `shown`, written as the value writes it (its decimals, what
// follows it), into `out`; a value with no number, or no room, comes back as it is.
[[nodiscard]] auto counted(std::string_view value, float shown, std::span<char> out) noexcept -> std::string_view;

// A turning ring (loading), `diameter` across, its quarter in `color`; it asks for frames.
void spinner(Ui& ui, render::Point centre, float diameter, render::Color color);

// Text that wraps (responsive text, 2026-10-29): its lines at `width`, a line `size` x text scale x
// `line` apart and never less than `min_line_h`; a path or a command breaks after its separators
// (ui::text::wrap). Line breaks in the text are kept.
struct TextBlockStyle {
    Face face = Face::Text;
    float size = 13.0f;
    render::Color color{};
    float line = 1.4f;
    float min_line_h = 0;
};
[[nodiscard]] auto text_block_height(Ui& ui, std::string_view text, const TextBlockStyle& style, float width) -> float;
// Draws it from `box`'s top left at `box.w`; returns its height.
auto text_block(Ui& ui, const layout::Box& box, std::string_view text, const TextBlockStyle& style, const TextFrom& from = {}) -> float;
// A layout leaf for it: `spec` lives until the layout's compute().
struct TextBlockSpec {
    Ui* ui = nullptr;
    std::string_view text;
    TextBlockStyle style;
};
[[nodiscard]] auto text_measure(const TextBlockSpec& spec) -> layout::Measure;

// A menu at a point (the right button's: Copy, Select all), kept inside the window, in the
// drop-down's look; a disabled item is dim and takes no click. Returns the item clicked (it
// closes); a press outside it closes it.
// `boxes` (as many as the items, or empty): where each was drawn.
auto context_menu(Ui& ui, Id id, render::Point at, std::span<const std::string_view> items, std::span<const bool> enabled,
                  bool& open, std::span<layout::Box> boxes = {}) -> std::optional<int>;

// A word in a tight place (a tab, a cell, a tile's label, a chip): one line cut with "…" like a
// label, but then its whole text shows in a tooltip under the mouse and is what a selection copies.
auto caption(Ui& ui, const layout::Box& box, Face face, float size, std::string_view text, render::Color color) -> float;

// One line of text in `box`, vertically centred, cut with "…" to its width; returns its width.
auto label(Ui& ui, const layout::Box& box, Face face, float size, std::string_view text, render::Color color) -> float;
[[nodiscard]] auto text_width(Ui& ui, Face face, float size, std::string_view text) -> float;

} // namespace ui::work
