// PC4 E1 (brushes@1, SPEC.md COLOR-09「ブラシのダブの描き方」): the line ops with a dab brush, without Python —
// define_brush's dabs (Genko's and upstream's settings, the pictures given as files taken in and kept by the book, what is
// refused), add_stroke's inputs (tilt, time, wheel stretched over the points kept; the flow; the 背景色 of the colour
// dynamics) and its line drawn — the same pixels drawn whole or in parts, again after saving and opening, after Undo and
// Redo —, its blend painted into a paint layer's pixels on 8-bit, 16-bit, 32-bit, CMYK, Lab and grey pages (refused on a pen
// layer; normal keeps a line as before), erase's brush and block, set_brush's kind, 背景色 and reset, 下地混色 on every kind
// of page, brush.get (`genko brushes get`); and every brush that is not a dab brush drawn as before (its pixels the same
// with the new keys of draw given or not).

#include <QElapsedTimer>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "core/base64.hpp"
#include "core/brush_dabs.hpp"
#include "core/brushes.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ops_util.hpp"
#include "core/paths.hpp"
#include "core/strokes.hpp"
#include "render/brushes.hpp"
#include "render/dabs/lines.hpp"
#include "render/image.hpp"
#include "render/image_ops.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/raster.hpp"
#include "render/selection.hpp"
#include "render/warp.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/transaction.hpp"
#include "storage/undo.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
using render::Box;
using render::Size;
using render::image_ops::Pixels;
namespace fs = std::filesystem;
namespace brushes = render::brushes;

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

// A book of one page (2 × 2 inches) with its pen layers, no panels.
core::Document pen_book() {
    core::Document doc = core::new_episode("ダブ", core::Num(1), 1, core::PageSpec::custom(2 * kInch, 2 * kInch, 40, 40, 1, 2, 2, 2, 2, 200, "color"));
    doc.edit_page(0).frames.clear();
    doc.edit_page(0).numero = false;
    return doc;
}

// A page of one 8-bit paint layer "paint", opaque light grey but a clear corner; its pixels `side` across the page (400:
// the working 200 dpi; 600: a layer of 300 dpi, as a picture opened on its own grid is).
core::Document paint_book(int side = kSide, bool gradient = false) {
    core::Document doc = pen_book();
    auto& p = doc.edit_page(0);
    p.layers.clear();
    core::Layer layer;
    layer.id = "paint";
    layer.role = core::LayerRole::User;
    layer.kind = core::LayerKind::Raster;
    layer.panel_clip = false;
    std::string bytes;
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            // (gradient: red across, green down, half blue — a colour of its own at each place)
            const int rgb[3] = {gradient ? x * 255 / side : 200, gradient ? y * 255 / side : 200, gradient ? 128 : 200};
            for (const int c : rgb) bytes.push_back(static_cast<char>(c));
            bytes.push_back(static_cast<char>(x < 20 && y < 20 ? 0 : 255));
        }
    }
    render::raster::save_raster(p, layer, render::Image::frombytes("RGBA", Size{side, side}, bytes));
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

const std::vector<std::string>& kinds() {
    static const std::vector<std::string> all{"8", "16", "32", "cmyk", "lab", "gray"};
    return all;
}

Pixels whole(core::Document& doc, const std::string& id = "paint") {
    const Json op = Json::object();
    const core::Actor actor("human:test");
    core::OpContext c{doc, op, actor};
    render::image_ops::Window window;
    (void)render::image_ops::layer_pixels(c, 0, id, Box{0, 0, 1, 1}, &window);
    return render::image_ops::layer_pixels(c, 0, id, Box{0, 0, window.grid.width, window.grid.height});
}

// A dab brush defined: soft, scattered a little, with a texture (procedural) — every part of the engine.
Json define_dab(const std::string& key = "my_dab", Json dabs = Json::parse(R"({"hardness": 0.4, "spacing": 0.15,
            "scatter": {"enabled": true, "scatter": {"jitter": 0.5}}, "texture": {"enabled": true, "depth": 0.4}, "seed": 5})")) {
    return Json{{"op", "define_brush"}, {"key", key}, {"label", "ダブ"}, {"engine", "dabs"}, {"dabs", std::move(dabs)}};
}

Json stroke(const std::string& kind, const std::string& layer = "ink") {
    return Json{{"op", "add_stroke"}, {"page", 1}, {"layer", layer}, {"kind", kind}, {"width_mm", 3.0}, {"stabilize", 0}, {"taper", false},
                {"points", Json::parse("[[8, 25, 0.5], [25, 30, 1.0], [42, 22, 0.8]]")}};
}

const core::Layer& ink(const core::Document& doc) { return *doc.page(0).first_layer(core::LayerRole::Ink); }

render::Image ink_picture(const core::Document& doc, int dpi = 200) {
    brushes::register_book(doc);
    return render::layer_image(doc.page(0), ink(doc), dpi, &doc);
}

int alpha_at(const render::Image& rgba, int x, int y) { return static_cast<int>(rgba.getpixel(x, y).back()); }

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

std::string grey_png(int w, int h, int value) {
    return render::write_png(render::Image::frombytes("L", Size{w, h}, std::string(std::size_t(w) * h, static_cast<char>(value))));
}

