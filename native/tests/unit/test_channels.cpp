// channels@1 (SPEC.md COLOR-03, schema-v4.md「チャンネル（channels@1）」): a page's alpha and spot channels — their JSON,
// the channel ops (PhotoCraft v0.2.0's channel_cmds/tests.rs: the defaults, a channel duplicated and inverted, the list
// renumbered when one is deleted or moved, the options), the selection from a channel, the pixel ops on a channel or one
// colour plate, split and merge back byte for byte at 8 bits, Merge Spot Channel against PhotoCraft's formula, the spots
// printed over the page and the panel's view, the book saved and read back, without Python.

#include <QtTest>

#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "core/base64.hpp"
#include "core/channels.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ink.hpp"
#include "render/channels.hpp"
#include "render/image.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/selection.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/snapshot.hpp"
#include "storage/transaction.hpp"
#include "storage/writer.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
namespace fs = std::filesystem;

namespace {

core::ApplyResult run_ops(const core::Document& doc, const Json& ops) {
    return core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor("human:test"));
}

core::Document applied(const core::Document& doc, const Json& op) { return run_ops(doc, Json::array({op})).doc; }

Json report(const core::Document& doc, const Json& op) {
    const core::ApplyResult made = run_ops(doc, Json::array({op}));
    return made.results.empty() ? Json() : made.results[0];
}

std::string refusal(const core::Document& doc, const Json& op) {
    try {
        run_ops(doc, Json::array({op}));
    } catch (const core::ApplyError& error) {
        return error.code() + ": " + error.what();
    }
    return {};
}

// A book of `pages` pages (60 × 80 mm, 150 dpi), their panels taken away.
core::Document small_book(int pages = 1) {
    core::PageSpec spec;
    spec.width_mm = core::Num(60);
    spec.height_mm = core::Num(80);
    spec.dpi = core::Num(150);
    spec.bleed_mm = core::Num(3);
    spec.inner_margin_mm = core::Num(5);
    core::Document doc = core::new_episode("channels", core::Num(1), pages, spec, core::Binding::Right);
    for (std::size_t i = 0; i < doc.pages.size(); ++i) doc.edit_page(i).frames.clear();
    return doc;
}

constexpr int kW = 472, kH = 630;  // (60 × 80 mm at 200 dpi)

// A picture over the page at 200 dpi: a different colour at each pixel (x, y), opaque.
render::Image pattern() {
    std::string bytes(std::size_t(kW) * kH * 4, '\xff');
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const std::size_t at = (std::size_t(y) * kW + x) * 4;
            bytes[at] = static_cast<char>((x * 7 + y) & 255);
            bytes[at + 1] = static_cast<char>((y * 3) & 255);
            bytes[at + 2] = static_cast<char>((x + 2 * y) & 255);
        }
    }
    return render::Image::frombytes("RGBA", render::Size{kW, kH}, bytes);
}

// The book with a paint layer over its first page holding the picture.
core::Document painted(const render::Image& image, int pages = 1) {
    core::Document doc = small_book(pages);
    core::Layer layer;
    layer.id = "paint0000001";
    layer.kind = core::LayerKind::Raster;
    layer.title = "絵";
    layer.panel_clip = false;
    layer.raster_png = std::make_shared<const std::string>(render::write_png(image));
    doc.edit_page(0).layers.push_back(std::move(layer));
    return doc;
}

render::Image solid(int r, int g, int b) { return render::Image::create("RGBA", render::Size{kW, kH}, render::Ink{r, g, b, 255}); }

const core::Layer& layer_of(const core::Page& page, const std::string& id) {
    for (const core::Layer& layer : page.layers) {
        if (layer.id == id) return layer;
    }
    throw std::runtime_error("no layer " + id);
}

std::string pixels(const core::Layer& layer) { return render::open_image(*layer.raster_png).convert("RGBA").tobytes(); }

render::channels::Plane plane(const core::Page& page, std::size_t i) { return render::channels::plane_of(page, page.channels.at(i)); }

float at(const render::channels::Plane& p, int x, int y) { return p.values[std::size_t(y) * p.width + x]; }

Json left_half() { return Json{{"poly", Json::array({Json::array({0, 0}), Json::array({30, 0}), Json::array({30, 80}), Json::array({0, 80})})}}; }

void write_book(const core::Document& doc, const fs::path& dir) {
    storage::ProjectLock lock(dir, "human:test");
    lock.try_acquire();
    storage::SaveRequest request;
    request.actor = "human:test";
    request.ops = Json::array();
    storage::Saver(lock).save(doc, request);
}

}  // namespace

class TestChannels : public QObject {
    Q_OBJECT

private slots:
    // A channel as the writer writes it: its kind's keys in order (an alpha channel's overlay, a spot channel's ink), a
    // null raster for none; read back (in any order: the journal's snapshots sort their keys); what is not as the writer
    // writes it refused (unknown and missing keys, ids, names, ranges, two of a name, more than 50).
    void channelJson() {
        core::Channel alpha;
        alpha.id = "ch_3f2a9c0d1e2b";
        alpha.name = "Alpha 1";
        Json written = core::to_json(alpha, "");
        const auto keys = [](const Json& value) {
            std::vector<std::string> out;
            for (const auto& [key, item] : value.items()) out.push_back(key);
            return out;
        };
        QCOMPARE(keys(written), (std::vector<std::string>{"id", "name", "kind", "raster", "color", "opacity", "indicates"}));
        QCOMPARE(written["raster"], Json());
        QCOMPARE(written["color"], Json::array({255, 0, 0}));
        QCOMPARE(written["opacity"], Json(0.5));
        QCOMPARE(written["indicates"], Json("masked"));
        core::Channel spot;
        spot.id = "ch_77aa01bc22cd";
        spot.name = "特色 1";
        spot.kind = core::ChannelKind::Spot;
        spot.color = {230, 0, 18};
        const std::string ref = "sha256:" + std::string(64, 'a');
        written = core::to_json(spot, ref);
        QCOMPARE(keys(written), (std::vector<std::string>{"id", "name", "kind", "raster", "color", "solidity"}));
        QCOMPARE(written["raster"], Json(ref));
        QCOMPARE(written["kind"], Json("spot"));
        QCOMPARE(written["solidity"], Json(0.0));
        std::vector<std::string> refs;
        const std::vector<core::Channel> back = core::channels_from_json(Json::array({core::to_json(alpha, ""), written}), refs);
        QCOMPARE(back.size(), std::size_t{2});
        QCOMPARE(refs, (std::vector<std::string>{"", ref}));
        QCOMPARE(back[1].color, (std::array<int, 3>{230, 0, 18}));
        QVERIFY(back[1].kind == core::ChannelKind::Spot);
        // (sorted keys: read the same)
        Json sorted = Json::object();
        for (const char* key : {"color", "id", "indicates", "kind", "name", "opacity", "raster"}) sorted[key] = core::to_json(alpha, "")[key];
        QCOMPARE(core::channels_from_json(Json::array({sorted}), refs).front().name, std::string("Alpha 1"));
        const auto refused = [&](const Json& value) {
            try {
                core::channels_from_json(value, refs);
            } catch (const core::Error& error) {
                return error.code() == "value";
            }
            return false;
        };
        Json item = core::to_json(alpha, "");
        Json other = item;
        other["extra"] = 1;
        QVERIFY(refused(Json::array({other})));
        other = item;
        other.erase("indicates");
        QVERIFY(refused(Json::array({other})));
        other = item;
        other["solidity"] = 0.5;  // (a spot channel's key on an alpha channel)
        QVERIFY(refused(Json::array({other})));
        for (const auto& [key, value] : std::vector<std::pair<std::string, Json>>{{"id", "ch_XYZ"}, {"id", "3f2a9c0d1e2b"}, {"name", ""},
                                                                                {"name", " A"}, {"name", std::string(65, 'n')}, {"kind", "plate"},
                                                                                {"raster", "x"}, {"color", Json::array({256, 0, 0})},
                                                                                {"color", Json::array({1, 2})}, {"opacity", 1.5},
                                                                                {"indicates", "both"}}) {
            other = item;
            other[key] = value;
            QVERIFY2(refused(Json::array({other})), key.c_str());
        }
        other = item;
        other["id"] = "ch_000000000002";
        QVERIFY(refused(Json::array({item, other})));  // (two named Alpha 1)
        Json many = Json::array();
        for (int i = 0; i < 51; ++i) {
            core::Channel each = alpha;
            each.id = core::new_channel_id();
            each.name = "A" + std::to_string(i);
            many.push_back(core::to_json(each, ""));
        }
        QVERIFY(refused(many));
        many.erase(many.size() - 1);
        QCOMPARE(core::channels_from_json(many, refs).size(), std::size_t{50});
    }

