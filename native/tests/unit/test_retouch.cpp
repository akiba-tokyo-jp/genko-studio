// PC3 B4 (SPEC.md COLOR-06「修復」): the retouching ops as PhotoCraft v0.2.0's engine/src/retouch_cmds/tests.rs and
// edit_menu_cmds' tests check them, on every kind of layer (8 bits; 16 and 32 bits; the inks of CMYK, Lab and grey
// pages): the Clone Stamp copies exactly from its offset, samples the pixels before the stroke and gives the next
// source when not aligned, takes the layer, the layers up to it or the page, keeps to the selection and the opacity;
// the Healing Brush takes the destination's colour and the source's texture; the Spot Healing Brush's three ways remove a
// blemish and leave the rest; the History Brush paints back the pixels of a state the book holds (and refuses another
// layer's); Content-Aware Fill fills the selection on the layer, a new layer or a copy, keeping to its sampling area;
// a protected transparency stays; the same input gives the same pixels; and the refusals.

#include <QtTest>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/ink.hpp"
#include "render/image_ops.hpp"
#include "render/ops_registry.hpp"
#include "render/png.hpp"
#include "render/raster.hpp"
#include "storage/asset_store.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
using Rgba = render::image_ops::Rgba;

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

// A pixel's centre, and a length, in mm.
double mm(double px) { return (px + 0.5) * kInch / kSide; }
double len(double px) { return px * kInch / kSide; }

using Colour = std::array<double, 4>;  // (straight 0..1)

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

// A page of one 8-bit paint layer "paint" drawn by `at`.
core::Document book(const std::function<Colour(int, int)>& at) {
    auto doc = core::new_episode("修復", core::Num(1), 1, core::PageSpec::custom(kInch, kInch, 20, 20, 1, 2, 2, 2, 2, 200, "color"));
    auto& p = doc.edit_page(0);
    p.numero = false;
    p.frames.clear();
    p.layers.clear();
    p.layers.push_back(paint_layer(p, "paint", at));
    return doc;
}

// The book's page as a kind: "8" (as it is), "16" and "32" (precise RGB), "cmyk" (16 bits), "lab" (16 bits), "gray".
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

bool has_layer(const core::Document& doc, const std::string& id) {
    return std::any_of(doc.page(0).layers.begin(), doc.page(0).layers.end(), [&](const core::Layer& l) { return l.id == id; });
}

// The layer's pixel (x, y of the 200 grid) as straight RGBA (any kind: its colours as RGB).
Rgba pixel(const core::Document& doc, const std::string& id, int x, int y) {
    const render::image_ops::Pixels p = render::image_ops::shown_layer_pixels(doc, doc.page(0), layer_of(doc, id));
    const auto rgba = render::image_ops::rgba_of(p);
    const int px = static_cast<int>(std::floor((x + 0.5) * p.width() / kSide)), py = static_cast<int>(std::floor((y + 0.5) * p.height() / kSide));
    return rgba[static_cast<std::size_t>(py) * p.width() + static_cast<std::size_t>(px)];
}

bool near(const Rgba& a, const Rgba& b, float tol) {
    for (std::size_t c = 0; c < 4; ++c)
        if (std::fabs(a[c] - b[c]) > tol) return false;
    return true;
}

QString text(const Rgba& p) { return QStringLiteral("(%1, %2, %3, %4)").arg(p[0]).arg(p[1]).arg(p[2]).arg(p[3]); }

Json stroke(const char* op, const std::vector<std::pair<double, double>>& points, double width_px, const std::string& layer = "paint") {
    Json pts = Json::array();
    for (const auto& [x, y] : points) pts.push_back(Json::array({mm(x), mm(y)}));
    return Json{{"op", op}, {"page", 1}, {"layer_id", layer}, {"points", pts}, {"width_mm", len(width_px)}, {"hardness", 100}};
}

