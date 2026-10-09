// The window's commands (M2-G1 試験 1): the 52 actions of M2 and their three groups (docs/cpp-migration/ledger.json,
// kinds gui_action and gui_action_group) are in the menus with Python's words, keys and tips (genko/app/main.py
// MainWindow._build_actions), and each does what it says — a change to the book (checked in the book afterwards), to
// the view or the tool, or the dialog it opens. Every action is triggered at least once (checked at the end).

#include <QtTest>

#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>

#include <map>
#include <memory>
#include <set>

#include "app/canvas.hpp"
#include "app/config.hpp"
#include "app/dialogs.hpp"
#include "app/documents.hpp"
#include "app/icons.hpp"
#include "app/inject.hpp"
#include "app/main_window.hpp"
#include "app/navigator.hpp"
#include "app/pages_panel.hpp"
#include "app/templates.hpp"
#include "app/theme.hpp"
#include "gui_support.hpp"
#include "render/ops_registry.hpp"

using namespace gui_test;
using genko::app::MainWindow;
using genko::app::PageCanvas;
using genko::app::SaveKind;
using genko::app::Session;
namespace inject = genko::app::inject;
namespace core = genko::core;

namespace {

using Std = QKeySequence::StandardKey;

struct Expected {
    const char* attribute;
    const char* text;
    QList<QKeySequence> keys;
    const char* tip;
    bool checkable;
    const char* menu;  // where it is: "ページ/コマ" is the コマ submenu of ページ
};

// (each key once — on Windows the standard Redo is Ctrl+Y itself, and a key bound twice would never fire —, and none
// where the system has no standard one: Quit on Windows)
QList<QKeySequence> k(std::initializer_list<QKeySequence> keys) {
    QList<QKeySequence> unique;
    for (const QKeySequence& key : keys) {
        if (!key.isEmpty() && !unique.contains(key)) unique << key;
    }
    return unique;
}
QKeySequence s(const char* text) { return QKeySequence(QString::fromLatin1(text)); }

// Python's words, keys and tips (main.py _build_actions), and the menu each is in.
const std::vector<Expected>& expected() {
    static const std::vector<Expected> list = {
        {"act_new", "新しい原稿…", k({QKeySequence(Std::New)}), "", false, "ファイル"},
        {"act_open", "開く…", k({QKeySequence(Std::Open)}), "", false, "ファイル"},
        {"act_save", "保存", k({QKeySequence(Std::Save)}), "変更は自動で保存されます。今すぐ書き込むときに使います", false, "ファイル"},
        {"act_save_as", "別の場所に保存…", k({QKeySequence(Std::SaveAs)}), "", false, "ファイル"},
        {"act_undo", "元に戻す", k({QKeySequence(Std::Undo)}), "", false, "編集"},
        {"act_redo", "やり直す", k({QKeySequence(Std::Redo), s("Ctrl+Y")}), "", false, "編集"},
        {"act_fit", "全体を表示", k({s("Ctrl+0")}), "", false, "表示"},
        {"act_zoom_in", "拡大", k({QKeySequence(Std::ZoomIn), s("Ctrl+=")}), "", false, "表示"},
        {"act_zoom_out", "縮小", k({QKeySequence(Std::ZoomOut)}), "", false, "表示"},
        {"act_actual", "原寸（紙の大きさ）", k({s("Ctrl+1")}), "", false, "表示"},
        {"act_turn_left", "左に回す（15°）", k({s("-"), s("Ctrl+Alt+Left")}),
         "表示だけを回します（原稿は回りません）。Shift＋スペースを押しながらドラッグでも回せます", false, "表示"},
        {"act_turn_right", "右に回す（15°）", k({s("^"), s("Ctrl+Alt+Right")}), "表示だけを回します（原稿は回りません）", false, "表示"},
        {"act_turn_reset", "回転・反転を戻す", k({s("Ctrl+Alt+0")}), "", false, "表示"},
        {"act_zoom_tool", "虫めがね", k({s("Z")}), "クリックで拡大、Alt＋クリックで縮小、ドラッグで囲んだ所を画面いっぱいに", true, "表示"},
        {"act_zoom_value", "表示倍率を打ち込む…", {}, "倍率（%）を数で決めます。ステータスバーの倍率でも", false, "表示"},
        {"act_mirror", "左右反転して見る", k({s("H")}), "表示だけを左右反転します（絵の歪みを見つける）。原稿は変わりません", true, "表示"},
        {"act_overview", "ページを並べて見る", k({s("Ctrl+Shift+O")}), "全ページを縮小図で並べ、ダブルクリックで開きます", false, "表示"},
        {"act_prev", "◀ 前のページ", k({QKeySequence(Std::MoveToPreviousPage), s("Ctrl+Left")}), "", false, "表示"},
        {"act_next", "次のページ ▶", k({QKeySequence(Std::MoveToNextPage), s("Ctrl+Right")}), "", false, "表示"},
        {"act_guides", "仕上がり線・基本枠を表示", k({s("Ctrl+;")}), "断ち切り（裁ち落とし）・仕上がり線・基本枠", true, "表示"},
        {"act_cmyk_proof", "CMYK で見る（色校正）", {}, "印刷したときの色の見当（CMYK の範囲に収めた色）で表示します。プロファイルは書き出しで選んだもの", true, "表示"},
        {"act_select", "選択", k({s("V")}), "コマを選ぶ・フキダシを動かす・ドラッグで表示を動かす", true, "ツール"},
        {"act_pen", "ペン", k({s("B")}), "レイヤー パネルで選んだレイヤーに描きます", true, "ツール"},
        {"act_eraser", "消しゴム", k({s("E")}), "ペンの線は触れた所で切れます", true, "ツール"},
        {"act_frame", "コマ割り", k({s("F")}),
         "コマの中をドラッグして割る（斜めも。水平・垂直に吸い付く、Alt で自由）・間の白をドラッグで間隔を動かす・選んだコマの角をドラッグで形を変える", true,
         "ツール"},
        {"act_move", "レイヤー移動", k({s("Q")}), "描く先のレイヤーを丸ごとドラッグで動かす（Shift で縦・横・45°）", true, "ツール"},
        {"act_point_wider", "選んだ点を太く", k({s("Ctrl+Alt+]")}), "線の編集で選んだ制御点のところだけ、線を太くします", false, "ツール"},
        {"act_color", "ペンの色…", k({s("C")}), "", false, "ツール"},
        {"act_split_h", "コマを横に割る（上下に分ける）", k({s("Ctrl+Shift+H")}), "", false, "ページ/コマ"},
        {"act_split_v", "コマを縦に割る（左右に分ける）", k({s("Ctrl+Shift+V")}), "", false, "ページ/コマ"},
        {"act_merge", "コマを結合（割る前に戻す）", k({s("Ctrl+Shift+M")}), "", false, "ページ/コマ"},
        {"act_delete_frame", "このコマを消す（ほかのコマはそのまま）", {}, "", false, "ページ/コマ"},
        {"act_frame_selection", "このコマを選択範囲にする", {}, "選んだコマの形を選択範囲にします（塗りつぶし・トーン・消去をコマの中だけに）", false, "ページ/コマ"},
        {"act_gutters", "コマ間隔の設定…", {}, "新しく割るときの上下・左右の間隔", false, "ページ/コマ"},
        {"act_border", "選んだコマの枠線の太さ…", {}, "", false, "ページ/コマ"},
        {"act_corner", "選んだコマの角の丸み…", {}, "角を丸くします（0 で角ばる）", false, "ページ/コマ"},
        {"act_no_border", "選んだコマの枠線をなくす", {}, "", false, "ページ/コマ"},
        {"act_bleed", "選んだコマを断ち切りにする（紙の端まで）", {}, "", false, "ページ/コマ"},
        {"act_border_colour", "選んだコマの枠線の色…", {}, "", false, "ページ/コマ"},
        {"act_frame_numbers", "コマ番号（読み順）を表示", {}, "コマの読み順を番号で見ます（印刷には出ません）", true, "ページ/コマ"},
        {"act_template", "テンプレートでコマを割る…", {}, "今のページのコマと台詞を作り直します", false, "ページ/コマ"},
        {"act_save_template", "今のコマ割りをテンプレートに残す…", {},
         "このページのコマ割り（形・枠線・断ち切り・角の丸み）を、自分のテンプレートとして残します", false, "ページ/コマ"},
        {"act_add_page", "ページを追加（この後ろに）", {}, "", false, "ページ"},
        {"act_del_page", "このページを消す…", {}, "", false, "ページ"},
        {"act_dup_page", "このページを複製", {}, "", false, "ページ"},
        {"act_page_up", "このページを前へ", k({s("Ctrl+Shift+Up")}), "", false, "ページ"},
        {"act_page_down", "このページを後ろへ", k({s("Ctrl+Shift+Down")}), "", false, "ページ"},
        {"act_name_ok", "ネーム完了 → 作画へ進む", {}, "承認の要らない原稿（AI を使わない原稿）で使います", false, "ページ"},
        // (every key the system has for Close, Ctrl+W among them: Python binds only the first, Ctrl+F4 on Windows)
        {"act_close", "閉じる", QKeySequence::keyBindings(Std::Close), "この原稿を閉じます（最後の原稿ならウィンドウも）", false, "ファイル"},
        {"act_new_window", "新しいウィンドウ（同じ原稿）", {}, "この原稿をもう 1 つのウィンドウで開きます。拡大して描きながら、別の窓で全体を見る", false,
         "ウィンドウ"},
        {"act_next_doc", "次の原稿", k({s("Ctrl+Tab")}), "", false, "ウィンドウ"},
        {"act_prev_doc", "前の原稿", k({s("Ctrl+Shift+Tab")}), "", false, "ウィンドウ"},
        {"act_quit", "Genko を終わる", k({QKeySequence(Std::Quit)}), "", false, "ファイル"},
    };
    return list;
}

// Every menu path an action is in ("ページ/コマ").
void find_in(QMenu* menu, const QString& path, QAction* wanted, QStringList& found) {
    for (QAction* a : menu->actions()) {
        if (a == wanted) found << path;
        if (a->menu() != nullptr) find_in(a->menu(), path + QStringLiteral("/") + a->menu()->title(), wanted, found);
    }
}

QStringList menus_of(MainWindow* window, QAction* action) {
    QStringList found;
    for (QAction* top : window->menuBar()->actions()) {
        if (top->menu() != nullptr) find_in(top->menu(), top->menu()->title(), action, found);
    }
    return found;
}

std::set<QString>& fired() {
    static std::set<QString> names;
    return names;
}

// A book of three pages in a window (the session saves 50 ms after a change), the questions answered by the test.
struct Studio {
    QTemporaryDir dir;
    fs::path book;
    std::shared_ptr<Session> session;
    std::unique_ptr<Answers> answers = std::make_unique<Answers>();
    std::unique_ptr<MainWindow> window;
    std::vector<Json> ops;

