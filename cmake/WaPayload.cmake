# What the installer carries (docs/specs/installer.md § Goals 2): the program, the DLLs it needs,
# agent-reader.exe (phase 13a § 6), uninstall.exe and the licenses — an explicit list, never
# "everything in the folder". No tests, no console tools (tool-runner, agent-cli, default-config),
# no .pdb, no -dev folders, no data.
#
# WA_PAYLOAD_FILES: the names inside the package (flat: the install folder's root). The DLL names
# are the configuration's own (vcpkg's Debug DLLs are freetyped.dll, zd.dll, libcurl-d.dll): the
# package holds what agent.exe of that build imports. The tests read the same variable.

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(WA_PAYLOAD_DLLS freetyped.dll harfbuzz.dll libcurl-d.dll md4c.dll miniz.dll nghttp2.dll zd.dll)
else()
    set(WA_PAYLOAD_DLLS freetype.dll harfbuzz.dll libcurl.dll md4c.dll miniz.dll nghttp2.dll z.dll)
endif()
set(WA_PAYLOAD_FILES agent.exe agent-reader.exe ${WA_PAYLOAD_DLLS} uninstall.exe LICENSE THIRD-PARTY-NOTICES.txt)
