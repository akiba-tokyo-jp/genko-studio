#include "app/links.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#endif

#include "core/json.hpp"

namespace genko::app::links {

namespace {

constexpr const char* kSocketBuffer = "genko_buffer";

}  // namespace

bool is_link(const QString& text) { return text.toLower().startsWith(QStringLiteral("genko:")); }

QString server_name() {
    // getpass.getuser(): LOGNAME, USER, LNAME, USERNAME (then the system's name for the user)
    QString user;
    for (const char* key : {"LOGNAME", "USER", "LNAME", "USERNAME"}) {
        user = qEnvironmentVariable(key);
        if (!user.isEmpty()) break;
    }
    if (user.isEmpty()) user = QStringLiteral("user");
    QString kept;
    for (const QChar c : user) {
        if (c.isLetterOrNumber()) kept += c;
    }
    return QStringLiteral("genko-studio-links-%1").arg(kept.isEmpty() ? QStringLiteral("user") : kept);
}

bool send(const QString& link, int wait_ms) {
    QLocalSocket socket;
    socket.connectToServer(server_name());
    if (!socket.waitForConnected(wait_ms)) return false;
    allow_front();
    socket.write(link.toUtf8() + "\n");
    socket.flush();
    socket.waitForBytesWritten(wait_ms);
    // the open Genko answers once it has the link: going before that can lose it (Windows' pipes)
    socket.waitForReadyRead(std::max(wait_ms, 2000));
    socket.disconnectFromServer();
    return true;
}

void allow_front() {
#ifdef _WIN32
    AllowSetForegroundWindow(ASFW_ANY);
#endif
}

void come_forward(QWidget* window) {
    if (window->isMinimized()) window->showNormal();
    window->raise();
    window->activateWindow();
#ifdef _WIN32
    SetForegroundWindow(reinterpret_cast<HWND>(window->winId()));
#endif
}

Listener::Listener(QObject* parent) : QObject(parent) {
    server_ = new QLocalServer(this);
    if (!server_->listen(server_name())) {
        QLocalServer::removeServer(server_name());  // (a name left by a Genko that did not close cleanly)
        server_->listen(server_name());
    }
    connect(server_, &QLocalServer::newConnection, this, &Listener::take);
    // (Windows' pipes do not always say a later start has come: look now and then too)
    poll_ = new QTimer(this);
    poll_->setInterval(250);
    connect(poll_, &QTimer::timeout, this, &Listener::look);
    poll_->start();
}

bool Listener::listening() const { return server_->isListening(); }

void Listener::look() {
    if (server_->isListening()) server_->waitForNewConnection(0);  // (a waiting start is taken in through newConnection)
    if (server_->hasPendingConnections()) take();
}

void Listener::take() {
    while (server_->hasPendingConnections()) {
        QLocalSocket* socket = server_->nextPendingConnection();
        socket->setProperty(kSocketBuffer, QByteArray());
        connect(socket, &QLocalSocket::readyRead, this, [this, socket] { read(socket); });
        connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
            read(socket, true);
            socket->deleteLater();
        });
        // (on Windows the later start may have written and gone before this runs: read what is there now)
        if (socket->bytesAvailable() > 0) read(socket);
    }
}

void Listener::read(QLocalSocket* socket, bool final) {
    QByteArray data = socket->property(kSocketBuffer).toByteArray() + socket->readAll();
    QList<QByteArray> lines = data.split('\n');
    QByteArray rest = lines.takeLast();
    if (final) {
        lines << rest;
        rest.clear();
    }
    socket->setProperty(kSocketBuffer, rest);
    for (const QByteArray& raw : lines) {
        const QString text = QString::fromUtf8(raw).trimmed();
        if (!is_link(text)) continue;
        emit received(text);
        if (socket->state() == QLocalSocket::ConnectedState) {
            socket->write("ok\n");  // (the later start may go now)
            socket->flush();
        }
    }
}

QStringList command() { return {QCoreApplication::applicationFilePath(), QStringLiteral("app")}; }

