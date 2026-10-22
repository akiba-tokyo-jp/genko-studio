// PC4 E1 (brushes@1, SPEC.md COLOR-09「ブラシのダブの描き方」): the dab engine (render/dabs) — PhotoCraft v0.2.0's own
// tests of its brush engine (crates/paint/src/lib.rs's tests and brush_tests.rs: spacing, the coverage's profile, the
// opacity's ceiling and the flow's build-up, the tips, the pencil, the dynamics and their controls, the scatter, the
// colour dynamics, the transfer, the airbrush's build-up, the textures' ten modes, the dual brush, the wet edges and
// the noise, the smoothing, the pose, chunks), in Genko's millimetres: upstream's pixels are pixels of the working
// 200 dpi (kPx mm; 300 dpi where a texture's or the noise's pixel is upstream's: one is 1/300 inch). And this build's
// own: the same seed the same dabs, a line's seed from its id, the dabs fixed in mm (the same at 200 and 600 dpi, the
// coverage the same shape), a part drawn as the whole draws it there, the random numbers and the angles the same bits
// as the reference (rng.rs's mix64; atan2 against the C library's within 1e-12), the pictures kept by their ref only.

#include <QtTest>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <set>
#include <string>
#include <vector>

#include "core/brush_dabs.hpp"
#include "core/error.hpp"
#include "render/dabs/coverage.hpp"
#include "render/dabs/dynamics.hpp"
#include "render/dabs/lines.hpp"
#include "render/dabs/maths.hpp"
#include "render/dabs/procedural.hpp"
#include "render/dabs/rng.hpp"
#include "render/dabs/tile.hpp"
#include "render/png.hpp"
#include "storage/asset_store.hpp"

using namespace genko;
using namespace genko::render::dabs;
using core::DabControl;
using core::DabDynamic;
using core::DabMaskMode;
using core::DabSettings;
using render::Box;

namespace {

// A pixel of `dpi` in mm.
double px(double v, int dpi = 200) { return v * 25.4 / dpi; }

InputPoint pt(double x, double y, double pressure = 1.0, int dpi = 200) {
    InputPoint p;
    p.x = px(x, dpi);
    p.y = px(y, dpi);
    p.pressure = pressure;
    return p;
}

std::vector<InputPoint> line(double x0, double x1, double y, int dpi = 200) { return {pt(x0, y, 1.0, dpi), pt(x1, y, 1.0, dpi)}; }

// upstream's brush(): pressure_size off, spacing 0.25
DabSettings brush() {
    DabSettings s;
    s.pressure_size = false;
    s.spacing = 0.25;
    return s;
}

LineBrush with(const DabSettings& s, double size_px, int dpi = 200) {
    LineBrush b;
    b.settings = s;
    b.size_mm = px(size_px, dpi);
    b.seed = static_cast<std::uint64_t>(s.seed);
    return b;
}

std::vector<Dab> dabs_of(const DabSettings& s, double size_px, const std::vector<InputPoint>& pts, int dpi = 200) {
    return all_dabs(with(s, size_px, dpi), pts);
}

// The line laid at `dpi` over a page of 2000 × 2000 pixels (upstream's surface): its coverage at a pixel (the alpha the
// stroke leaves on a clear surface).
struct Painted {
    std::shared_ptr<const LineCover> cover;
    float a(int x, int y) const { return cover ? cover->at(x, y) : 0.0f; }
    std::array<float, 3> rgb(int x, int y) const { return cover->colour(x, y); }
};

Painted paint(const LineBrush& b, const std::vector<InputPoint>& pts, int dpi = 200, Box within = Box{-1000, -1000, 3000, 3000}) {
    DabGenerator g(b);
    std::vector<Dab> d, dual;
    g.push(pts, d, dual);
    g.finish(d, dual);
    return Painted{lay(context(b.settings, dpi), d, dual, within, b.settings.color.enabled)};
}

Painted paint(const DabSettings& s, double size_px, const std::vector<InputPoint>& pts, int dpi = 200) { return paint(with(s, size_px, dpi), pts, dpi); }

// A dab alone, rasterised (upstream's raster()).
struct Raster {
    Box rect;
    std::vector<float> v;
    float at(int x, int y) const { return v[std::size_t((y - rect.y0) * rect.width() + (x - rect.x0))]; }
};

Raster raster(const DabSettings& s, const Dab& d, int dpi = 200) {
    const auto ctx = context(s, dpi);
    Raster r{ctx->dab_rect(d, false), {}};
    ctx->rasterize(d, false, r.rect, r.v);
    return r;
}

Dab round_dab(double x, double y, double radius_px, int dpi = 200) {
    Dab d;
    d.x = px(x, dpi);
    d.y = px(y, dpi);
    d.radius = px(radius_px, dpi);
    return d;
}

// A grey tile kept by its ref (as a book's picture is): the ref.
std::string kept(const GrayTile& t) {
    auto png = std::make_shared<const std::string>(png_of_tile(t));
    const std::string ref = storage::AssetStore::ref(*png);
    keep_picture(ref, png);
    return ref;
}

float sum_alpha(const Painted& p, Box r) {
    float t = 0.0f;
    for (int y = r.y0; y < r.y1; ++y)
        for (int x = r.x0; x < r.x1; ++x) t += p.a(x, y);
    return t;
}

}  // namespace

class TestDabEngine : public QObject {
    Q_OBJECT

private slots:
    // rng.rs: deterministic, uniform-ish; its mix64 the reference's (SplitMix64's finaliser: its known first values).
    void randomIsDeterministic() {
        QCOMPARE(rand01(7, 3, 1), rand01(7, 3, 1));
        QVERIFY(rand01(7, 3, 1) != rand01(8, 3, 1));
        QVERIFY(rand01(7, 3, 1) != rand01(7, 3, 2));
        double mean = 0;
        for (std::uint64_t i = 0; i < 10000; ++i) {
            const float r = rand01(1, i, 0);
            QVERIFY(r >= 0.0f && r < 1.0f);
            mean += r;
        }
        QVERIFY(std::abs(mean / 10000 - 0.5) < 0.02);
        // SplitMix64 from 0: its first output (the reference value of the generator)
        QCOMPARE(mix64(0), std::uint64_t(0xE220A8397B1DCDAFULL));
        QCOMPARE(fnv1a(""), std::uint64_t(0xCBF29CE484222325ULL));
        QCOMPARE(fnv1a("a"), std::uint64_t(0xAF63DC4C8601EC8CULL));
    }

