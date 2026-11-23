# wa_theme — the one module that owns the agent's colours and everything else of a theme
# (src/theme/theme.hpp). It depends on nothing of the agent: the renderer, the app and the installer
# depend on it.
add_library(wa_theme STATIC
    builtin.cpp
    contrast.cpp
    current.cpp
    system.cpp
)
target_include_directories(wa_theme PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(wa_theme PRIVATE advapi32) # system.cpp reads Windows' apps' mode (read only)
wa_target_defaults(wa_theme)
