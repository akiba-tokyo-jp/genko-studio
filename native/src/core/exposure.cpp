// Exposure algorithm adapted from PhotoCraft; see third_party/photocraft/LICENSE-MIT.
#include "core/exposure.hpp"
#include <algorithm>
#include <cmath>
#include "core/error.hpp"
#include "core/pyconv.hpp"
namespace genko::core {
Exposure Exposure::parse(const Json& spec) {
    if (!spec.is_object()) throw PyValueError("exposure must be an object");
    for (const auto& [key, value] : spec.items()) {
        if (key != "kind" && key != "exposure" && key != "offset" && key != "gamma")
            throw PyValueError("unknown exposure parameter " + key);
    }
    const auto number = [&](const char* key, double fallback, double lo, double hi) {
        if (!spec.contains(key)) return fallback;
        const double v = py_float(spec[key]);
        if (!std::isfinite(v)) throw PyValueError(std::string(key)+" must be finite");
        return std::clamp(v, lo, hi);
    };
    return {number("exposure", 0, -20, 20), number("offset", 0, -.5, .5), number("gamma", 1, .01, 9.99)};
}
double Exposure::apply(double linear) const {
    // Clamping the base before pow is algebraically equivalent for positive gamma
    // and prevents overflow for a finite HDR sample and a very small gamma.
    return std::pow(std::clamp(linear*std::exp2(stops)+offset, 0.0, 1.0), 1/gamma);
}
double srgb_to_linear(double x) { return x <= .04045 ? x/12.92 : std::pow((x+.055)/1.055, 2.4); }
double linear_to_srgb(double x) { return x <= .0031308 ? x*12.92 : 1.055*std::pow(x, 1/2.4)-.055; }
} // namespace genko::core
