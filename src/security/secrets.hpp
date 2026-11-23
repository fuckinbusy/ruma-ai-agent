#pragma once

// security::secrets — the API key in secrets.bin (03-SECURITY § Secrets and logs): encrypted for
// the current user with DPAPI (CryptProtectData) and the application's entropy; in memory only as
// a SecureString, wiped (SecureZeroMemory) when it goes.

#include "core/error.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string_view>

namespace security {

// Holds a secret; wipes it when destroyed. Move-only; never converted to std::string implicitly.
class SecureString {
public:
    SecureString() = default;
    explicit SecureString(std::string_view text);
    ~SecureString();
    SecureString(SecureString&& other) noexcept;
    auto operator=(SecureString&& other) noexcept -> SecureString&;
    SecureString(const SecureString&) = delete;
    auto operator=(const SecureString&) -> SecureString& = delete;

    [[nodiscard]] auto view() const noexcept -> std::string_view { return {data_.get(), size_}; }

private:
    void wipe() noexcept;
    std::unique_ptr<char[]> data_;
    std::size_t size_ = 0;
};

inline constexpr std::string_view kSecretsEntropy = "WinAiAgent/secrets.bin/v1";

// The file holds the keys of several providers, each under its provider's name (UI update, stage
// 4a). Before that it held one key with no name: such a file is read as the key of `unnamed_is`
// (the provider there was then) and is written in the new form at the first store or removal.
// Inside the encryption: a mark of the form, then a name and a key, a line each.

// `name`'s key in `file`, the others kept (the file written next to it first, then moved over it).
// A file that cannot be read is replaced: what it held is lost either way. A name or a key that is
// empty or has a line break: InvalidArgument, nothing written.
auto store_key(std::string_view name, std::string_view key, const std::filesystem::path& file, std::string_view unnamed_is,
               std::string_view entropy = kSecretsEntropy) -> core::Result<void>;

// NotFound without a file or without that name in it; an error, never a key, when the file cannot
// be decrypted (another user, another entropy, a damaged file).
auto load_key(std::string_view name, const std::filesystem::path& file, std::string_view unnamed_is,
              std::string_view entropy = kSecretsEntropy) -> core::Result<SecureString>;

// `name`'s key taken out, the others kept; no such key or no file: nothing to do, no error.
auto remove_key(std::string_view name, const std::filesystem::path& file, std::string_view unnamed_is,
                std::string_view entropy = kSecretsEntropy) -> core::Result<void>;

} // namespace security
