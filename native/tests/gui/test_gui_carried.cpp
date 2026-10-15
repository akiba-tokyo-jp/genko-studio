// The features carried over to M6 (Python's test_h4, test_i0, test_f6, test_m10, test_j9): オートアクション (recorded,
// played on another page with new ids as one step to undo, aimed at the layer drawn on and the chosen panel, managed,
// played on every page), 画像を読み込む (into the chosen panel, kept in the book), マスクを編集する (the pen paints the
// mask, not lines), 枠線の間隔・破線の長さ・揺れ, and 本の形でプレビュー (the spreads as the book is bound, a page turning).

#include <QtTest>

#include <QDialog>
#include <QDoubleSpinBox>
#include <QImage>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QTextBrowser>
#include <QPainter>
#include <QRegularExpression>

#include <memory>

#include "app/actions.hpp"
#include "app/bookview.hpp"
#include "app/canvas.hpp"
#include "app/config.hpp"
#include "app/help.hpp"
#include "app/inject.hpp"
#include "app/layer_panel.hpp"
#include "app/main_window.hpp"
#include "gui_support.hpp"
#include "storage/reader.hpp"

using namespace genko;
using core::Json;

namespace {

struct Book {
    QTemporaryDir tmp;
    std::filesystem::path path;
    std::unique_ptr<app::MainWindow> window;
    gui_test::Answers answers;
    std::function<int(QDialog*)> on_dialog;

    explicit Book(core::Document doc = core::new_episode("t", core::Num(1), 3, core::PageSpec::b4_comic())) {
        (void)gui_test::config_folder();
        path = gui_test::path_of(tmp.path() + QStringLiteral("/b.genko"));
        gui_test::write_book(path, doc);
        window = std::make_unique<app::MainWindow>(app::Session::open(path, gui_test::quick(gui_test::path_of(tmp.path() + QStringLiteral("/recovery")))));
        window->resize(1280, 800);
        window->show();
        for (int i = 0; i < 5; ++i) QCoreApplication::processEvents();
        answers.responder->exec = [this](QDialog* dialog) { return on_dialog ? on_dialog(dialog) : 0; };
        answers.responder->question = [](const QString&, const QString&) { return true; };
    }
    QAction* act(const char* name) const { return window->action(QString::fromLatin1(name)); }
    const core::Document& doc() const { return window->book(); }
    const core::Page& page(std::size_t row) const { return *doc().pages[row]; }
    const core::Layer* layer(std::size_t row, const std::string& id) const {
        for (const core::Layer& l : page(row).layers) {
            if (l.id == id) return &l;
        }
        return nullptr;
    }
};

}  // namespace

