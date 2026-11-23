# app: the agent program (agent.exe) and the library its tests link.
add_library(wa_app STATIC
    shell.cpp
    config.cpp
    frame_rate.cpp
    scenes/accordion.cpp
    scenes/approval.cpp
    scenes/fold.cpp
    scenes/send.cpp
    scenes/sheet.cpp
    scenes/sign.cpp
    scenes/stream.cpp
    instance.cpp
    answer.cpp
    sheet_model.cpp
    decode_jobs.cpp
    details.cpp
    drop_target.cpp
    sheet_view.cpp
    sheet_steps.cpp
    sheet_markdown.cpp
    sheet_conversation.cpp
    sheet_panel.cpp
    sheet_space.cpp
    sheet_threat.cpp
    sheet_activity.cpp
    sheet_translate.cpp
    sheet_permission.cpp
    sheet_log.cpp
    sheet_report.cpp
    sheet_background.cpp
    sheet_handle.cpp
    picker.cpp
    sheet_menu.cpp
    tray_menu.cpp
    tray_icon.cpp
    sheet_screen.cpp
    sheet_document.cpp
    sheet_tools.cpp
    sheet_terminal.cpp
    sim.cpp
    agent_tools.cpp
    diag_keys.cpp
    settings.cpp
    gpu.cpp
    keycheck.cpp
    history.cpp
    journal.cpp
    memory.cpp
    monitor.cpp
    report.cpp
    present.cpp
    sizes.cpp
    space.cpp
    model_agent.cpp
    motion.cpp
    scene_run.cpp
    notify.cpp
    desktop_icons.cpp
    terminal.cpp
    hotkeys.cpp
    fullscreen.cpp
    zorder.cpp
    debug_metrics.cpp
)
target_include_directories(wa_app PUBLIC ${PROJECT_SOURCE_DIR}/src)
find_package(glaze CONFIG REQUIRED)
# the answer's Markdown: wa_markdown (phase 8; its own library since phase 9)
target_link_libraries(wa_app PUBLIC wa_ui wa_tools wa_ai dwmapi shcore advapi32 PRIVATE glaze::glaze wa_markdown shlwapi)
target_include_directories(wa_app PRIVATE ${PROJECT_SOURCE_DIR}/res) # resource.h: the icon id
wa_target_defaults(wa_app)

# The debug window (Dear ImGui, F12) exists only in Debug builds: Release has no ImGui at all
# (scripts/check.ps1 checks the Release exe).
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    find_package(imgui CONFIG REQUIRED)
    target_sources(wa_app PRIVATE debug_window.cpp)
    target_link_libraries(wa_app PRIVATE imgui::imgui)
    target_compile_definitions(wa_app PUBLIC WA_DEBUG_UI=1)
endif()

add_executable(agent WIN32 main.cpp agent.rc) # agent.rc: the icon and the version (installer, task 1)
target_link_libraries(agent PRIVATE wa_app)
target_include_directories(agent PRIVATE ${PROJECT_SOURCE_DIR}/res) # resource.h and icon/ for the .rc
wa_target_defaults(agent)
install(TARGETS agent RUNTIME DESTINATION .) # build/<Config>/agent.exe, next to the DLLs
wa_net_runtime(agent .) # libcurl and its DLLs next to it: delay-loaded (the first request), not copied by vcpkg

if(WA_BUILD_TESTS)
    # The whole agent process, nothing shown: ask, stream, press a button, finish (exit code 0).
    add_test(NAME agent_smoke COMMAND agent --smoke --sim-speed=4)
    set_tests_properties(agent_smoke PROPERTIES TIMEOUT 60)
    # The real agent's path (phase 7) on a recording: no network, the report and its cost.
    # The installer's exit message ends the agent as the tray's Exit does (installer, task 5).
    add_test(NAME agent_smoke_exit COMMAND agent --smoke --smoke-exit)
    set_tests_properties(agent_smoke_exit PROPERTIES TIMEOUT 60)
    add_test(NAME agent_replay_smoke COMMAND agent --smoke --replay=${PROJECT_SOURCE_DIR}/tests/fixtures/dialogs/slow.cassette)
    set_tests_properties(agent_replay_smoke PROPERTIES TIMEOUT 60)
endif()
