// The look and comfort over long sessions (Python's tests/test_ui_look.py, UI-A … UI-G, and the 環境設定 of
// test_j1, test_w14_speed, test_f6, test_hd_report, test_w15_color and test_t_report) on the offscreen platform: light,
// dark and the brightness step, the canvas's neutral surround, hints into tooltips, grey icons, the start screen, the
// page alone (Tab), the stages' layouts, the round menu at the pen, the rest reminder, fewer moving things and the
// letters at once, the bundled letters, the clean grey row, the top bar and the panel rows, the layers' rows, the
// rarely used panels, the icons, the folding settings, the brush list, the empty lines list, the zoom's glide and the
// page let go, the system's glass, the page on the graphics card, the dialogs' parts, the paper picture, the
// preferences (keys, the pen tablet, the screen, the pointer, the keys held, a tool's own modifier keys, the key held
// for a moment), the pen's side button, the number fields and the wheel, and the backups.

#include <QtTest>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFrame>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QTimer>
#include <QMenuBar>
#include <QOpenGLWidget>
#include <QPalette>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QSlider>
#include <QSpinBox>
#include <QStyleHints>
#include <QTabBar>
#include <QTabWidget>
#include <QToolBar>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QWheelEvent>

#include <memory>

#include "app/backup.hpp"
#include "app/brush_panel.hpp"
#include "app/canvas.hpp"
#include "app/comfort.hpp"
#include "app/config.hpp"
#include "app/dialogs.hpp"
#include "app/export_dialog.hpp"
#include "app/glass.hpp"
#include "app/icons.hpp"
#include "app/inject.hpp"
#include "app/inputs.hpp"
#include "app/layer_panel.hpp"
#include "app/look.hpp"
#include "app/main_window.hpp"
#include "app/preferences.hpp"
#include "app/story_panel.hpp"
#include "app/theme.hpp"
#include "app/tool_settings.hpp"
#include "app/workspace.hpp"
#include "gui_support.hpp"
#include "render/zip.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;

namespace {

// A book of two pages in a window, its questions answered by the test.
struct Book {
    QTemporaryDir tmp;
    std::filesystem::path path;
    std::unique_ptr<app::MainWindow> window;
    gui_test::Answers answers;
    std::function<int(QDialog*)> on_dialog;

    Book() {
        (void)gui_test::config_folder();
        path = gui_test::path_of(tmp.path() + QStringLiteral("/b.genko"));
        gui_test::write_book(path, gui_test::new_doc(2));
        window = std::make_unique<app::MainWindow>(app::Session::open(path, gui_test::quick(gui_test::path_of(tmp.path() + QStringLiteral("/recovery")))));
        window->resize(1280, 800);
        window->show();
        for (int i = 0; i < 5; ++i) QCoreApplication::processEvents();
        answers.responder->exec = [this](QDialog* dialog) { return on_dialog ? on_dialog(dialog) : 0; };
    }
    QAction* act(const char* name) const { return window->action(QString::fromLatin1(name)); }
    app::PageCanvas* canvas() const { return window->canvas(); }
    QDockWidget* dock(const QString& title) const {
        for (QDockWidget* d : window->findChildren<QDockWidget*>()) {
            if (d->windowTitle() == title) return d;
        }
        return nullptr;
    }
    std::vector<QDockWidget*> shown_docks() const {
        std::vector<QDockWidget*> out;
        for (QDockWidget* d : window->findChildren<QDockWidget*>()) {
            if (d->isVisible()) out.push_back(d);
        }
        return out;
    }
    QSet<QString> visible_titles() const {
        QSet<QString> out;
        for (QDockWidget* d : window->findChildren<QDockWidget*>()) {
            if (d->isVisible()) out.insert(d->windowTitle());
        }
        return out;
    }
};

std::unique_ptr<QSettings> store() { return app::settings(); }

bool any_ink(const QImage& image) {
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (image.pixelColor(x, y).alpha() != 0) return true;
        }
    }
    return false;
}

}  // namespace

