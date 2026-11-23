#include "tools/image.hpp"

#include <windows.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <unordered_set>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shlwapi.lib")

namespace tools::image {
namespace {

using Microsoft::WRL::ComPtr;

// COM for the call: the multithreaded apartment unless the thread has one already (WIC works in either).
struct Com {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Com() = default;
    Com(const Com&) = delete;
    auto operator=(const Com&) -> Com& = delete;
    ~Com() {
        if (hr == S_OK || hr == S_FALSE) {
            CoUninitialize();
        }
    }
};

auto failed(std::string_view what, HRESULT hr) -> core::Error {
    return core::Error{core::Code::Io, std::format("{} (0x{:08X})", what, static_cast<unsigned>(hr))};
}

auto factory() -> core::Result<ComPtr<IWICImagingFactory>> {
    ComPtr<IWICImagingFactory> f;
    if (const HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)); FAILED(hr)) {
        return std::unexpected{failed("Windows' imaging is not available", hr)};
    }
    return f;
}

// A JPEG's end marker after its first scan: the segments before it are stepped over by their
// lengths (an EXIF thumbnail in APP1 ends with a marker of its own); the scan's data never holds
// FF D9 (its FF bytes are stuffed), and what follows the end (a motion photo's clip) is not read.
auto jpeg_ends(std::string_view b) -> bool {
    const auto at = [&](std::size_t i) { return static_cast<unsigned char>(b[i]); };
    std::size_t i = 2; // after SOI
    while (i + 4 <= b.size() && at(i) == 0xFF) {
        const auto marker = at(i + 1);
        if (marker == 0xDA) { // the first scan
            return b.find("\xFF\xD9", i + 2) != std::string_view::npos;
        }
        if (marker == 0xFF) { // a fill byte
            ++i;
        } else if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) { // no length
            i += 2;
        } else {
            i += 2 + ((static_cast<std::size_t>(at(i + 2)) << 8) | at(i + 3));
        }
    }
    return false; // no scan before the end: cut short
}

// The EXIF orientation (1..8) of a frame; 1 when it has none.
auto orientation_of(IWICBitmapFrameDecode* frame) -> int {
    ComPtr<IWICMetadataQueryReader> reader;
    if (FAILED(frame->GetMetadataQueryReader(&reader))) {
        return 1;
    }
    for (const wchar_t* name : {L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}"}) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(reader->GetMetadataByName(name, &v))) {
            const int o = v.vt == VT_UI2 ? v.uiVal : 1;
            PropVariantClear(&v);
            return o >= 1 && o <= 8 ? o : 1;
        }
    }
    return 1;
}

auto rotator_options(int orientation) -> WICBitmapTransformOptions {
    switch (orientation) {
    case 2: return WICBitmapTransformFlipHorizontal;
    case 3: return WICBitmapTransformRotate180;
    case 4: return WICBitmapTransformFlipVertical;
    case 5: return static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal);
    case 6: return WICBitmapTransformRotate90;
    case 7: return static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal);
    case 8: return WICBitmapTransformRotate270;
    default: return WICBitmapTransformRotate0;
    }
}

// The pixels of `source`, as 32bppBGRA.
auto pixels_of(IWICImagingFactory* f, IWICBitmapSource* source) -> core::Result<Picture> {
    ComPtr<IWICFormatConverter> converter;
    HRESULT hr = f->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr)) {
        hr = converter->Initialize(source, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom);
    }
    UINT w = 0, h = 0;
    if (SUCCEEDED(hr)) {
        hr = converter->GetSize(&w, &h);
    }
    if (FAILED(hr)) {
        return std::unexpected{failed("the picture cannot be read", hr)};
    }
    Picture p{.width = static_cast<int>(w), .height = static_cast<int>(h), .bgra = {}};
    p.bgra.resize(static_cast<std::size_t>(w) * h * 4);
    if (hr = converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(p.bgra.size()), p.bgra.data()); FAILED(hr)) {
        return std::unexpected{failed("the picture is damaged or cut short", hr)};
    }
    return p;
}

auto bitmap_of(IWICImagingFactory* f, const Picture& p) -> core::Result<ComPtr<IWICBitmap>> {
    ComPtr<IWICBitmap> bitmap;
    const HRESULT hr = f->CreateBitmapFromMemory(static_cast<UINT>(p.width), static_cast<UINT>(p.height), GUID_WICPixelFormat32bppBGRA,
                                                 static_cast<UINT>(p.width) * 4, static_cast<UINT>(p.bgra.size()),
                                                 const_cast<BYTE*>(p.bgra.data()), &bitmap);
    if (FAILED(hr)) {
        return std::unexpected{failed("the picture cannot be held", hr)};
    }
    return bitmap;
}