// A smooth colour gradient and a fine deterministic noise (retouch_cmds/tests.rs's texture).
Colour texture(int x, int y) {
    const double n = static_cast<double>(static_cast<std::uint32_t>((x * 73856093) ^ (y * 19349663)) % 1000) / 1000.0 * 0.06 - 0.03;
    return {0.3 + 0.002 * x + n, 0.5 + 0.0015 * y + n, 0.45 + n, 1.0};
}

// The rms difference of two books' layer over a box (RGB).
double rmse(const core::Document& a, const core::Document& b, int x0, int y0, int x1, int y1) {
    double e = 0;
    int n = 0;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const Rgba p = pixel(a, "paint", x, y), q = pixel(b, "paint", x, y);
            for (int c = 0; c < 3; ++c) e += std::pow(p[c] - q[c], 2.0), ++n;
        }
    return std::sqrt(e / n);
}

}  // namespace

class TestRetouch : public QObject {
    Q_OBJECT
private slots:
    // Clone Stamp: exactly the source's pixels under a hard brush, from its offset, on every kind of layer; one change.
    void cloneStampCopiesExactlyOnEveryKind() {
        const core::Document base = book([](int x, int y) { return Colour{x / 200.0, y / 200.0, ((x * 7 + y * 3) % 11) / 11.0, 1.0}; });
        for (const std::string& kind : kinds()) {
            const core::Document doc = as_kind(base, kind);
            Json op = stroke("clone_stamp", {{60, 30}, {80, 30}}, 10);
            op["source_mm"] = Json::array({mm(20), mm(20)});
            const core::ApplyResult done = run_ops(doc, Json::array({op}));
            const Json& r = done.results.at(0);
            QCOMPARE(r["changed"], Json(true));
            QCOMPARE(r["offset_mm"], Json::array({-5.08, -1.27}));
            QVERIFY(r["next_source_mm"].is_null());  // (aligned: the offset is kept)
            QVERIFY(r["box_mm"].is_array());
            for (const int x : {60, 70, 80}) {
                const Rgba got = pixel(done.doc, "paint", x, 30), want = pixel(doc, "paint", x - 40, 20);
                QVERIFY2(near(got, want, 2e-3f), qPrintable(QString::fromStdString(kind) + QStringLiteral(" x %1: ").arg(x) + text(got) + " " + text(want)));
            }
            QCOMPARE(pixel(done.doc, "paint", 70, 50), pixel(doc, "paint", 70, 50));  // (outside the brush)
            QCOMPARE(pixel(done.doc, "paint", 100, 30), pixel(doc, "paint", 100, 30));
        }
    }

    // The source is the pixels before the stroke (a stroke over its own source does not smear); not aligned, the next
    // source is the same point; an offset works too; and the source is required.
    void samplesThePixelsBeforeTheStroke() {
        const core::Document doc = as_kind(book([](int x, int) { return Colour{x / 200.0, 0, 0, 1}; }), "16");
        Json op = stroke("clone_stamp", {{10, 10}, {50, 10}}, 6);
        op["source_mm"] = Json::array({mm(14), mm(10)});
        op["aligned"] = false;
        const core::ApplyResult done = run_ops(doc, Json::array({op}));
        QCOMPARE(done.results.at(0)["next_source_mm"], Json::array({std::round(mm(14) * 1e4) / 1e4, std::round(mm(10) * 1e4) / 1e4}));
        for (const int x : {20, 30, 45})  // (the picture was 8 bits: its values, not x / 200 exactly)
            QVERIFY2(std::fabs(pixel(done.doc, "paint", x, 10)[0] - pixel(doc, "paint", x + 4, 10)[0]) < 1e-4f, qPrintable(QString::number(x)));
        Json bare = stroke("clone_stamp", {{1, 1}}, 4);
        QVERIFY(refusal(doc, bare).starts_with("apply: "));
        Json both = bare;
        both["source_mm"] = Json::array({1, 1});
        both["offset_mm"] = Json::array({1, 1});
        QVERIFY(refusal(doc, both).starts_with("apply: "));
        Json moved = stroke("clone_stamp", {{70, 10}}, 4);
        moved["offset_mm"] = Json::array({len(-60), 0});
        const core::Document again = run(done.doc, moved);
        QVERIFY(std::fabs(pixel(again, "paint", 70, 10)[0] - pixel(doc, "paint", 14, 10)[0]) < 1e-4f);  // (x 10 was cloned from x 14)
    }

