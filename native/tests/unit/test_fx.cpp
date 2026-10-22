// PC4 節 D (SPEC.md COLOR-08「フィルター」, design 4.1-4.3): the engine of フィルター's kinds (render/fx) without Python —
// PhotoCraft's own tests of its filter core (algo/src/tests.rs: every filter tile independent 79, an empty selection
// changes nothing 91, half a selection mixes halfway 104, a tiny Gaussian the identity, flat colour and mass kept 120,
// 16-bit precision 135, HDR kept 153, the Gaussian's sigma its radius 160, the canvas edge repeated 432) on Genko's
// surfaces; then the same bytes whatever the tiling and the threads, the three ways (8 bits, precise u16 and f32, CMYK,
// Lab, grey and multichannel pages through filter_raster), the ways a page of inks is worked (each plate, its lightness,
// the colours it means), the same look at 200 and 600 dpi, the imaging budget refusing a window it cannot hold, a run
// stopped and its progress, a heavy filter told, a preview's copy at the screen's resolution, and PC4's new ops' frames.
// D1 (ぼかし・シャープ・ノイズ・その他・ビデオ): PhotoCraft's tests of them (tests.rs 182-335, 379; tests_ext.rs 476-563,
// 589-637), the fast algorithms (render/fx_rank) against the ones as written (the same values; floats, a shape at the
// surface blur's threshold's very edge and pages of inks too), 平均 the mean colour inside the selection, the presets at
// PhotoCraft's settings, a page of inks worked by the lightness, a Lab page's ハイパス・カスタム about their middle and its
// grey grains grey, the sharpening's exact Gaussian, the work a kind is on floats, and the same look at 200 and 600 dpi.
// The review of D1: a window past the budget worked in smaller tiles, a smart filter past it even then said.

#include <QtTest/QtTest>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "core/adjust.hpp"
#include "core/base64.hpp"
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/filter_kinds.hpp"
#include "core/ink.hpp"
#include "core/smart.hpp"
#include "render/fx.hpp"
#include "render/fx_rank.hpp"
#include "render/image.hpp"
#include "render/ink.hpp"
#include "render/not_yet_ported.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/raster.hpp"
#include "render/segment/segment.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
using render::fx::Rect;

namespace {

constexpr double kInch = 25.4;  // 200 px at the working 200 dpi

// A precise raster (GKCR) of w × h from straight RGBA floats.
std::string precise(std::uint32_t w, std::uint32_t h, const std::vector<std::array<float, 4>>& px, bool f32) {
    return core::encode_color_pixels(w, h, f32 ? "f32" : "u16", [&](std::size_t i) {
        return std::array<double, 4>{px[i][0], px[i][1], px[i][2], px[i][3]};
    });
}

// PhotoCraft's test picture over w × h (opaque).
std::vector<std::array<float, 4>> pattern(std::uint32_t w, std::uint32_t h) {
    std::vector<std::array<float, 4>> out;
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x)
            out.push_back({static_cast<float>((x * 7 + y * 3) % 64) / 63.0f, static_cast<float>((x * x + y) % 50) / 49.0f,
                           static_cast<float>(y % 9) / 8.0f, 1.0f});
    return out;
}

std::vector<std::array<float, 4>> flat(std::uint32_t w, std::uint32_t h, std::array<float, 4> px) {
    return std::vector<std::array<float, 4>>(std::size_t(w) * h, px);
}

const render::fx::Kernel& kernel(const char* kind) {
    const render::fx::Kernel* k = render::fx::kernel(kind);
    if (k == nullptr) throw core::Error("value", std::string("no kernel ") + kind);
    return *k;
}

// A kind run on a precise raster (its new bytes).
std::string run_precise(const std::string& raster, const char* kind, const Json& params, const std::string* cover = nullptr,
                        render::fx::Tiling tiling = {}, double s = 1, const Rect* bounds = nullptr) {
    const auto surface = render::fx::precise_surface(raster);
    const Rect grid{0, 0, int(surface->width()), int(surface->height())};
    render::fx::apply(*surface, kernel(kind), params, core::FilterInks::Plates, bounds != nullptr ? *bounds : grid, cover, s, nullptr, tiling);
    return render::fx::take_bytes(*surface);
}

std::array<double, 4> pixel(const std::string& raster, std::uint32_t x, std::uint32_t y) {
    const core::ColorRasterView view(raster);
    return view.pixel(std::size_t(y) * view.width() + x);
}

core::ApplyResult run(const core::Document& doc, const Json& op) {
    return core::CommandBus(render::ops_registry()).apply(doc, Json::array({op}), core::Actor("human:test"));
}

std::string refusal(const core::Document& doc, const Json& op) {
    try {
        run(doc, op);
    } catch (const core::ApplyError& error) {
        return error.code() + ": " + error.what();
    }
    return {};
}

core::Document blank(double side_mm = kInch) {
    auto doc = core::new_episode("フィルター", core::Num(1), 1, core::PageSpec::custom(side_mm, side_mm, 20, 20, 1, 2, 2, 2, 2, 200, "color"));
    auto& p = doc.edit_page(0);
    p.numero = false;
    p.frames.clear();
    p.layers.clear();
    return doc;
}

// A page with one precise paint layer "paint" (u16 or f32) holding `px` on a grid of w × h.
core::Document precise_page(std::uint32_t w, std::uint32_t h, const std::vector<std::array<float, 4>>& px, bool f32, double side_mm = kInch) {
    auto doc = blank(side_mm);
    Json pixels = Json::array();
    for (const auto& p : px)
        for (const float v : p) pixels.push_back(f32 ? Json(static_cast<double>(v)) : Json(static_cast<std::int64_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 65535))));
    doc = core::CommandBus().apply(doc, Json::array({Json{{"op", "put_color_raster"}, {"page", 1}, {"width", w}, {"height", h},
                                                          {"precision", f32 ? "f32" : "u16"}, {"pixels", pixels}}}), core::Actor("human:test")).doc;
    doc.edit_page(0).layers[0].id = "paint";
    return doc;
}

const core::Layer& layer_of(const core::Document& doc, const char* id) {
    for (const auto& layer : doc.page(0).layers)
        if (layer.id == id) return layer;
    throw core::Error("value", std::string("no layer ") + id);
}

// A page of inks (`mode` cmyk, lab, gray or multichannel, 16 bits — multichannel 8 — or `depth`) with a paint layer "pic" of
// side × side pixels over the page (1 inch): `ink(x, y, c)` its values.
core::Document ink_page(const std::string& mode, const std::function<double(std::uint32_t, std::uint32_t, unsigned)>& ink, std::uint32_t side = 64,
                        int depth = 0) {
    Json set{{"op", "set_color_mode"}, {"mode", mode}, {"depth", depth != 0 ? depth : mode == "multichannel" ? 8 : 16}};
    if (mode == "cmyk") set["profile"] = "coated-cmyk";
    auto doc = core::CommandBus(render::ops_registry())
                   .apply(blank(), Json::array({set, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pic"}}}),
                          core::Actor("human:test"))
                   .doc;
    const core::PageColor& color = *doc.page(0).color;
    core::InkPixels p = core::blank_ink_pixels(color.space, color.depth, side, side, color.space == core::InkSpace::Multi ? unsigned(color.plates.size()) : 0);
    for (std::uint32_t y = 0; y < side; ++y)
        for (std::uint32_t x = 0; x < side; ++x) {
            for (unsigned c = 0; c < p.colours(); ++c) p.set(std::size_t(y) * side + x, c, ink(x, y, c));
            p.set(std::size_t(y) * side + x, p.colours(), 1.0);
        }
    doc.edit_page(0).layers.back().ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(p));
    return doc;
}

// Each kind's settings for the tests of every kind (a kind whose defaults change nothing or take long on the test
// picture: its settings here — each group adds its own; others: their defaults).
Json kind_params(const std::string& kind) {
    static const Json shifted = [] {  // (カスタム: the right neighbour twice, halved)
        Json out = Json::array();
        for (int i = 0; i < 25; ++i) out.push_back(i == 13 ? 2 : 0);
        return out;
    }();
    static const std::map<std::string, Json> kTestParams{
        {"blur_gaussian", Json{{"radius", 2.5}}},
        {"blur_box", Json{{"radius", 2}}},
        // D1 (PhotoCraft's all_filters' and new_filters' settings: tests.rs 37-60, tests_ext.rs)
        {"blur_motion", Json{{"angle", 30}, {"distance", 7}}},
        {"blur_radial", Json{{"amount", 20}, {"method", "spin"}, {"center_x", 0.5}, {"center_y", 0.5}}},
        {"blur_surface", Json{{"radius", 3}, {"threshold", 20}}},
        {"blur_smart", Json{{"radius", 3}, {"threshold", 60}}},
        {"blur_shape", Json{{"radius", 6}, {"shape", "ring"}}},
        {"blur_lens", Json{{"radius", 6}, {"brightness", 100}, {"threshold", 200}}},
        {"sharpen_unsharp_mask", Json{{"amount", 150}, {"radius", 1.5}, {"threshold", 2}}},
        {"sharpen_smart", Json{{"amount", 100}, {"radius", 1}, {"reduce_noise", 20}}},
        {"noise_add", Json{{"amount", 25}, {"distribution", "gaussian"}, {"seed", 3}}},
        {"noise_median", Json{{"radius", 2}}},
        {"noise_dust_and_scratches", Json{{"radius", 2}, {"threshold", 10}}},
        {"other_custom", Json{{"kernel", shifted}, {"scale", 2}, {"offset", 0}}},
        {"other_high_pass", Json{{"radius", 3}}},
        {"other_minimum", Json{{"radius", 1}}},
        {"other_maximum", Json{{"radius", 3}, {"preserve", "roundness"}}},
        {"other_offset", Json{{"horizontal", 7}, {"vertical", -3}, {"undefined_areas", "wrap"}}},
        // D3
        {"filter_gallery", Json{{"effects", Json::array({Json{{"filter", "cutout"}}})}}},
    };
    return kTestParams.contains(kind) ? kTestParams.at(kind) : core::normalize_filter(kind, Json::object(), nullptr);
}

// A test kernel: the largest value of each channel in the 3 × 3 square (to see how a page of inks is worked).
render::fx::Kernel maximum3() {
    render::fx::Kernel k;
    k.kind = "test_maximum";
    k.halo = [](const Json&, const render::fx::Ctx&) { return render::fx::Halo::margin(1); };
    k.run = [](const Json&, const render::fx::Window& src, const Rect& out, const render::fx::Ctx& ctx) {
        std::vector<float> res;
        for (int y = out.y0; y < out.y1; ++y)
            for (int x = out.x0; x < out.x1; ++x)
                for (std::size_t c = 0; c < src.ch; ++c) {
                    float most = -1e30f;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) most = std::max(most, src.at(x + dx, y + dy, c));
                    res.push_back(c + 1 == src.ch && ctx.alpha ? src.get(x, y, c) : most);
                }
        return res;
    };
    return k;
}

// A test kernel: each pixel as it is (a colour left as it was keeps its inks).
render::fx::Kernel identity() {
    render::fx::Kernel k;
    k.kind = "test_identity";
    k.halo = [](const Json&, const render::fx::Ctx&) { return render::fx::Halo::margin(0); };
    k.run = [](const Json&, const render::fx::Window& src, const Rect& out, const render::fx::Ctx&) { return src.crop(out); };
    return k;
}

}  // namespace

class TestFx : public QObject {
    Q_OBJECT

private slots:
    // --- every kind of the table (D1…D6's kinds come in here as their groups add them) ---------------------------------------

