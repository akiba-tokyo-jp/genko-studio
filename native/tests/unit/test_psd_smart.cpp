// PC3 A3 (the user's decision 3: a PSD's smart objects are always read and written; SPEC.md COLOR-05「PSD のスマート
// オブジェクト」): a placed layer's blocks and the linked files of a PSD (render/psd_smart) — upstream PhotoCraft's
// io/src/linked.rs tests (items written and found by uuid, malformed data left out) and io/src/blocks.rs tests (SoLd's id
// and transform, a placed layer's warp: a style and a custom mesh), this build's own SoLd and PlLd read back —; a PSD or
// PSB as a smart object's file (render/smart_decode through render/psd): 16 and 32 bits kept, a PSB read, a CMYK file's
// inks, a Lab file shown in sRGB, a file without a real merged picture drawn from its layers; and import_psd making smart
// objects of placed layers (Photoshop's picture shown until the smart object changes, then its file drawn), written back
// by the PSD writer with the block and item it was read with, byte for byte; the records a book keeps of them checked.

#include <QtTest>

#include <QElapsedTimer>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <optional>
#include <string>

#include "core/base64.hpp"
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/exposure.hpp"
#include "core/ink.hpp"
#include "core/model.hpp"
#include "core/paths.hpp"
#include "core/smart.hpp"
#include "formats/export.hpp"
#include "formats/open_file.hpp"
#include "render/image.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/psd.hpp"
#include "render/psd_descriptor.hpp"
#include "render/psd_filters.hpp"
#include "render/psd_smart.hpp"
#include "render/smart_decode.hpp"
#include "psd_build.hpp"
#include "raw_testgen.hpp"
#include "testsupport.hpp"

namespace core = genko::core;
namespace render = genko::render;
namespace psd = genko::render::psd;
namespace build = genko::test::psd_build;
namespace rawgen = genko::test::rawgen;
namespace fs = std::filesystem;
using core::Json;
using psd::descriptor::Writer;

namespace {

const core::CommandBus& bus() {
    static const core::CommandBus b(render::ops_registry());
    return b;
}
core::Document run(const core::Document& doc, const Json& ops) { return bus().apply(doc, ops, core::Actor("human:作者")).doc; }
core::ApplyResult applied(const core::Document& doc, const Json& ops) { return bus().apply(doc, ops, core::Actor("human:作者")); }

std::string padded(std::string s) {
    while (s.size() % 4 != 0) s += '\0';
    return s;
}

// A SoLd payload of a descriptor (soLD, version 4).
std::string sold_of(const Writer& d) {
    std::string out = "soLD";
    psd::descriptor::put_u32(out, 4);
    return padded(out + d.versioned("null"));
}

Writer bounds(double right, double bottom) {
    Writer b;
    b.unit("Top ", "#Pxl", 0);
    b.unit("Left", "#Pxl", 0);
    b.unit("Btom", "#Pxl", bottom);
    b.unit("Rght", "#Pxl", right);
    return b;
}

// An RGB picture of w × h: red on its left half, green on its right (upstream's test picture).
std::string halves_png(int w, int h) {
    render::Image picture = render::Image::create("RGB", render::Size{w, h}, render::Ink{0, 255, 0});
    picture.paste(render::Ink{255, 0, 0}, render::Box{0, 0, w / 2, h});
    return render::write_png(picture);
}

// A page of 4 × 6 inches at 100 dpi (400 × 600 pixels), no bleed: a PSD of that canvas placed on it pixel for pixel.
core::Document small_book() {
    core::PageSpec spec = core::PageSpec::a4_mono();
    spec.width_mm = core::Num(101.6);
    spec.height_mm = core::Num(152.4);
    spec.dpi = core::Num(100);
    spec.bleed_mm = core::Num(0);
    spec.inner_margin_mm = core::Num(5);
    spec.trim_w_mm.reset();
    spec.trim_h_mm.reset();
    spec.margins_mm.reset();
    spec.preset.reset();
    spec.expression = "color";
    return core::new_episode("PSD", core::Num(1), 1, spec);
}

struct PlacedPsd {
    std::string bytes;
    std::string sold;   // the layer's SoLd payload
    std::string item;   // its lnk2 item (after its length)
    std::string png;    // the file it shows
};

// A 400 × 600 PSD with a white background layer and one placed layer of a 10 × 8 PNG (red | green) laid at (100, 150)
// four times larger, its picture — Photoshop's — deliberately blue (to see what is drawn from where).
// (PC4 D7: `sold` in place of this build's block — Photoshop's, its smart filters with it —, `more` global blocks after lnk2)
// (PC4 F-e: `file` named `file_name` in place of the PNG — a camera RAW's —)
PlacedPsd placed_psd(const std::array<double, 8>& corners = {100, 150, 140, 150, 140, 182, 100, 182}, const std::string& sold = {},
                     const std::string& more = {}, const std::string& file = {}, const std::string& file_name = "in.png") {
    PlacedPsd out;
    out.png = file.empty() ? halves_png(10, 8) : file;
    psd::PlacedOut placed;
    placed.uuid = "uuid-1";
    placed.placed = "placed-1";
    placed.corners = corners;
    placed.width = 10;
    placed.height = 8;
    placed.resolution = 72;
    out.sold = sold.empty() ? psd::sold_block(placed) : sold;
    const std::string framed = psd::linked_item("uuid-1", file_name, file.empty() ? "png " : "    ", out.png);
    // (the item's own length: what read_linked gives back)
    const auto items = psd::read_linked(framed);
    out.item = std::string(items.at(0).item);
    build::File f;
    f.width = 400;
    f.height = 600;
    build::Layer bg;
    bg.name = "bg";
    bg.right = 400;
    bg.bottom = 600;
    for (int c = 0; c < 3; ++c) bg.channels.emplace_back(c, build::samples(400 * 600, 255, 8));
    build::Layer layer;
    layer.name = "placed";
    layer.left = 100;
    layer.top = 150;
    layer.right = 140;
    layer.bottom = 182;
    layer.channels.emplace_back(-1, build::samples(40 * 32, 255, 8));
    layer.channels.emplace_back(0, build::samples(40 * 32, 0, 8));
    layer.channels.emplace_back(1, build::samples(40 * 32, 0, 8));
    layer.channels.emplace_back(2, build::samples(40 * 32, 255, 8));
    layer.blocks = build::block("SoLd", out.sold);
    f.layers = {bg, layer};
    f.global = build::block("lnk2", framed) + more;
    for (int c = 0; c < 3; ++c) f.merged.push_back(build::samples(400 * 600, 255, 8));
    out.bytes = build::bytes(f);
    return out;
}

render::Image page_picture(const core::Document& doc, int dpi = 100) {
    render::RenderOptions o;
    o.mode = "screen";
    return render::render_page(doc.page(0), dpi, o, &doc).image.convert("RGB");
}

std::vector<double> at(const render::Image& image, int x, int y) { return image.getpixel(x, y); }

// (PC4 D7) A four-character id as a filterID.
std::int32_t id4(const char (&c)[5]) {
    return static_cast<std::int32_t>((static_cast<std::uint32_t>(static_cast<unsigned char>(c[0])) << 24) | (static_cast<std::uint32_t>(static_cast<unsigned char>(c[1])) << 16) |
                                     (static_cast<std::uint32_t>(static_cast<unsigned char>(c[2])) << 8) | static_cast<unsigned char>(c[3]));
}

Writer rgbc(double r, double g, double b) {
    Writer c;
    c.number("Rd  ", r);
    c.number("Grn ", g);
    c.number("Bl  ", b);
    return c;
}

// One of Photoshop's smart filters (filterFX): its name, its descriptor (class and items; none: a filter without a dialog),
// its id, its blend mode (BlnM) and opacity, shown or not.
Writer filter_item(const std::string& name, const std::optional<std::pair<std::string, Writer>>& fltr, std::int32_t id, std::string_view blend = "Nrml",
                   double opacity = 100, bool shown = true) {
    Writer options;
    options.unit("Opct", "#Prc", opacity);
    options.enumerated("Md  ", "BlnM", blend);
    Writer item;
    item.text("Nm  ", psd::utf16(name));
    item.object("blendOptions", options, "blendOptions");
    item.boolean("enab", shown);
    item.boolean("hasoptions", fltr.has_value());
    item.object("FrgC", rgbc(0, 0, 0), "RGBC");
    item.object("BckC", rgbc(255, 255, 255), "RGBC");
    if (fltr) item.object("Fltr", fltr->second, fltr->first);
    item.integer("filterID", id);
    return item;
}

// A placed layer's filterFX of these filters, the bottom one first; its filter mask on or off.
Writer filter_fx(const std::vector<Writer>& items, bool mask = false, bool on = true) {
    Writer fx;
    fx.boolean("enab", on);
    fx.boolean("validAtPosition", true);
    fx.boolean("filterMaskEnable", mask);
    fx.boolean("filterMaskLinked", true);
    fx.boolean("filterMaskExtendWithWhite", false);
    std::vector<std::pair<std::string, Writer>> list;
    for (const Writer& item : items) list.emplace_back("filterFX", item);
    fx.objects("filterFXList", list);
    return fx;
}

// Photoshop's SoLd of placed_psd's layer (its id, place, size and resolution) with this filterFX.
std::string sold_with(const Writer& fx) {
    Writer d;
    d.text("Idnt", u"uuid-1");
    d.text("placed", u"placed-1");
    d.integer("PgNm", 1);
    d.integer("totalPages", 1);
    d.integer("Type", 2);
    d.numbers("Trnf", {100, 150, 140, 150, 140, 182, 100, 182});
    d.numbers("nonAffineTransform", {100, 150, 140, 150, 140, 182, 100, 182});
    Writer size;
    size.number("Wdth", 10);
    size.number("Hght", 8);
    d.object("Sz  ", size, "Pnt ");
    d.unit("Rslt", "#Rsl", 72);
    d.object("filterFX", fx, "filterFXStyle");
    return sold_of(d);
}

Writer gaussian(double radius) {
    Writer g;
    g.unit("Rds ", "#Pxl", radius);
    return g;
}

// The file's filter effects (FEid, version 3) of one entry `id`: its picture's rectangle with no channels written, and its
// mask (`mask`: compression and data over a w × h rectangle; none: not written).
std::string filter_effects(const std::string& id, const std::optional<std::pair<unsigned, std::string>>& mask, int w = 40, int h = 32) {
    std::string entry;
    build::u8(entry, static_cast<unsigned>(id.size()));
    entry += id;
    build::u32(entry, 1);
    std::string body;
    for (int v : {150, 100, 182, 140}) build::u32(body, static_cast<std::uint32_t>(v));
    build::u32(body, 8);  // (its depth)
    build::u32(body, 3);  // (its channels: none written, nor its masks)
    for (int c = 0; c < 5; ++c) build::u32(body, 0);
    build::u64(entry, body.size());
    entry += body;
    if (mask) {
        build::u8(entry, 1);
        for (int v : {150, 100, 150 + h, 100 + w}) build::u32(entry, static_cast<std::uint32_t>(v));
        build::u64(entry, mask->second.size() + 2);
        build::u16(entry, mask->first);
        entry += mask->second;
    } else {
        build::u8(entry, 0);
    }
    std::string out;
    build::u32(out, 3);
    build::u64(out, entry.size());
    out += entry;
    while (out.size() % 4 != 0) out += '\0';
    return build::block("FEid", out);
}

const core::Layer* layer_of(const core::Document& doc, const std::string& id) {
    for (const core::Layer& l : doc.page(0).layers)
        if (l.id == id) return &l;
    return nullptr;
}

core::ApplyResult imported(const std::string& bytes) {
    return applied(small_book(), Json::array({Json{{"op", "import_psd"}, {"page", 1}, {"psd", core::b64encode(bytes)}, {"fit", "paper"}, {"id", "ps"}}}));
}

bool noted(const std::vector<std::string>& notes, const std::string& part) {
    return std::any_of(notes.begin(), notes.end(), [&part](const std::string& n) { return n.find(part) != std::string::npos; });
}

}  // namespace

