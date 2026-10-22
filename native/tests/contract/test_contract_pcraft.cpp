// PC4 F-f (SPEC.md FILE-01, .pcraft; design section F 2.8) against PhotoCraft v0.2.0's own reader and writer: the .pcraft
// files upstream's writer made (native/tests/data/pcraft, tools/migration/pcraft_fixtures; RGB of 8, 16 and 32 bits, grey of
// 8 and 16, CMYK, Lab, indexed, every layer kind, the 16 adjustments, masks, vector masks, channels, paths, guides, smart
// objects) opened as Genko's books — their layers in upstream's order, their pixels, masks and channels at the samples
// upstream's reader reads (MANIFEST.json's "upstream_read", what `pcraft-fixtures check` said), what Genko has no place for
// said in the notes —; a Genko book written as a .pcraft and read back (its layers, pixels, channels and guides; what a
// .pcraft cannot hold said), every upstream file read, written by this build and read again the same; `genko export
// --format pcraft` and `genko new --from x.pcraft`; 埋め込みを配置 of a .pcraft as a smart object whose contents are its
// page. With $GENKO_PCRAFT_CHECK (the tool built), the files this build writes are read by upstream's reader too.

#include <QtTest>

#include <QCryptographicHash>
#include <QProcess>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/base64.hpp"
#include "core/channels.hpp"
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/ink.hpp"
#include "core/json.hpp"
#include "core/paths.hpp"
#include "core/smart.hpp"
#include "formats/open_file.hpp"
#include "formats/page_picture.hpp"
#include "formats/pcraft_write.hpp"
#include "render/image.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/pcraft_read.hpp"
#include "render/png.hpp"
#include "render/raster.hpp"
#include "render/zip.hpp"
#include "storage/asset_store.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/transaction.hpp"
#include "storage/writer.hpp"
#include "testsupport.hpp"

namespace core = genko::core;
namespace render = genko::render;
namespace formats = genko::formats;
namespace storage = genko::storage;
namespace pc = genko::render::pcraft;
namespace fs = std::filesystem;
using core::Json;

namespace {

const char* const kFixtures[] = {"rich_rgb8.pcraft", "adjustments.pcraft", "rgb16.pcraft", "rgb32.pcraft", "gray8.pcraft",
                                 "gray16.pcraft",    "cmyk8.pcraft",       "lab8.pcraft",  "indexed.pcraft"};

std::string fixture(const std::string& name) { return genko::test::read_bytes(genko::test::test_data(QStringLiteral("pcraft/") + QString::fromStdString(name))); }

const Json& manifest() {
    static const Json m = genko::test::read_json(genko::test::test_data(QStringLiteral("pcraft/MANIFEST.json")));
    return m;
}

const Json& upstream_read(const std::string& name) { return manifest()["files"][name]["upstream_read"]; }

core::Document run(const core::Document& doc, const Json& ops) { return core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor("human:test")).doc; }

// A layer's own pixels, normalised: its colours as its page holds them and its alpha (n a pixel).
struct Pixels {
    int w = 0, h = 0;
    unsigned n = 0;
    std::vector<double> v;
    double at(int x, int y, unsigned c) const { return v[(std::size_t(y) * w + x) * n + c]; }
};

std::optional<Pixels> pixels_of(const core::Layer& layer) {
    Pixels p;
    if (layer.raster_png) {
        const render::Image image = render::open_image(*layer.raster_png).convert("RGBA");
        const std::string raw = image.tobytes();
        p.w = image.width();
        p.h = image.height();
        p.n = 4;
        p.v.resize(raw.size());
        for (std::size_t i = 0; i < raw.size(); ++i) p.v[i] = static_cast<unsigned char>(raw[i]) / 255.0;
        return p;
    }
    if (layer.color_raster) {
        const core::ColorRasterView view(*layer.color_raster);
        p.w = static_cast<int>(view.width());
        p.h = static_cast<int>(view.height());
        p.n = 4;
        p.v.reserve(std::size_t(p.w) * p.h * 4);
        for (std::size_t i = 0; i < std::size_t(p.w) * p.h; ++i) {
            const auto px = view.pixel(i);
            p.v.insert(p.v.end(), px.begin(), px.end());
        }
        return p;
    }
    if (layer.ink_raster) {
        const core::InkPixels inks = core::decode_ink_raster(*layer.ink_raster);
        p.w = static_cast<int>(inks.width);
        p.h = static_cast<int>(inks.height);
        p.n = inks.channels();
        p.v.reserve(inks.pixel_count() * p.n);
        for (std::size_t i = 0; i < inks.pixel_count(); ++i) {
            for (unsigned c = 0; c < p.n; ++c) p.v.push_back(inks.value(i, c));
        }
        return p;
    }
    return std::nullopt;
}

// A grey PNG's (a mask's) value at (x, y), 0..1.
double grey_at(const std::string& png, int x, int y) {
    const render::Image image = render::open_image(png).convert("L");
    return static_cast<unsigned char>(image.tobytes()[std::size_t(y) * image.width() + x]) / 255.0;
}

// How near a sample of this depth is read again (8 and 16 bits: half a step; floats: as they are).
double step_of(const std::string& depth) { return depth == "U8" ? 0.5 / 255 + 1e-9 : depth == "U16" ? 0.5 / 65535 + 1e-9 : 1e-6; }

// Upstream's layers as Genko orders them: a group's children first, then the group (as a folder); each with whether its
// groups show.
void flattened(const Json& layers, bool shown, std::vector<std::pair<const Json*, bool>>& out) {
    for (const Json& l : layers) {
        const bool visible = shown && l["visible"].get<bool>();
        if (l["kind"] == Json("group")) flattened(l["children"], visible, out);
        out.emplace_back(&l, visible);
    }
}

