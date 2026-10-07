#include "render/vectorize.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

#include "core/ids.hpp"
#include "core/pynum.hpp"
#include "core/strokes.hpp"
#include "render/grid.hpp"
#include "render/op_limits.hpp"

namespace genko::render::vectorize {

namespace {

constexpr int kMaxDepth = 24;

using Pixel = std::pair<int, int>;  // (y, x), as numpy's nonzero gives them

// vectorize._thin: Zhang–Suen thinning (each half-step decided on the picture before it, as numpy does it)
BoolGrid thin(const BoolGrid& on) {
    const int w = on.width;
    const int h = on.height;
    // padded by one pixel of zeros
    std::vector<unsigned char> img(static_cast<std::size_t>(w + 2) * static_cast<std::size_t>(h + 2), 0);
    const auto at = [&](int x, int y) -> unsigned char& {
        return img[static_cast<std::size_t>(y) * static_cast<std::size_t>(w + 2) + static_cast<std::size_t>(x)];
    };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) at(x + 1, y + 1) = on.at(x, y);
    }
    std::vector<std::size_t> gone;
    for (;;) {
        bool changed = false;
        for (int step = 0; step < 2; ++step) {
            gone.clear();
            for (int y = 1; y <= h; ++y) {
                for (int x = 1; x <= w; ++x) {
                    if (at(x, y) != 1) continue;
                    const int p2 = at(x, y - 1), p3 = at(x + 1, y - 1), p4 = at(x + 1, y), p5 = at(x + 1, y + 1);
                    const int p6 = at(x, y + 1), p7 = at(x - 1, y + 1), p8 = at(x - 1, y), p9 = at(x - 1, y - 1);
                    const int ring[9] = {p2, p3, p4, p5, p6, p7, p8, p9, p2};
                    int b = 0;
                    for (int k = 0; k < 8; ++k) b += ring[k];
                    int a = 0;
                    for (int k = 0; k < 8; ++k) a += (ring[k] == 0 && ring[k + 1] == 1) ? 1 : 0;
                    const int c1 = step == 0 ? p2 * p4 * p6 : p2 * p4 * p8;
                    const int c2 = step == 0 ? p4 * p6 * p8 : p2 * p6 * p8;
                    if (b >= 2 && b <= 6 && a == 1 && c1 == 0 && c2 == 0) {
                        gone.push_back(static_cast<std::size_t>(y) * static_cast<std::size_t>(w + 2) + static_cast<std::size_t>(x));
                    }
                }
            }
            if (!gone.empty()) {
                for (const std::size_t i : gone) img[i] = 0;
                changed = true;
            }
        }
        if (!changed) break;
    }
    BoolGrid out(w, h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) out.at(x, y) = at(x + 1, y + 1);
    }
    return out;
}

// vectorize._depth: how deep inside the marks each pixel is (erosion steps, capped)
std::vector<float> depth_of(const Image& alpha) {
    std::vector<float> depth(static_cast<std::size_t>(alpha.width()) * static_cast<std::size_t>(alpha.height()), 0.0f);
    Image current = alpha;
    for (int k = 0; k < kMaxDepth; ++k) {
        const std::string values = current.tobytes();
        bool any = false;
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (static_cast<unsigned char>(values[i]) > 127) {
                depth[i] += 1.0f;
                any = true;
            }
        }
        if (!any) break;
        current = current.filter(Filter::min_filter(3));
    }
    return depth;
}

// CPython 3.12's hash((y, x)) for ints 0 <= y, x (hash(n) is n for them)
std::uint64_t tuple_hash(std::int64_t a, std::int64_t b) {
    constexpr std::uint64_t kPrime1 = 11400714785074694791ULL;
    constexpr std::uint64_t kPrime2 = 14029467366897019727ULL;
    constexpr std::uint64_t kPrime5 = 2870177450012600261ULL;
    std::uint64_t acc = kPrime5;
    for (const std::int64_t lane : {a, b}) {
        acc += static_cast<std::uint64_t>(lane) * kPrime2;
        acc = (acc << 31) | (acc >> 33);
        acc *= kPrime1;
    }
    acc += 2 ^ (kPrime5 ^ 3527539ULL);
    if (acc == ~std::uint64_t{0}) return 1546275796;
    return acc;
}

