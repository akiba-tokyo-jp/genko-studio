// AI と作る in the window (M5④: Python's genko/app/ai_link.py AiDialog, with SPEC AI-01 and improvement I07): the dialog
// as Python lays it out (the clients and their text to paste, the folder, the first request, the AIs that worked on the
// book, the AI chosen last kept in the settings), and what this build adds —
//   - the connection's state in one word — 未設定・設定済み・未接続・接続済み・処理中・人の確認待ち・失敗 — with the facts
//     under it (the settings, the test connection, the book and conversation asked for, the last call and why it failed,
//     the work, the requests waiting), and 失敗 on the status line's AI button;
//   - the client's settings written only after the person has seen the change, the copy of the file and confirmed it:
//     Claude Desktop's JSON and Hermes Agent's YAML (the other servers and settings kept, shown by name only, never their
//     values), cancelled, put back (also when the file changed since: that state kept aside), refused when the file
//     cannot be changed safely — in a home folder of the test's own, never the person's;
//   - the test connection: Genko's MCP server started as the settings start it (here a stand-in of the test,
//     genko_fake_mcp), asked only initialize and tools/list — no tool called, the book not changed —, and the reason
//     when it fails: it ends, it says something that is not MCP, it refuses, it does not answer, it cannot start;
//   - the book and the conversation (MCP session) the first request names, the conversation's name checked as `genko mcp`
//     checks it, kept by the window;
//   - the first setup run through: empty settings → the dialog → the change seen and confirmed → the file written → the
//     client started again (the stand-in, as the file says) → book and conversation chosen → the AI's requests in two
//     books from two conversations → each waiting in its own book's box (none in the other) → the person's approval and
//     a reply in two lines → the AI reads them back, after the connection is closed and made again; and the same run with
//     this build's `genko mcp` (M5③) as the server, which keeps its tool log and presence in each book as Python's
//     server does.

#include <QtTest>

#include <QApplication>
#include <QKeyEvent>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSignalSpy>

#include <cmath>
#include <deque>

#include "app/ai_dialog.hpp"
#include "app/ai_link.hpp"
#include "app/client_config.hpp"
#include "app/config.hpp"
#include "app/main_window.hpp"
#include "app/mcp_probe.hpp"
#include "app/path_label.hpp"
#include "app/reply_box.hpp"
#include "app/review_model.hpp"
#include "app/studio_widgets.hpp"
#include "core/json.hpp"
#include "core/paths.hpp"
#include "gui_support.hpp"
#include "service/studio_service.hpp"
#include "storage/reader.hpp"
#include "studio/files.hpp"
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

QString fake_mcp() { return QStringLiteral(GENKO_FAKE_MCP); }

double now_seconds() { return static_cast<double>(QDateTime::currentMSecsSinceEpoch()) / 1000.0; }

// A book made with agents, as `create_project` makes it (strict gates), in `root`.
fs::path agent_book(const fs::path& root, const std::string& name, int pages = 2) {
    service::StudioService studio(root, "human:tester");
    const Json made = studio.call("create_project", Json::array(), Json{{"name", name}, {"title", name}, {"pages", pages}}).to_dict();
    if (!made.value("ok", false)) qFatal("create_project: %s", made.dump().c_str());
    return root / name;
}

// The window on a book, its questions answered by the test: a yes or no for each question in turn (none left: no), the
// AI dialog and the settings change by functions.
struct Desk {
    std::shared_ptr<app::Session> session;
    std::unique_ptr<app::MainWindow> window;
    gui_test::Answers answers;
    std::deque<bool> yes;
    QStringList questions;
    QStringList warnings;
    std::function<int(app::AiDialog*)> on_ai;
    std::function<int(app::ConfigChangeDialog*)> on_change;

    explicit Desk(const fs::path& book, const fs::path& recovery) {
        session = app::Session::open(book, gui_test::quick(recovery));
        window = std::make_unique<app::MainWindow>(session);
        window->resize(1280, 860);
        window->show();
        answers.responder->question = [this](const QString& title, const QString& text) {
            questions << title + QStringLiteral(": ") + text;
            if (yes.empty()) return false;
            const bool answer = yes.front();
            yes.pop_front();
            return answer;
        };
        answers.responder->warning = [this](const QString& title, const QString& text) { warnings << title + QStringLiteral(": ") + text; };
        answers.responder->exec = [this](QDialog* dialog) -> int {
            if (auto* ai = qobject_cast<app::AiDialog*>(dialog); ai != nullptr && on_ai) return on_ai(ai);
            if (auto* change = qobject_cast<app::ConfigChangeDialog*>(dialog); change != nullptr && on_change) return on_change(change);
            return QDialog::Rejected;
        };
    }
    ~Desk() {
        window.reset();
        session.reset();
    }
    // The AI dialog as the AI button opens it, `drive` run on it.
    void with_dialog(const std::function<void(app::AiDialog*)>& drive) {
        bool ran = false;
        on_ai = [&](app::AiDialog* dialog) {
            drive(dialog);
            ran = true;
            return QDialog::Rejected;
        };
        window->ai_button()->click();
        on_ai = nullptr;
        QVERIFY(ran);
    }
};

// An MCP client of the test (the AI's side): one JSON-RPC message a line over the server's stdio.
struct Client {
    QProcess process;
    QByteArray buffer;
    int next = 1;

    bool start(const QStringList& command) {
        process.setProgram(command.front());
        process.setArguments(command.mid(1));
        process.start();
        return process.waitForStarted(30000);
    }
    Json request(const std::string& method, const Json& params) {
        const int id = next++;
        process.write(QByteArray::fromStdString(core::dump_python(Json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}}) + "\n"));
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < 300000) {
            const qsizetype end = buffer.indexOf('\n');
            if (end >= 0) {
                const QByteArray line = buffer.left(end);
                buffer.remove(0, end + 1);
                const Json message = core::parse_python_json(line.toStdString());
                if (message.value("id", Json()) == Json(id)) return message;
                continue;
            }
            if (!process.waitForReadyRead(1000) && process.state() == QProcess::NotRunning) break;
            buffer += process.readAllStandardOutput();
        }
        return Json{{"error", "no answer"}};
    }
    // tools/call: the tool's reply (its text content, read as JSON).
    Json call(const std::string& tool, const Json& args) {
        const Json reply = request("tools/call", Json{{"name", tool}, {"arguments", args}});
        if (!reply.contains("result")) return Json{{"ok", false}, {"error", reply.dump()}};
        return core::parse_python_json(reply["result"]["content"][0]["text"].get<std::string>());
    }
    void close() {
        process.closeWriteChannel();
        if (!process.waitForFinished(60000)) process.kill();
    }
};

}  // namespace

class TestGuiAi : public QObject {
    Q_OBJECT

    QTemporaryDir home_;
    QString log_;

    fs::path home() const { return gui_test::path_of(home_.path()); }

    QStringList methods() const {
        QFile file(log_);
        if (!file.open(QIODevice::ReadOnly)) return {};
        return QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    }

private slots:
    void initTestCase() {
        (void)gui_test::config_folder();
        qRegisterMetaType<app::BookChange>();
        // the AI clients' settings in a home folder of the test's own: never the person's
        QVERIFY(home_.isValid());
        qputenv("HOME", home_.path().toUtf8());
        qputenv("XDG_CONFIG_HOME", (home_.path() + QStringLiteral("/.config")).toUtf8());
        qputenv("XDG_DATA_HOME", (home_.path() + QStringLiteral("/.local/share")).toUtf8());
        qputenv("APPDATA", (home_.path() + QStringLiteral("/AppData/Roaming")).toUtf8());
        qputenv("LOCALAPPDATA", (home_.path() + QStringLiteral("/AppData/Local")).toUtf8());
        log_ = home_.path() + QStringLiteral("/fake-mcp.log");
        qputenv("GENKO_FAKE_MCP_LOG", log_.toUtf8());
        app::ai_link::set_command_for_tests(QStringList{fake_mcp(), QStringLiteral("--mode"), QStringLiteral("ok")});
    }

