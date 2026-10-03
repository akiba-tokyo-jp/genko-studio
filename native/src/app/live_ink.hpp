#pragma once

#include <QImage>
#include <QRect>

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/brushes.hpp"
#include "core/geometry.hpp"
#include "core/json.hpp"
#include "core/model.hpp"
#include "render/brushes.hpp"
#include "render/image.hpp"

// The line being drawn, drawn the way it will print (Python's genko/app/live_ink.py): the same brush, pressure,
// colour and opacity, the same seed (the id the line will get), the layer's panel clip, in the page's pixels at the
// resolution the canvas shows — so the pixels of the live line are the pixels the page will have once it is committed.
//
// Brushes whose line is the union of its segments (round pens without grain or scatter) are added to piece by piece
// as the pen moves: only the new part is drawn, so a long line costs no more per move than a short one. The others
// (stamps, scatter, grain, airbrush, watercolour, light-touch shading) depend on the whole line and are drawn whole.
// The steadying (stabilize) moves only the last few points: the settled part is drawn once, the tail again each time.
// When the pen lifts, finish() draws the committed line (its smoothing, tapered ends and id) in their place.

namespace genko::app {

class LiveInk {
public:
    // doc: the book (kept while the line is shown: its brush colour, custom brushes, the page); page/layer: where the
    // line goes (in doc); dpi: the canvas's resolution for the page; fields: the add_stroke fields of the pen in hand;
    // seed: the id the line will get.
    LiveInk(std::shared_ptr<const core::Document> doc, const core::Page& page, const core::Layer& layer, int dpi,
            const core::Json& fields, std::string seed);

    // The line so far (packed points in mm with their pressure, and the pen's turns if it reports them).
    void follow(const core::PenPoints& points, std::span<const double> rotation = {});
    // The committed line, drawn whole in place of the live one.
    void finish(const core::Stroke& stroke);

    int dpi() const { return dpi_; }
    const std::string& seed() const { return seed_; }
    // The picture to show (premultiplied ARGB) and where it sits on the page (pixels at dpi()); empty when nothing yet.
    const QImage& image() const { return image_; }
    QRect box() const { return box_; }
    // The page box (mm) the line covers, for knowing which tiles it is drawn on.
    QRectF box_mm() const;
    // The line as the layer's picture (RGBA over box(), the renderer's own steps: transparent, the line's colour
    // composited, the panel clip): tests compare it with render::layer_image.
    render::Image patch() const;
    // Whether this pen's line is drawn piece by piece (else whole on every move).
    bool piecewise() const { return piecewise_; }
    // The page pixels (at dpi()) whose picture changed since the last call: the screen is painted again only there.
    QRect take_changed() {
        const QRect out = changed_;
        changed_ = QRect();
        return out;
    }
    // How many points of the line are drawn for good (the settled part).
    std::size_t drawn() const { return drawn_; }

    // The points as add_stroke will keep them, for what is known while the pen moves (steadying, pressure curves,
    // three decimals): `settled` is how many of them will not move any more.
    core::PenPoints as_committed(const core::PenPoints& raw, std::size_t& settled) const;

private:
    void add_coverage(const render::Image& mask, render::Point origin, bool replace);
    void set_tail(const std::optional<render::brushes::Coverage>& tail);
    void ensure_box(const QRect& wanted);
    void refresh(const QRect& part);
    render::Image clip_for(const render::Box& area) const;

    std::shared_ptr<const core::Document> doc_;
    const core::Page* page_ = nullptr;
    const core::Layer* layer_ = nullptr;
    int dpi_ = 96;
    render::Size size_;
    std::string seed_;
    std::string kind_;
    double width_mm_ = 0.5;
    std::vector<std::int64_t> rgb_;
    double opacity_ = 1.0;  // the stroke's opacity × the brush's
    double gamma_ = 1.0;
    std::string curve_;
    std::int64_t stabilize_ = 0;
    std::int64_t post_smooth_ = 0;
    double pressure_opacity_ = 0;
    bool piecewise_ = true;
    bool clip_ = false;
    std::optional<std::string> panel_;  // panel_each: the panel the line began in (none: all the panels)

    QRect box_;                    // the coverage's box on the page (px)
    render::Image coverage_;       // "L" over box_: the settled line
    std::optional<render::brushes::Coverage> tail_;  // the part the steadying still moves
    QImage image_;                 // what is shown (coverage and tail), premultiplied
    QRect changed_;                // page px drawn again since take_changed()
    std::size_t drawn_ = 0;
};

}  // namespace genko::app
