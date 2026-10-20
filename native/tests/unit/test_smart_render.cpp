// smart-objects@1 (PC3 A0, section A 4): a smart object drawn on its page — one layer converted draws as it did, the same
// pixels, on a page of 8, 16 and 32 bits, CMYK, Lab and grey (PhotoCraft's tests.rs:61-93: converting changes nothing);
// a placed picture converted, and the same 8-bit PNG and JPEG placed by place_asset and by smart_place in the same panel,
// draw the same pixels on the screen and in print (the monochrome finish); made smaller and back the same as before
// (tests.rs:220-251: never resampled twice); a part of the page the same pixels as the whole cut there, moved, scaled and
// turned; a 16-bit file drawn on a 16-bit page with its 16 bits; layers converted together drawn (not the same pixels:
// they are composited alone, as Photoshop's).

#include <QtTest>

#include <QTemporaryDir>

#include <atomic>
#include <cmath>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "core/base64.hpp"
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/ink.hpp"
#include "core/model.hpp"
#include "core/paths.hpp"
#include "core/smart.hpp"
#include "render/image.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/smart.hpp"
#include "storage/asset_store.hpp"
#include "testsupport.hpp"

namespace core = genko::core;
namespace render = genko::render;
namespace storage = genko::storage;
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

core::PageSpec small_page() {
    core::PageSpec spec = core::PageSpec::a5_doujin();
    return spec;
}

// A picture with colours, greys and an alpha that fades.
std::string picture_png(int w, int h, int seed) {
    std::string bytes(std::size_t(w) * h * 4, '\0');
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            unsigned char* p = reinterpret_cast<unsigned char*>(bytes.data()) + (std::size_t(y) * w + x) * 4;
            p[0] = static_cast<unsigned char>((x * 7 + seed) % 256);
            p[1] = static_cast<unsigned char>((y * 5 + seed * 3) % 256);
            p[2] = static_cast<unsigned char>((x * y + seed) % 256);
            p[3] = static_cast<unsigned char>(x < w / 4 ? 0 : std::min(255, 40 + (x + y) * 2));
        }
    }
    return render::write_png(render::Image::frombytes("RGBA", render::Size{w, h}, bytes));
}

// A one-page book with a paint layer "p1" (a picture, a mask, multiply at 70%) under a pen layer "p2" with lines, in
// the colour mode the op gives (none: 8 bits, sRGB).
Document book(const Json& mode = Json()) {
    Document doc = core::new_episode("描く", Num(1), 1, small_page());
    doc = run(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "p1"}, {"name", "絵"}},
                                  Json{{"op", "put_raster"}, {"page", 1}, {"id", "p1"}, {"png_base64", core::b64encode(picture_png(96, 140, 3))}},
                                  Json{{"op", "set_layer"}, {"page", 1}, {"id", "p1"}, {"opacity", 0.7}, {"blend", "multiply"}},
                                  Json{{"op", "set_layer_mask"}, {"page", 1}, {"id", "p1"},
                                       {"area", Json{{"poly", Json::array({Json::array({10, 10}), Json::array({120, 20}), Json::array({60, 180})})}}}},
                                  Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "p2"}, {"name", "線"}},
                                  Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", "p2"},
                                       {"points", Json::array({Json::array({20, 30}), Json::array({80, 60}), Json::array({110, 150})})},
                                       {"width_mm", 1.2}}}));
    if (!mode.is_null()) doc = run(doc, Json::array({mode}));
    return doc;
}

render::RenderResult drawn(const Document& doc, const std::string& mode, int dpi = 100, std::optional<render::RenderRegion> region = std::nullopt) {
    render::RenderOptions options;
    options.mode = mode;
    options.inks = doc.page(0).color.has_value();
    options.precise = !doc.page(0).color && doc.page(0).rgb && doc.page(0).rgb->depth > 8;
    options.region = region;
    return render::render_page(doc.page(0), dpi, options, &doc);
}

