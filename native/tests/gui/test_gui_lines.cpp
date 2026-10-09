// The lines of dialogue in the window (M4①c, Python's genko/app/main.py StoryPanel and its line handlers, canvas.py's
// text tool and balloon handles, tool_settings.py TextToolSettings, story_editor.py, bookview.ReplaceDialog,
// text_style.py and lettering.py): the commands as Python's window has them; a line typed where the text tool clicks
// (keys, the input method's words converted then settled, Enter for the next column, Ctrl+Enter, Esc, a click
// elsewhere), with the text tool's settings; a balloon drawn by hand; a balloon chosen, moved, resized, turned, its
// tail's tip and bend dragged, a corner added; typed over in place, deleted, turned vertical or across, its shape; the
// 台詞 panel's list, order, words and lettering; the story editor and 台詞の検索・置換; a balloon's own menu. Every
// change is an op, recorded as the window applies it, and compared with the op Python's window makes (the numbers from
// the Python reference: genko.app.lettering on the same page); and after the ops, the page on the canvas is drawn as
// render_page draws the book.

#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFile>
#include <QInputMethodEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QPointer>
#include <QScrollArea>
#include <QSpinBox>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QToolBar>
#include <QtTest>

#include <cmath>
#include <fstream>
#include <functional>

#include "app/canvas.hpp"
#include "app/ime.hpp"
#include "app/inject.hpp"
#include "app/lettering.hpp"
#include "app/main_window.hpp"
#include "app/story_editor.hpp"
#include "app/story_panel.hpp"
#include "app/text_style.hpp"
#include "app/tiles.hpp"
#include "app/tool_settings.hpp"
#include "app/wording.hpp"
#include "core/base64.hpp"
#include "core/pynum.hpp"
#include "gui_support.hpp"
#include "render/page.hpp"
#include "render/png.hpp"

using namespace genko;
using core::Json;
namespace inject = genko::app::inject;

namespace {

double r2(double v) { return core::py_round(v, 2); }

struct Studio {
    QTemporaryDir tmp;
    QTemporaryDir config;  // (the computer's fonts listed for this test alone)
    QByteArray previous_config;
    std::shared_ptr<app::Session> session;
    std::unique_ptr<app::MainWindow> window;
    gui_test::Answers answers;
    std::vector<Json> ops;  // every op applied, in order
    // (edit: the book as it is on disk before it opens, e.g. a style written by hand)
    explicit Studio(const std::function<void(core::Document&)>& edit = {}) {
        (void)gui_test::config_folder();
        previous_config = qgetenv("GENKO_CONFIG_DIR");
        qputenv("GENKO_CONFIG_DIR", config.path().toUtf8());
        const auto path = gui_test::path_of(tmp.path() + "/book");
        core::Document doc = gui_test::new_doc(2);  // (A4, two pages: Python's new_episode("試し", 1, 2, PageSpec.a4_mono()))
        if (edit) edit(doc);
        gui_test::write_book(path, doc);
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
    app::StoryPanel& panel() const { return *window->story(); }
    const core::Page& page() const { return window->book().page(static_cast<std::size_t>(window->page_index())); }
    std::string frame_id() const { return page().leaf_frames().front()->id; }
    void trigger(const char* name) const { window->action(QString::fromLatin1(name))->trigger(); }
    Json last() const { return ops.empty() ? Json() : ops.back(); }
    // ops applied since `from`
    Json since(std::size_t from) const {
        Json out = Json::array();
        for (std::size_t i = from; i < ops.size(); ++i) out.push_back(ops[i]);
        return out;
    }
    const core::StoryLine* line(const std::string& id) const { return window->line_by_id(id); }
    // a line put by the test itself (as an agent or the CLI would): its id
    std::string put(Json op) {
        if (!op.contains("page")) op["page"] = page().index.json();
        if (!op.contains("frame_id")) op["frame_id"] = frame_id();
        const std::size_t before = window->book().story.size();
        if (!window->apply_ops(Json::array({op}))) return {};
        return window->book().story.size() > before ? window->book().story.back().id : std::string();
    }
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

QAction* find_action(QMenu* menu, const QString& text) {
    for (QAction* a : menu->actions()) {
        if (a->text() == text) return a;
        if (a->menu() != nullptr) {
            if (QAction* inner = find_action(a->menu(), text)) return inner;
        }
    }
    return nullptr;
}

void ime_preedit(QWidget* widget, const QString& words) {
    QInputMethodEvent event(words, {});
    QCoreApplication::sendEvent(widget, &event);
}

void ime_commit(QWidget* widget, const QString& words) {
    QInputMethodEvent event;
    event.setCommitString(words);
    QCoreApplication::sendEvent(widget, &event);
}

void send_mouse(app::PageCanvas* canvas, QEvent::Type type, const QPointF& mm, Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers = {}) {
    const QPointF at = canvas->to_widget(mm);
    QMouseEvent event(type, at, canvas->mapToGlobal(at), Qt::LeftButton, buttons, modifiers);
    QApplication::sendEvent(canvas, &event);
}

// Ctrl+Enter typed in the editor, as the window system sends it: Ctrl goes down (the editor does not take it: it
// reaches the canvas, as in Python, which holds the select tool), Enter keeps the line, and the keys come up where the
// focus is then (the canvas, once the editor is gone).
void ctrl_enter(QWidget* editor) {
    QTest::keyPress(editor, Qt::Key_Control);
    QTest::keyPress(editor, Qt::Key_Return, Qt::ControlModifier);
    QWidget* focus = QApplication::focusWidget() != nullptr ? QApplication::focusWidget() : editor;
    QTest::keyRelease(focus, Qt::Key_Return, Qt::ControlModifier);
    QTest::keyRelease(focus, Qt::Key_Control);
}

void double_click_mm(app::PageCanvas* canvas, const QPointF& mm) {
    send_mouse(canvas, QEvent::MouseButtonPress, mm, Qt::LeftButton);
    send_mouse(canvas, QEvent::MouseButtonRelease, mm, Qt::NoButton);
    send_mouse(canvas, QEvent::MouseButtonDblClick, mm, Qt::LeftButton);
    send_mouse(canvas, QEvent::MouseButtonRelease, mm, Qt::NoButton);
}

QImage rendered(const core::Document& book, std::size_t index, int dpi) {
    render::RenderOptions options;
    options.mode = "proof";
    options.skip_unported = true;
    const auto image = render::render_page(book.page(index), dpi, options, &book).image;
    const auto bytes = image.tobytes();
    return QImage(reinterpret_cast<const uchar*>(bytes.data()), image.width(), image.height(), image.width() * 3, QImage::Format_RGB888)
        .convertToFormat(QImage::Format_RGB32);
}

// The page on the canvas, drawn as render_page draws the book now (its lines and balloons included).
QImage shown(Studio& s) {
    if (!s.canvas()->wait_rendered(60000)) return {};
    return s.canvas()->renderer().compose(s.canvas()->base_dpi());
}

bool drawn_as_book(Studio& s) {
    const QImage now = shown(s);
    return !now.isNull() && now == rendered(s.window->book(), static_cast<std::size_t>(s.window->page_index()), s.canvas()->base_dpi());
}

// Dark pixels (the letters, the balloon's line) inside this box of the page (mm), at the canvas's resolution.
int ink_in(const QImage& image, const QRectF& mm, int dpi) {
    const double px = dpi / 25.4;
    const QRect box = QRectF(mm.x() * px, mm.y() * px, mm.width() * px, mm.height() * px).toAlignedRect().intersected(image.rect());
    int n = 0;
    for (int y = box.top(); y <= box.bottom(); ++y)
        for (int x = box.left(); x <= box.right(); ++x)
            if (qGray(image.pixel(x, y)) < 128) ++n;
    return n;
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

class TestGuiLines : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QVERIFY(gui_test::config_folder().isValid()); }

    void commandsAsPythonsWindowHasThem() {
        Studio s;
        const std::vector<Expected> expected{
            {"act_text", "テキスト", "T", "クリックした所に台詞を入力します（縦書き）", true, "ツール"},
            {"act_line_type", "台詞を入れる（テキストの道具）", "", "", false, "ページ/台詞"},
            {"act_balloon_pen", "フキダシを手で描く", "", "ドラッグで囲んだ形のフキダシに台詞を入れます", true, "ページ/台詞"},
            {"act_line_edit", "選んだ台詞をその場で直す", "F2", "", false, "ページ/台詞"},
            {"act_line_delete", "選んだ台詞を消す", "", "", false, "ページ/台詞"},
            {"act_line_wrap", "縦書き・横書きを切り替える", "", "", false, "ページ/台詞"},
            {"act_story_editor", "ストーリーエディター…", "Ctrl+Shift+L", "全ページの台詞をまとめて直す・台本を流し込む", false, "ページ"},
            {"act_replace", "台詞の検索・置換…", "Ctrl+Alt+F", "全ページの台詞から言葉を探して置き換えます", false, "ページ"},
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
        // the text tool: one of the tools, its picture and its key in its tooltip, on the palette before the panel tool
        QAction* text = s.window->action(QStringLiteral("act_text"));
        QCOMPARE(s.window->tool_actions().at(QStringLiteral("text")), text);
        QVERIFY(s.window->tools_group()->actions().contains(text));
        QVERIFY(!text->icon().isNull());
        QCOMPARE(text->toolTip(), QStringLiteral("テキスト（T）\nクリックした所に台詞を入力します（縦書き）"));
        QToolBar* palette = s.window->findChild<QToolBar*>(QStringLiteral("tools"));
        QVERIFY(palette != nullptr);
        const auto tools = palette->actions();
        QVERIFY(tools.indexOf(text) >= 0 && tools.indexOf(text) + 1 == tools.indexOf(s.window->action(QStringLiteral("act_frame"))));
        // ツール: … 図形, テキスト, コマ割り; ページ → 台詞 (with フキダシの形: Python's kinds), then the book's lines
        QMenu* tool_menu = s.menu_at(QStringLiteral("ツール"));
        const auto in_tools = tool_menu->actions();
        QCOMPARE(in_tools.indexOf(text), in_tools.indexOf(s.window->action(QStringLiteral("act_shape"))) + 1);
        QMenu* lines = s.menu_at(QStringLiteral("ページ/台詞"));
        QVERIFY(lines != nullptr);
        QStringList order;
        for (QAction* a : lines->actions()) order << (a->isSeparator() ? QStringLiteral("|") : a->text());
        QCOMPARE(order, QStringList({QStringLiteral("台詞を入れる（テキストの道具）"), QStringLiteral("フキダシを手で描く"), QStringLiteral("|"),
                                     QStringLiteral("選んだ台詞をその場で直す"), QStringLiteral("縦書き・横書きを切り替える"), QStringLiteral("フキダシの形"),
                                     QStringLiteral("選んだ台詞を消す")}));
        QMenu* shapes = s.menu_at(QStringLiteral("ページ/台詞/フキダシの形"));
        QVERIFY(shapes != nullptr);
        QCOMPARE(shapes->actions().size(), 15);
        QCOMPARE(shapes->actions().front()->text(), QStringLiteral("普通（楕円）"));
        QCOMPARE(shapes->actions().back()->text(), QStringLiteral("飾り枠（テーマ・題目）"));
        // the 台詞 panel: a tab of the right row (not closed: as Python's), scrolling on a small screen
        auto* dock = s.window->findChild<QDockWidget*>(QStringLiteral("台詞"));
        QVERIFY(dock != nullptr);
        QVERIFY(qobject_cast<QScrollArea*>(dock->widget()) != nullptr);
        QVERIFY(!dock->features().testFlag(QDockWidget::DockWidgetClosable));
        QVERIFY(s.window->tabifiedDockWidgets(dock).contains(s.window->findChild<QDockWidget*>(QStringLiteral("ページ"))));
        QVERIFY(s.panel().empty_note->isVisible() || !dock->isVisible());
        QCOMPARE(s.panel().empty_note->text(), QStringLiteral("このページにはまだ台詞がありません。\nテキストの道具（T）で、置きたい所をクリックします"));
        // ツールの設定: the text tool's page (TextToolSettings), and the select tool's with the chosen line's lettering
        auto* settings = s.window->text_settings();
        QCOMPARE(s.window->tool_settings()->page_for(QStringLiteral("text")), static_cast<QWidget*>(settings));
        QCOMPARE(settings->balloon->count(), 15);
        QCOMPARE(settings->balloon->currentData().toString(), QStringLiteral("speech"));
        QVERIFY(settings->vertical->isChecked());
        QCOMPARE(settings->draw_balloon->text(), QStringLiteral("フキダシを手で描く"));
        QCOMPARE(settings->font->itemText(0), QStringLiteral("いつもの書体（アンチック）"));
        QCOMPARE(settings->font->count(), 7);
        QCOMPARE(settings->size->specialValueText(), QStringLiteral("自動（フキダシに合わせる）"));
        QWidget* select_page = s.window->tool_settings()->page_for(QStringLiteral("select"));
        QVERIFY(select_page != nullptr);
        QCOMPARE(s.panel().style_box->parentWidget(), select_page);
        QVERIFY(select_page->findChildren<QPushButton*>().size() >= 3);
        bool editor_row = false;
        for (QPushButton* b : select_page->findChildren<QPushButton*>()) editor_row = editor_row || b->text().startsWith(QStringLiteral("ストーリーエディター"));
        QVERIFY(editor_row);
        QCOMPARE(s.panel().style_title->text(), QStringLiteral("台詞をクリックすると設定が出ます"));
        QVERIFY(!s.panel().style_body->isVisibleTo(s.panel().style_box));
        // the tool chosen: the canvas's tool, its settings shown, the I-beam
        s.trigger("act_text");
        QCOMPARE(s.canvas()->tool(), QStringLiteral("text"));
        QCOMPARE(s.window->tool_settings()->current_page(), static_cast<QWidget*>(settings));
        QCOMPARE(s.canvas()->cursor().shape(), Qt::IBeamCursor);
        s.trigger("act_select");
        s.trigger("act_line_type");
        QCOMPARE(s.canvas()->tool(), QStringLiteral("text"));
        QVERIFY(text->isChecked());
    }

    void aLineIsTypedWhereTheTextToolClicks() {
        Studio s;
        const std::string frame = s.frame_id();
        const QImage before = shown(s);
        s.trigger("act_text");
        // a click: the editor opens there, with the focus
        inject::click_mm(s.canvas(), QPointF(100.0, 150.0));
        app::InlineEditor* editor = s.canvas()->editor();
        QVERIFY(editor != nullptr);
        QVERIFY(editor->isVisible());
        QCOMPARE(editor->size(), QSize(260, 110));
        const QPointF at = s.canvas()->to_widget(QPointF(100.0, 150.0));
        QCOMPARE(editor->pos(), QPoint(static_cast<int>(std::max(0.0, std::min(s.canvas()->width() - 260.0, at.x()))),
                                       static_cast<int>(std::max(20.0, std::min(s.canvas()->height() - 110.0, at.y())))));
        QCOMPARE(editor->hint()->text(), QStringLiteral("Ctrl+Enter で決定・Esc でやめる・文字を選んで右クリックで大きく・太く・色"));
        QCOMPARE(QApplication::focusWidget(), static_cast<QWidget*>(editor));
        QVERIFY(s.ops.empty());
        // the input method's words: converted in place, then settled; Enter starts the next column
        ime_preedit(editor, QStringLiteral("おはよう"));
        QCOMPARE(editor->preedit(), QStringLiteral("おはよう"));
        ctrl_enter(editor);  // (while converting, Ctrl+Enter does not end the line)
        QCOMPARE(s.canvas()->editor(), editor);
        QVERIFY(s.ops.empty());
        ime_commit(editor, QStringLiteral("おはよう"));
        QVERIFY(editor->preedit().isEmpty());
        QTest::keyClick(editor, Qt::Key_Return);
        ime_commit(editor, QStringLiteral("漢字《かんじ》の{大|日}"));
        QCOMPARE(editor->toPlainText(), QStringLiteral("おはよう\n漢字《かんじ》の{大|日}"));
        ctrl_enter(editor);
        QVERIFY(s.canvas()->editor() == nullptr);
        // Python's add_line: place_at(100.0, 150.0, …, "speech", vertical, the panel) and parse_marks
        QCOMPARE(s.ops.size(), std::size_t{1});
        const Json want = Json::parse(R"({"op": "add_line", "page": 1, "text": "おはよう\n漢字の日", "balloon": "speech", "x_mm": 90.66,
            "y_mm": 135.47, "w_mm": 18.67, "h_mm": 29.06, "wrap": "vertical", "frame_id": "", "ruby_runs": [["漢字", "かんじ"]],
            "style_runs": [["日", {"scale": 1.4}]]})");
        Json got = s.last();
        QCOMPARE(got["frame_id"], Json(frame));
        got["frame_id"] = "";
        QCOMPARE(got.dump(), want.dump());
        // chosen, the select tool, the panel listing it; the page drawn with it
        const std::string id = s.window->book().story.back().id;
        QCOMPARE(s.canvas()->selected_line_id, std::optional<std::string>(id));
        QCOMPARE(s.canvas()->tool(), QStringLiteral("select"));
        QCOMPARE(s.panel().current_id(), std::optional<std::string>(id));
        QCOMPARE(s.panel().list->item(0)->text(), QStringLiteral("1.〔普通〕おはよう 漢字の日"));
        QCOMPARE(s.panel().style_title->text(), QStringLiteral("選んだ台詞の文字とフキダシ: 「おはよう\n漢字の日」"));
        QVERIFY(drawn_as_book(s));
        const QImage after = shown(s);
        QVERIFY(after != before);
        QVERIFY(ink_in(after, QRectF(90.66, 135.47, 18.67, 29.06), s.canvas()->base_dpi()) > ink_in(before, QRectF(90.66, 135.47, 18.67, 29.06), s.canvas()->base_dpi()));

        // Esc drops the words
        s.trigger("act_text");
        inject::click_mm(s.canvas(), QPointF(60.0, 60.0));
        editor = s.canvas()->editor();
        QVERIFY(editor != nullptr);
        ime_commit(editor, QStringLiteral("やめる"));
        QTest::keyClick(editor, Qt::Key_Escape);
        QVERIFY(s.canvas()->editor() == nullptr);
        QCOMPARE(s.ops.size(), std::size_t{1});
        // the text tool's settings: the kind, across, a face and a size; a click near the panel's corner keeps the
        // balloon inside it (Python's int-or-float of the clamp: the panel's corner is a float there)
        auto* settings = s.window->text_settings();
        settings->balloon->setCurrentIndex(settings->balloon->findData(QStringLiteral("shout")));
        settings->vertical->setChecked(false);
        settings->font->setCurrentIndex(settings->font->findData(QStringLiteral("maru")));
        settings->size->setValue(6.5);
        s.trigger("act_text");
        inject::click_mm(s.canvas(), QPointF(14.0, 14.0));
        editor = s.canvas()->editor();
        QVERIFY(editor != nullptr);
        QTest::keyClicks(editor, QStringLiteral("  "));
        ime_commit(editor, QStringLiteral("やあ"));
        // a click elsewhere keeps it (the editor loses the focus)
        inject::click_mm(s.canvas(), QPointF(150.0, 250.0));
        QVERIFY(s.canvas()->editor() == nullptr);
        const Json want2 = Json::parse(R"({"op": "add_line", "page": 1, "text": "やあ", "balloon": "shout", "x_mm": 14.0, "y_mm": 14.0,
            "w_mm": 22.47, "h_mm": 12.83, "wrap": "horizontal", "style": {"font": "maru", "size_mm": 6.5}, "frame_id": ""})");
        got = s.ops.at(1);
        QCOMPARE(got["frame_id"], Json(frame));
        got["frame_id"] = "";
        QCOMPARE(got.dump(), want2.dump());
        QVERIFY(got["x_mm"].is_number_float() && got["y_mm"].is_number_float());
        QVERIFY(drawn_as_book(s));
        // nothing typed: nothing added
        s.trigger("act_text");
        const std::size_t count = s.ops.size();
        inject::click_mm(s.canvas(), QPointF(120.0, 200.0));
        QVERIFY(s.canvas()->editor() != nullptr);
        ctrl_enter(s.canvas()->editor());
        QCOMPARE(s.ops.size(), count);
    }

