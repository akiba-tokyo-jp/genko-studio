// `genko mcp` and `genko serve` end to end where this build draws its own lines (SPEC COMP-01a) and where an agent must
// not get through (ACCEPTANCE AC-API), without Python: the tool list holds no person's tool and the server never runs as
// a person; an agent's refusals through MCP and HTTP (a gate, a lock, the person's switch for chat approvals, what a
// person pinned, a person's change undone, a panel's regions) leave the book as it was; numbers this build cannot carry (NaN,
// infinities, integers past 64 bits, in JSON or as text) are refused; a v1–v3 book is not written (needs_migration); a
// person's upscaler gets only the arguments the person registered (the agent picks a registered name and a scale, nothing
// that reaches the command line), and what it prints never reaches the protocol; the HTTP API: no route reaches a
// person's approvals, an agent's token cannot approve or unlock, tokens.json is read again when it changes (a revoked
// token stops at once), /v1/new never writes over a book, the files ops read stay in the root inside for_pages too,
// NaN and deep JSON are refused, pictures that are not pictures or too big are refused, oversized and malformed
// requests (Content-Length, headers) are refused before anything is read, the guard answers from the head before any
// body is read, import_pages reads books inside the root only, integers past the signed 64 bits are refused as numbers
// and as text, a request's folder must be inside the root, a --root that cannot be resolved ends the command; the
// server: at most 32 connections from one address and 64 in all, early refusals read whole by a client still
// sending, a slow head cut off after 10 s, a stop that does not wait for requests still arriving, request threads
// bounded (stdio and the 2026 era's HTTP); the MCP HTTP endpoint's Host, Origin, size and body checks and its strict
// chunked bodies.

#include <QtTest>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QProcess>
#include <QRegularExpression>
#include <QTcpSocket>
#include <QTemporaryDir>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "core/base64.hpp"
#include "core/json.hpp"
#include "render/image.hpp"
#include "render/png.hpp"
#include "testsupport.hpp"

using genko::core::Json;

namespace {

QString genko_exe() { return genko::test::genko_cli(); }

// A server's text ("" when the value is not one: the check that uses it then fails instead of throwing).
std::string str(const Json& value) { return value.is_string() ? value.get<std::string>() : std::string(); }

// One `genko mcp` over stdio: messages in, lines out.
class Mcp {
public:
    Mcp(const QString& root, const QString& config, const QString& agent = QStringLiteral("ai:test"),
        const std::map<QString, QString>& extra = {}) {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("GENKO_CONFIG_DIR"), config);
        env.insert(QStringLiteral("LANG"), QStringLiteral("C"));  // (Qt's locale warning goes to stderr, never stdout)
        env.remove(QStringLiteral("LC_ALL"));
        for (const auto& [k, v] : extra) env.insert(k, v);
        process_.setProcessEnvironment(env);
        process_.start(genko_exe(), {QStringLiteral("mcp"), QStringLiteral("--root"), root, QStringLiteral("--agent"), agent});
        process_.waitForStarted(30000);
    }
    ~Mcp() {
        if (process_.state() != QProcess::NotRunning) {
            process_.closeWriteChannel();
            if (!process_.waitForFinished(120000)) process_.kill();
        }
    }

    void send(const Json& message) { send_raw(genko::core::dump_python(message)); }
    // One line as it is (JSON this build's own types may not hold: NaN, Infinity, big integers).
    void send_raw(const std::string& line) {
        process_.write(QByteArray::fromStdString(line + "\n"));
        process_.waitForBytesWritten(30000);
    }

    // The answer to `id` (null after the timeout). Every line must be a JSON-RPC message.
    Json answer(const Json& id, int timeout_ms = 300000) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            for (auto it = pending_.begin(); it != pending_.end(); ++it) {
                if ((*it).contains("id") && (*it)["id"] == id) {
                    Json found = *it;
                    pending_.erase(it);
                    return found;
                }
            }
            while (process_.canReadLine()) {
                const QByteArray line = process_.readLine();
                lines_.push_back(line);
                try {
                    pending_.push_back(genko::core::parse_python_json(line.toStdString()));
                } catch (const std::exception&) {  // (not a message: kept in lines_, where the tests look at every line)
                }
            }
            if (std::any_of(pending_.begin(), pending_.end(), [&id](const Json& m) { return m.contains("id") && m["id"] == id; })) continue;
            if (std::chrono::steady_clock::now() > deadline) return Json();
            if (process_.state() == QProcess::NotRunning && !process_.canReadLine()) return Json();  // (it ended: no answer)
            process_.waitForReadyRead(200);
        }
    }

    Json request(const Json& id, const std::string& method, const Json& params = Json()) {
        Json message = Json::object({{"jsonrpc", "2.0"}, {"id", id}, {"method", method}});
        if (!params.is_null()) message["params"] = params;
        send(message);
        return answer(id);
    }

    void initialize() {
        Json reply = request(0, "initialize",
                                   Json::object({{"protocolVersion", "2025-06-18"},
                                                 {"capabilities", Json::object()},
                                                 {"clientInfo", Json::object({{"name", "test"}, {"version", "1"}})}}));
        QVERIFY(reply.contains("result"));
        send(Json::object({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}));
    }

    // tools/call: (the reply parsed from its first text, is_error, the content blocks)
    Json call(const std::string& tool, const Json& args) {
        Json reply = request(++next_, "tools/call", Json::object({{"name", tool}, {"arguments", args}}));
        return reply;
    }
    // The JSON a tool answered with (its first text block).
    // (null when there is none: the server did not answer, or answered with an error)
    static Json text(const Json& reply) {
        const Json* content = reply.is_object() && reply.contains("result") && reply["result"].is_object() ? &reply["result"] : nullptr;
        if (content == nullptr || !content->contains("content") || !(*content)["content"].is_array() || (*content)["content"].empty() ||
            !(*content)["content"][0].contains("text")) {
            return Json();
        }
        const std::string words = str((*content)["content"][0]["text"]);
        try {
            return genko::core::parse_python_json(words);
        } catch (const std::exception&) {
            return Json(words);  // (a text that is not JSON: "Error executing tool …")
        }
    }

    // What was printed on stdout, all of it: every line a JSON-RPC message.
    std::vector<QByteArray> lines() {
        process_.closeWriteChannel();
        process_.waitForFinished(300000);
        while (process_.canReadLine()) lines_.push_back(process_.readLine());
        const QByteArray rest = process_.readAll();
        if (!rest.isEmpty()) lines_.push_back(rest);
        return lines_;
    }
    QByteArray err() { return process_.readAllStandardError(); }
    qint64 pid() const { return process_.processId(); }

private:
    QProcess process_;
    std::vector<Json> pending_;
    std::vector<QByteArray> lines_;
    int next_ = 100;
};

struct Response {
    int status = 0;
    std::map<std::string, std::string> headers;
    std::string body;
    Json json() const {
        try {
            return genko::core::parse_python_json(body);
        } catch (const std::exception&) {
            return Json();  // (not JSON: the checks on it fail)
        }
    }
};

// One raw HTTP/1.1 request (the server closes the connection after its answer).
Response http(std::uint16_t port, const std::string& raw, int timeout_ms = 300000, const QString& source = {}) {
    QTcpSocket socket;
    if (!source.isEmpty()) socket.bind(QHostAddress(source), 0);
    socket.connectToHost(QStringLiteral("127.0.0.1"), port);
    Response out;
    if (!socket.waitForConnected(30000)) return out;
    socket.write(raw.data(), static_cast<qint64>(raw.size()));
    socket.waitForBytesWritten(30000);
    std::string data;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (socket.waitForReadyRead(500)) {
            data += socket.readAll().toStdString();
        } else if (socket.state() != QAbstractSocket::ConnectedState) {
            data += socket.readAll().toStdString();
            break;
        }
    }
    const std::size_t head_end = data.find("\r\n\r\n");
    if (head_end == std::string::npos) return out;
    const std::string head = data.substr(0, head_end);
    out.status = std::stoi(head.substr(9, 3));
    std::size_t at = head.find("\r\n");
    while (at != std::string::npos) {
        const std::size_t next = head.find("\r\n", at + 2);
        const std::string line = head.substr(at + 2, next == std::string::npos ? std::string::npos : next - at - 2);
        const std::size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string name = line.substr(0, colon);
            for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            out.headers[name] = line.substr(colon + 2);
        }
        at = next;
    }
    out.body = data.substr(head_end + 4);
    return out;
}

std::string request_text(const std::string& method, const std::string& path, std::uint16_t port, const std::string& token,
                         const std::string& body = {}, const std::string& type = "application/json", const std::string& extra = {}) {
    std::string raw = method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\n";
    if (!token.empty()) raw += "Authorization: Bearer " + token + "\r\n";
    if (!body.empty()) raw += "Content-Type: " + type + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    raw += extra + "\r\n" + body;
    return raw;
}

