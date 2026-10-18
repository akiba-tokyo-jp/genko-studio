// smart-objects@1 (PC3 A0, decision 2): a smart object's file read as it is (render/smart_decode) — a PNG of 16 bits and a
// TIFF of 16 and 32 bits with every bit, what placing it records (its format, size, depth, colours, resolution, profile),
// a CMYK JPEG and TIFF on a CMYK page as their plates, what is refused before anything is placed (a file past 256 MiB, a
// decompression bomb, one cut short, a suffix that is not its kind), and its colours by the use the contents keep (keep:
// its numbers; embedded: from its own profile; assign: from the one given).

#include <QtTest>

#include <png.h>
#include <tiffio.h>
#include <zlib.h>

#include <cstdio>
#include <cstring>
#include <jpeglib.h>

#include <QTemporaryDir>

#include <string>
#include <vector>

#include "core/color_model.hpp"
#include "core/error.hpp"
#include "core/ink.hpp"
#include "core/model.hpp"
#include "render/ink.hpp"
#include "render/not_yet_ported.hpp"
#include "render/png.hpp"
#include "render/proof.hpp"
#include "render/smart_decode.hpp"
#include "testsupport.hpp"

namespace core = genko::core;
namespace render = genko::render;
namespace smart = genko::render::smart;

