// PC4 F-c (SPEC.md FILE-01: Web 用に保存（従来）, クイック書き出し): formats/web_export and render/codecs/web against PhotoCraft
// v0.2.0's own tests of them (crates/engine/src/web_cmds/tests.rs: every format estimated, GIF and PNG-8 keeping the
// transparency and the palette, the matte, JPEG's quality, progressive scans and metadata, the presets through their
// parameters, the previews showing the optimised pixels; crates/codecs/src/web.rs: the indexed GIF read back, interlaced or
// not, its transparent entry, WBMP written and read): the colours a palette holds and the dither, the transparency and the
// matte, Web Snap, the estimate against the file written, WBMP's round trip and its refusals; and render/codecs/resize
// (the picture made another size: the same size as it is, a flat colour flat, no dark fringe at a clear edge).

#include <QtTest>

#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <span>
#include <tuple>
#include <string>
#include <vector>

#include "core/error.hpp"
#include "core/file_info.hpp"
#include "core/paths.hpp"
#include "formats/open_file.hpp"
#include "formats/web_export.hpp"
#include "render/codecs/codecs.hpp"
#include "render/codecs/resize.hpp"
#include "render/codecs/web.hpp"
#include "storage/fsutil.hpp"

namespace core = genko::core;
namespace formats = genko::formats;
namespace codecs = genko::render::codecs;
namespace fs = std::filesystem;
using codecs::CodecImage;
using codecs::Layout;
using codecs::Sample;
using core::Json;

namespace {

// PhotoCraft's test picture: 64 × 48, clear; a red square (8..32, 8..32) with a 40 % edge (32..34); a band of a gradient
// under it (rows 36..48).
formats::Rgba8 picture() {
    formats::Rgba8 p;
    p.width = 64;
    p.height = 48;
    p.px.assign(64 * 48 * 4, 0);
    const auto put = [&](int x, int y, std::array<std::uint8_t, 4> c) { std::copy(c.begin(), c.end(), p.px.begin() + (y * 64 + x) * 4); };
    for (int y = 8; y < 32; ++y) {
        for (int x = 8; x < 32; ++x) put(x, y, {255, 0, 0, 255});
        for (int x = 32; x < 34; ++x) put(x, y, {255, 0, 0, 102});
    }
    for (int x = 0; x < 64; ++x) {
        for (int y = 36; y < 48; ++y) put(x, y, {static_cast<std::uint8_t>(std::lround(x / 63.0 * 255)), 128, static_cast<std::uint8_t>(std::lround((63 - x) / 63.0 * 255)), 255});
    }
    return p;
}

std::string optimised(const formats::Rgba8& p, const formats::WebSettings& st, const std::optional<std::string>& xmp = std::nullopt) {
    std::string out;
    const codecs::web::Sink sink = [&out](std::string_view bytes) { out += bytes; };
    const formats::Optimized o = formats::optimize(p, st, std::nullopt, xmp, 72.0, false, &sink);
    if (o.bytes != out.size()) throw std::runtime_error("the bytes counted are not the bytes written");
    return out;
}

CodecImage rgba(const std::string& bytes) { return codecs::decode(bytes).convert(Layout::Rgba, Sample::U8); }

std::array<int, 4> px(const CodecImage& image, int x, int y) {
    const auto* d = reinterpret_cast<const unsigned char*>(image.data().data()) + (std::size_t(y) * image.width() + x) * 4;
    return {d[0], d[1], d[2], d[3]};
}

formats::WebSettings settings(const Json& params) { return formats::web_settings_from_json(params); }

// A book of one page holding the test picture as its layer (its paper white under it).
core::Document book_of(const formats::Rgba8& p) {
    std::string data(p.px.begin(), p.px.end());
    CodecImage image = CodecImage::from_bytes(p.width, p.height, Layout::Rgba, Sample::U8, std::move(data));
    image.meta.dpi = std::make_pair(72.0, 72.0);
    return formats::picture_book(std::move(image), "web").doc;
}

}  // namespace