    void cleanup() {
        app::ai_link::set_command_for_tests(QStringList{fake_mcp(), QStringLiteral("--mode"), QStringLiteral("ok")});
        app::settings()->remove(QStringLiteral("ai/client"));
    }

    void settingsFilesAreTheTests() {
        QCOMPARE(QDir::homePath(), home_.path());
        const auto desktop = app::client_config::default_file(QStringLiteral("claude-desktop"));
        const auto hermes = app::client_config::default_file(QStringLiteral("hermes"));
        QVERIFY(desktop && hermes);
        QVERIFY(gui_test::qpath(*desktop).startsWith(home_.path() + QLatin1Char('/')));
        QCOMPARE(gui_test::qpath(*desktop), home_.path() + QStringLiteral("/.config/Claude/claude_desktop_config.json"));
        QCOMPARE(gui_test::qpath(*hermes), home_.path() + QStringLiteral("/.hermes/config.yaml"));
        QVERIFY(!app::client_config::default_file(QStringLiteral("claude-code")));
        QVERIFY(!app::client_config::default_file(QStringLiteral("other")));
    }

    // Python's dialog: the clients, the text to paste for this computer, the folder, the first request, the card.
    void dialogAsPython() {
        QTemporaryDir tmp;
        const fs::path root = gui_test::path_of(tmp.path());
        const fs::path book = agent_book(root, "summer.genko");
        Desk desk(book, root / "recovery");
        QCOMPARE(desk.window->ai_button()->text(), QStringLiteral("AI と作る"));
        desk.with_dialog([&](app::AiDialog* d) {
            QCOMPARE(d->windowTitle(), QStringLiteral("AI と作る"));
            QCOMPARE(d->client->count(), 4);
            QCOMPARE(d->client->itemText(0), QStringLiteral("Claude Desktop"));
            QCOMPARE(d->client->itemText(3), QStringLiteral("そのほかの MCP クライアント"));
            QCOMPARE(d->client_key(), QStringLiteral("claude-desktop"));
            QCOMPARE(d->root->text(), QDir::toNativeSeparators(tmp.path()));
            const auto snippet = app::ai_link::snippet(QStringLiteral("claude-desktop"), app::ai_link::root_text(tmp.path()));
            QCOMPARE(d->text->toPlainText(), snippet.text);
            QVERIFY(snippet.text.contains(QStringLiteral("\"--agent\",\n        \"ai:claude-desktop\"")));
            QCOMPARE(d->where->text(), snippet.where);
            QCOMPARE(d->ask->toPlainText(), app::ai_link::first_request(book));
            QCOMPARE(d->people->text(), QStringLiteral("この原稿では、まだ AI が作業していません。"));
            QCOMPARE(d->state_word->text(), QStringLiteral("未設定"));
            QVERIFY(d->write_button->isVisibleTo(d));
            // another AI: its text, and it is remembered
            d->client->setCurrentIndex(2);
            QCOMPARE(d->text->toPlainText(), app::ai_link::snippet(QStringLiteral("hermes"), app::ai_link::root_text(tmp.path())).text);
            QVERIFY(d->text->toPlainText().startsWith(QStringLiteral("mcp_servers:\n  genko:\n")));
            d->client->setCurrentIndex(1);  // (Claude Code: a command to run; its settings are not a file Genko writes)
            QVERIFY(d->text->toPlainText().startsWith(QStringLiteral("claude mcp add genko -- ")));
            QVERIFY(!d->write_button->isVisibleTo(d));
            // the copied text, and the button says so for a moment
            d->copy->click();
            QCOMPARE(QGuiApplication::clipboard()->text(), d->text->toPlainText());
            QCOMPARE(d->copy->text(), QStringLiteral("コピーしました"));
            QVERIFY(gui_test::wait_for([&] { return d->copy->text() == QStringLiteral("設定をコピー"); }, 60000));
            QCOMPARE(d->state_word->text(), QStringLiteral("設定済み・未接続"));  // (the person set it up: Genko cannot see Claude Code's)
            d->root->setText(tmp.path() + QStringLiteral("/../") + QDir(tmp.path()).dirName() + QStringLiteral("/"));
            QVERIFY(!d->text->toPlainText().contains(QStringLiteral("//")));
        });
        QCOMPARE(app::settings()->value(QStringLiteral("ai/client")).toString(), QStringLiteral("claude-code"));
        desk.with_dialog([&](app::AiDialog* d) { QCOMPARE(d->client_key(), QStringLiteral("claude-code")); });
        // the AIs that wrote the book, newest first; the status line
        studio::touch(book, "ai:hermes/9204", now_seconds() - 12 * 60);
        studio::touch(book, "ai:claude-desktop", now_seconds() - 30);
        desk.window->refresh_ai();
        QCOMPARE(desk.window->ai_button()->text(), QStringLiteral("● AI: claude-desktop 作業中 ほか 1"));
        desk.with_dialog([&](app::AiDialog* d) {
            QCOMPARE(d->people->text(), QStringLiteral("claude-desktop — <b>● 作業中</b><br>hermes（9204） — 12 分前"));
            QCOMPARE(d->state_word->text(), QStringLiteral("処理中"));
        });
    }

