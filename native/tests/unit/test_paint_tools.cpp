// PC4 E5 (SPEC.md COLOR-09「画素を変える筆」): the brushes that change pixels as PhotoCraft v0.2.0 checks them —
// algo/src/retouch.rs's tests (the toning and focus kernels), engine/src/retouch_cmds/tests.rs (Dodge and Burn brighten and
// darken their tones, Sponge moves the saturation, Blur lowers a checker's variance and Sharpen raises it — Protect Detail
// within the local range —, Smudge carries colour and finger painting starts with the 描画色), eraser_cmds/tests.rs (the
// Background Eraser's three samplings and limits, Protect Foreground, its refusals) and brush_cmds/tests.rs (the Mixer
// Brush's reservoir, drying and picking up, Color Replacement's four modes) — on every kind of layer (8 bits; 16 and 32
// bits; the inks of CMYK, Lab and grey pages); only inside the selection; a protected transparency kept; an ink layer's
// pixels the brush leaves keep their inks; a 32-bit page's bright pixels left by Dodge, Burn and Sponge; a dab brush's tip;
// the same input the same bytes; the work refused before it is laid; 色混ぜ's precise and ink values (design 0.4); and a
// layer's mask painted through the lending.

#include <QtTest>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/ink.hpp"
#include "render/image.hpp"
#include "render/image_ops.hpp"
#include "render/ink_edit.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/paint_tools/tone.hpp"
#include "render/png.hpp"
#include "render/raster.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
using Rgba = render::image_ops::Rgba;
namespace pt = render::paint_tools;

namespace {

constexpr double kInch = 25.4;  // (200 px at the working 200 dpi)
constexpr int kSide = 200;

core::ApplyResult run_ops(const core::Document& doc, const Json& ops) {
    return core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor("human:test"));
}

core::Document run(const core::Document& doc, const Json& op) { return run_ops(doc, Json::array({op})).doc; }

std::string refusal(const core::Document& doc, const Json& op) {
    try {
        run_ops(doc, Json::array({op}));
    } catch (const core::ApplyError& error) {
        return error.code() + ": " + error.what();
    }
    return {};
}

double mm(double px) { return (px + 0.5) * kInch / kSide; }
double len(double px) { return px * kInch / kSide; }

using Colour = std::array<double, 4>;

render::Image picture(const std::function<Colour(int, int)>& at) {
    std::string bytes;
    for (int y = 0; y < kSide; ++y)
        for (int x = 0; x < kSide; ++x)
            for (const double v : at(x, y)) bytes.push_back(static_cast<char>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)));
    return render::Image::frombytes("RGBA", render::Size{kSide, kSide}, bytes);
}

core::Layer paint_layer(core::Page& page, const std::string& id, const std::function<Colour(int, int)>& at) {
    core::Layer layer;
    layer.id = id;
    layer.role = core::LayerRole::User;
    layer.kind = core::LayerKind::Raster;
    layer.panel_clip = false;
    render::raster::save_raster(page, layer, picture(at));
    return layer;
}

// A 1-inch page of one 8-bit paint layer "paint" drawn by `at` (and, with `top`, a clear one "top" over it).
core::Document book(const std::function<Colour(int, int)>& at, bool top = false) {
    auto doc = core::new_episode("画素の道具", core::Num(1), 1, core::PageSpec::custom(kInch, kInch, 20, 20, 1, 2, 2, 2, 2, 200, "color"));
    auto& p = doc.edit_page(0);
    p.numero = false;
    p.frames.clear();
    p.layers.clear();
    p.layers.push_back(paint_layer(p, "paint", at));
    if (top) p.layers.push_back(paint_layer(p, "top", [](int, int) { return Colour{0, 0, 0, 0}; }));
    return doc;
}

core::Document as_kind(const core::Document& doc, const std::string& kind) {
    if (kind == "8") return doc;
    if (kind == "16" || kind == "32") return run(doc, Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", kind == "16" ? 16 : 32}});
    if (kind == "cmyk") return run(doc, Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}, {"depth", 16}});
    if (kind == "lab") return run(doc, Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "lab"}, {"depth", 16}});
    return run(doc, Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "gray"}});
}

const std::vector<std::string>& kinds() {
    static const std::vector<std::string> all{"8", "16", "32", "cmyk", "lab", "gray"};
    return all;
}

const core::Layer& layer_of(const core::Document& doc, const std::string& id) {
    for (const core::Layer& layer : doc.page(0).layers)
        if (layer.id == id) return layer;
    throw std::runtime_error("no layer " + id);
}

// The layer's pixels as straight RGBA (an ink layer's: the colours its inks mean), on the 200 grid.
struct Shown {
    int width = 0, height = 0;
    std::vector<Rgba> rgba;
    Rgba at(int x, int y) const {
        const int px = static_cast<int>(std::floor((x + 0.5) * width / kSide)), py = static_cast<int>(std::floor((y + 0.5) * height / kSide));
        return rgba[static_cast<std::size_t>(py) * width + static_cast<std::size_t>(px)];
    }
};

Shown shown(const core::Document& doc, const std::string& id = "paint") {
    const render::image_ops::Pixels p = render::image_ops::shown_layer_pixels(doc, doc.page(0), layer_of(doc, id));
    return Shown{static_cast<int>(p.width()), static_cast<int>(p.height()), render::image_ops::rgba_of(p)};
}

Rgba pixel(const core::Document& doc, int x, int y, const std::string& id = "paint") { return shown(doc, id).at(x, y); }

QString text(const Rgba& p) { return QStringLiteral("(%1, %2, %3, %4)").arg(p[0]).arg(p[1]).arg(p[2]).arg(p[3]); }

Json stroke(const char* op, const std::vector<std::pair<double, double>>& points, double width_px, const std::string& layer = "paint") {
    Json pts = Json::array();
    for (const auto& [x, y] : points) pts.push_back(Json::array({mm(x), mm(y)}));
    return Json{{"op", op}, {"page", 1}, {"layer_id", layer}, {"points", pts}, {"width_mm", len(width_px)}, {"hardness", 100}};
}

// The layer's own bytes (its picture, precise samples or inks).
std::string bytes_of(const core::Document& doc, const std::string& id = "paint") {
    const core::Layer& l = layer_of(doc, id);
    if (l.color_raster) return *l.color_raster;
    if (l.ink_raster) return *l.ink_raster;
    return l.raster_png ? *l.raster_png : std::string();
}

float saturation(const Rgba& p) { return pt::saturation({p[0], p[1], p[2]}); }
float lum(const Rgba& p) { return 0.3f * p[0] + 0.59f * p[1] + 0.11f * p[2]; }  // (W3C's, as the non-separable blends take it)

double variance(const Shown& s, int x0, int y0, int x1, int y1) {
    std::vector<double> v;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) v.push_back(s.at(x, y)[0]);
    double m = 0;
    for (const double a : v) m += a / v.size();
    double var = 0;
    for (const double a : v) var += (a - m) * (a - m) / v.size();
    return var;
}

}  // namespace

