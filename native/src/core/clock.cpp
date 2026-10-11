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

std::string local_time_text() { return local_time_text("%Y-%m-%dT%H:%M:%S"); }

std::string local_time_text(const char* format) {
    if (const auto& fixed = fixed_text()) return *fixed;
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::array<char, 128> out{};
    const std::size_t n = std::strftime(out.data(), out.size(), format, &local);
    return std::string(out.data(), n);
}

std::string utc_time_text(const char* format) {
    if (const auto& fixed = fixed_text()) return *fixed;
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    std::array<char, 128> out{};
    const std::size_t n = std::strftime(out.data(), out.size(), format, &utc);
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
