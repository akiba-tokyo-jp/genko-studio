// 素材・トーン・効果線, the view's extras and the layer commands (M3④-5, Python's genko/app/material_panel.py and
// main.py): the commands as Python's window has them; the effect tool putting an effect line where clicked (its centre,
// in its panel, inside the selection), its centre dragged, its settings edited in the 効果線 tab, kept inside or clear
// of the selection, shaped by a pen line, made pen lines and deleted; tones put on the selection or the chosen panel or
// where clicked, and the トーン tab editing the tone layer drawn on (pattern, density, dot shape, the screen's shift,
// gradient) and how the eraser scrapes it; the material tool putting the material chosen; the person's library from
// the 素材 tab's buttons (a picture added, a selection kept as a part, renamed, tagged, deleted, a folder, a pack written
// and read); and the layer and view commands. Every change is an op, recorded as the window applies it.

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QScrollArea>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QToolButton>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QtTest>

#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>

#include "app/brush_panel.hpp"
#include "app/canvas.hpp"
#include "app/inject.hpp"
#include "app/layer_panel.hpp"
#include "app/main_window.hpp"
#include "app/material_panel.hpp"
#include "app/material_tabs.hpp"
#include "app/scanner.hpp"
#include "app/subview.hpp"
#include "app/tiles.hpp"
#include "app/tool_settings.hpp"
#include "app/wording.hpp"
#include "core/pynum.hpp"
#include "gui_support.hpp"
#include "render/materials.hpp"
#include "render/png.hpp"

using namespace genko;
using core::Json;
namespace inject = genko::app::inject;

