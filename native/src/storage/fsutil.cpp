#include "storage/fsutil.hpp"

#include <QByteArray>
#include <QByteArrayView>
#include <QCryptographicHash>
#include <QRandomGenerator>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <system_error>
#include <vector>

#include "core/error.hpp"
#include "core/pyconv.hpp"
#include "storage/fault.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace genko::storage {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kChunk = 1 << 16;

// The C library's English text (Python reads errno messages in the "C" locale).
std::string errno_text(int error_number) {
    switch (error_number) {
        case ENOENT: return "No such file or directory";
        case EACCES: return "Permission denied";
        case EISDIR: return "Is a directory";
        case ENOTDIR: return "Not a directory";
        case ENOSPC: return "No space left on device";
        case EROFS: return "Read-only file system";
        case EEXIST: return "File exists";
        case EIO: return "Input/output error";
        case ENAMETOOLONG: return "File name too long";
        case EMFILE: return "Too many open files";
        case EPERM: return "Operation not permitted";
        default: return std::generic_category().message(error_number);
    }
}

[[noreturn]] void fail(int error_number, const fs::path& path) {
    throw core::Error(error_number == ENOENT ? "not_found" : "io", os_error_text(error_number, path),
                      path_to_utf8(path));
}

std::string temp_name(const fs::path& path) {
    const auto token = QRandomGenerator::system()->generate64();
    char hex[17];
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(token));
#ifdef _WIN32
    const auto pid = static_cast<unsigned long>(GetCurrentProcessId());
#else
    const auto pid = static_cast<unsigned long>(getpid());
#endif
    return path_to_utf8(path.filename()) + "." + std::to_string(pid) + "." + hex + ".tmp";
}

fs::path folder_of(const fs::path& path) { return path.has_parent_path() ? path.parent_path() : fs::path("."); }

#ifdef _WIN32

int errno_from_windows(DWORD code) {
    switch (code) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND: return ENOENT;
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION: return EACCES;
        case ERROR_DISK_FULL:
        case ERROR_HANDLE_DISK_FULL: return ENOSPC;
        case ERROR_WRITE_PROTECT: return EROFS;
        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS: return EEXIST;
        case ERROR_DIRECTORY: return ENOTDIR;
        default: return EIO;
    }
}

[[noreturn]] void fail_windows(const fs::path& path) { fail(errno_from_windows(GetLastError()), path); }

struct Handle {
    HANDLE h = INVALID_HANDLE_VALUE;
    Handle() = default;
    explicit Handle(HANDLE handle) : h(handle) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    bool valid() const { return h != INVALID_HANDLE_VALUE; }
};

void write_all(HANDLE h, const char* data, std::size_t size, const fs::path& path) {
    std::size_t done = 0;
    while (done < size) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size - done, std::size_t{1} << 30));
        DWORD wrote = 0;
        if (!WriteFile(h, data + done, chunk, &wrote, nullptr)) fail_windows(path);
        done += wrote;
    }
}

// Read the next piece; 0 at the end.
std::size_t read_some(HANDLE h, char* buffer, std::size_t size, const fs::path& path) {
    DWORD got = 0;
    if (!ReadFile(h, buffer, static_cast<DWORD>(size), &got, nullptr)) fail_windows(path);
    return got;
}

HANDLE open_read(const fs::path& path) {
    return CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}

#else

struct Fd {
    int fd = -1;
    Fd() = default;
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    ~Fd() {
        if (fd >= 0) ::close(fd);
    }
};

int open_retry(const fs::path& path, int flags, mode_t mode = 0) {
    int fd = -1;
    do {
        fd = ::open(path.c_str(), flags, mode);
    } while (fd < 0 && errno == EINTR);
    return fd;
}

void write_all(int fd, const char* data, std::size_t size, const fs::path& path) {
    std::size_t done = 0;
    while (done < size) {
        const ssize_t wrote = ::write(fd, data + done, size - done);
        if (wrote < 0) {
            if (errno == EINTR) continue;
            fail(errno, path);
        }
        done += static_cast<std::size_t>(wrote);
    }
}

std::size_t read_some(int fd, char* buffer, std::size_t size, const fs::path& path) {
    for (;;) {
        const ssize_t got = ::read(fd, buffer, size);
        if (got < 0) {
            if (errno == EINTR) continue;
            fail(errno, path);
        }
        return static_cast<std::size_t>(got);
    }
}

