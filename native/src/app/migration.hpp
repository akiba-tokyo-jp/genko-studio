#pragma once

#include <QSettings>
#include <QString>
#include <QStringList>

#include <filesystem>
#include <functional>
#include <vector>

class QWidget;

// The Python app's settings, taken over once (M6, COMP-01a). This app keeps its own things apart (core::config_dir:
// ~/.config/genko-studio, %APPDATA%\GenkoStudio); on the first start, when the Python app's are there (its settings
// QSettings("Genko", "Genko Studio") and its folder ~/.config/genko, %APPDATA%\genko), the person is asked whether to
// take them over — 環境設定, recent books, their own brushes, colours, poses, materials, actions, the AI keys, plugins —
// and they are copied only on yes. The Python app's things are only read: never changed, moved or removed — nor
// reachable through the copies (a link is never copied as a link: the file it points to is copied, a folder it points
// to is left and said).

namespace genko::app::migration {

// What is never taken: the books' recovery (the Python app's own crashes), caches (cache/, thumbs/, fonts.json), plugin
// runs' leftovers, locks and unfinished files.
bool kept_back(const std::filesystem::path& relative);

// What the Python app has that can be taken: its settings' keys and its folder's files (paths inside the folder; a
// link to a file is one of them, its file copied), the links to folders (left, and said), and what could not be read.
struct Found {
    QStringList keys;
    std::vector<std::filesystem::path> files;
    std::vector<std::filesystem::path> folder_links;
    QStringList unreadable;  // (with why: the walk stopped there, or a name that cannot be read)
    bool empty() const { return keys.isEmpty() && files.empty() && folder_links.empty() && unreadable.isEmpty(); }
};
Found find(const std::filesystem::path& legacy_dir, const QSettings* legacy);

// What a copy did: what could not be copied (with why: then it is asked again), and what was left because this app
// has its own already (said, not a failure).
struct Copied {
    QStringList failed;
    QStringList own_kept;
};

// Copies what was found, never over what this app has already (a key it has, a file that is there): the keys into
// `settings`, the files into `dir` (each as a whole: a temporary file renamed into place, with the file's own
// permissions, so the AI keys stay the person's own; the new folder with the old folder's permissions). `progress`
// (files done, all files) is called as it goes. What `found` could not read is among the failures.
Copied copy(const Found& found, const std::filesystem::path& legacy_dir, const QSettings* legacy, const std::filesystem::path& dir,
            QSettings& settings, const std::function<void(int, int)>& progress = {});

// Where things are (the app's own, or a test's).
struct Places {
    std::filesystem::path legacy_dir;
    const QSettings* legacy = nullptr;  // none: no settings to take
    std::filesystem::path dir;
    QSettings* settings = nullptr;
};

// The settings key that remembers the answer: "copied", "declined" or "none" (nothing to take).
inline constexpr const char* kDoneKey = "migration/done";

// Asks once (ask::question): nothing asked when the answer is remembered or there is nothing to take, nor while another
// start of the app is asking (a lock in this app's folder). On yes the things are copied (a progress window when it
// takes long); what was left (folders reached through a link, files this app has already) is said (ask::warning);
// when some could not be copied that is said and the question comes again on the next start (what was copied stays:
// nothing is copied twice). Returns the answer remembered ("" when asked again later or not asked now).
QString offer(QWidget* parent, const Places& places);

// offer() with the app's own places, on the first start; nothing (and "") when $GENKO_CONFIG_DIR is set (that folder
// is this app's own, chosen on purpose) or the two folders are the same.
QString offer_on_first_start(QWidget* parent = nullptr);

}  // namespace genko::app::migration