bool same(const render::RenderResult& a, const render::RenderResult& b, const char* what) {
    if (a.image.tobytes() != b.image.tobytes()) {
        qWarning("%s: the pictures differ", what);
        return false;
    }
    if (a.inks.has_value() != b.inks.has_value() || (a.inks && (a.inks->samples != b.inks->samples || a.inks->floats != b.inks->floats))) {
        qWarning("%s: the inks differ", what);
        return false;
    }
    if (a.precise != b.precise) {
        qWarning("%s: the values differ", what);
        return false;
    }
    return true;
}

std::filesystem::path path_of(const QString& dir) { return core::path_from_utf8(dir.toStdString()); }

const core::Layer* smart_layer(const core::Page& page) {
    for (const core::Layer& l : page.layers)
        if (l.kind == core::LayerKind::Smart) return &l;
    return nullptr;
}

}  // namespace

class TestSmartRender : public QObject {
    Q_OBJECT

private slots:
    void oneLayerConvertedDrawsAsItDid_data() {
        QTest::addColumn<QString>("mode");
        QTest::newRow("8 bits") << QString();
        QTest::newRow("16 bits") << QStringLiteral(R"({"op": "set_color_mode", "page": 1, "mode": "rgb", "depth": 16})");
        QTest::newRow("32 bits") << QStringLiteral(R"({"op": "set_color_mode", "page": 1, "mode": "rgb", "depth": 32})");
        QTest::newRow("CMYK") << QStringLiteral(R"({"op": "set_color_mode", "page": 1, "mode": "cmyk", "profile": "coated-cmyk"})");
        QTest::newRow("Lab") << QStringLiteral(R"({"op": "set_color_mode", "page": 1, "mode": "lab", "depth": 16})");
        QTest::newRow("grey") << QStringLiteral(R"({"op": "set_color_mode", "page": 1, "mode": "gray"})");
    }
    void oneLayerConvertedDrawsAsItDid() {
        QFETCH(QString, mode);
        const Document before = book(mode.isEmpty() ? Json() : core::parse_python_json(mode.toStdString()));
        const Document after = run(before, Json::array({Json{{"op", "smart_convert"}, {"page", 1}, {"id", "p1"}, {"new_id", "so"}}}));
        QVERIFY(smart_layer(after.page(0)) != nullptr);
        for (const char* how : {"proof", "print"}) QVERIFY2(same(drawn(before, how), drawn(after, how), how), how);
        // (the pen layer too, and both at once)
        const Document pen = run(before, Json::array({Json{{"op", "smart_convert"}, {"page", 1}, {"id", "p2"}, {"new_id", "so2"}}}));
        QVERIFY(same(drawn(before, "proof"), drawn(pen, "proof"), "the pen layer"));
        // (a part of it: the same)
        const render::RenderRegion part{17, 23, 101, 77};
        QVERIFY(same(drawn(before, "proof", 100, part), drawn(after, "proof", 100, part), "a part"));
    }

    void aPlacedPictureConvertedDrawsAsItDid() {
        QTemporaryDir tmp;
        Document doc = core::new_episode("配置", Num(1), 1, small_page());
        doc.asset_dir = path_of(tmp.path());
        doc.edit_page(0).name_ok = true;
        const std::string ref = storage::AssetStore(*doc.asset_dir).put_bytes(picture_png(64, 48, 9), ".png");
        doc = run(doc, Json::array({Json{{"op", "place_asset"}, {"page", 1}, {"asset", ref}, {"frame_id", doc.page(0).frames[0].id}}}));
        std::string placed;
        for (const core::Layer& l : doc.page(0).layers)
            if (l.kind == core::LayerKind::Placed) placed = l.id;
        const Document after = run(doc, Json::array({Json{{"op", "smart_convert"}, {"page", 1}, {"id", placed}}}));
        const core::Layer* so = smart_layer(after.page(0));
        QVERIFY(so != nullptr && so->frame_id && so->clip_to == "frame");
        QCOMPARE(core::smart::contents_of(after, *so)->kind, std::string("file"));
        for (const char* how : {"proof", "print", "name"}) QVERIFY2(same(drawn(doc, how), drawn(after, how), how), how);
        const render::RenderRegion part{30, 40, 120, 90};
        QVERIFY(same(drawn(doc, "print", 100, part), drawn(after, "print", 100, part), "a part in print"));
    }

