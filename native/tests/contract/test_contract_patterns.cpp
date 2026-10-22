// PC4 E3 (patterns@1, SPEC.md COLOR-09「パターン」, schema-v4.md「パターン」): the book's patterns as a person or an agent
// uses them — add_pattern (a PNG, GKCR, GKIR; the same picture once), rename_pattern, delete_pattern (refused while a fill
// uses it); a pattern fill layer (add_layer, set_layer) drawn on 8-, 16-, 32-bit RGB, CMYK, Lab and grey pages (its
// values: a pattern of the page's mode as it is); fill_area's and fill's pattern (on an 8-bit layer a picture; on a
// precise or ink layer its pixels; inside the area only; its transparency kept; a blend); a shape part's pattern fill;
// rasterized; through the command line saved (patterns@1 named while the book keeps any), opened again the same, undone;
// read-only when a fill names a pattern the book does not keep; a PSD's Patt block and PtFl (a pattern fill layer, a shape
// layer's fill) read by psd-tools and by import_psd, a PSD without patterns as before; `genko patterns` (list, define,
// import and export of .pat, rename, delete, op, texture). (This build's own contract; PhotoCraft v0.2.0's
// pattern_cmds.rs tests: a pattern fill layer at every depth, undone; a fill with a pattern inside the selection only.)

#include <QtTest>

#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/base64.hpp"
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ink.hpp"
#include "core/json.hpp"
#include "core/patterns.hpp"
#include "render/image.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/patterns.hpp"
#include "render/png.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
namespace fs = std::filesystem;

namespace {

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

std::string ink_of(const core::Page& page) {
    for (const core::Layer& layer : page.layers)
        if (layer.role == core::LayerRole::Ink) return layer.id;
    throw std::runtime_error("no ink layer");
}

const core::Layer& layer_of(const core::Page& page, std::string_view id) {
    for (const core::Layer& layer : page.layers)
        if (layer.id == id) return layer;
    throw std::runtime_error("no layer " + std::string(id));
}

// A page of 60 × 80 mm without panels.
core::Document small_book() {
    core::PageSpec spec;
    spec.width_mm = core::Num(60);
    spec.height_mm = core::Num(80);
    spec.dpi = core::Num(150);
    spec.bleed_mm = core::Num(3);
    spec.inner_margin_mm = core::Num(5);
    core::Document doc = core::new_episode("patterns", core::Num(1), 1, spec, core::Binding::Right);
    doc.edit_page(0).frames.clear();
    return doc;
}

// An 8 × 8 checker of 4 × 4 squares: red at the top left and bottom right, blue in the others; opaque.
std::string checker_png() {
    std::string bytes;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            const bool red = (x < 4) == (y < 4);
            bytes += red ? std::string("\xff\x00\x00\xff", 4) : std::string("\x00\x00\xff\xff", 4);
        }
    return core::b64encode(render::write_png(render::Image::frombytes("RGBA", render::Size{8, 8}, bytes)));
}

// The checker's pattern at 100 dpi: one of its squares 4/100 inch (≈ 1.016 mm) across.
Json add_checker(const char* id = nullptr) {
    Json op{{"op", "add_pattern"}, {"name", "市松"}, {"png", checker_png()}, {"dpi", 100}};
    if (id != nullptr) op["id"] = id;
    return op;
}

render::Image drawn(const core::Document& doc, int dpi = 100) {
    render::RenderOptions options;
    options.mode = "print";
    return render::render_page(doc.page(0), dpi, options, &doc).image.convert("RGB");
}

std::array<int, 3> rgb_at(const render::Image& image, int x, int y) {
    const auto px = image.getpixel(x, y);
    return {static_cast<int>(px[0]), static_cast<int>(px[1]), static_cast<int>(px[2])};
}

bool reddish(const std::array<int, 3>& c) { return c[0] > c[2] + 60; }
bool bluish(const std::array<int, 3>& c) { return c[2] > c[0] + 60; }

// The page at 100 dpi (the checker's own): pixel (x, y) is its square ((x / 4 + y / 4) even: red).
bool red_square(int x, int y) { return (x / 4 + y / 4) % 2 == 0; }

core::Document on_page(const std::string& mode) {
    core::Document book = small_book();
    if (mode == "rgb16" || mode == "rgb32") book = run(book, Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", mode == "rgb16" ? 16 : 32}});
    if (mode == "cmyk") book = run(book, Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}});
    if (mode == "lab") book = run(book, Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "lab"}});
    if (mode == "gray") book = run(book, Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "gray"}});
    return book;
}

}  // namespace

