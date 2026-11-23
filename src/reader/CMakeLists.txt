# agent-reader.exe (phase 13a § 6): the administrator's read-only helper. Its own sources only —
# no wa_tools, wa_core or any library that can change something: the import table is what the
# tests read. requireAdministrator in its manifest; the UAC prompt is Windows' own.
#
# The runtime: the C++ library static, the C runtime Windows' own (UCRT, part of Windows 10 and
# 11), the release ones in every configuration. A wholly static runtime brings TerminateProcess,
# GetProcAddress and LoadLibraryExW into the import table by itself; the debug one brings the
# last two (LIBCMTD's pdblkup, libvcruntimed's winapi_thunks) — found 2026-10-28.
add_executable(agent-reader
    main.cpp
    pipe_io.cpp
    collect.cpp
    ${PROJECT_SOURCE_DIR}/src/tools/winsys/readonly_com.cpp
)
set_property(TARGET agent-reader PROPERTY MSVC_RUNTIME_LIBRARY MultiThreaded)
set_property(TARGET agent-reader PROPERTY MSVC_RUNTIME_CHECKS "") # /RTC1 pulls LIBCMT's pdblkup
find_package(glaze CONFIG REQUIRED)
target_include_directories(agent-reader PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(agent-reader PRIVATE glaze::glaze wevtapi ole32 oleaut32 advapi32 taskschd wbemuuid)
target_link_options(agent-reader PRIVATE
    "/MANIFESTUAC:level='requireAdministrator'"
    # Its DLLs from System32 only: one put beside it by another program is never loaded (final review C2).
    /DEPENDENTLOADFLAG:0x800
    /OPT:REF
    /NODEFAULTLIB:libucrt.lib
    ucrt.lib
)
wa_target_defaults(agent-reader)
install(TARGETS agent-reader RUNTIME DESTINATION .) # build/<Config>/agent-reader.exe, next to agent.exe
