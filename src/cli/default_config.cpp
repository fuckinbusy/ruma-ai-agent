// default-config <file>: writes config.json with every default, through app::config::save — the
// same text the agent writes. The build runs it, and the install puts the file into
// build/<Config>/data/ (a portable build: the agent's data beside the program).

#include "app/config.hpp"
#include "core/crash.hpp"

#include <cstdio>
#include <filesystem>

int wmain(int argc, wchar_t** argv) {
    core::crash::quiet_failures();
    if (argc != 2) {
        std::fputs("usage: default-config <file>\n", stderr);
        return 2;
    }
    if (const auto saved = app::config::save(std::filesystem::path{argv[1]}, app::config::Config{}); !saved) {
        std::fprintf(stderr, "default-config: %s\n", saved.error().message.c_str());
        return 1;
    }
    return 0;
}
