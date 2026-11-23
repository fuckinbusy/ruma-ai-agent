#include "setup/payload.hpp"

#include "core/fs.hpp"
#include "core/utf8.hpp"
#include "setup/win_path.hpp"

#include <windows.h>

#include <miniz.h>

#include <cctype>
#include <format>
#include <set>

namespace setup {
namespace {

// A file name of the install folder's root and nothing else.
auto plain_name(std::string_view n) -> bool {
    if (n.empty() || n == "." || n == "..")
        return false;
    for (const char c : n) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|')
            return false;
    }
    return n.back() != '.' && n.back() != ' ';
}

class Reader {
public:
    Reader() = default;
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    ~Reader() {
        if (open_)
            mz_zip_reader_end(&zip_);
    }

    // Opens the archive and lists it; refuses folders, odd names and repeated names.
    auto open(std::span<const std::byte> bytes) -> core::Result<std::vector<Entry>> {
        if (bytes.empty() || !mz_zip_reader_init_mem(&zip_, bytes.data(), bytes.size(), 0))
            return core::make_error(core::Code::Parse, "the package is damaged");
        open_ = true;
        std::vector<Entry> entries;
        std::set<std::string> seen;
        for (mz_uint i = 0, n = mz_zip_reader_get_num_files(&zip_); i < n; ++i) {
            mz_zip_archive_file_stat stat{};
            if (!mz_zip_reader_file_stat(&zip_, i, &stat))
                return core::make_error(core::Code::Parse, "the package is damaged");
            const std::string_view name{stat.m_filename};
            if (stat.m_is_directory || !plain_name(name))
                return core::make_error(core::Code::Parse, std::format("the package has a bad name: {}", name));
            std::string lower{name};
            for (auto& c : lower)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (!seen.insert(lower).second)
                return core::make_error(core::Code::Parse, std::format("the package has {} twice", name));
            entries.push_back({std::string{name}, stat.m_uncomp_size});
        }
        if (!mz_zip_validate_archive(&zip_, MZ_ZIP_FLAG_VALIDATE_HEADERS_ONLY))
            return core::make_error(core::Code::Parse, "the package is damaged");
        return entries;
    }

    auto zip() -> mz_zip_archive* { return &zip_; }

private:
    mz_zip_archive zip_{};
    bool open_ = false;
};

struct Sink {
    HANDLE file = INVALID_HANDLE_VALUE;
    std::uint64_t done = 0;
    std::uint64_t total = 0;
    const Progress* progress = nullptr;
    const std::stop_token* stop = nullptr;
    bool cancelled = false;
    DWORD os_error = 0;
};

auto write_chunk(void* opaque, mz_uint64 /*offset*/, const void* data, size_t n) -> size_t {
    auto& sink = *static_cast<Sink*>(opaque);
    if (sink.stop->stop_requested()) {
        sink.cancelled = true;
        return 0;
    }
    DWORD wrote = 0;
    if (!WriteFile(sink.file, data, static_cast<DWORD>(n), &wrote, nullptr) || wrote != n) {
        sink.os_error = GetLastError();
        return 0;
    }
    sink.done += n;
    if (*sink.progress)
        (*sink.progress)(sink.done, sink.total);
    return n;
}

} // namespace

auto payload_entries(std::span<const std::byte> zip) -> core::Result<std::vector<Entry>> {
    Reader reader;
    return reader.open(zip);
}

