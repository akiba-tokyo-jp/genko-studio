// PC3 C-d (SPEC.md COLOR-07「移動・整列」) as an agent or a person uses the ops: each kind of layer's box (a pen layer's
// lines and shape parts as drawn, a paint layer's pixels without its mask, a placed picture where it lies, a tone's screen,
// a folder's layers together; a fill or an adjustment layer none) and a line of dialogue's (its balloon's frame);
// align_layers to the layers together, to one alone's paper or selection, to the trim, the basic frame and a panel, by
// whole working pixels (a line exactly), the linked layers along, a position lock refused only when the layer would move;
// distribute_layers by edges and gaps; arrange_layers in a folder and across it (Reverse), nothing moving refused;
// link_layers making and undoing link groups (a group of one undone), the linked layers moving together; set_layer's
// lock_position. And through the command line: saved with transform@1, undone, `genko inspect --at` listing the layers
// shown at a point, the topmost first. (Python has none of these: this build's own contract.)

#include <QtTest>

#include <QTemporaryDir>

#include <array>
#include <cmath>
#include <string>
#include <string_view>

#include "core/base64.hpp"
#include "core/command_bus.hpp"
#include "core/json.hpp"
#include "render/image.hpp"
#include "render/layer_bounds.hpp"
#include "render/layer_transform.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/raster.hpp"
#include "storage/asset_store.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
namespace lb = render::layer_bounds;

namespace {

constexpr double kInch = 25.4;  // (a 25.4 mm page: 200 working pixels a side)
constexpr int kSide = 200;
constexpr double kPx = kInch / kSide;

core::ApplyResult run_ops(const core::Document& doc, const Json& ops) { return core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor("human:test")); }
core::Document run(const core::Document& doc, const Json& op) { return run_ops(doc, Json::array({op})).doc; }

std::string refusal(const core::Document& doc, const Json& op) {
    try {
        run_ops(doc, Json::array({op}));
    } catch (const core::ApplyError& error) {
        return error.code() + ": " + error.what();
    }
    return {};
}

// A clear picture with a block from (x0, y0) to (x1, y1) working pixels.
render::Image block(int x0, int y0, int x1, int y1) {
    std::string bytes(std::size_t(kSide) * kSide * 4, '\0');
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const std::size_t i = (std::size_t(y) * kSide + std::size_t(x)) * 4;
            bytes[i] = static_cast<char>(200);
            bytes[i + 1] = static_cast<char>(x % 256);
            bytes[i + 2] = static_cast<char>(y % 256);
            bytes[i + 3] = static_cast<char>(255);
        }
    }
    return render::Image::frombytes("RGBA", render::Size{kSide, kSide}, bytes);
}

void add_paint(core::Document& doc, const std::string& id, const render::Image& picture) {
    auto& p = doc.edit_page(0);
    core::Layer layer;
    layer.id = id;
    layer.kind = core::LayerKind::Raster;
    layer.panel_clip = false;
    render::raster::save_raster(p, layer, picture);
    p.layers.push_back(std::move(layer));
}

// A 25.4 mm page without panels: the paint layer "paint" (a block from (40, 50) to (80, 110)) and the pen layer "pen"
// (a line from (5, 5) to (10, 6) mm).
core::Document book(bool panels = false) {
    auto doc = core::new_episode("整列", core::Num(1), 1, core::PageSpec::custom(kInch, kInch, 20, 20, 1, 2, 2, 2, 2, 200, "color"));
    auto& p = doc.edit_page(0);
    p.numero = false;
    if (!panels) p.frames.clear();
    p.layers.clear();
    add_paint(doc, "paint", block(40, 50, 80, 110));
    doc = run(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "pen"}, {"panel_each", false}});
    doc = run(doc, Json{{"op", "set_layer"}, {"page", 1}, {"id", "pen"}, {"panel_clip", false}});
    doc = run(doc, Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", "pen"}, {"points", Json::array({Json::array({5, 5}), Json::array({10, 6})})},
                        {"width_mm", 0.4}});
    return doc;
}

const core::Layer& layer_of(const core::Document& doc, std::string_view id) {
    for (const core::Layer& layer : doc.page(0).layers)
        if (layer.id == id) return layer;
    throw std::runtime_error("no layer " + std::string(id));
}

