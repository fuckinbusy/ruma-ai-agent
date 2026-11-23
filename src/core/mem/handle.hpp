#pragma once

// core::Handle<Traits> — RAII owner for any C or OS handle (HANDLE, HWND, HGLRC, GL names,
// CURL*, FT_Face, ...). Traits supplies:
//   using type = ...;                         // the raw handle type
//   static type invalid() noexcept;           // the "no resource" value
//   static void close(type) noexcept;         // releases a valid handle
// Move-only; closes exactly once.

#include <utility>

namespace core {

template <class Traits>
class Handle {
public:
    using value_type = typename Traits::type;

    Handle() noexcept = default;
    explicit Handle(value_type value) noexcept : value_(value) {}
    ~Handle() { reset(); }

    Handle(Handle&& other) noexcept : value_(other.release()) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    [[nodiscard]] auto get() const noexcept -> value_type { return value_; }
    explicit operator bool() const noexcept { return value_ != Traits::invalid(); }

    // Gives up ownership without closing.
    [[nodiscard]] auto release() noexcept -> value_type {
        return std::exchange(value_, Traits::invalid());
    }

    // Closes the current resource (if any) and takes `value`. Resetting to the value already
    // held is a no-op, so the resource is never closed while still owned.
    void reset(value_type value = Traits::invalid()) noexcept {
        if (value == value_) {
            return;
        }
        const value_type old = std::exchange(value_, value);
        if (old != Traits::invalid()) {
            Traits::close(old);
        }
    }

private:
    value_type value_ = Traits::invalid();
};

} // namespace core
