// render/raw (PC4 F-e, SPEC.md FILE-01「カメラ RAW」): LibRaw reading PhotoCraft v0.2.0's synthetic RAW files
// (tests/support/raw_testgen: raw/src/testgen.rs — DNG uncompressed, lossless-JPEG strips and tiles, LinearRaw, big-endian,
// CR2, TIFF/EP), against the values they were made of rather than a camera's (LibRaw's pixels differ a little between its
// versions): the black and white points, the white balance's multipliers, the orientation and the size; the colour
// temperature and tint to the multipliers and back; the exposure, the depth, the output spaces, Camera Raw's adjustments
// and the output sharpening; the window's preview; the embedded preview of a RAW LibRaw cannot develop; the limits checked
// before LibRaw unpacks the sensor data; and files cut short or corrupted read without a crash (raw/tests/hostile.rs). And
// core/raw_develop: camera-raw@1's settings, their normal form and what a book may hold.

#include <QtTest>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/color_model.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/raw_develop.hpp"
#include "raw_testgen.hpp"
#include "render/codecs/codecs.hpp"
#include "render/page.hpp"
#include "render/raw.hpp"

namespace raw = genko::render::raw;
namespace rawgen = genko::test::rawgen;
namespace codecs = genko::render::codecs;
using genko::core::Json;

namespace {

// XYZ (D65) → linear sRGB: a "camera" whose colours are sRGB's (its ColorMatrix1, illuminant D65).
constexpr std::array<double, 9> kSrgbCamera{3.2404542, -1.5371385, -0.4985314, -0.9692660, 1.8760108, 0.0415560, 0.0556434, -0.2040259, 1.0572252};

double srgb_encode(double v) { return v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1 / 2.4) - 0.055; }
double srgb_decode(double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); }

// A flat scene of neutral grey `level` (linear) shot under a white whose camera coordinates are `neutral`: a 64 × 48
// mosaic of 12 bits (black 64, white 4000).
rawgen::DngSpec neutral_dng(double level, std::array<double, 3> neutral = {0.5, 1.0, 0.7}, std::size_t w = 64, std::size_t h = 48) {
    std::vector<std::array<float, 3>> rgb(w * h);
    for (auto& p : rgb)
        for (int c = 0; c < 3; ++c) p[std::size_t(c)] = static_cast<float>(level * neutral[std::size_t(c)]);
    rawgen::DngSpec d = rawgen::DngSpec::cfa_of(w, h, rawgen::mosaic(rgb, w, {0, 1, 1, 2}, 64, 4000));
    d.bits = 12;
    d.white = 4000;
    d.black = {64};
    d.as_shot_neutral = neutral;
    d.color_matrix1 = std::make_pair(std::uint16_t(21), kSrgbCamera);
    return d;
}

// The scene of PhotoCraft's tests (smooth colours), as a 64 × 48 DNG of 12 bits.
rawgen::DngSpec scene_dng(std::size_t w = 64, std::size_t h = 48) {
    rawgen::DngSpec d = rawgen::DngSpec::cfa_of(w, h, rawgen::mosaic(rawgen::scene(w, h), w, {0, 1, 1, 2}, 64, 4000));
    d.bits = 12;
    d.white = 4000;
    d.black = {64};
    d.as_shot_neutral = std::array<double, 3>{0.5, 1.0, 0.7};
    d.color_matrix1 = std::make_pair(std::uint16_t(21), kSrgbCamera);
    return d;
}

Json raw_with(const Json& develop, const Json& adjust = Json()) {
    Json out{{"develop", develop}};
    if (!adjust.is_null()) out["adjust"] = adjust;
    return out;
}

// The mean of each colour of a developed picture over its middle (its edges left out: the demosaic's border).
std::array<double, 3> middle_mean(const codecs::CodecImage& p) {
    std::array<double, 3> sum{0, 0, 0};
    double n = 0;
    const std::uint32_t w = p.width(), h = p.height();
    for (std::uint32_t y = h / 4; y < h - h / 4; ++y)
        for (std::uint32_t x = w / 4; x < w - w / 4; ++x) {
            for (int c = 0; c < 3; ++c) sum[std::size_t(c)] += p.value((std::size_t(y) * w + x) * 3 + std::size_t(c));
            n += 1;
        }
    for (double& v : sum) v /= n;
    return sum;
}

codecs::CodecImage developed(const std::string& bytes, const Json& develop, const Json& adjust = Json()) {
    return raw::develop(bytes, raw_with(develop, adjust)).picture;
}

// A small JPEG of a flat colour (an embedded preview).
std::string jpeg_of(std::uint32_t w, std::uint32_t h, std::array<std::uint8_t, 3> rgb) {
    std::string data(std::size_t(w) * h * 3, '\0');
    for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<char>(rgb[i % 3]);
    const auto picture = codecs::CodecImage::from_bytes(w, h, codecs::Layout::Rgb, codecs::Sample::U8, data);
    return codecs::encode(picture, codecs::Format::Jpeg);
}

}  // namespace

class TestRaw : public QObject {
    Q_OBJECT

private slots:
    void theLibraryIsLibRaw022() {
        const std::string version = raw::libraw_version();
        QVERIFY2(version.starts_with("0.22."), version.c_str());
    }

