#include "security/secrets.hpp"

#include <windows.h>
#include <dpapi.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "crypt32.lib")

namespace security {
namespace {

auto blob(std::string_view s) -> DATA_BLOB {
    return {static_cast<DWORD>(s.size()), reinterpret_cast<BYTE*>(const_cast<char*>(s.data()))};
}

// DPAPI's output, wiped and freed with LocalFree.
struct LocalBlob {
    DATA_BLOB b{};
    LocalBlob() = default;
    LocalBlob(const LocalBlob&) = delete;
    auto operator=(const LocalBlob&) -> LocalBlob& = delete;
    ~LocalBlob() {
        if (b.pbData != nullptr) {
            SecureZeroMemory(b.pbData, b.cbData);
            LocalFree(b.pbData); // mem-ok: DPAPI allocates its output with LocalAlloc; only LocalFree frees it
        }
    }
};

} // namespace

SecureString::SecureString(std::string_view text) : data_(std::make_unique<char[]>(text.size())), size_(text.size()) {
    std::memcpy(data_.get(), text.data(), text.size());
}

SecureString::~SecureString() {
    wipe();
}

SecureString::SecureString(SecureString&& other) noexcept : data_(std::move(other.data_)), size_(other.size_) {
    other.size_ = 0;
}

auto SecureString::operator=(SecureString&& other) noexcept -> SecureString& {
    if (this != &other) {
        wipe();
        data_ = std::move(other.data_);
        size_ = other.size_;
        other.size_ = 0;
    }
    return *this;
}

void SecureString::wipe() noexcept {
    if (data_) {
        SecureZeroMemory(data_.get(), size_);
    }
    data_.reset();
    size_ = 0;
}

namespace {

constexpr std::string_view kForm = "WAK2\n"; // the file's plain text starts with it; then "name\nkey\n" for each key

struct Named {
    std::string name;
    SecureString key;
};

// The file's keys. NotFound: no file. AccessDenied: it cannot be decrypted.
auto read_keys(const std::filesystem::path& file, std::string_view unnamed_is, std::string_view entropy)
    -> core::Result<std::vector<Named>> {
    std::ifstream f(file, std::ios::binary);
    if (!f) {
        // No file is "no key"; a file that is there and does not open (held by another program for
        // a moment) is neither that nor a damaged one: whoever stores must not write over it.
        std::error_code ec;
        if (std::filesystem::exists(file, ec)) {
            return core::make_error(core::Code::Io, "the stored API keys cannot be opened right now");
        }
        return core::make_error(core::Code::NotFound, "no stored API key");
    }
    const std::string bytes{std::istreambuf_iterator<char>{f}, std::istreambuf_iterator<char>{}};
    DATA_BLOB in = blob(bytes);
    DATA_BLOB ent = blob(entropy);
    LocalBlob out; // wiped when it goes: the keys in the clear
    if (!CryptUnprotectData(&in, nullptr, &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out.b)) {
        return core::make_error(core::Code::AccessDenied,
                                "the stored API keys cannot be decrypted (another user or program, or a damaged file)");
    }
    std::string_view plain{reinterpret_cast<const char*>(out.b.pbData), out.b.cbData};
    std::vector<Named> keys;
    if (!plain.starts_with(kForm)) { // the earlier kind: the one key itself
        if (!plain.empty()) {
            keys.push_back({std::string{unnamed_is}, SecureString{plain}});
        }
        return keys;
    }
    plain.remove_prefix(kForm.size());
    while (!plain.empty()) {
        const auto name_end = plain.find('\n');
        const auto key_end = name_end == std::string_view::npos ? name_end : plain.find('\n', name_end + 1);
        if (key_end == std::string_view::npos) {
            return core::make_error(core::Code::AccessDenied, "the stored API keys are damaged");
        }
        keys.push_back({std::string{plain.substr(0, name_end)}, SecureString{plain.substr(name_end + 1, key_end - name_end - 1)}});
        plain.remove_prefix(key_end + 1);
    }
    return keys;
}

auto write_keys(const std::vector<Named>& keys, const std::filesystem::path& file, std::string_view entropy) -> core::Result<void> {
    std::size_t size = kForm.size();
    for (const auto& k : keys) {
        size += k.name.size() + 1 + k.key.view().size() + 1;
    }
    // The plain text in a buffer wiped on every way out, never in a std::string.
    struct Wiped {
        std::unique_ptr<char[]> data;
        std::size_t size = 0;
        Wiped(const Wiped&) = delete;
        auto operator=(const Wiped&) -> Wiped& = delete;
        explicit Wiped(std::size_t n) : data(std::make_unique<char[]>(n)), size(n) {}
        ~Wiped() { SecureZeroMemory(data.get(), size); }
    } plain{size};
    char* at = plain.data.get();
    const auto put = [&at](std::string_view s) {
        std::memcpy(at, s.data(), s.size());
        at += s.size();
    };
    put(kForm);
    for (const auto& k : keys) {
        put(k.name);
        put("\n");
        put(k.key.view());
        put("\n");
    }
    DATA_BLOB in = blob(std::string_view{plain.data.get(), plain.size});
    DATA_BLOB ent = blob(entropy);
    LocalBlob out;
    if (!CryptProtectData(&in, L"WinAiAgent API key", &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out.b)) {
        return core::os_error(GetLastError(), "encrypting the API keys");
    }
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    auto tmp = file;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(out.b.pbData), static_cast<std::streamsize>(out.b.cbData));
        if (!f.flush()) {
            return core::make_error(core::Code::Io, "writing secrets.bin");
        }
    }
    if (!MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return core::os_error(GetLastError(), "replacing secrets.bin");
    }
    return {};
}