lb::Box box_of(const core::Document& doc, const std::string& id) { return *lb::of_layer(doc, doc.page(0), id); }

// The box in working pixels.
std::array<long, 4> px(const lb::Box& b) { return {std::lround(b[0] / kPx), std::lround(b[1] / kPx), std::lround(b[2] / kPx), std::lround(b[3] / kPx)}; }

// The working pixels a box (mm) touches (its edges anti-aliased: a pixel partly in is in).
std::array<long, 4> covering(const lb::Box& b) {
    return {static_cast<long>(std::floor(b[0] / kPx)), static_cast<long>(std::floor(b[1] / kPx)), static_cast<long>(std::ceil(b[2] / kPx)),
            static_cast<long>(std::ceil(b[3] / kPx))};
}

bool near(double a, double b, double within) { return std::fabs(a - b) <= within; }

std::vector<std::string> order(const core::Document& doc) {
    std::vector<std::string> out;
    for (const core::Layer& layer : doc.page(0).layers) out.push_back(layer.id);
    return out;
}

Json ids(std::initializer_list<const char*> list) {
    Json out = Json::array();
    for (const char* id : list) out.push_back(id);
    return out;
}

}  // namespace

class TestContractAlign : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;
    QString path(const QString& rel) const { return tmp_.path() + QStringLiteral("/") + rel; }
    Json genko(const QStringList& args, int* exit = nullptr) {
        const auto ran = genko::test::run_genko(args);
        if (exit != nullptr) *exit = ran.exit_code;
        if (!ran.finished) return Json{{"not_run", ran.described().toStdString()}};
        return genko::test::one_line(ran.out);
    }
    Json apply(const QString& book, const QString& name, const Json& ops) {
        genko::test::write_bytes(path(name), ops.dump());
        int exit = -1;
        const Json out = genko({QStringLiteral("apply"), book, path(name), QStringLiteral("--agent"), QStringLiteral("human:test")}, &exit);
        if (exit != 0) return Json{{"failed", out}};
        return out;
    }
    Json project(const QString& book) { return Json::parse(genko::test::read_bytes(book + QStringLiteral("/project.json"))); }

