# security: what a path names and which zone it is in; the policy; the shell guard; the audit;
# the API key. Never linked into wa_ui (01-ARCHITECTURE § Invariants).
add_library(wa_security STATIC
    paths.cpp
    policy.cpp
    shellguard.cpp
    sha256.cpp
    audit.cpp
    secrets.cpp
)
find_package(glaze CONFIG REQUIRED)
target_include_directories(wa_security PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(wa_security PUBLIC wa_core PRIVATE glaze::glaze bcrypt crypt32)
wa_target_defaults(wa_security)
