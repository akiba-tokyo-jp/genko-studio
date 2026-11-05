// PC5 H-e (SPEC.md VIEW-01「環境設定と API」) as a person's script or an agent uses it: `genko prefs get / set / reset` —
// their form, all of the values or none, a path unknown and a value out of its range refused, keys that are no keys refused,
// the app's memories never set nor reset, the settings of an earlier version left as they were (only what is set changes) —,
// `genko commands` (the screen's commands with this computer's keys: the key set, the person's own set, the person's keys
// laid over them as the app does; a menu, words), `genko session` (no app: not running; a session.json of a process that
// is there read, of one gone not), and MCP's inspect target=prefs, commands and session — reading only (no tool writes a
// preference, the file untouched), a book's folder only as a project of the server's root (the coordinator's decision, 節 H
// 8-8 (a)).

#include <QtTest>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSettings>
#include <QTemporaryDir>

#include <algorithm>
#include <chrono>
#include <string>
#include <tuple>
#include <vector>

#include "core/commands_list.hpp"
#include "core/json.hpp"
#include "testsupport.hpp"

using genko::core::Json;

namespace {

QProcessEnvironment env_for(const QString& config) {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("GENKO_CONFIG_DIR"), config);
    env.insert(QStringLiteral("LANG"), QStringLiteral("C"));
    env.remove(QStringLiteral("LC_ALL"));
    return env;
}

struct Said {
    int code = -1;
    Json json;
    QString described;
};

Said cli(const QString& config, const QStringList& args) {
    const genko::test::Run r = genko::test::run(genko::test::genko_cli(), args, env_for(config), 120000);
    Said out;
    out.code = r.exit_code;
    out.described = r.described();
    out.json = genko::test::one_line(r.out);
    return out;
}

QByteArray bytes_of(const QString& file) {
    QFile f(file);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

const Json* command(const Json& listed, const std::string& id) {
    for (const Json& c : listed["commands"]) {
        if (c["id"] == id) return &c;
    }
    return nullptr;
}

// One `genko mcp` over stdio, as an agent talks to it.
class Mcp {
public:
    Mcp(const QString& root, const QString& config) {
        process_.setProcessEnvironment(env_for(config));
        process_.start(genko::test::genko_cli(), {QStringLiteral("mcp"), QStringLiteral("--root"), root, QStringLiteral("--agent"), QStringLiteral("ai:test")});
        process_.waitForStarted(30000);
        (void)request(0, "initialize", Json{{"protocolVersion", "2025-06-18"}, {"capabilities", Json::object()}, {"clientInfo", Json{{"name", "t"}, {"version", "1"}}}});
        send(Json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
    }
    ~Mcp() {
        process_.closeWriteChannel();
        if (!process_.waitForFinished(60000)) process_.kill();
    }
    void send(const Json& message) {
        process_.write(QByteArray::fromStdString(message.dump() + "\n"));
        process_.waitForBytesWritten(30000);
    }
    Json request(int id, const std::string& method, const Json& params) {
        send(Json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}});
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
        while (std::chrono::steady_clock::now() < deadline) {
            while (process_.canReadLine()) {
                try {
                    Json line = Json::parse(process_.readLine().toStdString());
                    if (line.contains("id") && line["id"] == id) return line;
                } catch (const std::exception&) {
                }
            }
            process_.waitForReadyRead(200);
        }
        return Json();
    }
    // A tool's answer (its first text, as JSON).
    Json call(const std::string& tool, const Json& args) {
        const Json reply = request(++next_, "tools/call", Json{{"name", tool}, {"arguments", args}});
        try {
            return Json::parse(reply["result"]["content"][0]["text"].get<std::string>());
        } catch (const std::exception&) {
            return reply;
        }
    }

private:
    QProcess process_;
    int next_ = 10;
};

}  // namespace

