#pragma once

// tools::ocr — text in a picture, read on this machine by Windows' own recognizer
// (Windows.Media.Ocr, phase 9): nothing is sent anywhere. The languages are those installed in
// Windows (Settings, Time & language).

#include "core/error.hpp"
#include "tools/image.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tools {

// Phase 10 (screen 20): where each line and word is, in the picture's own pixels.
struct OcrBox {
    int x = 0, y = 0, w = 0, h = 0;
};
struct OcrWord {
    std::string text;
    OcrBox box;
    bool doubtful = false; // ocr::doubtful(): Windows says nothing of its certainty
};
struct OcrLine {
    std::string text;
    OcrBox box; // its words' box
    std::vector<OcrWord> words;
};

struct OcrText {
    std::string language; // the BCP-47 tag it read with: "en-US", "ru"
    std::vector<OcrLine> lines;
    std::optional<double> angle; // the text's slant in degrees, when Windows found one
};

// A language of the user's Windows profile, for the panel's chips: whether there is a recognizer.
struct ProfileLanguage {
    std::string tag;  // "ru", "en-US"
    std::string name; // its own name: Russian's in Russian, "English (United States)"
    bool installed = false;
};

namespace ocr {

// The installed recognizer languages' tags.
auto languages() -> std::vector<std::string>;
// The profile's languages, in its order.
auto profile() -> std::vector<ProfileLanguage>;
// `language` empty: the user profile's languages, else the first installed. A picture larger than
// the recognizer takes is made smaller first; the boxes are still the picture's own pixels.
auto recognize(const Picture& picture, std::string_view language) -> core::Result<OcrText>;
// Windows OCR gives no certainty, so a word is doubtful by what it is made of: letters of two
// scripts in one word ("cmp." with a Cyrillic c and p and a Latin m), or of a script the language (a BCP-47 tag)
// does not write. Latin is never foreign; digits and marks are no script.
auto doubtful(std::string_view word, std::string_view language) -> bool;

} // namespace ocr
} // namespace tools
