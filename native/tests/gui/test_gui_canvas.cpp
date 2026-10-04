// The canvas's own gestures (Python's canvas.py): the panel tool — a drag across a panel cuts it (a nearly level cut is
// level, Alt keeps the angle), a gutter dragged moves, a chosen panel's corner dragged reshapes it, a rectangle drawn in
// 長方形 mode becomes a panel — each arriving as the op Python sends; and the view — Ctrl+wheel and a pinch zoom about
// their point, Space+drag moves the page, the view turns and mirrors without the page changing.

#include <QtTest>

#include <QWheelEvent>

#include <memory>

#include "app/canvas.hpp"
#include "app/tiles.hpp"
#include "render/page.hpp"
#include "app/frame_tools.hpp"
#include "app/icons.hpp"
#include "app/inject.hpp"
#include "app/main_window.hpp"
#include "app/theme.hpp"
#include "gui_support.hpp"

using namespace gui_test;
using genko::app::MainWindow;
using genko::app::PageCanvas;
using genko::app::Session;
using genko::core::Num;
namespace inject = genko::app::inject;
namespace core = genko::core;

namespace {

struct Desk {
    QTemporaryDir dir;
    fs::path book;
    std::shared_ptr<Session> session;
    std::unique_ptr<MainWindow> window;
    std::vector<Json> ops;

    Desk() {
        book = path_of(dir.filePath("book.genko"));
        make_book(book, 2);
        session = Session::open(book, quick(path_of(dir.filePath("recovery"))));
        window = std::make_unique<MainWindow>(session);
        window->resize(1200, 900);
        window->show();
        canvas()->fit_page();
        QObject::connect(session.get(), &Session::changed, window.get(), [this](const genko::app::BookChange& c) {
            if (c.why == genko::app::BookChange::Why::Edit) ops.push_back(c.ops[0]);
        });
    }
    ~Desk() {
        window.reset();
        session.reset();
    }
    PageCanvas* canvas() const { return window->canvas(); }
    const core::Page& page() const { return *window->current_page(); }
    // A drag with the left button, from a to b (page mm), in steps.
    void drag(const QPointF& a, const QPointF& b, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        inject::mouse(canvas(), inject::Phase::Press, a, modifiers);
        for (int i = 1; i <= 8; ++i) inject::mouse(canvas(), inject::Phase::Move, a + (b - a) * (i / 8.0), modifiers);
        inject::mouse(canvas(), inject::Phase::Release, b, modifiers);
    }
};

QPointF centre(const core::Frame& f) { return QPointF(f.rect.x.value() + f.rect.width.value() / 2, f.rect.y.value() + f.rect.height.value() / 2); }

}  // namespace

