// PC3 B-0 (SPEC.md COLOR-05, schema-v4.md「マスク（masks@1）」): the selection's algorithms as PhotoCraft's
// algo/selection.rs tests them (the wand and its 8-bit region, Color Range's samples, families and tones, the distance
// transform's expand, contract and border, smooth, feather, the anti-aliased polygon), the area kinds and modifiers this
// build adds (an area without them the same bytes as before; their order; a kind not written yet refused, naming its
// group), the layer mask's settings (masks@1: written only when held, read back, refused out of range, the feature named
// only while used; density and feather drawn on 8-bit, precise and ink pages) and a mask lent to a pixel op as a layer to
// paint (only the pixels the op changed come back).

#include <QtTest>

#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <numeric>
#include <string>
#include <vector>

#include "core/base64.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/masks.hpp"
#include "core/paths.hpp"
#include "render/area_kinds.hpp"
#include "render/fill.hpp"
#include "render/image.hpp"
#include "render/layer_masks.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/selection.hpp"
#include "render/selection_ext.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/transaction.hpp"
#include "storage/writer.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
namespace fs = std::filesystem;
namespace sx = render::selection_ext;

namespace {

std::vector<sx::Rgba> img(int w, int h, const std::function<sx::Rgba(int, int)>& f) {
    std::vector<sx::Rgba> out;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) out.push_back(f(x, y));
    return out;
}

std::size_t count_selected(const std::vector<float>& m) {
    return static_cast<std::size_t>(std::count_if(m.begin(), m.end(), [](float v) { return v > 0.0f; }));
}

std::vector<float> square(std::size_t w, std::size_t h) {
    std::vector<float> m(w * h, 0.0f);
    for (std::size_t i = 0; i < m.size(); ++i) {
        const std::size_t x = i % w, y = i / w;
        if (x >= 5 && x < 15 && y >= 5 && y < 15) m[i] = 1.0f;
    }
    return m;
}

std::vector<sx::Rgba8> blobs(int w, int h) {
    std::vector<sx::Rgba8> out;
    for (int i = 0; i < w * h; ++i) {
        const int x = i % w, y = i / w;
        const int v = (x * 7 + y * 3) % 23 + ((x / 5 + y / 4) % 3 == 0 ? 120 : 0);
        out.push_back({static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v / 2), static_cast<std::uint8_t>(255 - v),
                       static_cast<std::uint8_t>(x % 11 == 0 ? 128 : 255)});
    }
    return out;
}

core::ApplyResult run_ops(const core::Document& doc, const Json& ops) {
    return core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor("human:test"));
}

std::string refusal(const core::Document& doc, const Json& op) {
    try {
        run_ops(doc, Json::array({op}));
    } catch (const core::ApplyError& error) {
        return error.code() + ": " + error.what();
    }
    return {};
}

// A book of one page 60 × 80 mm, its panels taken away.
core::Document small_book() {
    core::PageSpec spec;
    spec.width_mm = core::Num(60);
    spec.height_mm = core::Num(80);
    spec.dpi = core::Num(150);
    spec.bleed_mm = core::Num(3);
    spec.inner_margin_mm = core::Num(5);
    core::Document doc = core::new_episode("pc3", core::Num(1), 1, spec, core::Binding::Right);
    doc.edit_page(0).frames.clear();
    return doc;
}

constexpr int kW = 472, kH = 630;  // (60 × 80 mm at 200 dpi)

// The book with an opaque grey paint layer over its page and a half-transparent square in it.
core::Document painted() {
    core::Document doc = small_book();
    std::string bytes(std::size_t(kW) * kH * 4, '\0');
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const std::size_t at = (std::size_t(y) * kW + x) * 4;
            bytes[at] = bytes[at + 1] = bytes[at + 2] = static_cast<char>(100);
            bytes[at + 3] = static_cast<char>(x >= 100 && x < 200 && y >= 100 && y < 200 ? 128 : (x < 50 ? 0 : 255));
        }
    }
    core::Layer layer;
    layer.id = "paint0000001";
    layer.kind = core::LayerKind::Raster;
    layer.panel_clip = false;
    layer.raster_png = std::make_shared<const std::string>(render::write_png(render::Image::frombytes("RGBA", render::Size{kW, kH}, bytes)));
    doc.edit_page(0).layers.push_back(std::move(layer));
    return doc;
}

