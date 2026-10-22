#include "render/dabs/tile.hpp"

#include <algorithm>
#include <cmath>

#include "core/base64.hpp"
#include "core/error.hpp"
#include "render/image.hpp"
#include "render/png.hpp"

// PhotoCraft v0.2.0 crates/paint/src/tile.rs, MIT: native/third_party/photocraft/NOTICE.md.

namespace genko::render::dabs {

std::uint16_t q16(float v) { return static_cast<std::uint16_t>(std::clamp(v, 0.0f, 1.0f) * 65535.0f + 0.5f); }

GrayTile GrayTile::from_fn(std::uint32_t width, std::uint32_t height, const std::function<float(std::uint32_t, std::uint32_t)>& f) {
    GrayTile t{width, height, {}};
    t.data.reserve(std::size_t(width) * height);
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x) t.data.push_back(q16(f(x, y)));
    return t;
}

std::vector<float> GrayTile::to_f32() const {
    std::vector<float> out(data.size());
    for (std::size_t i = 0; i < data.size(); ++i) out[i] = static_cast<float>(data[i]) / 65535.0f;
    return out;
}

GrayTile tile_of_png(std::string_view png) {
    Image image;
    try {
        image = read_png(png, PngLimits{4096LL * 4096LL, 4096});
    } catch (const core::Error&) {
        throw core::Error("value", "the picture is not a PNG this build reads");
    }
    const auto w = static_cast<std::uint32_t>(image.width()), h = static_cast<std::uint32_t>(image.height());
    GrayTile t{w, h, {}};
    t.data.resize(std::size_t(w) * h);
    const std::string raw = image.tobytes();
    if (image.mode() == "L") {
        for (std::size_t i = 0; i < t.data.size(); ++i) t.data[i] = static_cast<std::uint16_t>(static_cast<unsigned char>(raw[i]) * 257);
    } else if (image.mode() == "I;16") {
        for (std::size_t i = 0; i < t.data.size(); ++i) {
            t.data[i] = static_cast<std::uint16_t>(static_cast<unsigned char>(raw[2 * i]) | (static_cast<unsigned char>(raw[2 * i + 1]) << 8));
        }
    } else {
        throw core::Error("value", "the picture is not a grey PNG of 8 or 16 bits");
    }
    if (!t.valid()) throw core::Error("value", "the picture has no pixels");
    return t;
}

std::string png_of_tile(const GrayTile& tile) {
    const bool eight = std::all_of(tile.data.begin(), tile.data.end(), [](std::uint16_t v) { return v % 257 == 0; });
    const Size size{static_cast<int>(tile.width), static_cast<int>(tile.height)};
    std::string raw;
    if (eight) {
        raw.resize(tile.data.size());
        for (std::size_t i = 0; i < tile.data.size(); ++i) raw[i] = static_cast<char>(tile.data[i] / 257);
        return write_png(Image::frombytes("L", size, raw), 9);
    }
    raw.resize(tile.data.size() * 2);
    for (std::size_t i = 0; i < tile.data.size(); ++i) {
        raw[2 * i] = static_cast<char>(tile.data[i] & 0xFF);
        raw[2 * i + 1] = static_cast<char>(tile.data[i] >> 8);
    }
    return write_png(Image::frombytes("I;16", size, raw), 9);
}

GrayTile tile_of_json_form(std::uint32_t width, std::uint32_t height, const std::string* base64, const std::vector<double>* floats) {
    GrayTile t{width, height, {}};
    const std::size_t n = std::size_t(width) * height;
    if (base64 != nullptr) {
        std::string bytes;
        try {
            bytes = core::a2b_base64(*base64);
        } catch (const core::Error&) {
            throw core::Error("value", "the tile's data is not base64");
        }
        for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) {
            t.data.push_back(static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[i]) | (static_cast<unsigned char>(bytes[i + 1]) << 8)));
        }
    } else if (floats != nullptr) {
        for (const double v : *floats) t.data.push_back(q16(static_cast<float>(v)));
    }
    if (n == 0 || t.data.size() != n) {
        throw core::Error("value", "the tile's data has " + std::to_string(t.data.size()) + " samples, not " + std::to_string(width) + "×" +
                                       std::to_string(height));
    }
    return t;
}

Mips::Mips(const GrayTile& tile) {
    levels.push_back(Level{tile.width, tile.height, tile.to_f32()});
    while (true) {
        const Level& v = levels.back();
        if (v.width <= 1 && v.height <= 1) break;
        Level next{(v.width + 1) / 2, (v.height + 1) / 2, {}};
        next.values.assign(next.width * next.height, 0.0f);
        for (std::size_t y = 0; y < next.height; ++y) {
            for (std::size_t x = 0; x < next.width; ++x) {
                float s = 0.0f, c = 0.0f;
                for (std::size_t dy = 0; dy < 2; ++dy) {
                    for (std::size_t dx = 0; dx < 2; ++dx) {
                        const std::size_t sx = x * 2 + dx, sy = y * 2 + dy;
                        if (sx < v.width && sy < v.height) {
                            s += v.values[sy * v.width + sx];
                            c += 1.0f;
                        }
                    }
                }
                next.values[y * next.width + x] = s / c;
            }
        }
        levels.push_back(std::move(next));
    }
}

std::size_t Mips::level_for(float px) const {
    const Level& base = levels.front();
    float ratio = static_cast<float>(std::max(base.width, base.height)) / std::max(px, 1.0f);
    std::size_t level = 0;
    while (ratio >= 2.0f) {  // (floor(log2(ratio)): halving is exact)
        ratio /= 2.0f;
        ++level;
    }
    return std::min(level, levels.size() - 1);
}

float Mips::sample_clamped(std::size_t level, float u, float v) const {
    const Level& l = levels[level];
    const float fx = u * static_cast<float>(l.width) - 0.5f;
    const float fy = v * static_cast<float>(l.height) - 0.5f;
    const float x0 = std::floor(fx), y0 = std::floor(fy);
    const float tx = fx - x0, ty = fy - y0;
    const auto xi = static_cast<std::int64_t>(x0), yi = static_cast<std::int64_t>(y0);
    const auto get = [&](std::int64_t x, std::int64_t y) {
        if (x < 0 || y < 0 || x >= static_cast<std::int64_t>(l.width) || y >= static_cast<std::int64_t>(l.height)) return 0.0f;
        return l.values[std::size_t(y) * l.width + std::size_t(x)];
    };
    const float a = get(xi, yi) + (get(xi + 1, yi) - get(xi, yi)) * tx;
    const float b = get(xi, yi + 1) + (get(xi + 1, yi + 1) - get(xi, yi + 1)) * tx;
    return a + (b - a) * ty;
}

float PatternImage::sample_wrap(double x, double y) const {
    const auto w = static_cast<std::int64_t>(width), h = static_cast<std::int64_t>(height);
    std::int64_t xi = static_cast<std::int64_t>(std::floor(x)) % w;
    std::int64_t yi = static_cast<std::int64_t>(std::floor(y)) % h;
    if (xi < 0) xi += w;
    if (yi < 0) yi += h;
    return data[std::size_t(yi) * width + std::size_t(xi)];
}

}  // namespace genko::render::dabs
