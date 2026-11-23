#include "security/shellguard.hpp"

#include "core/utf8.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <deque>
#include <format>
#include <optional>
#include <span>

namespace security::shellguard {
namespace {

// ---- reading the command

struct Token {
    std::string text;      // unquoted, unescaped, environment variables and ~ expanded
    bool quoted = false;   // held a quoted string
    bool variable = false; // a variable whose value the guard cannot know
    bool expr = false;     // held a $( ), ( ), { }, @( ) or a [type]
    bool wildcard = false; // * ? [
};

struct Part {
    std::vector<Token> tokens;
    std::vector<Token> redirects; // its output to files ($null and stream merges left out)
    bool nested = false;          // inside $( ), ( ) or { }
    bool call = false;            // & or . at its start: it runs what it names
    bool piped = false;           // after a |: it takes what the part before gives (final review S7)
    std::uint32_t pipeline = 0;   // the parts joined by | share it
};

struct Parsed {
    std::vector<Part> parts;
    std::string unread; // why some of it could not be read; empty: all of it was
};

auto env(std::string_view name) -> std::optional<std::string> {
    const auto w = core::utf8::to_wide(name);
    if (!w || w->empty()) {
        return std::nullopt;
    }
    const DWORD n = GetEnvironmentVariableW(w->c_str(), nullptr, 0);
    if (n == 0) {
        return std::nullopt;
    }
    std::wstring value(n, L'\0');
    value.resize(GetEnvironmentVariableW(w->c_str(), value.data(), n));
    auto s = core::utf8::to_utf8(value);
    return s ? std::optional{std::move(*s)} : std::nullopt;
}

auto lower(std::string s) -> std::string {
    std::ranges::transform(s, s.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
    return s;
}

auto space(char c) -> bool {
    return c == ' ' || c == '\t' || c == '\r';
}

// A space PowerShell splits words at beyond ASCII (U+00A0, U+2000-U+200A, U+202F, U+205F, U+3000):
// its length in bytes at `s`, or 0 (final review S4).
auto wide_space(std::string_view s) -> std::size_t {
    if (s.starts_with("\xC2\xA0")) {
        return 2;
    }
    if (s.size() >= 3 && s[0] == '\xE2' && s[1] == '\x80' && ((s[2] >= '\x80' && s[2] <= '\x8A') || s[2] == '\xAF')) {
        return 3;
    }
    if (s.starts_with("\xE2\x81\x9F") || s.starts_with("\xE3\x80\x80")) {
        return 3;
    }
    return 0;
}

// An en, em or horizontal dash, which PowerShell takes as the - of a parameter.
auto wide_dash(std::string_view s) -> bool {
    return s.starts_with("\xE2\x80\x93") || s.starts_with("\xE2\x80\x94") || s.starts_with("\xE2\x80\x95");
}

class Reader {
public:
    explicit Reader(std::string_view s) : s_(s) {}

    auto read() -> Parsed {
        list(0, false);
        return std::move(out_);
    }

private:
    void unread(std::string_view why) {
        if (out_.unread.empty()) {
            out_.unread = why;
        }
    }

    auto at(std::size_t k) const -> char {
        return i_ + k < s_.size() ? s_[i_ + k] : '\0';
    }

    // A quote at i_ (PowerShell takes the typographic ones too): its kind and its length.
    auto quote() const -> std::pair<char, std::size_t> {
        const auto rest = s_.substr(i_);
        if (rest.starts_with('\'') || rest.starts_with('"')) {
            return {rest[0], 1};
        }
        if (rest.starts_with("\xE2\x80\x98") || rest.starts_with("\xE2\x80\x99")) {
            return {'\'', 3};
        }
        if (rest.starts_with("\xE2\x80\x9C") || rest.starts_with("\xE2\x80\x9D")) {
            return {'"', 3};
        }
        return {'\0', 0};
    }

    // Parts up to `close` (')' or '}'; 0: the end).
    void list(char close, bool nested) {
        std::uint32_t pipeline = ++pipes_;
        Part part{.nested = nested, .pipeline = pipeline};
        // `piped`: the next part takes what this one gives (a single |); otherwise a new pipeline.
        const auto flush = [&](bool piped = false) {
            if (!part.tokens.empty() || !part.redirects.empty() || part.call) {
                out_.parts.push_back(std::move(part));
            }
            if (!piped) {
                pipeline = ++pipes_;
            }
            part = Part{.nested = nested, .piped = piped, .pipeline = pipeline};
        };
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (space(c)) {
                ++i_;
            } else if (const auto n = wide_space(s_.substr(i_)); n > 0) {
                i_ += n;
            } else if (close != 0 && c == close) {
                ++i_;
                flush();
                return;
            } else if (c == ')' || c == '}') {
                unread("a closing bracket with no opening one");
                ++i_;
            } else if (c == '\n' || c == ';') {
                flush();
                ++i_;
            } else if (c == '|') {
                const bool both = at(1) == '|'; // || runs the next one after; | gives it this one's output
                flush(!both);
                i_ += both ? 2 : 1;
            } else if (c == '&') {
                if (at(1) == '&') {
                    flush();
                    i_ += 2;
                } else if (part.tokens.empty() && !part.call) {
                    part.call = true; // the call operator
                    ++i_;
                } else {
                    flush(); // a background job: the command before it
                    ++i_;
                }
            } else if (c == '#') {
                while (i_ < s_.size() && s_[i_] != '\n') {
                    ++i_;
                }
            } else if (c == ',') {
                ++i_;
            } else if (c == '<') {
                unread("an input redirection");
                ++i_;
            } else if (c == '>' || ((c == '*' || (c >= '0' && c <= '9')) && at(1) == '>')) {
                redirect(part);
            } else if (c == '.' && part.tokens.empty() && !part.call && (at(1) == '\0' || space(at(1)))) {
                part.call = true; // dot-sourcing
                ++i_;
            } else if (s_.substr(i_).starts_with("--%")) {
                unread("--%: PowerShell passes the rest unread");
                i_ = s_.size();
            } else if (c == '@' && (at(1) == '\'' || at(1) == '"')) {
                unread("a here-string");
                i_ = s_.size();
            } else {
                token(part);
            }
        }
        flush();
        if (close != 0) {
            unread("an unclosed bracket");
        }
    }

    void redirect(Part& part) {
        if (s_[i_] != '>') {
            ++i_; // the stream's number
        }
        ++i_;
        if (at(0) == '>') {
            ++i_;
        }
        if (at(0) == '&') { // 2>&1: into another stream, no file
            i_ += (at(1) >= '0' && at(1) <= '9') ? 2 : 1;
            return;
        }
        while (i_ < s_.size() && space(s_[i_])) {
            ++i_;
        }
        Part target;
        if (i_ < s_.size()) {
            token(target);
        }
        if (target.tokens.empty()) {
            unread("a redirection with no file");
            return;
        }
        if (lower(target.tokens[0].text) != "$null") {
            part.redirects.push_back(std::move(target.tokens[0]));
        }
    }

    void token(Part& part) {
        Token t;
        if (s_[i_] == '[') {
            t.expr = true; // a type: [IO.File]::Delete(...)
        }
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (space(c) || c == '\n' || c == ';' || c == '|' || c == ',' || c == '<' || c == '>' || c == '&' || c == ')' || c == '}' ||
                wide_space(s_.substr(i_)) > 0) {
                break;
            }
            if (t.text.empty() && !t.quoted && wide_dash(s_.substr(i_))) { // –Recurse is -Recurse to PowerShell
                t.text += '-';
                i_ += 3;
                continue;
            }
            if (const auto [q, n] = quote(); q != '\0') {
                i_ += n;
                t.quoted = true;
                q == '\'' ? single(t) : doubled(t);
            } else if (c == '`') {
                escape(t, false);
            } else if (c == '$') {
                dollar(t);
            } else if (c == '(' || c == '{') {
                ++i_;
                t.expr = true;
                list(c == '(' ? ')' : '}', true);
            } else if (c == '@' && (at(1) == '(' || at(1) == '{')) {
                const char open = at(1);
                i_ += 2;
                t.expr = true;
                list(open == '(' ? ')' : '}', true);
            } else if (c == '%') {
                percent(t);
            } else if (c == '~' && t.text.empty() && !t.quoted && (at(1) == '\\' || at(1) == '/' || at(1) == '\0' || space(at(1)))) {
                if (const auto home = env("USERPROFILE")) {
                    t.text += *home;
                } else {
                    t.variable = true;
                }
                ++i_;
            } else {
                t.text += c;
                ++i_;
            }
        }
        t.wildcard = !t.expr && t.text.find_first_of("*?[") != std::string::npos;
        part.tokens.push_back(std::move(t));
    }