    // The suffixes and marks: a DNG and a CR2 by their marks, a TIFF-shaped RAW by its suffix; a TIFF, a PNG are not.
    void whatIsARaw() {
        for (const char* suffix : {".dng", ".CR2", ".cr3", ".nef", ".arw", ".raf", ".orf", ".rw2", ".pef"}) QVERIFY2(raw::raw_suffix(suffix), suffix);
        for (const char* suffix : {".tif", ".png", ".jpg", ""}) QVERIFY2(!raw::raw_suffix(suffix), suffix);
        const std::string dng = neutral_dng(0.18).build();
        QVERIFY(raw::looks_raw(dng));
        QVERIFY(raw::is_raw(dng, ""));
        rawgen::Cr2Spec cr2;
        cr2.width = 64;
        cr2.height = 48;
        cr2.data = rawgen::mosaic(rawgen::scene(64, 48), 64, {0, 1, 1, 2}, 600, 3600);
        QVERIFY(raw::looks_raw(cr2.build()));
        const std::string nef = rawgen::tiff_ep("NIKON", "", 64, 48, rawgen::mosaic(rawgen::scene(64, 48), 64, {0, 1, 1, 2}, 0, 4095), {0, 1, 1, 2}, 12);
        QVERIFY(!raw::looks_raw(nef));  // (TIFF-shaped: known by its suffix)
        const auto tiff = codecs::encode(codecs::CodecImage::blank(30, 30, codecs::Layout::Rgb, codecs::Sample::U16), codecs::Format::Tiff);
        QVERIFY(!raw::looks_raw(tiff));
        QVERIFY(!raw::is_raw(tiff, ".tif"));
        QVERIFY(!raw::is_raw(tiff, ".dng"));  // (named so, LibRaw does not open it)
        const auto png = codecs::encode(codecs::CodecImage::blank(30, 30, codecs::Layout::Rgb, codecs::Sample::U8), codecs::Format::Png);
        QVERIFY(!raw::looks_raw(png));
    }

    // What LibRaw reads of a DNG's header: the camera, the sizes, the levels, the camera's white balance (AsShotNeutral's
    // reciprocals), its matrix, the baseline exposure.
    void aDngsHeader() {
        rawgen::DngSpec spec = neutral_dng(0.18);
        spec.baseline_exposure = 0.5;
        const raw::Info info = raw::identify(spec.build());
        QCOMPARE(info.camera.make, std::string("Photocraft"));
        QCOMPARE(info.camera.model, std::string("Synthetic"));
        QCOMPARE(info.camera.format, std::string("DNG"));
        QCOMPARE(info.width, std::int64_t(64));
        QCOMPARE(info.height, std::int64_t(48));
        QCOMPARE(info.flip, 0);
        QVERIFY(info.mosaic);
        QVERIFY(info.matrix);
        QVERIFY2(std::abs(info.as_shot[0] - 2.0) < 1e-3 && info.as_shot[1] == 1.0 && std::abs(info.as_shot[2] - 1.0 / 0.7) < 1e-3,
                 (std::to_string(info.as_shot[0]) + " " + std::to_string(info.as_shot[2])).c_str());
        QCOMPARE(info.black, 64.0);
        QCOMPARE(info.white, 4000.0);
        QVERIFY(std::abs(info.baseline_exposure - 0.5) < 1e-3);
        QCOMPARE(raw::suffix_of(info), std::string(".dng"));
    }

    // The black and white points: a neutral grey of 0.18 comes out grey of 0.18 (sRGB's curve on it), black black, white
    // white — through the camera's own white balance — in every storage of the DNG.
    void blackGreyAndWhite() {
        const Json srgb16{{"space", "srgb"}, {"depth", 16}};
        for (const double level : {0.0, 0.18, 0.5, 1.0}) {
            const auto p = developed(neutral_dng(level).build(), srgb16);
            QCOMPARE(p.width(), 64u);
            QCOMPARE(p.height(), 48u);
            QCOMPARE(p.sample(), codecs::Sample::U16);
            QVERIFY(!p.icc);  // (sRGB: no profile of its own)
            const auto m = middle_mean(p);
            const double want = srgb_encode(level);
            for (int c = 0; c < 3; ++c)
                QVERIFY2(std::abs(m[std::size_t(c)] - want) < 0.01, (std::to_string(level) + ": " + std::to_string(m[std::size_t(c)]) + " not " + std::to_string(want)).c_str());
        }
        // (the storages: the same picture)
        const std::string strips = scene_dng().build();
        const auto base = developed(strips, srgb16);
        const auto same = [&](rawgen::DngSpec spec, const char* what) {
            const auto other = developed(spec.build(), srgb16);
            QCOMPARE(other.width(), base.width());
            QCOMPARE(other.height(), base.height());
            double most = 0;
            for (std::size_t i = 0; i < std::size_t(base.width()) * base.height() * 3; ++i) most = std::max<double>(most, std::abs(other.value(i) - base.value(i)));
            QVERIFY2(most < 1e-4, (std::string(what) + ": " + std::to_string(most)).c_str());
        };
        rawgen::DngSpec lj = scene_dng();
        lj.storage = rawgen::DngStorage::Lj92Strips;
        lj.rows = 16;
        same(lj, "lossless JPEG strips");
        rawgen::DngSpec tiles = scene_dng();
        tiles.storage = rawgen::DngStorage::Lj92Tiles;
        tiles.tile_width = 32;
        tiles.tile_height = 16;
        same(tiles, "lossless JPEG tiles");
        rawgen::DngSpec bands = scene_dng();
        bands.rows = 5;
        bands.big_endian = true;
        same(bands, "big-endian strips");
    }