    void aFilePlacedAsPlaceAssetPlacesItDrawsTheSame() {
        QTemporaryDir tmp;
        const render::Image picture = render::open_image(picture_png(80, 60, 1), render::kPillowOpenLimits);
        const std::string png = render::write_png(picture.convert("RGB"));
        const std::string jpeg = render::write_jpeg(picture, 90);
        for (const auto& [file, suffix] : {std::pair{png, std::string(".png")}, std::pair{jpeg, std::string(".jpg")}}) {
            Document doc = core::new_episode("同じ", Num(1), 1, small_page());
            doc.asset_dir = path_of(tmp.path());
            doc.edit_page(0).name_ok = true;
            const std::string panel = doc.page(0).frames[0].id;
            // (place_asset: the PNG of the picture as it is read, as 画像を読み込む… made it; smart_place: the file itself)
            const std::string as_png = render::write_png(render::open_image(file, render::kPillowOpenLimits).convert("RGB"));
            const std::string placed_ref = storage::AssetStore(*doc.asset_dir).put_bytes(as_png, ".png");
            const std::string file_ref = storage::AssetStore(*doc.asset_dir).put_bytes(file, suffix);
            const Document placed = run(doc, Json::array({Json{{"op", "place_asset"}, {"page", 1}, {"asset", placed_ref}, {"frame_id", panel}}}));
            const Document smart = run(doc, Json::array({Json{{"op", "smart_place"}, {"page", 1}, {"asset", file_ref}, {"name", "a" + suffix}, {"frame_id", panel}}}));
            for (const char* how : {"proof", "print"}) QVERIFY2(same(drawn(placed, how), drawn(smart, how), how), qPrintable(QString::fromStdString(suffix) + how));
            // (over the page out to the bleed, as 画像を読み込む… places one with no panel chosen)
            const core::Rect b = doc.page(0).bleed_rect_mm();
            const Json box = Json::array({b.x.json(), b.y.json(), b.width.json(), b.height.json()});
            const Document placed_page = run(doc, Json::array({Json{{"op", "place_asset"}, {"page", 1}, {"asset", placed_ref}, {"placement_mm", box}}}));
            const Document smart_page = run(doc, Json::array({Json{{"op", "smart_place"}, {"page", 1}, {"asset", file_ref}, {"name", "a" + suffix}, {"placement_mm", box}}}));
            QVERIFY(same(drawn(placed_page, "print"), drawn(smart_page, "print"), "over the page"));
        }
    }