QString desktop_argument(const QString& text) {
    QString quoted;
    for (const QChar c : text) {
        if (c == QLatin1Char('"') || c == QLatin1Char('`') || c == QLatin1Char('$') || c == QLatin1Char('\\')) quoted += QLatin1Char('\\');
        if (c == QLatin1Char('%')) quoted += QLatin1Char('%');
        quoted += c;
    }
    // (the string value's own escape of "\\", applied before the quoting: a backslash is four)
    quoted.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
    return QLatin1Char('"') + quoted + QLatin1Char('"');
}

std::pair<bool, QString> register_links() {
#if defined(_WIN32)
    QStringList parts;
    for (const QString& part : command()) parts << QStringLiteral("\"%1\"").arg(QDir::toNativeSeparators(part));
    const QString line = parts.join(QLatin1Char(' ')) + QStringLiteral(" \"%1\"");
    QSettings key(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\genko"), QSettings::NativeFormat);
    key.setValue(QStringLiteral("Default"), QStringLiteral("URL:Genko Studio"));
    key.setValue(QStringLiteral("URL Protocol"), QString());
    key.setValue(QStringLiteral("shell/open/FriendlyAppName"), QStringLiteral("Genko Studio"));  // (the browser's question)
    key.setValue(QStringLiteral("shell/open/command/Default"), line);
    key.setValue(QStringLiteral("Application/ApplicationName"), QStringLiteral("Genko Studio"));
    key.sync();
    if (key.status() != QSettings::NoError) return {false, QStringLiteral("登録できませんでした（レジストリに書けません）")};
    return {true, QStringLiteral("ブラウザの「この絵柄を使う」で Genko が開くようになりました")};
#elif defined(__APPLE__)
    return {false, QStringLiteral("Mac では Genko のアプリ（.app）が自分で登録します。今は Genko の「絵柄を選ぶ」から選んでください")};
#else
    QString data = qEnvironmentVariable("XDG_DATA_HOME");
    if (data.isEmpty()) data = QDir::homePath() + QStringLiteral("/.local/share");
    const QString folder = data + QStringLiteral("/applications");
    QDir().mkpath(folder);
    const QString entry = folder + QStringLiteral("/genko-links.desktop");
    QFile file(entry);
    // (each argument quoted as the Desktop Entry Specification asks: a home folder with a space in its name still
    // runs this program; Python wrote them bare)
    QStringList exec;
    for (const QString& part : command()) {
        if (part.contains(QLatin1Char('\n')) || part.contains(QLatin1Char('\r'))) {
            return {false, QStringLiteral("登録できませんでした（プログラムの場所に改行があります: %1）").arg(part)};
        }
        exec << desktop_argument(part);
    }
    const QString text = QStringLiteral("[Desktop Entry]\nType=Application\nName=Genko Studio\nNoDisplay=true\nExec=%1 %u\nMimeType=x-scheme-handler/genko;\n")
                             .arg(exec.join(QLatin1Char(' ')));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(text.toUtf8()) < 0) {
        return {false, QStringLiteral("登録できませんでした（%1 に書けません）").arg(entry)};
    }
    file.close();
    const QString tool = QStandardPaths::findExecutable(QStringLiteral("xdg-mime"));
    if (tool.isEmpty()) return {false, QStringLiteral("%1 を作りました。xdg-mime が無いので、ブラウザへの登録は手で行ってください").arg(entry)};
    QProcess process;
    process.start(tool, {QStringLiteral("default"), QStringLiteral("genko-links.desktop"), QStringLiteral("x-scheme-handler/genko")});
    const bool done = process.waitForFinished(30000) && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
    return {done, done ? QStringLiteral("ブラウザの「この絵柄を使う」で Genko が開くようになりました") : QStringLiteral("登録できませんでした（xdg-mime が失敗）")};
#endif
}

int register_command(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (QCoreApplication::arguments().size() > 2) {
        std::fputs(core::dump_python(core::Json{{"ok", false}, {"error", "genko register-links: unrecognized arguments"}, {"code", "usage"}}).c_str(), stderr);
        std::fputs("\n", stderr);
        return 2;
    }
    const auto [done, words] = register_links();
    const std::string out = core::dump_python(core::Json{{"ok", done}, {"message", words.toStdString()}}) + "\n";
    std::fwrite(out.data(), 1, out.size(), stdout);
    return done ? 0 : 1;
}

}  // namespace genko::app::links
