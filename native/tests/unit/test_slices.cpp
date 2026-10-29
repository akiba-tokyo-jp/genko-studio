// PC5 J-b (slices@1, SPEC.md PROD-01「スライス」): a page's slices as PhotoCraft v0.2.0's own tests check them —
// crates/doc/src/slices.rs (the auto slices tile the rest of the canvas, Photoshop's numbering and names),
// crates/engine/src/slice_cmds/tests.rs (user slices and their options, an auto slice promoted and a slice divided, the
// slices from the guides and clearing them, a layer-based slice following its layer and its effects and going with it),
// crates/psd/src/slices.rs and crates/io/src/slices_map.rs (1050 version 6 with the outsets, version 7, broken ones) — on
// Genko's pages: rects in mm, their pixels at the page's resolution (a 254 dpi page: 10 pixels a mm), each edge rounded
// alone (neighbours share it at any resolution), carried by the page size ops, a duplicated page's layer-based slice
// naming its copy's layer, the question slices_info answered without a change, a .pcraft's slices, MCP's apply_ops not
// taking the ops.

#include <QtTest>

#include <algorithm>
#include <cmath>
#include <string>

#include "core/command_bus.hpp"
#include "core/json.hpp"
#include "core/model.hpp"
#include "core/production_json.hpp"
#include "core/slice_layout.hpp"
#include "render/image.hpp"
#include "render/ops_production.hpp"
#include "render/ops_registry.hpp"
#include "render/ops_slices.hpp"
#include "render/pcraft_production.hpp"
#include "render/pcraft_slices.hpp"
#include "render/psd_descriptor.hpp"
#include "render/psd_production.hpp"
#include "render/psd_slices.hpp"
#include "render/raster.hpp"
#include "service/studio_service.hpp"

using namespace genko;
using core::Json;
namespace ps = render::psd_slices;

namespace {

core::ApplyResult run_ops(const core::Document& doc, const Json& ops, const char* actor = "human:test") {
    return core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor(actor));
}
core::Document run(const core::Document& doc, const Json& op) { return run_ops(doc, Json::array({op})).doc; }
Json report(const core::Document& doc, const Json& op) {
    const auto applied = run_ops(doc, Json::array({op}));
    return applied.results.empty() ? Json() : applied.results[0];
}
std::string refusal(const core::Document& doc, const Json& op) {
    try {
        run_ops(doc, Json::array({op}));
    } catch (const std::exception& error) {
        return error.what();
    }
    return {};
}

Json op(const char* name, Json more = Json::object()) {
    Json out{{"op", name}, {"page", 1}};
    for (auto& [k, v] : more.items()) out[k] = v;
    return out;
}

// A 12 × 9 mm page at 254 dpi (10 pixels a mm: 120 × 90, PhotoCraft's test canvas) without panels.
core::Document book(double w = 12, double h = 9) {
    auto doc = core::new_episode("スライス", core::Num(1), 1, core::PageSpec::custom(w, h, w - 2, h - 2, 1, 1, 1, 1, 1, 254, "color"));
    auto& p = doc.edit_page(0);
    p.numero = false;
    p.frames.clear();
    p.layers.clear();
    return doc;
}

// A paint layer "a": a block over x 1..3 mm, y 1..2 mm (drawn at the working 200 dpi, its edges on that grid).
void add_block(core::Document& doc) {
    auto& p = doc.edit_page(0);
    const int w = static_cast<int>(std::lround(p.spec.width_mm.value() / 25.4 * 200)), h = static_cast<int>(std::lround(p.spec.height_mm.value() / 25.4 * 200));
    std::string bytes(std::size_t(w) * std::size_t(h) * 4, '\0');
    const auto px = [](double mm) { return static_cast<int>(std::lround(mm / 25.4 * 200)); };
    for (int y = px(1); y < px(2); ++y)
        for (int x = px(1); x < px(3); ++x) {
            const std::size_t i = (std::size_t(y) * std::size_t(w) + std::size_t(x)) * 4;
            bytes[i] = static_cast<char>(200);
            bytes[i + 3] = static_cast<char>(255);
        }
    core::Layer layer;
    layer.id = "a";
    layer.kind = core::LayerKind::Raster;
    layer.panel_clip = false;
    render::raster::save_raster(p, layer, render::Image::frombytes("RGBA", render::Size{w, h}, bytes));
    p.layers.push_back(std::move(layer));
}

