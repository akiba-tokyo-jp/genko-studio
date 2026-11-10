#include "render/selection_ext.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "render/selection.hpp"

// PhotoCraft v0.2.0 crates/algo/src/selection.rs (lines 80-655), MIT: native/third_party/photocraft/NOTICE.md.

namespace genko::render::selection_ext {

namespace {

bool close(const Rgba& a, const Rgba& b, float tol) {
    for (int c = 0; c < 4; ++c) {
        if (std::fabs(a[c] - b[c]) * 255.0f > tol + 1e-3f) return false;
    }
    return true;
}

// Softens hard mask edges: an edge pixel becomes 0.5 + half its 3×3 mean (selected) or half of it (not).
void antialias(std::vector<float>& m, std::size_t w, std::size_t h) {
    const std::vector<float> src = m;
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            const std::size_t i = y * w + x;
            float sum = 0.0f;
            float cnt = 0.0f;
            bool edge = false;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const long long xx = static_cast<long long>(x) + dx;
                    const long long yy = static_cast<long long>(y) + dy;
                    const float v = (xx < 0 || yy < 0 || xx >= static_cast<long long>(w) || yy >= static_cast<long long>(h))
                                        ? src[i]
                                        : src[static_cast<std::size_t>(yy) * w + static_cast<std::size_t>(xx)];
                    edge = edge || v != src[i];
                    sum += v;
                    cnt += 1.0f;
                }
            }
            if (edge) m[i] = src[i] > 0.5f ? 0.5f + 0.5f * sum / cnt : 0.5f * sum / cnt;
        }
    }
}

// 1D squared distance transform (Felzenszwalb & Huttenlocher).
void dt1(const float* f, std::size_t n, float* out, std::vector<std::size_t>& v, std::vector<float>& z) {
    std::size_t k = 0;
    v[0] = 0;
    z[0] = -std::numeric_limits<float>::infinity();
    z[1] = std::numeric_limits<float>::infinity();
    for (std::size_t q = 1; q < n; ++q) {
        for (;;) {
            const std::size_t p = v[k];
            const float s = ((f[q] + static_cast<float>(q * q)) - (f[p] + static_cast<float>(p * p))) /
                            (2.0f * (static_cast<float>(q) - static_cast<float>(p)));
            if (s <= z[k] && k > 0) {
                --k;
                continue;
            }
            if (s <= z[k]) {
                v[0] = q;
                z[0] = -std::numeric_limits<float>::infinity();
                z[1] = std::numeric_limits<float>::infinity();
                break;
            }
            ++k;
            v[k] = q;
            z[k] = s;
            z[k + 1] = std::numeric_limits<float>::infinity();
            break;
        }
    }
    k = 0;
    for (std::size_t q = 0; q < n; ++q) {
        while (z[k + 1] < static_cast<float>(q)) ++k;
        const float d = static_cast<float>(q) - static_cast<float>(v[k]);
        out[q] = d * d + f[v[k]];
    }
}

std::vector<float> box_blur(const std::vector<float>& m, std::size_t w, std::size_t h, std::size_t r) {
    if (r == 0) return m;
    std::vector<float> tmp(m.size(), 0.0f);
    const float win = static_cast<float>(2 * r + 1);
    const long long ri = static_cast<long long>(r);
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            float s = 0.0f;
            for (long long k = static_cast<long long>(x) - ri; k <= static_cast<long long>(x) + ri; ++k) {
                s += m[y * w + static_cast<std::size_t>(std::clamp<long long>(k, 0, static_cast<long long>(w) - 1))];
            }
            tmp[y * w + x] = s / win;
        }
    }
    std::vector<float> out(m.size(), 0.0f);
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            float s = 0.0f;
            for (long long k = static_cast<long long>(y) - ri; k <= static_cast<long long>(y) + ri; ++k) {
                s += tmp[static_cast<std::size_t>(std::clamp<long long>(k, 0, static_cast<long long>(h) - 1)) * w + x];
            }
            out[y * w + x] = s / win;
        }
    }
    return out;
}

