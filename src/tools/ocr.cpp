#include "tools/ocr.hpp"

#include "core/utf8.hpp"
#include "core/win.hpp"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.System.UserProfile.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <format>
#include <numbers>

#pragma comment(lib, "windowsapp.lib")

namespace tools::ocr {
namespace {

namespace wmo = winrt::Windows::Media::Ocr;
namespace wgi = winrt::Windows::Graphics::Imaging;

auto utf8(winrt::hstring const& s) -> std::string {
    auto u = core::utf8::to_utf8(std::wstring_view{s});
    return u ? std::move(*u) : std::string{};
}

auto installed_list() -> std::string {
    std::string out;
    for (const auto& tag : languages()) {
        out += out.empty() ? tag : ", " + tag;
    }
    return out.empty() ? std::string{"none"} : out;
}

auto engine_for(std::string_view language) -> core::Result<wmo::OcrEngine> {
    if (language.empty()) {
        if (auto e = wmo::OcrEngine::TryCreateFromUserProfileLanguages()) {
            return e;
        }
        const auto available = wmo::OcrEngine::AvailableRecognizerLanguages();
        if (available.Size() > 0) {
            if (auto e = wmo::OcrEngine::TryCreateFromLanguage(available.GetAt(0))) {
                return e;
            }
        }
        return core::make_error(core::Code::NotFound, "no OCR language is installed in Windows");
    }
    const auto wide = core::utf8::to_wide(language);
    if (wide) {
        const winrt::Windows::Globalization::Language wanted{winrt::hstring{*wide}};
        if (wmo::OcrEngine::IsLanguageSupported(wanted)) {
            if (auto e = wmo::OcrEngine::TryCreateFromLanguage(wanted)) {
                return e;
            }
        }
    }
    return core::make_error(core::Code::NotFound, std::format("no OCR language {}; installed: {}", language, installed_list()));
}

enum class Script : std::uint8_t { None, Latin, Cyrillic, Greek, Han, Kana, Hangul, Arabic, Other };

auto script_of(wchar_t c) -> Script {
    if ((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= 0x00C0 && c <= 0x024F && c != 0x00D7 && c != 0x00F7) ||
        (c >= 0x1E00 && c <= 0x1EFF)) {
        return Script::Latin;
    }
    if (c >= 0x0400 && c <= 0x052F) return Script::Cyrillic;
    if (c >= 0x0370 && c <= 0x03FF) return Script::Greek;
    if ((c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF) || (c >= 0xF900 && c <= 0xFAFF)) return Script::Han;
    if (c >= 0x3040 && c <= 0x30FF) return Script::Kana;
    if ((c >= 0xAC00 && c <= 0xD7AF) || (c >= 0x1100 && c <= 0x11FF)) return Script::Hangul;
    if (c >= 0x0600 && c <= 0x06FF) return Script::Arabic;
    if (c < 0x80 || std::iswdigit(c) || std::iswpunct(c) || std::iswspace(c) || !std::iswalpha(c)) return Script::None;
    return Script::Other;
}

// The script a language writes, by its first subtag.
auto script_of(std::string_view language) -> Script {
    const auto primary = language.substr(0, language.find('-'));
    for (const std::string_view cyrillic : {"ru", "uk", "be", "bg", "sr", "mk", "kk", "ky", "mn", "tg"}) {
        if (primary == cyrillic) return Script::Cyrillic;
    }
    if (primary == "zh" || primary == "ja") return Script::Han; // Japanese kana counts as its Han (below)
    if (primary == "ko") return Script::Hangul;
    if (primary == "el") return Script::Greek;
    if (primary == "ar" || primary == "fa" || primary == "ur") return Script::Arabic;
    return Script::Latin;
}

} // namespace

auto doubtful(std::string_view word, std::string_view language) -> bool {
    const auto wide = core::utf8::to_wide(word);
    if (!wide) {
        return true;
    }
    const bool japanese = language.substr(0, language.find('-')) == "ja";
    Script seen = Script::None;
    for (const wchar_t c : *wide) {
        auto s = script_of(c);
        if (s == Script::Kana && japanese) {
            s = Script::Han;
        }
        if (s == Script::None) {
            continue;
        }
        if (seen != Script::None && s != seen) {
            return true; // two scripts in one word
        }
        seen = s;
    }
    return seen != Script::None && seen != Script::Latin && seen != script_of(language);
}

auto profile() -> std::vector<ProfileLanguage> {
    core::win::keep_mta(); // before WinRT: its factories outlive a borrowed MTA (core/win.hpp)
    std::vector<ProfileLanguage> out;
    try {
        for (const auto& tag : winrt::Windows::System::UserProfile::GlobalizationPreferences::Languages()) {
            const winrt::Windows::Globalization::Language l{tag};
            out.push_back({.tag = utf8(tag), .name = utf8(l.NativeName()), .installed = wmo::OcrEngine::IsLanguageSupported(l)});
        }
    } catch (const winrt::hresult_error&) {
        out.clear();
    }
    return out;
}

auto languages() -> std::vector<std::string> {
    core::win::keep_mta(); // before WinRT: its factories outlive a borrowed MTA (core/win.hpp)
    std::vector<std::string> out;
    try {
        for (const auto& l : wmo::OcrEngine::AvailableRecognizerLanguages()) {
            out.push_back(utf8(l.LanguageTag()));
        }
    } catch (const winrt::hresult_error&) {
        out.clear(); // no recognizer at all: an empty list says it
    }
    return out;
}

auto recognize(const Picture& picture, std::string_view language) -> core::Result<OcrText> {
    if (picture.width <= 0 || picture.height <= 0) {
        return core::make_error(core::Code::InvalidArgument, "an empty picture");
    }
    core::win::keep_mta(); // before WinRT: its factories outlive a borrowed MTA (core/win.hpp)
    try {
        auto engine = engine_for(language);
        if (!engine) {
            return std::unexpected{engine.error()};
        }
        const auto most = static_cast<int>(wmo::OcrEngine::MaxImageDimension());
        const Picture fitted = image::fit(picture, most);
        const auto bytes = static_cast<std::uint32_t>(fitted.bgra.size());
        winrt::Windows::Storage::Streams::Buffer buffer{bytes};
        std::memcpy(buffer.data(), fitted.bgra.data(), bytes);
        buffer.Length(bytes);
        const auto bitmap = wgi::SoftwareBitmap::CreateCopyFromBuffer(buffer, wgi::BitmapPixelFormat::Bgra8, fitted.width, fitted.height,
                                                                     wgi::BitmapAlphaMode::Straight);
        const auto result = engine->RecognizeAsync(bitmap).get(); // the tool's thread: never the window's
        OcrText out{.language = utf8(engine->RecognizerLanguage().LanguageTag()), .lines = {}, .angle = {}};
        if (const auto angle = result.TextAngle()) {
            out.angle = angle.Value();
        }
        // The boxes back in the picture's own pixels (it was read smaller when it was larger). With a
        // slant found, Windows gives them as the text stood straightened (turned about the picture's
        // middle): each box's middle is turned back by it, the box kept the text's size. It finds
        // slants of 2 degrees in straight text too (measured): unturned, those boxes sat 10-20 px off.
        const double sx = static_cast<double>(picture.width) / fitted.width;
        const double sy = static_cast<double>(picture.height) / fitted.height;
        const double turn = out.angle.value_or(0.0) * std::numbers::pi / 180.0;
        const double cx = fitted.width / 2.0, cy = fitted.height / 2.0;
        const auto box_of = [&](const winrt::Windows::Foundation::Rect& r) {
            const double dx = r.X + r.Width / 2.0 - cx, dy = r.Y + r.Height / 2.0 - cy;
            const double mx = cx + dx * std::cos(turn) - dy * std::sin(turn);
            const double my = cy + dx * std::sin(turn) + dy * std::cos(turn);
            const auto x0 = static_cast<int>(std::floor((mx - r.Width / 2.0) * sx)), y0 = static_cast<int>(std::floor((my - r.Height / 2.0) * sy));
            const auto x1 = static_cast<int>(std::ceil((mx + r.Width / 2.0) * sx)), y1 = static_cast<int>(std::ceil((my + r.Height / 2.0) * sy));
            return OcrBox{x0, y0, x1 - x0, y1 - y0};
        };
        for (const auto& line : result.Lines()) {
            OcrLine l{.text = utf8(line.Text()), .box = {}, .words = {}};
            int x0 = picture.width, y0 = picture.height, x1 = 0, y1 = 0;
            for (const auto& word : line.Words()) {
                OcrWord w{.text = utf8(word.Text()), .box = box_of(word.BoundingRect()), .doubtful = false};
                w.doubtful = doubtful(w.text, out.language);
                x0 = std::min(x0, w.box.x);
                y0 = std::min(y0, w.box.y);
                x1 = std::max(x1, w.box.x + w.box.w);
                y1 = std::max(y1, w.box.y + w.box.h);
                l.words.push_back(std::move(w));
            }
            if (!l.words.empty()) {
                l.box = {x0, y0, x1 - x0, y1 - y0};
            }
            out.lines.push_back(std::move(l));
        }
        return out;
    } catch (const winrt::hresult_error& e) {
        return core::make_error(core::Code::Io, "the text could not be read: " + utf8(e.message()));
    }
}

} // namespace tools::ocr
