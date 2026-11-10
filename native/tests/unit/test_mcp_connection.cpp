// api/mcp_server's Connection over stdio's framing, driven directly: a request read before the handshake is answered by
// the state it was read in (the MCP SDK's session gates a request on receipt), however late the thread that serves it
// starts — the initialize after it, served inline on the reader's thread, does not let it through.

#include <QtTest>

#include <QTemporaryDir>

#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "api/mcp_server.hpp"
#include "core/json.hpp"
#include "core/paths.hpp"

namespace mcp = genko::api::mcp;
using genko::core::Json;

namespace {

mcp::Inbound message(const char* text) {
    auto decoded = mcp::decode(text);
    if (!decoded) throw std::runtime_error(std::string("not a message: ") + text);
    return std::move(*decoded);
}

}  // namespace

class TestMcpConnection : public QObject {
    Q_OBJECT
private slots:
    void aRequestBeforeTheHandshakeIsGatedAsItCame() {
        QTemporaryDir tmp;
        const mcp::Engine engine(genko::core::path_from_utf8(tmp.path().toStdString()), "ai:test");
        for (int round = 0; round < 300; ++round) {
            std::mutex mutex;
            std::map<std::string, Json> replies;  // by id
            {
                mcp::Connection connection(
                    engine,
                    [&](const Json& reply, const std::optional<Json>&) {
                        const std::lock_guard<std::mutex> lock(mutex);
                        if (reply.contains("id")) replies[reply["id"].dump()] = reply;
                    },
                    mcp::Connection::Options{});
                connection.receive(message(R"({"jsonrpc": "2.0", "id": 0, "method": "tools/list"})"));
                connection.receive(message(R"({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2025-06-18",)"
                                           R"( "capabilities": {}, "clientInfo": {"name": "test", "version": "1"}}})"));
                const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
                while (connection.busy() && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                connection.close();
            }
            QCOMPARE(replies.size(), std::size_t{2});
            QVERIFY2(replies["0"].contains("error"), qPrintable(QStringLiteral("round %1: %2").arg(round).arg(QString::fromStdString(replies["0"].dump()))));
            QVERIFY(replies["1"].contains("result"));
        }
    }
};

QTEST_GUILESS_MAIN(TestMcpConnection)
#include "test_mcp_connection.moc"
