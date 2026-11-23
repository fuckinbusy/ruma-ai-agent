#include "core/crash.hpp"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cwchar>

#ifdef _DEBUG
#include <crtdbg.h>
#include <rtcapi.h>
#endif

namespace core::crash {
namespace {

#ifdef _DEBUG
int __cdecl rtc_failed(int, const wchar_t* file, int line, const wchar_t* module, const wchar_t* format, ...) {
    std::fwprintf(stderr, L"[crt] run-time check failed in %ls (%ls:%d): ", module != nullptr ? module : L"?",
                  file != nullptr ? file : L"?", line);
    if (format != nullptr) {
        va_list args;
        va_start(args, format);
        std::vfwprintf(stderr, format, args);
        va_end(args);
    }
    std::fwprintf(stderr, L"\n");
    std::fflush(stderr);
    TerminateProcess(GetCurrentProcess(), kFailedCheck);
    return 0;
}

// After a report (an assert's text is already on stderr): end now, never a dialog.
int __cdecl reported(int kind, char* message, int* result) {
    std::fprintf(stderr, "[crt] %s%s", kind == _CRT_WARN ? "warning: " : "", message != nullptr ? message : "");
    std::fflush(stderr);
    if (kind == _CRT_WARN) {
        *result = 0;
        return TRUE; // handled, go on
    }
    TerminateProcess(GetCurrentProcess(), kFailedCheck);
    return TRUE;
}

int __cdecl reported_wide(int kind, wchar_t* message, int* result) {
    std::fwprintf(stderr, L"[crt] %ls%ls", kind == _CRT_WARN ? L"warning: " : L"", message != nullptr ? message : L"");
    std::fflush(stderr);
    if (kind == _CRT_WARN) {
        *result = 0;
        return TRUE;
    }
    TerminateProcess(GetCurrentProcess(), kFailedCheck);
    return TRUE;
}
#endif

} // namespace

void quiet_failures() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#ifdef _DEBUG
    _CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, &reported);
    _CrtSetReportHookW2(_CRT_RPTHOOK_INSTALL, &reported_wide);
    _RTC_SetErrorFuncW(&rtc_failed);
#endif
}

} // namespace core::crash
