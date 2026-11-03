// PC5 I-h (SPEC.md AUTO-01「HDR Pro に統合」「写真の切り抜きと角度補正」) as a person or a program outside uses it, this build's
// own contract: `genko automate hdr` merges exposures of one scene — PNGs and JPEGs of a known radiance shot at three
// exposures, synthetic camera RAW files (PC4 F-e's maker: support/raw_testgen) — into a new book written at --book: at 32
// bits a page of linear light in the built-in linear-srgb (its values past 1 kept, proportional to the scene), at 16 and 8
// bits tone mapped (sRGB); the exposures given (--ev), the cameras' (EXIF) or estimated; the pictures never written; what it
// refuses (one picture, different sizes, RAW files with pictures that are not, a book there already). `genko automate
// crop-photos` writes a book of each photo on a scan (the scan unchanged), says when there are none.

#include <QTemporaryDir>
#include <QtTest>

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/color_model.hpp"
#include "core/exif.hpp"
#include "core/json.hpp"
#include "core/paths.hpp"
#include "formats/page_picture.hpp"
#include "raw_testgen.hpp"
#include "render/codecs/codecs.hpp"
#include "render/raw.hpp"
#include "storage/fsutil.hpp"
#include "storage/reader.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
namespace fs = std::filesystem;
namespace codecs = render::codecs;
namespace rawgen = genko::test::rawgen;

namespace {

fs::path path_of(const QString& text) { return core::path_from_utf8(text.toStdString()); }
QString q(const fs::path& path) { return QString::fromStdString(core::path_to_utf8(path)); }

void write(const fs::path& file, const std::string& bytes) {
    std::ofstream out(file, std::ios::binary);
    out << bytes;
}

std::pair<int, Json> cli(const QStringList& args) {
    const test::Run run = test::run_genko(args);
    return {run.exit_code, test::one_line(run.out)};
}

constexpr std::size_t kW = 120, kH = 48;

// The scene's radiance: 10 stops left to right, a texture down.
std::array<float, 3> radiance(std::size_t x, std::size_t y) {
    const float base = 0.004f * std::pow(2.0f, static_cast<float>(x) / kW * 10.0f);
    const float tex = 1.0f + 0.25f * std::sin(static_cast<float>(y) * 0.4f);
    return {base * tex, base * tex * 0.85f, base * tex * 0.7f};
}

// The scene shot at `dt` through sRGB's curve, 8 bits (`exif`: the camera's shutter written into it).
std::string shot(float dt, codecs::Format format, std::optional<double> exif_seconds = std::nullopt) {
    std::vector<float> v;
    for (std::size_t y = 0; y < kH; ++y) {
        for (std::size_t x = 0; x < kW; ++x) {
            for (const float c : radiance(x, y)) {
                const float l = std::min(c * dt, 1.0f);
                v.push_back(l <= 0.0031308f ? 12.92f * l : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f);
            }
        }
    }
    codecs::CodecImage image = codecs::CodecImage::from_normalized(kW, kH, codecs::Layout::Rgb, codecs::Sample::U8, v);
    if (exif_seconds) {
        core::exif::Camera camera;
        camera.exposure_time = *exif_seconds;
        camera.f_number = 8.0;
        camera.iso = 100.0;
        image.meta.exif = core::exif::build(camera);
    }
    codecs::EncodeOptions how;
    if (format == codecs::Format::Jpeg) how.jpeg_quality = 95;
    return codecs::encode(image, format, how);
}

// The same scene on a camera's sensor at `dt`: a 12-bit CFA DNG (linear).
std::string raw_shot(float dt) {
    std::vector<std::array<float, 3>> rgb(kW * kH);
    for (std::size_t y = 0; y < kH; ++y)
        for (std::size_t x = 0; x < kW; ++x) {
            const auto r = radiance(x, y);
            for (std::size_t c = 0; c < 3; ++c) rgb[y * kW + x][c] = std::min(r[c] * dt, 1.0f);
        }
    rawgen::DngSpec d = rawgen::DngSpec::cfa_of(kW, kH, rawgen::mosaic(rgb, kW, {0, 1, 1, 2}, 64, 4000));
    d.bits = 12;
    d.white = 4000;
    d.black = {64};
    d.as_shot_neutral = std::array<double, 3>{1.0, 1.0, 1.0};
    d.color_matrix1 = std::make_pair(std::uint16_t(21), std::array<double, 9>{3.2404542, -1.5371385, -0.4985314, -0.9692660, 1.8760108, 0.0415560, 0.0556434,
                                                                              -0.2040259, 1.0572252});
    return d.build();
}

core::Document book_at(const fs::path& dir) { return storage::load_document(dir).document; }

// The page's picture at its own grid.
codecs::CodecImage picture_of(const core::Document& doc) {
    const core::Page& page = doc.page(0);
    return formats::page_picture(page, doc, static_cast<int>(std::lround(page.spec.dpi.value()))).image;
}

}  // namespace

