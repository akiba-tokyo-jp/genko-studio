// PC5 I-h (SPEC.md AUTO-01「HDR Pro に統合」): render/hdr_merge — PhotoCraft v0.2.0's algo/src/hdr.rs tests (MIT) and more:
// exposures of a known radiance (12 stops left to right) shot through a smooth camera response at 8 and 16 bits are merged
// back to it (the log error within the bracket), the exposures estimated from the pictures alone, the recovered response
// monotonic; shots of linear light (a RAW's) merged with no response; ghosts taken from the base exposure; Ward's MTB
// finding a shift, align() setting it; the four tone mappings into the display range; then service/automate_photos: a
// merge made a book of one page (32 bits: linear light in linear-srgb; 16, 8: tone mapped in sRGB), the exposures' EV
// from the cameras' EXIF, the settings it refuses, the work's memory past the budget refused before it is made, a stop.

#include <QtTest>

#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "core/color_model.hpp"
#include "core/error.hpp"
#include "core/exif.hpp"
#include "core/json.hpp"
#include "formats/open_file.hpp"
#include "formats/page_picture.hpp"
#include "render/codecs/codecs.hpp"
#include "render/hdr_merge.hpp"
#include "render/page.hpp"
#include "service/automate_photos.hpp"

using namespace genko;
namespace hdr = render::hdr;
namespace codecs = render::codecs;
namespace automate = service::automate;

namespace {

using Rgb = std::array<float, 3>;

// A radiance map of a wide range: 12 stops left to right, a texture down.
std::vector<Rgb> scene(std::size_t w, std::size_t h) {
    std::vector<Rgb> out(w * h);
    for (std::size_t i = 0; i < w * h; ++i) {
        const float x = static_cast<float>(i % w) / static_cast<float>(w), y = static_cast<float>(i / w) / static_cast<float>(h);
        const float base = 0.002f * std::pow(2.0f, x * 12.0f);
        const float tex = 1.0f + 0.3f * std::sin(y * 40.0f);
        out[i] = {base * tex, base * tex * 0.8f, base * tex * 0.6f};
    }
    return out;
}

// A smooth response that is not sRGB's.
float camera(float e) { return std::min(std::pow(std::max(e, 0.0f), 0.45f), 1.0f); }

// The scene shot at `dt` (8 or 16 bits).
hdr::Shot shoot(const std::vector<Rgb>& sc, std::size_t w, std::size_t h, float dt, codecs::Sample sample = codecs::Sample::U8,
                std::optional<double> exposure = std::nullopt) {
    std::vector<float> v;
    v.reserve(sc.size() * 3);
    for (const Rgb& r : sc)
        for (const float c : r) v.push_back(camera(c * dt));
    return hdr::Shot("shot", codecs::CodecImage::from_normalized(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), codecs::Layout::Rgb, sample, v),
                     exposure);
}

// The scene as linear light a sensor clips at 1 (a RAW's development).
hdr::Shot shoot_linear(const std::vector<Rgb>& sc, std::size_t w, std::size_t h, float dt) {
    std::string bytes(sc.size() * 3 * sizeof(float), '\0');
    auto* f = reinterpret_cast<float*>(bytes.data());
    for (std::size_t i = 0; i < sc.size(); ++i)
        for (std::size_t c = 0; c < 3; ++c) f[i * 3 + c] = std::min(sc[i][c] * dt, 1.0f);
    return hdr::Shot("raw", codecs::CodecImage::from_bytes(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), codecs::Layout::Rgb, codecs::Sample::F32,
                                                           std::move(bytes)));
}

std::vector<double> exposures_of(const std::vector<float>& dts) { return {dts.begin(), dts.end()}; }

}  // namespace