namespace {

std::string be32(std::uint32_t v) {
    return std::string{static_cast<char>(v >> 24), static_cast<char>(v >> 16), static_cast<char>(v >> 8), static_cast<char>(v)};
}

std::string chunk(const std::string& type, const std::string& data) {
    const uLong crc = crc32(crc32(0, reinterpret_cast<const Bytef*>(type.data()), 4), reinterpret_cast<const Bytef*>(data.data()),
                            static_cast<uInt>(data.size()));
    return be32(static_cast<std::uint32_t>(data.size())) + type + data + be32(static_cast<std::uint32_t>(crc));
}

// A PNG of 16 bits a sample (colour type 0 grey, 2 RGB, 6 RGBA), its samples given big-endian as PNG keeps them, with a
// pHYs of `ppm` pixels a metre (0: none) and an iCCP of `icc` (empty: none).
std::string png16(int w, int h, int type, const std::vector<std::uint16_t>& samples, std::uint32_t ppm = 0, const std::string& icc = {}) {
    const int channels = type == 0 ? 1 : type == 2 ? 3 : 4;
    std::string raw;
    for (int y = 0; y < h; ++y) {
        raw.push_back('\0');
        for (int i = 0; i < w * channels; ++i) {
            const std::uint16_t v = samples[std::size_t(y) * w * channels + i];
            raw.push_back(static_cast<char>(v >> 8));
            raw.push_back(static_cast<char>(v & 0xff));
        }
    }
    uLongf size = compressBound(static_cast<uLong>(raw.size()));
    std::string packed(size, '\0');
    compress(reinterpret_cast<Bytef*>(packed.data()), &size, reinterpret_cast<const Bytef*>(raw.data()), static_cast<uLong>(raw.size()));
    packed.resize(size);
    std::string ihdr = be32(static_cast<std::uint32_t>(w)) + be32(static_cast<std::uint32_t>(h));
    ihdr += std::string{16, static_cast<char>(type), 0, 0, 0};
    std::string out("\x89PNG\r\n\x1a\n", 8);
    out += chunk("IHDR", ihdr);
    if (ppm > 0) out += chunk("pHYs", be32(ppm) + be32(ppm) + std::string(1, '\1'));
    if (!icc.empty()) {
        uLongf isize = compressBound(static_cast<uLong>(icc.size()));
        std::string ipacked(isize, '\0');
        compress(reinterpret_cast<Bytef*>(ipacked.data()), &isize, reinterpret_cast<const Bytef*>(icc.data()), static_cast<uLong>(icc.size()));
        ipacked.resize(isize);
        out += chunk("iCCP", std::string("icc\0\0", 5) + ipacked);
    }
    out += chunk("IDAT", packed);
    out += chunk("IEND", "");
    return out;
}

// A TIFF written by libtiff: `photometric` (MINISBLACK, RGB, SEPARATED), `bits` 8, 16 or 32 (32: IEEE floats), samples
// together, 300 dpi.
std::string make_tiff(const QTemporaryDir& dir, int w, int h, int photometric, int channels, int bits, const std::vector<double>& values) {
    const std::string path = dir.path().toStdString() + "/t.tif";
    TIFF* t = TIFFOpen(path.c_str(), "w");
    TIFFSetField(t, TIFFTAG_IMAGEWIDTH, w);
    TIFFSetField(t, TIFFTAG_IMAGELENGTH, h);
    TIFFSetField(t, TIFFTAG_SAMPLESPERPIXEL, channels);
    TIFFSetField(t, TIFFTAG_BITSPERSAMPLE, bits);
    TIFFSetField(t, TIFFTAG_SAMPLEFORMAT, bits == 32 ? SAMPLEFORMAT_IEEEFP : SAMPLEFORMAT_UINT);
    TIFFSetField(t, TIFFTAG_PHOTOMETRIC, photometric);
    TIFFSetField(t, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(t, TIFFTAG_XRESOLUTION, 300.0f);
    TIFFSetField(t, TIFFTAG_YRESOLUTION, 300.0f);
    TIFFSetField(t, TIFFTAG_RESOLUTIONUNIT, RESUNIT_INCH);
    if (photometric == PHOTOMETRIC_SEPARATED) TIFFSetField(t, TIFFTAG_INKSET, INKSET_CMYK);
    std::vector<unsigned char> row(std::size_t(w) * channels * (bits / 8));
    for (int y = 0; y < h; ++y) {
        for (int i = 0; i < w * channels; ++i) {
            const double v = values[std::size_t(y) * w * channels + i];
            if (bits == 8) row[i] = static_cast<unsigned char>(std::lround(v * 255));
            if (bits == 16) {
                const auto s = static_cast<std::uint16_t>(std::lround(v * 65535));
                std::memcpy(row.data() + i * 2, &s, 2);
            }
            if (bits == 32) {
                const auto f = static_cast<float>(v);
                std::memcpy(row.data() + i * 4, &f, 4);
            }
        }
        TIFFWriteScanline(t, row.data(), static_cast<std::uint32_t>(y), 0);
    }
    TIFFClose(t);
    return genko::test::read_bytes(QString::fromStdString(path));
}

// A CMYK JPEG (libjpeg, Adobe's inverted CMYK as Photoshop and Pillow write it).
std::string cmyk_jpeg(int w, int h, const std::array<int, 4>& ink) {
    jpeg_compress_struct c{};
    jpeg_error_mgr err{};
    c.err = jpeg_std_error(&err);
    jpeg_create_compress(&c);
    unsigned char* buffer = nullptr;
    unsigned long size = 0;
    jpeg_mem_dest(&c, &buffer, &size);
    c.image_width = static_cast<JDIMENSION>(w);
    c.image_height = static_cast<JDIMENSION>(h);
    c.input_components = 4;
    c.in_color_space = JCS_CMYK;
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, 100, TRUE);
    c.write_Adobe_marker = TRUE;
    jpeg_start_compress(&c, TRUE);
    std::vector<unsigned char> row(std::size_t(w) * 4);
    for (int x = 0; x < w; ++x)
        for (int k = 0; k < 4; ++k) row[std::size_t(x) * 4 + k] = static_cast<unsigned char>(255 - ink[k]);  // (Adobe: inverted)
    while (c.next_scanline < c.image_height) {
        JSAMPROW r = row.data();
        jpeg_write_scanlines(&c, &r, 1);
    }
    jpeg_finish_compress(&c);
    std::string out(reinterpret_cast<char*>(buffer), size);
    jpeg_destroy_compress(&c);
    std::free(buffer);
    return out;
}

core::PageColor cmyk_page(int depth) {
    core::PageColor color;
    color.space = core::InkSpace::Cmyk;
    color.depth = depth;
    const core::BuiltinProfile* coated = core::builtin_profile("coated-cmyk");
    color.profile = coated->ref();
    color.icc = coated->bytes;
    return color;
}

}  // namespace

