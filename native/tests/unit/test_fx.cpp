// PC4 節 D (SPEC.md COLOR-08「フィルター」, design 4.1-4.3): the engine of フィルター's kinds (render/fx) without Python —
// PhotoCraft's own tests of its filter core (algo/src/tests.rs: every filter tile independent 79, an empty selection
// changes nothing 91, half a selection mixes halfway 104, a tiny Gaussian the identity, flat colour and mass kept 120,
// 16-bit precision 135, HDR kept 153, the Gaussian's sigma its radius 160, the canvas edge repeated 432) on Genko's
// surfaces; then the same bytes whatever the tiling and the threads, the three ways (8 bits, precise u16 and f32, CMYK,
// Lab, grey and multichannel pages through filter_raster), the ways a page of inks is worked (each plate, its lightness,
// the colours it means), the same look at 200 and 600 dpi, the imaging budget refusing a window it cannot hold, a run
// stopped and its progress, a heavy filter told, a preview's copy at the screen's resolution, and PC4's new ops' frames.

#include <QtTest/QtTest>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <map>
#include <stop_token>
#include <string>
#include <vector>

#include "core/adjust.hpp"
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/filter_kinds.hpp"
#include "core/ink.hpp"
#include "core/smart.hpp"
#include "render/fx.hpp"
#include "render/image.hpp"
#include "render/ink.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
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

// A page of inks (`mode` cmyk, lab, gray or multichannel, 16 bits) with a paint layer "pic" of side × side pixels over the page
// (1 inch): `ink(x, y, c)` its values.
core::Document ink_page(const std::string& mode, const std::function<double(std::uint32_t, std::uint32_t, unsigned)>& ink, std::uint32_t side = 64) {
    Json set{{"op", "set_color_mode"}, {"mode", mode}, {"depth", mode == "multichannel" ? 8 : 16}};
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
        // (a kind whose defaults change nothing or take long on the test picture: its settings here — each group adds its own)
        const std::map<std::string, Json> kTestParams{{"blur_gaussian", Json{{"radius", 2.5}}}, {"blur_box", Json{{"radius", 2}}},
                                                      {"filter_gallery", Json{{"effects", Json::array({Json{{"filter", "cutout"}}})}}}};
        QVERIFY(!core::filter_infos().empty());
        for (const core::FilterInfo& info : core::filter_infos()) {
            const std::string kind(info.kind);
            QVERIFY2(render::fx::kernel(kind) != nullptr, kind.c_str());
            const Json params = kTestParams.contains(kind) ? kTestParams.at(kind) : core::normalize_filter(kind, Json::object(), nullptr);
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

    // --- PC4's new ops: frames until their groups ----------------------------------------------------------------------------

    void newOpsAreFramesForNow() {
        const auto doc = precise_page(8, 8, pattern(8, 8), false);
        QVERIFY(refusal(doc, Json{{"op", "render_picture"}, {"page", 1}, {"kind", "tree"}}).starts_with("not_yet_ported"));
        QVERIFY(refusal(doc, Json{{"op", "vanishing_point"}, {"page", 1}}).starts_with("not_yet_ported"));
        // a kind not in the table yet: an unknown filter
        QVERIFY(!refusal(doc, Json{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "blur_motion"}}).empty());
    }
};

QTEST_GUILESS_MAIN(TestFx)
#include "test_fx.moc"