auto scaled(const Picture& p, int w, int h) -> Picture {
    if (w == p.width && h == p.height) {
        return p;
    }
    const Com com;
    auto f = factory();
    auto bitmap = f ? bitmap_of(f->Get(), p) : core::Result<ComPtr<IWICBitmap>>{std::unexpected{f.error()}};
    ComPtr<IWICBitmapScaler> scaler;
    if (!bitmap || FAILED((*f)->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(bitmap->Get(), static_cast<UINT>(w), static_cast<UINT>(h), WICBitmapInterpolationModeFant))) {
        return p;
    }
    auto out = pixels_of(f->Get(), scaler.Get());
    return out ? std::move(*out) : p;
}

auto read_stream(IStream* stream) -> core::Result<std::string> {
    STATSTG stat{};
    HRESULT hr = stream->Stat(&stat, STATFLAG_NONAME);
    std::string out;
    if (SUCCEEDED(hr)) {
        out.resize(static_cast<std::size_t>(stat.cbSize.QuadPart));
        const LARGE_INTEGER zero{};
        hr = stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    }
    ULONG read = 0;
    if (SUCCEEDED(hr)) {
        hr = stream->Read(out.data(), static_cast<ULONG>(out.size()), &read);
    }
    if (FAILED(hr)) {
        return std::unexpected{failed("the picture could not be written", hr)};
    }
    out.resize(read);
    return out;
}

auto container_of(ImageFormat format) -> GUID {
    switch (format) {
    case ImageFormat::Jpeg: return GUID_ContainerFormatJpeg;
    case ImageFormat::Png: return GUID_ContainerFormatPng;
    case ImageFormat::Bmp: return GUID_ContainerFormatBmp;
    case ImageFormat::Tiff: return GUID_ContainerFormatTiff;
    case ImageFormat::Gif: return GUID_ContainerFormatGif;
    }
    return GUID_ContainerFormatPng;
}

// Few colours (a screenshot, a diagram) is better as PNG; a photo as JPEG.
auto few_colours(const Picture& p) -> bool {
    const std::size_t pixels = static_cast<std::size_t>(p.width) * p.height;
    const std::size_t step = std::max<std::size_t>(1, pixels / 65536);
    std::unordered_set<std::uint32_t> seen;
    for (std::size_t i = 0; i < pixels; i += step) {
        const auto* px = &p.bgra[i * 4];
        seen.insert(static_cast<std::uint32_t>(px[0]) | (static_cast<std::uint32_t>(px[1]) << 8) | (static_cast<std::uint32_t>(px[2]) << 16));
        if (seen.size() > 256) {
            return false;
        }
    }
    return true;
}

} // namespace

auto decode(std::span<const std::byte> file, int max_side) -> core::Result<Picture> {
    if (file.empty()) {
        return core::make_error(core::Code::InvalidArgument, "the file is empty");
    }
    const Com com;
    auto f = factory();
    if (!f) {
        return std::unexpected{f.error()};
    }
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    HRESULT hr = (*f)->CreateStream(&stream);
    if (SUCCEEDED(hr)) {
        hr = stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<std::byte*>(file.data())), static_cast<DWORD>(file.size()));
    }
    if (SUCCEEDED(hr)) {
        hr = (*f)->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder);
    }
    if (SUCCEEDED(hr)) {
        hr = decoder->GetFrame(0, &frame);
    }
    UINT w = 0, h = 0;
    if (SUCCEEDED(hr)) {
        hr = frame->GetSize(&w, &h);
    }
    if (FAILED(hr) || w == 0 || h == 0) {
        return std::unexpected{failed("not a picture Windows can read (no codec for it, or damaged)", hr)};
    }
    // WIC fills a PNG or JPEG cut short with grey and says nothing: its end marker tells — after
    // the picture's own data, wherever the file ends (a phone's motion photo appends its clip).
    GUID container{};
    if (SUCCEEDED(decoder->GetContainerFormat(&container))) {
        const std::string_view bytes{reinterpret_cast<const char*>(file.data()), file.size()};
        const bool cut = (container == GUID_ContainerFormatPng && bytes.find("IEND", 8) == std::string_view::npos) ||
                         (container == GUID_ContainerFormatJpeg && !jpeg_ends(bytes));
        if (cut) {
            return core::make_error(core::Code::Io, "the picture is cut short (the file is not whole)");
        }
    }
    // Scaled while decoding: to `max_side`, and never past kMaxPixels.
    double scale = 1.0;
    if (max_side > 0) {
        scale = std::min(scale, static_cast<double>(max_side) / static_cast<double>(std::max(w, h)));
    }
    scale = std::min(scale, std::sqrt(static_cast<double>(kMaxPixels) / (static_cast<double>(w) * static_cast<double>(h))));
    ComPtr<IWICBitmapSource> source = frame;
    if (scale < 1.0) {
        const auto tw = static_cast<UINT>(std::max(1.0, std::floor(w * scale)));
        const auto th = static_cast<UINT>(std::max(1.0, std::floor(h * scale)));
        ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(hr = (*f)->CreateBitmapScaler(&scaler)) ||
            FAILED(hr = scaler->Initialize(source.Get(), tw, th, WICBitmapInterpolationModeFant))) {
            return std::unexpected{failed("the picture cannot be scaled", hr)};
        }
        source = scaler;
    }
    const auto turn = rotator_options(orientation_of(frame.Get()));
    const auto quarter = turn & 3; // the rotation's part (90 = 1, 180 = 2, 270 = 3); flips are higher bits
    const bool sideways = quarter == WICBitmapTransformRotate90 || quarter == WICBitmapTransformRotate270;
    if (turn != WICBitmapTransformRotate0) {
        ComPtr<IWICBitmapFlipRotator> rotator;
        if (SUCCEEDED((*f)->CreateBitmapFlipRotator(&rotator)) && SUCCEEDED(rotator->Initialize(source.Get(), turn))) {
            source = rotator;
        }
    }
    auto out = pixels_of(f->Get(), source.Get());
    if (out) {
        out->source_width = static_cast<int>(sideways ? h : w);
        out->source_height = static_cast<int>(sideways ? w : h);
        out->turned = quarter == WICBitmapTransformRotate90 ? 90 : quarter == WICBitmapTransformRotate180 ? 180 : quarter == WICBitmapTransformRotate270 ? 270 : 0;
    }
    return out;
}

