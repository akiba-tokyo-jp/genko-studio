// PC4 節 D5 (SPEC.md COLOR-08「フィルター」: Camera Raw フィルター) without Python — PhotoCraft's own tests of it
// (algo/src/camera_raw.rs: the defaults the identity, the basic panel moving the right way, the curves, the colour mixer,
// grading, detail and effects, float documents keeping values past 1; engine/src/lens_cmds.rs: on 8, 16 and 32 bits, inside
// a selection, as a smart filter) on Genko's surfaces and ops; each stage changing the picture alone; the same bytes whatever
// the tiling and the threads; a page of inks worked on the colours it means (its inks as they were at the defaults, a grey
// on the plates' rules); the same look at 200 and 600 dpi; the frame what shows of the layer; its planes held in the imaging
// budget and its work stopped when asked; the settings' table (the point curves, the wheels, the catalogue).

#include <QtTest/QtTest>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/filter_kinds.hpp"
#include "core/ink.hpp"
#include "core/smart.hpp"
#include "render/fx.hpp"
#include "render/fx_camera_raw.hpp"
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

constexpr double kInch = 25.4;
using Px = std::array<float, 4>;

// camera_raw.rs's test picture: a smooth ramp of colours.
std::vector<Px> ramp(std::uint32_t w, std::uint32_t h) {
    std::vector<Px> out;
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const float fx = static_cast<float>(x) / static_cast<float>(w), fy = static_cast<float>(y) / static_cast<float>(h);
            out.push_back({0.2f + 0.6f * fx, 0.3f + 0.4f * fy, 0.5f - 0.3f * fx * fy, 1.0f});
        }
    return out;
}

std::vector<Px> flat(std::uint32_t w, std::uint32_t h, const Px& colour) { return std::vector<Px>(std::size_t(w) * h, colour); }

// A grey picture with a pixel's worth of noise (a hash of its place).
std::vector<Px> noisy(std::uint32_t w, std::uint32_t h) {
    std::vector<Px> out;
    for (std::uint32_t i = 0; i < w * h; ++i) {
        std::uint32_t z = i * 2654435761u;
        z ^= z >> 15;
        z *= 2246822519u;
        z ^= z >> 13;
        const float n = static_cast<float>(z % 1000) / 1000.0f * 0.2f;
        out.push_back({0.4f + n, 0.4f + n, 0.4f + n, 1.0f});
    }
    return out;
}

std::string precise(std::uint32_t w, std::uint32_t h, const std::vector<Px>& px, bool f32) {
    return core::encode_color_pixels(w, h, f32 ? "f32" : "u16", [&](std::size_t i) { return std::array<double, 4>{px[i][0], px[i][1], px[i][2], px[i][3]}; });
}

render::Image eight_of(std::uint32_t w, std::uint32_t h, const std::vector<Px>& px) {
    std::string bytes;
    for (const Px& p : px)
        for (const float v : p) bytes.push_back(static_cast<char>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)));
    return render::Image::frombytes("RGBA", render::Size{static_cast<int>(w), static_cast<int>(h)}, bytes);
}

const render::fx::Kernel& kernel() {
    const render::fx::Kernel* k = render::fx::kernel("camera_raw");
    if (k == nullptr) throw core::Error("value", "no kernel camera_raw");
    return *k;
}

Json normal(const Json& given) { return core::normalize_filter("camera_raw", given, nullptr); }

std::string run_precise(const std::string& raster, const Json& given, render::fx::Tiling tiling = {}, double s = 1, const std::string* cover = nullptr) {
    const auto surface = render::fx::precise_surface(raster);
    const Rect grid{0, 0, int(surface->width()), int(surface->height())};
    render::fx::apply(*surface, kernel(), normal(given), core::FilterInks::Meaning, grid, cover, s, nullptr, tiling);
    return render::fx::take_bytes(*surface);
}

std::string run_eight(const render::Image& picture, const Json& given, double s = 1, render::fx::Tiling tiling = {}) {
    const auto surface = render::fx::eight_surface(picture);
    render::fx::apply(*surface, kernel(), normal(given), core::FilterInks::Meaning, Rect{0, 0, picture.width(), picture.height()}, nullptr, s, nullptr, tiling);
    return render::fx::take_image(*surface).tobytes();
}

// The pixels of a precise raster as floats.
std::vector<Px> pixels(const std::string& raster) {
    const core::ColorRasterView view(raster);
    std::vector<Px> out;
    for (std::size_t i = 0; i < std::size_t(view.width()) * view.height(); ++i) {
        const auto p = view.pixel(i);
        out.push_back({static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2]), static_cast<float>(p[3])});
    }
    return out;
}

