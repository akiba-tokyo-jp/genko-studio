// PC5 G-a (type@1, SPEC.md TEXT-02) in the window: the text tools' type layers — with 作るもの テキストレイヤー a click a
// point text, a drag a box text, a click on the target path a text along it (the input method's words, Ctrl+Enter) —,
// 横書き文字ツール (across whatever the option says), a type layer typed over (its text clicked with a text tool, its row in
// the layer panel double-clicked), the chosen type layer's box on the canvas, and 書式's commands: アンチエイリアス ▸ (the
// chosen one checked), 方向 ▸, ポイントテキストに変換・段落テキストに変換, テキストレイヤーをラスタライズ, すべてのテキストレイヤー
// を更新, Lorem ipsum をペースト, 台詞をテキストレイヤーに・テキストレイヤーを台詞に. Each gesture's op as the window applies it.

#include <QComboBox>
#include <QCheckBox>
#include <QInputMethodEvent>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QTemporaryDir>
#include <QtTest>

#include <functional>

#include "app/canvas.hpp"
#include "app/ime.hpp"
#include "app/inject.hpp"
#include "app/layer_panel.hpp"
#include "app/main_window.hpp"
#include "app/tool_settings.hpp"
#include "core/type_text.hpp"
#include "gui_support.hpp"
#include "render/text/lettering.hpp"

using namespace genko;
using core::Json;
namespace inject = genko::app::inject;

namespace {

struct Studio {
    QTemporaryDir tmp;
    QTemporaryDir config;
    QByteArray previous_config;
    std::shared_ptr<app::Session> session;
    std::unique_ptr<app::MainWindow> window;
    std::vector<Json> ops;
    Studio() {
        (void)gui_test::config_folder();
        previous_config = qgetenv("GENKO_CONFIG_DIR");
        qputenv("GENKO_CONFIG_DIR", config.path().toUtf8());
        const auto path = gui_test::path_of(tmp.path() + "/book");
        gui_test::write_book(path, gui_test::new_doc(2));
        session = app::Session::open(path, gui_test::quick(gui_test::path_of(tmp.path() + "/recovery")));
        window = std::make_unique<app::MainWindow>(session);
        window->resize(1300, 900);
        window->show();
        window->activateWindow();
        (void)QTest::qWaitForWindowActive(window.get());
        canvas()->fit_page();
        QObject::connect(session.get(), &app::Session::changed, window.get(), [this](const app::BookChange& c) {
            if (c.why == app::BookChange::Why::Edit)
                for (const Json& op : c.ops) ops.push_back(op);
        });
    }
    ~Studio() {
        window.reset();
        qputenv("GENKO_CONFIG_DIR", previous_config);
    }
    app::PageCanvas* canvas() const { return window->canvas(); }
    const core::Page& page() const { return window->book().page(static_cast<std::size_t>(window->page_index())); }
    QAction* act(const char* name) const { return window->action(QString::fromLatin1(name)); }
    Json last() const { return ops.empty() ? Json() : ops.back(); }
    const core::Layer* layer(const std::string& id) const {
        for (const core::Layer& l : page().layers)
            if (l.id == id) return &l;
        return nullptr;
    }
    // type layers rather than lines
    void make_type_layers() const {
        auto* make = window->text_settings()->make;
        make->setCurrentIndex(make->findData(QStringLiteral("type")));
    }
    QMenu* menu_at(const QString& path) const {
        QMenu* found = nullptr;
        std::function<void(QMenu*, const QString&)> walk = [&](QMenu* menu, const QString& at) {
            if (at == path) found = menu;
            for (QAction* a : menu->actions())
                if (a->menu() != nullptr) walk(a->menu(), at + QStringLiteral("/") + a->menu()->title());
        };
        for (QAction* top : window->menuBar()->actions())
            if (top->menu() != nullptr) walk(top->menu(), top->menu()->title());
        return found;
    }
};

void ime_commit(QWidget* widget, const QString& words) {
    QInputMethodEvent event;
    event.setCommitString(words);
    QCoreApplication::sendEvent(widget, &event);
}

void ctrl_enter(QWidget* editor) {
    QTest::keyPress(editor, Qt::Key_Control);
    QTest::keyPress(editor, Qt::Key_Return, Qt::ControlModifier);
    QWidget* focus = QApplication::focusWidget() != nullptr ? QApplication::focusWidget() : editor;
    QTest::keyRelease(focus, Qt::Key_Return, Qt::ControlModifier);
    QTest::keyRelease(focus, Qt::Key_Control);
}

// Words typed in the editor the canvas opened (through the input method), kept with Ctrl+Enter.
bool type_in(Studio& s, const QString& words) {
    app::InlineEditor* editor = s.canvas()->editor();
    if (editor == nullptr) return false;
    ime_commit(editor, words);
    ctrl_enter(editor);
    return s.canvas()->editor() == nullptr;
}

QStringList texts(QMenu* menu) {
    QStringList out;
    for (QAction* a : menu->actions()) out << (a->isSeparator() ? QStringLiteral("|") : a->text());
    return out;
}

}  // namespace