class TestContractHdr : public QObject {
    Q_OBJECT

private slots:
    void thirtyTwoBitsOfLinearLight() {
        QTemporaryDir dir;
        const fs::path root = path_of(dir.path());
        const std::vector<float> dts{0.25f, 1.0f, 4.0f};
        QStringList args{QStringLiteral("automate"), QStringLiteral("hdr")};
        for (std::size_t i = 0; i < dts.size(); ++i) {
            const fs::path file = root / ("s" + std::to_string(i) + ".png");
            write(file, shot(dts[i], codecs::Format::Png));
            args << q(file);
        }
        const std::string before = storage::read_file(root / "s1.png");
        // (--no-align: this scene is a ramp in one direction clipped over much of the brightest picture, where Ward's MTB has
        // no edges to lock on — the alignment is test_hdr_merge's; here the light)
        const auto [code, out] = cli(args + QStringList{QStringLiteral("--book"), q(root / "hdr"), QStringLiteral("--ev=-2,0,2"),
                                                         QStringLiteral("--no-align"), QStringLiteral("--json")});
        QVERIFY2(code == 0, out.dump().c_str());
        QCOMPARE(out["ok"].get<bool>(), true);
        QCOMPARE(out["files"], Json::array({core::path_to_utf8(root / "hdr")}));
        QCOMPARE(out["hdr"]["mode"].get<int>(), 32);
        QCOMPARE(out["hdr"]["exposure_source"].get<std::string>(), std::string("given"));
        QCOMPARE(out["hdr"]["ev"], Json::array({0.0, 2.0, 4.0}));
        QCOMPARE(storage::read_file(root / "s1.png"), before);  // (the pictures are never written)
        const core::Document doc = book_at(root / "hdr");
        QCOMPARE(doc.pages.size(), std::size_t{1});
        const core::Page& page = doc.page(0);
        QVERIFY(page.rgb.has_value());
        QCOMPARE(page.rgb->depth, 32);
        QVERIFY(page.rgb->icc != nullptr);
        QCOMPARE(*page.rgb->icc, *core::builtin_profile("linear-srgb")->bytes);
        // (its values: the scene's light, in proportion across 10 stops, past 1 kept)
        const codecs::CodecImage pic = picture_of(doc);
        QCOMPARE(pic.sample(), codecs::Sample::F32);
        QCOMPARE(pic.width(), std::uint32_t(kW));
        const auto ratio = [&](std::uint32_t x) { return pic.at(x, kH / 2, 1) / radiance(x, kH / 2)[1]; };
        const float k = ratio(kW / 2);
        for (const std::uint32_t x : {8u, 30u, 90u, 110u}) QVERIFY2(std::abs(std::log(ratio(x) / k)) < 0.1, qPrintable(QStringLiteral("%1: %2").arg(x).arg(ratio(x) / k)));
        float most = 0;
        for (std::uint32_t x = 0; x < kW; ++x) most = std::max(most, pic.at(x, kH / 2, 0));
        QVERIFY2(most > 1.0f, qPrintable(QString::number(most)));
        // (never over a book)
        const auto [again, refused] = cli(args + QStringList{QStringLiteral("--book"), q(root / "hdr"), QStringLiteral("--json")});
        QCOMPARE(again, 1);
        QCOMPARE(refused["code"].get<std::string>(), std::string("exists"));
    }

