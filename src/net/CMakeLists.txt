# net: HTTP for the agent's model (01-ARCHITECTURE § net). libcurl is delay-loaded: a process
# pays for libcurl.dll (and the nghttp2.dll and z.dll it brings) only at its first request.
add_library(wa_net STATIC
    curl.cpp
    sse.cpp
    proxy.cpp
    http.cpp
    stream.cpp
)
find_package(CURL CONFIG REQUIRED)
target_include_directories(wa_net PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(wa_net PUBLIC wa_core CURL::libcurl PRIVATE winhttp)
# vcpkg builds the Debug library as libcurl-d.dll: the option names the one each build links.
set(WA_CURL_DLL $<IF:$<CONFIG:Debug>,libcurl-d.dll,libcurl.dll>)
target_link_options(wa_net INTERFACE /DELAYLOAD:${WA_CURL_DLL})
target_compile_definitions(wa_net PUBLIC WA_CURL_DLL=L"${WA_CURL_DLL}" WA_CURL_DLL_NAME="${WA_CURL_DLL}")
target_link_libraries(wa_net INTERFACE delayimp)
wa_target_defaults(wa_net)
# vcpkg's app-local copy reads a program's ordinary imports only: libcurl is delay-loaded, so
# neither it nor the nghttp2 and zlib it needs would be copied, and a program linking wa_net would
# find none at its first request (Release found this). wa_net_runtime(target destination) copies
# them beside the target after each build and installs them with it into `destination`.
set(WA_NET_DLL_DIR "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}$<$<CONFIG:Debug>:/debug>/bin" CACHE INTERNAL "")
set(WA_NET_DLLS
    "${WA_NET_DLL_DIR}/${WA_CURL_DLL}"
    "${WA_NET_DLL_DIR}/nghttp2.dll"
    "${WA_NET_DLL_DIR}/$<IF:$<CONFIG:Debug>,zd.dll,z.dll>"
    CACHE INTERNAL "")
function(wa_net_runtime target destination)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${WA_NET_DLLS} $<TARGET_FILE_DIR:${target}>)
    install(FILES ${WA_NET_DLLS} DESTINATION ${destination})
endfunction()
