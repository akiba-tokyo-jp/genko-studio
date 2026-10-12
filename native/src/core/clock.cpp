#include "core/clock.hpp"

#include <array>
#include <cstdio>
#include <ctime>
#include <optional>
#include <utility>

namespace genko::core {

namespace {

std::optional<std::string>& fixed_text() {
    static std::optional<std::string> text;
    return text;
}

// The fixed clock's time in `format`: the text read as "%Y-%m-%dT%H:%M:%S" and written as asked (a job's id and an
// export's folder have no ":" then, which Windows takes in no name), or the text itself when it is no such time (as
// tools/migration/studio_harness.py's fixed_clock does).
std::string fixed_in(const std::string& text, const char* format) {
    std::tm at{};
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    char tail = '\0';
    if (std::sscanf(text.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d%c", &year, &month, &day, &hour, &minute, &second, &tail) != 6) return text;
    at.tm_year = year - 1900;
    at.tm_mon = month - 1;
    at.tm_mday = day;
    at.tm_hour = hour;
    at.tm_min = minute;
    at.tm_sec = second;
    at.tm_isdst = -1;
    std::tm normal = at;
    (void)std::mktime(&normal);  // (the day of the week and of the year, as strptime gives them)
    at.tm_wday = normal.tm_wday;
    at.tm_yday = normal.tm_yday;
    std::array<char, 128> out{};
    const std::size_t n = std::strftime(out.data(), out.size(), format, &at);
    return std::string(out.data(), n);
}

}  // namespace

std::string local_time_text() { return local_time_text("%Y-%m-%dT%H:%M:%S"); }

std::string local_time_text(const char* format) {
    if (const auto& fixed = fixed_text()) return fixed_in(*fixed, format);
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
    if (const auto& fixed = fixed_text()) return fixed_in(*fixed, format);
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
