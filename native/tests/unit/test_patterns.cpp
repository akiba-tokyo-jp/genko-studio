// PC4 E3 (patterns@1, SPEC.md COLOR-09「パターン」): a pattern laid over the page (render/patterns, PhotoCraft v0.2.0's
// compose/src/pattern.rs and its tests: a whole-pixel placement at 100 % gives the tile exactly; the phase, the scale and
// the angle move it; a scaled one stays seamless), the pattern fill's normal form, the patterns of a .pat file and a
// PSD's Patt block written and read back (psd/src/patterns.rs and io/src/pattern_map.rs's tests: every depth, RGB, grey,
// CMYK and Lab, with and without their transparency; byte-stable; broken input refused without a crash), the machine's
// library (its built-in patterns, a pattern added, renamed, deleted, its pictures kept and tidied, reset), and a pattern's
// grey (a brush's paper from it, brush.texturePattern).

#include <QTemporaryDir>
#include <QtTest>

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "core/error.hpp"
#include "core/exposure.hpp"
#include "core/ink.hpp"
#include "core/patterns.hpp"
#include "render/color_canvas.hpp"
#include "render/ink_canvas.hpp"
#include "render/pat.hpp"
#include "render/pattern_library.hpp"
#include "render/patterns.hpp"
#include "render/png.hpp"

using namespace genko;
using core::Json;
namespace pt = render::patterns;
namespace fs = std::filesystem;

namespace {

// A 4 × 4 checker (upstream's): red, its top left and bottom right 2 × 2 blue; opaque, 8-bit RGB.
pt::Tile checker() {
    pt::Tile t;
    t.mode = "rgb";
    t.depth = 8;
    t.width = 4;
    t.height = 4;
    t.colours = 3;
    for (unsigned y = 0; y < 4; ++y) {
        for (unsigned x = 0; x < 4; ++x) {
            const bool blue = (x < 2 && y < 2) || (x >= 2 && y >= 2);
            t.values.insert(t.values.end(), {blue ? 0.0f : 1.0f, 0.0f, blue ? 1.0f : 0.0f, 1.0f});
        }
    }
    return t;
}

// A tile of a mode and depth: values that differ pixel to pixel (exact at 8 bits), with or without transparency.
pt::Tile tile_of(const std::string& mode, int depth, bool clear) {
    pt::Tile t;
    t.mode = mode;
    t.depth = depth;
    t.width = 5;
    t.height = 3;
    t.colours = mode == "gray" ? 1 : mode == "cmyk" ? 4 : 3;
    for (unsigned y = 0; y < 3; ++y) {
        for (unsigned x = 0; x < 5; ++x) {
            for (unsigned c = 0; c < t.colours; ++c) t.values.push_back(static_cast<float>(((x * 3 + y * 7 + c * 5) % 11) * 25) / 255.0f);
            t.values.push_back(clear ? static_cast<float>(((x + y) % 3) * 85) / 255.0f : 1.0f);
        }
    }
    return t;
}

core::Pattern pattern_of(const pt::Tile& t, const std::string& bytes, double dpi = 100.0) {
    core::Pattern p;
    p.id = "pat-1";
    p.name = "市松";
    p.asset = core::bytes_ref(bytes);
    p.mode = t.mode;
    p.depth = t.depth;
    p.width = t.width;
    p.height = t.height;
    p.dpi = dpi;
    return p;
}

std::array<float, 4> at(const pt::Laid& laid, int x, int y) {
    float v[5];
    laid.at(x, y, v);
    return {v[0], v[1], v[2], v[3]};
}

const std::array<float, 4> kBlue{0, 0, 1, 1}, kRed{1, 0, 0, 1};

std::string error_of(const std::function<void()>& f) {
    try {
        f();
    } catch (const core::Error& error) {
        return error.what();
    }
    return {};
}

}  // namespace

class TestPatterns : public QObject {
    Q_OBJECT

private slots:
    void init() { pt::forget(); }