    // A backtick: the next character as it is; at a line's end, the line goes on.
    void escape(Token& t, bool in_string) {
        ++i_;
        if (at(0) == '\r' && at(1) == '\n') {
            i_ += 2;
            return;
        }
        if (at(0) == '\n') {
            ++i_;
            return;
        }
        if (i_ < s_.size()) {
            const char c = s_[i_++];
            t.text += in_string && c == 'n' ? '\n' : in_string && c == 't' ? '\t' : c;
        }
    }

    void single(Token& t) {
        while (i_ < s_.size()) {
            if (const auto [q, n] = quote(); q == '\'') {
                i_ += n;
                if (const auto [again, m] = quote(); again == '\'') { // '' is one '
                    t.text += '\'';
                    i_ += m;
                    continue;
                }
                return;
            }
            t.text += s_[i_++];
        }
        unread("an unclosed quote");
    }

    void doubled(Token& t) {
        while (i_ < s_.size()) {
            if (const auto [q, n] = quote(); q == '"') {
                i_ += n;
                if (const auto [again, m] = quote(); again == '"') {
                    t.text += '"';
                    i_ += m;
                    continue;
                }
                return;
            }
            if (s_[i_] == '`') {
                escape(t, true);
            } else if (s_[i_] == '$') {
                dollar(t);
            } else {
                t.text += s_[i_++];
            }
        }
        unread("an unclosed quote");
    }

    // $( ), ${name}, $name, $env:NAME.
    void dollar(Token& t) {
        if (at(1) == '(') {
            i_ += 2;
            t.expr = true;
            list(')', true);
            return;
        }
        std::string name;
        if (at(1) == '{') {
            i_ += 2;
            while (i_ < s_.size() && s_[i_] != '}') {
                name += s_[i_++];
            }
            if (i_ >= s_.size()) {
                unread("an unclosed ${");
                return;
            }
            ++i_;
        } else {
            ++i_;
            while (i_ < s_.size()) {
                const char c = s_[i_];
                const bool word = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                                  c == ':' || c == '?' || (name.empty() && (c == '$' || c == '^'));
                if (!word) {
                    break;
                }
                name += c;
                ++i_;
            }
            if (name.empty()) {
                t.text += '$';
                return;
            }
        }
        const auto key = lower(name);
        if (key.starts_with("env:")) {
            if (const auto v = env(name.substr(4))) {
                t.text += *v;
                return;
            }
        } else if (key == "home") {
            if (const auto v = env("USERPROFILE")) {
                t.text += *v;
                return;
            }
        } else if (key == "null" || key == "true" || key == "false") {
            t.text += "$" + key;
            return;
        }
        t.variable = true;
        t.text += "$" + name;
    }

    // %NAME% (cmd's): its value when it has one.
    void percent(Token& t) {
        const auto end = s_.find('%', i_ + 1);
        if (end != std::string_view::npos && end > i_ + 1) {
            const auto name = s_.substr(i_ + 1, end - i_ - 1);
            if (name.find_first_of(" \t\r\n;|&") == std::string_view::npos) {
                if (const auto v = env(name)) {
                    t.text += *v;
                    i_ = end + 1;
                    return;
                }
            }
        }
        t.text += '%';
        ++i_;
    }