    void smallerAndBackIsAsBefore() {
        const Document before = run(book(), Json::array({Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({"p1", "p2"})}, {"new_id", "so"}}}));
        const Document smaller = run(before, Json::array({Json{{"op", "smart_transform"}, {"page", 1}, {"id", "so"}, {"matrix", Json::array({0.3, 0, 0, 0.3, 5, 5})}}}));
        QVERIFY(!same(drawn(before, "proof"), drawn(smaller, "proof"), "(smaller: not the same, as expected)"));
        const Document back = run(smaller, Json::array({Json{{"op", "smart_transform"}, {"page", 1}, {"id", "so"},
                                                               {"matrix", Json::array({1 / 0.3, 0, 0, 1 / 0.3, -5 / 0.3, -5 / 0.3})}}}));
        QVERIFY(core::smart::is_identity(core::smart::matrix_of(*smart_layer(back.page(0)))));
        QVERIFY(same(drawn(before, "proof"), drawn(back, "proof"), "smaller and back"));
    }

    void aPartIsTheWholeCutThere() {
        const Document base = run(book(), Json::array({Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({"p1", "p2"})}, {"new_id", "so"}}}));
        const double turn = 15 * 3.14159265358979323846 / 180;
        for (const Json& m : {Json::array({0.7, 0, 0, 0.6, 12.5, 7.25}), Json::array({std::cos(turn), std::sin(turn), -std::sin(turn), std::cos(turn), 20, -5}),
                              Json::array({1, 0, 0, 1, 3, 4})}) {
            const Document moved = run(base, Json::array({Json{{"op", "smart_transform"}, {"page", 1}, {"id", "so"}, {"set_matrix", m}}}));
            const render::Image whole = drawn(moved, "proof").image;
            for (const render::RenderRegion part : {render::RenderRegion{0, 0, 64, 64}, render::RenderRegion{50, 70, 90, 60}, render::RenderRegion{200, 300, 77, 55}}) {
                const render::Image piece = drawn(moved, "proof", 100, part).image;
                QVERIFY2(piece.tobytes() == whole.crop(render::Box{part.x, part.y, part.x + part.w, part.y + part.h}).tobytes(), m.dump().c_str());
            }
        }
    }

    void aSixteenBitFileKeepsItsBits() {
        // (a 16-bit grey PNG placed on a 16-bit colour page: its values where 8 bits could not tell them apart)
        core::PageSpec colour = small_page();
        colour.expression = "color";
        Document doc = run(core::new_episode("深い", Num(1), 1, colour), Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}}}));
        render::Image grey = render::Image::create("I;16", render::Size{4, 4}, render::Ink(1000));
        const std::string png = render::write_png(grey);
        const core::Rect b = doc.page(0).bleed_rect_mm();
        doc = run(doc, Json::array({Json{{"op", "smart_place"}, {"page", 1}, {"data", core::b64encode(png)}, {"name", "g.png"},
                                           {"placement_mm", Json::array({b.x.json(), b.y.json(), b.width.json(), b.height.json()})}}}));
        QCOMPARE(core::smart::contents_of(doc, *smart_layer(doc.page(0)))->depth, 16);
        const render::RenderResult out = drawn(doc, "proof", 50);
        QVERIFY(out.precise.has_value());
        const core::ColorRasterView values(*out.precise);
        const auto p = values.pixel(std::size_t(values.width()) * (values.height() / 2) + values.width() / 2);
        QVERIFY2(std::abs(p[0] - 1000 / 65535.0) < 1e-4, qPrintable(QString::number(p[0] * 65535)));
        QVERIFY(std::abs(p[0] * 255 - std::round(p[0] * 255)) > 1e-3);  // (not an 8-bit step)
    }

    // A smart object's layer mask with its density (masks@1) on a page where its picture is drawn in more than 8 bits or in
    // inks: the part it hides shown at half, as on an 8-bit page (not hidden whole).
    void maskDensityOnDeepPages_data() {
        QTest::addColumn<QString>("way");
        for (const char* way : {"eight", "u16", "cmyk"}) QTest::newRow(way) << QString::fromLatin1(way);
    }
    void maskDensityOnDeepPages() {
        QFETCH(QString, way);
        Document doc;
        double x = 60, y = 60;  // (a point the mask hides, where the picture shows)
        if (way == "u16") {
            core::PageSpec colour = small_page();
            colour.expression = "color";
            doc = run(core::new_episode("深い", Num(1), 1, colour), Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}}}));
            const std::string png = render::write_png(render::Image::create("I;16", render::Size{4, 4}, render::Ink(1000)));
            const core::Rect b = doc.page(0).bleed_rect_mm();
            doc = run(doc, Json::array({Json{{"op", "smart_place"}, {"page", 1}, {"data", core::b64encode(png)}, {"name", "g.png"},
                                               {"placement_mm", Json::array({b.x.json(), b.y.json(), b.width.json(), b.height.json()})}}}));
        } else {
            const Json mode = way == "cmyk" ? Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}} : Json();
            doc = run(book(mode), Json::array({Json{{"op", "smart_convert"}, {"page", 1}, {"id", "p1"}, {"new_id", "so"}}}));
        }
        const std::string id = smart_layer(doc.page(0))->id;
        const double w = doc.page(0).spec.width_mm.value(), h = doc.page(0).spec.height_mm.value();
        const Json shown_part{{"poly", Json::array({Json::array({70.0, -10.0}), Json::array({w + 10, -10.0}), Json::array({w + 10, h + 10}), Json::array({70.0, h + 10})})}};
        const Document masked = run(doc, Json::array({Json{{"op", "set_layer_mask"}, {"page", 1}, {"id", id}, {"area", shown_part}}}));
        const Document dense = run(masked, Json::array({Json{{"op", "set_layer_mask"}, {"page", 1}, {"id", id}, {"density", 0.5}}}));
        const auto at = [&](const Document& d) {
            const render::Image image = drawn(d, "proof", 50).image.convert("RGB");
            return image.getpixel(render::mm_to_px(x, 50), render::mm_to_px(y, 50));
        };
        const auto hidden = at(masked), open = at(doc), half = at(dense);
        int apart = 0, from_hidden = 0, from_open = 0;
        for (std::size_t c = 0; c < 3; ++c) {
            apart = std::max(apart, std::abs(static_cast<int>(hidden[c] - open[c])));
            from_hidden = std::max(from_hidden, std::abs(static_cast<int>(half[c] - hidden[c])));
            from_open = std::max(from_open, std::abs(static_cast<int>(half[c] - open[c])));
        }
        QVERIFY2(apart > 30, qPrintable(QString::number(apart)));  // (the mask hides something there)
        QVERIFY2(from_hidden > apart / 4 && from_open > apart / 4, qPrintable(QStringLiteral("%1 %2 of %3").arg(from_hidden).arg(from_open).arg(apart)));
    }

    // (independent review A P1-1) A file of 16 bits on a page of 16 bits is decoded once for all the tiles drawn of it —
    // one picture of it kept (its values in the page's space), not two that put each other out —, and once when the tiles
    // are drawn together (the others wait for the one making it).
    void aDeepFileIsDecodedOnceForItsTiles() {
        core::PageSpec colour = small_page();
        colour.expression = "color";
        Document doc = run(core::new_episode("深い", Num(1), 1, colour), Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}}}));
        const std::string png = render::write_png(render::Image::create("I;16", render::Size{900, 1200}, render::Ink(1000)));
        const core::Rect b = doc.page(0).bleed_rect_mm();
        doc = run(doc, Json::array({Json{{"op", "smart_place"}, {"page", 1}, {"data", core::b64encode(png)}, {"name", "g.png"},
                                           {"placement_mm", Json::array({b.x.json(), b.y.json(), b.width.json(), b.height.json()})}}}));
        const std::vector<render::RenderRegion> tiles{{0, 0, 128, 128}, {128, 0, 128, 128}, {0, 128, 128, 128}, {128, 128, 128, 128}};
        // (kept within 24 MiB: the file is 17 MB as RGBA floats)
        render::smart::set_budget_for_testing(std::size_t(24) << 20);
        render::clear_render_caches();
        auto before = render::smart::counts().files_decoded;
        for (const render::RenderRegion& tile : tiles) QVERIFY(drawn(doc, "proof", 100, tile).precise.has_value());
        QCOMPARE(render::smart::counts().files_decoded - before, std::uint64_t(1));
        render::smart::set_budget_for_testing(0);
        // (drawn together)
        render::clear_render_caches();
        before = render::smart::counts().files_decoded;
        std::vector<std::thread> threads;
        std::atomic<int> precise{0};
        for (const render::RenderRegion& tile : tiles) {
            threads.emplace_back([&doc, &precise, tile] {
                if (drawn(doc, "proof", 100, tile).precise.has_value()) ++precise;
            });
        }
        for (std::thread& t : threads) t.join();
        QCOMPARE(precise.load(), 4);
        QCOMPARE(render::smart::counts().files_decoded - before, std::uint64_t(1));
    }

    // (independent review A P1-1) A page made 16 bits takes a file of more bits only within what such a page keeps whole:
    // past it, refused as placing it is.
    void aPageMadeSixteenBitsTakesLargeFilesOnlyWithinItsLimit() {
        core::PageSpec colour = small_page();
        colour.expression = "color";
        Document doc = core::new_episode("深い", Num(1), 1, colour);
        const int side = static_cast<int>(std::ceil(std::sqrt(double(core::kColorRasterMaxPixels) + 1)));
        QTemporaryDir folder;
        doc.asset_dir = path_of(folder.path());
        const std::string ref =
            storage::AssetStore(*doc.asset_dir).put_bytes(render::write_png(render::Image::create("I;16", render::Size{side, side}, render::Ink(1000))), ".png");
        doc = run(doc, Json::array({Json{{"op", "smart_place"}, {"page", 1}, {"asset", ref}, {"name", "large.png"}}}));
        try {
            (void)run(doc, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}}}));
            QFAIL("a page of 16 bits took the large file");
        } catch (const core::ApplyError& error) {
            QVERIFY2(std::string(error.what()).find("too large to keep its 16 or 32 bits") != std::string::npos, error.what());
        }
        // (8 bits: as it was)
        (void)run(doc, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 8}}}));
    }

    // (independent review A P2-5) Contents in precise colour laid other than by whole pixels — made smaller, turned — keep
    // their bits on a page of 16 bits (drawn in precise colour, then laid through the matrix).
    void preciseContentsLaidSmallerKeepTheirBits() {
        core::PageSpec colour = small_page();
        colour.expression = "color";
        Document doc = run(core::new_episode("深い", Num(1), 1, colour), Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}}}));
        constexpr int kSide = 64;
        Json pixels = Json::array();
        for (int y = 0; y < kSide; ++y) {
            for (int x = 0; x < kSide; ++x) {
                const auto v = static_cast<std::int64_t>(std::lround((0.3 + 0.0013 * x + 0.0007 * y) * 65535));
                for (int c = 0; c < 3; ++c) pixels.push_back(v);
                pixels.push_back(65535);
            }
        }
        doc = run(doc, Json::array({Json{{"op", "put_color_raster"}, {"page", 1}, {"width", kSide}, {"height", kSide}, {"precision", "u16"}, {"pixels", pixels}}}));
        const std::string id = doc.page(0).layers.back().id;
        doc = run(doc, Json::array({Json{{"op", "smart_convert"}, {"page", 1}, {"id", id}, {"new_id", "so"}}}));
        const double turn = 3 * 3.14159265358979323846 / 180;
        for (const Json& m : {Json::array({0.9, 0, 0, 0.9, 4, 6}), Json::array({std::cos(turn), std::sin(turn), -std::sin(turn), std::cos(turn), 5, -3})}) {
            const Document laid = run(doc, Json::array({Json{{"op", "smart_transform"}, {"page", 1}, {"id", "so"}, {"set_matrix", m}}}));
            render::clear_render_caches();
            const render::RenderResult out = drawn(laid, "proof", 50);
            QVERIFY(out.precise.has_value());
            const core::ColorRasterView values(*out.precise);
            int between = 0, seen = 0;
            const std::size_t row = values.height() / 2;
            for (std::size_t x = values.width() / 4; x < values.width() * 3 / 4; ++x) {
                const auto p = values.pixel(row * values.width() + x);
                ++seen;
                if (std::abs(p[0] * 255 - std::round(p[0] * 255)) > 1e-3) ++between;  // (not an 8-bit step)
            }
            QVERIFY2(between * 2 > seen, qPrintable(QStringLiteral("%1: %2 of %3 values between 8-bit steps").arg(QString::fromStdString(m.dump())).arg(between).arg(seen)));
        }
        // (a part of the page the whole cut there)
        const Document smaller = run(doc, Json::array({Json{{"op", "smart_transform"}, {"page", 1}, {"id", "so"}, {"set_matrix", Json::array({0.9, 0, 0, 0.9, 4, 6})}}}));
        const render::RenderResult whole = drawn(smaller, "proof", 50);
        const render::RenderResult part = drawn(smaller, "proof", 50, render::RenderRegion{40, 50, 70, 60});
        QVERIFY(whole.precise && part.precise);
        const core::ColorRasterView a(*whole.precise), c(*part.precise);
        for (std::uint32_t y = 0; y < 60; ++y)
            for (std::uint32_t x = 0; x < 70; ++x) QCOMPARE(c.pixel(std::size_t(y) * 70 + x), a.pixel(std::size_t(y + 50) * a.width() + x + 40));
    }

    void layersConvertedTogetherAreDrawn() {
        const Document before = book();
        const Document after = run(before, Json::array({Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({"p1", "p2"})}, {"new_id", "so"}}}));
        const render::RenderResult out = drawn(after, "proof");
        QCOMPARE(out.image.size(), drawn(before, "proof").image.size());
        QVERIFY(out.image.tobytes() != drawn(core::new_episode("描く", Num(1), 1, small_page()), "proof").image.tobytes());
    }
};

QTEST_MAIN(TestSmartRender)
#include "test_smart_render.moc"