    // add_channel: PhotoCraft's defaults (Alpha n red at 50 % over the masked areas; 特色 n, solidity 0), black is no
    // raster, white and an area at the channel's resolution (alpha 200 dpi, spot the book's), the next free number; the
    // refusals (the same name twice, more than 50, fill area without one, settings of the other kind, out of range).
    void addChannelDefaults() {
        core::Document doc = small_book();
        Json made = report(doc, Json{{"op", "add_channel"}, {"page", 1}});
        doc = applied(doc, Json{{"op", "add_channel"}, {"page", 1}});
        const core::Channel first = doc.page(0).channels.at(0);
        QVERIFY(core::is_channel_id(first.id));
        QVERIFY(core::is_channel_id(made["channel"].get<std::string>()));
        QCOMPARE(first.name, std::string("Alpha 1"));
        QVERIFY(first.kind == core::ChannelKind::Alpha);
        QCOMPARE(first.color, (std::array<int, 3>{255, 0, 0}));
        QCOMPARE(first.opacity, 0.5);
        QCOMPARE(first.indicates, std::string("masked"));
        QVERIFY(!first.raster);  // (black: all 0)
        doc = applied(doc, Json{{"op", "add_channel"}, {"page", 1}, {"fill", "white"}, {"color", Json::array({0, 0, 255})}, {"opacity", 0.25},
                              {"indicates", "selected"}});
        const core::Channel white = doc.page(0).channels.at(1);
        QCOMPARE(white.name, std::string("Alpha 2"));
        const core::InkRasterHeader header = core::ink_raster_header(*white.raster);
        QVERIFY(header.space == core::InkSpace::Mask);
        QCOMPARE(header.depth, 8);
        QCOMPARE(header.width, std::uint32_t(kW));
        QCOMPARE(header.height, std::uint32_t(kH));
        QCOMPARE(at(plane(doc.page(0), 1), 10, 10), 1.0f);
        doc = applied(doc, Json{{"op", "add_channel"}, {"page", 1}, {"kind", "spot"}, {"color", Json::array({230, 0, 18})}, {"solidity", 0.4},
                              {"area", left_half()}});
        const core::Channel spot = doc.page(0).channels.at(2);
        QCOMPARE(spot.name, std::string("特色 1"));
        QCOMPARE(spot.solidity, 0.4);
        const core::InkRasterHeader spot_header = core::ink_raster_header(*spot.raster);
        QCOMPARE(spot_header.width, std::uint32_t(354));  // (60 mm at the book's 150 dpi)
        const render::channels::Plane sp = plane(doc.page(0), 2);
        QCOMPARE(at(sp, 10, 200), 1.0f);  // (from the area: the left half)
        QCOMPARE(at(sp, 300, 200), 0.0f);
        // the next free number
        doc = applied(doc, Json{{"op", "rename_channel"}, {"page", 1}, {"channel", first.id}, {"name", "Mine"}});
        doc = applied(doc, Json{{"op", "add_channel"}, {"page", 1}});
        QCOMPARE(doc.page(0).channels.back().name, std::string("Alpha 1"));
        // refusals
        QVERIFY(refusal(doc, Json{{"op", "add_channel"}, {"page", 1}, {"name", "Mine"}}).find("already") != std::string::npos);
        QVERIFY(refusal(doc, Json{{"op", "add_channel"}, {"page", 1}, {"fill", "area"}}).find("needs the area") != std::string::npos);
        QVERIFY(!refusal(doc, Json{{"op", "add_channel"}, {"page", 1}, {"opacity", 1.5}}).empty());
        QVERIFY(!refusal(doc, Json{{"op", "add_channel"}, {"page", 1}, {"solidity", 0.5}}).empty());
        QVERIFY(!refusal(doc, Json{{"op", "add_channel"}, {"page", 1}, {"kind", "spot"}, {"opacity", 0.5}}).empty());
        QVERIFY(!refusal(doc, Json{{"op", "add_channel"}, {"page", 1}, {"kind", "plate"}}).empty());
        QVERIFY(!refusal(doc, Json{{"op", "add_channel"}, {"page", 1}, {"name", "  "}}).empty());
        core::Document full = small_book();
        Json ops = Json::array();
        for (int i = 0; i < 50; ++i) ops.push_back(Json{{"op", "add_channel"}, {"page", 1}});
        full = run_ops(full, ops).doc;
        QCOMPARE(full.page(0).channels.size(), std::size_t{50});
        QVERIFY(refusal(full, Json{{"op", "add_channel"}, {"page", 1}}).find("at most 50") != std::string::npos);
        // (none of it is in the public list: no unknown-key warnings for the channel ops)
        QVERIFY(run_ops(small_book(), Json::array({Json{{"op", "add_channel"}, {"page", 1}, {"fill", "white"}}})).warnings.empty());
    }

    // duplicate_channel (PhotoCraft's duplicate): a copy named "… のコピー" (a free name), inverted when asked; a colour
    // plate or the composite as an alpha channel; onto another page of the same size, or a new grey page; another size
    // refused. delete_channel and move_channel renumber the list; a plate is not deleted without to_multichannel.
    void duplicateDeleteMove() {
        core::Document doc = painted(solid(255, 0, 0));
        doc = applied(doc, Json{{"op", "add_channel"}, {"page", 1}, {"name", "W"}, {"area", left_half()}, {"fill", "area"}});
        const std::string w = doc.page(0).channels[0].id;
        doc = applied(doc, Json{{"op", "duplicate_channel"}, {"page", 1}, {"channel", "W"}});
        QCOMPARE(doc.page(0).channels[1].name, std::string("W のコピー"));
        QCOMPARE(at(plane(doc.page(0), 1), 10, 10), 1.0f);
        doc = applied(doc, Json{{"op", "duplicate_channel"}, {"page", 1}, {"channel", w}, {"invert", true}});
        QCOMPARE(doc.page(0).channels[2].name, std::string("W のコピー 2"));
        QCOMPARE(at(plane(doc.page(0), 2), 10, 10), 0.0f);
        QCOMPARE(at(plane(doc.page(0), 2), 400, 10), 1.0f);
        doc = applied(doc, Json{{"op", "duplicate_channel"}, {"page", 1}, {"channel", "red"}, {"name", "R"}});
        QCOMPARE(at(plane(doc.page(0), 3), 100, 100), 1.0f);  // (a red page: its red plate full)
        doc = applied(doc, Json{{"op", "duplicate_channel"}, {"page", 1}, {"channel", "green"}});
        QCOMPARE(doc.page(0).channels[4].name, std::string("グリーン のコピー"));
        QCOMPARE(at(plane(doc.page(0), 4), 100, 100), 0.0f);
        doc = applied(doc, Json{{"op", "duplicate_channel"}, {"page", 1}, {"channel", "composite"}});
        QCOMPARE(doc.page(0).channels[5].name, std::string("RGB のコピー"));
        QVERIFY(std::abs(at(plane(doc.page(0), 5), 100, 100) - 0.299f) < 0.003f);
        // onto another page; a page of another size refused; a new grey page
        doc = applied(doc, Json{{"op", "add_page"}});
        const Json onto = report(doc, Json{{"op", "duplicate_channel"}, {"page", 1}, {"channel", "W"}, {"to_page", 2}});
        QCOMPARE(onto["page"], Json(2));
        doc = applied(doc, Json{{"op", "duplicate_channel"}, {"page", 1}, {"channel", "W"}, {"to_page", 2}});
        QCOMPARE(doc.page(1).channels.size(), std::size_t{1});
        QCOMPARE(doc.page(1).channels[0].name, std::string("W のコピー"));
        core::Document sized = doc;
        sized.edit_page(1).spec.width_mm = core::Num(70);
        QVERIFY(refusal(sized, Json{{"op", "duplicate_channel"}, {"page", 1}, {"channel", "W"}, {"to_page", 2}}).find("not the same size") !=
                std::string::npos);
        const Json fresh = report(doc, Json{{"op", "duplicate_channel"}, {"page", 1}, {"channel", "W"}, {"to_page", "new"}});
        QCOMPARE(fresh["page"], Json(2));
        const core::Document with_new = applied(doc, Json{{"op", "duplicate_channel"}, {"page", 1}, {"channel", "W"}, {"to_page", "new"}});
        QCOMPARE(with_new.pages.size(), std::size_t{3});
        QVERIFY(with_new.page(1).color && with_new.page(1).color->space == core::InkSpace::Gray);
        QVERIFY(with_new.page(1).frames.empty());
        // move: the list renumbered, the place reported
        const std::string third = doc.page(0).channels[2].id;
        QCOMPARE(report(doc, Json{{"op", "move_channel"}, {"page", 1}, {"channel", w}, {"to", 99}})["channel"], Json(5));
        doc = applied(doc, Json{{"op", "move_channel"}, {"page", 1}, {"channel", w}, {"to", 99}});
        QCOMPARE(doc.page(0).channels.back().id, w);
        QCOMPARE(doc.page(0).channels[1].id, third);
        QVERIFY(!refusal(doc, Json{{"op", "move_channel"}, {"page", 1}, {"channel", w}, {"to", -1}}).empty());
        // delete: the others move up
        doc = applied(doc, Json{{"op", "delete_channel"}, {"page", 1}, {"channel", doc.page(0).channels[0].id}});
        QCOMPARE(doc.page(0).channels[0].id, third);
        QCOMPARE(doc.page(0).channels.size(), std::size_t{5});
        QVERIFY(refusal(doc, Json{{"op", "delete_channel"}, {"page", 1}, {"channel", "red"}}).find("to_multichannel") != std::string::npos);
        QVERIFY(refusal(doc, Json{{"op", "delete_channel"}, {"page", 1}, {"channel", "red"}, {"to_multichannel", true}}).starts_with("not_yet_ported"));
        QVERIFY(refusal(doc, Json{{"op", "delete_channel"}, {"page", 1}, {"channel", "nope"}}).find("no channel") != std::string::npos);
    }

