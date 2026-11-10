// PC3 C-d (SPEC.md COLOR-07「移動・整列」): the arithmetic of Layer › Align, Distribute, Arrange and Link Layers and of View ›
// Snap, as PhotoCraft v0.2.0's own tests check them — crates/engine/src/layer_multi_cmds.rs (795-1006): each edge aligned
// (a half pixel up or left), one layer to a selection or the paper, three uneven layers distributed by each edge and by
// equal gaps (the outermost two stay), linked layers going together, Reverse; commands.rs' arrange; snap.rs (271-347): a
// point snaps within the reach only and the reach is the screen's over the zoom, a guide wins a tie, the grid's nearest
// line, a moving box snaps by its nearest edge or middle, the smart guides' lines reach over both boxes. And this build's
// own: the stacking order among siblings with folders (a folder moves with its layers, a layer in a folder stays in it),
// several layers forward together, Reverse across folders, a line of dialogue moved exactly while a layer moves by whole
// working pixels.

#include <QtTest>

#include <cmath>

#include "core/model.hpp"
#include "core/snap.hpp"
#include "render/layer_align.hpp"

using namespace genko;
using core::Json;
namespace la = render::layer_align;
namespace sn = core::snap;

namespace {

constexpr double kMm = 25.4 / 200;  // (a working pixel, mm)

// A box given in working pixels (x0, y0, x1, y1), as a layer's item.
la::Item px(double x0, double y0, double x1, double y1) { return la::Item{la::Box{x0 * kMm, y0 * kMm, x1 * kMm, y1 * kMm}, true}; }

// The box after the move, back in working pixels.
std::array<long, 4> after(const la::Item& item, const std::array<double, 2>& d) {
    return {std::lround((item.box[0] + d[0]) / kMm), std::lround((item.box[1] + d[1]) / kMm), std::lround((item.box[2] + d[0]) / kMm),
            std::lround((item.box[3] + d[1]) / kMm)};
}

la::Box united(const std::vector<la::Item>& items) {
    std::vector<sn::Box> boxes;
    for (const la::Item& i : items) boxes.push_back(i.box);
    return *sn::united(boxes);
}

core::Layer layer(const std::string& id, const std::string& parent = {}, core::LayerKind kind = core::LayerKind::Strokes) {
    core::Layer l;
    l.id = id;
    l.kind = kind;
    if (!parent.empty()) l.parent_id = parent;
    return l;
}

// bg, a, f1 and f2 (in the folder F), F, c — the page's order, the bottom first.
std::vector<core::Layer> stack() {
    return {layer("bg"), layer("a"), layer("f1", "F"), layer("f2", "F"), layer("F", {}, core::LayerKind::Folder), layer("c")};
}

std::vector<std::string> order(const std::vector<core::Layer>& layers) {
    std::vector<std::string> out;
    for (const core::Layer& l : layers) out.push_back(l.id);
    return out;
}

std::string parent_in(const std::vector<core::Layer>& layers, const std::string& id) {
    for (const core::Layer& l : layers)
        if (l.id == id) return l.parent_id.is_string() ? l.parent_id.get<std::string>() : std::string();
    return "?";
}

sn::Targets targets() {
    sn::Targets t;
    t.extent = {0, 0, 400, 300};
    t.add_box({0, 0, 400, 300}, sn::Kind::PaperEdge, sn::Kind::PaperMiddle);
    t.x.push_back(sn::Target{120, sn::Kind::Guide, {0, 300}});
    return t;
}

}  // namespace

