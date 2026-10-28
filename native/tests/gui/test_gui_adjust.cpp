// PC2a (SPEC.md COLOR-04, design 5) on the offscreen platform: レイヤー → 色調補正's tone commands (Ctrl+L, Ctrl+M,
// Ctrl+I; each key once among the window's), their windows — the channel switch by the page's mode, the formula's
// choice, 自動, a 3D LUT read from a file —, the result shown on the page while it changes and applied as one op;
// 平均化 and the 自動 commands; the 色調補正 panel's buttons (a correction layer in PhotoCraft's form, the user's decision
// 3) and the 属性 panel (what is changed shown at once, applied as one set_adjustment: one Undo); ファイル →
// カラールックアップテーブルを書き出す… (a .cube of the size asked).

#include <QtTest>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>

#include <algorithm>
#include <cmath>
#include <set>

#include "gui_support.hpp"
#include "app/adjust_forms.hpp"
#include "app/adjust_panels.hpp"
#include "app/canvas.hpp"
#include "app/layer_panel.hpp"
#include "app/main_window.hpp"
#include "app/workspace.hpp"
#include "core/base64.hpp"
#include "core/command_bus.hpp"
#include "core/lut3d.hpp"
#include "render/image.hpp"
#include "render/ops_registry.hpp"
#include "render/png.hpp"
#include "testsupport.hpp"
using namespace genko;
using core::Json;

namespace {

// A page of a picture (a grey ramp, a green square) on a paint layer "pic"; `mode` its colour mode.
core::Document book_doc(const std::string& mode = "rgb") {
    auto doc = core::new_episode("補正", core::Num(1), 1, core::PageSpec::custom(40, 40, 30, 30, 1, 2, 2, 2, 2, 72, "color"));
    render::Image picture = render::Image::create("RGBA", render::Size{60, 60}, render::Ink{0, 0, 0, 0});
    for (int x = 0; x < 60; ++x) picture.paste(render::Ink{static_cast<unsigned char>(70 + x), static_cast<unsigned char>(70 + x), static_cast<unsigned char>(70 + x), 255}, render::Box{x, 0, x + 1, 40});
    picture.paste(render::Ink{30, 160, 90, 255}, render::Box{0, 40, 60, 60});
    Json ops = Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pic"}},
                            Json{{"op", "set_layer"}, {"page", 1}, {"id", "pic"}, {"panel_each", false}, {"panel_clip", false}},
                            Json{{"op", "put_raster"}, {"page", 1}, {"id", "pic"}, {"png_base64", core::b64encode(render::write_png(picture))}}});
    if (mode == "cmyk") ops.push_back(Json{{"op", "set_color_mode"}, {"mode", "cmyk"}, {"icc", core::b64encode(genko::test::read_bytes(genko::test::test_data(QStringLiteral("colour/photocraft-coated-cmyk.icc"))))}, {"depth", 16}});
    return core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor("human:tester")).doc;
}

struct Book {
    QTemporaryDir tmp;
    std::shared_ptr<app::Session> session;
    std::unique_ptr<app::MainWindow> window;
    explicit Book(const core::Document& doc = book_doc()) {
        (void)gui_test::config_folder();
        const auto path = gui_test::path_of(tmp.path() + "/book");
        gui_test::write_book(path, doc);
        session = app::Session::open(path, gui_test::quick(gui_test::path_of(tmp.path() + "/recovery")));
        window = std::make_unique<app::MainWindow>(session);
        window->resize(1200, 800);
        window->show();
        QCoreApplication::processEvents();
        window->set_target_layer("pic");
    }
    const core::Page& page() const { return session->document().page(0); }
    const core::Layer* layer(const std::string& id) const {
        for (const auto& l : page().layers)
            if (l.id == id) return &l;
        return nullptr;
    }
    std::string pixels(const std::string& id) const {
        const core::Layer* l = layer(id);
        if (l == nullptr) return {};
        if (l->ink_raster) return *l->ink_raster;
        return l->raster_png ? *l->raster_png : std::string();
    }
    void undo() const { window->action(QStringLiteral("act_undo"))->trigger(); }
    QMenu* menu(const QString& title) const {
        for (QAction* a : window->menuBar()->actions())
            if (a->menu() != nullptr && a->text() == title) return a->menu();
        return nullptr;
    }
};