    // Each kind with its settings for the test (its defaults where kTestParams has none): the same bytes whatever the tiling
    // and threads, an empty selection changes nothing, it changes PhotoCraft's test picture, and filter_raster takes it on
    // every way (8 bits, precise u16 and f32, CMYK, Lab, grey, multichannel) and as a smart filter.
    void everyKindOfTheTable() {
        QVERIFY(!core::filter_infos().empty());
        for (const core::FilterInfo& info : core::filter_infos()) {
            const std::string kind(info.kind);
            QVERIFY2(render::fx::kernel(kind) != nullptr, kind.c_str());
            const Json params = kind_params(kind);
            const std::string raster = precise(40, 30, pattern(40, 30), true);
            const std::string a = run_precise(raster, kind.c_str(), params, nullptr, {256, 1});
            QVERIFY2(a == run_precise(raster, kind.c_str(), params, nullptr, {7, 3}), (kind + ": differs between tilings").c_str());
            QVERIFY2(a != raster, (kind + " did nothing").c_str());
            const std::string none(40 * 30, '\0');
            QVERIFY2(run_precise(raster, kind.c_str(), params, &none) == raster, (kind + " ignored an empty selection").c_str());
            // every way through the op
            Json op{{"op", "filter_raster"}, {"page", 1}, {"kind", kind}};
            op.update(params);
            for (const bool f32 : {false, true}) {
                Json on = op;
                on["id"] = "paint";
                QVERIFY2(refusal(precise_page(16, 16, pattern(16, 16), f32), on).empty(), (kind + (f32 ? " f32" : " u16")).c_str());
            }
            for (const std::string mode : {"cmyk", "lab", "gray", "multichannel"}) {
                Json on = op;
                on["id"] = "pic";
                const auto doc = ink_page(mode, [](std::uint32_t x, std::uint32_t y, unsigned c) { return ((x * 7 + y * 3 + c * 11) % 64) / 63.0; });
                QVERIFY2(refusal(doc, on).empty(), (kind + " " + mode + ": " + refusal(doc, on)).c_str());
            }
            {
                auto doc = blank();
                core::Layer layer;
                layer.id = "paint";
                layer.kind = core::LayerKind::Raster;
                layer.panel_clip = false;
                render::raster::save_raster(doc.page(0), layer, render::Image::create("RGBA", render::Size{200, 200}, render::Ink{90, 120, 30, 255}));
                doc.edit_page(0).layers.push_back(layer);
                Json on = op;
                on["id"] = "paint";
                QVERIFY2(refusal(doc, on).empty(), (kind + " 8 bits").c_str());
            }
            QVERIFY2(core::smart::filter_kind(kind), kind.c_str());
        }
    }

    // --- PhotoCraft's tests of the filter core (algo/src/tests.rs) --------------------------------------------------------

    void everyKindIsTileAndThreadIndependent() {
        for (const bool f32 : {false, true}) {
            const std::string raster = precise(40, 30, pattern(40, 30), f32);
            for (const Json& params : {Json{{"radius", 2.5}}, Json{{"radius", 6.0}}, Json{{"radius", 13.0}}}) {
                for (const char* kind : {"blur_gaussian", "blur_box"}) {
                    const std::string a = run_precise(raster, kind, params, nullptr, {256, 1});
                    const std::string b = run_precise(raster, kind, params, nullptr, {7, 1});
                    const std::string c = run_precise(raster, kind, params, nullptr, {7, 4});
                    const std::string d = run_precise(raster, kind, params, nullptr, {13, 3}, 1.7);
                    const std::string e = run_precise(raster, kind, params, nullptr, {256, 1}, 1.7);
                    QVERIFY2(a == b && b == c, qPrintable(QStringLiteral("%1 %2 differs between tilings").arg(kind, QString::fromStdString(params.dump()))));
                    QVERIFY2(d == e, kind);
                    QVERIFY2(a != raster, kind);  // (it did something)
                }
            }
        }
        // an 8-bit picture too
        std::string bytes;
        for (int y = 0; y < 23; ++y)
            for (int x = 0; x < 37; ++x)
                for (const int v : {(x * 9) % 256, (y * 17) % 256, (x * y) % 256, 255}) bytes.push_back(static_cast<char>(v));
        const render::Image picture = render::Image::frombytes("RGBA", render::Size{37, 23}, bytes);
        std::vector<std::string> made;
        for (const render::fx::Tiling tiling : {render::fx::Tiling{256, 1}, render::fx::Tiling{5, 4}, render::fx::Tiling{11, 2}}) {
            const auto surface = render::fx::eight_surface(picture);
            render::fx::apply(*surface, kernel("blur_gaussian"), Json{{"radius", 5}}, core::FilterInks::Plates, Rect{0, 0, 37, 23}, nullptr, 1, nullptr, tiling);
            made.push_back(render::fx::take_image(*surface).tobytes());
        }
        QCOMPARE(made[1], made[0]);
        QCOMPARE(made[2], made[0]);
    }

    void anEmptySelectionChangesNothingAndHalfMixesHalfway() {
        const std::string raster = precise(40, 30, pattern(40, 30), true);
        const std::string none(40 * 30, '\0');
        QCOMPARE(run_precise(raster, "blur_gaussian", Json{{"radius", 3}}, &none), raster);
        const std::string full = run_precise(raster, "blur_gaussian", Json{{"radius", 3}});
        const std::string half(40 * 30, static_cast<char>(128));
        const std::string mixed = run_precise(raster, "blur_gaussian", Json{{"radius", 3}}, &half);
        for (std::uint32_t y = 0; y < 30; y += 3)
            for (std::uint32_t x = 0; x < 40; x += 3) {
                const auto o = pixel(raster, x, y), f = pixel(full, x, y), m = pixel(mixed, x, y);
                for (unsigned c = 0; c < 4; ++c) QVERIFY2(std::fabs(m[c] - (o[c] + (f[c] - o[c]) * 128.0 / 255.0)) < 1e-5, qPrintable(QString::number(m[c])));
            }
    }

    void aTinyGaussianIsTheIdentity() {
        std::string bytes;
        for (int y = 0; y < 10; ++y)
            for (int x = 0; x < 20; ++x)
                for (const int v : {x % 10 == y ? 255 : 0, x % 10 == y ? 128 : 0, x % 10 == y ? 3 : 0, 255}) bytes.push_back(static_cast<char>(v));
        const render::Image picture = render::Image::frombytes("RGBA", render::Size{20, 10}, bytes);
        const auto surface = render::fx::eight_surface(picture);
        render::fx::apply(*surface, kernel("blur_gaussian"), Json{{"radius", 0.01}}, core::FilterInks::Plates, Rect{0, 0, 20, 10}, nullptr, 1);
        QCOMPARE(render::fx::take_image(*surface).tobytes(), picture.tobytes());
    }

    void flatColourAndTheMassAreKept() {
        const std::string raster = precise(40, 30, flat(40, 30, {0.3f, 0.6f, 0.9f, 1.0f}), true);
        const std::string out = run_precise(raster, "blur_gaussian", Json{{"radius", 3}});
        QVERIFY(std::fabs(pixel(out, 20, 15)[0] - 0.3) < 1e-5 && std::fabs(pixel(out, 20, 15)[3] - 1.0) < 1e-5);
        // a red square in the clear: its alpha's sum kept (blurring into the clear), no colour from the clear pixels
        std::vector<std::array<float, 4>> px = flat(40, 40, {0, 0, 0, 0});
        for (std::uint32_t y = 15; y < 25; ++y)
            for (std::uint32_t x = 15; x < 25; ++x) px[std::size_t(y) * 40 + x] = {1.0f, 0.0f, 0.0f, 1.0f};
        const std::string small = precise(40, 40, px, true);
        for (const Json& params : {Json{{"kind", "blur_gaussian"}, {"radius", 2}}, Json{{"kind", "blur_box"}, {"radius", 3}}, Json{{"kind", "blur_gaussian"}, {"radius", 4.5}}}) {
            const std::string kind = params["kind"].get<std::string>();
            const std::string blurred = run_precise(small, kind.c_str(), Json{{"radius", params["radius"]}});
            double sum = 0;
            for (std::uint32_t y = 0; y < 40; ++y)
                for (std::uint32_t x = 0; x < 40; ++x) sum += pixel(blurred, x, y)[3];
            QVERIFY2(std::fabs(sum - 100.0) < 0.01, qPrintable(QStringLiteral("%1: %2").arg(QString::fromStdString(kind)).arg(sum)));
            const auto edge = pixel(blurred, 14, 20);
            QVERIFY2(edge[3] > 0.0 && std::fabs(edge[0] - 1.0) < 1e-4, qPrintable(QString::fromStdString(kind)));
        }
    }

    void sixteenBitPrecisionIsKept() {
        std::vector<std::array<float, 4>> px;
        for (int x = 0; x < 200; ++x) px.push_back({static_cast<float>(x / 65535.0), 0.0f, 0.0f, 1.0f});
        const std::string raster = precise(200, 1, px, false);
        const std::string out = run_precise(raster, "blur_gaussian", Json{{"radius", 2}});
        for (std::uint32_t x = 20; x < 180; ++x) QCOMPARE(static_cast<int>(std::lround(pixel(out, x, 0)[0] * 65535)), static_cast<int>(x));
        // (the box passes' exact sums too)
        const std::string wide = run_precise(raster, "blur_gaussian", Json{{"radius", 6}});
        for (std::uint32_t x = 30; x < 170; ++x) QCOMPARE(static_cast<int>(std::lround(pixel(wide, x, 0)[0] * 65535)), static_cast<int>(x));
    }

    void hdrValuesAreKept() {
        const std::string raster = precise(40, 30, flat(40, 30, {4.0f, 2.0f, 0.5f, 1.0f}), true);
        for (const double radius : {2.0, 7.0}) {
            const std::string out = run_precise(raster, "blur_gaussian", Json{{"radius", radius}});
            QVERIFY(std::fabs(pixel(out, 20, 15)[0] - 4.0) < 1e-4);
        }
        QVERIFY(std::fabs(pixel(run_precise(raster, "blur_box", Json{{"radius", 4}}), 20, 15)[0] - 4.0) < 1e-4);
    }

    void theGaussiansSigmaIsItsRadius() {
        std::vector<std::array<float, 4>> px = flat(40, 30, {0, 0, 0, 0});
        px[15 * 40 + 20] = {1, 1, 1, 1};
        const std::string raster = precise(40, 30, px, true);
        const std::string out = run_precise(raster, "blur_gaussian", Json{{"radius", 2}});
        const double a0 = pixel(out, 20, 15)[3], a2 = pixel(out, 22, 15)[3];
        QVERIFY2(std::fabs(a2 / a0 - std::exp(-0.5)) < 1e-3, qPrintable(QString::number(a2 / a0)));
        // (at the scale of a 600 dpi layer: three times the pixels)
        const std::string scaled = run_precise(raster, "blur_gaussian", Json{{"radius", 1}}, nullptr, {}, 2.0);
        QVERIFY(std::fabs(pixel(scaled, 22, 15)[3] / pixel(scaled, 20, 15)[3] - std::exp(-0.5)) < 1e-3);
    }

    void theGridsEdgeIsRepeated() {
        for (const bool f32 : {false, true}) {
            const std::string raster = precise(40, 30, flat(40, 30, {0.2f, 0.6f, 0.4f, 1.0f}), f32);
            for (const char* kind : {"blur_gaussian", "blur_box"}) {
                const std::string out = run_precise(raster, kind, Json{{"radius", 6}});
                for (const auto& [x, y] : std::vector<std::pair<std::uint32_t, std::uint32_t>>{{0, 0}, {39, 0}, {0, 29}, {39, 29}, {20, 0}}) {
                    const auto p = pixel(out, x, y);
                    QVERIFY2(std::fabs(p[3] - 1.0) < 1e-3 && std::fabs(p[1] - 0.6) < 2e-3, kind);
                }
            }
        }
    }

    // --- the three ways (filter_raster) ------------------------------------------------------------------------------------

