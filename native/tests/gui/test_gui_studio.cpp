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
// the book changing under it and another tab, until sent; a draft not dropped without asking. A panel chosen in the
// window while an agent reorders the pages: no conflict holds up the window's saves (SPEC COMP-01a).

#include <QtTest>

#include <QApplication>
#include <QKeyEvent>
#include <QBuffer>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDockWidget>
#include <QInputMethodEvent>
#include <QLabel>
#include <QLineEdit>
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
#include "app/save_status.hpp"
#include "app/studio_widgets.hpp"
#include "app/style_picker.hpp"
#include "app/viewer.hpp"
#include "core/json.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "gui_support.hpp"
#include "render/image.hpp"
#include "render/png.hpp"
#include "service/human_service.hpp"
#include "service/studio_service.hpp"
#include "service/stylecat.hpp"
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

    Desk(const fs::path& book, const fs::path& recovery) : Desk(book, gui_test::quick(recovery)) {}
    Desk(const fs::path& book, const app::Session::Options& options) {
        session = app::Session::open(book, options);
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

bool review_model_has(const Desk& desk, const std::string& ticket) {
    for (const auto& item : app::review_model::inbox(desk.window->book())) {
        if (item.ticket_id == ticket) return true;
    }
    return false;
}

// A style catalog of the test's own (file://) with ギャグ漫画 › ゆるい落書き風 (gag-doodle, version 3), set for the
// window while it lives.
struct StyleCatalog {
    QTemporaryDir site;
    StyleCatalog() {
        const QString catalog = site.path() + QStringLiteral("/catalog");
        QDir().mkpath(catalog + QStringLiteral("/v1/styles"));
        genko::test::write_bytes(catalog + QStringLiteral("/v1/tree"),
                                 R"({"nodes": [{"id": "gag", "parent": null, "level": 1, "title": "ギャグ漫画"}, {"id": "gag-doodle", "parent": "gag", "level": 2, "title": "ゆるい落書き風"}]})");
        genko::test::write_bytes(catalog + QStringLiteral("/v1/styles/gag-doodle"),
                                 R"({"id": "gag-doodle", "title": "ゆるい落書き風", "level": 2, "expression": "mono", "path": [{"id": "gag", "title": "ギャグ漫画"}],)"
                                 R"( "prompt": {"ja": "震える細い線", "en": "shaky lines"}, "avoid": [], "samples": [], "version": 3})");
        qputenv("GENKO_STYLE_CATALOG", (QStringLiteral("file://") + catalog).toUtf8());
    }
    ~StyleCatalog() { qunsetenv("GENKO_STYLE_CATALOG"); }
};

bool has_catalog_style(const fs::path& book) {
    const Json& studio = gui_test::read_book(book).studio;
    return studio.is_object() && studio.contains("style") && studio["style"].is_object() && studio["style"].contains("catalog");
}

const Json* ticket_with(const core::Document& doc, const std::string& kind, const std::function<bool(const Json&)>& also = {}) {
    for (const Json& t : doc.tickets) {
        if (t.value("kind", Json()) == Json(kind) && (!also || also(t))) return &t;
    }
    return nullptr;
}

// A reply draft's file in a book: studio/reply_drafts/<sha256 of the request's key>.json (schema-v4 §1).
fs::path draft_file(const fs::path& book, const std::string& item) {
    const QByteArray hash = QCryptographicHash::hash(QByteArray::fromStdString(item), QCryptographicHash::Sha256).toHex();
    return book / "studio" / "reply_drafts" / (hash.toStdString() + ".json");
}

std::string file_bytes(const fs::path& path) {
    QFile file(QString::fromStdString(core::path_to_utf8(path)));
    if (!file.open(QIODevice::ReadOnly)) return std::string("(none)");
    return file.readAll().toStdString();
}

void put_file(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    QFile file(QString::fromStdString(core::path_to_utf8(path)));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) qFatal("put_file");
    file.write(bytes.data(), static_cast<qint64>(bytes.size()));
}

std::string draft_json(const std::string& item, const std::string& text, const std::string& format = "genko-reply-draft@1") {
    return core::dump_python(Json{{"format", format}, {"item", item}, {"text", text}, {"at", "2026-10-01T09:00:00"}}) + "\n";
}

// A style catalog in a folder (GENKO_STYLE_CATALOG=file://…, as Python's tests fake stylecat._get): 少年漫画 › バトル ›
// 低頭身のデフォルメ, and ギャグ漫画 with a style of the same name under it; thumbnails and samples are PNGs there.
struct Catalog {
    QTemporaryDir site;
    QString root;
    QByteArray before;

    static Json branch(const QString& root, const std::string& id, const std::string& title, int level, const Json& path, const Json& children) {
        Json full = path;
        full.push_back(Json{{"id", id}, {"title", title}});
        return Json{{"id", id}, {"title", title}, {"summary", title + "の絵柄。"}, {"level", level}, {"expression", "mono"}, {"path", full},
                    {"prompt", Json{{"ja", "モノクロの日本のマンガのコマ。灰色はすべて網点で表す。"}, {"en", "Black-and-white manga panel."}}},
                    {"avoid", Json::array({"文字", "フキダシ"})},
                    {"samples", Json::array({Json{{"subject", "upper_body"}, {"url", (QStringLiteral("file://") + root + QStringLiteral("/files/")).toStdString() + id + ".png"}}})},
                    {"reference", 0}, {"version", 3}, {"updated_at", "2026-09-27T11:18:12Z"}, {"children", children}};
    }

