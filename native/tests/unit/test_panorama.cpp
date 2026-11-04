// PC5 I-g (SPEC.md AUTO-01「自動整列・自動合成」「Photomerge」): render/panorama and render/pyramid as PhotoCraft's
// algo/src/panorama.rs and pyramid.rs test them — three crops of one scene placed again by the reposition, collage and
// perspective layouts, a projective view recovered, a turning camera's focal length and its cylindrical layout, placements that
// go and come back in every projection, a gain and no vignetting found, a seam that goes round what moved, a multi-band blend
// without a step; the Laplacian pyramid's round trip, a one-hot blend's two sides and its ramp, focus stacking's sharp picture,
// the panorama weights' seam in the overlap — and what this build asks more: the placements within 0.5 px, the same result again
// (the seeds fixed), the tiled blend (1024 px tiles) as the whole one.

#include <QtTest>

#include <cmath>
#include <numbers>
#include <vector>

#include "render/panorama.hpp"
#include "render/pyramid.hpp"

using namespace genko;
namespace pano = render::panorama;
namespace pyr = render::pyramid;
using pano::Homography;
using pano::XY;

namespace {

// A smooth random texture: blobs over a gradient (upstream's scene).
std::vector<float> scene(std::size_t w, std::size_t h, std::uint64_t seed) {
    render::features::Rng rng{seed};
    std::vector<float> img(w * h);
    for (std::size_t i = 0; i < w * h; ++i) img[i] = 0.25f + 0.2f * (static_cast<float>(i % w) / static_cast<float>(w));
    const std::size_t count = std::max<std::size_t>(w * h / 900, 40);
    for (std::size_t k = 0; k < count; ++k) {
        const double cx = static_cast<double>(rng.next() % w), cy = static_cast<double>(rng.next() % h);
        const double r = 3.0 + static_cast<double>(rng.next() % 9);
        const float v = static_cast<float>(rng.next() % 1000) / 1000.0f;
        const bool sq = rng.next() % 2 == 0;
        const std::size_t x0 = static_cast<std::size_t>(std::max(cx - r, 0.0)), x1 = std::min(static_cast<std::size_t>(cx + r) + 1, w);
        const std::size_t y0 = static_cast<std::size_t>(std::max(cy - r, 0.0)), y1 = std::min(static_cast<std::size_t>(cy + r) + 1, h);
        for (std::size_t y = y0; y < y1; ++y) {
            for (std::size_t x = x0; x < x1; ++x) {
                const double dx = static_cast<double>(x) - cx, dy = static_cast<double>(y) - cy;
                if (sq ? std::fabs(dx) < r && std::fabs(dy) < r * 0.6 : std::hypot(dx, dy) < r) img[y * w + x] = v;
            }
        }
    }
    return img;
}

float bilinear(const std::vector<float>& img, std::size_t w, std::size_t h, double x, double y) {
    const std::size_t x0 = static_cast<std::size_t>(std::floor(x)), y0 = static_cast<std::size_t>(std::floor(y));
    const std::size_t x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
    const float tx = static_cast<float>(x - static_cast<double>(x0)), ty = static_cast<float>(y - static_cast<double>(y0));
    const float top = img[y0 * w + x0] * (1 - tx) + img[y0 * w + x1] * tx;
    const float bot = img[y1 * w + x0] * (1 - tx) + img[y1 * w + x1] * tx;
    return top * (1 - ty) + bot * ty;
}

// tw × th views of a scene through `map` (view px → scene px).
template <class Map>
std::vector<float> view(const std::vector<float>& sc, std::size_t sw, std::size_t sh, std::size_t tw, std::size_t th, Map map) {
    std::vector<float> out(tw * th, 0.0f);
    for (std::size_t i = 0; i < tw * th; ++i) {
        const auto [x, y] = map(static_cast<double>(i % tw), static_cast<double>(i / tw));
        if (x < 0.0 || y < 0.0 || x > static_cast<double>(sw - 1) || y > static_cast<double>(sh - 1)) continue;
        out[i] = bilinear(sc, sw, sh, x, y);
    }
    return out;
}

}  // namespace

