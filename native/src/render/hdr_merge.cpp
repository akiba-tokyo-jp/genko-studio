// HDR Pro に統合 (PC5 I-h): PhotoCraft v0.2.0's algo/src/hdr.rs and photo_util.rs (solve_dense, Normal, the sRGB curve)
// in C++ — MIT, native/third_party/photocraft/NOTICE.md. What Genko adds (linear shots, the shots read as held) is said in
// render/hdr_merge.hpp.

#include "render/hdr_merge.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>

#include "core/error.hpp"
#include "render/page.hpp"

namespace genko::render::hdr {

namespace {

float luma(float r, float g, float b) { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }
float luma(const Rgba& c) { return luma(c[0], c[1], c[2]); }

// Hat weight on a 0..=255 code value.
double hat(double z) { return std::max(z <= 127.5 ? z : 255.0 - z, 0.0); }

void check_stop(const std::function<bool()>& stopped) {
    if (stopped && stopped()) throw Cancelled();
}

// Gaussian elimination with partial pivoting (photo_util::solve_dense): none when singular or not finite.
std::optional<std::vector<double>> solve_dense(std::size_t n, std::vector<double> a, std::vector<double> b) {
    for (std::size_t col = 0; col < n; ++col) {
        std::size_t piv = col;
        for (std::size_t i = col + 1; i < n; ++i)
            if (std::abs(a[i * n + col]) > std::abs(a[piv * n + col])) piv = i;
        if (std::abs(a[piv * n + col]) < 1e-14) return std::nullopt;
        if (piv != col) {
            for (std::size_t k = 0; k < n; ++k) std::swap(a[col * n + k], a[piv * n + k]);
            std::swap(b[col], b[piv]);
        }
        const double d = a[col * n + col];
        for (std::size_t r = col + 1; r < n; ++r) {
            const double f = a[r * n + col] / d;
            if (f == 0.0) continue;
            for (std::size_t k = col; k < n; ++k) a[r * n + k] -= f * a[col * n + k];
            b[r] -= f * b[col];
        }
    }
    std::vector<double> x(n, 0.0);
    for (std::size_t r = n; r-- > 0;) {
        double s = 0;
        for (std::size_t k = r + 1; k < n; ++k) s += a[r * n + k] * x[k];
        x[r] = (b[r] - s) / a[r * n + r];
    }
    for (double v : x)
        if (!std::isfinite(v)) return std::nullopt;
    return x;
}

// Weighted rows gathered into the normal equations AᵀA x = Aᵀb (photo_util::Normal).
struct Normal {
    std::size_t n;
    std::vector<double> ata, atb;
    explicit Normal(std::size_t size) : n(size), ata(size * size, 0.0), atb(size, 0.0) {}
    void add(std::initializer_list<std::pair<std::size_t, double>> row, double rhs, double w) {
        const double w2 = w * w;
        for (const auto& [i, a] : row) {
            atb[i] += w2 * a * rhs;
            for (const auto& [j, b] : row) ata[i * n + j] += w2 * a * b;
        }
    }
};

double g_at(const std::vector<double>& g, float v) {
    const double x = static_cast<double>(std::clamp(v, 0.0f, 1.0f) * 255.0f);
    const std::size_t i = std::min<std::size_t>(static_cast<std::size_t>(std::floor(x)), 254);
    const double t = x - static_cast<double>(i);
    return g[i] * (1.0 - t) + g[i + 1] * t;
}

template <class T>
T read_sample(const std::string& data, std::size_t i) {
    T v;
    std::memcpy(&v, data.data() + i * sizeof(T), sizeof(T));
    return v;
}

}  // namespace

float srgb_to_linear(float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); }
float linear_to_srgb(float v) { return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f; }

// --- the shots -----------------------------------------------------------------------------------------------------------

