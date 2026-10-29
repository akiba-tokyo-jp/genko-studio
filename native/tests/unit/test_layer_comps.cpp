// PC5 J-a (layer-comps@1, SPEC.md PROD-01「レイヤーカンプ」): the layer comps of a page as PhotoCraft v0.2.0's own tests
// check them — crates/doc/src/comps.rs (capture records every layer, the ids, the layers gone) and
// engine/src/comps_cmds/tests.rs (visibility, position and appearance put back each alone; the last document state; the
// comps managed and gone round in their order; the warnings of the layers gone; the refusals) — on Genko's layers and
// pages: a paint layer's box in mm by whole working pixels, a folder, a fill and a layer showing nothing without a place,
// a pen layer's lines and a placed picture moved by applying, a layer style put back, the 8-bit, 16-bit and CMYK pages, a
// copied page's comps naming the copy's layers, the comps' JSON as written.

#include <QtTest>

#include <cmath>
#include <string>

#include "core/command_bus.hpp"
#include "core/json.hpp"
#include "core/layer_comps.hpp"
#include "core/model.hpp"
#include "render/image.hpp"
#include "render/layer_bounds.hpp"
#include "render/ops_layer_comps.hpp"
#include "render/ops_registry.hpp"
#include "render/raster.hpp"

using namespace genko;
using core::Json;
namespace lb = render::layer_bounds;
namespace lc = render::layer_comps;

namespace {

constexpr double kInch = 25.4;  // (a 25.4 mm page: 200 working pixels a side)
constexpr int kSide = 200;
constexpr double kPx = kInch / kSide;

core::ApplyResult run_ops(const core::Document& doc, const Json& ops) { return core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor("human:test")); }
core::Document run(const core::Document& doc, const Json& op) { return run_ops(doc, Json::array({op})).doc; }
Json report(const core::Document& doc, const Json& op) {
    const auto applied = run_ops(doc, Json::array({op}));
    return applied.results.empty() ? Json() : applied.results[0];
}

std::string refusal(const core::Document& doc, const Json& op) {
    try {
        run_ops(doc, Json::array({op}));
    } catch (const core::ApplyError& error) {
        return error.what();
    }
    return {};
}

render::Image block(int x0, int y0, int x1, int y1) {
    std::string bytes(std::size_t(kSide) * kSide * 4, '\0');
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const std::size_t i = (std::size_t(y) * kSide + std::size_t(x)) * 4;
            bytes[i] = static_cast<char>(200);
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

// A 25.4 mm page without panels: "a" (a block from (4, 4) to (14, 14) working pixels) and "b" (from (30, 20) to (40, 30)).
core::Document book() {
    auto doc = core::new_episode("カンプ", core::Num(1), 1, core::PageSpec::custom(kInch, kInch, 20, 20, 1, 2, 2, 2, 2, 200, "color"));
    auto& p = doc.edit_page(0);
    p.numero = false;
    p.frames.clear();
    p.layers.clear();
    add_paint(doc, "a", block(4, 4, 14, 14));
    add_paint(doc, "b", block(30, 20, 40, 30));
    return doc;
}

const core::Layer& layer_of(const core::Document& doc, std::string_view id) {
    for (const core::Layer& layer : doc.page(0).layers)
        if (layer.id == id) return layer;
    throw std::runtime_error("no layer " + std::string(id));
}

// The layer's box's top left in working pixels.
std::array<long, 2> at(const core::Document& doc, const std::string& id) {
    const auto box = lb::of_layer(doc, doc.page(0), id);
    if (!box) return {-1, -1};
    return {std::lround((*box)[0] / kPx), std::lround((*box)[1] / kPx)};
}

Json move(const std::string& id, double dx_px, double dy_px) {
    return Json{{"op", "transform_layers"}, {"page", 1}, {"ids", Json::array({id})}, {"move", Json::array({dx_px * kPx, dy_px * kPx})}};
}

Json op(const char* name, Json more = Json::object()) {
    Json out{{"op", name}, {"page", 1}};
    for (auto& [k, v] : more.items()) out[k] = v;
    return out;
}

const core::LayerComps& comps(const core::Document& doc) { return *doc.page(0).layer_comps; }

}  // namespace

