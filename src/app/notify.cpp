#include "app/notify.hpp"

#include "core/agent_ids.hpp"
#include "core/log.hpp"
#include "core/utf8.hpp"
#include "core/win.hpp"

#include <windows.h>
#include <NotificationActivationCallback.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shlobj.h>
#include <shobjidl_core.h>
#include <wrl/implements.h>
#include <wrl/module.h>

#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Notifications.h>

#include <algorithm>
#include <charconv>
#include <format>
#include <mutex>

#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "propsys.lib")

namespace app::notify {
namespace {

constexpr wchar_t kAumid[] = L"WinAiAgent.Agent";
// The agent's notification activator (its own GUID; the spike's was another).
constexpr wchar_t kClsidKey[] = L"Software\\Classes\\CLSID\\{3F6B2C1D-8E4A-4B7F-A2D9-5C1E7B3A9D64}\\LocalServer32";
constexpr GUID kClsid{0x3f6b2c1d, 0x8e4a, 0x4b7f, {0xa2, 0xd9, 0x5c, 0x1e, 0x7b, 0x3a, 0x9d, 0x64}};
// The installer registers and removes the same names (core::agent_ids).
static_assert(std::wstring_view{kAumid} == core::agent_ids::kAumid);
static_assert(std::wstring_view{kClsidKey} ==
              L"Software\\Classes\\CLSID\\{3F6B2C1D-8E4A-4B7F-A2D9-5C1E7B3A9D64}\\LocalServer32");
static_assert(core::agent_ids::kActivatorClsid == L"{3F6B2C1D-8E4A-4B7F-A2D9-5C1E7B3A9D64}");

std::mutex g_mutex;
std::function<void(std::wstring)> g_clicked; // under g_mutex

// Where a button's click arrives (a COM call from Windows, on one of its threads).
class __declspec(uuid("3F6B2C1D-8E4A-4B7F-A2D9-5C1E7B3A9D64")) Activator
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                          INotificationActivationCallback> {
public:
    IFACEMETHODIMP Activate(LPCWSTR, LPCWSTR invoked_args, const NOTIFICATION_USER_INPUT_DATA*, ULONG) override {
        std::scoped_lock lock{g_mutex};
        if (g_clicked) {
            g_clicked(invoked_args != nullptr ? std::wstring{invoked_args} : std::wstring{});
        }
        return S_OK;
    }
};
CoCreatableClass(Activator);

auto escape(std::string_view s) -> std::string {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default: out += c;
        }
    }
    return out;
}

auto exe_path() -> std::wstring {
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return exe;
}

auto installed() -> bool {
    return shortcut_name(std::filesystem::path{exe_path()}.parent_path()) == core::agent_ids::kShortcut;
}

// %APPDATA%\Microsoft\Windows\Start Menu\Programs\Agent.lnk (WinAiAgent.lnk when installed)
auto shortcut_path() -> std::wstring {
    wchar_t programs[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PROGRAMS, nullptr, SHGFP_TYPE_CURRENT, programs))) {
        return {};
    }
    return std::wstring{programs} + L"\\" + std::wstring{shortcut_name(std::filesystem::path{exe_path()}.parent_path())};
}

// The shortcut with the app's id and its activator: how Windows knows an unpackaged app's
// notifications (the spike's third attempt). An installed agent keeps the installer's one.
auto make_shortcut() -> bool {
    using Microsoft::WRL::ComPtr;
    const auto path = shortcut_path();
    if (path.empty()) {
        return false;
    }
    if (installed() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return true;
    }
    ComPtr<IShellLinkW> link;
    ComPtr<IPropertyStore> props;
    ComPtr<IPersistFile> file;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) ||
        FAILED(link->SetPath(exe_path().c_str())) || FAILED(link.As(&props)) || FAILED(link.As(&file))) {
        return false;
    }
    PROPVARIANT id{};
    PROPVARIANT clsid{};
    if (FAILED(InitPropVariantFromString(kAumid, &id)) || FAILED(InitPropVariantFromCLSID(kClsid, &clsid))) {
        return false;
    }
    const bool ok = SUCCEEDED(props->SetValue(PKEY_AppUserModel_ID, id)) &&
                    SUCCEEDED(props->SetValue(PKEY_AppUserModel_ToastActivatorCLSID, clsid)) && SUCCEEDED(props->Commit()) &&
                    SUCCEEDED(file->Save(path.c_str(), TRUE));
    PropVariantClear(&id);
    PropVariantClear(&clsid);
    return ok;
}

// Windows starts the agent for a click when it is not running.
auto register_server() -> bool {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kClsidKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const std::wstring value = L"\"" + exe_path() + L"\"";
    const bool ok = RegSetValueExW(key, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                                   static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    RegCloseKey(key);
    return ok;
}

} // namespace

auto shortcut_name(const std::filesystem::path& exe_dir) -> std::wstring_view {
    std::error_code ec;
    return std::filesystem::exists(exe_dir / L"install.json", ec) ? core::agent_ids::kShortcut
                                                                   : core::agent_ids::kBuildShortcut;
}

auto arguments(std::uint64_t task, std::string_view action) -> std::string {
    return std::format("drop:{}:{}", task, action);
}

