// PC5 H-c (SPEC VIEW-01「窓の区画と並べ方」, 節 H 6 章 H-c, the user's decision 7): ウィンドウ → アレンジ in one window — the
// books side by side in cells (4 アップ: four cells, the canvas in the one in front, the others' pages in panes), a pane
// clicked brings its tab to the front there and a line drawn goes into its book, a tab in no cell comes to the cell in
// front, sixteen cells at most (the rest by their tabs), a pane's book drawn again once its changes stop; ウィンドウを分離・
// すべてのウィンドウを分離 move tabs into windows of their own and すべてをタブに統合 takes them back (one book opened in two
// windows not twice); 同じズーム倍率で表示・同じ位置に表示・同じ回転で表示・すべてを同じ表示にする; Shift with the wheel, the
// hand and the zoom tool in every pane; closing asks once for a book however it is shown; the canvas drawn by the graphics
// card after it moves to another cell (its context made again: on Xvfb with Mesa — test_gui_arrange_gpu —, else the way
// back to the processor); ウィンドウ → ツール, 修飾キー (Shift, Ctrl, Alt kept down for the tools) and the panels' switches;
// and the one window, one pane way as before: the canvas alone, the same picture, the book's bytes untouched.

#include <QtTest>

#include <QAction>
#include <QCryptographicHash>
#include <QDockWidget>
#include <QMenu>
#include <QMenuBar>
#include <QOpenGLWidget>
#include <QSplitter>
#include <QTabBar>
#include <QToolBar>
#include <QToolButton>
#include <QWheelEvent>

#include <cmath>
#include <map>

#include "app/canvas.hpp"
#include "app/doc_area.hpp"
#include "app/documents.hpp"
#include "app/icons.hpp"
#include "app/inject.hpp"
#include "app/main_window.hpp"
#include "app/pane_view.hpp"
#include "app/save_status.hpp"
#include "app/theme.hpp"
#include "core/command_bus.hpp"
#include "gui_support.hpp"

using namespace gui_test;
using genko::app::DocArea;
using genko::app::MainWindow;
using genko::app::PageCanvas;
using genko::app::PaneView;
using genko::app::Session;
namespace core = genko::core;
namespace inject = genko::app::inject;

namespace {

// A window on a book, more books opened in its tabs, the questions answered by the test.
struct Desk {
    QTemporaryDir dir;
    fs::path book;
    std::shared_ptr<Session> session;
    std::vector<std::shared_ptr<Session>> more;
    std::unique_ptr<Answers> answers = std::make_unique<Answers>();
    std::unique_ptr<MainWindow> window;

    explicit Desk(int books = 1) {
        book = path_of(dir.filePath("book.genko"));
        write_book(book, new_doc(2, "本0"));
        session = Session::open(book, quick(path_of(dir.filePath("recovery"))));
        window = std::make_unique<MainWindow>(session);
        window->resize(1400, 900);
        window->show();
        for (int i = 1; i < books; ++i) open(i);
        QCoreApplication::processEvents();
    }
    ~Desk() {
        for (MainWindow* other : genko::app::documents::windows()) {
            if (other != window.get()) other->close();
        }
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        window.reset();
        more.clear();
        session.reset();
        answers.reset();
    }
    std::shared_ptr<Session> open(int i, int pages = 2) {
        const fs::path where = path_of(dir.filePath(QStringLiteral("book%1.genko").arg(i)));
        write_book(where, new_doc(pages, "本" + std::to_string(i)));
        auto s = Session::open(where, quick(path_of(dir.filePath("recovery"))));
        more.push_back(s);
        window->add_document(s);
        return s;
    }
    DocArea* area() const { return window->doc_area(); }
    PageCanvas* canvas() const { return window->canvas(); }
    void trigger(const char* name) const {
        QAction* a = window->action(QString::fromLatin1(name));
        QVERIFY2(a != nullptr, name);
        a->trigger();
    }
    // The title of the book a pane shows.
    static std::string title_of(const PageCanvas* c) { return c->doc() ? c->doc()->title : std::string(); }
    // The book's files and their bytes.
    static std::map<std::string, QByteArray> files(const fs::path& dir) {
        std::map<std::string, QByteArray> out;
        for (const auto& entry : fs::recursive_directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;
            const std::string name = entry.path().lexically_relative(dir).generic_string();
            if (name.find("lock") != std::string::npos) continue;  // (the open book's own lock)
            QFile file(qpath(entry.path()));
            if (file.open(QIODevice::ReadOnly)) out[name] = file.readAll();
        }
        return out;
    }
};

int cell_of_canvas(const Desk& desk) {
    for (int i = 0; i < static_cast<int>(desk.area()->tabs().size()); ++i) {
        if (desk.canvas()->parentWidget() == desk.area()->cell(i)) return i;
    }
    return -1;
}

}  // namespace

