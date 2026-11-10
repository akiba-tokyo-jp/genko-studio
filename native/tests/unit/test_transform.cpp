// PC3 C-c (SPEC.md COLOR-07, ARCHITECTURE.md「層の変形」): the transforms' algorithms as PhotoCraft's own tests check them —
// algo/src/puppet.rs (773-): the mesh covers the shape only and is finer at more points, no pins or unmoved pins leave the
// picture as it was at every depth, one pin moves it rigidly and a turned pin turns it, the ARAP energy falls and the
// modes differ, a pin's depth orders the triangles; algo/src/perspective.rs (265-): the identity, a square onto a
// trapezoid exactly a homography, a shared edge continuous, straighten levels and plumbs; algo/src/liquify.rs (678-): no
// stroke and a stroke of no pressure change nothing, Forward Warp moves along the stroke and Reconstruct All undoes it,
// the Reconstruct brush shrinks the field and Freeze protects (Thaw, Mask All, Invert, None), each tool's sense, the same
// strokes the same field (8 and 32 bits alike); algo/src/seam.rs (240-): a seam follows low energy, shrinking keeps a
// salient bar and enlarging doubles flat columns, heights and protection, the skin rule; geom/src/warp.rs and
// warp_cmds.rs: a warp's normal form read back the same, a split warp the same surface and its removal the warp before.
// And what this build adds: the triangles and the inverse map of render/resample draw the identity exactly, Liquify's
// forward map undoes its field.

#include <QtTest>

#include <cmath>
#include <numeric>
#include <vector>

#include "core/json.hpp"
#include "core/page_geometry.hpp"
#include "render/layer_transform.hpp"
#include "render/liquify_field.hpp"
#include "render/perspective_warp.hpp"
#include "render/puppet.hpp"
#include "render/resample.hpp"
#include "render/seam.hpp"

using namespace genko;
using core::Json;
namespace rs = render::resample;
namespace pp = render::puppet;
namespace pw = render::perspective;
namespace lq = render::liquify;
namespace sm = render::seam;
namespace lt = render::layer_transform;
namespace pg = core::page_geometry;

namespace {

// An L: a bar from (20, 10) to (36, 70) and a foot from (36, 56) to (70, 70), on an 80 × 80 plane.
rs::Plane blob(bool hdr) {
    rs::Plane p(80, 80, 4, true);
    p.hdr = hdr;
    for (int y = 10; y < 70; ++y) {
        for (int x = 20; x < 36; ++x) {
            float* v = p.at(x, y);
            v[0] = static_cast<float>((x + y) % 7) / 6.0f;
            v[1] = 0.5f;
            v[2] = static_cast<float>(y) / 80.0f;
            v[3] = 1.0f;
        }
    }
    for (int y = 56; y < 70; ++y) {
        for (int x = 36; x < 70; ++x) {
            float* v = p.at(x, y);
            v[0] = 0.2f;
            v[1] = static_cast<float>((x * 3) % 5) / 4.0f;
            v[2] = 0.8f;
            v[3] = 1.0f;
        }
    }
    return p;
}

pp::Coverage coverage(const rs::Plane& p) {
    pp::Coverage c;
    c.width = p.width;
    c.height = p.height;
    for (int y = 0; y < p.height; ++y)
        for (int x = 0; x < p.width; ++x) c.alpha.push_back(p.at(x, y)[3]);
    return c;
}

// The puppet's picture: its triangles drawn at their places (as render/layer_transform draws them).
rs::Plane puppet_picture(const rs::Plane& src, const pp::Deformed& d, rs::Interp interp) {
    std::vector<rs::Corner> corners;
    for (std::size_t i = 0; i < d.verts.size(); ++i) corners.push_back(rs::Corner{d.verts[i][0], d.verts[i][1], d.mesh.verts[i][0], d.mesh.verts[i][1]});
    std::vector<std::array<std::size_t, 3>> tris;
    for (const std::size_t t : d.order) tris.push_back(d.mesh.tris[t]);
    return rs::triangles(src, corners, tris, src.width, src.height, interp);
}

float worst(const rs::Plane& a, const rs::Plane& b) {
    float most = 0.0f;
    for (std::size_t i = 0; i < a.v.size(); ++i) most = std::max(most, std::fabs(a.v[i] - b.v[i]));
    return most;
}

std::array<int, 4> content(const rs::Plane& p) {
    int x0 = p.width, y0 = p.height, x1 = -1, y1 = -1;
    for (int y = 0; y < p.height; ++y) {
        for (int x = 0; x < p.width; ++x) {
            if (!(p.at(x, y)[p.channels - 1] > 0.0f)) continue;
            x0 = std::min(x0, x);
            y0 = std::min(y0, y);
            x1 = std::max(x1, x + 1);
            y1 = std::max(y1, y + 1);
        }
    }
    return {x0, y0, x1, y1};
}

double d2(const pp::XY& a, const pp::XY& b) { return (a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]); }