    Catalog() {
        root = site.path() + QStringLiteral("/catalog");
        QDir().mkpath(root + QStringLiteral("/v1/styles"));
        QDir().mkpath(root + QStringLiteral("/files"));
        QDir().mkpath(root + QStringLiteral("/t"));
        const std::string thumb = (QStringLiteral("file://") + root).toStdString() + "/t/shonen.png";
        const Json tree = Json{{"version", 1}, {"nodes", Json::array({
            Json{{"id", "shonen"}, {"parent", nullptr}, {"level", 1}, {"title", "少年漫画"}, {"summary", "熱い"}, {"thumbnail_url", thumb}},
            Json{{"id", "shonen-battle"}, {"parent", "shonen"}, {"level", 2}, {"title", "バトル"}, {"summary", "戦い"}, {"thumbnail_url", nullptr}},
            Json{{"id", "shonen-battle-chibi"}, {"parent", "shonen-battle"}, {"level", 3}, {"title", "低頭身のデフォルメ"}, {"summary", "2〜3頭身"}},
            Json{{"id", "gag"}, {"parent", nullptr}, {"level", 1}, {"title", "ギャグ漫画"}, {"summary", "笑い"}},
            Json{{"id", "gag-chibi"}, {"parent", "gag"}, {"level", 2}, {"title", "低頭身のデフォルメ"}, {"summary", "ちび"}}})}};
        genko::test::write_bytes(root + QStringLiteral("/v1/tree"), tree.dump());
        const Json shonen = Json::array({Json{{"id", "shonen"}, {"title", "少年漫画"}}});
        const Json battle = Json::array({Json{{"id", "shonen"}, {"title", "少年漫画"}}, Json{{"id", "shonen-battle"}, {"title", "バトル"}}});
        const Json gag = Json::array({Json{{"id", "gag"}, {"title", "ギャグ漫画"}}});
        const std::vector<Json> styles{
            branch(root, "shonen", "少年漫画", 1, Json::array(), Json::array({Json{{"id", "shonen-battle"}, {"title", "バトル"}}})),
            branch(root, "shonen-battle", "バトル", 2, shonen, Json::array({Json{{"id", "shonen-battle-chibi"}, {"title", "低頭身のデフォルメ"}}})),
            branch(root, "shonen-battle-chibi", "低頭身のデフォルメ", 3, battle, Json::array()),
            branch(root, "gag", "ギャグ漫画", 1, Json::array(), Json::array({Json{{"id", "gag-chibi"}, {"title", "低頭身のデフォルメ"}}})),
            branch(root, "gag-chibi", "低頭身のデフォルメ", 2, gag, Json::array())};
        for (const Json& style : styles) {
            const QString id = QString::fromStdString(style["id"].get<std::string>());
            genko::test::write_bytes(root + QStringLiteral("/v1/styles/") + id, style.dump());
            genko::test::write_bytes(root + QStringLiteral("/files/") + id + QStringLiteral(".png"), grey_png(30, 40, 128));
        }
        genko::test::write_bytes(root + QStringLiteral("/t/shonen.png"), grey_png(18, 24, 90));
        before = qgetenv("GENKO_STYLE_CATALOG");
        qputenv("GENKO_STYLE_CATALOG", (QStringLiteral("file://") + root).toUtf8());
    }
    ~Catalog() {
        if (before.isEmpty()) {
            qunsetenv("GENKO_STYLE_CATALOG");
        } else {
            qputenv("GENKO_STYLE_CATALOG", before);
        }
    }
};