private slots:
    void initTestCase() { QVERIFY(tmp_.isValid()); }

    // What each kind of layer is aligned by: what it holds as drawn (without its masks), on the working grid.
    void theBoxesOfEachKind() {
        core::Document doc = book();
        QCOMPARE(px(box_of(doc, "paint")), (std::array<long, 4>{40, 50, 80, 110}));
        {  // (a pen line as drawn: its width around its points)
            const lb::Box b = box_of(doc, "pen");
            QVERIFY2(near(b[0], 4.8, 0.3) && near(b[1], 4.8, 0.3) && near(b[2], 10.2, 0.3) && near(b[3], 6.2, 0.3), core::Json(b).dump().c_str());
            for (const double v : b) QVERIFY(near(v / kPx, std::round(v / kPx), 1e-6));  // (whole working pixels)
        }
        // a shape part (its fill), a tone's screen, a placed picture where it lies, a folder's layers, a fill: none
        doc = run(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "shapes"}, {"panel_each", false}});
        doc = run(doc, Json{{"op", "set_layer"}, {"page", 1}, {"id", "shapes"}, {"panel_clip", false}});
        doc = run(doc, Json{{"op", "add_shape_part"}, {"page", 1}, {"layer_id", "shapes"}, {"id", "r1"},
                            {"shape", Json{{"kind", "rect"}, {"rect", Json::array({2, 12, 4, 4})}}}, {"fill", Json{{"rgb", Json::array({0, 0, 0})}}}});
        QCOMPARE(px(box_of(doc, "shapes")), covering(lb::Box{2, 12, 6, 16}));
        doc = run(doc, Json{{"op", "add_tone"}, {"page", 1}, {"id", "tn"}, {"density", 0.5}, {"area", Json{{"rect", Json::array({12, 14, 6, 4})}}}});
        {
            const lb::Box b = box_of(doc, "tn");
            QVERIFY2(b[0] >= 12 - 2 * kPx && b[1] >= 14 - 2 * kPx && b[2] <= 18 + 2 * kPx && b[3] <= 18 + 2 * kPx && b[2] - b[0] > 5 && b[3] - b[1] > 3, core::Json(b).dump().c_str());
        }
        QTemporaryDir assets;
        doc.asset_dir = std::filesystem::path(assets.path().toStdString());
        {
            core::Layer placed;
            placed.id = "placed";
            placed.kind = core::LayerKind::Placed;
            placed.asset = storage::AssetStore(*doc.asset_dir).put_bytes(render::write_png(block(0, 0, 200, 200)), ".png");
            placed.placement_mm = core::Rect{core::Num(3.0), core::Num(15.0), core::Num(5.08), core::Num(5.08)};
            placed.fit = "stretch";
            placed.clip_to = "none";
            doc.edit_page(0).layers.push_back(placed);
        }
        QCOMPARE(px(box_of(doc, "placed")), px(lb::Box{3, 15, 8.08, 20.08}));  // (laid on the nearest pixels)
        doc = run(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "folder"}, {"id", "box"}});
        doc = run(doc, Json{{"op", "set_layer"}, {"page", 1}, {"id", "pen"}, {"parent", "box"}});
        doc = run(doc, Json{{"op", "set_layer"}, {"page", 1}, {"id", "shapes"}, {"parent", "box"}});
        const lb::Box folder = box_of(doc, "box"), pen = box_of(doc, "pen"), shapes = box_of(doc, "shapes");
        QCOMPARE(folder, (lb::Box{std::min(pen[0], shapes[0]), std::min(pen[1], shapes[1]), std::max(pen[2], shapes[2]), std::max(pen[3], shapes[3])}));
        doc = run(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "fill"}, {"id", "fill"}});
        doc = run(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"}, {"id", "adj"}});
        QVERIFY(!lb::of_layer(doc, doc.page(0), "fill") && !lb::of_layer(doc, doc.page(0), "adj"));
        // (a mask does not cut what the layer holds)
        doc = run(doc, Json{{"op", "set_layer_mask"}, {"page", 1}, {"id", "paint"}, {"area", Json{{"rect", Json::array({0, 0, 7, 25.4})}}}});
        QCOMPARE(px(box_of(doc, "paint")), (std::array<long, 4>{40, 50, 80, 110}));
        // Auto-Select: the layers showing at a point, the topmost first (the mask shows the paint's left part only)
        QCOMPARE(lb::layers_at(doc, doc.page(0), 4, 13), (std::vector<std::string>{"fill", "shapes"}));
        QCOMPARE(lb::layers_at(doc, doc.page(0), 6, 9), (std::vector<std::string>{"fill", "paint"}));
        QCOMPARE(lb::layers_at(doc, doc.page(0), 9, 9), (std::vector<std::string>{"fill"}));
        QCOMPARE(lb::outermost(doc.page(0), "shapes"), std::string("box"));
        QCOMPARE(lb::outermost(doc.page(0), "paint"), std::string("paint"));
        doc = run(doc, Json{{"op", "set_layer"}, {"page", 1}, {"id", "box"}, {"visible", false}});
        QCOMPARE(lb::layers_at(doc, doc.page(0), 4, 13), (std::vector<std::string>{"fill"}));
    }

    // Align: to the layers together; one alone to its paper or the selection; Genko's bases; a line exactly; links along.
    void alignLayersAndLines() {
        core::Document doc = run(book(), Json{{"op", "add_line"}, {"page", 1}, {"text", "ね"}, {"x_mm", 14.3}, {"y_mm", 2}, {"w_mm", 6}, {"h_mm", 4}, {"id", "l1"}});
        Json op{{"op", "align_layers"}, {"page", 1}, {"ids", ids({"paint", "pen"})}, {"lines", ids({"l1"})}, {"edge", "left"}};
        core::ApplyResult done = run_ops(doc, Json::array({op}));
        const lb::Box pen = box_of(doc, "pen");
        QCOMPARE(px(box_of(done.doc, "paint"))[0], px(pen)[0]);
        QCOMPARE(px(box_of(done.doc, "pen")), px(pen));  // (the leftmost stays)
        QVERIFY(near(done.doc.story.front().x_mm.value(), pen[0], 1e-6));  // (the line exactly on it)
        QCOMPARE(done.results[0]["moved"], ids({"paint"}));
        QCOMPARE(done.results[0]["lines"], ids({"l1"}));
        // (the paint layer's pixels moved exactly: the block whole)
        QCOMPARE(px(box_of(done.doc, "paint"))[2] - px(box_of(done.doc, "paint"))[0], 40L);
        // the right edges: to the line's (the rightmost), the layers by whole pixels
        op["edge"] = "right";
        done = run_ops(doc, Json::array({op}));
        QVERIFY(near(box_of(done.doc, "paint")[2], 20.3, kPx / 2 + 1e-9));
        QVERIFY(near(done.doc.story.front().x_mm.value(), 14.3, 1e-9));
        // one alone: to the paper; with an area: to the area
        Json one{{"op", "align_layers"}, {"page", 1}, {"ids", ids({"paint"})}, {"edge", "hcenter"}};
        done = run_ops(doc, Json::array({one}));
        QCOMPARE(px(box_of(done.doc, "paint")), (std::array<long, 4>{80, 50, 120, 110}));
        QCOMPARE(done.results[0]["box"], Json::array({0.0, 0.0, 25.4, 25.4}));
        one["edge"] = "top";
        one["area"] = Json{{"rect", Json::array({0, 2.54, 10, 10})}};
        QCOMPARE(px(box_of(run(doc, one), "paint"))[1], 20L);
        one["to"] = "layers";  // (the layers' own: one alone stays)
        QCOMPARE(px(box_of(run(doc, one), "paint")), px(box_of(doc, "paint")));
        one["to"] = "selection";
        one.erase("area");
        QVERIFY(refusal(doc, one).find("no selection") != std::string::npos);
        // Genko's bases: the trim, the basic frame, a panel
        core::Document framed = book(true);
        const core::Page& page = framed.page(0);
        const core::Rect trim = page.trim_rect_mm(), inner = page.inner_rect_mm();
        Json base{{"op", "align_layers"}, {"page", 1}, {"ids", ids({"paint"})}, {"edge", "left"}, {"to", "trim"}};
        QVERIFY(near(box_of(run(framed, base), "paint")[0], trim.x.value(), kPx / 2 + 1e-9));
        base["to"] = "frame";
        base["edge"] = "bottom";
        QVERIFY(near(box_of(run(framed, base), "paint")[3], (inner.y + inner.height).value(), kPx / 2 + 1e-9));
        base["to"] = "panel";
        QVERIFY(refusal(framed, base).find("frame_id") != std::string::npos);
        const core::Frame* panel = page.leaf_frames().front();
        base["frame_id"] = panel->id;
        base["edge"] = "vcenter";
        const lb::Box in_panel = box_of(run(framed, base), "paint");
        QVERIFY(near((in_panel[1] + in_panel[3]) / 2, (panel->rect.y + panel->rect.height / core::Num(2)).value(), kPx + 1e-9));
        base["frame_id"] = "nope";
        QVERIFY(refusal(framed, base).find("not found") != std::string::npos);
        // a linked layer goes along by the same amount; a position lock refuses, unless the layer stays
        core::Document linked = run(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "friend"}, {"panel_each", false}});
        linked = run(linked, Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", "friend"}, {"points", Json::array({Json::array({20, 20}), Json::array({22, 20})})}});
        linked = run(linked, Json{{"op", "link_layers"}, {"page", 1}, {"ids", ids({"paint", "friend"})}});
        const double friend_x = layer_of(linked, "friend").strokes->items[0]->points[0].x;
        done = run_ops(linked, Json::array({Json{{"op", "align_layers"}, {"page", 1}, {"ids", ids({"paint", "pen"})}, {"edge", "left"}}}));
        const double moved = box_of(done.doc, "paint")[0] - box_of(linked, "paint")[0];
        QVERIFY(near(layer_of(done.doc, "friend").strokes->items[0]->points[0].x, friend_x + moved, 1e-6));
        QCOMPARE(done.results[0]["moved"], ids({"paint", "friend"}));
        core::Document locked = linked;
        for (core::Layer& layer : locked.edit_page(0).layers)
            if (layer.id == "friend") layer.lock_position = true;
        QVERIFY(refusal(locked, Json{{"op", "align_layers"}, {"page", 1}, {"ids", ids({"paint", "pen"})}, {"edge", "left"}}).find("position") != std::string::npos);
        QVERIFY(refusal(locked, Json{{"op", "align_layers"}, {"page", 1}, {"ids", ids({"paint", "pen"})}, {"edge", "top"}}).find("position") != std::string::npos);
        // (a locked layer that stays where it is: no refusal — the pen is the leftmost)
        core::Document pen_locked = doc;
        for (core::Layer& layer : pen_locked.edit_page(0).layers)
            if (layer.id == "pen") layer.lock_position = true;
        QVERIFY(refusal(pen_locked, Json{{"op", "align_layers"}, {"page", 1}, {"ids", ids({"paint", "pen"})}, {"edge", "left"}}).empty());
        QVERIFY(refusal(doc, Json{{"op", "align_layers"}, {"page", 1}, {"ids", ids({"paint"})}, {"edge", "nowhere"}}).find("edge is") != std::string::npos);
        QVERIFY(refusal(doc, Json{{"op", "align_layers"}, {"page", 1}, {"ids", ids({"paint"})}, {"edge", "top"}, {"to", "moon"}}).find("to is") != std::string::npos);
        QVERIFY(refusal(doc, Json{{"op", "align_layers"}, {"page", 1}, {"ids", ids({"nope"})}, {"edge", "top"}}).find("not found") != std::string::npos);
        core::Document empty = run(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "empty"}});
        QVERIFY(refusal(empty, Json{{"op", "align_layers"}, {"page", 1}, {"ids", ids({"empty"})}, {"edge", "top"}}).find("nothing to align") != std::string::npos);
    }

    // Distribute: three by an edge (the outer two stay) and by equal gaps; fewer refused.
    void distributeLayers() {
        core::Document doc = book();
        add_paint(doc, "mid", block(100, 20, 120, 40));
        add_paint(doc, "far", block(170, 30, 200, 60));
        Json op{{"op", "distribute_layers"}, {"page", 1}, {"ids", ids({"paint", "mid", "far"})}, {"by", "left"}};
        core::ApplyResult done = run_ops(doc, Json::array({op}));
        QCOMPARE(px(box_of(done.doc, "mid"))[0], 105L);  // (40, 170: 105)
        QCOMPARE(px(box_of(done.doc, "paint")), px(box_of(doc, "paint")));
        QCOMPARE(px(box_of(done.doc, "far")), px(box_of(doc, "far")));
        op["by"] = "horizontal";  // (span 160, widths 90: gaps of 35)
        QCOMPARE(px(box_of(run(doc, op), "mid"))[0], 115L);
        op["by"] = "vertical";
        QVERIFY(refusal(doc, op).empty());
        op["by"] = "diagonal";
        QVERIFY(refusal(doc, op).find("by is") != std::string::npos);
        op["by"] = "top";
        op["ids"] = ids({"paint", "mid"});
        QVERIFY(refusal(doc, op).find("three or more") != std::string::npos);
    }

    // Arrange among siblings: a folder with its layers; nothing moving refused; Reverse.
    void arrangeLayers() {
        core::Document doc = book();
        doc = run(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "folder"}, {"id", "box"}});
        doc = run(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "in1"}, {"parent", "box"}});
        doc = run(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "in2"}, {"parent", "box"}});
        doc = run(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "top"}});
        using V = std::vector<std::string>;
        const V before = order(doc);
        QCOMPARE(before, (V{"paint", "pen", "box", "in1", "in2", "top"}));  // (add_layer puts a folder's layers over it)
        Json op{{"op", "arrange_layers"}, {"page", 1}, {"ids", ids({"pen"})}, {"to", "forward"}};
        core::ApplyResult done = run_ops(doc, Json::array({op}));
        QCOMPARE(order(done.doc), (V{"paint", "box", "in1", "in2", "pen", "top"}));
        QCOMPARE(done.results[0]["order"], Json(order(done.doc)));
        op["ids"] = ids({"in1"});
        op["to"] = "front";
        QCOMPARE(order(run(doc, op)), (V{"paint", "pen", "box", "in2", "in1", "top"}));
        QCOMPARE(layer_of(run(doc, op), "in1").parent_id, Json("box"));
        op["ids"] = ids({"box"});
        op["to"] = "back";
        QCOMPARE(order(run(doc, op)), (V{"box", "in1", "in2", "paint", "pen", "top"}));
        op["ids"] = ids({"top"});
        op["to"] = "forward";
        QVERIFY(refusal(doc, op).find("cannot move further") != std::string::npos);
        op["ids"] = ids({"paint", "top"});
        op["to"] = "reverse";
        QCOMPARE(order(run(doc, op)), (V{"top", "pen", "box", "in1", "in2", "paint"}));
        op["ids"] = ids({"paint"});
        QVERIFY(refusal(doc, op).find("two or more") != std::string::npos);
        op["to"] = "upward";
        QVERIFY(refusal(doc, op).find("to is") != std::string::npos);
    }

    // Links: made, moved together, undone by the same command; a group of one is none; lock_position by set_layer.
    void linksAndPositionLocks() {
        core::Document doc = book();
        add_paint(doc, "other", block(0, 0, 10, 10));
        core::ApplyResult done = run_ops(doc, Json::array({Json{{"op", "link_layers"}, {"page", 1}, {"ids", ids({"paint", "pen"})}}}));
        QCOMPARE(done.results[0]["linked"], Json(true));
        QCOMPARE(done.results[0]["link"], Json("link-1"));
        core::Document linked = done.doc;
        QCOMPARE(layer_of(linked, "paint").link, std::optional<std::string>("link-1"));
        QCOMPARE(layer_of(linked, "pen").link, std::optional<std::string>("link-1"));
        // (moved together: transform_layers names one)
        const core::Document moved = run(linked, Json{{"op", "transform_layers"}, {"page", 1}, {"ids", ids({"pen"})}, {"move", Json::array({kPx * 3, 0})}});
        QCOMPARE(px(box_of(moved, "paint"))[0], 43L);
        // the same again: unlinked
        done = run_ops(linked, Json::array({Json{{"op", "link_layers"}, {"page", 1}, {"ids", ids({"pen", "paint"})}}}));
        QCOMPARE(done.results[0]["linked"], Json(false));
        QVERIFY(!layer_of(done.doc, "paint").link && !layer_of(done.doc, "pen").link);
        // pen and other: a new group; paint left alone in its own is no link
        done = run_ops(linked, Json::array({Json{{"op", "link_layers"}, {"page", 1}, {"ids", ids({"pen", "other"})}}}));
        QCOMPARE(done.results[0]["link"], Json("link-2"));
        QVERIFY(!layer_of(done.doc, "paint").link);
        QCOMPARE(layer_of(done.doc, "other").link, std::optional<std::string>("link-2"));
        // link: false undoes, link: true links even a group already
        QVERIFY(!layer_of(run(linked, Json{{"op", "link_layers"}, {"page", 1}, {"ids", ids({"paint"})}, {"link", false}}), "pen").link);
        QVERIFY(refusal(doc, Json{{"op", "link_layers"}, {"page", 1}, {"ids", ids({"paint"})}}).find("two or more") != std::string::npos);
        QVERIFY(refusal(doc, Json{{"op", "link_layers"}, {"page", 1}, {"ids", ids({"paint", "pen"})}, {"link", "yes"}}).find("true or false") != std::string::npos);
        // set_layer's lock_position: the layer does not move
        const core::Document locked = run(doc, Json{{"op", "set_layer"}, {"page", 1}, {"id", "paint"}, {"lock_position", true}});
        QVERIFY(layer_of(locked, "paint").lock_position);
        QVERIFY(refusal(locked, Json{{"op", "transform_layers"}, {"page", 1}, {"ids", ids({"paint"})}, {"move", Json::array({1, 0})}}).find("position") != std::string::npos);
        QVERIFY(!layer_of(run(locked, Json{{"op", "set_layers"}, {"page", 1}, {"ids", ids({"paint", "pen"})}, {"lock_position", false}}), "paint").lock_position);
    }

    // (independent review A P2-3) Auto-Select draws each layer only around the point — a few working pixels, and as far
    // as its effects reach — and answers as the whole layer drawn says: a mask, a panel's clip, an outline drawn round a
    // picture, a pen line, a shape part, a tone, a fill.
    void autoSelectDrawsOnlyAroundThePoint() {
        core::Document doc = run(book(true), Json{{"op", "set_layer"}, {"page", 1}, {"id", "paint"}, {"effect", Json{{"border", Json{{"width_mm", 1.0}}}}}});
        doc = run(doc, Json{{"op", "set_layer_mask"}, {"page", 1}, {"id", "paint"}, {"area", Json{{"rect", Json::array({0, 0, 9, 25.4})}}}});
        doc = run(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "shapes"}, {"panel_each", false}});
        doc = run(doc, Json{{"op", "add_shape_part"}, {"page", 1}, {"layer_id", "shapes"}, {"id", "r1"},
                            {"shape", Json{{"kind", "rect"}, {"rect", Json::array({2, 12, 4, 4})}}}, {"fill", Json{{"rgb", Json::array({0, 0, 0})}}}});
        doc = run(doc, Json{{"op", "add_tone"}, {"page", 1}, {"id", "tn"}, {"density", 0.5}, {"area", Json{{"rect", Json::array({12, 14, 6, 4})}}}});
        const auto whole = [&](double x, double y) {  // (each layer drawn whole, as Auto-Select drew it before)
            std::vector<std::string> out;
            const auto& layers = doc.page(0).layers;
            for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
                if (!it->visible || it->kind == core::LayerKind::Folder || it->kind == core::LayerKind::Adjust) continue;
                core::Layer shown = *it;
                shown.opacity = 1.0;
                const render::Image picture = render::layer_image(doc.page(0), shown, lb::kDpi, &doc, true);
                const auto g = lb::grid(doc.page(0));
                const int px = static_cast<int>(std::floor(x * g[0])), py = static_cast<int>(std::floor(y * g[1]));
                if (px < 0 || py < 0 || px >= picture.width() || py >= picture.height()) continue;
                if (picture.mode() != "RGBA" || picture.getpixel(px, py).back() > 0) out.push_back(it->id);
            }
            return out;
        };
        int checked = 0;
        for (double y = 0.3; y < kInch; y += 1.1) {
            for (double x = 0.2; x < kInch; x += 0.9) {
                QCOMPARE(lb::layers_at(doc, doc.page(0), x, y), whole(x, y));
                ++checked;
            }
        }
        QVERIFY(checked > 500);
        // (each layer a few pixels round the point, never the page: the outline's reach at most)
        const std::uint64_t before = lb::pixels_drawn();
        (void)lb::layers_at(doc, doc.page(0), 6, 9);
        const std::uint64_t drawn = lb::pixels_drawn() - before;
        QVERIFY2(drawn <= std::uint64_t(doc.page(0).layers.size()) * 80 * 80, qPrintable(QString::number(drawn)));
    }

    // (independent review A P3-1) The box Free Transform starts from is the box the Move tool aligns and snaps by: the
    // page's working grid, on a paper that is no whole number of 0.127 mm too.
    void theTransformBoxIsOnThePagesGrid() {
        auto doc = core::new_episode("整列", core::Num(1), 1, core::PageSpec::custom(182, 257, 160, 230, 3, 10, 10, 10, 10, 200, "color"));
        auto& p = doc.edit_page(0);
        p.frames.clear();
        p.layers.clear();
        const int w = render::mm_to_px(182, lb::kDpi), h = render::mm_to_px(257, lb::kDpi);
        std::string bytes(std::size_t(w) * h * 4, '\0');
        for (int y = h - 300; y < h - 20; ++y)
            for (int x = w - 400; x < w; ++x) bytes[(std::size_t(y) * w + x) * 4 + 3] = static_cast<char>(255);
        add_paint(doc, "edge", render::Image::frombytes("RGBA", render::Size{w, h}, bytes));
        const lb::Box moved = *lb::of_layer(doc, doc.page(0), "edge");
        const auto transformed = render::layer_transform::content_box(doc, doc.page(0), {"edge"});
        QVERIFY(transformed.has_value());
        for (std::size_t i = 0; i < 4; ++i) QVERIFY2(std::fabs((*transformed)[i] - moved[i]) < 1e-9, qPrintable(QStringLiteral("%1: %2 / %3").arg(i).arg((*transformed)[i], 0, 'g', 12).arg(moved[i], 0, 'g', 12)));
    }

    // Through the command line: saved (transform@1 named), undone; inspect --at.
    void throughTheCommandLine() {
        const QString book_path = path(QStringLiteral("book.genko"));
        int exit = -1;
        const auto made = genko::test::run_genko({QStringLiteral("new"), book_path, QStringLiteral("--pages"), QStringLiteral("1")});
        QVERIFY2(made.exit_code == 0, qPrintable(made.described()));
        Json done = apply(book_path, QStringLiteral("layers.json"),
                          Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pic"}},
                                       Json{{"op", "set_layer"}, {"page", 1}, {"id", "pic"}, {"panel_each", false}, {"panel_clip", false}},
                                       Json{{"op", "put_raster"}, {"page", 1}, {"id", "pic"}, {"png_base64", core::b64encode(render::write_png(block(40, 50, 80, 110)))}},
                                       Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "line"}, {"panel_each", false}},
                                       Json{{"op", "set_layer"}, {"page", 1}, {"id", "line"}, {"panel_clip", false}},
                                       Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", "line"}, {"points", Json::array({Json::array({30, 30}), Json::array({60, 30})})},
                                            {"width_mm", 1.0}}}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        const Json before = project(book_path);
        QVERIFY(before.value("features", Json::array()).dump().find("transform@1") == std::string::npos);
        done = apply(book_path, QStringLiteral("align.json"),
                     Json::array({Json{{"op", "link_layers"}, {"page", 1}, {"ids", ids({"pic", "line"})}},
                                  Json{{"op", "set_layer"}, {"page", 1}, {"id", "line"}, {"lock_position", true}},
                                  Json{{"op", "arrange_layers"}, {"page", 1}, {"ids", ids({"line"})}, {"to", "back"}}}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        const Json saved = project(book_path);
        QVERIFY2(saved.value("features", Json::array()).dump().find("transform@1") != std::string::npos, saved.value("features", Json::array()).dump().c_str());
        // (the line's lock refuses a move of the linked picture)
        done = apply(book_path, QStringLiteral("move.json"),
                     Json::array({Json{{"op", "align_layers"}, {"page", 1}, {"ids", ids({"pic"})}, {"edge", "left"}, {"to", "page"}}}));
        QVERIFY2(done.contains("failed"), done.dump().c_str());
        // inspect --at: the layers shown there, the topmost first
        const Json at = genko({QStringLiteral("inspect"), book_path, QStringLiteral("--at"), QStringLiteral("1:45,30")}, &exit);
        QVERIFY2(exit == 0, at.dump().c_str());
        QCOMPARE(at["page"], Json(1));
        std::vector<std::string> listed;
        for (const Json& l : at["layers"]) listed.push_back(l["id"].get<std::string>());
        QVERIFY2(std::find(listed.begin(), listed.end(), "line") != listed.end(), at.dump().c_str());
        const Json nothing = genko({QStringLiteral("inspect"), book_path, QStringLiteral("--at"), QStringLiteral("1:300,300")}, &exit);
        QVERIFY2(exit == 0, nothing.dump().c_str());
        QVERIFY2(std::none_of(nothing["layers"].begin(), nothing["layers"].end(), [](const Json& l) { return l["id"] == Json("line") || l["id"] == Json("pic"); }),
                 nothing.dump().c_str());
        genko({QStringLiteral("inspect"), book_path, QStringLiteral("--at"), QStringLiteral("one")}, &exit);
        QVERIFY(exit != 0);
        // undone: no link, no lock, the order as it was; transform@1 no longer named
        for (int i = 0; i < 1; ++i) {
            const Json undone = genko({QStringLiteral("undo"), book_path, QStringLiteral("--as"), QStringLiteral("human:test")}, &exit);
            QVERIFY2(exit == 0, undone.dump().c_str());
        }
        QCOMPARE(project(book_path)["pages"], before["pages"]);
        QCOMPARE(project(book_path).value("features", Json::array()), before.value("features", Json::array()));
    }
};

QTEST_GUILESS_MAIN(TestContractAlign)
#include "test_contract_align.moc"