Shot::Shot(std::string shot_name, codecs::CodecImage picture, std::optional<double> shot_exposure)
    : name(std::move(shot_name)), exposure(shot_exposure), picture_(std::move(picture)) {
    if (picture_.layout() != codecs::Layout::Rgb || (picture_.sample() != codecs::Sample::U8 && picture_.sample() != codecs::Sample::U16 && !linear()))
        throw core::Error("value", "an exposure is RGB of 8 or 16 bits or floats");
    if (linear()) {
        // (the level it clips at: its largest finite value — a sensor's white —, 1 when it holds none above 0)
        float most = 0.0f;
        const std::size_t n = picture_.sample_count();
        for (std::size_t i = 0; i < n; ++i) {
            const float v = read_sample<float>(picture_.data(), i);
            if (std::isfinite(v)) most = std::max(most, v);
        }
        white_ = most > 0.0f ? most : 1.0f;
    }
}

float Shot::value(std::int64_t x, std::int64_t y, int c) const {
    const std::int64_t w = picture_.width(), h = picture_.height();
    const std::int64_t sx = std::clamp<std::int64_t>(x - dx, 0, w - 1), sy = std::clamp<std::int64_t>(y - dy, 0, h - 1);
    const std::size_t i = (static_cast<std::size_t>(sy) * static_cast<std::size_t>(w) + static_cast<std::size_t>(sx)) * 3 + static_cast<std::size_t>(c);
    switch (picture_.sample()) {
        case codecs::Sample::U8: return static_cast<float>(static_cast<unsigned char>(picture_.data()[i])) / 255.0f;
        case codecs::Sample::U16: return static_cast<float>(read_sample<std::uint16_t>(picture_.data(), i)) / 65535.0f;
        default: {
            const float v = read_sample<float>(picture_.data(), i);
            return std::isfinite(v) ? std::max(v, 0.0f) : 0.0f;
        }
    }
}

float Shot::display(std::int64_t x, std::int64_t y, int c) const {
    const float v = value(x, y, c);
    return linear() ? linear_to_srgb(std::clamp(v / white_, 0.0f, 1.0f)) : v;
}

float Shot::light(std::int64_t x, std::int64_t y, int c) const {
    const float v = value(x, y, c);
    return linear() ? v : srgb_to_linear(v);
}

codecs::CodecImage rgb_of(codecs::CodecImage picture) {
    if (codecs::is_cmyk(picture.layout())) throw core::Error("value", "an exposure of CMYK cannot be merged (choose RGB or grey pictures)");
    const codecs::Sample sample = codecs::is_float(picture.sample()) ? codecs::Sample::F32 : picture.sample();
    if (picture.layout() == codecs::Layout::Rgb && picture.sample() == sample) return picture;
    return picture.convert(codecs::Layout::Rgb, sample);
}

std::optional<double> exposure_factor(std::optional<double> seconds, std::optional<double> f_number, std::optional<double> iso) {
    if (!seconds || !std::isfinite(*seconds) || *seconds <= 0) return std::nullopt;
    const double n = std::max(f_number && std::isfinite(*f_number) && *f_number > 0 ? *f_number : 1.0, 0.1);
    const double speed = iso && std::isfinite(*iso) && *iso > 0 ? *iso : 100.0;
    return *seconds * speed / 100.0 / (n * n);
}

Shot reduced(const Shot& shot, int max_side) {
    const std::uint32_t w = shot.width(), h = shot.height();
    const std::uint32_t k = std::max<std::uint32_t>(1, (std::max(w, h) + static_cast<std::uint32_t>(std::max(max_side, 1)) - 1) / static_cast<std::uint32_t>(std::max(max_side, 1)));
    const std::uint32_t sw = std::max<std::uint32_t>(1, (w + k - 1) / k), sh = std::max<std::uint32_t>(1, (h + k - 1) / k);
    std::vector<float> sums(std::size_t(sw) * sh * 3, 0.0f);
    std::vector<float> counts(std::size_t(sw) * sh, 0.0f);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::size_t i = std::size_t(y / k) * sw + x / k;
            for (int c = 0; c < 3; ++c) sums[i * 3 + std::size_t(c)] += shot.value(x + shot.dx, y + shot.dy, c);
            counts[i] += 1.0f;
        }
    }
    for (std::size_t i = 0; i < counts.size(); ++i)
        for (int c = 0; c < 3; ++c) sums[i * 3 + std::size_t(c)] /= std::max(counts[i], 1.0f);
    const codecs::Sample sample = shot.picture().sample();
    codecs::CodecImage small = sample == codecs::Sample::F32
                                   ? codecs::CodecImage::from_bytes(sw, sh, codecs::Layout::Rgb, sample, std::string(reinterpret_cast<const char*>(sums.data()), sums.size() * sizeof(float)))
                                   : codecs::CodecImage::from_normalized(sw, sh, codecs::Layout::Rgb, sample, sums);
    Shot out(shot.name, std::move(small), shot.exposure);
    return out;
}