class TestContractPatterns : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;
    QString path(const QString& rel) const { return tmp_.path() + QStringLiteral("/") + rel; }

    Json genko(const QStringList& args, int* exit = nullptr, const QProcessEnvironment& env = {}) {
        const auto ran = genko::test::run_genko(args, env);
        if (exit != nullptr) *exit = ran.exit_code;
        if (!ran.finished) return Json{{"not_run", ran.described().toStdString()}};
        return genko::test::one_line(ran.out);
    }
    QProcessEnvironment config_env() const {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("GENKO_CONFIG_DIR"), path(QStringLiteral("config")));
        return env;
    }
    QString make_book(const QString& name) {
        const QString book = path(name);
        const auto made = genko::test::run_genko({QStringLiteral("new"), book, QStringLiteral("--pages"), QStringLiteral("1")});
        if (made.exit_code != 0) qWarning("%s", qPrintable(made.described()));
        return book;
    }
    Json apply(const QString& book, const QString& name, const Json& ops) {
        const QString file = path(name);
        genko::test::write_bytes(file, ops.dump());
        int exit = -1;
        const Json reply = genko({QStringLiteral("apply"), book, file, QStringLiteral("--agent"), QStringLiteral("human:test")}, &exit);
        if (exit != 0 || !reply.value("ok", false)) qWarning("%s", reply.dump().c_str());
        return reply;
    }
    Json project(const QString& book) { return Json::parse(genko::test::read_bytes(book + QStringLiteral("/project.json"))); }
    static bool has_feature(const Json& saved, const char* feature) {
        return saved.contains("features") && std::find(saved["features"].begin(), saved["features"].end(), Json(feature)) != saved["features"].end();
    }

