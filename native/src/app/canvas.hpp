#pragma once

#include <QColor>
#include <QElapsedTimer>
#include <QImage>
#include <QPointF>
#include <QPointer>
#include <QRectF>
#include <QRegion>
#include <QTimer>
#include <QTransform>
#include <QVector>
#include <QWidget>

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <string>
#include <vector>

#include "app/frame_tools.hpp"
#include "app/live_ink.hpp"
#include "app/pen.hpp"
#include "app/tiles.hpp"
#include "core/geometry.hpp"
#include "core/guide_layout.hpp"
#include "core/json.hpp"
#include "core/model.hpp"
#include "core/snap.hpp"
#include "core/units.hpp"
#include "render/dabs/dynamics.hpp"

class QKeyEvent;
class QPainter;
class QVariantAnimation;

namespace genko::app {
class InlineEditor;
}

// The page canvas (Python's genko/app/canvas.py PageCanvas): the page as it will print (drawn by the renderer in
// tiles on worker threads) with light overlays on top — the bleed, trim line and basic frame, the reading order of
// the panels, the chosen panel, the panel tool's gutters, corners and bows, the line being drawn.
//
// Tools: 選択 (click a panel to choose it, drag a balloon to move it — its box's handles resize it, its tail's ● and ◇
// move the tip and bend it, the ○ above turns it —, double-click a balloon to type over it, drag elsewhere to move the
// view), テキスト (click to type a line there, Ctrl+Enter keeps it; or draw a balloon's outline by hand: canvas_lines.cpp),
// レイヤー移動, ペン and 消しゴム
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
    QString tool;  // pen | pencil | eraser
    // brushes@1 (PC4 E1): the pen's inputs at each point — its tilt (x and y), the time (ms from the first point) and the
    // airbrush wheel (0..1) —, what a dab brush's line reads
    std::vector<double> tilt;
    std::vector<double> time_ms;
    std::vector<double> wheel;
};

class PageCanvas : public QWidget {
    Q_OBJECT

public:
    static constexpr double kMinScale = 0.3;
    // The most the view zooms in on any page (screen px a mm); on a page of a finer resolution, as far as its pixels at
    // kMaxPixelZoom times (PC5 H-a, 画素の段: Photoshop's 3200 %) — max_scale().
    static constexpr double kMaxScale = 12.0;
    static constexpr double kMaxPixelZoom = 32.0;
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
    // A picture made elsewhere shown as the page (a frame while an animation plays), until the page changes; a null
    // image: the page's own tiles again.
    void show_frame(const QImage& picture);
    bool showing_frame() const { return !shown_frame_.isNull(); }

