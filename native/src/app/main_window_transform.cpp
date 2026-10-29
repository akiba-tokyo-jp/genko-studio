// The window's transforms (PC3 C-c, SPEC.md COLOR-07, the user's decision 5: Photoshop's names): 編集 → 自由変形 (Ctrl+T),
// 変形 ▸ 再実行・拡大・縮小・回転・ゆがみ・遠近法・水平方向に／垂直方向に／交差方向に分割ワープ・ワープ分割を削除・180° 回転・
// 90° 回転（時計回り）・90° 回転（反時計回り）, パペットワープ, 遠近法ワープ, コンテンツに応じて拡大・縮小…, ゆがみ…; 台詞 →
// ワープテキスト…. What they move: the layers chosen in the layer panel (else the layer drawn on), the folders' layers and
// the linked ones with them, within the selection when there is one (the op transform_layers). The free transform box is
// the canvas's (canvas_transform.cpp); its options bar is here.

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QRadioButton>
#include <QSpinBox>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>
#include <cstring>
#include <map>
#include <tuple>

#include "app/ask.hpp"
#include "app/canvas.hpp"
#include "app/layer_panel.hpp"
#include "app/main_window.hpp"
#include "core/channels.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "render/layer_transform.hpp"
#include "render/liquify_field.hpp"
#include "render/page.hpp"
#include "render/perspective_warp.hpp"
#include "render/resample.hpp"
#include "render/smart_filters.hpp"
#include "service/wording.hpp"

namespace genko::app {

using core::Json;
namespace lt = render::layer_transform;

namespace {

double r3(double v) { return core::py_round(v, 3); }

// The warp styles by Photoshop's names (ワープテキスト and the warp's 形).
const std::vector<std::pair<const char*, QString>>& warp_styles() {
    static const std::vector<std::pair<const char*, QString>> styles{
        {"arc", QStringLiteral("円弧")},          {"arcLower", QStringLiteral("下弦")},      {"arcUpper", QStringLiteral("上弦")},
        {"arch", QStringLiteral("アーチ")},        {"bulge", QStringLiteral("下降")},         {"shellLower", QStringLiteral("貝殻（下向き）")},
        {"shellUpper", QStringLiteral("貝殻（上向き）")}, {"flag", QStringLiteral("旗")},      {"wave", QStringLiteral("波形")},
        {"fish", QStringLiteral("魚形")},          {"rise", QStringLiteral("上昇")},          {"fisheye", QStringLiteral("魚眼レンズ")},
        {"inflate", QStringLiteral("膨張")},       {"squeeze", QStringLiteral("絞り込み")},   {"twist", QStringLiteral("ねじれ")}};
    return styles;
}

QDoubleSpinBox* percent_box(const QString& name, double lo, double hi, double value) {
    auto* box = new QDoubleSpinBox;
    box->setObjectName(name);
    box->setRange(lo, hi);
    box->setDecimals(0);
    box->setValue(value);
    box->setSuffix(QStringLiteral(" %"));
    return box;
}

// --- Filter › Liquify's window ---------------------------------------------------------------------------------------

// The layer's picture shown at the window's resolution, the strokes drawn on it in mm and the picture bent by them
// (render/liquify_field on the page's mm, as the op replays them).
class LiquifyView : public QWidget {
public:
    LiquifyView(const render::Image& picture, double page_w, double page_h) : page_w_(page_w), page_h_(page_h) {
        setObjectName(QStringLiteral("liquify_view"));
        base_ = render::resample::from_rgba8(picture.mode() == "RGBA" ? picture : picture.convert("RGBA"));
        shown_ = to_image(base_);
        setFixedSize(shown_.size());
        setMouseTracking(false);
    }

    std::function<render::liquify::Stroke()> brush;  // (the tool and brush chosen now, without points)
    const Json& strokes() const { return strokes_; }
    // (PC3 A5: a smart object's Liquify filter edited again: the strokes it has, shown, more added after them)
    void set_strokes(const Json& strokes) {
        strokes_ = strokes.is_array() ? strokes : Json::array();
        redraw();
    }
    // A stroke of the whole field (すべてを復元, the mask's buttons).
    void add_global(const char* tool, std::optional<double> amount = std::nullopt) {
        Json s{{"tool", tool}};
        if (amount) s["amount"] = *amount;
        strokes_.push_back(s);
        redraw();
    }
    void add_stroke(const render::liquify::Stroke& s) {
        Json points = Json::array();
        for (const auto& p : s.points) points.push_back(Json::array({r3(p[0]), r3(p[1]), core::py_round(p[2], 3)}));
        strokes_.push_back(Json{{"tool", std::string(render::liquify::to_string(s.tool))}, {"size_mm", r3(s.size)}, {"density", s.density}, {"pressure", s.pressure},
                                {"rate", s.rate}, {"points", points}});
        redraw();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), Qt::white);
        p.drawImage(0, 0, shown_);
        if (!frozen_.isNull()) p.drawImage(0, 0, frozen_);
        if (!points_.empty()) {
            p.setPen(QPen(QColor(28, 126, 214), 1));
            QPolygonF line;
            for (const auto& q : points_) line << QPointF(q[0] / mm_x(), q[1] / mm_y());
            p.drawPolyline(line);
        }
    }
    void mousePressEvent(QMouseEvent* e) override {
        points_.clear();
        add_point(e->position());
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!points_.empty()) add_point(e->position());
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        if (points_.empty()) return;
        add_point(e->position());
        render::liquify::Stroke s = brush ? brush() : render::liquify::Stroke{};
        s.points = std::move(points_);
        points_.clear();
        add_stroke(s);
    }

