#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QPointF>
#include <QPointer>
#include <QTimer>
#include <QVector>
#include <QWidget>

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/frame_tools.hpp"
#include "app/live_ink.hpp"
#include "app/tiles.hpp"
#include "core/geometry.hpp"
#include "core/json.hpp"
#include "core/model.hpp"

class QKeyEvent;
class QVariantAnimation;

// The page canvas (Python's genko/app/canvas.py PageCanvas): the page as it will print (drawn by the renderer in
// tiles on worker threads) with light overlays on top — the bleed, trim line and basic frame, the reading order of
// the panels, the chosen panel, the panel tool's gutters, corners and bows, the line being drawn.
//
// Tools: 選択 (click a panel to choose it, drag elsewhere to move the view), レイヤー移動, ペン and 消しゴム
// (the pen's lines are cut where it passes), コマ割り (drag across a panel to cut it — level and upright cuts snap,
// Alt for a free angle —, drag a gutter, a chosen panel's corners and the ◇ on its edges; or draw new panels:
// rect, poly, free), 範囲選択 (canvas_select.cpp: a rectangle, an ellipse, a lasso, a polyline, auto-select, colour,
// the selection pen and eraser; Shift adds, Alt takes away; the area moved, scaled, turned, slanted or pulled freely
// by its handles) and 虫めがね. The view fits the page when it opens; Ctrl+wheel or a pinch zooms, the wheel or two
// fingers scroll, Space+drag or the middle button pans (a page let go while moving slides on and slows down),
// Shift+Space+drag turns the view; the view can be mirrored either way. The page itself never turns.

namespace genko::app {

struct ViewState {
    double scale = 2.4;  // screen px per mm
    QPointF pan{20, 20};
    bool fitted = true;
    double rotation = 0.0;
    bool flipped = false;
    bool flipped_v = false;
};

// A line drawn on the canvas: its points as packed (mm, pressure), the pen's turns, and the id it is drawn with.
struct StrokeInput {
    core::PenPoints points;
    std::vector<double> rotation;
    std::string id;
    QString tool;  // pen | eraser
};

class PageCanvas : public QWidget {
    Q_OBJECT

public:
    static constexpr double kMinScale = 0.3;
    static constexpr double kMaxScale = 12.0;
    static constexpr int kHandlePx = 7;

    explicit PageCanvas(QWidget* parent = nullptr);
    ~PageCanvas() override;

    // --- the page --------------------------------------------------------------------------------------------
    // The page to show: doc->page(index). The same page in a newer book draws again only what changed.
    void set_page(DocPtr doc, std::size_t index);
    void clear_page();
    const core::Page* page() const;
    const DocPtr& doc() const { return doc_; }
    std::size_t page_index() const { return index_; }
    PageRenderer& renderer() { return *renderer_; }
    const PageRenderer& renderer() const { return *renderer_; }
    void set_render_mode(const std::string& mode);

