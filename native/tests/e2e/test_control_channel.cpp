// 制御チャンネル (PC5 J-g, SPEC.md PROD-01「制御チャンネル」, automation:control-channel; the user's decision 6): the running
// app's local socket, driven from outside as a program would — in this process (the window's state seen) and from
// `genko control` in another, and `genko app` started with the channel on.
//
//   off by default (no socket); a line before `auth` (or a wrong token) is never answered; an ai: token's ops go into the
//   journal as ai:…; the page locks, strict_gates and the person's own ops refuse it as MCP's apply_ops does — a human:
//   token too; no method reaches an approval, a consent, a setting, a token, a file (the whole table gone round, and the
//   upstream methods that would); ui.menu.invoke takes the view's commands only; a line past 1 MiB and a fifth connection
//   are refused; the pen draws as the token's actor, is refused on a locked page, holds the person's input while it draws
//   (the status bar says who), gives it back when it ends or the connection goes, and never reaches a dialog or a menu.

#include <QtTest>

#include <QDialog>
#include <QImage>
#include <QLabel>
#include <QLocalSocket>
#include <QMenuBar>
#include <QProcess>
#include <QSettings>
#include <QTemporaryDir>

#include <memory>

#include "app/ask.hpp"
#include "app/canvas.hpp"
#include "app/control_channel.hpp"
#include "app/inject.hpp"
#include "app/main_window.hpp"
#include "core/prefs_table.hpp"
#include "../gui/gui_support.hpp"
#include "service/control.hpp"
#include "service/tokens.hpp"
#include "storage/journal.hpp"

using namespace genko;
using core::Json;

