#include "tools/docs/docs.hpp"

#include <miniz.h>

#include <format>

namespace tools::docs {

struct ZipReader::State {
    std::string data; // the archive: miniz reads from it while the reader lives
    mz_zip_archive zip{};
    std::size_t inflated = 0; // all the parts read so far
    State() = default;
    State(const State&) = delete;
    auto operator=(const State&) -> State& = delete;
    ~State() { mz_zip_reader_end(&zip); }
};

namespace {

// "4.6 GB", "70.0 MB".
auto size_text(std::uint64_t bytes) -> std::string {
    return bytes >= (1ull << 30) ? std::format("{:.1f} GB", static_cast<double>(bytes) / (1ull << 30))
                                 : std::format("{:.1f} MB", static_cast<double>(bytes) / (1ull << 20));
}

// "120 000": thousands apart, as the cards write them.
auto grouped(std::uint64_t n) -> std::string {
    auto s = std::to_string(n);
    for (auto i = static_cast<std::ptrdiff_t>(s.size()) - 3; i > 0; i -= 3) {
        s.insert(static_cast<std::size_t>(i), 1, ' ');
    }
    return s;
}

} // namespace

ZipReader::ZipReader(std::unique_ptr<State> s) : state_(std::move(s)) {}
ZipReader::ZipReader(ZipReader&&) noexcept = default;
auto ZipReader::operator=(ZipReader&&) noexcept -> ZipReader& = default;
ZipReader::~ZipReader() = default;

auto ZipReader::open(std::span<const std::byte> zip) -> core::Result<ZipReader> {
    auto s = std::make_unique<State>();
    s->data.assign(reinterpret_cast<const char*>(zip.data()), zip.size());
    if (!mz_zip_reader_init_mem(&s->zip, s->data.data(), s->data.size(), 0)) {
        return core::make_error(core::Code::Parse, "not a ZIP file (or a damaged one)");
    }
    // A ZIP bomb (phase 10): its parts together unpack past what one reader may inflate, at over
    // 100 times the file's size (documents pack 3-20 times) - refused before a byte is inflated.
    std::uint64_t total = 0;
    for (mz_uint i = 0, n = mz_zip_reader_get_num_files(&s->zip); i < n; ++i) {
        mz_zip_archive_file_stat stat{};
        if (mz_zip_reader_file_stat(&s->zip, i, &stat)) {
            total += stat.m_uncomp_size;
        }
    }
    if (total > kMaxInflated && total / std::max<std::size_t>(1, zip.size()) > 100) {
        auto e = core::make_error(core::Code::InvalidArgument, std::format("a ZIP bomb: it unpacks to {}", size_text(total)));
        e.error().kind = "zip_bomb";
        e.error().facts = std::format("unpacks to {} \xE2\x80\x94 {}\xC3\x97 the file size", size_text(total), grouped(total / std::max<std::size_t>(1, zip.size())));
        return e;
    }
    return ZipReader{std::move(s)};
}

auto ZipReader::read(std::string_view name, std::size_t max) -> core::Result<std::string> {
    // Only plain names inside the archive: a climbing one is never even looked up.
    if (name.empty() || name.front() == '/' || name.find("..") != std::string_view::npos || name.find('\\') != std::string_view::npos ||
        name.find(':') != std::string_view::npos) {
        return core::make_error(core::Code::InvalidArgument, std::format("not a name inside the document: {}", name));
    }
    const std::string wanted{name};
    const int index = mz_zip_reader_locate_file(&state_->zip, wanted.c_str(), nullptr, 0);
    if (index < 0) {
        return core::make_error(core::Code::NotFound, std::format("the document has no {}", name));
    }
    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(&state_->zip, static_cast<mz_uint>(index), &stat)) {
        return core::make_error(core::Code::Parse, std::format("{} cannot be read", name));
    }
    if (stat.m_uncomp_size > max) { // before inflating a byte: a ZIP bomb stays a few KB
        auto e = core::make_error(core::Code::InvalidArgument,
                                  std::format("{} is too large ({} MB inflated, at most {} MB)", name, stat.m_uncomp_size >> 20, max >> 20));
        e.error().kind = "too_large";
        e.error().facts = std::format("{} unpacked", size_text(stat.m_uncomp_size));
        return e;
    }
    if (stat.m_uncomp_size > kMaxInflated - state_->inflated) {
        auto e = core::make_error(core::Code::InvalidArgument, std::format("the document inflates past {} MB (at {})", kMaxInflated >> 20, name));
        e.error().kind = "too_large";
        e.error().facts = std::format("{} unpacked", size_text(stat.m_uncomp_size));
        return e;
    }
    std::string out(static_cast<std::size_t>(stat.m_uncomp_size), '\0');
    if (!mz_zip_reader_extract_to_mem(&state_->zip, static_cast<mz_uint>(index), out.data(), out.size(), 0)) {
        return core::make_error(core::Code::Parse, std::format("{} is damaged", name));
    }
    state_->inflated += out.size();
    return out;
}

auto ZipReader::head(std::string_view name, std::size_t bytes) -> std::string {
    const std::string wanted{name};
    const int index = mz_zip_reader_locate_file(&state_->zip, wanted.c_str(), nullptr, 0);
    if (index < 0) {
        return {};
    }
    mz_zip_reader_extract_iter_state* it = mz_zip_reader_extract_iter_new(&state_->zip, static_cast<mz_uint>(index), 0);
    if (it == nullptr) {
        return {};
    }
    std::string out(bytes, '\0');
    out.resize(mz_zip_reader_extract_iter_read(it, out.data(), out.size()));
    mz_zip_reader_extract_iter_free(it);
    return out;
}

auto ZipReader::names() const -> std::vector<std::string> {
    std::vector<std::string> out;
    const mz_uint n = mz_zip_reader_get_num_files(&state_->zip);
    for (mz_uint i = 0; i < n; ++i) {
        char name[512]{};
        mz_zip_reader_get_filename(&state_->zip, i, name, sizeof name);
        out.emplace_back(name);
    }
    return out;
}

void ZipWriter::add(std::string_view name, std::string_view data) {
    parts_.emplace_back(std::string{name}, std::string{data});
}

auto ZipWriter::finish() -> core::Result<std::string> {
    std::string out;
    mz_zip_archive zip{};
    zip.m_pIO_opaque = &out;
    zip.m_pWrite = [](void* opaque, mz_uint64 at, const void* data, size_t n) -> size_t {
        auto& s = *static_cast<std::string*>(opaque);
        if (s.size() < at + n) {
            s.resize(static_cast<std::size_t>(at + n));
        }
        std::copy_n(static_cast<const char*>(data), n, s.data() + at);
        return n;
    };
    if (!mz_zip_writer_init(&zip, 0)) {
        return core::make_error(core::Code::Internal, "the ZIP writer did not start");
    }
    bool ok = true;
    for (const auto& [name, data] : parts_) {
        ok = ok && mz_zip_writer_add_mem(&zip, name.c_str(), data.data(), data.size(), static_cast<mz_uint>(MZ_DEFAULT_LEVEL));
    }
    ok = ok && mz_zip_writer_finalize_archive(&zip);
    mz_zip_writer_end(&zip);
    if (!ok) {
        return core::make_error(core::Code::Internal, "the document could not be packed");
    }
    return out;
}

} // namespace tools::docs
