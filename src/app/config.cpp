#include "app/config.hpp"

#include "core/fs.hpp"
#include "core/json.hpp"

#include <glaze/glaze.hpp>

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <format>
#include <system_error>

namespace app::config {
namespace {

using Json = glz::generic;

// 1-based line of the byte at `offset` (where a JSON error stopped the reader).
auto line_at(std::string_view text, std::size_t offset) -> int {
    offset = std::min(offset, text.size());
    return 1 + static_cast<int>(std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(offset), '\n'));
}

auto trim(std::string_view s) -> std::string_view {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

auto lower(std::string_view s) -> std::string {
    std::string out{s};
    std::ranges::transform(out, out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

constexpr std::pair<std::string_view, Gpu> kGpus[] = {
    {"auto", Gpu::Auto}, {"integrated", Gpu::Integrated}, {"discrete", Gpu::Discrete}};
constexpr std::pair<std::string_view, Backdrop> kBackdrops[] = {
    {"acrylic", Backdrop::Acrylic}, {"solid", Backdrop::Solid}};

template <class E, std::size_t N>
auto name_of(const std::pair<std::string_view, E> (&table)[N], E value) -> std::string_view {
    for (const auto& [name, v] : table) {
        if (v == value) {
            return name;
        }
    }
    return table[0].first;
}

template <class E, std::size_t N>
auto value_of(const std::pair<std::string_view, E> (&table)[N], std::string_view name)
    -> std::optional<E> {
    const std::string key = lower(name);
    for (const auto& [n, v] : table) {
        if (n == key) {
            return v;
        }
    }
    return std::nullopt;
}

auto parse_scale(std::string_view s) -> std::optional<float> {
    float v = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || end != s.data() + s.size() || !(v >= 0.75f && v <= 2.0f)) {
        return std::nullopt;
    }
    return v;
}

auto parse_bool(std::string_view s) -> std::optional<bool> {
    const std::string v = lower(s);
    if (v == "true" || v == "yes" || v == "on" || v == "1") {
        return true;
    }
    if (v == "false" || v == "no" || v == "off" || v == "0") {
        return false;
    }
    return std::nullopt;
}

struct KeyName {
    std::string_view name;
    unsigned key;
};
constexpr KeyName kNamedKeys[] = {
    {"Space", VK_SPACE},   {"Enter", VK_RETURN}, {"Tab", VK_TAB},     {"Esc", VK_ESCAPE},
    {"Insert", VK_INSERT}, {"Home", VK_HOME},    {"End", VK_END},     {"PageUp", VK_PRIOR},
    {"PageDown", VK_NEXT}, {"Up", VK_UP},        {"Down", VK_DOWN},   {"Left", VK_LEFT},
    {"Right", VK_RIGHT},   {"`", VK_OEM_3},      {"/", VK_OEM_2},     {".", VK_OEM_PERIOD},
};

auto key_of(std::string_view token) -> std::optional<unsigned> {
    if (token.size() == 1 && std::isalnum(static_cast<unsigned char>(token[0]))) {
        return static_cast<unsigned>(std::toupper(static_cast<unsigned char>(token[0])));
    }
    const std::string t = lower(token);
    if (t.size() >= 2 && t.size() <= 3 && t[0] == 'f') {
        int n = 0;
        for (std::size_t i = 1; i < t.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(t[i]))) {
                return std::nullopt;
            }
            n = n * 10 + (t[i] - '0');
        }
        if (n >= 1 && n <= 24) {
            return static_cast<unsigned>(VK_F1 + n - 1);
        }
        return std::nullopt;
    }
    for (const auto& k : kNamedKeys) {
        if (lower(k.name) == t) {
            return k.key;
        }
    }
    return std::nullopt;
}

auto key_name(unsigned key) -> std::string {
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) {
        return std::string(1, static_cast<char>(key));
    }
    if (key >= VK_F1 && key <= VK_F24) {
        return std::format("F{}", key - VK_F1 + 1);
    }
    for (const auto& k : kNamedKeys) {
        if (k.key == key) {
            return std::string{k.name};
        }
    }
    return std::format("0x{:02X}", key);
}

