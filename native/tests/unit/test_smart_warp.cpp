// smart-objects@1, PC3 A5 (SPEC.md COLOR-05「スマートオブジェクトの上の変形」): a smart object bent without its contents
// changing — its warp kept in its contents' mm and drawn from them each time, so a warp undone gives the pixels it had
// (PhotoCraft's warp_cmds.rs:476-500, smart_object_warp_is_lossless) and a new warp takes the place of the one it held; a
// quad its corners; a perspective warp, a puppet warp and Liquify kept as its smart filters (distort_cmds.rs:233-272),
// bending its picture as the op bends it rasterized, set again from their settings; its filter mask moved with it; each
// way of drawing (8 bits, 16 bits, CMYK), a part of the page the whole cut there; its warp written to a PSD and read back;
// what a reader refuses.

#include <QtTest>

#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>

#include "core/base64.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/model.hpp"
#include "core/paths.hpp"
#include "core/smart.hpp"
#include "formats/export.hpp"
#include "render/image.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/psd.hpp"
#include "testsupport.hpp"

namespace core = genko::core;
namespace render = genko::render;
namespace psd = genko::render::psd;
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

Document run(const Document& doc, const Json& ops) { return bus().apply(doc, ops.is_array() ? ops : Json::array({ops}), Actor("human:作者")).doc; }

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

// A picture with colours, sharp edges and an alpha that fades.
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

// A one-page colour book with a paint layer "p1" holding a picture over the page (the colour mode the op gives: none,
// 8 bits sRGB), made a smart object "so".
Document book(const Json& mode = Json()) {
    Document doc = core::new_episode("変形", Num(1), 1, colour_page());
    if (!mode.is_null()) doc = run(doc, Json::array({mode}));
    doc = run(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "p1"}, {"name", "絵"}},
                                Json{{"op", "put_raster"}, {"page", 1}, {"id", "p1"}, {"png_base64", core::b64encode(picture_png(120, 90))}}}));
    return run(doc, Json{{"op", "smart_convert"}, {"page", 1}, {"id", "p1"}, {"new_id", "so"}});
}

// The smart object rasterized: the same picture as a paint layer of the working 200 dpi (named "so"), which the ops bend
// as they bend any paint layer.
Document rasterized() { return run(book(), Json{{"op", "rasterize_layer"}, {"page", 1}, {"id", "so"}}); }

render::RenderResult drawn(const Document& doc, int dpi = 60, std::optional<render::RenderRegion> region = std::nullopt) {
    render::RenderOptions options;
    options.mode = "proof";
    options.inks = doc.page(0).color.has_value();
    options.precise = !doc.page(0).color && doc.page(0).rgb && doc.page(0).rgb->depth > 8;
    options.region = region;
    return render::render_page(doc.page(0), dpi, options, &doc);
}

std::string pixels(const Document& doc, int dpi = 60) {
    render::clear_render_caches();
    return drawn(doc, dpi).image.tobytes();
}

// The most and the mean two pictures of the same size are apart (256: not the same size).
int most_apart(const render::Image& a, const render::Image& b) {
    const std::string x = a.convert("RGB").tobytes(), y = b.convert("RGB").tobytes();
    if (x.size() != y.size()) return 256;
    int most = 0;
    for (std::size_t i = 0; i < x.size(); ++i) most = std::max(most, std::abs(int(static_cast<unsigned char>(x[i])) - int(static_cast<unsigned char>(y[i]))));
    return most;
}
double mean_apart(const render::Image& a, const render::Image& b) {
    const std::string x = a.convert("RGB").tobytes(), y = b.convert("RGB").tobytes();
    if (x.size() != y.size() || x.empty()) return 256;
    double sum = 0;
    for (std::size_t i = 0; i < x.size(); ++i) sum += std::abs(int(static_cast<unsigned char>(x[i])) - int(static_cast<unsigned char>(y[i])));
    return sum / static_cast<double>(x.size());
}

const core::Layer& so_of(const Document& doc) {
    for (const core::Layer& l : doc.page(0).layers)
        if (l.id == "so") return l;
    throw std::runtime_error("no smart object");
}

const Json& smart_of(const Document& doc) { return *so_of(doc).smart; }

Json transform(Json more) {
    Json out{{"op", "transform_layers"}, {"page", 1}, {"ids", Json::array({"so"})}};
    out.update(more);
    return out;
}

Json op(const char* name, Json more = Json::object()) {
    Json out{{"op", name}, {"page", 1}, {"id", "so"}};
    out.update(more);
    return out;
}