class TestHdrMerge : public QObject {
    Q_OBJECT

private slots:
    void exposureEstimationAndMergeRecoverRadiance() {
        const std::size_t w = 192, h = 64;
        const auto sc = scene(w, h);
        const std::vector<float> dts{0.25f, 1.0f, 4.0f, 16.0f};
        for (const codecs::Sample sample : {codecs::Sample::U8, codecs::Sample::U16}) {
            std::vector<hdr::Shot> shots;
            for (const float dt : dts) shots.push_back(shoot(sc, w, h, dt, sample));
            std::vector<const hdr::Shot*> refs;
            for (const auto& s : shots) refs.push_back(&s);
            const auto est = hdr::estimate_exposures(refs);
            // (the ratios between consecutive exposures are 4×, estimated through an sRGB guess: loose)
            for (std::size_t k = 1; k < 4; ++k) {
                const double r = est[k] / est[k - 1];
                QVERIFY2(r > 2.0 && r < 8.0, qPrintable(QString::number(r)));
            }
            hdr::MergeOptions options;
            options.exposures = exposures_of(dts);
            const hdr::Merged m = hdr::merge(shots, options);
            QCOMPARE(m.width, std::uint32_t(w));
            QVERIFY(m.response.has_value());
            // (the radiance proportional to the scene's within the bracket: its log ratio across the 12-stop ramp)
            const auto probe = [&](std::size_t x) { return static_cast<double>(m.rgb[((h / 2) * w + x) * 3 + 1]) / sc[(h / 2) * w + x][1]; };
            const double k0 = probe(w / 2);
            for (const std::size_t x : {10, 50, 100, 150, 170}) {
                const double r = probe(x) / k0;
                QVERIFY2(std::abs(std::log(r)) < 0.25, qPrintable(QStringLiteral("x %1: %2").arg(x).arg(r)));
            }
            QVERIFY2(m.stops > 9.0, qPrintable(QString::number(m.stops)));
            // (the recovered response is monotonic, each channel)
            for (const auto& g : *m.response)
                for (std::size_t k = 1; k < g.size(); ++k) QVERIFY(g[k] > g[k - 1]);
        }
    }

    // A RAW's shots are linear light: no response is estimated, the radiance is theirs (the base's values kept).
    void linearShotsMergeWithoutResponse() {
        const std::size_t w = 160, h = 48;
        const auto sc = scene(w, h);
        const std::vector<float> dts{1.0f, 8.0f, 64.0f};
        std::vector<hdr::Shot> shots;
        for (const float dt : dts) shots.push_back(shoot_linear(sc, w, h, dt));
        QVERIFY(shots[0].linear());
        hdr::MergeOptions options;
        options.exposures = exposures_of(dts);
        const hdr::Merged m = hdr::merge(shots, options);
        QVERIFY(!m.response.has_value());
        const auto probe = [&](std::size_t x) { return static_cast<double>(m.rgb[((h / 2) * w + x) * 3]) / sc[(h / 2) * w + x][0]; };
        const double k0 = probe(w / 2);
        for (std::size_t x = 4; x < 110; x += 13) {  // (within the bracket: past x ≈ 115 the darkest shot clips too)
            const double r = probe(x) / k0;
            QVERIFY2(std::abs(std::log(r)) < 0.02, qPrintable(QStringLiteral("x %1: %2").arg(x).arg(r)));
        }
        // (shots of display values and of linear light are not merged together)
        std::vector<hdr::Shot> mixed{shoot(sc, w, h, 1.0f), shoot_linear(sc, w, h, 8.0f)};
        options.exposures = {1.0, 8.0};
        QVERIFY_THROWS_EXCEPTION(core::Error, hdr::merge(mixed, options));
    }

    void ghostsComeFromTheBaseExposure() {
        const std::size_t w = 64, h = 64;
        const std::vector<Rgb> sc(w * h, Rgb{0.2f, 0.2f, 0.2f});
        // (a "person" only in the third exposure)
        std::vector<hdr::Shot> shots{shoot(sc, w, h, 0.5f), shoot(sc, w, h, 1.0f)};
        {
            std::vector<float> v;
            for (std::size_t i = 0; i < w * h; ++i) {
                const std::size_t x = i % w, y = i / w;
                const bool in = x >= 20 && x < 40 && y >= 20 && y < 40;
                for (int c = 0; c < 3; ++c) v.push_back(in ? 0.2f : camera(0.2f * 2.0f));
            }
            shots.emplace_back("third", codecs::CodecImage::from_normalized(w, h, codecs::Layout::Rgb, codecs::Sample::U8, v));
        }
        hdr::MergeOptions options;
        options.exposures = {0.5, 1.0, 2.0};
        options.ghost_base = 1;
        const hdr::Merged plain = hdr::merge(shots, options);
        options.remove_ghosts = true;
        const hdr::Merged clean = hdr::merge(shots, options);
        const auto at = [&](const hdr::Merged& m, std::size_t x, std::size_t y) { return m.rgb[(y * w + x) * 3 + 1]; };
        const float bg = at(clean, 5, 5);
        QVERIFY2(std::abs(at(clean, 30, 30) / bg - 1.0f) < 0.15f, qPrintable(QStringLiteral("%1 vs %2").arg(at(clean, 30, 30)).arg(bg)));
        QVERIFY(std::abs(at(plain, 30, 30) / at(plain, 5, 5) - 1.0f) > 0.2f);
        QVERIFY(clean.ghost_fraction > 0.05);
        QCOMPARE(clean.ghost_base, std::size_t{1});
        QCOMPARE(plain.ghost_fraction, 0.0);
    }