class TestGuiCanvas : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        config_folder();
        qRegisterMetaType<genko::app::StrokeInput>();
        qRegisterMetaType<genko::app::BookChange>();
        genko::app::icons::init_resources();
        genko::app::theme::apply(qApp);
    }

    void onionReferenceChangesRefreshTheVisibleTiles() {
        auto doc = std::make_shared<core::Document>(core::new_episode(
            "オニオン試験", Num(1), 3, core::PageSpec::custom(70, 95, 60, 85, 3, 8, 8, 7, 6)));
        doc->edit_page(0).paint(core::LayerRole::Bg, core::NumList{Num(240), Num(40), Num(20)});
        doc->edit_page(1).onion_from = Num(1);
        genko::app::PageRenderer renderer;
        renderer.show(doc, 1);
        renderer.want(72, 72, QRectF(0, 0, 70, 95));
        QTRY_VERIFY_WITH_TIMEOUT(renderer.settled(), 10000);
        const QImage before = renderer.compose(72);
        const auto generation = renderer.generation();
        auto updated = std::make_shared<core::Document>(*doc);
        updated->edit_page(0).paint(core::LayerRole::Bg, core::NumList{Num(20), Num(40), Num(240)});
        QCOMPARE(updated->pages[1].get(), doc->pages[1].get());
        renderer.show(updated, 1);
        QVERIFY(renderer.generation() > generation);
        QTRY_VERIFY_WITH_TIMEOUT(renderer.settled(), 10000);
        const QImage after = renderer.compose(72);
        QVERIFY(after != before);
        genko::render::RenderOptions options;
        options.mode = "proof";
        options.skip_unported = true;
        const auto expected = genko::render::render_page(updated->page(1), 72, options, updated.get()).image;
        const auto bytes = expected.tobytes();
        const QImage reference(reinterpret_cast<const uchar*>(bytes.data()), expected.width(), expected.height(),
                               expected.width() * 3, QImage::Format_RGB888);
        QCOMPARE(after, reference.convertToFormat(QImage::Format_RGB32));
        renderer.set_mode("print");
        QTRY_VERIFY_WITH_TIMEOUT(renderer.settled(), 10000);
        const QImage printed = renderer.compose(72);
        const auto print_generation = renderer.generation();
        auto again = std::make_shared<core::Document>(*updated);
        again->edit_page(0).paint(core::LayerRole::Bg, core::NumList{Num(80), Num(200), Num(40)});
        renderer.show(again, 1);
        QCOMPARE(renderer.generation(), print_generation);
        QTRY_VERIFY_WITH_TIMEOUT(renderer.settled(), 10000);
        QCOMPARE(renderer.compose(72), printed);
    }

    void unrelatedPageChangesKeepCurrentOnionTiles() {
        auto doc = std::make_shared<core::Document>(core::new_episode(
            "オニオン局所性", Num(1), 3, core::PageSpec::custom(70, 95, 60, 85, 3, 8, 8, 7, 6)));
        doc->edit_page(1).onion_from = Num(1);
        genko::app::PageRenderer renderer;
        renderer.show(doc, 1);
        renderer.want(72, 72, QRectF(0, 0, 70, 95));
        QTRY_VERIFY_WITH_TIMEOUT(renderer.settled(), 10000);
        const auto generation = renderer.generation();
        const QImage before = renderer.compose(72);
        auto updated = std::make_shared<core::Document>(*doc);
        updated->edit_page(2).paint(core::LayerRole::Bg, core::NumList{Num(20), Num(40), Num(240)});
        renderer.show(updated, 1);
        QCOMPARE(renderer.generation(), generation);
        QVERIFY(renderer.settled());
        QCOMPARE(renderer.compose(72), before);
    }

    void onionReferenceAppearsAndDisappears() {
        auto doc = std::make_shared<core::Document>(core::new_episode(
            "参照先の変化", Num(1), 3, core::PageSpec::custom(70, 95, 60, 85, 3, 8, 8, 7, 6)));
        doc->edit_page(0).index = Num(42);
        doc->edit_page(0).paint(core::LayerRole::Bg, core::NumList{Num(240), Num(40), Num(20)});
        doc->edit_page(1).onion_from = Num(1);
        genko::app::PageRenderer renderer;
        renderer.show(doc, 1);
        renderer.want(72, 72, QRectF(0, 0, 70, 95));
        QTRY_VERIFY_WITH_TIMEOUT(renderer.settled(), 10000);
        const QImage absent = renderer.compose(72);
        const auto generation = renderer.generation();
        auto appeared = std::make_shared<core::Document>(*doc);
        appeared->edit_page(0).index = Num(1);
        QCOMPARE(appeared->pages[1].get(), doc->pages[1].get());
        renderer.show(appeared, 1);
        QVERIFY(renderer.generation() > generation);
        QTRY_VERIFY_WITH_TIMEOUT(renderer.settled(), 10000);
        QVERIFY(renderer.compose(72) != absent);
        const auto appearance_generation = renderer.generation();
        auto gone = std::make_shared<core::Document>(*appeared);
        gone->edit_page(0).index = Num(42);
        renderer.show(gone, 1);
        QVERIFY(renderer.generation() > appearance_generation);
        QTRY_VERIFY_WITH_TIMEOUT(renderer.settled(), 10000);
        QCOMPARE(renderer.compose(72), absent);
    }

    void onionUpdatesInTheStudioWindow() {
        Desk desk;
        desk.session->apply(Json::array({
            Json::object({{"op", "add_layer"}, {"page", 2}, {"kind", "paint"}, {"id", "onion-paint"}}),
            Json::object({{"op", "fill_area"}, {"page", 2}, {"layer_id", "onion-paint"},
                          {"area", Json::object({{"rect", Json::array({20, 20, 100, 100})}})}, {"rgb", Json::array({240, 40, 20})}}),
            Json::object({{"op", "set_onion"}, {"page", 1}, {"from", 2}})}));
        QTRY_VERIFY_WITH_TIMEOUT(desk.canvas()->renderer().settled(), 10000);
        const int dpi = desk.canvas()->renderer().shown_dpi();
        const QImage before = desk.canvas()->renderer().compose(dpi);
        const auto generation = desk.canvas()->renderer().generation();
        const QString folder = qEnvironmentVariable("GENKO_TEST_SCREENSHOT_DIR");
        if (!folder.isEmpty()) {
            QVERIFY(QDir().mkpath(folder));
            QVERIFY(desk.window->grab().save(folder + "/onion-before.png"));
        }
        desk.session->apply(Json::array({
            Json::object({{"op", "fill_area"}, {"page", 2}, {"layer_id", "onion-paint"},
                          {"area", Json::object({{"rect", Json::array({20, 20, 100, 100})}})}, {"rgb", Json::array({20, 40, 240})}})}));
        QVERIFY(desk.canvas()->renderer().generation() > generation);
        QTRY_VERIFY_WITH_TIMEOUT(desk.canvas()->renderer().settled(), 10000);
        QVERIFY(desk.canvas()->renderer().compose(dpi) != before);
        QCoreApplication::processEvents();
        if (!folder.isEmpty()) QVERIFY(desk.window->grab().save(folder + "/onion-after.png"));
    }

    void aNearlyLevelCutIsLevel() {
        Desk desk;
        desk.window->choose_tool(QStringLiteral("frame"));
        const core::Frame& root = *desk.page().leaf_frames().front();
        const QPointF c = centre(root);
        desk.drag(QPointF(c.x() - 60, c.y()), QPointF(c.x() + 60, c.y() + 3));  // (3 mm over 120: within the snap)
        QCOMPARE(desk.ops.size(), std::size_t{1});
        const Json& op = desk.ops[0];
        QCOMPARE(op["op"].get<std::string>(), std::string("cut_frame"));
        QCOMPARE(op["p0"][1], op["p1"][1]);
        QCOMPARE(op["gutter_mm"].get<double>(), 6.0);  // (a level cut: the gutter between tiers)
        QCOMPARE(desk.page().leaf_frames().size(), std::size_t{2});
    }

    void altKeepsTheAngle() {
        Desk desk;
        desk.window->choose_tool(QStringLiteral("frame"));
        const QPointF c = centre(*desk.page().leaf_frames().front());
        desk.drag(QPointF(c.x() - 60, c.y()), QPointF(c.x() + 60, c.y() + 3), Qt::AltModifier);
        QCOMPARE(desk.ops.size(), std::size_t{1});
        const Json& op = desk.ops[0];
        QVERIFY(std::abs(op["p1"][1].get<double>() - op["p0"][1].get<double>() - 3.0) < 0.05);
        QCOMPARE(desk.page().leaf_frames().size(), std::size_t{2});
    }

    void aGutterDraggedMoves() {
        Desk desk;
        desk.window->choose_tool(QStringLiteral("frame"));
        const QPointF c = centre(*desk.page().leaf_frames().front());
        desk.drag(QPointF(c.x() - 60, c.y()), QPointF(c.x() + 60, c.y()));
        const auto gutters = genko::app::gutters(desk.page().frames[0]);
        QCOMPARE(gutters.size(), std::size_t{1});
        const QPointF middle = (gutters[0].p0 + gutters[0].p1) / 2;
        const double top_before = desk.page().leaf_frames().front()->rect.height.value();
        desk.drag(middle, middle + QPointF(0, 10));
        const Json& op = desk.ops.back();
        QCOMPARE(op["op"].get<std::string>(), std::string("move_gutter"));
        QVERIFY(std::abs(op["delta_mm"].get<double>() - 10.0) < 0.05);
        // (the gutter went down: the upper panel grew by that much)
        QVERIFY(std::abs(desk.page().leaf_frames().front()->rect.height.value() - top_before - 10.0) < 0.1);
    }

    void aChosenPanelsCornerReshapesIt() {
        Desk desk;
        desk.window->choose_tool(QStringLiteral("frame"));
        const core::Frame& root = *desk.page().leaf_frames().front();
        const std::string id = root.id;
        const QPointF corner(root.rect.x.value(), root.rect.y.value());
        // a click (no drag) with the panel tool chooses the panel: its corners get handles
        inject::click_mm(desk.canvas(), centre(root));
        QCOMPARE(desk.page().selected_frame_id.get<std::string>(), id);
        desk.drag(corner, corner + QPointF(8, 5));
        const Json& op = desk.ops.back();
        QCOMPARE(op["op"].get<std::string>(), std::string("set_frame"));
        QCOMPARE(op["frame_id"].get<std::string>(), id);
        QVERIFY(op.contains("poly"));
        const core::Frame& shaped = *desk.page().find_frame(id);
        QVERIFY(shaped.poly.has_value());
        bool moved = false;
        for (const core::Point& p : *shaped.poly) {
            if (std::abs(p.x.value() - (corner.x() + 8)) < 0.05 && std::abs(p.y.value() - (corner.y() + 5)) < 0.05) moved = true;
        }
        QVERIFY(moved);
    }

    void aRectangleDrawnIsAPanel() {
        Desk desk;
        desk.window->choose_tool(QStringLiteral("frame"));
        desk.canvas()->frame_mode = QStringLiteral("rect");
        desk.drag(QPointF(40, 50), QPointF(120, 140));
        const Json& op = desk.ops.back();
        QCOMPARE(op["op"].get<std::string>(), std::string("add_frame"));
        QVERIFY(op.contains("rect"));
        QVERIFY(std::abs(op["rect"][2].get<double>() - 80) < 0.05 && std::abs(op["rect"][3].get<double>() - 90) < 0.05);
        QVERIFY(desk.window->last_notice().startsWith(QStringLiteral("コマを描きました。")));
    }

    void theViewZoomsMovesTurnsAndMirrors() {
        Desk desk;
        PageCanvas* c = desk.canvas();
        const auto before = desk.page().id;
        const double scale = c->scale();
        // Ctrl+wheel: zoom in about the point under the pointer (that page point stays put)
        const QPointF at(c->width() / 2.0 + 40, c->height() / 2.0 - 30);
        const QPointF under = c->to_mm(at);
        QWheelEvent wheel(at, c->mapToGlobal(at), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(c, &wheel);
        QVERIFY(c->scale() > scale);
        const QPointF still = c->to_widget(under);
        QVERIFY(std::abs(still.x() - at.x()) < 1.0 && std::abs(still.y() - at.y()) < 1.0);
        // a pinch: spread to zoom about the fingers
        const double before_pinch = c->scale();
        c->pinch(1.5, 0.0, at, QPointF());
        QVERIFY(std::abs(c->scale() / before_pinch - 1.5) < 1e-6 || c->scale() == PageCanvas::kMaxScale);
        // Space + drag: the page moves with the pointer
        c->setFocus();
        QTest::keyPress(c, Qt::Key_Space);
        const QPointF pan = c->pan();
        QMouseEvent press(QEvent::MouseButtonPress, at, c->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(c, &press);
        const QPointF to = at + QPointF(50, 20);
        QMouseEvent move(QEvent::MouseMove, to, c->mapToGlobal(to), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(c, &move);
        QMouseEvent release(QEvent::MouseButtonRelease, to, c->mapToGlobal(to), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(c, &release);
        QTest::keyRelease(c, Qt::Key_Space);
        QVERIFY(std::abs(c->pan().x() - pan.x() - 50) < 0.5 && std::abs(c->pan().y() - pan.y() - 20) < 0.5);
        // turned and mirrored: a page point still maps back to itself; the page is unchanged
        c->rotate_view(15);
        c->flip_view(true);
        const QPointF mm(100, 150);
        const QPointF back = c->to_mm(c->to_widget(mm));
        QVERIFY(std::abs(back.x() - mm.x()) < 1e-6 && std::abs(back.y() - mm.y()) < 1e-6);
        QCOMPARE(desk.page().id, before);
        QVERIFY(desk.ops.empty());
    }
};

QTEST_MAIN(TestGuiCanvas)
#include "test_gui_canvas.moc"
