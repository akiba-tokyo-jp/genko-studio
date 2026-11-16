// guides/figures.png and guides/regions.png (SPEC FIGURE-03): see figure_guide.hpp.

#include "service/figure_guide.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>

#include "core/linalg3.hpp"
#include "core/mesh3d.hpp"
#include "core/prim3d.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "service/blocking.hpp"

namespace genko::service::figure_guide {

namespace {

namespace mesh3d = core::mesh3d;
using core::la::Vec3;

constexpr int kSamples = 3;     // samples a pixel, each way
constexpr int kBandRows = 32;   // pixel rows drawn at a time (the samples of one band in memory)

// The palette: the material colours (and the regions' colours), in the order people get them.
constexpr std::array<std::array<int, 3>, 12> kPalette{{
    {236, 204, 150},  // warm beige
    {172, 174, 236},  // lavender
    {172, 170, 40},   // olive
    {30, 150, 180},   // teal
    {240, 150, 130},  // salmon
    {120, 200, 110},  // green
    {210, 140, 210},  // orchid
    {240, 180, 60},   // amber
    {110, 150, 240},  // cornflower
    {200, 110, 80},   // terracotta
    {130, 220, 190},  // mint
    {230, 120, 160},  // rose
}};

// The light (as the page has it: page.extra.light {dir, ambient}); the usual one without.
struct Light {
    Vec3 key{};     // toward the key light
    Vec3 fill{};    // toward the fill (the key's mirror, from a little below)
    Vec3 half{};    // between the key and the eye (the highlight)
    double ambient = 0.35;
};

Vec3 unit(const Vec3& v) {
    const double n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    return n > 0.0 ? Vec3{v[0] / n, v[1] / n, v[2] / n} : Vec3{0.0, 0.0, -1.0};
}

Light light_of(const core::Page& page) {
    Vec3 dir{-0.5, -0.7, -0.6};
    double ambient = 0.35;
    if (const Json* light = core::get(page.extra, "light"); light != nullptr && light->is_object()) {
        if (const Json* d = core::get(*light, "dir"); d != nullptr && d->is_array() && d->size() == 3 &&
                                                       std::all_of(d->begin(), d->end(), [](const Json& v) { return v.is_number(); })) {
            const Vec3 given{core::to_float((*d)[0]), core::to_float((*d)[1]), core::to_float((*d)[2])};
            if (std::isfinite(given[0]) && std::isfinite(given[1]) && std::isfinite(given[2]) &&
                given[0] * given[0] + given[1] * given[1] + given[2] * given[2] > 0.0) {
                dir = given;
            }
        }
        if (const Json* a = core::get(*light, "ambient"); a != nullptr && a->is_number()) {
            const double v = core::to_float(*a);
            if (std::isfinite(v)) ambient = std::max(0.0, std::min(1.0, v));
        }
    }
    Light out;
    out.key = unit(dir);
    out.fill = unit(Vec3{-out.key[0], 0.35, out.key[2] - 0.4});
    out.half = unit(Vec3{out.key[0], out.key[1], out.key[2] - 1.0});  // (the eye is toward −z)
    out.ambient = ambient;
    return out;
}

// One prim on the samples' grid: its points (sample units), depth and seen normals, and its triangles.
struct Seen {
    std::vector<double> sx;
    std::vector<double> sy;
    std::vector<double> depth;
    std::vector<Vec3> n;
    std::shared_ptr<const mesh3d::Model> model;
    std::array<double, 3> albedo{};
};

std::vector<Seen> seen_all(const core::Page& page, std::span<const Json> prims, const View& view) {
    const Json* page_camera = core::get(page.extra, "camera");
    const double kx = view.px[0] * kSamples / view.width;
    const double ky = view.px[1] * kSamples / view.height;
    std::vector<Seen> out;
    for (std::size_t i = 0; i < prims.size(); ++i) {
        const Json& prim = prims[i];
        const Json* camera = mesh3d::camera_of(prim, page_camera);
        Seen s;
        s.model = mesh3d::model_of(prim);
        const std::array<int, 3> c = colour(i);
        s.albedo = {c[0] / 255.0, c[1] / 255.0, c[2] / 255.0};
        if (!s.model->smooth()) {  // (figures, heads and hands are smooth; nothing else is drawn here)
            out.push_back(std::move(s));
            continue;
        }
        const mesh3d::OnPage at = mesh3d::to_page(prim, s.model->mesh.v, camera);
        s.n = mesh3d::seen_normals(prim, *s.model, camera);
        s.sx.resize(at.pts.size());
        s.sy.resize(at.pts.size());
        for (std::size_t k = 0; k < at.pts.size(); ++k) {
            s.sx[k] = (at.pts[k][0] - view.x) * kx;
            s.sy[k] = (at.pts[k][1] - view.y) * ky;
        }
        s.depth = at.depth;
        out.push_back(std::move(s));
    }
    return out;
}

double clamp01(double v) { return v > 0.0 ? (v < 1.0 ? v : 1.0) : 0.0; }

// The colour of a sample: the material colour lit by the key light (its terminator soft: half the light wraps past
// it), a fill from the other side and what the page's ambient gives, darker where the surface turns away from the eye
// (so each form's round reads, in the light and in the shade), and over it a broad soft sheen and a small bright
// highlight, both a little of the material's own colour. Only + − × ÷ and the square root.
std::array<int, 3> shade(const Vec3& normal, const std::array<double, 3>& albedo, const Light& light) {
    const Vec3 n = unit(normal);
    const double nl = n[0] * light.key[0] + n[1] * light.key[1] + n[2] * light.key[2];
    const double key = std::max(0.0, nl);
    const double half = 0.5 * nl + 0.5;  // (1 toward the light, 0 away from it)
    const double fill = std::max(0.0, n[0] * light.fill[0] + n[1] * light.fill[1] + n[2] * light.fill[2]);
    const double facing = std::max(0.0, -n[2]);  // (the eye is toward −z)
    const double h = std::max(0.0, n[0] * light.half[0] + n[1] * light.half[1] + n[2] * light.half[2]);
    const double h2 = h * h;
    const double h4 = h2 * h2;
    const double h8 = h4 * h4;
    const double h32 = h8 * h8 * h8 * h8;
    const double lit = 0.55 * key + 0.45 * half * half;
    const double round = 0.55 + 0.45 * std::sqrt(facing);
    const double diffuse = (0.04 + 0.28 * light.ambient + 0.86 * lit + 0.1 * fill) * round;
    const double sheen = 0.36 * h8 * h4;
    const double spark = 0.22 * h32 * h32;
    std::array<int, 3> out{};
    for (int c = 0; c < 3; ++c) {
        const double a = albedo[static_cast<std::size_t>(c)];
        double v = a * diffuse + (sheen + spark) * (0.55 + 0.45 * a);
        // (the brightest quarter is pressed together, never cut off at white: the form reads in the highlights too)
        if (v > 0.72) {
            const double over = (v - 0.72) / 0.28;
            v = 0.72 + 0.28 * over / (1.0 + over);
        }
        out[static_cast<std::size_t>(c)] = static_cast<int>(std::floor(clamp01(v) * 255.0 + 0.5));
    }
    return out;
}

struct Drawn {
    std::string rgb;          // px[0] × px[1] × 3
    std::vector<int> owner;   // px[0] × px[1]: the prim seen at the pixel's middle (else at its first covered sample), −1
};

// The prims drawn: each sample the nearest surface's colour (and whose it is); a pixel the mean of its samples.
Drawn draw(const core::Page& page, std::span<const Json> prims, const View& view) {
    const int width = view.px[0];
    const int height = view.px[1];
    Drawn out;
    out.rgb.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3, '\0');
    out.owner.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), -1);
    if (width <= 0 || height <= 0 || !(view.width > 0.0) || !(view.height > 0.0) || prims.empty()) return out;
    const Light light = light_of(page);
    const std::vector<Seen> all = seen_all(page, prims, view);
    const int sw = width * kSamples;
    const int band_rows = kBandRows * kSamples;
    const std::size_t band_size = static_cast<std::size_t>(sw) * static_cast<std::size_t>(band_rows);
    std::vector<double> zbuf(band_size);
    std::vector<std::array<std::uint8_t, 3>> colour_at(band_size);
    std::vector<int> who(band_size);
    for (int band = 0; band * kBandRows < height; ++band) {
        const int y0 = band * band_rows;                          // (in samples)
        const int y1 = std::min(height * kSamples, y0 + band_rows);  // (outside)
        std::fill(zbuf.begin(), zbuf.end(), std::numeric_limits<double>::infinity());
        std::fill(colour_at.begin(), colour_at.end(), std::array<std::uint8_t, 3>{0, 0, 0});
        std::fill(who.begin(), who.end(), -1);
        for (std::size_t p = 0; p < all.size(); ++p) {
            const Seen& s = all[p];
            if (s.sx.empty()) continue;
            for (const auto& t : s.model->tris) {
                const auto a = static_cast<std::size_t>(t[0]);
                const auto b = static_cast<std::size_t>(t[1]);
                const auto c = static_cast<std::size_t>(t[2]);
                const double ax = s.sx[a], ay = s.sy[a], bx = s.sx[b], by = s.sy[b], cx = s.sx[c], cy = s.sy[c];
                const double min_y = std::min(ay, std::min(by, cy));
                const double max_y = std::max(ay, std::max(by, cy));
                if (!(max_y >= y0 - 1) || !(min_y <= y1 + 1)) continue;
                const double min_x = std::min(ax, std::min(bx, cx));
                const double max_x = std::max(ax, std::max(bx, cx));
                if (!(max_x >= -1) || !(min_x <= sw + 1)) continue;
                const double den = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);
                if (!(std::fabs(den) >= 1e-12)) continue;
                const int ix0 = std::max(0, static_cast<int>(std::floor(min_x)));
                const int ix1 = std::min(sw - 1, static_cast<int>(std::ceil(max_x)));
                const int iy0 = std::max(y0, static_cast<int>(std::floor(min_y)));
                const int iy1 = std::min(y1 - 1, static_cast<int>(std::ceil(max_y)));
                for (int yi = iy0; yi <= iy1; ++yi) {
                    const double gy = static_cast<double>(yi) + 0.5;
                    for (int xi = ix0; xi <= ix1; ++xi) {
                        const double gx = static_cast<double>(xi) + 0.5;
                        const double l1 = ((by - cy) * (gx - cx) + (cx - bx) * (gy - cy)) / den;
                        const double l2 = ((cy - ay) * (gx - cx) + (ax - cx) * (gy - cy)) / den;
                        const double l3 = 1.0 - l1 - l2;
                        if (!(l1 >= -1e-9 && l2 >= -1e-9 && l3 >= -1e-9)) continue;
                        const double depth = l1 * s.depth[a] + l2 * s.depth[b] + l3 * s.depth[c];
                        const std::size_t at = static_cast<std::size_t>(yi - y0) * static_cast<std::size_t>(sw) + static_cast<std::size_t>(xi);
                        if (!(depth < zbuf[at])) continue;
                        zbuf[at] = depth;
                        const Vec3 n{l1 * s.n[a][0] + l2 * s.n[b][0] + l3 * s.n[c][0], l1 * s.n[a][1] + l2 * s.n[b][1] + l3 * s.n[c][1],
                                     l1 * s.n[a][2] + l2 * s.n[b][2] + l3 * s.n[c][2]};
                        const std::array<int, 3> rgb = shade(n, s.albedo, light);
                        colour_at[at] = {static_cast<std::uint8_t>(rgb[0]), static_cast<std::uint8_t>(rgb[1]), static_cast<std::uint8_t>(rgb[2])};
                        who[at] = static_cast<int>(p);
                    }
                }
            }
        }
        // the band's pixels: the mean of their samples (rounded), and whose
        for (int py = y0 / kSamples; py * kSamples < y1; ++py) {
            for (int px = 0; px < width; ++px) {
                int sum[3] = {0, 0, 0};
                int first = -1;
                for (int j = 0; j < kSamples; ++j) {
                    for (int i = 0; i < kSamples; ++i) {
                        const std::size_t at = static_cast<std::size_t>(py * kSamples + j - y0) * static_cast<std::size_t>(sw) +
                                               static_cast<std::size_t>(px * kSamples + i);
                        for (int c = 0; c < 3; ++c) sum[c] += colour_at[at][static_cast<std::size_t>(c)];
                        if (first < 0) first = who[at];
                    }
                }
                const std::size_t pixel = static_cast<std::size_t>(py) * static_cast<std::size_t>(width) + static_cast<std::size_t>(px);
                constexpr int n = kSamples * kSamples;
                for (int c = 0; c < 3; ++c) out.rgb[pixel * 3 + static_cast<std::size_t>(c)] = static_cast<char>((sum[c] + n / 2) / n);
                const std::size_t middle = static_cast<std::size_t>(py * kSamples + kSamples / 2 - y0) * static_cast<std::size_t>(sw) +
                                           static_cast<std::size_t>(px * kSamples + kSamples / 2);
                out.owner[pixel] = who[middle] >= 0 ? who[middle] : first;
            }
        }
    }
    return out;
}

