// 色の校正・校正の設定・色域外警告 and カラー設定's book part (SPEC COLOR-03, PC1b): render/proof against PhotoCraft
// v0.2.0's own tests (engine/src/proof_sim.rs:341-418, cms/src/gamut.rs; compose/src/psblend.rs for the text gamma) —
// the colour vision simulations keep the greys, the plates are grey and follow their ink, Legacy Macintosh RGB shows the
// middle greys lighter, the gamut check finds sRGB's greys in and a saturated green out of coated-cmyk; 「CMYK で見る」
// (書き出しの CMYK) exactly as render/colour::proof (Python's); a picture's own profile read from its file and taken to
// sRGB; and set_text_gamma (color-model@1's book text_gamma): the op, the book on disk, the letters drawn — none is
// Python's drawing, byte for byte.

#include <QtTest>

#include <QTemporaryDir>

#include <zlib.h>

#include <array>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "core/color_model.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ink.hpp"
#include "core/paths.hpp"
#include "render/colour.hpp"
#include "render/image.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/proof.hpp"
#include "render/text/lettering.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/transaction.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
namespace fs = std::filesystem;
namespace proof = render::proof;

namespace {

const fs::path kIcc = fs::path(GENKO_TEST_DATA) / "colour" / "photocraft-coated-cmyk.icc";

core::Bytes coated() { return core::builtin_profile("coated-cmyk")->bytes; }

render::Image pixels(const std::vector<std::array<int, 3>>& colours) {
    std::string bytes;
    for (const auto& c : colours)
        for (const int v : c) bytes.push_back(static_cast<char>(v));
    return render::Image::frombytes("RGB", render::Size{static_cast<int>(colours.size()), 1}, bytes);
}

std::array<int, 3> at(const render::Image& image, int x) {
    const std::vector<double> p = image.getpixel(x, 0);
    return {static_cast<int>(p[0]), static_cast<int>(p[1]), static_cast<int>(p[2])};
}

bool grey(const std::array<int, 3>& c, int within) { return std::abs(c[0] - c[1]) <= within && std::abs(c[1] - c[2]) <= within; }

// A picture of colours from all over sRGB (a 6³ grid), one row.
render::Image spread() {
    std::vector<std::array<int, 3>> colours;
    for (int b = 0; b < 6; ++b)
        for (int g = 0; g < 6; ++g)
            for (int r = 0; r < 6; ++r) colours.push_back({r * 51, g * 51, b * 51});
    return pixels(colours);
}

core::ApplyResult run_ops(const core::Document& doc, const Json& ops) {
    return core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor("human:test"));
}

std::string refusal(const core::Document& doc, const Json& ops) {
    try {
        run_ops(doc, ops);
    } catch (const core::ApplyError& error) {
        return error.code() + ": " + error.what();
    }
    return {};
}

core::Document lettered_book() {
    core::PageSpec spec;
    spec.width_mm = core::Num(60);
    spec.height_mm = core::Num(60);
    spec.dpi = core::Num(150);
    spec.bleed_mm = core::Num(3);
    spec.inner_margin_mm = core::Num(5);
    core::Document doc = core::new_episode("letters", core::Num(1), 1, spec, core::Binding::Right);
    return run_ops(doc, Json::array({Json{{"op", "add_line"}, {"page", 1}, {"text", "ガンマの字"}, {"x_mm", 10}, {"y_mm", 10}, {"w_mm", 40},
                                          {"h_mm", 30}, {"balloon", "speech"}}}))
        .doc;
}

void write_book(const core::Document& doc, const fs::path& dir) {
    storage::ProjectLock lock(dir, "human:test");
    lock.try_acquire();
    storage::SaveRequest request;
    request.actor = "human:test";
    request.ops = Json::array();
    storage::Saver(lock).save(doc, request);
}

std::string png_chunk(const std::string& type, const std::string& data) {
    std::string out;
    for (int i = 3; i >= 0; --i) out.push_back(static_cast<char>((data.size() >> (8 * i)) & 0xff));
    out += type + data;
    const uLong crc = crc32(crc32(0, reinterpret_cast<const Bytef*>(type.data()), 4), reinterpret_cast<const Bytef*>(data.data()), static_cast<uInt>(data.size()));
    for (int i = 3; i >= 0; --i) out.push_back(static_cast<char>((crc >> (8 * i)) & 0xff));
    return out;
}

}  // namespace