// --- exposures -------------------------------------------------------------------------------------------------------------

std::vector<double> estimate_exposures(const std::vector<const Shot*>& shots) {
    const std::size_t n = shots.size();
    if (n == 0) return {};
    const std::int64_t w = shots[0]->width(), h = shots[0]->height();
    const std::size_t count = static_cast<std::size_t>(w * h);
    std::vector<double> mean(n, 0.0);
    for (std::size_t j = 0; j < n; ++j) {
        double s = 0;
        for (std::int64_t y = 0; y < h; ++y)
            for (std::int64_t x = 0; x < w; ++x) s += luma(shots[j]->display(x, y, 0), shots[j]->display(x, y, 1), shots[j]->display(x, y, 2));
        mean[j] = s / static_cast<double>(std::max<std::size_t>(count, 1));
    }
    std::vector<std::size_t> order(n);
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return mean[a] < mean[b]; });
    std::vector<double> ex(n, 1.0);
    for (std::size_t k = 1; k < n; ++k) {
        const Shot& a = *shots[order[k - 1]];
        const Shot& b = *shots[order[k]];
        std::vector<double> ratios;
        for (std::size_t i = 0; i < count; i += 7) {
            const std::int64_t x = static_cast<std::int64_t>(i % static_cast<std::size_t>(w)), y = static_cast<std::int64_t>(i / static_cast<std::size_t>(w));
            const float la = luma(a.display(x, y, 0), a.display(x, y, 1), a.display(x, y, 2));
            const float lb = luma(b.display(x, y, 0), b.display(x, y, 1), b.display(x, y, 2));
            if (!(la >= 0.08f && la < 0.92f && lb >= 0.08f && lb < 0.92f)) continue;
            // (light: display values through sRGB's curve, as hdr.rs; linear shots their own light)
            const double light_a = a.linear() ? luma(a.value(x, y, 0), a.value(x, y, 1), a.value(x, y, 2)) : srgb_to_linear(la);
            const double light_b = b.linear() ? luma(b.value(x, y, 0), b.value(x, y, 1), b.value(x, y, 2)) : srgb_to_linear(lb);
            ratios.push_back(light_b / std::max(light_a, 1e-6));
        }
        std::sort(ratios.begin(), ratios.end());
        const double r = ratios.empty() ? 2.0 : std::max(ratios[ratios.size() / 2], 1.0);
        ex[order[k]] = ex[order[k - 1]] * r;
    }
    return ex;
}

// --- alignment ---------------------------------------------------------------------------------------------------------------

