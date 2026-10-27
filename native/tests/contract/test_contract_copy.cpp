// PC4 F-c (SPEC.md FILE-01: 書き出し形式…, クイック書き出し, レイヤーを書き出し, レイヤーからファイル…, `genko copy`) as a person or an
// agent uses them (this build's own contract): a page written at a scale, at a size asked exactly, cut to its finished
// size; a layer alone cut to what it holds, over white without its transparency; JPEG's and WebP's quality; the metadata's
// three choices (none, the copyright and contact, all — the page's EXIF upright); the colours of a CMYK page the same in
// every format and every way out; a picture too large refused before it is drawn; the estimate (exact, and from a proxy)
// against the file; クイック書き出し's places (ask, beside the book, the book's export folder) and formats; レイヤーからファイル…'s
// formats, prefix and hidden layers — its default files as before, byte for byte —; `genko copy` to a file and to a folder,
// --pages, --layer, --estimate, --web and its refusals, and `genko export --format layers` with its new choices.

#include <QtTest>

#include <QElapsedTimer>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "core/command_bus.hpp"
#include "core/exif.hpp"
#include "core/model.hpp"
#include "render/codecs/metadata.hpp"
#include "core/file_info.hpp"
#include "core/json.hpp"
#include "core/paths.hpp"
#include "formats/copy.hpp"
#include "formats/export.hpp"
#include "formats/file_xmp.hpp"
#include "formats/open_file.hpp"
#include "formats/page_picture.hpp"
#include "formats/pillow_save.hpp"
#include "formats/web_export.hpp"
#include "render/codecs/codecs.hpp"
#include "render/image.hpp"
#include "render/ops_registry.hpp"
#include "storage/fsutil.hpp"
#include "storage/lock.hpp"
#include "storage/transaction.hpp"
#include "testsupport.hpp"

namespace core = genko::core;
namespace render = genko::render;
namespace formats = genko::formats;
namespace codecs = genko::render::codecs;
namespace storage = genko::storage;
namespace fs = std::filesystem;
using codecs::CodecImage;
using codecs::Layout;
using codecs::Sample;
using core::Json;

namespace {

fs::path path_of(const QString& text) { return core::path_from_utf8(text.toStdString()); }

void write_book(const core::Document& doc, const fs::path& dir) {
    storage::ProjectLock lock(dir, "human:test");
    lock.try_acquire();
    storage::SaveRequest request;
    request.actor = "human:test";
    request.base_revision = doc.revision;
    request.ops = Json::array();
    storage::Saver(lock).save(doc, request);
}

// A picture book of 120 × 90 at 150 dpi: a red square (20..60, 30..70) on a layer over the white paper.
core::Document square_book() {
    CodecImage image = CodecImage::blank(120, 90, Layout::Rgba, Sample::U8);
    for (int y = 30; y < 70; ++y)
        for (int x = 20; x < 60; ++x) {
            char* p = image.data().data() + (std::size_t(y) * 120 + x) * 4;
            p[0] = static_cast<char>(255);
            p[3] = static_cast<char>(255);
        }
    image.meta.dpi = std::make_pair(150.0, 150.0);
    return formats::picture_book(std::move(image), "square").doc;
}

std::string raster_of(const core::Document& doc) {
    for (const core::Layer& l : doc.page(0).layers)
        if (l.kind == core::LayerKind::Raster) return l.id;
    return {};
}

CodecImage read(const fs::path& path) { return codecs::decode(storage::read_file(path)); }

std::array<int, 4> px(const CodecImage& image, std::uint32_t x, std::uint32_t y) {
    const CodecImage rgba = image.convert(Layout::Rgba, Sample::U8);
    const auto* d = reinterpret_cast<const unsigned char*>(rgba.data().data()) + (std::size_t(y) * rgba.width() + x) * 4;
    return {d[0], d[1], d[2], d[3]};
}

std::uint64_t size_of(const fs::path& path) { return fs::file_size(path); }

// A picture book whose layer is a red square (20..60, 30..70) with a soft top band (its first 10 rows at alpha 128) and a
// hole (10 × 10 clear at its left, rows 15..25): what a JPEG of the layer alone shows over white.
core::Document soft_book() {
    CodecImage image = CodecImage::blank(120, 90, Layout::Rgba, Sample::U8);
    for (int y = 30; y < 70; ++y)
        for (int x = 20; x < 60; ++x) {
            if (y >= 45 && y < 55 && x < 30) continue;  // (the hole)
            char* p = image.data().data() + (std::size_t(y) * 120 + x) * 4;
            p[0] = static_cast<char>(255);
            p[3] = static_cast<char>(y < 40 ? 128 : 255);
        }
    image.meta.dpi = std::make_pair(150.0, 150.0);
    return formats::picture_book(std::move(image), "soft").doc;
}

// The image resources' ids of a PSD (after its header and colour mode data).
std::vector<int> psd_resources(const std::string& b) {
    const auto be32 = [&](std::size_t at) {
        return (std::uint32_t(static_cast<unsigned char>(b[at])) << 24) | (std::uint32_t(static_cast<unsigned char>(b[at + 1])) << 16) |
               (std::uint32_t(static_cast<unsigned char>(b[at + 2])) << 8) | std::uint32_t(static_cast<unsigned char>(b[at + 3]));
    };
    std::vector<int> ids;
    if (b.size() < 30) return ids;
    std::size_t at = 26;
    at += 4 + be32(at);  // (colour mode data)
    const std::size_t end = at + 4 + be32(at);
    at += 4;
    while (at + 12 <= end && b.compare(at, 4, "8BIM") == 0) {
        const int id = (static_cast<unsigned char>(b[at + 4]) << 8) | static_cast<unsigned char>(b[at + 5]);
        std::size_t name = 1 + static_cast<unsigned char>(b[at + 6]);
        name += name % 2;
        const std::uint32_t size = be32(at + 6 + name);
        ids.push_back(id);
        at += 6 + name + 4 + size + size % 2;
    }
    return ids;
}

// A little-endian EXIF of a camera: IFD0 (turned 90° right; the Exif IFD) → the Exif IFD (PixelXDimension 4000 as a LONG,
// PixelYDimension 3000 as a SHORT) → IFD1, the thumbnail of the shot before it was edited (JPEGInterchangeFormat).
std::string camera_exif() {
    std::string b("II*\0\x08\0\0\0", 8);
    const auto u16 = [&](std::uint16_t v) { b += static_cast<char>(v & 0xFF); b += static_cast<char>(v >> 8); };
    const auto u32 = [&](std::uint32_t v) {
        for (int k = 0; k < 4; ++k) b += static_cast<char>((v >> (8 * k)) & 0xFF);
    };
    const auto entry = [&](std::uint16_t tag, std::uint16_t type, std::uint32_t value) {
        u16(tag);
        u16(type);
        u32(1);
        if (type == 3) {
            u16(static_cast<std::uint16_t>(value));
            u16(0);
        } else {
            u32(value);
        }
    };
    const std::string thumbnail = std::string("\xFF\xD8", 2) + "THUMBNAIL-OF-THE-SHOT-BEFORE-IT-WAS-CUT" + std::string("\xFF\xD9", 2);
    u16(2);  // IFD0 at 8
    entry(0x0112, 3, 6);
    entry(0x8769, 4, 38);
    u32(68);  // → IFD1
    u16(2);   // the Exif IFD at 38
    entry(0xA002, 4, 4000);
    entry(0xA003, 3, 3000);
    u32(0);
    u16(2);  // IFD1 at 68
    entry(0x0201, 4, 98);
    entry(0x0202, 4, static_cast<std::uint32_t>(thumbnail.size()));
    u32(0);
    b += thumbnail;  // at 98
    return b;
}

// What an EXIF says of the size and of what follows IFD0.
std::tuple<std::uint32_t, std::uint32_t, std::size_t> exif_size(const std::string& exif) {
    const core::exif::Tiff t(exif);
    const std::size_t ifd0 = t.first_ifd();
    const std::size_t sub = t.whole(*t.entry(ifd0, 0x8769)).value_or(0);
    return {t.whole(*t.entry(sub, 0xA002)).value_or(0), t.whole(*t.entry(sub, 0xA003)).value_or(0), t.next_ifd(ifd0)};
}

QString q(const fs::path& path) { return QString::fromStdString(core::path_to_utf8(path)); }

}  // namespace