class TestPsdSmart : public QObject {
    Q_OBJECT

private slots:
    // upstream linked.rs:114-148: items written and found by uuid; malformed data left out
    void linkedFilesRoundTrip() {
        const std::string a = psd::linked_item("abc-1", "a.png", "png ", std::string("\x01\x02\x03", 3));
        const std::string b = psd::linked_item("def-2", "b.psd", "8BPS", std::string(10, '\x09'));
        QCOMPARE(a.size() % 4, std::size_t(0));
        const std::string both = a + b;  // (the items read are views into the block: it stays while they are used)
        const auto items = psd::read_linked(both);
        QCOMPARE(items.size(), std::size_t(2));
        QCOMPARE(items[0].kind, std::string("liFD"));
        QCOMPARE(items[0].uuid, std::string("abc-1"));
        QCOMPARE(items[0].name, std::string("a.png"));
        QCOMPARE(std::string(items[0].data), std::string("\x01\x02\x03", 3));
        QCOMPARE(items[1].uuid, std::string("def-2"));
        QCOMPARE(items[1].type, std::string("8BPS"));
        QCOMPARE(std::string(items[1].data), std::string(10, '\x09'));
        QCOMPARE(psd::framed_item(items[1].item), b);  // (an item kept as it was read, framed again: the same bytes)
        QVERIFY(psd::read_linked(std::string("\0\0\0\0\0\0\0\xc8\x01\x02", 10)).empty());
        QVERIFY(psd::read_linked({}).empty());
        std::string cut = psd::linked_item("x", "f", "png ", std::string(4, '\x05'));
        cut.resize(cut.size() - 6);
        QVERIFY(psd::read_linked(cut).empty());
        // (Japanese names in UTF-16, as Photoshop writes them)
        QCOMPARE(psd::read_linked(psd::linked_item("u", "絵.png", "png ", "x")).at(0).name, std::string("絵.png"));
        QCOMPARE(psd::uuid_of("seed").size(), std::size_t(36));
        QCOMPARE(psd::uuid_of("seed"), psd::uuid_of("seed"));
        QVERIFY(psd::uuid_of("seed") != psd::uuid_of("other"));
    }

    // upstream blocks.rs:546-562 (smart_transform_from_sold) and 472-512 (placed_layer_warp_is_read)
    void placedBlocksRead() {
        Writer d;
        d.text("Idnt", u"uuid-1");
        d.numbers("Trnf", {10.0, 20.0, 110.0, 20.0, 110.0, 70.0, 10.0, 70.0});
        Writer size;
        size.number("Wdth", 50);
        size.number("Hght", 25);
        d.object("Sz  ", size, "Pnt ");
        std::string data = sold_of(d) + std::string(2, '\0');
        const auto placed = psd::read_placed("SoLd", data);
        QVERIFY(placed.has_value());
        QCOMPARE(placed->uuid, std::string("uuid-1"));
        QVERIFY(placed->has_corners);
        QCOMPARE(placed->corners, (std::array<double, 8>{10, 20, 110, 20, 110, 70, 10, 70}));
        QCOMPARE(placed->width, 50.0);
        QCOMPARE(placed->height, 25.0);
        QVERIFY(!placed->warp);
        QVERIFY(psd::parallelogram(placed->corners));
        // (its matrix, as upstream reads it: [2, 0, 0, 2, 10, 20])
        const auto& p = placed->corners;
        QCOMPARE((std::array<double, 6>{(p[2] - p[0]) / 50, (p[3] - p[1]) / 50, (p[6] - p[0]) / 25, (p[7] - p[1]) / 25, p[0], p[1]}),
                 (std::array<double, 6>{2, 0, 0, 2, 10, 20}));
        QVERIFY(!psd::read_placed("PlLd", data));  // (not a PlLd)
        QVERIFY(!psd::read_placed("PlLd", std::string(20, '\0')));
        // a warp of a style (arc, bend 40, bounds 0, 0, 60, 30)
        Writer arc;
        arc.enumerated("warpStyle", "warpStyle", "warpArc");
        arc.number("warpValue", 40);
        arc.number("warpPerspective", 0);
        arc.number("warpPerspectiveOther", 0);
        arc.enumerated("warpRotate", "Ornt", "Hrzn");
        arc.object("bounds", bounds(60, 30), "Rctn");
        Writer with_arc;
        with_arc.text("Idnt", u"u");
        with_arc.object("warp", arc, "warp");
        auto warped = psd::read_placed("SoLd", sold_of(with_arc));
        QVERIFY(warped && warped->warp);
        QCOMPARE((*warped->warp)["style"], Json("arc"));
        QCOMPARE((*warped->warp)["bend"], Json(40.0));
        QCOMPARE((*warped->warp)["bounds"], Json::array({0.0, 0.0, 60.0, 30.0}));
        QCOMPARE((*warped->warp)["vertical"], Json(false));
        // a custom mesh (4 × 4 points, the last moved 7 to the right)
        std::vector<double> xs, ys;
        for (int j = 0; j < 4; ++j) {
            for (int i = 0; i < 4; ++i) {
                xs.push_back(i * 20.0 + (i == 3 && j == 3 ? 7.0 : 0.0));
                ys.push_back(j * 10.0);
            }
        }
        Writer envelope;
        envelope.object_array("meshPoints", 16, "rationalPoint", {{"Hrzn", xs}, {"Vrtc", ys}}, "#Pxl");
        Writer custom;
        custom.enumerated("warpStyle", "warpStyle", "warpCustom");
        custom.object("bounds", bounds(60, 30), "Rctn");
        custom.object("customEnvelopeWarp", envelope, "customEnvelopeWarp");
        Writer with_custom;
        with_custom.text("Idnt", u"u");
        with_custom.object("warp", custom, "warp");
        warped = psd::read_placed("SoLd", sold_of(with_custom));
        QVERIFY(warped && warped->warp);
        QCOMPARE((*warped->warp)["style"], Json("custom"));
        QCOMPARE((*warped->warp)["mesh"]["points"][15], Json::array({67.0, 30.0}));
        // (warpNone, a style of no bend, a mesh where its grid is: no warp)
        Writer none;
        none.enumerated("warpStyle", "warpStyle", "warpNone");
        none.object("bounds", bounds(60, 30), "Rctn");
        Writer with_none;
        with_none.text("Idnt", u"u");
        with_none.object("warp", none, "warp");
        QVERIFY(!psd::read_placed("SoLd", sold_of(with_none))->warp);
        // (no id: not read)
        Writer nameless;
        nameless.numbers("Trnf", {0, 0, 1, 0, 1, 1, 0, 1});
        QVERIFY(!psd::read_placed("SoLd", sold_of(nameless)));
    }

