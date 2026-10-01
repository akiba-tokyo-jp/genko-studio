#include "storage/fsutil.hpp"

#include <QRandomGenerator>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <system_error>

#include "core/error.hpp"
#include "core/pyconv.hpp"

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

#ifndef _WIN32
// Remove the temporary file, then fail.
[[noreturn]] void fail_removing(int error_number, const fs::path& tmp, const fs::path& path) {
    ::unlink(tmp.c_str());
    fail(error_number, path);
}
#endif

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

struct Handle {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~Handle() {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
};

#else

struct Fd {
    int fd = -1;
    ~Fd() {
        if (fd >= 0) ::close(fd);
    }
};

#endif

}  // namespace

std::string os_error_text(int error_number, const fs::path& path) {
    return "[Errno " + std::to_string(error_number) + "] " + errno_text(error_number) + ": " +
           core::py_repr_str(path_to_utf8(path));
}

std::string read_file(const fs::path& path) {
#ifdef _WIN32
    Handle file;
    file.h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file.h == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        std::error_code ec;
        if (code == ERROR_ACCESS_DENIED && fs::is_directory(path, ec)) fail(EISDIR, path);
        fail(errno_from_windows(code), path);
    }
    std::string out;
    char buf[1 << 16];
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(file.h, buf, static_cast<DWORD>(sizeof buf), &got, nullptr)) fail(errno_from_windows(GetLastError()), path);
        if (got == 0) break;
        out.append(buf, got);
    }
    return out;
#else
    Fd file;
    do {
        file.fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    } while (file.fd < 0 && errno == EINTR);
    if (file.fd < 0) fail(errno, path);
    struct stat st {};
    if (::fstat(file.fd, &st) == 0 && S_ISDIR(st.st_mode)) fail(EISDIR, path);
    std::string out;
    if (st.st_size > 0) out.reserve(static_cast<std::size_t>(st.st_size));
    char buf[1 << 16];
    for (;;) {
        const ssize_t got = ::read(file.fd, buf, sizeof buf);
        if (got < 0) {
            if (errno == EINTR) continue;
            fail(errno, path);
        }
        if (got == 0) break;
        out.append(buf, static_cast<std::size_t>(got));
    }
    return out;
#endif
}

void write_atomic(const fs::path& path, std::string_view bytes) {
    const fs::path folder = path.has_parent_path() ? path.parent_path() : fs::path(".");
    std::error_code ec;
    fs::create_directories(folder, ec);
    if (ec) throw core::Error("io", os_error_text(ec.value(), folder), path_to_utf8(folder));
    const fs::path tmp = folder / path_from_utf8(temp_name(path));
#ifdef _WIN32
    {
        Handle file;
        file.h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file.h == INVALID_HANDLE_VALUE) fail(errno_from_windows(GetLastError()), tmp);
        std::size_t done = 0;
        while (done < bytes.size()) {
            const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - done, 1u << 30));
            DWORD wrote = 0;
            if (!WriteFile(file.h, bytes.data() + done, chunk, &wrote, nullptr)) {
                const DWORD code = GetLastError();
                CloseHandle(file.h);
                file.h = INVALID_HANDLE_VALUE;
                DeleteFileW(tmp.c_str());
                fail(errno_from_windows(code), tmp);
            }
            done += wrote;
        }
        if (!FlushFileBuffers(file.h)) {
            const DWORD code = GetLastError();
            CloseHandle(file.h);
            file.h = INVALID_HANDLE_VALUE;
            DeleteFileW(tmp.c_str());
            fail(errno_from_windows(code), tmp);
        }
    }
    for (int attempt = 0;; ++attempt) {
        if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return;
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
        do {
            file.fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        } while (file.fd < 0 && errno == EINTR);
        if (file.fd < 0) fail(errno, tmp);
        std::size_t done = 0;
        while (done < bytes.size()) {
            const ssize_t wrote = ::write(file.fd, bytes.data() + done, bytes.size() - done);
            if (wrote < 0) {
                if (errno == EINTR) continue;
                fail_removing(errno, tmp, tmp);
            }
            done += static_cast<std::size_t>(wrote);
        }
        if (::fsync(file.fd) != 0) fail_removing(errno, tmp, tmp);
        const int fd = file.fd;
        file.fd = -1;
        if (::close(fd) != 0) fail_removing(errno, tmp, tmp);
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) fail_removing(errno, tmp, path);
    Fd dir;
    dir.fd = ::open(folder.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir.fd < 0) fail(errno, folder);
    // (some file systems cannot sync a folder: EINVAL there means there is nothing more to do)
    if (::fsync(dir.fd) != 0 && errno != EINVAL) fail(errno, folder);
#endif
}

}  // namespace genko::storage