Json listed(const core::Document& doc) { return report(doc, op("slices_info")); }

const Json* by_id(const Json& info, std::int64_t id) {
    for (const Json& s : info["slices"])
        if (s["id"] == Json(id)) return &s;
    return nullptr;
}

std::int64_t area(const std::vector<core::SliceBox>& boxes) {
    std::int64_t a = 0;
    for (const auto& b : boxes) a += b.width() * b.height();
    return a;
}

void guide(core::Page& page, const char* axis, double at) {
    page.rulers.push_back(Json{{"id", std::string("g") + axis + std::to_string(at)}, {"kind", "guide"}, {"points", Json::array()}, {"active", true},
                               {"visible", true}, {"axis", axis}, {"at", at}});
}

}  // namespace

class TestSlices : public QObject {
    Q_OBJECT

private slots:
    // doc/src/slices.rs auto_slices_tile_the_rest_of_the_canvas.
    void autoSlicesTileTheCanvas() {
        const core::SliceBox canvas{0, 0, 100, 80};
        const std::vector<core::SliceBox> user{{20, 10, 60, 30}};
        const auto made = core::auto_slices(canvas, user);
        QCOMPARE(area(made) + area(user), std::int64_t(100 * 80));
        for (std::size_t i = 0; i < made.size(); ++i) {
            QVERIFY(made[i].intersect(user[0]).empty());
            for (std::size_t j = i + 1; j < made.size(); ++j) QVERIFY(made[i].intersect(made[j]).empty());
        }
        QCOMPARE(made.size(), std::size_t(4));  // (above, left, right, below)
        QCOMPARE(made[0], (core::SliceBox{0, 0, 100, 10}));
        QCOMPARE(core::auto_slices(canvas, {}), (std::vector<core::SliceBox>{canvas}));
    }

    // doc/src/slices.rs resolve_numbers_top_to_bottom: one auto slice named <base> alone, then <base>_NN; the user's own
    // name kept.
    void numberedTopToBottom() {
        core::Slices none;
        auto alone = core::resolve_slices(none, {}, core::SliceBox{0, 0, 100, 80}, "site");
        QCOMPARE(alone.size(), std::size_t(1));
        QCOMPARE(alone[0].name, std::string("site"));
        core::Slices one;
        core::Slice s;
        s.id = 1;
        one.list.push_back(s);
        one.next_id = 2;
        auto all = core::resolve_slices(one, {{1, core::SliceBox{20, 10, 60, 30}}}, core::SliceBox{0, 0, 100, 80}, "site");
        QCOMPARE(all.size(), std::size_t(5));
        QCOMPARE(all[0].name, std::string("site_01"));
        const auto user = std::find_if(all.begin(), all.end(), [](const core::ResolvedSlice& r) { return r.id == 1; });
        QCOMPARE(user->number, 3);  // (after the top band and the left cell)
        QCOMPARE(user->origin, std::string("user"));
        one.list[0].name = "logo";
        all = core::resolve_slices(one, {{1, core::SliceBox{20, 10, 60, 30}}}, core::SliceBox{0, 0, 100, 80}, "site");
        QVERIFY(std::any_of(all.begin(), all.end(), [](const core::ResolvedSlice& r) { return r.name == "logo"; }));
        // (a file name: letters of any script kept, the rest '_')
        QCOMPARE(core::slice_file_name("表紙 1/2.png", "x"), std::string("表紙_1_2_png"));
        QCOMPARE(core::slice_file_name("", "slice"), std::string("slice"));
    }

    // Each edge its mm rounded alone: two slices sharing an edge share its pixel at any resolution (no gap, no overlap), and
    // the auto slices fill the rest exactly.
    void neighboursShareTheirEdges() {
        for (const double dpi : {72.0, 96.0, 254.0, 350.0, 600.0}) {
            const double k = dpi / 25.4;
            const core::SliceGrid g = core::slice_grid(12.3, 9.7, std::llround(12.3 * k), std::llround(9.7 * k));
            const auto a = core::slice_box({0.37, 1.13, 3.333, 2.2}, g);
            const auto b = core::slice_box({3.703, 1.13, 4.0, 2.2}, g);
            QCOMPARE(a.x1, b.x0);
            QCOMPARE(a.y0, b.y0);
            const auto made = core::auto_slices(g.canvas, {a, b});
            QCOMPARE(area(made) + area({a, b}), g.canvas.width() * g.canvas.height());
        }
    }

