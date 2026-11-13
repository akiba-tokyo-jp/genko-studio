// フキダシ in the window (fukidashi@1, SPEC BALLOON-01 — the user's 「今風の漫画の吹き出しを各種作って置けるように」「手描き風は
// 消して良いです」): the kinds 縦長（カプセル） and 多角形（機械の声） in every menu of kinds; no control of the hand-drawn look
// left, a line of before that holds style.hand and style.wobble still edited; the 台詞 panel's new settings of a
// balloon's shape, each the one key of one edit_line and one step of 取り消し, shown only for the kinds they change; the
// palette of balloons — its pictures drawn by the renderer, only once it is shown; a pick gives the chosen line the
// look (取り消し gives the line back exactly) or is the look of the next line the text tool places; and the box of a
// capsule and of a polygon typed in the window is the one the auto-lettering measures (service::letter::measure).

#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QInputMethodEvent>
#include <QLabel>
#include <QListWidget>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>
#include <functional>

#include "app/balloon_palette.hpp"
#include "app/canvas.hpp"
#include "app/config.hpp"
#include "app/ime.hpp"
#include "app/inject.hpp"
#include "app/lettering.hpp"
#include "app/main_window.hpp"
#include "app/story_editor.hpp"
#include "app/story_panel.hpp"
#include "app/theme.hpp"
#include "app/tool_settings.hpp"
#include "core/fukidashi.hpp"
#include "core/pynum.hpp"
#include "gui_support.hpp"
#include "render/text/lettering.hpp"
#include "service/letter.hpp"

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
    std::vector<Json> ops;  // every op applied, in order
    // (edit: the book as it is on disk before it opens)
    explicit Studio(const std::function<void(core::Document&)>& edit = {}) {
        (void)gui_test::config_folder();
        previous_config = qgetenv("GENKO_CONFIG_DIR");
        qputenv("GENKO_CONFIG_DIR", config.path().toUtf8());
        const auto path = gui_test::path_of(tmp.path() + "/book");
        core::Document doc = gui_test::new_doc(2);
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
    app::BalloonPalette& palette() const { return *window->balloon_palette(); }
    const core::Page& page() const { return window->book().page(static_cast<std::size_t>(window->page_index())); }
    std::string frame_id() const { return page().leaf_frames().front()->id; }
    void trigger(const char* name) const { window->action(QString::fromLatin1(name))->trigger(); }
    Json last() const { return ops.empty() ? Json() : ops.back(); }
    const core::StoryLine* line(const std::string& id) const { return window->line_by_id(id); }
    // a line put by the test itself (as an agent or the CLI would): its id
    std::string put(Json op) {
        if (!op.contains("page")) op["page"] = page().index.json();
        if (!op.contains("frame_id")) op["frame_id"] = frame_id();
        const std::size_t before = window->book().story.size();
        if (!window->apply_ops(Json::array({op}))) return {};
        return window->book().story.size() > before ? window->book().story.back().id : std::string();
    }
    void choose(const std::string& id) {
        trigger("act_select");
        canvas()->selected_line_id = id;
        window->on_line_selected(id, false);
    }
    void choose_none() {
        canvas()->selected_line_id.reset();
        panel().list->setCurrentRow(-1);
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
    // words typed with the text tool at a place of the page, and kept (Ctrl+Enter): the add_line made
    Json type_at(const QPointF& mm, const QString& words) {
        const std::size_t before = ops.size();
        inject::click_mm(canvas(), mm);
        app::InlineEditor* editor = canvas()->editor();
        if (editor == nullptr) return Json();
        QInputMethodEvent event;
        event.setCommitString(words);
        QCoreApplication::sendEvent(editor, &event);
        QTest::keyPress(editor, Qt::Key_Control);
        QTest::keyPress(editor, Qt::Key_Return, Qt::ControlModifier);
        QWidget* focus = QApplication::focusWidget() != nullptr ? QApplication::focusWidget() : editor;
        QTest::keyRelease(focus, Qt::Key_Return, Qt::ControlModifier);
        QTest::keyRelease(focus, Qt::Key_Control);
        return ops.size() > before ? ops.back() : Json();
    }
};

QStringList texts(const QMenu* menu) {
    QStringList out;
    for (const QAction* a : menu->actions()) out << a->text();
    return out;
}

QStringList items(const QComboBox* combo) {
    QStringList out;
    for (int i = 0; i < combo->count(); ++i) out << combo->itemText(i);
    return out;
}

// What a line is, for comparing it before an edit and after its undo.
Json snapshot(const core::StoryLine& l) {
    Json tails = Json::array();
    for (const Json& t : l.tails) tails.push_back(t);
    return Json{{"id", l.id},           {"text", l.text},       {"balloon", l.balloon},  {"wrap", l.wrap},  {"x", l.x_mm.json()}, {"y", l.y_mm.json()},
                {"w", l.w_mm.json()},   {"h", l.h_mm.json()},   {"style", l.style},      {"tails", tails},
                {"tail", l.tail ? Json::array({l.tail->x.json(), l.tail->y.json()}) : Json()}};
}

int dark_pixels(const QImage& image) {
    int n = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (qGray(image.pixel(x, y)) < 128) ++n;
    return n;
}

const QString kCapsule = QStringLiteral("縦長（カプセル）");
const QString kPolygon = QStringLiteral("多角形（機械の声）");

}  // namespace