class TestGuiType : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QVERIFY(gui_test::config_folder().isValid()); }

    // 書式's commands in Photoshop's order; 横書き文字ツール beside テキスト (Shift+T), its options the text tools'
    void commands() {
        Studio s;
        QMenu* type = s.menu_at(QStringLiteral("書式"));
        QVERIFY(type != nullptr);
        QCOMPARE(texts(type), QStringList({QStringLiteral("アンチエイリアス"), QStringLiteral("方向"), QStringLiteral("|"), QStringLiteral("作業用パスを作成"),
                                           QStringLiteral("シェイプに変換"), QStringLiteral("テキストレイヤーをラスタライズ"), QStringLiteral("ポイントテキストに変換"),
                                           QStringLiteral("段落テキストに変換"), QStringLiteral("|"), QStringLiteral("ワープテキスト…"), QStringLiteral("|"),
                                           QStringLiteral("すべてのテキストレイヤーを更新"), QStringLiteral("|"), QStringLiteral("Lorem ipsum をペースト"),
                                           QStringLiteral("|"), QStringLiteral("台詞をテキストレイヤーに"), QStringLiteral("テキストレイヤーを台詞に")}));
        QCOMPARE(texts(s.menu_at(QStringLiteral("書式/アンチエイリアス"))),
                 QStringList({QStringLiteral("なし"), QStringLiteral("シャープ"), QStringLiteral("鮮明"), QStringLiteral("強く"), QStringLiteral("滑らかに"),
                              QStringLiteral("Windows LCD"), QStringLiteral("Windows")}));
        QCOMPARE(texts(s.menu_at(QStringLiteral("書式/方向"))), QStringList({QStringLiteral("横書き"), QStringLiteral("縦書き")}));
        QAction* across = s.act("act_text_h");
        QCOMPARE(across->text(), QStringLiteral("横書き文字ツール"));
        QCOMPARE(across->shortcut(), QKeySequence(QStringLiteral("Shift+T")));
        QVERIFY(s.window->tools_group()->actions().contains(across));
        QCOMPARE(s.window->tool_actions().at(QStringLiteral("text_h")), across);
        QMenu* tools = s.menu_at(QStringLiteral("ツール"));
        QCOMPARE(tools->actions().indexOf(across), tools->actions().indexOf(s.act("act_text")) + 1);
        across->trigger();
        QCOMPARE(s.canvas()->tool(), QStringLiteral("text_h"));
        QCOMPARE(s.window->tool_settings()->page_for(QStringLiteral("text_h")), static_cast<QWidget*>(s.window->text_settings()));
        QVERIFY(!s.window->text_settings()->vertical->isEnabled());
        s.act("act_text")->trigger();
        QVERIFY(s.window->text_settings()->vertical->isEnabled());
        // what they make: lines, as before
        QCOMPARE(s.window->text_settings()->make->currentData().toString(), QStringLiteral("line"));
        // nothing chosen: the commands that need a text are off
        s.window->refresh_type_commands();
        QVERIFY(!s.act("act_type_aa_none")->isEnabled() && !s.act("act_type_to_point")->isEnabled());
    }

    // a click a point text, a drag a box text; 横書き文字ツール across; the new layer the target, over the one chosen
    void madeByTheTools() {
        Studio s;
        s.make_type_layers();
        QVERIFY(s.canvas()->type_layers);
        s.act("act_text_h")->trigger();
        inject::click_mm(s.canvas(), QPointF(40, 60));
        QVERIFY(type_in(s, QStringLiteral("あいう")));
        Json op = s.last();
        QCOMPARE(op["op"], Json("add_type_layer"));
        QCOMPARE(op["point"], Json::array({40.0, 60.0}));
        QCOMPARE(op["wrap"], Json("horizontal"));
        QCOMPARE(op["text"], Json("あいう"));
        const std::string point_id = op["new_id"].get<std::string>();
        const core::Layer* made = s.layer(point_id);
        QVERIFY(made != nullptr && made->kind == core::LayerKind::Type);
        QCOMPARE(s.window->target_layer()->id, point_id);
        QVERIFY(s.canvas()->type_frame.has_value() && !s.canvas()->type_frame->box_text);
        // テキスト (down): a drag, a box text
        s.act("act_text")->trigger();
        inject::mouse_stroke(s.canvas(), {QPointF(100, 100), QPointF(120, 120), QPointF(130, 150)});
        QVERIFY(type_in(s, QStringLiteral("かきく")));
        op = s.last();
        QCOMPARE(op["box"], Json::array({100.0, 100.0, 30.0, 50.0}));
        QCOMPARE(op["wrap"], Json("vertical"));
        QCOMPARE(op["above"], Json(point_id));  // (over the layer chosen)
        QVERIFY(s.canvas()->type_frame.has_value() && s.canvas()->type_frame->box_text);
        // the anti-alias the options give
        auto* aa = s.window->text_settings()->antialias;
        aa->setCurrentIndex(aa->findData(QStringLiteral("none")));
        inject::click_mm(s.canvas(), QPointF(40, 200));
        QVERIFY(type_in(s, QStringLiteral("さ")));
        QCOMPARE(s.last()["antialias"], Json("none"));
        // lines again: a click a line, as before
        s.window->text_settings()->make->setCurrentIndex(0);
        QVERIFY(!s.canvas()->type_layers);
        inject::click_mm(s.canvas(), QPointF(60, 250));
        QVERIFY(type_in(s, QStringLiteral("た")));
        QCOMPARE(s.last()["op"], Json("add_line"));
    }

    // a type layer's text clicked with a text tool, or its row double-clicked: typed over (edit_type with the marks typed)
    void typedOver() {
        Studio s;
        QVERIFY(s.window->apply_ops(Json::array({Json{{"op", "add_type_layer"}, {"page", 1}, {"text", "はじめ"}, {"wrap", "horizontal"},
                                                      {"box", Json::array({40, 40, 60, 20})}, {"style", Json{{"size_mm", 6}}}, {"new_id", "t1"}}})));
        s.act("act_text")->trigger();
        inject::click_mm(s.canvas(), QPointF(45, 45));
        app::InlineEditor* editor = s.canvas()->editor();
        QVERIFY(editor != nullptr);
        QCOMPARE(editor->toPlainText(), QStringLiteral("はじめ"));
        editor->selectAll();
        ime_commit(editor, QStringLiteral("｜次《つぎ》"));
        ctrl_enter(editor);
        Json op = s.last();
        QCOMPARE(op["op"], Json("edit_type"));
        QCOMPARE(op["layer_id"], Json("t1"));
        QCOMPARE(op["text"], Json("次"));
        QCOMPARE(s.layer("t1")->type->text, std::string("次"));
        QVERIFY(!s.layer("t1")->type->ruby_runs.empty() || s.layer("t1")->type->ruby_spans);
        QCOMPARE(s.layer("t1")->title, std::string("次"));  // (named by its text)
        // its row double-clicked in the layer panel
        s.window->set_target_layer("t1");
        s.window->layer_panel()->refresh();
        QListWidget* list = s.window->layer_panel()->list();
        QListWidgetItem* row = nullptr;
        for (int i = 0; i < list->count(); ++i)
            if (list->item(i)->text().contains(QStringLiteral("次"))) row = list->item(i);
        QVERIFY(row != nullptr);
        list->setCurrentItem(row);
        emit list->itemDoubleClicked(row);
        QVERIFY(s.canvas()->editor() != nullptr);
        QCOMPARE(s.canvas()->editor()->toPlainText(), QStringLiteral("｜次《つぎ》"));
        s.canvas()->editor()->finish(false);
    }

    // 書式's commands on the chosen type layer: アンチエイリアス (checked), 方向, the conversions, Lorem, ラスタライズ
    void typeMenu() {
        Studio s;
        QVERIFY(s.window->apply_ops(Json::array({Json{{"op", "add_type_layer"}, {"page", 1}, {"text", "ABC"}, {"wrap", "horizontal"},
                                                      {"point", Json::array({30, 30})}, {"style", Json{{"size_mm", 6}}}, {"new_id", "t1"}}})));
        s.window->set_target_layer("t1");
        s.window->refresh_type_commands();
        QVERIFY(s.act("act_type_aa_smooth")->isChecked());
        QVERIFY(!s.act("act_type_to_point")->isEnabled() && s.act("act_type_to_box")->isEnabled());
        s.act("act_type_aa_none")->trigger();
        QCOMPARE(s.last(), (Json{{"op", "edit_type"}, {"page", 1}, {"layer_id", "t1"}, {"antialias", "none"}}));
        QVERIFY(s.act("act_type_aa_none")->isChecked());
        s.act("act_type_vertical")->trigger();
        QCOMPARE(s.last()["wrap"], Json("vertical"));
        QCOMPARE(s.layer("t1")->type->wrap, std::string("vertical"));
        s.act("act_type_to_box")->trigger();
        QCOMPARE(s.last()["box"], Json(true));
        QCOMPARE(render::text::shape_of(*s.layer("t1")->type), std::string("box"));
        s.act("act_type_to_point")->trigger();
        QCOMPARE(render::text::shape_of(*s.layer("t1")->type), std::string("point"));
        s.act("act_type_lorem")->trigger();
        QCOMPARE(s.last()["insert"]["at"], Json(3));
        QVERIFY(s.layer("t1")->type->text.size() > 10);
        s.act("act_type_update_all")->trigger();  // (set again: nothing changes in the book)
        QCOMPARE(s.last()["op"], Json("edit_type"));
        s.act("act_type_rasterize")->trigger();
        QCOMPARE(s.last()["op"], Json("rasterize_lines"));
        QCOMPARE(s.layer("t1")->kind, core::LayerKind::Raster);
        QVERIFY(!s.canvas()->type_frame.has_value());
    }

    // a line made a type layer and back (書式's last two); a click on the target path: a text along it
    void linesAndPaths() {
        Studio s;
        QVERIFY(s.window->apply_ops(Json::array({Json{{"op", "add_line"}, {"page", 1}, {"id", "l1"}, {"text", "かき"}, {"x_mm", 30}, {"y_mm", 30},
                                                      {"balloon", "none"}}})));
        s.canvas()->selected_line_id = std::string("l1");
        s.act("act_line_to_type_layer")->trigger();
        QCOMPARE(s.last()["op"], Json("line_to_type_layer"));
        QVERIFY(s.window->book().story.empty());
        const std::string id = s.last()["new_id"].get<std::string>();
        QCOMPARE(s.window->target_layer()->id, id);
        s.act("act_type_layer_to_line")->trigger();
        QCOMPARE(s.last()["op"], Json("type_layer_to_line"));
        QCOMPARE(s.window->book().story.size(), std::size_t{1});
        QVERIFY(s.layer(id) == nullptr);
        // the target path (the work path of a type layer's letters, the layer gone): a click on it, a text along it
        QVERIFY(s.window->apply_ops(Json::array({Json{{"op", "add_type_layer"}, {"page", 1}, {"text", "ロ"}, {"point", Json::array({100, 100})},
                                                      {"style", Json{{"size_mm", 20}}}, {"new_id", "big"}},
                                                 Json{{"op", "text_to_path"}, {"page", 1}, {"id", "big"}},
                                                 Json{{"op", "delete_layer"}, {"page", 1}, {"id", "big"}}})));
        QVERIFY(s.page().work_path && !s.page().work_path->subpaths.empty());
        const auto knot = s.page().work_path->subpaths.front().knots.front().anchor;
        s.make_type_layers();
        s.act("act_text_h")->trigger();
        inject::click_mm(s.canvas(), QPointF(knot.x, knot.y));
        QVERIFY(type_in(s, QStringLiteral("そう")));
        QCOMPARE(s.last()["op"], Json("add_type_layer"));
        QVERIFY(s.last()["style"].contains("text_path"));
        QVERIFY(s.last().contains("box"));
    }
};

QTEST_MAIN(TestGuiType)
#include "test_gui_type.moc"
