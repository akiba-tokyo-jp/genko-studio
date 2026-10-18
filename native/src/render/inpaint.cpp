#include "render/inpaint.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>

#include "render/poisson.hpp"

// PhotoCraft v0.2.0 crates/algo/src/inpaint.rs (lines 1-620), MIT: native/third_party/photocraft/NOTICE.md.

namespace genko::render::inpaint {

std::uint64_t Rng::next() {
    state_ += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

int Rng::range(int lo, int hi) {
    if (hi <= lo) return lo;
    return lo + static_cast<int>(next() % static_cast<std::uint64_t>(hi - lo + 1));
}

std::optional<HoleBox> hole_box(std::size_t w, std::size_t h, const std::vector<bool>& hole) {
    std::optional<HoleBox> b;
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            if (!hole[y * w + x]) continue;
            if (!b) {
                b = HoleBox{x, y, x + 1, y + 1};
            } else {
                b->x0 = std::min(b->x0, x);
                b->y0 = std::min(b->y0, y);
                b->x1 = std::max(b->x1, x + 1);
                b->y1 = std::max(b->y1, y + 1);
            }
        }
    }
    return b;
}

namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();

struct Level {
    std::size_t w = 0, h = 0;
    std::vector<float> img;
    std::vector<bool> hole;
};

Level downsample(const Level& l, std::size_t ch) {
    Level d;
    d.w = (l.w + 1) / 2;
    d.h = (l.h + 1) / 2;
    d.img.assign(d.w * d.h * ch, 0.0f);
    d.hole.assign(d.w * d.h, false);
    for (std::size_t y = 0; y < d.h; ++y) {
        for (std::size_t x = 0; x < d.w; ++x) {
            float n = 0.0f;
            const std::size_t o = (y * d.w + x) * ch;
            for (std::size_t k = 0; k < 4; ++k) {
                const std::size_t sx = x * 2 + (k & 1), sy = y * 2 + (k >> 1);
                if (sx >= l.w || sy >= l.h) continue;
                const std::size_t si = sy * l.w + sx;
                if (l.hole[si]) {
                    d.hole[y * d.w + x] = true;
                } else {
                    for (std::size_t c = 0; c < ch; ++c) d.img[o + c] += l.img[si * ch + c];
                    n += 1.0f;
                }
            }
            if (n > 0.0f) {
                for (std::size_t c = 0; c < ch; ++c) d.img[o + c] /= n;
            }
        }
    }
    return d;
}

// The summed-area table of a boolean grid ((w + 1) × (h + 1)).
std::vector<std::uint32_t> integral(std::size_t w, std::size_t h, const std::vector<bool>& m) {
    std::vector<std::uint32_t> s((w + 1) * (h + 1), 0);
    for (std::size_t y = 0; y < h; ++y) {
        std::uint32_t row = 0;
        for (std::size_t x = 0; x < w; ++x) {
            row += m[y * w + x] ? 1 : 0;
            s[(y + 1) * (w + 1) + x + 1] = s[y * (w + 1) + x + 1] + row;
        }
    }
    return s;
}

// How many cells are set in the window [x0, x1) × [y0, y1), clipped to the grid.
std::uint32_t window_count(const std::vector<std::uint32_t>& s, std::size_t gw, std::size_t gh, long x0, long y0, long x1, long y1) {
    const auto ux0 = static_cast<std::size_t>(std::max(x0, 0L)), uy0 = static_cast<std::size_t>(std::max(y0, 0L));
    const std::size_t ux1 = std::min(static_cast<std::size_t>(std::max(x1, 0L)), gw), uy1 = std::min(static_cast<std::size_t>(std::max(y1, 0L)), gh);
    if (ux1 <= ux0 || uy1 <= uy0) return 0;
    const std::size_t w1 = gw + 1;
    return s[uy1 * w1 + ux1] + s[uy0 * w1 + ux0] - s[uy0 * w1 + ux1] - s[uy1 * w1 + ux0];
}

using Match = std::pair<int, int>;

