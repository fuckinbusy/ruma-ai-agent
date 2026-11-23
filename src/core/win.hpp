#pragma once

// Thin Win32 helpers: RAII handle traits and shell folders.

#include "core/error.hpp"
#include "core/mem/handle.hpp"

#include <windows.h>

#include <filesystem>
#include <optional>

namespace core::win {

// Handles whose "none" value is nullptr (events, threads, processes, mutexes).
struct KernelHandleTraits {
    using type = HANDLE;
    static auto invalid() noexcept -> HANDLE { return nullptr; }
    static void close(HANDLE h) noexcept { CloseHandle(h); }
};

// CreateFileW returns INVALID_HANDLE_VALUE on failure, not nullptr.
struct FileHandleTraits {
    using type = HANDLE;
    static auto invalid() noexcept -> HANDLE { return INVALID_HANDLE_VALUE; }
    static void close(HANDLE h) noexcept { CloseHandle(h); }
};

// FindFirstFileExW handles are closed with FindClose.
struct FindHandleTraits {
    using type = HANDLE;
    static auto invalid() noexcept -> HANDLE { return INVALID_HANDLE_VALUE; }
    static void close(HANDLE h) noexcept { FindClose(h); }
};

// What Win32 allocates for the caller with LocalAlloc (SID text, security descriptors,
// SetEntriesInAclW's lists): T is the pointer type the call hands out.
template <class T>
struct LocalTraits {
    using type = T;
    static auto invalid() noexcept -> T { return nullptr; }
    static void close(T p) noexcept { LocalFree(p); } // mem-ok: the API allocated it with LocalAlloc; only LocalFree frees it
};

// What Win32 allocates for the caller with GlobalAlloc (WinHTTP's proxy strings).
template <class T>
struct GlobalTraits {
    using type = T;
    static auto invalid() noexcept -> T { return nullptr; }
    static void close(T p) noexcept { GlobalFree(p); } // mem-ok: the API allocated it with GlobalAlloc; only GlobalFree frees it
};

using KernelHandle = core::Handle<KernelHandleTraits>;
using FileHandle = core::Handle<FileHandleTraits>;
using FindHandle = core::Handle<FindHandleTraits>;
template <class T>
using LocalPtr = core::Handle<LocalTraits<T>>;
template <class T>
using GlobalPtr = core::Handle<GlobalTraits<T>>;

// A shell known folder (FOLDERID_LocalAppData, FOLDERID_Desktop, ...).
auto known_folder(const GUID& id) -> core::Result<std::filesystem::path>;

// This process's own .exe.
auto module_path() -> std::filesystem::path;

// <the program's folder>\data when it is a folder: a portable build keeps the agent's data
// (config.json, logs, history, audit, the key) beside the program. None: %LOCALAPPDATA%\Agent.
auto data_dir_beside(const std::filesystem::path& exe) -> std::optional<std::filesystem::path>;

// The data folder of the agent at `exe`: the installed agent's (its exe under `program_files`)
// is the user's %LOCALAPPDATA%\WinAiAgent\data (Program Files is the administrators'); a build's
// is beside it when it has one, otherwise %LOCALAPPDATA%\Agent.
auto data_dir_for(const std::filesystem::path& exe, const std::filesystem::path& program_files,
                  const std::filesystem::path& local_app_data) -> std::filesystem::path;

// The agent's data folder: beside this program when it has one, otherwise %LOCALAPPDATA%\Agent
// (%TEMP%\Agent when even that is unknown).
auto agent_data_dir() -> std::filesystem::path;

// The process's multithreaded apartment, kept from the first call to the end of the process.
// Called before any WinRT use: C++/WinRT keeps every factory it gets for the whole process, but
// keeps the MTA itself only when it finds none. Run in an MTA another thread held (the shell's
// IFileOperation thread holds one for about 30 s), the factories outlived it — COM unloaded their
// DLLs when it ended, and the next call through one crashed (2026-11-22, doc.read's OCR).
void keep_mta() noexcept;

} // namespace core::win