    // LinearRaw (no mosaic: three samples a pixel): its grey as the mosaic's.
    void aLinearRaw() {
        const std::size_t w = 40, h = 30;
        std::vector<std::uint16_t> data(w * h * 3);
        const std::array<double, 3> neutral{0.5, 1.0, 0.7};
        for (std::size_t i = 0; i < w * h; ++i)
            for (int c = 0; c < 3; ++c) data[i * 3 + std::size_t(c)] = static_cast<std::uint16_t>(std::lround(64 + 0.18 * neutral[std::size_t(c)] * (4000 - 64)));
        rawgen::DngSpec spec = rawgen::DngSpec::cfa_of(w, h, data);
        spec.samples = 3;
        spec.bits = 16;
        spec.white = 4000;
        spec.black = {64};
        spec.as_shot_neutral = neutral;
        spec.color_matrix1 = std::make_pair(std::uint16_t(21), kSrgbCamera);
        const std::string bytes = spec.build();
        const raw::Info info = raw::identify(bytes);
        QVERIFY(!info.mosaic);
        const auto m = middle_mean(developed(bytes, Json{{"space", "srgb"}}));
        for (int c = 0; c < 3; ++c) QVERIFY2(std::abs(m[std::size_t(c)] - srgb_encode(0.18)) < 0.01, std::to_string(m[std::size_t(c)]).c_str());
    }

    // The orientation: the file's (6: turned right — its sides swapped) or none.
    void theOrientation() {
        rawgen::DngSpec spec = scene_dng(64, 40);
        spec.orientation = 6;
        const std::string bytes = spec.build();
        const raw::Info info = raw::identify(bytes);
        QCOMPARE(info.flip, 6);
        {
            genko::core::raw::Develop d;
            QCOMPARE(raw::developed_size(info, d), (std::pair<std::int64_t, std::int64_t>{40, 64}));
            d.orientation = "none";
            QCOMPARE(raw::developed_size(info, d), (std::pair<std::int64_t, std::int64_t>{64, 40}));
        }
        const auto turned = developed(bytes, Json::object());
        QCOMPARE(turned.width(), 40u);
        QCOMPARE(turned.height(), 64u);
        const auto plain = developed(bytes, Json{{"orientation", "none"}});
        QCOMPARE(plain.width(), 64u);
        QCOMPARE(plain.height(), 40u);
        // (turned right: the plain picture's bottom-left pixel is the turned one's top-left)
        for (int c = 0; c < 3; ++c) {
            const double a = turned.value((std::size_t(2) * 40 + 2) * 3 + std::size_t(c));
            const double b = plain.value((std::size_t(40 - 3) * 64 + 2) * 3 + std::size_t(c));
            QVERIFY2(std::abs(a - b) < 0.02, (std::to_string(a) + " " + std::to_string(b)).c_str());
        }
    }