// The order a CPython 3.12 set made from `keys` (in this order, all different) holds them: its table's slots, as
// set_add_entry, set_table_resize and set_insert_clean fill them.
std::vector<std::size_t> set_order(const std::vector<Pixel>& keys) {
    constexpr std::size_t kLinearProbes = 9;
    constexpr int kPerturbShift = 5;
    struct Slot {
        std::uint64_t hash = 0;
        std::int64_t key = -1;
    };
    std::vector<Slot> table(8);
    std::size_t fill = 0;
    std::size_t used = 0;
    const auto insert_clean = [&](std::vector<Slot>& into, std::uint64_t hash, std::int64_t key) {
        const std::size_t mask = into.size() - 1;
        std::uint64_t perturb = hash;
        std::size_t i = static_cast<std::size_t>(hash) & mask;
        for (;;) {
            if (into[i].key < 0) {
                into[i] = Slot{hash, key};
                return;
            }
            if (i + kLinearProbes <= mask) {
                for (std::size_t j = 1; j <= kLinearProbes; ++j) {
                    if (into[i + j].key < 0) {
                        into[i + j] = Slot{hash, key};
                        return;
                    }
                }
            }
            perturb >>= kPerturbShift;
            i = static_cast<std::size_t>(i * 5 + 1 + perturb) & mask;
        }
    };
    const auto resize = [&](std::size_t minused) {
        std::size_t size = 8;
        while (size <= minused) size <<= 1;
        std::vector<Slot> grown(size);
        for (const Slot& slot : table) {
            if (slot.key >= 0) insert_clean(grown, slot.hash, slot.key);
        }
        table = std::move(grown);
        fill = used;
    };
    for (std::size_t k = 0; k < keys.size(); ++k) {
        const std::uint64_t hash = tuple_hash(keys[k].first, keys[k].second);
        const std::size_t mask = table.size() - 1;
        std::uint64_t perturb = hash;
        std::size_t i = static_cast<std::size_t>(hash) & mask;
        bool placed = false;
        while (!placed) {
            const std::size_t probes = (i + kLinearProbes <= mask) ? kLinearProbes : 0;
            for (std::size_t j = 0; j <= probes; ++j) {
                Slot& slot = table[i + j];
                if (slot.key < 0) {
                    slot = Slot{hash, static_cast<std::int64_t>(k)};
                    ++fill;
                    ++used;
                    if (fill * 5 >= mask * 3) resize(used > 50000 ? used * 2 : used * 4);
                    placed = true;
                    break;
                }
            }
            if (placed) break;
            perturb >>= kPerturbShift;
            i = static_cast<std::size_t>(i * 5 + 1 + perturb) & mask;
        }
    }
    std::vector<std::size_t> out;
    out.reserve(used);
    for (const Slot& slot : table) {
        if (slot.key >= 0) out.push_back(static_cast<std::size_t>(slot.key));
    }
    return out;
}