// A server process (`genko serve` / `genko mcp --transport streamable-http`) and the port it said it listens on.
class Server {
public:
    Server(const QStringList& args, const QString& config) {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("GENKO_CONFIG_DIR"), config);
        process_.setProcessEnvironment(env);
        process_.start(genko_exe(), args);
        process_.waitForStarted(30000);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
        while (port == 0 && std::chrono::steady_clock::now() < deadline) {
            if (!process_.waitForReadyRead(500) && process_.state() != QProcess::Running) break;
            err_ += process_.readAllStandardError();
            const QRegularExpressionMatch m = QRegularExpression(QStringLiteral(R"(http://127\.0\.0\.1:(\d+))")).match(QString::fromUtf8(err_));
            if (m.hasMatch()) port = static_cast<std::uint16_t>(m.captured(1).toUInt());
        }
    }
    ~Server() {
        if (process_.state() != QProcess::NotRunning) {
            process_.terminate();  // (SIGTERM: served requests end, then it stops)
            if (!process_.waitForFinished(120000)) process_.kill();
        }
    }
    int stop() {
        process_.terminate();
        process_.waitForFinished(120000);
        return process_.exitCode();
    }
    qint64 pid() const { return process_.processId(); }
    std::uint16_t port = 0;

private:
    QProcess process_;
    QByteArray err_;
};

// The threads a process runs now (Linux: /proc; 0 elsewhere).
int threads_of(qint64 pid) {
    QFile status(QStringLiteral("/proc/%1/status").arg(pid));
    if (!status.open(QIODevice::ReadOnly)) return 0;
    for (const QByteArray& line : status.readAll().split('\n')) {
        if (line.startsWith("Threads:")) return line.mid(8).trimmed().toInt();
    }
    return 0;
}

// A connection from `source` (a loopback address of this machine) that has sent `raw` and stays open.
std::unique_ptr<QTcpSocket> held_from(const QString& source, std::uint16_t port, const std::string& raw) {
    auto socket = std::make_unique<QTcpSocket>();
    socket->bind(QHostAddress(source), 0);
    socket->connectToHost(QStringLiteral("127.0.0.1"), port);
    if (!socket->waitForConnected(30000)) return nullptr;
    socket->write(raw.data(), static_cast<qint64>(raw.size()));
    socket->waitForBytesWritten(30000);
    return socket;
}

// Everything a server answers to `head` followed by `extra` bytes the client keeps sending (in pieces, without
// reading in between), read once the client has sent them all or the server stopped taking them.
std::string answer_while_sending(std::uint16_t port, const std::string& head, std::size_t extra, int timeout_ms = 60000) {
    QTcpSocket socket;
    socket.connectToHost(QStringLiteral("127.0.0.1"), port);
    if (!socket.waitForConnected(30000)) return {};
    socket.write(head.data(), static_cast<qint64>(head.size()));
    socket.waitForBytesWritten(30000);
    const std::string piece(64 * 1024, 'x');
    for (std::size_t sent = 0; sent < extra && socket.state() == QAbstractSocket::ConnectedState; sent += piece.size()) {
        if (socket.write(piece.data(), static_cast<qint64>(piece.size())) < 0) break;
        if (!socket.waitForBytesWritten(5000)) break;
    }
    std::string data;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (socket.waitForReadyRead(500)) {
            data += socket.readAll().toStdString();
        } else if (socket.state() != QAbstractSocket::ConnectedState) {
            data += socket.readAll().toStdString();
            break;
        }
    }
    return data;
}

// A 2026-era request over HTTP (its envelope, its routing headers).
std::string modern_post(std::uint16_t port, int id, const std::string& tool, const Json& arguments) {
    const Json meta = Json::object({{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                                    {"io.modelcontextprotocol/clientCapabilities", Json::object()},
                                    {"io.modelcontextprotocol/clientInfo", Json::object({{"name", "t"}, {"version", "1"}})}});
    const std::string body = genko::core::dump_python(Json::object(
        {{"jsonrpc", "2.0"}, {"id", id}, {"method", "tools/call"}, {"params", Json::object({{"name", tool}, {"arguments", arguments}, {"_meta", meta}})}}));
    return "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
           "\r\nContent-Type: application/json\r\nAccept: application/json, text/event-stream\r\nMCP-Protocol-Version: 2026-07-28\r\n"
           "Mcp-Method: tools/call\r\nMcp-Name: " + tool + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

std::string png_bytes(int w, int h) { return genko::render::write_png(genko::render::Image::create("RGB", genko::render::Size{w, h})); }

// A PNG whose header says it is huge (the pixels are not there): refused before anything is decoded.
std::string huge_png() {
    std::string png = png_bytes(4, 4);
    // IHDR's width and height (bytes 16..23), then its CRC fixed
    const auto put = [&png](std::size_t at, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) png[at + i] = static_cast<char>((v >> (24 - 8 * i)) & 0xFF);
    };
    put(16, 200000);
    put(20, 200000);
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 12; i < 29; ++i) {
        crc ^= static_cast<unsigned char>(png[i]);
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    put(29, crc ^ 0xFFFFFFFFu);
    return png;
}

std::map<std::string, std::string> tree(const QString& dir) { return genko::test::tree_hashes(dir, false); }
// The same, without what an MCP call always leaves (the tool log and presence: studio/logs/, studio/presence.json).
std::map<std::string, std::string> book_tree(const QString& dir) {
    std::map<std::string, std::string> out = tree(dir);
    std::erase_if(out, [](const auto& item) { return item.first.starts_with("studio/logs/") || item.first == "studio/presence.json"; });
    return out;
}
// What differs between two trees ("" when nothing).
std::string tree_diff(const std::map<std::string, std::string>& a, const std::map<std::string, std::string>& b) {
    std::string out;
    for (const auto& [name, hash] : a) {
        const auto found = b.find(name);
        if (found == b.end()) out += "gone: " + name + "\n";
        else if (found->second != hash) out += "changed: " + name + "\n";
    }
    for (const auto& [name, hash] : b) {
        if (!a.contains(name)) out += "new: " + name + "\n";
    }
    return out;
}

void write_file(const QString& path, const std::string& bytes) {
    QDir().mkpath(QFileInfo(path).path());
    genko::test::write_bytes(path, bytes);
}

// A job record that says the job is running, its heartbeat in the future (it never looks lost during a test).
void running_job(const QString& book, const std::string& id) {
    const double now = static_cast<double>(QDateTime::currentMSecsSinceEpoch()) / 1000.0;
    write_file(book + "/studio/jobs/" + QString::fromStdString(id) + ".json",
               genko::core::dump_python(Json::object({{"id", id}, {"kind", "export"}, {"actor", "ai:test"}, {"status", "running"},
                                                      {"started", now}, {"beat", now + 3600}, {"pid", 1}})));
}

const char* const kTools[] = {"projects", "create_project", "status", "next", "inspect", "render", "import_image", "generation_request",
                              "import_images", "candidates", "review_candidates", "adopt", "request_fix", "report_regions",
                              "finish_page", "preflight", "export_proof", "check", "undo", "export", "record_chat_approval",
                              "export_status", "upscale", "derive", "import_name", "analyze_name", "propose_lines", "proposals",
                              "review_page", "ask_human", "set_bible", "set_script", "submit_name", "take_panel_art", "style_catalog",
                              "use_style", "apply_ops", "record_review", "request_approval", "tickets", "resolve_ticket"};

}  // namespace

class TestMcpApiE2e : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;

    QString fresh(const QString& name) {
        const QString dir = tmp_.path() + "/" + name;
        QDir().mkpath(dir + "/root");
        QDir().mkpath(dir + "/config");
        return dir;
    }

    // A book made through MCP (`pages` pages), its first page's frame's id.
    static std::string make_book(Mcp& mcp, const std::string& name, int pages = 1) {
        Json made = Mcp::text(mcp.call("create_project", Json::object({{"name", name}, {"title", "t"}, {"pages", pages}, {"spec_preset", "doujin-a5"}})));
        if (made["ok"] != Json(true)) return {};
        return frame_of(mcp, name, 1);
    }
    static std::string frame_of(Mcp& mcp, const std::string& name, int page) {
        Json snapshot = Mcp::text(mcp.call("inspect", Json::object({{"project", name}, {"target", "snapshot"}, {"page", page}})));
        Json found = snapshot.is_object() ? snapshot.value(Json::json_pointer("/snapshot/pages/0/leaves/0/id"), Json()) : Json();
        return found.is_string() ? found.get<std::string>() : std::string();
    }

