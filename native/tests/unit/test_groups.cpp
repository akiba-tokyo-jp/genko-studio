// PC5 I-b (groups@1, SPEC.md AUTO-01「グループ」, core/groups.hpp): a folder drawn as Photoshop's group, as PhotoCraft's
// compose/src/tests.rs (137-170) tests it — pass-through against isolated, the opacity once, a pass-through group's opacity
// mixing — on Genko's three ways (8 bits, precise, inks): what a hidden group holds (nested too), its mask, a layer
// clipped to a pass-through group, the tree of the page's layers (core::group_steps), the ops' checks (group only on a
// folder, pass_through a group's), Frame from Layers (frame_from_layers) and its undo, the book written and read again
// (groups@1), a page drawn in tiles the same as in one piece, and a folder that is not a group drawn as before — the
// same pixels as the page without it, the same bytes written.

#include <QtTest>
#include <QElapsedTimer>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "core/base64.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/groups.hpp"
#include "formats/export.hpp"
#include "formats/open_file.hpp"
#include "render/image.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "storage/reader.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;

namespace {

core::ApplyResult run_ops(const core::Document& doc, const Json& ops) {
    return core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor("human:test"));
}
core::Document run(const core::Document& doc, const Json& ops) { return run_ops(doc, ops.is_array() ? ops : Json::array({ops})).doc; }

std::string refusal(const core::Document& doc, const Json& op) {
    try {
        run_ops(doc, Json::array({op}));
    } catch (const core::ApplyError& error) {
        return error.code() + ": " + error.what();
    }
    return {};
}

// A book of one page 60 × 80 mm, its panels taken away: "8" (8 bits, colour), "16" (precise RGB) or "cmyk" (inks).
core::Document book(const std::string& way = "8") {
    core::PageSpec spec;
    spec.width_mm = core::Num(60);
    spec.height_mm = core::Num(80);
    spec.dpi = core::Num(150);
    spec.bleed_mm = core::Num(3);
    spec.inner_margin_mm = core::Num(5);
    spec.expression = "color";
    core::Document doc = core::new_episode("gr", core::Num(1), 1, spec, core::Binding::Right);
    doc.edit_page(0).frames.clear();
    if (way == "16") doc = run(doc, Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}});
    if (way == "cmyk") doc = run(doc, Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}, {"depth", 16}});
    return doc;
}

Json fill(const std::string& id, int grey, const std::string& blend = "normal") {
    return Json{{"op", "add_layer"}, {"page", 1}, {"kind", "fill"}, {"id", id}, {"rgb", Json::array({grey, grey, grey})}, {"blend", blend}};
}

// The page drawn (proof, no monochrome finish) at 50 dpi; one pixel's grey (its red).
render::Image drawn(const core::Document& doc, int dpi = 50) {
    render::RenderOptions options;
    options.mode = "proof";
    options.finish = false;
    return render::render_page(doc.page(0), dpi, options, &doc).image.convert("RGB");
}
int grey_at(const core::Document& doc, int x = 30, int y = 40) { return drawn(doc).getpixel(x, y)[0]; }

// base grey 128, then a group holding a grey 128 in `blend` (the group's own settings `group`)
core::Document grouped(const std::string& way, const Json& group, const std::string& child_blend = "multiply", int child = 128, bool base = true) {
    Json ops = Json::array();
    if (base) ops.push_back(fill("base", 128));
    Json folder{{"op", "add_layer"}, {"page", 1}, {"kind", "folder"}, {"id", "g"}, {"name", "グループ"}};
    Json settings{{"op", "set_layer"}, {"page", 1}, {"id", "g"}};  // (add_layer takes no opacity or visibility)
    for (const auto& [key, value] : group.items()) (key == "group" || key == "blend" ? folder : settings)[key] = value;
    ops.push_back(folder);
    ops.push_back(fill("child", child, child_blend));
    ops.push_back(Json{{"op", "move_layers"}, {"page", 1}, {"ids", Json::array({"child"})}, {"parent", "g"}});
    if (settings.size() > 3) ops.push_back(settings);
    return run(book(way), ops);
}