class TestContractPrefs : public QObject {
    Q_OBJECT

private slots:
    // get, set, reset: their form; all or none; refusals with the path; the app's memories kept.
    void prefsGetSetReset() {
        QTemporaryDir dir;
        const QString config = dir.path();
        Said said = cli(config, {"prefs", "get", "interface.theme"});
        QCOMPARE(said.code, 0);
        QCOMPARE(said.json, (Json{{"ok", true}, {"values", Json{{"interface.theme", "system"}}}}));
        said = cli(config, {"prefs", "set", "interface.theme", "\"dark\""});
        QVERIFY2(said.code == 0, qPrintable(said.described));
        QCOMPARE(said.json["values"]["interface.theme"], Json("dark"));
        QSettings ini(config + QStringLiteral("/settings.ini"), QSettings::IniFormat);
        QCOMPARE(ini.value(QStringLiteral("ui/theme")).toString(), QStringLiteral("dark"));
        // one wrong: nothing written, the path said
        said = cli(config, {"prefs", "set", "--values", R"({"fileHandling.recentFileCount": 7, "cursors.painting": "huge"})"});
        QCOMPARE(said.code, 1);
        QCOMPARE(said.json["ok"], Json(false));
        QCOMPARE(said.json["error"], Json("bad_value"));
        QCOMPARE(said.json["path"], Json("cursors.painting"));
        ini.sync();
        QVERIFY(!ini.contains(QStringLiteral("files/recent_count")));
        for (const auto& [path, value, code] : std::vector<std::tuple<const char*, const char*, const char*>>{
                 {"nope.nothing", "1", "unknown_pref"},
                 {"general.notAField", "true", "unknown_pref"},
                 {"performance.historyStates", "1001", "bad_value"},
                 {"general.zoomWithScrollWheel", "\"yes\"", "bad_value"},
                 {"shortcuts.ブラシツール", "\"あいう\"", "bad_value"},
                 {"state.brush.kind", "\"gpen\"", "bad_value"}}) {
            said = cli(config, {"prefs", "set", QString::fromUtf8(path), QString::fromUtf8(value)});
            QVERIFY2(said.code == 1 && said.json["error"] == Json(code), path);
            QCOMPARE(said.json["path"], Json(path));
            QVERIFY(said.json["message"].is_string() && !said.json["message"].get<std::string>().empty());
        }
        said = cli(config, {"prefs", "set", "--values", R"({"historyLog": {"enabled": true, "detail": "detailed"}, "shortcuts.ブラシツール": "Ctrl+Alt+K"})"});
        QVERIFY2(said.code == 0, qPrintable(said.described));
        ini.sync();
        QCOMPARE(ini.value(QStringLiteral("history_log/enabled")).toString(), QStringLiteral("true"));
        QCOMPARE(ini.value(QStringLiteral("shortcuts/ブラシツール")).toString(), QStringLiteral("Ctrl+Alt+K"));
        // get: a section, everything (without the app's memories)
        said = cli(config, {"prefs", "get", "historyLog"});
        QCOMPARE(said.json["values"], (Json{{"historyLog.enabled", true}, {"historyLog.destination", "metadata"}, {"historyLog.filePath", ""},
                                            {"historyLog.detail", "detailed"}}));
        said = cli(config, {"prefs", "get"});
        QVERIFY(said.json["values"].contains("general.colorPicker") && said.json["values"].contains("shortcuts.ブラシツール"));
        for (const auto& [path, value] : said.json["values"].items()) QVERIFY2(path.rfind("state.", 0) != 0, path.c_str());
        // reset: a section; the app's memories never
        said = cli(config, {"prefs", "reset", "historyLog"});
        QCOMPARE(said.code, 0);
        QCOMPARE(said.json["values"]["historyLog.enabled"], Json(false));
        ini.sync();
        QVERIFY(!ini.contains(QStringLiteral("history_log/enabled")));
        QCOMPARE(ini.value(QStringLiteral("ui/theme")).toString(), QStringLiteral("dark"));
        QCOMPARE(cli(config, {"prefs", "reset", "state"}).json["error"], Json("bad_value"));
        QCOMPARE(cli(config, {"prefs", "get", "nope"}).json["error"], Json("unknown_pref"));
        // bad usage: exit 2, nothing on stdout
        for (const QStringList& args : {QStringList{"prefs"}, QStringList{"prefs", "set", "interface.theme"},
                                        QStringList{"prefs", "set", "interface.theme", "dark"}, QStringList{"prefs", "get", "a", "b"},
                                        QStringList{"prefs", "set", "--values", "[1]"}}) {
            QCOMPARE(cli(config, args).code, 2);
        }
    }