private slots:
    void initTestCase() { QVERIFY(tmp_.isValid()); }

    // add_pattern (a PNG — RGB or grey —, GKCR of 16 bits, GKIR CMYK; the same picture, mode, depth and dpi once),
    // rename_pattern, delete_pattern (refused while a fill uses it; its picture gone with the last pattern of it); what is
    // not well formed refused, the book as it was.
    void theBooksPatterns() {
        core::Document book = small_book();
        core::ApplyResult made = run_ops(book, Json::array({add_checker()}));
        QCOMPARE(made.results[0], (Json{{"pattern", "pat-1"}, {"added", true}}));
        book = made.doc;
        const core::Pattern& p = book.patterns.at(0);
        QCOMPARE(p.mode, std::string("rgb"));
        QCOMPARE(p.depth, 8);
        QCOMPARE(p.width, std::int64_t(8));
        QCOMPARE(p.dpi, 100.0);
        QVERIFY(book.pattern_files.contains(p.asset));
        made = run_ops(book, Json::array({add_checker()}));  // (the same again: the one it keeps)
        QCOMPARE(made.results[0], (Json{{"pattern", "pat-1"}, {"added", false}}));
        QCOMPARE(made.doc.patterns.size(), std::size_t(1));
        Json other_dpi = add_checker();
        other_dpi["dpi"] = 300;
        book = run(book, other_dpi);
        QCOMPARE(book.patterns.size(), std::size_t(2));
        // grey (an L PNG: kept as LA), 16-bit RGB (GKCR), CMYK (GKIR)
        const std::string grey = render::write_png(render::Image::frombytes("L", render::Size{2, 2}, std::string("\x00\x40\x80\xff", 4)));
        made = run_ops(book, Json::array({Json{{"op", "add_pattern"}, {"name", "灰"}, {"png", core::b64encode(grey)}}}));
        book = made.doc;
        QCOMPARE(core::find_pattern(book, made.results[0]["pattern"].get<std::string>())->mode, std::string("gray"));
        const std::string deep = core::encode_color_pixels(3, 2, "u16", [](std::size_t i) { return std::array<double, 4>{0.25, i / 6.0, 0.5, 1.0}; });
        made = run_ops(book, Json::array({Json{{"op", "add_pattern"}, {"name", "深い"}, {"gkcr", core::b64encode(deep)}}}));
        book = made.doc;
        QCOMPARE(core::find_pattern(book, made.results[0]["pattern"].get<std::string>())->depth, 16);
        core::InkPixels inks = core::blank_ink_pixels(core::InkSpace::Cmyk, 8, 2, 2);
        for (std::size_t i = 0; i < 4; ++i) {
            inks.set(i, 0, i / 4.0);
            inks.set(i, 4, 1.0);
        }
        made = run_ops(book, Json::array({Json{{"op", "add_pattern"}, {"name", "版"}, {"gkir", core::b64encode(core::encode_ink_raster(inks))}}}));
        book = made.doc;
        const std::string cmyk = made.results[0]["pattern"].get<std::string>();
        QCOMPARE(core::find_pattern(book, cmyk)->mode, std::string("cmyk"));
        // renamed; deleted (unused: its picture gone with it)
        book = run(book, Json{{"op", "rename_pattern"}, {"id", "pat-1"}, {"name", "チェック"}});
        QCOMPARE(core::find_pattern(book, "pat-1")->name, std::string("チェック"));
        const std::string asset = core::find_pattern(book, cmyk)->asset;
        book = run(book, Json{{"op", "delete_pattern"}, {"id", cmyk}});
        QVERIFY(core::find_pattern(book, cmyk) == nullptr);
        QVERIFY(!book.pattern_files.contains(asset));
        // used by a fill: not deleted
        book = run(book, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "fill"}, {"id", "pat-layer"}, {"pattern", Json{{"id", "pat-1"}}}});
        QVERIFY(refusal(book, Json{{"op", "delete_pattern"}, {"id", "pat-1"}}).find("used by a fill") != std::string::npos);
        QVERIFY(core::patterns_used(book).contains("pat-1"));
        // refused: no such pattern, two pictures, a mode of no picture, a fill naming none
        QVERIFY(!refusal(book, Json{{"op", "rename_pattern"}, {"id", "nope"}, {"name", "x"}}).empty());
        QVERIFY(!refusal(book, Json{{"op", "add_pattern"}, {"png", checker_png()}, {"gkcr", core::b64encode(deep)}}).empty());
        QVERIFY(!refusal(book, Json{{"op", "add_pattern"}, {"png", checker_png()}, {"mode", "cmyk"}}).empty());
        QVERIFY(!refusal(book, Json{{"op", "add_pattern"}, {"png", checker_png()}, {"dpi", 10}}).empty());
        QVERIFY(refusal(book, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "fill"}, {"pattern", Json{{"id", "nope"}}}}).find("no pattern") != std::string::npos);
        QVERIFY(!refusal(book, Json{{"op", "set_layer"}, {"page", 1}, {"id", "pat-layer"}, {"fill", Json{{"pattern", Json{{"id", "pat-1"}, {"scale", 0}}}}}}).empty());
    }

    // upstream pattern_fill_layer_renders_at_every_depth: a pattern fill layer drawn on 8-, 16-, 32-bit RGB, CMYK, Lab
    // and grey pages — its squares where they are (a whole-pixel placement at the pattern's dpi), its scale and phase
    // moving them; a pattern of the page's mode (CMYK on a CMYK page) as its inks.
    void aPatternFillLayerOnEveryKindOfPage() {
        for (const char* mode : {"rgb8", "rgb16", "rgb32", "cmyk", "lab", "gray"}) {
            const std::string m = mode;
            core::Document book = on_page(m);
            book = run_ops(book, Json::array({add_checker(), Json{{"op", "add_layer"}, {"page", 1}, {"kind", "fill"}, {"id", "pl"}, {"pattern", Json{{"id", "pat-1"}}}}})).doc;
            const core::Layer& layer = layer_of(book.page(0), "pl");
            QCOMPARE(layer.fill->value("pattern", Json())["scale"], Json(100));
            QCOMPARE(layer.title, std::string("パターン"));
            const render::Image seen = drawn(book);
            for (const auto& [x, y] : {std::pair{1, 1}, {5, 1}, {1, 5}, {6, 6}, {10, 2}, {21, 14}}) {
                const auto c = rgb_at(seen, x, y);
                if (m == "gray") {  // (red and blue as greys: red the lighter)
                    const auto other = rgb_at(seen, x + 4, y);
                    QVERIFY2(red_square(x, y) ? c[0] > other[0] : c[0] < other[0], (m + " " + std::to_string(x) + "," + std::to_string(y)).c_str());
                    continue;
                }
                QVERIFY2(red_square(x, y) ? reddish(c) : bluish(c),
                         (m + " at " + std::to_string(x) + "," + std::to_string(y) + ": " + std::to_string(c[0]) + " " + std::to_string(c[2])).c_str());
            }
            // scaled 200 %: a square 8 pixels across; moved by its phase (4 squares' width: the same picture)
            const core::Document big = run(book, Json{{"op", "set_layer"}, {"page", 1}, {"id", "pl"}, {"fill", Json{{"pattern", Json{{"id", "pat-1"}, {"scale", 200}}}}}});
            const render::Image twice = drawn(big);
            if (m != "gray") {
                QVERIFY2(reddish(rgb_at(twice, 6, 2)) && bluish(rgb_at(twice, 10, 2)), mode);
            }
            const double whole = 8 * 25.4 / 100.0;  // (one tile: the same picture)
            const core::Document moved =
                run(book, Json{{"op", "set_layer"}, {"page", 1}, {"id", "pl"}, {"fill", Json{{"pattern", Json{{"id", "pat-1"}, {"phase_mm", Json::array({whole, whole})}}}}}});
            const render::Image same = drawn(moved);
            QVERIFY2(rgb_at(same, 12, 12) == rgb_at(seen, 12, 12) && rgb_at(same, 30, 9) == rgb_at(seen, 30, 9), mode);
        }
        // a CMYK pattern on a CMYK page: its inks as they are (K only: a grey on K alone, not four inks)
        core::Document book = on_page("cmyk");
        core::InkPixels k = core::blank_ink_pixels(core::InkSpace::Cmyk, 8, 1, 1);
        k.set(0, 3, 1.0);
        k.set(0, 4, 1.0);
        book = run_ops(book, Json::array({Json{{"op", "add_pattern"}, {"name", "墨"}, {"gkir", core::b64encode(core::encode_ink_raster(k))}},
                                         Json{{"op", "add_layer"}, {"page", 1}, {"kind", "fill"}, {"id", "kl"}, {"pattern", Json{{"id", "pat-1"}}}}}))
                   .doc;
        render::RenderOptions options;
        options.mode = "print";
        options.inks = true;
        const render::RenderResult result = render::render_page(book.page(0), 100, options, &book);
        QVERIFY(result.inks.has_value());
        const std::size_t at = 10 * result.inks->width + 10;
        QCOMPARE(result.inks->value(at, 0), 0.0);
        QCOMPARE(result.inks->value(at, 3), 1.0);
    }

    // upstream fill_with_pattern_respects_selection: fill_area's pattern inside the area only — on an 8-bit pen layer a
    // picture laid on it; on precise and ink paint layers in their pixels —; its transparency kept; in a blend; fill's
    // (the bucket's) too; a shape part's pattern fill; and rasterized, the look the same.
    void fillsAndPartsWithAPattern() {
        for (const char* mode : {"rgb8", "rgb16", "cmyk"}) {
            const std::string m = mode;
            core::Document book = on_page(m);
            book = run_ops(book, Json::array({add_checker(), Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "paint1"}}})).doc;
            const core::Document filled = run(book, Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", "paint1"},
                                                         {"area", Json{{"rect", Json::array({0, 0, 20.32, 20.32})}}}, {"pattern", Json{{"id", "pat-1"}}}});
            const core::Layer& layer = layer_of(filled.page(0), "paint1");
            if (m == "rgb8") {
                QCOMPARE(layer.patches.size(), std::size_t(1));  // (a picture laid on the 8-bit layer)
            } else if (m == "rgb16") {
                QVERIFY(layer.color_raster != nullptr);  // (its precise pixels)
            } else {
                QVERIFY(layer.ink_raster != nullptr);  // (its inks)
            }
            const render::Image seen = drawn(filled);
            QVERIFY2(reddish(rgb_at(seen, 1, 1)) && bluish(rgb_at(seen, 5, 1)), (m + " inside").c_str());
            const auto outside = rgb_at(seen, 60, 60);
            QVERIFY2(outside[0] > 240 && outside[2] > 240, (m + " outside").c_str());
        }
        // its transparency kept: only where the layer had pixels
        core::Document book = small_book();
        book = run_ops(book, Json::array({add_checker(), Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "paint1"}},
                                         Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", "paint1"}, {"area", Json{{"rect", Json::array({0, 0, 10, 10})}}},
                                              {"rgb", Json::array({0, 200, 0})}}}))
                   .doc;
        const core::Document kept = run(book, Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", "paint1"}, {"area", Json{{"all", true}}},
                                                   {"pattern", Json{{"id", "pat-1"}}}, {"preserve_transparency", true}});
        const render::Image k = drawn(kept);
        QVERIFY(reddish(rgb_at(k, 1, 1)));
        const auto clear = rgb_at(k, 60, 60);
        QVERIFY(clear[0] > 240 && clear[1] > 240 && clear[2] > 240);
        // in a blend (multiply over the green): darker than either
        const core::Document blended = run(book, Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", "paint1"}, {"area", Json{{"rect", Json::array({0, 0, 10, 10})}}},
                                                      {"pattern", Json{{"id", "pat-1"}}}, {"blend", "multiply"}});
        const auto mixed = rgb_at(drawn(blended), 1, 1);
        QVERIFY2(mixed[0] < 40 && mixed[1] < 40, (std::to_string(mixed[0]) + " " + std::to_string(mixed[1])).c_str());
        // a pattern given whole: put into the book with the op (its undo takes it out)
        const core::ApplyResult whole = run_ops(small_book(), Json::array({Json{{"op", "fill_area"}, {"page", 1}, {"layer", "ink"},
                                                                                 {"area", Json{{"rect", Json::array({0, 0, 10, 10})}}},
                                                                                 {"pattern", Json{{"png", checker_png()}, {"dpi", 100}, {"scale", 50}}}}}));
        QCOMPARE(whole.doc.patterns.size(), std::size_t(1));
        QCOMPARE(whole.results[0]["pattern"], Json("pat-1"));
        // fill (the bucket): the area the lines close
        core::Document ring = small_book();
        ring = run_ops(ring, Json::array({add_checker(), Json{{"op", "add_stroke"}, {"page", 1}, {"layer", "ink"},
                                                              {"points", Json::array({Json::array({5, 5}), Json::array({40, 5}), Json::array({40, 40}),
                                                                                      Json::array({5, 40}), Json::array({5, 5})})},
                                                              {"width_mm", 1}}}))
                   .doc;
        const core::Document bucket = run(ring, Json{{"op", "fill"}, {"page", 1}, {"layer", "ink"}, {"x_mm", 20}, {"y_mm", 20}, {"pattern", Json{{"id", "pat-1"}}}});
        const render::Image b = drawn(bucket);
        const int x20 = static_cast<int>(20 / 25.4 * 100);
        QVERIFY(reddish(rgb_at(b, x20, x20)) || bluish(rgb_at(b, x20, x20)));
        const auto beyond = rgb_at(b, 60, 60 * 2);
        QVERIFY(beyond[0] > 240 && beyond[2] > 240);
        // a shape part's pattern fill
        core::Document shaped = run_ops(small_book(), Json::array({add_checker(), Json{{"op", "add_shape_part"}, {"page", 1}, {"layer", "ink"},
                                                                                       {"shape", Json{{"kind", "rect"}, {"rect", Json::array({0, 0, 20.32, 20.32})}}},
                                                                                       {"fill", Json{{"pattern", Json{{"id", "pat-1"}}}}}}}))
                                   .doc;
        const render::Image s = drawn(shaped);
        QVERIFY(reddish(rgb_at(s, 1, 1)) && bluish(rgb_at(s, 5, 1)));
        QVERIFY(core::patterns_used(shaped).contains("pat-1"));
        // rasterized (8-bit, 16-bit, CMYK): its look the same
        for (const char* mode : {"rgb8", "rgb16", "cmyk"}) {
            core::Document page = on_page(mode);
            page = run_ops(page, Json::array({add_checker(), Json{{"op", "add_layer"}, {"page", 1}, {"kind", "fill"}, {"id", "pl"}, {"pattern", Json{{"id", "pat-1"}}}}})).doc;
            const core::Document flat = run(page, Json{{"op", "rasterize_layer"}, {"page", 1}, {"id", "pl"}});
            QVERIFY(layer_of(flat.page(0), "pl").kind == core::LayerKind::Raster);
            const render::Image before = drawn(page, 200), after = drawn(flat, 200);
            for (const auto& [x, y] : {std::pair{3, 3}, {11, 3}, {40, 40}}) {
                const auto a = rgb_at(before, x, y), c = rgb_at(after, x, y);
                QVERIFY2(std::abs(a[0] - c[0]) <= 2 && std::abs(a[2] - c[2]) <= 2, mode);
            }
        }
    }

    // Through the command line: the patterns saved (patterns@1 named while the book keeps any, the picture its asset),
    // opened again the same, undone; a fill naming a pattern the book does not keep: read-only.
    void savedOpenedAndUndone() {
        const QString book = make_book(QStringLiteral("p.genko"));
        QVERIFY(!has_feature(project(book), "patterns@1"));
        Json reply = apply(book, QStringLiteral("a.json"),
                           Json::array({add_checker(), Json{{"op", "add_layer"}, {"page", 1}, {"kind", "fill"}, {"id", "pl"}, {"pattern", Json{{"id", "pat-1"}, {"angle", 30}}}}}));
        QVERIFY2(reply.value("ok", false), reply.dump().c_str());
        Json saved = project(book);
        QVERIFY(has_feature(saved, "patterns@1"));
        QCOMPARE(saved["patterns"].size(), std::size_t(1));
        QCOMPARE(saved["patterns"][0]["id"], Json("pat-1"));
        const std::string asset = saved["patterns"][0]["asset"].get<std::string>();
        QVERIFY(fs::exists(fs::path(book.toStdString()) / "assets" / asset.substr(7, 2) / (asset.substr(7) + ".png")));
        bool found = false;
        for (const Json& layer : saved["pages"][0]["layers"]) {
            if (layer.value("id", std::string()) != "pl") continue;
            found = true;
            QCOMPARE(layer["fill"]["pattern"]["angle"], Json(30));
        }
        QVERIFY(found);
        int exit = -1;
        // opened again the same (and written again: not read-only)
        reply = apply(book, QStringLiteral("b.json"), Json::array({Json{{"op", "rename_pattern"}, {"id", "pat-1"}, {"name", "チェック"}}}));
        QVERIFY2(reply.value("ok", false), reply.dump().c_str());
        QCOMPARE(project(book)["patterns"][0]["name"], Json("チェック"));
        reply = genko({QStringLiteral("undo"), book}, &exit);
        QVERIFY2(exit == 0, reply.dump().c_str());
        // undone: the layer and the pattern gone, the feature too
        reply = genko({QStringLiteral("undo"), book}, &exit);
        QVERIFY2(exit == 0, reply.dump().c_str());
        saved = project(book);
        QVERIFY(!has_feature(saved, "patterns@1"));
        QVERIFY(!saved.contains("patterns"));
        // a fill naming a pattern the book does not keep: read-only (opened, not written)
        reply = genko({QStringLiteral("redo"), book}, &exit);
        QVERIFY2(exit == 0, reply.dump().c_str());
        saved = project(book);
        QVERIFY(has_feature(saved, "patterns@1"));
        saved["patterns"][0]["id"] = "pat-9";
        genko::test::write_bytes(book + QStringLiteral("/project.json"), saved.dump());
        genko::test::write_bytes(path(QStringLiteral("c.json")), Json::array({Json{{"op", "rename_pattern"}, {"id", "pat-9"}, {"name", "x"}}}).dump());
        const Json refused = genko({QStringLiteral("apply"), book, path(QStringLiteral("c.json")), QStringLiteral("--agent"), QStringLiteral("human:test")}, &exit);
        QVERIFY(exit != 0);
        QVERIFY2(refused.dump().find("read_only") != std::string::npos && refused.dump().find("pat-1") != std::string::npos, refused.dump().c_str());
    }

    // PSD: the pattern fill layer as Photoshop's (PtFl) and a shape part's pattern fill, their patterns in the Patt
    // block — read by psd-tools and by import_psd (the same pattern back, its fill layer a pattern fill layer); a book
    // without patterns writes no Patt.
    void psdPatterns() {
        const QString book = make_book(QStringLiteral("psd.genko"));
        Json reply = apply(book, QStringLiteral("psd.json"),
                           Json::array({add_checker(),
                                        Json{{"op", "add_layer"}, {"page", 1}, {"kind", "fill"}, {"id", "pl"}, {"name", "模様"}, {"pattern", Json{{"id", "pat-1"}, {"scale", 200}}}},
                                        Json{{"op", "set_layer"}, {"page", 1}, {"id", "pl"}, {"panel_clip", false}},
                                        Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "sh"}, {"name", "形"}},
                                        Json{{"op", "set_layer"}, {"page", 1}, {"id", "sh"}, {"panel_clip", false}, {"panel_each", false}},
                                        Json{{"op", "add_shape_part"}, {"page", 1}, {"layer_id", "sh"}, {"shape", Json{{"kind", "rect"}, {"rect", Json::array({20, 20, 40, 40})}}},
                                             {"fill", Json{{"pattern", Json{{"id", "pat-1"}}}}}}}));
        QVERIFY2(reply.value("ok", false), reply.dump().c_str());
        int exit = -1;
        const QString file = path(QStringLiteral("p.psd"));
        const Json out = genko({QStringLiteral("export"), book, file, QStringLiteral("--format"), QStringLiteral("psd"), QStringLiteral("--dpi"), QStringLiteral("100"),
                                QStringLiteral("--json")},
                               &exit);
        QVERIFY2(exit == 0, out.dump().c_str());
        const std::string bytes = genko::test::read_bytes(file);
        QVERIFY(bytes.find("8BIMPatt") != std::string::npos);
        QVERIFY(bytes.find("PtFl") != std::string::npos);
        QVERIFY2(!genko::test::python_ref().isEmpty(), "the reference Python (GENKO_PYREF) reads the PSD with psd-tools");
        constexpr const char* kRead = R"PY(
