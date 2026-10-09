// The canvas's own gestures (Python's canvas.py): the panel tool — a drag across a panel cuts it (a nearly level cut is
// level, Alt keeps the angle), a gutter dragged moves, a chosen panel's corner dragged reshapes it, a rectangle drawn in
// 長方形 mode becomes a panel — each arriving as the op Python sends; and the view — Ctrl+wheel and a pinch zoom about
// their point, Space+drag moves the page, the view turns and mirrors without the page changing.

#include <QtTest>

#include <QWheelEvent>

#include <memory>

#include "app/canvas.hpp"
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
