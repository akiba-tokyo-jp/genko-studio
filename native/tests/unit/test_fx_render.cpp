// PC4 節 D4 (SPEC.md COLOR-08「フィルター」: 描画・ぼかしギャラリー) without Python — PhotoCraft's own tests of them
// (algo/src/tests_ext.rs 407-475, 565-588; engine/src/extra_cmds.rs average_and_clouds; render_cmds.rs; render2.rs) on
// Genko's surfaces and ops: ファイバー between its colours and seeded, 逆光 brightest at its centre (a screen never darkens),
// 照明効果's point light falling off and a spot light's cone, its bumps from the layer and from an alpha channel of the page
// (refused for a smart filter), 雲模様 1 varied and smooth and 雲模様 2 over it with the same seed black — through the op on 8
// bits, precise and ink pages and an opened picture's page; ぼかしギャラリー's geometry (a sharp band, an iris's core, a spin's
// ellipse, a path's direction), the same bytes whatever the tiling and threads, the same look at 200 and 600 dpi, its pins
// moving with a smart object; the lists refused past their limits before anything is had. render_picture: each of 炎・木・額縁
// draws, is seeded and the same twice at every depth and on every page of inks, its shapes for every type and design, the
// flame along the page's path (else upstream's arc), the frame and the tree in the trim (or the selection's box), inside the
// selection, a new layer when asked, refused past its shapes, for a path too long, a locked layer.

#include <QElapsedTimer>
#include <QtTest/QtTest>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/filter_kinds.hpp"
#include "core/ink.hpp"
#include "formats/open_file.hpp"
#include "formats/pillow_save.hpp"
#include "render/fx.hpp"
#include "render/image.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/raster.hpp"
#include "render/render_picture.hpp"
#include "render/smart_filters.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
using render::fx::Rect;

namespace {

constexpr double kInch = 25.4;
using Px = std::array<float, 4>;

double mm(double px) { return px * kInch / 200.0; }  // (a pixel of the working 200 dpi in mm)

std::string precise(std::uint32_t w, std::uint32_t h, const std::vector<Px>& px, bool f32) {
    return core::encode_color_pixels(w, h, f32 ? "f32" : "u16", [&](std::size_t i) { return std::array<double, 4>{px[i][0], px[i][1], px[i][2], px[i][3]}; });
}

std::vector<Px> flat(std::uint32_t w, std::uint32_t h, Px v) { return std::vector<Px>(std::size_t(w) * h, v); }

// PhotoCraft's test pattern (tests_ext.rs pattern).
std::vector<Px> pattern(std::uint32_t w, std::uint32_t h) {
    std::vector<Px> out;
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x)
            out.push_back({static_cast<float>((x * 7 + y * 3) % 64) / 63.0f, static_cast<float>((x * x + y) % 50) / 49.0f, static_cast<float>(y % 9) / 8.0f, 1.0f});
    return out;
}

// A smooth picture (for the look at two resolutions): gradients across and down.
std::vector<Px> smooth(std::uint32_t w, std::uint32_t h) {
    std::vector<Px> out;
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x)
            out.push_back({static_cast<float>(x) / static_cast<float>(w), static_cast<float>(y) / static_cast<float>(h),
                           0.5f + 0.4f * std::sin(static_cast<float>(x + y) / static_cast<float>(w) * 6.0f), 1.0f});
    return out;
}

const render::fx::Kernel& kernel(const std::string& kind) {
    const render::fx::Kernel* k = render::fx::kernel(kind);
    if (k == nullptr) throw core::Error("value", "no kernel " + kind);
    return *k;
}

std::string run_precise(const std::string& raster, const std::string& kind, const Json& given, render::fx::Tiling tiling = {}, double s = 1,
                        const std::string* cover = nullptr) {
    const auto surface = render::fx::precise_surface(raster);
    const Rect grid{0, 0, int(surface->width()), int(surface->height())};
    render::fx::apply(*surface, kernel(kind), core::normalize_filter(kind, given, nullptr), core::filter_info(kind)->inks, grid, cover, s, nullptr, tiling);
    return render::fx::take_bytes(*surface);
}

std::array<double, 4> pixel(const std::string& raster, std::uint32_t x, std::uint32_t y) {
    const core::ColorRasterView view(raster);
    return view.pixel(std::size_t(y) * view.width() + x);
}

core::ApplyResult apply_ops(const core::Document& doc, const Json& ops) { return core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor("human:test")); }

std::string refusal(const core::Document& doc, const Json& op) {
    try {
        apply_ops(doc, Json::array({op}));
    } catch (const core::ApplyError& error) {
        return error.code() + ": " + error.what();
    }
    return {};
}

// A page of w × h working pixels (its trim a mm in from each side), no layers.
core::Document blank(double w_mm, double h_mm) {
    auto doc = core::new_episode("描画", core::Num(1), 1, core::PageSpec::custom(w_mm, h_mm, w_mm - 2, h_mm - 2, 1, 0.5, 0.5, 0.5, 0.5, 200, "color"));
    auto& p = doc.edit_page(0);
    p.numero = false;
    p.frames.clear();
    p.layers.clear();
    return doc;
}

const core::Layer& layer_of(const core::Document& doc, std::string_view id) {
    for (const auto& layer : doc.page(0).layers)
        if (layer.id == id) return layer;
    throw core::Error("value", "no layer " + std::string(id));
}

// A page of w × h pixels at 200 dpi with an 8-bit paint layer "paint".
core::Document eight_page(int w, int h, const render::Image& picture) {
    auto doc = blank(mm(w), mm(h));
    core::Layer layer;
    layer.id = "paint";
    layer.kind = core::LayerKind::Raster;
    layer.panel_clip = false;
    render::raster::save_raster(doc.page(0), layer, picture);
    doc.edit_page(0).layers.push_back(layer);
    return doc;
}

render::Image eight_of(std::uint32_t w, std::uint32_t h, const std::vector<Px>& px) {
    std::string bytes;
    for (const Px& p : px)
        for (const float v : p) bytes.push_back(static_cast<char>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)));
    return render::Image::frombytes("RGBA", render::Size{static_cast<int>(w), static_cast<int>(h)}, bytes);
}