class TestSmartDecode : public QObject {
    Q_OBJECT

private slots:
    void sixteenBitsAreKept() {
        // (a PNG of 16 bits: values one 8-bit step could not tell apart)
        const std::vector<std::uint16_t> rgb{1000, 1001, 1002, 65535, 0, 32769, 300, 301, 302, 40000, 40001, 40002};
        const std::string png = png16(2, 2, 2, rgb, 11811);  // (11811 a metre: 300 dpi)
        const smart::FileInfo info = smart::look_at(png, ".png");
        QCOMPARE(info.format, std::string("png"));
        QCOMPARE(info.depth, 16);
        QCOMPARE(info.mode, std::string("rgb"));
        QCOMPARE(info.width, std::int64_t{2});
        QVERIFY(std::abs(info.dpi - 300) < 0.01);
        const smart::DeepPicture deep = smart::deep_picture(png);
        QCOMPARE(deep.depth, 16);
        QCOMPARE(deep.channels, 3u);
        for (std::size_t i = 0; i < rgb.size(); ++i) QCOMPARE(std::lround(deep.samples[i] * 65535.0), static_cast<long>(rgb[i]));
        // (the 8-bit page reads it as any picture is read: 8 bits)
        QCOMPARE(render::open_image(png, render::kPillowOpenLimits).mode(), std::string_view("RGB"));
        // (grey with an alpha: two channels)
        const smart::DeepPicture grey = smart::deep_picture(png16(1, 1, 0, {12345}));
        QCOMPARE(grey.mode, std::string("gray"));
        QCOMPARE(std::lround(grey.samples[0] * 65535.0), 12345L);
    }

    void tiffsOf16And32BitsAreKept() {
        QTemporaryDir dir;
        const std::vector<double> values{0.001, 0.25, 0.5, 0.75, 0.999, 0.123456, 1.0, 0.0, 0.333333, 0.6, 0.7, 0.8};
        const std::string t16 = make_tiff(dir, 2, 2, PHOTOMETRIC_RGB, 3, 16, values);
        const smart::FileInfo info = smart::look_at(t16, ".tif");
        QCOMPARE(info.depth, 16);
        QCOMPARE(info.format, std::string("tiff"));
        QVERIFY(std::abs(info.dpi - 300) < 0.01);
        const smart::DeepPicture d16 = smart::deep_picture(t16);
        for (std::size_t i = 0; i < values.size(); ++i) QCOMPARE(std::lround(d16.samples[i] * 65535.0), std::lround(values[i] * 65535));
        const std::string t32 = make_tiff(dir, 2, 2, PHOTOMETRIC_RGB, 3, 32, values);
        QCOMPARE(smart::look_at(t32, ".tif").depth, 32);
        const smart::DeepPicture d32 = smart::deep_picture(t32);
        QCOMPARE(d32.depth, 32);
        for (std::size_t i = 0; i < values.size(); ++i) QCOMPARE(d32.samples[i], static_cast<float>(values[i]));
        // (sRGB floats, kept: the numbers as they are)
        const std::vector<float> srgb = smart::srgb_floats(d32, std::nullopt, "relative", true);
        QCOMPARE(srgb[0], static_cast<float>(values[0]));
        QCOMPARE(srgb[3], 1.0f);
    }

    void aCmykFileKeepsItsPlates() {
        QTemporaryDir dir;
        // (a CMYK TIFF of 8 and 16 bits: its inks onto a CMYK page as they are, taken as the page's)
        const std::vector<double> inks{0.1, 0.2, 0.3, 0.4, 0.9, 0.0, 0.5, 0.05};
        for (const int bits : {8, 16}) {
            const std::string file = make_tiff(dir, 2, 1, PHOTOMETRIC_SEPARATED, 4, bits, inks);
            const smart::FileInfo info = smart::look_at(file, ".tif");
            QCOMPARE(info.mode, std::string("cmyk"));
            const core::InkPixels plates = smart::file_inks(file, cmyk_page(16), "keep");
            QCOMPARE(plates.space, core::InkSpace::Cmyk);
            QCOMPARE(plates.width, 2u);
            for (std::size_t i = 0; i < 2; ++i) {
                for (unsigned k = 0; k < 4; ++k) {
                    const double want = inks[i * 4 + k], got = plates.value(i, k);
                    QVERIFY2(std::abs(want - got) <= (bits == 8 ? 0.5 / 255 : 1.0 / 65535) + 1e-9,
                             qPrintable(QStringLiteral("%1 bits: %2 against %3").arg(bits).arg(got).arg(want)));
                }
            }
        }
        // (a CMYK JPEG: its plates too)
        const std::string jpeg = cmyk_jpeg(8, 8, {30, 60, 90, 120});
        QCOMPARE(smart::look_at(jpeg, ".jpg").mode, std::string("cmyk"));
        const core::InkPixels plates = smart::file_inks(jpeg, cmyk_page(8), "keep");
        const std::array<int, 4> want{30, 60, 90, 120};
        for (unsigned k = 0; k < 4; ++k) QVERIFY(std::abs(plates.value(0, k) * 255 - want[k]) <= 2);
    }