    // review P2-3: genko prefs reset (everything, a section) passes over the person's own lists; named, they go back.
    void resetKeepsThePersonsLists() {
        QTemporaryDir dir;
        const QString config = dir.path();
        QCOMPARE(cli(config, {"prefs", "set", "--values", R"({"workspace.quickAccess": "検索", "workspace.commandBar": "取り消し", "interface.theme": "dark"})"}).code, 0);
        QCOMPARE(cli(config, {"prefs", "reset"}).code, 0);
        QCOMPARE(cli(config, {"prefs", "reset", "workspace"}).code, 0);
        Json got = cli(config, {"prefs", "get", "workspace"}).json["values"];
        QCOMPARE(got["workspace.quickAccess"], Json("検索"));
        QCOMPARE(got["workspace.commandBar"], Json("取り消し"));
        QCOMPARE(cli(config, {"prefs", "get", "interface.theme"}).json["values"]["interface.theme"], Json("system"));
        QCOMPARE(cli(config, {"prefs", "reset", "workspace.quickAccess"}).code, 0);
        got = cli(config, {"prefs", "get", "workspace"}).json["values"];
        QCOMPARE(got["workspace.quickAccess"], Json(""));
        QCOMPARE(got["workspace.commandBar"], Json("取り消し"));
    }

    // J-f's review P3-6: the plugins' settings (plugin_settings.json: whether they run, which may — the person's consent —,
    // the additional folder) are the person's, in the plugins' own window: genko prefs and MCP's inspect prefs neither show
    // them nor write them, and a reset leaves the file as it was.
    void pluginsAreThePersonsWindowsOnly() {
        QTemporaryDir dir;
        const QString config = dir.path() + QStringLiteral("/config");
        const QString root = dir.path() + QStringLiteral("/root");
        QVERIFY(QDir().mkpath(config) && QDir().mkpath(root + QStringLiteral("/a.genko")));
        const std::string extra = (dir.path() + QStringLiteral("/my-extra-plugins")).toStdString();
        const Json chosen{{"secret_plugin", Json{{"sha256", "0123abcd"}}}};
        const QString file = config + QStringLiteral("/plugin_settings.json");
        {
            QFile f(file);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(QByteArray::fromStdString(
                Json{{"enabled", true}, {"python", ""}, {"chosen", chosen}, {"use_extra_folder", true}, {"extra_folder", extra}}.dump(1)));
        }
        const QByteArray was = bytes_of(file);
        const auto unsaid = [&extra](const Json& said) {
            const std::string text = said.dump();
            return text.find("my-extra-plugins") == std::string::npos && text.find("secret_plugin") == std::string::npos &&
                   text.find(extra) == std::string::npos;
        };
        // get: everything and the pane's section — nothing of the plugins
        Said said = cli(config, {"prefs", "get"});
        QVERIFY2(said.code == 0, qPrintable(said.described));
        QVERIFY2(unsaid(said.json), said.json.dump().c_str());
        for (const auto& [path, value] : said.json["values"].items()) QVERIFY2(path.rfind("plugIns.", 0) != 0, path.c_str());
        said = cli(config, {"prefs", "get", "plugIns"});
        QVERIFY2(said.code == 0, qPrintable(said.described));
        QCOMPARE(said.json["values"], Json::object());
        for (const char* path : {"plugIns.enabled", "plugIns.chosen", "plugIns.useExtraFolder", "plugIns.extraFolder", "plugIns.extra_folder"}) {
            QCOMPARE(cli(config, {"prefs", "get", QString::fromUtf8(path)}).json["error"], Json("unknown_pref"));
        }
        // set: refused by name, all or none (the other value not written either)
        for (const auto& [path, value] : std::vector<std::pair<const char*, const char*>>{{"plugIns.enabled", "false"},
                                                                                       {"plugIns.chosen", "{}"},
                                                                                       {"plugIns.use_extra_folder", "false"},
                                                                                       {"plugIns.extraFolder", "\"/tmp\""}}) {
            said = cli(config, {"prefs", "set", QString::fromUtf8(path), QString::fromUtf8(value)});
            QVERIFY2(said.code == 1 && said.json["error"] == Json("unknown_pref"), path);
        }
        said = cli(config, {"prefs", "set", "--values", R"({"interface.theme": "dark", "plugIns": {"enabled": false, "chosen": {}}})"});
        QCOMPARE(said.json["error"], Json("unknown_pref"));
        QCOMPARE(cli(config, {"prefs", "get", "interface.theme"}).json["values"]["interface.theme"], Json("system"));
        // reset: everything, the section — the plugins' file as it was
        QCOMPARE(cli(config, {"prefs", "reset", "plugIns"}).code, 0);
        QCOMPARE(cli(config, {"prefs", "reset"}).code, 0);
        QCOMPARE(bytes_of(file), was);
        // MCP: inspect prefs — nothing of the plugins, the plugins' paths unknown
        {
            Mcp mcp(root, config);
            Json got = mcp.call("inspect", Json{{"project", "a.genko"}, {"target", "prefs"}});
            QVERIFY(got["prefs"].is_object() && !got["prefs"].empty());
            QVERIFY2(unsaid(got), got.dump().c_str());
            got = mcp.call("inspect", Json{{"project", "a.genko"}, {"target", "prefs"}, {"op", "plugIns"}});
            QCOMPARE(got["prefs"], Json::object());
            for (const char* path : {"plugIns.enabled", "plugIns.chosen", "plugIns.extraFolder"}) {
                got = mcp.call("inspect", Json{{"project", "a.genko"}, {"target", "prefs"}, {"op", path}});
                QVERIFY2(got.dump().find("unknown_pref") != std::string::npos && unsaid(got), got.dump().c_str());
            }
        }
        QCOMPARE(bytes_of(file), was);
    }

