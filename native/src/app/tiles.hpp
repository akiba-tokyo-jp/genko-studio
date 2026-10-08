#pragma once

#include <filesystem>
#include <functional>
#include <optional>

#include <QImage>
#include <QObject>
#include <QRect>
#include <QRectF>
#include <QStringList>
#include <QThreadPool>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "core/model.hpp"

class QPainter;

// The page on screen, drawn in tiles (512 × 512 pixels at the resolution shown) by worker threads with the renderer's
// part-of-a-page drawing (render_page with a region: the same pixels as the whole page cut). The whole page at the
// base resolution (up to 220 dpi), and when zoomed in further the tiles in sight at the finer one (up to 432 dpi):
// 精細化. A change to the page marks only the tiles it touched (a line's own box at each resolution; anything else:
// the whole page) and only those are drawn again. Every result carries the generation of the page it was drawn from:
// one older than the tile's last change is thrown away, so an old picture never covers a newer line. A page seen for
// the first time with many lines shows a rough picture first (plain polylines) and then the real one.
//
// Things this build does not draw yet are left out (skip_unported) and reported (omittedChanged), never silently.

namespace genko::app {

using DocPtr = std::shared_ptr<const core::Document>;

class PageRenderer : public QObject {
    Q_OBJECT

public:
    static constexpr int kTile = 512;
    static constexpr int kBaseDpi = 220;
    static constexpr int kDetailDpi = 432;

    explicit PageRenderer(QObject* parent = nullptr);
    ~PageRenderer() override;

    // The page to show (the book it belongs to, and its position). A different page starts again; the same page in
    // a newer book is compared with the one shown and only what changed is drawn again.
    void show(DocPtr doc, std::size_t index);
    void clear();
    // proof (the page as it prints, the name lines in blue) | name
    void set_mode(const std::string& mode);
    // 色校正: the page as it will print in CMYK (render/colour::proof through the profile, or the plain conversion
    // without one), or as it is (nothing).
    void set_cmyk_proof(std::optional<std::optional<std::filesystem::path>> proof);
    bool cmyk_proof() const { return proof_.has_value(); }
    // Where the proof's profile is looked up each time tiles are drawn (Python's icc_setting: the one chosen last for
    // export, while its file is there; none: the plain conversion): one chosen or taken away meanwhile is followed.
    void set_proof_profile(std::function<std::optional<std::filesystem::path>()> profile) { proof_profile_ = std::move(profile); }
    // An animation page: the frame shown (its exposed cels only) and whether the frames around it show faint over it
    // (オニオンスキン, and the light table). A page that is not an animation is drawn as it is.
    void set_anim(std::int64_t frame, bool onion);
    std::int64_t anim_frame() const { return frame_; }

    // What the view needs: the resolution of the whole page (base) and the one wanted (finer when zoomed in), and
    // the part of the page in sight (mm).
    void want(int base_dpi, int wanted_dpi, const QRectF& visible_mm);

    // Draw the tiles there are (painter in page mm: the caller's transform maps mm to the screen).
    void paint(QPainter& painter, const QRectF& visible_mm) const;
    // The tiles of this resolution under `rect_mm` show the page as it is now (and not its rough picture).
    bool current(int dpi, const QRectF& rect_mm) const;
    // Every tile wanted now is current (tests and timings).
    bool settled() const;
    // The resolution of the finest tiles drawn for the part in sight.
    int shown_dpi() const;
    std::uint64_t generation() const { return generation_; }
    // Some picture of the page is there to show.
    bool any_shown() const { return first_shown_; }
    const QStringList& omitted() const { return omitted_; }
    const DocPtr& doc() const { return doc_; }
    const core::Page* page() const;
    // The page's pixels at `dpi` composed from the tiles there are (tests: compare with render_page).
    QImage compose(int dpi) const;

signals:
    // New pixels for this part of the page (mm; empty: all of it).
    void updated(const QRectF& area_mm);
    void omittedChanged(const QStringList& elements);
    void failed(const QString& message);
    // the first picture of a page is up (rough or not), and the fine one (timings)
    void firstShown();
    void settledChanged();

private:
    struct Tile {
        QImage image;                 // RGB32, the tile's size (made at the first result)
        std::uint64_t valid = 0;      // the generation its pixels show (0: none yet)
        std::uint64_t changed = 0;    // the newest generation that changed it
        QRect pending;                // what is to be drawn again (tile coordinates); empty: nothing
        bool rough = false;           // its pixels are the rough first look
        bool want_rough = false;
        bool running = false;
        std::shared_ptr<std::stop_source> stop;
    };
    struct Level {
        int dpi = 0;
        QSize size;  // the page in pixels at dpi
        int cols = 0;
        int rows = 0;
        bool whole = false;  // every tile is wanted (the base); else only the ones in sight
        std::vector<Tile> tiles;
        std::vector<int> wanted;  // tile indices wanted now, most needed first
    };
    struct Result;

    Level& level(int dpi, bool whole);
    QRect tile_rect(const Level& level, int index) const;
    void mark(Level& level, const QRect& px, std::uint64_t generation);
    void dispatch();
    void deliver(const Result& result);
    void report_omitted(const std::vector<std::string>& elements);

    DocPtr doc_;
    std::size_t index_ = 0;
    std::string page_id_;
    const core::Page* shown_page_ = nullptr;
    std::shared_ptr<const core::Page> page_ptr_;
    std::uint64_t generation_ = 0;
    std::string mode_ = "proof";
    std::optional<std::optional<std::filesystem::path>> proof_;  // set_cmyk_proof
    std::function<std::optional<std::filesystem::path>()> proof_profile_;
    std::map<int, Level> levels_;
    int base_dpi_ = 0;
    int wanted_dpi_ = 0;
    QRectF visible_mm_;
    int running_ = 0;
    bool first_shown_ = false;
    bool rough_first_ = false;
    QStringList omitted_;
    std::int64_t frame_ = 1;
    bool onion_ = true;
    struct OnionStore;
    std::shared_ptr<OnionStore> onions_;  // (the onion skin of the frame shown, made once for all its tiles)
    QThreadPool pool_;
    std::shared_ptr<PageRenderer*> self_;  // (results find the renderer through this; nulled when it goes)
};

}  // namespace genko::app