auto fit(const Picture& picture, int max_side) -> Picture {
    const int longer = std::max(picture.width, picture.height);
    if (max_side <= 0 || longer <= max_side) {
        return picture;
    }
    const double s = static_cast<double>(max_side) / longer;
    return scaled(picture, std::max(1, static_cast<int>(std::lround(picture.width * s))),
                  std::max(1, static_cast<int>(std::lround(picture.height * s))));
}

auto fit_box(const Picture& picture, int width, int height) -> Picture {
    if (picture.width <= 0 || picture.height <= 0) {
        return picture;
    }
    const double s = std::min({1.0, static_cast<double>(width) / picture.width, static_cast<double>(height) / picture.height});
    return scaled(picture, std::max(1, static_cast<int>(std::lround(picture.width * s))),
                  std::max(1, static_cast<int>(std::lround(picture.height * s))));
}

auto rotate(const Picture& picture, int degrees) -> Picture {
    const int turns = ((degrees / 90) % 4 + 4) % 4;
    if (turns == 0) {
        return picture;
    }
    const int w = picture.width, h = picture.height;
    Picture out{.width = turns == 2 ? w : h, .height = turns == 2 ? h : w, .bgra = std::vector<std::uint8_t>(picture.bgra.size())};
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int nx = x, ny = y;
            if (turns == 1) { // clockwise: (x, y) -> (h-1-y, x)
                nx = h - 1 - y;
                ny = x;
            } else if (turns == 2) {
                nx = w - 1 - x;
                ny = h - 1 - y;
            } else {
                nx = y;
                ny = w - 1 - x;
            }
            std::copy_n(&picture.bgra[(static_cast<std::size_t>(y) * w + x) * 4], 4,
                        &out.bgra[(static_cast<std::size_t>(ny) * out.width + nx) * 4]);
        }
    }
    return out;
}