    // --- tools ----------------------------------------------------------------------------------------------
    const QString& tool() const { return tool_; }
    // A key held switched the tool for a moment (letting it go brings back the one before).
    bool tool_held() const { return held_tool_.has_value(); }
    void set_tool(const QString& tool);
    // The pen in hand (add_stroke fields) and the layer it draws on (none: no live line, a plain guide line).
    void set_live_pen(const core::Json& fields, std::optional<std::string> layer_id);
    const core::Json& live_pen() const { return pen_fields_; }
    double brush_width_mm = 0.35;
    double eraser_mm = 2.0;
    // brushes@1 (PC4 E1): the smoothing's way — "stabilize" (add_stroke's 手ぶれ補正), "string" (紐で引く) or "catch_up"
    // (追いつく: the pen's input smoothed here, render/dabs Smoother; the line shown and kept is the smoothed one) — and its
    // amount (0..1)
    QString smoothing = QStringLiteral("stabilize");
    double smoothing_amount = 0.5;
    // The tools that draw a line (ブラシツール, 鉛筆ツール).
    static bool draws_line(const QString& tool);
    QString frame_mode = QStringLiteral("cut");  // cut | rect | poly | free
    bool show_guides = true;
    bool show_frame_numbers = false;
    // スマホの画面の範囲 (canvas_scale.cpp, Python's canvas.phone_view): on a vertical-scroll book, where each phone screen
    // (the page's width filling a screen held upright, 390 × 844) ends down the page, and the screen under the cursor.
    bool phone_view = false;
    static constexpr double kPhoneAspect = 844.0 / 390.0;
    // Where each phone screen ends down the page (mm from the page's top).
    std::vector<double> phone_screens() const;
    // 目盛り (canvas_scale.cpp, Python's canvas_shapes): mm scales along the top and the left following the view, from
    // which guide lines are pulled out (a drag from the top: across, "h"; from the left: down, "v"; let go on the page:
    // rulerPlaced {"kind": "guide", "axis", "at"}). A guide placed is not drawn (as Python's), only while it is pulled.
    bool show_scale = false;
    static constexpr int kScalePx = 16;
    // A problem the checks found, marked on the page ([x, y, w, h] mm; Python's highlight_box).
    std::optional<std::array<double, 4>> highlight_box;
    core::Binding binding = core::Binding::Right;
    // --- the selection (範囲選択: the marquee tool) --------------------------------------------------------------
    // The area chosen (an op's area: {"poly"} or {"mask"}, mm) and its outline on the screen (a mask's box).
    struct Selection {
        core::Json area;
        std::vector<QPointF> outline;
    };
    void set_selection(std::optional<core::Json> area, std::optional<std::vector<QPointF>> outline = std::nullopt);
    const std::optional<Selection>& selection() const { return selection_; }
    // How the marquee tool chooses: rect | ellipse | lasso | polyline | wand | pen | erase (選択ペン・選択消し).
    QString marquee = QStringLiteral("rect");
    double selection_pen_mm = 4.0;
    bool quick_mask = false;  // the page is in Quick Mask (its channel, laid over in its colour, is what the tools paint)
    // PC3 B1 (ツールの設定 選択範囲ツール): how a new area joins when no key is held (選択方法: replace | add | subtract |
    // intersect; Shift, Alt and both still choose for one drag), whether an ellipse, a lasso and a polygon are drawn
    // anti-aliased (a rectangle never is), the feather they are made with (mm), and the rectangle's and ellipse's style
    // (normal | ratio: 縦横比を固定 | fixed: 固定, its width and height — the ratio's two numbers, or the size in mm).
    QString select_method = QStringLiteral("replace");
    bool select_antialias = true;
    double select_feather_mm = 0;
    QString marquee_style = QStringLiteral("normal");
    double style_width = 1;
    double style_height = 1;
    // PC3 B3: クイック選択ツール's brush (its diameter, mm) — the marquee's way "quick" —; オブジェクト選択ツール is "object".
    double quick_size_mm = 4.0;
    // 表示 → 選択範囲の境界線: the outline (and a mask selection's tint) shown or not; the handles stay.
    bool show_selection_edges = true;
    // 選択範囲を変形: the handles move, scale, turn and slant the selection's outline only, not what is in it.
    bool transform_outline = false;
    // 再選択: the selection let go last on the page shown (none: none).
    std::optional<Selection> previous_selection() const;
    // 選択範囲ランチャー (M6, Python's launcher): what is usually done next with a selection, kept just under it (hidden
    // while there is none, while it is dragged or warped). Made by the window; null: none.
    QWidget* launcher = nullptr;
    void place_launcher();
    bool pivot_mode = false;  // 基準位置を動かす: the next press with the marquee tool puts the pivot there
    // 切り抜きツール (PC3 C-e, canvas_crop.cpp): the box drawn (its middle and size in mm, its turn clockwise in degrees) and
    // the shape a box drawn or resized keeps (its width over its height; 0: free). Enter: cropRequested, Esc: let go.
    struct CropBox {
        QPointF centre;
        QSizeF size;
        double angle = 0.0;
        // The box upright (before its turn), mm.
        QRectF rect() const { return QRectF(centre.x() - size.width() / 2, centre.y() - size.height() / 2, size.width(), size.height()); }
    };
    const std::optional<CropBox>& crop() const { return crop_; }
    void set_crop(std::optional<CropBox> box);
    double crop_ratio = 0.0;
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
    // 自由変形 and the warps (canvas_transform.cpp, PC3 C-c): a box over what the window transforms (its box, mm: x0, y0,
    // x1, y1) taking the mouse and the keys until Enter or Esc. "free": inside moves, a □ scales from the other side
    // (Shift: the ratio kept), outside turns (Shift: by 15°), Ctrl+□ a corner alone (自由な形に) or a side slid along
    // itself (ゆがみ), Ctrl+Alt+Shift+corner a corner and its mirror (遠近法); "scale", "rotate", "skew", "distort" and
    // "perspective" make a drag do only that. "warp": a Bézier mesh's anchors (their handles with them) and handles
    // dragged, a click marks where a split goes. "puppet": a click puts a pin, a drag moves one, Alt+click takes one out.
    // "perspective_warp": Layout — the planes' corners dragged (corners that meet move together), a drag on nothing a new
    // plane; Warp — where the corners go. A drag let go: transformPreview(the spec); Enter: transformCommitted(spec); Esc:
    // transformCancelled(). The spec is transform_layers' way: {"matrix"} | {"quad"} | {"warp"} | {"puppet": {pins}} |
    // {"perspective": {planes}}.
    bool start_transform(const QString& mode, const std::array<double, 4>& box);
    void finish_transform();
    void cancel_transform();
    bool transforming() const;
    QString transform_mode() const;
    std::array<double, 4> transform_box() const;
    core::Json transform_spec() const;
    // The warp being edited (its normal form, render/layer_transform), set again (a style chosen, a split made); the last
    // place clicked in the box (where a split goes).
    core::Json transform_warp() const;
    void set_transform_warp(const core::Json& warp);
    std::optional<QPointF> transform_point() const;
    // Puppet Warp's pins (where each was put, where it is); Perspective Warp's phase (Warp, else Layout) and planes.
    std::vector<std::pair<QPointF, QPointF>> puppet_pins() const;
    // (PC3 A5: the pins a smart object's puppet warp filter has, edited again: each where it was put and where it is)
    void set_puppet_pins(const std::vector<std::pair<QPointF, QPointF>>& pins);
    void set_perspective_warping(bool on);
    bool perspective_warping() const;
    std::vector<std::array<QPointF, 4>> perspective_planes(bool destination) const;
    void set_perspective_planes(const std::vector<std::array<QPointF, 4>>& src, const std::vector<std::array<QPointF, 4>>& dst);
    // 図形 (canvas_tools.cpp): line | polyline | curve | rect | ellipse | polygon, and a polygon's corners. The shape
    // tools (PC3 C-b) add triangle and custom (the custom shape's path: shape_custom, shown fitted into the box dragged;
    // Shift keeps its proportions) and a star's inner ratio (shape_star: 1 a polygon). Alt draws from the centre.
    QString shape_kind = QStringLiteral("line");
    int shape_sides = 5;
    double shape_star = 1.0;
    std::optional<core::vector::Path> shape_custom;
    // 色混ぜ and ゆがみ: the brush's size (mm).
    double blend_mm = 6.0;
    // スポイト: where it picked last (mm) and whether from the drawing layer (color-mode@1: the window reads the inks
    // there on a CMYK or Lab page).
    std::optional<std::pair<QPointF, bool>> last_pick;
    // (PC4 E4) the last pick was an Alt+click with the スポイトツール in hand — not held for a moment by Alt —: for the 背景色.
    bool last_pick_background = false;
    // スポイト: the colour as seen ("view") or the drawing layer's own ("layer": asked of layer_colour_at).
    QString pick_source = QStringLiteral("view");
    std::function<std::optional<std::array<int, 3>>(double x_mm, double y_mm)> layer_colour_at;
    // The polyline or curve being drawn with 図形 ended (Enter or a double click; closed: Shift+Enter), or forgotten (Esc).
    bool finish_shape(bool closed = false);
    bool cancel_shape();
    // A tool that draws a line as the pen does: pen, eraser, 色混ぜ (blend), ゆがみ (liquify), the retouching brushes.
    static bool is_stroke_tool(const QString& tool);
    // 修復の道具 (PC3 B4, retouch_tools.cpp): スポット修復ブラシツール (spotheal), 修復ブラシツール (heal), コピースタンプツール
    // (clone), ヒストリーブラシツール (historybrush); the two that sample a source (heal, clone: Alt+click sets it).
    static bool is_retouch_tool(const QString& tool);
    static bool is_clone_tool(const QString& tool);
    // 画素を変える筆 (PC4 E5, app/paint_tools.cpp): ぼかしツール (blurtool), シャープツール (sharpentool), 指先ツール (smudgetool),
    // 覆い焼きツール (dodge), 焼き込みツール (burn), スポンジツール (sponge), 混合ブラシツール (mixer), 背景消しゴムツール (bgeraser),
    // 色の置き換えツール (colorreplace); their brush's diameter (mm).
    static bool is_pixel_tool(const QString& tool);
    double pixel_tool_mm = 5.0;
    // Their brush's diameter (mm), and the clone source as the window keeps it (コピーソース): where it was set (mm), the
    // destination paired with it by the first stroke after (調整あり; none: the stroke's own start), how the destination's
    // offsets map to the source's (W/H, rotation, flips: clone_matrix's), and the overlay — a picture of what is sampled
    // (its place on the page, mm) shown at the brush as the source lies there, at an opacity, clipped to the brush, hidden
    // while painting, inverted, by a blend (normal, darken, lighten, difference).
    double retouch_mm = 5.0;
    struct CloneShown {
        std::optional<QPointF> source;
        std::optional<QPointF> anchor;
        std::array<double, 4> m{1.0, 0.0, 0.0, 1.0};
        bool overlay = true;
        double opacity = 1.0;
        bool clipped = true;
        bool auto_hide = true;
        bool invert = false;
        QString blend = QStringLiteral("normal");
        QImage picture;
        QRectF picture_mm;
    };
    CloneShown clone_shown;
    // 線の編集 (the vector tool, canvas_vector.cpp): the lines chosen and the point chosen of the first; cut where clicked
    // (クリックした所で線を切る); edit | widen | narrow | redraw | redraw_width | join | simplify (なぞって直す) and how far
    // from a trace the lines are mended. 線の修正（つまむ）: how far the pinch reaches, the line's ends kept.
    std::vector<std::string> vector_ids;
    std::optional<int> vector_point;
    bool vector_cut = false;
    QString vector_mode = QStringLiteral("edit");
    double vector_radius_mm = 2.0;
    double reshape_radius_mm = 6.0;
    bool reshape_pin_ends = false;
    // The layer drawn on (its lines are the ones edited, its rulers the ones shown), as the window says.
    std::function<const core::Layer*()> drawing_layer;
    // 定規 and the grid (canvas_guides.cpp): shown, the pen snapping to them; the kind the ruler tool places (and a
    // perspective ruler's vanishing points, a symmetry ruler's copies); the ruler chosen (its points dragged).
    bool rulers_visible = true;
    bool snap_rulers = true;
    bool grid_visible = false;
    bool grid_snap = false;
    double grid_mm = 5.0;
    QString ruler_kind = QStringLiteral("line");
    int ruler_vps = 1;
    int ruler_copies = 2;
    std::optional<std::string> selected_ruler_id;
    // 3D: the figure or box chosen (its handles dragged with the 3D tool).
    std::optional<std::string> selected_prim_id;
    // 効果線: the effect line chosen (its centre dragged with the effect tool).
    std::optional<std::string> selected_effect_id;
    // A point on the grid when snapping to it is on (else the point itself).
    QPointF grid_point(double x, double y) const;
    // The ruler being placed click by click ended (Enter, a double click) or forgotten (Esc).
    bool finish_curve();
    bool cancel_ruler();
    // Delete with the vector tool: the chosen point, else the chosen lines.
    bool vector_delete();
    // --- paths (canvas_pen.cpp, PC3 C-a: ペンツール, 曲線ペンツール, パス選択ツール) -------------------------------------
    // The path the path tools draw on and show: "work" (the work path), "saved:<name>" (a saved path), "mask:<layer id>"
    // (a layer's vector mask), "shape:<layer id>/<part id>" (a pen layer's shape part, PC3 C-b) — the Paths panel's
    // choice, or the shape part the path selection tool clicked.
    QString target_path = QStringLiteral("work");
    // The shape part on the page under a point (mm; the topmost of the shown pen layers'): its "shape:<layer>/<part>"
    // target and which of its subpaths, or none.
    std::optional<std::pair<QString, std::size_t>> shape_part_at(const QPointF& mm) const;
    // 表示 → ターゲットパス: the target path drawn over the page (always while a path tool is in hand).
    bool show_target_path = true;
    // The target path as the page holds it now (none: there is none).
    std::optional<core::vector::Path> target_path_now() const;
    // The pen's subpath being drawn ended (closed or open; one anchor alone: forgotten): pathDrawn.
    bool finish_pen(bool closed = false);
    bool pen_drawing() const { return !pen_knots_.empty(); }
    // The pen's last anchor taken back (Backspace while drawing).
    bool pen_back();
    // The path selection tool's chosen subpath (its place in the target path); Delete takes it out (pathEdited).
    std::optional<std::size_t> path_subpath;
    bool path_delete();
    // The moving layer's picture (QImage and where it sits, mm), given by the window when a layer move starts.
    void set_move_image(const QImage& image, const QRectF& where_mm);
    // --- 表示 → スナップ・スナップ先 and the move tool's options (PC3 C-d, canvas_snap.cpp) ---------------------------------
    // Snapping on (スナップ) and what to (スナップ先: the guides — the guide rulers —, the grid — grid_snap —, the layers —
    // their boxes' edges and middles —, the document's bounds — the paper's edges and middle). The move tool's layers,
    // the free transform box, the crop tool's box, the shape tools' corners and the pen tools' anchors snap within 8
    // screen pixels; Ctrl held: not. What they line up with is drawn while the drag lasts (smart guides in magenta).
    bool snap_on = true;
    bool snap_guides = true;
    bool snap_layers = true;
    bool snap_bounds = true;
    // The layers' boxes to snap to (x0, y0, x1, y1 mm: the window's, drawn on a worker; none until they are), and the
    // layers a drag moves (not snapped to).
    std::function<std::vector<std::pair<std::string, std::array<double, 4>>>()> snap_boxes;
    std::vector<std::string> snap_moving;
    // What the last snap lined up with (drawn while the drag lasts).
    const std::vector<core::snap::Line>& snap_lines() const { return snap_lines_; }
    // The move tool (移動ツール): 自動選択 (a click picks the layer — or its outermost folder: auto_select_group — showing
    // there, the topmost; Ctrl+click does when it is off), バウンディングボックスを表示 (the box of what it moves, its □
    // grabbed: 自由変形 from there). The box is the window's (moving_box); the click asks the window (layerPicked).
    bool auto_select = false;
    bool auto_select_group = false;
    bool show_move_box = false;
    std::function<std::optional<std::array<double, 4>>()> moving_box;
    // --- the lines (台詞, canvas_lines.cpp) ------------------------------------------------------------------
    // The line chosen: its box shown, and with the select tool its handles.
    std::optional<std::string> selected_line_id;
    // The text tool draws a balloon's outline by hand (フキダシを手で描く) instead of placing a line where clicked.
    bool balloon_pen = false;
    // PC5 G-a (type@1): the text tools — テキスト ("text": down or across as its options say) and 横書き文字ツール
    // ("text_h": across). What they make (the window's, from its options): lines, or type layers — a click a point text
    // (typePointRequested), a drag a box text (typeBoxRequested). A click on a type layer's text, with either, types over
    // it (typeLayerClicked).
    static bool is_text_tool(const QString& tool);
    bool type_layers = false;
    // The type layer chosen (the window's target layer, when it is one): its box (mm; a point text's round its letters),
    // its matrix (none: upright), whether a box text's letters run past its box (Photoshop's ⊞ at its foot) — drawn with
    // the text tools and the move tool.
    struct TypeFrame {
        std::string layer_id;
        QRectF box;
        std::optional<std::array<double, 6>> transform;
        bool box_text = false;
        bool overflow = false;
    };
    std::optional<TypeFrame> type_frame;
    // The topmost type layer shown on the page whose text is at this point (mm), or none.
    std::optional<std::string> hit_type_layer(double x_mm, double y_mm) const;
    // The page's lines (Python's canvas.lines: the book's story for the page shown).
    std::vector<const core::StoryLine*> lines() const;
    // Type a line at this point of the page (Python's open_editor): on_done(the words, or nothing) when finished. An
    // editor still open is finished first (its words kept).
    InlineEditor* open_editor(double x_mm, double y_mm, const QString& text, std::function<void(std::optional<QString>)> on_done);
    InlineEditor* editor() const;
    // A handle of the chosen line (Python's _handles): resize (key nw … w), turn, or a tail's tip (part "to"), its bend
    // ("via") or one of its corners ("vias", via); where it is (mm).
    struct LineHandle {
        QString kind;  // resize | turn | tail
        QString key;   // the resize handle's corner or side
        int tail = -1;
        QString part;  // to | via | vias
        int via = -1;
        QPointF at;
    };
    std::vector<LineHandle> line_handles() const;
    // balloons.tails_of(line): its tails with a tip, or its single old tail as one.
    static core::Json tails_of(const core::StoryLine& line);

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
    // プリントサイズ: the paper as large as it is (at 96 dpi, the screen Photoshop's print size assumes on Windows — PC5 H-b:
    // 環境設定 → 単位・定規 → 画面の解像度, print_dpi).
    void actual_size();
    double print_dpi = 96;
    // PC5 H-a (the user's decision 11): the zoom in the page's pixels — 100 % one page pixel a screen pixel (the screen's
    // own pixels: devicePixelRatio counted), 200 % two. The scale (screen px a mm) of 100 %.
    double pixel_scale() const;
    // 100%, 200%: the page's pixels at that zoom about the middle of the view.
    void actual_pixels(double percent = 100.0);
    // The most the view zooms in here (kMaxScale, or the page's pixels at kMaxPixelZoom times).
    double max_scale() const;
    // レイヤーを画面サイズに合わせる (and the hand and zoom tools' 画面にフィット): the view zoomed and centred on a box of the
    // page (mm), with a margin.
    void fit_box(const QRectF& box_mm);
    void center_on(double x_mm, double y_mm);
    void rotate_view(double degrees);
    void set_rotation(double degrees);
    void flip_view(std::optional<bool> on = std::nullopt);
    void flip_view_vertical(std::optional<bool> on = std::nullopt);
    void reset_view();
    // The zoom in the page's pixels (decision 11), rounded, and as it is.
    void set_zoom_percent(double percent);
    int zoom_percent() const;
    double zoom_percent_exact() const;
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
    // The resolution that matches the zoom (one tile pixel per screen pixel), and the whole page's. Up to the page's own
    // (画素の段, PC5 H-a): zoomed in past it, the tiles are the page's pixels, laid enlarged.
    int wanted_dpi() const;
    // Zoomed in past the page's own pixels (画素の段): they show as squares.
    bool pixel_step() const;
    // ピクセルグリッド (view.show.pixelGrid, PC5 H-a): the grid of the layer drawn on (grid_layer: a paint or precise layer's
    // pixels, a pen or vector layer's — or none — the page's resolution) drawn over the page once a cell of it is
    // kPixelGridPx screen pixels or more (Photoshop's 500 % and up), turning with the view.
    bool show_pixel_grid = false;
    std::optional<std::string> grid_layer;
    static constexpr double kPixelGridPx = 5.0;
    int pixel_grid_dpi() const;
    // (a cell of it, mm across and down)
    QSizeF pixel_grid_cell_mm() const;
    bool pixel_grid_shown() const;
    // ピクセルアートのプレビュー (view.pixelArtPreview): the page's tiles laid nearest, never smoothed.
    void set_pixel_art(bool on);
    bool pixel_art() const;
    // ズームツール's options (PC5 H-a): ズームアウト (a click zooms out, Alt in) and スクラブズーム (dragging left and right
    // zooms out and in about the point pressed, instead of a box).
    bool zoom_out_mode = false;
    bool scrubby_zoom = false;
    int base_dpi() const { return std::min(wanted_dpi(), PageRenderer::kBaseDpi); }
    // A two-finger gesture: spread to zoom about the fingers, twist to turn the view, slide to move.
    void pinch(double factor, double turn_deg, const QPointF& centre, const QPointF& moved);