    // The colour temperature and tint: a white's xy and back, and through a camera's matrix to its multipliers and back.
    void temperatureAndTintBothWays() {
        for (const double t : {2000.0, 2856.0, 3200.0, 4500.0, 5500.0, 6504.0, 8000.0, 12000.0, 25000.0, 50000.0})
            for (const double tint : {-150.0, -40.0, 0.0, 25.0, 150.0}) {
                const auto [t2, tint2] = raw::temperature_tint(raw::white_xy(t, tint));
                QVERIFY2(std::abs(t2 - t) / t < 2e-3 && std::abs(tint2 - tint) < 0.2,
                         (std::to_string(t) + "/" + std::to_string(tint) + " → " + std::to_string(t2) + "/" + std::to_string(tint2)).c_str());
            }
        // (the Planckian locus: illuminant A's white is 2856 K and on it)
        const auto [ta, tinta] = raw::temperature_tint({0.44757, 0.40745});
        QVERIFY2(std::abs(ta - 2856) < 15 && std::abs(tinta) < 2, (std::to_string(ta) + " " + std::to_string(tinta)).c_str());
        // (D65 is about 6500 K, a little toward green)
        const auto [td, tintd] = raw::temperature_tint({0.3127, 0.3290});
        QVERIFY2(std::abs(td - 6504) < 60 && tintd > 0 && tintd < 15, (std::to_string(td) + " " + std::to_string(tintd)).c_str());
        const raw::Info info = raw::identify(neutral_dng(0.18).build());
        for (const double t : {3000.0, 5000.0, 6504.0, 9000.0})
            for (const double tint : {-30.0, 0.0, 30.0}) {
                const auto m = raw::multipliers_for(info, t, tint);
                QCOMPARE(m[1], 1.0);
                const auto [t2, tint2] = raw::temperature_of(info, m);
                QVERIFY2(std::abs(t2 - t) / t < 3e-3 && std::abs(tint2 - tint) < 0.3, (std::to_string(t2) + " " + std::to_string(tint2)).c_str());
            }
        // (warmer light: more blue and less red to balance it)
        const auto warm = raw::multipliers_for(info, 3000, 0), cool = raw::multipliers_for(info, 9000, 0);
        QVERIFY(warm[0] < cool[0] && warm[2] > cool[2]);
        // (the camera's own white — AsShotNeutral (0.5, 1, 0.7) — has a temperature and tint, and balanced so, the
        // picture is as shot)
        const auto [shot_t, shot_tint] = raw::temperature_of(info, info.as_shot);
        QVERIFY(shot_t > 2000 && shot_t < 50000);
        const std::string bytes = neutral_dng(0.18).build();
        const auto as_shot = middle_mean(developed(bytes, Json{{"space", "srgb"}}));
        const auto custom = middle_mean(developed(bytes, Json{{"space", "srgb"}, {"white_balance", "custom"}, {"temperature_k", shot_t}, {"tint", shot_tint}}));
        for (int c = 0; c < 3; ++c) QVERIFY2(std::abs(as_shot[std::size_t(c)] - custom[std::size_t(c)]) < 0.01, std::to_string(custom[std::size_t(c)]).c_str());
        // (another white: the grey is no longer grey — warmer light assumed, the picture bluer)
        const auto warmer = middle_mean(developed(bytes, Json{{"space", "srgb"}, {"white_balance", "custom"}, {"temperature_k", 3000}, {"tint", 0}}));
        QVERIFY(warmer[2] > warmer[0] + 0.05);
        // (the multipliers it was balanced with: the camera's)
        const auto made = raw::develop(bytes, raw_with(Json::object()));
        QVERIFY(std::abs(made.multipliers[0] - 2.0) < 0.01 && std::abs(made.multipliers[2] - 1.0 / 0.7) < 0.01);
        // (automatic: grey world — the flat grey stays grey)
        const auto automatic = middle_mean(developed(bytes, Json{{"space", "srgb"}, {"white_balance", "auto"}}));
        QVERIFY(std::abs(automatic[0] - automatic[1]) < 0.02 && std::abs(automatic[2] - automatic[1]) < 0.02);
    }

    // The exposure (EV on linear light, after the DNG's baseline exposure), the depth, the output spaces and their profiles.
    void exposureDepthAndSpaces() {
        const std::string bytes = neutral_dng(0.1).build();
        const double base = srgb_decode(middle_mean(developed(bytes, Json{{"space", "srgb"}}))[1]);
        const double up = srgb_decode(middle_mean(developed(bytes, Json{{"space", "srgb"}, {"exposure_ev", 1}}))[1]);
        const double down = srgb_decode(middle_mean(developed(bytes, Json{{"space", "srgb"}, {"exposure_ev", -1}}))[1]);
        QVERIFY2(std::abs(up / base - 2) < 0.05 && std::abs(base / down - 2) < 0.05, (std::to_string(base) + " " + std::to_string(up) + " " + std::to_string(down)).c_str());
        rawgen::DngSpec brighter = neutral_dng(0.1);
        brighter.baseline_exposure = 1.0;
        const double baseline = srgb_decode(middle_mean(developed(brighter.build(), Json{{"space", "srgb"}}))[1]);
        QVERIFY2(std::abs(baseline / base - 2) < 0.05, std::to_string(baseline).c_str());
        // (8 bits: the 16 bits' values within a step of 8 bits)
        const auto p8 = developed(bytes, Json{{"space", "srgb"}, {"depth", 8}});
        const auto p16 = developed(bytes, Json{{"space", "srgb"}, {"depth", 16}});
        QCOMPARE(p8.sample(), codecs::Sample::U8);
        for (std::size_t i = 0; i < std::size_t(p8.width()) * p8.height() * 3; i += 97) QVERIFY(std::abs(p8.value(i) - p16.value(i)) <= 0.5 / 255 + 1e-6);
        // (each space: its built-in profile, and a neutral grey the same grey: its curve's)
        for (const char* space : {"prophoto-compat", "adobe-rgb-compat", "display-p3"}) {
            const auto p = developed(bytes, Json{{"space", space}});
            QVERIFY(p.icc);
            QVERIFY(*p.icc == *genko::core::builtin_profile(space)->bytes);
            const auto m = middle_mean(p);
            QVERIFY2(std::abs(m[0] - m[1]) < 0.01 && std::abs(m[2] - m[1]) < 0.01, space);
        }
        // (and a colour: the same colour in each space, its values by each space's)
        const std::string colours = scene_dng().build();
        const auto pro = developed(colours, Json{{"space", "prophoto-compat"}}), srgb = developed(colours, Json{{"space", "srgb"}});
        QVERIFY(std::abs(middle_mean(pro)[0] - middle_mean(srgb)[0]) > 0.01);
    }

