#include "storage/lock.hpp"

#include <QCoreApplication>
#include <QSysInfo>

#include <cerrno>
#include <thread>
#include <utility>

#include "core/ids.hpp"
#include "core/pyconv.hpp"
#include "storage/fsutil.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace genko::storage {

namespace fs = std::filesystem;
using core::Json;

namespace {

constexpr std::size_t kMaxContent = 65536;

double now_seconds() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// Python's ProjectLock._read: the content as JSON, {} when it is empty or not JSON.
Json parse_content(const std::string& raw) {
    bool blank = true;
    for (const char c : raw) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\v' && c != '\f') {
            blank = false;
            break;
        }
    }
    if (blank) return Json::object();
    try {
        Json value = core::parse_python_json(raw);
        return value.is_object() ? value : Json::object();
    } catch (const core::Error&) {
        return Json::object();
    }
}

// Python's json.dumps(data): ASCII only, ", " and ": ".
std::string dump_content(const Json& value) { return core::dump_python(value, true); }

std::string holder_of(const Json& content, const char* fallback) {
    const auto it = content.find("agent");
    return it == content.end() ? std::string(fallback) : core::py_str(*it);
}

// A file written by a Genko from before the OS lock: no token, not released, taken under 15 minutes ago.
bool legacy_holder(const Json& content) {
    if (content.empty() || content.contains("token")) return false;
    if (const auto it = content.find("released"); it != content.end() && core::py_truthy(*it)) return false;
    double acquired = 0.0;
    if (const auto it = content.find("acquired_at"); it != content.end() && core::py_truthy(*it)) {
        try {
            acquired = core::py_float(*it);
        } catch (const core::Error&) {
            acquired = 0.0;
        }
    }
    return now_seconds() - acquired < kLegacyLockStaleSeconds;
}

std::string locked_message(const fs::path& lock_file, const std::string& holder) {
    return "project locked: " + path_to_utf8(lock_file) + " (by " + holder + ")";
}

#ifdef _WIN32

std::string read_handle(HANDLE h) {
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    if (!SetFilePointerEx(h, zero, nullptr, FILE_BEGIN)) return {};
    std::string out(kMaxContent, '\0');
    DWORD got = 0;
    if (!ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr)) return {};
    out.resize(got);
    return out;
}

void write_handle(HANDLE h, const std::string& body, const fs::path& lock_file) {
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    DWORD wrote = 0;
    if (!SetFilePointerEx(h, zero, nullptr, FILE_BEGIN) || !SetEndOfFile(h) ||
        !WriteFile(h, body.data(), static_cast<DWORD>(body.size()), &wrote, nullptr) || wrote != body.size()) {
        throw core::Error("io", os_error_text(EIO, lock_file), path_to_utf8(lock_file));
    }
}

void unlock_handle(HANDLE h) {
    OVERLAPPED at{};
    UnlockFileEx(h, 0, 1, 0, &at);
}

#else

std::string read_fd(int fd) {
    if (::lseek(fd, 0, SEEK_SET) < 0) return {};
    std::string out(kMaxContent, '\0');
    ssize_t got = -1;
    do {
        got = ::read(fd, out.data(), out.size());
    } while (got < 0 && errno == EINTR);
    if (got < 0) return {};
    out.resize(static_cast<std::size_t>(got));
    return out;
}

void write_fd(int fd, const std::string& body, const fs::path& lock_file) {
    if (::lseek(fd, 0, SEEK_SET) < 0 || ::ftruncate(fd, 0) != 0) {
        throw core::Error("io", os_error_text(errno, lock_file), path_to_utf8(lock_file));
    }
    std::size_t done = 0;
    while (done < body.size()) {
        const ssize_t wrote = ::write(fd, body.data() + done, body.size() - done);
        if (wrote < 0) {
            if (errno == EINTR) continue;
            throw core::Error("io", os_error_text(errno, lock_file), path_to_utf8(lock_file));
        }
        done += static_cast<std::size_t>(wrote);
    }
}

#endif

}  // namespace

LockedError::LockedError(const std::string& message) : core::Error("locked", message) {}

ProjectLock::ProjectLock(fs::path project, std::string agent) : ProjectLock(std::move(project), std::move(agent), Options{}) {}

ProjectLock::ProjectLock(fs::path project, std::string agent, Options options)
    : project_(std::move(project)), agent_(std::move(agent)), options_(options), token_(core::new_book_id()) {}

ProjectLock::~ProjectLock() {
    try {
        release();
    } catch (...) {
        // (the OS gives the lock back when the handle closes, which release() does first of all failures)
    }
}

bool ProjectLock::held() const {
#ifdef _WIN32
    return handle_ != nullptr;
#else
    return fd_ >= 0;
#endif
}

