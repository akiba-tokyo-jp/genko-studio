#pragma once

#include <chrono>
#include <filesystem>
#include <string>

#include "core/error.hpp"
#include "core/json.hpp"

// project.lock: one writer at a time (Python's genko/lock.py, and the same lock, so Python and C++ writers exclude
// each other). The lock is the OS's: flock(LOCK_EX | LOCK_NB) on POSIX, LockFileEx(LOCKFILE_EXCLUSIVE_LOCK |
// LOCKFILE_FAIL_IMMEDIATELY) on the first byte on Windows (Python's msvcrt.locking(fd, LK_NBLCK, 1)). A crashed
// process never leaves a stale lock. The file's content is only for display: {"token", "agent", "pid", "host",
// "acquired_at"} while held, {"released": true, "agent", "released_at"} after. A file written by an older Genko (no
// token, not released, acquired under 15 minutes ago) is still respected.

namespace genko::storage {

inline constexpr double kLegacyLockStaleSeconds = 15 * 60;

// Someone else holds the lock: code "locked", Python's message "project locked: <dir>/project.lock (by <agent>)".
class LockedError : public core::Error {
public:
    explicit LockedError(const std::string& message);
};

class ProjectLock {
public:
    struct Options {
        bool create = true;         // make the folder and project.lock when they are missing
        bool write_content = true;  // write {token, agent, …} on taking it and "released" on giving it back
    };

    explicit ProjectLock(std::filesystem::path project, std::string agent = "genko");
    ProjectLock(std::filesystem::path project, std::string agent, Options options);
    ~ProjectLock();
    ProjectLock(const ProjectLock&) = delete;
    ProjectLock& operator=(const ProjectLock&) = delete;

    // Take the lock now, or throw LockedError (another process, or an older Genko's file, holds it). Other failures
    // (no such folder with create=false, permissions) throw core::Error("io"/"not_found").
    void try_acquire();
    // Keep trying (every 50 ms) for up to `timeout`, then throw as try_acquire does.
    void acquire(std::chrono::milliseconds timeout);
    // Give it back (and mark the file released when we wrote it). Nothing when it is not held.
    void release();
    bool held() const;

    const std::filesystem::path& project() const { return project_; }
    std::filesystem::path path() const { return project_ / "project.lock"; }
    const std::string& agent() const { return agent_; }
    const std::string& token() const { return token_; }

    // The file's content as JSON ({} when it cannot be read or is not a JSON object).
    static core::Json read_content(const std::filesystem::path& lock_file);

private:
    std::filesystem::path project_;
    std::string agent_;
    Options options_;
    std::string token_;
#ifdef _WIN32
    void* handle_ = nullptr;
#else
    int fd_ = -1;
#endif
};

}  // namespace genko::storage