class TestProof : public QObject {
    Q_OBJECT

private slots:
    // upstream's dichromat_keeps_neutrals_and_merges_red_green
    void colourVisionKeepsTheGreys() {
        for (const bool protan : {true, false}) {
            const auto white = proof::dichromat({1, 1, 1}, protan);
            for (const float c : white) QVERIFY2(std::abs(c - 1.0f) < 0.02f, protan ? "protan" : "deutan");
            const auto dark = proof::dichromat({0.2f, 0.2f, 0.2f}, protan);
            for (const float c : dark) QVERIFY(std::abs(c - 0.2f) < 0.02f);
            const auto red = proof::dichromat({1, 0, 0}, protan);
            const auto green = proof::dichromat({0, 1, 0}, protan);
            QVERIFY(std::abs(red[0] - red[1]) < 0.15f && std::abs(green[0] - green[1]) < 0.15f);  // (both yellowish)
            QVERIFY(proof::dichromat({0, 0, 1}, protan)[2] > 0.8f);  // (blue stays blue)
        }
        QVERIFY(proof::dichromat({1, 0, 0}, true)[0] < proof::dichromat({1, 0, 0}, false)[0]);  // (protanopes see red darker)
        // on a picture: its greys as they were, within a level or so
        for (const proof::Kind kind : {proof::Kind::Protan, proof::Kind::Deutan}) {
            proof::Setup setup;
            setup.kind = kind;
            const render::Image shown = proof::apply(pixels({{255, 255, 255}, {128, 128, 128}, {40, 40, 40}, {255, 0, 0}}), setup);
            for (int x = 0; x < 3; ++x) {
                const auto c = at(shown, x);
                const int was = x == 0 ? 255 : x == 1 ? 128 : 40;
                QVERIFY2(grey(c, 3) && std::abs(c[0] - was) <= 4, qPrintable(QStringLiteral("%1 %2 %3").arg(c[0]).arg(c[1]).arg(c[2])));
            }
            const auto red = at(shown, 3);
            QVERIFY(std::abs(red[0] - red[1]) < 60);
        }
    }

    // upstream's plate_previews_are_gray_and_track_ink: pure cyan heavy on the cyan plate (dark), white none, red little
    void platesAreGreyAndFollowTheirInk() {
        proof::Setup setup;
        setup.kind = proof::Kind::Plate;
        setup.plate = 0;
        setup.icc = coated();
        const render::Image shown = proof::apply(pixels({{0, 255, 255}, {255, 255, 255}, {255, 0, 0}}), setup);
        const auto cyan = at(shown, 0), white = at(shown, 1), red = at(shown, 2);
        for (const auto& c : {cyan, white, red}) QVERIFY(grey(c, 8));
        QVERIFY(white[0] > 0.9 * 255);
        QVERIFY(cyan[0] < red[0] && cyan[0] < 0.6 * 255);
        // the black plate of a grey: grey; the CMY plates of black: some ink, but not as black as it
        setup.plate = 3;
        QVERIFY(grey(at(proof::apply(pixels({{90, 90, 90}}), setup), 0), 8));
        setup.kind = proof::Kind::CmyPlate;
        const auto cmy = at(proof::apply(pixels({{0, 0, 0}}), setup), 0);
        QVERIFY(cmy[0] > 20);
        // a profile that is not CMYK cannot show plates
        setup.icc = core::builtin_profile("display-p3")->bytes;
        QVERIFY_THROWS_EXCEPTION(core::Error, proof::apply(pixels({{0, 0, 0}}), setup));
        // Genko's own conversion (no profile): the plain conversion's black plate, printed back by its own way
        proof::Setup plain;
        plain.kind = proof::Kind::Plate;
        plain.plate = 3;
        const render::Image picture = pixels({{90, 90, 90}, {200, 30, 30}});
        std::string cmyk = render::colour::to_cmyk(picture).tobytes();
        for (std::size_t i = 0; i < cmyk.size(); i += 4) cmyk[i] = cmyk[i + 1] = cmyk[i + 2] = 0;
        QCOMPARE(proof::apply(picture, plain).tobytes(), render::colour::from_cmyk(render::Image::frombytes("CMYK", picture.size(), cmyk)).tobytes());
    }

