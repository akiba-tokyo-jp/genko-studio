// fukidashi@1 (SPEC BALLOON-01, AC-BALLOON): the balloons drawn clean.
//
//   - the geometry (render/text/balloon_clean): an outline is its own mirror image exactly; the line has its width at
//     any angle; dashes are of one length with no short one where the outline closes; a tail leaves the balloon from
//     its outline;
//   - drawn: a tile is the whole page cut there; the line of a 600 dpi page made black and white is its width; a
//     balloon on a layer of its own laid over the page is the page (no dark edge); a solid black balloon with white
//     letters; a tone box; the keys of before (hand, wobble) taken and drawn clean;
//   - the book: the feature named while a new kind, key or tail is used; a book of before read — its hand-drawn ovals
//     grown so that the letters keep the size they had (tests/data/fukidashi: the book main 5cf6551a wrote and the size
//     of its letters there) —, saved and read again without growing twice; a book of now saved and read back the same.
//
// GENKO_FUKIDASHI_OUT=<folder> also writes the sample sheets there (the pictures a person looks at).

#include <QtTest>

#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "core/fukidashi.hpp"
#include "core/model.hpp"
#include "formats/pillow_save.hpp"
#include "fukidashi_books.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/text/balloon_clean.hpp"
#include "render/text/balloons.hpp"
#include "render/text/lettering.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"
#include "storage/reader.hpp"
#include "storage/writer.hpp"
#include "testsupport.hpp"

using genko::core::Document;
using genko::core::Json;
using genko::render::Image;
namespace clean = genko::render::text::clean;

namespace {

Document run_ops(const Document& doc, const Json& ops) { return genko::test::classic_bus().apply(doc, ops, genko::core::Actor("human:作者")).doc; }

Document blank_book() { return genko::core::new_episode("フキダシ", genko::core::Num(1), 1, genko::core::PageSpec::a4_mono()); }

// A book with one line (id L1) of `kind` at (40, 40), 50 × 40 mm, with `more` of the op over it.
Document one_line(const char* kind, Json more = Json::object(), const char* text = "今日は\nいい天気") {
    Json op{{"op", "add_line"}, {"page", 1}, {"id", "L1"}, {"balloon", kind}, {"x_mm", 40}, {"y_mm", 40}, {"w_mm", 50}, {"h_mm", 40}, {"text", text}};
    for (const auto& [key, value] : more.items()) op[key] = value;
    return run_ops(blank_book(), Json::array({op}));
}

Image page_picture(const Document& doc, int dpi, std::size_t page = 0) {
    genko::render::text::clear_balloon_cache();
    return genko::render::render_page(doc.page(page), dpi, {}, &doc).image;
}

// A part of the page (mm) at `dpi`.
Image page_part(const Document& doc, int dpi, double x, double y, double w, double h, std::size_t page = 0) {
    genko::render::RenderOptions options;
    const auto px = [dpi](double mm) { return static_cast<int>(std::lround(mm / 25.4 * dpi)); };
    options.region = genko::render::RenderRegion{px(x), px(y), px(w), px(h)};
    return genko::render::render_page(doc.page(page), dpi, options, &doc).image;
}

// The first band of a picture as numbers, row by row.
struct Grey {
    int w = 0, h = 0;
    std::string v;
    int at(int x, int y) const { return static_cast<unsigned char>(v[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)]); }
};
Grey grey_of(const Image& image) { return Grey{image.width(), image.height(), image.convert("L").tobytes()}; }

double plane_at(const clean::Plane& plane, double x, double y) {  // (between the pixels' middles)
    const double fx = x - 0.5, fy = y - 0.5;
    const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
    const double tx = fx - x0, ty = fy - y0;
    const auto v = [&](int px, int py) {
        if (px < 0 || py < 0 || px >= plane.width || py >= plane.height) return 0.0;
        return static_cast<double>(plane.at(px, py));
    };
    return (v(x0, y0) * (1 - tx) + v(x0 + 1, y0) * tx) * (1 - ty) + (v(x0, y0 + 1) * (1 - tx) + v(x0 + 1, y0 + 1) * tx) * ty;
}

std::filesystem::path old_book_dir() { return std::filesystem::path(genko::test::test_data("fukidashi/old_book").toStdString()); }

Json payload_of(const Document& doc) {
    QTemporaryDir dir;
    genko::storage::AssetStore store(std::filesystem::path(dir.path().toStdString()));
    return genko::storage::project_payload_v4(doc, store);
}

bool names_feature(const Json& payload) {
    const Json& features = payload["features"];
    return std::find(features.begin(), features.end(), Json(std::string(genko::core::kFukidashiFeature))) != features.end();
}

// The box of the ink (alpha > 40) of a lettering.
std::array<int, 2> ink_size(const Image& image) {
    const std::string alpha = image.getchannel(image.bands() - 1).tobytes();
    int x0 = image.width(), y0 = image.height(), x1 = 0, y1 = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (static_cast<unsigned char>(alpha[static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width()) + static_cast<std::size_t>(x)]) <= 40) continue;
            x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x + 1), y1 = std::max(y1, y + 1);
        }
    }
    return {std::max(0, x1 - x0), std::max(0, y1 - y0)};
}

}  // namespace

class TestFukidashi : public QObject {
    Q_OBJECT

private slots:
    // --- the geometry --------------------------------------------------------------------------------------------------

