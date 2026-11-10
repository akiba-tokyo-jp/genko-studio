// render/quantize (color-model@1, PC1d: インデックスカラー・2 階調): PhotoCraft v0.2.0's own tests of
// crates/algo/src/quantize.rs (the fixed palettes' sizes, Exact and its refusal past 256 colours, the computed palettes'
// counts and forced colours, every dither onto the palette, the bitmap methods), and the same bits as PhotoCraft's: the
// reference values below are PhotoCraft's quantize.rs itself (copied as it is, built with rustc -C opt-level=3 on
// x86-64) run on the same pictures — its palettes, quantized pixels, bitmaps, Color Table presets and Lab, hashed
// (FNV-1a 64) — so the port gives PhotoCraft's pixels, and the same everywhere (render/libm_float).

#include <QtTest>

#include <bit>
#include <cmath>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "core/error.hpp"
#include "render/libm_float.hpp"
#include "render/quantize.hpp"

using namespace genko::render;
namespace q = genko::render::quantize;

namespace {

std::uint64_t fnv(const std::vector<std::uint8_t>& bytes) {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (const std::uint8_t b : bytes) {
        h ^= b;
        h *= 0x100000001b3ULL;
    }
    return h;
}

std::vector<std::uint8_t> palette_bytes(const std::vector<q::Rgb8>& p) {
    std::vector<std::uint8_t> out;
    for (const q::Rgb8& c : p) out.insert(out.end(), c.begin(), c.end());
    return out;
}

std::vector<std::uint8_t> pixel_bytes(const std::vector<q::Rgba>& px) {
    std::vector<std::uint8_t> out;
    for (const q::Rgba& p : px) {
        for (const float v : p) {
            const auto bits = std::bit_cast<std::uint32_t>(v);
            for (int k = 0; k < 4; ++k) out.push_back(static_cast<std::uint8_t>(bits >> (8 * k)));
        }
    }
    return out;
}

// (the reference program's picture: gradients, a third colour in steps, an alpha ramp in the last two rows)
std::vector<q::Rgba> picture(std::size_t w, std::size_t h) {
    std::vector<q::Rgba> out;
    for (std::size_t i = 0; i < w * h; ++i) {
        const float x = static_cast<float>(i % w), y = static_cast<float>(i / w);
        const float r = x / static_cast<float>(w - 1), g = y / static_cast<float>(h - 1);
        const float b = std::fmod(x * 7.0f + y * 3.0f, 17.0f) / 16.0f;
        const float a = static_cast<std::size_t>(y) >= h - 2 ? x / static_cast<float>(w - 1) : 1.0f;
        out.push_back({r, g, b, a});
    }
    return out;
}

std::vector<q::Rgba> gradient(std::size_t w, std::size_t h) {
    std::vector<q::Rgba> out;
    for (std::size_t i = 0; i < w * h; ++i) {
        out.push_back({static_cast<float>(i % w) / static_cast<float>(w - 1), static_cast<float>(i / w) / static_cast<float>(h - 1), 0.3f, 1.0f});
    }
    return out;
}

std::string hex(std::uint64_t v) { return QString::number(v, 16).rightJustified(16, '0').toStdString(); }

}  // namespace

class QuantizeTest : public QObject {
    Q_OBJECT

private slots:
    // quantize.rs fixed_palettes_have_expected_sizes
    void fixedPalettesHaveTheirSizes() {
        QCOMPARE(q::web_palette().size(), std::size_t(216));
        QCOMPARE(q::mac_palette().size(), std::size_t(256));
        QCOMPARE(q::windows_palette().size(), std::size_t(256));
        QCOMPARE(q::uniform_palette(27).size(), std::size_t(27));
        const auto mac = q::mac_palette();
        QCOMPARE(std::set<q::Rgb8>(mac.begin(), mac.end()).size(), std::size_t(256));  // (unique)
    }

    // quantize.rs exact_palette_and_failure
    void exactPaletteAndItsRefusal() {
        const std::vector<q::Rgba> px{{1, 0, 0, 1}, {0, 0, 1, 1}, {1, 0, 0, 1}};
        QCOMPARE(q::build_palette(px, q::PaletteKind::Exact, 256, q::Forced::None).size(), std::size_t(2));
        bool refused = false;
        try {
            (void)q::build_palette(gradient(64, 64), q::PaletteKind::Exact, 256, q::Forced::None);
        } catch (const genko::core::Error& error) {
            refused = std::string(error.what()).find("Exact needs 256 or fewer") != std::string::npos;
        }
        QVERIFY(refused);
    }

