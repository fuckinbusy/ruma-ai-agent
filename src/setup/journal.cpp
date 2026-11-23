#include "setup/journal.hpp"

#include "core/json.hpp"
#include "core/utf8.hpp"

#include <glaze/glaze.hpp>

namespace setup {

// The file's shape: UTF-8 strings throughout (paths and registry names are wide in the program).
// Outside the anonymous namespace: glaze's reflection needs types with external linkage.
namespace doc {
struct ValueDoc {
    std::string key;
    std::string name;
};

struct JournalDoc {
    std::string version;
    std::string root;
    std::vector<std::string> files;
    std::vector<std::string> dirs;
    std::vector<std::string> shortcuts;
    std::vector<std::string> keys;
    std::vector<ValueDoc> values;
};
} // namespace doc

namespace {

using doc::JournalDoc;
using doc::ValueDoc;

auto narrow(std::wstring_view w) -> std::string {
    auto s = core::utf8::to_utf8(w);
    return s ? std::move(*s) : std::string{};
}

auto narrow_all(const std::vector<std::filesystem::path>& paths) -> std::vector<std::string> {
    std::vector<std::string> out;
    out.reserve(paths.size());
    for (const auto& p : paths)
        out.push_back(narrow(p.native()));
    return out;
}

auto widen(std::string_view s) -> core::Result<std::wstring> {
    return core::utf8::to_wide(s);
}

auto widen_paths(const std::vector<std::string>& texts) -> core::Result<std::vector<std::filesystem::path>> {
    std::vector<std::filesystem::path> out;
    out.reserve(texts.size());
    for (const auto& t : texts) {
        auto w = widen(t);
        if (!w)
            return std::unexpected{w.error()};
        out.emplace_back(std::move(*w));
    }
    return out;
}

} // namespace

auto journal_path(const std::filesystem::path& root) -> std::filesystem::path {
    return root / kJournalName;
}

auto to_json(const Journal& journal) -> std::string {
    JournalDoc doc;
    doc.version = journal.version;
    doc.root = narrow(journal.root.native());
    doc.files = narrow_all(journal.files);
    doc.dirs = narrow_all(journal.dirs);
    doc.shortcuts = narrow_all(journal.shortcuts);
    for (const auto& k : journal.keys)
        doc.keys.push_back(narrow(k));
    for (const auto& v : journal.values)
        doc.values.push_back({narrow(v.key), narrow(v.name)});
    auto text = core::json::write_pretty(doc);
    return text ? std::move(*text) : std::string{};
}

auto journal_from(std::string_view text) -> core::Result<Journal> {
    JournalDoc doc;
    if (const auto ec = glz::read<glz::opts{.error_on_unknown_keys = false}>(doc, text))
        return core::make_error(core::Code::Parse, "install.json: " + glz::format_error(ec, text));
    if (doc.root.empty())
        return core::make_error(core::Code::Parse, "install.json: no install folder");

    Journal j;
    j.version = std::move(doc.version);
    auto root = widen(doc.root);
    if (!root)
        return std::unexpected{root.error()};
    j.root = std::move(*root);
    auto files = widen_paths(doc.files);
    auto dirs = widen_paths(doc.dirs);
    auto shortcuts = widen_paths(doc.shortcuts);
    if (!files || !dirs || !shortcuts)
        return core::make_error(core::Code::Parse, "install.json: a path is not UTF-8");
    j.files = std::move(*files);
    j.dirs = std::move(*dirs);
    j.shortcuts = std::move(*shortcuts);
    for (const auto& k : doc.keys) {
        auto w = widen(k);
        if (!w)
            return std::unexpected{w.error()};
        j.keys.push_back(std::move(*w));
    }
    for (const auto& v : doc.values) {
        auto key = widen(v.key);
        auto name = widen(v.name);
        if (!key || !name)
            return core::make_error(core::Code::Parse, "install.json: a registry name is not UTF-8");
        j.values.push_back({std::move(*key), std::move(*name)});
    }
    return j;
}

} // namespace setup