    // slice_cmds/tests.rs user_slices_and_options: a user slice and its options by number, the refusals, undone in one step.
    void userSlicesAndOptions() {
        const core::Document doc = book();
        const Json made = report(doc, op("add_slice", {{"rect_px", {20, 10, 40, 20}}, {"name", "logo"}, {"url", "https://example.org"}, {"alt", "Logo"}}));
        QCOMPARE(made["number"], Json(3));
        const core::Document one = run(doc, op("add_slice", {{"rect_px", {20, 10, 40, 20}}, {"name", "logo"}, {"url", "https://example.org"}, {"alt", "Logo"}}));
        const Json info = listed(one);
        QCOMPARE(info["slices"].size(), std::size_t(5));
        QCOMPARE(info["width_px"], Json(120));
        const Json* me = by_id(info, made["slice"].get<std::int64_t>());
        QVERIFY(me != nullptr);
        QCOMPARE((*me)["name"], Json("logo"));
        QCOMPARE((*me)["rect_px"], Json::array({20, 10, 40, 20}));
        QCOMPARE((*me)["rect_mm"], Json::array({2.0, 1.0, 4.0, 2.0}));
        QCOMPARE((*me)["origin"], Json("user"));
        QCOMPARE(static_cast<int>(std::count_if(info["slices"].begin(), info["slices"].end(), [](const Json& s) { return s["origin"] == Json("auto"); })), 4);
        QCOMPARE(info["slices"][0]["name"], Json("p001_01"));  // (the page's file name as the base)
        const core::Document set = run(one, op("set_slice", {{"number", 3}, {"kind", "noImage"}, {"cell_text", "hi"}, {"background", "#FF0000"}}));
        QCOMPARE(set.page(0).slices->list[0].kind, std::string("noImage"));
        QCOMPARE(*set.page(0).slices->list[0].background, std::string("#ff0000"));
        QCOMPARE(set.page(0).slices->list.size(), std::size_t(1));
        QVERIFY(refusal(one, op("set_slice", {{"slice", made["slice"]}, {"kind", "bogus"}})).find("kind is image, noImage or table") != std::string::npos);
        QVERIFY(refusal(doc, op("add_slice", {{"rect_px", {500, 500, 10, 10}}})).find("outside the page") != std::string::npos);
        QVERIFY(refusal(doc, op("add_slice", {{"rect_mm", {1, 1, 0, 2}}})).find("rect_mm is [x, y, width, height]") != std::string::npos);
        QVERIFY(refusal(doc, op("add_slice", {{"rect_px", {1, 1, 5, 5}}, {"outsets", {1, 1, 1, 1}}})).find("outsets") != std::string::npos);
        // (cut to the paper)
        const core::Document cut = run(doc, op("add_slice", {{"rect_mm", {-2, 8, 5, 5}}}));
        QCOMPARE(cut.page(0).slices->list[0].rect_mm, (std::array<double, 4>{0, 8, 3, 1}));
        // (the slices_info question changes nothing)
        const auto asked = run_ops(one, Json::array({op("slices_info")}));
        QVERIFY(asked.journal_ops.empty());
    }

    // slice_cmds/tests.rs promote_auto_slice_and_divide.
    void promoteAndDivide() {
        core::Document doc = run(book(), op("add_slice", {{"rect_px", {0, 0, 60, 45}}}));
        const Json r = report(doc, op("set_slice", {{"number", 2}, {"name", "right"}}));
        QCOMPARE(r["origin"], Json("user"));
        QCOMPARE(r["name"], Json("right"));
        doc = run(doc, op("set_slice", {{"number", 2}, {"name", "right"}}));
        QCOMPARE(doc.page(0).slices->list.size(), std::size_t(2));
        QCOMPARE(doc.page(0).slices->list[1].rect_mm, (std::array<double, 4>{6, 0, 6, 4.5}));
        const std::int64_t id = doc.page(0).slices->list[0].id;
        const Json divided = report(doc, op("divide_slice", {{"slice", id}, {"rows", 3}, {"columns", 2}}));
        QCOMPARE(divided["count"], Json(6));
        doc = run(doc, op("divide_slice", {{"slice", id}, {"rows", 3}, {"columns", 2}}));
        QCOMPARE(doc.page(0).slices->list.size(), std::size_t(7));
        QCOMPARE(doc.page(0).slices->list[0].id, id);  // (the first part keeps its id)
        std::vector<core::SliceBox> parts;
        const core::SliceGrid g = render::slices::page_grid(doc.page(0));
        for (std::size_t i = 0; i < 6; ++i) parts.push_back(core::slice_box(doc.page(0).slices->list[i].rect_mm, g));
        QCOMPARE(area(parts), std::int64_t(60 * 45));
        QVERIFY(refusal(doc, op("divide_slice", {{"slice", id}})).find("above 1") != std::string::npos);
        QVERIFY(refusal(doc, op("divide_slice", {{"slice", id}, {"columns", 1000}})).find("too small") != std::string::npos);
        // ユーザー定義スライスに変換 of an auto slice alone (promote)
        const core::Document promoted = run(run(book(), op("add_slice", {{"rect_px", {0, 0, 60, 45}}})), op("set_slice", {{"number", 2}, {"promote", true}}));
        QCOMPARE(promoted.page(0).slices->list.size(), std::size_t(2));
    }