    // What is sampled: the layer alone (an empty layer clones nothing), the page as it shows, the layers up to this one
    // (not those above); the opacity.
    void theLayerTheLayersBelowOrThePage() {
        core::Document doc = book([](int x, int) { return x < 100 ? Colour{1, 0, 0, 1} : Colour{1, 1, 1, 1}; });
        core::Layer top;
        top.id = "top";
        top.role = core::LayerRole::User;
        top.kind = core::LayerKind::Raster;
        top.panel_clip = false;
        doc.edit_page(0).layers.push_back(top);
        Json op = stroke("clone_stamp", {{150, 100}}, 8, "top");
        op["source_mm"] = Json::array({mm(50), mm(100)});
        QCOMPARE(pixel(run(doc, op), "top", 150, 100)[3], 0.0f);
        op["sample"] = "all";
        const Rgba all = pixel(run(doc, op), "top", 150, 100);
        QVERIFY2(all[0] > 0.99f && all[1] < 0.01f && all[3] > 0.99f, qPrintable(text(all)));
        // a green layer over "top": the page shows green there, the layers up to "top" red
        doc.edit_page(0).layers.push_back(paint_layer(doc.edit_page(0), "cover", [](int x, int) { return x < 100 ? Colour{0, 1, 0, 1} : Colour{0, 0, 0, 0}; }));
        QVERIFY(pixel(run(doc, op), "top", 150, 100)[1] > 0.99f);
        op["sample"] = "current_and_below";
        op["opacity"] = 50;
        const Rgba below = pixel(run(doc, op), "top", 150, 100);
        QVERIFY2(below[0] > 0.99f && below[1] < 0.01f && std::fabs(below[3] - 0.5f) < 0.02f, qPrintable(text(below)));
        op["sample"] = "layers";
        QVERIFY(refusal(doc, op).starts_with("apply: "));
    }

    // Only inside the selection (a CMYK page): cloned blue inside, the yellow outside untouched.
    void theSelectionLimitsIt() {
        const core::Document doc = as_kind(book([](int x, int) { return x < 100 ? Colour{0, 0, 1, 1} : Colour{1, 1, 0, 1}; }), "cmyk");
        Json op = stroke("clone_stamp", {{110, 100}, {170, 100}}, 8);
        op["offset_mm"] = Json::array({len(-80), 0});
        op["area"] = Json{{"rect", Json::array({len(120), 0, len(20), kInch})}};
        const core::Document done = run(doc, op);
        const Rgba inside = pixel(done, "paint", 130, 100), outside = pixel(done, "paint", 150, 100);
        QVERIFY2(inside[2] > inside[0] + 0.2f, qPrintable(text(inside)));
        QCOMPARE(outside, pixel(doc, "paint", 150, 100));
        QCOMPARE(pixel(done, "paint", 115, 100), pixel(doc, "paint", 115, 100));
    }

    // The clone source's transform: mirrored left to right, the source read backwards.
    void theSourcesTransform() {
        const core::Document doc = as_kind(book([](int x, int) { return Colour{x / 200.0, 0, 0, 1}; }), "16");
        Json op = stroke("clone_stamp", {{140, 100}, {160, 100}}, 6);
        op["source_mm"] = Json::array({mm(50), mm(100)});
        op["transform"] = Json{{"flip_h", true}};
        const core::Document done = run(doc, op);
        // (the anchor at x 140 reads x 50; x 150, ten to the right, reads ten to the left)
        QVERIFY2(std::fabs(pixel(done, "paint", 150, 100)[0] - 40 / 200.0f) < 3e-3f, qPrintable(text(pixel(done, "paint", 150, 100))));
        op["transform"] = Json{{"scale", Json::array({200, 100})}};
        const core::Document wide = run(doc, op);
        // (twice as wide on the page: ten to the right reads five to the right)
        QVERIFY2(std::fabs(pixel(wide, "paint", 150, 100)[0] - 55 / 200.0f) < 3e-3f, qPrintable(text(pixel(wide, "paint", 150, 100))));
    }

