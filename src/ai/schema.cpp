#include "ai/schema.hpp"

#include "core/json.hpp"

#include <glaze/glaze.hpp>

#include <algorithm>
#include <format>

namespace ai {

namespace {

using Json = glz::generic;

// What strict mode takes (api-docs.deepseek.com, guides/tool_calls, 2026-10-12).
constexpr std::string_view kKeywords[] = {"type",    "properties", "required", "additionalProperties", "items",
                                          "enum",    "anyOf",      "$ref",     "$defs",                "description",
                                          "pattern", "format",     "minimum",  "maximum",              "exclusiveMinimum",
                                          "exclusiveMaximum", "multipleOf", "const", "default"};
// What it does not, and what we drop without a word: limits (their sense is in the description)
// and glaze's titles.
constexpr std::string_view kDropped[] = {"minLength", "maxLength", "minItems", "maxItems", "title"};
constexpr std::string_view kTypes[] = {"object", "string", "number", "integer", "boolean", "array", "null"};

// An empty object. Not Json{Json::object_t{}}: braces pick generic's initializer_list constructor
// and make an array holding the object.
auto object() -> Json {
    Json j;
    j.data = Json::object_t{};
    return j;
}

auto in(std::span<const std::string_view> list, std::string_view s) -> bool {
    return std::ranges::find(list, s) != list.end();
}

auto at(const std::string& path, std::string_view step) -> std::string {
    return path.empty() ? std::string{step} : path + "." + std::string{step};
}

auto failed(const std::string& path, std::string_view what) -> core::Result<void> {
    return core::make_error(core::Code::Parse, std::format("{}: {}", path.empty() ? std::string{"the schema"} : path, what));
}

// The root's $defs: each "$ref": "#/$defs/x" is put in place of the reference, because the live
// API wants a "type" in every anyOf branch and glaze's optional numbers are {"$ref"} branches
// (probe, 2026-10-13: "field `anyOf`: missing field `type`").
using Defs = Json::object_t;

auto make_strict(Json& node, const std::string& path, const Defs& defs, int depth) -> core::Result<void>;

// Every schema under `node`'s properties, items and anyOf.
auto children(Json& node, const std::string& path, const Defs& defs, int depth) -> core::Result<void> {
    auto& o = node.get_object();
    if (const auto p = o.find("properties"); p != o.end() && p->second.is_object()) {
        for (auto& [name, child] : p->second.get_object()) {
            if (auto r = make_strict(child, at(path, "properties." + name), defs, depth); !r) {
                return r;
            }
        }
    }
    if (const auto i = o.find("items"); i != o.end()) {
        if (auto r = make_strict(i->second, at(path, "items"), defs, depth); !r) {
            return r;
        }
    }
    if (const auto a = o.find("anyOf"); a != o.end() && a->second.is_array()) {
        auto& list = a->second.get_array();
        for (std::size_t k = 0; k < list.size(); ++k) {
            if (auto r = make_strict(list[k], at(path, std::format("anyOf[{}]", k)), defs, depth); !r) {
                return r;
            }
        }
    }
    return {};
}

auto make_strict(Json& node, const std::string& path, const Defs& defs, int depth) -> core::Result<void> {
    if (!node.is_object()) {
        return failed(path, "not a schema");
    }
    if (const auto r = node.get_object().find("$ref"); r != node.get_object().end()) {
        constexpr std::string_view prefix = "#/$defs/";
        const std::string target = r->second.is_string() ? r->second.get_string() : std::string{};
        const auto def = target.starts_with(prefix) ? defs.find(std::string_view{target}.substr(prefix.size())) : defs.end();
        if (def == defs.end()) {
            return failed(path, std::format("a $ref to nothing ({})", target));
        }
        if (depth > 16) {
            return failed(path, "references nested too deep (a definition that refers to itself?)");
        }
        Json in_place = def->second;
        if (const auto d = node.get_object().find("description"); d != node.get_object().end() && in_place.is_object()) {
            in_place["description"] = d->second;
        }
        node = std::move(in_place);
        return make_strict(node, path, defs, depth + 1);
    }
    auto& o = node.get_object();
    for (const auto key : kDropped) {
        if (const auto k = o.find(key); k != o.end()) {
            o.erase(k);
        }
    }
    for (const auto& [key, value] : o) {
        if (!in(kKeywords, key)) {
            return failed(path, std::format("\"{}\" is not taken by strict mode", key));
        }
    }
    if (const auto t = o.find("type"); t != o.end() && t->second.is_array()) {
        // [X, "null"]: anyOf [{X and its keywords}, {null}]; the description stays outside.
        Json base = node;
        base.get_object().erase("type");
        base.get_object().erase("description");
        Json::array_t any;
        for (const auto& type : t->second.get_array()) {
            if (!type.is_string()) {
                return failed(path, "a type that is not a name");
            }
            Json branch = type.get_string() == "null" ? object() : base;
            branch["type"] = type.get_string();
            any.push_back(std::move(branch));
        }
        Json made = object();
        if (const auto d = o.find("description"); d != o.end()) {
            made["description"] = d->second;
        }
        made["anyOf"] = std::move(any);
        node = std::move(made);
        return children(node, path, defs, depth);
    }
    if (const auto t = o.find("type"); t != o.end() && !(t->second.is_string() && in(kTypes, t->second.get_string()))) {
        return failed(path, std::format("type {} is not taken by strict mode", core::json::write(t->second).value_or("?")));
    }
    if (const auto t = o.find("type"); t != o.end() && t->second.get_string() == "object") {
        Json::array_t required;
        if (const auto p = o.find("properties"); p != o.end() && p->second.is_object()) {
            for (const auto& [name, child] : p->second.get_object()) {
                required.emplace_back(name);
            }
        }
        node["required"] = std::move(required);
        node["additionalProperties"] = false;
    }
    return children(node, path, defs, depth);
}

void check(const Json& node, const std::string& path, std::vector<std::string>& out) {
    const auto where = path.empty() ? std::string{"the schema"} : path;
    if (!node.is_object()) {
        out.push_back(where + ": not a schema");
        return;
    }
    const auto& o = node.get_object();
    for (const auto& [key, value] : o) {
        if (!in(kKeywords, key)) {
            out.push_back(std::format("{}: \"{}\"", where, key));
        }
    }
    if (o.contains("$ref")) {
        out.push_back(where + ": a $ref (the live API wants the type in place)");
    }
    if (const auto t = o.find("type"); t != o.end()) {
        if (t->second.is_array()) {
            out.push_back(where + ": a list of types");
        } else if (!t->second.is_string() || !in(kTypes, t->second.get_string())) {
            out.push_back(where + ": an unknown type");
        } else if (t->second.get_string() == "object") {
            const auto a = o.find("additionalProperties");
            if (a == o.end() || !a->second.is_boolean() || a->second.get_boolean()) {
                out.push_back(where + ": additionalProperties is not false");
            }
            const auto r = o.find("required");
            if (const auto p = o.find("properties"); p != o.end() && p->second.is_object()) {
                for (const auto& [name, child] : p->second.get_object()) {
                    const bool listed = r != o.end() && r->second.is_array() &&
                                        std::ranges::any_of(r->second.get_array(), [&](const Json& n) {
                                            return n.is_string() && n.get_string() == name;
                                        });
                    if (!listed) {
                        out.push_back(std::format("{}: \"{}\" is not required", where, name));
                    }
                }
            }
        }
    }
    if (const auto p = o.find("properties"); p != o.end() && p->second.is_object()) {
        for (const auto& [name, child] : p->second.get_object()) {
            check(child, at(path, "properties." + name), out);
        }
    }
    if (const auto i = o.find("items"); i != o.end()) {
        check(i->second, at(path, "items"), out);
    }
    if (const auto a = o.find("anyOf"); a != o.end() && a->second.is_array()) {
        const auto& list = a->second.get_array();
        for (std::size_t k = 0; k < list.size(); ++k) {
            check(list[k], at(path, std::format("anyOf[{}]", k)), out);
        }
    }
    if (const auto d = o.find("$defs"); d != o.end() && d->second.is_object()) {
        for (const auto& [name, child] : d->second.get_object()) {
            check(child, at(path, "$defs." + name), out);
        }
    }
}

} // namespace

auto strict_schema(std::string_view schema) -> core::Result<std::string> {
    Json j;
    if (const auto error = glz::read_json(j, schema)) {
        return core::make_error(core::Code::Parse, "a tool's schema: " + glz::format_error(error, schema));
    }
    Defs defs;
    if (j.is_object()) {
        if (const auto d = j.get_object().find("$defs"); d != j.get_object().end()) {
            if (d->second.is_object()) {
                defs = d->second.get_object();
            }
            j.get_object().erase(d); // every reference is put in place below
        }
    }
    if (auto made = make_strict(j, {}, defs, 0); !made) {
        return std::unexpected{made.error()};
    }
    auto text = core::json::write(j);
    if (!text) {
        return core::make_error(core::Code::Internal, "a tool's schema could not be written");
    }
    return std::move(*text);
}

auto strict_violations(std::string_view schema) -> std::vector<std::string> {
    Json j;
    if (glz::read_json(j, schema)) {
        return {"the schema: not JSON"};
    }
    std::vector<std::string> out;
    check(j, {}, out);
    return out;
}

} // namespace ai
