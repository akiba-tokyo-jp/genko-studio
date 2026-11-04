// PC5 I-h (SPEC.md AUTO-01「写真の切り抜きと角度補正」): render/scancrop — PhotoCraft v0.2.0's algo/src/scancrop.rs test
// (MIT) and more: a scan of rotated textured photos on a white lid gives their number, centres, sides and angles (within
// 0.5°) in reading order, none on a uniform scan; a photo cut out upright matches the texture it was drawn from, at the
// scan's layout, depth, profile and resolution; then service/automate_photos: a page's photos made books of one page
// ("… のコピー n", the page unchanged), at most kMaxPhotos.

#include <QtTest>

#include <array>
#include <cmath>
#include <numbers>
#include <string>
#include <vector>

#include "core/error.hpp"
#include "core/json.hpp"
#include "formats/open_file.hpp"
#include "formats/page_picture.hpp"
#include "render/codecs/codecs.hpp"
#include "render/codecs/limits.hpp"
#include "render/page.hpp"
#include "render/scancrop.hpp"
#include "service/automate_photos.hpp"

using namespace genko;
namespace scancrop = render::scancrop;
namespace codecs = render::codecs;

namespace {

// The texture of photo k at (u, v) of its own axes (centred).
std::array<float, 3> texture(std::size_t k, double u, double v) {
    const auto t = static_cast<float>(std::sin(u * 0.05) * std::cos(v * 0.07));
    return {0.3f + 0.2f * t, 0.2f + 0.1f * static_cast<float>(k), 0.5f - 0.2f * t};
}

// A scan: a white lid with these photos on it (normalized floats, RGB).
std::vector<float> scan(std::size_t w, std::size_t h, const std::vector<scancrop::FoundPhoto>& photos) {
    std::vector<float> px;
    px.reserve(w * h * 3);
    for (std::size_t i = 0; i < w * h; ++i) {
        px.push_back(0.97f);
        px.push_back(0.97f);
        px.push_back(0.96f);
    }
    for (std::size_t k = 0; k < photos.size(); ++k) {
        const auto& p = photos[k];
        const double rad = p.angle * std::numbers::pi / 180.0;
        const double s = std::sin(rad), c = std::cos(rad);
        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < w; ++x) {
                const double dx = static_cast<double>(x) + 0.5 - p.center[0], dy = static_cast<double>(y) + 0.5 - p.center[1];
                const double u = c * dx + s * dy, v = -s * dx + c * dy;
                if (std::abs(u) < p.width / 2.0 && std::abs(v) < p.height / 2.0) {
                    const auto t = texture(k, u, v);
                    for (std::size_t ch = 0; ch < 3; ++ch) px[(y * w + x) * 3 + ch] = t[ch];
                }
            }
        }
    }
    return px;
}

scancrop::FoundPhoto photo(double cx, double cy, double w, double h, double angle) {
    scancrop::FoundPhoto p;
    p.center = {cx, cy};
    p.width = w;
    p.height = h;
    p.angle = angle;
    return p;
}

const std::vector<scancrop::FoundPhoto> kTruth{photo(150, 120, 180, 120, 7), photo(450, 130, 160, 130, -12), photo(300, 330, 220, 110, 3)};

}  // namespace