bool read_through(const app::StylePicker& picker) {
    return gui_test::wait_for([&] { return picker.reading() == 0; }, 60000);
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
        QCOMPARE(pages.value(paper + 1), QStringLiteral("絵柄を選ぶ…"));
        QCOMPARE(pages.value(paper + 2), QStringLiteral("チャットでの承認…"));
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

    // The words not sent are kept in the book (studio/reply_drafts/, one file for each request): written a moment after
    // the typing stops and when the window closes, read again by another run (the words in memory first), broken files
    // thrown away with a notice, removed when sent or emptied, not kept when too long; never in the book's history.
    void draftsKeptInBook() {
        StudioBook book;
        const QString words = QStringLiteral("一行目\n二行目");
        std::string name_ticket;
        std::string question_ticket;
        {
            Desk desk(book.path, book.root / "recovery");
            QVERIFY(desk.choose(QStringLiteral("ネームの承認: 1, 2 ページ")));
            name_ticket = desk.box()->current()->ticket_id;
            const std::string journal = file_bytes(book.path / "studio" / "journal.jsonl");
            const std::string project = file_bytes(book.path / "project.json");
            const bool could_undo = desk.session->can_undo();
            QVERIFY(app::Drafts::instance().kept_on_disk(desk.box()->book_key()));
            type_in(desk.box()->reason, QStringLiteral("一行目"));
            QTest::keyClick(desk.box()->reason, Qt::Key_Return);
            type_in(desk.box()->reason, QStringLiteral("二行目"));
            const fs::path file = draft_file(book.path, name_ticket);
            QVERIFY(gui_test::wait_for([&] { return fs::exists(file); }, 30000));
            const Json kept = genko::test::read_json(QString::fromStdString(core::path_to_utf8(file)));
            QCOMPARE(kept.size(), std::size_t(4));
            QCOMPARE(kept.value("format", Json()), Json("genko-reply-draft@1"));
            QCOMPARE(kept.value("item", Json()), Json(name_ticket));
            QCOMPARE(kept.value("text", Json()), Json(words.toStdString()));
            QVERIFY(QDateTime::fromString(QString::fromStdString(kept.value("at", Json("")).get<std::string>()), Qt::ISODate).isValid());
            // not a change of the book: no journal line, no Undo, nothing to save
            QCOMPARE(file_bytes(book.path / "studio" / "journal.jsonl"), journal);
            QCOMPARE(file_bytes(book.path / "project.json"), project);
            QCOMPARE(desk.session->can_undo(), could_undo);
            QVERIFY(!desk.session->unsaved());
            // another request's words, and the window closed at once: written as it closes
            QVERIFY(desk.choose(QStringLiteral("相談（1 ページ）")));
            question_ticket = desk.box()->current()->ticket_id;
            type_in(desk.box()->reason, QStringLiteral("紺で"));
            desk.window->close();
            QVERIFY(fs::exists(draft_file(book.path, question_ticket)));
            QCOMPARE(genko::test::read_json(QString::fromStdString(core::path_to_utf8(draft_file(book.path, question_ticket)))).value("text", Json()),
                     Json("紺で"));
        }
        // another run of the app (a copy of the book: nothing of it in memory), with broken files beside the drafts
        const fs::path copy = book.root / "copy.genko";
        genko::test::copy_tree(QString::fromStdString(core::path_to_utf8(book.path)), QString::fromStdString(core::path_to_utf8(copy)));
        const fs::path drafts = copy / "studio" / "reply_drafts";
        const std::vector<fs::path> broken{draft_file(copy, "not json"), draft_file(copy, "big"), draft_file(copy, "t-a"),
                                           draft_file(copy, "t-long"), draft_file(copy, "t-list"), draft_file(copy, "t-format")};
        put_file(broken[0], "{\"format\": \"genko-reply-draft@1\", \"item\": ");
        put_file(broken[1], draft_json("big", std::string(300 * 1024, 'a')));
        put_file(broken[2], draft_json("t-b", "x"));
        put_file(broken[3], draft_json("t-long", std::string(65 * 1024, 'a')));
        put_file(broken[4], "[\"t-list\"]\n");
        put_file(broken[5], draft_json("t-format", "x", "someone-else@1"));
        // (a newer Genko's drafts: neither read nor thrown away)
        const fs::path newer = draft_file(copy, "t-other");
        put_file(newer, draft_json("t-other", "x", "genko-reply-draft@9"));
        put_file(drafts / "notes.txt", "keep");
        const fs::path temporary = drafts / (draft_file(copy, "t-x").filename().string() + ".123.abc.tmp");
        put_file(temporary, "half");
        QStringList notices;  // (what the drafts said, from before the window is made)
        const QMetaObject::Connection listening =
            QObject::connect(&app::Drafts::instance(), &app::Drafts::problem, [&notices](const QString&, const QString& message) { notices << message; });
        struct Unlisten {
            QMetaObject::Connection link;
            ~Unlisten() { QObject::disconnect(link); }
        } unlisten{listening};
        Desk again(copy, book.root / "recovery");
        QVERIFY(again.choose(QStringLiteral("ネームの承認: 1, 2 ページ")));  // (the book's drafts read when first wanted)
        const QString notice = notices.value(0);
        QVERIFY2(notice.startsWith(QStringLiteral("返事の下書きのファイルが壊れていたため、読まずに捨てました: ")), qPrintable(notice));
        for (const fs::path& gone : broken) {
            QVERIFY2(!fs::exists(gone), gone.filename().string().c_str());
            QVERIFY(notice.contains(QString::fromStdString(gone.filename().string())));
        }
        QVERIFY(notice.contains(QStringLiteral("大きすぎます（256 KiB まで）")));
        QVERIFY(notice.contains(QStringLiteral("言葉が長すぎます（64 KiB まで）")));
        QVERIFY(notice.contains(QStringLiteral("ファイルの名前が中身の依頼と合いません")));
        QVERIFY(notice.contains(QStringLiteral("UTF-8 の JSON として読めません")));
        QVERIFY(fs::exists(drafts / "notes.txt"));
        QVERIFY(fs::exists(temporary));
        QVERIFY(fs::exists(newer));
        QVERIFY2(notices.join(QLatin1Char('\n')).contains(QStringLiteral("この版の Genko が知らない形式の返事の下書きがあります。読まずに、そのまま残しました: ") +
                                                          QString::fromStdString(newer.filename().string())),
                 qPrintable(notices.join(QLatin1Char('\n'))));
        QCOMPARE(again.box()->reason->toPlainText(), words);
        QVERIFY(again.choose(QStringLiteral("相談（1 ページ）")));
        QCOMPARE(again.box()->reason->toPlainText(), QStringLiteral("紺で"));
        // sent: its file goes with it
        again.yes = {true};
        again.box()->back_button->click();
        QVERIFY(again.saved());
        app::Drafts::instance().flush();  // (the files are written and removed on a worker of their own)
        QVERIFY(!fs::exists(draft_file(copy, question_ticket)));
        // emptied: the file goes
        QVERIFY(again.choose(QStringLiteral("ネームの承認: 1, 2 ページ")));
        QCOMPARE(again.box()->reason->toPlainText(), words);
        again.box()->reason->selectAll();
        QTest::keyClick(again.box()->reason, Qt::Key_Delete);
        QVERIFY(gui_test::wait_for([&] { return !fs::exists(draft_file(copy, name_ticket)); }, 30000));
        // too long to keep (64 KiB of UTF-8): no file (not an older, shorter one either), and the person is told once
        type_in(again.box()->reason, QStringLiteral("短い"));
        app::Drafts::instance().flush();
        QVERIFY(fs::exists(draft_file(copy, name_ticket)));
        again.box()->reason->setPlainText(QString(22 * 1024, QChar(0x3042)));  // (3 bytes each: 66 KiB)
        app::Drafts::instance().flush();
        QVERIFY(!fs::exists(draft_file(copy, name_ticket)));
        QVERIFY2(again.window->last_error().startsWith(QStringLiteral("返事の下書きが長すぎるため（64 KiB まで）、原稿には残しません")),
                 qPrintable(again.window->last_error()));
        QCOMPARE(again.box()->reason->toPlainText().size(), 22 * 1024);  // (the words stay in the box)
        again.box()->reason->setPlainText(QString(21 * 1024, QChar(0x3042)));
        app::Drafts::instance().flush();
        QVERIFY(fs::exists(draft_file(copy, name_ticket)));
    }

    // Save As: the words not sent go on in the new book (its folder keeps them); the old book keeps its own.
    void draftsGoOnAfterSaveAs() {
        StudioBook book;
        Desk desk(book.path, book.root / "recovery");
        QVERIFY(desk.choose(QStringLiteral("ネームの承認: 1, 2 ページ")));
        const std::string ticket = desk.box()->current()->ticket_id;
        type_in(desk.box()->reason, QStringLiteral("続き"));
        app::Drafts::instance().flush();
        QVERIFY(fs::exists(draft_file(book.path, ticket)));
        const fs::path target = book.root / "saved.genko";
        desk.answers.responder->save_path = [&](const QString&, const QString&) { return QString::fromStdString(core::path_to_utf8(target)); };
        desk.window->action(QStringLiteral("act_save_as"))->trigger();  // (別の場所に保存…: it waits for the copy)
        QVERIFY(desk.session->path() && *desk.session->path() == target);
        app::Drafts::instance().flush();
        QVERIFY(fs::exists(draft_file(target, ticket)));
        QCOMPARE(genko::test::read_json(QString::fromStdString(core::path_to_utf8(draft_file(target, ticket)))).value("text", Json()), Json("続き"));
        QVERIFY(gui_test::wait_for([&] { return desk.box()->reason->toPlainText() == QStringLiteral("続き") &&
                                                desk.box()->book_key() == QString::fromStdString(core::path_to_utf8(target)); }, 30000));
        QVERIFY(fs::exists(draft_file(book.path, ticket)));
    }

    // Only a v4 book that can be changed keeps drafts in its folder: a v1–v3 book (M5②'s needs_migration), a book
    // opened read-only and a book not saved yet keep them in memory, and nothing is written in their folders.
    void draftsOnlyInV4Books() {
        QTemporaryDir tmp;
        const fs::path v3 = gui_test::path_of(tmp.path()) / "v3.genko";
        genko::test::copy_tree(genko::test::repo_root() + QStringLiteral("/native/tests/data/legacy/book-v3.genko"),
                               QString::fromStdString(core::path_to_utf8(v3)));
        auto& drafts = app::Drafts::instance();
        const QString old_key = QString::fromStdString(core::path_to_utf8(v3));
        drafts.place(old_key, v3, true);
        QVERIFY(!drafts.kept_on_disk(old_key));
        drafts.set(old_key, QStringLiteral("t-1"), QStringLiteral("言葉"));
        drafts.flush();
        QCOMPARE(drafts.get(old_key, QStringLiteral("t-1")), QStringLiteral("言葉"));
        QVERIFY(!fs::exists(v3 / "studio" / "reply_drafts"));
        StudioBook book(false);
        const QString locked = QString::fromStdString(core::path_to_utf8(book.path));
        drafts.place(locked, book.path, false);
        QVERIFY(!drafts.kept_on_disk(locked));
        drafts.set(locked, QStringLiteral("t-1"), QStringLiteral("言葉"));
        drafts.flush();
        QVERIFY(!fs::exists(book.path / "studio" / "reply_drafts"));
        drafts.place(QStringLiteral("book:new"), std::nullopt, true);
        QVERIFY(!drafts.kept_on_disk(QStringLiteral("book:new")));
        // (the same folder can be changed now: its drafts are written)
        drafts.place(locked, book.path, true);
        QVERIFY(drafts.kept_on_disk(locked));
        drafts.flush();
        QVERIFY(fs::exists(draft_file(book.path, "t-1")));
        drafts.forget(locked, QStringLiteral("t-1"));
        drafts.flush();
        QVERIFY(!fs::exists(draft_file(book.path, "t-1")));
        drafts.forget(old_key, QStringLiteral("t-1"));
        // studio/reply_drafts a link out of the book: nothing written through it, nothing removed there, and said
        StudioBook linked(false);
        QTemporaryDir elsewhere;
        fs::create_directories(linked.path / "studio");
        fs::create_directory_symlink(gui_test::path_of(elsewhere.path()), linked.path / "studio" / "reply_drafts");
        put_file(gui_test::path_of(elsewhere.path()) / draft_file(linked.path, "t-9").filename(), draft_json("t-9", "外のもの"));
        const QString linked_key = QString::fromStdString(core::path_to_utf8(linked.path));
        QSignalSpy said(&drafts, &app::Drafts::problem);
        drafts.place(linked_key, linked.path, true);
        QVERIFY(drafts.kept_on_disk(linked_key));
        QVERIFY(drafts.get(linked_key, QStringLiteral("t-9")).isEmpty());  // (not read through the link)
        drafts.set(linked_key, QStringLiteral("t-1"), QStringLiteral("言葉"));
        drafts.forget(linked_key, QStringLiteral("t-9"));
        drafts.flush();
        QCOMPARE(QDir(elsewhere.path()).entryList(QDir::Files).size(), 1);  // (only what was there)
        QVERIFY(said.count() >= 1);
        QVERIFY(said.at(0).at(1).toString().startsWith(QStringLiteral("返事の下書きの置き場所（studio/reply_drafts）が、原稿の中のフォルダーではありません")));
        drafts.forget(linked_key, QStringLiteral("t-1"));
        drafts.flush();
    }

    // An instruction asked about while another writer changes the book (the window reads it again during the question):
    // the panel is found by its page's id and its own id after the answer — the pages moved, it goes to the panel where it
    // is now; its page gone, nothing is sent and the words stay.
    void instructionWhileTheBookChanges() {
        StudioBook book(false);
        Desk desk(book.path, book.root / "recovery");
        QDockWidget* dock = desk.dock(QStringLiteral("コマの詳細"));
        dock->raise();
        QVERIFY(gui_test::wait_for([&] { return dock->isVisible(); }, 60000));
        app::PanelView* view = desk.window->panel_view();
        emit desk.window->canvas()->frameSelected(qs(book.leaves[0]));
        QVERIFY(gui_test::wait_for([&] { return view->candidates->count() == 2; }, 60000));
        QVERIFY(desk.saved());  // (the choice of the panel is a change of its own: saved before the book changes outside)
        const std::string page_id = gui_test::read_book(book.path).page(0).id;
        // (the question: before it is answered, the book changes outside and the window reads it again)
        const auto changed_meanwhile = [&](const Json& ops) {
            desk.answers.responder->question = [&, ops](const QString& title, const QString& text) {
                desk.questions << title + QStringLiteral(": ") + text;
                const auto changed = book.ai(ops, "human:outsider");
                if (!changed.ok) qFatal("the outside change was refused: %s", changed.to_dict().dump().c_str());
                desk.session->check_outside();
                const auto before = desk.window->book().revision;
                if (!gui_test::wait_for([&] { return desk.window->book().revision != before; }, 60000)) qFatal("not read again");
                return true;
            };
        };
        changed_meanwhile(Json::array({Json{{"op", "reorder"}, {"order", {2, 1, 3}}}}));
        type_in(view->instruction, QStringLiteral("左を向く"));
        view->send_button->click();
        QVERIFY2(desk.saved(), qPrintable(app::failure_reason(desk.session->status()) + QStringLiteral(" / ") + desk.window->last_error()));
        {
            const core::Document saved = gui_test::read_book(book.path);
            QCOMPARE(saved.page(1).id, page_id);  // (the panel's page is the second now)
            const core::Frame* frame = saved.page(1).find_frame(book.leaves[0]);
            QVERIFY(frame != nullptr && frame->panel && frame->panel->is_object());
            const Json instruction = frame->panel->value("instruction", Json::object());
            QCOMPARE(instruction.is_object() ? instruction.value("text", Json()) : Json(), Json("左を向く"));
            QVERIFY(ticket_with(saved, "fix", [&](const Json& t) { return t.value("frame_id", Json()) == Json(book.leaves[0]); }) != nullptr);
        }
        QVERIFY(gui_test::wait_for([&] { return view->instruction->toPlainText().isEmpty(); }, 60000));
        // its page gone while asked: nothing sent, the words kept
        desk.window->select_page(1);  // (the panel shown again where it is now)
        emit desk.window->canvas()->frameSelected(qs(book.leaves[0]));
        QVERIFY(gui_test::wait_for([&] { return view->frame_id == book.leaves[0] && view->candidates->count() == 2; }, 60000));
        QVERIFY(desk.saved());
        const int fixes = static_cast<int>(gui_test::read_book(book.path).tickets.size());
        changed_meanwhile(Json::array({Json{{"op", "delete_page"}, {"page", 2}}}));
        type_in(view->instruction, QStringLiteral("右を向く"));
        view->send_button->click();
        QVERIFY(desk.saved());
        QVERIFY2(desk.window->last_error().startsWith(QStringLiteral("確認の間に原稿が変わり、そのコマが見つからなくなったため、指示を送っていません")),
                 qPrintable(desk.window->last_error()));
        QCOMPARE(static_cast<int>(gui_test::read_book(book.path).tickets.size()), fixes);
        QCOMPARE(app::Drafts::instance().get(app::draft_book_key(*desk.window), QStringLiteral("panel:") + qs(book.leaves[0])), QStringLiteral("右を向く"));
    }

    // The window and an agent on the same book (M5's exit; F3: the book handled safely from the window and the MCP
    // tools at once): a panel chosen in the window — a change of its own, the choice not saved in the book — and, before
    // it is saved, the agent reorders the pages (its apply_ops, committed). The window reads the book again: no
    // conflict holds up its saves, the agent's order stands, the choice stays on its panel's page (now page 2); the
    // person's instruction for the panel and the agent's next change go on as usual.
    void choiceWhileTheAgentReorders() {
        StudioBook book(false);
        app::Session::Options options = gui_test::quick(book.root / "recovery");
        options.idle = std::chrono::hours(1);  // (the autosave waits longer than the test: the choice waits when the agent writes)
        options.longest = std::chrono::hours(1);
        Desk desk(book.path, options);
        QSignalSpy conflicts(desk.session.get(), &app::Session::conflicts);
        QDockWidget* dock = desk.dock(QStringLiteral("コマの詳細"));
        dock->raise();
        QVERIFY(gui_test::wait_for([&] { return dock->isVisible(); }, 60000));
        app::PanelView* view = desk.window->panel_view();
        const std::vector<std::string> ids = [&] {
            std::vector<std::string> out;
            for (const auto& page : desk.window->book().pages) out.push_back(page->id);
            return out;
        }();
        emit desk.window->canvas()->frameSelected(qs(book.leaves[0]));
        QVERIFY(gui_test::wait_for([&] { return view->candidates->count() == 2; }, 60000));
        QCOMPARE(desk.session->waiting(), std::size_t{1});  // (the choice, not saved yet)
        const auto reordered = book.ai(Json::array({Json{{"op", "reorder"}, {"order", {2, 1, 3}}}}));
        QVERIFY2(reordered.ok, reordered.to_dict().dump().c_str());
        const auto revision = desk.window->book().revision;
        desk.session->check_outside();  // (the window's file watcher)
        QVERIFY(gui_test::wait_for([&] { return desk.window->book().revision != revision || desk.session->status().kind == app::SaveKind::Failed; }, 60000));
        desk.session->save_now();
        QVERIFY2(desk.saved(), qPrintable(app::failure_reason(desk.session->status()) + QStringLiteral(" / ") + desk.window->last_error()));
        QCOMPARE(conflicts.count(), 0);
        {
            const core::Document saved = gui_test::read_book(book.path);
            QCOMPARE(saved.page(0).id, ids[1]);
            QCOMPARE(saved.page(1).id, ids[0]);
            QCOMPARE(desk.window->book().page(1).selected_frame_id, Json(book.leaves[0]));
            QVERIFY(desk.window->book().page(0).selected_frame_id.is_null());
        }
        // the person goes on: the panel shown where it is now, an instruction for it
        desk.window->select_page(1);
        emit desk.window->canvas()->frameSelected(qs(book.leaves[0]));
        QVERIFY(gui_test::wait_for([&] { return view->frame_id == book.leaves[0] && view->candidates->count() == 2; }, 60000));
        desk.yes = {true};
        type_in(view->instruction, QStringLiteral("左を向く"));
        view->send_button->click();
        QVERIFY2(desk.saved(), qPrintable(app::failure_reason(desk.session->status()) + QStringLiteral(" / ") + desk.window->last_error()));
        {
            const core::Document saved = gui_test::read_book(book.path);
            const core::Frame* frame = saved.page(1).find_frame(book.leaves[0]);
            QVERIFY(frame != nullptr && frame->panel && frame->panel->is_object());
            const Json instruction = frame->panel->value("instruction", Json::object());
            QCOMPARE(instruction.is_object() ? instruction.value("text", Json()) : Json(), Json("左を向く"));
        }
        // and so does the agent
        const auto noted = book.ai(Json::array({Json{{"op", "set_note"}, {"page", 2}, {"note", "AI のメモ"}}}));
        QVERIFY2(noted.ok, noted.to_dict().dump().c_str());
        desk.session->check_outside();
        QVERIFY(gui_test::wait_for([&] { return desk.window->book().page(1).note == "AI のメモ"; }, 60000));
        QVERIFY(desk.saved());
        QCOMPARE(conflicts.count(), 0);
    }

    // The approval box asked about one request while it comes to choose another, or the request is closed elsewhere: the
    // answer is not sent to the other request nor to the closed one; the words stay with the request they were written
    // for.
    void answerWhileTheBoxChanges() {
        StudioBook book;
        Desk desk(book.path, book.root / "recovery");
        const QString words = QStringLiteral("直してください");
        QVERIFY(desk.choose(QStringLiteral("相談（1 ページ）")));
        const std::string help_ticket = desk.box()->current()->ticket_id;
        type_in(desk.box()->reason, QStringLiteral("紺で"));  // (the question's own reply, not sent)
        QVERIFY(desk.choose(QStringLiteral("ネームの承認: 1, 2 ページ")));
        const std::string name_ticket = desk.box()->current()->ticket_id;
        type_in(desk.box()->reason, words);
        // (while 差し戻す is asked, the box comes to choose the question)
        desk.answers.responder->question = [&](const QString& title, const QString& text) {
            desk.questions << title + QStringLiteral(": ") + text;
            desk.choose(QStringLiteral("相談（1 ページ）"));
            return true;
        };
        desk.box()->back_button->click();
        QVERIFY(desk.saved());
        QVERIFY2(desk.window->last_error().startsWith(QStringLiteral("確認の間に、承認箱の依頼か原稿が変わったため、送っていません")), qPrintable(desk.window->last_error()));
        {
            const core::Document saved = gui_test::read_book(book.path);
            QCOMPARE(ticket_with(saved, "gate", [](const Json& t) { return t.value("gate", Json()) == Json("name"); })->value("status", Json()), Json("open"));
            QVERIFY(ticket_with(saved, "fix", [&](const Json& t) { return t.value("text", Json()) == Json(words.toStdString()); }) == nullptr);
            QCOMPARE(ticket_with(saved, "help")->value("status", Json()), Json("open"));
        }
        const QString key = desk.box()->book_key();
        QCOMPARE(app::Drafts::instance().get(key, qs(name_ticket)), words);
        QCOMPARE(app::Drafts::instance().get(key, qs(help_ticket)), QStringLiteral("紺で"));  // (the other request's words: not taken)
        QCOMPARE(desk.box()->reason->toPlainText(), QStringLiteral("紺で"));
        // a question closed elsewhere while 返事なしで閉じる is asked: nothing applied to it, the reply kept
        QVERIFY(desk.choose(QStringLiteral("相談（1 ページ）")));
        desk.answers.responder->question = [&](const QString& title, const QString& text) {
            desk.questions << title + QStringLiteral(": ") + text;
            const Json closed = service::HumanService(book.path, "human:outsider").close_ticket(Json(help_ticket));
            if (!closed.value("ok", false)) qFatal("not closed: %s", closed.dump().c_str());
            desk.session->check_outside();
            if (!gui_test::wait_for([&] { return !review_model_has(desk, help_ticket); }, 60000)) qFatal("not read again");
            return true;
        };
        const int revision_before = static_cast<int>(gui_test::read_book(book.path).revision);
        desk.box()->approve_button->click();
        QVERIFY(desk.saved());
        QVERIFY2(desk.window->last_error().startsWith(QStringLiteral("確認の間に、承認箱の依頼か原稿が変わったため、送っていません")), qPrintable(desk.window->last_error()));
        QCOMPARE(static_cast<int>(gui_test::read_book(book.path).revision), revision_before + 1);  // (the outside change only)
        QCOMPARE(app::Drafts::instance().get(key, qs(help_ticket)), QStringLiteral("紺で"));
    }

    // The official export waits for the book to be saved: a save that fails (the book cannot be written) stops it before
    // the export dialog, and says so.
    void exportWaitsForTheSave() {
        StudioBook book;
        Desk desk(book.path, book.root / "recovery");
        int dialogs = 0;
        desk.on_dialog = [&](QDialog*) {
            ++dialogs;
            return int(QDialog::Rejected);
        };
        const auto& items = desk.box()->items();
        int row = -1;
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (items[i].gate_text() == "export") row = static_cast<int>(i);
        }
        QVERIFY(row >= 0);
        desk.box()->list->setCurrentRow(row);
        // (the book cannot be written: a folder where its journal was, so every save stops before it writes)
        const fs::path journal = book.path / "studio" / "journal.jsonl";
        const fs::path kept = book.path / "studio" / "journal.kept";
        fs::rename(journal, kept);
        fs::create_directory(journal);
        struct Restore {
            fs::path journal, kept;
            ~Restore() {
                std::error_code ec;
                fs::remove(journal, ec);
                fs::rename(kept, journal, ec);
            }
        } restore{journal, kept};
        desk.session->apply(Json::array({Json{{"op", "set_note"}, {"page", 1}, {"note", "書き出す前"}}}));
        desk.box()->approve();
        QCOMPARE(dialogs, 0);
        QVERIFY2(desk.window->last_error().startsWith(QStringLiteral("原稿を保存できていないため、正式な書き出しをしていません")), qPrintable(desk.window->last_error()));
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
        QVERIFY(desk.saved());  // (written before the book changes outside)
        // switched elsewhere while asked (another window of the person's): read again after the answer, nothing changed
        desk.answers.responder->question = [&](const QString& title, const QString& text) {
            desk.questions << title + QStringLiteral(": ") + text;
            service::HumanService(book.path, "human:tester").chat_approval(Json(false));
            desk.session->check_outside();
            if (!gui_test::wait_for([&] { return !service::chat_approval_open(desk.window->book()); }, 60000)) qFatal("not read again");
            return true;
        };
        desk.window->action(QStringLiteral("act_chat_approval"))->trigger();
        QVERIFY2(desk.window->last_error().startsWith(QStringLiteral("確認の間に原稿か、チャットでの承認の設定が変わったため、何も変えていません")),
                 qPrintable(desk.window->last_error()));
        QVERIFY(desk.saved());
        QVERIFY(!service::chat_approval_open(gui_test::read_book(book.path)));
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

    // 絵柄を選ぶ (Python's StylePicker) on a catalog in a folder: the genres with their thumbnails, a branch's card (its
    // sample, its words when asked, the site's page — asked of the browser, never opened by the test), down and up the
    // levels, the search, a pasted genko:// link, a black-and-white style marked on a colour book, the book's style
    // opened where it is, and a catalog that cannot be read.
    void stylePicker() {
        Catalog catalog;
        gui_test::Answers answers;
        {
            app::StylePicker picker(nullptr);
            QVERIFY(read_through(picker));
            QCOMPARE(picker.where->text(), QStringLiteral("絵柄カタログ — 1段目（ジャンル）"));
            QCOMPARE(picker.list->count(), 2);
            QCOMPARE(picker.list->item(0)->text(), QStringLiteral("少年漫画\n熱い（下に 1 種）"));
            QCOMPARE(picker.list->item(1)->text(), QStringLiteral("ギャグ漫画\n笑い（下に 1 種）"));
            QVERIFY(!picker.list->item(0)->icon().isNull());
            QVERIFY(picker.list->item(1)->icon().isNull());
            QVERIFY(!picker.up->isEnabled());
            QVERIFY(!picker.ok->isEnabled());
            QVERIFY(picker.plain->isHidden());
            picker.list->setCurrentRow(0);
            QVERIFY(picker.ok->isEnabled());
            QVERIFY(read_through(picker));
            QCOMPARE(picker.title->text(), QStringLiteral("少年漫画"));
            QCOMPARE(picker.summary->text(), QStringLiteral("熱い"));
            QCOMPARE(picker.fit->text(), QStringLiteral("白黒・版 3"));
            QCOMPARE(picker.words->text(), QStringLiteral("モノクロの日本のマンガのコマ。灰色はすべて網点で表す。"));
            QVERIFY(!picker.picture->pixmap().isNull());
            QVERIFY(picker.words_area->isHidden());
            picker.show_words->click();
            QVERIFY(!picker.words_area->isHidden());
            picker.site->click();
            QCOMPARE(answers.asked.value(0), QStringLiteral("open_url: ") + QString::fromStdString(service::stylecat::page_url("shonen")));
            // down (Enter, as a double click), down again: the third level
            QTest::keyClick(picker.list, Qt::Key_Return);
            QVERIFY(picker.here() == std::optional<std::string>("shonen"));
            QCOMPARE(picker.where->text(), QStringLiteral("絵柄カタログ › 少年漫画 — 2段目"));
            QVERIFY(picker.up->isEnabled());
            picker.go_down("shonen-battle");
            QCOMPARE(picker.list->item(0)->data(Qt::UserRole).toString(), QStringLiteral("shonen-battle-chibi"));
            QVERIFY(picker.where->text().endsWith(QStringLiteral("3段目（仕上げの絵柄）")));
            picker.go_down("shonen-battle-chibi");  // (nothing under it: stays)
            QVERIFY(picker.here() == std::optional<std::string>("shonen-battle"));
            // up twice: the genres, with the one come from chosen
            picker.up->click();
            QVERIFY(picker.chosen_id() == std::optional<std::string>("shonen-battle"));
            picker.up->click();
            QVERIFY(picker.here() == std::optional<std::string>());
            QVERIFY(picker.chosen_id() == std::optional<std::string>("shonen"));
            // the search: only the rows with every word
            picker.find->setText(QStringLiteral("少年　熱い"));
            QVERIFY(!picker.list->item(0)->isHidden());
            QVERIFY(picker.list->item(1)->isHidden());
            picker.find->setText(QStringLiteral("ない言葉"));
            QVERIFY(picker.list->item(0)->isHidden());
            picker.find->clear();
            QVERIFY(!picker.list->item(1)->isHidden());
            // a link pasted: that style, in its level
            picker.find->setText(QStringLiteral("genko://use-style?id=shonen-battle-chibi"));
            QVERIFY(picker.find->text().isEmpty());
            QVERIFY(picker.here() == std::optional<std::string>("shonen-battle"));
            QVERIFY(picker.chosen_id() == std::optional<std::string>("shonen-battle-chibi"));
            picker.ok->click();
            QCOMPARE(picker.result(), int(QDialog::Accepted));
            QVERIFY(picker.chosen == std::optional<std::string>("shonen-battle-chibi"));
            QVERIFY(!picker.back_to_genko);
        }
        {
            // a colour book: a black-and-white style is marked
            app::StylePicker picker(nullptr, Json(), "color");
            QVERIFY(read_through(picker));
            picker.list->setCurrentRow(1);
            QVERIFY(read_through(picker));
            QCOMPARE(picker.fit->text(), QStringLiteral("白黒・版 3　この原稿はカラーなので、絵の依頼には使われません"));
            QVERIFY(!picker.show_words->isChecked());
        }
        {
            // the book's style: opened where it is, Genko's own words offered
            const Json kept = service::stylecat::saved(service::stylecat::style(Json("shonen-battle-chibi")), std::nullopt);
            app::StylePicker picker(nullptr, kept);
            QVERIFY(read_through(picker));
            QVERIFY(picker.here() == std::optional<std::string>("shonen-battle"));
            QVERIFY(picker.chosen_id() == std::optional<std::string>("shonen-battle-chibi"));
            QVERIFY(!picker.plain->isHidden());
            picker.plain->click();
            QCOMPARE(picker.result(), int(QDialog::Accepted));
            QVERIFY(picker.back_to_genko);
        }
        {
            // a catalog that is not there: said where the levels are, nothing listed
            qputenv("GENKO_STYLE_CATALOG", (QStringLiteral("file://") + catalog.site.path() + QStringLiteral("/nowhere")).toUtf8());
            app::StylePicker picker(nullptr);
            QVERIFY(read_through(picker));
            QCOMPARE(picker.list->count(), 0);
            QVERIFY(!picker.where->text().isEmpty());
            QVERIFY(picker.where->text() != QStringLiteral("絵柄カタログを読んでいます…"));
            qputenv("GENKO_STYLE_CATALOG", (QStringLiteral("file://") + catalog.root).toUtf8());
        }
        QCOMPARE(answers.asked.size(), 1);  // (one page asked of the browser; nothing else)
    }

    // 絵柄を選ぶ… in the window (Python's _pick_style): a style whose name is used elsewhere is asked about — a "no" goes
    // back to the picker —, the next one is taken into the book (its words and sample kept); again, Genko's own words.
    void pickStyle() {
        Catalog catalog;
        StudioBook book(false);
        Desk desk(book.path, book.root / "recovery");
        int shown = 0;
        QList<bool> plain_offered;  // (Genko の言葉に戻す: only for a book with a style)
        desk.on_dialog = [&](QDialog* dialog) -> int {
            auto* picker = qobject_cast<app::StylePicker*>(dialog);
            if (picker == nullptr || !read_through(*picker)) return QDialog::Rejected;
            ++shown;
            if (shown == 1) {
                picker->find->setText(QStringLiteral("genko://use-style?id=shonen-battle-chibi"));
                picker->ok->click();
            } else if (shown == 2) {
                plain_offered << !picker->plain->isHidden();
                picker->list->setCurrentRow(1);  // (ギャグ漫画: a genre may be chosen)
                picker->ok->click();
            } else {
                plain_offered << !picker->plain->isHidden();
                picker->plain->click();
            }
            return picker->result();
        };
        desk.yes = {false};
        desk.window->action(QStringLiteral("act_style"))->trigger();
        QCOMPARE(shown, 2);
        QCOMPARE(desk.questions.size(), 1);
        QVERIFY(desk.questions.value(0).startsWith(QStringLiteral("絵柄を選ぶ: 「スタジオ」の絵柄を「少年漫画 › バトル › 低頭身のデフォルメ」にしますか？")));
        QVERIFY(desk.questions.value(0).contains(QStringLiteral("同じ名前の絵柄がほかの分類にもあります: ギャグ漫画 › 低頭身のデフォルメ")));
        const Json kept = desk.window->book().studio["style"]["catalog"];
        QCOMPARE(kept["id"], Json("gag"));
        QVERIFY(storage::AssetStore(book.path).has(kept["sample"].get<std::string>(), ".png"));
        QVERIFY(desk.window->last_notice().startsWith(QStringLiteral("絵柄を「ギャグ漫画」にしました（版 3）")));
        QVERIFY(desk.saved());
        QCOMPARE(gui_test::read_book(book.path).studio["style"]["catalog"]["id"], Json("gag"));
        // again: Genko's own words
        desk.window->action(QStringLiteral("act_style"))->trigger();
        QCOMPARE(shown, 3);
        QCOMPARE(plain_offered, QList<bool>({false, true}));
        QCOMPARE(desk.window->last_notice(), QStringLiteral("絵柄を Genko の言葉に戻しました"));
        QVERIFY(desk.saved());
        const Json style = gui_test::read_book(book.path).studio.value("style", Json::object());
        QVERIFY(!style.is_object() || !core::py_truthy(style.value("catalog", Json())));
        QCOMPARE(desk.questions.size(), 1);
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
        // (a home of the test's own: the later start reads and writes nothing of the person's)
        for (const char* name : {"HOME", "USERPROFILE"}) env.insert(QString::fromLatin1(name), config.path() + QStringLiteral("/home"));
        env.insert(QStringLiteral("XDG_CONFIG_HOME"), config.path() + QStringLiteral("/home/.config"));
        env.insert(QStringLiteral("XDG_DATA_HOME"), config.path() + QStringLiteral("/home/.local/share"));
        env.insert(QStringLiteral("XDG_CACHE_HOME"), config.path() + QStringLiteral("/home/.cache"));
        env.insert(QStringLiteral("APPDATA"), config.path() + QStringLiteral("/home/AppData/Roaming"));
        env.insert(QStringLiteral("LOCALAPPDATA"), config.path() + QStringLiteral("/home/AppData/Local"));
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
        {
            const StyleCatalog catalog;
            desk.yes = {true};
            desk.window->open_link(received.at(0).at(0).toString());
        }
        QCOMPARE(desk.questions.size(), 1);
        QCOMPARE(desk.questions.value(0), QStringLiteral("絵柄を選ぶ: 「スタジオ」の絵柄を「ギャグ漫画 › ゆるい落書き風」にしますか？"));
        QCOMPARE(desk.window->book().studio["style"]["catalog"]["id"], Json("gag-doodle"));
        QVERIFY(desk.window->last_notice().startsWith(QStringLiteral("絵柄を「ギャグ漫画 › ゆるい落書き風」にしました（版 3）")));
    }

    // The style asked about for the book in front while another book comes forward (a book opened meanwhile): after the
    // answer nothing is changed — neither in the book asked about nor in the one in front now.
    void styleWhileAnotherBookComesForward() {
        StudioBook book(false);
        StudioBook other(false);
        Desk desk(book.path, book.root / "recovery");
        const StyleCatalog catalog;
        desk.answers.responder->question = [&](const QString& title, const QString& text) {
            desk.questions << title + QStringLiteral(": ") + text;
            desk.window->add_document(app::Session::open(other.path, gui_test::quick(other.root / "recovery")));
            return true;
        };
        desk.window->open_link(QStringLiteral("genko://use-style?id=gag-doodle"));
        QCOMPARE(desk.questions.size(), 1);
        QVERIFY(desk.window->asking().session != desk.session);  // (the other book in front now)
        QCOMPARE(desk.window->last_error(), QStringLiteral("確認の間に前に出ている原稿が替わったため、絵柄を変えていません。"));
        QVERIFY(desk.saved());
        QVERIFY(desk.window->asking().session->wait_saved(std::chrono::seconds(30)));
        QVERIFY(!has_catalog_style(book.path));
        QVERIFY(!has_catalog_style(other.path));
    }

    // A link that comes while no book on disk is in front waits for one. When the book asked for cannot be opened, the
    // link is kept, and said so; the book opened next takes it.
    void linksKeptWhenTheBookCannotBeOpened() {
        StudioBook book(false);
        QTemporaryDir tmp;
        gui_test::Answers answers;  // (before the window: the window's questions are answered here to its end)
        QStringList warnings;
        QStringList questions;
        answers.responder->warning = [&](const QString&, const QString& text) { warnings << text; };
        answers.responder->question = [&](const QString& title, const QString& text) {
            questions << title + QStringLiteral(": ") + text;
            return false;
        };
        app::Session::Options options;
        options.actor = "human:tester";
        options.autosave = false;
        options.recovery_root = gui_test::path_of(tmp.path() + QStringLiteral("/recovery"));
        app::MainWindow window(std::make_shared<app::Session>(gui_test::new_doc(1), std::nullopt, options));
        window.show();
        window.open_link_when_ready(QStringLiteral("genko://use-style?id=gag-doodle"));
        QVERIFY(questions.isEmpty());  // (waits for a book)
        const fs::path broken = gui_test::path_of(tmp.path() + QStringLiteral("/broken.genko"));
        fs::create_directories(broken);
        genko::test::write_bytes(gui_test::qpath(broken / "project.json"), "{");
        window.open_project(broken);
        QVERIFY(gui_test::wait_for([&] { return !warnings.isEmpty(); }, 60000));
        QVERIFY2(warnings.at(0).startsWith(QStringLiteral("原稿を開けませんでした")), qPrintable(warnings.at(0)));
        QVERIFY2(warnings.at(0).endsWith(QStringLiteral("届いていたリンク（genko://use-style?id=gag-doodle）は、次に原稿を開いたときに使います。")),
                 qPrintable(warnings.at(0)));
        QVERIFY(questions.isEmpty());
        // the next book opened takes it: the style is asked about for that book
        const StyleCatalog catalog;
        window.open_project(book.path);
        QVERIFY(gui_test::wait_for([&] { return !questions.isEmpty(); }, 60000));
        QCOMPARE(questions.at(0), QStringLiteral("絵柄を選ぶ: 「スタジオ」の絵柄を「ギャグ漫画 › ゆるい落書き風」にしますか？"));
        QVERIFY(!has_catalog_style(book.path));  // (answered no)
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
