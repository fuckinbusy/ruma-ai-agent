# ai: the model's side of the agent (01-ARCHITECTURE § ai, 05-AI-AGENT): DeepSeek's JSON and
# stream, prices, strict schemas, recordings, the agent's loop. It knows neither the interface nor
# the tools: tools come to it as specs and a function that runs them.

# The system prompt, built into the program: a raw string made at configure time, made again
# whenever prompts/system.md changes.
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/prompts/system.md WA_SYSTEM_PROMPT)
configure_file(prompt_text.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/prompt_text.cpp @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/prompts/system.md)

add_library(wa_ai STATIC
    catalog.cpp
    chat.cpp
    prompt.cpp
    stream.cpp
    cost.cpp
    schema.cpp
    cassette.cpp
    agent.cpp
    repeats.cpp
    compact.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/prompt_text.cpp
)
find_package(glaze CONFIG REQUIRED)
target_include_directories(wa_ai PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(wa_ai PUBLIC wa_net wa_core PRIVATE glaze::glaze)
wa_target_defaults(wa_ai)
