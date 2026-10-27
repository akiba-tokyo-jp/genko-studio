// PC4 E6 (layer-styles@1, SPEC.md COLOR-09「レイヤースタイル」): a layer's style without Python — each effect of
// PhotoCraft v0.2.0's compose/effects.rs drawn where it belongs on the 8-bit page (a drop shadow beyond the shape down the
// light, a stroke outside it, the overlays, inner shadow and glows, satin, a bevel's lit and shaded sides, a pattern overlay
// from a picture put into the book), the fill opacity and Blend If, the same style on 16-bit, 32-bit, CMYK, Lab and grey
// pages; the ops (an effect merged or added, scaled, hidden, the global light, レイヤーを作成, merged down); a book without
// styles drawn and written as before; saved, opened, undone and done again; written to a PSD (lfx2, iOpa, the blending
// ranges) and read back by this build and by psd-tools.

#include <QDir>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/base64.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/layer_styles.hpp"
#include "core/paths.hpp"
#include "formats/export.hpp"
#include "render/image.hpp"
#include "render/image_ops.hpp"
#include "render/import_psd.hpp"
#include "render/layer_styles.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/psd_effects.hpp"
#include "render/raster.hpp"
#include "storage/fsutil.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/transaction.hpp"
#include "storage/undo.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
using render::Size;
using render::image_ops::Rgba;
namespace fs = std::filesystem;

namespace {

constexpr double kInch = 25.4;
constexpr int kSide = 400;  // (2 inches at the working 200 dpi)

core::ApplyResult run_ops(const core::Document& doc, const Json& ops) {
    return core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor("human:test"));
}
core::Document run(const core::Document& doc, const Json& op) { return run_ops(doc, Json::array({op})).doc; }

std::string refusal(const core::Document& doc, const Json& ops) {
    try {
        run_ops(doc, ops);
    } catch (const core::ApplyError& error) {
        return error.code() + ": " + error.what();
    }
    return {};
}

// A page of one 8-bit paint layer "paint": a grey (200) square from 12.7 to 38.1 mm both ways, clear around it.
core::Document square_book() {
    core::Document doc = core::new_episode("スタイル", core::Num(1), 1, core::PageSpec::custom(2 * kInch, 2 * kInch, 40, 40, 1, 2, 2, 2, 2, 200, "color"));
    doc.edit_page(0).frames.clear();
    doc.edit_page(0).numero = false;
    auto& p = doc.edit_page(0);
    p.layers.clear();
    core::Layer layer;
    layer.id = "paint";
    layer.title = "四角";
    layer.role = core::LayerRole::User;
    layer.kind = core::LayerKind::Raster;
    layer.panel_clip = false;
    std::string bytes;
    for (int y = 0; y < kSide; ++y)
        for (int x = 0; x < kSide; ++x)
            bytes += (x >= 100 && x < 300 && y >= 100 && y < 300) ? std::string("\xC8\xC8\xC8\xFF", 4) : std::string(4, '\0');
    render::raster::save_raster(p, layer, render::Image::frombytes("RGBA", Size{kSide, kSide}, bytes));
    p.layers.push_back(layer);
    return doc;
}

core::Document as_kind(const core::Document& doc, const std::string& kind) {
    if (kind == "8") return doc;
    if (kind == "16" || kind == "32") return run(doc, Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", kind == "16" ? 16 : 32}});
    if (kind == "cmyk") return run(doc, Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}, {"depth", 16}});
    if (kind == "lab") return run(doc, Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "lab"}, {"depth", 16}});
    return run(doc, Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "gray"}});
}

core::Document styled(const core::Document& doc, const Json& style, const std::string& id = "paint") {
    return run(doc, Json{{"op", "set_layer_style"}, {"page", 1}, {"id", id}, {"style", style}});
}

Json one(const Json& effect) { return Json{{"effects", Json::array({effect})}}; }

// The page's colours as it shows (an ink page's as its inks mean them), straight 0..1, at its working 200 dpi.
std::vector<Rgba> shown(const core::Document& doc) { return render::image_ops::rgba_of(render::image_ops::page_pixels(doc, doc.page(0))); }