// vectorize._chains: the skeleton's pixels followed into chains, from the ends first, then the loops
std::vector<std::vector<Pixel>> chains_of(const BoolGrid& skeleton) {
    std::vector<Pixel> pixels;
    for (int y = 0; y < skeleton.height; ++y) {
        for (int x = 0; x < skeleton.width; ++x) {
            if (skeleton.at(x, y) != 0) pixels.emplace_back(y, x);
        }
    }
    BoolGrid left = skeleton;
    static constexpr int kSteps[8][2] = {{-1, 0}, {0, 1}, {1, 0}, {0, -1}, {-1, 1}, {1, 1}, {1, -1}, {-1, -1}};
    const auto near = [&](const Pixel& p) {
        std::vector<Pixel> out;
        for (const auto& s : kSteps) {
            const int y = p.first + s[0];
            const int x = p.second + s[1];
            if (y >= 0 && x >= 0 && y < left.height && x < left.width && left.at(x, y) != 0) out.emplace_back(y, x);
        }
        return out;
    };
    std::vector<Pixel> starts;
    for (const std::size_t k : set_order(pixels)) {
        if (near(pixels[k]).size() == 1) starts.push_back(pixels[k]);
    }
    std::vector<Pixel> sorted = pixels;  // (row-major is already sorted)
    starts.insert(starts.end(), sorted.begin(), sorted.end());
    std::vector<std::vector<Pixel>> out;
    for (const Pixel& start : starts) {
        if (left.at(start.second, start.first) == 0) continue;
        std::vector<Pixel> chain{start};
        left.at(start.second, start.first) = 0;
        Pixel p = start;
        for (;;) {
            const std::vector<Pixel> ahead = near(p);
            if (ahead.empty()) break;
            p = ahead.front();
            left.at(p.second, p.first) = 0;
            chain.push_back(p);
        }
        out.push_back(std::move(chain));
    }
    return out;
}

// vectorize._simplify (Ramer–Douglas–Peucker; the first farthest point splits)
std::vector<core::PointF> simplify(const std::vector<core::PointF>& points, double epsilon) {
    if (points.size() < 3) return points;
    std::vector<bool> keep(points.size(), false);
    keep.front() = keep.back() = true;
    std::vector<std::pair<std::size_t, std::size_t>> todo{{0, points.size() - 1}};
    while (!todo.empty()) {
        const auto [first, last] = todo.back();
        todo.pop_back();
        if (last - first + 1 < 3) continue;
        const double x0 = points[first].x, y0 = points[first].y;
        const double x1 = points[last].x, y1 = points[last].y;
        const double length = core::py_hypot(x1 - x0, y1 - y0);
        std::size_t far = first;
        double far_d = -1.0;
        for (std::size_t k = first + 1; k < last; ++k) {
            const double px = points[k].x, py = points[k].y;
            const double d = length == 0 ? core::py_hypot(px - x0, py - y0)
                                         : std::fabs((x1 - x0) * (y0 - py) - (x0 - px) * (y1 - y0)) / length;
            if (d > far_d) {
                far = k;
                far_d = d;
            }
        }
        if (far_d <= epsilon) continue;
        keep[far] = true;
        todo.emplace_back(far, last);
        todo.emplace_back(first, far);
    }
    std::vector<core::PointF> out;
    for (std::size_t k = 0; k < points.size(); ++k) {
        if (keep[k]) out.push_back(points[k]);
    }
    return out;
}

}  // namespace