// "ai.roles" (stage 4c): each role as written, or its default when anything in it is wrong (said).
// Chat names no model: it answers with the one chosen in the menu.
template <class Bad>
void read_roles(const Json& j, ai::routing::Roles& roles, const Bad& bad) {
    if (!j.is_object()) {
        bad("ai.roles", "expected an object");
        return;
    }
    const auto known = [](const Json& v) { return v.is_string() && ai::catalog::model(v.get_string()) != nullptr; };
    for (auto [name, role] : {std::pair{"chat", &roles.chat}, std::pair{"hard", &roles.hard}, std::pair{"images", &roles.images}}) {
        if (!j.contains(name)) {
            continue;
        }
        const Json& r = j[name];
        const std::string key = std::format("ai.roles.{}", name);
        if (!r.is_object()) {
            bad(key, "expected an object");
            continue;
        }
        ai::routing::Role got = *role; // a field left out keeps its default
        if (r.contains("model")) {
            const Json& m = r["model"];
            if (role == &roles.chat) {
                bad(key + ".model", "not set: Everyday chat answers with the model chosen in the menu");
                continue;
            }
            if (!(m.is_string() && (m.get_string().empty() || known(m)))) {
                bad(key + ".model", "a model's id from the catalog, or \"\" for the one chosen");
                continue;
            }
            got.model = m.get_string();
        }
        if (r.contains("fallbacks")) {
            const Json& f = r["fallbacks"];
            if (!f.is_array() || !std::ranges::all_of(f.get_array(), known)) {
                bad(key + ".fallbacks", "a list of models' ids from the catalog");
                continue;
            }
            got.fallbacks.clear();
            for (const auto& id : f.get_array()) {
                got.fallbacks.push_back(id.get_string());
            }
        }
        *role = std::move(got);
    }
}

void write_role(Json& out, const ai::routing::Role& role, bool with_model) {
    out = Json::object_t{};
    if (with_model) {
        out["model"] = role.model;
    }
    Json::array_t list;
    for (const auto& id : role.fallbacks) {
        list.emplace_back(id);
    }
    out["fallbacks"] = std::move(list);
}

} // namespace

auto parse_hotkey(std::string_view text) -> std::optional<Hotkey> {
    Hotkey h;
    bool have_key = false;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto plus = text.find('+', start);
        const std::string_view token =
            trim(text.substr(start, plus == std::string_view::npos ? std::string_view::npos : plus - start));
        if (token.empty() || have_key) {
            return std::nullopt; // "Ctrl+" or something after the key
        }
        const std::string t = lower(token);
        unsigned modifier = 0;
        if (t == "ctrl" || t == "control") {
            modifier = MOD_CONTROL;
        } else if (t == "alt") {
            modifier = MOD_ALT;
        } else if (t == "shift") {
            modifier = MOD_SHIFT;
        } else if (t == "win") {
            modifier = MOD_WIN;
        }
        if (modifier != 0) {
            if ((h.modifiers & modifier) != 0) {
                return std::nullopt; // repeated
            }
            h.modifiers |= modifier;
        } else {
            const auto key = key_of(token);
            if (!key) {
                return std::nullopt;
            }
            h.key = *key;
            have_key = true;
        }
        if (plus == std::string_view::npos) {
            break;
        }
        start = plus + 1;
    }
    if (!have_key || h.modifiers == 0) {
        return std::nullopt;
    }
    return h;
}

auto hotkey_name(const Hotkey& h) -> std::string {
    std::string out;
    const std::pair<unsigned, std::string_view> order[] = {
        {MOD_CONTROL, "Ctrl"}, {MOD_ALT, "Alt"}, {MOD_SHIFT, "Shift"}, {MOD_WIN, "Win"}};
    for (const auto& [bit, name] : order) {
        if ((h.modifiers & bit) != 0) {
            out += name;
            out += '+';
        }
    }
    return out + key_name(h.key);
}