auto encode(const Picture& given, ImageFormat format, int quality) -> core::Result<std::string> {
    if (given.width <= 0 || given.height <= 0) {
        return core::make_error(core::Code::InvalidArgument, "an empty picture");
    }
    // JPEG and GIF keep no transparency: dropped, a clear pixel showed its colour — black, as
    // editors leave it (phase 9's deferred item). Such a picture stands on white instead.
    std::optional<Picture> flat;
    const bool opaque_only = format == ImageFormat::Jpeg || format == ImageFormat::Gif;
    if (opaque_only) {
        for (std::size_t i = 3; i < given.bgra.size(); i += 4) {
            if (given.bgra[i] != 255) {
                flat = given;
                break;
            }
        }
    }
    if (flat) {
        for (std::size_t i = 0; i + 3 < flat->bgra.size(); i += 4) {
            const unsigned a = flat->bgra[i + 3];
            for (std::size_t c = 0; c < 3; ++c) {
                flat->bgra[i + c] = static_cast<std::uint8_t>((flat->bgra[i + c] * a + 255u * (255u - a) + 127u) / 255u);
            }
            flat->bgra[i + 3] = 255;
        }
    }
    const Picture& picture = flat ? *flat : given;
    const Com com;
    auto f = factory();
    if (!f) {
        return std::unexpected{f.error()};
    }
    auto bitmap = bitmap_of(f->Get(), picture);
    if (!bitmap) {
        return std::unexpected{bitmap.error()};
    }
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(nullptr, 0)); // its one reference, owned
    if (!stream) {
        return core::make_error(core::Code::Io, "no memory for the picture");
    }
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    HRESULT hr = (*f)->CreateEncoder(container_of(format), nullptr, &encoder);
    if (SUCCEEDED(hr)) {
        hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    }
    if (SUCCEEDED(hr)) {
        hr = encoder->CreateNewFrame(&frame, &props);
    }
    if (SUCCEEDED(hr) && format == ImageFormat::Jpeg && props) {
        PROPBAG2 option{};
        option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = static_cast<float>(std::clamp(quality, 1, 100)) / 100.0f;
        hr = props->Write(1, &option, &v);
    }
    if (SUCCEEDED(hr)) {
        hr = frame->Initialize(props.Get());
    }
    if (SUCCEEDED(hr)) {
        hr = frame->SetSize(static_cast<UINT>(picture.width), static_cast<UINT>(picture.height));
    }
    ComPtr<IWICBitmapSource> source = *bitmap;
    if (SUCCEEDED(hr) && format == ImageFormat::Gif) { // a palette of its own 256 colours
        ComPtr<IWICPalette> palette;
        ComPtr<IWICFormatConverter> indexed;
        hr = (*f)->CreatePalette(&palette);
        if (SUCCEEDED(hr)) {
            hr = palette->InitializeFromBitmap(bitmap->Get(), 256, FALSE);
        }
        if (SUCCEEDED(hr)) {
            hr = (*f)->CreateFormatConverter(&indexed);
        }
        if (SUCCEEDED(hr)) {
            hr = indexed->Initialize(bitmap->Get(), GUID_WICPixelFormat8bppIndexed, WICBitmapDitherTypeErrorDiffusion, palette.Get(), 0.0,
                                     WICBitmapPaletteTypeCustom);
        }
        if (SUCCEEDED(hr)) {
            hr = frame->SetPalette(palette.Get());
        }
        source = indexed;
    }
    if (SUCCEEDED(hr)) {
        WICPixelFormatGUID wanted = format == ImageFormat::Jpeg  ? GUID_WICPixelFormat24bppBGR
                                    : format == ImageFormat::Gif ? GUID_WICPixelFormat8bppIndexed
                                                                 : GUID_WICPixelFormat32bppBGRA;
        hr = frame->SetPixelFormat(&wanted);
    }
    if (SUCCEEDED(hr)) {
        hr = frame->WriteSource(source.Get(), nullptr); // converts to the frame's format as it writes
    }
    if (SUCCEEDED(hr)) {
        hr = frame->Commit();
    }
    if (SUCCEEDED(hr)) {
        hr = encoder->Commit();
    }
    if (FAILED(hr)) {
        return std::unexpected{failed("the picture could not be written in that format", hr)};
    }
    return read_stream(stream.Get());
}

auto for_model(const Picture& picture, bool high, std::string label, std::size_t max_bytes) -> core::Result<ImageOut> {
    Picture fitted = fit(picture, high ? kHighSide : kLowSide);
    const bool png = few_colours(fitted);
    for (int attempt = 0; attempt < 8; ++attempt) {
        for (const int quality : {85, 70, 55}) {
            auto bytes = encode(fitted, png ? ImageFormat::Png : ImageFormat::Jpeg, quality);
            if (!bytes) {
                return std::unexpected{bytes.error()};
            }
            if (bytes->size() <= max_bytes) {
                ImageOut out{.mime = png ? "image/png" : "image/jpeg", .bytes = std::move(*bytes), .label = std::move(label), .path = {},
                             .width = fitted.width, .height = fitted.height, .thumb = fit_box(fitted, 240, 120),
                             .source_width = picture.source_width > 0 ? picture.source_width : picture.width,
                             .source_height = picture.source_height > 0 ? picture.source_height : picture.height};
                return out;
            }
            if (png) {
                break; // PNG has no quality: smaller then
            }
        }
        fitted = fit(fitted, std::max(64, static_cast<int>(std::max(fitted.width, fitted.height) * 0.75)));
    }
    return core::make_error(core::Code::Io, "the picture does not get small enough for the model");
}

} // namespace tools::image