std::pair<float, float> hue_chroma(float r, float g, float b) {
    const float mx = std::max(std::max(r, g), b);
    const float mn = std::min(std::min(r, g), b);
    const float d = mx - mn;
    if (d <= 0.0f) return {0.0f, 0.0f};
    float h = 0.0f;
    if (mx == r) {
        const float t = (g - b) / d;
        h = t - 6.0f * std::floor(t / 6.0f);  // (rem_euclid(6))
        if (h >= 6.0f) h = 0.0f;
    } else if (mx == g) {
        h = (b - r) / d + 2.0f;
    } else {
        h = (r - g) / d + 4.0f;
    }
    return {h * 60.0f, d};
}

float rem360(float v) {
    float r = std::fmod(v, 360.0f);
    if (r < 0.0f) r += 360.0f;
    return r;
}

}  // namespace

float Region::at(int x, int y) const {
    if (!box.contains(x, y)) return 0.0f;
    return static_cast<float>(mask[static_cast<std::size_t>(y - box.y0) * static_cast<std::size_t>(box.width()) + static_cast<std::size_t>(x - box.x0)]) /
           255.0f;
}

std::vector<float> magic_wand(const std::vector<Rgba>& px, const Rect& area, std::pair<int, int> seed, float tolerance, bool contiguous,
                              bool anti_alias) {
    const std::size_t w = static_cast<std::size_t>(std::max(0, area.width()));
    const std::size_t h = static_cast<std::size_t>(std::max(0, area.height()));
    std::vector<float> m(w * h, 0.0f);
    if (!area.contains(seed.first, seed.second)) return m;
    const std::size_t si = static_cast<std::size_t>(seed.second - area.y0) * w + static_cast<std::size_t>(seed.first - area.x0);
    const Rgba target = px[si];
    if (contiguous) {
        std::vector<std::size_t> stack{si};
        m[si] = 1.0f;
        while (!stack.empty()) {
            const std::size_t i = stack.back();
            stack.pop_back();
            const std::size_t x = i % w, y = i / w;
            const auto visit = [&](std::size_t j) {
                if (m[j] == 0.0f && close(px[j], target, tolerance)) {
                    m[j] = 1.0f;
                    stack.push_back(j);
                }
            };
            if (x > 0) visit(i - 1);
            if (x + 1 < w) visit(i + 1);
            if (y > 0) visit(i - w);
            if (y + 1 < h) visit(i + w);
        }
    } else {
        for (std::size_t i = 0; i < m.size(); ++i)
            if (close(px[i], target, tolerance)) m[i] = 1.0f;
    }
    if (anti_alias) antialias(m, w, h);
    return m;
}

