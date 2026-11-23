#include "ui/layout.hpp"

#include "core/hash.hpp"

#include <algorithm>
#include <bit>
#include <string_view>

namespace ui::layout {
namespace {

auto mix(std::uint64_t h, std::uint64_t value) noexcept -> std::uint64_t {
    return core::hash::fnv1a64(
        std::string_view{reinterpret_cast<const char*>(&value), sizeof(value)}, h);
}

auto bits(float value) noexcept -> std::uint64_t {
    return std::bit_cast<std::uint32_t>(value);
}

auto own_hash(const Style& s, const Measure& m) noexcept -> std::uint64_t {
    std::uint64_t h = core::hash::kFnvOffset;
    h = mix(h, static_cast<std::uint64_t>(s.axis) | static_cast<std::uint64_t>(s.align) << 8 |
                   static_cast<std::uint64_t>(s.align_self) << 16 |
                   static_cast<std::uint64_t>(s.justify) << 24 | static_cast<std::uint64_t>(s.wrap) << 32);
    for (const float f : {s.padding.left, s.padding.top, s.padding.right, s.padding.bottom, s.gap,
                          s.width, s.height, s.min_width, s.max_width, s.min_height, s.max_height,
                          s.grow, s.shrink}) {
        h = mix(h, bits(f));
    }
    h = mix(h, reinterpret_cast<std::uintptr_t>(m.fn));
    return mix(h, m.key);
}

auto bounded(float v) noexcept -> bool {
    return v < kUnbounded;
}

// min wins over max, as in CSS.
auto clamp(float v, float lo, float hi) noexcept -> float {
    return std::max(lo, std::min(v, hi));
}

auto main_min(const Style& s, bool row) noexcept -> float {
    return row ? s.min_width : s.min_height;
}
auto main_max(const Style& s, bool row) noexcept -> float {
    return row ? s.max_width : s.max_height;
}

} // namespace

void Layout::begin() {
    nodes_.clear();
    ++frame_;
    std::erase_if(cache_, [this](const auto& item) { return frame_ - item.second.frame > kKeepFrames; });
}

auto Layout::add(NodeId parent, const Style& style, const Measure& measure) -> NodeId {
    const auto id = static_cast<NodeId>(nodes_.size());
    nodes_.push_back({style, measure});
    if (parent != kNone) {
        Node& p = nodes_[parent];
        if (p.last_child == kNone) {
            p.first_child = id;
        } else {
            nodes_[p.last_child].next_sibling = id;
        }
        p.last_child = id;
    }
    return id;
}

void Layout::compute(NodeId root, Size available) {
    // Children are added after their parent: walking backwards hashes every subtree bottom-up.
    for (std::size_t i = nodes_.size(); i-- > 0;) {
        Node& n = nodes_[i];
        std::uint64_t h = own_hash(n.style, n.measure);
        for (NodeId c = n.first_child; c != kNone; c = nodes_[c].next_sibling) {
            h = mix(h, nodes_[c].hash);
        }
        n.hash = h;
    }
    const Size measured = measure(root, available.w, available.h);
    const Style& s = nodes_[root].style;
    const float w = s.width >= 0 ? measured.w
                    : bounded(available.w) ? clamp(available.w, s.min_width, s.max_width)
                                           : measured.w;
    const float h = s.height >= 0 ? measured.h
                    : bounded(available.h) ? clamp(available.h, s.min_height, s.max_height)
                                           : measured.h;
    arrange(root, {0, 0, w, h});
}

auto Layout::measure(NodeId id, float avail_w, float avail_h) -> Size {
    const Node& n = nodes_[id];
    const bool leaf = n.measure.fn != nullptr;
    if (leaf && n.measure.key == 0) { // no content key: nothing to cache by
        const Style& s = n.style;
        const float outer_w = s.width >= 0 ? s.width : std::min(avail_w, s.max_width);
        const float inner_w = std::max(outer_w - s.padding.left - s.padding.right, 0.0f);
        ++measure_calls_;
        const Size content = n.measure.fn(n.measure.context, inner_w);
        return {clamp(s.width >= 0 ? s.width : content.w + s.padding.left + s.padding.right,
                      s.min_width, s.max_width),
                clamp(s.height >= 0 ? s.height : content.h + s.padding.top + s.padding.bottom,
                      s.min_height, s.max_height)};
    }
    const float key_h = leaf ? 0.0f : avail_h; // a leaf's size depends on its width only
    const std::uint64_t key = mix(mix(n.hash, bits(avail_w)), bits(key_h));
    if (auto it = cache_.find(key); it != cache_.end()) {
        Cached& c = it->second;
        if (c.hash == n.hash && c.avail_w == avail_w && c.avail_h == key_h) {
            c.frame = frame_;
            return c.size;
        }
    }

    const Style& s = n.style;
    const float pad_x = s.padding.left + s.padding.right;
    const float pad_y = s.padding.top + s.padding.bottom;
    const float outer_w = s.width >= 0 ? s.width : std::min(avail_w, s.max_width);
    const float outer_h = s.height >= 0 ? s.height : std::min(avail_h, s.max_height);
    const float inner_w = std::max(outer_w - pad_x, 0.0f);
    const float inner_h = std::max(outer_h - pad_y, 0.0f);

    Size content;
    if (leaf) {
        ++measure_calls_;
        content = n.measure.fn(n.measure.context, inner_w);
    } else {
        content = flex(id, inner_w, inner_h, false, 0, 0);
    }
    Size size{s.width >= 0 ? s.width : content.w + pad_x,
              s.height >= 0 ? s.height : content.h + pad_y};
    size.w = clamp(size.w, s.min_width, s.max_width);
    size.h = clamp(size.h, s.min_height, s.max_height);
    cache_.insert_or_assign(key, Cached{n.hash, avail_w, key_h, size, frame_});
    return size;
}

auto Layout::flex(NodeId id, float inner_w, float inner_h, bool place, float x0, float y0)
    -> Size {
    const Style s = nodes_[id].style;
    const bool row = s.axis == Axis::Row;
    const float inner_main = row ? inner_w : inner_h;
    const float inner_cross = row ? inner_h : inner_w;
    if (s.wrap && row && bounded(inner_w)) {
        return wrap_rows(id, inner_w, place, x0, y0);
    }

    // items_ is a stack shared with nested calls: this call owns [first, first + count) and
    // reads it by index, because nested calls may grow (reallocate) the vector.
    const std::size_t first = items_.size();
    for (NodeId c = nodes_[id].first_child; c != kNone; c = nodes_[c].next_sibling) {
        const Size m = row ? measure(c, kUnbounded, inner_cross) : measure(c, inner_cross, kUnbounded);
        const float base = row ? m.w : m.h;
        items_.push_back({c, base, base, -1.0f, false});
    }
    const std::size_t count = items_.size() - first;
    if (count == 0) {
        return {0, 0};
    }
    const float gaps = s.gap * static_cast<float>(count - 1);

    // Main axis: share free space by grow, or take overflow by shrink x base, freezing items
    // that hit their max (min) and sharing again among the rest.
    if (bounded(inner_main)) {
        float sum_base = 0;
        for (std::size_t i = first; i < first + count; ++i) {
            sum_base += items_[i].base;
        }
        const float free = inner_main - gaps - sum_base;
        const bool growing = free > 0;
        if (free != 0) {
            for (std::size_t i = first; i < first + count; ++i) {
                const Style& cs = nodes_[items_[i].node].style;
                items_[i].frozen = growing ? cs.grow <= 0 : cs.shrink * items_[i].base <= 0;
            }
            for (std::size_t round = 0; round < count; ++round) {
                float weights = 0, frozen_space = 0, base_space = 0;
                for (std::size_t i = first; i < first + count; ++i) {
                    const Item& it = items_[i];
                    const Style& cs = nodes_[it.node].style;
                    if (it.frozen) {
                        frozen_space += it.main;
                    } else {
                        weights += growing ? cs.grow : cs.shrink * it.base;
                        base_space += it.base;
                    }
                }
                if (weights <= 0) {
                    break;
                }
                const float remaining = inner_main - gaps - frozen_space - base_space;
                bool clamped = false;
                for (std::size_t i = first; i < first + count; ++i) {
                    Item& it = items_[i];
                    if (it.frozen) {
                        continue;
                    }
                    const Style& cs = nodes_[it.node].style;
                    const float weight = growing ? cs.grow : cs.shrink * it.base;
                    const float target = it.base + remaining * weight / weights;
                    const float limited = clamp(target, main_min(cs, row), main_max(cs, row));
                    it.main = limited;
                    if (limited != target) {
                        it.frozen = true;
                        clamped = true;
                    }
                }
                if (!clamped) {
                    break;
                }
                for (std::size_t i = first; i < first + count; ++i) { // re-share among the rest
                    if (!items_[i].frozen) {
                        items_[i].main = items_[i].base;
                    }
                }
            }
        }
    }
    float main_total = gaps;
    for (std::size_t i = first; i < first + count; ++i) {
        main_total += items_[i].main;
    }

    // Cross axis: a stretched child with no cross size takes the line; others their own size.
    const auto natural_cross = [&](std::size_t i) {
        const Item it = items_[i];
        const Size m = row ? measure(it.node, it.main, inner_cross) : measure(it.node, inner_cross, it.main);
        return row ? m.h : m.w;
    };
    const auto stretched = [&](std::size_t i) {
        const Style& cs = nodes_[items_[i].node].style;
        const Align align = cs.align_self == Align::Auto ? s.align : cs.align_self;
        return align == Align::Stretch && (row ? cs.height : cs.width) < 0;
    };
    float line = inner_cross;
    if (!bounded(inner_cross)) {
        line = 0;
        for (std::size_t i = first; i < first + count; ++i) {
            items_[i].cross = natural_cross(i);
            line = std::max(line, items_[i].cross);
        }
    }
    float content_cross = 0;
    for (std::size_t i = first; i < first + count; ++i) {
        if (stretched(i)) {
            const Style& cs = nodes_[items_[i].node].style;
            items_[i].cross = row ? clamp(line, cs.min_height, cs.max_height)
                                  : clamp(line, cs.min_width, cs.max_width);
        } else if (items_[i].cross < 0) {
            items_[i].cross = natural_cross(i);
        }
        content_cross = std::max(content_cross, items_[i].cross);
    }

    if (place) {
        const float free = bounded(inner_main) ? std::max(inner_main - main_total, 0.0f) : 0.0f;
        float pos = 0;
        float between = s.gap;
        switch (s.justify) {
        case Justify::Start: break;
        case Justify::Center: pos = free / 2; break;
        case Justify::End: pos = free; break;
        case Justify::SpaceBetween:
            if (count > 1) {
                between += free / static_cast<float>(count - 1);
            }
            break;
        }
        for (std::size_t i = first; i < first + count; ++i) {
            const Item it = items_[i];
            const Style& cs = nodes_[it.node].style;
            const Align align = cs.align_self == Align::Auto ? s.align : cs.align_self;
            const float offset = align == Align::Center ? (line - it.cross) / 2
                                 : align == Align::End  ? line - it.cross
                                                        : 0.0f;
            arrange(it.node, row ? Box{x0 + pos, y0 + offset, it.main, it.cross}
                                 : Box{x0 + offset, y0 + pos, it.cross, it.main});
            pos += it.main + between;
        }
    }
    items_.resize(first);
    return row ? Size{main_total, content_cross} : Size{content_cross, main_total};
}

auto Layout::wrap_rows(NodeId id, float inner_w, bool place, float x0, float y0) -> Size {
    const Style s = nodes_[id].style;
    const std::size_t first = items_.size(); // a stack slice, read by index (see flex)
    for (NodeId c = nodes_[id].first_child; c != kNone; c = nodes_[c].next_sibling) {
        const float natural = measure(c, kUnbounded, kUnbounded).w;
        items_.push_back({c, natural, std::min(natural, inner_w), -1.0f, false});
    }
    const std::size_t end = items_.size();
    float y = 0;
    float widest = 0;
    for (std::size_t i = first; i < end;) {
        // A line: as many as fit, at least one.
        std::size_t last = i;
        float used = 0;
        while (last < end) {
            const float add = (last > i ? s.gap : 0.0f) + items_[last].main;
            if (last > i && used + add > inner_w) {
                break;
            }
            used += add;
            ++last;
        }
        float weights = 0;
        for (std::size_t k = i; k < last; ++k) {
            weights += nodes_[items_[k].node].style.grow;
        }
        if (weights > 0 && used < inner_w) {
            const float free = inner_w - used;
            used = 0;
            for (std::size_t k = i; k < last; ++k) {
                const Style& cs = nodes_[items_[k].node].style;
                items_[k].main = clamp(items_[k].main + free * cs.grow / weights, cs.min_width, cs.max_width);
                used += (k > i ? s.gap : 0.0f) + items_[k].main;
            }
        }
        float line = 0;
        for (std::size_t k = i; k < last; ++k) {
            items_[k].cross = measure(items_[k].node, items_[k].main, kUnbounded).h;
            line = std::max(line, items_[k].cross);
        }
        if (place) {
            const float free = std::max(inner_w - used, 0.0f);
            const auto count = static_cast<float>(last - i);
            float pos = s.justify == Justify::Center ? free / 2 : s.justify == Justify::End ? free : 0.0f;
            const float between = s.gap + (s.justify == Justify::SpaceBetween && count > 1 ? free / (count - 1) : 0.0f);
            for (std::size_t k = i; k < last; ++k) {
                const Item it = items_[k];
                const Style& cs = nodes_[it.node].style;
                const Align align = cs.align_self == Align::Auto ? s.align : cs.align_self;
                const bool stretch = align == Align::Stretch && cs.height < 0;
                const float h = stretch ? clamp(line, cs.min_height, cs.max_height) : it.cross;
                const float offset = align == Align::Center ? (line - h) / 2 : align == Align::End ? line - h : 0.0f;
                arrange(it.node, {x0 + pos, y0 + y + offset, it.main, h});
                pos += it.main + between;
            }
        }
        widest = std::max(widest, used);
        y += line + (last < end ? s.gap : 0.0f);
        i = last;
    }
    items_.resize(first);
    return {widest, y};
}

void Layout::arrange(NodeId id, const Box& box) {
    nodes_[id].box = box;
    const Node& n = nodes_[id];
    if (n.first_child == kNone) {
        return;
    }
    const Insets& p = n.style.padding;
    flex(id, std::max(box.w - p.left - p.right, 0.0f), std::max(box.h - p.top - p.bottom, 0.0f),
         true, box.x + p.left, box.y + p.top);
}

} // namespace ui::layout