class TestScancrop : public QObject {
    Q_OBJECT

private slots:
    void findsRotatedPhotosInReadingOrder() {
        const std::size_t w = 600, h = 440;
        const codecs::CodecImage image = codecs::CodecImage::from_normalized(w, h, codecs::Layout::Rgb, codecs::Sample::U8, scan(w, h, kTruth));
        const auto found = scancrop::find_photos(image);
        QCOMPARE(found.size(), std::size_t{3});
        for (std::size_t i = 0; i < 3; ++i) {
            const auto& f = found[i];
            const auto& t = kTruth[i];
            const QString what = QStringLiteral("photo %1: centre %2,%3 size %4×%5 angle %6").arg(i).arg(f.center[0]).arg(f.center[1]).arg(f.width).arg(f.height).arg(f.angle);
            QVERIFY2(std::abs(f.angle - t.angle) < 0.5, qPrintable(what));
            QVERIFY2(std::abs(f.center[0] - t.center[0]) < 3.0 && std::abs(f.center[1] - t.center[1]) < 3.0, qPrintable(what));
            QVERIFY2(std::abs(f.width - t.width) < 6.0 && std::abs(f.height - t.height) < 6.0, qPrintable(what));
            QVERIFY(f.area > 0.01);
        }
        // (the corners agree with the centre)
        const auto c = found[0].corners();
        QVERIFY(std::abs((c[0][0] + c[2][0]) / 2.0 - found[0].center[0]) < 1e-9);
        QVERIFY(std::abs((c[0][1] + c[2][1]) / 2.0 - found[0].center[1]) < 1e-9);
        // (none on a uniform scan, none on a tiny one)
        const std::vector<float> white(w * h * 3, 1.0f);
        QVERIFY(scancrop::find_photos(codecs::CodecImage::from_normalized(w, h, codecs::Layout::Rgb, codecs::Sample::U8, white)).empty());
        QVERIFY(scancrop::find_photos(codecs::CodecImage::blank(6, 6, codecs::Layout::Rgb, codecs::Sample::U8)).empty());
        // (a larger scan worked on small: the same photos, in the scan's pixels)
        std::vector<scancrop::FoundPhoto> big;
        for (const auto& t : kTruth) big.push_back(photo(t.center[0] * 2, t.center[1] * 2, t.width * 2, t.height * 2, t.angle));
        const auto twice = scancrop::find_photos(codecs::CodecImage::from_normalized(1200, 880, codecs::Layout::Rgb, codecs::Sample::U8, scan(1200, 880, big)));
        QCOMPARE(twice.size(), std::size_t{3});
        for (std::size_t i = 0; i < 3; ++i) {
            QVERIFY2(std::abs(twice[i].angle - big[i].angle) < 0.5, qPrintable(QString::number(twice[i].angle)));
            QVERIFY(std::abs(twice[i].width - big[i].width) < 10.0 && std::abs(twice[i].height - big[i].height) < 10.0);
        }
    }

    // Each photo cut out upright: its texture as drawn, at the scan's kind of picture.
    void cutsEachPhotoUpright() {
        const std::size_t w = 600, h = 440;
        for (const codecs::Sample sample : {codecs::Sample::U8, codecs::Sample::U16, codecs::Sample::F32}) {
            codecs::CodecImage image = codecs::CodecImage::from_normalized(w, h, codecs::Layout::Rgb, sample, scan(w, h, kTruth));
            image.icc = std::string("an icc");
            image.meta.dpi = std::make_pair(300.0, 300.0);
            const auto found = scancrop::find_photos(image);
            QCOMPARE(found.size(), std::size_t{3});
            for (std::size_t k = 0; k < 3; ++k) {
                const codecs::CodecImage cut = scancrop::cut_photo(image, found[k]);
                QCOMPARE(cut.layout(), codecs::Layout::Rgb);
                QCOMPARE(cut.sample(), sample);
                QCOMPARE(cut.icc, image.icc);
                QCOMPARE(cut.meta.dpi, image.meta.dpi);
                QCOMPARE(cut.width(), static_cast<std::uint32_t>(std::lround(found[k].width)));
                // (its middle half against the texture at the same place of the photo's axes)
                double err = 0;
                std::size_t n = 0;
                const double cw = cut.width(), ch = cut.height();
                for (std::uint32_t y = cut.height() / 4; y < cut.height() * 3 / 4; ++y) {
                    for (std::uint32_t x = cut.width() / 4; x < cut.width() * 3 / 4; ++x) {
                        const auto t = texture(k, x + 0.5 - cw / 2.0, y + 0.5 - ch / 2.0);
                        for (unsigned c = 0; c < 3; ++c) err += std::abs(cut.at(x, y, c) - t[c]);
                        n += 3;
                    }
                }
                QVERIFY2(err / static_cast<double>(n) < 0.02, qPrintable(QString::number(err / static_cast<double>(n))));
            }
        }
        // (a grey scan stays grey)
        const codecs::CodecImage rgb = codecs::CodecImage::from_normalized(w, h, codecs::Layout::Rgb, codecs::Sample::U8, scan(w, h, kTruth));
        const codecs::CodecImage grey = rgb.convert(codecs::Layout::Gray, codecs::Sample::U16);
        const auto found = scancrop::find_photos(grey);
        QCOMPARE(found.size(), std::size_t{3});
        QCOMPARE(scancrop::cut_photo(grey, found[0]).layout(), codecs::Layout::Gray);
    }