std::vector<core::Stroke> trace_layer(const Image& picture, int dpi, double min_mm, const core::ColorRasterView* precise) {
    if (static_cast<std::int64_t>(picture.width()) * picture.height() == 0) return {};
    const double factor = static_cast<double>(kTraceDpi) / dpi;
    Image small = picture.mode() == "RGBA" ? picture : picture.convert("RGBA");
    if (factor < 1) {
        const int w = std::max(1, static_cast<int>(core::py_round_int(picture.width() * factor)));
        const int h = std::max(1, static_cast<int>(core::py_round_int(picture.height() * factor)));
        small = small.resize(Size{w, h}, Resample::Bilinear);
    }
    const double scale = (factor < 1 ? kTraceDpi : dpi) / 25.4;
    const auto marks = [](int v) { return v > 90 ? 255 : 0; };
    const auto box = small.getchannel(3).point(marks).getbbox();
    if (!box) return {};
    const int x0 = std::max(0, box->x0 - 2);
    const int y0 = std::max(0, box->y0 - 2);
    const Image crop = small.crop(Box{x0, y0, std::min(small.width(), box->x1 + 2), std::min(small.height(), box->y1 + 2)});
    const Image alpha = crop.getchannel(3).point(marks);
    const std::vector<float> depth = depth_of(alpha);
    const BoolGrid skeleton = thin(compare(alpha, 127, true));
    const std::string colours = crop.convert("RGB").tobytes();
    const int cw = crop.width();
    std::vector<core::Stroke> strokes;
    for (const std::vector<Pixel>& chain : chains_of(skeleton)) {
        const double n = static_cast<double>(chain.size());
        if (n * 1.0 / scale < min_mm && chain.size() < 3) continue;
        std::vector<core::PointF> pts;
        pts.reserve(chain.size());
        for (const auto& [y, x] : chain) pts.push_back(core::PointF{(x + x0 + 0.5) / scale, (y + y0 + 0.5) / scale});
        std::vector<double> steps;
        for (std::size_t i = 0; i + 1 < pts.size(); ++i) steps.push_back(core::py_dist(pts[i].x, pts[i].y, pts[i + 1].x, pts[i + 1].y));
        if (core::py_float_sum(steps) < min_mm) continue;
        // np.median(depth[ys, xs]) in float32, then float()
        std::vector<float> depths;
        depths.reserve(chain.size());
        for (const auto& [y, x] : chain) depths.push_back(depth[static_cast<std::size_t>(y) * static_cast<std::size_t>(cw) + static_cast<std::size_t>(x)]);
        std::sort(depths.begin(), depths.end());
        const std::size_t half = depths.size() / 2;
        const float median = depths.size() % 2 == 1 ? depths[half] : (depths[half - 1] + depths[half]) / 2.0f;
        const double width = core::py_max(0.1, static_cast<double>(median) * 2 / scale);
        // colours[ys, xs].mean(axis=0).round(): float32 sums, divided by the count (an intp: in float64, kept in
        // float32), rounded half to even
        std::vector<std::int64_t> rgb;
        for (int c = 0; c < 3; ++c) {
            float sum = 0.0f;
            for (const auto& [y, x] : chain) {
                sum += static_cast<float>(static_cast<unsigned char>(
                    colours[(static_cast<std::size_t>(y) * static_cast<std::size_t>(cw) + static_cast<std::size_t>(x)) * 3 + static_cast<std::size_t>(c)]));
            }
            const auto mean = static_cast<float>(static_cast<double>(sum) / static_cast<double>(chain.size()));
            rgb.push_back(static_cast<std::int64_t>(std::nearbyint(mean)));
        }
        const std::vector<core::PointF> simple = simplify(pts, 0.4 / scale);
        core::Stroke stroke;
        stroke.id = core::new_id();
        for (const core::PointF& p : simple) stroke.points.push_back(core::PointF{core::py_round(p.x, 3), core::py_round(p.y, 3)});
        stroke.pressure.assign(simple.size(), 1.0);
        stroke.width_mm = core::py_round(width, 3);
        stroke.kind = "mili";
        if (precise) {
            std::array<double, 4> mean{};
            for (const auto& [y, x] : chain) {
                const auto sx = std::min(precise->width()-1, static_cast<std::uint32_t>((x+x0+.5)*precise->width()/small.width()));
                const auto sy = std::min(precise->height()-1, static_cast<std::uint32_t>((y+y0+.5)*precise->height()/small.height()));
                const auto p = precise->pixel(std::size_t(sy)*precise->width()+sx);
                for (unsigned c=0;c<4;++c) mean[c] += p[c]/chain.size();
            }
            core::Json values = core::Json::array();
            const auto precision = precise->metadata("")["precision"];
            for (unsigned c=0;c<3;++c) values.push_back(precision == "u16" ? std::nearbyint(mean[c]*65535)/65535 : mean[c]);
            stroke.color_rgb = core::Json{{"precision", precision}, {"values", std::move(values)}};
            core::validate_stroke_color(*stroke.color_rgb);
            stroke.opacity = std::clamp(mean[3],0.,1.);
        } else stroke.rgb = rgb;
        strokes.push_back(std::move(stroke));
    }
    return strokes;
}

}  // namespace genko::render::vectorize
