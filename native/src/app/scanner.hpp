#pragma once

#include <QString>
#include <QStringList>

#include <functional>
#include <optional>
#include <stdexcept>
#include <string>

// Scanning a page straight in (スキャナーから取り込む; Python's genko/scanner.py), through what the computer already has:
// WIA on Windows (its own scan window), SANE's scanimage on Linux. Macs have no scanning command to call; there the scan
// is saved from the Image Capture app and read as a file. The errors are the person's words (ScanError).

namespace genko::app::scanner {

class ScanError : public std::runtime_error {
public:
    explicit ScanError(const QString& message) : std::runtime_error(message.toStdString()) {}
};

// "wia" (Windows), "sane" (Linux with scanimage), or nothing (nothing to scan with here).
std::optional<QString> method();

// The command that scans one page into `out` (PNG or BMP): the program and its arguments.
QStringList command(const QString& how, const QString& out, int dpi = 600, const QString& mode = QStringLiteral("gray"));

// How a command ran (tests give their own): its exit code, or nothing when it could not be started or did not finish
// in time (`error` says why); what it wrote to stderr.
struct Ran {
    std::optional<int> code;
    QString error;
    QString stderr_text;
};
using Runner = std::function<Ran(const QStringList& command, int timeout_ms)>;

// One page from the scanner, as the file's bytes (none found, stopped, failed: ScanError). `how` and `run` are for
// tests (by default: method() and a QProcess).
std::string scan(int dpi = 600, const QString& mode = QStringLiteral("gray"), const Runner& run = {}, int timeout_ms = 300000,
                 const std::optional<std::optional<QString>>& how = std::nullopt);

}  // namespace genko::app::scanner