import json, sys
from psd_tools import PSDImage
from psd_tools.constants import Tag
psd = PSDImage.open(sys.argv[2])
out = {"layers": {}, "patterns": []}
blocks = psd._record.layer_and_mask_information.tagged_blocks
for key in (Tag.PATTERNS1, Tag.PATTERNS2, Tag.PATTERNS3):
    if blocks is not None and key in blocks:
        for p in blocks.get_data(key):
            out["patterns"].append({"name": p.name.strip("\x00"), "id": p.pattern_id, "w": p.data.rectangle[3] - p.data.rectangle[1]})
for l in psd.descendants():
    tb = l.tagged_blocks
    out["layers"][l.name] = {"kind": l.kind, "ptfl": tb is not None and Tag.PATTERN_FILL_SETTING in tb,
                             "vscg": tb is not None and Tag.VECTOR_STROKE_CONTENT_DATA in tb}
json.dump(out, open(sys.argv[1], "w"), ensure_ascii=False)
)PY";
        const QString json = file + QStringLiteral(".json");
        const auto ran = genko::test::run(genko::test::python_ref(), {QStringLiteral("-c"), QString::fromUtf8(kRead), json, file},
                                          genko::test::python_env(path(QStringLiteral("pyenv"))));
        QVERIFY2(ran.exit_code == 0, ran.err.constData());
        const Json view = genko::test::read_json(json);
        QCOMPARE(view["patterns"].size(), std::size_t(1));
        QCOMPARE(view["patterns"][0]["name"], Json("市松"));
        QCOMPARE(view["patterns"][0]["w"], Json(8));
        QVERIFY2(view["layers"].contains("模様") && view["layers"]["模様"]["ptfl"].get<bool>(), view.dump().c_str());
        QVERIFY2(view["layers"].contains("形") && view["layers"]["形"]["vscg"].get<bool>(), view.dump().c_str());
        // read back: the pattern fill layer and the shape part's pattern, the pattern the same picture
        const QString again = make_book(QStringLiteral("again.genko"));
        reply = apply(again, QStringLiteral("i.json"), Json::array({Json{{"op", "import_psd"}, {"page", 1}, {"path", file.toStdString()}, {"fit", "paper"}}}));
        QVERIFY2(reply.value("ok", false), reply.dump().c_str());
        const Json read = project(again);
        QVERIFY(has_feature(read, "patterns@1"));
        QCOMPARE(read["patterns"].size(), std::size_t(1));
        QCOMPARE(read["patterns"][0]["asset"], project(book)["patterns"][0]["asset"]);
        bool fill = false;
        for (const Json& layer : read["pages"][0]["layers"]) {
            if (layer.value("kind", std::string()) != "fill") continue;
            fill = true;
            const Json& p = layer["fill"]["pattern"];
            QCOMPARE(p["id"], read["patterns"][0]["id"]);
            QVERIFY2(std::fabs(p["scale"].get<double>() - 200) < 1, p.dump().c_str());
        }
        QVERIFY(fill);
        // a book without patterns: no Patt, no PtFl
        const QString plain = make_book(QStringLiteral("plain.genko"));
        const QString plain_file = path(QStringLiteral("plain.psd"));
        (void)genko({QStringLiteral("export"), plain, plain_file, QStringLiteral("--format"), QStringLiteral("psd"), QStringLiteral("--dpi"), QStringLiteral("100")}, &exit);
        QCOMPARE(exit, 0);
        const std::string plain_bytes = genko::test::read_bytes(plain_file);
        QVERIFY(plain_bytes.find("8BIMPatt") == std::string::npos && plain_bytes.find("PtFl") == std::string::npos);
    }

    // `genko patterns`: the library (its built-in groups), a pattern defined from a page (its area's box), a .pat
    // exported and imported again, renamed, deleted, its add_pattern op put into a book, its grey (brush.texturePattern).
    void theLibraryFromTheCommandLine() {
        const QProcessEnvironment env = config_env();
        int exit = -1;
        Json listed = genko({QStringLiteral("patterns"), QStringLiteral("list")}, &exit, env);
        QVERIFY2(exit == 0, listed.dump().c_str());
        QCOMPARE(listed["groups"][0]["name"], Json("幾何学"));
        const QString book = make_book(QStringLiteral("lib.genko"));
        Json reply = apply(book, QStringLiteral("l.json"),
                           Json::array({Json{{"op", "fill_area"}, {"page", 1}, {"layer", "ink"}, {"area", Json{{"rect", Json::array({10, 10, 10, 10})}}},
                                             {"rgb", Json::array({200, 0, 0})}}}));
        QVERIFY(reply.value("ok", false));
        Json defined = genko({QStringLiteral("patterns"), QStringLiteral("define"), book, QStringLiteral("--page"), QStringLiteral("1"), QStringLiteral("--rect"),
                              QStringLiteral("5,5,20,20"), QStringLiteral("--name"), QStringLiteral("赤い所")},
                             &exit, env);
        QVERIFY2(exit == 0, defined.dump().c_str());
        QCOMPARE(defined["name"], Json("赤い所"));
        QCOMPARE(defined["mode"], Json("rgb"));
        const int side = defined["width"].get<int>();
        QVERIFY2(std::abs(side - static_cast<int>(std::lround(20 / 25.4 * defined["dpi"].get<double>()))) <= 2, defined.dump().c_str());
        const QString pat = path(QStringLiteral("lib.pat"));
        Json exported = genko({QStringLiteral("patterns"), QStringLiteral("export"), pat, QStringLiteral("--patterns"), QStringLiteral("赤い所,水玉")}, &exit, env);
        QVERIFY2(exit == 0 && exported["count"] == Json(2), exported.dump().c_str());
        QVERIFY(genko::test::read_bytes(pat).starts_with("8BPT"));
        Json renamed = genko({QStringLiteral("patterns"), QStringLiteral("rename"), QStringLiteral("赤い所"), QStringLiteral("赤")}, &exit, env);
        QVERIFY2(exit == 0, renamed.dump().c_str());
        Json deleted = genko({QStringLiteral("patterns"), QStringLiteral("delete"), QStringLiteral("赤")}, &exit, env);
        QVERIFY2(exit == 0, deleted.dump().c_str());
        Json imported = genko({QStringLiteral("patterns"), QStringLiteral("import"), pat, QStringLiteral("--group"), QStringLiteral("読み込んだ")}, &exit, env);
        QVERIFY2(exit == 0 && imported["imported"].size() == 2, imported.dump().c_str());
        QCOMPARE(imported["imported"][0], Json("赤い所"));
        Json op = genko({QStringLiteral("patterns"), QStringLiteral("op"), QStringLiteral("赤い所")}, &exit, env);
        QVERIFY2(exit == 0 && op["op"] == Json("add_pattern"), op.dump().c_str());
        reply = apply(book, QStringLiteral("o.json"), Json::array({op}));
        QVERIFY2(reply.value("ok", false), reply.dump().c_str());
        QCOMPARE(project(book)["patterns"].size(), std::size_t(1));
        Json texture = genko({QStringLiteral("patterns"), QStringLiteral("texture"), QStringLiteral("水玉")}, &exit, env);
        QVERIFY2(exit == 0 && texture.contains("png"), texture.dump().c_str());
        const render::Image grey = render::read_png(core::a2b_base64(texture["png"].get<std::string>()));
        QCOMPARE(grey.mode(), std::string("L"));
        QCOMPARE(grey.size(), (render::Size{12, 12}));
        Json reset = genko({QStringLiteral("patterns"), QStringLiteral("reset")}, &exit, env);
        QVERIFY2(exit == 0, reset.dump().c_str());
        listed = genko({QStringLiteral("patterns"), QStringLiteral("list")}, &exit, env);
        QCOMPARE(listed["groups"].size(), std::size_t(2));
    }
};

QTEST_GUILESS_MAIN(TestContractPatterns)
#include "test_contract_patterns.moc"
