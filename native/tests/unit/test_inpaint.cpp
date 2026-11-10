// PC3 B4 (SPEC.md COLOR-06「修復」): the fills under the Healing Brush, the Spot Healing Brush and Content-Aware Fill as
// PhotoCraft v0.2.0's tests check them (algo/src/poisson.rs:232-300, inpaint.rs:623-710, content_aware.rs:255-330): the
// membrane reproduces a harmonic (linear) edge, the cascadic solve matches plain Gauss–Seidel, seamless cloning takes the
// destination's colour and the source's texture; PatchMatch completion fills a blemish from the texture around it and
// carries stripes on, the proximity match finds the period, texture synthesis stays near; Content-Aware Fill fills only
// the hole with known texture, keeps to its sampling area, follows a gradient with colour adaptation, and its turned,
// scaled and mirrored copies are the right size. Each is deterministic: the same input, the same values.

#include <QtTest>

#include <cmath>
#include <numbers>
#include <vector>

#include "render/content_aware.hpp"
#include "render/inpaint.hpp"
#include "render/poisson.hpp"

using namespace genko::render;

namespace {

std::vector<bool> disc(std::size_t w, std::size_t h, float cx, float cy, float r) {
    std::vector<bool> out(w * h);
    for (std::size_t i = 0; i < w * h; ++i) out[i] = std::hypot(static_cast<float>(i % w) - cx, static_cast<float>(i / w) - cy) < r;
    return out;
}

// A smooth gradient with a fine deterministic noise (inpaint.rs's texture).
std::vector<float> texture(std::size_t w, std::size_t h) {
    inpaint::Rng rng(7);
    std::vector<float> out;
    for (std::size_t i = 0; i < w * h; ++i) {
        const auto x = static_cast<float>(i % w), y = static_cast<float>(i / w);
        const float n = static_cast<float>(rng.next() % 1000) / 1000.0f * 0.04f - 0.02f;
        out.insert(out.end(), {0.3f + 0.004f * x + n, 0.5f + 0.002f * y + n, 0.4f + n});
    }
    return out;
}

float hole_rmse(const std::vector<float>& a, const std::vector<float>& b, const std::vector<bool>& hole, std::size_t ch) {
    double e = 0;
    std::size_t n = 0;
    for (std::size_t i = 0; i < hole.size(); ++i) {
        if (!hole[i]) continue;
        for (std::size_t c = 0; c < ch; ++c) {
            e += std::pow(a[i * ch + c] - b[i * ch + c], 2.0);
            ++n;
        }
    }
    return static_cast<float>(std::sqrt(e / static_cast<double>(n)));
}

}  // namespace

class TestInpaint : public QObject {
    Q_OBJECT
private slots:
    void membraneReproducesALinearBoundary() {
        const std::size_t w = 80, h = 60;
        const std::vector<bool> unknown = disc(w, h, 40, 30, 20);
        std::vector<float> truth(w * h), v(w * h);
        for (std::size_t i = 0; i < w * h; ++i) {
            truth[i] = 0.2f + 0.005f * static_cast<float>(i % w) + 0.003f * static_cast<float>(i / w);
            v[i] = unknown[i] ? 0.0f : truth[i];
        }
        poisson::solve_membrane(w, h, unknown, v);
        float err = 0;
        for (std::size_t i = 0; i < w * h; ++i) err = std::max(err, std::fabs(v[i] - truth[i]));
        QVERIFY2(err < 2e-3f, qPrintable(QString::number(err)));
    }

