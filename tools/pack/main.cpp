// wa-pack <package.zip> <file>... — the installer's package (cmake/WaPayload.cmake): each file under
// its own name, through setup::pack, the same code the tests check. A build tool, never shipped.
#include "core/fs.hpp"
#include "setup/payload.hpp"

#include <cstdio>
#include <filesystem>
#include <vector>

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::fputs("usage: wa-pack <package.zip> <file>...\n", stderr);
        return 2;
    }
    std::vector<std::filesystem::path> files;
    for (int i = 2; i < argc; ++i)
        files.emplace_back(argv[i]);
    auto zip = setup::pack(files);
    if (!zip) {
        std::fprintf(stderr, "wa-pack: %s\n", zip.error().message.c_str());
        return 1;
    }
    if (auto saved = core::fs::write_file_atomic(argv[1], *zip); !saved) {
        std::fprintf(stderr, "wa-pack: %s\n", saved.error().message.c_str());
        return 1;
    }
    std::printf("wa-pack: %zu files, %zu bytes\n", files.size(), zip->size());
    return 0;
}