// What mixed_pieces gave for mixingReadsUnderTheLineOnly's line before it read the layer's colours at the line's points
// only (the whole layer copied in float then): the same pieces now.
constexpr const char* kMixedPieces = R"json({
    "8": [{"rgb":[10,28,226],"color_rgb":null},{"rgb":[25,65,190],"color_rgb":null},{"rgb":[36,85,172],"color_rgb":null},{"rgb":[46,96,163],"color_rgb":null},{"rgb":[54,102,158],"color_rgb":null},{"rgb":[62,107,156],"color_rgb":null},{"rgb":[70,110,155],"color_rgb":null},{"rgb":[78,113,154],"color_rgb":null},{"rgb":[86,115,154],"color_rgb":null},{"rgb":[93,117,154],"color_rgb":null},{"rgb":[101,116,153],"color_rgb":null},{"rgb":[108,114,153],"color_rgb":null},{"rgb":[115,112,153],"color_rgb":null},{"rgb":[122,109,153],"color_rgb":null},{"rgb":[130,106,153],"color_rgb":null},{"rgb":[137,102,153],"color_rgb":null},{"rgb":[144,99,153],"color_rgb":null},{"rgb":[151,96,153],"color_rgb":null},{"rgb":[157,93,153],"color_rgb":null}],
    "16": [{"rgb":[10,28,226],"color_rgb":{"precision":"u16","values":[0.03897779128130745,0.11097984313964844,0.8881285424325981]}},{"rgb":[25,65,190],"color_rgb":{"precision":"u16","values":[0.0982816359576057,0.2548651003370098,0.7464002422257966]}},{"rgb":[36,85,172],"color_rgb":{"precision":"u16","values":[0.14275657803404565,0.33149797028186273,0.6747693230124081]}},{"rgb":[46,96,163],"color_rgb":{"precision":"u16","values":[0.18000304278205423,0.37479625028722424,0.6385645249310662]}},{"rgb":[54,102,158],"color_rgb":{"precision":"u16","values":[0.21354015574735755,0.4011123058842678,0.6202673220166973]}},{"rgb":[62,107,156],"color_rgb":{"precision":"u16","values":[0.24500549914790135,0.4184162513882506,0.6110196581073836]}},{"rgb":[70,110,155],"color_rgb":{"precision":"u16","values":[0.2754362517712163,0.4319705140356924,0.6063453823912377]}},{"rgb":[78,113,154],"color_rgb":{"precision":"u16","values":[0.30571142458448225,0.4427326576382506,0.6039828431372549]}},{"rgb":[86,115,154],"color_rgb":{"precision":"u16","values":[0.33568184048521754,0.4524276135014553,0.6027888279335172]}},{"rgb":[93,117,154],"color_rgb":{"precision":"u16","values":[0.3653143789253983,0.45879594391467526,0.6021845200482536]}},{"rgb":[101,116,153],"color_rgb":{"precision":"u16","values":[0.39424495323031555,0.45627839331533393,0.6018790450750613]}},{"rgb":[108,114,153],"color_rgb":{"precision":"u16","values":[0.4231390261182598,0.4483905268650429,0.6017250809015012]}},{"rgb":[115,112,153],"color_rgb":{"precision":"u16","values":[0.45170880485983456,0.4380601770737592,0.6016475303500306]}},{"rgb":[122,109,153],"color_rgb":{"precision":"u16","values":[0.48011121562882964,0.42662966859106927,0.601608336205576]}},{"rgb":[130,106,153],"color_rgb":{"precision":"u16","values":[0.5084323060278799,0.41429260852290134,0.601588649375766]}},{"rgb":[137,102,153],"color_rgb":{"precision":"u16","values":[0.5367113300398284,0.4011687334846048,0.6015786563648897]}},{"rgb":[144,99,153],"color_rgb":{"precision":"u16","values":[0.5650495342179841,0.3879365509631587,0.6015736897786458]}},{"rgb":[151,96,153],"color_rgb":{"precision":"u16","values":[0.5932393990310968,0.3749129052255668,0.6015712364047181]}},{"rgb":[157,93,153],"color_rgb":{"precision":"u16","values":[0.6143403894761029,0.36499882118374694,0.6015701593137255]}}],
    "32": [{"rgb":[10,28,226],"color_rgb":{"precision":"f32","values":[0.03897779128130745,0.11097984313964844,0.8881285424325981]}},{"rgb":[25,65,190],"color_rgb":{"precision":"f32","values":[0.0982816359576057,0.2548651003370098,0.7464002422257966]}},{"rgb":[36,85,172],"color_rgb":{"precision":"f32","values":[0.14275657803404565,0.33149797028186273,0.6747693230124081]}},{"rgb":[46,96,163],"color_rgb":{"precision":"f32","values":[0.18000304278205423,0.37479625028722424,0.6385645249310662]}},{"rgb":[54,102,158],"color_rgb":{"precision":"f32","values":[0.21354015574735755,0.4011123058842678,0.6202673220166973]}},{"rgb":[62,107,156],"color_rgb":{"precision":"f32","values":[0.24500549914790135,0.4184162513882506,0.6110196581073836]}},{"rgb":[70,110,155],"color_rgb":{"precision":"f32","values":[0.2754362517712163,0.4319705140356924,0.6063453823912377]}},{"rgb":[78,113,154],"color_rgb":{"precision":"f32","values":[0.30571142458448225,0.4427326576382506,0.6039828431372549]}},{"rgb":[86,115,154],"color_rgb":{"precision":"f32","values":[0.33568184048521754,0.4524276135014553,0.6027888279335172]}},{"rgb":[93,117,154],"color_rgb":{"precision":"f32","values":[0.3653143789253983,0.45879594391467526,0.6021845200482536]}},{"rgb":[101,116,153],"color_rgb":{"precision":"f32","values":[0.39424495323031555,0.45627839331533393,0.6018790450750613]}},{"rgb":[108,114,153],"color_rgb":{"precision":"f32","values":[0.4231390261182598,0.4483905268650429,0.6017250809015012]}},{"rgb":[115,112,153],"color_rgb":{"precision":"f32","values":[0.45170880485983456,0.4380601770737592,0.6016475303500306]}},{"rgb":[122,109,153],"color_rgb":{"precision":"f32","values":[0.48011121562882964,0.42662966859106927,0.601608336205576]}},{"rgb":[130,106,153],"color_rgb":{"precision":"f32","values":[0.5084323060278799,0.41429260852290134,0.601588649375766]}},{"rgb":[137,102,153],"color_rgb":{"precision":"f32","values":[0.5367113300398284,0.4011687334846048,0.6015786563648897]}},{"rgb":[144,99,153],"color_rgb":{"precision":"f32","values":[0.5650495342179841,0.3879365509631587,0.6015736897786458]}},{"rgb":[151,96,153],"color_rgb":{"precision":"f32","values":[0.5932393990310968,0.3749129052255668,0.6015712364047181]}},{"rgb":[157,93,153],"color_rgb":{"precision":"f32","values":[0.6143403894761029,0.36499882118374694,0.6015701593137255]}}],
    "cmyk": [{"rgb":[10,28,227],"color_rgb":null},{"rgb":[25,65,191],"color_rgb":null},{"rgb":[37,85,172],"color_rgb":null},{"rgb":[46,96,163],"color_rgb":null},{"rgb":[55,102,158],"color_rgb":null},{"rgb":[63,107,156],"color_rgb":null},{"rgb":[71,110,155],"color_rgb":null},{"rgb":[78,113,154],"color_rgb":null},{"rgb":[86,115,154],"color_rgb":null},{"rgb":[93,117,154],"color_rgb":null},{"rgb":[101,116,154],"color_rgb":null},{"rgb":[108,114,154],"color_rgb":null},{"rgb":[115,112,153],"color_rgb":null},{"rgb":[123,109,154],"color_rgb":null},{"rgb":[130,106,154],"color_rgb":null},{"rgb":[137,103,154],"color_rgb":null},{"rgb":[144,99,154],"color_rgb":null},{"rgb":[151,96,154],"color_rgb":null},{"rgb":[157,93,154],"color_rgb":null}],
    "lab": [{"rgb":[10,28,226],"color_rgb":null},{"rgb":[25,65,190],"color_rgb":null},{"rgb":[36,85,172],"color_rgb":null},{"rgb":[46,96,163],"color_rgb":null},{"rgb":[55,102,158],"color_rgb":null},{"rgb":[63,107,156],"color_rgb":null},{"rgb":[70,110,155],"color_rgb":null},{"rgb":[78,113,154],"color_rgb":null},{"rgb":[86,115,154],"color_rgb":null},{"rgb":[93,117,154],"color_rgb":null},{"rgb":[101,116,153],"color_rgb":null},{"rgb":[108,114,153],"color_rgb":null},{"rgb":[115,112,153],"color_rgb":null},{"rgb":[122,109,153],"color_rgb":null},{"rgb":[130,106,153],"color_rgb":null},{"rgb":[137,102,153],"color_rgb":null},{"rgb":[144,99,153],"color_rgb":null},{"rgb":[151,96,153],"color_rgb":null},{"rgb":[157,93,153],"color_rgb":null}],
    "gray": [{"rgb":[26,26,223],"color_rgb":null},{"rgb":[59,59,184],"color_rgb":null},{"rgb":[77,77,165],"color_rgb":null},{"rgb":[87,87,157],"color_rgb":null},{"rgb":[94,94,154],"color_rgb":null},{"rgb":[99,99,154],"color_rgb":null},{"rgb":[102,102,156],"color_rgb":null},{"rgb":[106,106,158],"color_rgb":null},{"rgb":[109,109,161],"color_rgb":null},{"rgb":[111,111,163],"color_rgb":null},{"rgb":[112,112,164],"color_rgb":null},{"rgb":[112,112,163],"color_rgb":null},{"rgb":[112,112,163],"color_rgb":null},{"rgb":[112,112,163],"color_rgb":null},{"rgb":[112,112,163],"color_rgb":null},{"rgb":[111,111,162],"color_rgb":null},{"rgb":[111,111,162],"color_rgb":null},{"rgb":[112,112,163],"color_rgb":null},{"rgb":[112,112,163],"color_rgb":null}]
})json";

}  // namespace

