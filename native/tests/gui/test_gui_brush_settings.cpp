// PC4 E2 (window.panel.brushSettings・.brushes, brush.presets.*, brush.defineFromSelection, edit.defineBrushPreset,
// edit.presets.*; SPEC.md COLOR-09「ブラシのプリセット」「ブラシ設定パネル」「プリセットマネージャー」) on the offscreen platform:
// ウィンドウ → ブラシ設定 (F5: Photoshop's sections of the brush in hand's dab settings; a change to a built-in brush copies it
// as one's own first and chooses it, a change to one's own changes it, kept in the library; its line drawn; a brush of
// Genko's way of drawing made a dab brush), ウィンドウ → ブラシ (the library's groups with their tips' pictures made off the
// window's thread; a click chooses the brush; a new one, renamed, moved, deleted; an .abr read with what it could not
// keep said), 編集 → ブラシを定義… (the selection's paint), a built-in dab brush's line (the book keeps it and its tip's
// picture), the pixel tools' ブラシ (the op's brush and seed), 編集 → プリセット ▸ (プリセットマネージャー…, 書き出し／読み込み…,
// 移行), and their places in the menus.

#include <QtTest>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTreeWidget>

#include <filesystem>
#include <fstream>
#include <optional>

#include "gui_support.hpp"
#include "app/brush_panel.hpp"
#include "app/brush_settings_panel.hpp"
#include "app/brushes_panel.hpp"
#include "app/canvas.hpp"
#include "app/colours.hpp"
#include "app/config.hpp"
#include "app/fields.hpp"
#include "app/inject.hpp"
#include "app/main_window.hpp"
#include "app/preset_manager.hpp"
#include "app/session.hpp"
#include "core/base64.hpp"
#include "core/brush_dabs.hpp"
#include "core/command_bus.hpp"
#include "render/abr.hpp"
#include "render/brush_presets.hpp"
#include "render/brushes.hpp"
#include "render/gradient_presets.hpp"
#include "render/ops_registry.hpp"
#include "render/png.hpp"
#include "render/preset_store.hpp"

using namespace genko;
using core::Json;
namespace inject = genko::app::inject;
namespace presets = render::brush_presets;

namespace {

constexpr int kSide = 472;  // (the 60 mm page at the working 200 dpi)

// A book of one 60 × 60 mm colour page: a paint layer "paint" — white, a black box at 10..20 mm × 30..36 mm.
core::Document book_doc() {
    auto doc = core::new_episode("ブラシ", core::Num(1), 1, core::PageSpec::custom(60, 60, 50, 50, 1, 2, 2, 2, 2, 72, "color"));
    std::string bytes;
    const auto px = [](double mm) { return static_cast<int>(mm * kSide / 60.0); };
    for (int y = 0; y < kSide; ++y)
        for (int x = 0; x < kSide; ++x) {
            const bool ink = x >= px(10) && x < px(20) && y >= px(30) && y < px(36);
            bytes += ink ? std::string("\x00\x00\x00\xFF", 4) : std::string("\xFF\xFF\xFF\xFF", 4);
        }
    const render::Image picture = render::Image::frombytes("RGBA", render::Size{kSide, kSide}, bytes);
    return core::CommandBus(render::ops_registry())
        .apply(doc,
               Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "paint"}},
                            Json{{"op", "set_layer"}, {"page", 1}, {"id", "paint"}, {"panel_each", false}, {"panel_clip", false}},
                            Json{{"op", "put_raster"}, {"page", 1}, {"id", "paint"}, {"png_base64", core::b64encode(render::write_png(picture))}}}),
               core::Actor("human:tester"))
        .doc;
}

void forget_library() {
    std::error_code ec;
    for (const char* name : {"brushes.json", "brush_papers.json", "tool_presets.json", "gradients.json", "patterns.json", "shapes.json"})
        std::filesystem::remove(app::config_dir() / name, ec);
    for (const char* name : {"brush_tips", "brush_papers", "patterns"}) std::filesystem::remove_all(app::config_dir() / name, ec);
}