    // Healing Brush: the healed disc takes the destination's colour and keeps the source's texture.
    void healingTakesTheDestinationsColourAndTheSourcesTexture() {
        const core::Document base = book([](int x, int) {
            if (x < 100) {
                const double t = (x / 3) % 2 == 0 ? 0.08 : -0.08;
                return Colour{0.2 + t, 0.25 + t, 0.5 + t, 1};
            }
            return Colour{0.9, 0.6, 0.3, 1};
        });
        for (const std::string& kind : {std::string("8"), std::string("32"), std::string("cmyk")}) {
            const core::Document doc = as_kind(base, kind);
            Json op = stroke("healing_brush", {{150, 100}}, 30);
            op["source_mm"] = Json::array({mm(50), mm(100)});
            const core::Document done = run(doc, op);
            std::array<double, 3> mean{};
            std::vector<Rgba> seen;
            for (int dy = -8; dy <= 8; ++dy)
                for (int dx = -8; dx <= 8; ++dx) seen.push_back(pixel(done, "paint", 150 + dx, 100 + dy));
            const Rgba dest = pixel(doc, "paint", 150, 100);
            for (const Rgba& p : seen)
                for (int c = 0; c < 3; ++c) mean[c] += p[c] / seen.size();
            for (int c = 0; c < 3; ++c)
                QVERIFY2(std::fabs(mean[c] - dest[c]) < 0.06, qPrintable(QString::fromStdString(kind) + QStringLiteral(" %1: %2 / %3").arg(c).arg(mean[c]).arg(dest[c])));
            double var = 0;
            for (const Rgba& p : seen) var += std::pow(p[1] - mean[1], 2.0) / seen.size();
            QVERIFY2(var > 0.003, qPrintable(QString::fromStdString(kind) + QStringLiteral(" variance %1").arg(var)));
            if (kind != "cmyk") {
                const float a = pixel(done, "paint", 150, 100)[1] - pixel(done, "paint", 149, 100)[1];
                const float b = pixel(doc, "paint", 50, 100)[1] - pixel(doc, "paint", 49, 100)[1];
                QVERIFY2(std::fabs(a - b) < 0.03f, qPrintable(QStringLiteral("%1 vs %2").arg(a).arg(b)));
            }
            QCOMPARE(pixel(done, "paint", 195, 5), pixel(doc, "paint", 195, 5));
        }
    }

    // Spot Healing Brush: each way removes a dark blemish from a texture; the rest untouched; the same again.
    void spotHealingRemovesABlemish() {
        const core::Document clean8 = book(texture);
        const core::Document dirty8 = book([](int x, int y) { return std::hypot(x - 100.0, y - 100.0) < 6 ? Colour{0.05, 0.03, 0.02, 1} : texture(x, y); });
        const std::vector<std::pair<std::string, std::string>> cases{{"8", "content_aware"},    {"16", "content_aware"}, {"32", "content_aware"},
                                                                     {"8", "proximity_match"},  {"16", "create_texture"}, {"cmyk", "content_aware"},
                                                                     {"lab", "content_aware"},  {"gray", "content_aware"}};
        for (const auto& [kind, type] : cases) {
            const core::Document clean = as_kind(clean8, kind), dirty = as_kind(dirty8, kind);
            Json op = stroke("spot_healing", {{100, 100}}, 20);
            op["type"] = type;
            const core::ApplyResult done = run_ops(dirty, Json::array({op}));
            QCOMPARE(done.results.at(0)["changed"], Json(true));
            const double before = rmse(dirty, clean, 92, 92, 108, 108), after = rmse(done.doc, clean, 92, 92, 108, 108);
            QVERIFY2(after < 0.05 && after < before * 0.2, qPrintable(QString::fromStdString(kind + " " + type) + QStringLiteral(": %1 → %2").arg(before).arg(after)));
            QCOMPARE(pixel(done.doc, "paint", 10, 10), pixel(dirty, "paint", 10, 10));
            if (kind == "8") QCOMPARE(*layer_of(run(dirty, op), "paint").raster_png, *layer_of(done.doc, "paint").raster_png);
        }
        Json bad = stroke("spot_healing", {{100, 100}}, 20);
        bad["type"] = "magic";
        QVERIFY(refusal(dirty8, bad).starts_with("apply: "));
    }

