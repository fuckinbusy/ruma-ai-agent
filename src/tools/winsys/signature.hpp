#pragma once

// tools::winsys::signature — is a file signed, by whom, and is it Microsoft's (phase 13a § 1):
// WinVerifyTrust on its embedded signature, else on the Windows catalog that holds its hash.

#include <filesystem>
#include <string>

namespace tools::winsys {

struct Signature {
    bool signed_ok = false; // a valid signature, embedded or by catalog
    bool microsoft = false; // signed by Microsoft (its leaf certificate's subject)
    std::string publisher;  // the leaf certificate's subject name; empty when unsigned
    std::string sha256;     // lowercase hex; empty for a file over 512 MB or unreadable
};

[[nodiscard]] auto signature(const std::filesystem::path& file) -> Signature;

} // namespace tools::winsys