// One level's PatchMatch and EM.
struct Solver {
    std::size_t w = 0, h = 0, ch = 0;
    int r = 0;
    std::vector<float>* img = nullptr;
    const std::vector<bool>* hole = nullptr;
    // Valid sources: the whole patch inside the grid, no hole pixel in it.
    std::vector<bool> valid;
    std::vector<Match> valid_list;
    // Targets: the patch overlaps the hole.
    std::vector<bool> targets;

    bool is_valid(int x, int y) const {
        return x >= 0 && y >= 0 && static_cast<std::size_t>(x) < w && static_cast<std::size_t>(y) < h &&
               valid[static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)];
    }

    // The mean squared difference of the (clipped) target patch at t and the source patch at s.
    float dist(Match t, Match s, float cutoff) const {
        const int iw = static_cast<int>(w), ih = static_cast<int>(h);
        const float full = static_cast<float>((2 * r + 1) * (2 * r + 1)) * static_cast<float>(ch);
        const std::vector<float>& im = *img;
        float sum = 0.0f;
        std::size_t n = 0;
        for (int dy = -r; dy <= r; ++dy) {
            const int ty = t.second + dy;
            if (ty < 0 || ty >= ih) continue;
            const int sy = s.second + dy;
            for (int dx = -r; dx <= r; ++dx) {
                const int tx = t.first + dx;
                if (tx < 0 || tx >= iw) continue;
                const std::size_t ti = (static_cast<std::size_t>(ty) * w + static_cast<std::size_t>(tx)) * ch;
                const std::size_t si = (static_cast<std::size_t>(sy) * w + static_cast<std::size_t>(s.first + dx)) * ch;
                for (std::size_t c = 0; c < ch; ++c) {
                    const float d = im[ti + c] - im[si + c];
                    sum += d * d;
                }
                n += ch;
            }
            // (early out: the mean can only be ≥ sum / the whole patch's size)
            if (sum > cutoff * full) return kInf;
        }
        return n == 0 ? kInf : sum / static_cast<float>(n);
    }

    // One PatchMatch pass over the targets, in bands of rows (each band propagates within itself, with its own
    // generator: the same result whichever order the bands run in).
    void patchmatch(std::vector<Match>& nnf, std::vector<float>& cost, std::size_t pass, std::uint64_t seed, int search_radius) const {
        constexpr std::size_t kBand = 16;
        const bool reverse = pass % 2 == 1;
        const int step = reverse ? -1 : 1;
        const std::size_t bands = (h + kBand - 1) / kBand;
        for (std::size_t bi = 0; bi < bands; ++bi) {
            Rng rng(seed ^ (static_cast<std::uint64_t>(bi) * 0x2545F4914F6CDD1DULL) ^ (static_cast<std::uint64_t>(pass) << 40));
            const std::size_t y0 = bi * kBand;
            const std::size_t rows = std::min(kBand, h - y0);
            Match* nn = nnf.data() + y0 * w;
            float* co = cost.data() + y0 * w;
            for (std::size_t ry = 0; ry < rows; ++ry) {
                const std::size_t ly = reverse ? rows - 1 - ry : ry;
                const std::size_t y = y0 + ly;
                for (std::size_t rx = 0; rx < w; ++rx) {
                    const std::size_t x = reverse ? w - 1 - rx : rx;
                    const std::size_t li = ly * w + x;
                    if (!targets[y * w + x]) continue;
                    const Match t{static_cast<int>(x), static_cast<int>(y)};
                    Match best = nn[li];
                    float bc = co[li];
                    // propagation from the neighbours already visited (in this band)
                    const long nx = static_cast<long>(x) - step;
                    if (nx >= 0 && static_cast<std::size_t>(nx) < w) {
                        const Match c = nn[ly * w + static_cast<std::size_t>(nx)];
                        const Match cand{c.first + step, c.second};
                        if (is_valid(cand.first, cand.second) && cand != best) {
                            const float d = dist(t, cand, bc);
                            if (d < bc) best = cand, bc = d;
                        }
                    }
                    const long nly = static_cast<long>(ly) - step;
                    if (nly >= 0 && static_cast<std::size_t>(nly) < rows) {
                        const Match c = nn[static_cast<std::size_t>(nly) * w + x];
                        const Match cand{c.first, c.second + step};
                        if (is_valid(cand.first, cand.second) && cand != best) {
                            const float d = dist(t, cand, bc);
                            if (d < bc) best = cand, bc = d;
                        }
                    }
                    // a random search in windows halving each time
                    for (int rad = search_radius; rad >= 1; rad /= 2) {
                        const int cx = best.first + rng.range(-rad, rad);
                        const int cy = best.second + rng.range(-rad, rad);
                        const Match cand{cx, cy};
                        if (is_valid(cand.first, cand.second) && cand != best) {
                            const float d = dist(t, cand, bc);
                            if (d < bc) best = cand, bc = d;
                        }
                    }
                    nn[li] = best;
                    co[li] = bc;
                }
            }
        }
    }

    // The hole's pixels again, voted by every patch match over them (weighted by how alike).
    void vote(const std::vector<Match>& nnf, const std::vector<float>& cost) {
        // σ² from the 75th percentile of the match costs (a robust scale, as Wexler et al.)
        std::vector<float> cs;
        for (std::size_t i = 0; i < cost.size(); ++i) {
            if (targets[i] && std::isfinite(cost[i])) cs.push_back(cost[i]);
        }
        float sigma2 = 1.0f;
        if (!cs.empty()) {
            const std::size_t k = std::min(cs.size() * 3 / 4, cs.size() - 1);
            std::nth_element(cs.begin(), cs.begin() + static_cast<std::ptrdiff_t>(k), cs.end());
            sigma2 = std::max(cs[k], 1e-6f);
        }
        const std::vector<float>& im = *img;
        std::vector<float> voted(w * h * ch, 0.0f);
        std::vector<char> made(w * h, 0);
        std::vector<float> acc(ch);
        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < w; ++x) {
                if (!(*hole)[y * w + x]) continue;
                std::fill(acc.begin(), acc.end(), 0.0f);
                float wsum = 0.0f;
                for (int dy = -r; dy <= r; ++dy) {
                    const long ty = static_cast<long>(y) + dy;
                    if (ty < 0 || static_cast<std::size_t>(ty) >= h) continue;
                    for (int dx = -r; dx <= r; ++dx) {
                        const long tx = static_cast<long>(x) + dx;
                        if (tx < 0 || static_cast<std::size_t>(tx) >= w) continue;
                        const std::size_t ti = static_cast<std::size_t>(ty) * w + static_cast<std::size_t>(tx);
                        if (!targets[ti] || !std::isfinite(cost[ti])) continue;
                        const Match s = nnf[ti];
                        const int sx = s.first - dx, sy = s.second - dy;
                        const float wt = std::max(std::exp(-cost[ti] / (2.0f * sigma2)), 1e-8f);
                        const std::size_t si = (static_cast<std::size_t>(sy) * w + static_cast<std::size_t>(sx)) * ch;
                        for (std::size_t c = 0; c < ch; ++c) acc[c] += im[si + c] * wt;
                        wsum += wt;
                    }
                }
                if (wsum > 0.0f) {
                    for (std::size_t c = 0; c < ch; ++c) voted[(y * w + x) * ch + c] = acc[c] / wsum;
                    made[y * w + x] = 1;
                }
            }
        }
        // (laid in after all are voted: each from the pixels as they were)
        for (std::size_t i = 0; i < w * h; ++i) {
            if (made[i]) std::copy(voted.begin() + static_cast<std::ptrdiff_t>(i * ch), voted.begin() + static_cast<std::ptrdiff_t>((i + 1) * ch),
                                   img->begin() + static_cast<std::ptrdiff_t>(i * ch));
        }
    }
};

}  // namespace

