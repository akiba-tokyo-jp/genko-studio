// PC5 I-g (SPEC.md AUTO-01「自動整列・自動合成」): the registration of レイヤーを自動整列 and Photomerge — render/features, as
// PhotoCraft's algo/src/features.rs tests it (Harris finds a square's corners, the fits exact on clean data, RANSAC leaves the
// outliers out, a translation and a similarity registered, the descriptors the same again) — and what this build asks more
// (design 節 I 6 章 I-g): pairs cut from one picture by a known translation, turn and homography come back within 0.5 px, and
// the same seed gives the same result.

#include <QtTest>

#include <cmath>
#include <numbers>
#include <vector>

#include "render/features.hpp"

using namespace genko;
namespace ft = render::features;
using ft::Homography;
using ft::XY;

namespace {

// A deterministic texture: blobs of brightness (upstream's test texture).
std::vector<float> texture(std::size_t w, std::size_t h, std::uint64_t seed = 42) {
    ft::Rng rng{seed};
    std::vector<float> img(w * h, 0.3f);
    for (int k = 0; k < 140; ++k) {
        const double cx = rng.unit() * static_cast<double>(w), cy = rng.unit() * static_cast<double>(h);
        const double r = 3.0 + rng.unit() * 9.0;
        const float v = static_cast<float>(rng.unit());
        const bool sq = rng.unit() > 0.5;
        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < w; ++x) {
                const double dx = static_cast<double>(x) - cx, dy = static_cast<double>(y) - cy;
                const bool inside = sq ? std::fabs(dx) < r && std::fabs(dy) < r * 0.6 : std::hypot(dx, dy) < r;
                if (inside) img[y * w + x] = v;
            }
        }
    }
    return img;
}

// `img` drawn again through a map (destination → source `inv`), bilinear.
std::vector<float> warp(std::size_t w, std::size_t h, const std::vector<float>& img, const Homography& inv) {
    std::vector<float> out(w * h, 0.3f);
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            const auto [sx, sy] = inv.apply(static_cast<double>(x), static_cast<double>(y));
            if (sx < 0.0 || sy < 0.0 || sx >= static_cast<double>(w - 1) || sy >= static_cast<double>(h - 1)) continue;
            const std::size_t x0 = static_cast<std::size_t>(std::floor(sx)), y0 = static_cast<std::size_t>(std::floor(sy));
            const float tx = static_cast<float>(sx - static_cast<double>(x0)), ty = static_cast<float>(sy - static_cast<double>(y0));
            const auto p = [&](std::size_t xx, std::size_t yy) { return img[yy * w + xx]; };
            const float top = p(x0, y0) + (p(x0 + 1, y0) - p(x0, y0)) * tx;
            const float bot = p(x0, y0 + 1) + (p(x0 + 1, y0 + 1) - p(x0, y0 + 1)) * tx;
            out[y * w + x] = top + (bot - top) * ty;
        }
    }
    return out;
}

// The largest distance between m·truth(p) and p over points well inside the picture.
double undo_error(const Homography& m, const Homography& truth, std::size_t w, std::size_t h) {
    double worst = 0.0;
    for (double fy : {0.25, 0.5, 0.75}) {
        for (double fx : {0.25, 0.5, 0.75}) {
            const double x = fx * static_cast<double>(w), y = fy * static_cast<double>(h);
            const auto [tx, ty] = truth.apply(x, y);
            const auto [bx, by] = m.apply(tx, ty);
            worst = std::max(worst, std::hypot(bx - x, by - y));
        }
    }
    return worst;
}

}  // namespace

class TestFeatures : public QObject {
    Q_OBJECT

private slots:
    void harrisFindsSquareCorners() {
        const std::size_t w = 80, h = 80;
        std::vector<float> img(w * h, 0.0f);
        for (std::size_t y = 30; y < 50; ++y)
            for (std::size_t x = 30; x < 50; ++x) img[y * w + x] = 1.0f;
        const auto c = ft::harris(w, h, img, 10, 5.0f);
        QVERIFY(c.size() >= 4);
        for (const auto& [x, y] : {std::pair{30.0f, 30.0f}, std::pair{49.0f, 49.0f}}) {
            bool near = false;
            for (const auto& k : c) near = near || (std::fabs(k.x - x) <= 2.0f && std::fabs(k.y - y) <= 2.0f);
            QVERIFY2(near, "a corner near the square's");
        }
        // (too small a picture for the patch: none)
        QVERIFY(ft::harris(20, 20, std::vector<float>(400, 0.5f), 10, 5.0f).empty());
    }