namespace {

core::Document book_doc() {
    core::Document doc = core::new_episode("素材", core::Num(1), 2, core::PageSpec::custom(120, 160, 100, 140, 1, 2, 2, 2, 2, 72, "mono"));
    doc.edit_page(0).name_ok = true;
    doc.edit_page(1).name_ok = true;
    return doc;
}

struct Studio {
    QTemporaryDir tmp;
    QTemporaryDir config;  // (the person's material library of this test alone)
    QByteArray previous_config;
    std::shared_ptr<app::Session> session;
    std::unique_ptr<app::MainWindow> window;
    gui_test::Answers answers;
    std::vector<Json> ops;  // every op applied, in order
    Studio() {
        (void)gui_test::config_folder();
        previous_config = qgetenv("GENKO_CONFIG_DIR");
        qputenv("GENKO_CONFIG_DIR", config.path().toUtf8());
        const auto path = gui_test::path_of(tmp.path() + "/book");
        gui_test::write_book(path, book_doc());
        session = app::Session::open(path, gui_test::quick(gui_test::path_of(tmp.path() + "/recovery")));
        window = std::make_unique<app::MainWindow>(session);
        window->resize(1300, 900);
        window->show();
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
    app::MaterialPanel& panel() const { return *window->materials(); }
    const core::Page& page() const { return window->book().page(static_cast<std::size_t>(window->page_index())); }
    void trigger(const char* name) const { window->action(QString::fromLatin1(name))->trigger(); }
    Json last() const { return ops.empty() ? Json() : ops.back(); }
    std::filesystem::path config_dir() const { return gui_test::path_of(config.path()); }
    QStringList menus_of(QAction* wanted) const {
        QStringList found;
        std::function<void(QMenu*, const QString&)> walk = [&](QMenu* menu, const QString& path) {
            for (QAction* a : menu->actions()) {
                if (a == wanted) found << path;
                if (a->menu() != nullptr) walk(a->menu(), path + QStringLiteral("/") + a->menu()->title());
            }
        };
        for (QAction* top : window->menuBar()->actions())
            if (top->menu() != nullptr) walk(top->menu(), top->menu()->title());
        return found;
    }
    QAction* effect_action(const char* label) const {
        for (QAction* a : window->findChildren<QAction*>())
            if (a->objectName() == QStringLiteral("cmd:%1").arg(QString::fromUtf8(label))) return a;
        return nullptr;
    }
    const Json* effect(const std::string& id) const {
        for (const Json& e : page().effects)
            if (e.value("id", std::string()) == id) return &e;
        return nullptr;
    }
};

bool close_to(const Json& v, double want, double within = 0.05) { return v.is_number() && std::abs(v.get<double>() - want) <= within; }

QDoubleSpinBox* effect_spin(app::MaterialPanel& panel, const char* key) {
    const auto found = panel.effect_fields.find(key);
    return found == panel.effect_fields.end() ? nullptr : qobject_cast<QDoubleSpinBox*>(found->second);
}

void type_into(QAbstractSpinBox* spin, double value) {
    if (auto* d = qobject_cast<QDoubleSpinBox*>(spin)) d->setValue(value);
    if (auto* i = qobject_cast<QSpinBox*>(spin)) i->setValue(static_cast<int>(value));
    emit spin->editingFinished();
}

struct Expected {
    const char* attribute;
    const char* text;
    const char* key;
    const char* tip;
    bool checkable;
    const char* menu;
};

}  // namespace

class TestGuiEffects : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QVERIFY(gui_test::config_folder().isValid()); }

    void commandsAsPythonsWindowHasThem() {
        Studio s;
        const std::vector<Expected> expected{
            {"act_effect", "効果線", "K", "コマの中をクリックすると、選んだ効果線（集中線など）が入る。中心の＋をドラッグで動かす", true, "ツール"},
            {"act_stamp", "素材を置く", "", "素材パネルで選んだ素材を、クリックした所に置く", true, "ツール"},
            {"act_tone_here", "選択範囲・選んだコマにトーンを貼る", "Ctrl+Shift+T", "素材パネルで選んだトーン（なければ網点 60 線 30%）を貼ります", false, "ツール/トーン・効果線"},
            {"act_tone_click", "クリックした所にトーンを貼る", "", "線で囲まれた所をクリックすると、そこにトーンが入ります", false, "ツール/トーン・効果線"},
            {"act_effect_within", "選択範囲の中にだけ描く", "", "選んだ効果線を、投げ縄・長方形の選択範囲の中にだけ描きます", false, "ツール/トーン・効果線"},
            {"act_effect_avoid", "選択範囲を避ける", "", "選んだ効果線を、選択範囲（顔や人物を投げ縄で囲む）の手前で止めます。線は止まる所で細くなります", false,
             "ツール/トーン・効果線"},
            {"act_effect_clear", "避ける範囲を外す", "", "選んだ効果線の「中にだけ描く」「避ける」を外して、コマ全体に描きます", false, "ツール/トーン・効果線"},
            {"act_materials", "素材パネルを開く", "", "", false, "ツール/トーン・効果線"},
            {"act_view_flip_v", "上下反転して見る", "Shift+H", "表示だけを上下反転します。原稿は変わりません", true, "表示"},
            {"act_onion", "前のページを透かす（オニオンスキン）", "", "", false, "表示"},
            {"act_screen_dots", "網点を画面で見る", "", "トーンを、印刷と同じ網点で表示します（拡大すると点が見えます）。切ると平らな灰色で速く描きます", true, "表示"},
            {"act_reset_shape", "選んだコマの形を元に戻す", "", "", false, "ページ/コマ"},
            {"act_layer_pen", "新しいペンのレイヤー", "Ctrl+Shift+N", "", false, "レイヤー"},
            {"act_layer_paint", "新しいペイントのレイヤー", "", "", false, "レイヤー"},
            {"act_layer_folder", "新しいフォルダ", "", "", false, "レイヤー"},
            {"act_layer_dup", "レイヤーを複製", "Ctrl+J", "", false, "レイヤー"},
            {"act_layer_merge", "下のレイヤーと結合", "Ctrl+Shift+E", "", false, "レイヤー"},
            {"act_layer_delete", "レイヤーを消す", "", "", false, "レイヤー"},
            {"act_layer_up", "レイヤーを前へ", "Ctrl+]", "", false, "レイヤー"},
            {"act_layer_down", "レイヤーを後ろへ", "Ctrl+[", "", false, "レイヤー"},
            {"act_layer_draft", "下描きにする（書き出さない）／戻す", "", "", false, "レイヤー"},
            {"act_import_scan", "スキャン画像を線画にして取り込む…", "",
             "紙に描いた絵の画像を新しいレイヤーに置き、線だけを抜き出します（強さ・下描きの青を消す・ゴミ取りを見ながら決める）", false, "ファイル"},
            {"act_scanner", "スキャナーから取り込む…", "", "スキャナーで読んだ紙を新しいレイヤーに置き、線だけを抜き出します（Windows・Linux）", false, "ファイル"},
        };
        for (const Expected& e : expected) {
            QAction* a = s.window->action(QString::fromLatin1(e.attribute));
            QVERIFY2(a != nullptr, e.attribute);
            QCOMPARE(a->text(), QString::fromUtf8(e.text));
            QCOMPARE(a->shortcut(), QKeySequence(QString::fromLatin1(e.key)));
            QCOMPARE(a->statusTip(), QString::fromUtf8(e.tip));
            QCOMPARE(a->isCheckable(), e.checkable);
            QVERIFY2(s.menus_of(a).contains(QString::fromUtf8(e.menu)), e.attribute);
        }
        // the effect kinds, in the menu and as pictures on the effect tool's page (drawn when the tool is first chosen)
        for (const char* label : {"集中線", "流線", "ウニフラッシュ", "ベタフラッシュ"}) {
            QAction* a = s.effect_action(label);
            QVERIFY2(a != nullptr, label);
            QVERIFY(s.menus_of(a).contains(QStringLiteral("ツール/トーン・効果線")));
            QVERIFY(a->icon().isNull());
        }
        QVERIFY(s.window->tool_actions().contains(QStringLiteral("effect")) && s.window->tool_actions().contains(QStringLiteral("stamp")));
        s.trigger("act_effect");
        QCOMPARE(s.canvas()->tool(), QStringLiteral("effect"));
        for (const char* label : {"集中線", "流線", "ウニフラッシュ", "ベタフラッシュ"}) QVERIFY2(!s.effect_action(label)->icon().isNull(), label);
        QWidget* page = s.window->tool_settings()->page_for(QStringLiteral("effect"));
        QVERIFY(page != nullptr);
        QCOMPARE(page->findChildren<QToolButton*>(QStringLiteral("effectTile")).size(), 4);
        QVERIFY(s.window->tool_settings()->page_for(QStringLiteral("stamp")) != nullptr);
        // the materials panel: three pages
        auto* dock = s.window->findChild<QDockWidget*>(QStringLiteral("素材"));
        QVERIFY(dock != nullptr);
        // (as Python's side panels: it scrolls on a small screen, a tab in the right row that closes when done with)
        QVERIFY(qobject_cast<QScrollArea*>(dock->widget()) != nullptr);
        QVERIFY(dock->features().testFlag(QDockWidget::DockWidgetClosable));
        QCOMPARE(dock->minimumWidth(), 200);
        QCOMPARE(s.panel().tabs->count(), 3);
        QCOMPARE(s.panel().tabs->tabText(0), QStringLiteral("素材"));
        QCOMPARE(s.panel().tabs->tabText(1), QStringLiteral("トーン"));
        QCOMPARE(s.panel().tabs->tabText(2), QStringLiteral("効果線"));
        s.trigger("act_materials");
        QVERIFY(dock->isVisible());
        QVERIFY(s.window->tabifiedDockWidgets(dock).contains(s.window->findChild<QDockWidget*>(QStringLiteral("ページ"))));
    }

    void effectLinesPutMovedAndEdited() {
        Studio s;
        auto& panel = s.panel();
        // 集中線 from the menu: the effect tool; a click puts one centred there, in its panel
        s.effect_action("集中線")->trigger();
        QCOMPARE(s.canvas()->tool(), QStringLiteral("effect"));
        QCOMPARE(s.window->effect_kind(), std::string("focus"));
        inject::click_mm(s.canvas(), QPointF(50.123, 70.456));
        Json op = s.last();
        QCOMPARE(op["op"], Json("add_effect"));
        QCOMPARE(op["kind"], Json("focus"));
        QVERIFY(close_to(op["params"]["center"][0], 50.12, 0.03) && close_to(op["params"]["center"][1], 70.46, 0.03));
        const core::Frame* frame = s.page().frame_at(core::Num(50.12), core::Num(70.46));
        QCOMPARE(op["frame_id"], frame != nullptr ? Json(frame->id) : Json());
        QVERIFY(!op["params"].contains("within"));
        const std::string id = op["id"].get<std::string>();
        QCOMPARE(s.canvas()->selected_effect_id, std::optional<std::string>(id));
        auto* dock = s.window->findChild<QDockWidget*>(QStringLiteral("素材"));
        QVERIFY(dock->isVisible());
        QCOMPARE(panel.effects->count(), 1);
        QCOMPARE(panel.effects->currentItem()->text(), QStringLiteral("集中線"));
        // its centre dragged
        inject::mouse_stroke(s.canvas(), {QPointF(50.12, 70.46), QPointF(55, 72), QPointF(60, 75)});
        op = s.last();
        QCOMPARE(op["op"], Json("edit_effect"));
        QVERIFY(close_to(op["params"]["center"][0], 60, 0.05) && close_to(op["params"]["center"][1], 75, 0.05));
        // the 効果線 tab's fields
        auto* count = effect_spin(panel, "count");
        QVERIFY(count != nullptr);
        QCOMPARE(count->value(), 90.0);
        type_into(count, 120);
        QCOMPARE(s.last()["params"], Json({{"count", 120}}));
        QVERIFY(s.last()["params"]["count"].is_number_integer());
        auto* inner = effect_spin(panel, "inner_r");
        QVERIFY(inner != nullptr);
        type_into(inner, 12);
        QCOMPARE(s.last()["params"]["inner"][0], Json(12.0));
        QVERIFY(s.last()["params"]["inner"][1].is_number());
        type_into(effect_spin(panel, "length_mm"), 0);
        QCOMPARE(s.last()["params"], Json({{"length_mm", nullptr}}));  // (0: to the edge)
        type_into(effect_spin(panel, "width_mm"), 1.25);
        QCOMPARE(s.last()["params"], Json({{"width_mm", 1.25}}));
        QVERIFY(effect_spin(panel, "bundle") != nullptr && effect_spin(panel, "jitter_width") != nullptr);
        auto* taper = qobject_cast<QComboBox*>(panel.effect_fields.at("taper"));
        QCOMPARE(taper->currentData().toString(), QStringLiteral("in"));
        taper->setCurrentIndex(taper->findData(QStringLiteral("none")));
        emit taper->activated(taper->currentIndex());
        QCOMPARE(s.last()["params"], Json({{"taper", false}}));
        // typed and ended with Return while the field has the focus: one edit only (the fields are made again under
        // the focus, and the one losing it sends nothing more)
        s.window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(s.window.get()));
        auto* width = effect_spin(panel, "width_mm");
        width->setFocus();
        QApplication::processEvents();
        const std::size_t edits = s.ops.size();
        width->selectAll();
        QTest::keyClicks(width, QStringLiteral("0.75"));
        QTest::keyClick(width, Qt::Key_Return);
        QApplication::processEvents();
        s.canvas()->setFocus();
        QApplication::processEvents();
        QCOMPARE(s.ops.size(), edits + 1);
        QCOMPARE(s.last()["params"], Json({{"width_mm", 0.75}}));
        // inside the selection only, clear of it, both taken away
        s.trigger("act_marquee");
        inject::mouse_stroke(s.canvas(), {QPointF(30, 40), QPointF(50, 60), QPointF(70, 90)});
        QVERIFY(s.canvas()->selection().has_value());
        s.trigger("act_effect_within");
        QCOMPARE(s.last()["params"]["within"].size(), std::size_t(4));
        s.trigger("act_effect_avoid");
        QCOMPARE(s.last()["params"]["avoid"].size(), std::size_t(1));
        QVERIFY(s.last()["params"]["avoid"][0].contains("path"));
        s.trigger("act_effect_avoid");
        QCOMPARE(s.last()["params"]["avoid"].size(), std::size_t(2));  // (kept, and one more)
        s.trigger("act_effect_clear");
        QCOMPARE(s.last()["params"], Json({{"within", nullptr}, {"avoid", nullptr}}));
        // a new effect with a selection is drawn inside it
        s.effect_action("流線")->trigger();
        inject::click_mm(s.canvas(), QPointF(40, 50));
        op = s.last();
        QCOMPARE(op["kind"], Json("speed"));
        QVERIFY(!op["params"].contains("center"));  // (流線 has no centre)
        QCOMPARE(op["params"]["within"].size(), std::size_t(4));
        const std::string speed = op["id"].get<std::string>();
        s.trigger("act_deselect");
        // its path drawn with the pen: the line is the effect's, not ink
        s.panel().select_effect(speed);
        auto* draw = s.panel().findChild<QPushButton*>(QStringLiteral("effect:draw"));
        QVERIFY(draw != nullptr);
        QCOMPARE(draw->text(), QStringLiteral("描いた線に沿わせる"));
        const std::size_t strokes_before = s.window->target_layer()->strokes->items.size();
        draw->click();
        QCOMPARE(s.canvas()->tool(), QStringLiteral("pen"));
        inject::mouse_stroke(s.canvas(), {QPointF(20, 30), QPointF(40, 35), QPointF(60, 45), QPointF(80, 60)});
        op = s.last();
        QCOMPARE(op["op"], Json("edit_effect"));
        QCOMPARE(op["id"], Json(speed));
        QVERIFY(op["params"]["path"].size() >= 2);
        QCOMPARE(s.window->target_layer()->strokes->items.size(), strokes_before);
        QVERIFY(s.panel().findChild<QPushButton*>(QStringLiteral("effect:undraw")) != nullptr);
        s.panel().findChild<QPushButton*>(QStringLiteral("effect:undraw"))->click();
        QCOMPARE(s.last()["params"], Json({{"path", nullptr}}));
        // made pen lines on the layer drawn on; deleted
        s.panel().select_effect(id);
        s.panel().effect_to_layer();
        QCOMPARE(s.last()["op"], Json("effect_to_layer"));
        QCOMPARE(s.last()["id"], Json(id));
        s.panel().select_effect(speed);
        s.panel().delete_effect();
        QCOMPARE(s.last()["op"], Json("delete_effect"));
        QCOMPARE(s.last()["id"], Json(speed));
        QVERIFY(s.effect(speed) == nullptr);
        QVERIFY(!s.canvas()->selected_effect_id);
        // nothing chosen: the commands say how
        s.trigger("act_effect_within");
        QVERIFY(s.window->last_notice().startsWith(QStringLiteral("先に効果線を選びます")));
    }

    void tonesPutAndEdited() {
        Studio s;
        auto& panel = s.panel();
        s.window->show_dock(QStringLiteral("素材"));
        QVERIFY(!panel.tone_box->isEnabled());  // (not a tone layer)
        // no selection and no panel chosen: the material tool waits for a click inside lines
        s.trigger("act_tone_here");
        QCOMPARE(s.canvas()->tool(), QStringLiteral("stamp"));
        QVERIFY(s.window->pending_material().has_value());
        QCOMPARE((*s.window->pending_material())["id"], Json("dot-60-30"));
        s.window->brush_panel()->gap->setValue(1.7);
        inject::click_mm(s.canvas(), QPointF(40, 60));
        Json op = s.last();
        QCOMPARE(op["op"], Json("stamp_material"));
        QCOMPARE(op["material_id"], Json("dot-60-30"));
        QVERIFY(close_to(op["at"]["x_mm"], 40) && close_to(op["at"]["y_mm"], 60));
        QCOMPARE(op["at"]["gap_mm"], Json(1.7));  // (the brush panel's 隙間を閉じる: self.brush.gap)
        // the tone made is the layer drawn on: the トーン tab edits it
        const std::string tone = op["id"].get<std::string>();
        QCOMPARE(s.window->target_layer()->id, tone);
        QVERIFY(panel.tone_box->isEnabled());
        QCOMPARE(panel.pattern->currentData().toString(), QStringLiteral("dot"));
        QCOMPARE(panel.density->value(), 30);
        type_into(panel.density, 45);
        QCOMPARE(s.last(), Json({{"op", "set_tone"}, {"page", 1}, {"id", tone}, {"density", 0.45}}));
        type_into(panel.lpi, 85);
        QCOMPARE(s.last()["lpi"], Json(85.0));
        type_into(panel.angle, 30);
        QCOMPARE(s.last()["angle"], Json(30.0));
        panel.dot_shape->setCurrentIndex(panel.dot_shape->findData(QStringLiteral("square")));
        emit panel.dot_shape->activated(panel.dot_shape->currentIndex());
        QCOMPARE(s.last()["dot_shape"], Json("square"));
        panel.pattern->setCurrentIndex(panel.pattern->findData(QStringLiteral("check")));
        emit panel.pattern->activated(panel.pattern->currentIndex());
        QCOMPARE(s.last()["pattern"], Json("check"));
        QVERIFY(panel.scale->isEnabled());
        type_into(panel.scale, 4.5);
        QCOMPARE(s.last()["scale_mm"], Json(4.5));
        // the screen's shift, kept a moment after typing (and at once on Enter)
        const std::size_t before = s.ops.size();
        panel.off_x->setValue(1.25);
        QCOMPARE(s.ops.size(), before);
        QTRY_COMPARE_WITH_TIMEOUT(s.ops.size(), before + 1, 3000);
        QCOMPARE(s.last()["offset_mm"], Json::array({1.25, 0.0}));
        // a gradient: its fields come on; start and end the same are made the density and 0
        panel.gradient->setCurrentIndex(panel.gradient->findData(QStringLiteral("linear")));
        emit panel.gradient->activated(panel.gradient->currentIndex());
        QVERIFY(panel.g_angle->isEnabled());
        QCOMPARE(s.last()["gradient"]["shape"], Json("linear"));
        QCOMPARE(s.last()["gradient"]["angle"], Json(90.0));
        type_into(panel.g_end, 10);
        QCOMPARE(s.last()["gradient"]["end"], Json(0.1));
        panel.gradient->setCurrentIndex(panel.gradient->findData(QString()));
        emit panel.gradient->activated(panel.gradient->currentIndex());
        QCOMPARE(s.last()["gradient"], Json());
        // the eraser scrapes it, softly when asked
        panel.soft->setChecked(true);
        s.window->choose_tool(QStringLiteral("eraser"));
        inject::mouse_stroke(s.canvas(), {QPointF(30, 50), QPointF(40, 55), QPointF(50, 60)});
        QCOMPARE(s.last()["op"], Json("erase"));
        QCOMPARE(s.last()["soft"], Json(true));
        // with a selection: on it at once
        s.trigger("act_marquee");
        inject::mouse_stroke(s.canvas(), {QPointF(20, 20), QPointF(40, 40)});
        s.trigger("act_tone_here");
        op = s.last();
        QCOMPARE(op["op"], Json("stamp_material"));
        QVERIFY(op["area"].contains("poly"));
        QVERIFY(s.window->last_notice().contains(QStringLiteral("を貼りました")));
        // クリックした所にトーンを貼る (on the next page: this one's tone is everywhere, and a click on it is on its dots)
        s.trigger("act_deselect");
        s.window->go_to_page(2);
        s.trigger("act_tone_click");
        QCOMPARE(s.canvas()->tool(), QStringLiteral("stamp"));
        inject::click_mm(s.canvas(), QPointF(60, 100));
        QVERIFY2(s.last().contains("at"), qPrintable(s.window->last_error() + QStringLiteral(" / ") + s.window->last_notice()));
    }

    void materialsPutWhereClicked() {
        Studio s;
        auto& panel = s.panel();
        s.window->show_dock(QStringLiteral("素材"));
        // 貼る with nothing chosen
        panel.use();
        QCOMPARE(s.window->last_notice(), QStringLiteral("先に素材を選びます"));
        // a part (lines): the material tool, then a click on the layer drawn on
        panel.select_material(QStringLiteral("mark-汗"));
        QCOMPARE(panel.current_material()->value("id", std::string()), std::string("mark-汗"));
        panel.use();
        QCOMPARE(s.canvas()->tool(), QStringLiteral("stamp"));
        QVERIFY(s.window->last_notice().contains(QStringLiteral("置きたい所をクリックします")));
        inject::click_mm(s.canvas(), QPointF(33.333, 44.444));
        Json op = s.last();
        QCOMPARE(op["op"], Json("stamp_material"));
        QCOMPARE(op["material_id"], Json("mark-汗"));
        QCOMPARE(op["layer_id"], Json(s.window->target_layer()->id));
        QCOMPARE(op["x_mm"], Json(33.33));
        QCOMPARE(op["y_mm"], Json(44.44));
        // an effect material: in the chosen panel at once, else where clicked (centred there)
        panel.select_material(QStringLiteral("focus"));
        panel.use();
        QCOMPARE(s.canvas()->tool(), QStringLiteral("stamp"));
        inject::click_mm(s.canvas(), QPointF(50, 60));
        op = s.last();
        QCOMPARE(op["material_id"], Json("focus"));
        QVERIFY(op.contains("frame_id") && !op.contains("layer_id"));
        // a brush material: chosen at once
        panel.select_material(QStringLiteral("brush-雨ブラシ"));
        panel.use();
        QCOMPARE(s.last()["op"], Json("stamp_material"));
        QCOMPARE(s.canvas()->tool(), QStringLiteral("pen"));
        // the pages' search: a search looks in every folder
        auto* folder = panel.browser->folder_box();
        folder->setCurrentIndex(folder->findData(QStringLiteral("漫符")));
        const int marks = panel.browser->list()->count();
        QVERIFY(marks > 1);
        panel.browser->search_box()->setText(QStringLiteral("集中線"));
        QVERIFY(panel.browser->list()->count() >= 1);
        QVERIFY(panel.browser->list()->item(0)->data(Qt::UserRole).toString().startsWith(QStringLiteral("focus")));
        panel.browser->search_box()->clear();
        QCOMPARE(panel.browser->list()->count(), marks);
    }

    void theLibraryFromItsButtons() {
        Studio s;
        auto& panel = s.panel();
        s.window->show_dock(QStringLiteral("素材"));
        const auto config = s.config_dir();
        // a picture added (into the folder chosen, or 画像)
        const QString picture = s.tmp.path() + QStringLiteral("/青い四角.png");
        render::save_png(render::Image::create("RGBA", render::Size{40, 20}, render::Ink{20, 40, 200, 255}), gui_test::path_of(picture));
        s.answers.responder->open_path = [&](const QString&, const QString&) { return picture; };
        panel.import_image();
        Json user = render::materials::user_materials(config);
        QCOMPARE(user.size(), std::size_t(1));
        QCOMPARE(user[0]["name"], Json("青い四角"));
        QCOMPARE(user[0]["folder"], Json("画像"));
        QCOMPARE(user[0]["aspect"], Json(0.5));
        const QString image_id = QString::fromStdString(user[0]["id"].get<std::string>());
        QCOMPARE(panel.browser->current_id(), image_id);
        QVERIFY(s.window->last_notice().contains(QStringLiteral("を素材にしました")));
        // its preview, drawn when it comes into view
        QTRY_VERIFY_WITH_TIMEOUT(!panel.browser->list()->currentItem()->icon().isNull(), 5000);
        // a selection's lines kept as a part
        const std::string ink = gui_test::ink_of(s.page())->id;
        s.window->set_target_layer(ink);
        QVERIFY(s.window->apply_ops(Json::array({Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", ink},
                                                      {"points", Json::parse("[[30, 30], [40, 35], [50, 30]]")}}})));
        s.trigger("act_marquee");
        inject::mouse_stroke(s.canvas(), {QPointF(25, 25), QPointF(55, 40)});
        s.answers.responder->get_text = [](const QString& title, const QString&, const QString&) {
            return title == QStringLiteral("素材に登録") ? std::optional<QString>(QStringLiteral("  山  ")) : std::nullopt;
        };
        panel.register_selection();
        user = render::materials::user_materials(config);
        QCOMPARE(user.size(), std::size_t(2));
        QCOMPARE(user[1]["name"], Json("山"));
        QCOMPARE(user[1]["kind"], Json("lines"));
        QCOMPARE(user[1]["items"]["strokes"].size(), std::size_t(1));
        const QString part_id = QString::fromStdString(user[1]["id"].get<std::string>());
        QCOMPARE(panel.browser->current_id(), part_id);
        QTRY_VERIFY_WITH_TIMEOUT(!panel.browser->list()->currentItem()->icon().isNull(), 5000);  // (its lines drawn)
        // renamed and moved to another folder; tagged
        s.answers.responder->get_text = [](const QString& title, const QString&, const QString&) {
            if (title == QStringLiteral("素材の名前")) return std::optional<QString>(QStringLiteral("遠い山"));
            if (title == QStringLiteral("素材のタグ")) return std::optional<QString>(QStringLiteral("山、背景, 風景"));
            return std::optional<QString>();
        };
        s.answers.responder->get_item = [](const QString&, const QString&, const QStringList&, int, bool) {
            return std::optional<QString>(QStringLiteral("背景"));
        };
        panel.rename();
        panel.edit_tags();
        const Json part = render::materials::get_material(config, part_id.toStdString());
        QCOMPARE(part["name"], Json("遠い山"));
        QCOMPARE(part["folder"], Json("背景"));
        QCOMPARE(part["tags"], Json::array({"山", "背景", "風景"}));
        // a built-in one cannot be renamed or deleted
        panel.select_material(QStringLiteral("dot-60-30"));
        panel.rename();
        QCOMPARE(s.window->last_notice(), QStringLiteral("名前を変えられるのは自分で登録した素材です"));
        panel.remove();
        QCOMPARE(s.window->last_notice(), QStringLiteral("最初から入っている素材は消せません"));
        // a folder made
        s.answers.responder->get_text = [](const QString& title, const QString&, const QString&) {
            return title == QStringLiteral("フォルダを作る") ? std::optional<QString>(QStringLiteral(" 空 ")) : std::nullopt;
        };
        panel.new_folder();
        QCOMPARE(panel.browser->folder(), QStringLiteral("空"));
        QCOMPARE(panel.browser->list()->count(), 0);
        // the person's own written as a pack, and read back
        panel.browser->choose_folder(QString());
        const QString pack = s.tmp.path() + QStringLiteral("/パック.zip");
        s.answers.responder->save_path = [&](const QString&, const QString&) { return pack; };
        panel.export_pack();
        QVERIFY(QFileInfo::exists(pack));
        QVERIFY(s.window->last_notice().contains(QStringLiteral("素材 2 個を書き出しました")));
        s.answers.responder->open_path = [&](const QString&, const QString&) { return pack; };
        panel.import_pack(true);
        QVERIFY(s.window->last_notice().contains(QStringLiteral("素材を 2 個読み込みました")));
        QCOMPARE(render::materials::user_materials(config).size(), std::size_t(4));
        // deleted (when the person says so)
        panel.select_material(image_id);
        panel.remove();
        QCOMPARE(render::materials::user_materials(config).size(), std::size_t(4));  // (asked: no)
        s.answers.responder->question = [](const QString&, const QString&) { return true; };
        panel.remove();
        QCOMPARE(render::materials::user_materials(config).size(), std::size_t(3));
        QVERIFY(!QFileInfo::exists(QString::fromStdString((config / "materials" / (image_id.toStdString() + ".png")).string())));
        // a library that cannot be read is left as it is, and said so
        const auto file = config / "materials" / "library.json";
        { std::ofstream(file, std::ios::binary | std::ios::trunc) << "{broken"; }
        s.answers.responder->open_path = [&](const QString&, const QString&) { return picture; };
        panel.import_image();
        QVERIFY2(s.window->last_error().contains(QStringLiteral("書き換えずにそのままにしました")), qPrintable(s.window->last_error()));
        std::ifstream again(file, std::ios::binary);
        QCOMPARE(std::string((std::istreambuf_iterator<char>(again)), std::istreambuf_iterator<char>()), std::string("{broken"));
    }

    // スキャン画像の線画抽出: the paper fitted on a new paint layer, then the line-art filter with the settings chosen
    void scansReadAsLineArt() {
        Studio s;
        render::Image paper = render::Image::create("RGB", render::Size{400, 300}, render::Ink{250, 250, 245});
        paper.paste(render::Ink{20, 20, 20}, render::Box{100, 100, 300, 110});
        const QString file = s.tmp.path() + QStringLiteral("/紙.png");
        render::save_png(paper, gui_test::path_of(file));
        s.answers.responder->open_path = [&](const QString&, const QString&) { return file; };
        // the filter's settings asked: stopped, the paper stays as it is
        s.trigger("act_import_scan");
        const Json put = s.last();
        QCOMPARE(put["op"], Json("put_raster"));
        const std::string layer_id = put["id"].get<std::string>();
        const auto layer = std::find_if(s.page().layers.begin(), s.page().layers.end(), [&](const core::Layer& l) { return l.id == layer_id; });
        QVERIFY(layer != s.page().layers.end());
        QCOMPARE(QString::fromStdString(layer->title), QStringLiteral("線画（紙）"));
        QVERIFY(!s.answers.asked.isEmpty());  // (the line-art filter's settings were asked)
        QVERIFY(s.window->last_notice().startsWith(QStringLiteral("紙のままレイヤーに置きました")));
        // the filter's settings taken: lineart on the new layer
        s.answers.responder->exec = [](QDialog*) { return int(QDialog::Accepted); };
        s.trigger("act_import_scan");
        QCOMPARE(s.last()["op"], Json("filter_raster"));
        QCOMPARE(s.last()["kind"], Json("lineart"));
        QCOMPARE(s.last()["drop_blue"], Json(1));
        // from the scanner (here: none to scan with, then one that answers)
        s.window->scanner_source = [](int) -> std::string { throw app::scanner::ScanError(QStringLiteral("スキャンをやめました")); };
        const std::size_t before = s.ops.size();
        s.trigger("act_scanner");
        QCOMPARE(s.ops.size(), before);
        QCOMPARE(s.window->last_error(), QStringLiteral("スキャンをやめました"));
        s.window->scanner_source = [](int dpi) {
            QTest::qWait(1);
            return render::write_png(render::Image::create("L", render::Size{dpi / 6, dpi / 6}, render::Ink(255)));
        };
        s.trigger("act_scanner");
        QCOMPARE(s.last()["op"], Json("filter_raster"));
    }

    // the scanner's commands, as Python's scanner.command and scan give them
    void scannerCommands() {
        QCOMPARE(app::scanner::command(QStringLiteral("sane"), QStringLiteral("/t/scan.png"), 300, QStringLiteral("color")),
                 (QStringList{"scanimage", "--format=png", "--resolution=300", "--mode=Color", "--output-file=/t/scan.png"}));
        QCOMPARE(app::scanner::command(QStringLiteral("sane"), QStringLiteral("o.png")).at(3), QStringLiteral("--mode=Gray"));
        QCOMPARE(app::scanner::command(QStringLiteral("wia"), QStringLiteral("C:/t/scan.bmp")).front(), QStringLiteral("powershell"));
        QVERIFY(app::scanner::command(QStringLiteral("wia"), QStringLiteral("C:/t/scan.bmp"))
                    .back()
                    .contains(QStringLiteral("SaveFile('%1')").arg(QDir::toNativeSeparators(QStringLiteral("C:/t/scan.bmp")))));
        QVERIFY(app::scanner::command(QStringLiteral("wia"), QStringLiteral("C:/O'Brien/scan.bmp"))
                    .back()
                    .contains(QStringLiteral("SaveFile('%1')").arg(QDir::toNativeSeparators(QStringLiteral("C:/O''Brien/scan.bmp")))));
        const auto fails = [](const app::scanner::Runner& run, const QString& how) {
            try {
                app::scanner::scan(600, QStringLiteral("gray"), run, 1000, std::optional<QString>(how));
            } catch (const app::scanner::ScanError& e) {
                return QString::fromStdString(e.what());
            }
            return QString();
        };
        try {
            app::scanner::scan(600, QStringLiteral("gray"), {}, 1000, std::optional<QString>());
            QFAIL("nothing to scan with");
        } catch (const app::scanner::ScanError& e) {
            QVERIFY(QString::fromStdString(e.what()).startsWith(QStringLiteral("このパソコンでは")));
        }
        QCOMPARE(fails([](const QStringList&, int) { return app::scanner::Ran{3, {}, {}}; }, QStringLiteral("wia")),
                 QStringLiteral("スキャナーが見つかりません（つないであるか、電源が入っているかを確かめてください。スキャンした画像のファイルなら「スキャン画像を線画にして取り込む…」で読めます）"));
        QCOMPARE(fails([](const QStringList&, int) { return app::scanner::Ran{2, {}, {}}; }, QStringLiteral("wia")), QStringLiteral("スキャンをやめました"));
        QCOMPARE(fails([](const QStringList&, int) { return app::scanner::Ran{1, {}, QStringLiteral("x\nscanimage: no SANE devices found\n")}; }, QStringLiteral("sane")),
                 QStringLiteral("スキャンできませんでした: scanimage: no SANE devices found"));
        QCOMPARE(fails([](const QStringList&, int) { return app::scanner::Ran{0, {}, {}}; }, QStringLiteral("sane")),
                 QStringLiteral("スキャンできませんでした: スキャナーが見つからないか、使えません"));
        QCOMPARE(fails([](const QStringList&, int) { return app::scanner::Ran{std::nullopt, QStringLiteral("timeout"), {}}; }, QStringLiteral("sane")),
                 QStringLiteral("スキャナーが応えませんでした（timeout）"));
        // one that wrote the page
        const std::string bytes = app::scanner::scan(600, QStringLiteral("gray"), [](const QStringList& command, int) {
            const QString out = command.back().mid(QStringLiteral("--output-file=").size());
            QFile file(out);
            if (file.open(QIODevice::WriteOnly)) file.write("PNGDATA");
            return app::scanner::Ran{0, {}, {}};
        }, 1000, std::optional<QString>(QStringLiteral("sane")));
        QCOMPARE(bytes, std::string("PNGDATA"));
    }

    // サブビュー: reference pictures kept for every book; a click takes the colour under it for the pen
    void referencePicturesBesideThePage() {
        Studio s;
        auto* view = s.window->subview();
        QVERIFY(view != nullptr);
        auto* dock = s.window->findChild<QDockWidget*>(QStringLiteral("サブビュー"));
        QVERIFY(dock != nullptr);
        QVERIFY(!dock->isVisible());  // (for some work only: brought from the menu, as Python's; a small screen fits without it)
        QCOMPARE(view->choice->count(), 0);
        const QString file = s.tmp.path() + QStringLiteral("/資料.png");
        render::Image picture = render::Image::create("RGB", render::Size{40, 20}, render::Ink{200, 10, 10});
        picture.paste(render::Ink{10, 200, 10}, render::Box{20, 0, 40, 20});
        render::save_png(picture, gui_test::path_of(file));
        s.answers.responder->open_path = [&](const QString&, const QString&) { return file; };
        view->add_dialog();
        QCOMPARE(view->choice->count(), 1);
        QCOMPARE(view->choice->currentText(), QStringLiteral("資料.png"));
        QCOMPARE(app::kept_pictures(), QStringList{file});
        QVERIFY(!view->picture->image().isNull());
        // a file that is not a picture: said so, nothing kept
        const QString junk = s.tmp.path() + QStringLiteral("/junk.png");
        { std::ofstream(gui_test::path_of(junk), std::ios::binary) << "not a picture"; }
        s.answers.responder->open_path = [&](const QString&, const QString&) { return junk; };
        view->add_dialog();
        QCOMPARE(s.window->last_notice(), QStringLiteral("その画像は開けませんでした"));
        QCOMPARE(app::kept_pictures(), QStringList{file});
        // the colour under a click (the picture fitted to the panel)
        view->picture->resize(200, 100);
        QCOMPARE(view->picture->colour_at(QPointF(50, 50)), (std::optional<std::array<int, 3>>{{200, 10, 10}}));
        QCOMPARE(view->picture->colour_at(QPointF(150, 50)), (std::optional<std::array<int, 3>>{{10, 200, 10}}));
        view->picture->resize(200, 200);  // (letterboxed: above the picture is nothing)
        QVERIFY(!view->picture->colour_at(QPointF(100, 10)));
        QTest::mouseClick(view->picture, Qt::LeftButton, Qt::NoModifier, QPoint(150, 100));
        QCOMPARE(s.window->brush_panel()->rgb(), (std::array<int, 3>{10, 200, 10}));
        QVERIFY(s.window->last_notice().startsWith(QStringLiteral("資料の色 (10, 200, 10)")));
        // kept for the next window; taken away
        app::SubView again(s.window.get());
        QCOMPARE(again.choice->count(), 1);
        QVERIFY(again.picture->image().isNull());  // (read when the panel is first shown)
        again.show();
        QVERIFY(!again.picture->image().isNull());
        view->remove_current();
        QCOMPARE(view->choice->count(), 0);
        QVERIFY(app::kept_pictures().isEmpty());
    }

    void libraryRefusalsInThePersonsWords() {
        // (the library's and the packs' refusals beyond Python, as the person reads them)
        using app::wording::error;
        QCOMPARE(error(std::string("the material library would be too large for the materials panel (at most 1 MB)")),
                 QStringLiteral("素材ライブラリの一覧が大きくなりすぎるので、書き換えませんでした（1 MB まで）"));
        QCOMPARE(error(std::string("the pack.json is too large (at most 16 MB): pack.json")), QStringLiteral("素材パックの pack.json が大きすぎます（16 MB まで）"));
        QCOMPARE(error(std::string("the pack.json is a link or not a file: /x/pack.json")),
                 QStringLiteral("素材パックの pack.json がリンクかファイルではないので読みません"));
        QCOMPARE(error(std::string("the pack cannot be read as a zip file (a name inside is not UTF-8)")),
                 QStringLiteral("zip ファイルの中のファイル名が読めません（UTF-8 ではありません）"));
        QCOMPARE(error(std::string("the pack cannot be read as a zip file (it ends too soon)")), QStringLiteral("zip ファイルが壊れていて読めません"));
        QCOMPARE(error(std::string("the picture is too large (at most 64 MB): a.png")), QStringLiteral("画像ファイルが大きすぎます（64 MB まで）"));
    }

    void layerAndViewCommands() {
        Studio s;
        const std::size_t layers = s.page().layers.size();
        s.trigger("act_layer_pen");
        QCOMPARE(s.last()["op"], Json("add_layer"));
        QCOMPARE(s.page().layers.size(), layers + 1);
        s.trigger("act_layer_paint");
        QCOMPARE(s.last()["kind"], Json("paint"));
        s.trigger("act_layer_folder");
        QCOMPARE(s.last()["kind"], Json("folder"));
        const std::size_t now = s.ops.size();
        s.trigger("act_layer_dup");
        QCOMPARE(s.ops.size(), now + 1);
        s.trigger("act_layer_down");
        QCOMPARE(s.ops.size(), now + 2);
        s.trigger("act_layer_up");
        QCOMPARE(s.ops.size(), now + 3);
        s.trigger("act_layer_draft");
        QCOMPARE(s.last()["exportable"], Json(false));
        s.trigger("act_layer_draft");
        QCOMPARE(s.last()["exportable"], Json(true));
        // the view
        s.trigger("act_view_flip_v");
        QVERIFY(s.window->action("act_view_flip_v")->isChecked());
        QVERIFY(s.canvas()->flipped_vertical());
        s.trigger("act_view_flip_v");
        s.trigger("act_screen_dots");
        QVERIFY(s.canvas()->renderer().screen_dots());
        s.trigger("act_screen_dots");
        QVERIFY(!s.canvas()->renderer().screen_dots());
        // オニオンスキン: from the second page on
        const std::size_t before = s.ops.size();
        s.trigger("act_onion");
        QCOMPARE(s.ops.size(), before);
        s.window->go_to_page(2);  // (by its number)
        QCOMPARE(s.window->page_index(), 1);
        s.trigger("act_onion");
        QCOMPARE(s.last()["op"], Json("step_onion"));
        QCOMPARE(s.last()["delta"], Json(-1));
    }
};

QTEST_MAIN(TestGuiEffects)
#include "test_gui_effects.moc"