    // upstream's legacy_mac_is_darker_in_midtones: the numbers on a display of gamma 1.8 (0.5 → 0.287 linear, about 0.57)
    void legacyMacintoshShowsTheMiddleLighter() {
        proof::Setup setup;
        setup.kind = proof::Kind::LegacyMac;
        const render::Image shown = proof::apply(pixels({{128, 128, 128}, {255, 255, 255}, {0, 0, 0}}), setup);
        const auto mid = at(shown, 0);
        QVERIFY2(mid[0] > 0.52 * 255 && mid[0] < 0.62 * 255, qPrintable(QString::number(mid[0])));
        QCOMPARE(at(shown, 1), (std::array<int, 3>{255, 255, 255}));
        QCOMPARE(at(shown, 2), (std::array<int, 3>{0, 0, 0}));
    }

    // 書き出しの CMYK: render/colour::proof itself (Python's 「CMYK で見る」), byte for byte, with and without the profile
    // chosen for export; through a CMYK profile's bytes (作業用 CMYK) Little CMS's soft proof, near the same round trip.
    void theExportProofIsPythons() {
        const render::Image picture = spread();
        proof::Setup setup;
        QCOMPARE(proof::apply(picture, setup).tobytes(), render::colour::proof(picture).tobytes());
        setup.export_icc = kIcc;
        QCOMPARE(proof::apply(picture, setup).tobytes(), render::colour::proof(picture, kIcc).tobytes());
        proof::Setup working;
        working.icc = coated();
        const std::string soft = proof::apply(picture, working).tobytes();
        const std::string trip = render::colour::proof(picture, kIcc).tobytes();
        QCOMPARE(soft.size(), trip.size());
        // (the round trip keeps the inks in 8 bits and goes through two optimized transforms; the soft proof is one
        // transform in floats: dark and saturated colours out of the gamut differ by a few levels — 8 here)
        int worst = 0;
        for (std::size_t i = 0; i < soft.size(); ++i)
            worst = std::max(worst, std::abs(int(static_cast<unsigned char>(soft[i])) - int(static_cast<unsigned char>(trip[i]))));
        QVERIFY2(worst <= 10, qPrintable(QString::number(worst)));
        // a saturated green comes back duller; the paper white stays white (relative)
        const render::Image shown = proof::apply(pixels({{0, 255, 0}, {255, 255, 255}}), working);
        QVERIFY(at(shown, 0)[1] < 250 || at(shown, 0)[0] > 5 || at(shown, 0)[2] > 5);
        QVERIFY(at(shown, 1)[0] >= 250);
        // simulating the paper: its white is not the screen's
        working.simulate_paper = true;
        QVERIFY(at(proof::apply(pixels({{255, 255, 255}}), working), 0) != (std::array<int, 3>{255, 255, 255}));
        // RGBA keeps its alpha
        render::Image rgba = picture.convert("RGBA");
        rgba.putalpha(77);
        const render::Image kept = proof::apply(rgba, setup);
        QCOMPARE(kept.mode(), std::string_view("RGBA"));
        QCOMPARE(kept.getchannel(3).tobytes(), rgba.getchannel(3).tobytes());
    }

    // インターネット標準の RGB, モニターの RGB: the page's numbers shown as sRGB — the screen's picture itself for a page
    // without a profile; through a Display P3 page's profile, its saturated colours become duller numbers.
    void theNumbersAsSrgb() {
        const render::Image picture = spread();
        for (const proof::Kind kind : {proof::Kind::Srgb, proof::Kind::Monitor}) {
            proof::Setup setup;
            setup.kind = kind;
            QCOMPARE(proof::apply(picture, setup).tobytes(), picture.tobytes());
            const proof::Source p3{core::builtin_profile("display-p3")->bytes, false};
            const render::Image shown = proof::apply(pixels({{255, 0, 0}, {128, 128, 128}}), setup, p3);
            QVERIFY(at(shown, 0)[0] < 250 || at(shown, 0)[1] > 10);  // (sRGB's red is inside P3: smaller numbers)
            QVERIFY(grey(at(shown, 1), 2));
            const proof::Source sgray{core::builtin_profile("sgray")->bytes, true};
            const auto g = at(proof::apply(pixels({{128, 128, 128}}), setup, sgray), 0);
            QVERIFY(grey(g, 0) && std::abs(g[0] - 128) <= 1);
        }
        for (const char* id : {"profile", "workingCmyk", "workingCyanPlate", "workingMagentaPlate", "workingYellowPlate", "workingBlackPlate", "workingCmyPlate",
                               "internetStandardRgb", "monitorRgb", "legacyMacintoshRgb", "colorBlindnessProtanopia", "colorBlindnessDeuteranopia"}) {
            const auto kind = proof::kind_from_id(id);
            QVERIFY2(kind, id);
            if (std::string_view(id) != "workingCmyk") QCOMPARE(proof::kind_id(kind->first, kind->second), std::string(id));
        }
        QVERIFY(!proof::kind_from_id("nope"));
    }