    // slice_cmds/tests.rs slices_from_guides_and_clear: every slice replaced by the guides' cells; スライスを消去.
    void fromGuidesAndClear() {
        core::Document doc = run(book(), op("add_slice", {{"rect_px", {1, 1, 5, 5}}}));
        auto& p = doc.edit_page(0);
        guide(p, "v", 4);
        guide(p, "v", 8);
        guide(p, "h", 3);
        guide(p, "h", 30);  // (outside the paper: not a cut)
        const Json made = report(doc, op("slices_from_guides"));
        QCOMPARE(made["slices"], Json(6));
        doc = run(doc, op("slices_from_guides"));
        QCOMPARE(doc.page(0).slices->list.size(), std::size_t(6));
        const core::SliceGrid g = render::slices::page_grid(doc.page(0));
        bool found = false;
        for (const core::Slice& s : doc.page(0).slices->list) found = found || core::slice_box(s.rect_mm, g) == core::SliceBox{40, 30, 80, 90};
        QVERIFY(found);
        QVERIFY(std::none_of(doc.page(0).slices->list.begin(), doc.page(0).slices->list.end(), [](const core::Slice& s) { return s.id == 1; }));
        QCOMPARE(report(doc, op("delete_slices", {{"slices", "all"}}))["deleted"], Json(6));
        const core::Document cleared = run(doc, op("delete_slices", {{"slices", "all"}}));
        // (the numbering kept: next_id written with the page — J-0's P3-2)
        QVERIFY(cleared.page(0).slices && cleared.page(0).slices->list.empty());
        QVERIFY(refusal(cleared, op("delete_slices", {{"slices", "all"}})).find("no user or layer-based slices") != std::string::npos);
        QVERIFY(refusal(book(), op("slices_from_guides")).find("no guides inside its paper") != std::string::npos);
        // (one by its id)
        const std::int64_t first = doc.page(0).slices->list[0].id;
        QCOMPARE(run(doc, op("delete_slices", {{"slices", {first}}})).page(0).slices->list.size(), std::size_t(5));
        QVERIFY(refusal(doc, op("delete_slices", {{"slices", {999}}})).find("no slice 999") != std::string::npos);
    }

