// PC5 I-f (SPEC.md AUTO-01「プリント」) as a person or a program outside uses it, this build's own contract: `genko print`
// places each sheet as プリント… does (fitted to the printable part less the marks' room, at its size, scaled, centred or at a
// position), its marks round the finished size (PhotoCraft v0.2.0's print_cmds.rs shapes: corner and centre crop marks,
// registration marks, the label and the description), the colours handled by Genko through the printer's profile (the
// page's own inks for a CMYK page) as Little CMS converts them, into a PDF or not at all (--dry-run); the book never
// written; its refusals.

#include <QCryptographicHash>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <lcms2.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

#include "core/color_model.hpp"
#include "core/command_bus.hpp"
#include "core/json.hpp"
#include "core/model.hpp"
#include "core/paths.hpp"
#include "formats/open_file.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "storage/fsutil.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/transaction.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
namespace fs = std::filesystem;

namespace {

constexpr double kPt = 25.4 / 72.0;

fs::path path_of(const QString& text) { return core::path_from_utf8(text.toStdString()); }
QString q(const fs::path& path) { return QString::fromStdString(core::path_to_utf8(path)); }

// A book of one page: a new image of w × h px at 72 ppi, its background `colour` (mode rgb or cmyk), its File Info's
// description `description` when given.
void write_book(const fs::path& dir, int w, int h, const std::string& colour, const std::string& mode = "rgb", const std::string& description = {}) {
    formats::NewImage image;
    image.title = "題名";
    image.width = w;
    image.height = h;
    image.dpi = 72;
    image.mode = mode;
    image.background = colour;
    core::Document doc = formats::new_image(image).doc;
    Json ops = Json::array();
    if (!description.empty()) ops.push_back(Json{{"op", "set_file_info"}, {"description", description}});
    if (!ops.empty()) doc = core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor("human:test")).doc;
    storage::ProjectLock lock(dir, "human:test");
    lock.try_acquire();
    storage::SaveRequest request;
    request.actor = "human:test";
    request.base_revision = doc.revision;
    request.ops = Json::array();
    storage::Saver(lock).save(doc, request);
}

std::pair<int, Json> cli(const QStringList& args) {
    const test::Run run = test::run_genko(args);
    return {run.exit_code, test::one_line(run.out)};
}

std::string sha_of(const std::string& bytes) {
    return QCryptographicHash::hash(QByteArrayView(bytes.data(), qsizetype(bytes.size())), QCryptographicHash::Sha256).toHex().toStdString();
}

// A picture of n pixels each (r, g, b), as its RGB rows.
std::string solid(std::size_t n, unsigned char r, unsigned char g, unsigned char b) {
    std::string out;
    out.reserve(n * 3);
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(static_cast<char>(r));
        out.push_back(static_cast<char>(g));
        out.push_back(static_cast<char>(b));
    }
    return out;
}

std::vector<double> numbers(const Json& list) {
    std::vector<double> out;
    for (const Json& v : list) out.push_back(v.get<double>());
    return out;
}

bool near(double a, double b, double tolerance = 0.002) { return std::abs(a - b) <= tolerance; }

}  // namespace

