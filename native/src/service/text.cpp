#include "service/text.hpp"

#include <cmath>

namespace genko::service::text {

namespace {

bool continuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

}  // namespace

std::size_t length(std::string_view utf8) {
    std::size_t n = 0;
    for (const char c : utf8) n += continuation(c) ? 0 : 1;
    return n;
}

std::string head(std::string_view utf8, std::size_t n) {
    std::size_t seen = 0;
    for (std::size_t i = 0; i < utf8.size(); ++i) {
        if (!continuation(utf8[i])) {
            if (seen == n) return std::string(utf8.substr(0, i));
            ++seen;
        }
    }
    return std::string(utf8);
}

}  // namespace genko::service::text

namespace genko::service {

double py_floordiv(double vx, double wx) {
    double mod = std::fmod(vx, wx);
    // fmod is exact, so vx - mod is mathematically a multiple of wx; the division may still land near an integer
    double div = (vx - mod) / wx;
    if (mod != 0.0) {
        if ((wx < 0) != (mod < 0)) div -= 1.0;  // (the remainder takes the sign of the denominator)
    }
    double floordiv = 0.0;
    if (div != 0.0) {
        floordiv = std::floor(div);
        if (div - floordiv > 0.5) floordiv += 1.0;
    } else {
        floordiv = std::copysign(0.0, vx / wx);
    }
    return floordiv;
}

}  // namespace genko::service
