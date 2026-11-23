# Project-wide compile settings applied per target (docs/09-BUILD.md).

option(WA_WARNINGS_AS_ERRORS "Treat warnings as errors" ON)

function(wa_target_defaults target)
    target_compile_options(${target} PRIVATE
        /permissive- /Zc:preprocessor /Zc:__cplusplus /utf-8 /W4 /EHsc /GR-
        /external:anglebrackets /external:W0
        $<$<BOOL:${WA_WARNINGS_AS_ERRORS}>:/WX>
        $<$<CONFIG:Release>:/Oi /Gy /Gw>
    )
    target_compile_definitions(${target} PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS
    )
    target_link_options(${target} PRIVATE
        $<$<CONFIG:Release>:/OPT:REF /OPT:ICF>
    )
endfunction()
