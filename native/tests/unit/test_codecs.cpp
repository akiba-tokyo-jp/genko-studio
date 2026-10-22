// render/codecs (PC4 F-0, SPEC.md FILE-01: the codecs' foundation) against PhotoCraft v0.2.0's own tests of
// crates/codecs (tests/detect_caps.rs: the magic numbers, the extensions and the capability table with its symmetric
// guarantee; tests/fidelity.rs: the losses said; tests/limits.rs: the bounds checked before the pixels are allocated;
// src/image.rs's tests: the picture's conversions) on this build's table — PNG, JPEG, TIFF, WebP, GIF and BMP read through
// this build's readers, PNG and JPEG written through its writers —, the wrapped formats read and written back, and the
// losses said exactly when the round trip changes the picture.

#include <QtTest>

#include <cmath>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "core/color_raster.hpp"
#include "core/error.hpp"
#include "core/ink.hpp"
#include "formats/pillow_save.hpp"
#include "render/codecs/codecs.hpp"
#include "render/image.hpp"
#include "render/not_yet_ported.hpp"
#include "render/png.hpp"

namespace codecs = genko::render::codecs;
using codecs::CodecImage;
using codecs::Format;
using codecs::Layout;
using codecs::Loss;
using codecs::Sample;

namespace {

// A deterministic picture (PhotoCraft's common::synth in spirit): smooth gradients with a little noise, its alpha varying.
CodecImage synth(std::uint32_t w, std::uint32_t h, Layout layout, Sample sample, std::uint32_t seed = 1) {
    std::vector<float> values;
    const unsigned ch = codecs::channels(layout);
    std::uint32_t state = seed * 2654435761u + 7;
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            for (unsigned c = 0; c < ch; ++c) {
                state = state * 1664525u + 1013904223u;
                const float noise = static_cast<float>((state >> 8) & 0xff) / 255.0f * 0.1f;
                float v = (static_cast<float>(x) / std::max(1u, w - 1) * 0.6f + static_cast<float>(y) / std::max(1u, h - 1) * 0.3f + noise) / 1.0f;
                if (c == 1) v = 1.0f - v;
                if (c == 2) v = 0.5f * v + 0.25f;
                if (codecs::has_alpha(layout) && c == ch - 1) v = 0.25f + 0.75f * static_cast<float>(x % 7) / 6.0f;
                values.push_back(std::clamp(v, 0.0f, 1.0f));
            }
        }
    }
    return CodecImage::from_normalized(w, h, layout, sample, values);
}

double psnr(const CodecImage& a, const CodecImage& b) {
    const std::vector<float> x = a.normalized(), y = b.normalized();
    if (x.size() != y.size() || x.empty()) return 0;
    double sum = 0;
    for (std::size_t i = 0; i < x.size(); ++i) sum += (x[i] - y[i]) * (x[i] - y[i]);
    const double mse = sum / static_cast<double>(x.size());
    return mse <= 0 ? 1e9 : 10.0 * std::log10(1.0 / mse);
}

bool has(const std::vector<Loss>& losses, Loss::Kind kind) {
    return std::any_of(losses.begin(), losses.end(), [kind](const Loss& l) { return l.kind == kind; });
}

std::string code_of(const std::function<void()>& run) {
    try {
        run();
    } catch (const genko::render::NotYetPorted& error) {
        return "not_yet_ported:" + error.element();
    } catch (const genko::core::Error& error) {
        return error.code();
    }
    return {};
}

std::string png_chunk(const char* type, const std::string& data) {
    std::string out;
    const auto n = static_cast<std::uint32_t>(data.size());
    for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<char>((n >> s) & 0xff));
    std::string body = std::string(type, 4) + data;
    std::uint32_t crc = 0xffffffffu;
    for (const char ch : body) {
        crc ^= static_cast<unsigned char>(ch);
        for (int k = 0; k < 8; ++k) crc = (crc & 1) != 0 ? 0xEDB88320u ^ (crc >> 1) : crc >> 1;
    }
    crc = ~crc;
    out += body;
    for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<char>((crc >> s) & 0xff));
    return out;
}

std::string be32(std::uint32_t v) {
    return {static_cast<char>(v >> 24), static_cast<char>((v >> 16) & 0xff), static_cast<char>((v >> 8) & 0xff), static_cast<char>(v & 0xff)};
}