private:
    double mm_x() const { return page_w_ / base_.width; }
    double mm_y() const { return page_h_ / base_.height; }
    void add_point(const QPointF& at) {
        points_.push_back({at.x() * mm_x(), at.y() * mm_y(), 1.0});
        update();
    }
    static QImage to_image(const render::resample::Plane& plane) {
        const render::Image made = render::resample::to_rgba8(plane);
        const std::string bytes = made.tobytes();
        QImage out(made.width(), made.height(), QImage::Format_RGBA8888);
        for (int y = 0; y < made.height(); ++y) std::memcpy(out.scanLine(y), bytes.data() + std::size_t(y) * std::size_t(made.width()) * 4, std::size_t(made.width()) * 4);
        return out;
    }
    void redraw() {
        constexpr double unit = 25.4 / 200.0;
        render::liquify::Field field(0.0, 0.0, page_w_, page_h_, render::liquify::auto_cell(page_w_, page_h_, unit), unit);
        for (const Json& s : strokes_) {
            render::liquify::Stroke st;
            st.tool = *render::liquify::tool_from(s["tool"].get<std::string>());
            st.size = s.value("size_mm", 12.7);
            st.density = s.value("density", 50.0);
            st.pressure = s.value("pressure", 100.0);
            st.rate = s.value("rate", 80.0);
            if (s.contains("amount")) st.amount = s["amount"].get<double>();
            if (s.contains("points"))
                for (const Json& q : s["points"]) st.points.push_back({q[0].get<double>(), q[1].get<double>(), q.size() > 2 ? q[2].get<double>() : 1.0});
            field.apply(st);
        }
        const double sx = 1.0 / mm_x(), sy = 1.0 / mm_y();
        shown_ = to_image(render::resample::remap(
            base_,
            [&](double X, double Y) {
                const double x = X / sx, y = Y / sy;
                const auto d = field.sample_cubic(x, y);
                return std::pair<double, double>{(x + d[0]) * sx, (y + d[1]) * sy};
            },
            base_.width, base_.height, render::resample::Interp::Bilinear));
        frozen_ = QImage(base_.width, base_.height, QImage::Format_ARGB32);
        frozen_.fill(Qt::transparent);
        bool any = false;
        for (int y = 0; y < base_.height; y += 1) {
            for (int x = 0; x < base_.width; x += 1) {
                const double f = field.freeze_at((x + 0.5) * mm_x(), (y + 0.5) * mm_y());
                if (f <= 0.01) continue;
                any = true;
                frozen_.setPixelColor(x, y, QColor(220, 40, 40, static_cast<int>(110 * f)));
            }
        }
        if (!any) frozen_ = QImage();
        update();
    }

    double page_w_, page_h_;
    render::resample::Plane base_;
    QImage shown_, frozen_;
    std::vector<std::array<double, 3>> points_;
    Json strokes_ = Json::array();
};

}  // namespace

// --- the commands -----------------------------------------------------------------------------------------------------

void MainWindow::build_transform_actions() {
    make("act_free_transform", QStringLiteral("自由変形"), [this] { start_layer_transform(QStringLiteral("free")); }, {QKeySequence(QStringLiteral("Ctrl+T"))},
         QStringLiteral("選んだレイヤー（選択範囲があればその中）を枠のつまみで移動・拡大縮小・回転する。Ctrl で隅だけ・Ctrl+Alt+Shift で遠近法。Enter で確定、Esc でやめる"));
    make("act_transform_again", QStringLiteral("再実行"), [this] { transform_again(); }, {}, QStringLiteral("直前の変形を、選んだレイヤーにもう一度かけます"));
    make("act_transform_scale", QStringLiteral("拡大・縮小"), [this] { start_layer_transform(QStringLiteral("scale")); });
    make("act_transform_rotate", QStringLiteral("回転"), [this] { start_layer_transform(QStringLiteral("rotate")); });
    make("act_transform_skew", QStringLiteral("ゆがみ"), [this] { start_layer_transform(QStringLiteral("skew")); });
    make("act_transform_perspective", QStringLiteral("遠近法"), [this] { start_layer_transform(QStringLiteral("perspective")); });
    make("act_split_warp_cross", QStringLiteral("交差方向に分割ワープ"), [this] { split_warp_box(0); });
    make("act_split_warp_h", QStringLiteral("水平方向に分割ワープ"), [this] { split_warp_box(1); });
    make("act_split_warp_v", QStringLiteral("垂直方向に分割ワープ"), [this] { split_warp_box(2); });
    make("act_remove_warp_split", QStringLiteral("ワープ分割を削除"), [this] { split_warp_box(3); });
    make("act_transform_rotate_180", QStringLiteral("180° 回転"), [this] { turn_layers("180"); });
    make("act_transform_rotate_90cw", QStringLiteral("90° 回転（時計回り）"), [this] { turn_layers("90cw"); });
    make("act_transform_rotate_90ccw", QStringLiteral("90° 回転（反時計回り）"), [this] { turn_layers("90ccw"); });
    make("act_puppet_warp", QStringLiteral("パペットワープ"), [this] { start_layer_transform(QStringLiteral("puppet")); }, {},
         QStringLiteral("クリックでピンを置き、ピンをドラッグして形を曲げる（Alt+クリックでピンを外す）。Enter で確定"));
    make("act_perspective_warp", QStringLiteral("遠近法ワープ"), [this] { start_layer_transform(QStringLiteral("perspective_warp")); }, {},
         QStringLiteral("レイアウトで面（4 隅）を置き、ワープで隅を動かして遠近を直す。Enter で確定"));
    make("act_content_aware_scale", QStringLiteral("コンテンツに応じて拡大・縮小…"), [this] { content_aware_scale_dialog(); },
         {QKeySequence(QStringLiteral("Ctrl+Alt+Shift+C"))}, QStringLiteral("大事な所の形を保ったまま、描く先のペイントのレイヤーを拡大・縮小します"));
    make("act_liquify_filter", QStringLiteral("ゆがみ…"), [this] { liquify_dialog(); }, {QKeySequence(QStringLiteral("Ctrl+Shift+X"))},
         QStringLiteral("前方ワープ・再構築・渦巻き・縮小・膨張などのツールで、描く先のレイヤーを押し流す"));
    make("act_warp_text", QStringLiteral("ワープテキスト…"), [this] { warp_text_dialog(); }, {}, QStringLiteral("選んだ台詞の文字を円弧・旗・波形などの形に曲げます"));
    connect(canvas_, &PageCanvas::transformPreview, this, [this](const Json& spec) { transform_previewed(spec); });
    connect(canvas_, &PageCanvas::transformCommitted, this, [this](const Json& spec) { transform_committed(spec); });
    connect(canvas_, &PageCanvas::transformCancelled, this, [this] { transform_cancelled(); });
}

void MainWindow::build_transform_items(QMenu* edit) {
    // (Photoshop's order, before 変形 ▸; ゆがみ… is Filter's there — Genko has no Filter menu, so it stands here)
    for (const char* name : {"act_content_aware_scale", "act_puppet_warp", "act_perspective_warp", "act_free_transform"})
        edit->addAction(action(QString::fromLatin1(name)));
}

std::vector<std::string> MainWindow::transform_targets() const {
    std::vector<std::string> ids;
    if (layer_panel_ != nullptr) ids = layer_panel_->selected_ids();
    if (ids.size() <= 1) {
        ids.clear();
        if (const core::Layer* layer = target_layer()) ids.push_back(layer->id);
    }
    return ids;
}