    void multigridMatchesTheReferenceSolve() {
        const std::size_t w = 96, h = 72;
        const std::vector<bool> unknown = disc(w, h, 45, 36, 28);
        std::vector<float> init(w * h);
        for (std::size_t i = 0; i < w * h; ++i)
            init[i] = std::sin(static_cast<float>(i % w) * 0.21f) * 0.3f + std::cos(static_cast<float>(i / w) * 0.13f) * 0.2f + 0.5f;
        std::vector<float> fast = init, reference = init;
        poisson::solve_membrane(w, h, unknown, fast);
        poisson::solve_membrane_reference(w, h, unknown, reference, 1e-7f, 100000);
        float err = 0;
        for (std::size_t i = 0; i < w * h; ++i) {
            err = std::max(err, std::fabs(fast[i] - reference[i]));
            if (!unknown[i]) QCOMPARE(fast[i], init[i]);  // (the known pixels untouched)
        }
        QVERIFY2(err < 2e-3f, qPrintable(QString::number(err)));
    }

    void seamlessCloneTakesTheDestinationsColourAndTheSourcesTexture() {
        const std::size_t w = 64, h = 64, ch = 3;
        const std::vector<bool> mask = disc(w, h, 32, 32, 20);
        std::vector<float> dst, src;
        for (std::size_t i = 0; i < w * h; ++i) {
            dst.insert(dst.end(), {0.7f, 0.3f, 0.2f});
            const float t = ((i % w) / 4) % 2 == 0 ? 0.05f : -0.05f;
            src.insert(src.end(), {0.1f + t, 0.2f + t, 0.8f + t});
        }
        const std::vector<float> out = poisson::seamless_clone(w, h, ch, src, dst, mask);
        for (std::size_t c = 0; c < ch; ++c) {
            double sum = 0;
            std::size_t n = 0;
            for (std::size_t i = 0; i < w * h; ++i) {
                if (mask[i]) sum += out[i * ch + c], ++n;
            }
            QVERIFY2(std::fabs(sum / static_cast<double>(n) - dst[c]) < 0.02, qPrintable(QString::number(sum / static_cast<double>(n))));
        }
        double e = 0;
        std::size_t n = 0;
        for (std::size_t i = 0; i + 1 < w * h; ++i) {
            if (!mask[i] || !mask[i + 1]) continue;
            e += std::fabs((out[(i + 1) * ch] - out[i * ch]) - (src[(i + 1) * ch] - src[i * ch]));
            ++n;
        }
        QVERIFY2(e / static_cast<double>(n) < 0.01, qPrintable(QString::number(e / static_cast<double>(n))));
        QCOMPARE(out[0], dst[0]);  // (outside untouched)
        QCOMPARE(poisson::seamless_clone(w, h, ch, src, dst, mask), out);  // (the same again)
    }

    void completionFillsABlemishFromTexture() {
        const std::size_t w = 96, h = 96, ch = 3;
        const std::vector<float> clean = texture(w, h);
        const std::vector<bool> hole = disc(w, h, 48, 48, 9);
        std::vector<float> dirty = clean;
        for (std::size_t i = 0; i < w * h; ++i) {
            if (hole[i]) std::fill_n(dirty.begin() + static_cast<std::ptrdiff_t>(i * ch), ch, 0.02f);
        }
        const auto out = inpaint::complete(w, h, ch, dirty, hole);
        QVERIFY(out);
        const float before = hole_rmse(dirty, clean, hole, ch), after = hole_rmse(*out, clean, hole, ch);
        QVERIFY2(after < 0.06f && after < before * 0.15f, qPrintable(QStringLiteral("%1 → %2").arg(before).arg(after)));
        for (std::size_t i = 0; i < w * h * ch; ++i) {
            if (!hole[i / ch]) QCOMPARE((*out)[i], dirty[i]);
        }
        QCOMPARE(*inpaint::complete(w, h, ch, dirty, hole), *out);  // (deterministic)
    }

    void completionContinuesStructure() {
        const std::size_t w = 80, h = 80;
        std::vector<float> clean(w * h), dirty(w * h);
        const std::vector<bool> hole = disc(w, h, 40, 40, 7);
        for (std::size_t i = 0; i < w * h; ++i) {
            clean[i] = ((i % w) / 4) % 2 == 0 ? 0.2f : 0.8f;
            dirty[i] = hole[i] ? 0.5f : clean[i];
        }
        const auto out = inpaint::complete(w, h, 1, dirty, hole);
        QVERIFY(out);
        QVERIFY2(hole_rmse(*out, clean, hole, 1) < 0.15f, qPrintable(QString::number(hole_rmse(*out, clean, hole, 1))));
    }