std::string data_file(const char* relative) {
    QFile f(QStringLiteral(GENKO_TEST_DATA "/") + QString::fromUtf8(relative));
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray bytes = f.readAll();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

}  // namespace

class CodecsTest : public QObject {
    Q_OBJECT

private slots:
    // --- the capability table (detect_caps.rs) ------------------------------------------------------------------------

    void everyEnabledFormatIsSymmetricOrSaysWhy() {
        for (const Format f : codecs::kFormats) {
            const codecs::FormatCaps c = codecs::caps(f);
            if (!(c.read || c.write) || (c.read && c.write)) continue;
            const auto list = codecs::asymmetric_formats();
            const auto it = std::find_if(list.begin(), list.end(), [f](const codecs::Asymmetric& a) { return a.format == f; });
            QVERIFY2(it != list.end(), std::string(codecs::name(f)).c_str());
            QVERIFY(it->reason.size() > 20);
        }
        // (never written and not read back)
        for (const Format f : codecs::kFormats) {
            if (codecs::caps(f).write) QVERIFY2(codecs::caps(f).read, std::string(codecs::name(f)).c_str());
        }
        const auto list = codecs::asymmetric_formats();
        std::set<Format> seen;
        for (const codecs::Asymmetric& a : list) QVERIFY(seen.insert(a.format).second);
    }

    void thisBuildsTable() {
        // (PC4 F-0: PNG and JPEG read and written; TIFF, WebP, GIF and BMP read; the others neither — F-a's)
        for (const Format f : {Format::Png, Format::Jpeg}) QVERIFY(codecs::caps(f).read && codecs::caps(f).write);
        for (const Format f : {Format::Tiff, Format::WebP, Format::Gif, Format::Bmp}) QVERIFY(codecs::caps(f).read && !codecs::caps(f).write);
        for (const Format f : {Format::Tga, Format::Ico, Format::Pnm, Format::Qoi, Format::OpenExr, Format::Hdr, Format::Avif}) {
            QVERIFY(!codecs::enabled(f));
            QCOMPARE(code_of([&] { (void)codecs::decode_as(f, "x"); }), std::string("not_yet_ported:image_format"));
            QCOMPARE(code_of([&] { (void)codecs::encode(synth(4, 4, Layout::Rgb, Sample::U8), f); }), std::string("not_yet_ported:image_write"));
        }
    }

    void capsAreConsistent() {
        for (const Format f : codecs::kFormats) {
            const codecs::FormatCaps c = codecs::caps(f);
            QVERIFY(!c.depths.empty() && !c.layouts.empty());
            QCOMPARE(c.alpha, std::any_of(c.layouts.begin(), c.layouts.end(), [](Layout l) { return codecs::has_alpha(l); }));
            QVERIFY(!codecs::extensions(f).empty() && !codecs::name(f).empty() && codecs::mime_type(f).starts_with("image/"));
            QCOMPARE(codecs::from_id(codecs::id_of(f)), std::optional<Format>(f));
        }
    }

    // --- detection --------------------------------------------------------------------------------------------------

    void detectsWhatItWrites() {
        for (const Format f : codecs::kFormats) {
            if (!codecs::caps(f).write) continue;
            const std::string bytes = codecs::encode(synth(16, 12, Layout::Rgba, Sample::U8), f);
            QCOMPARE(codecs::detect(bytes), std::optional<Format>(f));
        }
    }