    // this build's SoLd and PlLd (a placed layer it writes) read back: the same id, corners, size, resolution and warp
    void placedBlocksWrittenAndReadBack() {
        psd::PlacedOut out;
        out.uuid = "1b2c3d4e-0000-4000-8000-000000000001";
        out.placed = "p";
        out.corners = {10.5, 20, 210.5, 40, 200.5, 140, -9.5, 120};
        out.width = 200;
        out.height = 100;
        out.resolution = 350;
        Json mesh = Json::array();
        for (int k = 0; k < 16; ++k) mesh.push_back(Json::array({(k % 4) * 200.0 / 3 + (k == 5 ? 9 : 0), (k / 4) * 100.0 / 3}));
        out.warp = Json{{"style", "custom"}, {"bend", 0}, {"h_distort", 0}, {"v_distort", 0}, {"vertical", false}, {"bounds", Json::array({0, 0, 200, 100})},
                        {"mesh", Json{{"us", Json::array({0, 1})}, {"vs", Json::array({0, 1})}, {"points", mesh}}}};
        const std::string sold = psd::sold_block(out), plld = psd::plld_block(out);
        QCOMPARE(sold.size() % 4, std::size_t(0));
        QCOMPARE(plld.size() % 4, std::size_t(0));
        for (const auto& [key, data] : {std::pair<std::string, std::string>{"SoLd", sold}, {"PlLd", plld}}) {
            const auto back = psd::read_placed(key, data);
            QVERIFY2(back.has_value(), key.c_str());
            QCOMPARE(back->uuid, out.uuid);
            QCOMPARE(back->corners, out.corners);
            QVERIFY(back->warp.has_value());
            QCOMPARE((*back->warp)["mesh"]["points"][5], mesh[5]);
            QCOMPARE((*back->warp)["style"], Json("custom"));
        }
        const auto back = psd::read_placed("SoLd", sold);
        QCOMPARE(back->placed, std::string("p"));
        QCOMPARE(back->width, 200.0);
        QCOMPARE(back->height, 100.0);
        QCOMPARE(back->resolution, 350.0);
        QCOMPARE(back->type, 2);
        QVERIFY(!psd::parallelogram(out.corners));
    }