    // --- 環境設定 (M6: Python's canvas.pen_button, cursor_kind, modifier_tools, tool_modifiers) ----------------------
    // The pen's side button (or its middle one): menu (the right click's menu) | picker (スポイト) | hand (moves the view).
    QString pen_button = QStringLiteral("menu");
    // The pen's pointer on the page: circle (the brush's circle and a dot) | circle_cross | cross | dot.
    QString cursor_kind = QStringLiteral("circle_cross");
    // brushes@1 (PC4 E1): 表示 › ブラシプレビュー (off: the cross alone) and 描いている間は十字だけ; the pen's brush (its kind: a
    // dab brush's tip drawn as the pointer for cursor_kind "tip")
    bool brush_preview = true;
    // PC4 E3 (view.patternPreview): the page shown repeated around itself, as Photoshop's パターンプレビュー (the book does
    // not change)
    bool pattern_preview = false;
    bool cross_while_drawing = false;
    std::string pointer_kind;
    // The tool a held Alt or Ctrl switches to for a moment ("": none).
    std::map<QString, QString> modifier_tools{{QStringLiteral("alt"), QStringLiteral("picker")}, {QStringLiteral("ctrl"), QStringLiteral("select")}};
    // 道具ごとの修飾キー: {tool: {alt | ctrl | shift: a tool, or "none"}} — a tool's own wins over the common one; Shift
    // switches only where a tool says so.
    std::map<QString, std::map<QString, QString>> tool_modifiers;
    // Alt / Ctrl (or a tool's own Shift) held: the chosen tool for a moment, back to the tool before when let go.
    void hold_modifier(const QString& key, bool down);
    // The pointer as the tool and cursor_kind say now.
    void refresh_cursor() { update_cursor(); }