namespace {

struct Grey {
    std::size_t w = 0, h = 0;
    std::vector<float> g;
};

Grey shrink(const Grey& in) {
    Grey out;
    out.w = std::max<std::size_t>(in.w / 2, 1);
    out.h = std::max<std::size_t>(in.h / 2, 1);
    out.g.assign(out.w * out.h, 0.0f);
    const auto p = [&](std::size_t xx, std::size_t yy) { return in.g[std::min(yy, in.h - 1) * in.w + std::min(xx, in.w - 1)]; };
    for (std::size_t y = 0; y < out.h; ++y)
        for (std::size_t x = 0; x < out.w; ++x)
            out.g[y * out.w + x] = 0.25f * (p(2 * x, 2 * y) + p(2 * x + 1, 2 * y) + p(2 * x, 2 * y + 1) + p(2 * x + 1, 2 * y + 1));
    return out;
}

void bitmaps(const std::vector<float>& g, std::vector<char>& threshold, std::vector<char>& exclusion) {
    std::vector<float> s = g;
    std::nth_element(s.begin(), s.begin() + static_cast<std::ptrdiff_t>(s.size() / 2), s.end());
    const float med = s[s.size() / 2];
    threshold.resize(g.size());
    exclusion.resize(g.size());
    for (std::size_t i = 0; i < g.size(); ++i) {
        threshold[i] = g[i] > med ? 1 : 0;
        exclusion[i] = std::abs(g[i] - med) > 4.0f / 255.0f ? 1 : 0;
    }
}

std::pair<int, int> mtb(const Grey& reference, const Grey& img, int levels) {
    const std::size_t w = reference.w, h = reference.h;
    if (levels == 0 || w < 16 || h < 16) return {0, 0};
    const auto [sx, sy] = mtb(shrink(reference), shrink(img), levels - 1);
    const int cur_x = sx * 2, cur_y = sy * 2;
    std::vector<char> tr, er, ti, ei;
    bitmaps(reference.g, tr, er);
    bitmaps(img.g, ti, ei);
    std::uint64_t best = std::numeric_limits<std::uint64_t>::max();
    std::pair<int, int> at{0, 0};
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const int ox = cur_x + dx, oy = cur_y + dy;
            std::uint64_t err = 0;
            for (int y = 0; y < static_cast<int>(h); ++y) {
                const int yy = y - oy;
                if (yy < 0 || yy >= static_cast<int>(h)) continue;
                for (int x = 0; x < static_cast<int>(w); ++x) {
                    const int xx = x - ox;
                    if (xx < 0 || xx >= static_cast<int>(w)) continue;
                    const std::size_t a = std::size_t(y) * w + std::size_t(x), b = std::size_t(yy) * w + std::size_t(xx);
                    if (tr[a] != ti[b] && er[a] && ei[b]) ++err;
                }
            }
            if (err < best) {
                best = err;
                at = {ox, oy};
            }
        }
    }
    return at;
}

}  // namespace

std::pair<int, int> mtb_offset(std::size_t w, std::size_t h, const std::vector<float>& reference, const std::vector<float>& img, int levels) {
    return mtb(Grey{w, h, reference}, Grey{w, h, img}, levels);
}

void align(std::vector<Shot>& shots, const std::vector<double>& exposures, const std::function<bool()>& stopped) {
    if (shots.size() < 2) return;
    std::vector<std::size_t> order(shots.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return exposures[a] < exposures[b]; });
    const std::size_t mid = order[order.size() / 2];
    const std::size_t w = shots[0].width(), h = shots[0].height();
    const std::size_t k = std::max<std::size_t>(1, (std::max(w, h) + 1535) / 1536);
    const std::size_t sw = (w + k - 1) / k, sh = (h + k - 1) / k;
    const auto grey = [&](const Shot& s) {
        Grey g{sw, sh, std::vector<float>(sw * sh, 0.0f)};
        std::vector<float> c(sw * sh, 0.0f);
        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < w; ++x) {
                const std::size_t j = (y / k) * sw + x / k;
                const auto X = static_cast<std::int64_t>(x), Y = static_cast<std::int64_t>(y);
                g.g[j] += (54.0f * s.display(X, Y, 0) + 183.0f * s.display(X, Y, 1) + 19.0f * s.display(X, Y, 2)) / 256.0f;
                c[j] += 1.0f;
            }
        }
        for (std::size_t j = 0; j < g.g.size(); ++j) g.g[j] /= std::max(c[j], 1.0f);
        return g;
    };
    for (Shot& s : shots) s.dx = s.dy = 0;
    const Grey reference = grey(shots[mid]);
    for (std::size_t i = 0; i < shots.size(); ++i) {
        check_stop(stopped);
        if (i == mid) continue;
        const auto [dx, dy] = mtb(reference, grey(shots[i]), 5);
        shots[i].dx = dx * static_cast<int>(k);
        shots[i].dy = dy * static_cast<int>(k);
    }
}

// --- the response curve ----------------------------------------------------------------------------------------------------