    std::string_view s_;
    std::size_t i_ = 0;
    std::uint32_t pipes_ = 0;
    Parsed out_;
};

// ---- the classes

enum class Class : std::uint8_t { Auto, Other, Ask, Deny }; // Other: asked, its start may be allowed for the task

struct Judged {
    Class cls = Class::Auto;
    std::string reason;
    std::string prefix;
};

using Names = std::initializer_list<std::string_view>;

auto one_of(std::string_view s, Names names) -> bool {
    return std::ranges::find(names, s) != names.end();
}

// A command's name: its last part, lower case, without .exe / .com.
auto name_of(const Token& t) -> std::string {
    std::string n = t.text;
    if (const auto cut = n.find_last_of("\\/"); cut != std::string::npos) {
        n = n.substr(cut + 1);
    }
    n = lower(std::move(n));
    for (const std::string_view ext : {".exe", ".com"}) {
        if (n.ends_with(ext)) {
            n.resize(n.size() - ext.size());
        }
    }
    return n;
}

// A word a start may hold: letters, digits, . _ - (no path, no drive, no quote).
auto plain(const Token& t) -> bool {
    return !t.quoted && !t.variable && !t.expr && !t.wildcard && !t.text.empty() &&
           std::ranges::all_of(t.text, [](char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
           });
}

auto option(const Token& t) -> bool {
    return !t.quoted && t.text.size() > 1 && t.text[0] == '-';
}

// /x, /fo:csv, /s/q (switches run together) — not /dir/file.
auto slash_option(const Token& t) -> bool {
    if (t.quoted || t.text.size() < 2 || t.text[0] != '/' || t.text.find('\\') != std::string::npos) {
        return false;
    }
    std::string_view rest = std::string_view{t.text}.substr(1);
    if (const auto colon = rest.find(':'); colon != std::string_view::npos) { // /c:text: its value may hold anything
        rest = rest.substr(0, colon);
        return !rest.empty() && rest.size() <= 4 && rest.find('/') == std::string_view::npos;
    }
    while (!rest.empty()) {
        const auto cut = rest.find('/');
        const auto flag = rest.substr(0, cut);
        if (flag.empty() || flag.size() > 4) {
            return false;
        }
        rest = cut == std::string_view::npos ? std::string_view{} : rest.substr(cut + 1);
    }
    return true;
}

// A switch among slash options, alone or run together: /s in "/s", "/s/q", "/q/s" (final review S3).
auto has_slash_flag(const Token& t, std::string_view flag) -> bool {
    if (!slash_option(t)) {
        return false;
    }
    const auto l = lower(t.text);
    std::string_view rest = std::string_view{l}.substr(1);
    rest = rest.substr(0, rest.find(':'));
    while (!rest.empty()) {
        const auto cut = rest.find('/');
        if (rest.substr(0, cut) == flag) {
            return true;
        }
        rest = cut == std::string_view::npos ? std::string_view{} : rest.substr(cut + 1);
    }
    return false;
}

// A parameter given by any unambiguous start of its name, as PowerShell takes it: its key ("-recurse"
// from "-Rec" or "-Recurse:$true") against `full`.
auto names_param(const Token& t, std::string_view full) -> bool {
    if (!option(t)) {
        return false;
    }
    const auto l = lower(t.text);
    const auto key = std::string_view{l}.substr(0, l.find(':'));
    return key.size() >= 2 && full.starts_with(key);
}

auto path_like(const Token& t) -> bool {
    return t.text.find_first_of("\\/:") != std::string::npos || t.text.starts_with('.') || t.text.starts_with('~');
}

// What a path-like word names for the file system: a file, a registry key, another provider's item.
enum class Provider : std::uint8_t { File, Registry, Other };
auto provider_of(std::string& text) -> Provider {
    const auto l = lower(text);
    for (const std::string_view fs : {"microsoft.powershell.core\\filesystem::", "filesystem::"}) {
        if (l.starts_with(fs)) {
            text = text.substr(fs.size());
            return Provider::File;
        }
    }
    if (l.starts_with("registry::") || l.starts_with("hkcu:") || l.starts_with("hklm:") || l.starts_with("hkey_")) {
        return Provider::Registry;
    }
    const auto colon = l.find(':');
    if (l.find("::") != std::string::npos || (colon != std::string::npos && colon >= 2 &&
                                              std::ranges::all_of(l.substr(0, colon), [](char c) { return c >= 'a' && c <= 'z'; }))) {
        return Provider::Other; // env:, cert:, variable:, function:...
    }
    return Provider::File;
}

// A wildcard's folder: what is before the name holding it ("C:\x\*.txt" -> "C:\x", "*.txt" -> ".").
auto folder_of(std::string_view text) -> std::string {
    const auto wild = text.find_first_of("*?[");
    const auto cut = text.substr(0, wild).find_last_of("\\/");
    if (cut == std::string_view::npos) {
        return ".";
    }
    return std::string{text.substr(0, cut == 2 && text[1] == ':' ? cut + 1 : cut)}; // C:\* -> C:\ (the drive)
}

auto wide(std::string_view s) -> std::wstring {
    auto w = core::utf8::to_wide(s);
    return w ? std::move(*w) : std::wstring{};
}

// Installs or changes software, however written (final review S5). The package managers that run a
// project's scripts are judged by what is known to only read or run (npm's misspellings install).
auto installs(const std::string& name, std::span<const Token> args) -> bool {
    const auto arg = [&](std::size_t i) { return i < args.size() ? lower(args[i].text) : std::string{}; };
    const auto has = [&](std::string_view a) { return std::ranges::any_of(args, [&](const Token& t) { return lower(t.text) == a; }); };
    if (one_of(name, {"msiexec", "choco", "install-module", "update-module", "save-module", "install-package", "install-script",
                      "update-script", "save-script", "install-psresource", "update-psresource", "save-psresource", "add-appxpackage",
                      "add-windowscapability", "enable-windowsoptionalfeature", "install-windowsfeature"})) {
        return true;
    }
    if (one_of(name, {"winget", "scoop"})) {
        return !args.empty() && !one_of(arg(0), {"list", "search", "show", "info", "status", "which", "help", "home", "--version", "-v",
                                                  "--info", "-?", "--help"});
    }
    if (one_of(name, {"npm", "pnpm", "yarn", "bun"})) {
        if (arg(0) == "audit") {
            return has("fix");
        }
        return !one_of(arg(0), {"test", "t", "tst", "run", "run-script", "rum", "urn", "start", "stop", "restart", "ls", "list", "ll",
                                "la", "view", "info", "show", "outdated", "help", "why", "explain", "root", "prefix", "bin", "search",
                                "fund", "docs", "repo", "bugs", "doctor", "-v", "--version", "-h", "--help"});
    }
    const auto pip = [&](std::size_t from) {
        return !one_of(arg(from), {"list", "show", "freeze", "check", "help", "debug", "inspect", "-v", "-V", "--version", "-h", "--help"});
    };
    if (one_of(name, {"pip", "pip3", "pipx", "uv", "poetry", "conda", "mamba"})) {
        return pip(0);
    }
    if (one_of(name, {"python", "python3", "py"})) {
        for (std::size_t i = 0; i + 1 < args.size() && option(args[i]); ++i) {
            if (lower(args[i].text) == "-m") {
                const auto module = arg(i + 1);
                return module == "ensurepip" || (one_of(module, {"pip", "pip3"}) && pip(i + 2));
            }
        }
        return false;
    }
    if (name == "cargo") {
        return one_of(arg(0), {"install", "uninstall", "add", "remove", "update"});
    }
    if (name == "go") {
        return one_of(arg(0), {"install", "get"});
    }
    if (name == "gem") {
        return one_of(arg(0), {"install", "update", "uninstall"});
    }
    if (name == "dotnet") {
        return arg(0) == "add" || arg(0) == "workload" ||
               (arg(0) == "tool" && one_of(arg(1), {"install", "update", "uninstall", "restore"})) ||
               (arg(0) == "new" && one_of(arg(1), {"install", "uninstall"}));
    }
    return false;
}

class Judge {
public:
    explicit Judge(std::vector<Path>& paths) : paths_(paths) {}