// The window of a correction answered: its form's fields set by `fill`, then かける (or やめる).
std::function<int(QDialog*)> answer(std::function<void(QDialog*)> fill, bool apply = true) {
    return [fill, apply](QDialog* dialog) {
        if (dialog->objectName() != QStringLiteral("adjust_dialog")) return int(QDialog::Rejected);
        if (fill) fill(dialog);
        return int(apply ? QDialog::Accepted : QDialog::Rejected);
    };
}

}  // namespace

class TestGuiAdjust : public QObject {
    Q_OBJECT

private slots:
    // レイヤー → 色調補正 holds the tone commands; the keys of the design (Ctrl+L, Ctrl+M, Ctrl+I), each once among every
    // command of the window; カラールックアップテーブル… (PC4 F-0: Photoshop's name and place) last in ファイル → 書き出し.
    void menusAndKeys() {
        Book book;
        QMenu* layers = book.menu(QStringLiteral("レイヤー"));
        QVERIFY(layers != nullptr);
        QMenu* adjust = nullptr;
        for (QAction* a : layers->actions())
            if (a->menu() != nullptr && a->menu()->objectName() == QStringLiteral("menu_adjust")) adjust = a->menu();
        QVERIFY(adjust != nullptr);
        QStringList texts;
        for (QAction* a : adjust->actions())
            if (!a->isSeparator()) texts << a->text();
        // (Photoshop's names, the same as the layer panel's 色調補正のレイヤー of the same kind: the user's decision 8 —
        // コマンドを探す tells them apart by their place, below)
        for (const char* text : {"明るさ・コントラスト…", "レベル補正…", "トーンカーブ…", "露光量…", "カラールックアップ…", "階調の反転", "階調化…",
                                 "2 値化…", "グラデーションマップ…", "平均化（イコライズ）", "自動トーン補正", "自動コントラスト", "自動カラー補正"}) {
            QVERIFY2(texts.contains(QString::fromUtf8(text)), text);
        }
        // コマンドを探す: レベル補正… of レイヤー → 色調補正 and of the layer panel's 色調補正のレイヤー, each found with its
        // place after its words; the names the quick access and the command bar keep and offer (their chooser) the same
        // words and place, each its own command
        QAction* levels = book.window->action(QStringLiteral("act_adjust_levels"));
        QAction* layer_levels = nullptr;
        for (QAction* a : book.window->layer_panel()->special_menu()->actions())
            if (a->menu() != nullptr)
                for (QAction* b : a->menu()->actions())
                    if (b->text() == QStringLiteral("レベル補正…")) layer_levels = b;
        QVERIFY(levels != nullptr && layer_levels != nullptr && levels != layer_levels);
        const QString menu_place = QStringLiteral("レイヤー → 色調補正");
        const QString panel_place = QStringLiteral("レイヤー → 塗り・グラデーション・色調補正のレイヤー → 色調補正のレイヤー");
        QCOMPARE(app::where(book.window.get(), levels), menu_place);
        QCOMPARE(app::where(book.window.get(), layer_levels), panel_place);
        app::CommandSearch* search = book.window->find_command();
        search->query->setText(QStringLiteral("レベル補正"));
        QVERIFY(std::find(search->found().begin(), search->found().end(), levels) != search->found().end());
        QVERIFY(std::find(search->found().begin(), search->found().end(), layer_levels) != search->found().end());
        QStringList rows;
        for (int i = 0; i < search->results->count(); ++i) rows << search->results->item(i)->text();
        for (const QString& place : {menu_place, panel_place}) {
            const QString row = QStringLiteral("レベル補正…　（%1）").arg(place);
            QVERIFY2(std::any_of(rows.begin(), rows.end(), [&row](const QString& r) { return r.startsWith(row); }), qPrintable(rows.join(QStringLiteral(" | "))));
        }
        search->close();
        const auto names = app::by_text(book.window.get());
        const QString menu_name = QStringLiteral("レベル補正…（%1）").arg(menu_place);
        const QString panel_name = QStringLiteral("レベル補正…（%1）").arg(panel_place);
        QVERIFY(names.count(menu_name) == 1 && names.at(menu_name) == levels);
        QVERIFY(names.count(panel_name) == 1 && names.at(panel_name) == layer_levels);
        app::ChooseCommands chooser(book.window.get(), QStringLiteral("クイックアクセスに並べるもの"), {});
        chooser.filter->setText(QStringLiteral("レベル補正"));
        QStringList offered;
        for (int i = 0; i < chooser.all->count(); ++i) offered << chooser.all->item(i)->text();
        QVERIFY2(offered.contains(menu_name) && offered.contains(panel_name), qPrintable(offered.join(QStringLiteral(" | "))));
        QCOMPARE(book.window->action(QStringLiteral("act_adjust_levels"))->shortcut(), QKeySequence(QStringLiteral("Ctrl+L")));
        QCOMPARE(book.window->action(QStringLiteral("act_adjust_curves"))->shortcut(), QKeySequence(QStringLiteral("Ctrl+M")));
        QCOMPARE(book.window->action(QStringLiteral("act_adjust_invert"))->shortcut(), QKeySequence(QStringLiteral("Ctrl+I")));
        std::map<QString, QString> keys;
        for (const auto& [name, act] : book.window->actions_by_name()) {
            for (const QKeySequence& key : act->shortcuts()) {
                const QString text = key.toString();
                QVERIFY2(!keys.contains(text), qPrintable(text + QStringLiteral(": ") + keys[text] + QStringLiteral(" and ") + name));
                keys[text] = name;
            }
        }
        QMenu* exports = book.window->findChild<QMenu*>(QStringLiteral("menu_export"));
        QVERIFY(exports != nullptr);
        QStringList items;
        for (QAction* a : exports->actions()) items << (a->isSeparator() ? QStringLiteral("|") : a->property("genko_attribute").toString());
        // (after 原稿を書き出す…, PC4 F-c's レイヤーからファイル… and PC3 C-a's パスを Illustrator へ…: test_gui_output)
        QCOMPARE(items.mid(items.size() - 5), (QStringList{QStringLiteral("act_export"), QStringLiteral("|"), QStringLiteral("act_layers_to_files"),
                                                           QStringLiteral("act_export_paths_ai"), QStringLiteral("act_export_lookup")}));
        QCOMPARE(book.window->action(QStringLiteral("act_export_lookup"))->text(), QStringLiteral("カラールックアップテーブル…"));
        // the panels in ウィンドウ
        QVERIFY(book.window->findChild<QDockWidget*>(QStringLiteral("色調補正")) != nullptr);
        QVERIFY(book.window->findChild<QDockWidget*>(QStringLiteral("属性")) != nullptr);
    }

