// The pen on the canvas (M2-G1 試験 2). Lines drawn with the mouse and with a pen — QMouseEvent, and QTabletEvent with
// pressure, tilt and the barrel's turn — arrive packed as Python's canvas packs them (stroke.pack_point: no pressure is
// 0.7, kept in 0.05..1, |xTilt|/60 adds up to a quarter; a tap is a dot 0.01 mm long), go to the book as one add_stroke
// with the pen's fields and the id the live line was drawn with, and the live line's pixels are the committed line's
// pixels — the renderer's layer_image at the canvas's resolution, 100 % zoom — for every brush, without tolerance:
// while the pen moves (with the brush's settings that leave the line as drawn) and after it lifts (with the brush's own
// settings: steadying, tapered ends).

#include <QtTest>

#include <QSignalSpy>

#include <cmath>
#include <memory>

#include "app/canvas.hpp"
#include "app/icons.hpp"
#include "app/inject.hpp"
#include "app/live_ink.hpp"
#include "app/main_window.hpp"
#include "app/session.hpp"
#include "app/theme.hpp"
#include "core/brushes.hpp"
#include "core/stroke_geom.hpp"
#include "gui_support.hpp"
#include "render/page.hpp"

using namespace gui_test;
using genko::app::MainWindow;
using genko::app::PageCanvas;
using genko::app::Session;
using genko::app::StrokeInput;
namespace inject = genko::app::inject;
namespace render = genko::render;
namespace core = genko::core;

namespace {

// A book of two pages open in a window at 100 % (the canvas's resolution 96 dpi), the pen in hand.
struct Desk {
    QTemporaryDir dir;
    fs::path book;
    std::shared_ptr<Session> session;
    std::unique_ptr<MainWindow> window;
    std::vector<StrokeInput> lines;  // what the canvas handed over
    std::vector<Json> ops;           // what reached the book

    Desk() {
        book = path_of(dir.filePath("book.genko"));
        make_book(book, 2);
        session = Session::open(book, quick(path_of(dir.filePath("recovery"))));
        window = std::make_unique<MainWindow>(session);
        window->resize(1200, 900);
        window->show();
        canvas()->set_zoom_percent(100);
        canvas()->center_on(105, 148);
        window->choose_tool(QStringLiteral("pen"));
        QObject::connect(canvas(), &PageCanvas::strokeCommitted, window.get(), [this](const StrokeInput& s) { lines.push_back(s); });
        QObject::connect(session.get(), &Session::changed, window.get(), [this](const genko::app::BookChange& c) {
            if (c.why == genko::app::BookChange::Why::Edit) ops.push_back(c.ops);
        });
    }
    ~Desk() {
        window.reset();
        session.reset();
    }
    PageCanvas* canvas() const { return window->canvas(); }
    const core::Page& page() const { return session->document().page(0); }
    const core::Layer& ink() const { return *ink_of(page()); }
    void pen(const genko::app::PenSettings& settings) {
        window->pen() = settings;
        window->pen_changed();
    }
};

// A wave across the middle of the page seen, with the pressure rising and falling.
std::vector<inject::PenInput> wave(const QRectF& seen, int count = 28) {
    std::vector<inject::PenInput> points;
    const QPointF c = seen.center();
    for (int i = 0; i < count; ++i) {
        const double t = static_cast<double>(i) / (count - 1);
        inject::PenInput p;
        p.mm = QPointF(c.x() - 35 + 70 * t, c.y() + 12 * std::sin(t * 6.0));
        p.pressure = 0.15 + 0.85 * std::sin(t * 3.14159);
        points.push_back(p);
    }
    return points;
}

QString describe_difference(const render::Image& a, const render::Image& b) {
    if (a.mode() != b.mode() || a.width() != b.width() || a.height() != b.height()) {
        return QStringLiteral("%1 %2×%3 vs %4 %5×%6")
            .arg(QString::fromStdString(std::string(a.mode())))
            .arg(a.width())
            .arg(a.height())
            .arg(QString::fromStdString(std::string(b.mode())))
            .arg(b.width())
            .arg(b.height());
    }
    const std::string x = a.tobytes();
    const std::string y = b.tobytes();
    int differing = 0;
    int largest = 0;
    int first = -1;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const int d = std::abs(static_cast<int>(static_cast<unsigned char>(x[i])) - static_cast<int>(static_cast<unsigned char>(y[i])));
        if (d == 0) continue;
        ++differing;
        largest = std::max(largest, d);
        if (first < 0) first = static_cast<int>(i);
    }
    return QStringLiteral("%1 bytes differ (largest %2, first at pixel %3)").arg(differing).arg(largest).arg(first / 4);
}