lq::Stroke stroke(lq::Tool tool, double size, std::vector<std::array<double, 3>> points) {
    lq::Stroke s;
    s.tool = tool;
    s.size = size;
    s.points = std::move(points);
    return s;
}

// A liquify field over a 96 × 64 "pixel" box (unit 1: the upstream's pixels).
lq::Field field() { return lq::Field(0.0, 0.0, 96.0, 64.0, 2.0, 1.0); }

std::vector<float> bar(std::size_t w, std::size_t h) {
    std::vector<float> v(w * h * 3, 0.5f);
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 10; x < 14; ++x) {
            const std::array<float, 3> c = x % 2 == 0 ? std::array<float, 3>{1, 0, 0} : std::array<float, 3>{0, 0, 1};
            std::copy(c.begin(), c.end(), v.begin() + static_cast<std::ptrdiff_t>((y * w + x) * 3));
        }
    }
    return v;
}

std::size_t red_columns(std::size_t w, std::size_t h, const std::vector<float>& img) {
    std::size_t n = 0;
    for (std::size_t x = 0; x < w; ++x) {
        bool pure = true;
        for (std::size_t y = 0; y < h && pure; ++y) {
            const float* p = img.data() + (y * w + x) * 3;
            pure = p[1] < 0.05f && ((p[0] > 0.95f && p[2] < 0.05f) || (p[2] > 0.95f && p[0] < 0.05f));
        }
        if (pure) ++n;
    }
    return n;
}

}  // namespace

class TestTransform : public QObject {
    Q_OBJECT

private slots:
    // --- puppet.rs ---

    void puppetMeshCoversTheShapeOnly() {
        const pp::Coverage cov = coverage(blob(false));
        const pp::Mesh m = pp::build_mesh(cov, pp::Density::Normal, 2.0);
        QVERIFY(m.tris.size() > 20);
        const auto inside = [&](double x, double y) { return m.locate(x, y).has_value(); };
        QVERIFY(inside(28, 30) && inside(60, 62));
        QVERIFY2(!inside(60, 20), "the empty corner of the L has no mesh");
        const pp::Mesh more = pp::build_mesh(cov, pp::Density::More, 2.0);
        const pp::Mesh fewer = pp::build_mesh(cov, pp::Density::Fewer, 2.0);
        QVERIFY(more.tris.size() > m.tris.size() && m.tris.size() > fewer.tris.size());
    }

    void noPinsOrUnmovedPinsAreTheIdentity() {
        for (const bool hdr : {false, true}) {
            const rs::Plane s = blob(hdr);
            for (const std::vector<pp::Pin>& pins : {std::vector<pp::Pin>{}, std::vector<pp::Pin>{pp::Pin{{28, 20}, {28, 20}, {}, 0}, pp::Pin{{60, 62}, {60, 62}, {}, 0}}}) {
                pp::Warp w;
                w.pins = pins;
                QVERIFY(w.is_identity());
                const pp::Deformed d = pp::deform(coverage(s), w);
                const rs::Plane out = puppet_picture(s, d, rs::Interp::Bicubic);
                QVERIFY2(worst(s, out) <= 1.0f / 255.0f + 1e-6f, std::to_string(worst(s, out)).c_str());
            }
        }
    }