class TestLayerComps : public QObject {
    Q_OBJECT
private slots:
    // comps.rs capture_records_every_layer: each layer of the page, its place the top left of its box; a folder, a fill and
    // a layer that shows nothing have none.
    void captureRecordsEveryLayer() {
        core::Document doc = book();
        doc = run(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "fill"}, {"id", "fill"}});
        doc = run(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "empty"}});
        doc = run(doc, Json{{"op", "group_layers"}, {"page", 1}, {"ids", Json::array({"b"})}, {"id", "F"}});
        doc = run(doc, Json{{"op", "set_layer"}, {"page", 1}, {"id", "a"}, {"opacity", 0.5}, {"blend", "multiply"}});
        const auto states = lc::capture(doc, doc.page(0));
        QCOMPARE(states.size(), doc.page(0).layers.size());
        for (const core::CompLayerState& s : states) {
            QVERIFY(s.visible && s.has_position && s.appearance);
            const bool placed = s.layer == "a" || s.layer == "b";
            QCOMPARE(s.position.has_value(), placed);
        }
        const auto& a = *std::find_if(states.begin(), states.end(), [](const auto& s) { return s.layer == "a"; });
        QCOMPARE(std::lround((*a.position)[0] / kPx), 4L);
        QCOMPARE(std::lround((*a.position)[1] / kPx), 4L);
        QCOMPARE(a.appearance->blend, std::string("multiply"));
        QCOMPARE(a.appearance->opacity, 0.5);
        QVERIFY(!a.appearance->style);
        // (new_layer_comp: "レイヤーカンプ 1", the page's own state the comp applied)
        doc = run(doc, op("new_layer_comp"));
        QCOMPARE(comps(doc).comps.size(), std::size_t(1));
        QCOMPARE(comps(doc).comps[0].name, std::string("レイヤーカンプ 1"));
        QCOMPARE(comps(doc).comps[0].id, std::int64_t(1));
        QCOMPARE(comps(doc).last_applied, std::optional<std::int64_t>(1));
        QCOMPARE(comps(doc).comps[0].layers, lc::capture(doc, doc.page(0)));
        // (its JSON as the book writes it reads back the same)
        QCOMPARE(core::layer_comps_from_json(core::to_json(comps(doc))), comps(doc));
        QCOMPARE(report(doc, op("new_layer_comp")), (Json{{"index", 0}, {"op", "new_layer_comp"}, {"comp", 2}}));
    }

    // comps_cmds/tests.rs capture_apply_visibility_position_appearance_independently, on an 8-bit page, a 16-bit one and a
    // CMYK one: each option alone puts back what it says, the pixels really moved.
    void applyEachAlone() {
        for (const char* mode : {"", "rgb16", "cmyk"}) {
            core::Document doc = book();
            if (std::string(mode) == "rgb16") doc = run(doc, Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}});
            if (std::string(mode) == "cmyk") {
                doc = run(doc, Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "cmyk"}, {"depth", 16}, {"profile", "coated-cmyk"}});
            }
            doc = run(doc, op("new_layer_comp", {{"name", "Start"}}));
            const std::int64_t start = comps(doc).comps[0].id;
            doc = run(doc, move("a", 10, 5));
            doc = run(doc, Json{{"op", "set_layer"}, {"page", 1}, {"id", "b"}, {"visible", false}, {"blend", "multiply"}, {"opacity", 0.5}});
            QCOMPARE(at(doc, "a"), (std::array<long, 2>{14, 9}));
            // visibility alone
            doc = run(doc, op("set_layer_comp", {{"comp", start}, {"position", false}, {"appearance", false}}));
            doc = run(doc, op("apply_layer_comp", {{"comp", start}}));
            QVERIFY2(layer_of(doc, "b").visible, mode);
            QCOMPARE(at(doc, "a"), (std::array<long, 2>{14, 9}));
            QCOMPARE(layer_of(doc, "b").blend, std::string("multiply"));
            // position alone
            doc = run(doc, Json{{"op", "set_layer"}, {"page", 1}, {"id", "b"}, {"visible", false}});
            doc = run(doc, op("set_layer_comp", {{"comp", start}, {"visibility", false}, {"position", true}}));
            doc = run(doc, op("apply_layer_comp"));
            QCOMPARE(at(doc, "a"), (std::array<long, 2>{4, 4}));
            QVERIFY(!layer_of(doc, "b").visible);
            // appearance alone
            doc = run(doc, op("set_layer_comp", {{"comp", "Start"}, {"position", false}, {"appearance", true}}));
            doc = run(doc, op("apply_layer_comp"));
            QCOMPARE(layer_of(doc, "b").blend, std::string("normal"));
            QCOMPARE(layer_of(doc, "b").opacity, 1.0);
            QVERIFY(!layer_of(doc, "b").visible);
            // (the pixels moved, not a box kept aside: the block is back where it was and nothing is left where it went)
            const auto box = lb::of_layer(doc, doc.page(0), "a");
            QCOMPARE(std::lround(((*box)[2] - (*box)[0]) / kPx), 10L);
        }
    }

    // A pen layer's lines and a layer style come back too; a locked and a position-locked layer move (the comp is not a
    // drag); a layer is moved alone, not its linked one.
    void linesStylesAndLocks() {
        core::Document doc = book();
        doc = run(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "pen"}, {"panel_each", false}});
        doc = run(doc, Json{{"op", "set_layer"}, {"page", 1}, {"id", "pen"}, {"panel_clip", false}});
        doc = run(doc, Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", "pen"}, {"points", Json::array({Json::array({5, 5}), Json::array({10, 6})})},
                            {"width_mm", 0.4}});
        const Json style{{"effects", Json::array({Json{{"kind", "stroke"}, {"size_mm", 0.5}}})}};
        doc = run(doc, Json{{"op", "set_layer_style"}, {"page", 1}, {"id", "a"}, {"style", style}});
        const Json held = *layer_of(doc, "a").style;
        doc = run(doc, Json{{"op", "link_layers"}, {"page", 1}, {"ids", Json::array({"a", "b"})}});
        doc = run(doc, op("new_layer_comp"));
        const auto pen_at = at(doc, "pen");
        doc = run(doc, move("pen", 20, 10));
        doc = run(doc, move("a", 3, 0));  // (b goes with it: linked)
        QCOMPARE(at(doc, "b"), (std::array<long, 2>{33, 20}));
        doc = run(doc, Json{{"op", "set_layer_style"}, {"page", 1}, {"id", "a"}, {"style", nullptr}});
        doc = run(doc, Json{{"op", "set_layer"}, {"page", 1}, {"id", "a"}, {"locked", true}, {"lock_position", true}});
        QVERIFY(!layer_of(doc, "a").style);
        doc = run(doc, op("set_layer_comp", {{"comp", 1}, {"position", true}}));
        // (b recorded at (30, 20): back there itself; a back at (4, 4) though locked, and alone)
        doc = run(doc, op("apply_layer_comp", {{"comp", 1}}));
        QCOMPARE(at(doc, "pen"), pen_at);
        QCOMPARE(at(doc, "a"), (std::array<long, 2>{4, 4}));
        QCOMPARE(at(doc, "b"), (std::array<long, 2>{30, 20}));
        QVERIFY(layer_of(doc, "a").style);
        QCOMPARE(*layer_of(doc, "a").style, held);
    }

    // comps_cmds/tests.rs undo_redo_and_last_document_state: applied over the page's own state, that state is kept and
    // "last_state" puts it back (no comp applied then).
    void lastDocumentState() {
        core::Document doc = book();
        doc = run(doc, op("new_layer_comp"));
        QVERIFY(!comps(doc).last_state);
        doc = run(doc, move("a", 3, 0));
        // (applying the comp applied last: no own state kept)
        doc = run(doc, op("apply_layer_comp"));
        QCOMPARE(at(doc, "a"), (std::array<long, 2>{4, 4}));
        QVERIFY(!comps(doc).last_state);
        QVERIFY(refusal(doc, op("apply_layer_comp", {{"comp", "last_state"}})).find("no layer comp has been applied") != std::string::npos);
        doc = run(doc, move("a", 3, 0));
        doc = run(doc, op("delete_layer_comp"));
        QVERIFY(!doc.page(0).layer_comps);  // (an empty one is none)
        doc = run(doc, op("new_layer_comp"));
        doc = run(doc, move("a", 2, 2));  // (7, 4) → (9, 6)
        // (a fresh own state — the comp applied last forgotten —: kept when a comp is applied over it)
        core::Page& p = doc.edit_page(0);
        p.layer_comps->last_applied.reset();
        doc = run(doc, op("apply_layer_comp"));
        QCOMPARE(at(doc, "a"), (std::array<long, 2>{7, 4}));
        QVERIFY(comps(doc).last_state);
        const Json back = report(doc, op("apply_layer_comp", {{"comp", "last_state"}}));
        QCOMPARE(back["comp"], Json(nullptr));
        doc = run(doc, op("apply_layer_comp", {{"comp", "last_state"}}));
        QCOMPARE(at(doc, "a"), (std::array<long, 2>{9, 6}));
        QVERIFY(!comps(doc).last_applied);
        QVERIFY(comps(doc).last_state);
    }

    // comps_cmds/tests.rs manage_comps_and_cycle: duplicate (after its original, 「… のコピー」), rename, comment, next and
    // previous round the list's order, one property updated, every comp updated.
    void manageAndCycle() {
        core::Document doc = book();
        doc = run(doc, op("new_layer_comp", {{"name", "One"}, {"comment", "first"}}));
        doc = run(doc, Json{{"op", "set_layer"}, {"page", 1}, {"id", "b"}, {"visible", false}});
        doc = run(doc, op("new_layer_comp", {{"name", "Two"}}));
        QCOMPARE(report(doc, op("duplicate_layer_comp", {{"comp", 1}}))["comp"], Json(3));
        doc = run(doc, op("duplicate_layer_comp", {{"comp", 1}}));
        std::vector<std::string> names;
        for (const auto& c : comps(doc).comps) names.push_back(c.name);
        QCOMPARE(names, (std::vector<std::string>{"One", "One のコピー", "Two"}));
        QCOMPARE(comps(doc).comps[1].comment, std::string("first"));
        doc = run(doc, op("set_layer_comp", {{"comp", 3}, {"name", "Three"}}));
        doc = run(doc, op("set_layer_comp", {{"comp", "Three"}, {"comment", "dup"}}));
        QCOMPARE(comps(doc).comps[1].name, std::string("Three"));
        QCOMPARE(comps(doc).comps[1].comment, std::string("dup"));
        // next and previous: in the list's order, round
        doc = run(doc, op("apply_layer_comp", {{"comp", 2}}));
        doc = run(doc, op("apply_layer_comp", {{"comp", "next"}}));
        QCOMPARE(comps(doc).last_applied, std::optional<std::int64_t>(1));
        QVERIFY(layer_of(doc, "b").visible);
        doc = run(doc, op("apply_layer_comp", {{"comp", "previous"}}));
        QCOMPARE(comps(doc).last_applied, std::optional<std::int64_t>(2));
        QVERIFY(!layer_of(doc, "b").visible);
        doc = run(doc, op("apply_layer_comp", {{"comp", "previous"}}));
        QCOMPARE(comps(doc).last_applied, std::optional<std::int64_t>(3));
        // one property updated: One's visibility follows the page again, its places as recorded
        doc = run(doc, move("a", 1, 0));
        QVERIFY(layer_of(doc, "b").visible);  // (Three — One's copy — shows it)
        doc = run(doc, Json{{"op", "set_layer"}, {"page", 1}, {"id", "b"}, {"visible", false}});
        doc = run(doc, op("update_layer_comp", {{"comp", 1}, {"what", "visibility"}}));
        const auto& one = comps(doc).comps[0];
        for (const auto& s : one.layers) {
            if (s.layer == "b") QCOMPARE(s.visible, std::optional<bool>(false));
            if (s.layer == "a") QCOMPARE(std::lround((*s.position)[0] / kPx), 4L);
        }
        QCOMPARE(report(doc, op("update_layer_comp", {{"comp", "*"}}))["updated"], (Json::array({1, 3, 2})));
        doc = run(doc, op("update_layer_comp", {{"comp", "*"}, {"what", "position"}}));
        for (const auto& c : comps(doc).comps)
            for (const auto& s : c.layers)
                if (s.layer == "a") QCOMPARE(std::lround((*s.position)[0] / kPx), 5L);
    }

    // comps_cmds/tests.rs warnings_for_deleted_layers: a layer gone is left out of applying and said; clearing drops it.
    void warningsForLayersGone() {
        core::Document doc = book();
        doc = run(doc, op("new_layer_comp"));
        QVERIFY(refusal(doc, op("clear_layer_comp_warnings")).find("no layer comp of the page names") != std::string::npos);
        doc = run(doc, Json{{"op", "delete_layer"}, {"page", 1}, {"id", "a"}});
        QCOMPARE(lc::missing(doc.page(0), comps(doc).comps[0].layers), (std::vector<std::string>{"a"}));
        QCOMPARE(lc::listing(doc.page(0))["comps"][0]["missing"], Json::array({"a"}));
        QCOMPARE(report(doc, op("apply_layer_comp"))["missing"], Json::array({"a"}));
        QCOMPARE(report(doc, op("clear_layer_comp_warnings"))["cleared"], (Json::array({Json{{"comp", 1}, {"missing", Json::array({"a"})}}})));
        doc = run(doc, op("clear_layer_comp_warnings"));
        QCOMPARE(lc::listing(doc.page(0))["comps"][0]["missing"], Json::array());
        QCOMPARE(comps(doc).comps[0].layers.size(), std::size_t(1));
    }

    // comps_cmds/tests.rs disabled_states_and_bad_params, in this build's words.
    void refusals() {
        core::Document doc = book();
        for (const char* name : {"apply_layer_comp", "update_layer_comp", "set_layer_comp", "duplicate_layer_comp", "delete_layer_comp"}) {
            QVERIFY2(refusal(doc, op(name, {{"name", "x"}})).find("the page has no layer comps") != std::string::npos, name);
        }
        doc = run(doc, op("new_layer_comp"));
        QVERIFY(refusal(doc, op("apply_layer_comp", {{"comp", 999}})).find("no layer comp with id 999") != std::string::npos);
        QVERIFY(refusal(doc, op("apply_layer_comp", {{"comp", "nope"}})).find("no layer comp named nope") != std::string::npos);
        QVERIFY(refusal(doc, op("apply_layer_comp", {{"comp", 1.5}})).find("comp is a layer comp's id or name") != std::string::npos);
        QVERIFY(refusal(doc, op("update_layer_comp", {{"what", "colour"}})).find("what is all") != std::string::npos);
        QVERIFY(refusal(doc, op("set_layer_comp")).find("give name, comment") != std::string::npos);
        QVERIFY(refusal(doc, op("set_layer_comp", {{"name", ""}})).find("name is 1 to 255 characters") != std::string::npos);
        QVERIFY(refusal(doc, op("set_layer_comp", {{"name", std::string(256, 'x')}})).find("name is 1 to 255 characters") != std::string::npos);
        QVERIFY(refusal(doc, op("set_layer_comp", {{"name", std::string(255, 'x')}})).empty());
        QVERIFY(refusal(doc, op("new_layer_comp", {{"visibility", "yes"}})).find("visibility is true or false") != std::string::npos);
        // (at most 256 a page)
        core::Page& p = doc.edit_page(0);
        while (p.layer_comps->comps.size() < core::kLayerCompsMax) {
            core::LayerComp c = p.layer_comps->comps[0];
            c.id = static_cast<std::int64_t>(p.layer_comps->comps.size()) + 1;
            p.layer_comps->comps.push_back(std::move(c));
        }
        QVERIFY(refusal(doc, op("new_layer_comp")).find("at most 256 layer comps") != std::string::npos);
        QVERIFY(refusal(doc, op("duplicate_layer_comp")).find("at most 256 layer comps") != std::string::npos);
        // (a comp's file name's part)
        QCOMPARE(lc::file_part("No/B"), std::string("No_B"));
        QCOMPARE(lc::file_part("a:b*c?. "), std::string("a_b_c_"));
    }

    // A copied page's comps name the copy's layers (duplicate_page gives them new ids).
    void copiedPage() {
        core::Document doc = book();
        doc = run(doc, op("new_layer_comp"));
        doc = run(doc, Json{{"op", "duplicate_page"}, {"page", 1}});
        QCOMPARE(doc.pages.size(), std::size_t(2));
        const core::Page& copy = doc.page(1);
        QVERIFY(copy.layer_comps);
        std::set<std::string> ids;
        for (const core::Layer& l : copy.layers) ids.insert(l.id);
        for (const auto& s : copy.layer_comps->comps[0].layers) QVERIFY2(ids.contains(s.layer), s.layer.c_str());
        QVERIFY(lc::missing(copy, copy.layer_comps->comps[0].layers).empty());
    }
};

QTEST_GUILESS_MAIN(TestLayerComps)
#include "test_layer_comps.moc"
