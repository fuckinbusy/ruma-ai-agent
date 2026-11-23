#include "app/history.hpp"

#include "core/log.hpp"
#include "core/utf8.hpp"

#include <windows.h>

#include <glaze/glaze.hpp>

#include <algorithm>
#include <format>
#include <fstream>
#include <system_error>

namespace app::history {

auto json_string(std::string_view s) -> std::string {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    std::size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    out += std::format("\\u{:04x}", c);
                } else {
                    out += static_cast<char>(c);
                }
            }
            ++i;
            continue;
        }
        const auto d = core::utf8::decode(s, i);
        if (d.valid) {
            out.append(s.substr(i, d.length));
            i += d.length;
        } else {
            out += "\xEF\xBF\xBD"; // U+FFFD
            i += d.length > 0 ? d.length : 1;
        }
    }
    out += '"';
    return out;
}

auto to_json(const Record& r) -> std::string {
    std::string plan;
    if (!r.plan.empty()) {
        plan = R"(,"plan":[)";
        for (std::size_t i = 0; i < r.plan.size(); ++i) {
            const auto state = r.plan[i].state;
            plan += std::format(R"({}{{"title":{},"state":"{}"}})", i == 0 ? "" : ",", json_string(r.plan[i].title),
                                state == events::ItemState::Pending  ? "pending"
                                : state == events::ItemState::Active ? "active"
                                : state == events::ItemState::Done   ? "done"
                                                                     : "skipped");
        }
        plan += ']';
    }
    return std::format(R"({{"time":{},"task":{},"kind":{},"question":{},"text":{}{}}})", json_string(r.time), r.task,
                       json_string(r.kind), json_string(r.question), json_string(r.text), plan);
}

auto now_utc() -> std::string {
    SYSTEMTIME t{};
    GetSystemTime(&t);
    return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                       t.wSecond);
}

auto record_of(const sheet::Model& m, const events::Event& e, std::string time) -> std::optional<Record> {
    if (e.task != m.task) {
        return std::nullopt;
    }
    Record r{.time = std::move(time), .task = m.task, .question = m.question};
    if (std::holds_alternative<events::Actions>(e.event)) {
        r.kind = "answer";
        r.text = m.answer;
    } else if (std::holds_alternative<events::Finished>(e.event)) {
        r.kind = "done";
        r.text = m.done_detail.empty() ? m.done_title : m.done_title + ": " + m.done_detail;
    } else if (std::holds_alternative<events::Failed>(e.event)) {
        r.kind = "failed";
        r.text = m.done_title;
    } else {
        return std::nullopt;
    }
    if (m.checklist) {
        r.plan = m.checklist->items;
    }
    return r;
}

auto stopped(const sheet::Model& m, std::uint64_t stopped_task, std::uint64_t answered_task, std::string time)
    -> std::optional<Record> {
    if (m.answer.empty() || stopped_task == answered_task) {
        return std::nullopt;
    }
    return Record{.time = std::move(time),
                  .task = stopped_task,
                  .kind = "stopped",
                  .question = m.question,
                  .text = m.answer,
                  .plan = m.checklist ? m.checklist->items : std::vector<events::ChecklistItem>{}};
}

Writer::Writer(std::filesystem::path dir)
    : dir_(std::move(dir)), thread_([this](const std::stop_token& stop) { run(stop); }) {}

Writer::~Writer() {
    thread_.request_stop();
    ready_.notify_all();
}

void Writer::add(Record r) {
    {
        std::scoped_lock lock{mutex_};
        queue_.push_back(std::move(r));
    }
    ready_.notify_one();
}

void Writer::run(const std::stop_token& stop) {
    for (;;) {
        Record r;
        {
            std::unique_lock lock{mutex_};
            ready_.wait(lock, stop, [&] { return !queue_.empty(); });
            if (queue_.empty()) {
                return; // stopped with nothing left
            }
            r = std::move(queue_.front());
            queue_.pop_front();
        }
        std::error_code ec;
        std::filesystem::create_directories(dir_, ec);
        const auto file = dir_ / (r.time.substr(0, 7) + ".jsonl"); // "2026-10"
        std::ofstream out(file, std::ios::binary | std::ios::app);
        out << to_json(r) << '\n';
        out.close();
        if (out.fail()) {
            // Not file.string(): it throws for a name outside the ANSI code page (review N-5).
            const auto name = core::utf8::to_utf8(file.wstring());
            WA_WARN("app", "history: could not append to {}", name ? *name : std::string{"?"});
        }
    }
}

// What recent_questions reads of a record (glaze reflects it: not in an anonymous namespace).
namespace detail {
struct Line {
    std::uint64_t task = 0;
    std::string question;
};
} // namespace detail

namespace {

// The end of a file, at most `limit` bytes, from the start of a line.
auto tail(const std::filesystem::path& file, std::size_t limit) -> std::string {
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in) {
        return {};
    }
    const auto size = static_cast<std::size_t>(in.tellg());
    const std::size_t from = size > limit ? size - limit : 0;
    std::string text(size - from, '\0');
    in.seekg(static_cast<std::streamoff>(from));
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (from > 0) {
        const auto nl = text.find('\n');
        text.erase(0, nl == std::string::npos ? text.size() : nl + 1); // a cut first line
    }
    return text;
}

} // namespace

auto recent_questions(const std::filesystem::path& dir, std::size_t max) -> std::vector<std::string> {
    std::vector<std::filesystem::path> months;
    std::error_code ec;
    for (std::filesystem::directory_iterator it{dir, ec}, end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() == L".jsonl") {
            months.push_back(it->path());
        }
    }
    std::ranges::sort(months); // "YYYY-MM": by name is by time
    if (months.size() > 2) {
        months.erase(months.begin(), months.end() - 2);
    }
    std::vector<std::string> out;
    std::uint64_t last_task = 0;
    std::string last_question;
    for (const auto& file : months) {
        const std::string text = tail(file, 256 * 1024);
        for (std::size_t pos = 0; pos < text.size();) {
            const auto nl = text.find('\n', pos);
            const std::string_view line{text.data() + pos, (nl == std::string::npos ? text.size() : nl) - pos};
            pos = nl == std::string::npos ? text.size() : nl + 1;
            detail::Line l;
            if (glz::read<glz::opts{.error_on_unknown_keys = false}>(l, line) || l.question.empty()) {
                continue; // not a record: skipped
            }
            if (l.task == last_task && l.question == last_question) {
                continue; // the same task's next step
            }
            last_task = l.task;
            last_question = l.question;
            out.push_back(std::move(l.question));
        }
    }
    if (out.size() > max) {
        out.erase(out.begin(), out.end() - static_cast<std::ptrdiff_t>(max));
    }
    return out;
}

} // namespace app::history