    void everyWayThroughFilterRaster() {
        // 8 bits: a paint layer of a picture
        {
            auto doc = blank();
            render::Image picture = render::Image::create("RGBA", render::Size{200, 200}, render::Ink{40, 40, 40, 255});
            picture.paste(render::Ink{220, 30, 30, 255}, render::Box{80, 80, 120, 120});
            core::Layer layer;
            layer.id = "paint";
            layer.kind = core::LayerKind::Raster;
            layer.panel_clip = false;
            render::raster::save_raster(doc.page(0), layer, picture);
            doc.edit_page(0).layers.push_back(layer);
            const auto done = run(doc, Json{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "blur_gaussian"}, {"radius", 4}}).doc;
            const render::Image after = render::raster::layer_pixels(done.page(0), layer_of(done, "paint")).convert("RGBA");
            QVERIFY(after.tobytes() != picture.tobytes());
            QCOMPARE(after.getpixel(10, 10), (std::vector<double>{40, 40, 40, 255}));  // (flat stays flat)
            // inside a selection only: outside it as it was
            const auto inside = run(doc, Json{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "blur_box"}, {"radius", 5},
                                              {"area", Json{{"rect", Json::array({0, 0, kInch / 2, kInch})}}}}).doc;
            const render::Image part = render::raster::layer_pixels(inside.page(0), layer_of(inside, "paint")).convert("RGBA");
            QCOMPARE(part.getpixel(119, 100), picture.getpixel(119, 100));
            QVERIFY(part.getpixel(82, 100) != picture.getpixel(82, 100));
            // (D1: シャープ's settings sharpen the block's edge, flat stays flat)
            const auto sharp = run(doc, Json{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "sharpen_unsharp_mask"}, {"amount", 60},
                                             {"radius", 0.5}, {"threshold", 0}}).doc;
            const render::Image edged = render::raster::layer_pixels(sharp.page(0), layer_of(sharp, "paint")).convert("RGBA");
            QVERIFY(edged.getpixel(80, 100) != picture.getpixel(80, 100));
            QCOMPARE(edged.getpixel(10, 10), picture.getpixel(10, 10));
        }
        // precise u16 and f32 (HDR)
        for (const bool f32 : {false, true}) {
            std::vector<std::array<float, 4>> px = flat(64, 64, {0.25f, 0.5f, f32 ? 3.0f : 0.75f, 1.0f});
            for (std::uint32_t y = 20; y < 40; ++y)
                for (std::uint32_t x = 20; x < 40; ++x) px[std::size_t(y) * 64 + x] = {1, 0, 0, 1};
            const auto doc = precise_page(64, 64, px, f32);
            const auto done = run(doc, Json{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "blur_gaussian"}, {"radius", 3}}).doc;
            const std::string& now = *layer_of(done, "paint").color_raster;
            QVERIFY(now != *layer_of(doc, "paint").color_raster);
            QCOMPARE(core::ColorRasterView(now).metadata("")["precision"], Json(f32 ? "f32" : "u16"));
            QVERIFY(std::fabs(pixel(now, 2, 2)[2] - (f32 ? 3.0 : 0.75)) < 1e-4);  // (HDR kept on f32)
        }
        // pages of inks: CMYK (a grey on K alone stays on K alone: each plate as it is), Lab, grey, multichannel
        for (const std::string mode : {"cmyk", "lab", "gray", "multichannel"}) {
            const auto doc = ink_page(mode, [&](std::uint32_t x, std::uint32_t y, unsigned c) {
                if (mode == "cmyk") return c == 3 ? (x < 32 ? 0.9 : 0.1) : 0.0;
                if (mode == "lab") return c == 0 ? (x < 32 ? 0.2 : 0.9) : 0.5;
                return x < 32 ? 0.8 : 0.2 + 0.01 * (y % 3);
            });
            const auto done = run(doc, Json{{"op", "filter_raster"}, {"page", 1}, {"id", "pic"}, {"kind", "blur_gaussian"}, {"radius", 3}}).doc;
            const core::Layer& after = layer_of(done, "pic");
            QVERIFY2(after.ink_raster && *after.ink_raster != *layer_of(doc, "pic").ink_raster, mode.c_str());
            const core::InkPixels inks = core::decode_ink_raster(*after.ink_raster);
            if (mode == "cmyk") {
                for (std::size_t i = 0; i < inks.pixel_count(); ++i)
                    for (unsigned c = 0; c < 3; ++c) QCOMPARE(inks.value(i, c), 0.0);
                QVERIFY(inks.value(32 * 64 + 31, 3) < 0.9 && inks.value(32 * 64 + 31, 3) > 0.1);  // (blurred across the edge)
                QVERIFY(std::fabs(inks.value(32 * 64 + 5, 3) - 0.9) < 1e-3);                      // (flat stays flat)
            }
        }
    }

    // --- a page of inks: each plate, its lightness, the colours it means --------------------------------------------------

    void theWaysOfInks() {
        // a CMYK raster: K 90% on the left half, K 10% on the right
        const auto doc = ink_page("cmyk", [](std::uint32_t x, std::uint32_t, unsigned c) { return c == 3 ? (x < 32 ? 0.9 : 0.1) : 0.0; });
        const core::Layer& layer = layer_of(doc, "pic");
        const core::InkPixels inks = core::decode_ink_raster(*layer.ink_raster);
        const core::PageColor& color = *doc.page(0).color;
        const render::fx::Kernel most = maximum3();
        const auto k_at = [](const std::string& bytes, std::size_t i) { return core::decode_ink_raster(bytes).value(i, 3); };
        // each plate: the most ink spreads (the dark side grows)
        auto plates = render::fx::ink_surface(inks, color);
        render::fx::apply(*plates, most, Json::object(), core::FilterInks::Plates, Rect{0, 0, 64, 64}, nullptr, 1);
        const std::string by_plates = render::fx::take_bytes(*plates);
        QVERIFY(std::fabs(k_at(by_plates, 10 * 64 + 32) - 0.9) < 1e-3);
        // its lightness: the light spreads as on an RGB page (the light side grows)
        auto light = render::fx::ink_surface(inks, color);
        render::fx::apply(*light, most, Json::object(), core::FilterInks::Lightness, Rect{0, 0, 64, 64}, nullptr, 1);
        const std::string by_light = render::fx::take_bytes(*light);
        QVERIFY(std::fabs(k_at(by_light, 10 * 64 + 31) - 0.1) < 1e-3);
        QVERIFY(std::fabs(k_at(by_light, 10 * 64 + 29) - 0.9) < 1e-3);
        // a Lab page's lightness: its L alone (the light spreads), its a and b as they were
        {
            const auto lab = ink_page("lab", [](std::uint32_t x, std::uint32_t y, unsigned c) {
                if (c == 0) return x < 32 ? 0.2 : 0.9;
                return c == 1 ? 0.3 + 0.01 * (y % 7) : 0.7 - 0.01 * (x % 5);
            });
            const core::InkPixels was = core::decode_ink_raster(*layer_of(lab, "pic").ink_raster);
            auto surface = render::fx::ink_surface(was, *lab.page(0).color);
            render::fx::apply(*surface, most, Json::object(), core::FilterInks::Lightness, Rect{0, 0, 64, 64}, nullptr, 1);
            const core::InkPixels now = core::decode_ink_raster(render::fx::take_bytes(*surface));
            QVERIFY(std::fabs(now.value(10 * 64 + 31, 0) - 0.9) < 1e-3);  // (L: the most of the 3 × 3)
            QVERIFY(std::fabs(now.value(10 * 64 + 29, 0) - 0.2) < 1e-3);
            for (std::size_t i = 0; i < now.pixel_count(); ++i)
                for (unsigned c = 1; c < 3; ++c) QCOMPARE(now.value(i, c), was.value(i, c));
        }
        // the colours it means: a colour left as it is keeps its inks
        auto meaning = render::fx::ink_surface(inks, color);
        render::fx::apply(*meaning, identity(), Json::object(), core::FilterInks::Meaning, Rect{0, 0, 64, 64}, nullptr, 1);
        QCOMPARE(render::fx::take_bytes(*meaning), *layer.ink_raster);
    }

    // --- the look at 200 and 600 dpi --------------------------------------------------------------------------------------

    void theSameLookAtEveryResolution() {
        // the same disc on a precise layer of 200 × 200 (the page at 200 dpi) and of 600 × 600 (600 dpi): blurred by the same
        // radius, the 600 dpi one shrunk to a third is the 200 dpi one
        const auto disc = [](std::uint32_t side) {
            std::vector<std::array<float, 4>> px;
            for (std::uint32_t y = 0; y < side; ++y)
                for (std::uint32_t x = 0; x < side; ++x) {
                    const double u = (x + 0.5) / side - 0.5, v = (y + 0.5) / side - 0.5;
                    px.push_back(u * u + v * v < 0.09 ? std::array<float, 4>{0.9f, 0.2f, 0.1f, 1.0f} : std::array<float, 4>{0.1f, 0.3f, 0.8f, 1.0f});
                }
            return px;
        };
        for (const char* kind : {"blur_gaussian", "blur_box"}) {
            const auto low = run(precise_page(200, 200, disc(200), false), Json{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", kind}, {"radius", 4}}).doc;
            const auto high = run(precise_page(600, 600, disc(600), false), Json{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", kind}, {"radius", 4}}).doc;
            const std::string& a = *layer_of(low, "paint").color_raster;
            const std::string& b = *layer_of(high, "paint").color_raster;
            double worst = 0;
            for (std::uint32_t y = 0; y < 200; y += 2)
                for (std::uint32_t x = 0; x < 200; x += 2) {
                    std::array<double, 4> mean{};
                    for (std::uint32_t dy = 0; dy < 3; ++dy)
                        for (std::uint32_t dx = 0; dx < 3; ++dx) {
                            const auto p = pixel(b, x * 3 + dx, y * 3 + dy);
                            for (unsigned c = 0; c < 4; ++c) mean[c] += p[c] / 9;
                        }
                    const auto q = pixel(a, x, y);
                    for (unsigned c = 0; c < 3; ++c) worst = std::max(worst, std::fabs(mean[c] - q[c]));
                }
            // (the box's side is whole pixels of each grid: 9 working pixels at 200 dpi, 25 / 3 at 600)
            QVERIFY2(worst < (std::string(kind) == "blur_box" ? 0.06 : 0.03), qPrintable(QStringLiteral("%1: %2").arg(kind).arg(worst)));
        }
    }

    // A radius as large as the table takes on a layer far smaller than it reaches (review P2-1: the window is cut to the
    // grid, its edge repeated past it — never more than the layer, whatever the halo): drawn, not refused — an 8-bit page
    // at 200 dpi (ぼかし（ガウス） 1000, ぼかし（ボックス） 2000), a precise layer at 600 dpi (the same), inside the budget the op
    // works in —, the same bytes whatever the tiling, every value between the layer's least and most.
    void aLargeRadiusOnASmallLayerIsDrawn() {
        const render::ImageAllocationBudget budget{2ULL * 1024 * 1024 * 1024};
        const std::vector<Json> ops{Json{{"kind", "blur_gaussian"}, {"radius", 1000}}, Json{{"kind", "blur_box"}, {"radius", 2000}}};
        {  // (8 bits: the page of 1 inch at 200 dpi, s = 1)
            auto doc = blank();
            render::Image picture = render::Image::create("RGBA", render::Size{200, 200}, render::Ink{40, 40, 40, 255});
            picture.paste(render::Ink{220, 200, 30, 255}, render::Box{0, 0, 100, 200});
            core::Layer layer;
            layer.id = "paint";
            layer.kind = core::LayerKind::Raster;
            layer.panel_clip = false;
            render::raster::save_raster(doc.page(0), layer, picture);
            doc.edit_page(0).layers.push_back(layer);
            for (const Json& given : ops) {
                Json op{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}};
                op.update(given);
                const auto done = run(doc, op).doc;
                const render::Image after = render::raster::layer_pixels(done.page(0), layer_of(done, "paint")).convert("RGBA");
                QVERIFY2(after.tobytes() != picture.tobytes(), given.dump().c_str());
                for (int x = 0; x < 200; x += 9) {
                    const auto p = after.getpixel(x, 100);
                    QVERIFY2(p[0] >= 40 && p[0] <= 220 && p[3] == 255, given.dump().c_str());
                }
            }
        }
        {  // (precise at 600 dpi: a layer of 600 × 600 over the 1-inch page, s = 3 — σ 3000 of its pixels, a box of 12001)
            const auto doc = precise_page(600, 600, pattern(600, 600), false);
            for (const Json& given : ops) {
                Json op{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}};
                op.update(given);
                QVERIFY2(refusal(doc, op).empty(), (given.dump() + ": " + refusal(doc, op)).c_str());
            }
        }
        // (the engine at s = 3 on 50 × 50: one tile, and tiles of 7 on 3 threads — the window cut on every side — the same)
        const std::string raster = precise(50, 50, pattern(50, 50), true);
        for (const Json& given : ops) {
            const char* kind = given["kind"].get_ref<const std::string&>().c_str();
            const Json params{{"radius", given["radius"]}};
            const std::string one = run_precise(raster, kind, params, nullptr, {256, 1}, 3.0);
            QVERIFY2(one == run_precise(raster, kind, params, nullptr, {7, 3}, 3.0), kind);
            QVERIFY(one != raster);
            for (std::uint32_t i = 0; i < 50; i += 7) {
                const auto p = pixel(one, i, 49 - i);
                QVERIFY2(p[0] >= -1e-6 && p[0] <= 1 + 1e-6 && std::fabs(p[3] - 1.0) < 1e-6, kind);
            }
        }
    }

    // --- the budget, stopping, progress, heaviness, the preview's copy --------------------------------------------------

    // A window past the imaging budget: at the kind's own tile, smaller tiles (on fewer threads if need be) — the same
    // bytes —; a tile asked for, or a window the whole grid even in the smallest tiles, refused as too large (review P2-2).
    void aWindowPastTheBudgetIsRefused() {
        const std::string raster = precise(512, 512, pattern(512, 512), true);
        const std::string unbounded = run_precise(raster, "blur_gaussian", Json{{"radius", 40}});
        const render::ImageAllocationBudget budget{16ULL * 1024 * 1024};
        const auto refusal_of = [&](const Json& params, render::fx::Tiling tiling) {
            try {
                (void)run_precise(raster, "blur_gaussian", params, nullptr, tiling);
            } catch (const core::OpError& error) {
                return std::string(error.what());
            }
            return std::string();
        };
        QVERIFY(run_precise(raster, "blur_gaussian", Json{{"radius", 40}}) == unbounded);
        QVERIFY(refusal_of(Json{{"radius", 40}}, {512, 0}).find("too large to work on at once") != std::string::npos);
        QVERIFY(refusal_of(Json{{"radius", 200}}, {}).find("too large to work on at once") != std::string::npos);
        // a small one fits
        QVERIFY(run_precise(raster, "blur_gaussian", Json{{"radius", 1}}) != raster);
    }

    // A smart filter on a page drawn at 600 dpi (its smart object's picture the page's grid): one whose window at its own
    // tile is past the budget drawn in smaller tiles; one past it even then — its window the whole grid — not left out
    // silently: a render that does not leave things out stops with smart:filters, one that does lists it (review P2-2).
    void aSmartFilterPastTheBudgetIsSaid() {
        std::string bytes;
        for (int y = 0; y < 120; ++y)
            for (int x = 0; x < 120; ++x)
                for (const int v : {(x * 7) % 256, (y * 5) % 256, (x * y) % 256, 255}) bytes.push_back(static_cast<char>(v));
        const std::string png = render::write_png(render::Image::frombytes("RGBA", render::Size{120, 120}, bytes));
        auto doc = blank(60);
        doc = core::CommandBus(render::ops_registry())
                  .apply(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "p1"}},
                                           Json{{"op", "put_raster"}, {"page", 1}, {"id", "p1"}, {"png_base64", core::b64encode(png)}},
                                           Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({"p1"})}, {"new_id", "so"}}}),
                         core::Actor("human:test"))
                  .doc;
        const auto with = [&](double radius) {
            return core::CommandBus(render::ops_registry())
                .apply(doc, Json::array({Json{{"op", "smart_filter_add"}, {"page", 1}, {"id", "so"}, {"kind", "other_maximum"}, {"radius", radius}}}),
                       core::Actor("human:test"))
                .doc;
        };
        const auto draw = [](const core::Document& d, bool skip) {
            render::RenderOptions options;
            options.mode = "print";
            options.skip_unported = skip;
            return render::render_page(d.page(0), 600, options, &d);
        };
        std::uint64_t plain_peak = 0;
        {
            const render::ImageAllocationBudget measure{8ULL * 1024 * 1024 * 1024};
            (void)draw(doc, false);
            plain_peak = measure.peak();
        }
        // (the page 1417 pixels square at 600 dpi: the whole grid's window about 160 MB, a window of 128-pixel tiles of a
        // reach of 200 about 22 MB, of its own tile 57 MB)
        const auto past = with(500), within = with(66);
        const render::ImageAllocationBudget budget{plain_peak + 32ULL * 1024 * 1024};
        bool said = false;
        try {
            (void)draw(past, false);
        } catch (const render::NotYetPorted& e) {
            said = e.element() == "smart:filters";
        }
        QVERIFY(said);
        const render::RenderResult left = draw(past, true);
        QVERIFY(std::find(left.omitted.begin(), left.omitted.end(), "smart:filters") != left.omitted.end());
        const render::RenderResult drawn = draw(within, false);
        QVERIFY(drawn.omitted.empty());
        QVERIFY(drawn.image.tobytes() != draw(doc, false).image.tobytes());
    }

    void aRunStopsAndSaysHowFarItIs() {
        const std::string raster = precise(300, 300, pattern(300, 300), false);
        std::atomic<double> done{0.5};
        {
            const render::fx::ProgressScope progress(done);
            QCOMPARE(done.load(), 0.0);
            (void)run_precise(raster, "blur_gaussian", Json{{"radius", 2}}, nullptr, {32, 2});
            QCOMPARE(done.load(), 1.0);
        }
        std::stop_source stop;
        stop.request_stop();
        const render::segment::StopScope scope(stop.get_token());
        bool cancelled = false;
        try {
            (void)run_precise(raster, "blur_gaussian", Json{{"radius", 2}});
        } catch (const render::Cancelled&) {
            cancelled = true;
        }
        QVERIFY(cancelled);
    }

    void aHeavyFilterIsTold() {
        // B4 at 600 dpi (6071 × 8598) and a Gaussian of 10: heavy; the page at 200 dpi and ぼかし (0.6): not
        QVERIFY(render::fx::work("blur_gaussian", Json{{"radius", 10}}, 6071ULL * 8598, 3) >= render::fx::kHeavyWork);
        QVERIFY(render::fx::work("blur_gaussian", Json{{"radius", 0.6}}, 2024ULL * 2866, 1) < render::fx::kHeavyWork);
        QVERIFY(render::fx::work("blur_box", Json{{"radius", 1}}, 2024ULL * 2866, 1) < render::fx::kHeavyWork);
        QCOMPARE(render::fx::work("no_such_kind", Json::object(), 100, 1), 0.0);
        const auto doc = precise_page(600, 600, pattern(600, 600), false);
        const auto grid = render::fx::layer_grid(doc.page(0), layer_of(doc, "paint"));
        QCOMPARE(grid.width, 600u);
        QVERIFY(std::fabs(grid.s - 3.0) < 1e-9);
        // the samples' grid: 65535ths, floats, a page of inks' depth, an 8-bit layer's 255ths (review P2-1)
        QCOMPARE(grid.levels, 65535u);
        const auto floats = precise_page(60, 60, pattern(60, 60), true);
        QCOMPARE(render::fx::layer_grid(floats.page(0), layer_of(floats, "paint")).levels, 0u);
        const auto grey8 = ink_page("gray", [](std::uint32_t x, std::uint32_t, unsigned) { return x / 64.0; }, 64, 8);
        QCOMPARE(render::fx::layer_grid(grey8.page(0), layer_of(grey8, "pic")).levels, 255u);
        auto eight = blank();
        core::Layer paint;
        paint.id = "paint";
        paint.kind = core::LayerKind::Raster;
        eight.edit_page(0).layers.push_back(paint);
        QCOMPARE(render::fx::layer_grid(eight.page(0), layer_of(eight, "paint")).levels, 255u);
        // a rank filter and the surface blur reckoned on floats by their work there (the values sorted, more bins)
        for (const auto& [kind, params] : {std::pair{"noise_median", Json{{"radius", 50}}}, std::pair{"noise_dust_and_scratches", Json{{"radius", 50}}},
                                           std::pair{"blur_surface", Json{{"radius", 30}}}})
            QVERIFY2(render::fx::work(kind, params, 700000, 0.36, 0) > render::fx::work(kind, params, 700000, 0.36, 65535), kind);
        // the kinds of one pixel on B4 at 600 dpi: heavy (the engine's reading and writing of each pixel counted)
        for (const char* kind : {"blur_average", "noise_add", "video_de_interlace", "other_offset"})
            QVERIFY2(render::fx::work(kind, Json::object(), 6071ULL * 8598, 3) >= render::fx::kHeavyWork, kind);
        // the sharpening's exact Gaussian costs its radius
        QVERIFY(render::fx::work("sharpen_unsharp_mask", Json{{"radius", 100}}, 1000, 1) > 5 * render::fx::work("sharpen_unsharp_mask", Json{{"radius", 10}}, 1000, 1));
    }

    void aPreviewsCopyIsAtTheScreensResolution() {
        const auto doc = precise_page(600, 600, pattern(600, 600), true);
        const core::Document small = render::fx::preview_book(doc, 0, "paint", 200);
        const core::ColorRasterView view(*layer_of(small, "paint").color_raster);
        QCOMPARE(view.width(), 200u);
        QCOMPARE(view.metadata("")["precision"], Json("f32"));
        QVERIFY(doc.page(0).layers[0].color_raster != small.page(0).layers[0].color_raster);  // (a copy: the book as it was)
        // (a filter on it scales with it: s = 1 on a 200-pixel grid of a 1-inch page)
        QVERIFY(std::fabs(render::fx::layer_grid(small.page(0), layer_of(small, "paint")).s - 1.0) < 1e-9);
        // an ink layer too; a small one as it is
        const auto inks = ink_page("cmyk", [](std::uint32_t x, std::uint32_t, unsigned c) { return c == 3 ? x / 64.0 : 0.0; });
        QCOMPARE(core::ink_raster_header(*layer_of(render::fx::preview_book(inks, 0, "pic", 32), "pic").ink_raster).width, 32u);
        QVERIFY(layer_of(render::fx::preview_book(inks, 0, "pic", 100), "pic").ink_raster == layer_of(inks, "pic").ink_raster);
    }


    // Every kind as a smart filter of a smart object on an 8-bit page, a 16-bit one and a CMYK one: drawn (the smart
    // object's picture changed by it — not left out as a filter that cannot be drawn), the same when drawn again.
    void everyKindAsASmartFilter() {
        std::string bytes;
        for (int y = 0; y < 90; ++y)
            for (int x = 0; x < 120; ++x) {
                const bool square = (x / 8 + y / 8) % 2 == 0;
                for (const int v : {square ? 230 : (x * 7) % 256, square ? 40 : (y * 5) % 256, (x * y) % 256, 255}) bytes.push_back(static_cast<char>(v));
            }
        const std::string png = render::write_png(render::Image::frombytes("RGBA", render::Size{120, 90}, bytes));
        for (const Json& mode : {Json(), Json{{"op", "set_color_mode"}, {"mode", "rgb"}, {"depth", 16}},
                                 Json{{"op", "set_color_mode"}, {"mode", "cmyk"}, {"depth", 16}, {"profile", "coated-cmyk"}}}) {
            auto doc = blank(40);
            Json ops = Json::array();
            if (!mode.is_null()) ops.push_back(mode);
            ops.push_back(Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "p1"}});
            ops.push_back(Json{{"op", "put_raster"}, {"page", 1}, {"id", "p1"}, {"png_base64", core::b64encode(png)}});
            ops.push_back(Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({"p1"})}, {"new_id", "so"}});
            doc = core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor("human:test")).doc;
            const auto drawn = [](const core::Document& d) {
                render::RenderOptions options;
                options.mode = "proof";
                options.inks = d.page(0).color.has_value();
                options.precise = !d.page(0).color && d.page(0).rgb && d.page(0).rgb->depth > 8;
                return render::render_page(d.page(0), 200, options, &d).image.tobytes();
            };
            const std::string plain = drawn(doc);
            for (const core::FilterInfo& info : core::filter_infos()) {
                const std::string kind(info.kind);
                Json add{{"op", "smart_filter_add"}, {"page", 1}, {"id", "so"}, {"kind", kind}};
                add.update(kind_params(kind));
                const auto with = core::CommandBus(render::ops_registry()).apply(doc, Json::array({add}), core::Actor("human:test")).doc;
                const std::string once = drawn(with);
                QVERIFY2(once != plain, (kind + " " + mode.dump()).c_str());
                QVERIFY2(once == drawn(with), (kind + " again " + mode.dump()).c_str());
            }
        }
    }

    // --- D1: PhotoCraft's tests of ぼかし・シャープ・ノイズ・その他・ビデオ (tests.rs, tests_ext.rs) -------------------------------

    void unsharpThresholdAndContrast() {
        const std::string raster = precise(40, 30, pattern(40, 30), true);
        QCOMPARE(run_precise(raster, "sharpen_unsharp_mask", Json{{"amount", 200}, {"radius", 2}, {"threshold", 255}}), raster);  // (255: none)
        const std::string sharp = run_precise(raster, "sharpen_unsharp_mask", Json{{"amount", 200}, {"radius", 2}, {"threshold", 0}});
        const auto spread = [](const std::string& r) {
            double mean = 0, var = 0;
            std::vector<double> reds;
            for (std::uint32_t y = 5; y < 25; ++y)
                for (std::uint32_t x = 5; x < 35; ++x) reds.push_back(pixel(r, x, y)[0]);
            for (const double v : reds) mean += v / double(reds.size());
            for (const double v : reds) var += (v - mean) * (v - mean);
            return var;
        };
        QVERIFY(spread(sharp) > spread(raster));
    }

    void highPassOfFlatIsMiddleGrey() {
        const auto p = pixel(run_precise(precise(40, 30, flat(40, 30, {0.9f, 0.1f, 0.4f, 1.0f}), true), "other_high_pass", Json{{"radius", 4}}), 20, 15);
        QVERIFY(std::fabs(p[0] - 0.5) < 1e-5 && std::fabs(p[1] - 0.5) < 1e-5);
    }

    void noiseIsDeterministicMonochromeAndCentred() {
        const std::string raster = precise(40, 30, flat(40, 30, {0.5f, 0.5f, 0.5f, 1.0f}), true);
        const Json params{{"amount", 50}, {"distribution", "uniform"}, {"monochromatic", true}, {"seed", 1}};
        const std::string a = run_precise(raster, "noise_add", params);
        QCOMPARE(run_precise(raster, "noise_add", params), a);
        double mean = 0;
        for (std::uint32_t y = 0; y < 30; ++y)
            for (std::uint32_t x = 0; x < 40; ++x) {
                const auto p = pixel(a, x, y);
                QCOMPARE(p[0], p[1]);
                QCOMPARE(p[1], p[2]);
                QVERIFY(std::fabs(p[0] - 0.5) <= 0.25 + 1e-6);
                mean += p[0] / 1200.0;
            }
        QVERIFY2(std::fabs(mean - 0.5) < 0.03, qPrintable(QString::number(mean)));
        Json other = params;
        other["seed"] = 2;
        QVERIFY(run_precise(raster, "noise_add", other) != a);
    }

    void medianAndDustRemoveASpeck() {
        std::vector<std::array<float, 4>> px = flat(40, 30, {0.2f, 0.2f, 0.2f, 1.0f});
        px[10 * 40 + 10] = {1, 1, 1, 1};
        const std::string raster = precise(40, 30, px, true);
        QVERIFY(std::fabs(pixel(run_precise(raster, "noise_median", Json{{"radius", 1}}), 10, 10)[0] - 0.2) < 1e-6);
        QVERIFY(std::fabs(pixel(run_precise(raster, "noise_dust_and_scratches", Json{{"radius", 1}, {"threshold", 255}}), 10, 10)[0] - 1.0) < 1e-6);
        QVERIFY(std::fabs(pixel(run_precise(raster, "noise_dust_and_scratches", Json{{"radius", 1}, {"threshold", 50}}), 10, 10)[0] - 0.2) < 1e-6);
    }

    void minimumMaximumSpread() {
        std::vector<std::array<float, 4>> px = flat(40, 30, {0.5f, 0.5f, 0.5f, 1.0f});
        px[10 * 40 + 10] = {1, 1, 1, 1};
        const std::string raster = precise(40, 30, px, true);
        const std::string mx = run_precise(raster, "other_maximum", Json{{"radius", 1}});
        QVERIFY(std::fabs(pixel(mx, 11, 11)[0] - 1.0) < 1e-6);
        QVERIFY(std::fabs(pixel(mx, 12, 10)[0] - 0.5) < 1e-6);
        QVERIFY(std::fabs(pixel(run_precise(raster, "other_minimum", Json{{"radius", 1}}), 10, 10)[0] - 0.5) < 1e-6);
    }

    // One white pixel on black: 明るさの最大値 spreads it over its shape — a disc (真円率) or a square (直角度) —, at 8 and 16
    // bits and in floats.
    void maximumRoundnessGrowsAPointIntoADisc() {
        for (const int depth : {8, 16, 32}) {
            std::vector<std::array<float, 4>> px = flat(40, 30, {0, 0, 0, 1});
            px[15 * 40 + 20] = {1, 1, 1, 1};
            for (const double radius : {1.0, 2.0, 3.5, 6.0}) {
                const auto made = [&](const char* preserve) {
                    const Json params{{"radius", radius}, {"preserve", preserve}};
                    std::vector<std::array<float, 4>> out;
                    if (depth == 8) {
                        std::string bytes;
                        for (const auto& p : px)
                            for (const float v : p) bytes.push_back(static_cast<char>(std::lround(v * 255)));
                        const auto surface = render::fx::eight_surface(render::Image::frombytes("RGBA", render::Size{40, 30}, bytes));
                        render::fx::apply(*surface, kernel("other_maximum"), params, core::FilterInks::Plates, Rect{0, 0, 40, 30}, nullptr, 1);
                        const std::string after = render::fx::take_image(*surface).tobytes();
                        for (std::size_t i = 0; i < 1200; ++i)
                            out.push_back({static_cast<unsigned char>(after[i * 4]) / 255.0f, 0, 0, 1});
                    } else {
                        const std::string after = run_precise(precise(40, 30, px, depth == 32), "other_maximum", params);
                        for (std::uint32_t i = 0; i < 1200; ++i) out.push_back({static_cast<float>(pixel(after, i % 40, i / 40)[0]), 0, 0, 1});
                    }
                    return out;
                };
                const auto round = made("roundness"), square = made("squareness");
                const int rs = static_cast<int>(std::lround(radius));
                for (int y = 0; y < 30; ++y)
                    for (int x = 0; x < 40; ++x) {
                        const int dx = x - 20, dy = y - 15;
                        QCOMPARE(round[std::size_t(y * 40 + x)][0] > 0.5f, double(dx * dx + dy * dy) <= radius * radius);
                        QCOMPARE(square[std::size_t(y * 40 + x)][0] > 0.5f, std::abs(dx) <= rs && std::abs(dy) <= rs);
                    }
            }
        }
    }

    void offsetWrapsRepeatsAndClears() {
        const std::string raster = precise(40, 30, pattern(40, 30), false);
        QCOMPARE(run_precise(raster, "other_offset", Json{{"horizontal", 40}, {"vertical", 30}, {"undefined_areas", "wrap"}}), raster);
        const std::string clear = run_precise(raster, "other_offset", Json{{"horizontal", 5}, {"vertical", 0}, {"undefined_areas", "transparent"}});
        QCOMPARE(pixel(clear, 2, 3)[3], 0.0);
        QCOMPARE(pixel(clear, 15, 3), pixel(raster, 10, 3));
        QCOMPARE(pixel(run_precise(raster, "other_offset", Json{{"horizontal", 5}, {"vertical", 0}, {"undefined_areas", "repeat"}}), 2, 3), pixel(raster, 0, 3));
    }

    void motionBlurSpreadsAlongItsAngleOnly() {
        std::vector<std::array<float, 4>> px = flat(40, 30, {0, 0, 0, 1});
        for (std::uint32_t y = 0; y < 30; ++y) px[y * 40 + 20] = {1, 1, 1, 1};
        const std::string raster = precise(40, 30, px, true);
        QVERIFY(pixel(run_precise(raster, "blur_motion", Json{{"angle", 0}, {"distance", 6}}), 22, 15)[0] > 0.05);
        QVERIFY(pixel(run_precise(raster, "blur_motion", Json{{"angle", 90}, {"distance", 6}}), 22, 15)[0] < 1e-4);
    }

    // (tests_ext.rs noisy: a flat grey with a Gaussian noise of 20 %, seed 4)
    static std::string noisy() {
        return run_precise(precise(40, 30, flat(40, 30, {0.5f, 0.5f, 0.5f, 1.0f}), true), "noise_add",
                           Json{{"amount", 20}, {"distribution", "gaussian"}, {"monochromatic", false}, {"seed", 4}});
    }
    static double variance(const std::string& r, const Rect& box, unsigned c) {
        double mean = 0, var = 0;
        const double count = double(box.area());
        for (int y = box.y0; y < box.y1; ++y)
            for (int x = box.x0; x < box.x1; ++x) mean += pixel(r, std::uint32_t(x), std::uint32_t(y))[c] / count;
        for (int y = box.y0; y < box.y1; ++y)
            for (int x = box.x0; x < box.x1; ++x) {
                const double d = pixel(r, std::uint32_t(x), std::uint32_t(y))[c] - mean;
                var += d * d;
            }
        return var / count;
    }

    void reduceNoiseLowersNoise() {
        const std::string raster = noisy();
        const std::string out = run_precise(raster, "noise_reduce", Json{{"strength", 10}, {"preserve_details", 0}, {"reduce_color_noise", 100},
                                                                         {"sharpen_details", 0}, {"remove_jpeg_artifact", false}});
        for (unsigned c = 0; c < 3; ++c) QVERIFY2(variance(out, Rect{8, 8, 32, 22}, c) < variance(raster, Rect{8, 8, 32, 22}, c) * 0.5, qPrintable(QString::number(c)));
    }

    void smartBlurKeepsEdgesAndSmoothsNoise() {
        std::string raster = noisy();
        {  // (the right half white)
            const core::ColorRasterView view(raster);
            std::vector<std::array<float, 4>> px;
            for (std::uint32_t i = 0; i < 1200; ++i) {
                const auto p = view.pixel(i);
                px.push_back(i % 40 >= 20 ? std::array<float, 4>{1, 1, 1, 1} : std::array<float, 4>{float(p[0]), float(p[1]), float(p[2]), 1});
            }
            raster = precise(40, 30, px, true);
        }
        const std::string out = run_precise(raster, "blur_smart", Json{{"radius", 3}, {"threshold", 60}, {"quality", "high"}, {"mode", "normal"}});
        QVERIFY(std::fabs(pixel(out, 20, 15)[0] - 1.0) < 1e-5 && pixel(out, 19, 15)[0] < 0.8);
        QVERIFY(variance(out, Rect{4, 4, 16, 26}, 0) < variance(raster, Rect{4, 4, 16, 26}, 0));
        const std::string edges = run_precise(raster, "blur_smart", Json{{"radius", 3}, {"threshold", 60}, {"quality", "high"}, {"mode", "edge_only"}});
        QCOMPARE(pixel(edges, 20, 15)[0], 1.0);
        QCOMPARE(pixel(edges, 30, 15)[0], 0.0);
    }

    void shapedBlursSpreadAPointIntoTheShape() {
        std::vector<std::array<float, 4>> px = flat(40, 30, {0, 0, 0, 1});
        px[15 * 40 + 20] = {1, 1, 1, 1};
        const std::string raster = precise(40, 30, px, true);
        const std::string ring = run_precise(raster, "blur_shape", Json{{"radius", 6}, {"shape", "ring"}});
        QCOMPARE(pixel(ring, 20, 15)[0], 0.0);  // (a ring has a hole)
        QVERIFY(pixel(ring, 25, 15)[0] > 0.0);
        const std::string lens = run_precise(raster, "blur_lens", Json{{"radius", 6}, {"shape", "triangle"}, {"brightness", 100}, {"threshold", 200}});
        double sum = 0;
        for (std::uint32_t i = 0; i < 1200; ++i) sum += pixel(lens, i % 40, i / 40)[0];
        QVERIFY2(sum > 1.5, qPrintable(QString::number(sum)));  // (the highlight bloomed)
        QVERIFY(pixel(lens, 20, 20)[0] != pixel(lens, 20, 10)[0]);  // (a triangle, not a circle)
    }

    // ぼかし（レンズ）'s depth from a plane (the layer's mask): the focal plane untouched, the far one blurred.
    void lensBlursDepthKeepsTheFocalPlaneSharp() {
        const std::string raster = precise(40, 30, pattern(40, 30), true);
        std::vector<float> depth(1200, 0.0f);
        for (std::uint32_t y = 0; y < 30; ++y)
            for (std::uint32_t x = 20; x < 40; ++x) depth[y * 40 + x] = 1.0f;
        const auto surface = render::fx::precise_surface(raster);
        render::fx::Reads reads;
        reads.plane = &depth;
        render::fx::apply(*surface, kernel("blur_lens"), Json{{"radius", 5}, {"blade_curvature", 100}, {"depth_map", "layer_mask"}}, core::FilterInks::Plates,
                          Rect{0, 0, 40, 30}, nullptr, 1, nullptr, {}, reads);
        const std::string out = render::fx::take_bytes(*surface);
        QCOMPARE(pixel(out, 5, 10), pixel(raster, 5, 10));
        QVERIFY(pixel(out, 30, 10) != pixel(raster, 30, 10));
        // (and through filter_raster: the layer's own mask)
        auto doc = precise_page(40, 30, pattern(40, 30), true);
        render::Image mask = render::Image::create("L", render::Size{40, 30}, render::Ink(0));
        mask.paste(render::Ink(255), render::Box{20, 0, 40, 30});
        core::Mask m;
        m.png = std::make_shared<const std::string>(render::write_png(mask));
        doc.edit_page(0).layers[0].mask = m;
        // (the layer of 40 pixels over the 1-inch page: s = 0.2 — a radius of 30 working pixels is 6 of its)
        const auto done = run(doc, Json{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "blur_lens"}, {"radius", 30}, {"blade_curvature", 100},
                                        {"depth_map", "layer_mask"}}).doc;
        const std::string& now = *layer_of(done, "paint").color_raster;
        QCOMPARE(pixel(now, 5, 10), pixel(raster, 5, 10));
        QVERIFY(pixel(now, 30, 10) != pixel(raster, 30, 10));
    }

    void customKernelShiftsAndOffsets() {
        const std::string raster = precise(40, 30, pattern(40, 30), true);
        Json right = Json::array(), id = Json::array();
        for (int i = 0; i < 25; ++i) {
            right.push_back(i == 13 ? 2 : 0);
            id.push_back(i == 12 ? 1 : 0);
        }
        QCOMPARE(pixel(run_precise(raster, "other_custom", Json{{"kernel", right}, {"scale", 2}, {"offset", 0}}), 10, 10)[0], pixel(raster, 11, 10)[0]);
        const std::string off = run_precise(precise(40, 30, flat(40, 30, {0.2f, 0.2f, 0.2f, 1.0f}), true), "other_custom", Json{{"kernel", id}, {"scale", 1}, {"offset", 51}});
        QVERIFY(std::fabs(pixel(off, 5, 5)[0] - 0.4) < 1e-5);
    }

    void hsbEncodesAndDecodes() {
        const std::string red = precise(40, 30, flat(40, 30, {1, 0, 0, 1}), true);
        const std::string hsb = run_precise(red, "other_hsb_hsl", Json{{"input_mode", "rgb"}, {"row_order", "hsb"}});
        QCOMPARE(pixel(hsb, 3, 3), (std::array<double, 4>{0, 1, 1, 1}));
        QCOMPARE(pixel(run_precise(hsb, "other_hsb_hsl", Json{{"input_mode", "hsb"}, {"row_order", "rgb"}}), 3, 3), pixel(red, 3, 3));
    }

    void deinterlaceRebuildsAField() {
        std::vector<std::array<float, 4>> px;
        for (std::uint32_t y = 0; y < 30; ++y)
            for (std::uint32_t x = 0; x < 40; ++x) {
                const float v = y % 2 == 0 ? 0.8f : 0.2f;
                px.push_back({v, v, v, 1});
            }
        const std::string raster = precise(40, 30, px, true);
        const std::string odd = run_precise(raster, "video_de_interlace", Json{{"eliminate", "odd_fields"}, {"create_by", "duplication"}});
        for (std::uint32_t y = 0; y < 30; ++y) QVERIFY2(std::fabs(pixel(odd, 10, y)[0] - 0.2) < 1e-6, qPrintable(QString::number(y)));
        QVERIFY(std::fabs(pixel(run_precise(raster, "video_de_interlace", Json{{"eliminate", "even_fields"}, {"create_by", "interpolation"}}), 10, 5)[0] - 0.8) < 1e-6);
        // (on a grid of three pixels a working one: lines of three rows, the same field rebuilt)
        std::vector<std::array<float, 4>> fine;
        for (std::uint32_t y = 0; y < 30; ++y)
            for (std::uint32_t x = 0; x < 40; ++x) {
                const float v = (y / 3) % 2 == 0 ? 0.8f : 0.2f;
                fine.push_back({v, v, v, 1});
            }
        const std::string lines = run_precise(precise(40, 30, fine, true), "video_de_interlace", Json{{"eliminate", "odd_fields"}, {"create_by", "duplication"}}, nullptr, {}, 3);
        for (std::uint32_t y = 0; y < 30; ++y) QVERIFY2(std::fabs(pixel(lines, 10, y)[0] - 0.2) < 1e-6, qPrintable(QString::number(y)));
    }

    void ntscLimitsSaturatedColoursOnly() {
        const std::string grey = precise(40, 30, flat(40, 30, {0.5f, 0.5f, 0.5f, 1}), true);
        QCOMPARE(pixel(run_precise(grey, "video_ntsc_colors", Json::object()), 3, 3), pixel(grey, 3, 3));
        const auto y = pixel(run_precise(precise(40, 30, flat(40, 30, {1, 1, 0, 1}), true), "video_ntsc_colors", Json::object()), 3, 3);
        QVERIFY(y[2] > 0.05);  // (yellow desaturated)
        QVERIFY(std::fabs((0.299 * y[0] + 0.587 * y[1] + 0.114 * y[2]) - (0.299 + 0.587)) < 1e-3);  // (its luma kept)
    }

    // --- D1: the fast algorithms against the ones as written (design 4.3) -----------------------------------------------

    // 明るさの中間値・ダスト＆スクラッチ (a sliding histogram), 明るさの最大値・最小値 (running extremes) and ぼかし（表面） (sliding
    // bins) on 8 and 16 bits: the same values as PhotoCraft's algorithms as written; ぼかし（表面） on 16 bits and floats within
    // half a level of 8 bits. The same whatever the tiling.
    void fastAlgorithmsAgreeWithTheNaive() {
        using render::fx::Method;
        const std::uint32_t w = 57, h = 43;
        std::string bytes;
        for (std::uint32_t y = 0; y < h; ++y)
            for (std::uint32_t x = 0; x < w; ++x)
                for (const unsigned v : {(x * 37 + y * 11) % 256, (x * x + 3 * y) % 251, ((x ^ y) * 29) % 256, (x * 7 + y) % 9 == 0 ? 0u : 255u})
                    bytes.push_back(static_cast<char>(v));
        const render::Image picture = render::Image::frombytes("RGBA", render::Size{int(w), int(h)}, bytes);
        std::vector<std::array<float, 4>> px;
        for (std::uint32_t i = 0; i < w * h; ++i)
            px.push_back({std::min(1.0f, static_cast<unsigned char>(bytes[i * 4]) / 255.0f + static_cast<float>((i * 13) % 7) / 65535.0f),
                          static_cast<unsigned char>(bytes[i * 4 + 1]) / 255.0f, static_cast<unsigned char>(bytes[i * 4 + 2]) / 255.0f,
                          static_cast<unsigned char>(bytes[i * 4 + 3]) / 255.0f});
        // (where two rasters first differ: the pixel, the channel and both values)
        const auto differ = [w](const std::string& a, const std::string& b, bool precise_bytes) -> QString {
            if (a == b) return QString();
            if (!precise_bytes) {
                for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i)
                    if (a[i] != b[i])
                        return QStringLiteral("at %1,%2 c%3: %4 %5").arg(i / 4 % w).arg(i / 4 / w).arg(i % 4).arg(int(static_cast<unsigned char>(a[i]))).arg(int(static_cast<unsigned char>(b[i])));
                return QStringLiteral("sizes");
            }
            const core::ColorRasterView va(a), vb(b);
            for (std::size_t i = 0; i < std::size_t(va.width()) * va.height(); ++i) {
                const auto p = va.pixel(i), q = vb.pixel(i);
                for (unsigned c = 0; c < 4; ++c)
                    if (p[c] != q[c]) return QStringLiteral("at %1,%2 c%3: %4 %5").arg(i % w).arg(i / w).arg(c).arg(p[c] * 65535).arg(q[c] * 65535);
            }
            return QStringLiteral("header");
        };
        // (each algorithm on a surface's window: a kernel that calls it with the method given)
        const auto with = [](std::function<std::vector<float>(const render::fx::Window&, const Rect&, const render::fx::Ctx&)> work, int reach) {
            render::fx::Kernel k;
            k.kind = "test_rank";
            k.halo = [reach](const Json&, const render::fx::Ctx&) { return render::fx::Halo::margin(reach); };
            k.run = [work](const Json&, const render::fx::Window& src, const Rect& out, const render::fx::Ctx& ctx) { return work(src, out, ctx); };
            return k;
        };
        const auto on_eight = [&](const render::fx::Kernel& k, render::fx::Tiling tiling = {}) {
            const auto surface = render::fx::eight_surface(picture);
            render::fx::apply(*surface, k, Json::object(), core::FilterInks::Plates, Rect{0, 0, int(w), int(h)}, nullptr, 1, nullptr, tiling);
            return render::fx::take_image(*surface).tobytes();
        };
        const auto on_precise = [&](const render::fx::Kernel& k, bool f32, render::fx::Tiling tiling = {}) {
            const std::string raster = precise(w, h, px, f32);  // (the surface reads it while it lives)
            const auto surface = render::fx::precise_surface(raster);
            render::fx::apply(*surface, k, Json::object(), core::FilterInks::Plates, Rect{0, 0, int(w), int(h)}, nullptr, 1, nullptr, tiling);
            return render::fx::take_bytes(*surface);
        };
        for (const int r : {3, 5, 9}) {
            for (const std::optional<float> threshold : {std::optional<float>(), std::optional<float>(12.0f / 255.0f)}) {
                const auto by = [&](Method m) {
                    return with([r, threshold, m](const auto& src, const auto& out, const auto& ctx) { return render::fx::median(src, out, ctx, r, threshold, m); }, r + 1);
                };
                const std::string fast = on_eight(by(Method::Fast)), naive = on_eight(by(Method::Naive));
                QVERIFY2(fast == naive, qPrintable(QStringLiteral("median 8 bits r %1 %2: %3").arg(r).arg(bool(threshold)).arg(differ(fast, naive, false))));
                QVERIFY(fast == on_eight(by(Method::Fast), {7, 3}));
                const std::string fast16 = on_precise(by(Method::Fast), false), naive16 = on_precise(by(Method::Naive), false);
                QVERIFY2(fast16 == naive16, qPrintable(QStringLiteral("median 16 bits r %1 %2: %3").arg(r).arg(bool(threshold)).arg(differ(fast16, naive16, true))));
                QVERIFY(fast16 == on_precise(by(Method::Fast), false, {11, 2}));
                // (floats: the window's own values ranked — the same values)
                const std::string fast32 = on_precise(by(Method::Fast), true), naive32 = on_precise(by(Method::Naive), true);
                QVERIFY2(fast32 == naive32, qPrintable(QStringLiteral("median f32 r %1 %2: %3").arg(r).arg(bool(threshold)).arg(differ(fast32, naive32, true))));
                QVERIFY(fast32 == on_precise(by(Method::Fast), true, {11, 2}));
            }
            for (const double radius : {double(r), r + 0.5}) {
                for (const bool square : {true, false})
                    for (const bool max : {true, false}) {
                        const auto by = [&](Method m) {
                            return with([radius, square, max, m](const auto& src, const auto& out, const auto&) { return render::fx::extreme(src, out, radius, square, max, m); },
                                        int(std::ceil(radius)) + 1);
                        };
                        QVERIFY(on_eight(by(Method::Fast)) == on_eight(by(Method::Naive)));
                        QVERIFY(on_precise(by(Method::Fast), true) == on_precise(by(Method::Naive), true));
                        QVERIFY(on_precise(by(Method::Fast), true, {5, 4}) == on_precise(by(Method::Fast), true));
                    }
            }
        }
        for (const int r : {9, 14}) {
            for (const float t : {5.0f / 255.0f * 2.5f, 40.0f / 255.0f * 2.5f, 2.5f}) {
                const auto by = [&](Method m) {
                    return with([r, t, m](const auto& src, const auto& out, const auto& ctx) { return render::fx::surface(src, out, ctx, r, t, m); }, r + 1);
                };
                const std::string fast = on_eight(by(Method::Fast)), naive = on_eight(by(Method::Naive));
                QVERIFY2(fast == naive, qPrintable(QStringLiteral("surface 8 bits r %1 t %2: %3").arg(r).arg(t).arg(differ(fast, naive, false))));
                QVERIFY(fast == on_eight(by(Method::Fast), {13, 3}));
                for (const bool f32 : {false, true}) {
                    const std::string a = on_precise(by(Method::Fast), f32), b = on_precise(by(Method::Naive), f32);
                    double worst = 0;
                    for (std::uint32_t i = 0; i < w * h; ++i)
                        for (unsigned c = 0; c < 4; ++c) worst = std::max(worst, std::fabs(pixel(a, i % w, i / w)[c] - pixel(b, i % w, i / w)[c]));
                    // (each sample weighed as written: 16 bits the same, floats within their last bits)
                    if (!f32) QVERIFY2(a == b, qPrintable(QStringLiteral("surface u16 r %1 t %2: %3").arg(r).arg(t).arg(differ(a, b, true))));
                    QVERIFY2(worst <= 1e-6, qPrintable(QStringLiteral("surface %1 r %2 t %3: %4").arg(f32 ? "f32" : "u16").arg(r).arg(t).arg(worst)));
                    QVERIFY(a == on_precise(by(Method::Fast), f32, {9, 2}));
                }
            }
        }
    }

    // The fast algorithms where the review found them unchecked: the surface blur on a shape at its threshold's very edge
    // (points ringed by a step of exactly the threshold, 8 bits raised to 16 and to floats: P3-1) and pages of inks (CMYK
    // and Lab at 16 bits, grey at 8: P3-2) — the same values as written.
    void fastAlgorithmsOnTheEdgeAndOnInks() {
        using render::fx::Method;
        const auto with = [](std::function<std::vector<float>(const render::fx::Window&, const Rect&, const render::fx::Ctx&)> work, int reach) {
            render::fx::Kernel k;
            k.kind = "test_rank";
            k.halo = [reach](const Json&, const render::fx::Ctx&) { return render::fx::Halo::margin(reach); };
            k.run = [work](const Json&, const render::fx::Window& src, const Rect& out, const render::fx::Ctx& ctx) { return work(src, out, ctx); };
            return k;
        };
        const auto surface_by = [&](int r, float t, Method m) {
            return with([r, t, m](const auto& src, const auto& out, const auto& ctx) { return render::fx::surface(src, out, ctx, r, t, m); }, r + 1);
        };
        // (levels 15 with points of 10 and 20 and a block of 30: steps of 5 levels, ブラー（表面）'s threshold 2 × 2.5)
        const std::uint32_t w = 61, h = 47;
        std::string bytes;
        std::vector<std::array<float, 4>> px;
        for (std::uint32_t y = 0; y < h; ++y)
            for (std::uint32_t x = 0; x < w; ++x) {
                const int level = x % 7 == 3 && y % 5 == 2 ? ((x + y) % 2 != 0 ? 10 : 20) : x > 40 && y > 30 ? 30 : 15;
                for (const int v : {level, 25 - level / 2, level, 255}) bytes.push_back(static_cast<char>(v));
                px.push_back({level / 255.0f, (25 - level / 2) / 255.0f, level / 255.0f, 1.0f});
            }
        const render::Image picture = render::Image::frombytes("RGBA", render::Size{int(w), int(h)}, bytes);
        for (const int r : {9, 12}) {
            for (const float t : {static_cast<float>(2 / 255.0 * 2.5), static_cast<float>(1 / 255.0 * 2.5), static_cast<float>(4 / 255.0 * 2.5)}) {
                const auto eight = [&](Method m) {
                    const auto surface = render::fx::eight_surface(picture);
                    render::fx::apply(*surface, surface_by(r, t, m), Json::object(), core::FilterInks::Plates, Rect{0, 0, int(w), int(h)}, nullptr, 1);
                    return render::fx::take_image(*surface).tobytes();
                };
                QVERIFY2(eight(Method::Fast) == eight(Method::Naive), qPrintable(QStringLiteral("8 bits r %1 t %2").arg(r).arg(t)));
                for (const bool f32 : {false, true}) {
                    const std::string raster = precise(w, h, px, f32);
                    const auto run = [&](Method m) {
                        const auto surface = render::fx::precise_surface(raster);
                        render::fx::apply(*surface, surface_by(r, t, m), Json::object(), core::FilterInks::Plates, Rect{0, 0, int(w), int(h)}, nullptr, 1);
                        return render::fx::take_bytes(*surface);
                    };
                    const std::string a = run(Method::Fast), b = run(Method::Naive);
                    double worst = 0;
                    for (std::uint32_t i = 0; i < w * h; ++i)
                        for (unsigned c = 0; c < 4; ++c) worst = std::max(worst, std::fabs(pixel(a, i % w, i / w)[c] - pixel(b, i % w, i / w)[c]));
                    if (!f32) QVERIFY2(a == b, qPrintable(QStringLiteral("u16 r %1 t %2: %3").arg(r).arg(t).arg(worst)));
                    QVERIFY2(worst <= 1e-6, qPrintable(QStringLiteral("%1 r %2 t %3: %4").arg(f32 ? "f32" : "u16").arg(r).arg(t).arg(worst)));
                }
            }
        }
        // pages of inks: the median (and dust), the extremes and the surface blur, fast and as written
        for (const auto& [mode, depth] : {std::pair{"cmyk", 16}, std::pair{"lab", 16}, std::pair{"gray", 8}}) {
            const auto doc = ink_page(mode, [](std::uint32_t x, std::uint32_t y, unsigned c) { return ((x * 37 + y * 11 + c * 23) % 97) / 96.0; }, 48, depth);
            const core::InkPixels inks = core::decode_ink_raster(*layer_of(doc, "pic").ink_raster);
            const core::PageColor& color = *doc.page(0).color;
            const auto on = [&](const render::fx::Kernel& k, core::FilterInks way) {
                auto surface = render::fx::ink_surface(inks, color);
                render::fx::apply(*surface, k, Json::object(), way, Rect{0, 0, 48, 48}, nullptr, 1);
                return render::fx::take_bytes(*surface);
            };
            const auto median_by = [&](std::optional<float> threshold, Method m) {
                return with([threshold, m](const auto& src, const auto& out, const auto& ctx) { return render::fx::median(src, out, ctx, 5, threshold, m); }, 6);
            };
            const auto extreme_by = [&](Method m) {
                return with([m](const auto& src, const auto& out, const auto&) { return render::fx::extreme(src, out, 3.5, false, true, m); }, 5);
            };
            for (const std::optional<float> threshold : {std::optional<float>(), std::optional<float>(10.0f / 255.0f)})
                QVERIFY2(on(median_by(threshold, Method::Fast), core::FilterInks::Plates) == on(median_by(threshold, Method::Naive), core::FilterInks::Plates), mode);
            QVERIFY2(on(extreme_by(Method::Fast), core::FilterInks::Lightness) == on(extreme_by(Method::Naive), core::FilterInks::Lightness), mode);
            QVERIFY2(on(surface_by(10, 0.1f, Method::Fast), core::FilterInks::Plates) == on(surface_by(10, 0.1f, Method::Naive), core::FilterInks::Plates), mode);
        }
    }

    // A Lab page: ハイパス and カスタム about their middle — L at its middle grey, a and b at their 0 (as RGB's colours about
    // grey) —, グレースケールノイズ on L alone (grey grains: a and b as they were) (review P3-9).
    void aLabPageAboutItsMiddle() {
        const double neutral = 128.0 / 255.0;
        const auto lab = ink_page("lab", [](std::uint32_t, std::uint32_t, unsigned c) { return c == 0 ? 0.7 : c == 1 ? 0.3 : 0.8; });
        const auto values = [](const core::Document& d) { return core::decode_ink_raster(*layer_of(d, "pic").ink_raster); };
        const core::InkPixels high = values(run(lab, Json{{"op", "filter_raster"}, {"page", 1}, {"id", "pic"}, {"kind", "other_high_pass"}, {"radius", 4}}).doc);
        QVERIFY(std::fabs(high.value(32 * 64 + 32, 0) - 0.5) < 1e-4);
        QVERIFY(std::fabs(high.value(32 * 64 + 32, 1) - neutral) < 1e-4);
        QVERIFY(std::fabs(high.value(32 * 64 + 32, 2) - neutral) < 1e-4);
        // カスタム: a kernel of the difference from the right neighbour (its sum 0) and an offset: L the offset, a and b their 0;
        // the identity: as it was
        Json edge = Json::array(), same = Json::array();
        for (int i = 0; i < 25; ++i) {
            edge.push_back(i == 12 ? 1 : i == 13 ? -1 : 0);
            same.push_back(i == 12 ? 1 : 0);
        }
        const core::InkPixels custom = values(
            run(lab, Json{{"op", "filter_raster"}, {"page", 1}, {"id", "pic"}, {"kind", "other_custom"}, {"kernel", edge}, {"scale", 1}, {"offset", 128}}).doc);
        QVERIFY(std::fabs(custom.value(32 * 64 + 32, 0) - neutral) < 1e-4);
        QVERIFY(std::fabs(custom.value(32 * 64 + 32, 1) - neutral) < 1e-4);
        QVERIFY(std::fabs(custom.value(32 * 64 + 32, 2) - neutral) < 1e-4);
        const core::InkPixels kept = values(run(lab, Json{{"op", "filter_raster"}, {"page", 1}, {"id", "pic"}, {"kind", "other_custom"}, {"kernel", same}}).doc);
        const core::InkPixels was = values(lab);
        for (std::size_t i = 0; i < was.pixel_count(); ++i)
            for (unsigned c = 0; c < 3; ++c) QVERIFY(std::fabs(kept.value(i, c) - was.value(i, c)) <= 1.0 / 65535.0);
        // グレースケールノイズ: L alone; colour noise: a and b too
        const core::InkPixels grey = values(
            run(lab, Json{{"op", "filter_raster"}, {"page", 1}, {"id", "pic"}, {"kind", "noise_add"}, {"amount", 25}, {"monochromatic", true}, {"seed", 3}}).doc);
        const core::InkPixels colour = values(run(lab, Json{{"op", "filter_raster"}, {"page", 1}, {"id", "pic"}, {"kind", "noise_add"}, {"amount", 25}, {"seed", 3}}).doc);
        bool lightness_moved = false, colour_moved = false;
        for (std::size_t i = 0; i < was.pixel_count(); ++i) {
            QCOMPARE(grey.value(i, 1), was.value(i, 1));
            QCOMPARE(grey.value(i, 2), was.value(i, 2));
            lightness_moved = lightness_moved || grey.value(i, 0) != was.value(i, 0);
            colour_moved = colour_moved || colour.value(i, 1) != was.value(i, 1);
        }
        QVERIFY(lightness_moved && colour_moved);
        // (an RGB page's ハイパス: grey as before)
        const auto p = pixel(run_precise(precise(40, 30, flat(40, 30, {0.9f, 0.1f, 0.4f, 1.0f}), true), "other_high_pass", Json{{"radius", 4}}), 20, 15);
        QVERIFY(std::fabs(p[0] - 0.5) < 1e-5 && std::fabs(p[2] - 0.5) < 1e-5);
    }

    // アンシャープマスク・ハイパス blur by the exact Gaussian at every radius (upstream's with_blur: no box passes past 4, whose
    // difference × the amount would show — review P3-6): ハイパス of a step against the Gaussian worked out here.
    void theSharpeningsGaussianIsExact() {
        const std::uint32_t w = 96, h = 6;
        std::vector<std::array<float, 4>> px;
        for (std::uint32_t y = 0; y < h; ++y)
            for (std::uint32_t x = 0; x < w; ++x) px.push_back(x < 48 ? std::array<float, 4>{0.2f, 0.2f, 0.2f, 1.0f} : std::array<float, 4>{0.8f, 0.8f, 0.8f, 1.0f});
        const double sigma = 6;
        const std::string high = run_precise(precise(w, h, px, true), "other_high_pass", Json{{"radius", sigma}});
        const int reach = static_cast<int>(std::ceil(3 * sigma));
        double total = 0;
        for (int i = -reach; i <= reach; ++i) total += std::exp(-i * i / (2 * sigma * sigma));
        double worst = 0;
        for (int x = 0; x < int(w); ++x) {
            double blurred = 0;
            for (int i = -reach; i <= reach; ++i) blurred += std::exp(-i * i / (2 * sigma * sigma)) / total * (std::clamp(x + i, 0, int(w) - 1) < 48 ? 0.2 : 0.8);
            const double o = x < 48 ? 0.2 : 0.8;
            worst = std::max(worst, std::fabs(pixel(high, std::uint32_t(x), 3)[0] - (0.5 + o - blurred)));
        }
        QVERIFY2(worst < 2e-5, qPrintable(QString::number(worst)));
    }

    // ぼかし（シェイプ）・ぼかし（レンズ） on HDR samples too large for their whole-number sums (1e18: past 2^62 in a row's
    // prefix sums): summed as they are instead — a flat area stays flat, the same whatever the tiling (review P3-3).
    void shapedBlursOfHugeSamples() {
        std::vector<std::array<float, 4>> px;
        for (std::uint32_t y = 0; y < 30; ++y)
            for (std::uint32_t x = 0; x < 40; ++x) px.push_back(x < 20 ? std::array<float, 4>{1e18f, 2e18f, 1e18f, 1.0f} : std::array<float, 4>{2e18f, 1e18f, 2e18f, 1.0f});
        const std::string raster = precise(40, 30, px, true);
        for (const auto& [kind, params] : {std::pair{"blur_shape", Json{{"radius", 5}, {"shape", "circle"}}}, std::pair{"blur_lens", Json{{"radius", 4}}}}) {
            const std::string a = run_precise(raster, kind, params, nullptr, {256, 1});
            QVERIFY2(a == run_precise(raster, kind, params, nullptr, {7, 3}), kind);
            const auto inside = pixel(a, 5, 15), across = pixel(a, 20, 15);
            QVERIFY2(std::fabs(inside[0] / 1e18 - 1.0) < 1e-5 && std::fabs(inside[1] / 2e18 - 1.0) < 1e-5, kind);
            QVERIFY2(across[0] > 1.05e18 && across[0] < 1.95e18, kind);  // (blurred across the edge)
        }
    }

    // --- D1: 平均, the presets, a page of inks, the look at 200 and 600 dpi -----------------------------------------------

    // 平均 (filter_raster blur_average): what the selection covers filled with its mean colour (each colour weighed by its
    // alpha) — an ellipse's, not its box's —; outside it as it was; with no selection the layer's mean.
    void averageIsTheMeanInsideTheSelection() {
        // a precise layer: the left half red, the right half blue (PhotoCraft's average_and_clouds)
        std::vector<std::array<float, 4>> px;
        for (std::uint32_t y = 0; y < 200; ++y)
            for (std::uint32_t x = 0; x < 200; ++x) px.push_back(x < 100 ? std::array<float, 4>{1, 0, 0, 1} : std::array<float, 4>{0, 0, 1, 1});
        const auto doc = precise_page(200, 200, px, false);
        const auto whole = run(doc, Json{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "blur_average"}}).doc;
        const auto p = pixel(*layer_of(whole, "paint").color_raster, 5, 5);
        QVERIFY(std::fabs(p[0] - 0.5) < 0.01 && std::fabs(p[2] - 0.5) < 0.01);
        // an ellipse over three quarters red: its mean, its box's outside it as it was
        const Json area{{"ellipse", Json::array({kInch * 0.1, kInch * 0.25, kInch * 0.6, kInch * 0.5})}};  // ([x, y, w, h] mm)
        const auto inside = run(doc, Json{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "blur_average"}, {"area", area}}).doc;
        const std::string& now = *layer_of(inside, "paint").color_raster;
        double red = 0, count = 0;
        for (std::uint32_t y = 50; y < 150; ++y)
            for (std::uint32_t x = 20; x < 140; ++x) {
                const double u = (x + 0.5 - 80) / 60.0, v = (y + 0.5 - 100) / 50.0;
                if (u * u + v * v > 0.8) continue;  // (well inside)
                red += px[y * 200 + x][0];
                count += 1;
            }
        const auto mid = pixel(now, 80, 100);
        QVERIFY2(std::fabs(mid[0] - red / count) < 0.03, qPrintable(QStringLiteral("%1 %2").arg(mid[0]).arg(red / count)));
        QVERIFY(std::fabs(mid[0] + mid[2] - 1.0) < 1e-3);
        QCOMPARE(pixel(now, 21, 51), pixel(*layer_of(doc, "paint").color_raster, 21, 51));  // (the box's corner: outside the ellipse)
        QCOMPARE(pixel(now, 180, 100), pixel(*layer_of(doc, "paint").color_raster, 180, 100));
        // an 8-bit layer: the same
        auto eight = blank();
        render::Image picture = render::Image::create("RGBA", render::Size{200, 200}, render::Ink{0, 0, 255, 255});
        picture.paste(render::Ink{255, 0, 0, 255}, render::Box{0, 0, 100, 200});
        core::Layer layer;
        layer.id = "paint";
        layer.kind = core::LayerKind::Raster;
        layer.panel_clip = false;
        render::raster::save_raster(eight.page(0), layer, picture);
        eight.edit_page(0).layers.push_back(layer);
        const auto done = run(eight, Json{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "blur_average"}, {"area", area}}).doc;
        const render::Image after = render::raster::layer_pixels(done.page(0), layer_of(done, "paint")).convert("RGBA");
        QVERIFY2(std::fabs(after.getpixel(80, 100)[0] / 255.0 - red / count) < 0.03, qPrintable(QString::number(after.getpixel(80, 100)[0])));
        QCOMPARE(after.getpixel(21, 51), picture.getpixel(21, 51));
    }

    // The commands without a window send PhotoCraft's settings (filters.rs 45-50): ぼかし 0.6, ぼかし（強） 1.4, シャープ 60 %
    // 0.5 0, シャープ（強） 150 % 0.6 0, シャープ（輪郭のみ） 100 % 0.8 6, 輪郭以外をぼかす ぼかし（表面） 2 12 — each now a kind of this build.
    void thePresetsArePhotoCraftsSettings() {
        const std::map<std::string, std::pair<std::string, Json>> want{
            {"filter.blur.blur", {"blur_gaussian", Json{{"radius", 0.6}}}},
            {"filter.blur.blurMore", {"blur_gaussian", Json{{"radius", 1.4}}}},
            {"filter.sharpen.sharpen", {"sharpen_unsharp_mask", Json{{"amount", 60}, {"radius", 0.5}, {"threshold", 0}}}},
            {"filter.sharpen.sharpenMore", {"sharpen_unsharp_mask", Json{{"amount", 150}, {"radius", 0.6}, {"threshold", 0}}}},
            {"filter.sharpen.sharpenEdges", {"sharpen_unsharp_mask", Json{{"amount", 100}, {"radius", 0.8}, {"threshold", 6}}}},
            {"filter.noise.despeckle", {"blur_surface", Json{{"radius", 2}, {"threshold", 12}}}},
        };
        QCOMPARE(core::filter_presets().size(), want.size());
        const auto doc = precise_page(16, 16, pattern(16, 16), false);
        for (const core::FilterPreset& preset : core::filter_presets()) {
            const auto& [kind, params] = want.at(std::string(preset.upstream));
            QCOMPARE(std::string(preset.kind), kind);
            QCOMPARE(preset.params, params);
            QVERIFY(core::pc4_filter(preset.kind) && render::fx::kernel(preset.kind) != nullptr);
            Json op{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", kind}};
            op.update(params);
            QVERIFY2(refusal(doc, op).empty(), preset.upstream.data());
        }
    }

    // On a CMYK page 明るさの最大値 spreads the light (as on RGB: by the lightness), 明るさの中間値 each plate (a K line stays K).
    void aPageOfInksByTheLightness() {
        const auto doc = ink_page("cmyk", [](std::uint32_t x, std::uint32_t, unsigned c) { return c == 3 ? (x < 32 ? 0.9 : 0.1) : 0.0; });
        // (64 pixels over the 1-inch page: s = 0.32 — 4 working pixels are 1 of its)
        const auto most = run(doc, Json{{"op", "filter_raster"}, {"page", 1}, {"id", "pic"}, {"kind", "other_maximum"}, {"radius", 4}}).doc;
        const core::InkPixels after = core::decode_ink_raster(*layer_of(most, "pic").ink_raster);
        QVERIFY(std::fabs(after.value(10 * 64 + 31, 3) - 0.1) < 1e-3);  // (the light side grew)
        QVERIFY(std::fabs(after.value(10 * 64 + 29, 3) - 0.9) < 1e-3);
        const auto mid = run(doc, Json{{"op", "filter_raster"}, {"page", 1}, {"id", "pic"}, {"kind", "noise_median"}, {"radius", 6}}).doc;
        const core::InkPixels m = core::decode_ink_raster(*layer_of(mid, "pic").ink_raster);
        for (std::size_t i = 0; i < m.pixel_count(); ++i)
            for (unsigned c = 0; c < 3; ++c) QCOMPARE(m.value(i, c), 0.0);
    }

    // The same disc at 200 and 600 dpi: ぼかし（移動）, ぼかし（表面）, 明るさの中間値, 明るさの最大値, アンシャープマスク and
    // ノイズを軽減 of the same settings — the 600 dpi one shrunk to a third near the 200 dpi one.
    void theSameLookAtEveryResolutionD1() {
        const auto disc = [](std::uint32_t side) {
            std::vector<std::array<float, 4>> px;
            for (std::uint32_t y = 0; y < side; ++y)
                for (std::uint32_t x = 0; x < side; ++x) {
                    const double u = (x + 0.5) / side - 0.5, v = (y + 0.5) / side - 0.5;
                    px.push_back(u * u + v * v < 0.09 ? std::array<float, 4>{0.9f, 0.2f, 0.1f, 1.0f} : std::array<float, 4>{0.1f, 0.3f, 0.8f, 1.0f});
                }
            return px;
        };
        const std::vector<std::pair<Json, double>> cases{
            {Json{{"kind", "blur_motion"}, {"angle", 30}, {"distance", 12}}, 0.05},
            {Json{{"kind", "blur_surface"}, {"radius", 4}, {"threshold", 200}}, 0.06},
            {Json{{"kind", "noise_median"}, {"radius", 4}}, 0.06},
            {Json{{"kind", "other_maximum"}, {"radius", 3}}, 0.08},
            {Json{{"kind", "sharpen_unsharp_mask"}, {"amount", 100}, {"radius", 3}}, 0.06},
            {Json{{"kind", "blur_shape"}, {"radius", 6}}, 0.06},
        };
        for (const auto& [given, bound] : cases) {
            Json op{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}};
            op.update(given);
            const auto low = run(precise_page(200, 200, disc(200), false), op).doc;
            const auto high = run(precise_page(600, 600, disc(600), false), op).doc;
            const std::string& a = *layer_of(low, "paint").color_raster;
            const std::string& b = *layer_of(high, "paint").color_raster;
            double total = 0, apart = 0;
            for (std::uint32_t y = 0; y < 200; ++y)
                for (std::uint32_t x = 0; x < 200; ++x) {
                    std::array<double, 4> mean{};
                    for (std::uint32_t dy = 0; dy < 3; ++dy)
                        for (std::uint32_t dx = 0; dx < 3; ++dx) {
                            const auto p = pixel(b, x * 3 + dx, y * 3 + dy);
                            for (unsigned c = 0; c < 3; ++c) mean[c] += p[c] / 9;
                        }
                    const auto q = pixel(a, x, y);
                    double here = 0;
                    for (unsigned c = 0; c < 3; ++c) {
                        total += std::fabs(mean[c] - q[c]) / (200.0 * 200.0 * 3.0);
                        here = std::max(here, std::fabs(mean[c] - q[c]));
                    }
                    if (here > 0.1) apart += 1.0 / (200.0 * 200.0);
                }
            // (the mean over the whole, and the pixels apart past a tenth: the disc's edge, a pixel or so of it)
            QVERIFY2(total < bound * 0.25, qPrintable(QStringLiteral("%1: mean %2").arg(QString::fromStdString(given.dump())).arg(total)));
            QVERIFY2(apart < 0.03, qPrintable(QStringLiteral("%1: apart %2").arg(QString::fromStdString(given.dump())).arg(apart)));
        }
    }

    // --- PC4's new ops: frames until their groups ----------------------------------------------------------------------------

    void newOpsAreFramesForNow() {
        const auto doc = precise_page(8, 8, pattern(8, 8), false);
        QVERIFY(refusal(doc, Json{{"op", "render_picture"}, {"page", 1}, {"kind", "tree"}}).starts_with("not_yet_ported"));
        QVERIFY(refusal(doc, Json{{"op", "vanishing_point"}, {"page", 1}}).starts_with("not_yet_ported"));
        // a kind not in the table yet: an unknown filter (D5's)
        QVERIFY(!refusal(doc, Json{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "camera_raw"}}).empty());
    }
};

QTEST_GUILESS_MAIN(TestFx)
#include "test_fx.moc"