class TestPaintTools : public QObject {
    Q_OBJECT
private slots:
    // algo/src/retouch.rs's tests: the kernels.
    void theKernels() {
        const auto gain = [](float v) { return pt::dodge_burn({v, v, v}, 0.5f, pt::ToneRange::Midtones, false, true)[0] - v; };
        QVERIFY(gain(0.5f) > 0.1f);
        QVERIFY(gain(0.5f) > 2.0f * gain(0.1f) && gain(0.5f) > 2.0f * gain(0.95f));
        QCOMPARE(pt::dodge_burn({0, 0, 0}, 0.5f, pt::ToneRange::Midtones, false, true), (std::array<float, 3>{0, 0, 0}));
        QVERIFY(pt::dodge_burn({0.5f, 0.5f, 0.5f}, 0.5f, pt::ToneRange::Midtones, true, true)[0] < 0.45f);
        const auto d = [](float v, pt::ToneRange r) { return pt::tone_curve(v, 0.5f, r, false) - v; };
        QVERIFY(d(0.1f, pt::ToneRange::Shadows) > d(0.9f, pt::ToneRange::Shadows));
        QVERIFY(d(0.9f, pt::ToneRange::Highlights) > d(0.1f, pt::ToneRange::Highlights));
        const auto b = [](float v, pt::ToneRange r) { return v - pt::tone_curve(v, 0.5f, r, true); };
        QVERIFY(b(0.9f, pt::ToneRange::Highlights) > b(0.2f, pt::ToneRange::Highlights));
        QVERIFY(b(0.3f, pt::ToneRange::Shadows) / 0.3f > b(0.9f, pt::ToneRange::Shadows) / 0.9f);
        const std::array<float, 3> c{0.6f, 0.3f, 0.2f};
        const std::array<float, 3> p = pt::dodge_burn(c, 0.8f, pt::ToneRange::Midtones, false, true);
        QVERIFY(std::all_of(p.begin(), p.end(), [](float v) { return v >= 0.0f && v <= 1.0f; }));
        QVERIFY(std::fabs(p[0] / p[1] - c[0] / c[1]) < 0.05f);  // (the hue kept)
        const std::array<float, 3> s{0.8f, 0.4f, 0.3f};
        const std::array<float, 3> less = pt::sponge(s, 0.5f, false, false), more = pt::sponge(s, 0.5f, true, false);
        QVERIFY(pt::saturation(less) < pt::saturation(s) && std::fabs(pt::luma(less) - pt::luma(s)) < 1e-4f);
        QVERIFY(pt::saturation(more) > pt::saturation(s) && std::all_of(more.begin(), more.end(), [](float v) { return v >= 0.0f && v <= 1.0f; }));
        const std::array<float, 3> vib = pt::sponge({1.0f, 0.1f, 0.1f}, 0.5f, true, true), nvib = pt::sponge({1.0f, 0.1f, 0.1f}, 0.5f, true, false);
        QVERIFY(std::fabs(vib[1] - 0.1f) <= std::fabs(nvib[1] - 0.1f));
        QCOMPARE(pt::sponge({0.5f, 0.5f, 0.5f}, 1.0f, false, false), (std::array<float, 3>{0.5f, 0.5f, 0.5f}));
        // blur lowers a checker's variance, sharpen raises a gradient's; protect detail stays in the local range
        const render::Size grid{16, 16};
        std::vector<float> checker(256), smooth(256);
        for (int i = 0; i < 256; ++i) {
            checker[std::size_t(i)] = (i % 16 + i / 16) % 2 == 0 ? 0.2f : 0.8f;
            smooth[std::size_t(i)] = 0.3f + 0.03f * static_cast<float>(i % 16) + (i % 16 == 8 ? 0.2f : 0.0f);
        }
        const auto reader = [](const std::vector<float>& v) { return [&v](int x, int y) { return v.data() + std::size_t(y) * 16 + std::size_t(x); }; };
        const render::Box win{4, 4, 12, 12};
        const auto var = [](const std::vector<float>& v) {
            float m = 0;
            for (const float a : v) m += a / static_cast<float>(v.size());
            float out = 0;
            for (const float a : v) out += (a - m) * (a - m) / static_cast<float>(v.size());
            return out;
        };
        std::vector<float> orig, so;
        for (int y = 4; y < 12; ++y)
            for (int x = 4; x < 12; ++x) {
                orig.push_back(checker[std::size_t(y) * 16 + std::size_t(x)]);
                so.push_back(smooth[std::size_t(y) * 16 + std::size_t(x)]);
            }
        QVERIFY(var(pt::local_blur(reader(checker), grid, 1, -1, win, 1.5f)) < var(orig) * 0.2f);
        QVERIFY(var(pt::local_sharpen(reader(smooth), grid, 1, -1, win, 1.0f, false)) > var(so));
        const float hi = *std::max_element(smooth.begin(), smooth.end());
        for (const float v : pt::local_sharpen(reader(smooth), grid, 1, -1, win, 4.0f, true)) QVERIFY(v <= hi + 1e-6f);
        // a clear neighbour lends no colour: red stays red beside clear green
        std::vector<float> rg;
        for (int x = 0; x < 8; ++x) {
            const std::array<float, 4> v = x < 4 ? std::array<float, 4>{1, 0, 0, 1} : std::array<float, 4>{0, 1, 0, 0};
            rg.insert(rg.end(), v.begin(), v.end());
        }
        const std::vector<float> blurred = pt::local_blur([&rg](int x, int) { return rg.data() + std::size_t(x) * 4; }, render::Size{8, 1}, 4, 3, render::Box{2, 0, 6, 1}, 1.0f);
        for (std::size_t i = 0; i < 4; ++i) {
            if (blurred[i * 4 + 3] > 0.01f) QVERIFY(blurred[i * 4] > 0.99f && blurred[i * 4 + 1] < 0.01f);
        }
        QVERIFY(std::fabs(pt::per_dab(0.5f, 1.0f) - 0.5f) < 1e-6f && pt::per_dab(0.5f, 0.25f) < 0.5f);
    }

    // retouch_cmds/tests.rs: Dodge brightens the midtones more than the shadows, Burn darkens; Shadows dodges the dark side
    // more — on every kind of layer.
    void dodgeAndBurnOnEveryKind() {
        const core::Document base = book([](int x, int) { return x < 100 ? Colour{0.5, 0.5, 0.5, 1} : Colour{0.08, 0.08, 0.08, 1}; });
        for (const std::string& kind : kinds()) {
            const core::Document doc = as_kind(base, kind);
            const QString k = QString::fromStdString(kind);
            const core::ApplyResult dodged = run_ops(doc, Json::array({stroke("dodge", {{10, 100}, {190, 100}}, 24)}));
            QCOMPARE(dodged.results.at(0)["changed"], Json(true));
            QVERIFY(dodged.results.at(0)["box_mm"].is_array());
            const Shown was = shown(doc), now = shown(dodged.doc);
            const float mid = now.at(50, 100)[0] - was.at(50, 100)[0], shadow = now.at(150, 100)[0] - was.at(150, 100)[0];
            QVERIFY2(mid > 0.05f, qPrintable(k + QStringLiteral(" midtone +%1").arg(mid)));
            QVERIFY2(mid > 2.0f * std::max(shadow, 0.0f), qPrintable(k + QStringLiteral(" midtone +%1 shadow +%2").arg(mid).arg(shadow)));
            QCOMPARE(now.at(50, 160), was.at(50, 160));  // (outside the brush)
            Json burn = stroke("burn", {{10, 60}, {90, 60}}, 16);
            burn["exposure"] = 80;
            QVERIFY2(pixel(run(doc, burn), 50, 60)[0] < 0.45f, qPrintable(k));
            Json shadows = stroke("dodge", {{10, 30}, {190, 30}}, 10);
            shadows["range"] = "shadows";
            const Shown lifted = shown(run(doc, shadows));
            QVERIFY2(lifted.at(150, 30)[0] - was.at(150, 30)[0] > lifted.at(50, 30)[0] - was.at(50, 30)[0], qPrintable(k));
        }
    }

    // Sponge: desaturate, then saturate (without vibrance) past where it began.
    void spongeMovesTheSaturation() {
        const core::Document base = book([](int, int) { return Colour{0.8, 0.4, 0.3, 1}; });
        for (const std::string& kind : {std::string("8"), std::string("16"), std::string("32"), std::string("cmyk"), std::string("lab")}) {
            const core::Document doc = as_kind(base, kind);
            const QString k = QString::fromStdString(kind);
            const float s0 = saturation(pixel(doc, 100, 100));
            const float s1 = saturation(pixel(run(doc, stroke("sponge", {{10, 100}, {190, 100}}, 20)), 100, 100));
            QVERIFY2(s1 < s0 * 0.8f, qPrintable(k + QStringLiteral(": %1 → %2").arg(s0).arg(s1)));
            Json more = stroke("sponge", {{10, 100}, {190, 100}}, 20);
            more["mode"] = "saturate";
            more["vibrance"] = false;
            const float s2 = saturation(pixel(run(doc, more), 100, 100));
            QVERIFY2(s2 > s0, qPrintable(k + QStringLiteral(": %1 → %2").arg(s0).arg(s2)));
        }
    }

