// Checks of Python's tests that no other C++ test made (docs/cpp-migration/ledger.json, M7): the main window, its
// dialogs and panels on the offscreen platform, and the few checks of the same Python tests that need no window (the
// words of every error, parse_marks, the remembered lines drawn again). Ported from tests/test_f1.py, test_f2.py,
// test_f3.py, test_f4.py, test_f5.py, test_g1.py, test_g2.py, test_g3.py, test_g4.py, test_g5.py, test_h2.py,
// test_h4.py, test_i0.py, test_j1.py and test_hd_report.py; each slot is named after the Python test it ports.
//
// DEPS genko_app genko_test_support GUI

#include <QtTest>

#include <QAbstractButton>
#include <QAbstractScrollArea>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTabBar>
#include <QTabWidget>
#include <QToolBar>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <numbers>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "app/brush_panel.hpp"
#include "app/canvas.hpp"
#include "app/config.hpp"
#include "app/dialogs.hpp"
#include "app/help.hpp"
#include "app/icons.hpp"
#include "app/ime.hpp"
#include "app/inject.hpp"
#include "app/layer_panel.hpp"
#include "app/lettering.hpp"
#include "app/live_ink.hpp"
#include "app/main_window.hpp"
#include "app/material_tabs.hpp"
#include "app/navigator.hpp"
#include "app/pages_panel.hpp"
#include "app/pen.hpp"
#include "app/story_panel.hpp"
#include "app/theme.hpp"
#include "app/thumbs.hpp"
#include "app/tool_settings.hpp"
#include "app/workspace.hpp"
#include "core/command_bus.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "core/strokes.hpp"
#include "formats/exporting.hpp"
#include "gui_support.hpp"
#include "render/image.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/tones.hpp"
#include "service/wording.hpp"
#include "storage/writer.hpp"
#include "testsupport.hpp"

using namespace gui_test;
using genko::app::MainWindow;
using genko::app::PageCanvas;
using genko::app::Session;
namespace app = genko::app;
namespace core = genko::core;
namespace inject = genko::app::inject;
namespace render = genko::render;
namespace wording = genko::wording;

namespace {

// The panels on the right of Python's window (main.py studio_docks), in its order.
const QStringList& studio_docks() {
    static const QStringList titles = {QStringLiteral("承認箱"), QStringLiteral("レイヤー"), QStringLiteral("台詞"), QStringLiteral("ページ"),
                                       QStringLiteral("カラー"), QStringLiteral("コマの詳細"), QStringLiteral("ヒストリー"), QStringLiteral("素材"),
                                       QStringLiteral("定規・3D"), QStringLiteral("点検"), QStringLiteral("資料"),
                                       // (PC5 J-a: ウィンドウ → レイヤーカンプ is a command, its dock's own switch beside it)
                                       QStringLiteral("レイヤーカンプ")};
    return titles;
}

// Python's new_episode("t", 1, pages, spec): the books of the tests ported here.
core::Document book_of(int pages, const core::PageSpec& spec = core::PageSpec::b4_comic()) {
    return core::new_episode("t", core::Num(1), pages, spec);
}

const core::CommandBus& bus() {
    static const core::CommandBus b(genko::render::ops_registry());
    return b;
}

// Python's apply_ops(ep, ops) on a book before it is saved (the actor "genko", as the command line): "" when the ops
// applied, else what refused them.
std::string apply_to(core::Document& doc, const Json& ops) {
    try {
        doc = bus().apply(doc, ops, core::Actor()).doc;
        return {};
    } catch (const std::exception& error) {
        return error.what();
    }
}

#define APPLY(doc, ops)                                     \
    do {                                                    \
        const std::string refused_ = apply_to((doc), (ops)); \
        QVERIFY2(refused_.empty(), refused_.c_str());       \
    } while (0)

// [[x, y], …] for an op's points.
Json pts(std::initializer_list<std::array<double, 2>> points) {
    Json out = Json::array();
    for (const auto& p : points) out.push_back(Json::array({p[0], p[1]}));
    return out;
}

std::string spec_json(const core::PageSpec& spec) { return genko::storage::spec_to_json(spec).dump(); }

const core::Layer* first_layer(const core::Page& page, core::LayerRole role) {
    for (const core::Layer& layer : page.layers) {
        if (layer.role == role) return &layer;
    }
    return nullptr;
}

const core::Layer* layer_by_id(const core::Page& page, const std::string& id) {
    for (const core::Layer& layer : page.layers) {
        if (layer.id == id) return &layer;
    }
    return nullptr;
}

// A book written as Python's save_episode writes it and opened in a window of this size (its questions answered by
// the test: none answered unless a test says so, and each one asked is noted).
struct Book {
    QTemporaryDir dir;
    fs::path path;
    std::shared_ptr<Session> session;
    std::unique_ptr<Answers> answers = std::make_unique<Answers>();
    std::unique_ptr<MainWindow> window;

    explicit Book(const core::Document& doc, QSize size = QSize(1280, 800), const QString& name = QStringLiteral("b.genko"),
                  const std::function<void(const fs::path&)>& before = {}) {
        path = path_of(dir.filePath(name));
        write_book(path, doc);
        if (before) before(path);
        session = Session::open(path, quick(path_of(dir.filePath(QStringLiteral("recovery")))));
        window = std::make_unique<MainWindow>(session);
        window->resize(size);
        window->show();
        (void)QTest::qWaitForWindowExposed(window.get());
        for (int i = 0; i < 3; ++i) QCoreApplication::processEvents();
    }
    ~Book() {
        window.reset();
        session.reset();
        answers.reset();
    }
    Book(const Book&) = delete;
    Book& operator=(const Book&) = delete;

    MainWindow* w() const { return window.get(); }
    PageCanvas* canvas() const { return window->canvas(); }
    const core::Page& page() const { return *window->current_page(); }
    QAction* act(const char* name) const { return window->action(QString::fromLatin1(name)); }
    void trigger(const char* name) const {
        QAction* a = act(name);
        QVERIFY2(a != nullptr, name);
        a->trigger();
    }
    QDockWidget* dock(const QString& title) const {
        for (QDockWidget* d : window->findChildren<QDockWidget*>()) {
            if (d->windowTitle() == title) return d;
        }
        return nullptr;
    }
    QMenu* menu(const QString& title) const {
        for (QAction* top : window->menuBar()->actions()) {
            if (top->text() == title) return top->menu();
        }
        return nullptr;
    }
};

QStringList texts(QMenu* menu) {
    QStringList out;
    if (menu == nullptr) return out;
    for (QAction* a : menu->actions()) out << a->text();
    return out;
}

QMenu* submenu(QMenu* menu, const QString& title) {
    if (menu == nullptr) return nullptr;
    for (QAction* a : menu->actions()) {
        if (a->menu() != nullptr && a->text() == title) return a->menu();
    }
    return nullptr;
}

// Python's overflow(root): the parts of a panel in sight that reach past its right edge (cut off). What is inside a
// list or a text box scrolls there, and is not counted.
QStringList overflow(QWidget* root) {
    const int width = root->width();
    QStringList out;
    for (QWidget* child : root->findChildren<QWidget*>()) {
        if (!child->isVisibleTo(root) || qobject_cast<QScrollBar*>(child) != nullptr || child->width() <= 0) continue;
        bool inside = false;
        for (QWidget* up = child->parentWidget(); up != nullptr && up != root; up = up->parentWidget()) {
            if (qobject_cast<QAbstractScrollArea*>(up) != nullptr) {
                inside = true;
                break;
            }
        }
        if (inside) continue;
        const int right = child->mapTo(root, child->rect().topRight()).x();
        if (right > width + 1) {
            QString text;
            if (auto* button = qobject_cast<QAbstractButton*>(child)) text = button->text();
            if (auto* label = qobject_cast<QLabel*>(child)) text = label->text();
            out << QStringLiteral("%1 %2 → %3/%4").arg(QString::fromLatin1(child->metaObject()->className()), text).arg(right).arg(width);
        }
    }
    return out;
}

// How much of two pictures' ink is the same (alpha over 96): the pixels inked in both over those inked in either.
double overlap(const render::Image& a, const render::Image& b) {
    const std::string x = a.getchannel(std::string_view("A")).tobytes();
    const std::string y = b.getchannel(std::string_view("A")).tobytes();
    if (x.size() != y.size() || x.empty()) return 0.0;
    std::size_t both = 0;
    std::size_t either = 0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const bool p = static_cast<unsigned char>(x[i]) > 96;
        const bool q = static_cast<unsigned char>(y[i]) > 96;
        both += (p && q) ? 1 : 0;
        either += (p || q) ? 1 : 0;
    }
    return static_cast<double>(both) / static_cast<double>(std::max<std::size_t>(1, either));
}