void close_checked(Fd& file, const fs::path& path) {
    const int fd = file.fd;
    file.fd = -1;
    if (::close(fd) != 0) fail(errno, path);
}

#endif

}  // namespace

std::string os_error_text(int error_number, const fs::path& path) {
    return "[Errno " + std::to_string(error_number) + "] " + errno_text(error_number) + ": " +
           core::py_repr_str(path_to_utf8(path));
}

std::string read_file(const fs::path& path) {
#ifdef _WIN32
    Handle file(open_read(path));
    if (!file.valid()) {
        const DWORD code = GetLastError();
        std::error_code ec;
        if (code == ERROR_ACCESS_DENIED && fs::is_directory(path, ec)) fail(EISDIR, path);
        fail(errno_from_windows(code), path);
    }
    std::string out;
    std::vector<char> buf(kChunk);
    for (;;) {
        const std::size_t got = read_some(file.h, buf.data(), buf.size(), path);
        if (got == 0) break;
        out.append(buf.data(), got);
    }
    return out;
#else
    Fd file;
    file.fd = open_retry(path, O_RDONLY | O_CLOEXEC);
    if (file.fd < 0) fail(errno, path);
    struct stat st {};
    if (::fstat(file.fd, &st) == 0 && S_ISDIR(st.st_mode)) fail(EISDIR, path);
    std::string out;
    if (st.st_size > 0) out.reserve(static_cast<std::size_t>(st.st_size));
    std::vector<char> buf(kChunk);
    for (;;) {
        const std::size_t got = read_some(file.fd, buf.data(), buf.size(), path);
        if (got == 0) break;
        out.append(buf.data(), got);
    }
    return out;
#endif
}

std::string read_file_bounded(const fs::path& path, std::size_t maximum) {
#ifdef _WIN32
    Handle file(open_read(path));
    if (!file.valid()) fail(errno_from_windows(GetLastError()), path);
    if (GetFileType(file.h) != FILE_TYPE_DISK) throw core::Error("format", "bounded asset must be a regular file");
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(file.h, &length)) fail(errno_from_windows(GetLastError()), path);
    if (length.QuadPart < 0 || static_cast<std::uintmax_t>(length.QuadPart) > maximum)
        throw core::Error("memory", "bounded asset read limit exceeded");
    const auto size = static_cast<std::size_t>(length.QuadPart);
    const auto read = [&](char* out, std::size_t count) { return read_some(file.h, out, count, path); };
#else
    Fd file;
    file.fd = open_retry(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (file.fd < 0) fail(errno, path);
    struct stat st{};
    if (::fstat(file.fd, &st) != 0) fail(errno, path);
    if (!S_ISREG(st.st_mode)) throw core::Error("format", "bounded asset must be a regular file");
    if (st.st_size < 0 || static_cast<std::uintmax_t>(st.st_size) > maximum)
        throw core::Error("memory", "bounded asset read limit exceeded");
    const auto size = static_cast<std::size_t>(st.st_size);
    const auto read = [&](char* out, std::size_t count) { return read_some(file.fd, out, count, path); };
#endif
    // Allocate exactly the checked size. Content-addressed assets must not change
    // while being read; never append a growth discovered after this allocation.
    std::string out(size, '\0');
    std::size_t at = 0;
    while (at < size) {
        const auto got = read(out.data()+at, std::min(kChunk, size-at));
        if (!got) throw core::Error("format", "asset shrank during bounded read");
        at += got;
    }
    char extra = 0;
    if (read(&extra, 1)) throw core::Error("format", "asset grew during bounded read");
    return out;
}