std::optional<std::vector<float>> complete(std::size_t w, std::size_t h, std::size_t ch, const std::vector<float>& img, const std::vector<bool>& hole,
                                           const CompleteParams& p) {
    assert(img.size() == w * h * ch && hole.size() == w * h);
    const std::optional<HoleBox> box = hole_box(w, h, hole);
    if (!box) return img;
    const std::size_t r = std::max<std::size_t>(p.patch_radius, 1);
    const std::size_t psz = 2 * r + 1;
    // the pyramid: halved until the hole is a few patches across
    std::vector<Level> levels;
    levels.push_back(Level{w, h, img, hole});
    std::size_t ext = std::max(box->x1 - box->x0, box->y1 - box->y0);
    std::vector<std::size_t> exts{ext};
    for (;;) {
        const Level& l = levels.back();
        if (ext <= 2 * psz || l.w / 2 < 3 * psz || l.h / 2 < 3 * psz) break;
        Level d = downsample(l, ch);
        levels.push_back(std::move(d));
        ext = (ext + 1) / 2;
        exts.push_back(ext);
    }
    const std::size_t nlev = levels.size();
    std::vector<Match> nnf;
    struct Prev {
        std::size_t w = 0, h = 0;
        std::vector<float> img;
    };
    std::optional<Prev> prev;
    for (std::size_t li = nlev; li-- > 0;) {
        const std::size_t lw = levels[li].w, lh = levels[li].h;
        std::vector<float> limg = levels[li].img;
        const std::vector<bool>& lhole = levels[li].hole;
        // the hole started by the membrane at the coarsest level, by the coarser result elsewhere
        if (!prev) {
            limg = poisson::membrane_fill(lw, lh, ch, limg, lhole);
        } else {
            for (std::size_t y = 0; y < lh; ++y) {
                for (std::size_t x = 0; x < lw; ++x) {
                    if (!lhole[y * lw + x]) continue;
                    const std::size_t sx = std::min(x / 2, prev->w - 1), sy = std::min(y / 2, prev->h - 1);
                    std::copy_n(prev->img.begin() + static_cast<std::ptrdiff_t>((sy * prev->w + sx) * ch), ch,
                                limg.begin() + static_cast<std::ptrdiff_t>((y * lw + x) * ch));
                }
            }
        }
        const int ri = static_cast<int>(r);
        const std::vector<std::uint32_t> hs = integral(lw, lh, lhole);
        Solver solver;
        solver.w = lw;
        solver.h = lh;
        solver.ch = ch;
        solver.r = ri;
        solver.img = &limg;
        solver.hole = &lhole;
        solver.valid.assign(lw * lh, false);
        solver.targets.assign(lw * lh, false);
        for (long y = 0; y < static_cast<long>(lh); ++y) {
            for (long x = 0; x < static_cast<long>(lw); ++x) {
                const std::uint32_t cnt = window_count(hs, lw, lh, x - ri, y - ri, x + ri + 1, y + ri + 1);
                const bool inside = x >= ri && y >= ri && x + ri < static_cast<long>(lw) && y + ri < static_cast<long>(lh);
                const std::size_t i = static_cast<std::size_t>(y) * lw + static_cast<std::size_t>(x);
                if (inside && cnt == 0) {
                    solver.valid[i] = true;
                    solver.valid_list.emplace_back(static_cast<int>(x), static_cast<int>(y));
                }
                solver.targets[i] = cnt > 0;
            }
        }
        if (solver.valid_list.empty()) {
            if (li == 0) return std::nullopt;
            prev = Prev{lw, lh, std::move(limg)};
            nnf.clear();
            continue;
        }
        // the match field: from the coarser level where it can be, at random elsewhere
        Rng rng(p.seed ^ (static_cast<std::uint64_t>(li) << 20));
        const std::size_t pw_prev = prev ? prev->w : 0;
        const std::vector<Match> old = std::move(nnf);
        std::vector<Match> fresh(lw * lh, Match{0, 0});
        for (std::size_t y = 0; y < lh; ++y) {
            for (std::size_t x = 0; x < lw; ++x) {
                if (!solver.targets[y * lw + x]) continue;
                std::optional<Match> cand;
                if (!old.empty() && pw_prev > 0) {
                    const std::size_t pi = (y / 2) * pw_prev + (x / 2);
                    if (pi < old.size()) {
                        const Match c{old[pi].first * 2 + static_cast<int>(x % 2), old[pi].second * 2 + static_cast<int>(y % 2)};
                        if (solver.is_valid(c.first, c.second)) cand = c;
                    }
                }
                fresh[y * lw + x] = cand ? *cand : solver.valid_list[static_cast<std::size_t>(rng.next() % solver.valid_list.size())];
            }
        }
        nnf = std::move(fresh);
        const bool refine_only = li + 1 < nlev && exts[li] > p.refine_extent;
        const std::size_t em = li + 1 == nlev ? p.em_iters : refine_only ? 1 : std::max(p.em_iters >> (nlev - 1 - li), p.min_em_iters);
        const std::size_t pm_iters = refine_only ? 1 : p.pm_iters;
        const int full_search = static_cast<int>(std::max(lw, lh));
        std::vector<float> cost(lw * lh, kInf);
        for (std::size_t it = 0; it < em; ++it) {
            // (the hole's pixels changed: the costs again before each PatchMatch)
            for (std::size_t i = 0; i < cost.size(); ++i) {
                if (solver.targets[i]) cost[i] = solver.dist(Match{static_cast<int>(i % lw), static_cast<int>(i / lw)}, nnf[i], kInf);
            }
            // a wide random search on the coarsest level and at first, a local one after
            const int radius = refine_only ? 2 : (li + 1 == nlev || it == 0) ? full_search : std::min(4 * static_cast<int>(psz), full_search);
            for (std::size_t pass = 0; pass < pm_iters; ++pass) {
                solver.patchmatch(nnf, cost, pass + it * pm_iters, p.seed + static_cast<std::uint64_t>(li * 1000 + it), radius);
            }
            solver.vote(nnf, cost);
        }
        prev = Prev{lw, lh, std::move(limg)};
    }
    if (!prev) return std::nullopt;
    return std::move(prev->img);
}