    void detectsMagicNumbers() {
        using std::string_view;
        const auto is = [](string_view bytes, std::optional<Format> f) { return codecs::detect(bytes) == f; };
        QVERIFY(is(string_view("\x89PNG\r\n\x1a\n\0\0", 10), Format::Png));
        QVERIFY(is("\xFF\xD8\xFF\xE0", Format::Jpeg));
        QVERIFY(is("\xFF\xD8", std::nullopt));
        for (const string_view tiff : {string_view("II*\0\x08\0\0\0", 8), string_view("MM\0*\0\0\0\x08", 8), string_view("II+\0\x08\0\0\0", 8),
                                       string_view("MM\0+\0\x08\0\0", 8)})
            QVERIFY(is(tiff, Format::Tiff));
        QVERIFY(is(string_view("RIFF\x10\0\0\0WEBPVP8L", 16), Format::WebP));
        QVERIFY(is(string_view("RIFF\x10\0\0\0WAVEfmt ", 16), std::nullopt));
        QVERIFY(is(string_view("GIF87a\x01\0", 8), Format::Gif));
        QVERIFY(is(string_view("GIF89a\x01\0", 8), Format::Gif));
        QVERIFY(is(string_view("GIF90a\x01\0", 8), std::nullopt));
        QVERIFY(is(std::string("BM") + std::string(52, '\0'), Format::Bmp));
        QVERIFY(is("BM", std::nullopt));
        QVERIFY(is(string_view("\0\0\1\0\1\0\x10\x10", 8), Format::Ico));
        QVERIFY(is(string_view("\0\0\1\0\0\0", 6), std::nullopt));  // (no pictures)
        for (const char* pnm : {"P1\n", "P2 ", "P3\t", "P4\n", "P5\r", "P6\n", "P7\n", "PF\n", "Pf\n"}) QVERIFY2(is(pnm, Format::Pnm), pnm);
        QVERIFY(is("P8\n", std::nullopt));
        QVERIFY(is("P6x", std::nullopt));
        QVERIFY(is(string_view("qoif\0\0\0\x01\0\0\0\x01\x03\0", 14), Format::Qoi));
        QVERIFY(is(string_view("\x76\x2F\x31\x01\x02\0\0\0", 8), Format::OpenExr));
        QVERIFY(is("#?RADIANCE\n", Format::Hdr));
        QVERIFY(is("#?RGBE\n", Format::Hdr));
        QVERIFY(is(string_view("\0\0\0\x1c" "ftypavif\0\0\0\0avifmif1miaf", 28), Format::Avif));
        QVERIFY(is(string_view("\0\0\0\x1c" "ftypavis\0\0\0\0avismsf1miaf", 28), Format::Avif));
        QVERIFY(is(string_view("\0\0\0\x18" "ftypmif1\0\0\0\0mif1avif", 24), Format::Avif));  // (a compatible brand)
        QVERIFY(is(string_view("\0\0\0\x18" "ftypisom\0\0\0\0isommp41", 24), std::nullopt));  // (an MP4)
    }

    void detectsTgaByItsHeader() {
        std::string b{0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 2, 0, 24, 0};
        b += std::string(12, '\0');
        QCOMPARE(codecs::detect(b), std::optional<Format>(Format::Tga));
        std::string bad = b;
        bad[2] = 7;  // (no such image type)
        QCOMPARE(codecs::detect(bad), std::optional<Format>());
        bad = b;
        bad[16] = 13;  // (no such depth)
        QCOMPARE(codecs::detect(bad), std::optional<Format>());
    }

    void detectsNothingElse() {
        for (const std::string& bytes : {std::string(), std::string("x"), std::string("hello world, this is not an image"), std::string("%PDF-1.7\n"),
                                         std::string("PK\x03\x04"), std::string(64, '\0'), std::string("8BPS\0\1", 6)})
            QCOMPARE(codecs::detect(bytes), std::optional<Format>());
        // (random bytes are rarely taken for a TGA)
        std::uint32_t state = 5;
        int hits = 0;
        for (int i = 0; i < 2000; ++i) {
            std::string bytes(64, '\0');
            for (char& c : bytes) {
                state = state * 1664525u + 1013904223u;
                c = static_cast<char>(state >> 24);
            }
            if (codecs::detect(bytes) == Format::Tga) ++hits;
        }
        QVERIFY2(hits < 10, std::to_string(hits).c_str());
        QCOMPARE(code_of([] { (void)codecs::decode("garbage garbage"); }), std::string("unidentified_image"));
    }

