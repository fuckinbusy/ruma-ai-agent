#include "ai/routing.hpp"

#include <algorithm>

namespace ai::routing {

auto default_roles() -> Roles {
    Roles r;
    std::vector<std::string> all;
    for (const auto& m : catalog::models()) {
        all.emplace_back(m.id);
    }
    r.chat.fallbacks = all;
    r.hard.fallbacks = all;
    const auto sees = std::ranges::find(catalog::models(), catalog::Tri::Yes, &catalog::Model::vision);
    if (sees != catalog::models().end()) {
        r.images.model = std::string{sees->id};
    }
    return r;
}

auto need_of(bool picture, Thinking level) -> Need {
    if (picture) {
        return Need::Images;
    }
    return level == Thinking::High || level == Thinking::Max ? Need::Hard : Need::Chat;
}

auto chain(const Roles& roles, Need need, const catalog::Model& chosen) -> std::vector<const catalog::Model*> {
    const Role& role = need == Need::Images ? roles.images : need == Need::Hard ? roles.hard : roles.chat;
    std::vector<const catalog::Model*> out;
    const auto add = [&out](const catalog::Model* m) {
        if (m != nullptr && std::ranges::find(out, m) == out.end()) {
            out.push_back(m);
        }
    };
    const catalog::Model* first = need == Need::Chat || role.model.empty() ? nullptr : catalog::model(role.model);
    add(first != nullptr ? first : &chosen);
    for (const auto& id : role.fallbacks) {
        add(catalog::model(id));
    }
    return out;
}

auto chains(const Roles& roles, const catalog::Model& chosen) -> Chains {
    return {.chat = chain(roles, Need::Chat, chosen), .hard = chain(roles, Need::Hard, chosen), .images = chain(roles, Need::Images, chosen)};
}

auto why_of(const core::Error& e) -> std::optional<Why> {
    if (e.kind == "busy") {
        return Why::Busy;
    }
    if (e.kind == "key_rejected") {
        return Why::KeyRejected;
    }
    if (e.kind == "no_key") {
        return Why::NoKey;
    }
    if (!e.kind.empty()) {
        return std::nullopt; // balance_empty and any other named refusal: the request ends here
    }
    if (e.code == core::Code::Timeout) {
        return Why::NoAnswer;
    }
    if (e.code == core::Code::Net) {
        return Why::Offline;
    }
    return std::nullopt;
}

auto provider_wide(Why why) -> bool {
    return why == Why::NoKey || why == Why::KeyRejected || why == Why::Offline;
}

} // namespace ai::routing