std::vector<std::size_t> sample_pixels(const Shot& middle, std::size_t count) {
    const std::size_t w = middle.width(), h = middle.height();
    std::vector<std::vector<std::size_t>> by_bin(32);
    const auto lum = [&](std::size_t i) {
        const auto x = static_cast<std::int64_t>(i % w), y = static_cast<std::int64_t>(i / w);
        return luma(middle.display(x, y, 0), middle.display(x, y, 1), middle.display(x, y, 2));
    };
    const std::size_t step = std::max<std::size_t>(1, static_cast<std::size_t>(std::max(1.0, std::sqrt(static_cast<double>(w * h / std::max<std::size_t>(count * 40, 1))))));
    for (std::size_t y = 2; y + 2 < h; y += step) {
        for (std::size_t x = 2; x + 2 < w; x += step) {
            const std::size_t i = y * w + x;
            const float l = lum(i);
            bool flat = true;
            for (const std::size_t j : {i - 2, i + 2, i - 2 * w, i + 2 * w}) flat = flat && std::abs(lum(j) - l) < 0.04f;
            if (flat) by_bin[std::min<std::size_t>(static_cast<std::size_t>(l * 31.99f), 31)].push_back(i);
        }
    }
    // (round-robin over the bins so every intensity is represented)
    std::vector<std::size_t> out;
    std::size_t k = 0;
    const auto any_left = [&] {
        for (const auto& b : by_bin)
            if (k < b.size()) return true;
        return false;
    };
    while (out.size() < count && any_left()) {
        for (const auto& b : by_bin) {
            if (k < b.size()) {
                out.push_back(b[k * 7 % std::max<std::size_t>(b.size(), 1)]);
                if (out.size() >= count) break;
            }
        }
        ++k;
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<double> response_curve(const std::vector<std::vector<std::uint8_t>>& z, const std::vector<double>& log_dt, double lambda) {
    const std::size_t p = z.size();
    const std::size_t n = z.empty() ? 0 : z[0].size();
    Normal ne(256 + n);
    for (std::size_t j = 0; j < p; ++j) {
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t zz = z[j][i];
            const double w = hat(static_cast<double>(zz)) + 1.0;
            ne.add({{zz, 1.0}, {256 + i, -1.0}}, log_dt[j], w);
        }
    }
    ne.add({{128, 1.0}}, 0.0, 100.0);
    for (std::size_t k = 1; k < 255; ++k) {
        const double w = std::sqrt(lambda) * (hat(static_cast<double>(k)) + 1.0);
        ne.add({{k - 1, 1.0}, {k, -2.0}, {k + 1, 1.0}}, 0.0, w);
    }
    const auto x = solve_dense(ne.n, std::move(ne.ata), std::move(ne.atb));
    std::vector<double> g(256);
    if (!x) {
        // (sRGB's curve in its place)
        for (std::size_t k = 0; k < 256; ++k)
            g[k] = std::log(std::max<double>(srgb_to_linear(static_cast<float>(k) / 255.0f), 1e-5)) - std::log(static_cast<double>(srgb_to_linear(128.0f / 255.0f)));
        return g;
    }
    std::copy(x->begin(), x->begin() + 256, g.begin());
    // (monotonic: noise at the extremes can fold the curve)
    for (std::size_t k = 128; k-- > 0;) g[k] = std::min(g[k], g[k + 1] - 1e-4);
    for (std::size_t k = 129; k < 256; ++k) g[k] = std::max(g[k], g[k - 1] + 1e-4);
    return g;
}

// --- the merge ---------------------------------------------------------------------------------------------------------------

Merged merge(const std::vector<Shot>& shots, const MergeOptions& options) {
    const std::size_t p = shots.size();
    if (p == 0) throw core::Error("value", "no exposure to merge");
    if (options.exposures.size() != p) throw core::Error("value", "one exposure value an image");
    const std::size_t w = shots[0].width(), h = shots[0].height();
    const bool linear = shots[0].linear();
    for (const Shot& s : shots) {
        if (s.width() != w || s.height() != h) throw core::Error("value", "the exposures must have the same pixel dimensions");
        if (s.linear() != linear) throw core::Error("value", "the exposures must all be camera RAW files (linear light) or all not");
    }
    std::vector<double> log_dt(p);
    for (std::size_t j = 0; j < p; ++j) log_dt[j] = std::log(std::max(options.exposures[j], 1e-12));
    const auto px_at = [w](std::size_t i) { return std::pair<std::int64_t, std::int64_t>(static_cast<std::int64_t>(i % w), static_cast<std::int64_t>(i / w)); };
    const auto lum_display = [&](const Shot& s, std::int64_t x, std::int64_t y) { return luma(s.display(x, y, 0), s.display(x, y, 1), s.display(x, y, 2)); };
    // (the base: the given one, else the one with most well-exposed pixels)
    std::size_t base = 0;
    if (options.ghost_base && *options.ghost_base < p) {
        base = *options.ghost_base;
    } else {
        std::size_t most = 0;
        for (std::size_t j = 0; j < p; ++j) {
            std::size_t well = 0;
            for (std::size_t i = 0; i < w * h; i += 3) {
                const auto [x, y] = px_at(i);
                const float l = lum_display(shots[j], x, y);
                if (l >= 0.1f && l < 0.9f) ++well;
            }
            if (j == 0 || well > most) {
                most = well;
                base = j;
            }
        }
    }
    Merged out;
    out.width = static_cast<std::uint32_t>(w);
    out.height = static_cast<std::uint32_t>(h);
    out.ghost_base = base;
    if (!linear) {
        if (options.response) {
            out.response = options.response;
        } else {
            std::vector<std::size_t> order(p);
            std::iota(order.begin(), order.end(), 0);
            std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return options.exposures[a] < options.exposures[b]; });
            const Shot& mid = shots[order[p / 2]];
            const std::vector<std::size_t> samples = sample_pixels(mid, 160);
            std::array<std::vector<double>, 3> response;
            for (int c = 0; c < 3; ++c) {
                std::vector<std::vector<std::uint8_t>> z(p);
                for (std::size_t j = 0; j < p; ++j) {
                    for (const std::size_t i : samples) {
                        const auto [x, y] = px_at(i);
                        z[j].push_back(static_cast<std::uint8_t>(std::lround(std::clamp(shots[j].value(x, y, c), 0.0f, 1.0f) * 255.0f)));
                    }
                }
                response[std::size_t(c)] = response_curve(z, log_dt, 40.0);
            }
            out.response = std::move(response);
        }
    }
    // (ln of the radiance shot j says of channel c: its response less ln Δt — a linear shot's own light —, and the weight)
    const auto log_radiance = [&](std::size_t j, std::int64_t x, std::int64_t y, int c) -> double {
        if (linear) return std::log(std::max<double>(shots[j].value(x, y, c), 1e-12)) - log_dt[j];
        return g_at((*out.response)[std::size_t(c)], shots[j].value(x, y, c)) - log_dt[j];
    };
    const image_ops::Held held(std::uint64_t(w) * h * 3 * sizeof(float) + (options.remove_ghosts ? std::uint64_t(w) * h * sizeof(float) : 0));
    out.rgb.assign(w * h * 3, 0.0f);
    for (std::size_t y = 0; y < h; ++y) {
        if (y % 32 == 0) check_stop(options.stopped);
        const auto Y = static_cast<std::int64_t>(y);
        for (std::size_t x = 0; x < w; ++x) {
            const auto X = static_cast<std::int64_t>(x);
            float* o = out.rgb.data() + (y * w + x) * 3;
            for (int c = 0; c < 3; ++c) {
                double num = 0, den = 0;
                for (std::size_t j = 0; j < p; ++j) {
                    const double wt = hat(static_cast<double>(std::clamp(shots[j].display(X, Y, c), 0.0f, 1.0f)) * 255.0);
                    if (wt <= 0.0) continue;
                    num += wt * log_radiance(j, X, Y, c);
                    den += wt;
                }
                double le = 0;
                if (den > 1e-6) {
                    le = num / den;
                } else {
                    // (saturated everywhere: the darkest exposure if bright, the brightest if dark)
                    float sum = 0;
                    for (std::size_t j = 0; j < p; ++j) sum += shots[j].display(X, Y, c);
                    const bool bright = sum / static_cast<float>(p) > 0.5f;
                    std::size_t pick = 0;
                    for (std::size_t j = 1; j < p; ++j)
                        if (bright ? log_dt[j] < log_dt[pick] : log_dt[j] > log_dt[pick]) pick = j;
                    le = log_radiance(pick, X, Y, c);
                }
                o[c] = static_cast<float>(std::exp(le));
            }
        }
    }
    if (options.remove_ghosts && p > 1) {
        // (the weighted spread of the exposures' log radiance — their luminance)
        std::vector<float> ghost(w * h, 0.0f);
        for (std::size_t y = 0; y < h; ++y) {
            if (y % 32 == 0) check_stop(options.stopped);
            const auto Y = static_cast<std::int64_t>(y);
            for (std::size_t x = 0; x < w; ++x) {
                const auto X = static_cast<std::int64_t>(x);
                double s = 0, s2 = 0, sw = 0;
                for (std::size_t j = 0; j < p; ++j) {
                    const double wt = hat(static_cast<double>(lum_display(shots[j], X, Y)) * 255.0);
                    if (wt < 20.0) continue;
                    double lr = 0;
                    for (int c = 0; c < 3; ++c) lr += 0.33 * log_radiance(j, X, Y, c);
                    s += wt * lr;
                    s2 += wt * lr * lr;
                    sw += wt;
                }
                if (sw <= 0.0) continue;
                const double m = s / sw;
                ghost[y * w + x] = std::sqrt(std::max(s2 / sw - m * m, 0.0)) > 0.35 ? 1.0f : 0.0f;
            }
        }
        // (grown and feathered)
        const float r = std::max(static_cast<float>(std::max(w, h)) / 200.0f, 2.0f);
        image_ops::gauss_blur(ghost, w, h, 1, r);
        std::size_t ghosts = 0;
        for (float& g : ghost) {
            g = std::min(g * 3.0f, 1.0f);
            if (g > 0.5f) ++ghosts;
        }
        out.ghost_fraction = static_cast<double>(ghosts) / static_cast<double>(std::max<std::size_t>(w * h, 1));
        for (std::size_t i = 0; i < w * h; ++i) {
            if (ghost[i] <= 0.0f) continue;
            const auto [x, y] = px_at(i);
            for (int c = 0; c < 3; ++c) {
                const auto e = static_cast<float>(std::exp(log_radiance(base, x, y, c)));
                float& v = out.rgb[i * 3 + std::size_t(c)];
                v = v * (1.0f - ghost[i]) + e * ghost[i];
            }
        }
    }
    // (scaled per channel so the base exposure's well-exposed pixels keep their linear values)
    const Shot& b = shots[base];
    for (int c = 0; c < 3; ++c) {
        std::vector<double> r;
        for (std::size_t i = 0; i < w * h; i += 37) {
            const auto [x, y] = px_at(i);
            const float v = b.display(x, y, c);
            const float m = out.rgb[i * 3 + std::size_t(c)];
            if (v >= 0.15f && v < 0.85f && m > 0.0f) r.push_back(static_cast<double>(b.light(x, y, c)) / (static_cast<double>(m) * options.exposures[base]));
        }
        std::sort(r.begin(), r.end());
        const double k = (r.empty() ? 1.0 : r[r.size() / 2]) * options.exposures[base];
        for (std::size_t i = 0; i < w * h; ++i) {
            float& v = out.rgb[i * 3 + std::size_t(c)];
            v = static_cast<float>(static_cast<double>(v) * k);
        }
    }
    std::vector<float> l;
    for (std::size_t i = 0; i < w * h; i += 13) l.push_back(std::max(luma(out.rgb[i * 3], out.rgb[i * 3 + 1], out.rgb[i * 3 + 2]), 1e-9f));
    std::sort(l.begin(), l.end());
    if (!l.empty()) {
        const auto top = static_cast<std::size_t>(static_cast<double>(l.size()) * 0.999) % l.size();
        out.stops = std::log2(static_cast<double>(l[top]) / static_cast<double>(l[l.size() / 100]));
    }
    return out;
}

