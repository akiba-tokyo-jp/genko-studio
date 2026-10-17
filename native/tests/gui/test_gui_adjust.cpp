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
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>

#include <cmath>
#include <set>

#include "gui_support.hpp"
#include "app/adjust_forms.hpp"
#include "app/adjust_panels.hpp"
#include "app/canvas.hpp"
#include "app/layer_panel.hpp"
#include "app/main_window.hpp"
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
    // command of the window; カラールックアップテーブルを書き出す… in ファイル, its own group after 印刷….
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
        // (the ones a correction layer of the same kind is named by say what they do: no two commands of the window have the
        // same words, test_gui_ledger_a)
        for (const char* text : {"明るさ・コントラストを変える…", "レベル補正をかける…", "トーンカーブをかける…", "露光量…", "カラールックアップをかける…",
                                 "階調の反転", "階調化する…", "2 値化する…", "グラデーションマップをかける…", "平均化（イコライズ）", "自動トーン補正",
                                 "自動コントラスト", "自動カラー補正"}) {
            QVERIFY2(texts.contains(QString::fromUtf8(text)), text);
        }
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
        QMenu* file = book.menu(QStringLiteral("ファイル"));
        QStringList items;
        for (QAction* a : file->actions()) items << (a->isSeparator() ? QStringLiteral("|") : a->property("genko_attribute").toString());
        // (its own group after 書き出し… and 印刷…, which stay together as in Python's menu: test_gui_output)
        const qsizetype print = items.indexOf(QStringLiteral("act_print"));
        QCOMPARE(items.indexOf(QStringLiteral("act_export")), print - 1);
        QCOMPARE(items.mid(print + 1, 3), (QStringList{QStringLiteral("|"), QStringLiteral("act_export_lookup"), QStringLiteral("|")}));
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
};

QTEST_MAIN(TestGuiAdjust)
#include "test_gui_adjust.moc"