    // --- the graphics card (Python's GpuPageCanvas, 環境設定 ui/gpu_canvas) -------------------------------------------
    // The same drawing done through OpenGL: a QOpenGLWidget under the canvas's other children paints the canvas (the
    // canvas keeps the events, the cursor and the focus). Off: drawn by the processor again.
    void use_gpu(bool on);
    bool gpu() const { return gpu_surface_ != nullptr; }
    // (PC5 H-a) the card could not draw (its context not made, or lost): the canvas draws by the processor again and
    // says so (gpuFailed)
    void gpu_failed();
    // GPU の記憶 (環境設定 ui/gpu_memory_mb, MB): the most the page's tiles take on the card (PageRenderer's texture budget).
    static std::size_t gpu_budget_bytes();
    QWidget* gpu_surface() const { return gpu_surface_; }
    // (every repaint asked of the canvas reaches the surface too)
    void update();
    void update(const QRect& area);
    void update(const QRegion& area);
    // Draw the canvas as it is now (the page, the overlays) with this painter: the canvas's own paint or the surface's.
    void paint_frame(QPainter& painter);

    // --- tests ----------------------------------------------------------------------------------------------
    // Wait (processing events) until every tile wanted now is drawn for the page as it is now.
    bool wait_rendered(int ms);
    const LiveInk* live() const { return live_.get(); }
    const LiveInk* live_copy(std::size_t i) const { return i < live_copies_.size() ? live_copies_[i].get() : nullptr; }
    std::size_t overlays() const { return overlays_.size(); }
    // The picture of a line committed a moment ago, shown until its tiles are drawn (null: none at i).
    const LiveInk* overlay(std::size_t i) const { return i < overlays_.size() ? overlays_[i].ink.get() : nullptr; }
    bool coasting() const;
    bool gliding() const { return glide_.has_value(); }

    // (PC4 D4) A piece of work over the page that takes the mouse and the keys before the tools do — ぼかしギャラリー's pins
    // (app/blur_gallery_tool.cpp) —: what it draws over the page (`at`: a place of the page in mm where the painter draws it;
    // `per_mm`: the screen's pixels a mm), the left button — or the pen, its eraser end too; a double click's second press a press — pressed, dragged and let go (the page's mm, the keys held; true:
    // taken) and the keys (true: taken). The view still pans (Space, the middle button) and zooms under it. None: the tools.
    // `remove`: 消去's keys (Delete, Backspace — 編集's command holds them) while it is on: its chosen part taken away.
    struct CanvasWork {
        std::function<void(QPainter& painter, const std::function<QPointF(QPointF)>& at, double per_mm)> paint;
        std::function<bool(QPointF mm, Qt::KeyboardModifiers keys)> press, move, release;
        std::function<bool(QKeyEvent* event)> key;
        std::function<bool()> remove;
    };
    void set_work(std::shared_ptr<CanvasWork> work);
    bool has_work() const { return work_ != nullptr; }
    bool work_remove() { return work_ && work_->remove && work_->remove(); }