const core::Layer& layer_of(const core::Page& page, const std::string& id) {
    for (const core::Layer& layer : page.layers) {
        if (layer.id == id) return layer;
    }
    throw std::runtime_error("no layer " + id);
}

void write_book(const core::Document& doc, const fs::path& dir) {
    storage::ProjectLock lock(dir, "human:test");
    lock.try_acquire();
    storage::SaveRequest request;
    request.actor = "human:test";
    request.ops = Json::array();
    storage::Saver(lock).save(doc, request);
}

std::string bytes_of(const fs::path& file) { return test::read_bytes(QString::fromStdString(core::path_to_utf8(file))); }

render::Image mask_pixels(const core::Layer& layer) { return render::open_image(*layer.mask->png).convert("L"); }

}  // namespace

class TestSelectionExt : public QObject {
    Q_OBJECT

private slots:
    // selection.rs: wand_contiguous_vs_global, wand_tolerance
    void wand() {
        const auto px = img(20, 10, [](int x, int) {
            return (x >= 2 && x < 6) || (x >= 12 && x < 16) ? sx::Rgba{1, 0, 0, 1} : sx::Rgba{1, 1, 1, 1};
        });
        const sx::Rect a{0, 0, 20, 10};
        QCOMPARE(count_selected(sx::magic_wand(px, a, {3, 3}, 10, true, false)), std::size_t(40));
        QCOMPARE(count_selected(sx::magic_wand(px, a, {3, 3}, 10, false, false)), std::size_t(80));
        const auto aa = sx::magic_wand(px, a, {3, 3}, 10, true, true);
        QVERIFY(aa[3 * 20 + 6] > 0.0f && aa[3 * 20 + 6] < 1.0f);
        QCOMPARE(count_selected(sx::magic_wand(px, a, {50, 3}, 10, true, false)), std::size_t(0));
        const auto ramp = img(10, 1, [](int x, int) { return sx::Rgba{x * 10.0f / 255.0f, 0, 0, 1}; });
        QCOMPARE(count_selected(sx::magic_wand(ramp, sx::Rect{0, 0, 10, 1}, {0, 0}, 25, true, false)), std::size_t(3));
    }

    // selection.rs: wand_region_matches_reference_magic_wand
    void wandRegionMatchesTheWand() {
        const int w = 37, h = 29;
        const sx::Rect area{3, -2, 3 + w, -2 + h};
        const auto bytes = blobs(w, h);
        std::vector<sx::Rgba> f;
        for (const auto& p : bytes) f.push_back({p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f});
        struct Case {
            std::pair<int, int> seed;
            float tol;
            bool contiguous, aa;
        };
        for (const Case& c : {Case{{10, 5}, 10, true, true}, Case{{10, 5}, 10, true, false}, Case{{20, 20}, 40, false, true},
                              Case{{4, -2}, 0, true, true}, Case{{30, 10}, 255, true, true}}) {
            const auto reference = sx::magic_wand(f, area, c.seed, c.tol, c.contiguous, c.aa);
            const auto r = sx::wand_region(bytes, area, c.seed, c.tol, c.contiguous, c.aa);
            QVERIFY(r.has_value());
            for (int y = area.y0; y < area.y1; ++y) {
                for (int x = area.x0; x < area.x1; ++x) {
                    const float want = std::round(reference[std::size_t((y - area.y0) * w + x - area.x0)] * 255.0f);
                    QVERIFY2(std::fabs(r->at(x, y) * 255.0f - want) <= 1.0f, qPrintable(QStringLiteral("%1,%2").arg(x).arg(y)));
                }
            }
        }
        QVERIFY(!sx::wand_region(bytes, area, {0, 0}, 10, true, true).has_value());
    }