Rgba at(const std::vector<Rgba>& px, double x_mm, double y_mm) {
    const double k = kSide / (2 * kInch);
    const std::size_t x = std::size_t(std::floor(x_mm * k)), y = std::size_t(std::floor(y_mm * k));
    return px.at(y * kSide + x);
}

float light(const Rgba& c) { return (c[0] + c[1] + c[2]) / 3; }

bool near(const Rgba& c, float r, float g, float b, float tolerance = 0.03f) {
    return std::abs(c[0] - r) <= tolerance && std::abs(c[1] - g) <= tolerance && std::abs(c[2] - b) <= tolerance;
}

QString told(const Rgba& c) { return QStringLiteral("(%1, %2, %3, %4)").arg(c[0]).arg(c[1]).arg(c[2]).arg(c[3]); }

std::string proof_bytes(const core::Document& doc) { return render::render_page(doc.page(0), 200, render::proof_options(), &doc).image.tobytes(); }

bool has_feature(const core::Document& doc, const std::string& name) {
    return std::find(doc.features.begin(), doc.features.end(), name) != doc.features.end();
}

void write_book(const core::Document& doc, const fs::path& dir) {
    storage::ProjectLock lock(dir, "human:test");
    lock.try_acquire();
    storage::SaveRequest request;
    request.actor = "human:test";
    request.ops = Json::array();
    storage::Saver(lock).save(doc, request);
}

void save_next(const core::Document& doc, const fs::path& dir, const Json& ops) {
    storage::ProjectLock lock(dir, "human:test");
    lock.try_acquire();
    storage::SaveRequest request;
    request.actor = "human:test";
    request.base_revision = storage::read_disk_state(dir).revision;
    request.ops = ops;
    storage::Saver(lock).save(doc, request);
}

std::string read_text(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// A 2 × 2 picture of red and blue as add_pattern takes it.
Json checker() {
    std::string bytes;
    for (int i = 0; i < 4; ++i) bytes += (i == 0 || i == 3) ? std::string("\xFF\x00\x00\xFF", 4) : std::string("\x00\x00\xFF\xFF", 4);
    const std::string png = render::write_png(render::Image::frombytes("RGBA", Size{2, 2}, bytes));
    return Json{{"name", "チェック"}, {"png", core::b64encode(png)}, {"mode", "rgb"}, {"depth", 8}, {"dpi", 300}};
}

}  // namespace

class TestLayerStyles : public QObject {
    Q_OBJECT

private slots:
    // a layer without a style: the page drawn as before and the book written without layer-styles@1; a style taken off
    // again: the same bytes as never styled
    void withoutStylesAsBefore() {
        const core::Document doc = square_book();
        const std::string before = proof_bytes(doc);
        const core::Document same = styled(doc, nullptr);
        QVERIFY(!same.page(0).layers[0].style);
        QCOMPARE(proof_bytes(same), before);
        QVERIFY(!has_feature(same, std::string(core::kLayerStylesFeature)));
        const core::Document on = styled(doc, one(Json{{"kind", "drop_shadow"}}));
        QVERIFY(has_feature(on, std::string(core::kLayerStylesFeature)));
        QVERIFY(proof_bytes(on) != before);
        const core::Document off = styled(on, nullptr);
        QCOMPARE(proof_bytes(off), before);
        QVERIFY(!has_feature(off, std::string(core::kLayerStylesFeature)));
        // a style of nothing ({"effects": []}): no style
        QVERIFY(!styled(doc, Json{{"effects", Json::array()}}).page(0).layers[0].style);
        // written: no style, no global light, no feature
        QTemporaryDir scratch;
        const fs::path dir = core::path_from_utf8(scratch.path().toStdString()) / "plain.genko";
        write_book(off, dir);
        std::string written;
        for (const auto& entry : fs::recursive_directory_iterator(dir))
            if (entry.path().extension() == ".json") written += read_text(entry.path());
        QVERIFY(written.find("layer-styles@1") == std::string::npos);
        QVERIFY(written.find("global_light") == std::string::npos);
        QVERIFY(written.find("\"style\"") == std::string::npos);
    }

