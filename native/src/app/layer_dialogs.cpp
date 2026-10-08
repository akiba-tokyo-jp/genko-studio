#include "app/layer_dialogs.hpp"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <map>
#include <variant>

#include "app/ask.hpp"
#include "core/filters.hpp"
#include "render/plugins.hpp"

namespace genko::app {

using core::Json;

namespace {

// --- the words ---------------------------------------------------------------------------------------------------------

const std::vector<std::pair<std::string, QString>> kFilters{
    {"levels", QStringLiteral("レベル補正")}, {"curve", QStringLiteral("明るさの曲線（トーンカーブ）")},
    {"hue", QStringLiteral("色相・彩度・明度")}, {"blur", QStringLiteral("ぼかし")}, {"sharpen", QStringLiteral("シャープ")},
    {"mosaic", QStringLiteral("モザイク")}, {"motion_blur", QStringLiteral("移動ぼかし")}, {"radial_blur", QStringLiteral("放射ぼかし")},
    {"zoom_blur", QStringLiteral("ズームぼかし")}, {"noise", QStringLiteral("ノイズ")}, {"wave", QStringLiteral("波形")},
    {"twirl", QStringLiteral("渦巻き")}, {"lineart", QStringLiteral("線画抽出")}, {"invert", QStringLiteral("色調反転")},
    {"posterize", QStringLiteral("階調化（ポスタリゼーション）")}, {"threshold", QStringLiteral("2 値化（しきい値）")},
    {"bitonal", QStringLiteral("白黒にする")}, {"gradient_map", QStringLiteral("グラデーションマップ")},
    {"brightness_contrast", QStringLiteral("明るさ・コントラスト")}, {"despeckle", QStringLiteral("ゴミ取り")},
    {"glow", QStringLiteral("光彩拡散")}, {"rain", QStringLiteral("雨")}};
const std::vector<std::pair<std::string, QString>> kAdjustments{
    {"levels", QStringLiteral("レベル補正")}, {"curve", QStringLiteral("トーンカーブ")}, {"hue", QStringLiteral("色相・彩度・明度")},
    {"invert", QStringLiteral("色調反転")}, {"posterize", QStringLiteral("階調化")}, {"threshold", QStringLiteral("2 値化")},
    {"gradient_map", QStringLiteral("グラデーションマップ")}, {"brightness_contrast", QStringLiteral("明るさ・コントラスト")}};
const std::vector<std::pair<std::string, QString>> kBlend{
    {"normal", QStringLiteral("通常")}, {"multiply", QStringLiteral("乗算")}, {"screen", QStringLiteral("スクリーン")},
    {"add", QStringLiteral("加算（発光）")}, {"overlay", QStringLiteral("オーバーレイ")}, {"darken", QStringLiteral("比較（暗）")},
    {"lighten", QStringLiteral("比較（明）")}, {"color_burn", QStringLiteral("焼き込みカラー")}, {"linear_burn", QStringLiteral("焼き込み（リニア）")},
    {"color_dodge", QStringLiteral("覆い焼きカラー")}, {"soft_light", QStringLiteral("ソフトライト")}, {"hard_light", QStringLiteral("ハードライト")},
    {"difference", QStringLiteral("差の絶対値")}, {"exclusion", QStringLiteral("除外")}, {"subtract", QStringLiteral("減算")},
    {"divide", QStringLiteral("除算")}, {"hue", QStringLiteral("色相")}, {"saturation", QStringLiteral("彩度")},
    {"color", QStringLiteral("カラー")}, {"luminosity", QStringLiteral("輝度")}};

// --- the fields of each filter (FILTER_FIELDS) ---------------------------------------------------------------------------

struct Number { double lo, hi, value; };
struct Choice { std::vector<std::pair<QString, Json>> options; Json value; };
struct Field { const char* key; QString label; std::variant<Number, Choice> spec; };

const Choice kChannels{{{QStringLiteral("RGB（全部）"), "rgb"}, {QStringLiteral("赤"), "r"}, {QStringLiteral("緑"), "g"}, {QStringLiteral("青"), "b"}}, "rgb"};

const std::map<std::string, std::vector<Field>>& fields() {
    static const std::map<std::string, std::vector<Field>> all{
        {"blur", {{"radius", QStringLiteral("ぼかしの強さ"), Number{0.5, 30, 2.0}}}},
        {"levels", {{"channel", QStringLiteral("色"), kChannels}, {"black", QStringLiteral("黒くする所（0〜255）"), Number{0, 254, 20}},
                    {"white", QStringLiteral("白くする所（1〜255）"), Number{1, 255, 235}},
                    {"gamma", QStringLiteral("中間（1 より大きいと明るく）"), Number{0.1, 9.99, 1.0}},
                    {"out_black", QStringLiteral("出す暗さの下限"), Number{0, 255, 0}}, {"out_white", QStringLiteral("出す明るさの上限"), Number{0, 255, 255}}}},
        {"curve", {{"channel", QStringLiteral("色"), kChannels}}},
        {"brightness_contrast", {{"brightness", QStringLiteral("明るさ"), Number{-100, 100, 0}}, {"contrast", QStringLiteral("コントラスト"), Number{-100, 100, 0}}}},
        {"sharpen", {{"amount", QStringLiteral("強さ（1 で普通、2 で強め）"), Number{0.1, 5, 1.0}}}},
        {"lineart", {{"threshold", QStringLiteral("拾う強さ（大きいほど薄い線まで）"), Number{0.3, 0.98, 0.72}},
                     {"radius", QStringLiteral("線の太さの目安（px）"), Number{3, 41, 7}},
                     {"min_px", QStringLiteral("取るゴミの大きさ（px）"), Number{0, 400, 12}},
                     {"drop_blue", QStringLiteral("水色の下描き"), Choice{{{QStringLiteral("残す"), 0}, {QStringLiteral("消す"), 1}}, 1}},
                     {"keep_solid", QStringLiteral("ベタ"), Choice{{{QStringLiteral("残す"), 1}, {QStringLiteral("線だけにする"), 0}}, 1}}}},
        {"glow", {{"radius", QStringLiteral("広がり（px）"), Number{1, 120, 12}}, {"amount", QStringLiteral("強さ"), Number{0.1, 3, 0.8}},
                  {"threshold", QStringLiteral("光らせる明るさ（0〜255）"), Number{0, 255, 170}}}},
        {"rain", {{"count", QStringLiteral("本数"), Number{10, 5000, 400}}, {"length", QStringLiteral("長さ（px）"), Number{4, 400, 40}},
                  {"angle", QStringLiteral("傾き（°、右へ +）"), Number{-60, 60, 15}}, {"width", QStringLiteral("太さ（px）"), Number{1, 8, 1}},
                  {"opacity", QStringLiteral("濃さ"), Number{0.05, 1, 0.7}},
                  {"rgb", QStringLiteral("色"), Choice{{{QStringLiteral("白"), Json::array({255, 255, 255})}, {QStringLiteral("灰"), Json::array({150, 150, 150})},
                                                      {QStringLiteral("黒"), Json::array({20, 20, 20})}}, Json::array({255, 255, 255})}}}},
        {"despeckle", {{"size_mm", QStringLiteral("取るゴミの大きさ（mm）"), Number{0.05, 5, 0.3}},
                       {"what", QStringLiteral("取るもの"), Choice{{{QStringLiteral("黒い点（ゴミ）"), "ink"}, {QStringLiteral("線の中の白い穴"), "holes"},
                                                                {QStringLiteral("両方"), "both"}}, "ink"}}}},
        {"hue", {{"shift", QStringLiteral("色相（°）"), Number{-180, 180, 30}}, {"saturation", QStringLiteral("彩度（倍）"), Number{0, 3, 1.0}},
                 {"value", QStringLiteral("明度（倍）"), Number{0, 3, 1.0}}}},
        {"mosaic", {{"block", QStringLiteral("モザイクの大きさ（px）"), Number{2, 64, 8}}}},
        {"motion_blur", {{"distance", QStringLiteral("流す長さ（px）"), Number{1, 300, 12}}, {"angle", QStringLiteral("向き（°）"), Number{-180, 180, 0}}}},
        {"radial_blur", {{"amount", QStringLiteral("強さ"), Number{0.01, 0.5, 0.08}}, {"cx", QStringLiteral("中心（横 0〜1）"), Number{0, 1, 0.5}},
                         {"cy", QStringLiteral("中心（縦 0〜1）"), Number{0, 1, 0.5}}}},
        {"zoom_blur", {{"amount", QStringLiteral("強さ"), Number{0.01, 0.5, 0.08}}, {"cx", QStringLiteral("中心（横 0〜1）"), Number{0, 1, 0.5}},
                       {"cy", QStringLiteral("中心（縦 0〜1）"), Number{0, 1, 0.5}}}},
        {"noise", {{"amount", QStringLiteral("量（0〜1）"), Number{0.01, 1, 0.15}}}},
        {"wave", {{"amplitude", QStringLiteral("揺れ幅（px）"), Number{0, 200, 6}}, {"wavelength", QStringLiteral("波の長さ（px）"), Number{2, 1000, 60}}}},
        {"twirl", {{"angle", QStringLiteral("回す角度（°）"), Number{-720, 720, 90}}, {"radius", QStringLiteral("広さ（0〜1）"), Number{0.05, 1, 0.45}}}},
        {"posterize", {{"levels", QStringLiteral("段階の数"), Number{2, 64, 4}}}},
        {"threshold", {{"threshold", QStringLiteral("しきい値（0〜255）"), Number{0, 255, 128}}}},
        {"bitonal", {{"threshold", QStringLiteral("しきい値（0〜255）"), Number{0, 255, 180}}}},
    };
    return all;
}

// what the numbers mean in practice (FIELD_TIPS)
QString tip(const std::string& kind, const std::string& key) {
    if (kind == "despeckle" && key == "size_mm")
        return QStringLiteral("これより小さい点を取ります。スキャンの細かいゴミは 0.3〜0.5mm、スマホで撮った紙のざらつきは 1〜2mm。"
                              "大きくしすぎると句読点や細かい描き込みも消えます");
    if (kind == "lineart" && key == "threshold")
        return QStringLiteral("0.6 前後: 濃い線だけ。0.72（標準）: ふつうの鉛筆・ペン。0.85 以上: 薄い線や紙のムラまで拾う");
    if (kind == "lineart" && key == "radius") return QStringLiteral("線の太さのおよそ（px）。細いペンは 5〜7、太い筆は 11〜21。紙のムラを拾うときは大きく");
    if (kind == "lineart" && key == "min_px") return QStringLiteral("これより小さい黒い点を捨てます（px）。スキャンなら 8〜20、撮った紙なら 30〜80");
    return {};
}

QPixmap histogram_pixmap(const std::vector<int>& counts, int width = 202, int height = 56) {
    QPixmap pixmap(width, height);
    pixmap.fill(QColor(250, 250, 250));
    std::vector<int> sorted = counts;
    std::sort(sorted.begin(), sorted.end());
    // (one huge bar, often paper, does not flatten the rest)
    const int top = std::max(1, sorted.size() > 1 ? sorted[sorted.size() - 2] : (sorted.empty() ? 1 : sorted.back()));
    QPainter p(&pixmap);
    p.setPen(QPen(QColor(90, 90, 90), 1));
    for (std::size_t i = 0; i < counts.size() && i < 256; ++i) {
        const double x = 1 + static_cast<double>(i) * (width - 2) / 255;
        const int h = std::min(height - 2, static_cast<int>(std::lround((height - 2) * static_cast<double>(counts[i]) / top)));
        if (h > 0) p.drawLine(QPointF(x, height - 1), QPointF(x, height - 1 - h));
    }
    return pixmap;
}

const std::vector<std::pair<QString, std::vector<Stop>>>& presets() {
    static const std::vector<std::pair<QString, std::vector<Stop>>> all{
        {QStringLiteral("空"), {{0.0, {70, 130, 210}, 1}, {0.6, {150, 200, 240}, 1}, {1.0, {235, 245, 255}, 1}}},
        {QStringLiteral("夕焼け"), {{0.0, {40, 50, 110}, 1}, {0.45, {210, 90, 90}, 1}, {0.75, {250, 170, 80}, 1}, {1.0, {255, 230, 170}, 1}}},
        {QStringLiteral("夜明け"), {{0.0, {20, 30, 70}, 1}, {0.5, {120, 110, 170}, 1}, {1.0, {250, 200, 180}, 1}}},
        {QStringLiteral("虹"), {{0.0, {230, 60, 60}, 1}, {0.2, {245, 160, 50}, 1}, {0.4, {240, 230, 70}, 1}, {0.6, {80, 190, 90}, 1},
                               {0.8, {70, 120, 220}, 1}, {1.0, {140, 80, 200}, 1}}},
        {QStringLiteral("セピア"), {{0.0, {40, 25, 15}, 1}, {0.5, {150, 110, 70}, 1}, {1.0, {245, 230, 205}, 1}}},
        {QStringLiteral("黒から透明"), {{0.0, {20, 20, 20}, 1}, {1.0, {20, 20, 20}, 0}}},
    };
    return all;
}

class Bar : public QWidget {
public:
    explicit Bar(StopsEditor* editor) : QWidget(editor), editor_(editor) { setMinimumHeight(22); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        for (int y = 0; y < height(); y += 6)  // (see-through shows as squares)
            for (int x = 0; x < width(); x += 6) p.fillRect(x, y, 6, 6, (x / 6 + y / 6) % 2 ? QColor(210, 210, 210) : QColor(245, 245, 245));
        QLinearGradient grad(0, 0, width(), 0);
        for (const Stop& s : editor_->stops()) grad.setColorAt(s.at, QColor(s.rgb[0], s.rgb[1], s.rgb[2], static_cast<int>(std::lround(255 * s.opacity))));
        p.fillRect(rect(), grad);
    }
private:
    StopsEditor* editor_;
};

QDialogButtonBox* ok_cancel(QDialog* dialog, const QString& ok = {}, const QString& cancel = {}) {
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    if (!ok.isEmpty()) buttons->button(QDialogButtonBox::Ok)->setText(ok);
    if (!cancel.isEmpty()) buttons->button(QDialogButtonBox::Cancel)->setText(cancel);
    QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    return buttons;
}

double rounded(double v, int places) {
    const double k = std::pow(10.0, places);
    return std::round(v * k) / k;
}

}  // namespace

const std::vector<std::pair<std::string, QString>>& filter_kinds() { return kFilters; }
const std::vector<std::pair<std::string, QString>>& adjustment_kinds() { return kAdjustments; }
const std::vector<std::pair<std::string, QString>>& blend_modes() { return kBlend; }

// --- CurveEditor ---------------------------------------------------------------------------------------------------------

CurveEditor::CurveEditor(std::vector<std::array<double, 2>> points, QWidget* parent) : QWidget(parent), points_(std::move(points)) {
    setFixedSize(kSide + 2, kSide + 2);
    setToolTip(QStringLiteral("クリックで点を足す・ドラッグで動かす・右クリックで消す"));
    setObjectName(QStringLiteral("curve_editor"));
}

QPointF CurveEditor::to_view(double x, double y) const { return QPointF(1 + x / 255 * kSide, 1 + (255 - y) / 255 * kSide); }

std::array<double, 2> CurveEditor::to_value(QPointF pos) const {
    const double x = std::clamp((pos.x() - 1) / kSide * 255, 0.0, 255.0);
    const double y = std::clamp(255 - (pos.y() - 1) / kSide * 255, 0.0, 255.0);
    return {std::round(x), std::round(y)};
}

std::optional<std::size_t> CurveEditor::near(QPointF pos) const {
    for (std::size_t i = 0; i < points_.size(); ++i) {
        const QPointF p = to_view(points_[i][0], points_[i][1]);
        if (std::abs(p.x() - pos.x()) <= 6 && std::abs(p.y() - pos.y()) <= 6) return i;
    }
    return std::nullopt;
}

void CurveEditor::mousePressEvent(QMouseEvent* event) {
    const QPointF pos = event->position();
    auto index = near(pos);
    if (event->button() == Qt::RightButton) {
        if (index && *index > 0 && *index + 1 < points_.size()) {
            points_.erase(points_.begin() + static_cast<std::ptrdiff_t>(*index));
            update();
            emit changed();
        }
        return;
    }
    if (!index) {
        const auto v = to_value(pos);
        if (std::any_of(points_.begin(), points_.end(), [&](const auto& p) { return std::abs(p[0] - v[0]) < 3; })) return;
        points_.push_back(v);
        std::sort(points_.begin(), points_.end());
        index = static_cast<std::size_t>(std::find(points_.begin(), points_.end(), v) - points_.begin());
        emit changed();
    }
    drag_ = index;
    update();
}

void CurveEditor::mouseMoveEvent(QMouseEvent* event) {
    if (!drag_) return;
    const auto v = to_value(event->position());
    const std::size_t i = *drag_;
    const double lo = i > 0 ? points_[i - 1][0] + 1 : 0;
    double hi = i + 1 < points_.size() ? points_[i + 1][0] - 1 : 255;
    if (i == 0) hi = std::min(hi, 254.0);
    points_[i] = {std::min(hi, std::max(lo, v[0])), v[1]};
    update();
    emit changed();
}

void CurveEditor::mouseReleaseEvent(QMouseEvent*) { drag_.reset(); }

void CurveEditor::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor(250, 250, 250));
    p.setPen(QPen(QColor(215, 215, 215), 1));
    for (int k = 1; k < 4; ++k) {
        const double v = 1 + k * kSide / 4.0;
        p.drawLine(QPointF(v, 1), QPointF(v, kSide + 1));
        p.drawLine(QPointF(1, v), QPointF(kSide + 1, v));
    }
    p.setPen(QPen(QColor(160, 160, 160), 1));
    p.drawRect(0, 0, kSide + 1, kSide + 1);
    Json pts = Json::array();
    for (const auto& pt : points_) pts.push_back(Json::array({pt[0], pt[1]}));
    std::vector<int> table;
    try {
        table = core::adjustment("curve", Json{{"points", pts}}).tables[0];
    } catch (const std::exception&) {
        return;
    }
    QPainterPath path(to_view(0, table[0]));
    for (int x = 1; x < 256; ++x) path.lineTo(to_view(x, table[static_cast<std::size_t>(x)]));
    p.setPen(QPen(QColor(30, 30, 30), 1.5));
    p.drawPath(path);
    p.setBrush(QColor(255, 255, 255));
    for (const auto& pt : points_) p.drawEllipse(to_view(pt[0], pt[1]), 4, 4);
}

