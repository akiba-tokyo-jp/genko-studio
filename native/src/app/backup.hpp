#pragma once

#include <QDateTime>
#include <QString>

#include <filesystem>
#include <optional>
#include <vector>

// バックアップ (Python's genko/backup.py): the whole book zipped into another folder now and then (after a save), the
// newest `keep` kept. The folder is chosen in 環境設定 (backup/folder, backup/minutes, backup/keep).

namespace genko::app::backup {

// This book's backups in the folder ("<stem>_YYYYMMDD-HHMMSS.zip"), oldest first.
std::vector<std::filesystem::path> backups(const std::filesystem::path& folder, const QString& stem);
// Zip the book into the folder (named by the time `now`, local) and drop the oldest past `keep`: written as
// "….zip.part" and renamed. Every file of the book is in it under "<book folder's name>/…" but the ones being written
// (.tmp) and the lock (.lock). Throws std::invalid_argument("バックアップのフォルダーは、原稿のフォルダーの外にしてください")
// for a folder inside the book (or the book itself), core::Error("io") when it cannot be written.
std::filesystem::path make(const std::filesystem::path& project, const std::filesystem::path& folder, int keep = 10,
                           std::optional<QDateTime> now = std::nullopt);
// Whether the last backup of this book is older than `minutes` (or there is none).
bool due(const std::filesystem::path& folder, const QString& stem, double minutes, std::optional<QDateTime> now = std::nullopt);

}  // namespace genko::app::backup