    // Each of the six states, from its facts; 失敗 on the status line too.
    void connectionStates() {
        QTemporaryDir tmp;
        const fs::path root = gui_test::path_of(tmp.path());
        const fs::path book = agent_book(root, "states.genko");
        Desk desk(book, root / "recovery");
        app::settings()->setValue(QStringLiteral("ai/client"), QStringLiteral("claude-desktop"));
        const auto config = *app::client_config::default_file(QStringLiteral("claude-desktop"));
        QFile::remove(gui_test::qpath(config));
        QString rows;
        desk.with_dialog([&](app::AiDialog* d) {
            QCOMPARE(d->link(), app::ai_link::Link::Unset);
            QCOMPARE(d->state_word->text(), QStringLiteral("未設定"));
            QVERIFY(d->state_rows->text().contains(QStringLiteral("<b>テスト接続</b>: まだしていません")));
            QVERIFY(d->state_rows->text().contains(QStringLiteral("<b>最後の呼び出し</b>: まだありません")));
            QVERIFY(d->state_rows->text().contains(QStringLiteral("<b>確認待ち</b>: なし")));
            // set up: the client's file names Genko's server
            const auto proposal = app::client_config::propose(QStringLiteral("claude-desktop"), config,
                                                              app::ai_link::snippet(QStringLiteral("claude-desktop"), d->root_typed()).server);
            app::client_config::apply(proposal);
            d->fill();
            QCOMPARE(d->state_word->text(), QStringLiteral("設定済み・未接続"));
            // the test connection answered
            d->test_connection();
            QVERIFY(gui_test::wait_for([&] { return !d->probe()->running(); }, 120000));
            QCOMPARE(d->state_word->text(), QStringLiteral("接続済み"));
            QVERIFY(d->state_rows->text().contains(QStringLiteral("成功")));
            // an AI writing now
            studio::touch(book, "ai:claude-desktop", now_seconds());
            d->fill();
            QCOMPARE(d->state_word->text(), QStringLiteral("処理中"));
            QVERIFY(d->state_rows->text().contains(QStringLiteral("<b>作業</b>: 処理中")));
        });
        // a request waiting for the person (written by the AI into the book; the window reads it)
        service::StudioService ai(root, "ai:claude-desktop");
        QVERIFY(ai.call("request_approval", Json::array(), Json{{"project", "states.genko"}, {"gate", "name"}, {"pages", Json::array({1})}}).ok);
        desk.session->check_outside();
        QVERIFY(gui_test::wait_for([&] { return desk.window->approvals()->items().size() == 1; }, 60000));
        desk.with_dialog([&](app::AiDialog* d) {
            QCOMPARE(d->state_word->text(), QStringLiteral("人の確認待ち"));
            QVERIFY(d->state_rows->text().contains(QStringLiteral("<b>確認待ち</b>: 1 件（承認箱）")));
        });
        // the AI's last call failed (after the test): 失敗, with why
        studio::record_tool(book, "ai:claude-desktop", "submit_name", Json{{"project", "states.genko"}}, std::nullopt,
                            std::string("1 ページのネームが読めない"), core::Num(3), now_seconds() + 1);
        desk.with_dialog([&](app::AiDialog* d) {
            QCOMPARE(d->link(), app::ai_link::Link::Failed);
            QCOMPARE(d->state_word->text(), QStringLiteral("失敗"));
            rows = d->state_rows->text();
        });
        QVERIFY2(rows.contains(QStringLiteral("claude-desktop の submit_name: 失敗 — 1 ページのネームが読めない")), qPrintable(rows));
        desk.window->refresh_ai();
        QVERIFY(desk.window->ai_button()->text().endsWith(QStringLiteral("・失敗")));
        QVERIFY(desk.window->ai_button()->toolTip().contains(QStringLiteral("1 ページのネームが読めない")));
        // a call that worked after it: no longer failed
        studio::record_tool(book, "ai:claude-desktop", "status", Json{{"project", "states.genko"}}, Json{{"ok", true}}, std::nullopt, core::Num(1),
                            now_seconds() + 2);
        desk.window->refresh_ai();
        QVERIFY(!desk.window->ai_button()->text().contains(QStringLiteral("失敗")));
        desk.with_dialog([&](app::AiDialog* d) { QCOMPARE(d->state_word->text(), QStringLiteral("人の確認待ち")); });
        QFile::remove(gui_test::qpath(config));
    }