Json MainWindow::transform_op(const Json& spec) const {
    const core::Page* page = current_page();
    if (refilter_) {  // (PC3 A5: a smart object's perspective or puppet warp filter set again)
        Json params = spec.contains("puppet") ? spec["puppet"] : spec.contains("perspective") ? spec["perspective"] : Json::object();
        if (spec.contains("puppet")) {
            params["mode"] = puppet_mode_;
            params["density"] = puppet_density_;
            params["expansion_mm"] = puppet_expansion_;
        }
        params["interp"] = transform_interp_;
        return Json{{"op", "smart_filter_set"}, {"page", page != nullptr ? page->index.json() : Json(1)}, {"id", refilter_->first},
                    {"index", refilter_->second}, {"params", params}};
    }
    Json ids = Json::array();
    for (const std::string& id : transform_ids_) ids.push_back(id);
    Json op{{"op", "transform_layers"}, {"page", page != nullptr ? page->index.json() : Json(1)}, {"ids", ids}, {"interp", transform_interp_}};
    const auto& b = transform_box_;
    op["box"] = Json::array({r3(b[0]), r3(b[1]), r3(b[2] - b[0]), r3(b[3] - b[1])});
    if (transform_area_) op["area"] = *transform_area_;
    for (const auto& [key, value] : spec.items()) op[key] = value;
    if (op.contains("puppet")) {
        op["puppet"]["mode"] = puppet_mode_;
        op["puppet"]["density"] = puppet_density_;
        op["puppet"]["expansion_mm"] = puppet_expansion_;
    }
    return op;
}

void MainWindow::start_layer_transform(const QString& mode, const std::vector<std::string>& only) {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    if (canvas_->transforming()) canvas_->cancel_transform();
    refilter_.reset();
    const std::vector<std::string> ids = only.empty() ? transform_targets() : only;
    if (ids.empty()) {
        flash(QStringLiteral("先に変形するレイヤーを選びます"), 3000);
        return;
    }
    std::optional<std::array<double, 4>> box;
    try {
        const std::vector<std::string> moved = lt::layers_moved(*page, ids);
        box = lt::content_box(book(), *page, moved);
    } catch (const std::exception& error) {
        flash(wording::error(QString::fromUtf8(error.what())), 3000, true);
        return;
    }
    transform_area_ = selection_area();
    if (transform_area_) {  // (within the selection: its box, as far as the layers show)
        const auto s = canvas_->selection_box();
        box = box ? std::optional<std::array<double, 4>>(std::array<double, 4>{std::max((*box)[0], s[0]), std::max((*box)[1], s[1]), std::min((*box)[2], s[2]),
                                                                                std::min((*box)[3], s[3])})
                  : std::nullopt;
    }
    if (!box || !((*box)[2] > (*box)[0] && (*box)[3] > (*box)[1])) {
        flash(QStringLiteral("変形するものがありません"), 3000);
        return;
    }
    // (PC3 A5: a smart object's warp edited from the one it holds, over its box on the page)
    std::optional<core::page_geometry::Warp> held;
    if (mode == QLatin1String("warp") && !transform_area_ && ids.size() == 1) {
        for (const core::Layer& layer : page->layers) {
            if (layer.id == ids.front() && layer.kind == core::LayerKind::Smart) held = lt::smart_warp_on_page(book(), layer);
        }
        if (held) box = held->box;
    }
    transform_ids_ = ids;
    transform_box_ = *box;
    try {  // (PC3 C-d: what the box moves is not snapped to)
        canvas_->snap_moving = lt::layers_moved(*page, ids);
    } catch (const std::exception&) {
    }
    canvas_->start_transform(mode, *box);
    if (held) canvas_->set_transform_warp(lt::warp_to_json(*held));
    show_transform_bar(mode);
    canvas_->setFocus();
    static const std::map<QString, QString> hints{
        {QStringLiteral("puppet"), QStringLiteral("クリックでピンを置き、ピンをドラッグして曲げます。Enter で確定、Esc でやめます")},
        {QStringLiteral("perspective_warp"), QStringLiteral("面の隅を動かして置き（何もない所をドラッグで面を足す）、「ワープ」で隅を動かします。Enter で確定")},
        {QStringLiteral("warp"), QStringLiteral("点やハンドルを引っぱって曲げます（上の欄で形を選べます）。Enter で確定、Esc でやめます")}};
    const auto hint = hints.find(mode);
    flash(hint != hints.end() ? hint->second : QStringLiteral("枠のつまみで変形し、Enter で確定、Esc でやめます"), 5000);
}

void MainWindow::transform_previewed(const Json& spec) {
    try {
        preview_ops(Json::array({transform_op(spec)}));
    } catch (const std::exception&) {
    }
}

void MainWindow::transform_committed(const Json& spec) {
    hide_transform_bar();
    preview_ops(std::nullopt);
    const Json op = transform_op(spec);
    const std::optional<Json> area = transform_area_;
    if (refilter_) {  // (PC3 A5: a smart filter set again — not one for 再実行)
        refilter_.reset();
        apply_ops(Json::array({op}));
        return;
    }
    if (apply_ops(Json::array({op}))) {
        last_transform_ = spec;
        if (area) {
            const auto m = spec.contains("matrix") ? std::optional<Json>(spec["matrix"]) : std::nullopt;
            if (m && canvas_->selection()) {  // (the selection goes with what it moved)
                std::array<double, 6> mm{};
                for (std::size_t i = 0; i < 6; ++i) mm[i] = (*m)[i].get<double>();
                std::vector<QPointF> moved;
                for (const QPointF& p : canvas_->selection()->outline) moved.emplace_back(mm[0] * p.x() + mm[2] * p.y() + mm[4], mm[1] * p.x() + mm[3] * p.y() + mm[5]);
                canvas_->set_selection(moved_area(*area, mm), moved);
            } else {
                canvas_->set_selection(std::nullopt);
            }
        }
    }
}

void MainWindow::transform_cancelled() {
    hide_transform_bar();
    preview_ops(std::nullopt);
    refilter_.reset();
}

// --- a smart object's (PC3 A5) -------------------------------------------------------------------------------------------

void MainWindow::smart_bend(const QString& mode) {
    const core::Layer* layer = target_layer();
    if (current_page() == nullptr || layer == nullptr || layer->kind != core::LayerKind::Smart) {
        flash(QStringLiteral("先にスマートオブジェクトを選びます"), 3000);
        return;
    }
    start_layer_transform(mode, {layer->id});
}