    // quantize.rs adaptive_and_perceptual_respect_count_and_forced
    void computedPalettesKeepTheCountAndTheForcedColours() {
        const auto px = gradient(40, 40);
        for (const auto kind : {q::PaletteKind::Adaptive, q::PaletteKind::Perceptual, q::PaletteKind::Selective}) {
            const auto pal = q::build_palette(px, kind, 16, q::Forced::BlackWhite);
            QVERIFY(pal.size() <= 16 && pal.size() >= 8);
            QCOMPARE(pal[0], (q::Rgb8{0, 0, 0}));
            QCOMPARE(pal[1], (q::Rgb8{255, 255, 255}));
        }
    }

    // quantize.rs forced_colours_filling_the_count_do_not_panic
    void forcedColoursFillingTheCount() {
        const std::vector<q::Rgba> white(16, q::Rgba{1, 1, 1, 1});
        for (const auto kind : {q::PaletteKind::Adaptive, q::PaletteKind::Perceptual, q::PaletteKind::Selective}) {
            QCOMPARE(q::build_palette(white, kind, 2, q::Forced::BlackWhite), (std::vector<q::Rgb8>{{0, 0, 0}, {255, 255, 255}}));
            QCOMPARE(q::build_palette(gradient(8, 8), kind, 8, q::Forced::Primaries).size(), std::size_t(8));
        }
    }

    // quantize.rs quantize_maps_to_palette_with_each_dither
    void everyDitherMapsOntoThePalette() {
        const std::vector<q::Rgb8> pal{{0, 0, 0}, {255, 255, 255}};
        for (const auto d : {q::Dither::None, q::Dither::Diffusion, q::Dither::Pattern, q::Dither::Noise}) {
            std::vector<q::Rgba> px;
            for (int i = 0; i < 64; ++i) {
                const float v = static_cast<float>(i) / 63.0f;
                px.push_back({v, v, v, 1.0f});
            }
            const auto idx = q::quantize(px, 8, pal, d, 1.0f, std::nullopt);
            for (const auto& p : px) QVERIFY(p[0] == 0.0f || p[0] == 1.0f);
            QCOMPARE(idx.size(), std::size_t(64));
            if (d == q::Dither::Diffusion) {  // (diffusion keeps the mean brightness; a plain threshold does not)
                float mean = 0.0f;
                for (const auto& p : px) mean += p[0];
                QVERIFY(std::abs(mean / 64.0f - 0.5f) < 0.08f);
            }
        }
        std::vector<q::Rgba> clear{{1, 1, 1, 0.2f}};
        const auto idx = q::quantize(clear, 1, pal, q::Dither::None, 1.0f, std::size_t(1));
        QCOMPARE(int(idx[0]), 1);
        QCOMPARE(clear[0][3], 0.0f);
    }

    // quantize.rs bitmap_methods_are_binary_and_track_tone
    void bitmapMethodsAreBinaryAndFollowTheTone() {
        using K = q::BitmapMethod::Kind;
        for (const K kind : {K::Threshold, K::Pattern, K::Diffusion, K::Halftone}) {
            q::BitmapMethod m;
            m.kind = kind;
            m.cell = 6.0f;
            m.angle = 45.0f;
            std::vector<float> g(32 * 32, 0.25f);
            q::to_bitmap(g, 32, m);
            float mean = 0.0f;
            for (const float v : g) {
                QVERIFY(v == 0.0f || v == 1.0f);
                mean += v;
            }
            mean /= static_cast<float>(g.size());
            if (kind == K::Threshold) QCOMPARE(mean, 0.0f);
            else QVERIFY(std::abs(mean - 0.25f) < 0.15f);
        }
    }

    void bayerIsAnOrderedMatrix() {
        QCOMPARE(q::bayer8(0, 0), (0.0f + 0.5f) / 64.0f - 0.5f);
        std::set<float> seen;
        float sum = 0.0f;
        for (std::size_t y = 0; y < 8; ++y) {
            for (std::size_t x = 0; x < 8; ++x) {
                seen.insert(q::bayer8(x, y));
                sum += q::bayer8(x, y);
                QCOMPARE(q::bayer8(x + 8, y + 16), q::bayer8(x, y));  // (it repeats every 8)
            }
        }
        QCOMPARE(seen.size(), std::size_t(64));
        QVERIFY(std::abs(sum) < 1e-5f);
    }