    // The angles: sin and cos the paper's; atan2 within 1e-12 of the C library's everywhere (its own bits on every
    // computer).
    void anglesAreFixed() {
        for (int i = 0; i < 2000; ++i) {
            const double y = std::sin(i * 0.37) * (1 + i % 7), x = std::cos(i * 0.91) * (1 + i % 5);
            QVERIFY2(std::abs(atan2_degrees(y, x) - std::atan2(y, x) * 180.0 / 3.141592653589793) < 1e-9, qPrintable(QString::number(i)));
        }
        QCOMPARE(atan2_degrees(0, 0), 0.0);
        QCOMPARE(atan2_degrees(1, 0), 90.0);
        QCOMPARE(atan2_degrees(0, -1), 180.0);
        QCOMPARE(atan2_degrees(-1, 0), -90.0);
        QCOMPARE(rem_euclid(-30.0, 360.0), 330.0);
    }

    // lib.rs dab_spacing, brush_tests spacing_sets_dab_count: spacing × the diameter, carried across segments.
    void spacing() {
        DabSettings s;
        s.pressure_size = false;
        const auto d = dabs_of(s, 10, line(0, 100, 0));
        QVERIFY2(d.size() >= 95 && d.size() <= 105, qPrintable(QString::number(d.size())));
        s.spacing = 1.0;
        QCOMPARE(dabs_of(s, 10, line(0, 100, 0)).size(), std::size_t(11));
        for (const auto& [spacing, len, expect] : std::vector<std::tuple<double, double, std::size_t>>{{0.25, 100, 21}, {1.0, 100, 6}, {0.5, 200, 21}}) {
            DabSettings b = brush();
            b.spacing = spacing;
            QCOMPARE(dabs_of(b, 20, line(0, len, 0)).size(), expect);
        }
        DabSettings b = brush();
        b.spacing = 0.5;
        std::vector<InputPoint> pts;
        for (int i = 0; i <= 20; ++i) pts.push_back(pt(i * 10.0, 0));
        const auto many = dabs_of(b, 20, pts);
        QCOMPARE(many.size(), std::size_t(21));
        for (std::size_t i = 1; i < many.size(); ++i) QVERIFY(std::abs((many[i].x - many[i - 1].x) - px(10)) < 1e-9);
    }

    // lib.rs coverage_profile; brush_tests hardness_profile_is_monotonic.
    void coverageProfile() {
        QCOMPARE(dab_coverage(0.0f, 10.0f, 1.0f), 1.0f);
        QCOMPARE(dab_coverage(20.0f, 10.0f, 1.0f), 0.0f);
        const float soft_mid = dab_coverage(5.0f, 10.0f, 0.0f);
        QVERIFY(soft_mid > 0.2f && soft_mid < 0.8f);
        QVERIFY(dab_coverage(9.9f, 10.0f, 1.0f) > 0.5f);
        for (const double h : {0.0, 0.5, 1.0}) {
            DabSettings s = brush();
            s.hardness = h;
            const Raster r = raster(s, round_dab(50, 50, 20));
            std::vector<float> prof;
            for (int i = 0; i < 22; ++i) prof.push_back(r.at(50 + i, 50));
            for (std::size_t i = 1; i < prof.size(); ++i) QVERIFY(prof[i] <= prof[i - 1] + 1e-6f);
            QVERIFY(prof[0] > 0.99f);
            QCOMPARE(prof[21], 0.0f);
            const float mid = r.at(60, 50);
            if (h == 1.0) QVERIFY(mid > 0.99f);
            if (h == 0.0) QVERIFY(mid > 0.2f && mid < 0.8f);
        }
    }

    // lib.rs hard_brush_paints_solid_line, opacity_caps_overlap, pressure_changes_size.
    void lineOpacityAndPressure() {
        DabSettings s;
        s.pressure_size = false;
        const Painted p = paint(s, 8, line(10, 90, 10));
        QCOMPARE(p.a(50, 10), 1.0f);
        QCOMPARE(p.a(50, 30), 0.0f);
        DabSettings half = s;
        half.opacity = 0.5;
        const Painted q = paint(half, 10, {pt(10, 10), pt(60, 10), pt(10, 10)});
        QVERIFY2(std::abs(q.a(30, 10) - 0.5f) < 0.02f, qPrintable(QString::number(q.a(30, 10))));
        DabSettings pressed = s;
        pressed.pressure_size = true;
        const auto d = dabs_of(pressed, 20, {pt(0, 0, 0.1), pt(100, 0, 1.0)});
        QVERIFY(d.front().radius < d.back().radius);
    }

    // brush_tests round_tip_roundness_and_angle.
    void roundTipRoundnessAndAngle() {
        const DabSettings b = brush();
        Dab d = round_dab(50, 50, 20);
        d.roundness = 0.25;
        Raster r = raster(b, d);
        QVERIFY(r.at(65, 50) > 0.99f);
        QVERIFY(r.at(50, 58) < 0.01f);
        QVERIFY(r.at(50, 53) > 0.99f);
        d.angle = 90;  // (counter-clockwise: now tall)
        r = raster(b, d);
        QVERIFY(r.at(50, 35) > 0.99f);
        QVERIFY(r.at(58, 50) < 0.01f);
        d.angle = 45;  // (the long axis up and right: y down on the page)
        r = raster(b, d);
        QVERIFY(r.at(60, 40) > 0.99f);
        QVERIFY(r.at(60, 60) < 0.01f);
    }

