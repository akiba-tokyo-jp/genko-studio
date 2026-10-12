// The studio's panels in the window (M5④: Python's genko/app/studio_widgets.py, review_model.py, viewer.py, links.py and
// the studio's part of main.py): for a book made with agents only — the approval box over the tabs while something waits
// (each kind of request: its words, its buttons, its preview, its answer as the person: human:<name>), the process bar's
// counts, the panel view (the panel's art, candidates and where they came from, adopting, an instruction, regions drawn
// and taken away), the library, the viewer (fitted, page by page), チャットでの承認, the note and the box in front when
// the AI's requests arrive; a book drawn alone has none of them. And genko:// links: handed over by a later start of this
// build's genko, a style taken from the catalog (a folder of the test's), `genko register-links` in a home of its own.
//
// C++ (SPEC UX-02, improvement I13): the words for the AI in several lines (Enter a new line, the input method's words,
// Ctrl+Enter sends), asked about once more before they go, kept as a draft for each request across another request,
// the book changing under it and another tab, until sent; a draft not dropped without asking.

#include <QtTest>

#include <QApplication>
#include <QKeyEvent>
#include <QBuffer>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
#include <QComboBox>
#include <QDockWidget>
#include <QInputMethodEvent>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSignalSpy>
#include <QTabBar>
#include <QTextEdit>

#include <deque>

#include "app/canvas.hpp"
#include "app/config.hpp"
#include "app/links.hpp"
#include "app/main_window.hpp"
#include "app/reply_box.hpp"
#include "app/review_model.hpp"
#include "app/studio_widgets.hpp"
#include "app/viewer.hpp"
#include "core/json.hpp"
#include "core/paths.hpp"
#include "gui_support.hpp"
#include "render/image.hpp"
#include "render/png.hpp"
#include "service/human_service.hpp"
#include "service/studio_service.hpp"
#include "storage/asset_store.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
namespace fs = std::filesystem;

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

// Words typed into a widget as key presses with their text (QTest::keyClicks takes ASCII only).
void type_in(QWidget* widget, const QString& text) {
    for (const QChar c : text) {
        QKeyEvent press(QEvent::KeyPress, 0, Qt::NoModifier, QString(c));
        QCoreApplication::sendEvent(widget, &press);
        QKeyEvent release(QEvent::KeyRelease, 0, Qt::NoModifier, QString(c));
        QCoreApplication::sendEvent(widget, &release);
    }
}

std::string grey_png(int w, int h, int grey) {
    QImage image(w, h, QImage::Format_RGB888);
    image.fill(QColor(grey, grey, grey));
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes.toStdString();
}

// A book made with agents (create_project), with what an agent puts in it: two characters (ユキ with two sheet
// candidates), a location with its picture, page 1's panel with a brief, two candidates (one adopted) and a face
// region; and, with `requests`, the agent's requests for the person: the name of pages 1 and 2, ユキ's sheet, a
// question on page 1's panel, a layout proposal for page 2 and the export.
struct StudioBook {
    QTemporaryDir tmp;
    fs::path root;
    fs::path path;
    std::vector<std::string> leaves;
    std::map<std::string, std::string> refs;

    service::ToolResult ai(const Json& ops, const std::string& actor = "ai:hermes") const {
        service::StudioService agent(root, actor);
        return agent.call("apply_ops", Json::array({"s.genko", ops}), Json{{"commit", true}});
    }