    // Claude Desktop's settings: the change seen, cancelled, written, put back; the other servers kept and never shown.
    void claudeDesktopSettings() {
        QTemporaryDir tmp;
        const fs::path root = gui_test::path_of(tmp.path());
        const fs::path book = agent_book(root, "cfg.genko");
        Desk desk(book, root / "recovery");
        app::settings()->setValue(QStringLiteral("ai/client"), QStringLiteral("claude-desktop"));
        const fs::path config = *app::client_config::default_file(QStringLiteral("claude-desktop"));
        const QString file = gui_test::qpath(config);
        QFile::remove(file);
        QDir(gui_test::qpath(config.parent_path())).removeRecursively();
        // no file: the change shown, then cancelled — nothing written
        QStringList seen;
        desk.on_change = [&](app::ConfigChangeDialog* c) {
            seen << c->file->path() << c->before->text() << c->after->text() << c->kept->text() << c->backup->text();
            return QDialog::Rejected;
        };
        desk.with_dialog([&](app::AiDialog* d) {
            d->write_config();
            QVERIFY(d->applied_note->text().contains(QStringLiteral("取りやめ")));
        });
        QVERIFY(!QFile::exists(file));
        QCOMPARE(seen.value(0), QDir::toNativeSeparators(file));
        QCOMPARE(seen.value(1), QStringLiteral("（ファイルがまだありません。新しく作ります）"));
        QVERIFY(seen.value(2).startsWith(QStringLiteral("command: \"") + fake_mcp()));
        QVERIFY(seen.value(2).contains(QStringLiteral("\"mcp\", \"--root\"")));
        QCOMPARE(seen.value(4), QStringLiteral("（元のファイルが無いので要りません）"));
        // written: Python's text to paste, as the file
        desk.on_change = [](app::ConfigChangeDialog* c) {
            c->write_button->click();
            return c->result();
        };
        QString snippet;
        desk.with_dialog([&](app::AiDialog* d) {
            snippet = d->text->toPlainText();
            d->write_config();
            QCOMPARE(d->state_word->text(), QStringLiteral("設定済み・未接続"));
            QVERIFY(d->undo_button->isEnabled());
        });
        QCOMPARE(QString::fromStdString(genko::test::read_bytes(file)), snippet + QStringLiteral("\n"));
        // put back: the file Genko made is gone
        desk.yes = {true};
        desk.with_dialog([&](app::AiDialog* d) { d->undo_config(); });
        QVERIFY(!QFile::exists(file));
        QCOMPARE(desk.questions.size(), 1);

        // a file with other servers (one with a key in its env) and settings
        const std::string original = R"({"globalShortcut": "Ctrl+Space",
  "mcpServers": {"files": {"command": "npx", "args": ["-y", "files"], "env": {"API_KEY": "sk-secret-123"}},
                 "genko": {"command": "/old/genko", "args": ["mcp"], "env": {"TOKEN": "tok-secret-456"}}}}
)";
        QDir().mkpath(gui_test::qpath(config.parent_path()));
        genko::test::write_bytes(file, original);
        QFile::setPermissions(file, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        QStringList shown;
        desk.on_change = [&](app::ConfigChangeDialog* c) {
            for (QLabel* label : c->findChildren<QLabel*>()) shown << label->text();
            c->write_button->click();
            return c->result();
        };
        QString backup;
        desk.with_dialog([&](app::AiDialog* d) {
            d->write_config();
            const auto copies = app::client_config::backups(config);
            QCOMPARE(copies.size(), std::size_t(1));
            backup = gui_test::qpath(copies.front());
            QVERIFY(d->applied_note->text().contains(QDir::toNativeSeparators(backup)));
        });
        const QString everything = shown.join(QLatin1Char('\n'));
        QVERIFY(everything.contains(QStringLiteral("command: \"/old/genko\"")));
        QVERIFY(everything.contains(QStringLiteral("ほかのサーバー: files")));
        QVERIFY(everything.contains(QStringLiteral("ほかの設定: globalShortcut")));
        QVERIFY(everything.contains(QStringLiteral("genko のほかの項目: env")));
        QVERIFY2(!everything.contains(QStringLiteral("secret")), qPrintable(everything));  // (no value of the other settings on the screen)
        QCOMPARE(genko::test::read_bytes(backup), original);
        QCOMPARE(QFileInfo(backup).permissions() & 0x7777, QFileInfo(file).permissions() & 0x7777);
        QCOMPARE(QFileInfo(file).permissions() & (QFileDevice::ReadOther | QFileDevice::ReadGroup), QFileDevice::Permissions());
        const Json written = genko::test::read_json(file);
        QCOMPARE(written["globalShortcut"], Json("Ctrl+Space"));
        QCOMPARE(written["mcpServers"]["files"]["env"]["API_KEY"], Json("sk-secret-123"));
        QCOMPARE(written["mcpServers"]["genko"]["env"]["TOKEN"], Json("tok-secret-456"));
        QCOMPARE(written["mcpServers"]["genko"]["command"], Json(fake_mcp().toStdString()));
        QCOMPARE(written["mcpServers"]["genko"]["args"][0], Json("--mode"));
        // the same again: nothing to write, no copy
        desk.warnings.clear();
        desk.with_dialog([&](app::AiDialog* d) { d->write_config(); });
        QCOMPARE(desk.warnings.size(), 1);
        QVERIFY(desk.warnings.value(0).contains(QStringLiteral("すでに同じ設定")));
        QCOMPARE(app::client_config::backups(config).size(), std::size_t(1));
        // the client changed the file after: put back, its state kept aside first
        const std::string later = genko::test::read_bytes(file) + " ";
        genko::test::write_bytes(file, later);
        desk.questions.clear();
        desk.yes = {true};
        desk.with_dialog([&](app::AiDialog* d) {
            d->undo_config();
            QVERIFY(d->applied_note->text().contains(QStringLiteral("戻す前のファイルの写し")));
        });
        QVERIFY(desk.questions.value(0).contains(QStringLiteral("書き込んだ後に変わっています")));
        QCOMPARE(genko::test::read_bytes(file), original);
        // (the file as it was before the undo is kept apart from Genko's copies: no later undo puts it back)
        const auto copies = app::client_config::backups(config);
        QCOMPARE(copies.size(), std::size_t(1));
        const QStringList asides = QDir(gui_test::qpath(config.parent_path())).entryList({config.filename().string().c_str() + QStringLiteral(".genko-before-undo-*")});
        QCOMPARE(asides.size(), 1);
        QCOMPARE(genko::test::read_bytes(gui_test::qpath(config.parent_path()) + QLatin1Char('/') + asides.front()), later);
        // pressed again: nothing to put back (the file is the copy's): no question, nothing changed
        desk.questions.clear();
        desk.yes = {true};
        desk.with_dialog([&](app::AiDialog* d) {
            QVERIFY(!d->undo_button->isEnabled());
            d->undo_config();
        });
        QVERIFY(desk.questions.isEmpty());
        QCOMPARE(genko::test::read_bytes(file), original);
        QCOMPARE(QDir(gui_test::qpath(config.parent_path())).entryList({config.filename().string().c_str() + QStringLiteral(".genko-before-undo-*")}).size(), 1);
        // a file that is not JSON: refused, untouched
        genko::test::write_bytes(file, "{ not json");
        desk.warnings.clear();
        desk.on_change = [](app::ConfigChangeDialog*) {
            qFatal("the change was offered for a file that cannot be read");
            return 0;
        };
        desk.with_dialog([&](app::AiDialog* d) { d->write_config(); });
        QVERIFY(desk.warnings.value(0).contains(QStringLiteral("JSON として読めません")));
        QCOMPARE(genko::test::read_bytes(file), std::string("{ not json"));
        QDir(gui_test::qpath(config.parent_path())).removeRecursively();
    }

    // Claude Desktop's file Genko does not change (nothing is written): numbers JSON cannot carry back as they are (NaN,
    // 1e400, an integer beyond 64 bits), a key twice in one object, a file that is not UTF-8.
    void claudeDesktopFileRefused_data() {
        QTest::addColumn<QByteArray>("before");
        QTest::addColumn<QString>("refusal");
        QTest::newRow("NaN") << QByteArray(R"({"a": NaN, "mcpServers": {}})") << QStringLiteral("ふつうの数で表せない数があります（/a）");
        QTest::newRow("1e400") << QByteArray(R"({"mcpServers": {"x": {"timeout": 1e400}}})") << QStringLiteral("ふつうの数で表せない数があります（/mcpServers/x/timeout）");
        QTest::newRow("beyond 64 bits") << QByteArray(R"({"id": 123456789012345678901234567890})") << QStringLiteral("64 ビットに収まらない大きな整数があります（/id）");
        QTest::newRow("a key twice") << QByteArray(R"({"mcpServers": {}, "mcpServers": {"x": {"command": "y"}}})") << QStringLiteral("同じ鍵（mcpServers）が二つ");
        QTest::newRow("a key twice inside") << QByteArray(R"({"mcpServers": {"genko": {"command": "a", "command": "b"}}})") << QStringLiteral("同じ鍵（command）が二つ");
        QTest::newRow("utf-16") << QByteArray("\xFF\xFE{\x00}\x00", 6) << QStringLiteral("UTF-16 か UTF-32");
        QTest::newRow("utf-32 be") << QByteArray("\x00\x00\xFE\xFF\x00\x00\x00{\x00\x00\x00}", 12) << QStringLiteral("UTF-16 か UTF-32");
        QTest::newRow("not utf-8") << QByteArray("{\"a\": \"\xFF\"}") << QStringLiteral("UTF-8 ではありません");
    }
    void claudeDesktopFileRefused() {
        QFETCH(QByteArray, before);
        QFETCH(QString, refusal);
        QTemporaryDir tmp;
        const fs::path file = gui_test::path_of(tmp.path() + QStringLiteral("/claude_desktop_config.json"));
        genko::test::write_bytes(gui_test::qpath(file), before.toStdString());
        const auto proposal = app::client_config::propose(QStringLiteral("claude-desktop"), file, {QStringLiteral("/g"), QStringLiteral("mcp")});
        QVERIFY2(proposal.refusal.contains(refusal), qPrintable(proposal.refusal));
        QVERIFY_THROWS_EXCEPTION(core::Error, app::client_config::apply(proposal));
        QCOMPARE(QByteArray::fromStdString(genko::test::read_bytes(gui_test::qpath(file))), before);
    }

    // A UTF-8 BOM at the start of Claude Desktop's file is read past and written back (and said to be kept).
    void claudeDesktopFileWithBom() {
        QTemporaryDir tmp;
        const fs::path file = gui_test::path_of(tmp.path() + QStringLiteral("/claude_desktop_config.json"));
        genko::test::write_bytes(gui_test::qpath(file), "\xEF\xBB\xBF{\"theme\": \"dark\"}\n");
        const auto proposal = app::client_config::propose(QStringLiteral("claude-desktop"), file, {QStringLiteral("/g"), QStringLiteral("mcp")});
        QVERIFY2(proposal.refusal.isEmpty(), qPrintable(proposal.refusal));
        QVERIFY(proposal.kept.join(QLatin1Char('\n')).contains(QStringLiteral("BOM")));
        app::client_config::apply(proposal);
        const std::string written = genko::test::read_bytes(gui_test::qpath(file));
        QVERIFY(written.starts_with("\xEF\xBB\xBF{"));
        const Json read = core::parse_python_json(written.substr(3));
        QCOMPARE(read["theme"], Json("dark"));
        QCOMPARE(read["mcpServers"]["genko"]["command"], Json("/g"));
        QVERIFY(app::client_config::propose(QStringLiteral("claude-desktop"), file, {QStringLiteral("/g"), QStringLiteral("mcp")}).same);
    }

    // Claude Desktop from the Microsoft Store (MSIX) keeps its file in its package's folder: that folder is found under
    // %LOCALAPPDATA%\Packages\Claude_* (looked at first on Windows; to be seen on a real Windows in M6).
    void msixSettingsFolder() {
        QTemporaryDir local;
        QVERIFY(!app::client_config::msix_claude_file(local.path()));
        QVERIFY(!app::client_config::msix_claude_file(QString()));
        QDir().mkpath(local.path() + QStringLiteral("/Packages/Other_1/LocalCache/Roaming/Claude"));
        QDir().mkpath(local.path() + QStringLiteral("/Packages/Claude_pzs8sxrjxfjjc/LocalCache"));
        QVERIFY(!app::client_config::msix_claude_file(local.path()));  // (the package without its Claude folder yet)
        QDir().mkpath(local.path() + QStringLiteral("/Packages/Claude_pzs8sxrjxfjjc/LocalCache/Roaming/Claude"));
        const auto found = app::client_config::msix_claude_file(local.path());
        QVERIFY(found);
        QCOMPARE(gui_test::qpath(*found), local.path() + QStringLiteral("/Packages/Claude_pzs8sxrjxfjjc/LocalCache/Roaming/Claude/claude_desktop_config.json"));
    }

    // The test connection with a folder that is not there: nothing started, no folder made, the reason said.
    void testConnectionWithoutTheFolder() {
        QTemporaryDir tmp;
        const fs::path root = gui_test::path_of(tmp.path());
        const fs::path book = agent_book(root, "nofolder.genko");
        Desk desk(book, root / "recovery");
        QFile::remove(log_);
        const QString missing = tmp.path() + QStringLiteral("/no/such/folder");
        desk.with_dialog([&](app::AiDialog* d) {
            d->root->setText(missing);
            d->test_connection();
            QVERIFY(d->test_button->isEnabled());
            QVERIFY(d->probe()->sent().isEmpty());
            QVERIFY(d->probe()->findChildren<QProcess*>().isEmpty());
            QCOMPARE(d->state_word->text(), QStringLiteral("失敗"));
            QVERIFY2(d->state_rows->text().contains(QStringLiteral("がありません")), qPrintable(d->state_rows->text()));
        });
        QVERIFY(!QFileInfo::exists(missing));
        QVERIFY(!QFileInfo::exists(tmp.path() + QStringLiteral("/no")));
        QVERIFY(methods().isEmpty());
    }

    // Hermes Agent's YAML: Python's text for a new file; the rest of a file kept, Genko's entry replaced or added; a file of
    // a shape Genko cannot change safely refused.
    void hermesSettings_data() {
        QTest::addColumn<QString>("before");
        QTest::addColumn<QString>("after");  // (#C, #A: the command and the arguments; "" with a refusal)
        QTest::addColumn<QString>("refusal");
        const QString fresh = QStringLiteral("mcp_servers:\n  genko:\n    command: #C\n    args: #A\n    timeout: 60\n");
        QTest::newRow("none") << QString() << fresh << QString();
        QTest::newRow("empty") << QStringLiteral("\n\n") << fresh << QString();
        QTest::newRow("other settings") << QStringLiteral("model: claude\nproviders:\n  a: 1\n")
                                        << QStringLiteral("model: claude\nproviders:\n  a: 1\n") + fresh << QString();
        QTest::newRow("no line end") << QStringLiteral("model: claude") << QStringLiteral("model: claude\n") + fresh << QString();
        QTest::newRow("other servers") << QStringLiteral("mcp_servers:\n  files:\n    command: npx\n    env:\n      KEY: secret\n# end\nmodel: x\n")
                                       << QStringLiteral("mcp_servers:\n  files:\n    command: npx\n    env:\n      KEY: secret\n  genko:\n    command: #C\n"
                                                         "    args: #A\n    timeout: 60\n# end\nmodel: x\n")
                                       << QString();
        QTest::newRow("genko there") << QStringLiteral("mcp_servers:\n    genko:\n        # mine\n        command: /old\n        env:\n            T: x\n"
                                                       "        args: [\"mcp\"]\n        timeout: 120\n    other:\n        command: y\n")
                                     << QStringLiteral("mcp_servers:\n    genko:\n        # mine\n        command: #C\n        env:\n            T: x\n"
                                                       "        args: #A\n        timeout: 120\n    other:\n        command: y\n")
                                     << QString();
        QTest::newRow("empty map") << QStringLiteral("mcp_servers: {}\n") << fresh << QString();
        QTest::newRow("windows lines") << QStringLiteral("model: x\r\n") << QStringLiteral("model: x\r\n") + QString(fresh).replace(QStringLiteral("\n"), QStringLiteral("\r\n"))
                                       << QString();
        QTest::newRow("inline map") << QStringLiteral("mcp_servers: {a: {command: x}}\n") << QString() << QStringLiteral("一行で書かれている");
        QTest::newRow("tabs") << QStringLiteral("mcp_servers:\n\tgenko:\n") << QString() << QStringLiteral("タブの字下げ");
        QTest::newRow("document start") << QStringLiteral("---\nmodel: x\n") << QStringLiteral("---\nmodel: x\n") + fresh << QString();
        QTest::newRow("document start, servers") << QStringLiteral("# head\n--- # one\nmcp_servers:\n  a:\n    command: y\n")
                                                 << QStringLiteral("# head\n--- # one\nmcp_servers:\n  a:\n    command: y\n  genko:\n    command: #C\n"
                                                                   "    args: #A\n    timeout: 60\n")
                                                 << QString();
        QTest::newRow("documents") << QStringLiteral("---\nmodel: x\n---\nmodel: y\n") << QString() << QStringLiteral("複数の文書");
        QTest::newRow("document end") << QStringLiteral("model: x\n...\n") << QString() << QStringLiteral("複数の文書");
        QTest::newRow("flow document") << QStringLiteral("{\"model\": \"x\", \"mcp_servers\": {}}\n") << QString() << QStringLiteral("「{」で始まる");
        QTest::newRow("flow list") << QStringLiteral("[a, b]\n") << QString() << QStringLiteral("「[」で始まる");
        QTest::newRow("explicit key") << QStringLiteral("? mcp_servers\n: {other: {command: y}}\n") << QString() << QStringLiteral("「?」で始まる");
        QTest::newRow("anchor") << QStringLiteral("&base model: x\n") << QString() << QStringLiteral("「&」で始まる");
        QTest::newRow("merge") << QStringLiteral("base: {command: y}\nmcp_servers:\n  <<: {a: 1}\n") << QString() << QStringLiteral("<< で別の鍵を取り込んでいる");
        QTest::newRow("tag in genko") << QStringLiteral("mcp_servers:\n  genko:\n    !x command: y\n") << QString() << QStringLiteral("「!」で始まる");
        QTest::newRow("block scalar at the edge") << QStringLiteral("model: x\n|\n  text\n") << QString() << QStringLiteral("「|」で始まる");
        QTest::newRow("a line that is not a key") << QStringLiteral("model: x\nplain words\n") << QString() << QStringLiteral("鍵として読めない行");
        QTest::newRow("comments kept") << QStringLiteral("mcp_servers:\n  genko:\n    command: /old\n    # the arguments\n    args: [\"x\"]\n    # keep me\n\n"
                                                         "  other:\n    command: y\n# the end\n")
                                       << QStringLiteral("mcp_servers:\n  genko:\n    command: #C\n    # the arguments\n    args: #A\n    timeout: 60\n    # keep me\n\n"
                                                         "  other:\n    command: y\n# the end\n")
                                       << QString();
        QTest::newRow("comments after the last key") << QStringLiteral("mcp_servers:\n  genko:\n    args: [\"x\"]\n    command: /old\n    timeout: 9\n\n# after\nmodel: m\n")
                                                     << QStringLiteral("mcp_servers:\n  genko:\n    command: #C\n    args: #A\n    timeout: 9\n\n# after\nmodel: m\n")
                                                     << QString();
        QTest::newRow("utf-8 bom") << QStringLiteral("\uFEFFmodel: x\n") << QStringLiteral("\uFEFFmodel: x\n") + fresh << QString();
        QTest::newRow("genko inline") << QStringLiteral("mcp_servers:\n  genko: {command: x}\n") << QString() << QStringLiteral("genko が一行で書かれている");
        QTest::newRow("two") << QStringLiteral("mcp_servers:\n  a: 1\nmcp_servers:\n  b: 2\n") << QString() << QStringLiteral("mcp_servers が二つ");
    }
    void hermesSettings() {
        QFETCH(QString, before);
        QFETCH(QString, after);
        QFETCH(QString, refusal);
        QTemporaryDir tmp;
        const fs::path file = gui_test::path_of(tmp.path() + QStringLiteral("/config.yaml"));
        if (!before.isNull()) genko::test::write_bytes(gui_test::qpath(file), before.toStdString());
        const QStringList server = {QStringLiteral("/作業/genko"), QStringLiteral("mcp"), QStringLiteral("--root"), QStringLiteral("/r s"),
                                    QStringLiteral("--agent"), QStringLiteral("ai:hermes")};
        const auto proposal = app::client_config::propose(QStringLiteral("hermes"), file, server);
        if (!refusal.isEmpty()) {
            QVERIFY2(proposal.refusal.contains(refusal), qPrintable(proposal.refusal));
            QVERIFY_THROWS_EXCEPTION(core::Error, app::client_config::apply(proposal));
            QCOMPARE(QByteArray::fromStdString(genko::test::read_bytes(gui_test::qpath(file))), before.toUtf8());
            return;
        }
        QVERIFY2(proposal.refusal.isEmpty(), qPrintable(proposal.refusal));
        const QString expected = QString(after)
                                     .replace(QStringLiteral("#C"), QStringLiteral("\"/作業/genko\""))
                                     .replace(QStringLiteral("#A"), QStringLiteral("[\"mcp\", \"--root\", \"/r s\", \"--agent\", \"ai:hermes\"]"));
        // (compared as bytes: QString::fromUtf8 reads past a BOM)
        QCOMPARE(proposal.after, expected.toUtf8());
        const auto applied = app::client_config::apply(proposal);
        QCOMPARE(QByteArray::fromStdString(genko::test::read_bytes(gui_test::qpath(file))), expected.toUtf8());
        QCOMPARE(applied.backup.empty(), before.isNull());
        QVERIFY(app::client_config::configured(QStringLiteral("hermes"), file));
        // written again: the same, nothing to do
        QVERIFY(app::client_config::propose(QStringLiteral("hermes"), file, server).same);
        app::client_config::undo(applied);
        if (before.isNull()) {
            QVERIFY(!QFile::exists(gui_test::qpath(file)));
        } else {
            QCOMPARE(QByteArray::fromStdString(genko::test::read_bytes(gui_test::qpath(file))), before.toUtf8());
        }
    }

    // The test connection: only initialize and tools/list; the book unchanged; each failure with its reason.
    void testConnection_data() {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<int>("timeout");
        QTest::addColumn<bool>("ok");
        QTest::addColumn<QString>("words");
        QTest::addColumn<QString>("detail");
        QTest::newRow("answers") << "ok" << 120000 << true << QStringLiteral("Genko の MCP サーバーが応答しました（genko fake、道具") << QString();
        QTest::newRow("ends") << "fail" << 120000 << false << QStringLiteral("応答の前に終わりました（終了コード 3）")
                              << QStringLiteral("genko mcp: the root folder cannot be read (fake)");
        QTest::newRow("not mcp") << "garbage" << 120000 << false << QStringLiteral("MCP ではない出力がありました: this is not json") << QString();
        QTest::newRow("refuses") << "reject" << 120000 << false << QStringLiteral("初期化を断られました: unsupported protocol version (fake)") << QString();
        QTest::newRow("silent") << "hang" << 1500 << false << QStringLiteral("秒たっても応答がありませんでした") << QString();
        QTest::newRow("missing") << "missing" << 120000 << false << QStringLiteral("起動できませんでした: ") << QString();
    }
    void testConnection() {
        QFETCH(QString, mode);
        QFETCH(int, timeout);
        QFETCH(bool, ok);
        QFETCH(QString, words);
        QFETCH(QString, detail);
        QTemporaryDir tmp;
        const fs::path root = gui_test::path_of(tmp.path());
        const fs::path book = agent_book(root, "probe.genko");
        Desk desk(book, root / "recovery");
        QVERIFY(desk.session->wait_saved(std::chrono::seconds(30)));
        const auto hashes = genko::test::tree_hashes(gui_test::qpath(book), false);
        QFile::remove(log_);
        QStringList sent;
        app::ai_link::set_command_for_tests(mode == QLatin1String("missing") ? QStringList{tmp.path() + QStringLiteral("/no-such-genko")}
                                                                             : QStringList{fake_mcp(), QStringLiteral("--mode"), mode});
        desk.with_dialog([&](app::AiDialog* d) {
            d->probe_timeout_ms = timeout;
            QSignalSpy finished(d->probe(), &app::McpProbe::finished);
            d->test_connection();
            QVERIFY(!d->test_button->isEnabled());
            QCOMPARE(d->test_button->text(), QStringLiteral("確かめています…"));
            QVERIFY(gui_test::wait_for([&] { return finished.count() == 1; }, 180000));
            const auto result = finished.at(0).at(0).value<app::ai_link::Probe>();
            QCOMPARE(result.ok, ok);
            QVERIFY2(result.words.contains(words), qPrintable(result.words));
            if (!detail.isEmpty()) QVERIFY2(result.detail.contains(detail), qPrintable(result.detail));
            QVERIFY(d->test_button->isEnabled());
            QCOMPARE(d->state_word->text(), ok ? QStringLiteral("接続済み") : QStringLiteral("失敗"));
            QVERIFY(d->state_rows->text().contains(ok ? QStringLiteral("成功") : QStringLiteral("失敗")));
            // the server never outlives the test
            QVERIFY(gui_test::wait_for([&] { return d->probe()->findChildren<QProcess*>().isEmpty(); }, 60000));
            sent = d->probe()->sent();
        });
        // what the test sent (the server may end before it reads a last line: the probe's own record decides)
        const QStringList asked = mode == QLatin1String("missing") ? QStringList()
                                  : mode == QLatin1String("ok")    ? QStringList({"initialize", "notifications/initialized", "tools/list"})
                                                                   : QStringList({"initialize"});
        QCOMPARE(sent, asked);
        QVERIFY(!sent.contains(QStringLiteral("tools/call")));
        if (mode == QLatin1String("ok")) QCOMPARE(methods(), (QStringList{"initialize", "notifications/initialized", "tools/list"}));
        QVERIFY(!methods().contains(QStringLiteral("tools/call")));  // (nothing generated, charged or written)
        QCOMPARE(genko::test::tree_hashes(gui_test::qpath(book), false), hashes);
    }

    // The book and the conversation: the name checked as `genko mcp` checks it; kept by the window; the first request.
    void bookAndConversation() {
        QTemporaryDir tmp;
        const fs::path root = gui_test::path_of(tmp.path());
        const fs::path a = agent_book(root, "a.genko");
        agent_book(root, "b.genko");
        studio::touch(a, "ai:claude-desktop/telegram-77", now_seconds() - 60);
        studio::touch(a, "ai:hermes/x", now_seconds() - 30);
        Desk desk(a, root / "recovery");
        app::settings()->setValue(QStringLiteral("ai/client"), QStringLiteral("claude-desktop"));
        desk.with_dialog([&](app::AiDialog* d) {
            QCOMPARE(d->book_pick->count(), 2);
            QCOMPARE(d->book_pick->currentText(), QStringLiteral("a.genko"));
            QCOMPARE(d->book_pick->itemText(1), QStringLiteral("b.genko"));
            QCOMPARE(d->session_pick->count(), 2);  // (none, and the client's own conversation seen in the book)
            QCOMPARE(d->session_pick->itemText(1), QStringLiteral("telegram-77"));
            d->session_pick->setEditText(QStringLiteral("a b"));
            QVERIFY(d->session_note->isVisibleTo(d));
            QCOMPARE(d->session_note->text(), QStringLiteral("session は 1〜40 文字の英数字と _ . : @ - だけ（例 9204、telegram-9204）"));
            QCOMPARE(d->ask->toPlainText(), app::ai_link::first_request(a));
            d->session_pick->setEditText(QStringLiteral("9204"));
            QVERIFY(!d->session_note->isVisibleTo(d));
            QCOMPARE(d->ask->toPlainText(), app::ai_link::first_request(a) + QStringLiteral("この会話では、どの道具にも session に「9204」を渡してください。"));
            d->book_pick->setCurrentIndex(1);
            QVERIFY(d->ask->toPlainText().startsWith(QStringLiteral("Genko の「b.genko」で")));
            QVERIFY(d->target_label->text().contains(QStringLiteral("b.genko")));
            QVERIFY(d->state_rows->text().contains(QStringLiteral("<b>依頼先</b>: 原稿「b.genko」・会話「9204」")));
        });
        QCOMPARE(desk.window->ai_target().session, QStringLiteral("9204"));
        QVERIFY(desk.window->ai_target().book && desk.window->ai_target().book->filename() == "b.genko");
        desk.with_dialog([&](app::AiDialog* d) {  // (kept while the window is open)
            QCOMPARE(d->book_pick->currentText(), QStringLiteral("b.genko"));
            QCOMPARE(d->chosen_session(), QStringLiteral("9204"));
        });
    }

    // The first setup run through (ACCEPTANCE AC-API, the GUI's part with a stand-in client; §6), with the stand-in server.
    void firstSetupToResult() { first_setup_to_result(false); }

    // The same with this build's own `genko mcp` as the server the client starts (M5③): the settings written, the test
    // connection, the requests, the approval and the reply, read back by the AI — and what the server keeps in the books
    // (studio/logs/tools.jsonl, studio/presence.json) written as Python's genko/mcp/server.py writes them.
    void firstSetupWithGenkoMcp() { first_setup_to_result(true); }

