#include "ai/cassette.hpp"

#include "ai/chat.hpp"
#include "core/hash.hpp"
#include "net/sse.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace ai::cassette {

namespace {

constexpr std::string_view kHeader = "# winaiagent cassette v1";
constexpr std::string_view kProfile = "C:\\Users\\user";
constexpr std::string_view kComputer = "PC";

auto lower(std::string_view s) -> std::string {
    std::string out{s};
    std::ranges::transform(out, out.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
    return out;
}

// `s` with every `needle` (ASCII case aside) made `with`.
// A picture (data:<mime>;base64,…) is the user's — a photo, the screen: the recording keeps its
// hash and size only (phase 9). Before the names are replaced, which could touch base64 text.
auto without_images(std::string_view text) -> std::string {
    static constexpr std::string_view kStart = "data:image/";
    static constexpr std::string_view kMark = ";base64,";
    std::string out;
    std::size_t from = 0;
    for (auto at = text.find(kStart); at != std::string_view::npos; at = text.find(kStart, from)) {
        const auto mark = text.find(kMark, at);
        const auto quote = text.find('"', at);
        if (mark == std::string_view::npos || quote == std::string_view::npos || mark > quote) {
            out.append(text.substr(from, at + kStart.size() - from));
            from = at + kStart.size();
            continue;
        }
        const auto data = text.substr(mark + kMark.size(), quote - mark - kMark.size());
        const auto pad = static_cast<std::size_t>(std::ranges::count(data.substr(data.size() > 2 ? data.size() - 2 : 0), '='));
        out.append(text.substr(from, at - from));
        out += std::format("<image fnv1a64:{:016x} {} bytes>", core::hash::fnv1a64(data), data.size() / 4 * 3 - pad);
        from = quote;
    }
    out.append(text.substr(from));
    return out;
}

auto replaced(const std::string& s, std::string_view needle, std::string_view with) -> std::string {
    if (needle.empty()) {
        return s;
    }
    const std::string low = lower(s);
    const std::string want = lower(needle);
    std::string out;
    std::size_t from = 0;
    for (auto at = low.find(want); at != std::string::npos; at = low.find(want, from)) {
        out.append(s, from, at - from);
        out += with;
        from = at + want.size();
    }
    out.append(s, from);
    return out;
}

// Each backslash `n` times: a path as it reads inside JSON (2) and inside JSON in JSON (4).
auto escaped(std::string_view path, int n) -> std::string {
    std::string out;
    for (const char c : path) {
        out.append(c == '\\' ? static_cast<std::size_t>(n) : 1, c);
    }
    return out;
}

struct Exchange {
    std::string path, body;
    int status = 0;
    std::vector<std::string> events; // a 2xx: each event's data
    std::string error_body;          // a non-2xx
};

// The recorder's state: the exchange in flight, written whole when it ends.
struct Writer {
    std::filesystem::path file;
    std::string profile, computer;
    std::mutex mutex;
    Exchange pending;
};

auto parse(const std::string& text) -> core::Result<std::vector<Exchange>> {
    std::vector<std::string> lines;
    std::size_t from = 0;
    while (from <= text.size()) {
        auto end = text.find('\n', from);
        std::string line = text.substr(from, end == std::string::npos ? std::string::npos : end - from);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
        if (end == std::string::npos) {
            break;
        }
        from = end + 1;
    }
    if (lines.empty() || lines.front() != kHeader) {
        return core::make_error(core::Code::Parse, "not a cassette (no \"# winaiagent cassette v1\" line)");
    }
    std::vector<Exchange> out;
    std::size_t i = 1;
    while (i < lines.size()) {
        if (lines[i].empty()) {
            ++i;
            continue;
        }
        if (!lines[i].starts_with(">>> POST ") || i + 2 >= lines.size() || !lines[i + 2].starts_with("<<< ")) {
            return core::make_error(core::Code::Parse, std::format("the cassette's line {} does not start an exchange", i + 1));
        }
        Exchange e{.path = lines[i].substr(9), .body = lines[i + 1]};
        const std::string_view status = std::string_view{lines[i + 2]}.substr(4);
        if (std::from_chars(status.data(), status.data() + status.size(), e.status).ec != std::errc{}) {
            return core::make_error(core::Code::Parse, std::format("the cassette's line {}: not a status", i + 3));
        }
        i += 3;
        std::string content;
        while (i < lines.size() && !lines[i].starts_with(">>> ")) {
            content += lines[i];
            content += '\n';
            ++i;
        }
        if (e.status >= 200 && e.status <= 299) {
            net::sse::Parser sse;
            const auto bytes = std::as_bytes(std::span{content.data(), content.size()});
            if (const auto fed = sse.feed(bytes, [&](const net::sse::Event& ev) { e.events.push_back(ev.data); }); !fed) {
                return std::unexpected{fed.error()};
            }
        } else {
            while (!content.empty() && content.back() == '\n') {
                content.pop_back();
            }
            e.error_body = std::move(content);
        }
        out.push_back(std::move(e));
    }
    return out;
}

// Recordings are of the catalog's first provider (format v1 names none).
void play(const Exchange& e, const StreamSink& sink) {
    const catalog::Provider& by = catalog::provider_of(catalog::default_model());
    if (e.status == 0) {
        sink.on_done(core::make_error(core::Code::Net, "no answer (as recorded)"));
        return;
    }
    if (e.status < 200 || e.status > 299) {
        sink.on_done(std::unexpected{chat::error_of(by, net::Response{.status = e.status, .headers = {}, .error_body = e.error_body})});
        return;
    }
    for (const auto& data : e.events) {
        auto chunk = chat::parse_chunk(data);
        if (!chunk) {
            sink.on_done(std::unexpected{chunk.error()});
            return;
        }
        for (const auto& d : chunk->deltas) {
            if (sink.on_delta) {
                sink.on_delta(d);
            }
        }
    }
    sink.on_done({});
}

} // namespace

auto scrub(std::string_view text, std::string_view profile, std::string_view computer) -> std::string {
    std::string out = without_images(text);
    if (!profile.empty()) {
        for (const int n : {4, 2, 1}) {
            out = replaced(out, escaped(profile, n), escaped(kProfile, n));
        }
        // The user's name alone too (a greeting, a process's owner): the folder's last part.
        const auto slash = profile.find_last_of("\\/");
        const auto name = slash == std::string_view::npos ? profile : profile.substr(slash + 1);
        if (name.size() >= 3) {
            out = replaced(out, name, "user");
        }
    }
    if (!computer.empty()) {
        out = replaced(out, computer, kComputer);
    }
    return out;
}

auto recorder(const std::filesystem::path& file, std::string profile, std::string computer) -> core::Result<Tap> {
    {
        std::ofstream out{file, std::ios::binary | std::ios::trunc};
        if (!(out << kHeader << '\n')) {
            return core::make_error(core::Code::Io, "the cassette could not be written: " + file.string());
        }
    }
    auto w = std::make_shared<Writer>();
    w->file = file;
    w->profile = std::move(profile);
    w->computer = std::move(computer);
    return Tap{.request =
                   [w](std::string_view path, std::string_view body) {
                       const std::scoped_lock lock{w->mutex};
                       w->pending = Exchange{.path = std::string{path}, .body = scrub(body, w->profile, w->computer)};
                   },
               .event =
                   [w](std::string_view data) {
                       const std::scoped_lock lock{w->mutex};
                       w->pending.events.push_back(scrub(data, w->profile, w->computer));
                   },
               .answer =
                   [w](int status, std::string_view error_body) {
                       const std::scoped_lock lock{w->mutex};
                       std::string block = std::format(">>> POST {}\n{}\n<<< {}\n", w->pending.path, w->pending.body, status);
                       if (status >= 200 && status <= 299) {
                           for (const auto& data : w->pending.events) {
                               std::size_t from = 0;
                               for (auto end = data.find('\n', from);; end = data.find('\n', from)) {
                                   block += "data: ";
                                   block.append(data, from, end == std::string::npos ? std::string::npos : end - from);
                                   block += '\n';
                                   if (end == std::string::npos) {
                                       break;
                                   }
                                   from = end + 1;
                               }
                               block += '\n';
                           }
                       } else if (!error_body.empty()) {
                           block += scrub(error_body, w->profile, w->computer);
                           block += '\n';
                       }
                       std::ofstream out{w->file, std::ios::binary | std::ios::app};
                       out << block;
                       w->pending = {};
                   }};
}

auto player(const std::filesystem::path& file) -> core::Result<Provider> {
    std::ifstream in{file, std::ios::binary};
    if (!in) {
        return core::make_error(core::Code::NotFound, "no cassette at " + file.string());
    }
    auto exchanges = parse({std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}});
    if (!exchanges) {
        return std::unexpected{exchanges.error()};
    }
    auto played = std::make_shared<std::vector<Exchange>>(std::move(*exchanges));
    auto next = std::make_shared<std::atomic<std::size_t>>(0);
    return Provider{[played, next](const ChatRequest&, StreamSink sink) -> Stream {
        const std::size_t k = next->fetch_add(1);
        if (k >= played->size()) {
            sink.on_done(core::make_error(core::Code::Parse, std::format("the cassette has {} exchanges; call {} is past them", played->size(), k + 1)));
        } else {
            play((*played)[k], sink);
        }
        return Stream{.cancel = [] {}};
    }};
}

} // namespace ai::cassette