class TestDabLines : public QObject {
    Q_OBJECT

private slots:
    void init() {
        brushes::clear_custom();
        render::dabs::clear_pictures();
    }

    // define_brush: a dab brush's settings (only those that differ kept), its pictures given as files taken in (a tip's
    // ink, a texture's lightness) and kept by the book; a picture the book keeps named by its ref; upstream's settings
    // read, what is not the brush's here said; what cannot be refused (and nothing changed); "engine": null back to
    // drawing as its base does.
    void defineBrushTakesDabs() {
        const std::string tip_file = core::b64encode(render::write_png(render::Image::create("RGBA", Size{24, 16}, render::Ink{0, 0, 0, 255})));
        const std::string tile_file = core::b64encode(grey_png(32, 32, 90));
        Json dabs = Json::parse(R"({"hardness": 0.5, "texture": {"enabled": true}, "dual": {"enabled": true}})");
        dabs["tip"] = Json{{"png", tip_file}};
        dabs["texture"]["pattern"] = Json{{"png", tile_file}};
        dabs["dual"]["tip"] = Json{{"png", tip_file}};
        const core::Document doc = run(pen_book(), define_dab("my_pic", dabs));
        const Json& kept = doc.brush_custom["my_pic"];
        QCOMPARE(kept["engine"], Json("dabs"));
        const core::DabSettings s = core::dabs_from_json(kept["dabs"]);
        QCOMPARE(s.hardness, 0.5);
        QVERIFY(!s.tip.empty() && s.tip == s.dual.tip && !s.texture.pattern.asset.empty());
        QCOMPARE(doc.brush_tips.size(), std::size_t(2));
        QVERIFY(doc.brush_tips.contains(s.tip) && doc.brush_tips.contains(s.texture.pattern.asset));
        QVERIFY(core::uses_brushes(doc));
        QVERIFY_THROWS_NO_EXCEPTION(core::validate_brushes(doc));
        // named by its ref, once the book keeps it
        Json again = Json::parse(R"({"tip": {"asset": ""}})");
        again["tip"]["asset"] = s.tip;
        const core::Document named = run(doc, define_dab("my_named", again));
        QCOMPARE(core::dabs_from_json(named.brush_custom["my_named"]["dabs"]).tip, s.tip);
        Json missing = Json::parse(R"({"tip": {"asset": ""}})");
        missing["tip"]["asset"] = "sha256:" + std::string(64, 'a');
        QVERIFY(refusal(pen_book(), Json::array({define_dab("my_m", missing)})).find("the book has no brush picture") != std::string::npos);
        QVERIFY(refusal(pen_book(), Json::array({define_dab("my_x", Json::parse(R"({"nope": 1})"))})).find("nope") != std::string::npos);
        Json engine = define_dab("my_e");
        engine["engine"] = "ink";
        QVERIFY(refusal(pen_book(), Json::array({engine})).find("engine must be dabs") != std::string::npos);
        Json broken = Json::parse(R"({"tip": {"png": "bm90IGEgcGljdHVyZQ=="}})");
        QVERIFY(refusal(pen_book(), Json::array({define_dab("my_b", broken)})).find("tip's picture") != std::string::npos);
        // upstream's settings ("format": "photocraft"): read, and what is the line's here said
        Json up = Json{{"op", "define_brush"}, {"key", "my_up"}, {"label", "上流"}, {"engine", "dabs"}, {"format", "photocraft"},
                       {"dabs", Json::parse(R"({"size": 30, "hardness": 0.2, "color": [1, 0, 0, 1], "tip": {"sampled": {"width": 2, "height": 2, "data": [0, 1, 1, 0]}}})")}};
        const auto result = run_ops(pen_book(), Json::array({up}));
        const core::DabSettings us = core::dabs_from_json(result.doc.brush_custom["my_up"]["dabs"]);
        QVERIFY(std::abs(us.size_mm - 30 * 25.4 / 200) < 1e-12);
        QCOMPARE(result.doc.brush_tips.size(), std::size_t(1));
        QVERIFY(result.warnings.dump().find("color") != std::string::npos);
        // engine null: draws as its base does
        Json plain = Json{{"op", "define_brush"}, {"key", "my_pic"}, {"label", "ふつう"}, {"engine", nullptr}};
        const core::Document back = run(doc, plain);
        QVERIFY(!back.brush_custom["my_pic"].contains("engine"));
        // the keys are this build's own: never said to be unknown
        QVERIFY(run_ops(pen_book(), Json::array({define_dab()})).warnings.dump().find("unknown") == std::string::npos);
    }

    // add_stroke with a dab brush: the inputs stretched over the points it keeps, the flow, the line drawn (soft: values
    // between 0 and 255), the same drawn again and in parts (a region of the page), refused past its dabs' limit.
    void aDabLineIsALine() {
        core::Document doc = run(pen_book(), define_dab());
        Json op = stroke("my_dab");
        op["tilt"] = Json::parse("[[0, 0], [30, -20], [60, 10]]");
        op["time_ms"] = Json::parse("[0, 40, 90]");
        op["wheel"] = Json::parse("[1, 0.5, 0.25]");
        op["flow"] = 0.6;
        const auto result = run_ops(doc, Json::array({op}));
        QVERIFY(result.warnings.dump().find("unknown") == std::string::npos);
        doc = result.doc;
        const core::Stroke& line = *ink(doc).strokes->items.back();
        QCOMPARE(line.kind, std::string("my_dab"));
        QCOMPARE(line.tilt.size(), 2 * line.points.size());
        QCOMPARE(line.time_ms.size(), line.points.size());
        QCOMPARE(line.wheel.size(), line.points.size());
        QCOMPARE(line.time_ms.front(), 0.0);
        QCOMPARE(line.time_ms.back(), 90.0);
        QCOMPARE(*line.flow, 0.6);
        QVERIFY(core::uses_brushes(doc));
        const render::Image a = ink_picture(doc);
        int partial = 0, full = 0;
        for (int y = 0; y < a.height(); ++y) {
            for (int x = 0; x < a.width(); ++x) {
                const int v = alpha_at(a, x, y);
                partial += v > 0 && v < 255 ? 1 : 0;
                full += v == 255 ? 1 : 0;
            }
        }
        QVERIFY2(partial > 200, qPrintable(QString::number(partial)));
        QCOMPARE(a.tobytes(), ink_picture(doc).tobytes());
        // the page drawn in a part: the same pixels as the whole there
        brushes::register_book(doc);
        render::RenderOptions whole_options;
        const render::Image page = render::render_page(doc.page(0), 200, whole_options, &doc).image;
        render::RenderOptions part_options;
        part_options.region = render::RenderRegion{100, 150, 120, 90};
        const render::Image part = render::render_page(doc.page(0), 200, part_options, &doc).image;
        QCOMPARE(part.tobytes(), page.crop(Box{100, 150, 220, 240}).tobytes());
        // inputs that are not one a point: refused
        Json bad = stroke("my_dab");
        bad["tilt"] = Json::parse("[[0, 0]]");
        QVERIFY(refusal(doc, Json::array({bad})).find("tilt is") != std::string::npos);
        bad = stroke("my_dab");
        bad["flow"] = 2;
        QVERIFY(refusal(doc, Json::array({bad})).find("flow is") != std::string::npos);
        // too many dabs: refused, nothing kept
        Json dense = define_dab("my_dense", Json::parse(R"({"spacing": 0.01})"));
        Json long_line = stroke("my_dense");
        long_line["width_mm"] = 0.01;
        // (the steps are at least 1/400 inch: 320 times across the page, 16 m of line, are 250,000 of them)
        Json zigzag = Json::array();
        for (int i = 0; i < 320; ++i) zigzag.push_back(Json::array({i % 2 == 0 ? 0 : 50, 1 + i % 40}));
        long_line["points"] = zigzag;
        QVERIFY(refusal(doc, Json::array({dense, long_line})).find("dabs") != std::string::npos);
        // an erasing brush does not draw a line
        QVERIFY(refusal(doc, Json::array({define_dab("my_erasing", Json::parse(R"({"erase": true})")), stroke("my_erasing")}))
                    .find("erases") != std::string::npos);
    }

