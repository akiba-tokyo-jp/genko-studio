// PC4 E2 (brush.presets.importAbr, assets:adobe-presets; SPEC.md COLOR-09「ブラシのプリセット」): Photoshop brush files read
// whole (render/abr parse: versions 1 and 2 with computed and sampled tips of 8 and 16 bits, version 6's tips, patterns
// and brushPreset descriptors) and their brushes as dab brushes (render/abr_map) — PhotoCraft v0.2.0's crates/psd/src/
// abr.rs tests and crates/io/src/abr_map/tests.rs, the files made by its writers (write_v12, write_v6) ported here; a
// broken or huge file refused before it is unpacked, never read past its end.

#include <QElapsedTimer>
#include <QtTest/QtTest>

#include <chrono>

#include "core/brush_dabs.hpp"
#include "core/error.hpp"
#include "render/abr.hpp"
#include "render/abr_map.hpp"
#include "render/dabs/tile.hpp"
#include "render/pat.hpp"
#include "render/psd_descriptor.hpp"

using namespace genko;
using core::Json;
namespace abr = render::abr;
namespace desc = render::psd::descriptor;
namespace dabs = render::dabs;

namespace {

// upstream's psd abr.rs sample(): a tip whose bytes vary
abr::Sample sample(const std::string& id, std::uint32_t w, std::uint32_t h, std::uint16_t depth) {
    abr::Sample s;
    s.id = id;
    s.width = w;
    s.height = h;
    s.depth = depth;
    const std::size_t bpp = depth / 8;
    for (std::size_t i = 0; i < std::size_t(w) * h * bpp; ++i) s.data.push_back(static_cast<char>((i / bpp) % 3 == 0 ? 255 : (i % 7) * 30));
    return s;
}
// abr_map/tests.rs's: every other pixel full paint
abr::Sample checkered(const std::string& id, std::uint32_t w, std::uint32_t h, std::uint16_t depth) {
    abr::Sample s;
    s.id = id;
    s.width = w;
    s.height = h;
    s.depth = depth;
    for (std::size_t i = 0; i < std::size_t(w) * h; ++i) s.data.append(depth / 8, static_cast<char>(i % 2 == 0 ? 255 : 0));
    return s;
}

abr::LegacyBrush computed(std::uint16_t spacing, std::uint16_t diameter, std::uint16_t hardness, std::int16_t angle, std::uint16_t roundness) {
    abr::LegacyBrush b;
    b.spacing = spacing;
    b.computed = true;
    b.diameter = diameter;
    b.hardness = hardness;
    b.angle = angle;
    b.roundness = roundness;
    return b;
}
abr::LegacyBrush sampled(const std::string& name, std::uint16_t spacing, bool aa, abr::Sample s) {
    abr::LegacyBrush b;
    b.name = name;
    b.spacing = spacing;
    b.anti_alias = aa;
    b.sample = std::move(s);
    return b;
}

// descriptor values (abr_map/tests.rs's helpers)
desc::Writer var(int control, double jitter, double minimum) {
    desc::Writer w;
    w.integer("bVTy", control);
    w.integer("fStp", 40);
    w.unit("jitter", "#Prc", jitter);
    w.unit("Mnm ", "#Prc", minimum);
    return w;
}

desc::Writer sampled_tip(const std::u16string& id) {
    desc::Writer t;
    t.unit("Dmtr", "#Pxl", 64.0);
    t.unit("Angl", "#Ang", 30.0);
    t.unit("Rndn", "#Prc", 80.0);
    t.unit("Spcn", "#Prc", 40.0);
    t.boolean("Intr", true);
    t.boolean("flipX", true);
    t.boolean("flipY", false);
    t.text("sampledData", id);
    return t;
}

// a preset touching every section
desc::Writer full_preset() {
    desc::Writer p;
    p.text("Nm  ", u"Kitchen Sink");
    p.object("Brsh", sampled_tip(u"$tip-1"), "sampledBrush");
    p.boolean("useTipDynamics", true);
    p.boolean("flipX", true);
    p.boolean("flipY", false);
    p.unit("minimumDiameter", "#Prc", 25.0);
    p.unit("minimumRoundness", "#Prc", 10.0);
    p.unit("tiltScale", "#Prc", 0.0);
    p.object("szVr", var(2, 20.0, 0.0), "brVr");
    p.object("angleDynamics", var(6, 10.0, 0.0), "brVr");
    p.object("roundnessDynamics", var(3, 5.0, 0.0), "brVr");
    p.boolean("useScatter", true);
    p.integer("Cnt ", 3);
    p.boolean("bothAxes", true);
    p.object("countDynamics", var(0, 50.0, 0.0), "brVr");
    p.object("scatterDynamics", var(1, 250.0, 0.0), "brVr");
    p.boolean("useTexture", true);
    p.boolean("TxtC", true);
    p.boolean("InvT", true);
    p.unit("textureScale", "#Prc", 150.0);
    p.integer("textureBrightness", -75);
    p.integer("textureContrast", 25);
    p.enumerated("textureBlendMode", "BlnM", "CBrn");
    p.unit("textureDepth", "#Prc", 60.0);
    p.unit("minimumDepth", "#Prc", 20.0);
    p.object("textureDepthDynamics", var(2, 30.0, 0.0), "brVr");
    {
        desc::Writer t;
        t.text("Nm  ", u"Grit");
        t.text("Idnt", u"pat-uuid");
        p.object("Txtr", t, "Ptrn");
    }
    {
        desc::Writer d;
        d.boolean("useDualBrush", true);
        d.boolean("Flip", true);
        desc::Writer tip;
        tip.unit("Dmtr", "#Pxl", 12.0);
        tip.unit("Hrdn", "#Prc", 50.0);
        d.object("Brsh", tip, "computedBrush");
        d.enumerated("BlnM", "BlnM", "hardMix");
        d.unit("Spcn", "#Prc", 30.0);
        d.integer("Cnt ", 2);
        d.boolean("bothAxes", true);
        d.object("scatterDynamics", var(0, 120.0, 0.0), "brVr");
        p.object("dualBrush", d, "dualBrush");
    }
    p.boolean("useColorDynamics", true);
    p.object("clVr", var(2, 40.0, 0.0), "brVr");
    p.unit("H   ", "#Prc", 10.0);
    p.unit("Strt", "#Prc", 20.0);
    p.unit("Brgh", "#Prc", 30.0);
    p.unit("purity", "#Prc", -50.0);
    p.boolean("colorDynamicsPerTip", false);
    p.boolean("usePaintDynamics", true);
    p.object("opVr", var(2, 0.0, 10.0), "brVr");
    p.object("prVr", var(1, 15.0, 5.0), "brVr");
    p.object("wtVr", var(0, 0.0, 0.0), "brVr");
    p.boolean("useBrushPose", true);
    p.integer("brushPoseTiltX", 50);
    p.integer("brushPoseAngle", 90);
    p.boolean("overridePoseAngle", true);
    p.boolean("Wtdg", true);
    p.boolean("Nose", true);
    p.boolean("Rpt ", true);
    p.boolean("someFutureKey", true);
    {
        desc::Writer to;
        to.integer("Opct", 80);
        to.integer("flow", 60);
        to.boolean("smoothing", true);
        to.integer("smoothingValue", 40);
        to.boolean("usePressureOverridesSize", true);
        p.object("toolOptions", to, "currentToolOptions");
    }
    return p;
}

render::pat::PsdPattern grit_pattern() {
    render::pat::PsdPattern p;
    p.mode = 1;
    p.width = 4;
    p.height = 2;
    p.name = "Grit";
    p.id = "pat-uuid";
    p.depth = 8;
    p.channels = {std::string("\x00\x40\x80\xff\xff\x80\x40\x00", 8)};
    return p;
}

constexpr double kMm = 25.4 / 200.0;

bool near(double a, double b, double tolerance = 1e-6) { return std::abs(a - b) <= tolerance; }

core::DabSettings dabs_of(const render::abr_map::Preset& p) { return core::dabs_from_json(p.settings["dabs"]); }

bool any_warning(const std::vector<std::string>& warnings, const std::string& part) {
    return std::any_of(warnings.begin(), warnings.end(), [&](const std::string& w) { return w.find(part) != std::string::npos; });
}

dabs::GrayTile tile_of(const render::abr_map::Import& imp, const std::string& ref) { return render::dabs::tile_of_png(*imp.pictures.at(ref)); }

}  // namespace