    void extensionsMapBack() {
        std::set<std::string_view> all;
        std::size_t count = 0;
        for (const Format f : codecs::kFormats) {
            for (const std::string_view e : codecs::extensions(f)) {
                const std::string ext(e);
                QCOMPARE(codecs::from_extension(ext), std::optional<Format>(f));
                std::string upper = ext;
                for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                QCOMPARE(codecs::from_extension(upper), std::optional<Format>(f));
                QCOMPARE(codecs::from_extension("." + ext), std::optional<Format>(f));
                QCOMPARE(codecs::from_extension("dir/file.name." + ext), std::optional<Format>(f));
                all.insert(e);
                ++count;
            }
        }
        QCOMPARE(all.size(), count);  // (each extension one format's)
        QCOMPARE(codecs::from_extension("JPG"), std::optional<Format>(Format::Jpeg));
        QCOMPARE(codecs::from_extension("pfm"), std::optional<Format>(Format::Pnm));
        QCOMPARE(codecs::from_extension("exr"), std::optional<Format>(Format::OpenExr));
        QCOMPARE(codecs::from_extension("psd"), std::optional<Format>());
        QCOMPARE(codecs::from_extension(""), std::optional<Format>());
        QCOMPARE(codecs::from_extension("png.bak"), std::optional<Format>());
    }

    // --- the picture (image.rs) -------------------------------------------------------------------------------------

    void picturesConvert() {
        std::string bytes;
        for (int v = 0; v < 256; ++v) bytes.push_back(static_cast<char>(v));
        const CodecImage grey = CodecImage::from_bytes(256, 1, Layout::Gray, Sample::U8, bytes);
        QCOMPARE(grey.convert(Layout::Gray, Sample::U16).convert(Layout::Gray, Sample::U8).data(), bytes);
        std::vector<float> wide;
        for (std::uint32_t v = 0; v <= 65535; v += 7) wide.push_back(static_cast<float>(v / 65535.0));
        const CodecImage sixteen = CodecImage::from_normalized(static_cast<std::uint32_t>(wide.size()), 1, Layout::Gray, Sample::U16, wide);
        QCOMPARE(sixteen.convert(Layout::Gray, Sample::F32).convert(Layout::Gray, Sample::U16).data(), sixteen.data());
        // (a profile kept only for the same colours)
        CodecImage cmyk = CodecImage::blank(2, 2, Layout::Cmyk, Sample::U8);
        cmyk.icc = std::string("\1\2\3", 3);
        QVERIFY(!cmyk.convert(Layout::Rgb, Sample::U8).icc);
        QVERIFY(cmyk.convert(Layout::CmykA, Sample::U16).icc);
        QCOMPARE(code_of([] { (void)CodecImage::from_bytes(2, 2, Layout::Rgb, Sample::U8, std::string(11, '\0')); }), std::string("value"));
        const CodecImage ga = CodecImage::from_bytes(1, 1, Layout::GrayA, Sample::U8, std::string("\x0a\x14", 2));
        QCOMPARE(ga.convert(Layout::Rgba, Sample::U8).data(), std::string("\x0a\x0a\x0a\x14", 4));
        QCOMPARE(CodecImage::from_bytes(1, 1, Layout::Cmyk, Sample::U8, std::string("\0\0\0\xff", 4)).convert(Layout::Rgba, Sample::U8).data(),
                 std::string("\0\0\0\xff", 4));
        QCOMPARE(CodecImage::from_bytes(1, 1, Layout::Rgb, Sample::U8, std::string("\xff\0\0", 3)).convert(Layout::Cmyk, Sample::U8).data(),
                 std::string("\0\xff\xff\0", 4));
        // (adding an alpha quickly is the general conversion)
        for (const Sample s : codecs::kSamples) {
            for (const Layout l : {Layout::Gray, Layout::Rgb, Layout::Cmyk}) {
                const CodecImage p = synth(7, 3, l, s);
                const CodecImage fast = p.convert(codecs::with_alpha(l), s);
                QCOMPARE(fast.data(), codecs::quantize(codecs::convert_layout(p.normalized(), l, codecs::with_alpha(l)), s));
            }
        }
    }

    void halfFloats() {
        for (const float v : {0.0f, 1.0f, -2.0f, 0.5f, 65504.0f, 6.1035156e-05f, 5.9604645e-08f, 0.33325195f}) {
            QCOMPARE(codecs::float_from_half(codecs::half_from_float(v)), v);
        }
        QCOMPARE(codecs::half_from_float(1.0f), std::uint16_t(0x3c00));
        QCOMPARE(codecs::half_from_float(65520.0f), std::uint16_t(0x7c00));  // (past the largest: infinity)
        QVERIFY(std::isnan(codecs::float_from_half(codecs::half_from_float(std::nanf("")))));
        QCOMPARE(codecs::half_from_float(1.0f + 1.0f / 2048.0f), std::uint16_t(0x3c00));  // (halfway: to even)
    }

