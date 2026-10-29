// The window's checks of Python's GUI tests that no other C++ test made (docs ledger, tport-gui-b): tests/test_m10.py,
// test_m7_gui.py, test_ui_look.py, test_ux.py, test_ux_review.py, test_j3.py, test_j5.py, test_j6.py, test_j7.py,
// test_j8.py, test_k1.py, test_j11.py, test_m12.py, test_m14.py and test_m15.py. Each slot is named after the Python test
// it ports and makes the checks of it that were missing (what other C++ tests already check is said where they are), on
// the offscreen platform.
//
// DEPS genko_app GUI (TIMEOUT 900)

#include <QtTest>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTabBar>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "app/brush_panel.hpp"
#include "app/canvas.hpp"
#include "app/config.hpp"
#include "app/dialogs.hpp"
#include "app/documents.hpp"
#include "app/export_dialog.hpp"
#include "app/frame_tools.hpp"
#include "app/icons.hpp"
#include "app/inject.hpp"
#include "app/layer_panel.hpp"
#include "app/main_window.hpp"
#include "app/material_panel.hpp"
#include "app/material_tabs.hpp"
#include "app/pages_panel.hpp"
#include "app/review_model.hpp"
#include "app/story_panel.hpp"
#include "app/studio_widgets.hpp"
#include "app/theme.hpp"
#include "core/actor.hpp"
#include "core/brushes.hpp"
#include "core/command_bus.hpp"
#include "core/config_dir.hpp"
#include "core/mannequin.hpp"
#include "core/pyconv.hpp"
#include "formats/exporting.hpp"
#include "gui_support.hpp"
#include "render/materials.hpp"
#include "render/ops_registry.hpp"
#include "service/briefs.hpp"
#include "service/human_service.hpp"
#include "service/studio_service.hpp"
#include "service/wording.hpp"

using namespace gui_test;
namespace app = genko::app;
namespace core = genko::core;
namespace formats = genko::formats;
namespace inject = genko::app::inject;
namespace render = genko::render;
namespace service = genko::service;
namespace wording = genko::wording;
using inject::Phase;

namespace {

// new_episode(title, 1, pages, PageSpec.b4_comic()), as most of Python's window tests make their book.
core::Document b4(int pages = 1, const std::string& title = "t") {
    return core::new_episode(title, core::Num(1), pages, core::PageSpec::b4_comic());
}

// The ops applied to a book in memory as a person (Python's apply_ops on the episode before it is saved).
core::Document applied(const core::Document& doc, const Json& ops) {
    return core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor("human:tester")).doc;
}

const core::Layer* layer_with_role(const core::Page& page, core::LayerRole role) {
    for (const core::Layer& layer : page.layers)
        if (layer.role == role) return &layer;
    return nullptr;
}

const core::Layer* layer_with_id(const core::Page& page, const std::string& id) {
    for (const core::Layer& layer : page.layers)
        if (layer.id == id) return &layer;
    return nullptr;
}

const Json* prim_with_id(const core::Page& page, const std::string& id) {
    if (!page.prims.is_array()) return nullptr;
    for (const Json& prim : page.prims)
        if (prim.is_object() && prim.value("id", Json()) == Json(id)) return &prim;
    return nullptr;
}

const core::StoryLine* story_line(const core::Document& doc, const std::string& id) {
    for (const core::StoryLine& line : doc.story)
        if (line.id == id) return &line;
    return nullptr;
}

// A mouse event at a point of a widget, as the window system gives it (Python's QMouseEvent handed to the canvas).
void send_mouse(QWidget* widget, QEvent::Type type, const QPointF& pos, Qt::MouseButton button, Qt::MouseButtons buttons,
                Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QMouseEvent event(type, pos, widget->mapToGlobal(pos), button, buttons, modifiers);
    QApplication::sendEvent(widget, &event);
}

// test_m12's _drag: from a to b (page mm) in `steps` moves.
void drag_in_steps(app::PageCanvas* canvas, const QPointF& a, const QPointF& b, int steps = 6) {
    inject::mouse(canvas, Phase::Press, a);
    QPointF at = a;
    for (int i = 1; i <= steps; ++i) {
        at = a + (b - a) * (static_cast<double>(i) / steps);
        inject::mouse(canvas, Phase::Move, at);
    }
    inject::mouse(canvas, Phase::Release, at);
}

// An action of a menu (or of its submenus) by its words.
QAction* titled(QMenu* menu, const QString& text) {
    if (menu == nullptr) return nullptr;
    for (QAction* a : menu->actions()) {
        if (a->text() == text) return a;
        if (a->menu() != nullptr) {
            if (QAction* inner = titled(a->menu(), text)) return inner;
        }
    }
    return nullptr;
}

// A menu of the menu bar by its path ("ツール/定規").
QMenu* menu_at(const app::MainWindow* window, const QString& path) {
    const QStringList parts = path.split(QLatin1Char('/'));
    QMenu* menu = nullptr;
    QList<QAction*> actions = window->menuBar()->actions();
    for (const QString& part : parts) {
        QMenu* found = nullptr;
        for (QAction* a : actions)
            if (a->menu() != nullptr && a->menu()->title() == part) found = a->menu();
        if (found == nullptr) return nullptr;
        menu = found;
        actions = menu->actions();
    }
    return menu;
}

// An action of the window by its words (the ones made without an attribute name: the pose presets).
QAction* window_action(const app::MainWindow* window, const QString& text) {
    for (QAction* a : window->findChildren<QAction*>())
        if (a->text() == text) return a;
    return nullptr;
}

// The ids of the materials in a list as it shows them.
QStringList listed_ids(const QListWidget* list) {
    QStringList ids;
    for (int row = 0; row < list->count(); ++row) ids << list->item(row)->data(Qt::UserRole).toString();
    return ids;
}

// The rulers drawn on the canvas now (the canvas with its rulers shown is not the canvas with them hidden).
bool rulers_drawn(app::PageCanvas* canvas) {
    (void)canvas->wait_rendered(30000);
    canvas->rulers_visible = true;
    const QImage with = canvas->grab().toImage();
    canvas->rulers_visible = false;
    const QImage without = canvas->grab().toImage();
    canvas->rulers_visible = true;
    return with != without;
}

// test_j11's _book: a B5 book whose pages' names are approved.
core::Document volume(const std::string& title, int pages = 3) {
    core::Document doc = core::new_episode(title, core::Num(1), pages, core::PageSpec::b5_doujin());
    for (std::size_t i = 0; i < doc.pages.size(); ++i) doc.edit_page(i).name_ok = true;
    return doc;
}

// test_j11's _stroke(page)
Json stroke_on(int page) {
    return Json::array({Json{{"op", "add_stroke"}, {"page", page}, {"layer", "ink"}, {"points", Json::array({Json::array({40, 40}), Json::array({90, 120})})}}});
}

// test_j5's _book: a paint layer "red" with a red square, a pen layer "blue" with a blue line.
core::Document layers_book() {
    return applied(b4(), Json::array({
        Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "red"}},
        Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", "red"}, {"rgb", Json::array({200, 30, 30})}, {"area", Json{{"rect", Json::array({50, 50, 40, 40})}}}},
        Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "blue"}},
        Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", "blue"}, {"stabilize", 0}, {"points", Json::array({Json::array({40, 150}), Json::array({140, 170})})},
             {"width_mm", 1.5}, {"rgb", Json::array({0, 0, 200})}},
    }));
}

// test_j6's window book: two panels (a level cut in the middle) and a line "l".
core::Document two_panels_book() {
    const core::Document doc = b4();
    return applied(doc, Json::array({
        Json{{"op", "split_frame"}, {"page", 1}, {"frame_id", doc.page(0).frames[0].id}, {"axis", "horizontal"}, {"ratio", 0.5}},
        Json{{"op", "add_line"}, {"page", 1}, {"text", "やあ"}, {"x_mm", 60}, {"y_mm", 60}, {"w_mm", 30}, {"h_mm", 30}, {"id", "l"}},
    }));
}

// A book made with agents (Python's test_m7_gui._project, cut down to what these tests look at): the agent asks for the
// approval of page 1's name and asks the person about page 2. "" when it was made; else why not.
QString make_studio(const fs::path& root, fs::path& book) {
    service::StudioService agent(root, "ai:hermes");
    const auto made = agent.call("create_project", Json::array({"s.genko", "スタジオ", 3}), Json::object());
    if (!made.ok) return QString::fromStdString(made.to_dict().dump());
    book = root / "s.genko";
    const Json asked = Json::array({
        Json{{"op", "request_approval"}, {"gate", "name"}, {"pages", Json::array({1})}, {"note", "1 ページのネームを見てください"}},
        Json{{"op", "ask_human"}, {"text", "2 ページの構図が決まらない"}, {"page", 2}},
    });
    const auto requests = agent.call("apply_ops", Json::array({"s.genko", asked}), Json{{"commit", true}});
    return requests.ok ? QString() : QString::fromStdString(requests.to_dict().dump());
}

// A book open in a window (the session saves 50 ms after a change), its questions answered by the test (each: no),
// every op the window applies kept in order.
struct Desk {
    QTemporaryDir dir;
    fs::path book;
    std::unique_ptr<Answers> answers = std::make_unique<Answers>();
    std::shared_ptr<app::Session> session;
    std::unique_ptr<app::MainWindow> window;
    std::vector<Json> ops;