    void aSinglePinMovesRigidly() {
        const rs::Plane s = blob(false);
        pp::Warp w;
        w.pins = {pp::Pin{{28, 30}, {38, 25}, {}, 0}};
        w.mode = pp::Mode::Rigid;
        const pp::Deformed d = pp::deform(coverage(s), w);
        for (std::size_t i = 0; i < d.verts.size(); ++i) {
            QVERIFY(std::fabs(d.verts[i][0] - d.mesh.verts[i][0] - 10.0) < 1e-3);
            QVERIFY(std::fabs(d.verts[i][1] - d.mesh.verts[i][1] + 5.0) < 1e-3);
        }
        const rs::Plane out = puppet_picture(s, d, rs::Interp::Bilinear);
        const auto a = content(s), b = content(out);
        QCOMPARE(b[0], a[0] + 10);
        QCOMPARE(b[1], a[1] - 5);
        QCOMPARE(b[2], a[2] + 10);
        QCOMPARE(b[3], a[3] - 5);
        for (int k = 0; k < 4; ++k) QVERIFY(std::fabs(out.at(30, 20)[k] - s.at(20, 25)[k]) < 1e-6f);
        // (a fixed turn turns the whole mesh about the pin: (dx, dy) → (−dy, dx) for +90° with y down)
        w.pins = {pp::Pin{{28, 30}, {28, 30}, 90.0, 0}};
        const pp::Deformed t = pp::deform(coverage(s), w);
        std::size_t far = 0;
        for (std::size_t i = 1; i < t.mesh.verts.size(); ++i)
            if (t.mesh.verts[i][0] > t.mesh.verts[far][0]) far = i;
        const double dx = t.mesh.verts[far][0] - 28.0, dy = t.mesh.verts[far][1] - 30.0;
        QVERIFY(std::fabs(t.verts[far][0] - (28.0 - dy)) < 1.0 && std::fabs(t.verts[far][1] - (30.0 + dx)) < 1.0);
    }

    void theEnergyFallsAndTheModesDiffer() {
        const pp::Mesh mesh = pp::build_mesh(coverage(blob(false)), pp::Density::Normal, 2.0);
        const std::vector<pp::XY> src{{28.0, 14.0}, {64.0, 62.0}, {28.0, 62.0}};
        const std::vector<pp::XY> dst{{10.0, 30.0}, {70.0, 50.0}, {28.0, 62.0}};
        const pp::Solver solver(mesh, src, {false, false, false});
        const std::vector<std::optional<double>> rot(3);
        const auto [v, e] = solver.solve(dst, rot, pp::Mode::Rigid, 30);
        QCOMPARE(e.size(), std::size_t(30));
        const double start = solver.energy(solver.solve(dst, rot, pp::Mode::Rigid, 0).first, dst, rot);
        QVERIFY(e[0] <= start * (1.0 + 1e-9));
        for (std::size_t i = 1; i < e.size(); ++i) QVERIFY(e[i] <= e[i - 1] * (1.0 + 1e-6) + 1e-9);
        QVERIFY(e.back() < start);
        const std::vector<pp::XY> pinned = solver.pinned(v);
        for (std::size_t k = 0; k < dst.size(); ++k) QVERIFY(d2(pinned[k], dst[k]) < 0.25);
        const auto distorted = solver.solve(dst, rot, pp::Mode::Distort, 30).first;
        QVERIFY(distorted != v);
    }

    void aPinsDepthOrdersTheTriangles() {
        const pp::Mesh mesh = pp::build_mesh(coverage(blob(false)), pp::Density::Fewer, 2.0);
        const std::vector<pp::XY> pins{{28.0, 14.0}, {64.0, 62.0}};
        const pp::Solver solver(mesh, pins, {false, false});
        const std::vector<std::size_t> order = solver.draw_order({1, 0}, pins);
        const pp::Mesh& m = solver.mesh();
        const auto near0 = [&](std::size_t t) {
            pp::XY c{0, 0};
            for (const std::size_t i : m.tris[t]) {
                c[0] += m.verts[i][0] / 3.0;
                c[1] += m.verts[i][1] / 3.0;
            }
            return d2(c, pins[0]) < d2(c, pins[1]);
        };
        const auto k = std::find_if(order.begin(), order.end(), near0);
        QVERIFY(k != order.end());
        QVERIFY(std::none_of(order.begin(), k, near0) && std::all_of(k, order.end(), near0));
    }