    // Every brush that is not a dab brush draws its line exactly as before: the new arguments of draw (a line's inputs,
    // a part of the page) change nothing of its pixels, and a book without dab brushes and inputs is not brushes@1.
    void everyOtherBrushDrawsAsBefore() {
        const core::PenPoints points{{3, 4, 0.3}, {10, 12, 0.9}, {18, 6, 0.6}, {25, 14, 1.0}};
        const std::vector<double> tilt{1, 2, 3, 4, 5, 6, 7, 8};
        render::dabs::LineInputs inputs;
        inputs.tilt = tilt;
        inputs.flow = 0.2;
        for (const core::Brush& b : core::builtin_brushes()) {
            const auto plain = brushes::draw(Size{300, 300}, points, 200, 1.5, b.key, "seed-1", {}, 0.3);
            const auto given = brushes::draw(Size{300, 300}, points, 200, 1.5, b.key, "seed-1", {}, 0.3, &inputs, Box{0, 0, 50, 50});
            QCOMPARE(plain.has_value(), given.has_value());
            if (!plain) continue;
            QVERIFY2(plain->origin == given->origin && plain->mask.tobytes() == given->mask.tobytes(), b.key.c_str());
            QVERIFY(!given->dabs);
            QCOMPARE(brushes::extent(Size{300, 300}, points, 200, 1.5, b.key), brushes::extent(Size{300, 300}, points, 200, 1.5, b.key));
        }
        core::Document doc = run(pen_book(), stroke("gpen"));
        QVERIFY(!core::uses_brushes(doc));
    }

    // The colour dynamics: the line keeps the 背景色 it mixes towards (the op's, else the book's), and its pixels carry
    // colours between the two.
    void colourDynamicsKeepTheirBackground() {
        core::Document doc = run(pen_book(), Json{{"op", "set_brush"}, {"background_rgb", Json::array({0, 0, 255})}});
        doc = run(doc, define_dab("my_colour", Json::parse(R"({"spacing": 0.6, "color": {"enabled": true, "fg_bg": {"jitter": 1}}, "seed": 2})")));
        Json op = stroke("my_colour");
        op["rgb"] = Json::array({255, 0, 0});
        doc = run(doc, op);
        QCOMPARE(*ink(doc).strokes->items.back()->background_rgb, (std::vector<std::int64_t>{0, 0, 255}));
        const render::Image a = ink_picture(doc);
        bool blueish = false, reddish = false;
        for (int y = 0; y < a.height(); ++y) {
            for (int x = 0; x < a.width(); ++x) {
                const auto p = a.getpixel(x, y);
                if (p[3] < 250) continue;
                blueish = blueish || p[2] > 150;
                reddish = reddish || p[0] > 150;
            }
        }
        QVERIFY(blueish && reddish);
        Json own = stroke("my_colour");
        own["background_rgb"] = Json::array({0, 255, 0});
        QCOMPARE(*ink(run(doc, own)).strokes->items.back()->background_rgb, (std::vector<std::int64_t>{0, 255, 0}));
        own["background_rgb"] = Json::array({0, 256, 0});
        QVERIFY(refusal(doc, Json::array({own})).find("background_rgb") != std::string::npos);
    }

    // add_stroke's blend on a paint layer of each kind: painted into its pixels (multiply darkens where the line goes,
    // nothing else changes), no line kept; a dab brush's too; on a pen layer refused; "normal" a line as before.
    void blendPaintsAPaintLayer() {
        for (const std::string& kind : kinds()) {
            core::Document doc = as_kind(paint_book(), kind);
            doc = run(doc, define_dab());
            const Pixels before = whole(doc);
            Json op = stroke("gpen", "");
            op.erase("layer");
            op["layer_id"] = "paint";
            op["blend"] = "multiply";
            op["rgb"] = Json::array({60, 60, 60});
            const auto result = run_ops(doc, Json::array({op}));
            core::Document painted = result.doc;
            QVERIFY(result.warnings.dump().find("unknown") == std::string::npos);
            const core::Layer& layer = painted.page(0).layers[0];
            QCOMPARE(layer.stroke_count(), std::size_t(0));
            const Pixels after = whole(painted);
            QCOMPARE(after.width(), before.width());
            const std::size_t w = before.width();
            const double k = w / (2 * kInch);  // (pixels a mm)
            const std::size_t on = std::size_t(std::lround(30 * k)) * w + std::size_t(std::lround(25 * k));
            const std::size_t off = std::size_t(std::lround(45 * k)) * w + std::size_t(std::lround(45 * k));
            QVERIFY2(!std::equal(before.at(on), before.at(on) + before.all(), after.at(on)), kind.c_str());
            QVERIFY2(std::equal(before.at(off), before.at(off) + before.all(), after.at(off)), kind.c_str());
            // the dab brush's line too
            Json dab = op;
            dab["kind"] = "my_dab";
            const Pixels dabbed = whole(*std::make_unique<core::Document>(run(doc, dab)));
            QVERIFY2(!std::equal(before.at(on), before.at(on) + before.all(), dabbed.at(on)), kind.c_str());
        }
        core::Document pen = pen_book();
        Json on_pen = stroke("gpen");
        on_pen["blend"] = "multiply";
        QVERIFY(refusal(pen, Json::array({on_pen})).find("blend paints a paint layer's pixels") != std::string::npos);
        on_pen["blend"] = "normal";
        QCOMPARE(ink(run(pen, on_pen)).stroke_count(), std::size_t(1));
        Json unknown = stroke("gpen");
        unknown.erase("layer");
        unknown["layer_id"] = "paint";
        unknown["blend"] = "sparkle";
        QVERIFY(refusal(paint_book(), Json::array({unknown})).find("blend is not one of the blends") != std::string::npos);
    }

