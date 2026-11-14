// fonts@1 (render/font_files.hpp): where the added faces' files are, whether they are there, and their bytes.

#include "render/font_files.hpp"

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>

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
    std::map<std::string, bool, std::less<>> there;
    std::map<std::string, Kept, std::less<>> kept;
    std::uint64_t kept_bytes = 0;
    std::uint64_t tick = 0;
};

State& state() {
    static State s;
    return s;
}

std::string dir_locked(const State& s) {
    if (s.testing_dir) return *s.testing_dir;
    if (QCoreApplication::instance() == nullptr) return {};
    const QString beside = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("../share/genko-studio/fonts"));
    return QDir::cleanPath(beside).toStdString();
}

QString file_in(const std::string& folder, const core::fonts::Entry& entry) {
    return QDir(QString::fromStdString(folder)).filePath(QString::fromStdString(entry.file));
}

bool regular_of_size(const QString& file, std::int64_t bytes) {
    const QFileInfo info(file);
    return info.isFile() && info.size() == bytes;
}

}  // namespace

std::string dir() {
    State& s = state();
    const std::lock_guard<std::mutex> lock(s.mutex);
    return dir_locked(s);
}

void set_dir_for_testing(const std::optional<std::string>& dir) {
    State& s = state();
    const std::lock_guard<std::mutex> lock(s.mutex);
    s.testing_dir = dir;
    s.there.clear();
    s.kept.clear();
    s.kept_bytes = 0;
}

std::string path(std::string_view key) {
    const core::fonts::Entry* entry = core::fonts::find(key);
    if (entry == nullptr || !entry->added) return {};
    const std::string folder = dir();
    if (folder.empty()) return {};
    return file_in(folder, *entry).toStdString();
}

bool present(std::string_view key) {
    const core::fonts::Entry* entry = core::fonts::find(key);
    if (entry == nullptr || !entry->added) return false;
    State& s = state();
    std::string folder;
    {
        const std::lock_guard<std::mutex> lock(s.mutex);
        if (const auto it = s.there.find(key); it != s.there.end()) return it->second;
        folder = dir_locked(s);
    }
    // (before there is a program to be beside, nothing is learnt: asked again once there is)
    if (folder.empty()) return false;
    const bool there = regular_of_size(file_in(folder, *entry), entry->bytes);
    const std::lock_guard<std::mutex> lock(s.mutex);
    if (dir_locked(s) != folder) return there;  // (the folder changed meanwhile — tests: this answer is not kept)
    return s.there.emplace(std::string(key), there).first->second;
}

std::shared_ptr<const QByteArray> bytes(std::string_view key) {
    const core::fonts::Entry* entry = core::fonts::find(key);
    if (entry == nullptr || !entry->added || !present(key)) throw NotYetPorted("default_font");
    State& s = state();
    std::string folder;
    {
        const std::lock_guard<std::mutex> lock(s.mutex);
        if (const auto it = s.kept.find(key); it != s.kept.end()) {
            it->second.used = ++s.tick;
            return it->second.bytes;
        }
        folder = dir_locked(s);
    }
    // (read outside the lock: two readers of one face at once both read it, and the first kept is shared from then on)
    QFile source(file_in(folder, *entry));
    if (!source.open(QIODevice::ReadOnly | QIODevice::Unbuffered)) throw NotYetPorted("default_font");
    if (source.size() != entry->bytes) throw NotYetPorted("default_font");
    auto read = std::make_shared<QByteArray>(source.readAll());
    if (read->size() != entry->bytes) throw NotYetPorted("default_font");
    const std::lock_guard<std::mutex> lock(s.mutex);
    if (dir_locked(s) != folder) return read;  // (tests changed the folder meanwhile: this one is the caller's alone)
    if (const auto it = s.kept.find(key); it != s.kept.end()) {
        it->second.used = ++s.tick;
        return it->second.bytes;
    }
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
    return read;
}

std::vector<std::string> missing() {
    std::vector<std::string> out;
    for (const core::fonts::Entry& entry : core::fonts::entries()) {
        if (entry.added && !present(entry.key)) out.push_back(entry.key);
    }
    return out;
}

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