    // brush_tests sampled_tip_is_scaled_and_oriented: a picture tip, flipped, turned, drawn small through its mips.
    void sampledTip() {
        DabSettings b = brush();
        b.tip = kept(GrayTile::from_fn(16, 16, [](std::uint32_t x, std::uint32_t) { return x < 8 ? 1.0f : 0.0f; }));
        const Dab d = round_dab(50, 50, 20);
        Raster r = raster(b, d);
        QVERIFY(r.at(40, 50) > 0.99f && r.at(60, 50) < 0.01f);
        Dab flipped = d;
        flipped.flip_x = true;
        r = raster(b, flipped);
        QVERIFY(r.at(40, 50) < 0.01f && r.at(60, 50) > 0.99f);
        Dab turned = d;
        turned.angle = 90;
        r = raster(b, turned);
        QVERIFY(r.at(50, 60) > 0.99f && r.at(50, 40) < 0.01f);
        r = raster(b, round_dab(50, 50, 2));
        QVERIFY(std::accumulate(r.v.begin(), r.v.end(), 0.0f) > 1.0f);
        // a picture this process does not know: missing_asset
        DabSettings unknown = brush();
        unknown.tip = "sha256:" + std::string(64, 'e');
        bool missing = false;
        try {
            (void)context(unknown, 200);
        } catch (const core::Error& error) {
            missing = error.code() == "missing_asset";
        }
        QVERIFY(missing);
        // only bytes that hash to their ref are kept
        QVERIFY(!keep_picture("sha256:" + std::string(64, 'f'), std::make_shared<const std::string>("not it")));
    }

    // brush_tests pencil_is_aliased.
    void pencilIsAliased() {
        DabSettings b = brush();
        b.aliased = true;
        b.hardness = 0.0;
        const Painted p = paint(b, 9, line(10, 50, 20.5));
        for (int y = 10; y < 32; ++y)
            for (int x = 0; x < 64; ++x) QVERIFY(p.a(x, y) == 0.0f || p.a(x, y) == 1.0f);
        QCOMPARE(p.a(30, 20), 1.0f);
        b.aliased = false;
        const Painted soft = paint(b, 9, line(10, 50, 20.3));
        bool partial = false;
        for (int y = 10; y < 32; ++y)
            for (int x = 0; x < 64; ++x) partial = partial || (soft.a(x, y) > 0.01f && soft.a(x, y) < 0.99f);
        QVERIFY(partial);
        // the pencil tool's brush: hard and aliased
        const core::Json pencil = pencil_brush();
        const DabSettings ps = core::dabs_from_json(pencil["dabs"]);
        QVERIFY(ps.aliased && ps.hardness == 1.0);
    }

    // brush_tests pressure_controls_size_with_minimum, fade_ramps_over_steps.
    void pressureAndFade() {
        DabSettings b = brush();
        b.shape.enabled = true;
        b.shape.size = DabDynamic{0.0, DabControl::Pressure, 25, 0.25};
        const auto d = dabs_of(b, 40, {pt(0, 0, 0.0), pt(200, 0, 1.0)});
        QVERIFY2(std::abs(d.front().radius - px(5)) < 1e-6, qPrintable(QString::number(d.front().radius / px(1))));
        QVERIFY(std::abs(d.back().radius - px(20)) < px(0.5));
        for (std::size_t i = 1; i < d.size(); ++i) QVERIFY(d[i].radius >= d[i - 1].radius - 1e-9);
        DabSettings f = brush();
        f.spacing = 0.5;
        f.shape.enabled = true;
        f.shape.size = DabDynamic{0.0, DabControl::Fade, 10, 0.0};
        const auto fd = dabs_of(f, 20, line(0, 300, 0));
        QVERIFY(std::abs(fd[0].radius - px(10)) < 1e-9);
        QVERIFY(fd[5].radius < fd[0].radius * 0.6);
        for (std::size_t i = 10; i < fd.size(); ++i) QVERIFY(fd[i].radius <= px(0.5) + 1e-12);
        DabSettings t = brush();
        t.transfer.enabled = true;
        t.transfer.opacity = DabDynamic{0.0, DabControl::Fade, 4, 0.0};
        const auto td = dabs_of(t, 20, line(0, 100, 0));
        QCOMPARE(td[0].opacity, 1.0f);
        QCOMPARE(td[4].opacity, 0.0f);
    }

    // brush_tests tilt_direction_and_rotation_drive_angle.
    void tiltDirectionRotation() {
        const auto mk = [](DabControl c) {
            DabSettings s = brush();
            s.shape.enabled = true;
            s.shape.angle.control = c;
            return s;
        };
        InputPoint p = pt(0, 0);
        p.tilt_y = -45;  // (the pen leaning up the screen)
        InputPoint q = p;
        q.x = px(50);
        auto d = dabs_of(mk(DabControl::Tilt), 20, {p, q});
        QVERIFY(std::abs(d[0].angle - 90.0) < 1e-6);
        d = dabs_of(mk(DabControl::Direction), 20, {pt(0, 0), pt(0, 50)});
        for (const Dab& x : d) QVERIFY(std::abs(x.angle + 90.0) < 1e-6);  // (down the page: −90°)
        InputPoint r = pt(0, 0);
        r.rotation = 30;
        InputPoint r2 = r;
        r2.x = px(20);
        d = dabs_of(mk(DabControl::Rotation), 20, {r, r2});
        QVERIFY(std::abs(d[0].angle - 30.0) < 1e-6);
        DabSettings sz = brush();
        sz.shape.enabled = true;
        sz.shape.size.control = DabControl::Tilt;
        InputPoint flat = pt(0, 0);
        flat.tilt_x = 90;
        d = dabs_of(sz, 20, {flat});
        QVERIFY(d[0].radius <= px(0.5) + 1e-12);
    }

