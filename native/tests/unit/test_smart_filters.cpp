// smart-objects@1, PC3 A2 (SPEC.md COLOR-05「スマートフィルター」): a smart object's smart filters — set again from the
// contents, never from the filtered pixels (PhotoCraft's tests.rs:125-156: the same settings, the same pixels; hidden, the
// picture as it was), their order, blend modes and opacities, all of them off and on, cleared (tests.rs:158-191), the
// selection made the filter mask (tests.rs:193-211), following the smart object's transforms (tests.rs:253-264); drawn at
// 200 dpi as rasterize_layer and filter_raster draw them (8 bits, 16 bits, CMYK, Lab), a part of the page the whole cut
// there; what cannot be a smart filter refused; written and read back with their mask, and what a reader refuses.

#include <QtTest>

#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>

#include "core/base64.hpp"
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/model.hpp"
#include "core/paths.hpp"
#include "core/smart.hpp"
#include "render/image.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "storage/asset_store.hpp"
#include "storage/reader.hpp"
#include "testsupport.hpp"

namespace core = genko::core;
namespace render = genko::render;
namespace storage = genko::storage;
namespace fs = std::filesystem;
using core::Actor;
using core::Document;
using core::Json;
using core::Num;

namespace {

const core::CommandBus& bus() {
    static const core::CommandBus b(render::ops_registry());
    return b;
}

Document run(const Document& doc, const Json& ops) { return bus().apply(doc, ops, Actor("human:作者")).doc; }

// The op refused: what it says.
std::string refused(const Document& doc, const Json& op) {
    try {
        (void)bus().apply(doc, Json::array({op}), Actor("human:作者"));
    } catch (const std::exception& error) {
        return error.what();
    }
    return {};
}

core::PageSpec colour_page() {
    core::PageSpec spec = core::PageSpec::a5_doujin();
    spec.expression = "color";
    return spec;
}

// A picture with colours, greys, sharp edges and an alpha that fades.
std::string picture_png(int w, int h) {
    std::string bytes(std::size_t(w) * h * 4, '\0');
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            unsigned char* p = reinterpret_cast<unsigned char*>(bytes.data()) + (std::size_t(y) * w + x) * 4;
            const bool square = (x / 8 + y / 8) % 2 == 0;
            p[0] = static_cast<unsigned char>(square ? 230 : (x * 7) % 256);
            p[1] = static_cast<unsigned char>(square ? 40 : (y * 5) % 256);
            p[2] = static_cast<unsigned char>((x * y) % 256);
            p[3] = static_cast<unsigned char>(x < w / 6 ? 0 : std::min(255, 90 + (x + y) * 2));
        }
    }
    return render::write_png(render::Image::frombytes("RGBA", render::Size{w, h}, bytes));
}

// A one-page colour book with a paint layer "p1" (a picture over part of the page) made a smart object "so", in the
// colour mode the op gives (none: 8 bits, sRGB).
Document book(const Json& mode = Json()) {
    Document doc = core::new_episode("フィルター", Num(1), 1, colour_page());
    if (!mode.is_null()) doc = run(doc, Json::array({mode}));
    return run(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "p1"}, {"name", "絵"}},
                                 Json{{"op", "put_raster"}, {"page", 1}, {"id", "p1"}, {"png_base64", core::b64encode(picture_png(120, 90))}},
                                 Json{{"op", "smart_convert"}, {"page", 1}, {"id", "p1"}, {"new_id", "so"}}}));
}

render::RenderResult drawn(const Document& doc, int dpi = 60, std::optional<render::RenderRegion> region = std::nullopt, const std::string& mode = "proof") {
    render::RenderOptions options;
    options.mode = mode;
    options.inks = doc.page(0).color.has_value();
    options.precise = !doc.page(0).color && doc.page(0).rgb && doc.page(0).rgb->depth > 8;
    options.region = region;
    return render::render_page(doc.page(0), dpi, options, &doc);
}