    // slice_cmds/tests.rs layer_based_slice_follows_the_layer_and_its_effects: its rect the layer's box (its effects too)
    // and the outsets, made again as the layer moves; one a layer; the layer deleted takes its slice.
    void layerBasedFollowsTheLayer() {
        core::Document doc = book();
        add_block(doc);
        const Json made = report(doc, op("add_slice", {{"layer", "a"}}));
        // (the layer's box on the working 200 dpi grid — about 1..3 × 1..2 mm —, in the page's pixels)
        const std::array<double, 4> box = render::slices::layer_rect_mm(doc, doc.page(0), [] {
            core::Slice s;
            s.origin = "layer";
            s.layer = "a";
            return s;
        }()).value();
        QCOMPARE(made["rect_mm"], core::production::to_json(box));
        QVERIFY(std::fabs(box[0] - 1) < 0.1 && std::fabs(box[1] - 1) < 0.1 && std::fabs(box[2] - 2) < 0.15 && std::fabs(box[3] - 1) < 0.15);
        doc = run(doc, op("add_slice", {{"layer", "a"}}));
        QVERIFY(refusal(doc, op("add_slice", {{"layer", "a"}})).find("has a layer-based slice already") != std::string::npos);
        const std::int64_t id = doc.page(0).slices->list[0].id;
        const Json at = (*by_id(listed(doc), id))["rect_px"];
        // the layer moved 2 mm right and 1 mm down: the slice follows
        const core::Document moved = run(doc, op("transform_layers", {{"ids", {"a"}}, {"matrix", {1, 0, 0, 1, 2, 1}}, {"box", {0, 0, 12, 9}}}));
        const Json moved_at = (*by_id(listed(moved), id))["rect_px"];
        QVERIFY2(std::abs(moved_at[0].get<int>() - at[0].get<int>() - 20) <= 1 && std::abs(moved_at[1].get<int>() - at[1].get<int>() - 10) <= 1,
                 moved_at.dump().c_str());
        // its outsets grow it (the page's pixels: top 1, left 2, bottom 3, right 4)
        const core::Document grown = run(doc, op("set_slice", {{"slice", id}, {"outsets", {1, 2, 3, 4}}}));
        const Json grown_at = (*by_id(listed(grown), id))["rect_px"];
        QCOMPARE(grown_at, Json::array({at[0].get<int>() - 2, at[1].get<int>() - 1, at[2].get<int>() + 6, at[3].get<int>() + 4}));
        // a drop shadow grows it too
        const core::Document styled = run(doc, op("set_layer_style", {{"id", "a"},
                                                                      {"style", Json{{"effects", Json::array({Json{{"kind", "drop_shadow"}, {"blend", "normal"}, {"opacity", 1},
                                                                                                                    {"distance_mm", 0.5}, {"size_mm", 0.5}}})}}}}));
        const Json grown_by_style = (*by_id(listed(styled), id))["rect_px"];
        QVERIFY2(grown_by_style[2].get<int>() > at[2].get<int>() && grown_by_style[3].get<int>() > at[3].get<int>(), grown_by_style.dump().c_str());
        // its rect given: a user slice where it is put
        const core::Document user = run(doc, op("set_slice", {{"slice", id}, {"rect_mm", {0, 0, 1, 1}}}));
        QCOMPARE(user.page(0).slices->list[0].origin, std::string("user"));
        QVERIFY(!user.page(0).slices->list[0].layer);
        // ユーザー定義スライスに変換: where the layer put it
        const core::Document promoted = run(moved, op("set_slice", {{"slice", id}, {"promote", true}}));
        QCOMPARE(promoted.page(0).slices->list[0].origin, std::string("user"));
        QCOMPARE((*by_id(listed(promoted), id))["rect_px"], moved_at);
        // the layer deleted: its slice too, whichever op took it
        const core::Document gone = run(doc, op("delete_layer", {{"id", "a"}}));
        QVERIFY(!gone.page(0).slices);
        // a fill or an adjustment layer: none
        core::Document filled = book();
        core::Layer fill;
        fill.id = "f";
        fill.kind = core::LayerKind::Fill;
        filled.edit_page(0).layers.push_back(fill);
        QVERIFY(refusal(filled, op("add_slice", {{"layer", "f"}})).find("no bounds") != std::string::npos);
        QVERIFY(refusal(book(), op("add_slice", {{"layer", "x"}})).find("no layer x") != std::string::npos);
    }

    // A duplicated page's layer-based slice follows its copy's layer.
    void duplicatedPageNamesItsLayer() {
        core::Document doc = book();
        add_block(doc);
        doc = run(doc, op("add_slice", {{"layer", "a"}}));
        const core::Document two = run(doc, Json{{"op", "duplicate_page"}, {"page", 1}});
        QCOMPARE(two.pages.size(), std::size_t(2));
        const core::Page& copy = two.page(1);
        QVERIFY(copy.slices);
        QCOMPARE(*copy.slices->list[0].layer, copy.layers[0].id);
        QVERIFY(copy.layers[0].id != "a");
    }

    // The page size ops carry the slices where the page goes; past 1000 m: refused.
    void carriedByThePageSizeOps() {
        const core::Document doc = run(book(), op("add_slice", {{"rect_mm", {1, 2, 3, 4}}}));
        const core::Document turned = run(doc, op("rotate_pages", {{"turn", "90cw"}}));
        // (90° clockwise on a 12 × 9 page: (x, y) → (9 − y, x); the box's corners)
        QCOMPARE(turned.page(0).slices->list[0].rect_mm, (std::array<double, 4>{3, 1, 4, 3}));
        core::Document far = book();
        core::Slices s;
        core::Slice one;
        one.rect_mm = {900000, 1, 1, 1};
        s.list.push_back(one);
        s.next_id = 2;
        far.edit_page(0).slices = s;
        QVERIFY(refusal(far, op("resize_pages", {{"width_mm", 30}})).find("slices would lie past 1000 m") != std::string::npos);
        QVERIFY(!run(book(), op("rotate_pages", {{"turn", "90cw"}})).page(0).slices);
    }

