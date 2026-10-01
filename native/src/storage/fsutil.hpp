#pragma once

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
// holds the file. Missing parent folders are made. Throws core::Error("io").
void write_atomic(const std::filesystem::path& path, std::string_view bytes);

// The whole file. Throws core::Error("not_found") when it does not exist and core::Error("io") otherwise, with
// Python's message ("[Errno 2] No such file or directory: 'book.genko/project.json'").
std::string read_file(const std::filesystem::path& path);

// Python's OSError text for an errno and a path: "[Errno 13] Permission denied: '…'".
std::string os_error_text(int error_number, const std::filesystem::path& path);

}  // namespace genko::storage