std::string pixels(const Document& doc, int dpi = 60) { return drawn(doc, dpi).image.tobytes(); }

// The most two pictures of the same size are apart in a sample (256: not the same size).
int most_apart(const render::Image& a, const render::Image& b) {
    const std::string x = a.convert("RGB").tobytes(), y = b.convert("RGB").tobytes();
    if (x.size() != y.size()) return 256;
    int most = 0;
    for (std::size_t i = 0; i < x.size(); ++i) most = std::max(most, std::abs(int(static_cast<unsigned char>(x[i])) - int(static_cast<unsigned char>(y[i]))));
    return most;
}

const core::Layer& so_of(const Document& doc) {
    for (const core::Layer& l : doc.page(0).layers)
        if (l.id == "so") return l;
    throw std::runtime_error("no smart object");
}

const Json& filters_of(const Document& doc) { return (*so_of(doc).smart)["filters"]; }

Json op(const char* name, Json more = Json::object()) {
    Json out{{"op", name}, {"page", 1}, {"id", "so"}};
    out.update(more);
    return out;
}


}  // namespace

class TestSmartFilters : public QObject {
    Q_OBJECT

private slots:
    void setAgainFromTheContents_data() {
        QTest::addColumn<QString>("mode");
        QTest::newRow("8 bits") << QString();
        QTest::newRow("16 bits") << QStringLiteral(R"({"op": "set_color_mode", "page": 1, "mode": "rgb", "depth": 16})");
    }
    void setAgainFromTheContents() {
        QFETCH(QString, mode);
        const Document original = book(mode.isEmpty() ? Json() : core::parse_python_json(mode.toStdString()));
        const Document r2 = run(original, Json::array({op("smart_filter_add", Json{{"kind", "blur"}, {"radius", 2}})}));
        QCOMPARE(filters_of(r2).size(), std::size_t(1));
        QCOMPARE(filters_of(r2)[0], (Json{{"kind", "blur"}, {"params", Json{{"radius", 2}}}, {"blend", "normal"}, {"opacity", 1.0}, {"visible", true}}));
        QVERIFY(core::smart::contents_of(r2, so_of(r2)) == core::smart::contents_of(original, so_of(original)));  // (the contents untouched)
        QVERIFY(most_apart(drawn(r2).image, drawn(original).image) > 12);
        // (another radius: from the contents, not from the blurred pixels; the first again: the same pixels)
        const Document r6 = run(r2, Json::array({op("smart_filter_set", Json{{"index", 0}, {"params", Json{{"radius", 6}}}})}));
        QVERIFY(most_apart(drawn(r6).image, drawn(r2).image) > 3);
        const Document back = run(r6, Json::array({op("smart_filter_set", Json{{"params", Json{{"radius", 2}}}})}));
        QCOMPARE(pixels(back), pixels(r2));
        // (hidden — shown or hidden the other way when nothing else is asked —: the picture as it was)
        const Document hidden = run(back, Json::array({op("smart_filter_set", Json{{"index", 0}})}));
        QCOMPARE(filters_of(hidden)[0]["visible"], Json(false));
        QCOMPARE(pixels(hidden), pixels(original));
        const Document shown = run(hidden, Json::array({op("smart_filter_set", Json{{"visible", true}})}));
        QCOMPARE(pixels(shown), pixels(r2));
    }

