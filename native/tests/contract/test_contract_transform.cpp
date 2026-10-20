// PC3 C-c (SPEC.md COLOR-07「変形」) as an agent or a person uses the ops: transform_layers on each kind of layer and what
// it holds — a whole-pixel move, a right-angle turn and a flip move the pixels exactly on 8-bit, 16-bit, 32-bit (a colour
// past 1 kept), CMYK and Lab layers; a scale moves a pen line's points and widens it and moves a patch's box; a quad is a
// true homography (a parallelogram exactly the matrix); a warp bends what is in its box and gives its normal form back;
// a perspective warp of one plane is its quad's homography; a puppet pin moved alone moves the layer; folders, links and a
// linked mask move along, an unlinked mask stays; an area lifts only what is in it; shape parts alone; a smart object's
// matrix (its warp: PC3 A5), a placed picture's box (turned: as a smart object), the locks refused; lines of dialogue moved
// with them, their text upright. content_aware_scale keeps a salient bar, scales plainly at amount 0, protects an alpha
// channel. liquify's strokes move the pixels and the pen lines, Reconstruct All undoes them, on precise and ink layers too.
// The dialogue's native_warp (Warp Text) bends the letters and names transform@1. And through the command line: saved,
// reopened, undone and done again, a book without the new keys naming no new feature. (Python has none of these: this
// build's own contract.)

#include <QtTest>

#include <QTemporaryDir>

#include <array>
#include <cmath>
#include <string>
#include <string_view>

#include "core/base64.hpp"
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/ink.hpp"
#include "core/json.hpp"
#include "core/smart.hpp"
#include "core/vector.hpp"
#include "render/image.hpp"
#include "render/own_pixels.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/raster.hpp"
#include "render/resample.hpp"
#include "render/text/lettering.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
namespace rs = render::resample;

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

// A picture: clear, with a block of varied colours from (40, 50) to (80, 110), a dark mark at its top left.
render::Image picture() {
    std::string bytes(std::size_t(kSide) * kSide * 4, '\0');
    for (int y = 50; y < 110; ++y) {
        for (int x = 40; x < 80; ++x) {
            const std::size_t i = (std::size_t(y) * kSide + std::size_t(x)) * 4;
            bytes[i] = static_cast<char>((x * 5 + y * 3) % 256);
            bytes[i + 1] = static_cast<char>((x * 11) % 256);
            bytes[i + 2] = static_cast<char>((y * 7) % 256);
            bytes[i + 3] = static_cast<char>(x < 44 && y < 54 ? 255 : 200 + (x + y) % 56);
        }
    }
    return render::Image::frombytes("RGBA", render::Size{kSide, kSide}, bytes);
}

core::Document book() {
    auto doc = core::new_episode("変形", core::Num(1), 1, core::PageSpec::custom(kInch, kInch, 20, 20, 1, 2, 2, 2, 2, 200, "color"));
    auto& p = doc.edit_page(0);
    p.numero = false;
    p.frames.clear();
    p.layers.clear();
    core::Layer layer;
    layer.id = "paint";
    layer.kind = core::LayerKind::Raster;
    layer.panel_clip = false;
    render::raster::save_raster(p, layer, picture());
    p.layers.push_back(std::move(layer));
    return doc;
}

const core::Layer& layer_of(const core::Document& doc, std::string_view id) {
    for (const core::Layer& layer : doc.page(0).layers)
        if (layer.id == id) return layer;
    throw std::runtime_error("no layer " + std::string(id));
}

// The layer's own pixels as a plane (its raw values: 8-bit / 255, precise, inks normalized).
rs::Plane own(const core::Document& doc, const std::string& id) {
    const auto p = render::own_pixels::OwnPixels::of(doc.page(0), layer_of(doc, id));
    if (!p) return rs::Plane();
    return p->read(render::own_pixels::Window{0, 0, p->width(), p->height()});
}

std::array<int, 4> content(const rs::Plane& p) {
    int x0 = p.width, y0 = p.height, x1 = -1, y1 = -1;
    for (int y = 0; y < p.height; ++y) {
        for (int x = 0; x < p.width; ++x) {
            if (!(p.at(x, y)[p.channels - 1] > 0.0f)) continue;
            x0 = std::min(x0, x);
            y0 = std::min(y0, y);
            x1 = std::max(x1, x + 1);
            y1 = std::max(y1, y + 1);
        }
    }
    return {x0, y0, x1, y1};
}