auto parse_ini(std::string_view text) -> Parsed {
    if (text.starts_with("\xEF\xBB\xBF")) { // Notepad's UTF-8 mark would hide the first key
        text.remove_prefix(3);
    }
    Parsed out;
    Json unknown = Json::object_t{};
    int number = 0;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const auto newline = text.find('\n', pos);
        const std::string_view raw =
            text.substr(pos, newline == std::string_view::npos ? std::string_view::npos : newline - pos);
        pos = newline == std::string_view::npos ? text.size() : newline + 1;
        ++number;
        const std::string_view line = trim(raw);
        if (line.empty() || line.front() == '#' || line.front() == ';') {
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string_view::npos) {
            out.problems.push_back({number, "expected key = value"});
            continue;
        }
        const std::string key = lower(trim(line.substr(0, eq)));
        const std::string_view value = trim(line.substr(eq + 1));
        const auto bad = [&] {
            out.problems.push_back({number, std::format("bad value for {}: '{}'", key, value)});
        };
        if (key == "hotkey") {
            if (const auto h = parse_hotkey(value)) {
                out.config.hotkey = hotkey_name(*h);
            } else {
                bad();
            }
        } else if (key == "gpu") {
            if (const auto g = value_of(kGpus, value)) {
                out.config.gpu = *g;
            } else {
                bad();
            }
        } else if (key == "over_fullscreen") {
            if (const auto b = parse_bool(value)) {
                out.config.over_fullscreen = *b;
            } else {
                bad();
            }
        } else if (key == "rest_animation") {
            if (const auto b = parse_bool(value)) {
                out.config.rest_animation = *b;
            } else {
                bad();
            }
        } else if (key == "text_scale") {
            if (const auto s = parse_scale(value)) {
                out.config.text_scale = *s;
            } else {
                bad();
            }
        } else if (key == "backdrop") {
            if (const auto b = value_of(kBackdrops, value)) {
                out.config.backdrop = *b;
            } else {
                bad();
            }
        } else {
            unknown[key] = std::string{value};
        }
    }
    if (!unknown.get_object().empty()) {
        if (auto doc = core::json::write(unknown)) {
            out.config.document = std::move(*doc);
        }
    }
    return out;
}