// --- StopsEditor -----------------------------------------------------------------------------------------------------------

StopsEditor::StopsEditor(std::vector<Stop> stops, bool with_opacity, QWidget* parent)
    : QWidget(parent), stops_(stops.empty() ? presets().back().second : std::move(stops)), with_opacity_(with_opacity) {
    setObjectName(QStringLiteral("stops_editor"));
    bar_ = new Bar(this);
    presets_ = new QComboBox;
    presets_->addItem(QStringLiteral("見本から選ぶ…"), QString());
    for (const auto& [name, _] : presets()) presets_->addItem(name, name);
    connect(presets_, &QComboBox::activated, this, [this](int) {
        const QString name = presets_->currentData().toString();
        for (const auto& [preset, rows] : presets())
            if (preset == name) set_stops(rows);
        presets_->setCurrentIndex(0);
    });
    rows_ = new QGridLayout;
    auto* add = new QPushButton(QStringLiteral("＋ 色を足す"));
    add->setObjectName(QStringLiteral("stops_add"));
    connect(add, &QPushButton::clicked, this, [this] {
        auto now = this->stops();
        const Stop a = now[now.size() - 2], b = now.back();
        Stop mid{(a.at + b.at) / 2, {}, (a.opacity + b.opacity) / 2};
        for (unsigned c = 0; c < 3; ++c) mid.rgb[c] = static_cast<int>(std::lround((a.rgb[c] + b.rgb[c]) / 2.0));
        now.insert(now.end() - 1, mid);
        set_stops(now);
    });
    auto* top = new QHBoxLayout;
    top->addWidget(presets_, 1);
    top->addWidget(add);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(bar_);
    layout->addLayout(top);
    layout->addLayout(rows_);
    fill();
}