void ProjectLock::try_acquire() {
    if (held()) return;
    const fs::path lock_file = path();
    if (options_.create) make_dirs_durable(project_);  // (Python: self.project.mkdir(parents=True, exist_ok=True))
#ifdef _WIN32
    const DWORD access = options_.write_content ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ;
    HANDLE h = CreateFileW(lock_file.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           options_.create ? OPEN_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        const int error_number = code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND ? ENOENT : EACCES;
        throw core::Error(error_number == ENOENT ? "not_found" : "io", os_error_text(error_number, lock_file),
                          path_to_utf8(lock_file));
    }
    OVERLAPPED at{};
    if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &at)) {
        const DWORD code = GetLastError();
        const std::string holder = holder_of(parse_content(read_handle(h)), "another process");
        CloseHandle(h);
        if (code == ERROR_LOCK_VIOLATION || code == ERROR_IO_PENDING) throw LockedError(locked_message(lock_file, holder));
        throw core::Error("io", os_error_text(EIO, lock_file), path_to_utf8(lock_file));
    }
    const Json current = parse_content(read_handle(h));
    if (legacy_holder(current)) {
        unlock_handle(h);
        CloseHandle(h);
        throw LockedError(locked_message(lock_file, holder_of(current, "an older Genko")));
    }
    if (options_.write_content) {
        Json content = Json::object();
        content["token"] = token_;
        content["agent"] = agent_;
        content["pid"] = static_cast<std::int64_t>(QCoreApplication::applicationPid());
        content["host"] = QSysInfo::machineHostName().toStdString();
        content["acquired_at"] = now_seconds();
        try {
            write_handle(h, dump_content(content), lock_file);
        } catch (...) {
            unlock_handle(h);
            CloseHandle(h);
            throw;
        }
    }
    handle_ = h;
#else
    int fd = -1;
    const int flags = (options_.write_content ? O_RDWR : O_RDONLY) | (options_.create ? O_CREAT : 0) | O_CLOEXEC;
    do {
        fd = ::open(lock_file.c_str(), flags, 0644);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        const int error_number = errno;
        throw core::Error(error_number == ENOENT ? "not_found" : "io", os_error_text(error_number, lock_file),
                          path_to_utf8(lock_file));
    }
    int locked = -1;
    do {
        locked = ::flock(fd, LOCK_EX | LOCK_NB);
    } while (locked != 0 && errno == EINTR);
    if (locked != 0) {
        const int error_number = errno;
        const std::string holder = holder_of(parse_content(read_fd(fd)), "another process");
        ::close(fd);
        if (error_number == EWOULDBLOCK) throw LockedError(locked_message(lock_file, holder));
        throw core::Error("io", os_error_text(error_number, lock_file), path_to_utf8(lock_file));
    }
    const Json current = parse_content(read_fd(fd));
    if (legacy_holder(current)) {
        ::flock(fd, LOCK_UN);
        ::close(fd);
        throw LockedError(locked_message(lock_file, holder_of(current, "an older Genko")));
    }
    if (options_.write_content) {
        Json content = Json::object();
        content["token"] = token_;
        content["agent"] = agent_;
        content["pid"] = static_cast<std::int64_t>(QCoreApplication::applicationPid());
        content["host"] = QSysInfo::machineHostName().toStdString();
        content["acquired_at"] = now_seconds();
        try {
            write_fd(fd, dump_content(content), lock_file);
        } catch (...) {
            ::flock(fd, LOCK_UN);
            ::close(fd);
            throw;
        }
    }
    fd_ = fd;
#endif
}

void ProjectLock::acquire(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        try {
            try_acquire();
            return;
        } catch (const LockedError&) {
            if (std::chrono::steady_clock::now() >= deadline) throw;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void ProjectLock::release() {
    if (!held()) return;
    const fs::path lock_file = path();
#ifdef _WIN32
    HANDLE h = static_cast<HANDLE>(handle_);
    handle_ = nullptr;
    try {
        if (options_.write_content) {
            const Json current = parse_content(read_handle(h));
            const auto token = current.find("token");
            if (token != current.end() && *token == token_) {
                Json content = Json::object();
                content["released"] = true;
                content["agent"] = agent_;
                content["released_at"] = now_seconds();
                write_handle(h, dump_content(content), lock_file);
            }
        }
    } catch (...) {
        unlock_handle(h);
        CloseHandle(h);
        throw;
    }
    unlock_handle(h);
    CloseHandle(h);
#else
    const int fd = fd_;
    fd_ = -1;
    try {
        if (options_.write_content) {
            const Json current = parse_content(read_fd(fd));
            const auto token = current.find("token");
            if (token != current.end() && *token == token_) {
                Json content = Json::object();
                content["released"] = true;
                content["agent"] = agent_;
                content["released_at"] = now_seconds();
                write_fd(fd, dump_content(content), lock_file);
            }
        }
    } catch (...) {
        ::flock(fd, LOCK_UN);
        ::close(fd);
        throw;
    }
    ::flock(fd, LOCK_UN);
    ::close(fd);
#endif
}

Json ProjectLock::read_content(const fs::path& lock_file) {
    try {
        std::string raw = read_file(lock_file);
        if (raw.size() > kMaxContent) raw.resize(kMaxContent);
        return parse_content(raw);
    } catch (const core::Error&) {
        return Json::object();
    }
}

}  // namespace genko::storage
