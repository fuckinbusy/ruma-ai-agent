#include "tools/winsys/private_rules.hpp"

#include "core/fs.hpp"
#include "core/json.hpp"

#include <glaze/glaze.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

namespace tools::winsys {

auto load_private(const std::filesystem::path& file) -> std::vector<PrivateRule> {
    std::vector<PrivateRule> rules;
    if (file.empty()) {
        return rules;
    }
    std::ifstream in{file, std::ios::binary};
    if (!in) {
        return rules;
    }
    std::ostringstream text;
    text << in.rdbuf();
    if (glz::read<glz::opts{.error_on_unknown_keys = false}>(rules, text.str())) {
        rules.clear(); // broken: as if none (the file stays for the user to see)
    }
    return rules;
}

namespace {

constexpr std::size_t kMaxRules = 50;

auto trimmed(std::string s) -> std::string {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.erase(s.begin());
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.pop_back();
    }
    return s;
}

} // namespace

auto add_private(const std::filesystem::path& file, std::string program, std::string words, std::string date) -> core::Result<PrivateRule> {
    program = trimmed(std::move(program));
    words = trimmed(std::move(words));
    if (program.empty() && words.empty()) {
        return core::make_error(core::Code::InvalidArgument, "give a program or words in a window's title");
    }
    auto rules = load_private(file);
    for (const auto& r : rules) {
        if (_stricmp(r.program.c_str(), program.c_str()) == 0 && _stricmp(r.words.c_str(), words.c_str()) == 0) {
            return r; // already there
        }
    }
    if (rules.size() >= kMaxRules) {
        return core::make_error(core::Code::InvalidArgument, "the private windows list is full (50): ask the user to forget one");
    }
    std::uint64_t last = 0;
    for (const auto& r : rules) {
        last = std::max(last, r.id);
    }
    PrivateRule rule{.id = last + 1, .program = std::move(program), .words = std::move(words), .date = std::move(date)};
    rules.push_back(rule);
    if (auto saved = save_private(file, rules); !saved) {
        return std::unexpected{saved.error()};
    }
    return rule;
}

auto forget_private(const std::filesystem::path& file, std::uint64_t id) -> bool {
    auto rules = load_private(file);
    if (std::erase_if(rules, [&](const PrivateRule& r) { return r.id == id; }) == 0) {
        return false;
    }
    return save_private(file, rules).has_value();
}

auto describe(const PrivateRule& r) -> std::string {
    const std::string quoted = "\xE2\x80\x9C" + r.words + "\xE2\x80\x9D";
    if (r.program.empty()) {
        return "Windows with " + quoted + " in the title";
    }
    return r.words.empty() ? r.program : r.program + " windows with " + quoted;
}

auto save_private(const std::filesystem::path& file, const std::vector<PrivateRule>& rules) -> core::Result<void> {
    const auto json = core::json::write_pretty(rules);
    if (!json) {
        return core::make_error(core::Code::Io, "the private windows list could not be written");
    }
    return core::fs::write_file_atomic(file, *json);
}

} // namespace tools::winsys
