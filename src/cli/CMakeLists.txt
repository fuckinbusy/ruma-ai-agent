# tool-runner: one tool call from the console, through the policy and the audit (agent-cli's
# first part, 05-AI-AGENT § The CLI harness).
add_executable(tool-runner tool_runner.cpp console.cpp)
target_link_libraries(tool-runner PRIVATE wa_app) # app::config and the agent's settings_of
wa_target_defaults(tool-runner)
install(TARGETS tool-runner RUNTIME DESTINATION .) # build/<Config>/tool-runner.exe, next to agent.exe

# agent-cli: the agent in a console (05-AI-AGENT § The CLI harness): --set-key puts the DeepSeek key into
# DPAPI from stdin; --probe asks DeepSeek what phase 7 must know first (with the user's key, by hand
# only); the request loop with --record/--replay comes with the agent's loop.
add_executable(agent-cli agent_cli.cpp console.cpp)
target_link_libraries(agent-cli PRIVATE wa_app wa_ai)
wa_target_defaults(agent-cli)
install(TARGETS agent-cli RUNTIME DESTINATION .)
wa_net_runtime(agent-cli .) # libcurl and its DLLs next to it: delay-loaded, not copied by vcpkg

# The build is portable: the agent's data (config.json, logs, history, audit, the key) in data/
# beside agent.exe (core::win::agent_data_dir). default-config writes config.json with the
# defaults at build time; the install ships it with an empty logs/.
add_executable(default-config default_config.cpp)
target_link_libraries(default-config PRIVATE wa_app)
wa_target_defaults(default-config)
set(WA_DEFAULT_CONFIG "${CMAKE_BINARY_DIR}/generated/config.json" CACHE INTERNAL "")
add_custom_command(OUTPUT ${WA_DEFAULT_CONFIG}
                   COMMAND default-config ${WA_DEFAULT_CONFIG}
                   DEPENDS default-config
                   COMMENT "config.json with the defaults"
                   VERBATIM)
add_custom_target(default-config-file ALL DEPENDS ${WA_DEFAULT_CONFIG})
# The defaults go only where there is no config.json yet: a build installed again keeps the
# settings its user chose in the tray.
install(CODE "
    set(_config \"\${CMAKE_INSTALL_PREFIX}/data/config.json\")
    if(EXISTS \"\${_config}\")
        message(STATUS \"Kept: \${_config}\")
    else()
        file(INSTALL \"${WA_DEFAULT_CONFIG}\" DESTINATION \"\${CMAKE_INSTALL_PREFIX}/data\")
    endif()")
install(DIRECTORY DESTINATION data/logs)