auto parse(std::string_view json) -> Parsed {
    if (json.starts_with("\xEF\xBB\xBF")) {
        json.remove_prefix(3);
    }
    Parsed out;
    if (trim(json).empty()) {
        return out;
    }
    const std::string buffer{json};
    Json doc;
    if (const auto ec = glz::read<glz::opts{.comments = true}>(doc, buffer)) {
        out.problems.push_back({line_at(buffer, ec.count), glz::format_error(ec, buffer)});
        return out;
    }
    if (!doc.is_object()) {
        out.problems.push_back({1, "the settings must be one JSON object { ... }"});
        return out;
    }
    out.config.document = buffer;
    Config& c = out.config;
    const auto bad = [&](std::string_view key, std::string_view why) {
        out.problems.push_back({0, std::format("bad value for {}: {}", key, why)});
    };
    // Each reader leaves the default when the key is missing and reports a wrong type or value.
    const auto text = [&](const Json& o, std::string_view key) -> std::optional<std::string> {
        if (!o.contains(key)) {
            return std::nullopt;
        }
        if (!o[key].is_string()) {
            bad(key, "expected a string");
            return std::nullopt;
        }
        return o[key].get<std::string>();
    };
    const auto flag = [&](const Json& o, std::string_view key) -> std::optional<bool> {
        if (!o.contains(key)) {
            return std::nullopt;
        }
        if (!o[key].is_boolean()) {
            bad(key, "expected true or false");
            return std::nullopt;
        }
        return o[key].get<bool>();
    };
    const auto number = [&](const Json& o, std::string_view key) -> std::optional<double> {
        if (!o.contains(key)) {
            return std::nullopt;
        }
        if (!o[key].is_number()) {
            bad(key, "expected a number");
            return std::nullopt;
        }
        return o[key].get<double>();
    };
    if (const auto v = text(doc, "hotkey")) {
        if (const auto h = parse_hotkey(*v)) {
            c.hotkey = hotkey_name(*h);
        } else {
            bad("hotkey", std::format("'{}' is not a shortcut", *v));
        }
    }
    if (const auto v = text(doc, "gpu")) {
        if (const auto g = value_of(kGpus, *v)) {
            c.gpu = *g;
        } else {
            bad("gpu", std::format("'{}' (auto, integrated, discrete)", *v));
        }
    }
    if (const auto v = flag(doc, "over_fullscreen")) {
        c.over_fullscreen = *v;
    }
    if (const auto v = text(doc, "backdrop")) {
        if (const auto b = value_of(kBackdrops, *v)) {
            c.backdrop = *b;
        } else {
            bad("backdrop", std::format("'{}' (acrylic, solid)", *v));
        }
    }
    if (const auto v = number(doc, "text_scale")) {
        if (*v >= 0.75 && *v <= 2.0) {
            c.text_scale = static_cast<float>(*v);
        } else {
            bad("text_scale", "0.75 .. 2");
        }
    }
    if (const auto v = number(doc, "transparency")) {
        if (*v >= 0 && *v <= kMostTransparency && std::fmod(*v, kTransparencyStep) == 0) {
            c.transparency = static_cast<int>(*v);
        } else {
            bad("transparency", "0 .. 50, a step of 5");
        }
    }
    if (const auto v = flag(doc, "rest_animation")) {
        c.rest_animation = *v;
    }
    if (doc.contains("frame_rate")) {
        const Json& f = doc["frame_rate"];
        const double hz = f.is_number() ? f.get<double>() : -1.0;
        if (f.is_string() && f.get_string() == "monitor") {
            c.frame_rate = FrameRate::Monitor;
        } else if (hz == 120) {
            c.frame_rate = FrameRate::Hz120;
        } else if (hz == 60) {
            c.frame_rate = FrameRate::Hz60;
        } else if (hz == 30) {
            c.frame_rate = FrameRate::Hz30;
        } else {
            bad("frame_rate", "\"monitor\", 120, 60 or 30");
        }
    }
    if (const auto v = text(doc, "theme")) {
        if (const auto choice = theme::choice_of(*v)) {
            c.theme = *choice;
        } else {
            bad("theme", "\"auto\", \"dark\" or \"light\"");
        }
    }
    if (doc.contains("advanced")) {
        const Json& a = doc["advanced"];
        if (!a.is_object()) {
            bad("advanced", "expected an object");
        } else {
            if (const auto v = flag(a, "allow_system_writes")) {
                c.advanced.allow_system_writes = *v;
            }
            if (const auto v = flag(a, "allow_secrets_read")) {
                c.advanced.allow_secrets_read = *v;
            }
            if (const auto v = flag(a, "allow_permanent_delete")) {
                c.advanced.allow_permanent_delete = *v;
            }
        }
    }
    if (doc.contains("access")) {
        const Json& a = doc["access"];
        if (!a.is_object()) {
            bad("access", "expected an object");
        } else {
            for (const auto& [key, value] : a.get<Json::object_t>()) {
                const std::string* v = value.is_string() ? &value.get<std::string>() : nullptr;
                if (key == "read") {
                    if (v != nullptr && (*v == "auto" || *v == "ask")) {
                        c.access.read = *v == "ask" ? ReadLevel::Ask : ReadLevel::Auto;
                    } else {
                        bad("access.read", "\"auto\" or \"ask\"");
                    }
                } else if (key == "change") {
                    if (v != nullptr && (*v == "folder" || *v == "always")) {
                        c.access.change = *v == "always" ? ChangeLevel::Always : ChangeLevel::Folder;
                    } else {
                        bad("access.change", "\"folder\" or \"always\"");
                    }
                } else {
                    bad("access." + key, "not a level: deletes and the system are always asked");
                }
            }
        }
    }
    if (doc.contains("network")) {
        const Json& w = doc["network"];
        if (!w.is_object()) {
            bad("network", "expected an object");
        } else if (w.contains("proxy")) {
            const Json& p = w["proxy"];
            const std::string* v = p.is_string() ? &p.get<std::string>() : nullptr;
            if (v != nullptr && (*v == "system" || *v == "none" || v->starts_with("http://"))) {
                c.network.proxy = *v;
            } else {
                bad("network.proxy", "\"system\", \"none\" or \"http://host:port\"");
            }
        }
    }
    if (doc.contains("ai")) {
        const Json& a = doc["ai"];
        if (!a.is_object()) {
            bad("ai", "expected an object");
        } else {
            if (a.contains("model")) {
                const Json& m = a["model"];
                if (m.is_string() && ai::catalog::model(m.get_string()) != nullptr) {
                    c.ai.model = m.get_string();
                } else {
                    std::string known; // the catalog's ids, as the file would spell them
                    for (const auto& k : ai::catalog::models()) {
                        known += (known.empty() ? "\"" : " or \"") + std::string{k.id} + "\"";
                    }
                    bad("ai.model", known);
                }
            }
            if (a.contains("thinking")) {
                const Json& t = a["thinking"];
                const std::string* v = t.is_string() ? &t.get_string() : nullptr;
                if (v != nullptr && (*v == "off" || *v == "low" || *v == "high" || *v == "max")) {
                    c.ai.thinking = *v;
                } else {
                    bad("ai.thinking", "\"off\", \"low\", \"high\" or \"max\"");
                }
            }
            if (a.contains("budget_usd")) {
                const Json& b = a["budget_usd"];
                if (b.is_number() && b.get_number() > 0 && b.get_number() <= 100) {
                    c.ai.budget_usd = b.get_number();
                } else {
                    bad("ai.budget_usd", "a number of dollars above 0, up to 100");
                }
            }
            if (a.contains("roles")) {
                read_roles(a["roles"], c.ai.roles, bad);
            }
            if (a.contains("disabled")) {
                const Json& d = a["disabled"];
                const auto known = [](const Json& v) { return v.is_string() && ai::catalog::model(v.get_string()) != nullptr; };
                if (d.is_array() && std::ranges::all_of(d.get_array(), known)) {
                    for (const auto& id : d.get_array()) {
                        c.ai.disabled.push_back(id.get_string());
                    }
                } else {
                    bad("ai.disabled", "a list of models' ids from the catalog");
                }
            }
        }
    }
    if (doc.contains("limits")) {
        const Json& l = doc["limits"];
        if (!l.is_object()) {
            bad("limits", "expected an object");
        } else {
            const auto count = [&](std::string_view key, std::uint32_t& into) {
                if (const auto v = number(l, key)) {
                    if (*v >= 1 && *v <= 1.0e6 && *v == std::floor(*v)) {
                        into = static_cast<std::uint32_t>(*v);
                    } else {
                        bad(key, "a whole number from 1");
                    }
                }
            };
            count("max_files", c.limits.max_files);
            count("max_megabytes", c.limits.max_megabytes);
            count("max_trash_batch", c.limits.max_trash_batch);
            count("max_tree", c.limits.max_tree);
            count("max_calls", c.limits.max_calls);
            count("think_seconds", c.limits.think_seconds);
        }
    }
    return out;
}