    // The highlights: a sensor clipped in one channel — clip keeps it white, the others keep what is below white for an
    // exposure brought down (unclip: the clipped channel's own value).
    void theHighlights() {
        const std::string bytes = neutral_dng(1.3).build();  // (past white: clipped in every channel)
        for (const char* mode : {"clip", "unclip", "blend", "rebuild"}) {
            const auto p = developed(bytes, Json{{"space", "srgb"}, {"highlights", mode}});
            const auto m = middle_mean(p);
            QVERIFY2(m[1] > 0.9, mode);
        }
        const std::string half = neutral_dng(0.6).build();
        const auto clip = middle_mean(developed(half, Json{{"space", "srgb"}, {"highlights", "clip"}, {"exposure_ev", -1}}));
        const auto unclip = middle_mean(developed(half, Json{{"space", "srgb"}, {"highlights", "unclip"}, {"exposure_ev", -1}}));
        for (int c = 0; c < 3; ++c) QVERIFY2(std::abs(clip[std::size_t(c)] - unclip[std::size_t(c)]) < 0.02, std::to_string(unclip[std::size_t(c)]).c_str());
    }

    // The demosaics: each develops the flat grey to the same grey.
    void everyDemosaic() {
        const std::string bytes = neutral_dng(0.3).build();
        for (const std::string_view mode : genko::core::raw::kDemosaics) {
            const auto m = middle_mean(developed(bytes, Json{{"space", "srgb"}, {"demosaic", std::string(mode)}}));
            for (int c = 0; c < 3; ++c) QVERIFY2(std::abs(m[std::size_t(c)] - srgb_encode(0.3)) < 0.015, std::string(mode).c_str());
        }
    }

    // Camera Raw's adjustments (節 D's camera_raw, in prophoto-compat) and the output sharpening.
    void adjustmentsAndSharpening() {
        const std::string bytes = scene_dng().build();
        const auto plain = developed(bytes, Json::object());
        const auto none = developed(bytes, Json::object(), Json::object());
        QCOMPARE(none.data(), plain.data());  // (no adjustment: the same picture)
        const auto brighter = developed(bytes, Json::object(), Json{{"exposure", 1.0}});
        QVERIFY(middle_mean(brighter)[1] > middle_mean(plain)[1] + 0.05);
        const auto grey = developed(bytes, Json{{"space", "srgb"}}, Json{{"saturation", -100}});
        const auto g = middle_mean(grey);
        QVERIFY(std::abs(g[0] - g[1]) < 0.02 && std::abs(g[2] - g[1]) < 0.02);
        // (sharpening: an edge's contrast grows, a flat grey stays)
        const std::size_t w = 64, h = 48;
        std::vector<std::array<float, 3>> edge(w * h);
        for (std::size_t i = 0; i < edge.size(); ++i) {
            const float v = (i % w) < w / 2 ? 0.1f : 0.6f;
            edge[i] = {v * 0.5f, v, v * 0.7f};
        }
        rawgen::DngSpec spec = rawgen::DngSpec::cfa_of(w, h, rawgen::mosaic(edge, w, {0, 1, 1, 2}, 64, 4000));
        spec.bits = 12;
        spec.white = 4000;
        spec.black = {64};
        spec.as_shot_neutral = std::array<double, 3>{0.5, 1.0, 0.7};
        spec.color_matrix1 = std::make_pair(std::uint16_t(21), kSrgbCamera);
        const std::string edged = spec.build();
        const auto soft = developed(edged, Json{{"space", "srgb"}});
        const auto sharp = developed(edged, Json{{"space", "srgb"}, {"sharpen", "matte"}});
        const auto at = [&](const codecs::CodecImage& p, std::size_t x) { return p.value((std::size_t(24) * w + x) * 3 + 1); };
        QVERIFY(at(sharp, w / 2 - 2) < at(soft, w / 2 - 2) && at(sharp, w / 2 + 1) > at(soft, w / 2 + 1));
        QVERIFY(std::abs(at(sharp, 5) - at(soft, 5)) < 0.005);
    }

    // The window's preview: half the size and at most max_side, floats in prophoto-compat, shown as sRGB.
    void thePreview() {
        const std::string bytes = neutral_dng(0.18, {0.5, 1.0, 0.7}, 200, 120).build();
        const raw::Preview p = raw::preview(bytes, genko::core::raw::Develop{}, 64);
        QVERIFY(p.width <= 64 && p.height <= 64 && p.width > 0);
        QCOMPARE(p.rgba.size(), std::size_t(p.width) * p.height * 4);
        QCOMPARE(p.rgba[3], 1.0f);
        std::vector<float> shown(p.rgba.begin(), p.rgba.begin() + 4);
        raw::working_to_screen(shown.data(), 1);
        for (int c = 0; c < 3; ++c) QVERIFY2(std::abs(shown[std::size_t(c)] - srgb_encode(0.18)) < 0.015, std::to_string(shown[std::size_t(c)]).c_str());
        std::vector<float> white{1, 1, 1, 0.5f};
        raw::working_to_screen(white.data(), 1);
        QVERIFY(std::abs(white[0] - 1) < 0.01 && std::abs(white[2] - 1) < 0.01 && white[3] == 0.5f);
        // (自動: a grey of 0.09 is brought up a step to the middle grey, one of 0.18 left)
        const raw::Preview dark = raw::preview(neutral_dng(0.09, {0.5, 1.0, 0.7}, 200, 120).build(), genko::core::raw::Develop{}, 64);
        QVERIFY2(std::abs(raw::auto_exposure(dark) - 1.0) <= 0.1, std::to_string(raw::auto_exposure(dark)).c_str());
        QVERIFY2(std::abs(raw::auto_exposure(p)) <= 0.1, std::to_string(raw::auto_exposure(p)).c_str());
    }