    // The same bits as PhotoCraft's own quantize.rs on the same picture (see the file's comment).
    void palettesAreThoseOfPhotoCraft() {
        const auto px = picture(37, 23);
        const std::vector<std::tuple<const char*, q::PaletteKind, std::size_t, std::uint64_t>> want{
            {"systemMac", q::PaletteKind::SystemMac, 256, 0x1ae56e1795d562f7ULL},
            {"systemWindows", q::PaletteKind::SystemWindows, 256, 0xe91cc9c38533abb1ULL},
            {"web", q::PaletteKind::Web, 216, 0x5381e3035b4e351fULL},
            {"uniform", q::PaletteKind::Uniform, 8, 0x1a8811fb9bf8992fULL},
            {"perceptual", q::PaletteKind::Perceptual, 24, 0xbfc67e46e6808dcfULL},
            {"selective", q::PaletteKind::Selective, 24, 0x0307a81b0a61e99dULL},
            {"adaptive", q::PaletteKind::Adaptive, 24, 0xad005458304b5e1bULL},
        };
        for (const auto& [name, kind, size, hash] : want) {
            const auto pal = q::build_palette(px, kind, 24, q::Forced::BlackWhite);
            QVERIFY2(pal.size() == size, name);
            QVERIFY2(fnv(palette_bytes(pal)) == hash, (std::string(name) + " " + hex(fnv(palette_bytes(pal)))).c_str());
        }
        bool refused = false;
        try {
            (void)q::build_palette(px, q::PaletteKind::Exact, 24, q::Forced::BlackWhite);
        } catch (const genko::core::Error& error) {
            refused = std::string(error.what()) == "the image has 815 colours; Exact needs 256 or fewer";
        }
        QVERIFY(refused);
        QCOMPARE(fnv(palette_bytes(q::uniform_palette(27))), 0x2d245bb22bc5b954ULL);
    }

    void quantizedPixelsAreThoseOfPhotoCraft() {
        const auto px = picture(37, 23);
        const auto pal = q::build_palette(px, q::PaletteKind::Perceptual, 24, q::Forced::BlackWhite);
        const std::vector<std::tuple<const char*, q::Dither, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t>> want{
            {"none", q::Dither::None, 0xc6e8eadf0ecba352ULL, 0xfb638626ae71055aULL, 0x06410a743a2c1ac8ULL, 0xb84cff6eced26b56ULL},
            {"diffusion", q::Dither::Diffusion, 0x353c2d996daeeeadULL, 0xdbd968fe3085c1a3ULL, 0x53e10fc9d87dd7a6ULL, 0x59ecde76da077d11ULL},
            {"pattern", q::Dither::Pattern, 0xb40d2b7b88427403ULL, 0x398b5082317571c4ULL, 0xca95582c996d741eULL, 0xd4a0fcc8c66bbfc4ULL},
            {"noise", q::Dither::Noise, 0x8234aca19e3d2f17ULL, 0x9ce8dfe0357476e6ULL, 0x39b90338f886ed33ULL, 0x678ef2a503743b88ULL},
        };
        for (const auto& [name, dither, idx_hash, px_hash, opaque_idx, opaque_px] : want) {
            auto a = px;
            const auto idx = q::quantize(a, 37, pal, dither, 0.75f, pal.size() - 1);
            QVERIFY2(fnv(idx) == idx_hash && fnv(pixel_bytes(a)) == px_hash, (std::string(name) + " " + hex(fnv(idx)) + " " + hex(fnv(pixel_bytes(a)))).c_str());
            auto b = px;
            const auto idx2 = q::quantize(b, 37, pal, dither, 1.0f, std::nullopt);
            QVERIFY2(fnv(idx2) == opaque_idx && fnv(pixel_bytes(b)) == opaque_px, name);
        }
    }

