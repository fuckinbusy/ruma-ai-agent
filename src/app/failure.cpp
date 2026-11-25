#include "app/failure.hpp"

#include <format>
#include <string>

namespace app::failure {

auto card(const core::Error& e, const ai::catalog::Model& last) -> Shown {
    const auto& p = ai::catalog::provider_of(last);
    Shown s;
    s.card.detail = std::string{last.full_name};
    const events::Action retry{"retry", "Retry", true};
    if (e.kind == "no_key") {
        s.card = {.kind = events::CardKind::NoKey,
                  .tag = "No key",
                  .title = std::format("No {} key yet", p.name),
                  .detail = std::move(s.card.detail),
                  .body = std::format("Your message wasn't sent. Add a key to use {}'s models.", p.name)};
        s.actions = {{"set_key", "Add key", true}};
    } else if (e.kind == "key_rejected") {
        s.card = {.kind = events::CardKind::KeyRejected,
                  .tag = "Key rejected",
                  .title = std::format("{} didn't accept the key", p.name),
                  .detail = std::move(s.card.detail),
                  .body = "The key is invalid or expired. The request wasn't accepted, so nothing was charged for it."};
        s.actions = {{"set_key", "Update key", true}, {"retry", "Retry", false}};
    } else if (e.kind == "busy") {
        s.card = {.kind = events::CardKind::Busy,
                  .tag = "Busy",
                  .title = std::format("{} is busy", p.name),
                  .detail = std::move(s.card.detail),
                  .body = "I retried a few times and it stayed busy. Try again in a minute."};
        s.actions = {retry};
    } else if (e.kind == "balance_empty") {
        s.card = {.kind = events::CardKind::Balance,
                  .tag = "Balance",
                  .title = std::format("{} balance is empty", p.name),
                  .detail = std::move(s.card.detail),
                  .body = std::format("Top up on {}'s site, then try again. I didn't send it to another model: that would "
                                      "spend money you didn't choose to.",
                                      p.name)};
        s.actions = {retry};
    } else if (e.kind.empty() && e.code == core::Code::Timeout) {
        s.card = {.kind = events::CardKind::NoAnswer,
                  .tag = "No answer",
                  .title = std::format("{} didn't answer", last.full_name),
                  .detail = std::move(s.card.detail),
                  .body = "The request timed out. Nothing came back, so nothing was charged."};
        s.actions = {retry};
    } else if (e.kind.empty() && e.code == core::Code::Net) {
        s.card = {.kind = events::CardKind::Offline,
                  .tag = "Offline",
                  .title = std::format("Can't reach {}", p.name),
                  .detail = std::move(s.card.detail),
                  .body = "Your message wasn't sent. Check the connection, then try again."};
        s.actions = {retry};
    } else {
        s.card = {.kind = events::CardKind::Error, .tag = "Error", .title = e.message, .detail = std::move(s.card.detail), .body = {}};
        s.actions = {retry};
    }
    return s;
}

} // namespace app::failure
