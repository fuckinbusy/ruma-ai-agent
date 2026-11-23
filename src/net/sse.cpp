#include "net/sse.hpp"

#include <format>
#include <string_view>

namespace net::sse {

namespace {

constexpr std::string_view kBom = "\xEF\xBB\xBF";

} // namespace

auto Parser::feed(std::span<const std::byte> bytes, const std::function<void(const Event&)>& on_event) -> core::Result<void> {
    const auto over = [&] {
        failed_ = true;
        line_.clear();
        data_.clear();
        return core::make_error(core::Code::Parse, std::format("an event of the stream is over {} bytes", max_event_));
    };
    if (failed_) {
        return core::make_error(core::Code::Parse, "the event stream failed before");
    }
    for (const std::byte b : bytes) {
        const char c = static_cast<char>(b);
        if (start_) {
            // A BOM at the very start goes, even when it comes split between pieces.
            if (c == kBom[bom_seen_]) {
                if (++bom_seen_ == kBom.size()) {
                    start_ = false;
                }
                continue;
            }
            line_.append(kBom.substr(0, bom_seen_)); // what looked like a BOM was text
            start_ = false;
        }
        if (after_cr_) {
            after_cr_ = false;
            if (c == '\n') {
                continue; // "\r\n": one end of line, perhaps across two pieces
            }
        }
        if (c == '\r' || c == '\n') {
            after_cr_ = c == '\r';
            line(line_, on_event); // its data is no longer than the line was: within the limit
            line_.clear();
            continue;
        }
        line_.push_back(c);
        if (line_.size() + data_.size() > max_event_) {
            return over();
        }
    }
    return {};
}

void Parser::line(std::string_view text, const std::function<void(const Event&)>& on_event) {
    if (text.empty()) {
        // The end of an event: given only when it has data.
        if (have_data_) {
            data_.pop_back(); // the "\n" after the last data line
            on_event(Event{.event = event_.empty() ? std::string{"message"} : event_, .data = data_, .id = id_});
        }
        data_.clear();
        event_.clear();
        have_data_ = false;
        return;
    }
    if (text.front() == ':') {
        return; // a comment (": keep-alive")
    }
    const auto colon = text.find(':');
    const std::string_view field = text.substr(0, colon);
    std::string_view value = colon == std::string_view::npos ? std::string_view{} : text.substr(colon + 1);
    if (value.starts_with(' ')) {
        value.remove_prefix(1);
    }
    if (field == "data") {
        data_.append(value);
        data_.push_back('\n');
        have_data_ = true;
    } else if (field == "event") {
        event_.assign(value);
    } else if (field == "id") {
        if (value.find('\0') == std::string_view::npos) {
            id_.assign(value);
        }
    } // "retry" and unknown fields: ignored
}

} // namespace net::sse