    // selection.rs: color_range_falloff, color_range_several_samples_and_localized, hue_families, tonal_ranges
    void colorRange() {
        const std::vector<sx::Rgba> px{{1, 0, 0, 1}, {0.9f, 0, 0, 1}, {0, 0, 1, 1}, {1, 0, 0, 0}};
        const auto m = sx::color_range(px, {1, 0, 0}, 40);
        QCOMPARE(m[0], 1.0f);
        QVERIFY(m[1] > 0.3f && m[1] < 1.0f);
        QCOMPARE(m[2], 0.0f);
        QCOMPARE(m[3], 0.0f);
        const sx::Rgba red{1, 0, 0, 1}, blue{0, 0, 1, 1}, green{0, 1, 0, 1};
        const std::vector<sx::Rgba> four{red, blue, green, red};
        const auto s = [](const sx::Rgba& c, std::optional<std::pair<float, float>> at) { return sx::RangeSample{{c[0], c[1], c[2]}, at}; };
        QCOMPARE(sx::color_range_samples(four, 4, {s(red, {}), s(blue, {})}, 10, {}), (std::vector<float>{1, 1, 0, 1}));
        QCOMPARE(sx::color_range_samples(four, 4, {s(red, {})}, 40, {}), sx::color_range(four, {1, 0, 0}, 40));
        QCOMPARE(sx::color_range_samples(four, 4, {s(red, std::make_pair(0.0f, 0.0f))}, 10, 2.0f), (std::vector<float>{1, 0, 0, 0}));
        const std::vector<sx::Rgba> reds(4, red);
        QCOMPARE(sx::color_range_samples(reds, 4, {s(red, std::make_pair(0.0f, 0.0f))}, 10, 2.0f), (std::vector<float>{1, 0.5f, 0, 0}));
        QVERIFY(count_selected(sx::color_range_samples(four, 4, {}, 10, {})) == 0);
        const std::vector<sx::Rgba> hues{{1, 0, 0, 1}, {1, 0.5f, 0, 1}, {0.5f, 0.5f, 0.5f, 1}, {1, 0.5f, 0.5f, 1}, {0, 0, 1, 1}, {1, 0, 0, 0}};
        const auto r = sx::hue_range(hues, 0);
        QCOMPARE(r[0], 1.0f);
        QVERIFY(std::fabs(r[1] - 0.5f) < 1e-5f);
        QCOMPARE(r[2], 0.0f);
        QVERIFY(std::fabs(r[3] - 0.5f) < 1e-5f);
        QCOMPARE(r[4], 0.0f);
        QCOMPARE(r[5], 0.0f);
        QVERIFY(std::fabs(sx::hue_range(hues, 60)[1] - 0.5f) < 1e-5f);
        QCOMPARE(sx::hue_range(hues, 240)[4], 1.0f);
        QCOMPARE(sx::hue_range({{1, 0, 1, 1}}, 300)[0], 1.0f);
        QVERIFY(std::fabs(sx::hue_range({{1, 0, 0.5f, 1}}, 300)[0] - 0.5f) < 1e-5f);
        const auto g = [](float v) { return sx::Rgba{v / 255, v / 255, v / 255, 1}; };
        const std::vector<sx::Rgba> tones{g(0), g(60), g(75), g(128), g(200), g(255)};
        const auto near = [](const std::vector<float>& a, const std::vector<float>& b) {
            for (std::size_t i = 0; i < a.size(); ++i)
                if (std::fabs(a[i] - b[i]) >= 1e-4f) return false;
            return true;
        };
        QVERIFY(near(sx::tone_range(tones, 0, 65, 20), {1, 1, 0.5f, 0, 0, 0}));
        QVERIFY(near(sx::tone_range(tones, 190, 255, 20), {0, 0, 0, 0, 1, 1}));
        QVERIFY(near(sx::tone_range(tones, 105, 150, 20), {0, 0, 0, 1, 0, 0}));
    }