const Json kBox = Json::array({30, 20, 100, 170});  // (x, y, w, h mm: the box the warps are over)

}  // namespace

class TestSmartWarp : public QObject {
    Q_OBJECT

private slots:
    // PhotoCraft's smart_object_warp_is_lossless: warped and unwarped, the pixels it had; a second warp takes the
    // first's place (it does not bend the bent picture again); its contents never change.
    void aWarpIsKeptAndUndoneWithoutLoss() {
        const Document original = book();
        const std::string before = pixels(original);
        const auto contents = core::smart::contents_id(so_of(original));
        const Document waved = run(original, transform(Json{{"box", kBox}, {"warp", Json{{"style", "wave"}, {"bend", 60}}}}));
        const Json& warp = smart_of(waved)["warp"];
        QCOMPARE(warp["style"], Json("wave"));
        QCOMPARE(warp["bend"], Json(60.0));
        QCOMPARE(warp["bounds"], Json::array({30.0, 20.0, 130.0, 190.0}));  // (its matrix the identity: the box as it is)
        QVERIFY(core::smart::is_identity(core::smart::matrix_of(so_of(waved))));
        QCOMPARE(core::smart::contents_id(so_of(waved)), contents);
        QVERIFY(waved.smart_contents.at(*contents) == original.smart_contents.at(*contents));  // (the same contents, untouched)
        const std::string bent = pixels(waved);
        QVERIFY(bent != before);
        core::smart::validate_smart(waved);
        // (unwarped: the smart object as it was, the same pixels)
        const Document flat = run(waved, transform(Json{{"box", kBox}, {"warp", Json{{"style", "none"}}}}));
        QVERIFY(!smart_of(flat).contains("warp"));
        QCOMPARE(smart_of(flat), smart_of(original));
        QCOMPARE(pixels(flat), before);
        // (an arc and then a wave: the wave alone)
        const Document twice = run(original, Json::array({transform(Json{{"box", kBox}, {"warp", Json{{"style", "arc"}, {"bend", 20}}}}),
                                                          transform(Json{{"box", kBox}, {"warp", Json{{"style", "wave"}, {"bend", 60}}}})}));
        QCOMPARE(smart_of(twice)["warp"], warp);
        QCOMPARE(pixels(twice), bent);
        // (a custom mesh too: its points kept in the contents' mm; set as it is with smart_transform, and taken away)
        Json mesh = Json{{"style", "custom"}, {"bounds", Json::array({30, 20, 129, 191})}};
        mesh["mesh"] = Json{{"us", Json::array({0, 1})}, {"vs", Json::array({0, 1})}, {"points", Json::array()}};
        for (int j = 0; j < 4; ++j)
            for (int i = 0; i < 4; ++i) mesh["mesh"]["points"].push_back(Json::array({30 + i * 33 + (i == 1 && j == 1 ? 12 : 0), 20 + j * 57}));
        const Document pulled = run(original, transform(Json{{"warp", mesh}}));
        QCOMPARE(smart_of(pulled)["warp"]["style"], Json("custom"));
        QCOMPARE(smart_of(pulled)["warp"]["mesh"]["points"][5], Json::array({75.0, 77.0}));
        const Document set = run(original, op("smart_transform", Json{{"warp", smart_of(pulled)["warp"]}}));
        QCOMPARE(smart_of(set)["warp"], smart_of(pulled)["warp"]);
        QCOMPARE(pixels(set), pixels(pulled));
        QCOMPARE(pixels(run(set, op("smart_transform", Json{{"warp", nullptr}}))), before);
    }

    // The warp is the contents', before the matrix: scaled and turned with it, scaled back the same pixels; a warp given on
    // the page of a scaled smart object kept in its contents' mm.
    void theWarpGoesWithTheMatrix() {
        const Document waved = run(book(), transform(Json{{"box", kBox}, {"warp", Json{{"style", "flag"}, {"bend", 40}}}}));
        const Document half = run(waved, transform(Json{{"matrix", Json::array({0.5, 0, 0, 0.5, 10, 10})}}));
        QCOMPARE(smart_of(half)["warp"], smart_of(waved)["warp"]);
        const core::smart::Matrix m = core::smart::matrix_of(so_of(half));
        QCOMPARE(m.a, 0.5);
        const Document back = run(half, transform(Json{{"matrix", Json::array({2, 0, 0, 2, -20, -20})}}));
        QCOMPARE(pixels(back), pixels(waved));
        // (scaled first, then warped over a box of the page: the warp in the contents' mm, its bounds the box brought back)
        const Document scaled = run(book(), transform(Json{{"matrix", Json::array({0.5, 0, 0, 0.5, 10, 10})}}));
        const Document then = run(scaled, transform(Json{{"box", Json::array({20, 20, 50, 80})}, {"warp", Json{{"style", "arc"}, {"bend", 30}}}}));
        QCOMPARE(smart_of(then)["warp"]["bounds"], Json::array({20.0, 20.0, 120.0, 180.0}));
    }