    void toneMappedAndTheCamerasExposures() {
        QTemporaryDir dir;
        const fs::path root = path_of(dir.path());
        const std::vector<float> dts{0.5f, 2.0f, 8.0f};
        QStringList files;
        for (std::size_t i = 0; i < dts.size(); ++i) {
            const fs::path file = root / ("s" + std::to_string(i) + ".jpg");
            write(file, shot(dts[i], codecs::Format::Jpeg, dts[i] / 100.0));
            files << q(file);
        }
        for (const auto& [mode, method] : {std::pair{16, "local_adaptation"}, std::pair{8, "highlight_compression"}, std::pair{8, "equalize_histogram"},
                                           std::pair{16, "exposure_gamma"}}) {
            const fs::path dest = root / ("t" + std::to_string(mode) + method);
            const auto [code, out] = cli(QStringList{QStringLiteral("automate"), QStringLiteral("hdr")} + files +
                                         QStringList{QStringLiteral("--book"), q(dest), QStringLiteral("--mode"), QString::number(mode), QStringLiteral("--method"),
                                                     QString::fromLatin1(method), QStringLiteral("--no-align"), QStringLiteral("--json")});
            QVERIFY2(code == 0, out.dump().c_str());
            QCOMPARE(out["hdr"]["exposure_source"].get<std::string>(), std::string("exif"));
            QVERIFY(std::abs(out["hdr"]["ev"][2].get<double>() - 4.0) < 1e-6);
            QCOMPARE(out["hdr"]["method"].get<std::string>(), std::string(method));
            const core::Document doc = book_at(dest);
            const core::Page& page = doc.page(0);
            QCOMPARE(page.rgb ? page.rgb->depth : 8, mode);
            QVERIFY(!page.rgb || page.rgb->icc == nullptr);  // (sRGB)
            const codecs::CodecImage pic = picture_of(doc);
            QCOMPARE(pic.sample(), mode == 16 ? codecs::Sample::U16 : codecs::Sample::U8);
            // (both ends of the bracket seen — past x ≈ 105 even the darkest picture is white —: darker on the left than on the
            // right, neither black nor white)
            const float left = pic.at(4, kH / 2, 1), right = pic.at(100, kH / 2, 1);
            QVERIFY2(left < right, method);
            if (std::string_view(method) != "exposure_gamma") QVERIFY2(left > 0.01f && right < 0.999f, qPrintable(QStringLiteral("%1: %2 %3").arg(method).arg(left).arg(right)));
        }
        // (--settings of the method; ghosts with a base; estimated without the cameras')
        const auto [code, out] = cli(QStringList{QStringLiteral("automate"), QStringLiteral("hdr")} + files +
                                     QStringList{QStringLiteral("--book"), q(root / "g"), QStringLiteral("--mode"), QStringLiteral("8"), QStringLiteral("--settings"),
                                                 QStringLiteral("{\"radius\": 20, \"strength\": 1}"), QStringLiteral("--remove-ghosts"), QStringLiteral("--ghost-base"),
                                                 QStringLiteral("2"), QStringLiteral("--no-align"), QStringLiteral("--json")});
        QVERIFY2(code == 0, out.dump().c_str());
        QCOMPARE(out["hdr"]["ghost_base"].get<int>(), 1);
        QCOMPARE(out["hdr"]["offsets"], Json::array({Json::array({0, 0}), Json::array({0, 0}), Json::array({0, 0})}));
    }

