// PC5 H-a (SPEC VIEW-01, 節 H 6 章 H-a): the view of the page's pixels — the zoom in the page's pixels (the user's decision
// 11: 100% one page pixel a screen pixel, 200%, プリントサイズ the paper), a 600 dpi B4 page zoomed to its pixels (画素の段:
// its own pixels laid as squares), レイヤーを画面サイズに合わせる, ピクセルグリッド (the grid of the layer drawn on) and where it
// shows, ピクセルアートプレビュー, 手のひらツール and ズームツール (a click, Alt+click, a drag, and their options: ズームアウト,
// スクラブズーム — the clicks pc0 found untested), スクリーンモード in F's order, and the 32 bit プレビューオプション window.

#include <QtTest>

#include <QAction>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QMenuBar>
#include <QSpinBox>
#include <QStatusBar>
#include <QToolBar>

#include <cmath>

#include "app/canvas.hpp"
#include "app/comfort.hpp"
#include "app/config.hpp"
#include "app/icons.hpp"
#include "app/inject.hpp"
#include "app/main_window.hpp"
#include "app/theme.hpp"
#include "app/tool_settings.hpp"
#include "core/command_bus.hpp"
#include "gui_support.hpp"
#include "render/hdr_preview.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/raster.hpp"

using namespace gui_test;
using genko::app::MainWindow;
using genko::app::PageCanvas;
using genko::app::Session;
namespace inject = genko::app::inject;
namespace core = genko::core;
namespace render = genko::render;

namespace {

// A paint layer with a block from (x0, y0) to (x1, y1) mm (its working pixels: the page's working grid).
void add_block(core::Document& doc, const std::string& id, double x0, double y0, double x1, double y1) {
    auto& page = doc.edit_page(0);
    core::Layer paint;
    paint.id = id;
    paint.kind = core::LayerKind::Raster;
    paint.panel_clip = false;
    const render::Image blank = render::raster::ensure_raster(page, paint);
    std::string bytes = blank.tobytes();
    const double px = blank.width() / page.spec.width_mm.value();
    for (int y = 0; y < blank.height(); ++y) {
        for (int x = 0; x < blank.width(); ++x) {
            if (x < x0 * px || x >= x1 * px || y < y0 * px || y >= y1 * px) continue;
            const std::size_t i = (std::size_t(y) * std::size_t(blank.width()) + std::size_t(x)) * 4;
            bytes[i] = static_cast<char>(40);
            bytes[i + 1] = static_cast<char>(90);
            bytes[i + 2] = static_cast<char>(160);
            bytes[i + 3] = static_cast<char>(255);
        }
    }
    render::raster::save_raster(page, paint, render::Image::frombytes("RGBA", blank.size(), bytes));
    page.layers.push_back(std::move(paint));
}

// A book in a window, the questions answered by the test.
struct View {
    QTemporaryDir dir;
    fs::path book;
    std::shared_ptr<Session> session;
    std::unique_ptr<Answers> answers = std::make_unique<Answers>();
    std::unique_ptr<MainWindow> window;

    explicit View(const core::Document& doc) {
        book = path_of(dir.filePath("book.genko"));
        write_book(book, doc);
        session = Session::open(book, quick(path_of(dir.filePath("recovery"))));
        window = std::make_unique<MainWindow>(session);
        window->resize(1200, 900);
        window->show();
        canvas()->fit_page();
    }
    ~View() {
        window.reset();
        session.reset();
        answers.reset();
    }
    PageCanvas* canvas() const { return window->canvas(); }
    QAction* act(const char* name) const { return window->action(QString::fromLatin1(name)); }
    void trigger(const char* name) const {
        QAction* a = act(name);
        QVERIFY2(a != nullptr, name);
        a->trigger();
    }
    // (a glide ends at once: the view as it will be)
    void settle() const { QTRY_VERIFY_WITH_TIMEOUT(!canvas()->gliding(), 3000); }
};

double pixel_scale_of(const PageCanvas* c, double dpi) { return dpi / 25.4 / c->devicePixelRatioF(); }

// A 60 × 80 mm colour page at 150 dpi made 32 bits, a precise layer of two colours over it: 1.5 (past white) on the
// left, 0.25 on the right (the page's sRGB values).
core::Document deep_book() {
    core::Document doc = core::new_episode("32 bit", core::Num(1), 1, core::PageSpec::custom(60, 80, 50, 70, 3, 5, 5, 5, 5, 150, "color"));
    doc.edit_page(0).numero = false;
    const Json ops = Json::array(
        {Json{{"op", "put_color_raster"}, {"page", 1}, {"width", 2}, {"height", 1}, {"precision", "f32"},
              {"pixels", Json::array({1.5, 1.5, 1.5, 1.0, 0.25, 0.25, 0.25, 1.0})}},
         Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 32}}});
    return core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor("human:tester")).doc;
}

}  // namespace