void MainWindow::edit_bend_filter(const std::string& id, int index) {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    const core::Layer* layer = nullptr;
    for (const core::Layer& l : page->layers) {
        if (l.id == id && l.kind == core::LayerKind::Smart) layer = &l;
    }
    const Json* filters = layer != nullptr && layer->smart ? core::get(*layer->smart, "filters") : nullptr;
    if (filters == nullptr || !filters->is_array() || index < 0 || index >= static_cast<int>(filters->size())) return;
    const Json f = (*filters)[static_cast<std::size_t>(index)];
    const std::string kind = core::py_str(core::py_get(f, "kind", Json("")));
    // (its settings as the page has them: its places brought from the smart object's own mm — independent review A P2-1)
    const Json params = render::smart_filters::params_on_page(book(), *page, *layer, f);
    if (kind == "liquify") {
        liquify_dialog(std::make_pair(id, index));
        return;
    }
    if (kind != "puppet_warp" && kind != "perspective_warp") return;
    const auto point = [](const Json& p) { return QPointF(p[0].get<double>(), p[1].get<double>()); };
    if (params.contains("interp") && params["interp"].is_string()) transform_interp_ = params["interp"].get<std::string>();
    if (kind == "puppet_warp") {
        puppet_mode_ = params.value("mode", std::string("normal"));
        puppet_density_ = params.value("density", std::string("normal"));
        if (params.contains("expansion_mm") && params["expansion_mm"].is_number()) puppet_expansion_ = params["expansion_mm"].get<double>();
        start_layer_transform(QStringLiteral("puppet"), {id});
        if (!canvas_->transforming()) return;
        refilter_ = std::make_pair(id, index);
        std::vector<std::pair<QPointF, QPointF>> pins;
        if (params.contains("pins") && params["pins"].is_array())
            for (const Json& pin : params["pins"]) pins.emplace_back(point(pin["src"]), point(pin["dst"]));
        canvas_->set_puppet_pins(pins);
    } else {
        start_layer_transform(QStringLiteral("perspective_warp"), {id});
        if (!canvas_->transforming()) return;
        refilter_ = std::make_pair(id, index);
        std::vector<std::array<QPointF, 4>> src, dst;
        if (params.contains("planes") && params["planes"].is_array()) {
            for (const Json& plane : params["planes"]) {
                std::array<QPointF, 4> a, b;
                for (std::size_t k = 0; k < 4; ++k) {
                    a[k] = point(plane["src"][k]);
                    b[k] = point(plane["dst"][k]);
                }
                src.push_back(a);
                dst.push_back(b);
            }
        }
        if (!src.empty()) canvas_->set_perspective_planes(src, dst);
        if (transform_bar_ != nullptr) {  // (its corners where they went: the ワープ phase)
            if (auto* warp = transform_bar_->findChild<QToolButton*>(QStringLiteral("perspective_warp_phase"))) warp->setChecked(true);
        }
    }
    flash(QStringLiteral("スマートフィルター「%1」を直しています。Enter で確定、Esc でやめます")
              .arg(kind == "puppet_warp" ? QStringLiteral("パペットワープ") : QStringLiteral("遠近法ワープ")),
          5000);
    transform_previewed(canvas_->transform_spec());
}

void MainWindow::transform_again() {
    if (!last_transform_) {
        flash(QStringLiteral("まだ変形していません"), 2500);
        return;
    }
    const core::Page* page = current_page();
    const std::vector<std::string> ids = transform_targets();
    if (page == nullptr || ids.empty()) return;
    try {
        const auto box = lt::content_box(book(), *page, lt::layers_moved(*page, ids));
        if (!box) {
            flash(QStringLiteral("変形するものがありません"), 3000);
            return;
        }
        transform_box_ = *box;
    } catch (const std::exception& error) {
        flash(wording::error(QString::fromUtf8(error.what())), 3000, true);
        return;
    }
    transform_ids_ = ids;
    transform_area_ = selection_area();
    apply_ops(Json::array({transform_op(*last_transform_)}));
}

void MainWindow::turn_layers(const char* turn) {
    const core::Page* page = current_page();
    const std::vector<std::string> ids = transform_targets();
    if (page == nullptr || ids.empty()) {
        flash(QStringLiteral("先に変形するレイヤーを選びます"), 3000);
        return;
    }
    const auto area = selection_area();
    Json list = Json::array();
    for (const std::string& id : ids) list.push_back(id);
    Json op{{"op", "transform_layers"}, {"page", page->index.json()}, {"ids", list}, {"turn", turn}};
    if (area) {
        const auto [x0, y0, x1, y1] = canvas_->selection_box();
        op["area"] = *area;
        op["box"] = Json::array({r3(x0), r3(y0), r3(x1 - x0), r3(y1 - y0)});
    }
    const std::vector<QPointF> outline = area ? canvas_->selection()->outline : std::vector<QPointF>{};
    if (!apply_ops(Json::array({op}))) return;
    last_transform_ = Json{{"turn", turn}};
    if (!area) return;
    // (the selection turned with it: the same map, about the box's middle)
    const auto [x0, y0, x1, y1] = canvas_->selection_box();
    const double cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    std::array<double, 6> m{1, 0, 0, 1, 0, 0};
    const std::string t = turn;
    if (t == "flip_h") m = {-1, 0, 0, 1, 2 * cx, 0};
    if (t == "flip_v") m = {1, 0, 0, -1, 0, 2 * cy};
    if (t == "180") m = {-1, 0, 0, -1, 2 * cx, 2 * cy};
    if (t == "90cw") m = {0, 1, -1, 0, cx + cy, cy - cx};
    if (t == "90ccw") m = {0, -1, 1, 0, cx - cy, cx + cy};
    std::vector<QPointF> moved;
    for (const QPointF& p : outline) moved.emplace_back(m[0] * p.x() + m[2] * p.y() + m[4], m[1] * p.x() + m[3] * p.y() + m[5]);
    canvas_->set_selection(moved_area(*area, m), moved);
}

void MainWindow::split_warp_box(int how) {
    if (canvas_->transform_mode() != QLatin1String("warp")) {
        flash(QStringLiteral("ワープの最中に使います（編集 → 変形 → ワープ）"), 3000);
        return;
    }
    try {
        const auto box = canvas_->transform_box();
        const auto warp = lt::warp_from_json(canvas_->transform_warp(), box);
        std::optional<lt::XY> at;
        if (const auto p = canvas_->transform_point()) at = lt::XY{p->x(), p->y()};
        const auto split = lt::split_warp(warp, at, static_cast<lt::Split>(how));
        if (!split) {
            flash(QStringLiteral("そこでは分割できません"), 2500);
            return;
        }
        canvas_->set_transform_warp(lt::warp_to_json(*split));
        transform_previewed(canvas_->transform_spec());
    } catch (const std::exception& error) {
        flash(wording::error(QString::fromUtf8(error.what())), 3000, true);
    }
}

// --- the selection's own (変形を数で決める…, 自由な形に and ワープ over a selection, 水平方向に反転…) --------------------------

