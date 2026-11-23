#pragma once

// ai::schema — a tool's JSON Schema as DeepSeek's strict mode takes it (05-AI-AGENT § Facts):
// every property of every object required (an optional one is sent as null), additionalProperties
// false, no minLength/maxLength/minItems/maxItems, types from object/string/number/integer/
// boolean/array/null, anyOf/$ref/$defs/enum allowed.

#include "core/error.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace ai {

// glaze's schema made strict: each object's properties all required and additionalProperties
// false; "type":[X,"null"] becomes anyOf [{X with its keywords}, {null}] (the description stays
// outside); the length and item-count limits and "title" are dropped. A keyword strict mode does
// not know, or a type it does not, is an error naming the path ("properties.x").
auto strict_schema(std::string_view schema) -> core::Result<std::string>;

// What keeps `schema` from being strict, one line each with its path; empty: it is strict.
auto strict_violations(std::string_view schema) -> std::vector<std::string>;

} // namespace ai
