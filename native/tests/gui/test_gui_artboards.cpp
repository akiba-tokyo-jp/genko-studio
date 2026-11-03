// PC5 I-c (artboards@1, SPEC.md AUTO-01「アートボード」) in the window: the commands in their places (レイヤー → 新規 ▸ アートボード…・
// グループからのアートボード…・レイヤーからのアートボード… before レイヤーからのフレーム…, イメージ → アートボードの設定… after カンバス
// サイズ…, 表示 → アートボードを画面サイズに合わせる after 画面サイズに合わせる and 選択したアートボードのガイドを消去 after ガイドを消去,
// 表示・非表示 ▸ アートボードガイド・アートボード名 after カンバスガイド, 書き出し ▸ アートボードからファイル…・PDF… before レイヤーから
// ファイル…); アートボード… asked in its window (a preset, a name) making the page and showing it, the pages panel's mark;
// アートボードの設定… (its background, its size); レイヤーからのアートボード… (the name asked); the artboard's guides shown by
// their own switch and cleared; アートボードを画面サイズに合わせる; アートボードからファイル… and PDF… writing their files; each
// change one Undo.

#include <QtTest>

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QSpinBox>
#include <QTemporaryDir>

#include <memory>
#include <string>

#include "app/artboard_dialog.hpp"
#include "app/canvas.hpp"
#include "app/layer_panel.hpp"
#include "app/main_window.hpp"
#include "app/pages_panel.hpp"
#include "render/page.hpp"
#include "gui_support.hpp"

using namespace genko;
using core::Json;

namespace {

struct Book {
    QTemporaryDir tmp;
    std::filesystem::path path;
    std::unique_ptr<app::MainWindow> window;
    gui_test::Answers answers;

    Book() {
        (void)gui_test::config_folder();
        path = gui_test::path_of(tmp.path() + QStringLiteral("/b.genko"));
        core::PageSpec spec = core::PageSpec::b4_comic();
        spec.dpi = core::Num(72);
        gui_test::write_book(path, core::new_episode("t", core::Num(1), 2, spec));
        window = std::make_unique<app::MainWindow>(app::Session::open(path, gui_test::quick(gui_test::path_of(tmp.path() + QStringLiteral("/recovery")))));
        window->resize(1280, 800);
        window->show();
        for (int i = 0; i < 5; ++i) QCoreApplication::processEvents();
        answers.responder->question = [](const QString&, const QString&) { return true; };
    }
    QAction* act(const char* name) const { return window->action(QString::fromLatin1(name)); }
    const core::Document& doc() const { return window->book(); }
    bool apply(const Json& op) { return window->apply_ops(Json::array({op})); }
    QStringList items(QMenu* menu) const {
        QStringList out;
        for (QAction* a : menu->actions()) out << (a->isSeparator() ? QStringLiteral("|") : a->property("genko_attribute").toString());
        return out;
    }
    QMenu* menu_of(const QString& title) const {
        for (QAction* a : window->menuBar()->actions())
            if (a->text() == title) return a->menu();
        return nullptr;
    }
    QMenu* submenu(QMenu* in, const QString& title) const {
        for (QAction* a : in->actions())
            if (a->menu() != nullptr && a->text() == title) return a->menu();
        return nullptr;
    }
};

}  // namespace