    // A camera RAW file is linear light: merged as it is (no response), the result in proportion to the scene.
    void cameraRawFilesAsLinearLight() {
        QTemporaryDir dir;
        const fs::path root = path_of(dir.path());
        const std::vector<float> dts{0.25f, 1.0f, 4.0f};
        QStringList files;
        for (std::size_t i = 0; i < dts.size(); ++i) {
            const fs::path file = root / ("r" + std::to_string(i) + ".dng");
            write(file, raw_shot(dts[i]));
            files << q(file);
        }
        const auto [code, out] = cli(QStringList{QStringLiteral("automate"), QStringLiteral("hdr")} + files +
                                     QStringList{QStringLiteral("--book"), q(root / "raw"), QStringLiteral("--ev"), QStringLiteral("0,2,4"), QStringLiteral("--no-align"),
                                                 QStringLiteral("--json")});
        QVERIFY2(code == 0, out.dump().c_str());
        const core::Document doc = book_at(root / "raw");
        QCOMPARE(doc.page(0).rgb->depth, 32);
        const codecs::CodecImage pic = picture_of(doc);
        QCOMPARE(pic.width(), std::uint32_t(kW));
        // (within the bracket the light is the scene's, in proportion — the sensor's linear values, no curve guessed)
        const auto ratio = [&](std::uint32_t x) { return pic.at(x, kH / 2, 1) / radiance(x, kH / 2)[1]; };
        const float k = ratio(kW / 2);
        for (const std::uint32_t x : {10u, 30u, 80u, 105u}) QVERIFY2(std::abs(std::log(ratio(x) / k)) < 0.1, qPrintable(QStringLiteral("%1: %2").arg(x).arg(ratio(x) / k)));
        // (RAW files with pictures that are not: refused)
        write(root / "p.png", shot(1.0f, codecs::Format::Png));
        const auto [mixed, refused] = cli(QStringList{QStringLiteral("automate"), QStringLiteral("hdr"), files[0], q(root / "p.png"), QStringLiteral("--book"),
                                                      q(root / "mixed"), QStringLiteral("--json")});
        QCOMPARE(mixed, 1);
        QCOMPARE(refused["code"].get<std::string>(), std::string("value"));
        QVERIFY(!fs::exists(root / "mixed"));
    }

    void whatItRefuses() {
        QTemporaryDir dir;
        const fs::path root = path_of(dir.path());
        write(root / "a.png", shot(1.0f, codecs::Format::Png));
        write(root / "b.png", shot(4.0f, codecs::Format::Png));
        write(root / "small.png", codecs::encode(codecs::CodecImage::blank(10, 10, codecs::Layout::Rgb, codecs::Sample::U8), codecs::Format::Png));
        const auto run = [&](const QStringList& more) { return cli(QStringList{QStringLiteral("automate"), QStringLiteral("hdr")} + more); };
        auto [code, out] = run({q(root / "a.png"), QStringLiteral("--book"), q(root / "x")});
        QCOMPARE(code, 1);
        QCOMPARE(out["code"].get<std::string>(), std::string("value"));  // (one picture)
        std::tie(code, out) = run({q(root / "a.png"), q(root / "small.png"), QStringLiteral("--book"), q(root / "x")});
        QCOMPARE(code, 1);
        QCOMPARE(out["code"].get<std::string>(), std::string("value"));  // (sizes)
        QVERIFY(!fs::exists(root / "x"));
        for (const QStringList& usage : {QStringList{q(root / "a.png"), q(root / "b.png")},  // (no --book)
                                         QStringList{q(root / "a.png"), q(root / "b.png"), QStringLiteral("--book"), q(root / "x"), QStringLiteral("--mode"), QStringLiteral("12")},
                                         QStringList{q(root / "a.png"), q(root / "b.png"), QStringLiteral("--book"), q(root / "x"), QStringLiteral("--method"),
                                                     QStringLiteral("local_adaptation")},  // (a method for 32 bits)
                                         QStringList{q(root / "a.png"), q(root / "b.png"), QStringLiteral("--book"), q(root / "x"), QStringLiteral("--ev"), QStringLiteral("0")},
                                         QStringList{q(root / "a.png"), q(root / "b.png"), QStringLiteral("--book"), q(root / "x"), QStringLiteral("--ghost-base"), QStringLiteral("1")},
                                         QStringList{q(root / "a.png"), q(root / "b.png"), QStringLiteral("--book"), q(root / "x"), QStringLiteral("--ev"), QStringLiteral("0,40")}}) {
            const test::Run r = test::run_genko(QStringList{QStringLiteral("automate"), QStringLiteral("hdr")} + usage);
            QVERIFY2(r.exit_code == 2, qPrintable(r.described()));
        }
        QVERIFY(!fs::exists(root / "x"));
        // (the review's P1-1, P2-1: too much to hold, seen from the headers before anything is decoded — these are headers
        // with no pixels, which decoding would call broken)
        QStringList big;
        for (int i = 0; i < 8; ++i) {
            const fs::path file = root / ("big" + std::to_string(i) + ".ppm");
            write(file, "P6\n6000 6000\n255\n");
            big << q(file);
        }
        const auto [big_code, big_out] = run(big + QStringList{QStringLiteral("--book"), q(root / "x")});
        QCOMPARE(big_code, 1);
        QCOMPARE(big_out["code"].get<std::string>(), std::string("image_too_large"));
        QVERIFY(!fs::exists(root / "x"));
        // (a camera RAW file's linear development counted at its 18 bytes a pixel: refused past what it is given)
        render::raw::Options tight;
        tight.limits.max_alloc = 100000;  // (LibRaw's stage fits — 16 bytes a pixel —, the linear one's 18 do not)
        try {
            render::raw::develop_linear(raw_shot(1.0f), tight);
            QFAIL("developed");
        } catch (const core::Error& error) {
            QCOMPARE(error.code(), std::string("image_too_large"));
        }
    }