class TestContractPrint : public QObject {
    Q_OBJECT

private slots:
    // The sheet's place on the paper: fitted (the whole printable part of a PDF's A4, no marks: no room kept), at its size,
    // scaled, at a position; landscape; the book unchanged.
    void placesTheSheet() {
        QTemporaryDir dir;
        const fs::path book = path_of(dir.filePath("本.genko"));
        write_book(book, 120, 90, "#3366cc");
        const std::string before = storage::read_file(book / "project.json");
        const auto run = [&](QStringList more) {
            QStringList args{QStringLiteral("print"), q(book), QStringLiteral("--dry-run"), QStringLiteral("--dpi"), QStringLiteral("72")};
            args << more;
            return cli(args);
        };
        auto [code, out] = run({});
        QCOMPARE(code, 0);
        QCOMPARE(out["ok"], Json(true));
        QCOMPARE(out["dry_run"], Json(true));
        QCOMPARE(out["paper"]["name"], Json("a4"));
        QVERIFY(near(out["paper"]["width_mm"].get<double>(), 210.0, 0.01) && near(out["paper"]["height_mm"].get<double>(), 297.0, 0.01));
        QCOMPARE(out["color"], (Json{{"handling", "printer"}}));
        QCOMPARE(out["sheets"].size(), std::size_t{1});
        const Json& sheet = out["sheets"][0];
        QCOMPARE(sheet["pages"], Json::array({1}));
        QCOMPARE(sheet["pixels"], Json::array({120, 90}));
        QCOMPARE(sheet["dpi"], Json(72));
        // (120 px at 72 ppi is 42.333 mm: fitted across the 210 mm, centred down the paper)
        std::vector<double> image = numbers(sheet["image_mm"]);
        QVERIFY2(near(image[0], 0.0) && near(image[2], 210.0) && near(image[3], 157.5) && near(image[1], (297.0 - 157.5) / 2.0), qPrintable(QString::fromStdString(sheet.dump())));
        QCOMPARE(sheet["trim_mm"], sheet["image_mm"]);  // (a picture's page: its finished size is all of it)
        QVERIFY(near(sheet["scale"].get<double>(), 210.0 / (120 * 25.4 / 72) * 100.0, 0.001));
        QCOMPARE(sheet["outside"], Json(false));
        QVERIFY(sheet["marks"]["lines"].empty() && sheet["marks"]["circles"].empty() && sheet["texts"].empty());
        // the picture as the page prints: its colour as it is (the printer manages the colours)
        QCOMPARE(sheet["sha256"], Json(sha_of(solid(120 * 90, 0x33, 0x66, 0xcc))));
        // at its size; scaled; at a position (from the paper's corner, mm)
        std::tie(code, out) = run({QStringLiteral("--scale"), QStringLiteral("actual")});
        QCOMPARE(code, 0);
        image = numbers(out["sheets"][0]["image_mm"]);
        QVERIFY(near(image[2], 42.333) && near(image[3], 31.75) && near(image[0], (210.0 - 42.333) / 2.0, 0.003));
        QVERIFY(near(out["sheets"][0]["scale"].get<double>(), 100.0));
        std::tie(code, out) = run({QStringLiteral("--scale"), QStringLiteral("50%"), QStringLiteral("--position"), QStringLiteral("10,20")});
        QCOMPARE(code, 0);
        image = numbers(out["sheets"][0]["image_mm"]);
        QVERIFY(near(image[0], 10.0) && near(image[1], 20.0) && near(image[2], 21.167) && near(image[3], 15.875));
        // past the paper: said
        std::tie(code, out) = run({QStringLiteral("--scale"), QStringLiteral("1000%"), QStringLiteral("--position"), QStringLiteral("100,100")});
        QCOMPARE(code, 0);
        QCOMPARE(out["sheets"][0]["outside"], Json(true));
        // landscape A5, and a paper of its own size
        std::tie(code, out) = run({QStringLiteral("--paper"), QStringLiteral("a5"), QStringLiteral("--orientation"), QStringLiteral("landscape")});
        QCOMPARE(code, 0);
        QVERIFY(near(out["paper"]["width_mm"].get<double>(), 210.0, 0.01) && near(out["paper"]["height_mm"].get<double>(), 148.0, 0.01));
        std::tie(code, out) = run({QStringLiteral("--paper"), QStringLiteral("100x150")});
        QCOMPARE(code, 0);
        QVERIFY(near(out["paper"]["width_mm"].get<double>(), 100.0, 0.01) && near(out["paper"]["height_mm"].get<double>(), 150.0, 0.01));
        QCOMPARE(storage::read_file(book / "project.json"), before);  // (read only: never written)
        QVERIFY(!fs::exists(book / "project.lock"));
    }