    // selection.rs: expand_contract_border, smooth_removes_specks_and_feather_softens, polygon_area_and_aa
    void modifyAndPolygon() {
        const auto m = square(20, 20);
        const auto e = sx::expand(m, 20, 20, 2);
        QCOMPARE(e[10 * 20 + 3], 1.0f);
        QCOMPARE(e[10 * 20 + 2], 0.0f);
        const auto c = sx::contract(m, 20, 20, 2);
        QCOMPARE(c[10 * 20 + 6], 0.0f);
        QCOMPARE(c[10 * 20 + 7], 1.0f);
        const auto b = sx::border(m, 20, 20, 4);
        QCOMPARE(b[10 * 20 + 10], 0.0f);
        QVERIFY(b[10 * 20 + 5] > 0.5f && b[10 * 20 + 4] > 0.5f);
        auto speck = square(20, 20);
        speck[2 * 20 + 2] = 1.0f;
        const auto s = sx::smooth(speck, 20, 20, 1);
        QCOMPARE(s[2 * 20 + 2], 0.0f);
        QCOMPARE(s[10 * 20 + 10], 1.0f);
        const auto f = sx::feather(square(20, 20), 20, 20, 4);
        QVERIFY(f[10 * 20 + 5] > 0.3f && f[10 * 20 + 5] < 0.8f);
        QVERIFY(std::fabs(std::accumulate(f.begin(), f.end(), 0.0f) - 100.0f) < 1.0f);
        const sx::Rect a{0, 0, 20, 20};
        const auto sq = sx::polygon({{2, 2}, {12, 2}, {12, 12}, {2, 12}}, a, true);
        QVERIFY(std::fabs(std::accumulate(sq.begin(), sq.end(), 0.0f) - 100.0f) < 1e-3f);
        const auto tri = sx::polygon({{0, 0}, {10, 0}, {0, 10}}, a, true);
        QVERIFY(std::fabs(std::accumulate(tri.begin(), tri.end(), 0.0f) - 50.0f) < 1.0f);
        QVERIFY(std::any_of(tri.begin(), tri.end(), [](float v) { return v > 0 && v < 1; }));
        const auto hard = sx::polygon({{0, 0}, {10, 0}, {0, 10}}, a, false);
        QVERIFY(std::all_of(hard.begin(), hard.end(), [](float v) { return v == 0 || v == 1; }));
        QCOMPARE(count_selected(sx::polygon({{0, 0}, {1, 1}}, a, true)), std::size_t(0));
    }