    // A CMYK page's plates: its own inks, printed through its profile.
    void aCmykPagesOwnPlates() {
        core::InkPixels inks = core::blank_ink_pixels(core::InkSpace::Cmyk, 8, 2, 1);
        for (unsigned c = 0; c < 4; ++c) inks.set(0, c, c == 0 ? 1.0 : 0.0);  // (cyan alone)
        for (unsigned c = 0; c < 4; ++c) inks.set(1, c, c == 3 ? 0.5 : 0.0);  // (half black)
        inks.set(0, 4, 1.0);
        inks.set(1, 4, 1.0);
        proof::Setup setup;
        setup.kind = proof::Kind::Plate;
        setup.plate = 0;
        const render::Image cyan = proof::page_plates(inks, coated(), setup);
        QVERIFY(grey(at(cyan, 0), 8) && at(cyan, 0)[0] < 100);
        QVERIFY(at(cyan, 1)[0] > 245);  // (no cyan in the black)
        setup.plate = 3;
        const render::Image black = proof::page_plates(inks, coated(), setup);
        QVERIFY(at(black, 0)[0] > 245 && at(black, 1)[0] < 200);
    }

    // 色域外警告 (upstream's GamutCheck): sRGB's greys are in coated-cmyk's gamut, a saturated green is not; an RGB profile
    // that holds sRGB has nothing out; the plain conversion keeps its greys; the mask and its count.
    void theGamut() {
        proof::Setup setup;
        setup.icc = coated();
        const auto check = proof::Gamut::of(setup);
        // (upstream's proofing_state_and_gamut_warning: mid grey and white in, saturated blue out — and Genko's ink (20) in;
        // pure black is out without black point compensation, as upstream checks it)
        for (const int v : {20, 40, 60, 90, 128, 200, 255}) QVERIFY2(check->delta_e(v, v, v) < proof::kDefaultThreshold, qPrintable(QString::number(v)));
        QVERIFY(check->delta_e(0, 0, 0) > proof::kDefaultThreshold);
        QVERIFY(check->delta_e(0, 255, 0) > proof::kDefaultThreshold);
        QVERIFY(check->delta_e(0, 0, 255) > proof::kDefaultThreshold);
        QCOMPARE(proof::Gamut::of(setup).get(), check.get());  // (kept)
        std::size_t out = 0;
        const auto mask = check->mask(pixels({{128, 128, 128}, {0, 255, 0}, {255, 255, 255}}), proof::kDefaultThreshold, &out);
        QCOMPARE(mask, (std::vector<unsigned char>{0, 255, 0}));
        QCOMPARE(out, std::size_t(1));
        render::Image transparent = pixels({{0, 255, 0}}).convert("RGBA");
        transparent.putalpha(0);
        QCOMPARE(check->mask(transparent, proof::kDefaultThreshold, &out), (std::vector<unsigned char>{0}));
        QCOMPARE(out, std::size_t(0));
        proof::Setup wide;
        wide.icc = core::builtin_profile("rec2020")->bytes;
        const auto rec2020 = proof::Gamut::of(wide);
        std::size_t none = 1;
        rec2020->mask(spread(), proof::kDefaultThreshold, &none);
        QCOMPARE(none, std::size_t(0));
        const auto plain = proof::Gamut::of(proof::Setup{});
        for (const int v : {0, 128, 255}) QVERIFY(plain->delta_e(v, v, v) < proof::kDefaultThreshold);
    }