    explicit Desk(const core::Document& doc, QSize size = QSize(1280, 800)) : book(path_of(dir.filePath(QStringLiteral("book.genko")))) {
        write_book(book, doc);
        open(size);
    }
    // (a book made elsewhere: the studio's)
    Desk(const fs::path& existing, QSize size) : book(existing) { open(size); }
    ~Desk() {
        window.reset();
        session.reset();
        answers.reset();
    }
    Desk(const Desk&) = delete;
    Desk& operator=(const Desk&) = delete;

    void open(QSize size) {
        session = app::Session::open(book, quick(path_of(dir.filePath(QStringLiteral("recovery")))));
        window = std::make_unique<app::MainWindow>(session);
        window->resize(size);
        window->show();
        for (int i = 0; i < 10; ++i) QCoreApplication::processEvents();
        QObject::connect(session.get(), &app::Session::changed, window.get(), [this](const app::BookChange& c) {
            if (c.why == app::BookChange::Why::Edit)
                for (const Json& op : c.ops) ops.push_back(op);
        });
    }
    app::PageCanvas* canvas() const { return window->canvas(); }
    const core::Page& page() const { return *window->current_page(); }
    QAction* act(const char* name) const { return window->action(QString::fromLatin1(name)); }
    void trigger(const char* name) const {
        QAction* a = act(name);
        QVERIFY2(a != nullptr, name);
        a->trigger();
    }
    // Another book in this desk's folder.
    fs::path write(const QString& name, const core::Document& doc) const {
        const fs::path path = path_of(dir.filePath(name + QStringLiteral(".genko")));
        write_book(path, doc);
        return path;
    }
    // The last op of this name applied (null: none).
    Json last_op(const std::string& name) const {
        for (auto it = ops.rbegin(); it != ops.rend(); ++it)
            if (it->value("op", std::string()) == name) return *it;
        return Json();
    }
};

}  // namespace

