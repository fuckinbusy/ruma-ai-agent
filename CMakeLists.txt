cmake_minimum_required(VERSION 4.0)

project(WinAiAgent VERSION 0.0.1 LANGUAGES CXX)
# The product's name, one place for all that is shown: the code (core/product.hpp through wa_version.h),
# the files' version resources, the installer's file name, the notices. The project keeps its own name.
set(WA_PRODUCT "Ruma")

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "WinAiAgent targets Windows with MSVC only")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# No C++20 named modules yet (docs/00-OVERVIEW.md): skip the per-file module scan step.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
# Static MSVC runtime (/MT, /MTd): the standard runtime is always loaded anyway, so it lives
# inside the exe; only third-party libraries ship as DLLs (docs/02-LIBRARIES.md).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Release optimization flags in one place (CMake's default /Ob2 would conflict with /Ob3: D9025).
set(CMAKE_CXX_FLAGS_RELEASE "/O2 /Ob3 /DNDEBUG")

list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)
include(WaCompile)
include(WaVersion)
include(WaNotices)
include(WaPayload)

option(WA_BUILD_TESTS "Build unit tests" ON)
option(WA_BUILD_SPIKES "Build phase-0 spikes" ON)
option(WA_BUILD_BENCH "Build micro-benchmarks" ON)
# The public source tree holds only what builds the program: the developer's folders (tests, spikes,
# benchmarks, the galleries) may be absent, and then they are simply not built.
foreach(dev IN ITEMS TESTS:tests SPIKES:spikes BENCH:bench)
    string(REPLACE ":" ";" dev "${dev}")
    list(GET dev 0 what)
    list(GET dev 1 dir)
    if(NOT EXISTS "${PROJECT_SOURCE_DIR}/${dir}/CMakeLists.txt")
        set(WA_BUILD_${what} OFF)
    endif()
endforeach()

if(WA_BUILD_TESTS)
    enable_testing() # before the source directories: src/app registers the agent smoke test
endif()

# The install: build/<Config>/ is the application only (agent.exe, agent-cli, tool-runner, their
# DLLs, data/ with config.json and logs/); the developer's programs (tests, benchmarks, spikes,
# the gallery) go to build/<Config>-dev/ beside it (a decision made for it, phase 7).
get_filename_component(WA_INSTALL_NAME "${CMAKE_INSTALL_PREFIX}" NAME)
set(WA_DEV "../${WA_INSTALL_NAME}-dev")

add_subdirectory(src/core)
add_subdirectory(src/net)
add_subdirectory(src/ai)
add_subdirectory(src/markdown)
add_subdirectory(src/security)
add_subdirectory(src/tools)
add_subdirectory(src/reader)
add_subdirectory(src/theme)
add_subdirectory(src/ui)
add_subdirectory(src/app)
add_subdirectory(src/cli)
add_subdirectory(src/setup)

# The project's license and the shipped libraries' notices go with the program (the installer packs them).
install(FILES LICENSE ${WA_NOTICES_FILE} DESTINATION .)

if(WA_BUILD_TESTS)
    add_subdirectory(tests)
endif()

add_subdirectory(tools/pack) # the installer's package: always
foreach(gallery IN ITEMS tools/gallery tools/setup_gallery)
    if(EXISTS "${PROJECT_SOURCE_DIR}/${gallery}/CMakeLists.txt")
        add_subdirectory(${gallery})
    endif()
endforeach()

if(WA_BUILD_SPIKES)
    add_subdirectory(spikes)
endif()

if(WA_BUILD_BENCH)
    add_subdirectory(bench)
endif()

# 01-ARCHITECTURE § Invariants: the UI library never reaches security or tools, not even through
# another library. Checked at configure time, after every target is known, so a violation never builds.
include(WaInvariants)
wa_forbid_link(wa_ui wa_security wa_tools wa_net wa_ai)
wa_forbid_link(wa_ai wa_ui wa_security wa_tools)