    // A picture's own profile, from each kind of file, and what it does: an sRGB one changes nothing, a Display P3 one
    // takes the picture's red to a deeper sRGB red; a grey one of sRGB's curve keeps the greys.
    void aPicturesOwnProfile() {
        const std::string icc = *core::builtin_profile("display-p3")->bytes;
        // PNG iCCP
        {
            uLongf size = compressBound(static_cast<uLong>(icc.size()));
            std::string packed(size, '\0');
            QCOMPARE(compress(reinterpret_cast<Bytef*>(packed.data()), &size, reinterpret_cast<const Bytef*>(icc.data()), static_cast<uLong>(icc.size())), Z_OK);
            packed.resize(size);
            std::string png("\x89PNG\r\n\x1a\n", 8);
            png += png_chunk("IHDR", std::string("\0\0\0\1\0\0\0\1\x08\x02\0\0\0", 13));
            png += png_chunk("iCCP", std::string("P3\0\0", 4) + packed);
            png += png_chunk("IEND", "");
            QCOMPARE(proof::embedded_profile(png).value_or(""), icc);
        }
        // JPEG APP2 ICC_PROFILE, in two parts
        {
            std::string jpeg("\xff\xd8", 2);
            const std::size_t half = icc.size() / 2;
            for (int part = 1; part <= 2; ++part) {
                const std::string data = std::string("ICC_PROFILE\0", 12) + static_cast<char>(part) + static_cast<char>(2) +
                                         icc.substr(part == 1 ? 0 : half, part == 1 ? half : std::string::npos);
                const std::size_t length = data.size() + 2;
                jpeg += std::string("\xff\xe2", 2) + static_cast<char>(length >> 8) + static_cast<char>(length & 0xff) + data;
            }
            jpeg += std::string("\xff\xda\0\2\xff\xd9", 6);
            QCOMPARE(proof::embedded_profile(jpeg).value_or(""), icc);
        }
        // TIFF tag 34675
        {
            std::string tiff("II*\0\x08\0\0\0", 8);
            const auto le16 = [](unsigned v) { return std::string{static_cast<char>(v & 0xff), static_cast<char>(v >> 8)}; };
            const auto le32 = [&](std::size_t v) { return le16(static_cast<unsigned>(v & 0xffff)) + le16(static_cast<unsigned>(v >> 16)); };
            tiff += le16(1) + le16(34675) + le16(7) + le32(icc.size()) + le32(8 + 2 + 12 + 4) + le32(0);
            tiff += icc;
            QCOMPARE(proof::embedded_profile(tiff).value_or(""), icc);
        }
        // WebP ICCP
        {
            std::string body = std::string("WEBP") + "ICCP";
            for (int i = 0; i < 4; ++i) body.push_back(static_cast<char>((icc.size() >> (8 * i)) & 0xff));
            body += icc;
            if (icc.size() % 2 == 1) body.push_back('\0');
            std::string webp = "RIFF";
            for (int i = 0; i < 4; ++i) webp.push_back(static_cast<char>((body.size() >> (8 * i)) & 0xff));
            QCOMPARE(proof::embedded_profile(webp + body).value_or(""), icc);
        }
        // PSD image resource 1039
        {
            std::string psd = std::string("8BPS\0\1", 6) + std::string(20, '\0') + std::string(4, '\0');  // (header, no colour mode data)
            std::string resource = std::string("8BIM\x04\x0f\0\0", 8);
            for (int i = 3; i >= 0; --i) resource.push_back(static_cast<char>((icc.size() >> (8 * i)) & 0xff));
            resource += icc;
            if (icc.size() % 2 == 1) resource.push_back('\0');
            for (int i = 3; i >= 0; --i) psd.push_back(static_cast<char>((resource.size() >> (8 * i)) & 0xff));
            QCOMPARE(proof::embedded_profile(psd + resource).value_or(""), icc);
        }
        QVERIFY(!proof::embedded_profile(std::string("\x89PNG\r\n\x1a\n", 8) + png_chunk("IEND", "")));
        QVERIFY(!proof::embedded_profile("not a picture"));
        // which ones change nothing
        QVERIFY(proof::same_as_srgb(render::colour::srgb_icc()));
        QVERIFY(proof::same_as_srgb(*core::builtin_profile("sgray")->bytes));
        QVERIFY(!proof::same_as_srgb(icc));
        QVERIFY(!proof::same_as_srgb(*core::builtin_profile("gray-gamma-2.2")->bytes));
        QVERIFY(!proof::same_as_srgb(*coated()));
        // taken to sRGB
        const render::Image red = proof::convert_picture(pixels({{255, 0, 0}, {128, 128, 128}}), icc, "relative", true);
        QCOMPARE(red.mode(), std::string_view("RGB"));
        QVERIFY(at(red, 0)[0] >= 254 && at(red, 0)[1] < 40);  // (P3's red is past sRGB's: clipped red)
        QVERIFY(grey(at(red, 1), 2));
        render::Image la = render::Image::create("LA", render::Size{1, 1}, render::Ink{100, 150});
        const render::Image kept = proof::convert_picture(la, *core::builtin_profile("sgray")->bytes, "relative", true);
        QCOMPARE(kept.mode(), std::string_view("RGBA"));
        QVERIFY(std::abs(static_cast<int>(kept.getpixel(0, 0)[0]) - 100) <= 1 && kept.getpixel(0, 0)[3] == 150);
        QVERIFY_THROWS_EXCEPTION(core::Error, proof::convert_picture(pixels({{1, 2, 3}}), *coated(), "relative", true));
    }

