// PC5 G-d (SPEC.md TEXT-02「文字・段落・スタイル・字形のパネル」「スペルチェック」「検索と置換」「書体の置き換え」「環境設定 →
// テキスト」) in the window: ウィンドウ → 文字・段落・文字スタイル・段落スタイル・字形 and 書式 → パネル ▸; the 文字 panel's fields on the
// range chosen in the editor typing over a type layer (set_type with its range) and on the whole; the 段落 panel; the styles'
// panels (a style made from the text, applied, its override marked +); a glyph put at the caret; 検索と置換…'s 次を検索・置換・
// 置換して次を検索 over the lines and the type layers; スペルチェック…'s window (変更, すべてを変更, 辞書に追加, 表記の表); 見つからない
// フォントを解決… and すべての見つからないフォントを置換; フォントプレビューのサイズ ▸; 環境設定 → テキスト (Esc to keep, smart quotes);
// the names Photoshop gives (縦書き文字ツール, 検索と置換…) and Shift+T turning the type tools round.

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QInputMethodEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtTest>

#include "app/ask.hpp"
#include "app/canvas.hpp"
#include "app/config.hpp"
#include "app/ime.hpp"
#include "app/main_window.hpp"
#include "app/preferences.hpp"
#include "app/story_editor.hpp"
#include "app/type_dialogs.hpp"
#include "app/type_panels.hpp"
#include "core/type_styles.hpp"
#include "core/type_text.hpp"
#include "gui_support.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;

namespace {

struct Studio {
    QTemporaryDir tmp;
    QTemporaryDir config;
    QByteArray previous_config;
    std::shared_ptr<app::Session> session;
    std::unique_ptr<app::MainWindow> window;
    std::vector<Json> ops;
    std::shared_ptr<app::ask::Responder> responder = std::make_shared<app::ask::Responder>();
    Studio() {
        (void)gui_test::config_folder();
        previous_config = qgetenv("GENKO_CONFIG_DIR");
        qputenv("GENKO_CONFIG_DIR", config.path().toUtf8());
        app::settings()->setValue(QStringLiteral("units/type"), QStringLiteral("mm"));
        const auto path = gui_test::path_of(tmp.path() + "/book");
        gui_test::write_book(path, gui_test::new_doc(2));
        session = app::Session::open(path, gui_test::quick(gui_test::path_of(tmp.path() + "/recovery")));
        window = std::make_unique<app::MainWindow>(session);
        window->resize(1300, 900);
        window->show();
        window->activateWindow();
        (void)QTest::qWaitForWindowActive(window.get());
        window->canvas()->fit_page();
        QObject::connect(session.get(), &app::Session::changed, window.get(), [this](const app::BookChange& c) {
            if (c.why == app::BookChange::Why::Edit)
                for (const Json& op : c.ops) ops.push_back(op);
        });
        app::ask::set_responder(responder);
    }
    ~Studio() {
        app::ask::set_responder(nullptr);
        window.reset();
        qputenv("GENKO_CONFIG_DIR", previous_config);
    }
    QAction* act(const char* name) const { return window->action(QString::fromLatin1(name)); }
    Json last() const { return ops.empty() ? Json() : ops.back(); }
    const core::Page& page() const { return window->book().page(static_cast<std::size_t>(window->page_index())); }
    const core::StoryLine& record(const std::string& id = "t1") const {
        for (const core::Layer& l : page().layers)
            if (l.id == id) return *l.type;
        throw std::runtime_error("no layer " + id);
    }
    bool apply(const Json& op) { return window->apply_ops(Json::array({op})); }
    // A type layer t1 across (a point text) chosen.
    void typed(const std::string& text, const std::string& id = "t1") {
        apply(Json{{"op", "add_type_layer"}, {"page", page().index.json()}, {"text", text}, {"point", Json::array({20, 30})}, {"wrap", "horizontal"},
                   {"new_id", id}, {"style", Json{{"size_mm", 5}}}});
        window->set_target_layer(id);
    }
    QDockWidget* dock(const QString& name) const { return window->findChild<QDockWidget*>(name); }
};

QMenu* top_menu(const app::MainWindow& w, const QString& title) {
    for (QAction* top : w.menuBar()->actions())
        if (top->menu() != nullptr && top->menu()->title() == title) return top->menu();
    return nullptr;
}

QStringList texts(QMenu* menu) {
    QStringList out;
    for (QAction* a : menu->actions()) out << (a->isSeparator() ? QStringLiteral("|") : a->text());
    return out;
}

}  // namespace