    // CR2 (lossless JPEG in Canon's slices, the white balance in its maker note) and TIFF/EP (NIKON, uncompressed).
    void aCr2AndATiffEp() {
        const std::size_t w = 64, h = 48;
        rawgen::Cr2Spec cr2;
        cr2.width = w;
        cr2.height = h;
        cr2.data = rawgen::mosaic(rawgen::scene(w, h), w, {0, 1, 1, 2}, 600, 3600);
        cr2.slices = {24, 24, 16};
        cr2.wb_rggb = std::array<std::uint16_t, 4>{2000, 1024, 1024, 1500};
        const std::string bytes = cr2.build();
        const raw::Info info = raw::identify(bytes);
        QCOMPARE(info.camera.make, std::string("Canon"));
        QCOMPARE(info.camera.format, std::string("CR2"));
        QVERIFY2(info.width > 22 && info.height > 22, (std::to_string(info.width) + "x" + std::to_string(info.height)).c_str());
        const auto p = developed(bytes, Json::object());
        QVERIFY(p.width() > 0);
        if (info.as_shot[0] > 0) QVERIFY2(std::abs(info.as_shot[0] - 2000.0 / 1024) < 0.02 && std::abs(info.as_shot[2] - 1500.0 / 1024) < 0.02, std::to_string(info.as_shot[0]).c_str());
        const std::string nef = rawgen::tiff_ep("NIKON", "D3X", w, h, rawgen::mosaic(rawgen::scene(w, h), w, {0, 1, 1, 2}, 0, 4095), {0, 1, 1, 2}, 12);
        QVERIFY(raw::is_raw(nef, ".nef"));
        const raw::Info nikon = raw::identify(nef);
        QCOMPARE(nikon.camera.make, std::string("Nikon"));
        QCOMPARE(raw::suffix_of(nikon), std::string(".nef"));
        QVERIFY(developed(nef, Json::object()).width() > 0);
    }

    // A RAW LibRaw cannot develop (a compression it does not know): its embedded JPEG preview; LibRaw's own preview of one
    // it opens; none for a file that carries none.
    void theEmbeddedPreview() {
        rawgen::DngSpec unknown = neutral_dng(0.18);
        unknown.compression = 65000;
        unknown.preview_jpeg = jpeg_of(48, 32, {200, 100, 50});
        const std::string bytes = unknown.build();
        QVERIFY(raw::looks_raw(bytes));
        try {
            (void)raw::develop(bytes, Json::object());
            QFAIL("developed");
        } catch (const genko::core::Error& error) {
            QCOMPARE(std::string(error.code()), std::string("unidentified_image"));
        }
        const auto preview = raw::embedded_preview(bytes);
        QVERIFY(preview);
        QCOMPARE(preview->width(), 48u);
        QCOMPARE(preview->height(), 32u);
        QVERIFY(std::abs(preview->value(0) - 200.0 / 255) < 0.05);
        rawgen::DngSpec none = neutral_dng(0.18);
        none.compression = 65000;
        const auto nothing = raw::embedded_preview(none.build());
        // (its IFD0's 2×2 uncompressed thumbnail is not a JPEG: none)
        QVERIFY(!nothing || nothing->width() <= 2);
        QVERIFY(!raw::embedded_preview(std::string(100, 'x')));
    }

    // The drawing's file: an uncompressed TIFF of the development's depth with its output space's profile.
    void theDrawingsFile() {
        const std::string bytes = scene_dng().build();
        const std::string file = raw::developed_file(bytes, raw_with(Json{{"space", "adobe-rgb-compat"}, {"depth", 16}}));
        const auto read = codecs::decode(file);
        QCOMPARE(read.width(), 64u);
        QCOMPARE(read.sample(), codecs::Sample::U16);
        QVERIFY(read.icc && *read.icc == *genko::core::builtin_profile("adobe-rgb-compat")->bytes);
        QCOMPARE(read.data(), developed(bytes, Json{{"space", "adobe-rgb-compat"}}).data());
        const auto eight = codecs::decode(raw::developed_file(bytes, raw_with(Json{{"space", "srgb"}, {"depth", 8}})));
        QCOMPARE(eight.sample(), codecs::Sample::U8);
        QVERIFY(!eight.icc);
        // (placing it: camera-raw@1's defaults and what LibRaw read of the camera)
        const Json defaults = raw::default_raw(bytes);
        QCOMPARE(defaults["develop"], genko::core::raw::to_json(genko::core::raw::Develop{}));
        QCOMPARE(defaults["camera"]["make"], Json("Photocraft"));
        QCOMPARE(defaults["camera"]["format"], Json("DNG"));
        QCOMPARE(defaults["camera"]["libraw"], Json(raw::libraw_version()));
        QVERIFY(!defaults.contains("adjust"));
    }