class TestGuiArrange : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        config_folder();
        genko::app::icons::init_resources();
        genko::app::theme::apply(qApp);
    }

    // The one window, one pane way (節 H: the same look, the same keys, the same saves): the canvas alone in the middle
    // with no marks on the tabs; arranged and taken back into tabs, the same again — the same picture, the book's files
    // byte for byte.
    void onePaneIsAsBefore() {
        Desk desk;
        DocArea* area = desk.area();
        PageCanvas* c = desk.canvas();
        QCOMPARE(c->parentWidget(), static_cast<QWidget*>(area));
        QCOMPARE(area->layout(), QStringLiteral("tabs"));
        QCOMPARE(area->front(), -1);
        QVERIFY(area->panes().empty());
        QCOMPARE(c->geometry(), area->rect());
        QVERIFY(desk.window->doc_tabs()->tabIcon(0).isNull());
        QVERIFY(!c->view_only());
        desk.window->choose_tool(QStringLiteral("pen"));
        const QPointF m = c->seen_mm().center();
        inject::mouse_stroke(c, {m, m + QPointF(12, 4)});
        QCOMPARE(ink_strokes(desk.session->document()), std::size_t{1});
        desk.session->save_now();
        QVERIFY(desk.session->wait_saved(10s));
        const auto bytes = Desk::files(desk.book);
        QVERIFY(!bytes.empty());
        QVERIFY(c->wait_rendered(30000));
        const QRect place = c->geometry();
        const auto away = [c] {  // (the pointer off the page: no brush circle in the picture)
            QEvent leave(QEvent::Leave);
            QCoreApplication::sendEvent(c, &leave);
        };
        away();
        const QImage before = c->grab().toImage();
        // (one book: nothing to arrange — said, nothing moved)
        desk.trigger("act_arrange_4up");
        QCOMPARE(area->layout(), QStringLiteral("tabs"));
        QVERIFY(desk.window->last_notice().contains(QStringLiteral("2 つ以上")));
        // (one book: nothing to float — said, the window as it was)
        const std::size_t windows = genko::app::documents::windows().size();
        desk.trigger("act_float_window");
        QCOMPARE(genko::app::documents::windows().size(), windows);
        QVERIFY(desk.window->last_notice().contains(QStringLiteral("1 つ")));
        // two books side by side, then the tabs again
        desk.open(1);
        desk.window->switch_document(0);
        desk.trigger("act_arrange_2up_vertical");
        QCOMPARE(area->layout(), QStringLiteral("twoUpVertical"));
        desk.trigger("act_arrange_consolidate");
        QCOMPARE(area->layout(), QStringLiteral("tabs"));
        QCOMPARE(c->parentWidget(), static_cast<QWidget*>(area));
        QVERIFY(area->panes().empty());
        QVERIFY(desk.window->doc_tabs()->tabIcon(0).isNull() && desk.window->doc_tabs()->tabIcon(1).isNull());
        QCoreApplication::processEvents();
        QCOMPARE(c->geometry(), place);
        QVERIFY(c->fitted());
        QVERIFY(c->wait_rendered(30000));
        QTest::qWait(250);
        QVERIFY(c->wait_rendered(30000));
        away();
        QCOMPARE(c->grab().toImage(), before);
        desk.session->save_now();
        QVERIFY(desk.session->wait_saved(10s));
        QVERIFY(Desk::files(desk.book) == bytes);  // (the arrangement is the window's, never the book's)
    }

    // 4 アップ: four cells (quarters), the tab in front in the first with the canvas, the next three tabs' pages in panes
    // (only looked at, the whole page's tiles only), each tab marked with its cell; a pane clicked brings its tab to the
    // front in that cell (the canvas moves there, the pane to the cell it left, showing the book that left the front), a
    // line drawn goes into that book; a pane's book changed elsewhere is drawn again once the changes stop.
    void fourUpPanes() {
        Desk desk(5);
        desk.window->switch_document(0);
        DocArea* area = desk.area();
        desk.trigger("act_arrange_4up");
        QCOMPARE(area->layout(), QStringLiteral("fourUp"));
        QCOMPARE(area->tabs().size(), std::size_t{4});
        QCOMPARE(area->front(), 0);
        QCOMPARE(cell_of_canvas(desk), 0);
        QCOMPARE(area->panes().size(), std::size_t{3});
        QCoreApplication::processEvents();
        const QSize room = area->size();
        for (int i = 0; i < 4; ++i) {
            const QRect r = area->cell(i)->geometry();
            QVERIFY2(std::abs(r.width() - room.width() / 2) <= 8 && std::abs(r.height() - room.height() / 2) <= 8,
                     qPrintable(QStringLiteral("cell %1: %2×%3 of %4×%5").arg(i).arg(r.width()).arg(r.height()).arg(room.width()).arg(room.height())));
        }
        for (int i = 1; i < 4; ++i) {
            PaneView* pane = area->pane(i);
            QVERIFY(pane != nullptr && pane->view_only());
            QCOMPARE(Desk::title_of(pane), "本" + std::to_string(i));
            QVERIFY(pane->wait_rendered(30000));
            QCOMPARE(pane->renderer().shown_dpi(), pane->base_dpi());  // (節 H 4.5: the whole page's tiles only)
        }
        for (int i = 0; i < 4; ++i) QVERIFY2(!desk.window->doc_tabs()->tabIcon(i).isNull(), qPrintable(QString::number(i)));
        QVERIFY(desk.window->doc_tabs()->tabIcon(4).isNull());  // (the fifth in no cell)
        // a pane clicked
        PaneView* third = area->pane(2);
        QTest::mouseClick(third, Qt::LeftButton, {}, third->rect().center());
        QCOMPARE(desk.window->current_document(), 2);
        QCOMPARE(desk.window->book().title, std::string("本2"));
        QCOMPARE(area->front(), 2);
        QCOMPARE(cell_of_canvas(desk), 2);
        QCOMPARE(area->tabs()[2], desk.window->documents()[2].tab);
        PaneView* moved = area->pane(0);
        QVERIFY(moved != nullptr);
        QCOMPARE(Desk::title_of(moved), std::string("本0"));
        QCOMPARE(desk.window->doc_tabs()->currentIndex(), 2);
        // a line drawn goes into the book in front (本2), no other
        desk.window->choose_tool(QStringLiteral("pen"));
        const QPointF m = desk.canvas()->seen_mm().center();
        inject::mouse_stroke(desk.canvas(), {m, m + QPointF(10, 5)});
        QCOMPARE(ink_strokes(desk.more[1]->document()), std::size_t{1});
        QCOMPARE(ink_strokes(desk.session->document()), std::size_t{0});
        QCOMPARE(ink_strokes(desk.more[0]->document()), std::size_t{0});
        // 本1 changed elsewhere (as an agent's edit): its pane draws it again once the changes stop (節 H 4.5)
        PaneView* first = area->pane(1);
        QCOMPARE(Desk::title_of(first), std::string("本1"));
        const auto before = first->doc();
        desk.more[0]->apply(Json::array({Json{{"op", "add_page"}, {"count", 1}}}));
        QVERIFY(first->waiting());
        QCOMPARE(first->doc(), before);  // (not at once)
        QVERIFY(wait_for([&] { return !first->waiting(); }, 5000));
        QCOMPARE(first->doc(), desk.more[0]->snapshot());
        // the cells' order kept: back to the first cell's tab
        QTest::mouseClick(moved, Qt::LeftButton, {}, moved->rect().center());
        QCOMPARE(desk.window->current_document(), 0);
        QCOMPARE(cell_of_canvas(desk), 0);
        QCOMPARE(Desk::title_of(area->pane(2)), std::string("本2"));
    }

    // A tab in no cell, chosen by its tab, comes to the cell in front (Photoshop's n-up); a tab closed leaves its cell.
    void aTabInNoCellComesToTheFront() {
        Desk desk(3);
        desk.window->switch_document(0);
        DocArea* area = desk.area();
        desk.trigger("act_arrange_2up_vertical");
        QCOMPARE(area->tabs().size(), std::size_t{2});
        const std::uint64_t third = desk.window->documents()[2].tab;
        QCOMPARE(area->cell_of(third), -1);
        desk.window->doc_tabs()->setCurrentIndex(2);
        QCOMPARE(desk.window->current_document(), 2);
        QCOMPARE(area->cell_of(third), 0);
        QCOMPARE(cell_of_canvas(desk), 0);
        QCOMPARE(area->cell_of(desk.window->documents()[0].tab), -1);
        QCOMPARE(Desk::title_of(area->pane(1)), std::string("本1"));
        // the pane's tab closed: one book left in cells — the tabs' way
        QVERIFY(desk.window->close_document(1));
        QCOMPARE(area->layout(), QStringLiteral("tabs"));
        QVERIFY(area->panes().empty());
        QCOMPARE(desk.canvas()->parentWidget(), static_cast<QWidget*>(area));
    }

    // Sixteen cells at most (節 H 3 章): the seventeenth book by its tab, shown in the cell in front.
    void sixteenAtMost() {
        Desk desk(17);
        desk.window->switch_document(0);
        DocArea* area = desk.area();
        desk.trigger("act_arrange_tile_vertical");
        QCOMPARE(area->tabs().size(), std::size_t{DocArea::kMostPanes});
        QCOMPARE(area->panes().size(), std::size_t{DocArea::kMostPanes - 1});
        QVERIFY(desk.window->last_notice().contains(QStringLiteral("16")));
        const std::uint64_t last = desk.window->documents()[16].tab;
        QCOMPARE(area->cell_of(last), -1);
        desk.window->switch_document(16);
        QCOMPARE(area->cell_of(last), area->front());
        QCOMPARE(area->tabs().size(), std::size_t{DocArea::kMostPanes});
        // the arrangements' cells (PhotoCraft's cells)
        QCOMPARE(DocArea::capacity(QStringLiteral("sixUp"), 17), 6);
        QCOMPARE(DocArea::capacity(QStringLiteral("fourUp"), 3), 3);
        QCOMPARE(DocArea::capacity(QStringLiteral("tile"), 5), 5);
        QCOMPARE(DocArea::capacity(QStringLiteral("tabs"), 5), 0);
        QCOMPARE(DocArea::capacity(QStringLiteral("twoUpVertical"), 1), 0);
        const auto stacked = DocArea::cells(QStringLiteral("threeUpStacked"), 3);
        QCOMPARE(stacked.size(), std::size_t{3});
        QCOMPARE(stacked[0], QRectF(0, 0, 0.5, 1));
        QCOMPARE(stacked[2], QRectF(0.5, 0.5, 0.5, 0.5));
        const auto tiled = DocArea::cells(QStringLiteral("tile"), 5);  // (3 columns, 2 rows, the last short)
        QCOMPARE(tiled.size(), std::size_t{5});
        QCOMPARE(tiled[4], QRectF(1.0 / 3, 0.5, 1.0 / 3, 0.5));
    }

    // ウィンドウを分離 moves the tab in front into a window of its own (its page and view with it), すべてのウィンドウを分離 the
    // others; すべてをタブに統合 takes every window's tabs back, closing those windows without a question; a book opened in two
    // windows comes back once.
    void floatingAndTakingBack() {
        Desk desk(3);
        const std::size_t windows = genko::app::documents::windows().size();
        desk.window->switch_document(1);
        desk.window->go_to_page(2);
        desk.canvas()->set_zoom_percent(30);
        const double zoom = desk.canvas()->zoom_percent_exact();
        desk.trigger("act_float_window");
        QCOMPARE(genko::app::documents::windows().size(), windows + 1);
        QCOMPARE(desk.window->documents().size(), std::size_t{2});
        MainWindow* floated = nullptr;
        for (MainWindow* w : genko::app::documents::windows())
            if (w != desk.window.get()) floated = w;
        QVERIFY(floated != nullptr);
        QCOMPARE(floated->book().title, std::string("本1"));
        QCOMPARE(floated->documents().size(), std::size_t{1});
        QCOMPARE(floated->current_page()->index, core::Num(2));
        QVERIFY(std::abs(floated->canvas()->zoom_percent_exact() - zoom) < 0.5);
        QVERIFY(desk.answers->asked.isEmpty());  // (nothing asked: the book is open there)
        // すべてのウィンドウを分離: the other tab here into its own window
        desk.trigger("act_float_all");
        QCOMPARE(genko::app::documents::windows().size(), windows + 2);
        QCOMPARE(desk.window->documents().size(), std::size_t{1});
        // the same book in one more window (新規ウィンドウ), then すべてをタブに統合
        desk.trigger("act_new_window");
        QCOMPARE(genko::app::documents::windows().size(), windows + 3);
        desk.trigger("act_arrange_consolidate");
        QCOMPARE(genko::app::documents::windows().size(), windows);
        QCOMPARE(desk.window->documents().size(), std::size_t{3});  // (本0 once)
        QStringList titles;
        for (const auto& d : desk.window->documents()) titles << QString::fromStdString(d.session->document().title);
        titles.sort();
        QCOMPARE(titles, (QStringList{QStringLiteral("本0"), QStringLiteral("本1"), QStringLiteral("本2")}));
        QCOMPARE(desk.window->doc_tabs()->count(), 3);
        QVERIFY(desk.answers->asked.isEmpty());
        // 本1 came back with its page
        for (int i = 0; i < 3; ++i) {
            if (desk.window->documents()[static_cast<std::size_t>(i)].session->document().title == "本1") {
                desk.window->switch_document(i);
                QCOMPARE(desk.window->current_page()->index, core::Num(2));
            }
        }
        // 重ねて表示: the others into windows 40 px apart, this one on top
        desk.trigger("act_arrange_cascade");
        QCOMPARE(genko::app::documents::windows().size(), windows + 2);
        QCOMPARE(desk.window->documents().size(), std::size_t{1});
        desk.trigger("act_arrange_consolidate");
        QCOMPARE(genko::app::documents::windows().size(), windows);
        QCOMPARE(desk.window->documents().size(), std::size_t{3});
    }

    // 同じズーム倍率で表示・同じ位置に表示・同じ回転で表示・すべてを同じ表示にする: the panes (and other windows) take the view in front —
    // its zoom by its %, its place on its page, its turn and mirrors.
    void matchingViews() {
        Desk desk(3);
        desk.window->switch_document(0);
        desk.trigger("act_arrange_3up_vertical");
        PageCanvas* c = desk.canvas();
        c->set_zoom_percent(40);
        c->center_on(60, 90);
        c->set_rotation(30);
        c->flip_view(true);
        desk.trigger("act_match_zoom");
        for (PaneView* pane : desk.area()->panes()) {
            QVERIFY(std::abs(pane->zoom_percent_exact() - c->zoom_percent_exact()) < 0.01);
            QCOMPARE(pane->rotation(), 0.0);
        }
        desk.trigger("act_match_all");
        const QPointF middle = c->to_mm(QPointF(c->width() / 2.0, c->height() / 2.0));
        for (PaneView* pane : desk.area()->panes()) {
            QVERIFY(std::abs(pane->zoom_percent_exact() - c->zoom_percent_exact()) < 0.01);
            QCOMPARE(pane->rotation(), 30.0);
            QVERIFY(pane->flipped());
            const QPointF there = pane->to_mm(QPointF(pane->width() / 2.0, pane->height() / 2.0));
            QVERIFY2(std::abs(there.x() - middle.x()) < 0.5 && std::abs(there.y() - middle.y()) < 0.5,
                     qPrintable(QStringLiteral("%1,%2 against %3,%4").arg(there.x()).arg(there.y()).arg(middle.x()).arg(middle.y())));
        }
        // the view as the pane showed it goes with its tab to the front
        PaneView* pane = desk.area()->pane(1);
        const double seen = pane->zoom_percent_exact();
        QTest::mouseClick(pane, Qt::LeftButton, {}, pane->rect().center());
        QVERIFY(std::abs(desk.canvas()->zoom_percent_exact() - seen) < 0.01);
        QCOMPARE(desk.canvas()->rotation(), 30.0);
    }

    // Shift with the wheel (and Ctrl+Shift's zoom) moves every pane as the one in front; without Shift, the front alone.
    // The hand tool's すべてのウィンドウをスクロール and the zoom tool's すべてのウィンドウをズーム, other windows too.
    void shiftMovesEveryPane() {
        Desk desk(2);
        desk.window->switch_document(0);
        desk.trigger("act_arrange_2up_vertical");
        PageCanvas* c = desk.canvas();
        PaneView* pane = desk.area()->pane(1);
        const auto wheel = [c](Qt::KeyboardModifiers keys) {
            const QPointF at(c->width() / 2.0, c->height() / 2.0);
            QWheelEvent event(at, c->mapToGlobal(at), QPoint(), QPoint(0, 120), Qt::NoButton, keys, Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(c, &event);
        };
        const QPointF pan = pane->pan();
        const double scale = pane->scale();
        wheel(Qt::NoModifier);
        QCOMPARE(pane->pan(), pan);
        wheel(Qt::ShiftModifier);
        QVERIFY(pane->pan() != pan);
        QCOMPARE(pane->pan().x() - pan.x(), 40.0);  // (Shift's wheel across, as the front's)
        const double front = c->scale();
        wheel(Qt::ControlModifier | Qt::ShiftModifier);
        QVERIFY(std::abs(pane->scale() / scale - c->scale() / front) < 1e-6);
        QVERIFY(c->scale() > front);
        // ズームツール's すべてのウィンドウをズーム: a click zooms the pane and another window's page
        MainWindow* other = desk.window->new_window();
        const double there = other->canvas()->scale();
        const double here = pane->scale();
        desk.window->action(QStringLiteral("act_zoom_all"))->setChecked(true);
        QVERIFY(c->zoom_all);
        desk.window->choose_tool(QStringLiteral("zoom"));
        inject::click_mm(c, c->seen_mm().center());
        QTRY_VERIFY(!c->gliding());
        QVERIFY(std::abs(pane->scale() / here - 2.0) < 0.01);
        QVERIFY(std::abs(other->canvas()->scale() / there - 2.0) < 0.01);
        desk.window->action(QStringLiteral("act_zoom_all"))->setChecked(false);
        inject::click_mm(c, c->seen_mm().center());
        QVERIFY(std::abs(pane->scale() / here - 2.0) < 0.01);  // (off: the front alone)
        // 手のひらツール's すべてのウィンドウをスクロール
        desk.window->action(QStringLiteral("act_scroll_all"))->setChecked(true);
        desk.window->choose_tool(QStringLiteral("hand"));
        const QPointF before = pane->pan();
        const QPointF other_before = other->canvas()->pan();
        const QPointF m = c->seen_mm().center();
        inject::mouse_stroke(c, {m, m + QPointF(5, 0), m + QPointF(10, 0)});
        QVERIFY(pane->pan().x() != before.x());
        QVERIFY(other->canvas()->pan().x() != other_before.x());
        other->close();
    }

    // Closing the window asks once for a book with changes not saved, whether it is in front or in a pane.
    void closingAsksOnce() {
        Desk desk(2);
        auto untitled = std::make_shared<Session>(core::new_episode("無題の本", core::Num(1), 2, core::PageSpec::a4_mono()), std::nullopt);
        desk.window->add_document(untitled);
        untitled->apply(Json::array({Json{{"op", "add_page"}, {"count", 1}}}));
        QVERIFY(untitled->unsaved());
        // (to another tab: そのまま切り替える — the changes stay in their tab)
        desk.answers->responder->exec = [](QDialog* dialog) {
            if (auto* guard = qobject_cast<genko::app::CloseGuard*>(dialog)) guard->discard_button()->click();
            return 0;
        };
        desk.answers->responder->question = [](const QString&, const QString&) { return true; };
        desk.window->switch_document(0);
        QCOMPARE(desk.window->current_document(), 0);
        desk.answers->responder->question = [](const QString&, const QString&) { return false; };
        desk.trigger("act_arrange_tile");
        QCOMPARE(desk.area()->tabs().size(), std::size_t{3});
        QCOMPARE(desk.area()->cell_of(desk.window->documents()[2].tab), 2);  // (in a pane)
        int asked = 0;
        desk.answers->responder->exec = [&asked](QDialog* dialog) {
            if (qobject_cast<genko::app::CloseGuard*>(dialog) != nullptr) ++asked;
            return 0;  // (キャンセル: the window stays)
        };
        QVERIFY(!desk.window->close());
        QCOMPARE(asked, 1);
        QVERIFY(!desk.window->closed());
    }

    // The canvas drawn by the graphics card moved to another cell: drawn again there, as the processor draws it (its
    // context may be made again — 節 H 9). With no OpenGL (offscreen), the way back to the processor, said once.
    void theCardFollowsTheFrontPane() {
        Desk desk(2);
        desk.window->switch_document(0);
        desk.trigger("act_arrange_2up_vertical");
        PageCanvas* c = desk.canvas();
        c->show_guides = false;
        QStringList failed;
        QObject::connect(c, &PageCanvas::gpuFailed, c, [&failed](const QString& message) { failed << message; });
        c->use_gpu(true);
        QTRY_VERIFY_WITH_TIMEOUT(!c->gpu() || qobject_cast<QOpenGLWidget*>(c->gpu_surface())->isValid(), 5000);
        PaneView* pane = desk.area()->pane(1);
        QTest::mouseClick(pane, Qt::LeftButton, {}, pane->rect().center());
        QCOMPARE(cell_of_canvas(desk), 1);
        QCOMPARE(desk.window->current_document(), 1);
        // (a view that is not resampled — one tile pixel a screen pixel, as test_gui_canvas's "as it is" —: held to 1/255)
        c->actual_size();
        c->center_on(105, 148);
        QVERIFY(c->wait_rendered(30000));
        QTest::qWait(250);
        QVERIFY(c->wait_rendered(30000));
        if (!c->gpu()) {
            QCOMPARE(failed.size(), 1);
            QVERIFY(failed.front().contains(QStringLiteral("プロセッサー")));
            QVERIFY(!c->grab().toImage().isNull());
            return;
        }
        auto* surface = qobject_cast<QOpenGLWidget*>(c->gpu_surface());
        QVERIFY(surface->isValid());
        QCOMPARE(surface->parentWidget(), static_cast<QWidget*>(c));
        QCOMPARE(surface->size(), c->size());
        QTRY_VERIFY_WITH_TIMEOUT(!surface->grabFramebuffer().isNull(), 5000);
        const QImage shown = surface->grabFramebuffer().convertToFormat(QImage::Format_RGB32);
        c->use_gpu(false);
        const QImage processor = c->grab().toImage().convertToFormat(QImage::Format_RGB32);
        const QImage card = shown.scaled(processor.size());
        int most = 0;
        for (int y = 0; y < card.height(); ++y) {
            for (int x = 0; x < card.width(); ++x) {
                const QRgb a = card.pixel(x, y), b = processor.pixel(x, y);
                most = std::max({most, std::abs(qRed(a) - qRed(b)), std::abs(qGreen(a) - qGreen(b)), std::abs(qBlue(a) - qBlue(b))});
            }
        }
        QVERIFY2(most <= 1, qPrintable(QStringLiteral("the card's picture after the move is %1/255 from the processor's").arg(most)));
        QVERIFY(failed.isEmpty());
    }

    // ウィンドウ → ツール (the palette shown or hidden), 修飾キー (Shift, Ctrl, Alt kept down for the canvas's tools) and the
    // panels' switches (ヒストリー, レイヤー, ナビゲーター, オプション).
    void thePanels() {
        Desk desk;
        MainWindow* w = desk.window.get();
        QMenu* window = nullptr;
        for (QAction* top : w->menuBar()->actions())
            if (top->menu() != nullptr && top->menu()->title() == QStringLiteral("ウィンドウ")) window = top->menu();
        QVERIFY(window != nullptr);
        const auto item = [window](const QString& words) -> QAction* {
            for (QAction* a : window->actions())
                if (a->text() == words) return a;
            return nullptr;
        };
        QAction* tools = item(QStringLiteral("ツール"));
        QVERIFY(tools != nullptr);
        QVERIFY(w->tool_palette()->isVisible());
        tools->trigger();
        QVERIFY(!w->tool_palette()->isVisible());
        tools->trigger();
        QVERIFY(w->tool_palette()->isVisible());
        // (オプション: always there, as before — its switch shows that it is)
        QAction* options = item(QStringLiteral("オプション"));
        QVERIFY(options != nullptr && options->isChecked());
        for (const char* panel : {"ヒストリー", "レイヤー", "ナビゲーター"}) {
            QAction* a = item(QString::fromUtf8(panel));
            QVERIFY2(a != nullptr, panel);
            const bool was = a->isChecked();
            a->trigger();
            QVERIFY2(a->isChecked() == !was, panel);
            a->trigger();
            QVERIFY2(a->isChecked() == was, panel);
        }
        // 修飾キー
        QAction* keys = item(QStringLiteral("修飾キー"));
        QVERIFY(keys != nullptr);
        auto* dock = w->findChild<QDockWidget*>(QStringLiteral("修飾キー"));
        QVERIFY(dock != nullptr && !dock->isVisible());
        keys->trigger();
        QVERIFY(dock->isVisible());
        auto* shift = dock->findChild<QToolButton*>(QStringLiteral("mod_shift"));
        auto* alt = dock->findChild<QToolButton*>(QStringLiteral("mod_alt"));
        QVERIFY(shift != nullptr && alt != nullptr && dock->findChild<QToolButton*>(QStringLiteral("mod_ctrl")) != nullptr);
        // Shift kept down: the marquee's new area adds to the selection
        w->action(QStringLiteral("act_marquee"))->trigger();
        PageCanvas* c = desk.canvas();
        const QPointF m = c->seen_mm().center();
        inject::mouse_stroke(c, {m, m + QPointF(10, 10)});
        QCOMPARE(c->selection_how(), QStringLiteral("replace"));
        shift->click();
        QCOMPARE(w->latched_keys(), Qt::KeyboardModifiers(Qt::ShiftModifier));
        inject::mouse_stroke(c, {m + QPointF(20, 20), m + QPointF(30, 30)});
        QCOMPARE(c->selection_how(), QStringLiteral("add"));
        shift->click();
        QCOMPARE(w->latched_keys(), Qt::KeyboardModifiers());
        // Alt kept down with the pen: 環境設定's Alt tool (スポイト) in hand, as the key held
        w->choose_tool(QStringLiteral("pen"));
        alt->click();
        QVERIFY(c->tool_held());
        QCOMPARE(c->tool(), QStringLiteral("picker"));
        alt->click();
        QVERIFY(!c->tool_held());
        QCOMPARE(c->tool(), QStringLiteral("pen"));
        // hidden: the keys let go
        alt->click();
        keys->trigger();
        QVERIFY(!dock->isVisible());
        QCOMPARE(w->latched_keys(), Qt::KeyboardModifiers());
    }
};

QTEST_MAIN(TestGuiArrange)
#include "test_gui_arrange.moc"
