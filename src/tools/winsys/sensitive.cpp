#include "tools/winsys/sensitive.hpp"

#include "core/utf8.hpp"
#include "core/win.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <string_view>

namespace tools::winsys {
namespace {

// Lower case for any script (CharLowerBuffW knows Cyrillic): titles come in the user's language.
auto lower(std::string_view utf8) -> std::wstring {
    auto w = core::utf8::to_wide(utf8).value_or(std::wstring{});
    if (!w.empty()) {
        CharLowerBuffW(w.data(), static_cast<DWORD>(w.size()));
    }
    return w;
}

constexpr std::array kPasswordManagers{L"keepass.exe",  L"keepassxc.exe", L"bitwarden.exe", L"1password.exe", L"lastpass.exe", L"dashlane.exe",
                                       L"enpass.exe",   L"protonpass.exe", L"proton pass.exe", L"nordpass.exe", L"roboform.exe", L"keeweb.exe"};
constexpr std::array kSignIn{L"credentialuibroker.exe", L"consent.exe", L"logonui.exe"};
constexpr std::array kPrivate{L"inprivate", L"incognito", L"private browsing", L"инкогнито", L"приватный просмотр", L"приватное окно"};
constexpr std::array kBank{L"bank", L"банк"};

template <class List>
auto any_in(const std::wstring& text, const List& words) -> bool {
    return std::ranges::any_of(words, [&](const wchar_t* w) { return text.find(w) != std::wstring::npos; });
}

} // namespace

auto facts_of(HWND w) -> WindowFacts {
    WindowFacts f;
    wchar_t title[512]{};
    GetWindowTextW(w, title, 512);
    f.title = core::utf8::to_utf8(title).value_or("");
    DWORD pid = 0;
    const DWORD thread = GetWindowThreadProcessId(w, &pid);
    if (const core::win::KernelHandle p{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)}) {
        wchar_t image[MAX_PATH * 2]{};
        DWORD size = MAX_PATH * 2;
        if (QueryFullProcessImageNameW(p.get(), 0, image, &size)) {
            f.program = core::utf8::to_utf8(std::filesystem::path{std::wstring_view{image, size}}.filename().wstring()).value_or("");
        }
    }
    DWORD affinity = WDA_NONE;
    f.capture_excluded = GetWindowDisplayAffinity(w, &affinity) && affinity != WDA_NONE;
    // The window's thread's focus: a classic password field (a browser's is seen by its title only).
    GUITHREADINFO gui{sizeof(gui)};
    if (GetGUIThreadInfo(thread, &gui) && gui.hwndFocus != nullptr && (gui.hwndFocus == w || IsChild(w, gui.hwndFocus))) {
        wchar_t cls[32]{};
        GetClassNameW(gui.hwndFocus, cls, 32);
        f.password_focused = _wcsicmp(cls, L"Edit") == 0 && (GetWindowLongW(gui.hwndFocus, GWL_STYLE) & ES_PASSWORD) != 0;
    }
    return f;
}

auto sensitivity_of(const WindowFacts& facts, std::span<const PrivateRule> rules) -> Sensitivity {
    const auto program = lower(facts.program);
    const auto title = lower(facts.title);
    if (std::ranges::any_of(kPasswordManagers, [&](const wchar_t* p) { return program == p; })) {
        return {true, "it is a password manager"};
    }
    if (std::ranges::any_of(kSignIn, [&](const wchar_t* p) { return program == p; })) {
        return {true, "it is Windows' sign-in or permission window"};
    }
    if (any_in(title, kPrivate)) {
        return {true, "it is a private browser window"};
    }
    if (facts.capture_excluded) {
        return {true, "it keeps itself out of screen captures"};
    }
    if (facts.password_focused) {
        return {true, "a password field in it is in focus"};
    }
    if (any_in(title, kBank)) {
        return {true, "its title looks like a bank's"};
    }
    for (const auto& r : rules) {
        if (r.program.empty() && r.words.empty()) {
            continue;
        }
        const bool by_program = r.program.empty() || lower(r.program) == program;
        const bool by_words = r.words.empty() || title.find(lower(r.words)) != std::wstring::npos;
        if (by_program && by_words) {
            return {true, "it is on your private windows list"};
        }
    }
    return {};
}

} // namespace tools::winsys