    // each effect where it belongs (the global light 120°: the light from the upper left)
    void eachEffect() {
        const core::Document doc = square_book();
        const std::vector<Rgba> plain = shown(doc);
        const float grey = 200.0f / 255.0f;
        QVERIFY2(near(at(plain, 25.4, 25.4), grey, grey, grey), qPrintable(told(at(plain, 25.4, 25.4))));
        QVERIFY(near(at(plain, 39.2, 30), 1, 1, 1));
        // ドロップシャドウ: 3 mm down the light (1.5 mm right, 2.6 mm down), hard
        auto px = shown(styled(doc, one(Json{{"kind", "drop_shadow"}, {"blend", "normal"}, {"opacity", 1}, {"distance_mm", 3}, {"size_mm", 0}})));
        QVERIFY2(near(at(px, 39.2, 30), 0, 0, 0), qPrintable(told(at(px, 39.2, 30))));
        QVERIFY2(near(at(px, 11.5, 25.4), 1, 1, 1), qPrintable(told(at(px, 11.5, 25.4))));
        QVERIFY(near(at(px, 25.4, 25.4), grey, grey, grey));
        // 境界線: outside, 1 mm, red
        px = shown(styled(doc, one(Json{{"kind", "stroke"}, {"size_mm", 1}, {"rgb", Json::array({255, 0, 0})}})));
        QVERIFY2(near(at(px, 38.6, 25.4), 1, 0, 0), qPrintable(told(at(px, 38.6, 25.4))));
        QVERIFY(near(at(px, 25.4, 25.4), grey, grey, grey));
        QVERIFY(near(at(px, 40.0, 25.4), 1, 1, 1));
        // inside: within the shape
        px = shown(styled(doc, one(Json{{"kind", "stroke"}, {"size_mm", 1}, {"position", "inside"}, {"rgb", Json::array({255, 0, 0})}})));
        QVERIFY2(near(at(px, 13.2, 25.4), 1, 0, 0), qPrintable(told(at(px, 13.2, 25.4))));
        QVERIFY(near(at(px, 38.6, 25.4), 1, 1, 1));
        // カラーオーバーレイ
        px = shown(styled(doc, one(Json{{"kind", "color_overlay"}, {"rgb", Json::array({0, 0, 255})}})));
        QVERIFY2(near(at(px, 25.4, 25.4), 0, 0, 1), qPrintable(told(at(px, 25.4, 25.4))));
        QVERIFY(near(at(px, 5, 5), 1, 1, 1));
        // シャドウ（内側）: 2 mm, hard: the inside of the upper left edges
        px = shown(styled(doc, one(Json{{"kind", "inner_shadow"}, {"blend", "normal"}, {"opacity", 1}, {"distance_mm", 2}, {"size_mm", 0}})));
        QVERIFY2(near(at(px, 13.2, 25.4), 0, 0, 0), qPrintable(told(at(px, 13.2, 25.4))));
        QVERIFY(near(at(px, 37.6, 25.4), grey, grey, grey));
        // 光彩（外側）・（内側）: red in the normal mode
        px = shown(styled(doc, one(Json{{"kind", "outer_glow"}, {"blend", "normal"}, {"opacity", 1}, {"rgb", Json::array({255, 0, 0})}, {"size_mm", 1}, {"spread", 1}})));
        QVERIFY2(at(px, 38.6, 25.4)[0] > 0.9f && at(px, 38.6, 25.4)[1] < 0.2f, qPrintable(told(at(px, 38.6, 25.4))));
        QVERIFY(near(at(px, 45, 25.4), 1, 1, 1));
        px = shown(styled(doc, one(Json{{"kind", "inner_glow"}, {"blend", "normal"}, {"opacity", 1}, {"rgb", Json::array({255, 0, 0})}, {"size_mm", 1}, {"choke", 1}})));
        QVERIFY2(at(px, 13.0, 25.4)[0] > 0.9f && at(px, 13.0, 25.4)[1] < 0.2f, qPrintable(told(at(px, 13.0, 25.4))));
        QVERIFY(near(at(px, 25.4, 25.4), grey, grey, grey));
        // グラデーションオーバーレイ: black to white across
        px = shown(styled(doc, one(Json{{"kind", "gradient_overlay"}, {"gradient", Json{{"stops", Json::parse("[[0, [0, 0, 0]], [1, [255, 255, 255]]]")}, {"angle", 0}}}})));
        QVERIFY2(light(at(px, 14, 25.4)) + 0.5f < light(at(px, 37, 25.4)), qPrintable(told(at(px, 14, 25.4)) + told(at(px, 37, 25.4))));
        // サテン: the inside changed
        QVERIFY(shown(styled(doc, one(Json{{"kind", "satin"}, {"opacity", 1}, {"blend", "normal"}}))) != plain);
        // ベベルとエンボス: the lit upper left side lighter than the shaded lower right one
        px = shown(styled(doc, one(Json{{"kind", "bevel"}, {"size_mm", 1}, {"depth", 2}})));
        QVERIFY2(light(at(px, 13.2, 25.4)) > light(at(px, 37.6, 25.4)) + 0.2f, qPrintable(told(at(px, 13.2, 25.4)) + told(at(px, 37.6, 25.4))));
        // パターンオーバーレイ: a picture given whole is put into the book; the inside red and blue
        const core::Document patterned = styled(doc, one(Json{{"kind", "pattern_overlay"}, {"pattern", checker()}}));
        QCOMPARE(patterned.patterns.size(), std::size_t(1));
        QCOMPARE((*patterned.page(0).layers[0].style)["effects"][0]["pattern"]["id"], Json(patterned.patterns[0].id));
        px = shown(patterned);
        const Rgba c = at(px, 25.4, 25.4);
        QVERIFY2(near(c, 1, 0, 0) || near(c, 0, 0, 1), qPrintable(told(c)));
        QVERIFY(near(at(px, 5, 5), 1, 1, 1));
        // (a pattern a style uses: delete_pattern refuses it, as a fill's)
        QVERIFY(!refusal(patterned, Json::array({Json{{"op", "delete_pattern"}, {"id", patterned.patterns[0].id}}})).empty());
        // the effects switched off together: as without
        Json hidden = one(Json{{"kind", "color_overlay"}, {"rgb", Json::array({0, 0, 255})}});
        hidden["enabled"] = false;
        QCOMPARE(shown(styled(doc, hidden)), plain);
    }