    // --- tools ----------------------------------------------------------------------------------------------
    const QString& tool() const { return tool_; }
    void set_tool(const QString& tool);
    // The pen in hand (add_stroke fields) and the layer it draws on (none: no live line, a plain guide line).
    void set_live_pen(const core::Json& fields, std::optional<std::string> layer_id);
    const core::Json& live_pen() const { return pen_fields_; }
    double brush_width_mm = 0.35;
    double eraser_mm = 2.0;
    QString frame_mode = QStringLiteral("cut");  // cut | rect | poly | free
    bool show_guides = true;
    bool show_frame_numbers = false;
    core::Binding binding = core::Binding::Right;
    // --- the selection (範囲選択: the marquee tool) --------------------------------------------------------------
    // The area chosen (an op's area: {"poly"} or {"mask"}, mm) and its outline on the screen (a mask's box).
    struct Selection {
        core::Json area;
        std::vector<QPointF> outline;
    };
    void set_selection(std::optional<core::Json> area, std::optional<std::vector<QPointF>> outline = std::nullopt);
    const std::optional<Selection>& selection() const { return selection_; }
    // How the marquee tool chooses: rect | ellipse | lasso | polyline | wand | color | pen | erase (選択ペン・選択消し).
    QString marquee = QStringLiteral("rect");
    double selection_pen_mm = 4.0;
    bool quick_mask = false;  // the selection shown in red where it is not, to be painted with the selection pen
    bool pivot_mode = false;  // 基準位置を動かす: the next press with the marquee tool puts the pivot there
    // How a new area joins the selection: Shift adds, Alt takes away, both keep the overlap.
    static QString how_from(Qt::KeyboardModifiers modifiers);
    const QString& selection_how() const { return sel_how_; }
    // The box (x0, y0, x1, y1 mm) of the outline, and where it turns and scales about (its middle unless moved).
    std::array<double, 4> selection_box() const;
    QPointF selection_pivot() const;
    // 自由変形: pull the corners (perspective) or a grid of points (mesh, columns × rows cells); Enter applies, Esc
    // stops.
    bool start_warp(const QString& kind, int columns = 2, int rows = 2);
    void finish_warp();
    void cancel_warp();
    bool warping() const { return warp_.has_value(); }
    // The polyline selection's corners: Enter or a double click closes it; Esc forgets them.
    bool finish_points();
    bool cancel_points();
    // The lines and the point chosen with the vector tool (M3), for 選んだ点を太く.
    std::vector<std::string> vector_ids;
    std::optional<int> vector_point;
    // The moving layer's picture (QImage and where it sits, mm), given by the window when a layer move starts.
    void set_move_image(const QImage& image, const QRectF& where_mm);

    // The line with this id is on the page now: it is shown exactly as the page will be until its tiles are drawn.
    void stroke_applied(const std::string& id);
    // The line could not be applied: its live picture goes.
    void stroke_dropped();

    // --- the view -------------------------------------------------------------------------------------------
    double scale() const { return view_.scale; }
    QPointF pan() const { return view_.pan; }
    bool fitted() const { return view_.fitted; }
    double rotation() const { return view_.rotation; }
    bool flipped() const { return view_.flipped; }
    bool flipped_vertical() const { return view_.flipped_v; }
    void fit_page();
    void zoom_by(double factor, std::optional<QPointF> anchor = std::nullopt);
    void actual_size();
    void center_on(double x_mm, double y_mm);
    void rotate_view(double degrees);
    void set_rotation(double degrees);
    void flip_view(std::optional<bool> on = std::nullopt);
    void flip_view_vertical(std::optional<bool> on = std::nullopt);
    void reset_view();
    void set_zoom_percent(double percent);
    int zoom_percent() const;
    void zoom_to_rect(double x0, double y0, double x1, double y1);
    // Change the view at once and draw the way there over a moment (unless motion is reduced).
    void glide(const std::function<void()>& change);
    ViewState view_state() const { return view_; }
    void set_view_state(const ViewState& state);
    // A point on the widget as page mm (through the view's turn and mirror), and back.
    QPointF to_mm(const QPointF& widget) const;
    QPointF to_widget(const QPointF& mm) const;
    // The part of the page in sight (mm).
    QRectF seen_mm() const;
    // The resolution that matches the zoom (one page pixel per screen pixel, up to the detail), and the whole page's.
    int wanted_dpi() const;
    int base_dpi() const { return std::min(wanted_dpi(), PageRenderer::kBaseDpi); }
    // A two-finger gesture: spread to zoom about the fingers, twist to turn the view, slide to move.
    void pinch(double factor, double turn_deg, const QPointF& centre, const QPointF& moved);