    // --- read and write -----------------------------------------------------------------------------------------------

    void pngRoundTripsLosslessly() {
        for (const Layout l : {Layout::Gray, Layout::GrayA, Layout::Rgb, Layout::Rgba}) {
            const CodecImage p = synth(33, 21, l, Sample::U8, 3);
            QVERIFY(codecs::losses(p, Format::Png).empty());
            const CodecImage back = codecs::decode(codecs::encode(p, Format::Png));
            QCOMPARE(back.layout(), l);
            QCOMPARE(back.sample(), Sample::U8);
            QCOMPARE(back.data(), p.data());
        }
    }

    void jpegRoundTripsWithinItsLoss() {
        const CodecImage p = synth(64, 48, Layout::Rgb, Sample::U8, 4);
        const auto losses = codecs::losses(p, Format::Jpeg);
        QCOMPARE(losses.size(), std::size_t(1));
        QVERIFY(has(losses, Loss::Kind::LossyCompression));
        const CodecImage back = codecs::decode(codecs::encode(p, Format::Jpeg));
        QCOMPARE(back.layout(), Layout::Rgb);
        QVERIFY2(psnr(back, p) > 30.0, std::to_string(psnr(back, p)).c_str());
        codecs::EncodeOptions low;
        low.jpeg_quality = 10;
        QVERIFY(codecs::encode(p, Format::Jpeg, low).size() < codecs::encode(p, Format::Jpeg).size());
    }

    // What is said is what the round trip does: a write that says no loss of the pixels gives them back exactly, one
    // that says one (depth, range, alpha, colours, compression) changes them (fidelity.rs's guarantee).
    void lossesAreWhatTheRoundTripDoes() {
        for (const Format f : codecs::kFormats) {
            if (!codecs::caps(f).write) continue;
            for (const Layout l : codecs::kLayouts) {
                for (const Sample s : codecs::kSamples) {
                    const CodecImage p = synth(17, 9, l, s, 9);
                    const auto losses = codecs::losses(p, f);
                    const CodecImage back = codecs::decode(codecs::encode(p, f));
                    const codecs::Plan plan = codecs::plan(p, f);
                    QCOMPARE(back.layout(), plan.layout);
                    const bool pixels_kept = back.convert(l, s).data() == p.data();
                    const bool said = has(losses, Loss::Kind::DepthReduced) || has(losses, Loss::Kind::RangeClipped) ||
                                      has(losses, Loss::Kind::AlphaDiscarded) || has(losses, Loss::Kind::CmykConverted) ||
                                      has(losses, Loss::Kind::ColorToGray) || has(losses, Loss::Kind::LossyCompression);
                    const std::string what = std::string(codecs::name(f)) + " " + std::string(codecs::to_string(l)) + " " + std::string(codecs::to_string(s));
                    QVERIFY2(said != pixels_kept, what.c_str());
                }
            }
        }
    }