    // The area kinds and modifiers: an area without the new ones resolved to the same bytes as before (the modifiers
    // pass the mask through untouched); "antialias" draws a grey edge; expand_mm, contract_mm, border_mm and smooth_mm
    // are the distance transform's, in their order after grow_mm; matrix moves the mask; the kinds made known to the bus.
    void areaKindsAndModifiers() {
        const core::Document doc = painted();
        const core::Page& page = doc.page(0);
        const Json poly{{"poly", Json::array({Json::array({10.3, 10.7}), Json::array({40.2, 12}), Json::array({30, 50.5})})}};
        const render::Image plain = render::selection::to_mask(poly, page, &doc);
        // (no new key: the mask the old language makes, byte for byte — Pillow's polygon, two-valued)
        const std::string plain_bytes = plain.tobytes();
        QVERIFY(std::all_of(plain_bytes.begin(), plain_bytes.end(), [](char v) { return v == 0 || static_cast<unsigned char>(v) == 255; }));
        Json with_false = poly;
        with_false["antialias"] = false;
        QCOMPARE(render::selection::to_mask(with_false, page, &doc).tobytes(), plain_bytes);
        Json smooth = poly;
        smooth["antialias"] = true;
        const std::string aa = render::selection::to_mask(smooth, page, &doc).tobytes();
        QVERIFY(std::any_of(aa.begin(), aa.end(), [](char v) { return v != 0 && static_cast<unsigned char>(v) != 255; }));
        // (the triangle's area, exact within the sub-rows: 0.5 |…| mm² at 200 dpi)
        double covered = 0;
        for (const char v : aa) covered += static_cast<unsigned char>(v) / 255.0;
        const double px = 200 / 25.4;
        const double want = 0.5 * std::fabs((40.2 - 10.3) * (50.5 - 10.7) - (30 - 10.3) * (12 - 10.7)) * px * px;
        QVERIFY2(std::fabs(covered - want) / want < 0.002, qPrintable(QString::number(covered) + " " + QString::number(want)));
        // expand_mm: the EDT's on the base mask
        Json grown = poly;
        grown["expand_mm"] = 1.0;
        const render::Image expanded = render::selection::to_mask(grown, page, &doc);
        const std::vector<float> want_expand = sx::expand(sx::from_l(plain), std::size_t(plain.width()), std::size_t(plain.height()), float(px));
        QCOMPARE(expanded.tobytes(), sx::to_l(want_expand, plain.size()).tobytes());
        // the order: grow_mm, then expand_mm, contract_mm, border_mm, smooth_mm, then invert
        Json all = poly;
        all["grow_mm"] = 0.5;
        all["contract_mm"] = 0.3;
        all["smooth_mm"] = 0.2;
        all["invert"] = true;
        Json grow_only = poly;
        grow_only["grow_mm"] = 0.5;
        auto m = sx::from_l(render::selection::to_mask(grow_only, page, &doc));
        m = sx::contract(m, std::size_t(plain.width()), std::size_t(plain.height()), float(0.3 * px));
        m = sx::smooth(m, std::size_t(plain.width()), std::size_t(plain.height()), float(0.2 * px));
        render::Image expected = sx::to_l(m, plain.size());
        expected = expected.point([](int v) { return 255 - v; });
        QCOMPARE(render::selection::to_mask(all, page, &doc).tobytes(), expected.tobytes());
        // matrix: moved 10 mm right, as the mask's pixels
        Json moved = Json{{"rect", Json::array({5, 5, 10, 10})}, {"matrix", Json::array({1, 0, 0, 1, 10, 0})}};
        const render::Image shifted = render::selection::to_mask(moved, page, &doc);
        const auto box = shifted.getbbox();
        QVERIFY(box.has_value());
        QVERIFY(std::abs(box->x0 - static_cast<int>(std::lround(15 * px))) <= 1 && std::abs(box->y0 - static_cast<int>(std::lround(5 * px))) <= 1);
        // the bus resolves an area holding them
        render::area_kinds::use_keys();
        QVERIFY(core::area_needs_resolving(smooth));
        QVERIFY(core::area_needs_resolving(Json{{"transparency", "x"}}));
        QVERIFY(!core::area_needs_resolving(poly));
    }