    // brush_tests jitter_is_deterministic_per_seed: the same seed the same dabs, another seed others, within their
    // ranges, the same in chunks.
    void jitterIsDeterministicPerSeed() {
        DabSettings b = brush();
        b.seed = 42;
        b.shape.enabled = true;
        b.shape.size = DabDynamic{1.0, DabControl::Off, 25, 0.0};
        b.shape.angle = DabDynamic{1.0, DabControl::Off, 25, 0.0};
        b.shape.roundness = DabDynamic{1.0, DabControl::Off, 25, 0.2};
        b.shape.flip_x_jitter = true;
        b.shape.flip_y_jitter = true;
        b.scatter.enabled = true;
        b.scatter.scatter.jitter = 2.0;
        b.scatter.both_axes = true;
        b.scatter.count = 3;
        b.scatter.count_jitter.jitter = 1.0;
        const auto pts = line(0, 300, 50);
        const auto a = dabs_of(b, 30, pts);
        QVERIFY(a == dabs_of(b, 30, pts));
        DabSettings other = b;
        other.seed = 43;
        QVERIFY(a != dabs_of(other, 30, pts));
        bool small = false, flipped = false, plain = false;
        for (const Dab& d : a) {
            small = small || d.radius < px(10);
            QVERIFY(d.radius <= px(15) + 1e-9);
            QVERIFY(d.roundness >= 0.2 - 1e-6 && d.roundness <= 1.0);
            flipped = flipped || d.flip_x;
            plain = plain || !d.flip_x;
        }
        QVERIFY(small && flipped && plain);
        std::vector<InputPoint> many;
        for (int i = 0; i <= 30; ++i) many.push_back(pt(i * 10.0, 50));
        DabGenerator g(with(b, 30));
        std::vector<Dab> chunked, dual;
        for (std::size_t i = 0; i < many.size(); i += 4) g.push(std::vector<InputPoint>(many.begin() + long(i), many.begin() + long(std::min(i + 4, many.size()))), chunked, dual);
        g.finish(chunked, dual);
        QVERIFY(chunked == dabs_of(b, 30, many));
    }

    // brush_tests scattering_stays_in_bounds.
    void scatteringStaysInBounds() {
        for (const bool both : {false, true}) {
            DabSettings b = brush();
            b.scatter.enabled = true;
            b.scatter.scatter.jitter = 2.0;
            b.scatter.both_axes = both;
            b.scatter.count = 4;
            const auto d = dabs_of(b, 20, line(0, 400, 100));
            QCOMPARE(d.size(), std::size_t(81 * 4));
            bool far = false;
            for (const Dab& x : d) {
                QVERIFY(std::abs(x.y - px(100)) <= px(20) + 1e-9);
                far = far || std::abs(x.y - px(100)) > px(10);
            }
            QVERIFY(far);
            bool off_grid = false;
            for (const Dab& x : d) {
                const double steps = x.x / px(5);
                if (!both) QVERIFY(std::abs(steps - std::round(steps)) < 1e-6);
                off_grid = off_grid || std::abs(steps - std::round(steps)) > 0.1;
            }
            QCOMPARE(off_grid, both);
        }
    }

    // brush_tests color_dynamics_ranges: foreground/background, hue, purity, once a line, each dab's colour in the pixels.
    void colorDynamicsRanges() {
        DabSettings base = brush();
        base.seed = 5;
        base.color.enabled = true;
        LineBrush lb = with(base, 20);
        lb.color = {1, 0, 0, 1};
        lb.background = {0, 0, 1, 1};
        lb.settings.color.fg_bg.jitter = 1.0;
        auto d = all_dabs(lb, line(0, 200, 0));
        bool blue = false, red = false;
        for (const Dab& x : d) {
            QVERIFY((std::abs(x.color[1]) < 1e-5f && std::abs(x.color[0] + x.color[2] - 1.0f) < 1e-3f) || std::max(x.color[0], x.color[2]) >= 0.999f);
            blue = blue || x.color[2] > 0.5f;
            red = red || x.color[0] > 0.5f;
        }
        QVERIFY(blue && red);
        lb.settings.color.fg_bg.jitter = 0.0;
        lb.settings.color.hue_jitter = 1.0;
        d = all_dabs(lb, line(0, 200, 0));
        bool moved = false;
        for (const Dab& x : d) {
            const float mx = std::max({x.color[0], x.color[1], x.color[2]}), mn = std::min({x.color[0], x.color[1], x.color[2]});
            QVERIFY(std::abs(mx - 1.0f) < 1e-4f && std::abs(mn) < 1e-4f);
            moved = moved || x.color[1] > 0.5f || x.color[2] > 0.5f;
        }
        QVERIFY(moved);
        lb.settings.color.hue_jitter = 0.0;
        lb.settings.color.purity = -1.0;
        for (const Dab& x : all_dabs(lb, line(0, 20, 0))) QVERIFY(std::abs(x.color[0] - x.color[1]) < 1e-5f && std::abs(x.color[1] - x.color[2]) < 1e-5f);
        lb.settings.color.purity = 0.0;
        lb.settings.color.per_tip = false;
        lb.settings.color.hue_jitter = 1.0;
        d = all_dabs(lb, line(0, 200, 0));
        for (std::size_t i = 1; i < d.size(); ++i) QVERIFY(d[i].color == d[0].color);
        // each dab's colour in the pixels
        LineBrush wide = with(base, 10);
        wide.settings.spacing = 3.0;
        wide.settings.color.fg_bg.jitter = 1.0;
        wide.color = {1, 0, 0, 1};
        wide.background = {0, 0, 1, 1};
        const Painted p = paint(wide, line(10, 310, 20));
        bool b2 = false, r2 = false;
        for (int i = 0; i < 11; ++i) {
            const auto c = p.rgb(10 + i * 30, 20);
            b2 = b2 || c[2] > 0.6f;
            r2 = r2 || c[0] > 0.6f;
        }
        QVERIFY(b2 && r2);
    }