    explicit Studio(int pages = 3, bool agent = false) {
        book = path_of(dir.filePath("book.genko"));
        core::Document doc = new_doc(pages);
        doc.strict_gates = agent;
        write_book(book, doc);
        session = Session::open(book, quick(path_of(dir.filePath("recovery"))));
        window = std::make_unique<MainWindow>(session);
        window->resize(1280, 860);
        window->show();
        for (const auto& [name, action] : window->actions_by_name()) {
            const QString attribute = name;
            QObject::connect(action, &QAction::triggered, window.get(), [attribute] { fired().insert(attribute); });
        }
        watch(session.get());
    }
    ~Studio() {
        window.reset();
        session.reset();
        answers.reset();
    }
    void watch(Session* s) {
        QObject::connect(s, &Session::changed, window.get(), [this](const genko::app::BookChange& c) {
            if (c.why == genko::app::BookChange::Why::Edit) ops.push_back(c.ops);
        });
    }
    QAction* act(const char* name) const { return window->action(QString::fromLatin1(name)); }
    void trigger(const char* name) const {
        QAction* a = act(name);
        QVERIFY2(a != nullptr, name);
        a->trigger();
    }
    const core::Document& doc() const { return window->session().document(); }
    const core::Page& page() const { return *window->current_page(); }
    PageCanvas* canvas() const { return window->canvas(); }
    QPointF centre_of(const core::Frame& frame) const {
        return QPointF(frame.rect.x.value() + frame.rect.width.value() / 2, frame.rect.y.value() + frame.rect.height.value() / 2);
    }
    // Choose a panel the way a person does: the select tool, a click inside it.
    void choose(const core::Frame& frame) {
        window->choose_tool(QStringLiteral("select"));
        canvas()->fit_page();
        const std::string id = frame.id;
        inject::click_mm(canvas(), centre_of(frame));
        QVERIFY(page().selected_frame_id.is_string());
        QCOMPARE(page().selected_frame_id.get<std::string>(), id);
    }
    std::size_t leaves() const { return page().leaf_frames().size(); }
    const core::Frame& selected() const { return *page().find_frame(page().selected_frame_id.get<std::string>()); }
};

}  // namespace