    // {"transparency": id}: the layer's alpha grey as it is ({"layer"} is two-valued); {"layer_mask": id}: the mask with its
    // density; a kind not written yet: not_yet_ported, naming it and its group; ops take the new kinds through the bus.
    void newKinds() {
        core::Document doc = painted();
        const core::Page& page = doc.page(0);
        const render::Image alpha = render::selection::to_mask(Json{{"transparency", "paint0000001"}}, page, &doc);
        QCOMPARE(static_cast<int>(alpha.getpixel(150, 150)[0]), 128);
        QCOMPARE(static_cast<int>(alpha.getpixel(300, 300)[0]), 255);
        QCOMPARE(static_cast<int>(alpha.getpixel(10, 300)[0]), 0);
        const render::Image two = render::selection::to_mask(Json{{"layer", "paint0000001"}}, page, &doc);
        QCOMPARE(static_cast<int>(two.getpixel(150, 150)[0]), 255);
        doc = run_ops(doc, Json::array({Json{{"op", "set_layer_mask"}, {"page", 1}, {"id", "paint0000001"}, {"fill", "hide"}, {"density", 0.25}}})).doc;
        const render::Image masked = render::selection::to_mask(Json{{"layer_mask", "paint0000001"}}, doc.page(0), &doc);
        QCOMPARE(static_cast<int>(masked.getpixel(200, 200)[0]), 191);  // (255 − 0.25 × 255)
        try {
            render::selection::to_mask(Json{{"range", Json{{"select", "reds"}}}}, page, &doc);
            QFAIL("a kind not written yet was solved");
        } catch (const core::Error& error) {
            QCOMPARE(error.code(), std::string("not_yet_ported"));
            QVERIFY(std::string(error.what()).find("range") != std::string::npos && std::string(error.what()).find("B1") != std::string::npos);
        }
        // (the layer's own alpha: its mask, hiding at a density of 0.25 now, not applied)
        QCOMPARE(static_cast<int>(render::selection::to_mask(Json{{"transparency", "paint0000001"}}, doc.page(0), &doc).getpixel(300, 300)[0]), 255);
        QVERIFY(refusal(doc, Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", "paint0000001"}, {"area", Json{{"path_name", "work"}}}, {"rgb", Json::array({1, 2, 3})}})
                    .starts_with("not_yet_ported"));
        // (a transparency area through the bus: the fill's patch covers grey where the alpha is)
        const core::Document filled = run_ops(doc, Json::array({Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", "paint0000001"},
                                                                     {"area", Json{{"transparency", "paint0000001"}}}, {"rgb", Json::array({255, 0, 0})}}}))
                                          .doc;
        const core::Layer after = layer_of(filled.page(0), "paint0000001");
        QCOMPARE(after.patches.size(), layer_of(doc.page(0), "paint0000001").patches.size() + 1);
        const core::Patch& patch = after.patches.back();
        const render::Image cover = render::open_image(*patch.png).convert("L");
        const double dots = 300 / 25.4;  // (a fill's patch: 300 dpi)
        const long left = std::lround(patch.attrs["box"][0].get<double>() * dots), top = std::lround(patch.attrs["box"][1].get<double>() * dots);
        const auto at = [&](double x_mm, double y_mm) {
            return static_cast<int>(cover.getpixel(static_cast<int>(std::lround(x_mm * dots) - left), static_cast<int>(std::lround(y_mm * dots) - top))[0]);
        };
        QVERIFY2(at(38.1, 38.1) > 250, std::to_string(at(38.1, 38.1)).c_str());  // (opaque: the fill whole)
        const int half = at(19.05, 19.05);
        QVERIFY2(half > 100 && half < 155, std::to_string(half).c_str());  // (half: the alpha's grey)
    }

    // masks@1: a mask's settings written only when held (a mask without them: the same bytes as before, no feature),
    // read back, refused out of range by the op, the book with a bad value read-only; the Quick Mask is one alpha channel.
    void masksFeature() {
        QTemporaryDir tmp;
        core::Document doc = run_ops(painted(), Json::array({Json{{"op", "set_layer_mask"}, {"page", 1}, {"id", "paint0000001"}, {"fill", "show"}}})).doc;
        QVERIFY(!layer_of(doc.page(0), "paint0000001").mask->linked);
        write_book(doc, fs::path(tmp.path().toStdString()) / "plain");
        const std::string plain = bytes_of(fs::path(tmp.path().toStdString()) / "plain" / "project.json");
        QVERIFY(plain.find("masks@1") == std::string::npos && plain.find("\"linked\"") == std::string::npos);
        const auto features = storage::load_document(fs::path(tmp.path().toStdString()) / "plain").document.features;
        QVERIFY(std::find(features.begin(), features.end(), "masks@1") == features.end());
        // the op's settings, kept when the mask's picture is made again
        doc = run_ops(doc, Json::array({Json{{"op", "set_layer_mask"}, {"page", 1}, {"id", "paint0000001"}, {"linked", true}, {"density", 0.5}, {"feather_mm", 2}},
                                        Json{{"op", "set_layer_mask"}, {"page", 1}, {"id", "paint0000001"}, {"fill", "hide"}},
                                        Json{{"op", "paint_mask"}, {"page", 1}, {"id", "paint0000001"}, {"points", Json::array({Json::array({10, 10}), Json::array({20, 20})})}}}))
                  .doc;
        const core::Mask& kept = *layer_of(doc.page(0), "paint0000001").mask;
        QVERIFY(kept.linked && kept.density == 0.5 && kept.feather_mm == 2.0);
        const fs::path set = fs::path(tmp.path().toStdString()) / "set";
        write_book(doc, set);
        const storage::LoadResult back = storage::load_document(set);
        QVERIFY(back.document.read_only_reason.empty());
        QVERIFY(std::find(back.document.features.begin(), back.document.features.end(), "masks@1") != back.document.features.end());
        const core::Mask& read = *layer_of(back.document.page(0), "paint0000001").mask;
        QVERIFY(read.linked && read.density == 0.5 && read.feather_mm == 2.0);
        QVERIFY(refusal(doc, Json{{"op", "set_layer_mask"}, {"page", 1}, {"id", "paint0000001"}, {"density", 1.5}}).find("density") != std::string::npos);
        // a book whose mask holds a density out of range: read-only
        Json payload = test::read_json(QString::fromStdString(core::path_to_utf8(set / "project.json")));
        for (Json& layer : payload["pages"][0]["layers"])
            if (layer.contains("mask")) layer["mask"]["density"] = 3;
        test::write_bytes(QString::fromStdString(core::path_to_utf8(set / "project.json")), core::dump_python_indent2(payload));
        QVERIFY(!storage::load_document(set).document.read_only_reason.empty());
        // the Quick Mask: one alpha channel, never a spot one
        core::Document quick = small_book();
        core::Channel channel;
        channel.id = "ch_000000000001";
        channel.name = "クイックマスク";
        channel.quick_mask = true;
        quick.edit_page(0).channels.push_back(channel);
        core::validate_masks(quick);
        QVERIFY(core::uses_masks(quick));
        QVERIFY(core::quick_mask_of(quick.page(0)) != nullptr);
        channel.id = "ch_000000000002";
        channel.name = "二つ目";
        quick.edit_page(0).channels.push_back(channel);
        QVERIFY_THROWS_EXCEPTION(core::Error, core::validate_masks(quick));
    }

