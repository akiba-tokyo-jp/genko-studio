// 制御チャンネル (PC5 J-g, SPEC.md PROD-01「制御チャンネル」): the local socket, its connections and their limits, the first
// line, the methods answered (service/control.hpp's table), the pen and the person's input held while it draws.

#include "app/control_channel.hpp"

#include <QApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QLocalServer>
#include <QLocalSocket>
#include <QStringList>
#include <QTimer>

#include <algorithm>

#include "app/documents.hpp"
#include "app/main_window.hpp"
#include "core/error.hpp"
#include "core/prefs_table.hpp"
#include "service/control.hpp"
#include "service/tokens.hpp"
#include "service/wording.hpp"

namespace genko::app::control {

namespace rules = service::control;
using core::Json;

struct Server::Connection {
    QLocalSocket* socket = nullptr;
    QByteArray buffer;
    bool authed = false;
    std::string actor;
    QTimer* idle = nullptr;    // no whole line for 30 s: closed
    QTimer* writing = nullptr; // an answer not taken in 30 s: closed
    bool closing = false;
};

Server& Server::instance() {
    static Server* server = new Server;  // (lives as long as the app: never destroyed after QApplication)
    return *server;
}

Server::Server() : QObject(QCoreApplication::instance()) {
    pen_timer_ = new QTimer(this);
    pen_timer_->setSingleShot(true);
    pen_timer_->setInterval(rules::kTimeoutMs);
    connect(pen_timer_, &QTimer::timeout, this, [this] { let_go_pen(QStringLiteral("制御チャンネルのペンが 30 秒止まったので、線を描かずに戻しました")); });
}

Server::~Server() = default;

void Server::follow_settings() {
    bool on = false;
    try {
        on = core::prefs::value("integrations.controlChannel") == Json(true);
    } catch (const std::exception&) {
        on = false;
    }
    if (on && !listening()) {
        start();
    } else if (!on && listening()) {
        stop();
    }
}

bool Server::start() {
    if (listening()) return true;
    if (server_ == nullptr) {
        server_ = new QLocalServer(this);
        server_->setSocketOptions(QLocalServer::UserAccessOption);  // (the person's own: no one else connects)
        server_->setMaxPendingConnections(rules::kMaxConnections + 1);
        connect(server_, &QLocalServer::newConnection, this, &Server::take);
    }
    const QString name = rules::server_name();
    if (!server_->listen(name)) {
        // (a name left by a Genko that did not end cleanly: taken over only when no one answers on it)
        QLocalSocket probe;
        probe.connectToServer(name);
        const bool answered = probe.waitForConnected(300);
        probe.abort();
        if (answered || !QLocalServer::removeServer(name) || !server_->listen(name)) {
            tell_windows();
            return false;
        }
    }
    tell_windows();
    return true;
}

void Server::stop() {
    let_go_pen(QStringLiteral("制御チャンネルを切ったので、線を描かずに戻しました"));
    while (!connections_.empty()) drop(connections_.front().get());
    if (server_ != nullptr) server_->close();
    tell_windows();
}

bool Server::listening() const { return server_ != nullptr && server_->isListening(); }

int Server::named() const {
    return static_cast<int>(std::count_if(connections_.begin(), connections_.end(), [](const auto& c) { return c->authed && !c->closing; }));
}

int Server::connections() const { return static_cast<int>(connections_.size()); }

QString Server::name() const { return listening() ? server_->fullServerName() : QString(); }

QString Server::drawing() const { return pen_owner_ != nullptr ? pen_actor_ : QString(); }

QString Server::words() const {
    if (!listening()) return {};
    if (!drawing().isEmpty()) return QStringLiteral("制御チャンネルが描いています（%1）").arg(drawing());
    QStringList who;
    for (const auto& c : connections_) {
        if (c->authed && !c->closing && !who.contains(QString::fromStdString(c->actor))) who << QString::fromStdString(c->actor);
    }
    if (who.isEmpty()) return QStringLiteral("制御チャンネル: 待っています");
    return QStringLiteral("制御チャンネル: %1 がつないでいます").arg(who.join(QStringLiteral("・")));
}

void Server::tell_windows() {
    const QString text = words();
    for (MainWindow* window : documents::windows()) window->control_state(text, !drawing().isEmpty());
    emit stateChanged();
}

void Server::take() {
    while (server_ != nullptr && server_->hasPendingConnections()) {
        QLocalSocket* socket = server_->nextPendingConnection();
        if (socket == nullptr) break;
        if (static_cast<int>(connections_.size()) >= rules::kMaxConnections) {  // (the fifth: closed before a word)
            socket->abort();
            socket->deleteLater();
            continue;
        }
        auto c = std::make_unique<Connection>();
        c->socket = socket;
        c->idle = new QTimer(socket);
        c->idle->setSingleShot(true);
        c->idle->setInterval(rules::kTimeoutMs);
        c->writing = new QTimer(socket);
        c->writing->setSingleShot(true);
        c->writing->setInterval(rules::kTimeoutMs);
        Connection* raw = c.get();
        connect(c->idle, &QTimer::timeout, this, [this, raw] { drop(raw); });
        connect(c->writing, &QTimer::timeout, this, [this, raw] { drop(raw); });
        connect(socket, &QLocalSocket::readyRead, this, [this, raw] { read(raw); });
        connect(socket, &QLocalSocket::bytesWritten, this, [raw](qint64) {
            if (raw->socket->bytesToWrite() == 0) raw->writing->stop();
        });
        connect(socket, &QLocalSocket::disconnected, this, [this, raw] { drop(raw); });
        c->idle->start();
        connections_.push_back(std::move(c));
        if (socket->bytesAvailable() > 0) read(raw);
    }
}

void Server::drop(Connection* c) {
    const auto it = std::find_if(connections_.begin(), connections_.end(), [c](const auto& own) { return own.get() == c; });
    if (it == connections_.end() || c->closing) return;
    c->closing = true;
    if (pen_owner_ == c) let_go_pen(QStringLiteral("制御チャンネルが切れたので、線を描かずに戻しました"));
    c->idle->stop();
    c->writing->stop();
    QLocalSocket* socket = c->socket;
    socket->disconnect(this);
    socket->abort();
    socket->deleteLater();
    dead_.push_back(std::move(*it));
    connections_.erase(it);
    QTimer::singleShot(0, this, [this] { dead_.clear(); });
    tell_windows();
}

void Server::reply(Connection* c, const Json& answer) {
    if (c->closing) return;
    c->socket->write(QByteArray::fromStdString(answer.dump(-1, ' ', false, Json::error_handler_t::replace)) + '\n');
    if (c->socket->bytesToWrite() > 0 && !c->writing->isActive()) c->writing->start();
}

void Server::read(Connection* c) {
    if (c->closing) return;
    c->buffer += c->socket->readAll();
    for (;;) {
        const qsizetype end = c->buffer.indexOf('\n');
        if (end < 0) {
            if (static_cast<std::size_t>(c->buffer.size()) >= rules::kMaxLine) {  // (a line past 1 MiB: refused, closed)
                if (c->authed) reply(c, rules::refusal(Json(), "line_too_long", "1 行は 1 MiB まで"));
                c->socket->flush();
                drop(c);
            }
            return;
        }
        if (static_cast<std::size_t>(end) + 1 > rules::kMaxLine) {
            if (c->authed) reply(c, rules::refusal(Json(), "line_too_long", "1 行は 1 MiB まで"));
            c->socket->flush();
            drop(c);
            return;
        }
        const QByteArray line = c->buffer.left(end);
        c->buffer.remove(0, end + 1);
        c->idle->start();
        if (!c->authed) {  // (rule 1: the first line names a token, or nothing is answered)
            std::map<std::string, std::string> tokens;
            try {
                tokens = service::tokens::load();
            } catch (const std::exception&) {
                tokens.clear();
            }
            const auto actor = rules::authenticate(std::string_view(line.constData(), static_cast<std::size_t>(line.size())), tokens);
            if (!actor) {
                drop(c);
                return;
            }
            c->authed = true;
            c->actor = *actor;
            Json id;
            try {
                id = Json::parse(line.toStdString()).value("id", Json());
            } catch (const std::exception&) {
            }
            reply(c, rules::answer(id, Json{{"authenticated", true}, {"actor", *actor}}));
            tell_windows();
            continue;
        }
        Json request;
        try {
            request = Json::parse(line.toStdString());
        } catch (const std::exception&) {
            reply(c, rules::refusal(Json(), "bad_request", "1 行に JSON のオブジェクトを 1 つ"));
            continue;
        }
        if (!request.is_object()) {
            reply(c, rules::refusal(Json(), "bad_request", "1 行に JSON のオブジェクトを 1 つ"));
            continue;
        }
        const Json id = request.value("id", Json());
        const Json params = request.value("params", Json::object());
        const std::string method = request.value("method", Json()).is_string() ? request["method"].get<std::string>() : std::string();
        Json answer;
        if (method == "ui.pointer") {
            MainWindow* w = window();
            answer = w == nullptr ? rules::refusal(id, "no_window", "開いている窓がありません") : pen(c, id, params, w, c->actor);
        } else {
            answer = handle(request, c->actor);
        }
        if (c->closing) return;
        reply(c, answer);
    }
}

MainWindow* Server::window() const {
    if (auto* active = qobject_cast<MainWindow*>(QApplication::activeWindow()); active != nullptr) return active;
    for (MainWindow* open : documents::windows()) {
        if (open->isVisible()) return open;
    }
    const auto all = documents::windows();
    return all.empty() ? nullptr : all.front();
}

Json Server::handle(const Json& request, const std::string& actor, MainWindow* w) {
    const Json id = request.value("id", Json());
    const std::string method = request.value("method", Json()).is_string() ? request["method"].get<std::string>() : std::string();
    Json params = request.value("params", Json::object());
    if (params.is_null()) params = Json::object();
    if (!params.is_object()) return rules::refusal(id, "bad_params", "params はオブジェクト");
    const rules::Method* known = rules::method(method);
    if (known == nullptr || method == "auth") return rules::refusal(id, "unknown_method", "この method はありません: " + method + "（methods で一覧）");
    if (method == "methods") return rules::answer(id, rules::methods_json());
    if (method == "engine.commands") return rules::answer(id, rules::engine_commands());
    if (w == nullptr) w = window();
    if (w == nullptr) return rules::refusal(id, "no_window", "開いている窓がありません");
    try {
        if (method == "ui.inspect") {
            Json out = w->control_inspect();
            out["actor"] = actor;
            out["channel"] = Json{{"connections", named()}, {"drawing", drawing().toStdString()}, {"input_held", holding_input_}};
            return rules::answer(id, std::move(out));
        }
        if (method == "ui.screenshot") {
            const bool whole = params.value("window", Json(false)) == Json(true);
            if (whole && !rules::human_token(actor)) return rules::refusal(id, "not_allowed", "窓全体の絵は human: のトークンだけ（ai: はキャンバスだけ）");
            const Json side = params.value("max_side", Json(0));
            if (!side.is_number_integer() || side.get<std::int64_t>() < 0 || side.get<std::int64_t>() > 16384) {
                return rules::refusal(id, "bad_params", "max_side は 0〜16384 の整数（0: そのまま）");
            }
            const QByteArray png = w->control_screenshot(whole, static_cast<int>(side.get<std::int64_t>()));
            if (png.isEmpty()) return rules::refusal(id, "failed", "絵を撮れませんでした");
            return rules::answer(id, Json{{"mime", "image/png"}, {"base64", png.toBase64().toStdString()}, {"part", whole ? "window" : "canvas"}});
        }
        const bool pen_here = pen_owner_ != nullptr && pen_window_ == w;
        if ((method == "ui.set" || method == "ui.menu.invoke") && pen_here) {
            return rules::refusal(id, "busy", "制御チャンネルのペンが描いている間は表示を変えられません");
        }
        if (method == "ui.set") return rules::answer(id, w->control_set(params));
        if (method == "ui.menu.invoke") {
            const Json name = params.value("id", Json());
            if (!name.is_string()) return rules::refusal(id, "bad_params", "id は命令の属性名（ui.inspect の menus の name）");
            return rules::answer(id, w->control_menu(QString::fromStdString(name.get<std::string>())));
        }
        if (method == "engine.execute") {
            const Json ops = params.value("ops", Json());
            if (const auto refused = rules::refused_ops(ops)) {
                Json out = rules::refusal(id, refused->code, refused->message);
                out["error"]["where"] = refused->where;
                return out;
            }
            if (pen_owner_ != nullptr && pen_window_ == w) return rules::refusal(id, "busy", "制御チャンネルのペンが描いている間は当てられません");
            return rules::answer(id, w->control_execute(ops, actor));
        }
        if (method == "ui.pointer") return rules::refusal(id, "bad_request", "ui.pointer はつないだ口から");
    } catch (const core::Error& error) {
        return rules::refusal(id, error.code().empty() ? std::string("refused") : error.code(),
                              genko::wording::error(QString::fromUtf8(error.what())).toStdString());
    } catch (const std::exception& error) {
        return rules::refusal(id, "failed", genko::wording::error(QString::fromUtf8(error.what())).toStdString());
    }
    return rules::refusal(id, "unknown_method", "この method はありません: " + method);
}

Json Server::pen(Connection* c, const Json& id, const Json& params, MainWindow* w, const std::string& actor) {
    if (pen_owner_ != nullptr && pen_owner_ != c) return rules::refusal(id, "busy", "ほかの接続のペンが描いています");
    if (pen_owner_ == c && pen_window_ != w) let_go_pen(QStringLiteral("前の窓が替わったので、線を描かずに戻しました"));
    if (!params.is_object() || !params.value("events", Json()).is_array()) return rules::refusal(id, "bad_params", "events は [{kind, x, y, pressure?}] の配列");
    bool holding = false;
    Json out;
    try {
        out = w->control_pen(params["events"], actor, holding);
    } catch (const core::Error& error) {
        if (!holding) let_go_pen(QString());
        return rules::refusal(id, error.code().empty() ? std::string("refused") : error.code(),
                              genko::wording::error(QString::fromUtf8(error.what())).toStdString());
    }
    if (holding) {
        const bool fresh = pen_owner_ == nullptr;
        pen_owner_ = c;
        pen_window_ = w;
        pen_actor_ = QString::fromStdString(actor);
        pen_timer_->start();
        if (fresh) {
            hold_input(true);
            tell_windows();
        }
    } else if (pen_owner_ == c) {
        pen_owner_ = nullptr;
        pen_window_.clear();
        pen_timer_->stop();
        hold_input(false);
        tell_windows();
    }
    return rules::answer(id, std::move(out));
}

void Server::let_go_pen(const QString& why) {
    if (pen_owner_ == nullptr) return;
    pen_owner_ = nullptr;
    pen_timer_->stop();
    if (pen_window_ != nullptr) {
        pen_window_->control_pen_cancel();
        if (!why.isEmpty()) pen_window_->flash(why, 5000);
    }
    pen_window_.clear();
    hold_input(false);
    tell_windows();
}

void Server::hold_input(bool on) {
    if (on == holding_input_) return;
    holding_input_ = on;
    if (on) {
        qApp->installEventFilter(this);
    } else {
        qApp->removeEventFilter(this);
    }
}

bool Server::eventFilter(QObject* watched, QEvent* event) {
    if (!holding_input_) return QObject::eventFilter(watched, event);
    switch (event->type()) {
    case QEvent::KeyPress:
        if (static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {  // (the person's way out: the line thrown away)
            QTimer::singleShot(0, this, [this] { let_go_pen(QStringLiteral("Esc で制御チャンネルのペンを止めました（線は描いていません）")); });
        }
        return true;
    case QEvent::KeyRelease:
    case QEvent::ShortcutOverride:
    case QEvent::Shortcut:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseMove:
    case QEvent::Wheel:
    case QEvent::TabletPress:
    case QEvent::TabletMove:
    case QEvent::TabletRelease:
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    case QEvent::TouchCancel:
    case QEvent::ContextMenu:
    case QEvent::NativeGesture:
    case QEvent::InputMethod:
        return true;  // (the person's input held while the channel's pen draws: the status bar says so)
    default:
        return QObject::eventFilter(watched, event);
    }
}

}  // namespace genko::app::control