render::Image picture_of(const core::Document& doc, const std::string& id) { return render::raster::layer_pixels(doc.page(0), layer_of(doc, id)).convert("RGBA"); }

// A precise page of w × h pixels at 200 dpi with one paint layer "paint".
core::Document precise_page(std::uint32_t w, std::uint32_t h, const std::vector<Px>& px, bool f32) {
    auto doc = blank(mm(w), mm(h));
    Json pixels = Json::array();
    for (const auto& p : px)
        for (const float v : p) pixels.push_back(f32 ? Json(static_cast<double>(v)) : Json(static_cast<std::int64_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 65535))));
    doc = core::CommandBus().apply(doc, Json::array({Json{{"op", "put_color_raster"}, {"page", 1}, {"width", w}, {"height", h},
                                                          {"precision", f32 ? "f32" : "u16"}, {"pixels", pixels}}}), core::Actor("human:test")).doc;
    doc.edit_page(0).layers[0].id = "paint";
    return doc;
}

// An RGB page of `depth` bits (8, 16 or 32) of w × h working pixels with an empty paint layer "paint" (past the panels: on
// a deep page it becomes one of the page's precision when drawn on, as a fill's does).
core::Document rgb_page(int depth, int w, int h) {
    auto doc = blank(mm(w), mm(h));
    Json ops = Json::array();
    if (depth != 8) ops.push_back(Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", depth}});
    ops.push_back(Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "paint"}});
    ops.push_back(Json{{"op", "set_layer"}, {"page", 1}, {"id", "paint"}, {"panel_clip", false}});
    return apply_ops(doc, ops).doc;
}