namespace {

// A grid of points pulled (the selection's ワープ: columns × rows points, row by row) as a Bézier mesh over its box: each
// cell a patch whose control points lie on the bilinear surface of its four corners.
Json mesh_of_grid(const Json& points, int across, int down, const std::array<double, 4>& box) {
    const int cx = across - 1, cy = down - 1;
    const std::size_t nx = std::size_t(3 * cx + 1), ny = std::size_t(3 * cy + 1);
    const auto at = [&](int i, int j) {
        const Json& p = points[std::size_t(j) * std::size_t(across) + std::size_t(i)];
        return std::pair<double, double>{p[0].get<double>(), p[1].get<double>()};
    };
    Json us = Json::array(), vs = Json::array(), out = Json::array();
    for (int i = 0; i <= cx; ++i) us.push_back(static_cast<double>(i) / cx);
    for (int j = 0; j <= cy; ++j) vs.push_back(static_cast<double>(j) / cy);
    for (std::size_t r = 0; r < ny; ++r) {
        for (std::size_t c = 0; c < nx; ++c) {
            const int i = std::min(static_cast<int>(c / 3), cx - 1), j = std::min(static_cast<int>(r / 3), cy - 1);
            const double s = (static_cast<double>(c) - 3.0 * i) / 3.0, t = (static_cast<double>(r) - 3.0 * j) / 3.0;
            const auto a = at(i, j), b = at(i + 1, j), d = at(i, j + 1), e = at(i + 1, j + 1);
            const double x = (a.first * (1 - s) + b.first * s) * (1 - t) + (d.first * (1 - s) + e.first * s) * t;
            const double y = (a.second * (1 - s) + b.second * s) * (1 - t) + (d.second * (1 - s) + e.second * s) * t;
            out.push_back(Json::array({r3(x), r3(y)}));
        }
    }
    return Json{{"style", "custom"}, {"bounds", Json::array({box[0], box[1], box[2], box[3]})}, {"mesh", Json{{"us", us}, {"vs", vs}, {"points", out}}}};
}

}  // namespace

bool MainWindow::transform_within_selection(const Json& spec) {
    const auto area = selection_area();
    const core::Page* page = current_page();
    const std::vector<std::string> ids = transform_targets();
    if (!area || page == nullptr || ids.empty() || !canvas_->selection()) return false;
    const auto [x0, y0, x1, y1] = canvas_->selection_box();
    Json list = Json::array();
    for (const std::string& id : ids) list.push_back(id);
    Json op{{"op", "transform_layers"}, {"page", page->index.json()}, {"ids", list}, {"area", *area},
            {"box", Json::array({r3(x0), r3(y0), r3(x1 - x0), r3(y1 - y0)})}, {"interp", transform_interp_}};
    for (const auto& [key, value] : spec.items()) op[key] = value;
    if (op.contains("grid_warp")) {  // (the selection's grid of points: a Bézier mesh)
        const Json& w = op["grid_warp"];
        op["warp"] = mesh_of_grid(w["mesh"], w.contains("grid") ? w["grid"][0].get<int>() : 3, w.contains("grid") ? w["grid"][1].get<int>() : 3,
                                  {x0, y0, x1, y1});
        op.erase("grid_warp");
    }
    const std::vector<QPointF> outline = canvas_->selection()->outline;
    if (!apply_ops(Json::array({op}))) return false;
    Json again = Json::object();  // (再実行 sends the map again: the grid made a mesh)
    for (const char* key : {"matrix", "quad", "turn", "warp"})
        if (op.contains(key)) again[key] = op[key];
    last_transform_ = again;
    if (spec.contains("matrix")) {  // (the selection goes with what it moved)
        std::array<double, 6> m{};
        for (std::size_t i = 0; i < 6; ++i) m[i] = spec["matrix"][i].get<double>();
        std::vector<QPointF> moved;
        for (const QPointF& p : outline) moved.emplace_back(m[0] * p.x() + m[2] * p.y() + m[4], m[1] * p.x() + m[3] * p.y() + m[5]);
        canvas_->set_selection(moved_area(*area, m), moved);
    } else if (!spec.contains("turn")) {
        canvas_->set_selection(std::nullopt);  // (a free transform lets the selection go)
    }
    return true;
}

// --- the options bar ---------------------------------------------------------------------------------------------------

void MainWindow::hide_transform_bar() {
    if (transform_bar_ != nullptr) transform_bar_->hide();
}

