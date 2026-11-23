#include "tools/winsys/signature.hpp"

#include "core/utf8.hpp"
#include "core/win.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <mscat.h>

#include <array>
#include <format>
#include <vector>

#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace tools::winsys {
namespace {

constexpr std::uint64_t kMaxHashed = 512ULL * 1024 * 1024;

auto name_of(PCCERT_CONTEXT cert) -> std::string {
    wchar_t buf[512]{};
    if (CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, buf, 512) <= 1) {
        return {};
    }
    return core::utf8::to_utf8(buf).value_or("");
}

// WinVerifyTrust over `data`, and the signer's leaf and root names while its state is open.
auto verify(WINTRUST_DATA& data, std::string& leaf, std::string& root) -> bool {
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL; // never a network fetch to check a file
    const LONG status = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    if (status == ERROR_SUCCESS) {
        if (auto* prov = WTHelperProvDataFromStateData(data.hWVTStateData)) {
            if (auto* signer = WTHelperGetProvSignerFromChain(prov, 0, FALSE, 0); signer != nullptr && signer->csCertChain > 0) {
                leaf = name_of(signer->pasCertChain[0].pCert);
                root = name_of(signer->pasCertChain[signer->csCertChain - 1].pCert);
            }
        }
    }
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    return status == ERROR_SUCCESS;
}

// The file's SHA-256, read in pieces; empty past kMaxHashed or unreadable.
auto sha256_of(HANDLE file) -> std::string {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || static_cast<std::uint64_t>(size.QuadPart) > kMaxHashed) {
        return {};
    }
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string out;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 && BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        SetFilePointer(file, 0, nullptr, FILE_BEGIN);
        std::vector<UCHAR> buf(1 << 20);
        DWORD got = 0;
        bool ok = true;
        while (ReadFile(file, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) && got > 0) {
            if (BCryptHashData(hash, buf.data(), got, 0) != 0) {
                ok = false;
                break;
            }
        }
        std::array<UCHAR, 32> digest{};
        if (ok && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0) {
            for (const auto b : digest) {
                out += std::format("{:02x}", b);
            }
        }
    }
    if (hash != nullptr) {
        BCryptDestroyHash(hash);
    }
    if (alg != nullptr) {
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return out;
}

// Signed by a Windows catalog that holds the file's hash (most of Windows' own files).
auto by_catalog(const std::filesystem::path& file, HANDLE h, std::string& leaf, std::string& root) -> bool {
    HCATADMIN admin = nullptr;
    if (!CryptCATAdminAcquireContext2(&admin, nullptr, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) {
        return false;
    }
    bool ok = false;
    DWORD size = 0;
    SetFilePointer(h, 0, nullptr, FILE_BEGIN);
    CryptCATAdminCalcHashFromFileHandle2(admin, h, &size, nullptr, 0);
    std::vector<BYTE> digest(size);
    SetFilePointer(h, 0, nullptr, FILE_BEGIN);
    if (size > 0 && CryptCATAdminCalcHashFromFileHandle2(admin, h, &size, digest.data(), 0)) {
        if (HCATINFO info = CryptCATAdminEnumCatalogFromHash(admin, digest.data(), size, 0, nullptr); info != nullptr) {
            CATALOG_INFO cat{sizeof(cat)};
            if (CryptCATCatalogInfoFromContext(info, &cat, 0)) {
                std::wstring tag;
                for (const BYTE b : digest) {
                    tag += std::format(L"{:02X}", b);
                }
                const std::wstring member = file.wstring();
                WINTRUST_CATALOG_INFO ci{sizeof(ci)};
                ci.pcwszCatalogFilePath = cat.wszCatalogFile;
                ci.pcwszMemberFilePath = member.c_str();
                ci.pcwszMemberTag = tag.c_str();
                ci.hMemberFile = h;
                ci.pbCalculatedFileHash = digest.data();
                ci.cbCalculatedFileHash = size;
                ci.hCatAdmin = admin;
                WINTRUST_DATA data{};
                data.dwUnionChoice = WTD_CHOICE_CATALOG;
                data.pCatalog = &ci;
                ok = verify(data, leaf, root);
            }
            CryptCATAdminReleaseCatalogContext(admin, info, 0);
        }
    }
    CryptCATAdminReleaseContext(admin, 0);
    return ok;
}

} // namespace

auto signature(const std::filesystem::path& file) -> Signature {
    Signature s;
    const core::win::FileHandle h{CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                              FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (!h) {
        return s;
    }
    std::string leaf, root;
    const std::wstring path = file.wstring();
    WINTRUST_FILE_INFO fi{sizeof(fi)};
    fi.pcwszFilePath = path.c_str();
    fi.hFile = h.get();
    WINTRUST_DATA data{};
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &fi;
    s.signed_ok = verify(data, leaf, root) || by_catalog(file, h.get(), leaf, root);
    if (s.signed_ok) {
        s.publisher = leaf;
        // Microsoft's own: its leaf says so and the chain ends at one of Microsoft's roots.
        s.microsoft = leaf.starts_with("Microsoft ") && root.starts_with("Microsoft Root Certificate Authority");
    }
    s.sha256 = sha256_of(h.get());
    return s;
}

} // namespace tools::winsys