// One step of the owners' growth into the pixels nobody has (4 and 8 neighbours by turns: a roughly round growth; the
// first person wins a tie). False when nothing changed.
bool grow_step(std::vector<int>& owner, std::vector<int>& next, int width, int height, int step) {
    const bool diagonal = step % 2 == 1;
    bool changed = false;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t at = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
            next[at] = owner[at];
            if (owner[at] >= 0) continue;
            int best = -1;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if ((dx == 0 && dy == 0) || (!diagonal && dx != 0 && dy != 0)) continue;
                    const int nx = x + dx;
                    const int ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
                    const int o = owner[static_cast<std::size_t>(ny) * static_cast<std::size_t>(width) + static_cast<std::size_t>(nx)];
                    if (o >= 0 && (best < 0 || o < best)) best = o;
                }
            }
            if (best >= 0) {
                next[at] = best;
                changed = true;
            }
        }
    }
    owner.swap(next);
    return changed;
}

// Each person's silhouette made one clean shape: grown by `by` px and taken back by as much, so the slits and
// notches narrower than twice that — between an arm and the body it hangs beside, where a hand meets a hip — are the
// person's own (what was the person's stays; nobody's pixel taken back is nobody's again, and the picture's edge
// counts as the person's, so a figure cut by it is not eaten).
void close_gaps(std::vector<int>& owner, int width, int height, int by) {
    if (by <= 0) return;
    const std::vector<int> first = owner;
    std::vector<int> next = owner;
    int steps = 0;
    for (; steps < by; ++steps) {
        if (!grow_step(owner, next, width, height, steps)) break;
    }
    for (int step = steps - 1; step >= 0; --step) {  // (taken back by the same steps)
        const bool diagonal = step % 2 == 1;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const std::size_t at = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
                next[at] = owner[at];
                if (owner[at] < 0 || first[at] >= 0) continue;  // (nobody's, or the person's from the start)
                bool keep = true;
                for (int dy = -1; dy <= 1 && keep; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        if ((dx == 0 && dy == 0) || (!diagonal && dx != 0 && dy != 0)) continue;
                        const int nx = x + dx;
                        const int ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
                        if (owner[static_cast<std::size_t>(ny) * static_cast<std::size_t>(width) + static_cast<std::size_t>(nx)] != owner[at]) {
                            keep = false;
                            break;
                        }
                    }
                }
                if (!keep) next[at] = -1;
            }
        }
        owner.swap(next);
    }
}