auto one_line(std::string_view s) -> bool {
    return !s.empty() && s.find('\n') == std::string_view::npos && s.find('\r') == std::string_view::npos;
}

} // namespace

auto store_key(std::string_view name, std::string_view key, const std::filesystem::path& file, std::string_view unnamed_is,
               std::string_view entropy) -> core::Result<void> {
    if (!one_line(name) || !one_line(key)) {
        return core::make_error(core::Code::InvalidArgument, "a key and its provider's name are one line each, not empty");
    }
    // No file, or one that cannot be decrypted (what it held is lost either way): a new one. One that
    // is there and did not open: nothing is written.
    auto read = read_keys(file, unnamed_is, entropy);
    if (!read && read.error().code == core::Code::Io) {
        return std::unexpected{read.error()};
    }
    auto keys = std::move(read).value_or(std::vector<Named>{});
    const auto it = std::ranges::find(keys, name, &Named::name);
    if (it != keys.end()) {
        it->key = SecureString{key};
    } else {
        keys.push_back({std::string{name}, SecureString{key}});
    }
    return write_keys(keys, file, entropy);
}

auto load_key(std::string_view name, const std::filesystem::path& file, std::string_view unnamed_is, std::string_view entropy)
    -> core::Result<SecureString> {
    auto keys = read_keys(file, unnamed_is, entropy);
    if (!keys) {
        return std::unexpected{keys.error()};
    }
    const auto it = std::ranges::find(*keys, name, &Named::name);
    if (it == keys->end()) {
        return core::make_error(core::Code::NotFound, "no stored API key");
    }
    return std::move(it->key);
}

auto remove_key(std::string_view name, const std::filesystem::path& file, std::string_view unnamed_is, std::string_view entropy)
    -> core::Result<void> {
    auto keys = read_keys(file, unnamed_is, entropy);
    if (!keys) {
        return keys.error().code == core::Code::NotFound ? core::Result<void>{} : core::Result<void>{std::unexpected{keys.error()}};
    }
    if (std::erase_if(*keys, [name](const Named& k) { return k.name == name; }) == 0) {
        return {};
    }
    return write_keys(*keys, file, entropy);
}

} // namespace security