    void orderBlendOffAndClear() {
        const Document original = book();
        const Document ab = run(original, Json::array({op("smart_filter_add", Json{{"kind", "mosaic"}, {"block", 10}}),
                                                       op("smart_filter_add", Json{{"kind", "blur"}, {"radius", 3}})}));
        QCOMPARE(filters_of(ab).size(), std::size_t(2));
        const Document ba = run(ab, Json::array({op("smart_filter_move", Json{{"index", 1}, {"to", 0}})}));
        QCOMPARE(filters_of(ba)[0]["kind"], Json("blur"));
        QVERIFY2(pixels(ba) != pixels(ab), "the order matters");
        QCOMPARE(pixels(run(ba, Json::array({op("smart_filter_move", Json{{"index", 0}, {"to", 1}})}))), pixels(ab));
        // (both at 0 % — 0..1, or past 1 a percentage —: the picture as it was; a blend kept)
        const Document none = run(ab, Json::array({op("smart_filter_set", Json{{"index", 0}, {"opacity", 0}}),
                                                   op("smart_filter_set", Json{{"index", 1}, {"opacity", 0}, {"blend", "multiply"}})}));
        QCOMPARE(filters_of(none)[1]["blend"], Json("multiply"));
        QVERIFY(most_apart(drawn(none).image, drawn(original).image) <= 1);
        const Document half = run(ab, Json::array({op("smart_filter_set", Json{{"opacity", 50}})}));
        QCOMPARE(filters_of(half)[1]["opacity"], Json(0.5));
        QVERIFY(pixels(half) != pixels(ab) && pixels(half) != pixels(original));
        const Document multiplied = run(ab, Json::array({op("smart_filter_set", Json{{"blend", "multiply"}})}));
        QVERIFY(pixels(multiplied) != pixels(ab));
        QVERIFY(refused(ab, op("smart_filter_set", Json{{"blend", "nope"}})).find("unknown blend mode") != std::string::npos);
        // (all of them off: the picture as it was; on again: as before)
        const Document off = run(ab, Json::array({op("smart_filter_state", Json{{"filters_on", false}})}));
        QCOMPARE((*so_of(off).smart)["filters_on"], Json(false));
        QCOMPARE(pixels(off), pixels(original));
        const Document on = run(off, Json::array({op("smart_filter_state", Json{{"filters_on", true}})}));
        QVERIFY(!so_of(on).smart->contains("filters_on"));
        QCOMPARE(pixels(on), pixels(ab));
        // (one deleted; a place it does not have refused; cleared: the smart object as it was before them)
        const Document one = run(ab, Json::array({op("smart_filter_delete", Json{{"index", 0}})}));
        QCOMPARE(filters_of(one).size(), std::size_t(1));
        QCOMPARE(filters_of(one)[0]["kind"], Json("blur"));
        QVERIFY(refused(ab, op("smart_filter_delete", Json{{"index", 5}})).find("is the filter's place") != std::string::npos);
        const Document cleared = run(ab, Json::array({op("smart_filter_delete", Json{{"all", true}})}));
        QCOMPARE(*so_of(cleared).smart, *so_of(original).smart);
        QCOMPARE(pixels(cleared), pixels(original));
        QVERIFY(refused(cleared, op("smart_filter_delete")).find("has no smart filters") != std::string::npos);
        QVERIFY(refused(cleared, op("smart_filter_state", Json{{"filters_on", false}})).find("has no smart filters") != std::string::npos);
    }

