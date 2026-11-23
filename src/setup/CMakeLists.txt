# setup: the installer and uninstall.exe (docs/specs/installer.md). wa_setup is the logic without a
# window — folders, versions, the journal, the package, install and uninstall — so its tests run it
# against substituted roots.
find_package(glaze CONFIG REQUIRED)
add_library(wa_setup STATIC
    model.cpp
    journal.cpp
    payload.cpp
    system.cpp
    steps.cpp
    flow.cpp
    self.cpp
)
find_package(miniz CONFIG REQUIRED)
target_include_directories(wa_setup PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(wa_setup PUBLIC wa_core PRIVATE glaze::glaze miniz::miniz)
# The package list (cmake/WaPayload.cmake), joined by '|': a ';' would split the definition.
list(JOIN WA_PAYLOAD_FILES "|" wa_setup_payload)
target_compile_definitions(wa_setup PRIVATE "WA_PAYLOAD_FILES=\"${wa_setup_payload}\"")
target_link_libraries(wa_setup PRIVATE ole32 oleaut32 shell32 propsys advapi32)
wa_target_defaults(wa_setup)

# wa_setup_ui: the installer's window drawn from a setup::State (design/ui_installer).
add_library(wa_setup_ui STATIC
    view/painter.cpp
    view/motion.cpp
)
target_link_libraries(wa_setup_ui PUBLIC wa_setup wa_ui)
wa_target_defaults(wa_setup_ui)

# wa_setup_app: the program's window, its work thread, the plain dialogs.
add_library(wa_setup_app STATIC
    app/window.cpp
)
target_link_libraries(wa_setup_app PUBLIC wa_setup_ui PRIVATE shcore comctl32 ole32 shell32)
wa_target_defaults(wa_setup_app)

# The setup's own DLLs travel inside it (resource type DLL) and are delay-loaded from its temporary
# folder: one exe on a clean computer. The configuration's own names (vcpkg's Debug freetyped.dll).
set(wa_vcpkg_root "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(WA_SETUP_OWN_DLLS freetyped.dll harfbuzz.dll miniz.dll)
    set(wa_dll_dir "${wa_vcpkg_root}/debug/bin")
else()
    set(WA_SETUP_OWN_DLLS freetype.dll harfbuzz.dll miniz.dll)
    set(wa_dll_dir "${wa_vcpkg_root}/bin")
endif()
set(wa_setup_dlls_rc "")
foreach(dll IN LISTS WA_SETUP_OWN_DLLS) # FREETYPE_DLL: setup::self::dll_resource_name
    string(TOUPPER "${dll}" name)
    string(REPLACE "." "_" name "${name}")
    string(APPEND wa_setup_dlls_rc "${name} DLL \"${wa_dll_dir}/${dll}\"\n")
endforeach()

# One program, two files: `name` with the package (`payload_rc`) or without it.
function(wa_setup_exe target name description payload_rc)
    set(WA_SETUP_NAME ${name})
    set(WA_SETUP_FILE ${name}.exe)
    set(WA_SETUP_DESCRIPTION ${description})
    set(WA_SETUP_DLLS_RC ${wa_setup_dlls_rc})
    set(WA_SETUP_PAYLOAD_RC ${payload_rc})
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/app/setup.rc.in ${CMAKE_CURRENT_BINARY_DIR}/${target}.rc @ONLY)
    add_executable(${target} WIN32 app/main.cpp ${CMAKE_CURRENT_BINARY_DIR}/${target}.rc app/setup.manifest)
    set_target_properties(${target} PROPERTIES OUTPUT_NAME ${name})
    target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/res) # resource.h and icon/ for the .rc
    target_link_libraries(${target} PRIVATE wa_setup_app delayimp)
    foreach(dll IN LISTS WA_SETUP_OWN_DLLS)
        target_link_options(${target} PRIVATE /DELAYLOAD:${dll})
    endforeach()
    target_link_options(${target} PRIVATE /DELAY:UNLOAD) # unloaded at the end, so their folder can go
    target_link_options(${target} PRIVATE /MANIFESTUAC:NO) # the level is setup.manifest's: an administrator's
    wa_target_defaults(${target})
endfunction()

wa_setup_exe(uninstaller uninstall "Remove WinAiAgent" "")
if(WA_BUILD_TESTS)
    # The installer's whole way in windows never shown: its DLLs unpacked and delay-loaded, the GL
    # window, the appearance, a screen, the launch (no agent started), its temporary folder removed.
    add_test(NAME setup_smoke COMMAND installer --smoke)
    # An administrator's program run as the user, without a prompt (Windows' RunAsInvoker layer).
    set_tests_properties(setup_smoke PROPERTIES ENVIRONMENT "__COMPAT_LAYER=RunAsInvoker")
    set_tests_properties(setup_smoke PROPERTIES TIMEOUT 60)
    # uninstall.exe the same way: its screens drawn, nothing pressed, nothing removed, no copy in %TEMP%.
    add_test(NAME uninstall_smoke COMMAND uninstaller --smoke)
    set_tests_properties(uninstall_smoke PROPERTIES ENVIRONMENT "__COMPAT_LAYER=RunAsInvoker")
    set_tests_properties(uninstall_smoke PROPERTIES TIMEOUT 60)
endif()

# The package: exactly cmake/WaPayload.cmake's files, from this build (agent.exe and the DLLs beside
# it, uninstall.exe, the licenses), packed by tools/pack.
set(WA_PAYLOAD_ZIP ${CMAKE_CURRENT_BINARY_DIR}/payload.zip)
set(wa_payload_sources $<TARGET_FILE:agent> $<TARGET_FILE:agent-reader>)
foreach(dll IN LISTS WA_PAYLOAD_DLLS)
    list(APPEND wa_payload_sources $<TARGET_FILE_DIR:agent>/${dll})
endforeach()
list(APPEND wa_payload_sources $<TARGET_FILE:uninstaller> ${PROJECT_SOURCE_DIR}/LICENSE ${WA_NOTICES_FILE})
add_custom_command(OUTPUT ${WA_PAYLOAD_ZIP}
    COMMAND wa-pack ${WA_PAYLOAD_ZIP} ${wa_payload_sources}
    DEPENDS agent agent-reader uninstaller wa-pack ${PROJECT_SOURCE_DIR}/LICENSE ${WA_NOTICES_FILE}
    COMMENT "Packing the installer's package"
    VERBATIM)
add_custom_target(payload DEPENDS ${WA_PAYLOAD_ZIP})

wa_setup_exe(installer WinAiAgentSetup-${PROJECT_VERSION} "WinAiAgent Setup" "PAYLOAD RCDATA \"${WA_PAYLOAD_ZIP}\"")
add_dependencies(installer payload)
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/installer.rc PROPERTIES OBJECT_DEPENDS ${WA_PAYLOAD_ZIP})
set(WA_INSTALLER_EXE $<TARGET_FILE:installer> PARENT_SCOPE)
# The file to hand out: build/installer/ (Release only; the package is the Release program).
install(TARGETS installer RUNTIME DESTINATION ../installer CONFIGURATIONS Release)
