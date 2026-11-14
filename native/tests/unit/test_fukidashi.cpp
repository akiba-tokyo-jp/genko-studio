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
//   - fukidashi@2 (SPEC BALLOON-02) leaves what was there alone: every page of the two sample books (the book of before and
//     the sample of now, tests/support/fukidashi_books.hpp) at 150 and 600 dpi, and each as it is saved, are what the build
//     of before the additions drew and wrote (tests/data/fukidashi/pins, recorded from main da6acebd plus the fonts).
//
// GENKO_FUKIDASHI_OUT=<folder> also writes the sample sheets there (the pictures a person looks at).

#include <QtTest>

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QRegularExpression>

#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
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
    int at(int x, int y) const {  // (a place off the picture is the test's own mistake: a failure, never a read past it)
        if (x < 0 || y < 0 || x >= w || y >= h) throw std::out_of_range("a pixel off the picture");
        return static_cast<unsigned char>(v[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)]);
    }
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


// Whether a line drawn dark on a white picture closes round `inside` (x, y): the white that reaches the picture's edge
// does not reach it (four ways from pixel to pixel; `dark`: below this is line).
bool closed_round(const Grey& g, int inside_x, int inside_y, int dark = 128) {
    std::vector<char> seen(static_cast<std::size_t>(g.w) * static_cast<std::size_t>(g.h), 0);
    std::vector<std::pair<int, int>> stack;
    const auto visit = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= g.w || y >= g.h) return;
        char& mark = seen[static_cast<std::size_t>(y) * static_cast<std::size_t>(g.w) + static_cast<std::size_t>(x)];
        if (mark != 0 || g.at(x, y) < dark) return;
        mark = 1;
        stack.emplace_back(x, y);
    };
    for (int x = 0; x < g.w; ++x) visit(x, 0), visit(x, g.h - 1);
    for (int y = 0; y < g.h; ++y) visit(0, y), visit(g.w - 1, y);
    while (!stack.empty()) {
        const auto [x, y] = stack.back();
        stack.pop_back();
        visit(x + 1, y), visit(x - 1, y), visit(x, y + 1), visit(x, y - 1);
    }
    return seen[static_cast<std::size_t>(inside_y) * static_cast<std::size_t>(g.w) + static_cast<std::size_t>(inside_x)] == 0;
}

// A line's balloon alone (no letters) on a white picture of `w_mm` × `h_mm` at `dpi`.
Image balloon_alone(const Document& doc, int dpi, double w_mm = 140, double h_mm = 120) {
    const genko::render::Size size{static_cast<int>(w_mm / 25.4 * dpi), static_cast<int>(h_mm / 25.4 * dpi)};
    Image page = Image::create("RGB", size, genko::render::Ink{255, 255, 255});
    const genko::render::text::LetterParts balloon_only{true, false, false, false};
    genko::render::text::clear_balloon_cache();
    genko::render::text::draw_lines(genko::render::text::whole_page(page), {&doc.story.front()}, dpi, std::nullopt, false, nullptr, {}, nullptr, &balloon_only);
    return page;
}

// --- the pins of before fukidashi@2 (tests/data/fukidashi/pins) -------------------------------------------------------------

std::string sha256_of(std::string_view bytes) {
    return QCryptographicHash::hash(QByteArrayView(bytes.data(), static_cast<qsizetype>(bytes.size())), QCryptographicHash::Sha256).toHex().toStdString();
}

// A picture's mode, size and pixels as one sha256.
std::string picture_sha(const Image& image) {
    return sha256_of(std::string(image.mode()) + "|" + std::to_string(image.width()) + "x" + std::to_string(image.height()) + "|" + image.tobytes());
}

// The sample of now with ids that are the same each time (its lines' are already: C1, C2, …), so that it is saved as
// the same bytes.
Document pinned_clean_book() {
    Document doc = genko::test::clean_book();
    doc.book_id = "fedcba9876543210fedcba9876543210";
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        genko::core::Page& page = doc.edit_page(i);
        page.id = "pg" + std::to_string(i + 1);
        for (std::size_t k = 0; k < page.layers.size(); ++k) page.layers[k].id = "ly" + std::to_string(i + 1) + "_" + std::to_string(k);
        for (std::size_t k = 0; k < page.frames.size(); ++k) page.frames[k].id = "fr" + std::to_string(i + 1) + "_" + std::to_string(k);
    }
    return doc;
}

Document pinned_book(const std::string& name) {
    if (name == "additions") return genko::test::additions_book();  // (fukidashi@2's sample: its ids are fixed as it is made)
    return name == "classic" ? genko::test::classic_book(genko::core::PageSpec::a4_mono()) : pinned_clean_book();
}

