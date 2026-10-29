// PC5 G-a (type@1, SPEC.md TEXT-02) in the window: the text tools' type layers — with 作るもの テキストレイヤー a click a
// point text, a drag a box text, a click on the target path a text along it (the input method's words, Ctrl+Enter) —,
// 横書き文字ツール (across whatever the option says), a type layer typed over (its text clicked with a text tool, its row in
// the layer panel double-clicked), the chosen type layer's box on the canvas, and 書式's commands: アンチエイリアス ▸ (the
// chosen one checked), 方向 ▸, ポイントテキストに変換・段落テキストに変換, テキストレイヤーをラスタライズ, すべてのテキストレイヤー
// を更新, Lorem Ipsum をペースト, 台詞をテキストレイヤーに・テキストレイヤーを台詞に. Each gesture's op as the window applies it.

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
#include "app/preferences.hpp"
#include "app/tool_settings.hpp"
#include "core/type_styles.hpp"
#include "core/type_text.hpp"
#include "gui_support.hpp"
#include "testsupport.hpp"
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
        // (G-b: OpenType ▸ after 方向 ▸, 言語オプション ▸ after ワープテキスト…)
        QCOMPARE(texts(type), QStringList({QStringLiteral("アンチエイリアス"), QStringLiteral("方向"), QStringLiteral("OpenType"), QStringLiteral("|"),
                                           QStringLiteral("作業用パスを作成"), QStringLiteral("シェイプに変換"), QStringLiteral("テキストレイヤーをラスタライズ"),
                                           QStringLiteral("ポイントテキストに変換"), QStringLiteral("段落テキストに変換"), QStringLiteral("|"),
                                           QStringLiteral("ワープテキスト…"), QStringLiteral("|"), QStringLiteral("言語オプション"), QStringLiteral("|"),
                                           QStringLiteral("すべてのテキストレイヤーを更新"), QStringLiteral("|"), QStringLiteral("Lorem Ipsum をペースト"),
                                           QStringLiteral("|"), QStringLiteral("初期設定の書式を読み込み"), QStringLiteral("初期設定の書式を保存"),  // (G-c)
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
        QVERIFY(!s.act("act_type_save_defaults")->isEnabled() && !s.act("act_type_load_defaults")->isEnabled());  // (G-c)
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
        QCOMPARE(s.last()["wrap"], Json("vertical"));  // (テキスト: as 縦書き says)
        // 横書き文字ツール: a line across, whatever 縦書き says (review P2-1)
        QVERIFY(s.window->text_settings()->vertical->isChecked());
        s.act("act_text_h")->trigger();
        inject::click_mm(s.canvas(), QPointF(60, 150));
        QVERIFY(type_in(s, QStringLiteral("ち")));
        QCOMPARE(s.last()["op"], Json("add_line"));
        QCOMPARE(s.last()["wrap"], Json("horizontal"));
    }

    // a type layer's text clicked with a text tool, or its row double-clicked: typed over (edit_type with the marks typed)
    void typedOver() {
        Studio s;
        QVERIFY(s.window->apply_ops(Json::array({Json{{"op", "add_type_layer"}, {"page", 1}, {"text", "はじめ"}, {"wrap", "horizontal"},
                                                      {"box", Json::array({40, 40, 60, 20})}, {"style", Json{{"size_mm", 6}}}, {"new_id", "t1"}}})));
        s.window->set_target_layer("t1");  // (making lines: only the type layer chosen is typed over — review P3-5)
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
        // all its text taken away: the type layer goes (review P3-5)
        s.canvas()->editor()->selectAll();
        QTest::keyClick(s.canvas()->editor(), Qt::Key_Delete);
        ctrl_enter(s.canvas()->editor());
        QCOMPARE(s.last(), (Json{{"op", "delete_layer"}, {"page", 1}, {"id", "t1"}}));
        QVERIFY(s.layer("t1") == nullptr);
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
        // 書式 → 作業用パスを作成 on the type layer chosen (review P2-6)
        s.act("act_line_work_path")->trigger();
        QCOMPARE(s.last(), (Json{{"op", "text_to_path"}, {"page", 1}, {"id", "t1"}}));
        s.act("act_type_update_all")->trigger();  // (set again: nothing changes in the book)
        QCOMPARE(s.last()["op"], Json("text_to_path"));
        s.act("act_type_rasterize")->trigger();
        QCOMPARE(s.last()["op"], Json("rasterize_lines"));
        QCOMPARE(s.layer("t1")->kind, core::LayerKind::Raster);
        QVERIFY(!s.canvas()->type_frame.has_value());
    }

    // PC5 G-b: 書式 → OpenType ▸ (the chosen text's features, those its font has) and 言語オプション ▸ (which features the menus
    // show, kept in the settings; the paragraph's composer and direction) — each a set_type on the whole text
    void openTypeAndLanguages() {
        Studio s;
        QCOMPARE(texts(s.menu_at(QStringLiteral("書式/OpenType"))),
                 QStringList({QStringLiteral("標準合字"), QStringLiteral("前後関係に依存する字形"), QStringLiteral("任意の合字"), QStringLiteral("スワッシュ字形"),
                              QStringLiteral("デザインのバリエーション"), QStringLiteral("タイトル用字形"), QStringLiteral("飾り文字"), QStringLiteral("序数表記"),
                              QStringLiteral("分数字"), QStringLiteral("オールドスタイル"), QStringLiteral("|"), QStringLiteral("プロポーショナルメトリクス")}));
        QCOMPARE(texts(s.menu_at(QStringLiteral("書式/言語オプション"))),
                 QStringList({QStringLiteral("初期設定の機能"), QStringLiteral("東アジア言語の機能"), QStringLiteral("中東言語および南アジア言語の機能"), QStringLiteral("|"),
                              QStringLiteral("中東および南アジア言語用 Composer"), QStringLiteral("右から左の段落")}));
        // East Asian by default (as Photoshop's Japanese edition): プロポーショナルメトリクス shown, 右から左の段落 not
        QVERIFY(s.act("act_type_lang_east_asian")->isChecked());
        QVERIFY(s.act("act_type_ot_palt")->isVisible() && !s.act("act_type_rtl")->isVisible());
        // nothing chosen: off
        s.window->refresh_type_commands();
        QVERIFY(!s.act("act_type_ot_liga")->isEnabled() && !s.act("act_type_composer_world")->isEnabled());
        const std::string inter = genko::test::test_data(QStringLiteral("type/inter-subset.ttf")).toStdString();
        QVERIFY(s.window->apply_ops(Json::array({Json{{"op", "add_type_layer"}, {"page", 1}, {"text", "office 1/2"}, {"wrap", "horizontal"},
                                                      {"point", Json::array({30, 30})}, {"style", Json{{"size_mm", 6}, {"font", inter}}}, {"new_id", "t1"}}})));
        s.window->set_target_layer("t1");
        s.window->refresh_type_commands();
        // the font's features: on where they are by default (calt), the others off; one it has not (Inter: liga, ornm): grey
        QVERIFY(!s.act("act_type_ot_liga")->isEnabled() && !s.act("act_type_ot_ornm")->isEnabled());
        QVERIFY(s.act("act_type_ot_calt")->isEnabled() && s.act("act_type_ot_calt")->isChecked());
        QVERIFY(s.act("act_type_ot_frac")->isEnabled() && !s.act("act_type_ot_frac")->isChecked());
        s.act("act_type_ot_frac")->trigger();
        QCOMPARE(s.last(), (Json{{"op", "set_type"}, {"page", 1}, {"layer_id", "t1"}, {"features", Json{{"frac", 1}}}}));
        QVERIFY(s.act("act_type_ot_frac")->isChecked());
        QVERIFY(s.act("act_type_ot_dlig")->isEnabled());
        s.act("act_type_ot_dlig")->trigger();
        QVERIFY2(s.last() == (Json{{"op", "set_type"}, {"page", 1}, {"layer_id", "t1"}, {"features", Json{{"dlig", 1}, {"frac", 1}}}}), s.last().dump().c_str());
        s.act("act_type_ot_dlig")->trigger();
        s.act("act_type_ot_calt")->trigger();  // (on by default: turned off as 0, frac kept)
        QCOMPARE(s.last()["features"], (Json{{"calt", 0}, {"frac", 1}}));
        QVERIFY(!s.act("act_type_ot_calt")->isChecked());
        s.act("act_type_ot_frac")->trigger();
        QCOMPARE(s.last()["features"], (Json{{"calt", 0}}));
        QCOMPARE((*s.layer("t1")->type->native_type)["shaping"], Json("opentype"));
        // 言語オプション: 中東言語 — 右から左の段落 shown, プロポーショナルメトリクス not; kept in the settings
        s.act("act_type_lang_middle_eastern")->trigger();
        QVERIFY(s.act("act_type_rtl")->isVisible() && !s.act("act_type_ot_palt")->isVisible());
        QCOMPARE(s.window->type_language_features(), QStringLiteral("middle_eastern"));
        s.window->refresh_type_commands();
        s.act("act_type_rtl")->trigger();
        QCOMPARE(s.last(), (Json{{"op", "set_type"}, {"page", 1}, {"layer_id", "t1"}, {"direction", "rtl"}}));
        s.act("act_type_composer_world")->trigger();
        QCOMPARE(s.last()["composer"], Json("world"));
        QCOMPARE((*s.layer("t1")->type->native_type)["para"], (Json{{"direction", "rtl"}, {"composer", "world"}}));
        s.window->refresh_type_commands();
        QVERIFY(s.act("act_type_rtl")->isChecked() && s.act("act_type_composer_world")->isChecked());
        // undone in one step
        s.act("act_undo")->trigger();
        QVERIFY(!s.layer("t1")->type->native_type->value("para", Json::object()).contains("composer"));
        s.window->type_language_features(QStringLiteral("default"));
        QVERIFY(!s.act("act_type_rtl")->isVisible() && !s.act("act_type_ot_palt")->isVisible());
    }

    // PC5 G-c: the styles through the window's type_style (as G-d's panels will call it: on the text chosen), their list read
    // with no op; 書式 → 初期設定の書式を保存 (the chosen text's formatting kept in the machine's settings, not the book) and
    // 初期設定の書式を読み込み (laid on the text chosen in one step, undone in one)
    void stylesAndDefaults() {
        Studio s;
        QVERIFY(s.window->apply_ops(Json::array({Json{{"op", "add_type_layer"}, {"page", 1}, {"text", "ABC"}, {"wrap", "horizontal"},
                                                      {"point", Json::array({30, 30})}, {"style", Json{{"size_mm", 6}}}, {"new_id", "t1"}}})));
        QVERIFY(s.window->apply_ops(Json::array({Json{{"op", "add_type_layer"}, {"page", 1}, {"text", "xyz"}, {"wrap", "horizontal"},
                                                      {"point", Json::array({30, 60})}, {"style", Json{{"size_mm", 4}}}, {"new_id", "t2"}}})));
        s.window->set_target_layer("t1");
        s.window->refresh_type_commands();
        QVERIFY(s.act("act_type_save_defaults")->isEnabled());
        QVERIFY(!s.act("act_type_load_defaults")->isEnabled());  // (nothing kept yet)
        // a character style made from the text and applied to it
        const std::optional<Json> made = s.window->type_style("character", "new", Json{{"apply", true}}, true);
        QVERIFY(made.has_value());
        QCOMPARE((*made)["id"], Json("cs-1"));
        QCOMPARE(s.last()["layer_id"], Json("t1"));
        QCOMPARE((*s.layer("t1")->type->native_type)["char_style"], Json("cs-1"));
        const Json listed = core::type_styles::list(s.window->book(), core::type_styles::Kind::Character);
        QCOMPARE(listed.size(), std::size_t{2});
        QCOMPARE(listed[1]["texts"], Json(1));
        QCOMPARE(listed[1]["char"]["size_mm"], Json(6));
        // a question through the window: answered, the book and its history as they were
        const std::size_t was = s.ops.size();
        const std::optional<Json> asked = s.window->type_style("character", "list", Json::object(), true);
        QVERIFY(asked.has_value() && (*asked)["current"]["character"] == Json("cs-1"));
        QCOMPARE(s.ops.size(), was);
        // refused (a style the book has not): said, nothing changed
        QVERIFY(!s.window->type_style("character", "apply", Json{{"id", "cs-9"}}, true).has_value());
        // 初期設定の書式を保存: in the settings, not in the book
        s.act("act_type_save_defaults")->trigger();
        QCOMPARE(s.ops.size(), was);
        const std::optional<Json> kept = app::preferences::default_type_styles();
        QVERIFY(kept.has_value());
        QCOMPARE((*kept)["char"]["size_mm"], Json(6));
        QVERIFY(s.act("act_type_load_defaults")->isEnabled());
        // 初期設定の書式を読み込み on the other text: one change, its look the kept one; undone in one step
        s.window->set_target_layer("t2");
        s.window->refresh_type_commands();
        s.act("act_type_load_defaults")->trigger();
        QCOMPARE(s.ops.size(), was + 3);
        const Json* styles = s.window->book().type_styles ? &*s.window->book().type_styles : nullptr;
        QCOMPARE(core::type_styles::defaults_of(*s.layer("t2")->type, styles), *kept);
        s.act("act_undo")->trigger();
        QVERIFY(!s.layer("t2")->type->native_type || !s.layer("t2")->type->native_type->contains("char"));
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
        QCOMPARE(id, std::string("l1"));  // (the line's id: the balloon's pen follows it)
        QCOMPARE(s.window->target_layer()->id, id);
        s.act("act_type_layer_to_line")->trigger();
        QCOMPARE(s.last()["op"], Json("type_layer_to_line"));
        QCOMPARE(s.window->book().story.size(), std::size_t{1});
        QCOMPARE(s.window->book().story[0].id, std::string("l1"));
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
