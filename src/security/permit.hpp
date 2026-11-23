#pragma once

// security::Permit — the only key to a changing operation. guarded_fs/guarded_proc take one and
// act only on what it names. Nobody outside security can make one: its constructor is private,
// grant() is its only friend, and grant() gives none for a refusal and none for a question
// without the user's click (03-SECURITY: "the policy decides, not the model").

#include "security/policy.hpp"

#include <optional>
#include <span>
#include <string_view>

namespace security {

class Permit;

// `ruling` must be decide()'s ruling for this very `request`. HardDeny and Deny: never; Ask: only
// with `user_confirmed` (a click in the UI thread); Auto: always. A Permit is counted in `session`.
auto grant(const Ruling& ruling, const Request& request, bool user_confirmed, Session& session) -> std::optional<Permit>;

class Permit {
public:
    Permit(Permit&&) noexcept = default;
    auto operator=(Permit&&) noexcept -> Permit& = default;
    Permit(const Permit&) = delete;
    auto operator=(const Permit&) -> Permit& = delete;

    [[nodiscard]] auto tool() const noexcept -> std::string_view { return request_.tool; }
    [[nodiscard]] auto risk() const noexcept -> Risk { return request_.risk; }
    [[nodiscard]] auto request() const noexcept -> const Request& { return request_; }
    // The canonical paths it is valid for.
    [[nodiscard]] auto paths() const noexcept -> std::span<const paths::Canonical> { return paths_; }
    // The zone the policy saw for each path: what is opened under it may be no stricter.
    [[nodiscard]] auto zones() const noexcept -> std::span<const paths::Zone> { return zones_; }
    [[nodiscard]] auto decided_by_user() const noexcept -> bool { return by_user_; }

private:
    Permit(Request request, std::vector<paths::Canonical> paths, std::vector<paths::Zone> zones, bool by_user)
        : request_(std::move(request)), paths_(std::move(paths)), zones_(std::move(zones)), by_user_(by_user) {}
    friend auto grant(const Ruling&, const Request&, bool, Session&) -> std::optional<Permit>;

    Request request_;
    std::vector<paths::Canonical> paths_;
    std::vector<paths::Zone> zones_;
    bool by_user_ = false;
};

} // namespace security