std::optional<Region> wand_region(const std::vector<Rgba8>& img, const Rect& area, std::pair<int, int> seed, float tolerance,
                                  bool contiguous, bool anti_alias) {
    if (!area.contains(seed.first, seed.second)) return std::nullopt;
    const std::size_t w = static_cast<std::size_t>(area.width()), h = static_cast<std::size_t>(area.height());
    const Rgba8 target = img[static_cast<std::size_t>(seed.second - area.y0) * w + static_cast<std::size_t>(seed.first - area.x0)];
    const int tol = static_cast<int>(std::max(0.0f, std::floor(tolerance + 1e-3f)));
    const auto similar = [&](const Rgba8& p) {
        for (int c = 0; c < 4; ++c)
            if (std::abs(static_cast<int>(p[c]) - static_cast<int>(target[c])) > tol) return false;
        return true;
    };
    std::vector<std::uint8_t> marks(w * h, 0);
    std::size_t bx0 = std::numeric_limits<std::size_t>::max(), by0 = std::numeric_limits<std::size_t>::max(), bx1 = 0, by1 = 0;
    if (contiguous) {
        // (span fill: a run filled, then the rows above and below scanned for new seeds)
        std::vector<std::pair<std::size_t, std::size_t>> stack{{static_cast<std::size_t>(seed.first - area.x0), static_cast<std::size_t>(seed.second - area.y0)}};
        while (!stack.empty()) {
            const auto [x, y] = stack.back();
            stack.pop_back();
            const std::size_t row = y * w;
            if (marks[row + x] != 0) continue;
            std::size_t l = x, r = x;
            while (l > 0 && marks[row + l - 1] == 0 && similar(img[row + l - 1])) --l;
            while (r + 1 < w && marks[row + r + 1] == 0 && similar(img[row + r + 1])) ++r;
            std::fill(marks.begin() + static_cast<std::ptrdiff_t>(row + l), marks.begin() + static_cast<std::ptrdiff_t>(row + r + 1), 255);
            bx0 = std::min(bx0, l);
            bx1 = std::max(bx1, r);
            by0 = std::min(by0, y);
            by1 = std::max(by1, y);
            for (const std::size_t ny : {y - 1, y + 1}) {  // (y − 1 wraps past h at the top)
                if (ny >= h) continue;
                const std::size_t nrow = ny * w;
                bool in_run = false;
                for (std::size_t nx = l; nx <= r; ++nx) {
                    const bool ok = marks[nrow + nx] == 0 && similar(img[nrow + nx]);
                    if (ok && !in_run) stack.emplace_back(nx, ny);
                    in_run = ok;
                }
            }
        }
    } else {
        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < w; ++x) {
                if (!similar(img[y * w + x])) continue;
                marks[y * w + x] = 255;
                bx0 = std::min(bx0, x);
                bx1 = std::max(bx1, x);
                by0 = std::min(by0, y);
                by1 = std::max(by1, y);
            }
        }
    }
    if (bx0 == std::numeric_limits<std::size_t>::max()) return std::nullopt;
    // (one pixel of fringe for anti-aliasing, within the area)
    const std::size_t pad = anti_alias ? 1 : 0;
    const std::size_t x0 = bx0 >= pad ? bx0 - pad : 0, y0 = by0 >= pad ? by0 - pad : 0;
    const std::size_t x1 = std::min(bx1 + 1 + pad, w), y1 = std::min(by1 + 1 + pad, h);
    const std::size_t bw = x1 - x0;
    Region out;
    out.mask.assign(bw * (y1 - y0), 0);
    for (std::size_t yy = 0; yy < y1 - y0; ++yy) {
        const std::size_t y = y0 + yy;
        for (std::size_t xx = 0; xx < bw; ++xx) {
            const std::size_t x = x0 + xx;
            const std::uint8_t v = marks[y * w + x];
            std::uint8_t o = v;
            if (anti_alias) {
                // (as antialias: an edge pixel with its 3×3 neighbourhood; outside the area counts as the pixel itself)
                unsigned sum = 0;
                bool edge = false;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const long long nx = static_cast<long long>(x) + dx, ny = static_cast<long long>(y) + dy;
                        const std::uint8_t n = (nx < 0 || ny < 0 || nx >= static_cast<long long>(w) || ny >= static_cast<long long>(h))
                                                   ? v
                                                   : marks[static_cast<std::size_t>(ny) * w + static_cast<std::size_t>(nx)];
                        edge = edge || n != v;
                        sum += n;
                    }
                }
                if (edge) {
                    const float avg = static_cast<float>(sum) / (9.0f * 255.0f);
                    const float f = v > 127 ? 0.5f + 0.5f * avg : 0.5f * avg;
                    o = static_cast<std::uint8_t>(std::lround(f * 255.0f));
                }
            }
            out.mask[yy * bw + xx] = o;
        }
    }
    out.box = Rect{area.x0 + static_cast<int>(x0), area.y0 + static_cast<int>(y0), area.x0 + static_cast<int>(x1), area.y0 + static_cast<int>(y1)};
    return out;
}

