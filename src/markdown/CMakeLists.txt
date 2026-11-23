# markdown: the model's Markdown as blocks (md4c). Its own library since phase 9: the answer is
# drawn from it (wa_app) and a DOCX is written from it (wa_tools, doc.write).
add_library(wa_markdown STATIC
    markdown.cpp
)
find_package(md4c CONFIG REQUIRED)
target_include_directories(wa_markdown PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(wa_markdown PRIVATE md4c::md4c)
wa_target_defaults(wa_markdown)