auto extract(std::span<const std::byte> zip, const std::filesystem::path& dir, const Progress& progress,
             const std::stop_token& stop, std::vector<std::filesystem::path>& written) -> core::Result<void> {
    Reader reader;
    auto entries = reader.open(zip);
    if (!entries)
        return std::unexpected{entries.error()};

    Sink sink;
    sink.progress = &progress;
    sink.stop = &stop;
    for (const auto& e : *entries)
        sink.total += e.size;

    for (mz_uint i = 0; i < entries->size(); ++i) {
        if (stop.stop_requested())
            return core::make_error(core::Code::Cancelled, "cancelled");
        auto wide = core::utf8::to_wide((*entries)[i].name);
        if (!wide)
            return std::unexpected{wide.error()};
        const auto target = dir / *wide;
        const auto temp = extended(dir / (*wide + L".wa-new"));
        sink.file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (sink.file == INVALID_HANDLE_VALUE)
            return core::os_error(GetLastError(), std::format("can't write {}", (*entries)[i].name));
        const bool ok = mz_zip_reader_extract_to_callback(reader.zip(), i, write_chunk, &sink, 0) != MZ_FALSE;
        const bool flushed = ok && FlushFileBuffers(sink.file);
        CloseHandle(sink.file);
        sink.file = INVALID_HANDLE_VALUE;
        if (!ok || !flushed) {
            const DWORD os = sink.os_error != 0 ? sink.os_error : GetLastError();
            DeleteFileW(temp.c_str());
            if (sink.cancelled)
                return core::make_error(core::Code::Cancelled, "cancelled");
            if (sink.os_error != 0 || !flushed)
                return core::os_error(os, std::format("can't write {}", (*entries)[i].name));
            return core::make_error(core::Code::Parse, std::format("the package is damaged at {}", (*entries)[i].name));
        }
        if (!MoveFileExW(temp.c_str(), extended(target).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const DWORD os = GetLastError();
            DeleteFileW(temp.c_str());
            return core::os_error(os, std::format("can't replace {}", (*entries)[i].name));
        }
        written.push_back(target);
    }
    return {};
}

auto read_entry(std::span<const std::byte> zip, std::string_view name) -> core::Result<std::string> {
    Reader reader;
    const auto entries = reader.open(zip);
    if (!entries)
        return std::unexpected{entries.error()};
    for (mz_uint i = 0; i < entries->size(); ++i) {
        if ((*entries)[i].name != name)
            continue;
        std::string out(static_cast<std::size_t>((*entries)[i].size), '\0');
        if (!mz_zip_reader_extract_to_mem(reader.zip(), i, out.data(), out.size(), 0))
            return core::make_error(core::Code::Parse, std::format("the package is damaged at {}", name));
        return out;
    }
    return core::make_error(core::Code::NotFound, std::format("the package has no {}", name));
}

auto payload_names() -> std::vector<std::string> {
    std::vector<std::string> names;
    std::string_view all{WA_PAYLOAD_FILES};
    while (!all.empty()) {
        const auto bar = all.find('|');
        names.emplace_back(all.substr(0, bar));
        all = bar == std::string_view::npos ? std::string_view{} : all.substr(bar + 1);
    }
    return names;
}

auto pack(std::span<const std::filesystem::path> files) -> core::Result<std::string> {
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_heap(&zip, 0, 0))
        return core::make_error(core::Code::Internal, "can't start the package");
    const auto fail = [&](core::Code code, std::string message) {
        mz_zip_writer_end(&zip);
        return core::make_error(code, std::move(message));
    };
    for (const auto& f : files) {
        auto name = core::utf8::to_utf8(f.filename().native());
        if (!name || !plain_name(*name))
            return fail(core::Code::InvalidArgument, "a file name the package can't hold");
        auto data = core::fs::read_file(f, 512ull << 20);
        if (!data)
            return fail(data.error().code, data.error().message);
        if (!mz_zip_writer_add_mem(&zip, name->c_str(), data->data(), data->size(), MZ_BEST_COMPRESSION))
            return fail(core::Code::Internal, std::format("can't pack {}", *name));
    }
    void* buffer = nullptr;
    std::size_t size = 0;
    if (!mz_zip_writer_finalize_heap_archive(&zip, &buffer, &size))
        return fail(core::Code::Internal, "can't finish the package");
    std::string out{static_cast<const char*>(buffer), size};
    mz_zip_writer_end(&zip);
    return out;
}

} // namespace setup