    // カラー設定's 文字の色をガンマで混ぜる, kept in the book: set_text_gamma (1, false or null: none), refused out of
    // 1..2.2; written under the book's "color" with color-model@1, read back; a page's own is not one (read-only); Undo.
    void theTextGammaOp() {
        const core::Document doc = lettered_book();
        QVERIFY(!doc.text_gamma);
        const core::ApplyResult set = run_ops(doc, Json::array({Json{{"op", "set_text_gamma"}, {"gamma", 1.45}}}));
        QVERIFY(set.doc.text_gamma && *set.doc.text_gamma == 1.45);
        QVERIFY(core::uses_color_model(set.doc));
        QCOMPARE(run_ops(doc, Json::array({Json{{"op", "set_text_gamma"}, {"gamma", true}}})).doc.text_gamma.value_or(0), 1.45);
        for (const Json& none : {Json(1), Json(false), Json(nullptr), Json(1.0)}) QVERIFY(!run_ops(set.doc, Json::array({Json{{"op", "set_text_gamma"}, {"gamma", none}}})).doc.text_gamma);
        for (const Json& bad : {Json(0.9), Json(2.3), Json("1.45"), Json::array()}) {
            QVERIFY2(refusal(doc, Json::array({Json{{"op", "set_text_gamma"}, {"gamma", bad}}})).starts_with("apply: "), bad.dump().c_str());
        }
        QVERIFY(!refusal(doc, Json::array({Json{{"op", "set_text_gamma"}}})).empty());
        QVERIFY(!core::uses_color_model(doc));

        QTemporaryDir tmp;
        const fs::path dir = core::path_from_utf8(tmp.filePath(QStringLiteral("book.genko")).toStdString());
        write_book(set.doc, dir);
        Json payload = Json::parse(genko::test::read_bytes(QString::fromStdString(core::path_to_utf8(dir / "project.json"))));
        QCOMPARE(payload["color"].dump(), std::string(R"({"mode":"rgb","depth":8,"intent":"relative","text_gamma":1.45})"));
        QCOMPARE(payload["features"], Json::array({"color-mode@1", "color-model@1"}));
        const auto loaded = storage::load_document(dir);
        QVERIFY2(loaded.document.read_only_reason.empty(), loaded.document.read_only_reason.c_str());
        QCOMPARE(loaded.document.text_gamma.value_or(0), 1.45);
        QVERIFY(!loaded.document.rgb && !loaded.document.color);
        // with the book's CMYK mode: under it, last
        core::Document cmyk = set.doc;
        core::PageColor inks;
        inks.space = core::InkSpace::Cmyk;
        inks.profile = core::builtin_profile("coated-cmyk")->ref();
        inks.icc = coated();
        cmyk.color = inks;
        QTemporaryDir tmp2;
        const fs::path dir2 = core::path_from_utf8(tmp2.filePath(QStringLiteral("book.genko")).toStdString());
        write_book(cmyk, dir2);
        payload = Json::parse(genko::test::read_bytes(QString::fromStdString(core::path_to_utf8(dir2 / "project.json"))));
        QCOMPARE(payload["color"].items().begin().key(), std::string("mode"));
        QCOMPARE(payload["color"]["text_gamma"], Json(1.45));
        QCOMPARE(std::prev(payload["color"].end()).key(), std::string("text_gamma"));
        const auto both = storage::load_document(dir2);
        QVERIFY(both.document.read_only_reason.empty() && both.document.color && both.document.text_gamma.value_or(0) == 1.45);
        // not one: read-only (on a page, out of range)
        for (const auto& [where, value] : {std::pair{"book", Json(3.0)}, std::pair{"page", Json(1.45)}}) {
            Json changed = Json::parse(genko::test::read_bytes(QString::fromStdString(core::path_to_utf8(dir / "project.json"))));
            if (std::string_view(where) == "book") changed["color"]["text_gamma"] = value;
            else changed["pages"][0]["color"] = Json{{"mode", "rgb"}, {"depth", 8}, {"intent", "relative"}, {"text_gamma", value}};
            QTemporaryDir other;
            const fs::path copy = core::path_from_utf8(other.filePath(QStringLiteral("book.genko")).toStdString());
            fs::copy(dir, copy, fs::copy_options::recursive);
            QFile file(QString::fromStdString(core::path_to_utf8(copy / "project.json")));
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            file.write(QByteArray::fromStdString(changed.dump()));
            file.close();
            QVERIFY2(!storage::load_document(copy).document.read_only_reason.empty(), where);
        }
    }

