#include "tools/winsys/tasks.hpp"

#include "core/utf8.hpp"
#include "core/win.hpp"
#include "tools/winsys/com_thread.hpp"
#include "tools/winsys/startup.hpp"

#include <windows.h>
#include <comdef.h>
#include <sddl.h>
#include <taskschd.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <vector>

#pragma comment(lib, "taskschd.lib")
#pragma comment(lib, "comsupp.lib")

namespace tools::winsys {
namespace {

using Microsoft::WRL::ComPtr;

auto utf8(BSTR s) -> std::string {
    return s != nullptr ? core::utf8::to_utf8(std::wstring_view{s, SysStringLen(s)}).value_or("") : std::string{};
}

class Bstr {
public:
    Bstr() = default;
    explicit Bstr(const wchar_t* s) : b_{SysAllocString(s)} {}
    ~Bstr() { SysFreeString(b_); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    auto put() -> BSTR* {
        SysFreeString(b_);
        b_ = nullptr;
        return &b_;
    }
    [[nodiscard]] auto get() const -> BSTR { return b_; }

private:
    BSTR b_ = nullptr;
};

auto trigger_name(TASK_TRIGGER_TYPE2 t) -> const char* {
    switch (t) {
    case TASK_TRIGGER_LOGON: return "at logon";
    case TASK_TRIGGER_BOOT: return "at startup";
    case TASK_TRIGGER_DAILY: return "daily";
    case TASK_TRIGGER_WEEKLY: return "weekly";
    case TASK_TRIGGER_MONTHLY:
    case TASK_TRIGGER_MONTHLYDOW: return "monthly";
    case TASK_TRIGGER_IDLE: return "on idle";
    case TASK_TRIGGER_TIME: return "once";
    case TASK_TRIGGER_EVENT: return "on an event";
    case TASK_TRIGGER_REGISTRATION: return "when registered";
    case TASK_TRIGGER_SESSION_STATE_CHANGE: return "on session change";
    default: return "other";
    }
}

// One task as its definition holds it (get_Definition only).
auto info_of(IRegisteredTask* t) -> TaskInfo {
    TaskInfo out;
    Bstr path;
    if (SUCCEEDED(t->get_Path(path.put()))) {
        out.path = utf8(path.get());
    }
    VARIANT_BOOL enabled = VARIANT_TRUE;
    if (SUCCEEDED(t->get_Enabled(&enabled))) {
        out.enabled = enabled != VARIANT_FALSE;
    }
    DATE last = 0;
    if (SUCCEEDED(t->get_LastRunTime(&last)) && last > 1.0) {
        SYSTEMTIME st{};
        FILETIME ft{};
        if (VariantTimeToSystemTime(last, &st) && SystemTimeToFileTime(&st, &ft)) {
            const auto ticks = (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
            out.last_run = static_cast<std::int64_t>(ticks / 10'000'000ULL) - 11'644'473'600LL;
        }
    }
    ComPtr<ITaskDefinition> def;
    if (FAILED(t->get_Definition(&def))) {
        return out;
    }
    ComPtr<IRegistrationInfo> reg;
    if (SUCCEEDED(def->get_RegistrationInfo(&reg))) {
        Bstr author;
        if (SUCCEEDED(reg->get_Author(author.put()))) {
            out.author = utf8(author.get());
        }
    }
    ComPtr<IPrincipal> principal;
    if (SUCCEEDED(def->get_Principal(&principal))) {
        Bstr user;
        if (SUCCEEDED(principal->get_UserId(user.put()))) {
            out.account = utf8(user.get());
        }
    }
    ComPtr<IActionCollection> actions;
    long count = 0;
    if (SUCCEEDED(def->get_Actions(&actions)) && SUCCEEDED(actions->get_Count(&count))) {
        for (long i = 1; i <= count && out.command.empty(); ++i) {
            ComPtr<IAction> a;
            TASK_ACTION_TYPE type{};
            if (FAILED(actions->get_Item(i, &a)) || FAILED(a->get_Type(&type)) || type != TASK_ACTION_EXEC) {
                continue;
            }
            ComPtr<IExecAction> exec;
            if (SUCCEEDED(a.As(&exec))) {
                Bstr p, args;
                exec->get_Path(p.put());
                exec->get_Arguments(args.put());
                std::wstring cmd = p.get() != nullptr ? std::wstring{p.get()} : L"";
                if (!cmd.empty() && cmd.front() != L'"' && cmd.find(L' ') != std::wstring::npos) {
                    cmd = L"\"" + cmd + L"\"";
                }
                if (args.get() != nullptr && SysStringLen(args.get()) > 0) {
                    cmd += L" " + std::wstring{args.get()};
                }
                out.command = cmd;
                out.file = file_of(cmd);
            }
        }
    }
    ComPtr<ITriggerCollection> triggers;
    if (SUCCEEDED(def->get_Triggers(&triggers)) && SUCCEEDED(triggers->get_Count(&count))) {
        for (long i = 1; i <= count; ++i) {
            ComPtr<ITrigger> tr;
            TASK_TRIGGER_TYPE2 type{};
            if (SUCCEEDED(triggers->get_Item(i, &tr)) && SUCCEEDED(tr->get_Type(&type))) {
                out.triggers += (out.triggers.empty() ? "" : ", ") + std::string{trigger_name(type)};
                out.at_logon_or_startup = out.at_logon_or_startup || type == TASK_TRIGGER_LOGON || type == TASK_TRIGGER_BOOT;
            }
        }
    }
    return out;
}

void walk(ITaskFolder* folder, TaskList& out) {
    ComPtr<IRegisteredTaskCollection> list;
    if (SUCCEEDED(folder->GetTasks(TASK_ENUM_HIDDEN, &list))) {
        long count = 0;
        list->get_Count(&count);
        for (long i = 1; i <= count; ++i) {
            ComPtr<IRegisteredTask> t;
            if (SUCCEEDED(list->get_Item(_variant_t(i), &t))) {
                out.tasks.push_back(info_of(t.Get()));
            }
        }
    } else {
        Bstr path;
        folder->get_Path(path.put());
        out.not_read.push_back(utf8(path.get()));
    }
    ComPtr<ITaskFolderCollection> subs;
    if (SUCCEEDED(folder->GetFolders(0, &subs))) {
        long count = 0;
        subs->get_Count(&count);
        for (long i = 1; i <= count; ++i) {
            ComPtr<ITaskFolder> sub;
            if (SUCCEEDED(subs->get_Item(_variant_t(i), &sub))) {
                walk(sub.Get(), out);
            }
        }
    }
}

} // namespace

auto tasks() -> TaskList {
    return on_own_mta([] {
        TaskList out;
        ComPtr<ITaskService> service;
        if (SUCCEEDED(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&service))) &&
            SUCCEEDED(service->Connect(_variant_t(), _variant_t(), _variant_t(), _variant_t()))) {
            ComPtr<ITaskFolder> root;
            const Bstr slash{L"\\"};
            if (SUCCEEDED(service->GetFolder(slash.get(), &root))) {
                walk(root.Get(), out);
            }
        } else {
            out.not_read.emplace_back("Task Scheduler");
        }
        return out;
    });
}

auto own_account(std::string_view account) -> bool {
    const auto wide = core::utf8::to_wide(account).value_or(L"");
    if (wide.empty()) {
        return false;
    }
    const auto same = [](std::wstring_view a, std::wstring_view b) {
        return !a.empty() && CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
    };
    // The Task Scheduler often holds a SID ("S-1-5-21-…"): the user's own, from the token.
    if (wide.starts_with(L"S-1-") || wide.starts_with(L"s-1-")) {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            return false;
        }
        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        std::vector<std::byte> buf(size);
        wchar_t* sid = nullptr;
        const bool read = size > 0 && GetTokenInformation(token, TokenUser, buf.data(), size, &size) &&
                          ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &sid);
        CloseHandle(token);
        if (!read) {
            return false;
        }
        const core::win::LocalPtr<wchar_t*> owned{sid};
        return same(wide, sid);
    }
    std::array<wchar_t, 257> name{};
    DWORD n = static_cast<DWORD>(name.size());
    if (!GetUserNameW(name.data(), &n)) {
        return false;
    }
    const auto slash = wide.find_last_of(L'\\');
    return same(slash == std::wstring::npos ? std::wstring_view{wide} : std::wstring_view{wide}.substr(slash + 1), std::wstring_view{name.data()});
}

} // namespace tools::winsys