    // Blur lowers a checker's variance (the rest untouched); Sharpen raises a low-contrast one, Protect Detail keeping it
    // within the local range.
    void blurAndSharpen() {
        for (const std::string& kind : kinds()) {
            const core::Document doc = as_kind(book([](int x, int y) { return (x + y) % 2 == 0 ? Colour{0.3, 0.3, 0.3, 1} : Colour{0.7, 0.7, 0.7, 1}; }), kind);
            const QString k = QString::fromStdString(kind);
            Json blur = stroke("blur_brush", {{20, 100}, {80, 100}}, 60);
            blur["strength"] = 100;
            const Shown was = shown(doc), now = shown(run(doc, blur));
            const double v0 = variance(was, 40, 90, 60, 110), v1 = variance(now, 40, 90, 60, 110);
            QVERIFY2(v1 < v0 * 0.5, qPrintable(k + QStringLiteral(": %1 → %2").arg(v0).arg(v1)));
            QVERIFY2(std::fabs(variance(now, 140, 90, 160, 110) - variance(was, 140, 90, 160, 110)) < 1e-6, qPrintable(k));
        }
        for (const std::string& kind : {std::string("8"), std::string("16"), std::string("32")}) {
            const core::Document doc = as_kind(book([](int x, int y) { return (x + y) % 2 == 0 ? Colour{0.45, 0.45, 0.45, 1} : Colour{0.55, 0.55, 0.55, 1}; }), kind);
            const QString k = QString::fromStdString(kind);
            Json sharp = stroke("sharpen_brush", {{20, 100}, {80, 100}}, 60);
            sharp["strength"] = 100;
            sharp["protect_detail"] = false;
            const Shown was = shown(doc);
            const double v0 = variance(was, 40, 90, 60, 110), v1 = variance(shown(run(doc, sharp)), 40, 90, 60, 110);
            QVERIFY2(v1 > v0 * 1.5, qPrintable(k + QStringLiteral(": %1 → %2").arg(v0).arg(v1)));
            sharp["protect_detail"] = true;
            const Shown kept = shown(run(doc, sharp));
            float hi = 0;
            for (int y = 90; y < 110; ++y)
                for (int x = 40; x < 60; ++x) hi = std::max(hi, kept.at(x, y)[0]);
            QVERIFY2(hi <= 0.55f + 2.5f / 255.0f, qPrintable(k + QStringLiteral(": %1").arg(hi)));
        }
        // (review P3-11) a 32-bit page's Sharpen without Protect Detail: nothing below 0 (above 1 it may go: HDR)
        const core::Document hdr = as_kind(book([](int x, int y) { return (x + y) % 2 == 0 ? Colour{0.02, 0.02, 0.02, 1} : Colour{0.95, 0.95, 0.95, 1}; }), "32");
        Json sharp = stroke("sharpen_brush", {{20, 100}, {180, 100}}, 60);
        sharp["strength"] = 100;
        sharp["protect_detail"] = false;
        const Shown sharpened = shown(run(hdr, sharp));
        float lo = 1.0f, top = 0.0f;
        for (const Rgba& p : sharpened.rgba) {
            lo = std::min({lo, p[0], p[1], p[2]});
            top = std::max(top, p[0]);
        }
        QVERIFY2(lo >= 0.0f && top > 1.0f, qPrintable(QStringLiteral("%1 .. %2").arg(lo).arg(top)));
    }

    // Smudge drags the red along, fading with the distance; finger painting starts with the colour given.
    void smudgeCarriesColour() {
        for (const std::string& kind : {std::string("8"), std::string("16"), std::string("32"), std::string("cmyk"), std::string("lab")}) {
            const core::Document doc = as_kind(book([](int x, int) { return x < 60 ? Colour{1, 0, 0, 1} : Colour{1, 1, 1, 1}; }), kind);
            const QString k = QString::fromStdString(kind);
            Json smudge = stroke("smudge_brush", {{40, 100}, {140, 100}}, 24);
            smudge["strength"] = 80;
            const Shown now = shown(run(doc, smudge));
            QVERIFY2(now.at(72, 100)[1] < 0.6f, qPrintable(k + QStringLiteral(": %1").arg(now.at(72, 100)[1])));
            QVERIFY2(now.at(72, 100)[1] < now.at(130, 100)[1], qPrintable(k));
            QVERIFY2(now.at(72, 20)[1] > 0.99f, qPrintable(k));  // (off the stroke)
            Json finger = stroke("smudge_brush", {{150, 160}, {190, 160}}, 12);
            finger["finger_painting"] = true;
            finger["rgb"] = Json::array({0, 0, 255});
            const Rgba blue = pixel(run(doc, finger), 152, 160);
            QVERIFY2(blue[0] < 0.7f && blue[2] > blue[0], qPrintable(k + " " + text(blue)));
        }
    }

    // brush_cmds/tests.rs: the Mixer Brush — a low load dries out and a full one does not (its paint left as the result's
    // reservoir); a wet brush picks the canvas's colour up and smears it, cleaned after the stroke unless asked not to;
    // 全レイヤーを対象 picks up what the page shows under a clear layer.
    void theMixerBrush() {
        for (const std::string& kind : {std::string("8"), std::string("16"), std::string("cmyk")}) {
            const QString k = QString::fromStdString(kind);
            const core::Document doc = as_kind(book([](int x, int) { return x < 100 ? Colour{0, 0, 1, 1} : Colour{1, 1, 1, 1}; }, true), kind);
            for (const int load : {0, 100}) {
                Json op = stroke("mixer_brush", {{4, 20}, {196, 20}}, 8, "top");
                op["wet"] = 0;
                op["load"] = load;
                op["mix"] = 0;
                op["rgb"] = Json::array({255, 0, 0});
                const core::ApplyResult done = run_ops(doc, Json::array({op}));
                const Shown top = shown(done.doc, "top");
                const float start = top.at(10, 20)[3], end = top.at(190, 20)[3];
                QVERIFY2(start > 0.95f, qPrintable(k + QStringLiteral(" %1").arg(start)));
                const Json& reservoir = done.results.at(0)["reservoir"];
                if (load == 0) {
                    QVERIFY2(end < start * 0.6f, qPrintable(k + QStringLiteral(" %1 → %2").arg(start).arg(end)));
                    QVERIFY(reservoir["amount"].get<double>() < 0.2);
                } else {
                    QVERIFY2(end > 0.95f, qPrintable(k + QStringLiteral(" %1").arg(end)));
                    QCOMPARE(reservoir["amount"].get<double>(), 1.0);
                }
                QVERIFY(reservoir["pickup"].is_null());  // (cleaned after the stroke)
                QVERIFY(reservoir["paint"].is_array());
            }
            // wet, not cleaned: blue dragged into the white, the pickup kept for the next stroke
            Json wet = stroke("mixer_brush", {{4, 60}, {196, 60}}, 8);
            wet["wet"] = 100;
            wet["load"] = 30;
            wet["mix"] = 90;
            wet["clean_after"] = false;
            wet["rgb"] = Json::array({255, 0, 0});
            const core::ApplyResult smeared = run_ops(doc, Json::array({wet}));
            const Rgba c = pixel(smeared.doc, 115, 60);
            QVERIFY2(c[2] > c[1] + 0.05f && c[1] < 0.95f, qPrintable(k + " " + text(c)));
            const Json pickup = smeared.results.at(0)["reservoir"]["pickup"];
            QVERIFY2(pickup.is_array() && pickup[2].get<double>() > 0.2, smeared.results.dump().c_str());
            // the reservoir given back: the next stroke starts with that paint (and with load_after off, keeps it)
            Json next = stroke("mixer_brush", {{150, 140}, {190, 140}}, 8);
            next["reservoir"] = smeared.results.at(0)["reservoir"];
            next["load_after"] = false;
            next["mix"] = 0;
            next["wet"] = 0;
            const core::ApplyResult again = run_ops(smeared.doc, Json::array({next}));
            QCOMPARE(again.results.at(0)["reservoir"]["paint"], smeared.results.at(0)["reservoir"]["paint"]);
            // 全レイヤーを対象: a clear layer picks up the blue under it
            Json all = stroke("mixer_brush", {{20, 180}, {60, 180}}, 10, "top");
            all["wet"] = 100;
            all["mix"] = 100;
            QCOMPARE(pixel(run(doc, all), 40, 180, "top")[3], 0.0f);  // (nothing to pick up on the layer itself)
            all["sample_all"] = true;
            const Rgba under = pixel(run(doc, all), 40, 180, "top"), below = pixel(doc, 40, 180);  // (CMYK's blue: as its inks show it)
            QVERIFY2(under[3] > 0.5f && under[2] > under[0] + 0.2f, qPrintable(k + " " + text(under)));
            for (std::size_t ch = 0; ch < 3; ++ch) QVERIFY2(std::fabs(under[ch] - below[ch]) < 0.08f, qPrintable(k + " " + text(under) + " " + text(below)));
        }
        Json bad = stroke("mixer_brush", {{10, 10}}, 8);
        bad["reservoir"] = Json{{"paint", "red"}};
        QVERIFY(refusal(book([](int, int) { return Colour{1, 1, 1, 1}; }), bad).starts_with("apply: "));
    }