double number_at(const Json& object, const char* key, double fallback) {
    const auto found = object.is_object() ? object.find(key) : object.end();
    return found != object.end() && found->is_number() ? found->get<double>() : fallback;
}

}  // namespace

class TestGuiLedgerA : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        config_folder();
        qRegisterMetaType<genko::app::StrokeInput>();
        qRegisterMetaType<genko::app::BookChange>();
        app::icons::init_resources();
        app::theme::apply(qApp);
    }

    // --- tests/test_f1.py -------------------------------------------------------------------------------------------

    // test_new_project_offers_the_papers_and_custom_numbers: 新しい原稿 offers the papers in Python's order (b4, b5, a5,
    // a4 … 自分で決める…) with a note of the basic frame; the paper dialog takes numbers of one's own (refused, in the
    // person's words, when the finished size is larger than the paper) and a preset again by its key; a book made on a
    // paper chosen through 自分で決める… is written on that paper.
    void test_new_project_offers_the_papers_and_custom_numbers() {
        Answers answers;
        app::settings()->remove(QStringLiteral("new/paper"));  // (環境設定's paper: none chosen, the first of the list)
        app::NewProjectDialog dialog;
        QStringList keys;
        for (int i = 0; i < dialog.paper->count(); ++i) keys << dialog.paper->itemData(i).toString();
        QCOMPARE(keys.mid(0, 4), (QStringList{QStringLiteral("b4"), QStringLiteral("b5"), QStringLiteral("a5"), QStringLiteral("a4")}));
        QCOMPARE(keys.last(), QStringLiteral("custom"));
        QStringList notes;
        for (QLabel* label : dialog.findChildren<QLabel*>()) notes << label->text();
        QVERIFY2(notes.join(QStringLiteral(" | ")).contains(QStringLiteral("基本枠 180×270")), qPrintable(notes.join(QStringLiteral(" | "))));
        // numbers of one's own
        {
            app::PaperDialog paper(nullptr, core::PageSpec::b4_comic());
            paper.trim_w->setValue(200);
            paper.inner->setValue(25);
            QCOMPARE(paper.spec().trim_size().first.value(), 200.0);
            QVERIFY(paper.ok_button()->isEnabled());
            paper.trim_w->setValue(400);  // (bigger than the paper)
            QVERIFY(!paper.ok_button()->isEnabled());
            QVERIFY2(paper.summary->text().contains(QStringLiteral("小さく")), qPrintable(paper.summary->text()));
            paper.preset->setCurrentIndex(paper.preset->findData(QStringLiteral("a5")));
            QCOMPARE(spec_json(paper.spec()), spec_json(core::PageSpec::a5_doujin()));
            QCOMPARE(paper.op()["preset"].dump(), std::string("\"a5\""));
        }
        // 自分で決める…: the paper dialog asked (the test chooses b5 there), the book made on it
        answers.responder->exec = [](QDialog* d) {
            auto* paper = qobject_cast<app::PaperDialog*>(d);
            if (paper == nullptr) return 0;
            paper->preset->setCurrentIndex(paper->preset->findData(QStringLiteral("b5")));
            return static_cast<int>(QDialog::Accepted);
        };
        dialog.paper->setCurrentIndex(dialog.paper->findData(QStringLiteral("custom")));
        QCOMPARE(spec_json(dialog.chosen_spec()), spec_json(core::PageSpec::b5_doujin()));
        QTemporaryDir where;
        dialog.folder->setText(where.path());
        dialog.title->setText(QStringLiteral("同人誌"));
        QVERIFY(dialog.create());
        QVERIFY(dialog.created.has_value());
        // (as Python's `==`: the book read gives its margins as floats, as Python's reader, 16.0 == 16)
        QVERIFY2(genko::core::py_equals(genko::storage::spec_to_json(read_book(*dialog.created).spec),
                                         genko::storage::spec_to_json(core::PageSpec::b5_doujin())),
                 spec_json(read_book(*dialog.created).spec).c_str());
    }

    // --- tests/test_f2.py -------------------------------------------------------------------------------------------

    // test_every_error_reads_in_japanese: every message Python's modules raise through ApplyError (and the ValueError,
    // WarpError and PSDError that become it), read from Python's own sources as Python's test reads them (each {…}
    // field written 3), reads in Japanese through wording::error — no English word of four letters or more left
    // (names without a Japanese form aside); and the "ops[i] op: " lead is taken off first.
    void test_every_error_reads_in_japanese() {
        const QString src = genko::test::repo_root() + QStringLiteral("/src/genko/");
        QVERIFY2(QFileInfo::exists(src + QStringLiteral("ops.py")), "Python's sources (src/genko) are part of the checkout");
        const QStringList sources = {
            "ops.py",       "layerops.py", "threeops.py", "bookops.py",  "fileops.py",  "plugins.py",   "anim.py",   "animops.py",
            "psd.py",       "colour.py",   "timelapse.py", "export.py",  "rulers.py",   "tones.py",     "effects.py", "nombre.py",
            "models.py",    "mannequin.py", "selection.py", "selops.py", "pagespec.py", "warp.py",      "brushes.py", "frames.py",
            "materials/__init__.py", "lock.py", "journal.py", "app/session.py", "__main__.py"};
        static const QRegularExpression raised(QStringLiteral("raise (?:ApplyError|ValueError|WarpError|PSDError)\\(f?\"([^\"]+)\""));
        static const QRegularExpression field(QStringLiteral(R"(\{[^{}]*(\{[^{}]*\}[^{}]*)*\})"));
        std::set<QString> messages;
        for (const QString& name : sources) {
            QFile file(src + name);
            QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(name));
            const QString text = QString::fromUtf8(file.readAll());
            for (auto it = raised.globalMatch(text); it.hasNext();) {
                QString message = it.next().captured(1);
                message.replace(field, QStringLiteral("3"));
                messages.insert(message);
            }
        }
        QVERIFY2(messages.size() > 120, qPrintable(QString::number(messages.size())));
        static const QRegularExpression japanese(QStringLiteral("[ぁ-んァ-ヶ一-龥]"));
        static const QRegularExpression english(QStringLiteral("[A-Za-z]{4,}"));
        QStringList left;
        for (const QString& message : messages) {
            if (japanese.match(message).hasMatch()) continue;  // (already Japanese)
            QString shown = wording::error(message);
            for (const char* name : {"Genko", "CMYK", "TIFF", "WebP", "ffmpeg"}) shown.remove(QString::fromLatin1(name));  // (no Japanese form)
            if (english.match(shown).hasMatch()) left << message + QStringLiteral(" → ") + wording::error(message);
        }
        QVERIFY2(left.isEmpty(), qPrintable(QStringLiteral("%1 in English:\n").arg(left.size()) + left.join(QStringLiteral("\n"))));
        // through the op's lead too
        QVERIFY(wording::error(QStringLiteral("ops[2] fill: nothing to fill there (the click is on a line)")).startsWith(QStringLiteral("線の上なので塗れません")));
    }

    // test_a_person_alone_inks_before_the_name_is_approved: a book drawn alone takes lines on the ink layer before any
    // name is approved (by the layer's id and by its role); a book made with agents (strict_gates) keeps the order and
    // says why in Japanese: 「AI と進める原稿」.
    void test_a_person_alone_inks_before_the_name_is_approved() {
        core::Document ep = book_of(1);
        const std::string ink = ink_of(ep.page(0))->id;
        APPLY(ep, Json::array({Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", ink}, {"points", pts({{50, 60}, {80, 90}})}},
                               Json{{"op", "add_stroke"}, {"page", 1}, {"layer", "ink"}, {"points", pts({{50, 60}, {80, 90}})}}}));
        QCOMPARE(ink_of(ep.page(0))->stroke_count(), std::size_t{2});
        core::Document studio = book_of(1);
        studio.strict_gates = true;
        QString said;
        bool refused = false;
        try {
            (void)bus().apply(studio, Json::array({Json{{"op", "add_stroke"}, {"page", 1}, {"layer", "ink"}, {"points", pts({{50, 60}, {80, 90}})}}}),
                              core::Actor());
        } catch (const core::ApplyError& error) {
            refused = true;
            said = wording::error(QString::fromUtf8(error.what()));
        }
        QVERIFY(refused);
        QVERIFY2(said.contains(QStringLiteral("AI と進める原稿")), qPrintable(said));
    }

    // test_a_new_book_inks_at_once_and_problems_are_notices: a new book drawn alone inks at once; a fill on a line is a
    // red notice (「線の上なので塗れません」, ⚠ in the status line), not a window; a locked layer says it cannot be
    // drawn on; nothing to paste and no panel chosen are notices too. No window stops the work (nothing is asked).
    void test_a_new_book_inks_at_once_and_problems_are_notices() {
        Book b(book_of(1), QSize(1280, 720));
        MainWindow* w = b.w();
        const std::string ink = ink_of(b.page())->id;
        w->set_target_layer(ink);
        b.trigger("act_pen");
        std::vector<QPointF> line;
        for (int i = 0; i < 20; ++i) line.emplace_back(60 + i, 100);
        inject::mouse_stroke(b.canvas(), line);
        QVERIFY(ink_of(b.page())->stroke_count() > 0);
        // a fill on a line: a red notice, no window
        QVERIFY(w->apply_ops(Json::array({Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", ink}, {"points", pts({{40, 150}, {200, 150}})},
                                               {"width_mm", 3}, {"kind", "mili"}, {"stabilize", 0}, {"taper", false}}})));
        b.trigger("act_fill");
        inject::click_mm(b.canvas(), QPointF(100, 150));
        QVERIFY2(w->last_error().startsWith(QStringLiteral("線の上なので塗れません")), qPrintable(w->last_error()));
        QVERIFY2(w->status_label()->text().contains(QStringLiteral("⚠")), qPrintable(w->status_label()->text()));
        // a locked layer, nothing to paste, no panel chosen
        QVERIFY(w->apply_ops(Json::array({Json{{"op", "set_layer"}, {"page", 1}, {"id", ink}, {"locked", true}}})));
        b.trigger("act_pen");
        inject::mouse_stroke(b.canvas(), {QPointF(60, 200), QPointF(90, 210)});
        QVERIFY2(w->last_notice().contains(QStringLiteral("描けません")), qPrintable(w->last_notice()));
        b.trigger("act_paste");
        QVERIFY(!b.page().selected_frame_id.is_string());  // (no panel chosen in a new book: Python sets it to None)
        b.trigger("act_merge");
        b.trigger("act_border");
        QVERIFY2(w->last_notice().contains(QStringLiteral("コマ")), qPrintable(w->last_notice()));
        QVERIFY2(b.answers->asked.isEmpty(), qPrintable(QStringLiteral("a window stopped the work: ") + b.answers->asked.join(QStringLiteral(" | "))));
    }

    // --- tests/test_f3.py -------------------------------------------------------------------------------------------

    void test_the_page_gets_the_room_data() {
        QTest::addColumn<QSize>("size");
        QTest::addColumn<int>("least");
        QTest::newRow("1280x720") << QSize(1280, 720) << 760;
        QTest::newRow("1024x640") << QSize(1024, 640) << 500;
    }

    // test_the_page_gets_the_room: the page's canvas keeps most of the window (at least 760 px wide at 1280 × 720, 500 px
    // at 1024 × 640), and the window does not grow taller than asked.
    void test_the_page_gets_the_room() {
        QFETCH(QSize, size);
        QFETCH(int, least);
        Book b(book_of(4), size);
        QVERIFY2(b.canvas()->width() >= least, qPrintable(QString::number(b.canvas()->width())));
        QVERIFY2(b.w()->height() <= size.height() + 1, qPrintable(QString::number(b.w()->height())));
    }

    // test_no_panel_is_cut_off: at 1024 × 640 nothing in the panels on the right, nor in ツールの設定 for any tool in
    // hand, reaches past the panel's right edge; and every tab's name is shown whole.
    void test_no_panel_is_cut_off() {
        Book b(book_of(4), QSize(1024, 640));
        MainWindow* w = b.w();
        QStringList docks = studio_docks();
        docks << QStringLiteral("オプション");  // (Python's brush_dock; PC5 H-0: Photoshop's name)
        QStringList cut;
        for (const QString& title : docks) {
            QDockWidget* dock = b.dock(title);
            if (dock == nullptr || !dock->isVisible()) continue;
            dock->raise();
            std::vector<QString> tools = {QString()};
            if (title == QStringLiteral("オプション")) {
                tools.clear();
                for (const auto& entry : w->tool_actions()) tools.push_back(entry.first);
            }
            for (const QString& tool : tools) {
                if (!tool.isEmpty()) w->choose_tool(tool);
                for (int i = 0; i < 2; ++i) QCoreApplication::processEvents();
                QWidget* content = dock->widget();
                auto* scroll = qobject_cast<QScrollArea*>(content);
                QWidget* root = scroll != nullptr ? scroll->viewport() : content;
                const QStringList found = overflow(root);
                if (!found.isEmpty()) cut << QStringLiteral("%1 %2: %3").arg(title, tool, found.mid(0, 4).join(QStringLiteral("; ")));
            }
        }
        QVERIFY2(cut.isEmpty(), qPrintable(cut.join(QStringLiteral("\n"))));
        // tab names are shown whole
        for (QTabBar* bar : w->findChildren<QTabBar*>()) {
            if (!bar->isVisible()) continue;
            for (int i = 0; i < bar->count(); ++i) {
                const QString text = bar->tabText(i);
                QVERIFY2(bar->tabRect(i).width() >= bar->fontMetrics().horizontalAdvance(text), qPrintable(text));
            }
        }
    }

    // test_names_say_what_they_are: two commands of the window with the same words (a panel's own switch aside) are told
    // apart where コマンドを探す shows them — each found there, its row (its words and its place in the menus) its own.
    // (The user's decision 8, SPEC COLOR-04: Photoshop's names, レイヤー → 色調補正 → レベル補正… and the layer panel's
    // 色調補正のレイヤー → レベル補正… alike; Python's rule was no two commands with the same words.) 定規, 3D 操作, 効果線
    // and 素材を置く are in ツール itself; コマ割り and 3D 操作 are named so; the pen has its picture and the tool palette
    // stands upright.
    void test_names_say_what_they_are() {
        Book b(book_of(4), QSize(1280, 720));
        MainWindow* w = b.w();
        std::map<QString, std::set<QAction*>> texts_of;
        for (QAction* action : w->findChildren<QAction*>()) {
            if (!action->text().isEmpty() && !action->isSeparator() && action->menu() == nullptr) texts_of[action->text()].insert(action);
        }
        const std::vector<QAction*> found = genko::app::commands(w);
        QStringList duplicates;
        for (const auto& [text, actions] : texts_of) {
            if (actions.size() < 2 || studio_docks().contains(text)) continue;
            QSet<QString> rows;
            for (QAction* action : actions) {
                const QString place = genko::app::where(w, action);
                const bool offered = std::find(found.begin(), found.end(), action) != found.end();
                if (place.isEmpty() || !offered || rows.contains(place)) duplicates << QStringLiteral("%1（%2）").arg(text, place);
                rows.insert(place);
            }
        }
        QVERIFY2(duplicates.isEmpty(), qPrintable(duplicates.join(QStringLiteral(" | "))));
        QMenu* tools = b.menu(QStringLiteral("ツール"));
        QVERIFY(tools != nullptr);
        const QList<QAction*> in_menu = tools->actions();
        for (const char* name : {"act_ruler", "act_3d", "act_effect", "act_stamp"}) QVERIFY2(in_menu.contains(b.act(name)), name);
        QCOMPARE(b.act("act_frame")->text(), QStringLiteral("コマ割り"));
        QCOMPARE(b.act("act_3d")->text(), QStringLiteral("3D 操作"));
        QVERIFY(!b.act("act_pen")->icon().isNull());
        QCOMPARE(w->tool_palette()->orientation(), Qt::Vertical);
    }

    // test_no_stray_tab_bars_and_the_right_panels_in_front: at 1024 × 640, for a book drawn alone and one made with
    // agents with a question waiting: each row of tabs once, each with two tabs or more; one row on the right with
    // レイヤー in front and 台詞 in it; the approval box stands alone over it (no tab), shown only while something waits.
    void test_no_stray_tab_bars_and_the_right_panels_in_front() {
        for (const bool studio : {false, true}) {
            core::Document doc = book_of(4);
            if (studio) {
                doc.strict_gates = true;
                doc.tickets.push_back(Json{{"id", "t_help"}, {"kind", "help"}, {"status", "open"}, {"text", "背景はどうしますか"}, {"page_index", 1},
                                           {"created_by", "ai:test"}});
            }
            Book b(doc, QSize(1024, 640), studio ? QStringLiteral("s.genko") : QStringLiteral("b.genko"));
            MainWindow* w = b.w();
            for (int i = 0; i < 3; ++i) QCoreApplication::processEvents();
            std::vector<QStringList> shown;
            for (QTabBar* bar : w->findChildren<QTabBar*>()) {
                if (!bar->isVisible() || bar->parentWidget() != w || bar->geometry().right() <= 0) continue;
                QStringList tabs;
                for (int i = 0; i < bar->count(); ++i) tabs << bar->tabText(i);
                shown.push_back(tabs);
            }
            QStringList described;
            QSet<QString> groups;
            for (const QStringList& tabs : shown) {
                described << QStringLiteral("[") + tabs.join(QStringLiteral(", ")) + QStringLiteral("]");
                groups.insert(tabs.join(QStringLiteral("\t")));
            }
            const QByteArray about = (QStringLiteral("studio %1: ").arg(studio ? QStringLiteral("yes") : QStringLiteral("no")) + described.join(QStringLiteral(" "))).toUtf8();
            // each group once, with every panel of the group
            QVERIFY2(static_cast<std::size_t>(groups.size()) == shown.size(), about.constData());
            for (const QStringList& tabs : shown) QVERIFY2(tabs.size() >= 2, about.constData());
            QSet<QString> fronts;
            for (QTabBar* bar : w->findChildren<QTabBar*>()) {
                if (bar->isVisible()) fronts.insert(bar->tabText(bar->currentIndex()));
            }
            // one row of tabs on the right (the layers in front); the approval box stands alone over it, no tab
            QVERIFY2(fronts.contains(QStringLiteral("レイヤー")) && !fronts.contains(QStringLiteral("台詞")) && !fronts.contains(QStringLiteral("承認箱")),
                     about.constData());
            std::vector<QStringList> right;
            for (const QStringList& tabs : shown) {
                if (tabs.contains(QStringLiteral("レイヤー"))) right.push_back(tabs);
            }
            QVERIFY2(right.size() == 1, about.constData());
            QVERIFY2(right[0].contains(QStringLiteral("台詞")) && !right[0].contains(QStringLiteral("承認箱")), about.constData());
            QDockWidget* box = b.dock(QStringLiteral("承認箱"));
            QVERIFY(box != nullptr);
            QCOMPARE(box->isVisible(), studio);
        }
    }

    // --- tests/test_f4.py -------------------------------------------------------------------------------------------

    // test_a_turned_and_mirrored_view_draws_where_the_pen_points: turned 30° and mirrored, the view says so and maps
    // both ways; mirrored, the left of the page is on the right of the screen; a line drawn on a turned, mirrored view
    // lands where the pen pointed, and the page itself never turns; Shift+Space and a drag turn the view.
    void test_a_turned_and_mirrored_view_draws_where_the_pen_points() {
        Book b(book_of(2));
        MainWindow* w = b.w();
        PageCanvas* c = b.canvas();
        w->set_target_layer(ink_of(b.page())->id);
        b.trigger("act_turn_right");
        b.trigger("act_turn_right");
        b.trigger("act_mirror");
        QCOMPARE(c->rotation(), 30.0);
        QVERIFY(c->flipped());
        QVERIFY2(w->zoom_label()->text().contains(QStringLiteral("回転 +30°")) && w->zoom_label()->text().contains(QStringLiteral("左右反転")),
                 qPrintable(w->zoom_label()->text()));
        // the mapping goes both ways
        for (const QPointF mm : {QPointF(10, 20), QPointF(150, 300), QPointF(240, 30)}) {
            const QPointF back = c->to_mm(c->to_widget(mm));
            QVERIFY(std::abs(back.x() - mm.x()) < 1e-6 && std::abs(back.y() - mm.y()) < 1e-6);
        }
        // a mirrored view puts the left of the page on the right of the screen
        const QPointF left = c->to_widget(QPointF(20, 180));
        const QPointF right = c->to_widget(QPointF(220, 180));
        b.trigger("act_turn_reset");
        b.act("act_mirror")->setChecked(true);
        c->flip_view(true);
        QVERIFY(c->to_widget(QPointF(20, 180)).x() > c->to_widget(QPointF(220, 180)).x());
        c->set_rotation(30);
        b.trigger("act_pen");
        std::vector<QPointF> path;
        for (int i = 0; i < 15; ++i) path.emplace_back(80 + i * 3, 100 + i * 2);
        inject::mouse(c, inject::Phase::Press, path.front());
        for (std::size_t i = 1; i < path.size(); ++i) inject::mouse(c, inject::Phase::Move, path[i]);
        inject::mouse(c, inject::Phase::Release, path.back());
        const core::Layer* ink = ink_of(b.page());
        QVERIFY(ink != nullptr && ink->stroke_count() > 0);
        const core::PenPoints drawn = core::stroke_points(*ink->strokes->items.back());
        QVERIFY(!drawn.empty());
        QVERIFY2(std::abs(drawn.front().x - 80) < 1 && std::abs(drawn.front().y - 100) < 1,
                 qPrintable(QStringLiteral("%1, %2").arg(drawn.front().x).arg(drawn.front().y)));
        QVERIFY2(std::abs(drawn.back().x - path.back().x()) < 1.5 && std::abs(drawn.back().y - path.back().y()) < 1.5,
                 qPrintable(QStringLiteral("%1, %2").arg(drawn.back().x).arg(drawn.back().y)));
        // the page itself never turned
        QCOMPARE(b.page().spec.width_mm.value(), core::PageSpec::b4_comic().width_mm.value());
        // Shift+Space drag turns the view
        b.trigger("act_turn_reset");
        QCOMPARE(c->rotation(), 0.0);
        QVERIFY(!c->flipped());
        QVERIFY(!b.act("act_mirror")->isChecked());
        QKeyEvent space(QEvent::KeyPress, Qt::Key_Space, Qt::ShiftModifier);
        QCoreApplication::sendEvent(c, &space);
        const QPointF centre(c->width() / 2.0, c->height() / 2.0);
        const QPointF start = centre + QPointF(200, 0);
        const QPointF end = centre + QPointF(0, 200);
        QMouseEvent press(QEvent::MouseButtonPress, start, c->mapToGlobal(start), Qt::LeftButton, Qt::LeftButton, Qt::ShiftModifier);
        QCoreApplication::sendEvent(c, &press);
        QMouseEvent move(QEvent::MouseMove, end, c->mapToGlobal(end), Qt::NoButton, Qt::LeftButton, Qt::ShiftModifier);
        QCoreApplication::sendEvent(c, &move);
        QMouseEvent release(QEvent::MouseButtonRelease, end, c->mapToGlobal(end), Qt::LeftButton, Qt::NoButton, Qt::ShiftModifier);
        QCoreApplication::sendEvent(c, &release);
        QKeyEvent let_go(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
        QCoreApplication::sendEvent(c, &let_go);
        QVERIFY2(std::abs(c->rotation() - 90) < 0.5, qPrintable(QString::number(c->rotation())));
        QVERIFY(left != right);
    }

    // --- tests/test_f5.py -------------------------------------------------------------------------------------------

    // test_the_balloon_pen_and_the_turn_handle: a balloon drawn by hand with the text tool (its words typed with 傍点
    // marks), turned with the ○ above it; the lines panel shows the turn and sets a slant (傾き) from its field.
    void test_the_balloon_pen_and_the_turn_handle() {
        Book b(book_of(1));
        MainWindow* w = b.w();
        PageCanvas* c = b.canvas();
        c->fit_page();
        b.trigger("act_text");
        w->text_settings()->draw_balloon->setChecked(true);
        QVERIFY(c->balloon_pen);
        std::vector<QPointF> ring;
        for (int t = 0; t < 11; ++t) {
            const double a = t / 10.0 * 2 * std::numbers::pi;
            ring.emplace_back(100 + 25 * std::cos(a), 150 + 18 * std::sin(a));
        }
        inject::mouse_stroke(c, ring);
        app::InlineEditor* editor = c->editor();
        QVERIFY(editor != nullptr);
        editor->setPlainText(QStringLiteral("《《描いた》》形"));
        editor->finish(true);
        const auto lines = w->book().story_for_page(core::Num(1));
        QVERIFY(!lines.empty());
        const core::StoryLine line = *lines.back();
        QVERIFY(line.path.has_value() && !line.path->empty());
        QCOMPARE(line.text, std::string("描いた形"));
        QCOMPARE(line.emphasis_runs, std::vector<std::string>{"描いた"});
        QVERIFY2(std::abs(line.x_mm.value() - 75) < 1 && std::abs(line.w_mm.value() - 50) < 1,
                 qPrintable(QStringLiteral("x %1, w %2").arg(line.x_mm.value()).arg(line.w_mm.value())));
        // turn it with the handle above it
        w->text_settings()->draw_balloon->setChecked(false);
        b.trigger("act_select");
        c->selected_line_id = line.id;
        const double cx = line.x_mm.value() + line.w_mm.value() / 2;
        const double top = line.y_mm.value() - 7;
        const QPointF to_the_right(line.x_mm.value() + line.w_mm.value() + 20, line.y_mm.value() + line.h_mm.value() / 2);
        inject::mouse_stroke(c, {QPointF(cx, top), QPointF(cx + 10, top + 2), to_the_right});
        const core::StoryLine* turned = w->line_by_id(line.id);
        QVERIFY(turned != nullptr);
        const double rotate = number_at(turned->style, "rotate_deg", 0.0);
        QVERIFY2(std::abs(rotate - 90) < 3, qPrintable(QString::number(rotate)));
        // the lines panel shows and sets it
        app::StoryPanel* story = w->story();
        story->refresh();
        story->select(line.id);
        QVERIFY2(std::abs(story->rotate->value() - rotate) < 0.6, qPrintable(QString::number(story->rotate->value())));
        story->skew->setValue(10);
        emit story->skew->editingFinished();
        QVERIFY(w->line_by_id(line.id) != nullptr);
        QCOMPARE(number_at(w->line_by_id(line.id)->style, "skew_deg", 0.0), 10.0);
    }

    // --- tests/test_g1.py -------------------------------------------------------------------------------------------

    // test_a_finished_page_stays_quick_to_draw_on: a page of 1500 lines drawn once (its lines remembered), then one line
    // more: drawn on the remembered lines, the page is the page drawn from nothing; a line taken away by the eraser
    // draws the layer again, correctly. (Python's bound of 0.5 s for the drawing belongs to the perf tests.)
    void test_a_finished_page_stays_quick_to_draw_on() {
        std::mt19937 rng(3);
        const auto uniform = [&rng](double lo, double hi) { return std::uniform_real_distribution<double>(lo, hi)(rng); };
        core::Document ep = book_of(1);
        const std::string ink = ink_of(ep.page(0))->id;
        Json ops = Json::array();
        for (int n = 0; n < 1500; ++n) {
            const double x = uniform(20, 230);
            const double y = uniform(20, 340);
            Json points = Json::array();
            for (int i = 0; i < 20; ++i) points.push_back(Json::array({x + i * 1.2, y + uniform(-2, 2), uniform(0.3, 1)}));
            ops.push_back(Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", ink}, {"stabilize", 0}, {"points", points}});
        }
        APPLY(ep, ops);
        const render::RenderOptions proof = render::proof_options();
        (void)render::render_page(ep.page(0), 110, proof, &ep);
        APPLY(ep, Json::array({Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", ink}, {"points", pts({{30, 30}, {200, 300}})}, {"stabilize", 0}}}));
        const std::string quick = render::render_page(ep.page(0), 110, proof, &ep).image.tobytes();
        render::clear_render_caches();
        QVERIFY(quick == render::render_page(ep.page(0), 110, proof, &ep).image.tobytes());
        // a line taken away (the eraser) draws the layer again, correctly
        APPLY(ep, Json::array({Json{{"op", "erase"}, {"page", 1}, {"layer_id", ink}, {"points", pts({{20, 20}, {240, 350}})}, {"width_mm", 4}}}));
        const std::string erased = render::render_page(ep.page(0), 110, proof, &ep).image.tobytes();
        render::clear_render_caches();
        QVERIFY(erased == render::render_page(ep.page(0), 110, proof, &ep).image.tobytes());
    }

    // test_a_new_books_first_lines_print: a new book's window starts on the ink layer (no 「印刷されません」), its first
    // lines are in the exported PNG; choosing the name layer says it does not print.
    void test_a_new_books_first_lines_print() {
        Book b(book_of(1));
        MainWindow* w = b.w();
        QVERIFY(w->target_layer() != nullptr);
        QCOMPARE(w->target_layer()->role, core::LayerRole::Ink);
        QVERIFY(!w->layer_panel()->target_row()->text().contains(QStringLiteral("印刷されません")));
        b.trigger("act_pen");
        std::vector<QPointF> path;
        for (int i = 0; i < 30; ++i) path.emplace_back(60 + i * 5, 150);
        inject::mouse_stroke(b.canvas(), path);
        w->session().save_now();
        QVERIFY(w->session().wait_saved(30000ms));
        genko::formats::RunOptions options;
        options.dpi = 60;
        const Json result = genko::formats::run(w->book(), w->session().path(), "png", path_of(b.dir.filePath(QStringLiteral("out"))), options);
        QVERIFY2(result.value("ok", false) && result.contains("files") && result["files"].is_array() && !result["files"].empty(), result.dump().c_str());
        QImage image(QString::fromStdString(result["files"][0].get<std::string>()));
        QVERIFY(!image.isNull());
        image = image.convertToFormat(QImage::Format_Grayscale8);
        int darkest = 255;
        for (int x = image.width() / 3; x < image.width() / 2; ++x) {
            for (int y = image.height() / 3; y < image.height() / 2; ++y) darkest = std::min(darkest, qGray(image.pixel(x, y)));
        }
        QVERIFY2(darkest < 100, qPrintable(QString::number(darkest)));
        // choosing the name layer says it does not print
        const core::Layer* name = first_layer(b.page(), core::LayerRole::Name);
        QVERIFY(name != nullptr);
        w->set_target_layer(name->id);
        w->layer_panel()->refresh();
        QVERIFY2(w->layer_panel()->target_row()->text().contains(QStringLiteral("印刷されません")), qPrintable(w->layer_panel()->target_row()->text()));
    }

    // --- tests/test_g2.py -------------------------------------------------------------------------------------------

    // test_the_chosen_lines_settings_sit_beside_the_tool: the chosen line's lettering beside the select tool (its
    // words in the title, the size set from there); the lines panel shows its list and words without scrolling; the
    // materials in three pages.
    void test_the_chosen_lines_settings_sit_beside_the_tool() {
        Book b(g2_book(), QSize(1280, 720));
        MainWindow* w = b.w();
        b.trigger("act_select");
        b.canvas()->selected_line_id = std::string("a");
        w->on_line_selected("a", false);
        QCoreApplication::processEvents();
        app::StoryPanel* story = w->story();
        QWidget* box = story->style_box;
        QWidget* page = w->tool_settings()->current_page();
        QVERIFY(page != nullptr && page->isAncestorOf(box) && box->isVisible());
        QVERIFY2(story->style_title->text().contains(QStringLiteral("今日こそ")), qPrintable(story->style_title->text()));
        QVERIFY(story->font->isEnabled());
        story->size->setValue(7);
        emit story->size->editingFinished();
        QVERIFY(w->line_by_id("a") != nullptr);
        QCOMPARE(number_at(w->line_by_id("a")->style, "size_mm", 0.0), 7.0);
        // the lines panel shows its list and words without scrolling
        w->show_dock(QStringLiteral("台詞"));
        QCoreApplication::processEvents();
        QDockWidget* lines = b.dock(QStringLiteral("台詞"));
        QVERIFY(lines != nullptr);
        auto* scroll = qobject_cast<QScrollArea*>(lines->widget());
        QVERIFY(scroll != nullptr && scroll->widget() != nullptr);
        QVERIFY2(scroll->widget()->minimumSizeHint().height() <= scroll->viewport()->height(),
                 qPrintable(QStringLiteral("%1 > %2").arg(scroll->widget()->minimumSizeHint().height()).arg(scroll->viewport()->height())));
        // the materials in three pages
        QTabWidget* tabs = w->materials()->tabs;
        QVERIFY(tabs != nullptr && tabs->count() >= 3);
        QCOMPARE((QStringList{tabs->tabText(0), tabs->tabText(1), tabs->tabText(2)}),
                 (QStringList{QStringLiteral("素材"), QStringLiteral("トーン"), QStringLiteral("効果線")}));
    }

    // test_layers_are_compact_and_empty_ones_have_no_picture: the layer list's pictures are 30 to 40 px tall, in rows
    // of at most 48 px; with nothing drawn, every row shows its kind's quiet mark, not a picture of the layer.
    void test_layers_are_compact_and_empty_ones_have_no_picture() {
        Book b(g2_book(), QSize(1280, 720));
        app::LayerPanel* panel = b.w()->layer_panel();
        panel->refresh();
        QListWidget* list = panel->list();
        QVERIFY2(list->iconSize().height() >= 30 && list->iconSize().height() <= 40, qPrintable(QString::number(list->iconSize().height())));
        QVERIFY(list->count() > 0);
        QVERIFY2(list->sizeHintForRow(0) <= 48, qPrintable(QString::number(list->sizeHintForRow(0))));
        for (int i = 0; i < list->count(); ++i) QCOMPARE(list->item(i)->data(Qt::UserRole + 1).toString(), QStringLiteral("mark"));
    }

    // test_no_agent_stage_in_a_book_drawn_alone: no ネーム完了 and no stage in a book drawn alone; the status line counts
    // the panels: 「コマ 1 個」.
    void test_no_agent_stage_in_a_book_drawn_alone() {
        Book b(g2_book(), QSize(1280, 720));
        QVERIFY(!b.act("act_name_ok")->isVisible());
        QVERIFY2(b.w()->status_label()->text().contains(QStringLiteral("コマ 1 個")), qPrintable(b.w()->status_label()->text()));
    }

    // test_the_line_while_drawing_is_steadied_like_the_finished_one: with steadying on (7), the line shown while the
    // pen moves (a shaky hand) covers nearly what the finished line covers (more than 90 % of the inked pixels shared).
    void test_the_line_while_drawing_is_steadied_like_the_finished_one() {
        Book b(g2_book(), QSize(1280, 720));
        MainWindow* w = b.w();
        PageCanvas* c = b.canvas();
        b.trigger("act_pen");
        app::PenSettings pen = app::PenSettings::for_kind("gpen");  // (the pen's settings said: gpen, no tapered ends, steadied)
        pen.taper = false;
        pen.stabilize = 7;
        w->pen() = pen;
        w->pen_changed();
        c->actual_size();  // (プリントサイズ: the paper at 96 dpi, the 100 % before PC5 H-a — the user's decision 11)
        c->center_on(97, 150);
        std::vector<QPointF> path;
        for (int i = 0; i < 50; ++i) path.emplace_back(60 + i * 1.5, 150 + 6 * std::sin(i * 1.3));  // (a shaky hand)
        inject::mouse(c, inject::Phase::Press, path.front());
        for (std::size_t i = 1; i < path.size(); ++i) {
            inject::mouse(c, inject::Phase::Move, path[i]);
            QCoreApplication::processEvents();
        }
        const app::LiveInk* live = c->live();
        QVERIFY(live != nullptr);
        const render::Image shown = live->patch();
        const QRect box = live->box();
        const int dpi = live->dpi();
        inject::mouse(c, inject::Phase::Release, path.back());
        const core::Layer* ink = ink_of(b.page());
        QVERIFY(ink != nullptr && ink->stroke_count() == 1);
        const render::Image layer = render::layer_image(b.page(), *ink, dpi, &w->book());
        const render::Image finished = layer.crop(render::Box{box.x(), box.y(), box.x() + box.width(), box.y() + box.height()});
        const double shared = overlap(shown, finished);
        QVERIFY2(shared > 0.9, qPrintable(QString::number(shared)));
    }

    // --- tests/test_g3.py -------------------------------------------------------------------------------------------

    // test_the_menus_people_look_for: the nine menus in Python's order (the rulers, 3D, tones and effect lines inside
    // ツール; the panels and the lines inside ページ), the commands people look for in them, and 最近使用したファイルを開く (PC4
    // F-0: Photoshop's name for Python's 最近使った原稿) listing the book.
    void test_the_menus_people_look_for() {
        core::Document doc = book_of(2);
        APPLY(doc, Json::array({Json{{"op", "add_line"}, {"page", 1}, {"id", "a"}, {"text", "やあ"}, {"x_mm", 150}, {"y_mm", 40}, {"w_mm", 30},
                                     {"h_mm", 30}, {"wrap", "vertical"}}}));
        Book b(doc, QSize(1280, 800), QStringLiteral("b.genko"), [](const fs::path& path) { app::remember_project(path); });
        QStringList titles;
        for (QAction* top : b.w()->menuBar()->actions()) titles << top->text();
        // (PC4 節 D, the user's decision 1: フィルター after レイヤー; PC5 G-0: Photoshop's 書式 between them, design section G 5.1)
        QCOMPARE(titles, (QStringList{QStringLiteral("ファイル"), QStringLiteral("編集"), QStringLiteral("表示"), QStringLiteral("ツール"), QStringLiteral("選択範囲"),
                                      QStringLiteral("レイヤー"), QStringLiteral("書式"), QStringLiteral("フィルター"), QStringLiteral("ページ"), QStringLiteral("ウィンドウ"),
                                      QStringLiteral("ヘルプ")}));
        const QStringList file = texts(b.menu(QStringLiteral("ファイル")));
        QVERIFY(file.contains(QStringLiteral("最近使用したファイルを開く")) && file.contains(QStringLiteral("閉じる")) && file.contains(QStringLiteral("終了")));
        const QStringList edit = texts(b.menu(QStringLiteral("編集")));
        QVERIFY(edit.contains(QStringLiteral("すべてを選択")) && edit.contains(QStringLiteral("消去")));
        QVERIFY(texts(b.menu(QStringLiteral("レイヤー"))).contains(QStringLiteral("レイヤーマスク")));
        // (PC5 G-0: 書式 with the type commands this build has, as Photoshop names them; G-a: アンチエイリアス ▸, 方向 ▸, the
        // conversions, ラスタライズ, 更新, Lorem ipsum and a line and a type layer each other's)
        QCOMPARE(texts(b.menu(QStringLiteral("書式"))),
                 (QStringList{QStringLiteral("アンチエイリアス"), QStringLiteral("方向"), QString(), QStringLiteral("作業用パスを作成"), QStringLiteral("シェイプに変換"),
                              QStringLiteral("テキストレイヤーをラスタライズ"), QStringLiteral("ポイントテキストに変換"), QStringLiteral("段落テキストに変換"), QString(),
                              QStringLiteral("ワープテキスト…"), QString(), QStringLiteral("すべてのテキストレイヤーを更新"), QString(),
                              QStringLiteral("Lorem Ipsum をペースト"), QString(), QStringLiteral("台詞をテキストレイヤーに"), QStringLiteral("テキストレイヤーを台詞に")}));
        QVERIFY(texts(submenu(b.menu(QStringLiteral("ページ")), QStringLiteral("台詞"))).contains(QStringLiteral("フキダシの形")));
        QMenu* tools = b.menu(QStringLiteral("ツール"));
        QVERIFY(texts(submenu(tools, QStringLiteral("定規"))).contains(QStringLiteral("直線定規")));
        QVERIFY(texts(submenu(tools, QStringLiteral("3D"))).contains(QStringLiteral("デッサン人形を置く")));
        QVERIFY(texts(submenu(tools, QStringLiteral("3D"))).contains(QStringLiteral("ポーズ")));
        QVERIFY(texts(submenu(tools, QStringLiteral("トーン・効果線"))).contains(QStringLiteral("集中線")));
        QVERIFY(texts(submenu(b.menu(QStringLiteral("ページ")), QStringLiteral("コマ"))).contains(b.act("act_split_h")->text()));
        // recent books
        QMenu* recent = submenu(b.menu(QStringLiteral("ファイル")), QStringLiteral("最近使用したファイルを開く"));
        QVERIFY(recent != nullptr);
        emit recent->aboutToShow();
        QVERIFY2(texts(recent).contains(QStringLiteral("b")), qPrintable(texts(recent).join(QStringLiteral(" | "))));
    }

    // --- tests/test_g4.py -------------------------------------------------------------------------------------------

    // test_part_of_a_line_larger_bolder_and_coloured: parse_marks reads {大|…} as larger (scale 1.4) and {赤、太|…} as
    // red and bold (rgb, bold: true). (The styled parts drawn and the refusal of scale 9: the text and lines contracts.)
    void test_part_of_a_line_larger_bolder_and_coloured() {
        const auto marks = app::lettering::parse_marks(QStringLiteral("それは{大|本当}か{赤、太|！}"));
        QCOMPARE(marks.text, std::string("それは本当か！"));
        const Json& styles = marks.style_runs;
        QVERIFY2(styles.is_array() && styles.size() == 2, styles.dump().c_str());
        QCOMPARE(styles[0][0].dump(), Json("本当").dump());
        QCOMPARE(styles[0][1].dump(), (Json{{"scale", 1.4}}).dump());
        QCOMPARE(styles[1][0].dump(), Json("！").dump());
        QVERIFY2(styles[1][1].is_object() && styles[1][1].size() == 2, styles.dump().c_str());
        QCOMPARE(styles[1][1].value("rgb", Json()).dump(), Json::array({210, 30, 30}).dump());
        QCOMPARE(styles[1][1].value("bold", Json()).dump(), Json(true).dump());
    }

    // --- tests/test_g5.py -------------------------------------------------------------------------------------------

    // test_the_layer_move_tool_moves_everything_on_the_layer: 移動ツール (its settings named so) moves the layer's lines
    // and filled patches by the drag's offset.
    void test_the_layer_move_tool_moves_everything_on_the_layer() {
        Book b(g5_book());
        MainWindow* w = b.w();
        w->set_target_layer(ink_of(b.page())->id);
        b.trigger("act_move");
        QCOMPARE(b.canvas()->tool(), QStringLiteral("move"));
        QVERIFY2(w->tool_settings()->title()->text().startsWith(QStringLiteral("移動ツール")), qPrintable(w->tool_settings()->title()->text()));
        inject::mouse_stroke(b.canvas(), {QPointF(100, 150), QPointF(110, 155), QPointF(120, 160)});
        const core::Layer* ink = ink_of(b.page());
        QVERIFY(ink != nullptr && ink->stroke_count() > 0);
        const core::PenPoints points = core::stroke_points(*ink->strokes->items.at(0));
        QVERIFY2(std::abs(points.front().x - 80) < 0.1 && std::abs(points.front().y - 110) < 0.1,
                 qPrintable(QStringLiteral("%1, %2").arg(points.front().x).arg(points.front().y)));
        QVERIFY(!ink->patches.empty());
        const Json box = ink->patches[0].attrs.value("box", Json());
        QVERIFY2(box.is_array() && box.size() >= 2 && std::abs(box[0].get<double>() - 170) < 1 && std::abs(box[1].get<double>() - 210) < 1, box.dump().c_str());
    }

    // test_the_navigator_and_the_page_overview: a click in 全体図 centres the view there (the place in the part seen);
    // ページを並べて見る shows every page with its picture and opens the one chosen.
    void test_the_navigator_and_the_page_overview() {
        Book b(g5_book());
        MainWindow* w = b.w();
        PageCanvas* c = b.canvas();
        c->zoom_by(3.0);
        QDockWidget* navigator_dock = b.dock(QStringLiteral("ナビゲーター"));  // (PC5 H-0: Photoshop's name)
        QVERIFY(navigator_dock != nullptr && navigator_dock->isVisible());
        app::Navigator* nav = w->navigator();
        const QRectF rect = nav->page_rect();
        QVERIFY(!rect.isEmpty());
        const QPointF target(rect.x() + rect.width() * 0.8, rect.y() + rect.height() * 0.2);
        QMouseEvent press(QEvent::MouseButtonPress, target, nav->mapToGlobal(target), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(nav, &press);
        const QPointF at = c->to_mm(QPointF(c->width() / 2.0, c->height() / 2.0));
        QVERIFY2(std::abs(at.x() - 257 * 0.8) < 2 && std::abs(at.y() - 364 * 0.2) < 2, qPrintable(QStringLiteral("%1, %2").arg(at.x()).arg(at.y())));
        const QRectF seen = c->seen_mm();
        QVERIFY(seen.left() < at.x() && at.x() < seen.right() && seen.top() < at.y() && at.y() < seen.bottom());
        // all pages at a glance
        b.trigger("act_overview");
        app::PageOverview* overview = nullptr;
        for (QWidget* top : QApplication::topLevelWidgets()) {
            if (auto* o = qobject_cast<app::PageOverview*>(top); o != nullptr && o->isVisible()) overview = o;
        }
        QVERIFY(overview != nullptr);
        QSignalSpy pictures(&overview->maker(), &app::ThumbMaker::done);
        QCOMPARE(overview->list()->count(), 4);
        QVERIFY(wait_for([&] { return pictures.count() >= 4; }, 60000));
        QVERIFY(!overview->list()->item(0)->icon().isNull());
        emit overview->list()->itemDoubleClicked(overview->list()->item(2));
        QVERIFY(wait_for([&] { return w->current_page() != nullptr && w->current_page()->index == core::Num(3); }, 10000));
    }

    // --- tests/test_h2.py -------------------------------------------------------------------------------------------

    // test_tools_can_show_their_names_and_it_is_remembered: 道具の名前を表示 (in a menu) puts the names beside the
    // pictures and is kept in the settings; the marquee is 長方形選択 in the palette, 長方形選択ツール in the menus.
    void test_tools_can_show_their_names_and_it_is_remembered() {
        app::settings()->remove(QStringLiteral("ui/tool_names"));
        Book b(h2_book(), QSize(1280, 720));
        MainWindow* w = b.w();
        QCOMPARE(w->tool_palette()->toolButtonStyle(), Qt::ToolButtonIconOnly);
        b.trigger("act_tool_names");
        QCOMPARE(w->tool_palette()->toolButtonStyle(), Qt::ToolButtonTextBesideIcon);
        QCOMPARE(app::settings()->value(QStringLiteral("ui/tool_names")).toString(), QStringLiteral("1"));
        QCOMPARE(b.act("act_marquee")->iconText(), QStringLiteral("長方形選択"));
        QCOMPARE(b.act("act_marquee")->text(), QStringLiteral("長方形選択ツール"));
        bool in_menu = false;
        for (QAction* top : w->menuBar()->actions()) {
            if (top->menu() != nullptr && top->menu()->actions().contains(b.act("act_tool_names"))) in_menu = true;
        }
        QVERIFY(in_menu);
        b.trigger("act_tool_names");
        QCOMPARE(app::settings()->value(QStringLiteral("ui/tool_names")).toString(), QStringLiteral("0"));
    }

    // test_fill_lasso_fill_and_picker_icons_look_different: 塗りつぶし, 囲って塗る, スポイト and グラデーション have pictures
    // of their own.
    void test_fill_lasso_fill_and_picker_icons_look_different() {
        QSet<QByteArray> shots;
        for (const char* name : {"fill", "lassofill", "picker", "gradient"}) {
            const QImage image = app::icons::icon(name).pixmap(32, 32).toImage();
            QVERIFY2(!image.isNull(), name);
            shots.insert(QByteArray(reinterpret_cast<const char*>(image.constBits()), static_cast<qsizetype>(image.sizeInBytes())));
        }
        QCOMPARE(shots.size(), 4);
    }

    // test_the_help_knows_todays_tools: the guide and the answers (as the person reads them, the menus' words filled in)
    // speak of {大|…}, 移動ツール, グラデーション, ナビゲーター (PC5 H-0: 全体図 before), 道具の名前を表示, 印刷 and 電子音.
    void test_the_help_knows_todays_tools() {
        Book b(h2_book(), QSize(1280, 720));
        const QString help = app::help::guide(b.w()) + app::help::faq(b.w());
        for (const QString& words : {QStringLiteral("{大|"), QStringLiteral("移動ツール"), QStringLiteral("グラデーション"), QStringLiteral("ナビゲーター"),
                                     QStringLiteral("道具の名前を表示"), QStringLiteral("印刷"), QStringLiteral("電子音")}) {
            QVERIFY2(help.contains(words), qPrintable(words));
        }
    }

    // test_an_electric_balloon_is_jagged_and_its_tail_zigzags: the electric balloon is offered as 電子音…. (Its shape,
    // tail and drawing: the balloons contract.)
    void test_an_electric_balloon_is_jagged_and_its_tail_zigzags() {
        bool offered = false;
        for (const auto& kind : app::lettering::kinds()) offered = offered || kind.first == QStringLiteral("electric");
        QVERIFY(offered);
        QVERIFY2(app::lettering::kind_label("electric").startsWith(QStringLiteral("電子音")), qPrintable(app::lettering::kind_label("electric")));
    }

    // test_weights_go_normal_bold_heavy: a part of a line made heavy is typed {極太|…} (bold 2). (The weights drawn and
    // the refusal of an unknown weight: the text and lines contracts.)
    void test_weights_go_normal_bold_heavy() {
        const auto marks = app::lettering::parse_marks(QStringLiteral("{極太|ドーン}と"));
        QCOMPARE(marks.text, std::string("ドーンと"));
        QCOMPARE(marks.style_runs.dump(), Json::parse(R"([["ドーン", {"bold": 2}]])").dump());
    }

    // --- tests/test_h4.py -------------------------------------------------------------------------------------------

    // test_the_layer_panel_marks_reference_layers: 参照にする marks the layer 参照 in the list, and the brush panel's fill
    // offers to look at the reference layers only.
    void test_the_layer_panel_marks_reference_layers() {
        core::Document doc = book_of(1);
        const std::string ink = ink_of(doc.page(0))->id;
        APPLY(doc, Json::array({Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", ink}, {"points", pts({{60, 60}, {120, 60}, {120, 120}, {60, 120}, {60, 60}})},
                                     {"width_mm", 1}, {"stabilize", 0}},
                                Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"name", "色"}, {"id", "colour"}}}));
        Book b(doc);
        MainWindow* w = b.w();
        w->set_target_layer(ink);
        w->layer_panel()->refresh();
        auto* reference = w->layer_panel()->findChild<QCheckBox*>(QStringLiteral("layer_reference"));
        QVERIFY(reference != nullptr);
        reference->click();
        QVERIFY(layer_by_id(b.page(), ink) != nullptr && layer_by_id(b.page(), ink)->reference);
        w->layer_panel()->refresh();
        const std::vector<std::string>& ids = w->layer_panel()->ids();
        const auto row = std::find(ids.begin(), ids.end(), ink);
        QVERIFY(row != ids.end());
        const QString text = w->layer_panel()->list()->item(static_cast<int>(row - ids.begin()))->text();
        QVERIFY2(text.contains(QStringLiteral("参照")), qPrintable(text));
        QVERIFY(w->brush_panel()->reference->findData(QStringLiteral("reference")) >= 0);
    }

    // --- tests/test_i0.py -------------------------------------------------------------------------------------------

    // test_the_3d_page_is_short: the 3D tool's settings are a short page (six buttons at most), what is put on the page
    // in one 置く button's menu (背景: 廊下 and 3D の箱を置く among them).
    void test_the_3d_page_is_short() {
        core::Document doc = book_of(4);
        APPLY(doc, Json::array({Json{{"op", "set_spread"}, {"page", 2}, {"with", 3}}}));
        Book b(doc);
        MainWindow* w = b.w();
        w->choose_tool(QStringLiteral("3d"));
        QCoreApplication::processEvents();
        QWidget* page = w->tool_settings()->current_page();
        QVERIFY(page != nullptr);
        QList<QPushButton*> buttons;
        for (QPushButton* button : page->findChildren<QPushButton*>()) {
            if (button->isVisible()) buttons << button;
        }
        QVERIFY2(buttons.size() <= 6, qPrintable(QString::number(buttons.size())));
        QPushButton* place = nullptr;
        for (QPushButton* button : buttons) {
            if (button->text().startsWith(QStringLiteral("置く")) && button->menu() != nullptr) place = button;
        }
        QVERIFY(place != nullptr);
        const QStringList placed = texts(place->menu());
        QVERIFY2(placed.contains(QStringLiteral("背景: 廊下")) && placed.contains(QStringLiteral("3D の箱を置く")), qPrintable(placed.join(QStringLiteral(" | "))));
    }

    // --- tests/test_j1.py -------------------------------------------------------------------------------------------

    // test_two_fingers_zoom_turn_and_move: two fingers spread to zoom, twist to turn the view, slide to move it.
    void test_two_fingers_zoom_turn_and_move() {
        Book b(book_of(2));
        PageCanvas* c = b.canvas();
        const double scale = c->scale();
        const double turn = c->rotation();
        const double pan = c->pan().x();
        c->pinch(1.5, 0, QPointF(300, 300), QPointF(0, 0));
        QVERIFY(c->scale() > scale);
        c->pinch(1.0, 20, QPointF(300, 300), QPointF(0, 0));
        QVERIFY2(std::abs(c->rotation() - turn - 20) < 1e-6, qPrintable(QString::number(c->rotation())));
        c->pinch(1.0, 0, QPointF(300, 300), QPointF(40, 0));
        QVERIFY(c->pan().x() != pan);
    }

    // --- tests/test_hd_report.py ------------------------------------------------------------------------------------

    // test_a_tool_chosen_while_a_key_holds_another_stays: a tool chosen while a held key has another in hand (Shift
    // held: the eraser for now, the pen when it is let go) is the tool from then on; the hold is dropped.
    void test_a_tool_chosen_while_a_key_holds_another_stays() {
        app::PageCanvas canvas;
        canvas.tool_modifiers[QStringLiteral("pen")][QStringLiteral("shift")] = QStringLiteral("eraser");
        canvas.set_tool(QStringLiteral("pen"));
        canvas.hold_modifier(QStringLiteral("shift"), true);
        QCOMPARE(canvas.tool(), QStringLiteral("eraser"));
        QVERIFY(canvas.tool_held());
        canvas.set_tool(QStringLiteral("vector"));
        QCOMPARE(canvas.tool(), QStringLiteral("vector"));
        QVERIFY(!canvas.tool_held());
        canvas.hold_modifier(QStringLiteral("shift"), false);  // (letting go keeps it)
        QCOMPARE(canvas.tool(), QStringLiteral("vector"));
    }

    // test_the_effect_list_follows_the_page_and_the_tone_shift_is_kept_without_enter: the materials panel lists the
    // effect lines of the page shown; a tone's shift typed without Enter is kept a moment later.
    void test_the_effect_list_follows_the_page_and_the_tone_shift_is_kept_without_enter() {
        core::Document doc = book_of(2, core::PageSpec::b5_doujin());
        const std::string frame = doc.page(0).leaf_frames().at(0)->id;
        APPLY(doc, Json::array({Json{{"op", "add_effect"}, {"page", 2}, {"kind", "speed"}, {"params", Json::object({{"angle", 0}, {"count", 10}})}},
                                Json{{"op", "add_tone"}, {"page", 1}, {"frame_id", frame}, {"density", 0.3}}}));
        Book b(doc, QSize(1280, 800), QStringLiteral("hd.genko"));
        MainWindow* w = b.w();
        app::MaterialPanel* materials = w->materials();
        w->select_page(1);
        QCoreApplication::processEvents();
        materials->refresh();
        QCOMPARE(materials->effects->count(), 1);
        w->select_page(0);
        materials->refresh();
        QCOMPARE(materials->effects->count(), 0);
        std::string tone;
        for (const core::Layer& layer : b.page().layers) {
            if (layer.kind == core::LayerKind::Tone) tone = layer.id;
        }
        QVERIFY(!tone.empty());
        w->set_target_layer(tone);
        materials->off_y->setValue(0.3);  // (typed, no Enter)
        const auto shift = [&]() -> QString {
            const core::Layer* layer = layer_by_id(*w->current_page(), tone);
            if (layer == nullptr) return QStringLiteral("(no tone layer)");
            const auto offset = render::tones::settings(*layer).offset_mm;
            if (!offset || !offset->is_array() || offset->size() < 2 || !(*offset)[0].is_number() || !(*offset)[1].is_number()) return QStringLiteral("(none)");
            return QStringLiteral("%1, %2").arg(core::py_round((*offset)[0].get<double>(), 2)).arg(core::py_round((*offset)[1].get<double>(), 2));
        };
        QTRY_COMPARE_WITH_TIMEOUT(shift(), QStringLiteral("0, 0.3"), 5000);  // (the moment after typing)
    }

    // test_adding_a_cover_keeps_the_other_pictures_in_the_page_list: a cover added, the page list keeps the pictures it
    // has of the other pages (the same pictures, not drawn again).
    void test_adding_a_cover_keeps_the_other_pictures_in_the_page_list() {
        Book b(book_of(3, core::PageSpec::b5_doujin()), QSize(1280, 800), QStringLiteral("hd.genko"));
        MainWindow* w = b.w();
        w->show_dock(QStringLiteral("ページ"));
        QCoreApplication::processEvents();
        app::PageList* list = w->pages();
        QVERIFY(list->wait_pictures(60000));
        std::map<QString, qint64> kept;
        for (int row = 0; row < list->count(); ++row) {
            if (list->has_picture(row)) kept[list->item(row)->data(Qt::UserRole + 1).toString()] = list->item(row)->icon().cacheKey();
        }
        QVERIFY(!kept.empty());
        QVERIFY(w->apply_ops(Json::array({Json{{"op", "add_cover"}, {"kind", "front"}}})));
        std::size_t still = 0;
        for (int row = 0; row < list->count(); ++row) {
            const auto found = kept.find(list->item(row)->data(Qt::UserRole + 1).toString());
            if (found == kept.end()) continue;
            QVERIFY2(list->has_picture(row), qPrintable(found->first));
            QCOMPARE(list->item(row)->icon().cacheKey(), found->second);
            ++still;
        }
        QCOMPARE(still, kept.size());
    }

private:
    // test_g2.py's book: two B4 pages, a line 「今日こそ言わなきゃ」 (id a) on page 1.
    static core::Document g2_book() {
        core::Document doc = book_of(2);
        const std::string refused = apply_to(doc, Json::array({Json{{"op", "add_line"}, {"page", 1}, {"id", "a"}, {"text", "今日こそ言わなきゃ"}, {"x_mm", 150},
                                                                    {"y_mm", 40}, {"w_mm", 30}, {"h_mm", 45}, {"wrap", "vertical"}}}));
        if (!refused.empty()) qWarning("g2 book: %s", refused.c_str());
        return doc;
    }
    // test_g5.py's book: four B4 pages; on page 1's ink layer a line from (60, 100) to (120, 140) and a filled square
    // from (150, 200).
    static core::Document g5_book() {
        core::Document doc = book_of(4);
        const std::string ink = ink_of(doc.page(0))->id;
        const std::string refused =
            apply_to(doc, Json::array({Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", ink}, {"points", pts({{60, 100}, {120, 140}})}, {"width_mm", 1},
                                            {"stabilize", 0}, {"taper", false}},
                                       Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", ink},
                                            {"area", Json::object({{"poly", pts({{150, 200}, {180, 200}, {180, 230}, {150, 230}})}})}, {"rgb", Json::array({0, 0, 0})}}}));
        if (!refused.empty()) qWarning("g5 book: %s", refused.c_str());
        return doc;
    }
    // test_h2.py's book: three B4 pages, a line 「もしもし」 (id a) on page 1.
    static core::Document h2_book() {
        core::Document doc = book_of(3);
        const std::string refused = apply_to(doc, Json::array({Json{{"op", "add_line"}, {"page", 1}, {"id", "a"}, {"text", "もしもし"}, {"x_mm", 150}, {"y_mm", 40},
                                                                    {"w_mm", 30}, {"h_mm", 45}, {"wrap", "vertical"}}}));
        if (!refused.empty()) qWarning("h2 book: %s", refused.c_str());
        return doc;
    }
};

QTEST_MAIN(TestGuiLedgerA)
#include "test_gui_ledger_a.moc"