    // --- perspective.rs ---

    void perspectiveIdentityAndAKnownHomography() {
        const pw::Quad q{{{10, 10}, {90, 12}, {85, 70}, {12, 75}}};
        const auto id = pw::Map::make({pw::Plane{q, q}}, 0.5);
        QVERIFY(id.has_value());
        for (const auto& [x, y] : {std::pair{20.0, 20.0}, {50.0, 40.0}, {0.0, 0.0}, {120.0, 90.0}}) {
            const auto p = id->map(x, y);
            QVERIFY(std::fabs(p[0] - x) <= 1e-9 && std::fabs(p[1] - y) <= 1e-9);
        }
        const pw::Quad src{{{0, 0}, {100, 0}, {100, 100}, {0, 100}}};
        const pw::Quad dst{{{20, 0}, {80, 0}, {100, 100}, {0, 100}}};
        const auto m = pw::Map::make({pw::Plane{src, dst}}, 0.5);
        for (std::size_t k = 0; k < 4; ++k) {
            const auto p = m->map(src[k][0], src[k][1]);
            QVERIFY(std::fabs(p[0] - dst[k][0]) <= 1e-9 && std::fabs(p[1] - dst[k][1]) <= 1e-9);
        }
        const auto h = rs::Homography::rect_to_quad({0, 0, 100, 100}, {{{20, 0}, {80, 0}, {100, 100}, {0, 100}}});
        const auto c = m->map(50, 50);
        const auto hc = h->apply(50, 50);
        QVERIFY(std::fabs(c[0] - hc.first) <= 1e-9 && std::fabs(c[1] - hc.second) <= 1e-9);
        QVERIFY(std::fabs(c[0] - 50.0) < 1e-9 && c[1] < 50.0);
    }

    void aSharedEdgeStaysContinuous() {
        const pw::Plane a{{{{0, 0}, {50, 0}, {50, 100}, {0, 100}}}, {{{0, 10}, {50, 0}, {55, 100}, {0, 90}}}};
        const pw::Plane b{{{{50.2, 0}, {100, 0}, {100, 100}, {50, 100.2}}}, {{{50, 0}, {110, 20}, {100, 80}, {55, 100}}}};
        const auto m = pw::Map::make({a, b}, 0.5);
        QVERIFY(m.has_value());
        for (const double t : {0.05, 0.25, 0.5, 0.77, 0.95}) {
            const double x = 50.1 - 0.1 * t, y = 100.1 * t;
            const auto l = m->map(x - 1e-6, y), r = m->map(x + 1e-6, y);
            QVERIFY2(std::fabs(l[0] - r[0]) <= 1e-4 && std::fabs(l[1] - r[1]) <= 1e-4, std::to_string(t).c_str());
        }
        QCOMPARE(pw::linked_corners({pw::Plane{}}, 0.5).size(), std::size_t(1));
    }

    void straightenLevelsAndPlumbs() {
        std::vector<pw::Plane> ps{pw::Plane{{{{0, 0}, {100, 0}, {100, 100}, {0, 100}}}, {{{0, 3}, {100, -2}, {96, 100}, {4, 104}}}}};
        pw::straighten(ps, pw::Straighten::Vertical, 0.5);
        QCOMPARE(ps[0].dst[1][0], ps[0].dst[2][0]);
        QCOMPARE(ps[0].dst[0][0], ps[0].dst[3][0]);
        QVERIFY(ps[0].dst[0][1] != ps[0].dst[1][1]);
        pw::straighten(ps, pw::Straighten::Auto, 0.5);
        QCOMPARE(ps[0].dst[0][1], ps[0].dst[1][1]);
        QCOMPARE(ps[0].dst[2][1], ps[0].dst[3][1]);
    }

    // --- liquify.rs ---

    void noStrokeChangesNothing() {
        lq::Field f = field();
        QVERIFY(f.is_identity());
        lq::Stroke s = stroke(lq::Tool::ForwardWarp, 30.0, {{40, 30, 1}, {60, 30, 1}});
        s.pressure = 0;
        f.apply(s);
        QVERIFY(f.is_identity());
        QVERIFY(!f.moved_box().has_value());
    }