class TestPanorama : public QObject {
    Q_OBJECT

private slots:
    void recoversTranslationsAndHomographies() {
        const std::size_t sw = 640, sh = 300, tw = 260, th = 220;
        const std::vector<float> sc = scene(sw, sh, 3);
        const std::array<XY, 3> offs{XY{20.0, 30.0}, XY{190.0, 40.0}, XY{360.0, 25.0}};
        std::vector<pano::ImageFeatures> feats;
        for (const XY& o : offs) {
            const auto img = view(sc, sw, sh, tw, th, [&](double x, double y) { return std::pair{x + o[0], y + o[1]}; });
            feats.push_back(pano::detect(tw, th, img, nullptr, 700));
        }
        const auto matches = pano::match_pairs(feats);
        QVERIFY(matches.size() >= 2);
        for (const pano::Layout layout : {pano::Layout::Reposition, pano::Layout::Collage, pano::Layout::Perspective}) {
            pano::AlignOptions o;
            o.layout = layout;
            o.reference = 1;
            const auto a = pano::align(feats, matches, o);
            QVERIFY(a);
            QCOMPARE(a->reference, std::size_t(1));
            for (std::size_t i = 0; i < 3; ++i) {
                QVERIFY(a->placements[i]);
                // (image px (50, 60) is scene (50 + ox, 60 + oy); against image 1)
                const XY u = a->placements[i]->forward(50.0, 60.0);
                const double eu = 50.0 + offs[i][0] - offs[1][0], ev = 60.0 + offs[i][1] - offs[1][1];
                QVERIFY2(std::fabs(u[0] - eu) < 0.5 && std::fabs(u[1] - ev) < 0.5,
                         qPrintable(QStringLiteral("%1 image %2: %3,%4 vs %5,%6").arg(QString::fromLatin1(pano::layout_name(layout).data())).arg(i).arg(u[0]).arg(u[1]).arg(eu).arg(ev)));
            }
            QVERIFY2(a->rms < 1.0, qPrintable(QString::number(a->rms)));
            // (the same again: the seeds are fixed)
            const auto again = pano::align(feats, pano::match_pairs(feats), o);
            QVERIFY(again);
            for (std::size_t i = 0; i < 3; ++i) QVERIFY(again->placements[i]->h.m == a->placements[i]->h.m);
        }
        // (a projective view, recovered by the perspective layout, inside the overlap)
        const Homography hq{{1.05, 0.03, 180.0, -0.02, 1.0, 35.0, 0.0001, -0.00005, 1.0}};
        const auto im2 = view(sc, sw, sh, tw, th, [&](double x, double y) { return hq.apply(x, y); });
        std::vector<pano::ImageFeatures> feats2{feats[0], pano::detect(tw, th, im2, nullptr, 700)};
        const auto m2 = pano::match_pairs(feats2);
        pano::AlignOptions o;
        o.layout = pano::Layout::Perspective;
        o.reference = 0;
        const auto a = pano::align(feats2, m2, o);
        QVERIFY(a && a->placements[1]);
        for (const XY& p : {XY{20.0, 30.0}, XY{70.0, 180.0}, XY{40.0, 100.0}}) {
            const auto [sx, sy] = hq.apply(p[0], p[1]);
            const XY u = a->placements[1]->forward(p[0], p[1]);
            QVERIFY2(std::fabs(u[0] - (sx - 20.0)) < 1.5 && std::fabs(u[1] - (sy - 30.0)) < 1.5, qPrintable(QStringLiteral("%1,%2").arg(u[0]).arg(u[1])));
        }
    }