auto serialize(const Config& c) -> std::string {
    // The document as read (unknown keys and their order kept), the known values set over it.
    Json doc = Json::object_t{};
    if (!c.document.empty()) {
        Json read;
        if (!glz::read<glz::opts{.comments = true}>(read, c.document) && read.is_object()) {
            doc = std::move(read);
        }
    }
    // A setting the agent no longer has goes at the next save; read, it is an unknown key (quiet).
    doc.get_object().erase("desktop_animation"); // the desktop sorting's animation (removed)
    doc["hotkey"] = c.hotkey;
    doc["gpu"] = std::string{name_of(kGpus, c.gpu)};
    doc["over_fullscreen"] = c.over_fullscreen;
    doc["backdrop"] = std::string{name_of(kBackdrops, c.backdrop)};
    doc["text_scale"] = std::round(static_cast<double>(c.text_scale) * 1.0e4) / 1.0e4;
    doc["transparency"] = c.transparency;
    doc["rest_animation"] = c.rest_animation;
    switch (c.frame_rate) {
    case FrameRate::Monitor: doc["frame_rate"] = std::string{"monitor"}; break;
    case FrameRate::Hz120: doc["frame_rate"] = 120; break;
    case FrameRate::Hz60: doc["frame_rate"] = 60; break;
    case FrameRate::Hz30: doc["frame_rate"] = 30; break;
    }
    doc["theme"] = std::string{theme::word_of(c.theme)};
    Json& a = doc["advanced"];
    if (!a.is_object()) {
        a = Json::object_t{};
    }
    a["allow_system_writes"] = c.advanced.allow_system_writes;
    a["allow_secrets_read"] = c.advanced.allow_secrets_read;
    a["allow_permanent_delete"] = c.advanced.allow_permanent_delete;
    Json& access = doc["access"];
    if (!access.is_object()) {
        access = Json::object_t{};
    }
    access["read"] = std::string{c.access.read == ReadLevel::Ask ? "ask" : "auto"};
    access["change"] = std::string{c.access.change == ChangeLevel::Always ? "always" : "folder"};
    Json& w = doc["network"];
    if (!w.is_object()) {
        w = Json::object_t{};
    }
    w["proxy"] = c.network.proxy;
    Json& ai = doc["ai"];
    if (!ai.is_object()) {
        ai = Json::object_t{};
    }
    ai["model"] = c.ai.model;
    ai["thinking"] = c.ai.thinking;
    ai["budget_usd"] = c.ai.budget_usd;
    if (!c.ai.disabled.empty() || ai.contains("disabled")) {
        Json::array_t off;
        for (const auto& id : c.ai.disabled) {
            off.emplace_back(id);
        }
        ai["disabled"] = std::move(off);
    }
    if (c.ai.roles != ai::routing::default_roles() || ai.contains("roles")) { // the defaults stay out of a new file
        Json& roles = ai["roles"];
        roles = Json::object_t{};
        write_role(roles["chat"], c.ai.roles.chat, false);
        write_role(roles["hard"], c.ai.roles.hard, true);
        write_role(roles["images"], c.ai.roles.images, true);
    }
    Json& l = doc["limits"];
    if (!l.is_object()) {
        l = Json::object_t{};
    }
    l["max_files"] = static_cast<double>(c.limits.max_files);
    l["max_megabytes"] = static_cast<double>(c.limits.max_megabytes);
    l["max_trash_batch"] = static_cast<double>(c.limits.max_trash_batch);
    l["max_tree"] = static_cast<double>(c.limits.max_tree);
    l["max_calls"] = static_cast<double>(c.limits.max_calls);
    l["think_seconds"] = static_cast<double>(c.limits.think_seconds);
    auto out = core::json::write_pretty(doc);
    return out ? std::move(*out) + "\n" : std::string{"{}\n"};
}

auto load(const std::filesystem::path& file) -> core::Result<Parsed> {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        return Parsed{};
    }
    auto text = core::fs::read_file(file, 1u << 20);
    if (!text) {
        return std::unexpected{text.error()};
    }
    return parse(*text);
}

auto save(const std::filesystem::path& file, const Config& config) -> core::Result<void> {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    return core::fs::write_file_atomic(file, serialize(config));
}

auto migrate(const std::filesystem::path& dir) -> core::Result<bool> {
    const auto ini = dir / "agent.ini";
    const auto json = dir / "config.json";
    std::error_code ec;
    if (std::filesystem::exists(json, ec) || !std::filesystem::exists(ini, ec)) {
        return false;
    }
    auto text = core::fs::read_file(ini, 1u << 20);
    if (!text) {
        return std::unexpected{text.error()};
    }
    if (auto saved = save(json, parse_ini(*text).config); !saved) {
        return std::unexpected{saved.error()};
    }
    const auto bak = dir / "agent.ini.bak";
    if (!MoveFileExW(ini.c_str(), bak.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        return core::os_error(GetLastError(), "agent.ini -> agent.ini.bak");
    }
    return true;
}

} // namespace app::config
