#include "tools/image_tools.hpp"

#include "core/json.hpp"
#include "core/utf8.hpp"
#include "tools/guarded_fs.hpp"
#include "tools/image.hpp"
#include "tools/ocr.hpp"
#include "tools/os.hpp"

#include <glaze/glaze.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>

// Argument and result types: named namespaces (glaze reflects only types with external linkage).
namespace tools::pictures::args {

struct View {
    std::string path;
    std::optional<std::string> detail; // "low" (the default) or "high"
};
struct Convert {
    std::vector<std::string> paths;
    std::optional<std::string> format; // jpeg, png, bmp, tiff, gif; null: as it is (png if Windows cannot write it)
    std::optional<int> max_side;
    std::optional<int> quality;
    std::optional<int> rotate; // 0, 90, 180, 270 clockwise
    std::optional<std::string> folder;
};
struct Capture {
    std::string target;                // "screen", "window" or "active"
    std::optional<std::string> window; // "window": a part of its title
};
struct Ocr {
    std::string path;
    std::optional<std::string> language; // a BCP-47 tag; null: the user's
};

} // namespace tools::pictures::args

namespace tools::pictures::out {

struct Attached {
    bool attached = true;
    int width = 0;
    int height = 0;
    std::string format; // png or jpeg, as the model gets it
    std::string note = "the image follows in the next message";
};
struct ConvertedFile {
    std::string from, to;
    int width = 0, height = 0; // the new file's
    std::uint64_t bytes = 0, from_bytes = 0;
    int rotated = 0;            // clockwise: its EXIF turn and the one asked
    bool already_small = false; // within max_side already: not made smaller
};
struct Converted {
    std::vector<std::string> made; // the new files; the sources are never changed
    std::vector<ConvertedFile> files;
    std::uint64_t bytes_before = 0, bytes_after = 0;
    std::string folder;
};
struct Doubt {
    std::size_t line = 0; // from 1
    std::string word;
};
struct Read {
    std::string language;
    std::string text;
    std::size_t line_count = 0;
    std::vector<Doubt> doubtful; // words Windows may have misread (two scripts in one, a foreign one)
    std::optional<double> angle;
};

} // namespace tools::pictures::out

template <>
struct glz::json_schema<tools::pictures::args::View> {
    schema path{.description = "The picture: JPEG, PNG, GIF, BMP, TIFF, ICO, WebP or HEIC."};
    schema detail{.description = "\"low\" (1024 px, the default) or \"high\" (2048 px: small text, fine detail)."};
};

template <>
struct glz::json_schema<tools::pictures::args::Convert> {
    schema paths{.description = "The pictures to convert (at most 100). They are never changed: new files are made."};
    schema format{.description = "\"jpeg\", \"png\", \"bmp\", \"tiff\" or \"gif\"; null keeps the format. WebP cannot be written."};
    schema max_side{.description = "The longer side at most this many pixels (never larger); null: the size as it is."};
    schema quality{.description = "JPEG quality 1-100 (85 if null)."};
    schema rotate{.description = "Turn clockwise by 90, 180 or 270 degrees; null: as it is."};
    schema folder{.description = "Where the new files go; null: a folder \"converted\" beside each picture."};
};

template <>
struct glz::json_schema<tools::pictures::args::Capture> {
    schema target{.description = "\"screen\" (the monitor the user works on), \"window\" (one by its title) or \"active\" "
                                 "(the window the user was in).",
                  .enumeration = std::vector<std::string_view>{"screen", "window", "active"}};
    schema window{.description = "For \"window\": a part of its title (\"Excel\", \"invoice.pdf\"); else null."};
};

template <>
struct glz::json_schema<tools::pictures::args::Ocr> {
    schema path{.description = "The picture with text: a scan, a photo of a page, a screenshot."};
    schema language{.description = "The text's language as a tag (\"en-US\", \"ru\", \"zh-Hans-CN\"); null: the user's own."};
};