    void fitsAreExactOnCleanData() {
        const std::vector<XY> src{{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {5.0, 3.0}};
        const Homography truth{{1.1, 0.05, 3.0, -0.02, 0.95, -2.0, 0.001, 0.0005, 1.0}};
        std::vector<XY> dst;
        for (const XY& p : src) {
            const auto [x, y] = truth.apply(p[0], p[1]);
            dst.push_back(XY{x, y});
        }
        const auto h = ft::fit(ft::Model::Homography, src, dst);
        QVERIFY(h);
        for (const XY& p : src) {
            const auto a = h->apply(p[0], p[1]), b = truth.apply(p[0], p[1]);
            QVERIFY(std::fabs(a.first - b.first) < 1e-6 && std::fabs(a.second - b.second) < 1e-6);
        }
        const Homography sim{{0.9, -0.2, 4.0, 0.2, 0.9, 1.0, 0.0, 0.0, 1.0}};
        dst.clear();
        for (const XY& p : src) {
            const auto [x, y] = sim.apply(p[0], p[1]);
            dst.push_back(XY{x, y});
        }
        const auto s = ft::fit(ft::Model::Similarity, src, dst);
        QVERIFY(s);
        for (std::size_t i = 0; i < 9; ++i) QVERIFY(std::fabs(s->m[i] - sim.m[i]) < 1e-9);
        // (too few points: none)
        QVERIFY(!ft::fit(ft::Model::Homography, {src.begin(), src.begin() + 3}, {dst.begin(), dst.begin() + 3}));
        QVERIFY(!ft::fit(ft::Model::Translation, {}, {}));
    }

    void ransacLeavesOutliersOut() {
        std::vector<XY> src, dst;
        for (int i = 0; i < 40; ++i) {
            const XY p{static_cast<double>(i * 7 % 50), static_cast<double>(i * 13 % 50)};
            src.push_back(p);
            dst.push_back(i % 4 == 0 ? XY{p[0] * 3.0 + 40.0, -p[1]} : XY{p[0] + 12.0, p[1] - 5.0});
        }
        const auto r = ft::ransac(ft::Model::Translation, src, dst, 100, 1.0, 1);
        QVERIFY(r);
        QCOMPARE(r->second.size(), std::size_t(30));
        QVERIFY(std::fabs(r->first.m[2] - 12.0) < 1e-9 && std::fabs(r->first.m[5] + 5.0) < 1e-9);
    }

    // A translation, a similarity (turned 4°, scaled 1.03 about the middle) and a homography undone within 0.5 px.
    void registersKnownMapsWithinHalfAPixel() {
        const std::size_t w = 192, h = 160;
        const std::vector<float> reference = texture(w, h);
        const Homography shift{{1.0, 0.0, 9.0, 0.0, 1.0, -6.0, 0.0, 0.0, 1.0}};
        const auto t = ft::register_pair(w, h, reference, warp(w, h, reference, *shift.inverse()), nullptr, nullptr, ft::Model::Translation);
        QVERIFY(t);
        QVERIFY(t->second >= 6);
        QVERIFY2(std::fabs(t->first.m[2] + 9.0) < 0.5 && std::fabs(t->first.m[5] - 6.0) < 0.5, "the shift undone");
        const double c = std::cos(4.0 * std::numbers::pi / 180.0) * 1.03, s = std::sin(4.0 * std::numbers::pi / 180.0) * 1.03;
        const double cx = static_cast<double>(w) / 2.0, cy = static_cast<double>(h) / 2.0;
        const Homography sim{{c, -s, cx - c * cx + s * cy, s, c, cy - s * cx - c * cy, 0.0, 0.0, 1.0}};
        const auto m = ft::register_pair(w, h, reference, warp(w, h, reference, *sim.inverse()), nullptr, nullptr, ft::Model::Similarity);
        QVERIFY(m);
        QVERIFY2(undo_error(m->first, sim, w, h) < 0.5, qPrintable(QString::number(undo_error(m->first, sim, w, h))));
        const Homography persp{{1.02, 0.03, -4.0, -0.01, 0.99, 3.0, 0.0002, -0.0001, 1.0}};
        const auto hm = ft::register_pair(w, h, reference, warp(w, h, reference, *persp.inverse()), nullptr, nullptr, ft::Model::Homography);
        QVERIFY(hm);
        QVERIFY2(undo_error(hm->first, persp, w, h) < 0.5, qPrintable(QString::number(undo_error(hm->first, persp, w, h))));
    }

    // Where a mask says a picture is not there, no corner is taken.
    void validMaskKeepsCornersOff() {
        const std::size_t w = 160, h = 120;
        const std::vector<float> img = texture(w, h);
        std::vector<bool> valid(w * h, true);
        for (std::size_t y = 0; y < h; ++y)
            for (std::size_t x = 0; x < w / 2; ++x) valid[y * w + x] = false;
        const auto c = ft::harris(w, h, img, 200, 4.0f, &valid);
        QVERIFY(!c.empty());
        for (const auto& k : c) QVERIFY(k.x >= static_cast<float>(w / 2 + ft::kPatchRadius));
    }

    void descriptorsAreDeterministic() {
        const std::size_t w = 96, h = 96;
        const std::vector<float> img = texture(w, h);
        const auto c = ft::harris(w, h, img, 50, 6.0f);
        QVERIFY(ft::describe(w, h, img, c, false) == ft::describe(w, h, img, c, false));
        QCOMPARE(ft::hamming({0, 0, 0, 0}, {1, 3, 0, ~std::uint64_t(0)}), 67u);
        // (the same seed, the same draws: the registration again the same to the last bit)
        const Homography shift{{1.0, 0.0, 5.0, 0.0, 1.0, 3.0, 0.0, 0.0, 1.0}};
        const std::vector<float> moved = warp(w, h, img, *shift.inverse());
        const auto a = ft::register_pair(w, h, img, moved, nullptr, nullptr, ft::Model::Homography);
        const auto b = ft::register_pair(w, h, img, moved, nullptr, nullptr, ft::Model::Homography);
        QVERIFY(a && b);
        QVERIFY(a->first.m == b->first.m);
        QCOMPARE(a->second, b->second);
    }

    // A flat picture has nothing to match: none (the op says 対応が見つかりません).
    void flatPicturesDoNotRegister() {
        const std::size_t w = 120, h = 100;
        const std::vector<float> flat(w * h, 0.5f);
        QVERIFY(!ft::register_pair(w, h, flat, flat, nullptr, nullptr, ft::Model::Homography));
    }
};

QTEST_GUILESS_MAIN(TestFeatures)
#include "test_features.moc"
