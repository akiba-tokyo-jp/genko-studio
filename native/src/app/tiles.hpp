#pragma once

#include <filesystem>
#include <functional>
#include <optional>

#include <QColor>
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
#include "render/hdr_preview.hpp"
#include "render/proof.hpp"

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
//
// Over the page as drawn (display only, SPEC COLOR-03 PC1b): the proof (色の校正, render/proof), the gamut warning's
// mark (色域外警告: the colours the proof profile cannot print, counted) and the monitor's colours (モニターの
// プロファイル, display_color). The tiles keep the page as drawn and proofed (compose, the eyedropper); what is painted
// on screen with the mark and in the monitor's colours is kept beside it only while there is one (compose_shown).

namespace genko::app {

namespace display_color {
class Monitor;
}

using DocPtr = std::shared_ptr<const core::Document>;

// 色域外警告: how the colours out of the proof profile's gamut are marked (環境設定 → 透明部分・色域).
struct GamutLook {
    double threshold = render::proof::kDefaultThreshold;  // CIE ΔE 1976
    QColor color = QColor(128, 128, 128);
    double opacity = 1.0;
    bool operator==(const GamutLook& other) const { return threshold == other.threshold && color == other.color && opacity == other.opacity; }
};

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
    // 校正の設定 (render/proof::Setup): what the proof shows and through which profile (none: the export profile or the
    // plain conversion, Python's 「CMYK で見る」). The gamut warning checks against its profile too.
    void set_proof_setup(const render::proof::Setup& setup);
    const render::proof::Setup& proof_setup() const { return setup_; }
    // 色の校正: the page shown as the proof makes it (on), or as it is.
    void set_proof(bool on);
    bool cmyk_proof() const { return proof_on_; }
    // 色域外警告: the colours out of the proof profile's gamut marked, or (none) not.
    void set_gamut(std::optional<GamutLook> look);
    const std::optional<GamutLook>& gamut() const { return gamut_; }
    // How many pixels of the whole page's tiles drawn are out of gamut, of how many (both 0 without the warning).
    std::pair<std::size_t, std::size_t> gamut_count() const;
    // モニターのプロファイル: the screen's colours (none: sRGB, nothing done).
    void set_monitor(std::shared_ptr<const display_color::Monitor> monitor);
    // channels@1: the channel panel's view of the page (render::RenderOptions' planes, overlays and spots); another view
    // draws all of it again.
    struct ChannelView {
        std::optional<std::vector<bool>> planes;
        std::vector<std::string> overlays;
        std::optional<std::vector<std::string>> spots;
        bool operator==(const ChannelView&) const = default;
    };
    void set_channel_view(ChannelView view);
    const ChannelView& channel_view() const { return channel_view_; }
    // 網点を画面で見る (render.SCREEN_DOTS): the tones drawn as their dots on screen too.
    void set_screen_dots(bool on);
    bool screen_dots() const { return screen_dots_; }
    // Where the proof's profile is looked up each time tiles are drawn (Python's icc_setting: the one chosen last for
    // export, while its file is there; none: the plain conversion): one chosen or taken away meanwhile is followed.
    void set_proof_profile(std::function<std::optional<std::filesystem::path>()> profile) { proof_profile_ = std::move(profile); }
    // An animation page: the frame shown (its exposed cels only) and whether the frames around it show faint over it
    // (オニオンスキン, and the light table). A page that is not an animation is drawn as it is.
    void set_anim(std::int64_t frame, bool onion);
    std::int64_t anim_frame() const { return frame_; }
    // 32 bit プレビューオプション (PC5 H-a, render/hdr_preview): a 32-bit page shown through it (display only; another page
    // as it is). Its tiles are drawn again when it changes.
    void set_hdr(const render::HdrPreview& preview);
    const render::HdrPreview& hdr() const { return hdr_; }
    // 画素の段 (PC5 H-a): the page's own resolution (its paper's dpi) — the finest tiles there are; zoomed in past it, they
    // are laid on the screen pixel by pixel (nearest: each page pixel a square, the pixel grid on their edges).
    int pixel_dpi() const;
    // ピクセルアートのプレビュー: every tile laid on the screen nearest, enlarged or not (never smoothed).
    void set_pixel_art(bool on);
    bool pixel_art() const { return pixel_art_; }
    // GPU の記憶 (render:GPU-CPU-parity, PC5 H-a): while the page is drawn by the graphics card (a budget given), the tiles
    // it has been given as textures are counted (their bytes) and, past the budget, the ones drawn longest ago are taken
    // back from it (their pictures copied, so the card lets their textures go; drawn again, they go to it again). None:
    // drawn by the processor, nothing counted.
    void set_texture_budget(std::optional<std::size_t> bytes);
    std::optional<std::size_t> texture_budget() const { return texture_budget_; }
    // The bytes of the tiles on the card now (0 without a budget), and how many were taken back since the budget was set.
    std::size_t texture_bytes() const { return texture_bytes_; }
    std::size_t textures_released() const { return textures_released_; }
    // (after a frame is drawn on the card: the ones past the budget let go)
    void trim_textures();

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
    // As they are painted on screen: with the gamut warning's mark and in the monitor's colours.
    QImage compose_shown(int dpi) const;
    // The colour shown at this point of the page (the whole page's tiles: スポイト), when it is drawn.
    std::optional<QColor> pixel_at(double x_mm, double y_mm) const;

signals:
    // New pixels for this part of the page (mm; empty: all of it).
    void updated(const QRectF& area_mm);
    void omittedChanged(const QStringList& elements);
    void failed(const QString& message);
    // the first picture of a page is up (rough or not), and the fine one (timings)
    void firstShown();
    void settledChanged();
    // the gamut warning's counts changed (gamut_count)
    void gamutCounted();

private:
    struct Tile {
        QImage image;                 // RGB32, the tile's size (made at the first result)
        QImage shown;                 // as painted, when it differs (the gamut mark, the monitor's colours); else null
        QImage mask;                  // Grayscale8: 255 out of gamut (with the gamut warning); else null
        std::size_t out = 0;          // (the mask's pixels out of gamut)
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
    void redraw_all();

    DocPtr doc_;
    std::size_t index_ = 0;
    std::string page_id_;
    const core::Page* shown_page_ = nullptr;
    std::shared_ptr<const core::Page> page_ptr_;
    std::uint64_t generation_ = 0;
    std::string mode_ = "proof";
    render::proof::Setup setup_;  // set_proof_setup
    bool proof_on_ = false;       // set_proof
    std::optional<GamutLook> gamut_;
    std::shared_ptr<const display_color::Monitor> monitor_;
    bool screen_dots_ = false;                                    // set_screen_dots
    ChannelView channel_view_;                                    // set_channel_view
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
    render::HdrPreview hdr_;   // set_hdr
    bool pixel_art_ = false;   // set_pixel_art
    // (the tiles on the card: by their level and index, the frame each was drawn last and its bytes)
    struct OnCard {
        std::uint64_t frame = 0;
        std::size_t bytes = 0;
    };
    std::optional<std::size_t> texture_budget_;
    mutable std::map<std::pair<int, int>, OnCard> on_card_;
    mutable std::size_t texture_bytes_ = 0;
    mutable std::uint64_t card_frame_ = 0;
    std::size_t textures_released_ = 0;
    void forget_on_card(int dpi, int index);
    struct OnionStore;
    std::shared_ptr<OnionStore> onions_;  // (the onion skin of the frame shown, made once for all its tiles)
    QThreadPool pool_;
    std::shared_ptr<PageRenderer*> self_;  // (results find the renderer through this; nulled when it goes)
};

}  // namespace genko::app