namespace tools::pictures {
namespace {

constexpr std::size_t kMaxFile = 50u << 20; // a picture file larger is not opened

template <class T>
auto parse(std::string_view json) -> core::Result<T> {
    T value{};
    if (const auto ec = glz::read<glz::opts{.error_on_unknown_keys = true, .error_on_missing_keys = true}>(value, json)) {
        return core::make_error(core::Code::InvalidArgument, "arguments: " + glz::format_error(ec, json));
    }
    return value;
}

auto utf8(std::wstring_view w) -> std::string {
    auto s = core::utf8::to_utf8(w);
    return s ? std::move(*s) : std::string{"?"};
}

auto lower_ext(const std::filesystem::path& p) -> std::string {
    auto e = p.extension().string();
    std::ranges::transform(e, e.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return e;
}

// A picture's extension: what Windows' codecs read.
auto image_ext(std::string_view ext) -> bool {
    for (const std::string_view e : {".jpg", ".jpeg", ".png", ".gif", ".bmp", ".tif", ".tiff", ".ico", ".webp", ".heic", ".heif"}) {
        if (ext == e) {
            return true;
        }
    }
    return false;
}

} // namespace

auto view_schema() -> std::string {
    auto s = glz::write_json_schema<args::View>();
    return s ? std::move(*s) : std::string{"{}"};
}

auto view_request(std::string_view json, const std::filesystem::path& wd) -> core::Result<security::Request> {
    auto a = parse<args::View>(json);
    if (!a) {
        return std::unexpected{a.error()};
    }
    if (a->detail && *a->detail != "low" && *a->detail != "high") {
        return core::make_error(core::Code::InvalidArgument, "arguments: \"detail\" is \"low\" or \"high\"");
    }
    auto path = core::utf8::to_wide(a->path);
    if (!path || path->empty()) {
        return core::make_error(core::Code::InvalidArgument, "arguments: \"path\" is empty or not UTF-8");
    }
    return security::Request{.tool = "image.view", .risk = security::Risk::Read, .paths = {std::move(*path)}, .working_dir = wd};
}

// The picture file the Permit names, decoded (scaled to `max_side` while it is read).
struct Opened {
    std::wstring path;
    Picture picture;
};
auto open_picture(std::string_view tool, const security::Permit& permit, int max_side) -> std::expected<Opened, Outcome> {
    auto file = guarded_fs::open_read(permit, permit.paths().empty() ? std::wstring{} : permit.paths()[0].path);
    if (!file) {
        return std::unexpected{failure(tool, file.error())};
    }
    if (file->directory) {
        return std::unexpected{failure(tool, core::Error{core::Code::InvalidArgument, utf8(file->path) + " is a folder"})};
    }
    auto bytes = guarded_fs::read(*file, kMaxFile);
    if (!bytes) {
        return std::unexpected{failure(tool, bytes.error())};
    }
    if (bytes->size > bytes->data.size()) {
        return std::unexpected{failure(tool, core::Error{core::Code::InvalidArgument, "the picture is larger than 50 MB"})};
    }
    auto picture = image::decode(std::as_bytes(std::span{bytes->data.data(), bytes->data.size()}), max_side);
    if (!picture) {
        // Not a picture by its name either: said as such (phase 10, screen 25), a program above all.
        auto e = picture.error();
        const std::filesystem::path name{file->path};
        const auto ext = lower_ext(name);
        if (!image_ext(ext)) {
            e.kind = "not_an_image";
            const bool program = ext == ".exe" || ext == ".scr" || ext == ".com" || ext == ".bat" || ext == ".cmd" || ext == ".msi" ||
                                 ext == ".pif" || ext == ".lnk" || ext == ".js" || ext == ".vbs" || ext == ".ps1";
            e.facts = program ? (image_ext(lower_ext(name.stem())) ? "a program disguised as a photo" : "a program, not a picture")
                              : std::format("a {} file, not a picture", ext.empty() ? std::string{"nameless"} : ext);
        }
        return std::unexpected{failure(tool, e)};
    }
    return Opened{.path = file->path, .picture = std::move(*picture)};
}

auto path_request(std::string_view tool, const std::string& path_utf8, const std::filesystem::path& wd) -> core::Result<security::Request> {
    auto path = core::utf8::to_wide(path_utf8);
    if (!path || path->empty()) {
        return core::make_error(core::Code::InvalidArgument, "arguments: \"path\" is empty or not UTF-8");
    }
    return security::Request{.tool = std::string{tool}, .risk = security::Risk::Read, .paths = {std::move(*path)}, .working_dir = wd};
}

auto view_run(const security::Permit& permit, std::string_view json) -> Outcome {
    const auto a = parse<args::View>(json);
    if (!a) {
        return failure("image.view", a.error());
    }
    const bool high = a->detail == "high";
    auto opened = open_picture("image.view", permit, high ? image::kHighSide : image::kLowSide);
    if (!opened) {
        return std::move(opened.error());
    }
    const auto leaf = utf8(std::filesystem::path{opened->path}.filename().wstring());
    auto shown = image::for_model(opened->picture, high, leaf);
    if (!shown) {
        return failure("image.view", shown.error());
    }
    shown->path = utf8(opened->path);
    const out::Attached data{.width = shown->width, .height = shown->height, .format = shown->mime == "image/png" ? "png" : "jpeg"};
    auto data_json = core::json::write(data);
    Outcome o{0, wrap("image.view", opened->path, data_json ? *data_json : std::string{"{}"}, false), {}};
    o.images.push_back(std::move(*shown));
    return o;
}

namespace {

constexpr std::size_t kMaxConverted = 100;

struct Written {
    ImageFormat format = ImageFormat::Png;
    std::string_view ext; // with its dot
};

// What a format name, or a source's own extension, is written as.
auto written_as(std::string_view name) -> std::optional<Written> {
    if (name == "jpeg" || name == ".jpg" || name == ".jpeg") return Written{ImageFormat::Jpeg, ".jpg"};
    if (name == "png" || name == ".png") return Written{ImageFormat::Png, ".png"};
    if (name == "bmp" || name == ".bmp") return Written{ImageFormat::Bmp, ".bmp"};
    if (name == "tiff" || name == ".tif" || name == ".tiff") return Written{ImageFormat::Tiff, ".tif"};
    if (name == "gif" || name == ".gif") return Written{ImageFormat::Gif, ".gif"};
    return std::nullopt;
}

// A file's format as people name it, by its extension: ".jpg" -> "JPEG".
auto format_name(std::string_view ext) -> std::string {
    if (ext == ".jpg" || ext == ".jpeg") return "JPEG";
    if (ext == ".tif" || ext == ".tiff") return "TIFF";
    std::string out{ext.empty() ? ext : ext.substr(1)};
    std::ranges::transform(out, out.begin(), [](char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); });
    return out;
}

} // namespace

auto convert_schema() -> std::string {
    auto s = glz::write_json_schema<args::Convert>();
    return s ? std::move(*s) : std::string{"{}"};
}

// Each picture and the file it becomes, decided here so the policy sees both (phase 10, screen 21):
// in a folder "converted" beside it (or the folder asked), "<stem>_<max_side>.<ext>" with a size,
// "<stem>.<ext>" without; a name taken (on disk or earlier in the batch) gets " (2)", " (3)"…
auto convert_request(std::string_view json, const std::filesystem::path& wd) -> core::Result<security::Request> {
    auto a = parse<args::Convert>(json);
    if (!a) {
        return std::unexpected{a.error()};
    }
    if (a->paths.empty() || a->paths.size() > kMaxConverted) {
        return core::make_error(core::Code::InvalidArgument, "arguments: \"paths\" names 1 to 100 pictures");
    }
    if (a->format && !written_as(*a->format)) {
        return core::make_error(core::Code::InvalidArgument, "arguments: \"format\" is jpeg, png, bmp, tiff or gif (Windows cannot write WebP)");
    }
    if (a->rotate && *a->rotate != 0 && *a->rotate != 90 && *a->rotate != 180 && *a->rotate != 270) {
        return core::make_error(core::Code::InvalidArgument, "arguments: \"rotate\" is 0, 90, 180 or 270");
    }
    if ((a->quality && (*a->quality < 1 || *a->quality > 100)) || (a->max_side && *a->max_side < 16)) {
        return core::make_error(core::Code::InvalidArgument, "arguments: \"quality\" is 1-100, \"max_side\" at least 16");
    }
    security::Request r{.tool = "image.convert", .risk = security::Risk::Write, .working_dir = wd};
    std::vector<std::filesystem::path> taken;
    for (const auto& p : a->paths) {
        const auto wide = core::utf8::to_wide(p);
        if (!wide || wide->empty()) {
            return core::make_error(core::Code::InvalidArgument, "arguments: a path is empty or not UTF-8");
        }
        const std::filesystem::path source = std::filesystem::path{*wide}.is_absolute() ? std::filesystem::path{*wide} : wd / *wide;
        std::filesystem::path folder = source.parent_path() / L"converted";
        if (a->folder) {
            const auto f = core::utf8::to_wide(*a->folder);
            if (!f) {
                return core::make_error(core::Code::InvalidArgument, "arguments: \"folder\" is not UTF-8");
            }
            folder = std::filesystem::path{*f}.is_absolute() ? std::filesystem::path{*f} : wd / *f;
        }
        const auto own = written_as(lower_ext(source));
        const auto out = a->format ? *written_as(*a->format) : own.value_or(Written{ImageFormat::Png, ".png"});
        std::wstring name = source.stem().wstring();
        if (a->max_side) {
            name += L"_" + std::to_wstring(*a->max_side);
        }
        const std::wstring ext{out.ext.begin(), out.ext.end()};
        std::filesystem::path target = folder / (name + ext);
        for (int k = 2; std::filesystem::exists(target) || std::ranges::find(taken, target) != taken.end(); ++k) {
            target = folder / (name + L" (" + std::to_wstring(k) + L")" + ext);
        }
        taken.push_back(target);
        r.paths.push_back(source.wstring());
        r.paths.push_back(target.wstring());
    }
    return r;
}

auto convert_run(const security::Permit& permit, std::string_view json) -> Outcome {
    const auto a = parse<args::Convert>(json);
    if (!a) {
        return failure("image.convert", a.error());
    }
    out::Converted data;
    Conversion conversion{.format = a->format.value_or(""), .max_side = a->max_side.value_or(0), .quality = a->quality.value_or(85), .folder = {},
                          .files = {}};
    std::vector<Effect> effects;
    const std::size_t total = permit.paths().size() / 2;
    for (std::size_t i = 0; i + 1 < permit.paths().size(); i += 2) {
        const auto stop = [&](const core::Error& e) {
            std::vector<std::wstring> rest;
            for (std::size_t k = i + 2; k + 1 < permit.paths().size(); k += 2) {
                rest.push_back(permit.paths()[k].logical);
            }
            auto done = core::json::write(data.made);
            auto o = partial("image.convert", e, done ? *done : std::string{"[]"}, permit.paths()[i].logical, rest);
            o.effects = std::move(effects);
            o.conversion = std::move(conversion);
            return o;
        };
        if (!proceed()) {
            return stop(stopped_error());
        }
        // The source as the policy decided on it (a change permit: read as fs.copy reads), by its final path.
        auto from = guarded_fs::target(permit, i);
        if (!from) {
            return stop(from.error());
        }
        std::error_code ec;
        const auto size = std::filesystem::file_size(std::filesystem::path{*from}, ec);
        if (ec) {
            return stop(core::Error{core::Code::NotFound, utf8(*from) + " cannot be read"});
        }
        if (size > kMaxFile) {
            return stop(core::Error{core::Code::InvalidArgument, "the picture is larger than 50 MB"});
        }
        std::ifstream in{std::filesystem::path{*from}, std::ios::binary};
        const std::string bytes{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
        auto picture = image::decode(std::as_bytes(std::span{bytes.data(), bytes.size()}));
        if (!picture) {
            return stop(picture.error());
        }
        ConvertedFile row{.from = *from, .to = {}, .from_format = format_name(lower_ext(std::filesystem::path{*from})), .to_format = {},
                          .from_w = 0, .from_h = 0, .to_w = 0, .to_h = 0, .from_bytes = size, .to_bytes = 0, .rotated = 0, .already = false,
                          .thumb = {}};
        const int seen_w = picture->source_width > 0 ? picture->source_width : picture->width;
        const int seen_h = picture->source_height > 0 ? picture->source_height : picture->height;
        const bool sideways = picture->turned == 90 || picture->turned == 270; // as stored: the sides back
        row.from_w = sideways ? seen_h : seen_w;
        row.from_h = sideways ? seen_w : seen_h;
        row.already = a->max_side && std::max(picture->width, picture->height) <= *a->max_side;
        row.rotated = (picture->turned + a->rotate.value_or(0)) % 360;
        Picture done = a->max_side ? image::fit(*picture, *a->max_side) : std::move(*picture);
        done = image::rotate(done, a->rotate.value_or(0));
        auto to = guarded_fs::target(permit, i + 1);
        if (!to) {
            return stop(to.error());
        }
        const auto own = written_as(lower_ext(std::filesystem::path{*to}));
        auto encoded = image::encode(done, own ? own->format : ImageFormat::Png, a->quality.value_or(85));
        if (!encoded) {
            return stop(encoded.error());
        }
        // Its folder ("converted") made the first time: in the change log before what goes in it.
        const auto folder = std::filesystem::path{*to}.parent_path();
        if (std::error_code missing; !std::filesystem::exists(folder, missing)) {
            if (auto made = os::current().make_dir(folder.wstring()); !made) {
                return stop(made.error());
            }
            effects.push_back({.kind = EffectKind::Created, .from = {}, .to = folder.wstring(), .undoable = true});
        }
        if (auto written = os::current().write_file(*to, *encoded, os::WriteMode::CreateOnly); !written) {
            return stop(written.error());
        }
        effects.push_back({.kind = EffectKind::Created, .from = {}, .to = *to, .undoable = true}); // undo: to the Recycle Bin
        data.made.push_back(utf8(*to));
        data.files.push_back({.from = utf8(*from), .to = utf8(*to), .width = done.width, .height = done.height, .bytes = encoded->size(),
                              .from_bytes = size, .rotated = row.rotated, .already_small = row.already});
        data.bytes_before += size;
        data.bytes_after += encoded->size();
        if (data.folder.empty()) {
            data.folder = utf8(folder.wstring());
            conversion.folder = folder.wstring();
        }
        row.to = *to;
        row.to_format = format_name(lower_ext(std::filesystem::path{*to}));
        row.to_w = done.width;
        row.to_h = done.height;
        row.to_bytes = encoded->size();
        row.thumb = image::fit_box(done, 80, 80);
        conversion.files.push_back(std::move(row));
        report(i / 2 + 1, total, permit.paths()[i].logical, encoded->size());
    }
    auto data_json = core::json::write(data);
    Outcome o{0, wrap("image.convert", {}, data_json ? *data_json : std::string{"{}"}, false), std::move(effects)};
    o.conversion = std::move(conversion);
    return o;
}

auto capture_schema() -> std::string {
    auto s = glz::write_json_schema<args::Capture>();
    return s ? std::move(*s) : std::string{"{}"};
}

auto capture_request(std::string_view json, const std::filesystem::path& wd) -> core::Result<security::Request> {
    auto a = parse<args::Capture>(json);
    if (!a) {
        return std::unexpected{a.error()};
    }
    if (a->target != "screen" && a->target != "window" && a->target != "active") {
        return core::make_error(core::Code::InvalidArgument, "arguments: \"target\" is screen, window or active");
    }
    if (a->target == "window" && (!a->window || a->window->empty())) {
        return core::make_error(core::Code::InvalidArgument, "arguments: \"window\" names a part of the window's title");
    }
    return security::Request{.tool = "screen.capture", .risk = security::Risk::Screen, .working_dir = wd};
}

namespace {

// What the question shows of a capture: the model's picture, the panel's larger one, and around it.
auto prepared_of(core::Result<os::Captured> shot) -> core::Result<Prepared> {
    if (!shot) {
        return std::unexpected{shot.error()};
    }
    auto shown = image::for_model(shot->picture, true, shot->title); // a screen's small text: high
    if (!shown) {
        return std::unexpected{shown.error()};
    }
    Prepared p{.image = std::move(*shown), .title = shot->title};
    p.preview = image::fit_box(shot->picture, 864, 432);
    p.source = shot->source.empty() ? shot->title : shot->source;
    p.monitors = shot->monitors;
    p.windows = std::move(shot->windows);
    if (shot->hole_w > 0 && shot->picture.width > 0 && shot->picture.height > 0) {
        const auto pw = static_cast<float>(shot->picture.width), ph = static_cast<float>(shot->picture.height);
        p.hole_x = static_cast<float>(shot->hole_x) / pw;
        p.hole_y = static_cast<float>(shot->hole_y) / ph;
        p.hole_w = static_cast<float>(shot->hole_w) / pw;
        p.hole_h = static_cast<float>(shot->hole_h) / ph;
    }
    return p;
}

} // namespace

auto capture_prepare(std::string_view json) -> core::Result<Prepared> {
    auto a = parse<args::Capture>(json);
    if (!a) {
        return std::unexpected{a.error()};
    }
    const auto target = a->target == "window" ? os::CaptureTarget::Window
                        : a->target == "active" ? os::CaptureTarget::Active
                                                : os::CaptureTarget::Screen;
    const auto part = core::utf8::to_wide(a->window.value_or(""));
    return prepared_of(os::current().capture({.target = target, .window = part ? *part : std::wstring{}, .monitor = 0}));
}

auto capture_prepare_again(std::string_view json, const Retake& retake) -> core::Result<Prepared> {
    switch (retake.what) {
    case Retake::What::Monitor: return prepared_of(os::current().capture({.target = os::CaptureTarget::Monitor, .window = {}, .monitor = retake.monitor}));
    case Retake::What::All: return prepared_of(os::current().capture({.target = os::CaptureTarget::All, .window = {}, .monitor = 0}));
    case Retake::What::Window: {
        const auto part = core::utf8::to_wide(retake.window);
        return prepared_of(os::current().capture({.target = os::CaptureTarget::Window, .window = part ? *part : std::wstring{}, .monitor = 0}));
    }
    case Retake::What::Same: break;
    }
    return capture_prepare(json);
}

auto capture_run(const security::Permit&, std::string_view) -> Outcome {
    return failure("screen.capture", core::Error{core::Code::Internal, "nothing was captured"});
}

auto capture_run_prepared(const security::Permit& permit, std::string_view, Prepared&& prepared) -> Outcome {
    if (permit.risk() != security::Risk::Screen || !prepared.image) {
        return failure("screen.capture", core::Error{core::Code::Policy, "not a permit for the screen"});
    }
    const out::Attached data{.width = prepared.image->width, .height = prepared.image->height,
                             .format = prepared.image->mime == "image/png" ? "png" : "jpeg"};
    auto data_json = core::json::write(data);
    Outcome o{0, wrap("screen.capture", {}, data_json ? *data_json : std::string{"{}"}, false), {}};
    o.images.push_back(std::move(*prepared.image));
    return o;
}

auto ocr_schema() -> std::string {
    auto s = glz::write_json_schema<args::Ocr>();
    return s ? std::move(*s) : std::string{"{}"};
}

auto ocr_request(std::string_view json, const std::filesystem::path& wd) -> core::Result<security::Request> {
    auto a = parse<args::Ocr>(json);
    if (!a) {
        return std::unexpected{a.error()};
    }
    return path_request("image.ocr", a->path, wd);
}

auto ocr_run(const security::Permit& permit, std::string_view json) -> Outcome {
    const auto a = parse<args::Ocr>(json);
    if (!a) {
        return failure("image.ocr", a.error());
    }
    auto opened = open_picture("image.ocr", permit, 0);
    if (!opened) {
        return std::move(opened.error());
    }
    auto read = ocr::recognize(opened->picture, a->language.value_or(""));
    if (!read) {
        return failure("image.ocr", read.error());
    }
    out::Read data{.language = read->language, .text = {}, .line_count = read->lines.size(), .doubtful = {}, .angle = read->angle};
    bool cut = false;
    for (std::size_t n = 0; n < read->lines.size(); ++n) {
        const auto& line = read->lines[n];
        if (data.text.size() + line.text.size() + 1 > kMaxOutput - 4096) { // the envelope fits 64 KB, the doubtful words too
            cut = true;
            break;
        }
        data.text += line.text;
        data.text += '\n';
        for (const auto& w : line.words) {
            if (w.doubtful && data.doubtful.size() < 50) {
                data.doubtful.push_back({.line = n + 1, .word = w.text});
            }
        }
    }
    auto data_json = core::json::write(data);
    Outcome o{0, wrap("image.ocr", opened->path, data_json ? *data_json : std::string{"{}"}, cut), {}};
    // For the panel (phase 10, screen 20): the picture smaller, its lines' boxes in its own pixels.
    o.ocr = OcrPage{.path = opened->path, .picture = image::fit(opened->picture, 640), .width = opened->picture.width,
                    .height = opened->picture.height, .text = std::move(*read), .languages = ocr::profile()};
    return o;
}

} // namespace tools::pictures
