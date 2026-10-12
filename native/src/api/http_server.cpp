#include "api/http_server.hpp"

#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace genko::api::http {

std::optional<std::string> Request::header(std::string_view name) const {
    for (const auto& [key, value] : headers) {
        if (key == name) return value;
    }
    return std::nullopt;
}

std::string_view reason_phrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 307: return "Temporary Redirect";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 406: return "Not Acceptable";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 411: return "Length Required";
        case 413: return "Request Entity Too Large";
        case 414: return "Request-URI Too Long";
        case 415: return "Unsupported Media Type";
        case 421: return "Misdirected Request";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        case 505: return "HTTP Version Not Supported";
        default: return "Unknown";
    }
}

std::string percent_decode(std::string_view text, bool plus_as_space) {
    std::string bytes;
    bytes.reserve(text.size());
    const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '%' && i + 2 < text.size()) {
            const int high = hex(text[i + 1]);
            const int low = hex(text[i + 2]);
            if (high >= 0 && low >= 0) {
                bytes += static_cast<char>(high * 16 + low);
                i += 2;
                continue;
            }
        }
        bytes += plus_as_space && c == '+' ? ' ' : c;  // (a "%" without two hex digits stays as it is)
    }
    // (UTF-8 with errors="replace")
    std::string out;
    std::size_t i = 0;
    while (i < bytes.size()) {
        const auto b = static_cast<unsigned char>(bytes[i]);
        std::size_t need = b < 0x80 ? 0 : (b >= 0xC2 && b <= 0xDF) ? 1 : (b >= 0xE0 && b <= 0xEF) ? 2 : (b >= 0xF0 && b <= 0xF4) ? 3 : 99;
        if (need == 0) {
            out += bytes[i++];
            continue;
        }
        if (need == 99) {
            out += "\xEF\xBF\xBD";
            ++i;
            continue;
        }
        unsigned char lo = 0x80;
        unsigned char hi = 0xBF;
        if (b == 0xE0) lo = 0xA0;
        if (b == 0xED) hi = 0x9F;
        if (b == 0xF0) lo = 0x90;
        if (b == 0xF4) hi = 0x8F;
        std::size_t k = 1;
        for (; k <= need && i + k < bytes.size(); ++k) {
            const auto c = static_cast<unsigned char>(bytes[i + k]);
            if (c < (k == 1 ? lo : 0x80) || c > (k == 1 ? hi : 0xBF)) break;
        }
        if (k == need + 1) {
            out.append(bytes, i, need + 1);
            i += need + 1;
        } else {
            out += "\xEF\xBF\xBD";
            i += k;
        }
    }
    return out;
}

// --- stopping ----------------------------------------------------------------------------------------------------------

std::atomic<bool>& stop_flag() {
    static std::atomic<bool> flag{false};
    return flag;
}

namespace {

#ifdef _WIN32
BOOL WINAPI console_handler(DWORD kind) {
    if (kind == CTRL_C_EVENT || kind == CTRL_BREAK_EVENT || kind == CTRL_CLOSE_EVENT) {
        stop_flag().store(true);
        return TRUE;
    }
    return FALSE;
}
#else
extern "C" void on_stop_signal(int) { stop_flag().store(true); }
#endif

}  // namespace

void install_stop_handlers() {
#ifdef _WIN32
    SetConsoleCtrlHandler(console_handler, TRUE);
#else
    struct sigaction action {};
    action.sa_handler = on_stop_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
    std::signal(SIGPIPE, SIG_IGN);  // (a client that went away: a failed write, not the end of the process)
#endif
}

// --- one connection ----------------------------------------------------------------------------------------------------

namespace {

using Clock = std::chrono::steady_clock;

class SocketResponder : public Responder {
public:
    explicit SocketResponder(QTcpSocket& socket) : socket_(socket) {}

    // The request whose head was read, and what every response to it gets added (Limits::decorate).
    void answering(const Request* head, const std::function<void(const Request&, int, Headers&)>* decorate) {
        head_ = head;
        decorate_ = decorate;
    }

