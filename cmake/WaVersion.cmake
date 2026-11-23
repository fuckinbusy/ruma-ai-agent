# The product version has one source: project(WinAiAgent VERSION x.y.z) in CMakeLists.txt.
# vcpkg.json must say the same (it can't read CMake), and the code and the .rc files read the
# generated wa_version.h — never a second copy of the number.

file(READ "${PROJECT_SOURCE_DIR}/vcpkg.json" wa_vcpkg_json)
string(JSON wa_vcpkg_version GET "${wa_vcpkg_json}" "version-string")
if(NOT wa_vcpkg_version STREQUAL PROJECT_VERSION)
    message(FATAL_ERROR "vcpkg.json version-string is ${wa_vcpkg_version}, the project is ${PROJECT_VERSION}: "
                        "change both together")
endif()

set(WA_GENERATED_DIR "${PROJECT_BINARY_DIR}/generated")
configure_file("${PROJECT_SOURCE_DIR}/src/core/wa_version.h.in" "${WA_GENERATED_DIR}/wa_version.h" @ONLY)
