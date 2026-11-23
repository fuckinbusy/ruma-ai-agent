#include "theme/theme.hpp"

// The module's own "config": the values of the themes that come with the agent. This is the only
// place under src/ where a colour of the interface is written in numbers. They are the designer's
// (the palette package's live mockup: its [data-th=dark] and [data-th=linen] tokens and the orb's
// TH.dark.orb / TH.linen.orb).

namespace theme {
namespace {

constexpr auto channel(unsigned rgb, int shift) -> std::uint8_t {
    return static_cast<std::uint8_t>((rgb >> shift) & 0xFFu);
}

// #RRGGBB, opaque.
constexpr auto rgb(unsigned hex) -> Color {
    return {channel(hex, 16), channel(hex, 8), channel(hex, 0), 255};
}

// #RRGGBB at `alpha` of 1 (CSS rgba's last number), rounded to the nearest of 255.
constexpr auto rgba(unsigned hex, double alpha) -> Color {
    return {channel(hex, 16), channel(hex, 8), channel(hex, 0), static_cast<std::uint8_t>(alpha * 255.0 + 0.5)};
}

constexpr unsigned kWhite = 0xFFFFFF;
constexpr unsigned kBlack = 0x000000;
constexpr unsigned kDarkText = 0xEDEDF0;
constexpr unsigned kDarkAccent = 0xA9A6F7;
constexpr unsigned kLightInk = 0x34281E;   // rgb(52,40,30): the light theme's lines
constexpr unsigned kLightText = 0x25232B;
constexpr unsigned kLightAccent = 0x5753CC;
constexpr unsigned kLightShade = 0x281E14; // rgb(40,30,20): the light theme's shadow
constexpr unsigned kDarkOk = 0x7FD1A6;
constexpr unsigned kDarkWarn = 0xE9C273;
constexpr unsigned kDarkCrit = 0xF09AA2;
// Darker than the first palette's (#1F7A52, #8A620F, #B23F57): a status on its soft ground over the
// panel fell under 4.5:1.
constexpr unsigned kLightOk = 0x1B714B;
constexpr unsigned kLightWarn = 0x80590C;
constexpr unsigned kLightCrit = 0xA93A52;

constexpr std::array<Theme, 2> kThemes{{
    // Dark — neutral graphite, as Windows 11's and VS Code's dark themes; the colour is in the
    // accent, the statuses and the orb only.
    {
        .surface = rgb(0x1A1A1C),
        .surface_top = rgb(0x202022),
        .panel = rgb(0x151517),
        .card = rgba(kWhite, 0.05),
        .card_2 = rgba(kWhite, 0.085),
        .line = rgba(kWhite, 0.08),
        .line_2 = rgba(kWhite, 0.13),
        .text = rgb(kDarkText),
        .text_2 = rgba(kDarkText, 0.80),
        .text_3 = rgba(kDarkText, 0.62),
        .text_4 = rgba(kDarkText, 0.50),
        .accent = rgb(kDarkAccent),
        .accent_2 = rgb(0x8FCDF0),
        .accent_soft = rgba(kDarkAccent, 0.13),
        .accent_line = rgba(kDarkAccent, 0.34),
        .on_accent = rgb(0x17162A),
        .ok = rgb(kDarkOk),
        .warn = rgb(kDarkWarn),
        .crit = rgb(kDarkCrit),
        .you = rgba(kWhite, 0.065),
        .you_line = rgba(kWhite, 0.10),
        .w1 = rgba(kDarkText, 0.045),
        .w2 = rgba(kDarkText, 0.075),
        .w3 = rgba(kDarkText, 0.11),
        .w4 = rgba(kDarkText, 0.18),
        .scroll = rgba(kDarkText, 0.24),
        .scroll_hi = rgba(kDarkText, 0.40),
        .sunk = rgba(kBlack, 0.24),
        .pop = rgb(0x212123),
        .selection = rgba(kDarkAccent, 0.30),
        .scrim = rgba(0x08080A, 0.42),
        .accent_soft_hi = rgba(kDarkAccent, 0.20),
        .ok_soft = rgba(kDarkOk, 0.13),
        .ok_soft_hi = rgba(kDarkOk, 0.20),
        .ok_line = rgba(kDarkOk, 0.34),
        .warn_soft = rgba(kDarkWarn, 0.13),
        .warn_soft_hi = rgba(kDarkWarn, 0.20),
        .warn_line = rgba(kDarkWarn, 0.34),
        .crit_soft = rgba(kDarkCrit, 0.13),
        .crit_soft_hi = rgba(kDarkCrit, 0.20),
        .crit_line = rgba(kDarkCrit, 0.34),
        .c1 = rgb(0xA9A6F7),
        .c2 = rgb(0x8FCDF0),
        .c3 = rgb(0xF0B7E6),
        .c4 = rgb(0xE3CDA2),
        .c_rest = rgba(kDarkText, 0.28),
        .f_folder = rgb(0xE2B55A),
        .f_folder_line = {},
        .f_doc = rgb(0x5B83C9),
        .f_sheet = rgb(0x4C9A70),
        .f_page = rgb(0xD4D4DA),
        .sign_bar = rgba(0xECECF0, 0.62),
        .sign_halo = rgba(0x0E0E10, 0.55),
        .send = {rgb(0xCBC9FF), rgb(0x9C99F0)},
        .shadow = {{{30, 70, -24, rgba(kBlack, 0.80)}, {10, 24, -12, rgba(kBlack, 0.55)}}},
        .pop_shadow = {{{14, 36, -10, rgba(kBlack, 0.65)}, {2, 8, 0, rgba(kBlack, 0.35)}}},
        .orb = {.l1 = rgb(0xA79BFF), .l2 = rgb(0x8FD6F5), .l3 = rgb(0xF0B7E6), .tint = rgb(0xF0EEFF), .glow = rgb(0xABA6F5), .amb = 0.0f},
        .radius = {},
        .name = "Dark",
        .light = false,
    },
    // Light — warm paper and periwinkle; the window is held by its shadow, not by its line.
    {
        .surface = rgb(0xFAF8F4),
        .surface_top = rgb(0xFFFEFB),
        .panel = rgb(0xF3F0EB),
        .card = rgb(kWhite),
        .card_2 = rgb(0xF7F4EF),
        .line = rgba(kLightInk, 0.085),
        .line_2 = rgba(kLightInk, 0.14),
        .text = rgb(kLightText),
        .text_2 = rgba(kLightText, 0.82),
        .text_3 = rgba(kLightText, 0.70),
        .text_4 = rgba(kLightText, 0.64),
        .accent = rgb(kLightAccent),
        .accent_2 = rgb(0xC9714F),
        .accent_soft = rgba(kLightAccent, 0.08),
        .accent_line = rgba(kLightAccent, 0.30),
        .on_accent = rgb(kWhite),
        .ok = rgb(kLightOk),
        .warn = rgb(kLightWarn),
        .crit = rgb(kLightCrit),
        .you = rgba(kLightAccent, 0.08),
        .you_line = rgba(kLightAccent, 0.18),
        .w1 = rgba(kLightText, 0.035),
        .w2 = rgba(kLightText, 0.06),
        .w3 = rgba(kLightText, 0.09),
        .w4 = rgba(kLightText, 0.14),
        .scroll = rgba(kLightText, 0.30),
        .scroll_hi = rgba(kLightText, 0.48),
        .sunk = rgba(kLightInk, 0.05),
        .pop = rgb(kWhite),
        .selection = rgba(kLightAccent, 0.20),
        .scrim = rgba(0x1E1C24, 0.18),
        .accent_soft_hi = rgba(kLightAccent, 0.11),
        .ok_soft = rgba(kLightOk, 0.08),
        .ok_soft_hi = rgba(kLightOk, 0.11),
        .ok_line = rgba(kLightOk, 0.30),
        .warn_soft = rgba(kLightWarn, 0.08),
        .warn_soft_hi = rgba(kLightWarn, 0.11),
        .warn_line = rgba(kLightWarn, 0.30),
        .crit_soft = rgba(kLightCrit, 0.08),
        .crit_soft_hi = rgba(kLightCrit, 0.11),
        .crit_line = rgba(kLightCrit, 0.30),
        .c1 = rgb(0x6662D6),
        .c2 = rgb(0x3F8BC0),
        .c3 = rgb(0xC0619C),
        .c4 = rgb(0xA88239),
        .c_rest = rgba(kLightText, 0.24),
        .f_folder = rgb(0xD9A43C),
        .f_folder_line = rgba(0x78500A, 0.28),
        .f_doc = rgb(0x3F6DB8),
        .f_sheet = rgb(0x2F8458),
        .f_page = rgb(kWhite),
        .sign_bar = rgba(0x26242C, 0.55),
        .sign_halo = rgba(kWhite, 0.7),
        .send = {rgb(0x7471E2), rgb(0x5450C8)},
        .shadow = {{{30, 70, -26, rgba(kLightShade, 0.40)}, {10, 24, -12, rgba(kLightShade, 0.18)}}},
        .pop_shadow = {{{14, 36, -12, rgba(kLightShade, 0.28)}, {2, 8, 0, rgba(kLightShade, 0.08)}}},
        .orb = {.l1 = rgb(0x8B86FF), .l2 = rgb(0xF2A78C), .l3 = rgb(0x8CC9F2), .tint = rgb(kWhite), .glow = rgb(0x8A87F0), .amb = 0.42f},
        .radius = {},
        .name = "Light",
        .light = true,
    },
}};

constexpr std::array<ColorToken, kColorTokens> kTokens{{
    {"surface", &Theme::surface},
    {"surface_top", &Theme::surface_top},
    {"panel", &Theme::panel},
    {"card", &Theme::card},
    {"card_2", &Theme::card_2},
    {"line", &Theme::line},
    {"line_2", &Theme::line_2},
    {"text", &Theme::text},
    {"text_2", &Theme::text_2},
    {"text_3", &Theme::text_3},
    {"text_4", &Theme::text_4},
    {"accent", &Theme::accent},
    {"accent_2", &Theme::accent_2},
    {"accent_soft", &Theme::accent_soft},
    {"accent_line", &Theme::accent_line},
    {"on_accent", &Theme::on_accent},
    {"ok", &Theme::ok},
    {"warn", &Theme::warn},
    {"crit", &Theme::crit},
    {"you", &Theme::you},
    {"you_line", &Theme::you_line},
    {"w1", &Theme::w1},
    {"w2", &Theme::w2},
    {"w3", &Theme::w3},
    {"w4", &Theme::w4},
    {"scroll", &Theme::scroll},
    {"scroll_hi", &Theme::scroll_hi},
    {"sunk", &Theme::sunk},
    {"pop", &Theme::pop},
    {"selection", &Theme::selection},
    {"scrim", &Theme::scrim},
    {"accent_soft_hi", &Theme::accent_soft_hi},
    {"ok_soft", &Theme::ok_soft},
    {"ok_soft_hi", &Theme::ok_soft_hi},
    {"ok_line", &Theme::ok_line},
    {"warn_soft", &Theme::warn_soft},
    {"warn_soft_hi", &Theme::warn_soft_hi},
    {"warn_line", &Theme::warn_line},
    {"crit_soft", &Theme::crit_soft},
    {"crit_soft_hi", &Theme::crit_soft_hi},
    {"crit_line", &Theme::crit_line},
    {"c1", &Theme::c1},
    {"c2", &Theme::c2},
    {"c3", &Theme::c3},
    {"c4", &Theme::c4},
    {"c_rest", &Theme::c_rest},
    {"f_folder", &Theme::f_folder},
    {"f_folder_line", &Theme::f_folder_line},
    {"f_doc", &Theme::f_doc},
    {"f_sheet", &Theme::f_sheet},
    {"f_page", &Theme::f_page},
    {"sign_bar", &Theme::sign_bar},
    {"sign_halo", &Theme::sign_halo},
}};

// A name's letters whatever their case (ASCII: the built-in names are).
constexpr auto same_name(std::string_view a, std::string_view b) -> bool {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

} // namespace

auto builtin() noexcept -> std::span<const Theme> {
    return kThemes;
}

auto dark() noexcept -> const Theme& {
    return kThemes[0];
}

auto light() noexcept -> const Theme& {
    return kThemes[1];
}

auto find(std::string_view name) noexcept -> const Theme* {
    for (const Theme& th : kThemes) {
        if (!name.empty() && same_name(th.name, name)) {
            return &th;
        }
    }
    return nullptr;
}

auto color_tokens() noexcept -> std::span<const ColorToken> {
    return kTokens;
}

} // namespace theme
