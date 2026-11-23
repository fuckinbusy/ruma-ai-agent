#pragma once

// agent-reader.exe's only way into COM (phase 13a § 6): WMI queries over named classes and the
// Task Scheduler's folders, tasks and definitions. Nothing here can change either.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace tools::winsys::readonly {

// One row of a WMI query, its properties as text, flag, number or list of text.
class Row {
public:
    virtual ~Row() = default;
    [[nodiscard]] virtual auto text(const wchar_t* name) const -> std::wstring = 0;
    [[nodiscard]] virtual auto flag(const wchar_t* name) const -> bool = 0;
    [[nodiscard]] virtual auto number(const wchar_t* name) const -> std::int64_t = 0;
    [[nodiscard]] virtual auto texts(const wchar_t* name) const -> std::vector<std::wstring> = 0;
};

// `wql` (a SELECT over one of the classes the wrapper names) in `ns`, each row to `f`; false when
// the namespace or the query could not be read, or the class is not one of them.
auto select(const wchar_t* ns, const std::wstring& wql, const std::function<void(const Row&)>& f) -> bool;

struct TaskRow {
    std::wstring path, account, command;
    bool enabled = true;
};
// Every task, hidden ones too; the folders that could not be read into `not_read`. False: the
// Task Scheduler could not be opened at all.
auto tasks(std::vector<TaskRow>& out, std::vector<std::wstring>& not_read) -> bool;

} // namespace tools::winsys::readonly