    // erase's brush (a dab brush of the book) and block on a paint layer of each kind: the alpha taken where it went
    // (partly at opacity 0.5), nothing elsewhere — on a layer of the working 200 dpi and on one of 300 dpi (a picture's own
    // grid: kept its size, the right and the bottom too) —; a brush that is not a dab brush refused.
    void eraseByBrushAndBlock() {
        for (const int side : {kSide, 600}) {  // (the working 200 dpi, and a layer of 300 dpi: a picture's own grid)
            for (const std::string& kind : kinds()) {
                core::Document doc = run(as_kind(paint_book(side), kind), define_dab("my_hard", Json::parse(R"({"hardness": 1, "spacing": 0.1})")));
                const Pixels before = whole(doc);
                const std::size_t w = before.width();
                const double k = w / (2 * kInch);
                const auto at = [&](double x_mm, double y_mm) { return std::size_t(std::lround(y_mm * k)) * w + std::size_t(std::lround(x_mm * k)); };
                const auto alpha = [&](const Pixels& p, std::size_t i) { return p.at(i)[p.colours()]; };
                for (const Json& how : {Json{{"brush", "my_hard"}}, Json{{"block", true}}}) {
                    Json op{{"op", "erase"}, {"page", 1}, {"layer_id", "paint"}, {"width_mm", 4.0}, {"points", Json::parse("[[10, 25], [40, 25]]")}};
                    for (const auto& [key, value] : how.items()) op[key] = value;
                    const Pixels after = whole(*std::make_unique<core::Document>(run(doc, op)));
                    QCOMPARE(after.width(), before.width());  // (its grid kept: the right and the bottom too)
                    QCOMPARE(after.height(), before.height());
                    QVERIFY2(alpha(after, at(25, 25)) < 0.01f, kind.c_str());
                    QVERIFY2(alpha(after, at(25, 40)) > 0.99f, kind.c_str());
                    QVERIFY2(alpha(after, after.width() * after.height() - 1) > 0.99f, kind.c_str());
                    op["opacity"] = 0.5;
                    const Pixels half = whole(*std::make_unique<core::Document>(run(doc, op)));
                    QVERIFY2(std::abs(alpha(half, at(25, 25)) - 0.5f) < 0.02f, qPrintable(QString::fromStdString(kind) + " " + QString::number(alpha(half, at(25, 25)))));
                }
            }
        }
        // a layer of 300 dpi (not the working 200: a picture's own grid) keeps its size, erased where the eraser went
        for (const Json& how : {Json{{"brush", "my_hard"}}, Json{{"block", true}}}) {
            const core::Document fine = run(paint_book(600), define_dab("my_hard", Json::parse(R"({"hardness": 1, "spacing": 0.1})")));
            Json op{{"op", "erase"}, {"page", 1}, {"layer_id", "paint"}, {"width_mm", 4.0}, {"points", Json::parse("[[10, 25], [40, 25]]")}};
            for (const auto& [key, value] : how.items()) op[key] = value;
            const core::Document erased = run(fine, op);
            const render::Image picture = render::raster::layer_pixels(erased.page(0), erased.page(0).layers[0]);
            QCOMPARE(picture.width(), 600);
            QCOMPARE(picture.height(), 600);
            const double k = 600 / (2 * kInch);
            QCOMPARE(alpha_at(picture, static_cast<int>(25 * k), static_cast<int>(25 * k)), 0);  // (where it went)
            QCOMPARE(alpha_at(picture, static_cast<int>(25 * k), static_cast<int>(40 * k)), 255);
            QCOMPARE(alpha_at(picture, 599, 599), 255);  // (the right and the bottom kept)
            QCOMPARE(alpha_at(picture, 599, static_cast<int>(25 * k)), 255);
        }
        core::Document doc = paint_book();
        Json op{{"op", "erase"}, {"page", 1}, {"layer_id", "paint"}, {"width_mm", 4.0}, {"points", Json::parse("[[10, 25], [40, 25]]")}, {"brush", "gpen"}};
        QVERIFY(refusal(doc, Json::array({op})).find("brush must be a dab brush") != std::string::npos);
        // opacity and flow without a brush or block: not used, said (the op goes through as the eraser of before)
        op.erase("brush");
        op["opacity"] = 0.5;
        op["flow"] = 0.3;
        const auto plain = run_ops(doc, Json::array({op}));
        QVERIFY2(plain.warnings.dump().find("opacity is the eraser's brush's") != std::string::npos, plain.warnings.dump().c_str());
        QVERIFY(plain.warnings.dump().find("flow is the eraser's brush's") != std::string::npos);
        Json before_e1 = op;
        before_e1.erase("opacity");
        before_e1.erase("flow");
        const Pixels used = whole(*std::make_unique<core::Document>(plain.doc));
        const Pixels as_before = whole(*std::make_unique<core::Document>(run(doc, before_e1)));
        QCOMPARE(used.width(), as_before.width());
        QVERIFY(std::equal(used.at(0), used.at(0) + std::size_t(used.width()) * used.height() * used.all(), as_before.at(0)));
    }

    // set_brush: the kind add_stroke draws with when it names none, the 背景色, reset back to the G pen.
    void setBrushKindBackgroundReset() {
        core::Document doc = run(pen_book(), define_dab());
        doc = run(doc, Json{{"op", "set_brush"}, {"kind", "my_dab"}, {"background_rgb", Json::array({1, 2, 3})}, {"width_mm", 2.0}});
        QCOMPARE(*doc.brush_kind, std::string("my_dab"));
        QCOMPARE(*doc.brush_background_rgb, (std::vector<std::int64_t>{1, 2, 3}));
        Json op = stroke("my_dab");
        op.erase("kind");
        QCOMPARE(ink(run(doc, op)).strokes->items.back()->kind, std::string("my_dab"));
        QVERIFY(refusal(doc, Json::array({Json{{"op", "set_brush"}, {"kind", "nothing"}}})).find("kind must be one of") != std::string::npos);
        const core::Document reset = run(doc, Json{{"op", "set_brush"}, {"reset", true}});
        QVERIFY(!reset.brush_kind && !reset.brush_background_rgb);
        QCOMPARE(reset.brush_width_mm, core::Document{}.brush_width_mm);
        QCOMPARE(ink(run(reset, op)).strokes->items.back()->kind, std::string("gpen"));
        QVERIFY(run_ops(doc, Json::array({Json{{"op", "set_brush"}, {"reset", true}}})).warnings.dump().find("unknown") == std::string::npos);
        // the state's brush deleted: the kind goes with it, a line without a kind is the G pen's (review P3-9)
        const core::Document deleted = run(doc, Json{{"op", "define_brush"}, {"key", "my_dab"}, {"delete", true}});
        QVERIFY(!deleted.brush_kind);
        QCOMPARE(ink(run(deleted, op)).strokes->items.back()->kind, std::string("gpen"));
        core::Document stale = doc;  // (a book whose state names a brush it no longer has)
        stale.brush_custom.erase("my_dab");
        QCOMPARE(ink(run(stale, op)).strokes->items.back()->kind, std::string("gpen"));
    }