class TestGuiActions : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        config_folder();
        qRegisterMetaType<genko::app::StrokeInput>();
        qRegisterMetaType<genko::app::BookChange>();
        genko::app::icons::init_resources();
        genko::app::theme::apply(qApp);
    }

    void cleanupTestCase() {
        // every one of the 53 was triggered by a test below
        QStringList missing;
        for (const Expected& e : expected()) {
            if (fired().count(QString::fromLatin1(e.attribute)) == 0) missing << QString::fromLatin1(e.attribute);
        }
        QVERIFY2(missing.isEmpty(), qPrintable(QStringLiteral("not triggered: ") + missing.join(QStringLiteral(", "))));
    }

    void theMenusHoldPythonsCommands() {
        Studio studio;
        MainWindow* w = studio.window.get();
        QCOMPARE(expected().size(), std::size_t{53});
        // Preserve all 53 Python actions; exposure, nombre and the six layer operations (merge, flatten, convert) have
        // their own GUI E2E tests (test_gui_materials, test_gui_color), the 31 of the selection (M3) theirs
        // (test_gui_select), the 3 of animation and the timelapse and the 2 of the filter plugins (M3) theirs
        // (test_gui_anim), the 13 of the drawing tools and the colour and the 7 of editing lines (M3) theirs
        // (test_gui_paint), the 31 of the rulers and 3D theirs (test_gui_guides), the 23 of effect lines, tones, materials,
        // the view's extras, the layer commands and the scans theirs (test_gui_effects), the 8 of the lines of dialogue
        // (M4: act_text, act_line_type, act_balloon_pen, act_line_edit, act_line_delete, act_line_wrap, act_story_editor,
        // act_replace) theirs (test_gui_lines), the 7 of the book and its pages (M4②c: act_spread, act_paper,
        // act_page_nombre, act_add_cover, act_assignee, act_merge_book, act_import_psd; act_nombre, Python's now, was
        // here before) theirs (test_gui_book).
        QCOMPARE(w->actions_by_name().size(), std::size_t{186});
        QVERIFY(w->action("act_exposure"));
        QVERIFY(w->action("act_nombre"));
        for (const char* name : {"act_layer_merge_down", "act_layer_merge_layers", "act_layer_merge_visible", "act_layer_flatten",
                                 "act_layer_convert_paint", "act_layer_convert_pen"}) {
            QVERIFY2(w->action(name), name);
        }
        QCOMPARE(w->action("act_nombre")->text(),QStringLiteral("ノンブルの設定…"));  // (Python's words)
        QVERIFY(menus_of(w,w->action("act_nombre")).contains(QStringLiteral("ページ")));
        for (const Expected& e : expected()) {
            const QString name = QString::fromLatin1(e.attribute);
            QAction* a = w->action(name);
            QVERIFY2(a != nullptr, e.attribute);
            QCOMPARE(a->text(), QString::fromUtf8(e.text));
            QCOMPARE(a->shortcuts(), e.keys);
            QCOMPARE(a->statusTip(), QString::fromUtf8(e.tip));
            QCOMPARE(a->isCheckable(), e.checkable);
            const QStringList where = menus_of(w, a);
            QVERIFY2(where.contains(QString::fromUtf8(e.menu)), qPrintable(name + QStringLiteral(" is in ") + where.join(QStringLiteral(", "))));
            // the tooltip: Python's "<title>  <key>\n<tip>", and for the tools on the palette "<name>（<key>）\n<tip>"
            const bool palette_tool = name == QLatin1String("act_select") || name == QLatin1String("act_pen") || name == QLatin1String("act_eraser") ||
                                      name == QLatin1String("act_frame") || name == QLatin1String("act_move");
            if (palette_tool) {
                QCOMPARE(a->toolTip(), QStringLiteral("%1（%2）\n%3").arg(a->text(), a->shortcut().toString(), a->statusTip()));
            } else if (*e.tip != '\0') {
                QCOMPARE(a->toolTip(),
                         (QString::fromUtf8(e.text) + QStringLiteral("  ") + a->shortcut().toString(QKeySequence::NativeText) + QStringLiteral("\n") +
                          QString::fromUtf8(e.tip))
                             .trimmed());
            }
        }
        // the palette and the command bar (Python's order); with M3's eight ways of the marquee (test_gui_select) and its
        // drawing tools and line editing (test_gui_paint), the effect lines and the material tool (test_gui_effects), the
        // text tool (test_gui_lines)
        QCOMPARE(w->tools_group()->actions().size(), 28);
    }

    void theToolsAreOneGroup() {
        Studio studio;
        MainWindow* w = studio.window.get();
        const std::map<QString, QString> tools = {{"select", "act_select"},    {"pen", "act_pen"},       {"eraser", "act_eraser"},
                                                  {"frame", "act_frame"},      {"move", "act_move"},     {"zoom", "act_zoom_tool"},
                                                  {"rect", "act_marquee"},     {"lasso", "act_lasso"},   {"wand", "act_wand"},
                                                  {"ellipse", "act_sel_ellipse"}, {"polyline", "act_sel_polyline"},
                                                  {"colour", "act_sel_colour"}, {"selpen", "act_sel_pen"}, {"selerase", "act_sel_erase"},
                                                  {"picker", "act_picker"},   {"fill", "act_fill"},     {"lassofill", "act_lassofill"},
                                                  {"gradient", "act_gradient"}, {"shape", "act_shape"}, {"blend", "act_blend"},
                                                  {"liquify", "act_liquify"}, {"reshape", "act_reshape"}, {"vector", "act_vector"},
                                                  {"ruler", "act_ruler"},     {"3d", "act_3d"},         {"effect", "act_effect"},
                                                  {"stamp", "act_stamp"},     {"text", "act_text"}};
        QCOMPARE(w->tool_actions().size(), tools.size());
        QVERIFY(w->tools_group()->isExclusive());
        QVERIFY(w->action(QStringLiteral("act_select"))->isChecked());
        for (const auto& [tool, attribute] : tools) {
            QAction* a = w->tool_actions().at(tool);
            QCOMPARE(a, w->action(attribute));
            QCOMPARE(a->actionGroup(), w->tools_group());
            QVERIFY(!a->autoRepeat());  // (a held key chooses the tool once)
            studio.trigger(attribute.toLatin1().constData());
            QCOMPARE(studio.canvas()->tool(), MainWindow::is_marquee_tool(tool) ? QStringLiteral("marquee") : tool);
            QVERIFY(a->isChecked());
            for (const auto& [other, other_attribute] : tools) {
                if (other != tool) QVERIFY(!w->action(other_attribute)->isChecked());
            }
        }
    }

    void theBorderKindsAreAGroup() {
        Studio studio;
        MainWindow* w = studio.window.get();
        const std::vector<std::pair<QString, std::string>> kinds = {
            {"枠線: 実線", "solid"}, {"枠線: 二重線", "double"}, {"枠線: 破線", "dashed"}, {"枠線: 点線", "dotted"}, {"枠線: 手描き風", "rough"}};
        QCOMPARE(w->border_kind_actions().size(), kinds.size());
        studio.choose(*studio.page().leaf_frames().front());
        for (std::size_t i = 0; i < kinds.size(); ++i) {
            QAction* a = w->border_kind_actions()[i];
            QCOMPARE(a->text(), kinds[i].first);
            QVERIFY(menus_of(w, a).contains(QStringLiteral("ページ/コマ")));
            a->trigger();
            const core::Frame& frame = studio.selected();
            if (kinds[i].second == "solid") {
                QVERIFY(!frame.line || frame.line->is_null());  // (a plain solid border is no style at all)
            } else {
                QVERIFY(frame.line.has_value());
                QCOMPARE((*frame.line)["kind"].get<std::string>(), kinds[i].second);
            }
        }
        // back to solid from another kind: the style goes
        w->border_kind_actions()[0]->trigger();
        QVERIFY(!studio.selected().line || studio.selected().line->is_null());
    }

    void theStagesAreAGroup() {
        Studio studio;
        MainWindow* w = studio.window.get();
        const std::vector<std::tuple<QString, QString, bool, bool>> stages = {
            // key, label, 全体図 shown, ページ shown
            {"name", "ネーム", false, true}, {"ink", "作画", true, false}, {"finish", "仕上げ", false, false},
            {"letter", "写植", false, true}, {"review", "承認", false, true}};
        QCOMPARE(w->stage_actions().size(), stages.size());
        for (const auto& [key, label, navigator, pages] : stages) {
            QAction* a = w->stage_actions().at(key);
            QCOMPARE(a->text(), label + QStringLiteral("の並び"));
            QCOMPARE(a->statusTip(), QStringLiteral("この段階でよく使うパネルだけを出します"));
            QVERIFY(menus_of(w, a).contains(QStringLiteral("ウィンドウ/作業の段階")));
            a->trigger();
            QCOMPARE(genko::app::settings()->value(QStringLiteral("ui/stage")).toString(), key);
            QCOMPARE(w->last_notice(), QStringLiteral("「%1」の並びにしました（ウィンドウ → 作業の段階）").arg(label));
            for (QDockWidget* dock : w->findChildren<QDockWidget*>()) {
                if (dock->windowTitle() == QLatin1String("全体図")) QCOMPARE(dock->isVisible(), navigator);
                if (dock->windowTitle() == QLatin1String("ページ")) QCOMPARE(dock->isVisible(), pages);
            }
        }
    }

    void undoAndRedo() {
        Studio studio;
        studio.window->choose_tool(QStringLiteral("pen"));
        const QPointF c = studio.canvas()->seen_mm().center();
        inject::mouse_stroke(studio.canvas(), {c, c + QPointF(10, 3), c + QPointF(20, 5)});
        QCOMPARE(ink_strokes(studio.doc()), std::size_t{1});
        studio.trigger("act_undo");
        QCOMPARE(ink_strokes(studio.doc()), std::size_t{0});
        studio.trigger("act_redo");
        QCOMPARE(ink_strokes(studio.doc()), std::size_t{1});
        // a saved change goes back through the book's journal
        QVERIFY(wait_for([&] { return studio.session->status().kind == SaveKind::Saved; }));
        studio.trigger("act_undo");
        QVERIFY(wait_for([&] { return ink_strokes(studio.doc()) == 0 && studio.session->status().kind == SaveKind::Saved; }));
        QCOMPARE(ink_strokes(read_book(studio.book)), std::size_t{0});
        // nothing more to take back: said in the status line, the book as it was
        studio.trigger("act_undo");
        QVERIFY(wait_for([&] { return !studio.session->job_running(); }));
        studio.trigger("act_undo");
        QVERIFY(!studio.window->last_notice().isEmpty());
    }

    void theViewCommands() {
        Studio studio;
        MainWindow* w = studio.window.get();
        PageCanvas* c = studio.canvas();
        c->fit_page();
        const double fitted = c->scale();
        studio.trigger("act_zoom_in");
        QVERIFY(qFuzzyCompare(c->scale(), fitted * 1.25));
        QVERIFY(!c->fitted());
        studio.trigger("act_zoom_out");
        QVERIFY(qFuzzyCompare(c->scale(), fitted * 1.25 * 0.8));
        studio.trigger("act_actual");
        QCOMPARE(c->zoom_percent(), 100);
        studio.trigger("act_fit");
        QVERIFY(c->fitted());
        QVERIFY(qFuzzyCompare(c->scale(), fitted));
        studio.answers->responder->get_int = [](const QString&, const QString& label, int, int lo, int hi) {
            return label == QStringLiteral("倍率（%。100 で紙の大きさ）") && lo == 5 && hi == 6400 ? std::optional<int>(250) : std::nullopt;
        };
        studio.trigger("act_zoom_value");
        QCOMPARE(c->zoom_percent(), 250);
        studio.trigger("act_turn_left");
        QCOMPARE(c->rotation(), -15.0);
        studio.trigger("act_turn_right");
        studio.trigger("act_turn_right");
        QCOMPARE(c->rotation(), 15.0);
        studio.trigger("act_mirror");
        QVERIFY(c->flipped());
        QVERIFY(w->action(QStringLiteral("act_mirror"))->isChecked());
        QVERIFY(w->zoom_label()->text().contains(QStringLiteral("回転 +15°")));
        QVERIFY(w->zoom_label()->text().contains(QStringLiteral("左右反転")));
        studio.trigger("act_turn_reset");
        QCOMPARE(c->rotation(), 0.0);
        QVERIFY(!c->flipped());
        QVERIFY(!w->action(QStringLiteral("act_mirror"))->isChecked());
        studio.trigger("act_zoom_tool");
        QCOMPARE(c->tool(), QStringLiteral("zoom"));
        studio.trigger("act_guides");
        QVERIFY(!c->show_guides);
        studio.trigger("act_guides");
        QVERIFY(c->show_guides);
        studio.trigger("act_cmyk_proof");  // (its pictures: test_gui_color cmykProofShowsThePageAsItPrints)
        QVERIFY(c->renderer().cmyk_proof());
        studio.trigger("act_cmyk_proof");
        QVERIFY(!c->renderer().cmyk_proof());
        QCOMPARE(w->page_index(), 0);
        studio.trigger("act_next");
        QCOMPARE(w->page_index(), 1);
        QCOMPARE(c->page_index(), std::size_t{1});
        studio.trigger("act_prev");
        QCOMPARE(w->page_index(), 0);
        studio.trigger("act_prev");  // (no page before the first)
        QCOMPARE(w->page_index(), 0);
        studio.trigger("act_overview");
        genko::app::PageOverview* overview = nullptr;
        for (QWidget* top : QApplication::topLevelWidgets()) {
            if (auto* o = qobject_cast<genko::app::PageOverview*>(top); o != nullptr && o->isVisible()) overview = o;
        }
        QVERIFY(overview != nullptr);
        QCOMPARE(overview->list()->count(), 3);
        overview->close();
    }

    void thePenCommands() {
        Studio studio;
        MainWindow* w = studio.window.get();
        studio.answers->responder->colour = [](const QColor&, const QString& title) {
            return title == QStringLiteral("色") ? std::optional<QColor>(QColor(200, 30, 40)) : std::nullopt;
        };
        studio.trigger("act_color");
        QCOMPARE(w->pen().rgb, (std::vector<std::int64_t>{200, 30, 40}));
        QCOMPARE(studio.canvas()->live_pen()["rgb"], Json::array({200, 30, 40}));
        // a line, then 選んだ点を太く on its second point (the vector tool that chooses points comes with M3)
        w->choose_tool(QStringLiteral("pen"));
        const QPointF c = studio.canvas()->seen_mm().center();
        inject::mouse_stroke(studio.canvas(), {c, c + QPointF(10, 3), c + QPointF(20, 5)});
        const std::string id = ink_of(studio.page())->strokes->items.at(0)->id;
        studio.trigger("act_point_wider");
        QCOMPARE(w->last_notice(), QStringLiteral("先に「線の編集」で線を選び、□（制御点）をクリックします"));
        studio.canvas()->vector_ids = {id};
        studio.canvas()->vector_point = 1;
        const auto before = studio.doc().pages[0];
        studio.trigger("act_point_wider");
        if (genko::render::ops_registry().find("vector_edit") != nullptr) {
            const core::Stroke& s = *ink_of(studio.page())->strokes->items.at(0);
            QVERIFY(s.pressure.at(1) > 0.7);
        } else {
            // (not in this build yet: refused in the person's words, the book untouched)
            QCOMPARE(w->last_error(), QStringLiteral("この版の Genko では、まだその操作（vector_edit）はできません"));
            QCOMPARE(studio.doc().pages[0], before);
        }
        // the layer-move tool: a drag moves the layer drawn on (transform_area, M3)
        studio.trigger("act_move");
        QCOMPARE(studio.canvas()->tool(), QStringLiteral("move"));
        inject::mouse_stroke(studio.canvas(), {c, c + QPointF(5, 0), c + QPointF(10, 0)});
        if (genko::render::ops_registry().find("transform_area") == nullptr) {
            QCOMPARE(w->last_error(), QStringLiteral("この版の Genko では、まだその操作（transform_area）はできません"));
            QCOMPARE(studio.doc().pages[0], before);
        } else {
            QVERIFY(studio.doc().pages[0] != before);
        }
    }

    void thePanelCommands() {
        Studio studio;
        MainWindow* w = studio.window.get();
        // nothing chosen: the commands say what to do first
        studio.trigger("act_split_h");
        QCOMPARE(w->last_notice(), QStringLiteral("先にコマをクリックして選びます（選択ツール）"));
        studio.trigger("act_border");
        QCOMPARE(w->last_notice(), QStringLiteral("先にコマをクリックして選びます"));
        studio.choose(*studio.page().leaf_frames().front());
        studio.trigger("act_split_h");
        QCOMPARE(studio.leaves(), std::size_t{2});
        QCOMPARE(studio.ops.back()[0]["op"].get<std::string>(), std::string("split_frame"));
        QCOMPARE(studio.ops.back()[0]["axis"].get<std::string>(), std::string("horizontal"));
        QCOMPARE(studio.ops.back()[0]["gutter_mm"].get<double>(), 6.0);  // (the gutter between tiers)
        // コマ間隔の設定: the dialog's two spins, kept in the settings, used by the next cut
        studio.answers->responder->exec = [](QDialog* dialog) {
            auto* h = dialog->findChild<QDoubleSpinBox*>(QStringLiteral("horizontal"));
            auto* v = dialog->findChild<QDoubleSpinBox*>(QStringLiteral("vertical"));
            if (h == nullptr || v == nullptr || dialog->windowTitle() != QStringLiteral("コマ間隔")) return 0;
            h->setValue(8.5);
            v->setValue(2.5);
            return static_cast<int>(QDialog::Accepted);
        };
        studio.trigger("act_gutters");
        QCOMPARE(genko::app::settings()->value(QStringLiteral("gutter_vertical")).toDouble(), 2.5);
        studio.choose(*studio.page().leaf_frames().front());
        studio.trigger("act_split_v");
        QCOMPARE(studio.leaves(), std::size_t{3});
        QCOMPARE(studio.ops.back()[0]["axis"].get<std::string>(), std::string("vertical"));
        QCOMPARE(studio.ops.back()[0]["gutter_mm"].get<double>(), 2.5);
        // the chosen panel's look
        studio.choose(*studio.page().leaf_frames().back());
        studio.answers->responder->get_double = [](const QString& title, const QString&, double, double, double hi, int) {
            if (title == QStringLiteral("枠線の太さ") && hi == 5.0) return std::optional<double>(1.5);
            if (title == QStringLiteral("角の丸み") && hi == 50.0) return std::optional<double>(4.0);
            return std::optional<double>();
        };
        studio.trigger("act_border");
        QCOMPARE(studio.selected().border_mm, 1.5);
        studio.trigger("act_corner");
        QCOMPARE(studio.selected().corner_mm, 4.0);
        studio.trigger("act_no_border");
        QCOMPARE(studio.selected().border_mm, 0.0);
        studio.trigger("act_bleed");
        QVERIFY(studio.selected().bleed);
        QCOMPARE(w->last_notice(), QStringLiteral("断ち切りにしました（紙の端に接する辺は枠線なし）"));
        studio.trigger("act_bleed");
        QVERIFY(!studio.selected().bleed);
        studio.answers->responder->colour = [](const QColor& now, const QString& title) {
            return title == QStringLiteral("枠線の色") && now == QColor(20, 20, 20) ? std::optional<QColor>(QColor(10, 120, 200)) : std::nullopt;
        };
        studio.trigger("act_border_colour");
        QCOMPARE(*studio.selected().line, Json::object({{"kind", "solid"}, {"rgb", Json::array({10, 120, 200})}}));
        studio.trigger("act_frame_numbers");
        QVERIFY(studio.canvas()->show_frame_numbers);
        studio.trigger("act_frame_selection");
        QVERIFY(studio.canvas()->selection().has_value());
        QVERIFY(studio.canvas()->selection()->outline.size() >= 4);
        QCOMPARE(w->last_notice(), QStringLiteral("コマの形を選択範囲にしました"));
        // 結合: a panel of the second cut, back with its neighbour (its split undone); 消す: one panel gone, the others
        // kept
        const core::Frame* nested = nullptr;
        for (const core::Frame* leaf : studio.page().leaf_frames()) {
            if (studio.page().parent_of(leaf->id) != &studio.page().frames[0]) nested = leaf;
        }
        QVERIFY(nested != nullptr);
        studio.choose(*nested);
        studio.trigger("act_merge");
        QCOMPARE(studio.leaves(), std::size_t{2});
        const auto kept = studio.page().leaf_frames().back()->id;
        studio.choose(*studio.page().leaf_frames().front());
        studio.trigger("act_delete_frame");
        QCOMPARE(studio.leaves(), std::size_t{1});
        QCOMPARE(studio.page().leaf_frames().front()->id, kept);
    }

    void theTemplates() {
        Studio studio;
        MainWindow* w = studio.window.get();
        // 今のコマ割りをテンプレートに残す
        studio.choose(*studio.page().leaf_frames().front());
        studio.trigger("act_split_h");
        studio.answers->responder->get_text = [](const QString& title, const QString&, const QString& suggested) {
            return title == QStringLiteral("テンプレートに残す") && suggested == QStringLiteral("試し 1 ページ") ? std::optional<QString>(QStringLiteral("わたしの二段"))
                                                                                                         : std::nullopt;
        };
        studio.trigger("act_save_template");
        const auto mine = genko::app::templates::mine();
        QVERIFY(std::any_of(mine.begin(), mine.end(), [](const auto& t) { return t.key == "わたしの二段"; }));
        QVERIFY(w->last_notice().startsWith(QStringLiteral("コマ割りを「わたしの二段」として残しました")));
        // テンプレートでコマを割る: page 2 (blank) with the first built-in template
        studio.trigger("act_next");
        bool mine_first = false;
        studio.answers->responder->exec = [&mine_first](QDialog* dialog) {
            auto* d = qobject_cast<genko::app::TemplateDialog*>(dialog);
            if (d == nullptr) return 0;
            mine_first = d->list()->item(0)->text() == QStringLiteral("自分: わたしの二段");  // (a person's own layouts first)
            for (int row = 0; row < d->list()->count(); ++row) {
                if (d->list()->item(row)->text() == QStringLiteral("4段の標準的な割り")) d->list()->setCurrentRow(row);
            }
            d->choose();
            return d->result();
        };
        studio.trigger("act_template");
        QVERIFY(mine_first);
        QCOMPARE(studio.leaves(), std::size_t{6});
        const std::size_t made = studio.leaves();
        // one undo takes the whole template back
        studio.trigger("act_undo");
        QCOMPARE(studio.leaves(), std::size_t{1});
        studio.trigger("act_redo");
        QCOMPARE(studio.leaves(), made);
    }

    void thePageCommands() {
        Studio studio;
        MainWindow* w = studio.window.get();
        const std::string first = studio.doc().pages[0]->id;
        const std::string second = studio.doc().pages[1]->id;
        studio.trigger("act_add_page");
        QCOMPARE(studio.doc().pages.size(), std::size_t{4});
        QCOMPARE(w->page_index(), 1);  // (the new page, after the first)
        QVERIFY(studio.doc().pages[1]->id != second);
        studio.trigger("act_dup_page");
        QCOMPARE(studio.doc().pages.size(), std::size_t{5});
        // このページを消す…: asked first (no: kept)
        studio.trigger("act_del_page");
        QCOMPARE(studio.doc().pages.size(), std::size_t{5});
        QVERIFY(studio.answers->asked.last().contains(QStringLiteral("ページを消しますか？")));
        studio.answers->responder->question = [](const QString&, const QString& text) { return text.contains(QStringLiteral("ページを消しますか？")); };
        studio.trigger("act_del_page");
        QCOMPARE(studio.doc().pages.size(), std::size_t{4});
        // the order: the page goes with the person's view
        w->select_page(0);
        studio.trigger("act_page_down");
        QCOMPARE(studio.doc().pages[1]->id, first);
        QCOMPARE(w->page_index(), 1);
        studio.trigger("act_page_up");
        QCOMPARE(studio.doc().pages[0]->id, first);
        QCOMPARE(w->page_index(), 0);
        // ネーム完了 is for books made with agents: hidden here
        QVERIFY(!studio.act("act_name_ok")->isVisible());
        studio.trigger("act_name_ok");  // (the person's own approval of the name: allowed for a person)
        QVERIFY(studio.page().name_ok);
    }

    void theNameIsApprovedInAnAgentsBook() {
        Studio studio(2, /*agent=*/true);
        QVERIFY(studio.act("act_name_ok")->isVisible());
        QVERIFY(!studio.page().name_ok);
        studio.trigger("act_name_ok");
        QVERIFY(studio.page().name_ok);
        QVERIFY(wait_for([&] { return studio.session->status().kind == SaveKind::Saved; }));
        QVERIFY(read_book(studio.book).page(0).name_ok);
    }

    void theBooksAndWindows() {
        Studio studio;
        MainWindow* w = studio.window.get();
        // 保存: written now
        w->choose_tool(QStringLiteral("pen"));
        const QPointF c = studio.canvas()->seen_mm().center();
        inject::mouse_stroke(studio.canvas(), {c, c + QPointF(10, 3)});
        studio.trigger("act_save");
        QCOMPARE(studio.session->status().kind, SaveKind::Saved);
        QCOMPARE(ink_strokes(read_book(studio.book)), std::size_t{1});
        QVERIFY(w->last_notice().startsWith(QStringLiteral("保存しました: ")));
        // 開く…: another book in a new tab
        const fs::path other = path_of(studio.dir.filePath("other.genko"));
        write_book(other, new_doc(2, "別の本"));
        studio.answers->responder->existing_dir = [other](const QString& caption, const QString&) {
            return caption == QStringLiteral("原稿（.genko のフォルダ）を開く") ? qpath(other) : QString();
        };
        studio.trigger("act_open");
        QVERIFY(wait_for([&] { return w->documents().size() == 2; }));
        QCOMPARE(w->book().title, std::string("別の本"));
        QCOMPARE(w->doc_tabs()->count(), 2);
        // 次の原稿 / 前の原稿
        studio.trigger("act_next_doc");
        QCOMPARE(w->current_document(), 0);
        QCOMPARE(w->book().title, std::string("試し"));
        studio.trigger("act_prev_doc");
        QCOMPARE(w->current_document(), 1);
        // 新しい原稿…: the dialog makes the book; it opens in a tab
        studio.answers->responder->exec = [&studio](QDialog* dialog) {
            auto* d = qobject_cast<genko::app::NewProjectDialog*>(dialog);
            if (d == nullptr) return 0;
            d->title->setText(QStringLiteral("新しい本"));
            d->folder->setText(studio.dir.path());
            return d->create() ? static_cast<int>(QDialog::Accepted) : 0;
        };
        studio.trigger("act_new");
        QVERIFY(wait_for([&] { return w->documents().size() == 3; }));
        QCOMPARE(w->book().title, std::string("新しい本"));
        QVERIFY(w->session().path().has_value());
        QVERIFY(fs::exists(*w->session().path() / "project.json"));
        // 別の場所に保存…: a copy with a new book id; the work goes on there
        const std::string old_id = w->book().book_id;
        const fs::path copy = path_of(studio.dir.filePath("写し"));
        studio.answers->responder->save_path = [copy](const QString& caption, const QString&) {
            return caption == QStringLiteral("原稿を保存する場所") ? qpath(copy) : QString();
        };
        studio.trigger("act_save_as");
        const fs::path copied = path_of(studio.dir.filePath("写し.genko"));
        QCOMPARE(*w->session().path(), copied);
        QVERIFY(read_book(copied).book_id != old_id);
        QCOMPARE(w->session().status().kind, SaveKind::Saved);
        // 閉じる: the tab (its book saved: nothing asked)
        studio.trigger("act_close");
        QCOMPARE(w->documents().size(), std::size_t{2});
        // 新しいウィンドウ（同じ原稿）: the same session in another window
        const std::size_t windows = genko::app::documents::windows().size();
        studio.trigger("act_new_window");
        const auto all = genko::app::documents::windows();
        QCOMPARE(all.size(), windows + 1);
        MainWindow* second = all.back();
        QCOMPARE(second->session_ptr(), w->session_ptr());
        second->close();
        QCOMPARE(genko::app::documents::windows().size(), windows);
        // Genko を終わる: every window (each book saved: nothing asked)
        studio.trigger("act_quit");
        QVERIFY(w->closed());
        QVERIFY(!studio.answers->asked.contains(QStringLiteral("genko::app::CloseGuard")));
    }
};

QTEST_MAIN(TestGuiActions)
#include "test_gui_actions.moc"
