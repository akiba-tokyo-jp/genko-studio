#pragma once

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>

#include <memory>
#include <string>
#include <vector>

#include "core/json.hpp"

class QLocalServer;
class QLocalSocket;
class QTimer;

// 制御チャンネル (PC5 J-g, SPEC.md PROD-01「制御チャンネル」, automation:control-channel; the user's decision 6): the running
// app answers a program of this computer's person over a local socket of the person's own, one JSON line a request,
// one a reply — never a TCP port, off unless the person turns it on (環境設定 → 連携). The rules are service/control.hpp's
// (the same tokens, the same gate as MCP's apply_ops, nothing that reaches an approval, a consent or a setting, no input of
// the person's but the pen on the canvas as the token's ops); what it does in a window is MainWindow's control_*
// (app/main_window_control.cpp). While the channel's pen draws, the person's input to the app is held (an event filter:
// Esc lets go) and the status bar says who draws; the pen's end, its connection gone or 30 s without a word give it back.

namespace genko::app {
class MainWindow;
}

namespace genko::app::control {

class Server : public QObject {
    Q_OBJECT

public:
    static Server& instance();

    // Listening when 環境設定 says so (integrations.controlChannel), else not (the connections closed). Each window's
    // preferences::apply_all calls it.
    void follow_settings();
    // Listen now (false: the name could not be had); stop (every connection closed, a pen let go).
    bool start();
    void stop();
    bool listening() const;
    // The connections that named a token (the status bar's count), and every connection.
    int named() const;
    int connections() const;
    QString name() const;
    // The pen drawing now: its token's actor (empty: none).
    QString drawing() const;
    // The status bar's words (empty: off).
    QString words() const;
    // Tests: a connection's line answered as the server answers it, as `actor` (the auth already made), and the window it
    // works in (none: the active MainWindow, else the first visible one).
    core::Json handle(const core::Json& request, const std::string& actor, MainWindow* window = nullptr);

signals:
    void stateChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Connection;
    Server();
    void take();
    void read(Connection* c);
    void reply(Connection* c, const core::Json& answer);
    void drop(Connection* c);
    MainWindow* window() const;
    core::Json pen(Connection* c, const core::Json& id, const core::Json& params, MainWindow* window, const std::string& actor);
    void hold_input(bool on);
    void let_go_pen(const QString& why);
    void tell_windows();

    QLocalServer* server_ = nullptr;
    std::vector<std::unique_ptr<Connection>> connections_;
    std::vector<std::unique_ptr<Connection>> dead_;  // (dropped, freed once the event loop is back: never under a handler)
    // the pen: whose it is, in which window, the time it waits for its next words
    Connection* pen_owner_ = nullptr;
    QPointer<MainWindow> pen_window_;
    QString pen_actor_;
    QTimer* pen_timer_ = nullptr;
    bool holding_input_ = false;
};

}  // namespace genko::app::control