std::vector<float> color_range(const std::vector<Rgba>& px, const std::array<float, 3>& color, float fuzziness) {
    const float f = std::max(fuzziness, 0.0f) + 1.0f;
    std::vector<float> out(px.size(), 0.0f);
    for (std::size_t i = 0; i < px.size(); ++i) {
        const Rgba& p = px[i];
        if (p[3] <= 0.0f) continue;
        float d = 0.0f;
        for (int c = 0; c < 3; ++c) d = std::max(d, std::fabs(p[c] - color[c]) * 255.0f);
        out[i] = std::clamp(1.0f - d / f, 0.0f, 1.0f);
    }
    return out;
}

std::vector<float> color_range_samples(const std::vector<Rgba>& px, std::size_t w, const std::vector<RangeSample>& samples, float fuzziness,
                                       std::optional<float> localized) {
    const float f = std::max(fuzziness, 0.0f) + 1.0f;
    const std::size_t row = std::max<std::size_t>(w, 1);
    std::vector<float> out(px.size(), 0.0f);
    for (std::size_t i = 0; i < px.size(); ++i) {
        const Rgba& p = px[i];
        if (p[3] <= 0.0f) continue;
        const float x = static_cast<float>(i % row), y = static_cast<float>(i / row);
        float best = 0.0f;
        for (const RangeSample& s : samples) {
            float d = 0.0f;
            for (int c = 0; c < 3; ++c) d = std::max(d, std::fabs(p[c] - s.color[static_cast<std::size_t>(c)]) * 255.0f);
            float k = std::clamp(1.0f - d / f, 0.0f, 1.0f);
            if (localized && s.at) {
                const float dist = std::sqrt((x - s.at->first) * (x - s.at->first) + (y - s.at->second) * (y - s.at->second));
                k *= std::clamp(1.0f - dist / std::max(*localized, 1.0f), 0.0f, 1.0f);
            }
            best = std::max(best, k);
        }
        out[i] = best;
    }
    return out;
}

std::vector<float> hue_range(const std::vector<Rgba>& px, float center) {
    std::vector<float> out(px.size(), 0.0f);
    for (std::size_t i = 0; i < px.size(); ++i) {
        const Rgba& p = px[i];
        if (p[3] <= 0.0f) continue;
        const auto [hue, chroma] = hue_chroma(std::clamp(p[0], 0.0f, 1.0f), std::clamp(p[1], 0.0f, 1.0f), std::clamp(p[2], 0.0f, 1.0f));
        float dh = rem360(hue - center);
        dh = std::min(dh, 360.0f - dh);
        out[i] = std::clamp(std::clamp(1.0f - dh / 60.0f, 0.0f, 1.0f) * chroma, 0.0f, 1.0f);
    }
    return out;
}

std::vector<float> tone_range(const std::vector<Rgba>& px, float lo, float hi, float falloff) {
    const float ramp = std::max(falloff, 1e-3f);
    std::vector<float> out(px.size(), 0.0f);
    for (std::size_t i = 0; i < px.size(); ++i) {
        const Rgba& p = px[i];
        if (p[3] <= 0.0f) continue;
        const float l = std::clamp(0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2], 0.0f, 1.0f) * 255.0f;
        const float beyond = l < lo ? lo - l : l > hi ? l - hi : 0.0f;
        out[i] = std::clamp(1.0f - beyond / ramp, 0.0f, 1.0f);
    }
    return out;
}

std::vector<float> edt(const std::vector<bool>& inside, std::size_t w, std::size_t h) {
    std::vector<float> g(inside.size());
    for (std::size_t i = 0; i < inside.size(); ++i) g[i] = inside[i] ? 0.0f : 1e20f;
    const std::size_t n = std::max<std::size_t>(std::max(w, h), 1);
    std::vector<float> f(n, 0.0f), o(n, 0.0f), z(n + 1, 0.0f);
    std::vector<std::size_t> v(n, 0);
    for (std::size_t x = 0; x < w; ++x) {
        for (std::size_t y = 0; y < h; ++y) f[y] = g[y * w + x];
        if (h > 0) dt1(f.data(), h, o.data(), v, z);
        for (std::size_t y = 0; y < h; ++y) g[y * w + x] = o[y];
    }
    for (std::size_t y = 0; y < h; ++y) {
        std::copy(g.begin() + static_cast<std::ptrdiff_t>(y * w), g.begin() + static_cast<std::ptrdiff_t>((y + 1) * w), f.begin());
        if (w > 0) dt1(f.data(), w, o.data(), v, z);
        for (std::size_t x = 0; x < w; ++x) g[y * w + x] = std::sqrt(o[x]);
    }
    return g;
}

