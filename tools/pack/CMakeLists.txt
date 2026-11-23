# wa-pack: builds the installer's package from cmake/WaPayload.cmake's list (installer, task 3).
add_executable(wa-pack main.cpp)
target_link_libraries(wa-pack PRIVATE wa_setup)
wa_target_defaults(wa-pack)
