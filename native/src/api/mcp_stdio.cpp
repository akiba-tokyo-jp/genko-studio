// stdio for `genko mcp` (the MCP SDK's mcp.server.stdio.stdio_server).

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <mutex>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "api/mcp_server.hpp"
#include "api/mcp_transport.hpp"
#include "service/jobs.hpp"

namespace genko::api::mcp {

std::string utf8_replace(std::string_view bytes) {
    static constexpr std::string_view kReplacement = "\xEF\xBF\xBD";
    std::string out;
    out.reserve(bytes.size());
    std::size_t i = 0;
    const auto at = [&bytes](std::size_t k) { return static_cast<unsigned char>(bytes[k]); };
    while (i < bytes.size()) {
        const unsigned char b = at(i);
        if (b < 0x80) {
            out += static_cast<char>(b);
            ++i;
            continue;
        }
        std::size_t need = 0;
        unsigned char low = 0x80;
        unsigned char high = 0xBF;
        if (b >= 0xC2 && b <= 0xDF) {
            need = 1;
        } else if (b >= 0xE0 && b <= 0xEF) {
            need = 2;
            if (b == 0xE0) low = 0xA0;
            if (b == 0xED) high = 0x9F;
        } else if (b >= 0xF0 && b <= 0xF4) {
            need = 3;
            if (b == 0xF0) low = 0x90;
            if (b == 0xF4) high = 0x8F;
        } else {
            out += kReplacement;  // (a byte that starts nothing)
            ++i;
            continue;
        }
        std::size_t k = 1;
        for (; k <= need; ++k) {
            if (i + k >= bytes.size()) break;
            const unsigned char c = at(i + k);
            const unsigned char lo = k == 1 ? low : 0x80;
            const unsigned char hi = k == 1 ? high : 0xBF;
            if (c < lo || c > hi) break;
        }
        if (k == need + 1) {
            out.append(bytes.substr(i, need + 1));
            i += need + 1;
        } else {
            out += kReplacement;  // (the maximal subpart of a sequence cut short: one replacement)
            i += k;
        }
    }
    return out;
}

namespace {

#ifdef _WIN32
using Fd = int;
int read_fd(Fd fd, char* buffer, unsigned size) { return _read(fd, buffer, size); }
int write_fd(Fd fd, const char* data, unsigned size) { return _write(fd, data, size); }
#else
using Fd = int;
long read_fd(Fd fd, char* buffer, std::size_t size) { return ::read(fd, buffer, size); }
long write_fd(Fd fd, const char* data, std::size_t size) { return ::write(fd, data, size); }
#endif

// The standard streams claimed for the protocol: private copies for the wire, fd 0 reading the null device and fd 1
// writing to stderr meanwhile; given back at the end.
class Claim {
public:
    Claim() {
#ifdef _WIN32
        in_ = _dup(0);
        out_ = _dup(1);
        const int null_in = _open("NUL", _O_RDONLY | _O_BINARY);
        if (null_in >= 0) {
            _dup2(null_in, 0);
            _close(null_in);
        }
        _dup2(2, 1);
        SetStdHandle(STD_INPUT_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(0)));
        SetStdHandle(STD_OUTPUT_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(1)));
        _setmode(in_, _O_BINARY);
        _setmode(out_, _O_BINARY);
#else
        in_ = ::fcntl(0, F_DUPFD_CLOEXEC, 3);
        out_ = ::fcntl(1, F_DUPFD_CLOEXEC, 3);
        const int null_in = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (null_in >= 0) {
            ::dup2(null_in, 0);
            ::close(null_in);
        }
        ::dup2(2, 1);
#endif
        if (in_ < 0) in_ = 0;  // (could not copy: serve the stream in place, as the SDK falls back to)
        if (out_ < 0) out_ = 1;
    }
    ~Claim() {
        std::fflush(stdout);
#ifdef _WIN32
        if (in_ > 2) {
            _dup2(in_, 0);
            _close(in_);
        }
        if (out_ > 2) {
            _dup2(out_, 1);
            _close(out_);
        }
#else
        if (in_ > 2) {
            ::dup2(in_, 0);
            ::close(in_);
        }
        if (out_ > 2) {
            ::dup2(out_, 1);
            ::close(out_);
        }
#endif
    }
    Claim(const Claim&) = delete;
    Claim& operator=(const Claim&) = delete;

    Fd in() const { return in_; }
    Fd out() const { return out_; }

private:
    Fd in_ = -1;
    Fd out_ = -1;
};

}  // namespace

int serve_stdio(const std::filesystem::path& root, const std::string& actor) {
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);  // (a client that went away: the write fails instead of ending the process)
#endif
    std::fflush(stdout);
    const Engine engine(root, actor);
    Claim claim;
    std::mutex write_mutex;
    bool broken = false;
    const auto send = [&](const Json& message, const std::optional<Json>&) {
        const std::string line = encode(message) + "\n";
        std::lock_guard<std::mutex> lock(write_mutex);
        if (broken) return;
        std::size_t done = 0;
        while (done < line.size()) {
            const auto n = write_fd(claim.out(), line.data() + done, static_cast<unsigned>(std::min<std::size_t>(line.size() - done, 1U << 30)));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                broken = true;  // (the client stopped reading: what is left is dropped)
                return;
            }
            done += static_cast<std::size_t>(n);
        }
    };
    {
        Connection connection(engine, send, Connection::Options{true, false});
        std::string pending;
        bool skipping = false;  // (inside a line past kMaxLine)
        bool after_cr = false;
        char buffer[65536];
        const auto line_done = [&](std::string line) {
            if (const auto decoded = decode(utf8_replace(line))) connection.receive(*decoded);
            // (a line that is not JSON is dropped, as the SDK drops a frame it cannot read)
        };
        for (;;) {
            const auto n = read_fd(claim.in(), buffer, sizeof buffer);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            for (long i = 0; i < static_cast<long>(n); ++i) {
                const char c = buffer[i];
                if (after_cr) {
                    after_cr = false;
                    if (c == '\n') continue;  // ("\r\n" is one line end)
                }
                if (c == '\n' || c == '\r') {
                    if (c == '\r') after_cr = true;
                    if (!skipping) line_done(std::move(pending));
                    pending.clear();
                    skipping = false;
                    continue;
                }
                if (skipping) continue;
                pending += c;
                if (pending.size() > kMaxLine) {
                    std::fprintf(stderr, "genko mcp: dropped a message longer than %zu bytes\n", kMaxLine);
                    pending.clear();
                    pending.shrink_to_fit();
                    skipping = true;
                }
            }
        }
        if (!skipping && !pending.empty()) line_done(std::move(pending));
        connection.close();
    }
    service::jobs::wait_all();  // (a job started in the background ends before the process does)
    return 0;
}

}  // namespace genko::api::mcp