std::vector<Stop> StopsEditor::stops() const {
    std::vector<Stop> out = stops_;
    std::stable_sort(out.begin(), out.end(), [](const Stop& a, const Stop& b) { return a.at < b.at; });
    return out;
}

void StopsEditor::set_stops(std::vector<Stop> stops) {
    stops_ = std::move(stops);
    fill();
    emit changed();
}

void StopsEditor::fill() {
    while (QLayoutItem* item = rows_->takeAt(0)) {
        if (QWidget* w = item->widget()) w->deleteLater();
        delete item;
    }
    stops_ = stops();
    for (std::size_t i = 0; i < stops_.size(); ++i) {
        const int row = static_cast<int>(i);
        const Stop& s = stops_[i];
        auto* colour = new QPushButton;
        colour->setFixedWidth(46);
        colour->setStyleSheet(QStringLiteral("background: rgb(%1,%2,%3)").arg(s.rgb[0]).arg(s.rgb[1]).arg(s.rgb[2]));
        colour->setToolTip(QStringLiteral("色を選ぶ"));
        connect(colour, &QPushButton::clicked, this, [this, i] {
            const auto& rgb = stops_[i].rgb;
            const auto picked = ask::colour(this, QColor(rgb[0], rgb[1], rgb[2]), QStringLiteral("色"));
            if (!picked) return;
            stops_[i].rgb = {picked->red(), picked->green(), picked->blue()};
            fill();
            emit changed();
        });
        auto* where = new QDoubleSpinBox;
        where->setRange(0, 100);
        where->setSuffix(QStringLiteral(" %"));
        where->setValue(rounded(s.at * 100, 1));
        where->setToolTip(QStringLiteral("はじめ（0 %）から終わり（100 %）のどこに置くか"));
        connect(where, &QDoubleSpinBox::valueChanged, this, [this, i](double v) { stops_[i].at = v / 100; bar_->update(); emit changed(); });
        rows_->addWidget(colour, row, 0);
        rows_->addWidget(where, row, 1);
        if (with_opacity_) {
            auto* strong = new QDoubleSpinBox;
            strong->setRange(0, 100);
            strong->setSuffix(QStringLiteral(" %"));
            strong->setValue(std::round(s.opacity * 100));
            strong->setToolTip(QStringLiteral("濃さ（0 % で透明）"));
            connect(strong, &QDoubleSpinBox::valueChanged, this, [this, i](double v) { stops_[i].opacity = v / 100; bar_->update(); emit changed(); });
            rows_->addWidget(strong, row, 2);
        }
        auto* remove = new QPushButton(QStringLiteral("×"));
        remove->setFixedWidth(28);
        remove->setEnabled(stops_.size() > 2);
        connect(remove, &QPushButton::clicked, this, [this, i] {
            if (stops_.size() <= 2) return;
            stops_.erase(stops_.begin() + static_cast<std::ptrdiff_t>(i));
            fill();
            emit changed();
        });
        rows_->addWidget(remove, row, 3);
    }
    bar_->update();
}

