// The canvas's own gestures (Python's canvas.py): the panel tool — a drag across a panel cuts it (a nearly level cut is
// level, Alt keeps the angle), a gutter dragged moves, a chosen panel's corner dragged reshapes it, a rectangle drawn in
// 長方形 mode becomes a panel — each arriving as the op Python sends; and the view — Ctrl+wheel and a pinch zoom about
// their point, Space+drag moves the page, the view turns and mirrors without the page changing.

#include <QtTest>

#include <QOpenGLWidget>
#include <QWheelEvent>

#include <functional>

#include <memory>

#include <QListWidget>

#include "app/brush_panel.hpp"
#include "app/canvas.hpp"
#include "app/colours.hpp"
#include "app/config.hpp"
#include "app/layer_panel.hpp"
#include "app/tiles.hpp"
#include "core/strokes.hpp"
#include "render/brushes.hpp"
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

// The page as render_page draws it in proof (what the tiles must compose to).
QImage drawn(const core::Document& doc, std::size_t index, int dpi) {
    genko::render::RenderOptions options;
    options.mode = "proof";
    options.skip_unported = true;
    const auto image = genko::render::render_page(doc.page(index), dpi, options, &doc).image;
    const auto bytes = image.tobytes();
    return QImage(reinterpret_cast<const uchar*>(bytes.data()), image.width(), image.height(), image.width() * 3, QImage::Format_RGB888)
        .convertToFormat(QImage::Format_RGB32);
}

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

    // The nombre (proof) shows the page's number, counted from the first page that is not a cover: a page that comes to
    // another number — a page before it deleted, a page before it made a cover — is drawn again with its new one,
    // though nothing of its own changed.
    void nombreFollowsThePageNumber() {
        auto doc = std::make_shared<core::Document>(core::new_episode(
            "ノンブル", Num(1), 6, core::PageSpec::custom(70, 95, 60, 85, 3, 8, 8, 7, 6)));
        genko::app::PageRenderer renderer;
        renderer.show(doc, 4);  // (page 5)
        renderer.want(72, 72, QRectF(0, 0, 70, 95));
        QTRY_VERIFY_WITH_TIMEOUT(renderer.settled(), 10000);
        const QImage five = renderer.compose(72);
        QCOMPARE(five, drawn(*doc, 4, 72));
        // page 2 (no lines on it) deleted: the page is page 4 now
        auto fewer = std::make_shared<core::Document>(*doc);
        fewer->pages.erase(fewer->pages.begin() + 1);
        for (std::size_t i = 1; i < fewer->pages.size(); ++i) fewer->edit_page(i).index = Num(static_cast<std::int64_t>(i + 1));
        QCOMPARE(fewer->page(3).id, doc->page(4).id);
        renderer.show(fewer, 3);
        QTRY_VERIFY_WITH_TIMEOUT(renderer.settled(), 10000);
        const QImage four = renderer.compose(72);
        QVERIFY(four != five);
        QCOMPARE(four, drawn(*fewer, 3, 72));
        // page 1 made a cover (the page itself the same): one page fewer counted before it
        auto covered = std::make_shared<core::Document>(*fewer);
        covered->edit_page(0).extra["cover"] = Json::object({{"kind", "front"}});
        QCOMPARE(covered->pages[3].get(), fewer->pages[3].get());
        renderer.show(covered, 3);
        QTRY_VERIFY_WITH_TIMEOUT(renderer.settled(), 10000);
        QVERIFY(renderer.compose(72) != four);
        QCOMPARE(renderer.compose(72), drawn(*covered, 3, 72));
    }

    // The nombre takes a white halo where something dark lies under it: a line drawn under part of it — in one tile,
    // where the nombre spans two — draws all of it again, so both halves have the halo (white on the page's light paper),
    // as the whole page drawn has.
    void nombreHaloFollowsWhatIsUnderIt() {
        // (118.2 mm wide: the nombre's middle, 59.1 mm, is at pixel 512 at 220 dpi, between two columns of tiles)
        auto doc = std::make_shared<core::Document>(core::new_episode(
            "ノンブルの白フチ", Num(1), 6, core::PageSpec::custom(118.2, 95, 108.2, 85, 3, 8, 8, 7, 6)));
        doc->nombre = Json::object({{"start", 1001}});  // ("1005": wide enough to span both)
        doc->edit_page(4).extra["paper_rgb"] = Json::array({240, 238, 236});  // (light enough for no halo, which shows on it)
        for (auto& layer : doc->edit_page(4).layers) {
            if (layer.role == core::LayerRole::Ink) layer.panel_clip = false;  // (its lines run out of the panel, under the nombre)
        }
        genko::app::PageRenderer renderer;
        renderer.show(doc, 4);
        renderer.want(220, 220, QRectF(0, 0, 118.2, 95));
        QTRY_VERIFY_WITH_TIMEOUT(renderer.settled(), 10000);
        QCOMPARE(renderer.compose(220), drawn(*doc, 4, 220));
        const auto nombres = genko::render::nombre_areas(doc->page(4), 220, doc.get());
        QCOMPARE(nombres.size(), std::size_t{1});
        QVERIFY(nombres[0].x0 < 512 && nombres[0].x1 > 512);  // (it spans two tiles)
        // a short dark line under its left half, in the left tile alone
        auto lined = std::make_shared<core::Document>(*doc);
        auto stroke = std::make_shared<core::Stroke>();
        stroke->id = "under-nombre";
        stroke->points = {{56.2, 86.0}, {57.2, 86.1}, {57.9, 86.0}};
        stroke->pressure = {1.0, 1.0, 1.0};
        stroke->width_mm = 0.4;
        for (auto& layer : lined->edit_page(4).layers) {
            if (layer.role == core::LayerRole::Ink) layer.strokes = core::make_strokes({stroke});
        }
        const auto line = genko::render::brushes::extent({1024, 823}, core::stroke_points(*stroke), 220, 0.4, "gpen");
        QVERIFY(line);
        const std::string where = "line " + std::to_string(line->x0) + "," + std::to_string(line->y0) + "-" + std::to_string(line->x1) + "," +
                                  std::to_string(line->y1) + "; nombre " + std::to_string(nombres[0].x0) + "," + std::to_string(nombres[0].y0) +
                                  "-" + std::to_string(nombres[0].x1) + "," + std::to_string(nombres[0].y1);
        QVERIFY2(line->x1 + 2 < 512 && line->x1 > nombres[0].x0 && line->y0 < nombres[0].y1 && line->y1 > nombres[0].y0, where.c_str());
        const auto generation = renderer.generation();
        renderer.show(lined, 4);
        QVERIFY(renderer.generation() > generation);
        QTRY_VERIFY_WITH_TIMEOUT(renderer.settled(), 10000);
        const QImage before = drawn(*doc, 4, 220);
        const QImage after = drawn(*lined, 4, 220);
        QVERIFY(before.copy(512, nombres[0].y0, nombres[0].x1 - 512, nombres[0].y1 - nombres[0].y0) !=
                after.copy(512, nombres[0].y0, nombres[0].x1 - 512, nombres[0].y1 - nombres[0].y0));  // (the halo, in the other tile)
        QCOMPARE(renderer.compose(220), after);
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

    void jpegAppearsInTheStudioWindow_data() {
        QTest::addColumn<QString>("name");
        for (const char* name : {"gray", "rgb444", "rgb420", "progressive", "cmyk"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }

    void jpegAppearsInTheStudioWindow() {
        QFETCH(QString, name);
        Desk desk;
        QVERIFY(QTest::qWaitForWindowExposed(desk.window.get()));
        desk.canvas()->fit_page();
        QTRY_VERIFY_WITH_TIMEOUT(desk.canvas()->renderer().settled(), 10000);
        const auto original = desk.session->snapshot();
        const auto referenceAt = [](const genko::app::DocPtr& doc, int dpi) {
            genko::render::RenderOptions options;
            options.mode = "proof";
            options.skip_unported = true;  // Matches preview; nombre is separately pending in M4.
            const auto image = genko::render::render_page(doc->page(0), dpi, options, doc.get()).image;
            const auto bytes = image.tobytes();
            return QImage(reinterpret_cast<const uchar*>(bytes.data()), image.width(), image.height(),
                          image.width() * 3, QImage::Format_RGB888).convertToFormat(QImage::Format_RGB32);
        };
        const QString path = QStringLiteral(GENKO_TEST_DATA) + "/pyref/jpeg/" + name + ".jpg";
        desk.session->apply(Json::array({Json::object({{"op", "put_raster"}, {"page", 1}, {"layer", "bg"},
                                                    {"path", path.toStdString()}})}));
        QTRY_VERIFY_WITH_TIMEOUT(desk.canvas()->renderer().settled(), 10000);
        const auto doc = desk.session->snapshot();
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas()->renderer().compose(desk.canvas()->renderer().shown_dpi()),
                                 referenceAt(doc, desk.canvas()->renderer().shown_dpi()), 10000);
        const int dpi = desk.canvas()->renderer().shown_dpi();
        const QImage shown = desk.canvas()->renderer().compose(dpi);
        QVERIFY(shown != referenceAt(original, dpi));
        QCoreApplication::processEvents();
        const QString folder = qEnvironmentVariable("GENKO_TEST_SCREENSHOT_DIR");
        if (!folder.isEmpty()) {
            QVERIFY(QDir().mkpath(folder));
            QVERIFY(desk.window->grab().save(folder + "/jpeg-" + name + ".png"));
        }
        desk.session->undo();
        // Fit-to-page may choose a new DPI after the window layout changes; compare at the real current DPI.
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas()->renderer().compose(desk.canvas()->renderer().shown_dpi()),
                                 referenceAt(original, desk.canvas()->renderer().shown_dpi()), 10000);
        desk.session->redo();
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas()->renderer().compose(desk.canvas()->renderer().shown_dpi()),
                                 referenceAt(doc, desk.canvas()->renderer().shown_dpi()), 10000);
        QVERIFY(desk.session->wait_saved(std::chrono::seconds(10)));
        const auto loaded = std::make_shared<core::Document>(read_book(desk.book));
        QCOMPARE(referenceAt(loaded, dpi), shown);
    }

    void bmpAppearsInTheStudioWindow_data() {
        QTest::addColumn<QString>("name");
        for (const char* name : {"rgb24", "gray8", "palette8", "mono1", "rgb32", "rgb24-topdown", "os2-rgb24", "rgb16", "rgb16-565", "palette4", "bitfields32-0", "bitfields32-1", "bitfields32-2", "bitfields32-3", "bitfields32-4", "bitfields32-5", "bitfields32-6", "bitfields32-7", "rgb24-no-lastpad", "rle4-encoded", "rle4-absolute", "rle8-encoded", "rle8-absolute", "rle8-delta", "rle8-gray-topdown", "rgb24-zero-offset", "palette8-zero-offset", "rle8-short-complete", "rle4-short-complete"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }

    void bmpAppearsInTheStudioWindow() {
        QFETCH(QString, name);
        Desk desk;
        QVERIFY(QTest::qWaitForWindowExposed(desk.window.get()));
        desk.canvas()->fit_page();
        QTRY_VERIFY_WITH_TIMEOUT(desk.canvas()->renderer().settled(), 10000);
        const auto original = desk.session->snapshot();
        const auto referenceAt = [](const genko::app::DocPtr& doc, int dpi) {
            genko::render::RenderOptions options;
            options.mode = "proof";
            options.skip_unported = true;  // Matches preview; nombre is separately pending in M4.
            const auto image = genko::render::render_page(doc->page(0), dpi, options, doc.get()).image;
            const auto bytes = image.tobytes();
            return QImage(reinterpret_cast<const uchar*>(bytes.data()), image.width(), image.height(),
                          image.width() * 3, QImage::Format_RGB888).convertToFormat(QImage::Format_RGB32);
        };
        const QString path = QStringLiteral(GENKO_TEST_DATA) + "/pyref/bmp/" + name + ".bmp";
        desk.session->apply(Json::array({Json::object({{"op", "put_raster"}, {"page", 1}, {"layer", "bg"},
                                                    {"path", path.toStdString()}})}));
        QTRY_VERIFY_WITH_TIMEOUT(desk.canvas()->renderer().settled(), 10000);
        const auto doc = desk.session->snapshot();
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas()->renderer().compose(desk.canvas()->renderer().shown_dpi()),
                                 referenceAt(doc, desk.canvas()->renderer().shown_dpi()), 10000);
        const int dpi = desk.canvas()->renderer().shown_dpi();
        const QImage shown = desk.canvas()->renderer().compose(dpi);
        QVERIFY(shown != referenceAt(original, dpi));
        QCoreApplication::processEvents();
        const QString folder = qEnvironmentVariable("GENKO_TEST_SCREENSHOT_DIR");
        if (!folder.isEmpty()) {
            QVERIFY(QDir().mkpath(folder));
            QVERIFY(desk.window->grab().save(folder + "/bmp-" + name + ".png"));
        }
        desk.session->undo();
        // Fit-to-page may choose a new DPI after the window layout changes; compare at the real current DPI.
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas()->renderer().compose(desk.canvas()->renderer().shown_dpi()),
                                 referenceAt(original, desk.canvas()->renderer().shown_dpi()), 10000);
        desk.session->redo();
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas()->renderer().compose(desk.canvas()->renderer().shown_dpi()),
                                 referenceAt(doc, desk.canvas()->renderer().shown_dpi()), 10000);
        QVERIFY(desk.session->wait_saved(std::chrono::seconds(10)));
        const auto loaded = std::make_shared<core::Document>(read_book(desk.book));
        QCOMPARE(referenceAt(loaded, dpi), shown);
    }

    void gifAppearsInTheStudioWindow_data() {
        QTest::addColumn<QString>("name");
        for (const char* name : {"palette", "transparent", "gray", "local-only", "local-override", "offset", "offset-transparent", "extent-expanded", "comment-application", "unknown-plaintext", "interlace", "animated-first-frame", "zero-logical-screen", "no-palette", "tiny-interlace-1", "tiny-interlace-2", "tiny-interlace-3", "tiny-interlace-4", "tiny-interlace-5", "tiny-interlace-6", "tiny-interlace-7", "frame-padding", "empty-extensions-with-terminators", "three-byte-GCE", "short-palette-2", "short-palette-4", "colored-global-gray-local", "gray-transparent-offset", "interlace-offset", "multiple-GCE"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }

    void gifAppearsInTheStudioWindow() {
        QFETCH(QString, name);
        Desk desk;
        QVERIFY(QTest::qWaitForWindowExposed(desk.window.get()));
        desk.canvas()->fit_page();
        QTRY_VERIFY_WITH_TIMEOUT(desk.canvas()->renderer().settled(), 10000);
        const auto original = desk.session->snapshot();
        const auto referenceAt = [](const genko::app::DocPtr& doc, int dpi) {
            genko::render::RenderOptions options;
            options.mode = "proof";
            options.skip_unported = true;  // Matches preview; nombre is separately pending in M4.
            const auto image = genko::render::render_page(doc->page(0), dpi, options, doc.get()).image;
            const auto bytes = image.tobytes();
            return QImage(reinterpret_cast<const uchar*>(bytes.data()), image.width(), image.height(),
                          image.width() * 3, QImage::Format_RGB888).convertToFormat(QImage::Format_RGB32);
        };
        const QString path = QStringLiteral(GENKO_TEST_DATA) + "/pyref/gif/" + name + ".gif";
        desk.session->apply(Json::array({Json::object({{"op", "put_raster"}, {"page", 1}, {"layer", "bg"},
                                                    {"path", path.toStdString()}})}));
        QTRY_VERIFY_WITH_TIMEOUT(desk.canvas()->renderer().settled(), 10000);
        const auto doc = desk.session->snapshot();
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas()->renderer().compose(desk.canvas()->renderer().shown_dpi()),
                                 referenceAt(doc, desk.canvas()->renderer().shown_dpi()), 10000);
        const int dpi = desk.canvas()->renderer().shown_dpi();
        const QImage shown = desk.canvas()->renderer().compose(dpi);
        QVERIFY(shown != referenceAt(original, dpi));
        QCoreApplication::processEvents();
        const QString folder = qEnvironmentVariable("GENKO_TEST_SCREENSHOT_DIR");
        if (!folder.isEmpty()) {
            QVERIFY(QDir().mkpath(folder));
            QVERIFY(desk.window->grab().save(folder + "/gif-" + name + ".png"));
        }
        desk.session->undo();
        // Fit-to-page may choose a new DPI after the window layout changes; compare at the real current DPI.
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas()->renderer().compose(desk.canvas()->renderer().shown_dpi()),
                                 referenceAt(original, desk.canvas()->renderer().shown_dpi()), 10000);
        desk.session->redo();
        QTRY_COMPARE_WITH_TIMEOUT(desk.canvas()->renderer().compose(desk.canvas()->renderer().shown_dpi()),
                                 referenceAt(doc, desk.canvas()->renderer().shown_dpi()), 10000);
        QVERIFY(desk.session->wait_saved(std::chrono::seconds(10)));
        const auto loaded = std::make_shared<core::Document>(read_book(desk.book));
        QCOMPARE(referenceAt(loaded, dpi), shown);
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

    // PC2d (a stall while drawing, found at PC1's exit): a part of the canvas painted again — as a line being drawn asks
    // for its piece — is the same pixels as the whole canvas painted there: inside the page (where the shadow under the
    // sheet and the shaded bleed bands are left out, none of them showing there), across its edge and the trim line,
    // with the view turned and mirrored too.
    void aPartPaintedIsTheWholePaintedThere() {
        Desk desk;
        PageCanvas* c = desk.canvas();
        QVERIFY(c->wait_rendered(10000));
        const double w = desk.page().spec.width_mm.value(), h = desk.page().spec.height_mm.value();
        for (const bool turned : {false, true}) {
            if (turned) {
                c->rotate_view(15);
                c->flip_view(true);
                QVERIFY(c->wait_rendered(10000));
            }
            QImage whole(c->size(), QImage::Format_ARGB32_Premultiplied);
            whole.fill(Qt::transparent);
            c->render(&whole);
            const auto around = [&](double x_mm, double y_mm, int half) {
                const QPointF at = c->to_widget(QPointF(x_mm, y_mm));
                return QRect(QPoint(int(at.x()) - half, int(at.y()) - half), QSize(2 * half, 2 * half)).intersected(c->rect());
            };
            const auto trim = desk.page().trim_rect_mm();
            for (const QRect& part : {around(w / 2, h / 2, 12), around(w / 3, h / 4, 3), around(0, 0, 14), around(w, h / 2, 10),
                                      around(trim.x.value(), trim.y.value(), 6), around(w / 2, trim.y.value() + trim.height.value(), 5)}) {
                QVERIFY(!part.isEmpty());
                QImage painted(c->size(), QImage::Format_ARGB32_Premultiplied);
                painted.fill(Qt::transparent);
                c->render(&painted, part.topLeft(), QRegion(part));  // (the part painted where it is)
                for (int y = part.top(); y <= part.bottom(); ++y)
                    for (int x = part.left(); x <= part.right(); ++x)
                        if (painted.pixel(x, y) != whole.pixel(x, y)) QFAIL(qPrintable(QStringLiteral("%1,%2 (turned %3)").arg(x).arg(y).arg(turned)));
            }
        }
    }

    // PC2d (stalls while drawing, older than PC1, found at its exit): 履歴 is written to the settings a moment after a
    // run of lines, not at each line (each write of settings.ini locks it and syncs it to the disk) — the same value,
    // and a panel made meanwhile reads it —; the layers' small pictures are drawn again only when the checker under them
    // changes (the look is refreshed for other reasons too: the computer's light or dark).
    void drawingDoesNotWaitForTheDisk() {
        Desk s;
        genko::app::ColourPanel* colours = s.window->colours();
        QVERIFY(colours != nullptr);
        genko::app::settings()->remove(QStringLiteral("colour/history"));
        colours->remember({1, 2, 3});
        colours->remember({4, 5, 6});
        QVERIFY(!genko::app::settings()->contains(QStringLiteral("colour/history")));  // (not yet: a moment later)
        QTRY_VERIFY_WITH_TIMEOUT(genko::app::settings()->contains(QStringLiteral("colour/history")), 8000);
        QVERIFY(genko::app::settings()->value(QStringLiteral("colour/history")).toString().startsWith(QStringLiteral("4,5,6;1,2,3")));
        colours->remember({7, 8, 9});
        {
            genko::app::BrushPanel brush;
            genko::app::ColourPanel again(&brush);  // (made meanwhile: it reads the colour just used)
            QCOMPARE(again.history().front(), (genko::app::Rgb{7, 8, 9}));
        }
        // the small pictures: kept when the look is refreshed with the same checker, drawn again over another
        QVERIFY(s.window->apply_ops(Json::array({Json{{"op", "add_stroke"}, {"page", 1}, {"layer", "ink"},
                                                      {"points", Json::array({Json::array({20, 20}), Json::array({80, 90})})}, {"width_mm", 1.0}}})));
        genko::app::settings()->setValue(QStringLiteral("view/checker_size"), QStringLiteral("medium"));
        const auto picture = [&] {
            QListWidget* list = s.window->layer_panel()->list();
            for (int i = 0; i < list->count(); ++i)
                if (list->item(i)->data(Qt::UserRole + 1).toString() == QStringLiteral("picture")) return list->item(i)->icon().cacheKey();
            return qint64(-1);
        };
        QMetaObject::invokeMethod(s.window.get(), "refresh_icons", Qt::DirectConnection);
        const qint64 before = picture();
        QVERIFY(before != -1);
        QMetaObject::invokeMethod(s.window.get(), "refresh_icons", Qt::DirectConnection);
        QCOMPARE(picture(), before);
        genko::app::settings()->setValue(QStringLiteral("view/checker_size"), QStringLiteral("large"));
        QMetaObject::invokeMethod(s.window.get(), "refresh_icons", Qt::DirectConnection);
        QVERIFY(picture() != before);
        genko::app::settings()->remove(QStringLiteral("view/checker_size"));
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
        QVERIFY(std::abs(c->scale() / before_pinch - 1.5) < 1e-6 || c->scale() == c->max_scale());
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

    // render:GPU-CPU-parity (PC5 H-a, 節 H 4.3; the coordinator's 3 章: the processor composites, the card only shows): the
    // page on the graphics card is the page the processor draws — within 1/255 everywhere as it is, mirrored, zoomed into
    // its pixels and proofed. (The user's decision 2026-10-29:) turned, or zoomed off one tile pixel a screen pixel (fitted), the card's texture filter and the
    // processor's painter resample the picture each their own way (both bilinear, their weights rounded otherwise): within
    // 1/255 at all but the edges of sharp lines (at
    // most 2 pixels in 1000 apart, never by more than 12/255, Mesa's llvmpipe), and the sheet's edges and soft shadow (the
    // polygons around the page, smoothed by the card's 4 samples a pixel) are left out. Where OpenGL cannot be had (the
    // offscreen test screen), the card's surface gives up, the canvas draws by the processor again and says so
    // (gpuFailed) — the same pixels; never skipped. (QT_QPA_PLATFORM=xcb on Xvfb and Mesa checks the card's way.)
    void theCardShowsWhatTheProcessorDraws() {
        Desk desk;
        PageCanvas* c = desk.canvas();
        c->show_guides = false;
        c->actual_size();
        c->center_on(105, 148);
        QVERIFY(c->wait_rendered(30000));
        const auto cpu = [c] { return c->grab().toImage().convertToFormat(QImage::Format_RGB32); };
        QStringList failed;
        QStringList apart;  // (the views where the card's picture is further from the processor's than allowed)
        QObject::connect(c, &PageCanvas::gpuFailed, c, [&failed](const QString& message) { failed << message; });
        // each view: the processor's picture, then the card's
        struct Look {
            const char* name;
            std::function<void()> set;
        };
        const std::vector<Look> looks{
            {"as it is", [] {}},
            {"mirrored", [c] { c->flip_view(true); }},
            {"its pixels", [c] { c->actual_pixels(800); }},
            {"proofed", [c] { c->renderer().set_proof(true); }},
            {"turned", [c] {
                 c->renderer().set_proof(false);
                 c->actual_size();
                 c->center_on(105, 148);
                 c->set_rotation(30);
             }},
            {"turned and mirrored", [c] { c->flip_view(false); }},
            {"fitted", [c] {
                 c->reset_view();
                 c->fit_page();
             }},
            // (review P3-6) the gamut warning's mark over the tiles; the finer level (288 dpi, one tile pixel a screen
            // pixel); the pixel art preview (fitted: laid nearest, made smaller); the page's pixels turned (nearest, turned)
            {"gamut", [c] {
                 c->actual_pixels(48);
                 c->center_on(105, 148);
                 c->renderer().set_gamut(genko::app::GamutLook{});
             }},
            {"finer level", [c] { c->renderer().set_gamut(std::nullopt); }},
            {"pixel art", [c] {
                 c->fit_page();
                 c->set_pixel_art(true);
             }},
            {"its pixels turned", [c] {
                 c->set_pixel_art(false);
                 c->actual_pixels(800);
                 c->center_on(105, 148);
                 c->set_rotation(30);
             }},
        };
        for (const Look& look : looks) {
            look.set();
            QVERIFY(c->wait_rendered(30000));
            c->use_gpu(false);
            const QImage processor = cpu();
            c->use_gpu(true);
            QVERIFY(c->renderer().texture_budget().has_value());
            // (the card's context made — or given up, and the canvas back on the processor)
            QTRY_VERIFY_WITH_TIMEOUT(!c->gpu() || qobject_cast<QOpenGLWidget*>(c->gpu_surface())->isValid(), 5000);
            if (!c->gpu()) {
                // (no OpenGL here: given up, drawn by the processor again, said once)
                QVERIFY(!c->renderer().texture_budget());
                QCOMPARE(failed.size(), 1);
                QVERIFY(failed.front().contains(QStringLiteral("プロセッサー")));
                QCOMPARE(cpu(), processor);
                failed.clear();
                continue;
            }
            auto* surface = qobject_cast<QOpenGLWidget*>(c->gpu_surface());
            const QImage shown = surface->grabFramebuffer().convertToFormat(QImage::Format_RGB32).scaled(processor.size());
            QCOMPARE(shown.size(), processor.size());
            // (resampled: turned, or zoomed off one tile pixel a screen pixel — bilinear, or nearest made smaller or turned)
            const bool nearest = std::string_view(look.name) == "pixel art";
            const bool resampled = c->rotation() != 0.0 || std::string_view(look.name) == "fitted" || nearest;
            // (resampled: the pixels within 14 of the sheet's edges — its edge and its shadow, 1.6 to 12.6 screen pixels out —
            // are not the page's)
            const double w = c->page()->spec.width_mm.value(), h = c->page()->spec.height_mm.value();
            const std::array<QPointF, 4> sheet{c->to_widget(QPointF(0, 0)), c->to_widget(QPointF(w, 0)), c->to_widget(QPointF(w, h)),
                                               c->to_widget(QPointF(0, h))};
            const auto on_edge = [&sheet](double x, double y) {
                for (std::size_t i = 0; i < sheet.size(); ++i) {
                    const QPointF a = sheet[i], d = sheet[(i + 1) % sheet.size()] - sheet[i];
                    const double length = d.x() * d.x() + d.y() * d.y();
                    if (length <= 0) continue;
                    const double t = std::clamp(((x - a.x()) * d.x() + (y - a.y()) * d.y()) / length, 0.0, 1.0);
                    if (std::hypot(x - (a.x() + t * d.x()), y - (a.y() + t * d.y())) < 14.0) return true;
                }
                return false;
            };
            int most = 0;
            QPoint where;
            std::size_t counted = 0, off = 0;
            for (int y = 0; y < shown.height(); ++y) {
                for (int x = 0; x < shown.width(); ++x) {
                    if (resampled && on_edge(x + 0.5, y + 0.5)) continue;
                    const QRgb a = shown.pixel(x, y);
                    const auto apart_from = [&a](QRgb b) {
                        return std::max({std::abs(qRed(a) - qRed(b)), std::abs(qGreen(a) - qGreen(b)), std::abs(qBlue(a) - qBlue(b))});
                    };
                    int apart_here = apart_from(processor.pixel(x, y));
                    // (nearest made smaller — ピクセルアートプレビュー —: where a screen pixel falls half way, the card and the
                    // processor may each take the page pixel on either side: the card's is one of the processor's next to it)
                    if (nearest && apart_here > 1) {
                        for (int dy = -1; dy <= 1; ++dy)
                            for (int dx = -1; dx <= 1; ++dx)
                                if (processor.rect().contains(x + dx, y + dy)) apart_here = std::min(apart_here, apart_from(processor.pixel(x + dx, y + dy)));
                    }
                    ++counted;
                    if (apart_here > 1) ++off;
                    if (apart_here > most) {
                        most = apart_here;
                        where = QPoint(x, y);
                    }
                }
            }
            const bool within = resampled ? most <= 12 && off * 1000 <= counted * 2 : most <= 1;
            if (!within) {
                apart << QStringLiteral("%1: %2/255 at %3, %4 (%5 of %6 pixels past 1/255)")
                             .arg(QString::fromLatin1(look.name))
                             .arg(most)
                             .arg(where.x())
                             .arg(where.y())
                             .arg(off)
                             .arg(counted);
            }
            if (qEnvironmentVariableIsSet("HA_REPORT")) qInfo("%s: %d/255, %zu of %zu past 1/255", look.name, most, off, counted);
            QVERIFY(failed.isEmpty());
            QVERIFY(c->renderer().texture_bytes() > 0);  // (the tiles on the card counted)
        }
        QVERIFY2(apart.isEmpty(), qPrintable(apart.join(QStringLiteral("; "))));
        c->use_gpu(false);
        QVERIFY(!c->renderer().texture_budget());
    }
};

QTEST_MAIN(TestGuiCanvas)
#include "test_gui_canvas.moc"
