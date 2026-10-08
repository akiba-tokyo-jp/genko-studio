#pragma once

#include <QWidget>

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"

class QComboBox;
class QGridLayout;

// The dialogs of the layer panel (Python's genko/app/main.py filter_params, gradient_dialog, gradient_map_dialog,
// screen_dialog; filter_dialog.py; gradient_editor.py): the numbers of a filter or a correction layer (with a tone
// curve to drag, the layer's lightness chart, and the result shown on the page while they change), a gradient's
// colours in a row, a layer's halftone (トーン化).

namespace genko::app {

// A gradient's colour: where it sits (0..1), its colour, how strong (0..1).
struct Stop {
    double at = 0;
    std::array<int, 3> rgb{0, 0, 0};
    double opacity = 1;
};

// トーンカーブ: click to add a point, drag to move it, right-click to take it away; the two ends always stay.
class CurveEditor : public QWidget {
    Q_OBJECT
public:
    static constexpr int kSide = 200;
    explicit CurveEditor(std::vector<std::array<double, 2>> points = {{0, 0}, {255, 255}}, QWidget* parent = nullptr);
    const std::vector<std::array<double, 2>>& points() const { return points_; }
signals:
    void changed();
protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
private:
    QPointF to_view(double x, double y) const;
    std::array<double, 2> to_value(QPointF pos) const;
    std::optional<std::size_t> near(QPointF pos) const;
    std::vector<std::array<double, 2>> points_;
    std::optional<std::size_t> drag_;
};

// The colours of a gradient: each a colour, a place (0..100 %) and how strong it is; ＋ adds one; ready-made rows
// (空・夕焼け…) to start from.
class StopsEditor : public QWidget {
    Q_OBJECT
public:
    explicit StopsEditor(std::vector<Stop> stops = {}, bool with_opacity = true, QWidget* parent = nullptr);
    std::vector<Stop> stops() const;  // sorted by place
    void set_stops(std::vector<Stop> stops);
signals:
    void changed();
private:
    void fill();
    std::vector<Stop> stops_;
    bool with_opacity_;
    QWidget* bar_ = nullptr;
    QComboBox* presets_ = nullptr;
    QGridLayout* rows_ = nullptr;
};

// A gradient's colours as stops, from its stops or its two ends (gradient_editor.stops_from).
std::vector<Stop> stops_from(const core::Json& spec);

// The filter's or the correction's words (wording.FILTERS, wording.ADJUSTMENTS) and the blend modes (wording.BLEND).
const std::vector<std::pair<std::string, QString>>& filter_kinds();
const std::vector<std::pair<std::string, QString>>& adjustment_kinds();
const std::vector<std::pair<std::string, QString>>& blend_modes();

// The numbers of a filter or a correction (filter_params): none when the person stopped. `preview(params)` shows the
// result on the page while they change (null: take it away); `histogram` the layer's lightness (levels and curve).
using FilterPreview = std::function<void(const std::optional<core::Json>&)>;
std::optional<core::Json> filter_params(QWidget* parent, const std::string& kind, const core::Json& now = core::Json::object(),
                                        FilterPreview preview = {}, std::optional<std::vector<int>> histogram = std::nullopt);
// グラデーションマップ: the colours dark to light, each at its place.
std::optional<core::Json> gradient_map_dialog(QWidget* parent, const core::Json& now, const std::vector<std::array<int, 3>>& colours);
// A gradient layer's settings (which way, its shape, whether it repeats, its colours).
std::optional<core::Json> gradient_dialog(QWidget* parent, const core::Page* page, const core::Json& now);
// レイヤーのトーン化: the screen its greys print as.
std::optional<core::Json> screen_dialog(QWidget* parent, const core::Json& now);

}  // namespace genko::app
