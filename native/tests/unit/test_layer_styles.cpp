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
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ink.hpp"
#include "core/layer_styles.hpp"
#include "core/paths.hpp"
#include "core/patterns.hpp"
#include "core/psd_keep.hpp"
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
#include "storage/asset_store.hpp"
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
core::Document square_book(const std::string& expression = "color") {
    core::Document doc = core::new_episode("スタイル", core::Num(1), 1, core::PageSpec::custom(2 * kInch, 2 * kInch, 40, 40, 1, 2, 2, 2, 2, 200, expression));
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
        QVERIFY(!core::uses_layer_styles(same));
        const core::Document on = styled(doc, one(Json{{"kind", "drop_shadow"}}));
        QVERIFY(core::uses_layer_styles(on));
        QVERIFY(proof_bytes(on) != before);
        const core::Document off = styled(on, nullptr);
        QCOMPARE(proof_bytes(off), before);
        QVERIFY(!core::uses_layer_styles(off));
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
        QVERIFY2(c[1] < 0.03f && std::abs(c[0] + c[2] - 1) < 0.05f, qPrintable(told(c)));  // (red and blue, sampled between)
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
        // (an inside stroke: drawn with the layer's interior effects, not faded with its pixels; an outside stroke of a
        // paint layer lies beneath the whole shape, as upstream's)
        Json style{{"effects", Json::array({Json{{"kind", "stroke"}, {"size_mm", 1}, {"position", "inside"}, {"rgb", Json::array({255, 0, 0})}},
                                            Json{{"kind", "drop_shadow"}, {"blend", "normal"}, {"opacity", 1}, {"distance_mm", 3}, {"size_mm", 0}}})}};
        style["fill_opacity"] = 0;
        auto px = shown(styled(doc, style));
        QVERIFY2(near(at(px, 25.4, 25.4), 1, 1, 1), qPrintable(told(at(px, 25.4, 25.4))));
        QVERIFY2(near(at(px, 13.2, 25.4), 1, 0, 0), qPrintable(told(at(px, 13.2, 25.4))));
        QVERIFY2(near(at(px, 39.2, 30), 0, 0, 0), qPrintable(told(at(px, 39.2, 30))));
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
                QVERIFY2(inside[2] > inside[0] + 0.25f, qPrintable(QString::fromStdString(kind) + told(inside)));
            }
            QVERIFY2(light(at(px, 39.2, 30)) < 0.2f, qPrintable(QString::fromStdString(kind) + told(at(px, 39.2, 30))));
            QVERIFY2(light(at(px, 11.5, 25.4)) > 0.9f, qPrintable(QString::fromStdString(kind) + told(at(px, 11.5, 25.4))));
        }
    }

    // (review P2-2) an effect reaching past the cap (a drop shadow 60 mm away, 60 mm soft, at 350 dpi: 1654 px) is scaled
    // down to it as the margin is: drawn in tiles of any size, the same pixels as drawn whole
    void bigReachSameAcrossTiles() {
        const Json style = one(Json{{"kind", "drop_shadow"}, {"blend", "normal"}, {"opacity", 1}, {"distance_mm", 60}, {"size_mm", 60}});
        const double dpi = 350;
        const int margin = render::layer_styles::margin_px(style, dpi);
        QVERIFY2(margin <= 1026, qPrintable(QString::number(margin)));
        const int W = 2600, H = 700;
        const render::layer_styles::Mixing mx = render::layer_styles::srgb_mixing();
        std::vector<float> content(std::size_t(W) * H * 4, 0.0f);
        for (int y = 250; y < 450; ++y)
            for (int x = 200; x < 400; ++x) {
                float* p = content.data() + (std::size_t(y) * W + x) * 4;
                p[0] = p[1] = p[2] = 0.8f;
                p[3] = 1;
            }
        const auto draw = [&](const render::Box& area) {
            render::layer_styles::Inputs in;
            in.style = &style;
            in.light = {180, 30};  // (the light from the left: the shadow falls right, along the row of tiles)
            in.dpi = dpi;
            in.page = Size{W, H};
            in.area = area;
            in.wide = render::Box{std::max(0, area.x0 - margin), std::max(0, area.y0 - margin), std::min(W, area.x1 + margin), std::min(H, area.y1 + margin)};
            std::vector<float> wide(std::size_t(in.wide.width()) * in.wide.height() * 4);
            for (int y = in.wide.y0; y < in.wide.y1; ++y)
                std::copy_n(content.data() + (std::size_t(y) * W + in.wide.x0) * 4, std::size_t(in.wide.width()) * 4,
                            wide.data() + (std::size_t(y - in.wide.y0) * in.wide.width()) * 4);
            std::vector<float> back(std::size_t(area.width()) * area.height() * 4, 1.0f);
            render::layer_styles::composite(in, mx, wide, back);
            return back;
        };
        const std::vector<float> whole = draw(render::Box{0, 0, W, H});
        const auto byte = [](float v) { return int(std::lround(std::clamp(v, 0.0f, 1.0f) * 255)); };
        // (the shadow reaches well past the shape: some 500 px right of it, spread thin by its size)
        QVERIFY2(byte(whole[(std::size_t(350) * W + 812) * 4]) < 240, qPrintable(QString::number(whole[(std::size_t(350) * W + 812) * 4])));
        for (const int side : {173, 256, 600}) {
            int worst = 0;
            for (int y0 = 0; y0 < H; y0 += side)
                for (int x0 = 0; x0 < W; x0 += side) {
                    const render::Box tile{x0, y0, std::min(W, x0 + side), std::min(H, y0 + side)};
                    const std::vector<float> got = draw(tile);
                    for (int y = tile.y0; y < tile.y1; ++y)
                        for (int x = tile.x0; x < tile.x1; ++x)
                            for (int k = 0; k < 4; ++k)
                                worst = std::max(worst, std::abs(byte(got[(std::size_t(y - tile.y0) * tile.width() + (x - tile.x0)) * 4 + k]) -
                                                                 byte(whole[(std::size_t(y) * W + x) * 4 + k])));
                }
            QVERIFY2(worst == 0, qPrintable(QStringLiteral("tiles of %1: %2 levels apart").arg(side).arg(worst)));
        }
        // (the page drawn so, whole and by its parts: the same pixels — review P2-3's tiles of a margin past the cap)
        const core::Document doc = styled(square_book(), style);
        render::RenderOptions options = render::proof_options();
        const render::Image whole_page = render::render_page(doc.page(0), int(dpi), options, &doc).image;
        const int side_px = whole_page.width();
        for (const render::RenderRegion part : {render::RenderRegion{0, 0, side_px / 2, side_px / 2}, render::RenderRegion{side_px / 2, side_px / 3, side_px - side_px / 2, side_px / 2}}) {
            options.region = part;
            const render::Image got = render::render_page(doc.page(0), int(dpi), options, &doc).image;
            QVERIFY2(got.tobytes() == whole_page.crop(render::Box{part.x, part.y, part.x + part.w, part.y + part.h}).tobytes(),
                     qPrintable(QStringLiteral("part at %1, %2").arg(part.x).arg(part.y)));
        }
    }

    // (review P2-1) 描画レイヤーに変換 keeps the layer's style on it, its effects not drawn into its pixels as well — on an
    // RGB page and on a page of inks alike: the page as before
    void convertedKeepsItsStyle() {
        const Json style = one(Json{{"kind", "drop_shadow"}, {"blend", "normal"}, {"opacity", 0.5}, {"distance_mm", 3}, {"size_mm", 0}});
        for (const std::string kind : {"8", "cmyk"}) {
            core::Document doc = as_kind(square_book(), kind);
            doc = run(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "pen"}});
            doc = run(doc, Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", "pen"}, {"points", Json::array({Json::array({8, 25}), Json::array({43, 25})})}, {"width_mm", 2}});
            doc = styled(doc, style, "pen");
            const std::vector<Rgba> before = shown(doc);
            const core::Document converted = run(doc, Json{{"op", "convert_layer"}, {"page", 1}, {"id", "pen"}, {"to", "paint"}});
            const core::Layer* pen = nullptr;
            for (const core::Layer& l : converted.page(0).layers)
                if (l.id == "pen") pen = &l;
            QVERIFY(pen != nullptr && pen->style.has_value());
            const std::vector<Rgba> after = shown(converted);
            // (the shadow below the line, over the grey square and over the paper: once, not twice)
            for (const auto& [x, y] : {std::pair{26.5, 27.8}, {41.0, 27.8}, {25.4, 25.0}})
                QVERIFY2(near(at(after, x, y), at(before, x, y)[0], at(before, x, y)[1], at(before, x, y)[2], 0.03f),
                         qPrintable(QString::fromStdString(kind) + told(at(after, x, y)) + told(at(before, x, y))));
        }
    }

    // (review P2-4) a layer from a PSD on a monochrome page is finished — grey — with its effects, in a proof
    void monochromePsdLayerFinished() {
        core::Document doc = square_book("mono");
        doc.edit_page(0).layers[0].source = Json{{"kind", "psd"}};
        const core::Document red = styled(doc, Json{{"effects", Json::array({Json{{"kind", "color_overlay"}, {"rgb", Json::array({255, 0, 0})}},
                                                                            Json{{"kind", "drop_shadow"}, {"blend", "normal"}, {"opacity", 1}, {"rgb", Json::array({0, 0, 255})}, {"distance_mm", 3}, {"size_mm", 0}}})}});
        const render::Image page = render::render_page(red.page(0), 200, render::proof_options(), &red).image.convert("RGB");
        const std::string bytes = page.tobytes();
        const auto px = [&](double x_mm, double y_mm) {
            const std::size_t x = std::size_t(x_mm * 200 / kInch), y = std::size_t(y_mm * 200 / kInch);
            const std::size_t i = (y * std::size_t(page.width()) + x) * 3;
            return std::array<int, 3>{static_cast<unsigned char>(bytes[i]), static_cast<unsigned char>(bytes[i + 1]), static_cast<unsigned char>(bytes[i + 2])};
        };
        for (const auto& [x, y] : {std::pair{25.4, 25.4}, {39.2, 30.0}}) {
            const auto c = px(x, y);
            QVERIFY2(c[0] == c[1] && c[1] == c[2] && c[0] < 250, qPrintable(QStringLiteral("(%1, %2, %3)").arg(c[0]).arg(c[1]).arg(c[2])));
        }
        // (by its parts the same)
        render::RenderOptions options = render::proof_options();
        options.region = render::RenderRegion{100, 120, 150, 100};
        QCOMPARE(render::render_page(red.page(0), 200, options, &red).image.convert("RGB").tobytes(), page.crop(render::Box{100, 120, 250, 220}).tobytes());
    }

    // (review P2-5) a smart object with a style keeps its own inks on a page of inks and its own bits on a page in precise
    // colour: inside it, the same values as without the style
    void smartObjectsKeepTheirOwnValues() {
        const Json style = one(Json{{"kind", "drop_shadow"}, {"blend", "normal"}, {"opacity", 1}, {"distance_mm", 3}, {"size_mm", 0}});
        {
            core::Document doc = as_kind(square_book(), "cmyk");
            doc = run(doc, Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({"paint"})}, {"new_id", "so"}});
            render::RenderOptions options = render::proof_options();
            options.inks = true;
            const auto inks_at = [&](const core::Document& d) {
                const render::RenderResult out = render::render_page(d.page(0), 200, options, &d);
                const core::InkPixels& inks = *out.inks;
                const std::size_t i = (std::size_t(200) * inks.width + 200) * inks.channels();
                return std::vector<std::uint16_t>(inks.samples.begin() + std::ptrdiff_t(i), inks.samples.begin() + std::ptrdiff_t(i + inks.colours()));
            };
            const std::vector<std::uint16_t> plain = inks_at(doc), with = inks_at(styled(doc, style, "so"));
            QCOMPARE(with.size(), plain.size());
            for (std::size_t k = 0; k < plain.size(); ++k)
                QVERIFY2(std::abs(int(with[k]) - int(plain[k])) <= 2, qPrintable(QStringLiteral("plate %1: %2 for %3").arg(k).arg(with[k]).arg(plain[k])));
        }
        {
            core::Document doc = square_book();
            doc = run(doc, Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}});
            render::Image grey = render::Image::create("I;16", Size{4, 4}, render::Ink(1000));
            const core::Rect b = doc.page(0).bleed_rect_mm();
            doc = run(doc, Json{{"op", "smart_place"}, {"page", 1}, {"data", core::b64encode(render::write_png(grey))}, {"name", "g.png"},
                                {"placement_mm", Json::array({b.x.json(), b.y.json(), b.width.json(), b.height.json()})}});
            std::string id;
            for (const core::Layer& l : doc.page(0).layers)
                if (l.kind == core::LayerKind::Smart) id = l.id;
            QVERIFY(!id.empty());
            render::RenderOptions options = render::proof_options();
            options.precise = true;
            // (an inside stroke: the middle of the picture its own, in more than 8 bits)
            const core::Document with = styled(doc, Json{{"effects", Json::array({Json{{"kind", "stroke"}, {"size_mm", 1}, {"position", "inside"}, {"rgb", Json::array({255, 0, 0})}}})}}, id);
            const render::RenderResult got = render::render_page(with.page(0), 100, options, &with);
            QVERIFY(got.precise.has_value());
            const core::ColorRasterView values(*got.precise);
            const auto p = values.pixel(std::size_t(values.width()) * (values.height() / 2) + values.width() / 2);
            QVERIFY2(std::abs(p[0] - 1000 / 65535.0) < 1e-4, qPrintable(QString::number(p[0] * 65535)));
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
        QVERIFY(core::uses_layer_styles(lit));
        QVERIFY(shown(lit) != with);
        QCOMPARE(core::global_light(run(lit, Json{{"op", "set_global_light"}})), (std::array<double, 2>{30, 45}));  // (nothing given: as it is)
        QVERIFY(!run(lit, Json{{"op", "set_global_light"}, {"angle", 120}, {"altitude", 30}}).global_light);  // (Photoshop's own: not written)
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
        // (review P3-12: the layer below put there itself, so the merge is always tried)
        core::Document below = two_fx;
        core::Layer under;
        under.id = "under";
        under.title = "下";
        under.role = core::LayerRole::User;
        under.kind = core::LayerKind::Raster;
        under.panel_clip = false;
        below.edit_page(0).layers.insert(below.edit_page(0).layers.begin(), under);
        QCOMPARE(below.page(0).layers.front().id, std::string("under"));
        const core::Document merged = run(below, Json{{"op", "merge_down"}, {"page", 1}, {"id", "paint"}});
        QCOMPARE(merged.page(0).layers.size(), std::size_t(1));
        QVERIFY(!merged.page(0).layers[0].style);
        const auto merged_px = shown(merged);
        for (const auto& [x, y] : {std::pair{39.2, 30.0}, {25.4, 25.4}, {5.0, 5.0}})
            QVERIFY2(near(at(merged_px, x, y), at(styled_px, x, y)[0], at(styled_px, x, y)[1], at(styled_px, x, y)[2], 0.02f),
                     qPrintable(told(at(merged_px, x, y)) + told(at(styled_px, x, y))));
        // (and on a page of inks: drawn in, once)
        const core::Document ink_below = as_kind(below, "cmyk");
        const auto ink_styled = shown(ink_below);
        const core::Document ink_merged = run(ink_below, Json{{"op", "merge_down"}, {"page", 1}, {"id", "paint"}});
        QVERIFY(!ink_merged.page(0).layers[0].style);
        const auto ink_merged_px = shown(ink_merged);
        for (const auto& [x, y] : {std::pair{39.2, 30.0}, {25.4, 25.4}, {5.0, 5.0}})
            QVERIFY2(near(at(ink_merged_px, x, y), at(ink_styled, x, y)[0], at(ink_styled, x, y)[1], at(ink_styled, x, y)[2], 0.03f),
                     qPrintable(told(at(ink_merged_px, x, y)) + told(at(ink_styled, x, y))));
        // refused: a kind of layer that takes none, an unknown effect, a length past 100 mm
        core::Document folder = run(doc, Json{{"op", "add_layer"}, {"page", 1}, {"folder", true}, {"id", "f"}});
        QVERIFY(!refusal(folder, Json::array({Json{{"op", "set_layer_style"}, {"page", 1}, {"id", "f"}, {"style", one(Json{{"kind", "stroke"}})}}})).empty());
        QVERIFY(!refusal(doc, Json::array({Json{{"op", "set_layer_style"}, {"page", 1}, {"id", "paint"}, {"style", one(Json{{"kind", "sparkle"}})}}})).empty());
        QVERIFY(!refusal(doc, Json::array({Json{{"op", "set_layer_style"}, {"page", 1}, {"id", "paint"}, {"style", one(Json{{"kind", "stroke"}, {"size_mm", 101}})}}})).empty());
        // a pattern's picture given to the op: the journal keeps its size, not the picture
        const auto applied = run_ops(doc, Json::array({Json{{"op", "set_layer_style"}, {"page", 1}, {"id", "paint"}, {"style", one(Json{{"kind", "pattern_overlay"}, {"pattern", checker()}})}}}));
        QVERIFY(applied.journal_ops.dump().find("base64 chars>") != std::string::npos);
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
        try {
            storage::restore(dir, "human:test", false, false);
        } catch (const core::Error& error) {
            QFAIL((std::string(error.code()) + ": " + error.what()).c_str());
        } catch (const std::exception& error) {
            QFAIL(error.what());
        }
        const core::Document undone = storage::load_document(dir).document;
        QVERIFY(!undone.page(0).layers[0].style);
        QVERIFY(!has_feature(undone, std::string(core::kLayerStylesFeature)));
        QCOMPARE(proof_bytes(undone), proof_bytes(doc));
        try {
            storage::restore(dir, "human:test", true, false);
        } catch (const core::Error& error) {
            QFAIL((std::string(error.code()) + ": " + error.what()).c_str());
        }
        QCOMPARE(*storage::load_document(dir).document.page(0).layers[0].style, *done.doc.page(0).layers[0].style);
    }

    // a book whose style is not well formed is read read-only (its report says why)
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
        // (read, said in the report: the book read-only, as any value not as the writer writes it)
        const storage::LoadResult read = storage::load_document(dir);
        QVERIFY(!read.report.clean());
        QVERIFY2(read.report.to_json().dump().find("style") != std::string::npos, read.report.to_json().dump().c_str());
    }

    // (review P2-6) the style of a placed picture and of the ink layer's lines goes into a PSD with them
    void psdOfPlacedAndInkLines() {
        QTemporaryDir scratch;
        const fs::path dir = core::path_from_utf8(scratch.path().toStdString());
        const Json style = one(Json{{"kind", "drop_shadow"}, {"distance_mm", 1}});
        const auto written = [&](const core::Document& doc, const char* name) {
            formats::export_page_psd(doc, doc.page(0), dir / name, 200);
            return read_text(dir / name);
        };
        // the ink layer's lines (「ペン入れ」)
        core::Document doc = square_book();
        core::Layer pen;
        pen.id = "ink";
        pen.title = "線";
        pen.role = core::LayerRole::Ink;
        pen.kind = core::LayerKind::Strokes;
        doc.edit_page(0).layers.push_back(pen);
        doc = run(doc, Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", "ink"}, {"points", Json::array({Json::array({8, 25}), Json::array({43, 25})})}, {"width_mm", 1}});
        QVERIFY(written(doc, "lines.psd").find("lfx2") == std::string::npos);
        QVERIFY(written(styled(doc, style, "ink"), "lines-styled.psd").find("lfx2") != std::string::npos);
        // a placed picture
        core::Document placed = square_book();
        placed.asset_dir = dir;
        placed.edit_page(0).name_ok = true;
        std::string bytes;
        for (int i = 0; i < 16 * 16; ++i) bytes += std::string("\x20\x80\xC0\xFF", 4);
        const std::string ref = storage::AssetStore(*placed.asset_dir).put_bytes(render::write_png(render::Image::frombytes("RGBA", Size{16, 16}, bytes)), ".png");
        placed = run(placed, Json{{"op", "place_asset"}, {"page", 1}, {"asset", ref}, {"placement_mm", Json::array({10, 10, 20, 20})}});
        std::string id;
        for (const core::Layer& l : placed.page(0).layers)
            if (l.kind == core::LayerKind::Placed) id = l.id;
        QVERIFY(!id.empty());
        QVERIFY(written(placed, "placed.psd").find("lfx2") == std::string::npos);
        QVERIFY(written(styled(placed, style, id), "placed-styled.psd").find("lfx2") != std::string::npos);
    }

    // (review P2-7) a PSD read into a book of another global light: its effects that took the file's light hold its angle
    // as their own, and the file's effects block is not kept (written back from the style: Photoshop draws them so too)
    void psdReadUnderAnotherLight() {
        const core::Document doc = styled(square_book(), one(Json{{"kind", "drop_shadow"}, {"distance_mm", 1.27}, {"size_mm", 0.5}}));
        QTemporaryDir scratch;
        const fs::path file = core::path_from_utf8(scratch.path().toStdString()) / "lit.psd";
        formats::export_page_psd(doc, doc.page(0), file, 200);
        const std::string bytes = read_text(file);
        core::Document read = core::new_episode("読む", core::Num(1), 1, core::PageSpec::custom(2 * kInch, 2 * kInch, 40, 40, 1, 2, 2, 2, 2, 200, "color"));
        read = run(read, Json{{"op", "set_global_light"}, {"angle", 90}, {"altitude", 30}});
        // (taken into a page of the book: not opened as a book of its own, which takes the file's light)
        const Json report = render::import_psd_data(read, Json{{"page", 1}, {"id", "ps"}, {"fit", "paper"}}, bytes, false);
        const core::Layer* got = nullptr;
        for (const core::Layer& l : read.page(0).layers)
            if (l.style) got = &l;
        QVERIFY2(got != nullptr, report.dump().c_str());
        const Json& effect = (*got->style)["effects"][0];
        QCOMPARE(effect["global_light"], Json(false));
        QCOMPARE(effect["angle"], Json(120.0));
        if (got->native_psd)
            for (const auto& block : got->native_psd->blocks) QVERIFY2(!core::psd_effect_key(block.first), block.first.c_str());
        // (written back: the effects from the style)
        const fs::path again = core::path_from_utf8(scratch.path().toStdString()) / "again.psd";
        formats::export_page_psd(read, read.page(0), again, 200);
        QVERIFY(read_text(again).find("lfx2") != std::string::npos);
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
        // (the file's effects block kept to be written back as it is while the style is not changed — but drawn by this
        // build now: not the panel's 効果（PSD）; a style set again forgets it)
        QVERIFY(!core::psd_effects_kept(*got));
        const std::string got_id = got->id;
        const core::Document restyled = run(read, Json{{"op", "set_layer_style"}, {"page", 1}, {"id", got_id}, {"style", one(Json{{"kind", "stroke"}})}});
        for (const core::Layer& l : restyled.page(0).layers)
            if (l.id == got_id && l.native_psd)
                for (const auto& block : l.native_psd->blocks) QVERIFY2(!core::psd_effect_key(block.first), block.first.c_str());

        // lfx2 alone: written and read back the same (its master switch too)
        render::psd_shapes::Canvas canvas;
        canvas.dpi = 200;
        canvas.width_mm = canvas.height_mm = 2 * kInch;
        canvas.width_px = canvas.height_px = kSide;
        Json off = *doc.page(0).layers[0].style;
        off.erase("fill_opacity");
        off.erase("blend_if");
        Json without_pattern = Json::array();  // (no book here: the pattern overlay left out)
        for (const Json& e : off["effects"])
            if (e["kind"] != Json("pattern_overlay")) without_pattern.push_back(e);
        off["effects"] = without_pattern;
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