    // 塗りの不透明度: the layer's own pixels and its inner effects faded, the outer ones not; ブレンド条件: this layer's grey
    // above the range hidden
    void fillAndBlendIf() {
        const core::Document doc = square_book();
        Json style = one(Json{{"kind", "stroke"}, {"size_mm", 1}, {"rgb", Json::array({255, 0, 0})}});
        style["fill_opacity"] = 0;
        auto px = shown(styled(doc, style));
        QVERIFY2(near(at(px, 25.4, 25.4), 1, 1, 1), qPrintable(told(at(px, 25.4, 25.4))));
        QVERIFY(near(at(px, 38.6, 25.4), 1, 0, 0));
        Json blend_if{{"effects", Json::array()}, {"blend_if", Json::array({Json{{"channel", "gray"}, {"this", Json::array({0, 0, 150, 150})}, {"under", Json::array({0, 0, 255, 255})}}})}};
        const core::Document hid = styled(doc, blend_if);
        QVERIFY(hid.page(0).layers[0].style.has_value());
        px = shown(hid);
        QVERIFY2(near(at(px, 25.4, 25.4), 1, 1, 1), qPrintable(told(at(px, 25.4, 25.4))));
        // a range that hides nothing: no Blend If kept
        Json open{{"effects", Json::array()}, {"blend_if", Json::array({Json{{"channel", "gray"}, {"this", Json::array({0, 0, 255, 255})}, {"under", Json::array({0, 0, 255, 255})}}})}};
        QVERIFY(!styled(doc, open).page(0).layers[0].style);
    }