    void forwardWarpMovesAlongAndReconstructAllUndoes() {
        lq::Field f = field();
        f.apply(stroke(lq::Tool::ForwardWarp, 40.0, {{40, 32, 1}, {52, 32, 1}}));
        const auto d = f.sample(52, 32);
        QVERIFY2(d[0] < -4.0 && std::fabs(d[1]) < 0.5, (std::to_string(d[0]) + " " + std::to_string(d[1])).c_str());
        // (the forward map takes a point of the picture to where it shows now: the inverse of the field)
        const auto q = f.forward(46.0, 32.0);
        const auto back = f.sample_cubic(q[0], q[1]);
        QVERIFY(std::fabs(q[0] + back[0] - 46.0) < 1e-4 && std::fabs(q[1] + back[1] - 32.0) < 1e-4);
        QVERIFY(q[0] > 46.0);
        lq::Stroke all = stroke(lq::Tool::ReconstructAll, 10.0, {});
        all.amount = 100.0;
        f.apply(all);
        QVERIFY(f.is_identity());
    }

    void reconstructShrinksAndFreezeProtects() {
        const lq::Stroke s = stroke(lq::Tool::ForwardWarp, 40.0, {{30, 32, 1}, {45, 32, 1}});
        lq::Field f = field();
        f.apply(s);
        const double before = f.max_displacement();
        f.apply(stroke(lq::Tool::Reconstruct, 80.0, std::vector<std::array<double, 3>>(30, {40, 32, 1})));
        QVERIFY(f.max_displacement() < before * 0.2);
        lq::Stroke freeze = stroke(lq::Tool::Freeze, 60.0, {{40, 32, 1}});
        freeze.density = 0;
        lq::Field g = field();
        g.apply(freeze);
        QVERIFY(g.freeze_at(40, 32) > 0.99);
        g.apply(s);
        QVERIFY(std::fabs(g.sample(40, 32)[0]) < 1e-3);
        lq::Stroke thaw = freeze;
        thaw.tool = lq::Tool::Thaw;
        g.apply(thaw);
        QVERIFY(g.freeze_at(40, 32) < 0.01);
        g.apply(stroke(lq::Tool::FreezeAll, 1.0, {}));
        QVERIFY(g.freeze_at(5, 5) > 0.99 && g.freeze_at(90, 60) > 0.99);
        g.apply(stroke(lq::Tool::InvertFreeze, 1.0, {}));
        QVERIFY(g.freeze_at(40, 32) < 0.01);
        g.apply(freeze);
        g.apply(stroke(lq::Tool::InvertFreeze, 1.0, {}));
        QVERIFY(g.freeze_at(40, 32) < 0.01 && g.freeze_at(5, 5) > 0.99);
        g.apply(stroke(lq::Tool::ThawAll, 1.0, {}));
        QVERIFY(g.freeze_at(5, 5) < 0.01 && g.freeze_at(40, 32) < 0.01);
    }

    void eachToolHasItsSense() {
        const auto at = [](lq::Tool tool, std::vector<std::array<double, 3>> pts) {
            lq::Field f = field();
            f.apply(stroke(tool, 40.0, std::move(pts)));
            return f;
        };
        const std::vector<std::array<double, 3>> still(10, {48, 32, 1});
        QVERIFY(at(lq::Tool::Pucker, still).sample(58, 32)[0] > 0.1);   // (pulled in: it reads from outward)
        QVERIFY(at(lq::Tool::Bloat, still).sample(58, 32)[0] < -0.1);
        QVERIFY(at(lq::Tool::TwirlCw, still).sample(58, 32)[1] < -0.1);  // (right of the centre moves down: read from above)
        QVERIFY(at(lq::Tool::TwirlCcw, still).sample(58, 32)[1] > 0.1);
        QVERIFY(at(lq::Tool::PushLeft, {{48, 44, 1}, {48, 20, 1}}).sample(48, 32)[0] > 0.5);
        lq::Field g = at(lq::Tool::ForwardWarp, {{40, 32, 1}, {50, 32, 1}});
        const double m0 = g.max_displacement();
        g.apply(stroke(lq::Tool::Smooth, 60.0, std::vector<std::array<double, 3>>(20, {45, 32, 1})));
        QVERIFY(g.max_displacement() < m0);
    }