    // eraser_cmds/tests.rs: the Background Eraser erases the colour sampled once (white), keeps the black square and what
    // lies outside the brush — on every kind; continuous sampling, Protect Foreground, the swatch; contiguous limits.
    void backgroundEraser() {
        const core::Document base = book([](int x, int y) { return x >= 60 && x < 140 && y >= 60 && y < 140 ? Colour{0, 0, 0, 1} : Colour{1, 1, 1, 1}; });
        for (const std::string& kind : kinds()) {
            const core::Document doc = as_kind(base, kind);
            const QString k = QString::fromStdString(kind);
            Json op = stroke("background_eraser", {{10, 100}, {190, 100}}, 40);
            op["sampling"] = "once";
            op["limits"] = "discontiguous";
            op["tolerance"] = 10;
            const core::ApplyResult done = run_ops(doc, Json::array({op}));
            QCOMPARE(done.results.at(0)["changed"], Json(true));
            const Shown now = shown(done.doc);
            QVERIFY2(now.at(20, 100)[3] == 0.0f, qPrintable(k + " white under the brush " + text(now.at(20, 100))));
            QVERIFY2(now.at(100, 100)[3] == 1.0f, qPrintable(k + " black kept"));
            QVERIFY2(now.at(180, 100)[3] == 0.0f, qPrintable(k + " white past the square"));
            QVERIFY2(now.at(20, 180)[3] == 1.0f, qPrintable(k + " outside the brush"));
        }
        const core::Document doc = base;
        Json op = stroke("background_eraser", {{10, 100}, {190, 100}}, 40);
        op["limits"] = "discontiguous";
        op["tolerance"] = 10;
        QCOMPARE(pixel(run(doc, op), 100, 100)[3], 0.0f);  // (continuous: the black goes once the centre is over it)
        Json kept = op;
        kept["protect_foreground"] = true;
        kept["foreground_rgb"] = Json::array({0, 0, 0});
        const Shown guarded = shown(run(doc, kept));
        QCOMPARE(guarded.at(100, 100)[3], 1.0f);
        QCOMPARE(guarded.at(20, 100)[3], 0.0f);
        Json swatch = op;
        swatch["sampling"] = "swatch";
        swatch["swatch_rgb"] = Json::array({0, 0, 0});
        const Shown black = shown(run(doc, swatch));
        QCOMPARE(black.at(100, 100)[3], 0.0f);
        QCOMPARE(black.at(20, 100)[3], 1.0f);
        // contiguous: a white island behind the black wall stays; discontiguous takes it
        const core::Document walled = book([](int x, int) { return x >= 90 && x < 96 ? Colour{0, 0, 0, 1} : Colour{1, 1, 1, 1}; });
        for (const char* limits : {"contiguous", "find_edges", "discontiguous"}) {
            Json one = stroke("background_eraser", {{80, 100}}, 60);
            one["sampling"] = "once";
            one["limits"] = limits;
            one["tolerance"] = 10;
            const Shown island = shown(run(walled, one));
            QCOMPARE(island.at(80, 100)[3], 0.0f);
            QCOMPARE(island.at(100, 100)[3] == 0.0f, std::string(limits) == "discontiguous");
        }
        // refused: a protected transparency, a bad sampling or limits
        core::Document locked = doc;
        locked.edit_page(0).layers.back().lock_alpha = true;
        QVERIFY(refusal(locked, op).find("transparency is locked") != std::string::npos);
        for (const auto& [key, value] : std::vector<std::pair<std::string, Json>>{{"sampling", "sometimes"}, {"limits", 3}, {"tolerance", "lots"}}) {
            Json bad = op;
            bad[key] = value;
            QVERIFY2(refusal(doc, bad).starts_with("apply: "), key.c_str());
        }
    }

    // brush_cmds/tests.rs and replace.rs's: Color Replacement's modes — Hue and Color keep the luminosity, Luminosity takes
    // the colour's; the blue the sampling did not take untouched; contiguous stops at a wall.
    void colorReplacement() {
        const core::Document base = book([](int x, int) { return x < 100 ? Colour{0.2, 0.6, 0.2, 1} : Colour{0.1, 0.1, 0.5, 1}; });
        for (const char* mode : {"hue", "saturation", "color", "luminosity"}) {
            const core::Document doc = as_kind(base, "32");
            Json op = stroke("color_replacement", {{40, 100}, {160, 100}}, 60);
            op["mode"] = mode;
            op["sampling"] = "once";
            op["limits"] = "discontiguous";
            op["tolerance"] = 10;
            op["rgb"] = Json::array({255, 0, 0});
            const Shown now = shown(run(doc, op));
            const Rgba c = now.at(40, 100);
            const Rgba green{0.2f, 0.6f, 0.2f, 1.0f};
            const QString m = QString::fromLatin1(mode);
            if (std::string(mode) == "hue" || std::string(mode) == "color") {
                QVERIFY2(std::fabs(lum(c) - lum(green)) < 2e-3f && c[0] > c[1], qPrintable(m + " " + text(c)));
            } else if (std::string(mode) == "saturation") {
                QVERIFY2(std::fabs(lum(c) - lum(green)) < 2e-3f && c[1] > c[0], qPrintable(m + " " + text(c)));
            } else {
                QVERIFY2(std::fabs(lum(c) - 0.3f) < 2e-3f, qPrintable(m + " " + text(c)));
            }
            const Rgba b = now.at(160, 100);
            QVERIFY2(std::fabs(b[2] - 0.5f) < 2e-3f && std::fabs(b[0] - 0.1f) < 2e-3f, qPrintable(m + " " + text(b)));
        }
        for (const std::string& kind : {std::string("8"), std::string("16"), std::string("cmyk"), std::string("lab")}) {
            const core::Document doc = as_kind(base, kind);
            Json op = stroke("color_replacement", {{40, 100}, {160, 100}}, 60);
            op["tolerance"] = 20;
            op["rgb"] = Json::array({255, 0, 0});
            const Shown now = shown(run(doc, op));
            const QString k = QString::fromStdString(kind);
            QVERIFY2(now.at(40, 100)[0] > now.at(40, 100)[1], qPrintable(k + " " + text(now.at(40, 100))));  // (continuous: both halves)
            QVERIFY2(now.at(160, 100)[0] > now.at(160, 100)[1], qPrintable(k + " " + text(now.at(160, 100))));
        }
        const core::Document walled = as_kind(book([](int x, int) { return x >= 90 && x < 96 ? Colour{0, 0, 1, 1} : Colour{0.2, 0.6, 0.2, 1}; }), "32");
        for (const char* limits : {"contiguous", "discontiguous"}) {
            Json op = stroke("color_replacement", {{80, 100}}, 60);
            op["sampling"] = "once";
            op["limits"] = limits;
            op["tolerance"] = 5;
            op["rgb"] = Json::array({255, 0, 0});
            const Shown now = shown(run(walled, op));
            QVERIFY(now.at(80, 100)[0] > 0.3f);
            QCOMPARE(std::fabs(now.at(100, 100)[0] - 0.2f) > 0.05f, std::string(limits) == "discontiguous");
        }
        Json bad = stroke("color_replacement", {{10, 10}}, 8);
        bad["mode"] = "bogus";
        QVERIFY(refusal(base, bad).starts_with("apply: "));
    }