class TestGuiCarried : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { (void)gui_test::config_folder(); }

    // --- オートアクション -------------------------------------------------------------------------------------------

    void recordAndPlayAnActionOnAnotherPage() {
        Book book;
        app::MainWindow* w = book.window.get();
        w->start_recording();
        QVERIFY(w->status_label()->text().contains(QStringLiteral("記録中")));
        QVERIFY(w->apply_ops(Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"name", "影"}, {"id", "shade"}, {"blend", "multiply"}},
                                          Json{{"op", "set_layer"}, {"page", 1}, {"id", "shade"}, {"opacity", 0.5}}})));
        QVERIFY(w->apply_ops(Json::array({Json{{"op", "add_effect"}, {"page", 1}, {"kind", "focus"}, {"frame_id", book.page(0).leaf_frames()[0]->id}}})));
        QVERIFY(w->stop_recording(QStringLiteral("影と集中線")));
        QVERIFY(!w->recording());
        const Json saved = app::actions::load()["影と集中線"]["ops"];
        QCOMPARE(saved.size(), std::size_t(3));
        QCOMPARE(saved[0]["op"].get<std::string>(), std::string("add_layer"));
        QCOMPARE(saved[2]["op"].get<std::string>(), std::string("add_effect"));
        QVERIFY(QFileInfo::exists(gui_test::qpath(app::config_dir() / "actions.json")));
        // played on page 3: a layer of its own (a new id), as one step to undo
        w->go_to_page(3);
        const std::size_t before = book.page(2).layers.size();
        const std::size_t steps = w->session().history().done.size();
        QVERIFY(w->play_action(QStringLiteral("影と集中線")));
        std::vector<const core::Layer*> added;
        for (const core::Layer& l : book.page(2).layers) {
            if (l.title == "影") added.push_back(&l);
        }
        QVERIFY(book.page(2).layers.size() == before + 1 && added.size() == 1 && added[0]->opacity == 0.5 && added[0]->id != "shade");
        QVERIFY(!book.page(2).effects.empty());
        QCOMPARE(w->session().history().done.size(), steps + 1);  // (one undo takes it all back)
        QVERIFY(w->play_action(QStringLiteral("影と集中線")));  // (again: new ids each time, no clash)
        int shades = 0;
        for (const core::Layer& l : book.page(2).layers) shades += l.title == "影" ? 1 : 0;
        QCOMPARE(shades, 2);
        // the menu
        emit w->actions_menu()->aboutToShow();
        QStringList titles;
        for (QAction* a : w->actions_menu()->actions()) titles << a->text();
        QVERIFY(titles.contains(QStringLiteral("実行: 影と集中線")));
        // every page but the covers, as one step
        const std::size_t layers_on_two = book.page(1).layers.size();
        const std::size_t before_all = w->session().history().done.size();
        QVERIFY(w->play_action_on_all(QStringLiteral("影と集中線")));
        QCOMPARE(book.page(1).layers.size(), layers_on_two + 1);
        QCOMPARE(w->session().history().done.size(), before_all + 1);  // (every page in one undo)
        QVERIFY(!app::actions::remove(QStringLiteral("影と集中線")));
        QVERIFY(!app::actions::load().contains("影と集中線"));
        // nothing recorded: nothing kept
        w->start_recording();
        QVERIFY(!w->stop_recording(QStringLiteral("空")));
        w->start_recording();
        w->drop_recording();
        QVERIFY(!w->recording() && !w->status_label()->text().contains(QStringLiteral("記録中")));
    }

    void replayAimsAtTheCurrentLayerAndPanel() {
        const Json ops = Json::array({Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", "old"}, {"points", Json::array({Json::array({1, 1}), Json::array({2, 2})})}},
                                      Json{{"op", "filter_raster"}, {"page", 1}, {"id", "old"}, {"kind", "blur"}},
                                      Json{{"op", "add_tone"}, {"page", 1}, {"frame_id", "f_old"}, {"density", 0.2}}});
        const Json out = app::actions::replay(ops, Json(5), std::string("now"), std::nullopt);
        QVERIFY(out[0]["page"] == 5 && out[0]["layer_id"] == "now" && out[1]["id"] == "now");
        QVERIFY(!out[2].contains("frame_id"));
        QCOMPARE(app::actions::replay(ops, Json(5), std::string("now"), std::string("f_new"))[2]["frame_id"].get<std::string>(), std::string("f_new"));
        QVERIFY(app::actions::recordable(Json::array({Json{{"op", "undo"}}, Json{{"op", "add_page"}, {"count", 1}}})).empty());
        // what the recording made gets a new id each time, and the steps that use it follow
        int n = 0;
        const Json made = app::actions::replay(Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "s"}},
                                                            Json{{"op", "set_layer"}, {"page", 1}, {"id", "s"}, {"opacity", 0.5}}}),
                                               Json(2), std::string("drawn"), std::nullopt, [&n] { return "id" + std::to_string(++n); });
        QVERIFY(made[0]["id"] == "id1" && made[1]["id"] == "id1");
    }

    void theActionManagerShowsReordersAndLeavesOutSteps() {
        Book book;
        app::actions::store(QStringLiteral("影"), Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"name", "影"}, {"id", "s"}},
                                                             Json{{"op", "edit_line"}, {"id", "L9"}, {"text", "やあ"}},
                                                             Json{{"op", "set_layer"}, {"page", 1}, {"id", "s"}, {"opacity", 0.5}}}));
        QVERIFY((app::actions::page_bound(app::actions::load()["影"]["ops"]) == std::vector<bool>{false, true, false}));
        auto* dialog = qobject_cast<app::actions::ActionsDialog*>(book.window->manage_actions());
        QVERIFY(dialog != nullptr);
        QVERIFY(dialog->names->count() == 1 && dialog->steps->count() == 3);
        QVERIFY(dialog->steps->item(1)->text().contains(QStringLiteral("このページだけ")));
        dialog->steps->setCurrentRow(1);
        dialog->drop_step();
        QCOMPARE(app::actions::load()["影"]["ops"].size(), std::size_t(2));
        dialog->steps->setCurrentRow(1);
        dialog->move_step(-1);
        QCOMPARE(app::actions::load()["影"]["ops"][0]["op"].get<std::string>(), std::string("set_layer"));
        dialog->close();
        // page-bound steps are skipped, the rest still plays
        app::actions::store(QStringLiteral("混ぜ"), Json::array({Json{{"op", "edit_line"}, {"id", "L9"}, {"text", "x"}},
                                                              Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"name", "足した"}, {"id", "n"}}}));
        QVERIFY(book.window->play_action(QStringLiteral("混ぜ")));
        bool found = false;
        for (const core::Layer& l : book.window->current_page()->layers) found = found || l.title == "足した";
        QVERIFY(found);
        for (const char* name : {"影", "混ぜ"}) app::actions::remove(QString::fromUtf8(name));
    }

    // --- 画像を読み込む --------------------------------------------------------------------------------------------

    void anImageGoesIntoTheSelectedPanel() {
        Book book;
        QImage picture(400, 300, QImage::Format_RGB32);
        picture.fill(Qt::white);
        {
            QPainter p(&picture);
            p.setPen(QPen(Qt::black, 8));
            p.drawEllipse(100, 50, 200, 200);
        }
        const QString file = book.tmp.path() + QStringLiteral("/pic.jpg");
        QVERIFY(picture.save(file, "JPEG"));
        const std::string frame = book.page(0).leaf_frames()[0]->id;
        QVERIFY(book.window->apply_ops(Json::array({Json{{"op", "select_frame"}, {"page", 1}, {"frame_id", frame}}})));
        // (ファイル → 画像を読み込む…: the file chosen in its dialog, answered here)
        QString filter;
        book.answers.responder->open_path = [&filter, file](const QString&, const QString& f) {
            filter = f;
            return file;
        };
        book.act("act_import")->trigger();
        QVERIFY2(filter.contains(QStringLiteral("*.jpg")) && filter.contains(QStringLiteral("*.psd")), qPrintable(filter));
        const core::Layer* placed = nullptr;
        for (const core::Layer& l : book.page(0).layers) {
            if (l.kind == core::LayerKind::Placed) placed = &l;
        }
        QVERIFY(placed != nullptr && placed->frame_id && *placed->frame_id == frame && placed->title == "pic");
        QVERIFY(book.window->session().wait_saved(std::chrono::milliseconds(30000)));
        bool on_disk = false;
        for (const core::Layer& l : gui_test::read_book(book.path).pages[0]->layers) on_disk = on_disk || l.kind == core::LayerKind::Placed;
        QVERIFY(on_disk);
        // not a picture: said, nothing placed
        const QString junk = book.tmp.path() + QStringLiteral("/junk.png");
        {
            QFile f(junk);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("not a picture");
        }
        const std::size_t layers = book.page(0).layers.size();
        QVERIFY(!book.window->import_image(junk));
        QCOMPARE(book.page(0).layers.size(), layers);
        QVERIFY(book.window->last_error().startsWith(QStringLiteral("読み込めない画像です")));
        // a book not saved yet: it asks for a place first
        app::MainWindow untitled;
        QVERIFY(!untitled.import_image(file));
    }

    // --- マスクを編集する --------------------------------------------------------------------------------------------

    void theMaskIsEditedWithThePen() {
        Book book;
        app::MainWindow* w = book.window.get();
        QVERIFY(w->apply_ops(Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"name", "u"}, {"id", "u"}}})));
        w->set_target_layer("u");
        app::LayerPanel* panel = w->layer_panel();
        panel->refresh();
        panel->mask(Json{{"area", Json{{"poly", Json::array({Json::array({0, 0}), Json::array({257, 0}), Json::array({257, 150}), Json::array({0, 150})})}}}});
        QVERIFY(book.layer(0, "u")->mask.has_value());
        panel->mask_edit_action()->setChecked(true);
        QVERIFY(w->mask_edit);
        const std::string before = *book.layer(0, "u")->mask->png;
        const std::size_t strokes = book.layer(0, "u")->strokes->items.size();
        book.act("act_pen")->trigger();
        app::inject::mouse_stroke(w->canvas(), {QPointF(60, 240), QPointF(62, 250), QPointF(64, 260)});
        QVERIFY(*book.layer(0, "u")->mask->png != before);  // (the mask changed, not the lines)
        QCOMPARE(book.layer(0, "u")->strokes->items.size(), strokes);
        panel->mask_edit_action()->setChecked(false);
        QVERIFY(!w->mask_edit);
        // a layer without a mask: one that shows everything first
        QVERIFY(w->apply_ops(Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"name", "v"}, {"id", "v"}}})));
        w->set_target_layer("v");
        panel->refresh();
        panel->mask_edit_action()->setChecked(true);
        QVERIFY(book.layer(0, "v")->mask.has_value());
        panel->mask_edit_action()->setChecked(false);
    }

    // --- 枠線の間隔・破線の長さ・揺れ ------------------------------------------------------------------------------------

    void theBorderDetail() {
        Book book;
        app::MainWindow* w = book.window.get();
        const std::string frame = book.page(0).leaf_frames()[0]->id;
        QVERIFY(w->apply_ops(Json::array({Json{{"op", "select_frame"}, {"page", 1}, {"frame_id", frame}}})));
        // a plain line: nothing to set
        book.on_dialog = [](QDialog*) { return int(QDialog::Accepted); };
        book.act("act_border_detail")->trigger();
        QVERIFY(w->last_notice().startsWith(QStringLiteral("実線には間隔も長さもありません")));
        // dashed: the gap and the dash are kept, the wobble is not
        w->border_kind_actions()[2]->trigger();  // (枠線: 破線)
        QCOMPARE(w->border_kind_actions()[2]->text(), QStringLiteral("枠線: 破線"));
        book.on_dialog = [](QDialog* dialog) {
            dialog->findChild<QDoubleSpinBox*>(QStringLiteral("gap_mm"))->setValue(2.345);
            dialog->findChild<QDoubleSpinBox*>(QStringLiteral("dash_mm"))->setValue(4.0);
            dialog->findChild<QDoubleSpinBox*>(QStringLiteral("wobble_mm"))->setValue(1.0);
            return int(QDialog::Accepted);
        };
        book.act("act_border_detail")->trigger();
        const core::Frame* f = book.page(0).find_frame(frame);
        QVERIFY(f != nullptr && f->line);
        QCOMPARE((*f->line)["kind"].get<std::string>(), std::string("dashed"));
        QCOMPARE((*f->line)["gap_mm"].get<double>(), 2.35);  // (round(…, 2))
        QCOMPARE((*f->line)["dash_mm"].get<double>(), 4.0);
        QVERIFY(!f->line->contains("wobble_mm"));
        // cancelled: unchanged
        book.on_dialog = [](QDialog*) { return int(QDialog::Rejected); };
        book.act("act_border_detail")->trigger();
        QCOMPARE((*book.page(0).find_frame(frame)->line)["gap_mm"].get<double>(), 2.35);
    }

    // --- ヘルプ (I10: made from the commands and their menus as they are) ------------------------------------------------

    void theHelpNamesTheMenusAndKeysAsTheyAre() {
        Book book;
        app::MainWindow* w = book.window.get();
        static const QRegularExpression token(QStringLiteral("\\{(menu|key|submenu|top|items)"));
        for (const char* text : {app::help::kGuide, app::help::kFaq}) {
            const app::help::Rendered done = app::help::render(w, QString::fromUtf8(text));
            QVERIFY2(done.missing.isEmpty(), qPrintable(done.missing.join(QStringLiteral(", "))));  // (every entrance named is in the menus)
            QVERIFY2(done.keyless.isEmpty(), qPrintable(done.keyless.join(QStringLiteral(", "))));  // (and every key named is there)
            QVERIFY(!done.html.contains(token) && !done.html.contains(QStringLiteral("act_")));
        }
        const QString guide = app::help::guide(w);
        QVERIFY(guide.contains(QStringLiteral("「ファイル → 新しい原稿…」")));
        QVERIFY(guide.contains(QStringLiteral("「ウィンドウ → 作業の段階」の「ネームの並び」「作画の並び」")));  // (where the stages are: Python's guide said 表示)
        QVERIFY(guide.contains(QStringLiteral("ペン（B）")));
        QVERIFY(guide.contains(QStringLiteral("[ と ] で太さ")));
        QVERIFY(guide.contains(QStringLiteral("「ページ → 台詞 → フキダシを手で描く」")));
        const QString faq = app::help::faq(w);
        QVERIFY(faq.contains(QStringLiteral("「ウィンドウ → ワークスペース → はじめの配置に戻す」")));  // (a menu filled as it opens)
        QVERIFY(faq.contains(QStringLiteral("「マスク ▾」、または「レイヤー → マスク → マスクを編集する（ペンで見せる・消しゴムで隠す）」")));
        QVERIFY(app::help::shortcut_html(w).contains(QStringLiteral("キーは「ファイル → 環境設定…」で変えられます")));
        // a key changed: the guide and the list say the new one
        book.act("act_pen")->setShortcut(QKeySequence(QStringLiteral("P")));
        QVERIFY(app::help::guide(w).contains(QStringLiteral("ペン（P）")));
        bool listed = false;
        for (const auto& [menu, command, keys] : app::help::shortcut_rows(w)) listed = listed || (command == QStringLiteral("ペン") && keys == QStringLiteral("P"));
        QVERIFY(listed);
        // keys taken away: said so, never the command's inner name; a key that is a sign of HTML: written as the sign
        book.act("act_pen")->setShortcuts({});
        const app::help::Rendered keyless = app::help::render(w, QString::fromUtf8(app::help::kGuide));
        QVERIFY(keyless.html.contains(QStringLiteral("ペン（キーなし）")) && !keyless.html.contains(QStringLiteral("act_")));
        QCOMPARE(keyless.keyless, QStringList{QStringLiteral("{key:act_pen}")});
        book.act("act_pen")->setShortcut(QKeySequence(Qt::Key_Less));
        QVERIFY(app::help::guide(w).contains(QStringLiteral("ペン（&lt;）")));
        QVERIFY(app::help::shortcut_html(w).contains(QStringLiteral("<td>ペン</td><td><b>&lt;</b></td>")));
        book.act("act_pen")->setShortcut(QKeySequence(QStringLiteral("B")));
        // a command not in the menus is found, never described (nor its inner name shown)
        const app::help::Rendered unknown = app::help::render(w, QStringLiteral("{menu:act_no_such_thing} {key:act_pen} {top:ありません}"));
        QCOMPARE(unknown.missing.size(), 2);
        QVERIFY(!unknown.html.contains(QStringLiteral("act_no_such_thing")));
        // the help menu, last of the bar, and its pages
        QCOMPARE(w->menuBar()->actions().back()->text(), QStringLiteral("ヘルプ"));
        book.act("act_help_guide")->trigger();
        auto* shown = qobject_cast<app::help::HelpDialog*>(w->help_dialog());
        QVERIFY(shown != nullptr && shown->text->toPlainText().contains(QStringLiteral("はじめての 1 冊")));
        book.act("act_help_faq")->trigger();
        shown = qobject_cast<app::help::HelpDialog*>(w->help_dialog());
        QVERIFY(shown != nullptr && shown->text->toPlainText().contains(QStringLiteral("線が描けない")));
        book.act("act_help_keys")->trigger();
        shown = qobject_cast<app::help::HelpDialog*>(w->help_dialog());
        QVERIFY(shown != nullptr && shown->text->toPlainText().contains(QStringLiteral("ペン")));
        QCOMPARE(book.act("act_help_keys")->shortcut(), QKeySequence(Qt::Key_F1));
        auto* keys_window = shown;
        book.act("act_about")->trigger();
        shown = qobject_cast<app::help::HelpDialog*>(w->help_dialog());
        QVERIFY(shown != nullptr && shown->text->toPlainText().contains(QStringLiteral("Genko Studio")));
        QVERIFY(keys_window->isVisible());  // (each its own window, as Python's: the list stays open beside the others)
        for (auto* help : w->findChildren<app::help::HelpDialog*>()) help->close();
    }

    // --- 本の形でプレビュー -------------------------------------------------------------------------------------------

    void spreadsOpenAsABook() {
        core::Document doc = core::new_episode("t", core::Num(1), 4, core::PageSpec::b4_comic());
        Book book(doc);
        QVERIFY(book.window->apply_ops(Json::array({Json{{"op", "add_cover"}, {"kind", "jacket"}, {"spine_mm", 6}, {"flap_mm", 0}}})));
        const auto pages = app::bookview::book_pages(book.doc());
        QStringList kinds;
        for (const auto& [kind, page] : pages) kinds << QString::fromStdString(kind);
        QCOMPARE(kinds, (QStringList{"front", "page", "page", "page", "page", "back"}));
        QVERIFY((app::bookview::spreads(static_cast<int>(pages.size())) == std::vector<std::vector<int>>{{0}, {1, 2}, {3, 4}, {5}}));
        // as bound (right): the front, page 1 alone, pages 2・3 facing, page 4 alone, the back
        QVERIFY((app::bookview::book_spreads(book.doc(), pages) == std::vector<std::vector<int>>{{0}, {1}, {2, 3}, {4}, {5}}));
    }

    void theBookPreviewTurns() {
        core::Document doc = core::new_episode("t", core::Num(1), 3, core::PageSpec::b4_comic());
        Book book(doc);
        QVERIFY(book.window->apply_ops(Json::array({Json{{"op", "add_cover"}, {"kind", "front"}}})));
        app::bookview::BookPreview preview(book.window.get());
        preview.show();
        QVERIFY(preview.caption().startsWith(QStringLiteral("表紙")));
        const auto [left, right] = preview.sides(0);
        QVERIFY(!right && left && *left == 0);  // (a right-bound book: the cover alone, on the left)
        preview.flip(1);  // (the next spread)
        for (int i = 0; i < 20; ++i) preview.step();
        QVERIFY(preview.spread() == 1 && preview.caption().contains(QStringLiteral("1 ページ")));
        preview.view->repaint();
        QVERIFY(!preview.pixmap(0).isNull());
        preview.close();
        QVERIFY(book.act("act_book_preview") != nullptr && book.act("act_book_preview")->shortcut() == QKeySequence(QStringLiteral("Ctrl+Shift+B")));
    }
};

QTEST_MAIN(TestGuiCarried)
#include "test_gui_carried.moc"