    // The settings of an earlier version stay as they were: reading changes nothing in the file; setting and resetting
    // change only what they name (the person's keys, the tools' memories, the workspaces kept, keys this build does not know).
    void anEarlierVersionsSettingsAreKept() {
        QTemporaryDir dir;
        const QString file = dir.path() + QStringLiteral("/settings.ini");
        {
            QSettings before(file, QSettings::IniFormat);
            before.setValue(QStringLiteral("ui/theme"), QStringLiteral("dark"));
            before.setValue(QStringLiteral("ui/font_pt"), 13);
            before.setValue(QStringLiteral("shortcuts/ブラシツール"), QStringLiteral("B|Ctrl+Alt+B"));
            before.setValue(QStringLiteral("brush/kind"), QStringLiteral("gpen"));
            before.setValue(QStringLiteral("workspaces/ペン入れ/state"), QByteArray("\x01\x02\x03", 3));
            before.setValue(QStringLiteral("someday/unknown"), QStringLiteral("kept"));
            before.setValue(QStringLiteral("migration/done"), QStringLiteral("yes"));
        }
        const QByteArray was = bytes_of(file);
        QCOMPARE(cli(dir.path(), {"prefs", "get"}).code, 0);
        QCOMPARE(cli(dir.path(), {"prefs", "get", "state"}).code, 0);
        QCOMPARE(cli(dir.path(), {"commands"}).code, 0);
        QCOMPARE(bytes_of(file), was);  // (read: not a byte written)
        QCOMPARE(cli(dir.path(), {"prefs", "set", "general.beepWhenDone", "true"}).code, 0);
        QCOMPARE(cli(dir.path(), {"prefs", "reset", "cursors"}).code, 0);
        QSettings after(file, QSettings::IniFormat);
        QCOMPARE(after.value(QStringLiteral("ui/theme")).toString(), QStringLiteral("dark"));
        QCOMPARE(after.value(QStringLiteral("ui/font_pt")).toInt(), 13);
        QCOMPARE(after.value(QStringLiteral("shortcuts/ブラシツール")).toString(), QStringLiteral("B|Ctrl+Alt+B"));
        QCOMPARE(after.value(QStringLiteral("brush/kind")).toString(), QStringLiteral("gpen"));
        QCOMPARE(after.value(QStringLiteral("workspaces/ペン入れ/state")).toByteArray(), QByteArray("\x01\x02\x03", 3));
        QCOMPARE(after.value(QStringLiteral("someday/unknown")).toString(), QStringLiteral("kept"));
        QCOMPARE(after.value(QStringLiteral("migration/done")).toString(), QStringLiteral("yes"));
        QCOMPARE(after.value(QStringLiteral("ui/beep_when_done")).toString(), QStringLiteral("true"));
        // (the values the app reads as before)
        const Json got = cli(dir.path(), {"prefs", "get", "interface"}).json["values"];
        QCOMPARE(got["interface.theme"], Json("dark"));
        QCOMPARE(got["interface.uiFontSize"], Json(13));
    }