    auto part(const Part& p) -> Judged {
        Judged j = judge(p);
        for (const auto& r : p.redirects) {
            add(r, Use::Change);
            j = worse(j, {Class::Ask, "writes its output to a file", {}});
        }
        return j;
    }

private:
    static auto worse(Judged a, Judged b) -> Judged {
        return b.cls > a.cls ? std::move(b) : std::move(a);
    }

    void add(const Token& t, Use use, bool tree = false) {
        if (t.text.empty() || t.variable || t.expr) {
            return;
        }
        std::string text = t.text;
        if (provider_of(text) != Provider::File) {
            return;
        }
        if (t.wildcard || text.find_first_of("*?") != std::string::npos) {
            text = folder_of(text);
        }
        paths_.push_back({wide(text), use, tree});
    }

    auto judge(const Part& p) -> Judged {
        if (p.call) {
            return {Class::Ask, "runs what & or . names: the guard cannot know what that is", {}};
        }
        if (p.tokens.empty()) {
            return {Class::Auto, {}, {}};
        }
        const Token& head = p.tokens.front();
        if (head.quoted || head.variable || head.expr || head.text.empty() || (head.text[0] >= '0' && head.text[0] <= '9')) {
            return {Class::Ask, "runs a PowerShell expression", {}};
        }
        const std::string name = name_of(head);
        const std::span<const Token> args{p.tokens.begin() + 1, p.tokens.end()};
        const auto arg = [&](std::size_t i) { return i < args.size() ? lower(args[i].text) : std::string{}; };
        const auto any_arg = [&](auto pred) { return std::ranges::any_of(args, [&](const Token& t) { return pred(lower(t.text)); }); };

        // 1. hard-deny
        const auto never = [&](std::string_view what) -> Judged {
            return {Class::Deny, std::format("{}: the agent never runs it (03-SECURITY)", what), {}};
        };
        if (one_of(name, {"format", "diskpart", "bcdedit", "bootrec", "set-executionpolicy", "stop-computer", "restart-computer",
                          "shutdown", "takeown", "format-volume", "clear-disk", "initialize-disk", "remove-partition", "runas"})) {
            return never(head.text);
        }
        if (name == "cipher" && any_arg([](const std::string& a) { return a.starts_with("/w"); })) {
            return never("cipher /w");
        }
        if (name == "vssadmin" && arg(0) == "delete") {
            return never("vssadmin delete");
        }
        if (name == "wmic" && any_arg([](const std::string& a) { return a == "shadowcopy"; })) {
            return never("wmic shadowcopy");
        }
        if (name == "reg" && (arg(0) == "add" || arg(0) == "delete") && (arg(1).starts_with("hklm") || arg(1).starts_with("hkey_local_machine"))) {
            return never("reg " + arg(0) + " HKLM");
        }
        if (name == "sc" && (arg(0) == "delete" || arg(0) == "config")) {
            return never("sc " + arg(0));
        }
        if ((name == "net" || name == "net1") && arg(0) == "user") {
            return never("net user");
        }
        if (name == "schtasks" && any_arg([](const std::string& a) { return a == "/create"; })) {
            return never("schtasks /create");
        }
        if (one_of(name, {"start-process", "saps", "start"}) &&
            any_arg([](const std::string& a) { return a == "runas" || a == "-verb:runas"; })) {
            return never("running as administrator");
        }

        // 2. ask, every time
        const auto asked = [&](std::string why) { return Judged{Class::Ask, std::move(why), {}}; };
        if (one_of(name, {"remove-item", "rm", "del", "erase", "rd", "rmdir", "ri", "clear-content", "clc", "remove-itemproperty", "rp"})) {
            // -r, -Rec, -Recurse:$true, –Recurse (a dash PowerShell reads as -); /s alone or with others (/s/q).
            const bool tree = std::ranges::any_of(args, [](const Token& t) { return names_param(t, "-recurse") || has_slash_flag(t, "s"); });
            changes(args, tree);
            return asked("deletes: asked every time");
        }
        if (one_of(name, {"move-item", "mv", "move", "mi", "rename-item", "ren", "rni"})) {
            changes(args, false);
            return asked("moves or renames: asked every time");
        }
        if (one_of(name, {"copy-item", "cp", "copy", "cpi"})) {
            copies(args);
            return asked("copies: asked every time");
        }
        if (one_of(name, {"set-content", "add-content", "ac", "out-file", "new-item", "ni", "mkdir", "md", "set-item", "si",
                          "clear-item", "cli", "tee-object", "tee", "export-csv", "epcsv", "export-clixml", "set-itemproperty", "sp",
                          "new-itemproperty", "icacls", "attrib", "robocopy", "xcopy"})) {
            changes(args, false);
            return asked("writes: asked every time");
        }
        if (name == "sc") {
            changes(args, false); // PowerShell 5's Set-Content, or the services' tool
            return asked("changes files or services: asked every time");
        }
        if (one_of(name, {"stop-process", "spps", "kill", "taskkill"})) {
            return asked("ends a process: asked every time");
        }
        if (one_of(name, {"start-process", "saps", "start"})) {
            return asked("starts a program: asked every time");
        }
        if (one_of(name, {"invoke-webrequest", "iwr", "curl", "wget", "invoke-restmethod", "irm", "start-bitstransfer", "bitsadmin",
                          "certutil"})) {
            return asked("reaches the network: asked every time");
        }
        if (installs(name, args)) {
            return asked("installs or changes software: asked every time");
        }
        if (one_of(name, {"invoke-expression", "iex", "invoke-command", "icm", "start-job", "sajb"})) {
            return asked("runs code it is given: asked every time");
        }
        if (one_of(name, {"cmd", "powershell", "pwsh", "bash", "sh", "wsl", "wscript", "cscript", "mshta", "rundll32", "regsvr32"})) {
            return asked("runs another shell or script host: it may do anything");
        }
        if (one_of(name, {"net", "net1", "netsh", "reg", "schtasks", "wmic", "set-service", "stop-service", "spsv", "start-service",
                          "sasv", "restart-service", "new-service", "remove-service", "suspend-service"})) {
            return asked("changes the system's settings or services: asked every time");
        }

        // A program named by its path is not the list's command of that name (final review S1):
        // ".\tools\git" may be any script the model wrote.
        if (head.text.find_first_of("\\/:") != std::string::npos) {
            for (const auto& t : args) {
                if (!option(t) && !slash_option(t) && path_like(t)) {
                    add(t, Use::Run);
                }
            }
            return asked("runs a program named by its path");
        }

        // 3. auto: only reading, as the list reads
        if (one_of(name, {"get-childitem", "gci", "ls", "dir", "get-item", "gi", "resolve-path", "rvpa"})) {
            listing_.push_back(p.pipeline); // what it gives to the next part are files (final review S7)
        }
        if (name == "git") {
            return git(head, args);
        }
        if (const auto r = reading(name, args, p)) {
            return *r;
        }

        // 4. everything else
        return other(p, head, name, args);
    }

