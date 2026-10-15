#pragma once

#include <QSettings>
#include <QString>
#include <QStringList>

#include <filesystem>
#include <vector>

class QWidget;

// The Python app's settings, taken over once (M6, COMP-01a). This app keeps its own things apart (core::config_dir:
// ~/.config/genko-studio, %APPDATA%\GenkoStudio); on the first start, when the Python app's are there (its settings
// QSettings("Genko", "Genko Studio") and its folder ~/.config/genko, %APPDATA%\genko), the person is asked whether to
// take them over — 環境設定, recent books, their own brushes, colours, poses, materials, actions, the AI keys, plugins —
// and they are copied only on yes. The Python app's things are only read: never changed, moved or removed.

namespace genko::app::migration {

// What is never taken: the books' recovery (the Python app's own crashes), caches, plugin runs' leftovers, locks and
// unfinished files.
bool kept_back(const std::filesystem::path& relative);

// What the Python app has that can be taken: its settings' keys and its folder's files (paths inside the folder).
struct Found {
    QStringList keys;
    std::vector<std::filesystem::path> files;
    bool empty() const { return keys.isEmpty() && files.empty(); }
};
Found find(const std::filesystem::path& legacy_dir, const QSettings* legacy);

// Copies what was found, never over what this app has already (a key it has, a file that is there): the keys into
// `settings`, the files into `dir` (each as a whole: a temporary file renamed into place, with the file's own
// permissions, so the AI keys stay the person's own). Returns what could not be copied (with why), nothing when all was.
QStringList copy(const Found& found, const std::filesystem::path& legacy_dir, const QSettings* legacy, const std::filesystem::path& dir,
                 QSettings& settings);

// Where things are (the app's own, or a test's).
struct Places {
    std::filesystem::path legacy_dir;
    const QSettings* legacy = nullptr;  // none: no settings to take
    std::filesystem::path dir;
    QSettings* settings = nullptr;
};

// The settings key that remembers the answer: "copied", "declined" or "none" (nothing to take).
inline constexpr const char* kDoneKey = "migration/done";

// Asks once (ask::question): nothing asked when the answer is remembered or there is nothing to take. On yes the
// things are copied; when some could not be, that is said (ask::warning) and the question comes again on the next
// start (what was copied stays: nothing is copied twice). Returns the answer remembered ("" when asked again later).
QString offer(QWidget* parent, const Places& places);

// offer() with the app's own places, on the first start; nothing (and "") when $GENKO_CONFIG_DIR is set (that folder
// is this app's own, chosen on purpose) or the two folders are the same.
QString offer_on_first_start(QWidget* parent = nullptr);

}  // namespace genko::app::migration