    // PSD's 1050 (psd/src/slices.rs v6_roundtrips, v7_descriptor_parses, malformed_input_is_an_error).
    void psdResource() {
        ps::FileSlices s;
        s.bounds = {0, 0, 80, 100};
        s.group = "site";
        ps::FileSlice a;
        a.id = 0;
        a.origin = 0;
        a.rect = {0, 0, 100, 10};
        ps::FileSlice b;
        b.id = 1;
        b.name = "ロゴ";
        b.rect = {20, 10, 60, 30};
        b.url = "https://example.org/";
        b.target = "_blank";
        b.message = "hi";
        b.alt = "Logo";
        b.cell_text_is_html = true;
        b.cell_text = "<b>x</b>";
        b.horizontal_align = 1;
        b.vertical_align = 2;
        b.color = {255, 10, 20, 30};
        ps::FileSlice c;
        c.id = 2;
        c.origin = 1;
        c.layer_id = 7;
        c.kind = 0;
        c.rect = {5, 40, 25, 70};
        c.outsets = {1, 2, 3, 4};
        s.slices = {a, b, c};
        const std::string bytes = ps::resource(s);
        const auto back = ps::parse(bytes);
        QVERIFY(back);
        QVERIFY(*back == s);
        QCOMPARE(ps::resource(*back), bytes);
        // (cut inside the records: none)
        for (const std::size_t cut : {std::size_t(0), std::size_t(3), std::size_t(10), std::size_t(30), std::size_t(120)}) QVERIFY(!ps::parse(bytes.substr(0, cut)));
        std::string huge("\0\0\0\x06", 4);
        huge += std::string(16, '\0');
        huge += std::string("\0\0\0\0", 4);
        huge += std::string("\xff\xff\xff\xff", 4);
        QVERIFY(!ps::parse(huge));
        QVERIFY(!ps::parse(std::string("\0\0\0\x09", 4)));
        // version 7: one descriptor
        namespace d = render::psd::descriptor;
        d::Writer bounds;
        bounds.integer("Top ", 0);
        bounds.integer("Left", 0);
        bounds.integer("Btom", 80);
        bounds.integer("Rght", 100);
        d::Writer sb;
        sb.integer("Top ", 10);
        sb.integer("Left", 20);
        sb.integer("Btom", 30);
        sb.integer("Rght", 60);
        d::Writer one;
        one.integer("sliceID", 1);
        one.enumerated("origin", "ESliceOrigin", "layerGenerated");
        one.integer("layerID", 7);
        one.text("Nm  ", u"logo");
        one.enumerated("Type", "ESliceType", "noImage");
        one.object("bounds", sb, "Rct1");
        one.text("altTag", u"Logo");
        one.text("null", u"_blank");
        one.integer("topOutset", 1);
        d::Writer top;
        top.object("bounds", bounds, "Rct1");
        top.objects("slices", {{"slice", one}});
        top.text("baseName", u"site");
        std::string v7("\0\0\0\x07", 4);
        v7 += top.versioned("null");
        const auto seven = ps::parse(v7);
        QVERIFY(seven);
        QCOMPARE(seven->version, std::uint32_t(7));
        QCOMPARE(seven->group, std::string("site"));
        QCOMPARE(seven->slices.size(), std::size_t(1));
        QCOMPARE(seven->slices[0].rect, (std::array<std::int32_t, 4>{20, 10, 60, 30}));
        QCOMPARE(seven->slices[0].origin, std::uint32_t(1));
        QCOMPARE(*seven->slices[0].layer_id, std::uint32_t(7));
        QCOMPARE(seven->slices[0].kind, std::uint32_t(0));
        QCOMPARE(seven->slices[0].alt, std::string("Logo"));
        QCOMPARE(seven->slices[0].target, std::string("_blank"));
        QCOMPARE(seven->slices[0].outsets[0], 1);
        QVERIFY(render::psd_production::copies_resource(1050));
    }

