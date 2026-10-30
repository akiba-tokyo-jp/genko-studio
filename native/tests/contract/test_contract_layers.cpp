// PC5 I-a (SPEC.md AUTO-01「レイヤーの基本」): the rest of Photoshop's layer basics as an agent or a person uses them —
// delete_layers (several, a group with what it holds, the core layers and すべてをロック kept and said, hidden, empty, every
// page; what named the layers let go — lines set under one, a ruler kept to one, an animation's cel — or kept — layer comps,
// variables —, the book written and read again), duplicate_layers (a group with what it holds, onto another page of the
// same paper and mode, refused onto another), ungroup_layers (one step), すべてをロック (layer-locks@1: nothing drawn, moved,
// changed or deleted; its visibility, name and order changed; a page op as Photoshop's), the channels a layer leaves out
// (blend-channels@1) on the three ways — 8 bits, precise colour, inks — drawn whole, in a part, through `genko render`, merged
// (8 bits; precise refused in words), bake_layer_styles (the look kept in the pixels on the three ways), PSD's lspf and brst
// as psd-tools reads them and read back, a .pcraft's locks and excluded channels, and a book without them as before (no
// feature, no key, the same bytes written twice).

#include <QtTest>

#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "core/command_bus.hpp"
#include "core/ink.hpp"
#include "core/json.hpp"
#include "core/layer_basics.hpp"
#include "core/paths.hpp"
#include "core/strokes.hpp"
#include "core/variables.hpp"
#include "formats/export.hpp"
#include "formats/open_file.hpp"
#include "formats/pcraft_write.hpp"
#include "render/image.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/psd.hpp"
#include "render/raster.hpp"
#include "render/zip.hpp"
#include "storage/reader.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
namespace fs = std::filesystem;

namespace {

core::ApplyResult run_ops(const core::Document& doc, const Json& ops) {
    return core::CommandBus(render::ops_registry()).apply(doc, ops.is_array() ? ops : Json::array({ops}), core::Actor("human:test"));
}
core::Document run(const core::Document& doc, const Json& ops) { return run_ops(doc, ops).doc; }

std::string refusal(const core::Document& doc, const Json& ops) {
    try {
        run_ops(doc, ops);
    } catch (const core::ApplyError& error) {
        return error.code() + ": " + error.what();
    }
    return {};
}

// A book of `pages` pages 60 × 80 mm, their panels taken away: "8" (8 bits, colour), "16" (precise RGB) or "cmyk" (inks).
core::Document book(const std::string& way = "8", int pages = 1) {
    core::PageSpec spec;
    spec.width_mm = core::Num(60);
    spec.height_mm = core::Num(80);
    spec.dpi = core::Num(150);
    spec.bleed_mm = core::Num(3);
    spec.inner_margin_mm = core::Num(5);
    spec.expression = "color";
    core::Document doc = core::new_episode("la", core::Num(1), pages, spec, core::Binding::Right);
    for (int p = 0; p < pages; ++p) doc.edit_page(static_cast<std::size_t>(p)).frames.clear();
    if (way == "16") doc = run(doc, Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}});
    if (way == "cmyk") doc = run(doc, Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}, {"depth", 16}});
    return doc;
}

Json op(const char* name, Json more = Json::object(), int page = 1) {
    Json out{{"op", name}, {"page", page}};
    for (auto& [k, v] : more.items()) out[k] = v;
    return out;
}
Json fill(const std::string& id, const Json& rgb, int page = 1) {
    return op("add_layer", {{"kind", "fill"}, {"id", id}, {"rgb", rgb}}, page);
}
Json paint(const std::string& id, int page = 1) { return op("add_layer", {{"kind", "paint"}, {"id", id}}, page); }
Json drawn_in(const std::string& id, double x0, double y0, double x1, double y1, const Json& rgb) {
    return op("fill_area", {{"layer_id", id},
                            {"area", Json{{"poly", Json::array({Json::array({x0, y0}), Json::array({x1, y0}), Json::array({x1, y1}), Json::array({x0, y1})})}}},
                            {"rgb", rgb}});
}
// A paint layer of pixels — a block of `rgb` over the part [x0, x1] × [y0, y1] of the page's working pixels.
void add_pixels(core::Document& doc, const std::string& id, const std::string& title, int x0, int y0, int x1, int y1, std::array<int, 3> rgb) {
    core::Page& page = doc.edit_page(0);
    const render::Size size{render::mm_to_px(page.spec.width_mm.value(), 200), render::mm_to_px(page.spec.height_mm.value(), 200)};
    std::string bytes(std::size_t(size.width) * std::size_t(size.height) * 4, '\0');
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const std::size_t i = (std::size_t(y) * std::size_t(size.width) + std::size_t(x)) * 4;
            for (int c = 0; c < 3; ++c) bytes[i + std::size_t(c)] = static_cast<char>(rgb[std::size_t(c)]);
            bytes[i + 3] = static_cast<char>(255);
        }
    }
    core::Layer layer;
    layer.id = id;
    layer.title = title;
    layer.kind = core::LayerKind::Raster;
    layer.panel_clip = false;
    render::raster::save_raster(page, layer, render::Image::frombytes("RGBA", size, bytes));
    page.layers.push_back(std::move(layer));
}

Json folder(const std::string& id, const Json& more = Json::object()) {
    Json out = op("add_layer", {{"kind", "folder"}, {"id", id}, {"group", true}});
    out.update(more);
    return out;
}
Json into(const std::vector<std::string>& ids, const std::string& group) {
    Json list = Json::array();
    for (const std::string& id : ids) list.push_back(id);
    return op("move_layers", {{"ids", list}, {"parent", group}});
}

const core::Layer* find(const core::Document& doc, const std::string& id, std::size_t page = 0) {
    for (const core::Layer& l : doc.page(page).layers)
        if (l.id == id) return &l;
    return nullptr;
}
std::size_t position(const core::Document& doc, const std::string& id, std::size_t page = 0) {
    const auto& layers = doc.page(page).layers;
    for (std::size_t i = 0; i < layers.size(); ++i)
        if (layers[i].id == id) return i;
    return layers.size();
}
const core::Layer* core_of(const core::Document& doc, core::LayerRole role) {
    for (const core::Layer& l : doc.page(0).layers)
        if (l.role == role) return &l;
    return nullptr;
}