    // genko commands: the catalogue's commands with the keys of the set chosen, the person's own set and the person's keys.
    void commandsWithThisComputersKeys() {
        QTemporaryDir dir;
        const QString config = dir.path();
        Said said = cli(config, {"commands"});
        QVERIFY2(said.code == 0, qPrintable(said.described));
        const Json& listed = said.json;
        QCOMPARE(listed["ok"], Json(true));
        QCOMPARE(listed["key_set"], Json("photoshop"));
        QCOMPARE(listed["commands"].size(), genko::core::commands::catalogue()["commands"].size());
        QVERIFY(listed["commands"].size() > 500);
        for (const Json& c : listed["commands"]) {
            QVERIFY(c["id"].is_string() && c["name"].is_string() && c["menu"].is_array() && c["keys"].is_array() && c["checkable"].is_boolean());
        }
        const Json* undo = command(listed, "act_undo");
        QVERIFY(undo != nullptr);
        // (PC5 H-f: 編集 → メモリをクリア ▸ 取り消し shares its words — the user's decision 8: told apart by their places)
        QCOMPARE((*undo)["name"], Json("取り消し（編集）"));
        QCOMPARE((*undo)["keys"], Json::array({"Ctrl+Z"}));
        QCOMPARE((*undo)["menu"], Json::array({"編集"}));
        // (PC5 H-f review P3-8: the history panel's switch by its own id, whatever its name — ヒストリー（ウィンドウ） now)
        const Json* history = command(listed, "dock_history");
        QVERIFY(history != nullptr);
        QCOMPARE((*history)["name"], Json("ヒストリー（ウィンドウ）"));
        const Json* move = command(listed, "act_move");
        QVERIFY(move != nullptr && move->value("tool", false));
        QCOMPARE((*move)["keys"], Json::array({"V"}));  // (Photoshop's 移動ツール)
        // the Genko set
        QCOMPARE(cli(config, {"prefs", "set", "keyboardShortcuts.keySet", "\"genko\""}).code, 0);
        said = cli(config, {"commands"});
        QCOMPARE(said.json["key_set"], Json("genko"));
        const Json* genko_move = command(said.json, "act_move");
        QVERIFY(genko_move != nullptr);
        QVERIFY((*genko_move)["keys"] != Json::array({"V"}));
        // the person's keys: theirs, and a set's key they gave another taken off the command that had it
        QCOMPARE(cli(config, {"prefs", "set", "keyboardShortcuts.keySet", "\"photoshop\""}).code, 0);
        QCOMPARE(cli(config, {"prefs", "set", "shortcuts.ズームイン", "\"Ctrl+Z\""}).code, 0);
        said = cli(config, {"commands"});
        const Json* zoom = command(said.json, "act_zoom_in");
        QVERIFY(zoom != nullptr);
        QCOMPARE((*zoom)["keys"], Json::array({"Ctrl+Z"}));
        QCOMPARE((*command(said.json, "act_undo"))["keys"], Json::array());
        // the person's own set over its base
        {
            QSettings ini(config + QStringLiteral("/settings.ini"), QSettings::IniFormat);
            ini.remove(QStringLiteral("shortcuts"));
            ini.setValue(QStringLiteral("key_sets/私の組/base"), QStringLiteral("photoshop"));
            ini.setValue(QStringLiteral("key_sets/私の組/keys"), QStringLiteral(R"({"取り消し":"Ctrl+Alt+Z"})"));
            ini.setValue(QStringLiteral("ui/key_set_own"), QStringLiteral("私の組"));
        }
        said = cli(config, {"commands"});
        QCOMPARE(said.json["key_set"], Json("own:私の組"));
        QCOMPARE((*command(said.json, "act_undo"))["keys"], Json::array({"Ctrl+Alt+Z"}));
        // a menu, words
        said = cli(config, {"commands", "--menu", "編集"});
        QVERIFY(!said.json["commands"].empty());
        for (const Json& c : said.json["commands"]) QCOMPARE(c["menu"][0], Json("編集"));
        said = cli(config, {"commands", "--search", "ズーム イン"});
        QVERIFY(command(said.json, "act_zoom_in") != nullptr);
        QCOMPARE(cli(config, {"commands", "--menu"}).code, 2);
        QCOMPARE(cli(config, {"commands", "extra"}).code, 2);
    }