    // brush_tests transfer_opacity_caps_and_flow_builds.
    void transferOpacityAndFlow() {
        DabSettings b = brush();
        b.transfer.enabled = true;
        b.transfer.opacity.control = DabControl::Pressure;
        const Painted p = paint(b, 10, {pt(10, 10, 0.4), pt(90, 10, 0.4)});
        QVERIFY2(std::abs(p.a(50, 10) - 0.4f) < 0.01f, qPrintable(QString::number(p.a(50, 10))));
        DabSettings f = brush();
        f.flow = 0.1;
        f.opacity = 0.8;
        const float one = paint(f, 10, {pt(50, 10)}).a(50, 10);
        const float many = paint(f, 10, line(40, 60, 10)).a(50, 10);
        QVERIFY2(std::abs(one - 0.08f) < 0.01f, qPrintable(QString::number(one)));
        QVERIFY(many > one * 3 && many <= 0.8f + 1e-4f);
        DabSettings j = brush();
        j.transfer.enabled = true;
        j.transfer.flow.jitter = 1.0;
        const auto d = dabs_of(j, 20, line(0, 100, 0));
        QVERIFY(std::any_of(d.begin(), d.end(), [](const Dab& x) { return x.alpha < 0.5f; }));
        QVERIFY(std::all_of(d.begin(), d.end(), [](const Dab& x) { return x.alpha <= 1.0f; }));
    }

    // brush_tests build_up_accumulates_over_time: the airbrush keeps laying dabs while the pen stays (the points' times).
    void buildUpAccumulatesOverTime() {
        DabSettings b = brush();
        b.build_up = true;
        b.build_up_rate = 20;
        b.flow = 0.05;
        const auto hold = [&](double ms) {
            InputPoint a = pt(50, 50), z = a;
            z.time = ms;
            return std::make_pair(dabs_of(b, 20, {a, z}).size(), paint(b, 20, {a, z}).a(50, 50));
        };
        const auto [n0, a0] = hold(0);
        const auto [n1, a1] = hold(500);
        const auto [n2, a2] = hold(2000);
        QCOMPARE(n0, std::size_t(1));
        QCOMPARE(n1, std::size_t(11));
        QCOMPARE(n2, std::size_t(41));
        QVERIFY(a0 < a1 && a1 < a2);
        b.build_up = false;
        InputPoint a = pt(50, 50), z = a;
        z.time = 2000;
        QCOMPARE(dabs_of(b, 20, {a, z}).size(), std::size_t(1));
    }

    // brush_tests texture_modulates_coverage (at 300 dpi: a texture's pixel is a page pixel at 100 %), the ten modes.
    void textureModulatesCoverage() {
        const std::string tile = kept(GrayTile::from_fn(8, 8, [](std::uint32_t x, std::uint32_t) { return x < 4 ? 1.0f : 0.0f; }));
        const auto tex = [&](DabMaskMode mode, double depth, bool each_tip) {
            DabSettings s = brush();
            s.texture.enabled = true;
            s.texture.pattern.asset = tile;
            s.texture.mode = mode;
            s.texture.depth = depth;
            s.texture.each_tip = each_tip;
            return s;
        };
        constexpr int kDpi = 300;
        for (const bool each_tip : {false, true}) {
            const Painted p = paint(with(tex(DabMaskMode::Multiply, 1.0, each_tip), 40, kDpi), line(20, 80, 30, kDpi), kDpi);
            QVERIFY2(p.a(46, 30) < 0.1f, "a dark texel blocks the paint");
            QVERIFY2(p.a(41, 30) > 0.9f, "a light texel lets it through");
            const Painted half = paint(with(tex(DabMaskMode::Multiply, 0.5, each_tip), 40, kDpi), line(20, 80, 30, kDpi), kDpi);
            const float v = half.a(46, 30);
            if (each_tip) {
                QVERIFY2(v > 0.5f && v < 1.0f, qPrintable(QString::number(v)));
            } else {
                QVERIFY2(std::abs(v - 0.5f) < 0.02f, qPrintable(QString::number(v)));
            }
        }
        DabSettings inv = tex(DabMaskMode::Multiply, 1.0, false);
        inv.texture.invert = true;
        Painted s = paint(with(inv, 40, kDpi), line(20, 80, 30, kDpi), kDpi);
        QVERIFY(s.a(46, 30) > 0.9f && s.a(41, 30) < 0.1f);
        inv.texture.invert = false;
        inv.texture.brightness = 1.0;
        s = paint(with(inv, 40, kDpi), line(20, 80, 30, kDpi), kDpi);
        QVERIFY(s.a(46, 30) > 0.9f);
        for (const DabMaskMode m : {DabMaskMode::Multiply, DabMaskMode::Subtract, DabMaskMode::Darken, DabMaskMode::Overlay, DabMaskMode::ColorDodge,
                                    DabMaskMode::ColorBurn, DabMaskMode::LinearBurn, DabMaskMode::HardMix, DabMaskMode::LinearHeight, DabMaskMode::Height}) {
            for (const float t : {0.0f, 0.3f, 1.0f}) {
                QCOMPARE(mask_combine(m, 0.0f, t, 1.0f), 0.0f);
                const float v = mask_combine(m, 0.7f, t, 0.8f);
                QVERIFY(v >= 0.0f && v <= 1.0f);
            }
            QVERIFY(mask_combine(m, 0.6f, 1.0f, 1.0f) >= 0.6f - 1e-5f || m == DabMaskMode::Overlay);
        }
        DabSettings jit = tex(DabMaskMode::Multiply, 0.8, true);
        jit.texture.depth_jitter = DabDynamic{1.0, DabControl::Off, 25, 0.25};
        const auto d = dabs_of(jit, 20, line(0, 200, 0));
        QVERIFY(std::all_of(d.begin(), d.end(), [](const Dab& x) { return x.depth <= 0.8f + 1e-6f && x.depth >= 0.8f * 0.25f - 1e-6f; }));
        QVERIFY(std::any_of(d.begin(), d.end(), [](const Dab& x) { return x.depth < 0.6f; }));
        // the procedural patterns: in range, not flat, the same every time, the noise seamless
        for (const char* style : {"noise", "paper", "canvas", "dots"}) {
            const PatternImage p = pattern(style, 64, 3);
            float var = 0;
            for (const float v : p.data) {
                QVERIFY(v >= 0.0f && v <= 1.0f);
                var += std::abs(v - 0.5f);
            }
            QVERIFY2(var / static_cast<float>(p.data.size()) > 0.02f, style);
            QVERIFY(p.data == pattern(style, 64, 3).data);
        }
        const PatternImage noise = pattern("noise", 64, 1);
        float seam = 0;
        for (int y = 0; y < 64; ++y) seam += std::abs(noise.data[std::size_t(y) * 64] - noise.data[std::size_t(y) * 64 + 63]);
        QVERIFY(seam / 64 < 0.08f);
        for (const GrayTile& t : {chalk_tip(48, 1), spatter_tip(48, 1, 12), bristle_tip(48, 1), charcoal_tip(48, 1), leaf_tip(48), grass_tip(48, 1, 9),
                                  sponge_tip(48, 1), star_tip(48), rake_tip(48, 1, 7)}) {
            QVERIFY(t.valid());
            const auto v = t.to_f32();
            QVERIFY(std::accumulate(v.begin(), v.end(), 0.0f) > 48.0f);
            QCOMPARE(t.get(0, 0), 0.0f);
        }
    }