std::vector<Stop> stops_from(const Json& spec) {
    std::vector<Stop> out;
    const auto colour = [](const Json& v, std::array<int, 3> fallback) {
        if (!v.is_array() || v.size() < 3) return fallback;
        return std::array<int, 3>{v[0].get<int>(), v[1].get<int>(), v[2].get<int>()};
    };
    if (spec.is_object() && spec.contains("stops") && spec["stops"].is_array() && !spec["stops"].empty()) {
        for (const Json& s : spec["stops"]) out.push_back(Stop{s[0].get<double>(), colour(s[1], {0, 0, 0}), s.size() > 2 ? s[2].get<double>() : 1.0});
        return out;
    }
    const Json from = spec.is_object() ? spec.value("rgb_from", Json()) : Json(), to = spec.is_object() ? spec.value("rgb_to", Json()) : Json();
    out.push_back(Stop{0.0, colour(from, {20, 20, 20}), spec.is_object() ? spec.value("opacity_from", 1.0) : 1.0});
    out.push_back(Stop{1.0, colour(to, {255, 255, 255}), spec.is_object() ? spec.value("opacity_to", 1.0) : 1.0});
    return out;
}

// --- the dialogs -------------------------------------------------------------------------------------------------------------

std::optional<Json> filter_params(QWidget* parent, const std::string& kind, const Json& now_in, FilterPreview preview,
                                  std::optional<std::vector<int>> histogram) {
    const Json now = now_in.is_object() ? now_in : Json::object();
    const auto found = fields().find(kind);
    const std::vector<Field>* asked = found != fields().end() ? &found->second : nullptr;
    std::vector<std::string> plugin_keys;  // (a plugin's own settings: plugins.fields, their names kept while it asks)
    std::vector<Field> plugin_fields;
    if (asked == nullptr && kind.starts_with(render::plugins::kPrefix)) {
        const auto given = render::plugins::fields(kind);
        plugin_keys.reserve(given.size());
        for (const auto& [key, label, lo, hi, value] : given) plugin_keys.push_back(key);
        for (std::size_t i = 0; i < given.size(); ++i) {
            const auto& [key, label, lo, hi, value] = given[i];
            plugin_fields.push_back(Field{plugin_keys[i].c_str(), QString::fromStdString(label), Number{lo, hi, value}});
        }
        asked = &plugin_fields;
    }
    if ((asked == nullptr || asked->empty()) && kind != "curve") return Json::object();
    QString title = QStringLiteral("フィルターの強さ");
    for (const auto& [key, label] : kAdjustments) if (key == kind) title = label;
    for (const auto& [key, label] : kFilters) if (key == kind) title = label;
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("filter_dialog"));
    dialog.setWindowTitle(title);
    auto* form = new QFormLayout(&dialog);
    if (histogram && (kind == "levels" || kind == "curve")) {
        auto* chart = new QLabel;
        chart->setPixmap(histogram_pixmap(*histogram));
        chart->setToolTip(QStringLiteral("このレイヤーの明るさの分布（左が黒、右が白）"));
        form->addRow(QStringLiteral("分布"), chart);
    }
    std::vector<std::pair<std::string, std::function<Json()>>> getters;
    auto* timer = new QTimer(&dialog);
    timer->setSingleShot(true);
    timer->setInterval(250);
    const auto restart = [timer] { timer->start(); };
    if (asked != nullptr) {
        for (const Field& field : *asked) {
            QWidget* box = nullptr;
            if (const auto* choice = std::get_if<Choice>(&field.spec)) {
                auto* combo = new QComboBox;
                combo->setObjectName(QStringLiteral("field_%1").arg(QString::fromLatin1(field.key)));
                int at = 0;
                const Json wanted = now.contains(field.key) ? now[field.key] : choice->value;
                for (std::size_t i = 0; i < choice->options.size(); ++i) {
                    combo->addItem(choice->options[i].first);
                    if (choice->options[i].second == wanted) at = static_cast<int>(i);
                }
                combo->setCurrentIndex(at);
                getters.emplace_back(field.key, [combo, options = choice->options] { return options[static_cast<std::size_t>(std::max(0, combo->currentIndex()))].second; });
                QObject::connect(combo, &QComboBox::currentIndexChanged, &dialog, restart);
                box = combo;
            } else {
                const Number number = std::get<Number>(field.spec);
                auto* spin = new QDoubleSpinBox;
                spin->setObjectName(QStringLiteral("field_%1").arg(QString::fromLatin1(field.key)));
                spin->setRange(number.lo, number.hi);
                spin->setDecimals(number.hi <= 1 ? 3 : number.hi <= 10 ? 2 : number.hi <= 300 ? 1 : 0);
                spin->setSingleStep(number.hi <= 1 ? 0.01 : number.hi <= 10 ? 0.1 : 1);
                const Json* given = now.contains(field.key) ? &now[field.key] : nullptr;
                spin->setValue(given != nullptr && given->is_number() ? given->get<double>() : number.value);
                getters.emplace_back(field.key, [spin] { return Json(rounded(spin->value(), 3)); });
                QObject::connect(spin, &QDoubleSpinBox::valueChanged, &dialog, restart);
                box = spin;
            }
            const QString hint = tip(kind, field.key);
            form->addRow(field.label, box);
            if (!hint.isEmpty()) {
                box->setToolTip(hint);
                auto* note = new QLabel(hint);
                note->setWordWrap(true);
                note->setProperty("role", "hint");
                form->addRow(QString(), note);
            }
        }
    }
    CurveEditor* curve = nullptr;
    if (kind == "curve") {
        std::vector<std::array<double, 2>> points;
        if (now.contains("points") && now["points"].is_array() && !now["points"].empty())
            for (const Json& p : now["points"]) points.push_back({p[0].get<double>(), p[1].get<double>()});
        else points = {{0, 0}, {255, 255}};
        curve = new CurveEditor(points);
        form->addRow(QStringLiteral("曲線"), curve);
        form->addRow(QString(), new QLabel(QStringLiteral("横が元の明るさ、縦がかけた後。上に持ち上げると明るく")));
        QObject::connect(curve, &CurveEditor::changed, &dialog, restart);
    }
    const auto values = [&]() {
        Json out = Json::object();
        for (const auto& [key, get] : getters) out[key] = get();
        if (curve != nullptr) {
            Json points = Json::array();
            for (const auto& p : curve->points()) points.push_back(Json::array({static_cast<int>(p[0]), static_cast<int>(p[1])}));
            out["points"] = points;
            out.erase("gamma");
        }
        return out;
    };
    QCheckBox* check = nullptr;
    if (preview) {
        check = new QCheckBox(QStringLiteral("プレビュー（ページで見る）"));
        check->setObjectName(QStringLiteral("filter_preview"));
        check->setChecked(true);
        form->addRow(QString(), check);
        QObject::connect(timer, &QTimer::timeout, &dialog, [&] {
            try {
                preview(check->isChecked() ? std::optional<Json>(values()) : std::nullopt);
            } catch (const std::exception&) {
                preview(std::nullopt);  // (a preview that cannot be made must not stop the dialog)
            }
        });
        QObject::connect(check, &QCheckBox::toggled, &dialog, restart);
        timer->start(0);
    }
    form->addRow(ok_cancel(&dialog, QStringLiteral("かける"), QStringLiteral("やめる")));
    const bool ok = ask::exec(&dialog) == QDialog::Accepted;
    timer->stop();
    const Json result = values();
    if (preview) preview(std::nullopt);
    if (!ok) return std::nullopt;
    return result;
}

