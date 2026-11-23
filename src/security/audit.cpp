#include "security/audit.hpp"

#include "core/json.hpp"
#include "security/sha256.hpp"

#include <glaze/glaze.hpp>

#include <windows.h>

#include <format>
#include <fstream>
#include <sstream>
#include <string_view>

namespace security {
// Named, not anonymous: glaze reflects only types with external linkage.
namespace audit_detail {

// Field order is the line's order.
struct Body {
    std::string ts;
    std::uint64_t task_id = 0;
    std::string tool;
    std::string args;
    std::string zone;
    std::string decision;
    std::string decided_by;
    double duration_ms = 0;
    int result_code = 0;
    std::string prev_hash;
};

struct Line {
    std::string ts;
    std::uint64_t task_id = 0;
    std::string tool;
    std::string args;
    std::string zone;
    std::string decision;
    std::string decided_by;
    double duration_ms = 0;
    int result_code = 0;
    std::string prev_hash;
    std::string hash;
};

} // namespace audit_detail

namespace {

using audit_detail::Body;
using audit_detail::Line;

constexpr glz::opts kStrict{.error_on_unknown_keys = true, .error_on_missing_keys = true};
constexpr std::string_view kHashKey = R"(,"hash":")";
const std::string kGenesis(64, '0');

auto now_utc() -> std::string {
    SYSTEMTIME t{};
    GetSystemTime(&t);
    return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                       t.wSecond, t.wMilliseconds);
}

auto read_all(const std::filesystem::path& file) -> core::Result<std::string> {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        return core::make_error(core::Code::NotFound, "no audit file");
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return core::make_error(core::Code::Io, "the audit file cannot be read");
    }
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

// A whole line checked against itself: strict JSON, and its hash is the hash of its body.
auto check_line(std::string_view text, std::size_t number) -> core::Result<Line> {
    Line line;
    if (const auto ec = glz::read<kStrict>(line, text)) {
        return core::make_error(core::Code::Parse, std::format("line {}: not an audit record ({})", number,
                                                               glz::format_error(ec, text)));
    }
    const auto at = text.rfind(kHashKey);
    if (at == std::string_view::npos || hex(sha256(text.substr(0, at))) != line.hash) {
        return core::make_error(core::Code::Parse, std::format("line {}: the record was changed (its hash does not match)", number));
    }
    return line;
}

// Lines of a file that ends with a newline; an unterminated end is reported as cut short.
auto split_lines(std::string_view all) -> core::Result<std::vector<std::string_view>> {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (start < all.size()) {
        const auto end = all.find('\n', start);
        if (end == std::string_view::npos) {
            return core::make_error(core::Code::Parse, std::format("line {}: cut short", lines.size() + 1));
        }
        lines.push_back(all.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

auto to_record(Line&& l) -> AuditRecord {
    return {.ts = std::move(l.ts), .task_id = l.task_id, .tool = std::move(l.tool), .args = std::move(l.args),
            .zone = std::move(l.zone), .decision = std::move(l.decision), .decided_by = std::move(l.decided_by),
            .duration_ms = l.duration_ms, .result_code = l.result_code};
}

} // namespace

auto AuditLog::open(const std::filesystem::path& file) -> core::Result<AuditLog> {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::string prev = kGenesis;
    if (auto all = read_all(file)) {
        auto lines = split_lines(*all);
        if (!lines) {
            return std::unexpected{lines.error()};
        }
        if (!lines->empty()) {
            auto last = check_line(lines->back(), lines->size());
            if (!last) {
                return std::unexpected{last.error()};
            }
            prev = std::move(last->hash);
        }
    } else if (all.error().code != core::Code::NotFound) {
        return std::unexpected{all.error()};
    }
    core::win::FileHandle handle{CreateFileW(file.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                             FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (!handle) {
        return core::os_error(GetLastError(), "opening the audit file");
    }
    return AuditLog{std::move(handle), std::move(prev)};
}

auto AuditLog::append(AuditRecord r) -> core::Result<void> {
    std::scoped_lock lock{*mutex_};
    Body body{.ts = r.ts.empty() ? now_utc() : std::move(r.ts), .task_id = r.task_id, .tool = std::move(r.tool),
              .args = std::move(r.args), .zone = std::move(r.zone), .decision = std::move(r.decision),
              .decided_by = std::move(r.decided_by), .duration_ms = r.duration_ms, .result_code = r.result_code,
              .prev_hash = prev_};
    auto json = core::json::write(body);
    if (!json || json->empty() || json->back() != '}') {
        return core::make_error(core::Code::Io, "the audit record could not be written as JSON");
    }
    std::string line = std::move(*json);
    line.pop_back(); // the body: everything but the closing brace
    const std::string hash = hex(sha256(line));
    line += kHashKey;
    line += hash;
    line += "\"}\n";
    DWORD written = 0;
    if (!WriteFile(file_.get(), line.data(), static_cast<DWORD>(line.size()), &written, nullptr) ||
        written != line.size() || !FlushFileBuffers(file_.get())) {
        return core::os_error(GetLastError(), "writing the audit file");
    }
    prev_ = hash;
    return {};
}

auto verify_audit(const std::filesystem::path& file) -> core::Result<std::size_t> {
    auto all = read_all(file);
    if (!all) {
        if (all.error().code == core::Code::NotFound) {
            return std::size_t{0};
        }
        return std::unexpected{all.error()};
    }
    auto lines = split_lines(*all);
    if (!lines) {
        return std::unexpected{lines.error()};
    }
    std::string prev = kGenesis;
    for (std::size_t i = 0; i < lines->size(); ++i) {
        auto line = check_line((*lines)[i], i + 1);
        if (!line) {
            return std::unexpected{line.error()};
        }
        if (line->prev_hash != prev) {
            return core::make_error(core::Code::Parse,
                                    std::format("line {}: the chain is broken (a record before it was removed or changed)", i + 1));
        }
        prev = std::move(line->hash);
    }
    return lines->size();
}

auto read_audit(const std::filesystem::path& file, std::size_t max) -> core::Result<std::vector<AuditRecord>> {
    auto all = read_all(file);
    if (!all) {
        if (all.error().code == core::Code::NotFound) {
            return std::vector<AuditRecord>{};
        }
        return std::unexpected{all.error()};
    }
    auto lines = split_lines(*all);
    if (!lines) {
        return std::unexpected{lines.error()};
    }
    std::vector<AuditRecord> out;
    const std::size_t from = lines->size() > max ? lines->size() - max : 0;
    for (std::size_t i = from; i < lines->size(); ++i) {
        Line line;
        if (const auto ec = glz::read<kStrict>(line, (*lines)[i])) {
            return core::make_error(core::Code::Parse, std::format("line {}: not an audit record", i + 1));
        }
        out.push_back(to_record(std::move(line)));
    }
    return out;
}

} // namespace security
