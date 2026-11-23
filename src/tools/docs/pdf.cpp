#include "tools/docs/docs.hpp"

#include "core/utf8.hpp"
#include "core/win.hpp"

#include <windows.h>
#include <filter.h>
#include <filterr.h>
#include <ntquery.h>
#include <propsys.h>
#include <shlwapi.h>
#include <wrl/client.h>

#include <winrt/Windows.Data.Pdf.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Storage.h>

#include <algorithm>
#include <cmath>
#include <format>

#pragma comment(lib, "query.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "windowsapp.lib")

namespace tools::docs {
namespace {

namespace wdp = winrt::Windows::Data::Pdf;
namespace wss = winrt::Windows::Storage::Streams;

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

auto utf8(std::wstring_view w) -> std::string {
    auto s = core::utf8::to_utf8(w);
    return s ? std::move(*s) : std::string{};
}

auto document(const std::wstring& path) -> core::Result<wdp::PdfDocument> {
    core::win::keep_mta(); // before WinRT: its factories outlive a borrowed MTA (core/win.hpp)
    try {
        const auto file = winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(winrt::hstring{path}).get();
        auto doc = wdp::PdfDocument::LoadFromFileAsync(file).get();
        if (doc.IsPasswordProtected()) {
            return core::make_error(core::Code::AccessDenied, "encrypted or unreadable PDF");
        }
        return doc;
    } catch (const winrt::hresult_error&) {
        return core::make_error(core::Code::Parse, "encrypted or unreadable PDF");
    }
}

} // namespace

namespace {

// The filter's text pieces, each as it comes (a piece with no text stays, empty).
auto pieces(const std::wstring& path) -> core::Result<std::vector<std::wstring>> {
    const Com com;
    // Windows' own PDF filter (Windows.Data.Pdf.dll) takes a stream; an older one, the path.
    static constexpr CLSID kPdfFilter = {0x6C337B26, 0x3E38, 0x4F98, {0x81, 0x3B, 0xFB, 0xA1, 0x8B, 0xAB, 0x64, 0xF5}};
    Microsoft::WRL::ComPtr<IFilter> filter;
    HRESULT hr = CoCreateInstance(kPdfFilter, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&filter));
    if (SUCCEEDED(hr)) {
        Microsoft::WRL::ComPtr<IInitializeWithStream> init;
        Microsoft::WRL::ComPtr<IStream> stream;
        hr = filter.As(&init);
        if (SUCCEEDED(hr)) {
            hr = SHCreateStreamOnFileEx(path.c_str(), STGM_READ | STGM_SHARE_DENY_WRITE, 0, FALSE, nullptr, &stream);
        }
        if (SUCCEEDED(hr)) {
            hr = init->Initialize(stream.Get(), STGM_READ);
        }
        if (FAILED(hr)) {
            filter.Reset();
        }
    }
    if (!filter) {
        hr = LoadIFilter(path.c_str(), nullptr, reinterpret_cast<void**>(filter.GetAddressOf()));
    }
    if (FAILED(hr) || !filter) {
        return core::make_error(core::Code::NotFound, std::format("Windows' PDF filter is not available here (0x{:08X})", static_cast<unsigned>(hr)));
    }
    ULONG flags = 0;
    if (FAILED(filter->Init(IFILTER_INIT_CANON_PARAGRAPHS | IFILTER_INIT_CANON_HYPHENS | IFILTER_INIT_CANON_SPACES, 0, nullptr, &flags))) {
        return core::make_error(core::Code::Parse, "encrypted or unreadable PDF");
    }
    std::vector<std::wstring> out;
    std::size_t total = 0;
    STAT_CHUNK chunk{};
    for (HRESULT got = filter->GetChunk(&chunk); got != FILTER_E_END_OF_CHUNKS; got = filter->GetChunk(&chunk)) {
        if (FAILED(got)) {
            if (got == FILTER_E_EMBEDDING_UNAVAILABLE || got == FILTER_E_LINK_UNAVAILABLE) {
                continue;
            }
            break; // what was read stays
        }
        if ((chunk.flags & CHUNK_TEXT) == 0) {
            continue;
        }
        out.emplace_back(); // a piece a page: Windows' filter gives them so, its break type says nothing (phase 10)
        auto& text = out.back();
        wchar_t buf[4096];
        for (;;) {
            ULONG n = static_cast<ULONG>(std::size(buf));
            const HRESULT r = filter->GetText(&n, buf);
            if (FAILED(r) || n == 0) {
                break;
            }
            text.append(buf, n);
            total += n;
            if (r == FILTER_S_LAST_TEXT || total > (64u << 20)) {
                break;
            }
        }
        if (total > (64u << 20)) {
            break;
        }
    }
    return out;
}

} // namespace