private slots:
    // The server is an agent's: a person's actor is refused before anything is served; the 41 tools hold none of a
    // person's (approvals, the chat switch, revocations, comments, proposals, tickets, the official export).
    void onlyAnAgentsTools() {
        const QString dir = fresh("tools");
        const genko::test::Run as_person = genko::test::run(genko_exe(), {"mcp", "--root", dir + "/root", "--agent", "human:me"},
                                                            QProcessEnvironment::systemEnvironment(), 60000);
        QCOMPARE(as_person.exit_code, 1);
        QVERIFY(as_person.out.isEmpty());
        QVERIFY(as_person.err.contains("ai:<"));
        const genko::test::Run legacy_actor = genko::test::run(genko_exe(), {"mcp", "--root", dir + "/root", "--agent", "genko"},
                                                               QProcessEnvironment::systemEnvironment(), 60000);
        QCOMPARE(legacy_actor.exit_code, 1);

        Mcp mcp(dir + "/root", dir + "/config");
        mcp.initialize();
        Json listed = mcp.request(1, "tools/list");
        std::vector<std::string> names;
        for (const Json& t : listed["result"]["tools"]) names.push_back(str(t["name"]));
        QCOMPARE(names.size(), std::size(kTools));
        for (std::size_t i = 0; i < names.size(); ++i) QCOMPARE(names[i], std::string(kTools[i]));
        for (const char* person : {"approve", "approve_name", "approve_art", "approve_sheet", "chat_approval", "revoke", "comment",
                                   "accept_proposal", "reject_proposal", "close_ticket", "reopen_ticket", "export_book", "upscalers"}) {
            Json reply = mcp.call(person, Json::object({{"project", "x"}}));
            QCOMPARE(reply["result"]["isError"], Json(true));
            QCOMPARE(str(reply["result"]["content"][0]["text"]), "Unknown tool: " + std::string(person));
        }
        // every line on stdout is a JSON-RPC message, with Qt's locale warning (LANG=C) on stderr
        for (const QByteArray& line : mcp.lines()) {
            Json message;
            try {
                message = genko::core::parse_python_json(line.toStdString());
            } catch (const std::exception&) {
                QFAIL(("a line on stdout that is not JSON: " + line.toStdString()).c_str());
            }
            QCOMPARE(message["jsonrpc"], Json("2.0"));
        }
    }

    // An agent's refusals through MCP leave the book as it was: a gate op, a page lock, the person's switch for chat
    // approvals, a field a person pinned (changed, removed, unpinned), a panel's regions (this build: a person's), a
    // person's last change undone. Only the tool log and presence change (each refusal is logged).
    void anAgentCannotGetThrough() {
        const QString dir = fresh("gates");
        const QString book = dir + "/root/b.genko";
        std::string frame2;
        {
            Mcp mcp(dir + "/root", dir + "/config");
            mcp.initialize();
            QVERIFY(!make_book(mcp, "b.genko", 2).empty());
            frame2 = frame_of(mcp, "b.genko", 2);
        }
        // a person stops chat approvals, locks page 1 and pins the shot of page 2's panel
        QCOMPARE(genko::test::run_genko({"studio", "chat-approval", book, "off", "--as", "human:owner"}).exit_code, 0);
        const QString ops = dir + "/lock.json";
        write_file(ops, genko::core::dump_python(Json::array(
                            {Json::object({{"op", "lock_page"}, {"page", 1}}),
                             Json::object({{"op", "set_panel"}, {"page", 2}, {"frame_id", frame2}, {"set", Json::object({{"shot", "CU"}})}, {"pin", Json::array({"shot"})}})})));
        const genko::test::Run locked = genko::test::run_genko({"apply", book, ops, "--agent", "human:owner"});
        QVERIFY2(locked.exit_code == 0, locked.out.constData());
        const auto before = book_tree(book);
        const std::size_t logged = genko::test::read_bytes(book + "/studio/logs/tools.jsonl").size();

        Mcp mcp(dir + "/root", dir + "/config");
        mcp.initialize();
        const auto refused = [&mcp](const char* tool, const Json& args, const std::string& code_or_words) {
            Json reply = Mcp::text(mcp.call(tool, args));
            QCOMPARE(reply["ok"], Json(false));
            const std::string all = genko::core::dump_python(reply);
            QVERIFY2(all.find(code_or_words) != std::string::npos, all.c_str());
        };
        for (const char* op : {"approve", "name_ok", "lock_page", "unlock_page", "allow_chat_approval", "revoke"}) {
            refused("apply_ops", Json::object({{"project", "b.genko"}, {"ops", Json::array({Json::object({{"op", op}, {"page", 1}})})}, {"commit", true}}),
                    "op_not_allowed");
        }
        refused("record_chat_approval",
                Json::object({{"project", "b.genko"}, {"gate", "name"}, {"message", "OK"}, {"pages", Json::array({1})}, {"person", "owner"}}),
                "chat_approval_off");
        refused("apply_ops", Json::object({{"project", "b.genko"}, {"ops", Json::array({Json::object({{"op", "set_note"}, {"page", 1}, {"note", "x"}})})},
                                           {"commit", true}}),
                "human:owner");
        refused("apply_ops",
                Json::object({{"project", "b.genko"},
                              {"ops", Json::array({Json::object(
                                          {{"op", "set_panel"}, {"page", 2}, {"frame_id", frame2},
                                           {"set", Json::object({{"regions", Json::array({Json::object({{"kind", "face"}, {"rect_mm", Json::array({1, 1, 5, 5})}})})}})}})})},
                              {"commit", true}}),
                "人だけ");
        const auto panel = [&frame2](const Json& fields) {
            Json op = Json::object({{"op", "set_panel"}, {"page", 2}, {"frame_id", frame2}});
            for (const auto& [key, value] : fields.items()) op[key] = value;
            return Json::object({{"project", "b.genko"}, {"ops", Json::array({op})}, {"commit", true}});
        };
        refused("apply_ops", panel(Json::object({{"set", Json::object({{"shot", "LS"}})}})), "shot is pinned by a person");
        refused("apply_ops", panel(Json::object({{"unset", Json::array({"shot"})}})), "shot is pinned by a person");
        refused("apply_ops", panel(Json::object({{"unpin", Json::array({"shot"})}})), "only a person can unpin");
        refused("undo", Json::object({{"project", "b.genko"}}), "human:owner");
        mcp.lines();
        const auto after = book_tree(book);
        QVERIFY2(after == before, tree_diff(before, after).c_str());
        QVERIFY(genko::test::read_bytes(book + "/studio/logs/tools.jsonl").size() > logged);
    }

    // Numbers this build cannot carry (NaN, infinities, integers past 64 bits; in the JSON or as text) are refused,
    // and nothing is written.
    void numbersThisBuildCannotCarry() {
        const QString dir = fresh("numbers");
        Mcp mcp(dir + "/root", dir + "/config");
        mcp.initialize();
        QVERIFY(!make_book(mcp, "b.genko").empty());
        const auto before = book_tree(dir + "/root/b.genko");
        struct Case {
            std::string arguments;
            std::string where;
        };
        const std::vector<Case> cases = {
            {R"({"project": "b.genko", "candidate_id": "c", "scale": NaN})", "/scale"},
            {R"({"project": "b.genko", "candidate_id": "c", "scale": Infinity})", "/scale"},
            {R"({"project": "b.genko", "candidate_id": "c", "offset_mm": [1, -Infinity]})", "/offset_mm/1"},
            {R"({"project": "b.genko", "candidate_id": "c", "page": 123456789012345678901234567890})", "/page"},
            {R"({"project": "b.genko", "candidate_id": "c", "scale": "1e400"})", "/scale"},
            {R"({"project": "b.genko", "candidate_id": "c", "scale": "nan"})", "/scale"},
            {R"({"project": "b.genko", "candidate_id": "c", "page": "123456789012345678901234567890"})", "/page"},
            {R"({"project": "b.genko", "candidate_id": "c", "offset_mm": "[1, NaN]"})", "/offset_mm/1"},
        };
        int id = 500;
        for (const Case& c : cases) {
            ++id;
            mcp.send_raw(R"({"jsonrpc": "2.0", "id": )" + std::to_string(id) + R"(, "method": "tools/call", "params": {"name": "adopt", "arguments": )" +
                         c.arguments + "}}");
            Json reply = mcp.answer(id);
            QVERIFY2(!reply.is_null(), c.arguments.c_str());
            Json text = Mcp::text(reply);
            QCOMPARE(text["code"], Json("unsupported_number"));
            QVERIFY2(str(text["detail"]).ends_with(" at " + c.where), str(text["detail"]).c_str());
            QVERIFY(str(text["error"]).find("扱えない数") != std::string::npos);
        }
        const auto after = book_tree(dir + "/root/b.genko");
        QVERIFY2(after == before, tree_diff(before, after).c_str());
    }

    // A v1–v3 book is read but not written through MCP (genko migrate first): needs_migration, and its files stay as
    // they were (no tool log, no presence either).
    void oldBooksAreNotWritten() {
        const QString dir = fresh("legacy");
        genko::test::copy_tree(genko::test::test_data("legacy/book-v3.genko"), dir + "/root/old.genko");
        const auto before = tree(dir + "/root/old.genko");
        Mcp mcp(dir + "/root", dir + "/config");
        mcp.initialize();
        Json status = Mcp::text(mcp.call("status", Json::object({{"project", "old.genko"}})));
        QCOMPARE(status["ok"], Json(true));
        Json write = Mcp::text(mcp.call(
            "apply_ops", Json::object({{"project", "old.genko"}, {"ops", Json::array({Json::object({{"op", "set_note"}, {"page", 1}, {"note", "x"}})})}, {"commit", true}})));
        QCOMPARE(write["ok"], Json(false));
        QCOMPARE(write["code"], Json("needs_migration"));
        QVERIFY(str(write["error"]).find("genko migrate") != std::string::npos);
        mcp.lines();
        const auto after = tree(dir + "/root/old.genko");
        QVERIFY2(after == before, tree_diff(before, after).c_str());
    }

    // A person's upscaler: the agent names a registered one and a scale; the command line is the person's, with Genko's
    // own temporary files; what the program prints never reaches the protocol. Names that are not registered (paths,
    // shell words) are refused without running anything.
    void anUpscalersArgumentsAreThePersons() {
        const QString dir = fresh("upscale");
        const QString log = dir + "/argv.log";
        const Json registry = Json::object(
            {{"fake", Json::object({{"command", Json::array({genko::test::argv_helper().toStdString(), "-i", "{in}", "-o", "{out}", "-s", "{scale}"})},
                                    {"scales", Json::array({2})},
                                    {"label", "test"}})}});
        write_file(dir + "/config/upscalers.json", genko::core::dump_python(registry));
        const QString book = dir + "/root/b.genko";
        std::string frame;
        std::string request;
        {
            Mcp mcp(dir + "/root", dir + "/config");
            mcp.initialize();
            frame = make_book(mcp, "b.genko");
            QVERIFY(!frame.empty());
        }
        QCOMPARE(genko::test::run_genko({"studio", "approve", book, "name", "--pages", "1", "--as", "human:owner"}).exit_code, 0);
        Mcp mcp(dir + "/root", dir + "/config", QStringLiteral("ai:test"), {{QStringLiteral("GENKO_ARGV_LOG"), log}});
        mcp.initialize();
        Json asked = Mcp::text(mcp.call("generation_request", Json::object({{"project", "b.genko"}, {"page", 1}, {"frame_id", frame}})));
        QVERIFY2(asked["ok"] == Json(true), genko::core::dump_python(asked).c_str());
        request = str(asked["request"]["id"]);
        Json imported = Mcp::text(mcp.call("import_image", Json::object({{"project", "b.genko"}, {"png_base64", genko::core::b64encode(png_bytes(40, 30))}})));
        QVERIFY2(imported["ok"] == Json(true), genko::core::dump_python(imported).c_str());
        Json taken = Mcp::text(mcp.call("take_panel_art", Json::object({{"project", "b.genko"},
                                                                               {"request_id", request},
                                                                               {"image", Json::object({{"asset", imported["asset"]}, {"origin", Json::object({{"kind", "agent"}, {"tool_id", "test"}})}})},
                                                                               {"upscale", false}})));
        QVERIFY2(taken["ok"] == Json(true), genko::core::dump_python(taken).c_str());
        for (const char* method : {"../fake", "/bin/sh", "fake; touch hacked", "fake --flag"}) {
            Json refused = Mcp::text(mcp.call("upscale", Json::object({{"project", "b.genko"}, {"page", 1}, {"frame_id", frame}, {"method", method}, {"scale", 2}})));
            QCOMPARE(refused["ok"], Json(false));
            QVERIFY(!QFile::exists(log));
        }
        Json done = Mcp::text(mcp.call("upscale", Json::object({{"project", "b.genko"}, {"page", 1}, {"frame_id", frame}, {"method", "fake"}, {"scale", 2}})));
        QVERIFY2(done["ok"] == Json(true), genko::core::dump_python(done).c_str());
        QFile file(log);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QStringList argv = QString::fromUtf8(file.readAll()).split('\n', Qt::SkipEmptyParts);
        QCOMPARE(argv.size(), 8);  // ("--", the program and its 6 arguments)
        QCOMPARE(argv[0], QStringLiteral("--"));
        QCOMPARE(argv[2], QStringLiteral("-i"));
        QVERIFY(argv[3].endsWith(QStringLiteral("/in.png")));
        QCOMPARE(argv[4], QStringLiteral("-o"));
        QVERIFY(argv[5].endsWith(QStringLiteral("/out.png")));
        QCOMPARE(argv[6], QStringLiteral("-s"));
        QCOMPARE(argv[7], QStringLiteral("2"));
        QVERIFY(!QFile::exists(dir + "/root/b.genko/hacked") && !QFile::exists(QDir::currentPath() + "/hacked"));
        // the program's own stdout ({"jsonrpc": …, "injected": true}) never reached the protocol
        for (const QByteArray& line : mcp.lines()) QVERIFY2(!line.contains("injected"), line.constData());
    }

    // The HTTP API's own lines: no route for a person's approvals and no way for an agent's token to approve, unlock,
    // let chat approvals in, change what a person pinned or a panel's regions; tokens.json read again when it changes; /v1/new never over a book; the files ops read stay inside the
    // root in for_pages too; NaN and deep JSON refused; pictures that are not, or are too big, refused; oversized and
    // malformed requests refused before they are read; v1–v3 books not written.
    void httpApi() {
        const QString dir = fresh("api");
        const QString root = dir + "/root";
        write_file(dir + "/config/tokens.json", R"({"tok-ai": {"actor": "ai:bot"}, "tok-human": {"actor": "human:owner"}})");
        write_file(dir + "/outside.png", png_bytes(8, 8));
        genko::test::copy_tree(genko::test::test_data("legacy/book-v3.genko"), root + "/old.genko");
        Server server({"serve", "--root", root, "--port", "0"}, dir + "/config");
        QVERIFY(server.port != 0);
        const std::uint16_t port = server.port;
        const auto post = [port](const std::string& path, const std::string& body, const std::string& token = "tok-ai") {
            return http(port, request_text("POST", path, port, token, body));
        };
        QCOMPARE(post("/v1/new", R"({"dest": "b.genko", "pages": 2})").status, 200);
        const auto book = tree(root + "/b.genko");
        // /v1/new never writes over a book (Python's does)
        const Response again = post("/v1/new", R"({"dest": "b.genko", "title": "over"})");
        QCOMPARE(again.status, 409);
        QCOMPARE(again.json()["code"], Json("exists"));
        QVERIFY(tree(root + "/b.genko") == book);
        // the files an op reads stay inside the root, in for_pages too (Python looks at the top level only)
        const Response nested = post("/v1/apply", R"({"path": "b.genko", "ops": [{"op": "for_pages", "pages": "all", "ops": [{"op": "put_raster", "path": ")" +
                                                      (dir + "/outside.png").toStdString() + R"("}]}]})");
        QCOMPARE(nested.status, 403);
        QVERIFY(str(nested.json()["error"]).starts_with("path outside --root"));
        // a list of points named path (add_line) is not a file: it is left alone
        const Response drawn = post("/v1/apply", R"({"path": "b.genko", "ops": [{"op": "add_line", "page": 1, "text": "x", "path": [[10, 10], [30, 10], [30, 30]]}]})");
        QCOMPARE(drawn.status, 200);
        // an agent's token cannot approve, let chat approvals in, unlock a person's page, change what a person pinned or
        // a panel's regions (a person's), or undo a person's change
        const Response inspected = http(port, request_text("GET", "/v1/inspect?path=b.genko", port, "tok-ai"));
        QCOMPARE(inspected.status, 200);
        const std::string frame = str(inspected.json()["pages"][0]["leaves"][0]["id"]);
        const auto panel = [&frame](const Json& fields) {
            Json op = Json::object({{"op", "set_panel"}, {"page", 1}, {"frame_id", frame}});
            for (const auto& [key, value] : fields.items()) op[key] = value;
            return op;
        };
        const auto apply = [](const Json& ops) { return genko::core::dump_python(Json::object({{"path", "b.genko"}, {"ops", ops}})); };
        QCOMPARE(post("/v1/apply",
                      apply(Json::array({Json::object({{"op", "lock_page"}, {"page", 2}}),
                                         panel(Json::object({{"set", Json::object({{"shot", "CU"}})}, {"pin", Json::array({"shot"})}}))})),
                      "tok-human")
                     .status,
                 200);
        const auto locked = tree(root + "/b.genko");
        const std::vector<std::pair<Json, std::string>> refusals = {
            {Json::object({{"op", "name_ok"}, {"page", 1}}), "name_ok needs a person (actor ai:bot cannot approve)"},
            {Json::object({{"op", "approve"}, {"gate", "name"}, {"page", 1}}), "approve needs a person (actor ai:bot cannot approve)"},
            {Json::object({{"op", "allow_chat_approval"}, {"on", true}}), "only a person can let approvals come through a chat"},
            {Json::object({{"op", "unlock_page"}, {"page", 2}}), "page 2 locked by human:owner; ai:bot cannot unlock it"},
            {Json::object({{"op", "set_note"}, {"page", 2}, {"note", "x"}}), "page 2 locked by human:owner"},
            {panel(Json::object({{"set", Json::object({{"shot", "LS"}})}})), "shot is pinned by a person"},
            {panel(Json::object({{"unpin", Json::array({"shot"})}})), "only a person can unpin"},
            {panel(Json::object({{"set", Json::object({{"regions", Json::array()}})}})), "only a person can set a panel's regions with set_panel"},
            {Json::object({{"op", "undo"}}), "nothing to undo"},  // (a request's book has no Undo of its own: a person's change stays)
        };
        for (const auto& [op, words] : refusals) {
            const Response r = post("/v1/apply", apply(Json::array({op})));
            QVERIFY2((r.status == 400 || r.status == 409) && str(r.json()["error"]).find(words) != std::string::npos,
                     (genko::core::dump_python(op) + " → " + r.body).c_str());
        }
        QVERIFY(tree(root + "/b.genko") == locked);
        // no route reaches a person's approvals
        for (const char* path : {"/v1/approve", "/v1/studio/approve", "/v1/export_book", "/v1/chat_approval"}) QCOMPARE(post(path, "{}").status, 404);
        // NaN, infinities and integers past 64 bits are refused; so is JSON nested deeper than 1000 levels
        const Response nan = post("/v1/apply", R"({"path": "b.genko", "ops": [{"op": "set_note", "page": 1, "note": NaN}]})");
        QCOMPARE(nan.status, 400);
        QVERIFY(str(nan.json()["error"]).starts_with("ops must not hold NaN or Infinity"));
        QCOMPARE(post("/v1/new", R"({"dest": "n.genko", "pages": 123456789012345678901234567890})").status, 400);
        QCOMPARE(post("/v1/new", std::string(2000, '[') + std::string(2000, ']')).json()["error"], Json("invalid JSON body"));
        QVERIFY(tree(root + "/b.genko") == locked);
        // a v1–v3 book is not written (genko migrate first)
        const auto old = tree(root + "/old.genko");
        const Response legacy = post("/v1/apply", R"({"path": "old.genko", "ops": [{"op": "set_note", "page": 1, "note": "x"}]})");
        QCOMPARE(legacy.status, 409);
        QCOMPARE(legacy.json()["code"], Json("needs_migration"));
        QCOMPARE(http(port, request_text("POST", "/v1/assets?path=old.genko", port, "tok-ai", png_bytes(4, 4), "image/png")).status, 409);
        QVERIFY(tree(root + "/old.genko") == old);
        // pictures: a zip, bytes that are not a picture, a header that says 200000 × 200000
        for (const std::string& bytes : {std::string("PK\x03\x04zipzipzip", 13), std::string("not a picture"), huge_png()}) {
            const Response r = http(port, request_text("POST", "/v1/assets?path=b.genko", port, "tok-ai", bytes, "image/png"));
            QCOMPARE(r.status, 400);
        }
        // requests refused before they are read: a body past 64 MiB, a bad Content-Length, headers too big or too many
        QCOMPARE(http(port, "POST /v1/new HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                                "\r\nAuthorization: Bearer tok-ai\r\nContent-Type: application/json\r\nContent-Length: 67108865\r\n\r\n")
                     .status,
                 413);
        QCOMPARE(http(port, "POST /v1/new HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\nContent-Length: -5\r\n\r\n").status, 400);
        QCOMPARE(http(port, "GET /health HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\nX-Big: " + std::string(70000, 'a') + "\r\n\r\n").status, 431);
        std::string many = "GET /health HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\n";
        for (int i = 0; i < 120; ++i) many += "X-" + std::to_string(i) + ": a\r\n";
        QCOMPARE(http(port, many + "\r\n").status, 431);
        QCOMPARE(http(port, "BROKEN\r\n\r\n").status, 400);
        // tokens.json is read again when it changes: a revoked token stops at once, a new one works at once
        QCOMPARE(http(port, request_text("GET", "/schema", port, "tok-ai")).status, 200);
        write_file(dir + "/config/tokens.json", R"({"tok-new": {"actor": "ai:bot"}, "tok-human": {"actor": "human:owner"}})");
        QCOMPARE(http(port, request_text("GET", "/schema", port, "tok-ai")).status, 401);
        QCOMPARE(http(port, request_text("GET", "/schema", port, "tok-new")).status, 200);
        QCOMPARE(server.stop(), 0);
    }

    // `genko token`: add (owner-only file), list (8 characters of each), revoke by prefix; actors other than human: /
    // ai: refused.
    void tokens() {
        const QString dir = fresh("tokens");
        QProcessEnvironment env;
        env.insert(QStringLiteral("GENKO_CONFIG_DIR"), dir + "/config");
        Json added = genko::test::one_line(genko::test::run_genko({"token", "add", "--actor", "ai:bot"}, env).out);
        QVERIFY2(added.is_object() && added["ok"] == Json(true), genko::core::dump_python(added).c_str());
        QCOMPARE(str(added["token"]).size(), std::size_t(43));
        QCOMPARE(QFile::permissions(dir + "/config/tokens.json") & (QFileDevice::ReadGroup | QFileDevice::ReadOther | QFileDevice::WriteOther), QFileDevice::Permissions());
        const genko::test::Run bad = genko::test::run_genko({"token", "add", "--actor", "root"}, env);
        QCOMPARE(bad.exit_code, 1);
        Json refused = genko::test::one_line(bad.out);
        QVERIFY2(refused.is_object() && refused["ok"] == Json(false) && refused["error"] == Json("actor must be human:<name> or ai:<name>"),
                 genko::core::dump_python(refused).c_str());
        Json listed = genko::test::one_line(genko::test::run_genko({"token", "list"}, env).out);
        QVERIFY2(listed.is_object() && listed["tokens"].is_array() && listed["tokens"].size() == 1, genko::core::dump_python(listed).c_str());
        QCOMPARE(listed["tokens"][0]["id"], Json(str(added["token"]).substr(0, 8)));
        Json none = genko::test::one_line(genko::test::run_genko({"token", "revoke"}, env).out);
        QCOMPARE(none, Json::object({{"ok", false}, {"error", "--id is required"}}));
        Json revoked = genko::test::one_line(genko::test::run_genko({"token", "revoke", "--id", QString::fromStdString(str(added["token"]).substr(0, 8))}, env).out);
        QVERIFY2(revoked.is_object() && revoked["revoked"] == Json(1), genko::core::dump_python(revoked).c_str());
    }

    // The MCP HTTP endpoint: Host and Origin checked, bodies past 4 MiB refused before they are read.
    void mcpHttpChecks() {
        const QString dir = fresh("mcphttp");
        Server server({"mcp", "--root", dir + "/root", "--agent", "ai:test", "--transport", "streamable-http", "--port", "0"}, dir + "/config");
        QVERIFY(server.port != 0);
        const std::uint16_t port = server.port;
        const std::string init = R"({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "t", "version": "1"}}})";
        const auto post = [port](const std::string& host, const std::string& extra, const std::string& body) {
            return http(port, "POST /mcp HTTP/1.1\r\nHost: " + host + "\r\nContent-Type: application/json\r\nAccept: application/json, text/event-stream\r\n" + extra +
                                  "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body);
        };
        const std::string host = "127.0.0.1:" + std::to_string(port);
        QCOMPARE(post(host, "", init).status, 200);
        QCOMPARE(post("evil.example:" + std::to_string(port), "", init).status, 421);
        QCOMPARE(post("127.0.0.1", "", init).status, 421);  // (the SDK's patterns need a port)
        QCOMPARE(post(host, "Origin: http://evil.example\r\n", init).status, 403);
        QCOMPARE(post(host, "Origin: http://localhost:5173\r\n", init).status, 200);
        QCOMPARE(http(port, "POST /mcp HTTP/1.1\r\nHost: " + host + "\r\nContent-Type: application/json\r\nContent-Length: 4194305\r\n\r\n").status, 413);
        // bodies that are not JSON, or not JSON-RPC: the SDK's status and code, this build's own words (SPEC COMP-01a)
        const Response not_json = post(host, "", "{not json");
        QCOMPARE(not_json.status, 400);
        QCOMPARE(not_json.json()["error"], Json::object({{"code", -32700}, {"message", "Parse error: the body is not JSON"}}));
        const Response not_rpc = post(host, "", R"({"jsonrpc": "2.0"})");
        QCOMPARE(not_rpc.status, 400);
        QCOMPARE(not_rpc.json()["error"], Json::object({{"code", -32602}, {"message", "Validation error: the body is not a JSON-RPC message"}}));
        QCOMPARE(server.stop(), 0);
    }

    // At most 32 connections at once from one address and 64 in all: the next one is answered 503 (what it sends is
    // read before the connection closes, so the answer is not lost to a reset); once they end, requests are served again.
    void connectionsAtOnce() {
        const QString dir = fresh("connections");
        write_file(dir + "/config/tokens.json", R"({"tok-ai": {"actor": "ai:bot"}})");
        Server server({"serve", "--root", dir + "/root", "--port", "0"}, dir + "/config");
        QVERIFY(server.port != 0);
        const std::uint16_t port = server.port;
        // (each has sent its head and a byte of the body it said it would send, and waits)
        const std::string pending = "POST /v1/new HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                                    "\r\nAuthorization: Bearer tok-ai\r\nContent-Type: application/json\r\nContent-Length: 100\r\n\r\n{";
        const std::string health = request_text("GET", "/health", port, "");
        const auto status_from = [&](const QString& source, int wanted) {
            Response r;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
            while (r.status != wanted && std::chrono::steady_clock::now() < deadline) {
                r = http(port, health, 60000, source);
                if (r.status != wanted) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            return r.status;
        };
        std::vector<std::unique_ptr<QTcpSocket>> held;
        for (int i = 0; i < 32; ++i) {
            held.push_back(held_from(QStringLiteral("127.0.0.1"), port, pending));
            QVERIFY(held.back() != nullptr);
        }
        QCOMPARE(status_from(QStringLiteral("127.0.0.1"), 503), 503);
        // the refusal is read whole even while the client goes on sending
        const std::string busy = answer_while_sending(port, "POST /v1/new HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                                                                "\r\nContent-Type: application/json\r\nContent-Length: 8388608\r\n\r\n",
                                                      8 * 1024 * 1024);
        QVERIFY2(busy.starts_with("HTTP/1.1 503 ") && busy.ends_with("Service Unavailable"), busy.substr(0, 200).c_str());
        QCOMPARE(http(port, health, 60000, QStringLiteral("127.0.0.2")).status, 200);  // (another address)
        for (int i = 0; i < 32; ++i) {
            held.push_back(held_from(QStringLiteral("127.0.0.2"), port, pending));
            QVERIFY(held.back() != nullptr);
        }
        QCOMPARE(status_from(QStringLiteral("127.0.0.3"), 503), 503);  // (64 in all)
        for (auto& socket : held) socket->abort();
        held.clear();
        QCOMPARE(status_from(QStringLiteral("127.0.0.1"), 200), 200);
        QCOMPARE(server.stop(), 0);
    }

    // Requests refused early (a body past 64 MiB said, headers past 64 KiB) are answered and the answer is read whole
    // while the client goes on sending (the server stops writing, then reads what still comes before it closes).
    void earlyRefusalsAreRead() {
        const QString dir = fresh("early");
        write_file(dir + "/config/tokens.json", R"({"tok-ai": {"actor": "ai:bot"}})");
        Server server({"serve", "--root", dir + "/root", "--port", "0"}, dir + "/config");
        QVERIFY(server.port != 0);
        const std::uint16_t port = server.port;
        const std::string big = answer_while_sending(port, "POST /v1/new HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                                                               "\r\nAuthorization: Bearer tok-ai\r\nContent-Type: application/json\r\nContent-Length: 67108865\r\n\r\n",
                                                     8 * 1024 * 1024);
        QVERIFY2(big.starts_with("HTTP/1.1 413 ") && big.ends_with(R"({"ok": false, "error": "body too large"})"), big.substr(0, 300).c_str());
        const std::string headers = answer_while_sending(port, "GET /health HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\nX-Big: ",
                                                         8 * 1024 * 1024);
        QVERIFY2(headers.starts_with("HTTP/1.1 431 "), headers.substr(0, 300).c_str());
        QCOMPARE(server.stop(), 0);
    }

    // A client that sends its head slowly is cut off once the time for a head (10 s) has passed, whatever it still
    // sends; a server told to stop does not wait for requests still arriving.
    void slowClients() {
        const QString dir = fresh("slow");
        write_file(dir + "/config/tokens.json", R"({"tok-ai": {"actor": "ai:bot"}})");
        Server server({"serve", "--root", dir + "/root", "--port", "0"}, dir + "/config");
        QVERIFY(server.port != 0);
        const std::uint16_t port = server.port;
        QTcpSocket socket;
        socket.connectToHost(QStringLiteral("127.0.0.1"), port);
        QVERIFY(socket.waitForConnected(30000));
        socket.write("GET /health HTTP/1.1\r\n");
        const auto started = std::chrono::steady_clock::now();
        bool closed = false;
        while (std::chrono::steady_clock::now() - started < std::chrono::seconds(40)) {
            socket.write("X-Slow: a\r\n");
            socket.waitForBytesWritten(1000);
            if (socket.waitForReadyRead(500)) socket.readAll();
            if (socket.state() != QAbstractSocket::ConnectedState) {
                closed = true;
                break;
            }
        }
        const auto took = std::chrono::steady_clock::now() - started;
        QVERIFY2(closed && took < std::chrono::seconds(25), std::to_string(std::chrono::duration_cast<std::chrono::seconds>(took).count()).c_str());
        // a request half sent when the server is told to stop: it stops at once
        auto pending = held_from(QStringLiteral("127.0.0.1"), port, "GET /health HTTP/1.1\r\n");
        QVERIFY(pending != nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const auto stopping = std::chrono::steady_clock::now();
        QCOMPARE(server.stop(), 0);
        QVERIFY(std::chrono::steady_clock::now() - stopping < std::chrono::seconds(10));
    }

    // `genko serve`'s guard reads the head only: a request it refuses (Host, Origin, Content-Type, token, in Python's
    // order) is answered before its body is read; an allowed Origin gets its CORS headers on those answers too.
    void apiGuardBeforeTheBody() {
        const QString dir = fresh("guard");
        write_file(dir + "/config/tokens.json", R"({"tok-ai": {"actor": "ai:bot"}})");
        Server server({"serve", "--root", dir + "/root", "--port", "0", "--allow-origin", "http://ok.example"}, dir + "/config");
        QVERIFY(server.port != 0);
        const std::uint16_t port = server.port;
        const std::string host = "127.0.0.1:" + std::to_string(port);
        // (each says a body of 60 MiB follows, and sends none of it)
        const auto head = [](const std::string& host_header, const std::string& extra) {
            return "POST /v1/new HTTP/1.1\r\nHost: " + host_header + "\r\n" + extra + "Content-Length: 62914560\r\n\r\n";
        };
        const auto started = std::chrono::steady_clock::now();
        QCOMPARE(http(port, head("evil.example", "Content-Type: application/json\r\nAuthorization: Bearer tok-ai\r\n"), 20000).status, 421);
        QCOMPARE(http(port, head(host, "Origin: http://evil.example\r\nContent-Type: application/json\r\nAuthorization: Bearer tok-ai\r\n"), 20000).status, 403);
        Response type = http(port, head(host, "Origin: http://ok.example\r\nContent-Type: text/plain\r\nAuthorization: Bearer tok-ai\r\n"), 20000);
        QCOMPARE(type.status, 415);
        QCOMPARE(type.headers["access-control-allow-origin"], std::string("http://ok.example"));
        Response token = http(port, head(host, "Origin: http://ok.example\r\nContent-Type: application/json\r\nAuthorization: Bearer nope\r\n"), 20000);
        QCOMPARE(token.status, 401);
        QCOMPARE(token.headers["access-control-allow-origin"], std::string("http://ok.example"));
        QCOMPARE(token.headers["vary"], std::string("Origin"));
        QVERIFY(std::chrono::steady_clock::now() - started < std::chrono::seconds(20));  // (the server waits 30 s for a body)
        // with a token the body is waited for: nothing is answered before it comes
        auto waiting = held_from(QStringLiteral("127.0.0.1"), port, head(host, "Content-Type: application/json\r\nAuthorization: Bearer tok-ai\r\n"));
        QVERIFY(waiting != nullptr);
        QVERIFY(!waiting->waitForReadyRead(2000));
        waiting->abort();
        QCOMPARE(server.stop(), 0);
    }

    // import_pages reads another book: through /v1/apply it must be a book inside --root (its project.json there), in
    // for_pages too; a link out of the root and a folder that is not a book are refused, and the book stays as it was.
    void importPagesStayInTheRoot() {
        const QString dir = fresh("import");
        const QString root = dir + "/root";
        write_file(dir + "/config/tokens.json", R"({"tok-ai": {"actor": "ai:bot"}})");
        QCOMPARE(genko::test::run_genko({"new", dir + "/outside.genko", "--pages", "1"}).exit_code, 0);
        QCOMPARE(genko::test::run_genko({"new", root + "/other.genko", "--pages", "1"}).exit_code, 0);
        QVERIFY(QDir().mkpath(root + "/notabook.genko/studio"));
        QVERIFY(QFile::link(dir + "/outside.genko", root + "/link.genko"));
        Server server({"serve", "--root", root, "--port", "0"}, dir + "/config");
        QVERIFY(server.port != 0);
        const std::uint16_t port = server.port;
        const auto post = [port](const std::string& path, const Json& body) {
            return http(port, request_text("POST", path, port, "tok-ai", genko::core::dump_python(body)));
        };
        QCOMPARE(post("/v1/new", Json::object({{"dest", "b.genko"}, {"pages", 1}})).status, 200);
        const auto before = tree(root + "/b.genko");
        const auto import = [](const std::string& from) { return Json::object({{"op", "import_pages"}, {"from", from}}); };
        const std::string outside = (dir + "/outside.genko").toStdString();
        const std::vector<std::pair<Json, int>> refused = {
            {Json::array({import(outside)}), 403},
            {Json::array({import("../outside.genko")}), 403},
            {Json::array({import("link.genko")}), 403},
            {Json::array({import("notabook.genko")}), 400},
            {Json::array({import("nothing.genko")}), 400},
            {Json::array({Json::object({{"op", "for_pages"}, {"pages", "all"}, {"ops", Json::array({import(outside)})}})}), 403},
        };
        for (const auto& [ops, status] : refused) {
            const Response r = post("/v1/apply", Json::object({{"path", "b.genko"}, {"ops", ops}}));
            QVERIFY2(r.status == status, (genko::core::dump_python(ops) + " → " + r.body).c_str());
        }
        QVERIFY(tree(root + "/b.genko") == before);
        // a book inside the root, named from the root as every path of a request is
        const Response taken = post("/v1/apply", Json::object({{"path", "b.genko"}, {"ops", Json::array({import("other.genko")})}}));
        QVERIFY2(taken.status == 200, taken.body.c_str());
        QCOMPARE(taken.json()["snapshot"]["pages"].size(), std::size_t(2));
        QCOMPARE(server.stop(), 0);
    }

    // Integers past the signed 64 bits (2^63 and up, below -2^63), as JSON numbers or as text, are refused alike:
    // in an MCP tool's arguments (an int or a float parameter, inside the ops) and in the HTTP API's body.
    void integersPast63Bits() {
        const QString dir = fresh("past63");
        {
            Mcp mcp(dir + "/root", dir + "/config");
            mcp.initialize();
            QVERIFY(!make_book(mcp, "b.genko").empty());
            const auto before = book_tree(dir + "/root/b.genko");
            int id = 700;
            const std::vector<std::pair<std::string, std::string>> cases = {
                {R"("render", "arguments": {"project": "b.genko", "page": 9223372036854775808})", "/page"},
                {R"("render", "arguments": {"project": "b.genko", "page": "9223372036854775808"})", "/page"},
                {R"("render", "arguments": {"project": "b.genko", "page": 18446744073709551615})", "/page"},
                {R"("render", "arguments": {"project": "b.genko", "page": "-9223372036854775809"})", "/page"},
                {R"("adopt", "arguments": {"project": "b.genko", "candidate_id": "c", "scale": 9223372036854775808})", "/scale"},
                {R"("adopt", "arguments": {"project": "b.genko", "candidate_id": "c", "scale": "9223372036854775808"})", "/scale"},
                {R"("check", "arguments": {"project": "b.genko", "pages": [1, 9223372036854775808]})", "/pages/1"},
                {R"("apply_ops", "arguments": {"project": "b.genko", "ops": [{"op": "set_note", "page": 9223372036854775808, "note": "x"}], "commit": true})",
                 "/ops/0/page"},
                {"\"apply_ops\", \"arguments\": " +
                     genko::core::dump_python(Json::object({{"project", "b.genko"},
                                                            {"ops", "[{\"op\": \"set_note\", \"page\": 9223372036854775808, \"note\": \"x\"}]"},
                                                            {"commit", true}})),
                 "/ops/0/page"},  // (the ops as a text, read as JSON)
            };
            for (const auto& [call, where] : cases) {
                ++id;
                mcp.send_raw(R"({"jsonrpc": "2.0", "id": )" + std::to_string(id) + R"(, "method": "tools/call", "params": {"name": )" + call + "}}");
                Json text = Mcp::text(mcp.answer(id, 120000));
                QVERIFY2(text.is_object() && text["code"] == Json("unsupported_number") && str(text["detail"]).ends_with(" at " + where),
                         (call + " → " + genko::core::dump_python(text)).c_str());
            }
            // 2^63 - 1 is a number this build carries: the tool answers as it would for any page
            mcp.send_raw(R"({"jsonrpc": "2.0", "id": 799, "method": "tools/call", "params": {"name": "render", "arguments": {"project": "b.genko", "page": 9223372036854775807}}})");
            Json last = Mcp::text(mcp.answer(799, 120000));
            QVERIFY2(last.is_object() && last["code"] != Json("unsupported_number"), genko::core::dump_python(last).c_str());
            mcp.lines();
            QVERIFY(book_tree(dir + "/root/b.genko") == before);
        }
        write_file(dir + "/config/tokens.json", R"({"tok-ai": {"actor": "ai:bot"}})");
        Server server({"serve", "--root", dir + "/root", "--port", "0"}, dir + "/config");
        QVERIFY(server.port != 0);
        const std::uint16_t port = server.port;
        const auto post = [port](const std::string& path, const std::string& body) { return http(port, request_text("POST", path, port, "tok-ai", body)); };
        for (const char* body : {R"({"dest": "n.genko", "pages": 9223372036854775808})", R"({"dest": "n.genko", "pages": "9223372036854775808"})",
                                 R"({"dest": "n.genko", "episode": -9223372036854775809})"}) {
            const Response r = post("/v1/new", body);
            QCOMPARE(r.status, 400);
            QVERIFY2(str(r.json()["error"]).starts_with("the body must not hold NaN, Infinity or an integer past 64 bits (at /"), r.body.c_str());
        }
        QVERIFY(!QFile::exists(dir + "/root/n.genko/project.json"));
        const Response ops = post("/v1/apply", R"({"path": "b.genko", "ops": [{"op": "set_note", "page": 9223372036854775808, "note": "x"}]})");
        QCOMPARE(ops.status, 400);
        QVERIFY2(str(ops.json()["error"]).starts_with("ops must not hold an integer past 64 bits (at /0/page)"), ops.body.c_str());
        const Response revision = post("/v1/apply", R"({"path": "b.genko", "ops": [], "expect_revision": "9223372036854775808"})");
        QCOMPARE(revision.status, 400);
        QCOMPARE(server.stop(), 0);
    }

    // tokens.json is read again whenever its bytes change, even when its size and its time stay the same.
    void tokensReadAgain() {
        const QString dir = fresh("reread");
        const QString file = dir + "/config/tokens.json";
        write_file(file, R"({"tok-aaa": {"actor": "ai:bot"}})");
        // (its time in whole seconds, so that the same time can be set again exactly)
        const QDateTime stamp = QDateTime::fromSecsSinceEpoch(QDateTime::currentSecsSinceEpoch() - 60);
        {
            QFile f(file);
            QVERIFY(f.open(QIODevice::ReadWrite));
            QVERIFY(f.setFileTime(stamp, QFileDevice::FileModificationTime));
        }
        Server server({"serve", "--root", dir + "/root", "--port", "0"}, dir + "/config");
        QVERIFY(server.port != 0);
        const std::uint16_t port = server.port;
        QCOMPARE(http(port, request_text("GET", "/schema", port, "tok-aaa")).status, 200);
        {
            QFile f(file);
            QVERIFY(f.open(QIODevice::ReadWrite));  // (in place: the same file, the same size)
            f.write(R"({"tok-bbb": {"actor": "ai:bot"}})");
            f.flush();
            QVERIFY(f.setFileTime(stamp, QFileDevice::FileModificationTime));
        }
        QCOMPARE(QFileInfo(file).lastModified(), stamp);
        QCOMPARE(http(port, request_text("GET", "/schema", port, "tok-aaa")).status, 401);
        QCOMPARE(http(port, request_text("GET", "/schema", port, "tok-bbb")).status, 200);
        // a token is compared whole: a part of one, or one with more after it, is not it
        QCOMPARE(http(port, request_text("GET", "/schema", port, "tok-bb")).status, 401);
        QCOMPARE(http(port, request_text("GET", "/schema", port, "tok-bbbb")).status, 401);
        QCOMPARE(server.stop(), 0);
    }

    // A request's files: its folder too must be inside --root (a link from studio/requests/ out of it serves nothing).
    void requestFolderInTheRoot() {
        const QString dir = fresh("requests");
        write_file(dir + "/config/tokens.json", R"({"tok-ai": {"actor": "ai:bot"}})");
        write_file(dir + "/elsewhere/request.json", "{}");
        write_file(dir + "/elsewhere/secret.png", png_bytes(4, 4));
        Server server({"serve", "--root", dir + "/root", "--port", "0"}, dir + "/config");
        QVERIFY(server.port != 0);
        const std::uint16_t port = server.port;
        QCOMPARE(http(port, request_text("POST", "/v1/new", port, "tok-ai", R"({"dest": "b.genko", "pages": 1})")).status, 200);
        QVERIFY(QDir().mkpath(dir + "/root/b.genko/studio/requests"));
        QVERIFY(QFile::link(dir + "/elsewhere", dir + "/root/b.genko/studio/requests/rq_x"));
        const Response r = http(port, request_text("GET", "/v1/requests/rq_x/files/secret.png?path=b.genko", port, "tok-ai"));
        QCOMPARE(r.status, 404);
        QVERIFY(r.body.find("\x89PNG") == std::string::npos);
        QCOMPARE(server.stop(), 0);
    }

    // A --root that cannot be resolved (a link to itself): `genko serve` says so and ends with 1.
    void rootThatCannotBeResolved() {
        const QString dir = fresh("loop");
        QVERIFY(QFile::link(dir + "/loop", dir + "/loop"));
        QProcessEnvironment env;
        env.insert(QStringLiteral("GENKO_CONFIG_DIR"), dir + "/config");
        const genko::test::Run run = genko::test::run_genko({"serve", "--root", dir + "/loop", "--port", "0"}, env);
        QCOMPARE(run.exit_code, 1);
        Json said = genko::test::one_line(run.out);
        QVERIFY2(said.is_object() && said["ok"] == Json(false) && str(said["error"]).find("--root") != std::string::npos, run.out.constData());
    }

    // Requests run on a bounded number of threads: 300 slow calls at once over stdio, and 300 slow 2026-era calls over
    // HTTP whose clients go away at once, keep the server's threads bounded; all the stdio calls are answered.
    void threadsAreBounded() {
#ifndef Q_OS_LINUX
        QSKIP("counts threads through /proc");
#endif
        const QString dir = fresh("threads");
        {
            Mcp setup(dir + "/root", dir + "/config");
            setup.initialize();
            QVERIFY(!make_book(setup, "b.genko").empty());
        }
        running_job(dir + "/root/b.genko", "job_slow");
        {
            Mcp mcp(dir + "/root", dir + "/config");
            mcp.initialize();
            for (int i = 0; i < 300; ++i) {
                mcp.send(Json::object({{"jsonrpc", "2.0"}, {"id", 1000 + i}, {"method", "tools/call"},
                                       {"params", Json::object({{"name", "export_status"},
                                                                {"arguments", Json::object({{"project", "b.genko"}, {"job", "job_slow"}, {"wait_s", 2}})}})}}));
            }
            int most = 0;
            for (int i = 0; i < 300; ++i) {
                most = std::max(most, threads_of(mcp.pid()));
                QVERIFY2(!mcp.answer(1000 + i).is_null(), std::to_string(i).c_str());
            }
            qInfo("stdio: at most %d threads", most);
            QVERIFY2(most <= 96, std::to_string(most).c_str());
        }
        Server server({"mcp", "--root", dir + "/root", "--agent", "ai:test", "--transport", "streamable-http", "--port", "0"}, dir + "/config");
        QVERIFY(server.port != 0);
        int most = 0;
        for (int i = 0; i < 300; ++i) {
            QTcpSocket socket;
            socket.connectToHost(QStringLiteral("127.0.0.1"), server.port);
            QVERIFY(socket.waitForConnected(30000));
            const std::string raw = modern_post(server.port, 2000 + i, "export_status", Json::object({{"project", "b.genko"}, {"job", "job_slow"}, {"wait_s", 3}}));
            socket.write(raw.data(), static_cast<qint64>(raw.size()));
            socket.waitForBytesWritten(30000);
            socket.abort();  // (gone at once)
            most = std::max(most, threads_of(server.pid()));
        }
        qInfo("2026-era HTTP: at most %d threads", most);
        QVERIFY2(most <= 160, std::to_string(most).c_str());
        // (served again once the connections of the clients that left have ended)
        Response ping;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
        while (ping.status != 200 && std::chrono::steady_clock::now() < deadline) {
            ping = http(server.port, modern_post(server.port, 3000, "projects", Json::object()), 120000);
            if (ping.status != 200) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        QCOMPARE(ping.status, 200);
        QCOMPARE(server.stop(), 0);
    }

    // Chunked bodies on the MCP endpoint, read as uvicorn reads them but stricter: Transfer-Encoding "chunked" alone
    // (not with a Content-Length), sizes of at most 16 hex digits, each chunk's data followed by CRLF; otherwise 400
    // "Invalid HTTP request received."; a size past the 4 MiB the endpoint takes, 413.
    void chunkedBodiesAreStrict() {
        const QString dir = fresh("chunked");
        Server server({"mcp", "--root", dir + "/root", "--agent", "ai:test", "--transport", "streamable-http", "--port", "0"}, dir + "/config");
        QVERIFY(server.port != 0);
        const std::uint16_t port = server.port;
        const std::string init = R"({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "t", "version": "1"}}})";
        const auto head = [port](const std::string& te) {
            return "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                   "\r\nContent-Type: application/json\r\nAccept: application/json, text/event-stream\r\n" + te + "\r\n";
        };
        const auto hex = [](std::size_t n) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%zx", n);
            return std::string(buf);
        };
        const std::string whole = hex(init.size()) + "\r\n" + init + "\r\n0\r\n\r\n";
        const Response good = http(port, head("Transfer-Encoding: chunked\r\n") + hex(10) + ";ext=1\r\n" + init.substr(0, 10) + "\r\n" + hex(init.size() - 10) + "\r\n" +
                                             init.substr(10) + "\r\n0\r\nX-Trailer: a\r\n\r\n");
        QCOMPARE(good.status, 200);
        const std::vector<std::string> bad = {
            head("Transfer-Encoding: gzip\r\n") + whole,
            head("Transfer-Encoding: chunked, gzip\r\n") + whole,
            head("Transfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n") + whole,
            head("Transfer-Encoding: chunked\r\nContent-Length: " + std::to_string(whole.size()) + "\r\n") + whole,
            head("Transfer-Encoding: chunked\r\n") + "zz\r\n" + init + "\r\n0\r\n\r\n",
            head("Transfer-Encoding: chunked\r\n") + "0000000000000000" + hex(init.size()) + "\r\n" + init + "\r\n0\r\n\r\n",
            head("Transfer-Encoding: chunked\r\n") + " " + hex(init.size()) + "\r\n" + init + "\r\n0\r\n\r\n",
            head("Transfer-Encoding: chunked\r\n") + hex(init.size()) + "\r\n" + init + "XX0\r\n\r\n",
        };
        for (const std::string& raw : bad) {
            const Response r = http(port, raw, 60000);
            QVERIFY2(r.status == 400 && r.body == "Invalid HTTP request received.", (raw.substr(raw.find("\r\n\r\n") - 60, 120) + " → " + std::to_string(r.status) + " " + r.body).c_str());
        }
        const Response huge = http(port, head("Transfer-Encoding: chunked\r\n") + "ffffffffffffffff\r\nabc", 60000);
        QCOMPARE(huge.status, 413);
        QCOMPARE(server.stop(), 0);
    }
};

QTEST_GUILESS_MAIN(TestMcpApiE2e)
#include "test_mcp_api_e2e.moc"