    // (PC5 J-0, SPEC.md PROD-01「エクストラ」) What the production draws over the page it shows — the slices (J-b), the notes
    // (J-c), the count marks and the ruler tool's line (J-d1) —: one painter a key (the 表示 → 表示・非表示 ▸ item that shows
    // it: "slices", "notes", "count", "ruler"), each given the page and the view (ExtraView): lines and words the same size at
    // any zoom (drawn in the screen's pixels). Drawn after the selection, before the tools' handles; not on the pictures
    // written or printed. `extras_shown` is エクストラ's master switch (節 H's H-b: it hides them all), show_extra one's own.
    // The painter turns and flips with the view (as the guides do: a word drawn on it is mirrored while the view is flipped —
    // a painter drawing words draws them at `at`'s point with the painter's turn undone, if they must read). Each canvas of
    // the app has its own painters (a new window's canvas too: the group adds them where the canvas is made).
    struct ExtraView {
        QTransform at;       // a place of the page (mm) → where the painter draws it
        double per_mm = 1;   // the screen's pixels a mm
        QRectF seen_mm;      // the part of the page in sight (mm): draw only what meets it (256 groups × 10000 marks)
        QRegion painting;    // the part of the canvas painted again (the canvas's own pixels; empty: all of it)
    };
    struct CanvasExtra {
        QString key;
        std::function<void(QPainter& painter, const core::Page& page, const ExtraView& view)> paint;
    };
    void add_extra(CanvasExtra extra);  // (a key added again: its painter in place of the one before)
    void show_extra(const QString& key, bool shown);
    bool extra_shown(const QString& key) const;
    std::vector<QString> extra_keys() const;
    bool extras_shown = true;