// Owners closed (close_gaps) and grown by `grow` px into the pixels nobody has, then painted.
render::Image paint(std::vector<int> owner, const std::array<int, 2>& px, int grow) {
    const int width = px[0];
    const int height = px[1];
    close_gaps(owner, width, height, grow);
    std::vector<int> next = owner;
    for (int step = 0; step < grow; ++step) {
        if (!grow_step(owner, next, width, height, step)) break;
    }
    std::string rgb(static_cast<std::size_t>(std::max(0, width)) * static_cast<std::size_t>(std::max(0, height)) * 3, '\0');
    for (std::size_t i = 0; i < owner.size(); ++i) {
        if (owner[i] < 0) continue;
        const std::array<int, 3> c = colour(static_cast<std::size_t>(owner[i]));
        for (std::size_t k = 0; k < 3; ++k) rgb[i * 3 + k] = static_cast<char>(c[k]);
    }
    return render::Image::frombytes("RGB", render::Size{width, height}, rgb);
}

}  // namespace

View view_of(const Box& box, std::array<int, 2> px, bool cover) {
    View v{box[0], box[1], box[2], box[3], px};
    if (!cover || px[0] <= 0 || px[1] <= 0 || !(box[2] > 0.0) || !(box[3] > 0.0)) return v;
    const double aspect = static_cast<double>(px[0]) / px[1];
    if (box[2] / box[3] < aspect) {  // (the image is wider: the box grows to the left and right)
        v.width = box[3] * aspect;
        v.x = box[0] - (v.width - box[2]) / 2;
    } else {
        v.height = box[2] / aspect;
        v.y = box[1] - (v.height - box[3]) / 2;
    }
    return v;
}