void MainWindow::show_transform_bar(const QString& mode) {
    if (transform_bar_ == nullptr) {
        transform_bar_ = new QToolBar(QStringLiteral("変形のオプション"), this);
        transform_bar_->setObjectName(QStringLiteral("transform_bar"));
        addToolBar(Qt::TopToolBarArea, transform_bar_);
    }
    transform_bar_->clear();
    auto* interp = new QComboBox;
    interp->setObjectName(QStringLiteral("transform_interp"));
    for (const auto& [key, label] : {std::pair{"bicubic", "バイキュービック"}, {"bilinear", "バイリニア"}, {"nearest", "ニアレストネイバー"}})
        interp->addItem(QString::fromUtf8(label), QString::fromLatin1(key));
    interp->setCurrentIndex(std::max(0, interp->findData(QString::fromStdString(transform_interp_))));
    connect(interp, &QComboBox::currentIndexChanged, this, [this, interp] {
        transform_interp_ = interp->currentData().toString().toStdString();
        if (canvas_->transforming()) transform_previewed(canvas_->transform_spec());
    });
    transform_bar_->addWidget(new QLabel(QStringLiteral("補間方法 ")));
    transform_bar_->addWidget(interp);
    if (mode == QLatin1String("warp")) {
        auto* style = new QComboBox;
        style->setObjectName(QStringLiteral("warp_style"));
        style->addItem(QStringLiteral("カスタム"), QStringLiteral("custom"));
        for (const auto& [key, label] : warp_styles()) style->addItem(label, QString::fromLatin1(key));
        auto* bend = percent_box(QStringLiteral("warp_bend"), -100, 100, 50);
        auto* h = percent_box(QStringLiteral("warp_h"), -100, 100, 0);
        auto* v = percent_box(QStringLiteral("warp_v"), -100, 100, 0);
        auto* vertical = new QCheckBox(QStringLiteral("垂直方向"));
        vertical->setObjectName(QStringLiteral("warp_vertical"));
        // (PC3 A5: the warp it starts from — a smart object's own — shown in the fields, before they say a change)
        if (const Json now = canvas_->transform_warp(); now.is_object() && now.contains("style") && now["style"].is_string()) {
            const std::string name = now["style"].get<std::string>();
            if (const int at = style->findData(QString::fromStdString(name)); at >= 0) style->setCurrentIndex(at);
            if (name != "custom") {
                bend->setValue(now.value("bend", 50.0));
                h->setValue(now.value("h_distort", 0.0));
                v->setValue(now.value("v_distort", 0.0));
                vertical->setChecked(now.value("vertical", false));
            }
        }
        const auto changed = [this, style, bend, h, v, vertical] {
            const QString key = style->currentData().toString();
            const auto box = canvas_->transform_box();
            Json spec = key == QLatin1String("custom") ? Json{{"style", "custom"}}
                                                       : Json{{"style", key.toStdString()}, {"bend", bend->value()}, {"h_distort", h->value()}, {"v_distort", v->value()},
                                                              {"vertical", vertical->isChecked()}};
            canvas_->set_transform_warp(lt::warp_to_json(lt::warp_from_json(spec, box)));
            transform_previewed(canvas_->transform_spec());
        };
        connect(style, &QComboBox::currentIndexChanged, this, changed);
        for (QDoubleSpinBox* box : {bend, h, v}) connect(box, &QDoubleSpinBox::valueChanged, this, changed);
        connect(vertical, &QCheckBox::toggled, this, changed);
        transform_bar_->addSeparator();
        transform_bar_->addWidget(new QLabel(QStringLiteral("ワープ ")));
        transform_bar_->addWidget(style);
        transform_bar_->addWidget(new QLabel(QStringLiteral(" カーブ ")));
        transform_bar_->addWidget(bend);
        transform_bar_->addWidget(new QLabel(QStringLiteral(" 水平方向のゆがみ ")));
        transform_bar_->addWidget(h);
        transform_bar_->addWidget(new QLabel(QStringLiteral(" 垂直方向のゆがみ ")));
        transform_bar_->addWidget(v);
        transform_bar_->addWidget(vertical);
        transform_bar_->addSeparator();
        for (const char* name : {"act_split_warp_cross", "act_split_warp_h", "act_split_warp_v", "act_remove_warp_split"}) transform_bar_->addAction(action(QString::fromLatin1(name)));
    }
    if (mode == QLatin1String("puppet")) {
        auto* kind = new QComboBox;
        kind->setObjectName(QStringLiteral("puppet_mode"));
        for (const auto& [key, label] : {std::pair{"normal", "標準"}, {"rigid", "固定"}, {"distort", "変形"}}) kind->addItem(QString::fromUtf8(label), QString::fromLatin1(key));
        kind->setCurrentIndex(std::max(0, kind->findData(QString::fromStdString(puppet_mode_))));
        auto* density = new QComboBox;
        density->setObjectName(QStringLiteral("puppet_density"));
        for (const auto& [key, label] : {std::pair{"normal", "標準"}, {"fewer", "少なく"}, {"more", "多く"}}) density->addItem(QString::fromUtf8(label), QString::fromLatin1(key));
        density->setCurrentIndex(std::max(0, density->findData(QString::fromStdString(puppet_density_))));
        auto* expansion = new QDoubleSpinBox;
        expansion->setObjectName(QStringLiteral("puppet_expansion"));
        expansion->setRange(-25, 25);
        expansion->setDecimals(2);
        expansion->setSuffix(QStringLiteral(" mm"));
        expansion->setValue(puppet_expansion_);
        const auto changed = [this, kind, density, expansion] {
            puppet_mode_ = kind->currentData().toString().toStdString();
            puppet_density_ = density->currentData().toString().toStdString();
            puppet_expansion_ = expansion->value();
            if (canvas_->transforming()) transform_previewed(canvas_->transform_spec());
        };
        connect(kind, &QComboBox::currentIndexChanged, this, changed);
        connect(density, &QComboBox::currentIndexChanged, this, changed);
        connect(expansion, &QDoubleSpinBox::valueChanged, this, changed);
        transform_bar_->addSeparator();
        transform_bar_->addWidget(new QLabel(QStringLiteral("モード ")));
        transform_bar_->addWidget(kind);
        transform_bar_->addWidget(new QLabel(QStringLiteral(" 密度 ")));
        transform_bar_->addWidget(density);
        transform_bar_->addWidget(new QLabel(QStringLiteral(" 拡張 ")));
        transform_bar_->addWidget(expansion);
    }
    if (mode == QLatin1String("perspective_warp")) {
        auto* layout = new QToolButton;
        layout->setObjectName(QStringLiteral("perspective_layout"));
        layout->setText(QStringLiteral("レイアウト"));
        layout->setCheckable(true);
        layout->setChecked(true);
        auto* warp = new QToolButton;
        warp->setObjectName(QStringLiteral("perspective_warp_phase"));
        warp->setText(QStringLiteral("ワープ"));
        warp->setCheckable(true);
        auto* group = new QButtonGroup(transform_bar_);
        group->addButton(layout);
        group->addButton(warp);
        connect(warp, &QToolButton::toggled, this, [this](bool on) {
            canvas_->set_perspective_warping(on);
            if (on) transform_previewed(canvas_->transform_spec());
        });
        transform_bar_->addSeparator();
        transform_bar_->addWidget(layout);
        transform_bar_->addWidget(warp);
        for (const auto& [key, label] : {std::pair{"horizontal", "水平方向に揃える"}, {"vertical", "垂直方向に揃える"}, {"auto", "水平・垂直方向に揃える"}}) {
            auto* button = new QToolButton;
            button->setObjectName(QStringLiteral("perspective_straighten_%1").arg(QString::fromLatin1(key)));
            button->setText(QString::fromUtf8(label));
            const std::string way = key;
            connect(button, &QToolButton::clicked, this, [this, way, warp] {
                warp->setChecked(true);
                std::vector<render::perspective::Plane> planes;
                const auto src = canvas_->perspective_planes(false), dst = canvas_->perspective_planes(true);
                for (std::size_t i = 0; i < src.size(); ++i) {
                    render::perspective::Plane p;
                    for (std::size_t k = 0; k < 4; ++k) {
                        p.src[k] = {src[i][k].x(), src[i][k].y()};
                        p.dst[k] = {dst[i][k].x(), dst[i][k].y()};
                    }
                    planes.push_back(p);
                }
                render::perspective::straighten(planes, *render::perspective::straighten_from(way), 25.4 / 400.0);
                std::vector<std::array<QPointF, 4>> s2, d2;
                for (const auto& p : planes) {
                    std::array<QPointF, 4> a, b;
                    for (std::size_t k = 0; k < 4; ++k) {
                        a[k] = QPointF(r3(p.src[k][0]), r3(p.src[k][1]));
                        b[k] = QPointF(r3(p.dst[k][0]), r3(p.dst[k][1]));
                    }
                    s2.push_back(a);
                    d2.push_back(b);
                }
                canvas_->set_perspective_planes(s2, d2);
                transform_previewed(canvas_->transform_spec());
            });
            transform_bar_->addWidget(button);
        }
    }
    transform_bar_->addSeparator();
    auto* done = new QToolButton;
    done->setObjectName(QStringLiteral("transform_commit"));
    done->setText(QStringLiteral("○ 確定"));
    connect(done, &QToolButton::clicked, this, [this] { canvas_->finish_transform(); });
    auto* cancel = new QToolButton;
    cancel->setObjectName(QStringLiteral("transform_cancel"));
    cancel->setText(QStringLiteral("× やめる"));
    connect(cancel, &QToolButton::clicked, this, [this] { canvas_->cancel_transform(); });
    transform_bar_->addWidget(done);
    transform_bar_->addWidget(cancel);
    transform_bar_->show();
}