// --- the tone mapping ------------------------------------------------------------------------------------------------------

std::optional<Tone::Method> method_from(std::string_view text) {
    if (text == "local_adaptation") return Tone::Method::LocalAdaptation;
    if (text == "exposure_gamma") return Tone::Method::ExposureGamma;
    if (text == "highlight_compression") return Tone::Method::HighlightCompression;
    if (text == "equalize_histogram") return Tone::Method::EqualizeHistogram;
    return std::nullopt;
}

const char* method_name(Tone::Method method) {
    switch (method) {
        case Tone::Method::LocalAdaptation: return "local_adaptation";
        case Tone::Method::ExposureGamma: return "exposure_gamma";
        case Tone::Method::HighlightCompression: return "highlight_compression";
        case Tone::Method::EqualizeHistogram: return "equalize_histogram";
    }
    return "local_adaptation";
}

void tone_map(std::vector<Rgba>& px, std::size_t w, std::size_t h, const Tone& tone) {
    std::size_t n = 0;
    double logs = 0;
    for (const Rgba& q : px) {
        if (!(q[3] > 0.0f)) continue;
        ++n;
        logs += std::log(static_cast<double>(std::max(luma(q), 1e-6f)));
    }
    const auto log_mean = static_cast<float>(std::exp(logs / static_cast<double>(std::max<std::size_t>(n, 1))));
    switch (tone.method) {
        case Tone::Method::LocalAdaptation: {
            // (the scene keyed to middle grey, compressed in log luminance, then encoded)
            const float k = 0.18f / std::max(log_mean, 1e-6f);
            for (Rgba& q : px)
                for (int c = 0; c < 3; ++c) q[std::size_t(c)] *= k;
            image_ops::hdr_toning(px, w, h, tone.local);
            for (Rgba& q : px)
                for (int c = 0; c < 3; ++c) q[std::size_t(c)] = linear_to_srgb(std::clamp(q[std::size_t(c)], 0.0f, 1.0f));
            break;
        }
        case Tone::Method::ExposureGamma: {
            const float k = std::pow(2.0f, tone.exposure);
            const float g = std::clamp(tone.gamma, 0.1f, 10.0f);
            for (Rgba& q : px)
                for (int c = 0; c < 3; ++c) q[std::size_t(c)] = std::pow(linear_to_srgb(std::clamp(q[std::size_t(c)] * k, 0.0f, 1.0f)), 1.0f / g);
            break;
        }
        case Tone::Method::HighlightCompression: {
            const float k = 0.18f / std::max(log_mean, 1e-6f);
            for (Rgba& q : px) {
                const float l = luma(q) * k;
                const float ld = l / (1.0f + l);
                const float r = l > 1e-9f ? ld / l * k : 0.0f;
                for (int c = 0; c < 3; ++c) q[std::size_t(c)] = linear_to_srgb(std::clamp(q[std::size_t(c)] * r, 0.0f, 1.0f));
            }
            break;
        }
        case Tone::Method::EqualizeHistogram: {
            constexpr std::size_t kBins = 1024;
            std::vector<float> lv(px.size());
            float lo = std::numeric_limits<float>::max(), hi = std::numeric_limits<float>::lowest();
            for (std::size_t i = 0; i < px.size(); ++i) {
                lv[i] = std::log(std::max(luma(px[i]), 1e-7f));
                if (px[i][3] > 0.0f) {
                    lo = std::min(lo, lv[i]);
                    hi = std::max(hi, lv[i]);
                }
            }
            const float span = std::max(hi - lo, 1e-6f);
            std::vector<double> hist(kBins, 0.0);
            for (std::size_t i = 0; i < px.size(); ++i) {
                if (px[i][3] > 0.0f) hist[std::min<std::size_t>(static_cast<std::size_t>((lv[i] - lo) / span * static_cast<float>(kBins - 1)), kBins - 1)] += 1.0;
            }
            const double total = std::max(std::accumulate(hist.begin(), hist.end(), 0.0), 1.0);
            std::vector<float> cdf(kBins);
            double acc = 0;
            for (std::size_t b = 0; b < kBins; ++b) {
                acc += hist[b];
                cdf[b] = static_cast<float>(acc / total);
            }
            for (std::size_t i = 0; i < px.size(); ++i) {
                const float target = cdf[static_cast<std::size_t>(std::clamp((lv[i] - lo) / span, 0.0f, 1.0f) * static_cast<float>(kBins - 1))];
                // (the display value from the CDF; the colours' ratios kept in linear light)
                const float tl = srgb_to_linear(target);
                const float l = std::max(luma(px[i]), 1e-7f);
                for (int c = 0; c < 3; ++c) px[i][std::size_t(c)] = linear_to_srgb(std::clamp(px[i][std::size_t(c)] / l * tl, 0.0f, 1.0f));
            }
            break;
        }
    }
}

}  // namespace genko::render::hdr
