// Tones and effect lines without the Python reference (M3-B): the tone and effect ops through the CommandBus — each
// op's success, its refusals (the book unchanged), the page lock and strict_gates, dry-run, Undo and Redo through the
// journal, the book saved and read again (patch pictures included; all twelve M3-B ops, the rulers' too, in one
// table) — and their drawing: the black share of each
// screen in print and the grey in proof, effect lines inside their panel, a part of a page drawn alone the same as
// the whole page cut, a tone layer alone, black and white with a dot screen; numpy's random numbers and sums, and the
// dot screen's ranks. The exact pixels against Python are in contract/test_contract_tone_render.cpp.
//
// (The ops are written as JSON with $NAME for the ids made at run time: moc reads raw strings as code, so each one
// keeps its braces balanced.)

#include <QtTest>

#include <QTemporaryDir>

#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <string>

#include "core/base64.hpp"
#include "core/command_bus.hpp"
#include "core/ids.hpp"
#include "core/pyvalue.hpp"
#include "m3b_support.hpp"
#include "render/effects.hpp"
#include "render/npcompat.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/tones.hpp"
#include "storage/snapshot.hpp"
#include "storage/undo.hpp"
#include "testsupport.hpp"

namespace render = genko::render;
using genko::core::Document;
using genko::core::Json;
using genko::core::LayerKind;
using genko::core::LayerRole;
using genko::core::Num;
namespace m3b = genko::test::m3b;

namespace {

using Values = std::map<std::string, std::string>;

// `text` with each $NAME replaced (the longest names first).
std::string with(std::string text, const Values& values) {
    for (auto it = values.rbegin(); it != values.rend(); ++it) {
        std::size_t at = 0;
        while ((at = text.find(it->first, at)) != std::string::npos) {
            text.replace(at, it->first.size(), it->second);
            at += it->second.size();
        }
    }
    return text;
}

const std::string kSquare = "{\"poly\": [[60, 80], [120, 80], [120, 140], [60, 140]]}";

Json j(const std::string& text, const Values& values = {}) {
    Values all = values;
    all["$SQUARE"] = kSquare;
    return genko::core::parse_python_json(with(text, all));
}

Document book(bool name_ok = true) {
    Document doc = genko::core::new_episode("トーン", Num(1), 1, genko::core::PageSpec::b4_comic());
    doc.edit_page(0).numero = false;  // (nombres are drawn in M4)
    if (name_ok) doc = m3b::bus().apply(doc, j(R"([{"op": "name_ok", "page": 1}])"), genko::core::Actor("genko")).doc;
    return doc;
}

std::string role_id(const Document& doc, LayerRole role) {
    for (const auto& layer : doc.page(0).layers) {
        if (layer.role == role) return layer.id;
    }
    return {};
}

const genko::core::Layer* layer_of(const Document& doc, const std::string& id) {
    for (const auto& layer : doc.page(0).layers) {
        if (layer.id == id) return &layer;
    }
    return nullptr;
}

std::string error_of(const Document& doc, const std::string& ops, const Values& values = {}, const std::string& actor = "genko") {
    const Json before = genko::storage::snapshot(doc, true);
    std::string out;
    try {
        (void)m3b::bus().apply(doc, j(ops, values), genko::core::Actor(actor));
    } catch (const genko::core::ApplyError& error) {
        out = error.what();
    }
    if (genko::storage::snapshot(doc, true) != before) out += " [the book changed]";
    return out;
}

// error_of for ops made in this process (numbers JSON text cannot hold: NaN, infinities).
std::string error_of_ops(const Document& doc, const Json& ops) {
    try {
        (void)m3b::bus().apply(doc, ops, genko::core::Actor("genko"));
    } catch (const genko::core::ApplyError& error) {
        return error.what();
    }
    return {};
}

Document applied(const Document& doc, const std::string& ops, const Values& values = {}) {
    return m3b::bus().apply(doc, j(ops, values), genko::core::Actor("genko")).doc;
}

int px(double mm, int dpi) { return render::mm_to_px(mm, dpi); }

// (black share, mean grey) of a box (mm) of the page in a mode (Python's test_m15 _black_share)
std::pair<double, double> share(const Document& doc, double x, double y, double w, double h, const std::string& mode = "print", int dpi = 150) {
    render::RenderOptions options;
    options.mode = mode;
    const render::Image grey = render::render_page(doc.page(0), dpi, options, &doc).image.convert("L");
    const std::string data = grey.crop(render::Box{px(x, dpi), px(y, dpi), px(x + w, dpi), px(y + h, dpi)}).tobytes();
    double black = 0, sum = 0;
    for (const char c : data) {
        const auto v = static_cast<unsigned char>(c);
        black += v < 128 ? 1 : 0;
        sum += v;
    }
    return {black / static_cast<double>(data.size()), sum / static_cast<double>(data.size())};
}

genko::core::StrokePtr line(double x0, double y0, double x1, double y1, double width, const char* kind) {
    auto s = std::make_shared<genko::core::Stroke>();
    s->id = genko::core::new_id();
    s->points = {{x0, y0}, {x1, y1}};
    s->kind = kind;
    s->width_mm = width;
    return s;
}

}  // namespace

class TestTonesEffects : public QObject {
    Q_OBJECT

    QTemporaryDir scratch_;

private slots:
    void initTestCase() { QVERIFY(scratch_.isValid()); }
    void init() { render::clear_render_caches(); }

    void the_ops_are_built_in_where_pages_are_drawn() {
        for (const char* name : {"add_tone", "set_tone", "delete_tone", "add_effect", "edit_effect", "delete_effect", "effect_to_layer",
                                 "add_ruler", "edit_ruler", "delete_ruler", "set_ruler", "ruler_to_layer"}) {
            QVERIFY2(genko::core::OpRegistry::builtin().find(name) != nullptr, name);
        }
    }

    // --- tones ----------------------------------------------------------------------------------------------------

