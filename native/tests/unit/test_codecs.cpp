// render/codecs (PC4 F-0 and F-a, SPEC.md FILE-01: the codecs) against PhotoCraft v0.2.0's own tests of crates/codecs
// (tests/detect_caps.rs: the magic numbers, the extensions and the capability table with its symmetric guarantee;
// tests/fidelity.rs: the losses said; tests/limits.rs: the bounds checked before the pixels are allocated; src/image.rs's
// tests: the picture's conversions; tests/roundtrip.rs: every format written × 4 samples × 6 layouts read back — the same
// values where the format is lossless, within a PSNR where it is not —; tests/metadata.rs: the profile, EXIF, XMP,
// resolution and text kept as each format's caps say; tests/malformed.rs: files cut short, bits flipped, bytes overwritten
// and random bytes read without a crash or a large allocation; tests/png_jpeg.rs: Adam7) on this build's table — every
// format read and written: PNG, JPEG, TIFF, WebP, GIF and BMP read through this build's readers, TGA, ICO, Netpbm, QOI,
// Radiance HDR here, OpenEXR with OpenEXR's library and AVIF with libavif and libaom —, the losses said exactly when the
// round trip changes the picture, and the drawing's 8-bit reader (open_image) reading the codecs' new formats.

#include <QtTest>

#include <array>
#include <cmath>
#include <cstring>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "core/color_raster.hpp"
#include "core/error.hpp"
#include "core/ink.hpp"
#include "formats/pillow_save.hpp"
#include "core/color_model.hpp"
#include "render/codecs/codecs.hpp"
#include "render/codecs/metadata.hpp"
#include "render/image.hpp"
#include "render/not_yet_ported.hpp"
#include "render/png.hpp"
#include "render/proof.hpp"

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


// --- PhotoCraft's tests/common (MIT): SplitMix64, its gradient pictures, its PSNR, its EXIF, ICC and XMP --------------

struct Rng {
    std::uint64_t state;
    explicit Rng(std::uint64_t seed) : state(seed) {}
    std::uint64_t next() {
        state += 0x9E3779B97F4A7C15ull;
        std::uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    float unit() { return static_cast<float>(next() >> 40) / static_cast<float>(1u << 24); }  // ([0, 1))
    std::string bytes(std::size_t n) {
        std::string out(n, '\0');
        for (char& c : out) c = static_cast<char>(next() & 0xff);
        return out;
    }
};

// A gradient with noise of `noise`, its alpha a gradient of its own from clear to opaque (common::synth).
CodecImage gradient(std::uint32_t w, std::uint32_t h, Layout layout, Sample sample, std::uint64_t seed, float noise) {
    Rng rng(seed);
    const unsigned nc = codecs::channels(layout);
    std::vector<float> v;
    v.reserve(std::size_t(w) * h * nc);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const float fx = static_cast<float>(x) / static_cast<float>(std::max(w, 2u) - 1), fy = static_cast<float>(y) / static_cast<float>(std::max(h, 2u) - 1);
            for (unsigned c = 0; c < nc; ++c) {
                const bool alpha = codecs::has_alpha(layout) && c == nc - 1;
                float base = 0;
                if (alpha) {
                    base = std::clamp((fx + fy) * 0.75f - 0.25f, 0.0f, 1.0f);
                } else {
                    switch (c % 4) {
                    case 0: base = fx; break;
                    case 1: base = fy; break;
                    case 2: base = 1.0f - fx * 0.5f - fy * 0.5f; break;
                    default: base = std::sqrt(fx * fy); break;
                    }
                }
                const float n = (rng.unit() - 0.5f) * noise;
                v.push_back(std::clamp(base + n, 0.0f, 1.0f));
            }
        }
    }
    return CodecImage::from_normalized(w, h, layout, sample, v);
}

CodecImage test_image(Layout layout, Sample sample) { return gradient(37, 23, layout, sample, 0xC0DEC, 0.2f); }

// PSNR over the samples clamped to 0..1 (common::psnr).
double clamped_psnr(const CodecImage& a, const CodecImage& b) {
    const std::vector<float> x = a.normalized(), y = b.normalized();
    if (x.size() != y.size() || x.empty()) return 0;
    double sum = 0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double d = std::clamp(x[i], 0.0f, 1.0f) - std::clamp(y[i], 0.0f, 1.0f);
        sum += d * d;
    }
    const double mse = sum / static_cast<double>(x.size());
    return mse == 0 ? 1e9 : 10.0 * std::log10(1.0 / mse);
}

// A TIFF-shaped EXIF block of one IFD with its orientation (common::sample_exif).
std::string sample_exif() {
    std::string v("II*\0\x08\0\0\0", 8);
    v += std::string("\x01\x00", 2);              // (one entry)
    v += std::string("\x12\x01\x03\x00", 4);      // (Orientation, SHORT)
    v += std::string("\x01\0\0\0\x01\0\0\0", 8);  // (count 1, value 1)
    v += std::string(4, '\0');                    // (no next IFD)
    return v;
}

// A stand-in ICC profile of `len` bytes: its size first, "acsp" at 36, noise after (common::sample_icc).
std::string sample_icc(std::size_t len) {
    std::string v = Rng(len).bytes(len);
    for (int i = 0; i < 4; ++i) v[static_cast<std::size_t>(i)] = static_cast<char>((len >> (24 - 8 * i)) & 0xff);
    if (len >= 40) v.replace(36, 4, "acsp");
    return v;
}

const std::string kSampleXmp =
    "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?><x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF "
    "xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\"/></x:xmpmeta><?xpacket end=\"w\"?>";

bool holds(std::span<const Layout> list, Layout l) { return std::find(list.begin(), list.end(), l) != list.end(); }
bool holds(std::span<const Sample> list, Sample s) { return std::find(list.begin(), list.end(), s) != list.end(); }

std::vector<Format> writable() {
    std::vector<Format> out;
    for (const Format f : codecs::kFormats) {
        if (codecs::caps(f).write && codecs::caps(f).read) out.push_back(f);
    }
    return out;
}

std::string label(Format f, Layout l, Sample s) {
    return std::string(codecs::name(f)) + " " + std::string(codecs::to_string(l)) + " " + std::string(codecs::to_string(s));
}