    // The marks (PhotoCraft's shapes and places): the corner crop marks on the finished size's corners outside the picture,
    // the centre crop marks, the registration marks (a ring and a cross each), the label above and the description under;
    // the room they take when the sheet is fitted.
    void putsTheMarks() {
        QTemporaryDir dir;
        const fs::path book = path_of(dir.filePath("印.genko"));
        write_book(book, 120, 90, "#3366cc", "rgb", "説明の文");
        auto [code, out] = cli({QStringLiteral("print"), q(book), QStringLiteral("--dry-run"), QStringLiteral("--dpi"), QStringLiteral("72"), QStringLiteral("--scale"),
                                QStringLiteral("actual"), QStringLiteral("--marks"), QStringLiteral("corner,center,registration,labels,description")});
        QCOMPARE(code, 0);
        const Json& sheet = out["sheets"][0];
        const std::vector<double> image = numbers(sheet["image_mm"]);
        const double gap = 6 * kPt, length = 18 * kPt, ring = 5 * kPt;
        const double left = image[0], top = image[1], right = image[0] + image[2], bottom = image[1] + image[3];
        const Json& lines = sheet["marks"]["lines"];
        QCOMPARE(lines.size(), std::size_t{8 + 4 + 8});  // (corner 8, centre 4, the registration marks' crosses 8)
        // the first corner: the top left one, across then down
        const std::vector<double> across = numbers(lines[0]), down = numbers(lines[1]);
        QVERIFY2(near(across[0], left - gap) && near(across[1], top) && near(across[2], left - gap - length) && near(across[3], top), qPrintable(QString::fromStdString(lines.dump())));
        QVERIFY(near(down[0], left) && near(down[1], top - gap) && near(down[2], left) && near(down[3], top - gap - length));
        // the last corner: the bottom right one
        const std::vector<double> br = numbers(lines[6]), br_down = numbers(lines[7]);
        QVERIFY(near(br[0], right + gap) && near(br[1], bottom) && near(br[2], right + gap + length));
        QVERIFY(near(br_down[0], right) && near(br_down[1], bottom + gap) && near(br_down[3], bottom + gap + length));
        // the centre marks: the top one on the middle
        const std::vector<double> centre = numbers(lines[8]);
        QVERIFY(near(centre[0], (left + right) / 2.0) && near(centre[1], top - gap) && near(centre[3], top - gap - length));
        // the registration marks: four rings at the middles, half a mark's length beyond the gap
        const Json& rings = sheet["marks"]["circles"];
        QCOMPARE(rings.size(), std::size_t{4});
        const std::vector<double> first = numbers(rings[0]);
        QVERIFY(near(first[0], (left + right) / 2.0) && near(first[1], top - gap - length / 2.0) && near(first[2], ring));
        // the texts: the label above (the book's title and its page), the description under
        QCOMPARE(sheet["texts"].size(), std::size_t{2});
        QCOMPARE(sheet["texts"][0]["kind"], Json("label"));
        QCOMPARE(sheet["texts"][0]["text"], Json("題名　1 ページ"));
        QVERIFY(near(numbers(sheet["texts"][0]["at_mm"])[1], top - 30 * kPt));
        QCOMPARE(sheet["texts"][1]["kind"], Json("description"));
        QCOMPARE(sheet["texts"][1]["text"], Json("説明の文"));
        QVERIFY(near(numbers(sheet["texts"][1]["at_mm"])[1], bottom + 30 * kPt + 8 * kPt));
        // fitted with marks: half an inch kept round it
        std::tie(code, out) = cli({QStringLiteral("print"), q(book), QStringLiteral("--dry-run"), QStringLiteral("--dpi"), QStringLiteral("72"), QStringLiteral("--marks"),
                                   QStringLiteral("corner")});
        QCOMPARE(code, 0);
        QVERIFY(near(numbers(out["sheets"][0]["image_mm"])[2], 210.0 - 2 * 12.7));
        // labels alone: a quarter inch, the label 6 pt above
        std::tie(code, out) = cli({QStringLiteral("print"), q(book), QStringLiteral("--dry-run"), QStringLiteral("--dpi"), QStringLiteral("72"), QStringLiteral("--marks"),
                                   QStringLiteral("labels")});
        QCOMPARE(code, 0);
        const std::vector<double> labelled = numbers(out["sheets"][0]["image_mm"]);
        QVERIFY(near(labelled[2], 210.0 - 2 * 6.35));
        QVERIFY(near(numbers(out["sheets"][0]["texts"][0]["at_mm"])[1], labelled[1] - 6 * kPt));
        QVERIFY(out["sheets"][0]["marks"]["lines"].empty());
    }

