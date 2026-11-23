#include "core/secret_like.hpp"

#include <array>

namespace core::secret_like {
namespace {

auto word(char c) noexcept -> bool {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

auto key_char(char c) noexcept -> bool {
    return word(c) || c == '-';
}

auto digit(char c) noexcept -> bool {
    return c >= '0' && c <= '9';
}

auto lower(char c) noexcept -> char {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

auto starts_nocase(std::string_view s, std::size_t i, std::string_view prefix) noexcept -> bool {
    if (s.size() - i < prefix.size()) {
        return false;
    }
    for (std::size_t k = 0; k < prefix.size(); ++k) {
        if (lower(s[i + k]) != prefix[k]) {
            return false;
        }
    }
    return true;
}

// A key with its maker's prefix: the prefix kept, `least` key characters after it.
auto prefixed(std::string_view s, std::size_t i) noexcept -> std::optional<Found> {
    struct Prefix {
        std::string_view text;
        std::size_t least;
    };
    static constexpr std::array<Prefix, 10> kPrefixes{{{"sk-", 8},
                                                       {"ghp_", 20},
                                                       {"gho_", 20},
                                                       {"ghs_", 20},
                                                       {"ghu_", 20},
                                                       {"github_pat_", 20},
                                                       {"xoxb-", 10},
                                                       {"xoxp-", 10},
                                                       {"xoxa-", 10},
                                                       {"AKIA", 16}}};
    for (const auto& p : kPrefixes) {
        if (s.substr(i, p.text.size()) != p.text) {
            continue;
        }
        std::size_t end = i + p.text.size();
        while (end < s.size() && key_char(s[end])) {
            ++end;
        }
        if (end - (i + p.text.size()) >= p.least) {
            return Found{i + p.text.size(), end, "an API key"};
        }
    }
    return std::nullopt;
}

// Authorization: its name, separators and scheme kept; the credential is the secret.
auto authorization(std::string_view s, std::size_t i) noexcept -> std::optional<Found> {
    constexpr std::string_view kAuth = "authorization";
    if (!starts_nocase(s, i, kAuth)) {
        return std::nullopt;
    }
    std::size_t j = i + kAuth.size();
    bool separator = false;
    while (j < s.size() && (s[j] == '"' || s[j] == ':' || s[j] == '=' || s[j] == ' ' || s[j] == '\t')) {
        separator = separator || s[j] == ':' || s[j] == '=';
        ++j;
    }
    if (!separator) {
        return std::nullopt;
    }
    for (const std::string_view scheme : {std::string_view{"bearer"}, std::string_view{"basic"}}) {
        if (starts_nocase(s, j, scheme) && j + scheme.size() < s.size() && (s[j + scheme.size()] == ' ' || s[j + scheme.size()] == '\t')) {
            j += scheme.size();
            while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) {
                ++j;
            }
            break;
        }
    }
    std::size_t end = j;
    while (end < s.size() && s[end] != ' ' && s[end] != '\t' && s[end] != '"' && s[end] != ',' && s[end] != '\r' && s[end] != '\n') {
        ++end;
    }
    return Found{j, end, "a token"};
}

// password=…, token: …, api_key = "…": the name and the separator kept, a value of 4 characters or more.
auto named(std::string_view s, std::size_t i) noexcept -> std::optional<Found> {
    static constexpr std::array<std::string_view, 11> kNames{"password", "passwd", "pwd",    "secret",       "client_secret", "api_key",
                                                             "apikey",   "api-key", "token", "access_token", "auth_token"};
    for (const auto name : kNames) {
        if (!starts_nocase(s, i, name)) {
            continue;
        }
        std::size_t j = i + name.size();
        if (j < s.size() && word(s[j])) {
            continue; // a longer word ("passwords", "tokens")
        }
        while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) {
            ++j;
        }
        if (j >= s.size() || (s[j] != '=' && s[j] != ':')) {
            continue; // the word alone: no value
        }
        ++j;
        while (j < s.size() && (s[j] == ' ' || s[j] == '\t' || s[j] == '"' || s[j] == '\'')) {
            ++j;
        }
        std::size_t end = j;
        while (end < s.size() && s[end] != ' ' && s[end] != '\t' && s[end] != '"' && s[end] != '\'' && s[end] != ',' &&
               s[end] != ';' && s[end] != '&' && s[end] != '\r' && s[end] != '\n') {
            ++end;
        }
        if (end - j >= 4) {
            return Found{j, end, name == "token" || name == "access_token" || name == "auth_token" ? "a token" : "a password"};
        }
    }
    return std::nullopt;
}

// The Russian names (final review M1): "password for the mail: …", "token = …" in Russian — up to three words between
// the name and its = or :, as Russian puts them; a value of 4 bytes or more.
auto named_ru(std::string_view s, std::size_t i) noexcept -> std::optional<Found> {
    struct Name {
        std::string_view text;
        std::string_view kind;
    };
    static constexpr std::array<Name, 9> kNames{
        Name{"\xD0\xBF\xD0\xB0\xD1\x80\xD0\xBE\xD0\xBB\xD1\x8C", "a password"}, // "parol'" (password)
        Name{"\xD0\x9F\xD0\xB0\xD1\x80\xD0\xBE\xD0\xBB\xD1\x8C", "a password"}, // "Parol'"
        Name{"\xD0\x9F\xD0\x90\xD0\xA0\xD0\x9E\xD0\x9B\xD0\xAC", "a password"}, // "PAROL'"
        Name{"\xD1\x82\xD0\xBE\xD0\xBA\xD0\xB5\xD0\xBD", "a token"},             // "token"
        Name{"\xD0\xA2\xD0\xBE\xD0\xBA\xD0\xB5\xD0\xBD", "a token"},             // "Token"
        Name{"\xD0\xA2\xD0\x9E\xD0\x9A\xD0\x95\xD0\x9D", "a token"},             // "TOKEN"
        Name{"\xD1\x81\xD0\xB5\xD0\xBA\xD1\x80\xD0\xB5\xD1\x82", "a password"}, // "sekret" (secret)
        Name{"\xD0\xA1\xD0\xB5\xD0\xBA\xD1\x80\xD0\xB5\xD1\x82", "a password"}, // "Sekret"
        Name{"\xD0\xA1\xD0\x95\xD0\x9A\xD0\xA0\xD0\x95\xD0\xA2", "a password"}, // "SEKRET"
    };
    const auto blank = [&](std::size_t k) { return k < s.size() && (s[k] == ' ' || s[k] == '\t'); };
    for (const auto& name : kNames) {
        if (!s.substr(i).starts_with(name.text)) {
            continue;
        }
        std::size_t j = i + name.text.size();
        if (j < s.size() && s[j] != ' ' && s[j] != '\t' && s[j] != ':' && s[j] != '=') {
            continue; // a longer word ("parolem", "tokeny": the words with an ending)
        }
        for (int words = 0;; ++words) {
            while (blank(j)) {
                ++j;
            }
            if (j >= s.size() || s[j] == ':' || s[j] == '=' || words == 3) {
                break;
            }
            while (j < s.size() && !blank(j) && s[j] != ':' && s[j] != '=') {
                ++j;
            }
        }
        if (j >= s.size() || (s[j] != ':' && s[j] != '=')) {
            continue;
        }
        ++j;
        while (j < s.size() && (blank(j) || s[j] == '"' || s[j] == '\'')) {
            ++j;
        }
        std::size_t end = j;
        while (end < s.size() && !blank(end) && s[end] != '"' && s[end] != '\'' && s[end] != ',' && s[end] != ';' && s[end] != '&' &&
               s[end] != '\r' && s[end] != '\n') {
            ++end;
        }
        if (end - j >= 4) {
            return Found{j, end, name.kind};
        }
    }
    return std::nullopt;
}

// 13 to 19 digits, single spaces or dashes between, a card's first digits (2221-2720, 3-6) and Luhn.
auto card(std::string_view s, std::size_t i) noexcept -> std::optional<Found> {
    if (!digit(s[i]) || (i > 0 && (word(s[i - 1]) || s[i - 1] == '.'))) {
        return std::nullopt;
    }
    std::array<int, 19> digits{};
    std::size_t n = 0;
    std::size_t j = i;
    while (j < s.size()) {
        if (digit(s[j])) {
            if (n == digits.size()) {
                return std::nullopt; // too long for a card
            }
            digits[n++] = s[j] - '0';
            ++j;
        } else if ((s[j] == ' ' || s[j] == '-') && j + 1 < s.size() && digit(s[j + 1])) {
            ++j;
        } else {
            break;
        }
    }
    if (n < 13 || (j < s.size() && word(s[j]))) {
        return std::nullopt;
    }
    const int first4 = digits[0] * 1000 + digits[1] * 100 + digits[2] * 10 + digits[3];
    if (!((digits[0] >= 3 && digits[0] <= 6) || (first4 >= 2221 && first4 <= 2720))) {
        return std::nullopt;
    }
    int sum = 0;
    for (std::size_t k = 0; k < n; ++k) {
        int d = digits[n - 1 - k];
        if (k % 2 == 1) {
            d *= 2;
            d -= d > 9 ? 9 : 0;
        }
        sum += d;
    }
    if (sum % 10 != 0) {
        return std::nullopt;
    }
    return Found{i, j, "a card number"};
}

} // namespace

auto at(std::string_view text, std::size_t i) noexcept -> std::optional<Found> {
    if (i >= text.size() || (i > 0 && word(text[i - 1]) && !digit(text[i]))) {
        return std::nullopt; // a secret starts a word
    }
    if (auto f = prefixed(text, i)) {
        return f;
    }
    if (auto f = authorization(text, i)) {
        return f;
    }
    if (auto f = named(text, i)) {
        return f;
    }
    if (auto f = named_ru(text, i)) {
        return f;
    }
    return card(text, i);
}

auto looks_secret(std::string_view text) -> std::optional<std::string> {
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (const auto f = at(text, i); f && f->end > f->value) {
            return "it looks like " + std::string{f->kind};
        }
    }
    return std::nullopt;
}

} // namespace core::secret_like