    void mtbFindsShifts() {
        const std::size_t w = 128, h = 96;
        std::vector<float> img(w * h), shifted(w * h);
        for (std::size_t i = 0; i < w * h; ++i) {
            const float x = static_cast<float>(i % w), y = static_cast<float>(i / w);
            img[i] = 0.5f + 0.4f * std::sin(x * 0.21f) * std::cos(y * 0.17f);
        }
        const int dx = 5, dy = -3;
        for (std::size_t i = 0; i < w * h; ++i) {
            const int x = std::clamp(static_cast<int>(i % w) - dx, 0, static_cast<int>(w) - 1);
            const int y = std::clamp(static_cast<int>(i / w) - dy, 0, static_cast<int>(h) - 1);
            shifted[i] = img[std::size_t(y) * w + std::size_t(x)] * 0.6f;  // (a darker exposure of the same scene, shifted)
        }
        QCOMPARE(hdr::mtb_offset(w, h, shifted, img, 4), std::make_pair(dx, dy));
        // (align: each shot read from where it lies on the middle exposure)
        std::vector<float> a, b, c;
        for (std::size_t i = 0; i < w * h; ++i) {
            for (int k = 0; k < 3; ++k) {
                a.push_back(img[i] * 0.6f);
                b.push_back(shifted[i] / 0.6f * 0.9f > 1.0f ? 1.0f : shifted[i] / 0.6f * 0.9f);
                c.push_back(std::min(img[i] * 1.2f, 1.0f));
            }
        }
        std::vector<hdr::Shot> shots;
        for (auto* v : {&a, &b, &c}) shots.emplace_back("s", codecs::CodecImage::from_normalized(w, h, codecs::Layout::Rgb, codecs::Sample::U8, *v));
        hdr::align(shots, {0.6, 0.9, 1.2});
        QCOMPARE(std::make_pair(shots[1].dx, shots[1].dy), std::make_pair(0, 0));  // (the middle one)
        QCOMPARE(std::make_pair(shots[0].dx, shots[0].dy), std::make_pair(dx, dy));
        QCOMPARE(std::make_pair(shots[2].dx, shots[2].dy), std::make_pair(dx, dy));
        // (read where aligned: the middle one's pixel)
        QCOMPARE(shots[0].value(60, 40, 0), static_cast<float>(std::lround(img[std::size_t(40 - dy) * w + std::size_t(60 - dx)] * 0.6f * 255.0f)) / 255.0f);
    }

    void toneMappingMethodsProduceDisplayRange() {
        const std::size_t w = 96, h = 32;
        const auto sc = scene(w, h);
        hdr::Tone local;
        local.local.radius = 7;
        local.local.strength = 0.52f;
        hdr::Tone gamma;
        gamma.method = hdr::Tone::Method::ExposureGamma;
        hdr::Tone highlight;
        highlight.method = hdr::Tone::Method::HighlightCompression;
        hdr::Tone equalize;
        equalize.method = hdr::Tone::Method::EqualizeHistogram;
        for (const hdr::Tone& tone : {local, gamma, highlight, equalize}) {
            std::vector<hdr::Rgba> px;
            for (const Rgb& r : sc) px.push_back({r[0], r[1], r[2], 1.0f});
            hdr::tone_map(px, w, h, tone);
            for (const auto& q : px)
                for (int c = 0; c < 3; ++c) QVERIFY(q[std::size_t(c)] >= 0.0f && q[std::size_t(c)] <= 1.0f);
            const auto row = [&](std::size_t x) { return px[(h / 2) * w + x][1]; };
            QVERIFY2(row(w - 2) > row(2), hdr::method_name(tone.method));
            if (tone.method != hdr::Tone::Method::ExposureGamma) {
                // (compression keeps both ends of a 12-stop ramp visible)
                QVERIFY2(row(4) > 0.01f && row(w - 4) < 0.999f, hdr::method_name(tone.method));
            }
        }
        QCOMPARE(hdr::method_from("equalize_histogram"), std::optional(hdr::Tone::Method::EqualizeHistogram));
        QVERIFY(!hdr::method_from("reinhard"));
    }