    void balloonsAreChosenMovedResizedTurnedAndTheirTailsDragged() {
        Studio s;
        const std::string id = s.put(Json{{"op", "add_line"}, {"text", "テスト"}, {"x_mm", 40}, {"y_mm", 50}, {"w_mm", 30}, {"h_mm", 40},
                                          {"tails", Json::array({Json{{"to", Json::array({20, 120})}}})}});
        QVERIFY(!id.empty());
        s.ops.clear();
        s.trigger("act_select");
        // a click chooses it (no op); the panel and the settings beside the tool show it
        inject::click_mm(s.canvas(), QPointF(60.0, 60.0));
        QVERIFY(s.ops.empty());
        QCOMPARE(s.canvas()->selected_line_id, std::optional<std::string>(id));
        QCOMPARE(s.panel().current_id(), std::optional<std::string>(id));
        QVERIFY(s.panel().style_body->isVisibleTo(s.panel().style_box));
        QCOMPARE(s.canvas()->line_handles().size(), std::size_t{8 + 1 + 2});
        // dragged: move_line to where it was let go (round(…, 2))
        inject::mouse_stroke(s.canvas(), {QPointF(60.0, 60.0), QPointF(65.0, 67.0), QPointF(70.0, 75.0)});
        QCOMPARE(s.last().dump(), (Json{{"op", "move_line"}, {"id", id}, {"x_mm", 50.0}, {"y_mm", 65.0}}).dump());
        QVERIFY(drawn_as_book(s));
        // the bottom-right handle: the box from its corner (x and y as they are)
        inject::mouse_stroke(s.canvas(), {QPointF(80.0, 105.0), QPointF(85.0, 110.0), QPointF(90.12, 120.46)});
        QCOMPARE(s.last().dump(), (Json{{"op", "move_line"}, {"id", id}, {"x_mm", 50.0}, {"y_mm", 65.0}, {"w_mm", 40.12}, {"h_mm", 55.46}}).dump());
        QVERIFY(s.last()["x_mm"].is_number_float());
        // the top-left one: at least 4 mm left
        inject::mouse_stroke(s.canvas(), {QPointF(50.0, 65.0), QPointF(95.0, 130.0)});
        QCOMPARE(s.last().dump(), (Json{{"op", "move_line"}, {"id", id}, {"x_mm", 86.12}, {"y_mm", 116.46}, {"w_mm", 4.0}, {"h_mm", 4.0}}).dump());
        s.window->apply_ops(Json::array({Json{{"op", "move_line"}, {"id", id}, {"x_mm", 50.0}, {"y_mm", 65.0}, {"w_mm", 40.12}, {"h_mm", 55.46}}}));
        // the ○ above: turned (Shift: 15° steps, an int as Python's round), back to upright (none)
        const double cx = 50.0 + 40.12 / 2;
        const double cy = 65.0 + 55.46 / 2;
        inject::mouse_stroke(s.canvas(), {QPointF(cx, 65.0 - 7), QPointF(cx + 10, cy - 30), QPointF(cx + 30, cy - 30)}, Qt::ShiftModifier);
        QCOMPARE(s.last().dump(), (Json{{"op", "edit_line"}, {"id", id}, {"style", Json{{"rotate_deg", 45}}}}).dump());
        inject::mouse_stroke(s.canvas(), {QPointF(cx, 65.0 - 7), QPointF(cx + 1, 30.0), QPointF(cx, 20.0)}, Qt::ShiftModifier);
        QCOMPARE(s.last().dump(), (Json{{"op", "edit_line"}, {"id", id}, {"style", Json{{"rotate_deg", nullptr}}}}).dump());
        // without Shift: a tenth of a degree, a float
        inject::mouse_stroke(s.canvas(), {QPointF(cx, 65.0 - 7), QPointF(cx + 20, cy - 20.5)});
        QVERIFY(s.last()["style"]["rotate_deg"].is_number_float());
        const double turned = s.last()["style"]["rotate_deg"].get<double>();
        QVERIFY(std::abs(turned - core::py_round(std::atan2(20.0, 20.5) * 180.0 / core::kPi, 1)) <= 0.11);
        s.window->apply_ops(Json::array({Json{{"op", "edit_line"}, {"id", id}, {"style", Json{{"rotate_deg", nullptr}}}}}));
        // the tail's tip (●): move_line with the tails, the tip where it was let go
        Json tails = app::PageCanvas::tails_of(*s.line(id));
        QCOMPARE(tails.size(), std::size_t{1});
        const QPointF tip(tails[0]["to"][0].get<double>(), tails[0]["to"][1].get<double>());
        inject::mouse_stroke(s.canvas(), {tip, QPointF(24.0, 125.0), QPointF(25.5, 130.25)});
        tails[0]["to"] = Json::array({25.5, 130.25});
        QCOMPARE(s.last().dump(), (Json{{"op", "move_line"}, {"id", id}, {"tails", tails}}).dump());
        QVERIFY(drawn_as_book(s));
        // its bend (◇, half way): a via is added
        const QPointF mid((cx + 25.5) / 2, (cy + 130.25) / 2);
        inject::mouse_stroke(s.canvas(), {mid, QPointF(mid.x() - 5, mid.y()), QPointF(40.0, 100.0)});
        tails[0]["via"] = Json::array({40.0, 100.0});
        QCOMPARE(s.last().dump(), (Json{{"op", "move_line"}, {"id", id}, {"tails", tails}}).dump());
        QVERIFY(drawn_as_book(s));
        // a corner added from its menu (_with_bend): the via becomes the first of its corners
        auto menu = s.window->line_menu(id);
        QVERIFY(menu != nullptr);
        QAction* bend = find_action(menu.get(), QStringLiteral("しっぽに曲がり角を足す（折れ線）"));
        QVERIFY(bend != nullptr);
        QCOMPARE(bend->toolTip(), QStringLiteral("しっぽの途中に角を足します。□をドラッグして折れ線のしっぽにします"));
        bend->trigger();
        Json bent = Json::object();
        bent["to"] = Json::array({25.5, 130.25});
        bent["vias"] = Json::array({Json::array({40.0, 100.0}), Json::array({r2((40.0 + 25.5) / 2 + 3), r2((100.0 + 130.25) / 2)})});
        QCOMPARE(s.last().dump(), (Json{{"op", "move_line"}, {"id", id}, {"tails", Json::array({bent})}}).dump());
        // a corner dragged (one ◇ per corner)
        QCOMPARE(s.canvas()->line_handles().size(), std::size_t{8 + 1 + 3});
        inject::mouse_stroke(s.canvas(), {QPointF(bent["vias"][1][0].get<double>(), bent["vias"][1][1].get<double>()), QPointF(30.0, 118.0)});
        bent["vias"][1] = Json::array({30.0, 118.0});
        QCOMPARE(s.last().dump(), (Json{{"op", "move_line"}, {"id", id}, {"tails", Json::array({bent})}}).dump());
        QVERIFY(drawn_as_book(s));
        // a press on a handle and no drag: the tails as they are (Python sends them all the same)
        const std::size_t count = s.ops.size();
        inject::click_mm(s.canvas(), QPointF(25.5, 130.25));
        QCOMPARE(s.ops.size(), count + 1);
        QCOMPARE(s.last()["tails"], Json::array({bent}));
        // a resize let go where it began: nothing
        inject::click_mm(s.canvas(), QPointF(50.0 + 40.12, 65.0 + 55.46));
        QCOMPARE(s.ops.size(), count + 1);
        // Esc while a balloon is dragged: it stays where it was (the release then chooses the panel, as Python's)
        inject::mouse(s.canvas(), inject::Phase::Press, QPointF(60.0, 80.0));
        inject::mouse(s.canvas(), inject::Phase::Move, QPointF(100.0, 150.0));
        QTest::keyClick(s.canvas(), Qt::Key_Escape);
        inject::mouse(s.canvas(), inject::Phase::Release, QPointF(100.0, 150.0));
        for (std::size_t i = count + 1; i < s.ops.size(); ++i) QVERIFY(s.ops[i]["op"] != Json("move_line"));
        QCOMPARE(s.line(id)->x_mm.value(), 50.0);
        // the pointer over a balloon: it can be dragged
        inject::mouse(s.canvas(), inject::Phase::Move, QPointF(60.0, 80.0));
        QCOMPARE(s.canvas()->cursor().shape(), Qt::SizeAllCursor);
    }

