#include "ai/stream.hpp"

#include <algorithm>
#include <iterator>

namespace ai {

void Assembler::add(const Delta& d) {
    message_.content += d.content;
    message_.reasoning += d.reasoning;
    if (d.tool_index >= 0) {
        // Calls come by index, interleaved; they are kept in index order.
        const auto at = std::ranges::lower_bound(indexes_, d.tool_index);
        const auto i = static_cast<std::size_t>(std::distance(indexes_.begin(), at));
        if (at == indexes_.end() || *at != d.tool_index) {
            indexes_.insert(at, d.tool_index);
            message_.tool_calls.insert(message_.tool_calls.begin() + static_cast<std::ptrdiff_t>(i), ToolCall{});
        }
        auto& call = message_.tool_calls[i];
        if (!d.tool_id.empty()) {
            call.id = d.tool_id;
        }
        if (!d.tool_name.empty()) {
            call.name = d.tool_name;
        }
        call.arguments += d.tool_arguments;
    }
    if (d.finish != Finish::None) {
        finish_ = d.finish;
    }
    if (d.usage) {
        usage_ = d.usage;
    }
}

} // namespace ai