    // 下地混色 (design 0.4, the leftover of PC2) on the paint layer of each kind: the grey under the blue line picked up
    // (the line drawn greyer than without mix: a precise or ink layer draws its lines into its pixels), each piece's
    // colour moving toward it with the pen's inputs carried; a precise line's pieces keep a precise colour, of the line's
    // precision.
    void mixingOnEveryKindOfLayer() {
        for (const std::string& kind : kinds()) {
            const core::Document doc = as_kind(paint_book(), kind);
            Json op = stroke("gpen", "");
            op.erase("layer");
            op["layer_id"] = "paint";
            op["rgb"] = Json::array({0, 0, 255});
            op["mix"] = 0.8;
            op["stretch"] = 0.0;
            op["tilt"] = Json::parse("[[10, 20], [30, 40], [50, 60]]");
            const bool precise = kind == "16" || kind == "32";
            if (precise) op["color_rgb"] = Json{{"precision", kind == "16" ? "u16" : "f32"}, {"values", Json::array({0.0, 0.0, 1.0})}};
            Json plain_op = op;
            plain_op.erase("mix");
            const auto mixed = run_ops(doc, Json::array({op}));
            const auto plain = run_ops(doc, Json::array({plain_op}));
            QVERIFY2(mixed.warnings.dump().find("color_rgb") == std::string::npos || !precise, kind.c_str());
            // drawn: at the line's middle point (25, 30 mm), greyer than the line without mix
            const auto colour_at = [](const core::Document& d) {
                brushes::register_book(d);
                return render::render_page(d.page(0), 200, render::RenderOptions{}, &d).image.convert("RGB").getpixel(197, 236);
            };
            const std::vector<double> m = colour_at(mixed.doc), q = colour_at(plain.doc);
            const QString seen = QStringLiteral("%1: mixed %2,%3,%4 plain %5,%6,%7").arg(QString::fromStdString(kind)).arg(m[0]).arg(m[1]).arg(m[2]).arg(q[0]).arg(q[1]).arg(q[2]);
            if (kind != "gray") QVERIFY2(q[2] - q[0] > 50, qPrintable(seen));  // (the plain line blue, as far as the page shows it)
            QVERIFY2(m[0] > q[0] + 60 && m[2] - m[0] < q[2] - q[0] + 1, qPrintable(seen));
            // the pieces themselves (an 8-bit layer keeps them as lines)
            const core::Page& page = doc.page(0);
            const core::Layer& layer = page.layers[0];
            core::Stroke line;
            line.id = "line";
            line.points = {core::PointF{8, 25}, core::PointF{25, 30}, core::PointF{42, 22}};
            line.width_mm = 3.0;
            line.rgb = std::vector<std::int64_t>{0, 0, 255};
            line.tilt = {10, 20, 30, 40, 50, 60};
            line.time_ms = {0, 16, 32};
            if (precise) line.color_rgb = op["color_rgb"];
            const std::vector<core::Stroke> pieces = render::raster::mixed_pieces(page, layer, line, 0.8, 0.0);
            QVERIFY2(pieces.size() > 1, kind.c_str());
            for (const core::Stroke& piece : pieces) {
                QVERIFY2((*piece.rgb)[0] > 100 && (*piece.rgb)[2] < 250, qPrintable(QString::fromStdString(kind + " " + Json(*piece.rgb).dump())));
                QCOMPARE(piece.tilt.size(), 2 * piece.points.size());
                QCOMPARE(piece.time_ms.size(), piece.points.size());
                QCOMPARE(piece.color_rgb.has_value(), precise);
                if (precise) {
                    QCOMPARE((*piece.color_rgb)["precision"].get<std::string>(), std::string(kind == "16" ? "u16" : "f32"));
                    QVERIFY((*piece.color_rgb)["values"][0].get<double>() > 0.4);
                }
            }
            if (kind == "8") QCOMPARE(mixed.doc.page(0).layers[0].stroke_count(), pieces.size());
        }
    }

    // A line's flow, 背景色 and pen inputs go with it when an op makes it again (review P2-1): moved and transformed
    // (point for point: drawn the same, moved), warped and liquified (denser: the inputs where its points are), copied
    // (the dict form) and pasted, cut by the eraser (each piece), cut to its crossing, simplified.
    void aLinesFlowAndInputsGoWithIt() {
        core::Document doc = run(pen_book(), define_dab("my_flow", Json::parse(R"({"spacing": 0.2, "shape": {"enabled": true, "angle": {"control": "tilt"}}})")));
        Json op = stroke("my_flow");
        op["flow"] = 0.3;
        op["background_rgb"] = Json::array({10, 20, 30});
        op["tilt"] = Json::parse("[[10, 20], [30, 40], [50, 60]]");
        op["time_ms"] = Json::parse("[0, 100, 200]");
        op["wheel"] = Json::parse("[0.2, 0.5, 0.9]");
        doc = run(doc, op);
        const core::Stroke line = *ink(doc).strokes->items.back();
        QCOMPARE(line.flow, std::optional<double>(0.3));
        const auto whole_kept = [&](const core::Stroke& s, const char* how) {
            QVERIFY2(s.flow == std::optional<double>(0.3), how);
            QVERIFY2(s.background_rgb == line.background_rgb, how);
            QVERIFY2(core::inputs_fit(s.tilt, s.points.size(), 2), how);
            QVERIFY2(core::inputs_fit(s.time_ms, s.points.size(), 1), how);
            QVERIFY2(core::inputs_fit(s.wheel, s.points.size(), 1), how);
        };
        // moved: point for point, the same inputs; drawn as the line was, 8 pixels (1.016 mm) to the right
        const core::Stroke moved = render::selection::transform_stroke(line, render::selection::Matrix{1.0, 0.0, 0.0, 1.0, 1.016, 0.0});
        whole_kept(moved, "moved");
        QCOMPARE(moved.tilt, line.tilt);
        QCOMPARE(moved.time_ms, line.time_ms);
        QCOMPARE(moved.wheel, line.wheel);
        brushes::register_book(doc);
        const auto cover_of = [&](const core::Stroke& s) {
            const auto in = brushes::line_inputs(s, {20, 20, 20});
            return brushes::draw(Size{kSide, kSide}, core::stroke_points(s), 200, s.width_mm, s.kind, line.id, s.rotation, s.pressure_opacity, &in);
        };
        const auto a = cover_of(line), b = cover_of(moved);
        QVERIFY(a && b);
        long sum_a = 0, sum_b = 0;
        for (const char c : a->mask.tobytes()) sum_a += static_cast<unsigned char>(c);
        for (const char c : b->mask.tobytes()) sum_b += static_cast<unsigned char>(c);
        QVERIFY2(std::abs(sum_a - sum_b) * 200 < sum_a, qPrintable(QString::number(sum_a) + " " + QString::number(sum_b)));
        QCOMPARE(b->origin.x - a->origin.x, 8);
        // its flow is what it is drawn with: without it the line is darker
        core::Stroke full = moved;
        full.flow.reset();
        long sum_full = 0;
        for (const char c : cover_of(full)->mask.tobytes()) sum_full += static_cast<unsigned char>(c);
        QVERIFY(sum_full > sum_b);
        // warped (denser): the inputs at its points along the line, the first and the last as they were
        const core::Stroke warped = render::warp::warp_stroke(line, [](double x, double y) { return std::pair<double, double>{x, y}; });
        whole_kept(warped, "warped");
        QVERIFY(warped.points.size() > line.points.size());
        QCOMPARE(warped.tilt.front(), line.tilt.front());
        QCOMPARE(warped.tilt.back(), line.tilt.back());
        QCOMPARE(warped.wheel.back(), line.wheel.back());
        // copied and pasted (the dict form)
        const core::Stroke copied = core::coerce_stroke(core::stroke_to_dict(line));
        whole_kept(copied, "copied");
        QCOMPARE(copied.tilt, line.tilt);
        // cut by the eraser: each piece
        const core::Document cut = run(doc, Json{{"op", "erase"}, {"page", 1}, {"layer", "ink"}, {"width_mm", 1.0}, {"points", Json::parse("[[25, 20], [25, 40]]")}});
        QVERIFY(ink(cut).stroke_count() >= 2);
        for (const core::StrokePtr& piece : ink(cut).strokes->items) whole_kept(*piece, "cut");
        // cut to its crossing: a line across it, erased between
        Json across = stroke("gpen");
        across["points"] = Json::parse("[[20, 10], [20, 45]]");
        const core::Document crossed = run(run(doc, across), Json{{"op", "erase"}, {"page", 1}, {"layer", "ink"}, {"width_mm", 1.0}, {"mode", "to_crossing"},
                                                                  {"points", Json::parse("[[12, 26], [13, 26]]")}});
        bool found = false;
        for (const core::StrokePtr& piece : ink(crossed).strokes->items) {
            if (piece->kind != "my_flow") continue;
            whole_kept(*piece, "to_crossing");
            found = true;
        }
        QVERIFY(found);
        // simplified: still its brush's line, with what it was drawn with
        const core::Document simple = run(doc, Json{{"op", "simplify_stroke"}, {"page", 1}, {"layer", "ink"}, {"index", 0}, {"epsilon_mm", 0.1}});
        const core::Stroke& s = *ink(simple).strokes->items[0];
        QCOMPARE(s.kind, std::string("my_flow"));
        whole_kept(s, "simplified");
    }