    // --- PC5 H-b: ガイド・グリッド・エクストラ・単位 (canvas_guide_lines.cpp; SPEC.md VIEW-01) --------------------------------------
    // What エクストラ hides with the rest (shows: one's own switch and the master's): the guides, the grid, the layers' edges,
    // the selection's edges, the target path, the smart guides, the warps' mesh and pins, the pixel grid, the production's.
    bool shows(bool own) const { return own && extras_shown; }
    // The guides (the page's rulers of kind "guide") drawn across the view — 表示 → 表示・非表示 ▸ ガイド (show_guide_lines)
    // and カンバスガイド (show_canvas_guides: the page's own; an artboard's come with 節 I) — in 環境設定 → ガイド・グリッド・
    // スライス's colour and style. The move tool grabs one (the other tools while Ctrl is held; the ruler tool never) and
    // drags it (rulerEdited {at}); let go on a scale or out of the view it is taken away (guideRemoved); Alt+click turns it
    // about where it was clicked ({axis, at}); a double click asks where it goes (guideAsked). ガイドをロック: none is
    // grabbed (nor one fixed). Snapping to them is the snap's (canvas_snap.cpp), as before.
    bool show_guide_lines = true;
    bool show_canvas_guides = true;
    bool lock_guides = false;
    QColor guide_colour = QColor(0x4a, 0xff, 0xff);
    Qt::PenStyle guide_style = Qt::SolidLine;  // lines | dashed lines | dots
    // The grid's colour and style, and its thick lines every `grid_subdivisions` of grid_mm (環境設定: グリッド線・分割数).
    QColor grid_colour = QColor(80, 140, 220);
    Qt::PenStyle grid_style = Qt::SolidLine;
    int grid_subdivisions = 5;
    // スマートガイド: the lines a drag lined up with layer to layer and with the paper's middle (magenta), and the gaps a
    // moved box has equal to its neighbours on both sides said in the ruler's unit.
    bool show_smart_guides = true;
    QColor smart_colour = QColor(0xe6, 0x49, 0x80);
    struct Spacing {  // a gap equal on both sides of a moved box: along x (vertical false) or along y, from … to, at `across`
        bool vertical = false;
        double from = 0, to = 0, across = 0;
    };
    const std::vector<Spacing>& spacing() const { return spacing_; }  // (while a drag lasts)
    // レイヤーエッジ: the chosen layers' boxes (x0, y0, x1, y1 mm, the window's) drawn as thin lines.
    bool show_layer_edges = false;
    std::function<std::vector<std::array<double, 4>>()> layer_edges;
    // メッシュ・編集ピン: the warps' grid and the puppet's box (mesh), their anchors, handles and pins (edit pins) — hidden,
    // the warp goes on as it was.
    bool show_mesh = true;
    bool show_edit_pins = true;
    // 単位・定規 (core/units.hpp): the scales and the words of a length in the ruler's unit (the page's pixels, its sides).
    core::units::Unit ruler_unit = core::units::Unit::Mm;
    double points_per_inch = 72;
    double column_pt = 180;
    double gutter_pt = 12;
    core::units::Basis unit_basis(bool across) const;  // (across: a length along the page's width)
    QString length_words(double mm, bool across) const;
    // The guide under a point of the widget that a press would grab (none: none, locked, hidden), and the one being dragged.
    std::optional<std::string> guide_at(const QPointF& widget) const;
    std::optional<std::string> dragged_guide() const;
    // 新規ガイドレイアウトを作成…'s preview: the guides it would add, drawn faintly while its window is open (none: none).
    std::optional<core::GuideLayout> guide_preview;

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
    // the move tool (PC3 C-d): 自動選択 asked at a point (mm; group: its outermost folder); the bounding box's □ grabbed
    // (the window starts 自由変形 for the press to go on in)
    void layerPicked(double x_mm, double y_mm, bool group);
    void moveBoxGrabbed();
    void contextMenuAt(const QString& frame_id, const QPoint& global);
    void toolHeld(const QString& tool);
    void omittedChanged(const QStringList& elements);
    void renderFailed(const QString& message);
    // PC5 H-a: the graphics card stopped drawing the page (the canvas draws it by the processor now)
    void gpuFailed(const QString& message);
    // the pointer over the page with no button down (mm): the status bar's values there (color-model@1)
    void pointerOver(double x_mm, double y_mm);
    // the selection: a new area and how it joins (replace | add | subtract | intersect); the selection pen's line
    // (true: add, false: take away); auto-select and colour-select asked at a point; the chosen area moved, scaled,
    // turned or slanted ([a, b, c, d, e, f]) or pulled freely ({perspective | mesh: points, grid?})
    void selectionDrawn(const genko::core::Json& area, const QString& how);
    void selectionPainted(const QVector<QPointF>& points, bool add);
    void wandRequested(double x_mm, double y_mm);
    // マジック消しゴムツール (PC3 B1): a click on the page (mm).
    void magicEraseRequested(double x_mm, double y_mm);
    // コピースタンプツール・修復ブラシツール (PC3 B4): Alt+click — the clone source set there (mm).
    void cloneSourcePicked(double x_mm, double y_mm);
    void selectionTransformed(const QVector<double>& matrix);
    void selectionWarped(const genko::core::Json& warp);
    // PC3 B3: クイック選択ツール's stroke (mm) and オブジェクト選択ツール's rectangle (mm), and how the area they find joins.
    void quickSelected(const QVector<QPointF>& points, const QString& how);
    void objectSelected(const QRectF& box, const QString& how);
    // the crop tool: its box changed; Enter on it (the box upright, mm, and its turn clockwise in degrees)
    void cropChanged();
    void cropRequested(const QRectF& box, double angle);
    // 自由変形 and the warps (PC3 C-c): a drag of the box let go (preview it), Enter (apply it), Esc.
    void transformPreview(const genko::core::Json& spec);
    void transformCommitted(const genko::core::Json& spec);
    void transformCancelled();
    // the drawing tools: the colour under the eyedropper; the fill tool's click; the area drawn around to fill; the
    // gradient's drag (mm); a figure drawn ({shape, points | box, sides?, closed?}: an add_shape op)
    void colourPicked(const std::array<int, 3>& rgb);
    void fillRequested(double x_mm, double y_mm);
    void areaFilled(const QVector<QPointF>& points);
    void gradientRequested(const QPointF& from, const QPointF& to);
    void shapeDrawn(const genko::core::Json& shape);
    // the lines edited: a vector_edit op's change (without page and layer); a trace [[x, y, pressure], …] and how it
    // mends the lines (a trace_edit op); a line pinched into new points (a reshape_stroke op)
    void vectorEdited(const genko::core::Json& change);
    void vectorTraced(const genko::core::Json& points, const QString& mode);
    void strokeReshaped(const QString& stroke_id, const genko::core::Json& points);
    // the guides: a ruler placed ({kind, points, …}: an add_ruler op), one's points moved ({points, angle?}: edit_ruler);
    // a 3D figure or box chosen ("" for none), moved or turned ({pos} | {rot}: edit_prim), a figure's part dragged
    // (the part, where to: pose_mannequin / pose_figure)
    void rulerPlaced(const genko::core::Json& ruler);
    void rulerEdited(const QString& ruler_id, const genko::core::Json& change);
    // (PC5 H-b) a guide dragged off the view or onto a scale (delete_ruler), a guide double-clicked (新規ガイド… to move it)
    void guideRemoved(const QString& ruler_id);
    void guideAsked(const QString& ruler_id);
    void primSelected(const QString& prim_id);
    void primEdited(const QString& prim_id, const genko::core::Json& change);
    void primPosed(const QString& prim_id, const QString& handle, const genko::core::Json& to);
    // 効果線 and 素材を置く: the effect tool clicked here (mm), an effect's centre chosen or dragged to (mm); the material
    // tool clicked here
    void effectRequested(double x_mm, double y_mm);
    void effectSelected(const QString& effect_id);
    void effectMoved(const QString& effect_id, const QPointF& centre);
    void stampRequested(double x_mm, double y_mm);
    // the lines: the text tool clicked here (mm); a balloon's outline drawn by hand ([[x, y], …] mm); a line chosen (and
    // whether its panel opens); a line's box or tails changed ({x_mm, y_mm, w_mm, h_mm} | {tails}: move_line, or {style}:
    // edit_line); a double click on a balloon (type over it); a right click on one; a balloon dragged to (mm)
    void textRequested(double x_mm, double y_mm);
    void balloonDrawn(const genko::core::Json& outline);
    void lineSelected(const QString& line_id, bool open_panel);
    void lineGeometry(const QString& line_id, const genko::core::Json& change);
    void lineEditRequested(const QString& line_id);
    void lineContextMenu(const QString& line_id, const QPoint& global);
    void textMoved(const QString& line_id, double x_mm, double y_mm);
    // PC5 G-a: a text tool making type layers clicked here (a point text) or dragged over this box (a box text, mm); a
    // type layer's text clicked with a text tool
    void typePointRequested(double x_mm, double y_mm);
    void typeBoxRequested(const QRectF& box_mm);
    void typeLayerClicked(const QString& layer_id);
    // the paths: a subpath drawn with a pen ({"closed", "knots"} in mm: a set_path's subpath); the target path changed by
    // the path selection tool (its whole path)
    void pathDrawn(const genko::core::Json& subpath);
    void pathEdited(const genko::core::Json& path);
    // the path selection tool chose another target path (a shape part clicked on the page: PC3 C-b)
    void targetPathChosen();

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
    void focusOutEvent(QFocusEvent* event) override;  // (a held key's tool let go: its release goes elsewhere)
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
    // The live line drawn for the stroke so far (live_draw); a line that cannot be drawn (a brush's paper picture this
    // process does not have) is not shown for the rest of the stroke and the window is told (renderFailed), never thrown
    // out of the event that drew it.
    void live_sync();
    void live_draw();
    // (brushes@1, PC4 E1) a dab brush's tip as the pointer at `at` (the view's pixels), `size_mm` across; false for a brush
    // that is not a dab brush (or one that cannot be laid)
    bool draw_tip(QPainter& painter, const QPointF& at, double size_mm) const;
    void begin_stroke(const QPointF& mm, double pressure, double rotation, bool tablet, const PenSample& sample = {});
    void extend_stroke(const QPointF& mm, double pressure, double rotation, bool straight, const PenSample& sample = {});
    // (brushes@1, PC4 E1) one point of the line being drawn, with its inputs
    void add_point(const core::PenPoint& p, double rotation, const PenSample& sample);
    void finish_stroke();
    void back_from_eraser_end();

    // painting
    QRegion painting_;  // (the part of the canvas a paint event paints again; none: all of it)
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
    // The rectangle's or ellipse's other corner from where it started, by the style (縦横比を固定, 固定).
    QPointF styled_corner(const QPointF& from, const QPointF& to) const;
    // A drawn polygon as the selection's area: anti-aliased (an ellipse, a lasso, a polygon) and feathered as the tool says.
    core::Json drawn_area(const core::Json& poly, bool smooth) const;
    void selection_done(const core::Json& area);
    bool marquee_press(const QPointF& pos, const QPointF& mm, Qt::KeyboardModifiers modifiers);
    bool marquee_move(const QPointF& mm, Qt::KeyboardModifiers modifiers, bool pressed);
    bool marquee_release();
    bool marquee_key(QKeyEvent* event);
    // PC3 B3: クイック選択ツール's stroke and オブジェクト選択ツール's rectangle (canvas_select.cpp)
    bool smart_press(const QPointF& mm, Qt::KeyboardModifiers modifiers);
    bool smart_move(const QPointF& mm, bool pressed);
    bool smart_release();
    void draw_smart_drag(QPainter& painter) const;
    // the crop tool (canvas_crop.cpp)
    struct CropDrag {
        QString kind;  // new | move | corner | turn
        QPointF start;
        CropBox was;
        int corner = 0;
    };
    std::optional<CropBox> crop_;
    std::optional<CropDrag> crop_drag_;
    bool crop_press(const QPointF& pos, const QPointF& mm, Qt::KeyboardModifiers modifiers);
    bool crop_move(const QPointF& mm, Qt::KeyboardModifiers modifiers, bool pressed);
    bool crop_release();
    bool crop_key(QKeyEvent* event);
    void draw_crop(QPainter& painter) const;
    // 自由変形 and the warps (canvas_transform.cpp): the box's state (null: none) and its handling of the mouse and keys.
    struct Transforming;
    std::shared_ptr<Transforming> xform_;
    bool transform_press(const QPointF& pos, const QPointF& mm, Qt::KeyboardModifiers modifiers);
    bool transform_move(const QPointF& mm, Qt::KeyboardModifiers modifiers);
    bool transform_release(const QPointF& mm);
    bool transform_key(QKeyEvent* event);
    void draw_transform(QPainter& painter) const;
    // snapping and the move tool's box (canvas_snap.cpp): the targets now; a point snapped (Ctrl: as it is); the move
    // that snaps a moving box (Ctrl: none); the lines drawn; the move tool's box and its □
    std::vector<core::snap::Line> snap_lines_;
    core::snap::Targets snap_targets() const;
    double snap_reach() const;
    QPointF snap_point(const QPointF& mm, Qt::KeyboardModifiers modifiers);
    QPointF snap_box(const std::array<double, 4>& box, Qt::KeyboardModifiers modifiers);
    void draw_snap(QPainter& painter) const;
    std::optional<std::array<double, 4>> move_box_now() const;
    std::optional<int> move_box_handle(const QPointF& pos) const;
    void draw_move_box(QPainter& painter) const;
    std::optional<std::array<double, 4>> move_snap_box_;  // (the moving layers' box as the drag began)
    void draw_frame_numbers(QPainter& painter) const;
    void draw_frame_tool(QPainter& painter) const;
    void draw_polygon_mm(QPainter& painter, const std::vector<QPointF>& points) const;
    void draw_in_page_px(QPainter& painter, const QImage& image, const QRect& box, int dpi) const;