// --- コンテンツに応じて拡大・縮小 ----------------------------------------------------------------------------------------

void MainWindow::content_aware_scale_dialog() {
    const core::Page* page = current_page();
    const core::Layer* layer = paint_layer();
    if (page == nullptr || layer == nullptr) return;
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("content_aware_scale_dialog"));
    dialog.setWindowTitle(QStringLiteral("コンテンツに応じて拡大・縮小"));
    auto* form = new QFormLayout(&dialog);
    auto* sx = percent_box(QStringLiteral("cas_scale_x"), 1, 400, 100);
    auto* sy = percent_box(QStringLiteral("cas_scale_y"), 1, 400, 100);
    auto* amount = percent_box(QStringLiteral("cas_amount"), 0, 100, 100);
    auto* protect = new QComboBox;
    protect->setObjectName(QStringLiteral("cas_protect"));
    protect->addItem(QStringLiteral("なし"), QString());
    for (const core::Channel& channel : page->channels)
        if (channel.kind == core::ChannelKind::Alpha && !channel.quick_mask) protect->addItem(QString::fromStdString(channel.name), QString::fromStdString(channel.id));
    auto* skin = new QCheckBox(QStringLiteral("スキントーンを保護"));
    skin->setObjectName(QStringLiteral("cas_skin"));
    form->addRow(QStringLiteral("幅"), sx);
    form->addRow(QStringLiteral("高さ"), sy);
    form->addRow(QStringLiteral("量"), amount);
    form->addRow(QStringLiteral("保護"), protect);
    form->addRow(skin);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);
    const std::string id = layer->id;
    const Json index = page->index.json();
    const std::optional<Json> area = selection_area();
    const auto op_of = [=] {
        Json op{{"op", "content_aware_scale"}, {"page", index}, {"id", id}, {"scale_x", sx->value()}, {"scale_y", sy->value()}, {"amount", amount->value()}};
        if (!protect->currentData().toString().isEmpty()) op["protect"] = protect->currentData().toString().toStdString();
        if (skin->isChecked()) op["protect_skin"] = true;
        if (area) op["area"] = *area;
        return op;
    };
    const auto preview = [this, op_of] { preview_ops(Json::array({op_of()})); };
    for (QDoubleSpinBox* box : {sx, sy, amount}) connect(box, &QDoubleSpinBox::editingFinished, this, preview);
    const Asked asked = asking();
    const int answer = ask::exec(&dialog);
    preview_ops(std::nullopt);
    if (answer != QDialog::Accepted || !still(asked)) return;
    apply_ops(Json::array({op_of()}));
}

// --- フィルター → ゆがみ… --------------------------------------------------------------------------------------------------

void MainWindow::liquify_dialog(const std::optional<std::pair<std::string, int>>& refilter) {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    // (PC3 A5: a smart object's — its strokes kept as its smart filter, the picture shown with its filters below them)
    const core::Layer* layer = nullptr;
    const core::Layer* target = target_layer();
    if (refilter) {
        for (const core::Layer& l : page->layers) {
            if (l.id == refilter->first && l.kind == core::LayerKind::Smart) layer = &l;
        }
    } else if (target != nullptr && target->kind == core::LayerKind::Smart) {
        if (target->locked) {
            flash(QStringLiteral("ロックされているレイヤーは変形できません（レイヤー パネルでロックを外します）"), 3000);
            return;
        }
        layer = target;
    } else {
        layer = paint_layer();
    }
    if (layer == nullptr) return;
    const double pw = page->spec.width_mm.value(), ph = page->spec.height_mm.value();
    const int dpi = std::clamp(static_cast<int>(640.0 / std::max(pw, ph) * 25.4), 10, 200);
    render::Image picture;
    Json kept;  // (the strokes of the filter edited again)
    try {
        core::Layer shown = *layer;
        shown.visible = true;
        if (refilter && shown.smart && shown.smart->contains("filters") && (*shown.smart)["filters"].is_array()) {
            Json& filters = (*shown.smart)["filters"];
            if (refilter->second < 0 || refilter->second >= static_cast<int>(filters.size())) return;
            kept = core::py_get(render::smart_filters::params_on_page(book(), *page, *layer, filters[static_cast<std::size_t>(refilter->second)]), "strokes",
                                Json::array());
            filters.erase(filters.begin() + refilter->second, filters.end());
        }
        picture = render::layer_image(*page, shown, dpi, &book(), true);
    } catch (const std::exception& error) {
        flash(wording::error(QString::fromUtf8(error.what())), 3000, true);
        return;
    }
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("liquify_dialog"));
    dialog.setWindowTitle(QStringLiteral("ゆがみ"));
    auto* outer = new QHBoxLayout(&dialog);
    auto* tools = new QVBoxLayout;
    auto* group = new QButtonGroup(&dialog);
    const std::vector<std::pair<const char*, QString>> kinds{
        {"forward_warp", QStringLiteral("前方ワープツール")}, {"reconstruct", QStringLiteral("再構築ツール")},  {"smooth", QStringLiteral("スムーズツール")},
        {"twirl_cw", QStringLiteral("渦巻きツール（時計回り）")}, {"twirl_ccw", QStringLiteral("渦巻きツール（反時計回り）")}, {"pucker", QStringLiteral("縮小ツール")},
        {"bloat", QStringLiteral("膨張ツール")},             {"push_left", QStringLiteral("左方向にプッシュツール")}, {"freeze", QStringLiteral("マスクツール")},
        {"thaw", QStringLiteral("マスク解除ツール")}};
    int n = 0;
    for (const auto& [key, label] : kinds) {
        auto* button = new QRadioButton(label);
        button->setObjectName(QStringLiteral("liquify_tool_%1").arg(QString::fromLatin1(key)));
        button->setProperty("tool", QString::fromLatin1(key));
        button->setChecked(n == 0);
        group->addButton(button, n++);
        tools->addWidget(button);
    }
    auto* size = new QDoubleSpinBox;
    size->setObjectName(QStringLiteral("liquify_brush_size"));
    size->setRange(0.2, 200);
    size->setSuffix(QStringLiteral(" mm"));
    size->setValue(10.0);
    auto* density = new QSpinBox;
    density->setObjectName(QStringLiteral("liquify_brush_density"));
    density->setRange(0, 100);
    density->setValue(50);
    auto* pressure = new QSpinBox;
    pressure->setObjectName(QStringLiteral("liquify_brush_pressure"));
    pressure->setRange(1, 100);
    pressure->setValue(100);
    auto* rate = new QSpinBox;
    rate->setObjectName(QStringLiteral("liquify_brush_rate"));
    rate->setRange(0, 100);
    rate->setValue(80);
    auto* brush = new QFormLayout;
    brush->addRow(QStringLiteral("サイズ"), size);
    brush->addRow(QStringLiteral("密度"), density);
    brush->addRow(QStringLiteral("筆圧"), pressure);
    brush->addRow(QStringLiteral("速度"), rate);
    tools->addLayout(brush);
    auto* view = new LiquifyView(picture, pw, ph);
    view->brush = [=] {
        render::liquify::Stroke s;
        s.tool = *render::liquify::tool_from(group->checkedButton()->property("tool").toString().toStdString());
        s.size = size->value();
        s.density = density->value();
        s.pressure = pressure->value();
        s.rate = rate->value();
        return s;
    };
    for (const auto& [name, label, tool] : {std::tuple{"liquify_restore_all", "すべてを復元", "reconstruct_all"}, {"liquify_mask_all", "すべてをマスク", "freeze_all"},
                                             {"liquify_mask_invert", "マスクを反転", "invert_freeze"}, {"liquify_mask_none", "マスクなし", "thaw_all"}}) {
        auto* button = new QToolButton;
        button->setObjectName(QString::fromLatin1(name));
        button->setText(QString::fromUtf8(label));
        const std::string kind = tool;
        connect(button, &QToolButton::clicked, &dialog, [view, kind] {
            if (kind == "reconstruct_all") {
                view->add_global("reconstruct_all", 100.0);
            } else {
                view->add_global(kind.c_str());
            }
        });
        tools->addWidget(button);
    }
    tools->addStretch(1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    tools->addWidget(buttons);
    outer->addLayout(tools);
    outer->addWidget(view);
    if (kept.is_array() && !kept.empty()) view->set_strokes(kept);
    const std::string id = layer->id;
    const Json index = page->index.json();
    const std::optional<Json> area = selection_area();
    const Asked asked = asking();
    if (ask::exec(&dialog) != QDialog::Accepted || !still(asked)) return;
    if (view->strokes().empty()) return;
    if (refilter) {  // (PC3 A5: the smart filter's strokes set again)
        apply_ops(Json::array({Json{{"op", "smart_filter_set"}, {"page", index}, {"id", id}, {"index", refilter->second},
                                    {"params", Json{{"strokes", view->strokes()}}}}}));
        return;
    }
    Json op{{"op", "liquify"}, {"page", index}, {"layer_id", id}, {"strokes", view->strokes()}};
    if (area) op["area"] = *area;
    apply_ops(Json::array({op}));
}