    // Every word a changing command names is a path it changes (a value taken for one only errs to stricter).
    void changes(std::span<const Token> args, bool tree) {
        for (std::size_t i = 0; i < args.size(); ++i) {
            const auto& t = args[i];
            if (option(t)) {
                const auto o = lower(t.text);
                if (const auto colon = o.find(':'); colon != std::string::npos) {
                    add({.text = t.text.substr(colon + 1)}, Use::Change, tree);
                } else if (one_of(o, {"-value", "-itemtype", "-type", "-encoding", "-filter", "-include", "-exclude", "-newname",
                                      "-name", "-stream", "-propertytype", "-delimiter"})) {
                    ++i;
                }
                continue;
            }
            if (!slash_option(t)) {
                add(t, Use::Change, tree);
            }
        }
    }

    // Copy-Item: the sources read, the destination changed.
    void copies(std::span<const Token> args) {
        std::size_t positional = 0;
        for (std::size_t i = 0; i < args.size(); ++i) {
            const auto& t = args[i];
            if (option(t)) {
                const auto o = lower(t.text);
                if ((o == "-destination" || o == "-dest") && i + 1 < args.size()) {
                    add(args[++i], Use::Change);
                } else if (one_of(o, {"-filter", "-include", "-exclude"})) {
                    ++i;
                }
                continue;
            }
            if (!slash_option(t)) {
                add(t, positional++ == 0 ? Use::Read : Use::Change);
            }
        }
    }

    // git status|log|diff: auto; the rest asked.
    auto git(const Token& head, std::span<const Token> args) -> Judged {
        std::size_t i = 0;
        std::optional<Token> folder;
        while (i < args.size() && option(args[i])) {
            const auto o = lower(args[i].text);
            if (o == "-c" && i + 1 < args.size() && args[i].text == "-C") { // -C <folder>
                folder = args[i + 1];
                i += 2;
            } else if (o == "--no-pager") {
                ++i;
            } else {
                return {Class::Ask, "git with options before its command (a configuration may run anything)", {}};
            }
        }
        if (i >= args.size()) {
            return {Class::Other, "runs git", "git"};
        }
        const auto sub = lower(args[i].text);
        const auto rest = args.subspan(i + 1);
        const auto has = [&](auto pred) { return std::ranges::any_of(rest, [&](const Token& t) { return pred(lower(t.text)); }); };
        if (sub == "push") {
            return {Class::Ask, "git push: asked every time", {}};
        }
        // What throws work away, asked every time (final review, ruled): reset --hard, clean, checkout,
        // restore, stash drop|clear, branch -d|-D.
        const bool throws = sub == "clean" || sub == "checkout" || sub == "restore" ||
                            (sub == "reset" && has([](const std::string& a) { return a == "--hard"; })) ||
                            (sub == "stash" && !rest.empty() && one_of(lower(rest[0].text), {"drop", "clear"})) ||
                            (sub == "branch" && has([](const std::string& a) { return a == "-d" || a == "--delete" || a == "-f"; }));
        if (throws) {
            return {Class::Ask, "git " + sub + " may throw work away: asked every time", {}};
        }
        if (one_of(sub, {"status", "log", "diff"})) {
            if (has([](const std::string& a) {
                    return a.starts_with("--output") || a == "--ext-diff" || a == "--no-index" || a.starts_with("--exec");
                })) {
                return {Class::Ask, "git " + sub + " writes a file or reads outside the repository", {}};
            }
            if (std::ranges::any_of(rest, [](const Token& t) { return t.variable || t.expr; })) {
                return {Class::Ask, "names a variable or an expression: what it reads cannot be known", {}};
            }
            if (folder) {
                add(*folder, Use::List);
            }
            return {Class::Auto, {}, {}};
        }
        if (!plain(args[i])) {
            return {Class::Ask, "runs git", {}};
        }
        return {Class::Other, "runs git " + args[i].text, head.text + " " + args[i].text};
    }

