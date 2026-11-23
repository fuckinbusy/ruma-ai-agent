add_library(wa_core STATIC
    version.cpp
    error.cpp
    time.cpp
    utf8.cpp
    base64.cpp
    jobs.cpp
    log.cpp
    secret_like.cpp
    win.cpp
    fs.cpp
    crash.cpp
    mem/alloc.cpp
    mem/arena.cpp
)
target_include_directories(wa_core PUBLIC ${PROJECT_SOURCE_DIR}/src ${WA_GENERATED_DIR}) # wa_version.h
target_link_libraries(wa_core PUBLIC shell32 ole32) # known folders, CoTaskMemFree

# Log levels compiled in: everything in Debug; INFO and above in Release unless overridden
# (0 trace, 1 debug, 2 info, 3 warn, 4 error). Lower levels cost nothing: no code is emitted.
set(WA_LOG_RELEASE_LEVEL 2 CACHE STRING "Lowest log level compiled into non-Debug builds")
target_compile_definitions(wa_core PUBLIC
    WA_LOG_COMPILE_LEVEL=$<IF:$<CONFIG:Debug>,0,${WA_LOG_RELEASE_LEVEL}>)
# Pull mem/alloc.cpp (global operator new/delete replacement) into every executable that links
# wa_core, even if it never calls core::mem directly.
target_link_options(wa_core INTERFACE /INCLUDE:wa_mem_force_link)
wa_target_defaults(wa_core)