    // The work of a line (review P2-2): the widest brush at a fine spacing gone back and forth over the page 1,000 times
    // — 10,000 dabs each covering the page — is refused at once, by the op and when it is drawn; a live line's dabs are
    // not remembered, a committed line's are (up to kDabsKept dabs).
    void aLineOfTooMuchWorkIsRefusedAtOnce() {
        core::Document doc = run(pen_book(), define_dab("my_big", Json::parse(R"({"spacing": 0.01})")));
        Json op = stroke("my_big");
        op["width_mm"] = 500.0;
        Json zigzag = Json::array();
        for (int i = 0; i < 1000; ++i) zigzag.push_back(Json::array({i % 2 == 0 ? 2 : 48, 2 + (i % 46)}));
        op["points"] = zigzag;
        QElapsedTimer clock;
        clock.start();
        const std::string why = refusal(doc, Json::array({op}));
        QVERIFY2(why.find("too much to lay") != std::string::npos, why.c_str());
        QVERIFY2(clock.elapsed() < 5000, qPrintable(QString::number(clock.elapsed())));
        // drawn (a book read with such a line): refused there too, at once
        core::Stroke line;
        line.id = "big";
        line.kind = "my_big";
        line.width_mm = 500.0;
        for (const Json& p : zigzag) line.points.push_back(core::PointF{p[0].get<double>(), p[1].get<double>()});
        brushes::register_book(doc);
        clock.restart();
        bool refused = false;
        try {
            (void)brushes::draw(Size{kSide, kSide}, core::stroke_points(line), 200, 500.0, "my_big", line.id, {}, 0.0, nullptr);
        } catch (const core::Error& error) {
            refused = error.code() == "too_large";
        }
        QVERIFY(refused);
        QVERIFY(clock.elapsed() < 5000);
        // remembered: a committed line's dabs, not a live one's
        doc = run(doc, define_dab());
        brushes::register_book(doc);
        const core::PenPoints pts{{5, 5, std::nullopt}, {40, 40, std::nullopt}};
        render::dabs::LineInputs live;
        live.remember = false;
        const std::size_t before = render::dabs::remembered_dabs();
        (void)brushes::draw(Size{kSide, kSide}, pts, 200, 3.0, "my_dab", "live-line", {}, 0.0, &live);
        QCOMPARE(render::dabs::remembered_dabs(), before);
        const render::dabs::LineInputs kept;
        (void)brushes::draw(Size{kSide, kSide}, pts, 200, 3.0, "my_dab", "kept-line", {}, 0.0, &kept);
        QVERIFY(render::dabs::remembered_dabs() > before);
        QVERIFY(render::dabs::remembered_dabs() <= render::dabs::kDabsKept);
    }

    // 下地混色 reads the colours under the line's points only (review P2-3: the layer is not copied whole in float): the
    // pieces of a line over a gradient are the colours they were when the whole layer was read (kMixedPieces: what
    // mixed_pieces gave before the change, on each kind of page), and an ink layer larger than a small imaging budget
    // still mixes (only its few pixels are held).
    void mixingReadsUnderTheLineOnly() {
        Json got = Json::object();
        for (const std::string& kind : kinds()) {
            const core::Document doc = as_kind(paint_book(kSide, true), kind);
            core::Stroke line;
            line.id = "line";
            line.points = {core::PointF{8, 25}, core::PointF{25, 30}, core::PointF{42, 22}};
            line.width_mm = 3.0;
            line.rgb = std::vector<std::int64_t>{0, 0, 255};
            if (kind == "16" || kind == "32") {
                line.color_rgb = Json{{"precision", kind == "16" ? "u16" : "f32"}, {"values", Json::array({0.0, 0.0, 1.0})}};
            }
            Json pieces = Json::array();
            for (const core::Stroke& piece : render::raster::mixed_pieces(doc.page(0), doc.page(0).layers[0], line, 0.8, 0.5)) {
                pieces.push_back(Json{{"rgb", *piece.rgb}, {"color_rgb", piece.color_rgb ? *piece.color_rgb : Json(nullptr)}});
            }
            got[kind] = pieces;
        }
        if (qEnvironmentVariableIsSet("GENKO_DAB_GOLDEN_DUMP")) qWarning("GOLDEN %s", got.dump().c_str());
        QCOMPARE(got.dump(), Json::parse(kMixedPieces).dump());
        // an ink layer of 16 bits, 600 pixels a side (its pixels in float: 7 MB), mixed in a budget of 2 MB
        const core::Document big = as_kind(paint_book(600, true), "cmyk");
        core::Stroke line;
        line.id = "line";
        line.points = {core::PointF{8, 25}, core::PointF{42, 22}};
        line.width_mm = 3.0;
        line.rgb = std::vector<std::int64_t>{0, 0, 255};
        const render::ImageAllocationBudget budget(2 * 1024 * 1024);
        QVERIFY(render::raster::mixed_pieces(big.page(0), big.page(0).layers[0], line, 0.8, 0.5).size() > 1);
    }