// The picture `given` makes of `px` (w × h) on a 16-bit precise layer (upstream's develop at float = false).
std::vector<Px> developed(std::uint32_t w, std::uint32_t h, const std::vector<Px>& px, const Json& given, bool f32 = false) {
    return pixels(run_precise(precise(w, h, px, f32), given));
}

float mean(const std::vector<Px>& px, std::size_t c) {
    double sum = 0;
    for (const Px& p : px) sum += p[c];
    return static_cast<float>(sum / static_cast<double>(px.size()));
}

float variance(const std::vector<Px>& px) {
    const float m = mean(px, 1);
    double sum = 0;
    for (const Px& p : px) sum += (p[1] - m) * (p[1] - m);
    return static_cast<float>(sum / static_cast<double>(px.size()));
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

core::Document blank(double w_mm, double h_mm) {
    auto doc = core::new_episode("Camera Raw", core::Num(1), 1, core::PageSpec::custom(w_mm, h_mm, w_mm - 2, h_mm - 2, 1, 0.5, 0.5, 0.5, 0.5, 200, "color"));
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
    auto doc = blank(w * kInch / 200.0, h * kInch / 200.0);
    core::Layer layer;
    layer.id = "paint";
    layer.kind = core::LayerKind::Raster;
    layer.panel_clip = false;
    render::raster::save_raster(doc.page(0), layer, picture);
    doc.edit_page(0).layers.push_back(layer);
    return doc;
}

render::Image picture_of(const core::Document& doc) { return render::raster::layer_pixels(doc.page(0), layer_of(doc, "paint")).convert("RGBA"); }

// A precise page of w × h pixels at 200 dpi with one paint layer "paint".
core::Document precise_page(std::uint32_t w, std::uint32_t h, const std::vector<Px>& px, bool f32) {
    auto doc = blank(w * kInch / 200.0, h * kInch / 200.0);
    Json values = Json::array();
    for (const auto& p : px)
        for (const float v : p) values.push_back(f32 ? Json(static_cast<double>(v)) : Json(static_cast<std::int64_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 65535))));
    doc = core::CommandBus().apply(doc, Json::array({Json{{"op", "put_color_raster"}, {"page", 1}, {"width", w}, {"height", h}, {"precision", f32 ? "f32" : "u16"},
                                                          {"pixels", values}}}),
                                   core::Actor("human:test"))
              .doc;
    doc.edit_page(0).layers[0].id = "paint";
    return doc;
}