    // レベル補正… (Ctrl+L): the window starts in PhotoCraft's form (the user's decision 3); its channels the page's (RGB
    // here, C/M/Y/K on a CMYK page); the page shows the result while it is open; かける applies one filter_raster; 自動
    // fills the channels' records.
    void levelsWindow() {
        Book book;
        gui_test::Answers answers;
        const std::string before = book.pixels("pic");
        bool previewed = false, automatic = false;
        QStringList channels;
        auto* canvas = book.window->canvas();
        answers.responder->exec = answer([&](QDialog* dialog) {
            auto* method = dialog->findChild<QComboBox*>(QStringLiteral("field_native.method"));
            QVERIFY(method != nullptr && method->currentIndex() == 0);  // (Photoshop 型)
            auto* which = dialog->findChild<QComboBox*>(QStringLiteral("adjust_channel"));
            for (int i = 0; i < which->count(); ++i) channels << which->itemText(i);
            dialog->findChild<QDoubleSpinBox*>(QStringLiteral("field_black"))->setValue(40);
            previewed = gui_test::wait_for([&] { return canvas->doc() != book.session->snapshot(); }, 10000);
            if (auto* button = dialog->findChild<QPushButton*>(QStringLiteral("adjust_auto"))) {
                button->click();
                automatic = dialog->findChild<QComboBox*>(QStringLiteral("adjust_channel")) != nullptr;
            }
            dialog->findChild<QDoubleSpinBox*>(QStringLiteral("field_black"))->setValue(40);
        });
        book.window->action(QStringLiteral("act_adjust_levels"))->trigger();
        QVERIFY(previewed);
        QVERIFY(automatic);
        QCOMPARE(channels, (QStringList{QStringLiteral("赤"), QStringLiteral("緑"), QStringLiteral("青")}));
        QVERIFY(book.pixels("pic") != before);
        QTRY_VERIFY(canvas->doc() == book.session->snapshot());
        book.undo();
        QVERIFY(book.pixels("pic") == before);
        // やめる: nothing
        answers.responder->exec = answer(nullptr, false);
        book.window->action(QStringLiteral("act_adjust_levels"))->trigger();
        QVERIFY(book.pixels("pic") == before);

        Book cmyk(book_doc("cmyk"));
        channels.clear();
        answers.responder->exec = answer([&](QDialog* dialog) {
            auto* which = dialog->findChild<QComboBox*>(QStringLiteral("adjust_channel"));
            for (int i = 0; i < which->count(); ++i) channels << which->itemText(i);
            which->setCurrentIndex(3);  // (black)
            dialog->findChild<QDoubleSpinBox*>(QStringLiteral("adjust_channel_out_white"))->setValue(128);
        });
        const std::string inks = cmyk.pixels("pic");
        cmyk.window->action(QStringLiteral("act_adjust_levels"))->trigger();
        QCOMPARE(channels, (QStringList{QStringLiteral("シアン"), QStringLiteral("マゼンタ"), QStringLiteral("イエロー"), QStringLiteral("ブラック")}));
        QVERIFY(cmyk.pixels("pic") != inks);
    }