    // Only inside the selection; a protected transparency stays clear under the brushes that would paint it.
    void theSelectionAndAProtectedTransparency() {
        const core::Document doc = as_kind(book([](int x, int y) { return (x + y) % 2 == 0 ? Colour{0.3, 0.3, 0.3, 1} : Colour{0.7, 0.7, 0.7, 1}; }), "cmyk");
        Json blur = stroke("blur_brush", {{20, 100}, {180, 100}}, 40);
        blur["strength"] = 100;
        blur["area"] = Json{{"rect", Json::array({len(100), 0, len(100), kInch})}};
        const Shown was = shown(doc), now = shown(run(doc, blur));
        QCOMPARE(now.at(50, 100), was.at(50, 100));
        QVERIFY(variance(now, 130, 90, 170, 110) < variance(was, 130, 90, 170, 110) * 0.5);
        core::Document clear = book([](int x, int) { return x < 100 ? Colour{1, 0, 0, 1} : Colour{0, 0, 0, 0}; });
        clear.edit_page(0).layers.back().lock_alpha = true;
        for (const char* op : {"smudge_brush", "blur_brush", "mixer_brush"}) {
            Json s = stroke(op, {{60, 100}, {160, 100}}, 20);
            s["strength"] = 100;
            s["wet"] = 100;
            const core::Document done = run(clear, s);
            QVERIFY2(pixel(done, 140, 100)[3] == 0.0f, op);
        }
    }

    // An ink layer's pixels the brush leaves keep their very inks (the page's rules only for those it changed); a 32-bit
    // page's pixels brighter than 1 are left by Dodge, Burn and Sponge.
    void theInksAndTheBrightPixelsKept() {
        const core::Document doc = as_kind(book([](int x, int y) { return Colour{0.2 + x / 400.0, 0.4, 0.6 - y / 500.0, 1}; }), "cmyk");
        const core::ApplyResult done = run_ops(doc, Json::array({stroke("dodge", {{100, 100}}, 30)}));
        const core::InkPixels before = core::decode_ink_raster(*layer_of(doc, "paint").ink_raster);
        const core::InkPixels after = core::decode_ink_raster(*layer_of(done.doc, "paint").ink_raster);
        QCOMPARE(after.width, before.width);
        std::size_t changed = 0, far = 0;
        const unsigned n = before.colours() + 1;
        for (std::uint32_t y = 0; y < before.height; ++y)
            for (std::uint32_t x = 0; x < before.width; ++x) {
                const std::size_t i = (std::size_t(y) * before.width + x) * n;
                const bool same = std::equal(before.samples.begin() + static_cast<std::ptrdiff_t>(i), before.samples.begin() + static_cast<std::ptrdiff_t>(i + n),
                                             after.samples.begin() + static_cast<std::ptrdiff_t>(i));
                const double dx = (x + 0.5) * kSide / before.width - 100.5, dy = (y + 0.5) * kSide / before.height - 100.5;
                if (!same) ++changed;
                if (!same && std::hypot(dx, dy) > 17) ++far;
            }
        QVERIFY(changed > 100);
        QCOMPARE(far, std::size_t{0});
        // a 32-bit page's HDR: a pixel past 1 is left as it is, one within 0..1 dodged
        core::Document hdr = as_kind(book([](int, int) { return Colour{0.5, 0.5, 0.5, 1}; }), "32");
        const core::ColorRasterView view(*layer_of(hdr, "paint").color_raster);
        const std::uint32_t w = view.width(), h = view.height();
        hdr.edit_page(0).layers.back().color_raster = std::make_shared<const std::string>(core::encode_color_pixels(w, h, "f32", [w](std::size_t i) {
            return i % w < w / 2 ? std::array<double, 4>{1.5, 1.2, 0.5, 1.0} : std::array<double, 4>{0.5, 0.5, 0.5, 1.0};
        }));
        for (const char* op : {"dodge", "burn", "sponge"}) {
            const core::Document toned = run(hdr, stroke(op, {{10, 100}, {190, 100}}, 30));
            const Shown s = shown(toned);
            const Rgba bright = s.at(50, 100), plain = s.at(150, 100);
            QVERIFY2(bright[0] == 1.5f && bright[1] == 1.2f && bright[2] == 0.5f, qPrintable(QString::fromLatin1(op) + " " + text(bright)));
            if (std::string(op) != "sponge") QVERIFY2(std::fabs(plain[0] - 0.5f) > 0.02f, qPrintable(QString::fromLatin1(op) + " " + text(plain)));
        }
    }

    // A dab brush of the book lays its own tip (here a soft one, unlike the hard round brush); unknown or not a dab brush:
    // refused. The same input, the same bytes (with a seed, a jittering brush's too).
    void aDabBrushAndTheSameBytes() {
        const core::Document base = book([](int x, int y) { return (x + y) % 2 == 0 ? Colour{0.3, 0.3, 0.3, 1} : Colour{0.7, 0.7, 0.7, 1}; });
        const core::Document doc = run(base, Json{{"op", "define_brush"}, {"key", "my_soft"}, {"label", "やわらか"}, {"engine", "dabs"},
                                                  {"dabs", Json{{"hardness", 0.0}, {"shape", Json{{"enabled", true}, {"size", Json{{"jitter", 0.5}}}}}}}});
        Json round = stroke("blur_brush", {{20, 100}, {180, 100}}, 40);
        round["strength"] = 100;
        Json soft = round;
        soft["brush"] = "my_soft";
        soft["seed"] = 7;
        soft.erase("hardness");
        const core::ApplyResult a = run_ops(doc, Json::array({soft}));
        QCOMPARE(a.results.at(0)["changed"], Json(true));
        QVERIFY(bytes_of(a.doc) != bytes_of(run(doc, round)));
        QCOMPARE(bytes_of(run(doc, soft)), bytes_of(a.doc));
        for (const char* op : {"dodge", "sponge", "smudge_brush", "mixer_brush", "background_eraser", "color_replacement", "sharpen_brush"}) {
            Json one = stroke(op, {{20, 60}, {180, 140}}, 30);
            QCOMPARE(bytes_of(run(doc, one)), bytes_of(run(doc, one)));
            one["brush"] = "my_soft";
            QCOMPARE(bytes_of(run(doc, one)), bytes_of(run(doc, one)));
        }
        soft["brush"] = "my_nothing";
        QVERIFY(refusal(doc, soft).starts_with("apply: "));
        soft["brush"] = "gpen";
        QVERIFY(refusal(doc, soft).find("dab brush") != std::string::npos);
    }

    // The work refused before it is laid (quickly, nothing held); points far off the page; the refusals every brush shares;
    // a stroke off the layer changes nothing.
    void theLimitsAndTheRefusals() {
        const core::Document doc = book([](int, int) { return Colour{0.5, 0.5, 0.5, 1}; });
        // the widest brush back and forth over the page at the finest spacing: every dab covers the layer
        Json heavy = stroke("blur_brush", {{0, 0}}, 10);
        Json pts = Json::array();
        for (int i = 0; i < 12000; ++i) pts.push_back(i % 2 == 0 ? Json::array({0.0, 0.0}) : Json::array({kInch, kInch}));
        heavy["points"] = pts;
        heavy["width_mm"] = 500;
        heavy["spacing"] = 1;
        const auto t0 = std::chrono::steady_clock::now();
        const std::string why = refusal(doc, heavy);
        QVERIFY2(why.find("too_large") != std::string::npos, why.c_str());
        QVERIFY(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(30));
        Json far = stroke("dodge", {{10, 10}}, 10);
        far["points"] = Json::array({Json::array({0, 0}), Json::array({200000, 0})});
        QVERIFY(refusal(doc, far).find("100000 mm") != std::string::npos);
        Json fine = stroke("dodge", {{10, 10}}, 10);
        fine["points"] = Json::array({Json::array({0, 0}), Json::array({99999, 0})});
        fine["width_mm"] = 0.05;
        fine["spacing"] = 1;
        QVERIFY2(refusal(doc, fine).find("dabs") != std::string::npos, refusal(doc, fine).c_str());
        // (review P3-8: two hundred thousand dabs a stroke, section E 3.6 — here some 300 000 half a pixel apart)
        Json many = stroke("dodge", {{10, 10}}, 10);
        many["points"] = Json::array({Json::array({0, 0}), Json::array({19050, 0})});
        many["width_mm"] = 0.2;
        many["spacing"] = 1;
        QVERIFY2(refusal(doc, many).find("more than 200000 dabs") != std::string::npos, refusal(doc, many).c_str());
        for (const char* op : {"dodge", "burn", "sponge", "blur_brush", "sharpen_brush", "smudge_brush", "mixer_brush", "background_eraser", "color_replacement"}) {
            Json good = stroke(op, {{100, 100}}, 10);
            for (const auto& [key, value] : std::vector<std::pair<std::string, Json>>{{"points", Json::array()}, {"points", "here"}, {"width_mm", 0}, {"hardness", "soft"}}) {
                Json bad = good;
                bad[key] = value;
                QVERIFY2(refusal(doc, bad).starts_with("apply: "), (std::string(op) + " " + key).c_str());
            }
            core::Document locked = doc;
            locked.edit_page(0).layers.back().locked = true;
            QVERIFY2(refusal(locked, good).find("locked") != std::string::npos, op);
            core::Document folder = doc;
            folder.edit_page(0).layers.back().kind = core::LayerKind::Folder;
            QVERIFY2(refusal(folder, good).find("not a paint layer") != std::string::npos, op);
            const core::ApplyResult none = run_ops(doc, Json::array({stroke(op, {{-500, -500}}, 10)}));
            QCOMPARE(none.results.at(0)["changed"], Json(false));
            QCOMPARE(bytes_of(none.doc), bytes_of(doc));
        }
        Json range = stroke("dodge", {{100, 100}}, 10);
        range["range"] = "bogus";
        QVERIFY(refusal(doc, range).starts_with("apply: "));
        Json mode = stroke("sponge", {{100, 100}}, 10);
        mode["mode"] = "wash";
        QVERIFY(refusal(doc, mode).starts_with("apply: "));
    }

