// フキダシ in the window (fukidashi@1, SPEC BALLOON-01 — the user's 「今風の漫画の吹き出しを各種作って置けるように」「手描き風は
// 消して良いです」): the kinds 縦長（カプセル） and 多角形（機械の声） in every menu of kinds; no control of the hand-drawn look
// left, a line of before that holds style.hand and style.wobble still edited; the 台詞 panel's new settings of a
// balloon's shape, each the one key of one edit_line and one step of 取り消し, shown only for the kinds they change; the
// palette of balloons — its pictures drawn by the renderer, only once it is shown; a pick gives the chosen line the
// look (取り消し gives the line back exactly) or is the look of the next line the text tool places; and the box of a
// capsule and of a polygon typed in the window is the one the auto-lettering measures (service::letter::measure).

#include <QRegularExpression>
#include <QAbstractSpinBox>
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
#include <set>

#include "app/ask.hpp"
#include "app/balloon_palette.hpp"
#include "app/canvas.hpp"
#include "app/config.hpp"
#include "app/ime.hpp"
#include "app/inject.hpp"
#include "app/lettering.hpp"
#include "app/main_window.hpp"
#include "app/preferences.hpp"
#include "app/story_editor.hpp"
#include "app/story_panel.hpp"
#include "app/theme.hpp"
#include "app/tool_settings.hpp"
#include "core/balloon_tails.hpp"
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