    // The limits, before LibRaw unpacks the sensor data: the picture's pixels and sides, the work's bytes, the file's size; a
    // header that says 200000 × 200000 (raw/tests/hostile.rs absurd_dimensions).
    void limitsBeforeTheSensorData() {
        const std::string bytes = scene_dng(64, 48).build();
        codecs::Limits small;
        small.max_pixels = 1000;
        const auto refused = [](const std::function<void()>& f, const char* what) {
            try {
                f();
                QFAIL(what);
            } catch (const genko::core::Error& error) {
                QVERIFY2(std::string(error.code()) == "image_too_large", (std::string(what) + ": " + error.code() + " " + error.what()).c_str());
            }
        };
        refused([&] { (void)raw::identify(bytes, small); }, "pixels");
        codecs::Limits narrow;
        narrow.max_width = 32;
        refused([&] { (void)raw::identify(bytes, narrow); }, "side");
        codecs::Limits tight;
        tight.max_alloc = 40000;  // (bytes of work: less than the development holds at once)
        raw::Options options;
        options.limits = tight;
        refused([&] { (void)raw::develop(bytes, Json::object(), options); }, "work");
        try {
            (void)raw::identify(std::string(raw::kFileMaxBytes + 1, '\0'));
            QFAIL("a file past 256 MiB");
        } catch (const genko::core::Error& error) {
            QCOMPARE(std::string(error.code()), std::string("too_large"));
        }
        // (a header that lies: 200000 × 200000)
        rawgen::DngSpec d = rawgen::DngSpec::cfa_of(32, 32, std::vector<std::uint16_t>(32 * 32, 1000));
        std::string lie = d.build();
        int patched = 0;
        for (std::size_t i = 0; i + 24 <= lie.size(); ++i) {
            if (static_cast<unsigned char>(lie[i]) == 0x00 && static_cast<unsigned char>(lie[i + 1]) == 0x01 && lie[i + 2] == 4 && lie[i + 3] == 0 &&
                static_cast<unsigned char>(lie[i + 8]) == 32 && lie[i + 9] == 0 && lie[i + 10] == 0 && lie[i + 11] == 0) {
                const char big[4] = {'\x40', '\x0D', '\x03', '\x00'};  // (200000)
                lie.replace(i + 8, 4, big, 4);
                lie.replace(i + 20, 4, big, 4);
                ++patched;
            }
        }
        QVERIFY(patched > 0);
        try {
            (void)raw::develop(lie, Json::object());
            QFAIL("a header that lies");
        } catch (const genko::core::Error& error) {
            QVERIFY2(std::string(error.code()) == "image_too_large" || std::string(error.code()) == "format" ||
                         std::string(error.code()) == "unidentified_image",
                     error.what());
        }
    }

    // Stopped: render::Cancelled, at once.
    void stopped() {
        const std::string bytes = scene_dng().build();
        raw::Options options;
        options.stopped = [] { return true; };
        QVERIFY_THROWS_EXCEPTION(genko::render::Cancelled, (void)raw::develop(bytes, Json::object(), options));
    }

    // Files cut short at every few bytes and corrupted at random read without a crash: core::Error only (hostile.rs's
    // every_truncation_fails_cleanly, random_corruption_never_panics), within 64 MiB of work.
    void hostileBytes() {
        std::vector<std::string> samples;
        rawgen::DngSpec d = scene_dng(32, 24);
        samples.push_back(d.build());
        d.storage = rawgen::DngStorage::Lj92Tiles;
        d.tile_width = 16;
        d.tile_height = 8;
        d.active_area = std::array<std::uint32_t, 4>{1, 2, 23, 30};
        d.default_crop = std::make_pair(std::array<std::uint32_t, 2>{1, 1}, std::array<std::uint32_t, 2>{24, 20});
        samples.push_back(d.build());
        d.storage = rawgen::DngStorage::Lj92Strips;
        d.rows = 5;
        d.big_endian = true;
        samples.push_back(d.build());
        rawgen::Cr2Spec cr2;
        cr2.width = 40;
        cr2.height = 24;
        cr2.data = rawgen::mosaic(rawgen::scene(40, 24), 40, {0, 1, 1, 2}, 600, 3600);
        cr2.slices = {16, 16, 8};
        cr2.borders = std::array<std::uint16_t, 4>{8, 2, 39, 23};
        cr2.wb_rggb = std::array<std::uint16_t, 4>{2000, 1024, 1024, 1500};
        cr2.orientation = 8;
        samples.push_back(cr2.build());
        samples.push_back(rawgen::tiff_ep("NIKON", "D3X", 32, 24, rawgen::mosaic(rawgen::scene(32, 24), 32, {0, 1, 1, 2}, 0, 4095), {0, 1, 1, 2}, 12));
        raw::Options options;
        options.limits.max_alloc = 64ull << 20;
        const auto exercise = [&](const std::string& b) {
            try {
                (void)raw::identify(b, options.limits);
            } catch (const genko::core::Error&) {
            }
            try {
                (void)raw::embedded_preview(b, options.limits);
            } catch (const genko::core::Error&) {
            }
            try {
                (void)raw::develop(b, Json::object(), options);
            } catch (const genko::core::Error&) {
            }
        };
        int developed_ok = 0;
        for (const std::string& b : samples) {
            try {
                (void)raw::develop(b, Json::object(), options);
                ++developed_ok;
            } catch (const genko::core::Error&) {
            }
            for (std::size_t n = 0; n < b.size(); n += std::max<std::size_t>(3, b.size() / 400)) exercise(b.substr(0, n));
        }
        QVERIFY(developed_ok >= 3);
        std::uint64_t s = 0xDEADBEEFull;
        const auto next = [&s] {
            s = s * 6364136223846793005ull + 1442695040888963407ull;
            return s >> 33;
        };
        for (const std::string& b : samples)
            for (int k = 0; k < 120; ++k) {
                std::string c = b;
                for (std::uint64_t j = 0, n = 1 + next() % 6; j < n; ++j) {
                    const std::size_t at = std::size_t(next() % c.size());
                    const auto how = next() % 4;
                    c[at] = how == 0 ? '\xFF' : how == 1 ? '\0' : static_cast<char>(next() & 0xFF);
                }
                exercise(c);
            }
    }