    explicit StudioBook(bool requests = true) {
        root = gui_test::path_of(tmp.path());
        service::StudioService agent(root, "ai:hermes");
        if (!agent.call("create_project", Json::array({"s.genko", "スタジオ", 3}), Json::object()).ok) qFatal("create_project");
        path = root / "s.genko";
        storage::AssetStore store(path);
        refs = {{"S1", store.put_bytes(grey_png(40, 60, 90), ".png")}, {"S2", store.put_bytes(grey_png(40, 60, 170), ".png")},
                {"A1", store.put_bytes(grey_png(80, 60, 120), ".png")}, {"A2", store.put_bytes(grey_png(80, 60, 200), ".png")},
                {"L1", store.put_bytes(grey_png(48, 32, 60), ".png")}};
        const core::Document doc = gui_test::read_book(path);
        for (const auto& page : doc.pages) leaves.push_back(page->leaf_frames().at(0)->id);
        const core::Rect r = doc.page(0).leaf_frames().at(0)->rect;
        const double x = r.x.value() + 10, y = r.y.value() + 10;
        const Json made = Json::array({
            Json{{"op", "upsert_character"}, {"character", Json{{"id", "c-yuki"}, {"name", "ユキ"}, {"look", Json{{"hair", "黒髪のボブ"}, {"eyes", "大きい"}}}}}},
            Json{{"op", "upsert_character"}, {"character", Json{{"id", "c-ken"}, {"name", "ケン"}}}},
            Json{{"op", "upsert_location"}, {"location", Json{{"id", "loc-roof"}, {"name", "屋上"}}}},
            Json{{"op", "attach_reference"}, {"target", Json{{"location_id", "loc-roof"}}}, {"asset", refs["L1"]}, {"kind", "location"}},
            Json{{"op", "import_candidates"}, {"character_id", "c-yuki"},
                 {"candidates", Json::array({Json{{"id", "cd_y1"}, {"asset", refs["S1"]}, {"px", {40, 60}}, {"origin", Json{{"kind", "agent"}}}},
                                             Json{{"id", "cd_y2"}, {"asset", refs["S2"]}, {"px", {40, 60}}, {"origin", Json{{"kind", "agent"}}}}})}},
            Json{{"op", "set_panel"}, {"page", 1}, {"frame_id", leaves[0]},
                 {"set", Json{{"shot", "MS"}, {"angle", "eye"}, {"action", "鞄を拾う"}, {"emotion", "驚き"}}}},
            Json{{"op", "open_request"}, {"request", Json{{"id", "rq_1"}, {"purpose", "art"}, {"mode", "new"}, {"target", Json{{"page", 1}, {"frame_id", leaves[0]}}}}}},
            Json{{"op", "import_candidates"}, {"request_id", "rq_1"},
                 {"candidates", Json::array({Json{{"id", "cd_a1"}, {"asset", refs["A1"]}, {"px", {80, 60}},
                                                  {"origin", Json{{"kind", "agent"}, {"tool_id", "openai:gpt-image-1"}, {"prompt", "a girl"}}}},
                                             Json{{"id", "cd_a2"}, {"asset", refs["A2"]}, {"px", {80, 60}}, {"origin", Json{{"kind", "agent"}}}, {"parent", "cd_a1"}}})}},
            Json{{"op", "review_candidates"}, {"page", 1}, {"frame_id", leaves[0]}, {"reviews", Json::array({Json{{"candidate_id", "cd_a1"}, {"score", 0.8}, {"note", "良い"}}})}},
            Json{{"op", "adopt_candidate"}, {"page", 1}, {"frame_id", leaves[0]}, {"candidate_id", "cd_a1"}},
            Json{{"op", "add_region"}, {"page", 1}, {"frame_id", leaves[0]}, {"region", Json{{"id", "rg_f"}, {"kind", "face"}, {"rect_mm", {x, y, 20, 22}}, {"char", "c-yuki"}}}},
        });
        // (the agent's art waits for the person's approval of the name: page 1's is approved first)
        const Json adopted = Json::array({made[9], made[10]});
        Json before = made;
        before.erase(10);
        before.erase(9);
        const auto first = ai(before);
        if (!first.ok) qFatal("studio book: %s", first.to_dict().dump().c_str());
        service::HumanService(path, "human:作者").approve_name(Json::array({1}));
        const auto art = ai(adopted);
        if (!art.ok) qFatal("studio book's art: %s", art.to_dict().dump().c_str());
        if (!requests) return;
        const Json asked = Json::array({
            Json{{"op", "request_approval"}, {"gate", "name"}, {"pages", {1, 2}}, {"note", "ネームを見てください"}},
            Json{{"op", "request_approval"}, {"gate", "sheet"}, {"character_id", "c-yuki"}},
            Json{{"op", "ask_human"}, {"text", "鞄の色は紺でいいですか"}, {"page", 1}, {"frame_id", leaves[0]}},
            Json{{"op", "propose"}, {"proposal", Json{{"id", "pr_l"}, {"kind", "layout"}, {"page", 2}, {"tree", Json{{"rect_mm", {20, 25, 150, 230}}}}}}},
            Json{{"op", "request_approval"}, {"gate", "export"}},
        });
        const auto second = ai(asked);
        if (!second.ok) qFatal("requests: %s", second.to_dict().dump().c_str());
    }
};

// The window on a book; its questions answered by the test (a yes or no for each in turn; none left: no), its dialogs
// by a function.
struct Desk {
    std::shared_ptr<app::Session> session;
    std::unique_ptr<app::MainWindow> window;
    gui_test::Answers answers;
    std::deque<bool> yes;
    QStringList questions;
    QStringList warnings;
    std::function<int(QDialog*)> on_dialog;

    Desk(const fs::path& book, const fs::path& recovery) {
        session = app::Session::open(book, gui_test::quick(recovery));
        window = std::make_unique<app::MainWindow>(session);
        window->resize(1366, 900);
        window->show();
        answers.responder->question = [this](const QString& title, const QString& text) {
            questions << title + QStringLiteral(": ") + text;
            if (yes.empty()) return false;
            const bool answer = yes.front();
            yes.pop_front();
            return answer;
        };
        answers.responder->warning = [this](const QString& title, const QString& text) { warnings << title + QStringLiteral(": ") + text; };
        answers.responder->exec = [this](QDialog* dialog) -> int { return on_dialog ? on_dialog(dialog) : QDialog::Rejected; };
    }
    ~Desk() {
        window.reset();
        session.reset();
    }
    app::ApprovalBox* box() const { return window->approvals(); }
    QDockWidget* dock(const QString& title) const {
        for (QDockWidget* d : window->findChildren<QDockWidget*>()) {
            if (d->windowTitle() == title) return d;
        }
        return nullptr;
    }
    // The request in the box with this title chosen.
    bool choose(const QString& title) {
        const auto& items = box()->items();
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (qs(items[i].title()) == title) {
                box()->list->setCurrentRow(static_cast<int>(i));
                return true;
            }
        }
        return false;
    }
    QStringList titles() const {
        QStringList out;
        for (const auto& item : box()->items()) out << qs(item.title());
        return out;
    }
    bool saved() { return session->wait_saved(std::chrono::seconds(30)); }
};

// The title of the request chosen in the box ("" when none is).
QString current_title(const Desk& desk) {
    const auto item = desk.box()->current();
    return item ? QString::fromStdString(item->title()) : QString();
}

const Json* ticket_with(const core::Document& doc, const std::string& kind, const std::function<bool(const Json&)>& also = {}) {
    for (const Json& t : doc.tickets) {
        if (t.value("kind", Json()) == Json(kind) && (!also || also(t))) return &t;
    }
    return nullptr;
}

}  // namespace