    // genko session: no app, or an app gone — not running; an app there — its session.json.
    void sessionOfTheRunningApp() {
        QTemporaryDir dir;
        const QString config = dir.path();
        QCOMPARE(cli(config, {"session"}).json, (Json{{"running", false}}));
        const qint64 me = QCoreApplication::applicationPid();
        const Json kept{{"v", 1}, {"pid", me}, {"app_version", "x"},
                        {"windows", Json::array({Json{{"active", true}, {"arrange", "tabs"}, {"front", 0},
                                                      {"tabs", Json::array({Json{{"index", 0}, {"title", "原稿"}, {"path", "/somewhere/a.genko"},
                                                                                 {"saved", true}, {"revision", 3}, {"page", 1}}})}}})},
                        {"foreground", "#000000"}, {"background", "#ffffff"}, {"updated_at", "2026-11-03T00:00:00.000Z"}};
        {
            QFile f(config + QStringLiteral("/session.json"));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(QByteArray::fromStdString(kept.dump()));
        }
        Said said = cli(config, {"session"});
        QCOMPARE(said.code, 0);
        QCOMPARE(said.json["running"], Json(true));
        QCOMPARE(said.json["windows"], kept["windows"]);
        QCOMPARE(said.json["pid"], Json(me));
        // a process gone: not running
        QProcess gone;
        gone.start(genko::test::genko_cli(), {QStringLiteral("--version")});
        QVERIFY(gone.waitForStarted(60000));
        const qint64 dead = gone.processId();  // (taken while it runs: none once it has finished)
        QVERIFY(gone.waitForFinished(60000));
        Json old = kept;
        old["pid"] = dead > 0 ? dead : 999999999;
        {
            QFile f(config + QStringLiteral("/session.json"));
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(QByteArray::fromStdString(old.dump()));
        }
        QCOMPARE(cli(config, {"session"}).json, (Json{{"running", false}}));
        {
            QFile f(config + QStringLiteral("/session.json"));
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write("not json");
        }
        QCOMPARE(cli(config, {"session"}).json, (Json{{"running", false}}));
        QCOMPARE(cli(config, {"session", "extra"}).code, 2);
    }