// A page of inks (16 bits, multichannel 8; set_color_mode's `mode`) of w × h working pixels with an empty paint layer "pic".
core::Document ink_page(const std::string& mode, int w, int h) {
    Json set{{"op", "set_color_mode"}, {"mode", mode}, {"depth", mode == "multichannel" ? 8 : 16}};
    if (mode == "cmyk") set["profile"] = "coated-cmyk";
    return apply_ops(blank(mm(w), mm(h)), Json::array({set, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pic"}}})).doc;
}

// How many pixels of a layer show (its alpha above 0.05), on any way.
std::size_t shown(const core::Document& doc, const std::string& id) {
    const core::Layer& layer = layer_of(doc, id);
    std::size_t n = 0;
    if (layer.color_raster) {
        const core::ColorRasterView view(*layer.color_raster);
        for (std::size_t i = 0; i < std::size_t(view.width()) * view.height(); ++i) n += view.pixel(i)[3] > 0.05;
        return n;
    }
    if (layer.ink_raster) {
        const core::InkPixels inks = core::decode_ink_raster(*layer.ink_raster);
        if (!inks.has_alpha()) return std::size_t(inks.width) * inks.height;
        for (std::size_t i = 0; i < std::size_t(inks.width) * inks.height; ++i) n += inks.value(i, inks.colours()) > 0.05;
        return n;
    }
    const std::string bytes = picture_of(doc, id).tobytes();
    for (std::size_t i = 3; i < bytes.size(); i += 4) n += static_cast<unsigned char>(bytes[i]) > 12;
    return n;
}

// The layer's pixels as bytes (to compare two results), on any way.
std::string bytes_of(const core::Document& doc, const std::string& id) {
    const core::Layer& layer = layer_of(doc, id);
    if (layer.color_raster) return *layer.color_raster;
    if (layer.ink_raster) return *layer.ink_raster;
    return picture_of(doc, id).tobytes();
}

// The id of the one layer the op made (the book's layers after, not before).
std::string made_layer(const core::Document& before, const core::Document& after) {
    for (const core::Layer& l : after.page(0).layers) {
        const bool old = std::any_of(before.page(0).layers.begin(), before.page(0).layers.end(), [&](const core::Layer& b) { return b.id == l.id; });
        if (!old) return l.id;
    }
    return {};
}

}  // namespace

class TestFxRender : public QObject {
    Q_OBJECT

private slots:
    // --- 描画 (render.rs, extra_cmds.rs; tests_ext.rs 407-475) ----------------------------------------------------------------

    void fibersLieBetweenColoursAndAreSeeded() {
        const std::string s = precise(40, 30, flat(40, 30, {1, 1, 1, 1}), true);
        const Json p{{"variance", 16}, {"strength", 4}, {"seed", 1}, {"foreground", Json::array({255, 0, 0})}, {"background", Json::array({0, 0, 255})}};
        const std::string a = run_precise(s, "render_fibers", p);
        QCOMPARE(a, run_precise(s, "render_fibers", p));
        for (std::uint32_t y = 0; y < 30; ++y)
            for (std::uint32_t x = 0; x < 40; ++x) {
                const auto px = pixel(a, x, y);
                QVERIFY(std::fabs(px[0] + px[2] - 1.0) < 1e-5 && std::fabs(px[1]) < 1e-6 && px[3] == 1.0);
            }
        Json q = p;
        q["seed"] = 2;
        QVERIFY(a != run_precise(s, "render_fibers", q));
        // (a render fills the base box: on a clear layer too)
        const std::string clear = precise(40, 30, flat(40, 30, {0, 0, 0, 0}), false);
        QCOMPARE(pixel(run_precise(clear, "render_fibers", p), 7, 7)[3], 1.0);
    }

    void lensFlareIsBrightestAtItsCentre() {
        const std::string s = precise(40, 30, flat(40, 30, {0.2f, 0.2f, 0.2f, 1}), true);
        const std::string out = run_precise(s, "render_lens_flare", Json{{"brightness", 100}, {"center_x", 0.25}, {"center_y", 0.5}, {"lens", "prime105"}});
        QVERIFY(pixel(out, 10, 15)[0] > 0.9);
        QVERIFY(pixel(out, 10, 15)[0] > pixel(out, 38, 2)[0]);
        QVERIFY2(pixel(out, 38, 2)[0] >= 0.2 - 1e-6, "a screen never darkens");
        for (const char* lens : {"zoom", "prime35", "movie_prime"})
            QVERIFY(run_precise(s, "render_lens_flare", Json{{"lens", lens}}) != s);
    }

    void lightingPointLightFallsOffAndASpotStaysInItsCone() {
        const std::string s = precise(40, 30, flat(40, 30, {0.8f, 0.8f, 0.8f, 1}), true);
        const Json point{{"kind", "point"}, {"x", 0.1}, {"y", 0.5}, {"z", 0.2}, {"radius", 1}, {"intensity", 50}};
        const std::string out = run_precise(s, "render_lighting_effects", Json{{"lights", Json::array({point})}, {"gloss", -100}, {"ambience", 0}, {"height", 0}});
        QVERIFY2(pixel(out, 4, 15)[0] > pixel(out, 36, 15)[0] + 0.1, qPrintable(QString::number(pixel(out, 4, 15)[0])));
        const Json spot{{"kind", "spot"}, {"x", 0.5}, {"y", 0.5}, {"z", 0.3}, {"target_x", 0.5}, {"target_y", 0.5}, {"cone", 20}};
        const std::string lit = run_precise(s, "render_lighting_effects", Json{{"lights", Json::array({spot})}, {"ambience", 0}, {"height", 0}});
        QVERIFY(pixel(lit, 20, 15)[0] > 0.3 && pixel(lit, 1, 1)[0] < 1e-4);
        // an infinite light lights it evenly; none: the ambience alone
        const std::string far = run_precise(s, "render_lighting_effects", Json{{"lights", Json::array({Json{{"kind", "infinite"}, {"elevation", 90}}})}, {"ambience", 0}});
        QVERIFY(std::fabs(pixel(far, 3, 3)[0] - pixel(far, 35, 25)[0]) < 1e-5 && pixel(far, 3, 3)[0] > 0.5);
        const std::string none = run_precise(s, "render_lighting_effects", Json{{"lights", Json::array()}, {"ambience", 100}});
        QVERIFY(std::fabs(pixel(none, 20, 15)[0] - 0.8 * 0.5) < 1e-4);
        // the bumps of the layer's luminance tilt the surface where it changes, nowhere else
        std::vector<Px> step = flat(40, 30, {0.3f, 0.3f, 0.3f, 1});
        for (std::uint32_t y = 0; y < 30; ++y)
            for (std::uint32_t x = 20; x < 40; ++x) step[y * 40 + x] = {0.9f, 0.9f, 0.9f, 1};
        const std::string stepped = precise(40, 30, step, true);
        const Json lights = Json::array({Json{{"kind", "infinite"}, {"angle", 0}, {"elevation", 30}}});
        const std::string flat_lit = run_precise(stepped, "render_lighting_effects", Json{{"lights", lights}, {"texture", "none"}});
        const std::string bumped = run_precise(stepped, "render_lighting_effects", Json{{"lights", lights}, {"texture", "luminance"}, {"height", 100}});
        QVERIFY(std::fabs(pixel(bumped, 20, 15)[0] - pixel(flat_lit, 20, 15)[0]) > 0.01);
        QCOMPARE(pixel(bumped, 5, 15), pixel(flat_lit, 5, 15));
    }

    // 雲模様 1 varies smoothly between the colours; 雲模様 2 with the same seed over it is black (extra_cmds.rs 787-795).
    void cloudsThenDifferenceCloudsAreBlack() {
        const std::string s = precise(40, 30, flat(40, 30, {0.4f, 0.4f, 0.4f, 1}), false);
        const std::string c = run_precise(s, "render_clouds", Json{{"seed", 3}});
        double lo = 1, hi = 0, step = 0;
        for (std::uint32_t x = 0; x < 40; ++x) {
            const double v = pixel(c, x, 12)[0];
            lo = std::min(lo, v);
            hi = std::max(hi, v);
            if (x > 0) step = std::max(step, std::fabs(v - pixel(c, x - 1, 12)[0]));
        }
        QVERIFY2(hi - lo > 0.05, "the clouds vary");
        QVERIFY2(step < 0.2, "the clouds are smooth");
        const std::string black = run_precise(c, "render_difference_clouds", Json{{"seed", 3}});
        for (std::uint32_t y = 0; y < 30; y += 3)
            for (std::uint32_t x = 0; x < 40; x += 3)
                for (unsigned k = 0; k < 3; ++k) QVERIFY2(pixel(black, x, y)[k] < 0.01, qPrintable(QString::number(pixel(black, x, y)[k])));
        // their colours: between the foreground and background
        const std::string coloured = run_precise(s, "render_clouds", Json{{"seed", 3}, {"foreground", Json::array({255, 0, 0})}, {"background", Json::array({0, 0, 255})}});
        QVERIFY(std::fabs(pixel(coloured, 9, 9)[0] + pixel(coloured, 9, 9)[2] - 1.0) < 1e-4);
        // through the op on every way: an 8-bit layer, precise u16 and f32, a CMYK page — 雲模様 2 over 雲模様 1 black
        const std::vector<core::Document> pages{eight_page(40, 30, eight_of(40, 30, pattern(40, 30))), precise_page(40, 30, pattern(40, 30), false),
                                                precise_page(40, 30, pattern(40, 30), true)};
        for (const core::Document& page : pages) {
            const auto done = apply_ops(page, Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "render_clouds"}, {"seed", 3}},
                                                           Json{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "render_difference_clouds"}, {"seed", 3}}}))
                                  .doc;
            const core::Layer& layer = layer_of(done, "paint");
            if (layer.color_raster) {
                QVERIFY(pixel(*layer.color_raster, 13, 11)[0] < 0.01 && pixel(*layer.color_raster, 13, 11)[3] > 0.99);
            } else {
                const auto px = picture_of(done, "paint").getpixel(13, 11);
                QVERIFY2(px[0] <= 2 && px[1] <= 2 && px[2] <= 2 && px[3] == 255, qPrintable(QString::number(px[0])));
            }
        }
        const auto cmyk = apply_ops(ink_page("cmyk", 40, 30),
                                    Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", "pic"}, {"kind", "render_clouds"}, {"seed", 3}}}))
                              .doc;
        QVERIFY(shown(cmyk, "pic") == 40 * 30);
        // (the clouds as large on the paper at every resolution: a 600 dpi layer's shrunk by 3 near the 200 dpi one's)
        const std::string big = run_precise(precise(120, 90, flat(120, 90, {0, 0, 0, 1}), true), "render_clouds", Json{{"seed", 5}}, {}, 3.0);
        const std::string small = run_precise(precise(40, 30, flat(40, 30, {0, 0, 0, 1}), true), "render_clouds", Json{{"seed", 5}}, {}, 1.0);
        double total = 0;
        for (std::uint32_t y = 0; y < 30; ++y)
            for (std::uint32_t x = 0; x < 40; ++x) {
                double mean = 0;
                for (std::uint32_t dy = 0; dy < 3; ++dy)
                    for (std::uint32_t dx = 0; dx < 3; ++dx) mean += pixel(big, x * 3 + dx, y * 3 + dy)[0] / 9.0;
                total += std::fabs(mean - pixel(small, x, y)[0]);
            }
        QVERIFY2(total / 1200.0 < 0.03, qPrintable(QString::number(total / 1200.0)));
    }

    // 照明効果's bumps from an alpha channel of the page (C++'s own, design 2.6); a smart filter's refused; a channel not on the
    // page refused.
    void lightingReadsAChannelOfThePage() {
        auto doc = precise_page(40, 30, flat(40, 30, {0.6f, 0.6f, 0.6f, 1}), true);
        doc = apply_ops(doc, Json::array({Json{{"op", "add_channel"}, {"page", 1}, {"fill", "area"}, {"area", Json{{"rect", Json::array({0, 0, mm(20), mm(30)})}}}}})).doc;
        const std::string name = doc.page(0).channels.at(0).name;
        const Json lights = Json::array({Json{{"kind", "infinite"}, {"angle", 0}, {"elevation", 30}}});
        const Json op{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "render_lighting_effects"}, {"lights", lights}, {"height", 100}};
        Json bumped = op;
        bumped["texture"] = Json{{"channel", name}};
        const auto flat_done = apply_ops(doc, Json::array({op})).doc;
        const auto bumped_done = apply_ops(doc, Json::array({bumped})).doc;
        const std::string& a = *layer_of(flat_done, "paint").color_raster;
        const std::string& b = *layer_of(bumped_done, "paint").color_raster;
        QVERIFY2(std::fabs(pixel(a, 20, 15)[0] - pixel(b, 20, 15)[0]) > 0.01, "the channel's edge tilts the surface");
        QCOMPARE(pixel(a, 5, 15), pixel(b, 5, 15));
        Json missing = op;
        missing["texture"] = Json{{"channel", "どこにもない"}};
        QVERIFY2(refusal(doc, missing).find("has no channel") != std::string::npos, refusal(doc, missing).c_str());
        const auto so = apply_ops(doc, Json::array({Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({"paint"})}, {"new_id", "so"}}})).doc;
        const std::string smart = refusal(so, Json{{"op", "smart_filter_add"}, {"page", 1}, {"id", "so"}, {"kind", "render_lighting_effects"}, {"texture", Json{{"channel", name}}}});
        QVERIFY2(smart.find("cannot be a channel") != std::string::npos, smart.c_str());
        QVERIFY(refusal(so, Json{{"op", "smart_filter_add"}, {"page", 1}, {"id", "so"}, {"kind", "render_lighting_effects"}, {"texture", "luminance"}}).empty());
    }

    // --- ぼかしギャラリー (gallery.rs; tests_ext.rs 565-588) ----------------------------------------------------------------------

    void galleryBlursFollowTheirGeometry() {
        const std::string s = precise(40, 30, pattern(40, 30), true);
        const std::string ts = run_precise(s, "blur_tilt_shift", Json{{"blur", 6}, {"center_x", 0.5}, {"center_y", 0.5}, {"angle", 0}, {"focus", 0.1}, {"transition", 0.2}});
        QCOMPARE(pixel(ts, 10, 15), pixel(s, 10, 15));  // (the band's centre sharp)
        QVERIFY(pixel(ts, 10, 1) != pixel(s, 10, 1));    // (far rows blurred)
        const std::string ir = run_precise(s, "blur_iris", Json{{"pins", Json::array({Json{{"blur", 6}, {"radius_x", 0.3}, {"radius_y", 0.3}}})}});
        QCOMPARE(pixel(ir, 20, 15), pixel(s, 20, 15));
        QVERIFY(pixel(ir, 1, 1) != pixel(s, 1, 1));
        const std::string sp = run_precise(s, "blur_spin", Json{{"pins", Json::array({Json{{"radius_x", 0.3}, {"radius_y", 0.3}, {"blur_angle", 40}}})}});
        QCOMPARE(pixel(sp, 1, 1), pixel(s, 1, 1));  // (outside the spin's ellipse)
        QVERIFY(pixel(sp, 24, 15) != pixel(s, 24, 15));
        const std::string fb = run_precise(s, "blur_field", Json{{"pins", Json::array({Json{{"x", 0.1}, {"y", 0.5}, {"blur", 0}}, Json{{"x", 0.9}, {"y", 0.5}, {"blur", 8}}})}});
        QVERIFY(pixel(fb, 36, 15) != pixel(s, 36, 15));
        // a horizontal path (the page's mm): vertical stripes blurred, horizontal ones kept
        std::vector<Px> stripes;
        for (std::uint32_t y = 0; y < 30; ++y)
            for (std::uint32_t x = 0; x < 40; ++x) stripes.push_back(y % 2 == 0 ? Px{1, 1, 1, 1} : Px{0, 0, 0, 1});
        const Json path{{"paths", Json::array({Json{{"points", Json::array({Json::array({0, mm(15)}), Json::array({mm(40), mm(15)})})}, {"speed", 6}}})}};
        const std::string pb = run_precise(precise(40, 30, stripes, true), "blur_path", path);
        QVERIFY(std::fabs(pixel(pb, 20, 10)[0] - 1.0) < 1e-5 && pixel(pb, 20, 11)[0] < 1e-5);
        std::vector<Px> columns;
        for (std::uint32_t y = 0; y < 30; ++y)
            for (std::uint32_t x = 0; x < 40; ++x) columns.push_back(x % 2 == 0 ? Px{1, 1, 1, 1} : Px{0, 0, 0, 1});
        const std::string blurred = run_precise(precise(40, 30, columns, true), "blur_path", path);
        QVERIFY(std::fabs(pixel(blurred, 20, 10)[0] - 0.5) < 0.2);
        // none given: upstream's path across the middle of the base box
        QVERIFY(run_precise(precise(40, 30, columns, true), "blur_path", Json::object()) != precise(40, 30, columns, true));
    }

    // Each kind of D4 the same bytes whatever the tiling and threads (a variable blur's levels exact sums; a spin's whole box),
    // at 1 and at 1.7 working pixels a layer pixel.
    void theSameBytesWhateverTheTiling() {
        const std::string raster = precise(64, 48, pattern(64, 48), true);
        const std::vector<std::pair<std::string, Json>> cases{
            {"blur_tilt_shift", Json{{"blur", 9}, {"angle", 30}}},
            {"blur_iris", Json{{"pins", Json::array({Json{{"blur", 12}, {"radius_x", 0.2}, {"radius_y", 0.3}, {"roundness", 40}}, Json{{"x", 0.8}, {"blur", 5}}})}}},
            {"blur_field", Json{{"pins", Json::array({Json{{"x", 0.2}, {"blur", 2}}, Json{{"x", 0.8}, {"y", 0.2}, {"blur", 14}}})}}},
            {"blur_spin", Json{{"pins", Json::array({Json{{"radius_x", 0.4}, {"radius_y", 0.3}, {"blur_angle", 60}}})}}},
            {"blur_path", Json{{"paths", Json::array({Json{{"points", Json::array({Json::array({mm(5), mm(40)}), Json::array({mm(30), mm(10)}), Json::array({mm(60), mm(30)})})},
                                                           {"speed", 12}, {"taper", 50}}})}}},
            {"render_lighting_effects", Json{{"texture", "luminance"}}},
            {"render_lens_flare", Json{{"center_x", 0.3}}},
            {"render_clouds", Json{{"seed", 4}}},
            {"render_fibers", Json{{"seed", 4}}}};
        for (const auto& [kind, params] : cases) {
            for (const double s : {1.0, 1.7}) {
                const std::string a = run_precise(raster, kind, params, {256, 1}, s);
                QVERIFY2(a == run_precise(raster, kind, params, {7, 3}, s), (kind + ": 7 × 3").c_str());
                QVERIFY2(a == run_precise(raster, kind, params, {13, 2}, s), (kind + ": 13 × 2").c_str());
                QVERIFY2(a != raster, (kind + " did nothing").c_str());
            }
        }
    }

    // A gallery blur looks the same on the paper at 200 and 600 dpi (its blur and distances in working pixels).
    void galleryBlursLookTheSameAtEveryResolution() {
        const std::vector<std::pair<std::string, Json>> cases{
            {"blur_tilt_shift", Json{{"blur", 4}, {"angle", 20}, {"focus", 0.05}}},
            {"blur_iris", Json{{"pins", Json::array({Json{{"blur", 5}, {"radius_x", 0.25}, {"radius_y", 0.2}}})}}},
            {"blur_spin", Json{{"pins", Json::array({Json{{"radius_x", 0.4}, {"radius_y", 0.4}, {"blur_angle", 30}}})}}},
            {"blur_path", Json{{"paths", Json::array({Json{{"points", Json::array({Json::array({mm(4), mm(15)}), Json::array({mm(36), mm(15)})})}, {"speed", 6}}})}}}};
        const std::string big_in = precise(120, 90, smooth(120, 90), true), small_in = precise(40, 30, smooth(40, 30), true);
        for (const auto& [kind, params] : cases) {
            const std::string big = run_precise(big_in, kind, params, {}, 3.0);
            const std::string small = run_precise(small_in, kind, params, {}, 1.0);
            double total = 0;
            for (std::uint32_t y = 0; y < 30; ++y)
                for (std::uint32_t x = 0; x < 40; ++x)
                    for (unsigned c = 0; c < 3; ++c) {
                        double mean = 0;
                        for (std::uint32_t dy = 0; dy < 3; ++dy)
                            for (std::uint32_t dx = 0; dx < 3; ++dx) mean += pixel(big, x * 3 + dx, y * 3 + dy)[c] / 9.0;
                        total += std::fabs(mean - pixel(small, x, y)[c]);
                    }
            QVERIFY2(total / 3600.0 < 0.02, qPrintable(QStringLiteral("%1: %2").arg(QString::fromStdString(kind)).arg(total / 3600.0)));
        }
    }

    // The pins of a smart filter are shares of the smart object's contents: moving it moves them on the page (as a radial
    // blur's centre); a path's points (mm) too.
    void pinsMoveWithTheSmartObject() {
        const auto doc = apply_ops(eight_page(120, 80, eight_of(120, 80, pattern(120, 80))),
                                   Json::array({Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({"paint"})}, {"new_id", "so"}},
                                                Json{{"op", "smart_filter_add"}, {"page", 1}, {"id", "so"}, {"kind", "blur_iris"},
                                                     {"pins", Json::array({Json{{"x", 0.25}, {"y", 0.5}, {"blur", 6}}})}},
                                                Json{{"op", "smart_filter_add"}, {"page", 1}, {"id", "so"}, {"kind", "blur_path"},
                                                     {"paths", Json::array({Json{{"points", Json::array({Json::array({mm(10), mm(40)}), Json::array({mm(100), mm(40)})})}}})}}}))
                             .doc;
        const Json iris = (*layer_of(doc, "so").smart)["filters"][0];
        const Json path = (*layer_of(doc, "so").smart)["filters"][1];
        const auto moved = apply_ops(doc, Json::array({Json{{"op", "smart_transform"}, {"page", 1}, {"id", "so"}, {"matrix", Json::array({1, 0, 0, 1, 6, 3})}}})).doc;
        const Json iris_moved = (*layer_of(moved, "so").smart)["filters"][0];
        const Json path_moved = (*layer_of(moved, "so").smart)["filters"][1];
        QCOMPARE(iris_moved["params"], iris["params"]);  // (kept as the contents' places)
        const double pw = doc.page(0).spec.width_mm.value(), ph = doc.page(0).spec.height_mm.value();
        const Json was = render::smart_filters::params_on_page(doc, doc.page(0), layer_of(doc, "so"), iris);
        const Json now = render::smart_filters::params_on_page(moved, moved.page(0), layer_of(moved, "so"), iris_moved);
        QVERIFY2(std::fabs((now["pins"][0]["x"].get<double>() - was["pins"][0]["x"].get<double>()) * pw - 6) < 1e-6 &&
                     std::fabs((now["pins"][0]["y"].get<double>() - was["pins"][0]["y"].get<double>()) * ph - 3) < 1e-6,
                 qPrintable(QString::fromStdString(was.dump() + " → " + now.dump())));
        const Json path_was = render::smart_filters::params_on_page(doc, doc.page(0), layer_of(doc, "so"), path);
        const Json path_now = render::smart_filters::params_on_page(moved, moved.page(0), layer_of(moved, "so"), path_moved);
        for (std::size_t k = 0; k < 2; ++k) {
            const Json& p0 = path_was["paths"][0]["points"][k];
            const Json& p1 = path_now["paths"][0]["points"][k];
            QVERIFY(std::fabs(p1[0].get<double>() - p0[0].get<double>() - 6) < 1e-6 && std::fabs(p1[1].get<double>() - p0[1].get<double>() - 3) < 1e-6);
        }
        // and the smart object drawn with them
        const render::Image drawn = render::layer_image(moved.page(0), layer_of(moved, "so"), 200, &moved);
        QVERIFY(drawn.width() > 0);
    }

    // The lists refused past their limits before anything is had (lights, pins, paths, a path's points); refused, not cut.
    void listsAreRefusedPastTheirLimits() {
        const auto doc = precise_page(64, 64, pattern(64, 64), false);
        const auto many = [](const Json& one, std::size_t n) {
            Json out = Json::array();
            for (std::size_t i = 0; i < n; ++i) out.push_back(one);
            return out;
        };
        const Json op{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}};
        Json lights = op;
        lights["kind"] = "render_lighting_effects";
        lights["lights"] = many(Json{{"kind", "point"}}, core::kLightingMostLights);
        QVERIFY(refusal(doc, lights).empty());
        lights["lights"] = many(Json{{"kind", "point"}}, core::kLightingMostLights + 1);
        QVERIFY2(refusal(doc, lights).find("lights must be a list of at most 64") != std::string::npos, refusal(doc, lights).c_str());
        lights["lights"] = Json::array({Json{{"kind", "laser"}}});
        QVERIFY(!refusal(doc, lights).empty());
        lights["lights"] = Json::array({Json{{"x", "far"}}});
        QVERIFY(!refusal(doc, lights).empty());
        lights["lights"] = Json::array({Json{{"x", 1e300}}});  // (out of range: cut)
        QVERIFY(refusal(doc, lights).empty());
        for (const char* kind : {"blur_iris", "blur_field", "blur_spin"}) {
            Json pins = op;
            pins["kind"] = kind;
            pins["pins"] = many(Json::object(), core::kBlurGalleryMostPins + 1);
            QVERIFY2(refusal(doc, pins).find("pins must be a list of 1 to 64") != std::string::npos, kind);
            pins["pins"] = Json::array();
            QVERIFY2(!refusal(doc, pins).empty(), kind);
            pins["pins"] = Json::array({Json{{"blur", 1e9}}});  // (cut to 500)
            QVERIFY2(refusal(doc, pins).empty() || std::string(kind) == "blur_spin", kind);
        }
        Json paths = op;
        paths["kind"] = "blur_path";
        paths["paths"] = many(Json{{"points", Json::array({Json::array({0, 0}), Json::array({1, 1})})}}, core::kBlurPathMostPaths + 1);
        QVERIFY(refusal(doc, paths).find("paths must be a list of at most 64") != std::string::npos);
        Json far = Json::array();
        for (std::size_t i = 0; i <= core::kBlurPathMostPoints; ++i) far.push_back(Json::array({static_cast<double>(i % 7), static_cast<double>(i % 5)}));
        paths["paths"] = Json::array({Json{{"points", far}}});
        QVERIFY(refusal(doc, paths).find("paths must be") != std::string::npos);
        paths["paths"] = Json::array({Json{{"points", Json::array({Json::array({0, 0}), Json::array({1e7, 0})})}}});
        QVERIFY(refusal(doc, paths).find("paths must be") != std::string::npos);
    }

    // --- render_picture (render2.rs, render_cmds.rs) -----------------------------------------------------------------------------

    void everyTypeAndDesignHasShapesAndIsSeeded() {
        const render::picture::Canvas canvas{0, 0, 400, 300};
        const auto a = render::picture::tree(core::normalize_picture("tree", Json::object()), canvas);
        QVERIFY(a == render::picture::tree(core::normalize_picture("tree", Json::object()), canvas));
        QVERIFY(a != render::picture::tree(core::normalize_picture("tree", Json{{"seed", 7}}), canvas));
        for (int k = 1; k <= 34; ++k)
            QVERIFY2(!render::picture::tree(core::normalize_picture("tree", Json{{"base_tree_type", k}}), canvas).empty(), qPrintable(QString::number(k)));
        for (int k = 1; k <= 16; ++k)
            QVERIFY2(!render::picture::picture_frame(core::normalize_picture("picture_frame", Json{{"frame", k}}), canvas).empty(), qPrintable(QString::number(k)));
        const auto path = render::picture::default_flame_path(canvas);
        for (const char* type : {"one_flame_along_path", "multiple_flames_along_path", "multiple_flames_path_directions", "multiple_flames_various_length",
                                 "candle_light", "multiple_flames_one_direction"})
            QVERIFY2(!render::picture::flame(core::normalize_picture("flame", Json{{"flame_type", type}}), {path}).empty(), type);
        // types and designs by name too; the normal form numbers them
        QCOMPARE(core::normalize_picture("tree", Json{{"base_tree_type", "Weeping Willow"}})["base_tree_type"], Json(6));
        QCOMPARE(core::normalize_picture("picture_frame", Json{{"frame", "doubleLine"}})["frame"], Json(13));
        QCOMPARE(core::normalize_picture("flame", Json{{"quality", 4}})["quality"], Json("fine"));
        QCOMPARE(core::normalize_picture("flame", Json{{"color", Json::array({0, 0, 255})}})["use_custom_color"], Json(true));
        QCOMPARE(core::normalize_picture("tree", Json{{"leaves_color", Json::array({0, 0, 255})}})["default_leaves"], Json(false));
        QVERIFY_THROWS_EXCEPTION(core::OpError, core::normalize_picture("tree", Json{{"base_tree_type", "baobob"}}));
        QVERIFY_THROWS_EXCEPTION(core::OpError, core::normalize_picture("flame", Json{{"flames", 3}}));
    }

    void eachRendererDrawsIsSeededAndTheSameTwiceAtEveryDepth() {
        for (const int depth : {8, 16, 32}) {
            for (const auto& [kind, given] : std::vector<std::pair<std::string, Json>>{
                     {"flame", Json::object()}, {"tree", Json{{"base_tree_type", 7}}}, {"picture_frame", Json{{"frame", "roses"}}}}) {
                const core::Document page = rgb_page(depth, 160, 120);
                Json op{{"op", "render_picture"}, {"page", 1}, {"id", "paint"}, {"kind", kind}};
                op.update(given);
                const core::ApplyResult done = apply_ops(page, Json::array({op}));
                const Json r = done.results.at(0);
                const std::string where = kind + "@" + std::to_string(depth) + ": " + r.dump();
                QVERIFY2(r["primitives"].get<std::size_t>() > 0, where.c_str());
                QVERIFY2(r["bounds_mm"].is_array(), where.c_str());
                QCOMPARE(r["id"], Json("paint"));
                QCOMPARE(r["new_layer"], Json(false));
                QVERIFY2(shown(done.doc, "paint") > 50, where.c_str());
                QVERIFY2(depth == 8 ? !layer_of(done.doc, "paint").color_raster : layer_of(done.doc, "paint").color_raster != nullptr, where.c_str());
                QCOMPARE(shown(page, "paint"), std::size_t(0));  // (the book before it as it was: one step back)
                QCOMPARE(bytes_of(apply_ops(page, Json::array({op})).doc, "paint"), bytes_of(done.doc, "paint"));
                Json other = op;
                other["seed"] = 5;
                QVERIFY2(bytes_of(apply_ops(page, Json::array({other})).doc, "paint") != bytes_of(done.doc, "paint"), where.c_str());
                if (kind == "tree") QCOMPARE(r["tree_type"], Json("cherry_blossom"));
                if (kind == "picture_frame") QCOMPARE(r["frame"], Json("roses"));
                if (kind == "flame") QCOMPARE(r["used_path"], Json(false));
            }
        }
    }

    // 炎 follows the page's path (render_cmds.rs flame_follows_the_work_path): the work path, a saved one by its name.
    void theFlameFollowsThePagesPath() {
        core::Document page = rgb_page(8, 160, 120);
        const auto r0 = apply_ops(page, Json::array({Json{{"op", "render_picture"}, {"page", 1}, {"id", "paint"}, {"kind", "flame"}}})).results.at(0);
        QCOMPARE(r0["used_path"], Json(false));
        core::vector::Subpath sub;
        sub.closed = false;
        sub.knots = {core::vector::Knot::corner(mm(20), mm(110)), core::vector::Knot::corner(mm(20), mm(20))};
        core::vector::Path work;
        work.subpaths = {sub};
        page.edit_page(0).work_path = work;
        const Json op{{"op", "render_picture"}, {"page", 1}, {"id", "paint"}, {"kind", "flame"}, {"width", 16}, {"length", 60}};
        const Json r = apply_ops(page, Json::array({op})).results.at(0);
        QCOMPARE(r["used_path"], Json(true));
        const Json& b = r["bounds_mm"];
        QVERIFY2(b[0].get<double>() < mm(20) && b[0].get<double>() + b[2].get<double>() < mm(60), r.dump().c_str());
        // a saved path by its name (the work path otherwise); one the page has not refused
        core::vector::Subpath across;
        across.closed = false;
        across.knots = {core::vector::Knot::corner(mm(10), mm(100)), core::vector::Knot::corner(mm(150), mm(100))};
        core::vector::Path saved;
        saved.subpaths = {across};
        page.edit_page(0).paths.push_back(core::vector::NamedPath{"炎の道", saved});
        Json named = op;
        named["path"] = "炎の道";
        const Json rn = apply_ops(page, Json::array({named})).results.at(0);
        QVERIFY2(rn["bounds_mm"][0].get<double>() + rn["bounds_mm"][2].get<double>() > mm(100), rn.dump().c_str());
        named["path"] = "ない道";
        QVERIFY(refusal(page, named).find("no path named") != std::string::npos);
        // multiple flames along it, at intervals
        Json many = op;
        many["flame_type"] = "multiple_flames_along_path";
        many["interval"] = 10;
        QVERIFY(apply_ops(page, Json::array({many})).results.at(0)["primitives"].get<std::size_t>() > r["primitives"].get<std::size_t>());
    }

    // 木 and 額縁 in the trim when nothing is selected (a manga page's bleed: design 3.2), in the selection's box otherwise; drawn
    // inside the selection only; a new layer when asked (render_cmds.rs selection_limits_and_new_layer).
    void theFrameIsTheTrimOrTheSelection() {
        const core::Document page = rgb_page(8, 160, 120);  // (its trim a mm in from each side)
        const Json r = apply_ops(page, Json::array({Json{{"op", "render_picture"}, {"page", 1}, {"id", "paint"}, {"kind", "picture_frame"}, {"margin", 0}}})).results.at(0);
        const Json& b = r["bounds_mm"];
        QVERIFY2(b[0].get<double>() >= 1.0 - 0.15 && b[1].get<double>() >= 1.0 - 0.15, r.dump().c_str());
        QVERIFY2(b[0].get<double>() + b[2].get<double>() <= mm(160) - 1.0 + 0.15, r.dump().c_str());
        QVERIFY2(b[1].get<double>() + b[3].get<double>() <= mm(120) - 1.0 + 0.15, r.dump().c_str());
        // a selection: the left half — its box the frame, nothing drawn past it
        const Json area{{"rect", Json::array({0, 0, mm(80), mm(120)})}};
        const core::ApplyResult done = apply_ops(page, Json::array({Json{{"op", "render_picture"}, {"page", 1}, {"new_layer", true}, {"kind", "tree"},
                                                                         {"base_tree_type", 1}, {"leaves_amount", 100}, {"area", area}}}));
        const Json rt = done.results.at(0);
        QCOMPARE(rt["new_layer"], Json(true));
        const std::string id = made_layer(page, done.doc);
        QCOMPARE(rt["id"], Json(id));
        QCOMPARE(layer_of(done.doc, id).title, std::string("木"));
        QCOMPARE(done.doc.page(0).layers.size(), page.page(0).layers.size() + 1);
        const render::Image laid = picture_of(done.doc, id);
        std::size_t right = 0;
        for (int y = 0; y < laid.height(); ++y)
            for (int x = 81; x < laid.width(); ++x) right += laid.getpixel(x, y)[3] > 0;
        QCOMPARE(right, std::size_t(0));
        QVERIFY(shown(done.doc, id) > 50);
        // an empty selection: nothing drawn
        const Json none = apply_ops(page, Json::array({Json{{"op", "render_picture"}, {"page", 1}, {"id", "paint"}, {"kind", "tree"},
                                                            {"area", Json{{"rect", Json::array({mm(200), mm(200), mm(10), mm(10)})}}}}}))
                              .results.at(0);
        QVERIFY(none["bounds_mm"].is_null());
        // no layer named, or one that is not a paint layer: a new one; a locked one refused
        QCOMPARE(apply_ops(page, Json::array({Json{{"op", "render_picture"}, {"page", 1}, {"kind", "flame"}}})).results.at(0)["new_layer"], Json(true));
        core::Document locked = page;
        locked.edit_page(0).layers.back().locked = true;
        QVERIFY(refusal(locked, Json{{"op", "render_picture"}, {"page", 1}, {"id", "paint"}, {"kind", "flame"}}).find("locked") != std::string::npos);
        QVERIFY(refusal(page, Json{{"op", "render_picture"}, {"page", 1}, {"id", "nope"}, {"kind", "flame"}}).find("no layer") != std::string::npos);
        QVERIFY(refusal(page, Json{{"op", "render_picture"}, {"page", 1}, {"kind", "bonfire"}}).find("kind must be") != std::string::npos);
    }

    // On every page of inks (render_cmds.rs works_in_cmyk_and_gray) and on an opened picture's page (its grid "finest").
    void onEveryPageOfInksAndAnOpenedPicture() {
        for (const std::string mode : {"cmyk", "lab", "gray", "multichannel"}) {
            const core::Document page = ink_page(mode, 96, 96);
            const core::ApplyResult done =
                apply_ops(page, Json::array({Json{{"op", "render_picture"}, {"page", 1}, {"id", "pic"}, {"kind", "picture_frame"}, {"frame", 13}}}));
            QVERIFY2(done.results.at(0)["bounds_mm"].is_array(), mode.c_str());
            QVERIFY2(shown(done.doc, "pic") > 50, mode.c_str());
            QVERIFY(layer_of(done.doc, "pic").ink_raster != nullptr);
        }
        const render::Image picture = eight_of(300, 200, pattern(300, 200)).convert("RGB");
        const std::string png = formats::png_bytes(picture, formats::PngSave{std::array<double, 2>{300, 300}, {}, false});
        const core::Document opened = formats::open_file(png, "picture.png").doc;
        const std::string id = opened.page(0).layers[0].id;
        const auto done = apply_ops(opened, Json::array({Json{{"op", "render_picture"}, {"page", 1}, {"id", id}, {"kind", "tree"}, {"base_tree_type", 3}},
                                                         Json{{"op", "filter_raster"}, {"page", 1}, {"id", id}, {"kind", "blur_iris"}},
                                                         Json{{"op", "filter_raster"}, {"page", 1}, {"id", id}, {"kind", "render_lens_flare"}}}))
                              .doc;
        const render::Image after = render::raster::layer_pixels(done.page(0), layer_of(done, id)).convert("RGBA");
        QCOMPARE(after.width(), 300);  // (the picture's own pixels)
        QVERIFY(after.tobytes() != render::raster::layer_pixels(opened.page(0), layer_of(opened, id)).convert("RGBA").tobytes());
    }

    // Refused before the shapes are had: flames past kPictureMostShapes, a path too long to follow.
    void tooManyShapesAreRefusedBeforeTheyAreMade() {
        core::Document page = rgb_page(8, 800, 300);  // (its arc across the trim 0.7 × 784 px: some 550 flames of 63 lines)
        const Json op{{"op", "render_picture"}, {"page", 1}, {"id", "paint"}, {"kind", "flame"}, {"flame_type", "multiple_flames_along_path"},
                      {"interval", 1}, {"length", 1000}, {"flame_lines", 100}};
        QElapsedTimer clock;
        clock.start();
        QVERIFY2(refusal(page, op).find("more than 2000000 shapes") != std::string::npos, refusal(page, op).c_str());
        QVERIFY2(clock.elapsed() < 5000, qPrintable(QString::number(clock.elapsed())));
        // a path of curls too fine to follow
        core::vector::Subpath curls;
        curls.closed = false;
        for (int i = 0; i < 600; ++i) {
            core::vector::Knot k = core::vector::Knot::corner(i % 2 == 0 ? 0 : 50000, 0);
            k.out = core::vector::XY{k.anchor.x + (i % 2 == 0 ? 90000 : -90000), 90000};
            k.in = core::vector::XY{k.anchor.x + (i % 2 == 0 ? -90000 : 90000), -90000};
            curls.knots.push_back(k);
        }
        core::vector::Path curly;
        curly.subpaths = {curls};
        page.edit_page(0).work_path = curly;
        const Json follow{{"op", "render_picture"}, {"page", 1}, {"id", "paint"}, {"kind", "flame"}};
        QVERIFY2(refusal(page, follow).find("too long or too finely curved") != std::string::npos, refusal(page, follow).c_str());
        QVERIFY2(refusal(rgb_page(8, 40, 40), Json{{"op", "render_picture"}, {"page", 1}, {"kind", "tree"}, {"new_layer", "yes"}}).find("new_layer") != std::string::npos,
                 "new_layer is true or false");
    }
};

QTEST_GUILESS_MAIN(TestFxRender)
#include "test_fx_render.moc"
