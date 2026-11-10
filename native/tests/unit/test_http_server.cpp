// api/http_server: the small HTTP/1.1 server of `genko serve` and `genko mcp --transport streamable-http`, driven over
// a loopback socket with its own handler: a handler that throws is answered 500; the head is admitted (or refused)
// before the body is read, and a refused request's body is never read; every answer after the head gets what decorate
// adds (a 413 too); a chunked body with an extension and a trailer is read whole.

#include <QtTest>

#include <QTcpSocket>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

#include "api/http_server.hpp"

namespace http = genko::api::http;

namespace {

// A server on a free port, served on a thread of its own until the end of the scope.
class Running {
public:
    Running(http::Handler handler, http::Limits limits) : server_(std::move(handler), std::move(limits)) {
        if (!server_.listen("127.0.0.1", 0)) port = server_.port();
        thread_ = std::thread([this] { server_.run(stop_); });
    }
    ~Running() {
        stop_.store(true);
        thread_.join();
    }
    std::uint16_t port = 0;

private:
    http::Server server_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

// Everything the server answers to `raw` (it closes the connection after its answer).
std::string exchange(std::uint16_t port, const std::string& raw, int timeout_ms = 30000) {
    QTcpSocket socket;
    socket.connectToHost(QStringLiteral("127.0.0.1"), port);
    if (!socket.waitForConnected(10000)) return {};
    socket.write(raw.data(), static_cast<qint64>(raw.size()));
    socket.waitForBytesWritten(10000);
    std::string data;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (socket.waitForReadyRead(200)) {
            data += socket.readAll().toStdString();
        } else if (socket.state() != QAbstractSocket::ConnectedState) {
            data += socket.readAll().toStdString();
            break;
        }
    }
    return data;
}

}  // namespace

class TestHttpServer : public QObject {
    Q_OBJECT

private slots:
    void aHandlerThatThrowsIsAnswered500() {
        Running server([](const http::Request&, http::Responder&) { throw std::runtime_error("broken"); }, http::Limits{});
        QVERIFY(server.port != 0);
        const std::string answer = exchange(server.port, "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
        QVERIFY2(answer.starts_with("HTTP/1.1 500 Internal Server Error\r\n") && answer.ends_with("\r\n\r\nInternal Server Error"), answer.c_str());
    }

    void theHeadIsAdmittedBeforeTheBody() {
        std::atomic<int> handled{0};
        http::Limits limits;
        limits.admit = [](http::Request& head, http::Responder& r) {
            if (head.header("authorization") != std::optional<std::string>("yes")) {
                r.send(401, {}, "no");
                return false;
            }
            head.principal = "someone";
            return true;
        };
        Running server(
            [&handled](const http::Request& request, http::Responder& r) {
                ++handled;
                r.send(200, {}, request.principal + ":" + request.body);
            },
            limits);
        // refused from its head: answered at once though the body it said would come never does
        const auto started = std::chrono::steady_clock::now();
        const std::string refused = exchange(server.port, "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1000\r\n\r\n", 20000);
        QVERIFY2(refused.starts_with("HTTP/1.1 401 ") && refused.ends_with("\r\n\r\nno"), refused.c_str());
        QVERIFY(std::chrono::steady_clock::now() - started < std::chrono::seconds(15));
        QCOMPARE(handled.load(), 0);
        const std::string taken = exchange(server.port, "POST / HTTP/1.1\r\nHost: x\r\nAuthorization: yes\r\nContent-Length: 4\r\n\r\nbody");
        QVERIFY2(taken.starts_with("HTTP/1.1 200 ") && taken.ends_with("\r\n\r\nsomeone:body"), taken.c_str());
        QCOMPARE(handled.load(), 1);
    }

    void everyAnswerAfterTheHeadIsDecorated() {
        http::Limits limits;
        limits.max_body = 10;
        limits.decorate = [](const http::Request& head, int status, http::Headers& headers) {
            headers.emplace_back("X-Seen", head.method + " " + std::to_string(status));
        };
        Running server([](const http::Request&, http::Responder& r) { r.send(200, {}, "ok"); }, limits);
        const std::string ok = exchange(server.port, "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
        QVERIFY2(ok.find("\r\nX-Seen: GET 200\r\n") != std::string::npos, ok.c_str());
        const std::string large = exchange(server.port, "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 11\r\n\r\n");
        QVERIFY2(large.starts_with("HTTP/1.1 413 ") && large.find("\r\nX-Seen: POST 413\r\n") != std::string::npos, large.c_str());
    }

    void aChunkedBodyIsReadWhole() {
        http::Limits limits;
        limits.chunked_bodies = true;
        Running server([](const http::Request& request, http::Responder& r) { r.send(200, {}, "[" + request.body + "]"); }, limits);
        const std::string answer =
            exchange(server.port, "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: Chunked\r\n\r\n5;name=value\r\nhello\r\n1\r\n \r\n5\r\nworld\r\n0\r\nX-Trailer: a\r\n\r\n");
        QVERIFY2(answer.starts_with("HTTP/1.1 200 ") && answer.ends_with("\r\n\r\n[hello world]"), answer.c_str());
    }
};

QTEST_GUILESS_MAIN(TestHttpServer)
#include "test_http_server.moc"