    // MCP: inspect target=prefs / commands / session reads; no tool writes a preference; a book's path never — a project of
    // the root, by its name.
    void mcpReadsOnly() {
        QTemporaryDir dir;
        const QString config = dir.path() + QStringLiteral("/config");
        const QString root = dir.path() + QStringLiteral("/root");
        QVERIFY(QDir().mkpath(config) && QDir().mkpath(root + QStringLiteral("/a.genko")));
        QCOMPARE(cli(config, {"prefs", "set", "interface.theme", "\"dark\""}).code, 0);
        // (review P2-1: a folder outside the root, a file inside it)
        const std::string outside = (dir.path() + QStringLiteral("/backups")).toStdString();
        const std::string inside = (root + QStringLiteral("/logs/history.txt")).toStdString();
        QVERIFY(QDir().mkpath(dir.path() + QStringLiteral("/backups")) && QDir().mkpath(root + QStringLiteral("/logs")));
        QCOMPARE(cli(config, {"prefs", "set", "--values", QString::fromStdString(Json{{"book.backupFolder", outside}, {"historyLog.filePath", inside}}.dump())}).code, 0);
        {
            QSettings ini(config + QStringLiteral("/settings.ini"), QSettings::IniFormat);
            ini.setValue(QStringLiteral("subview/images"), (dir.path() + QStringLiteral("/secret/reference.png")));
        }
        const qint64 me = QCoreApplication::applicationPid();
        const Json session{{"v", 1}, {"pid", me}, {"app_version", "x"},
                           {"windows", Json::array({Json{{"active", true}, {"arrange", "tabs"}, {"front", 0},
                                                         {"tabs", Json::array({Json{{"index", 0}, {"title", "中"}, {"path", (root + QStringLiteral("/a.genko")).toStdString()},
                                                                                    {"saved", true}, {"revision", 1}, {"page", 1}},
                                                                               Json{{"index", 1}, {"title", "外"}, {"path", (dir.path() + QStringLiteral("/b.genko")).toStdString()},
                                                                                    {"saved", false}, {"revision", 2}, {"page", 3}}})}}})}};
        {
            QFile f(config + QStringLiteral("/session.json"));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(QByteArray::fromStdString(session.dump()));
        }
        const QByteArray was = bytes_of(config + QStringLiteral("/settings.ini"));
        Mcp mcp(root, config);
        const Json tools = mcp.request(2, "tools/list", Json::object());
        for (const Json& t : tools["result"]["tools"]) {
            const std::string name = t["name"].get<std::string>();
            QVERIFY2(name.find("prefs") == std::string::npos && name.find("preference") == std::string::npos && name.find("setting") == std::string::npos, name.c_str());
        }
        Json got = mcp.call("inspect", Json{{"project", "a.genko"}, {"target", "prefs"}});
        QCOMPARE(got["prefs"]["interface.theme"], Json("dark"));
        QVERIFY(!got["prefs"].contains("state.brush.kind"));
        // (review P2-1: a path out of the root never said — in it, from the root; the app's memories refused by name too)
        QCOMPARE(got["prefs"]["book.backupFolder"], Json("(root の外)"));
        QCOMPARE(got["prefs"]["historyLog.filePath"], Json(QStringLiteral("logs/history.txt").toStdString()));
        QVERIFY2(got.dump().find(outside) == std::string::npos && got.dump().find(dir.path().toStdString()) == std::string::npos, got.dump().c_str());
        for (const char* memory : {"state", "state.subview.images", "state.ai.client"}) {
            got = mcp.call("inspect", Json{{"project", "a.genko"}, {"target", "prefs"}, {"op", memory}});
            QVERIFY2(got.dump().find("bad_value") != std::string::npos && got.dump().find("secret") == std::string::npos, got.dump().c_str());
        }
        got = mcp.call("inspect", Json{{"project", "a.genko"}, {"target", "prefs"}, {"op", "historyLog.enabled"}});
        QCOMPARE(got["prefs"], (Json{{"historyLog.enabled", false}}));
        got = mcp.call("inspect", Json{{"project", "a.genko"}, {"target", "prefs"}, {"op", "nope.nothing"}});
        QVERIFY2(got.dump().find("unknown_pref") != std::string::npos, got.dump().c_str());
        got = mcp.call("inspect", Json{{"project", "a.genko"}, {"target", "commands"}, {"op", "取り消し"}});
        QCOMPARE(got["key_set"], Json("photoshop"));
        QVERIFY(command(got, "act_undo") != nullptr);
        got = mcp.call("inspect", Json{{"project", "a.genko"}, {"target", "session"}});
        QCOMPARE(got["session"]["running"], Json(true));
        const Json& tabs = got["session"]["windows"][0]["tabs"];
        QCOMPARE(tabs.size(), std::size_t{2});
        QCOMPARE(tabs[0]["project"], Json("a.genko"));
        QVERIFY(!tabs[0].contains("path") && !tabs[1].contains("path") && !tabs[1].contains("project"));
        QCOMPARE(tabs[1]["title"], Json("外"));
        QCOMPARE(bytes_of(config + QStringLiteral("/settings.ini")), was);  // (read: the file as it was)
    }
};

QTEST_GUILESS_MAIN(TestContractPrefs)
#include "test_contract_prefs.moc"