    void send(int status, const std::vector<std::pair<std::string, std::string>>& headers, std::string_view body) override {
        if (started_) return;
        std::vector<std::pair<std::string, std::string>> all = headers;
        all.emplace_back("Content-Length", std::to_string(body.size()));
        if (!start(status, all)) return;
        write(body);
    }

    bool start(int status, const std::vector<std::pair<std::string, std::string>>& given) override {
        if (started_) return false;
        started_ = true;
        Headers headers = given;
        if (head_ != nullptr && decorate_ != nullptr && *decorate_) (*decorate_)(*head_, status, headers);
        std::string head = "HTTP/1.1 " + std::to_string(status) + " " + std::string(reason_phrase(status)) + "\r\n";
        bool has_connection = false;
        for (const auto& [name, value] : headers) {
            std::string lower = name;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lower == "connection") {
                has_connection = true;
                head += "Connection: close\r\n";  // (one request a connection)
                continue;
            }
            head += name + ": " + value + "\r\n";
        }
        if (!has_connection) head += "Connection: close\r\n";
        head += "\r\n";
        return write(head);
    }

    bool write(std::string_view piece) override {
        if (broken_) return false;
        std::size_t done = 0;
        while (done < piece.size()) {
            const qint64 n = socket_.write(piece.data() + done, static_cast<qint64>(piece.size() - done));
            if (n < 0) {
                broken_ = true;
                return false;
            }
            done += static_cast<std::size_t>(n);
        }
        while (socket_.bytesToWrite() > 0) {
            if (!socket_.waitForBytesWritten(30000)) {
                broken_ = true;
                return false;
            }
        }
        return true;
    }

    bool gone(int wait_ms) override {
        if (broken_ || socket_.state() != QAbstractSocket::ConnectedState) return true;
        if (socket_.bytesAvailable() > 0) socket_.readAll();
        if (socket_.waitForReadyRead(wait_ms)) {
            socket_.readAll();  // (the client sent more: it is still there)
            return false;
        }
        return socket_.state() != QAbstractSocket::ConnectedState || socket_.error() == QAbstractSocket::RemoteHostClosedError;
    }

    bool started() const override { return started_; }

private:
    QTcpSocket& socket_;
    bool started_ = false;
    bool broken_ = false;
    const Request* head_ = nullptr;
    const std::function<void(const Request&, int, Headers&)>* decorate_ = nullptr;
};

void plain(Responder& r, int status, std::string_view text) {
    r.send(status, {{"Content-Type", "text/plain; charset=utf-8"}}, text);
}

// The end of a connection: what was written is sent, the server's side is shut, and what the client still sends is
// read and dropped for up to `linger_ms` (until it closes its side): a client that is still sending a body the server
// refused gets the answer rather than a reset.
void finish(QTcpSocket& socket, int linger_ms) {
    if (socket.state() != QAbstractSocket::ConnectedState) return;
    socket.flush();
    while (socket.bytesToWrite() > 0) {
        if (!socket.waitForBytesWritten(5000)) break;
    }
    const qintptr native = socket.socketDescriptor();
#ifdef _WIN32
    if (native != -1) ::shutdown(static_cast<SOCKET>(native), SD_SEND);
#else
    if (native != -1) ::shutdown(static_cast<int>(native), SHUT_WR);
#endif
    const auto deadline = Clock::now() + std::chrono::milliseconds(linger_ms);
    while (socket.state() == QAbstractSocket::ConnectedState) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (left <= 0) break;
        if (socket.waitForReadyRead(static_cast<int>(std::min<long long>(left, 200)))) {
            socket.readAll();
        } else if (socket.state() != QAbstractSocket::ConnectedState || socket.error() == QAbstractSocket::RemoteHostClosedError) {
            break;
        }
    }
    socket.abort();
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string trimmed(const std::string& text) {
    const std::size_t a = text.find_first_not_of(" \t");
    if (a == std::string::npos) return {};
    const std::size_t b = text.find_last_not_of(" \t");
    return text.substr(a, b - a + 1);
}

