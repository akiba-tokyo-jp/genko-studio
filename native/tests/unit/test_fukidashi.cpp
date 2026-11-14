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
#include "formats/checks.hpp"
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

std::filesystem::path old_book_dir(const QString& name = QStringLiteral("old_book")) {
    return std::filesystem::path(genko::test::test_data(QStringLiteral("fukidashi/") + name).toStdString());
}

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

    // A tail leaves the balloon from its outline: the two ends of its base lie on it, either side of its root (where the
    // line from the balloon's middle to the tip leaves the outline) and as far from it as each other, the base as wide
    // as asked; rounded, each join's curve begins on the outline and ends on the tail's side, a small one.
    void aTailLeavesFromTheOutline() {
        const clean::Box box{100, 100, 500, 400};
        const clean::P centre{300, 250};
        for (const clean::Contour& outline : {clean::ellipse(box), clean::rounded(box, 90.0), clean::rounded(box, 150.0)}) {
            const std::vector<double> lengths = clean::lengths_of(outline);
            const double total = lengths.back();
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
            for (const clean::P tip : {clean::P{620, 520}, clean::P{300, 20}, clean::P{-40, 300}, clean::P{560, 250}, clean::P{300, 560}, clean::P{40, 60}}) {
                for (const bool bent : {false, true}) {
                    clean::TailSpec spec;
                    spec.tip = tip;
                    spec.base = 80.0;
                    spec.fillet = 12.0;
                    spec.depth = 40.0;
                    if (bent) {
                        spec.has_via = true;
                        // (a bend handle outside the balloon, off the straight way to the tip)
                        const double away = std::hypot(tip.x - centre.x, tip.y - centre.y);
                        spec.via = {centre.x + (tip.x - centre.x) * 0.85 - (tip.y - centre.y) / away * 30, centre.y + (tip.y - centre.y) * 0.85 + (tip.x - centre.x) / away * 30};
                        QVERIFY(!clean::inside(outline, spec.via));
                    }
                    const clean::Tail tail = clean::tail(outline, outline, centre, spec);
                    QVERIFY(off_outline(tail.left) < 1e-6);
                    QVERIFY(off_outline(tail.right) < 1e-6);
                    // (the root, and the base's two ends along the outline from it: half the width each way, the same)
                    const double root = clean::facing(outline, lengths, centre, bent ? spec.via : tip);
                    const auto along = [&](clean::P p) {  // (how far along the outline from the root, the short way)
                        double best = 1e9, where = 0.0;
                        for (double d = -80; d <= 80; d += 0.05) {
                            const clean::P q = clean::point_at(outline, lengths, root + d);
                            const double off = std::hypot(p.x - q.x, p.y - q.y);
                            if (off < best) best = off, where = d;
                        }
                        return where;
                    };
                    const double left = along(tail.left), right = along(tail.right);
                    QVERIFY2(left * right < 0, qPrintable(QStringLiteral("%1 %2").arg(left).arg(right)));
                    // (the sides are the same distance from the tail's middle line; along an outline that turns under one of
                    // them and runs straight under the other, that is a few px more on the one)
                    QVERIFY2(std::fabs(std::fabs(left) - std::fabs(right)) <= 3.2,
                             qPrintable(QStringLiteral("tip %1, %2 bent %3: %4 %5").arg(tip.x).arg(tip.y).arg(bent).arg(left).arg(right)));
                    // (as wide as asked, measured straight across between the two ends of its base: along the outline, which
                    // bows between them, a little more)
                    QVERIFY2(std::fabs(std::hypot(tail.left.x - tail.right.x, tail.left.y - tail.right.y) - 80.0) <= 0.8, qPrintable(QString::number(std::hypot(tail.left.x - tail.right.x, tail.left.y - tail.right.y))));
                    QVERIFY2(std::fabs(left) + std::fabs(right) >= 80.0 - 1.0 && std::fabs(left) + std::fabs(right) <= 80.0 * 1.05,
                             qPrintable(QStringLiteral("tip %1, %2 bent %3: %4 %5").arg(tip.x).arg(tip.y).arg(bent).arg(left).arg(right)));
                    QVERIFY(total > 0);
                    // (two joins, each a curve from the outline to the tail's side; only the curve is part of the line; small:
                    // no further along the outline than asked)
                    QVERIFY2(tail.joins.size() == 2, qPrintable(QStringLiteral("tip %1, %2 bent %3: %4 joins of an outline of %5").arg(tip.x).arg(tip.y).arg(bent).arg(tail.joins.size()).arg(outline.size())));
                    for (const clean::Shape& join : tail.joins) {
                        std::size_t first = join.stroke.size(), last = 0;
                        for (std::size_t i = 0; i < join.stroke.size(); ++i) {
                            if (!join.stroke[i]) continue;
                            first = std::min(first, i);
                            last = std::max(last, i + 1);
                        }
                        QVERIFY(first < last);
                        QVERIFY(off_outline(join.points[first]) < 1e-6);
                        QVERIFY2(off_outline(join.points[last % join.points.size()]) > 1.0,
                                 qPrintable(QStringLiteral("tip %1, %2 bent %3: %4").arg(tip.x).arg(tip.y).arg(bent).arg(off_outline(join.points[last % join.points.size()]))));
                        const double reach = std::min(std::hypot(join.points[first].x - tail.left.x, join.points[first].y - tail.left.y),
                                                      std::hypot(join.points[first].x - tail.right.x, join.points[first].y - tail.right.y));
                        QVERIFY2(reach <= 12.0 + 1e-6, qPrintable(QString::number(reach)));
                    }
                    // (without a join: a corner, no shape added)
                    spec.fillet = 0.0;
                    QVERIFY(clean::tail(outline, outline, centre, spec).joins.empty());
                }
            }
        }
    }

    // A tail whose tip lies straight out from its root has no bend, and is its own mirror image: each point of its
    // left side mirrors the point of its right side, to a hair. A tip a hair off either way gives tails that mirror each
    // other (the bend does not jump from one side to the other).
    void aTailStraightOutHasNoBend() {
        const clean::Box box{100, 100, 500, 400};
        const clean::Contour outline = clean::ellipse(box);
        const clean::P centre{300, 250};
        clean::TailSpec spec;
        spec.base = 70.0;
        spec.fillet = 10.0;
        spec.depth = 30.0;
        const auto mirrored = [&](const clean::Shape& one, const clean::Shape& other, double slack) {
            // (every point of one, mirrored in the line x = 300, lies on the other's outline)
            for (const clean::P p : one.points) {
                const clean::P m{600 - p.x, p.y};
                double best = 1e9;
                for (std::size_t i = 0; i < other.points.size(); ++i) {
                    const clean::P a = other.points[i], b = other.points[(i + 1) % other.points.size()];
                    const double dx = b.x - a.x, dy = b.y - a.y;
                    const double t = std::clamp(((m.x - a.x) * dx + (m.y - a.y) * dy) / std::max(1e-12, dx * dx + dy * dy), 0.0, 1.0);
                    best = std::min(best, std::hypot(m.x - (a.x + dx * t), m.y - (a.y + dy * t)));
                }
                if (best > slack) return false;
            }
            return true;
        };
        for (const double tip_y : {560.0, 430.0, -60.0}) {  // (below, far and near; above)
            spec.tip = {300, tip_y};
            const clean::Tail straight = clean::tail(outline, outline, centre, spec);
            QVERIFY(mirrored(straight.shape, straight.shape, 1e-6));
            // (outside the balloon — past its root, at y 400 below and y 100 above — never wider than its base: no bend)
            for (const clean::P p : straight.shape.points) {
                if (tip_y > 250 ? p.y >= 400.0 : p.y <= 100.0) QVERIFY(std::fabs(p.x - 300) <= 35.0 + 1e-6);
            }
            QCOMPARE(straight.joins.size(), std::size_t{2});
            QVERIFY(mirrored(straight.joins[0], straight.joins[1], 1e-6));
            spec.tip = {300.4, tip_y};
            const clean::Tail a = clean::tail(outline, outline, centre, spec);
            spec.tip = {299.6, tip_y};
            const clean::Tail b = clean::tail(outline, outline, centre, spec);
            QVERIFY(mirrored(a.shape, b.shape, 1e-6));
            QVERIFY(mirrored(a.shape, straight.shape, 1.0));  // (and next to none of a bend)
        }
    }

    // The balloon's own outline is as it is without a tail everywhere but at the tail's root: its fill and its line, away
    // from the tail and its two small joins, are the tail-less balloon's — an oval, a capsule and a rounded box, a tip
    // eight ways, near and far, straight and bent.
    void theBodyIsUntouchedAwayFromTheTail_data() {
        QTest::addColumn<int>("body");
        QTest::addColumn<bool>("bent");
        for (const int body : {0, 1, 2}) {
            for (const bool bent : {false, true}) {
                QTest::newRow(qPrintable(QStringLiteral("%1%2").arg(body == 0 ? "oval" : body == 1 ? "capsule" : "rounded").arg(bent ? ", bent" : ""))) << body << bent;
            }
        }
    }
    void theBodyIsUntouchedAwayFromTheTail() {
        QFETCH(int, body);
        QFETCH(bool, bent);
        const int w = 560, h = 480;
        const clean::Box box = body == 1 ? clean::Box{200, 110, 320, 370} : clean::Box{130, 130, 430, 350};
        const clean::P centre{box.cx(), box.cy()};
        const clean::Contour outline = body == 0 ? clean::ellipse(box) : body == 1 ? clean::rounded(box, 60.0) : clean::rounded(box, 66.0);
        const double width = 8.0, base = 44.0, fillet = 12.0;
        const std::vector<clean::Shape> alone{clean::whole(outline)};
        const clean::Plane fill0 = clean::fill_plane(alone, w, h);
        clean::Plane band0 = clean::ribbon_plane(clean::edge_runs(alone), width, w, h);
        for (std::size_t i = 0; i < band0.values.size(); ++i) band0.values[i] *= fill0.values[i];
        const double ways[8][2] = {{0, 1}, {0.7, 0.7}, {1, 0}, {0.7, -0.7}, {0, -1}, {-0.7, -0.7}, {-1, 0}, {-0.7, 0.7}};
        for (const auto& way : ways) {
            for (const double reach : {190.0, 235.0}) {
                clean::TailSpec spec;
                spec.tip = {centre.x + way[0] * reach, centre.y + way[1] * reach};
                spec.base = base;
                spec.fillet = fillet;
                spec.depth = 30.0;
                if (bent) {
                    spec.has_via = true;
                    spec.via = {centre.x + way[0] * reach * 0.7 - way[1] * 50, centre.y + way[1] * reach * 0.7 + way[0] * 50};
                }
                const clean::Tail tail = clean::tail(outline, outline, centre, spec);
                std::vector<clean::Shape> shapes{clean::whole(outline), tail.shape};
                for (const clean::Shape& join : tail.joins) shapes.push_back(join);
                const clean::Plane fill = clean::fill_plane(shapes, w, h);
                clean::Plane band = clean::ribbon_plane(clean::edge_runs(shapes), width, w, h);
                for (std::size_t i = 0; i < band.values.size(); ++i) band.values[i] *= fill.values[i];
                // (near the tail: inside it or its joins, or within the line's width and a little of their edges)
                std::vector<const clean::Contour*> parts{&tail.shape.points};
                for (const clean::Shape& join : tail.joins) parts.push_back(&join.points);
                const auto near_tail = [&](double x, double y) {
                    for (const clean::Contour* part : parts) {
                        if (clean::inside(*part, {x, y})) return true;
                        for (std::size_t i = 0; i < part->size(); ++i) {
                            const clean::P a = (*part)[i], b = (*part)[(i + 1) % part->size()];
                            const double dx = b.x - a.x, dy = b.y - a.y;
                            const double t = std::clamp(((x - a.x) * dx + (y - a.y) * dy) / std::max(1e-12, dx * dx + dy * dy), 0.0, 1.0);
                            if (std::hypot(x - (a.x + dx * t), y - (a.y + dy * t)) <= width + 2.5) return true;
                        }
                    }
                    return false;
                };
                int compared = 0, touched = 0;
                for (int y = 0; y < h; ++y) {
                    for (int x = 0; x < w; ++x) {
                        const float f = fill.at(x, y), f0 = fill0.at(x, y), b = band.at(x, y), b0 = band0.at(x, y);
                        if (std::fabs(f - f0) < 1e-3f && std::fabs(b - b0) < 1e-3f) continue;
                        if (near_tail(x + 0.5, y + 0.5)) {
                            ++touched;
                            continue;
                        }
                        QFAIL(qPrintable(QStringLiteral("the balloon differs at %1, %2, away from its tail (toward %3, %4)").arg(x).arg(y).arg(way[0]).arg(way[1])));
                    }
                    compared += w;
                }
                QVERIFY(touched > 100 && compared > 0);  // (the tail is there)
                // (and what the joins change of the balloon's own line is small: within the join's reach of the base's ends)
                const double hold = fillet + width + 3;
                for (int y = 0; y < h; ++y) {
                    for (int x = 0; x < w; ++x) {
                        if (fill0.at(x, y) < 0.999f || band0.at(x, y) < 0.999f || band.at(x, y) > 0.5f) continue;  // (the balloon's line, taken away)
                        if (clean::inside(tail.shape.points, {x + 0.5, y + 0.5})) continue;                           // (under the tail: the way in)
                        const double off = std::min(std::hypot(x + 0.5 - tail.left.x, y + 0.5 - tail.left.y), std::hypot(x + 0.5 - tail.right.x, y + 0.5 - tail.right.y));
                        QVERIFY2(off <= hold, qPrintable(QStringLiteral("the balloon's line is gone %1 px from the tail's base at %2, %3").arg(off).arg(x).arg(y)));
                    }
                }
            }
        }
    }

    // The join has no corner and nothing else: the joins add the two small notches to the union and the line runs
    // through each without a break.
    void aRoundedJoinFillsTheNotch() {
        const clean::Box box{100, 100, 500, 400};
        const clean::Contour outline = clean::ellipse(box);
        clean::TailSpec spec;
        spec.tip = {600, 520};
        spec.base = 80.0;
        spec.depth = 50.0;
        const clean::Tail sharp = clean::tail(outline, outline, {300, 250}, spec);
        spec.fillet = 14.0;
        const clean::Tail round = clean::tail(outline, outline, {300, 250}, spec);
        const auto covered = [&](const clean::Tail& tail) {
            std::vector<clean::Shape> shapes{clean::whole(outline), tail.shape};
            for (const clean::Shape& join : tail.joins) shapes.push_back(join);
            const clean::Plane fill = clean::fill_plane(shapes, 700, 600);
            double sum = 0.0;
            for (const float v : fill.values) sum += v;
            return sum;
        };
        const double more = covered(round) - covered(sharp);
        QVERIFY2(more > 4.0 && more < 2 * 14.0 * 14.0, qPrintable(QString::number(more)));
        std::vector<clean::Shape> shapes{clean::whole(outline), round.shape};
        for (const clean::Shape& join : round.joins) shapes.push_back(join);
        const std::vector<clean::Run> runs = clean::edge_runs(shapes);
        const clean::Plane band = clean::ribbon_plane(runs, 8.0, 700, 600);
        const clean::Plane fill = clean::fill_plane(shapes, 700, 600);
        for (const clean::Shape& join : round.joins) {
            for (std::size_t i = 0; i < join.points.size(); ++i) {
                if (!join.stroke[i]) continue;
                const clean::P p = join.points[i];
                QVERIFY(plane_at(band, p.x, p.y) > 0.99);
                QVERIFY(plane_at(fill, p.x, p.y) > 0.2);
            }
        }
    }

    // A bubble smaller than its line is wide (a thought's last bubble with a thick line): black through and through, no
    // pin-hole where the line folds over itself.
    void aBubbleSmallerThanItsLineIsSolid() {
        for (const double radius : {11.8, 6.0, 3.2, 14.0}) {  // (a 1 mm bubble at 600 dpi; smaller; one just under the 14.2 px line)
            const std::vector<clean::Shape> shapes{clean::whole(clean::circle({60.3, 59.6}, radius))};
            const clean::Plane fill = clean::fill_plane(shapes, 120, 120);
            const clean::Plane ribbon = clean::ribbon_plane(clean::edge_runs(shapes), 14.2, 120, 120);
            for (int y = 0; y < 120; ++y) {
                for (int x = 0; x < 120; ++x) {
                    if (fill.at(x, y) > 0.0f) QVERIFY2(ribbon.at(x, y) > 0.999f, qPrintable(QStringLiteral("r %1 at %2, %3: %4").arg(radius).arg(x).arg(y).arg(ribbon.at(x, y))));
                }
            }
        }
        // (a box of no size, or none a pixel wide, is nothing: no outline, no NaN)
        QVERIFY(clean::cloud(clean::Box{10, 10, 10, 10}, 8).empty());
        for (const clean::Contour& outline : {clean::ellipse(clean::Box{10, 10, 10, 30}), clean::rounded(clean::Box{10, 10, 10, 10}, 3.0), clean::squircle(clean::Box{5, 5, 5, 9}),
                                             clean::polygon(clean::Box{5, 5, 5, 5}, 8, 1.0), clean::burst(clean::Box{5, 5, 5, 5}, 12, 0.2, 0.0, {}),
                                             clean::sawtooth(clean::Box{5, 5, 5, 5}, 12, 0.1), clean::cloud(clean::Box{5, 5, 9, 5}, 8)}) {
            for (const clean::P p : outline) QVERIFY(std::isfinite(p.x) && std::isfinite(p.y));
            const clean::Plane fill = clean::fill_plane({clean::whole(outline)}, 20, 40);
            for (const float v : fill.values) QVERIFY(std::isfinite(v) && v == 0.0f);
        }
        const Document thin = one_line("cloud", Json{{"w_mm", 0.01}, {"h_mm", 0.01}, {"tail", Json::array({60, 60})}});
        const Image page = page_picture(thin, 150);
        QVERIFY(page.width() > 0);
    }

    // A zigzag tail runs to a point: where it is too slim to show white between its two lines it stops stepping out, so
    // no small closed patch of white (an "eye") is left along it. The white of the picture is two pieces — the paper
    // round the balloon, and the balloon's inside with its tail's — at any resolution and whichever way the tail points.
    // (Tails of an ordinary length, their tips 14 mm or more past the outline: a zigzag has sixteen steps whatever its
    // length, and on a much shorter one the steps themselves are closer than the line is wide.)
    void aZigzagTailClosesWithAPoint() {
        const genko::render::text::LetterParts balloon_only{true, false, false, false};
        const double tips[][2] = {{16, 20}, {-18, 19}, {32, 3}, {4, -29}, {-27, -19}, {30, 26}, {-34, 2}, {2, 30}};
        for (const int dpi : {150, 350, 600}) {
            for (const auto& tip : tips) {
                const Document doc = one_line("speech", Json{{"x_mm", 40}, {"y_mm", 40}, {"w_mm", 30}, {"h_mm", 24},
                                                             {"tails", Json::array({Json{{"to", Json::array({55 + tip[0], 52 + tip[1]})}, {"kind", "zigzag"}}})}});
                const genko::render::Size size{static_cast<int>(120 / 25.4 * dpi), static_cast<int>(110 / 25.4 * dpi)};
                Image page = Image::create("RGB", size, genko::render::Ink{255, 255, 255});
                genko::render::text::clear_balloon_cache();
                genko::render::text::draw_lines(genko::render::text::whole_page(page), {&doc.story.front()}, dpi, std::nullopt, false, nullptr, {}, nullptr, &balloon_only);
                const Grey g = grey_of(page);
                // the pieces of white (anything not mostly ink), joined side to side and corner to corner
                std::vector<int> piece(static_cast<std::size_t>(g.w) * static_cast<std::size_t>(g.h), 0);
                std::vector<std::int64_t> sizes;
                std::vector<int> stack;
                for (int start = 0; start < g.w * g.h; ++start) {
                    if (piece[static_cast<std::size_t>(start)] != 0 || g.at(start % g.w, start / g.w) <= 64) continue;
                    sizes.push_back(0);
                    const int id = static_cast<int>(sizes.size());
                    stack.push_back(start);
                    piece[static_cast<std::size_t>(start)] = id;
                    while (!stack.empty()) {
                        const int at = stack.back();
                        stack.pop_back();
                        ++sizes.back();
                        const int x = at % g.w, y = at / g.w;
                        for (int dy = -1; dy <= 1; ++dy) {
                            for (int dx = -1; dx <= 1; ++dx) {
                                const int nx = x + dx, ny = y + dy;
                                if (nx < 0 || ny < 0 || nx >= g.w || ny >= g.h) continue;
                                const int next = ny * g.w + nx;
                                if (piece[static_cast<std::size_t>(next)] != 0 || g.at(nx, ny) <= 64) continue;
                                piece[static_cast<std::size_t>(next)] = id;
                                stack.push_back(next);
                            }
                        }
                    }
                }
                QString said;
                for (const std::int64_t n : sizes) said += QStringLiteral(" %1").arg(n);
                QVERIFY2(sizes.size() == 2, qPrintable(QStringLiteral("%1 dpi, tip %2, %3: pieces of white%4").arg(dpi).arg(tip[0]).arg(tip[1]).arg(said)));
                // (and the tail is there: ink near its tip)
                const int tx = static_cast<int>((55 + tip[0] * 0.9) / 25.4 * dpi), ty = static_cast<int>((52 + tip[1] * 0.9) / 25.4 * dpi);
                int dark = 0;
                const int reach = std::max(3, dpi / 40);
                for (int y = ty - reach; y <= ty + reach; ++y)
                    for (int x = tx - reach; x <= tx + reach; ++x) dark += g.at(x, y) < 128 ? 1 : 0;
                QVERIFY2(dark > 0, qPrintable(QStringLiteral("%1 dpi, tip %2, %3").arg(dpi).arg(tip[0]).arg(tip[1])));
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
        QTest::newRow("150 dpi, the other tails and the joined balloons") << 150 << 2;
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

    // Two balloons that are the same but for their place are the same pixels; and a balloon whose tail points straight
    // down (or up) from its middle is its own mirror image on the page, within a level of 255: nothing of a tail leans by
    // chance. (300 dpi: the box 500..1100 px, its middle 800.)
    void theSameBalloonIsTheSamePixels() {
        const auto drawn = [](double x_mm, const Json& tail_by, int dpi) {
            const Document doc = one_line("speech", Json{{"x_mm", x_mm}, {"w_mm", 50.8}, {"h_mm", 40.64}, {"text", "あ"},
                                                         {"tails", Json::array({Json{{"to", Json::array({x_mm + 25.4 + tail_by[0].get<double>(), 60.32 + tail_by[1].get<double>()})}}})}});
            const genko::render::Size size{static_cast<int>(8.5 * dpi), static_cast<int>(5.0 * dpi)};
            Image page = Image::create("RGB", size, genko::render::Ink{255, 255, 255});
            const genko::render::text::LetterParts balloon_only{true, false, false, false};
            genko::render::text::clear_balloon_cache();
            genko::render::text::draw_lines(genko::render::text::whole_page(page), {&doc.story.front()}, dpi, std::nullopt, false, nullptr, {}, nullptr, &balloon_only);
            return page;
        };
        for (const Json& by : {Json::array({0.0, 42.0}), Json::array({0.0, -42.0}), Json::array({0.0, 30.0})}) {
            const Image here = drawn(42.333333333333336, by, 300);   // (500 px)
            const Image there = drawn(67.73333333333333, by, 300);   // (800 px: 300 px to the right)
            const genko::render::Box a{400, 0, 1200, here.height()};
            const genko::render::Box b{700, 0, 1500, there.height()};
            QVERIFY2(here.crop(a).tobytes() == there.crop(b).tobytes(), by.dump().c_str());
            const Grey g = grey_of(here.crop(a));
            int unlike = 0, dark = 0;
            for (int y = 0; y < g.h; ++y) {
                for (int x = 0; x < g.w / 2; ++x) {
                    unlike += std::abs(g.at(x, y) - g.at(g.w - 1 - x, y)) > 1 ? 1 : 0;
                    dark += g.at(x, y) < 128 ? 1 : 0;
                }
            }
            QVERIFY(dark > 500);
            QVERIFY2(unlike == 0, qPrintable(QStringLiteral("%1: %2 pixels are not mirrored").arg(QString::fromStdString(by.dump())).arg(unlike)));
        }
        // (a tail to the right and the same tail to the left: mirror images of each other)
        const Grey right = grey_of(drawn(42.333333333333336, Json::array({44.0, 12.0}), 300).crop(genko::render::Box{200, 0, 1400, 1500}));
        const Grey left = grey_of(drawn(42.333333333333336, Json::array({-44.0, 12.0}), 300).crop(genko::render::Box{200, 0, 1400, 1500}));
        int unlike = 0;
        for (int y = 0; y < right.h; ++y) {
            for (int x = 0; x < right.w; ++x) unlike += std::abs(right.at(x, y) - left.at(right.w - 1 - x, y)) > 1 ? 1 : 0;
        }
        QCOMPARE(unlike, 0);
    }

    // style.corner_mm: 0 is square corners (a rounded balloon with it is a box), not "not set".
    void aCornerOfZeroIsSquare() {
        const Image box = page_part(one_line("box"), 300, 30, 30, 70, 60);
        const Image rounded = page_part(one_line("rounded"), 300, 30, 30, 70, 60);
        const Image square = page_part(one_line("rounded", Json{{"style", Json{{"corner_mm", 0}}}}), 300, 30, 30, 70, 60);
        QVERIFY(rounded.tobytes() != box.tobytes());
        const Grey a = grey_of(square), b = grey_of(box);
        // (the outline: the corner pixel of the box is line in both — the letters' room differs, so only the edge is held)
        const int cx = static_cast<int>(std::lround(10 / 25.4 * 300)), cy = cx;
        QVERIFY(a.at(cx + 1, cy + 1) < 60 && b.at(cx + 1, cy + 1) < 60);
        QVERIFY(grey_of(rounded).at(cx + 1, cy + 1) > 200);
        // (and a box takes round corners when it is given some)
        QVERIFY(grey_of(page_part(one_line("box", Json{{"style", Json{{"corner_mm", 6}}}}), 300, 30, 30, 70, 60)).at(cx + 1, cy + 1) > 200);
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
    // — its letters' box (core::letter_box) the box it had, to the last bit —, what it holds from its corner (the balloon
    // eraser's cuts, the path its letters follow) moved to stay where it was on the page, a border_mm of 0 made the
    // hairline it drew; and nothing else of any line changes. Both books of before: old_book, old_book2.
    void aBookOfBeforeGrowsItsOvals_data() {
        QTest::addColumn<QString>("name");
        QTest::newRow("old_book") << QStringLiteral("old_book");
        QTest::newRow("old_book2") << QStringLiteral("old_book2");
    }
    void aBookOfBeforeGrowsItsOvals() {
        QFETCH(QString, name);
        const std::filesystem::path dir = old_book_dir(name);
        const Json before = Json::parse(genko::test::read_bytes(QString::fromStdString((dir / "project.json").string())));
        QVERIFY(!before.contains("balloon_geometry"));
        const genko::storage::LoadResult read = genko::storage::load_document(dir);
        QVERIFY2(read.document.read_only_reason.empty(), read.document.read_only_reason.c_str());
        const Document& doc = read.document;
        QCOMPARE(doc.story.size(), before["story"].size());
        int grown = 0, moved = 0, hairlines = 0;
        for (std::size_t i = 0; i < doc.story.size(); ++i) {
            const genko::core::StoryLine& line = doc.story[i];
            const Json& was = before["story"][i];
            const std::string kind = was.value("balloon", std::string("speech"));
            Json style = was.value("style", Json::object());
            if (!style.is_object()) style = Json::object();
            const bool oval = kind == "speech" || kind == "thought" || kind == "whisper";
            const bool has_path = was.contains("path") && !was["path"].is_null();
            const bool by_hand = oval && style.value("hand", true) && style.value("wobble", 0.0) == 0.0 && !has_path;
            const double x = was["x_mm"].get<double>(), y = was["y_mm"].get<double>(), w = was["w_mm"].get<double>(), h = was["h_mm"].get<double>();
            // (the letters' box: the box of before, exactly — whether the balloon's grew or not)
            const std::array<double, 4> letters = genko::core::letter_box(line);
            QVERIFY2(letters[0] == x && letters[1] == y && letters[2] == w && letters[3] == h, line.id.c_str());
            Json want = style;
            if (want.contains("border_mm") && want["border_mm"] == Json(0.0)) {
                want["border_mm"] = genko::core::kHairlineMm;
                ++hairlines;
            }
            if (!by_hand) {
                QCOMPARE(line.x_mm.value(), x);
                QCOMPARE(line.w_mm.value(), w);
                QCOMPARE(line.y_mm.value(), y);
                QCOMPARE(line.h_mm.value(), h);
            } else {
                ++grown;
                QVERIFY(std::fabs(line.w_mm.value() - w * 1.035) < 1e-9);
                QVERIFY(std::fabs(line.h_mm.value() - h * 1.035) < 1e-9);
                QVERIFY(std::fabs((line.x_mm.value() + line.w_mm.value() / 2) - (x + w / 2)) < 1e-9);
                QVERIFY(std::fabs((line.y_mm.value() + line.h_mm.value() / 2) - (y + h / 2)) < 1e-9);
                // (from its corner: each point as far from the new corner as it is on the page)
                const double dx = x - line.x_mm.value(), dy = y - line.y_mm.value();
                const auto shifted = [&](Json points) {
                    for (Json& point : points) point = Json::array({point[0].get<double>() + dx, point[1].get<double>() + dy});
                    return points;
                };
                const auto close = [](const Json& a, const Json& b) {
                    if (a.size() != b.size()) return false;
                    for (std::size_t k = 0; k < a.size(); ++k) {
                        if (std::fabs(a[k][0].get<double>() - b[k][0].get<double>()) > 1e-9 || std::fabs(a[k][1].get<double>() - b[k][1].get<double>()) > 1e-9) return false;
                    }
                    return true;
                };
                if (style.contains("text_path")) {
                    QVERIFY(close(line.style["text_path"], shifted(style["text_path"])));
                    want["text_path"] = line.style["text_path"];
                    ++moved;
                }
                if (style.contains("cuts")) {
                    QCOMPARE(line.style["cuts"].size(), style["cuts"].size());
                    for (std::size_t k = 0; k < style["cuts"].size(); ++k) {
                        QVERIFY(close(line.style["cuts"][k]["points"], shifted(style["cuts"][k]["points"])));
                        QCOMPARE(line.style["cuts"][k]["width_mm"], style["cuts"][k]["width_mm"]);
                    }
                    want["cuts"] = line.style["cuts"];
                    ++moved;
                }
            }
            // (its words, tails and the rest of its style as they were)
            QCOMPARE(line.text, was["text"].get<std::string>());
            QVERIFY2(line.style == want, (line.id + " " + line.style.dump() + " / " + want.dump()).c_str());
            QCOMPARE(Json(line.tails), was.value("tails", Json::array()));
        }
        if (name == QLatin1String("old_book")) {
            QVERIFY2(grown >= 30, qPrintable(QString::number(grown)));
        } else {
            QCOMPARE(grown, 12);
            QCOMPARE(moved, 4);       // (two lines on a path, two balloons cut)
            QCOMPARE(hairlines, 4);   // (an oval, a box, a rounded box, an oval that is not hand-drawn)
        }
    }

    // The letters of a book of before keep their size: every line of the books main 5cf6551a wrote is set at the em it
    // was set at there, its ink as big, at 150, 350, 600 and 1200 dpi (old_letters.json: recorded from that build's
    // libraries).
    void theLettersOfABookOfBeforeKeepTheirSize_data() {
        QTest::addColumn<QString>("name");
        QTest::addColumn<int>("dpi");
        for (const char* name : {"old_book", "old_book2"}) {
            for (const int dpi : {150, 350, 600, 1200}) QTest::newRow(qPrintable(QStringLiteral("%1 at %2").arg(name).arg(dpi))) << QString::fromLatin1(name) << dpi;
        }
    }
    void theLettersOfABookOfBeforeKeepTheirSize() {
        QFETCH(QString, name);
        QFETCH(int, dpi);
        const Json was = Json::parse(genko::test::read_bytes(genko::test::test_data(QStringLiteral("fukidashi/old_letters.json"))))[name.toStdString()];
        const Document doc = genko::storage::load_document(old_book_dir(name)).document;
        int checked = 0;
        QStringList unlike;
        for (const genko::core::StoryLine& line : doc.story) {
            if (!was.contains(line.id)) continue;  // (letters on a path: no lettering of their own — held by the page's letters below)
            const Json& old = was[line.id][std::to_string(dpi)];
            const genko::render::text::Layout layout = genko::render::text::text_layout(line, dpi);
            const std::array<int, 2> ink = ink_size(layout.image);
            ++checked;
            if (layout.em != old["em"].get<std::int64_t>() || ink[0] != old["ink"][0].get<int>() || ink[1] != old["ink"][1].get<int>()) {
                unlike << QStringLiteral("%1 (%2): em %3 ink %4×%5, was em %6 ink %7×%8")
                              .arg(QString::fromStdString(line.id), QString::fromStdString(line.balloon)).arg(layout.em).arg(ink[0]).arg(ink[1])
                              .arg(old["em"].get<std::int64_t>()).arg(old["ink"][0].get<int>()).arg(old["ink"][1].get<int>());
            }
        }
        QVERIFY(checked >= 15);
        QVERIFY2(unlike.isEmpty(), qPrintable(unlike.join(QStringLiteral("\n"))));
    }

    // …and are the same pixels at the same places: each page's letters alone (no balloon), drawn on nothing, are the
    // bytes main 5cf6551a's libraries drew — the letters in ovals that grew, on a path that moved with its box, moved
    // by text_dx_mm, turned (old_letter_layers.json: the sha256 of each page's letters at 150 and 600 dpi).
    void theLettersOfABookOfBeforeAreTheSamePixels() {
        const Json was = Json::parse(genko::test::read_bytes(genko::test::test_data(QStringLiteral("fukidashi/old_letter_layers.json"))));
        const genko::render::text::LetterParts only_letters{false, true, true, true};
        int pages = 0;
        for (const char* name : {"old_book", "old_book2"}) {
            const Document doc = genko::storage::load_document(old_book_dir(QString::fromLatin1(name))).document;
            for (std::size_t i = 0; i < doc.pages.size(); ++i) {
                const std::vector<const genko::core::StoryLine*> lines = doc.story_for_page(doc.page(i).index);
                for (const int dpi : {150, 600}) {
                    genko::render::text::clear_balloon_cache();
                    const genko::render::Size size = genko::render::render_page(doc.page(i), dpi, {}, &doc).image.size();
                    Image sheet = Image::create("RGBA", size, genko::render::Ink{0, 0, 0, 0});
                    genko::render::text::draw_lines(genko::render::text::whole_page(sheet), lines, dpi, std::nullopt, false, nullptr, {}, nullptr, &only_letters);
                    const std::string want = was[name][std::to_string(i + 1)][std::to_string(dpi)].get<std::string>();
                    QVERIFY2(genko::storage::sha256_hex(sheet.tobytes()) == want, qPrintable(QStringLiteral("%1 page %2 at %3 dpi").arg(name).arg(i + 1).arg(dpi)));
                    ++pages;
                }
            }
        }
        QCOMPARE(pages, 12);
    }

    // The checks tell a book of before what they told it: its issues — their codes, lines, words and boxes — are those
    // main 5cf6551a's libraries reported (old_issues.json), though its ovals' boxes grew: an oval 0.3 mm inside the trim,
    // 0.4 mm past the frame, two ovals sharing 14 % (the checks measure the letters' box).
    void theChecksTellABookOfBeforeWhatTheyToldIt() {
        const Json was = Json::parse(genko::test::read_bytes(genko::test::test_data(QStringLiteral("fukidashi/old_issues.json"))));
        for (const char* name : {"old_book", "old_book2"}) {
            const Document doc = genko::storage::load_document(old_book_dir(QString::fromLatin1(name))).document;
            for (std::size_t i = 0; i < doc.pages.size(); ++i) {
                const Json issues = genko::formats::checks::page_issues(doc, doc.page(i));
                std::string where;
                QVERIFY2(genko::test::same_content(issues, was[name][i], &where), (std::string(name) + " page " + std::to_string(i + 1) + ": " + where).c_str());
            }
        }
        // (the grown balloons themselves do cross those lines: without the letters' box the book would be told more)
        const Document doc = genko::storage::load_document(old_book_dir(QStringLiteral("old_book2"))).document;
        const genko::core::Rect trim = doc.page(1).trim_rect_mm();
        int past = 0;
        for (const genko::core::StoryLine& line : doc.story) {
            if (line.id == "Q1" && line.x_mm.value() + line.w_mm.value() > trim.x.value() + trim.width.value() + 0.01) ++past;
        }
        QCOMPARE(past, 1);
    }

    // The balloon eraser's stroke stays where it went: where the stroke of a book of before crosses the balloon's edge
    // (as the balloon is drawn now), the line is gone; the same balloon without its cuts has its line there.
    void aCutStaysWhereItWent() {
        const std::filesystem::path dir = old_book_dir(QStringLiteral("old_book2"));
        const Json before = Json::parse(genko::test::read_bytes(QString::fromStdString((dir / "project.json").string())));
        Document doc = genko::storage::load_document(dir).document;
        doc.story.erase(std::remove_if(doc.story.begin(), doc.story.end(), [](const genko::core::StoryLine& line) { return line.id != "P3" && line.id != "P4"; }), doc.story.end());
        Document uncut = doc;
        for (genko::core::StoryLine& line : uncut.story) line.style.erase("cuts");
        const int dpi = 300;
        const Grey cut_page = grey_of(page_picture(doc, dpi));
        const Grey whole_page = grey_of(page_picture(uncut, dpi));
        int crossings = 0;
        for (const Json& was : before["story"]) {
            const std::string id = was["id"].get<std::string>();
            if (id != "P3" && id != "P4") continue;
            const genko::core::StoryLine& line = *std::find_if(doc.story.begin(), doc.story.end(), [&](const genko::core::StoryLine& l) { return l.id == id; });
            // (the stroke on the page, as the book of before had it: from the corner it had)
            const double x0 = was["x_mm"].get<double>(), y0 = was["y_mm"].get<double>();
            const Json& points = was["style"]["cuts"][0]["points"];
            const double cx = line.x_mm.value() + line.w_mm.value() / 2, cy = line.y_mm.value() + line.h_mm.value() / 2;
            const double rx = line.w_mm.value() / 2, ry = line.h_mm.value() / 2;
            const auto level = [&](double x, double y) { return ((x - cx) / rx) * ((x - cx) / rx) + ((y - cy) / ry) * ((y - cy) / ry) - 1; };
            for (std::size_t k = 0; k + 1 < points.size(); ++k) {
                const double ax = x0 + points[k][0].get<double>(), ay = y0 + points[k][1].get<double>();
                const double bx = x0 + points[k + 1][0].get<double>(), by = y0 + points[k + 1][1].get<double>();
                for (int step = 0; step < 400; ++step) {
                    const double t0 = step / 400.0, t1 = (step + 1) / 400.0;
                    const double l0 = level(ax + (bx - ax) * t0, ay + (by - ay) * t0), l1 = level(ax + (bx - ax) * t1, ay + (by - ay) * t1);
                    if ((l0 < 0) == (l1 < 0)) continue;
                    // (just inside the edge there: the line's middle)
                    const double t = l0 < 0 ? t0 : t1;
                    double px = ax + (bx - ax) * t, py = ay + (by - ay) * t;
                    px += (cx - px) * 0.012, py += (cy - py) * 0.012;
                    const int ix = static_cast<int>(px / 25.4 * dpi), iy = static_cast<int>(py / 25.4 * dpi);
                    QVERIFY2(whole_page.at(ix, iy) < 100, qPrintable(QStringLiteral("%1: no line at %2, %3 without the cut").arg(QString::fromStdString(id)).arg(ix).arg(iy)));
                    QVERIFY2(cut_page.at(ix, iy) > 230, qPrintable(QStringLiteral("%1: the cut is not at %2, %3").arg(QString::fromStdString(id)).arg(ix).arg(iy)));
                    ++crossings;
                }
            }
        }
        QVERIFY2(crossings >= 3, qPrintable(QString::number(crossings)));
        // (and away from the strokes the balloons are whole: the two pages differ only near them)
        QVERIFY(cut_page.v != whole_page.v);
    }

    // A border_mm of 0 drew a hairline before and draws no line now: a book of before keeps its hairline (the line is
    // made kHairlineMm, one pixel at any resolution), a line of now with 0 has none — and a book of now that has one
    // says its balloons' generation, so the 0 is not made a hairline when it is read back.
    void aHairlineOfBeforeStaysAHairline() {
        Document old = genko::storage::load_document(old_book_dir(QStringLiteral("old_book2"))).document;
        for (const char* id : {"P5", "P6", "P7", "P8"}) {
            const genko::core::StoryLine& line = *std::find_if(old.story.begin(), old.story.end(), [&](const genko::core::StoryLine& l) { return l.id == id; });
            QCOMPARE(line.style["border_mm"], Json(genko::core::kHairlineMm));
            for (const int dpi : {150, 600}) {
                // (its outline: dark pixels along its box's left edge at its middle row — one pixel of line, about)
                Document alone = old;
                alone.story.erase(std::remove_if(alone.story.begin(), alone.story.end(), [&](const genko::core::StoryLine& l) { return l.id != id; }), alone.story.end());
                alone.story.front().text = "あ";
                const Grey g = grey_of(page_part(alone, dpi, line.x_mm.value() - 2, line.y_mm.value() + line.h_mm.value() / 2 - 0.5, 6, 1, 0));
                int dark = 0;
                double ink = 0.0;
                for (int x = 0; x < g.w; ++x) {
                    dark += g.at(x, g.h / 2) < 160 ? 1 : 0;
                    ink += (255 - g.at(x, g.h / 2)) / 255.0;
                }
                const bool second_line = std::string(id) == "P7";  // (its double line: a second hairline inside)
                QVERIFY2(dark >= 1 && ink >= 0.75 && ink <= (second_line ? 2.6 : 1.3), qPrintable(QStringLiteral("%1 at %2: %3 dark, %4 of ink").arg(id).arg(dpi).arg(dark).arg(ink)));
            }
        }
        // (a line of now: no line at all with 0)
        const Document now = one_line("box", Json{{"style", Json{{"border_mm", 0}}}, {"text", "あ"}});
        const Grey g = grey_of(page_part(now, 600, 38, 59.5, 6, 1));
        QVERIFY(std::all_of(g.v.begin(), g.v.end(), [](char c) { return static_cast<unsigned char>(c) == 255; }));
        // (…kept through a save: the book says its generation though it has no oval)
        const Json payload = payload_of(now);
        QCOMPARE(payload.value("balloon_geometry", std::int64_t{0}), genko::core::kBalloonGeometry);
        QTemporaryDir dir;
        const std::filesystem::path saved = std::filesystem::path(dir.path().toStdString()) / "now";
        std::filesystem::create_directories(saved);
        genko::test::write_project(now, saved);
        QCOMPARE(genko::storage::load_document(saved).document.story.front().style["border_mm"], Json(0.0));
    }

    // Saved, the book says its balloons' generation; read again it does not grow twice; and saved again it is the same
    // bytes. A book of now (made by this build) is read back as it was written. What the book says is read in any form
    // JSON writes a whole number in; what cannot be read opens read-only and grows nothing.
    void aBookGrowsOnlyOnce() {
        QTemporaryDir dir;
        for (const char* name : {"saved", "again", "fresh", "old_book2", "again2"}) std::filesystem::create_directories(std::filesystem::path(dir.path().toStdString()) / name);
        for (const char* name : {"old_book", "old_book2"}) {
            const Document first = genko::storage::load_document(old_book_dir(QString::fromLatin1(name))).document;
            const std::filesystem::path saved = std::filesystem::path(dir.path().toStdString()) / (std::string(name) == "old_book" ? "saved" : "old_book2");
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
                QVERIFY(second.story[i].style == first.story[i].style);
            }
            const std::filesystem::path again = std::filesystem::path(dir.path().toStdString()) / (std::string(name) == "old_book" ? "again" : "again2");
            genko::test::write_project(second, again);
            QVERIFY(genko::test::read_bytes(QString::fromStdString((again / "project.json").string())) ==
                    genko::test::read_bytes(QString::fromStdString((saved / "project.json").string())));
            // (the page drawn from the book in memory and from the book saved and read again: the same pixels)
            QVERIFY(page_picture(first, 150, 0).tobytes() == page_picture(second, 150, 0).tobytes());
        }
        const std::filesystem::path fresh = std::filesystem::path(dir.path().toStdString()) / "fresh";

        // a book of now: its ovals are not grown when it is read back
        const Document now = one_line("speech");
        genko::test::write_project(now, fresh);
        const Document back = genko::storage::load_document(fresh).document;
        QCOMPARE(back.story.front().w_mm.value(), 50.0);
        QCOMPARE(back.story.front().h_mm.value(), 40.0);
        QCOMPARE(back.story.front().x_mm.value(), 40.0);
        // (a book without an oval or a border of 0 says nothing: there is nothing a book of before is read for)
        QVERIFY(!payload_of(one_line("box")).contains("balloon_geometry"));
        QVERIFY(!payload_of(blank_book()).contains("balloon_geometry"));
        QVERIFY(payload_of(one_line("thought")).contains("balloon_geometry"));
        // (what the book says, in any form of a whole number: 2.0 is 2; a later generation is kept)
        Json said = payload_of(now);
        said["balloon_geometry"] = 2.0;
        const genko::storage::LoadResult as_float = genko::storage::load_document_payload(said, fresh);
        QVERIFY2(as_float.document.read_only_reason.empty(), as_float.document.read_only_reason.c_str());
        QCOMPARE(as_float.document.story.front().w_mm.value(), 50.0);
        said["balloon_geometry"] = 3;
        const Document read_later = genko::storage::load_document_payload(said, fresh).document;
        QCOMPARE(read_later.balloon_geometry, std::int64_t{3});
        QCOMPARE(read_later.story.front().w_mm.value(), 50.0);
        QCOMPARE(payload_of(read_later)["balloon_geometry"].get<std::int64_t>(), std::int64_t{3});
        // (1, or none: a book of before)
        said["balloon_geometry"] = 1;
        QCOMPARE(genko::storage::load_document_payload(said, fresh).document.story.front().w_mm.value(), 51.75);
        said.erase("balloon_geometry");
        QCOMPARE(genko::storage::load_document_payload(said, fresh).document.story.front().w_mm.value(), 51.75);
        // (what cannot be read — "2", 2.5, true, 0 — opens read-only and grows nothing: a book that may say 2 is never grown again)
        for (const Json& broken : {Json("2"), Json(2.5), Json(true), Json(0), Json::array({2})}) {
            said["balloon_geometry"] = broken;
            const genko::storage::LoadResult result = genko::storage::load_document_payload(said, fresh);
            QVERIFY2(!result.document.read_only_reason.empty(), broken.dump().c_str());
            QCOMPARE(result.document.story.front().w_mm.value(), 50.0);
            const bool reported = std::any_of(result.report.issues.begin(), result.report.issues.end(),
                                              [](const auto& issue) { return issue.pointer == "/balloon_geometry"; });
            QVERIFY2(reported, broken.dump().c_str());
        }
        // (a point or box text — type@1 — is not set in a balloon: its box is never grown)
        genko::core::StoryLine typed = now.story.front();
        QVERIFY(genko::core::legacy_oval(typed));
        typed.native_type = Json{{"shape", "box"}};
        QVERIFY(!genko::core::legacy_oval(typed));
        QVERIFY(!genko::core::migrate_legacy_line(typed));
        QCOMPARE(typed.w_mm.value(), 50.0);
        typed.native_type = Json{{"shape", "point"}};
        QVERIFY(!genko::core::legacy_oval(typed));
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
    // (not a test of their own, and never a SKIP: the audit of a stage's run refuses every skipped slot. As the screenshots
    // of test_gui_color, they are written at the end of a run that names a folder)
    void cleanupTestCase() {
        const QByteArray out = qgetenv("GENKO_FUKIDASHI_OUT");
        if (!out.isEmpty()) write_sample_sheets(out);
    }

private:
    static void write_sample_sheets(const QByteArray& out) {
        const std::filesystem::path dir(out.toStdString());
        std::filesystem::create_directories(dir);
        const auto write = [&](const std::string& name, const Image& image) {
            genko::storage::write_atomic(dir / name, genko::formats::png_bytes(image.convert("RGB")));
        };
        const Document doc = genko::test::clean_book();
        write("clean-150-kinds.png", page_picture(doc, 150, 0));
        write("clean-150-tails.png", page_picture(doc, 150, 1));
        write("clean-150-tails-more.png", page_picture(doc, 150, 2));
        // (parts of the 600 dpi pages, pixel for pixel: 84 mm is 1984 px)
        const struct {
            const char* name;
            std::size_t page;
            double x, y;
        } parts[] = {{"clean-600-kinds-a.png", 0, 12, 10}, {"clean-600-kinds-b.png", 0, 104, 10}, {"clean-600-kinds-c.png", 0, 12, 86},
                     {"clean-600-kinds-d.png", 0, 104, 86}, {"clean-600-kinds-e.png", 0, 12, 162}, {"clean-600-kinds-f.png", 0, 104, 162},
                     {"clean-600-kinds-g.png", 0, 12, 213}, {"clean-600-kinds-h.png", 0, 104, 213},
                     {"clean-600-tails-a.png", 1, 12, 6}, {"clean-600-tails-b.png", 1, 104, 6}, {"clean-600-tails-c.png", 1, 12, 92},
                     {"clean-600-tails-d.png", 1, 104, 92}, {"clean-600-tails-e.png", 1, 12, 178}, {"clean-600-tails-f.png", 1, 104, 178},
                     {"clean-600-tails-g.png", 2, 12, 6}, {"clean-600-tails-h.png", 2, 104, 6}, {"clean-600-tails-i.png", 2, 12, 92},
                     {"clean-600-tails-j.png", 2, 104, 92}, {"clean-600-joined.png", 2, 12, 172}, {"clean-600-joined-b.png", 2, 104, 172},
                     {"clean-600-foot-a.png", 2, 12, 210}, {"clean-600-foot-b.png", 2, 104, 210}};
        for (const auto& part : parts) write(part.name, page_part(doc, 600, part.x, part.y, 84, 84, part.page));
        // (the same pages made black and white, as the monochrome print: a part)
        write("clean-600-mono.png", genko::render::to_bitonal(page_part(doc, 600, 12, 6, 84, 84, 1)).convert("L"));
        // (the book of before, as this build draws it: beside the pictures main's build made of it)
        const Document old = genko::storage::load_document(old_book_dir()).document;
        for (std::size_t i = 0; i < old.pages.size(); ++i) write("after-" + std::to_string(i + 1) + ".png", page_picture(old, 200, i));
        const Document old2 = genko::storage::load_document(old_book_dir(QStringLiteral("old_book2"))).document;
        for (std::size_t i = 0; i < old2.pages.size(); ++i) write("after2-" + std::to_string(i + 1) + ".png", page_picture(old2, 200, i));
    }
};

QTEST_GUILESS_MAIN(TestFukidashi)
#include "test_fukidashi.moc"