class TestGuiLook : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { (void)gui_test::config_folder(); }

    // --- the look ------------------------------------------------------------------------------------------------

    void lightAndDarkAndTheBrightnessStep() {
        const app::theme::Tokens light = app::theme::light();
        const app::theme::Tokens dark = app::theme::dark();
        QVERIFY(!light.dark && dark.dark);
        QVERIFY(QColor(light.window).lightness() > 180 && QColor(dark.window).lightness() < 60);
        store()->setValue(QStringLiteral("ui/theme"), QStringLiteral("dark"));
        store()->setValue(QStringLiteral("ui/brightness"), 2);
        const app::theme::Tokens brighter = app::theme::tokens();
        QVERIFY(QColor(brighter.panel).lightness() > QColor(dark.panel).lightness());
        QCOMPARE(brighter.accent, dark.accent);  // (only the greys move)
        store()->setValue(QStringLiteral("ui/brightness"), 0);
        app::theme::apply(qApp);
        QVERIFY(QApplication::palette().color(QPalette::Window).lightness() < 80);
        QVERIFY(qApp->styleSheet().contains(QStringLiteral("QDockWidget::title")));
        store()->setValue(QStringLiteral("ui/theme"), QStringLiteral("light"));  // (Python's workspace.apply_theme("light"))
        app::theme::apply(qApp);
        QVERIFY(QApplication::palette().color(QPalette::Window).lightness() > 180);
        store()->setValue(QStringLiteral("ui/theme"), QStringLiteral("system"));
        app::theme::apply(qApp);
    }

    void theCanvasSurroundIsANeutralGrey() {
        const QColor colour = app::theme::surround();
        QVERIFY(std::abs(colour.red() - colour.green()) <= 6 && std::abs(colour.green() - colour.blue()) <= 6);
        store()->setValue(QStringLiteral("ui/surround"), QStringLiteral("128"));
        QCOMPARE(app::theme::surround().red(), 128);
        store()->setValue(QStringLiteral("ui/surround"), QStringLiteral("auto"));
    }

    void hintsCanGoIntoTooltips() {
        QWidget panel;
        auto* note = new QLabel(QStringLiteral("この道具の説明"), &panel);
        (new QVBoxLayout(&panel))->addWidget(note);
        app::theme::hint(note);
        app::theme::set_hints(false);
        QVERIFY(note->isHidden());
        QCOMPARE(panel.toolTip(), QStringLiteral("この道具の説明"));
        app::theme::set_hints(true);
        QVERIFY(!note->isHidden());
        app::theme::set_hints(false);
    }

    void iconsFollowTheLookAndCanBeGrey() {
        QVERIFY(!app::icons::icon("approve").isNull() && !app::icons::icon("duplicate").isNull());
        // (the tool in hand: its picture in the accent, or grey)
        store()->setValue(QStringLiteral("ui/mono_icons"), QStringLiteral("true"));
        const QImage grey = app::icons::icon("pen").pixmap(QSize(32, 32), QIcon::Normal, QIcon::On).toImage();
        store()->setValue(QStringLiteral("ui/mono_icons"), QStringLiteral("false"));
        const QImage colour = app::icons::icon("pen").pixmap(QSize(32, 32), QIcon::Normal, QIcon::On).toImage();
        QVERIFY(app::icons::icon("pen").pixmap(32, 32).toImage() != colour);  // (not in hand: the plain ink)
        QVERIFY(grey != colour);
        QVERIFY(!app::theme::mono_icons());
    }

    void theStartScreenShowsCovers() {
        QTemporaryDir tmp;
        const auto project = gui_test::path_of(tmp.path() + QStringLiteral("/c.genko"));
        gui_test::write_book(project, core::new_episode("表紙の本", core::Num(1), 1, core::PageSpec::b5_doujin()));
        app::remember_project(project);
        app::StartDialog dialog;
        bool found = false;
        for (int i = 0; i < dialog.recent()->count(); ++i) {
            if (dialog.recent()->item(i)->text().startsWith(QStringLiteral("表紙の本"))) {
                found = true;
                QVERIFY(!dialog.recent()->item(i)->icon().isNull());  // (its cover)
            }
        }
        QVERIFY(found);
    }

    // --- comfort -----------------------------------------------------------------------------------------------------

    void tabLeavesThePageAloneAndBringsThePanelsBack() {
        Book book;
        const std::vector<QDockWidget*> shown = book.shown_docks();
        QVERIFY(!shown.empty());
        // (Tab on the page only: elsewhere Tab moves between fields)
        QAction* only = book.act("act_canvas_only");
        QVERIFY(only->shortcut() == QKeySequence(Qt::Key_Tab) && only->shortcutContext() == Qt::WidgetWithChildrenShortcut);
        QVERIFY(book.canvas()->actions().contains(only) && !book.window->actions().contains(only));
        only->trigger();
        QCoreApplication::processEvents();
        QVERIFY(book.window->canvas_only()->on());
        for (QDockWidget* d : shown) QVERIFY(!d->isVisible());
        book.window->canvas_only()->watch_at(QPoint(book.window->width() - 2, 200));  // (the cursor at the right edge)
        QCOMPARE(book.window->canvas_only()->peek(), QStringLiteral("right"));
        bool right = false;
        for (QDockWidget* d : shown) right = right || (book.window->dockWidgetArea(d) == Qt::RightDockWidgetArea && d->isVisible());
        QVERIFY(right);
        QCoreApplication::processEvents();  // (the panels laid out where they show, before the cursor leaves: CanvasOnly looks every 120 ms)
        book.window->canvas_only()->watch_at(QPoint(book.window->width() / 2, 200));  // (away again)
        QVERIFY(book.window->canvas_only()->peek().isEmpty());
        book.window->canvas_only()->toggle();
        QCoreApplication::processEvents();
        for (QDockWidget* d : shown) QVERIFY(d->isVisible());
        QVERIFY(!book.window->canvas_only()->on());
        // (at the top of the window menu)
        for (QAction* top : book.window->menuBar()->actions()) {
            if (top->text() == QStringLiteral("ウィンドウ")) QCOMPARE(top->menu()->actions().front(), only);
        }
    }

    void eachStageHasItsLayout() {
        Book book;
        book.window->stage_actions().at(QStringLiteral("letter"))->trigger();
        QCoreApplication::processEvents();
        QSet<QString> visible = book.visible_titles();
        QVERIFY(visible.contains(QStringLiteral("台詞")) && !visible.contains(QStringLiteral("カラー")) && !visible.contains(QStringLiteral("素材")));
        book.window->stage_actions().at(QStringLiteral("ink"))->trigger();
        QCoreApplication::processEvents();
        visible = book.visible_titles();
        QVERIFY(visible.contains(QStringLiteral("レイヤー")) && visible.contains(QStringLiteral("カラー")));
    }

    void theRoundMenuAtThePen() {
        Book book;
        book.act("act_pen")->trigger();
        emit book.canvas()->contextMenuAt(QString(), book.canvas()->mapToGlobal(QPoint(400, 300)));
        app::RadialMenu* menu = book.window->radial();
        QVERIFY(menu != nullptr);
        QVERIFY(menu->commands().size() >= 6);
        int target = -1;
        for (std::size_t i = 0; i < menu->commands().size(); ++i) {
            if (menu->commands()[i] == book.act("act_eraser")) target = static_cast<int>(i);
        }
        QVERIFY(target >= 0);
        menu->choose(target);
        QCOMPARE(book.canvas()->tool(), QStringLiteral("eraser"));
        QCoreApplication::processEvents();
        QVERIFY(book.window->radial() == nullptr);  // (gone once chosen)
        // a tool that does not draw: the usual menu (no round one)
        book.act("act_select")->trigger();
        bool usual = false;
        QTimer::singleShot(0, [&usual] {
            if (auto* popup = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
                usual = !popup->actions().isEmpty();
                popup->close();
            }
        });
        emit book.canvas()->contextMenuAt(QString(), book.canvas()->mapToGlobal(QPoint(400, 300)));
        QVERIFY(usual);
        QVERIFY(book.window->radial() == nullptr);
        store()->setValue(QStringLiteral("ui/radial"), QStringLiteral("false"));
        QVERIFY(!app::radial_on());
        store()->remove(QStringLiteral("ui/radial"));
        QVERIFY(app::radial_on() && app::radial_tool(QStringLiteral("fill")) && !app::radial_tool(QStringLiteral("select")));
    }

    void theEyesAreRemindedOnlyWhenAsked() {
        Book book;
        app::RestReminder* rest = book.window->rest();
        rest->worked = 10000;
        rest->count();
        QCOMPARE(rest->worked, 10000.0);  // (off by default: nothing counted, nothing shown)
        QVERIFY(!app::input_watched());
        store()->setValue(QStringLiteral("ui/rest_minutes"), 30);
        app::touch_input();  // (the person is at work)
        int shown = 0;
        rest->on_remind = [&shown] { ++shown; };
        rest->count();
        QVERIFY(shown == 1 && rest->worked == 0.0);
        store()->setValue(QStringLiteral("ui/rest_minutes"), 0);
        rest->count();
        QVERIFY(!app::input_watched());  // (off again: nothing watches the input)
        // the reminder's own words
        rest->on_remind = nullptr;
        store()->setValue(QStringLiteral("ui/rest_minutes"), 45);
        rest->remind();
        QLabel* note = book.canvas()->findChild<QLabel*>(QStringLiteral("toast"));
        QVERIFY(note != nullptr);
        QCOMPARE(note->text(), QStringLiteral("45 分続けて作業しました。20 秒ほど遠くを見て、目を休めましょう"));
        store()->setValue(QStringLiteral("ui/rest_minutes"), 0);
        app::stop_input();
    }

    void fewerMovingThingsAndLettersAtOnce() {
        store()->setValue(QStringLiteral("ui/reduce_motion"), QStringLiteral("true"));
        app::apply_motion(qApp);
        QVERIFY(!QApplication::isEffectEnabled(Qt::UI_AnimateMenu));
        QVERIFY(app::reduce_motion());
        store()->setValue(QStringLiteral("ui/reduce_motion"), QStringLiteral("false"));
        app::apply_motion(qApp);
        QVERIFY(!app::reduce_motion());
        store()->remove(QStringLiteral("ui/reduce_motion"));
        QCOMPARE(app::reduce_motion(), app::system_reduces_motion());  // (no choice: as the computer is set)
        const int before = QApplication::font().pointSize();
        store()->setValue(QStringLiteral("ui/font_pt"), before + 2);
        app::apply_font(qApp);
        QCOMPARE(QApplication::font().pointSize(), before + 2);
        store()->setValue(QStringLiteral("ui/font_pt"), before);
        app::apply_font(qApp);
        QCOMPARE(QApplication::font().pointSize(), before);
        store()->remove(QStringLiteral("ui/font_pt"));
    }

    // --- the finish (D1–D4) ------------------------------------------------------------------------------------------

    void theBundledLettersAndTheirThreeSteps() {
        app::theme::apply(qApp);
        QVERIFY(app::theme::ui_family().contains(QStringLiteral("Plex")));
        QCOMPARE(QApplication::font().family(), app::theme::ui_family());
        const QString sheet = qApp->styleSheet();
        QVERIFY(sheet.contains(QStringLiteral("QLabel[role=\"section\"]")));
        QVERIFY(sheet.contains(QStringLiteral("QLabel[role=\"title\"] { font-weight: 700")));
        store()->setValue(QStringLiteral("ui/ui_font"), QStringLiteral("system"));
        app::theme::apply(qApp);
        QVERIFY(QApplication::font().family() != app::theme::ui_family());
        store()->setValue(QStringLiteral("ui/ui_font"), QStringLiteral("genko"));
        app::theme::apply(qApp);
        QCOMPARE(QApplication::font().family(), app::theme::ui_family());
    }

    void theChosenRowIsACleanGrey() {
        for (const app::theme::Tokens& t : {app::theme::light(), app::theme::dark()}) {
            const QColor chosen(t.selected);
            QVERIFY(chosen.hslSaturation() < 60);
            QVERIFY(!(0 <= chosen.hslHue() && chosen.hslHue() <= 60));  // (a cool grey, not a muddy orange)
        }
    }

    void theTopBarIsPicturesAndThePanelIsRows() {
        Book book;
        QToolBar* bar = book.window->command_bar();
        QCOMPARE(bar->toolButtonStyle(), Qt::ToolButtonIconOnly);
        for (QAction* a : bar->actions()) {
            if (!a->isSeparator() && !a->property("keep").toBool()) QVERIFY2(!a->icon().isNull(), qPrintable(a->text()));
        }
        book.act("act_frame")->trigger();
        QWidget* page = book.window->tool_settings()->current_page();
        QVERIFY(page != nullptr);
        QSet<QString> sections;
        for (QLabel* label : page->findChildren<QLabel*>()) {
            if (label->property("role").toString() == QLatin1String("section")) sections.insert(label->text());
        }
        QVERIFY(sections.contains(QStringLiteral("割る")) && sections.contains(QStringLiteral("枠線")) && sections.contains(QStringLiteral("形")));
        QList<QPushButton*> rows;
        QSet<int> widths;
        for (QPushButton* b : page->findChildren<QPushButton*>()) {
            if (b->property("row").toBool()) {
                rows << b;
                widths.insert(b->iconSize().width());
            }
        }
        QVERIFY(!rows.isEmpty() && widths.size() == 1);
        book.act("act_split_h")->setEnabled(false);
        QPushButton* split = nullptr;
        for (QPushButton* b : rows) {
            if (b->text().startsWith(QStringLiteral("横に割る"))) split = b;
        }
        QVERIFY(split != nullptr && !split->isEnabled());
        book.act("act_split_h")->setEnabled(true);
    }

    void layersHaveEyesAndEvenRows() {
        Book book;
        app::LayerPanel* panel = book.window->layer_panel();
        panel->refresh();
        QCOMPARE(panel->list()->objectName(), QStringLiteral("layerList"));
        QVERIFY((book.window->styleSheet() + qApp->styleSheet()).contains(QStringLiteral("layerList::indicator:checked")));
        QSet<int> heights;
        for (int i = 0; i < panel->list()->count(); ++i) heights.insert(panel->list()->visualItemRect(panel->list()->item(i)).height());
        QCOMPARE(heights.size(), 1);
    }

    void theRarelyUsedPanelsWaitInTheWindowMenu() {
        Book book;
        QVERIFY(!book.dock(QStringLiteral("サブビュー"))->isVisible() && !book.dock(QStringLiteral("タイムライン"))->isVisible());
        book.window->show_dock(QStringLiteral("タイムライン"));
        QVERIFY(book.dock(QStringLiteral("タイムライン"))->isVisible());
    }

    void theIconsAreLucideWithAFewOfGenkosOwn() {
        QFile licence(QStringLiteral(":/genko/lucide/LICENSE"));
        QVERIFY(licence.open(QIODevice::ReadOnly));
        QVERIFY(licence.readAll().startsWith("ISC License"));
        for (const char* name : {"pen", "text", "effect", "gradient", "kind_tone"}) {
            QVERIFY2(any_ink(app::icons::icon(name).pixmap(32, 32).toImage()), name);
        }
    }

    void theLayerSettingsFoldAway() {
        Book book;
        app::LayerPanel* panel = book.window->layer_panel();
        QVERIFY(!panel->details()->isVisible() && panel->details_toggle()->text().startsWith(QStringLiteral("▸")));
        panel->details_toggle()->click();
        QVERIFY(panel->details()->isVisible() && panel->details_toggle()->text().startsWith(QStringLiteral("▾")));
        panel->details_toggle()->click();
    }

    // --- UI-F ------------------------------------------------------------------------------------------------------

    void theBrushListShowsEachBrushsLine() {
        Book book;
        app::BrushPanel* brush = book.window->brush_panel();
        QVERIFY(brush->kinds->count() > 0);
        for (int i = 0; i < brush->kinds->count(); ++i) QVERIFY(!brush->kinds->item(i)->icon().isNull());
        const QImage image = brush->kinds->item(0)->icon().pixmap(72, 20).toImage();
        bool across = false;
        for (int x = 0; x < image.width(); ++x) across = across || image.pixelColor(x, image.height() / 2).alpha() != 0;
        QVERIFY(across);  // (a line runs across)
    }

    void theToolSettingsFoldToTheToolsName() {
        Book book;
        app::ToolSettings* ts = book.window->tool_settings();
        book.act("act_frame")->trigger();
        ts->fold_button()->click();
        QCoreApplication::processEvents();
        QVERIFY(ts->folded() && !ts->stack()->isVisible() && ts->title()->text().startsWith(QStringLiteral("コマ割り")));
        QDockWidget* dock = book.dock(QStringLiteral("オプション"));  // (PC5 H-0: Photoshop's name)
        QVERIFY(dock != nullptr && dock->maximumHeight() < 100);
        ts->fold_button()->click();
        QCoreApplication::processEvents();
        QVERIFY(ts->stack()->isVisible() && dock->maximumHeight() > 10000);
    }

    void anEmptyLinesListSaysWhatGoesThere() {
        Book book;
        QListWidget* list = book.window->story()->list;
        bool said = false;
        for (QLabel* label : list->viewport()->findChildren<QLabel*>()) {
            if (label->text().contains(QStringLiteral("台詞がありません")) && label->isVisibleTo(list)) said = true;
        }
        QVERIFY(said);
    }

    void theLayersTopRowsOnlyWhenNeeded() {
        Book book;
        app::LayerPanel* panel = book.window->layer_panel();
        panel->refresh();
        QVERIFY(!panel->search_box()->isVisibleTo(panel) && !panel->target_row()->isVisibleTo(panel));
    }

    // --- stage 1: motion and the system's glass ------------------------------------------------------------------------

    void zoomGlidesButTheViewIsAlreadyThere() {
        Book book;
        app::PageCanvas* canvas = book.canvas();
        const double before = canvas->scale();
        book.act("act_zoom_in")->trigger();
        QVERIFY(std::abs(canvas->scale() - before * 1.25) < 1e-6);  // (what the pen touches is the new view at once)
        QVERIFY(canvas->gliding());
        auto* motion = canvas->findChild<QVariantAnimation*>();
        QVERIFY(motion != nullptr && motion->state() == QAbstractAnimation::Running);
        (void)canvas->grab();  // (drawn part way without trouble)
        motion->setCurrentTime(motion->duration());
        QVERIFY(!canvas->gliding());
        store()->setValue(QStringLiteral("ui/reduce_motion"), QStringLiteral("true"));
        book.act("act_fit")->trigger();
        QVERIFY(!canvas->gliding());  // (fewer moving things: straight there)
        store()->setValue(QStringLiteral("ui/reduce_motion"), QStringLiteral("false"));
    }

    void theRoundMenuOpensOutward() {
        Book book;
        book.act("act_pen")->trigger();
        emit book.canvas()->contextMenuAt(QString(), book.canvas()->mapToGlobal(QPoint(400, 300)));
        app::RadialMenu* menu = book.window->radial();
        QVERIFY(menu != nullptr);
        QVERIFY(menu->grow() < 1.0 && menu->property("glass_wanted").toBool() && !menu->mask().isEmpty());
        menu->opening()->setCurrentTime(140);
        QCOMPARE(menu->grow(), 1.0);
        (void)menu->grab();
        menu->close();
    }

    void theSystemGlassOnlyWhereTheSystemHasIt() {
#ifndef _WIN32
        QVERIFY(app::glass::available().isEmpty() && !app::glass::install(qApp));
#endif
        QMenu menu;
        QVERIFY(app::glass::wants(&menu));
        QVERIFY(!app::glass::apply(&menu) || !app::glass::available().isEmpty());
    }

    // --- stage 2: the page on the graphics card -------------------------------------------------------------------------

    void theGpuCanvasIsTheSameCanvasOnOpenGL() {
        app::PageCanvas canvas;
        // (shown, as the page is: Qt keeps a hidden widget's resize until it is shown — not drawn on the offscreen screen,
        // which has no card)
        canvas.setAttribute(Qt::WA_DontShowOnScreen);
        canvas.resize(400, 300);
        canvas.show();
        QVERIFY(!canvas.gpu());
        canvas.use_gpu(true);
        auto* surface = qobject_cast<QOpenGLWidget*>(canvas.gpu_surface());
        QVERIFY(surface != nullptr && canvas.gpu());
        QVERIFY(surface->testAttribute(Qt::WA_TransparentForMouseEvents) && surface->parentWidget() == &canvas);
        canvas.resize(500, 320);
        QCOMPARE(surface->size(), QSize(500, 320));  // (it covers the canvas)
        QCOMPARE(canvas.children().indexOf(surface) >= 0, true);
        canvas.use_gpu(false);
        QVERIFY(!canvas.gpu() && canvas.findChild<QOpenGLWidget*>() == nullptr);
    }

    void thePageIsDrawnByTheProcessorWhereThereIsNoRealCard() {
        QVERIFY(!app::gpu_available());  // (the offscreen test screen)
        store()->setValue(QStringLiteral("ui/gpu_canvas"), QStringLiteral("true"));
        QVERIFY(!app::gpu_available());  // (chosen, but no real screen)
        store()->remove(QStringLiteral("ui/gpu_canvas"));
        for (const char* name : {"llvmpipe (LLVM 17.0.6, 256 bits)", "Microsoft Basic Render Driver", "Software Rasterizer"}) {
            QVERIFY(app::software_renderer(QString::fromLatin1(name)));
        }
        for (const char* name : {"NVIDIA GeForce RTX 4070/PCIe/SSE2", "AMD Radeon Pro 5500M OpenGL Engine", "Intel(R) Iris(R) Xe Graphics", "Apple M2"}) {
            QVERIFY(!app::software_renderer(QString::fromLatin1(name)));
        }
        Book book;
        QVERIFY(!book.canvas()->gpu());
    }

    // --- UI-G: the dialogs made of the same parts ---------------------------------------------------------------------

    void theDialogsHaveAHeadingACardAndOneMainButton() {
        Book book;
        std::vector<std::unique_ptr<QDialog>> made;
        made.push_back(std::make_unique<app::ExportDialog>(book.window.get(), book.window->session().snapshot(), book.path, book.window->session().actor(), false, 1));
        made.push_back(std::make_unique<app::NewProjectDialog>(book.window.get()));
        made.push_back(std::make_unique<app::PaperDialog>(book.window.get(), book.window->book().pages[0]->spec, true));
        made.push_back(std::make_unique<app::preferences::PreferencesDialog>(book.window.get()));
        made.push_back(std::make_unique<app::BrushDialog>(book.window.get(), "gpen"));
        for (const auto& dialog : made) {
            const char* name = dialog->metaObject()->className();
            bool title = false;
            for (QLabel* label : dialog->findChildren<QLabel*>()) title = title || label->property("role").toString() == QLatin1String("title");
            QVERIFY2(title, name);
            QPushButton* ok = dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok);
            QVERIFY2(ok != nullptr && ok->property("primary").toBool(), name);
            bool card = false;
            for (QFrame* f : dialog->findChildren<QFrame*>()) card = card || f->objectName() == QLatin1String("dialogCard");
            QVERIFY2(card || qobject_cast<app::preferences::PreferencesDialog*>(dialog.get()) != nullptr, name);
        }
    }

    void thePaperPictureFollowsTheNumbers() {
        Book book;
        app::PaperDialog dialog(book.window.get(), book.window->book().pages[0]->spec, true);
        const auto before = dialog.diagram()->spec();
        dialog.trim_w->setValue(dialog.trim_w->value() - 20);
        QVERIFY(dialog.diagram()->spec().has_value() && !(dialog.diagram()->spec()->trim_w_mm == before->trim_w_mm));
        QVERIFY(!dialog.diagram()->error());
        dialog.paper_w->setValue(50);  // (smaller than the finished size: the picture says so)
        QVERIFY(dialog.diagram()->error() && !dialog.ok_button()->isEnabled());
    }

    void thePreferencesPagesAreChosenAtTheSide() {
        Book book;
        app::preferences::PreferencesDialog dialog(book.window.get());
        QVERIFY(dialog.tabs->tabBar()->isHidden() && dialog.pages_list->count() == dialog.tabs->count());
        dialog.pages_list->setCurrentRow(2);
        QCOMPARE(dialog.tabs->currentIndex(), 2);
        dialog.tabs->setCurrentIndex(0);
        QCOMPARE(dialog.pages_list->currentRow(), 0);
    }

    // --- 環境設定 (test_j1, test_w14_speed, test_f6) ---------------------------------------------------------------------

    void heldAltIsTheEyedropperForAMoment() {
        Book book;
        app::PageCanvas* canvas = book.canvas();
        book.window->choose_tool(QStringLiteral("pen"));
        canvas->hold_modifier(QStringLiteral("alt"), true);
        QCOMPARE(canvas->tool(), QStringLiteral("picker"));
        canvas->hold_modifier(QStringLiteral("alt"), false);
        QCOMPARE(canvas->tool(), QStringLiteral("pen"));
        canvas->modifier_tools = {{QStringLiteral("alt"), QString()}, {QStringLiteral("ctrl"), QStringLiteral("move")}};
        canvas->hold_modifier(QStringLiteral("alt"), true);
        QCOMPARE(canvas->tool(), QStringLiteral("pen"));
        canvas->hold_modifier(QStringLiteral("ctrl"), true);
        QCOMPARE(canvas->tool(), QStringLiteral("move"));
        canvas->hold_modifier(QStringLiteral("ctrl"), false);
        QCOMPARE(canvas->tool(), QStringLiteral("pen"));
    }

    void theCursorKind() {
        Book book;
        app::PageCanvas* canvas = book.canvas();
        QCOMPARE(canvas->cursor_kind, QStringLiteral("circle"));  // (環境設定's default)
        book.window->choose_tool(QStringLiteral("pen"));
        canvas->cursor_kind = QStringLiteral("circle");
        canvas->refresh_cursor();
        QCOMPARE(canvas->cursor().shape(), Qt::BlankCursor);
        canvas->cursor_kind = QStringLiteral("cross");
        canvas->refresh_cursor();
        QCOMPARE(canvas->cursor().shape(), Qt::CrossCursor);
        canvas->cursor_kind = QStringLiteral("dot");
        canvas->refresh_cursor();
        QCOMPARE(canvas->cursor().shape(), Qt::BlankCursor);
        (void)canvas->grab();  // (each pointer drawn without trouble)
    }

    void thePreferencesHoldTheScreenSettings() {
        Book book;
        app::preferences::PreferencesDialog dialog(book.window.get());
        dialog.theme->setCurrentIndex(dialog.theme->findData(QStringLiteral("dark")));
        dialog.cursor->setCurrentIndex(dialog.cursor->findData(QStringLiteral("dot")));
        dialog.alt_tool->setCurrentIndex(dialog.alt_tool->findData(QStringLiteral("eraser")));
        dialog.save();
        QCOMPARE(store()->value(QStringLiteral("ui/theme")).toString(), QStringLiteral("dark"));
        QCOMPARE(book.canvas()->cursor_kind, QStringLiteral("dot"));
        QCOMPARE(book.canvas()->modifier_tools.at(QStringLiteral("alt")), QStringLiteral("eraser"));
        QVERIFY(QApplication::palette().color(QPalette::Window).lightness() < 80);
        for (const char* key : {"ui/theme", "ui/cursor", "keys/alt_tool"}) store()->remove(QString::fromLatin1(key));
        app::theme::apply(qApp);
    }

    void aToolsKeyHeldIsThatToolOnlyWhileHeld() {
        Book book;
        app::MainWindow* w = book.window.get();
        w->choose_tool(QStringLiteral("pen"));
        w->choose_tool(QStringLiteral("eraser"));  // (as E pressed)
        double start = w->tool_switch()->since;
        QVERIFY(!w->hold_key_released(Qt::Key_E, start + 0.1));  // (a tap: the eraser stays)
        QCOMPARE(w->canvas()->tool(), QStringLiteral("eraser"));
        w->choose_tool(QStringLiteral("pen"));
        w->choose_tool(QStringLiteral("eraser"));
        QVERIFY(w->hold_key_released(Qt::Key_E, app::monotonic_seconds() + 1.0));  // (held: back to the pen)
        QCOMPARE(w->canvas()->tool(), QStringLiteral("pen"));
        w->choose_tool(QStringLiteral("eraser"));
        QVERIFY(!w->hold_key_released(Qt::Key_B, app::monotonic_seconds() + 1.0));  // (another key: nothing)
        store()->setValue(QStringLiteral("keys/hold_swap"), false);
        w->choose_tool(QStringLiteral("pen"));
        w->choose_tool(QStringLiteral("eraser"));
        QVERIFY(!w->hold_key_released(Qt::Key_E, app::monotonic_seconds() + 1.0));
        store()->remove(QStringLiteral("keys/hold_swap"));
    }

    void eachToolHasItsOwnModifierKeys() {
        Book book;
        app::set_tool_modifier(QStringLiteral("pen"), QStringLiteral("shift"), QStringLiteral("eraser"));
        app::set_tool_modifier(QStringLiteral("pen"), QStringLiteral("alt"), QStringLiteral("none"));
        app::PageCanvas* canvas = book.canvas();
        canvas->tool_modifiers = app::tool_modifiers();
        canvas->set_tool(QStringLiteral("pen"));
        canvas->hold_modifier(QStringLiteral("shift"), true);
        QCOMPARE(canvas->tool(), QStringLiteral("eraser"));
        canvas->hold_modifier(QStringLiteral("alt"), false);  // (another key let go: the eraser stays)
        QCOMPARE(canvas->tool(), QStringLiteral("eraser"));
        canvas->hold_modifier(QStringLiteral("shift"), false);
        QCOMPARE(canvas->tool(), QStringLiteral("pen"));
        canvas->hold_modifier(QStringLiteral("alt"), true);  // (the pen says Alt does nothing)
        QCOMPARE(canvas->tool(), QStringLiteral("pen"));
        canvas->hold_modifier(QStringLiteral("alt"), false);
        canvas->set_tool(QStringLiteral("fill"));
        canvas->hold_modifier(QStringLiteral("alt"), true);  // (the fill has none of its own: the common スポイト)
        QCOMPARE(canvas->tool(), QStringLiteral("picker"));
        canvas->hold_modifier(QStringLiteral("alt"), false);
        canvas->hold_modifier(QStringLiteral("shift"), true);  // (no Shift of its own: no switch)
        QCOMPARE(canvas->tool(), QStringLiteral("fill"));
        canvas->hold_modifier(QStringLiteral("shift"), false);
        // the dialog keeps a tool's own keys
        book.on_dialog = [](QDialog* dialog) {
            auto* box = dialog->findChild<QComboBox*>(QStringLiteral("fill/ctrl"));
            box->setCurrentIndex(box->findData(QStringLiteral("move")));
            return int(QDialog::Accepted);
        };
        QVERIFY(app::preferences::tool_keys_dialog(book.window.get(), book.window.get()));
        QCOMPARE(app::tool_modifiers()[QStringLiteral("fill")][QStringLiteral("ctrl")], QStringLiteral("move"));
        QCOMPARE(canvas->tool_modifiers[QStringLiteral("fill")][QStringLiteral("ctrl")], QStringLiteral("move"));
        for (const char* tool : {"pen", "fill"}) {
            for (const char* key : {"shift", "alt", "ctrl"}) app::set_tool_modifier(QString::fromLatin1(tool), QString::fromLatin1(key), QString());
        }
        QVERIFY(app::tool_modifiers().empty());
    }

    void nothingWatchesEveryEventOfTheAppUnlessNeeded() {
        Book book;
        app::stop_input();
        book.window->rest()->count();
        QVERIFY(!app::input_watched());  // (the rest reminder is off: nothing counts the input)
        app::stop_key_releases();
        book.act("act_pen")->trigger();
        book.act("act_eraser")->trigger();  // (its button clicked: no key to watch)
        QVERIFY(!app::key_releases_watched());
        book.act("act_pen")->trigger();
        QShortcutEvent pressed(QKeySequence(QStringLiteral("E")), 0);
        QApplication::sendEvent(book.act("act_eraser"), &pressed);  // (E pressed: its release is watched)
        QVERIFY(book.canvas()->tool() == QStringLiteral("eraser") && app::key_releases_watched());
        QKeyEvent released(QEvent::KeyRelease, Qt::Key_E, Qt::NoModifier);
        QApplication::sendEvent(book.window.get(), &released);
        QVERIFY(!app::key_releases_watched());  // (let go: no longer watched)
        // the system's glass readied by the style (as on a Windows that has it), once
        app::glass::set_available_for_tests(QStringLiteral("windows"));
        QList<QWidget*> shown;
        app::glass::set_apply_for_tests([&shown](QWidget* w) {
            shown << w;
            return true;
        });
        {
            app::glass::GlassStyle style;
            QMenu menu;
            style.polish(&menu);  // (the style readies the floating parts: no watcher on the app)
            style.polish(&menu);  // (readied again, as when the style is set again: still once)
            QVERIFY(menu.testAttribute(Qt::WA_TranslucentBackground));
            menu.show();
            QCOMPARE(shown, QList<QWidget*>{&menu});
            QVERIFY(menu.property("glass").toBool());
            menu.close();
        }
        app::glass::set_available_for_tests(std::nullopt);
        app::glass::set_apply_for_tests(nullptr);
    }

    void preferencesChangeKeysPenAndWork() {
        Book book;
        app::MainWindow* w = book.window.get();
        app::preferences::PreferencesDialog dialog(w);
        // two commands on one key are refused
        dialog.editor(QStringLiteral("ブラシツール"))->setKeySequence(QKeySequence(QStringLiteral("E")));
        dialog.save();
        QVERIFY(dialog.clash->text().contains(QStringLiteral("「E」")));
        QCOMPARE(book.act("act_pen")->shortcut().toString(), QStringLiteral("B"));
        QCOMPARE(dialog.result(), int(QDialog::Rejected));
        // (a free key: F11 — P is ペンツール's since PC3 C-a, F7 the レイヤー panel's since PC5 H-0: Photoshop's)
        dialog.editor(QStringLiteral("ブラシツール"))->setKeySequence(QKeySequence(QStringLiteral("F11")));
        // the pen tablet: a light hand gets a softer curve
        for (int i = 0; i < 60; ++i) dialog.pad->pressures.push_back(0.25 + 0.01 * (i % 10));
        dialog.measure();
        QVERIFY(dialog.gamma && *dialog.gamma < 1 && dialog.gamma_note->text().contains(QStringLiteral("やわらかめ")));
        dialog.button->setCurrentIndex(dialog.button->findData(QStringLiteral("picker")));
        const int size_before = QApplication::font().pointSize();
        dialog.font_pt->setValue(13);
        dialog.paper->setCurrentIndex(dialog.paper->findData(QStringLiteral("b5")));
        dialog.save_after->setValue(5);
        dialog.save();
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QCOMPARE(book.act("act_pen")->shortcut().toString(), QStringLiteral("F11"));
        QVERIFY(book.act("act_pen")->toolTip().startsWith(QStringLiteral("ブラシツール（F11）")));  // (the tooltip says the new key)
        QCOMPARE(w->save_after(), 5000);
        // (never past the longest wait: a test book saves within 0.4 s, a person's within 10 s)
        QCOMPARE(w->session().idle(), std::min(std::chrono::milliseconds(5000), w->session().longest()));
        QCOMPARE(book.canvas()->pen_button, QStringLiteral("picker"));
        QComboBox* pressure = w->brush_panel()->pressure;
        QVERIFY(pressure->itemText(pressure->count() - 1).startsWith(QStringLiteral("自分に合わせた")));
        QCOMPARE(app::preferences::ui_font_pt(), 13);
        QCOMPARE(QApplication::font().pointSize(), 13);
        QCOMPARE(app::preferences::default_paper(), QStringLiteral("b5"));
        // a new window keeps the key; the new-book dialog starts on B5
        {
            app::MainWindow other(app::Session::open(book.path, gui_test::quick(gui_test::path_of(book.tmp.path() + QStringLiteral("/recovery2")))));
            QCOMPARE(other.action(QStringLiteral("act_pen"))->shortcut().toString(), QStringLiteral("F11"));
            QCOMPARE(other.save_after(), 5000);
            other.close();
        }
        QCOMPARE(app::NewProjectDialog(w).paper->currentData().toString(), QStringLiteral("b5"));
        // back to the first keys
        app::preferences::PreferencesDialog again(w);
        again.reset_keys();
        again.save();
        QCOMPARE(book.act("act_pen")->shortcut().toString(), QStringLiteral("B"));
        QVERIFY(!store()->contains(QStringLiteral("shortcuts/ペン")));
        QCOMPARE(app::preferences::gamma_for(std::vector<double>(20, 0.5)), 1.0);
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, app::preferences::gamma_for(std::vector<double>(3, 0.5)));
        for (const char* key : {"ui/font_pt", "new/paper", "save/after_ms", "tablet/gamma", "tablet/button"}) store()->remove(QString::fromLatin1(key));
        QFont font = QApplication::font();
        font.setPointSize(size_before);
        QApplication::setFont(font);
        app::theme::apply(qApp);
        // a command renamed since keeps the keys chosen under its old name
        store()->setValue(QStringLiteral("shortcuts/レイヤーを削除"), QStringLiteral("Ctrl+Alt+Del"));
        bool renamed = false;
        for (QAction* a : app::preferences::commands(w)) {
            if (a->text() == QStringLiteral("レイヤーを消す")) renamed = true;
        }
        if (renamed) {
            app::preferences::apply_shortcuts(w);
            for (QAction* a : app::preferences::commands(w)) {
                if (a->text() == QStringLiteral("レイヤーを消す")) QCOMPARE(a->shortcut().toString(), QStringLiteral("Ctrl+Alt+Del"));
            }
        }
        store()->remove(QStringLiteral("shortcuts/レイヤーを削除"));
        // two commands on one key, found in order
        const auto clashes = app::preferences::conflicts({{QStringLiteral("a"), {QStringLiteral("X")}}, {QStringLiteral("b"), {QStringLiteral("Y")}},
                                                          {QStringLiteral("c"), {QStringLiteral("X")}}});
        QVERIFY(clashes.size() == 1 && clashes[0].keys == QStringLiteral("X") && clashes[0].first == QStringLiteral("a") &&
                clashes[0].second == QStringLiteral("c"));
    }

    void thePensSideButtonPicksAColour() {
        Book book;
        app::PageCanvas* canvas = book.canvas();
        book.act("act_pen")->trigger();
        canvas->pen_button = QStringLiteral("picker");
        QVERIFY(canvas->wait_rendered(30000));
        QSignalSpy picked(canvas, &app::PageCanvas::colourPicked);
        const std::size_t before = gui_test::ink_strokes(book.window->book());
        app::inject::tablet_side(canvas, app::inject::Phase::Press, QPointF(60, 200));
        app::inject::tablet_side(canvas, app::inject::Phase::Release, QPointF(60, 200));
        QCOMPARE(picked.count(), 1);
        QCOMPARE(gui_test::ink_strokes(book.window->book()), before);
        // the hand: the view moves with the side button
        canvas->pen_button = QStringLiteral("hand");
        const QPointF pan = canvas->pan();
        app::inject::tablet_side(canvas, app::inject::Phase::Press, QPointF(60, 200));
        app::inject::tablet_side(canvas, app::inject::Phase::Move, QPointF(80, 200));
        app::inject::tablet_side(canvas, app::inject::Phase::Release, QPointF(80, 200));
        QVERIFY(canvas->pan().x() > pan.x() + 10);
        QCOMPARE(gui_test::ink_strokes(book.window->book()), before);
        canvas->pen_button = QStringLiteral("menu");
    }

    // --- the number fields (test_hd_report) ------------------------------------------------------------------------

    void numberFieldsSelectTheirValueAndTheWheelLeavesUnusedFieldsAlone() {
        app::inputs::install();
        QComboBox box;
        app::inputs::watch(&box);
        box.addItems({QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")});
        box.setFocusPolicy(Qt::StrongFocus);
        QWheelEvent wheel(QPointF(5, 5), QPointF(5, 5), QPoint(0, 0), QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(&box, &wheel);
        QCOMPARE(box.currentIndex(), 0);
        // entered: its value all chosen, so typing replaces it
        QWidget holder;
        auto* spin = new QSpinBox(&holder);
        spin->setValue(42);
        app::inputs::watch(&holder);
        holder.show();
        QVERIFY(QTest::qWaitForWindowExposed(&holder));
        holder.activateWindow();
        spin->setFocus();
        QVERIFY(gui_test::wait_for([&] { return !spin->findChild<QLineEdit*>()->selectedText().isEmpty(); }, 2000));
        QCOMPARE(spin->findChild<QLineEdit*>()->selectedText(), QStringLiteral("42"));
    }

    // --- the backups (test_w15_color) -------------------------------------------------------------------------------

    // zip64 (a backup past 4 GiB or 65535 files, Python's zipfile writes it so): its end records written and read back
    // (Python's zipfile reading the same archives whole: test_contract_materials' bigZipsAreReadWholeByPython, a
    // contract test, as it needs the reference Python)
    void bigBackupsAreZip64() {
        QTemporaryDir tmp;
        const auto source = gui_test::path_of(tmp.path() + QStringLiteral("/s.txt"));
        genko::test::write_bytes(gui_test::qpath(source), "abc");
        const auto forced = gui_test::path_of(tmp.path() + QStringLiteral("/forced.zip"));
        render::zip::pack(forced, {{"a.genko/project.json", source}, {"x/y.txt", source}}, 1);
        std::vector<std::pair<std::string, std::filesystem::path>> many;
        for (int i = 0; i < 65537; ++i) many.emplace_back("n/" + std::to_string(i) + ".txt", source);
        const auto crowded = gui_test::path_of(tmp.path() + QStringLiteral("/crowded.zip"));
        render::zip::pack(crowded, many);
        for (const auto& [archive, count] : {std::pair{forced, std::size_t{2}}, std::pair{crowded, std::size_t{65537}}}) {
            // (the end records: zip64's with the true count, the old one with its 16-bit count or 0xFFFF when it does not fit)
            const std::string bytes = genko::test::read_bytes(gui_test::qpath(archive));
            const auto number = [&bytes](std::size_t at, int size) {
                std::uint64_t value = 0;
                for (int i = size - 1; i >= 0; --i) value = (value << 8) | static_cast<unsigned char>(bytes[at + static_cast<std::size_t>(i)]);
                return value;
            };
            const std::size_t end64 = bytes.rfind(std::string("PK\x06\x06", 4));
            const std::size_t locator = bytes.rfind(std::string("PK\x06\x07", 4));
            const std::size_t end = bytes.rfind(std::string("PK\x05\x06", 4));
            QVERIFY(end64 != std::string::npos && locator != std::string::npos && end != std::string::npos && end64 < locator && locator < end);
            QCOMPARE(number(end64 + 32, 8), static_cast<std::uint64_t>(count));
            QCOMPARE(number(locator + 8, 8), static_cast<std::uint64_t>(end64));  // (the locator points at the zip64 end record)
            const std::uint64_t old_count = number(end + 10, 2);
            QVERIFY(old_count == 0xffff || (count < 0xffff && old_count == count));
            if (count > 0xffff) QCOMPARE(old_count, std::uint64_t{0xffff});
        }
    }

    void backupsAreZippedElsewhereAndTheOldestDropped() {
        QTemporaryDir tmp;
        const auto book = gui_test::path_of(tmp.path() + QStringLiteral("/a.genko"));
        gui_test::write_book(book, gui_test::new_doc(1));
        const auto folder = gui_test::path_of(tmp.path() + QStringLiteral("/backups"));
        QVERIFY(app::backup::due(folder, QStringLiteral("a"), 30));
        std::vector<std::filesystem::path> made;
        const QDateTime start = QDateTime::fromSecsSinceEpoch(1700000000);
        for (int i = 0; i < 3; ++i) made.push_back(app::backup::make(book, folder, 2, start.addSecs(60 * i)));
        const auto kept = app::backup::backups(folder, QStringLiteral("a"));
        QVERIFY(kept == std::vector<std::filesystem::path>(made.begin() + 1, made.end()));
        QCOMPARE(gui_test::qpath(kept.back().filename()), QStringLiteral("a_%1.zip").arg(start.addSecs(120).toString(QStringLiteral("yyyyMMdd-HHmmss"))));
        const auto inside = render::zip::read(kept.back());
        QVERIFY(inside.contains("a.genko/project.json"));
        QVERIFY(!app::backup::due(folder, QStringLiteral("a"), 30));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, app::backup::make(book, book / "backups"));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, app::backup::make(book, book));
        // what is being written (.tmp) and the lock are never in it; no part is left, after a backup or a failed one
        genko::test::write_bytes(gui_test::qpath(book / "assets" / "x.png.tmp"), "half");
        genko::test::write_bytes(gui_test::qpath(book / "project.lock"), "");
        const auto later = app::backup::make(book, folder, 5, start.addSecs(600));
        const auto held = render::zip::read(later);
        QVERIFY(held.contains("a.genko/project.json") && !held.contains("a.genko/assets/x.png.tmp") && !held.contains("a.genko/project.lock"));
        QVERIFY_THROWS_EXCEPTION(std::exception, app::backup::make(gui_test::path_of(tmp.path() + QStringLiteral("/gone.genko")), folder, 5));
        QVERIFY(QDir(gui_test::qpath(folder)).entryList({QStringLiteral("*.part")}, QDir::Files).isEmpty());
        // after a save in the window: kept in the folder chosen in 環境設定
        Book open;
        store()->setValue(QStringLiteral("backup/folder"), gui_test::qpath(folder));
        store()->setValue(QStringLiteral("backup/minutes"), 0);
        QVERIFY(open.window->apply_ops(Json::array({Json{{"op", "add_page"}, {"count", 1}}})));
        open.window->session().save_now();
        QVERIFY(gui_test::wait_for([&] { return !app::backup::backups(folder, QStringLiteral("b")).empty(); }, 60000));
        for (const char* key : {"backup/folder", "backup/minutes"}) store()->remove(QString::fromLatin1(key));
    }
};

QTEST_MAIN(TestGuiLook)
#include "test_gui_look.moc"