    void whatCannotBeDrawnIsRefused() {
        // (past 256 MiB: refused before anything is read)
        {
            std::string huge(smart::kFileMaxBytes + 1, '\0');
            std::memcpy(huge.data(), "\x89PNG\r\n\x1a\n", 8);
            try {
                (void)smart::look_at(huge, ".png");
                QFAIL("a file past 256 MiB was taken");
            } catch (const core::Error& error) {
                QCOMPARE(error.code(), std::string("too_large"));
            }
        }
        // (a decompression bomb: a picture that says it is huge)
        {
            std::string ihdr = be32(100000) + be32(100000) + std::string{8, 2, 0, 0, 0};
            const std::string bomb = std::string("\x89PNG\r\n\x1a\n", 8) + chunk("IHDR", ihdr) + chunk("IDAT", "x") + chunk("IEND", "");
            try {
                (void)smart::look_at(bomb, ".png");
                QFAIL("a decompression bomb was taken");
            } catch (const core::Error& error) {
                QCOMPARE(error.code(), std::string("image_too_large"));
            }
        }
        // (cut short; not a picture; another kind than its suffix says)
        const std::string png = render::write_png(render::Image::create("RGB", render::Size{20, 20}, render::Ink{1, 2, 3}));
        QVERIFY_THROWS_EXCEPTION(core::Error, smart::look_at(png.substr(0, png.size() - 30), ".png"));
        QVERIFY_THROWS_EXCEPTION(core::Error, smart::look_at("not a picture at all", ""));
        try {
            (void)smart::look_at(png, ".jpg");
            QFAIL("a PNG named .jpg was taken");
        } catch (const core::Error& error) {
            QVERIFY(QString::fromUtf8(error.what()).contains(QStringLiteral("its name says")));
        }
        QCOMPARE(smart::look_at(png, "").suffix, std::string(".png"));
    }

    void theColoursComeByTheUseKept() {
        const std::string p3 = *core::builtin_profile("display-p3")->bytes;
        const std::string png = png16(1, 1, 2, {32768, 16384, 8192}, 0, p3);
        const smart::FileInfo info = smart::look_at(png, ".png");
        QVERIFY(info.icc.has_value() && *info.icc == p3);
        const smart::DeepPicture deep = smart::deep_picture(png);
        const std::vector<float> kept = smart::srgb_floats(deep, std::nullopt, "relative", true);
        const std::vector<float> embedded = smart::srgb_floats(deep, info.icc, "relative", true);
        QVERIFY(std::abs(kept[0] - 32768 / 65535.0f) < 1e-6);
        QVERIFY(std::abs(embedded[0] - kept[0]) > 0.01);  // (P3's red is outside sRGB's: more red)
        // (assign: the colours taken as those of the profile given, e.g. Adobe RGB)
        const std::string adobe = *core::builtin_profile("adobe-rgb-compat")->bytes;
        const std::vector<float> assigned = smart::srgb_floats(deep, adobe, "relative", true);
        QVERIFY(std::abs(assigned[0] - kept[0]) > 0.01 && std::abs(assigned[0] - embedded[0]) > 0.001);
        // (a profile of other colours: refused)
        QVERIFY_THROWS_EXCEPTION(core::Error, smart::srgb_floats(deep, *core::builtin_profile("coated-cmyk")->bytes, "relative", true));
    }
};

QTEST_MAIN(TestSmartDecode)
#include "test_smart_decode.moc"
