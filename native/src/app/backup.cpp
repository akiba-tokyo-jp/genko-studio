// バックアップ (Python's genko/backup.py).

#include "app/backup.hpp"

#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>
#include <stdexcept>
#include <system_error>

#include "core/error.hpp"
#include "core/paths.hpp"
#include "render/zip.hpp"
#include "storage/fsutil.hpp"

namespace genko::app::backup {

namespace fs = std::filesystem;

namespace {

QString qpath(const fs::path& path) { return QString::fromStdString(core::path_to_utf8(path)); }

fs::path resolved(const fs::path& path) {
    std::error_code error;
    fs::path out = fs::weakly_canonical(fs::absolute(path, error), error);
    return error ? fs::absolute(path) : out;
}

// (Python's Path sorting: part by part)
bool path_before(const fs::path& a, const fs::path& b) {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
                                        [](const fs::path& x, const fs::path& y) { return x.native() < y.native(); });
}

}  // namespace

std::vector<fs::path> backups(const fs::path& folder, const QString& stem) {
    std::vector<fs::path> out;
    std::error_code error;
    if (!fs::is_directory(folder, error)) return out;
    const QRegularExpression name(QStringLiteral("^%1_.{8}-.{6}\\.zip$").arg(QRegularExpression::escape(stem)),
                                  QRegularExpression::UseUnicodePropertiesOption);
    for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
        if (name.match(qpath(it->path().filename())).hasMatch()) out.push_back(it->path());
    }
    std::sort(out.begin(), out.end(), path_before);
    return out;
}

fs::path make(const fs::path& project, const fs::path& folder, int keep, std::optional<QDateTime> now) {
    const fs::path book = resolved(project);
    const fs::path into = resolved(folder);
    for (fs::path at = into;; at = at.parent_path()) {
        if (at == book) throw std::invalid_argument("バックアップのフォルダーは、原稿のフォルダーの外にしてください");
        if (at == at.parent_path()) break;
    }
    std::error_code error;
    fs::create_directories(folder, error);
    if (error) throw core::Error("io", "cannot make " + core::path_to_utf8(folder) + ": " + error.message());
    const QString stem = qpath(project.stem());
    const QString stamp = now.value_or(QDateTime::currentDateTime()).toString(QStringLiteral("yyyyMMdd-HHmmss"));
    const fs::path out = folder / core::path_from_utf8((stem + QLatin1Char('_') + stamp + QStringLiteral(".zip")).toStdString());
    fs::path part = out;
    part.replace_extension(".zip.part");
    // (a part left by a backup of this book that was stopped — one runs at a time — is gone first; only this book's
    // name and a time: book "a" leaves book "a_b"'s part alone, which may be being written)
    const QRegularExpression stopped(QStringLiteral("^%1_\\d{8}-\\d{6}\\.zip\\.part$").arg(QRegularExpression::escape(stem)),
                                     QRegularExpression::UseUnicodePropertiesOption);
    for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
        if (stopped.match(qpath(it->path().filename())).hasMatch()) fs::remove(it->path(), error);
    }
    error.clear();
    // the book as one state: its project.json and journal the same before and after (a save by another program while
    // it is zipped: zipped again, never a backup that mixes two states)
    const auto state = [&project]() {
        std::error_code ignored;
        const fs::path journal = project / "studio" / "journal.jsonl";
        std::string sum;
        try {
            sum = storage::sha256_file(project / "project.json");
        } catch (const std::exception&) {
            sum = "missing";
        }
        return sum + ":" + std::to_string(fs::exists(journal, ignored) ? fs::file_size(journal, ignored) : 0);
    };
    for (int attempt = 0;; ++attempt) {
        const std::string before = state();
        // the book's files, in Python's order (rglob sorted), as "<book>/<path inside>"
        std::vector<fs::path> found;
        for (fs::recursive_directory_iterator it(project, error), end; !error && it != end; it.increment(error)) {
            const std::string name = core::path_to_utf8(it->path().filename());
            if (it->is_regular_file(error) && !name.ends_with(".tmp") && !name.ends_with(".lock")) found.push_back(it->path());
        }
        if (error) throw core::Error("io", "cannot read " + core::path_to_utf8(project) + ": " + error.message());
        std::sort(found.begin(), found.end(), path_before);
        std::vector<std::pair<std::string, fs::path>> files;
        const std::string top = core::path_to_utf8(project.filename().empty() ? project.parent_path().filename() : project.filename());
        for (const fs::path& file : found) {
            std::string inside = core::path_to_utf8(file.lexically_relative(project));
#ifdef _WIN32
            std::replace(inside.begin(), inside.end(), '\\', '/');  // (zip names part with "/", as ZipFile writes them)
#endif
            files.emplace_back(top + "/" + inside, file);
        }
        try {
            render::zip::pack(part, files);
        } catch (...) {
            fs::remove(part, error);
            if (state() != before && attempt < 2) continue;  // (a file went away while it was read: the book was saved)
            throw;
        }
        if (state() != before) {
            fs::remove(part, error);
            if (attempt < 2) continue;
            throw core::Error("io", "the book kept changing while it was being backed up");
        }
        break;
    }
    try {
        // (written through before it takes the name, and the oldest dropped only after: a power cut never leaves no
        // good backup)
        storage::truncate_durable(part, fs::file_size(part));
        storage::rename_new(part, out);
    } catch (...) {
        fs::remove(part, error);
        throw;
    }
    std::vector<fs::path> all = backups(folder, stem);
    const std::size_t kept = static_cast<std::size_t>(std::max(1, keep));
    for (std::size_t i = 0; all.size() > kept && i < all.size() - kept; ++i) fs::remove(all[i], error);
    return out;
}

bool due(const fs::path& folder, const QString& stem, double minutes, std::optional<QDateTime> now) {
    const std::vector<fs::path> last = backups(folder, stem);
    if (last.empty()) return true;
    const QDateTime written = QFileInfo(qpath(last.back())).lastModified();
    return written.msecsTo(now.value_or(QDateTime::currentDateTime())) / 1000.0 >= minutes * 60;
}

}  // namespace genko::app::backup