    void turningCameraFocalAndCylindricalLayout() {
        // (views of a scene on a cylinder from a camera panning by 25° at f = 300 px)
        const double f = 300.0;
        const std::size_t sw = 1400, sh = 500, tw = 320, th = 240;
        const std::vector<float> sc = scene(sw, sh, 9);
        std::vector<pano::ImageFeatures> feats;
        for (const double deg : {-25.0, 0.0, 25.0}) {
            const auto img = view(sc, sw, sh, tw, th, [&](double x, double y) {
                const double dx = x - static_cast<double>(tw) / 2.0, dy = y - static_cast<double>(th) / 2.0;
                const double t = std::atan2(dx, f) + deg * std::numbers::pi / 180.0;
                const double hh = dy / std::hypot(dx, f);
                return std::pair{static_cast<double>(sw) / 2.0 + f * t, static_cast<double>(sh) / 2.0 + f * hh};
            });
            feats.push_back(pano::detect(tw, th, img, nullptr, 700));
        }
        const auto matches = pano::match_pairs(feats);
        int adjacent = 0;
        for (const auto& m : matches) adjacent += m.b == m.a + 1 ? 1 : 0;
        QCOMPARE(adjacent, 2);
        bool near = false;
        for (const auto& m : matches) {
            if (const auto e = pano::focal_from_homography(m.homography, XY{160.0, 120.0}, XY{160.0, 120.0})) near = near || std::fabs(*e - f) / f < 0.15;
        }
        QVERIFY(near);
        pano::AlignOptions o;
        o.layout = pano::Layout::Cylindrical;
        o.reference = 1;
        const auto a = pano::align(feats, matches, o);
        QVERIFY(a);
        QVERIFY2(std::fabs(a->focal - f) / f < 0.1, qPrintable(QString::number(a->focal)));
        const auto& p = *a->placements[2];
        const XY u = p.forward(160.0, 120.0);
        const double expect = 160.0 + a->focal * 25.0 * std::numbers::pi / 180.0;
        QVERIFY2(std::fabs(u[0] - expect) < 4.0, qPrintable(QStringLiteral("%1 vs %2").arg(u[0]).arg(expect)));
        QVERIFY2(a->rms < 1.5, qPrintable(QString::number(a->rms)));
        const XY q = p.forward(40.0, 200.0);
        const auto back = p.inverse(q[0], q[1]);
        QVERIFY(back && std::fabs((*back)[0] - 40.0) < 1e-6 && std::fabs((*back)[1] - 200.0) < 1e-6);
    }

    void placementsRoundTripEveryProjection() {
        for (const pano::Projection projection : {pano::Projection::Plane, pano::Projection::Cylinder, pano::Projection::Sphere}) {
            pano::Placement p;
            p.projection = projection;
            p.focal = 400.0;
            p.center = XY{200.0, 150.0};
            p.k1 = 0.05;
            p.h = Homography{{0.98, -0.1, 30.0, 0.1, 0.98, -12.0, 0.0, 0.0, 1.0}};
            for (const XY& q : {XY{0.0, 0.0}, XY{390.0, 20.0}, XY{123.0, 280.0}}) {
                const XY u = p.forward(q[0], q[1]);
                const auto b = p.inverse(u[0], u[1]);
                QVERIFY(b && std::fabs((*b)[0] - q[0]) < 1e-6 && std::fabs((*b)[1] - q[1]) < 1e-6);
            }
            const pano::Placement s = p.scaled(2.0);
            const XY u = p.forward(100.0, 50.0), u2 = s.forward(200.0, 100.0);
            QVERIFY(std::fabs(u2[0] - 2.0 * u[0]) < 1e-6 && std::fabs(u2[1] - 2.0 * u[1]) < 1e-6);
        }
        // (a plane without distortion has a matrix; the others none)
        pano::Placement plane;
        plane.center = XY{10.0, 20.0};
        plane.h = Homography{{1.0, 0.0, 15.0, 0.0, 1.0, 25.0, 0.0, 0.0, 1.0}};
        const auto m = pano::plane_matrix(plane);
        QVERIFY(m);
        QCOMPARE(m->apply(0.0, 0.0).first, 5.0);
        plane.projection = pano::Projection::Cylinder;
        QVERIFY(!pano::plane_matrix(plane));
    }

    void photometricFindsGainNoVignette() {
        // (two half-overlapping views of a flat-ish scene, the second 30 % brighter)
        const std::size_t w = 200, h = 100;
        const auto base = [](int x, int y) { return 0.3f + 0.2f * std::fabs(std::sin(static_cast<float>(x) * 0.05f) * std::cos(static_cast<float>(y) * 0.07f)); };
        std::vector<pano::RoiImage> imgs;
        std::vector<pano::Placement> pls;
        for (const auto& [x0, g] : {std::pair{0, 1.0f}, std::pair{100, 1.3f}}) {
            pano::RoiImage im;
            im.x0 = x0;
            im.w = w;
            im.h = h;
            im.ch = 1;
            for (int y = 0; y < static_cast<int>(h); ++y)
                for (int x = 0; x < static_cast<int>(w); ++x) im.px.push_back(base(x + x0, y) * g);
            im.alpha.assign(w * h, 1.0f);
            imgs.push_back(im);
            pano::Placement p;
            p.focal = 200.0;
            p.center = XY{100.0, 50.0};
            p.h = Homography{{1.0, 0.0, 100.0 + x0, 0.0, 1.0, 50.0, 0.0, 0.0, 1.0}};
            pls.push_back(p);
        }
        const std::vector<std::pair<std::size_t, std::size_t>> sizes{{w, h}, {w, h}};
        const auto ph = pano::photometric(imgs, &pls, &sizes, true, false);
        QVERIFY2(std::fabs(ph.gains[1][0] / ph.gains[0][0] - 1.3f) < 0.03f, qPrintable(QString::number(ph.gains[1][0] / ph.gains[0][0])));
        const auto pv = pano::photometric(imgs, &pls, &sizes, true, true);
        QVERIFY(std::fabs(pv.vignette[0]) < 0.1);
    }