    // 1050 onto a page through the file's canvas (the file's pixels → mm), its auto slices left out, a layer-based slice whose
    // layer was made naming it (else a user slice); written back in the file's pixels with the auto ones around them.
    void psdOntoThePage() {
        ps::FileSlices s;
        s.group = "web";
        ps::FileSlice user;
        user.id = 3;
        user.rect = {20, 10, 60, 30};
        user.name = "hero";
        user.color = {255, 0x12, 0x34, 0x56};
        ps::FileSlice layered;
        layered.id = 4;
        layered.origin = 1;
        layered.layer_id = 9;
        layered.rect = {0, 50, 30, 80};
        ps::FileSlice lost;  // (a layer not made: a user slice where it was)
        lost.id = 5;
        lost.origin = 1;
        lost.layer_id = 77;
        lost.rect = {100, 50, 120, 90};
        ps::FileSlice automatic;
        automatic.id = 6;
        automatic.origin = 0;
        automatic.rect = {0, 0, 120, 10};
        s.slices = {user, layered, lost, automatic};
        core::Document doc = book();
        add_block(doc);
        core::Page& page = doc.edit_page(0);
        const core::vector::Affine onto{12, 0, 0, 9, 0, 0};
        render::psd_production::ReadContext context;
        context.onto = &onto;
        context.width_px = 240;  // (20 file pixels a mm)
        context.height_px = 180;
        context.layers = {{9, "a"}};
        Json said = Json::object();
        context.report = &said;
        QVERIFY(ps::read(page, ps::resource(s), context));
        QCOMPARE(said["slices"], Json(3));
        QCOMPARE(page.slices->group, std::string("web"));
        QCOMPARE(page.slices->list.size(), std::size_t(3));
        QCOMPARE(page.slices->list[0].rect_mm, (std::array<double, 4>{1, 0.5, 2, 1}));
        QCOMPARE(*page.slices->list[0].background, std::string("#123456"));
        QCOMPARE(page.slices->list[1].origin, std::string("layer"));
        QCOMPARE(*page.slices->list[1].layer, std::string("a"));
        QCOMPARE(page.slices->list[2].origin, std::string("user"));
        QCOMPARE(page.slices->next_id, std::int64_t(6));
        // (the page has some: not read over them)
        QVERIFY(!ps::read(page, ps::resource(s), context));
        QVERIFY(!ps::fingerprint(page, &doc).empty());
        // written in the file's pixels: the page's, then the auto ones (ids after)
        render::psd_production::WriteContext writing;
        writing.width_px = 120;
        writing.height_px = 90;
        writing.doc = &doc;
        writing.name_layer = [](const std::string& id) -> std::optional<std::int64_t> {
            if (id == "a") return 42;
            return std::nullopt;
        };
        const auto written = ps::parse(*ps::write(page, writing));
        QVERIFY(written);
        QCOMPARE(written->group, std::string("web"));
        QCOMPARE(written->bounds, (std::array<std::int32_t, 4>{0, 0, 90, 120}));
        QCOMPARE(written->slices[0].rect, (std::array<std::int32_t, 4>{10, 5, 30, 15}));
        QCOMPARE(written->slices[0].name, std::string("hero"));
        QCOMPARE(written->slices[1].origin, std::uint32_t(1));
        QCOMPARE(*written->slices[1].layer_id, std::uint32_t(42));
        // (where the layer is now, in the file's pixels)
        const auto layer_box = core::slice_box(*render::slices::layer_rect_mm(doc, page, page.slices->list[1]), core::slice_grid(12, 9, 120, 90));
        QCOMPARE(written->slices[1].rect, (std::array<std::int32_t, 4>{static_cast<std::int32_t>(layer_box.x0), static_cast<std::int32_t>(layer_box.y0),
                                                                        static_cast<std::int32_t>(layer_box.x1), static_cast<std::int32_t>(layer_box.y1)}));
        const auto autos = std::count_if(written->slices.begin(), written->slices.end(), [](const ps::FileSlice& f) { return f.origin == 0; });
        QVERIFY(autos > 0);
        for (const ps::FileSlice& f : written->slices)
            if (f.origin == 0) QVERIFY(f.id >= 6);
        // a page without slices: none
        QVERIFY(!ps::write(book().page(0), writing));
        QVERIFY(!ps::parse("broken"));
    }