    // --- tests ----------------------------------------------------------------------------------------------
    // Wait (processing events) until every tile wanted now is drawn for the page as it is now.
    bool wait_rendered(int ms);
    const LiveInk* live() const { return live_.get(); }
    std::size_t overlays() const { return overlays_.size(); }
    // The picture of a line committed a moment ago, shown until its tiles are drawn (null: none at i).
    const LiveInk* overlay(std::size_t i) const { return i < overlays_.size() ? overlays_[i].ink.get() : nullptr; }
    bool coasting() const;
    bool gliding() const { return glide_.has_value(); }

signals:
    void changed();
    void zoomChanged(double scale);
    void strokeCommitted(const genko::app::StrokeInput& stroke);
    void frameSelected(const QString& frame_id);
    void cutRequested(const QString& frame_id, const QPointF& p0, const QPointF& p1);
    void gutterMoved(const QString& node, int index, double delta);
    void frameShaped(const QString& frame_id, const QVector<QPointF>& poly);
    void frameBowed(const QString& frame_id, int edge, double mm);
    void frameDrawn(const QVector<QPointF>& points);
    void layerMoveStarted();
    void layerMoved(double dx, double dy);
    void contextMenuAt(const QString& frame_id, const QPoint& global);
    void toolHeld(const QString& tool);
    void omittedChanged(const QStringList& elements);
    void renderFailed(const QString& message);
    // the selection: a new area and how it joins (replace | add | subtract | intersect); the selection pen's line
    // (true: add, false: take away); auto-select and colour-select asked at a point; the chosen area moved, scaled,
    // turned or slanted ([a, b, c, d, e, f]) or pulled freely ({perspective | mesh: points, grid?})
    void selectionDrawn(const genko::core::Json& area, const QString& how);
    void selectionPainted(const QVector<QPointF>& points, bool add);
    void wandRequested(double x_mm, double y_mm);
    void colourAreaRequested(double x_mm, double y_mm);
    void selectionTransformed(const QVector<double>& matrix);
    void selectionWarped(const genko::core::Json& warp);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void tabletEvent(QTabletEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    bool event(QEvent* event) override;

private:
    struct FrameDrag {
        QString kind;  // gutter | cut | vertex | bow | rect | free
        Gutter gutter;
        QPointF start;
        QPointF offset;
        double delta = 0.0;
        QString frame;
        QPointF p0;
        QPointF p1;
        std::vector<QPointF> points;
        std::vector<QPointF> poly;
        int index = 0;
        int edge = 0;
        std::vector<double> curves;
        double mm = 0.0;
    };

    QTransform view_transform() const;
    QTransform mm_transform() const;  // page mm → widget (the view's turn and mirror included)
    QPointF unturned(const QPointF& widget) const;  // a widget point in the unturned view
    QPointF mm_of(const QPointF& unturned) const;
    QPointF pt(double x_mm, double y_mm) const;  // page mm → the unturned view
    QRectF view_rect_mm() const;
    void after_zoom();
    void request_tiles();
    void update_cursor(std::optional<QPointF> pos = std::nullopt);
    void start_pan(const QPointF& pos);
    void coast();
    void stop_coast();
    void live_reset();
    void live_sync();
    void begin_stroke(const QPointF& mm, double pressure, double rotation, bool tablet);
    void extend_stroke(const QPointF& mm, double pressure, double rotation, bool straight);
    void finish_stroke();
    void hold_modifier(const QString& key, bool down);
    void back_from_eraser_end();

    // painting
    void paint_page(QPainter& painter);
    void draw_shadow(QPainter& painter, const QRectF& page_rect) const;
    void draw_guides(QPainter& painter, const QRectF& page_rect) const;
    void draw_plain(QPainter& painter) const;
    void draw_selection(QPainter& painter) const;
    void draw_marquee(QPainter& painter) const;
    void draw_selection_mask(QPainter& painter) const;

    // the marquee tool (canvas_select.cpp)
    struct SelHandle {
        QString kind;  // scale | rotate | pivot | skew | warp
        QString key;
        int index = 0;
        QPointF at;  // mm
    };
    std::vector<SelHandle> sel_handles() const;
    std::array<double, 6> sel_matrix(const QPointF& mm) const;
    std::vector<QPointF> marquee_points() const;
    void selection_done(const core::Json& area);
    bool marquee_press(const QPointF& pos, const QPointF& mm, Qt::KeyboardModifiers modifiers);
    bool marquee_move(const QPointF& mm, Qt::KeyboardModifiers modifiers, bool pressed);
    bool marquee_release();
    bool marquee_key(QKeyEvent* event);
    void draw_frame_numbers(QPainter& painter) const;
    void draw_frame_tool(QPainter& painter) const;
    void draw_polygon_mm(QPainter& painter, const std::vector<QPointF>& points) const;
    void draw_in_page_px(QPainter& painter, const QImage& image, const QRect& box, int dpi) const;

    // the panel tool
    std::optional<Gutter> hit_gutter(double x_mm, double y_mm) const;
    std::vector<std::pair<int, QPointF>> vertex_handles() const;
    std::vector<std::pair<int, QPointF>> bow_handles() const;
    void frame_press(const QPointF& pos);
    void frame_move(const QPointF& pos);
    void frame_release();
    bool finish_frame_poly();

    DocPtr doc_;
    std::size_t index_ = 0;
    std::unique_ptr<PageRenderer> renderer_;
    ViewState view_;
    QString tool_ = QStringLiteral("select");
    std::optional<QString> held_tool_;
    QString held_key_;
    std::optional<QString> eraser_end_;
    core::Json pen_fields_ = core::Json::object();
    std::optional<std::string> pen_layer_;

    // the line being drawn
    core::PenPoints stroke_;
    std::vector<double> turns_;
    std::string stroke_id_;
    bool stroke_tablet_ = false;
    std::unique_ptr<LiveInk> live_;
    std::unique_ptr<LiveInk> committing_;
    struct Overlay {
        std::unique_ptr<LiveInk> ink;
        std::uint64_t generation = 0;
    };
    std::vector<Overlay> overlays_;

    // input
    bool panning_ = false;
    bool space_ = false;
    QPointF last_pos_;
    QPointF pan_speed_;
    QElapsedTimer pan_clock_;
    double pan_time_ = 0.0;
    QTimer coast_timer_;
    std::optional<QPointF> press_pos_;
    std::optional<QPointF> hover_;
    std::optional<std::pair<double, double>> turning_;  // Shift+Space drag: (start angle, rotation then)
    std::optional<std::pair<QPointF, QPointF>> zoom_drag_;
    bool zoom_out_ = false;
    std::optional<std::pair<QPointF, QPointF>> tool_drag_;  // the layer-move tool (mm)
    QImage move_image_;
    QRectF move_where_;
    std::optional<FrameDrag> frame_drag_;
    std::vector<QPointF> frame_poly_;
    Qt::KeyboardModifiers modifiers_;
    std::optional<Selection> selection_;
    QString sel_how_ = QStringLiteral("replace");
    struct SelDrag {
        QString kind;  // move | scale | rotate | skew | pivot | warp
        QString key;
        int index = 0;
        QPointF start;
        std::array<double, 4> box{};
        std::optional<std::array<double, 6>> matrix;
    };
    std::optional<SelDrag> sel_drag_;
    std::optional<QPointF> sel_pivot_;
    std::vector<QPointF> marquee_stroke_;  // the rectangle's, lasso's or selection pen's drag (mm)
    std::optional<std::pair<QPointF, QPointF>> ellipse_drag_;
    std::vector<QPointF> poly_points_;  // the polyline selection's corners
    struct Warp {
        QString kind;  // perspective | mesh
        std::array<double, 4> box{};  // x, y, w, h
        std::vector<QPointF> points;
        int columns = 3;  // points across and down
        int rows = 3;
    };
    std::optional<Warp> warp_;
    mutable std::optional<std::pair<std::string, QImage>> mask_picture_;  // (the area and the quick mask it shows)

    // gliding between views
    std::optional<std::pair<ViewState, ViewState>> glide_;
    double glide_t_ = 1.0;
    QVariantAnimation* glide_motion_ = nullptr;
    QTimer rerender_;
};

// Whether motion should be reduced (環境設定 ui/reduce_motion; with no choice, as the system says).
bool reduce_motion();

}  // namespace genko::app

Q_DECLARE_METATYPE(genko::app::StrokeInput)