class CopyContract : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { qputenv("QT_QPA_PLATFORM", "offscreen"); }

    // 書き出し形式's size: 100 % the page as コピーを保存… writes it (the same bytes); 50 % and 200 % the page drawn at that
    // resolution, its resolution in the file the one that keeps its size on paper; a width asked exactly (the height in
    // proportion), a height alone; 仕上がりで切る smaller than the paper.
    void aPageAtAScaleOrASize() {
        QTemporaryDir tmp;
        const core::Document doc = square_book();
        const fs::path dir = path_of(tmp.path());
        formats::write_copy(doc, 0, dir / "copy.png");
        formats::CopyOptions same;
        same.metadata = std::nullopt;
        formats::write_copy(doc, 0, dir / "same.png", same);
        QCOMPARE(storage::read_file(dir / "copy.png"), storage::read_file(dir / "same.png"));
        const CodecImage full = read(dir / "copy.png");
        QCOMPARE(full.width(), 120u);
        formats::CopyOptions half;
        half.scale = 50;
        const formats::CopyResult h = formats::write_copy(doc, 0, dir / "half.png", half);
        QCOMPARE(h.width, 60u);
        QCOMPARE(h.height, 45u);
        const CodecImage halfback = read(dir / "half.png");
        QCOMPARE(halfback.width(), 60u);
        QVERIFY(halfback.meta.dpi && std::abs(halfback.meta.dpi->first - 75.0) < 0.5);
        QCOMPARE(formats::copy_size(doc, 0, half).drawn_dpi, 75);
        formats::CopyOptions twice;
        twice.scale = 200;
        QCOMPARE(formats::write_copy(doc, 0, dir / "twice.png", twice).width, 240u);
        formats::CopyOptions wide;
        wide.width = 100;
        const formats::CopyResult w = formats::write_copy(doc, 0, dir / "wide.png", wide);
        QCOMPARE(w.width, 100u);
        QCOMPARE(w.height, 75u);
        formats::CopyOptions tall;
        tall.height = 30;
        QCOMPARE(formats::write_copy(doc, 0, dir / "tall.png", tall).width, 40u);
        // (a red pixel stays red in the middle of the square at every size)
        QCOMPARE(px(read(dir / "wide.png"), 33, 41)[0], 255);
        formats::CopyOptions out;
        out.scale = 0.5;
        QVERIFY_THROWS_EXCEPTION(core::Error, formats::write_copy(doc, 0, dir / "no.png", out));
        QVERIFY(!fs::exists(dir / "no.png"));
    }

    // A layer alone: drawn over nothing, cut to what it holds (the square, 40 × 40), its transparency kept — or over white
    // —; a JPEG of it (no alpha) over white; a folder or a layer of nothing refused.
    void aLayerAloneCutToWhatItHolds() {
        QTemporaryDir tmp;
        const core::Document doc = square_book();
        const fs::path dir = path_of(tmp.path());
        formats::CopyOptions layer;
        layer.layer = raster_of(doc);
        QVERIFY(!layer.layer->empty());
        const formats::CopyResult r = formats::write_copy(doc, 0, dir / "layer.png", layer);
        QCOMPARE(r.width, 40u);
        QCOMPARE(r.height, 40u);
        QCOMPARE(px(read(dir / "layer.png"), 0, 0), (std::array<int, 4>{255, 0, 0, 255}));
        formats::CopyOptions twice = layer;
        twice.scale = 200;
        // (drawn at twice the resolution: the square's edge as the page draws a picture larger — a pixel of soft edge at
        // most on each side)
        const std::uint32_t wide = formats::write_copy(doc, 0, dir / "layer2.png", twice).width;
        QVERIFY2(wide >= 80 && wide <= 82, qPrintable(QString::number(wide)));
        // (its estimate from a proxy: the layer's own size, not its page's — the layer at 1 % gone, all of it scaled; a
        // proxy about half the page's size, its soft edge a pixel or so)
        const formats::CopyEstimate guess = formats::estimate_copy(doc, 0, "png", layer, 64);
        QVERIFY(!guess.exact);
        QVERIFY2(guess.width >= 34 && guess.width <= 46 && guess.bytes > 0, qPrintable(QStringLiteral("%1 %2").arg(guess.width).arg(guess.bytes)));
        formats::CopyOptions sized = layer;
        sized.width = 10;
        QCOMPARE(formats::write_copy(doc, 0, dir / "layer10.png", sized).height, 10u);
        // (an empty layer: nothing written)
        core::Document blank = doc;
        core::Layer copy = blank.page(0).layers.back();
        copy.id = "empty-layer";
        copy.raster_png.reset();
        copy.color_raster.reset();
        copy.ink_raster.reset();
        blank.edit_page(0).layers.push_back(copy);
        formats::CopyOptions none;
        none.layer = "empty-layer";
        try {
            formats::write_copy(blank, 0, dir / "empty.png", none);
            QFAIL("an empty layer was written");
        } catch (const core::Error& error) {
            QCOMPARE(std::string(error.what()), std::string("the layer has nothing to write"));
        }
        none.layer = "no-such";
        QVERIFY_THROWS_EXCEPTION(core::Error, formats::write_copy(doc, 0, dir / "none.png", none));
    }

    // The quality: a JPEG at 10 % smaller than at 95 %; a WebP given a quality lossy (VP8 ), without one lossless (VP8L).
    void theQualityAsked() {
        QTemporaryDir tmp;
        const core::Document doc = square_book();
        const fs::path dir = path_of(tmp.path());
        formats::CopyOptions lo, hi;
        lo.quality_percent = 10;
        hi.quality_percent = 95;
        QVERIFY(formats::write_copy(doc, 0, dir / "lo.jpg", lo).bytes < formats::write_copy(doc, 0, dir / "hi.jpg", hi).bytes);
        formats::write_copy(doc, 0, dir / "lossy.webp", lo);
        formats::write_copy(doc, 0, dir / "lossless.webp");
        QVERIFY(storage::read_file(dir / "lossy.webp").find("VP8 ") != std::string::npos);
        QVERIFY(storage::read_file(dir / "lossless.webp").find("VP8L") != std::string::npos);
    }

    // The metadata's three: none (no packet), the copyright and contact (the copyright and the author, not the description),
    // all (the File Info, and the page's EXIF — of a picture opened with one — upright); コピーを保存… as before (the File Info).
    void theMetadataChosen() {
        QTemporaryDir tmp;
        const fs::path dir = path_of(tmp.path());
        // (a JPEG with an EXIF that says it is turned: opened upright, its EXIF kept)
        CodecImage photo = CodecImage::blank(30, 20, Layout::Rgb, Sample::U8);
        std::string exif("II*\0\x08\0\0\0\x01\0\x12\x01\x03\0\x01\0\0\0\x06\0\0\0\0\0\0\0", 26);
        photo.meta.exif = exif;
        const std::string jpeg = codecs::encode(photo, codecs::Format::Jpeg);
        core::Document doc = formats::open_file(jpeg, "photo.jpg").doc;
        core::FileInfo info;
        info.copyright = "© Someone";
        info.author = "An Author";
        info.description = "a private note";
        doc.file_info = info;
        formats::CopyOptions none, copyright, all;
        none.metadata = formats::Metadata::None;
        copyright.metadata = formats::Metadata::Copyright;
        all.metadata = formats::Metadata::All;
        formats::write_copy(doc, 0, dir / "none.jpg", none);
        formats::write_copy(doc, 0, dir / "copyright.jpg", copyright);
        formats::write_copy(doc, 0, dir / "all.jpg", all);
        formats::write_copy(doc, 0, dir / "copy.jpg");
        QVERIFY(!read(dir / "none.jpg").meta.xmp);
        QVERIFY(!read(dir / "none.jpg").meta.exif);
        const std::string c = read(dir / "copyright.jpg").meta.xmp.value_or("");
        QVERIFY2(c.find("Someone") != std::string::npos && c.find("An Author") != std::string::npos, c.c_str());
        QVERIFY(c.find("private note") == std::string::npos);
        QVERIFY(!read(dir / "copyright.jpg").meta.exif);
        const CodecImage a = read(dir / "all.jpg");
        QVERIFY(a.meta.xmp && a.meta.xmp->find("private note") != std::string::npos);
        QVERIFY(a.meta.exif.has_value());
        QCOMPARE(codecs::exif_orientation(*a.meta.exif), 1);
        QVERIFY(read(dir / "copy.jpg").meta.xmp->find("private note") != std::string::npos);
        QVERIFY(!read(dir / "copy.jpg").meta.exif);
    }

    // (PC4 F-c's review P2-1) A layer in a format without alpha — JPEG — over white by every way in: its hole white, its
    // soft edge mixed with white; never its alpha merely dropped. The same layer's PNG keeps its alpha.
    void aLayerInJpegIsOverWhite() {
        QTemporaryDir tmp;
        const fs::path dir = path_of(tmp.path());
        const core::Document doc = soft_book();
        formats::CopyOptions layer;
        layer.layer = raster_of(doc);
        layer.quality_percent = 100;
        QVERIFY(layer.transparency);  // (asked to keep it: a JPEG cannot)
        const formats::CopyResult r = formats::write_copy(doc, 0, dir / "layer.jpg", layer);
        QCOMPARE(r.width, 40u);
        const CodecImage jpeg = read(dir / "layer.jpg");
        const auto near = [](std::array<int, 4> c, std::array<int, 3> want, int tolerance) {
            for (int k = 0; k < 3; ++k) {
                if (std::abs(c[k] - want[k]) > tolerance) return false;
            }
            return true;
        };
        const auto hole = px(jpeg, 4, 20), edge = px(jpeg, 30, 4), inside = px(jpeg, 30, 30);
        QVERIFY2(near(hole, {255, 255, 255}, 8), qPrintable(QStringLiteral("%1 %2 %3").arg(hole[0]).arg(hole[1]).arg(hole[2])));
        QVERIFY2(near(edge, {255, 128, 128}, 12), qPrintable(QStringLiteral("%1 %2 %3").arg(edge[0]).arg(edge[1]).arg(edge[2])));
        QVERIFY2(near(inside, {255, 0, 0}, 8), qPrintable(QStringLiteral("%1 %2 %3").arg(inside[0]).arg(inside[1]).arg(inside[2])));
        // (the PNG keeps the hole clear and the edge half)
        const CodecImage png = read(formats::write_copy(doc, 0, dir / "layer.png", layer).file);
        QCOMPARE(px(png, 4, 20)[3], 0);
        QVERIFY(std::abs(px(png, 30, 4)[3] - 128) <= 1);
        // (`genko copy --layer` to a JPEG: the same)
        core::Document saved = doc;
        const fs::path book = dir / "soft.genko";
        write_book(saved, book);
        const auto run = genko::test::run_genko({QStringLiteral("copy"), q(book), q(dir / "cli.jpg"), QStringLiteral("--layer"), QString::fromStdString(*layer.layer),
                                                 QStringLiteral("--quality"), QStringLiteral("100%")});
        QVERIFY2(run.exit_code == 0, qPrintable(run.described()));
        const CodecImage cli = read(dir / "cli.jpg");
        QVERIFY(near(px(cli, 4, 20), {255, 255, 255}, 8));
        QVERIFY(near(px(cli, 30, 4), {255, 128, 128}, 12));
    }

    // (PC4 F-c's review P2-2) A PSD's, a PSB's and a .pcraft's copy with the metadata chosen: none — no XMP (resource 1060),
    // no File Info, no EXIF —; the copyright and contact — the copyright and the author, not the private description —;
    // none chosen as コピーを保存… writes it (the File Info whole). A layer's PSD alike.
    void theMetadataOfAPsdOrAPcraft() {
        QTemporaryDir tmp;
        const fs::path dir = path_of(tmp.path());
        CodecImage photo = CodecImage::blank(30, 20, Layout::Rgb, Sample::U8);
        photo.meta.exif = camera_exif();
        core::Document doc = formats::open_file(codecs::encode(photo, codecs::Format::Jpeg), "photo.jpg").doc;
        QVERIFY(doc.page(0).native_source && !doc.page(0).native_source->exif.empty());
        core::FileInfo info;
        info.copyright = "© Someone";
        info.author = "An Author";
        info.description = "a private note";
        doc.file_info = info;
        formats::CopyOptions none, copyright, layer_none;
        none.metadata = formats::Metadata::None;
        copyright.metadata = formats::Metadata::Copyright;
        layer_none = none;
        layer_none.layer = raster_of(doc);
        QVERIFY(!layer_none.layer->empty());
        for (const char* ext : {"psd", "psb"}) {
            const std::string e(ext);
            formats::write_copy(doc, 0, dir / ("none." + e), none);
            formats::write_copy(doc, 0, dir / ("copyright." + e), copyright);
            formats::write_copy(doc, 0, dir / ("default." + e));
            const std::vector<int> ids = psd_resources(storage::read_file(dir / ("none." + e)));
            QVERIFY2(!ids.empty() && std::find(ids.begin(), ids.end(), 1060) == ids.end(), ext);
            const std::string c = storage::read_file(dir / ("copyright." + e));
            QVERIFY2(c.find("Someone") != std::string::npos && c.find("An Author") != std::string::npos, ext);
            QVERIFY2(c.find("private note") == std::string::npos, ext);
            QVERIFY2(storage::read_file(dir / ("default." + e)).find("private note") != std::string::npos, ext);
        }
        formats::CopyOptions flat = none;
        flat.layers = false;
        formats::write_copy(doc, 0, dir / "flat.psd", flat);
        formats::write_copy(doc, 0, dir / "layer.psd", layer_none);
        for (const char* name : {"flat.psd", "layer.psd"}) {
            const std::string b = storage::read_file(dir / name);
            QVERIFY2(b.find("private note") == std::string::npos && b.find("Someone") == std::string::npos, name);
        }
        // (a .pcraft read back: its File Info and its EXIF as chosen)
        formats::write_copy(doc, 0, dir / "none.pcraft", none);
        formats::write_copy(doc, 0, dir / "copyright.pcraft", copyright);
        formats::write_copy(doc, 0, dir / "default.pcraft");
        const core::Document n = formats::open_file(storage::read_file(dir / "none.pcraft"), "none.pcraft").doc;
        QVERIFY(!n.file_info || n.file_info->empty());
        QVERIFY(!n.page(0).native_source || n.page(0).native_source->exif.empty());
        const core::Document c = formats::open_file(storage::read_file(dir / "copyright.pcraft"), "copyright.pcraft").doc;
        QVERIFY(c.file_info && c.file_info->copyright == "© Someone" && c.file_info->description.empty());
        QVERIFY(!c.page(0).native_source || c.page(0).native_source->exif.empty());
        const core::Document d = formats::open_file(storage::read_file(dir / "default.pcraft"), "default.pcraft").doc;
        QVERIFY(d.file_info && d.file_info->description == "a private note");
        QVERIFY(d.page(0).native_source && !d.page(0).native_source->exif.empty());
        // (the book itself unchanged)
        QCOMPARE(doc.file_info->description, std::string("a private note"));
        QVERIFY(!doc.page(0).native_source->exif.empty());
        // (`genko copy … --metadata none` to a PSD: no XMP)
        const fs::path book = dir / "photo.genko";
        write_book(doc, book);
        const auto run = genko::test::run_genko({QStringLiteral("copy"), q(book), q(dir / "cli.psd"), QStringLiteral("--metadata"), QStringLiteral("none")});
        QVERIFY2(run.exit_code == 0, qPrintable(run.described()));
        const std::vector<int> cli = psd_resources(storage::read_file(dir / "cli.psd"));
        QVERIFY(!cli.empty() && std::find(cli.begin(), cli.end(), 1060) == cli.end());
    }

    // (PC4 F-c's review P2-3) All the metadata: the page's EXIF without the camera's thumbnail (IFD1: the shot before it was
    // edited or cut), the sizes it says the written picture's — at another scale too —, upright; the camera's own kept. The
    // same in a Save for Web or クイック書き出し JPEG; none in their PNG (JPEG alone, as Photoshop's Save for Web).
    void allTheMetadataLeavesTheThumbnailOut() {
        QTemporaryDir tmp;
        const fs::path dir = path_of(tmp.path());
        CodecImage photo = CodecImage::blank(30, 20, Layout::Rgb, Sample::U8);
        photo.meta.exif = camera_exif();
        QVERIFY(photo.meta.exif->find("THUMBNAIL") != std::string::npos);
        const core::Document doc = formats::open_file(codecs::encode(photo, codecs::Format::Jpeg), "photo.jpg").doc;
        formats::CopyOptions all;
        all.metadata = formats::Metadata::All;
        const auto check = [&](const fs::path& path) {
            const CodecImage back = read(path);
            if (!back.meta.exif) return QStringLiteral("no EXIF");
            const std::string& exif = *back.meta.exif;
            if (exif.find("THUMBNAIL") != std::string::npos) return QStringLiteral("the thumbnail");
            const auto [w, h, next] = exif_size(exif);
            if (w != back.width() || h != back.height()) return QStringLiteral("size %1×%2 for %3×%4").arg(w).arg(h).arg(back.width()).arg(back.height());
            if (next != 0) return QStringLiteral("an IFD after IFD0");
            if (codecs::exif_orientation(exif) != 1) return QStringLiteral("not upright");
            return QString();
        };
        formats::write_copy(doc, 0, dir / "all.jpg", all);
        QCOMPARE(check(dir / "all.jpg"), QString());
        QCOMPARE(read(dir / "all.jpg").width(), 20u);  // (turned upright: 20 × 30)
        formats::CopyOptions twice = all;
        twice.scale = 200;
        formats::write_copy(doc, 0, dir / "twice.webp", twice);
        QCOMPARE(check(dir / "twice.webp"), QString());
        QCOMPARE(std::get<0>(exif_size(*read(dir / "twice.webp").meta.exif)), 40u);
        formats::ExportPreferences prefs;
        prefs.metadata = formats::Metadata::All;
        prefs.format = "jpg";
        formats::quick_export(doc, 0, dir / "quick.jpg", prefs);
        QCOMPARE(check(dir / "quick.jpg"), QString());
        prefs.format = "png";
        formats::quick_export(doc, 0, dir / "quick.png", prefs);
        QVERIFY(!read(dir / "quick.png").meta.exif);
        formats::save_for_web(doc, 0, formats::web_settings_from_json(Json{{"format", "jpeg"}, {"metadata", "all"}}), {}, dir / "web.jpg");
        QCOMPARE(check(dir / "web.jpg"), QString());
        formats::save_for_web(doc, 0, formats::web_settings_from_json(Json{{"format", "jpeg"}, {"metadata", "copyright"}}), {}, dir / "web-c.jpg");
        QVERIFY(!read(dir / "web-c.jpg").meta.exif);
        // (the block itself: IFD1 and the thumbnail cut off its end, the Exif IFD's SHORT kept a SHORT)
        const std::string cleaned = codecs::exif_for_copy(camera_exif(), 300, 200);
        QCOMPARE(cleaned.size(), std::size_t(68));
        QCOMPARE(exif_size(cleaned), (std::tuple<std::uint32_t, std::uint32_t, std::size_t>{300, 200, 0}));
        const core::exif::Tiff t(cleaned);
        QCOMPARE(t.entry(38, 0xA003)->type, std::uint16_t(3));
        // (a size past a SHORT's: made a LONG in its own field)
        const std::string wide = codecs::exif_for_copy(camera_exif(), 300, 70000);
        QCOMPARE(std::get<1>(exif_size(wide)), 70000u);
        QCOMPARE(codecs::exif_for_copy(std::string("not a TIFF"), 1, 1), std::string("not a TIFF"));
    }

    // (PC4 F-c's review P3-16) コピーを保存…'s default bytes are F-b's: the page drawn at its resolution with the book's XMP,
    // encoded as F-b encoded it (the codecs at Photoshop's quality 10; the exports' TIFF writer) — a recipe of its own, not
    // the new code's.
    void theCopysDefaultBytesAreAsBefore() {
        QTemporaryDir tmp;
        const fs::path dir = path_of(tmp.path());
        core::Document doc = square_book();
        core::FileInfo info;
        info.copyright = "© Someone";
        info.description = "a note";
        doc.file_info = info;
        const formats::PagePicture picture = formats::page_picture(doc.page(0), doc, 150);
        CodecImage image = picture.image;
        image.meta.xmp = formats::book_xmp(doc);
        QVERIFY(image.meta.xmp.has_value());
        codecs::EncodeOptions encode;
        encode.jpeg_quality = formats::jpeg_quality_of(10);
        QCOMPARE(formats::plan_copy(doc, 0, dir / "a.png").bytes, codecs::encode(image, codecs::Format::Png, encode));
        QCOMPARE(formats::plan_copy(doc, 0, dir / "a.jpg").bytes, codecs::encode(image, codecs::Format::Jpeg, encode));
        QCOMPARE(formats::plan_copy(doc, 0, dir / "a.webp").bytes, codecs::encode(image, codecs::Format::WebP, encode));
        const CodecImage rgb = picture.image.convert(Layout::Rgb, Sample::U8);
        formats::TiffSave save{"tiff_lzw", std::array<double, 2>{150.0, 150.0}, picture.image.icc.value_or(std::string()), *image.meta.xmp};
        const std::string tiff = formats::tiff_bytes(
            render::Image::frombytes("RGB", render::Size{static_cast<int>(rgb.width()), static_cast<int>(rgb.height())}, rgb.data()), save);
        QCOMPARE(formats::plan_copy(doc, 0, dir / "a.tif").bytes, tiff);
    }

    // (the coordinator's P3-8) The places no way out writes to: the book itself and anything in its folder, a file a smart
    // object links (its path, and where it is found), a picture a book was opened from — beside them is fine —; レイヤーからファイル…
    // refuses a linked file among its files and writes none; クイック書き出し's place never a file that is there (name-2,
    // name-3 …); `genko copy` and `genko export --format layers` into the book are a usage error, nothing written.
    void noWayOutWritesOverTheBook() {
        QTemporaryDir tmp;
        const fs::path dir = path_of(tmp.path());
        core::Document doc = square_book();
        const fs::path book = dir / "本.genko";
        write_book(doc, book);
        const fs::path linked = dir / "linked.png";
        storage::write_atomic(linked, "a linked picture");
        auto contents = std::make_shared<core::SmartContents>();
        contents->link = Json{{"path", core::path_to_utf8(linked)}};
        doc.smart_contents["sc-1"] = contents;
        const formats::WriteGuard guard = formats::write_guard(doc, book, {dir / "opened.jpg"});
        const auto said = [&](const fs::path& dest) -> std::string {
            try {
                formats::check_target(guard, dest);
                return "";
            } catch (const core::Error& error) {
                if (std::string(error.code()) != "refused_target") return "the code " + std::string(error.code());
                return error.what();
            }
        };
        QVERIFY(said(book).starts_with("cannot write over the book"));
        QVERIFY(said(book / "x.png").starts_with("cannot write inside the book's folder"));
        QVERIFY(said(book / "assets" / "deep" / "x.png").starts_with("cannot write inside the book's folder"));
        QVERIFY(said(book / ".." / "本.genko" / "y.png").starts_with("cannot write inside the book's folder"));
        QVERIFY(said(linked).starts_with("cannot write over a file the book links"));
        QVERIFY(said(dir / "opened.jpg").starts_with("cannot write over the picture a book was opened from"));
        QCOMPARE(said(dir / "x.png"), std::string());
        QCOMPARE(said(dir / "本.genko-out" / "x.png"), std::string());
        QCOMPARE(said(dir), std::string());
        // (レイヤーからファイル… with a linked file among its files: none written)
        formats::LayerFiles files;
        int checked = 0;
        files.check = [&](const fs::path& file) {
            ++checked;
            if (checked == 2) formats::check_target(formats::WriteGuard{{}, {file}, {}}, file);
        };
        const core::Document pages = core::new_episode("層", core::Num(1), 2, core::PageSpec::a4_mono());
        QVERIFY_THROWS_EXCEPTION(core::Error, formats::export_layers(pages, dir / "layers", 72, "paper", files));
        QCOMPARE(checked, 2);
        std::size_t left = 0;
        if (fs::exists(dir / "layers")) {
            for (const auto& entry : fs::recursive_directory_iterator(dir / "layers")) left += entry.is_regular_file() ? 1 : 0;
        }
        QCOMPARE(left, std::size_t(0));
        // (クイック書き出し: the next free name)
        formats::ExportPreferences prefs;
        prefs.location = formats::QuickLocation::SameFolder;
        const fs::path first = *formats::quick_export_place(prefs, book, doc.page(0));
        QCOMPARE(first, dir / "本_p001.png");
        storage::write_atomic(first, "one");
        QCOMPARE(*formats::quick_export_place(prefs, book, doc.page(0)), dir / "本_p001-2.png");
        storage::write_atomic(dir / "本_p001-2.png", "two");
        QCOMPARE(*formats::quick_export_place(prefs, book, doc.page(0)), dir / "本_p001-3.png");
        // (the command line: a usage error, nothing written)
        const auto copy = [&](const QStringList& more) { return genko::test::run_genko(QStringList{QStringLiteral("copy"), q(book)} + more); };
        for (const fs::path& dest : {book / "x.png", book / "folder"}) {
            const auto r = copy({q(dest)});
            QVERIFY2(r.exit_code == 2, qPrintable(r.described()));
            QVERIFY(!fs::exists(dest));
        }
        QCOMPARE(copy({q(book)}).exit_code, 2);
        auto r = genko::test::run_genko({QStringLiteral("export"), q(book), q(book / "layers"), QStringLiteral("--format"), QStringLiteral("layers")});
        QVERIFY2(r.exit_code == 2, qPrintable(r.described()));
        QVERIFY(!fs::exists(book / "layers"));
        r = copy({q(dir / "beside.png")});
        QVERIFY2(r.exit_code == 0, qPrintable(r.described()));
        // (a file there is written over by the command line, as before)
        r = copy({q(dir / "beside.png")});
        QVERIFY2(r.exit_code == 0, qPrintable(r.described()));
    }

    // A CMYK page's colours the same in every format and every way out (Little CMS from its profile to sRGB, never a
    // format's plain formula): コピーを保存… as PNG, WebP, GIF, QOI, TGA; sRGB asked (a JPEG then RGB); クイック書き出し; Save
    // for Web's PNG-24 and JPEG.
    void aCmykPageIsTheSameColourEverywhere() {
        QTemporaryDir tmp;
        const fs::path dir = path_of(tmp.path());
        formats::NewImage made;
        made.title = "cmyk";
        made.width = 40;
        made.height = 30;
        made.dpi = 100;
        made.mode = "cmyk";
        made.background = "#3366cc";
        const core::Document doc = formats::new_image(made).doc;
        formats::write_copy(doc, 0, dir / "base.png");
        const auto base = px(read(dir / "base.png"), 20, 15);
        const auto near = [&](const fs::path& path, int tolerance) {
            const auto c = px(read(path), 20, 15);
            for (int k = 0; k < 3; ++k) {
                if (std::abs(c[k] - base[k]) > tolerance) return false;
            }
            return true;
        };
        for (const char* ext : {"webp", "gif", "qoi", "tga", "bmp"}) {
            formats::write_copy(doc, 0, dir / (std::string("c.") + ext));
            QVERIFY2(near(dir / (std::string("c.") + ext), 2), ext);
        }
        formats::CopyOptions srgb;
        srgb.srgb = true;
        formats::write_copy(doc, 0, dir / "srgb.png", srgb);
        QVERIFY(near(dir / "srgb.png", 1));
        formats::write_copy(doc, 0, dir / "srgb.jpg", srgb);
        QVERIFY(render::codecs::is_rgb(read(dir / "srgb.jpg").layout()));
        QVERIFY(near(dir / "srgb.jpg", 6));
        formats::write_copy(doc, 0, dir / "kept.jpg");
        QVERIFY(render::codecs::is_cmyk(read(dir / "kept.jpg").layout()));  // (sRGB not asked: the JPEG holds the CMYK)
        formats::ExportPreferences prefs;
        formats::quick_export(doc, 0, dir / "quick.png", prefs);
        QVERIFY(near(dir / "quick.png", 1));
        prefs.format = "jpg";
        prefs.jpeg_quality = 100;
        formats::quick_export(doc, 0, dir / "quick.jpg", prefs);
        QVERIFY(near(dir / "quick.jpg", 6));
        formats::save_for_web(doc, 0, formats::web_settings_from_json(Json{{"format", "png24"}}), {}, dir / "web.png");
        QVERIFY(near(dir / "web.png", 1));
    }

    // Too large is refused before anything is drawn (a 1000 % of a 600 dpi B4 page would be 5 billion pixels): quickly, and
    // in its own words.
    void tooLargeIsRefusedFirst() {
        core::PageSpec spec = core::PageSpec::a4_mono();
        spec.dpi = core::Num(600);
        const core::Document doc = core::new_episode("big", core::Num(1), 1, spec);
        formats::CopyOptions huge;
        huge.scale = 1000;
        QElapsedTimer clock;
        clock.start();
        try {
            formats::estimate_copy(doc, 0, "png", huge);
            QFAIL("a picture past the limits was made");
        } catch (const core::Error& error) {
            QCOMPARE(std::string(error.code()), std::string("image_too_large"));
            QVERIFY2(std::string(error.what()).starts_with("the picture to write is too large"), error.what());
        }
        QVERIFY2(clock.elapsed() < 5000, qPrintable(QString::number(clock.elapsed())));
        formats::WebSize big;
        big.percent = 1000;
        QVERIFY_THROWS_EXCEPTION(core::Error, formats::save_for_web(doc, 0, formats::WebSettings{}, big));
        // (larger than the format holds — a GIF of 70000 px, an icon of 300 —: in the codecs' words, before it is drawn)
        const core::Document small = square_book();
        for (const auto& [ext, side, words] : {std::tuple<const char*, std::uint32_t, const char*>{"gif", 70000, "GIF holds at most 65535×65535 pixels"},
                                               {"ico", 300, "ICO holds at most 256×256 pixels"}}) {
            formats::CopyOptions wide;
            wide.width = side;
            try {
                formats::plan_copy(small, 0, fs::path(std::string("x.") + ext), wide);
                QFAIL(ext);
            } catch (const core::Error& error) {
                QCOMPARE(std::string(error.what()), std::string(words));
            }
            // (the window's estimate refuses it as the copy does: not a proxy's estimate of a file never made)
            try {
                formats::estimate_copy(small, 0, ext, wide, 512);
                QFAIL(ext);
            } catch (const core::Error& error) {
                QCOMPARE(std::string(error.what()), std::string(words));
            }
        }
        // (Save for Web's and クイック書き出し's JPEG past 65500 px a side: before it is drawn)
        formats::WebSize jpeg_wide;
        jpeg_wide.width = 66000;
        clock.restart();
        try {
            formats::save_for_web(small, 0, formats::web_settings_from_json(Json{{"format", "jpeg"}}), jpeg_wide);
            QFAIL("a JPEG past 65500 px");
        } catch (const core::Error& error) {
            QCOMPARE(std::string(error.what()), std::string("JPEG holds at most 65500×65500 pixels"));
        }
        QVERIFY2(clock.elapsed() < 5000, qPrintable(QString::number(clock.elapsed())));
    }

    // The estimate: exact — the file's own size — for each format; from a proxy, near it (and the size of the file).
    void theEstimate() {
        QTemporaryDir tmp;
        const core::Document doc = square_book();
        const fs::path dir = path_of(tmp.path());
        formats::CopyOptions o;
        o.scale = 300;
        for (const char* ext : {"png", "jpg", "webp", "tif", "gif", "psd", "pcraft"}) {
            const formats::CopyEstimate e = formats::estimate_copy(doc, 0, ext, o);
            const formats::CopyResult r = formats::write_copy(doc, 0, dir / (std::string("e.") + ext), o);
            QVERIFY(e.exact);
            if (std::string(ext) != "psd" && std::string(ext) != "pcraft") QCOMPARE(e.bytes, size_of(dir / (std::string("e.") + ext)));
            QVERIFY2(e.bytes > 0, ext);
            (void)r;
        }
        // (from a proxy at most 512 long, as the window makes it: the file 1200 × 900)
        formats::CopyOptions big;
        big.scale = 1000;
        for (const char* ext : {"png", "jpg"}) {
            const formats::CopyEstimate guess = formats::estimate_copy(doc, 0, ext, big, 512);
            QVERIFY(!guess.exact);
            QCOMPARE(guess.width, 1200u);
            const std::uint64_t actual = formats::write_copy(doc, 0, dir / (std::string("big.") + ext), big).bytes;
            QVERIFY2(guess.bytes > actual / 3 && guess.bytes < actual * 3, qPrintable(QStringLiteral("%1: %2 / %3").arg(ext).arg(guess.bytes).arg(actual)));
        }
        // (nothing written by an estimate)
        QVERIFY(!fs::exists(fs::path("estimate.png")));
    }

    // クイック書き出し: where it writes without asking — beside the book (<book>_p001.png), in the book's export folder
    // (<book>_書き出し) —, none when asking each time or for a book never saved; a layer's name in its file; its formats
    // (PNG-24, JPEG at the quality, GIF, WebP lossless) and a layer's transparency kept.
    void quickExportPlacesAndFormats() {
        QTemporaryDir tmp;
        const fs::path dir = path_of(tmp.path());
        const core::Document doc = square_book();
        const fs::path book = dir / "漫画.genko";
        formats::ExportPreferences prefs;
        QVERIFY(!formats::quick_export_place(prefs, book, doc.page(0)));
        prefs.location = formats::QuickLocation::SameFolder;
        QVERIFY(!formats::quick_export_place(prefs, std::nullopt, doc.page(0)));
        QCOMPARE(*formats::quick_export_place(prefs, book, doc.page(0)), dir / "漫画_p001.png");
        prefs.location = formats::QuickLocation::Exports;
        prefs.format = "jpg";
        QCOMPARE(*formats::quick_export_place(prefs, book, doc.page(0)), dir / "漫画_書き出し" / "漫画_p001.jpg");
        const core::Layer* layer = nullptr;
        for (const core::Layer& l : doc.page(0).layers)
            if (l.kind == core::LayerKind::Raster) layer = &l;
        QVERIFY(layer != nullptr);
        QVERIFY(core::path_to_utf8(formats::quick_export_place(prefs, book, doc.page(0), layer)->filename()).find(layer->title) != std::string::npos);
        for (const char* format : {"png", "jpg", "gif", "webp"}) {
            prefs.format = format;
            const fs::path dest = dir / (std::string("quick.") + format);
            const formats::CopyResult r = formats::quick_export(doc, 0, dest, prefs);
            QCOMPARE(r.bytes, size_of(dest));
            QCOMPARE(read(dest).width(), 120u);
        }
        QVERIFY(storage::read_file(dir / "quick.webp").find("VP8L") != std::string::npos);
        prefs.format = "png";
        formats::quick_export(doc, 0, dir / "layer.png", prefs, layer->id);
        const CodecImage cut = read(dir / "layer.png");
        QCOMPARE(cut.width(), 40u);
        prefs.format = "bmp";
        QVERIFY_THROWS_EXCEPTION(core::Error, formats::quick_export(doc, 0, dir / "x.bmp", prefs));
    }

    // レイヤーからファイル…: the default files as before (byte for byte the export's layers); each format; the prefix; the hidden
    // layers too when asked.
    void layersToFiles() {
        QTemporaryDir tmp;
        const fs::path dir = path_of(tmp.path());
        core::Document doc = core::new_episode("層", core::Num(1), 2, core::PageSpec::a4_mono());
        const std::vector<fs::path> before = formats::export_layers(doc, dir / "before", 72);
        const std::vector<fs::path> now = formats::export_layers(doc, dir / "now", 72, "paper", formats::LayerFiles{});
        QCOMPARE(before.size(), now.size());
        QVERIFY(!before.empty());
        for (std::size_t i = 0; i < before.size(); ++i) {
            QCOMPARE(before[i].filename(), now[i].filename());
            QCOMPARE(storage::read_file(before[i]), storage::read_file(now[i]));
        }
        for (const char* format : {"jpeg", "tiff", "psd", "webp"}) {
            formats::LayerFiles files;
            files.format = format;
            files.prefix = "pre";
            std::vector<std::string> notes;
            const std::vector<fs::path> made = formats::export_layers(doc, dir / format, 72, "paper", files, &notes);
            QCOMPARE(made.size(), before.size());
            for (const fs::path& p : made) {
                QVERIFY2(core::path_to_utf8(p.filename()).starts_with("pre_"), core::path_to_utf8(p).c_str());
                QVERIFY(fs::is_regular_file(p));
            }
            if (std::string(format) != "psd") (void)read(made.front());
        }
        // (a hidden layer — the picture's — left out by default, written when asked)
        core::Document hidden = square_book();
        bool hid = false;
        for (core::Layer& l : hidden.edit_page(0).layers) {
            if (!hid && l.kind == core::LayerKind::Raster && l.visible) {
                l.visible = false;
                hid = true;
            }
        }
        QVERIFY(hid);
        formats::LayerFiles all;
        all.visible_only = false;
        const auto shown = formats::export_layers(hidden, dir / "shown", 72, "paper", formats::LayerFiles{});
        const auto every = formats::export_layers(hidden, dir / "every", 72, "paper", all);
        QVERIFY2(every.size() > shown.size(), qPrintable(QStringLiteral("%1 / %2").arg(every.size()).arg(shown.size())));
        formats::LayerFiles wrong;
        wrong.format = "bmp";
        QVERIFY_THROWS_EXCEPTION(core::Error, formats::export_layers(doc, dir / "bad", 72, "paper", wrong));
    }

    // `genko copy`: a file (the first page, or --page), a folder (a file a page: p001.png …), --pages, --layer, --scale,
    // --quality N%, --metadata, --estimate (the sizes, nothing written), --web (Save for Web's file), and what it refuses.
    void genkoCopy() {
        QTemporaryDir tmp;
        const fs::path dir = path_of(tmp.path());
        core::Document doc = core::new_episode("cli", core::Num(1), 3, core::PageSpec::a4_mono());
        doc.spec.dpi = core::Num(72);
        for (std::size_t i = 0; i < doc.pages.size(); ++i) doc.edit_page(i).spec.dpi = core::Num(72);
        const fs::path book = dir / "cli.genko";
        write_book(doc, book);
        const auto run = [&](const QStringList& args) { return genko::test::run_genko(QStringList{QStringLiteral("copy")} + args); };
        auto r = run({q(book), q(dir / "one.png"), QStringLiteral("--json")});
        QVERIFY2(r.exit_code == 0, qPrintable(r.described()));
        Json out = genko::test::one_line(r.out);
        QCOMPARE(out["count"].get<std::int64_t>(), std::int64_t(1));
        QCOMPARE(out["bytes"].get<std::int64_t>(), std::int64_t(size_of(dir / "one.png")));
        r = run({q(book), q(dir / "pages"), QStringLiteral("--as"), QStringLiteral("jpg"), QStringLiteral("--quality"), QStringLiteral("80%"), QStringLiteral("--json")});
        QVERIFY2(r.exit_code == 0, qPrintable(r.described()));
        QCOMPARE(genko::test::one_line(r.out)["count"].get<std::int64_t>(), std::int64_t(3));
        QVERIFY(fs::is_regular_file(dir / "pages" / "p001.jpg") && fs::is_regular_file(dir / "pages" / "p003.jpg"));
        r = run({q(book), q(dir / "some"), QStringLiteral("--pages"), QStringLiteral("2-3"), QStringLiteral("--scale"), QStringLiteral("50")});
        QVERIFY2(r.exit_code == 0, qPrintable(r.described()));
        QVERIFY(!fs::exists(dir / "some" / "p001.png") && fs::is_regular_file(dir / "some" / "p002.png"));
        const CodecImage half = read(dir / "some" / "p002.png");
        const CodecImage whole = read(dir / "one.png");
        QCOMPARE(half.width(), static_cast<std::uint32_t>(std::lround(whole.width() / 2.0)));
        // (--estimate: each page's size, nothing written)
        r = run({q(book), q(dir / "guess.png"), QStringLiteral("--page"), QStringLiteral("2"), QStringLiteral("--estimate")});
        QVERIFY2(r.exit_code == 0, qPrintable(r.described()));
        out = genko::test::one_line(r.out);
        QVERIFY(out["estimate"].get<bool>());
        QCOMPARE(out["pages"][0]["page"].get<std::int64_t>(), std::int64_t(2));
        QVERIFY(!fs::exists(dir / "guess.png"));
        r = run({q(book), q(dir / "p2.png"), QStringLiteral("--page"), QStringLiteral("2"), QStringLiteral("--json")});
        QCOMPARE(out["bytes"].get<std::int64_t>(), genko::test::one_line(r.out)["bytes"].get<std::int64_t>());
        // (--web: Save for Web's GIF of 16 colours)
        r = run({q(book), q(dir / "web.gif"), QStringLiteral("--web"), QStringLiteral("{\"format\": \"gif\", \"colors\": 16}"), QStringLiteral("--json")});
        QVERIFY2(r.exit_code == 0, qPrintable(r.described()));
        QVERIFY(storage::read_file(dir / "web.gif").starts_with("GIF89a"));
        r = run({q(book), q(dir / "web"), QStringLiteral("--web"), QStringLiteral("{\"preset\": \"JPEG Low\"}"), QStringLiteral("--estimate")});
        QVERIFY2(r.exit_code == 0, qPrintable(r.described()));
        QCOMPARE(genko::test::one_line(r.out)["pages"].size(), std::size_t(3));
        // (refused: a file of several pages, --as against the file's, --web with --quality, a format not written, a scale)
        for (const QStringList& wrong :
             {QStringList{q(book), q(dir / "x.png"), QStringLiteral("--pages"), QStringLiteral("1-2")},
              QStringList{q(book), q(dir / "x.png"), QStringLiteral("--as"), QStringLiteral("jpg")},
              QStringList{q(book), q(dir / "x.gif"), QStringLiteral("--web"), QStringLiteral("{}"), QStringLiteral("--quality"), QStringLiteral("5")},
              QStringList{q(book), q(dir / "x.xyz")}, QStringList{q(book), q(dir / "x.png"), QStringLiteral("--scale"), QStringLiteral("0")},
              QStringList{q(book), q(dir / "x.png"), QStringLiteral("--quality"), QStringLiteral("13")},
              QStringList{q(book), q(dir / "x.png"), QStringLiteral("--srgb"), QStringLiteral("--keep-profile")}}) {
            r = run(wrong);
            QVERIFY2(r.exit_code == 2, qPrintable(wrong.join(QLatin1Char(' ')) + QStringLiteral(": ") + r.described()));
        }
        QVERIFY(!fs::exists(dir / "x.png"));
        // (`genko export --format layers` with its new choices; without them the files as before)
        r = genko::test::run_genko({QStringLiteral("export"), q(book), q(dir / "layers"), QStringLiteral("--format"), QStringLiteral("layers"), QStringLiteral("--layer-format"),
                                    QStringLiteral("jpeg"), QStringLiteral("--prefix"), QStringLiteral("x"), QStringLiteral("--json")});
        QVERIFY2(r.exit_code == 0, qPrintable(r.described()));
        for (const Json& f : genko::test::one_line(r.out)["files"]) QVERIFY(f.get<std::string>().ends_with(".jpg"));
        r = genko::test::run_genko({QStringLiteral("export"), q(book), q(dir / "nolayers"), QStringLiteral("--prefix"), QStringLiteral("x")});
        QCOMPARE(r.exit_code, 2);
    }
};

QTEST_GUILESS_MAIN(CopyContract)
#include "test_contract_copy.moc"