auto pdf_text(const std::wstring& path) -> core::Result<std::string> {
    auto got = pieces(path);
    if (!got) {
        return std::unexpected{got.error()};
    }
    std::wstring text;
    for (const auto& p : *got) {
        if (!text.empty()) {
            text += L'\n';
        }
        text += p;
    }
    return utf8(text);
}

auto pdf_pages(const std::wstring& path) -> core::Result<std::vector<std::string>> {
    auto got = pieces(path);
    if (!got) {
        return std::unexpected{got.error()};
    }
    std::vector<std::string> out;
    out.reserve(got->size());
    for (const auto& p : *got) {
        out.push_back(utf8(p));
    }
    return out;
}

auto pdf_page_count(const std::wstring& path) -> core::Result<int> {
    auto doc = document(path);
    if (!doc) {
        return std::unexpected{doc.error()};
    }
    return static_cast<int>(doc->PageCount());
}

auto pdf_render(const std::wstring& path, int page, int dpi) -> core::Result<Picture> {
    auto doc = document(path);
    if (!doc) {
        return std::unexpected{doc.error()};
    }
    try {
        if (page < 0 || static_cast<std::uint32_t>(page) >= doc->PageCount()) {
            return core::make_error(core::Code::InvalidArgument, std::format("the PDF has no page {}", page + 1));
        }
        const auto p = doc->GetPage(static_cast<std::uint32_t>(page));
        const auto size = p.Size(); // in device-independent pixels, 96 to the inch
        // At `dpi`, but never past kMaxRenderSide: a hostile page may say it is metres wide.
        const double w = std::max(1.0, static_cast<double>(size.Width)), h = std::max(1.0, static_cast<double>(size.Height));
        // Past kMaxPageMeters a page is no paper: drawn within kMaxRenderSide its text would be dots,
        // so it is not drawn at all - its size said (phase 10, screen 25).
        const double mw = w / 96.0 * 0.0254, mh = h / 96.0 * 0.0254;
        if (std::max(mw, mh) > kMaxPageMeters) {
            const auto metres = [](double m) { return m >= 10 ? std::format("{:.0f}", m) : std::format("{:.1f}", m); };
            auto e = core::make_error(core::Code::InvalidArgument, std::format("page {} is {} x {} m: too large to draw", page + 1, metres(mw), metres(mh)));
            e.error().kind = "page_too_large";
            e.error().facts = std::format("page size {} \xC3\x97 {} m", metres(mw), metres(mh));
            return e;
        }
        const double scale = std::min(static_cast<double>(dpi) / 96.0, kMaxRenderSide / std::max(w, h));
        wdp::PdfPageRenderOptions options;
        options.DestinationWidth(static_cast<std::uint32_t>(std::clamp(std::lround(w * scale), 1L, static_cast<long>(kMaxRenderSide))));
        options.DestinationHeight(static_cast<std::uint32_t>(std::clamp(std::lround(h * scale), 1L, static_cast<long>(kMaxRenderSide))));
        wss::InMemoryRandomAccessStream stream;
        p.RenderToStreamAsync(stream, options).get();
        const auto bytes = static_cast<std::uint32_t>(stream.Size());
        wss::DataReader reader{stream.GetInputStreamAt(0)};
        reader.LoadAsync(bytes).get();
        std::vector<std::uint8_t> encoded(bytes);
        reader.ReadBytes(encoded);
        return image::decode(std::as_bytes(std::span{encoded.data(), encoded.size()}));
    } catch (const winrt::hresult_error& e) {
        return core::make_error(core::Code::Io, "the page could not be drawn: " + utf8(std::wstring_view{e.message()}));
    }
}

} // namespace tools::docs