std::vector<float> expand(const std::vector<float>& m, std::size_t w, std::size_t h, float r) {
    std::vector<bool> inside(m.size());
    for (std::size_t i = 0; i < m.size(); ++i) inside[i] = m[i] >= 0.5f;
    const std::vector<float> d = edt(inside, w, h);
    std::vector<float> out(m.size());
    // (a pixel whose centre is d from the nearest selected centre is covered when d ≤ r; a fractional r, partly)
    for (std::size_t i = 0; i < m.size(); ++i) out[i] = std::max(m[i], std::clamp(r + 1.0f - d[i], 0.0f, 1.0f));
    return out;
}

std::vector<float> contract(const std::vector<float>& m, std::size_t w, std::size_t h, float r) {
    std::vector<float> inv(m.size());
    for (std::size_t i = 0; i < m.size(); ++i) inv[i] = 1.0f - m[i];
    std::vector<float> out = expand(inv, w, h, r);
    for (float& v : out) v = 1.0f - v;
    return out;
}

std::vector<float> border(const std::vector<float>& m, std::size_t w, std::size_t h, float r) {
    const std::vector<float> outer = expand(m, w, h, r / 2.0f);
    const std::vector<float> inner = contract(m, w, h, r / 2.0f);
    std::vector<float> out(m.size());
    for (std::size_t i = 0; i < m.size(); ++i) out[i] = std::max(outer[i] - inner[i], 0.0f);
    return out;
}

std::vector<float> smooth(const std::vector<float>& m, std::size_t w, std::size_t h, float r) {
    std::vector<float> out = box_blur(m, w, h, static_cast<std::size_t>(std::lround(std::max(r, 0.0f))));
    for (float& v : out) v = std::clamp((v - 0.5f) * 4.0f + 0.5f, 0.0f, 1.0f);
    return out;
}

std::vector<float> feather(const std::vector<float>& m, std::size_t w, std::size_t h, float radius) {
    const float sigma = std::max(radius, 0.0f) / 2.0f;
    if (sigma < 0.1f) return m;
    const long long r = static_cast<long long>(std::ceil(sigma * 3.0f));
    std::vector<float> k;
    for (long long i = -r; i <= r; ++i) k.push_back(std::exp(-static_cast<float>(i * i) / (2.0f * sigma * sigma)));
    float s = 0.0f;
    for (const float v : k) s += v;
    for (float& v : k) v /= s;
    std::vector<float> tmp(m.size(), 0.0f);
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            float a = 0.0f;
            for (std::size_t i = 0; i < k.size(); ++i) {
                const long long xx = static_cast<long long>(x) + static_cast<long long>(i) - r;
                if (xx >= 0 && xx < static_cast<long long>(w)) a += m[y * w + static_cast<std::size_t>(xx)] * k[i];
            }
            tmp[y * w + x] = a;
        }
    }
    std::vector<float> out(m.size(), 0.0f);
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            float a = 0.0f;
            for (std::size_t i = 0; i < k.size(); ++i) {
                const long long yy = static_cast<long long>(y) + static_cast<long long>(i) - r;
                if (yy >= 0 && yy < static_cast<long long>(h)) a += tmp[static_cast<std::size_t>(yy) * w + x] * k[i];
            }
            out[y * w + x] = a;
        }
    }
    return out;
}