    // The letters drawn with the book's gamma: the edges of the letters lighter over white (upstream's
    // text_gamma_lightens_dark_edges: 69 % blue over grey 129 is (57, 80, 146)); none: Python's, byte for byte, the same
    // as a book that never had one.
    void theLettersInTheGamma() {
        render::Image grey = render::Image::create("RGB", render::Size{1, 1}, render::Ink{129, 129, 129});
        render::Image blue = render::Image::create("RGBA", render::Size{1, 1}, render::Ink{0, 51, 153, 175});
        render::text::paste_in_gamma(grey, blue, render::Point{0, 0}, 1.45);
        const auto mixed = at(grey, 0);
        QVERIFY2(std::abs(mixed[0] - 57) <= 2 && std::abs(mixed[1] - 80) <= 2 && std::abs(mixed[2] - 146) <= 2,
                 qPrintable(QStringLiteral("%1 %2 %3").arg(mixed[0]).arg(mixed[1]).arg(mixed[2])));
        const core::Document plain = lettered_book();
        const core::Document gamma = run_ops(plain, Json::array({Json{{"op", "set_text_gamma"}, {"gamma", 1.45}}})).doc;
        const core::Document back = run_ops(gamma, Json::array({Json{{"op", "set_text_gamma"}, {"gamma", nullptr}}})).doc;
        const render::Image a = render::render_page(plain.page(0), 150, render::RenderOptions{}, &plain).image;
        const render::Image b = render::render_page(gamma.page(0), 150, render::RenderOptions{}, &gamma).image;
        const render::Image c = render::render_page(back.page(0), 150, render::RenderOptions{}, &back).image;
        QCOMPARE(c.tobytes(), a.tobytes());
        const std::string x = a.tobytes(), y = b.tobytes();
        QCOMPARE(x.size(), y.size());
        std::size_t differ = 0, lighter = 0;
        for (std::size_t i = 0; i < x.size(); ++i) {
            if (x[i] == y[i]) continue;
            ++differ;
            if (static_cast<unsigned char>(y[i]) > static_cast<unsigned char>(x[i])) ++lighter;
        }
        QVERIFY(differ > 0);
        QVERIFY2(lighter * 10 >= differ * 9, qPrintable(QStringLiteral("%1 of %2").arg(lighter).arg(differ)));  // (dark letters on white: lighter edges)
        // a part of the page: the same pixels as the whole page cut there
        render::RenderOptions part;
        part.region = render::RenderRegion{40, 40, 120, 90};
        QCOMPARE(render::render_page(gamma.page(0), 150, part, &gamma).image.tobytes(), b.crop(render::Box{40, 40, 160, 130}).tobytes());
    }
};

QTEST_GUILESS_MAIN(TestProof)
#include "test_proof.moc"