    void lossesSaid() {
        const CodecImage deep = synth(8, 8, Layout::Rgb, Sample::U16);
        const auto to_png = codecs::losses(deep, Format::Png);
        QVERIFY(has(to_png, Loss::Kind::DepthReduced));
        QCOMPARE(to_png.front().text(), std::string("16 bit を 8 bit にします"));
        QVERIFY(has(codecs::losses(synth(8, 8, Layout::Rgba, Sample::U8), Format::Jpeg), Loss::Kind::AlphaDiscarded));
        CodecImage opaque = synth(8, 8, Layout::Rgba, Sample::U8);
        for (std::size_t i = 3; i < opaque.data().size(); i += 4) opaque.data()[i] = static_cast<char>(0xff);
        QVERIFY(!has(codecs::losses(opaque, Format::Jpeg), Loss::Kind::AlphaDiscarded));
        const auto cmyk = codecs::losses(synth(8, 8, Layout::Cmyk, Sample::U8), Format::Png);
        QVERIFY(has(cmyk, Loss::Kind::CmykConverted));
        QCOMPARE(std::find_if(cmyk.begin(), cmyk.end(), [](const Loss& l) { return l.kind == Loss::Kind::CmykConverted; })->layout, Layout::Rgb);
        // (HDR values clipped by integers)
        std::vector<float> hdr(4 * 4 * 3, 0.5f);
        hdr[0] = 2.5f;
        QVERIFY(has(codecs::losses(CodecImage::from_normalized(4, 4, Layout::Rgb, Sample::F32, hdr), Format::Png), Loss::Kind::RangeClipped));
        QVERIFY(!has(codecs::losses(synth(4, 4, Layout::Rgb, Sample::F32), Format::Png), Loss::Kind::RangeClipped));
        // (the metadata: what the writer does not keep, said and gone)
        CodecImage tagged = synth(8, 8, Layout::Rgb, Sample::U8);
        tagged.icc = std::string(200, 'p');
        tagged.meta.dpi = std::make_pair(300.0, 300.0);
        tagged.meta.xmp = std::string("<x:xmpmeta/>");
        tagged.meta.exif = std::string("II*\0", 4);
        tagged.meta.text = {{"Comment", "hello"}};
        const auto dropped = codecs::losses(tagged, Format::Png);
        for (const Loss::Kind k : {Loss::Kind::IccDropped, Loss::Kind::DpiDropped, Loss::Kind::XmpDropped, Loss::Kind::ExifDropped, Loss::Kind::TextDropped})
            QVERIFY(has(dropped, k));
        const CodecImage back = codecs::decode(codecs::encode(tagged, Format::Png));
        QVERIFY(!back.icc && !back.meta.dpi && !back.meta.xmp && !back.meta.exif && back.meta.text.empty());
        codecs::EncodeOptions bare;
        bare.embed_icc = false;
        bare.embed_metadata = false;
        QVERIFY(codecs::losses(tagged, Format::Png, bare).empty());
        // (what stops a write)
        const auto tiff = codecs::losses(synth(4, 4, Layout::Rgb, Sample::U8), Format::Tiff);
        QCOMPARE(tiff.size(), std::size_t(1));
        QVERIFY(tiff.front().fatal());
        QCOMPARE(tiff.front().text(), std::string("この版の Genko では、TIFF を書けません"));
        const CodecImage wide = CodecImage::blank(70000, 1, Layout::Rgb, Sample::U8);
        const auto too_wide = codecs::losses(wide, Format::Jpeg);
        QVERIFY(has(too_wide, Loss::Kind::DimensionsExceeded) && too_wide.front().fatal());
        QCOMPARE(code_of([&] { (void)codecs::encode(wide, Format::Jpeg); }), std::string("image_too_large"));
        QCOMPARE(code_of([] { (void)codecs::encode(CodecImage(), Format::Png); }), std::string("value"));
    }