class TestGuiStudio : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        (void)gui_test::config_folder();
        qRegisterMetaType<app::BookChange>();
        // (the links' local socket under a name of the test's own: never a Genko the person has open)
        qputenv("LOGNAME", QByteArray("genkotest") + QByteArray::number(QCoreApplication::applicationPid()));
    }

    // A book drawn alone has no studio: no box, no panel view, no library, no process bar; チャットでの承認 is in ページ.
    void plainBookHasNoStudio() {
        QTemporaryDir tmp;
        const fs::path book = gui_test::path_of(tmp.path() + QStringLiteral("/plain.genko"));
        gui_test::make_book(book);
        Desk desk(book, gui_test::path_of(tmp.path() + QStringLiteral("/recovery")));
        for (const QString& title : {QStringLiteral("承認箱"), QStringLiteral("コマの詳細"), QStringLiteral("資料")}) {
            QDockWidget* d = desk.dock(title);
            QVERIFY2(d != nullptr, qPrintable(title));
            QVERIFY(!d->isVisible());
            QVERIFY(!d->toggleViewAction()->isVisible());
        }
        QVERIFY(!desk.window->process_bar()->isVisible());
        QVERIFY(desk.window->ai_button()->isVisible());
        QCOMPARE(desk.window->ai_button()->text(), QStringLiteral("AI と作る"));
        QStringList pages;
        for (QAction* top : desk.window->menuBar()->actions()) {
            if (top->menu() == nullptr || top->menu()->title() != QStringLiteral("ページ")) continue;
            for (QAction* a : top->menu()->actions()) pages << a->text();
        }
        const auto paper = pages.indexOf(QStringLiteral("原稿用紙の設定…"));
        QVERIFY(paper >= 0);
        QCOMPARE(pages.value(paper + 1), QStringLiteral("チャットでの承認…"));
        // an agent starts working on it: the studio comes, and its request is shown
        service::StudioService ai(gui_test::path_of(tmp.path()), "ai:hermes");
        QVERIFY(ai.call("apply_ops", Json::array({"plain.genko", Json::array({Json{{"op", "set_studio"}, {"premise", "屋上の話"}}})}), Json{{"commit", true}}).ok);
        QVERIFY(ai.call("request_approval", Json::array(), Json{{"project", "plain.genko"}, {"gate", "name"}, {"pages", {1}}}).ok);
        desk.session->check_outside();
        QVERIFY(gui_test::wait_for([&] { return desk.dock(QStringLiteral("承認箱"))->isVisible(); }, 60000));
        QVERIFY(gui_test::wait_for([&] { return desk.window->process_bar()->isVisible(); }, 60000));
        QVERIFY(desk.dock(QStringLiteral("コマの詳細"))->toggleViewAction()->isVisible());
    }

    // The box and the bar: each request's words and buttons as Python's; the counts.
    void boxAndBar() {
        StudioBook book;
        Desk desk(book.path, book.root / "recovery");
        QVERIFY(gui_test::wait_for([&] { return desk.dock(QStringLiteral("承認箱"))->isVisible(); }, 60000));
        QCOMPARE(desk.titles(), (QStringList{QStringLiteral("ネームの承認: 1, 2 ページ"), QStringLiteral("設定画の承認: ユキ"), QStringLiteral("相談（1 ページ）"),
                                            QStringLiteral("書き出しの承認"), QStringLiteral("アタリからの提案: 2 ページのコマ割り")}));
        QCOMPARE(desk.box()->count->text(), QStringLiteral("5"));
        QStringList chips;
        for (QLabel* label : desk.window->process_bar()->labels()) chips << label->text();
        QCOMPARE(chips.value(0), QStringLiteral("承認箱 <b>5</b>"));
        QVERIFY2(chips.contains(QStringLiteral("設定画未承認 <b>2</b>")), qPrintable(chips.join(QLatin1Char('|'))));
        QVERIFY(chips.contains(QStringLiteral("ネーム未提出 <b>2</b>")));
        QVERIFY(chips.contains(QStringLiteral("作画承認待ち <b>1</b>")));
        // the bar's chip opens the box
        desk.dock(QStringLiteral("承認箱"))->hide();
        QMouseEvent click(QEvent::MouseButtonRelease, QPointF(2, 2), QPointF(2, 2), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(desk.window->process_bar()->labels().front(), &click);
        QVERIFY(desk.dock(QStringLiteral("承認箱"))->isVisible());
        struct Kind {
            QString title, approve, back, placeholder;
            bool back_shown;
        };
        const Kind kinds[] = {
            {QStringLiteral("ネームの承認: 1, 2 ページ"), QStringLiteral("承認"), QStringLiteral("差し戻す"), QStringLiteral("差し戻す理由（AI への指示になる）"), true},
            {QStringLiteral("相談（1 ページ）"), QStringLiteral("返事なしで閉じる"), QStringLiteral("返事を送る"), QStringLiteral("返事（AI への指示になる）"), true},
            {QStringLiteral("アタリからの提案: 2 ページのコマ割り"), QStringLiteral("確定"), QStringLiteral("却下"), QStringLiteral("却下の理由"), true},
            {QStringLiteral("書き出しの承認"), QStringLiteral("書き出す…"), QStringLiteral("差し戻す"), QString(), false},
        };
        for (const Kind& k : kinds) {
            QVERIFY(desk.choose(k.title));
            QCOMPARE(desk.box()->approve_button->text(), k.approve);
            QCOMPARE(desk.box()->back_button->text(), k.back);
            QCOMPARE(desk.box()->back_button->isVisibleTo(desk.box()), k.back_shown);
            QCOMPARE(desk.box()->reason->isVisibleTo(desk.box()), k.back_shown);
            if (k.back_shown) QCOMPARE(desk.box()->reason->placeholderText(), k.placeholder);
            QVERIFY(desk.box()->detail->text().contains(QStringLiteral("依頼: AI（hermes）")));
        }
        // a name over two pages: the pages to pick, the page shown and the window following it
        QVERIFY(desk.choose(QStringLiteral("ネームの承認: 1, 2 ページ")));
        QVERIFY(desk.box()->page_pick->isVisibleTo(desk.box()));
        QCOMPARE(desk.box()->page_pick->count(), 2);
        QVERIFY(desk.box()->detail->text().startsWith(QStringLiteral("ネームを見てください<br>")));
        QVERIFY(desk.box()->detail->toolTip().startsWith(QStringLiteral("コマ割り・読み順")));
        QVERIFY(desk.box()->preview->has_image());
        desk.box()->page_pick->setCurrentIndex(1);
        QCOMPARE(desk.window->page_index(), 1);
        // the export's check: why it stops, and no approving it from here
        QVERIFY(desk.choose(QStringLiteral("書き出しの承認")));
        QVERIFY2(desk.box()->preview->text().startsWith(QStringLiteral("止めている理由:\n・")), qPrintable(desk.box()->preview->text()));
        QVERIFY(!desk.box()->approve_button->isEnabled());
        QVERIFY(!desk.box()->preview->has_image());
        // the viewer, page by page
        QVERIFY(desk.choose(QStringLiteral("ネームの承認: 1, 2 ページ")));
        desk.box()->page_pick->setCurrentIndex(0);
        QStringList seen;
        desk.on_dialog = [&](QDialog* dialog) {
            auto* viewer = qobject_cast<app::ViewerDialog*>(dialog);
            if (viewer == nullptr) return int(QDialog::Rejected);
            seen << viewer->windowTitle() << viewer->page_label()->text();
            viewer->step(1);
            seen << viewer->page_label()->text();
            viewer->step(1);  // (no page after the last)
            seen << viewer->page_label()->text();
            seen << QString::number(viewer->view()->scene()->items().size());  // (the page and its caption)
            return int(QDialog::Accepted);
        };
        desk.box()->big->click();
        QCOMPARE(seen, (QStringList{QStringLiteral("ネームの承認: 1, 2 ページ"), QStringLiteral("1 ページ（1 / 2）"), QStringLiteral("2 ページ（2 / 2）"),
                                   QStringLiteral("2 ページ（2 / 2）"), QStringLiteral("2")}));
    }

    // 承認 as the person: the name of both pages approved, the request closed, written at once.
    void approveName() {
        StudioBook book;
        Desk desk(book.path, book.root / "recovery");
        QVERIFY(desk.choose(QStringLiteral("ネームの承認: 1, 2 ページ")));
        desk.box()->approve_button->click();
        QVERIFY(desk.saved());
        const core::Document saved = gui_test::read_book(book.path);
        QVERIFY(saved.page(0).name_ok && saved.page(1).name_ok && !saved.page(2).name_ok);
        const Json* gate = ticket_with(saved, "gate", [](const Json& t) { return t.value("gate", Json()) == Json("name"); });
        QVERIFY(gate != nullptr);
        QCOMPARE(gate->value("status", Json()), Json("done"));
        QCOMPARE(gate->value("closed_by", Json()), Json("human:tester"));
        QVERIFY(gui_test::wait_for([&] { return desk.box()->items().size() == 4; }, 60000));
    }

    // 差し戻し in several lines (I13): Enter makes a line, the input method's words go in, the words are shown once more
    // before they go (no: nothing sent, the draft kept), the draft back on the request after another one and after the
    // book changed under it; Ctrl+Enter sends; an empty reason refused before asking.
    void sendBackInLines() {
        StudioBook book;
        Desk desk(book.path, book.root / "recovery");
        app::ReplyEdit* reason = desk.box()->reason;
        QVERIFY(desk.choose(QStringLiteral("ネームの承認: 1, 2 ページ")));
        desk.box()->back_button->click();  // (nothing written)
        QCOMPARE(desk.warnings, (QStringList{QStringLiteral("Genko: 理由（返事）を書いてから送ります")}));
        QVERIFY(desk.questions.isEmpty());
        reason->setFocus();
        type_in(reason, QStringLiteral("コマ割りを"));
        QTest::keyClick(reason, Qt::Key_Return);
        QInputMethodEvent typing(QStringLiteral("かいぎょう"), {});
        QCoreApplication::sendEvent(reason, &typing);
        QTest::keyClick(reason, Qt::Key_Return, Qt::ControlModifier);  // (while converting: nothing is sent)
        QVERIFY(desk.questions.isEmpty());
        QInputMethodEvent settled;
        settled.setCommitString(QStringLiteral("改行"));
        QCoreApplication::sendEvent(reason, &settled);
        QTest::keyClick(reason, Qt::Key_Return);
        type_in(reason, QStringLiteral("して直して"));
        const QString words = QStringLiteral("コマ割りを\n改行\nして直して");
        QCOMPARE(reason->toPlainText(), words);
        // asked, and declined: nothing sent, the words kept
        desk.box()->back_button->click();
        QCOMPARE(desk.questions.size(), 1);
        QVERIFY(desk.questions.value(0).startsWith(QStringLiteral("差し戻す: AI（hermes） に、次の言葉を送ります。\n\n") + words));
        QCOMPARE(reason->toPlainText(), words);
        QCOMPARE(desk.box()->items().size(), std::size_t(5));
        // another request, then back: the draft comes back; the other request's box is empty
        QVERIFY(desk.choose(QStringLiteral("相談（1 ページ）")));
        QVERIFY(reason->toPlainText().isEmpty());
        type_in(reason, QStringLiteral("紺で"));
        QVERIFY(desk.choose(QStringLiteral("ネームの承認: 1, 2 ページ")));
        QCOMPARE(reason->toPlainText(), words);
        // the agent writes the book meanwhile: the box is read again, the draft stays
        QVERIFY(book.ai(Json::array({Json{{"op", "ask_human"}, {"text", "もう一つ"}, {"page", 2}}})).ok);
        desk.session->check_outside();
        QVERIFY(gui_test::wait_for([&] { return desk.box()->items().size() == 6; }, 60000));
        QCOMPARE(current_title(desk), QStringLiteral("ネームの承認: 1, 2 ページ"));
        QCOMPARE(reason->toPlainText(), words);
        QCOMPARE(app::Drafts::instance().count(desk.box()->book_key()), 2);
        // sent with Ctrl+Enter: the request returned, the reason in its lines for each page
        desk.yes = {true};
        reason->setFocus();
        QTest::keyClick(reason, Qt::Key_Return, Qt::ControlModifier);
        QCOMPARE(desk.questions.size(), 2);
        QVERIFY(desk.saved());
        const core::Document saved = gui_test::read_book(book.path);
        const Json* gate = ticket_with(saved, "gate", [](const Json& t) { return t.value("gate", Json()) == Json("name"); });
        QCOMPARE(gate->value("status", Json()), Json("returned"));
        int fixes = 0;
        for (const Json& t : saved.tickets) {
            if (t.value("kind", Json()) == Json("fix") && t.value("text", Json()) == Json(words.toStdString())) ++fixes;
        }
        QCOMPARE(fixes, 2);
        QVERIFY(reason->toPlainText().isEmpty() || current_title(desk) != QStringLiteral("ネームの承認: 1, 2 ページ"));
        QCOMPARE(app::Drafts::instance().count(desk.box()->book_key()), 1);  // (the reply to the question is still kept)
    }

    // 返事なしで閉じる with a reply written and not sent: asked first; the draft goes only when it is closed.
    void closeQuestionWithDraft() {
        StudioBook book;
        Desk desk(book.path, book.root / "recovery");
        QVERIFY(desk.choose(QStringLiteral("相談（1 ページ）")));
        type_in(desk.box()->reason, QStringLiteral("書きかけ"));
        desk.box()->approve_button->click();
        QCOMPARE(desk.questions.size(), 1);
        QVERIFY(desk.questions.value(0).startsWith(QStringLiteral("返事なしで閉じる: 書きかけの返事があります")));
        QCOMPARE(desk.box()->items().size(), std::size_t(5));
        desk.yes = {true};
        desk.box()->approve_button->click();
        QVERIFY(desk.saved());
        const core::Document saved = gui_test::read_book(book.path);
        QCOMPARE(ticket_with(saved, "help")->value("status", Json()), Json("done"));
        QCOMPARE(app::Drafts::instance().count(desk.box()->book_key()), 0);
    }

    // A reply sent to a question: its words reach the agent as an instruction for the panel.
    void replyToQuestion() {
        StudioBook book;
        Desk desk(book.path, book.root / "recovery");
        QVERIFY(desk.choose(QStringLiteral("相談（1 ページ）")));
        QVERIFY(desk.box()->back_button->property("primary").toBool());
        type_in(desk.box()->reason, QStringLiteral("紺で"));
        QTest::keyClick(desk.box()->reason, Qt::Key_Return);
        type_in(desk.box()->reason, QStringLiteral("お願いします"));
        desk.yes = {true};
        desk.box()->back_button->click();
        QVERIFY(desk.questions.value(0).startsWith(QStringLiteral("返事を送る: AI（hermes） に、次の言葉を送ります。")));
        QVERIFY(desk.saved());
        const core::Document saved = gui_test::read_book(book.path);
        QCOMPARE(ticket_with(saved, "help")->value("status", Json()), Json("done"));
        const Json* fix = ticket_with(saved, "fix", [](const Json& t) { return t.value("text", Json()) == Json("紺で\nお願いします"); });
        QVERIFY(fix != nullptr);
        QCOMPARE(fix->value("frame_id", Json()), Json(book.leaves[0]));
        QCOMPARE(fix->value("scope", Json()), Json("frame"));
    }

    // A sheet: the candidates, the newest first; approved with the one chosen (its face kept as a picture of the book).
    void approveSheet() {
        StudioBook book;
        Desk desk(book.path, book.root / "recovery");
        QVERIFY(desk.choose(QStringLiteral("設定画の承認: ユキ")));
        QVERIFY(desk.box()->choices->isVisibleTo(desk.box()));
        QVERIFY(!desk.box()->preview->isVisibleTo(desk.box()));
        QCOMPARE(desk.box()->choices->count(), 2);
        QCOMPARE(desk.box()->choices->item(0)->text(), QStringLiteral("候補 2"));
        QCOMPARE(desk.box()->choices->item(1)->text(), QStringLiteral("候補 1"));
        QVERIFY(!desk.box()->choices->item(0)->icon().isNull());
        desk.box()->choices->setCurrentRow(1);
        desk.box()->approve_button->click();
        QVERIFY(desk.saved());
        const core::Document saved = gui_test::read_book(book.path);
        const Json& yuki = saved.bible.characters[0];
        QCOMPARE(yuki.value("locked", Json()), Json(true));
        bool face = false;
        for (const Json& ref : yuki["refs"]) {
            if (ref.value("kind", Json()) == Json("face")) face = storage::AssetStore(book.path).has(ref["asset"].get<std::string>(), ".png");
        }
        QVERIFY(face);
    }

    // A proposal: 確定 makes its layout; 却下 needs a reason.
    void proposal() {
        StudioBook book;
        Desk desk(book.path, book.root / "recovery");
        QVERIFY(desk.choose(QStringLiteral("アタリからの提案: 2 ページのコマ割り")));
        QVERIFY(desk.box()->preview->has_image());
        desk.box()->approve_button->click();
        QVERIFY(desk.saved());
        const core::Document saved = gui_test::read_book(book.path);
        QCOMPARE(saved.studio["proposals"]["pr_l"]["status"], Json("accepted"));
        QCOMPARE(saved.page(1).leaf_frames().at(0)->rect.x.value(), 20.0);
    }

    // The panel view: the panel's art and brief, its candidates and where they came from, adopting another, an
    // instruction in lines (asked before it goes), a region drawn and one taken away.
    void panelView() {
        StudioBook book(false);
        Desk desk(book.path, book.root / "recovery");
        QDockWidget* dock = desk.dock(QStringLiteral("コマの詳細"));
        QVERIFY(dock->toggleViewAction()->isChecked());
        dock->raise();
        QVERIFY(gui_test::wait_for([&] { return dock->isVisible(); }, 60000));
        app::PanelView* view = desk.window->panel_view();
        QVERIFY(view->info->text().startsWith(QStringLiteral("編集画面でコマをクリックすると")));
        emit desk.window->canvas()->frameSelected(qs(book.leaves[0]));
        QVERIFY(gui_test::wait_for([&] { return view->candidates->count() == 2; }, 60000));
        QVERIFY2(view->info->text().startsWith(QStringLiteral("<b>1 コマ目</b>　採用済み　取り込んだ絵")), qPrintable(view->info->text()));
        QVERIFY(view->info->text().contains(QStringLiteral("鞄を拾う / 驚き")));
        QCOMPARE(view->candidates->item(0)->text(), QStringLiteral("候補 1 ✔採用\n評価 0.8"));
        QCOMPARE(view->candidates->item(1)->text(), QStringLiteral("候補 2"));
        QCOMPARE(view->regions->count(), 1);
        QCOMPARE(view->regions->item(0)->text(), QStringLiteral("顔 c-yuki（AI）"));
        QVERIFY(!view->image->pixmap().isNull());
        QCOMPARE(view->image->boxes.size(), std::size_t(1));
        view->candidates->setCurrentRow(0);
        QVERIFY2(view->provenance->toPlainText().startsWith(QStringLiteral("作った道具: openai:gpt-image-1 \nid: cd_a1　元にした候補: なし")),
                 qPrintable(view->provenance->toPlainText()));
        QVERIFY(view->provenance->toPlainText().contains(QStringLiteral("プロンプト: a girl")));
        QVERIFY(view->provenance->toPlainText().contains(QStringLiteral("評価（AI（hermes））: 0.8 良い")));
        view->mode->setCurrentIndex(2);  // 比較
        QVERIFY(!view->image->pixmap().isNull());
        view->mode->setCurrentIndex(3);  // 元画像
        QVERIFY(!view->image->pixmap().isNull());
        // adopt the other
        view->candidates->setCurrentRow(1);
        view->adopt();
        QVERIFY(desk.saved());
        QCOMPARE(gui_test::read_book(book.path).page(0).leaf_frames().at(0)->panel->at("adopted").at("art"), Json("cd_a2"));
        // an instruction in two lines, asked first
        view->instruction->setFocus();
        type_in(view->instruction, QStringLiteral("顔を"));
        QTest::keyClick(view->instruction, Qt::Key_Return);
        type_in(view->instruction, QStringLiteral("明るく"));
        QTest::keyClick(view->instruction, Qt::Key_Return, Qt::ControlModifier);
        QCOMPARE(desk.questions.size(), 1);  // (declined: kept)
        QCOMPARE(view->instruction->toPlainText(), QStringLiteral("顔を\n明るく"));
        desk.yes = {true};
        view->send_button->click();
        QVERIFY(desk.saved());
        {
            const core::Document saved = gui_test::read_book(book.path);
            const Json& panel = *saved.page(0).leaf_frames().at(0)->panel;
            QCOMPARE(panel["instruction"]["text"], Json("顔を\n明るく"));
            QCOMPARE(panel["instruction"]["by"], Json("human:tester"));
            QVERIFY(ticket_with(saved, "fix", [](const Json& t) { return t.value("text", Json()) == Json("顔を\n明るく"); }) != nullptr);
        }
        QVERIFY(gui_test::wait_for([&] { return view->info->text().contains(QStringLiteral("指示: 顔を<br>明るく")); }, 60000));
        QVERIFY(view->instruction->toPlainText().isEmpty());
        // a region drawn on the panel as it prints, then the AI's taken away
        view->region_kind->setCurrentIndex(2);  // 空けておく
        view->region_button->setChecked(true);
        QCOMPARE(view->mode->currentIndex(), 1);
        QVERIFY(gui_test::wait_for([&] { return !view->image->picture_rect().isEmpty(); }, 60000));
        const QRectF area = view->image->picture_rect();
        const QPointF from = area.topLeft() + QPointF(area.width() * 0.1, area.height() * 0.2);
        const QPointF to = area.topLeft() + QPointF(area.width() * 0.5, area.height() * 0.6);
        QMouseEvent press(QEvent::MouseButtonPress, from, view->image->mapToGlobal(from), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(view->image, &press);
        QMouseEvent move(QEvent::MouseMove, to, view->image->mapToGlobal(to), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(view->image, &move);
        QMouseEvent release(QEvent::MouseButtonRelease, to, view->image->mapToGlobal(to), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(view->image, &release);
        QVERIFY(!view->region_button->isChecked());
        QVERIFY(desk.saved());
        {
            const core::Document saved = gui_test::read_book(book.path);
            const core::Frame& frame = *saved.page(0).leaf_frames().at(0);
            const Json& regions = (*frame.panel)["regions"];
            QCOMPARE(regions.size(), std::size_t(2));
            const Json& drawn = regions[1];
            QCOMPARE(drawn["kind"], Json("keep"));
            QCOMPARE(drawn["source"], Json("user"));
            const double x = drawn["rect_mm"][0].get<double>(), w = drawn["rect_mm"][2].get<double>();
            QVERIFY(std::abs(x - (frame.rect.x.value() + 0.1 * frame.rect.width.value())) < 1.0);
            QVERIFY(std::abs(w - 0.4 * frame.rect.width.value()) < 1.0);
        }
        QVERIFY(gui_test::wait_for([&] { return view->regions->count() == 2; }, 60000));
        view->regions->setCurrentRow(0);
        view->delete_region();
        QVERIFY(desk.saved());
        QCOMPARE((*gui_test::read_book(book.path).page(0).leaf_frames().at(0)->panel)["regions"].size(), std::size_t(1));
        // the candidates side by side
        QStringList captions;
        desk.on_dialog = [&](QDialog* dialog) {
            if (auto* viewer = qobject_cast<app::ViewerDialog*>(dialog)) {
                for (QGraphicsItem* item : viewer->view()->scene()->items()) {
                    if (auto* text = dynamic_cast<QGraphicsSimpleTextItem*>(item)) captions << text->text();
                }
            }
            return int(QDialog::Accepted);
        };
        view->compare_candidates();
        captions.sort();
        QCOMPARE(captions, (QStringList{QStringLiteral("候補 1"), QStringLiteral("候補 2 ✔採用")}));
    }

    // The library: characters and the location with their pictures and words.
    void library() {
        StudioBook book(false);
        Desk desk(book.path, book.root / "recovery");
        desk.window->show_dock(QStringLiteral("資料"));
        app::Library* lib = desk.window->library();
        QVERIFY(gui_test::wait_for([&] { return lib->list->count() == 3; }, 60000));
        QCOMPARE(lib->list->item(0)->text(), QStringLiteral("ユキ\n設定画 未承認"));
        QCOMPARE(lib->list->item(2)->text(), QStringLiteral("屋上\n場所"));
        QVERIFY(!lib->list->item(2)->icon().isNull());
        lib->list->setCurrentRow(0);
        QCOMPARE(lib->detail->toPlainText(), QStringLiteral("人物: ユキ（c-yuki）\n見た目: 黒髪のボブ、大きい"));
        lib->list->setCurrentRow(2);
        QVERIFY(lib->detail->toPlainText().startsWith(QStringLiteral("場所: 屋上（loc-roof）")));
    }

    // チャットでの承認: stopped for this book, after the question.
    void chatApproval() {
        StudioBook book(false);
        Desk desk(book.path, book.root / "recovery");
        desk.window->action(QStringLiteral("act_chat_approval"))->trigger();
        QCOMPARE(desk.questions.size(), 1);
        QVERIFY(desk.questions.value(0).startsWith(QStringLiteral("チャットでの承認: いまは、チャットで AI に承認を伝えると")));
        QVERIFY(service::chat_approval_open(desk.window->book()));  // (declined: unchanged)
        desk.yes = {true};
        desk.window->action(QStringLiteral("act_chat_approval"))->trigger();
        QVERIFY(!service::chat_approval_open(desk.window->book()));
        QCOMPARE(desk.window->last_notice(), QStringLiteral("この原稿ではチャットでの承認を止めました"));
        desk.yes = {true};
        desk.window->action(QStringLiteral("act_chat_approval"))->trigger();
        QVERIFY(desk.questions.last().contains(QStringLiteral("この原稿では、チャットでの承認を止めています。")));
        QVERIFY(service::chat_approval_open(desk.window->book()));
    }

    // The AI's requests arriving while the person works: a note, the box in front with the newest chosen.
    void requestsArrive() {
        StudioBook book(false);
        Desk desk(book.path, book.root / "recovery");
        QVERIFY(!desk.dock(QStringLiteral("承認箱"))->isVisible());
        QVERIFY(book.ai(Json::array({Json{{"op", "request_approval"}, {"gate", "name"}, {"pages", {3}}},
                                     Json{{"op", "ask_human"}, {"text", "どちらがいいですか"}, {"page", 2}}})).ok);
        desk.session->check_outside();
        QVERIFY(gui_test::wait_for([&] { return desk.box()->items().size() == 2; }, 60000));
        QVERIFY(desk.dock(QStringLiteral("承認箱"))->isVisible());
        QLabel* note = desk.window->canvas()->findChild<QLabel*>(QStringLiteral("toast"));
        QVERIFY(note != nullptr);
        QCOMPARE(note->text(), QStringLiteral("承認の依頼が届きました: 相談（2 ページ） ほか 1 件"));
        QCOMPARE(current_title(desk), QStringLiteral("相談（2 ページ）"));
        // another tab and back: the requests already there are not new
        delete note;
        QVERIFY(gui_test::wait_for([&] { return desk.window->canvas()->findChild<QLabel*>(QStringLiteral("toast")) == nullptr; }, 60000));
    }

    // genko:// links: one handed over by a later start of this build's genko; one the window does not know; a style from
    // the catalog (a folder of the test's) taken after the question.
    void links() {
        StudioBook book(false);
        Desk desk(book.path, book.root / "recovery");
        // (the local socket's file goes in the temporary folder, whose path must be short for a socket: one of the test's)
        QTemporaryDir sockets(QStringLiteral("/tmp/gkXXXXXX"));
        QVERIFY(sockets.isValid());
        const QByteArray tmpdir = qgetenv("TMPDIR");
        qputenv("TMPDIR", sockets.path().toUtf8());
        struct Restore {
            QByteArray value;
            ~Restore() { qputenv("TMPDIR", value); }
        } restore{tmpdir};
        app::links::Listener listener;
        QVERIFY(listener.listening());
        QSignalSpy received(&listener, &app::links::Listener::received);
        QTemporaryDir config;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("GENKO_CONFIG_DIR"), config.path());
        env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
        QProcess later;
        later.setProcessEnvironment(env);
        later.start(genko::test::genko_cli(), {QStringLiteral("app"), QStringLiteral("genko://use-style?id=gag-doodle")});
        QVERIFY(later.waitForStarted(60000));
        QVERIFY(gui_test::wait_for([&] { return received.count() == 1; }, 120000));
        QVERIFY(gui_test::wait_for([&] { return later.state() == QProcess::NotRunning; }, 120000));
        QCOMPARE(later.exitCode(), 0);  // (it handed the link over and ended)
        QCOMPARE(received.at(0).at(0).toString(), QStringLiteral("genko://use-style?id=gag-doodle"));
        // a link the window does not know
        desk.window->open_link(QStringLiteral("genko://other"));
        QCOMPARE(desk.window->last_error(), QStringLiteral("Genko の知らないリンクです: genko://other"));
        // the style: asked, then kept in the book with its words
        QTemporaryDir site;
        const QString catalog = site.path() + QStringLiteral("/catalog");
        QDir().mkpath(catalog + QStringLiteral("/v1/styles"));
        genko::test::write_bytes(catalog + QStringLiteral("/v1/tree"),
                                 R"({"nodes": [{"id": "gag", "parent": null, "level": 1, "title": "ギャグ漫画"}, {"id": "gag-doodle", "parent": "gag", "level": 2, "title": "ゆるい落書き風"}]})");
        genko::test::write_bytes(catalog + QStringLiteral("/v1/styles/gag-doodle"),
                                 R"({"id": "gag-doodle", "title": "ゆるい落書き風", "level": 2, "expression": "mono", "path": [{"id": "gag", "title": "ギャグ漫画"}],)"
                                 R"( "prompt": {"ja": "震える細い線", "en": "shaky lines"}, "avoid": [], "samples": [], "version": 3})");
        qputenv("GENKO_STYLE_CATALOG", (QStringLiteral("file://") + catalog).toUtf8());
        desk.yes = {true};
        desk.window->open_link(received.at(0).at(0).toString());
        qunsetenv("GENKO_STYLE_CATALOG");
        QCOMPARE(desk.questions.size(), 1);
        QCOMPARE(desk.questions.value(0), QStringLiteral("絵柄を選ぶ: 「スタジオ」の絵柄を「ギャグ漫画 › ゆるい落書き風」にしますか？"));
        QCOMPARE(desk.window->book().studio["style"]["catalog"]["id"], Json("gag-doodle"));
        QVERIFY(desk.window->last_notice().startsWith(QStringLiteral("絵柄を「ギャグ漫画 › ゆるい落書き風」にしました（版 3）")));
    }

    // `genko register-links` in a home of its own: the desktop entry, then xdg-mime's default (a stand-in), or why not.
    void registerLinks() {
        QTemporaryDir home;
        QDir().mkpath(home.path() + QStringLiteral("/bin"));
        const QString record = home.path() + QStringLiteral("/xdg-mime.args");
        genko::test::write_bytes(home.path() + QStringLiteral("/bin/xdg-mime"), ("#!/bin/sh\necho \"$@\" > '" + record.toStdString() + "'\n"));
        QFile::setPermissions(home.path() + QStringLiteral("/bin/xdg-mime"), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        QProcessEnvironment env;
        env.insert(QStringLiteral("HOME"), home.path());
        env.insert(QStringLiteral("XDG_DATA_HOME"), home.path() + QStringLiteral("/data"));
        env.insert(QStringLiteral("XDG_CONFIG_HOME"), home.path() + QStringLiteral("/config"));
        env.insert(QStringLiteral("PATH"), home.path() + QStringLiteral("/bin"));
        const auto done = genko::test::run(genko::test::genko_cli(), {QStringLiteral("register-links")}, env, 120000);
        QVERIFY2(done.finished && done.exit_code == 0, qPrintable(done.described()));
        QCOMPARE(genko::test::one_line(done.out), (Json{{"ok", true}, {"message", "ブラウザの「この絵柄を使う」で Genko が開くようになりました"}}));
        const std::string entry = genko::test::read_bytes(home.path() + QStringLiteral("/data/applications/genko-links.desktop"));
        QCOMPARE(qs(entry), QStringLiteral("[Desktop Entry]\nType=Application\nName=Genko Studio\nNoDisplay=true\nExec=%1 app %u\nMimeType=x-scheme-handler/genko;\n")
                                .arg(QFileInfo(genko::test::genko_cli()).canonicalFilePath()));
        QCOMPARE(qs(genko::test::read_bytes(record)).trimmed(), QStringLiteral("default genko-links.desktop x-scheme-handler/genko"));
        // no xdg-mime: the entry written, the rest left to the person
        env.insert(QStringLiteral("PATH"), home.path() + QStringLiteral("/nothing"));
        const auto alone = genko::test::run(genko::test::genko_cli(), {QStringLiteral("register-links")}, env, 120000);
        QCOMPARE(alone.exit_code, 1);
        const Json said = genko::test::one_line(alone.out);
        QCOMPARE(said["ok"], Json(false));
        QVERIFY(said["message"].get<std::string>().find("xdg-mime が無いので、ブラウザへの登録は手で行ってください") != std::string::npos);
    }
};

QTEST_MAIN(TestGuiStudio)
#include "test_gui_studio.moc"