class WebExportTest : public QObject {
    Q_OBJECT

private slots:
    // Every format at every depth estimated (PhotoCraft's estimates_every_format_at_every_depth): its size, its colours, a
    // WBMP's bytes exactly its header and a bit a pixel.
    void everyFormatIsOptimised() {
        const formats::Rgba8 p = picture();
        for (const char* f : {"gif", "png8", "png24", "jpeg", "wbmp"}) {
            const formats::WebSettings st = settings(Json{{"format", f}, {"colors", 16}});
            const formats::Optimized o = formats::optimize(p, st, std::nullopt, std::nullopt, 72.0, true, nullptr);
            QCOMPARE(o.width, 64u);
            QCOMPARE(o.height, 48u);
            QVERIFY2(o.bytes > 20, f);
            QCOMPARE(o.preview.size(), std::size_t(64 * 48 * 4));
            if (std::string(f) == "gif" || std::string(f) == "png8") QVERIFY2(o.colors && *o.colors <= 16, f);
            if (std::string(f) == "wbmp") QCOMPARE(o.bytes, std::uint64_t(4 + 8 * 48));
            // (the red square's middle stays red — a WBMP's dark)
            const std::size_t i = (16 * 64 + 16) * 4;
            if (std::string(f) == "wbmp") QCOMPARE(int(o.preview[i]), 0);
            else QVERIFY2(o.preview[i] > 200 && o.preview[i + 1] < 60 && o.preview[i + 2] < 60, f);
        }
        QVERIFY_THROWS_EXCEPTION(core::Error, settings(Json{{"format", "tiff"}}));
    }

    // GIF and PNG-8 keep the transparency and the palette (PhotoCraft's gif_and_png8_keep_transparency_and_palette): the clear
    // background clear, the square red, the 40 % edge — under ½ — clear, 8 colours with the clear one; transparency off:
    // everything over the matte.
    void indexedKeepTransparencyAndPalette() {
        const formats::Rgba8 p = picture();
        const CodecImage gif = rgba(optimised(p, settings(Json{{"format", "gif"}, {"colors", 8}, {"dither", "none"}})));
        QCOMPARE(px(gif, 0, 0)[3], 0);
        QCOMPARE(px(gif, 16, 16), (std::array<int, 4>{255, 0, 0, 255}));
        QCOMPARE(px(gif, 33, 16)[3], 0);
        std::set<std::array<int, 4>> colours;
        for (int y = 0; y < 48; ++y)
            for (int x = 0; x < 64; ++x)
                if (px(gif, x, y)[3] > 0) colours.insert(px(gif, x, y));
        QVERIFY2(colours.size() <= 7, qPrintable(QString::number(colours.size())));
        const CodecImage png8 = rgba(optimised(p, settings(Json{{"format", "png8"}, {"transparency", false}, {"matte", "#00ff00"}})));
        QCOMPARE(px(png8, 0, 0), (std::array<int, 4>{0, 255, 0, 255}));
        // (no matte with transparency: the colours opaque, the clear clear)
        const CodecImage bare = rgba(optimised(p, settings(Json{{"format", "png8"}, {"matte", "none"}, {"dither", "none"}})));
        QCOMPARE(px(bare, 0, 0)[3], 0);
        QCOMPARE(px(bare, 16, 16), (std::array<int, 4>{255, 0, 0, 255}));
    }