class TestGuiLedgerB : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        config_folder();
        qRegisterMetaType<app::StrokeInput>();
        qRegisterMetaType<app::BookChange>();
        app::icons::init_resources();
        app::theme::apply(qApp);
    }

    // --- test_m10 ----------------------------------------------------------------------------------------------------

    // test_the_pen_draws_on_the_selected_layer_quickly: a person alone starts on the ink (it prints); a pen layer added
    // in the layer panel is the one drawn on next (描く先); a drag draws one line there and nothing on the name, a tap a
    // dot, the eraser cuts the line on the same layer; a locked layer is not drawn on and the status line says 描けません.
    // (Python's drag within 0.5 s belongs to the perf tests.)
    void thePenDrawsOnTheSelectedLayerQuickly() {
        Desk desk(b4(3, "試し"), QSize(1280, 720));
        app::MainWindow* w = desk.window.get();
        QVERIFY(w->target_layer() != nullptr);
        QCOMPARE(w->target_layer()->role, core::LayerRole::Ink);
        w->show_dock(QStringLiteral("レイヤー"));
        w->layer_panel()->add("pen", QStringLiteral("ペン"));
        const std::string user = w->target_layer()->id;
        QCOMPARE(w->target_layer()->role, core::LayerRole::User);
        QVERIFY2(w->layer_panel()->target_row()->text().contains(QStringLiteral("描く先")), qPrintable(w->layer_panel()->target_row()->text()));
        const auto user_strokes = [w, &user] {
            const core::Layer* layer = layer_with_id(*w->current_page(), user);
            return layer == nullptr ? std::size_t{0} : layer->stroke_count();
        };
        w->action(QStringLiteral("act_pen"))->trigger();
        std::vector<QPointF> line;
        for (int i = 0; i < 20; ++i) line.emplace_back(40 + i * 3, 80 + (i % 3) * 2);
        inject::mouse_stroke(w->canvas(), line);
        QCOMPARE(user_strokes(), std::size_t{1});
        const core::Layer* name = layer_with_role(*w->current_page(), core::LayerRole::Name);
        QVERIFY(name == nullptr || name->stroke_count() == 0);
        // a tap is a dot
        inject::mouse_stroke(w->canvas(), {QPointF(60, 150)});
        QCOMPARE(user_strokes(), std::size_t{2});
        // the eraser cuts the line on the same layer
        w->action(QStringLiteral("act_eraser"))->trigger();
        inject::mouse_stroke(w->canvas(), {QPointF(70, 70), QPointF(70, 75), QPointF(70, 80), QPointF(70, 85), QPointF(70, 90)});
        QCOMPARE(user_strokes(), std::size_t{3});
        // a locked layer is not drawn on
        QVERIFY(w->apply_ops(Json::array({Json{{"op", "set_layer"}, {"page", 1}, {"id", user}, {"locked", true}}})));
        w->action(QStringLiteral("act_pen"))->trigger();
        inject::mouse_stroke(w->canvas(), {QPointF(40, 120), QPointF(60, 125), QPointF(80, 120)});
        QCOMPARE(user_strokes(), std::size_t{3});
        QVERIFY2(w->last_notice().contains(QStringLiteral("描けません")), qPrintable(w->last_notice()));
    }

    // --- test_m7_gui -------------------------------------------------------------------------------------------------

    // test_dragging_a_balloon_leaves_the_model_alone_until_release: while a balloon is dragged only its picture moves
    // (the book's line is where it was); let go, the line is moved and saved there. (The move_line sent on release:
    // test_gui_lines.)
    void draggingABalloonLeavesTheModelAloneUntilRelease() {
        Desk desk(new_doc(2), QSize(1500, 950));
        app::MainWindow* w = desk.window.get();
        QVERIFY(w->apply_ops(Json::array({Json{{"op", "add_line"}, {"page", 1}, {"text", "やあ"}, {"x_mm", 60}, {"y_mm", 60}, {"w_mm", 30}, {"h_mm", 30}}})));
        const std::string id = w->book().story.back().id;
        w->choose_tool(QStringLiteral("select"));
        app::PageCanvas* canvas = w->canvas();
        canvas->fit_page();
        canvas->selected_line_id.reset();  // (none chosen: its handles are not in the way)
        const double x = w->line_by_id(id)->x_mm.value();
        const double y = w->line_by_id(id)->y_mm.value();
        const QPointF start(x + 1, y + 1);
        const QPointF end = canvas->to_mm(canvas->to_widget(start) + QPointF(40, 30));
        inject::mouse(canvas, Phase::Press, start);
        inject::mouse(canvas, Phase::Move, end);
        QCOMPARE(w->line_by_id(id)->x_mm.value(), x);  // only the preview moved
        QCOMPARE(w->line_by_id(id)->y_mm.value(), y);
        inject::mouse(canvas, Phase::Release, end);
        desk.session->save_now();
        QVERIFY(desk.session->wait_saved(std::chrono::seconds(30)));
        const core::Document saved = read_book(desk.book);
        const core::StoryLine* moved = story_line(saved, id);
        QVERIFY(moved != nullptr);
        QVERIFY(moved->x_mm.value() > x && moved->y_mm.value() > y);
    }

    // --- test_ui_look ------------------------------------------------------------------------------------------------

    // test_a_page_let_go_slides_to_a_stop: a page let go while it was moving slides on a good way, and slows to a stop.
    void aPageLetGoSlidesToAStop() {
        app::settings()->setValue(QStringLiteral("ui/reduce_motion"), QStringLiteral("false"));
        Desk desk(b4(2));
        app::PageCanvas* canvas = desk.canvas();
        const QPointF at(100, 100);
        send_mouse(canvas, QEvent::MouseButtonPress, at, Qt::MiddleButton, Qt::MiddleButton);
        for (int i = 1; i <= 5; ++i) send_mouse(canvas, QEvent::MouseMove, at + QPointF(12.0 * i, 0), Qt::NoButton, Qt::MiddleButton);
        const double x = canvas->pan().x();
        send_mouse(canvas, QEvent::MouseButtonRelease, at + QPointF(60, 0), Qt::MiddleButton, Qt::NoButton);
        QVERIFY(canvas->coasting());
        QVERIFY(wait_for([&] { return canvas->pan().x() > x + 30; }, 3000));
        QVERIFY(wait_for([&] { return !canvas->coasting(); }, 5000));
        app::settings()->remove(QStringLiteral("ui/reduce_motion"));
    }

    // test_a_question_makes_the_reply_the_main_button: a question chosen in the approval box makes 返事を送る the main
    // button, a gate's request makes 承認 the main button (the question's alone: test_gui_studio replyToQuestion).
    void aQuestionMakesTheReplyTheMainButton() {
        QTemporaryDir root;
        fs::path book;
        const QString failed = make_studio(path_of(root.path()), book);
        QVERIFY2(failed.isEmpty(), qPrintable(failed));
        Desk desk(book, QSize(1280, 800));
        app::ApprovalBox* box = desk.window->approvals();
        QVERIFY(box != nullptr);
        box->refresh();
        int help = -1;
        int gate = -1;
        for (std::size_t i = 0; i < box->items().size(); ++i) {
            if (box->items()[i].kind == "help") help = static_cast<int>(i);
            if (box->items()[i].kind == "gate") gate = static_cast<int>(i);
        }
        QVERIFY(help >= 0 && gate >= 0);
        box->list->setCurrentRow(help);
        QVERIFY(box->back_button->property("primary").toBool() && !box->approve_button->property("primary").toBool());
        box->list->setCurrentRow(gate);
        QVERIFY(box->approve_button->property("primary").toBool() && !box->back_button->property("primary").toBool());
    }

    // test_the_start_screen_shows_covers: a recent book's cover is its first page 180 px high, under its title.
    void theStartScreenShowsCovers() {
        QTemporaryDir tmp;
        const fs::path project = path_of(tmp.filePath(QStringLiteral("c.genko")));
        write_book(project, core::new_episode("表紙の本", core::Num(1), 1, core::PageSpec::b5_doujin()));
        app::remember_project(project);
        app::StartDialog dialog;
        QListWidgetItem* item = nullptr;
        for (int row = 0; row < dialog.recent()->count(); ++row)
            if (dialog.recent()->item(row)->text() == QStringLiteral("表紙の本")) item = dialog.recent()->item(row);
        QVERIFY(item != nullptr);
        // (the blank card first; the cover when it is drawn, on a worker: in every mode of the icon)
        QVERIFY(wait_for([&] { return !item->icon().availableSizes(QIcon::Selected).isEmpty(); }, 30000));
        QCOMPARE(item->icon().availableSizes(QIcon::Selected).front().height(), 180);
        dialog.close();
    }

    // --- test_ux -----------------------------------------------------------------------------------------------------

    // test_errors_and_labels_are_in_plain_japanese: the ops' errors and the names of actors and layers in words.
    void errorsAndLabelsAreInPlainJapanese() {
        QCOMPARE(wording::error(QStringLiteral("ops[0] delete_page: cannot delete the last page")), QStringLiteral("最後の 1 ページは削除できません"));
        QVERIFY(wording::error(QStringLiteral("page 3: the name is approved; a person must revoke it before the layout changes (strict_gates)"))
                    .contains(QStringLiteral("承認済み")));
        QCOMPARE(wording::error(QStringLiteral("page 2 locked by ai:hermes")), QStringLiteral("2 ページは ai:hermes が作業中です"));
        QCOMPARE(wording::error(QStringLiteral("no page 9")), QStringLiteral("9 ページはありません"));
        QCOMPARE(wording::error(QStringLiteral("ops[0] add_layer: shape must be linear, radial, ellipse, angle, reflected or diamond")),
                 QStringLiteral("グラデーションの形は、直線・円・楕円・角度・反射・菱形から選びます"));
        QVERIFY(wording::error(QStringLiteral("something new")).startsWith(QStringLiteral("この操作はできませんでした")));
        QCOMPARE(wording::actor("ai:hermes"), QStringLiteral("AI（hermes）"));
        QCOMPARE(wording::actor("human:leaf"), QStringLiteral("leaf"));
        const core::Document doc = core::new_episode("t", core::Num(1), 1, core::PageSpec::a4_mono());
        QStringList labels;
        for (const core::Layer& layer : doc.page(0).layers) labels << wording::layer_label(layer);
        QVERIFY2(labels.contains(QStringLiteral("ネーム")) && labels.contains(QStringLiteral("ペン入れ")), qPrintable(labels.join(QStringLiteral(", "))));
        // placed art whose title is made of an id
        core::Layer placed;
        placed.title = "art 876604a32748";
        placed.role = doc.page(0).layers[0].role;
        placed.kind = core::LayerKind::Placed;
        QCOMPARE(wording::layer_label(placed), QStringLiteral("絵（配置）"));
    }

    // test_official_export_needs_a_saved_book_and_preflight: the official export is refused for a book never saved, for a
    // format it does not take (PSD), and by the check before printing.
    void officialExportNeedsASavedBookAndPreflight() {
        QTemporaryDir tmp;
        const fs::path project = path_of(tmp.filePath(QStringLiteral("plain.genko")));
        write_book(project, b4(1, "試し"));
        const core::Document doc = read_book(project);
        const fs::path out = path_of(tmp.filePath(QStringLiteral("o")));
        formats::RunOptions official;
        official.official = true;
        QVERIFY(!service::run_export(doc, std::nullopt, "pdf", out, official).value("ok", true));
        QVERIFY(!service::run_export(doc, project, "psd", out, official).value("ok", true));
        official.actor = "human:leaf";
        const Json refused = service::run_export(doc, project, "pdf", out, official);
        QVERIFY2(!refused.value("ok", true) && refused.contains("errors") && core::py_truthy(refused["errors"]), refused.dump().c_str());
    }

    // test_the_window_fits_a_laptop_screen_and_the_page_fits_the_view: a book made with agents fits 1024 × 640; the page
    // fits the view (zoomed in and back); the status line says where you are in words (ページ, no stage=, not the
    // computer's user name) and the page list has the name stage (ネーム). (Python's canvas.background is its own: this
    // build draws the page in tiles.)
    void theWindowFitsALaptopScreenAndThePageFitsTheView() {
        QTemporaryDir root;
        fs::path book;
        const QString failed = make_studio(path_of(root.path()), book);
        QVERIFY2(failed.isEmpty(), qPrintable(failed));
        Desk desk(book, QSize(1280, 720));
        app::MainWindow* w = desk.window.get();
        const QSize hint = w->minimumSizeHint();
        QVERIFY2(hint.width() <= 1024 && hint.height() <= 640, qPrintable(QStringLiteral("%1 × %2").arg(hint.width()).arg(hint.height())));
        app::PageCanvas* canvas = desk.canvas();
        const core::Page& page = desk.page();
        const QPointF top_left = canvas->to_widget(QPointF(0, 0));
        const QPointF bottom_right = canvas->to_widget(QPointF(page.spec.width_mm.value(), page.spec.height_mm.value()));
        QVERIFY(0 <= top_left.x() && bottom_right.x() <= canvas->width() && 0 <= top_left.y() && bottom_right.y() <= canvas->height());
        const double before = canvas->scale();
        desk.trigger("act_zoom_in");
        QVERIFY(canvas->scale() > before && !canvas->fitted());
        desk.trigger("act_fit");
        QVERIFY(std::abs(canvas->scale() - before) < 1e-6 * before && canvas->fitted());
        // the status says where you are, in words
        QTRY_VERIFY_WITH_TIMEOUT(w->status_label()->text().contains(QStringLiteral("ページ")), 10000);
        const QString status = w->status_label()->text();
        QVERIFY2(!status.contains(QStringLiteral("stage=")), qPrintable(status));
        QVERIFY2(!status.contains(QStringLiteral("tester")), qPrintable(status));  // (GENKO_USER: not shown, U-21)
        QVERIFY(w->pages()->count() > 0);
        QVERIFY2(w->pages()->item(0)->text().contains(QStringLiteral("ネーム")), qPrintable(w->pages()->item(0)->text()));
    }

    // test_wheel_scrolls_and_ctrl_wheel_zooms: the wheel scrolls the page (the zoom as it was), Ctrl+wheel zooms (Ctrl+wheel
    // alone: test_gui_canvas).
    void wheelScrollsAndCtrlWheelZooms() {
        Desk desk(b4(3, "試し"), QSize(1280, 720));
        app::PageCanvas* canvas = desk.canvas();
        const auto wheel = [canvas](int dy, Qt::KeyboardModifiers modifiers) {
            QWheelEvent event(QPointF(100, 100), canvas->mapToGlobal(QPointF(100, 100)), QPoint(0, 0), QPoint(0, dy), Qt::NoButton, modifiers,
                              Qt::NoScrollPhase, false);
            QApplication::sendEvent(canvas, &event);
        };
        const double pan = canvas->pan().y();
        const double scale = canvas->scale();
        wheel(-120, Qt::NoModifier);
        QVERIFY(canvas->pan().y() < pan);
        QCOMPARE(canvas->scale(), scale);
        wheel(120, Qt::ControlModifier);
        QVERIFY(canvas->scale() > scale);
    }

    // test_select_tool_selects_and_pans_and_the_pen_only_draws_when_chosen: with the select tool a click chooses the
    // panel and a drag moves the view without drawing; the pen draws one line, taken back and put back.
    void selectToolSelectsAndPansAndThePenOnlyDrawsWhenChosen() {
        Desk desk(b4(3, "試し"), QSize(1280, 720));
        app::MainWindow* w = desk.window.get();
        app::PageCanvas* canvas = desk.canvas();
        QCOMPARE(canvas->tool(), QStringLiteral("select"));
        const core::Rect inner = desk.page().inner_rect_mm();
        const QPointF inside = canvas->to_widget(QPointF(inner.x.value() + 20, inner.y.value() + 20));
        send_mouse(canvas, QEvent::MouseButtonPress, inside, Qt::LeftButton, Qt::LeftButton);
        send_mouse(canvas, QEvent::MouseButtonRelease, inside, Qt::LeftButton, Qt::NoButton);
        QVERIFY(w->current_page()->selected_frame_id.is_string());  // a click selects the panel
        const std::size_t strokes = w->target_layer()->stroke_count();
        // a drag with the select tool moves the view and draws nothing
        const double pan = canvas->pan().x();
        send_mouse(canvas, QEvent::MouseButtonPress, inside, Qt::LeftButton, Qt::LeftButton);
        send_mouse(canvas, QEvent::MouseMove, inside + QPointF(60, 0), Qt::NoButton, Qt::LeftButton);
        send_mouse(canvas, QEvent::MouseButtonRelease, inside + QPointF(60, 0), Qt::LeftButton, Qt::NoButton);
        QVERIFY(canvas->pan().x() > pan);
        QCOMPARE(w->target_layer()->stroke_count(), strokes);
        QVERIFY(wait_for([&] { return !canvas->coasting(); }, 5000));  // (the page let go comes to rest first)
        desk.trigger("act_pen");
        QCOMPARE(canvas->tool(), QStringLiteral("pen"));
        const QPointF from = canvas->to_widget(QPointF(inner.x.value() + 20, inner.y.value() + 20));
        send_mouse(canvas, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
        for (int step = 1; step < 8; ++step) send_mouse(canvas, QEvent::MouseMove, from + QPointF(step * 8, step * 4), Qt::NoButton, Qt::LeftButton);
        send_mouse(canvas, QEvent::MouseButtonRelease, from + QPointF(56, 28), Qt::LeftButton, Qt::NoButton);
        QCOMPARE(w->target_layer()->stroke_count(), strokes + 1);
        desk.trigger("act_undo");
        QCOMPARE(w->target_layer()->stroke_count(), strokes);
        desk.trigger("act_redo");
        QCOMPARE(w->target_layer()->stroke_count(), strokes + 1);
    }

    // test_new_manuscript_and_start_screen: 新しい原稿 makes a book of 12 pages on B4 (257 mm wide) in the folder; the
    // same name again is refused and nothing is written over; the start screen lists it.
    void newManuscriptAndStartScreen() {
        QTemporaryDir tmp;
        Answers answers;
        app::NewProjectDialog dialog;
        dialog.title->setText(QStringLiteral("夏の約束"));
        dialog.pages->setValue(12);
        dialog.folder->setText(tmp.path());
        QVERIFY(dialog.create());
        QVERIFY(dialog.created.has_value());
        const fs::path wanted = path_of(tmp.filePath(QStringLiteral("夏の約束.genko")));
        QVERIFY(fs::exists(wanted) && fs::equivalent(*dialog.created, wanted));
        const core::Document made = read_book(*dialog.created);
        QCOMPARE(made.title, std::string("夏の約束"));
        QCOMPARE(made.pages.size(), std::size_t{12});
        QCOMPARE(made.page(0).spec.width_mm.value(), 257.0);
        const std::int64_t revision = disk_revision(wanted);
        // the same name again is refused, nothing is overwritten
        dialog.created.reset();
        QVERIFY(!dialog.create());
        QVERIFY(!dialog.created.has_value());
        QVERIFY(!answers.asked.isEmpty() && answers.asked.last().contains(QStringLiteral("同じ名前の原稿がすでにあります")));
        QCOMPARE(disk_revision(wanted), revision);
        app::remember_project(wanted);
        app::StartDialog start;
        bool listed = false;
        for (int row = 0; row < start.recent()->count(); ++row) listed = listed || start.recent()->item(row)->text().startsWith(QStringLiteral("夏の約束"));
        QVERIFY(listed);
    }

    // test_export_dialog_shows_only_the_options_of_the_format: each format shows its own options (縦読み: the width and
    // 正式な書き出し, no resolution; PSD: the resolution, not 正式な書き出し); 正式な書き出し's own dialog lists only the
    // formats it takes (no PSD) with the switch on and fixed. (Writing the PSD from the dialog: test_gui_output.)
    void exportDialogShowsOnlyTheOptionsOfTheFormat() {
        Desk desk(b4(1, "試し"), QSize(1280, 720));
        app::ExportDialog dialog(desk.window.get(), desk.session->snapshot(), desk.book, "human:leaf");
        dialog.show();
        dialog.format->setCurrentIndex(dialog.format->findData(QStringLiteral("webtoon")));
        QCoreApplication::processEvents();
        QVERIFY(dialog.width->isVisible() && !dialog.dpi->isVisible() && dialog.official->isVisible());
        dialog.format->setCurrentIndex(dialog.format->findData(QStringLiteral("psd")));
        QCoreApplication::processEvents();
        QVERIFY(dialog.dpi->isVisible() && !dialog.official->isVisible());
        dialog.close();
        app::ExportDialog official(desk.window.get(), desk.session->snapshot(), desk.book, "human:leaf", true);
        QStringList keys;
        for (int i = 0; i < official.format->count(); ++i) keys << official.format->itemData(i).toString();
        QVERIFY2(!keys.isEmpty() && !keys.contains(QStringLiteral("psd")), qPrintable(keys.join(QStringLiteral(", "))));
        QVERIFY(official.official->isChecked() && !official.official->isEnabled());
    }

    // --- test_ux_review ----------------------------------------------------------------------------------------------

    // test_qt_speaks_japanese: Qt's own buttons in Japanese (はい・いいえ), whatever the computer's language.
    void qtSpeaksJapanese() {
        app::theme::apply(qApp);
        QMessageBox box(QMessageBox::Question, QStringLiteral("t"), QStringLiteral("q"), QMessageBox::Yes | QMessageBox::No);
        bool yes = false;
        bool no = false;
        QStringList words;
        for (QAbstractButton* button : box.buttons()) {
            words << button->text();
            yes = yes || button->text().contains(QStringLiteral("はい"));
            no = no || button->text().contains(QStringLiteral("いいえ"));
        }
        QVERIFY2(yes && no, qPrintable(words.join(QStringLiteral(", "))));
    }

    // test_the_approval_box_sits_on_the_right_in_sight: in a book made with agents with a question waiting, on a 1366 ×
    // 768 screen, the approval box is on the right, tall enough to see its list and a request, and the quick-access
    // panel does not lie over it.
    void theApprovalBoxSitsOnTheRightInSight() {
        core::Document doc = b4(2);
        doc.strict_gates = true;
        doc.tickets.push_back(Json{{"id", "t_help"}, {"kind", "help"}, {"status", "open"}, {"text", "背景はどうしますか"}, {"page_index", 1},
                                   {"created_by", "ai:test"}});
        Desk desk(doc, QSize(1366, 768));
        app::MainWindow* w = desk.window.get();
        for (int i = 0; i < 20; ++i) QCoreApplication::processEvents();
        QDockWidget* box = w->box_dock();
        QVERIFY(box != nullptr);
        QCOMPARE(box->windowTitle(), QStringLiteral("承認箱"));
        QCOMPARE(w->dockWidgetArea(box), Qt::RightDockWidgetArea);
        QTRY_VERIFY(box->isVisible());
        QTRY_VERIFY2(box->height() >= 120, qPrintable(QString::number(box->height())));  // (its list and at least a request in sight)
        QDockWidget* quick = w->quick_dock();
        QVERIFY(quick != nullptr);
        QVERIFY(!box->geometry().intersects(quick->geometry()));  // (nothing lies over the approval box)
    }

    // test_nine_menus_and_the_tabs_whole_on_a_1366_screen: nine menus (ten since PC4's フィルター, eleven since PC5's 書式); the panels' row of tabs with レイヤー shows every
    // tab whole (none behind the scroll arrows) on a 1366 × 768 screen.
    void nineMenusAndTheTabsWholeOnA1366Screen() {
        core::Document doc = b4(2);
        doc.strict_gates = true;
        doc.tickets.push_back(Json{{"id", "t_help"}, {"kind", "help"}, {"status", "open"}, {"text", "背景はどうしますか"}, {"page_index", 1},
                                   {"created_by", "ai:test"}});
        Desk desk(doc, QSize(1366, 768));
        app::MainWindow* w = desk.window.get();
        for (int i = 0; i < 20; ++i) QCoreApplication::processEvents();
        int menus = 0;
        for (QAction* a : w->menuBar()->actions())
            if (a->menu() != nullptr) ++menus;
        // (PC4 節 D, the user's decision 1: フィルター after レイヤー; PC5 G-0: 書式 between them, design section G 5.1 — the menu bar still
        // whole at 1366)
        QCOMPARE(menus, 11);
        const QRect last = w->menuBar()->actionGeometry(w->menuBar()->actions().last());
        QVERIFY2(!last.isEmpty() && last.right() <= w->menuBar()->width(), qPrintable(QStringLiteral("%1 of %2").arg(last.right()).arg(w->menuBar()->width())));
        QTabBar* bar = nullptr;
        for (QTabBar* b : w->findChildren<QTabBar*>()) {
            if (!b->isVisible()) continue;
            for (int i = 0; i < b->count(); ++i)
                if (b->tabText(i) == QStringLiteral("レイヤー")) bar = b;
        }
        QVERIFY(bar != nullptr);
        for (int i = 0; i < bar->count(); ++i)
            QVERIFY2(bar->tabRect(i).right() <= bar->width(), qPrintable(bar->tabText(i)));  // (no tab behind the arrows)
    }

    // --- test_j3 -----------------------------------------------------------------------------------------------------

    // test_the_brush_dialog_has_the_tip_settings: the brush dialog keeps a brush's pattern, its turn and its speed, and
    // what it gives makes a brush with them (its name, texture, width and opacity: test_gui_brushes ownBrushes).
    void theBrushDialogHasTheTipSettings() {
        Desk desk(b4());
        app::BrushDialog dialog(desk.window.get(), "stars");
        Json data = dialog.data();
        QCOMPARE(data["pattern"], Json("stars"));
        QCOMPARE(data["turn_jitter"], Json(true));
        dialog.pattern->setCurrentIndex(dialog.pattern->findData(QStringLiteral("hearts")));
        dialog.speed->setValue(40);
        data = dialog.data();
        QCOMPARE(data["pattern"], Json("hearts"));
        QCOMPARE(data["speed"], Json(0.4));
        const core::Brush made = core::brush_from_dict("my_x", data, std::nullopt, std::span<const core::Brush>());
        QCOMPARE(made.pattern, std::string("hearts"));
    }

    // --- test_j5 -----------------------------------------------------------------------------------------------------

    // test_the_layer_panel_works_on_several_layers: the layers chosen together put in a folder and merged, the search
    // box hiding the rows whose names do not hold the words (and showing them all again), the visible layers merged into
    // a copy. (Grouping, merging and merging the visible ones alone: test_gui_layers severalAtOnce.)
    void theLayerPanelWorksOnSeveralLayers() {
        Desk desk(layers_book());
        app::LayerPanel* panel = desk.window->layer_panel();
        const auto choose = [panel](const std::vector<std::string>& ids) {
            panel->list()->clearSelection();
            for (const std::string& id : ids) {
                const auto at = std::find(panel->ids().begin(), panel->ids().end(), id);
                if (at != panel->ids().end()) panel->list()->item(static_cast<int>(at - panel->ids().begin()))->setSelected(true);
            }
        };
        panel->refresh();
        choose({"red", "blue"});
        const auto chosen = panel->selected_ids();
        QCOMPARE(std::set<std::string>(chosen.begin(), chosen.end()), (std::set<std::string>{"red", "blue"}));
        QAction* group = titled(panel->many_menu(), QStringLiteral("選んだレイヤーをフォルダにまとめる"));
        QVERIFY(group != nullptr);
        group->trigger();
        panel->refresh();
        const core::Layer* folder = nullptr;
        for (const core::Layer& layer : desk.page().layers)
            if (layer.kind == core::LayerKind::Folder) folder = &layer;
        QVERIFY(folder != nullptr);
        QCOMPARE(layer_with_id(desk.page(), "red")->parent_id, Json(folder->id));
        choose({"red", "blue"});
        QAction* merge = titled(panel->many_menu(), QStringLiteral("選んだレイヤーを結合"));
        QVERIFY(merge != nullptr);
        merge->trigger();
        QVERIFY(layer_with_id(desk.page(), "blue") == nullptr);
        // the search box
        panel->search_box()->setText(QStringLiteral("まとめ"));
        QVERIFY(panel->list()->count() > 0);
        for (int row = 0; row < panel->list()->count(); ++row) QVERIFY2(panel->list()->item(row)->isHidden(), qPrintable(panel->list()->item(row)->text()));
        panel->search_box()->setText(QString());
        for (int row = 0; row < panel->list()->count(); ++row) QVERIFY(!panel->list()->item(row)->isHidden());
        QAction* visible_copy = titled(panel->many_menu(), QStringLiteral("表示レイヤーのコピーを結合"));
        QVERIFY(visible_copy != nullptr);
        visible_copy->trigger();
        QCOMPARE(desk.page().layers.back().title, std::string("表示レイヤーのコピー"));
    }

    // test_fill_and_correction_layers_from_the_panel: a fill layer of the colour chosen; a gradient map's colours start
    // from the pen's colour to white; a correction layer; its colour printed; the filters include motion blur, line art,
    // the gradient map and posterize; the twenty blend modes, colour dodge among them. (Fill, gradient and correction
    // layers alone: test_gui_layers.)
    void fillAndCorrectionLayersFromThePanel() {
        Desk desk(layers_book());
        app::MainWindow* w = desk.window.get();
        app::LayerPanel* panel = w->layer_panel();
        panel->refresh();
        desk.answers->responder->colour = [](const QColor&, const QString&) { return std::optional<QColor>(QColor(1, 2, 3)); };
        panel->add_fill();
        QCOMPARE(w->target_layer()->kind, core::LayerKind::Fill);
        QCOMPARE(*w->target_layer()->fill, (Json{{"rgb", Json::array({1, 2, 3})}}));
        // a gradient map: its colours as they first are (the dialog accepted as it opens). (PC2a, the user's decision 3: the
        // gradient map has the tone corrections' window, app/adjust_forms_tone — "adjust_dialog": the same colours, its
        // reverse and dither besides)
        desk.answers->responder->exec = [](QDialog* dialog) {
            return dialog->objectName() == QStringLiteral("adjust_dialog") ? static_cast<int>(QDialog::Accepted) : static_cast<int>(QDialog::Rejected);
        };
        QAction* map = titled(panel->special_menu(), QStringLiteral("グラデーションマップ…"));
        QVERIFY(map != nullptr);
        map->trigger();
        QCOMPARE(w->target_layer()->kind, core::LayerKind::Adjust);
        const Json stops = (*w->target_layer()->adjust)["stops"];
        QCOMPARE(stops.size(), std::size_t{2});
        QCOMPARE(stops[0][1], Json(w->pen().rgb));
        // 反転 (its own layer: no numbers to ask; named as レイヤー → 色調補正 names it, Photoshop's name: the user's decision 8)
        QAction* invert = titled(panel->special_menu(), QStringLiteral("階調の反転"));
        QVERIFY(invert != nullptr);
        invert->trigger();
        QCOMPARE(w->target_layer()->kind, core::LayerKind::Adjust);
        QCOMPARE((*w->target_layer()->adjust)["kind"], Json("invert"));
        QAction* prints = titled(panel->effects_menu(), QStringLiteral("表示色で印刷する"));
        QVERIFY(prints != nullptr);
        prints->setChecked(true);
        QVERIFY(w->target_layer()->color_prints);
        // (PC4 節 D, the user's decision 1: the filters are フィルター's — the panel's かける… opens it —, Genko's own in 漫画, the
        // corrections レイヤー → 色調補正's)
        QVERIFY(panel->filter_button() != nullptr);
        for (const char* name : {"act_filter_blur_gaussian", "act_filter_manga_lineart", "act_adjust_gradient_map", "act_adjust_posterize"})
            QVERIFY2(w->action(QString::fromLatin1(name)) != nullptr, name);
        auto* blend = panel->findChild<QComboBox*>(QStringLiteral("layer_blend"));
        QVERIFY(blend != nullptr);
        QStringList blends;
        for (int i = 0; i < blend->count(); ++i) blends << blend->itemData(i).toString();
        QCOMPARE(blends.size(), 20);
        QVERIFY(blends.contains(QStringLiteral("color_dodge")));
    }

    // test_skew_handles_and_the_resampling_choice: the selection's diamond on its top side slants it (the top slides, the
    // bottom stays), and the transform goes through with the resampling chosen in 編集 → 変形 → 補間方法.
    void skewHandlesAndTheResamplingChoice() {
        Desk desk(layers_book());
        app::MainWindow* w = desk.window.get();
        app::PageCanvas* canvas = desk.canvas();
        w->choose_tool(QStringLiteral("rect"));
        canvas->set_zoom_percent(100);  // (the handles apart on the screen)
        canvas->center_on(70, 70);
        const std::vector<QPointF> square{QPointF(50, 50), QPointF(90, 50), QPointF(90, 90), QPointF(50, 90)};
        canvas->set_selection(Json{{"poly", Json::array({Json::array({50, 50}), Json::array({90, 50}), Json::array({90, 90}), Json::array({50, 90})})}}, square);
        QAction* nearest = titled(menu_at(w, QStringLiteral("編集/変形/補間方法")), QStringLiteral("ハード（ニアレストネイバー・ドット絵やトーンに）"));
        QVERIFY(nearest != nullptr);
        nearest->trigger();
        w->set_target_layer("red");
        std::optional<QVector<double>> matrix;
        QObject::connect(canvas, &app::PageCanvas::selectionTransformed, w, [&matrix](const QVector<double>& m) { matrix = m; });
        // the top side's diamond (a quarter along it) dragged along the side
        inject::mouse_stroke(canvas, {QPointF(60, 50), QPointF(65, 50), QPointF(70, 50)});
        QVERIFY(matrix.has_value() && matrix->size() == 6);
        const double a = (*matrix)[0], b = (*matrix)[1], c = (*matrix)[2], d = (*matrix)[3], e = (*matrix)[4];
        QVERIFY(a == 1 && b == 0 && d == 1);
        QVERIFY(std::abs(c * 50 + e - 10) < 1e-6 && std::abs(c * 90 + e) < 1e-6);  // top slides, bottom stays
        // (PC3 C-c: the selection's transform goes as transform_layers over the area — the drawing layer — since 自由変形)
        const Json transform = desk.last_op("transform_layers");
        QVERIFY(transform.is_object());
        QCOMPARE(transform["interp"], Json("nearest"));  // (the transform went through with the choice)
        QCOMPARE(transform["ids"], Json::array({"red"}));
        QVERIFY(transform.contains("area"));
    }

    // --- test_j6 -----------------------------------------------------------------------------------------------------

    // test_bowing_a_panel_on_the_canvas: the ◇ in the middle of the chosen panel's bottom edge dragged 8 mm out bows that
    // edge 8 mm; the reading order shown; a double border for the chosen panel (those two alone: test_gui_actions).
    void bowingAPanelOnTheCanvas() {
        Desk desk(two_panels_book());
        app::MainWindow* w = desk.window.get();
        app::PageCanvas* canvas = desk.canvas();
        const std::string id = desk.page().leaf_frames()[0]->id;
        w->choose_tool(QStringLiteral("frame"));
        emit canvas->frameSelected(QString::fromStdString(id));
        QCOMPARE(desk.page().selected_frame_id, Json(id));
        canvas->fit_page();
        const std::vector<QPointF> corners = app::shape_of(*desk.page().find_frame(id));
        QCOMPARE(corners.size(), std::size_t{4});  // (a ◇ on each of its four edges)
        const app::EdgeNormal edge = app::edge_normal(corners, 2);  // (not bowed yet: the ◇ is the edge's middle)
        const QPointF handle = edge.mid;
        inject::mouse(canvas, Phase::Press, handle);
        inject::mouse(canvas, Phase::Move, handle + QPointF(0, 8));
        inject::mouse(canvas, Phase::Release, handle + QPointF(0, 8));
        const core::Frame* bowed = desk.page().find_frame(id);
        QVERIFY(bowed != nullptr && bowed->curves.has_value() && bowed->curves->size() > 2);
        QVERIFY2(std::abs((*bowed->curves)[2] - 8) <= 0.5, qPrintable(QString::number((*bowed->curves)[2])));
        desk.trigger("act_frame_numbers");
        QVERIFY(canvas->show_frame_numbers);
        canvas->repaint();
        emit canvas->frameSelected(QString::fromStdString(id));
        w->border_kind_actions()[1]->trigger();
        const core::Frame* lined = desk.page().find_frame(id);
        QVERIFY(lined != nullptr && lined->line.has_value());
        QCOMPARE(*lined->line, (Json{{"kind", "double"}}));
    }

    // test_the_lettering_fields: 長体・平体 (scale_x) and the punctuation's spacing (yakumono) of the lettering, each an
    // edit of the line's style (yakumono alone: test_gui_lines).
    void theLetteringFields() {
        Desk desk(two_panels_book());
        app::StoryPanel* panel = desk.window->story();
        panel->refresh();
        panel->select(std::string("l"));
        panel->scale_x->setValue(0.7);
        emit panel->scale_x->editingFinished();
        QCOMPARE(story_line(desk.window->book(), "l")->style.value("scale_x", Json()), Json(0.7));
        panel->yakumono->setChecked(false);
        emit panel->yakumono->clicked(false);
        QCOMPARE(story_line(desk.window->book(), "l")->style.value("yakumono", Json()), Json(false));
    }

    // test_the_ruler_menu_and_a_layer_ruler: the ruler menu has the curve rulers (parallel, multiple, radial); a ruler made
    // the ink's own is not shown while another layer is drawn on, and is again on the ink; the ruler pen draws its line
    // there (making the ruler the layer's own and the ruler pen alone: test_gui_guides).
    void theRulerMenuAndALayerRuler() {
        Desk desk(two_panels_book());
        app::MainWindow* w = desk.window.get();
        app::PageCanvas* canvas = desk.canvas();
        QMenu* rulers = menu_at(w, QStringLiteral("ツール/定規"));
        QVERIFY(rulers != nullptr);
        for (const auto& [title, kind] : {std::pair{"平行曲線定規", "parallel_curve"}, std::pair{"多重曲線定規", "multi_curve"}, std::pair{"放射曲線定規", "radial_curve"}}) {
            QAction* a = titled(rulers, QString::fromUtf8(title));
            QVERIFY2(a != nullptr, title);
            a->trigger();
            QCOMPARE(canvas->ruler_kind, QString::fromLatin1(kind));
        }
        w->choose_tool(QStringLiteral("select"));  // (the ruler tool shows every ruler)
        canvas->fit_page();
        QVERIFY(w->apply_ops(Json::array({Json{{"op", "add_ruler"}, {"page", 1}, {"kind", "line"}, {"points", Json::array({Json::array({20, 100}), Json::array({200, 100})})}, {"id", "r"}}})));
        canvas->selected_ruler_id = std::string("r");
        const std::string ink = layer_with_role(desk.page(), core::LayerRole::Ink)->id;
        w->set_target_layer(ink);
        desk.trigger("act_ruler_layer");
        QCOMPARE(desk.page().rulers[0].value("layer_id", Json()), Json(ink));
        QVERIFY(w->apply_ops(Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "x"}}})));
        w->set_target_layer("x");
        QVERIFY(!rulers_drawn(canvas));  // (another layer's ruler is left out)
        w->set_target_layer(ink);
        QVERIFY(rulers_drawn(canvas));
        desk.trigger("act_ruler_pen");
        const core::Layer* drawn = layer_with_role(desk.page(), core::LayerRole::Ink);
        QVERIFY(drawn->stroke_count() > 0);
        const core::PointF first = drawn->strokes->items.back()->points.front();
        QCOMPARE(first.x, 20.0);
        QCOMPARE(first.y, 100.0);
    }

    // --- test_j7 -----------------------------------------------------------------------------------------------------

    // test_search_finds_by_name_folder_tag_and_kind: the materials panel's search finds by a tag (恋: the heart), by a word
    // in materials of several kinds (教室: the background and the 3D classroom), by every word given; nothing for words
    // no material has; the tags hold 漫符. (By name and kind alone: test_gui_materials kindSearchAliasesMatchLegacy. Python's
    // all_tags has no function here: the tags of all_materials are looked at.)
    void searchFindsByNameFolderTagAndKind() {
        Desk desk(b4());
        app::MaterialBrowser* browser = desk.window->materials()->browser;
        QVERIFY(browser != nullptr);
        QLineEdit* search = browser->search_box();
        browser->search_box()->setText(QStringLiteral("汗"));
        QCOMPARE(listed_ids(browser->list()), QStringList{QStringLiteral("mark-汗")});
        search->setText(QStringLiteral("教室"));
        const QStringList classroom = listed_ids(browser->list());
        QVERIFY2(classroom.contains(QStringLiteral("bg-classroom")) && classroom.contains(QStringLiteral("3d-教室（3D）")), qPrintable(classroom.join(QStringLiteral(", "))));
        search->setText(QStringLiteral("恋"));  // (a tag)
        QVERIFY(browser->list()->count() > 0);
        QCOMPARE(listed_ids(browser->list()).front(), QStringLiteral("mark-ハート"));
        search->setText(QStringLiteral("ブラシ 雪"));  // (every word)
        QVERIFY(browser->list()->count() > 0);
        QCOMPARE(listed_ids(browser->list()).front(), QStringLiteral("brush-雪ブラシ"));
        search->setText(QStringLiteral("そんなものはない"));
        QCOMPARE(browser->list()->count(), 0);
        search->clear();
        std::set<std::string> tags;
        for (const Json& item : render::materials::all_materials(core::config_dir()))
            if (item.contains("tags") && item["tags"].is_array())
                for (const Json& tag : item["tags"]) tags.insert(core::py_str(tag));
        QVERIFY(tags.count("漫符") == 1);
    }

    // test_3d_materials_are_put_where_asked: a figure material, a box and a 3D scene put where asked (the box alone:
    // test_material_stamp).
    void the3dMaterialsArePutWhereAsked() {
        const core::Document doc = applied(b4(), Json::array({
            Json{{"op", "stamp_material"}, {"page", 1}, {"material_id", "3d-デッサン人形"}, {"x_mm", 80}, {"y_mm", 120}, {"id", "m"}},
            Json{{"op", "stamp_material"}, {"page", 1}, {"material_id", "3d-箱"}, {"x_mm", 150}, {"y_mm", 100}, {"id", "b"}},
            Json{{"op", "stamp_material"}, {"page", 1}, {"material_id", "3d-教室（3D）"}, {"x_mm", 120}, {"y_mm", 200}, {"id", "s"}},
        }));
        const Json* m = prim_with_id(doc.page(0), "m");
        const Json* b = prim_with_id(doc.page(0), "b");
        const Json* s = prim_with_id(doc.page(0), "s");
        QVERIFY(m != nullptr && b != nullptr && s != nullptr);
        QCOMPARE(m->value("kind", Json()), Json("figure"));
        QCOMPARE(b->value("kind", Json()), Json("box"));
        QCOMPARE(s->value("scene", Json()), Json("classroom"));
        QCOMPARE((*b)["pos"][0].get<double>(), 150.0);
        QCOMPARE((*b)["pos"][1].get<double>(), 100.0);
        QCOMPARE((*s)["pos"][0].get<double>(), 120.0);
        QCOMPARE((*s)["pos"][1].get<double>(), 200.0);
    }

    // test_agents_see_kinds_and_tags: the materials the agents are shown (inspect materials) have their tags, a lettering
    // material its words and a 3D one its kind; a material that does not exist is refused.
    void agentsSeeKindsAndTags() {
        std::map<std::string, Json> listed;
        for (const Json& item : service::briefs::materials_list()) listed[item.value("id", std::string())] = item;
        QVERIFY(listed.count("mark-汗") == 1 && listed.count("sfx-ドーン") == 1 && listed.count("3d-箱") == 1);
        QVERIFY(listed["mark-汗"].contains("tags") && core::py_truthy(listed["mark-汗"]["tags"]));
        QCOMPARE(listed["sfx-ドーン"].value("text", Json()), Json("ドーン"));
        QCOMPARE(listed["3d-箱"].value("kind", Json()), Json("prim"));
        QVERIFY_THROWS_EXCEPTION(core::ApplyError, applied(b4(), Json::array({Json{{"op", "stamp_material"}, {"page", 1}, {"material_id", "no-such"}}})));
    }

    // test_the_panel_searches_and_uses_brushes_and_3d: the panel's search; a brush material used is the pen's brush; a 3D
    // material (stairs) put by a click on the page (the search and the brush alone: test_gui_materials).
    void thePanelSearchesAndUsesBrushesAnd3d() {
        Desk desk(b4());
        app::MainWindow* w = desk.window.get();
        app::MaterialPanel* panel = w->materials();
        panel->browser->search_box()->setText(QStringLiteral("ハート"));
        QStringList names;
        for (int row = 0; row < panel->browser->list()->count(); ++row) names << panel->browser->list()->item(row)->text();
        QVERIFY(names.contains(QStringLiteral("ハート")) && names.size() < 5);
        panel->browser->search_box()->setText(QString());
        panel->select_material(QStringLiteral("brush-雪ブラシ"));
        panel->use();
        bool own = false;
        for (const auto& [key, value] : w->book().brush_custom.items()) own = own || key.starts_with("my_");
        QVERIFY(own);
        QCOMPARE(desk.canvas()->tool(), QStringLiteral("pen"));
        Json stairs;
        for (const Json& item : render::materials::all_materials(core::config_dir()))
            if (item.value("id", Json()) == Json("3d-階段")) stairs = item;
        QVERIFY(stairs.is_object());
        w->use_material(stairs);
        inject::click_mm(desk.canvas(), QPointF(100, 120));
        bool placed = false;
        for (const Json& prim : desk.page().prims) placed = placed || prim.value("kind", Json()) == Json("stairs");
        QVERIFY(placed);
    }

    // --- test_j8 -----------------------------------------------------------------------------------------------------

    // test_materials_and_agents: the 3D hand (peace) and head materials put on the page (the agents' ops: the service
    // contract).
    void materialsAndAgents() {
        const core::Document doc = applied(b4(), Json::array({
            Json{{"op", "stamp_material"}, {"page", 1}, {"material_id", "3d-手（ピース）"}, {"x_mm", 80}, {"y_mm", 80}, {"id", "k"}},
            Json{{"op", "stamp_material"}, {"page", 1}, {"material_id", "3d-頭部"}, {"x_mm", 150}, {"y_mm", 80}, {"id", "h"}},
        }));
        const Json* hand = prim_with_id(doc.page(0), "k");
        const Json* head = prim_with_id(doc.page(0), "h");
        QVERIFY(hand != nullptr && head != nullptr);
        QCOMPARE(hand->value("kind", Json()), Json("hand"));
        QCOMPARE(hand->value("pose", Json()), Json("peace"));
        QCOMPARE(head->value("kind", Json()), Json("head"));
    }

    // --- test_k1 -----------------------------------------------------------------------------------------------------

    // test_plugins_and_the_art_pen_in_the_app: the command that opens the plugins' folder in Python's words (the plugin
    // among the layer's filters and the pen's turn reaching the line: test_gui_anim, test_gui_pen).
    void pluginsAndTheArtPenInTheApp() {
        Desk desk(b4());
        QVERIFY(desk.act("act_plugins") != nullptr);
        QCOMPARE(desk.act("act_plugins")->text(), QStringLiteral("プラグインのフォルダーを開く"));
    }

    // --- test_j11 ----------------------------------------------------------------------------------------------------

    // test_several_books_in_tabs: another book opens in a tab of its own; back on the first, its page and zoom are as they
    // were and the other is written; opening a book already open goes to its tab; 次の原稿 back and forth; closing a tab
    // leaves the window with the other book.
    void severalBooksInTabs() {
        Desk desk(volume("一の巻"));
        app::MainWindow* w = desk.window.get();
        const fs::path b = desk.write(QStringLiteral("b"), volume("二の巻", 5));
        w->go_to_page(2);
        QTRY_COMPARE(w->page_index(), 1);
        w->canvas()->zoom_by(2.0);
        const double zoom = w->canvas()->view_state().scale;
        w->open_project(b);
        QVERIFY(wait_for([&] { return w->documents().size() == 2; }));
        QCOMPARE(w->doc_tabs()->count(), 2);
        QCOMPARE(w->doc_tabs()->currentIndex(), 1);
        QCOMPARE(w->book().title, std::string("二の巻"));
        QCOMPARE(w->book().pages.size(), std::size_t{5});
        QVERIFY(w->doc_tabs()->tabText(1).contains(QStringLiteral("二の巻")) && w->windowTitle().contains(QStringLiteral("二の巻")));
        QVERIFY(w->apply_ops(stroke_on(1)));
        w->switch_document(0);  // (back to the first book: its page and zoom as they were; the second one written)
        QCOMPARE(w->book().title, std::string("一の巻"));
        QVERIFY(w->current_page()->index == core::Num(2));
        QVERIFY(std::abs(w->canvas()->view_state().scale - zoom) < 1e-6 * zoom);
        QVERIFY(ink_strokes(read_book(b)) > 0);
        w->open_project(b);  // (open again: goes to its tab)
        QCOMPARE(w->doc_tabs()->count(), 2);
        QCOMPARE(w->book().title, std::string("二の巻"));
        w->next_document(1);
        QCOMPARE(w->book().title, std::string("一の巻"));
        desk.trigger("act_next_doc");
        QCOMPARE(w->book().title, std::string("二の巻"));
        QVERIFY(w->close_document());  // (the tab closes; the window stays with the other book)
        QCOMPARE(w->doc_tabs()->count(), 1);
        QCOMPARE(w->book().title, std::string("一の巻"));
        QVERIFY(w->isVisible());
    }

    // test_the_untitled_book_gives_its_place: a book opened into a window holding only the untitled book takes its place.
    void theUntitledBookGivesItsPlace() {
        QTemporaryDir tmp;
        Answers answers;
        const fs::path c = path_of(tmp.filePath(QStringLiteral("c.genko")));
        write_book(c, volume("三の巻"));
        {
            app::MainWindow win;
            win.show();
            win.open_project(c);
            QVERIFY(wait_for([&] { return win.book().title == "三の巻"; }));
            QCOMPARE(win.doc_tabs()->count(), 1);
            QCOMPARE(win.documents().size(), std::size_t{1});
            win.close();
        }
    }

    // test_one_book_in_two_windows: a second window on the book shares it, keeps its own zoom, and each sees the other's
    // edits and undo; once closed it is no longer told, and the book goes on.
    void oneBookInTwoWindows() {
        Desk desk(volume("一の巻"));
        app::MainWindow* w = desk.window.get();
        QPointer<app::MainWindow> other = w->new_window();
        for (int i = 0; i < 3; ++i) QCoreApplication::processEvents();
        QVERIFY(other != nullptr);
        QCOMPARE(other->session_ptr(), w->session_ptr());
        QVERIFY(other->isVisible());
        other->go_to_page(1);
        w->go_to_page(1);
        other->canvas()->fit_page();
        w->canvas()->zoom_by(3.0);  // (one zoomed in to draw, the other shows the whole page)
        QVERIFY(std::abs(w->canvas()->view_state().scale - other->canvas()->view_state().scale) > 1e-6);
        QVERIFY(w->apply_ops(stroke_on(1)));
        QVERIFY(ink_strokes(other->book()) > 0);
        QTRY_VERIFY(other->canvas()->doc() == other->session().snapshot());  // (its view shows the new stroke)
        QVERIFY(other->apply_ops(stroke_on(1)));
        QTRY_COMPARE(ink_strokes(*w->canvas()->doc()), std::size_t{2});
        QVERIFY(w->apply_ops(Json::array({Json{{"op", "add_page"}, {"count", 1}}})));
        QTRY_COMPARE(other->pages()->count(), 4);
        desk.trigger("act_undo");
        QTRY_COMPARE(other->pages()->count(), static_cast<int>(w->book().pages.size()));
        other->close();
        QVERIFY(w->isVisible());
        QTRY_VERIFY(other == nullptr);  // (closed for good)
        QVERIFY(w->apply_ops(stroke_on(2)));  // (the closed window is no longer told)
        desk.session->save_now();
        QVERIFY(desk.session->wait_saved(std::chrono::seconds(30)));
        QVERIFY(ink_strokes(read_book(desk.book), 1) > 0);
    }

    // test_the_same_book_opened_from_another_window_is_shared: a book open in one window, opened from another, is the same
    // book in both (a change in one is in the other).
    void theSameBookOpenedFromAnotherWindowIsShared() {
        Desk desk(volume("一の巻"));
        const fs::path d = desk.write(QStringLiteral("d"), volume("四の巻"));
        {
            app::MainWindow second(app::Session::open(d, quick(path_of(desk.dir.filePath(QStringLiteral("recovery-d"))))));
            second.show();
            second.open_project(*desk.session->path());
            QCOMPARE(second.session_ptr(), desk.window->session_ptr());
            QCOMPARE(second.doc_tabs()->count(), 2);
            QVERIFY(second.apply_ops(stroke_on(3)));
            QVERIFY(ink_strokes(desk.window->book(), 2) > 0);
            second.session().save_now();
            QVERIFY(second.session().wait_saved(std::chrono::seconds(30)));
            second.close();
        }
    }

    // test_window_menu_and_tabs_fit_a_small_screen: the window menu has 新しいウィンドウ and 次の原稿; on a 1024 × 640 screen
    // the books' tabs show, less than 60 px high (the commands themselves: test_gui_actions).
    void windowMenuAndTabsFitASmallScreen() {
        Desk desk(volume("一の巻"));
        QMenu* windows = menu_at(desk.window.get(), QStringLiteral("ウィンドウ"));
        QVERIFY(windows != nullptr);
        QStringList texts;
        for (QAction* a : windows->actions()) texts << a->text();
        QVERIFY(texts.contains(QStringLiteral("新しいウィンドウ（同じ原稿）")) && texts.contains(QStringLiteral("次の原稿")));
        desk.window->resize(1024, 640);
        QCoreApplication::processEvents();
        QVERIFY(desk.window->doc_tabs()->isVisible());
        QVERIFY2(desk.window->doc_tabs()->height() < 60, qPrintable(QString::number(desk.window->doc_tabs()->height())));
    }

    // --- test_m12 ----------------------------------------------------------------------------------------------------

    // test_panel_tool_cuts_moves_gutters_and_reshapes: a nearly level cut from the margin snaps level (the gutter between
    // tiers), a slanted cut, a gutter dragged, a panel's corner dragged into a shape of its own, and 選んだコマの形を元に
    // 戻す takes the shape back (the cuts, the gutter and the corner alone: test_gui_canvas).
    void panelToolCutsMovesGuttersAndReshapes() {
        Desk desk(b4(2, "コマ"), QSize(1280, 720));
        app::PageCanvas* canvas = desk.canvas();
        desk.trigger("act_frame");
        const double gutter = app::settings()->value(QStringLiteral("gutter_horizontal"), 6.0).toDouble();  // (the window's gutter_mm)
        const core::Rect r = desk.page().inner_rect_mm();
        const double rx = r.x.value(), ry = r.y.value(), rw = r.width.value();
        drag_in_steps(canvas, QPointF(rx - 3, ry + 120), QPointF(rx + rw + 3, ry + 122));  // from the margin, nearly level
        auto leaves = desk.page().leaf_frames();
        QCOMPARE(leaves.size(), std::size_t{2});
        for (const core::Frame* f : leaves) QVERIFY(!f->poly.has_value());  // snapped level
        std::sort(leaves.begin(), leaves.end(), [](const core::Frame* a, const core::Frame* b) { return a->rect.y.value() < b->rect.y.value(); });
        QVERIFY(std::abs(leaves[1]->rect.y.value() - (leaves[0]->rect.y.value() + leaves[0]->rect.height.value()) - gutter) < 0.01);
        drag_in_steps(canvas, QPointF(rx + 110, ry + 5), QPointF(rx + 150, ry + 110));  // a slanted cut in the top tier
        QCOMPARE(desk.page().leaf_frames().size(), std::size_t{3});
        std::optional<app::Gutter> level;
        for (const app::Gutter& g : app::gutters(desk.page().frames[0]))
            if (g.horizontal && !level) level = g;
        QVERIFY(level.has_value());
        const QPointF mid = (level->p0 + level->p1) / 2;
        drag_in_steps(canvas, mid, mid + QPointF(0, 20));
        const auto lowest = [&desk] {
            const auto all = desk.page().leaf_frames();
            return *std::max_element(all.begin(), all.end(), [](const core::Frame* a, const core::Frame* b) { return a->rect.y.value() < b->rect.y.value(); });
        };
        QVERIFY(std::abs(lowest()->rect.y.value() - (ry + 120 - gutter / 2 + gutter + 20)) < 1.5);
        const std::string bottom = lowest()->id;
        emit canvas->frameSelected(QString::fromStdString(bottom));
        const core::Rect br = desk.page().find_frame(bottom)->rect;
        const QPointF corner(br.x.value() + br.width.value(), br.y.value() + br.height.value());
        drag_in_steps(canvas, corner, corner - QPointF(30, 20));
        const core::Frame* shaped = desk.page().find_frame(bottom);
        QVERIFY(shaped != nullptr && shaped->custom && shaped->poly.has_value());
        desk.trigger("act_reset_shape");
        QVERIFY(!desk.page().find_frame(bottom)->poly.has_value());
    }

    // --- test_m14 ----------------------------------------------------------------------------------------------------

    // test_grid_and_shift_straight_lines: with the grid shown and snapping, a pen line drawn with Shift held is straight,
    // its ends on the grid (the toggles and grid_point alone: test_gui_guides).
    void gridAndShiftStraightLines() {
        Desk desk(applied(b4(), Json::array({Json{{"op", "name_ok"}, {"page", 1}}})), QSize(1280, 720));
        app::MainWindow* w = desk.window.get();
        w->set_target_layer(layer_with_role(desk.page(), core::LayerRole::Ink)->id);
        desk.trigger("act_grid");
        desk.trigger("act_grid_snap");
        QVERIFY(desk.canvas()->grid_visible && desk.canvas()->grid_snap);
        desk.trigger("act_pen");
        inject::mouse_stroke(desk.canvas(), {QPointF(51, 49), QPointF(70, 60), QPointF(99, 81)}, Qt::ShiftModifier);
        const core::Layer* ink = layer_with_role(desk.page(), core::LayerRole::Ink);
        QVERIFY(ink->stroke_count() > 0);
        const core::Stroke& stroke = *ink->strokes->items.back();
        QVERIFY2(std::abs(stroke.points.front().x - 50) < 1e-6 && std::abs(stroke.points.front().y - 50) < 1e-6,
                 qPrintable(QStringLiteral("%1, %2").arg(stroke.points.front().x).arg(stroke.points.front().y)));
        QVERIFY2(std::abs(stroke.points.back().x - 100) < 1e-6 && std::abs(stroke.points.back().y - 80) < 1e-6,
                 qPrintable(QStringLiteral("%1, %2").arg(stroke.points.back().x).arg(stroke.points.back().y)));
        desk.trigger("act_grid");
        desk.trigger("act_grid_snap");
    }

    // test_pose_a_figure_and_turn_a_box_on_the_canvas: the stick figure placed is chosen with the 3D tool; its hand dragged
    // on the canvas points the forearm where it was let go; a pose from the menu (the box moved and turned, the 3D traced
    // and deleted, and the 3D figure's hand with IK: test_gui_guides).
    void poseAFigureAndTurnABoxOnTheCanvas() {
        Desk desk(applied(b4(), Json::array({Json{{"op", "name_ok"}, {"page", 1}}})), QSize(1280, 720));
        app::MainWindow* w = desk.window.get();
        app::PageCanvas* canvas = desk.canvas();
        w->set_target_layer(layer_with_role(desk.page(), core::LayerRole::Ink)->id);
        desk.trigger("act_add_stick");
        QVERIFY(desk.page().prims.is_array() && !desk.page().prims.empty());
        const Json figure = desk.page().prims[0];
        const std::string id = figure.value("id", std::string());
        QCOMPARE(canvas->tool(), QStringLiteral("3d"));
        QCOMPARE(canvas->selected_prim_id, std::optional<std::string>(id));
        const auto hand = core::mannequin::skeleton(figure).point("r_hand");
        const QPointF from(hand[0], hand[1]);
        const QPointF target(hand[0] - 25, hand[1] - 30);
        canvas->set_zoom_percent(100);  // (the joints apart on the screen)
        canvas->center_on((from.x() + target.x()) / 2, (from.y() + target.y()) / 2);
        inject::mouse_stroke(canvas, {from, (from + target) / 2, target});
        const auto moved = core::mannequin::skeleton(desk.page().prims[0]);
        const auto& elbow = moved.point("r_elbow");
        const auto& now = moved.point("r_hand");
        const double got = std::atan2(now[1] - elbow[1], now[0] - elbow[0]);
        const double want = std::atan2(target.y() - elbow[1], target.x() - elbow[0]);
        QVERIFY2(std::cos(got - want) > 0.99, qPrintable(QStringLiteral("%1 vs %2").arg(got).arg(want)));
        // a preset from the menu
        QAction* run = window_action(w, QStringLiteral("ポーズ: 走る"));
        QVERIFY(run != nullptr);
        run->trigger();
        QCOMPARE(desk.page().prims[0].value("preset", Json()), Json("run"));
    }

    // --- test_m15 ----------------------------------------------------------------------------------------------------

    // test_builtin_materials_stamp_tones_and_effects: every material put in a panel — a tone layer with its title for each
    // tone, an effect line for each effect (one where it was put); the folders トーン, グラデーション and 効果線; a material
    // that does not exist is refused. (Python's 48 px thumbnails have no counterpart: this build shows the 56 px pictures
    // packaged with the catalogue, test_gui_materials.)
    void builtinMaterialsStampTonesAndEffects() {
        const Json catalog = render::materials::all_materials(core::config_dir());
        const std::vector<std::string> folders = render::materials::folders(core::config_dir());
        for (const char* folder : {"トーン", "グラデーション", "効果線"})
            QVERIFY2(std::find(folders.begin(), folders.end(), std::string(folder)) != folders.end(), folder);
        core::Document doc = applied(b4(), Json::array({Json{{"op", "name_ok"}, {"page", 1}}}));
        const std::string frame = doc.page(0).leaf_frames()[0]->id;
        std::size_t tones = 0;
        std::size_t effects = 0;
        for (const Json& item : catalog) {
            const std::string kind = item.value("kind", std::string());
            tones += kind == "tone" ? 1 : 0;
            effects += kind == "effect" ? 1 : 0;
            Json op{{"op", "stamp_material"}, {"page", 1}, {"material_id", item["id"]}, {"frame_id", frame}};
            if (kind != "tone") {
                op["x_mm"] = 120;
                op["y_mm"] = 150;
            }
            try {
                doc = applied(doc, Json::array({op}));
            } catch (const std::exception& error) {
                QFAIL(qPrintable(QString::fromStdString(item.value("id", std::string()) + ": " + error.what())));
            }
        }
        QVERIFY(tones > 0 && effects > 0);
        std::size_t tone_layers = 0;
        for (const core::Layer& layer : doc.page(0).layers) {
            if (layer.kind != core::LayerKind::Tone) continue;
            ++tone_layers;
            QVERIFY(!layer.title.empty());
        }
        QCOMPARE(tone_layers, tones);
        QCOMPARE(doc.page(0).effects.size(), effects);
        bool centred = false;
        for (const Json& effect : doc.page(0).effects)
            centred = centred || (effect.contains("params") && effect["params"].value("center", Json()) == Json::array({120.0, 150.0}));
        QVERIFY(centred);
        QVERIFY_THROWS_EXCEPTION(core::ApplyError, applied(doc, Json::array({Json{{"op", "stamp_material"}, {"page", 1}, {"material_id", "nope"}}})));
    }
};

QTEST_MAIN(TestGuiLedgerB)
#include "test_gui_ledger_b.moc"
