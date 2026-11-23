#include "app/memory.hpp"

#include "core/json.hpp"
#include "core/log.hpp"
#include "core/secret_like.hpp"

#include <glaze/glaze.hpp>

#include <algorithm>
#include <format>
#include <fstream>
#include <sstream>
#include <system_error>

namespace app::memory::file {

// memory.json as stored (a named namespace: glaze reflects only types with external linkage).
struct Stored {
    std::vector<Entry> entries;
};

} // namespace app::memory::file

namespace app::memory {
namespace {

using file::Stored;

auto chars(std::string_view s) -> std::size_t {
    return static_cast<std::size_t>(std::ranges::count_if(s, [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

auto trimmed(std::string_view s) -> std::string_view {
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && space(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && space(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

auto slurp(const std::filesystem::path& file) -> std::optional<std::string> {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

} // namespace

auto Memory::load(std::filesystem::path file) -> Memory {
    Memory m;
    m.file_ = std::move(file);
    const auto text = slurp(m.file_);
    if (!text) {
        return m;
    }
    Stored stored;
    if (const auto ec = glz::read<glz::opts{.error_on_unknown_keys = false}>(stored, *text)) {
        WA_WARN("memory", "{} not read: {}; nothing remembered until it is fixed or forgotten", m.file_.string(), glz::format_error(ec, *text));
        return m;
    }
    m.entries_ = std::move(stored.entries);
    for (const auto& e : m.entries_) {
        m.last_ = std::max(m.last_, e.id);
    }
    return m;
}

auto Memory::add(std::string_view text, std::uint64_t task, std::string date) -> core::Result<Entry> {
    const auto t = trimmed(text);
    if (t.empty()) {
        return core::make_error(core::Code::InvalidArgument, "nothing to remember");
    }
    if (chars(t) > kMaxChars) {
        return core::make_error(core::Code::InvalidArgument, std::format("at most {} characters: say it shorter", kMaxChars));
    }
    if (const auto why = core::secret_like::looks_secret(t)) {
        return core::make_error(core::Code::Policy, std::format("not remembered: {}; secrets are never kept", *why));
    }
    if (const auto it = std::ranges::find(entries_, t, &Entry::text); it != entries_.end()) {
        return *it; // remembered already
    }
    if (entries_.size() >= kMaxEntries) {
        return core::make_error(core::Code::InvalidArgument,
                                std::format("the memory is full ({} entries): forget one first with memory.forget", kMaxEntries));
    }
    entries_.push_back({.id = ++last_, .text = std::string{t}, .date = std::move(date), .task = task});
    return entries_.back();
}

auto Memory::forget(std::uint64_t id) -> bool {
    return std::erase_if(entries_, [&](const Entry& e) { return e.id == id; }) > 0;
}

void Memory::clear() {
    entries_.clear();
}

auto Memory::save() const -> core::Result<void> {
    auto json = core::json::write(Stored{entries_});
    if (!json) {
        return core::make_error(core::Code::Internal, "the memory could not be written as JSON");
    }
    std::error_code ec;
    std::filesystem::create_directories(file_.parent_path(), ec);
    auto tmp = file_;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << *json;
        if (!out) {
            return core::make_error(core::Code::Io, "the memory could not be written: " + tmp.string());
        }
    }
    std::filesystem::rename(tmp, file_, ec); // over the old one at once
    if (ec) {
        return core::make_error(core::Code::Io, "the memory could not be saved: " + ec.message());
    }
    return {};
}

auto Memory::prompt_lines() const -> std::string {
    std::string out;
    for (const auto& e : entries_) {
        out += std::format("- {} ({})\n", e.text, e.date);
    }
    return out;
}

auto read_rules(const std::filesystem::path& file) -> std::string {
    auto text = slurp(file);
    if (!text) {
        return {};
    }
    std::string rules = std::move(*text);
    if (rules.starts_with("\xEF\xBB\xBF")) {
        rules.erase(0, 3);
    }
    bool cut = false;
    if (rules.size() > kMaxRules) {
        std::size_t n = kMaxRules;
        while (n > 0 && (static_cast<unsigned char>(rules[n]) & 0xC0) == 0x80) {
            --n;
        }
        rules.resize(n);
        cut = true;
    }
    while (!rules.empty() && (rules.back() == '\n' || rules.back() == '\r' || rules.back() == ' ' || rules.back() == '\t')) {
        rules.pop_back();
    }
    if (cut) {
        rules += "\n[the rest of agent.md was not read: 4 KB at most]";
    }
    return rules;
}

} // namespace app::memory
