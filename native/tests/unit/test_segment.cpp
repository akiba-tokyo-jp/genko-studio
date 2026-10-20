// PC3 B3 (SPEC.md COLOR-06「賢い選択」): the smart selection's algorithms as PhotoCraft v0.2.0's algo/src/segment/tests.rs
// and sky.rs test them (MIT, native/third_party/photocraft/NOTICE.md) — the max-flow against a reference on random
// graphs, the grid cut, the colour models, the superpixels, GrabCut and Object Selection (at full resolution and with the
// band cut again), the Quick Selection brush (grown without leaking, stopped at a thin line, a reduced window), Select
// Subject and its saliency, Focus Area, Select Sky, the samplers' downsampling, the mask's cleaning, the geodesic barrier
// — by properties (IoU, which side), as upstream's; and that the same picture gives the same bytes again, and that a
// stop asked for stops it.

#include <QtTest>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <functional>
#include <stop_token>
#include <vector>

#include "render/matting.hpp"
#include "render/page.hpp"
#include "render/segment/segment.hpp"

using namespace genko::render;
using segment::Rect;
using segment::Region;
using segment::RgbImage;
using segment::Rng;

namespace {

RgbImage from_fn(std::size_t w, std::size_t h, const std::function<segment::Rgb(std::size_t, std::size_t)>& f) {
    RgbImage img(w, h);
    for (std::size_t y = 0; y < h; ++y)
        for (std::size_t x = 0; x < w; ++x) img.px[y * w + x] = f(x, y);
    return img;
}

segment::Rgb clamp01(segment::Rgb c) {
    for (float& v : c) v = std::clamp(v, 0.0f, 1.0f);
    return c;
}

// A textured disc on a noisy background, and the truth.
std::pair<RgbImage, std::vector<bool>> disc_scene(std::size_t w, std::size_t h, float cx, float cy, float r, std::uint64_t seed) {
    Rng rng(seed);
    RgbImage img(w, h);
    std::vector<bool> truth(w * h, false);
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            const float dx = static_cast<float>(x) + 0.5f - cx, dy = static_cast<float>(y) + 0.5f - cy;
            const bool inside = dx * dx + dy * dy <= r * r;
            truth[y * w + x] = inside;
            const float n = 0.05f * rng.normal();
            if (inside) {
                const float t = std::clamp(std::sin(static_cast<float>(x + y) * 0.35f) * 0.5f + 0.5f, 0.0f, 1.0f);
                img.px[y * w + x] = clamp01({0.85f + 0.05f * t + n, 0.2f + 0.3f * t + n, 0.15f + n});
            } else {
                img.px[y * w + x] = clamp01({0.25f + n, 0.45f + n, 0.7f + n});
            }
        }
    }
    return {std::move(img), std::move(truth)};
}

float iou(const Region& region, const std::vector<bool>& truth, std::size_t w, std::size_t h) {
    std::size_t inter = 0, uni = 0;
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            const bool a = region.at(static_cast<int>(x), static_cast<int>(y)) >= 0.5f, b = truth[y * w + x];
            inter += a && b ? 1 : 0;
            uni += a || b ? 1 : 0;
        }
    }
    return static_cast<float>(inter) / static_cast<float>(std::max<std::size_t>(uni, 1));
}

// Left: a textured warm region; right: a cool one; a strong vertical edge at x = 60.
RgbImage two_regions(std::size_t w, std::size_t h) {
    Rng rng(5);
    return from_fn(w, h, [&](std::size_t x, std::size_t y) {
        const float n = 0.04f * rng.normal();
        const float t = std::sin(static_cast<float>(x) * 0.5f) * std::cos(static_cast<float>(y) * 0.3f) * 0.05f;
        return x < 60 ? clamp01({0.8f + t + n, 0.5f + n, 0.3f + t}) : clamp01({0.2f + n, 0.35f + t, 0.6f + n});
    });
}