    void theSelectionIsTheFilterMask() {
        const Document original = book();
        const double w = original.page(0).spec.width_mm.value(), h = original.page(0).spec.height_mm.value();
        // (the left half of the page selected)
        const Json area{{"poly", Json::array({Json::array({0, 0}), Json::array({w / 2, 0}), Json::array({w / 2, h}), Json::array({0, h})})}};
        const Document masked = run(original, Json::array({op("smart_filter_add", Json{{"kind", "invert"}, {"area", area}})}));
        const Json& mask = (*so_of(masked).smart)["filter_mask"];
        QVERIFY(mask.is_object() && mask["asset"].get<std::string>().starts_with("sha256:") && mask["enabled"] == Json(true));
        QVERIFY(masked.smart_files.contains(mask["asset"].get<std::string>()));
        // (inside the mask: filtered; outside: as it was)
        const render::Image before = drawn(original).image, after = drawn(masked).image;
        const int mid = before.width() / 2;
        QVERIFY(most_apart(after.crop(render::Box{mid + 4, 0, before.width(), before.height()}), before.crop(render::Box{mid + 4, 0, before.width(), before.height()})) == 0);
        QVERIFY(most_apart(after.crop(render::Box{0, 0, mid - 4, before.height()}), before.crop(render::Box{0, 0, mid - 4, before.height()})) > 100);
        // (a second filter keeps the mask it has)
        const Json corner{{"poly", Json::array({Json::array({0, 0}), Json::array({5, 0}), Json::array({5, 5})})}};
        Json second = op("smart_filter_add", Json{{"kind", "blur"}});
        second["area"] = corner;
        const Document two = run(masked, Json::array({second}));
        QCOMPARE((*so_of(two).smart)["filter_mask"], mask);
        // (the mask off: everywhere; deleted: none)
        const Document unmasked = run(masked, Json::array({op("smart_filter_state", Json{{"mask_on", false}})}));
        QCOMPARE((*so_of(unmasked).smart)["filter_mask"]["enabled"], Json(false));
        QVERIFY(most_apart(drawn(unmasked).image.crop(render::Box{mid + 4, 0, before.width(), before.height()}),
                           before.crop(render::Box{mid + 4, 0, before.width(), before.height()})) > 100);
        const Document dropped = run(masked, Json::array({op("smart_filter_state", Json{{"mask", "delete"}})}));
        QVERIFY(!so_of(dropped).smart->contains("filter_mask"));
        QCOMPARE(pixels(dropped), pixels(unmasked));
        QVERIFY(refused(dropped, op("smart_filter_state", Json{{"mask_on", true}})).find("has no filter mask") != std::string::npos);
        // (made again of a selection: the right half now)
        const Json right{{"poly", Json::array({Json::array({w / 2, 0}), Json::array({w, 0}), Json::array({w, h}), Json::array({w / 2, h})})}};
        const Document again = run(dropped, Json::array({op("smart_filter_state", Json{{"mask", "from_area"}, {"area", right}})}));
        QVERIFY(most_apart(drawn(again).image.crop(render::Box{0, 0, mid - 4, before.height()}), before.crop(render::Box{0, 0, mid - 4, before.height()})) == 0);
    }

    void theFiltersFollowTheTransforms() {
        const Document blurred = run(book(), Json::array({op("smart_filter_add", Json{{"kind", "blur"}, {"radius", 2}})}));
        const Document twice = run(blurred, Json::array({op("smart_transform", Json{{"matrix", Json::array({2, 0, 0, 2, 0, 0})}})}));
        QCOMPARE(filters_of(twice).size(), std::size_t(1));
        QVERIFY(pixels(twice) != pixels(blurred));
        const Document back = run(twice, Json::array({op("smart_transform", Json{{"matrix", Json::array({0.5, 0, 0, 0.5, 0, 0})}})}));
        QCOMPARE(pixels(back), pixels(blurred));
    }