private:
    void first_setup_to_result(bool genko_mcp) {
        if (genko_mcp) app::ai_link::set_command_for_tests(QStringList{genko::test::genko_cli()});
        QTemporaryDir tmp;
        const fs::path root = gui_test::path_of(tmp.path());
        const fs::path a = agent_book(root, "A.genko");
        const fs::path b = agent_book(root, "B.genko");
        const fs::path config = *app::client_config::default_file(QStringLiteral("claude-desktop"));
        QDir(gui_test::qpath(config.parent_path())).removeRecursively();
        app::settings()->setValue(QStringLiteral("ai/client"), QStringLiteral("claude-desktop"));
        Desk desk(a, root / "recovery");
        QVERIFY(!desk.window->box_dock()->isVisible());  // (nothing waits yet)
        // 1. the dialog: the settings file written after the change is seen and confirmed
        desk.on_change = [](app::ConfigChangeDialog* c) {
            c->write_button->click();
            return c->result();
        };
        desk.with_dialog([&](app::AiDialog* d) {
            QCOMPARE(d->state_word->text(), QStringLiteral("未設定"));
            d->write_config();
            QCOMPARE(d->state_word->text(), QStringLiteral("設定済み・未接続"));
            // 2. the test connection (as the client will start it)
            d->test_connection();
            QVERIFY(gui_test::wait_for([&] { return !d->probe()->running(); }, 120000));
            QCOMPARE(d->state_word->text(), QStringLiteral("接続済み"));
            // 3. the book and the conversation
            d->session_pick->setEditText(QStringLiteral("9204"));
            QVERIFY(d->ask->toPlainText().contains(QStringLiteral("「A.genko」")));
            QVERIFY(d->ask->toPlainText().contains(QStringLiteral("session に「9204」")));
        });
        // 4. the client started again, as its settings file says
        const Json settings = genko::test::read_json(gui_test::qpath(config));
        QStringList command{QString::fromStdString(settings["mcpServers"]["genko"]["command"].get<std::string>())};
        for (const Json& arg : settings["mcpServers"]["genko"]["args"]) command << QString::fromStdString(arg.get<std::string>());
        QCOMPARE(command, app::ai_link::snippet(QStringLiteral("claude-desktop"), app::ai_link::root_text(gui_test::qpath(root))).server);
        QVERIFY(desk.session->wait_saved(std::chrono::seconds(30)));
        Client ai;
        if (genko_mcp) QCOMPARE(command.value(0), genko::test::genko_cli());
        QVERIFY(ai.start(command));
        const Json hello = ai.request("initialize", initialize_params());
        QVERIFY2(hello.contains("result"), hello.dump().c_str());
        QCOMPARE(hello["result"].value("protocolVersion", Json()), Json("2025-06-18"));
        // 5. the first requests: two in A from conversation 9204, one in B from another conversation
        Json reply = ai.call("request_approval", Json{{"project", "A.genko"}, {"gate", "name"}, {"pages", Json::array({1})}, {"note", "1 ページのネームを見てください"},
                                                      {"session", "9204"}});
        QVERIFY2(reply.value("ok", false), reply.dump().c_str());
        reply = ai.call("ask_human", Json{{"project", "A.genko"}, {"text", "鞄の色は紺でいいですか"}, {"page", 1}, {"session", "9204"}});
        QVERIFY2(reply.value("ok", false), reply.dump().c_str());
        reply = ai.call("request_approval", Json{{"project", "B.genko"}, {"gate", "name"}, {"pages", Json::array({2})}, {"session", "7777"}});
        QVERIFY2(reply.value("ok", false), reply.dump().c_str());
        // 6. waiting for the person: in A's box, said with a note; nothing of B's
        desk.session->check_outside();
        QVERIFY(gui_test::wait_for([&] { return desk.window->approvals()->items().size() == 2; }, 60000));
        QVERIFY(gui_test::wait_for([&] { return desk.window->box_dock()->isVisible(); }, 60000));
        QLabel* note = desk.window->canvas()->findChild<QLabel*>(QStringLiteral("toast"));
        QVERIFY(note != nullptr);
        QVERIFY(note->text().startsWith(QStringLiteral("承認の依頼が届きました: ")));
        auto* box = desk.window->approvals();
        QStringList titles;
        for (const auto& item : box->items()) {
            titles << qs(item.title());
            QCOMPARE(item.by, std::string("ai:claude-desktop/9204"));
        }
        QCOMPARE(titles, (QStringList{QStringLiteral("ネームの承認: 1 ページ"), QStringLiteral("相談（1 ページ）")}));
        QVERIFY(desk.window->process_bar()->labels().front()->text().contains(QStringLiteral("承認箱 <b>2</b>")));
        desk.with_dialog([&](app::AiDialog* d) {
            QCOMPARE(d->book_pick->currentText(), QStringLiteral("A.genko"));
            QCOMPARE(d->chosen_session(), QStringLiteral("9204"));
            QCOMPARE(d->state_word->text(), QStringLiteral("人の確認待ち"));
            QVERIFY2(d->state_rows->text().contains(QStringLiteral("claude-desktop（9204） の ask_human: 成功")), qPrintable(d->state_rows->text()));
            QVERIFY(d->state_rows->text().contains(QStringLiteral("<b>確認待ち</b>: 2 件（承認箱）")));
            QVERIFY(d->people->text().contains(QStringLiteral("claude-desktop（9204） — <b>● 作業中</b>")));
            d->book_pick->setCurrentIndex(1);  // (B: its own request, from its own conversation)
            QVERIFY(d->state_rows->text().contains(QStringLiteral("<b>確認待ち</b>: 1 件（承認箱）")));
            d->book_pick->setCurrentIndex(0);
        });
        // 7. the person's approval, and a reply in two lines (asked once more before it goes)
        box->list->setCurrentRow(0);
        QCOMPARE(box->approve_button->text(), QStringLiteral("承認"));
        box->approve_button->click();
        QVERIFY(gui_test::wait_for([&] { return box->items().size() == 1; }, 60000));
        box->list->setCurrentRow(0);
        QCOMPARE(box->back_button->text(), QStringLiteral("返事を送る"));
        box->reason->setFocus();
        type_in(box->reason, QStringLiteral("紺でいいです"));
        QTest::keyClick(box->reason, Qt::Key_Return);
        type_in(box->reason, QStringLiteral("持ち手は茶色に"));
        QCOMPARE(box->reason->toPlainText(), QStringLiteral("紺でいいです\n持ち手は茶色に"));
        desk.yes = {true};
        QTest::keyClick(box->reason, Qt::Key_Return, Qt::ControlModifier);
        QVERIFY(desk.questions.last().contains(QStringLiteral("AI（claude-desktop/9204） に、次の言葉を送ります。")));
        QVERIFY(gui_test::wait_for([&] { return box->items().empty(); }, 60000));
        QVERIFY(desk.session->wait_saved(std::chrono::seconds(30)));
        // 8. the result reaches the AI — after the connection is closed and made again
        ai.close();
        Client again;
        QVERIFY(again.start(command));
        QVERIFY(again.request("initialize", initialize_params()).contains("result"));
        const Json tickets = again.call("tickets", Json{{"project", "A.genko"}, {"status", "all"}, {"session", "9204"}});
        QVERIFY2(tickets.value("ok", false), tickets.dump().c_str());
        bool fix = false, approved = false, answered = false;
        for (const Json& t : tickets["tickets"]) {
            if (t.value("kind", Json()) == Json("fix") && t.value("text", Json()) == Json("紺でいいです\n持ち手は茶色に")) fix = true;
            if (t.value("kind", Json()) == Json("gate") && t.value("status", Json()) == Json("done")) approved = true;
            if (t.value("kind", Json()) == Json("help") && t.value("status", Json()) == Json("done")) answered = true;
        }
        QVERIFY2(fix && approved && answered, tickets.dump().c_str());
        const core::Document saved_a = gui_test::read_book(a);
        QVERIFY(saved_a.page(0).name_ok);
        // B's request untouched by what was decided in A
        const core::Document saved_b = gui_test::read_book(b);
        const auto waiting_b = app::review_model::inbox(saved_b);
        QCOMPARE(waiting_b.size(), std::size_t(1));
        QCOMPARE(waiting_b.front().by, std::string("ai:claude-desktop/7777"));
        QVERIFY(!saved_b.page(1).name_ok);
        again.close();
        // the dialog after the reconnection: the same book and conversation; nothing waits now
        desk.with_dialog([&](app::AiDialog* d) {
            QCOMPARE(d->book_pick->currentText(), QStringLiteral("A.genko"));
            QCOMPARE(d->chosen_session(), QStringLiteral("9204"));
            QVERIFY(d->state_rows->text().contains(QStringLiteral("<b>確認待ち</b>: なし")));
        });
        if (genko_mcp) {
            // the tool log: a line for each call on the book, as toollog.record writes it (json.dumps of {at, actor,
            // tool, args, ms, ok}: the time rounded to the millisecond, the call's length to a tenth of one, the
            // arguments summed up — numbers and short strings)
            const QList<Json> logged_a = log_lines(a);
            QStringList tools_a;
            for (const Json& entry : logged_a) tools_a << QString::fromStdString(entry["tool"].get<std::string>());
            QCOMPARE(tools_a, (QStringList{QStringLiteral("request_approval"), QStringLiteral("ask_human"), QStringLiteral("tickets")}));
            QCOMPARE(logged_a.value(1)["args"], (Json{{"project", "A.genko"}, {"text", "鞄の色は紺でいいですか"}, {"page", 1}}));
            QCOMPARE(logged_a.value(0)["args"], (Json{{"project", "A.genko"}, {"gate", "name"}, {"note", "1 ページのネームを見てください"}}));
            const QList<Json> logged_b = log_lines(b);
            QCOMPARE(logged_b.size(), 1);
            QCOMPARE(logged_b.value(0)["actor"], Json("ai:claude-desktop/7777"));
            // presence: each conversation that wrote the book, at the time of its last write (presence.touch)
            QCOMPARE(presence_of(a), QStringList{QStringLiteral("ai:claude-desktop/9204")});
            QCOMPARE(presence_of(b), QStringList{QStringLiteral("ai:claude-desktop/7777")});
        }
        QDir(gui_test::qpath(config.parent_path())).removeRecursively();
    }

    // initialize as a client sends it: the version of the protocol, its capabilities and its name (the server checks all).
    static Json initialize_params() {
        return Json{{"protocolVersion", "2025-06-18"}, {"capabilities", Json::object()}, {"clientInfo", Json{{"name", "test-client"}, {"version", "1.0"}}}};
    }

    // The lines of a book's tool log, each checked to be written as Python's json.dumps(entry, ensure_ascii=False) of
    // toollog.record's entry.
    static QList<Json> log_lines(const fs::path& book) {
        QList<Json> out;
        const std::string bytes = genko::test::read_bytes(gui_test::qpath(book / "studio" / "logs" / "tools.jsonl"));
        const QStringList lines = QString::fromStdString(bytes).split(QLatin1Char('\n'));
        [&] { QCOMPARE(lines.last(), QString()); }();  // (each line ends with "\n")
        const double now = static_cast<double>(QDateTime::currentMSecsSinceEpoch()) / 1000.0;
        for (const QString& line : lines.mid(0, lines.size() - 1)) {
            const Json entry = core::parse_python_json(line.toStdString());
            [&] {
                QCOMPARE(QString::fromStdString(core::dump_python(entry)), line);
                QStringList keys;
                for (const auto& [key, value] : entry.items()) keys << QString::fromStdString(key);
                QCOMPARE(keys.mid(0, 6), (QStringList{QStringLiteral("at"), QStringLiteral("actor"), QStringLiteral("tool"), QStringLiteral("args"),
                                                      QStringLiteral("ms"), QStringLiteral("ok")}));
                for (const QString& key : keys.mid(6)) QVERIFY2(key == QStringLiteral("codes") || key == QStringLiteral("error"), qPrintable(line));
                QVERIFY2(entry["at"].is_number_float() && std::abs(entry["at"].get<double>() - now) < 600, qPrintable(line));
                QCOMPARE(entry["at"].get<double>(), std::round(entry["at"].get<double>() * 1000) / 1000);
                QVERIFY2(entry["ms"].is_number_float() && entry["ms"].get<double>() >= 0, qPrintable(line));
                QCOMPARE(entry["ms"].get<double>(), std::round(entry["ms"].get<double>() * 10) / 10);
                QCOMPARE(entry["ok"], Json(true));
                QVERIFY(entry["args"].is_object());
            }();
            out << entry;
        }
        return out;
    }

    // The conversations in a book's presence.json, checked to be written as Python's presence._write writes it
    // (json.dumps, {"actors": {name: {"at": the time}}, …}).
    static QStringList presence_of(const fs::path& book) {
        const std::string bytes = genko::test::read_bytes(gui_test::qpath(book / "studio" / "presence.json"));
        const Json data = core::parse_python_json(bytes);
        QStringList out;
        [&] {
            QCOMPARE(QString::fromStdString(core::dump_python(data)), QString::fromStdString(bytes));
            QVERIFY(data.contains("actors"));
            const double now = static_cast<double>(QDateTime::currentMSecsSinceEpoch()) / 1000.0;
            for (const auto& [name, item] : data["actors"].items()) {
                QCOMPARE(item.size(), std::size_t(1));
                QVERIFY2(item["at"].is_number_float() && std::abs(item["at"].get<double>() - now) < 600, bytes.c_str());
                out << QString::fromStdString(name);
            }
        }();
        return out;
    }
};

QTEST_MAIN(TestGuiAi)
#include "test_gui_ai.moc"