class TestAbrMap : public QObject {
    Q_OBJECT

private slots:
    // psd/src/abr.rs v1_v2_round_trip_raw_and_rle
    void versionsOneAndTwoRoundTrip() {
        for (const int version : {1, 2}) {
            for (const bool rle : {false, true}) {
                const std::vector<abr::LegacyBrush> brushes{computed(25, 19, 80, -30, 50),
                                                            sampled(version == 2 ? "Leaf ✓" : "", 40, false, sample("", 13, 9, 8)),
                                                            sampled("", 10, true, sample("", 4, 6, 16))};
                const abr::File f = abr::parse(abr::write_v12(version, brushes, rle));
                QCOMPARE(f.version, version);
                QVERIFY(f.legacy == brushes);
                QVERIFY2(f.warnings.empty(), f.warnings.empty() ? "" : f.warnings.front().c_str());
            }
        }
    }

    // psd/src/abr.rs v6_round_trip_both_subversions
    void versionSixRoundTrip() {
        const std::vector<abr::Sample> samples{sample("$abc", 20, 11, 8), sample("$def", 5, 5, 16)};
        desc::Writer preset;
        preset.text("Nm  ", u"Grass");
        desc::Writer tip;
        tip.text("sampledData", u"$abc");
        preset.object("Brsh", tip, "sampledBrush");
        for (const int sub : {1, 2}) {
            for (const bool rle : {false, true}) {
                const abr::File f = abr::parse(abr::write_v6(sub, samples, {}, {{"brushPreset", preset}}, rle));
                QCOMPARE(f.version, 6);
                QCOMPARE(f.subversion, sub);
                QVERIFY(f.samples == samples);
                QCOMPARE(f.presets.size(), std::size_t(1));
                QCOMPARE(f.presets[0].class_id, std::string("brushPreset"));
                QVERIFY(f.presets[0].get("Brsh") != nullptr);
                QVERIFY(f.warnings.empty());
            }
        }
        QCOMPARE(samples[1].unit(0), 1.0f);  // (16 bits: 0xffff is full)
    }

