#pragma once

// tools::winsys::RegistryView — the registry read, never written (phase 13a, rule 2). The real one
// reads HKLM (64-bit view) and HKCU; a test's reads root\HKLM\… and root\HKCU\… under its own
// branch HKCU\Software\WinAiAgent-tests\…, so no test ever depends on or reaches the real Run keys.

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tools::winsys {

enum class Hive : std::uint8_t { Machine, User };

struct RegValue {
    std::wstring name;
    DWORD type = REG_NONE;
    std::wstring text;            // REG_SZ / EXPAND_SZ as stored; MULTI_SZ items one per line; DWORD in decimal
    std::vector<std::byte> bytes; // the raw value
};

// The only branch a test's view may stand on (the fence, as setup::RegistrySystem's).
[[nodiscard]] auto valid_test_root(std::wstring_view root) -> bool;

class RegistryView {
public:
    static auto real() -> RegistryView;
    // Ends the process unless valid_test_root(root).
    explicit RegistryView(std::wstring test_root);

    [[nodiscard]] auto values(Hive hive, std::wstring_view key) const -> std::vector<RegValue>;
    [[nodiscard]] auto subkeys(Hive hive, std::wstring_view key) const -> std::vector<std::wstring>;
    [[nodiscard]] auto value(Hive hive, std::wstring_view key, std::wstring_view name) const -> std::optional<RegValue>;
    // Where `key` of `hive` really is: the real hive, or HKCU and the test branch.
    [[nodiscard]] auto locate(Hive hive, std::wstring_view key) const -> std::pair<HKEY, std::wstring>;
    [[nodiscard]] auto is_test() const noexcept -> bool { return !root_.empty(); }

private:
    RegistryView() = default;
    std::wstring root_; // empty: the real registry
};

} // namespace tools::winsys
