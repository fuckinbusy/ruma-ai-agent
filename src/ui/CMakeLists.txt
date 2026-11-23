find_package(freetype CONFIG REQUIRED)
find_package(harfbuzz CONFIG REQUIRED)
find_package(glad CONFIG REQUIRED)
find_package(Stb REQUIRED) # header-only: stb_image_write for PNG snapshots

add_library(wa_ui STATIC
    backdrop.cpp
    font_library.cpp
    anim.cpp
    motion.cpp
    gl/context.cpp
    gl/program_cache.cpp
    layout.cpp
    orb.cpp
    orb_look.cpp
    render/renderer.cpp
    render/target.cpp
    render/texture.cpp
    text/atlas.cpp
    text/fonts.cpp
    text/paragraph.cpp
    text/shape.cpp
    text/wrap.cpp
    text_map.cpp
    selection.cpp
    ui.cpp
    widgets.cpp
    work.cpp
    table.cpp
)
target_include_directories(wa_ui PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_include_directories(wa_ui SYSTEM PUBLIC ${Stb_INCLUDE_DIR})
target_link_libraries(wa_ui PUBLIC wa_core wa_theme Freetype::Freetype harfbuzz::harfbuzz glad::glad opengl32 dwmapi)
wa_target_defaults(wa_ui)