bool said(const std::vector<std::string>& notes, const std::string& part) {
    return std::any_of(notes.begin(), notes.end(), [&](const std::string& n) { return n.find(part) != std::string::npos; });
}

std::string joined(const std::vector<std::string>& notes) {
    std::string out;
    for (const std::string& n : notes) out += n + "\n";
    return out;
}

const core::Layer* titled(const core::Page& page, const std::string& title) {
    for (const core::Layer& l : page.layers) {
        if (l.title == title) return &l;
    }
    return nullptr;
}

core::Document small_book() {
    core::PageSpec spec;
    spec.width_mm = core::Num(60);
    spec.height_mm = core::Num(80);
    spec.dpi = core::Num(150);
    spec.bleed_mm = core::Num(3);
    spec.inner_margin_mm = core::Num(5);
    return core::new_episode("pcraft", core::Num(1), 1, spec, core::Binding::Right);
}

void write_book(const core::Document& doc, const fs::path& dir) {
    storage::ProjectLock lock(dir, "human:test");
    lock.try_acquire();
    storage::SaveRequest request;
    request.actor = "human:test";
    request.ops = Json::array();
    storage::Saver(lock).save(doc, request);
}

// The layers' pixels of two pages the same (those both have), compared at their page's resolution.
void same_layers(const core::Document& a, const core::Document& b, const std::string& what) {
    const core::Page& pa = a.page(0);
    const core::Page& pb = b.page(0);
    std::vector<std::string> ta, tb;
    for (const core::Layer& l : pa.layers) ta.push_back(l.title);
    for (const core::Layer& l : pb.layers) tb.push_back(l.title);
    QVERIFY2(ta == tb, qPrintable(QString::fromStdString(what + ": " + core::Json(ta).dump() + " / " + core::Json(tb).dump())));
    for (std::size_t i = 0; i < pa.layers.size(); ++i) {
        const core::Layer& la = pa.layers[i];
        const core::Layer& lb = pb.layers[i];
        const std::string where = what + " " + la.title;
        QVERIFY2(la.kind == lb.kind, where.c_str());
        QVERIFY2(la.visible == lb.visible, where.c_str());
        QVERIFY2(std::abs(la.opacity - lb.opacity) < 1e-9, where.c_str());
        QVERIFY2(la.blend == lb.blend, where.c_str());
        QVERIFY2(la.clip == lb.clip, where.c_str());
        QVERIFY2(la.mask.has_value() == lb.mask.has_value(), where.c_str());
        QVERIFY2(la.vector_mask.has_value() == lb.vector_mask.has_value(), where.c_str());
        const auto xa = pixels_of(la), xb = pixels_of(lb);
        QVERIFY2(xa.has_value() == xb.has_value(), where.c_str());
        if (xa) {
            QVERIFY2(xa->w == xb->w && xa->h == xb->h && xa->n == xb->n, where.c_str());
            for (std::size_t k = 0; k < xa->v.size(); k += xa->n) {
                if (xa->v[k + xa->n - 1] == 0.0 && xb->v[k + xb->n - 1] == 0.0) continue;  // (a clear pixel's colour)
                for (unsigned c = 0; c < xa->n; ++c) {
                    if (std::abs(xa->v[k + c] - xb->v[k + c]) > 1e-6) QFAIL(qPrintable(QString::fromStdString(where + " pixel " + std::to_string(k / xa->n))));
                }
            }
        }
        if (la.mask && lb.mask) QVERIFY2(render::open_image(*la.mask->png).tobytes() == render::open_image(*lb.mask->png).tobytes(), where.c_str());
        if (la.adjust) QVERIFY2(lb.adjust && (*la.adjust)["kind"] == (*lb.adjust)["kind"], where.c_str());
        if (la.fill) QVERIFY2(lb.fill.has_value(), where.c_str());
    }
    QCOMPARE(pb.channels.size(), pa.channels.size());
    for (std::size_t i = 0; i < pa.channels.size(); ++i) {
        QCOMPARE(pb.channels[i].name, pa.channels[i].name);
        QCOMPARE(pb.channels[i].kind, pa.channels[i].kind);
        QCOMPARE(pb.channels[i].raster != nullptr, pa.channels[i].raster != nullptr);
        if (pa.channels[i].raster && pb.channels[i].raster) {
            const core::InkPixels ca = core::decode_ink_raster(*pa.channels[i].raster), cb = core::decode_ink_raster(*pb.channels[i].raster);
            QVERIFY2(ca.samples == cb.samples && ca.floats == cb.floats, (what + " channel " + pa.channels[i].name).c_str());
        }
    }
    QCOMPARE(pb.paths.size(), pa.paths.size());
    std::size_t guides_a = 0, guides_b = 0;
    for (const Json& r : pa.rulers) guides_a += r.value("kind", std::string()) == "guide";
    for (const Json& r : pb.rulers) guides_b += r.value("kind", std::string()) == "guide";
    QCOMPARE(guides_b, guides_a);
}

}  // namespace

class TestContractPcraft : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;
    fs::path at(const std::string& name) const { return fs::path(tmp_.path().toStdString()) / name; }