    // The allow-list's reading commands; nullopt: not one of them.
    auto reading(const std::string& name, std::span<const Token> args, const Part& p) -> std::optional<Judged> {
        enum class Kind : std::uint8_t { Content, List, Select, Property, Values, Find, Info, Ip, Tasks, Where };
        Kind kind{};
        if (one_of(name, {"get-content", "gc", "cat", "type"})) {
            kind = Kind::Content;
        } else if (one_of(name, {"get-childitem", "gci", "ls", "dir"})) {
            kind = Kind::List;
        } else if (one_of(name, {"select-string", "sls"})) {
            kind = Kind::Select;
        } else if (one_of(name, {"get-itemproperty", "gp"})) {
            kind = Kind::Property;
        } else if (one_of(name, {"measure-object", "measure", "get-process", "gps", "ps", "get-service", "gsv"})) {
            kind = Kind::Values;
        } else if (name == "findstr") {
            kind = Kind::Find;
        } else if (name == "systeminfo") {
            kind = Kind::Info;
        } else if (name == "ipconfig") {
            kind = Kind::Ip;
        } else if (name == "tasklist") {
            kind = Kind::Tasks;
        } else if (one_of(name, {"where", "where-object", "?"})) {
            kind = Kind::Where;
        } else {
            return std::nullopt;
        }
        const auto not_so = [](std::string why) { return Judged{Class::Ask, "reads, but not as the guard lets it: " + why, {}}; };
        if (std::ranges::any_of(args, [](const Token& t) { return t.variable || t.expr; })) {
            return Judged{Class::Ask, "names a variable or an expression: what it reads cannot be known", {}};
        }
        if (std::ranges::any_of(args, [](const Token& t) {
                const auto a = lower(t.text);
                return option(t) && (a.starts_with("-comp") || a == "-cn" || a.starts_with("-sess") || a.starts_with("-cims"));
            })) {
            return not_so("on another computer");
        }
        // What the pipe gives it is not in its arguments (final review S7): paths for these three; for
        // Select-String, files when a part before it in the pipeline lists them (strings otherwise).
        if (p.piped) {
            if (kind == Kind::Content || kind == Kind::Property || kind == Kind::List) {
                return not_so("it takes its paths from the part before it");
            }
            if (kind == Kind::Select && std::ranges::find(listing_, p.pipeline) != listing_.end()) {
                return not_so("it searches the files the part before it lists");
            }
        }
        const auto natives = [&](Names allowed) -> std::optional<Judged> {
            for (std::size_t i = 0; i < args.size(); ++i) {
                const auto a = lower(args[i].text);
                if (a == "/fo" && i + 1 < args.size()) {
                    ++i;
                } else if (!one_of(a, allowed)) {
                    return not_so(args[i].text);
                }
            }
            return Judged{Class::Auto, {}, {}};
        };
        switch (kind) {
        case Kind::Info: return natives({"/nh", "/?"});
        case Kind::Ip: return natives({"/all", "/displaydns", "/allcompartments", "/?"});
        case Kind::Tasks:
            if (std::ranges::any_of(args, [](const Token& t) { return one_of(lower(t.text), {"/s", "/u", "/p"}); })) {
                return not_so("on another computer");
            }
            return Judged{Class::Auto, {}, {}};
        case Kind::Values: return Judged{Class::Auto, {}, {}};
        case Kind::Where:
            for (std::size_t i = 0; i + 1 < args.size(); ++i) {
                if (lower(args[i].text) == "/r") {
                    add(args[i + 1], Use::List, true); // every folder below (final review S9)
                }
            }
            return Judged{Class::Auto, {}, {}};
        case Kind::Find: return findstr(args, not_so);
        default: break;
        }

        // Get-Content, Get-ChildItem, Select-String, Get-ItemProperty: their paths, by parameter and position.
        const Use use = kind == Kind::List ? Use::List : Use::Read;
        const bool registry = kind == Kind::List || kind == Kind::Property;
        // Their parameters as PowerShell takes them (final review S2): by name, by alias, or by any start
        // of a name no other one shares. An alias is taken whole only.
        enum class Role : std::uint8_t { Path, Value, Switch, Recurse, Depth };
        struct Param {
            std::string_view name;
            Role role;
            bool alias = false;
        };
        static constexpr std::array kPaths{Param{"-path", Role::Path}, Param{"-literalpath", Role::Path}, Param{"-pspath", Role::Path, true},
                                           Param{"-lp", Role::Path, true}};
        static constexpr std::array kContent{
            Param{"-totalcount", Role::Value}, Param{"-head", Role::Value, true}, Param{"-first", Role::Value, true},
            Param{"-tail", Role::Value},       Param{"-last", Role::Value, true}, Param{"-readcount", Role::Value},
            Param{"-encoding", Role::Value},   Param{"-delimiter", Role::Value},  Param{"-stream", Role::Value},
            Param{"-filter", Role::Value},     Param{"-include", Role::Value},    Param{"-exclude", Role::Value},
            Param{"-raw", Role::Switch},       Param{"-wait", Role::Switch},      Param{"-force", Role::Switch},
            Param{"-asbytestream", Role::Switch}};
        static constexpr std::array kList{
            Param{"-filter", Role::Value},      Param{"-include", Role::Value},       Param{"-exclude", Role::Value},
            Param{"-attributes", Role::Value},  Param{"-attr", Role::Value, true},    Param{"-depth", Role::Depth},
            Param{"-recurse", Role::Recurse},   Param{"-s", Role::Recurse, true},     Param{"-r", Role::Recurse, true},
            Param{"-force", Role::Switch},      Param{"-name", Role::Switch},         Param{"-directory", Role::Switch},
            Param{"-file", Role::Switch},       Param{"-hidden", Role::Switch},       Param{"-readonly", Role::Switch},
            Param{"-system", Role::Switch},     Param{"-followsymlink", Role::Switch}, Param{"-ad", Role::Switch, true},
            Param{"-d", Role::Switch, true},    Param{"-af", Role::Switch, true},     Param{"-ah", Role::Switch, true},
            Param{"-h", Role::Switch, true},    Param{"-ar", Role::Switch, true},     Param{"-as", Role::Switch, true}};
        static constexpr std::array kSelect{
            Param{"-pattern", Role::Value},      Param{"-encoding", Role::Value},      Param{"-context", Role::Value},
            Param{"-include", Role::Value},      Param{"-exclude", Role::Value},       Param{"-culture", Role::Value},
            Param{"-inputobject", Role::Value},  Param{"-simplematch", Role::Switch},  Param{"-casesensitive", Role::Switch},
            Param{"-quiet", Role::Switch},       Param{"-list", Role::Switch},         Param{"-notmatch", Role::Switch},
            Param{"-allmatches", Role::Switch},  Param{"-raw", Role::Switch},          Param{"-noemphasis", Role::Switch}};
        static constexpr std::array kProperty{Param{"-name", Role::Value}, Param{"-filter", Role::Value}, Param{"-include", Role::Value},
                                              Param{"-exclude", Role::Value}};
        const std::span<const Param> own = kind == Kind::Content ? std::span<const Param>{kContent}
                                           : kind == Kind::List  ? std::span<const Param>{kList}
                                           : kind == Kind::Select ? std::span<const Param>{kSelect}
                                                                  : std::span<const Param>{kProperty};
        // nullptr: a parameter it does not have, or a start two share.
        const auto param_of = [&](const Token& t) -> const Param* {
            const auto l = lower(t.text);
            const std::string_view key = std::string_view{l}.substr(0, l.find(':'));
            const Param* found = nullptr;
            int starts = 0;
            for (const auto list : {std::span<const Param>{kPaths}, own}) {
                for (const auto& prm : list) {
                    if (prm.name == key) {
                        return &prm;
                    }
                    if (!prm.alias && prm.name.starts_with(key)) {
                        found = &prm;
                        ++starts;
                    }
                }
            }
            return starts == 1 ? found : nullptr;
        };
        bool strict = false, pattern_given = false, tree = false;
        for (const auto& t : args) {
            if (!option(t)) {
                continue;
            }
            const Param* prm = param_of(t);
            strict = strict || prm == nullptr;
            pattern_given = pattern_given || (prm != nullptr && prm->name == "-pattern");
            tree = tree || (prm != nullptr && (prm->role == Role::Depth ||
                                               (prm->role == Role::Recurse && !lower(t.text).ends_with(":$false"))));
        }
        std::vector<const Token*> named; // paths
        std::size_t positional = 0;
        for (std::size_t i = 0; i < args.size(); ++i) {
            const auto& t = args[i];
            if (option(t)) {
                const Param* prm = param_of(t);
                const auto colon = t.text.find(':');
                if (colon != std::string::npos) {
                    if (prm == nullptr || prm->role == Role::Path) { // an unknown one's value is taken as a path (only stricter)
                        path_words_.push_back({.text = t.text.substr(colon + 1)});
                        named.push_back(&path_words_.back());
                    }
                } else if (prm != nullptr && prm->role == Role::Path && i + 1 < args.size()) {
                    named.push_back(&args[++i]);
                } else if (prm != nullptr && (prm->role == Role::Value || prm->role == Role::Depth)) {
                    ++i;
                }
                continue; // an unknown one: what follows is taken as a path
            }
            const std::size_t at = positional++;
            if (!strict && ((kind == Kind::Select && at == 0 && !pattern_given) || (kind == Kind::List && at >= 1) ||
                            (kind == Kind::Property && at >= 1))) {
                continue; // a pattern, a filter, a property's name
            }
            named.push_back(&t);
        }
        if (kind == Kind::List && tree && named.empty()) {
            path_words_.push_back({.text = "."}); // the working folder, every folder below it
            named.push_back(&path_words_.back());
        }
        for (const Token* t : named) {
            std::string text = t->text;
            const auto provider = provider_of(text);
            if (provider == Provider::Registry && registry) {
                continue;
            }
            if (provider != Provider::File) {
                return not_so(t->text + " is not a file");
            }
            if (t->wildcard && use == Use::Read) {
                return not_so("a wildcard names the files it reads");
            }
            add(*t, use, tree);
        }
        return Judged{Class::Auto, {}, {}};
    }