    // pattern.rs identity_placement_tiles_exactly: at 100 % on a grid of the pattern's dpi, from the page's corner, each
    // pixel is the tile's (wrapping round to the left of the corner too).
    void aWholePixelPlacementGivesTheTile() {
        const pt::Tile t = checker();
        const std::string bytes = pt::encode(t);
        const core::Pattern p = pattern_of(t, bytes);
        const auto keep = std::make_shared<const std::string>(bytes);
        const pt::Laid laid = pt::laid(p, keep, Json{{"id", "pat-1"}}, pt::Target::srgb(), 100.0);
        QCOMPARE(at(laid, -4, 0), kBlue);  // (x = -4 wraps to 0)
        QCOMPARE(at(laid, 2, 0), kRed);
        QCOMPARE(at(laid, 4, 0), kBlue);
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) QCOMPARE(at(laid, x, y), ((x % 4 < 2) == (y % 4 < 2)) ? kBlue : kRed);
        // (a grid of twice the dpi at 50 %: the same pixels again; a phase of whole grid pixels moves it by them)
        const pt::Laid half = pt::laid(p, keep, Json{{"id", "pat-1"}, {"scale", 50}}, pt::Target::srgb(), 200.0);
        for (int x = 0; x < 8; ++x) QCOMPARE(at(half, x, 1), (x % 4 < 2) ? kBlue : kRed);
        const pt::Laid moved = pt::laid(p, keep, Json{{"id", "pat-1"}, {"phase_mm", Json::array({25.4 / 100.0, 0})}}, pt::Target::srgb(), 100.0);
        for (int x = 1; x < 9; ++x) QCOMPARE(at(moved, x, 0), ((x - 1) % 4 < 2) ? kBlue : kRed);
    }

    // pattern.rs link_phase_scale_and_angle_move_the_origin: the phase moves its start (mm: 2 grid pixels at 100 dpi), the
    // scale makes its pixels larger, 90° turns x and y.
    void thePhaseTheScaleAndTheAngleMoveIt() {
        const pt::Tile t = checker();
        const std::string bytes = pt::encode(t);
        const core::Pattern p = pattern_of(t, bytes);
        const auto keep = std::make_shared<const std::string>(bytes);
        const double two_px = 2 * 25.4 / 100.0;
        const pt::Laid phased = pt::laid(p, keep, Json{{"id", "pat-1"}, {"phase_mm", Json::array({two_px, 0})}}, pt::Target::srgb(), 100.0);
        QCOMPARE(at(phased, 2, 0), kBlue);
        const pt::Laid scaled = pt::laid(p, keep, Json{{"id", "pat-1"}, {"scale", 200}}, pt::Target::srgb(), 100.0);
        QCOMPARE(at(scaled, 2, 1), kBlue);
        QCOMPARE(at(scaled, 6, 1), kRed);
        const pt::Laid turned = pt::laid(p, keep, Json{{"id", "pat-1"}, {"angle", 90}}, pt::Target::srgb(), 100.0);
        const auto [u, v] = turned.map(1.0, 3.0);
        QVERIFY2(std::fabs(u - -3.0) < 1e-9 && std::fabs(v - 1.0) < 1e-9, qPrintable(QString::number(u) + "," + QString::number(v)));
        // (seamless when scaled: the pixels across a tile's edge are as smooth as inside it — no seam of transparency)
        const pt::Laid smooth = pt::laid(p, keep, Json{{"id", "pat-1"}, {"scale", 137}}, pt::Target::srgb(), 100.0);
        for (int x = 0; x < 40; ++x) QCOMPARE(at(smooth, x, 3)[3], 1.0f);
    }

    // The three ways a page holds colours: an RGB page's values, a precise picture's linear light, a page of inks — a
    // pattern of the page's mode as it is, an RGB one by the page's rules.
    void theTargetsHoldTheirOwnValues() {
        const pt::Tile cmyk = tile_of("cmyk", 16, false);
        const std::string bytes = pt::encode(cmyk);
        const core::Pattern p = pattern_of(cmyk, bytes);
        const auto keep = std::make_shared<const std::string>(bytes);
        core::PageColor color;
        color.space = core::InkSpace::Cmyk;
        color.depth = 16;
        const pt::Laid inks = pt::laid(p, keep, Json{{"id", "pat-1"}}, pt::Target::inks(color), 100.0);
        float v[5];
        inks.at(1, 2, v);
        for (unsigned c = 0; c < 4; ++c) QVERIFY(std::fabs(v[c] - cmyk.values[(2 * 5 + 1) * 5 + c]) < 1e-6);  // (its values as they are)
        const render::InkPicture picture = pt::inks(inks, render::Box{0, 0, 5, 3});
        QCOMPARE(picture.channels, 4u);
        QCOMPARE(picture.values.size(), std::size_t(5 * 3 * 5));
        // an 8-bit RGB pattern on a 16-bit page: its values as the page's (sRGB: linear light by the curve)
        const pt::Tile rgb = tile_of("rgb", 8, false);
        const std::string rgb_bytes = pt::encode(rgb);
        const core::Pattern q = pattern_of(rgb, rgb_bytes);
        const pt::Laid deep = pt::laid(q, std::make_shared<const std::string>(rgb_bytes), Json{{"id", "pat-1"}}, pt::Target::srgb(), 100.0);
        const render::ColorPicture lin = pt::linear(deep, render::Box{0, 0, 5, 3}, render::RgbSpace());
        QVERIFY(std::fabs(lin.pixels[7][0] - core::srgb_to_linear(rgb.values[7 * 4])) < 1e-6);
        // the made tiles are kept, and forgotten
        QVERIFY(pt::kept() >= 2);
        pt::forget();
        QCOMPARE(pt::kept(), std::size_t(0));
    }

    // A pattern fill's normal form (core::pattern_fill_spec), and what it refuses.
    void aPatternFillInItsNormalForm() {
        QCOMPARE(core::pattern_fill_spec(Json{{"id", "pat-1"}}),
                 (Json{{"id", "pat-1"}, {"scale", 100}, {"angle", 0}, {"phase_mm", Json::array({0, 0})}, {"link", true}}));
        QCOMPARE(core::pattern_fill_spec(Json{{"id", "p"}, {"scale", 12.5}, {"angle", -30}, {"phase_mm", Json::array({1.5, 2})}, {"link", false}}),
                 (Json{{"id", "p"}, {"scale", 12.5}, {"angle", -30}, {"phase_mm", Json::array({1.5, 2})}, {"link", false}}));
        for (const Json& bad : {Json{{"scale", 100}}, Json{{"id", "a b"}}, Json{{"id", "p"}, {"scale", 0.5}}, Json{{"id", "p"}, {"scale", 1001}},
                                Json{{"id", "p"}, {"angle", 361}}, Json{{"id", "p"}, {"phase_mm", Json::array({1})}},
                                Json{{"id", "p"}, {"link", 1}}, Json{{"id", "p"}, {"offset", 1}}, Json("pat-1")}) {
            QVERIFY2(!error_of([&] { (void)core::pattern_fill_spec(bad); }).empty(), bad.dump().c_str());
        }
    }

    // patterns.rs block_and_pat_file_round_trip_at_every_depth and pattern_map.rs
    // patterns_round_trip_through_psd_at_every_depth_and_model: each pattern written as a file's and read back the same.
    void patternsGoThroughPatFilesAndPattBlocks() {
        for (const std::string mode : {"rgb", "gray", "cmyk", "lab"}) {
            for (const int depth : {8, 16, 32}) {
                for (const bool clear : {false, true}) {
                    const pt::Tile t = tile_of(mode, depth, clear);
                    const std::string bytes = pt::encode(t);
                    const core::Pattern p = pattern_of(t, bytes, 300);
                    const render::pat::PsdPattern written = render::pat::to_psd(p, bytes, render::pat::file_id(p));
                    QCOMPARE(written.alpha.has_value(), clear);
                    const std::string what = mode + " " + std::to_string(depth) + (clear ? " clear" : "");
                    for (const bool block : {false, true}) {
                        const std::vector<render::pat::PsdPattern> read =
                            block ? render::pat::parse_block(render::pat::write_block({written})) : render::pat::parse_file(render::pat::write_file({written}));
                        QCOMPARE(read.size(), std::size_t(1));
                        QVERIFY2(read[0] == written, what.c_str());
                        const auto taken = render::pat::from_psd(read[0], 300);
                        QVERIFY2(taken.has_value(), what.c_str());
                        QCOMPARE(taken->pattern.mode, p.mode);
                        QCOMPARE(taken->pattern.depth, p.depth);
                        QCOMPARE(taken->pattern.width, p.width);
                        QCOMPARE(taken->pattern.name, p.name);
                        const pt::Tile back = pt::decode(taken->pattern, *taken->bytes);
                        QCOMPARE(back.values.size(), t.values.size());
                        for (std::size_t i = 0; i < t.values.size(); ++i) {
                            QVERIFY2(std::fabs(back.values[i] - t.values[i]) < (depth == 32 ? 1e-6 : 1.0 / 65535 + 1e-6), (what + " at " + std::to_string(i)).c_str());
                        }
                    }
                }
            }
        }
        // a block written again from what it read is the same bytes
        const pt::Tile t = tile_of("rgb", 8, true);
        const std::string bytes = pt::encode(t);
        const core::Pattern p = pattern_of(t, bytes);
        const std::string block = render::pat::write_block({render::pat::to_psd(p, bytes, "0bd2d3ba-1234-11d4-8f8f-aabbccddeeff")});
        QCOMPARE(block.size() % 4, std::size_t(0));
        QCOMPARE(render::pat::write_block(render::pat::parse_block(block)), block);
        QCOMPARE(render::pat::block_key(8), std::string_view("Patt"));
        QCOMPARE(render::pat::block_key(16), std::string_view("Pat2"));
        QCOMPARE(render::pat::block_key(32), std::string_view("Pat3"));
        // its unique id: a UUID kept, else one made from its picture and id (the same each time)
        QCOMPARE(render::pat::file_id(p), render::pat::file_id(p));
        QCOMPARE(render::pat::file_id(p).size(), std::size_t(36));
        core::Pattern read_one = p;
        read_one.id = "0bd2d3ba-1234-11d4-8f8f-aabbccddeeff";
        QCOMPARE(render::pat::file_id(read_one), read_one.id);
        QCOMPARE(render::pat::shown_name("$$$/Patterns/Water=Water"), std::string("Water"));
    }

    // patterns.rs malformed_input_errors_without_panicking: cut short anywhere, refused (core::Error), never a crash.
    void brokenInputIsRefused() {
        const pt::Tile t = tile_of("rgb", 8, true);
        const std::string bytes = pt::encode(t);
        const std::string block = render::pat::write_block({render::pat::to_psd(pattern_of(t, bytes), bytes, "x")});
        for (const std::size_t cut : {std::size_t(3), std::size_t(10), std::size_t(40), block.size() - 5}) {
            try {
                (void)render::pat::parse_block(block.substr(0, cut));
            } catch (const core::Error&) {
            }
        }
        QVERIFY(!error_of([] { (void)render::pat::parse_file(std::string("8BPS\0\x01", 6)); }).empty());
        QVERIFY(render::pat::parse_block("").empty());
        // a pattern of no pixels, or past 8192 a side: not taken
        render::pat::PsdPattern none;
        std::string why;
        QVERIFY(!render::pat::from_psd(none, 300, &why).has_value());
        QVERIFY(!why.empty());
    }

    // The machine's library: its built-in groups and patterns (made by the program, not files); one added (its picture
    // kept once in patterns/), renamed, deleted (its picture tidied away), the library reset; an item refused.
    void theLibrary() {
        namespace lib = render::pattern_library;
        namespace store = render::preset_store;
        QTemporaryDir tmp;
        const fs::path dir = fs::path(tmp.path().toStdString()) / "config";
        store::Library l = lib::load(dir);
        QVERIFY(!l.from_file && l.problem.empty());
        QCOMPARE(l.groups.size(), std::size_t(2));
        QCOMPARE(l.groups[0].name, std::string("幾何学"));
        QCOMPARE(l.groups[1].name, std::string("テクスチャ"));
        QCOMPARE(lib::builtins().size(), std::size_t(7));
        for (const lib::Builtin& b : lib::builtins()) {
            QVERIFY(!core::check_pattern_file(b.pattern, *b.bytes).has_value());
            QVERIFY(lib::picture(dir, b.pattern) != nullptr);
        }
        // (the same pictures each time: upstream's hash and the 8-bit steps)
        QCOMPARE(lib::builtins()[0].pattern.asset, lib::builtins()[0].pattern.asset);
        const pt::Tile t = tile_of("cmyk", 16, true);
        const std::string bytes = pt::encode(t);
        core::Pattern p = pattern_of(t, bytes, 200);
        p.id.clear();
        p.name = "インキ";
        const Json added = lib::add(dir, l.groups, p, std::make_shared<const std::string>(bytes), std::string_view("自分の"));
        QCOMPARE(added, (Json{{"name", "インキ"}, {"group", "自分の"}}));
        lib::save(dir, l.groups);
        QVERIFY(fs::exists(dir / "patterns" / (p.asset.substr(7) + ".gkir")));
        l = lib::load(dir);
        QVERIFY(l.from_file);
        const Json* item = store::find(l.groups, "インキ");
        QVERIFY(item != nullptr);
        const core::Pattern back = lib::pattern_of(*item);
        QCOMPARE(back.mode, std::string("cmyk"));
        QCOMPARE(back.dpi, 200.0);
        QVERIFY(lib::picture(dir, back) != nullptr);
        QCOMPARE(lib::add_op(back, lib::picture(dir, back)).value("op", std::string()), std::string("add_pattern"));
        QVERIFY(lib::add_op(back, lib::picture(dir, back)).contains("gkir"));
        store::edit(l.groups, lib::kind(), Json{{"action", "rename"}, {"preset", "インキ"}, {"name", "版"}});
        store::edit(l.groups, lib::kind(), Json{{"action", "delete"}, {"preset", "版"}});
        lib::save(dir, l.groups);
        lib::tidy(dir, l.groups);
        QVERIFY(!fs::exists(dir / "patterns" / (p.asset.substr(7) + ".gkir")));
        l.groups = store::reset(l.groups, lib::kind(), false);
        QCOMPARE(l.groups.size(), std::size_t(2));
        // an item its kind refuses
        QVERIFY(!error_of([] { (void)lib::pattern_of(Json{{"name", "x"}, {"mode", "hsv"}}); }).empty());
        // (list --full: each pattern's size, mode, depth and dpi)
        const Json full = lib::list(l.groups);
        QCOMPARE(full["groups"][0]["patterns"][0]["width"], Json(16));
    }

    // brush.texturePattern: a pattern's grey — its lightness over white by its alpha.
    void aPatternsGrey() {
        const pt::Tile t = checker();
        const std::string bytes = pt::encode(t);
        const std::string grey = pt::grey(pattern_of(t, bytes), bytes);
        QCOMPARE(grey.size(), std::size_t(16));
        QCOMPARE(static_cast<int>(static_cast<unsigned char>(grey[0])), static_cast<int>(std::lround(0.114 * 255)));  // (blue)
        QCOMPARE(static_cast<int>(static_cast<unsigned char>(grey[2])), static_cast<int>(std::lround(0.299 * 255)));  // (red)
        pt::Tile clear = checker();
        for (std::size_t i = 0; i < 16; ++i) clear.values[i * 4 + 3] = 0.0f;
        const std::string clear_bytes = pt::encode(clear);
        QCOMPARE(static_cast<unsigned char>(pt::grey(pattern_of(clear, clear_bytes), clear_bytes)[0]), static_cast<unsigned char>(255));  // (white)
        const render::Image swatch = pt::swatch(pattern_of(t, bytes), bytes, 16);
        QCOMPARE(swatch.size(), (render::Size{16, 16}));
    }
};

QTEST_GUILESS_MAIN(TestPatterns)
#include "test_patterns.moc"