std::optional<Json> gradient_map_dialog(QWidget* parent, const Json& now, const std::vector<std::array<int, 3>>& defaults) {
    std::vector<Stop> stops;
    if (now.is_object() && now.contains("stops") && now["stops"].is_array() && !now["stops"].empty()) {
        for (const Json& s : now["stops"]) stops.push_back(Stop{s[0].get<double>(), {s[1][0].get<int>(), s[1][1].get<int>(), s[1][2].get<int>()}, 1.0});
    } else {
        std::vector<std::array<int, 3>> colours = defaults;
        if (now.is_object() && now.contains("colors") && now["colors"].is_array() && now["colors"].size() >= 2) {
            colours.clear();
            for (const Json& c : now["colors"]) colours.push_back({c[0].get<int>(), c[1].get<int>(), c[2].get<int>()});
        }
        for (std::size_t i = 0; i < colours.size(); ++i)
            stops.push_back(Stop{static_cast<double>(i) / static_cast<double>(std::max<std::size_t>(1, colours.size() - 1)), colours[i], 1.0});
    }
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("gradient_map_dialog"));
    dialog.setWindowTitle(QStringLiteral("グラデーションマップ（暗い所 → 明るい所）"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* editor = new StopsEditor(stops, false);
    layout->addWidget(editor);
    layout->addWidget(ok_cancel(&dialog));
    if (ask::exec(&dialog) != QDialog::Accepted) return std::nullopt;
    Json out = Json::array();
    for (const Stop& s : editor->stops()) out.push_back(Json::array({rounded(s.at, 4), Json::array({s.rgb[0], s.rgb[1], s.rgb[2]})}));
    return Json{{"stops", out}};
}

std::optional<Json> gradient_dialog(QWidget* parent, const core::Page* page, const Json& now) {
    const double w = page != nullptr ? page->spec.width_mm.value() : 210, h = page != nullptr ? page->spec.height_mm.value() : 297;
    struct Way { QString label; std::array<double, 2> from, to; const char* shape; };
    const std::vector<Way> ways{{QStringLiteral("上から下へ"), {w / 2, 0}, {w / 2, h}, "linear"}, {QStringLiteral("下から上へ"), {w / 2, h}, {w / 2, 0}, "linear"},
                                {QStringLiteral("左から右へ"), {0, h / 2}, {w, h / 2}, "linear"}, {QStringLiteral("右から左へ"), {w, h / 2}, {0, h / 2}, "linear"},
                                {QStringLiteral("中心から外へ（円）"), {w / 2, h / 2}, {w / 2, 0}, "radial"},
                                {QStringLiteral("中心から外へ（楕円）"), {w / 2, h / 2}, {w / 2, 0}, "ellipse"}};
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("gradient_dialog"));
    dialog.setWindowTitle(QStringLiteral("グラデーション"));
    auto* form = new QFormLayout(&dialog);
    auto* way = new QComboBox;
    way->setObjectName(QStringLiteral("gradient_way"));
    for (const Way& item : ways) way->addItem(item.label);
    const std::string shape_now = now.is_object() ? now.value("shape", std::string("linear")) : std::string("linear");
    for (std::size_t i = 0; i < ways.size(); ++i)
        if (shape_now != "linear" && ways[i].shape == shape_now) { way->setCurrentIndex(static_cast<int>(i)); break; }
    auto* ratio = new QDoubleSpinBox;
    ratio->setRange(0.1, 10);
    ratio->setSingleStep(0.1);
    ratio->setValue(now.is_object() ? now.value("ratio", 0.5) : 0.5);
    ratio->setToolTip(QStringLiteral("楕円の横の幅（縦を 1 として）"));
    auto* repeat = new QComboBox;
    const std::vector<std::pair<QString, std::string>> repeats{{QStringLiteral("しない"), "none"}, {QStringLiteral("繰り返す"), "repeat"}, {QStringLiteral("折り返す"), "mirror"}};
    for (const auto& [label, key] : repeats) repeat->addItem(label, QString::fromStdString(key));
    repeat->setCurrentIndex(std::max(0, repeat->findData(QString::fromStdString(now.is_object() ? now.value("repeat", std::string("none")) : std::string("none")))));
    auto* colours = new StopsEditor(stops_from(now));
    form->addRow(QStringLiteral("向き"), way);
    form->addRow(QStringLiteral("楕円の横幅"), ratio);
    form->addRow(QStringLiteral("端から先"), repeat);
    form->addRow(QStringLiteral("色"), colours);
    form->addRow(ok_cancel(&dialog));
    if (ask::exec(&dialog) != QDialog::Accepted) return std::nullopt;
    const Way& chosen = ways[static_cast<std::size_t>(std::max(0, way->currentIndex()))];
    const auto stops = colours->stops();
    const auto rgb = [](const Stop& s) { return Json::array({s.rgb[0], s.rgb[1], s.rgb[2]}); };
    Json out{{"from", Json::array({rounded(chosen.from[0], 2), rounded(chosen.from[1], 2)})},
             {"to", Json::array({rounded(chosen.to[0], 2), rounded(chosen.to[1], 2)})}, {"shape", chosen.shape},
             {"rgb_from", rgb(stops.front())}, {"rgb_to", rgb(stops.back())},
             {"opacity_from", stops.front().opacity}, {"opacity_to", stops.back().opacity}};
    if (stops.size() > 2 || stops.front().at > 0 || stops.back().at < 1) {
        Json all = Json::array();
        for (const Stop& s : stops) all.push_back(Json::array({s.at, rgb(s), s.opacity}));
        out["stops"] = all;
    }
    if (std::string(chosen.shape) == "ellipse") out["ratio"] = rounded(ratio->value(), 2);
    if (repeat->currentData().toString() != QStringLiteral("none")) out["repeat"] = repeat->currentData().toString().toStdString();
    return out;
}