    // the scales, the guide pulled from them, the phone screens and the checks' mark (canvas_scale.cpp)
    std::optional<QString> scale_hit(const QPointF& widget) const;
    bool guide_press(const QPointF& widget);
    bool guide_move(const QPointF& widget);
    bool guide_release(const QPointF& widget);
    void draw_guide_drag(QPainter& painter) const;
    void draw_scale(QPainter& painter) const;
    void draw_scale_in_unit(QPainter& painter) const;  // (PC5 H-b: the scales in a unit other than mm)
    void draw_phone(QPainter& painter) const;
    void draw_highlight(QPainter& painter) const;

    // the drawing tools (canvas_tools.cpp): true when the press, move or release was theirs
    bool tool_press(const QPointF& pos, const QPointF& mm, Qt::KeyboardModifiers modifiers);
    bool tool_move(const QPointF& mm, Qt::KeyboardModifiers modifiers, bool pressed);
    bool tool_release();
    QPointF constrained(const QPointF& a, const QPointF& b, Qt::KeyboardModifiers modifiers) const;
    std::vector<QPointF> shape_preview(bool& closed) const;
    void pick_colour(const QPointF& pos, bool background = false);
    void draw_tools(QPainter& painter) const;
    // the retouching brushes (retouch_tools.cpp): Alt+click sets the clone source (true: taken); the source's cross and
    // the overlay at the brush
    bool retouch_press(const QPointF& mm, Qt::KeyboardModifiers modifiers);
    void draw_retouch(QPainter& painter) const;
    // the path tools (canvas_pen.cpp): true when the press, move or release was theirs
    bool path_press(const QPointF& pos, const QPointF& mm, Qt::KeyboardModifiers modifiers);
    bool path_move(const QPointF& mm, Qt::KeyboardModifiers modifiers);
    bool path_release();
    std::optional<core::vector::Path> path_dragged() const;
    void draw_paths(QPainter& painter) const;
    // 定規, the grid and 3D (canvas_guides.cpp)
    core::Json page_rulers() const;
    // snapped_preview: what the line will become on the rulers (and its symmetry copies), for showing while the pen is
    // down; nothing when no ruler takes it (snapping off, no rulers, fewer than two points).
    std::optional<std::vector<core::PenPoints>> snapped_preview(const core::PenPoints& points) const;
    bool snapped_preview_applies() const;  // (for the line being drawn)
    bool near_point(const QPointF& pos, const QPointF& mm, double px = 8) const;
    void draw_grid(QPainter& painter) const;
    void draw_pixel_grid(QPainter& painter) const;  // (PC5 H-a: canvas_view.cpp)
    void line_across(QPainter& painter, const QPointF& a, const QPointF& d, double length = 2000.0) const;
    void draw_one_ruler(QPainter& painter, const core::Json& ruler, bool selected) const;
    void draw_rulers(QPainter& painter) const;
    std::optional<core::Json> draft_ruler(const std::vector<QPointF>& points) const;
    void ruler_press(const QPointF& pos);
    bool ruler_move(const QPointF& pos);
    void ruler_release();
    void finish_ruler();
    std::vector<std::pair<std::string, QPointF>> prim_handles(const core::Json& prim) const;
    std::vector<std::pair<QPointF, QPointF>> prim_lines(const core::Json& prim) const;
    void draw_prims(QPainter& painter) const;
    void select_prim(std::optional<std::string> prim_id);
    void prim_press(const QPointF& pos);
    bool prim_move(const QPointF& pos);
    void prim_release();
    // 効果線 (canvas_effects.cpp)
    std::vector<std::pair<std::string, QPointF>> effect_handles() const;
    void draw_effect_handles(QPainter& painter) const;
    void effect_press(const QPointF& pos);
    bool effect_move(const QPointF& mm);
    bool effect_release();
    // the lines (canvas_lines.cpp): true when the press, move or release was theirs
    const core::StoryLine* selected_line() const;
    const core::StoryLine* hit_line(double x_mm, double y_mm) const;
    std::optional<LineHandle> hit_line_handle(const QPointF& pos) const;
    void draw_balloon_box(QPainter& painter, const core::StoryLine& line, std::optional<QPointF> at, bool strong = false,
                          bool fill = true) const;
    void draw_lines(QPainter& painter) const;
    void draw_line_handles(QPainter& painter) const;
    void draw_balloon_stroke(QPainter& painter) const;
    bool line_press(const QPointF& pos, const QPointF& mm);
    bool line_move(const QPointF& mm, Qt::KeyboardModifiers modifiers);
    bool line_release();
    bool line_double_click(const QPointF& mm);
    bool line_context_menu(const QPointF& mm, const QPoint& global);
    bool text_press(const QPointF& mm);
    bool text_move(const QPointF& mm, bool pressed);
    bool text_release();
    void draw_type(QPainter& painter) const;  // (PC5 G-a: the box dragged for a box text, the chosen type layer's frame)
    std::optional<std::pair<QPointF, QPointF>> type_drag_;  // (PC5 G-a: where the drag began and is now, mm)
    // 線の編集 and 線の修正 (canvas_vector.cpp)
    std::vector<core::StrokePtr> vector_strokes() const;
    core::StrokePtr vector_hit(double x, double y) const;
    std::optional<int> vector_point_hit(double x, double y) const;
    void vector_press(double x, double y, Qt::KeyboardModifiers modifiers);
    bool vector_move(double x, double y);
    bool vector_release();
    void draw_vector(QPainter& painter) const;
    void reshape_press(double x, double y);
    void reshape_move(double x, double y);
    bool reshape_release();

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
    QImage shown_frame_;  // show_frame
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
    std::vector<double> tilts_, times_, wheels_;   // (brushes@1: the pen's inputs, one a point; tilt two)
    double stroke_t0_ = 0.0;                        // (the time of the line's first point)
    std::optional<render::dabs::Smoother> smoother_;  // (the smoothing's way, when it is the window's)
    std::string stroke_id_;
    bool stroke_tablet_ = false;
    std::unique_ptr<LiveInk> live_;
    std::vector<std::unique_ptr<LiveInk>> live_copies_;  // the symmetry copies of a snapped line
    bool live_snapped_ = false;                          // live_ holds the snapped line (drawn whole each time)
    bool live_failed_ = false;                           // this stroke's live line could not be drawn (live_sync)
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
    struct GuideDrag {  // a guide line pulled from a scale: its axis (h | v) and where it is (mm; none: not yet moved)
        QString axis;
        std::optional<double> at;
        std::optional<std::string> id;  // (PC5 H-b: a guide placed, grabbed — its ruler's id)
        bool turn = false;              // (Alt held as it was grabbed: a click turns it)
    };
    std::optional<GuideDrag> guide_drag_;
    bool zoom_out_ = false;
    std::optional<QPointF> scrub_;  // (スクラブズーム: the point last dragged to)
    bool scrubbed_ = false;
    std::optional<std::pair<QPointF, QPointF>> tool_drag_;  // the layer-move tool (mm)
    // the path tools (canvas_pen.cpp): the pen's anchors so far (the curvature pen's: its anchors, a corner not smooth),
    // the anchor whose handles a drag pulls; a subpath or one of its points dragged with the path selection tool
    std::vector<core::vector::Knot> pen_knots_;
    bool pen_curve_ = false;
    std::optional<std::size_t> pen_handle_;
    struct PathDrag {
        core::vector::Path path;  // (as it was)
        std::size_t subpath = 0;
        int knot = -1;  // (-1: the whole subpath)
        int part = 0;   // (0 the anchor, 1 its handle in, 2 its handle out)
        QPointF start;
        QPointF now;
        bool copy = false;
    };
    std::optional<PathDrag> path_drag_;
    QImage move_image_;
    QRectF move_where_;
    std::optional<FrameDrag> frame_drag_;
    std::vector<QPointF> frame_poly_;
    Qt::KeyboardModifiers modifiers_;
    std::optional<Selection> selection_;
    std::optional<std::pair<std::string, Selection>> previous_;  // (再選択: the page's id and the selection let go)
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
    std::vector<QPointF> smart_drag_;   // (PC3 B3: the quick selection's stroke, the object selection's two corners; mm)
    QString smart_how_;
    std::optional<std::pair<QPointF, QPointF>> shape_drag_;  // 図形 dragged (mm)
    std::vector<QPointF> shape_pts_;                         // 図形's polyline or curve clicked (mm)
    std::vector<QPointF> lasso_fill_;                        // 囲って塗る's drag (mm)
    struct VectorDrag {
        std::string id;
        int index = 0;
        QPointF to;
    };
    std::optional<VectorDrag> vector_drag_;                         // a control point dragged
    std::optional<std::vector<std::array<double, 3>>> vector_trace_;  // なぞって直す: [x, y, pressure]
    struct Reshape {
        std::string id;
        std::vector<std::vector<double>> orig;  // the line walked in 1 mm steps: [x, y(, pressure)]
        std::vector<std::vector<double>> points;
        QPointF grab;
        std::vector<double> along;  // how far along the line each point is (mm)
    };
    std::optional<Reshape> reshape_;
    double last_pressure_ = 0.7;  // the pen's pressure at its last event (a trace takes it)
    std::optional<std::vector<QPointF>> ruler_draft_;  // the points of a ruler being placed
    std::optional<core::Json> ruler_first_;           // a multi-curve ruler's first curve, while the second is placed
    struct RulerDrag {
        std::string id;
        int index = 0;
        core::Json ruler;
        bool moved = false;
    };
    std::optional<RulerDrag> ruler_drag_;
    struct PrimDrag {
        std::string id;
        std::string handle;
        core::Json prim;
        core::Json orig;
        QPointF start;
        bool moved = false;
        bool grab = false;
        core::Json to;
    };
    std::optional<PrimDrag> prim_drag_;
    struct EffectDrag {
        std::string id;
        QPointF to;
        bool moved = false;
    };
    std::optional<EffectDrag> effect_drag_;
    struct LineDrag {  // a balloon dragged: where it was grabbed (from its corner) and where it is shown
        std::string id;
        QPointF grab;
        QPointF pos;
        QPointF orig;
    };
    std::optional<LineDrag> line_drag_;
    struct HandleDrag {  // a handle of the chosen line dragged: the box as it was and is now, the tails, the turn
        LineHandle handle;
        std::string line;
        std::array<core::Num, 4> orig;
        std::array<core::Num, 4> cur;
        core::Json tails;
        std::optional<core::Num> angle;
    };
    std::optional<HandleDrag> handle_drag_;
    std::vector<QPointF> balloon_stroke_;  // the balloon pen's outline (mm)
    QPointer<InlineEditor> editor_;
    struct Warp {
        QString kind;  // perspective | mesh
        std::array<double, 4> box{};  // x, y, w, h
        std::vector<QPointF> points;
        int columns = 3;  // points across and down
        int rows = 3;
    };
    std::optional<Warp> warp_;
    std::shared_ptr<CanvasWork> work_;  // (PC4 D4: set_work)
    std::vector<CanvasExtra> extras_;   // (PC5 J-0: add_extra, in the order added)
    std::vector<QString> extras_hidden_;
    void draw_extras(QPainter& painter) const;
    // (PC5 H-b: canvas_guide_lines.cpp) the guides, the layers' edges, the equal gaps of a moved box; a guide grabbed
    bool guide_grab(const QPointF& widget, Qt::KeyboardModifiers modifiers);
    void draw_guide_lines(QPainter& painter) const;
    void draw_layer_edges(QPainter& painter) const;
    void draw_spacing(QPainter& painter) const;
    std::vector<Spacing> spacing_;
    void find_spacing(const std::array<double, 4>& box);
    void forget_snap();
    mutable std::optional<std::pair<std::string, QImage>> mask_picture_;  // (the area and the quick mask it shows)

    // gliding between views
    std::optional<std::pair<ViewState, ViewState>> glide_;
    double glide_t_ = 1.0;
    QVariantAnimation* glide_motion_ = nullptr;
    QTimer rerender_;

    QWidget* gpu_surface_ = nullptr;  // use_gpu
};

// Whether motion should be reduced (環境設定 ui/reduce_motion; with no choice, as the system says).
bool reduce_motion();

// The page drawn by the graphics card (Python's canvas.gpu_available): only when the person turned it on (ui/gpu_canvas:
// some drivers give a blank white page), on a real screen (not offscreen, minimal or vnc), with an OpenGL context that
// can be made and is not done by the processor.
bool gpu_available();
// OpenGL done by the processor (a virtual machine, a remote desktop): slower than drawing without it.
bool software_renderer(const QString& name);

}  // namespace genko::app

Q_DECLARE_METATYPE(genko::app::StrokeInput)