class TestGuiFukidashi : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QVERIFY(gui_test::config_folder().isValid()); }

    // The two kinds of now are in every menu of kinds: ページ → 台詞 → フキダシの形, a balloon's own menu, the 台詞 panel,
    // the text tool's settings and the story editor's rows; and each sets the kind.
    void theKindsOfNowAreInEveryMenu() {
        Studio s;
        const std::string id = s.put(Json{{"op", "add_line"}, {"text", "そうなんだ"}, {"balloon", "speech"}, {"x_mm", 60}, {"y_mm", 60}, {"w_mm", 30}, {"h_mm", 24}});
        QVERIFY(!id.empty());
        // (the kinds as listed: the oval, then the capsule; the box, then the polygon)
        QStringList labels;
        for (const auto& kind : app::lettering::kinds()) labels << kind.second;
        QCOMPARE(labels.size(), 17);
        QCOMPARE(labels.mid(0, 5), QStringList({QStringLiteral("普通（楕円）"), kCapsule, QStringLiteral("角丸"), QStringLiteral("四角"), kPolygon}));
        QCOMPARE(app::lettering::kind_label("capsule"), kCapsule);
        QCOMPARE(app::lettering::kind_label("polygon"), kPolygon);
        // 1 ページ → 台詞 → フキダシの形
        QMenu* shapes = s.menu_at(QStringLiteral("ページ/台詞/フキダシの形"));
        QVERIFY(shapes != nullptr);
        QCOMPARE(texts(shapes), labels);
        // 2 the balloon's own menu
        {
            const std::unique_ptr<QMenu> menu = s.window->line_menu(id);
            QVERIFY(menu != nullptr);
            QMenu* own = nullptr;
            for (QAction* a : menu->actions())
                if (a->menu() != nullptr && a->text() == QStringLiteral("フキダシの形")) own = a->menu();
            QVERIFY(own != nullptr);
            QCOMPARE(texts(own), labels);
            for (QAction* a : own->actions())
                if (a->text() == kPolygon) a->trigger();
            QCOMPARE(s.last(), (Json{{"op", "edit_line"}, {"id", id}, {"balloon", "polygon"}}));
            QCOMPARE(s.line(id)->balloon, std::string("polygon"));
        }
        // 3 the 台詞 panel (its list names the kind by its first word)
        QCOMPARE(items(s.panel().kind), labels);
        s.choose(id);
        QCOMPARE(s.panel().kind->currentData().toString(), QStringLiteral("polygon"));
        QVERIFY(s.panel().list->item(0)->text().contains(QStringLiteral("〔多角形〕")));
        // 4 the text tool's settings
        QCOMPARE(items(s.window->text_settings()->balloon), labels);
        // 5 the story editor's rows
        s.trigger("act_story_editor");
        app::StoryEditor* editor = s.window->story_editor();
        QVERIFY(editor != nullptr);
        QVERIFY(editor->table->rowCount() >= 1);
        auto* row_kind = qobject_cast<QComboBox*>(editor->table->cellWidget(0, 3));
        QVERIFY(row_kind != nullptr);
        QCOMPARE(items(row_kind), labels);
        QCOMPARE(row_kind->currentData().toString(), QStringLiteral("polygon"));
        editor->close();
        // (the menu's command sets the chosen line's kind)
        s.choose(id);
        for (QAction* a : shapes->actions())
            if (a->text() == kCapsule) a->trigger();
        QCOMPARE(s.last(), (Json{{"op", "edit_line"}, {"id", id}, {"balloon", "capsule"}}));
        QTRY_VERIFY(s.panel().list->item(0)->text().contains(QStringLiteral("〔縦長〕")));  // (the panel follows an edit a moment later)
    }

    // The hand-drawn look has no control left (the 台詞 panel's 線の揺れ is gone, nothing offers 手描き風 for a balloon); a
    // line of before that holds style.hand and style.wobble opens, is listed, and is edited without losing them — and
    // the trembling its wobble is drawn as can be set from 線のふるえ, its letters keeping the oval they were fitted in.
    void noControlOfTheHandDrawnLookIsLeft() {
        Studio s([](core::Document& doc) {
            core::StoryLine old;
            old.id = "old1";
            old.page_index = doc.page(0).index;
            old.text = "むかしの\n台詞";
            old.balloon = "speech";
            old.wrap = "vertical";
            old.x_mm = core::Num(60);
            old.y_mm = core::Num(60);
            old.w_mm = core::Num(30);
            old.h_mm = core::Num(26);
            old.frame_id = doc.page(0).leaf_frames().front()->id;
            old.style = Json{{"hand", false}, {"wobble", 0.5}};
            doc.story.push_back(old);
            core::StoryLine hand = old;
            hand.id = "old2";
            hand.x_mm = core::Num(100);
            hand.style = Json{{"hand", true}};
            doc.story.push_back(hand);
        });
        QVERIFY(s.window->book().read_only_reason.empty());
        QVERIFY(s.line("old1") != nullptr);
        QCOMPARE(s.line("old1")->style, (Json{{"hand", false}, {"wobble", 0.5}}));
        // no control named for the wobble; no words of the hand-drawn look on the panel or the text tool's settings
        QVERIFY(s.panel().findChild<QWidget*>(QStringLiteral("story_wobble")) == nullptr);
        QVERIFY(s.window->findChild<QWidget*>(QStringLiteral("story_wobble")) == nullptr);
        QVERIFY(s.window->findChild<QWidget*>(QStringLiteral("story_hand")) == nullptr);
        s.choose("old1");
        const QStringList gone = {QStringLiteral("手描きのよう"), QStringLiteral("手描き風"), QStringLiteral("線の揺れ"), QStringLiteral("揺らす"), QStringLiteral("ゆらぎ")};
        for (QWidget* root : {static_cast<QWidget*>(s.panel().style_box), static_cast<QWidget*>(&s.panel()), static_cast<QWidget*>(s.window->text_settings()),
                              static_cast<QWidget*>(&s.palette())}) {
            for (QWidget* w : root->findChildren<QWidget*>()) {
                QString words = w->toolTip();
                if (const auto* label = qobject_cast<QLabel*>(w)) words += label->text();
                if (const auto* button = qobject_cast<QAbstractButton*>(w)) words += button->text();
                for (const QString& word : gone) QVERIFY2(!words.contains(word), qPrintable(words));
            }
        }
        // (and no command of the menus: ページ → 台詞 and a balloon's own menu)
        std::function<void(const QMenu*)> walk = [&](const QMenu* menu) {
            for (const QAction* a : menu->actions()) {
                for (const QString& word : gone) QVERIFY2(!a->text().contains(word), qPrintable(a->text()));
                if (a->menu() != nullptr) walk(a->menu());
            }
        };
        walk(s.menu_at(QStringLiteral("ページ/台詞")));
        {
            const std::unique_ptr<QMenu> own = s.window->line_menu("old1");
            QVERIFY(own != nullptr);
            walk(own.get());
        }
        // the line of before edited from the panel: its line made thicker — hand and wobble stay as they were
        s.choose("old1");
        s.panel().border->setValue(0.6);
        emit s.panel().border->editingFinished();
        QCOMPARE(s.last()["op"], Json("edit_line"));
        QVERIFY(!s.last()["style"].contains("wobble"));
        QVERIFY(!s.last()["style"].contains("hand"));
        QCOMPARE(s.line("old1")->style["wobble"], Json(0.5));
        QCOMPARE(s.line("old1")->style["hand"], Json(false));
        QCOMPARE(s.line("old1")->style["border_mm"], Json(0.6));
        // its trembling shown as it is drawn (0.03 × 0.5 × 26 mm), and set: the wobble gives way to the amount said
        QCOMPARE(s.panel().wave->value(), 0.39);
        const std::size_t before = s.ops.size();
        emit s.panel().wave->editingFinished();  // (left as it was: no edit)
        QCOMPARE(s.ops.size(), before);
        s.panel().wave->setValue(0.2);
        emit s.panel().wave->editingFinished();
        QCOMPARE(s.last(), (Json{{"op", "edit_line"}, {"id", "old1"}, {"style", Json{{"wave_mm", 0.2}, {"wobble", nullptr}}}}));
        QVERIFY(!s.line("old1")->style.contains("wobble"));
        QCOMPARE(s.line("old1")->style["hand"], Json(false));  // (its letters stay in the ellipse they were fitted in)
        s.trigger("act_undo");
        QCOMPARE(s.line("old1")->style["wobble"], Json(0.5));
        QVERIFY(!s.line("old1")->style.contains("wave_mm"));
        // 既定の設定に戻す takes the keys of before away with the others
        s.choose("old2");
        s.panel().reset->click();
        QVERIFY(!s.line("old2")->style.contains("hand"));
        // saved and read again: the book still opens
        s.session->save_now();
        QVERIFY(s.session->wait_saved(std::chrono::milliseconds(20000)));
        const core::Document back = gui_test::read_book(gui_test::path_of(s.tmp.path() + "/book"));
        QVERIFY(back.read_only_reason.empty());
        QCOMPARE(back.story.size(), std::size_t{2});
    }

    // Each new setting of the 台詞 panel writes its one key in one edit_line, which one 取り消し takes back; a field left
    // as it was makes no edit; each row is there only for the kinds it changes.
    void eachShapeSettingWritesItsKey() {
        Studio s;
        const auto add = [&](const char* kind, Json more = Json::object()) {
            Json op{{"op", "add_line"}, {"text", "ねえ\n聞いて"}, {"balloon", kind}, {"x_mm", 60}, {"y_mm", 60}, {"w_mm", 32}, {"h_mm", 26}};
            for (const auto& [key, value] : more.items()) op[key] = value;
            return s.put(op);
        };
        const std::string rounded = add("rounded");
        const std::string speech = add("speech", Json{{"tails", Json::array({Json{{"to", Json::array({50, 100})}}})}});
        const std::string shout = add("shout");
        const std::string polygon = add("polygon");
        const std::string whisper = add("whisper");
        const std::string cloud = add("cloud");
        const std::string flash = add("flash");
        const std::string narration = add("narration");
        const std::string sfx = add("sfx");
        auto& p = s.panel();
        QFormLayout* form = p.style_form;
        QVERIFY(form != nullptr);
        const auto rows = [&] {
            QStringList shown;
            for (const auto& [widget, name] : std::initializer_list<std::pair<QWidget*, const char*>>{
                     {p.corner, "corner"}, {p.squircle, "squircle"}, {p.sides, "sides"}, {p.wave, "wave"}, {p.spike_alt, "spike_alt"},
                     {p.dash, "dash"}, {p.tail_kind, "tail_kind"}, {p.tail_fillet, "tail_fillet"}}) {
                if (form->isRowVisible(widget)) shown << QString::fromLatin1(name);
            }
            return shown.join(QLatin1Char(' '));
        };
        // (the rows by kind)
        s.choose(rounded);
        QCOMPARE(rows(), QStringLiteral("corner squircle wave dash tail_kind tail_fillet"));
        s.choose(speech);
        QCOMPARE(rows(), QStringLiteral("wave dash tail_kind tail_fillet"));
        s.choose(shout);
        QCOMPARE(rows(), QStringLiteral("spike_alt dash tail_kind tail_fillet"));
        s.choose(polygon);
        QCOMPARE(rows(), QStringLiteral("corner sides wave dash tail_kind tail_fillet"));
        s.choose(cloud);
        QCOMPARE(rows(), QStringLiteral("dash tail_kind tail_fillet"));
        s.choose(flash);
        QCOMPARE(rows(), QString());  // (a flash's spokes are its outline; it is drawn without tails)
        s.choose(narration);
        QCOMPARE(rows(), QStringLiteral("corner wave dash"));  // (a box without tails)
        s.choose(sfx);
        QCOMPARE(rows(), QString());
        // (their names, for the tests and the screen readers; their words in Japanese)
        QCOMPARE(p.corner->objectName(), QStringLiteral("story_corner"));
        QCOMPARE(p.tail_kind->objectName(), QStringLiteral("story_tail_kind"));
        QCOMPARE(qobject_cast<QLabel*>(form->labelForField(p.corner))->text(), QStringLiteral("角の丸み"));
        QCOMPARE(qobject_cast<QLabel*>(form->labelForField(p.sides))->text(), QStringLiteral("角の数"));
        QCOMPARE(qobject_cast<QLabel*>(form->labelForField(p.wave))->text(), QStringLiteral("線のふるえ"));
        QCOMPARE(qobject_cast<QLabel*>(form->labelForField(p.spike_alt))->text(), QStringLiteral("トゲの長短"));
        QCOMPARE(qobject_cast<QLabel*>(form->labelForField(p.dash))->text(), QStringLiteral("破線の長さ"));
        QCOMPARE(qobject_cast<QLabel*>(form->labelForField(p.tail_kind))->text(), QStringLiteral("しっぽの形"));
        QCOMPARE(qobject_cast<QLabel*>(form->labelForField(p.tail_fillet))->text(), QStringLiteral("しっぽの付け根の丸み"));
        for (QWidget* w : {static_cast<QWidget*>(p.corner), static_cast<QWidget*>(p.squircle), static_cast<QWidget*>(p.sides), static_cast<QWidget*>(p.wave),
                           static_cast<QWidget*>(p.spike_alt), static_cast<QWidget*>(p.dash), static_cast<QWidget*>(p.tail_kind),
                           static_cast<QWidget*>(p.tail_fillet)})
            QVERIFY2(!w->toolTip().isEmpty(), qPrintable(w->objectName()));

        // one setting changed: the op it makes, the key the line holds then, and one undo back to the line before
        const auto one = [&](const std::string& id, const std::function<void()>& change, const Json& style, const char* what) {
            s.choose(id);
            const Json before = snapshot(*s.line(id));
            const std::size_t count = s.ops.size();
            change();
            QVERIFY2(s.ops.size() == count + 1, what);
            QVERIFY2(s.last() == (Json{{"op", "edit_line"}, {"id", id}, {"style", style}}), qPrintable(QString::fromStdString(s.last().dump())));
            for (const auto& [key, value] : style.items()) {
                if (value.is_null()) {
                    QVERIFY2(!s.line(id)->style.contains(key), what);
                } else {
                    QVERIFY2(s.line(id)->style.at(key) == value, what);
                }
            }
            const Json after = snapshot(*s.line(id));
            s.trigger("act_undo");
            QVERIFY2(snapshot(*s.line(id)) == before, what);
            s.trigger("act_redo");
            QVERIFY2(snapshot(*s.line(id)) == after, what);
        };
        const auto spin = [](QDoubleSpinBox* box, double value) {
            return [box, value] {
                box->setValue(value);
                emit box->editingFinished();
            };
        };
        // 角の丸み: 自動 at first; 0 is square corners; 自動 again takes the key away
        s.choose(rounded);
        QCOMPARE(p.corner->text(), QStringLiteral("自動"));
        one(rounded, spin(p.corner, 0.0), Json{{"corner_mm", 0.0}}, "corner 0");
        QCOMPARE(p.corner->value(), 0.0);
        one(rounded, spin(p.corner, 4.5), Json{{"corner_mm", 4.5}}, "corner 4.5");
        one(rounded, spin(p.corner, p.corner->minimum()), Json{{"corner_mm", nullptr}}, "corner auto");
        QCOMPARE(p.corner->text(), QStringLiteral("自動"));
        {  // (left as it was: no edit)
            const std::size_t count = s.ops.size();
            emit p.corner->editingFinished();
            QCOMPARE(s.ops.size(), count);
        }
        // 角をふっくら丸くする
        one(rounded, [&] { p.squircle->click(); }, Json{{"squircle", true}}, "squircle on");
        s.choose(rounded);
        QVERIFY(p.squircle->isChecked());
        QVERIFY(!p.corner->isEnabled());  // (the fuller box has its own corners)
        one(rounded, [&] { p.squircle->click(); }, Json{{"squircle", nullptr}}, "squircle off");
        s.choose(rounded);
        QVERIFY(p.corner->isEnabled());
        // 角の数
        s.choose(polygon);
        QCOMPARE(p.sides->value(), 8);
        one(polygon, [&] {
                p.sides->setValue(6);
                emit p.sides->editingFinished();
            }, Json{{"sides", 6}}, "sides 6");
        one(polygon, [&] {
                p.sides->setValue(8);
                emit p.sides->editingFinished();
            }, Json{{"sides", nullptr}}, "sides 8");
        QCOMPARE(p.sides->minimum(), 5);
        QCOMPARE(p.sides->maximum(), 12);
        // 線のふるえ
        s.choose(speech);
        QCOMPARE(p.wave->text(), QStringLiteral("なし"));
        one(speech, spin(p.wave, 0.4), Json{{"wave_mm", 0.4}}, "wave 0.4");
        one(speech, spin(p.wave, 0.0), Json{{"wave_mm", nullptr}}, "wave 0");
        QCOMPARE(p.wave->maximum(), 3.0);
        // トゲの長短
        s.choose(shout);
        QCOMPARE(p.spike_alt->text(), QStringLiteral("そろえる"));
        one(shout, spin(p.spike_alt, 0.5), Json{{"spike_alt", 0.5}}, "spike_alt 0.5");
        one(shout, spin(p.spike_alt, 0.0), Json{{"spike_alt", nullptr}}, "spike_alt 0");
        QCOMPARE(p.spike_alt->maximum(), 0.8);
        // 破線の長さ: 実線 at first (a whisper: 自動 — it is dashed by itself); the shortest dash is 0.2 mm
        s.choose(speech);
        QCOMPARE(p.dash->text(), QStringLiteral("実線"));
        one(speech, spin(p.dash, 2.5), Json{{"dash_mm", 2.5}}, "dash 2.5");
        one(speech, spin(p.dash, 0.1), Json{{"dash_mm", 0.2}}, "dash 0.1");
        one(speech, spin(p.dash, 0.0), Json{{"dash_mm", nullptr}}, "dash 0");
        s.choose(whisper);
        QCOMPARE(p.dash->text(), QStringLiteral("自動"));
        // しっぽの形 and しっぽの付け根の丸み: every tail of the line, in one edit_line
        s.choose(whisper);
        QVERIFY(!p.tail_kind->isEnabled());  // (it has no tail)
        QVERIFY(!p.tail_fillet->isEnabled());
        s.choose(speech);
        QVERIFY(p.tail_kind->isEnabled());
        QCOMPARE(items(p.tail_kind), QStringList({QStringLiteral("くさび"), QStringLiteral("まっすぐ"), QStringLiteral("ギザギザ"), QStringLiteral("消える"),
                                                  QStringLiteral("泡（心の声）"), QStringLiteral("内向き（切れ込み）")}));
        QCOMPARE(p.tail_kind->currentData().toString(), QStringLiteral("wedge"));
        const auto pick_tail = [&](const char* kind) {
            p.tail_kind->setCurrentIndex(p.tail_kind->findData(QString::fromLatin1(kind)));
            emit p.tail_kind->activated(p.tail_kind->currentIndex());
        };
        {
            const Json before = snapshot(*s.line(speech));
            std::size_t count = s.ops.size();
            pick_tail("zigzag");
            QCOMPARE(s.ops.size(), count + 1);
            QCOMPARE(s.last(), (Json{{"op", "edit_line"}, {"id", speech}, {"tails", Json::array({Json{{"to", Json::array({50.0, 100.0})}, {"kind", "zigzag"}}})}}));
            // 内向き: a notch — its tip moved inside the balloon, on the line from the middle it pointed along
            count = s.ops.size();
            pick_tail("inward");
            QCOMPARE(s.ops.size(), count + 1);
            const Json inward = s.last()["tails"][0];
            QCOMPARE(inward["kind"], Json("inward"));
            const core::StoryLine& l = *s.line(speech);
            const double cx = l.x_mm.value() + l.w_mm.value() / 2, cy = l.y_mm.value() + l.h_mm.value() / 2;
            const double dx = inward["to"][0].get<double>() - cx, dy = inward["to"][1].get<double>() - cy;
            QVERIFY(std::hypot(dx / (l.w_mm.value() / 2), dy / (l.h_mm.value() / 2)) < 0.6);
            QVERIFY(std::fabs(dx * (100.0 - cy) - dy * (50.0 - cx)) < 0.05 * std::hypot(50.0 - cx, 100.0 - cy));  // (the same way)
            QVERIFY(dx * (50.0 - cx) + dy * (100.0 - cy) > 0);
            QVERIFY(core::uses_fukidashi(l));
            QCOMPARE(p.tail_kind->currentData().toString(), QStringLiteral("inward"));
            // …and back out: outside the balloon again
            pick_tail("wedge");
            const Json out = s.last()["tails"][0];
            QVERIFY(!out.contains("kind"));
            const double ox = out["to"][0].get<double>() - cx, oy = out["to"][1].get<double>() - cy;
            QVERIFY(std::hypot(ox / (l.w_mm.value() / 2), oy / (l.h_mm.value() / 2)) > 1.3);
            // the join's rounding: 自動 at first; 0 a corner
            QCOMPARE(p.tail_fillet->text(), QStringLiteral("自動"));
            count = s.ops.size();
            p.tail_fillet->setValue(2.5);
            emit p.tail_fillet->editingFinished();
            QCOMPARE(s.ops.size(), count + 1);
            QCOMPARE(s.last()["op"], Json("edit_line"));
            QCOMPARE(s.last()["tails"][0]["fillet_mm"], Json(2.5));
            QCOMPARE(s.line(speech)->tails.front()["fillet_mm"], Json(2.5));
            emit p.tail_fillet->editingFinished();  // (left as it was)
            QCOMPARE(s.ops.size(), count + 1);
            p.tail_fillet->setValue(0.0);
            emit p.tail_fillet->editingFinished();
            QCOMPARE(s.line(speech)->tails.front()["fillet_mm"], Json(0.0));
            p.tail_fillet->setValue(p.tail_fillet->minimum());
            emit p.tail_fillet->editingFinished();
            QVERIFY(!s.line(speech)->tails.front().contains("fillet_mm"));
            // (each was one step: as many undos as edits give the line back)
            for (int i = 0; i < 6; ++i) s.trigger("act_undo");
            QCOMPARE(snapshot(*s.line(speech)), before);
        }
        // a balloon's own menu offers the same kinds of tail
        {
            const std::unique_ptr<QMenu> menu = s.window->line_menu(speech);
            QMenu* kinds = nullptr;
            for (QAction* a : menu->actions())
                if (a->menu() != nullptr && a->text() == QStringLiteral("しっぽの形")) kinds = a->menu();
            QVERIFY(kinds != nullptr);
            QCOMPARE(texts(kinds), items(p.tail_kind));
        }
        // 既定の設定に戻す takes the new keys away too
        s.choose(rounded);
        p.corner->setValue(3);
        emit p.corner->editingFinished();
        p.wave->setValue(0.3);
        emit p.wave->editingFinished();
        QVERIFY(s.line(rounded)->style.contains("corner_mm") && s.line(rounded)->style.contains("wave_mm"));
        p.reset->click();
        QVERIFY(s.line(rounded)->style.empty());
    }

    // The palette: a dock named フキダシ, a tab beside 台詞, in ウィンドウ and ページ → 台詞, closed until asked for; its
    // presets each with a name, a tooltip and a picture the renderer draws — none drawn before the panel is shown,
    // each once; reached with the keys.
    void thePaletteAndItsPictures() {
        Studio s;
        QDockWidget* dock = s.window->balloon_dock();
        QVERIFY(dock != nullptr);
        QCOMPARE(dock->windowTitle(), QStringLiteral("フキダシ"));
        QCOMPARE(dock->objectName(), QStringLiteral("フキダシ"));
        QCOMPARE(dock->widget(), static_cast<QWidget*>(&s.palette()));
        QVERIFY(!dock->isVisible());                 // (closed until asked for)
        QVERIFY(!s.palette().pictures_drawn());      // (…and nothing drawn for it: opening the window draws no picture)
        QVERIFY(dock->features().testFlag(QDockWidget::DockWidgetClosable));
        // ウィンドウ → フキダシ, and ページ → 台詞 → フキダシのパレットを開く
        QMenu* windows = s.menu_at(QStringLiteral("ウィンドウ"));
        QVERIFY(windows != nullptr);
        QVERIFY(windows->actions().contains(dock->toggleViewAction()));
        QAction* open = s.window->action(QStringLiteral("act_balloon_palette"));
        QVERIFY(open != nullptr);
        QCOMPARE(open->text(), QStringLiteral("フキダシのパレットを開く"));
        QVERIFY(open->shortcuts().isEmpty());
        QVERIFY(s.menu_at(QStringLiteral("ページ/台詞"))->actions().contains(open));
        // the presets: about thirty, each its own key and name, a tooltip, a kind the ops take
        const auto& presets = app::balloon_presets();
        QVERIFY(presets.size() >= 24 && presets.size() <= 34);
        QStringList keys, names;
        for (const app::BalloonPreset& preset : presets) {
            keys << QString::fromLatin1(preset.key);
            names << QString::fromUtf8(preset.name);
            QVERIFY2(QString::fromUtf8(preset.name).size() <= 7, preset.key);
            QVERIFY2(!QString::fromUtf8(preset.tip).isEmpty(), preset.key);
            QVERIFY2(app::lettering::kind_label(preset.kind) != QString::fromLatin1(preset.kind), preset.key);  // (a kind of the menus)
            QVERIFY2(preset.about_tails == !preset.tails.empty(), preset.key);
        }
        QCOMPARE(QSet<QString>(keys.begin(), keys.end()).size(), keys.size());
        QCOMPARE(QSet<QString>(names.begin(), names.end()).size(), names.size());
        for (const char* key : {"oval", "capsule", "capsule_wide", "rounded", "squircle", "box", "narration", "thought_cloud", "thought", "shout", "shout_alt",
                                "electric", "flash", "whisper", "long_dash", "dotted_box", "tone_box", "fancy_box", "double", "wave", "polygon6", "polygon8",
                                "black_oval", "black_shout", "no_line", "thick", "inward_tail", "two_tails"})
            QVERIFY2(app::balloon_preset(QString::fromLatin1(key)) != nullptr, key);
        QListWidget* list = s.palette().list();
        QCOMPARE(list->count(), static_cast<int>(presets.size()));
        QCOMPARE(list->viewMode(), QListView::IconMode);
        QCOMPARE(list->item(0)->text(), QStringLiteral("ふつう"));
        for (int i = 0; i < list->count(); ++i) {
            QVERIFY(list->item(i)->toolTip().startsWith(list->item(i)->text()));
            QVERIFY(list->item(i)->toolTip().size() > list->item(i)->text().size() + 4);
        }
        // shown: the pictures are drawn (a few between events), every one
        open->trigger();
        QVERIFY(dock->isVisible());
        QVERIFY(!dock->isFloating());
        QVERIFY(s.window->tabifiedDockWidgets(dock).contains(s.window->findChild<QDockWidget*>(QStringLiteral("台詞"))));  // (a tab beside the lines)
        QTRY_VERIFY_WITH_TIMEOUT(s.palette().pictures_drawn(), 30000);
        const QSize size = app::preset_picture_size();
        QCOMPARE(list->iconSize(), size);
        std::map<QString, QImage> pictures;
        for (const app::BalloonPreset& preset : presets) {
            const QImage picture = app::preset_picture(preset);
            QCOMPARE(picture.size(), size);
            const bool lineless = QLatin1String(preset.key) == QLatin1String("no_line");
            QVERIFY2(dark_pixels(picture) > (lineless ? 8 : 40), preset.key);        // (something is drawn)
            QVERIFY2(dark_pixels(picture) < size.width() * size.height() * 3 / 4, preset.key);
            // (on white paper; the balloon without a line on grey, where its white shows)
            QCOMPARE(picture.pixel(0, 0), lineless ? qRgb(176, 176, 176) : qRgb(255, 255, 255));
            if (lineless) QCOMPARE(picture.pixel(size.width() / 2, size.height() / 4), qRgb(255, 255, 255));
            QCOMPARE(app::preset_picture(preset).constBits(), picture.constBits());  // (kept: the same picture, not drawn again)
            const QPixmap icon = s.palette().item(QString::fromLatin1(preset.key))->icon().pixmap(size);
            QVERIFY2(dark_pixels(icon.toImage()) > (lineless ? 8 : 40), preset.key);
            pictures[QString::fromLatin1(preset.key)] = picture;
        }
        // (no two presets draw the same picture)
        for (const auto& [a, picture_a] : pictures)
            for (const auto& [b, picture_b] : pictures)
                if (a < b) QVERIFY2(picture_a != picture_b, qPrintable(a + QStringLiteral(" = ") + b));
        QVERIFY(pictures[QStringLiteral("oval")] != pictures[QStringLiteral("shout")]);
        // (ベタ: mostly black inside; 線なし: only its letters)
        QVERIFY(dark_pixels(pictures[QStringLiteral("black_oval")]) > 4 * dark_pixels(pictures[QStringLiteral("oval")]));
        QVERIFY(dark_pixels(pictures[QStringLiteral("no_line")]) < dark_pixels(pictures[QStringLiteral("oval")]));
        QVERIFY(dark_pixels(pictures[QStringLiteral("thick")]) > dark_pixels(pictures[QStringLiteral("oval")]));
        // on a dense screen the picture is drawn again at its scale, not stretched
        const QImage dense = app::preset_picture(*app::balloon_preset(QStringLiteral("oval")), 2.0);
        QCOMPARE(dense.size(), size * 2);
        QCOMPARE(dense.devicePixelRatio(), 2.0);
        {  // (shown, not judged: how long every picture takes to draw afresh — at a scale not drawn yet)
            QElapsedTimer clock;
            clock.start();
            for (const app::BalloonPreset& preset : presets) (void)app::preset_picture(preset, 1.5);
            qInfo("the palette's %d pictures drawn afresh in %lld ms", static_cast<int>(presets.size()), static_cast<long long>(clock.elapsed()));
        }
        // the keys: the arrows move among the presets, Enter and Space pick the one in hand
        QSignalSpy picked(&s.palette(), &app::BalloonPalette::picked);
        list->setFocus();
        list->setCurrentRow(0);
        QTest::keyClick(list, Qt::Key_Right);
        QCOMPARE(list->currentRow(), 1);
        QTest::keyClick(list, Qt::Key_Return);
        QCOMPARE(picked.size(), 1);
        QCOMPARE(picked.at(0).at(0).toString(), keys.at(1));
        QTest::keyClick(list, Qt::Key_Space);
        QCOMPARE(picked.size(), 2);
        // a click picks
        const QRect at = list->visualItemRect(list->item(2));
        QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, at.center());
        QCOMPARE(picked.size(), 3);
        QCOMPARE(picked.at(2).at(0).toString(), keys.at(2));
        // the 写植 stage brings the palette with the lines
        dock->hide();
        s.window->stage_actions().at(QStringLiteral("letter"))->trigger();
        QVERIFY(!dock->isHidden());
        // its place goes with the window's layout, as every dock's: open or closed as it was kept
        const QByteArray open_layout = s.window->saveState();
        dock->hide();
        const QByteArray closed_layout = s.window->saveState();
        QVERIFY(s.window->restoreState(open_layout));
        QVERIFY(!dock->isHidden());
        QVERIFY(s.window->restoreState(closed_layout));
        QVERIFY(dock->isHidden());
    }

    // A pick with a line chosen: that line takes the look — its kind and its look's keys, the keys of the look it had
    // gone —, its words, place, size and tails kept; one edit_line, and one 取り消し gives the line back exactly.
    void aPickRestylesTheChosenLine() {
        Studio s;
        const Json tails = Json::array({Json{{"to", Json::array({48.0, 104.0})}, {"via", Json::array({52.0, 96.0})}, {"width_mm", 5.0}}});
        const std::string id = s.put(Json{{"op", "add_line"}, {"text", "なんだ\nって！"}, {"balloon", "shout"}, {"x_mm", 60}, {"y_mm", 60}, {"w_mm", 34}, {"h_mm", 28},
                                          {"tails", tails},
                                          {"style", Json{{"spikes", 16}, {"spike_depth", 0.3}, {"spike_alt", 0.5}, {"border_mm", 0.6}, {"font", "maru"},
                                                         {"size_mm", 4.5}, {"text_dx_mm", 1.0}, {"rotate_deg", 5.0}, {"rgb", Json::array({200, 30, 30})}}}});
        QVERIFY(!id.empty());
        s.choose(id);
        const Json before = snapshot(*s.line(id));
        std::size_t count = s.ops.size();
        s.window->use_balloon_preset(QStringLiteral("polygon6"));
        QCOMPARE(s.ops.size(), count + 1);  // (one op: one step)
        QCOMPARE(s.last(), (Json{{"op", "edit_line"}, {"id", id}, {"balloon", "polygon"},
                                 {"style", Json{{"border_mm", nullptr}, {"spikes", nullptr}, {"spike_depth", nullptr}, {"spike_alt", nullptr}, {"sides", 6}}}}));
        {
            const core::StoryLine& l = *s.line(id);
            QCOMPARE(l.balloon, std::string("polygon"));
            // (the look's keys of before gone, the preset's set; the letters' own and the balloon's turn kept)
            QCOMPARE(l.style, (Json{{"font", "maru"}, {"size_mm", 4.5}, {"text_dx_mm", 1.0}, {"rotate_deg", 5.0}, {"rgb", Json::array({200, 30, 30})}, {"sides", 6}}));
            Json after = snapshot(l);
            for (const char* same : {"id", "text", "wrap", "x", "y", "w", "h", "tails", "tail"}) QVERIFY2(after[same] == before[same], same);
        }
        QCOMPARE(s.canvas()->selected_line_id, std::optional<std::string>(id));  // (still the chosen line)
        QCOMPARE(s.canvas()->tool(), QStringLiteral("select"));
        QVERIFY(s.window->armed_balloon_preset() == nullptr);                    // (the next line's look is not changed by it)
        s.trigger("act_undo");
        QCOMPARE(snapshot(*s.line(id)), before);
        s.trigger("act_redo");
        QCOMPARE(s.line(id)->balloon, std::string("polygon"));
        // a click on the palette's item does the same
        s.window->show_dock(QStringLiteral("フキダシ"));
        s.palette().draw_pictures();
        count = s.ops.size();
        QListWidget* list = s.palette().list();
        QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, list->visualItemRect(s.palette().item(QStringLiteral("wave"))).center());
        QCOMPARE(s.ops.size(), count + 1);
        QCOMPARE(s.line(id)->balloon, std::string("speech"));
        QCOMPARE(s.line(id)->style["wave_mm"], Json(0.4));
        QVERIFY(!s.line(id)->style.contains("sides"));
        // ベタ白抜き: black inside, white letters; then a white balloon again: the white letters go with the black
        s.window->use_balloon_preset(QStringLiteral("black_oval"));
        QCOMPARE(s.line(id)->style["fill_rgb"], Json::array({0, 0, 0}));
        QCOMPARE(s.line(id)->style["rgb"], Json::array({255, 255, 255}));
        QVERIFY(!s.line(id)->style.contains("wave_mm"));
        s.window->use_balloon_preset(QStringLiteral("oval"));
        QVERIFY(!s.line(id)->style.contains("fill_rgb"));
        QVERIFY(!s.line(id)->style.contains("line_rgb"));
        QVERIFY(!s.line(id)->style.contains("rgb"));
        QCOMPARE(s.line(id)->style, (Json{{"font", "maru"}, {"size_mm", 4.5}, {"text_dx_mm", 1.0}, {"rotate_deg", 5.0}}));
        QCOMPARE(PageCanvas_tails(*s.line(id)), tails);  // (through all of it, its tail as it was)
        // a preset about its tails: the line's own tail keeps where it points and takes the kind (心の声の泡)…
        s.window->use_balloon_preset(QStringLiteral("thought_cloud"));
        QCOMPARE(s.line(id)->balloon, std::string("cloud"));
        {
            Json bubbles = tails;
            bubbles[0]["kind"] = "bubbles";
            QCOMPARE(PageCanvas_tails(*s.line(id)), bubbles);
        }
        // …しっぽ 2 本 gives its two; 内向きしっぽ its notch, inside the balloon; しっぽつき one tail out again
        s.window->use_balloon_preset(QStringLiteral("two_tails"));
        QCOMPARE(s.line(id)->tails.size(), std::size_t{2});
        s.window->use_balloon_preset(QStringLiteral("inward_tail"));
        QCOMPARE(s.line(id)->tails.size(), std::size_t{1});
        QCOMPARE(s.line(id)->tails.front()["kind"], Json("inward"));
        {
            const core::StoryLine& l = *s.line(id);
            const double tx = l.tails.front()["to"][0].get<double>(), ty = l.tails.front()["to"][1].get<double>();
            QVERIFY(tx > l.x_mm.value() && tx < l.x_mm.value() + l.w_mm.value() && ty > l.y_mm.value() && ty < l.y_mm.value() + l.h_mm.value());
        }
        s.window->use_balloon_preset(QStringLiteral("oval_tail"));
        QCOMPARE(s.line(id)->tails.size(), std::size_t{1});
        QVERIFY(!s.line(id)->tails.front().contains("kind"));
        QVERIFY(s.line(id)->tails.front()["to"][1].get<double>() > s.line(id)->y_mm.value() + s.line(id)->h_mm.value());
        // every preset can be given to a line: the ops take each (no refusal), and undo goes back each time
        for (const app::BalloonPreset& preset : app::balloon_presets()) {
            const Json was = snapshot(*s.line(id));
            count = s.ops.size();
            QVERIFY2(s.window->restyle_with_balloon_preset(QString::fromLatin1(preset.key)), preset.key);
            QVERIFY2(s.ops.size() == count + 1, preset.key);
            QVERIFY2(s.line(id)->balloon == preset.kind, preset.key);
            for (const auto& [key, value] : preset.style.items()) QVERIFY2(s.line(id)->style.at(key) == value, preset.key);
            QVERIFY2(snapshot(*s.line(id))["text"] == was["text"] && snapshot(*s.line(id))["w"] == was["w"] && snapshot(*s.line(id))["x"] == was["x"], preset.key);
            s.trigger("act_undo");
            QVERIFY2(snapshot(*s.line(id)) == was, preset.key);
        }
        // a line of before with a wobble: a look of now takes the trembling away with the look it was part of
        const std::string old = s.put(Json{{"op", "add_line"}, {"text", "むかし"}, {"balloon", "speech"}, {"x_mm", 110}, {"y_mm", 60}, {"w_mm", 30}, {"h_mm", 24},
                                           {"style", Json{{"wobble", 0.5}, {"hand", false}}}});
        s.choose(old);
        s.window->use_balloon_preset(QStringLiteral("rounded"));
        QCOMPARE(s.line(old)->style, (Json{{"hand", false}}));
        // no line chosen and asked for the chosen line: said, nothing changed
        s.choose_none();
        count = s.ops.size();
        QVERIFY(!s.window->restyle_with_balloon_preset(QStringLiteral("oval")));
        QCOMPARE(s.ops.size(), count);
    }

    // A pick with no line chosen (or the text tool in hand): the look of the next line placed — the text tool taken,
    // its settings saying the kind, the preset marked on the palette; the line typed on the page carries the kind, the
    // look's keys and the tails, in a box measured for that shape. The text tool's own フキダシ chosen lets it go.
    void aPickIsTheLookOfTheNextLine() {
        Studio s;
        s.trigger("act_select");
        QVERIFY(!s.canvas()->selected_line_id);
        QVERIFY(s.window->armed_balloon_preset() == nullptr);
        s.window->use_balloon_preset(QStringLiteral("capsule"));
        QVERIFY(s.ops.empty());  // (nothing changes until a line is placed)
        QCOMPARE(s.canvas()->tool(), QStringLiteral("text"));
        QVERIFY(s.window->armed_balloon_preset() != nullptr);
        QCOMPARE(QString::fromLatin1(s.window->armed_balloon_preset()->key), QStringLiteral("capsule"));
        QCOMPARE(s.palette().marked(), QStringLiteral("capsule"));
        QCOMPARE(s.palette().list()->currentItem(), s.palette().item(QStringLiteral("capsule")));
        QVERIFY(s.palette().item(QStringLiteral("capsule"))->font().bold());   // (marked: the list's highlight, heavier letters)
        QCOMPARE(s.palette().list()->selectedItems(), QList<QListWidgetItem*>{s.palette().item(QStringLiteral("capsule"))});
        QVERIFY(!s.palette().item(QStringLiteral("oval"))->font().bold());
        QCOMPARE(s.window->text_settings()->balloon->currentData().toString(), QStringLiteral("capsule"));
        // typed where the text tool clicks: a capsule, its box the auto-lettering's for those words
        Json placed = s.type_at(QPointF(100.0, 150.0), QStringLiteral("そうなんだ"));
        QCOMPARE(placed["op"], Json("add_line"));
        QCOMPARE(placed["balloon"], Json("capsule"));
        QVERIFY(!placed.contains("style"));
        QVERIFY(!placed.contains("tails"));
        const std::array<double, 2> capsule = service::letter::measure({"そうなんだ"}, "capsule");
        QCOMPARE(placed["w_mm"].get<double>(), core::py_round(capsule[0], 2));
        QCOMPARE(placed["h_mm"].get<double>(), core::py_round(capsule[1], 2));
        QCOMPARE(placed["wrap"], Json("vertical"));
        const std::string first = s.window->book().story.back().id;
        QCOMPARE(s.line(first)->balloon, std::string("capsule"));
        QVERIFY(core::uses_fukidashi(*s.line(first)));
        // (placed, it is the chosen line and the select tool is in hand — and the look stays the next line's)
        QCOMPARE(s.canvas()->tool(), QStringLiteral("select"));
        QCOMPARE(QString::fromLatin1(s.window->armed_balloon_preset()->key), QStringLiteral("capsule"));
        s.trigger("act_text");
        placed = s.type_at(QPointF(60.0, 80.0), QStringLiteral("うん"));
        QCOMPARE(placed["balloon"], Json("capsule"));
        // the text tool in hand: a pick is the next line's look, whatever line is chosen
        s.trigger("act_text");
        QVERIFY(s.canvas()->selected_line_id.has_value());
        std::size_t count = s.ops.size();
        s.window->use_balloon_preset(QStringLiteral("shout_alt"));
        QCOMPARE(s.ops.size(), count);
        QCOMPARE(s.palette().marked(), QStringLiteral("shout_alt"));
        placed = s.type_at(QPointF(140.0, 100.0), QStringLiteral("なんだって"));
        QCOMPARE(placed["balloon"], Json("shout"));
        QCOMPARE(placed["style"], (Json{{"spike_alt", 0.5}}));
        // the text tool's own face and size go with the preset's look
        s.window->text_settings()->font->setCurrentIndex(s.window->text_settings()->font->findData(QStringLiteral("maru")));
        s.trigger("act_text");
        s.window->use_balloon_preset(QStringLiteral("polygon6"));
        placed = s.type_at(QPointF(60.0, 200.0), QStringLiteral("ジュシン\nカンリョウ"));
        QCOMPARE(placed["balloon"], Json("polygon"));
        QCOMPARE(placed["style"], (Json{{"font", "maru"}, {"sides", 6}}));
        {  // (its box: the six-sided polygon's — 1.5 times the words' block —, the letters at their full size in it)
            const std::array<double, 2> six = service::letter::measure({"ジュシン", "カンリョウ"}, "polygon", service::letter::kEmMm, 6);
            QCOMPARE(placed["w_mm"].get<double>(), core::py_round(six[0], 2));
            QCOMPARE(placed["h_mm"].get<double>(), core::py_round(six[1], 2));
        }
        s.window->text_settings()->font->setCurrentIndex(0);
        // a preset about its tails places them with the line: しっぽ 2 本 below the box, 内向き inside it
        s.trigger("act_text");
        s.window->use_balloon_preset(QStringLiteral("two_tails"));
        placed = s.type_at(QPointF(150.0, 200.0), QStringLiteral("せーの"));
        QCOMPARE(placed["balloon"], Json("speech"));
        QCOMPARE(placed["tails"].size(), std::size_t{2});
        for (const Json& t : placed["tails"]) QVERIFY(t["to"][1].get<double>() > placed["y_mm"].get<double>() + placed["h_mm"].get<double>());
        QVERIFY(placed["tails"][0]["to"][0].get<double>() < placed["tails"][1]["to"][0].get<double>());
        QCOMPARE(s.window->book().story.back().tails.size(), std::size_t{2});
        // 横長: its words run across (the 横書き文字ツール taken)
        s.trigger("act_select");
        s.choose_none();
        s.window->use_balloon_preset(QStringLiteral("capsule_wide"));
        QCOMPARE(s.canvas()->tool(), QStringLiteral("text_h"));
        placed = s.type_at(QPointF(100.0, 60.0), QStringLiteral("ひとこと"));
        QCOMPARE(placed["balloon"], Json("capsule"));
        QCOMPARE(placed["wrap"], Json("horizontal"));
        QVERIFY(placed["w_mm"].get<double>() > placed["h_mm"].get<double>());
        // with the select tool and a line of this page chosen, the plain pick restyles; the item's own menu's 「次に置く
        // 台詞をこの形にする」 ("next") is the next line's look all the same
        const std::string chosen = s.window->book().story.back().id;
        s.choose(chosen);
        count = s.ops.size();
        s.window->use_balloon_preset(QStringLiteral("double"), QStringLiteral("next"));
        QCOMPARE(s.ops.size(), count);
        QCOMPARE(s.palette().marked(), QStringLiteral("double"));
        s.choose(chosen);
        s.window->use_balloon_preset(QStringLiteral("thick"));  // (restyles: the mark stays on the next line's look)
        QCOMPARE(s.ops.size(), count + 1);
        QCOMPARE(s.line(chosen)->style["border_mm"], Json(0.8));
        QCOMPARE(s.palette().marked(), QStringLiteral("double"));
        QCOMPARE(s.palette().list()->selectedItems(), QList<QListWidgetItem*>{s.palette().item(QStringLiteral("double"))});
        // the text tool's own フキダシ chosen: the palette's look is let go, the kind chosen placed plain
        auto* settings = s.window->text_settings();
        settings->balloon->setCurrentIndex(settings->balloon->findData(QStringLiteral("rounded")));
        emit settings->balloon->activated(settings->balloon->currentIndex());
        QVERIFY(s.window->armed_balloon_preset() == nullptr);
        QCOMPARE(s.palette().marked(), QString());
        QVERIFY(s.palette().list()->selectedItems().isEmpty());
        for (int i = 0; i < s.palette().list()->count(); ++i) QVERIFY(!s.palette().list()->item(i)->font().bold());
        s.trigger("act_text");
        placed = s.type_at(QPointF(100.0, 240.0), QStringLiteral("ふつう"));
        QCOMPARE(placed["balloon"], Json("rounded"));
        QVERIFY(!placed.contains("style"));
        // every line placed is drawn (none refused), and the book saves and reads back with its feature named
        s.session->save_now();
        QVERIFY(s.session->wait_saved(std::chrono::milliseconds(20000)));
        const core::Document back = gui_test::read_book(gui_test::path_of(s.tmp.path() + "/book"));
        QVERIFY(back.read_only_reason.empty());
        QCOMPARE(back.story.size(), s.window->book().story.size());
        QVERIFY(core::uses_fukidashi(back));
    }

    // The box the window gives a capsule and a polygon is the auto-lettering's (service::letter::measure): the one
    // formula; and the letters set in the box so made keep their full size.
    void theWindowMeasuresACapsuleAsTheAutoLettering() {
        const std::vector<std::vector<std::string>> samples = {{"そうなんだ"}, {"うん"}, {"あいうえお", "かきくけこ"}, {"ねえ", "聞いて", "るの"}};
        for (const auto& words : samples) {
            std::vector<std::u32string> breaks;
            for (const std::string& w : words) breaks.push_back(render::text::u32(w));
            for (const char* kind : {"capsule", "polygon"}) {
                const std::array<double, 2> want = service::letter::measure(words, kind);
                const std::pair<double, double> got = app::lettering::measure(breaks, kind);
                QCOMPARE(got.first, want[0]);
                QCOMPARE(got.second, want[1]);
            }
            const std::array<double, 2> six = service::letter::measure(words, "polygon", service::letter::kEmMm, 6);
            QCOMPARE(app::lettering::measure(breaks, "polygon", app::lettering::kEmMm, 6).first, six[0]);
            QVERIFY(six[0] > service::letter::measure(words, "polygon")[0]);  // (six sides cut further in than eight)
        }
        // (a one-column capsule: as wide as its round ends, taller than its words)
        const std::array<double, 2> capsule = service::letter::measure({"そうなんだ"}, "capsule");
        QVERIFY(capsule[0] < 10 && capsule[1] > 25 && capsule[1] < 36);
        QCOMPARE(service::letter::polygon_room(8), service::letter::polygon_room(0));
        QVERIFY(std::fabs(service::letter::polygon_room(8) - std::sqrt(2.0)) < 1e-3);
        QVERIFY(std::fabs(service::letter::polygon_room(6) - 1.5) < 1e-3);
        for (int sides = 5; sides <= 12; ++sides) {
            const double room = service::letter::polygon_room(sides);
            QVERIFY2(room >= 1.0 && room < 1.8, qPrintable(QStringLiteral("%1 sides: %2").arg(sides).arg(room)));
        }
        // typed in the window (place_at) and set by the renderer: the letters at their full 5 mm (118 px at 600 dpi)
        struct Case {
            const char* kind;
            Json style;
            const char* text;
            bool vertical;
        };
        const Case cases[] = {{"capsule", Json::object(), "そうなんだ", true},        {"capsule", Json::object(), "ひとこと", false},
                              {"capsule", Json::object(), "あいうえお\nかきくけこ", true}, {"polygon", Json::object(), "キカイ\nノコエ", true},
                              {"polygon", Json{{"sides", 6}}, "ロッカク\nノコエ", true},   {"polygon", Json{{"sides", 5}}, "ゴカク", true},
                              {"polygon", Json{{"sides", 7}}, "ナナカク\nデス", true},     {"polygon", Json{{"sides", 9}}, "キュウ\nカク", true},
                              {"polygon", Json{{"sides", 10}}, "ジュッカク", true},        {"polygon", Json{{"sides", 11}}, "ジュウ\nイチ", true},
                              {"polygon", Json{{"sides", 12}}, "ジュウニ\nカク", true},    {"polygon", Json::object(), "よこがき", false}};
        for (const Case& c : cases) {
            const Json box = app::lettering::place_at(100, 100, c.text, c.kind, c.vertical, nullptr, c.style);
            core::StoryLine line;
            line.id = "m";
            line.text = c.text;
            line.balloon = c.kind;
            line.wrap = c.vertical ? "vertical" : "horizontal";
            line.x_mm = core::Num(box["x_mm"].get<double>());
            line.y_mm = core::Num(box["y_mm"].get<double>());
            line.w_mm = core::Num(box["w_mm"].get<double>());
            line.h_mm = core::Num(box["h_mm"].get<double>());
            line.style = c.style;
            const render::text::Layout layout = render::text::text_layout(line, 600);
            QVERIFY2(layout.em >= 118, qPrintable(QStringLiteral("%1 %2 「%3」: em %4 in %5 × %6 mm")
                                                      .arg(QString::fromLatin1(c.kind), QString::fromStdString(c.style.dump()), QString::fromUtf8(c.text))
                                                      .arg(layout.em)
                                                      .arg(box["w_mm"].get<double>())
                                                      .arg(box["h_mm"].get<double>())));
        }
        // (a line typed over keeps its polygon's sides in its new box: refit reads the line's style)
        core::StoryLine six;
        six.balloon = "polygon";
        six.wrap = "vertical";
        six.style = Json{{"sides", 6}};
        six.x_mm = core::Num(50);
        six.w_mm = core::Num(20);
        const Json refit = app::lettering::refit(six, nullptr, "ロッカク", "polygon", true);
        QCOMPARE(refit["w_mm"].get<double>(), core::py_round(service::letter::measure({"ロッカク"}, "polygon", service::letter::kEmMm, 6)[0], 2));
    }

    // --- the pictures for the lead (written when asked: GENKO_FUKIDASHI_OUT) ---------------------------------------------
    void screenshots() {
        const QByteArray out = qgetenv("GENKO_FUKIDASHI_OUT");
        if (out.isEmpty()) QSKIP("the screenshots are written when GENKO_FUKIDASHI_OUT names a folder");
        const QString dir = QString::fromUtf8(out);
        QDir().mkpath(dir);
        for (const char* mode : {"light", "dark"}) {
            Studio s;
            app::settings()->setValue(QStringLiteral("ui/theme"), QString::fromLatin1(mode));  // (the Studio's own settings)
            app::theme::apply(qApp);
            const std::string id = s.put(Json{{"op", "add_line"}, {"text", "キカイ\nノコエ"}, {"balloon", "polygon"}, {"x_mm", 60}, {"y_mm", 60}, {"w_mm", 32}, {"h_mm", 28},
                                              {"tails", Json::array({Json{{"to", Json::array({52, 104})}}})}});
            s.put(Json{{"op", "add_line"}, {"text", "なんだ\nって！"}, {"balloon", "shout"}, {"x_mm", 110}, {"y_mm", 60}, {"w_mm", 34}, {"h_mm", 30},
                       {"style", Json{{"spike_alt", 0.5}}}});
            s.put(Json{{"op", "add_line"}, {"text", "そうなんだ"}, {"balloon", "capsule"}, {"x_mm", 150}, {"y_mm", 110}, {"w_mm", 9.5}, {"h_mm", 31}});
            s.window->show_dock(QStringLiteral("フキダシ"));
            s.window->balloon_dock()->setFloating(true);
            s.window->balloon_dock()->resize(440, 920);  // (every preset in sight: four across)
            QTRY_VERIFY_WITH_TIMEOUT(s.palette().pictures_drawn(), 30000);
            s.window->arm_balloon_preset(QStringLiteral("capsule"));
            QTest::qWait(200);
            const QString suffix = QLatin1String(mode) == QLatin1String("light") ? QString() : QStringLiteral("-dark");
            QVERIFY(s.window->balloon_dock()->grab().save(dir + QStringLiteral("/gui-palette%1.png").arg(suffix)));
            // the 台詞 panel's settings of the chosen line (beside the select tool), a polygon with a tail chosen
            s.window->arm_balloon_preset(QString());
            s.choose(id);
            QTest::qWait(200);
            QWidget* box = s.panel().style_box;
            QScrollArea* scroll = nullptr;
            for (QWidget* up = box->parentWidget(); up != nullptr && scroll == nullptr; up = up->parentWidget()) scroll = qobject_cast<QScrollArea*>(up);
            QDockWidget* options = nullptr;
            for (QWidget* up = box->parentWidget(); up != nullptr && options == nullptr; up = up->parentWidget()) options = qobject_cast<QDockWidget*>(up);
            if (options != nullptr) {
                options->setFloating(true);
                options->resize(340, 1100);
                QTest::qWait(200);
                QVERIFY(options->grab().save(dir + QStringLiteral("/gui-panel%1.png").arg(suffix)));
            } else {
                QVERIFY(box->grab().save(dir + QStringLiteral("/gui-panel%1.png").arg(suffix)));
            }
            if (QLatin1String(mode) == QLatin1String("light")) QVERIFY(s.window->grab().save(dir + QStringLiteral("/gui-window.png")));
            app::settings()->remove(QStringLiteral("ui/theme"));
        }
        app::theme::apply(qApp);
    }

private:
    static Json PageCanvas_tails(const core::StoryLine& l) { return app::PageCanvas::tails_of(l); }
};

QTEST_MAIN(TestGuiFukidashi)
#include "test_gui_fukidashi.moc"