class TestAlign : public QObject {
    Q_OBJECT
private slots:
    // layer_multi_cmds.rs align_each_edge: two layers, each edge to the edge of both together.
    void alignEachEdge() {
        struct Case {
            const char* edge;
            bool wide;  // (the horizontal cases: b wide, so it makes the target)
            std::array<long, 4> a, b;
        };
        const std::vector<Case> cases{
            {"top", false, {10, 5, 20, 15}, {40, 5, 50, 25}},
            {"bottom", false, {10, 20, 20, 30}, {40, 10, 50, 30}},
            // (5..30: the middle 17.5; a half pixel goes up)
            {"vcenter", false, {10, 12, 20, 22}, {40, 7, 50, 27}},
            {"left", true, {10, 5, 20, 15}, {10, 10, 50, 30}},
            {"right", true, {40, 5, 50, 15}, {10, 10, 50, 30}},
            {"hcenter", true, {25, 5, 35, 15}, {10, 10, 50, 30}},
        };
        for (const Case& c : cases) {
            const std::vector<la::Item> items{px(10, 5, 20, 15), c.wide ? px(10, 10, 50, 30) : px(40, 10, 50, 30)};
            const auto moves = la::align_moves(items, *la::edge_from(c.edge), united(items));
            QCOMPARE(moves.size(), std::size_t(2));
            QCOMPARE(after(items[0], moves[0]), c.a);
            QCOMPARE(after(items[1], moves[1]), c.b);
            // (by whole working pixels)
            for (const auto& m : moves)
                for (const double v : m) QVERIFY2(std::fabs(v / kMm - std::round(v / kMm)) < 1e-6, c.edge);
        }
        QVERIFY(!la::edge_from("topEdges"));
    }

    // align_single_layer_to_selection / align_single_layer_to_canvas: one layer to a box (the selection's, the paper's).
    void alignOneToABox() {
        const std::vector<la::Item> one{px(10, 10, 20, 20)};
        const la::Box selection{50 * kMm, 40 * kMm, 80 * kMm, 70 * kMm};
        QCOMPARE(after(one[0], la::align_moves(one, la::Edge::Right, selection)[0]), (std::array<long, 4>{70, 10, 80, 20}));
        QCOMPARE(after(one[0], la::align_moves(one, la::Edge::VMiddle, selection)[0]), (std::array<long, 4>{10, 50, 20, 60}));
        const la::Box paper{0, 0, 100 * kMm, 80 * kMm};
        QCOMPARE(after(one[0], la::align_moves(one, la::Edge::HMiddle, paper)[0]), (std::array<long, 4>{45, 10, 55, 20}));
        QCOMPARE(after(one[0], la::align_moves(one, la::Edge::VMiddle, paper)[0]), (std::array<long, 4>{10, 35, 20, 45}));
        QCOMPARE(after(one[0], la::align_moves(one, la::Edge::Left, paper)[0]), (std::array<long, 4>{0, 10, 10, 20}));
        // (a paper of a width not whole in working pixels: the layer by whole pixels, half up or left)
        const la::Box odd{0, 0, 25.0, 25.0};  // (196.85 working pixels)
        const auto m = la::align_moves(one, la::Edge::HMiddle, odd)[0];
        QVERIFY(std::fabs(m[0] / kMm - std::round(m[0] / kMm)) < 1e-6);
        QCOMPARE(std::lround(m[0] / kMm), 83L);  // (middle 98.43 − 15 = 83.43: 83)
    }

    // A line of dialogue moves exactly (its box is not on the working grid), a layer by whole pixels, in one go.
    void aLineMovesExactly() {
        const std::vector<la::Item> items{px(10, 10, 20, 20), la::Item{la::Box{5.0, 3.3, 15.0, 8.0}, false}};
        const auto up = la::align_moves(items, la::Edge::Top, united(items));
        QCOMPARE(up[0], (std::array<double, 2>{0, 0}));  // (the layer is the top already)
        QVERIFY(std::fabs(up[1][1] - (10 * kMm - 3.3)) < 1e-9);  // (the line's top exactly on the layer's)
        const auto down = la::align_moves(items, la::Edge::Bottom, united(items));
        QCOMPARE(down[1], (std::array<double, 2>{0, 0}));
        QCOMPARE(after(items[0], down[0])[3], 63L);  // (8 mm is 62.99 working pixels: the layer's bottom on the pixel nearest)
        QCOMPARE(after(items[0], down[0])[1] - after(items[0], down[0])[3], -10L);
    }