class TestGuiView : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        config_folder();
        qRegisterMetaType<genko::app::StrokeInput>();
        qRegisterMetaType<genko::app::BookChange>();
        genko::app::icons::init_resources();
        genko::app::theme::apply(qApp);
    }

    // The user's decision 11: the % is the page's pixels — 100% one page pixel a screen pixel (its own pixels:
    // devicePixelRatio counted), 200% two; プリントサイズ the paper at 96 dpi; the status bar's box and 表示倍率を打ち込む… in
    // the same %, up to 3200%.
    void theZoomIsInThePagesPixels() {
        View v(new_doc(2));  // (A4 at 600 dpi)
        PageCanvas* c = v.canvas();
        QCOMPARE(c->page()->spec.dpi.value(), 600.0);
        v.trigger("act_actual_pixels");
        v.settle();
        QCOMPARE(c->zoom_percent(), 100);
        QVERIFY(qFuzzyCompare(c->scale(), pixel_scale_of(c, 600)));
        QCOMPARE(c->wanted_dpi(), 600);
        QVERIFY(!c->pixel_step());
        v.trigger("act_200_percent");
        v.settle();
        QCOMPARE(c->zoom_percent(), 200);
        QVERIFY(qFuzzyCompare(c->scale(), 2 * pixel_scale_of(c, 600)));
        QCOMPARE(c->wanted_dpi(), 600);  // (the page's own pixels, laid enlarged)
        QVERIFY(c->pixel_step());
        v.trigger("act_actual");  // (プリントサイズ: the paper as large as it is, at 96 dpi)
        v.settle();
        QVERIFY(qFuzzyCompare(c->scale(), 96 / 25.4));
        QCOMPARE(c->zoom_percent(), static_cast<int>(std::lround(96.0 / 600 * 100 * c->devicePixelRatioF())));
        QTRY_VERIFY_WITH_TIMEOUT(v.window->zoom_label()->text().contains(QStringLiteral("表示 %1%").arg(c->zoom_percent())), 3000);
        // the box in the status bar: the same %, 1 to 3200
        QSpinBox* box = nullptr;
        for (QSpinBox* each : v.window->findChildren<QSpinBox*>()) {
            if (each->toolTip() == QStringLiteral("表示倍率。数を打ち込んで Enter")) box = each;
        }
        QVERIFY(box != nullptr);
        QCOMPARE(box->minimum(), 1);
        QCOMPARE(box->maximum(), 3200);
        box->setValue(400);
        QCOMPARE(c->zoom_percent(), 400);
        // 3200% at most (Photoshop's), more asked stays there
        c->set_zoom_percent(3200);
        QCOMPARE(c->zoom_percent(), 3200);
        QVERIFY(qFuzzyCompare(c->scale(), c->max_scale()));
        c->zoom_by(2.0);
        QCOMPARE(c->zoom_percent(), 3200);
        // 表示倍率を打ち込む…: the page's pixels
        v.answers->responder->get_int = [](const QString&, const QString& label, int, int lo, int hi) {
            return label == QStringLiteral("倍率（%。100 で画素等倍）") && lo == 1 && hi == 3200 ? std::optional<int>(50) : std::nullopt;
        };
        v.trigger("act_zoom_value");
        v.settle();
        QCOMPARE(c->zoom_percent(), 50);
        // 画面サイズに合わせる on an A4 at 600 dpi: a small %
        v.trigger("act_fit");
        v.settle();
        QVERIFY(c->zoom_percent() < 25);
        QVERIFY(std::abs(c->zoom_percent_exact() - c->scale() / pixel_scale_of(c, 600) * 100) < 1e-9);
    }

    // 画素の段: a B4 page at 600 dpi zooms to its pixels and past them (before PC5 H-a: 12 screen px a mm, about 305 dpi,
    // and tiles at 432 dpi at most); its tiles there are its own pixels, at the page's resolution.
    void aB4At600DpiZoomsToItsPixels() {
        core::Document doc = core::new_episode("B4", core::Num(1), 1, core::PageSpec::b4_comic());
        View v(doc);
        PageCanvas* c = v.canvas();
        QVERIFY(c->max_scale() > PageCanvas::kMaxScale);
        QVERIFY(qFuzzyCompare(c->max_scale(), PageCanvas::kMaxPixelZoom * pixel_scale_of(c, 600)));
        c->actual_pixels(100);
        c->center_on(128, 182);
        QVERIFY(c->wait_rendered(60000));
        QCOMPARE(c->renderer().shown_dpi(), 600);
        c->actual_pixels(800);
        c->center_on(128, 182);
        QVERIFY(c->pixel_step());
        QVERIFY(c->wait_rendered(60000));
        QCOMPARE(c->renderer().shown_dpi(), 600);  // (no finer: the page's pixels, laid 8 × 8)
        QCOMPARE(c->renderer().pixel_dpi(), 600);
        // half way (432 dpi before; the detail now up to the page's own)
        c->set_zoom_percent(85);
        QVERIFY(c->wanted_dpi() > 432 && c->wanted_dpi() <= 600);
    }

    // レイヤーを画面サイズに合わせる: the box of the layer drawn on (or the ones chosen) fills the view; a layer with nothing
    // on it: the page.
    void fitLayersOnScreen() {
        core::Document doc = core::new_episode("レイヤー", core::Num(1), 1, core::PageSpec::custom(120, 160, 100, 140, 3, 8, 8, 7, 6, 72, "color"));
        add_block(doc, "block", 30, 40, 50, 50);
        View v(doc);
        PageCanvas* c = v.canvas();
        const double fitted = c->scale();
        v.window->set_target_layer("block");
        v.trigger("act_fit_layers");
        v.settle();
        QVERIFY(c->scale() > fitted * 2);
        const QRectF seen = c->seen_mm();
        QVERIFY(seen.contains(QRectF(30.5, 40.5, 19, 9)));
        QVERIFY(seen.width() < 30);  // (the 20 mm block across the view, with its margin)
        // the ink layer, nothing on it: the page
        for (const core::Layer& layer : v.window->current_page()->layers) {
            if (layer.role == core::LayerRole::Ink) v.window->set_target_layer(layer.id);
        }
        v.trigger("act_fit_layers");
        v.settle();
        QVERIFY(c->seen_mm().contains(QRectF(0.5, 0.5, 119, 159)));
    }

    // ピクセルグリッド: the grid of the layer drawn on — a pen layer's the page's resolution, a paint layer's its own pixels
    // (the working 200 dpi of an 8-bit page) —, shown once a cell is 5 screen pixels or more; off by default.
    void thePixelGridIsTheLayersGrid() {
        core::Document doc = new_doc(1);  // (600 dpi)
        add_block(doc, "paint", 20, 20, 30, 30);
        View v(doc);
        PageCanvas* c = v.canvas();
        QAction* grid = v.act("act_pixel_grid");
        QVERIFY(grid->isCheckable() && !grid->isChecked());
        for (const core::Layer& layer : v.window->current_page()->layers) {
            if (layer.role == core::LayerRole::Ink) v.window->set_target_layer(layer.id);
        }
        QCOMPARE(c->pixel_grid_dpi(), 600);
        c->actual_pixels(600);
        QVERIFY(!c->pixel_grid_shown());  // (not asked for)
        grid->trigger();
        QVERIFY(c->show_pixel_grid && c->pixel_grid_shown());
        c->actual_pixels(400);
        QVERIFY(!c->pixel_grid_shown());  // (4 screen pixels a cell)
        v.window->set_target_layer("paint");
        QCOMPARE(c->pixel_grid_dpi(), 200);
        const QSizeF cell = c->pixel_grid_cell_mm();
        QVERIFY(std::abs(cell.width() - 210.0 / render::mm_to_px(210, 200)) < 1e-9);
        c->actual_pixels(200);  // (a 200 dpi pixel: 6 page pixels at 200%... 6 screen pixels)
        QVERIFY(c->pixel_grid_shown());
        c->actual_pixels(100);
        QVERIFY(!c->pixel_grid_shown());
        // painted over the page: the canvas with it differs from without it, in grey lines
        c->actual_pixels(1600);
        c->center_on(25, 25);
        QVERIFY(c->wait_rendered(60000));
        const QImage with = c->grab().toImage();
        grid->trigger();
        QVERIFY(!c->pixel_grid_shown());
        const QImage without = c->grab().toImage();
        QVERIFY(with != without);
    }

    // ピクセルアートプレビュー: the tiles laid nearest (a switch of the view; the book unchanged).
    void pixelArtPreview() {
        View v(new_doc(1));
        QAction* art = v.act("act_pixel_art");
        QVERIFY(art->isCheckable() && !art->isChecked());
        art->trigger();
        QVERIFY(v.canvas()->pixel_art() && v.canvas()->renderer().pixel_art());
        art->trigger();
        QVERIFY(!v.canvas()->pixel_art());
    }

    // 手のひらツール: one of the tools (Photoshop's H), a drag moves the view; its options 100%・画面サイズ・画面にフィット.
    void theHandTool() {
        View v(new_doc(1));
        PageCanvas* c = v.canvas();
        QAction* hand = v.act("act_hand_tool");
        QVERIFY(v.window->tools_group()->actions().contains(hand));
        QCOMPARE(hand->shortcut(), QKeySequence(QStringLiteral("H")));
        hand->trigger();
        QCOMPARE(c->tool(), QStringLiteral("hand"));
        QCOMPARE(v.window->tool_settings()->title()->text(), QStringLiteral("手のひらツール（H）"));
        c->actual_size();
        const QPointF before = c->pan();
        const QPointF a(100, 140), b(80, 120);
        inject::mouse(c, inject::Phase::Press, a);
        for (int i = 1; i <= 8; ++i) inject::mouse(c, inject::Phase::Move, a + (b - a) * (i / 8.0));
        inject::mouse(c, inject::Phase::Release, b);
        const QPointF moved = c->pan() - before;
        QVERIFY2(moved.x() < -40 && moved.y() < -40, qPrintable(QStringLiteral("%1, %2").arg(moved.x()).arg(moved.y())));
        // its page in オプション: 100%, 画面サイズ, 画面にフィット
        v.trigger("act_actual_pixels");
        v.settle();
        QCOMPARE(c->zoom_percent(), 100);
        v.trigger("act_fill_screen");
        v.settle();
        const QRectF seen = c->seen_mm();
        // (the page fills the view: one side just in, the other past it)
        QVERIFY(std::abs(seen.width() - 210) < 1.0 || std::abs(seen.height() - 297) < 1.0);
        QVERIFY(seen.width() <= 210 + 1.0 && seen.height() <= 297 + 1.0);
    }

    // ズームツール: a click zooms in about it, Alt+click out, a drag fills the view with its box (pc0: no test of the
    // clicks); its options — ズームアウト turns the click about, スクラブズーム zooms as it is dragged.
    void theZoomTool() {
        View v(new_doc(1));
        PageCanvas* c = v.canvas();
        v.trigger("act_zoom_tool");
        QCOMPARE(c->tool(), QStringLiteral("zoom"));
        c->actual_size();
        const auto click = [c, &v](const QPointF& mm, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
            inject::mouse(c, inject::Phase::Press, mm, modifiers);
            inject::mouse(c, inject::Phase::Release, mm, modifiers);
            v.settle();
        };
        double scale = c->scale();
        const QPointF at(100, 150);
        const QPointF screen = c->to_widget(at);
        click(at);
        QVERIFY(qFuzzyCompare(c->scale(), scale * 2));
        QVERIFY((c->to_widget(at) - screen).manhattanLength() < 1.0);  // (about the point clicked)
        scale = c->scale();
        click(at, Qt::AltModifier);
        QVERIFY(qFuzzyCompare(c->scale(), scale / 2));
        // a drag: its box fills the view
        scale = c->scale();
        inject::mouse(c, inject::Phase::Press, QPointF(60, 100));
        for (int i = 1; i <= 6; ++i) inject::mouse(c, inject::Phase::Move, QPointF(60 + 6 * i, 100 + 4 * i));
        inject::mouse(c, inject::Phase::Release, QPointF(96, 124));
        v.settle();
        QVERIFY(c->scale() > scale * 2);
        QVERIFY(c->seen_mm().contains(QRectF(61, 101, 34, 22)));
        // ズームアウト: the click zooms out (Alt in)
        auto* mode = v.window->findChild<QComboBox*>(QStringLiteral("zoom_mode"));
        QVERIFY(mode != nullptr);
        QCOMPARE(v.window->tool_settings()->page_for(QStringLiteral("zoom"))->isAncestorOf(mode), true);
        mode->setCurrentIndex(mode->findData(true));
        QVERIFY(c->zoom_out_mode);
        scale = c->scale();
        click(c->seen_mm().center());
        QVERIFY(qFuzzyCompare(c->scale(), scale / 2));
        scale = c->scale();
        click(c->seen_mm().center(), Qt::AltModifier);
        QVERIFY(qFuzzyCompare(c->scale(), scale * 2));
        mode->setCurrentIndex(mode->findData(false));
        // スクラブズーム: dragging right zooms in, left out, about the point pressed (no box)
        v.trigger("act_scrubby_zoom");
        QVERIFY(c->scrubby_zoom);
        c->actual_size();
        scale = c->scale();
        const QPointF from = c->seen_mm().center();
        const QPointF pressed = c->to_widget(from);
        inject::mouse(c, inject::Phase::Press, from);
        for (int i = 1; i <= 10; ++i) inject::mouse(c, inject::Phase::Move, c->to_mm(pressed + QPointF(10.0 * i, 0)));
        const double wider = c->scale();
        QVERIFY(wider > scale * 1.5);
        for (int i = 9; i >= -10; --i) inject::mouse(c, inject::Phase::Move, c->to_mm(pressed + QPointF(10.0 * i, 0)));
        inject::mouse(c, inject::Phase::Release, c->to_mm(pressed + QPointF(-100, 0)));
        v.settle();
        QVERIFY(c->scale() < scale);
        v.trigger("act_scrubby_zoom");
        QVERIFY(!c->scrubby_zoom);
    }

    // スクリーンモード: F goes round 標準 → メニュー付きフルスクリーン (the whole screen, menus and panels kept) → フルスクリーン
    // (the page alone: menus, the status line and the panels hidden) → 標準 (all back, the window as it was).
    void screenModesInFsOrder() {
        View v(new_doc(1));
        MainWindow* w = v.window.get();
        QCOMPARE(v.act("act_screen_next")->shortcut(), QKeySequence(QStringLiteral("F")));
        QVERIFY(v.act("act_screen_standard")->isChecked());
        QCOMPARE(w->screen_mode(), MainWindow::ScreenMode::Standard);
        const bool status = w->statusBar()->isVisible();
        v.trigger("act_screen_next");
        QCOMPARE(w->screen_mode(), MainWindow::ScreenMode::FullWithMenu);
        QVERIFY(w->windowState().testFlag(Qt::WindowFullScreen));
        QVERIFY(w->menuBar()->isVisible());
        QVERIFY(!w->canvas_only()->on());
        QVERIFY(v.act("act_screen_full_menu")->isChecked());
        v.trigger("act_screen_next");
        QCOMPARE(w->screen_mode(), MainWindow::ScreenMode::Full);
        QVERIFY(w->windowState().testFlag(Qt::WindowFullScreen));
        QVERIFY(!w->menuBar()->isVisible());
        QVERIFY(!w->statusBar()->isVisible());
        QVERIFY(w->canvas_only()->on());  // (the panels hidden; a side's come back at its edge)
        QVERIFY(v.act("act_screen_full")->isChecked());
        v.trigger("act_screen_next");
        QCOMPARE(w->screen_mode(), MainWindow::ScreenMode::Standard);
        QVERIFY(!w->windowState().testFlag(Qt::WindowFullScreen));
        QVERIFY(w->menuBar()->isVisible());
        QCOMPARE(w->statusBar()->isVisible(), status);
        QVERIFY(!w->canvas_only()->on());
        QVERIFY(v.act("act_screen_standard")->isChecked());
        // chosen from the menu directly; 原稿だけを表示 the person turned on before stays theirs
        w->canvas_only()->enter();
        v.trigger("act_screen_full");
        v.trigger("act_screen_standard");
        QVERIFY(w->canvas_only()->on());
        w->canvas_only()->leave();
    }

    // The view's commands change only the view: the book on disk keeps its revision and its bytes, nothing is sent as an
    // op (a book of now opens and is drawn as before: the tiles are render_page's — test_tiles, test_gui_canvas).
    void theViewChangesNothingInTheBook() {
        core::Document doc = new_doc(2);
        add_block(doc, "paint", 20, 20, 30, 30);
        View v(doc);
        PageCanvas* c = v.canvas();
        QVERIFY(c->wait_rendered(60000));
        const std::int64_t revision = disk_revision(v.book);
        const core::Document before = read_book(v.book);
        int edits = 0;
        QObject::connect(v.session.get(), &Session::changed, v.window.get(), [&edits](const genko::app::BookChange& change) {
            if (change.why == genko::app::BookChange::Why::Edit) ++edits;
        });
        v.window->set_target_layer("paint");
        for (const char* name : {"act_actual_pixels", "act_200_percent", "act_fit_layers", "act_pixel_grid", "act_pixel_art", "act_actual",
                                 "act_screen_next", "act_screen_next", "act_screen_next", "act_fill_screen", "act_hand_tool", "act_zoom_tool",
                                 "act_32bit_preview", "act_fit"}) {
            v.trigger(name);
            v.settle();
        }
        QVERIFY(c->wait_rendered(60000));
        QCOMPARE(edits, 0);
        QCOMPARE(disk_revision(v.book), revision);
        const core::Document after = read_book(v.book);
        QCOMPARE(after.pages.size(), before.pages.size());
        for (std::size_t i = 0; i < after.pages.size(); ++i) {
            QCOMPARE(after.page(i).id, before.page(i).id);
            QCOMPARE(after.page(i).layers.size(), before.page(i).layers.size());
        }
    }

    // 32 bit プレビューオプション…: on a 32-bit page, its window changes how the page shows (the tab's own; the book not),
    // キャンセル puts it back; a page of another depth is told it is for 32-bit pages.
    void theThirtyTwoBitPreviewWindow() {
        View v(deep_book());
        PageCanvas* c = v.canvas();
        QVERIFY(render::hdr_page(*c->page()));
        QVERIFY(c->renderer().hdr().identity());
        QVERIFY(c->wait_rendered(30000));
        const QImage plain = c->renderer().compose(c->renderer().shown_dpi());
        const auto raster = v.window->session().document().page(0).layers.back().color_raster;
        QVERIFY(raster != nullptr);
        v.answers->responder->exec = [](QDialog* dialog) {
            dialog->findChild<QDoubleSpinBox*>(QStringLiteral("hdr_exposure"))->setValue(-2.0);
            dialog->findChild<QDoubleSpinBox*>(QStringLiteral("hdr_gamma"))->setValue(1.2);
            return int(QDialog::Accepted);
        };
        v.trigger("act_32bit_preview");
        QCOMPARE(c->renderer().hdr().exposure, -2.0);
        QCOMPARE(c->renderer().hdr().gamma, 1.2);
        QVERIFY(c->wait_rendered(30000));
        const QImage darker = c->renderer().compose(c->renderer().shown_dpi());
        QVERIFY(darker != plain);
        QCOMPARE(v.window->session().document().page(0).layers.back().color_raster, raster);  // (the book as it was)
        QCOMPARE(v.window->session().document().page(0).rgb->depth, 32);
        // ハイライト圧縮: exposure and gamma not taken
        v.answers->responder->exec = [](QDialog* dialog) {
            auto* method = dialog->findChild<QComboBox*>(QStringLiteral("hdr_method"));
            method->setCurrentIndex(method->findData(true));
            if (dialog->findChild<QDoubleSpinBox*>(QStringLiteral("hdr_exposure"))->isEnabled()) return int(QDialog::Rejected);
            return int(QDialog::Accepted);
        };
        v.trigger("act_32bit_preview");
        QVERIFY(c->renderer().hdr().highlight_compression);
        // キャンセル: as it was, though changed while open
        v.answers->responder->exec = [](QDialog* dialog) {
            auto* method = dialog->findChild<QComboBox*>(QStringLiteral("hdr_method"));
            method->setCurrentIndex(method->findData(false));
            dialog->findChild<QDoubleSpinBox*>(QStringLiteral("hdr_exposure"))->setValue(3.0);
            return int(QDialog::Rejected);
        };
        v.trigger("act_32bit_preview");
        QVERIFY(c->renderer().hdr().highlight_compression);
        QCOMPARE(c->renderer().hdr().exposure, -2.0);
        // an 8-bit page: said, no window
        View eight(new_doc(1));
        bool asked = false;
        eight.answers->responder->exec = [&asked](QDialog*) {
            asked = true;
            return int(QDialog::Rejected);
        };
        eight.trigger("act_32bit_preview");
        QVERIFY(!asked);
        QVERIFY(eight.window->last_notice().contains(QStringLiteral("32 bit")));
    }
};

QTEST_MAIN(TestGuiView)
#include "test_gui_view.moc"