    void noKnownPatchIsNone() {
        const std::vector<bool> all(16 * 16, true);
        QVERIFY(!inpaint::complete(16, 16, 1, std::vector<float>(16 * 16, 0.5f), all));
        // nothing to fill: the same
        const std::vector<float> img(16 * 16, 0.25f);
        QCOMPARE(*inpaint::complete(16, 16, 1, img, std::vector<bool>(16 * 16, false)), img);
    }

    void proximityFindsTheMatchingOffset() {
        const std::size_t w = 80, h = 60;
        std::vector<float> img(w * h);
        for (std::size_t i = 0; i < w * h; ++i) img[i] = std::sin(static_cast<float>(i % w) * std::numbers::pi_v<float> * 2.0f / 10.0f) * 0.4f + 0.5f;
        const std::vector<bool> hole = disc(w, h, 40, 30, 5);
        const auto off = inpaint::best_offset(w, h, 1, img, hole, 3, 20);
        QVERIFY(off);
        QCOMPARE(((off->first % 10) + 10) % 10, 0);
        QVERIFY(*off != std::make_pair(0, 0));
    }

    void synthesisFillsTheHoleWithPlausibleValues() {
        const std::size_t w = 64, h = 64, ch = 3;
        const std::vector<float> clean = texture(w, h);
        const std::vector<bool> hole = disc(w, h, 32, 32, 6);
        const std::vector<float> out = inpaint::synthesize(w, h, ch, clean, hole, 8, 3);
        QVERIFY(hole_rmse(out, clean, hole, ch) < 0.15f);
        for (const float v : out) QVERIFY(std::isfinite(v));
        QCOMPARE(inpaint::synthesize(w, h, ch, clean, hole, 8, 3), out);
    }

    // Content-Aware Fill.
    void fillsOnlyTheHoleWithKnownTexture() {
        const std::size_t w = 48, h = 32;
        std::vector<float> img(w * h);
        std::vector<bool> hole(w * h);
        for (std::size_t i = 0; i < w * h; ++i) {
            img[i] = (i % w) % 6 < 3 ? 0.2f : 0.8f;
            hole[i] = (i % w > w / 2 ? i % w - w / 2 : w / 2 - i % w) < 4 && (i / w > h / 2 ? i / w - h / 2 : h / 2 - i / w) < 4;
        }
        content_aware::FillOptions opts;
        opts.color_adaptation = 0;
        const std::vector<float> out = content_aware::fill(w, h, 1, img, hole, std::vector<bool>(w * h, true), opts);
        bool dark = false, light = false;
        for (std::size_t i = 0; i < w * h; ++i) {
            if (!hole[i]) {
                QCOMPARE(out[i], img[i]);
                continue;
            }
            dark = dark || out[i] < 0.35f;
            light = light || out[i] > 0.65f;
        }
        QVERIFY(dark && light);  // (stripes, not a flat mean)
    }

    void theSamplingAreaRestrictsTheSources() {
        const std::size_t w = 40, h = 20;
        std::vector<float> img(w * h);
        std::vector<bool> hole(w * h), right(w * h);
        for (std::size_t i = 0; i < w * h; ++i) {
            img[i] = i % w < 20 ? 0.1f : 0.9f;
            hole[i] = i % w >= 16 && i % w < 24 && i / w >= 6 && i / w < 14;
            right[i] = i % w >= 24;
        }
        content_aware::FillOptions opts;
        opts.color_adaptation = 0;
        const std::vector<float> out = content_aware::fill(w, h, 1, img, hole, right, opts);
        double mean = 0;
        for (std::size_t i = 0; i < w * h; ++i) {
            if (hole[i]) mean += out[i];
        }
        QVERIFY2(mean / 64.0 > 0.8, qPrintable(QString::number(mean / 64.0)));
    }

