#include "security/paths.hpp"

#include "core/win.hpp"

#include <windows.h>

#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <optional>

namespace security::paths {
namespace {

auto lower(std::wstring_view s) -> std::wstring {
    std::wstring out{s};
    std::ranges::transform(out, out.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return out;
}

auto equal_nocase(std::wstring_view a, std::wstring_view b) -> bool {
    return a.size() == b.size() &&
           CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) ==
               CSTR_EQUAL;
}

auto starts_with_nocase(std::wstring_view s, std::wstring_view prefix) -> bool {
    return s.size() >= prefix.size() && equal_nocase(s.substr(0, prefix.size()), prefix);
}

auto invalid(std::string message) -> std::unexpected<core::Error> {
    return core::make_error(core::Code::InvalidArgument, std::move(message));
}

// CON, PRN, AUX, NUL, COM0-9, LPT0-9, CONIN$, CONOUT$ — with any extension, in any folder.
auto is_device_name(std::wstring_view component) -> bool {
    std::wstring base = lower(component.substr(0, component.find(L'.')));
    while (!base.empty() && (base.back() == L' ' || base.back() == L'.')) {
        base.pop_back();
    }
    if (base == L"con" || base == L"prn" || base == L"aux" || base == L"nul" || base == L"conin$" ||
        base == L"conout$") {
        return true;
    }
    return base.size() == 4 && (base.starts_with(L"com") || base.starts_with(L"lpt")) && base[3] >= L'0' &&
           base[3] <= L'9';
}

auto is_drive(std::wstring_view s) -> bool {
    return s.size() >= 2 && std::iswalpha(s[0]) && s[1] == L':';
}

auto computer_names() -> const std::vector<std::wstring>& {
    static const std::vector<std::wstring> names = [] {
        std::vector<std::wstring> out{L"localhost", L"127.0.0.1", L"::1", L"[::1]", L".",
                                      L"0--1.ipv6-literal.net"};
        for (const auto kind : {ComputerNameNetBIOS, ComputerNameDnsHostname, ComputerNameDnsFullyQualified}) {
            wchar_t buf[256]{};
            DWORD n = static_cast<DWORD>(std::size(buf));
            if (GetComputerNameExW(kind, buf, &n) && n > 0) {
                out.emplace_back(buf, n);
            }
        }
        return out;
    }();
    return names;
}

// "\\?\C:\x" -> "C:\x"; "\\?\UNC\s\h\x" -> "\\s\h\x".
auto from_final(std::wstring_view f) -> std::wstring {
    if (starts_with_nocase(f, L"\\\\?\\UNC\\")) {
        return L"\\\\" + std::wstring{f.substr(8)};
    }
    if (starts_with_nocase(f, L"\\\\?\\")) {
        return std::wstring{f.substr(4)};
    }
    return std::wstring{f};
}

// The final path the file system gives for an existing object, or nullopt.
auto final_path(const std::wstring& path, bool& no_dos_name, bool& unresolved_link) -> std::optional<std::wstring> {
    no_dos_name = false;
    unresolved_link = false;
    const std::wstring long_path = long_form(path);
    HANDLE h = CreateFileW(long_path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        // A link that cannot be followed (to nowhere, or somewhere this user cannot open): where
        // it leads is unknown, and a write through it would go there — never placed by its name.
        const DWORD attrs = GetFileAttributesW(long_path.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            unresolved_link = true;
            return std::nullopt;
        }
        // In use or locked (pagefile.sys, System Volume Information): the long name at least.
        wchar_t buf[32768];
        const DWORD n = GetLongPathNameW(long_path.c_str(), buf, static_cast<DWORD>(std::size(buf)));
        if (n > 0 && n < std::size(buf)) {
            return from_final({buf, n});
        }
        return std::nullopt;
    }
    std::wstring out(32768, L'\0');
    const DWORD n = GetFinalPathNameByHandleW(h, out.data(), static_cast<DWORD>(out.size()),
                                              FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);
    if (n == 0 || n >= out.size()) {
        no_dos_name = true; // e.g. a volume without a drive letter behind a junction
        return std::nullopt;
    }
    out.resize(n);
    return from_final(out);
}

auto exists(const std::wstring& path) -> bool {
    const std::wstring long_path = long_form(path);
    return GetFileAttributesW(long_path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

auto join(const std::wstring& prefix, const std::vector<std::wstring>& parts, std::size_t count) -> std::wstring {
    std::wstring out = prefix;
    for (std::size_t i = 0; i < count; ++i) {
        out += L'\\';
        out += parts[i];
    }
    if (count == 0 && is_drive(prefix)) {
        out += L'\\'; // "C:" alone is drive-relative: the root is "C:\"
    }
    return out;
}

auto known(REFKNOWNFOLDERID id) -> std::wstring {
    auto p = core::win::known_folder(id);
    return p ? p->wstring() : std::wstring{};
}

auto file_name(std::wstring_view p) -> std::wstring_view {
    const auto slash = p.find_last_of(L'\\');
    return slash == std::wstring_view::npos ? p : p.substr(slash + 1);
}

auto parent(std::wstring_view p) -> std::wstring_view {
    const auto slash = p.find_last_of(L'\\');
    return slash == std::wstring_view::npos ? std::wstring_view{} : p.substr(0, slash);
}

} // namespace

auto zone_name(Zone z) -> std::string_view {
    switch (z) {
    case Zone::System: return "System";
    case Zone::Secrets: return "Secrets";
    case Zone::AppSelf: return "AppSelf";
    case Zone::User: return "User";
    case Zone::Other: return "Other";
    }
    return "?";
}

auto long_form(std::wstring_view path) -> std::wstring {
    if (path.starts_with(L"\\\\?\\")) {
        return std::wstring{path};
    }
    if (path.starts_with(L"\\\\")) {
        return L"\\\\?\\UNC\\" + std::wstring{path.substr(2)};
    }
    return L"\\\\?\\" + std::wstring{path};
}

auto Canonical::long_form() const -> std::wstring {
    return paths::long_form(path);
}

auto is_within(std::wstring_view path, std::wstring_view root) -> bool {
    while (!root.empty() && root.back() == L'\\') {
        root.remove_suffix(1);
    }
    if (root.empty() || !starts_with_nocase(path, root)) {
        return false;
    }
    return path.size() == root.size() || path[root.size()] == L'\\';
}

auto canonical(std::wstring_view input, const std::filesystem::path& working_dir) -> core::Result<Canonical> {
    if (input.empty() || input.find(L'\0') != std::wstring_view::npos) {
        return invalid("empty path or a NUL inside");
    }
    Canonical c;
    std::wstring s{input};
    std::ranges::replace(s, L'/', L'\\');

    // Namespace prefixes.
    for (const std::wstring_view nt : {std::wstring_view{L"\\\\?\\"}, std::wstring_view{L"\\??\\"}}) {
        if (starts_with_nocase(s, nt)) {
            const std::wstring rest = s.substr(nt.size());
            if (starts_with_nocase(rest, L"UNC\\")) {
                s = L"\\\\" + rest.substr(4);
            } else if (is_drive(rest)) {
                s = rest;
            } else { // GLOBALROOT, Volume{…}, devices
                c.path = s;
                c.device = true;
                return c;
            }
            break;
        }
    }
    if (starts_with_nocase(s, L"\\\\.\\")) {
        const std::wstring rest = s.substr(4);
        if (is_drive(rest)) {
            s = rest;
        } else { // \\.\PhysicalDrive0, \\.\pipe\x
            c.path = s;
            c.device = true;
            return c;
        }
    }

    // The prefix: "C:" or "\\server\share"; the rest as components.
    std::wstring prefix;
    std::wstring rest;
    if (s.starts_with(L"\\\\")) {
        const auto server_end = s.find(L'\\', 2);
        const std::wstring server = s.substr(2, server_end == std::wstring::npos ? std::wstring::npos : server_end - 2);
        if (server.empty() || server_end == std::wstring::npos) {
            return invalid("a UNC path needs \\\\server\\share");
        }
        const auto share_end = s.find(L'\\', server_end + 1);
        const std::wstring share =
            s.substr(server_end + 1, share_end == std::wstring::npos ? std::wstring::npos : share_end - server_end - 1);
        if (share.empty()) {
            return invalid("a UNC path needs \\\\server\\share");
        }
        rest = share_end == std::wstring::npos ? std::wstring{} : s.substr(share_end);
        const bool loopback = std::ranges::any_of(computer_names(), [&](const std::wstring& n) { return equal_nocase(n, server); });
        if (loopback && share.size() == 2 && std::iswalpha(share[0]) && share[1] == L'$') {
            prefix = std::wstring{static_cast<wchar_t>(std::towupper(share[0]))} + L":"; // \\localhost\C$ is C:
        } else {
            prefix = L"\\\\" + server + L"\\" + share;
            c.unc = true;
        }
    } else if (is_drive(s)) {
        if (s.size() == 2 || s[2] != L'\\') {
            return invalid("drive-relative paths (C:name) are not accepted");
        }
        prefix = std::wstring{static_cast<wchar_t>(std::towupper(s[0]))} + L":";
        rest = s.substr(2);
    } else {
        std::wstring base = working_dir.wstring();
        std::ranges::replace(base, L'/', L'\\');
        if (base.empty() || !is_drive(base)) {
            return invalid("the working folder must be an absolute path on a drive");
        }
        prefix = std::wstring{static_cast<wchar_t>(std::towupper(base[0]))} + L":";
        rest = s.starts_with(L'\\') ? s : base.substr(2) + L"\\" + s; // "\x" is the drive's root
    }

    // Components, collapsed before the file system is asked.
    std::vector<std::wstring> parts;
    std::size_t pos = 0;
    while (pos <= rest.size()) {
        const auto next = rest.find(L'\\', pos);
        std::wstring part = rest.substr(pos, next == std::wstring::npos ? std::wstring::npos : next - pos);
        pos = next == std::wstring::npos ? rest.size() + 1 : next + 1;
        if (part.empty() || part == L".") {
            continue;
        }
        if (part == L"..") {
            if (!parts.empty()) {
                parts.pop_back();
            }
            continue;
        }
        if (const auto colon = part.find(L':'); colon != std::wstring::npos) {
            c.ads = true; // file:stream or file::$DATA
            part.resize(colon);
        }
        while (!part.empty() && (part.back() == L'.' || part.back() == L' ')) {
            part.pop_back(); // Windows drops trailing dots and spaces
        }
        if (part.empty()) {
            continue;
        }
        if (is_device_name(part)) {
            c.device = true;
        }
        parts.push_back(std::move(part));
    }
    c.logical = join(prefix, parts, parts.size());
    if (c.device) {
        c.path = c.logical;
        return c;
    }

    if (is_drive(prefix)) {
        const wchar_t root[] = {prefix[0], L':', L'\\', 0};
        const UINT type = GetDriveTypeW(root);
        c.remote = type == DRIVE_REMOTE;
        c.removable = type == DRIVE_REMOVABLE || type == DRIVE_CDROM;
    }

    // The longest existing part, resolved by the file system; the rest appended.
    std::size_t have = parts.size();
    while (have > 0 && !exists(join(prefix, parts, have))) {
        --have;
    }
    c.exists = have == parts.size() && exists(join(prefix, parts, have));
    bool no_dos_name = false;
    bool unresolved_link = false;
    std::wstring resolved;
    if (auto f = final_path(join(prefix, parts, have), no_dos_name, unresolved_link)) {
        resolved = std::move(*f);
    } else if (unresolved_link) {
        c.path = join(prefix, parts, parts.size());
        c.device = true; // where it leads is unknown: neither readable nor writable
        return c;
    } else if (no_dos_name) {
        c.path = join(prefix, parts, parts.size());
        c.device = true; // behind it: a volume without a drive letter
        return c;
    } else {
        resolved = join(prefix, parts, have);
    }
    while (resolved.size() > 3 && resolved.back() == L'\\') {
        resolved.pop_back();
    }
    if (resolved.starts_with(L"\\\\")) {
        c.unc = true; // a mapped drive or a junction to a share
    }
    for (std::size_t i = have; i < parts.size(); ++i) {
        if (!resolved.ends_with(L'\\')) {
            resolved += L'\\';
        }
        resolved += parts[i];
    }
    c.path = std::move(resolved);
    return c;
}

auto make_roots() -> Roots {
    return make_roots(core::win::module_path());
}

auto make_roots(const std::filesystem::path& exe) -> Roots {
    Roots r;
    r.system_root = known(FOLDERID_Windows);
    r.system_drive = r.system_root.substr(0, 2);
    for (const auto& id : {FOLDERID_ProgramFiles, FOLDERID_ProgramFilesX86, FOLDERID_ProgramFilesX64}) {
        if (auto p = known(id); !p.empty() && std::ranges::find(r.program_files, p) == r.program_files.end()) {
            r.program_files.push_back(p);
        }
    }
    r.program_data = known(FOLDERID_ProgramData);
    r.profile = known(FOLDERID_Profile);
    r.users = known(FOLDERID_UserProfiles);
    r.local_app_data = known(FOLDERID_LocalAppData);
    r.roaming_app_data = known(FOLDERID_RoamingAppData);
    r.agent_dirs.push_back(r.local_app_data + L"\\Agent");
    if (auto beside = core::win::data_dir_beside(exe)) { // a portable build: its data beside it
        r.agent_dirs.push_back(beside->wstring());
    }
    for (const wchar_t* dir : {L".ssh", L".gnupg", L".aws", L".azure", L".kube"}) {
        r.secret_dirs.push_back(r.profile + L"\\" + dir);
    }
    for (const wchar_t* dir : {L"Microsoft\\Credentials", L"Microsoft\\Protect", L"Microsoft\\Vault",
                               L"Mozilla\\Firefox\\Profiles", L"Opera Software"}) {
        r.secret_dirs.push_back(r.roaming_app_data + L"\\" + dir);
    }
    for (const wchar_t* dir : {L"Microsoft\\Credentials", L"Microsoft\\Vault", L"Google\\Chrome\\User Data",
                               L"Microsoft\\Edge\\User Data", L"BraveSoftware\\Brave-Browser\\User Data"}) {
        r.secret_dirs.push_back(r.local_app_data + L"\\" + dir);
    }

    resolve(r);
    return r;
}

void resolve(Roots& r) {
    const auto one = [](std::wstring& root) {
        if (root.size() < 3) {
            return;
        }
        if (auto c = canonical(root, std::filesystem::path{root.substr(0, 3)})) {
            root = c->path;
        }
    };
    one(r.system_root);
    for (auto& p : r.program_files) {
        one(p);
    }
    one(r.program_data);
    one(r.profile);
    one(r.users);
    one(r.local_app_data);
    one(r.roaming_app_data);
    for (auto& p : r.agent_dirs) {
        one(p);
    }
    for (auto& p : r.secret_dirs) {
        one(p);
    }
}

auto system_roots() -> const Roots& {
    static const Roots roots = make_roots();
    return roots;
}

namespace {

auto zone_of_path(std::wstring_view p, const Canonical& c, const Roots& r) -> Zone;

// See paths.hpp.
auto stricter_of(Zone a, Zone b) -> Zone {
    const auto rank = [](Zone z) {
        switch (z) {
        case Zone::Secrets: return 5;
        case Zone::System: return 4;
        case Zone::AppSelf: return 3;
        case Zone::Other: return 2;
        case Zone::User: return 1;
        }
        return 5;
    };
    return rank(a) >= rank(b) ? a : b;
}

} // namespace

auto stricter(Zone a, Zone b) -> Zone {
    return stricter_of(a, b);
}

auto zone_of(const Canonical& c, const Roots& r) -> Zone {
    if (c.device) {
        return Zone::Secrets; // raw disks, pipes, kernel paths: neither readable nor writable
    }
    const Zone resolved = zone_of_path(c.path, c, r);
    return c.logical.empty() ? resolved : stricter_of(resolved, zone_of_path(c.logical, c, r));
}

namespace {

auto zone_of_path(std::wstring_view p, const Canonical& c, const Roots& r) -> Zone {
    const std::wstring name = lower(file_name(p));
    const bool in_agent = std::ranges::any_of(r.agent_dirs, [&](const std::wstring& d) { return is_within(p, d); });

    // Secrets first: stricter than System for reading.
    if (std::ranges::any_of(r.secret_dirs, [&](const std::wstring& d) { return is_within(p, d); }) ||
        is_within(p, r.system_root + L"\\System32\\config") || // registry hives
        name.ends_with(L".pfx") || name.ends_with(L".p12") || name.ends_with(L".pem") || name.ends_with(L".key") ||
        name == L".env" || name.starts_with(L".env.") ||
        (in_agent && name == L"secrets.bin")) {
        return Zone::Secrets;
    }
    if (in_agent) {
        return Zone::AppSelf;
    }
    // \\host\C$, \\host\ADMIN$: a machine's own drives and Windows folder, this one's or another's.
    if (p.starts_with(L"\\\\")) {
        const auto share_start = p.find(L'\\', 2);
        if (share_start != std::wstring_view::npos) {
            const auto share_end = p.find(L'\\', share_start + 1);
            const auto share = p.substr(share_start + 1, share_end == std::wstring_view::npos ? std::wstring_view::npos
                                                                                               : share_end - share_start - 1);
            if (share.size() >= 2 && share.back() == L'$') {
                return Zone::System;
            }
        }
    }
    if (c.unc || c.remote || c.removable) {
        return Zone::Other;
    }
    if (is_within(p, r.system_root) ||
        std::ranges::any_of(r.program_files, [&](const std::wstring& d) { return is_within(p, d); }) ||
        is_within(p, r.program_data + L"\\Microsoft")) {
        return Zone::System;
    }
    // Any drive: the recycle bin, the volume's own folder, programs installed there.
    for (const wchar_t* sys : {L"$recycle.bin", L"system volume information", L"program files", L"program files (x86)"}) {
        const std::wstring lp = lower(p);
        if (lp.size() >= 3 && (lp.substr(2) == std::wstring{L"\\"} + sys || lp.substr(2).starts_with(std::wstring{L"\\"} + sys + L"\\"))) {
            return Zone::System;
        }
    }
    if (is_drive(p) && equal_nocase(p.substr(0, 2), r.system_drive)) {
        // The system drive's root and what sits directly in it, except folders of the user's own.
        if (p.size() <= 3) {
            return Zone::System;
        }
        const std::wstring top = lower(p.substr(3, p.find(L'\\', 3) == std::wstring_view::npos ? std::wstring_view::npos : p.find(L'\\', 3) - 3));
        static constexpr std::array<std::wstring_view, 12> kRootSystem{
            L"recovery", L"perflogs", L"boot", L"efi", L"config.msi", L"$winreagent", L"$sysreset",
            L"$windows.~bt", L"$windows.~ws", L"windows.old", L"documents and settings", L"msocache"};
        if (std::ranges::find(kRootSystem, top) != kRootSystem.end()) {
            return Zone::System;
        }
        if (parent(p).size() <= 2) { // directly in the root
            const DWORD attrs = GetFileAttributesW((L"\\\\?\\" + std::wstring{p}).c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                return Zone::System; // files in the root (pagefile.sys, bootmgr, …), or not there yet
            }
        }
    }
    if (is_within(p, r.program_data)) {
        return Zone::Other; // other programs' shared data
    }
    if (is_within(p, r.profile) || is_within(p, r.users + L"\\Public")) {
        return Zone::User;
    }
    if (is_within(p, r.users)) {
        return Zone::Other; // another user's profile
    }
    if (is_drive(p)) {
        return Zone::User; // the user's own folders on local fixed drives
    }
    return Zone::Other;
}

} // namespace

} // namespace security::paths
