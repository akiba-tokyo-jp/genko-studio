// Python's genko/scanner.py.

#include "app/scanner.hpp"

#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

namespace genko::app::scanner {

namespace {

const QString kCannot = QStringLiteral("このパソコンでは、スキャナーから直接取り込めません。スキャンした画像をファイルに保存して読み込んでください");

Ran run_process(const QStringList& command, int timeout_ms) {
    Ran ran;
    QProcess process;
    process.setProgram(command.front());
    process.setArguments(command.mid(1));
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start();
    if (!process.waitForStarted(30000)) {
        ran.error = process.errorString();
        return ran;
    }
    process.closeWriteChannel();
    if (!process.waitForFinished(timeout_ms)) {
        process.kill();
        process.waitForFinished(5000);
        ran.error = QStringLiteral("%1 秒で終わりませんでした").arg(timeout_ms / 1000);
        return ran;
    }
    ran.stderr_text = QString::fromLocal8Bit(process.readAllStandardError());
    if (process.exitStatus() != QProcess::NormalExit) {
        ran.error = process.errorString();
        return ran;
    }
    ran.code = process.exitCode();
    return ran;
}

}  // namespace

std::optional<QString> method() {
#ifdef _WIN32
    if (!QStandardPaths::findExecutable(QStringLiteral("powershell")).isEmpty() ||
        !QStandardPaths::findExecutable(QStringLiteral("powershell.exe")).isEmpty())
        return QStringLiteral("wia");
    return std::nullopt;
#elif defined(__linux__)
    if (!QStandardPaths::findExecutable(QStringLiteral("scanimage")).isEmpty()) return QStringLiteral("sane");
    return std::nullopt;
#else
    return std::nullopt;
#endif
}

QStringList command(const QString& how, const QString& out, int dpi, const QString& mode) {
    if (how == QLatin1String("sane")) {
        const QString colour = mode == QLatin1String("color")     ? QStringLiteral("Color")
                               : mode == QLatin1String("lineart") ? QStringLiteral("Lineart")
                                                                  : QStringLiteral("Gray");
        return {QStringLiteral("scanimage"), QStringLiteral("--format=png"), QStringLiteral("--resolution=%1").arg(dpi),
                QStringLiteral("--mode=%1").arg(colour), QStringLiteral("--output-file=%1").arg(out)};
    }
    if (how == QLatin1String("wia")) {
        // the scanner's own window (WIA): the person picks the scanner, the area and the colour there
        // exit 3: no scanner (WIA says so by throwing), exit 2: the person closed the window
        const QString script = QStringLiteral("$d = New-Object -ComObject WIA.CommonDialog; "
                                              "try { $i = $d.ShowAcquireImage() } catch { exit 3 }; "
                                              "if ($i -eq $null) { exit 2 }; $i.SaveFile('%1')")
                                   .arg(out);
        return {QStringLiteral("powershell"), QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"), QStringLiteral("-Command"), script};
    }
    throw ScanError(kCannot);
}

std::string scan(int dpi, const QString& mode, const Runner& run, int timeout_ms, const std::optional<std::optional<QString>>& how_given) {
    const std::optional<QString> how = how_given ? *how_given : method();
    if (!how) throw ScanError(kCannot);
    QTemporaryDir folder;
    if (!folder.isValid()) throw ScanError(QStringLiteral("スキャンできませんでした: 一時フォルダーを作れません"));
    const QString out = folder.filePath(*how == QLatin1String("wia") ? QStringLiteral("scan.bmp") : QStringLiteral("scan.png"));
    const Ran done = run ? run(command(*how, out, dpi, mode), timeout_ms) : run_process(command(*how, out, dpi, mode), timeout_ms);
    if (!done.code) throw ScanError(QStringLiteral("スキャナーが応えませんでした（%1）").arg(done.error));
    if (*done.code == 3 && *how == QLatin1String("wia")) {
        throw ScanError(QStringLiteral("スキャナーが見つかりません（つないであるか、電源が入っているかを確かめてください。"
                                       "スキャンした画像のファイルなら「スキャン画像を線画にして取り込む…」で読めます）"));
    }
    if (*done.code == 2 && *how == QLatin1String("wia")) throw ScanError(QStringLiteral("スキャンをやめました"));
    if (*done.code != 0 || !QFileInfo::exists(out)) {
        const QStringList lines = done.stderr_text.trimmed().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        const QString detail = lines.isEmpty() ? QStringLiteral("スキャナーが見つからないか、使えません") : lines.back().trimmed();
        throw ScanError(QStringLiteral("スキャンできませんでした: %1").arg(detail));
    }
    QFile file(out);
    if (!file.open(QIODevice::ReadOnly)) throw ScanError(QStringLiteral("スキャンできませんでした: 読んだ画像を開けません"));
    const QByteArray bytes = file.readAll();
    return bytes.toStdString();
}

}  // namespace genko::app::scanner
