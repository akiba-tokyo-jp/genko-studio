#pragma once

#include <QImage>
#include <QObject>
#include <QThreadPool>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "core/model.hpp"

// Small pictures of pages, kept on disk (SPEC PERF-01: user material previews in a lasting cache, made when needed):
// the key is the page's content hash, the drawing's version and how it is shown (height, mode), so a picture is
// drawn once for a content and found again after a restart; the folder (<config>/cache/thumbs) is kept under a
// size and a count, the least recently used going first. Pictures are made on a worker thread, only when asked for
// (the page list asks for the rows in sight): a book's pages are never all drawn when it opens.

namespace genko::app {

using DocPtr = std::shared_ptr<const core::Document>;

// The renderer's version for cached pictures (a change to drawing makes all of them new).
inline constexpr const char* kThumbVersion = "genko-native-render/1";

// A hash of everything a page's picture depends on (its paper, panels, layers and their lines and pictures, the
// book's own brushes), as 64 hex digits.
std::string page_fingerprint(const core::Document& doc, const core::Page& page);

class ThumbCache {
public:
    explicit ThumbCache(std::filesystem::path root, std::size_t max_files = 4000, std::uint64_t max_bytes = 256ull << 20);

    static std::string key(const core::Document& doc, const core::Page& page, int height, const std::string& mode);
    std::filesystem::path file(const std::string& key) const;
    std::optional<QImage> get(const std::string& key) const;  // (found: marked as used now)
    void put(const std::string& key, const QImage& image) const;
    // Remove the least recently used pictures beyond the limits; returns how many went.
    std::size_t trim() const;

    const std::filesystem::path& root() const { return root_; }

private:
    std::filesystem::path root_;
    std::size_t max_files_;
    std::uint64_t max_bytes_;
};

// Makes the pictures on a worker thread and hands them back on the GUI thread.
class ThumbMaker : public QObject {
    Q_OBJECT

public:
    explicit ThumbMaker(QObject* parent = nullptr);
    ~ThumbMaker() override;

    // A picture of doc->page(index) `height` pixels tall; done(page id, picture, from the cache).
    void request(DocPtr doc, std::size_t index, int height, const std::string& mode);
    // Drop the requests not started yet (a new book, a new order).
    void cancel_waiting();
    ThumbCache& cache() { return cache_; }
    // How many pictures were drawn (not found in the cache) since it was made.
    int drawn() const { return drawn_; }

signals:
    void done(const QString& page_id, int height, const QImage& image, bool cached);

private:
    ThumbCache cache_;
    QThreadPool pool_;
    std::shared_ptr<ThumbMaker*> self_;
    int drawn_ = 0;
};

}  // namespace genko::app
