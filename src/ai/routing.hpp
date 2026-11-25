#pragma once

// ai::routing — which model a request goes to (UI update, stage 4c): a request takes a role by what
// it needs (a picture, a high level of thinking, or neither); the role names its model and the
// models that stand in for it. The loop walks that chain and moves on when a model cannot do what
// is needed or does not answer; the interface says so every time (never a quiet change).

#include "ai/catalog.hpp"
#include "ai/types.hpp"
#include "core/error.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ai::routing {

// The role a request takes: Images when it carries a picture, Hard at High or Max thinking, else Chat.
enum class Need : std::uint8_t { Chat, Hard, Images };

// A role's model ("": the one the user chose) and the models that stand in for it, in order.
struct Role {
    std::string model;
    std::vector<std::string> fallbacks;
    bool operator==(const Role&) const = default;
};

// Chat's model is always the one chosen in the model menu; its `model` stays "".
struct Roles {
    Role chat, hard, images;
    bool operator==(const Roles&) const = default;
};

// What a new user (or a config without roles) gets: Chat and Hard on the chosen model with every
// model of the catalog behind it; Images on the first model that sees pictures, with none behind.
[[nodiscard]] auto default_roles() -> Roles;

[[nodiscard]] auto need_of(bool picture, Thinking level) -> Need;

// The role's models in order: its model (or `chosen`), then its fallbacks; ids the catalog does not
// know, and repeats, are dropped.
[[nodiscard]] auto chain(const Roles& roles, Need need, const catalog::Model& chosen) -> std::vector<const catalog::Model*>;

// The three chains, as the loop takes them (an empty one: the chosen model alone).
struct Chains {
    std::vector<const catalog::Model*> chat, hard, images;
};
[[nodiscard]] auto chains(const Roles& roles, const catalog::Model& chosen) -> Chains;

// Why a request went to another model than the one chosen: its role names another (Role), the
// chosen cannot do what is needed (NoVision, NoThinking), or a model before it in the chain failed.
enum class Why : std::uint8_t { Role, NoVision, NoThinking, NoKey, KeyRejected, Busy, NoAnswer, Offline };

// A failure that moves the request to the next model of the chain; nullopt: it ends the request
// (an empty balance is the user's money, never spent elsewhere; a bad request fails everywhere).
[[nodiscard]] auto why_of(const core::Error& e) -> std::optional<Why>;

// The failure is the provider's, not one model's: its other models are not tried either.
[[nodiscard]] auto provider_wide(Why why) -> bool;

} // namespace ai::routing