std::optional<std::pair<int, int>> best_offset(std::size_t w, std::size_t h, std::size_t ch, const std::vector<float>& img,
                                               const std::vector<bool>& hole, std::size_t ring_px, int max_radius) {
    const std::optional<HoleBox> box = hole_box(w, h, hole);
    if (!box) return std::nullopt;
    const long ring = static_cast<long>(std::max<std::size_t>(ring_px, 1));
    const std::vector<std::uint32_t> hs = integral(w, h, hole);
    const long iw = static_cast<long>(w), ih = static_cast<long>(h);
    // the sample points: the hole's ring (grown, less the hole), thinned for speed
    const long x0 = static_cast<long>(box->x0) - ring, y0 = static_cast<long>(box->y0) - ring;
    const long x1 = static_cast<long>(box->x1) + ring, y1 = static_cast<long>(box->y1) + ring;
    std::vector<std::pair<long, long>> all;
    for (long y = std::max(y0, 0L); y < std::min(y1, ih); ++y) {
        for (long x = std::max(x0, 0L); x < std::min(x1, iw); ++x) {
            if (hole[static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)]) continue;
            if (window_count(hs, w, h, x - ring, y - ring, x + ring + 1, y + ring + 1) > 0) all.emplace_back(x, y);
        }
    }
    const std::size_t stride = std::max<std::size_t>(all.size() / 2000, 1);
    std::vector<std::pair<long, long>> pts;
    for (std::size_t i = 0; i < all.size(); i += stride) pts.push_back(all[i]);
    const auto eval = [&](int dx, int dy) -> std::optional<float> {
        const long hx0 = x0 + dx, hy0 = y0 + dy, hx1 = x1 + dx, hy1 = y1 + dy;
        const bool inside = hx0 >= 0 && hy0 >= 0 && hx1 <= iw && hy1 <= ih;
        if ((dx == 0 && dy == 0) || !inside || window_count(hs, w, h, hx0, hy0, hx1, hy1) != 0) return std::nullopt;
        float e = 0.0f;
        for (const auto& [x, y] : pts) {
            const std::size_t a = (static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)) * ch;
            const std::size_t b = (static_cast<std::size_t>(y + dy) * w + static_cast<std::size_t>(x + dx)) * ch;
            for (std::size_t c = 0; c < ch; ++c) {
                const float d = img[a + c] - img[b + c];
                e += d * d;
            }
        }
        // (nearer sources slightly preferred on ties)
        return e / static_cast<float>(std::max<std::size_t>(pts.size(), 1)) *
               (1.0f + 1e-3f * std::sqrt(static_cast<float>(dx * dx + dy * dy)) / static_cast<float>(std::max(max_radius, 1)));
    };
    // a coarse grid over the search window, then every pixel around the best
    const int step = std::max(max_radius / 24, 1);
    std::optional<std::pair<std::pair<int, int>, float>> best;
    const auto consider = [&](int dx, int dy) {
        if (const std::optional<float> e = eval(dx, dy); e && (!best || *e < best->second)) best = std::make_pair(std::make_pair(dx, dy), *e);
    };
    for (int dy = -max_radius; dy <= max_radius; dy += step) {
        for (int dx = -max_radius; dx <= max_radius; dx += step) consider(dx, dy);
    }
    if (step > 1 && best) {
        const auto [bx, by] = best->first;
        for (int dy = by - step; dy <= by + step; ++dy) {
            for (int dx = bx - step; dx <= bx + step; ++dx) consider(dx, dy);
        }
    }
    if (!best) return std::nullopt;
    return best->first;
}