namespace {

// A runtime folder of this test's own (the socket's place, 0700)
const QTemporaryDir& runtime_folder() {
    static QTemporaryDir dir;
    static const bool set = [] {
        QFile::setPermissions(dir.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        qputenv("XDG_RUNTIME_DIR", dir.path().toUtf8());
        return true;
    }();
    (void)set;
    return dir;
}

void channel(bool on) {
    const auto settings = core::prefs::store();
    QVERIFY(core::prefs::set(*settings, Json{{"integrations.controlChannel", on}}).ok);
    settings->sync();
    app::control::Server::instance().follow_settings();
}

// A program on the channel: one JSON line out, one in.
struct Client {
    QLocalSocket socket;
    QByteArray buffer;
    int next = 1;

    bool open() {
        socket.connectToServer(service::control::server_name());
        return gui_test::wait_for([this] { return socket.state() == QLocalSocket::ConnectedState; }, 3000);
    }
    void send_raw(const QByteArray& bytes) {
        socket.write(bytes);
        socket.flush();
    }
    std::optional<Json> line(int ms = 5000) {
        Json out;
        const bool got = gui_test::wait_for([&] {
            buffer += socket.readAll();
            const qsizetype end = buffer.indexOf('\n');
            if (end < 0) return false;
            out = Json::parse(buffer.left(end).toStdString());
            buffer.remove(0, end + 1);
            return true;
        }, ms);
        if (!got) return std::nullopt;
        return out;
    }
    bool closed(int ms = 3000) {
        return gui_test::wait_for([this] {
            buffer += socket.readAll();
            return socket.state() == QLocalSocket::UnconnectedState;
        }, ms);
    }
    std::optional<Json> auth(const std::string& token) {
        send_raw(QByteArray::fromStdString(Json{{"id", "a"}, {"method", "auth"}, {"params", {{"token", token}}}}.dump()) + '\n');
        return line();
    }
    Json call(const std::string& method, const Json& params = Json::object()) {
        const int id = next++;
        send_raw(QByteArray::fromStdString(Json{{"id", id}, {"method", method}, {"params", params}}.dump()) + '\n');
        const auto got = line(20000);
        if (!got) return Json{{"ok", false}, {"error", {{"code", "no_answer"}}}};
        return *got;
    }
};

std::string code_of(const Json& answer) { return answer.value("error", Json::object()).value("code", std::string()); }
std::string message_of(const Json& answer) { return answer.value("error", Json::object()).value("message", std::string()); }

struct Book {
    QTemporaryDir tmp;
    std::filesystem::path path;
    std::unique_ptr<app::MainWindow> window;
    gui_test::Answers answers;

    explicit Book(core::Document doc = gui_test::new_doc(3)) {
        (void)gui_test::config_folder();
        (void)runtime_folder();
        path = gui_test::path_of(tmp.path() + QStringLiteral("/b.genko"));
        gui_test::write_book(path, doc);
        window = std::make_unique<app::MainWindow>(app::Session::open(path, gui_test::quick(gui_test::path_of(tmp.path() + QStringLiteral("/recovery")))));
        window->resize(1280, 800);
        window->show();
        window->activateWindow();
        for (int i = 0; i < 5; ++i) QCoreApplication::processEvents();
    }
    ~Book() { app::control::Server::instance().stop(); }
    const core::Document& doc() const { return window->book(); }
    std::string last_actor() {
        window->session().save_now();
        (void)gui_test::wait_for([this] { return !window->session().unsaved(); }, 10000);
        const auto stacks = storage::journal::stacks(path);
        return stacks.undo.empty() ? std::string() : core::py_str(stacks.undo.back().actor);
    }
    std::string ink_id() const { return gui_test::ink_of(doc().page(0))->id; }
};

Json stroke(const std::string& layer, double x = 20) {
    return Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", layer}, {"points", Json::array({Json::array({x, 20, 0.7}), Json::array({x + 30, 40, 0.7})})}};
}

}  // namespace

class TestControlChannel : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        (void)gui_test::config_folder();
        (void)runtime_folder();
    }

    void offByDefault() {
        Book book;
        QCOMPARE(core::prefs::value("integrations.controlChannel"), Json(false));
        app::control::Server::instance().follow_settings();
        QVERIFY(!app::control::Server::instance().listening());
        QLocalSocket socket;
        socket.connectToServer(service::control::server_name());
        QVERIFY(!socket.waitForConnected(500));
        QVERIFY(!book.window->findChild<QLabel*>(QStringLiteral("controlChannelState")) ||
                !book.window->findChild<QLabel*>(QStringLiteral("controlChannelState"))->isVisible());
        // 環境設定 → 連携 turns it on; off again, the connections closed
        channel(true);
        QVERIFY(app::control::Server::instance().listening());
        auto* label = book.window->findChild<QLabel*>(QStringLiteral("controlChannelState"));
        QVERIFY(label != nullptr && label->text().contains(QStringLiteral("制御チャンネル")));
        Client c;
        QVERIFY(c.open());
        QVERIFY(c.auth(service::tokens::add("ai:bot")));
        channel(false);
        QVERIFY(!app::control::Server::instance().listening());
        QVERIFY(c.closed());
    }

    void nothingAnsweredBeforeAuth() {
        Book book;
        channel(true);
        const std::string token = service::tokens::add("ai:bot");
        for (const QByteArray& first : {QByteArray(R"({"id":1,"method":"ui.inspect","params":{}})"), QByteArray(R"({"id":1,"method":"auth","params":{"token":"nope"}})"),
                                        QByteArray("not json"), QByteArray(R"({"id":1,"method":"engine.execute","params":{"ops":[]}})"),
                                        QByteArray(R"({"method":"auth","params":{"token":")") + QByteArray::fromStdString(token.substr(0, 10)) + "\"}}"}) {
            Client c;
            QVERIFY(c.open());
            c.send_raw(first + '\n');
            QVERIFY2(c.closed(), first.constData());
            QVERIFY2(c.buffer.isEmpty(), first.constData());  // (not a byte answered)
        }
        Client good;
        QVERIFY(good.open());
        const auto authed = good.auth(token);
        QVERIFY(authed && (*authed)["ok"] == true && (*authed)["result"]["actor"] == "ai:bot");
        // a token revoked works no more for a new connection
        QCOMPARE(service::tokens::revoke(token.substr(0, 8)), 1);
        Client late;
        QVERIFY(late.open());
        late.send_raw(QByteArray::fromStdString(Json{{"method", "auth"}, {"params", {{"token", token}}}}.dump()) + '\n');
        QVERIFY(late.closed() && late.buffer.isEmpty());
    }

    void aiTokensOpsGoIntoTheJournalAsAi() {
        Book book;
        channel(true);
        Client c;
        QVERIFY(c.open() && c.auth(service::tokens::add("ai:bot")));
        const std::size_t before = gui_test::ink_strokes(book.doc());
        const Json out = c.call("engine.execute", Json{{"ops", Json::array({stroke(book.ink_id())})}});
        QVERIFY2(out["ok"] == true, out.dump().c_str());
        QCOMPARE(gui_test::ink_strokes(book.doc()), before + 1);  // (shown at once in the window)
        QCOMPARE(book.last_actor(), std::string("ai:bot"));
        // a human: token: its name in the journal (the rights an agent's)
        Client h;
        QVERIFY(h.open() && h.auth(service::tokens::add("human:owner")));
        QVERIFY(h.call("engine.execute", Json{{"ops", Json::array({stroke(book.ink_id(), 60)})}})["ok"] == true);
        QCOMPARE(book.last_actor(), std::string("human:owner"));
        // the person's Undo takes it back as any change (one step)
        book.window->session().undo();
        QCOMPARE(gui_test::ink_strokes(book.doc()), before + 1);
    }

    void theSameRefusalsAsMcp() {
        core::Document doc = gui_test::new_doc(3);
        doc.strict_gates = true;
        doc.edit_page(1).name_ok = true;
        Book book(doc);
        channel(true);
        Client ai;
        QVERIFY(ai.open() && ai.auth(service::tokens::add("ai:bot")));
        Client human;
        QVERIFY(human.open() && human.auth(service::tokens::add("human:owner")));
        const std::int64_t revision = book.doc().revision;
        for (Client* c : {&ai, &human}) {
            // ops apply_ops does not take: the approvals, the locks, undo, the book's rules (strict_gates) — any token
            for (const Json& op : {Json{{"op", "approve"}, {"gate", "name"}, {"page", 1}}, Json{{"op", "revoke"}, {"gate", "name"}, {"page", 2}},
                                   Json{{"op", "lock_page"}, {"page", 1}}, Json{{"op", "unlock_page"}, {"page", 1}}, Json{{"op", "name_ok"}, {"page", 1}},
                                   Json{{"op", "advance"}, {"page", 1}}, Json{{"op", "allow_chat_approval"}, {"on", true}}, Json{{"op", "undo"}},
                                   Json{{"op", "set_meta"}, {"strict_gates", false}}, Json{{"op", "import_psd"}, {"path", "/etc/passwd"}},
                                   Json{{"op", "for_pages"}, {"pages", "all"}, {"ops", Json::array({Json{{"op", "approve"}, {"gate", "name"}}})}}}) {
                const Json out = c->call("engine.execute", Json{{"ops", Json::array({op})}});
                QVERIFY2(out["ok"] == false && code_of(out) == "op_not_allowed", out.dump().c_str());
            }
            // the person's own field (only a person changes the policy): refused to a human: token too
            const Json policy = c->call("engine.execute", Json{{"ops", Json::array({Json{{"op", "set_studio"}, {"policy", {{"pilot", false}}}}})}});
            QVERIFY2(policy["ok"] == false && !message_of(policy).empty(), policy.dump().c_str());
            // strict_gates: page 2's name approved — its panels stay
            const std::string frame = book.doc().page(1).leaf_frames()[0]->id;
            const Json split = c->call("engine.execute", Json{{"ops", Json::array({Json{{"op", "split_frame"}, {"page", 2}, {"frame_id", frame}, {"axis", "h"}, {"at", 0.5}}})}});
            QVERIFY2(split["ok"] == false, split.dump().c_str());
        }
        QCOMPARE(book.doc().revision, revision);
        QVERIFY(book.doc().page(1).name_ok);
        // a page the person locked: refused, as MCP's
        QVERIFY(book.window->apply_ops(Json::array({Json{{"op", "lock_page"}, {"page", 1}}})));
        const Json locked = ai.call("engine.execute", Json{{"ops", Json::array({stroke(book.ink_id())})}});
        QVERIFY2(locked["ok"] == false && message_of(locked).find("human:tester") != std::string::npos, locked.dump().c_str());
        const Json locked_human = human.call("engine.execute", Json{{"ops", Json::array({stroke(book.ink_id())})}});
        QVERIFY(locked_human["ok"] == false);
        // more than 500 ops, ops not a list
        Json many = Json::array();
        for (int i = 0; i < 501; ++i) many.push_back(stroke(book.ink_id()));
        QCOMPARE(code_of(ai.call("engine.execute", Json{{"ops", many}})), std::string("ops_invalid"));
        QCOMPARE(code_of(ai.call("engine.execute", Json{{"ops", "add_stroke"}})), std::string("ops_invalid"));
    }

    void noMethodReachesApprovalsConsentOrSettings() {
        Book book;
        channel(true);
        Client c;
        QVERIFY(c.open() && c.auth(service::tokens::add("human:owner")));
        // the whole table: exactly these, none an approval, a consent, a setting, a token, a key, a dialog or a file
        const Json table = c.call("methods");
        QVERIFY(table["ok"] == true);
        QStringList names;
        for (const Json& m : table["result"]) names << QString::fromStdString(m["name"].get<std::string>());
        QCOMPARE(names, (QStringList{"auth", "methods", "ui.inspect", "ui.screenshot", "ui.set", "ui.menu.invoke", "ui.pointer", "engine.commands", "engine.execute"}));
        for (const Json& m : table["result"]) {
            const bool changes = m["changes_book"].get<bool>();
            QCOMPARE(changes, m["name"] == "ui.pointer" || m["name"] == "engine.execute");
        }
        const std::int64_t revision = book.doc().revision;
        // each method of the table called (ui.pointer, engine.execute with nothing): nothing approved, nothing set
        for (const QString& name : names) {
            if (name == QLatin1String("auth")) continue;
            Json params = Json::object();
            if (name == QLatin1String("engine.execute")) params = Json{{"ops", Json::array()}};
            if (name == QLatin1String("ui.pointer")) params = Json{{"events", Json::array()}};
            if (name == QLatin1String("ui.menu.invoke")) params = Json{{"id", "act_zoom_in"}};
            const Json out = c.call(name.toStdString(), params);
            QVERIFY2(out.contains("ok"), out.dump().c_str());
        }
        QCOMPARE(book.doc().revision, revision);
        // upstream's and the person's ways in: no such method
        for (const char* method : {"auth", "human.approve", "approve", "revoke", "app.open", "app.save", "app.quit", "prefs.set", "prefs.get",
                                   "plugins.consent", "plugin.install", "tokens.add", "token.add", "ai.connect", "ui.key", "ui.type", "ui.dialog.open",
                                   "ui.dialog.confirm", "ui.window.open", "ui.window.close", "ui.focus", "ui.resize", "file.open", "edit.preferences"}) {
            QCOMPARE(code_of(c.call(method)), std::string("unknown_method"));
        }
        // the menus: the view's commands only
        for (const char* name : {"act_prefs", "act_plugin_settings", "act_plugins", "act_timeline", "act_quit", "act_save", "act_open", "act_undo",
                                 "act_keyboard_shortcuts", "act_go_to_frame", "act_new_guide", "act_clear_guides", "act_lock_guides", "act_snap_on",
                                 "act_screen_full", "act_32bit_preview", "act_render_video", "act_layer_merge"}) {
            QCOMPARE(code_of(c.call("ui.menu.invoke", Json{{"id", name}})), std::string("not_allowed"));
        }
        // the panels: never the approvals'
        for (const char* panel : {"承認箱", "コマの詳細", "資料", "クイックアクセス"}) {
            QCOMPARE(code_of(c.call("ui.set", Json{{"panels", {{panel, true}}}})), std::string("bad_params"));
        }
        // ui.inspect says which commands ui.menu.invoke takes, and never a file's path
        const Json seen = c.call("ui.inspect");
        QVERIFY(seen["ok"] == true);
        QVERIFY(seen["result"].dump().find(gui_test::qpath(book.path).toStdString()) == std::string::npos);
        int invocable = 0;
        std::function<void(const Json&)> walk = [&](const Json& m) {
            for (const Json& item : m["items"]) {
                if (item.contains("items")) walk(item);
                if (item.value("invoke", false)) {
                    ++invocable;
                    QVERIFY(service::control::view_command(item["name"].get<std::string>()));
                }
            }
        };
        for (const Json& m : seen["result"]["menus"]) walk(m);
        QVERIFY(invocable > 10);
        QCOMPARE(book.doc().revision, revision);
        QVERIFY(book.answers.asked.isEmpty());  // (not a question asked of the person)
    }

    void theViewOnly() {
        Book book;
        channel(true);
        Client c;
        QVERIFY(c.open() && c.auth(service::tokens::add("ai:bot")));
        const std::int64_t revision = book.doc().revision;
        Json set = c.call("ui.set", Json{{"page", 2}, {"zoom", 50}, {"tool", "pencil"}, {"panels", {{"アクション", true}}}});
        QVERIFY2(set["ok"] == true, set.dump().c_str());
        QCOMPARE(book.window->current_page()->index.json(), Json(2));
        QCOMPARE(book.window->canvas()->tool(), QStringLiteral("pencil"));
        QVERIFY(book.window->actions_dock()->isVisible());
        QVERIFY(std::abs(book.window->canvas()->zoom_percent_exact() - 50) < 0.5);
        // a value it does not take: nothing changed
        QCOMPARE(code_of(c.call("ui.set", Json{{"page", 1}, {"zoom", -3}})), std::string("bad_params"));
        QCOMPARE(book.window->current_page()->index.json(), Json(2));
        QCOMPARE(code_of(c.call("ui.set", Json{{"title", "x"}})), std::string("bad_params"));
        QCOMPARE(code_of(c.call("ui.set", Json{{"tool", "no_such_tool"}})), std::string("bad_params"));
        // a view's command
        const double zoom = book.window->canvas()->zoom_percent_exact();
        QVERIFY(c.call("ui.menu.invoke", Json{{"id", "act_zoom_in"}})["ok"] == true);
        QVERIFY(book.window->canvas()->zoom_percent_exact() > zoom);
        const bool guides = book.window->action(QStringLiteral("act_show_guides"))->isChecked();
        QVERIFY(c.call("ui.menu.invoke", Json{{"id", "act_show_guides"}})["ok"] == true);
        QCOMPARE(book.window->action(QStringLiteral("act_show_guides"))->isChecked(), !guides);
        QCOMPARE(book.doc().revision, revision);
        // the pictures: the canvas for ai:, the whole window for human: alone
        const Json shot = c.call("ui.screenshot", Json{{"max_side", 400}});
        QVERIFY(shot["ok"] == true && shot["result"]["part"] == "canvas");
        const QImage picture = QImage::fromData(QByteArray::fromBase64(QByteArray::fromStdString(shot["result"]["base64"].get<std::string>())), "PNG");
        QVERIFY(!picture.isNull() && std::max(picture.width(), picture.height()) <= 400);
        QCOMPARE(code_of(c.call("ui.screenshot", Json{{"window", true}})), std::string("not_allowed"));
        Client h;
        QVERIFY(h.open() && h.auth(service::tokens::add("human:owner")));
        QVERIFY(h.call("ui.screenshot", Json{{"window", true}, {"max_side", 300}})["result"]["part"] == "window");
        // the ops engine.execute takes: MCP's, none reading a file
        const Json commands = c.call("engine.commands");
        bool stroke_there = false;
        for (const Json& op : commands["result"]["ops"]) {
            QVERIFY(op["op"] != "approve" && op["op"] != "import_psd" && op["op"] != "set_meta");
            stroke_there = stroke_there || op["op"] == "add_stroke";
        }
        QVERIFY(stroke_there);
    }

    void linesAndConnectionsHaveLimits() {
        Book book;
        channel(true);
        const std::string token = service::tokens::add("ai:bot");
        // a line past 1 MiB: refused, the connection closed (before auth: not a word)
        Client before;
        QVERIFY(before.open());
        before.send_raw(QByteArray(int(service::control::kMaxLine) + 10, 'x'));
        QVERIFY(before.closed() && before.buffer.isEmpty());
        Client after;
        QVERIFY(after.open() && after.auth(token));
        after.send_raw(QByteArray("{\"id\":1,\"method\":\"ui.inspect\",\"params\":{\"x\":\"") + QByteArray(int(service::control::kMaxLine), 'y') + "\"}}\n");
        const auto refused = after.line();
        QVERIFY(refused && code_of(*refused) == "line_too_long");
        QVERIFY(after.closed());
        // four at once; the fifth closed before a word
        std::vector<std::unique_ptr<Client>> four;
        for (int i = 0; i < 4; ++i) {
            four.push_back(std::make_unique<Client>());
            QVERIFY(four.back()->open() && four.back()->auth(token));
        }
        Client fifth;
        fifth.socket.connectToServer(service::control::server_name());
        QVERIFY(fifth.closed());
        QVERIFY(fifth.buffer.isEmpty());
        QCOMPARE(app::control::Server::instance().connections(), 4);
        for (auto& c : four) QVERIFY(c->call("methods")["ok"] == true);
        four.pop_back();  // (one gone: room for another)
        QVERIFY(gui_test::wait_for([] { return app::control::Server::instance().connections() == 3; }));
        Client again;
        QVERIFY(again.open() && again.auth(token));
    }

    void thePenDrawsAsTheTokensActor() {
        Book book;
        channel(true);
        Client c;
        QVERIFY(c.open() && c.auth(service::tokens::add("ai:bot")));
        QVERIFY(c.call("ui.set", Json{{"tool", "pen"}})["ok"] == true);
        const std::size_t before = gui_test::ink_strokes(book.doc());
        Json down = c.call("ui.pointer", Json{{"events", Json::array({Json{{"kind", "down"}, {"x", 30}, {"y", 30}, {"pressure", 0.5}},
                                                                      Json{{"kind", "move"}, {"x", 50}, {"y", 40}}})}});
        QVERIFY2(down["ok"] == true && down["result"]["drawing"] == true, down.dump().c_str());
        // while it draws: the person's input held, the status bar says who draws
        auto* label = book.window->findChild<QLabel*>(QStringLiteral("controlChannelState"));
        QVERIFY(label != nullptr && label->isVisible() && label->text().contains(QStringLiteral("制御チャンネルが描いています（ai:bot）")));
        QVERIFY(c.call("ui.inspect")["result"]["channel"]["input_held"] == true);
        app::inject::mouse_stroke(book.window->canvas(), {QPointF(100, 100), QPointF(120, 130)});  // (the person's line: not taken)
        QTest::keyClick(book.window.get(), Qt::Key_Z, Qt::ControlModifier);  // (nor a key)
        QCOMPARE(gui_test::ink_strokes(book.doc()), before);
        // another connection's pen and ops wait
        Client other;
        QVERIFY(other.open() && other.auth(service::tokens::add("ai:other")));
        QCOMPARE(code_of(other.call("ui.pointer", Json{{"events", Json::array({Json{{"kind", "down"}, {"x", 1}, {"y", 1}}})}})), std::string("busy"));
        QCOMPARE(code_of(other.call("engine.execute", Json{{"ops", Json::array({stroke(book.ink_id())})}})), std::string("busy"));
        const Json up = c.call("ui.pointer", Json{{"events", Json::array({Json{{"kind", "move"}, {"x", 70}, {"y", 45}}, Json{{"kind", "up"}}})}});
        QVERIFY2(up["ok"] == true && up["result"]["applied"] == true && up["result"]["drawing"] == false, up.dump().c_str());
        QCOMPARE(gui_test::ink_strokes(book.doc()), before + 1);
        QCOMPARE(book.last_actor(), std::string("ai:bot"));
        // given back: the status bar, the person's line taken again
        QVERIFY(!label->text().contains(QStringLiteral("描いています")));
        QVERIFY(c.call("ui.inspect")["result"]["channel"]["input_held"] == false);
        app::inject::mouse_stroke(book.window->canvas(), {QPointF(100, 100), QPointF(120, 130)});
        QCOMPARE(gui_test::ink_strokes(book.doc()), before + 2);
        QCOMPARE(book.last_actor(), std::string("human:tester"));
        // never a recording of the person's
        QVERIFY(book.answers.asked.isEmpty());
    }

    void thePenIsRefusedOnALockedPageAndByOtherTools() {
        Book book;
        channel(true);
        Client c;
        QVERIFY(c.open() && c.auth(service::tokens::add("ai:bot")));
        QVERIFY(book.window->apply_ops(Json::array({Json{{"op", "lock_page"}, {"page", 1}}})));
        QVERIFY(c.call("ui.set", Json{{"tool", "pen"}, {"page", 1}})["ok"] == true);
        const std::size_t before = gui_test::ink_strokes(book.doc());
        const Json out = c.call("ui.pointer", Json{{"events", Json::array({Json{{"kind", "down"}, {"x", 30}, {"y", 30}}, Json{{"kind", "move"}, {"x", 60}, {"y", 30}},
                                                                           Json{{"kind", "up"}}})}});
        QVERIFY2(out["ok"] == false && code_of(out) == "refused" && message_of(out).find("human:tester") != std::string::npos, out.dump().c_str());
        QCOMPARE(gui_test::ink_strokes(book.doc()), before);
        QVERIFY(c.call("ui.inspect")["result"]["channel"]["input_held"] == false);
        // a tool that is not a pen (a text, a note, a crop would open the person's editors): refused, nothing opened
        for (const char* tool : {"text", "note", "crop", "select", "marquee", "fill"}) {
            if (book.window->tool_actions().find(QString::fromLatin1(tool)) == book.window->tool_actions().end()) continue;
            QVERIFY(c.call("ui.set", Json{{"tool", tool}})["ok"] == true);
            QCOMPARE(code_of(c.call("ui.pointer", Json{{"events", Json::array({Json{{"kind", "down"}, {"x", 30}, {"y", 30}}})}})), std::string("not_allowed"));
        }
        QVERIFY(book.answers.asked.isEmpty());
        QVERIFY(QApplication::activeModalWidget() == nullptr && QApplication::activePopupWidget() == nullptr);
        // a dialog the pen's line would bring is never shown: answered as cancelled
        {
            const int refused = app::ask::refused();
            const app::ask::Closed closed;
            QVERIFY(!app::ask::question(book.window.get(), QStringLiteral("t"), QStringLiteral("q")));
            QCOMPARE(app::ask::refused(), refused + 1);
        }
        QVERIFY(book.answers.asked.isEmpty());
    }

    void thePenIsGivenBackWhenTheConnectionGoes() {
        Book book;
        channel(true);
        const std::size_t before = gui_test::ink_strokes(book.doc());
        {
            Client c;
            QVERIFY(c.open() && c.auth(service::tokens::add("ai:bot")));
            QVERIFY(c.call("ui.set", Json{{"tool", "pencil"}})["ok"] == true);
            QVERIFY(c.call("ui.pointer", Json{{"events", Json::array({Json{{"kind", "down"}, {"x", 30}, {"y", 30}}, Json{{"kind", "move"}, {"x", 60}, {"y", 30}}})}})["ok"] == true);
            QVERIFY(!app::control::Server::instance().drawing().isEmpty());
        }  // (the program gone)
        QVERIFY(gui_test::wait_for([] { return app::control::Server::instance().drawing().isEmpty(); }));
        QVERIFY(!book.window->canvas()->control_drawing());
        QCOMPARE(gui_test::ink_strokes(book.doc()), before);  // (the line thrown away)
        app::inject::mouse_stroke(book.window->canvas(), {QPointF(100, 100), QPointF(120, 130)});
        QCOMPARE(gui_test::ink_strokes(book.doc()), before + 1);  // (the person's input back)
        // Esc: the person stops it
        Client c;
        QVERIFY(c.open() && c.auth(service::tokens::add("ai:bot")));
        QVERIFY(c.call("ui.pointer", Json{{"events", Json::array({Json{{"kind", "down"}, {"x", 30}, {"y", 30}}})}})["ok"] == true);
        QTest::keyClick(book.window.get(), Qt::Key_Escape);
        QVERIFY(gui_test::wait_for([] { return app::control::Server::instance().drawing().isEmpty(); }));
        QCOMPARE(code_of(c.call("ui.pointer", Json{{"events", Json::array({Json{{"kind", "up"}}})}})), std::string("bad_params"));
        QCOMPARE(gui_test::ink_strokes(book.doc()), before + 1);
    }

    void thePenGivesBackAfterThirtySecondsWithoutAWord() {
        Book book;
        channel(true);
        Client c;
        QVERIFY(c.open() && c.auth(service::tokens::add("ai:bot")));
        QVERIFY(c.call("ui.pointer", Json{{"events", Json::array({Json{{"kind", "down"}, {"x", 30}, {"y", 30}}})}})["ok"] == true);
        QVERIFY(!app::control::Server::instance().drawing().isEmpty());
        QVERIFY(gui_test::wait_for([] { return app::control::Server::instance().drawing().isEmpty(); }, service::control::kTimeoutMs + 5000));
        QVERIFY(!book.window->canvas()->control_drawing());
    }

    void genkoControlFromAnotherProcess() {
        Book book;
        channel(true);
        const std::string token = service::tokens::add("ai:cli");
        QProcess cli;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("GENKO_TOKEN"), QString::fromStdString(token));
        cli.setProcessEnvironment(env);
        const auto run = [&](const QStringList& args) {
            cli.start(QStringLiteral(GENKO_CLI), QStringList{QStringLiteral("control")} + args);
            if (!gui_test::wait_for([&] { return cli.state() == QProcess::NotRunning; }, 30000)) return Json();
            return Json::parse(cli.readAllStandardOutput().toStdString());
        };
        const Json listed = run({QStringLiteral("methods")});
        QVERIFY2(listed["ok"] == true && listed["result"].size() == 9, listed.dump().c_str());
        const std::size_t before = gui_test::ink_strokes(book.doc());
        const Json done = run({QStringLiteral("engine.execute"), QString::fromStdString(Json{{"ops", Json::array({stroke(book.ink_id())})}}.dump())});
        QVERIFY2(done["ok"] == true && cli.exitCode() == 0, done.dump().c_str());
        QCOMPARE(gui_test::ink_strokes(book.doc()), before + 1);
        QCOMPARE(book.last_actor(), std::string("ai:cli"));
        const Json refused = run({QStringLiteral("engine.execute"), QStringLiteral(R"({"ops":[{"op":"approve","gate":"name","page":1}]})")});
        QVERIFY(refused["ok"] == false && cli.exitCode() == 1);
        // a wrong token: not answered
        env.insert(QStringLiteral("GENKO_TOKEN"), QStringLiteral("wrong"));
        cli.setProcessEnvironment(env);
        QCOMPARE(run({QStringLiteral("methods")}).value("code", std::string()), std::string("not_authenticated"));
        // off: not listening
        channel(false);
        env.insert(QStringLiteral("GENKO_TOKEN"), QString::fromStdString(token));
        cli.setProcessEnvironment(env);
        QCOMPARE(run({QStringLiteral("methods")}).value("code", std::string()), std::string("not_listening"));
    }

    void theAppStartsWithTheChannelOn() {
        // `genko app <book>` in a process of its own, 連携 on in its settings: `genko control` draws in it as ai:…
        QTemporaryDir tmp;
        const auto path = gui_test::path_of(tmp.path() + QStringLiteral("/b.genko"));
        gui_test::make_book(path);
        QDir().mkpath(tmp.path() + QStringLiteral("/config"));
        QDir().mkpath(tmp.path() + QStringLiteral("/run"));
        QFile::setPermissions(tmp.path() + QStringLiteral("/run"), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        {
            QSettings ini(tmp.path() + QStringLiteral("/config/settings.ini"), QSettings::IniFormat);
            ini.setValue(QStringLiteral("control/enabled"), QStringLiteral("true"));
        }
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("GENKO_CONFIG_DIR"), tmp.path() + QStringLiteral("/config"));
        env.insert(QStringLiteral("XDG_RUNTIME_DIR"), tmp.path() + QStringLiteral("/run"));
        env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
        QProcess token_cli;
        token_cli.setProcessEnvironment(env);
        token_cli.start(QStringLiteral(GENKO_CLI), {QStringLiteral("token"), QStringLiteral("add"), QStringLiteral("--actor"), QStringLiteral("ai:outside")});
        QVERIFY(token_cli.waitForFinished(30000));
        const Json made = Json::parse(token_cli.readAllStandardOutput().toStdString());
        QVERIFY(made["ok"] == true);
        env.insert(QStringLiteral("GENKO_TOKEN"), QString::fromStdString(made["token"].get<std::string>()));
        QProcess app;
        app.setProcessEnvironment(env);
        app.start(QStringLiteral(GENKO_CLI), {QStringLiteral("app"), gui_test::qpath(path)});
        QVERIFY(app.waitForStarted(30000));
        QProcess cli;
        cli.setProcessEnvironment(env);
        Json seen;
        const bool answered = gui_test::wait_for([&] {
            cli.start(QStringLiteral(GENKO_CLI), {QStringLiteral("control"), QStringLiteral("ui.inspect")});
            cli.waitForFinished(30000);
            seen = Json::parse(cli.readAllStandardOutput().toStdString());
            return seen.value("ok", false) && seen["result"]["page"].is_object();
        }, 60000);
        QVERIFY2(answered, seen.dump().c_str());
        const std::string ink = gui_test::ink_of(gui_test::read_book(path).page(0))->id;
        cli.start(QStringLiteral(GENKO_CLI), {QStringLiteral("control"), QStringLiteral("engine.execute"), QString::fromStdString(Json{{"ops", Json::array({stroke(ink)})}}.dump())});
        QVERIFY(cli.waitForFinished(30000));
        const Json done = Json::parse(cli.readAllStandardOutput().toStdString());
        QVERIFY2(done["ok"] == true, done.dump().c_str());
        QVERIFY(gui_test::wait_for([&] {
            const auto stacks = storage::journal::stacks(path);
            return !stacks.undo.empty() && stacks.undo.back().actor == Json("ai:outside");
        }, 20000));
        app.terminate();
        if (!app.waitForFinished(10000)) app.kill();
    }
};

QTEST_MAIN(TestControlChannel)
#include "test_control_channel.moc"