std::optional<Json> screen_dialog(QWidget* parent, const Json& now_in) {
    const Json now = now_in.is_object() ? now_in : Json::object();
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("screen_dialog"));
    dialog.setWindowTitle(QStringLiteral("トーン化"));
    auto* form = new QFormLayout(&dialog);
    auto* pattern = new QComboBox;
    for (const auto& [label, key] : std::vector<std::pair<QString, QString>>{{QStringLiteral("網点"), "dot"}, {QStringLiteral("線"), "line"},
                                                                              {QStringLiteral("交差した線"), "cross"}, {QStringLiteral("砂目"), "noise"}})
        pattern->addItem(label, key);
    pattern->setCurrentIndex(std::max(0, pattern->findData(QString::fromStdString(now.value("pattern", std::string("dot"))))));
    auto* shape = new QComboBox;
    for (const auto& [label, key] : std::vector<std::pair<QString, QString>>{{QStringLiteral("丸"), "round"}, {QStringLiteral("四角"), "square"},
                                                                              {QStringLiteral("ひし形"), "diamond"}, {QStringLiteral("楕円"), "ellipse"}})
        shape->addItem(label, key);
    shape->setCurrentIndex(std::max(0, shape->findData(QString::fromStdString(now.value("shape", std::string("round"))))));
    QObject::connect(pattern, &QComboBox::currentIndexChanged, shape, [pattern, shape](int) { shape->setEnabled(pattern->currentData().toString() == QStringLiteral("dot")); });
    shape->setEnabled(pattern->currentData().toString() == QStringLiteral("dot"));
    auto* lpi = new QDoubleSpinBox;
    lpi->setRange(10, 150);
    lpi->setDecimals(0);
    lpi->setValue(now.value("lpi", 60.0));
    lpi->setSuffix(QStringLiteral(" 線"));
    lpi->setToolTip(QStringLiteral("多いほど細かい網点。60 前後が普通"));
    auto* angle = new QDoubleSpinBox;
    angle->setRange(0, 179);
    angle->setDecimals(0);
    angle->setValue(now.value("angle", 45.0));
    angle->setSuffix(QStringLiteral("°"));
    const Json offset = now.contains("offset_mm") && now["offset_mm"].is_array() ? now["offset_mm"] : Json::array({0, 0});
    auto* off_x = new QDoubleSpinBox;
    auto* off_y = new QDoubleSpinBox;
    for (auto [spin, value] : {std::pair{off_x, offset[0].get<double>()}, std::pair{off_y, offset[1].get<double>()}}) {
        spin->setRange(-20, 20);
        spin->setSingleStep(0.1);
        spin->setDecimals(2);
        spin->setSuffix(QStringLiteral(" mm"));
        spin->setValue(value);
    }
    form->addRow(QStringLiteral("網の種類"), pattern);
    form->addRow(QStringLiteral("網の形"), shape);
    form->addRow(QStringLiteral("線数"), lpi);
    form->addRow(QStringLiteral("角度"), angle);
    form->addRow(QStringLiteral("網のずれ（右）"), off_x);
    form->addRow(QStringLiteral("網のずれ（下）"), off_y);
    form->addRow(ok_cancel(&dialog));
    if (ask::exec(&dialog) != QDialog::Accepted) return std::nullopt;
    Json spec = now;
    spec["pattern"] = pattern->currentData().toString().toStdString();
    spec["shape"] = shape->currentData().toString().toStdString();
    spec["lpi"] = lpi->value();
    spec["angle"] = angle->value();
    if (off_x->value() != 0 || off_y->value() != 0) spec["offset_mm"] = Json::array({off_x->value(), off_y->value()});
    else spec.erase("offset_mm");
    return spec;
}

}  // namespace genko::app