// A value with its keys in order of their names: two styles that hold the same, whatever order they were written in.
std::string canon(const Json& value) { return nlohmann::json::parse(value.dump()).dump(); }

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
        QCOMPARE(labels.size(), 21);  // (fukidashi@2: the four symbols with the 17 of before)
        QCOMPARE(labels.mid(0, 5), QStringList({QStringLiteral("普通（楕円）"), kCapsule, QStringLiteral("角丸"), QStringLiteral("四角"), kPolygon}));
        // (two kinds said as they are drawn — dashes, lines spreading out; keys chosen under their names of before stay)
        QVERIFY(labels.contains(QStringLiteral("ささやき（破線）")) && labels.contains(QStringLiteral("フラッシュ（放射状の線）")));
        QCOMPARE(app::preferences::old_names(QStringLiteral("ささやき（破線）")), QStringList{QStringLiteral("ささやき（点線）")});
        QCOMPARE(app::preferences::old_names(QStringLiteral("フラッシュ（放射状の線）")), QStringList{QStringLiteral("フラッシュ（放射線）")});
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
            // (a kind that holds its letters as an oval does: the kind alone, the box as it was)
            for (QAction* a : own->actions())
                if (a->text() == QStringLiteral("雲（もくもく）")) a->trigger();
            QCOMPARE(s.ops.size(), std::size_t{2});
            QCOMPARE(s.last(), (Json{{"op", "edit_line"}, {"id", id}, {"balloon", "cloud"}}));
            // to a polygon, which holds its letters in its own shape: the kind, and in the same step the box fitted to
            // the words about its middle — an eight-sided box for one column of five (6.5 × 26.5 mm of words, √2 times)
            const Json before = snapshot(*s.line(id));
            for (QAction* a : own->actions())
                if (a->text() == kPolygon) a->trigger();
            QCOMPARE(s.ops.size(), std::size_t{4});
            QCOMPARE(s.ops[2], (Json{{"op", "edit_line"}, {"id", id}, {"balloon", "polygon"}}));
            QCOMPARE(s.ops[3]["op"], Json("move_line"));
            QCOMPARE(s.line(id)->balloon, std::string("polygon"));
            QVERIFY(std::fabs(s.line(id)->w_mm.value() - 9.19) <= 0.011);
            QVERIFY(std::fabs(s.line(id)->h_mm.value() - 37.48) <= 0.011);
            QVERIFY(std::fabs(s.line(id)->x_mm.value() + s.line(id)->w_mm.value() / 2 - 75.0) <= 0.011);  // (its middle where it was)
            QVERIFY(std::fabs(s.line(id)->y_mm.value() + s.line(id)->h_mm.value() / 2 - 72.0) <= 0.011);
            QCOMPARE(render::text::text_layout(*s.line(id), 600).em, std::int64_t{118});
            s.trigger("act_undo");  // (one step: the kind and the box together)
            QCOMPARE(snapshot(*s.line(id)), before);
            s.trigger("act_redo");
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
        const std::size_t count = s.ops.size();
        for (QAction* a : shapes->actions())
            if (a->text() == kCapsule) a->trigger();
        QCOMPARE(s.ops.size(), count + 2);
        QCOMPARE(s.ops[count], (Json{{"op", "edit_line"}, {"id", id}, {"balloon", "capsule"}}));
        QCOMPARE(s.ops[count + 1]["op"], Json("move_line"));
        // (a capsule for one column of five: 9.5 mm wide, its round ends 4.07 mm past the 25 mm of letters — not the
        // polygon's box with round ends)
        QCOMPARE(s.line(id)->w_mm.value(), 9.5);
        QCOMPARE(s.line(id)->h_mm.value(), 29.07);
        QVERIFY(std::fabs(s.line(id)->x_mm.value() + 4.75 - 75.0) <= 0.011);
        QTRY_VERIFY(s.panel().list->item(0)->text().contains(QStringLiteral("〔縦長〕")));  // (the panel follows an edit a moment later)
        // …and from it to an oval again: the oval's box for those words, not a pill's
        for (QAction* a : shapes->actions())
            if (a->text() == QStringLiteral("普通（楕円）")) a->trigger();
        QCOMPARE(s.line(id)->balloon, std::string("speech"));
        QVERIFY(s.line(id)->w_mm.value() != 9.5 && s.line(id)->h_mm.value() > 33.0);  // (an oval round one column of five: taller than the capsule)
        QCOMPARE(render::text::text_layout(*s.line(id), 600).em, std::int64_t{118});
    }

    // The hand-drawn look has no control left (the 台詞 panel's 線の揺れ is gone, nothing offers 手描き風 for a balloon); a
    // line of before that holds style.hand and style.wobble opens, is listed, and is edited without losing them — and
    // the trembling its wobble is drawn as can be set from 線のふるえ, its letters keeping the oval they were fitted in.
    void noControlOfTheHandDrawnLookIsLeft() {
        // lines of before, in an oval where their letters are set one way as a line with a wobble was fitted (in the true
        // ellipse of its box) and another way as an oval without one is (in the letters' oval, a little inside): taking
        // the wobble away without saying hand: false would set them differently
        // (how a line's letters are set: their size and the picture of them — its columns)
        const auto set_of = [](const core::StoryLine& line) {
            const render::text::Layout layout = render::text::text_layout(line, 600);
            return std::array<std::int64_t, 3>{layout.em, layout.image.width(), layout.image.height()};
        };
        core::StoryLine old;
        old.text = "むかしの台\nむかしの詞";
        old.balloon = "speech";
        old.wrap = "vertical";
        old.x_mm = core::Num(40);
        old.y_mm = core::Num(60);
        std::array<std::int64_t, 3> as_set{};
        {
            const std::array<double, 2> roomy = service::letter::measure({"むかしの台", "むかしの詞"}, "speech");
            bool found = false;
            for (int percent = 110; percent >= 70 && !found; --percent) {
                for (const double wide : {1.0, 1.3, 0.85}) {
                    old.w_mm = core::Num(core::py_round(roomy[0] * wide * percent / 100.0, 2));
                    old.h_mm = core::Num(core::py_round(roomy[1] * percent / 100.0, 2));
                    core::StoryLine with = old, without = old;
                    with.style = Json{{"wobble", 0.5}};
                    as_set = set_of(with);
                    found = as_set[0] == 118 && set_of(without) != as_set;
                    if (found) break;
                }
            }
            QVERIFY2(found, "no box tells the two fits apart");
        }
        Studio s([&old](core::Document& doc) {
            const struct {
                const char* id;
                double x;
                Json style;
            } lines[] = {{"old1", 30, Json{{"hand", false}, {"wobble", 0.5}}}, {"old2", 60, Json{{"hand", true}, {"wobble", 0.4}}},
                         {"old3", 90, Json{{"wobble", 0.6}}},                   {"old4", 120, Json{{"wobble", 0.5}}},
                         {"old5", 150, Json{{"hand", true}, {"wobble", 0.5}}}};
            for (const auto& each : lines) {
                core::StoryLine line = old;
                line.id = each.id;
                line.page_index = doc.page(0).index;
                line.x_mm = core::Num(each.x);
                line.frame_id = doc.page(0).leaf_frames().front()->id;
                line.style = each.style;
                doc.story.push_back(line);
            }
        });
        QVERIFY(s.window->book().read_only_reason.empty());
        QVERIFY(s.line("old1") != nullptr);
        QCOMPARE(s.line("old1")->style, (Json{{"hand", false}, {"wobble", 0.5}}));
        for (const char* id : {"old1", "old2", "old3", "old4", "old5"}) QCOMPARE(set_of(*s.line(id)), as_set);
        // no control named for the wobble; no words of the hand-drawn look on the panel, the text tool's settings or the
        // palette (its items' names and tooltips too)
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
        QVERIFY(s.palette().list()->count() > 0);
        for (int i = 0; i < s.palette().list()->count(); ++i) {
            const QString words = s.palette().list()->item(i)->text() + s.palette().list()->item(i)->toolTip();
            for (const QString& word : gone) QVERIFY2(!words.contains(word), qPrintable(words));
        }
        for (const app::BalloonPreset& preset : app::balloon_presets()) {
            const QString words = QString::fromUtf8(preset.name) + QString::fromUtf8(preset.tip);
            for (const QString& word : gone) QVERIFY2(!words.contains(word), qPrintable(words));
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
        // its trembling shown as it is drawn (0.03 × 0.5 × the box's narrow side), and set: the wobble gives way to the
        // amount said
        s.choose("old1");
        const double narrow = std::min(old.w_mm.value(), old.h_mm.value());
        QCOMPARE(s.panel().wave->value(), std::min(3.0, core::py_round(0.03 * 0.5 * narrow, 2)));
        const std::size_t before = s.ops.size();
        emit s.panel().wave->editingFinished();  // (left as it was: no edit)
        QCOMPARE(s.ops.size(), before);
        s.panel().wave->setValue(0.2);
        emit s.panel().wave->editingFinished();
        QCOMPARE(s.last(), (Json{{"op", "edit_line"}, {"id", "old1"}, {"style", Json{{"wave_mm", 0.2}, {"wobble", nullptr}}}}));
        QVERIFY(!s.line("old1")->style.contains("wobble"));
        QCOMPARE(s.line("old1")->style["hand"], Json(false));  // (said already: its letters stay in the ellipse they were fitted in)
        QCOMPARE(set_of(*s.line("old1")), as_set);
        s.trigger("act_undo");
        QCOMPARE(s.line("old1")->style["wobble"], Json(0.5));
        QVERIFY(!s.line("old1")->style.contains("wave_mm"));
        // a line whose hand says true, and one that says no hand: the wobble gone, hand: false is written with it — the
        // letters set as they were (without it they would be set smaller or in other columns: the fixture's box)
        for (const char* id : {"old2", "old3"}) {
            s.choose(id);
            s.panel().wave->setValue(0.25);
            emit s.panel().wave->editingFinished();
            QCOMPARE(s.last(), (Json{{"op", "edit_line"}, {"id", id}, {"style", Json{{"wave_mm", 0.25}, {"wobble", nullptr}, {"hand", false}}}}));
            QCOMPARE(canon(s.line(id)->style), canon(Json{{"hand", false}, {"wave_mm", 0.25}}));
            QCOMPARE(set_of(*s.line(id)), as_set);
            core::StoryLine bare = *s.line(id);
            bare.style.erase("hand");
            QVERIFY(set_of(bare) != as_set);
        }
        // …and the same through the palette: a look of now takes the wobble away, and says hand: false with it
        for (const char* id : {"old4", "old5"}) {
            s.choose(id);
            QVERIFY(s.window->restyle_with_balloon_preset(QStringLiteral("double")));
            QCOMPARE(canon(s.line(id)->style), canon(Json{{"hand", false}, {"double", true}}));
            QCOMPARE(set_of(*s.line(id)), as_set);
            s.trigger("act_undo");
            QVERIFY(s.line(id)->style.contains("wobble"));
            QCOMPARE(set_of(*s.line(id)), as_set);
        }
        // (a kind that is no oval has no such ellipse: nothing is said of hand)
        s.choose("old4");
        QVERIFY(s.window->restyle_with_balloon_preset(QStringLiteral("rounded")));
        QCOMPARE(s.line("old4")->style, Json::object());
        // 既定の設定に戻す takes the keys of before away with the others
        s.choose("old5");
        s.panel().reset->click();
        QVERIFY(s.line("old5")->style.empty());
        // saved and read again: the book still opens
        s.session->save_now();
        QVERIFY(s.session->wait_saved(std::chrono::milliseconds(20000)));
        const core::Document back = gui_test::read_book(gui_test::path_of(s.tmp.path() + "/book"));
        QVERIFY(back.read_only_reason.empty());
        QCOMPARE(back.story.size(), std::size_t{5});
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
        const std::string dotted = add("dotted_box");
        const std::string fancy = add("fancy_box");
        const std::string tone = add("tone_box");
        const std::string thought = add("thought", Json{{"tails", Json::array({Json{{"to", Json::array({50, 100})}}})}});
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
        s.choose(tone);
        QCOMPARE(rows(), QStringLiteral("corner wave dash"));
        s.choose(fancy);
        QCOMPARE(rows(), QStringLiteral("wave dash"));  // (its marks sit at square corners)
        s.choose(dotted);
        QCOMPARE(rows(), QString());  // (dots along a square edge: no corners, no trembling, no dashes)
        s.choose(thought);
        QCOMPARE(rows(), QStringLiteral("wave dash tail_fillet"));  // (its tails are always bubbles: no kind to choose)
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
        // a field left as it shows makes no edit, whatever the line holds: a default written out (sides 8), more decimals
        // than the field shows (2.123 mm), tails that differ from one another — none is rewritten by walking through
        {
            const std::string held = add("polygon", Json{{"style", Json{{"sides", 8}, {"corner_mm", 2.123}, {"wave_mm", 0.333}, {"dash_mm", 1.004}}},
                                                         {"tails", Json::array({Json{{"to", Json::array({50, 100})}, {"fillet_mm", 1.0}, {"width_mm", 3.0}},
                                                                                Json{{"to", Json::array({110, 100})}, {"fillet_mm", 2.0}, {"width_mm", 5.0}}})}});
            s.choose(held);
            const Json before = snapshot(*s.line(held));
            const std::size_t count = s.ops.size();
            QCOMPARE(p.corner->value(), 2.12);
            QCOMPARE(p.tail_fillet->value(), 1.0);
            for (QDoubleSpinBox* box : {p.corner, p.wave, p.dash, p.tail_fillet}) emit box->editingFinished();
            emit p.sides->editingFinished();
            QVERIFY2(s.ops.size() == count, s.last().dump().c_str());
            QCOMPARE(snapshot(*s.line(held)), before);
            // (しっぽの幅, a field of before, sends the line's style as every such field does — and no longer makes tails
            // of different widths one width when it is left as it shows)
            emit p.tail_width->editingFinished();
            for (std::size_t i = count; i < s.ops.size(); ++i) QVERIFY2(!s.ops[i].contains("tails"), s.ops[i].dump().c_str());
            QCOMPARE(snapshot(*s.line(held))["tails"], before["tails"]);
            s.choose(held);
            const std::size_t kept = s.ops.size();
            // (changed, it is written — and then every tail takes it)
            p.tail_fillet->setValue(3.0);
            emit p.tail_fillet->editingFinished();
            QCOMPARE(s.ops.size(), kept + 1);
            QCOMPARE(s.line(held)->tails[0]["fillet_mm"], Json(3.0));
            QCOMPARE(s.line(held)->tails[1]["fillet_mm"], Json(3.0));
            QCOMPARE(s.line(held)->tails[1]["width_mm"], Json(5.0));
            // (set and set back before the panel shows the line again: both are edits)
            p.corner->setValue(4.0);
            emit p.corner->editingFinished();
            p.corner->setValue(2.12);
            emit p.corner->editingFinished();
            QCOMPARE(s.ops.size(), kept + 3);
            QCOMPARE(s.line(held)->style["corner_mm"], Json(2.12));
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
        QVERIFY(presets.size() >= 24 && presets.size() <= 52);  // (fukidashi@2: 17 more — the pen's line, drips, the symbols, the tone)
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
        QCOMPARE(list->item(0)->text(), QStringLiteral("普通"));  // (as the kinds' menus say it: 普通（楕円）)
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
        // moved to a screen of another scale: the pictures are drawn again at it
        {
            QEvent moved(QEvent::DevicePixelRatioChange);
            QCoreApplication::sendEvent(&s.palette(), &moved);
            QVERIFY(!s.palette().pictures_drawn());
            QTRY_VERIFY_WITH_TIMEOUT(s.palette().pictures_drawn(), 30000);
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
    // gone —, its words, place, face, colour and tails kept; to or from a capsule or a polygon its box is fitted to its
    // words about its middle; all of it one step, and one 取り消し gives the line back exactly. A pick that would change
    // nothing makes no step.
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
        // a look of another kind that holds its letters as a burst does: the kind and the look, the box as it was
        s.window->use_balloon_preset(QStringLiteral("double"));
        QCOMPARE(s.ops.size(), count + 1);  // (one op)
        QCOMPARE(s.last(), (Json{{"op", "edit_line"}, {"id", id}, {"balloon", "speech"},
                                 {"style", Json{{"border_mm", nullptr}, {"spikes", nullptr}, {"spike_depth", nullptr}, {"spike_alt", nullptr}, {"double", true}}}}));
        {
            const core::StoryLine& l = *s.line(id);
            QCOMPARE(l.balloon, std::string("speech"));
            // (the look's keys of before gone, the preset's set; the letters' own — their colour too — and the turn kept)
            QCOMPARE(l.style, (Json{{"font", "maru"}, {"size_mm", 4.5}, {"text_dx_mm", 1.0}, {"rotate_deg", 5.0}, {"rgb", Json::array({200, 30, 30})}, {"double", true}}));
            const Json after = snapshot(l);
            for (const char* same : {"id", "text", "wrap", "x", "y", "w", "h", "tails", "tail"}) QVERIFY2(after[same] == before[same], same);
        }
        QCOMPARE(s.canvas()->selected_line_id, std::optional<std::string>(id));  // (still the chosen line)
        QCOMPARE(s.canvas()->tool(), QStringLiteral("select"));
        QVERIFY(s.window->armed_balloon_preset() == nullptr);                    // (the next line's look is not changed by it)
        s.trigger("act_undo");
        QCOMPARE(snapshot(*s.line(id)), before);
        s.trigger("act_redo");
        QCOMPARE(s.line(id)->balloon, std::string("speech"));
        // the same look again: nothing to change — no op, no step, and the status says so
        count = s.ops.size();
        QVERIFY(!s.window->restyle_with_balloon_preset(QStringLiteral("double")));
        QCOMPARE(s.ops.size(), count);
        QCOMPARE(s.window->last_notice(), QStringLiteral("選んだ台詞のフキダシは、もう「二重線」です"));
        s.trigger("act_undo");  // (the step before is the first pick's: no step was made by the second)
        QCOMPARE(snapshot(*s.line(id)), before);

        // to a polygon, which holds its letters in its own shape: the kind and the look, and in the same step the box
        // fitted to the words about its middle — six sides, 1.5 times the 13.5 × 16.5 mm of words
        count = s.ops.size();
        s.window->use_balloon_preset(QStringLiteral("polygon6"));
        QCOMPARE(s.ops.size(), count + 2);
        QCOMPARE(s.ops[count], (Json{{"op", "edit_line"}, {"id", id}, {"balloon", "polygon"},
                                     {"style", Json{{"border_mm", nullptr}, {"spikes", nullptr}, {"spike_depth", nullptr}, {"spike_alt", nullptr}, {"sides", 6}}}}));
        QCOMPARE(s.ops[count + 1]["op"], Json("move_line"));
        QVERIFY(!s.ops[count + 1].contains("tails"));
        {
            const core::StoryLine& l = *s.line(id);
            QCOMPARE(l.balloon, std::string("polygon"));
            QCOMPARE(l.w_mm.value(), 20.25);
            QCOMPARE(l.h_mm.value(), 24.75);
            QVERIFY(std::fabs(l.x_mm.value() + l.w_mm.value() / 2 - 77.0) <= 0.011);  // (its middle where it was)
            QVERIFY(std::fabs(l.y_mm.value() + l.h_mm.value() / 2 - 74.0) <= 0.011);
            const Json after = snapshot(l);
            for (const char* same : {"id", "text", "wrap", "tails", "tail"}) QVERIFY2(after[same] == before[same], same);
        }
        s.trigger("act_undo");  // (one step)
        QCOMPARE(snapshot(*s.line(id)), before);
        s.trigger("act_redo");
        QCOMPARE(s.line(id)->balloon, std::string("polygon"));
        // a click on the palette's item does the same; from the polygon to an oval: fitted again, as an oval
        s.window->show_dock(QStringLiteral("フキダシ"));
        s.palette().draw_pictures();
        count = s.ops.size();
        QListWidget* list = s.palette().list();
        QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, list->visualItemRect(s.palette().item(QStringLiteral("wave"))).center());
        QCOMPARE(s.ops.size(), count + 2);
        QCOMPARE(s.line(id)->balloon, std::string("speech"));
        QCOMPARE(s.line(id)->style["wave_mm"], Json(0.4));
        QVERIFY(!s.line(id)->style.contains("sides"));
        QVERIFY(s.line(id)->w_mm.value() != 20.25);
        // ベタ白抜き on letters of a colour the person chose: black inside, the letters' colour kept
        s.window->use_balloon_preset(QStringLiteral("black_oval"));
        QCOMPARE(s.line(id)->style["fill_rgb"], Json::array({0, 0, 0}));
        QCOMPARE(s.line(id)->style["rgb"], Json::array({200, 30, 30}));
        QVERIFY(!s.line(id)->style.contains("wave_mm"));
        s.window->use_balloon_preset(QStringLiteral("oval"));
        QVERIFY(!s.line(id)->style.contains("fill_rgb"));
        QVERIFY(!s.line(id)->style.contains("line_rgb"));
        QCOMPARE(s.line(id)->style, (Json{{"font", "maru"}, {"size_mm", 4.5}, {"text_dx_mm", 1.0}, {"rotate_deg", 5.0}, {"rgb", Json::array({200, 30, 30})}}));
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
        const auto inside_box = [&](const Json& tail) {
            const core::StoryLine& l = *s.line(id);
            const double tx = tail["to"][0].get<double>(), ty = tail["to"][1].get<double>();
            return tx > l.x_mm.value() && tx < l.x_mm.value() + l.w_mm.value() && ty > l.y_mm.value() && ty < l.y_mm.value() + l.h_mm.value();
        };
        QVERIFY(inside_box(s.line(id)->tails.front()));
        s.window->use_balloon_preset(QStringLiteral("oval_tail"));
        QCOMPARE(s.line(id)->tails.size(), std::size_t{1});
        QVERIFY(!s.line(id)->tails.front().contains("kind"));
        QVERIFY(s.line(id)->tails.front()["to"][1].get<double>() > s.line(id)->y_mm.value() + s.line(id)->h_mm.value());
        // tails that point both ways — a notch and a tail that runs out: under either preset of tails, the preset's own
        {
            const core::StoryLine& l = *s.line(id);
            const Json mixed = Json::array({Json{{"to", Json::array({l.x_mm.value() + l.w_mm.value() * 0.7, l.y_mm.value() + l.h_mm.value() * 0.6})}, {"kind", "inward"}},
                                            Json{{"to", Json::array({l.x_mm.value() - 8.0, l.y_mm.value() + l.h_mm.value() + 12.0})}}});
            QVERIFY(s.window->apply_ops(Json::array({Json{{"op", "edit_line"}, {"id", id}, {"tails", mixed}}})));
            s.choose(id);
            QVERIFY(s.window->restyle_with_balloon_preset(QStringLiteral("oval_tail")));
            QCOMPARE(s.line(id)->tails.size(), std::size_t{1});
            QVERIFY(!s.line(id)->tails.front().contains("kind"));
            QVERIFY(!inside_box(s.line(id)->tails.front()));
            QVERIFY(s.window->apply_ops(Json::array({Json{{"op", "edit_line"}, {"id", id}, {"tails", mixed}}})));
            s.choose(id);
            QVERIFY(s.window->restyle_with_balloon_preset(QStringLiteral("inward_tail")));
            QCOMPARE(s.line(id)->tails.size(), std::size_t{1});
            QCOMPARE(s.line(id)->tails.front()["kind"], Json("inward"));
            QVERIFY(inside_box(s.line(id)->tails.front()));
        }
        // every preset can be given to a line: the ops take each (no refusal), one undo goes back each time; the one the
        // line is already makes no step
        int given = 0;
        for (const app::BalloonPreset& preset : app::balloon_presets()) {
            s.choose(id);
            const Json was = snapshot(*s.line(id));
            count = s.ops.size();
            if (!s.window->restyle_with_balloon_preset(QString::fromLatin1(preset.key))) {
                QVERIFY2(s.ops.size() == count && snapshot(*s.line(id)) == was, preset.key);
                continue;
            }
            ++given;
            QVERIFY2(s.ops.size() > count, preset.key);
            QVERIFY2(s.line(id)->balloon == preset.kind, preset.key);
            for (const auto& [key, value] : preset.style.items()) {
                if (key != "rgb") QVERIFY2(s.line(id)->style.at(key) == value, preset.key);
            }
            QVERIFY2(snapshot(*s.line(id))["text"] == was["text"], preset.key);
            QVERIFY2(render::text::text_layout(*s.line(id), 150).em > 0, preset.key);  // (it is set and drawn)
            s.trigger("act_undo");
            QVERIFY2(snapshot(*s.line(id)) == was, preset.key);
        }
        QVERIFY(given >= static_cast<int>(app::balloon_presets().size()) - 2);  // (all but the two it is: 普通, 内向きしっぽ)

        // the letters' colour: ベタ gives white letters to a line whose letters are the usual black (none said, or black
        // said), and a white balloon gives them back — white letters went with the black inside
        for (const Json& said : {Json(), Json::array({0, 0, 0})}) {
            Json op{{"op", "add_line"}, {"text", "くろい\nこえ"}, {"balloon", "speech"}, {"x_mm", 110}, {"y_mm", 60}, {"w_mm", 30}, {"h_mm", 26}};
            if (!said.is_null()) op["style"] = Json{{"rgb", said}};
            const std::string plain = s.put(op);
            s.choose(plain);
            QVERIFY(s.window->restyle_with_balloon_preset(QStringLiteral("black_oval")));
            QCOMPARE(canon(s.line(plain)->style), canon(Json{{"fill_rgb", Json::array({0, 0, 0})}, {"line_rgb", Json::array({0, 0, 0})}, {"rgb", Json::array({255, 255, 255})}}));
            QVERIFY(s.window->restyle_with_balloon_preset(QStringLiteral("black_shout")));  // (ベタ to ベタ: the white letters stay)
            QCOMPARE(s.line(plain)->style["rgb"], Json::array({255, 255, 255}));
            QVERIFY(s.window->restyle_with_balloon_preset(QStringLiteral("rounded")));
            QCOMPARE(s.line(plain)->style, Json::object());
            QVERIFY(s.window->apply_ops(Json::array({Json{{"op", "delete_line"}, {"id", plain}}})));
        }
        // 白抜き文字 — white letters with their own edge, on a white balloon or on none: the person's, left as they are
        for (const char* kind : {"speech", "none", "sfx"}) {
            const Json letters{{"rgb", Json::array({255, 255, 255})}, {"outline_mm", 0.6}, {"outline_rgb", Json::array({0, 0, 0})}};
            const std::string white = s.put(Json{{"op", "add_line"}, {"text", "しろぬき"}, {"balloon", kind}, {"x_mm", 110}, {"y_mm", 100}, {"w_mm", 30}, {"h_mm", 26},
                                                 {"style", letters}});
            s.choose(white);
            QVERIFY2(s.window->restyle_with_balloon_preset(QStringLiteral("rounded")), kind);
            QCOMPARE(s.line(white)->balloon, std::string("rounded"));
            QCOMPARE(s.line(white)->style, letters);
            QVERIFY(s.window->apply_ops(Json::array({Json{{"op", "delete_line"}, {"id", white}}})));
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

    // A pick on one of joined balloons (style.group): they are drawn as one balloon — its inside, its line, its dashes
    // and its second line from its first line —, so every one of them takes the kind and the look, in one step. Each
    // keeps its words, its place, its face and its tails.
    void aPickOnAJoinedBalloonTakesEveryMember() {
        Studio s;
        const std::string first = s.put(Json{{"op", "add_line"}, {"text", "それで"}, {"balloon", "speech"}, {"x_mm", 40}, {"y_mm", 60}, {"w_mm", 34}, {"h_mm", 26},
                                             {"style", Json{{"group", "g1"}, {"font", "maru"}}}, {"tails", Json::array({Json{{"to", Json::array({36.0, 100.0})}}})}});
        const std::string second = s.put(Json{{"op", "add_line"}, {"text", "どうした"}, {"balloon", "speech"}, {"x_mm", 66}, {"y_mm", 74}, {"w_mm", 30}, {"h_mm", 24},
                                              {"style", Json{{"group", "g1"}}}});
        const std::string apart = s.put(Json{{"op", "add_line"}, {"text", "べつ"}, {"balloon", "speech"}, {"x_mm", 120}, {"y_mm", 60}, {"w_mm", 30}, {"h_mm", 24}});
        const Json was_first = snapshot(*s.line(first)), was_second = snapshot(*s.line(second)), was_apart = snapshot(*s.line(apart));
        // ベタ白抜き picked on the second: both black inside with white letters (on the second alone its letters would be
        // white on the first line's white inside — unseen)
        s.choose(second);
        std::size_t count = s.ops.size();
        QVERIFY(s.window->restyle_with_balloon_preset(QStringLiteral("black_oval")));
        QCOMPARE(s.ops.size(), count + 2);  // (one edit_line each, applied together)
        for (const std::string& id : {first, second}) {
            QCOMPARE(s.line(id)->style["fill_rgb"], Json::array({0, 0, 0}));
            QCOMPARE(s.line(id)->style["line_rgb"], Json::array({0, 0, 0}));
            QCOMPARE(s.line(id)->style["rgb"], Json::array({255, 255, 255}));
            QCOMPARE(s.line(id)->style["group"], Json("g1"));
        }
        QCOMPARE(s.line(first)->style["font"], Json("maru"));
        QCOMPARE(PageCanvas_tails(*s.line(first)), was_first["tails"]);
        QCOMPARE(snapshot(*s.line(apart)), was_apart);  // (a line that is not joined: untouched)
        QCOMPARE(s.window->last_notice(), QStringLiteral("つないだフキダシをまとめて「ベタ白抜き」にしました（元に戻す で取り消せます）"));
        // (drawn: the joined balloon is black where its letters are white — the second line's middle)
        {
            const core::StoryLine& l = *s.line(second);
            QVERIFY(s.canvas()->wait_rendered(60000));
            const QImage page = s.canvas()->renderer().compose(s.canvas()->base_dpi());
            const double px = s.canvas()->base_dpi() / 25.4;
            const QRect box = QRectF(l.x_mm.value() * px, l.y_mm.value() * px, l.w_mm.value() * px, l.h_mm.value() * px).toAlignedRect().intersected(page.rect());
            int dark = 0, light = 0;
            for (int y = box.top(); y <= box.bottom(); ++y)
                for (int x = box.left(); x <= box.right(); ++x) (qGray(page.pixel(x, y)) < 128 ? dark : light) += 1;
            QVERIFY2(dark > light && light > 20, qPrintable(QStringLiteral("%1 dark, %2 light").arg(dark).arg(light)));
        }
        s.trigger("act_undo");  // (one step for both)
        QCOMPARE(snapshot(*s.line(first)), was_first);
        QCOMPARE(snapshot(*s.line(second)), was_second);
        // 二重線 picked on the second: the first line, whose look the joined balloon is drawn with, takes it too
        s.choose(second);
        QVERIFY(s.window->restyle_with_balloon_preset(QStringLiteral("double")));
        QCOMPARE(s.line(first)->style["double"], Json(true));
        QCOMPARE(s.line(second)->style["double"], Json(true));
        QVERIFY(!s.window->restyle_with_balloon_preset(QStringLiteral("double")));  // (both are that look already: no step)
        // a preset about its tails: the kind and the look for both, its tails for the picked line alone
        s.choose(second);
        QVERIFY(s.window->restyle_with_balloon_preset(QStringLiteral("oval_tail")));
        QCOMPARE(s.line(second)->tails.size(), std::size_t{1});
        QCOMPARE(PageCanvas_tails(*s.line(first)), was_first["tails"]);
        QVERIFY(!s.line(first)->style.contains("double"));
        s.trigger("act_undo");
        s.trigger("act_undo");
        QCOMPARE(snapshot(*s.line(first)), was_first);
        QCOMPARE(snapshot(*s.line(second)), was_second);
    }

    // An inward tail — a notch — is a place in its balloon: it goes with the balloon when that is dragged or resized on
    // the page (the other tails stay where they point), 取り消し puts both back; its tip (●) dragged out of the balloon
    // stops inside it; しっぽをまっすぐにする keeps it a notch.
    void aNotchGoesWithItsBalloon() {
        Studio s;
        const Json tails = Json::array({Json{{"to", Json::array({64.0, 58.0})}, {"kind", "inward"}}, Json{{"to", Json::array({36.0, 92.0})}}});
        const std::string id = s.put(Json{{"op", "add_line"}, {"text", "そとの\nこえ"}, {"balloon", "speech"}, {"x_mm", 40}, {"y_mm", 40}, {"w_mm", 30}, {"h_mm", 26},
                                          {"tails", tails}});
        s.choose(id);
        const Json before = snapshot(*s.line(id));
        // dragged by (4, 3) mm: the notch with it, the other tail where it points
        inject::mouse_stroke(s.canvas(), {QPointF(55.0, 53.0), QPointF(57.0, 54.0), QPointF(59.0, 56.0)});
        QCOMPARE(s.last(), (Json{{"op", "move_line"}, {"id", id}, {"x_mm", 44.0}, {"y_mm", 43.0}}));
        QCOMPARE(s.line(id)->tails[0], (Json{{"to", Json::array({68.0, 61.0})}, {"kind", "inward"}}));
        QCOMPARE(s.line(id)->tails[1], tails[1]);
        QVERIFY(core::tail_hidden(*s.line(id), s.line(id)->tails[0]["to"]));
        s.trigger("act_undo");
        QCOMPARE(snapshot(*s.line(id)), before);
        s.trigger("act_redo");
        QCOMPARE(s.line(id)->tails[0]["to"], Json::array({68.0, 61.0}));
        s.trigger("act_undo");
        // made twice as wide and as tall from its corner: the notch the same share of the way across
        QVERIFY(s.window->apply_ops(Json::array({Json{{"op", "move_line"}, {"id", id}, {"w_mm", 60}, {"h_mm", 52}}})));
        QCOMPARE(s.line(id)->tails[0]["to"], Json::array({88.0, 76.0}));
        QVERIFY(core::tail_hidden(*s.line(id), s.line(id)->tails[0]["to"]));
        s.trigger("act_undo");
        QCOMPARE(snapshot(*s.line(id)), before);
        // its tip dragged far out of the balloon: it stops inside, on the way it was dragged
        s.choose(id);
        inject::mouse_stroke(s.canvas(), {QPointF(64.0, 58.0), QPointF(80.0, 60.0), QPointF(110.0, 64.0)});
        QCOMPARE(s.last()["op"], Json("move_line"));
        const Json dragged = s.last()["tails"][0];
        QCOMPARE(dragged["kind"], Json("inward"));
        QVERIFY(core::tail_hidden(*s.line(id), dragged["to"]));
        QVERIFY(dragged["to"][0].get<double>() > 64.0 && dragged["to"][0].get<double>() < 70.0);  // (toward where it was dragged, inside the 40..70 mm of the balloon)
        QCOMPARE(s.last()["tails"][1], tails[1]);
        // (dragged inside the balloon: where it was let go)
        inject::mouse_stroke(s.canvas(), {QPointF(dragged["to"][0].get<double>(), dragged["to"][1].get<double>()), QPointF(52.0, 50.0), QPointF(50.0, 48.0)});
        QCOMPARE(s.last()["tails"][0]["to"], Json::array({50.0, 48.0}));
        // しっぽをまっすぐにする: its bends go, its kind stays (a notch made a plain tail would point into its own balloon)
        const std::unique_ptr<QMenu> menu = s.window->line_menu(id);
        QVERIFY(menu != nullptr);
        for (QAction* a : menu->actions())
            if (a->text() == QStringLiteral("しっぽをまっすぐにする")) a->trigger();
        QCOMPARE(s.last()["tails"], Json::array({Json{{"to", Json::array({50.0, 48.0})}, {"kind", "inward"}}, tails[1]}));
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
        // (said with the text tool's settings too, where it shows while the palette is closed)
        auto* tool = s.window->text_settings();
        QVERIFY(!s.window->balloon_dock()->isVisible());
        QVERIFY(static_cast<QFormLayout*>(tool->layout())->isRowVisible(tool->preset_row));
        QCOMPARE(tool->preset_name->text(), QStringLiteral("パレットの形: 縦長"));
        QCOMPARE(tool->preset_clear->text(), QStringLiteral("外す"));
        QVERIFY(!tool->preset_clear->toolTip().isEmpty());
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
        {
            const std::unique_ptr<QMenu> menu = s.palette().item_menu(QStringLiteral("double"));
            QVERIFY(menu != nullptr);
            QCOMPARE(texts(menu.get()), QStringList({QStringLiteral("選んだ台詞のフキダシをこの形にする"), QStringLiteral("次に置く台詞をこの形にする")}));
            menu->actions().at(1)->trigger();
        }
        QCOMPARE(s.ops.size(), count);
        QCOMPARE(s.palette().marked(), QStringLiteral("double"));
        QCOMPARE(tool->preset_name->text(), QStringLiteral("パレットの形: 二重線"));
        QVERIFY(s.palette().item_menu(QStringLiteral("no such preset")) == nullptr);
        // …and its 「選んだ台詞のフキダシをこの形にする」 ("line") restyles the chosen line though the text tool is in hand
        s.choose(chosen);
        s.trigger("act_text");
        QVERIFY(s.canvas()->selected_line_id.has_value());
        {
            const std::unique_ptr<QMenu> menu = s.palette().item_menu(QStringLiteral("thick"));
            menu->actions().at(0)->trigger();
        }
        QVERIFY(s.ops.size() > count);
        QCOMPARE(s.line(chosen)->balloon, std::string("speech"));
        QCOMPARE(s.line(chosen)->style["border_mm"], Json(0.8));
        // (it was the wide capsule: made an oval, its box is the oval's for its words — in that one step)
        QCOMPARE(render::text::text_layout(*s.line(chosen), 600).em, std::int64_t{118});
        s.trigger("act_undo");
        QCOMPARE(s.line(chosen)->balloon, std::string("capsule"));
        s.trigger("act_redo");
        QCOMPARE(s.palette().marked(), QStringLiteral("double"));
        QCOMPARE(s.palette().list()->selectedItems(), QList<QListWidgetItem*>{s.palette().item(QStringLiteral("double"))});
        // the text tool's own フキダシ chosen: the palette's look is let go, the kind chosen placed plain
        auto* settings = s.window->text_settings();
        settings->balloon->setCurrentIndex(settings->balloon->findData(QStringLiteral("rounded")));
        emit settings->balloon->activated(settings->balloon->currentIndex());
        QVERIFY(s.window->armed_balloon_preset() == nullptr);
        QCOMPARE(s.palette().marked(), QString());
        QVERIFY(!static_cast<QFormLayout*>(tool->layout())->isRowVisible(tool->preset_row));
        QVERIFY(s.palette().list()->selectedItems().isEmpty());
        for (int i = 0; i < s.palette().list()->count(); ++i) QVERIFY(!s.palette().list()->item(i)->font().bold());
        s.trigger("act_text");
        placed = s.type_at(QPointF(100.0, 240.0), QStringLiteral("ふつう"));
        QCOMPARE(placed["balloon"], Json("rounded"));
        QVERIFY(!placed.contains("style"));
        // 外す beside the look's name lets it go too
        s.window->arm_balloon_preset(QStringLiteral("wave"));
        QVERIFY(static_cast<QFormLayout*>(tool->layout())->isRowVisible(tool->preset_row));
        tool->preset_clear->click();
        QVERIFY(s.window->armed_balloon_preset() == nullptr);
        QVERIFY(!static_cast<QFormLayout*>(tool->layout())->isRowVisible(tool->preset_row));
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
        // the window's measure (app::lettering) against sizes worked out by hand from the rule (5 mm letters, columns 2 mm
        // apart, 0.75 mm round the words; a capsule's round ends of radius half its words' narrow side + 2.25 mm, the
        // words' corners 0.75 mm inside them; a polygon the words' block √2 times as big for eight sides, 1.5 for six)
        struct Sized {
            std::vector<std::string> words;
            const char* kind;
            int sides;
            double w, h;
        };
        const Sized sized[] = {
            {{"そうなんだ"}, "capsule", 0, 9.5, 29.0718},                 // 5 + 4.5; 25 + 1.5 + 2 × (4.75 − √(4.75² − 3.25²))
            {{"うん"}, "capsule", 0, 9.5, 14.0718},                       // 10 + 1.5 + 2.5718
            {{"あいうえお", "かきくけこ"}, "capsule", 0, 16.5, 33.5132},  // 12 + 4.5; 25 + 1.5 + 2 × (8.25 − √(8.25² − 6.75²))
            {{"そうなんだ"}, "polygon", 0, 9.1924, 37.4767},              // 6.5 × √2, 26.5 × √2
            {{"あいうえお", "かきくけこ"}, "polygon", 0, 19.0919, 37.4767},
            {{"あいうえお", "かきくけこ"}, "polygon", 8, 19.0919, 37.4767},
            {{"あいうえお", "かきくけこ"}, "polygon", 6, 20.25, 39.75},   // 13.5 × 1.5, 26.5 × 1.5
        };
        for (const Sized& each : sized) {
            std::vector<std::u32string> breaks;
            for (const std::string& w : each.words) breaks.push_back(render::text::u32(w));
            const std::pair<double, double> got = app::lettering::measure(breaks, each.kind, app::lettering::kEmMm, each.sides);
            const QString what = QStringLiteral("%1 %2 sides, %3 columns: %4 × %5").arg(QString::fromLatin1(each.kind)).arg(each.sides).arg(each.words.size()).arg(got.first).arg(got.second);
            QVERIFY2(std::fabs(got.first - each.w) < 0.002 && std::fabs(got.second - each.h) < 0.002, qPrintable(what));
            // (the auto-lettering's is the same function: the two cannot drift apart)
            const std::array<double, 2> service = service::letter::measure(each.words, each.kind, service::letter::kEmMm, each.sides);
            QCOMPARE(got.first, service[0]);
            QCOMPARE(got.second, service[1]);
        }
        // (the same box turned for words across: one row of four in a wide capsule)
        {
            const Json across = app::lettering::place_at(100, 100, "ひとこと", "capsule", false, nullptr);
            QCOMPARE(across["w_mm"].get<double>(), 24.07);
            QCOMPARE(across["h_mm"].get<double>(), 9.5);
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
    // (not a test of their own, and never a SKIP: the audit of a stage's run refuses every skipped slot. As the screenshots
    // of test_gui_color, they are written at the end of a run that names a folder)
    // --- fukidashi@2 (SPEC BALLOON-02): the additions in the window -------------------------------------------------------

    // The four symbols are kinds of every menu (after the polygon); chosen from a balloon's menu they are drawn with the
    // box fitted to the words about the balloon's middle, in one step (they hold their letters in their own shape), the
    // letters kept at their size; and the palette has the additions' 17 looks, each drawn, each its own picture.
    void theSymbolsAreKindsAndThePaletteHasTheAdditions() {
        Studio s;
        QStringList labels;
        for (const auto& kind : app::lettering::kinds()) labels << kind.second;
        QCOMPARE(labels.mid(4, 5), QStringList({kPolygon, QStringLiteral("ハート（甘い声）"), QStringLiteral("星（はしゃぐ声・きらめき）"), QStringLiteral("花（やわらかい声）"),
                                                QStringLiteral("ひし形（冷たいひとこと）")}));
        for (const QString& label : labels) QVERIFY2(!QRegularExpression(QStringLiteral("[A-Za-z]{4,}")).match(label).hasMatch(), qPrintable(label));
        const std::string id = s.put(Json{{"op", "add_line"}, {"text", "すき\nだよ"}, {"balloon", "speech"}, {"x_mm", 60}, {"y_mm", 60}, {"w_mm", 30}, {"h_mm", 24}});
        for (const char* kind : {"heart", "star", "flower", "diamond"}) {
            s.choose(id);
            const Json before = snapshot(*s.line(id));
            const std::size_t count = s.ops.size();
            QMenu* shapes = s.menu_at(QStringLiteral("ページ/台詞/フキダシの形"));
            for (QAction* a : shapes->actions())
                if (a->text() == app::lettering::kind_label(kind)) a->trigger();
            QCOMPARE(s.ops.size(), count + 2);
            QCOMPARE(s.ops[count], (Json{{"op", "edit_line"}, {"id", id}, {"balloon", kind}}));
            QCOMPARE(s.ops[count + 1]["op"], Json("move_line"));
            const core::StoryLine& line = *s.line(id);
            // (the box the service measures for those words: two columns of two)
            const std::array<double, 2> measured = service::letter::measure_styled({"すき", "だよ"}, kind, service::letter::kEmMm, Json::object());
            QVERIFY2(std::fabs(line.w_mm.value() - core::py_round(measured[0], 2)) <= 0.011 && std::fabs(line.h_mm.value() - core::py_round(measured[1], 2)) <= 0.011,
                     qPrintable(QStringLiteral("%1: %2 × %3 for %4 × %5").arg(kind).arg(line.w_mm.value()).arg(line.h_mm.value()).arg(measured[0]).arg(measured[1])));
            QVERIFY(std::fabs(line.x_mm.value() + line.w_mm.value() / 2 - 75.0) <= 0.011);  // (its middle where it was)
            QCOMPARE(render::text::text_layout(line, 600).em, std::int64_t{118});
            s.trigger("act_undo");
            QCOMPARE(snapshot(*s.line(id)), before);
        }
        // (the palette's additions)
        const char* added[] = {"pen_swell", "g_pen", "brush", "g_pen_shout", "drips", "horror_black", "melting_box", "horror_wave", "heart", "star", "sparkle",
                               "flower", "diamond", "tone", "gradient_tone", "tone_shout", "gradient_dark"};
        QCOMPARE(std::size(added), std::size_t{17});
        QCOMPARE(app::balloon_presets().size(), std::size_t{32 + 17});
        std::set<std::string> pictures;
        for (const char* key : added) {
            const app::BalloonPreset* preset = app::balloon_preset(QString::fromLatin1(key));
            QVERIFY2(preset != nullptr, key);
            const QImage picture = app::preset_picture(*preset, 1.0);
            QVERIFY2(dark_pixels(picture) > 100, key);
            const QImage plain = picture.convertToFormat(QImage::Format_RGB888);
            pictures.insert(std::string(reinterpret_cast<const char*>(plain.constBits()), static_cast<std::size_t>(plain.sizeInBytes())));
            // (its words, as the person reads them)
            QVERIFY2(!QRegularExpression(QStringLiteral("[A-Za-z]{4,}")).match(QString::fromUtf8(preset->name) + QString::fromUtf8(preset->tip)).hasMatch(), key);
        }
        QCOMPARE(pictures.size(), std::size(added));  // (each its own picture)
        // (the keys a preset takes away from a line given another look: the additions' with the others)
        const std::vector<std::string>& look = app::balloon_look_keys();
        for (const char* key : {"pen_swell", "pen_angle", "pen_taper", "pen_strokes", "drip_mm", "drips", "drip_width_mm", "drip_seed", "points", "fill_tone", "fill_gradient"}) {
            QVERIFY2(std::find(look.begin(), look.end(), key) != look.end(), key);
        }
    }

    // A preset of the additions given to a line: its look in one step of 取り消し — the plain look takes the additions
    // away again; a look with drips makes the box as much taller (the body kept), and one without them as much shorter;
    // a look with drips given to a balloon drawn by hand says plainly that it has none.
    void aPresetOfTheAdditionsRestylesALine() {
        Studio s;
        const std::string id = s.put(Json{{"op", "add_line"}, {"text", "こわい"}, {"balloon", "speech"}, {"x_mm", 60}, {"y_mm", 60}, {"w_mm", 30}, {"h_mm", 24}});
        const Json before = snapshot(*s.line(id));
        s.choose(id);
        s.window->restyle_with_balloon_preset(QStringLiteral("horror_black"));
        const core::StoryLine& dripping = *s.line(id);
        QCOMPARE(dripping.style["drip_mm"], Json(7.0));
        QCOMPARE(dripping.h_mm.value(), 31.0);  // (24 and the drips' 7: the body as it was)
        QCOMPARE(dripping.y_mm.value(), 60.0);
        QCOMPARE(dripping.w_mm.value(), 30.0);
        s.trigger("act_undo");
        QCOMPARE(snapshot(*s.line(id)), before);
        s.trigger("act_redo");
        // (from drips to the pen's line: the drips go, the box with them; the black goes too — a look's every key)
        s.window->restyle_with_balloon_preset(QStringLiteral("g_pen"));
        const core::StoryLine& pen = *s.line(id);
        QCOMPARE(pen.h_mm.value(), 24.0);
        QVERIFY(!pen.style.contains("drip_mm") && !pen.style.contains("fill_rgb") && !pen.style.contains("drip_seed"));
        QCOMPARE(pen.style["pen_swell"], Json(0.5));
        QCOMPARE(pen.style["pen_taper"], Json(0.6));
        // (the plain look: nothing of the additions left)
        s.window->restyle_with_balloon_preset(QStringLiteral("oval"));
        for (const std::string& key : app::balloon_look_keys()) QVERIFY2(!s.line(id)->style.contains(key), key.c_str());
        // (a tone and a gradient tone: the whole tone, as the op keeps it)
        s.window->restyle_with_balloon_preset(QStringLiteral("gradient_tone"));
        QCOMPARE(s.line(id)->style["fill_tone"], (Json{{"density", 0.05}, {"lpi", 60.0}, {"angle", 45.0}, {"pattern", "dot"}, {"to", 0.5}, {"dir", 90.0}}));
        // (drawn by hand: its outline as it is, the look's line and fill — and no drips, said so)
        const Json path = Json::array({Json::array({100, 100}), Json::array({130, 96}), Json::array({138, 120}), Json::array({110, 128})});
        const std::string drawn = s.put(Json{{"op", "add_line"}, {"text", "て"}, {"balloon", "speech"}, {"path", path}});
        s.choose(drawn);
        const Json box{{"x", s.line(drawn)->x_mm.json()}, {"y", s.line(drawn)->y_mm.json()}, {"w", s.line(drawn)->w_mm.json()}, {"h", s.line(drawn)->h_mm.json()}};
        s.window->restyle_with_balloon_preset(QStringLiteral("drips"));
        QCOMPARE(s.window->last_notice(), QStringLiteral("手で描いたフキダシには、しずくは付きません（線と塗りだけ当てました）"));
        QCOMPARE((Json{{"x", s.line(drawn)->x_mm.json()}, {"y", s.line(drawn)->y_mm.json()}, {"w", s.line(drawn)->w_mm.json()}, {"h", s.line(drawn)->h_mm.json()}}), box);
        // (a symbol's look given to an oval: the kind and the box fitted to the words, one step)
        s.choose(id);
        const std::size_t count = s.ops.size();
        s.window->restyle_with_balloon_preset(QStringLiteral("sparkle"));
        QCOMPARE(s.line(id)->balloon, std::string("star"));
        QCOMPARE(s.line(id)->style["points"], Json(4));
        QCOMPARE(s.ops.size(), count + 2);  // (the kind and its look, then the box)
        QCOMPARE(render::text::text_layout(*s.line(id), 600).em, std::int64_t{118});
    }

    // The 台詞 panel's settings of the additions: shown only where they do something; each change one edit_line of its
    // key (the drips' length with the box's height, the tone as a whole) and one step of 取り消し; their words in
    // Japanese, each with a tooltip.
    void eachAdditionSettingWritesItsKey() {
        Studio s;
        const auto add = [&](const char* kind, Json more = Json::object()) {
            Json op{{"op", "add_line"}, {"text", "ねえ\n聞いて"}, {"balloon", kind}, {"x_mm", 60}, {"y_mm", 60}, {"w_mm", 32}, {"h_mm", 26}};
            for (const auto& [key, value] : more.items()) op[key] = value;
            return s.put(op);
        };
        const std::string speech = add("speech");
        const std::string star = add("star");
        const std::string flower = add("flower");
        const std::string dotted = add("dotted_box");
        const std::string flash = add("flash");
        const std::string sfx = add("sfx");
        const Json path = Json::array({Json::array({100, 100}), Json::array({130, 96}), Json::array({138, 120}), Json::array({110, 128})});
        const std::string drawn = add("speech", Json{{"path", path}});
        auto& p = s.panel();
        QFormLayout* form = p.style_form;
        const std::initializer_list<std::pair<QWidget*, const char*>> fields{
            {p.pen_swell, "pen_swell"}, {p.pen_angle, "pen_angle"}, {p.pen_taper, "pen_taper"}, {p.pen_strokes, "pen_strokes"},
            {p.drip_length, "drip_length"}, {p.drip_count, "drip_count"}, {p.drip_width, "drip_width"}, {p.drip_seed, "drip_seed"},
            {p.points, "points"}, {p.tone_density, "tone_density"}, {p.tone_lpi, "tone_lpi"}, {p.tone_pattern, "tone_pattern"},
            {p.tone_to, "tone_to"}, {p.tone_dir, "tone_dir"}, {p.fill_blend, "fill_blend"}};
        const auto rows = [&] {
            QStringList shown;
            for (const auto& [widget, name] : fields) {
                if (form->isRowVisible(widget)) shown << QString::fromLatin1(name);
            }
            return shown.join(QLatin1Char(' '));
        };
        s.choose(speech);
        QCOMPARE(rows(), QStringLiteral("pen_swell pen_taper drip_length tone_density fill_blend"));
        s.choose(star);
        QCOMPARE(rows(), QStringLiteral("pen_swell pen_taper drip_length points tone_density fill_blend"));
        s.choose(flower);
        QCOMPARE(rows(), QStringLiteral("pen_swell pen_taper drip_length points tone_density fill_blend"));
        s.choose(dotted);
        QCOMPARE(rows(), QStringLiteral("tone_density fill_blend"));  // (dots along its edge: no pen, no drips)
        s.choose(flash);
        QCOMPARE(rows(), QStringLiteral("tone_density fill_blend"));
        s.choose(drawn);
        QCOMPARE(rows(), QStringLiteral("pen_swell pen_taper tone_density fill_blend"));  // (drawn by hand: no drips)
        s.choose(sfx);
        QCOMPARE(rows(), QString());
        // (their names, words and tooltips: Japanese, no key's name)
        for (const auto& [widget, name] : fields) {
            QCOMPARE(widget->objectName(), QStringLiteral("story_") + QString::fromLatin1(name));
            QVERIFY2(!widget->toolTip().isEmpty(), name);
            QString words = widget->toolTip();
            if (const auto* label = qobject_cast<QLabel*>(form->labelForField(widget))) words += label->text();
            if (const auto* button = qobject_cast<QPushButton*>(widget)) words += button->text();
            if (const auto* box = qobject_cast<QAbstractSpinBox*>(widget)) words += box->specialValueText();
            QVERIFY2(!QRegularExpression(QStringLiteral("[A-Za-z]{4,}")).match(words).hasMatch(), qPrintable(words));
        }
        QCOMPARE(qobject_cast<QLabel*>(form->labelForField(p.pen_swell))->text(), QStringLiteral("線の強弱"));
        QCOMPARE(qobject_cast<QLabel*>(form->labelForField(p.pen_taper))->text(), QStringLiteral("入り抜き"));
        QCOMPARE(qobject_cast<QLabel*>(form->labelForField(p.drip_length))->text(), QStringLiteral("しずくの長さ"));
        QCOMPARE(qobject_cast<QLabel*>(form->labelForField(p.tone_density))->text(), QStringLiteral("中のトーン"));
        QVERIFY(p.tone_density->toolTip().contains(QStringLiteral("4〜90%")));  // (the range that prints, said where it is set)
        QVERIFY(p.fill_blend->toolTip().contains(QStringLiteral("白黒の原稿")));

        // one setting changed: the op, the key the line holds, one undo back
        const auto one = [&](const std::string& id, const std::function<void()>& change, const Json& ops, const char* what) {
            s.choose(id);
            const Json before = snapshot(*s.line(id));
            const std::size_t count = s.ops.size();
            change();
            QVERIFY2(s.ops.size() == count + ops.size(), what);
            for (std::size_t i = 0; i < ops.size(); ++i) {
                QVERIFY2(s.ops[count + i] == ops[i], qPrintable(QStringLiteral("%1: %2").arg(what, QString::fromStdString(s.ops[count + i].dump()))));
            }
            s.trigger("act_undo");
            QVERIFY2(snapshot(*s.line(id)) == before, what);
            s.trigger("act_redo");
        };
        const auto edit = [](const std::string& id, Json style) { return Json{{"op", "edit_line"}, {"id", id}, {"style", std::move(style)}}; };
        const auto typed = [](QAbstractSpinBox* box, double value) {
            if (auto* d = qobject_cast<QDoubleSpinBox*>(box)) d->setValue(value);
            if (auto* i = qobject_cast<QSpinBox*>(box)) i->setValue(static_cast<int>(value));
            emit box->editingFinished();
        };
        one(speech, [&] { typed(p.pen_swell, 0.5); }, Json::array({edit(speech, Json{{"pen_swell", 0.5}})}), "線の強弱");
        s.choose(speech);
        QVERIFY(form->isRowVisible(p.pen_angle));  // (shown once the line swells)
        one(speech, [&] { typed(p.pen_angle, 90); }, Json::array({edit(speech, Json{{"pen_angle", 90}})}), "太い側の向き");
        one(speech, [&] { typed(p.pen_taper, 0.6); }, Json::array({edit(speech, Json{{"pen_taper", 0.6}})}), "入り抜き");
        s.choose(speech);
        QVERIFY(form->isRowVisible(p.pen_strokes));
        one(speech, [&] { typed(p.pen_strokes, 3); }, Json::array({edit(speech, Json{{"pen_strokes", 3}})}), "筆の運び");
        // (the drips' length: and the box that much taller in the same step — 26 and 6)
        one(speech, [&] { typed(p.drip_length, 6); },
            Json::array({edit(speech, Json{{"drip_mm", 6.0}}), Json{{"op", "move_line"}, {"id", speech}, {"h_mm", 32.0}}}), "しずくの長さ");
        s.choose(speech);
        QVERIFY(form->isRowVisible(p.drip_count) && form->isRowVisible(p.drip_seed));
        one(speech, [&] { typed(p.drip_count, 7); }, Json::array({edit(speech, Json{{"drips", 7}})}), "しずくの数");
        one(speech, [&] { typed(p.drip_width, 2); }, Json::array({edit(speech, Json{{"drip_width_mm", 2.0}})}), "しずくの太さ");
        one(speech, [&] { p.drip_seed->click(); }, Json::array({edit(speech, Json{{"drip_seed", 1}})}), "しずくの並びを変える");
        one(speech, [&] { p.drip_seed->click(); }, Json::array({edit(speech, Json{{"drip_seed", 2}})}), "もう一度");
        one(star, [&] { typed(p.points, 4); }, Json::array({edit(star, Json{{"points", 4}})}), "星の先の数");
        s.choose(star);
        QCOMPARE(p.spike_depth->value(), 0.38);  // (a star's own depth, not a shout's)
        // (the tone: the whole of it at once)
        const Json tone{{"density", 0.1}, {"lpi", 60.0}, {"angle", 45.0}, {"pattern", "dot"}};
        one(speech, [&] { typed(p.tone_density, 10); }, Json::array({edit(speech, Json{{"fill_tone", tone}})}), "中のトーン");
        s.choose(speech);
        QVERIFY(form->isRowVisible(p.tone_lpi) && form->isRowVisible(p.tone_to) && !form->isRowVisible(p.tone_dir));
        Json graded = tone;
        graded["to"] = 0.5;
        graded["dir"] = 90.0;
        one(speech, [&] { typed(p.tone_to, 50); }, Json::array({edit(speech, Json{{"fill_tone", graded}})}), "先の濃さ");
        Json lines = graded;
        lines["pattern"] = "line";
        one(speech, [&] {
            p.tone_pattern->setCurrentIndex(1);
            emit p.tone_pattern->activated(1);
        }, Json::array({edit(speech, Json{{"fill_tone", lines}})}), "トーンの柄");
        one(speech, [&] { typed(p.tone_density, p.tone_density->minimum()); }, Json::array({edit(speech, Json{{"fill_tone", nullptr}})}), "トーンなし");
        // (a field left as it was shown: nothing)
        s.choose(speech);
        const std::size_t count = s.ops.size();
        typed(p.pen_swell, p.pen_swell->value());
        typed(p.tone_density, p.tone_density->value());
        QCOMPARE(s.ops.size(), count);
        // (the blend: a colour asked; set, the button takes it off again)
        auto responder = std::make_shared<app::ask::Responder>();
        responder->colour = [](const QColor&, const QString&) { return std::optional<QColor>(QColor(10, 20, 30)); };
        app::ask::set_responder(responder);
        one(speech, [&] { p.fill_blend->click(); }, Json::array({edit(speech, Json{{"fill_gradient", Json{{"to", Json::array({10, 20, 30})}, {"dir", 90.0}}}})}), "中の色のグラデーション");
        s.choose(speech);
        QCOMPARE(p.fill_blend->text(), QStringLiteral("中の色のグラデーションを外す"));
        one(speech, [&] { p.fill_blend->click(); }, Json::array({edit(speech, Json{{"fill_gradient", nullptr}})}), "外す");
        app::ask::set_responder(nullptr);
    }

    void cleanupTestCase() {
        const QByteArray out = qgetenv("GENKO_FUKIDASHI_OUT");
        if (!out.isEmpty()) write_screenshots(out);
    }

private:
    void write_screenshots(const QByteArray& out) {
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
            QTRY_VERIFY_WITH_TIMEOUT(s.palette().pictures_drawn(), 30000);
            const QString shade = QLatin1String(mode) == QLatin1String("light") ? QString() : QStringLiteral("-dark");
            // the window as it is with the palette open beside 台詞 and a look chosen for the next line (the text tool's
            // settings say it: パレットの形); and those settings alone
            s.window->arm_balloon_preset(QStringLiteral("double"));
            QTest::qWait(300);
            QVERIFY(s.window->grab().save(dir + QStringLiteral("/gui-window%1.png").arg(shade)));
            QVERIFY(s.window->text_settings()->grab().save(dir + QStringLiteral("/gui-armed%1.png").arg(shade)));
            s.window->balloon_dock()->setFloating(true);
            s.window->balloon_dock()->resize(440, 920);  // (every preset in sight: four across)
            QTRY_VERIFY_WITH_TIMEOUT(s.palette().pictures_drawn(), 30000);
            s.window->arm_balloon_preset(QStringLiteral("capsule"));
            QTest::qWait(200);
            const QString suffix = QLatin1String(mode) == QLatin1String("light") ? QString() : QStringLiteral("-dark");
            QVERIFY(s.window->balloon_dock()->grab().save(dir + QStringLiteral("/gui-palette%1.png").arg(suffix)));
            {  // (fukidashi@2: the palette scrolled to its last looks — the additions —, and the menu of kinds)
                QListWidget* list = s.palette().list();
                list->scrollToItem(list->item(list->count() - 1));
                s.window->arm_balloon_preset(QStringLiteral("heart"));
                QTest::qWait(200);
                QVERIFY(s.window->balloon_dock()->grab().save(dir + QStringLiteral("/gui-palette-additions%1.png").arg(suffix)));
                s.window->arm_balloon_preset(QString());
                QMenu* shapes = s.menu_at(QStringLiteral("ページ/台詞/フキダシの形"));
                shapes->popup(s.window->mapToGlobal(QPoint(400, 120)));
                QTest::qWait(300);
                QVERIFY(shapes->grab().save(dir + QStringLiteral("/gui-kinds%1.png").arg(suffix)));
                shapes->hide();
            }
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
            {  // (fukidashi@2: the panel of a star with the pen's line, drips and a gradient tone — every row of the additions)
                const std::string star = s.put(Json{{"op", "add_line"}, {"text", "やった"}, {"balloon", "star"}, {"x_mm", 40}, {"y_mm", 140}, {"w_mm", 36}, {"h_mm", 40},
                                                    {"style", Json{{"pen_swell", 0.5}, {"pen_taper", 0.6}, {"drip_mm", 6}, {"points", 6},
                                                                   {"fill_tone", Json{{"density", 0.05}, {"lpi", 60.0}, {"angle", 45.0}, {"pattern", "dot"}, {"to", 0.5}, {"dir", 90.0}}}}}});
                s.choose(star);
                QTest::qWait(200);
                QWidget* shot = options != nullptr ? static_cast<QWidget*>(options) : box;
                if (scroll != nullptr) scroll->ensureWidgetVisible(s.panel().fill_blend);
                QTest::qWait(100);
                QVERIFY(shot->grab().save(dir + QStringLiteral("/gui-panel-additions%1.png").arg(suffix)));
            }
            app::settings()->remove(QStringLiteral("ui/theme"));
        }
        app::theme::apply(qApp);
    }

    static Json PageCanvas_tails(const core::StoryLine& l) { return app::PageCanvas::tails_of(l); }
};

QTEST_MAIN(TestGuiFukidashi)
#include "test_gui_fukidashi.moc"