// --- 台詞 → ワープテキスト… ----------------------------------------------------------------------------------------------

void MainWindow::warp_text_dialog() {
    // (PC5 G-a review P2-6: the chosen type layer too — edit_type's warp —, else the chosen line)
    const std::optional<Json> target = type_target(false);
    const core::StoryLine* line = target ? type_record(*target) : selected_line_or_say();
    if (line == nullptr) return;
    const std::optional<Json> layer_target = target && target->contains("layer_id") ? target : std::nullopt;
    const Json now = line->style.is_object() && line->style.contains("native_warp") ? line->style["native_warp"] : Json();
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("warp_text_dialog"));
    dialog.setWindowTitle(QStringLiteral("ワープテキスト"));
    auto* form = new QFormLayout(&dialog);
    auto* style = new QComboBox;
    style->setObjectName(QStringLiteral("warp_text_style"));
    style->addItem(QStringLiteral("なし"), QString());
    for (const auto& [key, label] : warp_styles()) style->addItem(label, QString::fromLatin1(key));
    if (now.is_object()) style->setCurrentIndex(std::max(0, style->findData(QString::fromStdString(now.value("style", std::string())))));
    auto* horizontal = new QRadioButton(QStringLiteral("水平方向"));
    auto* vertical = new QRadioButton(QStringLiteral("垂直方向"));
    horizontal->setObjectName(QStringLiteral("warp_text_horizontal"));
    vertical->setObjectName(QStringLiteral("warp_text_vertical"));
    (now.is_object() && now.value("vertical", false) ? vertical : horizontal)->setChecked(true);
    auto* way = new QHBoxLayout;
    way->addWidget(horizontal);
    way->addWidget(vertical);
    auto* bend = percent_box(QStringLiteral("warp_text_bend"), -100, 100, now.is_object() ? now.value("bend", 50.0) : 50.0);
    auto* h = percent_box(QStringLiteral("warp_text_h"), -100, 100, now.is_object() ? now.value("h_distort", 0.0) : 0.0);
    auto* v = percent_box(QStringLiteral("warp_text_v"), -100, 100, now.is_object() ? now.value("v_distort", 0.0) : 0.0);
    form->addRow(QStringLiteral("スタイル"), style);
    form->addRow(way);
    form->addRow(QStringLiteral("カーブ"), bend);
    form->addRow(QStringLiteral("水平方向のゆがみ"), h);
    form->addRow(QStringLiteral("垂直方向のゆがみ"), v);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);
    const std::string id = line->id;
    const auto op_of = [=] {
        const QString key = style->currentData().toString();
        const Json warp = key.isEmpty() ? Json() : Json{{"style", key.toStdString()}, {"bend", bend->value()}, {"h_distort", h->value()}, {"v_distort", v->value()},
                                                       {"vertical", vertical->isChecked()}};
        if (layer_target) return Json{{"op", "edit_type"}, {"page", (*layer_target)["page"]}, {"layer_id", (*layer_target)["layer_id"]}, {"warp", warp}};
        return Json{{"op", "edit_line"}, {"id", id}, {"style", Json{{"native_warp", warp}}}};
    };
    const auto preview = [this, op_of] { preview_ops(Json::array({op_of()})); };
    connect(style, &QComboBox::currentIndexChanged, this, preview);
    for (QDoubleSpinBox* box : {bend, h, v}) connect(box, &QDoubleSpinBox::valueChanged, this, preview);
    connect(vertical, &QRadioButton::toggled, this, preview);
    const Asked asked = asking();
    const int answer = ask::exec(&dialog);
    preview_ops(std::nullopt);
    if (answer != QDialog::Accepted || !still(asked)) return;
    apply_ops(Json::array({op_of()}));
}

}  // namespace genko::app