    // psd/src/abr.rs hostile_input_errors_without_panicking
    void brokenFilesAreRefusedWithoutReadingPastThem() {
        const auto refused = [](std::string_view data) {
            try {
                (void)abr::parse(data);
                return false;
            } catch (const core::Error&) {
                return true;
            }
        };
        QVERIFY(refused(""));
        QVERIFY(refused(std::string("\x00\x03\x00\x00", 4)));
        QVERIFY(refused(std::string("\x00\x06\x00\x09", 4)));
        abr::Sample huge = sample("", 2, 2, 8);
        huge.width = abr::kMaxEdge + 1;
        try {
            (void)abr::write_v12(1, {sampled("", 1, true, huge)}, false);
            QFAIL("a tip past the largest edge was written");
        } catch (const core::Error&) {
        }
        const std::string good = abr::write_v6(2, {sample("$a", 30, 30, 8)}, {}, {{"brushPreset", desc::Writer()}}, true);
        for (std::size_t cut = 0; cut < good.size(); ++cut) {
            try {
                (void)abr::parse(std::string_view(good).substr(0, cut));
            } catch (const core::Error&) {
            }
        }
        const std::string v2 = abr::write_v12(2, {sampled("x", 1, true, sample("", 9, 9, 16))}, true);
        for (std::size_t cut = 0; cut < v2.size(); ++cut) {
            try {
                (void)abr::parse(std::string_view(v2).substr(0, cut));
            } catch (const core::Error&) {
            }
        }
        for (std::size_t i = 0; i < good.size(); ++i) {  // (each byte flipped once: never past the file)
            std::string b = good;
            b[i] = static_cast<char>(b[i] ^ 0xa5);
            try {
                (void)abr::parse(b);
            } catch (const core::Error&) {
            }
        }
        // a huge count with nothing behind it: an error, not an allocation
        QVERIFY(refused(std::string("\x00\x02\xff\xff", 4)));
    }