    // brush_tests dual_brush_intersects.
    void dualBrushIntersects() {
        DabSettings b = brush();
        b.dual.enabled = true;
        b.dual.size_mm = px(6);
        b.dual.spacing = 5.0;
        b.dual.hardness = 1.0;
        const Painted p = paint(b, 30, line(20, 200, 30));
        QVERIFY(p.a(20, 30) > 0.9f && p.a(50, 30) > 0.9f);
        QVERIFY2(p.a(35, 30) < 0.01f, qPrintable(QString::number(p.a(35, 30))));
        QCOMPARE(p.a(20, 50), 0.0f);
        b.dual = core::DabDual{};
        QVERIFY(paint(b, 30, line(20, 200, 30)).a(35, 30) > 0.9f);
    }

    // brush_tests wet_edges_and_noise (the noise's grain at 300 dpi: a page pixel).
    void wetEdgesAndNoise() {
        DabSettings b = brush();
        b.hardness = 0.0;
        b.wet_edges = true;
        const float centre = paint(b, 40, line(20, 120, 50)).a(70, 50);
        QVERIFY2(centre < 0.6f, qPrintable(QString::number(centre)));
        const float single = paint(b, 40, {pt(70, 50)}).a(70, 50);
        QVERIFY(std::abs(centre - single) < 0.05f);
        DabSettings hard = b;
        hard.hardness = 1.0;
        const Painted d = paint(hard, 40, {pt(50, 50)});
        QVERIFY(d.a(68, 50) > d.a(50, 50) + 0.2f);
        constexpr int kDpi = 300;
        DabSettings soft = brush();
        soft.hardness = 0.0;
        const Painted a = paint(with(soft, 40, kDpi), {pt(50, 50, 1.0, kDpi)}, kDpi);
        soft.noise = true;
        const Painted n = paint(with(soft, 40, kDpi), {pt(50, 50, 1.0, kDpi)}, kDpi);
        const auto extreme = [](const Painted& s) {
            int count = 0;
            for (int y = 30; y < 70; ++y)
                for (int x = 30; x < 70; ++x) count += (s.a(x, y) < 0.15f || s.a(x, y) > 0.85f) ? 1 : 0;
            return count;
        };
        QVERIFY2(extreme(n) > extreme(a) + 100, qPrintable(QString("%1 %2").arg(extreme(n)).arg(extreme(a))));
    }

    // brush_tests smoothing_modes: the window's smoothing (exponential, catch-up at the end, the pulled string, the
    // catch-up while the pen stays).
    void smoothingModes() {
        std::vector<InputPoint> zig;
        for (int i = 0; i < 40; ++i) zig.push_back(pt(i * 5.0, i % 2 == 0 ? 0.0 : 6.0));
        const auto dev = [](const std::vector<InputPoint>& p, std::size_t from, std::size_t to) {
            double s = 0;
            for (std::size_t i = from; i < to; ++i) s += std::abs(p[i].y - px(3));
            return s / static_cast<double>(to - from);
        };
        const double string = px(100);
        Smoothing cfg;
        cfg.amount = 0.8;
        const auto sm = smooth_points(cfg, string, zig);
        QVERIFY(dev(sm, 5, sm.size() - 1) < dev(zig, 0, zig.size()) * 0.5);
        QCOMPARE(sm.back().x, zig.back().x);
        QCOMPARE(sm.back().y, zig.back().y);
        Smoothing no_end = cfg;
        no_end.catch_up_on_end = false;
        QVERIFY(smooth_points(no_end, string, zig).back().x < zig.back().x - px(1));
        Smoothing ps;
        ps.amount = 0.2;
        ps.pulled_string = true;
        ps.catch_up_on_end = false;
        std::vector<InputPoint> jitter;
        for (int i = 0; i < 20; ++i) jitter.push_back(pt(i % 2 == 0 ? 0.0 : 5.0, 0));
        QCOMPARE(smooth_points(ps, string, jitter).size(), std::size_t(1));
        auto far = smooth_points(ps, string, {pt(0, 0), pt(100, 0)});
        QVERIFY(std::abs(far.back().x - px(80)) < 1e-9);
        far = smooth_points(ps, string / 2, {pt(0, 0), pt(100, 0)});  // (200 %: the string half as long on the page)
        QVERIFY(std::abs(far.back().x - px(90)) < 1e-9);
        InputPoint a = pt(0, 0);
        InputPoint b = pt(100, 0);
        b.time = 16;
        InputPoint c = b;
        c.time = 500;
        Smoothing cu;
        cu.amount = 0.8;
        cu.catch_up_on_end = false;
        Smoother s(cu, string);
        std::vector<InputPoint> out;
        for (const InputPoint& p : {a, b, c}) s.push(p, out);
        QVERIFY(out.back().x > px(99));
        cu.catch_up = false;
        Smoother s2(cu, string);
        out.clear();
        for (const InputPoint& p : {a, b, c}) s2.push(p, out);
        QVERIFY(out.back().x < px(70));
    }

