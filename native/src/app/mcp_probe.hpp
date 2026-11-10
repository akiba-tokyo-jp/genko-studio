#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QStringList>
#include <QTimer>

#include "app/ai_link.hpp"

// テスト接続 (C++ only, SPEC AI-01): Genko's MCP server started the way the client's settings start it — the same
// program and arguments, over stdio —, asked who it is (initialize) and which tools it has (tools/list), then closed.
// Nothing else is sent: no tool is called, so nothing is generated, charged or written, and the book is not changed.
// A server that starts but does not answer, answers with something that is not MCP, or ends, is a failure with its
// cause in words (and its last lines on stderr); the process never outlives the test.

namespace genko::app {

class McpProbe : public QObject {
    Q_OBJECT

public:
    explicit McpProbe(QObject* parent = nullptr);
    ~McpProbe() override;

    // Start it (`command`: the program, then its arguments). A probe running is stopped first.
    void start(const QStringList& command, int timeout_ms = 30000);
    bool running() const { return process_ != nullptr; }
    // Stop it without a result.
    void cancel();
    // The methods sent so far (tests: never tools/call).
    const QStringList& sent() const { return sent_; }

signals:
    void finished(const genko::app::ai_link::Probe& result);

private:
    void send(const char* method, int id, bool notification = false);
    void read_out();
    void done(bool ok, const QString& words, const QString& detail = {});
    QString stderr_tail() const;

    QPointer<QProcess> process_;
    QTimer timer_;
    QElapsedTimer clock_;
    QByteArray buffer_;
    QByteArray errors_;
    QStringList sent_;
    ai_link::Probe result_;
    int stage_ = 0;  // 1: initialize sent, 2: tools/list sent
};

}  // namespace genko::app
