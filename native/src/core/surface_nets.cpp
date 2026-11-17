// Surface Nets (SPEC FIGURE-01): see surface_nets.hpp.

#include "core/surface_nets.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "core/error.hpp"

namespace genko::core::surface_nets {

namespace {

using figure_shape::Field;
using figure_shape::Smooth;
using la::Vec3;

constexpr int kCells = 8;            // cells along a brick's side
constexpr int kSide = kCells + 1;    // samples along it
constexpr double kLipschitz = 1.3;   // (how fast the field may change: its parts at 1, the joints' falling widths a little more)
constexpr double kHalfDiagonal = 4.0 * 1.7320508075688772;  // (a brick's half diagonal, in steps)
constexpr std::int64_t kMaxBricks = 400000;

int floor_div(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

struct Brick {
    int bx = 0;
    int by = 0;
    int bz = 0;
    std::vector<std::uint8_t> use;
    std::vector<double> value;          // kSide³: (z × kSide + y) × kSide + x
    std::vector<std::int32_t> vertex;   // kCells³: the cell's point, or −1
};

// The twelve edges of a cell, as pairs of its corners (corner c = x + 2 y + 4 z).
constexpr int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};

}  // namespace

Smooth mesh(const Field& field, double h, double normal_steps) {
    Smooth out;
    const std::size_t parts = field.parts.size();
    if (parts == 0 || !(h > 0.0) || !std::isfinite(h)) return out;
    Vec3 lo{};
    Vec3 hi{};
    field.bounds(lo, hi);
    for (int a = 0; a < 3; ++a) {
        if (!std::isfinite(lo[a]) || !std::isfinite(hi[a])) return out;
    }
    // the samples (i + ½) h covering the box and two steps more; the bricks holding them
    int b0[3];
    int b1[3];
    for (int a = 0; a < 3; ++a) {
        const double first = std::floor(lo[a] / h - 0.5) - 2.0;
        const double last = std::ceil(hi[a] / h - 0.5) + 2.0;
        if (!(first > -1e8 && last < 1e8)) throw Error("image_too_large", "the 3D figure is too large to mesh");
        b0[a] = floor_div(static_cast<int>(first), kCells);
        b1[a] = floor_div(static_cast<int>(last), kCells);
    }
    const int nbx = b1[0] - b0[0] + 1;
    const int nby = b1[1] - b0[1] + 1;
    const int nbz = b1[2] - b0[2] + 1;
    if (static_cast<std::int64_t>(nbx) * nby * nbz > kMaxBricks) throw Error("image_too_large", "the 3D figure is too large to mesh");
    std::vector<std::int32_t> index(static_cast<std::size_t>(nbx) * static_cast<std::size_t>(nby) * static_cast<std::size_t>(nbz), -1);
    const auto point = [h](int i, int j, int k) {
        return Vec3{(static_cast<double>(i) + 0.5) * h, (static_cast<double>(j) + 0.5) * h, (static_cast<double>(k) + 0.5) * h};
    };
    std::vector<double> d(parts);
    std::vector<double> blend(parts);
    const double reach = kHalfDiagonal * h;      // (from a brick's centre to its corners)
    const double near = reach + 1.5 * h;         // (… and to every point it evaluates: a cell's point may move half a cell out)
    const double keep = 3.0 * h;                 // (values below this are made exactly; above it only their sign counts)
    std::vector<Brick> bricks;

    // 1. the bricks near the surface, their samples
    for (int bz = b0[2]; bz <= b1[2]; ++bz) {
        for (int by = b0[1]; by <= b1[1]; ++by) {
            for (int bx = b0[0]; bx <= b1[0]; ++bx) {
                const Vec3 c = point(bx * kCells + 4, by * kCells + 4, bz * kCells + 4);
                const double fc = field.at(c, nullptr, d.data());
                if (std::fabs(fc) > kLipschitz * reach) continue;
                Brick b;
                b.bx = bx;
                b.by = by;
                b.bz = bz;
                b.use.assign(parts, 0);
                // (a part this far away cannot reach the values that shape the mesh: its own distance, and the widest
                // blend it takes part in at a joint whose reach meets the brick's)
                std::fill(blend.begin(), blend.end(), 0.0);
                for (const figure_shape::Joint& jt : field.joints) {
                    const auto within = [&](const Vec3& at, double radius) {
                        const double dx = c[0] - at[0];
                        const double dy = c[1] - at[1];
                        const double dz = c[2] - at[2];
                        const double r = radius + near;
                        return dx * dx + dy * dy + dz * dz < r * r;
                    };
                    // (the width the joint can have in the brick: its own, and what it has more about its second place)
                    const double k = (within(jt.at, jt.radius) ? jt.k : 0.0) + (jt.more_k > 0.0 && within(jt.more, jt.more_radius) ? jt.more_k : 0.0);
                    if (!(k > 0.0)) continue;
                    blend[static_cast<std::size_t>(jt.p)] = std::max(blend[static_cast<std::size_t>(jt.p)], k);
                    blend[static_cast<std::size_t>(jt.c)] = std::max(blend[static_cast<std::size_t>(jt.c)], k);
                }
                for (std::size_t i = 0; i < parts; ++i) b.use[i] = d[i] - near < keep + 1.25 * blend[i] ? 1 : 0;
                b.value.resize(static_cast<std::size_t>(kSide * kSide * kSide));
                bool inside = false;
                bool outside = false;
                for (int z = 0; z < kSide; ++z) {
                    for (int y = 0; y < kSide; ++y) {
                        for (int x = 0; x < kSide; ++x) {
                            const double v = field.at(point(bx * kCells + x, by * kCells + y, bz * kCells + z), b.use.data(), d.data());
                            b.value[static_cast<std::size_t>((z * kSide + y) * kSide + x)] = v;
                            if (v < 0.0) {
                                inside = true;
                            } else {
                                outside = true;
                            }
                        }
                    }
                }
                if (!(inside && outside)) continue;
                b.vertex.assign(static_cast<std::size_t>(kCells * kCells * kCells), -1);
                index[(static_cast<std::size_t>(bz - b0[2]) * static_cast<std::size_t>(nby) + static_cast<std::size_t>(by - b0[1])) *
                          static_cast<std::size_t>(nbx) +
                      static_cast<std::size_t>(bx - b0[0])] = static_cast<std::int32_t>(bricks.size());
                bricks.push_back(std::move(b));
            }
        }
    }

    // 2. a point in each cell the surface crosses
    const double step = h * 0.05;  // (the gradient's step)
    const double normal_step = h * normal_steps;
    for (Brick& b : bricks) {
        const auto f = [&](const Vec3& x) { return field.at(x, b.use.data(), d.data()); };
        const auto gradient = [&](const Vec3& x, double e) {
            return Vec3{f(Vec3{x[0] + e, x[1], x[2]}) - f(Vec3{x[0] - e, x[1], x[2]}), f(Vec3{x[0], x[1] + e, x[2]}) - f(Vec3{x[0], x[1] - e, x[2]}),
                        f(Vec3{x[0], x[1], x[2] + e}) - f(Vec3{x[0], x[1], x[2] - e})};
        };
        for (int z = 0; z < kCells; ++z) {
            for (int y = 0; y < kCells; ++y) {
                for (int x = 0; x < kCells; ++x) {
                    double v[8];
                    int inside = 0;
                    for (int c = 0; c < 8; ++c) {
                        v[c] = b.value[static_cast<std::size_t>(((z + (c >> 2)) * kSide + (y + ((c >> 1) & 1))) * kSide + (x + (c & 1)))];
                        if (v[c] < 0.0) ++inside;
                    }
                    if (inside == 0 || inside == 8) continue;
                    const int i = b.bx * kCells + x;
                    const int j = b.by * kCells + y;
                    const int k = b.bz * kCells + z;
                    const Vec3 base = point(i, j, k);
                    Vec3 sum{0.0, 0.0, 0.0};
                    int count = 0;
                    for (const auto& e : kEdges) {
                        const double va = v[e[0]];
                        const double vb = v[e[1]];
                        if ((va < 0.0) == (vb < 0.0)) continue;
                        const double t = va / (va - vb);
                        const double pa[3] = {static_cast<double>(e[0] & 1), static_cast<double>((e[0] >> 1) & 1), static_cast<double>(e[0] >> 2)};
                        const double pb[3] = {static_cast<double>(e[1] & 1), static_cast<double>((e[1] >> 1) & 1), static_cast<double>(e[1] >> 2)};
                        for (int a = 0; a < 3; ++a) sum[a] = sum[a] + (pa[a] + (pb[a] - pa[a]) * t);
                        ++count;
                    }
                    Vec3 p{base[0] + sum[0] / count * h, base[1] + sum[1] / count * h, base[2] + sum[2] / count * h};
                    // two Newton steps onto the surface, kept within half a cell of the cell (the second only when the
                    // first has not reached it); the part nearest to the point
                    int nearest = 0;
                    for (int it = 0; it < 2; ++it) {
                        const double fp = field.at(p, b.use.data(), d.data(), &nearest);
                        if (it == 1 && std::fabs(fp) < 1e-4 * h) break;  // (on the surface already)
                        const Vec3 g = gradient(p, step);
                        const double gx = g[0] / (2.0 * step);
                        const double gy = g[1] / (2.0 * step);
                        const double gz = g[2] / (2.0 * step);
                        const double g2 = gx * gx + gy * gy + gz * gz;
                        if (!(g2 > 1e-12)) break;
                        const double s = fp / g2;
                        const Vec3 q{p[0] - gx * s, p[1] - gy * s, p[2] - gz * s};
                        for (int a = 0; a < 3; ++a) p[a] = std::min(base[a] + 1.5 * h, std::max(base[a] - 0.5 * h, q[a]));
                        if (it == 1) (void)field.at(p, b.use.data(), d.data(), &nearest);
                    }
                    // (the normal: the field's slope over a wider step, 0.4 h or what is asked — along a crease it turns from one side's
                    // to the other's over the points beside it, so the light shows a clean fold, not a ragged one)
                    const Vec3 g = gradient(p, normal_step);
                    const double n2 = g[0] * g[0] + g[1] * g[1] + g[2] * g[2];
                    const double len = std::sqrt(n2);
                    b.vertex[static_cast<std::size_t>((z * kCells + y) * kCells + x)] = static_cast<std::int32_t>(out.v.size());
                    out.v.push_back(p);
                    out.n.push_back(len > 0.0 ? Vec3{g[0] / len, g[1] / len, g[2] / len} : Vec3{0.0, 0.0, -1.0});
                    out.part.push_back(static_cast<std::uint8_t>(nearest < 0 ? 0 : nearest));
                    out.no_contour.push_back(nearest >= 0 && field.parts[static_cast<std::size_t>(nearest)].no_contour ? 1 : 0);
                }
            }
        }
    }

    // 3. a quad for each crossed edge, around it
    const auto cell = [&](int i, int j, int k) -> std::int32_t {
        const int bx = floor_div(i, kCells);
        const int by = floor_div(j, kCells);
        const int bz = floor_div(k, kCells);
        if (bx < b0[0] || bx > b1[0] || by < b0[1] || by > b1[1] || bz < b0[2] || bz > b1[2]) return -1;
        const std::int32_t at =
            index[(static_cast<std::size_t>(bz - b0[2]) * static_cast<std::size_t>(nby) + static_cast<std::size_t>(by - b0[1])) * static_cast<std::size_t>(nbx) +
                  static_cast<std::size_t>(bx - b0[0])];
        if (at < 0) return -1;
        const Brick& b = bricks[static_cast<std::size_t>(at)];
        return b.vertex[static_cast<std::size_t>(((k - bz * kCells) * kCells + (j - by * kCells)) * kCells + (i - bx * kCells))];
    };
    const auto dist2 = [&](std::int32_t a, std::int32_t b) {
        const Vec3& p = out.v[static_cast<std::size_t>(a)];
        const Vec3& q = out.v[static_cast<std::size_t>(b)];
        const double x = p[0] - q[0];
        const double y = p[1] - q[1];
        const double z = p[2] - q[2];
        return x * x + y * y + z * z;
    };
    for (const Brick& b : bricks) {
        for (int z = 0; z < kCells; ++z) {
            for (int y = 0; y < kCells; ++y) {
                for (int x = 0; x < kCells; ++x) {
                    const double v0 = b.value[static_cast<std::size_t>((z * kSide + y) * kSide + x)];
                    const int i = b.bx * kCells + x;
                    const int j = b.by * kCells + y;
                    const int k = b.bz * kCells + z;
                    for (int axis = 0; axis < 3; ++axis) {
                        const int dx = axis == 0 ? 1 : 0;
                        const int dy = axis == 1 ? 1 : 0;
                        const int dz = axis == 2 ? 1 : 0;
                        const double v1 = b.value[static_cast<std::size_t>(((z + dz) * kSide + (y + dy)) * kSide + (x + dx))];
                        if ((v0 < 0.0) == (v1 < 0.0)) continue;
                        // the four cells around the edge: −b−c, −c, 0, −b (b, c the next axes in turn): facing +axis
                        std::int32_t q[4];
                        if (axis == 0) {
                            q[0] = cell(i, j - 1, k - 1);
                            q[1] = cell(i, j, k - 1);
                            q[2] = cell(i, j, k);
                            q[3] = cell(i, j - 1, k);
                        } else if (axis == 1) {
                            q[0] = cell(i - 1, j, k - 1);
                            q[1] = cell(i - 1, j, k);
                            q[2] = cell(i, j, k);
                            q[3] = cell(i, j, k - 1);
                        } else {
                            q[0] = cell(i - 1, j - 1, k);
                            q[1] = cell(i, j - 1, k);
                            q[2] = cell(i, j, k);
                            q[3] = cell(i - 1, j, k);
                        }
                        if (q[0] < 0 || q[1] < 0 || q[2] < 0 || q[3] < 0) continue;
                        if (!(v0 < 0.0)) std::swap(q[1], q[3]);  // (the inside is past the edge: face the other way)
                        if (dist2(q[0], q[2]) <= dist2(q[1], q[3])) {
                            out.t.push_back({q[0], q[1], q[2]});
                            out.t.push_back({q[0], q[2], q[3]});
                        } else {
                            out.t.push_back({q[0], q[1], q[3]});
                            out.t.push_back({q[1], q[2], q[3]});
                        }
                    }
                }
            }
        }
    }
    return out;
}

}  // namespace genko::core::surface_nets