    // The dither: none maps each pixel to its nearest entry (a gradient of two colours is two flat parts); diffusion and
    // noise mix them (more changes along the row); the colours never past the palette.
    void ditherMixesTheColours() {
        formats::Rgba8 p;
        p.width = 256;
        p.height = 4;
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 256; ++x) {
                const auto v = static_cast<std::uint8_t>(x);
                p.px.insert(p.px.end(), {v, v, v, 255});
            }
        const auto changes = [](const CodecImage& image) {
            int n = 0;
            for (int x = 1; x < 256; ++x) n += px(image, x, 1)[0] != px(image, x - 1, 1)[0];
            return n;
        };
        const CodecImage flat = rgba(optimised(p, settings(Json{{"format", "gif"}, {"colors", 2}, {"dither", "none"}, {"palette", "uniform"}})));
        const CodecImage diffused = rgba(optimised(p, settings(Json{{"format", "gif"}, {"colors", 2}, {"dither", "diffusion"}, {"palette", "uniform"}})));
        const CodecImage noisy = rgba(optimised(p, settings(Json{{"format", "png8"}, {"colors", 2}, {"dither", "noise"}, {"ditherAmount", 100}, {"palette", "uniform"}})));
        QVERIFY2(changes(flat) <= 1, qPrintable(QString::number(changes(flat))));
        QVERIFY2(changes(diffused) > 20, qPrintable(QString::number(changes(diffused))));
        QVERIFY2(changes(noisy) > 4, qPrintable(QString::number(changes(noisy))));
    }

    // Web Snap at 100 %: every entry on the web-safe cube (each channel a multiple of 51).
    void webSnapPutsColoursOnTheCube() {
        const formats::Rgba8 p = picture();
        const CodecImage snapped = rgba(optimised(p, settings(Json{{"format", "gif"}, {"colors", 32}, {"dither", "none"}, {"webSnap", 100}})));
        for (int y = 0; y < 48; ++y)
            for (int x = 0; x < 64; ++x) {
                const auto c = px(snapped, x, y);
                if (c[3] == 0) continue;
                for (int k = 0; k < 3; ++k) QVERIFY2(c[k] % 51 == 0, qPrintable(QStringLiteral("%1,%2: %3").arg(x).arg(y).arg(c[k])));
            }
    }

    // JPEG: the quality makes it smaller, progressive scans (SOF2), the metadata chosen (PhotoCraft's
    // jpeg_quality_progressive_and_metadata): the copyright kept, the description not, nothing with none.
    void jpegQualityProgressiveAndMetadata() {
        const formats::Rgba8 p = picture();
        core::Document doc = book_of(p);
        core::FileInfo info;
        info.copyright = "© Example";
        info.author = "A. Person";
        info.description = "secret notes";
        doc.file_info = info;
        const std::string lo = optimised(p, settings(Json{{"format", "jpeg"}, {"quality", 10}}), formats::web_xmp(doc, formats::WebMetadata::None));
        const std::string hi = optimised(p, settings(Json{{"format", "jpeg"}, {"quality", 95}, {"progressive", true}}),
                                         formats::web_xmp(doc, formats::WebMetadata::Copyright));
        QVERIFY(lo.size() < hi.size());
        QVERIFY(hi.find("\xFF\xC2") != std::string::npos);
        QVERIFY(lo.find("\xFF\xC2") == std::string::npos);
        const std::string xmp = codecs::decode(hi).meta.xmp.value_or("");
        QVERIFY2(xmp.find("Example") != std::string::npos, xmp.c_str());
        QVERIFY(xmp.find("secret") == std::string::npos);
        QVERIFY(xmp.find("A. Person") == std::string::npos);  // (the copyright alone; the contact with CopyrightAndContact)
        QVERIFY(formats::web_xmp(doc, formats::WebMetadata::CopyrightAndContact)->find("A. Person") != std::string::npos);
        QVERIFY(!codecs::decode(lo).meta.xmp);
        QVERIFY(!formats::web_xmp(doc, formats::WebMetadata::None));
    }

    // The presets (PhotoCraft's web_settings_from_params): each through its parameters and back the same; a preset with
    // changes on top; names and values it does not know refused.
    void presetsAndTheirParameters() {
        const formats::WebSettings st = settings(Json{{"preset", "GIF 32 No Dither"}, {"matte", "none"}, {"webSnap", 50}});
        QVERIFY(st.format == formats::WebFormat::Gif);
        QCOMPARE(st.colors, std::size_t(32));
        QVERIFY(st.dither == genko::render::quantize::Dither::None);
        QVERIFY(!st.matte);
        QVERIFY(std::abs(st.web_snap - 0.5f) < 1e-6f);
        QCOMPARE(formats::web_presets().size(), std::size_t(12));
        for (const std::string_view name : formats::web_presets()) {
            const auto preset = formats::web_preset(name);
            QVERIFY(preset.has_value());
            QVERIFY2(settings(formats::to_json(*preset)) == *preset, std::string(name).c_str());
        }
        QVERIFY(formats::web_preset("JPEG Maximum")->quality == 100);
        QVERIFY_THROWS_EXCEPTION(core::Error, settings(Json{{"preset", "Nope"}}));
        QVERIFY_THROWS_EXCEPTION(core::Error, settings(Json{{"palette", "weird"}}));
        QVERIFY_THROWS_EXCEPTION(core::Error, settings(Json{{"matte", "red"}}));
        QVERIFY_THROWS_EXCEPTION(core::Error, settings(Json{{"colors", "many"}}));
        const formats::WebSize size = formats::web_size_from_json(Json{{"percent", 50}, {"resample", "nearest"}});
        QCOMPARE(*size.percent, 50.0);
        QVERIFY(size.resample == codecs::ResizeFilter::Nearest);
        QVERIFY_THROWS_EXCEPTION(core::Error, formats::web_size_from_json(Json{{"width", 1e9}}));
        QVERIFY_THROWS_EXCEPTION(core::Error, formats::web_size_from_json(Json{{"resample", "lanczos9"}}));
    }

    // A page saved for the Web (save_for_web): the estimate without a file is the file's size exactly, the file written whole
    // (nothing beside it), at a percent its size and an estimate from a proxy near the file; a layer alone keeps its
    // transparency, the page — its paper under it — none.
    void aPageSavedAndItsEstimate() {
        QTemporaryDir tmp;
        const core::Document doc = book_of(picture());
        for (const char* f : {"gif", "png8", "png24", "jpeg", "wbmp"}) {
            const formats::WebSettings st = settings(Json{{"format", f}});
            const formats::WebResult guess = formats::save_for_web(doc, 0, st);
            const fs::path dest = core::path_from_utf8(tmp.path().toStdString()) / (std::string("page.") + std::string(formats::web_extension(st.format)));
            const formats::WebResult written = formats::save_for_web(doc, 0, st, {}, dest);
            QVERIFY(guess.exact);
            QCOMPARE(guess.bytes, written.bytes);
            QCOMPARE(std::uint64_t(fs::file_size(dest)), written.bytes);
            QCOMPARE(QDir(tmp.path()).entryList(QDir::Files | QDir::Hidden).filter(QStringLiteral(".part")).size(), 0);
        }
        formats::WebSize half;
        half.percent = 50;
        const fs::path small = core::path_from_utf8(tmp.path().toStdString()) / "half.png";
        const formats::WebResult r = formats::save_for_web(doc, 0, settings(Json{{"format", "png24"}}), half, small);
        QCOMPARE(r.width, 32u);
        QCOMPARE(r.height, 24u);
        const CodecImage back = codecs::decode(genko::storage::read_file(small));
        QCOMPARE(back.width(), 32u);
        // (the page is opaque: its paper; the layer alone keeps what is clear)
        const CodecImage page = rgba(genko::storage::read_file(core::path_from_utf8(tmp.path().toStdString()) / "page.png"));
        QCOMPARE(px(page, 0, 0)[3], 255);
        std::string layer_id;
        for (const core::Layer& l : doc.page(0).layers)
            if (l.kind == core::LayerKind::Raster) layer_id = l.id;
        QVERIFY(!layer_id.empty());
        const fs::path alone = core::path_from_utf8(tmp.path().toStdString()) / "layer.png";
        formats::save_for_web(doc, 0, settings(Json{{"format", "png24"}}), {}, alone, layer_id);
        const CodecImage cut = rgba(genko::storage::read_file(alone));
        // (cut to what it holds: the square, its edge and the band — x 8..64? the band is the full width: 0..64, y 8..48)
        QCOMPARE(cut.width(), 64u);
        QCOMPARE(cut.height(), 40u);
        QCOMPARE(px(cut, 0, 0)[3], 0);
        // (an estimate from a proxy: near the file's size)
        formats::WebSize big;
        big.percent = 400;
        const formats::WebResult proxied = formats::save_for_web(doc, 0, settings(Json{{"format", "png24"}}), big, std::nullopt, std::nullopt, 64);
        const formats::WebResult exact = formats::save_for_web(doc, 0, settings(Json{{"format", "png24"}}), big);
        QVERIFY(!proxied.exact);
        QCOMPARE(proxied.width, 256u);
        QVERIFY2(proxied.bytes > exact.bytes / 4 && proxied.bytes < exact.bytes * 4, qPrintable(QStringLiteral("%1 / %2").arg(proxied.bytes).arg(exact.bytes)));
    }

    // GIF's writer (PhotoCraft's gif_roundtrips_through_the_decoder, gif_small_palettes_and_transparency): every index read
    // back as its colour, interlaced or not; the transparent entry clear; indices past the palette refused.
    void gifIndicesReadBack() {
        std::vector<codecs::web::Rgb8> pal;
        for (int i = 0; i < 200; ++i) pal.push_back({static_cast<std::uint8_t>(i), static_cast<std::uint8_t>(255 - i), static_cast<std::uint8_t>(i * 3 % 256)});
        for (const auto [w, h, interlaced] : {std::tuple{1u, 1u, false}, {37u, 23u, false}, {37u, 23u, true}, {300u, 200u, false}}) {
            std::vector<std::uint8_t> idx(std::size_t(w) * h);
            for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = static_cast<std::uint8_t>(((i % w) * 7 + (i / w) * 13 + (i * i) % 5) % pal.size());
            const CodecImage back = rgba(codecs::web::encode_gif_indexed(w, h, idx, pal, std::nullopt, interlaced));
            QCOMPARE(back.width(), w);
            for (std::size_t i = 0; i < idx.size(); ++i) {
                const auto c = px(back, int(i % w), int(i / w));
                QCOMPARE((std::array<int, 3>{c[0], c[1], c[2]}), (std::array<int, 3>{pal[idx[i]][0], pal[idx[i]][1], pal[idx[i]][2]}));
            }
        }
        const std::vector<codecs::web::Rgb8> three{{255, 255, 255}, {0, 0, 0}, {255, 0, 0}};
        std::vector<std::uint8_t> idx(64 * 64);
        for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = static_cast<std::uint8_t>(i % 3);
        const CodecImage clear = rgba(codecs::web::encode_gif_indexed(64, 64, idx, three, std::uint8_t(2), false));
        QCOMPARE(px(clear, 0, 0)[3], 255);
        QCOMPARE(px(clear, 2, 0)[3], 0);
        QVERIFY_THROWS_EXCEPTION(core::Error, codecs::web::encode_gif_indexed(2, 2, std::vector<std::uint8_t>{0, 1, 2, 3}, three, std::nullopt, false));
        QVERIFY_THROWS_EXCEPTION(core::Error, codecs::web::encode_gif_indexed(70000, 1, std::vector<std::uint8_t>(70000, 0), three, std::nullopt, false));
    }

    // PNG-8: as few bits as the palette needs (1 for two colours), read back as its colours, Adam7 too, the transparent entry.
    void png8ReadsBack() {
        const std::vector<codecs::web::Rgb8> two{{10, 20, 30}, {200, 100, 50}};
        std::vector<std::uint8_t> idx(13 * 7);
        for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = static_cast<std::uint8_t>((i * 7) % 2);
        for (const bool interlaced : {false, true}) {
            const std::string bytes = codecs::web::encode_png_indexed(13, 7, idx, two, std::uint8_t(1), interlaced, 300.0);
            QCOMPARE(static_cast<int>(static_cast<unsigned char>(bytes[24])), 1);  // (IHDR's bit depth)
            const CodecImage back = rgba(bytes);
            for (std::size_t i = 0; i < idx.size(); ++i) {
                const auto c = px(back, int(i % 13), int(i / 13));
                if (idx[i] == 1) QCOMPARE(c[3], 0);
                else QCOMPARE(c, (std::array<int, 4>{10, 20, 30, 255}));
            }
        }
    }

    // WBMP (PhotoCraft's wbmp_roundtrips): its header, a multi-byte width, read back; cut short, not a WBMP, or a size past
    // the limits refused — before its pixels are made.
    void wbmpRoundTripsAndRefuses() {
        std::vector<std::uint8_t> white(13 * 5);
        for (std::size_t i = 0; i < white.size(); ++i) white[i] = i % 3 == 0;
        const std::string bytes = codecs::web::encode_wbmp(13, 5, white);
        QCOMPARE(bytes.substr(0, 4), std::string("\0\0\x0D\x05", 4));
        const codecs::web::Wbmp back = codecs::web::decode_wbmp(bytes);
        QCOMPARE(back.width, 13u);
        QCOMPARE(back.height, 5u);
        QVERIFY(back.white == white);
        const std::string wide = codecs::web::encode_wbmp(300, 1, std::vector<std::uint8_t>(300, 1));
        QCOMPARE(wide.substr(2, 2), std::string("\x82\x2C", 2));
        QVERIFY_THROWS_EXCEPTION(core::Error, codecs::web::decode_wbmp(wide.substr(0, 10)));
        QVERIFY_THROWS_EXCEPTION(core::Error, codecs::web::decode_wbmp(std::string("\x01\x00\x01\x01\x00", 5)));
        // (a header that says 2^28 × 2^28: refused as too large, not allocated)
        const std::string bomb("\x00\x00\x81\x80\x80\x80\x00\x81\x80\x80\x80\x00", 12);
        try {
            codecs::web::decode_wbmp(bomb);
            QFAIL("a WBMP past the limits was read");
        } catch (const core::Error& error) {
            QCOMPARE(std::string(error.code()), std::string("image_too_large"));
        }
    }

    // The resampler: the same size is the picture as it is; a flat colour stays flat at every filter; a reduction of a
    // checker is its mean (no aliasing); an opaque pixel beside a clear one has no dark fringe; floats past 1 kept.
    void resizeKeepsWhatItShould() {
        CodecImage flat = CodecImage::blank(40, 30, Layout::Rgb, Sample::U8);
        for (std::size_t i = 0; i < flat.sample_count(); ++i) flat.data()[i] = static_cast<char>(i % 3 == 0 ? 200 : 50);
        QVERIFY(codecs::resized(flat, 40, 30) == flat);
        for (const auto filter : {codecs::ResizeFilter::Nearest, codecs::ResizeFilter::Bilinear, codecs::ResizeFilter::Bicubic}) {
            const CodecImage made = codecs::resized(flat, 17, 61, filter);
            QCOMPARE(made.width(), 17u);
            for (std::size_t i = 0; i < made.sample_count(); ++i) QCOMPARE(int(static_cast<unsigned char>(made.data()[i])), i % 3 == 0 ? 200 : 50);
        }
        CodecImage checker = CodecImage::blank(64, 64, Layout::Gray, Sample::U8);
        for (std::size_t i = 0; i < checker.sample_count(); ++i) checker.data()[i] = static_cast<char>(((i % 64) + (i / 64)) % 2 ? 255 : 0);
        const CodecImage mean = codecs::resized(checker, 8, 8, codecs::ResizeFilter::Bicubic);
        for (std::size_t i = 0; i < mean.sample_count(); ++i) QVERIFY(std::abs(int(static_cast<unsigned char>(mean.data()[i])) - 128) <= 2);
        CodecImage edge = CodecImage::blank(2, 1, Layout::Rgba, Sample::U8);
        const char red[8] = {char(255), 0, 0, char(255), 0, 0, 0, 0};
        std::memcpy(edge.data().data(), red, 8);
        const CodecImage wide = codecs::resized(edge, 8, 1, codecs::ResizeFilter::Bilinear);
        for (std::uint32_t x = 0; x < 8; ++x) {
            const auto c = px(wide, int(x), 0);
            if (c[3] > 0) QVERIFY2(c[0] >= 250, qPrintable(QStringLiteral("%1: %2").arg(x).arg(c[0])));
        }
        const float hdr[3] = {4.0f, 2.0f, 1.0f};
        CodecImage bright = CodecImage::from_normalized(1, 1, Layout::Rgb, Sample::F32, std::span<const float>(hdr, 3));
        const CodecImage kept = codecs::resized(bright, 3, 3, codecs::ResizeFilter::Bicubic);
        QVERIFY(std::abs(kept.at(1, 1, 0) - 4.0f) < 1e-4f);
    }
};

QTEST_GUILESS_MAIN(WebExportTest)
#include "test_web_export.moc"