    // PC5-0 (Z0's review P2-1): a smart object of a 32-bit sRGB page — its contents a page — written into the page's PSD
    // (its contents a PSD of 32 bits: linear light) and read back with import_psd (the placed layer a smart object of that
    // file, drawn from its deep picture), and its contents written as a PSD (リンクに変換…'s file) read as a smart object's
    // file: the values it had, the dark ones too (1/2.2 lifted 0.02 to about 0.09)
    void thirtyTwoBitContentsComeBack() {
        const core::Actor actor("human:test");
        const core::CommandBus bus(render::ops_registry());
        const core::PageSpec spec = core::PageSpec::custom(40.64, 20.32, 40.64, 20.32, 0, 0, 0, 0, 0, 200, "color");  // (320 × 160 px)
        core::Document doc = core::new_episode("中身", core::Num(1), 1, spec);
        doc.edit_page(0).frames.clear();
        doc.edit_page(0).numero = false;
        doc.edit_page(0).layers.clear();
        const std::array<double, 4> greys{0.02, 0.05, 0.2, 0.6};
        core::Json pixels = core::Json::array();
        for (const double g : greys)
            for (int k = 0; k < 8; ++k) {  // (each 8 pixels wide: the middle of a stripe is its value)
                for (int c = 0; c < 3; ++c) pixels.push_back(g);
                pixels.push_back(1.0);
            }
        doc = bus.apply(doc, core::Json::array({core::Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 32}},
                                                 core::Json{{"op", "put_color_raster"}, {"page", 1}, {"width", 32}, {"height", 1}, {"precision", "f32"}, {"pixels", pixels}}}),
                        actor)
                  .doc;
        const std::string painted = doc.page(0).layers.back().id;
        doc = bus.apply(doc, core::Json::array({core::Json{{"op", "smart_convert"}, {"page", 1}, {"ids", core::Json::array({painted})}, {"new_id", "so"}}}), actor).doc;
        QVERIFY(doc.page(0).layers.back().kind == core::LayerKind::Smart);
        render::RenderOptions options;
        options.mode = "print";
        options.precise = true;
        const auto values_at_stripes = [&](const core::Document& d) {
            const render::RenderResult made = render::render_page(d.page(0), 200, options, &d);  // (held: the view reads its bytes)
            const core::ColorRasterView view(*made.precise);
            std::vector<double> out;
            for (std::size_t s = 0; s < greys.size(); ++s) out.push_back(view.pixel(std::size_t(view.height() / 2) * view.width() + (2 * s + 1) * view.width() / 8)[1]);
            return out;
        };
        const std::vector<double> before = values_at_stripes(doc);
        for (std::size_t s = 0; s < greys.size(); ++s) QVERIFY2(std::abs(before[s] - greys[s]) < 1e-4, qPrintable(QString::number(before[s])));
        QTemporaryDir tmp;
        const fs::path file = core::path_from_utf8(tmp.path().toStdString()) / "so.psd";
        genko::formats::export_page_psd(doc, doc.page(0), file, 200);
        const std::string bytes = genko::test::read_bytes(QString::fromStdString(core::path_to_utf8(file)));
        core::Document back = core::new_episode("読む", core::Num(1), 1, spec);
        back.edit_page(0).frames.clear();
        back.edit_page(0).numero = false;
        back.edit_page(0).layers.clear();
        back = bus.apply(back, core::Json::array({core::Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 32}},
                                                   core::Json{{"op", "import_psd"}, {"page", 1}, {"psd", core::b64encode(bytes)}, {"fit", "paper"}, {"id", "back"}}}),
                         actor)
                   .doc;
        bool smart = false;
        for (const core::Layer& l : back.page(0).layers) smart = smart || l.kind == core::LayerKind::Smart;
        QVERIFY(smart);
        const std::vector<double> after = values_at_stripes(back);
        for (std::size_t s = 0; s < greys.size(); ++s)
            QVERIFY2(std::abs(after[s] - greys[s]) < 2e-3, qPrintable(QStringLiteral("stripe %1: %2 for %3").arg(s).arg(after[s]).arg(greys[s])));
        // the contents written as a PSD (リンクに変換…): its deep picture the contents' values
        const fs::path contents = core::path_from_utf8(tmp.path().toStdString()) / "contents.psd";
        const core::SmartContents* held = core::smart::contents_of(doc, doc.page(0).layers.back());
        QVERIFY(held != nullptr && held->page);
        genko::formats::export_contents_psd(doc, *held->page, contents, 200);
        const render::smart::DeepPicture deep = render::smart::deep_picture(genko::test::read_bytes(QString::fromStdString(core::path_to_utf8(contents))));
        QCOMPARE(deep.depth, 32);
        for (std::size_t s = 0; s < greys.size(); ++s) {
            const std::size_t at = (std::size_t(deep.height / 2) * deep.width + (2 * s + 1) * deep.width / 8) * deep.channels;
            QVERIFY2(std::abs(deep.samples[at] - greys[s]) < 1e-4, qPrintable(QStringLiteral("contents stripe %1: %2").arg(s).arg(deep.samples[at])));
        }
    }

    // a PSD or PSB as a smart object's file: 16 and 32 bits kept, a PSB read, its picture as the 8-bit page reads it
    void aPsdFileKeepsItsBits() {
        // RGB of 16 bits, merged only: 3 × 2 of (0x1234, 0xabcd, 0xffff)
        build::File f;
        f.width = 3;
        f.height = 2;
        f.depth = 16;
        f.merged = {build::samples(6, 0x1234, 16), build::samples(6, 0xabcd, 16), build::samples(6, 0xffff, 16)};
        for (const int version : {1, 2}) {
            f.version = version;
            const std::string bytes = build::bytes(f);
            const render::smart::FileInfo info = render::smart::look_at(bytes, "");
            QCOMPARE(info.format, std::string("psd"));
            QCOMPARE(info.suffix, std::string(version == 2 ? ".psb" : ".psd"));
            QCOMPARE(info.depth, 16);
            QCOMPARE(info.mode, std::string("rgb"));
            QCOMPARE(info.width, std::int64_t(3));
            QCOMPARE(info.dpi, 72.0);
            const render::smart::DeepPicture deep = render::smart::deep_picture(bytes);
            QCOMPARE(deep.depth, 16);
            QCOMPARE(deep.channels, 3u);
            QCOMPARE(deep.samples[0], static_cast<float>(0x1234) / 65535.0f);
            QCOMPARE(deep.samples[1], static_cast<float>(0xabcd) / 65535.0f);
            const render::Image eight = render::smart::eight_bit_picture(bytes);
            QCOMPARE(eight.mode(), std::string_view("RGB"));
            QCOMPARE(eight.getpixel(2, 1), (std::vector<double>{0x12, 0xab, 0xff}));
        }
        // grey of 32 bits: linear light, encoded by the file's grey curve — none: sGray's, the sRGB curve (PC5-0, Z0's review
        // P2-1: the inverse of the 32-bit PSD this build writes; before it 1/2.2) —, its values past 1 kept
        build::File grey;
        grey.width = 2;
        grey.height = 1;
        grey.depth = 32;
        grey.mode = 1;
        float quarter = 0.25f, two = 2.0f;
        std::uint32_t q = 0, t = 0;
        std::memcpy(&q, &quarter, 4);
        std::memcpy(&t, &two, 4);
        std::string plane;
        build::u32(plane, q);
        build::u32(plane, t);
        grey.merged = {plane};
        const std::string bytes = build::bytes(grey);
        const render::smart::DeepPicture deep = render::smart::deep_picture(bytes);
        QCOMPARE(deep.mode, std::string("gray"));
        QVERIFY(std::abs(deep.samples[0] - core::linear_to_srgb(0.25)) < 1e-6);
        QVERIFY(std::abs(deep.samples[1] - core::linear_to_srgb(2.0)) < 1e-6);
        QCOMPARE(render::smart::look_at(bytes, "").mode, std::string("gray"));
        QCOMPARE(render::smart::look_at(bytes, "").depth, 32);
        {  // (a NaN read as 0, as before; a value past the float's range after the curve kept finite)
            build::File odd = grey;
            const float nan = std::numeric_limits<float>::quiet_NaN(), huge = std::numeric_limits<float>::max();
            std::uint32_t n = 0, h = 0;
            std::memcpy(&n, &nan, 4);
            std::memcpy(&h, &huge, 4);
            std::string odd_plane;
            build::u32(odd_plane, n);
            build::u32(odd_plane, h);
            odd.merged = {odd_plane};
            const render::smart::DeepPicture read = render::smart::deep_picture(build::bytes(odd));
            QCOMPARE(read.samples[0], 0.0f);
            QVERIFY(std::isfinite(read.samples[1]) && read.samples[1] > 1.0f);
        }
        // a suffix that is not its kind, and a PSD that cannot be read, refused
        QVERIFY_THROWS_EXCEPTION(core::Error, render::smart::look_at(bytes, ".png"));
        QVERIFY_THROWS_EXCEPTION(core::Error, render::smart::look_at(bytes.substr(0, 30), ""));
    }

    // a CMYK file's inks kept on a CMYK page, a Lab file shown in sRGB, a file without a real merged picture drawn from
    // its layers
    void aPsdFilesColours() {
        build::File cmyk;
        cmyk.width = 2;
        cmyk.height = 2;
        cmyk.mode = 4;
        // (stored inverted: 255 no ink) C 20%, M 0, Y 0, K 60%
        cmyk.merged = {build::samples(4, 204, 8), build::samples(4, 255, 8), build::samples(4, 255, 8), build::samples(4, 102, 8)};
        const std::string bytes = build::bytes(cmyk);
        QCOMPARE(render::smart::look_at(bytes, "").mode, std::string("cmyk"));
        core::PageColor page;
        page.space = core::InkSpace::Cmyk;
        page.depth = 8;
        const core::InkPixels inks = render::smart::file_inks(bytes, page, "keep");
        QCOMPARE(inks.width, 2u);
        QCOMPARE(int(std::lround(inks.value(0, 0) * 100)), 20);
        QCOMPARE(int(std::lround(inks.value(0, 1) * 100)), 0);
        QCOMPARE(int(std::lround(inks.value(0, 3) * 100)), 60);
        // Lab: L 100, a 0, b 0 is white
        build::File lab;
        lab.width = 1;
        lab.height = 1;
        lab.mode = 9;
        lab.merged = {build::samples(1, 255, 8), build::samples(1, 128, 8), build::samples(1, 128, 8)};
        const render::Image white = render::smart::eight_bit_picture(build::bytes(lab));
        QCOMPARE(white.mode(), std::string_view("RGB"));
        for (const double v : white.getpixel(0, 0)) QVERIFY2(v >= 253, Json(white.getpixel(0, 0)).dump().c_str());
        // (and sRGB's red: L 53, a 80, b 67)
        lab.merged = {build::samples(1, 136, 8), build::samples(1, 208, 8), build::samples(1, 195, 8)};
        const std::vector<double> lab_red = render::smart::eight_bit_picture(build::bytes(lab)).getpixel(0, 0);
        QVERIFY2(lab_red[0] > 235 && lab_red[1] < 45 && lab_red[2] < 45, Json(lab_red).dump().c_str());
        // its merged picture blank (saved without maximised compatibility): its layers laid over each other
        build::File layered;
        layered.width = 4;
        layered.height = 4;
        layered.real_merged = false;
        build::Layer red;
        red.name = "red";
        red.left = 1;
        red.top = 1;
        red.right = 3;
        red.bottom = 3;
        red.channels = {{-1, build::samples(4, 255, 8)}, {0, build::samples(4, 255, 8)}, {1, build::samples(4, 0, 8)}, {2, build::samples(4, 0, 8)}};
        layered.layers = {red};
        layered.merged = {build::samples(16, 255, 8), build::samples(16, 255, 8), build::samples(16, 255, 8)};
        const render::Image drawn = render::smart::eight_bit_picture(build::bytes(layered));
        QCOMPARE(drawn.mode(), std::string_view("RGBA"));
        QCOMPARE(drawn.getpixel(1, 1), (std::vector<double>{255, 0, 0, 255}));
        QCOMPARE(drawn.getpixel(0, 0)[3], 0.0);
        QCOMPARE(render::smart::look_at(build::bytes(layered), "").mode, std::string("rgba"));
    }

    // the reader: a placed layer is a smart object with its block, and the PSD's linked file found by its uuid
    void theReaderFindsPlacedLayers() {
        const PlacedPsd made = placed_psd();
        const psd::File file = psd::File::read(made.bytes);
        QCOMPARE(file.layers().size(), std::size_t(2));
        const psd::Layer& layer = file.layers()[1];
        QCOMPARE(layer.kind, std::string("smart"));
        QVERIFY(layer.placed.has_value());
        QCOMPARE(layer.placed->uuid, std::string("uuid-1"));
        QCOMPARE(std::string(file.placed_block(layer)), made.sold);
        const auto linked = file.linked("uuid-1");
        QVERIFY(linked.has_value());
        QCOMPARE(std::string(linked->data), made.png);
        QCOMPARE(std::string(linked->item), made.item);
        QVERIFY(!file.linked("other"));
        QCOMPARE(file.layers()[0].kind, std::string("pixels"));
        // (its pixels are still Photoshop's picture: blue)
        QCOMPARE(file.image(layer).getpixel(110, 160), (std::vector<double>{0, 0, 255, 255}));
    }

    // import_psd: the placed layer a smart object of its file, Photoshop's picture shown until it changes; written back
    // by the PSD writer with the block and the item it was read with (the same canvas: byte for byte)
    void importedAndWrittenBack() {
        const PlacedPsd made = placed_psd();
        core::Document doc = small_book();
        const core::ApplyResult result =
            applied(doc, Json::array({Json{{"op", "import_psd"}, {"page", 1}, {"psd", core::b64encode(made.bytes)}, {"fit", "paper"}, {"id", "ps"}}}));
        doc = result.doc;
        const Json report = result.results.at(0);
        QVERIFY2(report.contains("smart_objects"), report.dump().c_str());
        QCOMPARE(report["smart_objects"][0]["id"], Json("ps-2"));
        const core::Layer* so = nullptr;
        for (const core::Layer& l : doc.page(0).layers)
            if (l.id == "ps-2") so = &l;
        QVERIFY(so != nullptr && so->kind == core::LayerKind::Smart && !so->raster_png);
        const core::SmartContents* contents = core::smart::contents_of(doc, *so);
        QVERIFY(contents != nullptr && contents->kind == "file");
        QCOMPARE(contents->format, std::string("png"));
        QCOMPARE(contents->name, std::string("in.png"));
        QCOMPARE(*doc.smart_files.at(contents->asset), made.png);
        QCOMPARE((*contents->psd)["uuid"], Json("uuid-1"));
        QCOMPARE((*so->smart)["psd"]["block"], Json("SoLd"));
        QCOMPARE(*doc.smart_files.at((*so->smart)["psd"]["raw"].get<std::string>()), made.sold);
        // (its matrix: the 10 × 8 file at 72 dpi laid at (100, 150) px, 40 × 32 px, of a page of 100 dpi)
        const core::smart::Matrix m = core::smart::matrix_of(*so);
        QVERIFY(std::abs(m.e - 25.4) < 1e-9 && std::abs(m.f - 38.1) < 1e-9);
        QVERIFY(std::abs(m.a * 10 * 25.4 / 72 - 10.16) < 1e-9 && std::abs(m.d * 8 * 25.4 / 72 - 8.128) < 1e-9);
        core::smart::validate_smart(doc);
        // drawn: Photoshop's picture (blue) until it changes
        render::Image page = page_picture(doc);
        QCOMPARE(at(page, 110, 160), (std::vector<double>{0, 0, 255}));
        // the PSD written: the same block and the same item, the layer's picture the same
        QTemporaryDir dir;
        const fs::path out = core::path_from_utf8(dir.filePath(QStringLiteral("p.psd")).toStdString());
        genko::formats::export_page_psd(doc, doc.page(0), out, 100);
        const psd::File written = psd::File::read(genko::test::read_bytes(dir.filePath(QStringLiteral("p.psd"))));
        const psd::Layer* placed = nullptr;
        for (const psd::Layer& l : written.layers())
            if (l.kind == "smart") placed = &l;
        QVERIFY(placed != nullptr);
        QCOMPARE(std::string(written.placed_block(*placed)), made.sold);
        QCOMPARE(std::string(written.linked("uuid-1")->item), made.item);
        QCOMPARE(written.image(*placed).getpixel(110, 160), (std::vector<double>{0, 0, 255, 255}));
        // changed (moved by a whole millimetre): drawn from its file — red on the left, green on the right —, written
        // with a fresh block of the same contents
        doc = run(doc, Json::array({Json{{"op", "smart_transform"}, {"page", 1}, {"id", "ps-2"}, {"matrix", Json::array({1, 0, 0, 1, 2.54, 0})}}}));
        QVERIFY(!doc.page(0).layers.empty());
        for (const core::Layer& l : doc.page(0).layers)
            if (l.id == "ps-2") QVERIFY(!l.smart->contains("psd"));
        page = page_picture(doc);
        QCOMPARE(at(page, 113, 166), (std::vector<double>{255, 0, 0}));
        QCOMPARE(at(page, 145, 166), (std::vector<double>{0, 255, 0}));
        genko::formats::export_page_psd(doc, doc.page(0), out, 100);
        const psd::File again = psd::File::read(genko::test::read_bytes(dir.filePath(QStringLiteral("p.psd"))));
        const psd::Layer* moved = nullptr;
        for (const psd::Layer& l : again.layers())
            if (l.kind == "smart") moved = &l;
        QVERIFY(moved != nullptr);
        QVERIFY(std::string(again.placed_block(*moved)) != made.sold);
        QCOMPARE(moved->placed->uuid, std::string("uuid-1"));
        const std::array<double, 8> expected{110, 150, 150, 150, 150, 182, 110, 182};
        for (std::size_t i = 0; i < 8; ++i) QVERIFY(std::abs(moved->placed->corners[i] - expected[i]) < 1e-9);
        QCOMPARE(std::string(again.linked("uuid-1")->item), made.item);  // (its contents unchanged: the item as read)
    }

    // the PSD records a book keeps checked (validate_smart): the layer's {block, raw, shown?}, the contents' {uuid, item, at}
    void psdRecordsChecked() {
        const PlacedPsd made = placed_psd();
        const core::Document doc =
            run(small_book(), Json::array({Json{{"op", "import_psd"}, {"page", 1}, {"psd", core::b64encode(made.bytes)}, {"fit", "paper"}, {"id", "ps"}}}));
        core::smart::validate_smart(doc);
        const auto refused = [](const core::Document& bad, const char* words) {
            try {
                core::smart::validate_smart(bad);
            } catch (const core::Error& error) {
                return QString::fromUtf8(error.what()).contains(QString::fromUtf8(words));
            }
            return false;
        };
        const auto with_layer = [&](const std::function<void(Json&)>& change) {
            core::Document bad = doc;
            for (core::Layer& l : bad.edit_page(0).layers)
                if (l.id == "ps-2") change(*l.smart);
            return bad;
        };
        QVERIFY(refused(with_layer([](Json& s) { s["psd"]["block"] = "XXXX"; }), "SoLd, SoLE or PlLd"));
        QVERIFY(refused(with_layer([](Json& s) { s["psd"]["raw"] = "x"; }), "raw is an asset"));
        QVERIFY(refused(with_layer([](Json& s) { s["psd"]["shown"] = Json{{"png", "x"}}; }), "{asset} or {ink_raster}"));
        QVERIFY(refused(with_layer([](Json& s) { s["psd"]["extra"] = 1; }), "is not known"));
        core::Document no_item = doc;
        const std::string id = no_item.smart_contents.begin()->first;
        no_item.edit_contents(id).psd->erase("item");
        QVERIFY(refused(no_item, "its psd's item"));
        // (smart_profile lets Photoshop's picture go, keeping the block)
        const core::Document kept = run(doc, Json::array({Json{{"op", "smart_profile"}, {"page", 1}, {"id", "ps-2"}, {"embedded_profile", "keep"}}}));
        for (const core::Layer& l : kept.page(0).layers) {
            if (l.id != "ps-2") continue;
            QVERIFY((*l.smart)["psd"].contains("raw") && !(*l.smart)["psd"].contains("shown"));
        }
    }

    // a PSD placed as a smart object's file, made a page to edit (smart_to_page): its layers each a layer of the page, its
    // placed layer a smart object inside it
    void aPsdFilesLayersToEdit() {
        const PlacedPsd made = placed_psd();
        core::Document doc = small_book();
        doc = run(doc, Json::array({Json{{"op", "smart_place"}, {"page", 1}, {"data", core::b64encode(made.bytes)}, {"name", "art.psd"}, {"new_id", "so"}}}));
        const core::ApplyResult result = applied(doc, Json::array({Json{{"op", "smart_to_page"}, {"page", 1}, {"id", "so"}, {"new_id", "in"}}}));
        doc = result.doc;
        const core::Layer* so = nullptr;
        for (const core::Layer& l : doc.page(0).layers)
            if (l.id == "so") so = &l;
        QVERIFY(so != nullptr);
        const core::SmartContents* contents = core::smart::contents_of(doc, *so);
        QVERIFY(contents != nullptr && contents->kind == "page");
        QCOMPARE(contents->page->layers.size(), std::size_t(2));
        QCOMPARE(contents->page->layers[0].title, std::string("bg"));
        QCOMPARE(contents->page->layers[1].title, std::string("placed"));
        QVERIFY(contents->page->layers[1].kind == core::LayerKind::Smart);
        QCOMPARE(result.results.at(0)["layer"], Json("in-2"));
        core::smart::validate_smart(doc);
        // (its page the file's pixels at 200 dpi)
        QVERIFY(std::abs(contents->page->spec.width_mm.value() - 400 * 25.4 / 200) < 1e-9);
    }

    // a placed layer whose file the PSD does not hold (an external link), or a file this build cannot read: its pixels,
    // said in the reply; a book without placed layers as before
    void placedLayersReadAsPixels() {
        PlacedPsd made = placed_psd();
        std::string bytes = made.bytes;
        const std::size_t at = bytes.find("8BIMlnk2");
        QVERIFY(at != std::string::npos);
        bytes.replace(at + 4, 4, "lnkX");  // (another global block: no file for it)
        core::Document doc = small_book();
        const core::ApplyResult result = applied(doc, Json::array({Json{{"op", "import_psd"}, {"page", 1}, {"psd", core::b64encode(bytes)}, {"id", "ps"}}}));
        const Json report = result.results.at(0);
        QVERIFY2(!report.contains("smart_objects"), report.dump().c_str());
        QCOMPARE(report["placed_as_pixels"], Json::array({Json{{"id", "ps-2"}, {"why", "no_file"}}}));
        for (const core::Layer& l : result.doc.page(0).layers) QVERIFY(l.kind != core::LayerKind::Smart);
        QVERIFY(result.doc.smart_contents.empty());
        // (a file not a picture this build reads: its pixels)
        made = placed_psd();
        std::string other = made.bytes;
        const std::size_t png = other.find("\x89PNG");
        QVERIFY(png != std::string::npos);
        other[png + 1] = 'X';
        const core::ApplyResult unread = applied(doc, Json::array({Json{{"op", "import_psd"}, {"page", 1}, {"psd", core::b64encode(other)}, {"id", "ps"}}}));
        QCOMPARE(unread.results.at(0)["placed_as_pixels"][0]["why"], Json("unreadable"));
    }

    // PC4 D7 (the decision 2 (b)): Photoshop's smart filters of a placed layer — each in the table — read as Genko's: their
    // settings in Genko's units (a 4-pixel Gaussian of a file laid pixel for pixel on a page of 100 dpi: 8 working pixels),
    // blend modes, opacities and whether each shows, the stack on or off; Photoshop's picture no longer shown (drawn from
    // its file through them); the PSD written back with the block it was read with; said when opened.
    void photoshopsSmartFiltersRead() {
        Writer unsharp;
        unsharp.unit("Amnt", "#Prc", 80);
        unsharp.unit("Rds ", "#Pxl", 1.5);
        unsharp.integer("Thsh", 2);
        Writer wind;
        wind.enumerated("WndM", "WndM", "Blst");
        wind.enumerated("Drct", "Drct", "Left");
        const Writer fx = filter_fx({filter_item("ぼかし (ガウス)", std::make_pair(std::string("GsnB"), gaussian(4)), id4("GsnB")),
                                     filter_item("アンシャープマスク", std::make_pair(std::string("UnsM"), unsharp), id4("UnsM"), "Mltp", 60),
                                     filter_item("風", std::make_pair(std::string("Wnd "), wind), id4("Wnd "), "Nrml", 100, false),
                                     filter_item("ファセット", std::nullopt, id4("Fct "))});
        const PlacedPsd made = placed_psd({100, 150, 140, 150, 140, 182, 100, 182}, sold_with(fx));
        const core::ApplyResult result = imported(made.bytes);
        const Json report = result.results.at(0);
        QVERIFY2(report.contains("smart_filters_read") && !report.contains("smart_filters_shown"), report.dump().c_str());
        QCOMPARE(report["smart_filters_read"], Json::array({"ps-2"}));
        const core::Document& doc = result.doc;
        const core::Layer* so = layer_of(doc, "ps-2");
        QVERIFY(so != nullptr && so->kind == core::LayerKind::Smart);
        const Json& filters = (*so->smart)["filters"];
        QCOMPARE(filters.size(), std::size_t(4));
        QCOMPARE(filters[0]["kind"], Json("blur_gaussian"));
        QVERIFY(std::abs(filters[0]["params"]["radius"].get<double>() - 8) < 1e-9);
        QCOMPARE(filters[1]["kind"], Json("sharpen_unsharp_mask"));
        QCOMPARE(filters[1]["params"]["amount"].get<double>(), 80.0);
        QVERIFY(std::abs(filters[1]["params"]["radius"].get<double>() - 3) < 1e-9);
        QCOMPARE(filters[1]["params"]["threshold"].get<double>(), 2.0);
        QCOMPARE(filters[1]["blend"], Json("multiply"));
        QCOMPARE(filters[1]["opacity"].get<double>(), 0.6);
        QCOMPARE(filters[2]["kind"], Json("stylize_wind"));
        QCOMPARE(filters[2]["params"]["method"], Json("blast"));
        QCOMPARE(filters[2]["params"]["direction"], Json("from_left"));
        QCOMPARE(filters[2]["visible"], Json(false));
        QCOMPARE(filters[3]["kind"], Json("pixelate_facet"));
        QVERIFY(!(*so->smart).contains("filters_on"));
        QVERIFY(!(*so->smart)["psd"].contains("shown"));
        QCOMPARE(*doc.smart_files.at((*so->smart)["psd"]["raw"].get<std::string>()), made.sold);
        core::smart::validate_smart(doc);
        QVERIFY(core::uses_filters(doc));
        // drawn from its file (red | green, blurred), not Photoshop's blue picture
        const render::Image page = page_picture(doc);
        const std::vector<double> left = at(page, 110, 166);
        QVERIFY2(left[2] < 100 && std::max(left[0], left[1]) > 100, (QByteArray::number(left[0]) + " " + QByteArray::number(left[1]) + " " + QByteArray::number(left[2])).constData());
        // the PSD written: the block it was read with (Photoshop's filters with it), the smart filters' picture said
        QTemporaryDir dir;
        const fs::path out = core::path_from_utf8(dir.filePath(QStringLiteral("p.psd")).toStdString());
        genko::formats::export_page_psd(doc, doc.page(0), out, 100);
        const psd::File written = psd::File::read(genko::test::read_bytes(dir.filePath(QStringLiteral("p.psd"))));
        const psd::Layer* placed = nullptr;
        for (const psd::Layer& l : written.layers())
            if (l.kind == "smart") placed = &l;
        QVERIFY(placed != nullptr && placed->placed && placed->placed->filters);
        QCOMPARE(std::string(written.placed_block(*placed)), made.sold);
        // (review P2-5: Photoshop's filters unchanged go back with it — not burnt in, said so)
        QVERIFY(!noted(genko::formats::psd_smart_notes(doc), "焼き込んで"));
        QVERIFY(noted(genko::formats::psd_smart_notes(doc), "読んだブロックのまま書き戻しました"));
        QVERIFY((*so->smart)["psd"].contains("filters_read"));
        // (changed — a radius of the Gaussian, or one more filter —: this build's block without Photoshop's filters, the
        // filters in the layer's pixels, said)
        for (const Json& op : {Json{{"op", "smart_filter_set"}, {"page", 1}, {"id", "ps-2"}, {"index", 0}, {"params", Json{{"radius", 3}}}},
                               Json{{"op", "smart_filter_add"}, {"page", 1}, {"id", "ps-2"}, {"kind", "noise_median"}, {"radius", 2}}}) {
            const core::Document changed = run(doc, Json::array({op}));
            const core::Layer* edited = layer_of(changed, "ps-2");
            QVERIFY(edited && !core::smart::photoshop_filters_kept(*edited));
            core::smart::validate_smart(changed);
            genko::formats::export_page_psd(changed, changed.page(0), out, 100);
            const psd::File again = psd::File::read(genko::test::read_bytes(dir.filePath(QStringLiteral("p.psd"))));
            const psd::Layer* rewritten = nullptr;
            for (const psd::Layer& l : again.layers())
                if (l.kind == "smart") rewritten = &l;
            QVERIFY2(rewritten != nullptr && rewritten->placed && !rewritten->placed->filters, op.dump().c_str());
            QVERIFY(std::string(again.placed_block(*rewritten)) != made.sold);
            QCOMPARE(rewritten->placed->uuid, std::string("uuid-1"));
            QVERIFY(noted(genko::formats::psd_smart_notes(changed), "焼き込んで"));
            QVERIFY(!noted(genko::formats::psd_smart_notes(changed), "読んだブロックのまま"));
        }
        // (the stack off: read off)
        const PlacedPsd off = placed_psd({100, 150, 140, 150, 140, 182, 100, 182},
                                         sold_with(filter_fx({filter_item("g", std::make_pair(std::string("GsnB"), gaussian(2)), id4("GsnB"))}, false, false)));
        const core::ApplyResult read_off = imported(off.bytes);
        const core::Layer* so_off = layer_of(read_off.doc, "ps-2");
        QVERIFY(so_off && (*so_off->smart)["filters_on"] == Json(false));
        // opened: said
        const genko::formats::Opened opened = genko::formats::open_file(made.bytes, "filtered.psd");
        QVERIFY2(noted(opened.notes, "Photoshop のスマートフィルターを Genko のスマートフィルターとして読みました"), opened.notes.empty() ? "" : opened.notes[0].c_str());
    }

    // PC4 F-e (the coordinator's decision on D7's overlap): a smart object of a camera RAW — Photoshop's Camera Raw settings
    // not read — keeps Photoshop's picture though its smart filters are read, said when opened; set_camera_raw develops it
    // with Genko's development from then on.
    void aRawsSmartObjectKeepsPhotoshopsPicture() {
        const std::size_t w = 40, h = 32;
        std::vector<std::array<float, 3>> rgb(w * h, std::array<float, 3>{0.15f, 0.3f, 0.21f});
        rawgen::DngSpec dng = rawgen::DngSpec::cfa_of(w, h, rawgen::mosaic(rgb, w, {0, 1, 1, 2}, 64, 4000));
        dng.bits = 12;
        dng.white = 4000;
        dng.black = {64};
        dng.as_shot_neutral = std::array<double, 3>{0.5, 1.0, 0.7};
        dng.color_matrix1 = std::make_pair(std::uint16_t(21), std::array<double, 9>{3.2404542, -1.5371385, -0.4985314, -0.9692660, 1.8760108, 0.0415560,
                                                                                     0.0556434, -0.2040259, 1.0572252});
        const PlacedPsd made = placed_psd({100, 150, 140, 150, 140, 182, 100, 182},
                                          sold_with(filter_fx({filter_item("g", std::make_pair(std::string("GsnB"), gaussian(1)), id4("GsnB"))})), {},
                                          dng.build(), "in.dng");
        const core::ApplyResult result = imported(made.bytes);
        const Json report = result.results.at(0);
        QVERIFY2(report.contains("smart_filters_read") && report.contains("smart_raw_shown"), report.dump().c_str());
        const core::Document& doc = result.doc;
        const core::Layer* so = layer_of(doc, "ps-2");
        QVERIFY(so != nullptr && so->kind == core::LayerKind::Smart);
        QCOMPARE((*so->smart)["filters"].size(), std::size_t(1));
        QVERIFY((*so->smart)["psd"].contains("shown"));
        const core::SmartContents* contents = core::smart::contents_of(doc, *so);
        QVERIFY(contents != nullptr && contents->format == "raw" && contents->raw);
        core::smart::validate_smart(doc);
        // (Photoshop's blue picture)
        const std::vector<double> before = at(page_picture(doc), 110, 166);
        QVERIFY2(before[2] > 200 && before[0] < 60, (QByteArray::number(before[0]) + " " + QByteArray::number(before[2])).constData());
        const genko::formats::Opened opened = genko::formats::open_file(made.bytes, "raw.psd");
        QVERIFY(noted(opened.notes, "の中身はカメラ RAW のため、Photoshop の見えのまま表示します"));
        // (developed: Genko's development of the grey, not Photoshop's blue)
        const std::string id = *core::smart::contents_id(*so);
        const core::Document developed = run(doc, Json::array({Json{{"op", "set_camera_raw"}, {"contents", id}, {"develop", Json{{"exposure_ev", 0.5}}}}}));
        QVERIFY(!(*layer_of(developed, "ps-2")->smart)["psd"].contains("shown"));
        const std::vector<double> after = at(page_picture(developed), 110, 166);
        QVERIFY2(std::abs(after[2] - after[0]) < 40 && after[2] < 200, (QByteArray::number(after[0]) + " " + QByteArray::number(after[2])).constData());
    }

    // PC4 D7: a stack of which one filter is not in the table — a filter Genko has no kind of, settings it cannot take (a
    // wave's own scale), a key this build does not know, a blend mode Genko has not —, more filters than a smart object
    // holds, or its filter mask on and hiding something (the file's filter effects say so, or cannot be read), is not read
    // at all: Photoshop's picture shown as before, the import saying which and why, the PSD written back the same.
    void aStackNotInTheTableShowsPhotoshopsPicture() {
        Writer liquify;
        liquify.raw("LqMe", std::string(16, '\x01'));
        Writer wave;
        wave.enumerated("Wvtp", "Wvtp", "WvSn");
        for (const char* key : {"NmbG", "WLMn", "WLMx", "AmMn", "AmMx", "RndS"}) wave.integer(key, 5);
        wave.integer("SclH", 50);
        wave.integer("SclV", 100);
        wave.enumerated("UndA", "UndA", "RptE");
        Writer extra = gaussian(2);
        extra.boolean("Prvw", true);
        const auto g = [](const std::string& name, std::string_view blend = "Nrml") {
            return filter_item(name, std::make_pair(std::string("GsnB"), gaussian(2)), id4("GsnB"), blend);
        };
        struct Case {
            std::string name;
            Writer fx;
            std::string more;
            std::string why;
            std::vector<std::string> named;
        };
        std::vector<Writer> many;
        for (int i = 0; i < 65; ++i) many.push_back(g("g"));
        const std::string painted(40 * 32, '\x80'), white(40 * 32, '\xff');
        std::string rle;  // (32 rows of 40 × 255 in PackBits: one run each, its count 2 bytes)
        for (int r = 0; r < 32; ++r) build::u16(rle, 2);
        for (int r = 0; r < 32; ++r) rle += std::string("\xd9\xff", 2);
        std::vector<Case> cases{
            {"liquify", filter_fx({g("ガウス"), filter_item("ゆがみ", std::make_pair(std::string("LqFy"), liquify), id4("LqFy"))}), {}, "unknown", {"ゆがみ"}},
            {"wave's scale", filter_fx({filter_item("波形", std::make_pair(std::string("Wave"), wave), id4("Wave"))}), {}, "unknown", {"波形"}},
            {"a key not known", filter_fx({filter_item("ガウス+", std::make_pair(std::string("GsnB"), extra), id4("GsnB"))}), {}, "unknown", {"ガウス+"}},
            {"vivid light", filter_fx({g("ビビッド", "vividLight")}), {}, "unknown", {"ビビッド"}},
            {"65 filters", filter_fx(many), {}, "too_many", {}},
            {"a painted mask", filter_fx({g("g")}, true), filter_effects("placed-1", std::make_pair(0u, painted)), "mask", {}},
            {"a mask of another layer painted", filter_fx({g("g")}, true), filter_effects("someone-else", std::make_pair(0u, painted)), "mask", {}},
            {"filter effects unread", filter_fx({g("g")}, true), build::block("FEid", std::string("\0\0\0\x09garbage!", 12)), "mask", {}},
            // (review P3-1: what cannot be told — its entry not found, its mask not written, a white rectangle not covering
            // the layer — may hide something)
            {"no entry of its own", filter_fx({g("g")}, true), filter_effects("someone-else", std::make_pair(0u, white)), "mask", {}},
            {"its mask not written", filter_fx({g("g")}, true), filter_effects("uuid-1", std::nullopt), "mask", {}},
            {"a white mask short of the layer", filter_fx({g("g")}, true), filter_effects("placed-1", std::make_pair(0u, std::string(20 * 32, '\xff')), 20, 32), "mask", {}},
        };
        for (const Case& c : cases) {
            const PlacedPsd made = placed_psd({100, 150, 140, 150, 140, 182, 100, 182}, sold_with(c.fx), c.more);
            const core::ApplyResult result = imported(made.bytes);
            const Json report = result.results.at(0);
            QVERIFY2(!report.contains("smart_filters_read") && report.contains("smart_filters_shown"), (c.name + " " + report.dump()).c_str());
            const Json& shown = report["smart_filters_shown"][0];
            QCOMPARE(shown["id"], Json("ps-2"));
            QCOMPARE(shown["why"], Json(c.why));
            if (!c.named.empty()) QCOMPARE(shown["filters"], Json(c.named));
            const core::Layer* so = layer_of(result.doc, "ps-2");
            QVERIFY2(so && so->kind == core::LayerKind::Smart && !(*so->smart).contains("filters") && (*so->smart)["psd"].contains("shown"), c.name.c_str());
            QCOMPARE(at(page_picture(result.doc), 110, 160), (std::vector<double>{0, 0, 255}));  // (Photoshop's picture)
            core::smart::validate_smart(result.doc);
        }
        // (a mask on that hides nothing — its own, white over the layer, raw or PackBits, found by its layer's id or its
        // contents' —: read)
        for (const std::string& more : {filter_effects("placed-1", std::make_pair(0u, white)), filter_effects("placed-1", std::make_pair(1u, rle)),
                                        filter_effects("uuid-1", std::make_pair(0u, white))}) {
            const Json report = imported(placed_psd({100, 150, 140, 150, 140, 182, 100, 182}, sold_with(filter_fx({g("g")}, true)), more).bytes).results.at(0);
            QVERIFY2(report.contains("smart_filters_read"), report.dump().c_str());
        }
        // (a mask off: whatever the filter effects say)
        const Json off = imported(placed_psd({100, 150, 140, 150, 140, 182, 100, 182}, sold_with(filter_fx({g("g")}, false)),
                                             filter_effects("placed-1", std::make_pair(0u, painted)))
                                      .bytes)
                             .results.at(0);
        QVERIFY2(off.contains("smart_filters_read"), off.dump().c_str());
        // opened: the smart object named, the filters not read named
        const PlacedPsd made = placed_psd({100, 150, 140, 150, 140, 182, 100, 182}, sold_with(cases[0].fx));
        const genko::formats::Opened opened = genko::formats::open_file(made.bytes, "liquify.psd");
        QVERIFY2(noted(opened.notes, "ゆがみ") && noted(opened.notes, "Photoshop の絵のまま"), opened.notes.empty() ? "" : opened.notes.back().c_str());
    }

    // PC4 D7: hostile descriptors and filter effects refused before what they claim is made — one descriptor budget for a
    // whole file (render/psd_descriptor's 512 MiB) shared by its placed layers' filters, a list claiming more items than its
    // bytes, filter effects claiming more than their block or more entries than kept —; the names said at most kNamesMax.
    void hostileFiltersAreBounded() {
        const std::string sold = sold_with(filter_fx({filter_item("g", std::make_pair(std::string("GsnB"), gaussian(2)), id4("GsnB"))}));
        const auto placed = psd::read_placed("SoLd", sold);
        QVERIFY(placed && placed->filters);
        const render::psd_filters::Placement where{1, 1, 0, 0, 400, 600, 101.6, 152.4, 400, 600};
        const render::psd_filters::Masks none;
        render::psd_filters::Budget budget;
        const auto read = render::psd_filters::read(sold, *placed, where, none, budget);
        QVERIFY(read.filters && read.filters->size() == 1);
        QVERIFY(budget.left < psd::descriptor::kUnpackedBudget);  // (spent)
        // (the budget spent: what follows refused, the budget not started again)
        render::psd_filters::Budget little{200};
        QCOMPARE(render::psd_filters::read(sold, *placed, where, none, little).why, std::string("unreadable"));
        QCOMPARE(little.left, std::size_t(0));
        QCOMPARE(render::psd_filters::read(sold, *placed, where, none, little).why, std::string("unreadable"));
        // (a list claiming a billion filters: refused at once)
        std::string claimed = sold;
        const std::size_t list = claimed.find("filterFXListVlLs");
        QVERIFY(list != std::string::npos);
        for (int k = 0; k < 4; ++k) claimed[list + 16 + k] = k == 0 ? '\x40' : '\0';
        QElapsedTimer clock;
        clock.start();
        render::psd_filters::Budget whole;
        QCOMPARE(render::psd_filters::read(claimed, *placed, where, none, whole).why, std::string("unreadable"));
        QVERIFY(clock.elapsed() < 1000);
        // (many filters not in the table: kNamesMax named, the rest counted)
        std::vector<Writer> unknown;
        for (int i = 0; i < 20; ++i) unknown.push_back(filter_item("x" + std::to_string(i), std::make_pair(std::string("LqFy"), Writer()), id4("LqFy")));
        render::psd_filters::Budget more;
        const auto named = render::psd_filters::read(sold_with(filter_fx(unknown)), *placed, where, none, more);
        QCOMPARE(named.why, std::string("unknown"));
        QCOMPARE(named.names.size(), render::psd_filters::kNamesMax);
        QCOMPARE(named.more, std::size_t(20) - render::psd_filters::kNamesMax);
        // filter effects: an entry past its block, a mask's PackBits past its rows, more entries than kept
        const auto masks_of = [](const std::string& global) {
            const std::string bytes = placed_psd({100, 150, 140, 150, 140, 182, 100, 182}, {}, global).bytes;
            const psd::File file = psd::File::read(bytes);
            return render::psd_filters::read_masks(file.global_blocks());
        };
        std::string past;
        build::u32(past, 3);
        build::u64(past, 1ull << 40);
        QVERIFY(masks_of(build::block("FEid", past)).unread);
        std::string bad_rle;
        for (int r = 0; r < 32; ++r) build::u16(bad_rle, 2);
        for (int r = 0; r < 32; ++r) bad_rle += std::string("\x7f\xff", 2);  // (a literal run of 128 claiming bytes it has not)
        const render::psd_filters::Masks rle = masks_of(filter_effects("placed-1", std::make_pair(1u, bad_rle)));
        QVERIFY(!rle.unread && rle.entries.at("placed-1").hides);
        std::string entries;
        build::u32(entries, 3);
        std::string one;
        build::u8(one, 1);
        one += "e";
        build::u32(one, 1);
        build::u64(one, 0);
        build::u8(one, 0);
        for (std::size_t i = 0; i <= render::psd_filters::kMaskEntriesMax; ++i) {
            build::u64(entries, one.size());
            entries += one;
            while (entries.size() % 4 != 0) entries += '\0';
        }
        QVERIFY(masks_of(build::block("FEid", entries)).unread);
    }

    // PC4 D7: psd-tools reads the placed layer's SoLd made with Photoshop's smart filters (filterFX: its list, each filter's
    // descriptor and blending options) — the files these tests read are Photoshop's form.
    void psdToolsReadsTheFilters() {
        QVERIFY2(!genko::test::python_ref().isEmpty(), "the reference Python (GENKO_PYREF) reads the PSD with psd-tools");
        Writer unsharp;
        unsharp.unit("Amnt", "#Prc", 80);
        unsharp.unit("Rds ", "#Pxl", 1.5);
        unsharp.integer("Thsh", 2);
        const PlacedPsd made = placed_psd({100, 150, 140, 150, 140, 182, 100, 182},
                                          sold_with(filter_fx({filter_item("ぼかし (ガウス)", std::make_pair(std::string("GsnB"), gaussian(4)), id4("GsnB")),
                                                               filter_item("アンシャープマスク", std::make_pair(std::string("UnsM"), unsharp), id4("UnsM"), "Mltp", 60)},
                                                              true)));
        QTemporaryDir dir;
        const QString file = dir.filePath(QStringLiteral("filtered.psd"));
        genko::test::write_bytes(file, made.bytes);
        constexpr const char* kRead = R"PY(
import json, sys
from psd_tools import PSDImage
from psd_tools.constants import Tag
def name(k):
    v = getattr(k, "value", k)
    return v.decode("latin-1") if isinstance(v, bytes) else str(v)
def get(d, key):
    for k, v in d.items():
        if name(k) == key:
            return v
    raise KeyError(key)
psd = PSDImage.open(sys.argv[2])
out = {}
for layer in psd.descendants():
    tb = layer.tagged_blocks
    if tb is None or Tag.SMART_OBJECT_LAYER_DATA1 not in tb:
        continue
    d = tb.get_data(Tag.SMART_OBJECT_LAYER_DATA1).data
    fx = get(d, "filterFX")
    out["mask"] = bool(get(fx, "filterMaskEnable").value)
    out["filters"] = []
    for f in get(fx, "filterFXList"):
        fltr = get(f, "Fltr")
        options = get(f, "blendOptions")
        out["filters"].append({"class": name(fltr.classID), "name": str(get(f, "Nm  ").value).rstrip("\x00"),
                               "radius": float(get(fltr, "Rds ").value), "mode": name(get(options, "Md  ").enum),
                               "opacity": float(get(options, "Opct").value), "id": int(get(f, "filterID").value)})
json.dump(out, open(sys.argv[1], "w"), ensure_ascii=False)
)PY";
        const QString json = file + QStringLiteral(".json");
        const auto ran = genko::test::run(genko::test::python_ref(), {QStringLiteral("-c"), QString::fromUtf8(kRead), json, file},
                                          genko::test::python_env(dir.filePath(QStringLiteral("pyenv"))));
        QVERIFY2(ran.exit_code == 0, ran.described().toUtf8().constData());
        const Json view = genko::test::read_json(json);
        QCOMPARE(view["mask"], Json(true));
        QCOMPARE(view["filters"].size(), std::size_t(2));
        QCOMPARE(view["filters"][0]["class"], Json("GsnB"));
        QCOMPARE(view["filters"][0]["name"], Json("ぼかし (ガウス)"));
        QCOMPARE(view["filters"][0]["radius"].get<double>(), 4.0);
        QCOMPARE(view["filters"][0]["id"].get<std::int64_t>(), std::int64_t(id4("GsnB")));
        QCOMPARE(view["filters"][1]["class"], Json("UnsM"));
        QCOMPARE(view["filters"][1]["mode"], Json("Mltp"));
        QCOMPARE(view["filters"][1]["opacity"].get<double>(), 60.0);
    }
};

QTEST_MAIN(TestPsdSmart)
#include "test_psd_smart.moc"