    void aLineIsTypedOverDeletedTurnedAndReshaped() {
        Studio s;
        const std::string id = s.put(Json{{"op", "add_line"}, {"text", "テスト"}, {"x_mm", 50.0}, {"y_mm", 65.0}, {"w_mm", 40.12}, {"h_mm", 55.46}});
        s.ops.clear();
        // nothing chosen: the commands say how
        s.trigger("act_line_edit");
        QCOMPARE(s.window->last_notice(), QStringLiteral("先に選択ツール（V）で台詞（フキダシ）をクリックして選びます"));
        QVERIFY(s.canvas()->editor() == nullptr);
        s.trigger("act_line_delete");
        s.trigger("act_line_wrap");
        QVERIFY(s.ops.empty());
        // a double click types over it: the editor on its corner with its words as typed (with_marks)
        s.trigger("act_select");
        double_click_mm(s.canvas(), QPointF(60.0, 80.0));
        app::InlineEditor* editor = s.canvas()->editor();
        QVERIFY(editor != nullptr);
        QCOMPARE(editor->toPlainText(), QStringLiteral("テスト"));
        QCOMPARE(s.canvas()->selected_line_id, std::optional<std::string>(id));
        editor->moveCursor(QTextCursor::End);
        ime_commit(editor, QStringLiteral("です"));
        ctrl_enter(editor);
        // edit_line, then move_line keeping the balloon's centre (Python's refit: 8.97 × 35.89 here)
        Json want = Json::array({Json{{"op", "edit_line"}, {"id", id}, {"text", "テストです"}, {"ruby_runs", Json::array()},
                                      {"emphasis_runs", Json::array()}, {"style_runs", Json::array()}},
                                 Json{{"op", "move_line"}, {"id", id}, {"x_mm", 65.58}, {"y_mm", 74.78}, {"w_mm", 8.97}, {"h_mm", 35.89}}});
        QCOMPARE(s.since(s.ops.size() - 2).dump(), want.dump());
        QVERIFY(drawn_as_book(s));
        // F2: the chosen line in place; the same words again change nothing
        s.window->apply_ops(Json::array({Json{{"op", "move_line"}, {"id", id}, {"x_mm", 50.0}, {"y_mm", 65.0}, {"w_mm", 40.12}, {"h_mm", 55.46}}}));
        const std::size_t count = s.ops.size();
        s.trigger("act_line_edit");
        QVERIFY(s.canvas()->editor() != nullptr);
        QCOMPARE(s.canvas()->editor()->toPlainText(), QStringLiteral("テストです"));
        ctrl_enter(s.canvas()->editor());
        QCOMPARE(s.ops.size(), count);
        // 縦書き・横書き: edit_line wrap and move_line to the new size (its top-right corner kept)
        s.window->apply_ops(Json::array({Json{{"op", "edit_line"}, {"id", id}, {"text", "テスト"}}}));
        s.trigger("act_line_wrap");
        want = Json::array({Json{{"op", "edit_line"}, {"id", id}, {"wrap", "horizontal"}},
                            Json{{"op", "move_line"}, {"id", id}, {"x_mm", 67.75}, {"y_mm", 65.0}, {"w_mm", 22.37}, {"h_mm", 8.96}}});
        QCOMPARE(s.since(s.ops.size() - 2).dump(), want.dump());
        QVERIFY(drawn_as_book(s));
        // フキダシの形 from the menu
        QAction* cloud = find_action(s.menu_at(QStringLiteral("ページ/台詞/フキダシの形")), QStringLiteral("雲（もくもく）"));
        QVERIFY(cloud != nullptr);
        cloud->trigger();
        QCOMPARE(s.last().dump(), (Json{{"op", "edit_line"}, {"id", id}, {"balloon", "cloud"}}).dump());
        QVERIFY(drawn_as_book(s));
        // 消す, and Delete on a chosen balloon
        s.trigger("act_line_delete");
        QCOMPARE(s.last().dump(), (Json{{"op", "delete_line"}, {"id", id}}).dump());
        QVERIFY(!s.canvas()->selected_line_id);
        QVERIFY(drawn_as_book(s));
        const std::string other = s.put(Json{{"op", "add_line"}, {"text", "もう一つ"}, {"x_mm", 20.0}, {"y_mm", 20.0}, {"w_mm", 20.0}, {"h_mm", 30.0}});
        s.trigger("act_select");
        inject::click_mm(s.canvas(), QPointF(25.0, 25.0));
        QCOMPARE(s.canvas()->selected_line_id, std::optional<std::string>(other));
        s.trigger("act_delete_area");
        QCOMPARE(s.last().dump(), (Json{{"op", "delete_line"}, {"id", other}}).dump());
        QVERIFY(!s.canvas()->selected_line_id);
        QVERIFY(s.window->book().story.empty());
    }