// A chunk's size line ("1a" or "1a;ext"): at most 16 hex digits (no sign, no space), nothing for anything else.
std::optional<std::uint64_t> chunk_size(std::string_view line) {
    const std::size_t semicolon = line.find(';');
    const std::string_view digits = line.substr(0, semicolon);
    if (digits.empty() || digits.size() > 16) return std::nullopt;
    std::uint64_t size = 0;
    for (const char c : digits) {
        int v = -1;
        if (c >= '0' && c <= '9') v = c - '0';
        if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        if (v < 0) return std::nullopt;
        size = (size << 4) | static_cast<std::uint64_t>(v);  // (16 digits at most: no overflow)
    }
    if (semicolon != std::string_view::npos) {  // (an extension: any text without CR or LF, as h11 takes it)
        for (const char c : line.substr(semicolon)) {
            if (c == '\r' || c == '\n' || c == '\0') return std::nullopt;
        }
    }
    return size;
}

// One request read off the socket and answered.
void serve_connection(qintptr descriptor, const Handler& handler, const Limits& limits, const std::atomic<bool>& stop) {
    QTcpSocket socket;
    if (!socket.setSocketDescriptor(descriptor)) return;
    SocketResponder responder(socket);
    const auto opened = Clock::now();
    const auto request_deadline = opened + std::chrono::milliseconds(limits.request_timeout_ms);
    const auto head_deadline = std::min(request_deadline, opened + std::chrono::milliseconds(limits.header_timeout_ms));
    std::string buffer;
    // (bytes from the client: false at the end of the connection, a timeout, or the server stopping)
    const auto more = [&](Clock::time_point deadline) {
        auto idle = Clock::now() + std::chrono::milliseconds(limits.idle_timeout_ms);
        while (socket.bytesAvailable() == 0) {
            if (stop.load()) return false;
            const auto now = Clock::now();
            const auto until = std::min(deadline, idle);
            if (now >= until) return false;
            const auto slice = std::chrono::duration_cast<std::chrono::milliseconds>(until - now).count();
            if (!socket.waitForReadyRead(static_cast<int>(std::min<long long>(slice, 200))) &&
                socket.state() != QAbstractSocket::ConnectedState) {
                return false;
            }
        }
        const QByteArray got = socket.readAll();
        buffer.append(got.constData(), static_cast<std::size_t>(got.size()));
        return true;
    };
    const auto refuse = [&](int status, std::string_view text) {
        plain(responder, status, text);
        finish(socket, limits.linger_ms);
    };
    std::size_t head_end = std::string::npos;
    while ((head_end = buffer.find("\r\n\r\n")) == std::string::npos) {
        if (buffer.size() > limits.max_header_bytes) return refuse(431, "Request Header Fields Too Large");
        if (!more(head_deadline)) {
            if (!buffer.empty() && socket.state() == QAbstractSocket::ConnectedState && !stop.load()) return refuse(408, "Request Timeout");
            socket.abort();
            return;
        }
    }
    if (head_end > limits.max_header_bytes) return refuse(431, "Request Header Fields Too Large");
    Request request;
    request.peer = socket.peerAddress().toString().toStdString();
    const std::string head = buffer.substr(0, head_end);
    buffer.erase(0, head_end + 4);  // (what came after the head: the body's start)
    std::size_t line_end = head.find("\r\n");
    const std::string line = head.substr(0, line_end);
    {
        const std::size_t a = line.find(' ');
        const std::size_t b = a == std::string::npos ? std::string::npos : line.find(' ', a + 1);
        if (a == std::string::npos || b == std::string::npos || line.find(' ', b + 1) != std::string::npos) return refuse(400, limits.bad_request);
        request.method = line.substr(0, a);
        request.target = line.substr(a + 1, b - a - 1);
        request.version = line.substr(b + 1);
        if (request.version.rfind("HTTP/1.", 0) != 0 || request.target.empty()) return refuse(400, limits.bad_request);
    }
    while (line_end != std::string::npos) {
        const std::size_t start = line_end + 2;
        line_end = head.find("\r\n", start);
        const std::string field = head.substr(start, line_end == std::string::npos ? std::string::npos : line_end - start);
        if (field.empty()) continue;
        const std::size_t colon = field.find(':');
        if (colon == std::string::npos || colon == 0 || field[0] == ' ' || field[0] == '\t') return refuse(400, limits.bad_request);
        request.headers.emplace_back(lower(field.substr(0, colon)), trimmed(field.substr(colon + 1)));
        if (request.headers.size() > limits.max_headers) return refuse(431, "Too many headers");
    }
    responder.answering(&request, &limits.decorate);
    // the body's framing
    std::optional<std::size_t> length;
    std::vector<std::string> encodings;
    for (const auto& [name, value] : request.headers) {
        if (name == "transfer-encoding") encodings.push_back(lower(value));
        if (name != "content-length") continue;
        if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos || value.size() > 18) return refuse(400, limits.bad_request);
        const std::size_t n = std::stoull(value);
        if (length && *length != n) return refuse(400, limits.bad_request);
        length = n;
    }
    bool chunked = false;
    if (limits.chunked_bodies && !encodings.empty()) {
        // (Transfer-Encoding "chunked" once, and never beside a Content-Length: a body read one way only)
        if (encodings.size() != 1 || encodings.front() != "chunked" || length) return refuse(400, limits.bad_request);
        chunked = true;
    }
    const auto too_large = [&] {
        if (limits.too_large) {
            limits.too_large(responder);
        } else {
            plain(responder, 413, "Request body too large");
        }
        finish(socket, limits.linger_ms);
    };
    if (length && *length > limits.max_body) return too_large();
    if (limits.admit && !limits.admit(request, responder)) {
        if (!responder.started()) plain(responder, 500, "Internal Server Error");
        return finish(socket, limits.linger_ms);
    }
    if (length) {
        while (buffer.size() < *length) {
            if (!more(request_deadline)) {
                socket.abort();
                return;
            }
        }
        buffer.resize(*length);
        request.body = std::move(buffer);
        buffer.clear();
    } else if (chunked) {
        std::size_t at = 0;
        const auto next_line = [&](std::string& out) {  // (a CRLF-ended line, at most a header's size)
            std::size_t eol;
            while ((eol = buffer.find("\r\n", at)) == std::string::npos) {
                if (buffer.size() - at > limits.max_header_bytes || !more(request_deadline)) return false;
            }
            out = buffer.substr(at, eol - at);
            at = eol + 2;
            return true;
        };
        for (;;) {
            std::string size_line;
            if (!next_line(size_line)) {
                if (socket.state() == QAbstractSocket::ConnectedState && !stop.load()) return refuse(400, limits.bad_request);
                socket.abort();
                return;
            }
            const auto size = chunk_size(size_line);
            if (!size) return refuse(400, limits.bad_request);
            if (*size == 0) break;
            if (*size > limits.max_body - request.body.size()) return too_large();
            const std::size_t n = static_cast<std::size_t>(*size);
            while (buffer.size() - at < n + 2) {
                if (!more(request_deadline)) {
                    socket.abort();
                    return;
                }
            }
            if (buffer.compare(at + n, 2, "\r\n") != 0) return refuse(400, limits.bad_request);  // (the data's CRLF)
            request.body.append(buffer, at, n);
            at += n + 2;
            if (at > 1024 * 1024) {  // (what was read is dropped as it goes)
                buffer.erase(0, at);
                at = 0;
            }
        }
        // the trailer: header lines up to an empty one
        std::size_t trailer = 0;
        for (;;) {
            std::string field;
            if (!next_line(field)) return refuse(400, limits.bad_request);
            if (field.empty()) break;
            trailer += field.size() + 2;
            if (trailer > limits.max_header_bytes || field.find(':') == std::string::npos) return refuse(400, limits.bad_request);
        }
    }
    try {
        handler(request, responder);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "genko: a request failed: %s\n", error.what());
    } catch (...) {
        std::fprintf(stderr, "genko: a request failed\n");
    }
    if (!responder.started()) plain(responder, 500, "Internal Server Error");
    finish(socket, limits.linger_ms);
}