    // History Brush: the pixels of a state the book holds painted back where the brush goes (8 bits, 32 bits in tiles,
    // CMYK); another layer's pixels, a missing asset and an unsaved book refused.
    void historyBrushPaintsBack() {
        QTemporaryDir tmp;
        const std::filesystem::path dir(tmp.path().toStdString());
        storage::AssetStore store(dir);
        const core::Document white = book([](int, int) { return Colour{1, 1, 1, 1}; });
        const core::Document red = book([](int, int) { return Colour{1, 0, 0, 1}; });
        for (const std::string& kind : {std::string("8"), std::string("32"), std::string("cmyk")}) {
            core::Document was = as_kind(white, kind), now = as_kind(red, kind);
            now.asset_dir = dir;
            const core::Layer before = layer_of(was, "paint");  // (a copy: the layer outlives the call)
            Json from;
            if (before.color_raster) {
                std::vector<std::string> refs;
                for (const std::string& tile : core::color_tiles(*before.color_raster)) refs.push_back(store.put_bytes(tile, core::kColorRasterSuffix));
                from = core::tiled_metadata(core::ColorRasterView(*before.color_raster), refs, core::color_raster_space(was.page(0).rgb));
            } else if (before.ink_raster) {
                from = store.put_bytes(*before.ink_raster, core::kInkRasterSuffix);
            } else {
                from = store.put_bytes(*before.raster_png, ".png");
            }
            Json op = stroke("history_brush", {{20, 100}, {60, 100}}, 12);
            op["from"] = from;
            const core::ApplyResult done = run_ops(now, Json::array({op}));
            QCOMPARE(done.results.at(0)["changed"], Json(true));
            QVERIFY2(near(pixel(done.doc, "paint", 40, 100), pixel(was, "paint", 40, 100), 2e-3f), qPrintable(QString::fromStdString(kind)));
            QCOMPARE(pixel(done.doc, "paint", 150, 100), pixel(now, "paint", 150, 100));
            if (kind == "8") {
                core::Document unsaved = now;
                unsaved.asset_dir.reset();
                QVERIFY(refusal(unsaved, op).starts_with("apply: "));
                Json missing = op;
                missing["from"] = "sha256:" + std::string(64, 'a');
                QVERIFY(refusal(now, missing).starts_with("apply: "));
                Json other = op;
                other["from"] = store.put_bytes(render::write_png(picture([](int, int) { return Colour{0, 0, 0, 1}; }).resize(render::Size{20, 20})), ".png");
                QVERIFY(refusal(now, other).find("cannot be painted back") != std::string::npos);
                Json none = op;
                none.erase("from");
                QVERIFY(refusal(now, none).starts_with("apply: "));
            }
        }
    }