    void aBalloonIsDrawnByHand() {
        Studio s;
        const std::string frame = s.frame_id();
        s.trigger("act_balloon_pen");
        QCOMPARE(s.canvas()->tool(), QStringLiteral("text"));
        QVERIFY(s.window->text_settings()->draw_balloon->isChecked());
        QVERIFY(s.canvas()->balloon_pen);
        QVERIFY(s.window->action(QStringLiteral("act_balloon_pen"))->isChecked());
        // the outline drawn, then the words typed at its corner: add_line with the outline as its path
        inject::mouse_stroke(s.canvas(), {QPointF(60.0, 60.0), QPointF(90.0, 60.0), QPointF(90.0, 90.0), QPointF(60.0, 90.0), QPointF(60.0, 61.0)});
        app::InlineEditor* editor = s.canvas()->editor();
        QVERIFY(editor != nullptr);
        const QPointF corner = s.canvas()->to_widget(QPointF(60.0, 60.0));
        QCOMPARE(editor->pos(), QPoint(static_cast<int>(corner.x()), static_cast<int>(corner.y())));
        QVERIFY(s.ops.empty());
        ime_commit(editor, QStringLiteral("フキダシ"));
        ctrl_enter(editor);
        Json want = Json::parse(R"({"op": "add_line", "page": 1, "text": "フキダシ", "balloon": "speech",
            "path": [[60.0, 60.0], [90.0, 60.0], [90.0, 90.0], [60.0, 90.0], [60.0, 61.0]], "wrap": "vertical", "frame_id": ""})");
        want["frame_id"] = frame;
        QCOMPARE(s.last().dump(), want.dump());
        QCOMPARE(s.canvas()->selected_line_id, std::optional<std::string>(s.window->book().story.back().id));
        QCOMPARE(s.canvas()->tool(), QStringLiteral("text"));  // (Python's _balloon_drawn keeps the tool)
        QVERIFY(drawn_as_book(s));
        // a narration, an effect or bare words drawn by hand are a speech balloon; across when asked
        s.window->text_settings()->balloon->setCurrentIndex(s.window->text_settings()->balloon->findData(QStringLiteral("narration")));
        s.window->text_settings()->vertical->setChecked(false);
        inject::mouse_stroke(s.canvas(), {QPointF(120.0, 200.0), QPointF(150.0, 200.0), QPointF(150.0, 230.0), QPointF(120.0, 230.0)});
        QVERIFY(s.canvas()->editor() != nullptr);
        ime_commit(s.canvas()->editor(), QStringLiteral("ナレ"));
        ctrl_enter(s.canvas()->editor());
        QCOMPARE(s.last()["balloon"], Json("speech"));
        QCOMPARE(s.last()["wrap"], Json("horizontal"));
        // too small an outline: nothing; a click: the editor where clicked, the line placed as without the pen
        const std::size_t count = s.ops.size();
        inject::mouse_stroke(s.canvas(), {QPointF(30.0, 30.0), QPointF(32.0, 31.0), QPointF(33.0, 33.0)});
        QVERIFY(s.canvas()->editor() == nullptr);
        QCOMPARE(s.ops.size(), count);
        inject::click_mm(s.canvas(), QPointF(100.0, 150.0));
        QVERIFY(s.canvas()->editor() != nullptr);
        ime_commit(s.canvas()->editor(), QStringLiteral("やあ"));
        ctrl_enter(s.canvas()->editor());
        QCOMPARE(s.last()["op"], Json("add_line"));
        QVERIFY(!s.last().contains("path"));
        QCOMPARE(s.last()["balloon"], Json("narration"));
        // off again (the menu's switch and the settings' follow each other)
        s.trigger("act_balloon_pen");
        QVERIFY(!s.window->text_settings()->draw_balloon->isChecked());
        QVERIFY(!s.canvas()->balloon_pen);
        s.window->text_settings()->draw_balloon->setChecked(true);
        QVERIFY(s.window->action(QStringLiteral("act_balloon_pen"))->isChecked());
    }

    void theEraserCutsBalloons() {
        Studio s;
        const std::string id = s.put(Json{{"op", "add_line"}, {"text", "フキダシ"}, {"x_mm", 40.0}, {"y_mm", 50.0}, {"w_mm", 30.0}, {"h_mm", 40.0}});
        s.put(Json{{"op", "add_line"}, {"text", "ドン"}, {"balloon", "sfx"}, {"x_mm", 40.0}, {"y_mm", 50.0}, {"w_mm", 30.0}, {"h_mm", 40.0}});
        s.ops.clear();
        s.window->choose_tool(QStringLiteral("eraser"));
        auto* box = s.window->tool_settings()->page_for(QStringLiteral("eraser"))->findChild<QCheckBox*>(QStringLiteral("eraser_balloons"));
        QVERIFY(box != nullptr);
        QCOMPARE(box->text(), QStringLiteral("フキダシを削る"));
        QCOMPARE(box->toolTip(), QStringLiteral("消しゴムでなぞった所の、台詞のフキダシ（中と線）を削ります。絵は消しません"));
        box->setChecked(true);
        const std::size_t strokes = gui_test::ink_strokes(s.window->book());
        // cut_balloon for each balloon the line went over (not the effect's letters), the eraser's width; nothing erased
        inject::mouse_stroke(s.canvas(), {QPointF(35.0, 60.0), QPointF(55.0, 70.0), QPointF(75.0, 80.0)});
        QCOMPARE(s.ops.size(), std::size_t{1});
        const Json op = s.last();
        QCOMPARE(op["op"], Json("cut_balloon"));
        QCOMPARE(op["id"], Json(id));
        QCOMPARE(op["width_mm"], Json(2.0));
        QVERIFY(op["points"].size() >= 3);
        QCOMPARE(op["points"][0], Json::array({35.0, 60.0}));
        QCOMPARE(gui_test::ink_strokes(s.window->book()), strokes);
        QVERIFY(drawn_as_book(s));
        // nowhere near a balloon: said so
        inject::mouse_stroke(s.canvas(), {QPointF(150.0, 250.0), QPointF(160.0, 260.0)});
        QCOMPARE(s.ops.size(), std::size_t{1});
        QCOMPARE(s.window->last_notice(), QStringLiteral("なぞった所にフキダシがありません"));
    }

