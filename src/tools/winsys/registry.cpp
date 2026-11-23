#include "tools/winsys/registry.hpp"

#include <cstdlib>
#include <cstring>
#include <format>

namespace tools::winsys {
namespace {

constexpr std::wstring_view kTestBranch = L"Software\\WinAiAgent-tests\\";

// An open key, closed when it goes.
class Key {
public:
    Key(HKEY root, const std::wstring& path, REGSAM extra) {
        if (RegOpenKeyExW(root, path.c_str(), 0, KEY_READ | extra, &h_) != ERROR_SUCCESS) {
            h_ = nullptr;
        }
    }
    ~Key() {
        if (h_ != nullptr) {
            RegCloseKey(h_);
        }
    }
    Key(const Key&) = delete;
    Key& operator=(const Key&) = delete;
    [[nodiscard]] auto get() const -> HKEY { return h_; }
    explicit operator bool() const { return h_ != nullptr; }

private:
    HKEY h_ = nullptr;
};

auto text_of(DWORD type, const std::vector<std::byte>& raw) -> std::wstring {
    switch (type) {
    case REG_SZ:
    case REG_EXPAND_SZ: {
        std::wstring s(raw.size() / sizeof(wchar_t), L'\0');
        std::memcpy(s.data(), raw.data(), s.size() * sizeof(wchar_t));
        s.resize(wcsnlen(s.c_str(), s.size()));
        return s;
    }
    case REG_MULTI_SZ: {
        std::wstring all(raw.size() / sizeof(wchar_t), L'\0');
        std::memcpy(all.data(), raw.data(), all.size() * sizeof(wchar_t));
        std::wstring out;
        std::size_t at = 0;
        while (at < all.size() && all[at] != L'\0') {
            const auto end = all.find(L'\0', at);
            const auto item = all.substr(at, end == std::wstring::npos ? std::wstring::npos : end - at);
            out += (out.empty() ? L"" : L"\n") + item;
            if (end == std::wstring::npos) {
                break;
            }
            at = end + 1;
        }
        return out;
    }
    case REG_DWORD:
        if (raw.size() >= sizeof(DWORD)) {
            DWORD v = 0;
            std::memcpy(&v, raw.data(), sizeof(v));
            return std::to_wstring(v);
        }
        return {};
    default: return {};
    }
}

} // namespace

auto valid_test_root(std::wstring_view root) -> bool {
    return root.starts_with(kTestBranch) && root.size() > kTestBranch.size();
}

auto RegistryView::real() -> RegistryView {
    return RegistryView{};
}

RegistryView::RegistryView(std::wstring test_root) : root_{std::move(test_root)} {
    // The fence: a test's registry is its own branch, never the real hives.
    if (!valid_test_root(root_)) {
        std::abort();
    }
}

auto RegistryView::locate(Hive hive, std::wstring_view key) const -> std::pair<HKEY, std::wstring> {
    if (root_.empty()) {
        return {hive == Hive::Machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, std::wstring{key}};
    }
    return {HKEY_CURRENT_USER, root_ + (hive == Hive::Machine ? L"\\HKLM\\" : L"\\HKCU\\") + std::wstring{key}};
}

auto RegistryView::values(Hive hive, std::wstring_view key) const -> std::vector<RegValue> {
    const auto [root, path] = locate(hive, key);
    const Key k{root, path, static_cast<REGSAM>(root_.empty() && hive == Hive::Machine ? KEY_WOW64_64KEY : 0)};
    std::vector<RegValue> out;
    if (!k) {
        return out;
    }
    DWORD count = 0, longest_name = 0, longest_data = 0;
    if (RegQueryInfoKeyW(k.get(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &count, &longest_name, &longest_data, nullptr,
                         nullptr) != ERROR_SUCCESS) {
        return out;
    }
    std::wstring name(longest_name + 1, L'\0');
    std::vector<std::byte> data(longest_data + sizeof(wchar_t) * 2);
    for (DWORD i = 0; i < count; ++i) {
        DWORD name_len = static_cast<DWORD>(name.size());
        DWORD data_len = static_cast<DWORD>(data.size());
        DWORD type = 0;
        if (RegEnumValueW(k.get(), i, name.data(), &name_len, nullptr, &type, reinterpret_cast<BYTE*>(data.data()), &data_len) !=
            ERROR_SUCCESS) {
            continue;
        }
        RegValue v{.name = std::wstring{name.data(), name_len}, .type = type, .text = {},
                   .bytes = std::vector<std::byte>(data.begin(), data.begin() + data_len)};
        v.text = text_of(type, v.bytes);
        out.push_back(std::move(v));
    }
    return out;
}

auto RegistryView::subkeys(Hive hive, std::wstring_view key) const -> std::vector<std::wstring> {
    const auto [root, path] = locate(hive, key);
    const Key k{root, path, static_cast<REGSAM>(root_.empty() && hive == Hive::Machine ? KEY_WOW64_64KEY : 0)};
    std::vector<std::wstring> out;
    if (!k) {
        return out;
    }
    wchar_t name[256];
    for (DWORD i = 0;; ++i) {
        DWORD len = 256;
        const LSTATUS s = RegEnumKeyExW(k.get(), i, name, &len, nullptr, nullptr, nullptr, nullptr);
        if (s == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (s == ERROR_SUCCESS) {
            out.emplace_back(name, len);
        }
    }
    return out;
}

auto RegistryView::value(Hive hive, std::wstring_view key, std::wstring_view name) const -> std::optional<RegValue> {
    for (auto& v : values(hive, key)) {
        if (_wcsicmp(v.name.c_str(), std::wstring{name}.c_str()) == 0) {
            return std::move(v);
        }
    }
    return std::nullopt;
}

} // namespace tools::winsys