    void theSameStrokesTheSameField() {
        const lq::Stroke s = stroke(lq::Tool::TwirlCw, 50.0, {{48, 32, 0.5}, {50, 33, 1.0}, {52, 34, 1.0}});
        lq::Field a = field(), b = field();
        a.apply(s);
        b.apply(s);
        for (double y = 0; y < 64; y += 3)
            for (double x = 0; x < 96; x += 3) QCOMPARE(a.sample(x, y), b.sample(x, y));
    }

    // --- seam.rs ---

    void aSeamFollowsLowEnergy() {
        std::vector<float> e(5 * 4, 9.0f);
        for (std::size_t y = 0; y < 4; ++y) e[y * 5 + 2] = 0.0f;
        QCOMPARE(sm::find_seam(5, 4, e), std::vector<std::size_t>(4, 2));
    }

    void shrinkingKeepsTheBarAndEnlargingDoublesFlatColumns() {
        const std::vector<float> img = bar(40, 12);
        const std::vector<float> out = sm::carve(40, 12, 3, img, nullptr, 24, 12);
        QCOMPARE(out.size(), std::size_t(24 * 12 * 3));
        QCOMPARE(red_columns(24, 12, out), std::size_t(4));
        QVERIFY(red_columns(24, 12, sm::resize_bilinear(40, 12, 3, img, 24, 12)) < 4);
        const std::vector<float> wide = sm::carve(30, 8, 3, bar(30, 8), nullptr, 50, 8);
        QCOMPARE(wide.size(), std::size_t(50 * 8 * 3));
        QCOMPARE(red_columns(50, 8, wide), std::size_t(4));
    }

    void heightAndProtection() {
        const std::size_t w = 20, h = 30;
        std::vector<float> img(w * h, 0.2f);
        for (std::size_t x = 0; x < w; ++x) img[12 * w + x] = 1.0f;
        const std::vector<float> out = sm::carve(w, h, 1, img, nullptr, w, 18);
        QCOMPARE(out.size(), w * 18);
        std::size_t rows = 0;
        for (std::size_t y = 0; y < 18; ++y) {
            bool all = true;
            for (std::size_t x = 0; x < w; ++x) all = all && out[y * w + x] > 0.9f;
            if (all) ++rows;
        }
        QCOMPARE(rows, std::size_t(1));
        std::vector<float> marked(16 * 4, 0.5f), protect(16 * 4, 0.0f);
        for (std::size_t y = 0; y < 4; ++y) {
            marked[y * 16 + 3] = 0.51f;
            protect[y * 16 + 3] = 1.0f;
        }
        const std::vector<float> kept = sm::carve(16, 4, 1, marked, &protect, 8, 4);
        for (std::size_t y = 0; y < 4; ++y) {
            bool found = false;
            for (std::size_t x = 0; x < 8; ++x) found = found || std::fabs(kept[y * 8 + x] - 0.51f) < 1e-6f;
            QVERIFY(found);
        }
    }

    void theSkinRule() {
        const std::vector<float> px{0.86f, 0.62f, 0.5f, 0.2f, 0.4f, 0.8f};
        QCOMPARE(sm::skin_mask(2, 1, 3, px), (std::vector<float>{1.0f, 0.0f}));
    }

    // --- the warps' forms (geom/src/warp.rs, warp_cmds.rs) ---

    void aWarpsNormalFormReadsBackTheSame() {
        const std::array<double, 4> box{10, 20, 70, 60};
        const pg::Warp arc = lt::warp_from_json(Json{{"style", "arc"}, {"bend", 40}, {"h_distort", -10}, {"vertical", true}}, box);
        const Json once = lt::warp_to_json(arc);
        QCOMPARE(lt::warp_to_json(lt::warp_from_json(once, {0, 0, 1, 1})), once);
        QCOMPARE(once["bounds"], Json::array({10, 20, 70, 60}));
        const pg::Warp grid = lt::warp_from_json(Json{{"grid", Json::array({3, 2})}}, box);
        QVERIFY(grid.style == pg::WarpStyle::Custom && grid.mesh && grid.mesh->us.size() == 4 && grid.mesh->vs.size() == 3);
        QVERIFY(grid.is_identity());
        const Json custom = lt::warp_to_json(grid);
        QCOMPARE(lt::warp_to_json(lt::warp_from_json(custom, box)), custom);
        for (const Json& bad : {Json{{"style", "spiral"}}, Json{{"bend", 200}}, Json{{"style", "custom"}, {"mesh", Json{{"us", Json::array({0, 1})}}}},
                                Json{{"style", "arc"}, {"colour", 1}}}) {
            QVERIFY_THROWS_EXCEPTION(core::OpError, lt::warp_from_json(bad, box));
        }
    }