    // Beyond upstream's tests: a tip's size in the file is checked before it is unpacked (a tip of 8192 × 8192 × 16 bits
    // from a few bytes is not made), and a file whose tips would unpack past the budget is refused before the one past it.
    void hugeTipsAreRefusedBeforeTheyAreUnpacked() {
        std::string tiny;
        tiny += std::string("\x00\x01\x00\x01\x00\x02", 6);  // (v1, one brush, sampled)
        std::string body;
        body += std::string(4, '\0');                                     // misc
        body += std::string("\x00\x19", 2);                               // spacing
        body += std::string(1, '\x01') + std::string(8, '\0');            // anti-alias, short bounds
        body += std::string("\x00\x00\x00\x00\x00\x00\x00\x00", 8);       // top, left
        body += std::string("\x00\x00\x20\x00\x00\x00\x20\x00", 8);       // bottom 8192, right 8192
        body += std::string("\x00\x10\x01", 3);                           // 16 bits, PackBits
        body += std::string(16, '\0');                                    // (a few row lengths only)
        tiny += std::string(1, '\0') + std::string(1, '\0') + std::string(1, '\0') + std::string(1, static_cast<char>(body.size()));
        tiny += body;
        const auto started = std::chrono::steady_clock::now();
        try {
            (void)abr::parse(tiny);
            QFAIL("a tip of no pixels was read");
        } catch (const core::Error& e) {
            QCOMPARE(QString::fromStdString(e.code()), QStringLiteral("format"));
        }
        QVERIFY(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
        // the budget: three tips of 64 × 64 × 8 bits are 12 KB; with 10 KB the third is refused, unread
        const std::string three = abr::write_v6(1, {sample("$a", 64, 64, 8), sample("$b", 64, 64, 8), sample("$c", 64, 64, 8)}, {}, {}, false);
        QCOMPARE(abr::parse(three, 3 * 64 * 64).samples.size(), std::size_t(3));
        try {
            (void)abr::parse(three, 10000);
            QFAIL("past the budget");
        } catch (const core::Error& e) {
            QCOMPARE(QString::fromStdString(e.code()), QStringLiteral("too_large"));
        }
        // a descriptor of a list longer than its bytes: a warning, read in no time (render/psd_descriptor's budget)
        std::string bomb = abr::write_v6(1, {sample("$a", 4, 4, 8)}, {}, {}, false);
        std::string list;
        list += std::string("\x00\x00\x00\x10", 4);                    // version 16
        list += std::string("\x00\x00\x00\x01\x00\x00", 6);            // name "" (1 char)
        list += std::string("\x00\x00\x00\x00null", 8);                // class id
        list += std::string("\x00\x00\x00\x01", 4);                    // 1 item
        list += std::string("\x00\x00\x00\x00", 4) + "Brsh" + "VlLs";  // key, type
        list += std::string("\x7f\xff\xff\xff", 4);                    // 2^31 items
        bomb += "8BIMdesc";
        bomb += std::string(1, '\0') + std::string(1, '\0') + std::string(1, '\0') + std::string(1, static_cast<char>(list.size()));
        bomb += list;
        const auto again = std::chrono::steady_clock::now();
        const abr::File f = abr::parse(bomb);
        QVERIFY(std::chrono::steady_clock::now() - again < std::chrono::seconds(2));
        QCOMPARE(f.samples.size(), std::size_t(1));
        QVERIFY(f.presets.empty());
        QVERIFY(any_warning(f.warnings, "desc"));
    }

    // io/src/abr_map/tests.rs v6_settings_map_onto_brush_sections
    void photoshopSettingsMapOntoTheDabSettings() {
        for (const int sub : {1, 2}) {
            desc::Writer round;
            round.text("Nm  ", u"Round 19");
            desc::Writer tip;
            tip.unit("Dmtr", "#Pxl", 19.0);
            tip.unit("Hrdn", "#Prc", 0.0);
            tip.unit("Spcn", "#Prc", 10.0);
            round.object("Brsh", tip, "computedBrush");
            const std::string bytes = abr::write_v6(sub, {checkered("$tip-1", 16, 8, 8)}, {grit_pattern()}, {{"brushPreset", full_preset()}, {"brushPreset", round}}, true);
            const render::abr_map::Import imp = render::abr_map::read(bytes);
            QCOMPARE(imp.presets.size(), std::size_t(2));
            QCOMPARE(imp.presets[0].name, std::string("Kitchen Sink"));
            QCOMPARE(imp.presets[0].settings["label"].get<std::string>(), std::string("Kitchen Sink"));
            const core::DabSettings b = dabs_of(imp.presets[0]);
            const dabs::GrayTile t = tile_of(imp, b.tip);
            QCOMPARE(t.width, 16u);
            QCOMPARE(t.height, 8u);
            QCOMPARE(t.data[0], std::uint16_t(65535));
            QCOMPARE(t.data[1], std::uint16_t(0));
            QVERIFY(near(b.size_mm, 64 * kMm));
            QVERIFY(near(b.angle, 30) && near(b.roundness, 0.8) && near(b.spacing, 0.4) && b.flip_x && !b.flip_y);
            QVERIFY(b.shape.enabled && b.shape.flip_x_jitter && !b.shape.flip_y_jitter);
            QVERIFY(b.shape.size.control == core::DabControl::Pressure && near(b.shape.size.jitter, 0.2) && near(b.shape.size.minimum, 0.25));
            QVERIFY(b.shape.angle.control == core::DabControl::Direction && b.shape.roundness.control == core::DabControl::Tilt);
            QVERIFY(near(b.shape.roundness.minimum, 0.1));
            QVERIFY(b.scatter.enabled && b.scatter.both_axes && b.scatter.count == 3);
            QVERIFY(near(b.scatter.scatter.jitter, 2.5) && b.scatter.scatter.control == core::DabControl::Fade && b.scatter.scatter.fade_steps == 40);
            QVERIFY(near(b.scatter.count_jitter.jitter, 0.5));
            QVERIFY(b.texture.enabled && b.texture.each_tip && b.texture.invert);
            QVERIFY(near(b.texture.scale, 1.5) && near(b.texture.brightness, -0.5) && near(b.texture.contrast, 0.5));
            QVERIFY(b.texture.mode == core::DabMaskMode::ColorBurn && near(b.texture.depth, 0.6));
            QVERIFY(near(b.texture.depth_jitter.jitter, 0.3) && near(b.texture.depth_jitter.minimum, 0.2));
            QVERIFY(!b.texture.pattern.asset.empty());  // (the pattern the file holds)
            const dabs::GrayTile pt = tile_of(imp, b.texture.pattern.asset);
            QCOMPARE(pt.width, 4u);
            QCOMPARE(pt.height, 2u);
            QCOMPARE(pt.data[3], std::uint16_t(65535));
            QVERIFY(b.dual.enabled && b.dual.flip && b.dual.both_axes && b.dual.tip.empty());
            QVERIFY(near(b.dual.size_mm, 12 * kMm) && near(b.dual.hardness, 0.5) && b.dual.mode == core::DabMaskMode::HardMix);
            QVERIFY(near(b.dual.spacing, 0.3) && b.dual.count == 2 && near(b.dual.scatter, 1.2));
            QVERIFY(b.color.enabled && !b.color.per_tip);
            QVERIFY(near(b.color.fg_bg.jitter, 0.4) && near(b.color.hue_jitter, 0.1) && near(b.color.saturation_jitter, 0.2));
            QVERIFY(near(b.color.brightness_jitter, 0.3) && near(b.color.purity, -0.5));
            QVERIFY(b.transfer.enabled && b.transfer.opacity.control == core::DabControl::Pressure && near(b.transfer.opacity.minimum, 0.1));
            QVERIFY(b.transfer.flow.control == core::DabControl::Fade && near(b.transfer.flow.jitter, 0.15));
            QVERIFY(b.pose.enabled && b.pose.override_rotation && !b.pose.override_tilt);
            QVERIFY(near(b.pose.tilt_x, 45) && near(b.pose.rotation, 90));
            QVERIFY(b.wet_edges && b.noise && b.build_up);
            QVERIFY(near(b.opacity, 0.8) && near(b.flow, 0.6) && b.pressure_size);
            QVERIFY(any_warning(imp.warnings, "someFutureKey"));
            QVERIFY(any_warning(imp.warnings, "滑らかさ"));  // (Photoshop's smoothing is the window's here: said)
            // the computed one
            const core::DabSettings r = dabs_of(imp.presets[1]);
            QVERIFY(r.tip.empty() && near(r.size_mm, 19 * kMm) && near(r.hardness, 0) && near(r.spacing, 0.1) && !r.pressure_size);
            // its line's width: the size, within the brush panel's 0.05..50 mm
            QVERIFY(near(imp.presets[1].settings["width_mm"].get<double>(), 19 * kMm));
            // every picture the presets name, and only those
            for (const auto& [ref, png] : imp.pictures) QVERIFY(ref == b.tip || ref == b.texture.pattern.asset);
        }
    }

    // io/src/abr_map/tests.rs missing_pattern_and_tip_are_reported
    void whatIsNotInTheFileIsSaid() {
        desc::Writer orphan;
        orphan.text("Nm  ", u"Orphan");
        orphan.object("Brsh", sampled_tip(u"$nowhere"), "sampledBrush");
        const std::string bytes = abr::write_v6(2, {checkered("$tip-1", 4, 4, 16), checkered("$tip-2", 4, 4, 8)}, {}, {{"brushPreset", full_preset()}, {"brushPreset", orphan}}, false);
        const render::abr_map::Import imp = render::abr_map::read(bytes);
        QCOMPARE(imp.presets.size(), std::size_t(1));
        const core::DabSettings b = dabs_of(imp.presets[0]);
        QVERIFY(b.texture.pattern.asset.empty());  // (the program's paper stands in)
        QCOMPARE(b.texture.pattern.procedural, std::string("paper"));
        QVERIFY(any_warning(imp.warnings, "Grit"));
        QVERIFY(any_warning(imp.warnings, "Orphan"));
        // a 16-bit tip's values kept as they are (full paint 65535)
        const dabs::GrayTile t = tile_of(imp, b.tip);
        QCOMPARE(t.data[0], std::uint16_t(65535));
    }

    // io/src/abr_map/tests.rs v1_v2_brushes_become_presets
    void versionsOneAndTwoBecomeBrushes() {
        const std::vector<abr::LegacyBrush> brushes{computed(25, 13, 50, 20, 60), sampled("Dune Grass", 0, false, checkered("", 30, 10, 8))};
        for (const int version : {1, 2}) {
            const render::abr_map::Import imp = render::abr_map::read(abr::write_v12(version, brushes, true));
            QCOMPARE(imp.version, version);
            QCOMPARE(imp.presets.size(), std::size_t(2));
            QCOMPARE(imp.presets[0].name, std::string("円ブラシ 13 px"));
            const core::DabSettings c = dabs_of(imp.presets[0]);
            QVERIFY(near(c.size_mm, 13 * kMm) && near(c.hardness, 0.5) && near(c.angle, 20) && near(c.roundness, 0.6) && near(c.spacing, 0.25));
            QCOMPARE(imp.presets[1].name, version == 2 ? std::string("Dune Grass") : std::string("サンプルブラシ 2"));
            const core::DabSettings s = dabs_of(imp.presets[1]);
            QVERIFY(near(s.size_mm, 30 * kMm) && near(s.spacing, 0.25) && s.aliased);
        }
    }

    // io/src/abr_map/tests.rs v6_without_settings_lists_tips
    void tipsWithoutSettingsAreBrushesEach() {
        const render::abr_map::Import imp = render::abr_map::read(abr::write_v6(1, {checkered("$a", 8, 8, 8), checkered("$b", 3, 9, 16)}, {}, {}, true));
        QCOMPARE(imp.presets.size(), std::size_t(2));
        QVERIFY(near(dabs_of(imp.presets[0]).size_mm, 8 * kMm));
        QVERIFY(near(dabs_of(imp.presets[1]).size_mm, 9 * kMm));
    }

    // io/src/abr_map/tests.rs big_tips_are_downsampled
    void bigTipsAreMadeSmaller() {
        abr::Sample s;
        s.id = "$big";
        s.width = 3000;
        s.height = 1500;
        s.depth = 8;
        s.data.assign(std::size_t(3000) * 1500, static_cast<char>(200));
        const render::abr_map::Import imp = render::abr_map::read(abr::write_v6(2, {s}, {}, {}, true));
        const core::DabSettings b = dabs_of(imp.presets[0]);
        const dabs::GrayTile t = tile_of(imp, b.tip);
        QCOMPARE(t.width, 2500u);
        QCOMPARE(t.height, 1250u);
        QVERIFY(near(b.size_mm, 3000 * kMm));
        QVERIFY(any_warning(imp.warnings, "小さく"));
        // (the line's width within the brush panel's 50 mm; the dab size its own)
        QVERIFY(near(imp.presets[0].settings["width_mm"].get<double>(), 50.0));
    }

    // io/src/abr_map/tests.rs garbage_is_an_error
    void garbageIsAnError() {
        for (const std::string& bad : {std::string(), std::string("8BPS not a brush"), std::string("\x00\x06\x00\x02", 4)}) {
            try {
                (void)render::abr_map::read(bad);
                QFAIL("garbage was read");
            } catch (const core::Error& e) {
                QCOMPARE(QString::fromStdString(e.code()), QStringLiteral("format"));
            }
        }
    }

    // (review P1-1) One budget for the whole file: its tips, patterns ("patt") and descriptors ("desc") together — sections
    // of each, every one within the budget alone, refuse the file before what is past it is unpacked.
    void oneBudgetForTheWholeFile() {
        const auto section = [](std::string_view key, const std::string& data) {
            std::string o = "8BIM";
            o += key;
            const std::size_t pad = (4 - data.size() % 4) % 4;
            const std::uint32_t n = static_cast<std::uint32_t>(data.size() + pad);
            for (const int shift : {24, 16, 8, 0}) o.push_back(static_cast<char>((n >> shift) & 0xff));
            o += data;
            o.append(pad, '\0');
            return o;
        };
        const std::string head("\x00\x06\x00\x02", 4);
        // the smallest budget (doubling from 1 KiB) that reads a file of one such section
        const auto least = [](const std::string& file) {
            std::size_t budget = 1024;
            for (;; budget *= 2) {
                try {
                    (void)abr::parse(file, budget);
                    return budget;
                } catch (const core::Error& e) {
                    if (e.code() != "too_large") throw;
                }
            }
        };
        render::pat::PsdPattern big;
        big.mode = 3;
        big.width = 64;
        big.height = 64;
        big.name = "Big";
        big.id = "big-uuid";
        for (int c = 0; c < 3; ++c) big.channels.push_back(std::string(64 * 64, static_cast<char>(40 * c)));
        desc::Writer preset;
        preset.text("Nm  ", u"Many");
        for (int i = 0; i < 400; ++i) preset.unit(("k" + std::to_string(i)).c_str(), "#Prc", i);
        desc::Writer root;
        root.objects("Brsh", {{"brushPreset", preset}});
        const std::string patt = section("patt", render::pat::write_block({big}));
        const std::string desc_one = section("desc", root.versioned("null"));
        for (const std::string& one : {patt, desc_one}) {
            // (a file of one: its patterns or its brushes; a sampled tip beside the patterns so the file holds a brush)
            const std::string tip = section("samp", [] {
                std::string e;
                e.push_back(static_cast<char>(0));
                e.append(264, '\0');
                for (const std::uint32_t v : {0u, 0u, 2u, 2u})
                    for (const int shift : {24, 16, 8, 0}) e.push_back(static_cast<char>((v >> shift) & 0xff));
                e += std::string("\x00\x08\x00", 3);
                e += std::string(4, '\x80');
                std::string out;
                const std::uint32_t n = static_cast<std::uint32_t>(e.size());
                for (const int shift : {24, 16, 8, 0}) out.push_back(static_cast<char>((n >> shift) & 0xff));
                out += e;
                out.append((4 - e.size() % 4) % 4, '\0');
                return out;
            }());
            const std::size_t budget = least(head + tip + one);
            const std::string three = head + tip + one + one + one;
            try {
                (void)abr::parse(three, budget);
                QFAIL("three sections were read within one section's budget");
            } catch (const core::Error& e) {
                QCOMPARE(QString::fromStdString(e.code()), QStringLiteral("too_large"));
            }
            QVERIFY(abr::parse(three).warnings.empty());  // (the whole file's budget: all three)
        }
    }

    // (review P1-2) A file of countless broken items: the first kMaxWarnings warnings kept and how many more said, the
    // broken items counted with the brushes (the reading stops at kMaxBrushes).
    void warningsHaveAnEnd() {
        std::string samp;
        const auto entry = [&samp](const std::string& e) {
            const std::uint32_t n = static_cast<std::uint32_t>(e.size());
            for (const int shift : {24, 16, 8, 0}) samp.push_back(static_cast<char>((n >> shift) & 0xff));
            samp += e;
            samp.append((4 - e.size() % 4) % 4, '\0');
        };
        {  // (one tip that can be read)
            std::string e;
            e.push_back(static_cast<char>(2));
            e += "$a";
            e.append(264, '\0');
            for (const std::uint32_t v : {0u, 0u, 2u, 2u})
                for (const int shift : {24, 16, 8, 0}) e.push_back(static_cast<char>((v >> shift) & 0xff));
            e += std::string("\x00\x08\x00", 3);
            e += std::string(4, '\xff');
            entry(e);
        }
        for (int i = 0; i < 100000; ++i) entry(std::string(1, '\x05'));  // (each broken: a name longer than the item)
        std::string file("\x00\x06\x00\x02", 4);
        file += "8BIMsamp";
        const std::uint32_t n = static_cast<std::uint32_t>(samp.size());
        for (const int shift : {24, 16, 8, 0}) file.push_back(static_cast<char>((n >> shift) & 0xff));
        file += samp;
        QElapsedTimer clock;
        clock.start();
        const abr::File f = abr::parse(file);
        QCOMPARE(f.samples.size(), std::size_t(1));
        QCOMPARE(f.warnings.size(), abr::kMaxWarnings);
        QCOMPARE(f.warning_count, abr::kMaxBrushes);  // (kMaxBrushes − 1 broken items, then where it stopped)
        QVERIFY(any_warning(f.warnings, "外しました"));
        const render::abr_map::Import imp = render::abr_map::map_file(f);
        QCOMPARE(imp.warnings.size(), abr::kMaxWarnings + 1);
        QVERIFY(imp.warnings.back().starts_with("ほか ") && imp.warnings.back().find("件の知らせがあります") != std::string::npos);
        QVERIFY2(clock.elapsed() < 20000, qPrintable(QString::number(clock.elapsed())));
    }

    // (review P2-1) A pattern many presets name as their texture is made a texture once.
    void aPatternMadeATextureOnce() {
        render::pat::PsdPattern big;
        big.mode = 1;
        big.width = 1024;
        big.height = 1024;
        big.name = "Grain";
        big.id = "grain-uuid";
        std::string plane(1024 * 1024, '\0');
        for (std::size_t i = 0; i < plane.size(); ++i) plane[i] = static_cast<char>((i * 2654435761u) >> 24);
        big.channels = {plane};
        std::vector<std::pair<std::string, desc::Writer>> presets;
        for (int i = 0; i < 1000; ++i) {
            desc::Writer p;
            p.text("Nm  ", u"Textured");
            p.boolean("useTexture", true);
            desc::Writer t;
            t.text("Nm  ", u"Grain");
            t.text("Idnt", u"grain-uuid");
            p.object("Txtr", t, "Ptrn");
            presets.emplace_back("brushPreset", p);
        }
        QElapsedTimer clock;
        clock.start();
        const render::abr_map::Import imp = render::abr_map::read(abr::write_v6(2, {}, {big}, presets, true));
        QCOMPARE(imp.presets.size(), std::size_t(1000));
        QCOMPARE(imp.pictures.size(), std::size_t(1));  // (one texture, named by all)
        const std::string ref = core::dabs_from_json(imp.presets[0].settings["dabs"]).texture.pattern.asset;
        for (const auto& p : imp.presets) QCOMPARE(core::dabs_from_json(p.settings["dabs"]).texture.pattern.asset, ref);
        QVERIFY2(clock.elapsed() < 20000, qPrintable(QString::number(clock.elapsed())));
    }
};

QTEST_GUILESS_MAIN(TestAbrMap)
#include "test_abr_map.moc"
