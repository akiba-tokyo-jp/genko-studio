#include "app/mcp_probe.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>

#include "core/error.hpp"
#include "core/json.hpp"
#include "core/pyconv.hpp"

namespace genko::app {

using core::Json;

namespace {

constexpr qsizetype kErrorsKept = 64 * 1024;  // (the server's stderr kept for the reason shown)
constexpr qsizetype kLineLongest = 16 * 1024 * 1024;

QString clipped(const QString& text, int most) { return text.size() > most ? text.left(most) + QStringLiteral("…") : text; }

}  // namespace

McpProbe::McpProbe(QObject* parent) : QObject(parent) {
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, [this] {
        done(false, QStringLiteral("%1 秒たっても応答がありませんでした").arg((clock_.elapsed() + 500) / 1000), stderr_tail());
    });
}

McpProbe::~McpProbe() {
    if (process_ != nullptr) {
        process_->disconnect(this);
        process_->kill();
        process_->waitForFinished(2000);
        delete process_;
    }
}

void McpProbe::start(const QStringList& command, int timeout_ms) {
    cancel();
    result_ = ai_link::Probe{};
    result_.command = command;
    buffer_.clear();
    errors_.clear();
    sent_.clear();
    stage_ = 0;
    if (command.isEmpty() || command.front().isEmpty()) {
        done(false, QStringLiteral("起動するコマンドがありません"));
        return;
    }
    auto* process = new QProcess(this);
    process_ = process;
    process->setProgram(command.front());
    process->setArguments(command.mid(1));
    connect(process, &QProcess::started, this, [this] {
        stage_ = 1;
        send("initialize", 1);
    });
    connect(process, &QProcess::readyReadStandardOutput, this, &McpProbe::read_out);
    connect(process, &QProcess::readyReadStandardError, this, [this] {
        if (process_ == nullptr) return;
        errors_ += process_->readAllStandardError();
        if (errors_.size() > kErrorsKept) errors_ = errors_.right(kErrorsKept);
    });
    connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && process_ != nullptr) {
            done(false, QStringLiteral("起動できませんでした: %1（%2）").arg(process_->program(), process_->errorString()));
        }
    });
    connect(process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        if (process_ == nullptr) return;
        errors_ += process_->readAllStandardError();
        read_out();
        if (process_ == nullptr) return;  // (its last answer finished the test)
        const QString how = status == QProcess::CrashExit ? QStringLiteral("異常終了") : QStringLiteral("終了コード %1").arg(code);
        done(false, QStringLiteral("応答の前に終わりました（%1）").arg(how), stderr_tail());
    });
    clock_.start();
    timer_.start(timeout_ms);
    process->start(QIODevice::ReadWrite);
}

void McpProbe::cancel() {
    timer_.stop();
    if (process_ == nullptr) return;
    QProcess* process = process_;
    process_ = nullptr;
    process->disconnect(this);
    process->closeWriteChannel();
    process->kill();
    connect(process, &QProcess::finished, process, &QObject::deleteLater);
    if (process->state() == QProcess::NotRunning) process->deleteLater();
}

void McpProbe::send(const char* method, int id, bool notification) {
    if (process_ == nullptr) return;
    Json message = {{"jsonrpc", "2.0"}, {"method", method}};
    if (!notification) message["id"] = id;
    if (std::string_view(method) == "initialize") {
        message["params"] = {{"protocolVersion", "2025-06-18"},
                             {"capabilities", Json::object()},
                             {"clientInfo", {{"name", "genko-app"}, {"version", QCoreApplication::applicationVersion().toStdString()}}}};
    } else if (!notification) {
        message["params"] = Json::object();
    }
    sent_ << QString::fromLatin1(method);
    process_->write(QByteArray::fromStdString(core::dump_python(message) + "\n"));
}