struct Studio {
    QTemporaryDir tmp;
    std::shared_ptr<app::Session> session;
    std::unique_ptr<app::MainWindow> window;
    gui_test::Answers answers;
    std::vector<Json> ops;
    Studio() {
        (void)gui_test::config_folder();
        const auto path = gui_test::path_of(tmp.path() + "/book");
        gui_test::write_book(path, book_doc());
        session = app::Session::open(path, gui_test::quick(gui_test::path_of(tmp.path() + "/recovery")));
        window = std::make_unique<app::MainWindow>(session);
        window->resize(1200, 860);
        window->show();
        window->canvas()->fit_page();
        window->set_target_layer("paint");
        QObject::connect(session.get(), &app::Session::changed, window.get(), [this](const app::BookChange& c) {
            if (c.why == app::BookChange::Why::Edit) ops.push_back(c.ops);
        });
    }
    const core::Document& book() const { return session->document(); }
    void trigger(const char* name) const { window->action(QString::fromLatin1(name))->trigger(); }
    QStringList menus_of(QAction* act) const {
        QStringList out;
        std::function<void(QMenu*, const QString&)> walk = [&](QMenu* menu, const QString& top) {
            for (QAction* a : menu->actions()) {
                if (a == act) out << top;
                if (a->menu() != nullptr) walk(a->menu(), top);
            }
        };
        for (QAction* top : window->menuBar()->actions())
            if (top->menu() != nullptr) walk(top->menu(), top->text());
        return out;
    }
    QMenu* menu(const QString& title) const {
        for (QAction* a : window->menuBar()->actions())
            if (a->menu() != nullptr && a->menu()->title() == title) return a->menu();
        return nullptr;
    }
};

QTreeWidgetItem* item_named(QTreeWidget* tree, const QString& name) {
    for (QTreeWidgetItemIterator it(tree); *it != nullptr; ++it)
        if ((*it)->text(0) == name && (*it)->parent() != nullptr) return *it;
    return nullptr;
}

std::optional<presets::Preset> own_named(const std::string& name) {
    for (const presets::Group& g : presets::load(app::config_dir()).groups) {
        if (g.builtin) continue;
        for (const presets::Preset& p : g.items)
            if (p.name == name) return p;  // (the test's own copy)
    }
    return std::nullopt;
}

}  // namespace