    // Genko's colour management: the picture through the printer's profile (a built-in one, or the same as an ICC file) by
    // the rendering intent with or without black point compensation, as Little CMS converts it; a CMYK page from its own
    // inks through its profile. A CMYK or grey printer profile is refused (Qt prints RGB).
    void handlesTheColours() {
        QTemporaryDir dir;
        const fs::path book = path_of(dir.filePath("色.genko"));
        write_book(book, 40, 30, "#3366cc");
        const core::BuiltinProfile* p3 = core::builtin_profile("display-p3");
        QVERIFY(p3 != nullptr && p3->bytes);
        const auto converted = [&](const unsigned char* in, cmsUInt32Number format, cmsHPROFILE from, cmsUInt32Number intent, bool bpc) {
            cmsHPROFILE to = cmsOpenProfileFromMem(p3->bytes->data(), static_cast<cmsUInt32Number>(p3->bytes->size()));
            cmsHTRANSFORM t = cmsCreateTransform(from, format, to, TYPE_RGB_8, intent, (bpc ? cmsFLAGS_BLACKPOINTCOMPENSATION : 0) | cmsFLAGS_NOCACHE);
            unsigned char out[3] = {0, 0, 0};
            cmsDoTransform(t, in, out, 1);
            cmsDeleteTransform(t);
            cmsCloseProfile(to);
            return std::array<unsigned char, 3>{out[0], out[1], out[2]};
        };
        const unsigned char blue[3] = {0x33, 0x66, 0xcc};
        for (const auto& [intent, number, bpc] : {std::tuple{"relative", INTENT_RELATIVE_COLORIMETRIC, true}, std::tuple{"perceptual", INTENT_PERCEPTUAL, false}}) {
            QStringList args{QStringLiteral("print"), q(book), QStringLiteral("--dry-run"), QStringLiteral("--dpi"), QStringLiteral("72"),
                             QStringLiteral("--profile"), QStringLiteral("display-p3"), QStringLiteral("--intent"), QString::fromLatin1(intent)};
            if (!bpc) args << QStringLiteral("--no-bpc");
            auto [code, out] = cli(args);
            QCOMPARE(code, 0);
            QCOMPARE(out["color"], (Json{{"handling", "genko"}, {"profile", "display-p3"}, {"intent", intent}, {"bpc", bpc}}));
            cmsHPROFILE srgb = cmsCreate_sRGBProfile();
            const auto want = converted(blue, TYPE_RGB_8, srgb, number, bpc);
            cmsCloseProfile(srgb);
            QVERIFY(want != (std::array<unsigned char, 3>{0x33, 0x66, 0xcc}));  // (another space's numbers)
            QCOMPARE(out["sheets"][0]["sha256"], Json(sha_of(solid(40 * 30, want[0], want[1], want[2]))));
        }
        // the same profile as an ICC file
        const fs::path icc = path_of(dir.filePath("printer.icc"));
        storage::write_atomic(icc, *p3->bytes);
        auto [code, by_id] = cli({QStringLiteral("print"), q(book), QStringLiteral("--dry-run"), QStringLiteral("--dpi"), QStringLiteral("72"), QStringLiteral("--profile"),
                                  QStringLiteral("display-p3")});
        QCOMPARE(code, 0);
        Json by_file;
        std::tie(code, by_file) = cli({QStringLiteral("print"), q(book), QStringLiteral("--dry-run"), QStringLiteral("--dpi"), QStringLiteral("72"), QStringLiteral("--profile"), q(icc)});
        QCOMPARE(code, 0);
        QCOMPARE(by_file["sheets"][0]["sha256"], by_id["sheets"][0]["sha256"]);
        // a CMYK page: its own inks through its profile
        const fs::path inked = path_of(dir.filePath("インキ.genko"));
        write_book(inked, 40, 30, "#3366cc", "cmyk");
        Json ink_out;
        std::tie(code, ink_out) = cli({QStringLiteral("print"), q(inked), QStringLiteral("--dry-run"), QStringLiteral("--dpi"), QStringLiteral("72"), QStringLiteral("--profile"),
                                       QStringLiteral("display-p3")});
        QCOMPARE(code, 0);
        const core::Document doc = storage::load_document(inked).document;
        const core::Page& page = doc.page(0);
        QVERIFY(page.color && page.color->icc);
        render::RenderOptions look;
        look.mode = "print";
        look.inks = true;
        const render::RenderResult drawn = render::render_page(page, 72, look, &doc);
        QVERIFY(drawn.inks);
        float cmyk[4];
        for (unsigned c = 0; c < 4; ++c) cmyk[c] = static_cast<float>(drawn.inks->value(0, c)) * 100.0f;
        cmsHPROFILE own = cmsOpenProfileFromMem(page.color->icc->data(), static_cast<cmsUInt32Number>(page.color->icc->size()));
        const auto want = converted(reinterpret_cast<const unsigned char*>(cmyk), TYPE_CMYK_FLT, own, INTENT_RELATIVE_COLORIMETRIC, true);
        cmsCloseProfile(own);
        QCOMPARE(ink_out["sheets"][0]["sha256"], Json(sha_of(solid(40 * 30, want[0], want[1], want[2]))));
        // refused: a CMYK printer profile, a grey one, one not there, an intent not known; --intent without --profile
        for (const QString& bad : {QStringLiteral("coated-cmyk"), QStringLiteral("gray-gamma-2.2"), q(path_of(dir.filePath("無い.icc")))}) {
            Json refused;
            std::tie(code, refused) = cli({QStringLiteral("print"), q(book), QStringLiteral("--dry-run"), QStringLiteral("--profile"), bad});
            QCOMPARE(code, 1);
            QCOMPARE(refused["code"], Json("value"));
            QVERIFY2(refused["error"].get<std::string>().starts_with("the printer's profile"), qPrintable(QString::fromStdString(refused.dump())));
        }
        Json refused;
        std::tie(code, refused) = cli({QStringLiteral("print"), q(book), QStringLiteral("--dry-run"), QStringLiteral("--profile"), QStringLiteral("display-p3"),
                                       QStringLiteral("--intent"), QStringLiteral("vivid")});
        QCOMPARE(code, 1);
        QVERIFY(refused["error"].get<std::string>().starts_with("the rendering intent"));
        const test::Run usage = test::run_genko({QStringLiteral("print"), q(book), QStringLiteral("--dry-run"), QStringLiteral("--intent"), QStringLiteral("perceptual")});
        QCOMPARE(usage.exit_code, 2);
    }

