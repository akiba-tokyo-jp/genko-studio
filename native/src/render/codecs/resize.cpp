// A codecs' picture made another size (render/codecs/resize.hpp).

#include "render/codecs/resize.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <vector>

#include "core/error.hpp"

namespace genko::render::codecs {

namespace {

// One output pixel's taps along an axis: the first source index and the weights from it.
struct Taps {
    std::size_t first = 0;
    std::vector<float> weights;
};

double kernel(ResizeFilter filter, double x) {
    x = std::abs(x);
    if (filter == ResizeFilter::Bilinear) return x < 1.0 ? 1.0 - x : 0.0;
    // (Catmull-Rom: Keys' cubic with a = -0.5)
    constexpr double a = -0.5;
    if (x < 1.0) return ((a + 2.0) * x - (a + 3.0)) * x * x + 1.0;
    if (x < 2.0) return (((x - 5.0) * x + 8.0) * x - 4.0) * a;
    return 0.0;
}

std::vector<Taps> taps_for(std::uint32_t from, std::uint32_t to, ResizeFilter filter) {
    std::vector<Taps> out(to);
    const double factor = double(from) / double(to);
    if (filter == ResizeFilter::Nearest) {
        for (std::uint32_t i = 0; i < to; ++i) {
            out[i].first = std::min<std::size_t>(from - 1, static_cast<std::size_t>(std::floor((i + 0.5) * factor)));
            out[i].weights = {1.0f};
        }
        return out;
    }
    const double scale = std::max(1.0, factor);  // (a reduction widens the filter: no aliasing)
    const double support = (filter == ResizeFilter::Bilinear ? 1.0 : 2.0) * scale;
    for (std::uint32_t i = 0; i < to; ++i) {
        const double centre = (i + 0.5) * factor;
        const auto lo = static_cast<std::int64_t>(std::max(0.0, std::floor(centre - support)));
        const auto hi = static_cast<std::int64_t>(std::min<double>(from, std::ceil(centre + support)));
        std::vector<double> w;
        double sum = 0.0;
        for (std::int64_t k = lo; k < hi; ++k) {
            const double v = kernel(filter, (double(k) + 0.5 - centre) / scale);
            w.push_back(v);
            sum += v;
        }
        out[i].first = static_cast<std::size_t>(lo);
        if (w.empty() || sum == 0.0) {  // (cannot happen past a size of 0; kept safe)
            out[i].first = std::min<std::size_t>(from - 1, static_cast<std::size_t>(centre));
            out[i].weights = {1.0f};
            continue;
        }
        for (const double v : w) out[i].weights.push_back(static_cast<float>(v / sum));
    }
    return out;
}

}  // namespace

std::optional<ResizeFilter> resize_filter(std::string_view id) {
    if (id == "nearest") return ResizeFilter::Nearest;
    if (id == "bilinear") return ResizeFilter::Bilinear;
    if (id == "bicubic") return ResizeFilter::Bicubic;
    return std::nullopt;
}

CodecImage resized(const CodecImage& picture, std::uint32_t width, std::uint32_t height, ResizeFilter filter) {
    if (width == 0 || height == 0) throw core::Error("value", "a picture of no pixels");
    if (picture.empty()) throw core::Error("value", "a picture of no pixels");
    if (width == picture.width() && height == picture.height()) return picture;
    const unsigned channels = codecs::channels(picture.layout());
    const bool alpha = has_alpha(picture.layout());
    const bool hdr = is_float(picture.sample());
    const std::uint32_t sw = picture.width(), sh = picture.height();
    const std::vector<Taps> across = taps_for(sw, width, filter);
    const std::vector<Taps> down = taps_for(sh, height, filter);
    // (a source row filtered across, premultiplied: made once, kept while the output rows still need it)
    std::deque<std::pair<std::size_t, std::vector<float>>> window;
    std::vector<float> source(std::size_t(sw) * channels);
    const auto filtered = [&](std::size_t y) -> const std::vector<float>& {
        for (const auto& [row, values] : window) {
            if (row == y) return values;
        }
        for (std::uint32_t x = 0; x < sw; ++x) {
            const std::size_t at = (std::size_t(y) * sw + x) * channels;
            for (unsigned c = 0; c < channels; ++c) source[std::size_t(x) * channels + c] = picture.value(at + c);
            if (alpha) {
                const float a = source[std::size_t(x) * channels + channels - 1];
                for (unsigned c = 0; c + 1 < channels; ++c) source[std::size_t(x) * channels + c] *= a;
            }
        }
        std::vector<float> row(std::size_t(width) * channels, 0.0f);
        for (std::uint32_t x = 0; x < width; ++x) {
            const Taps& t = across[x];
            for (std::size_t k = 0; k < t.weights.size(); ++k) {
                const float w = t.weights[k];
                const float* s = source.data() + (t.first + k) * channels;
                for (unsigned c = 0; c < channels; ++c) row[std::size_t(x) * channels + c] += w * s[c];
            }
        }
        window.emplace_back(y, std::move(row));
        return window.back().second;
    };
    CodecImage out = CodecImage::blank(width, height, picture.layout(), picture.sample());
    out.icc = picture.icc;
    out.meta = picture.meta;
    const std::size_t row_bytes = std::size_t(width) * channels * sample_bytes(picture.sample());
    std::vector<float> line(std::size_t(width) * channels);
    for (std::uint32_t y = 0; y < height; ++y) {
        const Taps& t = down[y];
        while (!window.empty() && window.front().first < t.first) window.pop_front();  // (rows no output row needs again)
        std::fill(line.begin(), line.end(), 0.0f);
        for (std::size_t k = 0; k < t.weights.size(); ++k) {
            const std::vector<float>& row = filtered(t.first + k);
            const float w = t.weights[k];
            for (std::size_t i = 0; i < line.size(); ++i) line[i] += w * row[i];
        }
        for (std::uint32_t x = 0; x < width; ++x) {
            float* p = line.data() + std::size_t(x) * channels;
            if (alpha) {
                const float a = std::clamp(p[channels - 1], 0.0f, 1.0f);  // (an alpha is 0..1 at every depth)
                p[channels - 1] = a;
                for (unsigned c = 0; c + 1 < channels; ++c) p[c] = a > 0.0f ? p[c] / a : 0.0f;
            }
            if (!hdr) {
                for (unsigned c = 0; c < channels; ++c) p[c] = std::clamp(p[c], 0.0f, 1.0f);
            }
        }
        const std::string stored = quantize(line, picture.sample());
        std::memcpy(out.data().data() + std::size_t(y) * row_bytes, stored.data(), row_bytes);
    }
    return out;
}

}  // namespace genko::render::codecs