    // brush_tests pose_overrides_stylus_values.
    void poseOverridesStylus() {
        DabSettings b = brush();
        b.pose.enabled = true;
        b.pose.pressure = 0.5;
        b.pose.override_pressure = true;
        b.shape.enabled = true;
        b.shape.size.control = DabControl::Pressure;
        for (const Dab& d : dabs_of(b, 40, line(0, 50, 0))) QVERIFY(std::abs(d.radius - px(10)) < 1e-9);
    }

    // This build's: the dabs are fixed in mm — the same list whatever the resolution, and the line's coverage the same
    // shape at 200 and 600 dpi (its covered area in mm² and its centre the same within a pixel of 200 dpi).
    void theDabsAreTheSameAtEveryResolution() {
        DabSettings b = brush();
        b.seed = 77;
        b.shape.enabled = true;
        b.shape.size.jitter = 0.5;
        b.scatter.enabled = true;
        b.scatter.scatter.jitter = 1.0;
        b.hardness = 0.6;
        LineBrush lb;
        lb.settings = b;
        lb.size_mm = 2.0;
        lb.seed = 77;
        std::vector<InputPoint> pts;
        for (int i = 0; i <= 12; ++i) {
            InputPoint p;
            p.x = 5.0 + i * 3.0;
            p.y = 20.0 + std::sin(i * 0.5) * 6.0;
            p.pressure = 0.4 + 0.05 * i;
            pts.push_back(p);
        }
        const auto dabs = all_dabs(lb, pts);
        QVERIFY(dabs.size() > 50);
        std::array<double, 3> mass{}, mx{}, my{};
        const std::array<int, 3> dpis{200, 300, 600};
        for (std::size_t k = 0; k < dpis.size(); ++k) {
            const int dpi = dpis[k];
            const auto cover = lay(context(b, dpi), dabs, {}, Box{0, 0, 2000, 2000}, false);
            const double px_mm = 25.4 / dpi;
            for (int y = cover->box.y0; y < cover->box.y1; ++y) {
                for (int x = cover->box.x0; x < cover->box.x1; ++x) {
                    const double a = cover->at(x, y);
                    mass[k] += a * px_mm * px_mm;
                    mx[k] += a * (x + 0.5) * px_mm * px_mm * px_mm;
                    my[k] += a * (y + 0.5) * px_mm * px_mm * px_mm;
                }
            }
        }
        for (std::size_t k = 1; k < dpis.size(); ++k) {
            QVERIFY2(std::abs(mass[k] - mass[0]) / mass[0] < 0.05, qPrintable(QString("%1 %2").arg(mass[k]).arg(mass[0])));
            QVERIFY(std::abs(mx[k] / mass[k] - mx[0] / mass[0]) < 25.4 / 200);
            QVERIFY(std::abs(my[k] / mass[k] - my[0] / mass[0]) < 25.4 / 200);
        }
    }

    // A part of the page laid alone is what the whole lays there (each pixel only its own dabs', in their order): the
    // screen's tiles and the print's bands draw the same line. And the same line laid again is the same, bit for bit.
    void aPartIsTheWhole() {
        DabSettings b = brush();
        b.seed = 3;
        b.hardness = 0.3;
        b.flow = 0.3;
        b.wet_edges = false;
        b.scatter.enabled = true;
        b.scatter.scatter.jitter = 1.5;
        b.scatter.count = 2;
        b.dual.enabled = true;
        b.dual.size_mm = px(5);
        b.dual.scatter = 1.0;
        b.texture.enabled = true;
        b.texture.depth = 0.6;
        const LineBrush lb = with(b, 24);
        std::vector<InputPoint> pts;
        for (int i = 0; i < 60; ++i) pts.push_back(pt(20 + i * 4.0, 60 + std::sin(i * 0.3) * 30));
        const Painted whole = paint(lb, pts);
        const Box part{100, 40, 164, 104};
        const Painted piece = paint(lb, pts, 200, part);
        for (int y = part.y0; y < part.y1; ++y)
            for (int x = part.x0; x < part.x1; ++x) QCOMPARE(piece.a(x, y), whole.a(x, y));
        QCOMPARE(piece.a(part.x0 - 1, part.y0), 0.0f);  // (nothing laid past the part)
        const Painted again = paint(lb, pts);
        for (int y = 0; y < 150; ++y)
            for (int x = 0; x < 300; ++x) QCOMPARE(again.a(x, y), whole.a(x, y));
        QVERIFY(sum_alpha(whole, Box{0, 0, 300, 150}) > 100.0f);
    }

    // A line's seed: its brush's xor its id's FNV-1a — two lines of one brush jitter apart, the same line the same.
    void aLinesSeedIsItsOwn() {
        DabSettings b = brush();
        b.seed = 9;
        b.shape.enabled = true;
        b.shape.angle.jitter = 1.0;
        LineInputs inputs;
        const auto pts = input_points(core::PenPoints{{1, 1, std::nullopt}, {20, 1, std::nullopt}}, {}, inputs);
        const auto first = line_dabs(line_brush(b, 1.0, "line-a", inputs), pts);
        const auto again = line_dabs(line_brush(b, 1.0, "line-a", inputs), pts);
        const auto other = line_dabs(line_brush(b, 1.0, "line-b", inputs), pts);
        QVERIFY(first->dabs == again->dabs);
        QVERIFY(first->dabs != other->dabs);
        QCOMPARE(line_brush(b, 1.0, "line-a", inputs).seed, std::uint64_t(9) ^ fnv1a("line-a"));
        // a line's inputs: one a point (lists that do not fit are not used), a mouse's without them
        const std::vector<double> tilt{10, 20, 30, 40}, time{0, 16}, wheel{0.2, 0.8}, wrong{1};
        LineInputs given;
        given.tilt = tilt;
        given.time_ms = time;
        given.wheel = wheel;
        const auto in = input_points(core::PenPoints{{1, 1, 0.5}, {2, 2, std::nullopt}}, std::vector<double>{5, 6}, given);
        QCOMPARE(in[0].tilt_x, 10.0);
        QCOMPARE(in[1].tilt_y, 40.0);
        QCOMPARE(in[1].time, 16.0);
        QCOMPARE(in[0].wheel, 0.2);
        QCOMPARE(in[1].rotation, 6.0);
        QCOMPARE(in[0].pressure, 0.5);
        QCOMPARE(in[1].pressure, 1.0);
        LineInputs bad;
        bad.wheel = wrong;
        QCOMPARE(input_points(core::PenPoints{{1, 1, std::nullopt}, {2, 2, std::nullopt}}, {}, bad)[0].wheel, 1.0);
        // too many dabs: refused
        DabSettings dense = brush();
        dense.spacing = 0.01;
        bool refused = false;
        try {
            (void)line_dabs(line_brush(dense, 0.01, "x", inputs), input_points(core::PenPoints{{0, 0, std::nullopt}, {2000, 0, std::nullopt}}, {}, inputs));
        } catch (const core::Error& error) {
            refused = error.code() == "too_large";
        }
        QVERIFY(refused);
    }