    // The formats this build reads through its own readers: the same pixels as they give, at the file's depth.
    void readsThroughThisBuildsReaders() {
        // (8-bit PNGs of every Pillow mode — palettes and a transparent colour as an alpha — and a 16-bit one)
        for (const char* name : {"rgb8.png", "rgba8.png", "l8.png", "la8.png", "p8.png", "p8_trns_index.png", "rgb_trns.png", "l_trns.png", "bit1.png"}) {
            const std::string bytes = data_file((std::string("pyref/png/") + name).c_str());
            QVERIFY2(!bytes.empty(), name);
            const CodecImage p = codecs::decode(bytes);
            const genko::render::Image pillow = genko::render::open_image(bytes);
            const bool alpha = codecs::has_alpha(p.layout());
            const std::string expected =
                codecs::is_gray(p.layout()) ? pillow.convert(alpha ? "LA" : "L").tobytes() : pillow.convert(alpha ? "RGBA" : "RGB").tobytes();
            QVERIFY2(p.data() == expected, name);
        }
        const CodecImage sixteen = codecs::decode(data_file("pyref/png/rgb16.png"));
        QCOMPARE(sixteen.sample(), Sample::U16);
        QCOMPARE(sixteen.layout(), Layout::Rgb);
        // (a JPEG: grey and CMYK; a BMP; a GIF's first frame)
        QCOMPARE(codecs::decode(data_file("pyref/jpeg/gray.jpg")).layout(), Layout::Gray);
        const std::string cmyk_bytes = data_file("pyref/jpeg/cmyk.jpg");
        const CodecImage cmyk = codecs::decode(cmyk_bytes);
        QCOMPARE(cmyk.layout(), Layout::Cmyk);
        QVERIFY(cmyk.data() == genko::render::open_image(cmyk_bytes).tobytes());
        QCOMPARE(codecs::decode(data_file("pyref/bmp/rgb24.bmp")).layout(), Layout::Rgb);
        QCOMPARE(codecs::decode(data_file("pyref/gif/animated-first-frame.gif")).sample(), Sample::U8);
        // (TIFFs: 8 bits as Pillow reads them; 16 bits with all their bits — RGB values and grey inks — and their resolution)
        const genko::render::Image rgb = genko::render::Image::create("RGB", genko::render::Size{9, 5}, genko::render::Ink{10, 200, 30});
        const CodecImage tiff8 = codecs::decode(genko::formats::tiff_bytes(rgb, genko::formats::TiffSave{"tiff_lzw", std::array<double, 2>{300, 300}, {}}));
        QCOMPARE(tiff8.layout(), Layout::Rgb);
        QVERIFY(tiff8.data() == rgb.tobytes());
        QCOMPARE(tiff8.meta.dpi, (std::optional<std::pair<double, double>>(std::make_pair(300.0, 300.0))));
        const CodecImage values = synth(11, 7, Layout::Rgba, Sample::U16, 5);
        const std::string gkcr = genko::core::encode_color_pixels(11, 7, "u16", [&](std::size_t i) {
            return std::array<double, 4>{values.value(i * 4), values.value(i * 4 + 1), values.value(i * 4 + 2), 1.0};
        });
        const CodecImage tiff16 = codecs::decode(genko::formats::values_tiff_bytes(genko::core::ColorRasterView(gkcr), 16, genko::formats::TiffSave{}));
        QCOMPARE(tiff16.sample(), Sample::U16);
        QCOMPARE(tiff16.layout(), Layout::Rgb);
        QVERIFY(tiff16.data() == values.convert(Layout::Rgb, Sample::U16).data());
        genko::core::InkPixels grey = genko::core::blank_ink_pixels(genko::core::InkSpace::Gray, 16, 6, 4);
        for (std::size_t i = 0; i < grey.pixel_count(); ++i) {
            grey.set(i, 0, static_cast<double>(i * 2731 % 65536) / 65535.0);
            grey.set(i, 1, 1.0);
        }
        const CodecImage tiff_grey = codecs::decode(genko::formats::ink_tiff_bytes(grey, genko::formats::TiffSave{}));
        QCOMPARE(tiff_grey.layout(), Layout::Gray);
        QCOMPARE(tiff_grey.sample(), Sample::U16);
        for (std::size_t i = 0; i < grey.pixel_count(); ++i) {
            std::uint16_t v = 0;
            std::memcpy(&v, tiff_grey.data().data() + i * 2, 2);
            QCOMPARE(v, grey.samples[i * 2]);
        }
        // (read as a format it is not: refused)
        QCOMPARE(code_of([&] { (void)codecs::decode_as(Format::Jpeg, data_file("pyref/png/rgb8.png")); }), std::string("format"));
        // (cut short or broken: an error, never a crash)
        const std::string png = data_file("pyref/png/rgb8.png");
        for (const std::size_t keep : {std::size_t(8), std::size_t(30), png.size() / 2, png.size() - 13}) {
            const std::string code = code_of([&] { (void)codecs::decode(png.substr(0, keep)); });
            QVERIFY2(code == "format" || code == "unidentified_image", (std::to_string(keep) + " " + code).c_str());
        }
    }

    // --- the limits (limits.rs) --------------------------------------------------------------------------------------