    // Content-Aware Fill: the selection filled from around it on the layer, on a new layer (the fill alone) or on a copy;
    // the sampling area kept to; the same seed, the same pixels; the refusals.
    void contentAwareFill() {
        const core::Document clean8 = book([](int x, int) { return (x / 6) % 2 == 0 ? Colour{0.2, 0.3, 0.4, 1} : Colour{0.8, 0.7, 0.6, 1}; });
        const core::Document dirty8 = book([](int x, int y) {
            if (std::hypot(x - 100.0, y - 100.0) < 8) return Colour{0, 0, 0, 1};
            return (x / 6) % 2 == 0 ? Colour{0.2, 0.3, 0.4, 1} : Colour{0.8, 0.7, 0.6, 1};
        });
        const Json area{{"rect", Json::array({len(90), len(90), len(20), len(20)})}};
        for (const std::string& kind : {std::string("8"), std::string("16"), std::string("cmyk")}) {
            const core::Document dirty = as_kind(dirty8, kind), clean = as_kind(clean8, kind);
            const Json op{{"op", "content_aware_fill"}, {"page", 1}, {"layer_id", "paint"}, {"area", area}};
            const core::ApplyResult done = run_ops(dirty, Json::array({op}));
            const Json& r = done.results.at(0);
            QCOMPARE(r["layer"], Json("paint"));
            QVERIFY(r["filled"].get<std::int64_t>() >= 400);
            QVERIFY(r["window_mm"].is_array() && r["window_mm"].size() == 4);
            // (stripes made again: light and dark both in the hole, near the clean texture's mean)
            int light = 0, dark = 0;
            for (int x = 92; x < 108; ++x) (pixel(done.doc, "paint", x, 100)[0] > 0.5f ? light : dark)++;
            QVERIFY2(light > 3 && dark > 3, qPrintable(QString::fromStdString(kind) + QStringLiteral(" %1/%2").arg(light).arg(dark)));
            QVERIFY2(rmse(done.doc, clean, 92, 92, 108, 108) < rmse(dirty, clean, 92, 92, 108, 108) * 0.6,
                     qPrintable(QString::fromStdString(kind) + QStringLiteral(" %1").arg(rmse(done.doc, clean, 92, 92, 108, 108))));
            QCOMPARE(pixel(done.doc, "paint", 10, 10), pixel(dirty, "paint", 10, 10));
            if (kind == "8") QCOMPARE(*layer_of(run(dirty, op), "paint").raster_png, *layer_of(done.doc, "paint").raster_png);
            // a new layer: the fill alone, above; the layer as it was
            Json made = op;
            made["output"] = "new";
            made["new_id"] = "caf";
            const core::ApplyResult fresh = run_ops(dirty, Json::array({made}));
            QCOMPARE(fresh.results.at(0)["layer"], Json("caf"));
            QCOMPARE(fresh.doc.page(0).layers.back().id, std::string("caf"));
            QCOMPARE(pixel(fresh.doc, "paint", 100, 100), pixel(dirty, "paint", 100, 100));
            QVERIFY(pixel(fresh.doc, "caf", 100, 100)[3] > 0.99f);
            QCOMPARE(pixel(fresh.doc, "caf", 10, 10)[3], 0.0f);
            // a copy, filled; the layer as it was
            Json copy = op;
            copy["output"] = "duplicate";
            copy["new_id"] = "twin";
            const core::Document twin = run(dirty, copy);
            QVERIFY(has_layer(twin, "twin"));
            QCOMPARE(pixel(twin, "paint", 100, 100), pixel(dirty, "paint", 100, 100));
            QVERIFY(pixel(twin, "twin", 100, 100)[0] > 0.1f);
        }
        // the sampling area: from the right half only (light), the hole filled light
        const core::Document halves = book([](int x, int) { return x < 100 ? Colour{0.1, 0.1, 0.1, 1} : Colour{0.9, 0.9, 0.9, 1}; });
        Json custom{{"op", "content_aware_fill"},
                    {"page", 1},
                    {"layer_id", "paint"},
                    {"area", Json{{"rect", Json::array({len(85), len(90), len(30), len(20)})}}},
                    {"sampling", "custom"},
                    {"color_adaptation", "none"},
                    {"source_area", Json{{"rect", Json::array({len(130), 0, len(70), kInch})}}}};
        const core::Document lit = run(halves, custom);
        for (const int x : {88, 100, 112}) QVERIFY2(pixel(lit, "paint", x, 100)[0] > 0.8f, qPrintable(QString::number(x)));
        // refusals
        const Json base{{"op", "content_aware_fill"}, {"page", 1}, {"layer_id", "paint"}, {"area", area}};
        for (const auto& [key, value] : std::vector<std::pair<std::string, Json>>{{"sampling", "magic"},
                                                                                   {"output", "elsewhere"},
                                                                                   {"seed", -1},
                                                                                   {"color_adaptation", "loud"},
                                                                                   {"rotation_adaptation", 45},
                                                                                   {"margin_mm", -1}}) {
            Json bad = base;
            bad[key] = value;
            QVERIFY2(refusal(dirty8, bad).starts_with("apply: "), key.c_str());
        }
        Json no_area = base;
        no_area.erase("area");
        QVERIFY(refusal(dirty8, no_area).starts_with("apply: "));
        Json custom_bare = base;
        custom_bare["sampling"] = "custom";
        QVERIFY(refusal(dirty8, custom_bare).starts_with("apply: "));
    }