class TestGuiArtboards : public QObject {
    Q_OBJECT
private slots:
    void theCommandsInTheirPlaces() {
        Book b;
        QMenu* layers = b.menu_of(QStringLiteral("レイヤー"));
        QVERIFY(layers != nullptr);
        const QStringList made = b.items(b.submenu(layers, QStringLiteral("新規")));
        const int frame = made.indexOf(QStringLiteral("act_layer_frame"));
        QVERIFY(frame >= 3);
        QCOMPARE(made.mid(frame - 3, 3), (QStringList{QStringLiteral("act_layer_artboard"), QStringLiteral("act_layer_artboard_from_group"),
                                                       QStringLiteral("act_layer_artboard_from_layers")}));
        QCOMPARE(made.at(frame - 4), QStringLiteral("act_layer_group_from_layers"));
        const QStringList pages = b.items(b.menu_of(QStringLiteral("ページ")));
        QCOMPARE(pages.indexOf(QStringLiteral("act_artboard_settings")), pages.indexOf(QStringLiteral("act_canvas_size")) + 1);
        const QStringList view = b.items(b.menu_of(QStringLiteral("表示")));
        QCOMPARE(view.indexOf(QStringLiteral("act_fit_artboard")), view.indexOf(QStringLiteral("act_fit")) + 1);
        QCOMPARE(view.indexOf(QStringLiteral("act_clear_artboard_guides")), view.indexOf(QStringLiteral("act_clear_guides")) + 1);
        const QStringList shown = b.items(b.window->findChild<QMenu*>(QStringLiteral("menu_show")));
        QCOMPARE(shown.indexOf(QStringLiteral("act_show_artboard_guides")), shown.indexOf(QStringLiteral("act_show_canvas_guides")) + 1);
        QCOMPARE(shown.indexOf(QStringLiteral("act_show_artboard_names")), shown.indexOf(QStringLiteral("act_show_canvas_guides")) + 2);
        const QStringList exports = b.items(b.window->findChild<QMenu*>(QStringLiteral("menu_export")));
        QCOMPARE(exports.indexOf(QStringLiteral("act_artboards_to_files")), exports.indexOf(QStringLiteral("act_layers_to_files")) - 2);
        QCOMPARE(exports.indexOf(QStringLiteral("act_artboards_to_pdf")), exports.indexOf(QStringLiteral("act_layers_to_files")) - 1);
        // Photoshop's names
        QCOMPARE(b.act("act_layer_artboard")->text(), QStringLiteral("アートボード…"));
        QCOMPARE(b.act("act_layer_artboard_from_group")->text(), QStringLiteral("グループからのアートボード…"));
        QCOMPARE(b.act("act_layer_artboard_from_layers")->text(), QStringLiteral("レイヤーからのアートボード…"));
        QCOMPARE(b.act("act_fit_artboard")->text(), QStringLiteral("アートボードを画面サイズに合わせる"));
        QCOMPARE(b.act("act_clear_artboard_guides")->text(), QStringLiteral("選択したアートボードのガイドを消去"));
        QCOMPARE(b.act("act_show_artboard_guides")->text(), QStringLiteral("アートボードガイド"));
        QCOMPARE(b.act("act_show_artboard_names")->text(), QStringLiteral("アートボード名"));
        QCOMPARE(b.act("act_artboards_to_files")->text(), QStringLiteral("アートボードからファイル…"));
        QCOMPARE(b.act("act_artboards_to_pdf")->text(), QStringLiteral("アートボードから PDF…"));
    }

    void makingAndSettingArtboards() {
        Book b;
        QStringList asked;
        b.answers.responder->exec = [&asked](QDialog* dialog) {
            asked << dialog->objectName();
            auto* preset = dialog->findChild<QComboBox*>(QStringLiteral("artboard_preset"));
            preset->setCurrentIndex(preset->findData(QStringLiteral("Web 1280")));
            dialog->findChild<QLineEdit*>(QStringLiteral("artboard_name"))->setText(QStringLiteral("ヘッダー"));
            return static_cast<int>(QDialog::Accepted);
        };
        b.act("act_layer_artboard")->trigger();
        QCOMPARE(asked, QStringList{QStringLiteral("artboardDialog")});
        QCOMPARE(b.doc().pages.size(), std::size_t(3));
        const core::Page& made = *b.doc().pages[1];
        QVERIFY(made.artboard);
        QCOMPARE(made.artboard->name, std::string("ヘッダー"));
        QCOMPARE(made.artboard->preset.value_or(""), std::string("Web 1280"));
        QCOMPARE(b.window->page_index(), 1);
        QVERIFY(app::page_text(made).contains(QStringLiteral("アートボード「ヘッダー」")));
        // its settings: black, wider from the left
        b.answers.responder->exec = [](QDialog* dialog) {
            auto* background = dialog->findChild<QComboBox*>(QStringLiteral("artboard_background"));
            background->setCurrentIndex(background->findData(QStringLiteral("black")));
            dialog->findChild<QSpinBox*>(QStringLiteral("artboard_width"))->setValue(1300);
            return static_cast<int>(QDialog::Accepted);
        };
        b.act("act_artboard_settings")->trigger();
        const core::Page& set = *b.doc().pages[1];
        QCOMPARE(set.artboard->background, std::string("black"));
        QVERIFY(!set.artboard->preset);
        QCOMPARE(render::mm_to_px(set.spec.width_mm.value(), 72), 1300);
        // fit on screen: the artboard fills the view
        b.act("act_fit_artboard")->trigger();
        // undone, one step each
        b.act("act_undo")->trigger();
        QCOMPARE(b.doc().pages[1]->artboard->background, std::string("white"));
        b.act("act_undo")->trigger();
        QCOMPARE(b.doc().pages.size(), std::size_t(2));
        // on a page that is no artboard: said, nothing asked
        b.window->go_to_page(0);
        int windows = 0;
        b.answers.responder->exec = [&windows](QDialog*) {
            ++windows;
            return 0;
        };
        b.act("act_artboard_settings")->trigger();
        QCOMPARE(windows, 0);
    }