    // distribute_uneven_layers: three uneven layers by each edge and by equal gaps; the outermost two stay.
    void distributeUnevenLayers() {
        const std::vector<la::Item> across{px(0, 0, 10, 10), px(12, 0, 32, 10), px(70, 0, 100, 10)};
        for (const char* by : {"left", "hcenter", "right"}) {
            const auto m = la::distribute_moves(across, by);
            QCOMPARE(after(across[1], m[1]), (std::array<long, 4>{35, 0, 55, 10}));
            QCOMPARE(after(across[0], m[0]), (std::array<long, 4>{0, 0, 10, 10}));
            QCOMPARE(after(across[2], m[2]), (std::array<long, 4>{70, 0, 100, 10}));
        }
        {  // (span 100, widths 60: gaps of 20)
            const auto m = la::distribute_moves(across, "horizontal");
            QCOMPARE(after(across[1], m[1]), (std::array<long, 4>{30, 0, 50, 10}));
            QCOMPARE(after(across[0], m[0]), (std::array<long, 4>{0, 0, 10, 10}));
            QCOMPARE(after(across[2], m[2]), (std::array<long, 4>{70, 0, 100, 10}));
        }
        const std::vector<la::Item> down{px(0, 0, 10, 10), px(0, 50, 10, 54), px(0, 60, 10, 80)};
        QCOMPARE(after(down[1], la::distribute_moves(down, "vertical")[1]), (std::array<long, 4>{0, 33, 10, 37}));  // (gaps of 23)
        QCOMPARE(after(down[1], la::distribute_moves(down, "top")[1]), (std::array<long, 4>{0, 30, 10, 34}));
        QCOMPARE(after(down[1], la::distribute_moves(down, "bottom")[1]), (std::array<long, 4>{0, 41, 10, 45}));
        const long y0 = after(down[1], la::distribute_moves(down, "vcenter")[1])[1];  // (middles 5 and 70: 37.5)
        QVERIFY2(y0 == 35 || y0 == 36, qPrintable(QString::number(y0)));
        // (the items in any order: the same moves for each)
        const std::vector<la::Item> shuffled{down[2], down[0], down[1]};
        QCOMPARE(after(shuffled[2], la::distribute_moves(shuffled, "vertical")[2]), (std::array<long, 4>{0, 33, 10, 37}));
        // (two: nothing moves)
        for (const auto& m : la::distribute_moves({down[0], down[1]}, "top")) QCOMPARE(m, (std::array<double, 2>{0, 0}));
    }