std::vector<float> synthesize(std::size_t w, std::size_t h, std::size_t ch, const std::vector<float>& img, const std::vector<bool>& hole,
                              std::size_t block, std::uint64_t seed) {
    std::vector<float> out = img;
    const std::optional<HoleBox> box = hole_box(w, h, hole);
    if (!box) return out;
    const long b = static_cast<long>(std::max<std::size_t>(block, 4));
    const long ov = std::max(b / 3, 1L);
    const long iw = static_cast<long>(w), ih = static_cast<long>(h);
    const std::vector<std::uint32_t> hs = integral(w, h, hole);
    // candidate blocks: the whole block (and its overlap) inside the grid and outside the hole
    std::vector<std::pair<long, long>> cands;
    for (long y = ov; y < ih - b - ov; ++y) {
        for (long x = ov; x < iw - b - ov; ++x) {
            if (window_count(hs, w, h, x - ov, y - ov, x + b + ov, y + b + ov) == 0) cands.emplace_back(x, y);
        }
    }
    if (cands.empty()) return poisson::membrane_fill(w, h, ch, img, hole);
    std::vector<bool> filled(w * h);
    for (std::size_t i = 0; i < w * h; ++i) filled[i] = !hole[i];
    Rng rng(seed);
    for (long by = static_cast<long>(box->y0); by < static_cast<long>(box->y1); by += b) {
        for (long bx = static_cast<long>(box->x0); bx < static_cast<long>(box->x1); bx += b) {
            if (window_count(hs, w, h, bx, by, bx + b, by + b) == 0) continue;
            // the candidate that best agrees with the pixels known (or made) around and in the block
            std::pair<long, long> chosen = cands[0];
            float chosen_e = std::numeric_limits<float>::infinity();
            for (int k = 0; k < 48; ++k) {
                const std::pair<long, long> c = cands[static_cast<std::size_t>(rng.next() % cands.size())];
                float e = 0.0f;
                std::uint32_t n = 0;
                for (long y = by - ov; y < by + b + ov; ++y) {
                    for (long x = bx - ov; x < bx + b + ov; ++x) {
                        if (x < 0 || y < 0 || x >= iw || y >= ih || !filled[static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)]) continue;
                        const std::size_t a = (static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)) * ch;
                        const std::size_t s = (static_cast<std::size_t>(y - by + c.second) * w + static_cast<std::size_t>(x - bx + c.first)) * ch;
                        for (std::size_t q = 0; q < ch; ++q) {
                            const float d = out[a + q] - img[s + q];
                            e += d * d;
                        }
                        ++n;
                    }
                }
                e = n == 0 ? 0.0f : e / static_cast<float>(n);
                if (e < chosen_e) chosen = c, chosen_e = e;
            }
            for (long y = by - ov; y < by + b + ov; ++y) {
                for (long x = bx - ov; x < bx + b + ov; ++x) {
                    if (x < 0 || y < 0 || x >= iw || y >= ih) continue;
                    const std::size_t i = static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x);
                    if (!hole[i]) continue;
                    const std::size_t a = i * ch;
                    const std::size_t s = (static_cast<std::size_t>(y - by + chosen.second) * w + static_cast<std::size_t>(x - bx + chosen.first)) * ch;
                    // (feathered into what was made already)
                    const float k = filled[i] ? 0.5f : 1.0f;
                    for (std::size_t q = 0; q < ch; ++q) out[a + q] += (img[s + q] - out[a + q]) * k;
                    filled[i] = true;
                }
            }
        }
    }
    return out;
}

}  // namespace genko::render::inpaint