    void adaptationsRunAndStayDeterministic() {
        const std::size_t w = 32, h = 24;
        std::vector<float> rgb;
        std::vector<bool> hole(w * h);
        for (std::size_t i = 0; i < w * h; ++i) {
            const float v = (i % w) % 6 < 3 ? 0.2f : 0.8f;
            rgb.insert(rgb.end(), {v, v * 0.5f, 1.0f - v});
            hole[i] = (i % w > w / 2 ? i % w - w / 2 : w / 2 - i % w) < 4 && (i / w > h / 2 ? i / w - h / 2 : h / 2 - i / w) < 4;
        }
        content_aware::FillOptions opts;
        opts.color_adaptation = 0.65f;
        opts.rotations = content_aware::rotation_level("low");
        opts.scale = true;
        opts.mirror = true;
        opts.seed = 7;
        const std::vector<bool> all(w * h, true);
        const std::vector<float> a = content_aware::fill(w, h, 3, rgb, hole, all, opts);
        QCOMPARE(content_aware::fill(w, h, 3, rgb, hole, all, opts), a);
        for (const float v : a) QVERIFY(std::isfinite(v) && v >= 0.0f && v <= 1.0f);
    }

    void colourAdaptationFollowsAGradient() {
        const std::size_t w = 40, h = 20;
        std::vector<float> img(w * h);
        std::vector<bool> hole(w * h);
        for (std::size_t i = 0; i < w * h; ++i) {
            img[i] = static_cast<float>(i % w) / static_cast<float>(w);
            hole[i] = i % w >= 14 && i % w < 26 && i / w >= 5 && i / w < 15;
        }
        content_aware::FillOptions opts;
        opts.color_adaptation = 0.9f;
        const std::vector<float> out = content_aware::fill(w, h, 1, img, hole, std::vector<bool>(w * h, true), opts);
        double err = 0;
        for (std::size_t i = 0; i < w * h; ++i) {
            if (hole[i]) err += std::fabs(out[i] - img[i]);
        }
        QVERIFY2(err / 120.0 < 0.12, qPrintable(QString::number(err / 120.0)));
    }

    void variantGeometry() {
        const std::vector<float> img(10 * 6, 0.5f);
        const std::vector<bool> ok(60, true);
        content_aware::Variant v = content_aware::variant(10, 6, 1, img, ok, 90, 1, false);
        QCOMPARE(v.w, std::size_t(6));
        QCOMPARE(v.h, std::size_t(10));
        v = content_aware::variant(10, 6, 1, img, ok, 0, 0.8f, true);
        QCOMPARE(v.w, std::size_t(8));
        QCOMPARE(v.h, std::size_t(5));
        QVERIFY(std::count(v.ok.begin(), v.ok.end(), true) > 20);
        QCOMPARE(content_aware::rotation_level("full"), (std::vector<float>{90, 180, 270}));
        QCOMPARE(content_aware::color_level("none"), 0.0f);
        QCOMPARE(content_aware::color_level("very_high"), 0.9f);
        QCOMPARE(content_aware::color_level("default"), 0.35f);
    }

    // The generator: SplitMix64 from the seed (the same numbers each time).
    void theGeneratorIsSeeded() {
        inpaint::Rng a(1), b(1), c(2);
        const std::uint64_t first = a.next();
        QCOMPARE(b.next(), first);
        QVERIFY(c.next() != first);
        for (int i = 0; i < 100; ++i) {
            const int v = a.range(-3, 3);
            QVERIFY(v >= -3 && v <= 3);
        }
        QCOMPARE(a.range(5, 5), 5);
    }
};

QTEST_MAIN(TestInpaint)
#include "test_inpaint.moc"