    // the same style on each kind of page: the overlay's colour inside, the shadow below and right of the shape
    void onEveryKindOfPage() {
        const Json style{{"effects", Json::array({Json{{"kind", "color_overlay"}, {"rgb", Json::array({0, 0, 255})}},
                                                  Json{{"kind", "drop_shadow"}, {"blend", "normal"}, {"opacity", 1}, {"distance_mm", 3}, {"size_mm", 0}}})}};
        for (const std::string kind : {"16", "32", "cmyk", "lab", "gray"}) {
            const core::Document doc = as_kind(square_book(), kind);
            const std::vector<Rgba> plain = shown(doc);
            const std::vector<Rgba> px = shown(styled(doc, style));
            const Rgba inside = at(px, 25.4, 25.4), was = at(plain, 25.4, 25.4);
            if (kind == "gray") {
                QVERIFY2(light(inside) + 0.3f < light(was), qPrintable(QString::fromStdString(kind) + told(inside)));
            } else {
                QVERIFY2(inside[2] > inside[0] + 0.4f, qPrintable(QString::fromStdString(kind) + told(inside)));
            }
            QVERIFY2(light(at(px, 39.2, 30)) < 0.2f, qPrintable(QString::fromStdString(kind) + told(at(px, 39.2, 30))));
            QVERIFY2(light(at(px, 11.5, 25.4)) > 0.9f, qPrintable(QString::fromStdString(kind) + told(at(px, 11.5, 25.4))));
        }
    }

