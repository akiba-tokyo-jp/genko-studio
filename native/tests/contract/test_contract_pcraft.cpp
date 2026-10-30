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
#include <QElapsedTimer>
#include <QProcess>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "core/base64.hpp"
#include "core/channels.hpp"
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/ink.hpp"
#include "core/json.hpp"
#include "core/paths.hpp"
#include "core/smart.hpp"
#include "formats/copy.hpp"
#include "formats/export.hpp"
#include "formats/open_file.hpp"
#include "formats/page_picture.hpp"
#include "formats/pcraft_write.hpp"
#include "render/blake3.hpp"
#include "render/color_canvas.hpp"
#include "render/image.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/pcraft_read.hpp"
#include "render/png.hpp"
#include "render/psd.hpp"
#include "render/raster.hpp"
#include "render/zip.hpp"
#include "render/zstd.hpp"
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
// (groups@1, PC5 I-b, the user's decision 2: a group read as a group — what is in it keeps its own visibility)
void flattened(const Json& layers, bool shown, std::vector<std::pair<const Json*, bool>>& out) {
    for (const Json& l : layers) {
        const bool visible = l["visible"].get<bool>();
        if (l["kind"] == Json("group")) flattened(l["children"], shown, out);
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

// The manifest of a .pcraft's bytes, as JSON.
Json manifest_of(const std::string& bytes) { return Json::parse(render::zip::NamedReader(bytes).read("manifest.json", 1u << 26)); }

// A .pcraft of `manifest`, the objects of `from` (an archive whose tiles and blobs it names) and `more` (name → bytes),
// the manifest first.
std::string bundle(const Json& manifest, const std::string& from, const std::map<std::string, std::string>& more = {}) {
    std::string out;
    render::zip::StoreWriter w([&out](std::string_view b) { out.append(b); });
    w.add("manifest.json", manifest.dump());
    if (!from.empty()) {
        const render::zip::NamedReader r(from);
        for (const std::string& name : r.names()) {
            if (name != "manifest.json" && !more.contains(name)) w.add(name, r.read(name, ~0ull));
        }
    }
    for (const auto& [name, data] : more) w.add(name, data);
    w.finish();
    return out;
}

// A blob of these bytes: its name in the archive and its zstd frame.
std::pair<std::string, std::string> blob_of(const std::string& bytes) {
    return {"blobs/" + render::blake3::hex(bytes) + ".zst", render::zstd::compress(bytes, 1)};
}

// An upstream layer of `content` (the other fields of a plain layer: shown, normal, nothing kept).
Json upstream_layer(const std::string& name, Json content) {
    return Json{{"id", 1},
                {"name", name},
                {"visible", true},
                {"locks", Json{{"transparency", false}, {"pixels", false}, {"position", false}, {"artboard", false}, {"all", false}}},
                {"blend", "Normal"},
                {"opacity", 1.0},
                {"fill_opacity", 1.0},
                {"clipped", false},
                {"mask", nullptr},
                {"vector_mask", nullptr},
                {"effects", Json{{"enabled", true}, {"items", Json::array()}, {"psd_raw", nullptr}, {"reference", nullptr}}},
                {"label", "None"},
                {"content", std::move(content)},
                {"psd_blocks", Json::array()},
                {"psd_id", nullptr},
                {"fill_cache", nullptr},
                {"link_group", nullptr}};
}

// A document of w × h of `mode` and `depth` with these layers (bottom first).
Json upstream_document(int w, int h, const char* mode, const char* depth, Json layers) {
    return Json{{"format_version", 1},
                {"generator", "test"},
                {"document", Json{{"id", 1},
                                  {"name", "test"},
                                  {"size", Json{{"width", w}, {"height", h}}},
                                  {"resolution_dpi", 300.0},
                                  {"mode", mode},
                                  {"depth", depth},
                                  {"icc_profile", nullptr},
                                  {"layers", std::move(layers)}}}};
}

// A raster layer's surface of a format: its tiles (none: nothing drawn).
Json raster_content(const char* mode, const char* sample, Json tiles = Json::array()) {
    const std::size_t bytes = std::string_view(sample) == "U8" ? 1 : std::string_view(sample) == "U16" ? 2 : 4;
    const std::size_t channels = std::string_view(mode) == "Grayscale" ? 2 : std::string_view(mode) == "Cmyk" ? 5 : 4;
    return Json{{"kind", "raster"},
                {"surface", Json{{"format", Json{{"mode", mode}, {"sample", sample}, {"alpha", true}}}, {"default", std::string(bytes * channels * 2, '0')}, {"tiles", std::move(tiles)}}}};
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
    request.base_revision = doc.revision;  // (a book read from its folder: saved over the revision it was read at)
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

// (placedContentsAreSavedWhole, its errors said)
void placed_whole(const fs::path& dir) {
    write_book(small_book(), dir);
    core::Document book = storage::load_document(dir).document;
    for (const char* name : {"rich_rgb8.pcraft", "adjustments.pcraft"}) {
        const std::string ref = storage::AssetStore(dir).put_bytes(fixture(name), ".pcraft");
        book = run(book, Json::array({Json{{"op", "smart_place"}, {"page", 1}, {"asset", ref}, {"name", name}}}));
    }
    write_book(book, dir);
    const storage::LoadResult again = storage::load_document(dir);
    QVERIFY2(again.report.clean(), again.report.to_json().dump().c_str());
    bool lut = false, lnsr = false;
    for (const auto& [id, contents] : again.document.smart_contents) {
        if (!contents->page) continue;
        for (const core::Layer& l : contents->page->layers) {
            if (l.adjust && l.adjust->value("kind", std::string()) == "color_lookup") lut = again.document.luts.contains(l.adjust->value("lut", std::string()));
            if (l.native_psd) {
                for (const auto& [key, ref] : l.native_psd->blocks) lnsr = lnsr || key == "lnsr";
            }
        }
    }
    QVERIFY(lut);
    QVERIFY(lnsr);
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
                if (kind == "group") {  // (groups@1, PC5 I-b: a group with its settings)
                    QVERIFY2(l.kind == core::LayerKind::Folder && l.group, where.c_str());
                    QVERIFY2(std::abs(l.opacity - u["opacity"].get<double>()) < 1e-3, where.c_str());
                    QVERIFY2(l.mask.has_value() == u["mask"].get<bool>(), where.c_str());
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
        // (PC5 I-a, blend-channels@1: its excluded R the layer's own now — not said)
        QCOMPARE(effects->blend_channels, (std::vector<bool>{false, true, true}));
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
        // (PC5 J-a: its layer comps are the page's now — not said)
        QVERIFY(page.layer_comps && page.layer_comps->comps.size() == 1 && page.layer_comps->comps[0].name == "Comp");
        QVERIFY(page.layer_comps->comps[0].apply_visibility && !page.layer_comps->comps[0].apply_position);
        QVERIFY(!said(notes, "レイヤーカンプ"));
        // (PC5 J-c: its note is the page's now — not said)
        QCOMPARE(page.annotations.size(), std::size_t(1));
        QCOMPARE(page.annotations[0].author, std::string("A"));
        QCOMPARE(page.annotations[0].text, std::string("note"));
        QVERIFY(!said(notes, "注釈"));
        QVERIFY(!said(notes, "高度な合成のチャンネル"));  // (PC5 I-a)
        for (const char* part : {"レイヤー効果", "ラベル", "文字レイヤーは画素", "リンクされたスマートオブジェクト", "用紙の外",
                                 "filter.unknown.later"}) {
            QVERIFY2(said(notes, part), qPrintable(QString::fromUtf8(part) + QStringLiteral("\n") + QString::fromStdString(joined(notes))));
        }
    }

    // PC4 D7: .pcraft's smart filters through the table (core::from_upstream, to_upstream) both ways — the fixture's Gaussian
    // in the working 200 dpi's pixels; a stack in PhotoCraft's words (camelCase, colours 0..1, a pin's blur in the document's
    // pixels, a hidden one) read as Genko's, one PhotoCraft does not know left out and said; written back as PhotoCraft's
    // commands and read the same —; a filter PhotoCraft draws that Genko cannot take, a blend Genko has not, or more filters
    // than a smart object holds make the smart object its picture (the stack never read in part), said; a Genko filter
    // PhotoCraft has none of makes the written smart object its picture, said.
    void smartFiltersBothWays() {
        const std::string rich = fixture("rich_rgb8.pcraft");
        const Json base = manifest_of(rich);
        const double dpi = base["document"]["resolution_dpi"].get<double>();
        const std::function<Json*(Json&, const std::string&)> find = [&find](Json& layers, const std::string& name) -> Json* {
            for (Json& l : layers) {
                if (l.value("name", std::string()) == name) return &l;
                if (l["content"].value("kind", std::string()) == "group")
                    if (Json* inside = find(l["content"]["children"], name)) return inside;
            }
            return nullptr;
        };
        const auto with_filters = [&](const Json& filters) {
            Json m = base;
            Json* smart = find(m["document"]["layers"], "Smart");
            if (smart == nullptr) throw std::runtime_error("no Smart layer");
            (*smart)["content"]["smart_filters"] = filters;
            return bundle(m, rich);
        };
        const auto sf = [](const std::string& command, Json params, const std::string& blend = "Normal", bool visible = true) {
            return Json{{"command", command}, {"params", std::move(params)}, {"blend", blend}, {"opacity", 0.75}, {"visible", visible}};
        };
        // (the fixture's: a Gaussian of 2 of its pixels)
        {
            const formats::Opened opened = formats::open_file(rich, "rich_rgb8.pcraft");
            const core::Layer* smart = titled(opened.doc.page(0), "Smart");
            QVERIFY(smart && smart->smart);
            const Json& f = (*smart->smart)["filters"][0];
            QCOMPARE(f["kind"], Json("blur_gaussian"));
            QVERIFY(std::abs(f["params"]["radius"].get<double>() - 2 * 200 / dpi) < 1e-9);
        }
        // read: PhotoCraft's words and units
        const formats::Opened read = formats::open_file(
            with_filters(Json::array({sf("filter.stylize.wind", Json{{"direction", "fromLeft"}, {"method", "stagger"}}),
                                      sf("filter.pixelate.pointillize", Json{{"cellSize", 12}, {"background", Json::array({0.0, 0.5, 1.0})}}, "Multiply"),
                                      sf("filter.blurGallery.irisBlur", Json{{"pins", Json::array({Json{{"x", 0.25}, {"y", 0.5}, {"blur", 30}}})}}, "Normal", false),
                                      sf("filter.unknown.later", Json::object())})),
            "words.pcraft");
        const core::Layer* smart = titled(read.doc.page(0), "Smart");
        QVERIFY(smart && smart->kind == core::LayerKind::Smart);
        const Json filters = (*smart->smart)["filters"];
        QCOMPARE(filters.size(), std::size_t(3));
        QCOMPARE(filters[0]["kind"], Json("stylize_wind"));
        QCOMPARE(filters[0]["params"]["direction"], Json("from_left"));
        QCOMPARE(filters[0]["params"]["method"], Json("stagger"));
        QCOMPARE(filters[0]["opacity"].get<double>(), 0.75);
        QCOMPARE(filters[1]["kind"], Json("pixelate_pointillize"));
        QVERIFY(std::abs(filters[1]["params"]["cell_size"].get<double>() - 12 * 200 / dpi) < 1e-9);
        QCOMPARE(filters[1]["params"]["background"], Json::array({0, 128, 255}));
        QCOMPARE(filters[1]["blend"], Json("multiply"));
        QCOMPARE(filters[2]["kind"], Json("blur_iris"));
        QVERIFY(std::abs(filters[2]["params"]["pins"][0]["blur"].get<double>() - 30 * 200 / dpi) < 1e-9);
        QCOMPARE(filters[2]["visible"], Json(false));
        QVERIFY2(said(read.notes, "filter.unknown.later"), joined(read.notes).c_str());
        core::smart::validate_smart(read.doc);
        // written back: PhotoCraft's commands, read the same
        std::vector<std::string> notes;
        const std::string bytes = formats::pcraft_bytes(read.doc, 0, formats::PcraftOptions{}, &notes);
        Json written = manifest_of(bytes);
        const Json* layer = find(written["document"]["layers"], "Smart");
        QVERIFY(layer && (*layer)["content"]["kind"] == Json("smart"));
        const Json& stack = (*layer)["content"]["smart_filters"];
        QCOMPARE(stack.size(), std::size_t(3));
        QCOMPARE(stack[0]["command"], Json("filter.stylize.wind"));
        QCOMPARE(stack[0]["params"]["direction"], Json("fromLeft"));
        QCOMPARE(stack[1]["command"], Json("filter.pixelate.pointillize"));
        QVERIFY(std::abs(stack[1]["params"]["cellSize"].get<double>() - 12) < 1e-9);
        QVERIFY(std::abs(stack[1]["params"]["background"][1].get<double>() - 128 / 255.0) < 1e-9);
        QCOMPARE(stack[1]["blend"], Json("Multiply"));
        QCOMPARE(stack[2]["command"], Json("filter.blurGallery.irisBlur"));
        QVERIFY(std::abs(stack[2]["params"]["pins"][0]["blur"].get<double>() - 30) < 1e-9);
        QCOMPARE(stack[2]["visible"], Json(false));
        const formats::Opened again = formats::open_file(bytes, "again.pcraft");
        const core::Layer* back = titled(again.doc.page(0), "Smart");
        QVERIFY(back && back->smart);
        QCOMPARE((*back->smart)["filters"].size(), std::size_t(3));
        for (std::size_t i = 0; i < 3; ++i) {
            QCOMPARE((*back->smart)["filters"][i]["kind"], filters[i]["kind"]);
            QCOMPARE((*back->smart)["filters"][i]["blend"], filters[i]["blend"]);
        }
        QCOMPARE((*back->smart)["filters"][1]["params"]["background"], Json::array({0, 128, 255}));
        // not mapped: the smart object its picture, the stack never read in part
        Json lots = Json::array();
        for (int i = 0; i < 300; ++i) lots.push_back(sf("filter.blur.gaussianBlur", Json{{"radius", 1}}));
        const std::vector<std::pair<Json, std::string>> unmapped{
            {Json::array({sf("filter.blur.gaussianBlur", Json{{"radius", 2}}), sf("filter.liquify", Json::object())}), "filter.liquify"},
            {Json::array({sf("filter.blur.gaussianBlur", Json{{"radius", 2}}, "VividLight")}), "VividLight"},
            {Json::array({sf("filter.distort.displace", Json{{"map", "/tmp/map.psd"}})}), "filter.distort.displace"},
            {lots, "64 個を超える"},
        };
        for (const auto& [stack_given, part] : unmapped) {
            const formats::Opened opened = formats::open_file(with_filters(stack_given), "unmapped.pcraft");
            const core::Layer* pixels = titled(opened.doc.page(0), "Smart");
            QVERIFY2(pixels && pixels->kind == core::LayerKind::Raster && !pixels->smart, part.c_str());
            QVERIFY2(said(opened.notes, part) && said(opened.notes, "見えのまま画素にしました"), (part + "\n" + joined(opened.notes)).c_str());
        }
        // review P2-4 and P3-5: one that does not show (hidden, or the stack off), or a smart object without upstream's
        // picture, keeps the smart object — the filter left out, said; one upstream does not draw as a smart filter (平均・
        // 雲模様) left out as upstream leaves it
        const auto kept_without = [&](Json stack_given, const std::function<void(Json&)>& edit) {
            Json m = base;
            Json* layer_given = find(m["document"]["layers"], "Smart");
            (*layer_given)["content"]["smart_filters"] = std::move(stack_given);
            edit((*layer_given)["content"]);
            return formats::open_file(bundle(m, rich), "kept.pcraft");
        };
        const Json liquify_hidden = Json::array({sf("filter.blur.gaussianBlur", Json{{"radius", 2}}), sf("filter.liquify", Json::object(), "Normal", false)});
        const Json liquify_shown = Json::array({sf("filter.blur.gaussianBlur", Json{{"radius", 2}}), sf("filter.liquify", Json::object())});
        const Json average = Json::array({sf("filter.blur.gaussianBlur", Json{{"radius", 2}}), sf("filter.blur.average", Json::object())});
        const std::vector<std::tuple<std::string, Json, std::function<void(Json&)>, std::string>> kept_cases{
            {"hidden", liquify_hidden, [](Json&) {}, "filter.liquify"},
            {"stack off", liquify_shown, [](Json& c) { c["filters_enabled"] = false; }, "filter.liquify"},
            {"no cache", liquify_shown, [](Json& c) { c["cache"] = nullptr; }, "filter.liquify"},
            {"VividLight, no cache", Json::array({sf("filter.blur.gaussianBlur", Json{{"radius", 2}}), sf("filter.blur.boxBlur", Json{{"radius", 2}}, "VividLight")}),
             [](Json& c) { c["cache"] = nullptr; }, "filter.blur.boxBlur"},
            {"average", average, [](Json&) {}, "filter.blur.average"},
        };
        for (const auto& [what, stack_given, edit, part] : kept_cases) {
            const formats::Opened opened = kept_without(stack_given, edit);
            const core::Layer* kept = titled(opened.doc.page(0), "Smart");
            QVERIFY2(kept && kept->kind == core::LayerKind::Smart && kept->smart, what.c_str());
            QCOMPARE((*kept->smart)["filters"].size(), std::size_t(1));
            QCOMPARE((*kept->smart)["filters"][0]["kind"], Json("blur_gaussian"));
            QVERIFY2(said(opened.notes, part) && said(opened.notes, "外しました"), (what + "\n" + joined(opened.notes)).c_str());
            core::smart::validate_smart(opened.doc);
        }
        // (the Genko kinds upstream does not draw as smart filters are written as the smart object's picture)
        const core::Document averaged =
            run(read.doc, Json::array({Json{{"op", "smart_filter_add"}, {"page", 1}, {"id", smart->id}, {"kind", "blur_average"}}}));
        std::vector<std::string> average_notes;
        Json average_written = manifest_of(formats::pcraft_bytes(averaged, 0, formats::PcraftOptions{}, &average_notes));
        const Json* average_layer = find(average_written["document"]["layers"], "Smart");
        QVERIFY(average_layer && (*average_layer)["content"]["kind"] == Json("raster"));
        QVERIFY2(said(average_notes, "平均"), joined(average_notes).c_str());
        // a Genko filter PhotoCraft has none of (Python's blur): written as its picture, said
        const core::Document blurred =
            run(read.doc, Json::array({Json{{"op", "smart_filter_add"}, {"page", 1}, {"id", smart->id}, {"kind", "blur"}, {"radius", 2}}}));
        std::vector<std::string> said_notes;
        Json picture = manifest_of(formats::pcraft_bytes(blurred, 0, formats::PcraftOptions{}, &said_notes));
        const Json* as_pixels = find(picture["document"]["layers"], "Smart");
        QVERIFY(as_pixels && (*as_pixels)["content"]["kind"] == Json("raster"));
        QVERIFY2(said(said_notes, "PhotoCraft に同じものが無いため、スマートオブジェクトを見えのまま画素にしました"), joined(said_notes).c_str());
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

    // PC4 F-f review P1-1: the read's memory — a layer of nothing made of nothing; a page in precise colour past the book's
    // precise budget read at 8 bits (said), as is a canvas past a precise raster's; past the layers', planes' and kept
    // bytes' limits refused (image_too_large), before what is past them is made.
    void hostileSizesAreRefusedBeforeTheyAreMade() {
        // (a manifest of a few KB: 40 layers of nothing on a canvas of 8192 × 8191 at 16 bits — the most a precise layer holds)
        Json layers = Json::array();
        for (int i = 0; i < 40; ++i) layers.push_back(upstream_layer("空 " + std::to_string(i), raster_content("Rgb", "U16")));
        const std::string empty = bundle(upstream_document(8192, 8191, "Rgb", "U16", layers), "");
        QVERIFY(empty.size() < 64 * 1024);
        QElapsedTimer clock;
        clock.start();
        const pc::Read read = pc::read(empty, "empty");
        QVERIFY(clock.elapsed() < 30000);
        QCOMPARE(read.doc.page(0).layers.size(), std::size_t(40));
        for (const core::Layer& l : read.doc.page(0).layers) QVERIFY(!l.raster_png && !l.color_raster && !l.ink_raster);
        QVERIFY(read.doc.page(0).rgb && read.doc.page(0).rgb->depth == 16);
        // (3 layers of one tile each at 16 bits on 7000 × 7000: past the book's 1 GiB of precise pixels — 8 bits, said)
        const std::string tile(std::size_t(256) * 256 * 8, '\x40');
        const std::string hash = render::blake3::hex(tile);
        const std::map<std::string, std::string> tiles{{"tiles/" + hash + ".zst", render::zstd::compress(tile, 1)}};
        Json three = Json::array();
        for (int i = 0; i < 3; ++i) three.push_back(upstream_layer("絵 " + std::to_string(i), raster_content("Rgb", "U16", Json::array({Json{{"tx", i}, {"ty", 0}, {"hash", hash}}}))));
        const std::string deep = bundle(upstream_document(7000, 7000, "Rgb", "U16", three), "", tiles);
        const pc::Read shallow = pc::read(deep, "deep");
        QVERIFY(!shallow.doc.page(0).rgb || shallow.doc.page(0).rgb->depth == 8);
        QVERIFY2(said(shallow.notes, "8 bit で読みました"), joined(shallow.notes).c_str());
        QVERIFY(shallow.doc.page(0).layers[0].raster_png);
        // (those that fit: kept at 16 bits)
        const pc::Read precise = pc::read(bundle(upstream_document(512, 512, "Rgb", "U16", three), "", tiles), "three");
        QVERIFY(precise.doc.page(0).rgb && precise.doc.page(0).rgb->depth == 16);
        QVERIFY(precise.doc.page(0).layers[0].color_raster);
        // (a canvas past a precise raster's pixels — 8193 × 8192 at 16 bits —: 8 bits, said)
        Json one = Json::array({upstream_layer("絵", raster_content("Rgb", "U16", Json::array({Json{{"tx", 0}, {"ty", 0}, {"hash", hash}}})))});
        const pc::Read wide = pc::read(bundle(upstream_document(8193, 8192, "Rgb", "U16", one), "", tiles), "wide");
        QVERIFY(!wide.doc.page(0).rgb || wide.doc.page(0).rgb->depth == 8);
        QVERIFY(said(wide.notes, "8 bit で読みました"));
        // (the limits: layers, planes, kept bytes — refused, never "broken")
        const auto code_of = [](const std::function<void()>& f) -> std::string {
            try {
                f();
            } catch (const core::Error& error) {
                return error.code();
            }
            return {};
        };
        pc::ReadLimits few;
        few.max_layers = 10;
        QCOMPARE(code_of([&] { (void)pc::read(empty, "few", few); }), std::string("image_too_large"));
        pc::ReadLimits planes;
        planes.max_plane_pixels = std::uint64_t(7000) * 7000 * 2;
        QCOMPARE(code_of([&] { (void)pc::read(deep, "planes", planes); }), std::string("image_too_large"));
        pc::ReadLimits kept;
        kept.max_kept_bytes = 1000;
        QCOMPARE(code_of([&] { (void)pc::read(fixture("rich_rgb8.pcraft"), "kept", kept); }), std::string("image_too_large"));
        // (8001 layers: past Photoshop's most)
        Json many = Json::array();
        for (int i = 0; i < 8001; ++i) many.push_back(upstream_layer("l", raster_content("Rgb", "U8")));
        QCOMPARE(code_of([&] { (void)pc::read(bundle(upstream_document(16, 16, "Rgb", "U8", many), ""), "many"); }), std::string("image_too_large"));
    }

    // PC4 F-f review P1-2: a .pcraft inside, shown by many smart objects, read once — its contents shared —; nesting counts in
    // the outer read's limits (1000 smart objects of a .pcraft of 1000 smart objects of a .pcraft: two reads, not a million).
    void nestedDocumentsAreReadOnce() {
        const auto smart_of = [](const std::string& hash, const std::string& file) {
            return Json{{"kind", "smart"},
                        {"source", Json{{"kind", "embedded"}, {"file_name", file}, {"blob", hash}}},
                        {"transform", Json{{"m", Json::array({1.0, 0.0, 0.0, 1.0, 0.0, 0.0})}}},
                        {"smart_filters", Json::array()},
                        {"cache", nullptr},
                        {"psd_raw", nullptr},
                        {"filters_enabled", true},
                        {"filter_mask", nullptr},
                        {"warp", nullptr},
                        {"stack_mode", nullptr}};
        };
        const std::string inner = bundle(upstream_document(40, 30, "Rgb", "U8", Json::array()), "");
        const auto [inner_name, inner_frame] = blob_of(inner);
        const std::string inner_hash = render::blake3::hex(inner);
        Json middle_layers = Json::array();
        for (int i = 0; i < 1000; ++i) middle_layers.push_back(upstream_layer("中 " + std::to_string(i), smart_of(inner_hash, "inner.pcraft")));
        const std::string middle = bundle(upstream_document(80, 60, "Rgb", "U8", middle_layers), "", {{inner_name, inner_frame}});
        const auto [middle_name, middle_frame] = blob_of(middle);
        const std::string middle_hash = render::blake3::hex(middle);
        Json outer_layers = Json::array();
        for (int i = 0; i < 1000; ++i) outer_layers.push_back(upstream_layer("外 " + std::to_string(i), smart_of(middle_hash, "middle.pcraft")));
        const std::string outer = bundle(upstream_document(160, 120, "Rgb", "U8", outer_layers), "", {{middle_name, middle_frame}});
        QElapsedTimer clock;
        clock.start();
        const formats::Opened opened = formats::open_file(outer, "outer.pcraft");
        QVERIFY2(clock.elapsed() < 60000, std::to_string(clock.elapsed()).c_str());
        QCOMPARE(opened.doc.page(0).layers.size(), std::size_t(1000));
        QCOMPARE(opened.doc.smart_contents.size(), std::size_t(2));  // (middle's page, and inner's inside it)
        std::set<std::string> ids;
        for (const core::Layer& l : opened.doc.page(0).layers) ids.insert((*l.smart)["contents"].get<std::string>());
        QCOMPARE(ids.size(), std::size_t(1));
        // (the layers inside count in the outer read's: past them, refused)
        pc::ReadLimits few;
        few.max_layers = 1500;
        QVERIFY_THROWS_EXCEPTION(core::Error, (void)pc::read(outer, "outer", few));
    }

    // PC4 F-f review P1-3: a .pcraft placed in a saved book (its contents a page), the book saved and read again whole — the
    // PSD's blocks and resources, the lookup tables, the filter masks its page names brought into the book —, never read-only.
    void placedContentsAreSavedWhole() {
        try {
            placed_whole(at("placed-whole.genko"));
        } catch (const std::exception& error) {
            QFAIL(error.what());
        }
    }

    // PC4 F-f review P2-2: names cut by characters, never inside one: long Japanese names of paths, channels and inks made
    // one each, the book saved and read again.
    void longNamesStayText() {
        const std::string bytes = fixture("rich_rgb8.pcraft");
        Json m = manifest_of(bytes);
        const auto times = [](const std::string& part, int n) {
            std::string out;
            for (int i = 0; i < n; ++i) out += part;
            return out;
        };
        const std::string long_name = times("長い名前", 18);  // (72 characters, 216 bytes: past the 64 characters a name holds)
        Json& d = m["document"];
        for (Json& path : d["paths"]) path["name"] = long_name;
        d["channels"][0]["name"] = long_name;
        d["channels"][1] = d["channels"][0];
        d["patterns"][0]["name"] = times("模様", 150);  // (300 characters: past the 255 a pattern's name holds)
        const formats::Opened opened = formats::open_file(bundle(m, bytes), "names.pcraft");
        const core::Page& page = opened.doc.page(0);
        const auto text = [](const std::string& name) { return QString::fromStdString(name).toStdString() == name; };
        QCOMPARE(page.paths.size(), std::size_t(2));
        QVERIFY(page.paths[0].name != page.paths[1].name);
        for (const auto& path : page.paths) QVERIFY2(text(path.name), path.name.c_str());
        std::vector<std::string> channels;
        for (const auto& ch : page.channels) {
            QVERIFY2(text(ch.name), ch.name.c_str());
            if (ch.name.starts_with("長い名前")) channels.push_back(ch.name);
        }
        QCOMPARE(channels.size(), std::size_t(2));
        QVERIFY(channels[0] != channels[1]);
        QCOMPARE(opened.doc.patterns.size(), std::size_t(1));
        QVERIFY(text(opened.doc.patterns[0].name));
        const fs::path dir = at("names.genko");
        write_book(opened.doc, dir);
        const storage::LoadResult again = storage::load_document(dir);
        QVERIFY2(again.report.clean(), again.report.to_json().dump().c_str());
        QCOMPARE(again.document.page(0).paths[1].name, page.paths[1].name);
    }

    // PC4 F-f review P2-4: a fill layer's gradient laid out in upstream's frame — a hide-all mask's painted area —, read and
    // written back the same.
    void aFillsFrameIsItsMasksPaintedArea() {
        const std::string bytes = fixture("rich_rgb8.pcraft");
        Json m = manifest_of(bytes);
        Json* gradient = nullptr;
        Json* solid = nullptr;
        for (Json& l : m["document"]["layers"]) {
            if (l["name"] == Json("Gradient")) gradient = &l;
            if (l["name"] == Json("Solid")) solid = &l;
        }
        QVERIFY(gradient && solid);
        (*gradient)["mask"] = (*solid)["mask"];  // (hide all, 200..260 × 150..190 shown)
        (*gradient)["visible"] = true;
        const pc::Read read = pc::read(bundle(m, bytes), "frame");
        const core::Layer* layer = titled(read.doc.page(0), "Gradient");
        QVERIFY(layer && layer->fill && layer->fill->contains("gradient") && layer->mask);
        // (the frame: where its mask shows)
        const render::Image mask = render::open_image(*layer->mask->png).convert("L");
        const std::string grey = mask.tobytes();
        int x0 = mask.width(), y0 = mask.height(), x1 = -1, y1 = -1;
        for (int y = 0; y < mask.height(); ++y) {
            for (int x = 0; x < mask.width(); ++x) {
                if (grey[std::size_t(y) * mask.width() + x] == 0) continue;
                x0 = std::min(x0, x);
                y0 = std::min(y0, y);
                x1 = std::max(x1, x);
                y1 = std::max(y1, y);
            }
        }
        QVERIFY(x1 >= x0 && (x1 - x0 + 1) * (y1 - y0 + 1) < mask.width() * mask.height() / 2);
        const Json& g = (*layer->fill)["gradient"];
        const double mm = 25.4 / static_cast<double>(read.doc.page(0).spec.dpi.value());
        const double cx = (g["from"][0].get<double>() + g["to"][0].get<double>()) / 2 / mm, cy = (g["from"][1].get<double>() + g["to"][1].get<double>()) / 2 / mm;
        QVERIFY2(std::abs(cx - (x0 + x1 + 1) / 2.0) < 1 && std::abs(cy - (y0 + y1 + 1) / 2.0) < 1,
                 (std::to_string(cx) + " " + std::to_string(cy) + " / " + std::to_string(x0) + ".." + std::to_string(x1) + " " + std::to_string(y0) + ".." +
                  std::to_string(y1))
                     .c_str());
        // (written back: the same angle and scale over the same frame)
        const Json written = manifest_of(formats::pcraft_bytes(read.doc, 0));
        bool found = false;
        for (const Json& l : written["document"]["layers"]) {
            if (l["name"] != Json("Gradient")) continue;
            found = true;
            const Json& f = l["content"]["fill"]["Gradient"];
            QVERIFY2(std::abs(f["angle"].get<double>() - 45.0) < 0.01, f.dump().c_str());
            QVERIFY2(std::abs(f["scale"].get<double>() - 1.0) < 0.01, f.dump().c_str());
            QCOMPARE(l["mask"]["surface"]["default"].get<std::string>(), std::string("00"));
        }
        QVERIFY(found);
    }

    // PC4 F-f review P2-3: on an RGB page of a profile, what Genko draws (sRGB: a pen's line) is written as the page's values.
    void anSrgbPictureIsWrittenAsThePagesValues() {
        formats::Opened opened = formats::open_file(fixture("rgb16.pcraft"), "p3.pcraft");
        core::Document doc = run(opened.doc, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}, {"profile", "display-p3"}},
                                                          Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "pen"}, {"name", "線"}},
                                                          Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", "pen"},
                                                               {"points", Json::array({Json::array({1.0, 4.0}), Json::array({11.0, 4.0})})}, {"width_mm", 2.0},
                                                               {"rgb", Json::array({255, 0, 0})}}}));
        QVERIFY(doc.page(0).rgb && !doc.page(0).rgb->profile.empty());
        std::vector<std::string> notes;
        const pc::Read back = pc::read(formats::pcraft_bytes(doc, 0, formats::PcraftOptions{}, &notes), "p3");
        QVERIFY2(said(notes, "ペンの線"), joined(notes).c_str());
        const core::Layer* line = titled(back.doc.page(0), "線");
        QVERIFY(line && line->color_raster);
        const auto px = pixels_of(*line);
        const int dpi = static_cast<int>(back.doc.page(0).spec.dpi.value());
        const int x = render::mm_to_px(6.0, dpi), y = render::mm_to_px(4.0, dpi);
        QCOMPARE(px->at(x, y, 3), 1.0);
        double rgb[3] = {1.0, 0.0, 0.0};
        render::RgbSpace::of(back.doc.page(0)).from_srgb(rgb, 1);
        for (unsigned c = 0; c < 3; ++c) QVERIFY2(std::abs(px->at(x, y, c) - rgb[c]) <= 1.5 / 255, (std::to_string(c) + ": " + std::to_string(px->at(x, y, c))).c_str());
        QVERIFY(std::abs(px->at(x, y, 0) - 1.0) > 2.0 / 255);  // (not sRGB's red as it is)
    }

    // PC4 F-f review P2-5: what upstream kept of a PSD, as PC4 F-d keeps it — the blocks of a kind made pixels left out (a
    // text's TySh), the resources this build writes not kept twice —, and the PSD written of it so.
    void psdBlocksAreKeptAsAPsdsAre() {
        const std::string bytes = fixture("rich_rgb8.pcraft");
        Json m = manifest_of(bytes);
        std::string lnsr;
        for (Json& l : m["document"]["layers"]) {
            if (l["name"] == Json("Paint")) lnsr = l["psd_blocks"][0][1].get<std::string>();
        }
        for (Json& l : m["document"]["layers"]) {
            if (l["name"] == Json("Type")) l["psd_blocks"] = Json::array({Json::array({"54795368", lnsr}), Json::array({"6c6e7372", lnsr})});  // (TySh, lnsr)
            if (l["name"] == Json("Paint")) l["psd_blocks"].push_back(Json::array({"54795368", lnsr}));  // (a TySh on a raster layer: not kept)
        }
        m["document"]["metadata"]["psd_resources"].push_back(Json::array({2999, "", lnsr}));  // (the clipping path: this build's)
        m["document"]["metadata"]["psd_resources"].push_back(Json::array({1046, "", lnsr}));  // (an indexed page's count)
        const formats::Opened opened = formats::open_file(bundle(m, bytes), "kept.pcraft");
        const core::Page& page = opened.doc.page(0);
        QVERIFY(!titled(page, "Type")->native_psd);  // (made pixels: what it kept of its text left out)
        for (const auto& [key, ref] : titled(page, "Paint")->native_psd->blocks) QVERIFY(key != "TySh");
        std::set<int> ids;
        for (const core::PsdResource& r : page.native_psd->resources) ids.insert(r.id);
        QCOMPARE(ids, (std::set<int>{1037}));
        // (its PSD: no TySh on a layer, one clipping path resource at most)
        const fs::path psd = at("kept.psd");
        formats::export_page_psd(opened.doc, page, psd);
        const render::psd::File file = render::psd::File::read(genko::test::read_bytes(QString::fromStdString(core::path_to_utf8(psd))));
        for (const auto& layer : file.layers()) {
            for (const auto& block : file.blocks(layer)) QVERIFY(block.key != "TySh");
        }
        int clipping = 0, counts = 0;
        for (const auto& r : file.resources()) {
            clipping += r.id == 2999;
            counts += r.id == 1046;
        }
        QVERIFY(clipping <= 1 && counts == 0);
    }

    // PC4 F-f review P2-6 and P2-1: what a .pcraft cannot hold said when it is written — a pen's lines made pixels, a
    // smart object whose file the book does not hold made pixels —; コピーを保存 of a .pcraft written beside its place and
    // moved there.
    void whatIsNotWrittenIsSaid() {
        core::Document doc = run(small_book(), Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "pen"}, {"name", "ペン"}},
                                                            Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", "pen"},
                                                                 {"points", Json::array({Json::array({5, 5}), Json::array({30, 30})})}},
                                                            Json{{"op", "smart_place"}, {"page", 1}, {"data", core::b64encode(fixture("rich_rgb8.pcraft"))},
                                                                 {"name", "x.pcraft"}}}));
        // (a smart object of a file the book no longer holds)
        core::Document lost = run(small_book(), Json::array({Json{{"op", "smart_place"}, {"page", 1},
                                                                 {"data", core::b64encode(render::write_png(render::Image::create("RGB", render::Size{8, 8}, render::Ink{1, 2, 3})))},
                                                                 {"name", "lost.png"}}}));
        lost.smart_files.clear();
        std::vector<std::string> notes;
        (void)formats::pcraft_bytes(doc, 0, formats::PcraftOptions{}, &notes);
        QVERIFY2(said(notes, "ペンの線"), joined(notes).c_str());
        std::vector<std::string> lost_notes;
        (void)formats::pcraft_bytes(lost, 0, formats::PcraftOptions{}, &lost_notes);
        QVERIFY2(said(lost_notes, "中身のファイルが本に無い"), joined(lost_notes).c_str());
        // (コピーを保存: the file whole, nothing left beside it)
        const fs::path folder = at("copy-out");
        fs::create_directories(folder);
        const formats::CopyResult copied = formats::write_copy(doc, 0, folder / "copy.pcraft");
        QVERIFY(pc::is_pcraft(genko::test::read_bytes(QString::fromStdString(core::path_to_utf8(copied.file)))));
        std::size_t files = 0;
        for (const auto& entry : fs::directory_iterator(folder)) files += entry.is_regular_file();
        QCOMPARE(files, std::size_t(1));
    }

    // (with $GENKO_PCRAFT_CHECK, the tool built: upstream's reader reads what this build wrote — every fixture read and
    // written again, its layers' samples where upstream read the fixture's; a Genko book's page)
    void upstreamReadsWhatThisBuildWrites() {
        const QString tool = qEnvironmentVariable("GENKO_PCRAFT_CHECK");
        if (tool.isEmpty()) QSKIP("GENKO_PCRAFT_CHECK is not set (tools/migration/pcraft_fixtures built)");
        QStringList files;
        for (const char* name : kFixtures) files << QString::fromStdString(core::path_to_utf8(at(std::string("again_") + name)));
        // (a Genko book's page: paper, a pen's lines, a shape, a fill, a correction, a smart object of a picture)
        core::Document book = run(small_book(), Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "pen"}},
                                                             Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", "pen"},
                                                                  {"points", Json::array({Json::array({5, 5}), Json::array({30, 30})})}},
                                                             Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "sh"}},
                                                             Json{{"op", "add_shape_part"}, {"page", 1}, {"layer_id", "sh"},
                                                                  {"shape", Json{{"kind", "rect"}, {"rect", Json::array({10, 10, 20, 15})}}}, {"fill", Json{{"rgb", Json::array({0, 0, 0})}}}},
                                                             Json{{"op", "add_layer"}, {"page", 1}, {"kind", "fill"}, {"id", "fl"}},
                                                             Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"}, {"id", "adj"}, {"adjust", Json{{"kind", "invert"}}}},
                                                             Json{{"op", "smart_place"}, {"page", 1}, {"data", core::b64encode(fixture("rich_rgb8.pcraft"))},
                                                                  {"name", "x.pcraft"}}}));
        const fs::path written = at("genko_book.pcraft");
        genko::test::write_bytes(QString::fromStdString(core::path_to_utf8(written)), formats::pcraft_bytes(book, 0));
        files << QString::fromStdString(core::path_to_utf8(written));
        for (const QString& f : files) QVERIFY2(QFileInfo::exists(f), qPrintable(f));
        QProcess process;
        process.start(tool, QStringList{QStringLiteral("check")} + files);
        QVERIFY(process.waitForFinished(600000));
        const QByteArray out = process.readAllStandardOutput();
        QVERIFY2(process.exitCode() == 0, out.constData());
        int lines = 0, compared = 0;
        for (const QByteArray& line : out.split('\n')) {
            if (line.trimmed().isEmpty()) continue;
            const Json v = Json::parse(line.toStdString());
            QVERIFY2(v["ok"].get<bool>(), line.constData());
            ++lines;
            // (a fixture's: its pixel layers' samples as upstream read the fixture's — the same layer by name, the first of it;
            // a text's pixels are its picture, kept as they are; what Genko draws again — shapes, smart objects — not compared)
            const std::string file = fs::path(v["file"].get<std::string>()).filename().string();
            if (!file.starts_with("again_")) continue;
            const std::string fixture_name = file.substr(6);
            const Json& up = upstream_read(fixture_name);
            std::map<std::string, const Json*> theirs;
            std::vector<std::pair<const Json*, bool>> flat;
            flattened(up["layers"], true, flat);
            for (const auto& [l, shown] : flat) theirs.emplace((*l)["name"].get<std::string>(), l);
            std::vector<std::pair<const Json*, bool>> mine;
            flattened(v["layers"], true, mine);
            const double step = step_of(up["depth"].get<std::string>());
            for (const auto& [l, shown] : mine) {
                const auto found = theirs.find((*l)["name"].get<std::string>());
                if (found == theirs.end() || !l->contains("pixels") || !found->second->contains("pixels")) continue;
                if ((*l)["kind"] != Json("raster") || ((*found->second)["kind"] != Json("raster") && (*found->second)["kind"] != Json("text"))) continue;
                ++compared;
                const Json& a = (*l)["pixels"];
                const Json& b = (*found->second)["pixels"];
                QCOMPARE(a.size(), b.size());
                for (std::size_t i = 0; i < a.size(); ++i) {
                    const Json& pa = a[i][2];
                    const Json& pb = b[i][2];
                    if (pa.back().get<double>() == 0.0 && pb.back().get<double>() == 0.0) continue;
                    for (std::size_t c = 0; c < pa.size(); ++c) {
                        QVERIFY2(std::abs(pa[c].get<double>() - pb[c].get<double>()) <= step,
                                 qPrintable(QString::fromStdString(file + " " + (*l)["name"].get<std::string>() + " " + a[i].dump() + " / " + b[i].dump())));
                    }
                }
            }
        }
        QCOMPARE(lines, static_cast<int>(files.size()));
        QVERIFY(compared >= static_cast<int>(std::size(kFixtures)));
    }
};

QTEST_GUILESS_MAIN(TestContractPcraft)
#include "test_contract_pcraft.moc"
