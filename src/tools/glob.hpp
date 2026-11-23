#pragma once

// fs.search's patterns (phase 12 § 2): a path pattern with ** for any subfolders, and the files
// .gitignore leaves out, as git reads it.

#include <string>
#include <string_view>
#include <vector>

namespace tools::glob {

// `*` and `?` within one folder's name, `**` for any folders (none included; a trailing one only
// what is inside), `[a-z]`, `[!0-9]`; `/` and `\` alike; any case, as Windows' names are.
[[nodiscard]] auto glob_match(std::string_view pattern, std::string_view relative_path) -> bool;

// The rules of the .gitignore files from the search's folder down: patterns, an anchoring `/`,
// `**`, `!` taking a match back, `dir/` for folders only; a deeper file's rule over a shallower
// one's, a later line over an earlier. `.git` and `node_modules` are always left out.
class Ignore {
public:
    // One .gitignore's text, in `folder` relative to the search's root ("" for the root itself).
    void add(std::string_view folder, std::string_view text);
    // `relative` to the search's root, `/` or `\`.
    [[nodiscard]] auto ignored(std::string_view relative, bool dir) const -> bool;

private:
    struct Rule {
        std::wstring folder;  // relative to the root, '/', lower case
        std::wstring pattern; // relative to its folder, lower case
        bool negate = false;
        bool dir_only = false;
        std::size_t depth = 0; // the folder's
    };
    std::vector<Rule> rules_;
};

} // namespace tools::glob