auto press_of(std::string_view args) -> std::optional<std::pair<std::uint64_t, std::string>> {
    if (!args.starts_with("drop:")) {
        return std::nullopt;
    }
    args.remove_prefix(5);
    const auto colon = args.find(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 >= args.size()) {
        return std::nullopt;
    }
    std::uint64_t task = 0;
    const auto [end, ec] = std::from_chars(args.data(), args.data() + colon, task);
    if (ec != std::errc{} || end != args.data() + colon) {
        return std::nullopt;
    }
    return std::pair{task, std::string{args.substr(colon + 1)}};
}

auto note_of(const sheet::Drop& d) -> Note {
    Note n{.id = d.task, .title = d.title, .text = d.text, .buttons = {}};
    // A threat check's (phase 13a, 07b): named as such, its verdict in words, Open result.
    if (d.answer && (d.title == "Check done" || d.title == "Check stopped")) {
        n.title = d.title == "Check done" ? "Threat check done" : "Threat check stopped";
        n.text = d.more.empty() ? d.text : d.more;
        n.buttons.push_back({"Open result", arguments(d.task, "review")});
        return n;
    }
    if (d.kind == sheet::DropKind::Question) {
        n.buttons.push_back({"Review", arguments(d.task, "review")}); // the card answers, never the notification
    } else if (d.kind == sheet::DropKind::Result) {
        for (const auto& a : d.actions) {
            n.buttons.push_back({a.label, arguments(d.task, a.id)});
        }
    }
    return n;
}

auto toast_xml(const Note& n) -> std::string {
    std::string xml = std::format("<toast launch='{}'><visual><binding template='ToastGeneric'><text>{}</text><text>{}</text>"
                                  "</binding></visual>",
                                  escape(arguments(n.id, "review")), escape(n.title), escape(n.text));
    if (!n.buttons.empty()) {
        xml += "<actions>";
        for (const auto& b : n.buttons) {
            xml += std::format("<action content='{}' arguments='{}'/>", escape(b.label), escape(b.arguments));
        }
        xml += "</actions>";
    }
    return xml + "</toast>";
}

void Courier::update(const sheet::Model& m, bool fullscreen, Notifier& n) {
    // Gone cards first: their notifications go too.
    for (auto it = shown_.begin(); it != shown_.end();) {
        if (std::ranges::none_of(m.drops, [&](const sheet::Drop& d) { return d.task == it->first && d.made == it->second; })) {
            n.hide(it->first);
            it = shown_.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto& d : m.drops) {
        // A background command's end goes at once (phase 12 § 8); the rest only over a fullscreen app.
        if ((fullscreen || d.notify) && std::ranges::find(shown_, std::pair{d.task, d.made}) == shown_.end()) {
            n.show(note_of(d));
            shown_.emplace_back(d.task, d.made);
        }
    }
}

WindowsNotifier::WindowsNotifier(std::function<void(std::wstring)> clicked) {
    {
        std::scoped_lock lock{g_mutex};
        g_clicked = std::move(clicked);
    }
    // Made before (an earlier run showed a notification): the activator is up from the start, so
    // a click that started this agent reaches it.
    if (const auto lnk = shortcut_path(); !lnk.empty() && GetFileAttributesW(lnk.c_str()) != INVALID_FILE_ATTRIBUTES) {
        (void)ready();
    }
}

WindowsNotifier::~WindowsNotifier() {
    if (ready_) {
        Microsoft::WRL::Module<Microsoft::WRL::OutOfProc>::GetModule().UnregisterObjects();
    }
    std::scoped_lock lock{g_mutex};
    g_clicked = nullptr;
}

auto WindowsNotifier::ready() -> bool {
    if (tried_) {
        return ready_;
    }
    tried_ = true;
    if (!make_shortcut() || !register_server()) {
        WA_WARN("notify", "the Start menu shortcut or the activator's key could not be written: no notifications");
        return false;
    }
    SetCurrentProcessExplicitAppUserModelID(kAumid);
    auto& module = Microsoft::WRL::Module<Microsoft::WRL::OutOfProc>::Create();
    if (FAILED(module.RegisterObjects())) {
        WA_WARN("notify", "the activator is not registered: a notification's buttons do nothing");
    }
    ready_ = true;
    WA_INFO("notify", "notifications ready (Agent.lnk in the Start menu)");
    return true;
}

void WindowsNotifier::show(const Note& note) {
    if (!ready()) {
        return;
    }
    namespace N = winrt::Windows::UI::Notifications;
    core::win::keep_mta(); // before WinRT: its factories outlive a borrowed MTA (core/win.hpp)
    try {
        winrt::Windows::Data::Xml::Dom::XmlDocument xml;
        xml.LoadXml(winrt::to_hstring(toast_xml(note)));
        N::ToastNotification toast{xml};
        toast.Tag(winrt::to_hstring(std::to_string(note.id)));
        N::ToastNotificationManager::CreateToastNotifier(kAumid).Show(toast);
    } catch (const winrt::hresult_error& e) {
        WA_WARN("notify", "a notification was not shown: 0x{:08x}", static_cast<unsigned>(e.code()));
    }
}

void WindowsNotifier::hide(std::uint64_t id) {
    if (!ready_) {
        return;
    }
    core::win::keep_mta(); // before WinRT: its factories outlive a borrowed MTA (core/win.hpp)
    try {
        winrt::Windows::UI::Notifications::ToastNotificationManager::History().Remove(winrt::to_hstring(std::to_string(id)), L"",
                                                                                      kAumid);
    } catch (const winrt::hresult_error&) {
        // gone already
    }
}

} // namespace app::notify