    void limitsBeforeThePixels() {
        for (const Format f : codecs::kFormats) {
            if (!codecs::caps(f).write) continue;
            const std::string bytes = codecs::encode(synth(64, 48, Layout::Rgba, Sample::U8), f);
            codecs::Limits l;
            l.max_pixels = 1000;
            QCOMPARE(code_of([&] { (void)codecs::decode(bytes, l); }), std::string("image_too_large"));
            l.max_pixels = 64 * 48;
            QCOMPARE(code_of([&] { (void)codecs::decode(bytes, l); }), std::string());
            l = {};
            l.max_width = 63;
            QCOMPARE(code_of([&] { (void)codecs::decode(bytes, l); }), std::string("image_too_large"));
            l = {};
            l.max_height = 47;
            QCOMPARE(code_of([&] { (void)codecs::decode(bytes, l); }), std::string("image_too_large"));
            l = {};
            l.max_alloc = 64 * 48;
            QVERIFY(!code_of([&] { (void)codecs::decode(bytes, l); }).empty());
            QCOMPARE(code_of([&] { (void)codecs::decode(bytes, codecs::Limits::none()); }), std::string());
        }
        // (a PNG whose header says 100000 × 100000 16-bit RGBA: refused on its header)
        std::string ihdr = be32(100000) + be32(100000) + std::string("\x10\x06\0\0\0", 5);
        const std::string bomb = std::string("\x89PNG\r\n\x1a\n", 8) + png_chunk("IHDR", ihdr) +
                                 png_chunk("IDAT", std::string("\x78\x9C\x03\x00\x00\x00\x00\x01", 8)) + png_chunk("IEND", {});
        QCOMPARE(code_of([&] { (void)codecs::decode(bomb); }), std::string("image_too_large"));
        // (a TIFF saying 1000000 × 1000000 grey)
        std::string tiff("II*\0\x08\0\0\0", 8);
        const std::array<std::array<std::uint32_t, 4>, 6> entries{{{256, 4, 1, 1000000}, {257, 4, 1, 1000000}, {258, 3, 1, 8}, {262, 3, 1, 1}, {273, 4, 1, 0}, {279, 4, 1, 0}}};
        const auto le = [&](std::uint32_t v, int n) {
            for (int i = 0; i < n; ++i) tiff.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
        };
        le(static_cast<std::uint32_t>(entries.size()), 2);
        for (const auto& e : entries) {
            le(e[0], 2);
            le(e[1], 2);
            le(e[2], 4);
            le(e[3], 4);
        }
        le(0, 4);
        QCOMPARE(code_of([&] { (void)codecs::decode(tiff); }), std::string("image_too_large"));
        // (a JPEG whose frame says 60000 × 60000, its scan's header and no scan data — this build's reader takes the size from
        // a header that reaches its scan, and libjpeg's sides stop at 65500)
        const std::string jpeg("\xFF\xD8\xFF\xC0\x00\x11\x08\xEA\x60\xEA\x60\x03\x01\x11\x00\x02\x11\x00\x03\x11\x00"
                               "\xFF\xDA\x00\x0C\x03\x01\x00\x02\x11\x03\x11\x00\x3F\x00\xFF\xD9", 37);
        codecs::Limits l;
        l.max_pixels = 1 << 24;
        QCOMPARE(code_of([&] { (void)codecs::decode(jpeg, l); }), std::string("image_too_large"));
        // (the check itself)
        const codecs::Limits defaults;
        QCOMPARE(code_of([&] { defaults.check(100, 100, Layout::Rgba, Sample::F32); }), std::string());
        QCOMPARE(code_of([&] { defaults.check(0, 1, Layout::Gray, Sample::U8); }), std::string("value"));
        QCOMPARE(code_of([&] { defaults.check(std::int64_t(1) << 32, 1, Layout::Gray, Sample::U8); }), std::string("image_too_large"));
        codecs::Limits small;
        small.max_alloc = 399;
        QCOMPARE(code_of([&] { small.check(10, 10, Layout::Rgba, Sample::U8); }), std::string("image_too_large"));
        QCOMPARE(code_of([&] { small.check(10, 10, Layout::Rgb, Sample::U8); }), std::string());
        // (Genko's own bounds: an op's picture, the work's 2 GiB)
        QCOMPARE(defaults.max_pixels, std::int64_t(120'000'000));
        QCOMPARE(defaults.max_width, std::int64_t(100'000));
        QCOMPARE(defaults.max_alloc, std::uint64_t(2) << 30);
    }
};

QTEST_GUILESS_MAIN(CodecsTest)
#include "test_codecs.moc"