    // トーンカーブ… (Ctrl+M), 階調の反転 (Ctrl+I), 平均化, 自動: each one op on the layer drawn on, undone in one step.
    void otherCommands() {
        Book book;
        gui_test::Answers answers;
        const std::string before = book.pixels("pic");
        for (const char* name : {"act_adjust_curves", "act_adjust_invert", "act_adjust_equalize", "act_auto_tone", "act_auto_contrast", "act_auto_color",
                                 "act_adjust_posterize", "act_adjust_brightness", "act_adjust_threshold", "act_adjust_gradient_map", "act_adjust_exposure"}) {
            bool curve_shown = false;
            answers.responder->exec = answer([&curve_shown](QDialog* dialog) {
                curve_shown = dialog->findChild<QWidget*>(QStringLiteral("curve_editor")) != nullptr;
                if (auto* spin = dialog->findChild<QDoubleSpinBox*>(QStringLiteral("field_brightness"))) spin->setValue(40);
                if (auto* spin = dialog->findChild<QDoubleSpinBox*>(QStringLiteral("field_exposure"))) spin->setValue(1);
            });
            book.window->action(QString::fromLatin1(name))->trigger();
            if (std::string(name) == "act_adjust_curves") QVERIFY(curve_shown);
            if (std::string(name) != "act_adjust_curves") QVERIFY2(book.pixels("pic") != before, name);  // (a straight curve changes nothing)
            book.undo();
            QVERIFY2(book.pixels("pic") == before, name);
        }
    }

    // カラールックアップ…: a .cube read from a file (sent as its text), the look's table on the layer.
    void lookupFromAFile() {
        Book book;
        gui_test::Answers answers;
        std::string cube = "LUT_3D_SIZE 2\n";
        for (int b = 0; b < 2; ++b)
            for (int g = 0; g < 2; ++g)
                for (int r = 0; r < 2; ++r) cube += std::to_string(1 - r) + " " + std::to_string(1 - g) + " " + std::to_string(1 - b) + "\n";
        const QString file = book.tmp.path() + QStringLiteral("/inv.cube");
        genko::test::write_bytes(file, cube);
        answers.responder->open_path = [file](const QString&, const QString&) { return file; };
        answers.responder->exec = answer([](QDialog* dialog) {
            dialog->findChild<QPushButton*>(QStringLiteral("adjust_lut_load"))->click();
            QVERIFY(dialog->findChild<QLabel*>(QStringLiteral("adjust_lut_name"))->text().contains(QStringLiteral("inv.cube")));
        });
        const std::string before = book.pixels("pic");
        book.window->action(QStringLiteral("act_adjust_lookup"))->trigger();
        QVERIFY(book.pixels("pic") != before);
    }