    // brush.get (`genko brushes get <key> [--book <dir>]`): a dab brush of the book with every setting (the defaults too,
    // a round tip as "round"); a built-in brush without dabs; a brush that is not there refused.
    void brushGetSaysEverySetting() {
        QTemporaryDir tmp;
        const QString book = tmp.path() + QStringLiteral("/b.genko");
        const fs::path dir = core::path_from_utf8(book.toStdString());
        const core::Document doc = pen_book();
        write_book(doc, dir);
        const auto defined = run_ops(doc, Json::array({define_dab()}));
        save_next(defined.doc, dir, defined.journal_ops);
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("GENKO_CONFIG_DIR"), tmp.path() + QStringLiteral("/config"));
        auto ran = genko::test::run_genko({QStringLiteral("brushes"), QStringLiteral("get"), QStringLiteral("my_dab"), QStringLiteral("--book"), book}, env);
        QVERIFY2(ran.finished && ran.exit_code == 0, qPrintable(ran.described()));
        Json got = genko::test::one_line(ran.out);
        QCOMPARE(got["key"], Json("my_dab"));
        QCOMPARE(got["from"], Json("book"));
        QCOMPARE(got["engine"], Json("dabs"));
        QCOMPARE(got["dabs"]["hardness"], Json(0.4));
        QCOMPARE(got["dabs"]["size_mm"].get<double>(), core::DabSettings{}.size_mm);
        QCOMPARE(got["dabs"]["tip"], Json("round"));
        QVERIFY(got["dabs"].contains("dual") && got["dabs"]["dual"].contains("spacing"));
        QCOMPARE(core::dabs_from_json(got["dabs"]), *core::brush_for(Json("my_dab"), defined.doc).dabs);  // (read back the same)
        ran = genko::test::run_genko({QStringLiteral("brushes"), QStringLiteral("get"), QStringLiteral("gpen")}, env);
        QVERIFY2(ran.finished && ran.exit_code == 0, qPrintable(ran.described()));
        got = genko::test::one_line(ran.out);
        QCOMPARE(got["from"], Json("builtin"));
        QVERIFY(got["engine"].is_null() && !got.contains("dabs"));
        QCOMPARE(got["settings"]["width_mm"], Json(core::find_builtin("gpen")->width_mm));
        ran = genko::test::run_genko({QStringLiteral("brushes"), QStringLiteral("get"), QStringLiteral("my_none")}, env);
        QCOMPARE(ran.exit_code, 1);
        QCOMPARE(genko::test::one_line(ran.out)["code"], Json("key"));
    }

    // A line's 背景色 (review P3-14): a line that has only it is brushes@1; written in the packed and the dict form and read
    // back; broken (a value past 255, two of them, a fraction) the line is not read (core::Error("format"): the reader
    // opens the book read-only).
    void aLinesBackgroundIsKeptAndChecked() {
        core::Document doc = run(pen_book(), stroke("gpen"));
        QVERIFY(!core::uses_brushes(doc));
        core::Stroke line = *ink(doc).strokes->items.back();
        line.background_rgb = std::vector<std::int64_t>{4, 5, 6};
        {
            core::Layer& layer = doc.edit_page(0).layers[static_cast<std::size_t>(&ink(doc) - doc.page(0).layers.data())];
            layer.strokes = core::make_strokes({std::make_shared<const core::Stroke>(line)});
        }
        QVERIFY(core::uses_brushes(doc));
        // (add_stroke keeps it only for a dab brush's line: a G pen's is not drawn with it — said, not kept)
        Json plain = stroke("gpen");
        plain["background_rgb"] = Json::array({4, 5, 6});
        plain["flow"] = 0.5;
        const auto unused = run_ops(pen_book(), Json::array({plain}));
        QVERIFY(!ink(unused.doc).strokes->items.back()->background_rgb && !ink(unused.doc).strokes->items.back()->flow);
        QVERIFY2(unused.warnings.dump().find("a dab brush's line's") != std::string::npos, unused.warnings.dump().c_str());
        QVERIFY(!core::uses_brushes(unused.doc));
        QCOMPARE(core::coerce_stroke(core::stroke_to_packed(line)).background_rgb, line.background_rgb);
        QCOMPARE(core::coerce_stroke(core::stroke_to_dict(line)).background_rgb, line.background_rgb);
        for (const Json& broken : {Json::array({0, 256, 0}), Json::array({1, 2}), Json::array({1.5, 2, 3}), Json::array({-1, 2, 3})}) {
            Json packed = core::stroke_to_packed(line);
            packed["background_rgb"] = broken;
            bool refused = false;
            try {
                (void)core::coerce_stroke(packed);
            } catch (const core::Error& error) {
                refused = error.code() == "format";
            }
            QVERIFY2(refused, broken.dump().c_str());
        }
    }

    // Saved, opened (brushes@1, the pictures), drawn the same; Undo gives back the book without the line (and without
    // the feature once the brush goes too); Redo the same pixels again.
    void savedOpenedUndoneRedone() {
        QTemporaryDir tmp;
        const fs::path dir = core::path_from_utf8((tmp.path() + QStringLiteral("/d.genko")).toStdString());
        core::Document doc = pen_book();
        write_book(doc, dir);
        Json dabs = Json::parse(R"({"hardness": 0.3, "texture": {"enabled": true, "depth": 0.5}})");
        dabs["texture"]["pattern"] = Json{{"png", core::b64encode(grey_png(16, 16, 128))}};
        const auto defined = run_ops(doc, Json::array({define_dab("my_saved", dabs)}));
        // (the journal keeps the op with its picture told, not held: the book keeps it as an asset — review P3-1)
        QVERIFY2(defined.journal_ops[0]["dabs"]["texture"]["pattern"]["png"].get<std::string>().starts_with("<"), defined.journal_ops.dump().substr(0, 300).c_str());
        save_next(defined.doc, dir, defined.journal_ops);
        // (a line with every input: tilt, time, wheel, flow and 背景色 — review P3-14)
        Json line = stroke("my_saved");
        line["tilt"] = Json::parse("[[10, 20], [-30, 40], [50, -60]]");
        line["time_ms"] = Json::parse("[0, 120.5, 300]");
        line["wheel"] = Json::parse("[0.25, 0.5, 1]");
        line["flow"] = 0.4;
        line["background_rgb"] = Json::array({1, 2, 250});
        const auto drawn = run_ops(defined.doc, Json::array({line}));
        save_next(drawn.doc, dir, drawn.journal_ops);
        const render::Image here = ink_picture(drawn.doc);
        const core::Stroke was = *ink(drawn.doc).strokes->items.back();
        const auto same_line = [&](const core::Document& d) {
            const core::Stroke& now = *ink(d).strokes->items.back();
            QCOMPARE(now.tilt, was.tilt);
            QCOMPARE(now.time_ms, was.time_ms);
            QCOMPARE(now.wheel, was.wheel);
            QCOMPARE(now.flow, was.flow);
            QCOMPARE(now.background_rgb, was.background_rgb);
        };
        brushes::clear_custom();
        render::dabs::clear_pictures();
        const auto loaded = storage::load_document(dir);
        QVERIFY2(loaded.document.read_only_reason.empty(), loaded.document.read_only_reason.c_str());
        QVERIFY(std::find(loaded.document.features.begin(), loaded.document.features.end(), "brushes@1") != loaded.document.features.end());
        same_line(loaded.document);
        QCOMPARE(ink_picture(loaded.document).tobytes(), here.tobytes());
        storage::restore(dir, "human:test", false, false);  // undo the line
        brushes::clear_custom();
        QCOMPARE(ink(storage::load_document(dir).document).stroke_count(), std::size_t(0));
        storage::restore(dir, "human:test", true, false);  // redo
        brushes::clear_custom();
        render::dabs::clear_pictures();
        const auto redone = storage::load_document(dir);
        same_line(redone.document);
        QCOMPARE(ink_picture(redone.document).tobytes(), here.tobytes());
        storage::restore(dir, "human:test", false, false);
        storage::restore(dir, "human:test", false, false);  // the brush undone too: no feature
        const auto bare = storage::load_document(dir);
        QVERIFY(std::find(bare.document.features.begin(), bare.document.features.end(), "brushes@1") == bare.document.features.end());
    }
};

QTEST_GUILESS_MAIN(TestDabLines)
#include "test_dab_lines.moc"