    auto findstr(std::span<const Token> args, auto not_so) -> std::optional<Judged> {
        bool pattern = false;
        for (const auto& t : args) {
            const auto a = lower(t.text);
            if (slash_option(t) || (a.starts_with('/') && a.find(':') == 2)) {
                // /s, alone or among others (/sim, /s/i): files of that name in every folder below (final review S8).
                if (a.find(':') == std::string::npos && a.find('s') != std::string::npos) {
                    return not_so("/s searches every folder below");
                }
                if (a.starts_with("/c:")) {
                    pattern = true;
                } else if (a.starts_with("/g:") || a.starts_with("/f:")) {
                    pattern = pattern || a.starts_with("/g:");
                    add({.text = t.text.substr(3)}, Use::Read);
                } else if (a.starts_with("/d:")) {
                    std::string_view dirs = std::string_view{t.text}.substr(3);
                    while (!dirs.empty()) {
                        const auto cut = dirs.find(';');
                        add({.text = std::string{dirs.substr(0, cut)}}, Use::List);
                        dirs = cut == std::string_view::npos ? std::string_view{} : dirs.substr(cut + 1);
                    }
                }
                continue;
            }
            if (!pattern) {
                pattern = true;
                continue;
            }
            if (t.wildcard) {
                return not_so("a wildcard names the files it reads");
            }
            add(t, Use::Read);
        }
        return Judged{Class::Auto, {}, {}};
    }