    void aSplitWarpIsTheSameSurfaceAndItsRemovalTheWarpBefore() {
        const std::array<double, 4> box{0, 0, 100, 50};
        const pg::Warp flag = lt::warp_from_json(Json{{"style", "flag"}, {"bend", 30}}, box);
        const auto split = lt::split_warp(flag, lt::XY{30, 20}, lt::Split::Crosswise);
        QVERIFY(split.has_value());
        QCOMPARE(split->mesh->us.size(), std::size_t(3));
        QCOMPARE(split->mesh->vs.size(), std::size_t(3));
        pg::Warp one;  // (the flag as one patch of the mesh: what the split cuts)
        one.style = pg::WarpStyle::Custom;
        one.box = box;
        one.mesh = flag.to_mesh(1, 1);
        for (const auto& [s, t] : {std::pair{0.1, 0.1}, {0.5, 0.7}, {0.9, 0.4}}) {
            const auto a = one.map(box[0] + s * 100, box[1] + t * 50), b = split->map(box[0] + s * 100, box[1] + t * 50);
            QVERIFY(std::fabs(a.x - b.x) < 1e-9 && std::fabs(a.y - b.y) < 1e-9);
        }
        auto removed = lt::split_warp(*split, lt::XY{30, 20}, lt::Split::Remove);
        QVERIFY(removed.has_value());
        removed = lt::split_warp(*removed, lt::XY{30, 20}, lt::Split::Remove);
        QVERIFY(removed.has_value());
        QCOMPARE(removed->mesh->us.size(), std::size_t(2));
        QCOMPARE(removed->mesh->vs.size(), std::size_t(2));
        for (std::size_t i = 0; i < removed->mesh->points.size(); ++i) {
            QVERIFY(std::fabs(removed->mesh->points[i].x - one.mesh->points[i].x) < 1e-6);
            QVERIFY(std::fabs(removed->mesh->points[i].y - one.mesh->points[i].y) < 1e-6);
        }
        QVERIFY(lt::split_warp(one, std::nullopt, lt::Split::Horizontal).has_value());
        QVERIFY(!lt::split_warp(one, lt::XY{30, 20}, lt::Split::Remove).has_value());  // (no inner edge to take out)
    }

    // --- render/resample's triangles and inverse map ---

    void theTrianglesAndTheInverseMapDrawTheIdentityExactly() {
        const rs::Plane s = blob(true);
        std::vector<rs::Corner> corners{{0, 0, 0, 0}, {80, 0, 80, 0}, {80, 80, 80, 80}, {0, 80, 0, 80}};
        const rs::Plane t = rs::triangles(s, corners, {{0, 1, 2}, {0, 2, 3}}, 80, 80, rs::Interp::Bicubic);
        QCOMPARE(worst(s, t), 0.0f);
        const rs::Plane r = rs::remap(s, [](double x, double y) { return std::pair<double, double>{x, y}; }, 80, 80, rs::Interp::Bicubic);
        QCOMPARE(worst(s, r), 0.0f);
        // (a move by whole pixels through the inverse map: the pixels moved)
        const rs::Plane moved = rs::remap(s, [](double x, double y) { return std::pair<double, double>{x - 3, y + 2}; }, 80, 80, rs::Interp::Bicubic);
        for (int k = 0; k < 4; ++k) QCOMPARE(moved.at(33, 20)[k], s.at(30, 22)[k]);
    }
};

QTEST_GUILESS_MAIN(TestTransform)
#include "test_transform.moc"