    // The service: the page's photos as books of one page, the page unchanged.
    void cropAndStraightenMakesBooks() {
        const std::size_t w = 600, h = 440;
        codecs::CodecImage image = codecs::CodecImage::from_normalized(w, h, codecs::Layout::Rgb, codecs::Sample::U16, scan(w, h, kTruth));
        image.meta.dpi = std::make_pair(300.0, 300.0);
        const formats::Opened opened = formats::picture_book(image, "スキャン");
        core::Json report;
        const std::vector<formats::Opened> books = service::automate::crop_and_straighten(opened.doc, 0, {}, &report);
        QCOMPARE(books.size(), std::size_t{3});
        QCOMPARE(report["photos"].size(), std::size_t{3});
        for (std::size_t k = 0; k < 3; ++k) {
            const core::Document& doc = books[k].doc;
            QCOMPARE(doc.title, "スキャン のコピー " + std::to_string(k + 1));
            QCOMPARE(doc.pages.size(), std::size_t{1});
            const core::Page& page = doc.page(0);
            QVERIFY(page.rgb.has_value());
            QCOMPARE(page.rgb->depth, 16);
            QCOMPARE(page.spec.dpi.value(), 300.0);
            QVERIFY(!doc.spec.grid.has_value());
            const formats::PagePicture pic = formats::page_picture(page, doc, 300);
            QCOMPARE(pic.image.width(), report["photos"][k]["width"].get<std::uint32_t>());
            QVERIFY(std::abs(report["photos"][k]["angle"].get<double>() - kTruth[k].angle) < 0.5);
        }
        // (none found: none made; past kMaxPhotos: refused, none made)
        const formats::Opened blank = formats::picture_book(codecs::CodecImage::from_normalized(w, h, codecs::Layout::Rgb, codecs::Sample::U8, std::vector<float>(w * h * 3, 1.0f)), "白");
        QVERIFY(service::automate::crop_and_straighten(blank.doc, 0, {}).empty());
        std::vector<scancrop::FoundPhoto> many;
        for (int r = 0; r < 5; ++r)
            for (int c = 0; c < 7; ++c) many.push_back(photo(100 + c * 200, 100 + r * 200, 140, 140, 0));
        const formats::Opened crowded = formats::picture_book(codecs::CodecImage::from_normalized(1400, 1000, codecs::Layout::Rgb, codecs::Sample::U8, scan(1400, 1000, many)), "多い");
        QVERIFY(many.size() > service::automate::kMaxPhotos);
        QVERIFY_THROWS_EXCEPTION(core::Error, service::automate::crop_and_straighten(crowded.doc, 0, {}));
    }

    // The review's P2-2, P3-5, P2-5: what cutting the photos out holds is seen before any is cut (past the budget:
    // image_too_large); a photo whose corners cannot be mapped is that one's refusal (the service leaves it out); a stop
    // stops it.
    void theCutsAreCountedAndStopped() {
        const codecs::CodecImage lid = codecs::CodecImage::blank(100, 100, codecs::Layout::Rgb, codecs::Sample::U16);
        const std::uint64_t small = service::automate::crop_work_bytes(lid, {photo(50, 50, 40, 30, 5)});
        QVERIFY(small > lid.data().size() && small < 1u << 20);
        // (a 16-bit scan of 8000 × 8000 px wholly one photo, as the books it makes are counted)
        const std::vector<scancrop::FoundPhoto> whole{photo(4000, 4000, 7990, 7990, 0)};
        QVERIFY(service::automate::crop_work_bytes(lid, whole) > codecs::kAllocBudget);
        scancrop::FoundPhoto flat = photo(50, 50, 0, 0, 0);
        QVERIFY_THROWS_EXCEPTION(scancrop::Unmappable, scancrop::cut_photo(lid, flat));
        const std::size_t w = 600, h = 440;
        const formats::Opened opened = formats::picture_book(codecs::CodecImage::from_normalized(w, h, codecs::Layout::Rgb, codecs::Sample::U8, scan(w, h, kTruth)), "s");
        QVERIFY_THROWS_EXCEPTION(render::Cancelled, service::automate::crop_and_straighten(opened.doc, 0, {}, nullptr, [] { return true; }));
        QVERIFY_THROWS_EXCEPTION(render::Cancelled, scancrop::find_photos(codecs::CodecImage::blank(100, 100, codecs::Layout::Rgb, codecs::Sample::U8), [] { return true; }));
    }
};

QTEST_GUILESS_MAIN(TestScancrop)
#include "test_scancrop.moc"