render::RenderOptions proof() {
    render::RenderOptions options;
    options.mode = "proof";
    options.finish = false;
    return options;
}
render::Image drawn(const core::Document& doc, int dpi = 50) { return render::render_page(doc.page(0), dpi, proof(), &doc).image.convert("RGB"); }
std::array<int, 3> rgb_at(const render::Image& image, int x, int y) {
    const auto p = image.getpixel(x, y);
    return {static_cast<int>(p[0]), static_cast<int>(p[1]), static_cast<int>(p[2])};
}
bool near(const std::array<int, 3>& a, const std::array<int, 3>& b, int by = 3) {
    for (int c = 0; c < 3; ++c)
        if (std::abs(a[c] - b[c]) > by) return false;
    return true;
}
QByteArray shown(const std::array<int, 3>& c) { return QByteArray::number(c[0]) + "," + QByteArray::number(c[1]) + "," + QByteArray::number(c[2]); }

}  // namespace

class TestContractLayers : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;
    QString path(const QString& rel) const { return tmp_.path() + QStringLiteral("/") + rel; }
    int views_ = 0;
    Json psd_view(const QString& psd) {
        const QString out = path(QStringLiteral("basics-%1.json").arg(++views_));
        const auto ran = genko::test::run(genko::test::python_ref(),
                                          {genko::test::repo_root() + QStringLiteral("/tools/migration/psd_layer_basics_check.py"), out, psd},
                                          genko::test::python_env(path(QStringLiteral("pyenv"))));
        if (ran.exit_code != 0) return Json{{"error", ran.err.toStdString()}};
        return genko::test::read_json(out)[psd.toStdString()];
    }