// The address of the client of a connection not yet wrapped in a socket ("" when it cannot be told).
std::string peer_of(qintptr descriptor) {
    sockaddr_storage address{};
#ifdef _WIN32
    int size = sizeof address;
    if (::getpeername(static_cast<SOCKET>(descriptor), reinterpret_cast<sockaddr*>(&address), &size) != 0) return {};
#else
    socklen_t size = sizeof address;
    if (::getpeername(static_cast<int>(descriptor), reinterpret_cast<sockaddr*>(&address), &size) != 0) return {};
#endif
    char text[INET6_ADDRSTRLEN] = {};
    if (address.ss_family == AF_INET) {
        ::inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(&address)->sin_addr, text, sizeof text);
    } else if (address.ss_family == AF_INET6) {
        ::inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6*>(&address)->sin6_addr, text, sizeof text);
    }
    return text;
}

}  // namespace

// --- the server --------------------------------------------------------------------------------------------------------

struct Server::Impl : public QTcpServer {
    Handler handler;
    Limits limits;
    std::deque<qintptr> pending;

    struct Worker {
        std::unique_ptr<QThread> thread;
        std::shared_ptr<std::atomic<bool>> done;
        std::string peer;
    };
    std::vector<Worker> workers;

    void incomingConnection(qintptr descriptor) override { pending.push_back(descriptor); }