    // A quad: the smart object's corners (its matrix through three of them), drawn as the quad bends it rasterized (at 200 dpi); set
    // back to a rectangle, its matrix exactly and no corners.
    void aQuadIsItsCorners() {
        const Json quad{{"box", kBox}, {"quad", Json::array({Json::array({40, 30}), Json::array({120, 20}), Json::array({130, 190}), Json::array({30, 170})})}};
        const Document cornered = run(book(), transform(quad));
        const Json& corners = smart_of(cornered)["perspective"];
        QVERIFY(corners.is_array() && corners.size() == 4);
        core::smart::validate_smart(cornered);
        const Document painted = run(rasterized(), transform(quad));
        const render::Image a = drawn(cornered, 200).image, b = drawn(painted, 200).image;
        QVERIFY(most_apart(a, drawn(book(), 100).image) > 50);  // (it does bend)
        const double apart = mean_apart(a, b);
        QVERIFY2(apart < 1.0, qPrintable(QStringLiteral("%1 levels apart on average").arg(apart)));
        // (its corners made a rectangle again: its matrix places it, no corners; the same as moved there)
        // (the paper of A5 is 174 × 236 mm)
        const Document square = run(cornered, op("smart_transform", Json{{"perspective", Json::array({Json::array({5, 6}), Json::array({179, 6}),
                                                                                                        Json::array({179, 242}), Json::array({5, 242})})}}));
        QVERIFY(!smart_of(square).contains("perspective"));
        const core::smart::Matrix m = core::smart::matrix_of(so_of(square));
        QVERIFY(std::abs(m.a - 1) < 1e-9 && std::abs(m.d - 1) < 1e-9 && std::abs(m.e - 5) < 1e-9 && std::abs(m.f - 6) < 1e-9);
        // (corners that cross or squash it refused)
        QVERIFY(refused(book(), op("smart_transform", Json{{"perspective", Json::array({Json::array({0, 0}), Json::array({10, 10}), Json::array({10, 0}),
                                                                                         Json::array({0, 10})})}}))
                    .find("convex") != std::string::npos);
    }