    // The stacking order among siblings: a folder with its layers, a layer in a folder kept in it.
    void arrangeAmongSiblings() {
        const auto run = [](std::vector<std::string> ids, const char* to) {
            std::vector<core::Layer> layers = stack();
            const bool moved = la::arrange(layers, ids, to);
            return std::make_pair(moved, layers);
        };
        using V = std::vector<std::string>;
        auto [moved, layers] = run({"a"}, "forward");  // (past the folder and its layers)
        QVERIFY(moved);
        QCOMPARE(order(layers), (V{"bg", "f1", "f2", "F", "a", "c"}));
        std::tie(moved, layers) = run({"c"}, "backward");  // (under the folder's layers)
        QCOMPARE(order(layers), (V{"bg", "a", "c", "f1", "f2", "F"}));
        std::tie(moved, layers) = run({"F"}, "forward");  // (the folder with its layers)
        QCOMPARE(order(layers), (V{"bg", "a", "c", "f1", "f2", "F"}));
        QCOMPARE(parent_in(layers, "f1"), std::string("F"));
        std::tie(moved, layers) = run({"f1"}, "front");  // (the front of its folder)
        QCOMPARE(order(layers), (V{"bg", "a", "f2", "f1", "F", "c"}));
        QCOMPARE(parent_in(layers, "f1"), std::string("F"));
        std::tie(moved, layers) = run({"a"}, "back");
        QCOMPARE(order(layers), (V{"a", "bg", "f1", "f2", "F", "c"}));
        std::tie(moved, layers) = run({"c"}, "front");  // (the front already: nothing)
        QVERIFY(!moved);
        QCOMPARE(order(layers), order(stack()));
        std::tie(moved, layers) = run({"f2"}, "forward");  // (the front of its folder already)
        QVERIFY(!moved);
        // several forward together: the one at the front stays, the other comes up one
        std::tie(moved, layers) = run({"a", "c"}, "forward");
        QCOMPARE(order(layers), (V{"bg", "f1", "f2", "F", "a", "c"}));
        std::tie(moved, layers) = run({"bg", "a"}, "forward");
        QCOMPARE(order(layers), (V{"f1", "f2", "F", "bg", "a", "c"}));
        std::tie(moved, layers) = run({"bg", "c"}, "front");  // (their order kept at the front)
        QCOMPARE(order(layers), (V{"a", "f1", "f2", "F", "bg", "c"}));
        std::tie(moved, layers) = run({"a", "c"}, "back");
        QCOMPARE(order(layers), (V{"a", "c", "bg", "f1", "f2", "F"}));
        // Reverse: their places end for end (duplicate_hide_lock_and_reverse_selected), across folders too
        std::tie(moved, layers) = run({"a", "c"}, "reverse");
        QCOMPARE(order(layers), (V{"bg", "c", "f1", "f2", "F", "a"}));
        std::tie(moved, layers) = run({"bg", "a", "c"}, "reverse");
        QCOMPARE(order(layers), (V{"c", "a", "f1", "f2", "F", "bg"}));
        std::tie(moved, layers) = run({"f1", "c"}, "reverse");
        QCOMPARE(order(layers), (V{"bg", "a", "c", "f2", "F", "f1"}));
        QCOMPARE(parent_in(layers, "c"), std::string("F"));
        QCOMPARE(parent_in(layers, "f1"), std::string());
        std::tie(moved, layers) = run({"a", "F"}, "reverse");  // (a folder's place taken with its layers)
        QCOMPARE(order(layers), (V{"bg", "f1", "f2", "F", "a", "c"}));
        QCOMPARE(parent_in(layers, "f2"), std::string("F"));
        QVERIFY_THROWS_EXCEPTION(core::OpError, la::arrange(layers, {"a"}, "sideways"));
        // (a folder whose layers lie over it in the page's order, as add_layer puts them: the same)
        const std::vector<core::Layer> over{layer("bg"), layer("F", {}, core::LayerKind::Folder), layer("f1", "F"), layer("f2", "F"), layer("c")};
        std::vector<core::Layer> l2 = over;
        QVERIFY(la::arrange(l2, {"bg"}, "forward"));
        QCOMPARE(order(l2), (V{"F", "f1", "f2", "bg", "c"}));
        l2 = over;
        QVERIFY(la::arrange(l2, {"c"}, "backward"));
        QCOMPARE(order(l2), (V{"bg", "c", "F", "f1", "f2"}));
        l2 = over;
        QVERIFY(la::arrange(l2, {"F"}, "front"));
        QCOMPARE(order(l2), (V{"bg", "c", "F", "f1", "f2"}));
        l2 = over;
        QVERIFY(la::arrange(l2, {"F"}, "back"));
        QCOMPARE(order(l2), (V{"F", "f1", "f2", "bg", "c"}));
        QVERIFY(!la::arrange(l2, {"F"}, "back"));
    }

    // Links (linked_layers_move_together_and_undo): the layers linked to one go with it; Link Layers toggles.
    void linksAndTopLevel() {
        core::Page page;
        page.layers = stack();
        page.layers[1].link = "link-1";  // a
        page.layers[5].link = "link-1";  // c
        using V = std::vector<std::string>;
        QCOMPARE(la::with_links(page, {"a"}), (V{"a", "c"}));
        QCOMPARE(la::with_links(page, {"bg"}), (V{"bg"}));
        QVERIFY(la::unlinks(page, {"a", "c"}));
        QVERIFY(la::unlinks(page, {"c"}));
        QVERIFY(!la::unlinks(page, {"a", "bg"}));
        QVERIFY(!la::unlinks(page, {"bg"}));
        // (a layer in a folder named with it: the folder carries it)
        QCOMPARE(la::top_level(page, {"f1", "F", "a"}), (V{"a", "F"}));
        QCOMPARE(la::top_level(page, {"f1", "f2"}), (V{"f1", "f2"}));
        QVERIFY_THROWS_EXCEPTION(core::OpKeyError, la::top_level(page, {"nope"}));
    }