std::vector<Json> panel_prims(const core::Page& page, const core::Frame& frame) {
    std::vector<Json> out;
    if (!page.prims.is_array()) return out;
    const Json* page_camera = core::get(page.extra, "camera");
    const double fx = frame.rect.x.value();
    const double fy = frame.rect.y.value();
    const double fw = frame.rect.width.value();
    const double fh = frame.rect.height.value();
    for (const Json& prim : page.prims) {
        if (!prim.is_object()) continue;
        const Json* kind = core::get(prim, "kind");
        if (kind == nullptr || !kind->is_string()) continue;
        const std::string k = kind->get<std::string>();
        if (k != "figure" && k != "head" && k != "hand") continue;
        const Json* frame_id = core::get(prim, "frame_id");
        if (frame_id != nullptr && core::py_truthy(*frame_id)) {
            if (frame_id->is_string() && frame_id->get<std::string>() == frame.id) out.push_back(prim);
            continue;
        }
        const auto [x, y, w, h] = core::prim3d::prim_bbox(prim, mesh3d::camera_of(prim, page_camera));
        const double cx = x + w / 2;
        const double cy = y + h / 2;
        if (cx >= fx && cx <= fx + fw && cy >= fy && cy <= fy + fh) out.push_back(prim);
    }
    return out;
}