    // EXIF's exposure: time × ISO/100 / N² (PhotoCraft's exposure_factor).
    void exposureFactorOfTheCamera() {
        QCOMPARE(*hdr::exposure_factor(0.004, 8.0, 200.0), 0.004 * 2.0 / 64.0);
        QCOMPARE(*hdr::exposure_factor(0.5, std::nullopt, std::nullopt), 0.5);
        QVERIFY(!hdr::exposure_factor(std::nullopt, 8.0, 100.0));
        QVERIFY(!hdr::exposure_factor(0.0, 8.0, 100.0));
    }

    // The service: a book of one page — 32 bits of linear light (linear-srgb), or 16 and 8 bits tone mapped.
    void mergeToHdrMakesABook() {
        const std::size_t w = 96, h = 40;
        const auto sc = scene(w, h);
        const std::vector<float> dts{0.25f, 2.0f, 16.0f};
        const auto shots = [&] {
            std::vector<hdr::Shot> out;
            for (const float dt : dts) out.push_back(shoot(sc, w, h, dt, codecs::Sample::U8, dt * 0.01));  // (each its camera's exposure)
            return out;
        };
        automate::HdrSettings settings;
        settings.title = "合成";
        automate::HdrMade made = automate::merge_to_hdr(shots(), settings);
        QCOMPARE(made.book.doc.title, std::string("合成"));
        const core::Page& page = made.book.doc.page(0);
        QVERIFY(page.rgb.has_value());
        QCOMPARE(page.rgb->depth, 32);
        QVERIFY(page.rgb->icc != nullptr);
        QCOMPARE(*page.rgb->icc, *core::builtin_profile("linear-srgb")->bytes);
        QCOMPARE(made.report["mode"].get<int>(), 32);
        QCOMPARE(made.report["exposure_source"].get<std::string>(), std::string("exif"));
        const core::Json ev = made.report["ev"];
        QCOMPARE(ev[0].get<double>(), 0.0);
        QVERIFY(std::abs(ev[1].get<double>() - 3.0) < 1e-6 && std::abs(ev[2].get<double>() - 6.0) < 1e-6);
        // (its values: linear light past 1 kept, rising across the ramp)
        const formats::PagePicture pic = formats::page_picture(page, made.book.doc, static_cast<int>(std::lround(page.spec.dpi.value())));
        QCOMPARE(pic.image.sample(), codecs::Sample::F32);
        QCOMPARE(pic.image.width(), std::uint32_t(w));
        QVERIFY(pic.image.at(std::uint32_t(w - 3), std::uint32_t(h / 2), 1) > pic.image.at(3, std::uint32_t(h / 2), 1) * 100.0f);
        for (const int depth : {16, 8}) {
            settings.depth = depth;
            settings.tone = automate::hdr_tone(depth == 16 ? "local_adaptation" : "highlight_compression");
            made = automate::merge_to_hdr(shots(), settings);
            const core::Page& toned = made.book.doc.page(0);
            QCOMPARE(toned.rgb ? toned.rgb->depth : 8, depth);
            QVERIFY(!toned.rgb || toned.rgb->icc == nullptr);  // (sRGB)
            QCOMPARE(made.report["method"].get<std::string>(), std::string(depth == 16 ? "local_adaptation" : "highlight_compression"));
        }
        // (EV given: as given; estimated without the cameras')
        settings.depth = 32;
        settings.ev = std::vector<double>{0, 3, 6};
        QCOMPARE(automate::merge_to_hdr(shots(), settings).report["exposure_source"].get<std::string>(), std::string("given"));
        settings.ev.reset();
        std::vector<hdr::Shot> unknown;
        for (const float dt : dts) unknown.push_back(shoot(sc, w, h, dt));
        QCOMPARE(automate::merge_to_hdr(std::move(unknown), settings).report["exposure_source"].get<std::string>(), std::string("estimated"));
    }