    // Into a PDF: a page for each sheet (a spread's two pages on one), the file written; a dry run writes none; the usage
    // and what the command refuses.
    void printsIntoAPdf() {
        QTemporaryDir dir;
        const fs::path book = path_of(dir.filePath("PDF.genko"));
        write_book(book, 60, 40, "#ffcc00");
        const QString pdf = dir.filePath("出力.pdf");
        auto [code, out] = cli({QStringLiteral("print"), q(book), QStringLiteral("--pdf"), pdf, QStringLiteral("--dpi"), QStringLiteral("72"), QStringLiteral("--marks"),
                                QStringLiteral("corner,registration,labels")});
        QCOMPARE(code, 0);
        QCOMPARE(out["dry_run"], Json(false));
        QCOMPARE(out["pdf"], Json(pdf.toStdString()));
        QFile file(pdf);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray data = file.readAll();
        QVERIFY(data.startsWith("%PDF"));
        QCOMPARE(data.count("/Type /Page") - data.count("/Type /Pages"), 1);
        // a dry run with --pdf writes nothing
        const QString none = dir.filePath("無し.pdf");
        std::tie(code, out) = cli({QStringLiteral("print"), q(book), QStringLiteral("--pdf"), none, QStringLiteral("--dry-run")});
        QCOMPARE(code, 0);
        QVERIFY(!QFile::exists(none));
        // the usage: two outputs, a paper, a scale, an area, marks, copies, a page not there
        for (const QStringList& bad : {QStringList{QStringLiteral("--pdf"), pdf, QStringLiteral("--printer"), QStringLiteral("x")},
                                       QStringList{QStringLiteral("--paper"), QStringLiteral("napkin")}, QStringList{QStringLiteral("--scale"), QStringLiteral("big")},
                                       QStringList{QStringLiteral("--area"), QStringLiteral("all")}, QStringList{QStringLiteral("--marks"), QStringLiteral("stars")},
                                       QStringList{QStringLiteral("--copies"), QStringLiteral("0")}, QStringList{QStringLiteral("--position"), QStringLiteral("1")},
                                       QStringList{QStringLiteral("--dpi"), QStringLiteral("9")}, QStringList{QStringLiteral("--wat")}}) {
            QStringList args{QStringLiteral("print"), q(book), QStringLiteral("--dry-run")};
            args << bad;
            const test::Run run = test::run_genko(args);
            QVERIFY2(run.exit_code == 2, qPrintable(bad.join(QLatin1Char(' '))));
        }
        std::tie(code, out) = cli({QStringLiteral("print"), q(book), QStringLiteral("--dry-run"), QStringLiteral("--pages"), QStringLiteral("5")});
        QCOMPARE(code, 1);
        // a printer the computer does not have
        std::tie(code, out) = cli({QStringLiteral("print"), q(book), QStringLiteral("--printer"), QStringLiteral("無いプリンター")});
        QCOMPARE(code, 1);
        QCOMPARE(out["code"], Json("not_found"));
    }
};

QTEST_GUILESS_MAIN(TestContractPrint)
#include "test_contract_print.moc"