class TestGuiTypePanels : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QVERIFY(gui_test::config_folder().isValid()); }

    // The panels in ウィンドウ and 書式 → パネル ▸ (Photoshop's names), hidden until asked, beside the layers
    void thePanelsAndTheirCommands() {
        Studio s;
        const QStringList window = texts(top_menu(*s.window, QStringLiteral("ウィンドウ")));
        for (const char* name : {"文字", "段落", "文字スタイル", "段落スタイル", "字形"}) QVERIFY2(window.contains(QString::fromUtf8(name)), name);
        QMenu* panels = s.window->findChild<QMenu*>(QStringLiteral("menu_type_panels"));
        QVERIFY(panels != nullptr);
        QCOMPARE(texts(panels), (QStringList{QStringLiteral("文字パネル"), QStringLiteral("段落パネル"), QStringLiteral("字形パネル"),
                                             QStringLiteral("文字スタイルパネル"), QStringLiteral("段落スタイルパネル")}));
        for (const char* d : {"type_character", "type_paragraph", "type_character_styles", "type_paragraph_styles", "type_glyphs"}) {
            QDockWidget* dock = s.dock(QString::fromLatin1(d));
            QVERIFY2(dock != nullptr && !dock->isVisible(), d);
        }
        s.act("act_type_panel_glyphs")->trigger();
        QTRY_VERIFY(s.dock(QStringLiteral("type_glyphs"))->isVisible());
        QVERIFY(s.act("act_panel_glyphs")->isChecked());
        s.act("act_panel_glyphs")->trigger();
        QTRY_VERIFY(!s.dock(QStringLiteral("type_glyphs"))->isVisible());
        // the renamed: 縦書き文字ツール (T), 編集 → 検索と置換…; Shift+T turns the type tools round
        QCOMPARE(s.act("act_text")->text(), QStringLiteral("縦書き文字ツール"));
        QCOMPARE(s.act("act_replace")->text(), QStringLiteral("検索と置換…"));
        QVERIFY(app::preferences::renamed().count(QStringLiteral("縦書き文字ツール")) == 1);
        s.act("act_text_h")->trigger();
        QCOMPARE(s.window->canvas()->tool(), QStringLiteral("text_h"));
        s.act("act_text_h")->trigger();
        QCOMPARE(s.window->canvas()->tool(), QStringLiteral("text"));
        // フォントプレビューのサイズ ▸: one checked, kept in 環境設定 → テキスト
        QVERIFY(s.act("act_type_preview_medium")->isChecked());
        s.act("act_type_preview_huge")->trigger();
        QCOMPARE(app::settings()->value(QStringLiteral("type/font_preview")).toString(), QStringLiteral("huge"));
        QVERIFY(s.act("act_type_preview_huge")->isChecked() && !s.act("act_type_preview_medium")->isChecked());
    }

    // 文字 panel: the range chosen in the editor typing over the type layer — set_type over it —, else the whole
    void characterPanelOnARange() {
        Studio s;
        s.typed("abcdef");
        s.act("act_panel_character")->trigger();
        app::CharacterPanel& chars = *s.window->character_panel();
        QTRY_VERIFY(chars.isEnabled());
        QCOMPARE(chars.title->text(), QStringLiteral("文字全体"));
        QCOMPARE(chars.size->value(), 5.0);
        // the whole: set_type
        chars.tracking->setValue(100);
        QCOMPARE(s.last(), (Json{{"op", "set_type"}, {"page", 1}, {"layer_id", "t1"}, {"tracking", 0.1}}));
        // typing over it: the letters chosen — "cd" — are the range
        s.window->edit_type_layer_inline("t1");
        app::InlineEditor* editor = s.window->canvas()->editor();
        QVERIFY(editor != nullptr);
        QTextCursor cursor = editor->textCursor();
        cursor.setPosition(2);
        cursor.setPosition(4, QTextCursor::KeepAnchor);
        editor->setTextCursor(cursor);
        QCOMPARE(s.window->type_range(), std::make_optional(std::make_pair(std::int64_t{2}, std::int64_t{4})));
        chars.refresh();
        QCOMPARE(chars.title->text(), QStringLiteral("選んだ文字（3〜4 字目）"));
        chars.underline->click();  // (the editor keeps what is typed: the same text — the range still its)
        QCOMPARE(s.last(), (Json{{"op", "set_type"}, {"page", 1}, {"layer_id", "t1"}, {"range", Json::array({2, 4})}, {"underline", true}}));
        const Json& runs = (*s.record().native_type)["runs"];
        QCOMPARE(runs, (Json::array({Json{{"start", 2}, {"end", 4}, {"char", Json{{"underline", true}}}}})));
        QTRY_VERIFY(chars.underline->isChecked());
        // the font, bundled: its names taken away
        chars.family->setCurrentIndex(chars.family->findData(QStringLiteral("mincho")));
        emit chars.family->activated(chars.family->currentIndex());
        QCOMPARE(s.last()["font"], Json("mincho"));
        QVERIFY(s.last()["family"].is_null());
        // 上付き: a smaller size moved up
        chars.superscript->click();
        QCOMPARE(s.last()["scale"], Json(0.58));
        QVERIFY(s.last()["baseline_shift_mm"].get<double>() > 1.5);
        // アンチエイリアス: the whole layer's
        chars.antialias->setCurrentIndex(chars.antialias->findData(QStringLiteral("none")));
        emit chars.antialias->activated(chars.antialias->currentIndex());
        QCOMPARE(s.last(), (Json{{"op", "edit_type"}, {"page", 1}, {"layer_id", "t1"}, {"antialias", "none"}}));
    }

    // 段落 panel: alignment, indents, kinsoku, on the whole (a type layer: set_type)
    void paragraphPanel() {
        Studio s;
        s.typed("一行目\n二行目");
        s.act("act_panel_paragraph")->trigger();
        app::ParagraphPanel& paragraph = *s.window->paragraph_panel();
        QTRY_VERIFY(paragraph.isEnabled());
        QToolButton* center = nullptr;
        for (QToolButton* b : paragraph.aligns)
            if (b->objectName() == QStringLiteral("type_align_center")) center = b;
        QVERIFY(center != nullptr);
        QVERIFY(paragraph.aligns.front()->isChecked());  // (a point text: left)
        center->click();
        QCOMPARE(s.last(), (Json{{"op", "set_type"}, {"page", 1}, {"layer_id", "t1"}, {"align", "center"}}));
        QTRY_VERIFY(center->isChecked());
        paragraph.first_indent->setValue(3);
        QCOMPARE(s.last()["first_indent_mm"], Json(3));
        paragraph.kinsoku->setCurrentIndex(paragraph.kinsoku->findData(QStringLiteral("strong")));
        emit paragraph.kinsoku->activated(paragraph.kinsoku->currentIndex());
        QCOMPARE(s.last()["kinsoku"], Json("strong"));
        QVERIFY(!paragraph.direction->isVisible());  // (東アジア言語の機能: no direction)
    }

    // 文字スタイル panel: a style made from the text and applied; the text's own attribute over it marked +; cleared
    void stylePanels() {
        Studio s;
        s.typed("強調する");
        s.act("act_panel_character_styles")->trigger();
        app::TypeStylesPanel& styles = *s.window->character_styles_panel();
        QTRY_COMPARE(styles.list->count(), 1);
        QCOMPARE(styles.list->item(0)->text(), QStringLiteral("▶ [なし]"));
        styles.new_style();
        QTRY_COMPARE(styles.list->count(), 2);
        QCOMPARE(s.window->book().type_styles->at("character").size(), std::size_t{1});
        styles.apply_row(styles.list->item(1), false);
        QCOMPARE(s.last()["action"], Json("apply"));
        QTRY_VERIFY(styles.list->item(1)->text().startsWith(QStringLiteral("▶ 文字スタイル 1")));
        s.apply(Json{{"op", "set_type"}, {"page", 1}, {"layer_id", "t1"}, {"rgb", Json::array({200, 0, 0})}});
        styles.refresh();
        QVERIFY2(styles.list->item(1)->text().endsWith(QStringLiteral("+")), qPrintable(styles.list->item(1)->text()));
        styles.clear_override();
        QTRY_VERIFY(!styles.list->item(1)->text().endsWith(QStringLiteral("+")));
        // the paragraph styles: [基本段落] first
        s.act("act_panel_paragraph_styles")->trigger();
        QTRY_VERIFY(s.window->paragraph_styles_panel()->list->count() >= 1);
        QCOMPARE(s.window->paragraph_styles_panel()->list->item(0)->data(Qt::UserRole).toString(), QStringLiteral("basic"));
    }

    // 字形 panel: the font's characters by category; a double click puts one at the caret (none typed: where the caret was)
    void glyphsPanel() {
        Studio s;
        s.typed("ab");
        s.act("act_panel_glyphs")->trigger();
        app::GlyphsPanel& glyphs = *s.window->glyphs_panel();
        QTRY_VERIFY(glyphs.glyphs->count() > 50);
        glyphs.category->setCurrentIndex(glyphs.category->findData(QStringLiteral("hiragana")));
        emit glyphs.category->activated(glyphs.category->currentIndex());
        QVERIFY(glyphs.glyphs->count() > 50);
        QCOMPARE(glyphs.glyphs->item(0)->data(Qt::UserRole).toString().toUcs4().front() >= 0x3040U, true);
        // typed: at the caret
        s.window->edit_type_layer_inline("t1");
        app::InlineEditor* editor = s.window->canvas()->editor();
        QVERIFY(editor != nullptr);
        QTextCursor cursor = editor->textCursor();
        cursor.setPosition(1);
        editor->setTextCursor(cursor);
        glyphs.insert(QStringLiteral("→"));
        QCOMPARE(editor->toPlainText(), QStringLiteral("a→b"));
        editor->finish(true);
        QTRY_COMPARE(s.record().text, std::string("a→b"));
        // none typed: where the caret was, by edit_type
        glyphs.insert(QStringLiteral("★"));
        QCOMPARE(s.last()["op"], Json("edit_type"));
        QCOMPARE(s.record().text, std::string("a→★b"));
        QCOMPARE(glyphs.recent->item(0)->text(), QStringLiteral("★"));
    }

    // 検索と置換…: 次を検索 over the lines then the type layers, 置換, 置換して次を検索, 単語単位 and すべてを置換
    void findAndReplace() {
        Studio s;
        s.apply(Json{{"op", "add_line"}, {"page", 1}, {"id", "L1"}, {"text", "cat and category"}, {"x_mm", 30}, {"y_mm", 30}, {"w_mm", 40}, {"h_mm", 20}});
        s.typed("a cat");
        QStringList seen;
        Json replaced;
        std::optional<std::string> chosen_line;
        std::string seen_layer;
        s.responder->exec = [&](QDialog* dialog) {
            auto* d = qobject_cast<app::ReplaceDialog*>(dialog);
            if (d == nullptr) return 0;
            seen << d->windowTitle();
            d->find->setText(QStringLiteral("cat"));
            d->replace->setText(QStringLiteral("dog"));
            d->whole_word->setChecked(true);
            seen << (d->find_next() ? QStringLiteral("found") : QStringLiteral("none"));  // (the line's first)
            chosen_line = s.window->canvas()->selected_line_id;
            seen << (d->replace_one() ? QStringLiteral("replaced") : QStringLiteral("not"));
            replaced = s.last();
            d->replace_and_find();  // (nothing more in the line — "category" is not a whole "cat" —: the type layer's, found)
            seen << d->found->text();
            seen_layer = s.window->target_layer() != nullptr ? s.window->target_layer()->id : std::string();  // (the type layer's "cat")
            return 0;
        };
        s.act("act_replace")->trigger();
        QCOMPARE(seen, (QStringList{QStringLiteral("検索と置換"), QStringLiteral("found"), QStringLiteral("replaced"), QStringLiteral("「cat」が見つかりました")}));
        QCOMPARE(chosen_line, std::optional<std::string>("L1"));
        QCOMPARE(seen_layer, std::string("t1"));
        QCOMPARE(replaced, (Json{{"op", "edit_type"}, {"replace", Json{{"start", 0}, {"end", 3}, {"text", "dog"}}}, {"line", "L1"}}));
        QCOMPARE(s.window->book().story[0].text, std::string("dog and category"));
        // すべてを置換: replace_text with whole_word, the type layer too
        s.responder->exec = [&](QDialog* dialog) {
            auto* d = qobject_cast<app::ReplaceDialog*>(dialog);
            if (d == nullptr) return 0;
            d->find->setText(QStringLiteral("cat"));
            d->replace->setText(QStringLiteral("cow"));
            d->whole_word->setChecked(true);
            d->replace_all();
            return 0;
        };
        s.act("act_replace")->trigger();
        QCOMPARE(s.last()["op"], Json("replace_text"));
        QCOMPARE(s.last()["whole_word"], Json(true));
        QCOMPARE(s.record().text, std::string("a cow"));
        QCOMPARE(s.window->book().story[0].text, std::string("dog and category"));
    }

    // スペルチェック…: a misspelt word and its suggestion changed (edit_type at its place), all of a word, the dictionary,
    // the notation table
    void spellingWindow() {
        Studio s;
        s.apply(Json{{"op", "add_line"}, {"page", 1}, {"id", "L1"}, {"text", "Teh cat, teh dog, 出来る"}, {"x_mm", 30}, {"y_mm", 30}, {"w_mm", 40}, {"h_mm", 20},
                     {"wrap", "horizontal"}});
        s.typed("Genkoo recieve");
        app::settings()->setValue(QStringLiteral("type/notations"), QStringLiteral(R"([{"from": "出来る", "to": "できる"}])"));
        QStringList seen;
        Json first_change;
        s.responder->exec = [&](QDialog* dialog) {
            auto* d = qobject_cast<app::SpellingDialog*>(dialog);
            if (d == nullptr) return 0;
            seen << d->word->text() << d->change_to->text();  // Teh → The
            d->change();
            first_change = s.last();
            seen << d->word->text();  // teh
            d->change_to->setText(QStringLiteral("the"));
            d->change_all();
            seen << d->word->text() << d->change_to->text();  // (the notation table's) 出来る → できる
            d->change();
            seen << d->word->text();  // Genkoo
            d->add_to_dictionary();
            seen << d->word->text();  // recieve
            d->ignore();
            seen << (d->current() == nullptr ? QStringLiteral("done") : d->word->text());
            return 0;
        };
        s.act("act_spelling")->trigger();
        QCOMPARE(seen, (QStringList{QStringLiteral("Teh"), QStringLiteral("The"), QStringLiteral("teh"), QStringLiteral("出来る"), QStringLiteral("できる"),
                                    QStringLiteral("Genkoo"), QStringLiteral("recieve"), QStringLiteral("done")}));
        QCOMPARE(first_change, (Json{{"op", "edit_type"}, {"replace", Json{{"start", 0}, {"end", 3}, {"text", "The"}}}, {"line", "L1"}}));
        QCOMPARE(s.window->book().story[0].text, std::string("The cat, the dog, できる"));
        QVERIFY(app::settings()->value(QStringLiteral("type/user_dictionary")).toString().split(QLatin1Char('\t')).contains(QStringLiteral("Genkoo")));
        QCOMPARE(s.record().text, std::string("Genkoo recieve"));
    }

    // 見つからないフォントを解決…: the missing fonts, each its font (a bundled one), replaced; すべての見つからないフォントを置換
    void missingFonts() {
        Studio s;
        s.apply(Json{{"op", "add_line"}, {"page", 1}, {"id", "L1"}, {"text", "書体"}, {"x_mm", 30}, {"y_mm", 30}, {"style", Json{{"font", "/gone/A.ttf"}}}});
        s.apply(Json{{"op", "add_line"}, {"page", 1}, {"id", "L2"}, {"text", "書体"}, {"x_mm", 30}, {"y_mm", 60}, {"style", Json{{"font", "/gone/B.ttf"}}}});
        s.responder->exec = [&](QDialog* dialog) {
            auto* d = qobject_cast<app::MissingFontsDialog*>(dialog);
            if (d == nullptr) return 0;
            if (d->table->rowCount() != 2) return 0;
            d->choices[0]->setCurrentIndex(d->choices[0]->findData(QStringLiteral("key:gothic")));
            d->choices[1]->setCurrentIndex(0);  // (いつもの書体)
            return static_cast<int>(QDialog::Accepted);
        };
        s.act("act_type_resolve_missing")->trigger();
        QCOMPARE(s.last(), (Json{{"op", "replace_fonts"}, {"map", Json{{"/gone/A.ttf", "gothic"}, {"/gone/B.ttf", nullptr}}}}));
        QCOMPARE(s.window->book().story[0].style["font"], Json("gothic"));
        QVERIFY(!s.window->book().story[1].style.contains("font"));
        s.apply(Json{{"op", "edit_line"}, {"id", "L1"}, {"style", Json{{"font", "/gone/C.ttf"}}}});
        s.act("act_type_replace_missing")->trigger();
        QCOMPARE(s.last(), (Json{{"op", "replace_fonts"}, {"all", true}}));
        QVERIFY(!s.window->book().story[0].style.contains("font"));
        s.act("act_type_replace_missing")->trigger();
        QCOMPARE(s.window->last_notice(), QStringLiteral("見つからないフォントはありません"));
    }

    // 環境設定 → テキスト: its page (its pane in 編集 → 環境設定 ▸); Esc keeps what was typed, smart quotes
    void textPreferences() {
        Studio s;
        QVERIFY(s.act("act_prefs_type") != nullptr);
        app::preferences::PreferencesDialog dialog(s.window.get());
        dialog.show_pane(QStringLiteral("type"));
        QWidget* page = dialog.tabs->currentWidget();
        QVERIFY(page != nullptr);
        auto* esc = page->findChild<QCheckBox*>(QStringLiteral("prefs_type_esc_commits"));
        auto* quotes = page->findChild<QCheckBox*>(QStringLiteral("prefs_type_smart_quotes"));
        QVERIFY(esc != nullptr && quotes != nullptr && !esc->isChecked());
        esc->setChecked(true);
        quotes->setChecked(true);
        dialog.save();
        QCOMPARE(app::settings()->value(QStringLiteral("type/esc_commits")).toString(), QStringLiteral("true"));
        s.typed("ab");
        s.window->edit_type_layer_inline("t1");
        app::InlineEditor* editor = s.window->canvas()->editor();
        QVERIFY(editor != nullptr && editor->esc_commits && editor->smart_quotes);
        editor->moveCursor(QTextCursor::End);
        QTest::keyClick(editor, Qt::Key_Space);
        QTest::keyClicks(editor, QStringLiteral("\"x\""));
        QCOMPARE(editor->toPlainText(), QStringLiteral("ab “x”"));
        QTest::keyClick(editor, Qt::Key_Escape);
        QTRY_COMPARE(s.record().text, std::string("ab “x”"));
    }
};

QTEST_MAIN(TestGuiTypePanels)
#include "test_gui_type_panels.moc"