    void fromLayersGuidesAndFiles() {
        Book b;
        QVERIFY(b.apply(Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "p"}, {"name", "p"}}));
        QVERIFY(b.apply(Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", "p"},
                             {"area", Json{{"poly", Json::array({Json::array({50, 60}), Json::array({100, 60}), Json::array({100, 90}), Json::array({50, 90})})}}},
                             {"rgb", Json::array({255, 0, 0})}}));
        b.window->go_to_page(0);
        b.window->layer_panel()->select_ids({"p"});
        b.answers.responder->get_text = [](const QString&, const QString&, const QString&) { return std::optional<QString>(QStringLiteral("赤い板")); };
        b.act("act_layer_artboard_from_layers")->trigger();
        QCOMPARE(b.doc().pages.size(), std::size_t(3));
        const core::Page& board = *b.doc().pages[1];
        QVERIFY(board.artboard);
        QCOMPARE(board.artboard->name, std::string("赤い板"));
        std::string titles;
        for (const core::Layer& l : board.layers) titles += l.title + "/" + std::to_string(int(l.kind)) + " ";
        QVERIFY2(std::abs(board.spec.width_mm.value() - 50) < 0.3, (board.spec.width_mm.repr() + " " + titles).c_str());
        QCOMPARE(b.window->page_index(), 1);
        // its guides: by アートボードガイド (not カンバスガイド), cleared by 選択したアートボードのガイドを消去
        QVERIFY(b.apply(Json{{"op", "add_ruler"}, {"page", 2}, {"kind", "guide"}, {"axis", "v"}, {"at", 10}}));
        app::PageCanvas* canvas = b.window->canvas();
        QVERIFY(canvas->page_guides_shown());
        b.act("act_show_canvas_guides")->setChecked(false);
        QVERIFY(canvas->page_guides_shown());
        b.act("act_show_artboard_guides")->setChecked(false);
        QVERIFY(!canvas->page_guides_shown());
        b.act("act_show_artboard_guides")->setChecked(true);
        b.act("act_show_canvas_guides")->setChecked(true);
        b.act("act_show_artboard_names")->setChecked(false);
        QVERIFY(!canvas->show_artboard_names);
        b.act("act_show_artboard_names")->setChecked(true);
        b.act("act_clear_artboard_guides")->trigger();
        const core::Page& cleared = *b.doc().pages[1];
        QVERIFY(std::none_of(cleared.rulers.begin(), cleared.rulers.end(), [](const Json& r) { return r.value("kind", Json()) == Json("guide"); }));
        // アートボードからファイル… and PDF…
        const QString out = b.tmp.path() + QStringLiteral("/out");
        b.answers.responder->exec = [&out](QDialog* dialog) {
            dialog->findChild<QLineEdit*>(QStringLiteral("artboards_where"))->setText(out);
            if (auto* prefix = dialog->findChild<QLineEdit*>(QStringLiteral("artboards_prefix"))) prefix->setText(QStringLiteral("x"));
            return static_cast<int>(QDialog::Accepted);
        };
        b.act("act_artboards_to_files")->trigger();
        QVERIFY(gui_test::wait_for([&] { return QFile::exists(out + QStringLiteral("/x_赤い板.png")); }));
        const QString pdf = b.tmp.path() + QStringLiteral("/boards.pdf");
        b.answers.responder->exec = [&pdf](QDialog* dialog) {
            dialog->findChild<QLineEdit*>(QStringLiteral("artboards_where"))->setText(pdf);
            return static_cast<int>(QDialog::Accepted);
        };
        b.act("act_artboards_to_pdf")->trigger();
        QVERIFY(gui_test::wait_for([&] { return QFile::exists(pdf); }));
        // the book unchanged by them
        QCOMPARE(b.doc().pages.size(), std::size_t(3));
    }
};

QTEST_MAIN(TestGuiArtboards)
#include "test_gui_artboards.moc"
