// fonts@1 (render/font_files.hpp): where the added faces' files are, whether they are the catalogue's, and their bytes.

#include "render/font_files.hpp"

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>

#include <atomic>
#include <map>
#include <mutex>

#include "core/font_catalog.hpp"
#include "render/not_yet_ported.hpp"

namespace genko::render::font_files {

namespace {

struct Kept {
    std::shared_ptr<const QByteArray> bytes;
    std::uint64_t used = 0;  // (the tick of its last use)
};

struct State {
    std::mutex mutex;
    std::optional<std::string> testing_dir;
    std::optional<std::string> program_dir;  // (found out once: QCoreApplication is asked no more than that)
    std::map<std::string, bool, std::less<>> installed;  // (a face's verdict, once reached)
    std::map<std::string, Kept, std::less<>> kept;
    std::uint64_t kept_bytes = 0;
    std::uint64_t tick = 0;
    std::uint64_t epoch = 0;  // (moves with the folder: a file read for another folder is not kept or judged)
};

State& state() {
    static State s;
    return s;
}

std::atomic<std::uint64_t>& generation_counter() {
    static std::atomic<std::uint64_t> n{0};
    return n;
}

std::string dir_locked(State& s) {
    if (s.testing_dir) return *s.testing_dir;
    if (s.program_dir) return *s.program_dir;
    if (QCoreApplication::instance() == nullptr) return {};
    const QString program = QCoreApplication::applicationDirPath();
    if (program.isEmpty()) return {};  // (never a folder read against the working folder)
    s.program_dir = QDir::cleanPath(QDir(program).filePath(QStringLiteral("../share/genko-studio/fonts"))).toStdString();
    return *s.program_dir;
}

QString file_in(const std::string& folder, const core::fonts::Entry& entry) {
    return QDir(QString::fromStdString(folder)).filePath(QString::fromStdString(entry.file));
}

bool regular_of_size(const QString& file, std::int64_t bytes) {
    const QFileInfo info(file);
    return info.isFile() && info.size() == bytes;
}

// The file read whole when it is the catalogue's: of its size (no more than that is read) and of its SHA-256.
std::shared_ptr<const QByteArray> read_checked(const QString& file, const core::fonts::Entry& entry) {
    if (!regular_of_size(file, entry.bytes)) return nullptr;
    QFile source(file);
    if (!source.open(QIODevice::ReadOnly | QIODevice::Unbuffered)) return nullptr;
    auto read = std::make_shared<QByteArray>(source.read(static_cast<qint64>(entry.bytes) + 1));
    if (static_cast<std::int64_t>(read->size()) != entry.bytes) return nullptr;
    if (QCryptographicHash::hash(*read, QCryptographicHash::Sha256).toHex().toStdString() != entry.sha256) return nullptr;
    return read;
}

void keep_locked(State& s, std::string_view key, const std::shared_ptr<const QByteArray>& read) {
    const auto size = static_cast<std::uint64_t>(read->size());
    // the least lately used let go until this one fits (their users keep theirs)
    while (!s.kept.empty() && s.kept_bytes + size > kCacheBytes) {
        auto oldest = s.kept.begin();
        for (auto it = s.kept.begin(); it != s.kept.end(); ++it) {
            if (it->second.used < oldest->second.used) oldest = it;
        }
        s.kept_bytes -= static_cast<std::uint64_t>(oldest->second.bytes->size());
        s.kept.erase(oldest);
    }
    if (size <= kCacheBytes) {
        s.kept.emplace(std::string(key), Kept{read, ++s.tick});
        s.kept_bytes += size;
    }
}

// The face's bytes from its file, judged: kept and returned when they are the catalogue's; nothing, and the face not
// installed from now on, when they are not (generation() moves if it was installed before).
std::shared_ptr<const QByteArray> load(const core::fonts::Entry& entry) {
    State& s = state();
    std::string folder;
    std::uint64_t epoch = 0;
    {
        const std::lock_guard<std::mutex> lock(s.mutex);
        if (const auto verdict = s.installed.find(entry.key); verdict != s.installed.end() && !verdict->second) return nullptr;
        if (const auto it = s.kept.find(entry.key); it != s.kept.end()) {
            it->second.used = ++s.tick;
            return it->second.bytes;
        }
        folder = dir_locked(s);
        epoch = s.epoch;
    }
    // (before there is a program to be beside, nothing is learnt: asked again once there is)
    if (folder.empty()) return nullptr;
    // (read outside the lock: two readers of one face at once both read it, and the first kept is shared from then on)
    std::shared_ptr<const QByteArray> read = read_checked(file_in(folder, entry), entry);
    const std::lock_guard<std::mutex> lock(s.mutex);
    if (s.epoch != epoch) return read;  // (tests changed the folder meanwhile: this answer is the caller's alone)
    if (!read) {
        const auto was = s.installed.find(entry.key);
        const bool lost = was != s.installed.end() && was->second;
        s.installed[entry.key] = false;
        if (lost) ++generation_counter();
        return nullptr;
    }
    if (const auto verdict = s.installed.find(entry.key); verdict != s.installed.end() && !verdict->second) return nullptr;
    s.installed[entry.key] = true;
    if (const auto it = s.kept.find(entry.key); it != s.kept.end()) {
        it->second.used = ++s.tick;
        return it->second.bytes;
    }
    keep_locked(s, entry.key, read);
    return read;
}

const core::fonts::Entry* added_entry(std::string_view key) {
    const core::fonts::Entry* entry = core::fonts::find(key);
    return entry != nullptr && entry->added ? entry : nullptr;
}

}  // namespace

std::string dir() {
    State& s = state();
    const std::lock_guard<std::mutex> lock(s.mutex);
    return dir_locked(s);
}

void prime() { (void)dir(); }

void set_dir_for_testing(const std::optional<std::string>& dir) {
    State& s = state();
    const std::lock_guard<std::mutex> lock(s.mutex);
    s.testing_dir = dir;
    s.installed.clear();
    s.kept.clear();
    s.kept_bytes = 0;
    ++s.epoch;
    ++generation_counter();
}

std::string path(std::string_view key) {
    const core::fonts::Entry* entry = added_entry(key);
    if (entry == nullptr) return {};
    const std::string folder = dir();
    if (folder.empty()) return {};
    return file_in(folder, *entry).toStdString();
}

bool file_there(std::string_view key) {
    const core::fonts::Entry* entry = added_entry(key);
    if (entry == nullptr) return false;
    State& s = state();
    std::string folder;
    {
        const std::lock_guard<std::mutex> lock(s.mutex);
        if (const auto verdict = s.installed.find(key); verdict != s.installed.end() && !verdict->second) return false;
        folder = dir_locked(s);
    }
    return !folder.empty() && regular_of_size(file_in(folder, *entry), entry->bytes);
}

std::vector<std::string> files_not_there() {
    std::vector<std::string> out;
    for (const core::fonts::Entry& entry : core::fonts::entries()) {
        if (entry.added && !file_there(entry.key)) out.push_back(entry.key);
    }
    return out;
}

bool present(std::string_view key) {
    const core::fonts::Entry* entry = added_entry(key);
    if (entry == nullptr) return false;
    {
        State& s = state();
        const std::lock_guard<std::mutex> lock(s.mutex);
        if (const auto verdict = s.installed.find(key); verdict != s.installed.end()) return verdict->second;
    }
    return load(*entry) != nullptr;
}

std::shared_ptr<const QByteArray> bytes(std::string_view key) {
    const core::fonts::Entry* entry = added_entry(key);
    if (entry == nullptr) throw NotYetPorted("default_font");
    std::shared_ptr<const QByteArray> read = load(*entry);
    if (!read) throw NotYetPorted("default_font");
    return read;
}

std::vector<std::string> missing() {
    std::vector<std::string> out;
    for (const core::fonts::Entry& entry : core::fonts::entries()) {
        if (entry.added && !present(entry.key)) out.push_back(entry.key);
    }
    return out;
}

std::uint64_t generation() { return generation_counter().load(); }

std::vector<Checked> verify(bool hashes) {
    const std::string folder = dir();
    std::vector<Checked> out;
    for (const core::fonts::Entry& entry : core::fonts::entries()) {
        if (!entry.added) continue;
        Checked c;
        c.key = entry.key;
        c.file = entry.file;
        if (!folder.empty()) {
            const QString file = file_in(folder, entry);
            const QFileInfo info(file);
            c.exists = info.isFile();
            c.size_ok = c.exists && info.size() == entry.bytes;
            if (hashes && c.size_ok) {
                QFile source(file);
                if (source.open(QIODevice::ReadOnly)) {
                    QCryptographicHash hash(QCryptographicHash::Sha256);
                    if (hash.addData(&source)) c.hash_ok = hash.result().toHex().toStdString() == entry.sha256;
                }
            }
        }
        out.push_back(std::move(c));
    }
    return out;
}

std::uint64_t cached_bytes() {
    State& s = state();
    const std::lock_guard<std::mutex> lock(s.mutex);
    return s.kept_bytes;
}

void clear_cache() {
    State& s = state();
    const std::lock_guard<std::mutex> lock(s.mutex);
    s.kept.clear();
    s.kept_bytes = 0;
}

}  // namespace genko::render::font_files
