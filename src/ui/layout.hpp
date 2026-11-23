#pragma once

// ui::layout — flex-like layout for the immediate-mode tree. Each frame: begin(), add() nodes
// parent first, compute(). Two passes: measure (sizes, bottom-up on demand) and place
// (boxes, top-down). Measurements are memoised by (subtree hash, available size) across
// frames, so a frame that describes the same tree calls no measure functions and allocates
// nothing.
//
// One line per container unless a row wraps (Style::wrap), no margins (wrap in a padded node).

#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

namespace ui::layout {

inline constexpr float kAuto = -1.0f;
inline constexpr float kUnbounded = std::numeric_limits<float>::infinity();

enum class Axis : std::uint8_t { Row, Column };
enum class Align : std::uint8_t { Auto, Start, Center, End, Stretch }; // Auto: the parent's
enum class Justify : std::uint8_t { Start, Center, End, SpaceBetween };

struct Insets {
    float left = 0, top = 0, right = 0, bottom = 0;
};
struct Size {
    float w = 0, h = 0;
};
struct Box {
    float x = 0, y = 0, w = 0, h = 0;
};

// Content size of a leaf (text, image) for a given maximum width (kUnbounded: its natural
// width). `key` identifies the content: the same key must measure the same. A leaf with key 0
// is measured every time it is asked (correct but not cached).
struct Measure {
    auto (*fn)(const void* context, float max_width) -> Size = nullptr;
    const void* context = nullptr;
    std::uint64_t key = 0;
};

struct Style {
    Axis axis = Axis::Column;
    Insets padding{};
    float gap = 0;
    float width = kAuto;
    float height = kAuto;
    float min_width = 0;
    float max_width = kUnbounded;
    float min_height = 0;
    float max_height = kUnbounded;
    float grow = 0;
    float shrink = 1;
    Align align = Align::Stretch;   // children's cross-axis alignment
    Align align_self = Align::Auto; // overrides the parent's `align` for this node
    Justify justify = Justify::Start;
    // A row only: children that do not fit go on to new lines, `gap` apart; each line is laid out
    // on its own (its children at their own widths, justified), the row as tall as its lines.
    bool wrap = false;
};

using NodeId = std::uint32_t;
inline constexpr NodeId kNone = 0xFFFFFFFFu;

class Layout {
public:
    // Starts a new tree; keeps memory and the measurement cache.
    void begin();
    auto add(NodeId parent, const Style& style, const Measure& measure = {}) -> NodeId;
    // Lays out the tree under `root`, whose box is at (0, 0) and takes the available size on
    // each axis where it has no fixed size (its content size where available is unbounded).
    void compute(NodeId root, Size available);

    [[nodiscard]] auto box(NodeId node) const -> const Box& { return nodes_[node].box; }
    // Calls of leaf measure functions so far (cache effectiveness, tests, profiler).
    [[nodiscard]] auto measure_calls() const noexcept -> std::uint64_t { return measure_calls_; }

    static constexpr std::uint64_t kKeepFrames = 60; // cache entries unused longer are dropped

private:
    struct Node {
        Style style;
        Measure measure;
        NodeId first_child = kNone;
        NodeId last_child = kNone;
        NodeId next_sibling = kNone;
        std::uint64_t hash = 0;
        Box box;
    };
    struct Item { // per-child scratch of one container pass
        NodeId node;
        float base;
        float main;
        float cross;
        bool frozen;
    };
    struct Cached {
        std::uint64_t hash;
        float avail_w;
        float avail_h;
        Size size;
        std::uint64_t frame;
    };

    auto measure(NodeId id, float avail_w, float avail_h) -> Size;
    // Lays out a container's children in its inner box; `place` writes their boxes.
    auto flex(NodeId id, float inner_w, float inner_h, bool place, float x0, float y0) -> Size;
    // A wrapping row's lines (Style::wrap).
    auto wrap_rows(NodeId id, float inner_w, bool place, float x0, float y0) -> Size;
    void arrange(NodeId id, const Box& box);

    std::vector<Node> nodes_;
    std::vector<Item> items_; // a stack: each flex() call uses the top slice
    std::unordered_map<std::uint64_t, Cached> cache_;
    std::uint64_t frame_ = 0;
    std::uint64_t measure_calls_ = 0;
};

} // namespace ui::layout