    // rename_channel and set_channel (Channel Options): a name trimmed, empty or taken refused; the overlay's colour,
    // opacity and what it marks; an alpha channel made a spot one (its ink at solidity 0) and back; values out of range
    // refused; Undo is the book before.
    void renameAndOptions() {
        core::Document doc = small_book();
        doc = run_ops(doc, Json::array({Json{{"op", "add_channel"}, {"page", 1}}, Json{{"op", "add_channel"}, {"page", 1}}})).doc;
        const std::string a = doc.page(0).channels[0].id;
        QVERIFY(refusal(doc, Json{{"op", "rename_channel"}, {"page", 1}, {"channel", a}, {"name", " "}}).find("empty") != std::string::npos);
        QVERIFY(refusal(doc, Json{{"op", "rename_channel"}, {"page", 1}, {"channel", a}, {"name", "Alpha 2"}}).find("already") != std::string::npos);
        doc = applied(doc, Json{{"op", "rename_channel"}, {"page", 1}, {"channel", a}, {"name", "  空  "}});
        QCOMPARE(doc.page(0).channels[0].name, std::string("空"));
        doc = applied(doc, Json{{"op", "rename_channel"}, {"page", 1}, {"channel", "空"}, {"name", "空"}});  // (its own name)
        doc = applied(doc, Json{{"op", "set_channel"}, {"page", 1}, {"channel", a}, {"color", Json::array({0, 255, 0})}, {"opacity", 0.25},
                              {"indicates", "selected"}});
        const core::Channel c = doc.page(0).channels[0];
        QCOMPARE(c.color, (std::array<int, 3>{0, 255, 0}));
        QCOMPARE(c.opacity, 0.25);
        QCOMPARE(c.indicates, std::string("selected"));
        doc = applied(doc, Json{{"op", "set_channel"}, {"page", 1}, {"channel", a}, {"kind", "spot"}, {"solidity", 0.4}});
        QVERIFY(doc.page(0).channels[0].kind == core::ChannelKind::Spot);
        QCOMPARE(doc.page(0).channels[0].solidity, 0.4);
        QCOMPARE(doc.page(0).channels[0].color, (std::array<int, 3>{0, 255, 0}));
        QVERIFY(!refusal(doc, Json{{"op", "set_channel"}, {"page", 1}, {"channel", a}, {"opacity", 0.5}}).empty());
        QVERIFY(!refusal(doc, Json{{"op", "set_channel"}, {"page", 1}, {"channel", a}, {"solidity", -0.1}}).empty());
        QVERIFY(!refusal(doc, Json{{"op", "set_channel"}, {"page", 1}, {"channel", a}, {"color", Json::array({0, 0})}}).empty());
        doc = applied(doc, Json{{"op", "set_channel"}, {"page", 1}, {"channel", a}, {"kind", "alpha"}});
        QCOMPARE(doc.page(0).channels[0].opacity, 0.5);
        QCOMPARE(doc.page(0).channels[0].indicates, std::string("masked"));
    }