    void seamsGoRoundWhatMovedAndBlendHasNoStep() {
        // (two images over a 120 × 40 strip overlapping in 40..80; the second has a bright thing at 50..58 the first has not)
        const std::size_t w = 120, h = 40;
        std::vector<std::vector<float>> luma{std::vector<float>(w * h, 0.5f), std::vector<float>(w * h, 0.5f)};
        std::vector<std::vector<bool>> cover(2, std::vector<bool>(w * h));
        for (std::size_t i = 0; i < w * h; ++i) {
            cover[0][i] = i % w < 80;
            cover[1][i] = i % w >= 40;
        }
        for (std::size_t y = 0; y < h; ++y)
            for (std::size_t x = 50; x < 58; ++x) luma[1][y * w + x] = 1.0f;
        const auto labels = pano::seam_labels(w, h, luma, cover, {0, 1});
        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 50; x < 58; ++x) QCOMPARE(labels[y * w + x], labels[y * w + 50]);
        }
        QCOMPARE(labels[5], 0);
        QCOMPARE(labels[w - 1], 1);
        const auto mk = [&](int x0, float v) {
            pano::RoiImage im;
            im.x0 = x0;
            im.w = 80;
            im.h = h;
            im.ch = 1;
            im.px.assign(80 * h, v);
            im.alpha.assign(80 * h, 1.0f);
            return im;
        };
        const std::vector<pano::RoiImage> imgs{mk(0, 0.3f), mk(40, 0.7f)};
        std::vector<std::vector<float>> wts(2, std::vector<float>(80 * h));
        for (std::size_t k = 0; k < 2; ++k) {
            for (std::size_t i = 0; i < 80 * h; ++i) {
                const int x = static_cast<int>(i % 80) + imgs[k].x0;
                wts[k][i] = (x < 60) == (k == 0) ? 1.0f : 0.0f;
            }
        }
        const auto [out, cov] = pano::multiband(w, h, imgs, wts, 4);
        std::vector<float> row(w);
        for (std::size_t x = 0; x < w; ++x) row[x] = out[20 * w + x];
        QVERIFY(std::fabs(row[2] - 0.3f) < 0.02f && std::fabs(row[w - 3] - 0.7f) < 0.02f);
        float jump = 0.0f;
        for (std::size_t x = 1; x < w; ++x) jump = std::max(jump, std::fabs(row[x] - row[x - 1]));
        QVERIFY2(jump < 0.1f, qPrintable(QString::number(jump)));
        for (const float c : cov) QCOMPARE(c, 1.0f);
    }

    // --- the pyramid (pyramid.rs's tests) ---

    void laplacianRoundTripIsExact() {
        const std::size_t w = 37, h = 23;
        std::vector<float> img(w * h * 2);
        for (std::size_t i = 0; i < img.size(); ++i) img[i] = static_cast<float>(i * 37 % 101) / 101.0f;
        const auto p = pyr::laplacian(w, h, 2, img, 5);
        QCOMPARE(p.size(), std::size_t(5));
        const auto back = pyr::collapse(p, 2);
        for (std::size_t i = 0; i < img.size(); ++i) QVERIFY(std::fabs(img[i] - back[i]) < 1e-4f);
    }

    void oneHotBlendKeepsEachSideAndRamps() {
        const std::size_t w = 64, h = 32;
        const std::vector<float> a(w * h, 0.2f), b(w * h, 0.8f);
        std::vector<float> wa(w * h), wb(w * h);
        for (std::size_t i = 0; i < w * h; ++i) {
            wa[i] = i % w < 32 ? 1.0f : 0.0f;
            wb[i] = 1.0f - wa[i];
        }
        const auto out = pyr::blend(w, h, 1, {&a, &b}, {&wa, &wb}, pyr::auto_levels(w, h));
        QVERIFY(std::fabs(out[5 * w + 2] - 0.2f) < 1e-3f);
        QVERIFY(std::fabs(out[5 * w + 61] - 0.8f) < 1e-3f);
        const float mid = out[5 * w + 31];
        QVERIFY(mid > 0.3f && mid < 0.7f);
    }

    void stackPicksTheSharpPicture() {
        const std::size_t w = 48, h = 48;
        const auto checker = [](std::size_t x, std::size_t y) { return (x / 2 + y / 2) % 2 == 0 ? 0.1f : 0.9f; };
        std::vector<float> i0(w * h), i1(w * h);
        for (std::size_t i = 0; i < w * h; ++i) {
            i0[i] = i % w < 24 ? checker(i % w, i / w) : 0.5f;
            i1[i] = i % w >= 24 ? checker(i % w, i / w) : 0.5f;
        }
        const std::vector<float> ones(w * h, 1.0f);
        const auto wts = pyr::stack_weights(w, h, {i0, i1}, {ones, ones});
        QCOMPARE(wts[0][20 * w + 4], 1.0f);
        QCOMPARE(wts[1][20 * w + 44], 1.0f);
    }

    void panoramaWeightsSplitTheOverlap() {
        const std::size_t w = 60, h = 20;
        std::vector<float> a(w * h), b(w * h);
        for (std::size_t i = 0; i < w * h; ++i) {
            a[i] = i % w < 40 ? 1.0f : 0.0f;
            b[i] = i % w >= 20 ? 1.0f : 0.0f;
        }
        const auto wts = pyr::panorama_weights(w, h, {a, b});
        QCOMPARE(wts[0][10 * w + 5], 1.0f);
        QCOMPARE(wts[1][10 * w + 55], 1.0f);
        std::size_t seam = 0;
        while (seam < w && wts[1][10 * w + seam] != 1.0f) ++seam;
        QVERIFY2(seam >= 25 && seam <= 35, qPrintable(QString::number(seam)));
    }

    // The blend a tile at a time (Genko's: 1024 px, an overlap of 5·2^levels) as the whole one.
    void tiledBlendIsTheWhole() {
        const std::size_t w = 2300, h = 1100, levels = 6;
        const std::vector<float> a = scene(w, h, 5), b = scene(w, h, 6);
        std::vector<float> wa(w * h), wb(w * h);
        for (std::size_t i = 0; i < w * h; ++i) {
            wa[i] = (i % w) < 1200 ? 1.0f : 0.0f;
            wb[i] = 1.0f - wa[i];
        }
        const auto whole = pyr::blend(w, h, 1, {&a, &b}, {&wa, &wb}, levels);
        std::vector<float> tiled(w * h, -1.0f);
        const std::vector<const std::vector<float>*> px{&a, &b}, wt{&wa, &wb};
        const auto part = [w](const std::vector<const std::vector<float>*>& from) {
            return [w, from](std::size_t i, std::size_t x0, std::size_t y0, std::size_t rw, std::size_t rh, std::vector<float>& out) {
                for (std::size_t y = 0; y < rh; ++y)
                    for (std::size_t x = 0; x < rw; ++x) out[y * rw + x] = (*from[i])[(y0 + y) * w + x0 + x];
            };
        };
        pyr::blend_tiled(w, h, 1, 2, part(px), part(wt), levels, true,
                         [&](std::size_t x0, std::size_t y0, std::size_t cw, std::size_t ch, const std::vector<float>& core) {
                             for (std::size_t y = 0; y < ch; ++y)
                                 for (std::size_t x = 0; x < cw; ++x) tiled[(y0 + y) * w + x0 + x] = core[y * cw + x];
                         });
        float worst = 0.0f;
        for (std::size_t i = 0; i < w * h; ++i) worst = std::max(worst, std::fabs(tiled[i] - whole[i]));
        QVERIFY2(worst < 1.0f / 255.0f, qPrintable(QString::number(worst)));
        QCOMPARE(pyr::tile_overlap(6), std::size_t(320));
    }
};

QTEST_GUILESS_MAIN(TestPanorama)
#include "test_panorama.moc"