    // A .pcraft's slices (PhotoCraft's Slices: groupName, list, rects in the document's pixels): read onto the page, written
    // back; an auto one left out; one not of the shape said.
    void pcraftSlices() {
        const Json value = Json::parse(R"({"groupName": "site", "list": [
            {"id": 1, "origin": "user", "name": "hero", "rect": {"x0": 10, "y0": 20, "x1": 50, "y1": 40}, "kind": "noImage",
             "url": "u", "target": "_top", "message": "m", "alt": "a", "cellTextIsHtml": true, "cellText": "<i>c</i>",
             "horizontalAlign": 2, "verticalAlign": 9, "background": [255, 1, 2, 3], "outsets": [0, 0, 0, 0]},
            {"id": 2, "origin": "layer", "layer": 5, "rect": {"x0": 0, "y0": 0, "x1": 10, "y1": 10}, "outsets": [1, 2, 3, 4]},
            {"id": 3, "origin": "auto", "rect": {"x0": 50, "y0": 0, "x1": 120, "y1": 90}}]})");
        core::Document doc = book();
        add_block(doc);
        render::pcraft_production::ReadContext context;
        context.page = &doc.edit_page(0);
        context.mm = 0.1;
        context.layers = {{"5", "a"}};
        std::vector<std::string> said;
        context.say = [&](std::string s) { said.push_back(std::move(s)); };
        render::pcraft_slices::read(value, context);
        QVERIFY(said.empty());
        const core::Slices& s = *doc.page(0).slices;
        QCOMPARE(s.group, std::string("site"));
        QCOMPARE(s.list.size(), std::size_t(2));
        QCOMPARE(s.list[0].rect_mm, (std::array<double, 4>{1, 2, 4, 2}));
        QCOMPARE(s.list[0].kind, std::string("noImage"));
        QCOMPARE(s.list[0].align, (std::array<int, 2>{2, 4}));
        QCOMPARE(*s.list[0].background, std::string("#010203"));
        QCOMPARE(s.list[1].origin, std::string("layer"));
        QCOMPARE(s.list[1].outsets, (std::array<int, 4>{1, 2, 3, 4}));
        // written back
        render::pcraft_production::WriteContext writing;
        writing.px = 10;
        writing.layers = {{"a", "5"}};
        writing.book = &doc;
        const auto out = render::pcraft_slices::write(doc.page(0), writing);
        QVERIFY(out);
        QCOMPARE((*out)["groupName"], Json("site"));
        QCOMPARE((*out)["list"][0]["rect"], Json::parse(R"({"x0": 10, "y0": 20, "x1": 50, "y1": 40})"));
        QCOMPARE((*out)["list"][0]["background"], Json::array({255, 1, 2, 3}));
        QCOMPARE((*out)["list"][1]["layer"], Json(5));
        QCOMPARE((*out)["list"][1]["origin"], Json("layer"));
        // (where the layer is now: the block at about 1..3 × 1..2 mm grown by its outsets)
        const auto r = *render::slices::layer_rect_mm(doc, doc.page(0), s.list[1]);
        const auto px = [](double mm) { return static_cast<std::int64_t>(std::llround(mm * 10)); };
        QCOMPARE((*out)["list"][1]["rect"], (Json{{"x0", px(r[0])}, {"y0", px(r[1])}, {"x1", px(r[0] + r[2])}, {"y1", px(r[1] + r[3])}}));
        QVERIFY(std::fabs(r[0] - (1 - 0.2)) < 0.1 && std::fabs(r[1] - (1 - 0.1)) < 0.1);
        // not of the shape: said, none
        core::Document other = book();
        render::pcraft_production::ReadContext bad;
        bad.page = &other.edit_page(0);
        bad.say = [&](std::string s2) { said.push_back(std::move(s2)); };
        render::pcraft_slices::read(Json::parse(R"({"list": [{"id": 1, "rect": [1, 2]}]})"), bad);
        QCOMPARE(said.size(), std::size_t(1));
        QCOMPARE(said[0], std::string("スライスは読めない形のため外しました"));
        QVERIFY(!other.page(0).slices);
    }

    // MCP's apply_ops does not take them (the C++ build's own, as J-a's, J-c's, J-d1's); none is a frame any more.
    void notTheAgents() {
        for (const char* name : {"add_slice", "set_slice", "delete_slices", "divide_slice", "slices_from_guides", "slices_info"}) {
            QVERIFY2(!service::agent_op(name), name);
            for (const std::string_view frame : render::production_op_names()) QVERIFY2(frame != name, name);
        }
    }
};

QTEST_GUILESS_MAIN(TestSlices)
#include "test_slices.moc"