// The reference max flow (Edmonds–Karp on a dense matrix; node 0 the source, n − 1 the sink).
double edmonds_karp(std::vector<std::vector<double>>& cap) {
    const std::size_t n = cap.size();
    double flow = 0;
    for (;;) {
        std::vector<std::size_t> prev(n, SIZE_MAX);
        prev[0] = 0;
        std::deque<std::size_t> q{0};
        while (!q.empty()) {
            const std::size_t u = q.front();
            q.pop_front();
            for (std::size_t v = 0; v < n; ++v) {
                if (prev[v] == SIZE_MAX && cap[u][v] > 1e-9) {
                    prev[v] = u;
                    q.push_back(v);
                }
            }
        }
        if (prev[n - 1] == SIZE_MAX) return flow;
        double b = std::numeric_limits<double>::max();
        for (std::size_t v = n - 1; v != 0; v = prev[v]) b = std::min(b, cap[prev[v]][v]);
        for (std::size_t v = n - 1; v != 0; v = prev[v]) {
            cap[prev[v]][v] -= b;
            cap[v][prev[v]] += b;
        }
        flow += b;
    }
}

}  // namespace

class TestSegment : public QObject {
    Q_OBJECT
private slots:
    // --- the max-flow ---

    void maxflowTinyGraph() {
        // s→a 3, s→b 2, a→b 1, a→t 2, b→t 3: 5
        segment::Graph g(2, 1);
        g.add_tweights(0, 3, 2);
        g.add_tweights(1, 2, 3);
        g.add_edge(0, 1, 1, 0);
        QCOMPARE(g.maxflow(), 5.0);
    }

    void maxflowTextbookNetwork() {
        segment::Graph g(4, 5);  // (v1..v4 = 0..3; 23)
        g.add_tweights(0, 16, 0);
        g.add_tweights(1, 13, 0);
        g.add_tweights(2, 0, 20);
        g.add_tweights(3, 0, 4);
        g.add_edge(0, 1, 10, 4);
        g.add_edge(0, 2, 12, 0);
        g.add_edge(2, 1, 9, 0);
        g.add_edge(1, 3, 14, 0);
        g.add_edge(3, 2, 7, 0);
        QCOMPARE(g.maxflow(), 23.0);
        // (the cut {s, v1, v2, v4} | {v3, t})
        QVERIFY(g.in_source(0) && g.in_source(1) && g.in_source(3));
        QVERIFY(!g.in_source(2));
        QCOMPARE(g.residual_out_of_source_side(), 0.0f);
    }

    void maxflowMatchesTheReference() {
        Rng rng(7);
        for (std::size_t trial = 0; trial < 60; ++trial) {
            const std::size_t n = 3 + trial % 12;
            segment::Graph g(n, n * 3);
            std::vector<std::vector<double>> cap(n + 2, std::vector<double>(n + 2, 0.0));
            struct Edge {
                std::size_t i, j;
                float a, b;
            };
            std::vector<Edge> edges;
            for (std::size_t i = 0; i < n; ++i) {
                float s = std::round(rng.f32() * 10.0f), t = std::round(rng.f32() * 10.0f);
                if (rng.f32() < 0.4f) s = 0;
                if (rng.f32() < 0.4f) t = 0;
                g.add_tweights(i, s, t);
                cap[0][i + 1] += s;
                cap[i + 1][n + 1] += t;
            }
            for (std::size_t i = 0; i < n; ++i) {
                for (std::size_t j = i + 1; j < n; ++j) {
                    if (rng.f32() < 0.35f) {
                        const float a = std::round(rng.f32() * 8.0f), b = std::round(rng.f32() * 8.0f);
                        g.add_edge(i, j, a, b);
                        cap[i + 1][j + 1] += a;
                        cap[j + 1][i + 1] += b;
                        edges.push_back({i, j, a, b});
                    }
                }
            }
            const auto orig = cap;
            const double reference = edmonds_karp(cap);
            const double f = g.maxflow();
            QVERIFY2(std::abs(f - reference) < 1e-3, qPrintable(QStringLiteral("trial %1: %2 vs %3").arg(trial).arg(f).arg(reference)));
            // the cut found holds exactly that much
            double cut = 0;
            for (std::size_t i = 0; i < n; ++i) cut += g.in_source(i) ? orig[i + 1][n + 1] : orig[0][i + 1];
            for (const Edge& e : edges) {
                if (g.in_source(e.i) && !g.in_source(e.j)) cut += e.a;
                if (g.in_source(e.j) && !g.in_source(e.i)) cut += e.b;
            }
            QVERIFY2(std::abs(cut - reference) < 1e-3, qPrintable(QStringLiteral("trial %1: cut %2 vs %3").arg(trial).arg(cut).arg(reference)));
        }
    }