    // A protected transparency: a clear pixel stays clear under the brush, a coloured one takes the clone.
    void aProtectedTransparencyStays() {
        core::Document doc = book([](int x, int) { return x < 100 ? Colour{1, 0, 0, 1} : Colour{0, 0, 0, 0}; });
        doc.edit_page(0).layers.back().lock_alpha = true;
        doc.edit_page(0).layers.back().raster_png = std::make_shared<const std::string>(
            render::write_png(picture([](int x, int y) { return x < 100 || y < 50 ? Colour{x < 100 ? 1.0 : 0.0, 0, x < 100 ? 0.0 : 1.0, 1} : Colour{0, 0, 0, 0}; })));
        Json op = stroke("clone_stamp", {{150, 40}, {150, 120}}, 8);
        op["offset_mm"] = Json::array({len(-100), 0});
        const core::Document done = run(doc, op);
        QCOMPARE(pixel(done, "paint", 150, 100)[3], 0.0f);
        const Rgba coloured = pixel(done, "paint", 150, 40);
        QVERIFY2(coloured[0] > 0.99f && coloured[3] > 0.99f, qPrintable(text(coloured)));
    }

    // The refusals every brush shares, and a stroke off the layer changes nothing.
    void refusals() {
        const core::Document doc = book([](int, int) { return Colour{1, 1, 1, 1}; });
        Json good = stroke("spot_healing", {{100, 100}}, 10);
        for (const auto& [key, value] : std::vector<std::pair<std::string, Json>>{
                 {"points", Json::array()}, {"points", "here"}, {"width_mm", 0}, {"width_mm", "wide"}, {"blend", "dissolve"}, {"hardness", "soft"}}) {
            Json bad = good;
            bad[key] = value;
            QVERIFY2(refusal(doc, bad).starts_with("apply: "), (key + " " + value.dump()).c_str());
        }
        Json no_layer = good;
        no_layer.erase("layer_id");
        QVERIFY(refusal(doc, no_layer).starts_with("apply: "));
        no_layer["layer_id"] = "nothing";
        QVERIFY(refusal(doc, no_layer).starts_with("apply: "));
        core::Document locked = doc;
        locked.edit_page(0).layers.back().locked = true;
        QVERIFY(refusal(locked, good).find("locked") != std::string::npos);
        core::Document folder = doc;
        folder.edit_page(0).layers.back().kind = core::LayerKind::Folder;
        QVERIFY(refusal(folder, good).find("not a paint layer") != std::string::npos);
        Json off = stroke("spot_healing", {{-500, -500}}, 10);
        const core::ApplyResult none = run_ops(doc, Json::array({off}));
        QCOMPARE(none.results.at(0)["changed"], Json(false));
        QCOMPARE(*layer_of(none.doc, "paint").raster_png, *layer_of(doc, "paint").raster_png);
    }
};

QTEST_MAIN(TestRetouch)
#include "test_retouch.moc"