void sync_dir(const fs::path& dir) {
#ifdef _WIN32
    (void)dir;  // (NTFS keeps its folders consistent; MoveFileEx is asked to write through)
#else
    const fs::path target = dir.empty() ? fs::path(".") : dir;
    Fd folder;
    folder.fd = open_retry(target, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (folder.fd < 0) fail(errno, target);
    // (some file systems cannot sync a folder: EINVAL there means there is nothing more to do)
    if (::fsync(folder.fd) != 0 && errno != EINVAL) fail(errno, target);
#endif
}

void make_dirs_durable(const fs::path& dir) {
    std::error_code ec;
    if (fs::is_directory(dir, ec)) return;
    std::vector<fs::path> missing;  // from the folder up to the first one that exists
    for (fs::path p = dir; !p.empty(); p = p.parent_path()) {
        if (fs::exists(p, ec)) break;
        missing.push_back(p);
        if (p == p.parent_path()) break;
    }
    fs::create_directories(dir, ec);
    if (ec) throw core::Error("io", os_error_text(ec.value(), dir), path_to_utf8(dir));
    for (auto it = missing.rbegin(); it != missing.rend(); ++it) sync_dir(folder_of(*it));
}

void write_atomic(const fs::path& path, std::string_view bytes) {
    fault::before_write(path);
    const fs::path folder = folder_of(path);
    make_dirs_durable(folder);
    const fs::path tmp = folder / path_from_utf8(temp_name(path));
    const std::size_t count = fault::bytes_to_write(bytes.size());
#ifdef _WIN32
    {
        Handle file(CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file.valid()) fail_windows(tmp);
        try {
            write_all(file.h, bytes.data(), count, tmp);
            if (count < bytes.size()) fault::after_write(path);  // (torn: the process stops here)
            if (!FlushFileBuffers(file.h)) fail_windows(tmp);
        } catch (...) {
            CloseHandle(file.h);
            file.h = INVALID_HANDLE_VALUE;
            DeleteFileW(tmp.c_str());
            throw;
        }
    }
    for (int attempt = 0;; ++attempt) {
        if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) break;
        const DWORD code = GetLastError();
        const bool busy = code == ERROR_SHARING_VIOLATION || code == ERROR_ACCESS_DENIED || code == ERROR_LOCK_VIOLATION;
        if (!busy || attempt == 7) {
            DeleteFileW(tmp.c_str());
            fail(errno_from_windows(code), path);
        }
        Sleep(static_cast<DWORD>(50u << attempt));  // (Python waits 0.05 × 2^attempt seconds)
    }
#else
    {
        Fd file;
        file.fd = open_retry(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        if (file.fd < 0) fail(errno, tmp);
        try {
            write_all(file.fd, bytes.data(), count, tmp);
            if (count < bytes.size()) fault::after_write(path);  // (torn: the process stops here)
            if (::fsync(file.fd) != 0) fail(errno, tmp);
            close_checked(file, tmp);
        } catch (...) {
            ::unlink(tmp.c_str());
            throw;
        }
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        const int error_number = errno;
        ::unlink(tmp.c_str());
        fail(error_number, path);
    }
    sync_dir(folder);  // (an error here comes after the rename: the file may hold the new bytes already)
#endif
    fault::after_write(path);
}

void append_durable(const fs::path& path, std::string_view bytes) {
    fault::before_write(path);
    const fs::path folder = folder_of(path);
    make_dirs_durable(folder);
    const std::size_t count = fault::bytes_to_write(bytes.size());
#ifdef _WIN32
    Handle file(CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid()) fail_windows(path);
    write_all(file.h, bytes.data(), count, path);
    if (count < bytes.size()) fault::after_write(path);  // (torn: the process stops here)
    if (!FlushFileBuffers(file.h)) fail_windows(path);
#else
    bool created = true;
    Fd file;
    file.fd = open_retry(path, O_WRONLY | O_APPEND | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (file.fd < 0 && errno == EEXIST) {
        created = false;
        file.fd = open_retry(path, O_WRONLY | O_APPEND | O_CLOEXEC);
    }
    if (file.fd < 0) fail(errno, path);
    write_all(file.fd, bytes.data(), count, path);
    if (count < bytes.size()) fault::after_write(path);  // (torn: the process stops here)
    if (::fsync(file.fd) != 0) fail(errno, path);
    close_checked(file, path);
    if (created) sync_dir(folder);
#endif
    fault::after_write(path);
}

void truncate_durable(const fs::path& path, std::uintmax_t size) {
#ifdef _WIN32
    Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid()) fail_windows(path);
    LARGE_INTEGER at;
    at.QuadPart = static_cast<LONGLONG>(size);
    if (!SetFilePointerEx(file.h, at, nullptr, FILE_BEGIN) || !SetEndOfFile(file.h) || !FlushFileBuffers(file.h)) {
        fail_windows(path);
    }
#else
    Fd file;
    file.fd = open_retry(path, O_WRONLY | O_CLOEXEC);
    if (file.fd < 0) fail(errno, path);
    if (::ftruncate(file.fd, static_cast<off_t>(size)) != 0) fail(errno, path);
    if (::fsync(file.fd) != 0) fail(errno, path);
    close_checked(file, path);
#endif
}

void write_new_file(const fs::path& path, std::string_view bytes) {
    std::error_code ec;
    fs::create_directories(folder_of(path), ec);
    if (ec) throw core::Error("io", os_error_text(ec.value(), folder_of(path)), path_to_utf8(folder_of(path)));
#ifdef _WIN32
    Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid()) fail_windows(path);
    write_all(file.h, bytes.data(), bytes.size(), path);
    if (!FlushFileBuffers(file.h)) fail_windows(path);
#else
    Fd file;
    file.fd = open_retry(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (file.fd < 0) fail(errno, path);
    write_all(file.fd, bytes.data(), bytes.size(), path);
    if (::fsync(file.fd) != 0) fail(errno, path);
    close_checked(file, path);
#endif
}

std::string copy_file_hashed(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_directories(folder_of(to), ec);
    if (ec) throw core::Error("io", os_error_text(ec.value(), folder_of(to)), path_to_utf8(folder_of(to)));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    std::vector<char> buf(kChunk);
#ifdef _WIN32
    Handle source(open_read(from));
    if (!source.valid()) fail_windows(from);
    Handle target(CreateFileW(to.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!target.valid()) fail_windows(to);
    for (;;) {
        const std::size_t got = read_some(source.h, buf.data(), buf.size(), from);
        if (got == 0) break;
        hash.addData(QByteArrayView(buf.data(), static_cast<qsizetype>(got)));
        write_all(target.h, buf.data(), got, to);
    }
    if (!FlushFileBuffers(target.h)) fail_windows(to);
#else
    Fd source;
    source.fd = open_retry(from, O_RDONLY | O_CLOEXEC);
    if (source.fd < 0) fail(errno, from);
    Fd target;
    target.fd = open_retry(to, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (target.fd < 0) fail(errno, to);
    for (;;) {
        const std::size_t got = read_some(source.fd, buf.data(), buf.size(), from);
        if (got == 0) break;
        hash.addData(QByteArrayView(buf.data(), static_cast<qsizetype>(got)));
        write_all(target.fd, buf.data(), got, to);
    }
    if (::fsync(target.fd) != 0) fail(errno, to);
    close_checked(target, to);
#endif
    return hash.result().toHex().toStdString();
}

std::string sha256_file(const fs::path& path) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    std::vector<char> buf(kChunk);
#ifdef _WIN32
    Handle file(open_read(path));
    if (!file.valid()) fail_windows(path);
    for (;;) {
        const std::size_t got = read_some(file.h, buf.data(), buf.size(), path);
        if (got == 0) break;
        hash.addData(QByteArrayView(buf.data(), static_cast<qsizetype>(got)));
    }
#else
    Fd file;
    file.fd = open_retry(path, O_RDONLY | O_CLOEXEC);
    if (file.fd < 0) fail(errno, path);
    for (;;) {
        const std::size_t got = read_some(file.fd, buf.data(), buf.size(), path);
        if (got == 0) break;
        hash.addData(QByteArrayView(buf.data(), static_cast<qsizetype>(got)));
    }
#endif
    return hash.result().toHex().toStdString();
}

std::string sha256_hex(std::string_view bytes) {
    return QCryptographicHash::hash(QByteArrayView(bytes.data(), static_cast<qsizetype>(bytes.size())),
                                    QCryptographicHash::Sha256)
        .toHex()
        .toStdString();
}

void rename_new(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    if (fs::exists(to, ec)) fail(EEXIST, to);
#ifdef _WIN32
    if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) fail_windows(to);
#else
    if (::rename(from.c_str(), to.c_str()) != 0) fail(errno, to);
    sync_dir(folder_of(from));
    if (folder_of(from) != folder_of(to)) sync_dir(folder_of(to));
#endif
}

}  // namespace genko::storage