private slots:
    // Each fixture as upstream's writer made it (MANIFEST.json: its bytes, and what upstream's reader read of it).
    void theFixturesAreUpstreams() {
        QCOMPARE(manifest()["upstream"]["commit"].get<std::string>(), std::string("ad863217386440ca968fccc9bfff65ba24e61142"));
        for (const char* name : kFixtures) {
            const std::string bytes = fixture(name);
            QVERIFY2(!bytes.empty(), name);
            QCOMPARE(QString::fromStdString(manifest()["files"][name]["sha256"].get<std::string>()),
                     QString::fromLatin1(QCryptographicHash::hash(QByteArray::fromStdString(bytes), QCryptographicHash::Sha256).toHex()));
            QVERIFY2(pc::is_pcraft(bytes), name);
            QVERIFY2(upstream_read(name)["ok"].get<bool>(), name);
            QCOMPARE(pc::read_manifest(bytes)["format_version"].get<int>(), 1);
        }
    }

    // Opened, each is a book of one page in memory: its paper the canvas at its resolution, its colour model the
    // document's, its layers upstream's in Genko's order with their settings, pixels, masks and channels as upstream reads them.
    void upstreamFilesOpenAsTheirLayers() {
        for (const char* name : kFixtures) {
            const Json up = upstream_read(name);
            const formats::Opened opened = formats::open_file(fixture(name), name);
            const core::Document& doc = opened.doc;
            const std::string what(name);
            QVERIFY(!doc.asset_dir);
            QCOMPARE(doc.pages.size(), std::size_t(1));
            const core::Page& page = doc.page(0);
            const int dpi = static_cast<int>(page.spec.dpi.value());
            QCOMPARE(dpi, std::max(72, static_cast<int>(std::lround(up["dpi"].get<double>()))));
            QCOMPARE(render::mm_to_px(page.spec.width_mm.value(), dpi), up["width"].get<int>());
            QCOMPARE(render::mm_to_px(page.spec.height_mm.value(), dpi), up["height"].get<int>());
            QVERIFY(page.frames.empty());
            QVERIFY(page.native_source);
            QCOMPARE(page.native_source->format, std::string("pcraft"));
            QCOMPARE(page.native_source->width_px, up["width"].get<std::int64_t>());
            // its colour model
            const std::string mode = up["mode"].get<std::string>(), depth = up["depth"].get<std::string>();
            if (mode == "Rgb" || mode == "Indexed") {
                QVERIFY2(!page.color, name);
                if (depth == "U8" && mode == "Rgb") QVERIFY2(!page.rgb || page.rgb->depth == 8, name);
                if (depth == "U16") QVERIFY2(page.rgb && page.rgb->depth == 16, name);
                if (depth == "F32") QVERIFY2(page.rgb && page.rgb->depth == 32, name);
                if (depth == "U8") QCOMPARE(page.spec.grid, std::optional<std::string>("finest"));
            } else {
                QVERIFY2(page.color, name);
                const core::InkSpace space = mode == "Grayscale" ? core::InkSpace::Gray : mode == "Cmyk" ? core::InkSpace::Cmyk : core::InkSpace::Lab;
                QVERIFY2(page.color->space == space, name);
                QCOMPARE(page.color->depth, depth == "U8" ? 8 : 16);
            }
            if (mode == "Indexed") {
                QVERIFY2(page.rgb && page.rgb->table, name);
                std::vector<std::array<int, 3>> colours;
                for (const Json& c : up["color_table"]) colours.push_back({c[0].get<int>(), c[1].get<int>(), c[2].get<int>()});
                QVERIFY2(page.rgb->table->colors == colours, name);
            }
            // its layers, in Genko's order
            std::vector<std::pair<const Json*, bool>> expected;
            flattened(up["layers"], true, expected);
            std::vector<std::pair<const Json*, bool>> kept;
            for (const auto& e : expected) {
                if (what == "adjustments.pcraft" && (*e.first)["name"] == Json("Unknown")) continue;  // (an adjustment Genko has not: said)
                kept.push_back(e);
            }
            QVERIFY2(page.layers.size() == kept.size(), qPrintable(QString::fromStdString(what + ": " + std::to_string(page.layers.size()) + " layers\n" + joined(opened.notes))));
            const double step = step_of(depth);
            for (std::size_t i = 0; i < kept.size(); ++i) {
                const Json& u = *kept[i].first;
                const core::Layer& l = page.layers[i];
                const std::string where = what + " " + u["name"].get<std::string>();
                QCOMPARE(l.title, u["name"].get<std::string>());
                QVERIFY2(l.visible == kept[i].second, where.c_str());
                const std::string kind = u["kind"].get<std::string>();
                if (kind == "group") {
                    QVERIFY2(l.kind == core::LayerKind::Folder, where.c_str());
                    QCOMPARE(l.opacity, 1.0);
                    continue;
                }
                QVERIFY2(l.clip == u["clipped"].get<bool>(), where.c_str());
                QVERIFY2(l.mask.has_value() == u["mask"].get<bool>(), where.c_str());
                QVERIFY2(l.vector_mask.has_value() == u["vector_mask"].get<bool>(), where.c_str());
                const double opacity = std::round(u["opacity"].get<double>() * std::min(1.0, u["fill_opacity"].get<double>()) * 1000.0) / 1000.0;
                QVERIFY2(std::abs(l.opacity - opacity) < 1e-6, qPrintable(QString::fromStdString(where + ": " + std::to_string(l.opacity))));
                const std::string blend = u["blend"].get<std::string>();
                if (kind != "adjustment") {
                    QVERIFY2(l.blend == (blend == "Multiply" ? "multiply" : blend == "Screen" ? "screen" : "normal"), where.c_str());
                }
                if (kind == "raster") QVERIFY2(l.kind == core::LayerKind::Raster, where.c_str());
                if (kind == "adjustment") QVERIFY2(l.kind == core::LayerKind::Adjust && l.adjust, where.c_str());
                if (kind == "fill") QVERIFY2(l.kind == core::LayerKind::Fill && l.fill, where.c_str());
                if (kind == "text") QVERIFY2(l.kind == core::LayerKind::Raster, where.c_str());  // (its picture)
                if (kind == "shape") QVERIFY2(l.kind == core::LayerKind::Strokes || l.kind == core::LayerKind::Raster, where.c_str());
                if (kind == "smart") QVERIFY2(l.kind == (u["source"].contains("linked") ? core::LayerKind::Raster : core::LayerKind::Smart), where.c_str());
                // its pixels where upstream read them (a layer Genko holds as pixels)
                if (const auto px = pixels_of(l); px && u.contains("pixels")) {
                    QCOMPARE(px->w, up["width"].get<int>());
                    QCOMPARE(px->h, up["height"].get<int>());
                    for (const Json& probe : u["pixels"]) {
                        const int x = probe[0].get<int>(), y = probe[1].get<int>();
                        const Json& values = probe[2];
                        QVERIFY2(values.size() == px->n, qPrintable(QString::fromStdString(where + ": " + values.dump())));
                        const double alpha = values.back().get<double>();
                        QVERIFY2(std::abs(px->at(x, y, px->n - 1) - alpha) <= step, qPrintable(QString::fromStdString(where + " alpha at " + probe.dump())));
                        if (alpha == 0.0) continue;
                        for (unsigned c = 0; c + 1 < px->n; ++c) {
                            const double want = values[c].get<double>(), got = px->at(x, y, c);
                            QVERIFY2(std::abs(got - want) <= step, qPrintable(QString::fromStdString(where + " at " + probe.dump() + ": " + std::to_string(got))));
                        }
                    }
                }
                if (l.mask && u.contains("mask_pixels")) {
                    for (const Json& probe : u["mask_pixels"]) {
                        const double want = probe[2][0].get<double>(), got = grey_at(*l.mask->png, probe[0].get<int>(), probe[1].get<int>());
                        QVERIFY2(std::abs(got - want) <= 0.5 / 255 + 1e-9, qPrintable(QString::fromStdString(where + " mask at " + probe.dump())));
                    }
                }
            }
            // its channels (and the Quick Mask)
            for (const Json& c : up["channels"]) {
                const core::Channel* ch = core::find_channel(page, c["name"].get<std::string>());
                QVERIFY2(ch != nullptr, qPrintable(QString::fromStdString(what + " channel " + c["name"].get<std::string>())));
                QVERIFY2((ch->kind == core::ChannelKind::Spot) == c["spot"].get<bool>(), name);
                const core::InkPixels mask = ch->raster ? core::decode_ink_raster(*ch->raster) : core::InkPixels{};
                for (const Json& probe : c["pixels"]) {
                    const double want = probe[2][0].get<double>();
                    const double got = ch->raster ? mask.value(std::size_t(probe[1].get<int>()) * mask.width + probe[0].get<int>(), 0) : 0.0;
                    QVERIFY2(std::abs(got - want) <= 0.5 / 255 + 1e-9, qPrintable(QString::fromStdString(what + " channel at " + probe.dump())));
                }
            }
            // its paths and guides
            QCOMPARE(page.paths.size(), up["paths"].size());
            QCOMPARE(page.work_path.has_value(), up["work_path"].get<bool>());
            QCOMPARE(page.clipping_path.has_value(), !up["clipping_path"].is_null());
            std::size_t guides = 0;
            for (const Json& r : page.rulers) guides += r.value("kind", std::string()) == "guide";
            QCOMPARE(guides, up["guides"][0].get<std::size_t>() + up["guides"][1].get<std::size_t>());
            // (its notes: what it could not keep; a file of nothing Genko lacks says nothing)
            if (what != "rich_rgb8.pcraft" && what != "adjustments.pcraft") QVERIFY2(opened.notes.empty(), qPrintable(QString::fromStdString(what + ": " + joined(opened.notes))));
        }
    }

    // The file of every kind: each layer's settings Genko holds, what it carried, and what Genko has no place for said.
    void theRichFileKeepsWhatGenkoHolds() {
        const formats::Opened opened = formats::open_file(fixture("rich_rgb8.pcraft"), "rich_rgb8.pcraft");
        const core::Document& doc = opened.doc;
        const core::Page& page = doc.page(0);
        const double mm = 25.4 / 300.0;
        const core::Layer* paint = titled(page, "Paint");
        QVERIFY(paint);
        QCOMPARE(paint->blend, std::string("multiply"));
        QCOMPARE(paint->opacity, 0.75);
        QVERIFY(paint->locked && !paint->lock_alpha);
        QVERIFY(paint->mask && !paint->mask->linked);
        QCOMPARE(paint->mask->density, 0.8);
        QVERIFY(std::abs(paint->mask->feather_mm - 3.5 * mm) < 1e-3);
        QVERIFY(paint->vector_mask);
        QVERIFY(paint->native_psd && paint->native_psd->id && *paint->native_psd->id == 42);
        QCOMPARE(paint->native_psd->blocks.size(), std::size_t(1));
        QCOMPARE(paint->native_psd->blocks[0].first, std::string("lnsr"));
        const core::Layer* clipped = titled(page, "Clipped");
        QVERIFY(clipped && clipped->clip && clipped->link && clipped->link == paint->link);
        // the groups: folders after their layers
        const core::Layer* group = titled(page, "Group");
        const core::Layer* inner = titled(page, "Inner");
        QVERIFY(group && group->kind == core::LayerKind::Folder && inner);
        QCOMPARE(inner->parent_id.get<std::string>() != group->id, true);  // (in "Nested", in "Group")
        const core::Layer* hue = titled(page, "Hue/Sat");
        QVERIFY(hue && hue->kind == core::LayerKind::Adjust && !hue->visible);
        QCOMPARE((*hue->adjust)["kind"].get<std::string>(), std::string("hue_saturation"));
        QCOMPARE(hue->parent_id, Json(group->id));
        // the fills: a colour, a gradient and a pattern of the book's
        const core::Layer* solid = titled(page, "Solid");
        QVERIFY(solid && solid->fill && solid->mask);
        QCOMPARE((*solid->fill)["rgb"], Json::array({26, 51, 77}));
        QCOMPARE(solid->opacity, 0.6);
        const core::Layer* gradient = titled(page, "Gradient");
        QVERIFY(gradient && gradient->fill && gradient->fill->contains("gradient") && !gradient->visible);
        const core::Layer* pattern = titled(page, "Pattern");
        QVERIFY(pattern && pattern->fill && pattern->fill->contains("pattern"));
        QCOMPARE(doc.patterns.size(), std::size_t(1));
        QCOMPARE(doc.patterns[0].name, std::string("Checks"));
        QCOMPARE((*pattern->fill)["pattern"]["id"].get<std::string>(), doc.patterns[0].id);
        // the text as its pixels, the shape as a pen part
        const core::Layer* type = titled(page, "Type");
        QVERIFY(type && type->raster_png);
        const core::Layer* shape = titled(page, "Shape");
        QVERIFY(shape && shape->kind == core::LayerKind::Strokes && shape->shapes->items.size() == 1);
        // the smart objects: an embedded picture with its filter, a .pcraft's page, a linked one as its pixels
        const core::Layer* smart = titled(page, "Smart");
        QVERIFY(smart && smart->smart);
        const std::string contents = (*smart->smart)["contents"].get<std::string>();
        QCOMPARE(doc.smart_contents.at(contents)->kind, std::string("file"));
        QCOMPARE(doc.smart_contents.at(contents)->format, std::string("png"));
        QVERIFY((*smart->smart).contains("filters") && (*smart->smart)["filters"].size() == 1);
        const core::Layer* nested = nullptr;
        for (const core::Layer& l : page.layers) {
            if (l.title == "Nested" && l.kind == core::LayerKind::Smart) nested = &l;
        }
        QVERIFY(nested);
        const core::SmartContents& page_contents = *doc.smart_contents.at((*nested->smart)["contents"].get<std::string>());
        QCOMPARE(page_contents.kind, std::string("page"));
        QCOMPARE(render::mm_to_px(page_contents.page->spec.width_mm.value(), 300), 40);
        QCOMPARE(page_contents.page->layers.size(), std::size_t(2));  // (its background and "Dot")
        const core::Layer* linked = titled(page, "Linked");
        QVERIFY(linked && linked->raster_png && !linked->smart);
        const core::Layer* effects = titled(page, "Effects");
        QVERIFY(effects && effects->opacity == 0.5);  // (its fill opacity: its effects left out)
        // channels: an alpha (its colour, opacity and what it shows), a spot colour, the Quick Mask
        const core::Channel* alpha = core::find_channel(page, "Alpha 1");
        QVERIFY(alpha && alpha->kind == core::ChannelKind::Alpha);
        QCOMPARE(alpha->opacity, 0.35);
        QCOMPARE(alpha->indicates, std::string("selected"));
        QCOMPARE(alpha->color, (std::array<int, 3>{0, 128, 255}));
        const core::Channel* spot = core::find_channel(page, "Spot");
        QVERIFY(spot && spot->kind == core::ChannelKind::Spot);
        QCOMPARE(spot->solidity, 0.7);
        QVERIFY(std::any_of(page.channels.begin(), page.channels.end(), [](const core::Channel& c) { return c.quick_mask; }));
        // guides (mm), paths, the clipping path
        std::vector<double> horizontal;
        for (const Json& r : page.rulers) {
            if (r["axis"] == Json("h")) horizontal.push_back(r["at"].get<double>());
        }
        QCOMPARE(horizontal.size(), std::size_t(2));
        QVERIFY(std::abs(horizontal[1] - std::round(20.5 * mm * 1000) / 1000) < 1e-9);
        QCOMPARE(page.paths[0].name, std::string("Path 1"));
        QVERIFY(page.clipping_path && page.clipping_path->name == "Path 1");
        QVERIFY(page.work_path);
        // what it carried: its XMP (File Info), EXIF, the PSD's resources and blocks
        QVERIFY(doc.file_info && !doc.file_info->xmp.empty());
        QVERIFY(!page.native_source->exif.empty());
        QVERIFY(page.native_psd);
        QCOMPARE(page.native_psd->resources.size(), std::size_t(1));
        QCOMPARE(page.native_psd->resources[0].id, 1037);
        QCOMPARE(page.native_psd->globals.size(), std::size_t(1));
        QCOMPARE(page.native_psd->globals[0].key, std::string("Anno"));
        // what Genko has no place for: said, never silently
        const std::vector<std::string>& notes = opened.notes;
        for (const char* part : {"レイヤー効果", "ラベル", "文字レイヤーは画素", "リンクされたスマートオブジェクト", "レイヤーカンプ", "注釈", "用紙の外",
                                 "高度な合成のチャンネル", "フォルダーの不透明度", "filter.unknown.later"}) {
            QVERIFY2(said(notes, part), qPrintable(QString::fromUtf8(part) + QStringLiteral("\n") + QString::fromStdString(joined(notes))));
        }
    }

    // The adjustments as correction layers of Genko's kinds; the one Genko has not, said.
    void adjustmentsBecomeCorrectionLayers() {
        const formats::Opened opened = formats::open_file(fixture("adjustments.pcraft"), "adjustments.pcraft");
        const core::Page& page = opened.doc.page(0);
        const std::map<std::string, std::string> kinds{
            {"Brightness", "brightness_contrast"}, {"Levels", "levels"},       {"Curves", "curve"},           {"Exposure", "exposure"},
            {"Vibrance", "vibrance"},              {"HueSat", "hue_saturation"}, {"Balance", "color_balance"},  {"BlackWhite", "black_white"},
            {"PhotoFilter", "photo_filter"},       {"Mixer", "channel_mixer"},  {"Lookup", "color_lookup"},    {"Invert", "invert"},
            {"Posterize", "posterize"},            {"Threshold", "threshold"},  {"GradientMap", "gradient_map"}, {"Selective", "selective_color"}};
        for (const auto& [title, kind] : kinds) {
            const core::Layer* l = titled(page, title);
            QVERIFY2(l && l->adjust, title.c_str());
            QCOMPARE((*l->adjust)["kind"].get<std::string>(), kind);
        }
        QVERIFY(!titled(page, "Unknown"));
        QVERIFY2(said(opened.notes, "Unknown"), joined(opened.notes).c_str());
        QCOMPARE(opened.doc.luts.size(), std::size_t(1));  // (the lookup's table, kept with the book)
        // (the page draws: its first correction over its gradient)
        const formats::PagePicture picture = formats::page_picture(page, opened.doc, 150);
        QCOMPARE(picture.image.width(), 64u);
    }

    // A Genko book written as a .pcraft and read back: its layers (pixels, a mask, a folder, a correction, a fill), a
    // channel and a guide; what a .pcraft cannot hold (the panels' borders) as pixels, said.
    void aBookIsWrittenAndReadBack() {
        core::Document doc = small_book();
        const int w = render::mm_to_px(60, 150), h = render::mm_to_px(80, 150);
        render::Image picture = render::Image::create("RGBA", render::Size{w, h}, render::Ink{0, 0, 0, 0});
        picture.paste(render::Ink{200, 40, 30, 255}, render::Box{10, 20, 120, 200});
        picture.paste(render::Ink{20, 90, 210, 128}, render::Box{80, 150, 300, 400});
        render::Image child = render::Image::create("RGBA", render::Size{w, h}, render::Ink{0, 0, 0, 0});
        child.paste(render::Ink{30, 200, 60, 255}, render::Box{200, 30, 330, 120});
        doc = run(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pic"}, {"name", "絵"}},
                                    Json{{"op", "set_layer"}, {"page", 1}, {"id", "pic"}, {"panel_each", false}, {"panel_clip", false}},
                                    Json{{"op", "put_raster"}, {"page", 1}, {"id", "pic"}, {"png_base64", core::b64encode(render::write_png(picture))}},
                                    Json{{"op", "set_layer_mask"}, {"page", 1}, {"id", "pic"}, {"area", Json{{"rect", Json::array({0, 0, 30, 80})}}}},
                                    Json{{"op", "add_layer"}, {"page", 1}, {"kind", "folder"}, {"id", "box"}, {"name", "まとめ"}},
                                    Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "kid"}, {"name", "子"}},
                                    Json{{"op", "set_layer"}, {"page", 1}, {"id", "kid"}, {"panel_each", false}, {"panel_clip", false}},
                                    Json{{"op", "put_raster"}, {"page", 1}, {"id", "kid"}, {"png_base64", core::b64encode(render::write_png(child))}},
                                    Json{{"op", "set_layer"}, {"page", 1}, {"id", "kid"}, {"parent", "box"}},
                                    Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"}, {"id", "adj"}, {"name", "反転"}, {"adjust", Json{{"kind", "invert"}}}},
                                    Json{{"op", "set_layer"}, {"page", 1}, {"id", "adj"}, {"visible", false}},
                                    Json{{"op", "add_channel"}, {"page", 1}, {"name", "空"}, {"fill", "area"}, {"area", Json{{"rect", Json::array({0, 0, 30, 40})}}}}}));
        doc.edit_page(0).rulers.push_back(Json{{"id", "g1"}, {"kind", "guide"}, {"axis", "h"}, {"at", 12.5}});
        std::vector<std::string> notes;
        const std::string bytes = formats::pcraft_bytes(doc, 0, formats::PcraftOptions{}, &notes);
        QVERIFY(pc::is_pcraft(bytes));
        // (the bundle: the manifest first, the previews, the tiles and blobs by their BLAKE3)
        const render::zip::NamedReader zip(bytes);
        QCOMPARE(zip.names().front(), std::string("manifest.json"));
        QVERIFY(zip.contains("thumb.png") && zip.contains("composite/preview.png"));
        const render::Image thumb = render::open_image(zip.read("thumb.png", 1 << 24));
        QCOMPARE(std::max(thumb.width(), thumb.height()), 256);
        const Json m = pc::read_manifest(bytes);
        QCOMPARE(m["format_version"].get<int>(), 1);
        QCOMPARE(m["document"]["size"]["width"].get<int>(), w);
        QCOMPARE(m["document"]["resolution_dpi"].get<double>(), 150.0);
        // read back
        const pc::Read read = pc::read(bytes, "pcraft");
        const core::Page& back = read.doc.page(0);
        QCOMPARE(render::mm_to_px(back.spec.width_mm.value(), 150), w);
        const core::Layer* pic = titled(back, "絵");
        QVERIFY(pic && pic->mask);
        const formats::PagePicture before = formats::layer_picture(doc.page(0), *titled(doc.page(0), "絵"), doc, 150);
        const formats::PagePicture after = formats::layer_picture(back, *pic, read.doc, 150);
        QVERIFY(before.image.data() == after.image.data());
        const render::Image mask_before = render::open_image(*titled(doc.page(0), "絵")->mask->png);
        const render::Image mask_after = render::open_image(*pic->mask->png);
        QCOMPARE(mask_after.width(), w);
        if (mask_before.width() == w && mask_before.height() == h) QVERIFY(mask_after.tobytes() == mask_before.convert("L").tobytes());
        const core::Layer* box = titled(back, "まとめ");
        const core::Layer* kid = titled(back, "子");
        QVERIFY(box && box->kind == core::LayerKind::Folder && kid && kid->parent_id == Json(box->id));
        const core::Layer* adj = titled(back, "反転");
        QVERIFY(adj && adj->adjust && (*adj->adjust)["kind"] == Json("invert") && !adj->visible);
        QCOMPARE(back.channels.size(), std::size_t(1));
        QCOMPARE(back.channels[0].name, std::string("空"));
        std::size_t guides = 0;
        for (const Json& r : back.rulers) {
            if (r.value("kind", std::string()) == "guide") {
                ++guides;
                QVERIFY(std::abs(r["at"].get<double>() - 12.5) < 25.4 / 150);
            }
        }
        QCOMPARE(guides, std::size_t(1));
        // (the panels' borders and the paper: pixels, said)
        QVERIFY2(said(notes, "PhotoCraft に無いもの"), joined(notes).c_str());
        QVERIFY(titled(back, "紙"));
    }

    // Every upstream file read by this build, written by it and read again: the same layers, pixels, masks, channels,
    // paths and guides (8, 16 and 32 bits; grey, CMYK, Lab, indexed).
    void upstreamFilesRoundTrip() {
        for (const char* name : kFixtures) {
            const formats::Opened opened = formats::open_file(fixture(name), name);
            std::vector<std::string> notes;
            const std::string bytes = formats::pcraft_bytes(opened.doc, 0, formats::PcraftOptions{}, &notes);
            const pc::Read again = pc::read(bytes, opened.doc.title);
            same_layers(opened.doc, again.doc, name);
            if (QTest::currentTestFailed()) return;
            genko::test::write_bytes(QString::fromStdString(core::path_to_utf8(at(std::string("again_") + name))), bytes);
        }
    }

    // What a .pcraft is refused for: a broken one ("format"), a newer version ("unsupported_version"), past the limits
    // ("image_too_large") — through open_file too.
    void refusals() {
        const std::string bytes = fixture("gray8.pcraft");
        const auto code_of = [](const std::function<void()>& f) -> std::string {
            try {
                f();
            } catch (const core::Error& error) {
                return error.code();
            }
            return {};
        };
        QCOMPARE(code_of([&] { formats::open_file(bytes.substr(0, bytes.size() / 2), "cut.pcraft"); }), std::string("format"));
        std::string newer;
        render::zip::StoreWriter writer([&newer](std::string_view b) { newer.append(b); });
        const render::zip::NamedReader zip(bytes);
        for (const std::string& n : zip.names()) {
            std::string data = zip.read(n, ~0ull);
            if (n == "manifest.json") {
                Json m = Json::parse(data);
                m["format_version"] = 2;
                data = m.dump();
            }
            writer.add(n, data);
        }
        writer.finish();
        QCOMPARE(code_of([&] { formats::open_file(newer, "newer.pcraft"); }), std::string("unsupported_version"));
        pc::ReadLimits small;
        small.max_unpacked_bytes = 1000;
        QCOMPARE(code_of([&] { pc::read(bytes, "small", small); }), std::string("image_too_large"));
        // (a multichannel page is not written)
        formats::Opened opened = formats::open_file(fixture("cmyk8.pcraft"), "cmyk8.pcraft");
        opened.doc = run(opened.doc, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "multichannel"}}}));
        QVERIFY(opened.doc.page(0).color && opened.doc.page(0).color->space == core::InkSpace::Multi);
        QCOMPARE(code_of([&] { formats::pcraft_bytes(opened.doc, 0); }), std::string("value"));
    }

    // 埋め込みを配置… of a .pcraft: a smart object whose contents are its page (PC3's contents of kind page), what it uses
    // brought into the book, what it could not keep in the op's notes; from the op's data, or the book's asset of it.
    void placingAPcraftMakesContentsOfItsPage() {
        const std::string bytes = fixture("rich_rgb8.pcraft");
        const core::ApplyResult placed = core::CommandBus(render::ops_registry()).apply(
            small_book(), Json::array({Json{{"op", "smart_place"}, {"page", 1}, {"data", core::b64encode(bytes)}, {"name", "rich_rgb8.pcraft"}}}),
            core::Actor("human:test"));
        const core::Page& page = placed.doc.page(0);
        const core::Layer* layer = nullptr;
        for (const core::Layer& l : page.layers) {
            if (l.kind == core::LayerKind::Smart) layer = &l;
        }
        QVERIFY(layer && layer->title == "rich_rgb8");
        const core::SmartContents& contents = *placed.doc.smart_contents.at((*layer->smart)["contents"].get<std::string>());
        QCOMPARE(contents.kind, std::string("page"));
        const formats::Opened opened = formats::open_file(bytes, "rich_rgb8.pcraft");
        QCOMPARE(contents.page->layers.size(), opened.doc.page(0).layers.size());
        QVERIFY(!contents.page->native_source);
        QCOMPARE(placed.doc.patterns.size(), std::size_t(1));  // (the pattern its fill uses, the book's now)
        const Json& result = placed.results[0];
        QVERIFY2(result.contains("size_px"), result.dump().c_str());
        QCOMPARE(result["size_px"], Json::array({300, 200}));
        QVERIFY(result.contains("notes") && !result["notes"].empty());
        // the book's asset of it (what the window places)
        const fs::path dir = at("placed.genko");
        write_book(small_book(), dir);
        const std::string ref = storage::AssetStore(dir).put_bytes(fixture("gray8.pcraft"), ".pcraft");
        core::Document book = storage::load_document(dir).document;
        book = run(book, Json::array({Json{{"op", "smart_place"}, {"page", 1}, {"asset", ref}, {"name", "gray8.pcraft"}}}));
        const auto made = std::find_if(book.page(0).layers.begin(), book.page(0).layers.end(), [](const core::Layer& l) { return l.kind == core::LayerKind::Smart; });
        QVERIFY(made != book.page(0).layers.end());
        const core::SmartContents& grey = *book.smart_contents.at((*made->smart)["contents"].get<std::string>());
        QVERIFY(grey.page->color && grey.page->color->space == core::InkSpace::Gray);
        // (what was read on the window's thread is the op's, once)
        const std::string other = fixture("lab8.pcraft");
        pc::Read early = pc::read(other, "lab8");
        early.doc.edit_page(0).layers[0].title = "前に読んだ";
        pc::remember_read(storage::AssetStore::ref(other), std::move(early));
        const core::Document remembered = run(small_book(), Json::array({Json{{"op", "smart_place"}, {"page", 1}, {"data", core::b64encode(other)}, {"name", "lab8.pcraft"}}}));
        bool seen = false;
        for (const auto& [id, c] : remembered.smart_contents) seen = seen || (c->page && !c->page->layers.empty() && c->page->layers[0].title == "前に読んだ");
        QVERIFY(seen);
    }

    // The command line: `genko export --format pcraft` (a page a file; a .pcraft path the first page) and
    // `genko new --from x.pcraft`.
    void theCommandLine() {
        const fs::path dir = at("cli.genko");
        core::Document doc = small_book();
        doc = run(doc, Json::array({Json{{"op", "add_page"}}}));
        write_book(doc, dir);
        const QString out = QString::fromStdString(core::path_to_utf8(at("cli-out")));
        const auto exported = genko::test::run_genko({QStringLiteral("export"), QString::fromStdString(core::path_to_utf8(dir)), out, QStringLiteral("--format"),
                                                      QStringLiteral("pcraft"), QStringLiteral("--json")});
        QVERIFY2(exported.exit_code == 0, qPrintable(exported.described()));
        const Json said_json = genko::test::one_line(exported.out);
        QCOMPARE(said_json["count"].get<int>(), static_cast<int>(doc.pages.size()));
        for (const Json& file : said_json["files"]) {
            const std::string bytes = genko::test::read_bytes(QString::fromStdString(file.get<std::string>()));
            QVERIFY(pc::is_pcraft(bytes));
            QVERIFY(QString::fromStdString(file.get<std::string>()).endsWith(QStringLiteral(".pcraft")));
            pc::read(bytes, "cli");
        }
        const QString one = QString::fromStdString(core::path_to_utf8(at("one.pcraft")));
        QCOMPARE(genko::test::run_genko({QStringLiteral("export"), QString::fromStdString(core::path_to_utf8(dir)), one, QStringLiteral("--format"),
                                         QStringLiteral("pcraft"), QStringLiteral("--dpi"), QStringLiteral("100")})
                     .exit_code,
                 0);
        QCOMPARE(pc::read_manifest(genko::test::read_bytes(one))["document"]["resolution_dpi"].get<double>(), 100.0);
        // a .pcraft as a new book
        const QString fixture_path = genko::test::test_data(QStringLiteral("pcraft/cmyk8.pcraft"));
        const QString book = QString::fromStdString(core::path_to_utf8(at("from.genko")));
        const auto made = genko::test::run_genko({QStringLiteral("new"), book, QStringLiteral("--from"), fixture_path});
        QVERIFY2(made.exit_code == 0, qPrintable(made.described()));
        const core::Document from = storage::load_document(fs::path(book.toStdString())).document;
        QVERIFY(from.page(0).color && from.page(0).color->space == core::InkSpace::Cmyk);
        QCOMPARE(from.page(0).native_source->format, std::string("pcraft"));
    }

    // (with $GENKO_PCRAFT_CHECK, the tool built: upstream's reader reads what this build wrote)
    void upstreamReadsWhatThisBuildWrites() {
        const QString tool = qEnvironmentVariable("GENKO_PCRAFT_CHECK");
        if (tool.isEmpty()) QSKIP("GENKO_PCRAFT_CHECK is not set (tools/migration/pcraft_fixtures built)");
        QStringList files;
        for (const char* name : kFixtures) files << QString::fromStdString(core::path_to_utf8(at(std::string("again_") + name)));
        for (const QString& f : files) QVERIFY2(QFileInfo::exists(f), qPrintable(f));
        QProcess process;
        process.start(tool, QStringList{QStringLiteral("check")} + files);
        QVERIFY(process.waitForFinished(600000));
        const QByteArray out = process.readAllStandardOutput();
        QVERIFY2(process.exitCode() == 0, out.constData());
        int lines = 0;
        for (const QByteArray& line : out.split('\n')) {
            if (line.trimmed().isEmpty()) continue;
            const Json v = Json::parse(line.toStdString());
            QVERIFY2(v["ok"].get<bool>(), line.constData());
            ++lines;
        }
        QCOMPARE(lines, static_cast<int>(files.size()));
    }
};

QTEST_GUILESS_MAIN(TestContractPcraft)
#include "test_contract_pcraft.moc"