    void cropPhotosWritesABookOfEachPhoto() {
        QTemporaryDir dir;
        const fs::path root = path_of(dir.path());
        // (a white lid with two tilted grey prints)
        const std::size_t w = 500, h = 360;
        std::vector<float> v(w * h * 3, 0.97f);
        const std::array<std::array<double, 5>, 2> prints{{{140, 120, 160, 110, 6}, {360, 240, 150, 120, -9}}};
        for (const auto& p : prints) {
            const double rad = p[4] * 3.14159265358979 / 180.0;
            for (std::size_t y = 0; y < h; ++y)
                for (std::size_t x = 0; x < w; ++x) {
                    const double dx = x + 0.5 - p[0], dy = y + 0.5 - p[1];
                    const double u = std::cos(rad) * dx + std::sin(rad) * dy, t = -std::sin(rad) * dx + std::cos(rad) * dy;
                    if (std::abs(u) < p[2] / 2 && std::abs(t) < p[3] / 2)
                        for (std::size_t c = 0; c < 3; ++c) v[(y * w + x) * 3 + c] = 0.35f + 0.1f * static_cast<float>(c);
                }
        }
        write(root / "scan.png", codecs::encode(codecs::CodecImage::from_normalized(w, h, codecs::Layout::Rgb, codecs::Sample::U8, v), codecs::Format::Png));
        const std::string before = storage::read_file(root / "scan.png");
        const auto [code, out] = cli({QStringLiteral("automate"), QStringLiteral("crop-photos"), q(root / "scan.png"), QStringLiteral("--out"), q(root / "photos"),
                                      QStringLiteral("--json")});
        QVERIFY2(code == 0, out.dump().c_str());
        QCOMPARE(out["files"], Json::array({core::path_to_utf8(root / "photos" / "scan-1"), core::path_to_utf8(root / "photos" / "scan-2")}));
        QCOMPARE(out["photos"].size(), std::size_t{2});
        for (std::size_t k = 0; k < 2; ++k) {
            QVERIFY(std::abs(out["photos"][k]["angle"].get<double>() - prints[k][4]) < 0.5);
            const core::Document doc = book_at(root / "photos" / ("scan-" + std::to_string(k + 1)));
            const codecs::CodecImage pic = picture_of(doc);
            QVERIFY(std::abs(static_cast<double>(pic.width()) - prints[k][2]) < 6 && std::abs(static_cast<double>(pic.height()) - prints[k][3]) < 6);
            QVERIFY(std::abs(pic.at(pic.width() / 2, pic.height() / 2, 0) - 0.35f) < 0.01f);
        }
        QCOMPARE(storage::read_file(root / "scan.png"), before);
        // (again: the books there already, none written; a uniform scan: none found)
        const auto [again, there] = cli({QStringLiteral("automate"), QStringLiteral("crop-photos"), q(root / "scan.png"), QStringLiteral("--out"), q(root / "photos")});
        QCOMPARE(again, 1);
        QCOMPARE(there["code"].get<std::string>(), std::string("exists"));
        write(root / "white.png", codecs::encode(codecs::CodecImage::from_normalized(w, h, codecs::Layout::Rgb, codecs::Sample::U8, std::vector<float>(w * h * 3, 1.0f)),
                                                 codecs::Format::Png));
        const auto [none, said] = cli({QStringLiteral("automate"), QStringLiteral("crop-photos"), q(root / "white.png"), QStringLiteral("--out"), q(root / "none")});
        QCOMPARE(none, 1);
        QCOMPARE(said["code"].get<std::string>(), std::string("not_found"));
        QVERIFY(!fs::exists(root / "none"));
    }
};

QTEST_GUILESS_MAIN(TestContractHdr)
#include "test_contract_hdr.moc"