// The committed line as the renderer draws its layer, cut to `box` (page pixels at dpi); and whether the line drew
// nothing outside the box.
render::Image rendered(const Desk& desk, int dpi, const QRect& box, bool& inside) {
    const render::Image layer = render::layer_image(desk.page(), desk.ink(), dpi, &desk.session->document());
    const auto drawn = layer.getbbox();
    inside = !drawn || (drawn->x0 >= box.x() && drawn->y0 >= box.y() && drawn->x1 <= box.x() + box.width() && drawn->y1 <= box.y() + box.height());
    return layer.crop(render::Box{box.x(), box.y(), box.x() + box.width(), box.y() + box.height()});
}

}  // namespace

class TestGuiPen : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        config_folder();
        qRegisterMetaType<StrokeInput>();
        qRegisterMetaType<genko::app::BookChange>();
        genko::app::icons::init_resources();
        genko::app::theme::apply(qApp);
    }

    void theMouseLineIsPackedAsPython() {
        Desk desk;
        const QPointF c = desk.canvas()->seen_mm().center();
        const std::vector<QPointF> mm = {c, c + QPointF(2.5, 1.25), c + QPointF(5, 3), c + QPointF(10, 4)};
        std::vector<QPointF> at;
        for (const QPointF& p : mm) at.push_back(desk.canvas()->to_widget(p));
        inject::mouse_stroke(desk.canvas(), mm);
        QCOMPARE(desk.lines.size(), std::size_t{1});
        const StrokeInput& line = desk.lines[0];
        QCOMPARE(line.tool, QStringLiteral("pen"));
        QCOMPARE(line.points.size(), mm.size());
        for (std::size_t i = 0; i < mm.size(); ++i) {
            // (Python's _to_mm of the event's position, and pack_point(x, y): no pressure is 0.7)
            const QPointF expected = desk.canvas()->to_mm(at[i]);
            QCOMPARE(line.points[i].x, expected.x());
            QCOMPARE(line.points[i].y, expected.y());
            QVERIFY(line.points[i].p.has_value());
            QCOMPARE(*line.points[i].p, 0.7);
        }
        // one add_stroke: these points, the pen's fields, the ink layer of page 1
        QCOMPARE(desk.ops.size(), std::size_t{1});
        const Json& op = desk.ops[0][0];
        QCOMPARE(op["op"].get<std::string>(), std::string("add_stroke"));
        QCOMPARE(op["page"], Json(1));
        QCOMPARE(op["layer_id"].get<std::string>(), desk.ink().id);
        QCOMPARE(op["points"].size(), mm.size());
        for (std::size_t i = 0; i < mm.size(); ++i) {
            QCOMPARE(op["points"][i], Json::array({line.points[i].x, line.points[i].y, 0.7}));
        }
        const Json fields = desk.window->pen().stroke_fields();
        for (const auto& [key, value] : fields.items()) QCOMPARE(op[key], value);
        QVERIFY(!op.contains("rotation"));
        // the line in the book is the one the live line was drawn as (its id: the seed of its texture)
        QCOMPARE(desk.ink().stroke_count(), std::size_t{1});
        QCOMPARE(desk.ink().strokes->items[0]->id, line.id);
    }

    void thePenLineIsPackedAsPython() {
        Desk desk;
        const QPointF c = desk.canvas()->seen_mm().center();
        const std::vector<double> pressures = {0.02, 0.3, 0.75, 1.0, 0.5};
        std::vector<inject::PenInput> points;
        for (std::size_t i = 0; i < pressures.size(); ++i) {
            inject::PenInput p;
            p.mm = c + QPointF(4.0 * static_cast<double>(i), 1.5 * static_cast<double>(i));
            p.pressure = pressures[i];
            p.x_tilt = -30;  // (|xTilt| / 60: a tilt of 0.5, a pressure an eighth more)
            p.y_tilt = 20;   // (Python reads only xTilt)
            p.rotation = 12.34 * static_cast<double>(i);
            points.push_back(p);
        }
        std::vector<QPointF> at;
        for (const auto& p : points) at.push_back(desk.canvas()->to_widget(p.mm));
        inject::tablet_stroke(desk.canvas(), points);
        QCOMPARE(desk.lines.size(), std::size_t{1});
        const StrokeInput& line = desk.lines[0];
        QCOMPARE(line.points.size(), points.size());
        QCOMPARE(line.rotation.size(), points.size());
        for (std::size_t i = 0; i < points.size(); ++i) {
            const QPointF mm = desk.canvas()->to_mm(at[i]);
            const core::PenPoint expected = core::pack_point(mm.x(), mm.y(), points[i].pressure, 30.0 / 60.0);
            QCOMPARE(line.points[i].x, expected.x);
            QCOMPARE(line.points[i].y, expected.y);
            QCOMPARE(*line.points[i].p, *expected.p);
            QCOMPARE(line.rotation[i], points[i].rotation);
        }
        // (the light touch is kept at 0.05 before the tilt's eighth is added, the full one at 1: 0.05625, 0.3375,
        // 0.84375, 1.0, 0.5625)
        QCOMPARE(*line.points[0].p, 0.05 * (1.0 + 0.25 * 0.5));
        QCOMPARE(*line.points[3].p, 1.0);
        const Json& op = desk.ops.at(0)[0];
        for (std::size_t i = 0; i < points.size(); ++i) QCOMPARE(op["points"][i][2].get<double>(), *line.points[i].p);
        // a pen that reports its barrel's turn: the turns go with the line, to a tenth of a degree
        QVERIFY(op.contains("rotation"));
        for (std::size_t i = 0; i < points.size(); ++i) QCOMPARE(op["rotation"][i].get<double>(), core::py_round(points[i].rotation, 1));
    }

    void aPressureWithoutTiltIsKeptAsGiven() {
        Desk desk;
        const QPointF c = desk.canvas()->seen_mm().center();
        std::vector<inject::PenInput> points(3);
        for (int i = 0; i < 3; ++i) {
            points[static_cast<std::size_t>(i)].mm = c + QPointF(3.0 * i, 0);
            points[static_cast<std::size_t>(i)].pressure = 0.25 + 0.25 * i;
        }
        inject::tablet_stroke(desk.canvas(), points);
        const StrokeInput& line = desk.lines.at(0);
        QCOMPARE(*line.points[0].p, 0.25);
        QCOMPARE(*line.points[1].p, 0.5);
        QCOMPARE(*line.points[2].p, 0.75);
        QVERIFY(!desk.ops.at(0)[0].contains("rotation"));  // (no turns reported: none sent)
    }

    void aTapIsADot() {
        Desk desk;
        const QPointF c = desk.canvas()->seen_mm().center();
        const QPointF at = desk.canvas()->to_widget(c);
        inject::mouse(desk.canvas(), inject::Phase::Press, c);
        inject::mouse(desk.canvas(), inject::Phase::Release, c);
        QCOMPARE(desk.lines.size(), std::size_t{1});
        const QPointF mm = desk.canvas()->to_mm(at);
        QCOMPARE(desk.lines[0].points.size(), std::size_t{2});
        QCOMPARE(desk.lines[0].points[1].x, mm.x() + 0.01);
        QCOMPARE(desk.lines[0].points[1].y, mm.y() + 0.01);
        QCOMPARE(*desk.lines[0].points[1].p, 0.7);
        // with the pen: the dot keeps the pen's pressure
        inject::PenInput tap;
        tap.mm = c + QPointF(10, 10);
        tap.pressure = 0.4;
        const QPointF at2 = desk.canvas()->to_widget(tap.mm);
        inject::tablet(desk.canvas(), inject::Phase::Press, tap);
        inject::tablet(desk.canvas(), inject::Phase::Release, tap);
        QCOMPARE(desk.lines.size(), std::size_t{2});
        const QPointF mm2 = desk.canvas()->to_mm(at2);
        QCOMPARE(desk.lines[1].points.size(), std::size_t{2});
        QCOMPARE(desk.lines[1].points[1].x, mm2.x() + 0.01);
        QCOMPARE(desk.lines[1].points[1].y, mm2.y() + 0.01);
        QCOMPARE(*desk.lines[1].points[1].p, 0.4);
        QCOMPARE(desk.ink().stroke_count(), std::size_t{2});
    }

    void thePenTurnedOverErases() {
        Desk desk;
        const QPointF c = desk.canvas()->seen_mm().center();
        auto line = wave(desk.canvas()->seen_mm(), 10);
        inject::tablet_stroke(desk.canvas(), line);
        QCOMPARE(desk.ink().stroke_count(), std::size_t{1});
        std::vector<inject::PenInput> across(4);
        for (int i = 0; i < 4; ++i) {
            across[static_cast<std::size_t>(i)].mm = QPointF(c.x(), c.y() - 20 + 13.0 * i);
            across[static_cast<std::size_t>(i)].pressure = 0.8;
        }
        inject::tablet_stroke(desk.canvas(), across, /*eraser_end=*/true);
        QCOMPARE(desk.lines.back().tool, QStringLiteral("eraser"));
        QCOMPARE(desk.ops.back()[0]["op"].get<std::string>(), std::string("erase"));
        QVERIFY(desk.ink().stroke_count() >= 2);  // (cut where the eraser passed)
        QCOMPARE(desk.canvas()->tool(), QStringLiteral("pen"));  // (back to the pen's end)
    }

    void theLiveLineIsTheCommittedLine_data() {
        QTest::addColumn<QString>("kind");
        for (const core::Brush& brush : core::builtin_brushes()) {
            QTest::newRow(brush.key.c_str()) << QString::fromStdString(brush.key);
        }
    }

    // While the pen moves: the live line's pixels are the committed line's (the brush's settings that leave the line
    // as drawn: no steadying, no tapered ends).
    void theLiveLineIsTheCommittedLine() {
        QFETCH(QString, kind);
        Desk desk;
        genko::app::PenSettings pen = genko::app::PenSettings::for_kind(kind.toStdString());
        pen.stabilize = 0;
        pen.taper = false;
        desk.pen(pen);
        const auto points = wave(desk.canvas()->seen_mm());
        inject::tablet(desk.canvas(), inject::Phase::Press, points.front());
        for (std::size_t i = 1; i < points.size(); ++i) {
            inject::tablet(desk.canvas(), inject::Phase::Move, points[i]);
            QCoreApplication::processEvents();  // (painted as it goes)
        }
        const genko::app::LiveInk* live = desk.canvas()->live();
        QVERIFY(live != nullptr);
        QCOMPARE(live->dpi(), 96);
        const render::Image shown = live->patch();
        const QRect box = live->box();
        inject::tablet(desk.canvas(), inject::Phase::Release, points.back());
        QCOMPARE(desk.ink().stroke_count(), std::size_t{1});
        bool inside = false;
        const render::Image committed = rendered(desk, 96, box, inside);
        QVERIFY2(inside, "the committed line drew outside the live line's box");
        QVERIFY2(shown.tobytes() == committed.tobytes() && shown.mode() == committed.mode(),
                 qPrintable(kind + QStringLiteral(": ") + describe_difference(shown, committed)));
    }

    void theCommittedLineShowsAsItRenders_data() { theLiveLineIsTheCommittedLine_data(); }

    // After the pen lifts (the brush's own settings): the picture shown until the tiles are drawn is the renderer's.
    void theCommittedLineShowsAsItRenders() {
        QFETCH(QString, kind);
        Desk desk;
        desk.pen(genko::app::PenSettings::for_kind(kind.toStdString()));
        inject::tablet_stroke(desk.canvas(), wave(desk.canvas()->seen_mm()));
        QCOMPARE(desk.ink().stroke_count(), std::size_t{1});
        const genko::app::LiveInk* kept = desk.canvas()->overlay(0);
        QVERIFY(kept != nullptr);
        QCOMPARE(kept->seed(), desk.ink().strokes->items[0]->id);
        bool inside = false;
        const render::Image committed = rendered(desk, kept->dpi(), kept->box(), inside);
        QVERIFY2(inside, "the committed line drew outside its picture's box");
        const render::Image shown = kept->patch();
        QVERIFY2(shown.tobytes() == committed.tobytes() && shown.mode() == committed.mode(),
                 qPrintable(kind + QStringLiteral(": ") + describe_difference(shown, committed)));
        // and once the tiles are drawn, the picture gives way to them
        QVERIFY(desk.canvas()->wait_rendered(30000));
        QVERIFY(wait_for([&] { return desk.canvas()->overlays() == 0; }, 10000));
    }
};

QTEST_MAIN(TestGuiPen)
#include "test_gui_pen.moc"