    void reap(bool all) {
        auto keep = std::partition(workers.begin(), workers.end(), [all](const Worker& w) { return !all && !w.done->load(); });
        for (auto it = keep; it != workers.end(); ++it) it->thread->wait();
        workers.erase(keep, workers.end());
    }
};

Server::Server(Handler handler, Limits limits) : impl_(std::make_unique<Impl>()) {
    impl_->handler = std::move(handler);
    impl_->limits = std::move(limits);
}

Server::~Server() {
    impl_->close();
    impl_->reap(true);
}

std::optional<std::string> Server::listen(const std::string& host, std::uint16_t port) {
    QHostAddress address;
    const QString name = QString::fromStdString(host);
    if (name == QLatin1String("localhost")) {
        address = QHostAddress(QHostAddress::LocalHost);
    } else if (!address.setAddress(name.startsWith(QLatin1Char('[')) && name.endsWith(QLatin1Char(']')) ? name.mid(1, name.size() - 2) : name)) {
        return "cannot listen on " + host + ": not an IP address";
    }
    impl_->setMaxPendingConnections(128);
    if (!impl_->QTcpServer::listen(address, port)) return "cannot listen on " + host + ":" + std::to_string(port) + ": " + impl_->errorString().toStdString();
    return std::nullopt;
}

std::uint16_t Server::port() const { return impl_->serverPort(); }

void Server::run(const std::atomic<bool>& stop) {
    while (!stop.load()) {
        impl_->waitForNewConnection(200);
        while (!impl_->pending.empty()) {
            const qintptr descriptor = impl_->pending.front();
            impl_->pending.pop_front();
            impl_->reap(false);
            const std::string peer = peer_of(descriptor);
            const auto from_peer = std::count_if(impl_->workers.begin(), impl_->workers.end(), [&peer](const Impl::Worker& w) { return w.peer == peer; });
            if (static_cast<int>(impl_->workers.size()) >= impl_->limits.max_connections || from_peer >= impl_->limits.max_connections_per_peer) {
                // (as many connections as this server serves at once, or from that address: this one is told to come
                // back, and what it already sent is read for a moment so that the answer is not lost to a reset)
                QTcpSocket busy;
                if (busy.setSocketDescriptor(descriptor)) {
                    SocketResponder r(busy);
                    plain(r, 503, "Service Unavailable");
                    finish(busy, std::min(impl_->limits.linger_ms, 100));
                }
                continue;
            }
            auto done = std::make_shared<std::atomic<bool>>(false);
            const Handler* handler = &impl_->handler;
            const Limits* limits = &impl_->limits;
            const std::atomic<bool>* stopping = &stop;
            std::unique_ptr<QThread> thread(QThread::create([descriptor, handler, limits, stopping, done] {
                try {
                    serve_connection(descriptor, *handler, *limits, *stopping);
                } catch (const std::exception& error) {
                    std::fprintf(stderr, "genko: a connection failed: %s\n", error.what());
                }
                done->store(true);
            }));
            thread->start();
            impl_->workers.push_back(Impl::Worker{std::move(thread), done, peer});
        }
    }
    impl_->close();
    impl_->reap(true);
}

}  // namespace genko::api::http