    void add_tone() {
        const Document doc = book();
        render::Image dot = render::Image::create("L", render::Size{4, 4}, render::Ink(0));
        dot.paste(render::Ink(255), render::Box{1, 1, 3, 3});
        const Values v{{"$FRAME", doc.page(0).leaf_frames().front()->id}, {"$MASKPNG", genko::core::b64encode(render::write_png(dot))}};
        const Document out = applied(doc, R"([{"op": "add_tone", "page": 1, "id": "t", "density": 0.4, "lpi": 50, "area": $SQUARE},
            {"op": "add_tone", "page": 1, "id": "r", "area": {"rect": [10, 20, 30, 40]}, "pattern": "line", "angle": 30, "name": "線"},
            {"op": "add_tone", "page": 1, "id": "e", "area": {"ellipse": [10, 20, 30, 40]}, "dot_shape": "square", "offset_mm": [0.5, 0]},
            {"op": "add_tone", "page": 1, "id": "f", "frame_id": "$FRAME", "gradient": {"shape": "radial", "start": 0, "end": 0.6}},
            {"op": "add_tone", "page": 1, "id": "all", "after": "t"},
            {"op": "add_tone", "page": 1, "id": "m", "area": {"mask": {"box": [5, 5, 10, 10], "png": "$MASKPNG"}}}])", v);
        const auto* t = layer_of(out, "t");
        QVERIFY(t != nullptr && t->kind == LayerKind::Tone && t->role == LayerRole::Tone);
        QCOMPARE(*t->tone, j(R"({"pattern": "dot"})"));
        QCOMPARE(t->lpi->value(), 50.0);
        QCOMPARE(t->density->value(), 0.4);
        QCOMPARE(t->patches.size(), std::size_t{1});
        const Json box = t->patches[0].attrs["box"];
        QVERIFY(std::fabs(box[0].get<double>() - 60) < 0.1 && std::fabs(box[1].get<double>() - 80) < 0.1);
        QVERIFY(std::fabs(box[2].get<double>() - 60) < 0.2 && std::fabs(box[3].get<double>() - 60) < 0.2);
        QCOMPARE(t->patches[0].attrs["rgb"], j("[0, 0, 0]"));
        QCOMPARE(layer_of(out, "r")->title, std::string("線"));
        QCOMPARE(layer_of(out, "r")->angle, 30.0);
        QCOMPARE(*layer_of(out, "e")->tone, j(R"({"pattern": "dot", "dot_shape": "square", "offset_mm": [0.5, 0.0]})"));
        QCOMPARE(layer_of(out, "f")->tone->at("gradient"), j(R"({"shape": "radial", "start": 0, "end": 0.6})"));
        QVERIFY(layer_of(out, "all")->patches.empty());  // (no area: every panel)
        QCOMPARE(layer_of(out, "m")->patches.size(), std::size_t{1});
        const auto& layers = out.page(0).layers;  // ("after": just after that layer)
        for (std::size_t i = 0; i + 1 < layers.size(); ++i) {
            if (layers[i].id == "t") QCOMPARE(layers[i + 1].id, std::string("all"));
        }
        // refusals
        const std::vector<std::pair<std::string, std::string>> refused{
            {R"([{"op": "add_tone", "page": 1, "pattern": "stripes"}])", "ops[0] add_tone: pattern must be one of dot, line, cross"},
            {R"([{"op": "add_tone", "page": 1, "density": 1.5}])", "ops[0] add_tone: density is 0 to 1 (the black share)"},
            {R"([{"op": "add_tone", "page": 1, "lpi": 400}])", "ops[0] add_tone: lpi is 5 to 300"},
            {R"([{"op": "add_tone", "page": 1, "pattern": "image"}])", "ops[0] add_tone: an image tone needs its picture"},
            {R"([{"op": "add_tone", "page": 1, "dot_shape": "star"}])", "ops[0] add_tone: dot_shape must be one of round, square"},
            {R"([{"op": "add_tone", "page": 1, "gradient": {"shape": "conic"}}])", "ops[0] add_tone: gradient shape must be linear or radial"},
            {R"([{"op": "add_tone", "page": 1, "offset_mm": [1, 2, 3]}])", "ops[0] add_tone: offset_mm is [x, y] in mm"},
            {R"([{"op": "add_tone", "page": 1, "scale_mm": 60}])", "ops[0] add_tone: scale_mm is 0.3 to 50"},
            {R"([{"op": "add_tone", "page": 1, "frame_id": "nope"}])", "ops[0] add_tone: no panel nope"},
            {R"([{"op": "add_tone", "page": 1, "area": {"poly": [[1, 2], [3, 4]]}}])", "ops[0] add_tone: an area needs at least three corners"},
            {R"([{"op": "add_tone", "page": 1, "area": {"mask": {"box": [1, 2, 3, 4]}}}])", "ops[0] add_tone: area is {poly"},
            {R"([{"op": "add_tone", "page": 1, "area": {"mask": {"box": [5, 5, 10, 10], "png": "$EMPTYPNG"}}}])", "ops[0] add_tone: the area is empty"},
            {R"([{"op": "add_tone", "page": 1, "after": "nope"}])", "ops[0] add_tone: no layer nope"},
            {R"([{"op": "add_tone", "page": 1, "angle": "inf"}])", "ops[0] add_tone: angle must be a finite number"},
            {R"([{"op": "add_tone", "page": 1, "area": {"layer": "x"}}])", "ops[0] add_tone: an area of this kind"}};
        const Values empty{{"$EMPTYPNG", genko::core::b64encode(render::write_png(render::Image::create("L", render::Size{4, 4}, render::Ink(0))))}};
        for (const auto& [ops, message] : refused) {
            QVERIFY2(error_of(doc, ops, empty).starts_with(message), (ops + " → " + error_of(doc, ops, empty)).c_str());
        }
        QVERIFY(error_of(out, R"([{"op": "add_tone", "page": 1, "id": "t"}])").starts_with("ops[0] add_tone: layer t already exists"));
        // (a polygon of no size is one pixel of tone, as in Python)
        QCOMPARE(layer_of(applied(doc, R"([{"op": "add_tone", "page": 1, "id": "p", "area": {"poly": [[1, 1], [1, 1], [1, 1]]}}])"), "p")->patches[0].attrs["box"],
                 j("[1.016, 1.016, 0.085, 0.085]"));
    }

    void tone_by_the_region_a_fill_takes() {
        Document doc = book(false);  // (the name stage: drawn in the name mode, as Python's fill looks at it)
        const std::string ink = role_id(doc, LayerRole::Ink);
        for (auto& layer : doc.edit_page(0).layers) {  // (the four sides of a square, put on the ink layer directly)
            if (layer.id == ink) {
                layer.strokes = genko::core::make_strokes({line(60, 80, 120, 80, 0.6, "mili"), line(120, 80, 120, 140, 0.6, "mili"),
                                                           line(120, 140, 60, 140, 0.6, "mili"), line(60, 140, 60, 80, 0.6, "mili")});
            }
        }
        const Document out = applied(doc, R"([{"op": "add_tone", "page": 1, "at": {"x_mm": 90, "y_mm": 110}, "density": 0.3, "id": "f"}])");
        const auto* f = layer_of(out, "f");
        QCOMPARE(f->patches.size(), std::size_t{1});
        const Json box = f->patches[0].attrs["box"];
        QVERIFY2(std::fabs(box[0].get<double>() - 60) < 1.5 && std::fabs(box[2].get<double>() - 60) < 2, genko::core::dump_python(box).c_str());
        QVERIFY(error_of(doc, R"([{"op": "add_tone", "page": 1, "at": {"x_mm": 60, "y_mm": 110}}])").starts_with("ops[0] add_tone: nothing to fill there"));
        QVERIFY(error_of(doc, R"([{"op": "add_tone", "page": 1, "at": {"x_mm": 60, "y_mm": 110, "reference": "reference"}}])")
                    .starts_with("ops[0] add_tone: no layer is set as the reference"));
        QVERIFY(error_of(doc, R"([{"op": "add_tone", "page": 1, "at": {"y_mm": 110}}])").starts_with("ops[0] add_tone: not found: 'x_mm'"));
        const Document by_layer = applied(doc, R"([{"op": "add_tone", "page": 1, "at": {"x_mm": 90, "y_mm": 110, "reference": "layer"}, "id": "g"}])");
        QCOMPARE(layer_of(by_layer, "g")->patches.size(), std::size_t{1});
    }

    void set_and_delete_tone() {
        Document doc = applied(book(), R"([{"op": "add_tone", "page": 1, "id": "g", "density": 0.5, "area": $SQUARE}])");
        doc = applied(doc, R"([{"op": "set_tone", "page": 1, "id": "g", "gradient": {"shape": "radial", "start": 0.0, "end": 0.6}, "offset_mm": [0.5, 0],
                               "pattern": "line", "scale_mm": 2, "name": "名", "lpi": 30, "angle": 0}])");
        const auto* g = layer_of(doc, "g");
        QCOMPARE(*g->tone, j(R"({"pattern": "line", "scale_mm": 2.0, "gradient": {"shape": "radial", "start": 0.0, "end": 0.6}, "offset_mm": [0.5, 0.0]})"));
        QCOMPARE(g->title, std::string("名"));
        QCOMPARE(g->lpi->value(), 30.0);
        doc = applied(doc, R"([{"op": "set_tone", "page": 1, "id": "g", "move_by_mm": [-0.5, 0], "scale_mm": null, "gradient": null}])");
        QCOMPARE(*layer_of(doc, "g")->tone, j(R"({"pattern": "line", "gradient": null})"));  // (moved back: no offset kept)
        QVERIFY(error_of(doc, R"([{"op": "set_tone", "page": 1, "id": "g", "pattern": "stripes"}])").starts_with("ops[0] set_tone: pattern must be one of"));
        QVERIFY(error_of(doc, R"([{"op": "set_tone", "page": 1, "id": "g", "density": 1.5}])").starts_with("ops[0] set_tone: density is 0 to 1"));
        QVERIFY(error_of(doc, R"([{"op": "set_tone", "page": 1, "id": "g", "dot_shape": "star"}])").starts_with("ops[0] set_tone: dot_shape must be one of"));
        QVERIFY(error_of(doc, R"([{"op": "set_tone", "page": 1, "id": "$INK", "density": 0.5}])", {{"$INK", role_id(doc, LayerRole::Ink)}})
                    .starts_with("ops[0] set_tone: that layer is not a tone"));
        QVERIFY(error_of(doc, R"([{"op": "set_tone", "page": 1, "id": "nope"}])").starts_with("ops[0] set_tone: no layer nope"));
        const Document gone = applied(doc, R"([{"op": "delete_tone", "page": 1, "id": "g"}])");
        QVERIFY(layer_of(gone, "g") == nullptr);
        QVERIFY(error_of(gone, R"([{"op": "delete_tone", "page": 1, "id": "g"}])").starts_with("ops[0] delete_tone: no tone g"));
    }

    // --- effect lines ---------------------------------------------------------------------------------------------

    void effect_ops() {
        const Document doc = book();
        Values v{{"$FRAME", doc.page(0).leaf_frames().front()->id}, {"$INK", role_id(doc, LayerRole::Ink)}};
        Document out = applied(doc, R"([{"op": "add_effect", "page": 1, "kind": "focus", "frame_id": "$FRAME", "id": "f",
                                         "params": {"count": 40, "bundle": 5, "bundle_gap": 0.7, "jitter_position": 0}},
                                        {"op": "add_effect", "page": 1, "kind": "speed", "id": "s", "params": {"count": 12}},
                                        {"op": "add_effect", "page": 1, "kind": "beta_flash", "frame_id": "$FRAME", "id": "b"}])", v);
        QCOMPARE(out.page(0).effects.size(), std::size_t{3});
        QCOMPARE(out.page(0).effects[1], j(R"({"id": "s", "kind": "speed", "frame_id": null, "params": {"count": 12}})"));
        out = applied(out, R"([{"op": "edit_effect", "page": 1, "id": "f", "params": {"bundle": null, "center": [90, 120]}, "visible": 0}])");
        QCOMPARE(out.page(0).effects[0]["params"], j(R"({"count": 40, "bundle_gap": 0.7, "jitter_position": 0, "center": [90, 120]})"));
        QCOMPARE(out.page(0).effects[0]["visible"], Json(false));
        const std::vector<std::pair<std::string, std::string>> refused{
            {R"([{"op": "add_effect", "page": 1, "kind": "sparkle"}])", "ops[0] add_effect: kind must be one of focus, speed"},
            {R"([{"op": "add_effect", "page": 1, "kind": "focus", "params": {"taper": "sideways"}}])", "ops[0] add_effect: taper is in, out, both or false"},
            {R"([{"op": "add_effect", "page": 1, "kind": "speed", "params": {"bundle": 0}}])", "ops[0] add_effect: bundle is 1 to 50 lines"},
            {R"([{"op": "add_effect", "page": 1, "kind": "speed", "params": {"count": 5000}}])", "ops[0] add_effect: too many lines"},
            {R"([{"op": "add_effect", "page": 1, "kind": "speed", "params": {"within": [[1, 2]]}}])", "ops[0] add_effect: within is a shape"},
            {R"([{"op": "add_effect", "page": 1, "kind": "speed", "params": {"avoid": [{"circle": 1}]}}])", "ops[0] add_effect: avoid is a list"},
            {R"([{"op": "add_effect", "page": 1, "kind": "speed", "frame_id": "nope"}])", "ops[0] add_effect: no panel nope"}};
        for (const auto& [ops, message] : refused) QVERIFY2(error_of(doc, ops).starts_with(message), ops.c_str());
        QVERIFY(error_of(out, R"([{"op": "edit_effect", "page": 1, "id": "f", "params": {"jitter": 3}}])").starts_with("ops[0] edit_effect: jitter is 0 to 1"));
        QVERIFY(error_of(out, R"([{"op": "edit_effect", "page": 1, "id": "x"}])").starts_with("ops[0] edit_effect: no effect x"));
        QVERIFY(error_of(out, R"([{"op": "delete_effect", "page": 1, "id": "x"}])").starts_with("ops[0] delete_effect: no effect x"));
        QCOMPARE(applied(out, R"([{"op": "delete_effect", "page": 1, "id": "s"}])").page(0).effects.size(), std::size_t{2});
        // effect_to_layer: pen lines (効果線ペン) and fills on a layer
        const Document drawn = applied(out, R"([{"op": "effect_to_layer", "page": 1, "id": "s", "layer_id": "$INK"},
                                                {"op": "effect_to_layer", "page": 1, "id": "b", "layer_id": "$INK", "keep": true}])", v);
        const auto* layer = layer_of(drawn, v["$INK"]);
        QCOMPARE(layer->stroke_count(), std::size_t{12});
        QCOMPARE(layer->patches.size(), std::size_t{2});
        for (const auto& s : layer->strokes->items) {
            QCOMPARE(s->kind, std::string("fx"));
            QCOMPARE(s->points.size(), s->pressure.size());
            QVERIFY(!s->rgb.has_value());
        }
        QCOMPARE(drawn.page(0).effects.size(), std::size_t{2});  // (b kept)
        QVERIFY(error_of(out, R"([{"op": "effect_to_layer", "page": 1, "id": "s", "layer": "bogus"}])").find("'bogus' is not a valid LayerRole") != std::string::npos);
        QVERIFY(error_of(out, R"([{"op": "effect_to_layer", "page": 1, "id": "x"}])").starts_with("ops[0] effect_to_layer: no effect x"));
    }

    void strict_gates_and_the_page_lock() {
        Document doc = book(false);
        doc = applied(doc, R"([{"op": "set_meta", "strict_gates": true}, {"op": "add_effect", "page": 1, "kind": "speed", "id": "s"}])");
        const Values v{{"$INK", role_id(doc, LayerRole::Ink)}, {"$NAME", role_id(doc, LayerRole::Name)}};
        QVERIFY(error_of(doc, R"([{"op": "effect_to_layer", "page": 1, "id": "s"}])").starts_with("ops[0] effect_to_layer: effect_to_layer on ink needs name_ok on page 1 (strict_gates)"));
        QVERIFY(error_of(doc, R"([{"op": "effect_to_layer", "page": 1, "id": "s", "layer_id": "$INK"}])", v)
                    .starts_with("ops[0] effect_to_layer: effect_to_layer on a printed layer needs name_ok on page 1 (strict_gates)"));
        QVERIFY(error_of(doc, R"([{"op": "effect_to_layer", "page": 1, "id": "s", "layer_id": "$NAME"}])", v).empty());  // (the name: a try)
        QVERIFY(error_of(doc, R"([{"op": "effect_to_layer", "page": 1, "id": "s", "layer": "draft"}])").empty());
        QVERIFY(error_of(doc, R"([{"op": "name_ok", "page": 1}, {"op": "effect_to_layer", "page": 1, "id": "s"}])").empty());
        const Document locked = applied(doc, R"([{"op": "lock_page", "page": 1, "agent": "ai:other"}])");
        for (const std::string op : {R"({"op": "add_tone", "page": 1})", R"({"op": "set_tone", "page": 1, "id": "x"})", R"({"op": "delete_tone", "page": 1, "id": "x"})",
                                     R"({"op": "add_effect", "page": 1, "kind": "focus"})", R"({"op": "edit_effect", "page": 1, "id": "s"})",
                                     R"({"op": "delete_effect", "page": 1, "id": "s"})", R"({"op": "effect_to_layer", "page": 1, "id": "s"})"}) {
            QVERIFY2(error_of(locked, "[" + op + "]").find(": page 1 locked by ai:other") != std::string::npos, op.c_str());
        }
        const auto dry = m3b::bus().apply(doc, j(R"([{"op": "add_tone", "page": 1, "id": "d"}])"), genko::core::Actor("genko"), true);
        QVERIFY(layer_of(dry.doc, "d") != nullptr && layer_of(doc, "d") == nullptr);
    }

    // Each of the twelve ops (the rulers' too) on one book: applied; a refusal leaves the book as it was; another
    // agent's page lock; strict_gates as Python has them (of these ops only effect_to_layer is a raster edit held back
    // until the name is approved; ruler_to_layer adds pen lines, which strict_gates leaves alone, as add_stroke);
    // dry-run gives the book applying it gives and keeps the given one; saved, read again, undone and redone through
    // the journal.
    void each_op_refused_locked_gated_dry_run_undone_and_read_again() {
        const Document base = applied(book(false), R"([{"op": "add_tone", "page": 1, "id": "t", "area": $SQUARE},
            {"op": "add_effect", "page": 1, "kind": "speed", "id": "s", "params": {"count": 6}},
            {"op": "add_effect", "page": 1, "kind": "focus", "id": "f", "params": {"count": 8}},
            {"op": "add_ruler", "page": 1, "kind": "line", "points": [[20, 100], [200, 160]], "id": "r"},
            {"op": "add_ruler", "page": 1, "kind": "guide", "axis": "h", "at": 50, "id": "g"}])");
        const Values v{{"$INK", role_id(base, LayerRole::Ink)}};
        struct Case {
            std::string op;      // applied
            std::string bad;     // refused
            std::string reason;  // how the refusal starts, after "ops[0] <op>: "
        };
        const std::vector<Case> cases{
            {R"({"op": "add_tone", "page": 1, "id": "n", "pattern": "line", "area": $SQUARE})", R"({"op": "add_tone", "page": 1, "density": 2})",
             "density is 0 to 1"},
            {R"({"op": "set_tone", "page": 1, "id": "t", "density": 0.6, "pattern": "cross"})", R"({"op": "set_tone", "page": 1, "id": "t", "lpi": 1})",
             "lpi is 5 to 300"},
            {R"({"op": "delete_tone", "page": 1, "id": "t"})", R"({"op": "delete_tone", "page": 1, "id": "zz"})", "no tone zz"},
            {R"({"op": "add_effect", "page": 1, "kind": "beta_flash", "id": "b"})", R"({"op": "add_effect", "page": 1, "kind": "sparkle"})",
             "kind must be one of focus, speed"},
            {R"({"op": "edit_effect", "page": 1, "id": "s", "params": {"angle": 45}})", R"({"op": "edit_effect", "page": 1, "id": "s", "params": {"jitter": 3}})",
             "jitter is 0 to 1"},
            {R"({"op": "delete_effect", "page": 1, "id": "f"})", R"({"op": "delete_effect", "page": 1, "id": "zz"})", "no effect zz"},
            {R"({"op": "effect_to_layer", "page": 1, "id": "s", "layer_id": "$INK"})", R"({"op": "effect_to_layer", "page": 1, "id": "zz"})", "no effect zz"},
            {R"({"op": "add_ruler", "page": 1, "kind": "ellipse", "points": [[10, 10], [60, 40]], "id": "e"})",
             R"({"op": "add_ruler", "page": 1, "kind": "line", "points": [[1, 1]]})", "a line ruler needs 2 point(s)"},
            {R"({"op": "edit_ruler", "page": 1, "id": "r", "points": [[30, 100], [200, 170]], "active": false})", R"({"op": "edit_ruler", "page": 1, "id": "zz"})",
             "no ruler zz"},
            {R"({"op": "delete_ruler", "page": 1, "id": "g"})", R"({"op": "delete_ruler", "page": 1, "id": "zz"})", "no ruler zz"},
            {R"({"op": "set_ruler", "page": 1, "kind": "perspective", "points": [[90, 30], [10, 30]]})", R"({"op": "set_ruler", "page": 1, "points": 5})",
             "a value of the wrong type ('int' object is not iterable)"},
            {R"({"op": "ruler_to_layer", "page": 1, "id": "r", "layer_id": "$INK"})", R"({"op": "ruler_to_layer", "page": 1, "id": "zz"})", "no ruler zz"}};
        const Document locked = applied(base, R"([{"op": "lock_page", "page": 1, "agent": "ai:other"}])");
        const Document strict = applied(base, R"([{"op": "set_meta", "strict_gates": true}])");
        const Document approved = applied(strict, R"([{"op": "name_ok", "page": 1}])");
        std::set<std::string> names;
        int n = 0;
        for (const Case& c : cases) {
            const std::string ops = "[" + c.op + "]";
            const std::string name = j(c.op, v)["op"].get<std::string>();
            names.insert(name);
            const std::string refused = error_of(base, "[" + c.bad + "]", v);
            QVERIFY2(refused.starts_with("ops[0] " + name + ": " + c.reason), (name + " → " + refused).c_str());
            const std::string lock = error_of(locked, ops, v);
            QVERIFY2(lock.find(": page 1 locked by ai:other") != std::string::npos, (name + " → " + lock).c_str());
            QVERIFY2(error_of(locked, ops, v, "ai:other").empty(), name.c_str());  // (its own page)
            const std::string gated = error_of(strict, ops, v);
            if (name == "effect_to_layer") {
                QVERIFY2(gated.starts_with("ops[0] effect_to_layer: effect_to_layer on a printed layer needs name_ok on page 1 (strict_gates)"), gated.c_str());
                QVERIFY(error_of(approved, ops, v).empty());
            } else {
                QVERIFY2(gated.empty(), (name + " → " + gated).c_str());
            }
            // dry-run: the same book as applying it (ids counted alike), the given book unchanged
            const QString folder = scratch_.path() + QStringLiteral("/each-%1").arg(n++);
            const Json given = m3b::state_of(base, m3b::to_path(folder + "/s0"));
            genko::core::ApplyResult real;
            genko::core::ApplyResult dry;
            {
                genko::core::ScopedIdSource ids(genko::core::counting_ids(500));
                real = m3b::bus().apply(base, j(ops, v), genko::core::Actor("genko"));
            }
            {
                genko::core::ScopedIdSource ids(genko::core::counting_ids(500));
                dry = m3b::bus().apply(base, j(ops, v), genko::core::Actor("genko"), true);
            }
            const Json applied_state = m3b::state_of(real.doc, m3b::to_path(folder + "/s1"));
            QVERIFY2(applied_state != given, name.c_str());  // (it did something)
            QCOMPARE(m3b::state_of(dry.doc, m3b::to_path(folder + "/s2")), applied_state);
            QCOMPARE(m3b::state_of(base, m3b::to_path(folder + "/s3")), given);
            // saved, read again, undone and redone through the journal
            const auto dir = m3b::to_path(folder + "/book.genko");
            m3b::save_new(dir, base);
            const Json before = m3b::state_of(m3b::read(dir), m3b::to_path(folder + "/s4"));
            const auto e = m3b::edit(dir, j(ops, v));
            const Json after = m3b::state_of(e.result.doc, m3b::to_path(folder + "/s5"));
            QVERIFY2(after != before, name.c_str());
            QCOMPARE(m3b::state_of(m3b::read(dir), m3b::to_path(folder + "/s6")), after);
            // (what Undo or Redo brings back that the current book does not have keeps the saved state's sorted key
            // order — storage::ordered_like: the same content)
            std::string where;
            genko::storage::restore(dir, "genko", false, false);
            QVERIFY2(genko::test::same_content(m3b::state_of(m3b::read(dir), m3b::to_path(folder + "/s7")), before, &where), (name + ": " + where).c_str());
            genko::storage::restore(dir, "genko", true, false);
            QVERIFY2(genko::test::same_content(m3b::state_of(m3b::read(dir), m3b::to_path(folder + "/s8")), after, &where), (name + ": " + where).c_str());
            QCOMPARE(genko::test::book_problems(QString::fromStdString(genko::storage::path_to_utf8(dir))), std::string());
        }
        QCOMPARE(names.size(), std::size_t{12});
        // a number that is not finite, which Python would keep and write into project.json as Infinity or NaN, is
        // refused, named by its key: made by float() from a str ("inf") where the op converts it, or given in the ops of
        // this process (the command line refuses NaN and Infinity in its input before any op runs)
        const double inf = std::numeric_limits<double>::infinity();
        Json center = j(R"([{"op": "add_effect", "page": 1, "kind": "focus", "params": {"center": [0, 2]}}])");
        center[0]["params"]["center"][0] = inf;
        Json twist = j(R"([{"op": "edit_effect", "page": 1, "id": "f", "params": {"twist": 0}}])");
        twist[0]["params"]["twist"] = -inf;
        Json gradient = j(R"([{"op": "set_tone", "page": 1, "id": "t", "gradient": {"shape": "linear", "angle": 0}}])");
        gradient[0]["gradient"]["angle"] = std::nan("");
        Json points = j(R"([{"op": "set_ruler", "page": 1, "points": [[0, 2]]}])");
        points[0]["points"][0][0] = inf;
        const std::vector<std::pair<Json, std::string>> nonfinite{
            {center, "ops[0] add_effect: center must be a finite number"},
            {twist, "ops[0] edit_effect: twist must be a finite number"},
            {gradient, "ops[0] set_tone: gradient.angle must be a finite number"},
            {points, "ops[0] set_ruler: points must be a finite number"},
            {j(R"([{"op": "set_tone", "page": 1, "id": "t", "offset_mm": [0, "inf"]}])"), "ops[0] set_tone: offset_mm must be a finite number"},
            {j(R"([{"op": "add_ruler", "page": 1, "kind": "concentric", "points": [[1, 2]], "ratio": "nan"}])"), "ops[0] add_ruler: ratio must be a finite number"}};
        for (const auto& [ops, message] : nonfinite) {
            const std::string got = error_of_ops(base, ops);
            QVERIFY2(got.starts_with(message), (message + " → " + got).c_str());
        }
    }

    void undo_redo_and_reopen() {
        const auto dir = m3b::to_path(scratch_.path() + "/undo.genko");
        m3b::save_new(dir, book());
        const Json before = m3b::state_of(m3b::read(dir), m3b::to_path(scratch_.path() + "/u1"));
        const Values v{{"$INK", role_id(m3b::read(dir), LayerRole::Ink)}};
        const auto e = m3b::edit(dir, j(R"([{"op": "add_tone", "page": 1, "id": "t", "pattern": "sand", "area": $SQUARE},
                                            {"op": "add_effect", "page": 1, "kind": "white", "id": "w"},
                                            {"op": "add_effect", "page": 1, "kind": "uni_flash", "id": "u", "params": {"count": 20}},
                                            {"op": "effect_to_layer", "page": 1, "id": "u", "layer_id": "$INK"},
                                            {"op": "effect_to_layer", "page": 1, "id": "w", "layer_id": "$INK"}])", v));
        const Json after = m3b::state_of(e.result.doc, m3b::to_path(scratch_.path() + "/u2"));
        const Document again = m3b::read(dir);
        QCOMPARE(m3b::state_of(again, m3b::to_path(scratch_.path() + "/u3")), after);
        QCOMPARE(layer_of(again, "t")->patches.size(), std::size_t{1});
        QVERIFY(render::read_png(*layer_of(again, "t")->patches[0].png).tobytes() == render::read_png(*layer_of(e.result.doc, "t")->patches[0].png).tobytes());
        genko::storage::restore(dir, "genko", false, false);
        QCOMPARE(m3b::state_of(m3b::read(dir), m3b::to_path(scratch_.path() + "/u4")), before);
        genko::storage::restore(dir, "genko", true, false);
        // (a Redo brings back the saved state, whose objects the current book does not have keep the state's sorted key
        // order — storage::ordered_like: the same content)
        std::string where;
        QVERIFY2(genko::test::same_content(m3b::state_of(m3b::read(dir), m3b::to_path(scratch_.path() + "/u5")), after, &where), where.c_str());
        m3b::edit(dir, j(R"([{"op": "set_tone", "page": 1, "id": "t", "density": 0.7}])"));
        genko::storage::restore(dir, "genko", false, false);
        QVERIFY2(genko::test::same_content(m3b::state_of(m3b::read(dir), m3b::to_path(scratch_.path() + "/u6")), after, &where), where.c_str());
        QCOMPARE(genko::test::book_problems(QString::fromStdString(genko::storage::path_to_utf8(dir))), std::string());
    }

    // Settings kept as given (a str "inf" or "nan" where a number goes), as Python keeps them. Drawing the page fails
    // where Python's fails (its int() of NaN or an infinity, math.sin of an infinity) and draws where Python's draws;
    // effect_to_layer gives Python's error where Python gives one, and is refused where Python would add lines of
    // Infinity and NaN to the layer (and write them into project.json). The outcomes expected are those the Python
    // baseline gave for the same ops.
    void settings_that_are_not_finite_numbers() {
        struct Case {
            const char* kind;
            const char* params;
            bool python_draws;
            const char* to_layer;  // "" = applied; else how the refusal starts, after "ops[0] effect_to_layer: "
        };
        const std::string corrupt = "this effect's settings give lines that are not finite numbers";
        const std::vector<Case> cases{
            {"focus", R"({"center": ["inf", 2]})", false, corrupt.c_str()},
            {"uni_flash", R"({"center": ["nan", 2]})", false, corrupt.c_str()},
            {"focus", R"({"inner": ["inf", 3]})", false, corrupt.c_str()},
            {"focus", R"({"length_mm": "inf"})", false, corrupt.c_str()},
            {"speed", R"({"width_mm": "inf"})", false, corrupt.c_str()},
            {"speed", R"({"curve": "inf"})", false, corrupt.c_str()},
            {"speed", R"({"spread_mm": "inf", "path": [[1, 2], [30, 40]]})", false, corrupt.c_str()},
            {"focus", R"({"twist": "inf"})", false, "a value of the wrong type (math domain error)"},
            {"speed", R"({"angle": "inf"})", false, "a value of the wrong type (math domain error)"},
            {"beta_flash", R"({"center": ["nan", 2]})", false, "a value of the wrong type (cannot convert float NaN to integer)"},
            // (Python: an OverflowError apply_ops lets out, a traceback)
            {"beta_flash", R"({"center": ["inf", 2]})", false, "a value of the wrong type (cannot convert float infinity to integer)"},
            {"white", R"({"twist": "inf"})", true, ""},
            {"speed", R"({"spacing_mm": "nan"})", true, ""},
            {"speed", R"({"center": ["inf", 2]})", true, ""}};
        for (const Case& c : cases) {
            const std::string label = std::string(c.kind) + " " + c.params;
            Document doc = genko::core::new_episode("t", Num(1), 1, genko::core::PageSpec::custom(60, 60, 54, 54, 2, 5, 5, 4, 4));
            doc.edit_page(0).numero = false;
            doc = applied(doc, R"([{"op": "add_effect", "page": 1, "kind": "$K", "id": "e", "params": $P}])", {{"$K", c.kind}, {"$P", c.params}});
            bool drew = true;
            try {
                (void)render::render_page(doc.page(0), 72, render::RenderOptions{}, &doc);
            } catch (const genko::core::Error&) {
                drew = false;
            }
            QVERIFY2(drew == c.python_draws, label.c_str());
            const std::string refused = error_of(doc, R"([{"op": "effect_to_layer", "page": 1, "id": "e"}])");
            if (*c.to_layer == '\0') {
                QVERIFY2(refused.empty(), (label + " → " + refused).c_str());
            } else {
                QVERIFY2(refused.starts_with(std::string("ops[0] effect_to_layer: ") + c.to_layer), (label + " → " + refused).c_str());
            }
        }
        // a tone's gradient at an angle of "inf" (kept as a str): Python's math.cos raises when the page is drawn
        const Document toned = applied(book(), R"([{"op": "add_tone", "page": 1, "id": "g", "area": $SQUARE, "gradient": {"shape": "linear", "angle": "inf"}}])");
        QVERIFY_THROWS_EXCEPTION(genko::core::Error, (void)render::render_page(toned.page(0), 72, render::RenderOptions{}, &toned));
    }

    // --- drawing --------------------------------------------------------------------------------------------------

    void tone_patterns_print_the_asked_black_share() {
        for (const char* pattern : {"dot", "line", "cross", "noise"}) {
            const Document doc = applied(book(), R"([{"op": "add_tone", "page": 1, "id": "t", "density": 0.3, "lpi": 50, "pattern": "$P", "area": $SQUARE}])",
                                         {{"$P", pattern}});
            QVERIFY2(std::fabs(share(doc, 65, 85, 50, 50).first - 0.3) < 0.05, pattern);
            QVERIFY(share(doc, 130, 85, 20, 20).first < 0.01);
            const double grey = share(doc, 65, 85, 50, 50, "proof").second;
            QVERIFY2(160 < grey && grey < 200, pattern);
        }
        // a gradient gets lighter along its direction; an old tone with no area covers every panel
        const Document gradient = applied(book(), R"([{"op": "add_tone", "page": 1, "id": "g", "density": 0.5, "area": $SQUARE,
                                                       "gradient": {"shape": "linear", "angle": 90, "start": 0.6, "end": 0.0}}])");
        QVERIFY(share(gradient, 62, 82, 56, 10).first > 0.4 && share(gradient, 62, 128, 56, 10).first < 0.1);
        const Document old = applied(book(), R"([{"op": "add_tone", "page": 1, "density": 0.3}])");
        QVERIFY(std::fabs(share(old, 40, 60, 100, 100).first - 0.3) < 0.05);
    }

    void effects_draw_inside_their_panel() {
        Document doc = book();
        doc.edit_page(0).split_frame(doc.page(0).frames[0].id, "horizontal", Num(0.5), Num(6));
        const auto leaves = doc.page(0).leaf_frames();
        const auto* top = leaves[0]->rect.y < leaves[1]->rect.y ? leaves[0] : leaves[1];
        const auto* bottom = top == leaves[0] ? leaves[1] : leaves[0];
        for (const char* kind : {"focus", "speed", "uni_flash", "beta_flash"}) {
            const Document out = applied(doc, R"([{"op": "add_effect", "page": 1, "kind": "$K", "frame_id": "$TOP", "id": "e", "params": {"count": 120}}])",
                                         {{"$K", kind}, {"$TOP", top->id}});
            const auto& r = top->rect;
            const auto& b = bottom->rect;
            QVERIFY2(share(out, r.x.value(), r.y.value(), r.width.value(), r.height.value()).first > 0.03, kind);
            QVERIFY2(share(out, b.x.value() + 5, b.y.value() + 5, b.width.value() - 10, b.height.value() - 10).first < 0.005, kind);
        }
    }

    void a_part_of_a_page_is_the_whole_page_cut() {
        Document doc = applied(book(), R"([{"op": "add_tone", "page": 1, "id": "a", "pattern": "noise", "density": 0.4, "area": $SQUARE},
            {"op": "add_tone", "page": 1, "id": "b", "pattern": "star", "area": {"rect": [100, 120, 60, 50]}, "gradient": {"start": 0.1, "end": 0.9}},
            {"op": "add_tone", "page": 1, "id": "c", "pattern": "sand", "area": {"ellipse": [30, 200, 80, 60]}, "angle": 20},
            {"op": "add_tone", "page": 1, "id": "d", "pattern": "dot", "dot_shape": "diamond", "offset_mm": [0.3, -0.2],
             "gradient": {"shape": "radial", "start": 0.0, "end": 0.7}},
            {"op": "add_effect", "page": 1, "kind": "focus", "params": {"count": 60, "avoid": [{"ellipse": [120, 180, 20, 15]}], "twist": 20}},
            {"op": "add_effect", "page": 1, "kind": "speed", "params": {"path": [[30, 50], [120, 90], [200, 60]], "count": 15}}])");
        const auto grey = [&](const char* screen) {
            for (auto& layer : doc.edit_page(0).layers) {
                if (layer.role != LayerRole::Ink) continue;
                auto s = line(40, 40, 150, 300, 6, "airbrush");
                std::const_pointer_cast<genko::core::Stroke>(s)->rgb = std::vector<std::int64_t>{110, 110, 110};
                layer.strokes = genko::core::make_strokes({s});
                layer.screen = j(screen);
            }
        };
        grey(R"({"pattern": "noise", "white": 0.9})");
        for (const char* mode : {"print", "proof"}) {
            render::RenderOptions options;
            options.mode = mode;
            options.screen_dots = true;
            const render::Image whole = render::render_page(doc.page(0), 100, options, &doc).image;
            for (const render::RenderRegion r : {render::RenderRegion{0, 0, 1, 1}, render::RenderRegion{170, 290, 230, 150},
                                                 render::RenderRegion{0, 600, whole.width(), 40}, render::RenderRegion{whole.width() - 60, whole.height() - 45, 60, 45}}) {
                render::RenderOptions part = options;
                part.region = r;
                const render::Image got = render::render_page(doc.page(0), 100, part, &doc).image;
                QVERIFY2(got.tobytes() == whole.crop(render::Box{r.x, r.y, r.x + r.w, r.y + r.h}).tobytes(), mode);
            }
        }
        grey(R"({"pattern": "dot", "lpi": 40, "shape": "ellipse", "offset_mm": [1, 0]})");  // (the layer's screen as dots)
        const render::Image whole = render::render_page(doc.page(0), 100, render::RenderOptions{}, &doc).image;
        render::RenderOptions part;
        part.region = render::RenderRegion{120, 130, 200, 260};
        QVERIFY(render::render_page(doc.page(0), 100, part, &doc).image.tobytes() == whole.crop(render::Box{120, 130, 320, 390}).tobytes());
    }

    void a_tone_layer_alone_and_black_and_white() {
        const Document doc = applied(book(), R"([{"op": "add_tone", "page": 1, "id": "t", "density": 0.5, "area": $SQUARE}])");
        const render::Image alone = render::layer_image(doc.page(0), *layer_of(doc, "t"), 72);
        QVERIFY(alone.getchannel(3).getextrema()[0].second > 100);  // (its ink as alpha)
        QCOMPARE(alone.getpixel(5, 5)[3], 0.0);
        // to_bitonal with a screen: the greys as dots; solid black and paper stay
        render::Image grey = render::Image::create("L", render::Size{400, 200}, render::Ink(255));
        grey.paste(render::Ink(150), render::Box{0, 0, 200, 200});
        grey.paste(render::Ink(0), render::Box{200, 0, 260, 200});
        const Json screen = j(R"({"lpi": 60, "dpi": 600})");
        const std::string toned = render::to_bitonal(grey, 180, &screen).convert("L").tobytes();
        double black = 0;
        bool solid = true, paper = true;
        for (int y = 0; y < 200; ++y) {
            for (int x = 0; x < 400; ++x) {
                const auto v = static_cast<unsigned char>(toned[static_cast<std::size_t>(y * 400 + x)]);
                if (x < 200) black += v == 0 ? 1 : 0;
                if (x >= 200 && x < 260 && v != 0) solid = false;
                if (x >= 300 && v != 255) paper = false;
            }
        }
        black /= 200.0 * 200.0;
        QVERIFY(0.2 < black && black < 0.6);
        QVERIFY(solid && paper);
    }

    // --- numpy and the screen ---------------------------------------------------------------------------------------

    void numpy_random_numbers_and_sums() {
        // numpy.random.default_rng(seed).random() (numpy 2.4)
        QCOMPARE(render::np::Pcg64::from_seed(7).random(), 0.625095466604667);
        QCOMPARE(render::np::Pcg64::from_seed(0).random(), 0.6369616873214543);
        QCOMPARE(render::np::Pcg64::from_seed(1).random(), 0.5118216247002567);
        QCOMPARE(render::np::Pcg64::from_seed(12345).random(), 0.22733602246716966);
        QCOMPARE(render::np::Pcg64::from_seed((1ULL << 40) + 3).random(), 0.7736560850714245);
        QCOMPARE(render::np::Pcg64({5, 0, 1}).random(), 0.7426581154321283);  // (2**64 + 5)
        // np.full((h, w), v, float32).mean()
        const auto mean_of = [](int h, int w, float value) {
            const std::vector<float> a(static_cast<std::size_t>(h) * static_cast<std::size_t>(w), value);
            return render::np::mean(a.data(), a.size());
        };
        QCOMPARE(mean_of(37, 53, 0.3f), 0.29999998f);
        QCOMPARE(mean_of(100, 101, 0.3f), 0.29999995f);
        QCOMPARE(mean_of(513, 257, static_cast<float>(1.0 / 3)), 0.33333334f);
        QCOMPARE(mean_of(300, 211, 0.7f), 0.6999999f);
        QCOMPARE(render::np::remainder(-0.25f, 1.0f), 0.75f);
        QCOMPARE(render::np::remainder(3.0f, 2.0f), 1.0f);
    }

    void dot_shapes_differ_and_keep_the_black_share() {
        std::map<std::string, std::string> tiles;
        for (const char* shape : {"round", "square", "diamond", "ellipse"}) tiles[shape] = render::tones::threshold_tile(8, 8, shape)->tobytes();
        for (const auto& [shape, tile] : tiles) {
            double below = 0;
            for (const char c : tile) below += static_cast<unsigned char>(c) < std::lround(0.3 * 256) ? 1 : 0;
            QVERIFY2(std::fabs(below / static_cast<double>(tile.size()) - 0.3) < 0.02, shape.c_str());
        }
        for (const char* shape : {"square", "diamond", "ellipse"}) {
            bool differs = false;
            for (int level = 20; level < 240 && !differs; level += 10) {
                for (std::size_t i = 0; i < tiles["round"].size(); ++i) {
                    if ((static_cast<unsigned char>(tiles[shape][i]) < level) != (static_cast<unsigned char>(tiles["round"][i]) < level)) differs = true;
                }
            }
            QVERIFY2(differs, shape);
        }
        QVERIFY_THROWS_EXCEPTION(genko::core::Error, render::tones::threshold_tile(60, 60, "round"));  // (too coarse)
    }
};

QTEST_GUILESS_MAIN(TestTonesEffects)
#include "test_tones_effects.moc"