    void gridCutSeparatesTwoHalves() {
        const RgbImage img = from_fn(20, 10, [](std::size_t x, std::size_t) { return x < 10 ? segment::Rgb{0.9f, 0.1f, 0.1f} : segment::Rgb{0.1f, 0.1f, 0.9f}; });
        const std::size_t n = img.w * img.h;
        std::vector<std::uint8_t> fixed(n, segment::kFree);
        fixed[5 * 20 + 1] = segment::kHardFg;
        fixed[5 * 20 + 18] = segment::kHardBg;
        const std::vector<float> zeros(n, 0.0f);
        const segment::Mask cut = segment::grid_cut(img, zeros, zeros, fixed, 50, segment::contrast_beta(img));
        for (std::size_t y = 0; y < 10; ++y)
            for (std::size_t x = 0; x < 20; ++x) QCOMPARE(cut[y * 20 + x] != 0, x < 10);
    }

    // --- the colour models and the superpixels ---

    void gmmRecoversClusters() {
        Rng rng(3);
        struct Truth {
            segment::Rgb mean;
            float sd, weight;
        };
        const Truth truth[3] = {{{0.2f, 0.3f, 0.8f}, 0.03f, 0.5f}, {{0.8f, 0.2f, 0.1f}, 0.05f, 0.3f}, {{0.5f, 0.9f, 0.4f}, 0.02f, 0.2f}};
        std::vector<segment::Rgb> samples;
        for (const Truth& t : truth) {
            for (std::size_t i = 0; i < static_cast<std::size_t>(t.weight * 6000.0f); ++i) {
                segment::Rgb s{};
                for (std::size_t c = 0; c < 3; ++c) s[c] = t.mean[c] + t.sd * rng.normal();
                samples.push_back(s);
            }
        }
        const auto g = segment::Gmm::fit(samples, 3, segment::kDefaultReg);
        QVERIFY(g);
        QCOMPARE(g->comps.size(), std::size_t{3});
        for (const Truth& t : truth) {
            const auto c = std::min_element(g->comps.begin(), g->comps.end(),
                                            [&](const auto& a, const auto& b) { return segment::d2(a.mean, t.mean) < segment::d2(b.mean, t.mean); });
            QVERIFY(std::sqrt(segment::d2(c->mean, t.mean)) < 0.01f);
            QVERIFY(std::abs(c->weight - t.weight) < 0.03f);
            QVERIFY(std::abs(std::sqrt(c->cov[0][0]) - t.sd) < 0.01f);
        }
        QVERIFY(g->neg_log({0.2f, 0.3f, 0.8f}) < g->neg_log({0.5f, 0.5f, 0.5f}));
        const auto again = g->refit(samples);
        QVERIFY(again && again->comps.size() == 3);
    }