    // At the working 200 dpi a smart filter draws as rasterize_layer and filter_raster leave the layer: a correction of
    // each pixel within a level, a blur within the 8-bit filter's own rounding of its passes (test_color_filters' measure).
    void likeRasterizeAndFilterRaster_data() {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<QString>("kind");
        QTest::addColumn<QString>("params");
        QTest::addColumn<int>("tolerance");
        const QString eight, sixteen = QStringLiteral(R"({"op": "set_color_mode", "page": 1, "mode": "rgb", "depth": 16})");
        const QString cmyk = QStringLiteral(R"({"op": "set_color_mode", "page": 1, "mode": "cmyk", "profile": "coated-cmyk"})");
        const QString lab = QStringLiteral(R"({"op": "set_color_mode", "page": 1, "mode": "lab", "depth": 16})");
        QTest::newRow("8 bits invert") << eight << "invert" << "{}" << 1;
        QTest::newRow("8 bits levels") << eight << "levels" << R"({"black": 20, "white": 220})" << 1;
        QTest::newRow("8 bits mosaic") << eight << "mosaic" << R"({"block": 9})" << 1;
        QTest::newRow("8 bits blur") << eight << "blur" << R"({"radius": 3})" << 2;
        QTest::newRow("8 bits hue_saturation") << eight << "hue_saturation" << R"({"hue": 40, "saturation": 20})" << 1;
        QTest::newRow("16 bits blur") << sixteen << "blur" << R"({"radius": 3})" << 1;
        QTest::newRow("16 bits levels") << sixteen << "levels" << R"({"black": 20, "white": 220})" << 1;
        QTest::newRow("CMYK blur") << cmyk << "blur" << R"({"radius": 3})" << 1;
        QTest::newRow("CMYK invert") << cmyk << "invert" << "{}" << 1;
        QTest::newRow("Lab blur") << lab << "blur" << R"({"radius": 3})" << 1;
    }
    void likeRasterizeAndFilterRaster() {
        QFETCH(QString, mode);
        QFETCH(QString, kind);
        QFETCH(QString, params);
        QFETCH(int, tolerance);
        Document doc = book(mode.isEmpty() ? Json() : core::parse_python_json(mode.toStdString()));
        // (a page of 16 bits composited in precise colour either way: a clear precise layer of its own on it — the
        // rasterized layer is precise, and a page with one composites so)
        if (doc.page(0).rgb && doc.page(0).rgb->depth > 8)
            doc = run(doc, Json::array({Json{{"op", "put_color_raster"}, {"page", 1}, {"width", 1}, {"height", 1}, {"precision", "u16"},
                                              {"pixels", Json::array({0, 0, 0, 0})}}}));
        Json add = op("smart_filter_add", Json{{"kind", kind.toStdString()}});
        add.update(core::parse_python_json(params.toStdString()));
        const Document smart = run(doc, Json::array({add}));
        Json filter{{"op", "filter_raster"}, {"page", 1}, {"id", "so"}, {"kind", kind.toStdString()}};
        filter.update(core::parse_python_json(params.toStdString()));
        const Document pixels_then = run(doc, Json::array({op("rasterize_layer"), filter}));
        QCOMPARE(so_of(pixels_then).kind, core::LayerKind::Raster);
        const render::RenderResult a = drawn(smart, 200), b = drawn(pixels_then, 200);
        QVERIFY(most_apart(a.image, drawn(doc, 200).image) > 2);  // (it does filter)
        const int apart = most_apart(a.image, b.image);
        QVERIFY2(apart <= tolerance, qPrintable(QStringLiteral("%1 levels apart").arg(apart)));
    }