    // An outline about its middle is its own mirror image left to right, every point, bit for bit; the round ones top to
    // bottom too.
    void outlinesAreTheirOwnMirrorImage() {
        const clean::Box box{-211.5, -160.25, 211.5, 160.25};
        struct Case {
            const char* name;
            clean::Contour outline;
            bool both;
        };
        const std::vector<Case> cases = {
            {"ellipse", clean::ellipse(box), true},
            {"rounded", clean::rounded(box, 48.0), true},
            {"capsule", clean::rounded(box, 160.25), true},
            {"box", clean::rounded(box, 0.0), true},
            {"squircle", clean::squircle(box), true},
            {"octagon", clean::polygon(box, 8, 20.0), false},
            {"hexagon", clean::polygon(box, 6, 20.0), false},
            {"pentagon", clean::polygon(box, 5, 20.0), false},
            {"cloud 10", clean::cloud(box, 10), false},
            {"cloud 11", clean::cloud(box, 11), false},
            {"burst 14", clean::burst(box, 14, 0.2, 0.0, {}), false},
            {"burst 15", clean::burst(box, 15, 0.2, 0.0, {}), false},
            {"burst long and short", clean::burst(box, 16, 0.25, 0.5, {}), false},
        };
        for (const Case& c : cases) {
            const std::size_t n = c.outline.size();
            QVERIFY2(n >= 4, c.name);
            // (it begins at its top, on the middle line; point i mirrors point n − i)
            QVERIFY2(c.outline[0].x == 0.0, c.name);
            for (std::size_t i = 1; i < n; ++i) {
                const clean::P a = c.outline[i];
                const clean::P b = c.outline[n - i];
                QVERIFY2(a.x == -b.x && a.y == b.y, qPrintable(QStringLiteral("%1: point %2").arg(c.name).arg(i)));
            }
            if (!c.both) continue;
            QVERIFY2(n % 4 == 0, c.name);
            for (std::size_t i = 0; i <= n / 2; ++i) {  // (top to bottom: point i mirrors point n / 2 − i)
                const clean::P a = c.outline[i];
                const clean::P b = c.outline[n / 2 - i];
                QVERIFY2(a.x == b.x && a.y == -b.y, qPrintable(QStringLiteral("%1: point %2 (top to bottom)").arg(c.name).arg(i)));
            }
            // (and it touches its box's four sides)
            double x1 = 0, y1 = 0;
            for (const clean::P& p : c.outline) x1 = std::max(x1, p.x), y1 = std::max(y1, p.y);
            QCOMPARE(x1, 211.5);
            QCOMPARE(y1, 160.25);
        }
        // (the coverage of a mirrored outline is mirrored: within one level of 255, the rasterizer's sums in another order)
        const clean::Plane fill = clean::fill_plane({clean::whole(clean::ellipse(clean::Box{20, 20, 380, 300}))}, 400, 320);
        for (int y = 0; y < 320; ++y) {
            for (int x = 0; x < 200; ++x) QVERIFY(std::fabs(fill.at(x, y) - fill.at(399 - x, y)) < 1.0 / 255);
        }
    }

    // The line of a balloon is as wide as asked, whatever way its edge runs: the coverage summed across it at 0°, 30°,
    // 45° and 90° is the width, within half a pixel — on a circle and on an ellipse.
    void theLineHasItsWidthAtAnyAngle_data() {
        QTest::addColumn<double>("rx");
        QTest::addColumn<double>("ry");
        QTest::addColumn<double>("width");
        QTest::newRow("circle, 8 px") << 220.0 << 220.0 << 8.0;
        QTest::newRow("ellipse, 8 px") << 260.0 << 170.0 << 8.0;
        QTest::newRow("ellipse, 3 px") << 130.0 << 90.0 << 3.0;
        QTest::newRow("ellipse, 2 px") << 61.5 << 40.0 << 2.0;
    }
    void theLineHasItsWidthAtAnyAngle() {
        QFETCH(double, rx);
        QFETCH(double, ry);
        QFETCH(double, width);
        const int w = 600, h = 520;
        const clean::P centre{300.0, 260.0};
        const std::vector<clean::Shape> shapes{clean::whole(clean::ellipse(clean::Box{centre.x - rx, centre.y - ry, centre.x + rx, centre.y + ry}))};
        const clean::Plane fill = clean::fill_plane(shapes, w, h);
        clean::Plane band = clean::ribbon_plane(clean::edge_runs(shapes), width, w, h);
        for (std::size_t i = 0; i < band.values.size(); ++i) band.values[i] *= fill.values[i];
        for (const double degrees : {0.0, 30.0, 45.0, 90.0, 135.0, 210.0, 300.0}) {
            const double a = degrees * 3.141592653589793 / 180;
            // (the edge's point at that angle of the ellipse, and the way across the line there: its normal)
            const clean::P on{centre.x + rx * std::cos(a), centre.y + ry * std::sin(a)};
            double nx = std::cos(a) / rx, ny = std::sin(a) / ry;
            const double n = std::hypot(nx, ny);
            nx /= n, ny /= n;
            double sum = 0.0;
            const double step = 0.05;
            for (double d = -width - 3; d <= 3; d += step) sum += plane_at(band, on.x + nx * d, on.y + ny * d) * step;
            QVERIFY2(std::fabs(sum - width) <= 0.5, qPrintable(QStringLiteral("%1°: %2 px (asked %3)").arg(degrees).arg(sum).arg(width)));
        }
    }

    // Dashes: a whole number of them round the outline, each as long as the others within a pixel, none cut short where
    // the outline closes.
    void dashesAreOfOneLength() {
        for (const clean::Contour& outline : {clean::ellipse(clean::Box{10, 10, 510, 390}), clean::rounded(clean::Box{10, 10, 510, 390}, 60.0),
                                             clean::rounded(clean::Box{10, 10, 170, 390}, 80.0)}) {
            const double on = 26.0, off = 15.6;
            const std::vector<clean::Run> runs = clean::edge_runs(clean::dashed({clean::whole(outline)}, on, off));
            const double total = clean::lengths_of(outline).back();
            const double count = std::floor(total / (on + off) + 0.5);
            QCOMPARE(static_cast<double>(runs.size()), count);
            double shortest = 1e9, longest = 0.0;
            for (const clean::Run& run : runs) {
                QVERIFY(!run.closed);
                double length = 0.0;
                for (std::size_t i = 0; i + 1 < run.points.size(); ++i) length += clean::distance(run.points[i], run.points[i + 1]);
                shortest = std::min(shortest, length);
                longest = std::max(longest, length);
            }
            QVERIFY2(longest - shortest <= 1.0, qPrintable(QStringLiteral("%1 .. %2").arg(shortest).arg(longest)));
            // (the dash is its share of the period the outline's length gives: no remainder left for a seam)
            QVERIFY(std::fabs(longest - total / count * on / (on + off)) <= 1.0);
        }
    }