// Decoding under tight limits, as malformed.rs does: anything may be thrown, nothing may crash.
void no_crash(Format f, std::string_view bytes) {
    codecs::Limits tight;
    tight.max_width = 4096;
    tight.max_height = 4096;
    tight.max_pixels = 1 << 22;
    tight.max_alloc = std::uint64_t(64) << 20;
    try {
        (void)codecs::decode(bytes, tight);
    } catch (const std::exception&) {
    }
    try {
        (void)codecs::decode_as(f, bytes, tight);
    } catch (const std::exception&) {
    }
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
        // (PC4 F-a: every format read and written, none asymmetric; AVIF read too — PhotoCraft's writes it only)
        for (const Format f : codecs::kFormats) QVERIFY2(codecs::caps(f).read && codecs::caps(f).write, std::string(codecs::name(f)).c_str());
        QVERIFY(codecs::asymmetric_formats().empty());
        // (the rows this build's writers widen or narrow from PhotoCraft's)
        QVERIFY(codecs::caps(Format::Bmp).dpi);
        QVERIFY(codecs::caps(Format::Avif).icc && codecs::caps(Format::Avif).exif && codecs::caps(Format::Avif).xmp);
        QVERIFY(!codecs::caps(Format::Tiff).exif);
        QVERIFY(!codecs::caps(Format::WebP).lossy);
        QCOMPARE(codecs::max_dimensions(Format::Jpeg), (std::optional<std::pair<std::uint32_t, std::uint32_t>>(std::make_pair(65500u, 65500u))));
        QCOMPARE(codecs::max_dimensions(Format::WebP), (std::optional<std::pair<std::uint32_t, std::uint32_t>>(std::make_pair(16383u, 16383u))));
        QVERIFY(codecs::linear_light(Format::OpenExr) && codecs::linear_light(Format::Hdr) && !codecs::linear_light(Format::Tiff));
        for (const Format f : {Format::Tga, Format::Ico, Format::Pnm, Format::Qoi, Format::OpenExr, Format::Hdr, Format::Avif}) QVERIFY(codecs::read_here(f));
        for (const Format f : {Format::Png, Format::Jpeg, Format::Tiff, Format::WebP, Format::Gif, Format::Bmp}) QVERIFY(!codecs::read_here(f));
        // (the libraries named for `genko --version`)
        QCOMPARE(codecs::openexr_version().substr(0, 2), std::string("3."));
        QCOMPARE(codecs::libavif_version().substr(0, 2), std::string("1."));
        QVERIFY2(!codecs::aom_version().empty() && std::isdigit(static_cast<unsigned char>(codecs::aom_version()[0])), codecs::aom_version().c_str());
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
                                      has(losses, Loss::Kind::AlphaDiscarded) || has(losses, Loss::Kind::AlphaBinarized) ||
                                      has(losses, Loss::Kind::CmykConverted) || has(losses, Loss::Kind::ColorToGray) ||
                                      has(losses, Loss::Kind::PaletteQuantized) || has(losses, Loss::Kind::LossyCompression);
                    const std::string what = std::string(codecs::name(f)) + " " + std::string(codecs::to_string(l)) + " " + std::string(codecs::to_string(s));
                    QVERIFY2(said != pixels_kept, what.c_str());
                }
            }
        }
    }

    void lossesSaid() {
        const CodecImage deep = synth(8, 8, Layout::Rgb, Sample::U16);
        // (PC4 F-a: PNG keeps 16 bits; QOI holds 8)
        QVERIFY(codecs::losses(deep, Format::Png).empty());
        const auto to_qoi = codecs::losses(deep, Format::Qoi);
        QVERIFY(has(to_qoi, Loss::Kind::DepthReduced));
        QCOMPARE(to_qoi.front().text(), std::string("16 bit を 8 bit にします"));
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
        // (the metadata: what the writer does not keep — QOI keeps none —, said and gone)
        CodecImage tagged = synth(8, 8, Layout::Rgb, Sample::U8);
        tagged.icc = std::string(200, 'p');
        tagged.meta.dpi = std::make_pair(300.0, 300.0);
        tagged.meta.xmp = std::string("<x:xmpmeta/>");
        tagged.meta.exif = std::string("II*\0", 4);
        tagged.meta.text = {{"Comment", "hello"}};
        const auto dropped = codecs::losses(tagged, Format::Qoi);
        for (const Loss::Kind k : {Loss::Kind::IccDropped, Loss::Kind::DpiDropped, Loss::Kind::XmpDropped, Loss::Kind::ExifDropped, Loss::Kind::TextDropped})
            QVERIFY(has(dropped, k));
        const CodecImage back = codecs::decode(codecs::encode(tagged, Format::Qoi));
        QVERIFY(!back.icc && !back.meta.dpi && !back.meta.xmp && !back.meta.exif && back.meta.text.empty());
        codecs::EncodeOptions bare;
        bare.embed_icc = false;
        bare.embed_metadata = false;
        QVERIFY(codecs::losses(tagged, Format::Qoi, bare).empty());
        // (PNG keeps them all, TIFF its seven text tags only, JPEG an EXIF block of one segment)
        QVERIFY(codecs::losses(tagged, Format::Png).empty());
        QVERIFY(has(codecs::losses(tagged, Format::Tiff), Loss::Kind::TextDropped));
        tagged.meta.text = {{"Copyright", "© 2026"}};
        QVERIFY(!has(codecs::losses(tagged, Format::Tiff), Loss::Kind::TextDropped));
        tagged.meta.exif = std::string("II*\0", 4) + std::string(70000, 'e');
        QVERIFY(has(codecs::losses(tagged, Format::Jpeg), Loss::Kind::ExifDropped));
        QVERIFY(!codecs::decode(codecs::encode(tagged, Format::Jpeg)).meta.exif);
        // (a lossy WebP when asked)
        codecs::EncodeOptions lossy;
        lossy.webp_lossless = false;
        QVERIFY(!has(codecs::losses(synth(8, 8, Layout::Rgb, Sample::U8), Format::WebP), Loss::Kind::LossyCompression));
        QVERIFY(has(codecs::losses(synth(8, 8, Layout::Rgb, Sample::U8), Format::WebP, lossy), Loss::Kind::LossyCompression));
        // (GIF: its colours as they are when they are 256 or fewer, its alpha all or nothing)
        CodecImage few = CodecImage::blank(20, 20, Layout::Rgba, Sample::U8);
        for (std::size_t i = 0; i < few.pixel_count(); ++i) {
            few.data()[i * 4] = static_cast<char>(i % 200);
            few.data()[i * 4 + 3] = static_cast<char>(i % 7 == 0 ? 0 : 255);
        }
        QVERIFY(codecs::losses(few, Format::Gif).empty());
        few.data()[3] = static_cast<char>(100);
        QVERIFY(has(codecs::losses(few, Format::Gif), Loss::Kind::AlphaBinarized));
        QVERIFY(has(codecs::losses(synth(40, 40, Layout::Rgb, Sample::U8), Format::Gif), Loss::Kind::PaletteQuantized));
        // (OpenEXR's linear light: the built-in linear-srgb profile is its own, no loss said of it; another one dropped)
        CodecImage linear = synth(8, 8, Layout::Rgb, Sample::F32);
        linear.icc = *genko::core::builtin_profile("linear-srgb")->bytes;
        QVERIFY(!has(codecs::losses(linear, Format::OpenExr), Loss::Kind::IccDropped));
        linear.icc = *genko::core::builtin_profile("display-p3")->bytes;
        QVERIFY(has(codecs::losses(linear, Format::OpenExr), Loss::Kind::IccDropped));
        // (what stops a write)
        const auto icon = codecs::losses(synth(300, 10, Layout::Rgba, Sample::U8), Format::Ico);
        QVERIFY(has(icon, Loss::Kind::DimensionsExceeded) && icon.front().fatal());
        QCOMPARE(icon.front().text(), std::string("画像が ICO の大きさの上限（256×256 px）を超えるため、書けません"));
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
        // (10000 × 10000 16-bit RGBA: within the pixels, but its floats with the 16-bit picture made from them — 24 bytes a
        // pixel — are past the 2 GiB: refused on its header too, before any row is allocated)
        ihdr = be32(10000) + be32(10000) + std::string("\x10\x06\0\0\0", 5);
        const std::string deep = std::string("\x89PNG\r\n\x1a\n", 8) + png_chunk("IHDR", ihdr) +
                                 png_chunk("IDAT", std::string("\x78\x9C\x03\x00\x00\x00\x00\x01", 8)) + png_chunk("IEND", {});
        QCOMPARE(code_of([&] { (void)codecs::decode(deep); }), std::string("image_too_large"));
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

    // --- PC4 F-a: every format (PhotoCraft's tests/roundtrip.rs) ---------------------------------------------------------

    // Every format written × every sample × every layout, read back: the plan's layout and sample where the format holds
    // the picture's, the same values (the picture converted as planned) where it is lossless, within a PSNR where it is
    // not (JPEG 30 dB, GIF 20 — its colours compared where it is opaque —, Radiance HDR 40, AVIF 28).
    void everyFormatRoundTrips() {
        for (const Format f : writable()) {
            const codecs::FormatCaps c = codecs::caps(f);
            for (const Layout l : codecs::kLayouts) {
                for (const Sample s : codecs::kSamples) {
                    const std::string what = label(f, l, s);
                    const CodecImage img = f == Format::Gif ? gradient(37, 23, l, s, 7, 0.0f) : c.lossy ? gradient(37, 23, l, s, 7, 0.02f) : test_image(l, s);
                    std::string bytes;
                    QVERIFY2(code_of([&] { bytes = codecs::encode(img, f); }).empty(), what.c_str());
                    QVERIFY2(codecs::detect(bytes) == f, what.c_str());
                    CodecImage back;
                    QVERIFY2(code_of([&] { back = codecs::decode(bytes); }).empty(), what.c_str());
                    QVERIFY2(back.width() == img.width() && back.height() == img.height(), what.c_str());
                    const bool float_pnm = f == Format::Pnm && codecs::is_float(s);
                    if (holds(c.layouts, l) && holds(c.depths, s) && !float_pnm) QVERIFY2(back.layout() == l && back.sample() == s, what.c_str());
                    if (holds(c.depths, s) && !float_pnm) QVERIFY2(back.sample() == s, what.c_str());
                    if (s == Sample::U16 && holds(c.depths, Sample::U16)) QVERIFY2(back.sample() == Sample::U16 || back.sample() == Sample::F32, what.c_str());
                    CodecImage expected = img.convert(back.layout(), back.sample());
                    if (f == Format::Gif && codecs::has_alpha(l)) {  // (an alpha of all or nothing: colours compared where opaque)
                        std::vector<float> e = expected.normalized();
                        const std::vector<float> b = back.normalized();
                        const unsigned ch = codecs::channels(back.layout());
                        for (std::size_t p = 0; p < back.pixel_count(); ++p) {
                            if (b[p * ch + ch - 1] == 0.0f) std::copy_n(b.begin() + static_cast<std::ptrdiff_t>(p * ch), ch, e.begin() + static_cast<std::ptrdiff_t>(p * ch));
                            e[p * ch + ch - 1] = b[p * ch + ch - 1];
                        }
                        expected = CodecImage::from_normalized(img.width(), img.height(), back.layout(), back.sample(), e);
                    }
                    if (c.lossy) {
                        const double floor = f == Format::Jpeg ? 30.0 : f == Format::Gif ? 20.0 : f == Format::Hdr ? 40.0 : 28.0;
                        const double p = clamped_psnr(expected, back);
                        QVERIFY2(p >= floor, (what + " PSNR " + std::to_string(p)).c_str());
                    } else {
                        QVERIFY2(expected.data() == back.data(), what.c_str());
                    }
                }
            }
        }
    }

    // Float formats keep light past 1 and below 0 (TIFF, OpenEXR, PFM exactly); Radiance HDR within a hundredth of a
    // pixel's brightest channel; OpenEXR's halves bit for bit with each lossless compression.
    void highDynamicRangeKept() {
        std::vector<float> values(16 * 8 * 3);
        for (std::size_t i = 0; i < values.size(); ++i) values[i] = (static_cast<float>(i) - 40.0f) * 0.37f;
        const CodecImage hdr = CodecImage::from_normalized(16, 8, Layout::Rgb, Sample::F32, values);
        for (const Format f : {Format::Tiff, Format::OpenExr, Format::Pnm}) {
            const CodecImage back = codecs::decode(codecs::encode(hdr, f));
            QCOMPARE(back.sample(), Sample::F32);
            QVERIFY2(back.normalized() == values, std::string(codecs::name(f)).c_str());
        }
        std::vector<float> bright(16 * 8 * 3);
        for (std::size_t i = 0; i < bright.size(); ++i) bright[i] = 0.01f + static_cast<float>(i) * 0.25f;
        const std::vector<float> got = codecs::decode(codecs::encode(CodecImage::from_normalized(16, 8, Layout::Rgb, Sample::F32, bright), Format::Hdr)).normalized();
        for (std::size_t p = 0; p < bright.size(); p += 3) {
            const float most = std::max({bright[p], bright[p + 1], bright[p + 2]});
            for (std::size_t c = 0; c < 3; ++c) QVERIFY2(std::abs(bright[p + c] - got[p + c]) <= most * 0.01f, std::to_string(p).c_str());
        }
        std::string halves(20 * 10 * 4 * 2, '\0');
        for (std::size_t i = 0; i < 20 * 10 * 4; ++i) {
            const std::uint16_t h = codecs::half_from_float(static_cast<float>(i) * 0.013f - 1.0f);
            std::memcpy(halves.data() + i * 2, &h, 2);
        }
        const CodecImage half = CodecImage::from_bytes(20, 10, Layout::Rgba, Sample::F16, halves);
        for (const codecs::ExrCompression comp : {codecs::ExrCompression::None, codecs::ExrCompression::Rle, codecs::ExrCompression::Zip1,
                                                  codecs::ExrCompression::Zip16, codecs::ExrCompression::Piz}) {
            codecs::EncodeOptions o;
            o.exr_compression = comp;
            const CodecImage back = codecs::decode(codecs::encode(half, Format::OpenExr, o));
            QCOMPARE(back.sample(), Sample::F16);
            QVERIFY(back.data() == halves);
        }
    }

    void compressionsAndQualities() {
        // (TIFF: every layout and depth with each compression, the same picture back)
        for (const Layout l : codecs::kLayouts) {
            for (const Sample s : {Sample::U8, Sample::U16, Sample::F32}) {
                const CodecImage img = test_image(l, s);
                for (const codecs::TiffCompression comp : {codecs::TiffCompression::None, codecs::TiffCompression::Lzw, codecs::TiffCompression::Deflate,
                                                           codecs::TiffCompression::PackBits}) {
                    codecs::EncodeOptions o;
                    o.tiff_compression = comp;
                    const CodecImage back = codecs::decode(codecs::encode(img, Format::Tiff, o));
                    QVERIFY2(back.layout() == l && back.sample() == s && back.data() == img.data(), label(Format::Tiff, l, s).c_str());
                }
            }
        }
        // (PNG: each compression; Adam7 the same picture as without it, 8 and 16 bits of each layout)
        const CodecImage deep = test_image(Layout::Rgba, Sample::U16);
        for (const codecs::PngCompression comp : {codecs::PngCompression::None, codecs::PngCompression::Fast, codecs::PngCompression::Default,
                                                  codecs::PngCompression::Best}) {
            codecs::EncodeOptions o;
            o.png_compression = comp;
            QVERIFY(codecs::decode(codecs::encode(deep, Format::Png, o)).data() == deep.data());
        }
        for (const Layout l : {Layout::Gray, Layout::GrayA, Layout::Rgb, Layout::Rgba}) {
            for (const Sample s : {Sample::U8, Sample::U16}) {
                const CodecImage img = test_image(l, s);
                codecs::EncodeOptions adam7;
                adam7.png_interlaced = true;
                const std::string interlaced = codecs::encode(img, Format::Png, adam7);
                QCOMPARE(static_cast<int>(interlaced[28]), 1);  // (IHDR's interlace method)
                const CodecImage back = codecs::decode(interlaced);
                QVERIFY2(back.layout() == l && back.sample() == s && back.data() == img.data(), label(Format::Png, l, s).c_str());
                QVERIFY(back.data() == codecs::decode(codecs::encode(img, Format::Png)).data());
            }
        }
        // (JPEG: a lower quality is smaller and further off; 4:4:4 keeps colour detail 4:2:0 does not)
        const CodecImage rgb = test_image(Layout::Rgb, Sample::U8);
        codecs::EncodeOptions lo, hi;
        lo.jpeg_quality = 20;
        hi.jpeg_quality = 98;
        const std::string jl = codecs::encode(rgb, Format::Jpeg, lo), jh = codecs::encode(rgb, Format::Jpeg, hi);
        QVERIFY(jl.size() < jh.size());
        QVERIFY(clamped_psnr(rgb, codecs::decode(jh)) > clamped_psnr(rgb, codecs::decode(jl)));
        const CodecImage detail = gradient(64, 64, Layout::Rgb, Sample::U8, 3, 0.5f);
        codecs::EncodeOptions full, sub;
        full.jpeg_quality = sub.jpeg_quality = 95;
        full.jpeg_chroma_subsampling = false;
        QVERIFY(clamped_psnr(detail, codecs::decode(codecs::encode(detail, Format::Jpeg, full))) >
                clamped_psnr(detail, codecs::decode(codecs::encode(detail, Format::Jpeg, sub))));
        // (WebP: lossless by default, lossy when asked — smaller, close —; the colours under a clear pixel kept losslessly)
        codecs::EncodeOptions lossy;
        lossy.webp_lossless = false;
        lossy.jpeg_quality = 80;
        const CodecImage photo = gradient(64, 48, Layout::Rgb, Sample::U8, 5, 0.3f);
        const std::string wl = codecs::encode(photo, Format::WebP, lossy);
        QVERIFY(wl.size() < codecs::encode(photo, Format::WebP).size());
        QVERIFY(clamped_psnr(photo, codecs::decode(wl)) > 25.0);
        // (AVIF: a lower quality is smaller)
        codecs::EncodeOptions av_lo, av_hi;
        av_lo.jpeg_quality = 30;
        av_hi.jpeg_quality = 95;
        QVERIFY(codecs::encode(photo, Format::Avif, av_lo).size() < codecs::encode(photo, Format::Avif, av_hi).size());
    }

    void oddSizes() {
        // (one pixel; nothing; an icon past 256; a larger 16-bit picture exactly)
        for (const Format f : writable()) {
            const CodecImage one = gradient(1, 1, Layout::Rgb, Sample::U8, 1, 0.0f);
            CodecImage back;
            QVERIFY2(code_of([&] { back = codecs::decode(codecs::encode(one, f)); }).empty(), std::string(codecs::name(f)).c_str());
            QVERIFY2(back.width() == 1 && back.height() == 1, std::string(codecs::name(f)).c_str());
            QVERIFY2(!code_of([&] { (void)codecs::encode(CodecImage(), f); }).empty(), std::string(codecs::name(f)).c_str());
        }
        QCOMPARE(code_of([] { (void)codecs::encode(CodecImage::blank(300, 10, Layout::Rgba, Sample::U8), Format::Ico); }), std::string("image_too_large"));
        QCOMPARE(code_of([] { (void)codecs::encode(CodecImage::blank(256, 256, Layout::Rgba, Sample::U8), Format::Ico); }), std::string());
        const CodecImage big = gradient(513, 257, Layout::Rgba, Sample::U16, 99, 1.0f);
        for (const Format f : {Format::Png, Format::Tiff, Format::Pnm}) QVERIFY2(codecs::decode(codecs::encode(big, f)).data() == big.data(), std::string(codecs::name(f)).c_str());
        // (an icon of several sizes: its largest read)
        const std::array<CodecImage, 3> sizes{gradient(16, 16, Layout::Rgba, Sample::U8, 1, 0.1f), gradient(48, 32, Layout::Rgba, Sample::U8, 2, 0.1f),
                                              gradient(32, 32, Layout::Rgba, Sample::U8, 3, 0.1f)};
        const CodecImage largest = codecs::decode(codecs::encode_icon(sizes));
        QVERIFY(largest.width() == 48 && largest.height() == 32 && largest.data() == sizes[1].data());
    }

    // --- metadata (tests/metadata.rs) -------------------------------------------------------------------------------

    // The profile, EXIF, XMP, resolution and text of a picture are back as each format's caps say, and what it cannot keep
    // is said (OpenEXR and Radiance HDR give their own profile, the built-in linear-srgb, to an RGB picture).
    void metadataKeptAsCapsSay() {
        const std::string& linear = *genko::core::builtin_profile("linear-srgb")->bytes;
        for (const Format f : writable()) {
            const codecs::FormatCaps c = codecs::caps(f);
            const Layout l = holds(c.layouts, Layout::Rgb) ? Layout::Rgb : c.layouts.front();
            CodecImage img = test_image(l, Sample::U8);
            img.icc = sample_icc(3000);
            img.meta.exif = sample_exif();
            img.meta.xmp = kSampleXmp;
            img.meta.dpi = std::make_pair(300.0, 150.0);
            img.meta.text = {{"Description", "synthetic test image"}, {"Software", "photocraft"}};
            const std::string name(codecs::name(f));
            const CodecImage back = codecs::decode(codecs::encode(img, f));
            const auto said = codecs::losses(img, f);
            if (c.icc) {
                QVERIFY2(back.icc == img.icc, name.c_str());
            } else {
                QVERIFY2(has(said, Loss::Kind::IccDropped), name.c_str());
                QVERIFY2(codecs::linear_light(f) ? back.icc == linear : !back.icc, name.c_str());
            }
            if (c.exif) QVERIFY2(back.meta.exif == img.meta.exif, name.c_str());
            else QVERIFY2(has(said, Loss::Kind::ExifDropped) && !back.meta.exif, name.c_str());
            if (c.xmp) QVERIFY2(back.meta.xmp == img.meta.xmp, name.c_str());
            else QVERIFY2(has(said, Loss::Kind::XmpDropped) && !back.meta.xmp, name.c_str());
            if (c.dpi) {
                QVERIFY2(back.meta.dpi && std::abs(back.meta.dpi->first - 300.0) < 0.05 && std::abs(back.meta.dpi->second - 150.0) < 0.05, name.c_str());
            } else {
                QVERIFY2(has(said, Loss::Kind::DpiDropped) && !back.meta.dpi, name.c_str());
            }
            if (c.text) {
                for (const auto& kv : img.meta.text) QVERIFY2(std::find(back.meta.text.begin(), back.meta.text.end(), kv) != back.meta.text.end(), name.c_str());
            } else {
                QVERIFY2(has(said, Loss::Kind::TextDropped) && back.meta.text.empty(), name.c_str());
            }
        }
    }

    void metadataEachWay() {
        // (16-bit PNG and TIFF keep the profile; CMYK TIFF and JPEG keep a CMYK profile; a CMYK profile is not put on RGB)
        for (const Format f : {Format::Png, Format::Tiff}) {
            CodecImage img = test_image(Layout::Rgba, Sample::U16);
            img.icc = sample_icc(512);
            img.meta.dpi = std::make_pair(72.0, 72.0);
            const CodecImage back = codecs::decode(codecs::encode(img, f));
            QVERIFY(back.icc == img.icc && back.sample() == Sample::U16);
        }
        for (const Format f : {Format::Tiff, Format::Jpeg}) {
            CodecImage img = test_image(Layout::Cmyk, Sample::U8);
            img.icc = sample_icc(900);
            const CodecImage back = codecs::decode(codecs::encode(img, f));
            QVERIFY(back.layout() == Layout::Cmyk && back.icc == img.icc);
        }
        CodecImage cmyk = test_image(Layout::Cmyk, Sample::U8);
        cmyk.icc = sample_icc(900);
        QVERIFY(!codecs::decode(codecs::encode(cmyk, Format::Png)).icc);
        QVERIFY(has(codecs::losses(cmyk, Format::Png), Loss::Kind::IccDropped));
        // (a JPEG's profile in as many APP2 segments as it takes)
        for (const std::size_t len : {std::size_t(65519), std::size_t(65520), std::size_t(150000), std::size_t(300001)}) {
            CodecImage img = test_image(Layout::Rgb, Sample::U8);
            img.icc = sample_icc(len);
            const std::string bytes = codecs::encode(img, Format::Jpeg);
            std::size_t app2 = 0;
            for (std::size_t at = bytes.find("\xFF\xE2"); at != std::string::npos; at = bytes.find("\xFF\xE2", at + 2)) {
                if (bytes.compare(at + 4, 10, "ICC_PROFIL") == 0) ++app2;
            }
            QCOMPARE(app2, (len + 65518) / 65519);
            QCOMPARE(codecs::decode(bytes).icc->size(), len);
            QCOMPARE(genko::render::proof::embedded_profile(bytes), img.icc);
        }
        // (what the options leave out is not written, and not said)
        CodecImage rich = test_image(Layout::Rgb, Sample::U8);
        rich.icc = sample_icc(3000);
        rich.meta.exif = sample_exif();
        rich.meta.xmp = kSampleXmp;
        rich.meta.text = {{"Description", "x"}};
        codecs::EncodeOptions bare;
        bare.embed_icc = false;
        bare.embed_metadata = false;
        for (const Format f : {Format::Png, Format::Jpeg, Format::Tiff, Format::WebP, Format::Avif}) {
            const CodecImage back = codecs::decode(codecs::encode(rich, f, bare));
            QVERIFY2(!back.icc && !back.meta.exif && !back.meta.xmp && back.meta.text.empty(), std::string(codecs::name(f)).c_str());
        }
        for (const Loss& l : codecs::losses(rich, Format::Bmp, bare)) QVERIFY(l.kind != Loss::Kind::IccDropped && l.kind != Loss::Kind::ExifDropped);
        // (PNG: Unicode text in iTXt, Latin-1 in tEXt; the XMP in its own iTXt, not among the text; EXIF in eXIf, no iCCP
        // without a profile)
        CodecImage text = test_image(Layout::Rgb, Sample::U8);
        text.meta.text = {{"Title", "Ünïcödé ☃ 雪"}, {"Author", "latin1 ok é"}};
        std::string bytes = codecs::encode(text, Format::Png);
        QVERIFY(bytes.find("iTXt") != std::string::npos && bytes.find("tEXt") != std::string::npos);
        auto got = codecs::decode(bytes).meta.text;
        auto want = text.meta.text;
        std::sort(got.begin(), got.end());
        std::sort(want.begin(), want.end());
        QVERIFY(got == want);
        CodecImage xmp = test_image(Layout::Gray, Sample::U8);
        xmp.meta.xmp = kSampleXmp;
        bytes = codecs::encode(xmp, Format::Png);
        QVERIFY(bytes.find("XML:com.adobe.xmp") != std::string::npos);
        QVERIFY(codecs::decode(bytes).meta.xmp == kSampleXmp && codecs::decode(bytes).meta.text.empty());
        CodecImage exif = test_image(Layout::Rgb, Sample::U8);
        exif.meta.exif = sample_exif();
        bytes = codecs::encode(exif, Format::Png);
        QVERIFY(bytes.find("eXIf") != std::string::npos && bytes.find("iCCP") == std::string::npos);
        QCOMPARE(codecs::read_metadata(Format::Png, bytes).exif, std::optional<std::string>(sample_exif()));
        QCOMPARE(codecs::read_metadata(Format::Jpeg, codecs::encode(exif, Format::Jpeg)).exif, std::optional<std::string>(sample_exif()));
        // (JPEG: the resolution in JFIF's density, two axes)
        CodecImage density = test_image(Layout::Gray, Sample::U8);
        density.meta.dpi = std::make_pair(96.0, 200.0);
        QCOMPARE(codecs::decode(codecs::encode(density, Format::Jpeg)).meta.dpi, (std::optional<std::pair<double, double>>(std::make_pair(96.0, 200.0))));
        // (a depth converted keeps the profile and the XMP)
        CodecImage floats = test_image(Layout::Rgb, Sample::F32);
        floats.icc = sample_icc(300);
        floats.meta.xmp = kSampleXmp;
        const CodecImage back = codecs::decode(codecs::encode(floats, Format::Png));
        QVERIFY(back.icc == floats.icc && back.meta.xmp == floats.meta.xmp);
    }

    // --- malformed input (tests/malformed.rs) --------------------------------------------------------------------------

    void malformedNeverCrashes() {
        std::vector<std::pair<Format, std::string>> samples;
        for (const Format f : writable()) {
            for (const auto& [l, s] : {std::make_pair(Layout::Rgba, Sample::U8), std::make_pair(Layout::Gray, Sample::U16), std::make_pair(Layout::Rgb, Sample::F32)}) {
                CodecImage img = gradient(19, 13, l, s, 11, 0.3f);
                img.icc = sample_icc(300);
                img.meta.exif = sample_exif();
                img.meta.xmp = kSampleXmp;
                samples.emplace_back(f, codecs::encode(img, f));
            }
        }
        codecs::EncodeOptions adam7;
        adam7.png_interlaced = true;
        samples.emplace_back(Format::Png, codecs::encode(gradient(19, 13, Layout::Rgb, Sample::U8, 1, 0.3f), Format::Png, adam7));
        samples.emplace_back(Format::Pnm, std::string("P3\n2 2\n255\n0 1 2 3 4 5 6 7 8 9 10 11\n"));
        samples.emplace_back(Format::Jpeg, codecs::encode(gradient(19, 13, Layout::Cmyk, Sample::U8, 1, 0.3f), Format::Jpeg));
        // (cut at many lengths: the first 80, every n / 97th, the last 40)
        for (const auto& [f, bytes] : samples) {
            const std::size_t n = bytes.size(), step = std::max<std::size_t>(n / 97, 1);
            std::vector<std::size_t> cuts;
            for (std::size_t i = 0; i < std::min<std::size_t>(n, 80); ++i) cuts.push_back(i);
            for (std::size_t i = 0; i < n; i += step) cuts.push_back(i);
            for (std::size_t i = n > 40 ? n - 40 : 0; i < n; ++i) cuts.push_back(i);
            for (const std::size_t cut : cuts) no_crash(f, std::string_view(bytes).substr(0, cut));
        }
        // (half a file of a format whose pixels are counted is an error, never a picture)
        for (const auto& [f, bytes] : samples) {
            if (f != Format::Png && f != Format::Pnm && f != Format::Qoi && f != Format::OpenExr && f != Format::Tiff && f != Format::Bmp) continue;
            codecs::Limits tight;
            tight.max_pixels = 1 << 22;
            QVERIFY2(!code_of([&] { (void)codecs::decode_as(f, std::string_view(bytes).substr(0, bytes.size() / 2), tight); }).empty(),
                     std::string(codecs::name(f)).c_str());
        }
        // (bits flipped; bytes of the header overwritten)
        Rng rng(42);
        for (const auto& [f, bytes] : samples) {
            for (int k = 0; k < 150; ++k) {
                std::string b = bytes;
                const int flips = 1 + static_cast<int>(rng.next() % 4);
                for (int i = 0; i < flips; ++i) b[rng.next() % b.size()] ^= static_cast<char>(1u << (rng.next() % 8));
                no_crash(f, b);
            }
            for (std::size_t i = 0; i < std::min<std::size_t>(bytes.size(), 64); ++i) {
                for (const char v : {'\0', '\x7f', '\xff'}) {
                    std::string b = bytes;
                    b[i] = v;
                    no_crash(f, b);
                }
            }
        }
        // (random bytes, with and without a format's magic number first; nothing at all)
        const std::array<std::string, 13> magics{std::string("\x89PNG\r\n\x1a\n", 8), std::string("\xFF\xD8\xFF"), std::string("II*\0", 4),
                                                 std::string("RIFF\0\0\0\0WEBP", 12), std::string("GIF89a"), std::string("BM"),
                                                 std::string("\0\0\1\0\1\0", 6), std::string("P6\n"), std::string("qoif"),
                                                 std::string("\x76\x2F\x31\x01"), std::string("#?RADIANCE\n"), std::string("P7\n"), std::string("PF\n")};
        for (int k = 0; k < 400; ++k) {
            const std::string data = rng.bytes(rng.next() % 2048);
            no_crash(Format::Png, data);
            const Format f = codecs::kFormats[rng.next() % codecs::kFormats.size()];
            no_crash(f, magics[rng.next() % magics.size()] + rng.bytes(rng.next() % 1024));
            no_crash(f, rng.bytes(rng.next() % 512));
        }
        for (const Format f : codecs::kFormats) QVERIFY2(!code_of([&] { (void)codecs::decode_as(f, ""); }).empty(), std::string(codecs::name(f)).c_str());
        // (one byte replaced and the file cut, at random)
        for (int k = 0; k < 400; ++k) {
            const auto& [f, bytes] = samples[rng.next() % samples.size()];
            std::string b = bytes;
            const std::size_t at = rng.next() % b.size();
            b[at] = static_cast<char>(rng.next() & 0xff);
            const std::size_t cut = std::max<std::size_t>(rng.next() % (b.size() + 1), at + 1);
            no_crash(f, std::string_view(b).substr(0, std::min(cut, b.size())));
        }
    }

    void netpbmAsItIsWritten() {
        // (broken: each refused)
        for (const std::string& bad : {std::string("P6\n"), std::string("P6\n2 2\n"), std::string("P6\n2 2\n255"), std::string("P6\n2 2\n0\n"),
                                       std::string("P6\n2 2\n70000\n"), std::string("P6\n-2 2\n255\n"), std::string("P6\n2 2\n255\n\x01\x02"),
                                       std::string("P5\n2 2\n65535\n\0\0\0", 16), std::string("P2\n2 2\n10\n1 2 3 11\n"), std::string("P1\n2 2\n0 1 2 0\n"),
                                       std::string("P4\n9 2\n\0", 8), std::string("P7\nWIDTH 2\nHEIGHT 2\nDEPTH 9\nMAXVAL 255\nENDHDR\n"),
                                       std::string("P7\nWIDTH 2\nHEIGHT 2\nMAXVAL 255\nENDHDR\n"), std::string("P7\nWIDTH 2\nBOGUS 3\nENDHDR\n"),
                                       std::string("PF\n2 2\n0.0\n"), std::string("PF\n2 2\nnan\n"), std::string("PF\n2 2\n-1.0\n\0\0\0\0", 16),
                                       std::string("P6 4294967295 4294967295 255\n")})
            QVERIFY2(!code_of([&] { (void)codecs::decode(bad); }).empty(), bad.c_str());
        // (read: text and bytes, a comment, maxvals other than 255 and 65535, PAM's tuple types, PFM's rows from the bottom)
        QCOMPARE(codecs::decode("P1\n# comment\n3 2\n1 0 1\n0 1 0\n").data(), std::string("\0\xff\0\xff\0\xff", 6));
        QCOMPARE(codecs::decode("P1\n3 2\n101010").data(), std::string("\0\xff\0\xff\0\xff", 6));
        QCOMPARE(codecs::decode(std::string("P4\n3 2\n\xA0\x40", 9)).data(), std::string("\0\xff\0\xff\0\xff", 6));
        QCOMPARE(codecs::decode("P2\n2 1\n15\n0 15\n").data(), std::string("\0\xff", 2));
        const CodecImage ten = codecs::decode("P2\n2 1\n1023\n0 1023\n");
        QCOMPARE(ten.sample(), Sample::U16);
        QCOMPARE(ten.value(1), 1.0f);
        const CodecImage p3 = codecs::decode("P3\n1 1\n255\n1 2 3\n");
        QVERIFY(p3.layout() == Layout::Rgb && p3.data() == std::string("\x01\x02\x03", 3));
        const CodecImage pam = codecs::decode(std::string("P7\nWIDTH 1\nHEIGHT 1\nDEPTH 2\nMAXVAL 255\nTUPLTYPE GRAYSCALE_ALPHA\nENDHDR\n\x10\x20"));
        QVERIFY(pam.layout() == Layout::GrayA && pam.data() == std::string("\x10\x20", 2));
        std::string pfm("PF\n1 2\n1.0\n");
        for (const float v : {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f}) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &v, 4);
            for (int s = 24; s >= 0; s -= 8) pfm.push_back(static_cast<char>((bits >> s) & 0xff));
        }
        QCOMPARE(codecs::decode(pfm).normalized(), (std::vector<float>{4, 5, 6, 1, 2, 3}));
        // (written by what the picture is: floats PFM, grey P5, RGB P6, an alpha or CMYK P7)
        QVERIFY(codecs::encode(test_image(Layout::Gray, Sample::F32), Format::Pnm).starts_with("Pf\n"));
        QVERIFY(codecs::encode(test_image(Layout::Rgb, Sample::F32), Format::Pnm).starts_with("PF\n"));
        QVERIFY(codecs::encode(test_image(Layout::Gray, Sample::U8), Format::Pnm).starts_with("P5\n"));
        QVERIFY(codecs::encode(test_image(Layout::Rgb, Sample::U16), Format::Pnm).starts_with("P6\n37 23\n65535\n"));
        QVERIFY(codecs::encode(test_image(Layout::CmykA, Sample::U8), Format::Pnm).find("TUPLTYPE CMYK_ALPHA") != std::string::npos);
        // (JPEG markers that lead nowhere)
        for (const std::string& bad : {std::string("\xFF\xD8\xFF", 3), std::string("\xFF\xD8\xFF\xE0\x00", 5), std::string("\xFF\xD8\xFF\xE0\x00\x01", 6),
                                       std::string("\xFF\xD8\xFF\xE2\x00\x10ICC_PROFILE\0\x01\x01", 20),
                                       std::string("\xFF\xD8\xFF\xC0\x00\x0B\x08\xFF\xFF\xFF\xFF\x03\0\0\0", 15), std::string("\xFF\xD8\xFF\xD9", 4)})
            QVERIFY(!code_of([&] { (void)codecs::decode(bad); }).empty());
    }

    void tinyTiffs() {
        // (a 1 × 1 grey TIFF read; one with an empty SampleFormat refused — PhotoCraft's regression of the tiff crate)
        const auto tiny = [](std::vector<std::array<std::uint32_t, 4>> extra) {
            std::vector<std::array<std::uint32_t, 4>> entries{{256, 3, 1, 1}, {257, 3, 1, 1}, {258, 3, 1, 8}, {259, 3, 1, 1}, {262, 3, 1, 1},
                                                              {273, 4, 1, 0}, {277, 3, 1, 1}, {278, 3, 1, 1}, {279, 4, 1, 1}};
            entries.insert(entries.end(), extra.begin(), extra.end());
            std::sort(entries.begin(), entries.end());
            const std::uint32_t strip = 8 + 2 + 12 * static_cast<std::uint32_t>(entries.size()) + 4;
            std::string b("II*\0\x08\0\0\0", 8);
            const auto le = [&b](std::uint32_t v, int n) {
                for (int i = 0; i < n; ++i) b.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
            };
            le(static_cast<std::uint32_t>(entries.size()), 2);
            for (const auto& e : entries) {
                le(e[0], 2);
                le(e[1], 2);
                le(e[2], 4);
                le(e[0] == 273 ? strip : e[3], 4);
            }
            le(0, 4);
            b.push_back('\x80');
            return b;
        };
        const CodecImage one = codecs::decode_as(Format::Tiff, tiny({}));
        QVERIFY(one.width() == 1 && one.height() == 1);
        const std::string empty_format = tiny({{339, 3, 0, 0}});
        no_crash(Format::Tiff, empty_format);
    }

    // --- the drawing's 8-bit reader --------------------------------------------------------------------------------

    // open_image reads the codecs' new formats as their 8-bit picture (picture_size from their headers, none "not read"):
    // grey, RGB, their alpha, CMYK; deeper samples rounded; OpenEXR's and Radiance HDR's linear light given sRGB's curve.
    void theDrawingReadsTheNewFormats() {
        for (const Format f : {Format::Tga, Format::Ico, Format::Pnm, Format::Qoi, Format::OpenExr, Format::Hdr, Format::Avif}) {
            for (const Layout l : codecs::kLayouts) {
                const CodecImage img = test_image(l, Sample::U16);
                const std::string bytes = codecs::encode(img, f);
                const std::string what = label(f, l, Sample::U16);
                QVERIFY2(!genko::render::unported_image_format(bytes), what.c_str());
                const genko::render::Image read = genko::render::open_image(bytes, genko::render::kPillowOpenLimits);
                QVERIFY2(read.width() == 37 && read.height() == 23, what.c_str());
                QVERIFY2(genko::render::picture_size(bytes).width == 37, what.c_str());
                const CodecImage back = codecs::decode(bytes);
                QVERIFY2(read.tobytes() == codecs::eight_bit(back, codecs::linear_light(f)).tobytes(), what.c_str());
                const std::string_view mode = read.mode();
                const Layout got = back.layout();
                const std::string_view want = got == Layout::Gray ? "L" : got == Layout::GrayA ? "LA" : got == Layout::Rgb ? "RGB" : got == Layout::Cmyk ? "CMYK" : "RGBA";
                QVERIFY2(mode == want, (what + " " + std::string(mode)).c_str());
            }
        }
        // (linear 0.5 is sRGB's 188; 2.0 cut to 255)
        const CodecImage light = CodecImage::from_normalized(2, 1, Layout::Rgb, Sample::F32, std::vector<float>{0.5f, 0.5f, 0.5f, 2.0f, 0.0f, 0.0f});
        const std::string drawn = genko::render::open_image(codecs::encode(light, Format::OpenExr), genko::render::kPillowOpenLimits).tobytes();
        QCOMPARE(drawn, std::string("\xbc\xbc\xbc\xff\0\0", 6));
        // (a PPM of 16 bits rounded; a CMYK PAM as Pillow's CMYK)
        const genko::render::Image ppm = genko::render::open_image(std::string("P6\n1 1\n65535\n\x80\x00\x40\x00\xff\xff", 19));
        QCOMPARE(ppm.tobytes(), std::string("\x80\x40\xff", 3));
        const genko::render::Image cmyk = genko::render::open_image(std::string("P7\nWIDTH 1\nHEIGHT 1\nDEPTH 4\nMAXVAL 255\nTUPLTYPE CMYK\nENDHDR\n\x10\x20\x30\x40"));
        QCOMPARE(std::string(cmyk.mode()), std::string("CMYK"));
        // (still not read: JPEG 2000, said as not read by this build)
        const std::string jp2("\0\0\0\x0cjP  \r\n\x87\n\0\0\0\x14" "ftypjp2 \0\0\0\0jp2 ", 32);
        QVERIFY(genko::render::unported_image_format(jp2));
        QCOMPARE(code_of([&] { (void)genko::render::open_image(jp2); }), std::string("not_yet_ported:image_format"));
    }
};

QTEST_GUILESS_MAIN(CodecsTest)
#include "test_codecs.moc"