    void aPartIsTheWholeCutThere() {
        const double w = 60, h = 80;
        const Json area{{"poly", Json::array({Json::array({10, 10}), Json::array({w, 15}), Json::array({w / 2, h})})}};
        for (const Json& mode : {Json(), Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}},
                                 Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}}}) {
            const Document doc = run(book(mode), Json::array({op("smart_filter_add", Json{{"kind", "twirl"}, {"angle", 60}, {"area", area}}),
                                                              op("smart_filter_add", Json{{"kind", "blur"}, {"radius", 4}}),
                                                              op("smart_filter_set", Json{{"blend", "screen"}, {"opacity", 0.7}}),
                                                              op("smart_transform", Json{{"matrix", Json::array({0.8, 0.1, -0.1, 0.8, 9.5, 4.25})}})}));
            render::clear_render_caches();
            const render::Image whole = drawn(doc, 60).image;
            for (const render::RenderRegion part : {render::RenderRegion{0, 0, 64, 64}, render::RenderRegion{50, 70, 90, 60}, render::RenderRegion{200, 300, 77, 55}}) {
                render::clear_render_caches();
                const render::Image piece = drawn(doc, 60, part).image;
                QVERIFY2(piece.tobytes() == whole.crop(render::Box{part.x, part.y, part.x + part.w, part.y + part.h}).tobytes(), mode.dump().c_str());
            }
        }
    }

    void whatCannotBeASmartFilterIsRefused() {
        const Document doc = book();
        const auto says = [&](const Json& more, const char* words) {
            const std::string why = refused(doc, op("smart_filter_add", more));
            if (why.empty() || why.find(words) == std::string::npos) qWarning("%s: %s", more.dump().c_str(), why.c_str());
            return !why.empty() && why.find(words) != std::string::npos;
        };
        QVERIFY(says(Json{{"kind", "plugin:sepia"}}, "a filter plugin cannot be a smart filter"));
        QVERIFY(says(Json{{"kind", "match_color"}, {"source_stats", Json::object()}}, "cannot be a smart filter"));
        QVERIFY(says(Json{{"kind", "nope"}}, "cannot be a smart filter"));
        QVERIFY(says(Json{{"kind", "color_lookup"}, {"data", "TITLE \"x\"\nLUT_3D_SIZE 2\n"}}, "one of the looks"));
        QVERIFY(says(Json{{"kind", "blur"}, {"radius", "wide"}}, ""));  // (as filter_raster refuses it)
        QVERIFY(says(Json{{"kind", "hue_saturation"}, {"hue", "red"}}, "hue"));
        QVERIFY(says(Json::object(), "kind names the filter"));
        // (on a layer that is not a smart object, or a smart object locked)
        QVERIFY(refused(doc, Json{{"op", "smart_filter_add"}, {"page", 1}, {"id", "nope"}, {"kind", "blur"}}).find("no layer") != std::string::npos);
        const Document locked = run(doc, Json::array({Json{{"op", "set_layer"}, {"page", 1}, {"id", "so"}, {"locked", true}}}));
        QVERIFY(refused(locked, op("smart_filter_add", Json{{"kind", "blur"}})).find("locked") != std::string::npos);
        // (levels' own r, g, b are its settings, not a channel of the page)
        const Document own = run(doc, Json::array({op("smart_filter_add", Json{{"kind", "brightness_contrast"}, {"brightness", 20}, {"channel", "g"}})}));
        QCOMPARE(filters_of(own)[0]["params"]["channel"], Json("g"));
        // (at most 64)
        Json many = Json::array();
        for (std::size_t i = 0; i < core::smart::kFiltersMax; ++i) many.push_back(op("smart_filter_add", Json{{"kind", "invert"}}));
        const Document full = run(doc, many);
        QVERIFY(refused(full, op("smart_filter_add", Json{{"kind", "invert"}})).find("at most 64") != std::string::npos);
    }

    void writtenAndReadBackWithTheirMask() {
        QTemporaryDir tmp;
        const Json area{{"poly", Json::array({Json::array({0, 0}), Json::array({70, 0}), Json::array({70, 90}), Json::array({0, 90})})}};
        const Document doc = run(book(), Json::array({op("smart_filter_add", Json{{"kind", "blur"}, {"radius", 3}, {"area", area}}),
                                                      op("smart_filter_add", Json{{"kind", "levels"}, {"black", 30}}),
                                                      op("smart_filter_set", Json{{"blend", "overlay"}, {"opacity", 0.4}})}));
        const fs::path dir = genko::core::path_from_utf8((tmp.path() + "/book").toStdString());
        genko::test::write_project(doc, dir);
        const Json payload = genko::test::read_json(tmp.path() + "/book/project.json");
        Json so;
        for (const Json& l : payload["pages"][0]["layers"])
            if (l["id"] == Json("so")) so = l;
        QCOMPARE(so["smart"]["filters"], filters_of(doc));
        const std::string ref = so["smart"]["filter_mask"]["asset"].get<std::string>();
        QVERIFY(storage::AssetStore(dir).has(ref, ".png"));
        const auto loaded = storage::load_document(dir);
        QVERIFY2(loaded.report.clean(), loaded.report.to_json().dump().c_str());
        QCOMPARE(loaded.document.read_only_reason, std::string());
        QCOMPARE(*so_of(loaded.document).smart, *so_of(doc).smart);
        QCOMPARE(pixels(loaded.document), pixels(doc));
        // (written again: the same pages and contents)
        genko::test::write_project(loaded.document, genko::core::path_from_utf8((tmp.path() + "/again").toStdString()));
        const Json again = genko::test::read_json(tmp.path() + "/again/project.json");
        QCOMPARE(again["pages"].dump(), payload["pages"].dump());
        for (const auto& [id, contents] : payload["smart_contents"].items()) QCOMPARE(again["smart_contents"][id]["page"].dump(), contents["page"].dump());
        // (the mask's file missing: said, read-only)
        fs::remove(storage::AssetStore(dir).path(ref, ".png"));
        const auto missing = storage::load_document(dir);
        QVERIFY(!missing.document.read_only_reason.empty());
        QVERIFY2(missing.report.to_json().dump().find("filter mask") != std::string::npos, missing.report.to_json().dump().c_str());
    }

    void aReaderRefusesWhatCannotBe() {
        const Document doc = run(book(), Json::array({op("smart_filter_add", Json{{"kind", "blur"}, {"radius", 3}})}));
        core::smart::validate_smart(doc);
        const auto with = [&](const std::function<void(Json&)>& change) {
            Document bad = doc;
            for (core::Layer& l : bad.edit_page(0).layers)
                if (l.id == "so") change(*l.smart);
            try {
                core::smart::validate_smart(bad);
            } catch (const core::Error& error) {
                return std::string(error.what());
            }
            return std::string();
        };
        QVERIFY(with([](Json& s) { s["filters"][0]["strength"] = 1; }).find("is not known to this build") != std::string::npos);
        QVERIFY(with([](Json& s) { s["filters"][0]["kind"] = "match_color"; }).find("cannot be a smart filter") != std::string::npos);
        QVERIFY(with([](Json& s) { s["filters"][0]["kind"] = "plugin:sepia"; }).find("cannot be a smart filter") != std::string::npos);
        QVERIFY(with([](Json& s) { s["filters"][0]["blend"] = "nope"; }).find("is not a blend mode") != std::string::npos);
        QVERIFY(with([](Json& s) { s["filters"][0]["opacity"] = 2; }).find("opacity is 0 to 1") != std::string::npos);
        QVERIFY(with([](Json& s) { s["filters"][0]["visible"] = "yes"; }).find("true or false") != std::string::npos);
        QVERIFY(with([](Json& s) { s["filters"][0] = Json{{"kind", "hue_saturation"}, {"params", Json{{"hue", "red"}}}}; }).find("cannot be read") !=
                std::string::npos);
        QVERIFY(with([](Json& s) { s["filters"][0] = Json{{"kind", "levels"}, {"params", Json{{"black", "x"}}}}; }).find("cannot be read") != std::string::npos);
        QVERIFY(with([](Json& s) { s["filter_mask"] = Json{{"asset", "a.png"}}; }).find("filter mask is an asset") != std::string::npos);
        QVERIFY(with([](Json& s) { s["filter_mask"] = Json{{"asset", "sha256:" + std::string(64, 'a')}, {"shown", true}}; }).find("is not known") !=
                std::string::npos);
        QVERIFY(with([](Json& s) {
                    for (std::size_t i = 0; i < core::smart::kFiltersMax; ++i) s["filters"].push_back(s["filters"][0]);
                }).find("at most 64") != std::string::npos);
        QVERIFY(with([](Json&) {}).empty());
    }
};

QTEST_MAIN(TestSmartFilters)
#include "test_smart_filters.moc"