    // camera-raw@1's settings (core/raw_develop): the defaults, the normal form, what is clamped and refused, what a book
    // may hold.
    void theSettings() {
        namespace cr = genko::core::raw;
        const auto from = [](const Json& j) { return cr::develop_from(&j); };
        const Json defaults = cr::to_json(cr::Develop{});
        QCOMPARE(defaults, (Json{{"white_balance", "as_shot"}, {"exposure_ev", 0.0}, {"demosaic", "ahd"}, {"highlights", "clip"}, {"space", "prophoto-compat"},
                                  {"depth", 16}, {"orientation", "file"}, {"sharpen", "none"}}));
        const cr::Develop custom = from(Json{{"white_balance", "custom"}});
        QCOMPARE(custom.temperature_k, 5500.0);
        const Json clamped = cr::to_json(from(Json{{"white_balance", "custom"}, {"temperature_k", 100}, {"tint", 999}, {"exposure_ev", -9}}));
        QCOMPARE(clamped["temperature_k"], Json(2000.0));
        QCOMPARE(clamped["tint"], Json(150.0));
        QCOMPARE(clamped["exposure_ev"], Json(-5.0));
        // (a temperature only for custom)
        QVERIFY(!cr::to_json(from(Json{{"temperature_k", 3000}})).contains("temperature_k"));
        for (const Json& bad : {Json{{"space", "lab"}}, Json{{"depth", 32}}, Json{{"demosaic", "x"}}, Json{{"exposure_ev", "1"}}, Json{{"what", 1}},
                                Json{{"white_balance", 1}}, Json{{"orientation", "left"}}, Json{{"sharpen", "high"}}, Json::array()}) {
            QVERIFY_THROWS_EXCEPTION(genko::core::OpError, (void)from(bad));
        }
        // (the normal form: an adjustment that changes nothing left out, the camera as given)
        const Json normal = cr::normal_raw(Json{{"develop", Json{{"depth", 8}}}, {"adjust", Json{{"seed", 3}}}, {"camera", Json{{"make", "X"}}}});
        QCOMPARE(normal["develop"]["depth"], Json(8));
        QVERIFY(!normal.contains("adjust"));
        QCOMPARE(normal["camera"], (Json{{"make", "X"}}));
        const Json adjusted = cr::normal_raw(Json{{"adjust", Json{{"exposure", 0.5}}}});
        QCOMPARE(adjusted["adjust"]["exposure"], Json(0.5));
        QCOMPARE(cr::normal_raw(adjusted), adjusted);  // (idempotent)
        QVERIFY_THROWS_EXCEPTION(genko::core::OpError, (void)cr::normal_raw(Json{{"camera", Json{{"lens", "x"}}}}));
        QVERIFY_THROWS_EXCEPTION(genko::core::OpError, (void)cr::normal_raw(Json{{"adjust", Json{{"exposure", "x"}}}}));
        QVERIFY_THROWS_EXCEPTION(genko::core::OpError, (void)cr::normal_raw(Json{{"other", 1}}));
        // (a book's: keys left out allowed, an unknown key or value refused)
        cr::check_raw(Json{{"develop", Json{{"white_balance", "as_shot"}}}, {"camera", Json{{"make", "Test"}}}});
        cr::check_raw(adjusted);
        for (const Json& bad : {Json{{"develop", Json{{"space", "lab"}}}}, Json{{"x", 1}}, Json{{"camera", Json{{"make", 1}}}}, Json{{"adjust", Json{{"exposure", "x"}}}}}) {
            try {
                cr::check_raw(bad);
                QFAIL(bad.dump().c_str());
            } catch (const genko::core::Error& error) {
                QCOMPARE(std::string(error.code()), std::string("value"));
            }
        }
    }
};

QTEST_GUILESS_MAIN(TestRaw)
#include "test_raw.moc"