std::vector<Place> cast_places(const Json& panel, const std::array<core::Num, 4>& rect) {
    std::vector<Place> out;
    for (const Figure& f : service::figures(panel, rect)) {
        const double x0 = std::min(f.head[0].value(), f.body[0].value());
        const double y0 = std::min(f.head[1].value(), f.body[1].value());
        const double x1 = std::max(f.head[0].value() + f.head[2].value(), f.body[0].value() + f.body[2].value());
        const double y1 = std::max(f.head[1].value() + f.head[3].value(), f.body[1].value() + f.body[3].value());
        out.push_back(Place{{x0, y0, x1 - x0, y1 - y0}, f.head[0].value() + f.head[2].value() / 2});
    }
    return out;
}

std::array<int, 3> colour(std::size_t i) { return kPalette[i % kPalette.size()]; }

render::Image figures(const core::Page& page, std::span<const Json> prims, const View& view) {
    const Drawn d = draw(page, prims, view);
    return render::Image::frombytes("RGB", render::Size{view.px[0], view.px[1]}, d.rgb);
}

render::Image regions(const core::Page& page, std::span<const Json> prims, const View& view, int grow) {
    if (grow < 0) grow = std::max(2, static_cast<int>(std::lround(std::max(view.px[0], view.px[1]) / 100.0)));
    Drawn d = draw(page, prims, view);
    return paint(std::move(d.owner), view.px, grow);
}

render::Image place_regions(std::span<const Place> places, const View& view) {
    const int width = view.px[0];
    const int height = view.px[1];
    std::vector<int> owner(static_cast<std::size_t>(std::max(0, width)) * static_cast<std::size_t>(std::max(0, height)), -1);
    if (view.width > 0.0 && view.height > 0.0) {
        for (int y = 0; y < height; ++y) {
            const double my = view.y + (y + 0.5) * view.height / height;
            for (int x = 0; x < width; ++x) {
                const double mx = view.x + (x + 0.5) * view.width / width;
                int best = -1;
                double near = 0.0;
                for (std::size_t i = 0; i < places.size(); ++i) {  // (where places overlap, the head nearest across)
                    const Box& b = places[i].box;
                    if (!(mx >= b[0] && mx < b[0] + b[2] && my >= b[1] && my < b[1] + b[3])) continue;
                    const double d = std::fabs(mx - places[i].middle);
                    if (best < 0 || d < near) {
                        best = static_cast<int>(i);
                        near = d;
                    }
                }
                owner[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] = best;
            }
        }
    }
    return paint(std::move(owner), view.px, 0);
}

}  // namespace genko::service::figure_guide