    void bitmapsAreThoseOfPhotoCraft() {
        const auto px = picture(37, 23);
        std::vector<float> grey;
        for (const auto& p : px) grey.push_back(0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]);
        using K = q::BitmapMethod::Kind;
        using S = q::HalftoneShape;
        const std::vector<std::tuple<const char*, q::BitmapMethod, std::uint64_t>> want{
            {"threshold", {K::Threshold, 6, 45, S::Round}, 0x0d4b1e1f4879e681ULL},
            {"pattern", {K::Pattern, 6, 45, S::Round}, 0x82613f460fa9c7bdULL},
            {"diffusion", {K::Diffusion, 6, 45, S::Round}, 0xbeb031535de4e5d4ULL},
            {"halftone-round", {K::Halftone, 300.0f / 53.0f, 45, S::Round}, 0x4c64f3d485edf9edULL},
            {"halftone-ellipse", {K::Halftone, 7.5f, -30, S::Ellipse}, 0x628b2b44f8ec9674ULL},
            {"halftone-line", {K::Halftone, 4.0f, 90, S::Line}, 0x11f356ac12b76175ULL},
            {"halftone-square", {K::Halftone, 5.0f, 15, S::Square}, 0x03b30ba16e374645ULL},
            {"halftone-diamond", {K::Halftone, 6.0f, 0, S::Diamond}, 0x42e1950c96a47c85ULL},
            {"halftone-cross", {K::Halftone, 9.0f, 180, S::Cross}, 0xdb529343888b97caULL},
        };
        for (const auto& [name, method, hash] : want) {
            auto g = grey;
            q::to_bitmap(g, 37, method);
            std::vector<std::uint8_t> bits;
            for (const float v : g) bits.push_back(v > 0.5f ? 1 : 0);
            QVERIFY2(fnv(bits) == hash, (std::string(name) + " " + hex(fnv(bits))).c_str());
        }
    }

    void colorTablePresetsAreThoseOfPhotoCraft() {
        const std::vector<std::tuple<const char*, std::size_t, std::uint64_t>> want{
            {"grayscale", 256, 0x3ac8574c48f9b025ULL},     {"blackBody", 256, 0xb2f788926ea88041ULL},
            {"spectrum", 256, 0x23549fcf074c6f47ULL},      {"systemMac", 256, 0x9d42193bd0221b91ULL},
            {"systemWindows", 256, 0x12bf2b07d6c791e7ULL}, {"web", 216, 0x636eabed373a35a9ULL},
        };
        for (const auto& [name, size, hash] : want) {
            const auto table = q::preset_table(name);
            QVERIFY2(table && table->size() == size, name);
            QVERIFY2(fnv(palette_bytes(*table)) == hash, name);
        }
        QVERIFY(!q::preset_table("custom"));
    }

    void labAndTheCubeRootAreThoseOfPhotoCraft() {
        const std::vector<std::pair<std::array<float, 3>, std::array<std::uint32_t, 6>>> want{
            {{0.0f, 0.0f, 0.0f}, {0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000}},
            {{1.0f, 1.0f, 1.0f}, {0x42c80000, 0xb9abe000, 0x38960000, 0x3f7fffd1, 0x3f7fffff, 0x3f7ffff7}},
            {{0.5f, 0.25f, 0.75f}, {0x422224d4, 0x42385628, 0xc2647673, 0x3effffb4, 0x3e800073, 0x3f3ffffd}},
            {{0.01f, 0.02f, 0.005f}, {0x3f96eea0, 0xbf8b68f7, 0x3f9a8b61, 0x3c23d6c3, 0x3ca3d719, 0x3ba3d6d9}},
            {{0.9f, 0.1f, 0.2f}, {0x42481966, 0x42917ebc, 0x422ecdaa, 0x3f66665b, 0x3dcccd37, 0x3e4ccca4}},
        };
        for (const auto& [rgb, bits] : want) {
            const auto lab = q::srgb_to_lab(rgb);
            const auto back = q::lab_to_srgb(lab);
            for (int k = 0; k < 3; ++k) {
                QCOMPARE(std::bit_cast<std::uint32_t>(lab[k]), bits[k]);
                QCOMPARE(std::bit_cast<std::uint32_t>(back[k]), bits[3 + k]);
            }
        }
        const std::vector<std::pair<float, std::uint32_t>> roots{{1.0f, 0x3f800000}, {2.0f, 0x3fa14518},     {27.0f, 0x40400000},
                                                                {64.0f, 0x40800000}, {216.0f, 0x40c00000},  {256.0f, 0x40cb2ff5},
                                                                {0.008856f, 0x3e53dbc5}, {0.3f, 0x3f2b5ff5}, {0.75f, 0x3f689768}};
        for (const auto& [v, bits] : roots) QCOMPARE(std::bit_cast<std::uint32_t>(libm::cbrtf(v)), bits);
        QCOMPARE(libm::cbrtf(-8.0f), -2.0f);
        QCOMPARE(libm::cbrtf(0.0f), 0.0f);
    }

    void idsAreRead() {
        QCOMPARE(q::palette_kind("localSelective"), std::optional<q::PaletteKind>(q::PaletteKind::Selective));
        QCOMPARE(q::palette_kind("system"), std::optional<q::PaletteKind>(q::PaletteKind::SystemMac));
        QVERIFY(!q::palette_kind("rainbow"));
        QCOMPARE(q::forced_of("blackAndWhite"), std::optional<q::Forced>(q::Forced::BlackWhite));
        QCOMPARE(q::dither_of("noise"), std::optional<q::Dither>(q::Dither::Noise));
        QVERIFY(q::halftone_shape("diamond") == q::HalftoneShape::Diamond);
        QVERIFY(q::halftone_shape("star") == q::HalftoneShape::Round);
    }
};

QTEST_GUILESS_MAIN(QuantizeTest)
#include "test_quantize.moc"
