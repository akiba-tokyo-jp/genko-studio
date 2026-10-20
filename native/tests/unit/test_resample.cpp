// PC3 C-0 (SPEC.md COLOR-07, ARCHITECTURE.md「再標本化」): pixels drawn again through a map, as PhotoCraft's
// algo/src/transform.rs, warp.rs and resample.rs test them — the homography takes a box onto its quad and back; the
// identity and a whole-pixel move exact; a doubled box twice the size, a quarter turn the box turned; a large reduction
// pre-filtered (no aliasing); an identity mesh warp a no-op at every depth, a moved one moved, an arc's centre kept and
// its ends dropped; Image Size's filters keep the identity, halve and double a ramp, average a checkerboard, keep a clear
// edge from darkening — and what this build adds: a right-angle turn or flip moves the pixels exactly (four quarter turns
// the plane again), a 32-bit plane's colours above 1 kept (HDR), an ink plane's channels, a grey plane's edge carried on.

#include <QtTest>

#include <cmath>
#include <string>
#include <vector>

#include "core/command_bus.hpp"
#include "core/geometry.hpp"
#include "core/model.hpp"
#include "core/page_geometry.hpp"
#include "render/image.hpp"
#include "render/image_ops.hpp"
#include "render/ops_registry.hpp"
#include "render/resample.hpp"

using namespace genko;
using core::Json;
namespace rs = render::resample;
namespace pg = core::page_geometry;

namespace {

rs::Plane rgba(int w, int h) { return rs::Plane(w, h, 4, true); }

void fill_rect(rs::Plane& p, int x0, int y0, int x1, int y1, std::array<float, 4> v) {
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) std::copy(v.begin(), v.end(), p.at(x, y));
}

// The box of the pixels with any alpha.
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

rs::Plane sample(int w, int h, int ox, int oy, int pw, int ph) {
    rs::Plane p = rgba(w, h);
    for (int y = 0; y < ph; ++y) {
        for (int x = 0; x < pw; ++x) {
            const float v = static_cast<float>((x * 7 + y * 13) % 17) / 16.0f;
            float* q = p.at(x + ox, y + oy);
            q[0] = v;
            q[1] = 1.0f - v;
            q[2] = static_cast<float>(x) / pw;
            q[3] = 1.0f;
        }
    }
    return p;
}

rs::Plane ramp(int w = 64, int h = 32) {
    rs::Plane p = rgba(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float* q = p.at(x, y);
            q[0] = x / 63.0f;
            q[1] = 0.5f;
            q[2] = 1.0f - x / 63.0f;
            q[3] = 1.0f;
        }
    return p;
}

}  // namespace

class TestResample : public QObject {
    Q_OBJECT

private slots:
    // transform.rs: homography_maps_rect_corners_and_inverts
    void homography() {
        const std::array<double, 4> r{10, 20, 110, 70};
        const std::array<std::array<double, 2>, 4> q{{{0, 0}, {200, 10}, {180, 150}, {-20, 120}}};
        const auto h = rs::Homography::rect_to_quad(r, q);
        QVERIFY(h.has_value());
        const std::array<std::array<double, 2>, 4> corners{{{10, 20}, {110, 20}, {110, 70}, {10, 70}}};
        for (std::size_t i = 0; i < 4; ++i) {
            const auto [x, y] = h->apply(corners[i][0], corners[i][1]);
            QVERIFY(std::fabs(x - q[i][0]) < 1e-9 && std::fabs(y - q[i][1]) < 1e-9);
        }
        const auto inv = h->inverse();
        const auto [x, y] = inv->apply(90, 60);
        const auto [bx, by] = h->apply(x, y);
        QVERIFY(std::fabs(bx - 90) < 1e-9 && std::fabs(by - 60) < 1e-9);
        QVERIFY(!rs::Homography::rect_to_quad(r, {{{0, 0}, {0, 0}, {0, 0}, {0, 0}}}).has_value() ||
                !rs::Homography::rect_to_quad(r, {{{0, 0}, {0, 0}, {0, 0}, {0, 0}}})->inverse().has_value());
    }

