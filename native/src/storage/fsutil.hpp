#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "core/paths.hpp"

namespace genko::storage {

// UTF-8 text ↔ paths (the conversions live in core/paths.hpp; Windows never goes through the ANSI code page).
using core::path_from_utf8;
using core::path_to_utf8;

// Write bytes so that a crash leaves either the old file or the new one: a temporary file in the same folder,
// flushed and fsynced, renamed over the target, then (POSIX) the folder fsynced. On Windows the rename is
// MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH), retried up to 8 times while another process
// holds the file. Missing parent folders are made (and their parents fsynced). Throws core::Error("io"); when the
// error comes after the rename (the folder's fsync), the file may already hold the new bytes.
void write_atomic(const std::filesystem::path& path, std::string_view bytes);

// Append bytes to a file and fsync it. A missing file is made (with its folders), and then its folder is fsynced so
// the new file survives a power cut. Throws core::Error("io").
void append_durable(const std::filesystem::path& path, std::string_view bytes);

// Cut a file to its first `size` bytes and fsync it. Throws core::Error("io").
void truncate_durable(const std::filesystem::path& path, std::uintmax_t size);

// fsync a folder, so the names made or renamed in it survive a power cut (POSIX; nothing to do on Windows).
void sync_dir(const std::filesystem::path& dir);

// Make a folder and its missing parents; the folder holding each one made is fsynced.
void make_dirs_durable(const std::filesystem::path& dir);

// Write a file that must not exist yet (made with O_EXCL; missing parents are made) and fsync it. Its folder is not
// fsynced: the caller does that once for many files.
void write_new_file(const std::filesystem::path& path, std::string_view bytes);

// Copy a file to a path that must not exist yet (missing parents are made), fsynced, and return the sha256 (64
// lowercase hex digits) of the bytes copied.
std::string copy_file_hashed(const std::filesystem::path& from, const std::filesystem::path& to);

// The sha256 (64 lowercase hex digits) of a file's bytes, read in pieces; of some bytes.
std::string sha256_file(const std::filesystem::path& path);
std::string sha256_hex(std::string_view bytes);

// Rename a file or folder to a name that must not exist, on the same volume (POSIX rename; Windows MoveFileExW with
// MOVEFILE_WRITE_THROUGH), then fsync the folders on both sides.
void rename_new(const std::filesystem::path& from, const std::filesystem::path& to);

// The whole file. Throws core::Error("not_found") when it does not exist and core::Error("io") otherwise, with
// Python's message ("[Errno 2] No such file or directory: 'book.genko/project.json'").
std::string read_file(const std::filesystem::path& path);

// Python's OSError text for an errno and a path: "[Errno 13] Permission denied: '…'".
std::string os_error_text(int error_number, const std::filesystem::path& path);

}  // namespace genko::storage