private slots:
    void initTestCase() { QVERIFY(tmp_.isValid()); }

    // Several layers deleted in one step, a group with what it holds; the core layers kept and said; what named them let go
    // or kept as their own designs say; the book written and read again, clean.
    void deletingSeveralLayers() {
        core::Document doc = run(book(), Json::array({folder("g"), paint("a"), paint("b"), into({"a", "b"}, "g"), paint("p"),
                                                      drawn_in("a", 5, 5, 20, 20, Json::array({0, 0, 0})), op("new_layer_comp", {{"name", "A"}})}));
        const std::string ink = core_of(doc, core::LayerRole::Ink)->id;
        // a line set under a, a ruler kept to a, an animation's cel of b and its light table, a variable of a
        doc = run(doc, op("add_line", {{"id", "l1"}, {"text", "あ"}, {"x_mm", 10}, {"y_mm", 10}}));
        for (core::StoryLine& line : doc.story)
            if (line.id == "l1") line.style["below_layer"] = "a";
        doc.edit_page(0).rulers = Json::array({Json{{"id", "r1"}, {"kind", "parallel"}, {"points", Json::array({Json::array({0, 0}), Json::array({10, 0})})},
                                                    {"layer_id", "a"}}});
        doc.edit_page(0).extra["anim"] = Json{{"tracks", Json::array({Json{{"folder", "g"}, {"cels", Json::array({Json::array({0, "b"})})}},
                                                                       Json{{"folder", "kept"}, {"cels", Json::array({Json::array({0, "a"})})}}})},
                                             {"light_table", Json::array({"b", "p"})}};
        // a selection kept from a (in a union too), one from a layer kept
        doc.edit_page(0).extra["saved_areas"] = Json{{"from_a", Json{{"layer", "a"}}},
                                                     {"both", Json{{"union", Json::array({Json{{"layer", ink}}, Json{{"layer", "b"}}})}}},
                                                     {"kept", Json{{"layer", ink}}}};
        doc = run(doc, Json{{"op", "set_variables"},
                            {"defs", Json::array({Json{{"name", "v"}, {"type", "visibility"}, {"page", 1}, {"layer", "a"}}})}});
        const core::ApplyResult done = run_ops(doc, op("delete_layers", {{"ids", Json::array({"g", "p", ink})}}));
        const core::Document& after = done.doc;
        for (const char* gone : {"g", "a", "b", "p"}) QVERIFY2(find(after, gone) == nullptr, gone);
        QVERIFY(find(after, ink) != nullptr);  // (a core layer: kept)
        const Json& report = done.results.back();
        QCOMPARE(report["deleted"].size(), std::size_t(4));
        QCOMPARE(report["kept"].size(), std::size_t(1));
        QCOMPARE(report["kept"][0]["id"], Json(ink));
        QCOMPARE(report["kept"][0]["why"], Json("core"));
        for (const core::StoryLine& line : after.story)
            if (line.id == "l1") QVERIFY2(!line.style.contains("below_layer"), line.style.dump().c_str());
        QCOMPARE(after.page(0).rulers[0]["layer_id"], Json(nullptr));
        // (the track of the folder g deleted goes; the other's cel of a emptied — the review's P3-3)
        QCOMPARE(after.page(0).extra["anim"]["tracks"].size(), std::size_t(1));
        QCOMPARE(after.page(0).extra["anim"]["tracks"][0]["folder"], Json("kept"));
        QCOMPARE(after.page(0).extra["anim"]["tracks"][0]["cels"][0][1], Json(nullptr));
        QCOMPARE(after.page(0).extra["saved_areas"], (Json{{"kept", Json{{"layer", ink}}}}));
        QCOMPARE(after.page(0).extra["anim"]["light_table"], Json::array());
        // (the comps and the variables keep naming them: they say so when applied — layer-comps@1, variables@1)
        QVERIFY(after.page(0).layer_comps && !after.page(0).layer_comps->comps.empty());
        QCOMPARE(core::variables_of(after).defs.front().layer, std::optional<std::string>("a"));
        // the notes and the measuring name no layer: as they were
        QCOMPARE(after.page(0).annotations.size(), doc.page(0).annotations.size());
        // written and read again: clean
        const fs::path dir = fs::path(path(QStringLiteral("deleted.genko")).toStdString());
        genko::test::write_project(after, dir);
        const auto back = storage::load_document(dir);
        QVERIFY2(back.report.clean(), back.report.to_json().dump().c_str());
        QVERIFY(find(back.document, "a") == nullptr);
        // a layer kept in a folder deleted goes up to the folder over it that stays
        core::Document nested = run(book(), Json::array({folder("outer"), folder("inner"), into({"inner"}, "outer"), paint("x"), into({"x"}, "inner"),
                                                         op("set_layer", {{"id", "x"}, {"lock_all", true}})}));
        const core::ApplyResult kept = run_ops(nested, op("delete_layers", {{"ids", Json::array({"inner"})}}));
        QVERIFY(find(kept.doc, "inner") == nullptr);
        QCOMPARE(find(kept.doc, "x")->parent_id, Json("outer"));
        QCOMPARE(kept.results.back()["kept"][0]["why"], Json("lock_all"));
        // (the review's P2-2: a folder locked so — itself and what it holds kept, said)
        const core::Document locked_folder = run(nested, Json::array({op("set_layer", {{"id", "x"}, {"lock_all", false}}), paint("y"),
                                                                       into({"y"}, "inner"), op("set_layer", {{"id", "inner"}, {"lock_all", true}})}));
        const core::ApplyResult whole = run_ops(locked_folder, op("delete_layers", {{"ids", Json::array({"inner"})}}));
        for (const char* stays : {"inner", "x", "y"}) QVERIFY2(find(whole.doc, stays) != nullptr, stays);
        QCOMPARE(whole.results.back()["kept"].size(), std::size_t(3));
        QCOMPARE(whole.results.back()["deleted"].size(), std::size_t(0));
        // (PC5 J-b, slices@1: a layer-based slice goes with its layer — delete_layers, ungroup_layers wrapped by follow_layers;
        // one of a layer kept stays)
        core::Document sliced = run(book(), Json::array({paint("s1"), drawn_in("s1", 5, 5, 20, 20, Json::array({0, 0, 0})), paint("s2"),
                                                         drawn_in("s2", 30, 30, 40, 40, Json::array({0, 0, 0})), op("add_slice", {{"layer", "s1"}}),
                                                         op("add_slice", {{"layer", "s2"}})}));
        QVERIFY(sliced.page(0).slices && sliced.page(0).slices->list.size() >= 2);
        const core::Document unsliced = run(sliced, op("delete_layers", {{"ids", Json::array({"s1"})}}));
        bool s1_slice = false, s2_slice = false;
        if (unsliced.page(0).slices) {
            for (const auto& s : unsliced.page(0).slices->list) {
                s1_slice = s1_slice || s.layer == std::optional<std::string>("s1");
                s2_slice = s2_slice || s.layer == std::optional<std::string>("s2");
            }
        }
        QVERIFY(!s1_slice && s2_slice);
        // refusals: nothing named; ids on every page; a layer the page does not have
        QVERIFY(refusal(doc, op("delete_layers")).find("needs ids, hidden or empty") != std::string::npos);
        QVERIFY(refusal(doc, Json{{"op", "delete_layers"}, {"page", "all"}, {"ids", Json::array({"a"})}}).find("one page") != std::string::npos);
        QVERIFY(refusal(doc, op("delete_layers", {{"ids", Json::array({"nope"})}})).find("no layer nope") != std::string::npos);
    }

    // Hidden layers, empty layers (an empty group too), every page; the core layers hidden kept.
    void hiddenAndEmptyOnEveryPage() {
        core::Document doc = book("8", 2);
        doc = run(doc, Json::array({paint("e1"), paint("full"), drawn_in("full", 5, 5, 20, 20, Json::array({0, 0, 0})), folder("eg"), paint("e2"),
                                    into({"e2"}, "eg"), paint("h"), drawn_in("h", 5, 5, 9, 9, Json::array({0, 0, 0})),
                                    op("set_layer", {{"id", "h"}, {"visible", false}}), paint("e3", 2)}));
        const std::string ink = core_of(doc, core::LayerRole::Ink)->id;
        doc = run(doc, op("set_layer", {{"id", ink}, {"visible", false}}));
        const core::ApplyResult hidden = run_ops(doc, op("delete_layers", {{"hidden", true}}));
        QVERIFY(find(hidden.doc, "h") == nullptr);
        QVERIFY(find(hidden.doc, ink) != nullptr);
        QVERIFY(find(hidden.doc, "e1") != nullptr);
        const core::ApplyResult empty = run_ops(doc, Json{{"op", "delete_layers"}, {"page", "all"}, {"empty", true}});
        for (const char* gone : {"e1", "eg", "e2"}) QVERIFY2(find(empty.doc, gone) == nullptr, gone);
        QVERIFY(find(empty.doc, "full") != nullptr && find(empty.doc, "h") != nullptr);
        QVERIFY(find(empty.doc, "e3", 1) == nullptr);  // (page 2 too)
        QVERIFY(core::empty_layer(doc.page(0).layers, position(doc, "eg")));
        QVERIFY(!core::empty_layer(doc.page(0).layers, position(doc, "full")));
    }

    // A group duplicated with what it holds, in its folders as it was; onto another page of the same paper; refused onto
    // another paper or mode.
    void duplicatingGroups() {
        core::Document doc = book("8", 2);
        doc = run(doc, Json::array({folder("g", {{"name", "G"}}), paint("a"), paint("b"), into({"a", "b"}, "g"), op("set_layer", {{"id", "a"}, {"name", "A"}})}));
        const core::ApplyResult made = run_ops(doc, op("duplicate_layers", {{"ids", Json::array({"g"})}, {"new_ids", Json{{"g", "g2"}}}}));
        const core::Document& after = made.doc;
        QCOMPARE(made.results.back()["ids"].size(), std::size_t(3));
        const core::Layer* copy = find(after, "g2");
        QVERIFY(copy != nullptr && copy->group);
        QCOMPARE(copy->title, std::string("G のコピー"));
        QCOMPARE(position(after, "g2"), position(after, "g") + 3);  // (just over the group, what it holds under its copy)
        std::size_t inside = 0;
        for (const core::Layer& l : after.page(0).layers) {
            if (l.parent_id == Json("g2")) {
                ++inside;
                QVERIFY(l.id != "a" && l.id != "b");
                if (l.title == "A") QVERIFY(true);
            }
        }
        QCOMPARE(inside, std::size_t(2));
        QCOMPARE(after.page(0).layers.size(), doc.page(0).layers.size() + 3);
        // onto page 2: on its top, in no folder of it
        const core::Document there = run(doc, op("duplicate_layers", {{"ids", Json::array({"g"})}, {"to_page", 2}, {"new_ids", Json{{"g", "g3"}}}}));
        QVERIFY(find(there, "g3", 1) != nullptr);
        QCOMPARE(find(there, "g3", 1)->parent_id, Json(nullptr));
        QCOMPARE(position(there, "g3", 1), there.page(1).layers.size() - 1);
        QCOMPARE(there.page(0).layers.size(), doc.page(0).layers.size());
        // (the review's P2-4) the page copied onto locked by another — its number written 2, 2.0 or "2" —: refused; in a book
        // of strict gates, onto a page whose name is not approved: refused, the page copied from approved or not
        core::Document held = doc;
        held.page_locks[held.page(1).id] = "ai:other";
        for (const Json& to : {Json(2), Json(2.0), Json("2")}) {
            const std::string why = refusal(held, op("duplicate_layers", {{"ids", Json::array({"a"})}, {"to_page", to}}));
            QVERIFY2(why.find("locked by ai:other") != std::string::npos, (to.dump() + " → " + why).c_str());
        }
        core::Document strict = doc;
        strict.strict_gates = true;
        strict.edit_page(0).name_ok = true;
        const std::string onto = refusal(strict, op("duplicate_layers", {{"ids", Json::array({"a"})}, {"to_page", "2"}}));
        QVERIFY2(onto.find("name_ok") != std::string::npos, onto.c_str());
        strict.edit_page(1).name_ok = true;
        QVERIFY(refusal(strict, op("duplicate_layers", {{"ids", Json::array({"a"})}, {"to_page", "2"}})).empty());
        // (the review's P3-6: deleting the empty layers of a page whose name is not approved — none printed taken: let)
        core::Document strict_empty = run(doc, paint("e"));
        strict_empty.strict_gates = true;
        QVERIFY(refusal(strict_empty, op("delete_layers", {{"hidden", true}})).empty());
        const std::string empty_why = refusal(strict_empty, op("delete_layers", {{"empty", true}}));
        QVERIFY2(empty_why.find("name_ok") != std::string::npos, empty_why.c_str());  // (e is empty and printed)
        // another mode: refused
        const core::Document other = run(doc, Json{{"op", "set_color_mode"}, {"page", 2}, {"mode", "rgb"}, {"depth", 16}});
        QVERIFY(refusal(other, op("duplicate_layers", {{"ids", Json::array({"g"})}, {"to_page", 2}})).find("another paper size or colour mode") !=
                std::string::npos);
    }

    // Ungroup: what the group holds out where it is (in the folder over it), the group gone, one step.
    void ungroupingInOneStep() {
        core::Document doc = run(book(), Json::array({folder("outer"), folder("g"), into({"g"}, "outer"), paint("a"), paint("b"), into({"a", "b"}, "g"),
                                                      op("set_layer", {{"id", "g"}, {"opacity", 0.5}})}));
        const std::size_t a_at = position(doc, "a"), b_at = position(doc, "b");
        const core::ApplyResult out = run_ops(doc, op("ungroup_layers", {{"id", "g"}}));
        QVERIFY(find(out.doc, "g") == nullptr);
        QCOMPARE(find(out.doc, "a")->parent_id, Json("outer"));
        QCOMPARE(find(out.doc, "b")->parent_id, Json("outer"));
        QCOMPARE(find(out.doc, "a")->opacity, 1.0);  // (the group's opacity not moved into what it held)
        QVERIFY(position(out.doc, "a") < position(out.doc, "b") && position(out.doc, "a") == a_at && position(out.doc, "b") == b_at);
        QCOMPARE(out.applied.size(), std::size_t(1));
        QVERIFY(refusal(doc, op("ungroup_layers", {{"id", "a"}})).find("not a group or folder") != std::string::npos);
    }

    // すべてをロック: nothing drawn on it, moved, changed or deleted; its visibility, name and order may change; a page op works.
    void lockAllKeepsTheLayer() {
        core::Document doc = run(book(), Json::array({paint("p"), drawn_in("p", 5, 5, 20, 20, Json::array({0, 0, 0})), paint("q"),
                                                      op("set_layer", {{"id", "p"}, {"lock_all", true}})}));
        QVERIFY(find(doc, "p")->lock_all && !find(doc, "p")->locked);  // (its other locks as they were)
        for (const Json& change : {drawn_in("p", 30, 30, 40, 40, Json::array({0, 0, 0})), op("set_layer", {{"id", "p"}, {"opacity", 0.5}}),
                                   op("set_layer", {{"id", "p"}, {"blend", "multiply"}}), op("delete_layer", {{"id", "p"}}),
                                   op("transform_layers", {{"ids", Json::array({"p"})}, {"move", Json::array({3, 0})}}),
                                   op("set_layer_mask", {{"id", "p"}, {"fill", "hide"}})}) {
            const std::string why = refusal(doc, change);
            QVERIFY2(why.find("lock_all") != std::string::npos, (change.dump() + " → " + why).c_str());
        }
        for (const Json& change : {op("set_layer", {{"id", "p"}, {"visible", false}}), op("set_layer", {{"id", "p"}, {"name", "P"}}),
                                   op("move_layers", {{"ids", Json::array({"p"})}, {"after", "q"}}), op("set_layer", {{"id", "q"}, {"opacity", 0.5}}),
                                   op("resize_pages", {{"width_px", 300}, {"height_px", 400}, {"resample", "nearest"}})}) {
            QVERIFY2(refusal(doc, change).empty(), (change.dump() + " → " + refusal(doc, change)).c_str());
        }
        const core::Document open = run(doc, op("set_layer", {{"id", "p"}, {"lock_all", false}}));
        QVERIFY(refusal(open, op("set_layer", {{"id", "p"}, {"opacity", 0.5}})).empty());
        // (one of the three locks taken off — as a client knowing only them unlocks a PSD's layer locked all — takes すべて off)
        QVERIFY(!find(run(doc, op("set_layer", {{"id", "p"}, {"locked", false}})), "p")->lock_all);
        QVERIFY(find(run(doc, op("set_layer", {{"id", "p"}, {"locked", true}})), "p")->lock_all);
        // set_layers' レイヤーをロック… too, and lock_nest kept
        const core::Document both = run(doc, op("set_layers", {{"ids", Json::array({"q"})}, {"lock_all", true}, {"lock_nest", true}}));
        QVERIFY(find(both, "q")->lock_all && find(both, "q")->lock_nest);
    }

    // The review's P2-1, P2-2 and P2-5: a core layer locked so not merged away (the image flattened, its layers merged: as
    // locked); a folder locked so locks what it holds (drawn on, changed, taken out of it, deleted with it: refused); the ops
    // that go over every layer skip those locked so; the ops on whole pages — flatten, HDR toning, a text style of the book —
    // as Photoshop's.
    void lockAllAcrossOps() {
        core::Document doc = run(book(), Json::array({paint("p"), drawn_in("p", 5, 5, 20, 20, Json::array({0, 0, 0})), paint("q"),
                                                      drawn_in("q", 30, 30, 40, 40, Json::array({0, 0, 255}))}));
        const std::string ink = core_of(doc, core::LayerRole::Ink)->id;
        const core::Document inked = run(doc, Json::array({drawn_in(ink, 10, 50, 20, 60, Json::array({0, 0, 0})), op("set_layer", {{"id", ink}, {"lock_all", true}})}));
        // a merge — the image flattened, layers merged, merged down — of a layer locked so: refused, its pixels and lock kept
        for (const Json& merge : {op("merge_visible", {{"copy", false}, {"flatten", true}}), op("merge_layers", {{"ids", Json::array({ink, "q"})}})}) {
            const std::string why = refusal(inked, merge);
            QVERIFY2(why.find("lock_all") != std::string::npos, (merge.dump() + " → " + why).c_str());
        }
        const core::Document p_locked = run(doc, op("set_layer", {{"id", "p"}, {"lock_all", true}}));
        QVERIFY(refusal(p_locked, op("merge_down", {{"id", "q"}})).find("lock_all") != std::string::npos);
        // a layer made anew under its id by another op than set_layer: as it was locked (the bus' look compared)
        {
            core::Document again = p_locked;
            const std::vector<core::LockedLayer> before = core::locked_layers(again);
            core::Layer fresh;
            fresh.id = "p";
            fresh.kind = core::LayerKind::Raster;
            for (core::Layer& l : again.edit_page(0).layers)
                if (l.id == "p") l = fresh;
            bool refused = false;
            try {
                core::check_locked_layers(before, again, op("merge_visible", {{"flatten", true}}));
            } catch (const core::OpError&) {
                refused = true;
            }
            QVERIFY(refused);
            bool let = true;
            try {
                core::check_locked_layers(before, again, op("set_layer", {{"id", "p"}, {"lock_all", false}}));
            } catch (const core::OpError&) {
                let = false;
            }
            QVERIFY(let);
        }
        // a folder locked so: what it holds locked too
        core::Document folded = run(doc, Json::array({folder("g"), into({"p"}, "g"), op("set_layer", {{"id", "g"}, {"lock_all", true}})}));
        QVERIFY(core::locked_all(folded.page(0).layers, position(folded, "p")));
        QVERIFY(!core::locked_all(folded.page(0).layers, position(folded, "q")));
        for (const Json& change : {drawn_in("p", 30, 30, 40, 40, Json::array({0, 0, 0})), op("set_layer", {{"id", "p"}, {"opacity", 0.5}}),
                                   op("move_layers", {{"ids", Json::array({"p"})}, {"parent", nullptr}}), op("delete_layer", {{"id", "p"}}),
                                   op("ungroup_layers", {{"id", "g"}})}) {
            const std::string why = refusal(folded, change);
            QVERIFY2(why.find("lock_all") != std::string::npos, (change.dump() + " → " + why).c_str());
        }
        QVERIFY(refusal(folded, op("set_layer", {{"id", "p"}, {"visible", false}})).empty());
        const core::Document opened = run(folded, op("set_layer", {{"id", "g"}, {"lock_all", false}}));
        QVERIFY(refusal(opened, op("set_layer", {{"id", "p"}, {"opacity", 0.5}})).empty());
        // the ops that go over every layer: those locked so skipped as those locked
        const Json style{{"effects", Json::array({Json{{"kind", "stroke"}, {"enabled", true}, {"blend", "normal"}, {"opacity", 1}, {"size_mm", 1},
                                                       {"position", "outside"}, {"rgb", Json::array({255, 0, 0})}}})}};
        const core::Document styled = run(doc, Json::array({op("set_layer_style", {{"id", "p"}, {"style", style}}), op("set_layer_style", {{"id", "q"}, {"style", style}}),
                                                            op("set_layer", {{"id", "p"}, {"lock_all", true}})}));
        const core::ApplyResult hidden = run_ops(styled, op("show_layer_effects", {{"show", false}}));
        QCOMPARE(hidden.results.back()["ids"], Json::array({"q"}));
        const core::Document smart = run(doc, Json::array({fill("f", Json::array({0, 255, 0})), fill("f2", Json::array({0, 0, 255})),
                                                           op("set_layer", {{"id", "f"}, {"lock_all", true}})}));
        const core::Document rasterized = run(smart, op("rasterize_layer", {{"all", true}}));
        QVERIFY(find(rasterized, "f")->kind == core::LayerKind::Fill);
        QVERIFY(find(rasterized, "f2")->kind != core::LayerKind::Fill);
        // the page ops: a page flattened (HDR toning) refuses a layer locked so as one locked (the merge's own check, as
        // before for locked); one that does not merge — the page's size — works on it (the bus lets it through)
        const std::string hdr = refusal(p_locked, op("hdr_toning", {{"strength", 1.5}}));
        QVERIFY2(hdr.find("lock_all") != std::string::npos, hdr.c_str());
        const std::string hdr_locked = refusal(run(doc, op("set_layer", {{"id", "p"}, {"locked", true}})), op("hdr_toning", {{"strength", 1.5}}));
        QVERIFY2(hdr_locked.find("is locked") != std::string::npos, hdr_locked.c_str());
        QVERIFY(refusal(p_locked, op("resize_pages", {{"width_px", 300}, {"height_px", 400}, {"resample", "nearest"}})).empty());
        const core::Document ruled = run(p_locked, op("set_ruler_line", {{"start", {10, 40}}, {"end", {50, 36}}}));
        QVERIFY2(refusal(ruled, op("straighten_layer", {{"crop", true}, {"trim", "fit"}})).empty(), refusal(ruled, op("straighten_layer", {{"crop", true}, {"trim", "fit"}})).c_str());
        const std::string turned = refusal(ruled, op("straighten_layer", {{"ids", Json::array({"p"})}}));  // (the layer itself turned: refused)
        QVERIFY2(turned.find("lock_all") != std::string::npos, turned.c_str());
    }

    // The channels a layer leaves out: what is under it kept there — 8 bits, precise colour, inks; whole, a part, the command
    // line; merged (8 bits), precise merges refused in words.
    void channelsLeftOut() {
        // 8 bits: a blue layer over the white paper, red left out: magenta (the paper's red kept)
        core::Document eight = run(book("8"), Json::array({fill("blue", Json::array({0, 0, 255})), op("set_layer", {{"id", "blue"}, {"blend_channels", Json::array({false, true, true})}})}));
        const render::Image whole = drawn(eight);
        QVERIFY2(near(rgb_at(whole, 30, 40), {255, 0, 255}), shown(rgb_at(whole, 30, 40)).constData());
        // a part drawn as the whole cut there
        render::RenderOptions part = proof();
        part.region = render::RenderRegion{20, 30, 30, 30};
        const render::Image cut = render::render_page(eight.page(0), 50, part, &eight).image.convert("RGB");
        QCOMPARE(cut.tobytes(), whole.crop(render::Box{20, 30, 50, 60}).tobytes());
        // green left out on a 16-bit page: cyan
        core::Document deep = run(book("16"), Json::array({fill("blue", Json::array({0, 0, 255})), op("set_layer", {{"id", "blue"}, {"blend_channels", Json::array({true, false, true})}})}));
        QVERIFY2(near(rgb_at(drawn(deep), 30, 40), {0, 255, 255}), shown(rgb_at(drawn(deep), 30, 40)).constData());
        // inks: a black layer, K left out — its K plate the paper's (none), its C, M, Y the black's
        core::Document inks = run(book("cmyk"), fill("black", Json::array({0, 0, 0})));
        render::RenderOptions with_inks = proof();
        with_inks.inks = true;
        const core::InkPixels all_on = *render::render_page(inks.page(0), 50, with_inks, &inks).inks;
        inks = run(inks, op("set_layer", {{"id", "black"}, {"blend_channels", Json::array({true, true, true, false})}}));
        const core::InkPixels k_off = *render::render_page(inks.page(0), 50, with_inks, &inks).inks;
        const std::size_t i = std::size_t(40) * k_off.width + 30;
        QVERIFY2(all_on.value(i, 3) > 0.5, std::to_string(all_on.value(i, 3)).c_str());
        QVERIFY2(k_off.value(i, 3) < 0.01, std::to_string(k_off.value(i, 3)).c_str());
        for (unsigned c = 0; c < 3; ++c) QVERIFY2(std::abs(k_off.value(i, c) - all_on.value(i, c)) < 0.01, std::to_string(c).c_str());
        // through the command line: `genko render` the same picture
        const QString dir = path(QStringLiteral("channels.genko"));
        genko::test::write_project(eight, fs::path(dir.toStdString()));
        const QString out = path(QStringLiteral("channels.png"));
        const auto ran = genko::test::run_genko({QStringLiteral("render"), dir, QStringLiteral("--page"), QStringLiteral("1"), QStringLiteral("--dpi"),
                                                 QStringLiteral("50"), QStringLiteral("--mode"), QStringLiteral("proof"), QStringLiteral("--out"), out});
        QVERIFY2(ran.exit_code == 0, ran.err.constData());
        const render::Image cli = render::read_png(genko::test::read_bytes(out)).convert("RGB");
        QVERIFY2(near(rgb_at(cli, 30, 40), {255, 0, 255}), shown(rgb_at(cli, 30, 40)).constData());
        // merged in 8 bits as the page draws it: a copy of what shows over a white layer the same (over nothing, a channel
        // left out is nothing's, as Photoshop's), the image flattened over the paper the same
        core::Document over_white = run(book("8"), Json::array({fill("white", Json::array({255, 255, 255})), fill("blue", Json::array({0, 0, 255})),
                                                                op("set_layer", {{"id", "blue"}, {"blend_channels", Json::array({false, true, true})}})}));
        core::Document merged = run(over_white, op("merge_visible", {{"copy", true}, {"id", "m"}}));
        merged = run(merged, Json::array({op("set_layer", {{"id", "blue"}, {"visible", false}}), op("set_layer", {{"id", "white"}, {"visible", false}})}));
        QVERIFY2(near(rgb_at(drawn(merged), 30, 40), {255, 0, 255}), shown(rgb_at(drawn(merged), 30, 40)).constData());
        const core::Document flat = run(eight, op("merge_visible", {{"copy", false}, {"flatten", true}}));
        QVERIFY2(near(rgb_at(drawn(flat), 30, 40), {255, 0, 255}), shown(rgb_at(drawn(flat), 30, 40)).constData());
        // precise (the review's P3-2): merged as the page draws them, in its values — a copy of what shows over a white layer,
        // the image flattened, the layers merged; a clipped first layer refused in words
        core::Document deep_white = book("8");
        add_pixels(deep_white, "dot", "dot", 400, 600, 410, 610, {0, 0, 0});  // (precise pixels under the rest, away from the points)
        deep_white = run(deep_white, Json::array({fill("white", Json::array({255, 255, 255})), Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}},
                                                  fill("blue", Json::array({0, 0, 255})),
                                                  op("set_layer", {{"id", "blue"}, {"blend_channels", Json::array({false, true, true})}})}));
        QVERIFY(find(deep_white, "dot")->color_raster != nullptr);
        const std::array<int, 3> shows = rgb_at(drawn(deep_white), 30, 40);
        QVERIFY2(near(shows, {255, 0, 255}), shown(shows).constData());
        core::Document deep_copy = run(deep_white, op("merge_visible", {{"copy", true}, {"id", "m"}}));
        QVERIFY(find(deep_copy, "m")->color_raster != nullptr);
        deep_copy = run(deep_copy, Json::array({op("set_layer", {{"id", "blue"}, {"visible", false}}), op("set_layer", {{"id", "white"}, {"visible", false}}),
                                                op("set_layer", {{"id", "dot"}, {"visible", false}})}));
        QVERIFY2(near(rgb_at(drawn(deep_copy), 30, 40), shows), shown(rgb_at(drawn(deep_copy), 30, 40)).constData());
        const core::Document deep_flat = run(deep, op("merge_visible", {{"copy", false}, {"flatten", true}}));
        QVERIFY2(near(rgb_at(drawn(deep_flat), 30, 40), {0, 255, 255}), shown(rgb_at(drawn(deep_flat), 30, 40)).constData());
        const core::Document deep_merged = run(deep_white, op("merge_layers", {{"ids", Json::array({"white", "blue"})}}));
        QVERIFY2(near(rgb_at(drawn(deep_merged), 30, 40), shows), shown(rgb_at(drawn(deep_merged), 30, 40)).constData());
        const core::Document clipped = run(deep_white, op("set_layer", {{"id", "blue"}, {"clip", true}}));
        const std::string why = refusal(clipped, op("merge_down", {{"id", "blue"}}));
        QVERIFY2(why.empty(), why.c_str());  // (merged down: the lower its own, the upper clipped to it — not clipped to what is under)
        const core::Document lowest_clipped = run(deep_white, Json::array({op("set_layer", {{"id", "white"}, {"clip", true}}),
                                                                           op("set_layer", {{"id", "white"}, {"blend_channels", Json::array({true, false, true})}})}));
        const std::string clip_why = refusal(lowest_clipped, op("merge_layers", {{"ids", Json::array({"white", "blue"})}}));
        QVERIFY2(clip_why.find("clipped layer that leaves colour channels out") != std::string::npos, clip_why.c_str());
        // the op's checks: a flag a channel, a page whose mode has them, a layer's (not a folder's); all on is none
        QVERIFY(refusal(eight, op("set_layer", {{"id", "blue"}, {"blend_channels", Json::array({true, false})}})).find("3 flags") != std::string::npos);
        const core::Document folded = run(eight, folder("f"));
        QVERIFY(refusal(folded, op("set_layer", {{"id", "f"}, {"blend_channels", Json::array({false, true, true})}})).find("not a folder") != std::string::npos);
        QVERIFY(run(eight, op("set_layer", {{"id", "blue"}, {"blend_channels", Json::array({true, true, true})}})).page(0).layers.back().blend_channels.empty());
        // a page's mode changed: its layers blend every channel again
        QVERIFY(find(run(eight, Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "gray"}}), "blue")->blend_channels.empty());
    }

    // bake_layer_styles: the layer with its style drawn into its pixels — the page looks the same; its opacity and blend kept.
    void bakingLayerStyles() {
        for (const std::string way : {"8", "16", "16-precise", "cmyk"}) {
            const Json style{{"effects", Json::array({Json{{"kind", "stroke"}, {"enabled", true}, {"blend", "normal"}, {"opacity", 1}, {"size_mm", 2},
                                                           {"position", "outside"}, {"rgb", Json::array({255, 0, 0})}}})}};
            // (the review's P2-3: a page of 16 bits holding no precise pixels is drawn in 8 bits — the layer baked into 8-bit
            // pixels, so that it is still; one holding some, in its linear light — baked into precise pixels)
            core::Document base = book(way == "16-precise" ? "8" : way);
            if (way == "16-precise") {
                add_pixels(base, "dot", "dot", 400, 600, 410, 610, {0, 0, 0});
                base = run(base, Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}});
                QVERIFY(find(base, "dot")->color_raster != nullptr);
            }
            core::Document doc = run(base, Json::array({paint("p"), drawn_in("p", 20, 20, 40, 60, Json::array({0, 0, 255})),
                                                             op("set_layer_style", {{"id", "p"}, {"style", style}}),
                                                             op("set_layer", {{"id", "p"}, {"opacity", 0.8}})}));
            const render::Image before = drawn(doc, 200);
            const core::ApplyResult baked = run_ops(doc, Json{{"op", "bake_layer_styles"}, {"page", "all"}});
            const core::Layer* p = find(baked.doc, "p");
            QVERIFY2(!p->style, way.c_str());
            if (way == "16") QVERIFY(p->color_raster == nullptr && p->raster_png != nullptr);
            if (way == "16-precise") QVERIFY(p->color_raster != nullptr);
            QCOMPARE(p->opacity, 0.8);
            QCOMPARE(baked.results.back()["baked"].size(), std::size_t(1));
            const render::Image after = drawn(baked.doc, 200);
            for (const auto& [x, y] : {std::array<int, 2>{120, 160}, {84, 160}, {152, 320}, {232, 160}, {20, 20}}) {
                QVERIFY2(near(rgb_at(after, x, y), rgb_at(before, x, y), 6),
                         (way + " " + std::to_string(x) + "," + std::to_string(y) + ": " + shown(rgb_at(after, x, y)).toStdString() + " / " +
                          shown(rgb_at(before, x, y)).toStdString())
                             .c_str());
            }
        }
        // a folder and a locked layer left, said
        core::Document doc = run(book(), Json::array({paint("p"), op("set_layer", {{"id", "p"}, {"effect", Json{{"border", Json{{"width_mm", 1}}}}}}),
                                                      op("set_layer", {{"id", "p"}, {"locked", true}})}));
        const core::ApplyResult left = run_ops(doc, op("bake_layer_styles", {{"ids", Json::array({"p"})}}));
        QCOMPARE(left.results.back()["skipped"][0]["why"], Json("locked"));
    }

    // PSD: すべてをロック (bit 31), the auto-nesting lock (bit 4) and the channels left out (brst) as psd-tools reads them, read
    // back as the layers'.
    void psdLocksAndChannels() {
        core::Document doc = book();
        add_pixels(doc, "p", "P", 40, 40, 160, 160, {0, 0, 0});
        add_pixels(doc, "c", "C", 200, 200, 400, 400, {0, 0, 255});
        doc = run(doc, Json::array({op("set_layer", {{"id", "p"}, {"lock_all", true}, {"lock_nest", true}}),
                                    op("set_layer", {{"id", "c"}, {"blend_channels", Json::array({false, true, false})}})}));
        const QString file = path(QStringLiteral("basics.psd"));
        (void)formats::export_page_psd(doc, doc.page(0), fs::path(file.toStdString()), 100);
        const Json view = psd_view(file);
        QVERIFY2(!view.contains("error"), view.dump().c_str());
        bool saw_p = false, saw_c = false;
        for (const Json& l : view["layers"]) {
            if (l["name"] == Json("P")) {
                saw_p = true;
                QCOMPARE(l["lspf"].get<std::int64_t>(), std::int64_t(0x80000010));
            }
            if (l["name"] == Json("C")) {
                saw_c = true;
                QCOMPARE(l["brst"], Json::array({0, 2}));
            }
        }
        QVERIFY2(saw_p && saw_c, view.dump().c_str());
        const formats::Opened opened = formats::open_file(genko::test::read_bytes(file), "basics.psd");
        const core::Layer* p = nullptr;
        const core::Layer* c = nullptr;
        for (const core::Layer& l : opened.doc.page(0).layers) {
            if (l.title == "P") p = &l;
            if (l.title == "C") c = &l;
        }
        QVERIFY(p != nullptr && c != nullptr);
        QVERIFY(p->lock_all && p->lock_nest);
        QCOMPARE(c->blend_channels, (std::vector<bool>{false, true, false}));
        QVERIFY(!c->native_psd || std::none_of(c->native_psd->blocks.begin(), c->native_psd->blocks.end(), [](const auto& b) { return b.first == "brst"; }));
        // written again from what was read: the same bits
        const QString again = path(QStringLiteral("again.psd"));
        (void)formats::export_page_psd(opened.doc, opened.doc.page(0), fs::path(again.toStdString()), 100);
        const Json view2 = psd_view(again);
        for (const Json& l : view2["layers"]) {
            if (l["name"] == Json("P")) QCOMPARE(l["lspf"].get<std::int64_t>(), std::int64_t(0x80000010));
            if (l["name"] == Json("C")) QCOMPARE(l["brst"], Json::array({0, 2}));
        }
        // a page without them: no lspf, no brst written
        const QString plain = path(QStringLiteral("plain.psd"));
        core::Document none = book();
        add_pixels(none, "p", "P", 40, 40, 160, 160, {0, 0, 0});
        (void)formats::export_page_psd(none, none.page(0), fs::path(plain.toStdString()), 100);
        for (const Json& l : psd_view(plain)["layers"]) QVERIFY2(l["lspf"].is_null() && l["brst"].is_null(), l.dump().c_str());
    }

    // .pcraft: the locks (all, artboard) and the excluded channels written and read back.
    void pcraftLocksAndChannels() {
        core::Document doc = book();
        add_pixels(doc, "p", "P", 40, 40, 160, 160, {0, 0, 0});
        doc = run(doc, Json::array({op("set_layer", {{"id", "p"}, {"blend_channels", Json::array({true, false, true})}}),
                                    op("set_layer", {{"id", "p"}, {"lock_all", true}, {"lock_nest", true}})}));
        std::vector<std::string> notes;
        const std::string bytes = formats::pcraft_bytes(doc, 0, {}, &notes);
        const formats::Opened opened = formats::open_file(bytes, "basics.pcraft");
        const core::Layer* p = nullptr;
        for (const core::Layer& l : opened.doc.page(0).layers)
            if (l.kind == core::LayerKind::Raster && l.lock_all) p = &l;
        QVERIFY(p != nullptr);
        QVERIFY(p->lock_nest);
        QCOMPARE(p->blend_channels, (std::vector<bool>{true, false, true}));
        // (the review's P3-7) a value not bits, or bits past the page's channels: opened, left out and said
        const auto with_excluded = [&bytes](const Json& value) {
            const render::zip::NamedReader reader(bytes);
            Json manifest = Json::parse(reader.read("manifest.json", 1u << 26));
            for (Json& layer : manifest["document"]["layers"])
                if (layer.contains("excluded_channels") && layer["excluded_channels"] != Json(0)) layer["excluded_channels"] = value;
            std::string out;
            render::zip::StoreWriter writer([&out](std::string_view b) { out.append(b); });
            writer.add("manifest.json", manifest.dump());
            for (const std::string& name : reader.names())
                if (name != "manifest.json") writer.add(name, reader.read(name, ~0ull));
            writer.finish();
            return out;
        };
        for (const Json& broken : {Json("R"), Json(-2), Json(2.5)}) {
            const formats::Opened read = formats::open_file(with_excluded(broken), "broken.pcraft");
            const bool said = std::any_of(read.notes.begin(), read.notes.end(), [](const std::string& n) { return n.find("チャンネルの指定が読めない") != std::string::npos; });
            QVERIFY2(said, broken.dump().c_str());
            for (const core::Layer& l : read.doc.page(0).layers) QVERIFY(l.blend_channels.empty());
        }
        const formats::Opened past = formats::open_file(with_excluded(Json(2 | 16)), "past.pcraft");
        QVERIFY(std::any_of(past.notes.begin(), past.notes.end(), [](const std::string& n) { return n.find("無いチャンネルの指定") != std::string::npos; }));
        bool green_out = false;
        for (const core::Layer& l : past.doc.page(0).layers) green_out = green_out || l.blend_channels == std::vector<bool>{true, false, true};
        QVERIFY(green_out);
    }

    // A book without them as before: no feature, no key, written twice the same; with them named, read back; a broken value
    // read-only.
    void booksWithoutThemAsBefore() {
        const core::Document plain = run(book(), Json::array({folder("g"), paint("a"), into({"a"}, "g"), op("set_layer", {{"id", "a"}, {"locked", true}, {"lock_position", true}})}));
        const fs::path one = fs::path(path(QStringLiteral("plain1.genko")).toStdString());
        const fs::path two = fs::path(path(QStringLiteral("plain2.genko")).toStdString());
        genko::test::write_project(plain, one);
        genko::test::write_project(storage::load_document(one).document, two);
        const std::string text = genko::test::read_bytes(QString::fromStdString((one / "project.json").string()));
        for (const char* absent : {"layer-locks@1", "blend-channels@1", "\"lock_all\"", "\"lock_nest\"", "\"blend_channels\""})
            QVERIFY2(text.find(absent) == std::string::npos, absent);
        QCOMPARE(genko::test::read_bytes(QString::fromStdString((two / "project.json").string())), text);
        const core::Document with = run(plain, Json::array({op("set_layer", {{"id", "a"}, {"blend_channels", Json::array({false, true, true})}}),
                                                            op("set_layer", {{"id", "a"}, {"lock_all", true}})}));
        const fs::path three = fs::path(path(QStringLiteral("with.genko")).toStdString());
        genko::test::write_project(with, three);
        Json project = genko::test::read_json(QString::fromStdString((three / "project.json").string()));
        for (const char* named : {"layer-locks@1", "blend-channels@1"})
            QVERIFY2(std::find(project["features"].begin(), project["features"].end(), Json(named)) != project["features"].end(), named);
        const auto back = storage::load_document(three);
        QVERIFY2(back.report.clean(), back.report.to_json().dump().c_str());
        QVERIFY(find(back.document, "a")->lock_all);
        QCOMPARE(find(back.document, "a")->blend_channels, (std::vector<bool>{false, true, true}));
        // broken: two flags on an RGB page — read-only with its reason
        for (Json& layer : project["pages"][0]["layers"])
            if (layer.value("id", std::string()) == "a") layer["blend_channels"] = Json::array({false, true});
        genko::test::write_bytes(QString::fromStdString((three / "project.json").string()), project.dump());
        const auto broken = storage::load_document(three);
        QVERIFY(!broken.report.clean());
    }
};

QTEST_GUILESS_MAIN(TestContractLayers)
#include "test_contract_layers.moc"