    // snap.rs point_snaps_within_threshold_only, threshold_scales_with_zoom, guides_win_ties, grid_snaps_to_nearest_line.
    void snapAPoint() {
        const sn::Targets t = targets();
        auto [p, lines] = t.point({123.0, 151.0}, 4.0);
        QCOMPARE(p, (std::array<double, 2>{120.0, 150.0}));
        QCOMPARE(lines.size(), std::size_t(2));
        QVERIFY(lines[0].kind == sn::Kind::Guide && lines[0].vertical);
        QVERIFY(lines[1].kind == sn::Kind::PaperMiddle && !lines[1].vertical);
        std::tie(p, lines) = t.point({130.0, 160.0}, 4.0);
        QCOMPARE(p, (std::array<double, 2>{130.0, 160.0}));
        QVERIFY(lines.empty());
        // (8 screen px: at 400% 2, at 25% 32)
        QVERIFY(!t.nearest(true, 125.0, 8.0 / 4.0));
        QCOMPARE(t.nearest(true, 145.0, 8.0 / 0.25)->pos, 120.0);
        sn::Targets tie = targets();
        tie.x.push_back(sn::Target{120, sn::Kind::LayerEdge, {0, 10}});
        QVERIFY(tie.nearest(true, 121.0, 5.0)->kind == sn::Kind::Guide);
        sn::Targets grid;
        grid.grid = 25.0;
        grid.extent = {0, 0, 100, 100};
        QCOMPARE(grid.point({48.0, 61.0}, 3.0).first, (std::array<double, 2>{50.0, 61.0}));
        QVERIFY(grid.nearest(false, 74.0, 3.0)->kind == sn::Kind::Grid);
        QVERIFY(sn::Targets{}.empty() && !t.empty());
    }

    // snap.rs rect_snaps_by_closest_anchor, smart_guide_lines_span_both_objects; union.
    void snapAMovingBox() {
        const sn::Targets t = targets();
        auto [d, lines] = t.box({68.0, 10.0, 118.0, 40.0}, 4.0);  // (its right edge 2 left of the guide)
        QCOMPARE(d[0], 2.0);
        QVERIFY(std::any_of(lines.begin(), lines.end(), [](const sn::Line& l) { return l.vertical && l.pos == 120.0; }));
        std::tie(d, lines) = t.box({148.0, 102.0, 248.0, 202.0}, 4.0);  // (its middle near the paper's)
        QCOMPARE(d, (std::array<double, 2>{2.0, -2.0}));
        QVERIFY(std::any_of(lines.begin(), lines.end(), [](const sn::Line& l) { return l.kind == sn::Kind::PaperMiddle && l.vertical; }));
        QVERIFY(std::any_of(lines.begin(), lines.end(), [](const sn::Line& l) { return l.kind == sn::Kind::PaperMiddle && !l.vertical; }));
        sn::Targets layers;
        layers.add_box({300, 200, 350, 260}, sn::Kind::LayerEdge, sn::Kind::LayerMiddle);
        std::tie(d, lines) = layers.box({302, 10, 330, 40}, 5.0);
        QCOMPARE(d[0], -2.0);
        const auto at = std::find_if(lines.begin(), lines.end(), [](const sn::Line& l) { return l.vertical && l.pos == 300.0; });
        QVERIFY(at != lines.end());
        QCOMPARE(std::make_pair(at->from, at->to), std::make_pair(10.0, 260.0));
        QVERIFY(sn::is_smart(at->kind) && !sn::is_smart(sn::Kind::Guide));
        QCOMPARE(*sn::united({{0, 0, 1, 1}, {2, -1, 3, 0.5}}), (sn::Box{0, -1, 3, 1}));
        QVERIFY(!sn::united({}));
    }
};

QTEST_GUILESS_MAIN(TestAlign)
#include "test_align.moc"