    // Not on any list: asked; its start ("npm test", "cmake --build") offered for the task when it can be told.
    auto other(const Part& p, const Token& head, const std::string& name, std::span<const Token> args) -> Judged {
        const std::string why = "runs " + head.text + ": not on the list of commands that only read";
        for (const auto& t : args) {
            if (!option(t) && !slash_option(t) && path_like(t)) {
                add(t, Use::Run);
            }
        }
        const bool readable = !p.nested && plain(head) && head.text.find_first_of("\\/") == std::string::npos &&
                              std::ranges::none_of(args, [](const Token& t) { return t.variable || t.expr; });
        if (!readable) {
            return {Class::Ask, why, {}};
        }
        const bool interpreter = one_of(name, {"python", "python3", "py", "node", "deno", "ruby", "perl", "php"});
        if (args.empty()) {
            return {Class::Other, why, head.text};
        }
        const Token& a = args[0];
        if (plain(a) && a.text[0] != '-') {
            return {Class::Other, why, head.text + " " + a.text};
        }
        if (interpreter) {
            if (lower(a.text) == "-m" && args.size() > 1 && plain(args[1]) && args[1].text[0] != '-') {
                return {Class::Other, why, head.text + " -m " + args[1].text};
            }
            return {Class::Ask, why, {}}; // -c / -e: code on the command line
        }
        if (plain(a) && a.text.starts_with("--")) {
            return {Class::Other, why, head.text + " " + a.text};
        }
        if (plain(a) && a.text.size() > 1 && a.text[0] == '-' && a.text[1] != '-') {
            if (args.size() > 1 && plain(args[1]) && args[1].text[0] != '-') {
                return {Class::Other, why, head.text + " " + a.text + " " + args[1].text};
            }
            return {Class::Other, why, head.text};
        }
        if (slash_option(a)) {
            return {Class::Other, why, head.text};
        }
        return {Class::Ask, why, {}};
    }

    std::vector<Path>& paths_;
    std::deque<Token> path_words_;       // values cut from -Path:value, kept while their pointers are
    std::vector<std::uint32_t> listing_; // the pipelines a part of which gives files to the next
};

// The command as a whole, beyond its parts: an encoded command, a download run, elevation.
auto never_as_a_whole(std::string_view command, const Parsed& parsed) -> std::optional<std::string> {
    const auto l = lower(std::string{command});
    if (l.find("powershell") != std::string::npos || l.find("pwsh") != std::string::npos) {
        std::string word;
        const auto check = [&] {
            std::string w = word;
            word.clear();
            if (w.size() >= 2 && w[0] == '/') {
                w[0] = '-';
            }
            return w.size() >= 2 && w.starts_with("-e") &&
                   (std::string_view{"-encodedcommand"}.starts_with(w) || w == "-ec" || std::string_view{"-encodedarguments"}.starts_with(w));
        };
        for (const char c : l) {
            if (space(c) || c == '\n' || c == '\'' || c == '"' || c == ',' || c == '(' || c == ')') {
                if (check()) {
                    return "an encoded command: the agent never runs it (03-SECURITY)";
                }
            } else {
                word += c;
            }
        }
        if (check()) {
            return "an encoded command: the agent never runs it (03-SECURITY)";
        }
    }
    // The same read on the words as PowerShell reads them, escapes gone and a call's name included
    // (final review S6): p`owershell -enc, & 'pwsh' -e, ht`tps://.
    bool runs = false, fetches = false;
    for (const auto& p : parsed.parts) {
        if (p.tokens.empty()) {
            continue;
        }
        const auto n = name_of(p.tokens.front());
        if (one_of(n, {"powershell", "pwsh"})) {
            for (const auto& t : std::span{p.tokens}.subspan(1)) {
                auto w = lower(t.text);
                if (w.size() >= 2 && w[0] == '/') {
                    w[0] = '-';
                }
                if (w.size() >= 2 && w.starts_with("-e") &&
                    (std::string_view{"-encodedcommand"}.starts_with(w) || w == "-ec" || std::string_view{"-encodedarguments"}.starts_with(w))) {
                    return "an encoded command: the agent never runs it (03-SECURITY)";
                }
            }
        }
        runs = runs || n == "iex" || n == "invoke-expression";
        fetches = fetches || one_of(n, {"invoke-webrequest", "iwr", "curl", "wget", "invoke-restmethod", "irm", "start-bitstransfer"}) ||
                  std::ranges::any_of(p.tokens, [](const Token& t) {
                      const auto w = lower(t.text);
                      return w.find("http://") != std::string::npos || w.find("https://") != std::string::npos ||
                             w.find("downloadstring") != std::string::npos || w.find("net.webclient") != std::string::npos;
                  });
    }
    fetches = fetches || l.find("downloadstring") != std::string::npos || l.find("downloaddata") != std::string::npos ||
              l.find("downloadfile") != std::string::npos || l.find("net.webclient") != std::string::npos ||
              l.find("http://") != std::string::npos || l.find("https://") != std::string::npos;
    if (runs && fetches) {
        return "runs what it downloads: the agent never does (03-SECURITY)";
    }
    return std::nullopt;
}

} // namespace

auto classify(std::string_view command) -> Verdict {
    Verdict v;
    const auto parsed = Reader{command}.read();
    if (const auto why = never_as_a_whole(command, parsed)) {
        v.decision = Decision::HardDeny;
        v.reason = *why;
        return v;
    }
    Judge judge{v.paths};
    Judged worst{Class::Auto, {}, {}};
    std::optional<std::string> start; // the one start of the parts asked as "everything else"
    bool one_start = true;
    for (const auto& p : parsed.parts) {
        auto j = judge.part(p);
        if (j.cls == Class::Other) {
            if (start && lower(*start) != lower(j.prefix)) {
                one_start = false;
            }
            start = j.prefix;
        }
        if (j.cls > worst.cls) {
            worst = std::move(j);
        }
    }
    if (!parsed.unread.empty() && worst.cls < Class::Ask) {
        worst = {Class::Ask, "a part of it the guard cannot read: " + parsed.unread, {}};
    }
    if (parsed.parts.empty() && worst.cls == Class::Auto) {
        worst = {Class::Ask, "nothing the guard can read as a command", {}};
    }
    switch (worst.cls) {
    case Class::Auto:
        v.decision = Decision::Auto;
        v.reason = "only reads: on the shell guard's list";
        break;
    case Class::Other:
        v.decision = Decision::Ask;
        v.reason = worst.reason;
        v.prefix = one_start && start && std::ranges::none_of(parsed.parts, [](const Part& p) { return p.nested; }) ? *start : std::string{};
        break;
    case Class::Ask:
        v.decision = Decision::Ask;
        v.reason = worst.reason;
        break;
    case Class::Deny:
        v.decision = Decision::HardDeny;
        v.reason = worst.reason;
        break;
    }
    return v;
}

} // namespace security::shellguard
