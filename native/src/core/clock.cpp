#include "core/clock.hpp"

#include <array>
#include <ctime>
#include <optional>
#include <utility>

namespace genko::core {

namespace {

std::optional<std::string>& fixed_text() {
    static std::optional<std::string> text;
    return text;
}

}  // namespace

std::string local_time_text() {
    if (const auto& fixed = fixed_text()) return *fixed;
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::array<char, 32> out{};
    const std::size_t n = std::strftime(out.data(), out.size(), "%Y-%m-%dT%H:%M:%S", &local);
    return std::string(out.data(), n);
}

ScopedClock::ScopedClock(std::string text) {
    auto& fixed = fixed_text();
    if (fixed) {
        previous_ = *fixed;
        had_previous_ = true;
    }
    fixed = std::move(text);
}

ScopedClock::~ScopedClock() {
    auto& fixed = fixed_text();
    if (had_previous_) {
        fixed = previous_;
    } else {
        fixed.reset();
    }
}

}  // namespace genko::core