    // A perspective warp and a puppet warp of a smart object: its smart filters (its contents and matrix as they are),
    // bending its picture as the op bends it rasterized (at 200 dpi); set again from their settings, back where
    // they were the picture it had.
    void perspectiveAndPuppetWarpsAreSmartFilters() {
        const Document original = book();
        const Json plane{{"src", Json::array({Json::array({30, 20}), Json::array({130, 20}), Json::array({130, 190}), Json::array({30, 190})})},
                         {"dst", Json::array({Json::array({30, 20}), Json::array({110, 40}), Json::array({125, 180}), Json::array({30, 190})})}};
        const Json perspective = transform(Json{{"perspective", Json{{"planes", Json::array({plane})}}}, {"interp", "bicubic"}});
        const Document warped = run(original, perspective);
        const Json& filters = smart_of(warped)["filters"];
        QCOMPARE(filters.size(), std::size_t(1));
        QCOMPARE(filters[0]["kind"], Json("perspective_warp"));
        QCOMPARE(filters[0]["params"]["planes"][0]["dst"][1], Json::array({110.0, 40.0}));
        QCOMPARE(filters[0]["params"]["interp"], Json("bicubic"));
        QVERIFY(core::smart::is_identity(core::smart::matrix_of(so_of(warped))));
        QVERIFY(!smart_of(warped).contains("perspective") && !smart_of(warped).contains("warp"));
        core::smart::validate_smart(warped);
        const Document painted = run(rasterized(), perspective);
        double apart = mean_apart(drawn(warped, 200).image, drawn(painted, 200).image);
        QVERIFY2(apart < 0.01, qPrintable(QStringLiteral("perspective: %1 levels apart on average").arg(apart)));
        // (set again: its planes left where they were — the picture it had)
        Json still = plane;
        still["dst"] = plane["src"];
        const Document undone = run(warped, op("smart_filter_set", Json{{"params", Json{{"planes", Json::array({still})}}}}));
        QVERIFY(most_apart(drawn(undone).image, drawn(original).image) <= 1);
        // a puppet warp
        const Json puppet = transform(Json{{"puppet", Json{{"pins", Json::array({Json{{"src", Json::array({60, 100})}, {"dst", Json::array({72, 108})}},
                                                                                  Json{{"src", Json::array({100, 160})}, {"dst", Json::array({100, 160})}},
                                                                                  Json{{"src", Json::array({110, 40})}, {"dst", Json::array({110, 40})}}})}}},
                                            {"interp", "bicubic"}});
        const Document pinned = run(original, puppet);
        QCOMPARE(smart_of(pinned)["filters"][0]["kind"], Json("puppet_warp"));
        QCOMPARE(smart_of(pinned)["filters"][0]["params"]["pins"][0]["dst"], Json::array({72.0, 108.0}));
        core::smart::validate_smart(pinned);
        const Document moved = run(rasterized(), puppet);
        apart = mean_apart(drawn(pinned, 200).image, drawn(moved, 200).image);
        QVERIFY2(apart < 0.01, qPrintable(QStringLiteral("puppet: %1 levels apart on average").arg(apart)));
        QVERIFY(most_apart(drawn(pinned).image, drawn(original).image) > 20);
        // (its pins moved again: another bend; back where they were put: the picture it had)
        const Document again = run(pinned, op("smart_filter_set", Json{{"params", Json{{"pins", Json::array({Json{{"src", Json::array({60, 100})},
                                                                                                                   {"dst", Json::array({60, 100})}}})}}}}));
        QVERIFY(most_apart(drawn(again).image, drawn(original).image) <= 1);
        // (a smart object is puppet-warped alone; its settings refused as the op refuses them)
        Document two = run(original, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "other"}});
        Json both = puppet;
        both["ids"] = Json::array({"so", "other"});
        QVERIFY(refused(two, both).find("puppet-warped alone") != std::string::npos);
        QVERIFY(refused(original, op("smart_filter_add", Json{{"kind", "puppet_warp"}, {"mode", "wobbly"}})).find("mode") != std::string::npos);
        QVERIFY(refused(original, op("smart_filter_add", Json{{"kind", "perspective_warp"}, {"planes", Json::array()}})).find("planes") != std::string::npos);
    }

    // Liquify's strokes on a smart object: its smart filter (the selection its filter mask), bending its picture as on a
    // paint layer; its strokes set again.
    void liquifyIsASmartFilter() {
        const Document original = book();
        const Json strokes = Json::array({Json{{"tool", "forward_warp"}, {"size_mm", 20}, {"points", Json::array({Json::array({70, 100}), Json::array({90, 104})})}}});
        const Document liquified = run(original, Json{{"op", "liquify"}, {"page", 1}, {"layer_id", "so"}, {"strokes", strokes}});
        QCOMPARE(smart_of(liquified)["filters"][0]["kind"], Json("liquify"));
        QCOMPARE(smart_of(liquified)["filters"][0]["params"]["strokes"], strokes);
        core::smart::validate_smart(liquified);
        const Document painted = run(rasterized(), Json{{"op", "liquify"}, {"page", 1}, {"layer_id", "so"}, {"strokes", strokes}});
        const double apart = mean_apart(drawn(liquified, 200).image, drawn(painted, 200).image);
        QVERIFY2(apart < 0.01, qPrintable(QStringLiteral("%1 levels apart on average").arg(apart)));
        QVERIFY(most_apart(drawn(liquified).image, drawn(original).image) > 20);
        // (the selection: its filter mask)
        const Json area{{"poly", Json::array({Json::array({0, 0}), Json::array({80, 0}), Json::array({80, 236}), Json::array({0, 236})})}};
        const Document masked = run(original, Json{{"op", "liquify"}, {"page", 1}, {"layer_id", "so"}, {"strokes", strokes}, {"area", area}});
        QVERIFY(smart_of(masked)["filter_mask"].is_object());
        // (its strokes set again: none bending, the picture it had)
        const Document reconstructed = run(liquified, op("smart_filter_set", Json{{"params", Json{{"strokes", Json::array()}}}}));
        QVERIFY(most_apart(drawn(reconstructed).image, drawn(original).image) <= 1);
    }

    // The filter mask goes with the smart object when it moves (transform_layers and smart_transform): the filtered part
    // the same pixels, moved.
    void theFilterMaskMovesWithIt() {
        const double w = 174;  // (the paper of A5)
        const Json left{{"poly", Json::array({Json::array({0, 0}), Json::array({w / 2, 0}), Json::array({w / 2, 236}), Json::array({0, 236})})}};
        const Document masked = run(book(), op("smart_filter_add", Json{{"kind", "invert"}, {"area", left}}));
        const std::string mask = smart_of(masked)["filter_mask"]["asset"].get<std::string>();
        for (const Json& move : {transform(Json{{"move", Json::array({10, 0})}}), op("smart_transform", Json{{"matrix", Json::array({1, 0, 0, 1, 10, 0})}})}) {
            const Document moved = run(masked, move);
            QVERIFY(smart_of(moved)["filter_mask"]["asset"].get<std::string>() != mask);
            core::smart::validate_smart(moved);
            // (at 127 dpi 10 mm is 50 pixels: each pixel of the smart object's picture moved the one 50 to its left, but where
            // the mask's edge is drawn again — its picture alone: the page's panels and nombre stay where they are)
            render::clear_render_caches();
            const render::Image before = render::layer_image(masked.page(0), so_of(masked), 127, &masked, true);
            const render::Image after = render::layer_image(moved.page(0), so_of(moved), 127, &moved, true);
            const int edge = static_cast<int>(std::lround(w / 2 / 25.4 * 127)) + 50;
            int differ = 0;
            for (int y = 0; y < before.height(); y += 3) {
                for (int x = 50; x < before.width(); x += 3) {
                    if (std::abs(x - edge) <= 5) continue;
                    if (before.getpixel(x - 50, y) != after.getpixel(x, y)) ++differ;
                }
            }
            QVERIFY2(differ == 0, qPrintable(QStringLiteral("%1 pixels differ").arg(differ)));
        }
    }

    // Each way of drawing bends it (8 bits, 16 bits, CMYK), and a part of the page is the whole cut there.
    void eachWayAndAPart() {
        const Json quad = Json::array({Json::array({40, 30}), Json::array({120, 20}), Json::array({130, 190}), Json::array({30, 170})});
        for (const Json& mode : {Json(), Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}},
                                 Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}}}) {
            const Document flat = book(mode);
            const Document doc = run(flat, Json::array({transform(Json{{"box", kBox}, {"warp", Json{{"style", "bulge"}, {"bend", 50}}}}),
                                                        transform(Json{{"box", kBox}, {"quad", quad}})}));
            QVERIFY(smart_of(doc).contains("warp") && smart_of(doc).contains("perspective"));
            render::clear_render_caches();
            const render::RenderResult whole = drawn(doc, 60);
            QVERIFY2(whole.omitted.empty(), mode.dump().c_str());
            QVERIFY2(most_apart(whole.image, drawn(flat, 60).image) > 20, mode.dump().c_str());
            for (const render::RenderRegion part : {render::RenderRegion{0, 0, 64, 64}, render::RenderRegion{50, 70, 90, 60}, render::RenderRegion{200, 300, 77, 55}}) {
                render::clear_render_caches();
                const render::Image piece = drawn(doc, 60, part).image;
                QVERIFY2(piece.tobytes() == whole.image.crop(render::Box{part.x, part.y, part.x + part.w, part.y + part.h}).tobytes(), mode.dump().c_str());
            }
        }
    }

    // A picture's smart object warped: its warp written to the PSD's placed layer (in the file's pixels) and read back as
    // it was; a warp split into patches written as one patch fitted to it.
    void itsWarpGoesToAPsdAndBack() {
        QTemporaryDir dir;
        const fs::path out = core::path_from_utf8(dir.filePath(QStringLiteral("w.psd")).toStdString());
        Document doc = core::new_episode("PSD", Num(1), 1, colour_page());
        doc = run(doc, Json{{"op", "smart_place"}, {"page", 1}, {"data", core::b64encode(picture_png(120, 90))}, {"name", "a.png"}, {"new_id", "so"}});
        doc = run(doc, transform(Json{{"warp", Json{{"style", "arcLower"}, {"bend", 35}, {"h_distort", 10}}}}));
        const Json kept = smart_of(doc)["warp"];
        QCOMPARE(kept["style"], Json("arcLower"));
        genko::formats::export_page_psd(doc, doc.page(0), out, 100);
        const std::string bytes = genko::test::read_bytes(dir.filePath(QStringLiteral("w.psd")));
        const psd::File written = psd::File::read(bytes);
        const psd::Layer* placed = nullptr;
        for (const psd::Layer& l : written.layers())
            if (l.kind == "smart") placed = &l;
        QVERIFY(placed != nullptr && placed->placed && placed->placed->warp);
        QCOMPARE((*placed->placed->warp)["style"], Json("arcLower"));
        // (read into another book: its warp in its contents' mm as it was)
        Document again = core::new_episode("PSD", Num(1), 1, colour_page());
        again = run(again, Json{{"op", "import_psd"}, {"page", 1}, {"psd", core::b64encode(bytes)}, {"fit", "paper"}, {"id", "ps"}});
        const core::Layer* read = nullptr;
        for (const core::Layer& l : again.page(0).layers)
            if (l.kind == core::LayerKind::Smart) read = &l;
        QVERIFY(read != nullptr);
        const Json& back = (*read->smart)["warp"];
        QCOMPARE(back["style"], Json("arcLower"));
        QVERIFY(std::abs(back["bend"].get<double>() - 35) < 1e-9 && std::abs(back["h_distort"].get<double>() - 10) < 1e-9);
        for (std::size_t i = 0; i < 4; ++i) QVERIFY(std::abs(back["bounds"][i].get<double>() - kept["bounds"][i].get<double>()) < 1e-6);
        // (a mesh split in two: one patch of 16 points fitted to it, its corners where they were)
        Json split{{"style", "custom"}, {"bounds", Json::array({30, 20, 130, 190})}};
        split["mesh"] = Json{{"us", Json::array({0, 0.5, 1})}, {"vs", Json::array({0, 1})}, {"points", Json::array()}};
        for (int j = 0; j < 4; ++j)
            for (int i = 0; i < 7; ++i) split["mesh"]["points"].push_back(Json::array({30 + i * 100.0 / 6 + (i == 3 ? 8 : 0), 20 + j * 170.0 / 3}));
        doc = run(doc, op("smart_transform", Json{{"warp", split}}));
        genko::formats::export_page_psd(doc, doc.page(0), out, 100);
        const psd::File fitted = psd::File::read(genko::test::read_bytes(dir.filePath(QStringLiteral("w.psd"))));
        for (const psd::Layer& l : fitted.layers()) {
            if (l.kind != "smart") continue;
            QVERIFY(l.placed && l.placed->warp);
            QCOMPARE((*l.placed->warp)["mesh"]["points"].size(), std::size_t(16));
        }
    }

    // What a reader refuses: a warp of a style it does not know, a mesh of the wrong size, a bend past 100; a bending
    // smart filter of keys or lists it does not know.
    void aReaderRefusesWhatCannotBe() {
        const Document original = book();
        const auto refuses = [&](const char* key, const Json& value, const char* words) {
            Document doc = original;
            for (core::Layer& l : doc.edit_page(0).layers)
                if (l.id == "so") (*l.smart)[key] = value;
            try {
                core::smart::validate_smart(doc);
            } catch (const core::Error& error) {
                if (std::string(error.what()).find(words) == std::string::npos) qWarning("%s", error.what());
                return std::string(error.what()).find(words) != std::string::npos;
            }
            return false;
        };
        QVERIFY(refuses("warp", Json{{"style", "spiral"}, {"bounds", Json::array({0, 0, 1, 1})}}, "style"));
        QVERIFY(refuses("warp", Json{{"style", "arc"}, {"bend", 140}, {"bounds", Json::array({0, 0, 1, 1})}}, "bend"));
        QVERIFY(refuses("warp", Json{{"style", "arc"}, {"bounds", Json::array({0, 0, 0, 1})}}, "bounds"));
        QVERIFY(refuses("warp", Json{{"style", "custom"}, {"bounds", Json::array({0, 0, 1, 1})},
                                     {"mesh", Json{{"us", Json::array({0, 1})}, {"vs", Json::array({0, 1})}, {"points", Json::array({Json::array({0, 0})})}}}},
                        "mesh"));
        QVERIFY(refuses("filters", Json::array({Json{{"kind", "puppet_warp"}, {"params", Json{{"pins", Json::array()}, {"wobble", 1}}}}}), "wobble"));
        QVERIFY(refuses("filters", Json::array({Json{{"kind", "perspective_warp"}, {"params", Json{{"planes", Json::array({Json{{"src", Json::array()}}})}}}}}),
                        "four corners"));
        QVERIFY(refuses("filters", Json::array({Json{{"kind", "liquify"}, {"params", Json{{"strokes", "many"}}}}}), "strokes"));
    }
};

QTEST_MAIN(TestSmartWarp)
#include "test_smart_warp.moc"