    // define_brush's "format": "photocraft": upstream's settings (camelCase, pixels of 200 dpi) as Genko's — the sizes in
    // mm, the controls and modes by their names, a sampled tip and a texture tile as pictures — and what is the line's
    // or the window's said, not used.
    void upstreamSettingsAreRead() {
        const core::Json up = core::Json::parse(R"({
            "size": 40, "hardness": 0.5, "spacing": 0.2, "pressureSize": false, "color": [1, 0, 0, 1], "mode": "multiply",
            "smoothing": {"amount": 0.5},
            "shapeDynamics": {"enabled": true, "size": {"jitter": 0.3, "control": "penPressure", "minimum": 0.1}, "flipXJitter": true},
            "scattering": {"enabled": true, "scatter": {"jitter": 2}, "bothAxes": true, "count": 3},
            "texture": {"enabled": true, "pattern": {"procedural": {"style": "canvas", "size": 64, "seed": 2}}, "mode": "colorBurn", "eachTip": true},
            "dualBrush": {"enabled": true, "size": 10, "mode": "hardMix", "tip": {"sampled": {"width": 2, "height": 1, "data": [0.0, 1.0]}}},
            "colorDynamics": {"enabled": true, "perTip": false, "fgBg": {"control": "fade", "fadeSteps": 30}},
            "transfer": {"enabled": true, "flow": {"control": "stylusWheel"}},
            "pose": {"enabled": true, "tiltX": 30, "overrideTilt": true},
            "wetEdges": true, "seed": 12})");
        std::map<std::string, core::Bytes> pictures;
        std::vector<std::string> ignored;
        const core::Json mine = dabs_from_photocraft(up, pictures, ignored);
        const DabSettings s = core::dabs_from_json(mine);
        QVERIFY(std::abs(s.size_mm - 40 * kPx) < 1e-12);
        QCOMPARE(s.hardness, 0.5);
        QVERIFY(!s.pressure_size);
        QVERIFY(s.shape.enabled && s.shape.size.control == DabControl::Pressure && s.shape.flip_x_jitter);
        QCOMPARE(s.scatter.count, std::int64_t(3));
        QCOMPARE(s.texture.pattern.procedural, std::string("canvas"));
        QCOMPARE(s.texture.mode, DabMaskMode::ColorBurn);
        QVERIFY(s.texture.each_tip);
        QVERIFY(std::abs(s.dual.size_mm - 10 * kPx) < 1e-12);
        QCOMPARE(s.dual.mode, DabMaskMode::HardMix);
        QCOMPARE(pictures.size(), std::size_t(1));
        QCOMPARE(s.dual.tip, pictures.begin()->first);
        QVERIFY(!core::check_tip_png(*pictures.begin()->second, core::kMaxTipEdge));
        QVERIFY(!s.color.per_tip && s.color.fg_bg.control == DabControl::Fade && s.color.fg_bg.fade_steps == 30);
        QCOMPARE(s.transfer.flow.control, DabControl::Wheel);
        QVERIFY(s.pose.override_tilt && s.pose.tilt_x == 30);
        QVERIFY(s.wet_edges);
        QCOMPARE(s.seed, std::int64_t(12));
        const std::set<std::string> said(ignored.begin(), ignored.end());
        QVERIFY(said.contains("color") && said.contains("mode") && said.contains("smoothing"));
        bool refused = false;
        try {
            (void)dabs_from_photocraft(core::Json::parse(R"({"shapeDynamics": {"size": {"control": "nope"}}})"), pictures, ignored);
        } catch (const core::Error&) {
            refused = true;
        }
        QVERIFY(refused);
        // a picture file as a tip: its alpha (transparent) or its darkness; as a texture its lightness; refused past its edge
        const std::string grey = render::write_png(render::Image::frombytes("L", render::Size{4, 2}, std::string("\x00\x40\x80\xff\x00\x40\x80\xff", 8)));
        const auto tip = take_in_picture(grey, true, 2500);
        QVERIFY(!core::check_tip_png(*tip.second, 2500));
        QCOMPARE(tip.first, storage::AssetStore::ref(*tip.second));
        const GrayTile t = tile_of_png(*tip.second);
        QCOMPARE(t.get(3, 0), 1.0f);  // (a grey picture: its values as they are)
        const std::string rgb = render::write_png(render::Image::create("RGB", render::Size{3, 3}, render::Ink{0, 0, 0}));
        QCOMPARE(tile_of_png(*take_in_picture(rgb, true, 2500).second).get(1, 1), 1.0f);   // (black: all ink)
        QCOMPARE(tile_of_png(*take_in_picture(rgb, false, 1024).second).get(1, 1), 0.0f);  // (a texture: its lightness)
        bool big = false;
        try {
            (void)take_in_picture(rgb, true, 2);
        } catch (const core::Error&) {
            big = true;
        }
        QVERIFY(big);
    }
};

QTEST_GUILESS_MAIN(TestDabEngine)
#include "test_dab_engine.moc"