    // the ops: an effect of a kind merged into the layer's own (add: another), scaled, hidden and shown, the global light,
    // レイヤーを作成, a merge down drawn as before; a folder refused; the journal's pictures kept small
    void theOps() {
        const core::Document doc = square_book();
        core::Document d = run(doc, Json{{"op", "add_layer_effect"}, {"page", 1}, {"id", "paint"}, {"effect", Json{{"kind", "drop_shadow"}, {"size_mm", 1}}}});
        d = run(d, Json{{"op", "add_layer_effect"}, {"page", 1}, {"id", "paint"}, {"effect", Json{{"kind", "drop_shadow"}, {"distance_mm", 2}}}});
        Json effects = (*d.page(0).layers[0].style)["effects"];
        QCOMPARE(effects.size(), std::size_t(1));
        QCOMPARE(effects[0]["size_mm"], Json(1));
        QCOMPARE(effects[0]["distance_mm"], Json(2));
        d = run(d, Json{{"op", "add_layer_effect"}, {"page", 1}, {"id", "paint"}, {"effect", Json{{"kind", "drop_shadow"}}}, {"add", true}});
        QCOMPARE((*d.page(0).layers[0].style)["effects"].size(), std::size_t(2));
        d = run(d, Json{{"op", "scale_layer_style"}, {"page", 1}, {"id", "paint"}, {"scale", 200}});
        QCOMPARE((*d.page(0).layers[0].style)["effects"][0]["size_mm"], Json(2));
        QCOMPARE((*d.page(0).layers[0].style)["effects"][0]["distance_mm"], Json(4));
        const std::vector<Rgba> with = shown(d);
        const core::Document hidden = run(d, Json{{"op", "show_layer_effects"}, {"page", 1}, {"show", false}});
        QCOMPARE((*hidden.page(0).layers[0].style)["enabled"], Json(false));
        QCOMPARE(shown(hidden), shown(doc));
        QCOMPARE(shown(run(hidden, Json{{"op", "show_layer_effects"}, {"page", 1}, {"show", true}})), with);
        // the global light: the book's, the shadows moved with it
        const core::Document lit = run(d, Json{{"op", "set_global_light"}, {"angle", 30}, {"altitude", 45}});
        QCOMPARE(core::global_light(lit), (std::array<double, 2>{30, 45}));
        QVERIFY(has_feature(lit, std::string(core::kLayerStylesFeature)));
        QVERIFY(shown(lit) != with);
        QCOMPARE(core::global_light(run(lit, Json{{"op", "set_global_light"}})), (std::array<double, 2>{120, 30}));
        // レイヤーを作成: a shadow's layer below (not clipped), an overlay's above (clipped); the page as it was
        const Json two{{"effects", Json::array({Json{{"kind", "color_overlay"}, {"rgb", Json::array({0, 0, 255})}, {"opacity", 0.5}},
                                                Json{{"kind", "drop_shadow"}, {"blend", "normal"}, {"opacity", 1}, {"distance_mm", 3}, {"size_mm", 0}}})}};
        const core::Document two_fx = styled(doc, two);
        const core::Document made = run(two_fx, Json{{"op", "layer_style_to_layers"}, {"page", 1}, {"id", "paint"}});
        const auto& layers = made.page(0).layers;
        QCOMPARE(layers.size(), std::size_t(3));
        QVERIFY(!layers[1].style);
        QCOMPARE(layers[1].id, std::string("paint"));
        QVERIFY(!layers[0].clip);
        QVERIFY(layers[2].clip);
        QCOMPARE(layers[2].opacity, 0.5);
        const auto styled_px = shown(two_fx), made_px = shown(made);
        for (const auto& [x, y] : {std::pair{39.2, 30.0}, {25.4, 25.4}, {5.0, 5.0}})
            QVERIFY2(near(at(made_px, x, y), at(styled_px, x, y)[0], at(styled_px, x, y)[1], at(styled_px, x, y)[2], 0.02f),
                     qPrintable(told(at(made_px, x, y)) + told(at(styled_px, x, y))));
        // merged down onto a layer below: the shadow baked in, drawn as before
        core::Document below = run(two_fx, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "under"}, {"before", "paint"}});
        if (below.page(0).layers.front().id != "under") below = run(two_fx, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "under"}});
        const auto& order = below.page(0).layers;
        if (order.front().id == "under") {
            const core::Document merged = run(below, Json{{"op", "merge_down"}, {"page", 1}, {"id", "paint"}});
            QCOMPARE(merged.page(0).layers.size(), std::size_t(1));
            QVERIFY(!merged.page(0).layers[0].style);
            const auto merged_px = shown(merged);
            for (const auto& [x, y] : {std::pair{39.2, 30.0}, {25.4, 25.4}, {5.0, 5.0}})
                QVERIFY2(near(at(merged_px, x, y), at(styled_px, x, y)[0], at(styled_px, x, y)[1], at(styled_px, x, y)[2], 0.02f),
                         qPrintable(told(at(merged_px, x, y)) + told(at(styled_px, x, y))));
        }
        // refused: a kind of layer that takes none, an unknown effect, a length past 100 mm
        core::Document folder = run(doc, Json{{"op", "add_layer"}, {"page", 1}, {"folder", true}, {"id", "f"}});
        QVERIFY(!refusal(folder, Json::array({Json{{"op", "set_layer_style"}, {"page", 1}, {"id", "f"}, {"style", one(Json{{"kind", "stroke"}})}}})).empty());
        QVERIFY(!refusal(doc, Json::array({Json{{"op", "set_layer_style"}, {"page", 1}, {"id", "paint"}, {"style", one(Json{{"kind", "sparkle"}})}}})).empty());
        QVERIFY(!refusal(doc, Json::array({Json{{"op", "set_layer_style"}, {"page", 1}, {"id", "paint"}, {"style", one(Json{{"kind", "stroke"}, {"size_mm", 101}})}}})).empty());
        // a pattern's picture given to the op: the journal keeps the book's id, not the picture
        const auto applied = run_ops(doc, Json::array({Json{{"op", "set_layer_style"}, {"page", 1}, {"id", "paint"}, {"style", one(Json{{"kind", "pattern_overlay"}, {"pattern", checker()}})}}}));
        QVERIFY(applied.journal_ops.dump().find("\"png\"") == std::string::npos);
    }

    // saved, opened the same, undone (no style, no feature), done again
    void savedOpenedUndone() {
        QTemporaryDir scratch;
        const fs::path dir = core::path_from_utf8(scratch.path().toStdString()) / "s.genko";
        const core::Document doc = square_book();
        write_book(doc, dir);
        const Json op{{"op", "set_layer_style"}, {"page", 1}, {"id", "paint"},
                      {"style", Json{{"effects", Json::array({Json{{"kind", "stroke"}, {"size_mm", 0.8}}, Json{{"kind", "pattern_overlay"}, {"pattern", checker()}}})}, {"fill_opacity", 0.5}}}};
        const auto done = run_ops(doc, Json::array({op, Json{{"op", "set_global_light"}, {"angle", 60}}}));
        save_next(done.doc, dir, done.journal_ops);
        const core::Document opened = storage::load_document(dir).document;
        QCOMPARE(*opened.page(0).layers[0].style, *done.doc.page(0).layers[0].style);
        QCOMPARE(core::global_light(opened)[0], 60.0);
        QVERIFY(has_feature(opened, std::string(core::kLayerStylesFeature)));
        QCOMPARE(proof_bytes(opened), proof_bytes(done.doc));
        storage::restore(dir, "human:test", false, false);
        const core::Document undone = storage::load_document(dir).document;
        QVERIFY(!undone.page(0).layers[0].style);
        QVERIFY(!has_feature(undone, std::string(core::kLayerStylesFeature)));
        QCOMPARE(proof_bytes(undone), proof_bytes(doc));
        storage::restore(dir, "human:test", true, false);
        QCOMPARE(*storage::load_document(dir).document.page(0).layers[0].style, *done.doc.page(0).layers[0].style);
    }

    // a book whose style is not well formed is refused when it is read
    void brokenStyleRefused() {
        QTemporaryDir scratch;
        const fs::path dir = core::path_from_utf8(scratch.path().toStdString()) / "b.genko";
        write_book(styled(square_book(), one(Json{{"kind", "stroke"}})), dir);
        bool changed = false;
        for (const auto& entry : fs::recursive_directory_iterator(dir)) {
            if (entry.path().extension() != ".json") continue;
            std::string text = read_text(entry.path());
            const auto at_kind = text.find("\"size_mm\"");
            if (at_kind == std::string::npos) continue;
            text.replace(at_kind, 9, "\"size_xx\"");
            storage::write_atomic(entry.path(), text);
            changed = true;
        }
        QVERIFY(changed);
        bool refused = false;
        try {
            (void)storage::load_document(dir);
        } catch (const std::exception&) {
            refused = true;
        }
        QVERIFY(refused);
    }

    // a PSD: the style as lfx2 (and iOpa, the blending ranges), read back by this build's import as the same style
    void psdWrittenAndRead() {
        const Json style{{"effects", Json::array({Json{{"kind", "stroke"}, {"size_mm", 0.762}, {"rgb", Json::array({255, 0, 0})}},
                                                  Json{{"kind", "drop_shadow"}, {"distance_mm", 1.27}, {"size_mm", 0.508}, {"opacity", 0.5}, {"rgb", Json::array({10, 20, 30})}},
                                                  Json{{"kind", "pattern_overlay"}, {"pattern", checker()}}})},
                         {"fill_opacity", 0.5},
                         {"blend_if", Json::array({Json{{"channel", "gray"}, {"this", Json::array({0, 10, 240, 250})}, {"under", Json::array({0, 0, 255, 255})}}})}};
        const core::Document doc = styled(square_book(), style);
        QTemporaryDir scratch;
        const fs::path file = core::path_from_utf8(scratch.path().toStdString()) / "s.psd";
        formats::export_page_psd(doc, doc.page(0), file, 200);
        const std::string bytes = read_text(file);
        QVERIFY(bytes.find("lfx2") != std::string::npos);
        QVERIFY(bytes.find("iOpa") != std::string::npos);
        // read back: the same effects (lengths through the file's pixels)
        core::Document read = core::new_episode("読む", core::Num(1), 1, core::PageSpec::custom(2 * kInch, 2 * kInch, 40, 40, 1, 2, 2, 2, 2, 200, "color"));
        const Json report = render::import_psd_data(read, Json{{"page", 1}, {"id", "ps"}, {"fit", "paper"}}, bytes, true);
        const core::Layer* got = nullptr;
        for (const core::Layer& l : read.page(0).layers)
            if (l.style) got = &l;
        QVERIFY2(got != nullptr, report.dump().c_str());
        const Json& effects = (*got->style)["effects"];
        QCOMPARE(effects.size(), std::size_t(3));
        QCOMPARE(effects[0]["kind"], Json("stroke"));
        QVERIFY(std::abs(effects[0]["size_mm"].get<double>() - 0.762) < 0.02);
        QCOMPARE(effects[0]["rgb"], Json::parse("[255, 0, 0]"));
        QCOMPARE(effects[1]["kind"], Json("pattern_overlay"));
        QVERIFY(core::find_pattern(read, effects[1]["pattern"]["id"].get<std::string>()) != nullptr);
        QCOMPARE(effects[2]["kind"], Json("drop_shadow"));
        QVERIFY(std::abs(effects[2]["distance_mm"].get<double>() - 1.27) < 0.02);
        QCOMPARE(effects[2]["opacity"], Json(0.5));
        QCOMPARE(effects[2]["rgb"], Json::parse("[10, 20, 30]"));
        QVERIFY(std::abs((*got->style)["fill_opacity"].get<double>() - 0.5) < 0.01);
        QCOMPARE((*got->style)["blend_if"][0]["this"], Json::parse("[0, 10, 240, 250]"));
        // (its kept blocks: not the effects — they are the layer's own now)
        if (got->native_psd)
            for (const auto& block : got->native_psd->blocks) QVERIFY2(!core::psd_effect_key(block.first), block.first.c_str());

        // lfx2 alone: written and read back the same (its master switch too)
        render::psd_shapes::Canvas canvas;
        canvas.dpi = 200;
        canvas.width_mm = canvas.height_mm = 2 * kInch;
        canvas.width_px = canvas.height_px = kSide;
        Json off = *doc.page(0).layers[0].style;
        off.erase("fill_opacity");
        off.erase("blend_if");
        off["effects"].erase(1);
        off["enabled"] = false;
        render::psd_shapes::Placement placement;
        placement.dpi = 200;
        placement.mm_x = placement.mm_y = kInch / 200;
        placement.width_px = placement.height_px = kSide;
        std::vector<std::string> notes;
        const auto back = render::psd_effects::style_of(render::psd_effects::lfx2(off, canvas), placement, notes);
        QVERIFY(back.has_value());
        QCOMPARE((*back)["enabled"], Json(false));
        QCOMPARE((*back)["effects"].size(), std::size_t(2));
        // broken: not read, said; never allocated past its bytes
        notes.clear();
        QVERIFY(!render::psd_effects::style_of(std::string("\0\0\0\0\0\0\0\x10\xFF\xFF\xFF\xFF", 12), placement, notes));
        QCOMPARE(notes.size(), std::size_t(1));

        // psd-tools: the layer's effects as Photoshop's
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
        const QString answer = scratch.path() + QStringLiteral("/answer.json");
        const auto ran = genko::test::run(genko::test::python_ref(),
                                          {QStringLiteral("-I"), genko::test::repo_root() + QStringLiteral("/tools/migration/psd_effects_check.py"), answer,
                                           QString::fromStdString(storage::path_to_utf8(file))},
                                          genko::test::python_env(scratch.path() + QStringLiteral("/pyenv")));
        QVERIFY2(ran.exit_code == 0, qPrintable(ran.described()));
        const Json seen = genko::test::read_json(answer);
        Json layer;
        for (const Json& l : seen["layers"])
            if (!l["effects"].empty()) layer = l;
        QVERIFY2(layer.is_object(), seen.dump().c_str());
        QCOMPARE(layer["enabled"], Json(true));
        std::vector<std::string> kinds;
        for (const Json& e : layer["effects"]) kinds.push_back(e["kind"].get<std::string>());
        std::sort(kinds.begin(), kinds.end());
        QCOMPARE(kinds, (std::vector<std::string>{"DropShadow", "PatternOverlay", "Stroke"}));
        for (const Json& e : layer["effects"]) {
            if (e["kind"] == Json("DropShadow")) {
                QVERIFY2(std::abs(e["distance"].get<double>() - 10) < 0.6, e.dump().c_str());  // (1.27 mm at 200 dpi)
                QVERIFY2(std::abs(e["opacity"].get<double>() - 50) < 0.6, e.dump().c_str());
            }
            if (e["kind"] == Json("Stroke")) QVERIFY2(std::abs(e["size"].get<double>() - 6) < 0.6, e.dump().c_str());
        }
        QVERIFY2(std::abs(layer["fill_opacity"].get<double>() - 128) <= 1, layer.dump().c_str());
    }
};

QTEST_GUILESS_MAIN(TestLayerStyles)
#include "test_layer_styles.moc"