// A page of inks (16 bits; `mode` set_color_mode's) of side × side with a paint layer "pic": `ink(x, y, c)` its plates.
core::Document ink_page(const Json& mode, std::uint32_t side, const std::function<double(std::uint32_t, std::uint32_t, unsigned)>& ink) {
    Json set{{"op", "set_color_mode"}, {"depth", 16}};
    set.update(mode);
    auto doc = apply_ops(blank(side * kInch / 200.0, side * kInch / 200.0), Json::array({set, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pic"}}})).doc;
    const core::PageColor& color = *doc.page(0).color;
    core::InkPixels p = core::blank_ink_pixels(color.space, color.depth, side, side, 0);
    for (std::uint32_t y = 0; y < side; ++y)
        for (std::uint32_t x = 0; x < side; ++x) {
            for (unsigned c = 0; c < p.colours(); ++c) p.set(std::size_t(y) * side + x, c, ink(x, y, c));
            p.set(std::size_t(y) * side + x, p.colours(), 1.0);
        }
    doc.edit_page(0).layers.back().ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(p));
    return doc;
}

// Every stage on (the tests of the tiling, the depths and the resolutions).
Json everything() {
    return Json{{"temperature", 20},
                {"tint", -10},
                {"exposure", 0.4},
                {"contrast", 30},
                {"highlights", -40},
                {"shadows", 50},
                {"whites", 20},
                {"blacks", -20},
                {"texture", 60},
                {"clarity", 40},
                {"dehaze", 30},
                {"vibrance", 30},
                {"saturation", 10},
                {"curve_shadows", 20},
                {"point_curve", Json::array({Json::array({0, 10}), Json::array({128, 140}), Json::array({255, 250})})},
                {"point_curve_blue", Json::array({Json::array({0, 0}), Json::array({255, 230})})},
                {"hsl_hue", Json::array({20, 0, 0, -10, 0, 0, 0, 0})},
                {"hsl_sat", Json::array({0, 30, 0, 0, 0, -20, 0, 0})},
                {"grade_shadows", Json{{"hue", 220}, {"sat", 40}, {"lum", 10}}},
                {"grade_highlights", Json{{"hue", 40}, {"sat", 30}, {"lum", 0}}},
                {"sharpen_amount", 80},
                {"sharpen_masking", 30},
                {"noise_luminance", 40},
                {"noise_color", 50},
                {"grain_amount", 40},
                {"seed", 5},
                {"vignette_amount", -40},
                {"vignette_roundness", 30}};
}

}  // namespace

class TestFxCameraRaw : public QObject {
    Q_OBJECT

private slots:
    // --- camera_raw.rs's tests --------------------------------------------------------------------------------------------

    // defaults_are_identity: at its defaults nothing changes — the same bytes on 8 bits, 16 and 32 (HDR too), and a page of
    // inks' plates as they were.
    void theDefaultsAreTheIdentity() {
        const auto px = ramp(40, 30);
        QCOMPARE(run_precise(precise(40, 30, px, false), Json::object()), precise(40, 30, px, false));
        auto hdr = px;
        hdr[3] = {2.5f, 1.25f, -0.1f, 1.0f};
        QCOMPARE(run_precise(precise(40, 30, hdr, true), Json::object()), precise(40, 30, hdr, true));
        const render::Image eight = eight_of(40, 30, px);
        QCOMPARE(run_eight(eight, Json::object()), eight.tobytes());
        QVERIFY(render::camera_raw::settings_of(normal(Json::object())).identity());
        for (const Json& mode : {Json{{"mode", "cmyk"}, {"profile", "coated-cmyk"}}, Json{{"mode", "lab"}}, Json{{"mode", "gray"}}}) {
            const auto doc = ink_page(mode, 24, [](std::uint32_t x, std::uint32_t y, unsigned c) { return ((x * 7 + y * 3 + c * 11) % 64) / 63.0; });
            const auto done = apply_ops(doc, Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", "pic"}, {"kind", "camera_raw"}}})).doc;
            QCOMPARE(*layer_of(done, "pic").ink_raster, *layer_of(doc, "pic").ink_raster);
        }
    }

    // basic_panel_moves_the_right_way
    void theBasicPanelMovesTheRightWay() {
        const std::uint32_t w = 64, h = 48;
        const auto base = ramp(w, h);
        const auto run = [&](const Json& given) { return developed(w, h, base, given); };
        const auto warm = run(Json{{"temperature", 50}});
        QVERIFY(mean(warm, 0) > mean(base, 0) && mean(warm, 2) < mean(base, 2));
        QVERIFY(mean(run(Json{{"exposure", 1.0}}), 1) > mean(base, 1) + 0.1f);
        QVERIFY(run(Json{{"shadows", 100}})[0][1] > base[0][1]);
        QVERIFY(run(Json{{"highlights", -100}})[w * h - 1][0] < base[w * h - 1][0]);
        const auto con = run(Json{{"contrast", 100}});
        QVERIFY(con[0][1] < base[0][1] && con[w * h - 1][0] > base[w * h - 1][0]);
        for (const Px& q : run(Json{{"saturation", -100}})) QVERIFY(std::fabs(q[0] - q[1]) < 1e-4f && std::fabs(q[1] - q[2]) < 1e-4f);
        const auto spread = [](const Px& q) { return std::max({q[0], q[1], q[2]}) - std::min({q[0], q[1], q[2]}); };
        QVERIFY(spread(run(Json{{"vibrance", 100}})[w * h / 2]) > spread(base[w * h / 2]));
        for (const Json& given : {Json{{"clarity", 80}}, Json{{"texture", 80}}, Json{{"dehaze", 60}}, Json{{"dehaze", -60}}, Json{{"whites", 50}, {"blacks", -50}}}) {
            const auto out = run(given);
            for (const Px& q : out)
                for (std::size_t c = 0; c < 3; ++c) QVERIFY2(std::isfinite(q[c]) && q[c] >= 0 && q[c] <= 1, given.dump().c_str());
            QVERIFY2(out != developed(w, h, base, Json::object()), given.dump().c_str());
        }
    }

    // curves_hsl_grading_detail_effects
    void curvesMixerGradingDetailAndEffects() {
        namespace cr = render::camera_raw;
        const auto lut = cr::curve_lut({{0, 0}, {64, 40}, {192, 220}, {255, 255}}, 256);
        QVERIFY(std::fabs(lut[64] - 40.0f / 255.0f) < 0.01f && std::fabs(lut[192] - 220.0f / 255.0f) < 0.01f);
        for (std::size_t i = 1; i < lut.size(); ++i) QVERIFY(lut[i] >= lut[i - 1]);
        for (int hue = 0; hue < 360; hue += 7) {
            const auto weights = cr::band_weights(static_cast<float>(hue));
            float sum = 0;
            for (const float v : weights) sum += v;
            QVERIFY2(std::fabs(sum - 1.0f) < 1e-4f, qPrintable(QString::number(hue)));
        }
        const std::uint32_t w = 48, h = 32;
        const Json reds{{"hsl_sat", Json::array({-100, 0, 0, 0, 0, 0, 0, 0})}};
        const auto red = developed(w, h, flat(w, h, {0.8f, 0.2f, 0.2f, 1}), reds);
        QVERIFY2(red[0][0] - red[0][1] < 0.1f, "the reds desaturated");
        const auto blue = developed(w, h, flat(w, h, {0.2f, 0.2f, 0.8f, 1}), reds);
        QVERIFY2(std::fabs(blue[0][2] - 0.8f) < 1e-4f, "the blues untouched");
        // grading: blue shadows tint the dark pixel, the bright one nearly alone
        const auto graded = developed(2, 1, {{0.1f, 0.1f, 0.1f, 1}, {0.95f, 0.95f, 0.95f, 1}}, Json{{"grade_shadows", Json{{"hue", 220}, {"sat", 80}, {"lum", 0}}}});
        QVERIFY(graded[0][2] > graded[0][0] + 0.05f && std::fabs(graded[1][2] - graded[1][0]) < 0.02f);
        // the point curve and the parametric one
        for (const Px& q : developed(w, h, ramp(w, h), Json{{"point_curve", Json::array({Json::array({0, 30}), Json::array({255, 255})})}, {"curve_shadows", 50}}))
            QVERIFY(q[1] >= 30.0f / 255.0f - 1e-4f);
        // noise reduction lessens the variance, sharpening adds to it, grain is the same each time
        const auto noise = noisy(w, h);
        QVERIFY(variance(developed(w, h, noise, Json{{"noise_luminance", 80}})) < variance(noise) * 0.5f);
        QVERIFY(variance(developed(w, h, noise, Json{{"sharpen_amount", 100}})) > variance(noise));
        const Json grain{{"grain_amount", 50}, {"seed", 4}};
        QCOMPARE(developed(w, h, ramp(w, h), grain), developed(w, h, ramp(w, h), grain));
        QVERIFY(developed(w, h, ramp(w, h), grain) != developed(w, h, ramp(w, h), Json::object()));
        QVERIFY(developed(w, h, ramp(w, h), grain) != developed(w, h, ramp(w, h), Json{{"grain_amount", 50}, {"seed", 5}}));
        // the post-crop vignette darkens the corners, leaves the centre
        const auto v = developed(w, h, flat(w, h, {0.5f, 0.5f, 0.5f, 1}), Json{{"vignette_amount", -80}});
        QVERIFY2(v[0][0] < 0.35f && std::fabs(v[(h / 2) * w + w / 2][0] - 0.5f) < 0.01f, qPrintable(QString::number(v[0][0])));
    }

    // float_documents_keep_overrange: a 32-bit precise layer keeps values past 1; 16 bits are within 0..1.
    void floatDocumentsKeepValuesPastOne() {
        const auto hdr = flat(4, 4, {2.0f, 1.5f, 0.5f, 1});
        QVERIFY(developed(4, 4, hdr, Json{{"exposure", 0.5}}, true)[0][0] > 2.0f);
        QVERIFY(developed(4, 4, flat(4, 4, {0.9f, 0.8f, 0.5f, 1}), Json{{"exposure", 2}})[0][0] <= 1.0f);
    }

    // --- Genko's ------------------------------------------------------------------------------------------------------------

    // Each stage alone changes the picture (white balance, exposure, tone, texture, clarity, haze, presence, curves, the
    // colour mixer, grading, sharpening, noise reduction, grain, the vignette of each style).
    void eachStageChangesThePicture() {
        const std::uint32_t w = 48, h = 36;
        const auto px = ramp(w, h);
        const auto before = developed(w, h, px, Json::object());
        for (const Json& given :
             {Json{{"temperature", -40}}, Json{{"tint", 40}}, Json{{"exposure", -0.7}}, Json{{"contrast", -50}}, Json{{"highlights", 60}}, Json{{"shadows", -60}},
              Json{{"whites", 60}}, Json{{"blacks", 60}}, Json{{"texture", -70}}, Json{{"clarity", -70}}, Json{{"dehaze", 40}}, Json{{"vibrance", -50}},
              Json{{"saturation", 40}}, Json{{"curve_highlights", -60}}, Json{{"curve_lights", 50}}, Json{{"curve_darks", 50}},
              Json{{"point_curve_red", Json::array({Json::array({0, 20}), Json::array({255, 255})})}},
              Json{{"point_curve_green", Json::array({Json::array({0, 0}), Json::array({255, 200})})}}, Json{{"hsl_hue", Json::array({0, 60, 60, 0, 0, 0, 0, 0})}},
              Json{{"hsl_lum", Json::array({0, -60, -60, 0, 0, 0, 0, 0})}}, Json{{"grade_midtones", Json{{"hue", 120}, {"sat", 60}, {"lum", 20}}}},
              Json{{"grade_global", Json{{"hue", 300}, {"sat", 40}, {"lum", 0}}}}, Json{{"sharpen_amount", 120}, {"sharpen_radius", 2}},
              Json{{"noise_color", 80}}, Json{{"noise_luminance", 60}}, Json{{"grain_amount", 30}},
              Json{{"vignette_amount", 50}, {"vignette_style", "color_priority"}}, Json{{"vignette_amount", -50}, {"vignette_style", "paint_overlay"}},
              Json{{"vignette_amount", -50}, {"vignette_highlights", 80}, {"vignette_roundness", -60}}}) {
            const auto after = developed(w, h, px, given);
            QVERIFY2(after != before, given.dump().c_str());
            for (const Px& q : after)
                for (std::size_t c = 0; c < 4; ++c) QVERIFY2(std::isfinite(q[c]) && q[c] >= 0 && q[c] <= 1, given.dump().c_str());
            QCOMPARE(after[0][3], 1.0f);  // (the alpha as it was)
        }
    }

    // Worked tile by tile (each its part grown by what its local stages reach, their sums in whole numbers): the same bytes
    // whatever the tiling and the threads, at a finer grid's scale too; a clear pixel's values kept.
    void theSameBytesWhateverTheTiling() {
        for (const bool f32 : {false, true}) {
            auto px = ramp(70, 50);
            const auto noise = noisy(70, 50);
            for (std::size_t i = 0; i < px.size(); ++i) px[i][1] = (px[i][1] + noise[i][1]) / 2.0f;
            px[5] = {0.3f, 0.2f, 0.1f, 0.0f};  // (a clear pixel)
            const std::string raster = precise(70, 50, px, f32);
            for (const double s : {1.0, 1.7}) {
                const std::string a = run_precise(raster, everything(), {256, 1}, s);
                QVERIFY(a != raster);
                for (const render::fx::Tiling tiling : {render::fx::Tiling{7, 3}, render::fx::Tiling{13, 2}, render::fx::Tiling{5, 4}, render::fx::Tiling{64, 0}})
                    QVERIFY2(run_precise(raster, everything(), tiling, s) == a, qPrintable(QStringLiteral("tile %1, s %2, f32 %3").arg(tiling.tile).arg(s).arg(f32)));
                const core::ColorRasterView before(raster), after(a);
                QCOMPARE(after.pixel(5), before.pixel(5));
            }
        }
        const render::Image eight = eight_of(70, 50, ramp(70, 50));
        const std::string whole = run_eight(eight, everything(), 1, {256, 1});
        QCOMPARE(run_eight(eight, everything(), 1, {9, 4}), whole);
        // (an empty selection: nothing changes)
        const std::string raster = precise(40, 30, ramp(40, 30), false);
        const std::string none(40 * 30, '\0');
        QCOMPARE(run_precise(raster, everything(), {}, 1, &none), raster);
    }

    // lens_cmds.rs camera_raw_filter_on_layers_selection_and_smart_objects: through filter_raster on 8, 16 and 32 bits (the
    // book before it as it was), inside a selection, refused for a word as a number, its report; as a smart filter.
    void throughTheOpOnEveryDepthAndInsideASelection() {
        const auto px = ramp(120, 80);
        for (const core::Document& doc : {eight_page(120, 80, eight_of(120, 80, px)), precise_page(120, 80, px, false), precise_page(120, 80, px, true)}) {
            const Json op{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "camera_raw"}, {"exposure", 1.0}, {"vibrance", 30}, {"clarity", 20}};
            const core::ApplyResult done = apply_ops(doc, Json::array({op}));
            const core::Layer& before = layer_of(doc, "paint");
            const core::Layer& after = layer_of(done.doc, "paint");
            if (after.color_raster) {
                const core::ColorRasterView a(*before.color_raster), b(*after.color_raster);
                const std::size_t at = 40 * 120 + 60;
                QVERIFY2(b.pixel(at)[1] > a.pixel(at)[1] + 0.05, qPrintable(QString::number(b.pixel(at)[1])));
            } else {
                QVERIFY(picture_of(done.doc).getpixel(60, 40)[1] > picture_of(doc).getpixel(60, 40)[1] + 12);
            }
            QVERIFY(layer_of(doc, "paint").raster_png == before.raster_png);  // (the book before it as it was)
            const Json report = done.results.at(0);
            QCOMPARE(report["filter"]["kind"], Json("camera_raw"));
            QCOMPARE(report["filter"]["exposure"], Json(1.0));
            QCOMPARE(report["filter"]["grade_shadows"], (Json{{"hue", 0}, {"sat", 0}, {"lum", 0}}));
            QCOMPARE(report["filter"]["point_curve"], Json::array());
        }
        // inside a selection: darker inside, outside as it was
        const auto doc = eight_page(120, 80, eight_of(120, 80, px));
        const Json inside{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "camera_raw"}, {"exposure", -1.0},
                          {"area", Json{{"rect", Json::array({0, 0, 60 * kInch / 200.0, 80 * kInch / 200.0})}}}};
        const render::Image after = picture_of(apply_ops(doc, Json::array({inside})).doc);
        QVERIFY(after.getpixel(10, 40)[1] < picture_of(doc).getpixel(10, 40)[1]);
        QCOMPARE(after.getpixel(100, 40), picture_of(doc).getpixel(100, 40));
        // refused: a word for a number, a curve of one point, a wheel's setting it has not, a style it does not know
        for (const Json& wrong : {Json{{"exposure", "bright"}}, Json{{"point_curve", Json::array({Json::array({0, 0})})}},
                                  Json{{"grade_shadows", Json{{"hue", 10}, {"chroma", 2}}}}, Json{{"vignette_style", "vintage"}}, Json{{"hsl_hue", Json::array({1, 2})}}}) {
            Json op{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "camera_raw"}};
            op.update(wrong);
            QVERIFY2(!refusal(doc, op).empty(), wrong.dump().c_str());
        }
    }

    // As a smart filter (filters@1): its settings kept in their normal form, the smart object drawn with it, set again.
    void asASmartFilter() {
        const auto doc = apply_ops(eight_page(120, 80, eight_of(120, 80, ramp(120, 80))),
                                   Json::array({Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({"paint"})}, {"new_id", "so"}}}))
                             .doc;
        const auto done = apply_ops(doc, Json::array({Json{{"op", "smart_filter_add"}, {"page", 1}, {"id", "so"}, {"kind", "camera_raw"}, {"saturation", -100}}})).doc;
        const Json filters = (*layer_of(done, "so").smart)["filters"];
        QCOMPARE(filters.size(), std::size_t(1));
        QCOMPARE(filters[0]["kind"], Json("camera_raw"));
        QVERIFY(core::uses_filters(done));
        const render::Image drawn = render::layer_image(done.page(0), layer_of(done, "so"), 200, &done).convert("RGBA");
        const auto p = drawn.getpixel(60, 40);
        QVERIFY2(std::abs(p[0] - p[2]) <= 2, qPrintable(QStringLiteral("%1 %2 %3").arg(p[0]).arg(p[1]).arg(p[2])));
        const auto set = apply_ops(done, Json::array({Json{{"op", "smart_filter_set"}, {"page", 1}, {"id", "so"}, {"index", 0}, {"params", Json{{"saturation", 0}, {"exposure", -1}}}}}))
                             .doc;
        const render::Image darker = render::layer_image(set.page(0), layer_of(set, "so"), 200, &set).convert("RGBA");
        QVERIFY(darker.getpixel(60, 40)[1] < render::layer_image(doc.page(0), layer_of(doc, "so"), 200, &doc).convert("RGBA").getpixel(60, 40)[1]);
        QVERIFY(std::abs(darker.getpixel(60, 40)[0] - darker.getpixel(60, 40)[2]) > 2);  // (no longer grey)
    }

    // A page of inks is worked on the colours its inks mean, back by the page's rules (FilterInks::Meaning): CMYK lighter
    // with less ink, Lab and grey too; saturation −100 leaves colours whose meaning is grey.
    void pagesOfInksOnTheColoursTheyMean() {
        const std::vector<Json> modes{Json{{"mode", "cmyk"}, {"profile", "coated-cmyk"}}, Json{{"mode", "lab"}}, Json{{"mode", "gray"}}};
        for (const Json& mode : modes) {
            const auto doc = ink_page(mode, 24, [&mode](std::uint32_t x, std::uint32_t y, unsigned c) {
                if (mode["mode"] == Json("lab")) return c == 0 ? 0.45 : (c == 1 ? 0.6 + x * 0.004 : 0.4 + y * 0.004);
                if (mode["mode"] == Json("gray")) return 0.4 + x * 0.01;
                return c == 3 ? 0.1 : 0.2 + c * 0.15 + x * 0.005;
            });
            const core::PageColor& color = *doc.page(0).color;
            const render::ink::Rules rules(color);
            const auto meaning = [&](const core::Document& d, std::size_t i) {
                const core::InkPixels inks = core::decode_ink_raster(*layer_of(d, "pic").ink_raster);
                std::vector<double> v(inks.colours());
                for (unsigned c = 0; c < inks.colours(); ++c) v[c] = render::ink::decode_sample(inks.space, c, inks.samples[i * inks.channels() + c], inks.depth);
                std::array<double, 3> rgb{};
                rules.meaning(v.data(), rgb.data(), 1);
                return rgb;
            };
            const auto lighter = apply_ops(doc, Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", "pic"}, {"kind", "camera_raw"}, {"exposure", 1}}})).doc;
            const auto a = meaning(doc, 12 * 24 + 12), b = meaning(lighter, 12 * 24 + 12);
            QVERIFY2(b[0] + b[1] + b[2] > a[0] + a[1] + a[2] + 0.2, qPrintable(mode.dump().c_str()));
            if (mode["mode"] == Json("gray")) continue;
            const auto grey = apply_ops(doc, Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", "pic"}, {"kind", "camera_raw"}, {"saturation", -100}}})).doc;
            const auto g = meaning(grey, 12 * 24 + 12);
            QVERIFY2(std::max({g[0], g[1], g[2]}) - std::min({g[0], g[1], g[2]}) < 0.04, qPrintable(QStringLiteral("%1: %2 %3 %4").arg(QString::fromStdString(mode.dump())).arg(g[0]).arg(g[1]).arg(g[2])));
        }
    }

    // The same look at 200 and 600 dpi: a 600 dpi layer (s 3) worked and averaged down by 3 is near the 200 dpi one's.
    void theSameLookAt200And600Dpi() {
        const auto picture = [](std::uint32_t w, std::uint32_t h, std::uint32_t k) {
            std::vector<Px> out;
            for (std::uint32_t y = 0; y < h; ++y)
                for (std::uint32_t x = 0; x < w; ++x) {
                    const std::uint32_t cx = x / k, cy = y / k;
                    const bool line = (cx % 16) < 2 || (cy % 16) < 2;
                    const float fx = static_cast<float>(cx) / 120.0f;
                    out.push_back({line ? 0.9f : 0.2f + 0.5f * fx, line ? 0.8f : 0.3f, line ? 0.7f : 0.5f - 0.3f * fx, 1.0f});
                }
            return out;
        };
        Json given = everything();
        given.erase("grain_amount");  // (a grain's place: by the working grid — its value noise is of the working pixels)
        for (const Json& params : {given, Json{{"grain_amount", 60}, {"grain_size", 50}, {"seed", 3}}}) {
            const std::vector<Px> big = pixels(run_precise(precise(360, 240, picture(360, 240, 3), true), params, {}, 3.0));
            const std::vector<Px> small = pixels(run_precise(precise(120, 80, picture(120, 80, 1), true), params, {}, 1.0));
            double total = 0;
            for (std::uint32_t y = 0; y < 80; ++y)
                for (std::uint32_t x = 0; x < 120; ++x)
                    for (std::size_t c = 0; c < 3; ++c) {
                        double m = 0;
                        for (std::uint32_t dy = 0; dy < 3; ++dy)
                            for (std::uint32_t dx = 0; dx < 3; ++dx) m += big[std::size_t(y * 3 + dy) * 360 + x * 3 + dx][c] / 9.0;
                        total += std::fabs(m - small[std::size_t(y) * 120 + x][c]);
                    }
            QVERIFY2(total / (120.0 * 80.0 * 3.0) < 0.03, qPrintable(QStringLiteral("%1: %2").arg(QString::fromStdString(params.dump())).arg(total / 28800.0)));
        }
    }

    // The frame is what shows of the layer (Kernel::shown_frame): a photo on a part of the layer comes out the same wherever it
    // lies (周辺光量 about its centre); the rest of the layer as it was.
    void theFrameIsWhatShowsOfTheLayer() {
        const render::Image photo = eight_of(60, 40, ramp(60, 40));
        const auto layer_with = [&](int x, int y) {
            render::Image layer = render::Image::create("RGBA", render::Size{160, 120}, render::Ink{0, 0, 0, 0});
            layer.paste(photo, render::Box{x, y, x + 60, y + 40});
            return layer;
        };
        const auto part = [](const std::string& bytes, int x, int y) {
            std::string out;
            for (int r = 0; r < 40; ++r) out += bytes.substr((std::size_t(y + r) * 160 + std::size_t(x)) * 4, 60 * 4);
            return out;
        };
        const Json params{{"vignette_amount", -80}, {"clarity", 50}, {"dehaze", 30}};
        const std::string here = part(run_eight(layer_with(10, 10), params), 10, 10);
        QCOMPARE(part(run_eight(layer_with(90, 70), params), 90, 70), here);
        QVERIFY(here != part(layer_with(10, 10).tobytes(), 10, 10));
        // (the photo's corner darkened by the vignette, its centre near as it was)
        const std::string whole = run_eight(layer_with(10, 10), Json{{"vignette_amount", -80}});
        const auto at = [&whole](int x, int y) { return static_cast<unsigned char>(whole[(std::size_t(y) * 160 + std::size_t(x)) * 4 + 1]); };
        const render::Image before = layer_with(10, 10);
        QVERIFY(at(10, 10) < before.getpixel(10, 10)[1] - 20);
        QVERIFY(std::abs(int(at(40, 30)) - int(before.getpixel(40, 30)[1])) <= 2);
        QCOMPARE(at(150, 110), static_cast<unsigned char>(0));  // (clear: as it was)
    }

    // Its planes of the whole are held in the imaging budget (refused before they are had when it cannot hold them), and a
    // run stops when asked (render::Cancelled, the layer's bytes untouched).
    void heldInTheBudgetAndStopped() {
        const std::string raster = precise(300, 300, ramp(300, 300), false);
        {
            // (the haze's proxy — 300 × 300 — and 明瞭度's base counted: some 16 MB past the window's 1.4 MB)
            const render::ImageAllocationBudget budget{1ULL << 30};
            (void)run_precise(raster, Json{{"dehaze", 40}, {"clarity", 30}});
            QVERIFY2(budget.peak() > 15'000'000, qPrintable(QString::number(budget.peak())));
        }
        {
            const render::ImageAllocationBudget budget{2u * 1024 * 1024};
            bool refused = false;
            try {
                (void)run_precise(raster, Json{{"dehaze", 40}, {"clarity", 30}});
            } catch (const core::OpError& error) {
                refused = std::string(error.what()).find("too large") != std::string::npos;
            }
            QVERIFY(refused);
        }
        std::stop_source stop;
        stop.request_stop();
        const render::segment::StopScope scope(stop.get_token());
        QVERIFY_EXCEPTION_THROWN((void)run_precise(raster, Json{{"dehaze", 40}}), render::Cancelled);
    }

    // The settings' table: the defaults (the identity), each in its range, the point curves read (straight, sorted, half a
    // level apart, 2 to 19), the wheels, the catalogue's entry.
    void theSettingsTable() {
        const Json read = normal(Json::object());
        QCOMPARE(read["exposure"], Json(0));
        QCOMPARE(read["curve_splits"], (Json::array({25, 50, 75})));
        QCOMPARE(read["grade_blending"], Json(50));
        QCOMPARE(read["vignette_style"], Json("highlight_priority"));
        QCOMPARE(read["seed"], Json(0));
        for (const char* key : {"point_curve", "point_curve_red", "point_curve_green", "point_curve_blue"}) QCOMPARE(read[key], Json::array());
        QCOMPARE(normal(Json{{"exposure", 9}})["exposure"], Json(5));
        QCOMPARE(normal(Json{{"grade_midtones", Json{{"hue", 400}}}})["grade_midtones"], (Json{{"hue", 360}, {"sat", 0}, {"lum", 0}}));
        const Json curve = normal(Json{{"point_curve", Json::array({Json::array({255, 300}), Json::array({0, 10}), Json::array({0.2, 50}), Json::array({128, 100})})}});
        QCOMPARE(curve["point_curve"], (Json::array({Json::array({0, 10}), Json::array({128, 100}), Json::array({255, 255})})));
        Json many = Json::array();
        for (int i = 0; i < 20; ++i) many.push_back(Json::array({i * 12, i * 12}));
        QVERIFY_EXCEPTION_THROWN(normal(Json{{"point_curve", many}}), core::OpError);
        QVERIFY_EXCEPTION_THROWN(normal(Json{{"point_curve", Json::array({Json::array({0, 0}), Json::array({0.3, 9})})}}), core::OpError);
        QVERIFY_EXCEPTION_THROWN(normal(Json{{"point_curve", "s"}}), core::OpError);
        const core::FilterInfo* info = core::filter_info("camera_raw");
        QVERIFY(info != nullptr);
        QCOMPARE(info->upstream, std::string_view("filter.cameraRaw"));
        QCOMPARE(info->name, std::string_view("Camera Raw フィルター"));
        QVERIFY(info->submenu.empty());
        QVERIFY(info->inks == core::FilterInks::Meaning);
        QVERIFY(core::smart::filter_kind("camera_raw"));
        bool listed = false;
        const Json catalogue = core::filter_catalogue();
        for (const Json& kind : catalogue["kinds"]) listed = listed || (kind["kind"] == Json("camera_raw") && kind["inks"] == Json("meaning") && kind["submenu"] == Json(""));
        QVERIFY(listed);
        // a heavy one by its work at 600 dpi (every stage on): its window works it on a thread
        QVERIFY(render::fx::work("camera_raw", normal(everything()), 6071ull * 8598ull, 3.0) >= render::fx::kHeavyWork);
        QVERIFY(render::fx::work("camera_raw", normal(Json::object()), 200ull * 200ull, 1.0) < render::fx::kHeavyWork);
    }
};

QTEST_GUILESS_MAIN(TestFxCameraRaw)
#include "test_fx_camera_raw.moc"