    // Density and feather drawn: a mask's value m shows as 1 − density·(1 − m), feathered by a Gaussian of σ = feather/2;
    // a mask of neither the same pixels as before; on an 8-bit page, a precise page and an ink page alike.
    void densityAndFeatherDrawn() {
        core::Mask mask;
        render::Image half = render::Image::create("L", render::Size{100, 100}, render::Ink(255));
        half.paste(render::Ink(0), render::Box{0, 0, 50, 100});
        mask.png = render::fills::png_bytes(half);
        const core::Document doc = small_book();
        const render::Size size{100, 100};
        const render::Box all{0, 0, 100, 100};
        const render::Image plain = render::layer_masks::shown(doc.page(0), mask, half, size, all, render::Resample::Bilinear);
        QCOMPARE(plain.tobytes(), half.resize_region(size, all, render::Resample::Bilinear).tobytes());
        mask.density = 0.4;
        const render::Image dense = render::layer_masks::shown(doc.page(0), mask, half, size, all, render::Resample::Bilinear);
        QCOMPARE(static_cast<int>(dense.getpixel(10, 50)[0]), 153);  // (255 − 0.4 × 255)
        QCOMPARE(static_cast<int>(dense.getpixel(90, 50)[0]), 255);
        mask.density = 1.0;
        mask.feather_mm = 6.0;  // (σ = 3 mm on a 60 mm page of 100 px: 5 px)
        const render::Image soft = render::layer_masks::shown(doc.page(0), mask, half, size, all, render::Resample::Bilinear);
        const int at_edge = soft.getpixel(50, 50)[0];
        QVERIFY(at_edge > 100 && at_edge < 155);
        QVERIFY(soft.getpixel(40, 50)[0] < soft.getpixel(50, 50)[0] && soft.getpixel(60, 50)[0] > soft.getpixel(50, 50)[0]);
        QCOMPARE(static_cast<int>(soft.getpixel(5, 50)[0]), 0);
        // (a part of the page: the same values as the whole has there)
        const render::Image part = render::layer_masks::shown(doc.page(0), mask, half, size, render::Box{40, 40, 60, 60}, render::Resample::Bilinear);
        QCOMPARE(part.tobytes(), soft.crop(render::Box{40, 40, 60, 60}).tobytes());

        // drawn on the three kinds of page: the masked layer's pixels where the mask hides at a density of 0.5
        for (const char* mode : {"rgb8", "rgb16", "cmyk"}) {
            core::Document book = painted();
            if (std::string(mode) == "rgb16") {
                book = run_ops(book, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}}})).doc;
            } else if (std::string(mode) == "cmyk") {
                book = run_ops(book, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}}})).doc;
            }
            const core::Document hidden = run_ops(book, Json::array({Json{{"op", "set_layer_mask"}, {"page", 1}, {"id", "paint0000001"}, {"fill", "hide"}}})).doc;
            const core::Document half_hidden =
                run_ops(hidden, Json::array({Json{{"op", "set_layer_mask"}, {"page", 1}, {"id", "paint0000001"}, {"density", 0.5}}})).doc;
            render::RenderOptions options;
            options.mode = "print";
            const auto px = [&](const core::Document& d) { return render::render_page(d.page(0), 100, options, &d).image.convert("RGB").getpixel(150, 200); };
            const auto shown = px(book), none = px(hidden), mid = px(half_hidden);
            QVERIFY2(none[0] > shown[0] + 50, mode);                                          // (hidden: the paper)
            QVERIFY2(mid[0] > shown[0] + 10 && mid[0] < none[0] - 10, mode);                   // (half: between)
        }
    }

    // "layer_mask": true on a pixel op: the layer's mask painted (a layer without one: one that reveals all made first,
    // linked); the layer's pixels untouched; only the mask's pixels the op changed come back, the rest their bytes.
    void layerMaskLent() {
        core::Document doc = run_ops(painted(), Json::array({Json{{"op", "set_layer_mask"}, {"page", 1}, {"id", "paint0000001"}, {"fill", "show"}}})).doc;
        const std::string before = render::open_image(*layer_of(doc.page(0), "paint0000001").raster_png).convert("RGBA").tobytes();
        const render::Image mask_before = mask_pixels(layer_of(doc.page(0), "paint0000001"));
        const Json fill{{"op", "fill_area"}, {"page", 1}, {"layer_id", "paint0000001"}, {"layer_mask", true}, {"rgb", Json::array({0, 0, 0})},
                        {"area", Json{{"poly", Json::array({Json::array({30, 30}), Json::array({50, 30}), Json::array({50, 50}), Json::array({30, 50})})}}}};
        const core::ApplyResult made = run_ops(doc, Json::array({fill}));
        QVERIFY(made.warnings.empty());
        const core::Layer layer = layer_of(made.doc.page(0), "paint0000001");  // (a copy: gcc sees the name as a temporary)
        QCOMPARE(render::open_image(*layer.raster_png).convert("RGBA").tobytes(), before);
        const render::Image mask = mask_pixels(layer);
        QCOMPARE(static_cast<int>(mask.getpixel(240, 240)[0]), 0);   // (40 mm at 150 dpi: inside, hidden)
        QCOMPARE(static_cast<int>(mask.getpixel(60, 60)[0]), 255);
        // (the pixels the fill did not reach: their bytes)
        int same = 0, total = 0;
        for (int y = 0; y < mask.height(); y += 7) {
            for (int x = 0; x < mask.width(); x += 7) {
                if (x > 160 && x < 310 && y > 160 && y < 310) continue;
                ++total;
                same += mask.getpixel(x, y)[0] == mask_before.getpixel(x, y)[0] ? 1 : 0;
            }
        }
        QCOMPARE(same, total);
        // a layer without a mask: one made, linked
        core::Document bare = painted();
        const core::Document painted_mask = run_ops(bare, Json::array({fill})).doc;
        QVERIFY(layer_of(painted_mask.page(0), "paint0000001").mask.has_value());
        QVERIFY(layer_of(painted_mask.page(0), "paint0000001").mask->linked);
        QVERIFY(refusal(doc, Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", "paint0000001"}, {"layer_mask", true}, {"channel", "red"},
                                  {"rgb", Json::array({0, 0, 0})}, {"area", Json{{"all", true}}}})
                    .find("layer_mask and channel") != std::string::npos);
    }
};

QTEST_GUILESS_MAIN(TestSelectionExt)
#include "test_selection_ext.moc"