std::vector<float> polygon(const std::vector<std::pair<float, float>>& points, const Rect& area, bool anti_alias) {
    const std::size_t w = static_cast<std::size_t>(std::max(0, area.width())), h = static_cast<std::size_t>(std::max(0, area.height()));
    std::vector<float> m(w * h, 0.0f);
    if (points.size() < 3) return m;
    const int sub = anti_alias ? 4 : 1;
    std::vector<float> xs;
    for (std::size_t py = 0; py < h; ++py) {
        for (int s = 0; s < sub; ++s) {
            const float y = static_cast<float>(area.y0) + static_cast<float>(py) + (static_cast<float>(s) + 0.5f) / static_cast<float>(sub);
            xs.clear();
            for (std::size_t i = 0; i < points.size(); ++i) {
                const auto a = points[i];
                const auto b = points[(i + 1) % points.size()];
                if ((a.second <= y && b.second > y) || (b.second <= y && a.second > y)) {
                    xs.push_back(a.first + (y - a.second) / (b.second - a.second) * (b.first - a.first));
                }
            }
            std::sort(xs.begin(), xs.end());
            for (std::size_t k = 0; k + 1 < xs.size(); k += 2) {
                const float x0 = std::max(xs[k] - static_cast<float>(area.x0), 0.0f);
                const float x1 = std::min(xs[k + 1] - static_cast<float>(area.x0), static_cast<float>(w));
                if (x1 <= x0) continue;
                const std::size_t c0 = static_cast<std::size_t>(std::floor(x0));
                const std::size_t c1 = std::min(static_cast<std::size_t>(std::ceil(x1)), w);
                for (std::size_t cx = c0; cx < c1; ++cx) {
                    float cov = 0.0f;
                    if (anti_alias) {
                        cov = std::min(static_cast<float>(cx + 1), x1) - std::max(static_cast<float>(cx), x0);
                    } else {
                        const float c = static_cast<float>(cx) + 0.5f;
                        cov = c >= x0 && c < x1 ? 1.0f : 0.0f;
                    }
                    m[py * w + cx] += std::max(cov, 0.0f) / static_cast<float>(sub);
                }
            }
        }
    }
    for (float& v : m) v = std::min(v, 1.0f);
    return m;
}

std::vector<float> from_l(const Image& mask) {
    const std::string bytes = mask.mode() == "L" ? mask.tobytes() : mask.convert("L").tobytes();
    std::vector<float> out(bytes.size());
    for (std::size_t i = 0; i < bytes.size(); ++i) out[i] = static_cast<unsigned char>(bytes[i]) / 255.0f;
    return out;
}

Image to_l(const std::vector<float>& m, Size size) {
    std::string bytes(m.size(), '\0');
    for (std::size_t i = 0; i < m.size(); ++i) {
        const float v = std::isfinite(m[i]) ? std::clamp(m[i], 0.0f, 1.0f) : 0.0f;
        bytes[i] = static_cast<char>(static_cast<unsigned char>(std::lround(v * 255.0f)));
    }
    return Image::frombytes("L", size, bytes);
}

Image region_image(const Region& region, Size size) {
    std::string bytes(static_cast<std::size_t>(size.width) * static_cast<std::size_t>(size.height), '\0');
    for (int y = std::max(0, region.box.y0); y < std::min(size.height, region.box.y1); ++y) {
        for (int x = std::max(0, region.box.x0); x < std::min(size.width, region.box.x1); ++x) {
            bytes[static_cast<std::size_t>(y) * static_cast<std::size_t>(size.width) + static_cast<std::size_t>(x)] =
                static_cast<char>(region.mask[static_cast<std::size_t>(y - region.box.y0) * static_cast<std::size_t>(region.box.width()) +
                                              static_cast<std::size_t>(x - region.box.x0)]);
        }
    }
    return Image::frombytes("L", size, bytes);
}

std::optional<core::Json> to_area(const Image& mask, int dpi) { return selection::from_mask(mask, dpi); }

}  // namespace genko::render::selection_ext