    void theLinesPanelListsReordersAndEdits() {
        Studio s;
        s.window->go_to_page(2);
        QCOMPARE(s.window->page_index(), 1);
        const std::string frame = s.frame_id();
        const std::string p1 = s.put(Json{{"op", "add_line"}, {"id", "P1"}, {"text", "一つ目"}, {"x_mm", 150.0}, {"y_mm", 20.0}, {"w_mm", 10.0}, {"h_mm", 20.0}});
        const std::string p2 = s.put(Json{{"op", "add_line"}, {"id", "P2"}, {"text", "二つ目"}, {"x_mm", 120.0}, {"y_mm", 20.0}, {"w_mm", 10.0}, {"h_mm", 20.0}});
        QCOMPARE(p1, std::string("P1"));
        s.ops.clear();
        auto& panel = s.panel();
        QTRY_COMPARE(panel.list->count(), 2);
        QCOMPARE(panel.list->item(0)->text(), QStringLiteral("1.〔普通〕一つ目"));
        QCOMPARE(panel.list->item(1)->toolTip(), QStringLiteral("二つ目\n普通（楕円）"));
        QVERIFY(!panel.apply_button->isEnabled());
        // コマに追加: into the chosen panel, left of the lines there (Python's place_new)
        panel.text->setPlainText(QStringLiteral("こんにちは"));
        panel.add();
        QCOMPARE(s.window->last_notice(), QStringLiteral("先に編集画面でコマをクリックして選びます（テキストツール T なら、置きたい所をクリック）"));
        s.window->apply_ops(Json::array({Json{{"op", "select_frame"}, {"page", 2}, {"frame_id", frame}}}));
        panel.add();
        const Json added = Json::parse(R"({"op": "add_line", "page": 2, "text": "こんにちは", "speaker": "", "frame_id": "", "balloon": "speech",
            "x_mm": 109.53, "y_mm": 16.0, "w_mm": 8.97, "h_mm": 35.89, "wrap": "vertical"})");
        Json got = s.last();
        got["frame_id"] = "";
        QCOMPARE(got.dump(), added.dump());
        QCOMPARE(s.last()["frame_id"], Json(frame));
        QCOMPARE(panel.text->toPlainText(), QStringLiteral("こんにちは"));  // (the words box cleared, then the new line chosen: its words)
        QCOMPARE(panel.list->count(), 3);
        const std::string fresh = s.window->book().story.back().id;
        QCOMPARE(panel.current_id(), std::optional<std::string>(fresh));
        QCOMPARE(s.canvas()->selected_line_id, std::optional<std::string>(fresh));
        QVERIFY(drawn_as_book(s));
        // the order: ↑ (reorder_lines), the one moved kept chosen
        panel.list->setCurrentRow(1);
        QCOMPARE(s.canvas()->selected_line_id, std::optional<std::string>(p2));
        panel.up_button->click();
        QCOMPARE(s.last().dump(), (Json{{"op", "reorder_lines"}, {"page", 2}, {"order", Json::array({"P2", "P1", fresh})}}).dump());
        QCOMPARE(panel.current_id(), std::optional<std::string>(p2));
        QCOMPARE(panel.list->item(0)->text(), QStringLiteral("1.〔普通〕二つ目"));
        const std::size_t count = s.ops.size();
        panel.up_button->click();  // (already first)
        QCOMPARE(s.ops.size(), count);
        panel.down_button->click();
        QCOMPARE(s.last()["order"], Json::array({"P1", "P2", fresh}));
        // the words, speaker, kind and direction: edit_line, and move_line to the new size (Python's refit)
        panel.select(std::string("P1"));
        QCOMPARE(panel.text->toPlainText(), QStringLiteral("一つ目"));
        QVERIFY(panel.vertical->isChecked());
        panel.speaker->setText(QStringLiteral(" 太郎 "));
        panel.text->setPlainText(QStringLiteral("直した台詞"));
        panel.kind->setCurrentIndex(panel.kind->findData(QStringLiteral("thought")));
        panel.vertical->setChecked(false);
        panel.apply_button->click();
        const Json edited = Json::array(
            {Json{{"op", "edit_line"}, {"id", "P1"}, {"text", "直した台詞"}, {"speaker", "太郎"}, {"balloon", "thought"}, {"wrap", "horizontal"},
                  {"ruby_runs", Json::array()}, {"emphasis_runs", Json::array()}, {"style_runs", Json::array()}},
             Json{{"op", "move_line"}, {"id", "P1"}, {"x_mm", 124.11}, {"y_mm", 20.0}, {"w_mm", 35.89}, {"h_mm", 8.97}}});
        QCOMPARE(s.since(s.ops.size() - 2).dump(), edited.dump());
        QVERIFY(drawn_as_book(s));
        // the same words again: the edit alone
        panel.apply_button->click();
        QCOMPARE(s.last()["op"], Json("edit_line"));
        panel.text->setPlainText(QStringLiteral("  "));
        const std::size_t edits = s.ops.size();
        panel.apply_button->click();
        QCOMPARE(s.ops.size(), edits);
        QCOMPARE(s.window->last_notice(), QStringLiteral("台詞が空です。消すときは「削除」を押します"));
        // ruby, dots and styled parts typed back as they were written (with_marks)
        s.window->apply_ops(Json::array({Json{{"op", "edit_line"}, {"id", "P1"}, {"text", "絶対に約束する"}, {"ruby_runs", Json::array({Json::array({"約束", "やくそく"})})},
                                              {"emphasis_runs", Json::array({"絶対"})},
                                              {"style_runs", Json::array({Json::array({"する", Json{{"scale", 1.25}, {"rgb", Json::array({1, 2, 255})}, {"bold", true}}})})}}}));
        s.window->on_line_selected("P1", false);
        QCOMPARE(panel.text->toPlainText(), QStringLiteral("《《絶対》》に｜約束《やくそく》{太、#0102ff、×1.25|する}"));
        // the lettering beside the tool: one edit_line with every field (Python's _style_changed)
        QVERIFY(panel.style_body->isVisibleTo(panel.style_box));
        QCOMPARE(panel.style_title->text(), QStringLiteral("選んだ台詞の文字とフキダシ: 「絶対に約束する」"));
        QCOMPARE(panel.leading->value(), 0.4);
        QVERIFY(panel.tcy->isChecked());
        QVERIFY(!panel.tail_width->isEnabled());
        QCOMPARE(panel.layer_order->itemText(0), QStringLiteral("いちばん上（既定）"));
        QCOMPARE(panel.layer_order->count(), static_cast<int>(s.page().layers.size()) + 1);
        panel.size->setValue(6.5);
        emit panel.size->editingFinished();
        const Json style = Json::parse(R"({"size_mm": 6.5, "tracking": 0.0, "leading": 0.4, "outline_mm": null, "border_mm": 0.35, "align": "top",
            "fill": "white", "tcy": true, "rotate_deg": null, "skew_deg": null, "arc": null, "latin": "rotate", "emphasis_mark": "sesame",
            "weight": null, "bold": null, "italic": null, "wobble": null, "double": null, "spikes": null, "spike_depth": null, "scale_x": null,
            "yakumono": null, "fill_opacity": null, "text_dx_mm": null, "text_dy_mm": null, "path_curve": null, "ruby_scale": null,
            "mono_ruby": null})");
        QCOMPARE(s.last().dump(), (Json{{"op", "edit_line"}, {"id", "P1"}, {"style", style}}).dump());
        QVERIFY(drawn_as_book(s));
        // (the panel follows an edit a moment later, as Python's side panels)
        QTRY_COMPARE(panel.size->value(), 6.5);
        panel.weight->setCurrentIndex(2);
        emit panel.weight->activated(2);
        QCOMPARE(s.last()["style"]["weight"], Json("heavy"));
        panel.fill_cover->setValue(40);
        emit panel.fill_cover->editingFinished();
        QCOMPARE(s.last()["style"]["fill_opacity"], Json(0.4));
        panel.spikes->setValue(12);
        emit panel.spikes->editingFinished();
        QVERIFY(s.last()["style"]["spikes"].is_number_integer());
        panel.yakumono->setChecked(false);
        emit panel.yakumono->clicked(false);
        QCOMPARE(s.last()["style"]["yakumono"], Json(false));
        // a face, a colour, a gradient, the layer it sits under
        panel.font->setCurrentIndex(panel.font->findData(QStringLiteral("maru")));
        emit panel.font->activated(panel.font->currentIndex());
        QCOMPARE(s.last().dump(), (Json{{"op", "edit_line"}, {"id", "P1"}, {"style", Json{{"font", "maru"}}}}).dump());
        s.answers.responder->colour = [](const QColor&, const QString& title) {
            return std::optional<QColor>(title == QStringLiteral("フキダシの線の色") ? QColor(10, 20, 30) : QColor(200, 100, 50));
        };
        panel.line_colour->click();
        QCOMPARE(s.last().dump(), (Json{{"op", "edit_line"}, {"id", "P1"}, {"style", Json{{"line_rgb", Json::array({10, 20, 30})}}}}).dump());
        panel.color->click();
        QCOMPARE(s.last()["style"], Json({{"rgb", Json::array({200, 100, 50})}}));
        panel.outline_colour->click();
        QCOMPARE(s.last()["style"].dump(), (Json{{"outline_rgb", Json::array({200, 100, 50})}, {"outline_mm", 0.6}}).dump());
        panel.gradient->click();
        QCOMPARE(s.last()["style"]["gradient"].dump(),
                 (Json{{"rgb_from", Json::array({200, 100, 50})}, {"rgb_to", Json::array({200, 100, 50})}, {"angle", 90}}).dump());
        QTRY_COMPARE(panel.gradient->text(), QStringLiteral("文字のグラデーションを外す"));
        panel.gradient->click();
        QCOMPARE(s.last()["style"], Json({{"gradient", nullptr}}));
        QTRY_COMPARE(panel.layer_order->count(), static_cast<int>(s.page().layers.size()) + 1);
        panel.layer_order->setCurrentIndex(1);
        emit panel.layer_order->activated(1);
        QCOMPARE(s.last()["style"], Json({{"below_layer", s.page().layers.back().id}}));
        QVERIFY(drawn_as_book(s));
        // the computer's fonts (fonts.json in the config folder, as Python keeps them)
        {
            std::ofstream(gui_test::path_of(s.config.path()) / "fonts.json")
                << R"([{"name": "Test Mincho", "style": "Regular", "path": "/fonts/test-mincho.ttf"}])";
        }
        s.answers.responder->get_item = [](const QString& title, const QString&, const QStringList& items, int, bool) {
            return title == QStringLiteral("パソコンの書体") && items == QStringList{QStringLiteral("Test Mincho Regular")} ? std::optional<QString>(items.front())
                                                                                                                       : std::nullopt;
        };
        panel.font->setCurrentIndex(panel.font->findData(QStringLiteral("__pick__")));
        emit panel.font->activated(panel.font->currentIndex());
        QCOMPARE(s.last()["style"], Json({{"font", "/fonts/test-mincho.ttf"}}));
        QTRY_COMPARE(panel.font->currentText(), QStringLiteral("test-mincho"));
        // 既定の設定に戻す: every style key but the group, cleared
        panel.reset->click();
        QCOMPARE(s.last()["style"].size(), std::size_t{46});
        QVERIFY(!s.last()["style"].contains("group"));
        QCOMPARE(s.last()["style"].begin().key(), std::string("font"));
        for (const auto& [key, value] : s.last()["style"].items()) QVERIFY2(value.is_null(), key.c_str());
        // a tail's width: the line's style, then its tails (both: the field is one of the style's, as in Python)
        s.window->go_to_page(1);
        const std::string tailed = s.put(Json{{"op", "add_line"}, {"text", "しっぽ"}, {"x_mm", 40.0}, {"y_mm", 50.0}, {"w_mm", 30.0}, {"h_mm", 40.0},
                                              {"tails", Json::array({Json{{"to", Json::array({20.0, 120.0})}, {"kind", "zigzag"}}})}});
        s.window->on_line_selected(tailed, false);
        QVERIFY(panel.tail_width->isEnabled());
        const std::size_t before_width = s.ops.size();
        panel.tail_width->setValue(4);
        emit panel.tail_width->editingFinished();
        QCOMPARE(s.ops.size(), before_width + 2);
        QCOMPARE(s.ops[before_width]["op"], Json("edit_line"));
        QCOMPARE(s.last().dump(),
                 (Json{{"op", "move_line"}, {"id", tailed}, {"tails", Json::array({Json{{"to", Json::array({20.0, 120.0})}, {"kind", "zigzag"}, {"width_mm", 4.0}}})}}).dump());
        QVERIFY(drawn_as_book(s));
        // 消す
        panel.delete_button->click();
        QCOMPARE(s.last().dump(), (Json{{"op", "delete_line"}, {"id", tailed}}).dump());
        QCOMPARE(panel.list->count(), 0);
        QVERIFY(panel.empty_note->isVisibleTo(panel.list));
    }

    void theStoryEditorPoursAScriptAndEditsEveryLine() {
        // the script read as Python's parse_script reads it
        const auto rows = app::parse_script(QStringLiteral("# 2\n太郎「おはよう」\nナレ：翌朝\n（心の声）\n---\n花子：遅いよ\n「やあ」\nP5\nN: 終わり"), 1);
        QCOMPARE(rows.size(), std::size_t{6});
        const std::vector<std::tuple<int, const char*, const char*, const char*>> want_rows = {
            {2, "太郎", "おはよう", "speech"}, {2, "", "翌朝", "narration"}, {2, "", "心の声", "thought"},
            {3, "花子", "遅いよ", "speech"},   {3, "", "やあ", "speech"},     {5, "", "終わり", "narration"}};
        for (std::size_t i = 0; i < rows.size(); ++i) {
            QCOMPARE(rows[i].page, std::int64_t{std::get<0>(want_rows[i])});
            QCOMPARE(rows[i].speaker, QString::fromUtf8(std::get<1>(want_rows[i])));
            QCOMPARE(rows[i].text, QString::fromUtf8(std::get<2>(want_rows[i])));
            QCOMPARE(rows[i].balloon, QString::fromUtf8(std::get<3>(want_rows[i])));
        }
        QCOMPARE(app::parse_script(QStringLiteral("＃３\n「全角」"), 1).front().page, std::int64_t{3});

        Studio s;
        s.trigger("act_story_editor");
        app::StoryEditor* editor = s.window->story_editor();
        QVERIFY(editor != nullptr && editor->isVisible());
        QCOMPARE(editor->windowTitle(), QStringLiteral("ストーリーエディター（全ページの台詞）"));
        QCOMPARE(editor->table->rowCount(), 0);
        QCOMPARE(editor->table->horizontalHeaderItem(2)->text(), QStringLiteral("台詞（｜漢字《かんじ》でルビ）"));
        // 台本を流し込む (the dialog answered: its words typed and 表に入れる)
        s.answers.responder->exec = [](QDialog* dialog) {
            auto* pour = qobject_cast<app::PourDialog*>(dialog);
            if (pour == nullptr) return 0;
            pour->text->setPlainText(QStringLiteral("# 2\n太郎「おはよう」\nナレ：翌朝\n（心の声）"));
            return static_cast<int>(QDialog::Accepted);
        };
        editor->pour();
        QCOMPARE(editor->table->rowCount(), 3);
        QCOMPARE(editor->status->text(), QStringLiteral("台詞 3 行（新しい行 3、消す行 0）"));
        editor->apply_button->click();
        // Python's plan: the new lines placed panel by panel on a copy, each with its own id
        const Json want = Json::parse(R"([{"op": "add_line", "page": 2, "id": "", "text": "おはよう", "speaker": "太郎", "balloon": "speech",
            "frame_id": "", "x_mm": 185.17, "y_mm": 16.0, "w_mm": 8.83, "h_mm": 29.59, "wrap": "vertical"},
            {"op": "add_line", "page": 2, "id": "", "text": "翌朝", "speaker": "", "balloon": "narration", "frame_id": "", "x_mm": 176.17,
            "y_mm": 16.0, "w_mm": 7.5, "h_mm": 12.5, "wrap": "vertical"},
            {"op": "add_line", "page": 2, "id": "", "text": "心の声", "speaker": "", "balloon": "thought", "frame_id": "", "x_mm": 165.71,
            "y_mm": 16.0, "w_mm": 8.96, "h_mm": 22.37, "wrap": "vertical"}])");
        Json got = s.since(0);
        QCOMPARE(got.size(), std::size_t{3});
        const std::string frame2 = s.window->book().page(1).leaf_frames().front()->id;
        for (Json& op : got) {
            QCOMPARE(op["id"].get<std::string>().size(), std::size_t{12});
            QCOMPARE(op["frame_id"], Json(frame2));
            op["id"] = "";
            op["frame_id"] = "";
        }
        QCOMPARE(got.dump(), want.dump());
        QCOMPARE(editor->table->rowCount(), 3);
        QVERIFY(editor->status->text().startsWith(QStringLiteral("原稿に反映しました（3 件の変更")));
        // a line edited in the table: edit_line, and move_line for the new words; a row deleted; the order of a page
        editor->table->item(0, 2)->setText(QStringLiteral("おはようございます"));
        editor->table->setCurrentCell(2, 2);
        editor->delete_rows();
        QCOMPARE(editor->status->text(), QStringLiteral("台詞 2 行（新しい行 0、消す行 1）"));
        editor->table->setCurrentCell(1, 2);
        editor->move(-1);
        s.ops.clear();
        editor->apply_button->click();
        QCOMPARE(s.ops.size(), std::size_t{4});
        QCOMPARE(s.ops[0]["op"], Json("delete_line"));
        QCOMPARE(s.ops[1]["op"], Json("edit_line"));
        QCOMPARE(s.ops[1]["text"], Json("おはようございます"));
        QCOMPARE(s.ops[2]["op"], Json("move_line"));
        QCOMPARE(s.ops[3]["op"], Json("reorder_lines"));
        QCOMPARE(s.ops[3]["page"], Json(2));
        // nothing changed: nothing applied
        s.ops.clear();
        editor->apply_button->click();
        QVERIFY(s.ops.empty());
        QCOMPARE(editor->status->text(), QStringLiteral("変わったところはありません"));
        // a row for a page the book does not have: add_page first
        editor->table->setCurrentCell(-1, -1);
        editor->add_row();
        const int r = editor->table->rowCount() - 1;
        editor->table->item(r, 0)->setText(QStringLiteral("3"));
        editor->table->item(r, 2)->setText(QStringLiteral("三ページ目"));
        editor->apply_button->click();
        QCOMPARE(s.ops.front().dump(), (Json{{"op", "add_page"}, {"count", 1}}).dump());
        QCOMPARE(s.last()["op"], Json("add_line"));
        QCOMPARE(s.last()["page"], Json(3));
        QCOMPARE(s.window->book().pages.size(), std::size_t{3});
        s.window->go_to_page(2);
        QVERIFY(drawn_as_book(s));
        editor->close();
    }

    void wordsAreFoundAndReplaced() {
        Studio s;
        s.put(Json{{"op", "add_line"}, {"text", "台詞の台詞"}, {"speaker", "台詞屋"}, {"x_mm", 40.0}, {"y_mm", 50.0}, {"w_mm", 20.0}, {"h_mm", 30.0}});
        s.ops.clear();
        int searched = -1;
        QString counted;
        QString listed;
        s.answers.responder->exec = [&](QDialog* dialog) {
            auto* d = qobject_cast<app::ReplaceDialog*>(dialog);
            if (d == nullptr) return 0;
            d->find->setText(QStringLiteral("台詞"));
            d->replace->setText(QStringLiteral("セリフ"));
            d->speakers->setChecked(true);
            searched = d->search();
            counted = d->count->text();
            listed = d->results->count() == 1 ? d->results->item(0)->text() : QString();
            d->replace_all();
            return 0;
        };
        s.trigger("act_replace");
        QCOMPARE(searched, 3);
        QCOMPARE(counted, QStringLiteral("3 か所（1 行）"));
        QCOMPARE(listed, QStringLiteral("1 ページ　台詞屋：台詞の台詞"));
        QCOMPARE(s.last().dump(), (Json{{"op", "replace_text"}, {"find", "台詞"}, {"replace", "セリフ"}, {"regex", false}, {"speakers", true}}).dump());
        QCOMPARE(s.window->last_notice(), QStringLiteral("3 か所を置き換えました（元に戻すは 1 回）"));
        QCOMPARE(s.window->book().story.front().text, std::string("セリフのセリフ"));
        QVERIFY(drawn_as_book(s));
        // a pattern of re's own: the op this build does not have yet, said in words
        s.answers.responder->exec = [&](QDialog* dialog) {
            auto* d = qobject_cast<app::ReplaceDialog*>(dialog);
            if (d == nullptr) return 0;
            d->find->setText(QStringLiteral("セ.フ"));
            d->regex->setChecked(true);
            d->replace_all();
            return 0;
        };
        const std::size_t count = s.ops.size();
        s.trigger("act_replace");
        QCOMPARE(s.ops.size(), count);
        QCOMPARE(s.window->last_error(),
                 QStringLiteral("この版の Genko では、まだ正規表現での置換はできません（「正規表現で探す」を外すと置き換えられます）"));
        // nothing there
        s.answers.responder->exec = [&](QDialog* dialog) {
            auto* d = qobject_cast<app::ReplaceDialog*>(dialog);
            if (d == nullptr) return 0;
            d->find->setText(QStringLiteral("ない言葉"));
            searched = d->search();
            counted = d->count->text();
            return 0;
        };
        s.trigger("act_replace");
        QCOMPARE(searched, 0);
        QCOMPARE(counted, QStringLiteral("見つかりません"));
    }

    void aBalloonsOwnMenu() {
        Studio s;
        const std::string a = s.put(Json{{"op", "add_line"}, {"id", "AAAAAAAAAAAA"}, {"text", "一"}, {"x_mm", 40.0}, {"y_mm", 50.0}, {"w_mm", 30.0}, {"h_mm", 40.0}});
        const std::string b = s.put(Json{{"op", "add_line"}, {"text", "二"}, {"x_mm", 100.0}, {"y_mm", 50.0}, {"w_mm", 30.0}, {"h_mm", 40.0}});
        s.ops.clear();
        auto menu = s.window->line_menu(a);
        QVERIFY(menu != nullptr);
        QCOMPARE(s.canvas()->selected_line_id, std::optional<std::string>(a));
        QStringList items;
        for (QAction* act : menu->actions()) items << (act->isSeparator() ? QStringLiteral("|") : act->text());
        QCOMPARE(items, QStringList({QStringLiteral("打ち直す（ダブルクリック）"), QStringLiteral("フキダシの形"), QStringLiteral("横書きにする"), QStringLiteral("|"),
                                     QStringLiteral("しっぽを足す"), QStringLiteral("|"), QStringLiteral("次の台詞のフキダシとつなげる"),
                                     QStringLiteral("画像のフキダシにする…"), QStringLiteral("文字をパスに沿わせる"), QStringLiteral("|"),
                                     QStringLiteral("AI 用の参照をコピー（この台詞のコマ）"), QStringLiteral("|"), QStringLiteral("消す")}));
        QVERIFY(find_action(menu.get(), QStringLiteral("普通（楕円）"))->isChecked());
        find_action(menu.get(), QStringLiteral("横書きにする"))->trigger();
        QCOMPARE(s.last().dump(), (Json{{"op", "edit_line"}, {"id", a}, {"wrap", "horizontal"}}).dump());
        find_action(menu.get(), QStringLiteral("しっぽを足す"))->trigger();
        QCOMPARE(s.last().dump(), (Json{{"op", "move_line"}, {"id", a}, {"tails", Json::array({Json{{"to", Json::array({34.0, 100.0})}}})}}).dump());
        QVERIFY(drawn_as_book(s));
        find_action(menu.get(), QStringLiteral("次の台詞のフキダシとつなげる"))->trigger();
        QCOMPARE(s.since(s.ops.size() - 2).dump(), (Json::array({Json{{"op", "edit_line"}, {"id", a}, {"style", Json{{"group", "g_AAAAAAAA"}}}},
                                                                 Json{{"op", "edit_line"}, {"id", b}, {"style", Json{{"group", "g_AAAAAAAA"}}}}}))
                                                       .dump());
        QVERIFY(drawn_as_book(s));
        // a menu made again: with the tail and the group, the tail's commands and つなげたフキダシを離す
        menu = s.window->line_menu(a);
        QVERIFY(find_action(menu.get(), QStringLiteral("縦書きにする")) != nullptr);
        QVERIFY(find_action(menu.get(), QStringLiteral("くさび"))->isChecked());
        find_action(menu.get(), QStringLiteral("ギザギザ"))->trigger();
        QCOMPARE(s.last()["tails"], Json::array({Json{{"to", Json::array({34.0, 100.0})}, {"kind", "zigzag"}}}));
        find_action(menu.get(), QStringLiteral("しっぽをまっすぐにする"))->trigger();
        QCOMPARE(s.last()["tails"], Json::array({Json{{"to", Json::array({34.0, 100.0})}}}));
        find_action(menu.get(), QStringLiteral("しっぽを 1 本消す"))->trigger();
        QCOMPARE(s.last()["tails"], Json::array());
        find_action(menu.get(), QStringLiteral("つなげたフキダシを離す"))->trigger();
        QCOMPARE(s.since(s.ops.size() - 2).dump(), (Json::array({Json{{"op", "edit_line"}, {"id", a}, {"style", Json{{"group", nullptr}}}},
                                                                 Json{{"op", "edit_line"}, {"id", b}, {"style", Json{{"group", nullptr}}}}}))
                                                       .dump());
        find_action(menu.get(), QStringLiteral("叫び（トゲ）"))->trigger();
        QCOMPARE(s.last().dump(), (Json{{"op", "edit_line"}, {"id", a}, {"balloon", "shout"}}).dump());
        // the words along a path: the presets (Python's numbers, an int 0 kept), drawn with the pen, taken off
        find_action(menu.get(), QStringLiteral("弧（上にふくらむ）"))->trigger();
        QCOMPARE(s.last().dump(), Json::parse(R"({"op": "edit_line", "id": "AAAAAAAAAAAA", "balloon": "none", "style": {"text_path":
            [[0, 40.0], [7.5, 12.0], [15.0, 0], [22.5, 12.0], [30.0, 40.0]]}})").dump());
        QVERIFY(s.last()["style"]["text_path"][0][0].is_number_integer());
        QVERIFY(drawn_as_book(s));
        find_action(menu.get(), QStringLiteral("描いて決める（次に引く線に沿わせる）"))->trigger();
        QCOMPARE(s.canvas()->tool(), QStringLiteral("pen"));
        QCOMPARE(s.window->last_notice(), QStringLiteral("文字を沿わせる線を、ペンで 1 本引きます"));
        const std::size_t strokes = gui_test::ink_strokes(s.window->book());
        inject::mouse_stroke(s.canvas(), {QPointF(40.0, 150.0), QPointF(60.0, 140.0), QPointF(80.0, 150.0), QPointF(100.0, 160.0)});
        QCOMPARE(gui_test::ink_strokes(s.window->book()), strokes);  // (the line is the words' path, not ink)
        QCOMPARE(s.last()["op"], Json("edit_line"));
        QCOMPARE(s.last()["balloon"], Json("none"));
        const Json path = s.last()["style"]["text_path"];
        QVERIFY(path.size() >= 2);
        QVERIFY(std::abs(path[0][0].get<double>() - 0.0) < 0.6 && std::abs(path[0][1].get<double>() - 100.0) < 0.6);
        menu = s.window->line_menu(a);
        find_action(menu.get(), QStringLiteral("パスから外す"))->trigger();
        QCOMPARE(s.last().dump(), (Json{{"op", "edit_line"}, {"id", a}, {"style", Json{{"text_path", nullptr}}}}).dump());
        // a picture as the balloon (the file answered)
        QTemporaryDir pictures;
        const QString png = pictures.filePath(QStringLiteral("balloon.png"));
        QImage picture(40, 24, QImage::Format_ARGB32);
        picture.fill(QColor(255, 0, 0, 200));
        QVERIFY(picture.save(png));
        s.answers.responder->open_path = [png](const QString& caption, const QString& filter) {
            return caption == QStringLiteral("フキダシにする画像") && filter == QStringLiteral("画像 (*.png *.webp *.jpg *.jpeg)") ? png : QString();
        };
        find_action(menu.get(), QStringLiteral("画像のフキダシにする…"))->trigger();
        QCOMPARE(s.last()["balloon"], Json("picture"));
        const auto decoded = render::open_image(core::a2b_base64(s.last()["style"]["picture"].get<std::string>()));
        QCOMPARE(decoded.width(), 40);
        QCOMPARE(decoded.height(), 24);
        QVERIFY(drawn_as_book(s));
        // the panel's words for an AI
        find_action(menu.get(), QStringLiteral("AI 用の参照をコピー（この台詞のコマ）"))->trigger();
        const QString words = QStringLiteral("1 ページ目の 1 コマ目（読み順）［page 1, frame_id \"%1\"］").arg(QString::fromStdString(s.frame_id()));
        QCOMPARE(QApplication::clipboard()->text(), words);
        // 消す
        find_action(menu.get(), QStringLiteral("消す"))->trigger();
        QCOMPARE(s.last().dump(), (Json{{"op", "delete_line"}, {"id", a}}).dump());
        QVERIFY(drawn_as_book(s));
    }

    void selectedCharactersAreStyledAsTyped() {
        QPlainTextEdit edit;
        app::text_style::install(&edit);
        edit.setPlainText(QStringLiteral("なんだと"));
        QVERIFY(!app::text_style::wrap(&edit, QStringLiteral("大")));  // (nothing chosen)
        edit.selectAll();
        QVERIFY(app::text_style::wrap(&edit, QStringLiteral("大")));
        QCOMPARE(edit.toPlainText(), QStringLiteral("{大|なんだと}"));
        edit.setPlainText(QStringLiteral("絶対"));
        edit.selectAll();
        QVERIFY(app::text_style::wrap(&edit, QString()));
        QCOMPARE(edit.toPlainText(), QStringLiteral("《《絶対》》"));
        gui_test::Answers answers;
        answers.responder->get_text = [](const QString& title, const QString& label, const QString&) {
            return title == QStringLiteral("ルビ") && label == QStringLiteral("「約束」の読み") ? std::optional<QString>(QStringLiteral(" やくそく ")) : std::nullopt;
        };
        edit.setPlainText(QStringLiteral("約束"));
        edit.selectAll();
        QVERIFY(app::text_style::wrap(&edit, QStringLiteral("ruby")));
        QCOMPARE(edit.toPlainText(), QStringLiteral("｜約束《やくそく》"));
        // the menu: Python's choices, enabled while characters are chosen, with their keys
        edit.selectAll();
        std::unique_ptr<QMenu> menu(app::text_style::menu_for(&edit));
        QCOMPARE(menu->title(), QStringLiteral("選んだ文字を"));
        QCOMPARE(menu->actions().size(), 11);
        QCOMPARE(menu->actions().at(3)->shortcut(), QKeySequence(QStringLiteral("Ctrl+B")));
        QVERIFY(menu->actions().at(0)->isEnabled());
        // and the notation read back: parse_marks and with_marks as Python's
        const auto marks = app::lettering::parse_marks(QStringLiteral("《《絶対》》に｜約束《やくそく》{大、赤|する}"));
        QCOMPARE(marks.text, std::string("絶対に約束する"));
        QCOMPARE(marks.ruby_runs.dump(), Json::parse(R"([["約束", "やくそく"]])").dump());
        QCOMPARE(marks.emphasis_runs.dump(), Json::parse(R"(["絶対"])").dump());
        QCOMPARE(marks.style_runs.dump(), Json::parse(R"([["する", {"scale": 1.4, "rgb": [210, 30, 30]}]])").dump());
        QCOMPARE(app::lettering::parse_marks(QStringLiteral("{謎|そのまま}")).text, std::string("{謎|そのまま}"));
        QCOMPARE(app::lettering::parse_marks(QStringLiteral("{×9|大きすぎ}")).style_runs.size(), std::size_t{0});
        // the balloons' sizes against Python's (box_size of every kind, down and across)
        const std::vector<std::tuple<const char*, double, double, double, double>> sizes = {
            {"speech", 37.38, 43.32, 43.32, 37.38}, {"rounded", 28.5, 32.5, 32.5, 28.5},  {"thought", 37.38, 43.32, 43.32, 37.38},
            {"shout", 50.25, 60.5, 50.25, 53.7},    {"electric", 37.5, 42.76, 42.76, 37.5}, {"sfx", 50.25, 36.0, 50.25, 48.0},
            {"none", 26.0, 30.0, 30.0, 26.0},       {"dotted_box", 33.5, 37.5, 37.5, 33.5}};
        const std::string words = "こんにちは、元気ですか。今日はいい天気ですね";
        for (const auto& [kind, vw, vh, hw, hh] : sizes) {
            QCOMPARE(app::lettering::box_size(words, kind, true, 60.5, 50.25), std::make_pair(vw, vh));
            QCOMPARE(app::lettering::box_size(words, kind, false, 60.5, 50.25), std::make_pair(hw, hh));
        }
        QCOMPARE(app::lettering::box_size("やあ", "speech", true, 120.0, 160.0), std::make_pair(8.93, 15.64));
        const auto measured = app::lettering::measure({U"あいう", U"えお"}, "speech");
        QCOMPARE(measured.first, 18.401649044222758);
        QCOMPARE(measured.second, 22.62170074439381);
        const auto shout = app::lettering::measure({U"あいう", U"えお"}, "shout");
        QCOMPARE(shout.first, 26.43995756636174);
        QCOMPARE(shout.second, 32.50343522601233);
    }

    void theLinesPanelShowsWhatItCanOfAStyle() {
        // A style the panel cannot show whole: colours of fewer than three numbers (Python's _merge_style keeps
        // [int(v) for v in value][:3], so edit_line and add_line take [255]), a cover the field cannot hold (fill_opacity
        // 1e17, a float the op takes), and values written into the book by hand that are not numbers. Python's
        // QColor(*rgb) takes one number as a QRgb and fails with two (PySide logs it and carries on); _picked stops at a
        // field it cannot read. Here every field that can be shown is, the colour dialogs start from the colour Python's
        // would (else the default colour), and nothing leaves a slot: the lettering is still edited from the panel.
        Studio s([](core::Document& doc) {
            core::StoryLine& hand = doc.add_line(core::Num(1), "手書きの設定", "", std::nullopt, "", core::Num(120), core::Num(30), core::Num(30), core::Num(40));
            hand.id = "HAND";
            hand.style = Json{{"size_mm", "big"}, {"tracking", 0.3}, {"spikes", "many"}, {"rgb", "red"}, {"outline_mm", Json::array({1})},
                              {"outline_rgb", Json::array({1})}, {"border_mm", "thin"}, {"fill_opacity", "half"}, {"text_dx_mm", 2.5}};
        });
        const std::string few = s.put(Json{{"op", "add_line"}, {"text", "短い色"}, {"x_mm", 40.0}, {"y_mm", 50.0}, {"w_mm", 30.0}, {"h_mm", 40.0},
                                           {"style", Json{{"line_rgb", Json::array({255})}, {"rgb", Json::array({7})}, {"outline_rgb", Json::array({1, 2})},
                                                          {"fill_rgb", Json::array({1, 2, 3})}, {"fill_opacity", 1e17}, {"text_dy_mm", 1.5}}}});
        QVERIFY(!few.empty());
        QCOMPARE(s.line(few)->style["line_rgb"], Json::array({255}));
        auto& panel = s.panel();
        QList<QColor> offered;
        s.answers.responder->colour = [&offered](const QColor& now, const QString&) {
            offered << now;
            return std::optional<QColor>();
        };
        s.window->on_line_selected(few, false);
        QCOMPARE(panel.current_id(), std::optional<std::string>(few));
        QVERIFY(panel.style_body->isVisibleTo(panel.style_box));
        QCOMPARE(panel.fill_cover->value(), 100);  // (round(100 * 1e17): more than the field holds)
        QCOMPARE(panel.text_dy->value(), 1.5);     // (and the fields after it)
        QCOMPARE(panel.layer_order->count(), static_cast<int>(s.page().layers.size()) + 1);
        panel.line_colour->click();     // [255]: QColor(255), a QRgb
        panel.color->click();           // [7]
        panel.outline_colour->click();  // [1, 2]: no colour of two numbers (Python's TypeError): the default
        panel.fill_colour->click();
        QCOMPARE(offered, (QList<QColor>{QColor(0, 0, 255), QColor(0, 0, 7), QColor(255, 255, 255), QColor(1, 2, 3)}));
        const std::size_t count = s.ops.size();
        panel.size->setValue(5.0);
        emit panel.size->editingFinished();
        QCOMPARE(s.ops.size(), count + 1);
        QCOMPARE(s.last()["style"]["size_mm"], Json(5.0));
        // written by hand: what is not a number shows as the field's default, the rest as it is
        s.window->on_line_selected("HAND", false);
        QCOMPARE(panel.current_id(), std::optional<std::string>("HAND"));
        QCOMPARE(panel.size->value(), 0.0);
        QCOMPARE(panel.tracking->value(), 0.3);
        QCOMPARE(panel.spikes->value(), 0);
        QCOMPARE(panel.outline->value(), 0.0);
        QCOMPARE(panel.border->value(), 0.35);
        QCOMPARE(panel.fill_cover->value(), 100);
        QCOMPARE(panel.text_dx->value(), 2.5);
        offered.clear();
        panel.color->click();           // "red": Python's QColor(*"red") fails: the default
        panel.outline_colour->click();  // [1]
        QCOMPARE(offered, (QList<QColor>{QColor(10, 10, 10), QColor(0, 0, 1)}));
        const std::size_t edits = s.ops.size();
        panel.tracking->setValue(0.5);
        emit panel.tracking->editingFinished();
        QCOMPARE(s.ops.size(), edits + 1);
        QCOMPARE(s.last()["id"], Json("HAND"));
        QCOMPARE(s.last()["style"]["tracking"], Json(0.5));
    }

    void joinedBalloonsKeepWholeCharactersInTheirGroup() {
        // 次の台詞のフキダシとつなげる: the group is "g_" and the first eight characters of the line's id (Python's
        // line_id[:8]: code points), so an id of other letters keeps them whole, and the book is saved with it
        Studio s;
        const std::string a = s.put(Json{{"op", "add_line"}, {"id", "L01_こんにちは"}, {"text", "一"}, {"x_mm", 40.0}, {"y_mm", 50.0}, {"w_mm", 30.0}, {"h_mm", 40.0}});
        const std::string b = s.put(Json{{"op", "add_line"}, {"text", "二"}, {"x_mm", 100.0}, {"y_mm", 50.0}, {"w_mm", 30.0}, {"h_mm", 40.0}});
        QCOMPARE(a, std::string("L01_こんにちは"));
        s.ops.clear();
        auto menu = s.window->line_menu(a);
        QVERIFY(menu != nullptr);
        find_action(menu.get(), QStringLiteral("次の台詞のフキダシとつなげる"))->trigger();
        QCOMPARE(s.ops.size(), std::size_t{2});
        const std::string group = s.ops[0]["style"]["group"].get<std::string>();
        QCOMPARE(QString::fromStdString(group), QStringLiteral("g_L01_こんにち"));
        QCOMPARE(s.ops[1]["style"]["group"], Json(group));
        QVERIFY(drawn_as_book(s));
        s.session->save_now();
        QVERIFY(s.session->wait_saved(std::chrono::milliseconds(20000)));
        QCOMPARE(s.session->status().kind, app::SaveKind::Saved);
        const core::Document saved = gui_test::read_book(gui_test::path_of(s.tmp.path() + "/book"));
        QCOMPARE(saved.story.size(), std::size_t{2});
        for (const core::StoryLine& line : saved.story) QCOMPARE(line.style["group"], Json(group));
    }

    void aLineTypedOverStopsWhenTheBookChanges() {
        // A line typed over in place (F2, a double click) while the person goes to another book (Ctrl+Tab, the tabs):
        // the words are not put into the other book's line of the same id. As for a line typed new or a balloon drawn:
        // the work stops, and says so.
        Studio s;
        const std::string id = s.put(Json{{"op", "add_line"}, {"id", "SAME"}, {"text", "テスト"}, {"x_mm", 50.0}, {"y_mm", 65.0}, {"w_mm", 40.12}, {"h_mm", 55.46}});
        QCOMPARE(id, std::string("SAME"));
        const auto other_path = gui_test::path_of(s.tmp.path() + "/other");
        gui_test::write_book(other_path, gui_test::new_doc(2, "別の本"));
        auto other = app::Session::open(other_path, gui_test::quick(gui_test::path_of(s.tmp.path() + "/recovery-other")));
        other->apply(Json::array({Json{{"op", "add_line"}, {"page", 1}, {"id", "SAME"}, {"text", "別の台詞"}, {"x_mm", 50.0}, {"y_mm", 65.0},
                                       {"w_mm", 40.12}, {"h_mm", 55.46}}}));
        QVERIFY(other->wait_saved(std::chrono::milliseconds(20000)));
        s.session->save_now();
        QVERIFY(s.session->wait_saved(std::chrono::milliseconds(20000)));
        s.trigger("act_select");
        double_click_mm(s.canvas(), QPointF(60.0, 80.0));
        QPointer<app::InlineEditor> editor = s.canvas()->editor();
        QVERIFY(editor != nullptr);
        QCOMPARE(editor->toPlainText(), QStringLiteral("テスト"));
        editor->moveCursor(QTextCursor::End);
        ime_commit(editor, QStringLiteral("です"));
        s.ops.clear();  // (the double click chose the panel there: select_frame)
        s.window->add_document(other);
        QCOMPARE(s.window->book().title, std::string("別の本"));
        if (s.canvas()->editor() != nullptr) ctrl_enter(s.canvas()->editor());
        QVERIFY(s.canvas()->editor() == nullptr);
        QCOMPARE(s.window->line_by_id("SAME")->text, std::string("別の台詞"));
        QCOMPARE(other->document().story.front().text, std::string("別の台詞"));
        QCOMPARE(s.session->document().story.front().text, std::string("テスト"));
        QVERIFY2(s.ops.empty(), s.since(0).dump().c_str());
        QCOMPARE(s.window->last_error(), QStringLiteral("確認中に対象の原稿・ページが変更されたため、操作を中止しました。やり直してください。"));
    }

    void theTilesFollowTheBooksFont() {
        // The canvas's tiles drawn again when the book's font changes (set_meta font_path: every line without a face of
        // its own is drawn in it, while the page, its lines and the brushes are the same)
        const auto spec = core::PageSpec::custom(70, 95, 60, 85, 3, 8, 8, 7, 6);
        auto doc = std::make_shared<core::Document>(core::new_episode("書体", core::Num(1), 2, spec));
        doc->add_line(core::Num(1), "あいう漢字", "", std::nullopt, "", core::Num(10), core::Num(10), core::Num(40), core::Num(30), "box");
        app::PageRenderer renderer;
        renderer.show(doc, 0);
        renderer.want(72, 72, QRectF(0, 0, 70, 95));
        QTRY_VERIFY_WITH_TIMEOUT(renderer.settled(), 20000);
        const QImage before = renderer.compose(72);
        QCOMPARE(before, rendered(*doc, 0, 72));
        // (the book's own font file: a copy of the bundled Mincho, which draws the kanji the antique default takes from
        // its Gothic)
        QTemporaryDir fonts;
        const QString file = fonts.filePath(QStringLiteral("book-font.ttf"));
        QVERIFY(QFile::copy(QStringLiteral(":/genko/text/mincho"), file));
        auto restyled = std::make_shared<core::Document>(*doc);
        restyled->font_path = file.toStdString();
        QCOMPARE(restyled->pages[0].get(), doc->pages[0].get());
        const auto generation = renderer.generation();
        renderer.show(restyled, 0);
        QVERIFY(renderer.generation() > generation);
        QTRY_VERIFY_WITH_TIMEOUT(renderer.settled(), 20000);
        QCOMPARE(renderer.compose(72), rendered(*restyled, 0, 72));
        QVERIFY(renderer.compose(72) != before);
    }

    void theTilesFollowThePageDrawnFaintly() {
        // The canvas's tiles drawn again when the lines of the page drawn faintly over this one change (its onion skin:
        // proof and name draw that page with its lines), and when the book's font changes there
        const auto spec = core::PageSpec::custom(70, 95, 60, 85, 3, 8, 8, 7, 6);
        auto onion = std::make_shared<core::Document>(core::new_episode("オニオンの台詞", core::Num(1), 3, spec));
        onion->add_line(core::Num(1), "あいう", "", std::nullopt, "", core::Num(10), core::Num(10), core::Num(40), core::Num(30), "box");
        onion->edit_page(1).onion_from = core::Num(1);
        app::PageRenderer faint;
        faint.show(onion, 1);
        faint.want(72, 72, QRectF(0, 0, 70, 95));
        QTRY_VERIFY_WITH_TIMEOUT(faint.settled(), 20000);
        const QImage was = faint.compose(72);
        QCOMPARE(was, rendered(*onion, 1, 72));
        auto reworded = std::make_shared<core::Document>(*onion);
        reworded->story.front().text = "かきくけこ漢字";
        reworded->story.front().balloon = "cloud";
        QCOMPARE(reworded->pages[0].get(), onion->pages[0].get());
        QCOMPARE(reworded->pages[1].get(), onion->pages[1].get());
        const auto faint_generation = faint.generation();
        faint.show(reworded, 1);
        QVERIFY(faint.generation() > faint_generation);
        QTRY_VERIFY_WITH_TIMEOUT(faint.settled(), 20000);
        QCOMPARE(faint.compose(72), rendered(*reworded, 1, 72));
        QVERIFY(faint.compose(72) != was);
        // (the book's font, there too: a copy of the bundled Mincho)
        QTemporaryDir fonts;
        const QString file = fonts.filePath(QStringLiteral("book-font.ttf"));
        QVERIFY(QFile::copy(QStringLiteral(":/genko/text/mincho"), file));
        auto refonted = std::make_shared<core::Document>(*reworded);
        refonted->font_path = file.toStdString();
        faint.show(refonted, 1);
        QTRY_VERIFY_WITH_TIMEOUT(faint.settled(), 20000);
        QCOMPARE(faint.compose(72), rendered(*refonted, 1, 72));
    }

    void lettersThisBuildCannotDrawAreLeftOutAndSaid() {
        // A line whose letters this build cannot draw — OpenType features across (Pillow without raqm, the BASIC layout
        // the reference is held to, raises KeyError) — does not stop the page on the canvas: its letters are left out,
        // the band says what in words, and the rest is drawn as render_page draws the book with skip_unported.
        Studio s;
        const std::string id = s.put(Json{{"op", "add_line"}, {"text", "ABC漢字"}, {"wrap", "horizontal"}, {"balloon", "box"}, {"x_mm", 40.0},
                                          {"y_mm", 50.0}, {"w_mm", 40.0}, {"h_mm", 20.0}, {"style", Json{{"features", Json::array({"jp78"})}}}});
        QVERIFY(!id.empty());
        QVERIFY(drawn_as_book(s));
        QTRY_VERIFY(s.window->unported_band()->isVisible());
        QVERIFY2(s.window->unported_band()->text().contains(QStringLiteral("（横書きの文字の字形の指定（OpenType 機能））")),
                 qPrintable(s.window->unported_band()->text()));
        QVERIFY(ink_in(shown(s), QRectF(40.0, 50.0, 40.0, 20.0), s.canvas()->base_dpi()) > 0);  // (the box drawn)
    }

    void lineRefusalsInWords() {
        namespace w = app::wording;
        QCOMPARE(w::error(QStringLiteral("ops[0] add_line: text is required")), QStringLiteral("台詞の文字が要ります（空の台詞は置けません）"));
        QCOMPARE(w::error(QStringLiteral("ops[0] move_line: a tail's to must be two numbers [x, y]")), QStringLiteral("しっぽの先は [x, y] の 2 つの数で指定します"));
        QCOMPARE(w::error(QStringLiteral("ops[0] move_line: x_mm must be a finite number")),
                 QStringLiteral("台詞の横の位置は、ふつうの数で指定します（無限大や NaN は使えません）"));
        QCOMPARE(w::error(QStringLiteral("ops[0] add_line: path must be a finite number")),
                 QStringLiteral("フキダシの形は、ふつうの数で指定します（無限大や NaN は使えません）"));
        QCOMPARE(w::error(QStringLiteral("ops[0] cut_balloon: points needs [x_mm, y_mm]")), QStringLiteral("点は [x, y]（mm）の 2 つの数で指定します"));
        QCOMPARE(w::error(QStringLiteral("ops[0] replace_text: replace_text with regex (a pattern in Python's re syntax) is not in the C++ build yet")),
                 QStringLiteral("この版の Genko では、まだ正規表現での置換はできません（「正規表現で探す」を外すと置き換えられます）"));
        // (the families keep the others: no line, ids, kinds, order)
        QCOMPARE(w::error(QStringLiteral("ops[0] edit_line: no line abc")), QStringLiteral("台詞が見つかりません（消されたか、別のページのものです）"));
        QCOMPARE(w::error(QStringLiteral("ops[0] add_line: line abc already exists")), QStringLiteral("同じ ID がすでにあります"));
        QCOMPARE(w::error(QStringLiteral("ops[0] edit_line: wrap must be vertical or horizontal")), QStringLiteral("縦書き・横書きの指定が正しくありません"));
        QCOMPARE(w::error(QStringLiteral("ops[0] reorder_lines: order must list every line of the page exactly once")), QStringLiteral("並び順の指定が正しくありません"));
        QCOMPARE(w::error(QStringLiteral("ops[0] edit_line: id is required")), QStringLiteral("対象の指定が要ります"));
    }
};

QTEST_MAIN(TestGuiLines)
#include "test_gui_lines.moc"