rs::Plane crop(const rs::Plane& p, const std::array<int, 4>& b) {
    rs::Plane out(b[2] - b[0], b[3] - b[1], p.channels, p.alpha);
    out.hdr = p.hdr;
    for (int y = 0; y < out.height; ++y)
        for (int x = 0; x < out.width; ++x) std::copy_n(p.at(b[0] + x, b[1] + y), p.channels, out.at(x, y));
    return out;
}

bool same(const rs::Plane& a, const rs::Plane& b) { return a.width == b.width && a.height == b.height && a.channels == b.channels && a.v == b.v; }

// The same within a level of 8 bits (a map made two ways: the last bit of a double may round a sample the other way).
bool within_a_level(const rs::Plane& a, const rs::Plane& b) {
    if (a.width != b.width || a.height != b.height || a.channels != b.channels) return false;
    for (std::size_t i = 0; i < a.v.size(); ++i)
        if (std::fabs(a.v[i] - b.v[i]) > 1.0f / 255.0f + 1e-6f) return false;
    return true;
}

Json transform(const std::vector<std::string>& ids) {
    Json list = Json::array();
    for (const std::string& id : ids) list.push_back(id);
    return Json{{"op", "transform_layers"}, {"page", 1}, {"ids", list}};
}

core::Document deep(int depth) { return run(book(), Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", depth}}); }
core::Document inks(const char* mode) {
    Json op{{"op", "set_color_mode"}, {"page", 1}, {"mode", mode}, {"depth", 16}};
    if (std::string(mode) == "cmyk") op["profile"] = "coated-cmyk";
    return run(book(), op);
}

}  // namespace

class TestContractTransform : public QObject {
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

    // A move by whole working pixels, a right-angle turn and a flip: the pixels moved exactly on every kind of layer.
    void exactMovesOnEveryKindOfPixels() {
        std::vector<std::pair<std::string, core::Document>> kinds{{"8-bit", book()}, {"16-bit", deep(16)}, {"32-bit", deep(32)}, {"cmyk", inks("cmyk")},
                                                                  {"lab", inks("lab")}};
        {  // (a 32-bit layer with a colour past 1)
            core::Document& hdr = kinds[2].second;
            core::Layer& layer = hdr.edit_page(0).layers[0];
            QVERIFY(layer.color_raster);
            const core::ColorRasterView view(*layer.color_raster);
            layer.color_raster = std::make_shared<const std::string>(core::encode_color_pixels(view.width(), view.height(), "f32", [&](std::size_t i) {
                auto px = view.pixel(i);
                if (i == std::size_t(60) * view.width() + 50) px = {2.5, 0.25, 1.5, 1.0};
                return px;
            }));
        }
        for (const auto& [name, doc] : kinds) {
            const rs::Plane before = own(doc, "paint");
            QVERIFY2(before.width > 0, name.c_str());
            const double s = before.width / kInch;  // (the layer's pixels a mm)
            // moved 5 working pixels right and 3 up
            Json op = transform({"paint"});
            op["move"] = Json::array({5 * kPx, -3 * kPx});
            const rs::Plane moved = own(run(doc, op), "paint");
            const int dx = static_cast<int>(std::lround(5 * kPx * s)), dy = static_cast<int>(std::lround(-3 * kPx * s));
            const auto b = content(before);
            QCOMPARE(content(moved), (std::array<int, 4>{b[0] + dx, b[1] + dy, b[2] + dx, b[3] + dy}));
            QVERIFY2(same(crop(moved, content(moved)), crop(before, b)), name.c_str());
            // turned and flipped about the box of what it shows: its pixels the same, turned
            for (const auto& [turn, way] : {std::pair{"90cw", rs::Turn::Cw90}, {"90ccw", rs::Turn::Ccw90}, {"180", rs::Turn::Rotate180},
                                            {"flip_h", rs::Turn::FlipH}, {"flip_v", rs::Turn::FlipV}}) {
                Json t = transform({"paint"});
                t["turn"] = turn;
                const core::ApplyResult done = run_ops(doc, Json::array({t}));
                const rs::Plane turned = own(done.doc, "paint");
                QVERIFY2(same(crop(turned, content(turned)), rs::turn(crop(before, b), way)), (name + " " + turn).c_str());
                QCOMPARE(done.results.at(0)["moved"], Json::array({"paint"}));
            }
        }
        // (the colour past 1 kept)
        Json op = transform({"paint"});
        op["turn"] = "180";
        const rs::Plane turned = own(run(kinds[2].second, op), "paint");
        float most = 0.0f;
        for (std::size_t i = 0; i < turned.v.size(); i += 4) most = std::max(most, turned.v[i]);
        QCOMPARE(most, 2.5f);
    }

    // A scale: a pen line's points scaled and its width, a patch's box, its picture the same; a quad a homography.
    void linesPatchesAndQuads() {
        core::Document doc = book();
        doc = run(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "pen"}, {"panel_each", false}});
        doc = run(doc, Json{{"op", "set_layer"}, {"page", 1}, {"id", "pen"}, {"panel_clip", false}});
        doc = run(doc, Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", "pen"}, {"points", Json::array({Json::array({5, 5}), Json::array({10, 6})})},
                            {"width_mm", 0.4}});
        doc = run(doc, Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", "pen"}, {"area", Json{{"poly", Json::array({Json::array({12, 12}), Json::array({16, 12}),
                                                                                                                         Json::array({16, 16})})}}},
                            {"rgb", Json::array({0, 0, 0})}});
        const core::Layer& pen = layer_of(doc, "pen");
        QCOMPARE(pen.stroke_count(), std::size_t(1));
        QCOMPARE(pen.patches.size(), std::size_t(1));
        const core::Stroke line = *pen.strokes->items[0];
        const Json box = pen.patches[0].attrs["box"];
        Json op = transform({"pen"});
        op["matrix"] = Json::array({2, 0, 0, 2, 0, 0});
        const core::Document scaled = run(doc, op);
        const core::Layer& after = layer_of(scaled, "pen");
        const core::Stroke& moved = *after.strokes->items[0];
        QCOMPARE(moved.id, line.id);
        QCOMPARE(moved.points.size(), line.points.size());
        for (std::size_t i = 0; i < line.points.size(); ++i) {
            QVERIFY(std::fabs(moved.points[i].x - 2 * line.points[i].x) < 1e-3 && std::fabs(moved.points[i].y - 2 * line.points[i].y) < 1e-3);
        }
        QVERIFY(std::fabs(moved.width_mm - 2 * line.width_mm) < 1e-3);
        QCOMPARE(*after.patches[0].png, *pen.patches[0].png);
        for (std::size_t k = 0; k < 4; ++k) QVERIFY(std::fabs(after.patches[0].attrs["box"][k].get<double>() - 2 * box[k].get<double>()) < 2e-3);
        // a parallelogram's quad is the matrix exactly; a trapezoid's a homography (the top narrower than the bottom)
        const rs::Plane before = own(doc, "paint");
        const auto b = content(before);
        const double x0 = b[0] * kPx, y0 = b[1] * kPx, x1 = b[2] * kPx, y1 = b[3] * kPx;
        Json quad = transform({"paint"});
        quad["quad"] = Json::array({Json::array({x0 + 1, y0}), Json::array({x1 + 1, y0}), Json::array({x1, y1}), Json::array({x0, y1})});
        Json matrix = transform({"paint"});
        const double k = 1.0 / (y1 - y0);
        matrix["matrix"] = Json::array({1, 0, -k, 1, 1 + k * y0, 0});  // (the same shear)
        const core::ApplyResult as_quad = run_ops(doc, Json::array({quad}));
        QVERIFY(as_quad.results.at(0).contains("matrix"));  // (a parallelogram: an affine map)
        QVERIFY(within_a_level(own(as_quad.doc, "paint"), own(run(doc, matrix), "paint")));
        quad["quad"] = Json::array({Json::array({x0 + 1.5, y0}), Json::array({x1 - 1.5, y0}), Json::array({x1, y1}), Json::array({x0, y1})});
        const rs::Plane trapezoid = own(run(doc, quad), "paint");
        const auto row = [&](int y) {
            int n = 0;
            for (int x = 0; x < trapezoid.width; ++x) n += trapezoid.at(x, y)[3] > 0.5f ? 1 : 0;
            return n;
        };
        QVERIFY2(row(b[1] + 1) < row(b[3] - 2) - 10, (std::to_string(row(b[1] + 1)) + " " + std::to_string(row(b[3] - 2))).c_str());
        quad["quad"] = Json::array({Json::array({0, 0}), Json::array({0, 0}), Json::array({0, 0}), Json::array({0, 0})});
        QVERIFY(refusal(doc, quad).find("quad") != std::string::npos);
    }

    // A warp bends what is in its box (the rest stays) and gives its normal form; a perspective warp of one plane is its
    // quad's homography; a puppet pin moved alone moves the layer.
    void warpsPerspectiveAndPuppet() {
        const core::Document doc = book();
        const rs::Plane before = own(doc, "paint");
        const auto b = content(before);
        Json warp = transform({"paint"});
        warp["warp"] = Json{{"style", "arc"}, {"bend", 50}};
        const core::ApplyResult done = run_ops(doc, Json::array({warp}));
        const Json form = done.results.at(0)["warp"];
        QCOMPARE(form["style"], Json("arc"));
        QCOMPARE(form["bend"], Json(50.0));
        QCOMPARE(form["bounds"].size(), std::size_t(4));
        const rs::Plane bent = own(done.doc, "paint");
        QVERIFY(!same(bent, before));
        const auto c = content(bent);
        QVERIFY2(c[0] < b[0] && c[2] > b[2] && std::abs(c[1] - b[1]) <= 1, ("an arc keeps the middle of the top and spreads its top round a circle: " + std::to_string(b[0]) + "," + std::to_string(b[1]) + "-" + std::to_string(b[2]) + "," + std::to_string(b[3]) + " to " + std::to_string(c[0]) + "," + std::to_string(c[1]) + "-" + std::to_string(c[2]) + "," + std::to_string(c[3])).c_str());
        // a custom mesh left as it is: the picture as it was
        Json flat = transform({"paint"});
        flat["warp"] = Json{{"grid", Json::array({2, 2})}};
        QVERIFY(same(own(run(doc, flat), "paint"), before));
        // a perspective warp of one plane: the quad's homography (within a level)
        const double x0 = b[0] * kPx, y0 = b[1] * kPx, x1 = b[2] * kPx, y1 = b[3] * kPx;
        const Json src = Json::array({Json::array({x0, y0}), Json::array({x1, y0}), Json::array({x1, y1}), Json::array({x0, y1})});
        const Json dst = Json::array({Json::array({x0 + 1, y0}), Json::array({x1 - 1, y0}), Json::array({x1, y1}), Json::array({x0, y1})});
        Json persp = transform({"paint"});
        persp["perspective"] = Json{{"planes", Json::array({Json{{"src", src}, {"dst", dst}}})}};
        Json quad = transform({"paint"});
        quad["quad"] = dst;
        const rs::Plane a = own(run(doc, persp), "paint"), q = own(run(doc, quad), "paint");
        // (the warp draws the homography by 4-pixel triangles: on average within a level, nine pixels in ten within 3 — the
        // picture's colours wrap round every few pixels, and where they do a sub-pixel difference is a large one)
        double total = 0.0;
        int counted = 0, beyond = 0;
        for (int y = b[1] + 3; y < b[3] - 3; ++y) {
            for (int x = b[0] + 8; x < b[2] - 8; ++x) {
                if (a.at(x, y)[3] < 0.5f || q.at(x, y)[3] < 0.5f) continue;
                float worst = 0.0f;
                for (int k = 0; k < 4; ++k) worst = std::max(worst, std::fabs(a.at(x, y)[k] - q.at(x, y)[k]));
                total += worst;
                beyond += worst > 3.0f / 255.0f ? 1 : 0;
                ++counted;
            }
        }
        QVERIFY2(counted > 500, std::to_string(counted).c_str());
        QVERIFY2(total / counted <= 1.0 / 255.0 && beyond * 10 <= counted,
                 (std::to_string(total / counted * 255) + " " + std::to_string(beyond) + " of " + std::to_string(counted)).c_str());
        // a puppet's one pin moved by whole pixels: the layer moved as it is
        Json puppet = transform({"paint"});
        const double px = 60 * kPx, py = 80 * kPx;
        puppet["puppet"] = Json{{"pins", Json::array({Json{{"src", Json::array({px, py})}, {"dst", Json::array({px + 6 * kPx, py + 4 * kPx})}}})}, {"mode", "rigid"}};
        const core::ApplyResult pinned = run_ops(doc, Json::array({puppet}));
        QCOMPARE(pinned.results.at(0)["puppet"]["mode"], Json("rigid"));
        const rs::Plane moved = own(pinned.doc, "paint");
        QCOMPARE(content(moved), (std::array<int, 4>{b[0] + 6, b[1] + 4, b[2] + 6, b[3] + 4}));
        float off = 0.0f;
        for (int y = b[1]; y < b[3]; ++y)
            for (int x = b[0]; x < b[2]; ++x)
                for (int k = 0; k < 4; ++k) off = std::max(off, std::fabs(moved.at(x + 6, y + 4)[k] - before.at(x, y)[k]));
        QVERIFY2(off <= 1.0f / 255.0f + 1e-6f, std::to_string(off * 255).c_str());
    }

    // Folders and links move along; a linked mask moves, an unlinked one stays; an area lifts only what is in it.
    void foldersLinksMasksAndAreas() {
        core::Document doc = book();
        doc = run(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "folder"}, {"id", "box"}});
        doc = run(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "inner"}, {"parent", "box"}, {"panel_each", false}});
        doc = run(doc, Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", "inner"}, {"points", Json::array({Json::array({3, 3}), Json::array({6, 3})})}});
        doc = run(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "friend"}, {"panel_each", false}});
        doc = run(doc, Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", "friend"}, {"points", Json::array({Json::array({3, 20}), Json::array({6, 20})})}});
        doc = run(doc, Json{{"op", "set_layer_mask"}, {"page", 1}, {"id", "paint"}, {"area", Json{{"rect", Json::array({0, 0, 12, 25.4})}}}});
        {  // (linked: the folder and the friend share a link; the mask moves with its layer)
            auto& page = doc.edit_page(0);
            for (core::Layer& layer : page.layers) {
                if (layer.id == "box" || layer.id == "friend") layer.link = "L1";
            }
        }
        Json op = transform({"box"});
        op["move"] = Json::array({1, 0});
        const core::ApplyResult done = run_ops(doc, Json::array({op}));
        QCOMPARE(done.results.at(0)["moved"], (Json::array({"box", "inner", "friend"})));
        QVERIFY(std::fabs(layer_of(done.doc, "inner").strokes->items[0]->points[0].x - 4) < 1e-6);
        QVERIFY(std::fabs(layer_of(done.doc, "friend").strokes->items[0]->points[0].x - 4) < 1e-6);
        // the paint layer's mask: unlinked it stays, linked it moves
        Json paint = transform({"paint"});
        paint["move"] = Json::array({4 * 25.4 / 150 * 2, 0});
        const core::Document fixed = run(doc, paint);
        QCOMPARE(*layer_of(fixed, "paint").mask->png, *layer_of(doc, "paint").mask->png);
        core::Document linked = doc;
        linked.edit_page(0).layers[0].mask->linked = true;
        const core::Document follows = run(linked, paint);
        QVERIFY(*layer_of(follows, "paint").mask->png != *layer_of(doc, "paint").mask->png);
        // an area: the left half of the block moves, the right stays; a line inside it moves, one outside stays
        const rs::Plane before = own(doc, "paint");
        const auto b = content(before);
        const double mid = (b[0] + b[2]) / 2 * kPx;
        Json lift = transform({"paint", "friend"});
        lift["area"] = Json{{"rect", Json::array({0, 0, mid, 25.4})}};
        lift["move"] = Json::array({0, 2 * kPx * 5});
        const core::Document part = run(doc, lift);
        const rs::Plane after = own(part, "paint");
        const int m = (b[0] + b[2]) / 2;
        for (int k = 0; k < 4; ++k) {
            QCOMPARE(after.at(m + 5, b[1] + 3)[k], before.at(m + 5, b[1] + 3)[k]);       // (the right half stays)
            QCOMPARE(after.at(b[0] + 5, b[1] + 13)[k], before.at(b[0] + 5, b[1] + 3)[k]);  // (the left moved 10 down)
        }
        QVERIFY(std::fabs(layer_of(part, "friend").strokes->items[0]->points[0].y - (20 + 10 * kPx)) < 1e-6);
    }

    // Shape parts alone; a smart object's matrix (its warp later); a placed picture's box; the locks; lines of dialogue.
    void partsSmartObjectsPlacedLocksAndLines() {
        core::Document doc = book();
        doc = run(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "shapes"}});
        {
            using namespace core::vector;
            auto& layer = doc.edit_page(0).layers.back();
            ShapePart a, b;
            a.id = "a";
            b.id = "b";
            for (ShapePart* p : {&a, &b}) {
                Subpath sub;
                sub.knots = {Knot::corner(2, 2), Knot::corner(6, 2), Knot::corner(6, 6)};
                p->path.subpaths = {sub};
                p->fill = Json{{"rgb", Json::array({0, 0, 0})}};
            }
            layer.shapes = make_shapes({a, b});
        }
        Json parts = transform({"shapes"});
        parts["parts"] = Json{{"shapes", Json::array({"b"})}};
        parts["box"] = Json::array({2, 2, 4, 4});
        parts["move"] = Json::array({3, 0});
        const core::Document moved = run(doc, parts);
        QCOMPARE(layer_of(moved, "shapes").shapes->items[0].path.subpaths[0].knots[0].anchor.x, 2.0);
        QCOMPARE(layer_of(moved, "shapes").shapes->items[1].path.subpaths[0].knots[0].anchor.x, 5.0);
        // a smart object: its matrix; a warp of it is A5's
        const core::Document so = run(book(), Json{{"op", "smart_convert"}, {"page", 1}, {"id", "paint"}, {"new_id", "so"}});
        Json scale = transform({"so"});
        scale["matrix"] = Json::array({2, 0, 0, 2, 1, 1});
        const core::Document scaled = run(so, scale);
        const auto m = core::smart::matrix_of(layer_of(scaled, "so"));
        QCOMPARE(m.a, 2.0);
        QCOMPARE(m.e, 1.0);
        Json warp = transform({"so"});
        warp["warp"] = Json{{"style", "arc"}};
        const std::string later = refusal(so, warp);
        QVERIFY2(later.find("not_yet_ported") == 0 && later.find("PC3 A5") != std::string::npos, later.c_str());
        // the locks
        core::Document locked = book();
        locked.edit_page(0).layers[0].lock_position = true;
        Json move = transform({"paint"});
        move["move"] = Json::array({1, 1});
        QVERIFY(refusal(locked, move).find("position") != std::string::npos);
        locked.edit_page(0).layers[0].lock_position = false;
        locked.edit_page(0).layers[0].locked = true;
        QVERIFY(refusal(locked, move).find("locked") != std::string::npos);
        // nothing to move, two ways at once, an unknown layer
        Json both = transform({"paint"});
        both["move"] = Json::array({1, 1});
        both["turn"] = "180";
        QVERIFY(refusal(book(), both).find("one of") != std::string::npos);
        QVERIFY(!refusal(book(), [] {
                     Json o = transform({"nothing"});
                     o["move"] = Json::array({1, 1});
                     return o;
                 }())
                     .empty());
        // a line of dialogue moved with them: its text upright
        core::Document talk = run(book(), Json{{"op", "add_line"}, {"page", 1}, {"text", "ね"}, {"x_mm", 2}, {"y_mm", 2}, {"w_mm", 6}, {"h_mm", 4}, {"id", "l1"}});
        Json turn = transform({"paint"});
        turn["turn"] = "90cw";
        turn["with"] = Json{{"lines", Json::array({"l1"})}};
        const core::Document turned = run(talk, turn);
        const core::StoryLine& line = turned.story.front();
        QCOMPARE(line.w_mm.value(), 4.0);  // (its box turned: the width the height was)
        QVERIFY(!line.style.contains("rotate_deg"));
    }

    // content_aware_scale: a salient bar kept, plain at amount 0, an alpha channel protects; refused on a pen layer.
    void contentAwareScale() {
        auto doc = book();
        {  // (grey, a red and blue bar from x 70 to 74 across the block)
            std::string bytes(std::size_t(kSide) * kSide * 4, '\0');
            for (int y = 50; y < 90; ++y) {
                for (int x = 40; x < 120; ++x) {
                    const std::size_t i = (std::size_t(y) * kSide + std::size_t(x)) * 4;
                    const bool in = x >= 70 && x < 74;
                    bytes[i] = static_cast<char>(in && x % 2 == 0 ? 255 : in ? 0 : 128);
                    bytes[i + 1] = static_cast<char>(in ? 0 : 128);
                    bytes[i + 2] = static_cast<char>(in && x % 2 == 1 ? 255 : in ? 0 : 128);
                    bytes[i + 3] = static_cast<char>(255);
                }
            }
            render::raster::save_raster(doc.edit_page(0), doc.edit_page(0).layers[0], render::Image::frombytes("RGBA", render::Size{kSide, kSide}, bytes));
        }
        const auto bars = [](const rs::Plane& p, int y) {
            int n = 0;
            for (int x = 0; x < p.width; ++x) {
                const float* v = p.at(x, y);
                n += v[3] > 0.99f && v[1] < 0.02f && (v[0] > 0.98f || v[2] > 0.98f) ? 1 : 0;
            }
            return n;
        };
        Json op{{"op", "content_aware_scale"}, {"page", 1}, {"id", "paint"}, {"scale_x", 60}};
        const core::ApplyResult done = run_ops(doc, Json::array({op}));
        QCOMPARE(done.results.at(0)["from"], Json::array({80, 40}));
        QCOMPARE(done.results.at(0)["to"], Json::array({48, 40}));
        const rs::Plane carved = own(done.doc, "paint");
        QCOMPARE(content(carved), (std::array<int, 4>{40, 50, 88, 90}));
        QCOMPARE(bars(carved, 60), 4);
        op["amount"] = 0;
        QVERIFY(bars(own(run(doc, op), "paint"), 60) < 4);
        // an alpha channel protects what it selects
        core::Document guarded = run(doc, Json{{"op", "add_channel"}, {"page", 1}, {"name", "守る"}, {"fill", "area"},
                                               {"area", Json{{"rect", Json::array({50 * kPx, 0, 4 * kPx, kInch})}}}});
        op["amount"] = 100;
        op["protect"] = "守る";
        QVERIFY(!refusal(guarded, op).size());
        QVERIFY(refusal(run(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "pen"}}), Json{{"op", "content_aware_scale"}, {"page", 1}, {"id", "pen"}, {"scale_x", 50}})
                    .find("paint layer") != std::string::npos);
    }

    // liquify's strokes: the pixels and the pen lines moved, Reconstruct All undoing them; precise and ink layers too.
    void liquifyStrokes() {
        core::Document doc = book();
        doc = run(doc, Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "pen"}, {"panel_each", false}});
        doc = run(doc, Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", "pen"}, {"points", Json::array({Json::array({8, 9}), Json::array({8, 13})})}});
        const Json push = Json::array({Json{{"tool", "forward_warp"}, {"size_mm", 6}, {"points", Json::array({Json::array({7, 10}), Json::array({9, 10})})}}});
        const core::ApplyResult done = run_ops(doc, Json::array({Json{{"op", "liquify"}, {"page", 1}, {"layer_id", "paint"}, {"strokes", push}}}));
        QCOMPARE(done.results.at(0)["changed"], Json(true));
        QVERIFY(done.warnings.empty());
        QVERIFY(!same(own(done.doc, "paint"), own(doc, "paint")));
        const core::ApplyResult lines = run_ops(doc, Json::array({Json{{"op", "liquify"}, {"page", 1}, {"layer_id", "pen"}, {"strokes", push}}}));
        bool pushed = false;
        for (const auto& p : layer_of(lines.doc, "pen").strokes->items[0]->points) pushed = pushed || p.x > 8.3;
        QVERIFY(pushed);
        Json undo = push;
        undo.push_back(Json{{"tool", "reconstruct_all"}, {"amount", 100}});
        const core::Document back = run(doc, Json{{"op", "liquify"}, {"page", 1}, {"layer_id", "paint"}, {"strokes", undo}});
        QVERIFY(same(own(back, "paint"), own(doc, "paint")));
        for (core::Document other : {deep(16), inks("cmyk")}) {
            const core::Document bent = run(other, Json{{"op", "liquify"}, {"page", 1}, {"layer_id", "paint"}, {"strokes", push}});
            QVERIFY(!same(own(bent, "paint"), own(other, "paint")));
            QCOMPARE(bool(layer_of(bent, "paint").color_raster), bool(layer_of(other, "paint").color_raster));
            QCOMPARE(bool(layer_of(bent, "paint").ink_raster), bool(layer_of(other, "paint").ink_raster));
        }
        QVERIFY(refusal(doc, Json{{"op", "liquify"}, {"page", 1}, {"layer_id", "paint"}, {"strokes", Json::array({Json{{"tool", "melt"}}})}}).find("tool") !=
                std::string::npos);
    }

    // Warp Text: the letters bent, transform@1 named; refused when not well formed.
    void warpText() {
        core::Document doc = run(book(), Json{{"op", "add_line"}, {"page", 1}, {"text", "ワープ"}, {"x_mm", 2}, {"y_mm", 2}, {"w_mm", 20}, {"h_mm", 8}, {"id", "l1"},
                                             {"wrap", "horizontal"}});
        const core::Document bent = run(doc, Json{{"op", "edit_line"}, {"id", "l1"}, {"style", Json{{"native_warp", Json{{"style", "arc"}, {"bend", 60}}}}}});
        QCOMPARE(bent.story.front().style["native_warp"]["style"], Json("arc"));
        QVERIFY(core::vector::uses_transform(bent));
        QVERIFY(!core::vector::uses_transform(doc));
        const render::Image plain = render::render_page(doc.page(0), 100, {}, &doc).image;
        const render::Image warped = render::render_page(bent.page(0), 100, {}, &bent).image;
        QVERIFY(plain.tobytes() != warped.tobytes());
        const std::string bad = refusal(doc, Json{{"op", "edit_line"}, {"id", "l1"}, {"style", Json{{"native_warp", Json{{"style", "spiral"}}}}}});
        QVERIFY2(bad.find("native_warp") != std::string::npos, bad.c_str());
        const core::Document cleared = run(bent, Json{{"op", "edit_line"}, {"id", "l1"}, {"style", Json{{"native_warp", nullptr}}}});
        QVERIFY(!cleared.story.front().style.contains("native_warp"));
    }

    // Through the command line: saved, reopened, undone and done again; no new feature named.
    void throughTheCommandLine() {
        const QString book_path = path(QStringLiteral("book.genko"));
        int exit = -1;
        const auto made = genko::test::run_genko({QStringLiteral("new"), book_path, QStringLiteral("--pages"), QStringLiteral("1")});
        QVERIFY2(made.exit_code == 0, qPrintable(made.described()));
        Json done = apply(book_path, QStringLiteral("paint.json"),
                          Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pic"}},
                                       Json{{"op", "set_layer"}, {"page", 1}, {"id", "pic"}, {"panel_each", false}, {"panel_clip", false}},
                                       Json{{"op", "put_raster"}, {"page", 1}, {"id", "pic"}, {"png_base64", core::b64encode(render::write_png(picture()))}}}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        const Json before = project(book_path);
        done = apply(book_path, QStringLiteral("turn.json"), Json::array({Json{{"op", "transform_layers"}, {"page", 1}, {"ids", Json::array({"pic"})}, {"turn", "90cw"}}}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        QCOMPARE(done["results"][0]["moved"], Json::array({"pic"}));
        const Json turned = project(book_path);
        QVERIFY(turned != before);
        QCOMPARE(turned.value("features", Json::array()), before.value("features", Json::array()));
        Json undone = genko({QStringLiteral("undo"), book_path, QStringLiteral("--as"), QStringLiteral("human:test")}, &exit);
        QVERIFY2(exit == 0, undone.dump().c_str());
        QCOMPARE(project(book_path)["pages"], before["pages"]);
        Json redone = genko({QStringLiteral("redo"), book_path, QStringLiteral("--as"), QStringLiteral("human:test")}, &exit);
        QVERIFY2(exit == 0, redone.dump().c_str());
        QCOMPARE(project(book_path)["pages"], turned["pages"]);
        done = apply(book_path, QStringLiteral("cas.json"),
                     Json::array({Json{{"op", "content_aware_scale"}, {"page", 1}, {"id", "pic"}, {"scale_y", 80}},
                                  Json{{"op", "liquify"}, {"page", 1}, {"layer_id", "pic"},
                                       {"strokes", Json::array({Json{{"tool", "bloat"}, {"size_mm", 20}, {"points", Json::array({Json::array({50, 50})})}}})}}}));
        QVERIFY2(done.value("ok", false) && done["warnings"].empty(), done.dump().c_str());
        const auto drawn = genko::test::run_genko({QStringLiteral("render"), book_path, QStringLiteral("--page"), QStringLiteral("1"), QStringLiteral("--dpi"),
                                                   QStringLiteral("20"), QStringLiteral("--out"), path(QStringLiteral("p1.png"))});
        QVERIFY2(drawn.exit_code == 0, qPrintable(drawn.described()));
    }
};

QTEST_GUILESS_MAIN(TestContractTransform)
#include "test_contract_transform.moc"