    // A tail leaves the balloon from its outline: the two ends of its base lie on it, the base as wide as asked along it;
    // rounded, the join's curve begins on the outline too.
    void aTailLeavesFromTheOutline() {
        const clean::Box box{100, 100, 500, 400};
        const clean::P centre{300, 250};
        for (const clean::Contour& outline : {clean::ellipse(box), clean::rounded(box, 90.0), clean::rounded(box, 0.0)}) {
            const std::vector<double> lengths = clean::lengths_of(outline);
            const auto off_outline = [&](clean::P p) {
                double best = 1e9;
                for (std::size_t i = 0; i < outline.size(); ++i) {
                    const clean::P a = outline[i], b = outline[(i + 1) % outline.size()];
                    const double dx = b.x - a.x, dy = b.y - a.y;
                    const double t = std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / std::max(1e-12, dx * dx + dy * dy), 0.0, 1.0);
                    best = std::min(best, std::hypot(p.x - (a.x + dx * t), p.y - (a.y + dy * t)));
                }
                return best;
            };
            for (const clean::P tip : {clean::P{620, 520}, clean::P{300, 20}, clean::P{-40, 300}, clean::P{560, 250}}) {
                clean::TailSpec spec;
                spec.tip = tip;
                spec.base = 90.0;
                spec.fillet = 24.0;
                spec.depth = 40.0;
                const clean::Tail tail = clean::tail(outline, centre, spec);
                QVERIFY(off_outline(tail.left) < 1e-6);
                QVERIFY(off_outline(tail.right) < 1e-6);
                // (the base's ends are the tail's own points: its sides begin there)
                const auto has = [&](clean::P p) {
                    return std::any_of(tail.shape.points.begin(), tail.shape.points.end(), [&](clean::P q) { return std::hypot(p.x - q.x, p.y - q.y) < 1e-9; });
                };
                QVERIFY(has(tail.left) && has(tail.right));
                // (as wide as asked, along the outline: the way from one end to the other past where the tail faces)
                const double at = clean::facing(outline, lengths, centre, tip);
                QVERIFY(std::hypot(clean::point_at(outline, lengths, at - 45).x - tail.left.x, clean::point_at(outline, lengths, at - 45).y - tail.left.y) < 1e-6);
                QVERIFY(std::hypot(clean::point_at(outline, lengths, at + 45).x - tail.right.x, clean::point_at(outline, lengths, at + 45).y - tail.right.y) < 1e-6);
                // (two joins, each a curve from the outline to the tail's side; only the curve is part of the line)
                QCOMPARE(tail.joins.size(), std::size_t{2});
                for (const clean::Shape& join : tail.joins) {
                    std::size_t first = join.stroke.size(), last = 0;
                    for (std::size_t i = 0; i < join.stroke.size(); ++i) {
                        if (!join.stroke[i]) continue;
                        first = std::min(first, i);
                        last = std::max(last, i + 1);
                    }
                    QVERIFY(first < last);
                    QVERIFY(off_outline(join.points[first]) < 1e-6);
                    QVERIFY(off_outline(join.points[last % join.points.size()]) > 1.0);
                }
            }
        }
    }

    // The join has no corner: along the edge of a balloon with a tail, the line's inner side turns nowhere by more than
    // a rounded join's share — no pixel of the page between the line and the fill is left white inside the notch, and
    // the notch's corner pixel itself (where the outline and a tail without a join meet) is inside the fill.
    void aRoundedJoinFillsTheNotch() {
        const clean::Box box{100, 100, 500, 400};
        const clean::Contour outline = clean::ellipse(box);
        clean::TailSpec spec;
        spec.tip = {600, 520};
        spec.base = 100.0;
        spec.depth = 50.0;
        const clean::Tail sharp = clean::tail(outline, {300, 250}, spec);
        spec.fillet = 30.0;
        const clean::Tail round = clean::tail(outline, {300, 250}, spec);
        const auto covered = [&](const clean::Tail& tail) {
            std::vector<clean::Shape> shapes{clean::whole(outline), tail.shape};
            for (const clean::Shape& join : tail.joins) shapes.push_back(join);
            const clean::Plane fill = clean::fill_plane(shapes, 700, 600);
            double sum = 0.0;
            for (const float v : fill.values) sum += v;
            return sum;
        };
        // (the joins add a little to the union — the two notches — and nothing else)
        const double more = covered(round) - covered(sharp);
        QVERIFY2(more > 20.0 && more < 2 * 30.0 * 30.0, qPrintable(QString::number(more)));
        // (and the line runs through the join without a break: one run round the whole group)
        std::vector<clean::Shape> shapes{clean::whole(outline), round.shape};
        for (const clean::Shape& join : round.joins) shapes.push_back(join);
        const clean::Plane band = clean::ribbon_plane(clean::edge_runs(shapes), 8.0, 700, 600);
        const clean::Plane fill = clean::fill_plane(shapes, 700, 600);
        // (walking just inside the union's edge near a join, the line is there all the way)
        for (const clean::Shape& join : round.joins) {
            for (std::size_t i = 0; i < join.points.size(); ++i) {
                if (!join.stroke[i]) continue;
                const clean::P p = join.points[i];
                QVERIFY(plane_at(band, p.x, p.y) > 0.99);
                QVERIFY(plane_at(fill, p.x, p.y) > 0.2);
            }
        }
    }

    // --- drawn ---------------------------------------------------------------------------------------------------------

    // A part of the page is the whole page cut there, every pixel: the sample of now at 600 dpi in tiles, and at 150.
    void aTileIsThePageCutThere_data() {
        QTest::addColumn<int>("dpi");
        QTest::addColumn<int>("page");
        QTest::newRow("600 dpi, the kinds") << 600 << 0;
        QTest::newRow("600 dpi, the tails") << 600 << 1;
        QTest::newRow("150 dpi, the tails") << 150 << 1;
    }
    void aTileIsThePageCutThere() {
        QFETCH(int, dpi);
        QFETCH(int, page);
        const Document doc = genko::test::clean_book();
        const Image whole = page_picture(doc, dpi, static_cast<std::size_t>(page));
        const int side = dpi == 600 ? 1024 : 300;
        for (int y = 0; y < whole.height(); y += side) {
            for (int x = 0; x < whole.width(); x += side) {
                genko::render::text::clear_balloon_cache();  // (each tile works its balloons out again)
                genko::render::RenderOptions options;
                options.region = genko::render::RenderRegion{x, y, std::min(side, whole.width() - x), std::min(side, whole.height() - y)};
                const Image tile = genko::render::render_page(doc.page(static_cast<std::size_t>(page)), dpi, options, &doc).image;
                const Image cut = whole.crop(genko::render::Box{x, y, x + options.region->w, y + options.region->h});
                QVERIFY2(tile.tobytes() == cut.tobytes(), qPrintable(QStringLiteral("tile at %1, %2").arg(x).arg(y)));
            }
        }
    }

    // A 600 dpi page made black and white (the monochrome print: grey above 180 is white): the line of an oval is its
    // width — 0.35 mm, 8 px — within a pixel, across the top, the side and the diagonal.
    void thePrintedLineIsItsWidth() {
        const Document doc = one_line("speech", Json{{"text", "あ"}});
        const Image mono = genko::render::to_bitonal(page_picture(doc, 600));
        const Grey g = grey_of(mono);
        const int x0 = static_cast<int>(std::lround(40 / 25.4 * 600)), y0 = static_cast<int>(std::lround(40 / 25.4 * 600));
        const int x1 = static_cast<int>(std::lround(90 / 25.4 * 600)), y1 = static_cast<int>(std::lround(80 / 25.4 * 600));
        const int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
        const auto run = [&](int sx, int sy, int dx, int dy) {  // (the black pixels met first, walking in from outside)
            int x = sx, y = sy, n = 0;
            while (g.at(x, y) >= 128) x += dx, y += dy;
            while (g.at(x, y) < 128) x += dx, y += dy, ++n;
            return n;
        };
        const int top = run(cx, y0 - 20, 0, 1);
        const int left = run(x0 - 20, cy, 1, 0);
        const int foot = run(cx, y1 + 20, 0, -1);
        QVERIFY2(std::abs(top - 8) <= 1 && std::abs(left - 8) <= 1 && std::abs(foot - 8) <= 1,
                 qPrintable(QStringLiteral("%1 %2 %3").arg(top).arg(left).arg(foot)));
        // (the diagonal: from the box's corner toward its middle, each step √2 of the way across... the ellipse's edge
        // there is not at 45° for a box that is not square, so the steps are measured across it)
        const double rx = (x1 - x0) / 2.0, ry = (y1 - y0) / 2.0;
        const double nx = (1 / std::sqrt(2.0)) / rx, ny = (1 / std::sqrt(2.0)) / ry;  // (the normal at 45° of the ellipse's angle)
        const double len = std::hypot(nx, ny);
        int black = 0;
        const double ex = cx + rx / std::sqrt(2.0), ey = cy + ry / std::sqrt(2.0);
        for (double d = -14; d <= 6; d += 0.25) {
            const int x = static_cast<int>(std::floor(ex + nx / len * d)), y = static_cast<int>(std::floor(ey + ny / len * d));
            if (g.at(x, y) < 128) ++black;
        }
        QVERIFY2(std::fabs(black * 0.25 - 8) <= 1.0, qPrintable(QString::number(black * 0.25)));
    }

    // A balloon on a layer of its own (the editable PSD's balloon layer: drawn on nothing) laid over the page is the
    // balloon drawn on the page: within a level and a half of 255 (three roundings), on white and on a dark page — and a white
    // balloon without a line on a white page leaves it white, every pixel (no dark edge from the layer's transparency).
    void aBalloonLayerLaidOverThePageIsThePage_data() {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<int>("dpi");
        for (const char* kind : {"speech", "rounded", "capsule", "polygon", "cloud", "shout", "electric", "whisper", "flash", "tone_box", "fancy_box"}) {
            for (const int dpi : {150, 600}) QTest::newRow(qPrintable(QStringLiteral("%1 at %2").arg(kind).arg(dpi))) << QString::fromLatin1(kind) << dpi;
        }
    }
    void aBalloonLayerLaidOverThePageIsThePage() {
        QFETCH(QString, kind);
        QFETCH(int, dpi);
        const std::string k = kind.toStdString();
        const Document doc = one_line(k.c_str(), Json{{"tail", Json::array({110, 100})}});
        const genko::core::StoryLine& line = doc.story.front();
        const genko::render::Size size{static_cast<int>(140 / 25.4 * dpi), static_cast<int>(120 / 25.4 * dpi)};
        const genko::render::text::LetterParts balloon_only{true, false, false, false};
        for (const int page_grey : {255, 60}) {
            Image page = Image::create("RGB", size, genko::render::Ink{page_grey, page_grey, page_grey});
            genko::render::text::clear_balloon_cache();
            genko::render::text::draw_lines(genko::render::text::whole_page(page), {&line}, dpi, std::nullopt, false, nullptr, {}, nullptr, &balloon_only);
            Image layer = Image::create("RGBA", size, genko::render::Ink{0, 0, 0, 0});
            genko::render::text::draw_lines(genko::render::text::whole_page(layer), {&line}, dpi, std::nullopt, false, nullptr, {}, nullptr, &balloon_only);
            const std::string direct = page.tobytes();
            const std::string over = layer.tobytes();
            double worst = 0.0;
            for (std::size_t i = 0; i < direct.size() / 3; ++i) {
                const double a = static_cast<unsigned char>(over[i * 4 + 3]) / 255.0;
                for (std::size_t c = 0; c < 3; ++c) {
                    const double laid = static_cast<unsigned char>(over[i * 4 + c]) * a + page_grey * (1 - a);
                    worst = std::max(worst, std::fabs(laid - static_cast<unsigned char>(direct[i * 3 + c])));
                }
            }
            // (the layer's colour, its alpha and the page's own pixel each rounded to a level: 1.5 at the very most)
            QVERIFY2(worst <= 1.5, qPrintable(QStringLiteral("on %1: %2 levels apart").arg(page_grey).arg(worst)));
        }
        // (no line, white on white: the layer's edge pixels are white where they are anything)
        const Document plain = one_line(k.c_str(), Json{{"style", Json{{"border_mm", 0}}}});
        Image layer = Image::create("RGBA", size, genko::render::Ink{0, 0, 0, 0});
        genko::render::text::clear_balloon_cache();
        genko::render::text::draw_lines(genko::render::text::whole_page(layer), {&plain.story.front()}, dpi, std::nullopt, false, nullptr, {}, nullptr, &balloon_only);
        const std::string bytes = layer.tobytes();
        if (k != "tone_box") {
            for (std::size_t i = 0; i < bytes.size() / 4; ++i) {
                if (static_cast<unsigned char>(bytes[i * 4 + 3]) == 0) continue;
                QVERIFY(static_cast<unsigned char>(bytes[i * 4]) == 255 && static_cast<unsigned char>(bytes[i * 4 + 1]) == 255 && static_cast<unsigned char>(bytes[i * 4 + 2]) == 255);
            }
        }
    }

    // ベタのフキダシに白抜き文字: fill_rgb and line_rgb black, the letters white — an oval, a burst and a flash: the balloon
    // is black through and through, the letters white inside it.
    void aSolidBlackBalloonWithWhiteLetters_data() {
        QTest::addColumn<QString>("kind");
        for (const char* kind : {"speech", "shout", "flash", "capsule", "rounded"}) QTest::newRow(kind) << QString::fromLatin1(kind);
    }
    void aSolidBlackBalloonWithWhiteLetters() {
        QFETCH(QString, kind);
        const Json black{{"fill_rgb", Json::array({0, 0, 0})}, {"line_rgb", Json::array({0, 0, 0})}, {"rgb", Json::array({255, 255, 255})}};
        const Document doc = one_line(kind.toStdString().c_str(), Json{{"style", black}}, "白抜き\nの文字");
        for (const int dpi : {150, 600}) {
            const Grey g = grey_of(page_part(doc, dpi, 40, 40, 50, 40));
            // (the balloon's middle part — what an ellipse of 0.6 of its box holds: black but for the letters, which are white)
            int black_px = 0, white_px = 0, other = 0, total = 0;
            for (int y = 0; y < g.h; ++y) {
                for (int x = 0; x < g.w; ++x) {
                    const double ux = (x + 0.5 - g.w / 2.0) / (g.w / 2.0), uy = (y + 0.5 - g.h / 2.0) / (g.h / 2.0);
                    if (ux * ux + uy * uy > 0.36) continue;
                    ++total;
                    const int v = g.at(x, y);
                    if (v < 40) ++black_px; else if (v > 215) ++white_px; else ++other;
                }
            }
            QVERIFY2(black_px > total * 0.6, qPrintable(QStringLiteral("%1 of %2 black").arg(black_px).arg(total)));
            QVERIFY2(white_px > total * 0.03, qPrintable(QStringLiteral("%1 of %2 white").arg(white_px).arg(total)));
            QVERIFY2(other < total * 0.12, qPrintable(QStringLiteral("%1 of %2 grey").arg(other).arg(total)));
            // (and its edge against the paper has no white ring inside it: walking in from the left at the middle row, after
            // the first black pixel nothing is light until the letters)
            const int row = g.h / 2;
            int x = 0;
            while (x < g.w && g.at(x, row) > 128) ++x;
            for (int k = 0; k < g.w / 8 && x + k < g.w; ++k) QVERIFY(g.at(x + k, row) < 128);
        }
    }

    // A tone box keeps its tone: dots inside the frame, about a tenth of it dark, lined up with the page (the same dots
    // in a tile), under the letters.
    void aToneBoxKeepsItsTone() {
        const Document doc = one_line("tone_box", Json::object(), "あ");
        for (const int dpi : {150, 600}) {
            const Grey g = grey_of(page_part(doc, dpi, 44, 44, 14, 32));  // (inside the frame, clear of the letters in its middle)
            double dark = 0;  // (how much of it the dots cover: their ink, a small dot's soft edge counted by its share)
            for (int i = 0; i < g.w * g.h; ++i) dark += (255 - static_cast<unsigned char>(g.v[static_cast<std::size_t>(i)])) / 255.0;
            const double share = dark / (g.w * g.h);
            QVERIFY2(share > 0.04 && share < 0.16, qPrintable(QString::number(share)));
        }
        // (a plain box has none)
        const Grey plain = grey_of(page_part(one_line("box", Json::object(), "あ"), 600, 44, 44, 14, 32));
        QVERIFY(std::all_of(plain.v.begin(), plain.v.end(), [](char c) { return static_cast<unsigned char>(c) > 200; }));
    }

    // The keys of before: `hand` is taken and changes nothing drawn; `wobble` is drawn as the trembling outline, the same
    // as the wave_mm it stands for, the same each time (nothing random); spike_jitter stays, by the line's id.
    void theKeysOfBeforeAreDrawnClean() {
        const Image plain = page_part(one_line("speech"), 300, 30, 30, 70, 60);
        QVERIFY(page_part(one_line("speech", Json{{"style", Json{{"hand", true}}}}), 300, 30, 30, 70, 60).tobytes() == plain.tobytes());
        QVERIFY(page_part(one_line("speech", Json{{"style", Json{{"hand", false}}}}), 300, 30, 30, 70, 60).tobytes() == plain.tobytes());
        const Image wobbled = page_part(one_line("speech", Json{{"style", Json{{"wobble", 0.5}}}}), 300, 30, 30, 70, 60);
        QVERIFY(wobbled.tobytes() != plain.tobytes());
        genko::render::text::clear_balloon_cache();
        QVERIFY(page_part(one_line("speech", Json{{"style", Json{{"wobble", 0.5}}}}), 300, 30, 30, 70, 60).tobytes() == wobbled.tobytes());
        // (wobble w on a box whose narrow side is 40 mm: a wave of kWobbleWave × w × 40 mm)
        const double wave = genko::render::text::balloon::kWobbleWave * 0.5 * 40;
        QVERIFY(page_part(one_line("speech", Json{{"style", Json{{"wave_mm", wave}}}}), 300, 30, 30, 70, 60).tobytes() == wobbled.tobytes());
        // (a burst's points: even without jitter, each its own reach with it, the same for the same line)
        const Image burst = page_part(one_line("shout"), 300, 30, 30, 70, 60);
        const Image jittered = page_part(one_line("shout", Json{{"style", Json{{"spike_jitter", 0.6}}}}), 300, 30, 30, 70, 60);
        QVERIFY(burst.tobytes() != jittered.tobytes());
        genko::render::text::clear_balloon_cache();
        QVERIFY(page_part(one_line("shout", Json{{"style", Json{{"spike_jitter", 0.6}}}}), 300, 30, 30, 70, 60).tobytes() == jittered.tobytes());
        // (without it a burst is its own mirror image on the page: its box's middle column to the left as to the right)
        const Document alone = one_line("shout", Json{{"text", "あ"}, {"x_mm", 42.333}, {"w_mm", 50.8}});  // (500 and 600 px: 1100 px wide at 300 dpi)
        const Grey g = grey_of(page_part(alone, 300, 42.333 - 8.467, 30, 50.8 + 2 * 8.467, 60));
        int unlike = 0;
        for (int y = 0; y < g.h / 3; ++y) {  // (above the letter)
            for (int x = 0; x < g.w / 2; ++x) unlike += std::abs(g.at(x, y) - g.at(g.w - 1 - x, y)) > 1 ? 1 : 0;
        }
        QCOMPARE(unlike, 0);
    }

    // --- the book ------------------------------------------------------------------------------------------------------

    // fukidashi@1 is named while a line uses a kind, a style key or a tail a build of before cannot draw, and not
    // otherwise (hand and wobble are of before: no feature).
    void theFeatureIsNamedWhileUsed() {
        QVERIFY(!names_feature(payload_of(one_line("speech"))));
        QVERIFY(!names_feature(payload_of(one_line("speech", Json{{"style", Json{{"hand", false}, {"wobble", 0.3}, {"double", true}}}}))));
        QVERIFY(names_feature(payload_of(one_line("capsule"))));
        QVERIFY(names_feature(payload_of(one_line("polygon"))));
        for (const Json& style : {Json{{"corner_mm", 3}}, Json{{"squircle", true}}, Json{{"sides", 6}}, Json{{"wave_mm", 0.4}}, Json{{"spike_alt", 0.5}}, Json{{"dash_mm", 2}}}) {
            const char* kind = style.contains("sides") ? "polygon" : style.contains("spike_alt") ? "shout" : "rounded";
            QVERIFY2(names_feature(payload_of(one_line(kind, Json{{"style", style}}))), style.dump().c_str());
        }
        QVERIFY(names_feature(payload_of(one_line("speech", Json{{"tails", Json::array({Json{{"to", Json::array({100, 100})}, {"kind", "inward"}}})}}))));
        QVERIFY(names_feature(payload_of(one_line("speech", Json{{"tails", Json::array({Json{{"to", Json::array({100, 100})}, {"fillet_mm", 2}}})}}))));
        // (taken away again, the feature goes)
        Document doc = one_line("capsule");
        doc = run_ops(doc, Json::array({Json{{"op", "edit_line"}, {"id", "L1"}, {"balloon", "speech"}}}));
        QVERIFY(!names_feature(payload_of(doc)));
        doc = run_ops(doc, Json::array({Json{{"op", "edit_line"}, {"id", "L1"}, {"style", Json{{"wave_mm", 0.5}}}}}));
        QVERIFY(names_feature(payload_of(doc)));
        doc = run_ops(doc, Json::array({Json{{"op", "edit_line"}, {"id", "L1"}, {"style", Json{{"wave_mm", nullptr}}}}}));
        QVERIFY(!names_feature(payload_of(doc)));
        // (a book that names it is read by this build, and written back with it)
        QTemporaryDir dir;
        const Document with = one_line("capsule", Json{{"style", Json{{"wave_mm", 0.3}}}});
        genko::test::write_project(with, std::filesystem::path(dir.path().toStdString()));
        const genko::storage::LoadResult read = genko::storage::load_document(std::filesystem::path(dir.path().toStdString()));
        QVERIFY2(read.document.read_only_reason.empty(), read.document.read_only_reason.c_str());
        QVERIFY(names_feature(payload_of(read.document)));
        QCOMPARE(read.document.story.front().balloon, std::string("capsule"));
    }

    // A book of before (no "balloon_geometry"): read, each hand-drawn oval's box is 1.035 times as big about its middle
    // — and nothing else of any line changes.
    void aBookOfBeforeGrowsItsOvals() {
        const Json before = Json::parse(genko::test::read_bytes(QString::fromStdString((old_book_dir() / "project.json").string())));
        QVERIFY(!before.contains("balloon_geometry"));
        const genko::storage::LoadResult read = genko::storage::load_document(old_book_dir());
        QVERIFY2(read.document.read_only_reason.empty(), read.document.read_only_reason.c_str());
        const Document& doc = read.document;
        QCOMPARE(doc.story.size(), before["story"].size());
        int grown = 0;
        for (std::size_t i = 0; i < doc.story.size(); ++i) {
            const genko::core::StoryLine& line = doc.story[i];
            const Json& was = before["story"][i];
            const std::string kind = was.value("balloon", std::string("speech"));
            const Json style = was.value("style", Json::object());
            const bool oval = kind == "speech" || kind == "thought" || kind == "whisper";
            const bool has_path = was.contains("path") && !was["path"].is_null();
            const bool by_hand = oval && style.value("hand", true) && style.value("wobble", 0.0) == 0.0 && !has_path;
            const double x = was["x_mm"].get<double>(), y = was["y_mm"].get<double>(), w = was["w_mm"].get<double>(), h = was["h_mm"].get<double>();
            if (!by_hand) {
                QCOMPARE(line.x_mm.value(), x);
                QCOMPARE(line.y_mm.value(), y);
                QCOMPARE(line.w_mm.value(), w);
                QCOMPARE(line.h_mm.value(), h);
            } else {
                ++grown;
                QVERIFY(std::fabs(line.w_mm.value() - w * 1.035) < 0.0006);
                QVERIFY(std::fabs(line.h_mm.value() - h * 1.035) < 0.0006);
                QVERIFY(std::fabs((line.x_mm.value() + line.w_mm.value() / 2) - (x + w / 2)) < 0.0011);
                QVERIFY(std::fabs((line.y_mm.value() + line.h_mm.value() / 2) - (y + h / 2)) < 0.0011);
            }
            // (its words, style and tails as they were)
            QCOMPARE(line.text, was["text"].get<std::string>());
            QVERIFY(line.style == style);
        }
        QVERIFY2(grown >= 30, qPrintable(QString::number(grown)));
    }

    // The letters of a book of before keep their size: every line of the book main 5cf6551a wrote is set at the em it was
    // set at there, its ink as big, at 150 and at 600 dpi (old_letters.json: recorded from that build).
    void theLettersOfABookOfBeforeKeepTheirSize() {
        const Json was = Json::parse(genko::test::read_bytes(genko::test::test_data(QStringLiteral("fukidashi/old_letters.json"))));
        const Document doc = genko::storage::load_document(old_book_dir()).document;
        int checked = 0;
        QStringList unlike;
        for (const genko::core::StoryLine& line : doc.story) {
            for (const int dpi : {150, 600}) {
                const Json& old = was[line.id][std::to_string(dpi)];
                const genko::render::text::Layout layout = genko::render::text::text_layout(line, dpi);
                const std::array<int, 2> ink = ink_size(layout.image);
                ++checked;
                if (layout.em != old["em"].get<std::int64_t>() || ink[0] != old["ink"][0].get<int>() || ink[1] != old["ink"][1].get<int>()) {
                    unlike << QStringLiteral("%1 (%2) at %3: em %4 ink %5×%6, was em %7 ink %8×%9")
                                  .arg(QString::fromStdString(line.id), QString::fromStdString(line.balloon)).arg(dpi).arg(layout.em).arg(ink[0]).arg(ink[1])
                                  .arg(old["em"].get<std::int64_t>()).arg(old["ink"][0].get<int>()).arg(old["ink"][1].get<int>());
                }
            }
        }
        QVERIFY(checked > 150);
        QVERIFY2(unlike.isEmpty(), qPrintable(unlike.join(QStringLiteral("\n"))));
    }

    // Saved, the book says its balloons' generation; read again it does not grow twice; and saved again it is the same
    // bytes. A book of now (made by this build) is read back as it was written.
    void aBookGrowsOnlyOnce() {
        const Document first = genko::storage::load_document(old_book_dir()).document;
        QTemporaryDir dir;
        const std::filesystem::path saved = std::filesystem::path(dir.path().toStdString()) / "saved";
        for (const char* name : {"saved", "again", "fresh"}) std::filesystem::create_directories(std::filesystem::path(dir.path().toStdString()) / name);
        genko::test::write_project(first, saved);
        const Json payload = Json::parse(genko::test::read_bytes(QString::fromStdString((saved / "project.json").string())));
        QCOMPARE(payload.value("balloon_geometry", std::int64_t{0}), genko::core::kBalloonGeometry);
        QVERIFY(!names_feature(payload));  // (nothing of it needs a build of now)
        const Document second = genko::storage::load_document(saved).document;
        QCOMPARE(second.story.size(), first.story.size());
        for (std::size_t i = 0; i < first.story.size(); ++i) {
            QCOMPARE(second.story[i].x_mm.value(), first.story[i].x_mm.value());
            QCOMPARE(second.story[i].y_mm.value(), first.story[i].y_mm.value());
            QCOMPARE(second.story[i].w_mm.value(), first.story[i].w_mm.value());
            QCOMPARE(second.story[i].h_mm.value(), first.story[i].h_mm.value());
        }
        const std::filesystem::path again = std::filesystem::path(dir.path().toStdString()) / "again";
        genko::test::write_project(second, again);
        QVERIFY(genko::test::read_bytes(QString::fromStdString((again / "project.json").string())) ==
                genko::test::read_bytes(QString::fromStdString((saved / "project.json").string())));
        // (the page drawn from the book in memory and from the book saved and read again: the same pixels)
        QVERIFY(page_picture(first, 150, 1).tobytes() == page_picture(second, 150, 1).tobytes());

        // a book of now: its ovals are not grown when it is read back
        const Document now = one_line("speech");
        const std::filesystem::path fresh = std::filesystem::path(dir.path().toStdString()) / "fresh";
        genko::test::write_project(now, fresh);
        const Document back = genko::storage::load_document(fresh).document;
        QCOMPARE(back.story.front().w_mm.value(), 50.0);
        QCOMPARE(back.story.front().h_mm.value(), 40.0);
        QCOMPARE(back.story.front().x_mm.value(), 40.0);
        // (a book without an oval says nothing: there is nothing to grow — and nothing is added to what it was saved as)
        QVERIFY(!payload_of(one_line("box")).contains("balloon_geometry"));
        QVERIFY(!payload_of(blank_book()).contains("balloon_geometry"));
        QVERIFY(payload_of(one_line("thought")).contains("balloon_geometry"));
        // (a book of a later generation keeps what it says)
        Json later = payload_of(now);
        later["balloon_geometry"] = 3;
        const Document read_later = genko::storage::load_document_payload(later, fresh).document;
        QCOMPARE(read_later.balloon_geometry, std::int64_t{3});
        QCOMPARE(read_later.story.front().w_mm.value(), 50.0);
        QCOMPARE(payload_of(read_later)["balloon_geometry"].get<std::int64_t>(), std::int64_t{3});
    }

    // A balloon of now is sized for its words as the letters are fitted: the smallest true ellipse round them (the
    // app's and the auto-lettering's measure); the letters set in it are not made smaller, and a capsule and a polygon
    // hold theirs in their own shape.
    void theLettersFitTheShapeOfNow() {
        for (const char* kind : {"speech", "thought", "whisper", "capsule", "polygon", "rounded", "cloud", "shout"}) {
            const Document doc = one_line(kind, Json{{"x_mm", 40}, {"y_mm", 40}, {"w_mm", 34}, {"h_mm", 40}}, "あいうえお\nかきくけこ");
            const genko::render::text::Layout layout = genko::render::text::text_layout(doc.story.front(), 600);
            QVERIFY2(layout.em >= 100, qPrintable(QStringLiteral("%1: em %2").arg(kind).arg(layout.em)));  // (5 mm is 118 px)
            // (the letters lie inside the box, about its middle)
            QVERIFY(std::fabs(layout.corner_x + layout.image.width() / 2.0) < 34 / 25.4 * 600 * 0.2);
            QVERIFY(std::fabs(layout.corner_y + layout.image.height() / 2.0) < 40 / 25.4 * 600 * 0.2);
        }
        // (a capsule's gauge: 1 on its edge — the side, the round end's top, the end's shoulder)
        using genko::render::text::shape_gauge;
        QVERIFY(std::fabs(shape_gauge("capsule", 50, 0, 50, 200) - 1) < 1e-12);
        QVERIFY(std::fabs(shape_gauge("capsule", 0, 200, 50, 200) - 1) < 1e-12);
        QVERIFY(std::fabs(shape_gauge("capsule", 30, 190, 50, 200) - 1) < 1e-9);  // (30² + 40² = 50²: on the round end)
        QVERIFY(std::fabs(shape_gauge("capsule", 200, 0, 200, 50) - 1) < 1e-12);
        QVERIFY(shape_gauge("capsule", 25, 100, 50, 200) < 0.6);
        QVERIFY(shape_gauge("capsule", 45, 195, 50, 200) > 1.0);
    }

    // --- the sample sheets (written when asked: GENKO_FUKIDASHI_OUT) -------------------------------------------------------
    void sampleSheets() {
        const QByteArray out = qgetenv("GENKO_FUKIDASHI_OUT");
        if (out.isEmpty()) QSKIP("the sample sheets are written when GENKO_FUKIDASHI_OUT names a folder");
        const std::filesystem::path dir(out.toStdString());
        std::filesystem::create_directories(dir);
        const auto write = [&](const std::string& name, const Image& image) {
            genko::storage::write_atomic(dir / name, genko::formats::png_bytes(image.convert("RGB")));
        };
        const Document doc = genko::test::clean_book();
        write("clean-150-kinds.png", page_picture(doc, 150, 0));
        write("clean-150-tails.png", page_picture(doc, 150, 1));
        // (parts of the 600 dpi pages, pixel for pixel: 84 mm is 1984 px)
        const struct {
            const char* name;
            std::size_t page;
            double x, y;
        } parts[] = {{"clean-600-kinds-a.png", 0, 12, 10}, {"clean-600-kinds-b.png", 0, 104, 10}, {"clean-600-kinds-c.png", 0, 12, 86},
                     {"clean-600-kinds-d.png", 0, 104, 86}, {"clean-600-kinds-e.png", 0, 12, 162}, {"clean-600-kinds-f.png", 0, 104, 162},
                     {"clean-600-kinds-g.png", 0, 12, 213}, {"clean-600-kinds-h.png", 0, 104, 213},
                     {"clean-600-tails-a.png", 1, 12, 6}, {"clean-600-tails-b.png", 1, 104, 6}, {"clean-600-tails-c.png", 1, 12, 82},
                     {"clean-600-tails-d.png", 1, 104, 82}, {"clean-600-tails-e.png", 1, 12, 158}, {"clean-600-tails-f.png", 1, 104, 158},
                     {"clean-600-joined.png", 1, 12, 213}, {"clean-600-joined-b.png", 1, 104, 213}};
        for (const auto& part : parts) write(part.name, page_part(doc, 600, part.x, part.y, 84, 84, part.page));
        // (the same pages made black and white, as the monochrome print: a part)
        write("clean-600-mono.png", genko::render::to_bitonal(page_part(doc, 600, 12, 6, 84, 84, 1)).convert("L"));
        // (the book of before, as this build draws it: beside the pictures main's build made of it)
        const Document old = genko::storage::load_document(old_book_dir()).document;
        for (std::size_t i = 0; i < old.pages.size(); ++i) write("after-" + std::to_string(i + 1) + ".png", page_picture(old, 200, i));
    }
};

QTEST_GUILESS_MAIN(TestFukidashi)
#include "test_fukidashi.moc"
