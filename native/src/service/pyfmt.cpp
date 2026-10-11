#include "service/pyfmt.hpp"

#include <array>
#include <charconv>
#include <cmath>

namespace genko::service {

std::string fixed(double x, int places) {
    if (std::isnan(x)) return "nan";
    if (std::isinf(x)) return x > 0 ? "inf" : "-inf";
    std::array<char, 400> buffer{};
    const auto done = std::to_chars(buffer.data(), buffer.data() + buffer.size(), x, std::chars_format::fixed, places);
    return std::string(buffer.data(), done.ptr);
}

std::string percent(double x, int places) { return fixed(x * 100.0, places) + "%"; }

}  // namespace genko::service