// a paint layer of a grey over the part [x0, x0 + w] × the page's height (mm) — on a 16-bit page its pixels precise, on a
// CMYK page its inks
Json paint_part(const std::string& id, int grey, double x0, double w, const std::string& blend = "normal") {
    return Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", id}, {"blend", blend}},
                        Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", id},
                             {"area", Json{{"poly", Json::array({Json::array({x0, 0}), Json::array({x0 + w, 0}), Json::array({x0 + w, 80}),
                                                                 Json::array({x0, 80})})}}},
                             {"rgb", Json::array({grey, grey, grey})}}});
}
// … laid as a blend into the layer's own values (fill_area's blend: paint_fill), so that on a 16-bit page the layer holds
// precise pixels (color_raster) — multiply over nothing lays the grey as it is
Json paint_precise(const std::string& id, int grey, double x0, double w) {
    Json ops = paint_part(id, grey, x0, w);
    ops[1]["blend"] = "multiply";
    return ops;
}
Json joined(std::initializer_list<Json> parts) {
    Json out = Json::array();
    for (const Json& part : parts) {
        if (part.is_array()) {
            for (const Json& op : part) out.push_back(op);
        } else {
            out.push_back(part);
        }
    }
    return out;
}
Json folder(const std::string& id, const Json& more = Json::object()) {
    Json op{{"op", "add_layer"}, {"page", 1}, {"kind", "folder"}, {"id", id}, {"group", true}};
    op.update(more);
    return op;
}
Json into(const std::string& id, const std::string& group) {
    return Json{{"op", "move_layers"}, {"page", 1}, {"ids", Json::array({id})}, {"parent", group}};
}
std::size_t deepest(const std::vector<core::Layer>& layers) {
    const auto owners = core::group_owners(layers);
    std::size_t most = 0;
    for (std::size_t i = 0; i < layers.size(); ++i) {
        std::size_t d = 0;
        for (auto at = owners[i]; at; at = owners[*at]) ++d;
        most = std::max(most, d);
    }
    return most;
}

const core::Layer& layer_of(const core::Document& doc, const std::string& id) {
    for (const core::Layer& l : doc.page(0).layers)
        if (l.id == id) return l;
    throw std::runtime_error("no layer " + id);
}

}  // namespace

class TestGroups : public QObject {
    Q_OBJECT

private slots:
    // (PhotoCraft's isolated_group_differs_from_pass_through) a multiply inside: pass-through multiplies with the page
    // below, an isolated (normal) group only with its own nothing
    void passThroughDiffersFromIsolated() {
        for (const std::string way : {"8", "16", "cmyk"}) {
            const int pass = grey_at(grouped(way, Json{{"group", true}}));
            const int alone = grey_at(grouped(way, Json{{"group", true}, {"blend", "normal"}}));
            const int base = grey_at(run(book(way), fill("base", 128)));
            QVERIFY2(std::abs(alone - base) <= 3, (way + ": isolated " + std::to_string(alone) + " base " + std::to_string(base)).c_str());
            QVERIFY2(pass < base - 20, (way + ": pass " + std::to_string(pass) + " base " + std::to_string(base)).c_str());
            if (way == "8") QVERIFY2(std::abs(pass - 64) <= 2, std::to_string(pass).c_str());
        }
    }