void McpProbe::read_out() {
    if (process_ == nullptr) return;
    buffer_ += process_->readAllStandardOutput();
    if (buffer_.size() > kLineLongest) {
        done(false, QStringLiteral("応答が大きすぎます"), stderr_tail());
        return;
    }
    while (process_ != nullptr) {
        const qsizetype end = buffer_.indexOf('\n');
        if (end < 0) return;
        const QByteArray line = buffer_.left(end).trimmed();
        buffer_.remove(0, end + 1);
        if (line.isEmpty()) continue;
        Json message;
        try {
            message = core::parse_python_json(line.toStdString());
        } catch (const core::Error&) {
            message = Json();
        }
        if (!message.is_object()) {
            done(false, QStringLiteral("MCP ではない出力がありました: %1").arg(clipped(QString::fromUtf8(line), 120)), stderr_tail());
            return;
        }
        if (message.contains("method")) {  // (a request or a notice from the server: nothing it asks is done here)
            if (message.contains("id")) {
                const Json reply = {{"jsonrpc", "2.0"}, {"id", message["id"]}, {"error", {{"code", -32601}, {"message", "not supported by the test"}}}};
                process_->write(QByteArray::fromStdString(core::dump_python(reply) + "\n"));
            }
            continue;
        }
        const Json id = message.value("id", Json());
        const auto error_words = [&]() {
            const Json error = message["error"];
            return QString::fromStdString(error.is_object() && error.contains("message") ? core::py_str(error["message"]) : core::py_str(error));
        };
        if (id == Json(1) && stage_ == 1) {
            if (message.contains("error")) {
                done(false, QStringLiteral("初期化を断られました: %1").arg(clipped(error_words(), 300)), stderr_tail());
                return;
            }
            const Json result = message.value("result", Json::object());
            const Json info = result.is_object() ? result.value("serverInfo", Json::object()) : Json::object();
            if (info.is_object()) {
                result_.server = QStringLiteral("%1 %2")
                                     .arg(QString::fromStdString(info.contains("name") ? core::py_str(info["name"]) : std::string()),
                                          QString::fromStdString(info.contains("version") ? core::py_str(info["version"]) : std::string()))
                                     .trimmed();
            }
            if (result.is_object() && result.contains("protocolVersion")) result_.protocol = QString::fromStdString(core::py_str(result["protocolVersion"]));
            send("notifications/initialized", 0, true);
            stage_ = 2;
            send("tools/list", 2);
            continue;
        }
        if (id == Json(2) && stage_ == 2) {
            if (message.contains("error")) {
                done(false, QStringLiteral("道具の一覧を返しませんでした: %1").arg(clipped(error_words(), 300)), stderr_tail());
                return;
            }
            const Json result = message.value("result", Json::object());
            const Json tools = result.is_object() ? result.value("tools", Json()) : Json();
            if (!tools.is_array()) {
                done(false, QStringLiteral("道具の一覧の形が違います"), stderr_tail());
                return;
            }
            result_.tools = static_cast<int>(tools.size());
            done(true, QStringLiteral("Genko の MCP サーバーが応答しました（%1、道具 %2 個）")
                           .arg(result_.server.isEmpty() ? QStringLiteral("名前なし") : result_.server)
                           .arg(result_.tools));
            return;
        }
    }
}

QString McpProbe::stderr_tail() const {
    const QString text = QString::fromUtf8(errors_).trimmed();
    if (text.isEmpty()) return {};
    QStringList lines = text.split(QLatin1Char('\n'));
    if (lines.size() > 6) lines = lines.mid(lines.size() - 6);
    return clipped(lines.join(QLatin1Char('\n')), 1200);
}

void McpProbe::done(bool ok, const QString& words, const QString& detail) {
    timer_.stop();
    result_.ok = ok;
    result_.at = QDateTime::currentDateTime();
    result_.words = words;
    result_.detail = detail;
    stage_ = 3;
    cancel();  // (the server is closed: its stdin first, then it is stopped)
    const ai_link::Probe result = result_;
    ai_link::set_last_probe(result);
    emit finished(result);
}

}  // namespace genko::app
