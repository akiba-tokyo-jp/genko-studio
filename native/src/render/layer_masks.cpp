#include "render/layer_masks.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "core/channels.hpp"

// (the lending of a mask to a pixel op: render/mask_lending.cpp)

namespace genko::render::layer_masks {

namespace {

unsigned char byte_of(double v) { return static_cast<unsigned char>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); }

// A Gaussian of `sigma` on one plane as three box passes along the rows, then three along the columns (Kovesi's radii,
// the edges repeated, the sums in double): as render/image_ops' gauss_blur takes it, for one plane.
void gauss_blur(std::vector<float>& v, std::size_t w, std::size_t h, double sigma) {
    const double n = 3.0;
    const double ideal = std::sqrt(12.0 * sigma * sigma / n + 1.0);
    int wl = static_cast<int>(std::floor(ideal));
    if (wl % 2 == 0) wl -= 1;
    wl = std::max(wl, 1);
    const int wu = wl + 2;
    const double m_value = (12.0 * sigma * sigma - n * wl * wl - 4.0 * n * wl - 3.0 * n) / (-4.0 * wl - 4.0);
    const auto m = static_cast<int>(std::clamp(std::round(m_value), 0.0, n));
    std::array<std::size_t, 3> radii{};
    for (int i = 0; i < 3; ++i) radii[std::size_t(i)] = static_cast<std::size_t>(i < m ? (wl - 1) / 2 : (wu - 1) / 2);
    std::vector<float> line(std::max(w, h));
    const auto pass = [](float* base, std::size_t count, std::size_t stride, std::size_t r, std::vector<float>& tmp) {
        if (r == 0 || count == 0) return;
        for (std::size_t i = 0; i < count; ++i) tmp[i] = base[i * stride];
        const double win = static_cast<double>(2 * r + 1);
        const auto at = [&](long long i) { return static_cast<double>(tmp[static_cast<std::size_t>(std::clamp<long long>(i, 0, static_cast<long long>(count) - 1))]); };
        double sum = 0.0;
        for (long long k = -static_cast<long long>(r); k <= static_cast<long long>(r); ++k) sum += at(k);
        for (std::size_t i = 0; i < count; ++i) {
            base[i * stride] = static_cast<float>(sum / win);
            sum += at(static_cast<long long>(i + r + 1)) - at(static_cast<long long>(i) - static_cast<long long>(r));
        }
    };
    for (const std::size_t r : radii)
        for (std::size_t y = 0; y < h; ++y) pass(v.data() + y * w, w, 1, r, line);
    for (const std::size_t r : radii)
        for (std::size_t x = 0; x < w; ++x) pass(v.data() + x, h, w, r, line);
}

}  // namespace

Image shown(const core::Page& page, const core::Mask& mask, const Image& decoded, Size size, const Box& area, Resample resample) {
    const double density = std::clamp(mask.density, 0.0, 1.0);
    const double feather = std::max(mask.feather_mm, 0.0);
    if (density >= 1.0 && feather <= 0.0) return decoded.resize_region(size, area, resample);
    Image out;
    const double width_mm = page.spec.width_mm.value();
    const double sigma = feather / 2.0 * (width_mm > 0 ? size.width / width_mm : 0.0);
    if (sigma >= 0.3) {
        // (feathered over the area widened by 3σ, so the blur at the area's edge sees what lies past it)
        const int reach = static_cast<int>(std::ceil(3.0 * sigma));
        const Box wide{std::max(0, area.x0 - reach), std::max(0, area.y0 - reach), std::min(size.width, area.x1 + reach),
                       std::min(size.height, area.y1 + reach)};
        const Image part = decoded.resize_region(size, wide, resample);
        const std::string bytes = part.tobytes();
        std::vector<float> values(bytes.size());
        for (std::size_t i = 0; i < bytes.size(); ++i) values[i] = static_cast<unsigned char>(bytes[i]) / 255.0f;
        gauss_blur(values, static_cast<std::size_t>(wide.width()), static_cast<std::size_t>(wide.height()), sigma);
        std::string cut(static_cast<std::size_t>(area.width()) * static_cast<std::size_t>(area.height()), '\0');
        for (int y = 0; y < area.height(); ++y) {
            for (int x = 0; x < area.width(); ++x) {
                const std::size_t from = static_cast<std::size_t>(y + area.y0 - wide.y0) * static_cast<std::size_t>(wide.width()) +
                                         static_cast<std::size_t>(x + area.x0 - wide.x0);
                cut[static_cast<std::size_t>(y) * static_cast<std::size_t>(area.width()) + static_cast<std::size_t>(x)] = static_cast<char>(byte_of(values[from]));
            }
        }
        out = Image::frombytes("L", Size{area.width(), area.height()}, cut);
    } else {
        out = decoded.resize_region(size, area, resample);
    }
    if (density < 1.0) out = out.point([density](int v) { return static_cast<int>(std::lround(255.0 - density * (255.0 - v))); });
    return out;
}

bool lends_to(std::string_view op) { return core::channel_target_op(op); }

}  // namespace genko::render::layer_masks