    // transform.rs: identity_and_translation_are_exact, scale_doubles_size_and_rotation_keeps_area,
    // large_downscale_is_prefiltered
    void projective() {
        rs::Plane s = rgba(40, 20);
        fill_rect(s, 4, 4, 20, 12, {1.0f, 128 / 255.0f, 0.0f, 1.0f});
        for (const auto interp : {rs::Interp::Nearest, rs::Interp::Bilinear, rs::Interp::Bicubic}) {
            const rs::Plane o = rs::project(s, rs::Homography{{1, 0, 7, 0, 1, -3, 0, 0, 1}}, 40, 20, interp);
            QCOMPARE(content(o), (std::array<int, 4>{11, 1, 27, 9}));
            QCOMPARE(o.at(15, 5)[1], 128 / 255.0f);
            // (a move of a fraction: still the pixels' values, by its weights)
            const rs::Plane half = rs::project(s, rs::Homography{{1, 0, 0.5, 0, 1, 0, 0, 0, 1}}, 40, 20, interp);
            QVERIFY(half.at(10, 8)[3] > 0.99f);
        }
        // (the source plane is the source rect: its content's bounds, as the upstream's warp_surface takes it)
        rs::Plane box = rgba(40, 20);
        fill_rect(box, 0, 0, 40, 20, {0.2f, 0.4f, 0.6f, 1.0f});
        const auto up = rs::Homography::rect_to_quad({0, 0, 40, 20}, {{{0, 0}, {80, 0}, {80, 40}, {0, 40}}});
        const auto b = content(rs::project(box, *up, 120, 60, rs::Interp::Bicubic));
        QVERIFY2(b[2] - b[0] >= 80 && b[2] - b[0] <= 82 && b[3] - b[1] >= 40 && b[3] - b[1] <= 42, qPrintable(QStringLiteral("%1 %2 %3 %4").arg(b[0]).arg(b[1]).arg(b[2]).arg(b[3])));
        const auto turn = rs::Homography::rect_to_quad({0, 0, 40, 20}, {{{30, -10}, {30, 30}, {10, 30}, {10, -10}}});
        const rs::Plane rot = rs::project(box, *turn, 120, 60, rs::Interp::Bilinear, 0, -20);
        const auto rb = content(rot);
        QVERIFY(std::abs((rb[2] - rb[0]) - 20) <= 2 && std::abs((rb[3] - rb[1]) - 40) <= 2);
        const float* p = rot.at(20, 30);
        QVERIFY(std::fabs(p[0] - 0.2f) < 0.01f && p[3] > 0.99f);
        rs::Plane checker = rgba(64, 64);
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x) {
                const float v = static_cast<float>((x + y) % 2);
                fill_rect(checker, x, y, x + 1, y + 1, {v, v, v, 1.0f});
            }
        const auto down = rs::Homography::rect_to_quad({0, 0, 64, 64}, {{{0, 0}, {8, 0}, {8, 8}, {0, 8}}});
        const rs::Plane small = rs::project(checker, *down, 8, 8, rs::Interp::Bicubic);
        QVERIFY(std::fabs(small.at(4, 4)[0] - 0.5f) < 0.1f);
    }

    // warp.rs: identity_warp_is_a_no_op_at_every_depth, translation_and_known_arc_points, gray masks (their edge here)
    void meshWarp() {
        const rs::Plane src = sample(50, 30, 5, 3, 40, 24);
        const std::array<double, 4> b{5, 3, 45, 27};
        // (identity: the source plane is the source rect, the content's bounds, as the upstream's warp_mesh_surface takes it;
        // a custom mesh holds the points outside its box at its edges)
        const rs::Plane inner = sample(40, 24, 0, 0, 40, 24);
        for (const bool hdr : {false, true}) {
            rs::Plane s = inner;
            const std::array<double, 4> ib{0, 0, 40, 24};
            s.hdr = hdr;
            pg::Warp none;
            none.box = ib;
            pg::Warp custom;
            custom.style = pg::WarpStyle::Custom;
            custom.box = ib;
            custom.mesh = pg::BezierMesh::identity(ib, 1, 1);
            pg::Warp arc0;
            arc0.style = pg::WarpStyle::Arc;
            arc0.box = ib;
            for (const pg::Warp* w : {&none, &custom, &arc0}) {
                for (const auto interp : {rs::Interp::Nearest, rs::Interp::Bilinear, rs::Interp::Bicubic}) {
                    const rs::Plane out = rs::mesh(s, [&](double x, double y) {
                        const auto p = w->map(x, y);
                        return std::pair{p.x, p.y};
                    }, 40, 24, interp);
                    float worst = 0;
                    for (std::size_t i = 0; i < out.v.size(); ++i) worst = std::max(worst, std::fabs(out.v[i] - s.v[i]));
                    QVERIFY2(worst <= 1.0f / 255.0f, qPrintable(QStringLiteral("%1 hdr %2 style %3 interp %4").arg(worst).arg(hdr).arg(int(w->style)).arg(int(interp))));
                }
            }
        }
        const rs::Plane moved = rs::mesh(src, [](double x, double y) { return std::pair{x + 10.0, y - 2.0}; }, 60, 30, rs::Interp::Bilinear);
        const auto mb = content(moved);
        QCOMPARE(mb, (std::array<int, 4>{15, 1, 55, 25}));
        for (int k = 0; k < 4; ++k) QCOMPARE(moved.at(20, 10)[k], src.at(10, 12)[k]);
        pg::Warp arc;
        arc.style = pg::WarpStyle::Arc;
        arc.bend = 50;
        arc.box = b;
        const rs::Plane bent = rs::mesh(src, [&](double x, double y) {
            const auto p = arc.map(x, y);
            return std::pair{p.x, p.y};
        }, 50, 40, rs::Interp::Bilinear);
        for (int k = 0; k < 4; ++k) QVERIFY(std::fabs(bent.at(25, 10)[k] - src.at(25, 10)[k]) < 0.05f);
        QVERIFY(content(bent)[3] > 27 + 2);
        // (grey: no alpha, its edge carried on outside)
        rs::Plane grey(20, 20, 1, false);
        std::fill(grey.v.begin(), grey.v.end(), 1.0f);
        for (int y = 4; y < 10; ++y)
            for (int x = 4; x < 10; ++x) grey.at(x, y)[0] = 0.0f;
        const rs::Plane g = rs::mesh(grey, [](double x, double y) { return std::pair{x + 5.0, y}; }, 20, 20, rs::Interp::Bilinear);
        QCOMPARE(g.at(12, 6)[0], 0.0f);
        QCOMPARE(g.at(18, 6)[0], 1.0f);
        const rs::Plane edge = rs::translate(grey, 5, 0, 20, 20);
        QCOMPARE(edge.at(0, 6)[0], grey.at(0, 6)[0]);  // (the left edge carried on)
    }

    // resample.rs: identity_scale_is_lossless, half_and_double_sizes, downscale_averages_checkerboard,
    // transparent_edges_do_not_darken, translate
    void imageSize() {
        for (const auto f : {rs::Filter::Nearest, rs::Filter::Bilinear, rs::Filter::Bicubic, rs::Filter::Lanczos}) {
            const rs::Plane s = ramp();
            const rs::Plane o = rs::resize(s, 64, 32, f);
            float d = 0;
            for (std::size_t i = 0; i < s.v.size(); ++i) d = std::max(d, std::fabs(s.v[i] - o.v[i]));
            QVERIFY(d < 1e-4f);
        }
        for (const auto f : {rs::Filter::Nearest, rs::Filter::Bilinear, rs::Filter::Bicubic, rs::Filter::Lanczos, rs::Filter::PreserveDetails}) {
            const rs::Plane s = ramp();
            const rs::Plane half = rs::resize(s, 32, 16, f);
            QVERIFY(std::fabs(half.at(16, 8)[1] - 0.5f) < 0.02f && std::fabs(half.at(16, 8)[3] - 1.0f) < 1e-3f);
            const rs::Plane dbl = rs::resize(s, 128, 64, f);
            for (int x = 9; x < 120; ++x) QVERIFY(dbl.at(x, 30)[0] >= dbl.at(x - 1, 30)[0] - 0.02f);
        }
        rs::Plane checker = rgba(64, 64);
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x) {
                const float v = static_cast<float>((x + y) % 2);
                fill_rect(checker, x, y, x + 1, y + 1, {v, v, v, 1});
            }
        QVERIFY(std::fabs(rs::resize(checker, 16, 16, rs::Filter::Bicubic).at(8, 8)[0] - 0.5f) < 0.05f);
        rs::Plane red = rgba(30, 30);
        red.hdr = true;
        fill_rect(red, 10, 10, 20, 20, {1, 0, 0, 1});
        const rs::Plane big = rs::resize(red, 51, 51, rs::Filter::Lanczos);
        const float* e = big.at(17, 25);
        QVERIFY(e[3] > 0 && std::fabs(e[0] - 1.0f) < 0.05f && std::fabs(e[1]) < 0.05f);
        const rs::Plane r = ramp();
        const rs::Plane t = rs::translate(r, 5, -3, 64, 32);
        for (int k = 0; k < 4; ++k) QCOMPARE(t.at(15, 7)[k], r.at(10, 10)[k]);
    }

    // What this build adds: turns and flips exact (four quarter turns the plane again, a flip twice too); HDR kept in a
    // 32-bit plane, cut in another; an ink plane's channels; the planes of a picture and back.
    void turnsHdrAndKinds() {
        const rs::Plane s = sample(7, 5, 0, 0, 7, 5);
        rs::Plane t = s;
        for (int i = 0; i < 4; ++i) t = rs::turn(t, rs::Turn::Cw90);
        QCOMPARE(t.v, s.v);
        QCOMPARE(rs::turn(rs::turn(s, rs::Turn::FlipH), rs::Turn::FlipH).v, s.v);
        const rs::Plane cw = rs::turn(s, rs::Turn::Cw90);
        QCOMPARE(cw.width, 5);
        QCOMPARE(cw.height, 7);
        for (int k = 0; k < 4; ++k) QCOMPARE(cw.at(5 - 1 - 0, 0)[k], s.at(0, 0)[k]);  // (the top left goes to the top right)
        QCOMPARE(rs::turn(rs::turn(s, rs::Turn::Cw90), rs::Turn::Ccw90).v, s.v);
        QCOMPARE(rs::turn(rs::turn(s, rs::Turn::Cw90), rs::Turn::Cw90).v, rs::turn(s, rs::Turn::Rotate180).v);
        // (a turn's affine and the pixels' map agree: the centre of pixel (x, y) goes to the centre of the turned one)
        const auto m = rs::turn_affine(rs::Turn::Cw90, 7, 5);
        const double cx = m[0] * 0.5 + m[2] * 0.5 + m[4], cy = m[1] * 0.5 + m[3] * 0.5 + m[5];
        QCOMPARE(cx, 4.5);
        QCOMPARE(cy, 0.5);
        // HDR
        rs::Plane hdr = rgba(10, 10);
        hdr.hdr = true;
        fill_rect(hdr, 0, 0, 10, 10, {3.5f, 0.25f, 0.0f, 1.0f});
        const rs::Plane kept = rs::project(hdr, rs::Homography{{1, 0, 0.5, 0, 1, 0, 0, 0, 1}}, 10, 10, rs::Interp::Bicubic);
        QVERIFY(std::fabs(kept.at(5, 5)[0] - 3.5f) < 1e-4f);
        rs::Plane cut = hdr;
        cut.hdr = false;
        QCOMPARE(rs::project(cut, rs::Homography{{1, 0, 0.5, 0, 1, 0, 0, 0, 1}}, 10, 10, rs::Interp::Bicubic).at(5, 5)[0], 1.0f);
        // an ink plane (CMYK and alpha): each channel by its weights, premultiplied
        rs::Plane inks(8, 8, 5, true);
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) {
                float* q = inks.at(x, y);
                q[0] = 0.1f;
                q[1] = 0.2f;
                q[2] = 0.3f;
                q[3] = 0.4f;
                q[4] = x < 4 ? 1.0f : 0.0f;
            }
        const rs::Plane moved = rs::project(inks, rs::Homography{{1, 0, 0.5, 0, 1, 0, 0, 0, 1}}, 8, 8, rs::Interp::Bilinear);
        const float* edge = moved.at(4, 3);
        QVERIFY(std::fabs(edge[4] - 0.5f) < 1e-5f && std::fabs(edge[3] - 0.4f) < 1e-5f);  // (no dark fringe: the inks kept)
        // an 8-bit picture's plane and back; a grey picture's
        render::Image picture = render::Image::create("RGBA", render::Size{3, 2}, render::Ink{10, 20, 30, 40});
        QCOMPARE(rs::to_rgba8(rs::from_rgba8(picture)).tobytes(), picture.tobytes());
        render::Image grey = render::Image::create("L", render::Size{3, 2}, render::Ink(77));
        QCOMPARE(rs::to_gray(rs::from_gray(grey)).tobytes(), grey.tobytes());
        render::image_ops::Pixels px(render::image_ops::Pixels::Kind::Precise, 2, 2, 3);
        px.floating = true;
        px.at(0)[0] = 2.5f;
        px.at(0)[3] = 1.0f;
        const rs::Plane plane = rs::from_pixels(px);
        QVERIFY(plane.hdr);
        const render::image_ops::Pixels again = rs::to_pixels(plane, px);
        QCOMPARE(again.at(0)[0], 2.5f);
        QVERIFY(again.floating);
    }

    // PC3-0: the new ops are registered (a book or a script naming one is told it is not in this build yet, not that it
    // is unknown) and refused with not_yet_ported naming the op and its group until the group writes it; the book stays.
    void theNewOpsAreFramesForNow() {
        const core::Document doc = core::new_episode("枠", core::Num(1), 1, core::PageSpec::custom(60, 60, 50, 50, 1, 2, 2, 2, 2, 72, "color"));
        const std::vector<std::pair<const char*, const char*>> frames{
            // (B1's quick_mask and magic_erase, B2's outline_area, apply_layer_mask and mask_from_transparency, B3's matting,
            // B4's clone_stamp … content_aware_fill, C-a's set_path … set_vector_mask, C-b's add_shape_part … move_shape_parts
            // and C-c's transform_layers and content_aware_scale: written)
            {"align_layers", "C-d"}, {"distribute_layers", "C-d"}, {"arrange_layers", "C-d"}, {"link_layers", "C-d"}};
        // (C-e's resize_pages, set_canvas_size, crop_page, trim_page, reveal_all and rotate_pages are written: test_page_size)
        for (const auto& [op, group] : frames) {
            try {
                core::CommandBus(render::ops_registry()).apply(doc, Json::array({Json{{"op", op}, {"page", 1}}}), core::Actor("human:test"));
                QFAIL(op);
            } catch (const core::ApplyError& error) {
                const std::string said = error.what();
                QVERIFY2(error.code() == "not_yet_ported", (std::string(op) + ": " + error.code() + " " + said).c_str());
                QVERIFY2(said.find(op) != std::string::npos && said.find(std::string("(PC3 ") + group + ")") != std::string::npos, said.c_str());
            }
        }
    }
};

QTEST_GUILESS_MAIN(TestResample)
#include "test_resample.moc"