    void whatTheMergeRefuses() {
        const std::size_t w = 40, h = 20;
        const auto sc = scene(w, h);
        automate::HdrSettings settings;
        const auto expect = [](auto&& work, const char* code) {
            try {
                work();
                QFAIL("not refused");
            } catch (const core::Error& error) {
                QCOMPARE(error.code(), std::string(code));
            }
        };
        expect([&] { automate::merge_to_hdr({shoot(sc, w, h, 1.0f)}, settings); }, "value");  // (one picture)
        expect([&] { automate::merge_to_hdr({shoot(sc, w, h, 1.0f), shoot(scene(w + 2, h), w + 2, h, 4.0f)}, settings); }, "value");  // (sizes)
        settings.ev = std::vector<double>{0};
        expect([&] { automate::merge_to_hdr({shoot(sc, w, h, 1.0f), shoot(sc, w, h, 4.0f)}, settings); }, "value");  // (an EV a picture)
        settings.ev.reset();
        settings.ghost_base = 2;
        expect([&] { automate::merge_to_hdr({shoot(sc, w, h, 1.0f), shoot(sc, w, h, 4.0f)}, settings); }, "value");
        settings.ghost_base.reset();
        settings.depth = 24;
        expect([&] { automate::merge_to_hdr({shoot(sc, w, h, 1.0f), shoot(sc, w, h, 4.0f)}, settings); }, "value");
        expect([] { automate::hdr_tone("reinhard"); }, "value");
        expect([] { automate::hdr_tone("local_adaptation", core::Json{{"radius", "big"}}); }, "value");
        expect([] { automate::hdr_tone("exposure_gamma", core::Json{{"radius", 3}}); }, "value");
        // (clamped to Photoshop's ranges)
        QCOMPARE(automate::hdr_tone("local_adaptation", core::Json{{"radius", 9000}}).local.radius, 500.0f);
        // (CMYK is no light to merge)
        QVERIFY_THROWS_EXCEPTION(core::Error, hdr::rgb_of(codecs::CodecImage::blank(4, 4, codecs::Layout::Cmyk, codecs::Sample::U8)));
        // (the work's memory past the budget: refused before it is made — 16 exposures of 6000 × 4000 at 16 bits)
        expect([] { automate::check_work(6000, 4000, std::uint64_t(16) * 6000 * 4000 * 6, automate::HdrSettings{}); }, "image_too_large");
        automate::check_work(1000, 800, std::uint64_t(3) * 1000 * 800 * 3, automate::HdrSettings{});
    }

    // 中止: the merge stops between rows (render::Cancelled), nothing made.
    void aStopStopsIt() {
        const std::size_t w = 64, h = 64;
        const auto sc = scene(w, h);
        int asked = 0;
        QVERIFY_THROWS_EXCEPTION(render::Cancelled, automate::merge_to_hdr({shoot(sc, w, h, 1.0f), shoot(sc, w, h, 4.0f)}, automate::HdrSettings{}, [&asked] {
            return ++asked > 2;
        }));
    }

    // The preview's shots: made small, their kind kept; the display of a merge 8-bit RGB.
    void thePreview() {
        const std::size_t w = 300, h = 100;
        const auto sc = scene(w, h);
        const hdr::Shot small = hdr::reduced(shoot(sc, w, h, 1.0f, codecs::Sample::U16, 0.5), 60);
        QCOMPARE(small.width(), 60u);
        QCOMPARE(small.height(), 20u);
        QCOMPARE(small.picture().sample(), codecs::Sample::U16);
        QCOMPARE(*small.exposure, 0.5);
        std::vector<hdr::Shot> shots{hdr::reduced(shoot(sc, w, h, 0.5f), 80), hdr::reduced(shoot(sc, w, h, 8.0f), 80)};
        automate::HdrSettings settings;
        const automate::HdrRadiance radiance = automate::hdr_radiance(shots, settings);
        const codecs::CodecImage shown = automate::hdr_display(radiance, settings, -1.0);
        QCOMPARE(shown.layout(), codecs::Layout::Rgb);
        QCOMPARE(shown.sample(), codecs::Sample::U8);
        QCOMPARE(shown.width(), 75u);  // (made smaller by a whole factor: 300 / 4)
        settings.depth = 8;
        QVERIFY(automate::hdr_display(radiance, settings) != shown);
    }
};

QTEST_GUILESS_MAIN(TestHdrMerge)
#include "test_hdr_merge.moc"