    // A selection from a channel ({"channel": id}): its density as the area's mask, at any resolution; an op's area of it
    // resolved by the bus; a channel the page has not refused.
    void selectionFromChannel() {
        core::Document doc = painted(solid(255, 255, 255));
        doc = applied(doc, Json{{"op", "add_channel"}, {"page", 1}, {"fill", "area"}, {"area", left_half()}});
        const std::string id = doc.page(0).channels[0].id;
        const render::Image mask = render::selection::to_mask(Json{{"channel", id}}, doc.page(0), &doc, 100);
        QCOMPARE(mask.getpixel(10, 100)[0], 255.0);
        QCOMPARE(mask.getpixel(200, 100)[0], 0.0);
        QVERIFY(core::area_needs_resolving(Json{{"channel", id}}));
        doc = applied(doc, Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", "paint0000001"}, {"area", Json{{"channel", id}}},
                              {"rgb", Json::array({0, 0, 0})}});
        const render::Image shown = render::render_page(doc.page(0), 100, render::proof_options(), &doc).image;
        QCOMPARE(shown.getpixel(10, 100)[0], 0.0);
        QCOMPARE(shown.getpixel(200, 100)[0], 255.0);
        // (save the selection to a channel: add_channel fill area; load: the area of the channel)
        doc = applied(doc, Json{{"op", "add_channel"}, {"page", 1}, {"fill", "area"}, {"area", Json{{"channel", id}, {"invert", true}}}});
        QCOMPARE(at(plane(doc.page(0), 1), 10, 100), 0.0f);
        QCOMPARE(at(plane(doc.page(0), 1), 400, 100), 1.0f);
        QVERIFY(refusal(doc, Json{{"op", "fill_area"}, {"page", 1}, {"area", Json{{"channel", "ch_000000000000"}}}}).find("no channel") !=
                std::string::npos);
    }

    // A pixel op's "channel": a channel worked as a grey picture (the fill, the eraser, a filter), the layers left as they
    // were; a colour plate alone (the other plates and the alpha as they were); "composite" and nothing as usual; a
    // channel the page has not refused.
    void pixelOpsOnChannels() {
        core::Document doc = painted(pattern());
        const std::string layer = "paint0000001";
        const std::string before = pixels(layer_of(doc.page(0), layer));
        doc = applied(doc, Json{{"op", "add_channel"}, {"page", 1}});
        const std::string id = doc.page(0).channels[0].id;
        doc = applied(doc, Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", layer}, {"channel", id}, {"area", left_half()},
                              {"rgb", Json::array({255, 255, 255})}});
        QCOMPARE(pixels(layer_of(doc.page(0), layer)), before);
        QCOMPARE(doc.page(0).layers.size(), small_book().page(0).layers.size() + 1);  // (no layer left over)
        render::channels::Plane p = plane(doc.page(0), 0);
        QCOMPARE(at(p, 10, 10), 1.0f);
        QCOMPARE(at(p, 400, 10), 0.0f);
        // the eraser on the channel: back to 0 where it went
        doc = applied(doc, Json{{"op", "erase_raster"}, {"page", 1}, {"layer_id", layer}, {"channel", id}, {"width_mm", 4},
                              {"points", Json::array({Json::array({5, 40}), Json::array({25, 40})})}});
        p = plane(doc.page(0), 0);
        QCOMPARE(at(p, 118, 315), 0.0f);
        QCOMPARE(at(p, 10, 10), 1.0f);
        QCOMPARE(pixels(layer_of(doc.page(0), layer)), before);
        // a filter on the channel (blur): the edge softened, the layer as it was
        doc = applied(doc, Json{{"op", "filter_raster"}, {"page", 1}, {"id", layer}, {"channel", id}, {"kind", "blur"}, {"radius", 3}});
        p = plane(doc.page(0), 0);
        const float edge = at(p, 236, 10);
        QVERIFY2(edge > 0.05f && edge < 0.95f, qPrintable(QString::number(edge)));
        QCOMPARE(pixels(layer_of(doc.page(0), layer)), before);
        // levels and curve keep their own channel (Python's r, g, b, else all three; its warning as Python gives it); a
        // channel's id there is the channel
        const core::ApplyResult levels = run_ops(doc, Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", layer}, {"kind", "levels"},
                                                                        {"black", 0}, {"white", 128}, {"channel", "r"}}}));
        QVERIFY(levels.warnings.dump().find("unknown keys ignored: black, white, channel") != std::string::npos);
        const std::string levelled = pixels(layer_of(levels.doc.page(0), layer));
        bool red_changed = false;
        for (std::size_t i = 0; i < levelled.size(); i += 4 * 97) {
            red_changed = red_changed || levelled[i] != before[i];
            QCOMPARE(levelled[i + 1], before[i + 1]);
            QCOMPARE(levelled[i + 2], before[i + 2]);
        }
        QVERIFY(red_changed);
        const core::Document on_channel = applied(doc, Json{{"op", "filter_raster"}, {"page", 1}, {"id", layer}, {"kind", "levels"}, {"black", 0},
                                                            {"white", 128}, {"channel", id}});
        QCOMPARE(pixels(layer_of(on_channel.page(0), layer)), before);
        const float raised = std::min(255.0f, std::round(edge * 255.0f) * 255.0f / 128.0f) / 255.0f;
        QVERIFY2(std::abs(at(plane(on_channel.page(0), 0), 236, 10) - raised) <= 2.0f / 255.0f,
                 qPrintable(QString("%1 %2").arg(at(plane(on_channel.page(0), 0), 236, 10)).arg(raised)));
        // a colour plate alone: the red plate filled, green, blue and the alpha as they were
        doc = applied(doc, Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", layer}, {"channel", "red"}, {"area", left_half()},
                              {"rgb", Json::array({255, 255, 255})}});
        const std::string now = pixels(layer_of(doc.page(0), layer));
        for (const auto& [x, y] : std::vector<std::pair<int, int>>{{10, 10}, {100, 300}, {200, 600}, {400, 300}}) {
            const std::size_t i = (std::size_t(y) * kW + x) * 4;
            const bool inside = x < 236;
            QCOMPARE(static_cast<unsigned char>(now[i]), inside ? 255 : static_cast<unsigned char>(before[i]));
            for (unsigned k = 1; k < 4; ++k) QCOMPARE(now[i + k], before[i + k]);
        }
        // composite: as usual; a channel the page has not: the key ignored as Python ignores it (pythonsWayWithTheChannelKey)
        const core::Document plain = applied(doc, Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", layer}, {"channel", "composite"},
                                                     {"area", left_half()}, {"rgb", Json::array({0, 0, 0})}});
        QVERIFY(!layer_of(plain.page(0), layer).patches.empty() || pixels(layer_of(plain.page(0), layer)) != now);
        QCOMPARE(refusal(doc, Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", layer}, {"channel", "Nope"}, {"area", left_half()}}), std::string());
        // (the channel key is this build's own on these ops: no unknown-key warning)
        QVERIFY(run_ops(doc, Json::array({Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", layer}, {"channel", id}, {"area", left_half()},
                                                {"rgb", Json::array({0, 0, 0})}}}))
                    .warnings.empty());
        // the pen on a channel: its line drawn into the channel
        doc = applied(doc, Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", layer}, {"channel", id}, {"width_mm", 3}, {"rgb", Json::array({255, 255, 255})},
                              {"points", Json::array({Json::array({40, 70}), Json::array({55, 70})})}});
        p = plane(doc.page(0), 0);
        QVERIFY2(at(p, 375, 551) > 0.9f, qPrintable(QString::number(at(p, 375, 551))));
        QVERIFY(layer_of(doc.page(0), layer).stroke_count() == 0);
    }

    // split_channels and merge_channels (PhotoCraft's Split / Merge Channels on pages): an RGB page split into three grey
    // pages (each a plate, opaque) and one for each channel, the original replaced (the user's decision) or kept; merged
    // back into an RGB page whose paint layer is the original pixels byte for byte at 8 bits; the refusals.
    void splitAndMerge() {
        const render::Image image = pattern();
        core::Document doc = painted(image, 2);
        doc = applied(doc, Json{{"op", "add_channel"}, {"page", 1}, {"name", "A"}, {"fill", "area"}, {"area", left_half()}});
        const Json split = report(doc, Json{{"op", "split_channels"}, {"page", 1}});
        QCOMPARE(split["pages"], Json::array({1, 2, 3, 4}));
        doc = applied(doc, Json{{"op", "split_channels"}, {"page", 1}});
        QCOMPARE(doc.pages.size(), std::size_t{5});  // (the original replaced: four pages, then the second)
        const std::string original = image.tobytes();
        for (std::size_t k = 0; k < 3; ++k) {
            const core::Page& page = doc.page(k);
            QVERIFY(page.color && page.color->space == core::InkSpace::Gray && page.color->depth == 8);
            const core::InkPixels grey = core::decode_ink_raster(*page.layers[1].ink_raster);
            QCOMPARE(grey.width, std::uint32_t(kW));
            for (std::size_t i = 0; i < grey.pixel_count(); i += 997) QCOMPARE(grey.samples[i * 2], std::uint16_t(static_cast<unsigned char>(original[i * 4 + k])));
            QCOMPARE(page.layers[1].title, std::string(k == 0 ? "レッド" : k == 1 ? "グリーン" : "ブルー"));
        }
        QCOMPARE(doc.page(3).layers[1].title, std::string("A"));
        // kept: the original stays before the pages made
        const core::Document kept = applied(painted(image), Json{{"op", "split_channels"}, {"page", 1}, {"replace", false}});
        QCOMPARE(kept.pages.size(), std::size_t{4});
        QVERIFY(!kept.page(0).color);
        // merged back: byte for byte, the alpha channel back as one, the grey pages replaced
        const Json merged_report = report(doc, Json{{"op", "merge_channels"}, {"pages", Json::array({1, 2, 3, 4})}, {"mode", "rgb"}});
        QCOMPARE(merged_report["page"], Json(1));
        doc = applied(doc, Json{{"op", "merge_channels"}, {"pages", Json::array({1, 2, 3, 4})}, {"mode", "rgb"}});
        QCOMPARE(doc.pages.size(), std::size_t{2});
        const core::Page& merged = doc.page(0);
        QVERIFY(!merged.color && !merged.rgb);
        const std::string back = render::open_image(*merged.layers[1].raster_png).convert("RGBA").tobytes();
        QCOMPARE(back.size(), original.size());
        const auto differ = std::mismatch(back.begin(), back.end(), original.begin());
        QVERIFY2(differ.first == back.end(), qPrintable(QString("pixel %1 byte %2: %3 against %4")
                                                             .arg((differ.first - back.begin()) / 4)
                                                             .arg((differ.first - back.begin()) % 4)
                                                             .arg(differ.first == back.end() ? 0 : int(static_cast<unsigned char>(*differ.first)))
                                                             .arg(differ.first == back.end() ? 0 : int(static_cast<unsigned char>(*differ.second)))));
        QCOMPARE(merged.channels.size(), std::size_t{1});
        QCOMPARE(merged.channels[0].name, std::string("Alpha 1"));
        QCOMPARE(at(plane(merged, 0), 10, 10), 1.0f);
        // refusals: too few, not grey, another size
        core::Document greys = applied(painted(image), Json{{"op", "split_channels"}, {"page", 1}});
        QVERIFY(refusal(greys, Json{{"op", "merge_channels"}, {"pages", Json::array({1, 2})}}).find("needs 3") != std::string::npos);
        core::Document rgb_page = applied(greys, Json{{"op", "add_page"}});
        QVERIFY(refusal(rgb_page, Json{{"op", "merge_channels"}, {"pages", Json::array({1, 2, 4})}}).find("not a grey page") != std::string::npos);
        greys.edit_page(2).spec.width_mm = core::Num(70);
        QVERIFY(refusal(greys, Json{{"op", "merge_channels"}, {"pages", Json::array({1, 2, 3})}}).find("not the same size") != std::string::npos);
        QVERIFY(refusal(small_book(), Json{{"op", "split_channels"}, {"page", 1}}).empty());
        // CMYK: the plates dark where there is ink; merged back into CMYK inks
        core::Document cmyk = applied(painted(solid(255, 255, 255)), Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}});
        cmyk = applied(cmyk, Json{{"op", "split_channels"}, {"page", 1}});
        QCOMPARE(cmyk.pages.size(), std::size_t{4});
        const core::InkPixels plate = core::decode_ink_raster(*cmyk.page(0).layers[1].ink_raster);
        QCOMPARE(plate.samples[0], std::uint16_t(255));  // (white paper: no cyan, light)
        cmyk = applied(cmyk, Json{{"op", "merge_channels"}, {"pages", Json::array({1, 2, 3, 4})}, {"mode", "cmyk"}});
        QVERIFY(cmyk.page(0).color && cmyk.page(0).color->space == core::InkSpace::Cmyk);
        QCOMPARE(cmyk.page(0).color->profile, core::builtin_profile("coated-cmyk")->ref());
    }

    // merge_spot_channel (the user's decision: as PhotoCraft — the page's pixel layers flattened into one, the spot
    // printed into it by c + (s·ink + (1 − s)·c·ink − c)·v, the channel gone) at solidity 0, 0.5 and 1, within 1/255;
    // a channel that is not a spot refused; Undo is the book before.
    void mergeSpot() {
        for (const double s : {0.0, 0.5, 1.0}) {
            core::Document doc = painted(solid(200, 150, 100));
            doc = applied(doc, Json{{"op", "add_channel"}, {"page", 1}, {"kind", "spot"}, {"color", Json::array({30, 90, 250})}, {"solidity", s},
                                  {"fill", "area"}, {"area", left_half()}});
            QVERIFY(refusal(doc, Json{{"op", "add_channel"}, {"page", 1}}).empty());
            const core::Document alpha = applied(doc, Json{{"op", "add_channel"}, {"page", 1}, {"name", "a"}});
            QVERIFY(refusal(alpha, Json{{"op", "merge_spot_channel"}, {"page", 1}, {"channel", "a"}}).find("not a spot") != std::string::npos);
            const Json made = report(doc, Json{{"op", "merge_spot_channel"}, {"page", 1}});
            doc = applied(doc, Json{{"op", "merge_spot_channel"}, {"page", 1}});
            QVERIFY(doc.page(0).channels.empty());
            const std::string flat_id = made["layer"].get<std::string>();
            const core::Layer& flat = layer_of(doc.page(0), flat_id);
            const std::string px = pixels(flat);
            const double c[3] = {200 / 255.0, 150 / 255.0, 100 / 255.0}, ink[3] = {30 / 255.0, 90 / 255.0, 250 / 255.0};
            for (unsigned k = 0; k < 3; ++k) {
                const double want = c[k] + (s * ink[k] + (1 - s) * c[k] * ink[k] - c[k]) * 1.0;
                QVERIFY2(std::abs(static_cast<unsigned char>(px[(100 * kW + 10) * 4 + k]) / 255.0 - want) <= 1.0 / 255.0 + 1e-9,
                         qPrintable(QString("s %1 k %2").arg(s).arg(k)));
                QCOMPARE(static_cast<unsigned char>(px[(100 * kW + 400) * 4 + k]), static_cast<unsigned char>(std::lround(c[k] * 255)));
            }
        }
        QVERIFY(refusal(small_book(), Json{{"op", "merge_spot_channel"}, {"page", 1}}).find("no spot channel") != std::string::npos);
    }

    // The spots printed over the page as it shows ((1 − s·v)·p·(1 − v + v·c) + s·v·c, PhotoCraft's multichannel display);
    // a part of the page the same pixels as the whole; spots=[] leaves them out (the exports that write them as plates);
    // the panel's view: one plate alone in grey, a plate hidden, an alpha channel laid over in its colour.
    void spotsAndView() {
        core::Document doc = painted(solid(200, 150, 100));
        doc = applied(doc, Json{{"op", "add_channel"}, {"page", 1}, {"kind", "spot"}, {"color", Json::array({30, 90, 250})}, {"solidity", 0.5},
                              {"fill", "area"}, {"area", left_half()}});
        render::RenderOptions options;
        const render::Image whole = render::render_page(doc.page(0), 100, options, &doc).image;
        const double c[3] = {200 / 255.0, 150 / 255.0, 100 / 255.0}, ink[3] = {30 / 255.0, 90 / 255.0, 250 / 255.0};
        const std::vector<double> got = whole.getpixel(20, 100);
        for (unsigned k = 0; k < 3; ++k) {
            const double want = (1 - 0.5) * c[k] * (1 - 1 + ink[k]) + 0.5 * ink[k];
            QVERIFY(std::abs(got[k] / 255.0 - want) <= 1.0 / 255.0 + 1e-9);
        }
        QCOMPARE(whole.getpixel(200, 100), (std::vector<double>{200, 150, 100}));
        render::RenderOptions part = options;
        part.region = render::RenderRegion{100, 50, 40, 60};
        QCOMPARE(render::render_page(doc.page(0), 100, part, &doc).image.tobytes(), whole.crop(render::Box{100, 50, 140, 110}).tobytes());
        render::RenderOptions none = options;
        none.spots = std::vector<std::string>{};
        QCOMPARE(render::render_page(doc.page(0), 100, none, &doc).image.getpixel(20, 100), (std::vector<double>{200, 150, 100}));
        // the view
        render::RenderOptions view = none;
        view.planes = std::vector<bool>{false, true, false};
        QCOMPARE(render::render_page(doc.page(0), 100, view, &doc).image.getpixel(200, 100), (std::vector<double>{150, 150, 150}));
        view.planes = std::vector<bool>{true, false, true};
        QCOMPARE(render::render_page(doc.page(0), 100, view, &doc).image.getpixel(200, 100), (std::vector<double>{200, 0, 100}));
        doc = applied(doc, Json{{"op", "add_channel"}, {"page", 1}, {"fill", "area"}, {"area", left_half()}, {"color", Json::array({0, 255, 0})},
                              {"opacity", 0.5}});
        render::RenderOptions laid = none;
        laid.overlays = {doc.page(0).channels[1].id};
        const render::Image over = render::render_page(doc.page(0), 100, laid, &doc).image;
        QCOMPARE(over.getpixel(20, 100), (std::vector<double>{200, 150, 100}));  // (selected: shown as it is)
        const std::vector<double> masked = over.getpixel(200, 100);  // (masked: half green)
        QVERIFY(masked[0] == 100 && std::abs(masked[1] - 202.5) <= 0.5 && masked[2] == 50);
        // (no colour plate shown: the first alpha channel laid over, in grey)
        laid.planes = std::vector<bool>{false, false, false};
        const render::Image alone = render::render_page(doc.page(0), 100, laid, &doc).image;
        QCOMPARE(alone.getpixel(20, 100), (std::vector<double>{255, 255, 255}));
        QCOMPARE(alone.getpixel(200, 100), (std::vector<double>{0, 0, 0}));
        // (an RGB page without channels: the same bytes as before)
        const core::Document bare = painted(pattern());
        render::RenderOptions plain;
        QCOMPARE(render::render_page(bare.page(0), 100, plain, &bare).image.tobytes(),
                 render::render_page(bare.page(0), 100, render::RenderOptions{}, &bare).image.tobytes());
    }

    // The book on disk: channels@1 named while a page has a channel (and not after), each raster an asset of its own,
    // read back the same; a channel at the page's depth (set_color_mode carries it: 8 → 16 is v × 257); set_page_spec
    // moves it with the layers; a page duplicated keeps its channels; the snapshot lists them.
    void bookOnDisk() {
        QTemporaryDir dir;
        core::Document doc = painted(pattern());
        doc = applied(doc, Json{{"op", "add_channel"}, {"page", 1}, {"fill", "area"}, {"area", left_half()}});
        doc = applied(doc, Json{{"op", "add_channel"}, {"page", 1}, {"kind", "spot"}, {"fill", "white"}});
        const fs::path book = fs::path(dir.path().toStdString()) / "b.genko";
        write_book(doc, book);
        const storage::LoadResult loaded = storage::load_document(book);
        QVERIFY2(loaded.report.clean(), loaded.report.to_json().dump().c_str());
        const core::Document& back = loaded.document;
        QVERIFY(std::find(back.features.begin(), back.features.end(), "channels@1") != back.features.end());
        QCOMPARE(back.page(0).channels.size(), std::size_t{2});
        QCOMPARE(*back.page(0).channels[0].raster, *doc.page(0).channels[0].raster);
        QCOMPARE(back.page(0).channels[1].name, std::string("特色 1"));
        const Json raw = Json::parse(genko::test::read_bytes(QString::fromStdString((book / "project.json").string())));
        QCOMPARE(raw["pages"][0]["channels"][0]["kind"], Json("alpha"));
        const std::string ref = raw["pages"][0]["channels"][0]["raster"].get<std::string>();
        QVERIFY(fs::exists(book / "assets" / ref.substr(7, 2) / (ref.substr(7) + ".gkir")));
        const Json snapshot = storage::snapshot(back);
        QCOMPARE(snapshot["pages"][0]["channels"].size(), std::size_t{2});
        QCOMPARE(snapshot["pages"][0]["channels"][0]["size"], Json::array({kW, kH}));
        QVERIFY(!storage::snapshot(small_book())["pages"][0].contains("channels"));
        // no channel left: not named
        core::Document none = run_ops(back, Json::array({Json{{"op", "delete_channel"}, {"page", 1}, {"channel", "Alpha 1"}},
                                                          Json{{"op", "delete_channel"}, {"page", 1}, {"channel", "特色 1"}}})).doc;
        const fs::path other = fs::path(dir.path().toStdString()) / "c.genko";
        write_book(none, other);
        QVERIFY(std::find(storage::load_document(other).document.features.begin(), storage::load_document(other).document.features.end(),
                          "channels@1") == storage::load_document(other).document.features.end());
        // the page's depth
        const core::Document deep = applied(doc, Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}});
        const core::InkPixels sixteen = core::decode_ink_raster(*deep.page(0).channels[0].raster);
        QCOMPARE(sixteen.depth, 16);
        QCOMPARE(sixteen.samples[0], std::uint16_t(65535));
        // set_page_spec: moved with the layers (the left half stays the left of the frame)
        const core::Document moved = applied(doc, Json{{"op", "set_page_spec"}, {"paper", Json::array({120, 160})}});
        const core::InkRasterHeader moved_header = core::ink_raster_header(*moved.page(0).channels[0].raster);
        QCOMPARE(moved_header.width, std::uint32_t(944));  // (the resolution it had over the new paper)
        // a page duplicated keeps them
        const core::Document twice = applied(doc, Json{{"op", "duplicate_page"}, {"page", 1}});
        QCOMPARE(twice.page(1).channels.size(), std::size_t{2});
        QCOMPARE(twice.page(1).channels[0].id, doc.page(0).channels[0].id);
    }

    // A spot channel at the book's 600 dpi worked by the pixel ops, which draw at the paint layers' 200 dpi: the channel
    // lent at 200 dpi and what the op changed put back at 600 — the eraser where it went (not a third of the way there),
    // delete_area and a filter within their areas, liquify about its brush —; what an op did not reach keeps its very
    // samples (the fine pattern of the channel too), and the lent layer is gone.
    void spotAtTheBooksResolution() {
        core::Document doc = painted(pattern());
        doc.edit_page(0).spec.dpi = core::Num(600);
        doc = applied(doc, Json{{"op", "add_channel"}, {"page", 1}, {"kind", "spot"}, {"fill", "white"}});
        const std::string id = doc.page(0).channels[0].id;
        const auto samples = [&](const core::Document& d) { return core::decode_ink_raster(*d.page(0).channels[0].raster); };
        const core::InkPixels white = samples(doc);
        QCOMPARE(white.width, std::uint32_t(1417));  // (60 mm at 600 dpi)
        QCOMPARE(white.height, std::uint32_t(1890));
        const auto px = [](double mm) { return static_cast<int>(mm / 25.4 * 600); };
        const auto value = [&](const core::InkPixels& p, double x_mm, double y_mm) { return p.value(std::size_t(px(y_mm)) * p.width + px(x_mm), 0); };
        // the rows of the channel from y0 to y1 (pixels), as stored
        const auto rows = [](const core::InkPixels& p, int y0, int y1) {
            return std::vector<std::uint16_t>(p.samples.begin() + std::ptrdiff_t(std::size_t(y0) * p.width), p.samples.begin() + std::ptrdiff_t(std::size_t(y1) * p.width));
        };
        // the eraser along y = 40 mm from x = 5 to 25 mm, 4 mm wide
        core::Document erased = applied(doc, Json{{"op", "erase_raster"}, {"page", 1}, {"layer_id", "paint0000001"}, {"channel", id}, {"width_mm", 4},
                                                  {"points", Json::array({Json::array({5, 40}), Json::array({25, 40})})}});
        core::InkPixels now = samples(erased);
        QCOMPARE(now.width, white.width);
        QCOMPARE(value(now, 15, 40), 0.0);
        QCOMPARE(value(now, 6, 40), 0.0);
        QCOMPARE(value(now, 15, 13.3), 1.0);  // (where 200-dpi pixels on the 600-dpi channel would have put it)
        QCOMPARE(value(now, 15, 44), 1.0);
        QCOMPARE(value(now, 40, 40), 1.0);
        QVERIFY(rows(now, 0, px(35)) == rows(white, 0, px(35)));
        QVERIFY(rows(now, px(45), int(now.height)) == rows(white, px(45), int(white.height)));
        QCOMPARE(erased.page(0).layers.size(), doc.page(0).layers.size());
        // delete_area within its area (40..50 × 10..20 mm)
        const Json box{{"poly", Json::array({Json::array({40, 10}), Json::array({50, 10}), Json::array({50, 20}), Json::array({40, 20})})}};
        now = samples(applied(doc, Json{{"op", "delete_area"}, {"page", 1}, {"layer_id", "paint0000001"}, {"channel", id}, {"area", box}}));
        QCOMPARE(value(now, 45, 15), 0.0);
        QCOMPARE(value(now, 41, 11), 0.0);
        QCOMPARE(value(now, 30, 15), 1.0);
        QCOMPARE(value(now, 45, 25), 1.0);
        QVERIFY(rows(now, px(25), int(now.height)) == rows(white, px(25), int(white.height)));
        // transform_area: a box filled at 10..20 × 10..20 mm moved 30 mm to the right
        const Json square{{"poly", Json::array({Json::array({10, 10}), Json::array({20, 10}), Json::array({20, 20}), Json::array({10, 20})})}};
        core::Document moved = applied(doc, Json{{"op", "delete_channel"}, {"page", 1}, {"channel", id}});
        moved = applied(moved, Json{{"op", "add_channel"}, {"page", 1}, {"kind", "spot"}, {"name", "box"}});
        const std::string boxed = moved.page(0).channels[0].id;
        moved = applied(moved, Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", "paint0000001"}, {"channel", boxed}, {"area", square},
                                    {"rgb", Json::array({255, 255, 255})}});
        moved = applied(moved, Json{{"op", "transform_area"}, {"page", 1}, {"layer_id", "paint0000001"}, {"channel", boxed}, {"area", square},
                                    {"matrix", Json::array({1, 0, 0, 1, 30, 0})}});
        now = samples(moved);
        QCOMPARE(now.width, white.width);
        QCOMPARE(value(now, 45, 15), 1.0);
        QCOMPARE(value(now, 15, 15), 0.0);
        QCOMPARE(value(now, 45, 25), 0.0);
        // a channel of fine detail (its own 600-dpi pixels: stripes a pixel wide), a filter within an area and liquify:
        // the detail where they did not reach as it was
        core::Document fine = doc;
        render::channels::Plane stripes;
        stripes.width = white.width;
        stripes.height = white.height;
        stripes.values.resize(std::size_t(stripes.width) * stripes.height);
        for (std::size_t i = 0; i < stripes.values.size(); ++i) stripes.values[i] = (i % stripes.width) % 2 == 0 ? 1.0f : 0.25f;
        fine.edit_page(0).channels[0].raster = render::channels::encode(stripes, 8);
        const core::InkPixels striped = samples(fine);
        const Json bottom{{"poly", Json::array({Json::array({0, 60}), Json::array({60, 60}), Json::array({60, 80}), Json::array({0, 80})})}};
        now = samples(applied(fine, Json{{"op", "filter_raster"}, {"page", 1}, {"id", "paint0000001"}, {"channel", id}, {"kind", "blur"}, {"radius", 2},
                                         {"area", bottom}}));
        QVERIFY(rows(now, 0, px(55)) == rows(striped, 0, px(55)));
        QVERIFY(rows(now, px(65), int(now.height)) != rows(striped, px(65), int(striped.height)));
        now = samples(applied(fine, Json{{"op", "liquify"}, {"page", 1}, {"layer_id", "paint0000001"}, {"channel", id}, {"width_mm", 8},
                                         {"points", Json::array({Json::array({30, 40}), Json::array({34, 40})})}}));
        QVERIFY(rows(now, 0, px(30)) == rows(striped, 0, px(30)));
        QVERIFY(rows(now, px(50), int(now.height)) == rows(striped, px(50), int(striped.height)));
        // an op that leaves it as it was (white filled over white): the channel's very bytes
        const core::Document same = applied(doc, Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", "paint0000001"}, {"channel", id},
                                                      {"area", left_half()}, {"rgb", Json::array({255, 255, 255})}});
        QCOMPARE(*same.page(0).channels[0].raster, *doc.page(0).channels[0].raster);
    }

    // What a lent channel gives back follows a grey page's rules: Genko's blacks (the greys 0, 10, 15 and 20, the pen's
    // and the swatches' black) are no ink and not selected, white is all; other greys their lightness.
    void genkosBlackClearsAChannel() {
        for (const int black : {20, 15, 10, 0}) {
            core::Document doc = painted(pattern());
            doc = applied(doc, Json{{"op", "add_channel"}, {"page", 1}, {"fill", "white"}});
            const std::string id = doc.page(0).channels[0].id;
            doc = applied(doc, Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", "paint0000001"}, {"channel", id}, {"area", left_half()},
                                  {"rgb", Json::array({black, black, black})}});
            const render::channels::Plane p = plane(doc.page(0), 0);
            QVERIFY2(at(p, 10, 10) == 0.0f, qPrintable(QString("%1: %2").arg(black).arg(at(p, 10, 10))));
            QCOMPARE(at(p, 400, 10), 1.0f);
        }
        core::Document doc = painted(pattern());
        doc = applied(doc, Json{{"op", "add_channel"}, {"page", 1}});
        const std::string id = doc.page(0).channels[0].id;
        doc = applied(doc, Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", "paint0000001"}, {"channel", id}, {"area", left_half()},
                              {"rgb", Json::array({128, 128, 128})}});
        QCOMPARE(at(plane(doc.page(0), 0), 10, 10), 128 / 255.0f);
        doc = applied(doc, Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", "paint0000001"}, {"channel", id}, {"area", left_half()},
                              {"rgb", Json::array({255, 255, 255})}});
        QCOMPARE(at(plane(doc.page(0), 0), 10, 10), 1.0f);
    }

    // A 16-bit channel worked by a pixel op: what it changed comes back at 8 bits — said in the op's result
    // (channel_precision: 8); a pixel it did not change keeps its 16-bit value; an 8-bit channel says nothing.
    void deepChannelSaysItsPrecision() {
        core::Document doc = applied(painted(pattern()), Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}});
        doc = applied(doc, Json{{"op", "add_channel"}, {"page", 1}});
        render::channels::Plane fine = plane(doc.page(0), 0);
        for (float& v : fine.values) v = 1000.0f / 65535.0f;
        doc.edit_page(0).channels[0].raster = render::channels::encode(fine, 16);
        const std::string id = doc.page(0).channels[0].id;
        const Json fill{{"op", "fill_area"}, {"page", 1}, {"layer_id", "paint0000001"}, {"channel", id}, {"area", left_half()},
                        {"rgb", Json::array({255, 255, 255})}};
        const Json said = report(doc, fill);
        QCOMPARE(said.value("channel_precision", Json()), Json(8));
        const core::InkPixels after = core::decode_ink_raster(*applied(doc, fill).page(0).channels[0].raster);
        QCOMPARE(after.samples[std::size_t(10) * after.width + 10], std::uint16_t(65535));
        QCOMPARE(after.samples[std::size_t(10) * after.width + 400], std::uint16_t(1000));
        core::Document eight = applied(painted(pattern()), Json{{"op", "add_channel"}, {"page", 1}});
        const Json plain = report(eight, Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", "paint0000001"}, {"channel", eight.page(0).channels[0].id},
                                              {"area", left_half()}, {"rgb", Json::array({255, 255, 255})}});
        QVERIFY(!plain.contains("channel_precision"));
    }

    // The pixel ops' "channel" keeps Python's way with an op where it names nothing this build has (a channel the page has
    // not, a number, null: Python ignores a key it does not know and says so — the op is not refused, its layer drawn on);
    // filter_raster's levels, curve and brightness_contrast read a "channel" of their own (Python's r, g, b, else all
    // three — "red" too, and a channel's name), where only an alpha or spot channel's id, or a plate of a CMYK or Lab page
    // (pages Python has not), is this build's.
    void pythonsWayWithTheChannelKey() {
        core::Document doc = painted(pattern());
        const std::string layer = "paint0000001";
        doc = applied(doc, Json{{"op", "add_channel"}, {"page", 1}, {"name", "A"}});
        const std::string id = doc.page(0).channels[0].id;
        const Json fill{{"op", "fill_area"}, {"page", 1}, {"layer_id", layer}, {"area", left_half()}, {"rgb", Json::array({0, 0, 0})}};
        const std::string drawn = render::render_page(run_ops(doc, Json::array({fill})).doc.page(0), 100, render::proof_options(), &doc).image.tobytes();
        for (const Json& value : {Json("Nope"), Json(5), Json(nullptr), Json("ch_000000000000")}) {
            Json op = fill;
            op["channel"] = value;
            const core::ApplyResult made = run_ops(doc, Json::array({op}));
            QCOMPARE(render::render_page(made.doc.page(0), 100, render::proof_options(), &made.doc).image.tobytes(), drawn);
            QVERIFY2(made.warnings.dump().find("ops[0] fill_area: unknown keys ignored: channel") != std::string::npos, made.warnings.dump().c_str());
            QVERIFY(!made.doc.page(0).channels[0].raster);
        }
        Json named = fill;
        named["channel"] = "A";  // (its name: this build's, no warning)
        QVERIFY(run_ops(doc, Json::array({named})).warnings.empty());
        // brightness_contrast: "red" and a channel's name all three colours, as Python; "r" the red alone; the id the channel
        const Json bc{{"op", "filter_raster"}, {"page", 1}, {"id", layer}, {"kind", "brightness_contrast"}, {"brightness", 40}, {"contrast", 0}};
        const std::string all = pixels(layer_of(applied(doc, bc).page(0), layer));
        const std::string before = pixels(layer_of(doc.page(0), layer));
        QVERIFY(all != before);
        for (const char* value : {"red", "A", "composite", "rgb"}) {
            Json op = bc;
            op["channel"] = value;
            QVERIFY2(pixels(layer_of(applied(doc, op).page(0), layer)) == all, value);
        }
        Json red = bc;
        red["channel"] = "r";
        const std::string reds = pixels(layer_of(applied(doc, red).page(0), layer));
        for (std::size_t i = 0; i < reds.size(); i += 4 * 101) {
            QCOMPARE(reds[i + 1], before[i + 1]);
            QCOMPARE(reds[i + 2], before[i + 2]);
        }
        Json on_channel = bc;
        on_channel["channel"] = id;
        const core::Document lit = applied(doc, on_channel);
        QCOMPARE(pixels(layer_of(lit.page(0), layer)), before);
        QVERIFY(lit.page(0).channels[0].raster);
        // levels on a CMYK page's cyan plate: the cyan alone (the other inks and the alpha as they were)
        const core::Document cmyk = applied(painted(pattern()), Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}});
        const core::InkPixels was = core::decode_ink_raster(*layer_of(cmyk.page(0), layer).ink_raster);
        const core::Document levelled = applied(cmyk, Json{{"op", "filter_raster"}, {"page", 1}, {"id", layer}, {"kind", "levels"}, {"black", 0},
                                                         {"white", 128}, {"channel", "cyan"}});
        const core::InkPixels now = core::decode_ink_raster(*layer_of(levelled.page(0), layer).ink_raster);
        bool cyan = false;
        for (std::size_t i = 0; i < now.pixel_count(); i += 37) {
            cyan = cyan || now.samples[i * 5] != was.samples[i * 5];
            for (unsigned c = 1; c < 5; ++c) QCOMPARE(now.samples[i * 5 + c], was.samples[i * 5 + c]);
        }
        QVERIFY(cyan);
    }

    // A studio book's rules (strict_gates) look at what a pixel op changes: before the name is approved a printed layer is
    // refused, its alpha channel is not (never printed), a spot channel is (printed), a plate of the layer is (the layer).
    void strictGatesLookAtTheChannel() {
        core::Document doc = painted(pattern());
        doc = run_ops(doc, Json::array({Json{{"op", "add_channel"}, {"page", 1}, {"name", "A"}},
                                        Json{{"op", "add_channel"}, {"page", 1}, {"kind", "spot"}, {"name", "S"}, {"fill", "white"}}})).doc;
        doc.strict_gates = true;
        const Json fill{{"op", "fill_area"}, {"page", 1}, {"layer_id", "paint0000001"}, {"area", left_half()}, {"rgb", Json::array({255, 255, 255})}};
        QVERIFY(refusal(doc, fill).find("needs name_ok") != std::string::npos);
        Json on_alpha = fill;
        on_alpha["channel"] = "A";
        QCOMPARE(refusal(doc, on_alpha), std::string());
        Json on_spot = fill;
        on_spot["channel"] = "S";
        QVERIFY(refusal(doc, on_spot).find("on a spot channel (printed) needs name_ok") != std::string::npos);
        Json on_plate = fill;
        on_plate["channel"] = "red";
        QVERIFY(refusal(doc, on_plate).find("on a printed layer needs name_ok") != std::string::npos);
    }

    // The page locks and for_pages: duplicate_channel onto another page asks that page's lock too; the ops that make or
    // take pages (split_channels, merge_channels, duplicate_channel to a new page) are not repeated page by page.
    void locksAndForPages() {
        core::Document doc = painted(pattern(), 2);
        doc = applied(doc, Json{{"op", "add_channel"}, {"page", 1}, {"name", "W"}, {"fill", "white"}});
        core::Document locked = doc;
        locked.page_locks[locked.page(1).id] = "ai:other";
        QVERIFY(refusal(locked, Json{{"op", "duplicate_channel"}, {"page", 1}, {"channel", "W"}, {"to_page", 2}}).find("page 2 locked by ai:other") !=
                std::string::npos);
        QCOMPARE(refusal(locked, Json{{"op", "duplicate_channel"}, {"page", 1}, {"channel", "W"}}), std::string());
        for (const Json& op : {Json{{"op", "split_channels"}}, Json{{"op", "merge_channels"}, {"pages", Json::array({1, 2, 3})}},
                               Json{{"op", "duplicate_channel"}, {"channel", "W"}, {"to_page", "new"}}}) {
            const std::string refused = refusal(doc, Json{{"op", "for_pages"}, {"pages", "all"}, {"ops", Json::array({op})}});
            QVERIFY2(refused.find("cannot be repeated page by page") != std::string::npos, refused.c_str());
        }
    }

    // A channel whose raster cannot be read (its asset broken on disk): the book opens read-only with the problem
    // reported; inspect lists the channel with its problem (the rest of the book as usual), and the page is drawn — the
    // broken channel as none — rather than the whole failing.
    void brokenChannelAssetIsReported() {
        QTemporaryDir dir;
        core::Document doc = painted(solid(200, 150, 100));
        doc = applied(doc, Json{{"op", "add_channel"}, {"page", 1}, {"kind", "spot"}, {"name", "S"}, {"fill", "white"}});
        doc = applied(doc, Json{{"op", "add_channel"}, {"page", 1}, {"name", "A"}, {"fill", "white"}});
        const fs::path book = fs::path(dir.path().toStdString()) / "b.genko";
        write_book(doc, book);
        const Json raw = Json::parse(genko::test::read_bytes(QString::fromStdString((book / "project.json").string())));
        const std::string ref = raw["pages"][0]["channels"][0]["raster"].get<std::string>();
        genko::test::write_bytes(QString::fromStdString((book / "assets" / ref.substr(7, 2) / (ref.substr(7) + ".gkir")).string()), "GKIR broken");
        const storage::LoadResult loaded = storage::load_document(book);
        QVERIFY(!loaded.report.clean());
        QVERIFY(!loaded.document.read_only_reason.empty());
        const Json snapshot = storage::snapshot(loaded.document);
        const Json listed = snapshot["pages"][0]["channels"];
        QCOMPARE(listed.size(), std::size_t{2});
        QVERIFY2(listed[0].contains("problem") && listed[0]["size"].is_null(), listed.dump().c_str());
        QVERIFY(!listed[1].contains("problem"));
        QCOMPARE(listed[1]["size"], Json::array({kW, kH}));
        render::RenderOptions options;
        options.overlays = {loaded.document.page(0).channels[1].id};
        const render::Image shown = render::render_page(loaded.document.page(0), 100, options, &loaded.document).image;
        QCOMPARE(shown.getpixel(20, 100), (std::vector<double>{200, 150, 100}));  // (the broken spot as none; the alpha all selected)
    }

    // An 8-bit RGB page of a profile (Display P3) split into its plates: the page's values (as a 16- or 32-bit page's),
    // not the sRGB the screen shows them in.
    void splitAProfiledPage() {
        const render::Image image = pattern();
        core::Document doc = applied(painted(image), Json{{"op", "assign_profile"}, {"page", 1}, {"profile", "display-p3"}});
        QVERIFY(doc.page(0).rgb && doc.page(0).rgb->depth == 8 && !doc.page(0).rgb->profile.empty());
        doc = applied(doc, Json{{"op", "split_channels"}, {"page", 1}});
        const std::string original = image.tobytes();
        int worst = 0;
        for (std::size_t k = 0; k < 3; ++k) {
            const core::InkPixels grey = core::decode_ink_raster(*doc.page(k).layers[1].ink_raster);
            for (std::size_t i = 0; i < grey.pixel_count(); i += 101) {
                worst = std::max(worst, std::abs(int(grey.samples[i * 2]) - int(static_cast<unsigned char>(original[i * 4 + k]))));
            }
        }
        QVERIFY2(worst <= 1, qPrintable(QString::number(worst)));
    }

    // The spot as the screen prints it is what Merge Spot Channel makes of the page (an RGB page: the same formula,
    // c + (s·ink + (1 − s)·c·ink − c)·v), at solidity 0, 0.5 and 1 and a half density: black ink of solidity 1 at half
    // over white is a 50 % grey on screen too, not 25 %.
    void spotOnScreenAsMerged() {
        for (const double s : {0.0, 0.5, 1.0}) {
            for (const auto& [paper, ink] : std::vector<std::pair<std::array<int, 3>, std::array<int, 3>>>{{{200, 150, 100}, {30, 90, 250}},
                                                                                                            {{255, 255, 255}, {0, 0, 0}}}) {
                core::Document doc = painted(solid(paper[0], paper[1], paper[2]));
                doc = applied(doc, Json{{"op", "add_channel"}, {"page", 1}, {"kind", "spot"}, {"color", Json::array({ink[0], ink[1], ink[2]})},
                                      {"solidity", s}, {"fill", "white"}});
                render::channels::Plane half = plane(doc.page(0), 0);
                std::fill(half.values.begin(), half.values.end(), 0.5f);
                doc.edit_page(0).channels[0].raster = render::channels::encode(half, 8);
                const std::vector<double> shown = render::render_page(doc.page(0), 100, render::RenderOptions{}, &doc).image.getpixel(200, 100);
                const core::Document merged = applied(doc, Json{{"op", "merge_spot_channel"}, {"page", 1}});
                const std::vector<double> made = render::render_page(merged.page(0), 100, render::RenderOptions{}, &merged).image.getpixel(200, 100);
                for (unsigned k = 0; k < 3; ++k) {
                    const double p = paper[k] / 255.0, c = ink[k] / 255.0, v = 128 / 255.0;
                    const double want = p + (s * c + (1 - s) * p * c - p) * v;
                    QVERIFY2(std::abs(shown[k] / 255.0 - want) <= 1.0 / 255.0 + 1e-9 && std::abs(shown[k] - made[k]) <= 1.0,
                             qPrintable(QString("s %1 k %2: shown %3 merged %4 want %5").arg(s).arg(k).arg(shown[k]).arg(made[k]).arg(want * 255)));
                }
            }
        }
    }

    // GKIR read a row at a time (core::read_ink_raster_rows, for the large channels): the values decode_ink_raster gives,
    // at 8, 16 and 32 bits; bytes cut short or following the pixels refused as decode_ink_raster refuses them.
    void rowsReadOneAtATime() {
        for (const int depth : {8, 16, 32}) {
            core::InkPixels pixels = core::blank_ink_pixels(core::InkSpace::Mask, depth, 37, 23);
            for (std::size_t i = 0; i < pixels.pixel_count(); ++i) pixels.set(i, 0, ((i * 7919) % 1000) / 999.0);
            const std::string bytes = core::encode_ink_raster(pixels);
            const core::InkPixels whole = core::decode_ink_raster(bytes);
            std::uint32_t rows = 0;
            core::read_ink_raster_rows(bytes, [&](std::uint32_t y, const float* values) {
                QCOMPARE(y, rows++);
                for (std::uint32_t x = 0; x < whole.width; ++x) QCOMPARE(values[x], static_cast<float>(whole.value(std::size_t(y) * whole.width + x, 0)));
            });
            QCOMPARE(rows, whole.height);
            const auto refused = [](const std::string& broken) {
                try {
                    core::read_ink_raster_rows(broken, [](std::uint32_t, const float*) {});
                } catch (const core::Error& error) {
                    return error.code() == "format";
                }
                return false;
            };
            QVERIFY(refused(bytes.substr(0, bytes.size() - 6)));
            QVERIFY(refused(bytes + "x"));
        }
    }

    // The panel's small picture of a channel: each pixel the mean of the channel's pixels it covers, read row by row
    // (not decoded whole); none for a channel without pixels or one that cannot be read.
    void channelThumbnail() {
        core::Document doc = applied(painted(pattern()), Json{{"op", "add_channel"}, {"page", 1}, {"fill", "area"}, {"area", left_half()}});
        const core::Channel& channel = doc.page(0).channels[0];
        const render::Image small = render::channels::thumbnail(channel, render::Size{27, 36});
        QCOMPARE(small.mode(), std::string_view("L"));
        QCOMPARE(small.getpixel(3, 20)[0], 255.0);
        QCOMPARE(small.getpixel(24, 20)[0], 0.0);
        const double edge = small.getpixel(13, 20)[0];  // (the column over the half's edge: part of it)
        QVERIFY2(edge > 0 && edge < 255, qPrintable(QString::number(edge)));
        core::Channel none = channel;
        none.raster = nullptr;
        QCOMPARE(render::channels::thumbnail(none, render::Size{27, 36}).getbbox(), std::optional<render::Box>());
        none.raster = std::make_shared<const std::string>("GKIR broken");
        QCOMPARE(render::channels::thumbnail(none, render::Size{27, 36}).getbbox(), std::optional<render::Box>());
    }
};

QTEST_GUILESS_MAIN(TestChannels)
#include "test_channels.moc"