    // ファイル → カラールックアップテーブルを書き出す…: the page's correction layers as a .cube of the size asked, where
    // the person chose; a page without one says so and writes nothing.
    void exportLookup() {
        Book book;
        gui_test::Answers answers;
        const QString file = book.tmp.path() + QStringLiteral("/look.cube");
        int asked = 0;
        answers.responder->get_int = [&asked](const QString&, const QString&, int value, int lo, int hi) {
            ++asked;
            if (value != 33 || lo != 2 || hi != 256) return std::optional<int>();
            return std::optional<int>(5);
        };
        answers.responder->save_path = [file](const QString&, const QString&) { return file; };
        book.window->action(QStringLiteral("act_export_lookup"))->trigger();
        QCOMPARE(asked, 0);  // (no correction layer: nothing asked)
        QVERIFY(!QFileInfo::exists(file));
        QVERIFY(book.window->apply_ops(Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"}, {"id", "inv"},
                                                         {"adjust", Json{{"kind", "invert"}}}}})));
        book.window->action(QStringLiteral("act_export_lookup"))->trigger();
        QCOMPARE(asked, 1);
        const core::lut3d::Table table = core::lut3d::parse_cube(genko::test::read_bytes(file));
        QCOMPARE(table.size, std::size_t(5));
        QCOMPARE(table.data.size(), std::size_t(5 * 5 * 5 * 3));
        for (std::size_t i = 0; i < 5 * 5 * 5; ++i) {  // (every colour of the lattice inverted; red fastest, then green, blue)
            const std::size_t at[3] = {i % 5, (i / 5) % 5, i / 25};
            for (std::size_t c = 0; c < 3; ++c) {
                const float want = 1.0f - static_cast<float>(at[c]) / 4.0f;
                QVERIFY2(std::abs(table.data[i * 3 + c] - want) < 1e-5f, qPrintable(QStringLiteral("%1.%2: %3").arg(i).arg(c).arg(table.data[i * 3 + c])));
            }
        }
    }

    // The 色調補正 panel: a button makes a correction layer over the layer drawn on, in PhotoCraft's form, and brings up
    // 属性; there a change is shown at once and applied as one set_adjustment — one Undo.
    void panels() {
        Book book;
        gui_test::Answers answers;
        auto* adjustments = book.window->findChild<QDockWidget*>(QStringLiteral("色調補正"));
        auto* properties = book.window->findChild<QDockWidget*>(QStringLiteral("属性"));
        adjustments->show();
        auto* button = adjustments->findChild<QPushButton*>(QStringLiteral("adjustment_levels"));
        QVERIFY(button != nullptr);
        button->click();
        const core::Layer* made = book.window->target_layer();
        QVERIFY(made != nullptr && made->kind == core::LayerKind::Adjust);
        QCOMPARE(*made->adjust, (Json{{"kind", "levels"}, {"native", Json{{"method", "upstream"}}}}));
        QVERIFY(properties->isVisible());
        const std::string id = made->id;
        auto* panel = qobject_cast<app::adjust_forms::PropertiesPanel*>(properties->widget());
        QVERIFY(panel != nullptr);
        panel->refresh();
        QVERIFY(panel->form() != nullptr);
        auto* black = panel->form()->findChild<QDoubleSpinBox*>(QStringLiteral("field_black"));
        black->setValue(20);
        black->setValue(30);
        panel->commit();
        QCOMPARE((*book.layer(id)->adjust)["black"], Json(30));
        QCOMPARE((*book.layer(id)->adjust)["native"]["method"], Json("upstream"));
        panel->blend()->setCurrentIndex(panel->blend()->findData(QStringLiteral("multiply")));
        panel->commit();
        QCOMPARE((*book.layer(id)->adjust)["native"]["blend"], Json("multiply"));
        book.undo();
        QVERIFY(!(*book.layer(id)->adjust)["native"].contains("blend"));
        book.undo();  // (the numbers: one step)
        QVERIFY(!book.layer(id)->adjust->contains("black"));
        // another layer: its opacity
        book.window->set_target_layer("pic");
        panel->refresh();
        QVERIFY(panel->form() == nullptr);
        panel->opacity()->setValue(50);
        panel->commit();
        QCOMPARE(book.layer("pic")->opacity, 0.5);
        // the layer panel's 色調補正のレイヤー starts a new one in PhotoCraft's form too (the user's decision 3)
        answers.responder->exec = answer([](QDialog* dialog) { dialog->findChild<QDoubleSpinBox*>(QStringLiteral("field_brightness"))->setValue(25); });
        book.window->layer_panel()->add_adjust("brightness_contrast");
        QCOMPARE((*book.window->target_layer()->adjust)["native"]["method"], Json("modern"));
        QCOMPARE((*book.window->target_layer()->adjust)["brightness"], Json(25));
    }

    // (the user's decision 3: an existing correction keeps its own form) 属性 sends only what is changed: Python's forms —
    // a tone curve of a gamma (no points), a levels' gamma of three places, a gradient map's colours — stay as they are
    // when only the opacity or the blend is changed (no set_adjustment at all for the opacity).
    void propertiesChangeOnlyWhatIsChanged() {
        auto doc = book_doc();
        const std::vector<std::pair<std::string, Json>> kept{
            {"gam", Json{{"kind", "curve"}, {"gamma", 2.0}}},
            {"lev", Json{{"kind", "levels"}, {"black", 10}, {"gamma", 1.234}}},
            {"map", Json{{"kind", "gradient_map"}, {"colors", Json::array({Json::array({0, 0, 80}), Json::array({255, 240, 200})})}}}};
        Json ops = Json::array();
        for (const auto& [id, adjust] : kept) ops.push_back(Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"}, {"id", id}, {"adjust", adjust}});
        doc = core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor("human:tester")).doc;
        Book book(doc);
        auto* properties = book.window->findChild<QDockWidget*>(QStringLiteral("属性"));
        properties->show();
        auto* panel = qobject_cast<app::adjust_forms::PropertiesPanel*>(properties->widget());
        QVERIFY(panel != nullptr);
        for (const auto& [id, adjust] : kept) {
            book.window->set_target_layer(id);
            panel->refresh();
            QVERIFY(panel->form() != nullptr);
            panel->opacity()->setValue(60);
            panel->commit();
            QCOMPARE(book.layer(id)->opacity, 0.6);
            QVERIFY2(*book.layer(id)->adjust == adjust, (id + ": " + book.layer(id)->adjust->dump()).c_str());
            panel->refresh();
            panel->blend()->setCurrentIndex(panel->blend()->findData(QStringLiteral("multiply")));
            panel->commit();
            Json blended = adjust;
            blended["native"] = Json{{"blend", "multiply"}};
            QVERIFY2(*book.layer(id)->adjust == blended, (id + ": " + book.layer(id)->adjust->dump()).c_str());
        }
    }

    // 属性: a channel record set back to doing nothing is taken out while another stays (levels' numbers, a tone curve's
    // points made straight).
    void propertiesTakeOutARecord() {
        auto doc = book_doc();
        const Json straight = Json::array({Json::array({0, 0}), Json::array({255, 255})});
        Json ops = Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"}, {"id", "lev"},
                                     {"adjust", Json{{"kind", "levels"}, {"native", Json{{"space", "rgb"}, {"channels", Json{{"red", Json{{"black", 20}}}, {"green", Json{{"black", 40}}}}}}}}}},
                                Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"}, {"id", "crv"},
                                     {"adjust", Json{{"kind", "curve"}, {"points", straight},
                                                     {"native", Json{{"space", "rgb"}, {"channels", Json{{"red", Json::array({Json::array({0, 20}), Json::array({255, 255})})},
                                                                                                        {"green", Json::array({Json::array({0, 40}), Json::array({255, 255})})}}}}}}}}});
        doc = core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor("human:tester")).doc;
        Book book(doc);
        auto* properties = book.window->findChild<QDockWidget*>(QStringLiteral("属性"));
        properties->show();
        auto* panel = qobject_cast<app::adjust_forms::PropertiesPanel*>(properties->widget());
        QVERIFY(panel != nullptr);
        // levels: green's black back to 0
        book.window->set_target_layer("lev");
        panel->refresh();
        auto* which = panel->form()->findChild<QComboBox*>(QStringLiteral("adjust_channel"));
        which->setCurrentIndex(which->findData(QStringLiteral("green")));
        panel->form()->findChild<QDoubleSpinBox*>(QStringLiteral("adjust_channel_black"))->setValue(0);
        panel->commit();
        Json channels = (*book.layer("lev")->adjust)["native"]["channels"];
        QVERIFY2(channels.contains("red") && !channels.contains("green"), channels.dump().c_str());
        // a tone curve: green's points straight
        book.window->set_target_layer("crv");
        panel->refresh();
        which = panel->form()->findChild<QComboBox*>(QStringLiteral("adjust_channel"));
        which->setCurrentIndex(which->findData(QStringLiteral("green")));
        auto* own = panel->form()->findChild<app::CurveEditor*>(QStringLiteral("adjust_channel_curve"));
        own->set_points({{0, 0}, {255, 255}});
        emit own->changed();
        panel->commit();
        channels = (*book.layer("crv")->adjust)["native"]["channels"];
        QVERIFY2(channels.contains("red") && !channels.contains("green"), channels.dump().c_str());
    }

    // The formula's choice says it in the person's words: Photoshop's or Genko's (not the maker's word 上流).
    void formulaInPlainWords() {
        for (const std::string kind : {"levels", "curve"}) {
            app::adjust_forms::Form form;
            app::adjust_forms::form_kind(kind)->build(form, {});
            auto* method = form.findChild<QComboBox*>(QStringLiteral("field_native.method"));
            QVERIFY(method != nullptr);
            for (int i = 0; i < method->count(); ++i) QVERIFY2(!method->itemText(i).contains(QStringLiteral("上流")), qPrintable(method->itemText(i)));
        }
    }
};

QTEST_MAIN(TestGuiAdjust)
#include "test_gui_adjust.moc"