    // (group_opacity_applies_once, pass_through_group_opacity_mixes) over white paper
    void opacityAppliesOnce() {
        for (const std::string way : {"8", "16", "cmyk"}) {
            // two black layers in an isolated group of 50 %: half grey once (not a quarter of white left)
            Json ops = Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "folder"}, {"id", "g"}, {"group", true}, {"blend", "normal"}},
                                    Json{{"op", "set_layer"}, {"page", 1}, {"id", "g"}, {"opacity", 0.5}}, fill("a", 0), fill("b", 0),
                                    Json{{"op", "move_layers"}, {"page", 1}, {"ids", Json::array({"a", "b"})}, {"parent", "g"}}});
            const int half = grey_at(run(book(way), ops));
            core::Document plain = run(book(way), fill("a", 0));
            plain = run(plain, Json{{"op", "set_layer"}, {"page", 1}, {"id", "a"}, {"opacity", 0.5}});
            QVERIFY2(std::abs(half - grey_at(plain)) <= 3, (way + ": " + std::to_string(half) + " vs " + std::to_string(grey_at(plain))).c_str());
            // a pass-through group of 25 % mixes the page before and after it
            const int quarter = grey_at(grouped(way, Json{{"group", true}, {"opacity", 0.25}}, "normal", 0, false));
            core::Document one = run(book(way), fill("a", 0));
            one = run(one, Json{{"op", "set_layer"}, {"page", 1}, {"id", "a"}, {"opacity", 0.25}});
            QVERIFY2(std::abs(quarter - grey_at(one)) <= 3, (way + ": " + std::to_string(quarter) + " vs " + std::to_string(grey_at(one))).c_str());
            if (way == "8") QVERIFY2(std::abs(quarter - 191) <= 2, std::to_string(quarter).c_str());
        }
    }

    // what a hidden group holds is not drawn — a group in it too; a folder that is not a group, hidden, still shows it
    void hiddenGroupsHideWhatTheyHold() {
        for (const std::string way : {"8", "16", "cmyk"}) {
            const int white = grey_at(book(way));
            QCOMPARE(grey_at(grouped(way, Json{{"group", true}, {"visible", false}}, "normal", 0, false)), white);
            core::Document nested = grouped(way, Json{{"group", true}}, "normal", 0, false);
            nested = run(nested, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "folder"}, {"id", "inner"}, {"group", true}, {"parent", "g"}},
                                              Json{{"op", "move_layers"}, {"page", 1}, {"ids", Json::array({"child"})}, {"parent", "inner"}}}));
            QVERIFY2(grey_at(nested) < 80, (way + ": " + std::to_string(grey_at(nested))).c_str());  // (black: an ink page shows its black a little lighter)
            nested = run(nested, Json{{"op", "set_layer"}, {"page", 1}, {"id", "inner"}, {"visible", false}});
            QCOMPARE(grey_at(nested), white);
            // (a folder as before: hidden, what it holds is drawn all the same)
            QVERIFY(grey_at(grouped(way, Json{{"visible", false}}, "normal", 0, false)) < 80);
        }
    }

    // a group's mask (and vector mask) hides what it holds; a layer clipped to a pass-through group clips to its shape
    void masksAndClipping() {
        for (const std::string way : {"8", "16", "cmyk"}) {
            const int white = grey_at(book(way));
            core::Document doc = grouped(way, Json{{"group", true}}, "normal", 0, false);
            // the left half shown
            const Json left{{"poly", Json::array({Json::array({0, 0}), Json::array({30, 0}), Json::array({30, 80}), Json::array({0, 80})})}};
            doc = run(doc, Json{{"op", "set_layer_mask"}, {"page", 1}, {"id", "g"}, {"area", left}});
            const render::Image shown = drawn(doc);
            QVERIFY2(shown.getpixel(15, 40)[0] < 80, way.c_str());
            QCOMPARE(shown.getpixel(90, 40)[0], white);
            // a layer clipped to the group above it: inside its shape only
            doc = run(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "fill"}, {"id", "red"}, {"rgb", Json::array({255, 0, 0})}, {"clip", true}}}));
            const render::Image clipped = drawn(doc);
            QVERIFY2(clipped.getpixel(15, 40)[0] > 200 && clipped.getpixel(15, 40)[1] < 60, way.c_str());
            QCOMPARE(clipped.getpixel(90, 40)[0], white);
            QCOMPARE(clipped.getpixel(90, 40)[1], white);
            // the same group isolated: the same picture
            core::Document alone = run(doc, Json{{"op", "set_layer"}, {"page", 1}, {"id", "g"}, {"blend", "normal"}});
            QCOMPARE(drawn(alone).tobytes() == clipped.tobytes() || way != "8", true);
        }
    }

    // the order the layers are drawn in: a group's begin where it is, what it holds, its end; a page without groups as is
    void theTreeOfLayers() {
        core::Document doc = grouped("8", Json{{"group", true}});
        const std::vector<core::Layer>& layers = doc.page(0).layers;
        const auto steps = core::group_steps(layers);
        QCOMPARE(steps.size(), layers.size() + 1);  // (base, begin g, child, end g; the page's own layers too)
        const auto begin = std::find_if(steps.begin(), steps.end(), [](const core::GroupStep& s) { return s.kind == core::GroupStep::Kind::Begin; });
        QVERIFY(begin != steps.end());
        QCOMPARE(layers[begin->layer].id, std::string("g"));
        QCOMPARE(steps[begin->end].kind, core::GroupStep::Kind::End);
        QCOMPARE(layers[steps[std::size_t(begin - steps.begin()) + 1].layer].id, std::string("child"));
        const auto contents = core::group_contents(layers, begin->layer);
        QCOMPARE(contents.size(), std::size_t(1));
        QCOMPARE(layers[contents.front()].id, std::string("child"));
        // a folder chain that comes back on itself is no group's
        std::vector<core::Layer> loop(2);
        loop[0].id = "a";
        loop[0].kind = core::LayerKind::Folder;
        loop[0].group = true;
        loop[0].parent_id = "b";
        loop[1].id = "b";
        loop[1].kind = core::LayerKind::Folder;
        loop[1].group = true;
        loop[1].parent_id = "a";
        const auto owners = core::group_owners(loop);
        QVERIFY(!owners[0] && !owners[1]);
        QCOMPARE(core::group_steps(loop).size(), std::size_t(4));
        // without groups: one step a layer, in order
        const core::Document plain = book("8");
        const auto flat = core::group_steps(plain.page(0).layers);
        QCOMPARE(flat.size(), plain.page(0).layers.size());
        for (std::size_t i = 0; i < flat.size(); ++i) QCOMPARE(flat[i].layer, i);
    }

    // the ops: group only on a folder, pass_through a group's; a new group is 通過; turned off, its blend normal
    void theOpsCheckGroups() {
        const core::Document doc = book("8");
        QVERIFY(refusal(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"group", true}}).find("group makes a folder") != std::string::npos);
        QVERIFY(refusal(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "folder"}, {"blend", "pass_through"}}).find("pass_through") != std::string::npos);
        const core::Document made = run(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "folder"}, {"id", "g"}, {"group", true}});
        QVERIFY(layer_of(made, "g").group);
        QCOMPARE(layer_of(made, "g").blend, std::string("pass_through"));
        const core::Document off = run(made, Json{{"op", "set_layer"}, {"page", 1}, {"id", "g"}, {"group", false}});
        QVERIFY(!layer_of(off, "g").group);
        QCOMPARE(layer_of(off, "g").blend, std::string("normal"));
        const core::Document on = run(off, Json{{"op", "set_layer"}, {"page", 1}, {"id", "g"}, {"group", true}, {"blend", "pass_through"}});
        QVERIFY(layer_of(on, "g").group);
        QCOMPARE(layer_of(on, "g").blend, std::string("pass_through"));
        // the old folder's ops as before: no group unless asked
        const core::Document folder = run(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "folder"}, {"id", "f"}});
        QVERIFY(!layer_of(folder, "f").group);
        QVERIFY(refusal(folder, Json{{"op", "set_layer_mask"}, {"page", 1}, {"id", "f"}, {"fill", "hide"}}).find("a folder cannot take a mask") != std::string::npos);
        const core::Document pen = run(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "p"}});
        QVERIFY(refusal(pen, Json{{"op", "set_layer"}, {"page", 1}, {"id", "p"}, {"group", true}}).find("group is set on a folder") != std::string::npos);
        // group_layers with group: a group of 通過
        const core::Document two = run(doc, Json::array({fill("a", 0), Json{{"op", "group_layers"}, {"page", 1}, {"ids", Json::array({"a"})}, {"id", "gg"}, {"group", true}}}));
        QVERIFY(layer_of(two, "gg").group);
        QCOMPARE(layer_of(two, "gg").blend, std::string("pass_through"));
        // layers of different groups are not merged together
        const core::Document both = run(two, fill("b", 0));
        QVERIFY(refusal(both, Json{{"op", "merge_layers"}, {"page", 1}, {"ids", Json::array({"a", "b"})}}).find("different groups") != std::string::npos);
    }

    // Frame from Layers: grouped, the group's vector mask the box of what it holds (its undo: test_gui_layers)
    void frameFromLayers() {
        core::Document doc = book("8");
        doc = run(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "p"}},
                                    Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", "p"},
                                         {"area", Json{{"poly", Json::array({Json::array({10, 10}), Json::array({30, 10}), Json::array({30, 40}), Json::array({10, 40})})}}},
                                         {"rgb", Json::array({0, 0, 0})}}}));
        const core::ApplyResult made = run_ops(doc, Json::array({Json{{"op", "frame_from_layers"}, {"page", 1}, {"ids", Json::array({"p"})}, {"id", "fr"}}}));
        const core::Layer frame = layer_of(made.doc, "fr");
        QVERIFY(frame.group);
        QCOMPARE(frame.blend, std::string("pass_through"));
        QVERIFY(frame.vector_mask.has_value());
        QCOMPARE(layer_of(made.doc, "p").parent_id, Json("fr"));
        QVERIFY(!made.results.empty());
        const Json& report = made.results.back();
        QCOMPARE(report["id"], Json("fr"));
        const Json box = report["frame_mm"];
        QVERIFY2(std::abs(box[0].get<double>() - 10) < 0.3 && std::abs(box[1].get<double>() - 10) < 0.3, box.dump().c_str());
        QVERIFY2(std::abs(box[2].get<double>() - 20) < 0.3 && std::abs(box[3].get<double>() - 30) < 0.3, box.dump().c_str());
        // what it shows: the same inside (the frame is the box of what it holds; its edge drawn by the mask's own pixels)
        const render::Image framed = drawn(made.doc, 100), before = drawn(doc, 100);
        for (const auto& [x, y] : {std::array<int, 2>{79, 98}, {50, 60}, {110, 150}, {20, 20}, {200, 250}})
            QCOMPARE(framed.getpixel(x, y)[0], before.getpixel(x, y)[0]);
    }

    // groups@1: written and read back; a book of folders that are not groups names no feature and writes no key
    void theBookKeepsGroups() {
        const QTemporaryDir tmp;
        const std::filesystem::path dir = std::filesystem::path(tmp.path().toStdString()) / "g.genko";
        const core::Document doc = grouped("8", Json{{"group", true}, {"opacity", 0.5}});
        test::write_project(doc, dir);
        const core::Json project = test::read_json(QString::fromStdString((dir / "project.json").string()));
        QVERIFY(std::find(project["features"].begin(), project["features"].end(), Json("groups@1")) != project["features"].end());
        const auto back = storage::load_document(dir);
        QVERIFY2(back.report.clean(), back.report.to_json().dump().c_str());
        QVERIFY(layer_of(back.document, "g").group);
        QCOMPARE(layer_of(back.document, "g").blend, std::string("pass_through"));
        QCOMPARE(drawn(back.document).tobytes() == drawn(doc).tobytes(), true);
        const std::filesystem::path old_dir = std::filesystem::path(tmp.path().toStdString()) / "f.genko";
        test::write_project(grouped("8", Json{{"visible", false}}), old_dir);
        const std::string text = test::read_bytes(QString::fromStdString((old_dir / "project.json").string()));
        QVERIFY(text.find("groups@1") == std::string::npos);
        QVERIFY(text.find("\"group\"") == std::string::npos);
        // a broken mark: read-only with its reason
        core::Json broken = project;
        for (auto& layer : broken["pages"][0]["layers"])
            if (layer["id"] == Json("child")) layer["group"] = true;
        const auto read = storage::load_document_payload(broken, dir);
        const std::string issues = read.report.to_json().dump();
        QVERIFY2(issues.find("marked a group") != std::string::npos, issues.c_str());
    }

    // a folder that is not a group: the page drawn as the page without it, pixel for pixel (8 bits, precise, inks)
    void foldersDrawAsBefore() {
        for (const std::string way : {"8", "16", "cmyk"}) {
            Json ops = Json::array({fill("base", 200),
                                    Json{{"op", "add_layer"}, {"page", 1}, {"kind", "folder"}, {"id", "f"}, {"opacity", 0.3}, {"blend", "multiply"}},
                                    fill("a", 90, "multiply"),
                                    Json{{"op", "move_layers"}, {"page", 1}, {"ids", Json::array({"a"})}, {"parent", "f"}},
                                    Json{{"op", "set_layer"}, {"page", 1}, {"id", "f"}, {"visible", false}}});
            const core::Document with = run(book(way), ops);
            const core::Document without = run(book(way), Json::array({fill("base", 200), fill("a", 90, "multiply")}));
            QVERIFY2(drawn(with, 100).tobytes() == drawn(without, 100).tobytes(), way.c_str());
        }
    }

    // a page of groups drawn whole (in tiles past a million pixels) is the page drawn a part at a time
    void tilesMatchTheWholePage() {
        for (const std::string way : {"8", "16"}) {
            core::Document doc = grouped(way, Json{{"group", true}, {"opacity", 0.6}}, "multiply", 60);
            const Json left{{"poly", Json::array({Json::array({0, 0}), Json::array({30, 0}), Json::array({30, 80}), Json::array({0, 80})})}};
            doc = run(doc, Json{{"op", "set_layer_mask"}, {"page", 1}, {"id", "g"}, {"area", left}});
            render::RenderOptions options;
            options.mode = "proof";
            options.finish = false;
            const render::Image whole = render::render_page(doc.page(0), 600, options, &doc).image.convert("RGB");
            QVERIFY(std::int64_t(whole.width()) * whole.height() > 1024 * 1024);
            options.region = render::RenderRegion{400, 400, 300, 300};  // (across the tiles' edge at 512)
            const render::Image part = render::render_page(doc.page(0), 600, options, &doc).image.convert("RGB");
            QVERIFY2(part.tobytes() == whole.crop(render::Box{400, 400, 700, 700}).tobytes(), way.c_str());
        }
    }
    // (review P1-1) a page in precise colour — a 16-bit page with pixels painted — takes groups as before: ＋フォルダ,
    // Ctrl+G and Frame from Layers (a 通過 group each), and paints on after them
    void groupsOnAPreciseColourPage() {
        core::Document doc = run(book("16"), paint_precise("p", 100, 0, 30));
        QVERIFY(layer_of(doc, "p").color_raster != nullptr);
        QCOMPARE(refusal(doc, folder("g")), std::string());
        QCOMPARE(refusal(doc, Json{{"op", "group_layers"}, {"page", 1}, {"ids", Json::array({"p"})}, {"id", "gg"}, {"group", true}}), std::string());
        QCOMPARE(refusal(doc, Json{{"op", "frame_from_layers"}, {"page", 1}, {"ids", Json::array({"p"})}, {"id", "fr"}}), std::string());
        doc = run(doc, Json{{"op", "frame_from_layers"}, {"page", 1}, {"ids", Json::array({"p"})}, {"id", "fr"}});
        doc = run(doc, paint_precise("q", 30, 30, 20));  // (painted on, precise, after the groups: not refused)
        QVERIFY(layer_of(doc, "q").color_raster != nullptr);
        QVERIFY(grey_at(doc, 15, 40) < 200);
    }

    // (review P1-1, P2-3) a 16-bit PSD of a 通過 group opens (open_file) and comes in (import_psd) as a group; a PSD of groups
    // nested past core::kGroupDepthMax opens with them nested at most that deep, the deeper ones said skipped
    void psdsOfGroupsOpen() {
        const QTemporaryDir tmp;
        const auto file = [&](const std::string& name) { return std::filesystem::path(tmp.path().toStdString()) / name; };
        core::Document deep = run(book("16"), joined({paint_precise("p", 100, 0, 30), folder("g"), into("p", "g")}));
        QVERIFY(layer_of(deep, "p").color_raster != nullptr);
        (void)formats::export_page_psd(deep, deep.page(0), file("deep.psd"), 100);
        const std::string bytes = test::read_bytes(QString::fromStdString(file("deep.psd").string()));
        const formats::Opened opened = formats::open_file(bytes, "deep.psd");
        bool group = false;
        for (const core::Layer& l : opened.doc.page(0).layers) group = group || (l.group && l.blend == "pass_through");
        QVERIFY(group);
        QVERIFY(opened.doc.page(0).rgb && opened.doc.page(0).rgb->depth == 16);
        const core::Document imported = run(book("16"), Json{{"op", "import_psd"}, {"page", 1}, {"psd", core::b64encode(bytes)}});
        QVERIFY(std::any_of(imported.page(0).layers.begin(), imported.page(0).layers.end(), [](const core::Layer& l) { return l.group; }));
        // nested 70 deep (通過: nothing held while drawn)
        Json ops = joined({paint_precise("p", 0, 0, 30)});
        for (int k = 0; k < 70; ++k)
            ops.push_back(folder("n" + std::to_string(k), k == 0 ? Json{{"name", "n0"}} : Json{{"parent", "n" + std::to_string(k - 1)}, {"name", "n" + std::to_string(k)}}));
        ops.push_back(into("p", "n69"));
        const core::Document nested = run(book("8"), ops);
        QCOMPARE(deepest(nested.page(0).layers), std::size_t(70));
        (void)formats::export_page_psd(nested, nested.page(0), file("nested.psd"), 50);
        const formats::Opened back = formats::open_file(test::read_bytes(QString::fromStdString(file("nested.psd").string())), "nested.psd");
        QCOMPARE(deepest(back.doc.page(0).layers), core::kGroupDepthMax);
        // (what the deeper groups held: kept, in the deepest one made — the only layer that deep; the folders not made said)
        const auto owners = core::group_owners(back.doc.page(0).layers);
        std::size_t kept = 0;
        for (std::size_t i = 0; i < owners.size(); ++i) {
            std::size_t d = 0;
            for (auto at = owners[i]; at; at = owners[*at]) ++d;
            if (d == core::kGroupDepthMax && back.doc.page(0).layers[i].kind != core::LayerKind::Folder) ++kept;
        }
        QCOMPARE(kept, std::size_t(1));
        const std::string said = Json(back.notes).dump();
        QVERIFY2(said.find("n64") != std::string::npos && said.find("n69") != std::string::npos, said.c_str());
    }

    // (review P2-1) a layer clipped to a 通過 group without a mask clips to what the group holds — on a page of inks too
    void clippedToWhatAGroupHolds() {
        for (const std::string way : {"8", "16", "cmyk"}) {
            const int white = grey_at(book(way), 90, 40);
            core::Document doc = run(book(way), joined({folder("g"), paint_part("child", 0, 0, 30), into("child", "g"),
                                                        Json{{"op", "add_layer"}, {"page", 1}, {"kind", "fill"}, {"id", "red"},
                                                             {"rgb", Json::array({255, 0, 0})}, {"clip", true}}}));
            const render::Image shown = drawn(doc);
            QVERIFY2(shown.getpixel(15, 40)[0] > shown.getpixel(15, 40)[1] + 100, way.c_str());
            QCOMPARE(shown.getpixel(90, 40)[0], white);
            QCOMPARE(shown.getpixel(90, 40)[1], white);
        }
    }

    // (review P2-2) the lines set under a layer in a hidden group (or a group not exported) are drawn as before
    void linesUnderAHiddenGroup() {
        core::Document doc = run(book("8"), joined({folder("g"), paint_part("child", 0, 0, 30), into("child", "g"),
                                                    Json{{"op", "set_layer"}, {"page", 1}, {"id", "g"}, {"visible", false}}}));
        core::Document with = doc;
        with.add_line(with.page(0).index, "下の台詞", "", std::nullopt, "", core::Num(30), core::Num(30), core::Num(25), core::Num(16), "rounded")
            .style["below_layer"] = "child";
        QVERIFY(drawn(with).tobytes() != drawn(doc).tobytes());
        core::Document shown = with;
        shown.edit_page(0).layers.erase(std::remove_if(shown.edit_page(0).layers.begin(), shown.edit_page(0).layers.end(),
                                                       [](const core::Layer& l) { return l.id == "g"; }),
                                        shown.edit_page(0).layers.end());
        for (core::Layer& l : shown.edit_page(0).layers) {
            if (l.id == "child") l.visible = false;  // (as before groups: the layer hidden, the line drawn where it is)
        }
        QVERIFY(drawn(with).tobytes() == drawn(shown).tobytes());
    }

    // (review P2-3) nested 64 deep, each with a layer clipped to the group in it, drawn in time; past what is held at once,
    // refused in words (group_nesting)
    void deepGroups() {
        Json ops = Json::array();
        for (int k = 0; k < 64; ++k) ops.push_back(folder("n" + std::to_string(k), k == 0 ? Json::object() : Json{{"parent", "n" + std::to_string(k - 1)}}));
        for (const Json& op : paint_part("p", 0, 0, 30)) ops.push_back(op);
        ops.push_back(into("p", "n63"));
        for (int k = 0; k < 63; ++k)
            ops.push_back(Json{{"op", "add_layer"}, {"page", 1}, {"kind", "fill"}, {"id", "c" + std::to_string(k)}, {"parent", "n" + std::to_string(k)},
                               {"rgb", Json::array({255, 0, 0})}, {"clip", true}});
        const core::Document doc = run(book("8"), ops);
        QElapsedTimer timer;
        timer.start();
        const render::Image shown = drawn(doc);
        QVERIFY2(timer.elapsed() < 60000, std::to_string(timer.elapsed()).c_str());
        QVERIFY(shown.getpixel(15, 40)[0] > shown.getpixel(15, 40)[1] + 100);
        QCOMPARE(shown.getpixel(90, 40)[1], 255);
        Json alone = Json::array();
        for (int k = 0; k < 70; ++k)
            alone.push_back(folder("a" + std::to_string(k), k == 0 ? Json{{"blend", "normal"}} : Json{{"blend", "normal"}, {"parent", "a" + std::to_string(k - 1)}}));
        const core::Document too_deep = run(book("8"), alone);
        try {
            (void)drawn(too_deep);
            QFAIL("drawn");
        } catch (const core::Error& error) {
            QCOMPARE(error.code(), std::string("not_yet_ported"));
            QVERIFY2(std::string(error.what()).find("group_nesting") != std::string::npos, error.what());
        }
    }

    // (review P2-5) 表示レイヤーを結合 and 画像を統合 through a group that changes the look: as the page draws it, the group
    // taken off; on a page of inks too; on a page in precise colour refused in words
    void mergingThroughGroups() {
        for (const std::string way : {"8", "cmyk"}) {
            const core::Document doc = grouped(way, Json{{"group", true}, {"blend", "normal"}, {"opacity", 0.5}}, "multiply", 0);
            const int before = grey_at(doc);
            for (const Json& merge : {Json{{"op", "merge_visible"}, {"page", 1}, {"copy", false}},
                                      Json{{"op", "merge_visible"}, {"page", 1}, {"copy", false}, {"flatten", true}}}) {
                const core::Document merged = run(doc, merge);
                QVERIFY2(!core::has_groups(merged.page(0).layers), merge.dump().c_str());
                QVERIFY2(std::abs(grey_at(merged) - before) <= 3, (way + " " + merge.dump() + ": " + std::to_string(grey_at(merged)) + " vs " +
                                                                    std::to_string(before)).c_str());
            }
        }
        const core::Document precise = run(book("16"), joined({paint_precise("base", 128, 0, 60), folder("g", Json{{"blend", "normal"}}),
                                                                paint_precise("child", 0, 0, 30), into("child", "g"),
                                                                Json{{"op", "set_layer"}, {"page", 1}, {"id", "g"}, {"opacity", 0.5}}}));
        QVERIFY(refusal(precise, Json{{"op", "merge_visible"}, {"page", 1}, {"copy", false}}).find("precise colour") != std::string::npos);
    }

    // (review P3-3) グループとして描く on and off keeps the folder's own blend and the book's bytes
    void theSwitchKeepsTheFolder() {
        core::Document doc = run(book("8"), joined({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "folder"}, {"id", "f"}, {"blend", "multiply"}},
                                                    fill("a", 90), into("a", "f")}));
        const core::Document on = run(doc, Json{{"op", "set_layer"}, {"page", 1}, {"id", "f"}, {"group", true}});
        QVERIFY(layer_of(on, "f").group);
        QCOMPARE(layer_of(on, "f").blend, std::string("multiply"));
        const core::Document off = run(on, Json{{"op", "set_layer"}, {"page", 1}, {"id", "f"}, {"group", false}});
        QCOMPARE(layer_of(off, "f").blend, std::string("multiply"));
        const QTemporaryDir tmp;
        const std::filesystem::path a = std::filesystem::path(tmp.path().toStdString()) / "a.genko", b = std::filesystem::path(tmp.path().toStdString()) / "b.genko";
        test::write_project(doc, a);
        test::write_project(off, b);
        const Json x = test::read_json(QString::fromStdString((a / "project.json").string())), y = test::read_json(QString::fromStdString((b / "project.json").string()));
        QCOMPARE(x["pages"].dump(), y["pages"].dump());
        QCOMPARE(x["features"], y["features"]);
    }

    // (review P3-1, P3-2) a 通過 group clipped to the layer below is cut by it; what a group holds clips to nothing outside it
    void clippingOfPassThroughGroups() {
        for (const std::string way : {"8", "16", "cmyk"}) {
            const int white = grey_at(book(way), 90, 40);
            const core::Document clipped = run(book(way), joined({paint_part("base", 128, 0, 30), folder("g", Json{{"clip", true}}),
                                                                  fill("black", 0), into("black", "g")}));
            QVERIFY2(grey_at(clipped, 15, 40) < 80, way.c_str());
            QCOMPARE(grey_at(clipped, 90, 40), white);
            core::Document lowest = run(book(way), joined({paint_part("base", 128, 0, 30), folder("g"), fill("black", 0), into("black", "g"),
                                                          Json{{"op", "set_layer"}, {"page", 1}, {"id", "black"}, {"clip", true}}}));
            QVERIFY2(grey_at(lowest, 90, 40) < 80, way.c_str());  // (not cut by the layer outside the group)
        }
    }
};

QTEST_GUILESS_MAIN(TestGroups)
#include "test_groups.moc"
