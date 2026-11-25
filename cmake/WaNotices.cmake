# THIRD-PARTY-NOTICES.txt: the license texts of the libraries the program ships (the DLLs beside
# agent.exe and the ones compiled into it). Their licenses ask for the notice to travel with every
# copy, so the installer packs this file with LICENSE. Built at configure time from vcpkg's
# share/<port>/copyright — never written by hand.

set(WA_NOTICE_PORTS curl freetype harfbuzz md4c miniz nghttp2 zlib glad glaze stb)
set(WA_NOTICES_FILE "${PROJECT_BINARY_DIR}/THIRD-PARTY-NOTICES.txt")

set(wa_vcpkg_root "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
set(wa_notices "Third-party software in ${WA_PRODUCT}\n\n${WA_PRODUCT} includes the libraries below. Each section is that library's license as its authors\npublished it.\n")
foreach(port IN LISTS WA_NOTICE_PORTS)
    set(copyright "${wa_vcpkg_root}/share/${port}/copyright")
    if(NOT EXISTS "${copyright}")
        message(FATAL_ERROR "No license file for ${port}: ${copyright}")
    endif()
    file(READ "${copyright}" text)
    if(port STREQUAL "stb")
        # stb's copyright file only points at its headers: take the license from the one we use.
        file(READ "${wa_vcpkg_root}/include/stb_image_write.h" header)
        string(FIND "${header}" "This software is available under 2 licenses" at REVERSE)
        if(at EQUAL -1)
            message(FATAL_ERROR "stb_image_write.h has no license block")
        endif()
        string(SUBSTRING "${header}" ${at} -1 license)
        string(APPEND text "\n${license}")
    endif()
    string(APPEND wa_notices "\n==== ${port} ====\n${text}\n")
endforeach()
# Written beside and copied only when it changed: a reconfigure does not touch an unchanged file.
file(WRITE "${WA_NOTICES_FILE}.new" "${wa_notices}")
configure_file("${WA_NOTICES_FILE}.new" "${WA_NOTICES_FILE}" COPYONLY)