    void slicCountsCompactnessAndEdges() {
        Rng rng(11);
        const std::size_t w = 120, h = 120;
        RgbImage img = from_fn(w, h, [](std::size_t x, std::size_t) { return x < 60 ? segment::Rgb{0.8f, 0.3f, 0.2f} : segment::Rgb{0.2f, 0.5f, 0.8f}; });
        for (segment::Rgb& p : img.px)
            for (float& v : p) v = std::clamp(v + 0.02f * rng.normal(), 0.0f, 1.0f);
        const segment::Superpixels sp = segment::slic(img, 64, 10, 10);
        QVERIFY2(sp.count >= 40 && sp.count <= 90, qPrintable(QString::number(sp.count)));
        std::vector<std::array<std::size_t, 4>> box(sp.count, {SIZE_MAX, SIZE_MAX, 0, 0});
        std::vector<std::size_t> left(sp.count, 0), total(sp.count, 0);
        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < w; ++x) {
                const std::size_t l = sp.labels[y * w + x];
                box[l] = {std::min(box[l][0], x), std::min(box[l][1], y), std::max(box[l][2], x), std::max(box[l][3], y)};
                ++total[l];
                if (x < 60) ++left[l];
            }
        }
        for (std::size_t l = 0; l < sp.count; ++l) {
            QVERIFY(total[l] > 0);
            QVERIFY(static_cast<float>(box[l][2] - box[l][0] + 1) <= 3.0f * sp.step && static_cast<float>(box[l][3] - box[l][1] + 1) <= 3.0f * sp.step);
            const float purity = static_cast<float>(std::max(left[l], total[l] - left[l])) / static_cast<float>(total[l]);
            QVERIFY2(purity > 0.95f, qPrintable(QString::number(purity)));
        }
    }

    // --- GrabCut and Object Selection ---

    void grabcutTrimapOnDisc() {
        const auto [img, truth] = disc_scene(120, 100, 60, 50, 30, 1);
        std::vector<std::uint8_t> tri(img.w * img.h, segment::kBg);
        for (std::size_t y = 12; y < 88; ++y)
            for (std::size_t x = 22; x < 98; ++x) tri[y * img.w + x] = segment::kPrFg;
        QVERIFY(segment::grabcut(img, tri, 8));
        std::size_t inter = 0, uni = 0;
        for (std::size_t i = 0; i < tri.size(); ++i) {
            const bool a = tri[i] == segment::kPrFg || tri[i] == segment::kFg;
            inter += a && truth[i] ? 1 : 0;
            uni += a || truth[i] ? 1 : 0;
        }
        const float v = static_cast<float>(inter) / static_cast<float>(uni);
        QVERIFY2(v > 0.95f, qPrintable(QString::number(v)));
    }

    void objectSelectDiscAtFullResolution() {
        const auto [img, truth] = disc_scene(200, 160, 100, 80, 50, 2);
        const segment::ImageSampler s(img);
        const auto reg = segment::object_select(s, Rect{0, 0, 200, 160}, Rect{40, 20, 160, 140}, 1'000'000);
        QVERIFY(reg);
        const float v = iou(*reg, truth, 200, 160);
        QVERIFY2(v > 0.95f, qPrintable(QString::number(v)));
    }

    void objectSelectDownsampledWithTheBandCutAgain() {
        const auto [img, truth] = disc_scene(640, 480, 330, 250, 150, 3);
        const segment::ImageSampler s(img);
        // (a small working budget: a step past 1 and the band cut again at full resolution)
        const auto reg = segment::object_select(s, Rect{0, 0, 640, 480}, Rect{160, 80, 500, 420}, 20'000);
        QVERIFY(reg);
        const float v = iou(*reg, truth, 640, 480);
        QVERIFY2(v > 0.95f, qPrintable(QString::number(v)));
        int far_wrong = 0;
        for (int y = 0; y < 480; ++y) {
            for (int x = 0; x < 640; ++x) {
                const float dx = static_cast<float>(x) + 0.5f - 330, dy = static_cast<float>(y) + 0.5f - 250;
                const float d = std::sqrt(dx * dx + dy * dy) - 150;
                if (std::abs(d) > 2 && (reg->at(x, y) >= 0.5f) != truth[static_cast<std::size_t>(y) * 640 + static_cast<std::size_t>(x)]) ++far_wrong;
            }
        }
        QVERIFY2(far_wrong < 300, qPrintable(QString::number(far_wrong)));
    }

    // --- Quick Selection ---

    void quickSelectGrowsWithoutLeaking() {
        const RgbImage img = two_regions(140, 100);
        const segment::ImageSampler s(img);
        const auto reg = segment::quick_select(s, Rect{0, 0, 140, 100}, {{20, 50}, {35, 50}}, 8, segment::kQuickWorkPx);
        QVERIFY(reg);
        int inside = 0;
        for (int y = 0; y < 100; ++y) {
            for (int x = 0; x < 140; ++x) {
                const float v = reg->at(x, y);
                if (x >= 62) QVERIFY2(v < 0.5f, qPrintable(QStringLiteral("leaked at %1,%2").arg(x).arg(y)));
                if (x < 58 && v >= 0.5f) ++inside;
            }
        }
        QVERIFY2(static_cast<float>(inside) > 0.97f * 58 * 100, qPrintable(QString::number(inside)));
    }

    void quickSelectStopsAtAThinLine() {
        Rng rng(9);
        const RgbImage img = from_fn(120, 80, [&](std::size_t x, std::size_t) {
            const float n = 0.03f * rng.normal();
            return x == 60 || x == 61 ? segment::Rgb{0.05f, 0.05f, 0.05f} : segment::Rgb{0.6f + n, 0.6f + n, 0.55f + n};
        });
        const segment::ImageSampler s(img);
        const auto reg = segment::quick_select(s, Rect{0, 0, 120, 80}, {{30, 40}}, 6, segment::kQuickWorkPx);
        QVERIFY(reg);
        QVERIFY(reg->at(10, 10) >= 0.5f && reg->at(50, 70) >= 0.5f);
        for (int y = 0; y < 80; ++y)
            for (int x = 63; x < 120; ++x) QVERIFY2(reg->at(x, y) < 0.5f, qPrintable(QStringLiteral("crossed at %1,%2").arg(x).arg(y)));
    }

    void quickSelectInAReducedWindow() {
        const auto [img, truth] = disc_scene(600, 500, 300, 250, 180, 4);
        const segment::ImageSampler s(img);
        const auto reg = segment::quick_select(s, Rect{0, 0, 600, 500}, {{260, 250}, {340, 250}}, 30, 15'000);
        QVERIFY(reg);
        const float v = iou(*reg, truth, 600, 500);
        QVERIFY2(v > 0.93f, qPrintable(QString::number(v)));
    }

    // --- Select Subject, Focus Area, Select Sky ---

    void selectSubjectFindsTheCentredObject() {
        const auto [img, truth] = disc_scene(200, 150, 100, 75, 40, 6);
        const segment::ImageSampler s(img);
        const auto reg = segment::select_subject(s, Rect{0, 0, 200, 150});
        QVERIFY(reg);
        const float v = iou(*reg, truth, 200, 150);
        QVERIFY2(v > 0.9f, qPrintable(QString::number(v)));
    }

    void saliencyHighlightsTheObject() {
        const auto [img, truth] = disc_scene(120, 90, 60, 45, 22, 8);
        const std::vector<float> sal = segment::saliency(img);
        double fi = 0, fn = 0, bi = 0, bn = 0;
        for (std::size_t i = 0; i < sal.size(); ++i) {
            if (truth[i]) {
                fi += sal[i];
                fn += 1;
            } else {
                bi += sal[i];
                bn += 1;
            }
        }
        QVERIFY2(fi / fn > 0.6 && bi / bn < 0.25, qPrintable(QStringLiteral("fg %1 bg %2").arg(fi / fn).arg(bi / bn)));
    }

    void focusAreaSelectsTheSharpHalf() {
        Rng rng(12);
        const std::size_t w = 160, h = 100;
        std::vector<float> noise(w * h);
        for (float& v : noise) v = rng.f32();
        const std::vector<float> blurred = matting::box_mean(noise, w, h, 4);
        const RgbImage img = from_fn(w, h, [&](std::size_t x, std::size_t y) {
            const float v = x < 80 ? noise[y * w + x] : blurred[y * w + x];
            return segment::Rgb{v, v * 0.8f, 0.3f};
        });
        const segment::ImageSampler s(img);
        const auto reg = segment::focus_area(s, Rect{0, 0, static_cast<int>(w), static_cast<int>(h)}, 0.5f, 0.0f);
        QVERIFY(reg);
        std::size_t l = 0, r = 0;
        for (int y = 0; y < static_cast<int>(h); ++y)
            for (int x = 0; x < static_cast<int>(w); ++x)
                if (reg->at(x, y) >= 0.5f) (x < 80 ? l : r) += 1;
        QVERIFY2(static_cast<float>(l) > 0.85f * 80 * h, qPrintable(QString::number(l)));
        QVERIFY2(static_cast<float>(r) < 0.1f * 80 * h, qPrintable(QString::number(r)));
    }

    void skyFoundNotTheGroundOrTheTree() {
        // a blue gradient sky over textured ground, a dark round crown on a trunk reaching into it
        const std::size_t w = 160, h = 100, horizon = h * 3 / 5;
        const RgbImage img = from_fn(w, h, [&](std::size_t x, std::size_t y) {
            const float cx = static_cast<float>(w) * 0.7f, cy = static_cast<float>(h) * 0.35f, rr = static_cast<float>(h) * 0.18f;
            const float fx = static_cast<float>(x), fy = static_cast<float>(y);
            const bool crown = std::sqrt((fx - cx) * (fx - cx) + (fy - cy) * (fy - cy)) < rr;
            const bool trunk = std::abs(fx - cx) < 3.0f && fy > cy && y < horizon;
            const float n = (static_cast<float>((x * 7919 + y * 104'729) % 97) / 97.0f - 0.5f) * 0.25f;
            if (crown) return segment::Rgb{0.1f + n * 0.5f, 0.35f + n, 0.1f + n * 0.5f};
            if (trunk) return segment::Rgb{0.3f + n * 0.3f, 0.2f, 0.1f};
            if (y < horizon) {
                const float t = fy / static_cast<float>(horizon);
                return segment::Rgb{0.25f + 0.4f * t, 0.5f + 0.3f * t, 0.9f + 0.05f * t};
            }
            return segment::Rgb{0.35f + n, 0.3f + n, 0.15f + n * 0.5f};
        });
        const segment::ImageSampler s(img);
        const auto r = segment::select_sky(s, Rect{0, 0, 160, 100}, segment::SkyParams{});
        QVERIFY(r);
        QVERIFY(r->at(10, 5) > 0.9f);
        QVERIFY(r->at(80, 50) > 0.9f);
        QVERIFY(r->at(10, 80) < 0.1f);
        QVERIFY(r->at(112, 35) < 0.1f);
        QVERIFY(r->box.y1 <= 64);
    }

    void noSkyOnGroundAlone() {
        const RgbImage img = from_fn(64, 64, [](std::size_t x, std::size_t y) {
            const float n = (static_cast<float>((x * 7919 + y * 104'729) % 97) / 97.0f - 0.5f) * 0.3f;
            return segment::Rgb{0.35f + n, 0.3f + n, 0.12f};
        });
        QVERIFY(!segment::select_sky(segment::ImageSampler(img), Rect{0, 0, 64, 64}, segment::SkyParams{}));
    }

    void anOvercastSkyIsFound() {
        const RgbImage img = from_fn(64, 64, [](std::size_t, std::size_t y) { return y < 30 ? segment::Rgb{0.85f, 0.86f, 0.88f} : segment::Rgb{0.2f, 0.3f, 0.1f}; });
        const auto r = segment::select_sky(segment::ImageSampler(img), Rect{0, 0, 64, 64}, segment::SkyParams{});
        QVERIFY(r);
        QVERIFY(r->at(30, 10) > 0.9f && r->at(30, 50) < 0.1f);
        QVERIFY(segment::colour_prior({0.4f, 0.6f, 0.95f}) > 0.8f);
        QVERIFY(segment::colour_prior({0.15f, 0.4f, 0.15f}) < 0.1f);
        QVERIFY(segment::colour_prior({0.9f, 0.9f, 0.92f}) > 0.8f);
    }

    // --- the helpers ---

    void theSamplersDownsampling() {
        const RgbImage img = from_fn(37, 29, [](std::size_t x, std::size_t y) {
            return segment::Rgb{static_cast<float>(x) / 37.0f, static_cast<float>(y) / 29.0f, static_cast<float>((x * y) % 7) / 7.0f};
        });
        const segment::ImageSampler s(img, {5, 3});
        const RgbImage a = s.rgb_scaled(Rect{5, 3, 42, 32}, 4);
        const RgbImage b = img.downsample(4);
        QCOMPARE(a.w, b.w);
        QCOMPARE(a.h, b.h);
        for (std::size_t i = 0; i < a.px.size(); ++i)
            for (std::size_t c = 0; c < 3; ++c) QVERIFY(std::abs(a.px[i][c] - b.px[i][c]) < 1e-5f);
    }

    void cleanMaskDropsIslandsAndFillsHoles() {
        const std::size_t w = 40, h = 40;
        segment::Mask m(w * h, 0);
        for (std::size_t y = 5; y < 30; ++y)
            for (std::size_t x = 5; x < 30; ++x) m[y * w + x] = 1;
        m[15 * w + 15] = 0;  // (a hole)
        m[36 * w + 36] = 1;  // (an island)
        const segment::Mask c = segment::clean_mask(m, w, h, 0.1f, 0.05f);
        QVERIFY(c[15 * w + 15] != 0);
        QVERIFY(c[36 * w + 36] == 0);
    }

    void geodesicDistanceJumpsAcrossEdges() {
        const RgbImage img = two_regions(140, 100);
        const segment::Mask seeds = segment::stroke_mask({{20, 50}}, 4, 140, 100);
        const std::vector<float> d = segment::geodesic(img, seeds);
        QVERIFY2(d[50 * 140 + 40] < 0.02f, qPrintable(QString::number(d[50 * 140 + 40])));
        QVERIFY2(d[50 * 140 + 100] > 0.1f, qPrintable(QString::number(d[50 * 140 + 100])));
    }

    void geodesicBarrierBlocksObliqueCrossings() {
        // (a large disc whose slightly tilted edge crosses the picture: paths along the soft edge must not sneak across)
        const auto [img, truth] = disc_scene(352, 372, -470, 186, 668, 4);
        const segment::Mask seeds = segment::stroke_mask({{175, 176}, {175, 196}}, 5, 352, 372);
        for (std::size_t i = 0; i < seeds.size(); ++i) QVERIFY(seeds[i] == 0 || truth[i]);
        const std::vector<float> d = segment::geodesic(img, seeds);
        QVERIFY(d[186 * 352 + 100] < 0.02f);
        for (std::size_t y = 0; y < 372; y += 31) QVERIFY2(d[y * 352 + 291] > 0.1f, qPrintable(QString::number(d[y * 352 + 291])));
    }

    // The same picture, the same bytes; a stop asked for stops the work (render::Cancelled) and the thread's work goes on
    // after the scope.
    void theSameAgainAndStopped() {
        const auto [img, truth] = disc_scene(200, 150, 100, 75, 40, 6);
        const segment::ImageSampler s(img);
        const auto a = segment::select_subject(s, Rect{0, 0, 200, 150});
        const auto b = segment::select_subject(s, Rect{0, 0, 200, 150});
        QVERIFY(a && b);
        QCOMPARE(a->box, b->box);
        QVERIFY(a->mask == b->mask);
        std::stop_source stop;
        stop.request_stop();
        bool stopped = false;
        try {
            const segment::StopScope scope(stop.get_token());
            (void)segment::select_subject(s, Rect{0, 0, 200, 150});
        } catch (const Cancelled&) {
            stopped = true;
        }
        QVERIFY(stopped);
        QVERIFY(segment::select_subject(s, Rect{0, 0, 200, 150}));
    }
};

QTEST_GUILESS_MAIN(TestSegment)
#include "test_segment.moc"