// The book as it is saved (project.json's text), but for the version of the build that wrote it.
std::string saved_text(const Document& doc) {
    QTemporaryDir dir;
    genko::storage::AssetStore store(std::filesystem::path(dir.path().toStdString()));
    Json payload = genko::storage::project_payload_v4(doc, store);
    payload["writer"]["version"] = "pinned";
    return genko::core::dump_python_indent2(payload);
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

    // --- fukidashi@2: the symbols -------------------------------------------------------------------------------------------

    // A heart, a star, a flower and a diamond are their own mirror image left to right, point for point and bit for bit,
    // and touch their box's four sides — in a box of any shape, with any number of points.
    void symbolsAreTheirOwnMirrorImage() {
        std::vector<std::pair<std::string, clean::Contour>> outlines;
        for (const clean::Box& box : {clean::Box{-211.5, -160.25, 211.5, 160.25}, clean::Box{-90.0, -300.5, 90.0, 300.5}, clean::Box{-40.0, -37.0, 40.0, 37.0}}) {
            outlines.emplace_back("heart", clean::heart(box, -1.0));
            outlines.emplace_back("heart, a round point", clean::heart(box, 12.0));
            outlines.emplace_back("diamond", clean::diamond(box, -1.0));
            outlines.emplace_back("diamond, sharp", clean::diamond(box, 0.0));
            for (int points = 4; points <= 12; ++points) {
                outlines.emplace_back("star of " + std::to_string(points), clean::star(box, points, 0.62, -1.0));
                outlines.emplace_back("sharp star of " + std::to_string(points), clean::star(box, points, 0.4, 0.0));
                outlines.emplace_back("flower of " + std::to_string(points), clean::flower(box, points));
            }
        }
        for (const auto& [name, outline] : outlines) {
            const std::size_t n = outline.size();
            QVERIFY2(n >= 4, name.c_str());
            QVERIFY2(outline[0].x == 0.0, name.c_str());  // (it begins on the middle line, at its top)
            for (std::size_t i = 1; i < n; ++i) {
                const clean::P& a = outline[i];
                const clean::P& b = outline[n - i];
                QVERIFY2(a.x == -b.x && a.y == b.y, qPrintable(QStringLiteral("%1: point %2").arg(QString::fromStdString(name)).arg(i)));
            }
            QVERIFY2(clean::shoelace(outline) > 0, name.c_str());  // (clockwise on the page)
            double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
            for (const clean::P& p : outline) x0 = std::min(x0, p.x), y0 = std::min(y0, p.y), x1 = std::max(x1, p.x), y1 = std::max(y1, p.y);
            // (the box is the one whose half width the outline reaches: each of the three above)
            const double half_w = x1, half_h = (y1 - y0) / 2;
            QVERIFY2(std::fabs(x0 + x1) < 1e-9 && std::fabs(y0 + y1) < 1e-9, name.c_str());
            QVERIFY2((std::fabs(half_w - 211.5) < 1e-9 && std::fabs(half_h - 160.25) < 1e-9) || (std::fabs(half_w - 90.0) < 1e-9 && std::fabs(half_h - 300.5) < 1e-9) ||
                         (std::fabs(half_w - 40.0) < 1e-9 && std::fabs(half_h - 37.0) < 1e-9),
                     qPrintable(QStringLiteral("%1: %2 × %3").arg(QString::fromStdString(name)).arg(half_w).arg(half_h)));
        }
        // (a flower is not a cloud, and a star of four points with deep valleys is the sparkle: slim points)
        const clean::Box box{-200, -200, 200, 200};
        {  // (a flower's petals stand higher than a cloud's bumps: the valley beside the top one lies further in)
            const auto valley = [](const clean::Contour& outline) {
                double least = 1e9;
                for (const clean::P& p : outline) least = std::min(least, std::hypot(p.x, p.y));
                return least;
            };
            QVERIFY(valley(clean::flower(box, 8)) < valley(clean::cloud(box, 8)) - 10);
        }
        const clean::Contour sparkle = clean::star(box, 4, 0.4, 0.0);
        QVERIFY(!clean::inside(sparkle, {120, 120}) && clean::inside(sparkle, {0, 190}) && clean::inside(sparkle, {190, 0}));
        // (the room a symbol leaves its letters lies inside it, and holds the box's middle)
        for (const char* kind : {"heart", "star", "flower", "diamond"}) {
            for (const int points : {4, 5, 6, 9}) {
                const clean::Contour room = clean::symbol_room(kind, box, points, 0.62);
                const std::string k = kind;
                const clean::Contour outline = k == "heart" ? clean::heart(box, -1.0) : k == "diamond" ? clean::diamond(box, -1.0)
                                               : k == "star" ? clean::star(box, points, 0.62, -1.0) : clean::flower(box, points);
                QVERIFY2(room.size() >= 3 && clean::inside(room, {0, 0}), kind);
                for (const clean::P& p : room) {
                    // (a hair inside: the room's corners lie on the outline itself)
                    QVERIFY2(clean::inside(outline, {p.x * 0.98, p.y * 0.98}), qPrintable(QStringLiteral("%1 of %2: %3, %4").arg(kind).arg(points).arg(p.x).arg(p.y)));
                }
            }
        }
    }

    // A symbol's letters are set inside it at any resolution: at the size asked (5 mm — not made smaller for a box that
    // holds them), their block inside the symbol's outline.
    void theLettersFitASymbol_data() {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<int>("dpi");
        for (const char* kind : {"heart", "star", "flower", "diamond"}) {
            for (const int dpi : {150, 350, 600, 1200}) QTest::newRow(qPrintable(QStringLiteral("%1 at %2").arg(kind).arg(dpi))) << QString::fromLatin1(kind) << dpi;
        }
    }
    void theLettersFitASymbol() {
        QFETCH(QString, kind);
        QFETCH(int, dpi);
        const std::string k = kind.toStdString();
        const Document doc = one_line(k.c_str(), Json{{"x_mm", 40}, {"y_mm", 40}, {"w_mm", 46}, {"h_mm", 44}}, "すき\nだよ");
        const genko::core::StoryLine& line = doc.story.front();
        const genko::render::text::Layout layout = genko::render::text::text_layout(line, dpi);
        const double em = 5.0 / 25.4 * dpi;
        QVERIFY2(std::fabs(static_cast<double>(layout.em) - em) <= 1.0, qPrintable(QStringLiteral("em %1 of %2").arg(layout.em).arg(em)));
        const double a = 46 / 25.4 * dpi / 2, b = 44 / 25.4 * dpi / 2;
        const clean::Box box{-a, -b, a, b};
        const clean::Contour outline = k == "heart" ? clean::heart(box, -1.0) : k == "diamond" ? clean::diamond(box, -1.0)
                                       : k == "star" ? clean::star(box, 5, 0.62, -1.0) : clean::flower(box, 6);
        // (the ink's box, from the letters' picture: every corner inside the symbol)
        const std::string alpha = layout.image.getchannel(layout.image.bands() - 1).tobytes();
        int x0 = layout.image.width(), y0 = layout.image.height(), x1 = 0, y1 = 0;
        for (int y = 0; y < layout.image.height(); ++y) {
            for (int x = 0; x < layout.image.width(); ++x) {
                if (static_cast<unsigned char>(alpha[static_cast<std::size_t>(y) * static_cast<std::size_t>(layout.image.width()) + static_cast<std::size_t>(x)]) <= 40) continue;
                x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x + 1), y1 = std::max(y1, y + 1);
            }
        }
        QVERIFY(x1 > x0 && y1 > y0);
        for (const double x : {static_cast<double>(x0), static_cast<double>(x1)}) {
            for (const double y : {static_cast<double>(y0), static_cast<double>(y1)}) {
                QVERIFY2(clean::inside(outline, {layout.corner_x + x, layout.corner_y + y}),
                         qPrintable(QStringLiteral("a corner of the letters at %1, %2 is outside").arg(layout.corner_x + x).arg(layout.corner_y + y)));
            }
        }
        // (and drawn: the balloon is there — its line dark round the letters, the page white outside its box)
        const Grey g = grey_of(page_part(doc, dpi, 38, 38, 50, 48));
        int dark = 0;
        for (const char c : g.v) dark += static_cast<unsigned char>(c) < 128 ? 1 : 0;
        QVERIFY(dark > g.w);
        QVERIFY(g.at(1, 1) > 250 && g.at(g.w - 2, 1) > 250);
    }

    // fukidashi@2 is named while a line uses a symbol kind or one of the additions' keys, and not otherwise; none of them
    // names fukidashi@1 (which is named for exactly what it was), and a book may name both. A book that names a feature
    // this build does not know — as a build of before meets fukidashi@2 — is opened read-only.
    void theSecondFeatureIsNamedWhileUsed() {
        const auto names = [](const Json& payload, std::string_view feature) {
            const Json& features = payload["features"];
            return std::find(features.begin(), features.end(), Json(std::string(feature))) != features.end();
        };
        const std::string_view second = genko::core::kFukidashi2Feature;
        QCOMPARE(second, std::string_view("fukidashi@2"));
        QVERIFY(!names(payload_of(one_line("speech")), second));
        QVERIFY(!names(payload_of(genko::test::clean_book()), second));  // (the kinds and keys of fukidashi@1: not the second)
        for (const char* kind : {"heart", "star", "flower", "diamond"}) {
            const Json payload = payload_of(one_line(kind));
            QVERIFY2(names(payload, second), kind);
            QVERIFY2(!names_feature(payload), kind);
        }
        const Json styles = Json::array({Json{{"pen_swell", 0.4}}, Json{{"pen_angle", 90}}, Json{{"pen_taper", 0.5}}, Json{{"pen_strokes", 3}}, Json{{"drip_mm", 5}},
                                         Json{{"drips", 5}}, Json{{"drip_width_mm", 2}}, Json{{"drip_seed", 3}}, Json{{"points", 6}},
                                         Json{{"fill_tone", Json{{"density", 0.2}}}}, Json{{"fill_gradient", Json{{"to", Json::array({200, 220, 255})}}}}});
        for (const Json& style : styles) {
            Document doc = one_line("speech", Json{{"style", style}});
            QVERIFY2(names(payload_of(doc), second), style.dump().c_str());
            QVERIFY2(!names_feature(payload_of(doc)), style.dump().c_str());
            // (the key taken away again, the feature goes)
            doc = run_ops(doc, Json::array({Json{{"op", "edit_line"}, {"id", "L1"}, {"style", Json{{style.begin().key(), nullptr}}}}}));
            QVERIFY2(!names(payload_of(doc), second), style.dump().c_str());
        }
        Document doc = one_line("heart");
        doc = run_ops(doc, Json::array({Json{{"op", "edit_line"}, {"id", "L1"}, {"balloon", "speech"}}}));
        QVERIFY(!names(payload_of(doc), second));
        // (both at once; read back by this build for writing, both still named)
        const Document both = one_line("capsule", Json{{"style", Json{{"wave_mm", 0.3}, {"pen_swell", 0.5}}}});
        QVERIFY(names(payload_of(both), second) && names_feature(payload_of(both)));
        QTemporaryDir dir;
        const std::filesystem::path path(dir.path().toStdString());
        genko::test::write_project(one_line("star", Json{{"style", Json{{"points", 4}, {"drip_mm", 4}}}}), path);
        const genko::storage::LoadResult read = genko::storage::load_document(path);
        QVERIFY2(read.document.read_only_reason.empty(), read.document.read_only_reason.c_str());
        QVERIFY(names(payload_of(read.document), second));
        QCOMPARE(read.document.story.front().balloon, std::string("star"));
        QCOMPARE(read.document.story.front().style["points"], Json(4));
        // (a type layer's record counts as a line does: a symbol there, or one of the keys, names the feature too)
        for (const Json& more : {Json{{"balloon", "heart"}}, Json{{"balloon", "speech"}, {"style", Json{{"size_mm", 8}, {"pen_swell", 0.5}}}}}) {
            Json op{{"op", "add_type_layer"}, {"page", 1}, {"text", "すき"}, {"box", Json::array({20, 20, 40, 36})}, {"new_id", "t1"}};
            for (const auto& [key, value] : more.items()) op[key] = value;
            const Document typed = run_ops(blank_book(), Json::array({op}));
            QVERIFY(typed.story.empty());
            QVERIFY2(names(payload_of(typed), second), more.dump().c_str());
        }
        {
            const Document typed = run_ops(blank_book(), Json::array({Json{{"op", "add_type_layer"}, {"page", 1}, {"text", "すき"}, {"box", Json::array({20, 20, 40, 36})},
                                                                        {"new_id", "t1"}, {"balloon", "speech"}}}));
            QVERIFY(!names(payload_of(typed), second));
        }
        // (a feature this build does not know: read-only, with the reason)
        Json payload = Json::parse(genko::test::read_bytes(QString::fromStdString((path / "project.json").string())));
        payload["features"].push_back("fukidashi@3");
        genko::storage::write_atomic(path / "project.json", payload.dump(2));
        const genko::storage::LoadResult unknown = genko::storage::load_document(path);
        QVERIFY(!unknown.document.read_only_reason.empty());
    }

    // --- fukidashi@2: a tone and a blend inside the outline -----------------------------------------------------------------

    // style.fill_tone lays a screentone inside any outline: its ink covers the density asked, within two hundredths — dots
    // at 10, 30, 50, 70 and 90%, lines at 20 and 60%, at 600 dpi and (a coarser lattice) at 150 —, in the line's colour.
    void aToneCoversItsDensity_data() {
        QTest::addColumn<QString>("pattern");
        QTest::addColumn<double>("density");
        QTest::addColumn<double>("lpi");
        QTest::addColumn<double>("angle");
        QTest::addColumn<int>("dpi");
        for (const double density : {0.1, 0.3, 0.5, 0.7, 0.9}) {
            QTest::newRow(qPrintable(QStringLiteral("dots %1 at 600").arg(density))) << QStringLiteral("dot") << density << 60.0 << 45.0 << 600;
        }
        QTest::newRow("dots 0.3 at 150") << QStringLiteral("dot") << 0.3 << 30.0 << 45.0 << 150;
        QTest::newRow("dots 0.3 at 1200") << QStringLiteral("dot") << 0.3 << 60.0 << 45.0 << 1200;
        QTest::newRow("dots 0.4 at 15 degrees") << QStringLiteral("dot") << 0.4 << 50.0 << 15.0 << 600;
        QTest::newRow("dots 0.3, each drawn where it lies") << QStringLiteral("dot") << 0.3 << 20.0 << 30.0 << 600;  // (26, 15: no period within 512 px)
        QTest::newRow("lines 0.2") << QStringLiteral("line") << 0.2 << 60.0 << 45.0 << 600;
        QTest::newRow("lines 0.6 across") << QStringLiteral("line") << 0.6 << 40.0 << 0.0 << 600;
        QTest::newRow("lines 0.35 at 30 degrees") << QStringLiteral("line") << 0.35 << 60.0 << 30.0 << 600;
        QTest::newRow("lines 0.5 at 150") << QStringLiteral("line") << 0.5 << 30.0 << 45.0 << 150;
    }
    void aToneCoversItsDensity() {
        QFETCH(QString, pattern);
        QFETCH(double, density);
        QFETCH(double, lpi);
        QFETCH(double, angle);
        QFETCH(int, dpi);
        const Json tone{{"density", density}, {"lpi", lpi}, {"angle", angle}, {"pattern", pattern.toStdString()}};
        const Document doc = one_line("box", Json{{"style", Json{{"fill_tone", tone}, {"line_rgb", Json::array({0, 0, 0})}}}});
        const genko::render::text::LetterParts balloon_only{true, false, false, false};
        const genko::render::Size size{static_cast<int>(100 / 25.4 * dpi), static_cast<int>(90 / 25.4 * dpi)};
        Image page = Image::create("RGB", size, genko::render::Ink{255, 255, 255});
        genko::render::text::clear_balloon_cache();
        genko::render::text::draw_lines(genko::render::text::whole_page(page), {&doc.story.front()}, dpi, std::nullopt, false, nullptr, {}, nullptr, &balloon_only);
        // (well inside the box: 46..84 × 46..74 mm of its 40..90 × 40..80)
        const auto px = [dpi](double mm) { return static_cast<int>(std::lround(mm / 25.4 * dpi)); };
        const Grey g = grey_of(page.crop(genko::render::Box{px(46), px(46), px(84), px(74)}));
        double dark = 0;
        for (const char c : g.v) dark += (255 - static_cast<unsigned char>(c)) / 255.0;
        const double share = dark / (static_cast<double>(g.w) * g.h);
        QVERIFY2(std::fabs(share - density) <= 0.02, qPrintable(QStringLiteral("%1 of %2").arg(share).arg(density)));
    }

    // A tone that grows darker (fill_tone's `to`) does: along its way — down the page when none is said, to the right for
    // dir 0, from the middle outward for radial — each tenth of the balloon is darker than the one before; and it ends
    // where it is told, light at the one end and dark at the other.
    void aToneGrowsDarkerAlongItsWay_data() {
        QTest::addColumn<QString>("pattern");
        QTest::addColumn<QString>("way");
        for (const char* pattern : {"dot", "line"}) {
            for (const char* way : {"down", "right", "up", "out"}) QTest::newRow(qPrintable(QStringLiteral("%1 %2").arg(pattern, way))) << QString::fromLatin1(pattern) << QString::fromLatin1(way);
        }
    }
    void aToneGrowsDarkerAlongItsWay() {
        QFETCH(QString, pattern);
        QFETCH(QString, way);
        Json tone{{"density", 0.05}, {"to", 0.7}, {"pattern", pattern.toStdString()}};
        if (way == "right") tone["dir"] = 0;
        if (way == "up") tone["dir"] = 270;
        if (way == "out") tone["radial"] = true;
        const int dpi = 600;
        const Document doc = one_line("box", Json{{"style", Json{{"fill_tone", tone}, {"line_rgb", Json::array({0, 0, 0})}}}});
        const genko::render::text::LetterParts balloon_only{true, false, false, false};
        const genko::render::Size size{static_cast<int>(100 / 25.4 * dpi), static_cast<int>(90 / 25.4 * dpi)};
        Image page = Image::create("RGB", size, genko::render::Ink{255, 255, 255});
        genko::render::text::clear_balloon_cache();
        genko::render::text::draw_lines(genko::render::text::whole_page(page), {&doc.story.front()}, dpi, std::nullopt, false, nullptr, {}, nullptr, &balloon_only);
        const auto px = [dpi](double mm) { return static_cast<int>(std::lround(mm / 25.4 * dpi)); };
        const Grey g = grey_of(page.crop(genko::render::Box{px(41), px(41), px(89), px(79)}));  // (inside the line)
        // (ten bands along the way; for "out", ten rings of the box's ellipse)
        std::array<double, 10> dark{}, count{};
        for (int y = 0; y < g.h; ++y) {
            for (int x = 0; x < g.w; ++x) {
                const double u = (x + 0.5) / g.w, v = (y + 0.5) / g.h;
                double t = way == "right" ? u : way == "up" ? 1 - v : way == "down" ? v : std::hypot(2 * u - 1, 2 * v - 1);
                if (t >= 1.0) continue;
                const auto band = static_cast<std::size_t>(t * 10);
                dark[band] += (255 - g.at(x, y)) / 255.0;
                count[band] += 1;
            }
        }
        for (std::size_t k = 0; k < 10; ++k) dark[k] /= std::max(1.0, count[k]);
        for (std::size_t k = 1; k < 10; ++k) {
            QVERIFY2(dark[k] > dark[k - 1] + 0.03, qPrintable(QStringLiteral("band %1: %2 after %3").arg(k).arg(dark[k]).arg(dark[k - 1])));
        }
        QVERIFY2(dark[0] < 0.12 && dark[9] > 0.6 && dark[9] < 0.72, qPrintable(QStringLiteral("%1 .. %2").arg(dark[0]).arg(dark[9])));
    }

    // A tone lies inside any outline, under the letters and clear of nothing but the line: inside a heart there are dots,
    // outside it none, the line as it was; with inset_mm white is left inside the line; a tone box with a fill_tone has
    // that tone and not its own dots; and the dots of two balloons with one tone lie on one lattice of the page — the
    // same balloon a whole number of the lattice's periods away is the same pixels.
    void aToneLiesInsideAnyOutline() {
        const Json tone{{"density", 0.3}};
        const genko::render::text::LetterParts balloon_only{true, false, false, false};
        const auto drawn = [&](const Document& doc, int dpi) {
            const genko::render::Size size{static_cast<int>(140 / 25.4 * dpi), static_cast<int>(120 / 25.4 * dpi)};
            Image page = Image::create("RGB", size, genko::render::Ink{255, 255, 255});
            genko::render::text::clear_balloon_cache();
            genko::render::text::draw_lines(genko::render::text::whole_page(page), {&doc.story.front()}, dpi, std::nullopt, false, nullptr, {}, nullptr, &balloon_only);
            return page;
        };
        for (const char* kind : {"speech", "heart", "shout", "cloud", "star", "rounded", "capsule"}) {
            const Document plain = one_line(kind, Json{{"tail", Json::array({100, 95})}});
            const Document toned = one_line(kind, Json{{"tail", Json::array({100, 95})}, {"style", Json{{"fill_tone", tone}}}});
            const Grey a = grey_of(drawn(plain, 300)), b = grey_of(drawn(toned, 300));
            int inside_dark = 0, lighter = 0;
            for (int y = 0; y < a.h; ++y) {
                for (int x = 0; x < a.w; ++x) {
                    const int was = a.at(x, y), is = b.at(x, y);
                    if (is > was) ++lighter;                      // (a tone only adds ink)
                    if (was > 250 && is < 128) ++inside_dark;      // (a dot where the fill was white)
                }
            }
            QVERIFY2(lighter == 0, kind);
            QVERIFY2(inside_dark > 2000, qPrintable(QStringLiteral("%1: %2").arg(kind).arg(inside_dark)));
            // (the page's corner — outside every balloon — is white, and the line's own pixels — all line: its colour,
            // 20 — are as they were)
            QVERIFY(b.at(3, 3) == 255);
            for (int y = 0; y < a.h; ++y) {
                for (int x = 0; x < a.w; ++x) {
                    if (a.at(x, y) == 20) QVERIFY2(b.at(x, y) == 20, kind);
                }
            }
        }
        {  // (white left inside the line: the pixels within a millimetre of the box's edge are white, further in there are dots)
            const Document inset = one_line("box", Json{{"style", Json{{"fill_tone", Json{{"density", 0.3}, {"inset_mm", 1.5}}}}}});
            const Grey g = grey_of(drawn(inset, 300));
            const auto px = [](double mm) { return static_cast<int>(std::lround(mm / 25.4 * 300)); };
            int near_dark = 0, deep_dark = 0;
            for (int y = px(50); y < px(70); ++y) {
                for (int x = px(40.7); x < px(41.6); ++x) near_dark += g.at(x, y) < 200 ? 1 : 0;
                for (int x = px(43); x < px(46); ++x) deep_dark += g.at(x, y) < 128 ? 1 : 0;
            }
            QCOMPARE(near_dark, 0);
            QVERIFY(deep_dark > 100);
        }
        {  // (a tone box: the tone asked instead of its own)
            const Document own = one_line("tone_box");
            const Document asked = one_line("tone_box", Json{{"style", Json{{"fill_tone", Json{{"density", 0.5}, {"pattern", "line"}}}}}});
            const Grey a = grey_of(drawn(own, 300).crop(genko::render::Box{560, 560, 900, 800})), b = grey_of(drawn(asked, 300).crop(genko::render::Box{560, 560, 900, 800}));
            double da = 0, db = 0;
            for (const char c : a.v) da += (255 - static_cast<unsigned char>(c)) / 255.0;
            for (const char c : b.v) db += (255 - static_cast<unsigned char>(c)) / 255.0;
            QVERIFY2(da / a.v.size() < 0.16 && db / b.v.size() > 0.4, qPrintable(QStringLiteral("%1, %2").arg(da / a.v.size()).arg(db / b.v.size())));
        }
        {  // (one lattice for the page: at 600 dpi, 60 lines at 45° is the vector (7, 7) — the same dots every 14 px.
           // A balloon 14 × 20 px to the right is the same pixels, moved; one 5 px to the right is not)
            const auto at_x = [&](double x_px) {
                const Document doc = one_line("box", Json{{"x_mm", x_px / 600 * 25.4}, {"y_mm", 1000.0 / 600 * 25.4}, {"w_mm", 30}, {"h_mm", 20}, {"style", Json{{"fill_tone", tone}}}});
                return drawn(doc, 600);
            };
            const Image here = at_x(900), there = at_x(900 + 280), off = at_x(905);
            const genko::render::Box a{800, 900, 1800, 1600};
            QVERIFY(here.crop(a).tobytes() == there.crop(genko::render::Box{a.x0 + 280, a.y0, a.x1 + 280, a.y1}).tobytes());
            QVERIFY(here.crop(a).tobytes() != off.crop(genko::render::Box{a.x0 + 5, a.y0, a.x1 + 5, a.y1}).tobytes());
        }
    }

    // The letters over a tone get a white edge, as a tone box's do (and none when the line says its own outline, or has no
    // balloon for the tone to lie in).
    void theLettersOverAToneHaveAWhiteEdge() {
        const auto layout = [](const char* kind, Json style) {
            const Document doc = one_line(kind, Json{{"style", std::move(style)}}, "あ");
            return genko::render::text::text_layout(doc.story.front(), 600);
        };
        const Json tone{{"fill_tone", Json{{"density", 0.3}}}};
        const auto plain = layout("speech", Json::object());
        const auto toned = layout("speech", tone);
        const std::int64_t grow = std::max<std::int64_t>(2, plain.em / 7);
        QCOMPARE(toned.image.width(), plain.image.width() + 2 * static_cast<int>(grow + 1));
        QCOMPARE(layout("tone_box", Json::object()).image.width(), layout("tone_box", tone).image.width());
        QCOMPARE(layout("sfx", tone).image.width(), layout("sfx", Json::object()).image.width());
        QCOMPARE(layout("none", tone).image.width(), layout("none", Json::object()).image.width());
        // (the edge is white: somewhere in the letters' picture a white pixel that is there — and none without the tone)
        const auto whites = [](const Image& image) {
            const std::string bytes = image.convert("RGBA").tobytes();
            int n = 0;
            for (std::size_t i = 0; i < bytes.size() / 4; ++i) {
                n += static_cast<unsigned char>(bytes[i * 4 + 3]) > 200 && static_cast<unsigned char>(bytes[i * 4]) > 240 ? 1 : 0;
            }
            return n;
        };
        QVERIFY(whites(toned.image) > 500);
        QCOMPARE(whites(plain.image), 0);
        // (the editable PSD gives a halo as its text layer's stroke: the edge over a tone is one, as wide and as white as
        // a tone box's — and a line without a tone has none)
        const auto stroke = [](const char* kind, Json style) {
            const Document doc = one_line(kind, Json{{"style", std::move(style)}}, "あ");
            return genko::render::text::set_layout(doc.story.front(), 600, std::nullopt, genko::render::text::LetterParts{false, true, false, false}).set.halo;
        };
        const auto over_tone = stroke("speech", tone), of_box = stroke("tone_box", Json::object());
        QVERIFY(over_tone.has_value() && of_box.has_value());
        QVERIFY(!stroke("speech", Json::object()).has_value());
        QCOMPARE(over_tone->first, of_box->first);
        QVERIFY(over_tone->second == of_box->second);
    }

    // style.fill_gradient: the fill runs from its own colour to another — down the page, from the fill's colour at the
    // top to the other at the foot, each row between them in turn; from white when no colour is said; on a picture with
    // transparency (a balloon's own layer) and on one of another kind the same colours.
    void aBlendRunsFromOneColourToTheOther() {
        const genko::render::text::LetterParts balloon_only{true, false, false, false};
        const Document doc = one_line("box", Json{{"style", Json{{"fill_rgb", Json::array({255, 240, 100})}, {"fill_gradient", Json{{"to", Json::array({40, 90, 250})}}}}}});
        const int dpi = 300;
        const genko::render::Size size{static_cast<int>(100 / 25.4 * dpi), static_cast<int>(90 / 25.4 * dpi)};
        const auto px = [dpi](double mm) { return static_cast<int>(std::lround(mm / 25.4 * dpi)); };
        Image page = Image::create("RGB", size, genko::render::Ink{255, 255, 255});
        genko::render::text::clear_balloon_cache();
        genko::render::text::draw_lines(genko::render::text::whole_page(page), {&doc.story.front()}, dpi, std::nullopt, false, nullptr, {}, nullptr, &balloon_only);
        const std::string rgb = page.tobytes();
        const auto at = [&](int x, int y) {
            const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(page.width()) + static_cast<std::size_t>(x)) * 3;
            return std::array<int, 3>{static_cast<unsigned char>(rgb[i]), static_cast<unsigned char>(rgb[i + 1]), static_cast<unsigned char>(rgb[i + 2])};
        };
        const int x = px(65), top = px(40.6), foot = px(79.4);
        const auto first = at(x, top), last = at(x, foot);
        QVERIFY2(std::abs(first[0] - 255) <= 4 && std::abs(first[1] - 240) <= 4 && std::abs(first[2] - 100) <= 4, qPrintable(QStringLiteral("%1 %2 %3").arg(first[0]).arg(first[1]).arg(first[2])));
        QVERIFY2(std::abs(last[0] - 40) <= 4 && std::abs(last[1] - 90) <= 4 && std::abs(last[2] - 250) <= 4, qPrintable(QStringLiteral("%1 %2 %3").arg(last[0]).arg(last[1]).arg(last[2])));
        for (int y = top + 1; y <= foot; ++y) {  // (red and green fall, blue rises, never the other way; the same along a row)
            const auto here = at(x, y), above = at(x, y - 1);
            QVERIFY(here[0] <= above[0] && here[1] <= above[1] && here[2] >= above[2]);
            QVERIFY(at(px(45), y) == here);
        }
        const auto middle = at(x, (top + foot) / 2);
        QVERIFY(std::abs(middle[0] - 148) <= 3 && std::abs(middle[2] - 175) <= 3);
        // (no colour said: from white)
        const Document white = one_line("speech", Json{{"style", Json{{"fill_gradient", Json{{"to", Json::array({0, 0, 0})}, {"dir", 0}}}}}});
        Image second = Image::create("RGB", size, genko::render::Ink{255, 255, 255});
        genko::render::text::clear_balloon_cache();
        genko::render::text::draw_lines(genko::render::text::whole_page(second), {&white.story.front()}, dpi, std::nullopt, false, nullptr, {}, nullptr, &balloon_only);
        const Grey g = grey_of(second);
        QVERIFY(g.at(px(42), px(60)) > 235 && g.at(px(88), px(60)) < 30 && std::abs(g.at(px(65), px(60)) - 128) <= 4);
        // (on its own layer — transparent where the balloon is not — and on a grey picture: the same colours)
        Image layer = Image::create("RGBA", size, genko::render::Ink{0, 0, 0, 0});
        genko::render::text::draw_lines(genko::render::text::whole_page(layer), {&doc.story.front()}, dpi, std::nullopt, false, nullptr, {}, nullptr, &balloon_only);
        const std::string rgba = layer.tobytes();
        for (const int y : {top, (top + foot) / 2, foot}) {
            const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(layer.width()) + static_cast<std::size_t>(x)) * 4;
            const auto here = at(x, y);
            QVERIFY(static_cast<unsigned char>(rgba[i + 3]) == 255);
            for (std::size_t c = 0; c < 3; ++c) QVERIFY(std::abs(static_cast<unsigned char>(rgba[i + c]) - here[c]) <= 1);
        }
        // (a picture of another kind with three colours — the fill is laid as a picture of the blend, brought to its kind)
        Image other = Image::create("YCbCr", size, genko::render::Ink{255, 128, 128});
        genko::render::text::draw_lines(genko::render::text::whole_page(other), {&doc.story.front()}, dpi, std::nullopt, false, nullptr, {}, nullptr, &balloon_only);
        const std::string back = other.convert("RGB").tobytes();
        for (const int y : {top, (top + foot) / 2, foot}) {
            const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(other.width()) + static_cast<std::size_t>(x)) * 3;
            const auto here = at(x, y);
            for (std::size_t c = 0; c < 3; ++c) {
                QVERIFY2(std::abs(static_cast<unsigned char>(back[i + c]) - here[c]) <= 3, qPrintable(QStringLiteral("row %1 band %2: %3 for %4").arg(y).arg(c).arg(static_cast<unsigned char>(back[i + c])).arg(here[c])));
            }
        }
    }

    // A tone printed (the page made black and white at 600 and at 1200 dpi): its dots are whole — no black pixel alone,
    // and about the density asked is black.
    void aPrintedToneIsClean_data() {
        QTest::addColumn<int>("dpi");
        QTest::addColumn<double>("density");
        for (const int dpi : {600, 1200}) {
            for (const double density : {0.1, 0.3, 0.5}) QTest::newRow(qPrintable(QStringLiteral("%1 at %2").arg(density).arg(dpi))) << dpi << density;
        }
    }
    void aPrintedToneIsClean() {
        QFETCH(int, dpi);
        QFETCH(double, density);
        const Document doc = one_line("speech", Json{{"style", Json{{"fill_tone", Json{{"density", density}}}, {"line_rgb", Json::array({0, 0, 0})}}}}, "あ");
        const Grey g = grey_of(genko::render::to_bitonal(page_part(doc, dpi, 46, 46, 12, 28)));  // (inside the oval, clear of the letter)
        int black = 0, alone = 0;
        for (int y = 1; y + 1 < g.h; ++y) {
            for (int x = 1; x + 1 < g.w; ++x) {
                if (g.at(x, y) >= 128) continue;
                ++black;
                if (g.at(x - 1, y) >= 128 && g.at(x + 1, y) >= 128 && g.at(x, y - 1) >= 128 && g.at(x, y + 1) >= 128) ++alone;
            }
        }
        const double share = static_cast<double>(black) / ((g.w - 2) * (g.h - 2));
        QCOMPARE(alone, 0);
        // (black is every pixel a dot covers 30% of or more — grey above 180 is white —: each dot a fifth of a pixel bigger
        // all round, so up to a fifth more than the dots' own area at 600 dpi)
        QVERIFY2(share > density - 0.02 && share < density * 1.2 + 0.02, qPrintable(QStringLiteral("%1 of %2").arg(share).arg(density)));
    }

    // --- fukidashi@2: drips ----------------------------------------------------------------------------------------------------

    // style.drip_mm: the balloon's body takes the box less that much of its foot, and drips run down from it — never past
    // the box: nothing is drawn below the box's foot or beside it, the longest drip reaches the foot, and above the
    // drips' roots the balloon is the balloon of the body's box alone, pixel for pixel.
    void dripsHangInsideTheBox_data() {
        QTest::addColumn<QString>("kind");
        for (const char* kind : {"speech", "box", "rounded", "capsule", "polygon", "shout", "cloud", "heart", "star", "diamond", "narration"}) QTest::newRow(kind) << QString::fromLatin1(kind);
    }
    void dripsHangInsideTheBox() {
        QFETCH(QString, kind);
        const std::string k = kind.toStdString();
        const int dpi = 300;
        // (the box 40..90 × 40..88 mm, drips of 8 mm: the body 40..80)
        const Document dripping = one_line(k.c_str(), Json{{"h_mm", 48}, {"style", Json{{"drip_mm", 8}, {"drip_seed", 5}}}});
        const Document body = one_line(k.c_str(), Json{{"h_mm", 40}});
        const Grey with = grey_of(balloon_alone(dripping, dpi)), without = grey_of(balloon_alone(body, dpi));
        const auto px = [dpi](double mm) { return static_cast<int>(std::lround(mm / 25.4 * dpi)); };
        const int x0 = px(40), x1 = px(90), y1 = px(88), body_foot = px(80);
        int below = 0, beside = 0, hanging = 0, lowest = 0;
        for (int y = 0; y < with.h; ++y) {
            for (int x = 0; x < with.w; ++x) {
                if (with.at(x, y) == 255) continue;
                if (y >= y1) ++below;
                if (x < x0 || x >= x1) ++beside;
                if (y >= body_foot + 2) ++hanging, lowest = std::max(lowest, y);
            }
        }
        QCOMPARE(below, 0);
        QCOMPARE(beside, 0);
        QVERIFY2(hanging > 200, qPrintable(QString::number(hanging)));
        QVERIFY2(lowest >= y1 - 2, qPrintable(QStringLiteral("the longest drip ends at %1 of %2").arg(lowest).arg(y1)));
        // (the upper three fifths of the body: as without drips)
        const int upto = px(40 + 40 * 0.6);
        int unlike = 0;
        for (int y = 0; y < upto; ++y) {
            for (int x = 0; x < with.w; ++x) unlike += with.at(x, y) != without.at(x, y) ? 1 : 0;
        }
        QCOMPARE(unlike, 0);
        // (and the line closes round the balloon: no gap where a drip joins it)
        QVERIFY(closed_round(with, px(65), px(60)));
    }

    // The drips are the line's own: the same line draws the same drips wherever it lies (300 px to the right: the same
    // pixels, moved) and each time; another seed draws others; and nothing but the seed arranges them — a line with
    // another id draws the same.
    void theSameSeedDrawsTheSameDrips() {
        const int dpi = 300;
        const auto drawn = [&](double x_mm, int seed, const char* id = "L1") {
            Document doc = one_line("speech", Json{{"id", id}, {"x_mm", x_mm}, {"w_mm", 50.8}, {"h_mm", 48}, {"style", Json{{"drip_mm", 8}, {"drip_seed", seed}}}});
            return balloon_alone(doc, dpi, 8.5 * 25.4, 5 * 25.4);
        };
        const Image here = drawn(42.333333333333336, 3);   // (500 px)
        const Image there = drawn(67.73333333333333, 3);   // (800 px: 300 px to the right)
        const genko::render::Box a{400, 0, 1200, here.height()};
        const genko::render::Box b{700, 0, 1500, there.height()};
        QVERIFY(here.crop(a).tobytes() == there.crop(b).tobytes());
        QVERIFY(drawn(42.333333333333336, 3).tobytes() == here.tobytes());
        QVERIFY(drawn(42.333333333333336, 3, "another").tobytes() == here.tobytes());
        QVERIFY(drawn(42.333333333333336, 4).tobytes() != here.tobytes());
        QVERIFY(drawn(42.333333333333336, 0).tobytes() != here.tobytes());
        // (no seed said is seed 0)
        const Document unsaid = one_line("speech", Json{{"x_mm", 42.333333333333336}, {"w_mm", 50.8}, {"h_mm", 48}, {"style", Json{{"drip_mm", 8}}}});
        QVERIFY(balloon_alone(unsaid, dpi, 8.5 * 25.4, 5 * 25.4).tobytes() == drawn(42.333333333333336, 0).tobytes());
    }

    // Whatever the seed, the number and the width, the line closes round the balloon and its drips — no gap where a drip
    // joins the outline, no drip left open —, on a smooth outline and on one with corners; and a line that says how many
    // drips and how wide has them so: more drips are more ink below the body.
    void dripsNeverOpenTheLine_data() {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<int>("seed");
        for (const char* kind : {"speech", "rounded", "heart", "shout", "box"}) {
            for (int seed = 0; seed < 24; ++seed) QTest::newRow(qPrintable(QStringLiteral("%1, seed %2").arg(kind).arg(seed))) << QString::fromLatin1(kind) << seed;
        }
    }
    void dripsNeverOpenTheLine() {
        QFETCH(QString, kind);
        QFETCH(int, seed);
        const int dpi = 300;
        const auto px = [dpi](double mm) { return static_cast<int>(std::lround(mm / 25.4 * dpi)); };
        Json style{{"drip_mm", 4.0 + seed % 5 * 2.0}, {"drip_seed", seed}};
        if (seed % 3 == 1) style["drips"] = 3 + seed % 9;
        if (seed % 4 == 2) style["drip_width_mm"] = 0.8 + seed % 6 * 0.6;
        if (seed % 7 == 3) style["border_mm"] = 0.7;
        const Document doc = one_line(kind.toStdString().c_str(), Json{{"h_mm", 50}, {"style", style}});
        const Grey g = grey_of(balloon_alone(doc, dpi));
        QVERIFY2(closed_round(g, px(65), px(55)), style.dump().c_str());
        // (below the body the white inside a drip is closed in too: the white the edge reaches has no ink outside the box)
        for (int y = px(90) + 1; y < g.h; ++y) QVERIFY(g.at(px(65), y) == 255);
    }

    // --- fukidashi@2: the pen's line -------------------------------------------------------------------------------------------

    // 強弱: with pen_swell s the line is width × (1 + s) where its edge faces the heavy way, width × (1 − s) where it faces
    // the other, and the width itself where it runs along it — the coverage summed across the line, within half a pixel:
    // on a circle and an ellipse, the heavy way down and to the right (45°, when none is said), down, and to the left.
    void thePensLineSwellsOnItsHeavySide_data() {
        QTest::addColumn<double>("rx");
        QTest::addColumn<double>("ry");
        QTest::addColumn<double>("width");
        QTest::addColumn<double>("swell");
        QTest::addColumn<double>("angle");
        QTest::newRow("circle, 8 px, 0.5") << 220.0 << 220.0 << 8.0 << 0.5 << 45.0;
        QTest::newRow("circle, 8 px, 0.9 down") << 220.0 << 220.0 << 8.0 << 0.9 << 90.0;
        QTest::newRow("ellipse, 8 px, 0.45") << 260.0 << 170.0 << 8.0 << 0.45 << 45.0;
        QTest::newRow("ellipse, 12 px, 0.6 left") << 260.0 << 170.0 << 12.0 << 0.6 << 180.0;
        QTest::newRow("ellipse, 3 px, 0.3") << 130.0 << 90.0 << 3.0 << 0.3 << 300.0;
    }
    void thePensLineSwellsOnItsHeavySide() {
        QFETCH(double, rx);
        QFETCH(double, ry);
        QFETCH(double, width);
        QFETCH(double, swell);
        QFETCH(double, angle);
        const int w = 600, h = 520;
        const clean::P centre{300.0, 260.0};
        const std::vector<clean::Shape> shapes{clean::whole(clean::ellipse(clean::Box{centre.x - rx, centre.y - ry, centre.x + rx, centre.y + ry}))};
        const clean::Plane fill = clean::fill_plane(shapes, w, h);
        clean::Pen pen;
        pen.width = width;
        pen.swell = swell;
        pen.heavy = {std::cos(angle * 3.141592653589793 / 180), std::sin(angle * 3.141592653589793 / 180)};
        const std::vector<clean::PenRun> runs = clean::pen_runs(clean::edge_runs(shapes), shapes, pen);
        QCOMPARE(runs.size(), std::size_t{1});
        QVERIFY(runs.front().closed);
        clean::Plane band = clean::pen_plane(runs, w, h);
        for (std::size_t i = 0; i < band.values.size(); ++i) band.values[i] *= fill.values[i];
        for (const double degrees : {0.0, 30.0, 45.0, 90.0, 135.0, 180.0, 225.0, 270.0, 315.0}) {
            const double a = degrees * 3.141592653589793 / 180;
            const clean::P on{centre.x + rx * std::cos(a), centre.y + ry * std::sin(a)};
            double nx = std::cos(a) / rx, ny = std::sin(a) / ry;  // (the ellipse's outward normal there)
            const double len = std::hypot(nx, ny);
            nx /= len, ny /= len;
            double sum = 0.0;
            const double step = 0.125;
            for (double d = -width * 2.5 - 3; d <= 3; d += step) sum += plane_at(band, on.x + nx * d, on.y + ny * d) * step;
            // (never under a pixel and a half: the line's thinnest)
            const double expected = std::max(std::min(1.5, width), width * (1 + swell * (nx * pen.heavy.x + ny * pen.heavy.y)));
            QVERIFY2(std::fabs(sum - expected) <= 0.5, qPrintable(QStringLiteral("%1°: %2 for %3").arg(degrees).arg(sum).arg(expected)));
        }
        // (without a swell or a taper the pen's line is the even line, within a hundredth of a pixel's coverage)
        clean::Pen even;
        even.width = width;
        const clean::Plane plain = clean::ribbon_plane(clean::edge_runs(shapes), width, w, h);
        const clean::Plane same = clean::pen_plane(clean::pen_runs(clean::edge_runs(shapes), shapes, even), w, h);
        double worst = 0.0;
        for (std::size_t i = 0; i < plain.values.size(); ++i) worst = std::max(worst, static_cast<double>(std::fabs(plain.values[i] * fill.values[i] - same.values[i] * fill.values[i])));
        QVERIFY2(worst < 0.02, qPrintable(QString::number(worst)));
    }

    // 入り抜き: each stroke thins toward its two ends and is its full width between them — the share of the stroke that
    // thins is pen_taper —; a smooth outline is drawn in pen_strokes strokes, beginning where its line is lightest (up and
    // to the left, for the heavy way of 45°); an outline with corners from corner to corner; a stroke shorter than six
    // widths is not thinned; and nowhere is the line thinner than a pixel and a half: it never opens.
    void aStrokeThinsTowardItsEnds() {
        const clean::P centre{300.0, 260.0};
        const std::vector<clean::Shape> oval{clean::whole(clean::ellipse(clean::Box{centre.x - 220, centre.y - 220, centre.x + 220, centre.y + 220}))};
        const double total = 2 * 3.141592653589793 * 220;
        for (const int strokes : {1, 2, 3, 4, 8}) {
            for (const double taper : {0.3, 0.6, 1.0}) {
                clean::Pen pen;
                pen.width = 12.0;
                pen.taper = taper;
                pen.strokes = strokes;
                pen.heavy = {std::sqrt(0.5), std::sqrt(0.5)};
                const std::vector<clean::PenRun> runs = clean::pen_runs(clean::edge_runs(oval), oval, pen);
                QCOMPARE(runs.size(), std::size_t{1});
                const clean::PenRun& run = runs.front();
                // (thin places: as many as strokes, the first up and to the left — 225° round the circle —, evenly spaced)
                int thin_places = 0;
                double least = 1e9, most = 0.0, full = 0.0, count = 0.0;
                const std::size_t n = run.points.size();
                for (std::size_t i = 0; i < n; ++i) {
                    const double r = run.reach[i];
                    least = std::min(least, r), most = std::max(most, r);
                    const double before = run.reach[(i + n - 1) % n], after = run.reach[(i + 1) % n];
                    if (r <= before && r < after && r < 12.0 * 0.3) {
                        ++thin_places;
                        const double at = std::atan2(run.points[i].y - centre.y, run.points[i].x - centre.x) * 180 / 3.141592653589793;
                        double off = std::fmod(at - 225.0 + 720.0, 360.0 / strokes);
                        off = std::min(off, 360.0 / strokes - off);
                        QVERIFY2(off < 3.0, qPrintable(QStringLiteral("%1 strokes: a thin place at %2°").arg(strokes).arg(at)));
                    }
                    full += r >= 12.0 - 1e-9 ? 1 : 0;
                    count += 1;
                }
                QCOMPARE(thin_places, strokes);
                QVERIFY2(std::fabs(least - 12.0 * 0.15) < 0.05 && std::fabs(most - 12.0) < 1e-9, qPrintable(QStringLiteral("%1 .. %2").arg(least).arg(most)));
                // (the share at full width: what the taper leaves, 1 − taper, within the points' spacing)
                QVERIFY2(std::fabs(full / count - (1 - taper)) < 0.04, qPrintable(QStringLiteral("%1 strokes, taper %2: %3 full").arg(strokes).arg(taper).arg(full / count)));
                // (no stretch between two points longer than the step)
                for (std::size_t i = 0; i < n; ++i) QVERIFY(clean::distance(run.points[i], run.points[(i + 1) % n]) <= pen.step + 1e-9);
                (void)total;
            }
        }
        // (a box: four strokes, thin at its corners — the corners its builder says are its own —, full in the middle of
        // each side)
        clean::Contour box_corners;
        const std::vector<clean::Shape> box{clean::whole(clean::rounded(clean::Box{100, 100, 500, 400}, 0.0, &box_corners))};
        QCOMPARE(box_corners.size(), std::size_t{4});
        clean::Pen pen;
        pen.width = 8.0;
        pen.taper = 0.5;
        pen.corners = box_corners;
        const std::vector<clean::PenRun> sides = clean::pen_runs(clean::edge_runs(box), box, pen);
        QCOMPARE(sides.size(), std::size_t{1});
        for (std::size_t i = 0; i < sides.front().points.size(); ++i) {
            const clean::P p = sides.front().points[i];
            const double r = sides.front().reach[i];
            const bool corner = (std::fabs(p.x - 100) < 1e-6 || std::fabs(p.x - 500) < 1e-6) && (std::fabs(p.y - 100) < 1e-6 || std::fabs(p.y - 400) < 1e-6);
            const bool middle = std::fabs(p.x - 300) < 30 || std::fabs(p.y - 250) < 30;
            if (corner) QVERIFY2(std::fabs(r - 1.5) < 1e-6, qPrintable(QString::number(r)));  // (0.15 of 8 px is 1.2: a pixel and a half at the least)
            if (middle) QVERIFY2(std::fabs(r - 8.0) < 1e-9, qPrintable(QString::number(r)));
            QCOMPARE(sides.front().stroke_start[i] != 0, corner);
        }
        // (without its corners said, the same box is one smooth line: two strokes, as an oval)
        clean::Pen smooth = pen;
        smooth.corners.clear();
        int starts = 0;
        const std::vector<clean::PenRun> as_smooth = clean::pen_runs(clean::edge_runs(box), box, smooth);
        for (const unsigned char start : as_smooth.front().stroke_start) starts += start;
        QCOMPARE(starts, 2);
        // (a small bubble — its outline shorter than six widths: not thinned; a thin line: never under a pixel, or its
        // own width when that is less)
        const std::vector<clean::Shape> bubble{clean::whole(clean::circle({100, 100}, 6.0))};
        for (const clean::PenRun& run : clean::pen_runs(clean::edge_runs(bubble), bubble, pen)) {
            for (const double r : run.reach) QCOMPARE(r, 8.0);
        }
        clean::Pen fine;
        fine.width = 2.0;
        fine.taper = 1.0;
        fine.swell = 0.9;
        for (const clean::PenRun& run : clean::pen_runs(clean::edge_runs(oval), oval, fine)) {
            for (const double r : run.reach) QVERIFY(r >= 1.5);
        }
        fine.width = 0.6;
        for (const clean::PenRun& run : clean::pen_runs(clean::edge_runs(oval), oval, fine)) {
            for (const double r : run.reach) QVERIFY(r >= 0.6 - 1e-12);
        }
    }

    // A balloon, its tail and the joins between them are one outline, and the pen draws it as one line: one closed run
    // round the whole group, with one corner — the tail's tip, where the line is thin on both sides. Two joined balloons
    // with a tail: one line too. A thought and its bubbles: a line each. Dashes are not joined: each its own, swelling
    // but not thinned.
    void aBalloonAndItsTailAreOneLine() {
        const clean::Box box{100, 100, 500, 400};
        const clean::Contour body = clean::ellipse(box);
        clean::TailSpec spec;
        spec.tip = {620, 520};
        spec.base = 80.0;
        spec.fillet = 12.0;
        spec.depth = 40.0;
        clean::Tail tail = clean::tail(body, body, {300, 250}, spec);
        QCOMPARE(tail.joins.size(), std::size_t{2});
        std::vector<clean::Shape> shapes{clean::whole(body), tail.shape};
        for (const clean::Shape& join : tail.joins) shapes.push_back(join);
        clean::Pen pen;
        pen.width = 8.0;
        pen.swell = 0.4;
        pen.taper = 0.6;
        pen.corners = tail.corners;  // (the tail's tip)
        QCOMPARE(tail.corners.size(), std::size_t{1});
        const std::vector<clean::Run> edge = clean::edge_runs(shapes);
        QVERIFY(edge.size() >= 4);  // (the balloon's, the tail's two sides, the joins': pieces)
        const std::vector<clean::PenRun> runs = clean::pen_runs(edge, shapes, pen);
        QCOMPARE(runs.size(), std::size_t{1});
        const clean::PenRun& line = runs.front();
        QVERIFY(line.closed);
        // (it passes the tail's tip, where it is thin, and the far side of the balloon, where it is not)
        double at_tip = 1e9, tip_reach = 0.0, far_reach = 0.0, far_at = 1e9;
        for (std::size_t i = 0; i < line.points.size(); ++i) {
            const double d = clean::distance(line.points[i], spec.tip);
            if (d < at_tip) at_tip = d, tip_reach = line.reach[i];
            const double f = clean::distance(line.points[i], {100, 250});
            if (f < far_at) far_at = f, far_reach = line.reach[i];
        }
        QVERIFY2(at_tip < 1e-6, qPrintable(QString::number(at_tip)));
        QVERIFY2(tip_reach < 8.0 * 0.3, qPrintable(QString::number(tip_reach)));
        QVERIFY2(far_reach > 8.0 * 0.6, qPrintable(QString::number(far_reach)));
        // (the union's inside is on the line's right all the way round: it runs clockwise on the page)
        QVERIFY(clean::shoelace(line.points) > 0);
        // (two balloons joined, one with a tail)
        const clean::Contour second = clean::ellipse(clean::Box{380, 280, 740, 560});
        std::vector<clean::Shape> group{clean::whole(body), clean::whole(second)};
        for (clean::Shape& join : clean::joins_between(body, second, 30.0)) group.push_back(std::move(join));
        spec.tip = {60, 520};
        clean::Tail other = clean::tail(body, body, {300, 250}, spec);
        group.push_back(other.shape);
        for (const clean::Shape& join : other.joins) group.push_back(join);
        const std::vector<clean::PenRun> joined = clean::pen_runs(clean::edge_runs(group), group, pen);
        QCOMPARE(joined.size(), std::size_t{1});
        QVERIFY(joined.front().closed);
        // (a balloon and three bubbles apart from it: four lines)
        std::vector<clean::Shape> thought{clean::whole(body)};
        for (clean::Contour& bubble : clean::bubble_trail(body, {300, 250}, {640, 560}, 30.0)) thought.push_back(clean::whole(std::move(bubble)));
        QCOMPARE(clean::pen_runs(clean::edge_runs(thought), thought, pen).size(), std::size_t{4});
        // (dashes: as many lines as dashes, none closed, none thinned — the swell alone)
        const std::vector<clean::Shape> plain{clean::whole(body)};
        const std::vector<clean::Run> dashes = clean::edge_runs(clean::dashed(plain, 26.0, 16.0));
        clean::Pen dashed = pen;
        dashed.chain = false;
        const std::vector<clean::PenRun> each = clean::pen_runs(dashes, plain, dashed);
        QCOMPARE(each.size(), dashes.size());
        double least = 1e9, most = 0.0;
        for (const clean::PenRun& run : each) {
            QVERIFY(!run.closed);
            for (const double r : run.reach) least = std::min(least, r), most = std::max(most, r);
        }
        QVERIFY2(std::fabs(least - 8.0 * 0.6) < 0.1 && std::fabs(most - 8.0 * 1.4) < 0.1, qPrintable(QStringLiteral("%1 .. %2").arg(least).arg(most)));
    }

    // Drawn: a line with the pen's keys has a line that is heavier below and to the right than above and to the left, and
    // closes round its balloon whatever the kind, the swell, the taper and the strokes — with tails, drips and joined
    // balloons too, at 150 and at 600 dpi, and made black and white (the monochrome print) as well; a dotted box and a
    // flash are drawn as without the keys; without them every balloon is the even line it was (the pins above).
    void thePensLineClosesRoundItsBalloon_data() {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<QString>("style");
        QTest::addColumn<int>("dpi");
        const Json styles = Json::array({Json{{"pen_swell", 0.9}}, Json{{"pen_taper", 1.0}}, Json{{"pen_swell", 0.5}, {"pen_taper", 0.6}, {"pen_strokes", 1}},
                                         Json{{"pen_swell", 0.9}, {"pen_taper", 1.0}, {"pen_strokes", 8}, {"border_mm", 0.2}},
                                         Json{{"pen_swell", 0.6}, {"pen_taper", 0.7}, {"pen_angle", 200}, {"drip_mm", 7}, {"drip_seed", 4}},
                                         Json{{"pen_swell", 0.7}, {"pen_taper", 0.5}, {"border_mm", 0.9}, {"double", true}}});
        for (const char* kind : {"speech", "box", "rounded", "capsule", "polygon", "shout", "cloud", "electric", "heart", "star", "flower", "diamond", "thought"}) {
            int k = 0;
            for (const Json& style : styles) {
                const int dpi = (k++ % 2 == 0) ? 600 : 150;
                QTest::newRow(qPrintable(QStringLiteral("%1 %2 at %3").arg(kind, QString::fromStdString(style.dump())).arg(dpi))) << QString::fromLatin1(kind) << QString::fromStdString(style.dump()) << dpi;
            }
        }
    }
    void thePensLineClosesRoundItsBalloon() {
        QFETCH(QString, kind);
        QFETCH(QString, style);
        QFETCH(int, dpi);
        const auto px = [dpi](double mm) { return static_cast<int>(std::lround(mm / 25.4 * dpi)); };
        // (the box 40..90 × 40..86 mm, its tail down and a little to the right — off a box's corner)
        const Document doc = one_line(kind.toStdString().c_str(), Json{{"h_mm", 46}, {"tail", Json::array({74, 106})}, {"style", Json::parse(style.toStdString())}});
        const Image drawn = balloon_alone(doc, dpi);
        QVERIFY2(closed_round(grey_of(drawn), px(65), px(58), 200), "the line is open");
        QVERIFY2(closed_round(grey_of(genko::render::to_bitonal(drawn)), px(65), px(58)), "the printed line is open");
    }
    void thePensLineIsDrawn() {
        const int dpi = 600;
        const auto px = [dpi](double mm) { return static_cast<int>(std::lround(mm / 25.4 * dpi)); };
        const auto ink = [&](const Grey& g, int x0, int y0, int x1, int y1) {
            double dark = 0;
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) dark += (255 - g.at(x, y)) / 255.0;
            }
            return dark;
        };
        const Grey plain = grey_of(balloon_alone(one_line("speech"), dpi));
        const Grey swelling = grey_of(balloon_alone(one_line("speech", Json{{"style", Json{{"pen_swell", 0.6}}}}), dpi));
        // (the box 40..90 × 40..80 mm: its upper left quarter and its lower right)
        const double light_plain = ink(plain, px(38), px(38), px(65), px(60)), heavy_plain = ink(plain, px(65), px(60), px(92), px(82));
        const double light = ink(swelling, px(38), px(38), px(65), px(60)), heavy = ink(swelling, px(65), px(60), px(92), px(82));
        QVERIFY(std::fabs(light_plain - heavy_plain) < light_plain * 0.02);
        // (the line's mean reach over a quarter turned 45° either side of the heavy way: 1 ± 0.9 × the swell)
        QVERIFY2(heavy > heavy_plain * 1.4 && heavy < heavy_plain * 1.65, qPrintable(QStringLiteral("%1 of %2").arg(heavy).arg(heavy_plain)));
        QVERIFY2(light < light_plain * 0.6 && light > light_plain * 0.35, qPrintable(QStringLiteral("%1 of %2").arg(light).arg(light_plain)));
        // (the same ink in all: the line's mean width is the width it was)
        const double all_plain = ink(plain, 0, 0, plain.w, plain.h), all = ink(swelling, 0, 0, swelling.w, swelling.h);
        QVERIFY2(std::fabs(all - all_plain) < all_plain * 0.03, qPrintable(QStringLiteral("%1 of %2").arg(all).arg(all_plain)));
        // (thinned strokes: less ink than the even line, the more the bigger the taper)
        const double half = ink(grey_of(balloon_alone(one_line("speech", Json{{"style", Json{{"pen_taper", 0.5}}}}), dpi)), 0, 0, plain.w, plain.h);
        const double whole = ink(grey_of(balloon_alone(one_line("speech", Json{{"style", Json{{"pen_taper", 1.0}}}}), dpi)), 0, 0, plain.w, plain.h);
        QVERIFY(whole < half && half < all_plain);
        // (a dotted box and a flash: as without the keys; a whisper keeps its dashes — as many pieces of line as before)
        const Json pen{{"pen_swell", 0.7}, {"pen_taper", 0.8}};
        for (const char* kind : {"dotted_box", "flash"}) {
            QVERIFY2(balloon_alone(one_line(kind, Json{{"style", pen}}), 300).tobytes() == balloon_alone(one_line(kind), 300).tobytes(), kind);
        }
        QVERIFY(balloon_alone(one_line("whisper", Json{{"style", pen}}), 300).tobytes() != balloon_alone(one_line("whisper"), 300).tobytes());
        // (the same line is the same pixels each time, and wherever it lies — nothing of the pen is left to chance)
        const auto at_x = [&](double x_mm) {
            return balloon_alone(one_line("speech", Json{{"x_mm", x_mm}, {"w_mm", 50.8}, {"h_mm", 40.64}, {"tail", Json::array({x_mm + 60, 100})},
                                                         {"style", Json{{"pen_swell", 0.5}, {"pen_taper", 0.7}}}}), 300, 8.5 * 25.4, 5 * 25.4);
        };
        const Image here = at_x(42.333333333333336), there = at_x(67.73333333333333);
        QVERIFY(here.crop(genko::render::Box{400, 0, 1300, here.height()}).tobytes() == there.crop(genko::render::Box{700, 0, 1600, there.height()}).tobytes());
        QVERIFY(at_x(42.333333333333336).tobytes() == here.tobytes());
    }

    // --- fukidashi@2, the review's fix round: the same line at every resolution -------------------------------------------------

    // The pen's strokes are the line's own, not the resolution's: the same balloon drawn at 150, 600 and 1200 dpi has the
    // same pieces of line, each with as many strokes, beginning at the same places (within a third of a millimetre) and
    // as heavy along each stroke (within an eighth of the line's width, a quarter on a stroke shorter than eight widths —
    // at 150 dpi the thinnest line is a pixel and a half).
    // Every kind, with a tail; rounded corners and sharp ones (the review's P2-1: a rounded corner cut into few points at
    // one resolution and many at another was a corner at one and none at the other).
    void thePensStrokesAreTheSameAtEveryResolution_data() {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<QString>("style");
        QTest::addColumn<bool>("tail");
        const Json pen{{"pen_taper", 1.0}, {"pen_swell", 0.5}, {"border_mm", 0.6}};
        const auto with = [&pen](Json more) {
            Json st = pen;
            for (const auto& [key, value] : more.items()) st[key] = value;
            return QString::fromStdString(st.dump());
        };
        const std::pair<const char*, Json> cases[] = {
            {"speech", Json::object()}, {"rounded", Json::object()}, {"rounded", Json{{"corner_mm", 1.0}}}, {"rounded", Json{{"corner_mm", 5.0}}},
            {"rounded", Json{{"squircle", true}}}, {"box", Json::object()}, {"box", Json{{"corner_mm", 1.5}}}, {"box", Json{{"corner_mm", 4.0}}},
            {"capsule", Json::object()}, {"polygon", Json::object()}, {"polygon", Json{{"sides", 5}, {"corner_mm", 3.0}}}, {"polygon", Json{{"sides", 12}, {"corner_mm", 0}}},
            {"cloud", Json::object()}, {"shout", Json::object()}, {"shout", Json{{"spikes", 9}, {"spike_depth", 0.05}}}, {"electric", Json::object()},
            {"thought", Json::object()}, {"heart", Json::object()}, {"heart", Json{{"corner_mm", 3.0}}}, {"star", Json::object()},
            {"star", Json{{"points", 4}, {"spike_depth", 0.6}}}, {"star", Json{{"points", 12}, {"corner_mm", 0}}}, {"star", Json{{"points", 7}, {"corner_mm", 2.5}}},
            {"flower", Json::object()}, {"diamond", Json::object()}, {"diamond", Json{{"corner_mm", 0}}}, {"diamond", Json{{"corner_mm", 4.0}}},
            {"speech", Json{{"wave_mm", 0.4}}}, {"heart", Json{{"wave_mm", 0.3}}}, {"speech", Json{{"pen_strokes", 5}, {"pen_angle", 200}}},
            {"speech", Json{{"drip_mm", 6}, {"drip_seed", 3}}}, {"shout", Json{{"drip_mm", 6}, {"drip_seed", 1}}},
        };
        for (const auto& [kind, more] : cases) {
            for (const bool tail : {false, true}) {
                QTest::newRow(qPrintable(QStringLiteral("%1 %2%3").arg(kind, QString::fromStdString(more.dump()), tail ? " with a tail" : ""))) << QString::fromLatin1(kind) << with(more) << tail;
            }
        }
    }
    void thePensStrokesAreTheSameAtEveryResolution() {
        QFETCH(QString, kind);
        QFETCH(QString, style);
        QFETCH(bool, tail);
        namespace balloon = genko::render::text::balloon;
        // (the reviewer's size, 24 × 22 mm, and a bigger one. A burst that drips is held at the bigger alone: on a burst of
        // 24 mm a drip's side leaves the outline about a line's width from a point of it — the two are one corner or two
        // by a hair's breadth of where the pixels put them, which is the balloon's shape and not how its curves are cut)
        const bool small_too = !(kind == "shout" && style.contains(QStringLiteral("drip_mm")));
        for (const auto& [w, h] : {std::pair{24.0, 22.0}, std::pair{46.0, 38.0}}) {
            if (w < 30 && !small_too) continue;
            Json more{{"w_mm", w}, {"h_mm", h}, {"style", Json::parse(style.toStdString())}};
            // (a tail straight down, a little to the right: between a star's two lower points, off a box's corner)
            if (tail) more["tails"] = Json::array({Json{{"to", Json::array({40 + w * 0.53, 40 + h + 9})}}});
            const Document doc = one_line(kind.toStdString().c_str(), more);
            const std::vector<const genko::core::StoryLine*> group{&doc.story.front()};
            // (each stretch of line as its strokes: where each begins, and how heavy it is at tenths of its length)
            struct Stroke {
                balloon::Pt at;
                std::array<double, 9> weight;
                double length = 0.0;
            };
            const auto strokes_of = [&](int dpi) {
                std::vector<std::vector<Stroke>> out;
                for (const balloon::PenTrace& trace : balloon::pen_trace(group, dpi)) {
                    const std::size_t n = trace.points_mm.size();
                    std::vector<double> along(n + 1, 0.0);
                    for (std::size_t i = 0; i < n; ++i) {
                        const balloon::Pt a = trace.points_mm[i], b = trace.points_mm[(i + 1) % n];
                        along[i + 1] = along[i] + ((trace.closed || i + 1 < n) ? std::hypot(b[0] - a[0], b[1] - a[1]) : 0.0);
                    }
                    const double total = along[n];
                    const auto weight_at = [&](double s) {
                        s = trace.closed ? std::fmod(std::fmod(s, total) + total, total) : std::clamp(s, 0.0, along[n - 1]);
                        std::size_t k = static_cast<std::size_t>(std::upper_bound(along.begin(), along.end(), s) - along.begin());
                        k = std::min(n - 1, k == 0 ? 0 : k - 1);
                        const double span = along[k + 1] - along[k];
                        const double t = span > 0 ? (s - along[k]) / span : 0.0;
                        return trace.weight[k] + (trace.weight[(k + 1) % n] - trace.weight[k]) * t;
                    };
                    std::vector<std::size_t> starts;
                    for (std::size_t i = 0; i < n; ++i) {
                        if (trace.stroke_start[i]) starts.push_back(i);
                    }
                    std::vector<Stroke> list;
                    for (std::size_t k = 0; k < starts.size(); ++k) {
                        const double from = along[starts[k]];
                        double to = k + 1 < starts.size() ? along[starts[k + 1]] : (trace.closed ? along[starts.front()] + total : from);
                        Stroke stroke;
                        stroke.at = trace.points_mm[starts[k]];
                        stroke.length = to - from;
                        for (std::size_t j = 0; j < 9; ++j) stroke.weight[j] = weight_at(from + (to - from) * (static_cast<double>(j) + 1) / 10);
                        list.push_back(stroke);
                    }
                    out.push_back(std::move(list));
                }
                return out;
            };
            const auto coarse = strokes_of(150), fine = strokes_of(600), finest = strokes_of(1200);
            QVERIFY(!fine.empty());
            for (const auto* other : {&coarse, &finest}) {
                const QString which = QStringLiteral("%1 × %2, %3 dpi").arg(w).arg(h).arg(other == &coarse ? 150 : 1200);
                QVERIFY2(other->size() == fine.size(), qPrintable(QStringLiteral("%1: %2 pieces of line for %3").arg(which).arg(other->size()).arg(fine.size())));
                for (std::size_t r = 0; r < fine.size(); ++r) {
                    QVERIFY2((*other)[r].size() == fine[r].size(),
                             qPrintable(QStringLiteral("%1: piece %2 has %3 strokes for %4").arg(which).arg(r).arg((*other)[r].size()).arg(fine[r].size())));
                    for (std::size_t k = 0; k < fine[r].size(); ++k) {
                        const Stroke& a = fine[r][k];
                        const Stroke& b = (*other)[r][k];
                        const double apart = std::hypot(a.at[0] - b.at[0], a.at[1] - b.at[1]);
                        QVERIFY2(apart <= 0.34, qPrintable(QStringLiteral("%1: stroke %2 begins %3 mm away").arg(which).arg(k).arg(apart)));
                        for (std::size_t j = 0; j < 9; ++j) {
                            // (at 150 dpi a line of 0.6 mm is 4 px and never thinner than 1.5: three eighths of its width)
                            const double low = other == &coarse ? 0.375 : 0.0;
                            // (a stroke shorter than eight widths — 4.8 mm — is thinned by how long it is, and a pixel of
                            // the coarser picture is 0.17 mm of it: a quarter of the width)
                            const double within = a.length < 4.8 ? 0.25 : 0.125;
                            QVERIFY2(std::fabs(std::max(a.weight[j], low) - std::max(b.weight[j], low)) <= within,
                                     qPrintable(QStringLiteral("%1: stroke %2 at %3 tenths: %4 for %5").arg(which).arg(k).arg(j + 1).arg(b.weight[j]).arg(a.weight[j])));
                        }
                    }
                }
            }
        }
    }

    // The drips are the line's own too (the review's P2-2): the same line has the same drips at 150, 350, 600 and 1200 dpi —
    // as many, each standing in the same place (within a pixel of the coarsest), as wide and reaching as far down. Over
    // sizes whose slot count sat on a knife edge (72 mm: 0.8 × 72 ÷ 12.8 is 4.5), the widths at the 4 mm limit, square
    // kinds, bursts, clouds and symbols, and twelve arrangements each.
    void theDripsAreTheSameAtEveryResolution_data() {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<double>("w");
        QTest::addColumn<double>("h");
        const std::tuple<const char*, double, double> cases[] = {
            {"speech", 72, 20}, {"speech", 38, 18}, {"speech", 56, 24}, {"speech", 88, 30}, {"speech", 104, 30}, {"speech", 16.8, 14}, {"speech", 40, 16},
            {"box", 40, 16}, {"box", 49.78, 20}, {"box", 64, 22}, {"rounded", 46, 30}, {"narration", 35.5556, 18}, {"shout", 44, 34}, {"cloud", 44, 34},
            {"heart", 40, 38}, {"star", 44, 42}, {"diamond", 44, 30}, {"capsule", 15, 40}, {"polygon", 40, 32}, {"flower", 40, 38},
        };
        for (const auto& [kind, w, h] : cases) QTest::newRow(qPrintable(QStringLiteral("%1 %2 × %3").arg(kind).arg(w).arg(h))) << QString::fromLatin1(kind) << w << h;
    }
    void theDripsAreTheSameAtEveryResolution() {
        QFETCH(QString, kind);
        QFETCH(double, w);
        QFETCH(double, h);
        namespace balloon = genko::render::text::balloon;
        int drawn = 0;
        for (int seed = 0; seed < 12; ++seed) {
            Json style{{"drip_mm", 5}, {"drip_seed", seed}};
            if (seed % 4 == 3) style["drip_width_mm"] = 1.0 + seed * 0.4;
            if (seed % 5 == 4) style["drips"] = 3 + seed;
            const Document doc = one_line(kind.toStdString().c_str(), Json{{"x_mm", 30.37}, {"y_mm", 41.13}, {"w_mm", w}, {"h_mm", h + 5}, {"style", style}});
            const std::vector<balloon::DripTrace> base = balloon::drip_trace(doc.story.front(), 600);
            drawn += static_cast<int>(base.size());
            for (const int dpi : {150, 350, 1200}) {
                const std::vector<balloon::DripTrace> other = balloon::drip_trace(doc.story.front(), dpi);
                const QString which = QStringLiteral("seed %1 at %2 dpi").arg(seed).arg(dpi);
                QVERIFY2(other.size() == base.size(), qPrintable(QStringLiteral("%1: %2 drips for %3").arg(which).arg(other.size()).arg(base.size())));
                const double pixel = 25.4 / std::min(dpi, 600);  // (a pixel of the coarser of the two)
                for (std::size_t k = 0; k < base.size(); ++k) {
                    QVERIFY2(std::fabs(other[k].x_mm - base[k].x_mm) <= pixel * 1.5, qPrintable(QStringLiteral("%1: drip %2 at %3 for %4").arg(which).arg(k).arg(other[k].x_mm).arg(base[k].x_mm)));
                    QVERIFY2(std::fabs(other[k].tip_mm - base[k].tip_mm) <= pixel * 2.5, qPrintable(QStringLiteral("%1: drip %2 down to %3 for %4").arg(which).arg(k).arg(other[k].tip_mm).arg(base[k].tip_mm)));
                    QVERIFY2(std::fabs(other[k].half_mm - base[k].half_mm) <= pixel, qPrintable(QStringLiteral("%1: drip %2 half %3 for %4").arg(which).arg(k).arg(other[k].half_mm).arg(base[k].half_mm)));
                }
            }
        }
        QVERIFY2(drawn >= 12, qPrintable(QString::number(drawn)));  // (at least the longest, every time)
    }

    // Where a short stroke lies between two corners and the strokes beside it thin toward them, the corner is as thin as
    // they are — no knob of the full width sits there (the review's P2-3) —, and corners nearer each other than the line
    // is wide are one corner. The geometry (a box with one corner cut off by a short side), and the review's hearts drawn:
    // along every pen's line no point is far heavier than the points either side of it.
    void aShortStrokeBetweenCornersIsNoKnob() {
        // (a box 400 × 300 whose corner at the top right is cut off: a side of 28 px between two corners, the line 8 px)
        const clean::Contour cut{{100, 100}, {480, 100}, {500, 120}, {500, 400}, {100, 400}};
        const std::vector<clean::Shape> shapes{clean::whole(cut)};
        clean::Pen pen;
        pen.width = 8.0;
        pen.taper = 0.6;
        pen.swell = 0.5;
        pen.corners = cut;
        const std::vector<clean::PenRun> runs = clean::pen_runs(clean::edge_runs(shapes), shapes, pen);
        QCOMPARE(runs.size(), std::size_t{1});
        int corners = 0;
        for (std::size_t i = 0; i < runs.front().points.size(); ++i) {
            const clean::P p = runs.front().points[i];
            const double r = runs.front().reach[i];
            corners += runs.front().stroke_start[i];
            // (on the short side and at its two ends: no heavier than a long stroke's end thinned, with the swell)
            if (p.x >= 480 - 1e-6 && p.y <= 120 + 1e-6) QVERIFY2(r <= 8.0 * 1.5 * 0.5 + 1e-9, qPrintable(QStringLiteral("%1, %2: %3").arg(p.x).arg(p.y).arg(r)));
            if ((std::fabs(p.x - 480) < 1e-6 && std::fabs(p.y - 100) < 1e-6) || (std::fabs(p.x - 500) < 1e-6 && std::fabs(p.y - 120) < 1e-6)) {
                QVERIFY2(r <= 8.0 * 0.15 * 1.5 + 1e-9, qPrintable(QString::number(r)));
            }
        }
        QCOMPARE(corners, 5);
        // (the same with the cut 6 px long — shorter than the line is wide: its two corners are one)
        const clean::Contour nick{{100, 100}, {496, 100}, {500, 104}, {500, 400}, {100, 400}};
        const std::vector<clean::Shape> nicked{clean::whole(nick)};
        pen.corners = nick;
        corners = 0;
        const std::vector<clean::PenRun> nicked_runs = clean::pen_runs(clean::edge_runs(nicked), nicked, pen);
        for (const unsigned char start : nicked_runs.front().stroke_start) corners += start;
        QCOMPARE(corners, 4);
        // (drawn: the review's hearts, and the kinds with short strokes — no point of the line stands out as a knob: none
        // is more than 1.8 times as heavy as the heavier of the points half a millimetre either side of it)
        namespace balloon = genko::render::text::balloon;
        const std::pair<const char*, Json> cases[] = {
            {"heart", Json{{"pen_taper", 0.8}, {"wave_mm", 0.3}}}, {"heart", Json{{"pen_taper", 0.8}, {"wave_mm", 0.3}, {"border_mm", 0.7}}},
            {"heart", Json{{"pen_taper", 0.8}, {"wave_mm", 0.3}, {"border_mm", 0.7}, {"pen_swell", 0.9}}}, {"heart", Json{{"pen_taper", 0.8}, {"border_mm", 0.7}}},
            {"star", Json{{"pen_taper", 1.0}, {"points", 12}, {"border_mm", 0.8}}}, {"shout", Json{{"pen_taper", 1.0}, {"spikes", 40}, {"border_mm", 0.8}}},
            {"cloud", Json{{"pen_taper", 1.0}, {"bumps", 40}, {"border_mm", 0.8}}}, {"electric", Json{{"pen_taper", 1.0}, {"border_mm", 0.8}}},
            {"polygon", Json{{"pen_taper", 1.0}, {"sides", 12}, {"border_mm", 1.0}}}, {"box", Json{{"pen_taper", 1.0}, {"corner_mm", 0.5}, {"border_mm", 1.0}}},
        };
        for (const auto& [kind, style] : cases) {
            for (const int dpi : {150, 600}) {
                const Document doc = one_line(kind, Json{{"w_mm", 30}, {"h_mm", 28}, {"tail", Json::array({66, 84})}, {"style", style}});
                const auto traces = balloon::pen_trace({&doc.story.front()}, dpi);
                QVERIFY(!traces.empty());
                for (const balloon::PenTrace& trace : traces) {
                    const std::size_t n = trace.weight.size();
                    for (std::size_t i = 0; i < n; ++i) {
                        if (!trace.closed && (i == 0 || i + 1 == n)) continue;
                        const double before = trace.weight[(i + n - 1) % n], after = trace.weight[(i + 1) % n];
                        QVERIFY2(trace.weight[i] <= 1.8 * std::max(before, after) + 0.3,
                                 qPrintable(QStringLiteral("%1 %2 at %3 dpi: %4 between %5 and %6").arg(kind, QString::fromStdString(style.dump())).arg(dpi).arg(trace.weight[i]).arg(before).arg(after)));
                    }
                }
            }
        }
    }

    // A balloon that drips keeps its second line (double), the white a tone leaves inside its line and a tone box's margin
    // along its body and its tails: they do not follow the drips' fingers (the review's P2-4 — a thick line's second line
    // along a slim finger took close to a minute). Drawn, a thick double line with drips takes about what it takes without
    // them (a wide margin: three times as long and three seconds more — a guard against the minute, not a measure); the
    // second line is there, in the body and not in a finger.
    void aThickDoubleLineWithDripsIsNotSlow() {
        const auto timed = [](const Document& doc, int dpi) {
            QElapsedTimer timer;
            timer.start();
            const Image drawn = balloon_alone(doc, dpi, 90, 80);
            return std::pair<qint64, Image>{timer.elapsed(), drawn};
        };
        for (const auto& [kind, border] : {std::pair{"cloud", 2.5}, std::pair{"speech", 1.2}, std::pair{"heart", 2.5}}) {
            const Json plain{{"border_mm", border}, {"double", true}};
            Json dripping = plain;
            dripping["drip_mm"] = 5;
            dripping["drip_seed"] = 2;
            dripping["pen_swell"] = 0.5;
            dripping["fill_tone"] = Json{{"density", 0.2}, {"inset_mm", 1.0}};
            const Document without = one_line(kind, Json{{"x_mm", 20}, {"y_mm", 20}, {"w_mm", 38}, {"h_mm", 13.5}, {"style", plain}});
            const Document with = one_line(kind, Json{{"x_mm", 20}, {"y_mm", 20}, {"w_mm", 38}, {"h_mm", 18.5}, {"style", dripping}});
            const auto [base_ms, base] = timed(without, 600);
            const auto [drip_ms, drawn] = timed(with, 600);
            QVERIFY2(drip_ms <= base_ms * 3 + 3000, qPrintable(QStringLiteral("%1, a line of %2 mm: %3 ms with drips, %4 ms without").arg(kind).arg(border).arg(drip_ms).arg(base_ms)));
            QVERIFY(drawn.tobytes() != base.tobytes());
        }
        // (the second line: in the body — above the body's foot, 3 to 4 widths inside its edge —, none in a finger)
        const int dpi = 600;
        const auto px = [dpi](double mm) { return static_cast<int>(std::lround(mm / 25.4 * dpi)); };
        const Json thin{{"double", true}, {"drip_mm", 8}, {"drip_seed", 5}, {"drip_width_mm", 4}, {"drips", 5}};
        const Document doc = one_line("box", Json{{"h_mm", 48}, {"style", thin}});  // (40..90 × 40..88: the body to 80)
        const Document plain = one_line("box", Json{{"h_mm", 40}, {"style", Json{{"double", true}}}});
        const Grey with = grey_of(balloon_alone(doc, dpi)), body = grey_of(balloon_alone(plain, dpi));
        // (above the body's foot less 6 widths — clear of the fingers' mouths — the two are the same pixels)
        int unlike = 0;
        for (int y = 0; y < px(80) - 60; ++y) {
            for (int x = 0; x < with.w; ++x) unlike += with.at(x, y) != body.at(x, y) ? 1 : 0;
        }
        QCOMPARE(unlike, 0);
        // (below the body's foot a finger has its one line each side and white between: walking across a row 3 mm below
        // the foot, each run of ink is a line's width — 8 px —, never a second line beside it)
        const int row = px(83);
        int run = 0, longest = 0, runs = 0;
        for (int x = 0; x < with.w; ++x) {
            if (with.at(x, row) < 128) {
                ++run;
            } else {
                if (run > 0) ++runs;
                longest = std::max(longest, run), run = 0;
            }
        }
        QVERIFY2(runs >= 2 && runs % 2 == 0 && longest <= 12, qPrintable(QStringLiteral("%1 runs, the longest %2 px").arg(runs).arg(longest)));
    }

    // A drip is never wider than its slot or its balloon can hold, its join with the balloon stops at the balloon's
    // corners, and the longest drip is always there (the review's P3-1: drips of 10 mm on a diamond of 20 mm left bumps
    // without a line and ran out of the box; on a small star none was drawn though the body was shortened). Whatever the
    // width asked and the arrangement: nothing outside the box, the line closed, ink below the body's foot.
    void aWideDripStaysInItsBalloon_data() {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<double>("w");
        QTest::addColumn<double>("h");
        QTest::addColumn<QString>("style");
        const Json wide{{"drip_mm", 4}, {"drip_width_mm", 10}, {"drips", 6}};
        QTest::newRow("diamond 20 × 13.5") << QStringLiteral("diamond") << 20.0 << 13.5 << QString::fromStdString(wide.dump());
        QTest::newRow("rounded 20 × 13.5") << QStringLiteral("rounded") << 20.0 << 13.5 << QString::fromStdString(wide.dump());
        QTest::newRow("star 20, twelve drips") << QStringLiteral("star") << 20.0 << 20.0 << QString::fromStdString(Json{{"drip_mm", 4}, {"drip_width_mm", 10}, {"drips", 12}}.dump());
        QTest::newRow("speech 20 × 13.5") << QStringLiteral("speech") << 20.0 << 13.5 << QString::fromStdString(wide.dump());
        QTest::newRow("box 20 × 13.5") << QStringLiteral("box") << 20.0 << 13.5 << QString::fromStdString(wide.dump());
        QTest::newRow("heart 24 × 22") << QStringLiteral("heart") << 24.0 << 22.0 << QString::fromStdString(wide.dump());
        QTest::newRow("capsule 12 × 30, 24 drips") << QStringLiteral("capsule") << 12.0 << 30.0 << QString::fromStdString(Json{{"drip_mm", 6}, {"drip_width_mm", 10}, {"drips", 24}}.dump());
        QTest::newRow("polygon 30 × 24, one drip") << QStringLiteral("polygon") << 30.0 << 24.0 << QString::fromStdString(Json{{"drip_mm", 6}, {"drip_width_mm", 10}, {"drips", 1}}.dump());
        QTest::newRow("cloud 30 × 24") << QStringLiteral("cloud") << 30.0 << 24.0 << QString::fromStdString(wide.dump());
        QTest::newRow("shout 30 × 24") << QStringLiteral("shout") << 30.0 << 24.0 << QString::fromStdString(wide.dump());
    }
    void aWideDripStaysInItsBalloon() {
        QFETCH(QString, kind);
        QFETCH(double, w);
        QFETCH(double, h);
        QFETCH(QString, style);
        const int dpi = 300;
        const auto px = [dpi](double mm) { return static_cast<int>(std::lround(mm / 25.4 * dpi)); };
        Json st = Json::parse(style.toStdString());
        const double drip = st["drip_mm"].get<double>();
        for (int seed = 0; seed < 32; ++seed) {
            st["drip_seed"] = seed;
            const Document doc = one_line(kind.toStdString().c_str(), Json{{"x_mm", 20}, {"y_mm", 20}, {"w_mm", w}, {"h_mm", h + drip}, {"style", st}});
            const Grey g = grey_of(balloon_alone(doc, dpi, 70, 70));
            int outside = 0, hanging = 0;
            for (int y = 0; y < g.h; ++y) {
                for (int x = 0; x < g.w; ++x) {
                    if (g.at(x, y) == 255) continue;
                    if (x < px(20) || x >= px(20 + w) || y < px(20) || y >= px(20 + h + drip)) ++outside;
                    if (y >= px(20 + h) + 2) ++hanging;
                }
            }
            QVERIFY2(outside == 0, qPrintable(QStringLiteral("seed %1: %2 px outside the box").arg(seed).arg(outside)));
            QVERIFY2(hanging > 0, qPrintable(QStringLiteral("seed %1: no drip").arg(seed)));
            QVERIFY2(closed_round(g, px(20 + w / 2), px(20 + h / 2)), qPrintable(QStringLiteral("seed %1: the line is open").arg(seed)));
        }
    }

    // A trembling heart's cleft is one V, not a cross (found on the fix round's own sample sheet: the wave pushed out on
    // both sides of the cleft crossed itself there, and the line drew an X with two long points, at every resolution).
    // The wave calms to nothing within a wave of a corner that turns in; a corner that turns out keeps its wave, so a
    // box's, a polygon's or a diamond's trembling outline is what it was.
    void aTremblingHeartsCleftDoesNotCrossItself() {
        const auto px = [](double mm, int dpi) { return static_cast<int>(std::lround(mm / 25.4 * dpi)); };
        for (const int dpi : {300, 600, 1200}) {
            for (const double wave : {0.2, 0.3, 0.6}) {
                for (const Json& more : {Json{{"border_mm", 0.7}}, Json{{"border_mm", 0.3}}, Json{{"border_mm", 0.7}, {"pen_taper", 0.8}}, Json{{"border_mm", 0.5}, {"pen_swell", 0.5}}}) {
                    Json st = more;
                    st["wave_mm"] = wave;
                    const Document doc = one_line("heart", Json{{"x_mm", 10}, {"y_mm", 10}, {"w_mm", 30}, {"h_mm", 28}, {"style", st}});
                    const Grey g = grey_of(balloon_alone(doc, dpi, 50, 48));
                    // (the ink in the middle 12 mm, down to the heart's middle: its deepest, and how wide the ink is
                    // within 0.6 mm of that — a V's point is about a line wide, the cross was 1.4 to 4.9 mm)
                    int deepest = -1;
                    for (int y = px(10, dpi); y < px(24, dpi); ++y) {
                        for (int x = px(19, dpi); x < px(31, dpi); ++x) {
                            if (g.at(x, y) < 128) deepest = y;
                        }
                    }
                    QVERIFY(deepest > 0);
                    int left = g.w, right = -1;
                    for (int y = deepest - px(0.6, dpi); y <= deepest; ++y) {
                        for (int x = px(19, dpi); x < px(31, dpi); ++x) {
                            if (g.at(x, y) < 128) {
                                left = std::min(left, x);
                                right = std::max(right, x);
                            }
                        }
                    }
                    const double spread = (right - left) / static_cast<double>(dpi) * 25.4;
                    QVERIFY2(spread < 1.0, qPrintable(QStringLiteral("%1 dpi, %2: the cleft's point is %3 mm wide").arg(dpi).arg(QString::fromStdString(st.dump())).arg(spread)));
                }
            }
        }
        // (a corner that turns out keeps its wave: the same outline with the corners said as without)
        const auto same = [](const clean::Contour& a, const clean::Contour& b) {
            return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](clean::P p, clean::P q) { return p.x == q.x && p.y == q.y; });
        };
        const clean::Box box{0, 0, 600, 400};
        for (const double radius : {0.0, 20.0}) {
            clean::Contour corners;
            const clean::Contour square = clean::rounded(box, radius, &corners);
            QVERIFY(!corners.empty());
            QVERIFY(same(clean::waved(square, 7, 50, corners), clean::waved(square, 7, 50)));
        }
        clean::Contour corners;
        const clean::Contour diamond = clean::diamond(box, 10, &corners);
        QVERIFY(same(clean::waved(diamond, 7, 50, corners), clean::waved(diamond, 7, 50)));
        const clean::Contour octagon = clean::polygon(box, 8, 10, &corners);
        QVERIFY(same(clean::waved(octagon, 7, 50, corners), clean::waved(octagon, 7, 50)));
        const clean::Contour heart = clean::heart(box, 10, &corners);
        QVERIFY(!same(clean::waved(heart, 7, 50, corners), clean::waved(heart, 7, 50)));
    }

    // The white edge of the letters over a tone follows the tone that is drawn (the review's P3-2): a group of joined
    // balloons has its first line's — the second line's letters have the edge when the first line has the tone, whatever
    // the second says, and none when the first has none —; a tone that draws nothing gives no edge.
    void theWhiteEdgeFollowsTheToneDrawn() {
        const Json tone{{"fill_tone", Json{{"density", 0.3}}}};
        const auto pair = [&](const Json& first, const Json& second) {
            Json a{{"op", "add_line"}, {"page", 1}, {"id", "L1"}, {"balloon", "speech"}, {"x_mm", 30}, {"y_mm", 40}, {"w_mm", 40}, {"h_mm", 34}, {"text", "あいう"}};
            Json b{{"op", "add_line"}, {"page", 1}, {"id", "L2"}, {"balloon", "speech"}, {"x_mm", 62}, {"y_mm", 56}, {"w_mm", 40}, {"h_mm", 34}, {"text", "えおか"}};
            a["style"] = first;
            a["style"]["group"] = "g";
            b["style"] = second;
            b["style"]["group"] = "g";
            return run_ops(blank_book(), Json::array({a, b}));
        };
        const auto drawn = [](const Document& doc) { return page_part(doc, 300, 24, 34, 84, 62).tobytes(); };
        const std::string both = drawn(pair(tone, tone));
        QVERIFY(drawn(pair(tone, Json::object())) == both);            // (the second line's own key does not matter)
        const std::string neither = drawn(pair(Json::object(), Json::object()));
        QVERIFY(drawn(pair(Json::object(), tone)) == neither);         // (no tone is drawn: no edge either)
        QVERIFY(both != neither);
        // (the letters' picture itself, set as draw_group sets it for its group: ToneUnder)
        const Document plain = one_line("speech", Json::object(), "あ");
        const Document toned = one_line("speech", Json{{"style", tone}}, "あ");
        const int own = genko::render::text::text_layout(plain.story.front(), 600).image.width();
        {
            const genko::render::text::ToneUnder under(true);
            QVERIFY(genko::render::text::text_layout(plain.story.front(), 600).image.width() > own);
        }
        {
            const genko::render::text::ToneUnder under(false);
            QCOMPARE(genko::render::text::text_layout(toned.story.front(), 600).image.width(), own);
        }
        QVERIFY(genko::render::text::text_layout(toned.story.front(), 600).image.width() > own);
        // (a tone of nothing — density 0, growing to nothing — draws nothing and gives no edge; growing to something, it does)
        for (const Json& nothing : {Json{{"density", 0}}, Json{{"density", 0.0}, {"to", 0}}, Json{{"lpi", 60}}}) {
            const Document doc = one_line("speech", Json::object(), "あ");
            Document with = doc;
            with.story.front().style["fill_tone"] = nothing;
            QCOMPARE(genko::render::text::text_layout(with.story.front(), 600).image.width(), own);
            QVERIFY(page_part(with, 300, 30, 30, 70, 60).tobytes() == page_part(doc, 300, 30, 30, 70, 60).tobytes());
        }
        Document growing = plain;
        growing.story.front().style["fill_tone"] = Json{{"density", 0}, {"to", 0.4}};
        QVERIFY(genko::render::text::text_layout(growing.story.front(), 600).image.width() > own);
    }

    // A book's styles are not checked when it is read: whatever a line's style holds under the additions' keys — a text, a
    // list, a truth value, a number far out of range, a tone of the wrong things —, the page is drawn (the review's P3-3:
    // a huge number of drips, a blend to a colour that is not three numbers stopped the page). A value of the wrong kind
    // is as if the key were not there; a number out of range is taken to the nearest it may be.
    void aMalformedStyleStillDraws() {
        const Json wrong = Json::array({"x", true, false, Json::array(), Json::array({1, 2}), Json::object(), Json{{"a", 1}}, nullptr});
        const Json numbers = Json::array({1e300, -1e300, 18446744073709551615ull, -5, 3.7, 0, 1e-300, 9223372036854775807ll});
        const char* keys[] = {"pen_swell", "pen_angle", "pen_taper", "pen_strokes", "drip_mm", "drips", "drip_width_mm", "drip_seed", "points", "fill_tone", "fill_gradient"};
        // (a line that has every addition well said, so that each key's neighbours are in play)
        const Json good{{"pen_swell", 0.4}, {"pen_taper", 0.5}, {"drip_mm", 5}, {"fill_tone", Json{{"density", 0.2}, {"lpi", 60.0}, {"angle", 45.0}, {"pattern", "dot"}}},
                        {"fill_gradient", Json{{"to", Json::array({200, 220, 255})}, {"dir", 90.0}}}};
        for (const char* kind : {"speech", "star", "box"}) {
            const Document base = one_line(kind, Json{{"h_mm", 45}, {"tail", Json::array({100, 100})}, {"style", good}});
            for (const char* key : keys) {
                Document absent = base;
                absent.story.front().style.erase(key);
                const std::string without = page_part(absent, 150, 30, 30, 80, 80).tobytes();
                for (const Json& value : wrong) {
                    Document doc = base;
                    doc.story.front().style[key] = value;
                    std::string drawn;
                    try {
                        drawn = page_part(doc, 150, 30, 30, 80, 80).tobytes();
                    } catch (const std::exception& error) {
                        QFAIL(qPrintable(QStringLiteral("%1 %2 = %3: %4").arg(kind, key, QString::fromStdString(value.dump()), QString::fromUtf8(error.what()))));
                    }
                    // (a truth value is no number here: as absent. An empty object for a tone or a blend: nothing to draw)
                    QVERIFY2(drawn == without, qPrintable(QStringLiteral("%1 %2 = %3 is not drawn as without it").arg(kind, key, QString::fromStdString(value.dump()))));
                }
                for (const Json& value : numbers) {
                    Document doc = base;
                    doc.story.front().style[key] = value;
                    try {
                        (void)page_part(doc, 150, 30, 30, 80, 80);
                        (void)genko::formats::checks::page_issues(doc, doc.page(0));
                        (void)payload_of(doc);
                    } catch (const std::exception& error) {
                        QFAIL(qPrintable(QStringLiteral("%1 %2 = %3: %4").arg(kind, key, QString::fromStdString(value.dump()), QString::fromUtf8(error.what()))));
                    }
                }
            }
            // (inside a tone and a blend: each member of the wrong kind, far out of range)
            const std::pair<const char*, const char*> members[] = {{"fill_tone", "density"}, {"fill_tone", "lpi"}, {"fill_tone", "angle"}, {"fill_tone", "pattern"},
                                                                   {"fill_tone", "to"}, {"fill_tone", "dir"}, {"fill_tone", "radial"}, {"fill_tone", "inset_mm"},
                                                                   {"fill_gradient", "to"}, {"fill_gradient", "dir"}, {"fill_gradient", "radial"}};
            Json values = wrong;
            for (const Json& value : numbers) values.push_back(value);
            for (const Json& more : {Json::array({1, 2, "x"}), Json::array({Json::array({1}), Json::array({2}), Json::array({3})}), Json::array({1e300, -4, 999}),
                                     Json::array({1, 2, 3, 4}), Json("line"), Json("zigzag")}) {
                values.push_back(more);
            }
            for (const auto& [key, member] : members) {
                for (const Json& value : values) {
                    Document doc = base;
                    doc.story.front().style[key][member] = value;
                    try {
                        (void)page_part(doc, 150, 30, 30, 80, 80);
                        (void)genko::render::text::text_layout(doc.story.front(), 150);
                        (void)genko::formats::checks::page_issues(doc, doc.page(0));
                    } catch (const std::exception& error) {
                        QFAIL(qPrintable(QStringLiteral("%1 %2.%3 = %4: %5").arg(kind, key, member, QString::fromStdString(value.dump()), QString::fromUtf8(error.what()))));
                    }
                }
            }
        }
        // (the review's own: a huge count and seed, a colour that is not three numbers — the page as without the key)
        const Document base = one_line("speech", Json{{"h_mm", 45}, {"style", Json{{"drip_mm", 5}}}});
        const std::string plain = page_part(base, 150, 30, 30, 80, 80).tobytes();
        Document doc = base;
        doc.story.front().style["drips"] = 1e300;
        Document most = base;
        most.story.front().style["drips"] = 24;
        QVERIFY(page_part(doc, 150, 30, 30, 80, 80).tobytes() == page_part(most, 150, 30, 30, 80, 80).tobytes());  // (as many as there may be)
        doc = base;
        doc.story.front().style["fill_gradient"] = Json{{"to", Json::array({1, 2, "x"})}};
        QVERIFY(page_part(doc, 150, 30, 30, 80, 80).tobytes() == plain);
    }

    // The pen's line with what else a balloon may have: a notch cut into it (an inward tail), an outline drawn by hand —
    // as given and smoothed —, the balloon eraser's cuts and a tail that fades. Drawn at 150, 600 and 1200 dpi, the same
    // each time; a notch and a hand-drawn outline close round the balloon; the eraser and the fading tail open the line
    // only where they do without the pen.
    void thePensLineWithANotchAPathCutsAndAFade() {
        const Json pen{{"pen_swell", 0.6}, {"pen_taper", 0.7}};
        const auto px_of = [](int dpi, double mm) { return static_cast<int>(std::lround(mm / 25.4 * dpi)); };
        const Json path = Json::array({Json::array({42, 44}), Json::array({70, 40}), Json::array({90, 54}), Json::array({84, 78}), Json::array({58, 80}), Json::array({40, 65})});
        for (const int dpi : {150, 600, 1200}) {
            // (the picture: page × 100 mm — the balloon and its tail reach 100 × 96, the fading tail's tip at 99.5 × 95.5)
            const double page = dpi == 1200 ? 115 : 140;
            {  // (a notch: its sides are part of the one line, closed)
                const Document doc = one_line("speech", Json{{"tails", Json::array({Json{{"to", Json::array({70, 62})}, {"kind", "inward"}}})}, {"style", pen}});
                const Image drawn = balloon_alone(doc, dpi, page, 100);
                QVERIFY(closed_round(grey_of(drawn), px_of(dpi, 50), px_of(dpi, 52), 200));
                genko::render::text::clear_balloon_cache();
                QVERIFY(balloon_alone(doc, dpi, page, 100).tobytes() == drawn.tobytes());
                if (dpi == 600) QCOMPARE(genko::render::text::balloon::pen_trace({&doc.story.front()}, dpi).size(), std::size_t{1});
            }
            for (const bool curved : {false, true}) {  // (drawn by hand)
                Json style = pen;
                if (curved) style["path_curve"] = true;
                const Document doc = one_line("speech", Json{{"path", path}, {"tail", Json::array({70, 92})}, {"style", style}});  // (a tail from its foot, clear of its corners)
                const Image drawn = balloon_alone(doc, dpi, page, 100);
                QVERIFY2(closed_round(grey_of(drawn), px_of(dpi, 65), px_of(dpi, 60), 200), qPrintable(QStringLiteral("%1 at %2 dpi").arg(curved ? "smoothed" : "as given").arg(dpi)));
                const auto traces = genko::render::text::balloon::pen_trace({&doc.story.front()}, dpi);
                QCOMPARE(traces.size(), std::size_t{1});
                int starts = 0;
                for (const unsigned char start : traces.front().stroke_start) starts += start;
                // (as given: its six points and its tail's tip, the two joins with the tail rounded; smoothed: the tip alone)
                QCOMPARE(starts, curved ? 1 : 7);
            }
            {  // (the eraser and a fading tail: where the pen's line is, the even line is too — nothing new is opened —, and
               // the balloon away from them closes as without them)
                const Document cut = run_ops(one_line("speech", Json{{"tail", Json::array({100, 96})}, {"style", pen}}),
                                             Json::array({Json{{"op", "cut_balloon"}, {"id", "L1"}, {"points", Json::array({Json::array({36, 52}), Json::array({48, 40})})}, {"width_mm", 4.0}}}));
                Document even = cut;
                even.story.front().style.erase("pen_swell");
                even.story.front().style.erase("pen_taper");
                const Grey a = grey_of(balloon_alone(cut, dpi, page, 100)), b = grey_of(balloon_alone(even, dpi, page, 100));
                int gone = 0, ink = 0;
                for (int y = 0; y < a.h; ++y) {
                    for (int x = 0; x < a.w; ++x) {
                        ink += a.at(x, y) < 128 ? 1 : 0;
                        // (where the eraser took the even line away, the pen's line is gone too)
                        if (b.at(x, y) == 255 && a.at(x, y) < 128) ++gone;
                    }
                }
                QVERIFY(ink > 100);
                QVERIFY2(gone < ink / 3, qPrintable(QStringLiteral("%1 of %2").arg(gone).arg(ink)));
                const Document fade = one_line("speech", Json{{"tails", Json::array({Json{{"to", Json::array({100, 96})}, {"kind", "fade"}}})}, {"style", pen}});
                const Grey f = grey_of(balloon_alone(fade, dpi, page, 100));
                QVERIFY(f.at(px_of(dpi, 99.5), px_of(dpi, 95.5)) > 200);  // (nothing left at the tip)
                QVERIFY(!closed_round(f, px_of(dpi, 65), px_of(dpi, 60), 200));  // (open at the tip, as a fading tail is)
            }
        }
    }

    // The pen's line and the drips at 1200 dpi: closed round the balloon, its tail and its drips, the same line as at 600
    // (the strokes above), and made black and white too.
    void thePenAndTheDripsAt1200Dpi() {
        const int dpi = 1200;
        const auto px = [dpi](double mm) { return static_cast<int>(std::lround(mm / 25.4 * dpi)); };
        for (const char* kind : {"speech", "box", "shout", "heart"}) {
            const Document doc = one_line(kind, Json{{"x_mm", 10}, {"y_mm", 10}, {"w_mm", 40}, {"h_mm", 40}, {"tail", Json::array({34, 60})},
                                                     {"style", Json{{"pen_swell", 0.6}, {"pen_taper", 0.8}, {"drip_mm", 7}, {"drip_seed", 4}}}});
            const Image drawn = balloon_alone(doc, dpi, 62, 66);
            QVERIFY2(closed_round(grey_of(drawn), px(30), px(24), 200), kind);
            QVERIFY2(closed_round(grey_of(genko::render::to_bitonal(drawn)), px(30), px(24)), kind);
            QVERIFY(!genko::render::text::balloon::drip_trace(doc.story.front(), dpi).empty());
        }
    }

    // A symbol's letters in boxes far from square and with other numbers of points than its own: still inside the symbol,
    // at every resolution (a box too small for them makes them smaller, never lets them out).
    void theLettersFitASymbolOfAnyShape_data() {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<double>("w");
        QTest::addColumn<double>("h");
        QTest::addColumn<QString>("style");
        const std::pair<double, double> boxes[] = {{80, 25}, {25, 80}, {30, 30}, {60, 20}, {20, 60}, {100, 100}};
        const std::pair<const char*, Json> kinds[] = {{"heart", Json::object()}, {"diamond", Json::object()}, {"star", Json{{"points", 4}, {"spike_depth", 0.6}}},
                                                      {"star", Json{{"points", 7}}}, {"star", Json{{"points", 12}, {"spike_depth", 0.05}}}, {"star", Json{{"points", 9}, {"spike_depth", 0.6}}},
                                                      {"flower", Json{{"points", 4}}}, {"flower", Json{{"points", 7}}}, {"flower", Json{{"points", 12}}}};
        for (const auto& [kind, style] : kinds) {
            for (const auto& [w, h] : boxes) {
                QTest::newRow(qPrintable(QStringLiteral("%1 %2 in %3 × %4").arg(kind, QString::fromStdString(style.dump())).arg(w).arg(h))) << QString::fromLatin1(kind) << w << h << QString::fromStdString(style.dump());
            }
        }
    }
    void theLettersFitASymbolOfAnyShape() {
        QFETCH(QString, kind);
        QFETCH(double, w);
        QFETCH(double, h);
        QFETCH(QString, style);
        const std::string k = kind.toStdString();
        const Json st = Json::parse(style.toStdString());
        const Document doc = one_line(k.c_str(), Json{{"x_mm", 20}, {"y_mm", 20}, {"w_mm", w}, {"h_mm", h}, {"style", st}}, "すき\nだよ");
        const genko::core::StoryLine& line = doc.story.front();
        for (const int dpi : {150, 600}) {
            const genko::render::text::Layout layout = genko::render::text::text_layout(line, dpi);
            const double a = w / 25.4 * dpi / 2, b = h / 25.4 * dpi / 2;
            const clean::Box box{-a, -b, a, b};
            const int points = genko::core::symbol_points(line);
            const double inner = genko::core::star_inner(line);
            const clean::Contour outline = k == "heart" ? clean::heart(box, -1.0) : k == "diamond" ? clean::diamond(box, -1.0)
                                           : k == "star" ? clean::star(box, points, inner, -1.0) : clean::flower(box, points);
            const std::string alpha = layout.image.getchannel(layout.image.bands() - 1).tobytes();
            int x0 = layout.image.width(), y0 = layout.image.height(), x1 = 0, y1 = 0;
            for (int y = 0; y < layout.image.height(); ++y) {
                for (int x = 0; x < layout.image.width(); ++x) {
                    if (static_cast<unsigned char>(alpha[static_cast<std::size_t>(y) * static_cast<std::size_t>(layout.image.width()) + static_cast<std::size_t>(x)]) <= 40) continue;
                    x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x + 1), y1 = std::max(y1, y + 1);
                }
            }
            QVERIFY(x1 > x0 && y1 > y0);
            QVERIFY(layout.em >= 8);
            // (letters at their smallest — 8 px — may not fit a box of a few millimetres at 150 dpi: the others are inside)
            if (layout.em <= 8) continue;
            for (const double x : {static_cast<double>(x0), static_cast<double>(x1)}) {
                for (const double y : {static_cast<double>(y0), static_cast<double>(y1)}) {
                    QVERIFY2(clean::inside(outline, {layout.corner_x + x, layout.corner_y + y}),
                             qPrintable(QStringLiteral("%1 dpi: a corner of the letters at %2, %3 is outside").arg(dpi).arg(layout.corner_x + x).arg(layout.corner_y + y)));
                }
            }
        }
    }

    // A tone printed at its lightest and its darkest (a page of 600 dpi made black and white): within the densities
    // tone_print_range says print — for 60 lines, from 4% — the dots are whole (no black pixel alone) and, to 90%, the
    // white between them too (no white pixel alone); lighter than that the dots do drop to single pixels, which is what
    // the book's check warns of: on a monochrome page a tone too light, or too dark, for its screen is said (a warning,
    // in Japanese, with the density to use), once for a group of joined balloons, and never on a colour page.
    void aToneOutsideWhatPrintsIsWarnedOf() {
        namespace balloon = genko::render::text::balloon;
        const balloon::TonePrint sixty = balloon::tone_print_range(600, 60, false);
        QVERIFY2(sixty.lightest > 0.03 && sixty.lightest < 0.04 && sixty.darkest == 0.9, qPrintable(QStringLiteral("%1 .. %2").arg(sixty.lightest).arg(sixty.darkest)));
        const balloon::TonePrint fine = balloon::tone_print_range(600, 85, false);
        QVERIFY2(fine.lightest > 0.06 && fine.lightest < 0.07 && fine.darkest > 0.8 && fine.darkest < 0.88, qPrintable(QStringLiteral("%1 .. %2").arg(fine.lightest).arg(fine.darkest)));
        const balloon::TonePrint stripes = balloon::tone_print_range(600, 60, true);
        QVERIFY(std::fabs(stripes.lightest - 0.2) < 1e-9 && std::fabs(stripes.darkest - 0.8) < 1e-9);
        QVERIFY(balloon::tone_print_range(1200, 60, false).lightest < sixty.lightest / 3.9);
        const auto lone = [](const Document& doc, int white_or_black) {
            const Grey g = grey_of(genko::render::to_bitonal(page_part(doc, 600, 50, 50, 8, 20)));  // (inside the oval, clear of its line and its letter)
            int alone = 0;
            for (int y = 1; y + 1 < g.h; ++y) {
                for (int x = 1; x + 1 < g.w; ++x) {
                    const bool here = (g.at(x, y) < 128) == (white_or_black == 0);
                    if (!here) continue;
                    const auto same = [&](int dx, int dy) { return (g.at(x + dx, y + dy) < 128) == (white_or_black == 0); };
                    if (!same(-1, 0) && !same(1, 0) && !same(0, -1) && !same(0, 1)) ++alone;
                }
            }
            return alone;
        };
        const auto toned = [](double density, double lpi = 60.0) {
            return one_line("speech", Json{{"style", Json{{"fill_tone", Json{{"density", density}, {"lpi", lpi}}}, {"line_rgb", Json::array({0, 0, 0})}}}}, "あ");
        };
        // (between, where neighbouring dots nearly touch — about 60 to 75% at 60 lines and 600 dpi —, the neck between two
        // dots leaves a white speck of a pixel or two: the passing from dots to holes of every screen, not a tone too light
        // or too dark; HANDOFF says so)
        for (const double density : {0.04, 0.06, 0.1, 0.3, 0.5, 0.9}) {
            QVERIFY2(lone(toned(density), 0) == 0, qPrintable(QStringLiteral("black pixels alone at %1").arg(density)));
            QVERIFY2(lone(toned(density), 1) == 0, qPrintable(QStringLiteral("white pixels alone at %1").arg(density)));
        }
        QVERIFY(lone(toned(0.84, 85.0), 1) == 0);  // (85 lines: up to 84%, its holes whole)
        QVERIFY(lone(toned(0.01), 0) > 0);  // (what the check is for)
        // (the check)
        const auto codes = [](const Document& doc) {
            std::vector<std::string> out;
            for (const Json& issue : genko::formats::checks::page_issues(doc, doc.page(0))) {
                const std::string code = issue.value("code", std::string());
                if (code.rfind("balloon_tone", 0) == 0) {
                    out.push_back(code);
                    const QString message = QString::fromStdString(issue.value("message", std::string()));
                    if (QRegularExpression(QStringLiteral("[A-Za-z]{4,}")).match(message).hasMatch() || !message.contains(QStringLiteral("トーン")) || !message.contains(QStringLiteral("%"))) {
                        out.push_back("a message that does not read: " + message.toStdString());
                    }
                    if (issue.value("level", std::string()) != "warning" || issue["target"].value("id", std::string()).empty()) out.push_back("not a warning on its line");
                }
            }
            return out;
        };
        using Codes = std::vector<std::string>;
        QCOMPARE(codes(toned(0.02)), Codes{"balloon_tone_light"});
        QCOMPARE(codes(toned(0.03)), Codes{"balloon_tone_light"});
        QCOMPARE(codes(toned(0.04)), Codes{});
        QCOMPARE(codes(toned(0.5)), Codes{});
        QCOMPARE(codes(toned(0.9)), Codes{});
        QCOMPARE(codes(toned(0.9, 85.0)), Codes{"balloon_tone_dark"});
        QCOMPARE(codes(toned(0.05, 85.0)), Codes{"balloon_tone_light"});
        QCOMPARE(codes(toned(0.5, 85.0)), Codes{});
        QCOMPARE(codes(one_line("speech", Json{{"style", Json{{"fill_tone", Json{{"density", 0}, {"to", 0.5}}}}}})), Codes{"balloon_tone_light"});  // (it begins at nothing)
        QCOMPARE(codes(one_line("speech", Json{{"style", Json{{"fill_tone", Json{{"density", 0.05}, {"to", 0.5}}}}}})), Codes{});
        QCOMPARE(codes(one_line("speech", Json{{"style", Json{{"fill_tone", Json{{"density", 0.1}, {"pattern", "line"}}}}}})), Codes{"balloon_tone_light"});
        QCOMPARE(codes(one_line("speech", Json{{"style", Json{{"fill_tone", Json{{"density", 0.85}, {"pattern", "line"}}}}}})), Codes{"balloon_tone_dark"});
        QCOMPARE(codes(one_line("speech", Json{{"style", Json{{"fill_tone", Json{{"density", 0.02}, {"to", 0.9}, {"lpi", 85}}}}}})), (Codes{"balloon_tone_light", "balloon_tone_dark"}));
        QCOMPARE(codes(one_line("sfx", Json{{"style", Json{{"fill_tone", Json{{"density", 0.02}}}}}})), Codes{});  // (no balloon: no tone)
        Document nothing = toned(0.02);
        nothing.story.front().style["fill_tone"] = Json{{"density", 0}};
        QCOMPARE(codes(nothing), Codes{});
        {  // (a colour page prints its tone as it is)
            Document colour = genko::core::new_episode("フキダシ", genko::core::Num(1), 1, genko::core::PageSpec::b5_doujin());
            colour = run_ops(colour, Json::array({Json{{"op", "add_line"}, {"page", 1}, {"id", "L1"}, {"balloon", "speech"}, {"x_mm", 40}, {"y_mm", 40}, {"w_mm", 50}, {"h_mm", 40},
                                                       {"text", "あ"}, {"style", Json{{"fill_tone", Json{{"density", 0.02}}}}}}}));
            if (colour.page(0).spec.expression == "color") QCOMPARE(codes(colour), Codes{});
        }
        {  // (joined balloons: the tone drawn is the first line's — said once, for that line)
            const Json light{{"fill_tone", Json{{"density", 0.02}}}, {"group", "g"}};
            const Document joined = run_ops(blank_book(), Json::array({Json{{"op", "add_line"}, {"page", 1}, {"id", "L1"}, {"balloon", "speech"}, {"x_mm", 30}, {"y_mm", 40}, {"w_mm", 40},
                                                                            {"h_mm", 34}, {"text", "あ"}, {"style", Json{{"group", "g"}}}},
                                                                       Json{{"op", "add_line"}, {"page", 1}, {"id", "L2"}, {"balloon", "speech"}, {"x_mm", 62}, {"y_mm", 56}, {"w_mm", 40},
                                                                            {"h_mm", 34}, {"text", "い"}, {"style", light}}}));
            QCOMPARE(codes(joined), Codes{});
        }
    }

    // The feature is named for a symbol on a type layer inside a smart object's contents too (the pages a book writes
    // beside its own), and the same picture is drawn whoever draws it: four threads at once, each from nothing, draw the
    // additions' pages as one does.
    void theAdditionsInsideContentsAndOnManyThreads() {
        const auto names = [](const Json& payload, std::string_view feature) {
            const Json& features = payload["features"];
            return std::find(features.begin(), features.end(), Json(std::string(feature))) != features.end();
        };
        Document doc = run_ops(blank_book(), Json::array({Json{{"op", "add_type_layer"}, {"page", 1}, {"text", "すき"}, {"box", Json::array({20, 20, 40, 36})}, {"new_id", "t1"},
                                                               {"balloon", "heart"}, {"style", Json{{"drip_mm", 4}}}}}));
        QVERIFY(names(payload_of(doc), genko::core::kFukidashi2Feature));
        doc = run_ops(doc, Json::array({Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({"t1"})}, {"new_id", "so"}}}));
        QVERIFY(doc.story.empty());
        bool on_the_page = false;
        for (const genko::core::Layer& layer : doc.page(0).layers) on_the_page = on_the_page || (layer.type && layer.type->balloon == "heart");
        QVERIFY(!on_the_page);  // (the record is in the contents now)
        QVERIFY(names(payload_of(doc), genko::core::kFukidashi2Feature));
        // (many threads)
        const Document book = genko::test::additions_book();
        const Json pins = Json::parse(genko::test::read_bytes(genko::test::test_data(QStringLiteral("fukidashi/pins/additions_pages.json"))));
        genko::render::text::clear_balloon_cache();
        std::vector<std::string> drawn(8);
        std::vector<std::thread> threads;
        for (std::size_t t = 0; t < 8; ++t) {
            threads.emplace_back([&, t] { drawn[t] = picture_sha(genko::render::render_page(book.page(t % 4), 150, {}, &book).image); });
            if (threads.size() == 4) {  // (four at a time)
                for (std::thread& thread : threads) thread.join();
                threads.clear();
                genko::render::text::clear_balloon_cache();
            }
        }
        for (std::size_t t = 0; t < 8; ++t) QCOMPARE(drawn[t], pins["additions"]["150"][t % 4].get<std::string>());
    }

    // --- the clean renderer: a tail at a box's corner ---------------------------------------------------------------------------

    // A tail whose root falls on a corner of a box (or of a rounded balloon with a small corner) left the line open
    // there: its base, as deep as asked, was wider than the corner, and the stretch of it outside the box had no line
    // (a defect of the clean renderer of 2026-11-13, found by the review). Such an end of the base is drawn in to just
    // inside the outline. Round each of the four corners — every degree and a half from 15° before the corner's own direction to 15°
    // past it —, for a wedge, a straight tail and a zigzag, on boxes of the sizes the review measured and with wide tails:
    // the line closes round the balloon and its tail.
    void aTailAtABoxCornerClosesItsLine_data() {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<QString>("style");
        QTest::addColumn<QString>("tail");
        const std::pair<const char*, Json> bodies[] = {{"box", Json::object()}, {"box", Json{{"corner_mm", 1.0}}}, {"rounded", Json{{"corner_mm", 0}}},
                                                       {"rounded", Json{{"corner_mm", 1.0}}}, {"rounded", Json{{"corner_mm", 3.0}}}, {"rounded", Json::object()},
                                                       {"box", Json{{"border_mm", 0.8}}}, {"box", Json{{"pen_swell", 0.5}, {"pen_taper", 0.7}}}};
        for (const auto& [kind, style] : bodies) {
            for (const char* tail : {"wedge", "straight", "zigzag"}) {
                QTest::newRow(qPrintable(QStringLiteral("%1 %2, %3").arg(kind, QString::fromStdString(style.dump()), tail))) << QString::fromLatin1(kind) << QString::fromStdString(style.dump()) << QString::fromLatin1(tail);
            }
        }
    }
    void aTailAtABoxCornerClosesItsLine() {
        QFETCH(QString, kind);
        QFETCH(QString, style);
        QFETCH(QString, tail);
        const int dpi = 300;
        const auto px = [dpi](double mm) { return static_cast<int>(std::lround(mm / 25.4 * dpi)); };
        struct Size {
            double w, h, width;
        };
        // (the review's sizes; the last two with a wide tail: 6 and 8 mm on a box of 24 × 18)
        const Size sizes[] = {{30, 24, 0}, {40, 30, 0}, {44, 36, 0}, {50, 46, 0}, {60, 40, 0}, {80, 30, 0}, {24, 18, 6}, {24, 18, 8}};
        int drawn = 0;
        for (const Size& size : sizes) {
            const double x = 30, y = 30;
            const double cx = x + size.w / 2, cy = y + size.h / 2;
            for (int corner = 0; corner < 4; ++corner) {
                const double to_corner = std::atan2((corner < 2 ? 1 : -1) * size.h / 2, (corner == 0 || corner == 3 ? 1 : -1) * size.w / 2) * 180 / 3.141592653589793;
                for (double off = -15.0; off <= 15.0; off += 1.5) {
                    const double angle = (to_corner + off) * 3.141592653589793 / 180;
                    const double ux = std::cos(angle), uy = std::sin(angle);
                    const double edge = std::min(std::fabs(ux) > 1e-9 ? (size.w / 2) / std::fabs(ux) : 1e9, std::fabs(uy) > 1e-9 ? (size.h / 2) / std::fabs(uy) : 1e9);
                    Json one{{"to", Json::array({cx + ux * (edge + 14), cy + uy * (edge + 14)})}};
                    if (tail != "wedge") one["kind"] = tail.toStdString();
                    if (size.width > 0) one["width_mm"] = size.width;
                    const Document doc = one_line(kind.toStdString().c_str(), Json{{"x_mm", x}, {"y_mm", y}, {"w_mm", size.w}, {"h_mm", size.h}, {"tails", Json::array({one})},
                                                                                    {"style", Json::parse(style.toStdString())}});
                    const Grey g = grey_of(balloon_alone(doc, dpi, x + size.w + 50, y + size.h + 50));
                    ++drawn;
                    QVERIFY2(closed_round(g, px(cx), px(cy), 200),
                             qPrintable(QStringLiteral("%1 × %2, corner %3, %4° off it%5: the line is open").arg(size.w).arg(size.h).arg(corner).arg(off).arg(size.width > 0 ? QStringLiteral(", a tail of %1 mm").arg(size.width) : QString())));
                }
            }
        }
        QCOMPARE(drawn, 8 * 4 * 21);
    }

    // A part of the additions' sample is the whole page cut there, every pixel: each page at 600 dpi in tiles, and at 150.
    void aTileOfTheAdditionsIsThePageCutThere_data() {
        QTest::addColumn<int>("dpi");
        QTest::addColumn<int>("page");
        const Document doc = genko::test::additions_book();
        for (std::size_t page = 0; page < doc.pages.size(); ++page) {
            if (doc.story_for_page(genko::core::Num(static_cast<double>(page + 1))).empty()) continue;
            for (const int dpi : {600, 150}) QTest::newRow(qPrintable(QStringLiteral("page %1 at %2").arg(page + 1).arg(dpi))) << dpi << static_cast<int>(page);
        }
    }
    void aTileOfTheAdditionsIsThePageCutThere() {
        QFETCH(int, dpi);
        QFETCH(int, page);
        const Document doc = genko::test::additions_book();
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

    // --- what fukidashi@2 leaves alone ----------------------------------------------------------------------------------

    // The additions (the pen's line, the drips, the symbols, the tone and the blend inside any outline: SPEC BALLOON-02)
    // change nothing of a book that uses none of them: every page of the book of before and of the sample of now, at 150
    // and at 600 dpi, is the picture the build before the additions drew (its sha256: tests/data/fukidashi/pins/pages.json).
    void aBookWithoutTheAdditionsIsDrawnAsBefore_data() {
        QTest::addColumn<QString>("book");
        QTest::addColumn<int>("dpi");
        for (const char* book : {"classic", "clean"}) {
            for (const int dpi : {150, 600}) QTest::newRow(qPrintable(QStringLiteral("%1 at %2").arg(book).arg(dpi))) << QString::fromLatin1(book) << dpi;
        }
    }
    void aBookWithoutTheAdditionsIsDrawnAsBefore() {
        QFETCH(QString, book);
        QFETCH(int, dpi);
        const Json pins = Json::parse(genko::test::read_bytes(genko::test::test_data(QStringLiteral("fukidashi/pins/pages.json"))));
        const Json& held = pins[book.toStdString()][std::to_string(dpi)];
        const Document doc = pinned_book(book.toStdString());
        QCOMPARE(held.size(), doc.pages.size());
        for (std::size_t page = 0; page < doc.pages.size(); ++page) {
            const std::string drawn = picture_sha(page_picture(doc, dpi, page));
            QVERIFY2(drawn == held[page].get<std::string>(), qPrintable(QStringLiteral("page %1: %2").arg(page + 1).arg(QString::fromStdString(drawn))));
        }
    }

    // … and it is saved as the same bytes, naming the same features (tests/data/fukidashi/pins/<book>_project.json: the
    // book as that build wrote it).
    void aBookWithoutTheAdditionsIsSavedAsBefore_data() {
        QTest::addColumn<QString>("book");
        QTest::newRow("classic") << QStringLiteral("classic");
        QTest::newRow("clean") << QStringLiteral("clean");
    }
    void aBookWithoutTheAdditionsIsSavedAsBefore() {
        QFETCH(QString, book);
        const std::string held = genko::test::read_bytes(genko::test::test_data(QStringLiteral("fukidashi/pins/%1_project.json").arg(book)));
        const std::string saved = saved_text(pinned_book(book.toStdString()));
        QVERIFY(!held.empty());
        QVERIFY2(saved == held, "the saved book is not the bytes of before");
        QVERIFY(saved.find("fukidashi@2") == std::string::npos);
    }


    // The additions themselves are held too: the sample of the additions (a page each: the symbols, the pen's line, the
    // drips, the tone and the blend) at 150 and 600 dpi is the picture this build's first drew (its sha256:
    // tests/data/fukidashi/pins/additions_pages.json), and it is saved as the bytes it was then, naming fukidashi@2
    // (additions_project.json) — a later change to how an addition is drawn or kept is seen.
    void theAdditionsAreDrawnAsRecorded_data() {
        QTest::addColumn<int>("dpi");
        QTest::newRow("150 dpi") << 150;
        QTest::newRow("600 dpi") << 600;
    }
    void theAdditionsAreDrawnAsRecorded() {
        QFETCH(int, dpi);
        const Json pins = Json::parse(genko::test::read_bytes(genko::test::test_data(QStringLiteral("fukidashi/pins/additions_pages.json"))));
        const Json& held = pins["additions"][std::to_string(dpi)];
        const Document doc = genko::test::additions_book();
        QCOMPARE(held.size(), doc.pages.size());
        for (std::size_t page = 0; page < doc.pages.size(); ++page) {
            const std::string drawn = picture_sha(page_picture(doc, dpi, page));
            QVERIFY2(drawn == held[page].get<std::string>(), qPrintable(QStringLiteral("page %1: %2").arg(page + 1).arg(QString::fromStdString(drawn))));
        }
    }
    void theAdditionsAreSavedAsRecorded() {
        const std::string held = genko::test::read_bytes(genko::test::test_data(QStringLiteral("fukidashi/pins/additions_project.json")));
        const Document doc = genko::test::additions_book();
        const std::string saved = saved_text(doc);
        QVERIFY2(saved == held, "the saved book is not the bytes recorded");
        // (it names both features — its pages use the kinds and keys of both —, and read back it is the same book, for writing)
        const Json payload = Json::parse(saved);
        QCOMPARE(payload["features"], Json::array({"fukidashi@1", "fukidashi@2"}));
        QTemporaryDir dir;
        const std::filesystem::path path(dir.path().toStdString());
        genko::test::write_project(doc, path);
        const genko::storage::LoadResult read = genko::storage::load_document(path);
        QVERIFY2(read.document.read_only_reason.empty(), read.document.read_only_reason.c_str());
        QVERIFY(saved_text(read.document) == saved);
    }

    // A balloon the page cuts off — past its right edge and its foot — is the part of it that is on the page: the same
    // pixels as on a page big enough for it, with every addition at once (the pen's line, drips, a tone that grows darker,
    // a blend).
    void aBalloonThePageCutsOffIsTheSame() {
        const Json style{{"pen_swell", 0.6}, {"pen_taper", 0.7}, {"drip_mm", 8}, {"drip_seed", 2}, {"fill_tone", Json{{"density", 0.1}, {"to", 0.6}}},
                         {"fill_gradient", Json{{"to", Json::array({255, 200, 120})}}}};
        for (const char* kind : {"speech", "shout", "heart", "box"}) {
            const Document doc = one_line(kind, Json{{"x_mm", 70}, {"y_mm", 60}, {"w_mm", 50}, {"h_mm", 48}, {"tail", Json::array({128, 116})}, {"style", style}});
            for (const int dpi : {150, 600}) {
                const Image small = balloon_alone(doc, dpi, 100, 90);
                const Image big = balloon_alone(doc, dpi, 150, 130);
                QVERIFY2(small.tobytes() == big.crop(genko::render::Box{0, 0, small.width(), small.height()}).tobytes(), kind);
                int dark = 0;
                for (const char c : grey_of(small).v) dark += static_cast<unsigned char>(c) < 128 ? 1 : 0;
                QVERIFY(dark > 100);
            }
        }
    }

    // --- the sample sheets (written when asked: GENKO_FUKIDASHI_OUT) -------------------------------------------------------
    // (not a test of their own, and never a SKIP: the audit of a stage's run refuses every skipped slot. As the screenshots
    // of test_gui_color, they are written at the end of a run that names a folder)
    void cleanupTestCase() {
        const QByteArray out = qgetenv("GENKO_FUKIDASHI_OUT");
        if (out.isEmpty()) return;
        // (GENKO_FUKIDASHI_SHEETS=additions: only the sheets of fukidashi@2's additions)
        if (qgetenv("GENKO_FUKIDASHI_SHEETS") == "additions") {
            std::filesystem::create_directories(std::filesystem::path(out.toStdString()));
            write_addition_sheets(std::filesystem::path(out.toStdString()));
            write_addition_pins(std::filesystem::path(out.toStdString()));
            return;
        }
        write_sample_sheets(out);
    }

private:
    static void write_sample_sheets(const QByteArray& out) {
        const std::filesystem::path dir(out.toStdString());
        std::filesystem::create_directories(dir / "pins-drawn");
        const auto write = [&](const std::string& name, const Image& image) {
            genko::storage::write_atomic(dir / name, genko::formats::png_bytes(image.convert("RGB")));
        };
        {  // (what this build draws and writes of the two sample books: beside tests/data/fukidashi/pins)
            Json pages = Json::object();
            for (const char* book : {"classic", "clean"}) {
                const Document pinned = pinned_book(book);
                for (const int dpi : {150, 600}) {
                    Json list = Json::array();
                    for (std::size_t page = 0; page < pinned.pages.size(); ++page) list.push_back(picture_sha(page_picture(pinned, dpi, page)));
                    pages[book][std::to_string(dpi)] = std::move(list);
                }
                genko::storage::write_atomic(dir / "pins-drawn" / (std::string(book) + "_project.json"), saved_text(pinned));
            }
            genko::storage::write_atomic(dir / "pins-drawn" / "pages.json", pages.dump(2) + "\n");
            write_addition_pins(dir);
        }
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
        write_addition_sheets(dir);
        // (the book of before, as this build draws it: beside the pictures main's build made of it)
        const Document old = genko::storage::load_document(old_book_dir()).document;
        for (std::size_t i = 0; i < old.pages.size(); ++i) write("after-" + std::to_string(i + 1) + ".png", page_picture(old, 200, i));
        const Document old2 = genko::storage::load_document(old_book_dir(QStringLiteral("old_book2"))).document;
        for (std::size_t i = 0; i < old2.pages.size(); ++i) write("after2-" + std::to_string(i + 1) + ".png", page_picture(old2, 200, i));
    }

    // (what this build draws and writes of the additions' sample: beside tests/data/fukidashi/pins)
    static void write_addition_pins(const std::filesystem::path& dir) {
        std::filesystem::create_directories(dir / "pins-drawn");
        const Document doc = genko::test::additions_book();
        Json pages = Json::object();
        for (const int dpi : {150, 600}) {
            Json list = Json::array();
            for (std::size_t page = 0; page < doc.pages.size(); ++page) list.push_back(picture_sha(page_picture(doc, dpi, page)));
            pages["additions"][std::to_string(dpi)] = std::move(list);
        }
        genko::storage::write_atomic(dir / "pins-drawn" / "additions_pages.json", pages.dump(2) + "\n");
        genko::storage::write_atomic(dir / "pins-drawn" / "additions_project.json", saved_text(doc));
    }

    // fukidashi@2: the sample of the additions — each page at 150 dpi, its parts at 600 dpi pixel for pixel, and the same
    // parts made black and white as the monochrome print.
    static void write_addition_sheets(const std::filesystem::path& dir) {
        const auto write = [&](const std::string& name, const Image& image) {
            genko::storage::write_atomic(dir / name, genko::formats::png_bytes(image.convert("RGB")));
        };
        const Document doc = genko::test::additions_book();
        const char* names[] = {"symbols", "pen", "drips", "fills"};
        for (std::size_t page = 0; page < doc.pages.size(); ++page) {
            bool any = false;
            for (const genko::core::StoryLine& line : doc.story) any = any || line.page_index.value() == static_cast<double>(page + 1);
            if (!any) continue;
            const std::string name = names[page];
            write("add-150-" + name + ".png", page_picture(doc, 150, page));
            int part = 0;
            for (const double y : {8.0, 94.0, 180.0}) {
                for (const double x : {12.0, 104.0}) {
                    const Image crop = page_part(doc, 600, x, y, 94, 94, page);
                    const std::string at = std::string(1, static_cast<char>('a' + part++));
                    write("add-600-" + name + "-" + at + ".png", crop);
                    write("add-600-" + name + "-" + at + "-mono.png", genko::render::to_bitonal(crop).convert("L"));
                }
            }
        }
    }
};

QTEST_GUILESS_MAIN(TestFukidashi)
#include "test_fukidashi.moc"