    // 色混ぜ (smudge, design section E 0.4): a precise layer's values smudged in float (finer than 8 bits), an ink layer's inks
    // with its line kept a line (not lent the 8-bit picture); an 8-bit layer as before (a picture laid on it).
    void smudgeOnPreciseAndInkLayers() {
        const core::Document base = book([](int x, int) { return x < 100 ? Colour{0.9, 0.1, 0.1, 1} : Colour{0.1, 0.1, 0.9, 1}; });
        const Json op{{"op", "smudge"}, {"page", 1}, {"layer_id", "paint"}, {"mode", "smudge"}, {"width_mm", 4},
                      {"points", Json::array({Json::array({mm(80), mm(100)}), Json::array({mm(150), mm(100)})})}};
        const core::Document eight = run(base, op);
        QCOMPARE(layer_of(eight, "paint").patches.size(), layer_of(base, "paint").patches.size() + 1);
        for (const char* kind : {"16", "32"}) {
            const core::Document doc = as_kind(base, kind);
            const core::Document done = run(doc, op);
            const core::Layer l = layer_of(done, "paint");
            QVERIFY(l.color_raster && l.patches.empty() && !l.raster_png);
            const Shown s = shown(done);
            bool finer = false;
            for (int x = 100; x < 140 && !finer; ++x) {
                const float v = s.at(x, 100)[0] * 255.0f;
                finer = std::fabs(v - std::round(v)) > 0.01f;
            }
            QVERIFY2(finer, kind);
            QVERIFY(s.at(120, 100)[0] > 0.2f);  // (the red carried into the blue)
        }
        core::Document inked = as_kind(base, "cmyk");
        inked = run(inked, Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", "paint"}, {"width_mm", 0.5},
                                {"points", Json::array({Json::array({mm(120), mm(20)}), Json::array({mm(120), mm(180)})})}});
        const std::size_t lines = layer_of(inked, "paint").stroke_count();
        QVERIFY(lines > 0);
        const core::Document done = run(inked, op);
        QCOMPARE(layer_of(done, "paint").stroke_count(), lines);
        QVERIFY(!layer_of(done, "paint").raster_png);
        QVERIFY(*layer_of(done, "paint").ink_raster != *layer_of(inked, "paint").ink_raster);
    }

    // (review P3-6) Every tool on every kind of layer (8, 16 and 32 bits, CMYK, Lab, grey): only inside the selection (what
    // lies outside it as it was, inside changed — Sponge and Color Replacement have no colour to move on a grey page), and a
    // protected transparency kept clear and opaque (the Background Eraser refused there).
    void everyToolInsideTheSelectionAndTheLock() {
        const auto colourful = [](int x, int y) { return (x / 4 + y / 4) % 2 == 0 ? Colour{0.8, 0.3, 0.2, 1} : Colour{0.2, 0.5, 0.8, 1}; };
        const auto halved = [&](int x, int y) { return x < 150 ? colourful(x, y) : Colour{0, 0, 0, 0}; };
        const std::vector<std::string> ops{"dodge", "burn", "sponge", "blur_brush", "sharpen_brush", "smudge_brush", "mixer_brush", "background_eraser",
                                           "color_replacement"};
        const auto with_settings = [](Json s) {
            s["strength"] = 100;
            s["protect_detail"] = false;
            s["wet"] = 100;
            s["rgb"] = Json::array({0, 160, 40});
            s["tolerance"] = 60;
            return s;
        };
        for (const std::string& kind : kinds()) {
            const core::Document doc = as_kind(book(colourful), kind);
            core::Document locked = as_kind(book(halved), kind);
            locked.edit_page(0).layers.back().lock_alpha = true;
            const Shown was = shown(doc), was_half = shown(locked);
            for (const std::string& op : ops) {
                const QString k = QString::fromStdString(kind + " " + op);
                Json s = with_settings(stroke(op.c_str(), {{20, 100}, {180, 100}}, 30));
                s["area"] = Json{{"rect", Json::array({len(100), 0, len(100), kInch})}};
                const Shown now = shown(run(doc, s));
                for (int y = 85; y <= 115; y += 5) {
                    for (int x = 20; x <= 90; x += 7) QVERIFY2(now.at(x, y) == was.at(x, y), qPrintable(k + QStringLiteral(" outside (%1, %2) ").arg(x).arg(y) + text(now.at(x, y))));
                }
                bool inside = false;
                for (int y = 90; y <= 110 && !inside; ++y) {
                    for (int x = 110; x <= 170 && !inside; ++x) inside = now.at(x, y) != was.at(x, y);
                }
                if (!(kind == "gray" && (op == "sponge" || op == "color_replacement"))) QVERIFY2(inside, qPrintable(k + " inside"));
                Json l = with_settings(stroke(op.c_str(), {{100, 100}, {190, 100}}, 30));
                if (op == "background_eraser") {
                    QVERIFY2(refusal(locked, l).find("transparency is locked") != std::string::npos, qPrintable(k));
                    continue;
                }
                const Shown kept = shown(run(locked, l));
                for (int y = 90; y <= 110; y += 5) {
                    for (int x = 156; x < 200; x += 4) QVERIFY2(kept.at(x, y)[3] == 0.0f, qPrintable(k + QStringLiteral(" clear (%1, %2) ").arg(x).arg(y) + text(kept.at(x, y))));
                    for (int x = 100; x < 146; x += 5) QVERIFY2(kept.at(x, y)[3] == was_half.at(x, y)[3], qPrintable(k + QStringLiteral(" opaque (%1, %2)").arg(x).arg(y)));
                }
            }
        }
    }

    // (review P3-2, P3-6) A pen layer's lines and a paint layer's become the layer's pixels before a brush changes them (the
    // line still there, as pixels); a stroke that changes nothing — off the layer, or over colours it cannot move — leaves
    // the layer and its lines as they were (no pixels made for nothing).
    void linesBecomePixelsOnlyWhenChanged() {
        for (const std::string& kind : {std::string("8"), std::string("cmyk")}) {
            core::Document doc = as_kind(book([](int, int) { return Colour{1, 1, 1, 1}; }), kind);
            const Json blue = Json::array({90, 140, 200});
            doc = run_ops(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "pen"}},
                                            Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", "pen"}, {"width_mm", 1.5}, {"rgb", blue},
                                                 {"points", Json::array({Json::array({mm(100), mm(20)}), Json::array({mm(100), mm(180)})})}},
                                            Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", "paint"}, {"width_mm", 1.5}, {"rgb", blue},
                                                 {"points", Json::array({Json::array({mm(60), mm(20)}), Json::array({mm(60), mm(180)})})}},
                                            Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "black"}},
                                            Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", "black"}, {"width_mm", 1.5}, {"rgb", Json::array({0, 0, 0})},
                                                 {"points", Json::array({Json::array({mm(140), mm(20)}), Json::array({mm(140), mm(180)})})}}}))
                      .doc;
            for (const std::string id : {"pen", "paint"}) {
                const QString k = QString::fromStdString(kind + " " + id);
                const core::Layer& before = layer_of(doc, id);
                const core::ApplyResult off = run_ops(doc, Json::array({stroke("dodge", {{-500, -500}}, 10, id)}));
                QCOMPARE(off.results.at(0)["changed"], Json(false));
                QVERIFY2(layer_of(off.doc, id).kind == before.kind && layer_of(off.doc, id).stroke_count() == 1, qPrintable(k));
                QCOMPARE(bytes_of(off.doc, id), bytes_of(doc, id));
                const int x = id == "pen" ? 100 : 60;
                const core::ApplyResult through = run_ops(doc, Json::array({stroke("burn", {{double(x - 20), 100}, {double(x + 20), 100}}, 16, id)}));
                QCOMPARE(through.results.at(0)["changed"], Json(true));
                const core::Layer& after = layer_of(through.doc, id);
                QVERIFY2(after.kind == core::LayerKind::Raster && after.stroke_count() == 0, qPrintable(k));
                const Rgba line = pixel(through.doc, x, 170, id);  // (far from the brush: the line, as pixels)
                QVERIFY2(line[3] > 0.9f && lum(line) < 0.7f && line[2] > line[0] + 0.15f, qPrintable(k + " " + text(line)));
            }
            if (kind != "8") continue;
            // over a black line the Sponge moves nothing: the line stays a line
            const core::ApplyResult grey = run_ops(doc, Json::array({stroke("sponge", {{120, 100}, {160, 100}}, 16, "black")}));
            QCOMPARE(grey.results.at(0)["changed"], Json(false));
            QCOMPARE(layer_of(grey.doc, "black").stroke_count(), std::size_t(1));
            QVERIFY(layer_of(grey.doc, "black").kind == core::LayerKind::Strokes);
        }
    }

    // (review P2-1, P3-10, P3-6) A colour plate worked alone — "cyan" of a CMYK page, "red" of an RGB one —: the brush's
    // pixels change that plate only, and every pixel of the layer it did not change shows as before, its lines on every
    // plate (drawn into the inks with them; filter_raster's plate alike); the Background Eraser refused on a plate. The
    // tools on an alpha channel, a spot channel and the Quick Mask change the channel, not the layer.
    void colourPlatesAndChannels() {
        const auto colourful = [](int x, int y) { return (x / 4 + y / 4) % 2 == 0 ? Colour{0.8, 0.3, 0.2, 1} : Colour{0.2, 0.5, 0.8, 1}; };
        core::Document cmyk = as_kind(book(colourful), "cmyk");
        cmyk = run(cmyk, Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", "paint"}, {"width_mm", 1.5}, {"rgb", Json::array({0, 0, 0})},
                              {"points", Json::array({Json::array({mm(150), mm(10)}), Json::array({mm(150), mm(190)})})}});
        const core::InkPixels showed = render::ink_edit::with_marks(cmyk.page(0), layer_of(cmyk, "paint"), &cmyk);
        const auto on_the_line = [&](const core::Document& done, unsigned from, const char* what) {
            const core::InkPixels now = render::ink_edit::with_marks(done.page(0), layer_of(done, "paint"), &done);
            QCOMPARE(now.width, showed.width);
            for (std::uint32_t y = 10; y < 190; y += 7) {
                for (std::uint32_t x = 145; x <= 155; ++x) {
                    const std::size_t i = std::size_t(y) * showed.width + x;
                    for (unsigned c = from; c < showed.channels(); ++c) {
                        if (now.value(i, c) != showed.value(i, c)) QFAIL(qPrintable(QStringLiteral("%1: (%2, %3) channel %4").arg(QString::fromLatin1(what)).arg(x).arg(y).arg(c)));
                    }
                }
            }
        };
        Json dodge = stroke("dodge", {{20, 60}, {80, 60}}, 20);
        dodge["channel"] = "cyan";
        const core::Document dodged = run(cmyk, dodge);
        on_the_line(dodged, 0, "dodge on cyan");
        QVERIFY(*layer_of(dodged, "paint").ink_raster != *layer_of(cmyk, "paint").ink_raster);
        const core::Document levelled = run(cmyk, Json{{"op", "filter_raster"}, {"page", 1}, {"id", "paint"}, {"kind", "levels"}, {"black", 0}, {"white", 128},
                                                       {"channel", "cyan"}});
        on_the_line(levelled, 1, "filter_raster on cyan");
        Json eraser = stroke("background_eraser", {{20, 60}, {80, 60}}, 20);
        eraser["channel"] = "cyan";
        QVERIFY(refusal(cmyk, eraser).find("colour plate") != std::string::npos);
        // an RGB page's red plate
        const core::Document rgb = book(colourful);
        Json red = stroke("dodge", {{20, 100}, {180, 100}}, 30);
        red["channel"] = "red";
        const Shown was = shown(rgb), now = shown(run(rgb, red));
        bool reds = false;
        for (int y = 90; y <= 110; y += 2) {
            for (int x = 30; x <= 170; x += 3) {
                reds = reds || now.at(x, y)[0] != was.at(x, y)[0];
                for (std::size_t c = 1; c < 4; ++c) QCOMPARE(now.at(x, y)[c], was.at(x, y)[c]);
            }
        }
        QVERIFY(reds);
        // channels: an alpha channel, a spot channel (the left half each), the Quick Mask
        core::Document channelled = run(rgb, Json{{"op", "add_channel"}, {"page", 1}, {"name", "A"}, {"fill", "area"},
                                                  {"area", Json{{"rect", Json::array({0, 0, len(100), kInch})}}}});
        channelled = run(channelled, Json{{"op", "add_channel"}, {"page", 1}, {"kind", "spot"}, {"name", "S"}, {"area", Json{{"rect", Json::array({0, 0, len(100), kInch})}}}});
        const auto channel_bytes = [](const core::Document& d, const std::string& name) {
            for (const core::Channel& ch : d.page(0).channels)
                if (ch.name == name) return ch.raster ? *ch.raster : std::string();
            return std::string("(none)");
        };
        for (const char* op : {"dodge", "burn", "sponge", "blur_brush", "sharpen_brush", "smudge_brush", "mixer_brush", "background_eraser", "color_replacement"}) {
            Json s = stroke(op, {{40, 100}, {160, 100}}, 30);
            s["strength"] = 100;
            s["protect_detail"] = false;
            s["wet"] = 100;
            s["rgb"] = Json::array({255, 255, 255});
            s["channel"] = "A";
            const core::Document done = run(channelled, s);
            QVERIFY2(bytes_of(done) == bytes_of(channelled), op);  // (the layer as it was)
            const std::string o = op;
            if (o == "blur_brush" || o == "smudge_brush" || o == "mixer_brush" || o == "sharpen_brush") QVERIFY2(channel_bytes(done, "A") != channel_bytes(channelled, "A"), op);
        }
        Json blur = stroke("blur_brush", {{40, 100}, {160, 100}}, 30);
        blur["strength"] = 100;
        blur["channel"] = "S";
        const core::Document spotted = run(channelled, blur);
        QCOMPARE(bytes_of(spotted), bytes_of(channelled));
        QVERIFY(channel_bytes(spotted, "S") != channel_bytes(channelled, "S"));
        const core::Document quick = run(channelled, Json{{"op", "quick_mask"}, {"page", 1}, {"on", true}});
        Json mixer = stroke("mixer_brush", {{40, 100}, {160, 100}}, 30);
        mixer["rgb"] = Json::array({0, 0, 0});  // (black: masked)
        mixer["channel"] = "クイックマスク";
        const core::Document masked = run(quick, mixer);
        QCOMPARE(bytes_of(masked), bytes_of(quick));
        const auto quick_bytes = [](const core::Document& d) {
            for (const core::Channel& ch : d.page(0).channels)
                if (ch.quick_mask) return ch.raster ? *ch.raster : std::string();
            return std::string("(none)");
        };
        QVERIFY(quick_bytes(quick) != "(none)");
        QVERIFY(quick_bytes(masked) != quick_bytes(quick));
    }

    // (review P1-1, P3-6) 色混ぜ's ぼかし and なじませ on precise and ink layers; its 指先 with a brush four times the layer wide
    // works within a small budget (its spot cut to the box: the same result), a wider brush refused before anything is
    // held, at once.
    void smudgeModesAndLimits() {
        const core::Document base = book([](int x, int) { return x < 100 ? Colour{0.9, 0.1, 0.1, 1} : Colour{0.1, 0.1, 0.9, 1}; });
        Json op{{"op", "smudge"}, {"page", 1}, {"layer_id", "paint"}, {"width_mm", 4},
                {"points", Json::array({Json::array({mm(80), mm(100)}), Json::array({mm(150), mm(100)})})}};
        for (const char* mode : {"blur", "blend"}) {
            for (const std::string& kind : {std::string("16"), std::string("cmyk")}) {
                const core::Document doc = as_kind(base, kind);
                op["mode"] = mode;
                const core::Document done = run(doc, op);
                const QString k = QString::fromStdString(kind) + " " + QString::fromLatin1(mode);
                QVERIFY2(bytes_of(done) != bytes_of(doc), qPrintable(k));
                const Shown s = shown(done);
                QVERIFY2(s.at(100, 100)[0] < 0.85f && s.at(100, 100)[2] > 0.15f, qPrintable(k + " " + text(s.at(100, 100))));  // (mixed at the edge)
                QCOMPARE(s.at(100, 20), shown(doc).at(100, 20));  // (off the stroke)
                if (kind != "16") continue;
                bool finer = false;
                for (int x = 90; x < 115 && !finer; ++x) {
                    const float v = s.at(x, 100)[0] * 255.0f;
                    finer = std::fabs(v - std::round(v)) > 0.01f;
                }
                QVERIFY2(finer, qPrintable(k));
            }
        }
        for (const std::string& kind : {std::string("16"), std::string("cmyk")}) {
            const core::Document doc = as_kind(base, kind);
            Json wide = op;
            wide["mode"] = "smudge";
            wide["width_mm"] = 100;  // (787 pixels on the 200-pixel layer: within four times its side)
            wide["points"] = Json::array({Json::array({mm(10), mm(100)}), Json::array({mm(60), mm(100)}), Json::array({mm(110), mm(100)}),
                                          Json::array({mm(160), mm(100)})});  // (a step a segment: the colour carried)
            {
                const render::ImageAllocationBudget small{12ULL * 1024 * 1024};
                QVERIFY2(bytes_of(run(doc, wide)) != bytes_of(doc), kind.c_str());
            }
            wide["width_mm"] = 1.0e7;
            const auto t0 = std::chrono::steady_clock::now();
            const std::string why = refusal(doc, wide);
            QVERIFY2(why.find("too wide") != std::string::npos && why.find("too_large") != std::string::npos, why.c_str());
            QVERIFY(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5));
        }
        // (an 8-bit layer: as before, no such limit)
        Json eight = op;
        eight["mode"] = "smudge";
        eight["width_mm"] = 100;
        QVERIFY(refusal(base, eight).empty());
    }

    // (review P2-2) 色混ぜ on an ink page's paint layer as before E5: what it shows — its lines too — smudged and laid in
    // its inks under its lines (they stay lines), on a layer of lines alone, on its second stroke and on a layer with inks:
    // the same look as the 8-bit picture smudged on the RGB page it was made from (alpha and lightness, within the 8
    // bits' and the grey's), never refused where only a line lies.
    void smudgeOnAnInkPageAsBefore() {
        core::Document rgb = book([](int x, int y) { return x >= 30 && x < 70 && y >= 30 && y < 170 ? Colour{0.5, 0.5, 0.5, 1} : Colour{0, 0, 0, 0}; });
        const auto line = [](const char* id, double x) {
            return Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", id}, {"width_mm", 1.2}, {"rgb", Json::array({0, 0, 0})},
                        {"points", Json::array({Json::array({mm(x), mm(20)}), Json::array({mm(x), mm(180)})})}};
        };
        rgb = run_ops(rgb, Json::array({line("paint", 70), Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "ink"}}, line("ink", 130)})).doc;
        for (auto& layer : rgb.edit_page(0).layers) layer.panel_clip = layer.panel_each = false;
        core::Document grey = as_kind(rgb, "gray");
        QVERIFY(!layer_of(grey, "ink").ink_raster && layer_of(grey, "ink").stroke_count() == 1);  // (a layer of lines alone)
        const auto smudge_at = [](const char* id, double x0, double x1, double y) {
            return Json{{"op", "smudge"}, {"page", 1}, {"layer_id", id}, {"mode", "smudge"}, {"width_mm", 4},
                        {"points", Json::array({Json::array({mm(x0), mm(y)}), Json::array({mm(x1), mm(y)})})}};
        };
        const auto alike = [&](const core::Document& a, const core::Document& b, const char* id, int y, const char* what) {
            const Shown sa = shown(a, id), sb = shown(b, id);
            int smeared = 0;
            for (int x = 40; x < 180; ++x) {
                const Rgba p = sa.at(x, y), q = sb.at(x, y);
                if (std::fabs(p[3] - q[3]) > 0.05f) QFAIL(qPrintable(QStringLiteral("%1: (%2, %3) alpha %4 / %5").arg(QString::fromLatin1(what)).arg(x).arg(y).arg(p[3]).arg(q[3])));
                // (the colours where both are nearly opaque: where clear pixels mix in, the 8-bit picture's straight values
                // darken toward its clear black, the values' premultiplied ones do not — SPEC's 色混ぜ)
                if (p[3] > 0.9f && q[3] > 0.9f && std::fabs(lum(p) - lum(q)) > 0.08f) QFAIL(qPrintable(QString::fromLatin1(what) + " " + text(p) + " / " + text(q)));
                smeared += p[3] > 0.05f && p[3] < 0.95f ? 1 : 0;
            }
            QVERIFY2(smeared > 3, what);  // (the line smeared, not a hard line)
        };
        // a layer of lines alone, then its second stroke (where only the line lies)
        core::Document rgb1 = run(rgb, smudge_at("ink", 110, 150, 60)), grey1 = run(grey, smudge_at("ink", 110, 150, 60));
        alike(rgb1, grey1, "ink", 60, "lines alone");
        QVERIFY(layer_of(grey1, "ink").ink_raster && layer_of(grey1, "ink").stroke_count() == 1);  // (its line still a line)
        core::Document rgb2 = run(rgb1, smudge_at("ink", 110, 150, 140)), grey2 = run(grey1, smudge_at("ink", 110, 150, 140));
        alike(rgb2, grey2, "ink", 140, "the second stroke");
        // a layer with inks and a line
        alike(run(rgb, smudge_at("paint", 50, 90, 100)), run(grey, smudge_at("paint", 50, 90, 100)), "paint", 100, "inks and a line");
        QCOMPARE(layer_of(run(grey, smudge_at("paint", 50, 90, 100)), "paint").stroke_count(), std::size_t(1));
    }

    // A layer's mask painted through the lending (layer_mask: the brushes' pixels untouched, the mask changed).
    void aMaskPainted() {
        core::Document doc = book([](int, int) { return Colour{0.5, 0.5, 0.5, 1}; });
        doc = run(doc, Json{{"op", "set_layer_mask"}, {"page", 1}, {"id", "paint"}, {"area", Json{{"rect", Json::array({0, 0, len(100), kInch})}}}});
        QVERIFY(layer_of(doc, "paint").mask && layer_of(doc, "paint").mask->png);
        Json blur = stroke("blur_brush", {{60, 100}, {140, 100}}, 40);
        blur["strength"] = 100;
        blur["layer_mask"] = true;
        const core::ApplyResult done = run_ops(doc, Json::array({blur}));
        QCOMPARE(done.results.at(0)["changed"], Json(true));
        QCOMPARE(bytes_of(done.doc), bytes_of(doc));
        QVERIFY(*layer_of(done.doc, "paint").mask->png != *layer_of(doc, "paint").mask->png);
    }
};

QTEST_MAIN(TestPaintTools)
#include "test_paint_tools.moc"
