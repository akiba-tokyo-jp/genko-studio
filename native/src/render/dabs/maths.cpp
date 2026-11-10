#include "render/dabs/maths.hpp"

#include <cmath>

#include "render/paper.hpp"

namespace genko::render::dabs {

namespace {

// atan(x) for |x| <= 1, in radians: halved twice (atan x = 2 atan(x / (1 + sqrt(1 + x²)))) to |x| <= tan(π/16), then
// its series to the 23rd power (the next term is below 1e-18).
double atan_unit(double x) {
    for (int i = 0; i < 2; ++i) x = x / (1.0 + std::sqrt(1.0 + x * x));
    const double x2 = x * x;
    double term = x, sum = 0.0;
    for (int k = 0; k < 12; ++k) {
        sum += (k % 2 == 0 ? term : -term) / static_cast<double>(2 * k + 1);
        term *= x2;
    }
    return 4.0 * sum;
}

constexpr double kPi = 3.141592653589793;

}  // namespace

std::pair<double, double> sin_cos(double degrees) { return paper::sin_cos_degrees(degrees); }

double atan2_degrees(double y, double x) {
    if (x == 0.0 && y == 0.0) return 0.0;
    if (x == 0.0) return y > 0 ? 90.0 : -90.0;  // (the axes exactly)
    if (y == 0.0) return x > 0 ? 0.0 : 180.0;
    const double ax = std::fabs(x), ay = std::fabs(y);
    // the angle of (ax, ay) in the first quadrant, from the smaller ratio
    double a = ay <= ax ? atan_unit(ay / ax) : kPi / 2 - atan_unit(ax / ay);
    if (x < 0) a = kPi - a;
    if (y < 0) a = -a;
    return a * (180.0 / kPi);
}

double rem_euclid(double x, double m) {
    const double r = std::fmod(x, m);
    return r < 0 ? r + m : r;
}

}  // namespace genko::render::dabs