class TestGuiBrushSettings : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { qRegisterMetaType<genko::app::StrokeInput>(); }
    void init() {
        (void)gui_test::config_folder();
        app::settings()->clear();
        forget_library();
    }

    // ウィンドウ → ブラシ設定 (F5), ブラシ, ツールプリセット; 編集 → ブラシを定義… (before パターンを定義…) and プリセット ▸.
    void thePlacesInTheMenus() {
        Studio s;
        QDockWidget* settings = s.window->brush_settings_dock();
        QVERIFY(settings != nullptr && !settings->isVisible());
        QCOMPARE(settings->toggleViewAction()->shortcut(), QKeySequence(QStringLiteral("F5")));
        QVERIFY(s.menus_of(settings->toggleViewAction()).contains(QStringLiteral("ウィンドウ")));
        QVERIFY(s.menus_of(s.window->brushes_dock()->toggleViewAction()).contains(QStringLiteral("ウィンドウ")));
        QVERIFY(s.menus_of(s.window->tool_presets_dock()->toggleViewAction()).contains(QStringLiteral("ウィンドウ")));
        QCOMPARE(s.window->action(QStringLiteral("act_define_brush"))->text(), QStringLiteral("ブラシを定義…"));
        QMenu* edit = s.menu(QStringLiteral("編集"));
        QVERIFY(edit != nullptr);
        const QList<QAction*> items = edit->actions();
        const qsizetype brush = items.indexOf(s.window->action(QStringLiteral("act_define_brush")));
        const qsizetype pattern = items.indexOf(s.window->action(QStringLiteral("act_define_pattern")));
        const qsizetype shape = items.indexOf(s.window->action(QStringLiteral("act_define_custom_shape")));
        QVERIFY(brush >= 0 && brush + 1 == pattern && pattern + 1 == shape);
        QVERIFY(shape + 1 < items.size() && items[shape + 1]->menu() != nullptr);
        QMenu* presets_menu = items[shape + 1]->menu();
        QCOMPARE(presets_menu->title(), QStringLiteral("プリセット"));
        QStringList names;
        for (QAction* a : presets_menu->actions()) names << a->text();
        QCOMPARE(names, (QStringList{QStringLiteral("プリセットマネージャー…"), QStringLiteral("プリセットを移行"), QStringLiteral("プリセットを書き出し／読み込み…")}));
    }

    // F5 with a built-in dab brush in hand: its sections show its settings; a change copies it as one's own (chosen, in the
    // library) — the built-in one never changes —, then changes that copy in its place (kept in the library a moment
    // later); the line drawn below.
    void theSettingsOfTheBrushInHand() {
        Studio s;
        s.window->choose_brush("my_genko_soft_round");
        QCOMPARE(s.window->brush_panel()->kind(), std::string("my_genko_soft_round"));
        s.window->brush_settings_dock()->toggleViewAction()->trigger();
        app::BrushSettingsPanel* panel = s.window->brush_settings_panel();
        QVERIFY(panel != nullptr);
        panel->refresh();
        QCOMPARE(panel->pages->currentIndex(), 0);
        auto* hardness = qobject_cast<QDoubleSpinBox*>(panel->field(QStringLiteral("bs_hardness")));
        auto* size = qobject_cast<QDoubleSpinBox*>(panel->field(QStringLiteral("bs_size")));
        QVERIFY(hardness != nullptr && size != nullptr);
        QCOMPARE(hardness->value(), 0.0);
        QVERIFY(std::abs(size->value() - 45 * 25.4 / 200.0) < 0.01);
        for (const char* name : {"bs_shape", "bs_scatter", "bs_texture", "bs_dual", "bs_color", "bs_transfer", "bs_pose", "bs_noise", "bs_wet_edges",
                                 "bs_build_up", "bs_protect_texture", "bs_texture_pattern", "bs_dual_mode", "bs_size_control"})
            QVERIFY2(panel->field(QString::fromLatin1(name)) != nullptr, name);
        hardness->setValue(40);
        const std::string copy = s.window->brush_panel()->kind();
        QVERIFY(copy.starts_with("my_") && presets::is_own(copy));
        QCOMPARE(panel->key(), copy);
        std::optional<presets::Preset> kept = own_named("ソフト円ブラシのコピー");
        QVERIFY(kept.has_value());
        QCOMPARE(kept->key, copy);
        QVERIFY(std::abs(core::dabs_from_json(presets::builtin("my_genko_soft_round")->settings["dabs"]).hardness) < 1e-9);  // (the built-in as it was)
        // a change to the copy: in its place, at once in this process, a moment later in the library
        qobject_cast<QCheckBox*>(panel->field(QStringLiteral("bs_scatter")))->setChecked(true);
        QCOMPARE(s.window->brush_panel()->kind(), copy);
        QVERIFY(render::brushes::brush(copy).dabs->scatter.enabled);
        QVERIFY(std::abs(render::brushes::brush(copy).dabs->hardness - 0.4) < 1e-9);
        panel->keep_now();
        kept = own_named("ソフト円ブラシのコピー");
        QVERIFY(core::dabs_from_json(kept->settings["dabs"]).scatter.enabled);
        // its line drawn below
        panel->sample->draw();
        QVERIFY(!panel->sample->pixmap().isNull());
    }

    // A brush of Genko's way of drawing: its 詳細 and 「Photoshop 型（ダブ）にする」, which makes one's own dab brush of it.
    void aGenkoBrushMadeADabBrush() {
        Studio s;
        s.window->choose_brush("gpen");
        s.window->brush_settings_dock()->toggleViewAction()->trigger();
        app::BrushSettingsPanel* panel = s.window->brush_settings_panel();
        panel->refresh();
        QCOMPARE(panel->pages->currentIndex(), 1);
        QVERIFY(panel->about->text().contains(QStringLiteral("G ペン")));
        panel->to_dabs->click();
        const std::string made = s.window->brush_panel()->kind();
        QVERIFY(presets::is_own(made));
        QVERIFY(render::brushes::is_dab(made));
        panel->refresh();
        QCOMPARE(panel->pages->currentIndex(), 0);
    }

    // ウィンドウ → ブラシ: the groups (Genko, 一般, ドライメディア, ウェットメディア, 特殊効果, one's own), the tips' pictures made off
    // the window's thread; a click chooses the brush; ＋ keeps the brush in hand as one's own, renamed, moved to a new group,
    // deleted; an .abr read into a group of its name, what it could not keep said.
    void theBrushesPanel() {
        Studio s;
        s.window->brushes_dock()->toggleViewAction()->trigger();
        app::BrushesPanel* panel = s.window->brushes_panel();
        panel->refresh();
        QTreeWidget* tree = panel->tree();
        QStringList groups;
        for (int i = 0; i < tree->topLevelItemCount(); ++i) groups << tree->topLevelItem(i)->text(0);
        QCOMPARE(groups.mid(0, 5), (QStringList{QStringLiteral("Genko"), QStringLiteral("一般"), QStringLiteral("ドライメディア"), QStringLiteral("ウェットメディア"),
                                               QStringLiteral("特殊効果")}));
        QVERIFY(gui_test::wait_for([&] { return panel->pictures_pending() == 0; }));
        QTreeWidgetItem* chalk = item_named(tree, QStringLiteral("チョーク"));
        QVERIFY(chalk != nullptr);
        QVERIFY(gui_test::wait_for([&] { return !item_named(tree, QStringLiteral("チョーク"))->icon(0).isNull(); }));
        tree->setCurrentItem(item_named(tree, QStringLiteral("チョーク")));
        QCOMPARE(s.window->brush_panel()->kind(), std::string("my_genko_chalk"));
        // ＋: the brush in hand as one's own (its pictures with it), renamed, moved, deleted
        panel->new_preset(QStringLiteral("自分のチョーク"));
        std::optional<presets::Preset> mine = own_named("自分のチョーク");
        QVERIFY(mine.has_value());
        const std::string key = mine->key;
        const std::string tip = core::dabs_from_json(mine->settings["dabs"]).tip;
        QVERIFY(render::brushes::library_tip(app::config_dir(), tip) != nullptr);
        QCOMPARE(s.window->brush_panel()->kind(), key);
        QCOMPARE(panel->chosen_key(), key);  // (the panel shows the brush in hand)
        panel->rename(QStringLiteral("チョーク（自分）"));
        QVERIFY(own_named("チョーク（自分）") != std::nullopt);
        panel->tree()->setCurrentItem(item_named(panel->tree(), QStringLiteral("チョーク（自分）")));
        panel->move_to_group(QStringLiteral("画材"));
        mine = own_named("チョーク（自分）");
        QCOMPARE(mine->group, std::string("画材"));
        panel->tree()->setCurrentItem(item_named(panel->tree(), QStringLiteral("チョーク（自分）")));
        panel->remove();
        QVERIFY(own_named("チョーク（自分）") == std::nullopt);
        // a built-in brush is not renamed or deleted (said)
        panel->tree()->setCurrentItem(item_named(panel->tree(), QStringLiteral("チョーク")));
        panel->remove();
        QVERIFY(item_named(panel->tree(), QStringLiteral("チョーク")) != nullptr);
        // an .abr: a group of its name, a setting with no place here said
        render::abr::LegacyBrush round;
        round.computed = true;
        round.diameter = 25;
        round.hardness = 100;
        render::abr::LegacyBrush picture;
        picture.name = "点々";
        picture.sample.width = 6;
        picture.sample.height = 6;
        for (int i = 0; i < 36; ++i) picture.sample.data.push_back(static_cast<char>(i % 2 == 0 ? 255 : 0));
        const QString file = s.tmp.path() + QStringLiteral("/画材.abr");
        std::ofstream(gui_test::path_of(file), std::ios::binary) << render::abr::write_v12(2, {round, picture}, false);
        QVERIFY(panel->import_file(file, false));
        mine = own_named("点々");
        QVERIFY(mine.has_value());
        QCOMPARE(mine->group, std::string("画材"));
        QVERIFY(item_named(panel->tree(), QStringLiteral("点々")) != nullptr);
        // a file that is not one: said, nothing added
        const QString broken = s.tmp.path() + QStringLiteral("/壊れ.abr");
        std::ofstream(gui_test::path_of(broken), std::ios::binary) << "8BPS";
        QVERIFY(!panel->import_file(broken, false));
        QVERIFY(!s.answers.asked.isEmpty() && s.answers.asked.back().contains(QStringLiteral("読み込めませんでした")));
    }

    // A built-in dab brush with a sampled tip drawn on the page: the book keeps it (under its key) with its tip's picture,
    // and the line keeps the look it was drawn with when the book is opened again (one Undo takes both).
    void aBuiltinDabBrushInTheBook() {
        Studio s;
        s.window->choose_brush("my_genko_chalk");
        s.window->set_target_layer("paint");
        s.trigger("act_pen");
        inject::mouse_stroke(s.window->canvas(), {QPointF(25, 10), QPointF(45, 12)});
        QVERIFY(gui_test::wait_for([&] { return s.book().brush_custom.contains("my_genko_chalk"); }));
        const core::Brush b = core::brush_from_dict("my_genko_chalk", s.book().brush_custom.at("my_genko_chalk"), std::nullopt, {});
        QVERIFY(b.dabs);
        for (const std::string& ref : core::dab_assets(*b.dabs)) QVERIFY(s.book().brush_tips.contains(ref));
        QVERIFY(!s.ops.empty());
        QCOMPARE(s.ops.back()[0]["op"].get<std::string>(), std::string("define_brush"));
        s.trigger("act_undo");
        QVERIFY(!s.book().brush_custom.contains("my_genko_chalk"));
    }

    // 編集 → ブラシを定義…: the selection's paint (the black box), the layer drawn on alone, into the library, chosen.
    void aBrushDefinedFromTheSelection() {
        Studio s;
        s.window->apply_ops(Json::array({Json{{"op", "select_area"}, {"page", 1},
                                              {"area", Json{{"poly", Json::array({Json::array({8.0, 28.0}), Json::array({22.0, 28.0}), Json::array({22.0, 38.0}),
                                                                                   Json::array({8.0, 38.0})})}}}}}));
        s.window->define_brush_preset(QStringLiteral("四角"));
        std::optional<presets::Preset> made = own_named("四角");
        QVERIFY(made.has_value());
        const core::DabSettings d = core::dabs_from_json(made->settings["dabs"]);
        QVERIFY(!d.tip.empty());
        QVERIFY(std::abs(d.size_mm - 10.0) < 0.4);  // (the box's larger side, 10 mm)
        QCOMPARE(s.window->brush_panel()->kind(), made->key);
    }

    // The pixel tools' ブラシ: the library's dab brushes; chosen, the stroke's op names it (the book keeps it first) with
    // the stroke's seed, the brush's own hardness and spacing.
    void thePixelToolsTakeADabBrush() {
        Studio s;
        auto* box = s.window->findChild<QComboBox*>(QStringLiteral("pixel_brush"));
        QVERIFY(box != nullptr);
        QCOMPARE(box->itemData(0).toString(), QString());
        const int chalk = box->findData(QStringLiteral("my_genko_chalk"));
        QVERIFY(chalk > 0);
        box->setCurrentIndex(chalk);
        emit box->activated(chalk);
        s.trigger("act_blur_tool");
        s.window->set_target_layer("paint");
        inject::mouse_stroke(s.window->canvas(), {QPointF(9, 33), QPointF(21, 33)});
        QVERIFY(gui_test::wait_for([&] { return !s.ops.empty(); }));
        const Json& last = s.ops.back();
        QCOMPARE(last[0]["op"].get<std::string>(), std::string("define_brush"));
        const Json& op = last[last.size() - 1];
        QCOMPARE(op["op"].get<std::string>(), std::string("blur_brush"));
        QCOMPARE(op["brush"].get<std::string>(), std::string("my_genko_chalk"));
        QVERIFY(op.contains("seed") && !op.contains("hardness") && !op.contains("spacing"));
        // chosen in the Brushes panel with a pixel tool in hand: that tool's ブラシ (the tool kept in hand)
        s.window->choose_brush("my_genko_spatter");
        QCOMPARE(box->currentData().toString(), QStringLiteral("my_genko_spatter"));
        QCOMPARE(s.window->tool_in_hand(), QStringLiteral("blurtool"));
        // a brush of Genko's way of drawing lays no dabs: the pen takes it
        s.window->choose_brush("gpen");
        QCOMPARE(s.window->brush_panel()->kind(), std::string("gpen"));
        QCOMPARE(s.window->tool_in_hand(), QStringLiteral("pen"));
    }

    // (review P2-4) A dab brush whose colour moves between the 描画色 and the 背景色 (葉): its line takes the カラー panel's 背景色.
    void aColourJitterBrushTakesTheBackground() {
        Studio s;
        s.window->colours()->set_background({90, 50, 20});
        s.window->choose_brush("my_genko_foliage");
        s.trigger("act_pen");
        s.window->set_target_layer("paint");
        inject::mouse_stroke(s.window->canvas(), {QPointF(25, 10), QPointF(45, 12)});
        QVERIFY(gui_test::wait_for([&] { return !s.ops.empty(); }));
        const Json& last = s.ops.back();
        const Json& line = last[last.size() - 1];
        QCOMPARE(line["op"].get<std::string>(), std::string("add_stroke"));
        QCOMPARE(line["background_rgb"], Json::array({90, 50, 20}));
        // a brush of Genko's way of drawing: none (its lines as before)
        s.window->choose_brush("gpen");
        inject::mouse_stroke(s.window->canvas(), {QPointF(25, 20), QPointF(45, 22)});
        QVERIFY(gui_test::wait_for([&] { return s.ops.size() >= 2; }));
        QVERIFY(!s.ops.back()[s.ops.back().size() - 1].contains("background_rgb"));
    }

    // (review P2-3) A group of many brushes deleted: the lists made again once.
    void aGroupDeletedAtOnce() {
        Studio s;
        for (const char* name : {"一", "二", "三", "四"})
            presets::save(app::config_dir(), std::nullopt, Json{{"label", name}, {"base", "gpen"}}, std::string(name), std::string("まとめて"));
        s.window->brushes_dock()->toggleViewAction()->trigger();
        app::BrushesPanel* panel = s.window->brushes_panel();
        panel->refresh();
        QTreeWidgetItem* group = nullptr;
        for (int i = 0; i < panel->tree()->topLevelItemCount(); ++i)
            if (panel->tree()->topLevelItem(i)->text(0) == QStringLiteral("まとめて")) group = panel->tree()->topLevelItem(i);
        QVERIFY(group != nullptr);
        panel->tree()->setCurrentItem(group);
        s.answers.responder->question = [](const QString&, const QString&) { return true; };
        const int before = s.window->brushes_changed_count();
        panel->remove_group();
        QCOMPARE(s.window->brushes_changed_count(), before + 1);
        QVERIFY(own_named("一") == std::nullopt && own_named("四") == std::nullopt);
    }

    // (review P2-2) A library holding an entry that is not a brush: the window starts, the others listed.
    void aLibraryWithEntriesThatAreNotBrushes() {
        (void)gui_test::config_folder();
        std::filesystem::create_directories(app::config_dir());
        std::ofstream(app::config_dir() / "brushes.json", std::ios::binary)
            << R"({"brushes": {"my_x": "x", "my_a": {"label": "残ったブラシ", "base": "gpen"}}})";
        Studio s;
        QVERIFY(s.window->findChild<QComboBox*>(QStringLiteral("pixel_brush")) != nullptr);
        s.window->brushes_dock()->toggleViewAction()->trigger();
        s.window->brushes_panel()->refresh();
        QVERIFY(item_named(s.window->brushes_panel()->tree(), QStringLiteral("残ったブラシ")) != nullptr);
    }

    // 編集 → プリセット ▸: the Preset Manager's kinds and an edit (a gradient renamed); プリセットを書き出し／読み込み… to a
    // file and back into the library; プリセットを移行 from a settings folder.
    void thePresetsCommands() {
        Studio s;
        {
            auto lib = render::preset_store::load(app::config_dir(), render::gradient_presets::kind());
            render::preset_store::add(lib.groups, render::gradient_presets::kind(),
                                      render::gradient_presets::item_of(Json{{"stops", Json::array({"#ff0000", "#00ff00"})}}, "赤緑"), std::string("自分の"), "x");
            render::preset_store::save(app::config_dir(), render::gradient_presets::kind(), lib.groups);
        }
        app::PresetManager manager(s.window.get());
        QCOMPARE(manager.kinds->count(), 6);  // (ブラシ・グラデーション・パターン・シェイプ・ツールプリセット、PC4 E6 のスタイル)
        manager.show_kind("gradients");
        QVERIFY(item_named(manager.tree(), QStringLiteral("赤緑")) != nullptr);
        QVERIFY(manager.apply(Json{{"action", "rename"}, {"preset", "赤緑"}, {"name", "赤と緑"}}));
        QVERIFY(item_named(manager.tree(), QStringLiteral("赤と緑")) != nullptr);
        manager.show_kind("brushes");
        QVERIFY(!manager.apply(Json{{"action", "rename"}, {"preset", "チョーク"}, {"name", "x"}}));  // (a built-in brush: said)
        QVERIFY(!s.answers.asked.isEmpty() && s.answers.asked.back().contains(QStringLiteral("組み込みのブラシ（チョーク）は変えられません")));
        presets::save(app::config_dir(), std::nullopt, Json{{"label", "自分の線"}, {"base", "gpen"}}, std::nullopt, std::nullopt);
        const QString file = s.tmp.path() + QStringLiteral("/持っていく.genkopresets");
        const Json counts = s.window->export_presets(file, {}, false);
        QCOMPARE(counts["brushes"].get<int>(), 1);
        presets::remove(app::config_dir(), {"自分の線"});
        const Json added = s.window->import_presets(file, {"brushes"});
        QCOMPARE(added["added"]["brushes"].get<int>(), 1);
        QVERIFY(own_named("自分の線") != std::nullopt);
        // from another settings folder: its groups not here yet
        QTemporaryDir other;
        presets::save(gui_test::path_of(other.path()), std::nullopt, Json{{"label", "よそのペン"}, {"base", "maru"}}, std::nullopt, std::string("よそ"));
        const Json moved = s.window->migrate_presets(other.path());
        QCOMPARE(moved["added"]["brushes"].get<int>(), 1);
        QVERIFY(own_named("よそのペン") != std::nullopt);
    }
};

QTEST_MAIN(TestGuiBrushSettings)
#include "test_gui_brush_settings.moc"
