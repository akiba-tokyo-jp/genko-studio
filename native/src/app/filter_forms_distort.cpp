// PC4 節 D2 (SPEC.md COLOR-08「フィルター」, design 5.2): the windows of フィルター → 変形・ピクセレート・表現手法
// (app/filter_forms.hpp) — each kind's fields in Photoshop's words, シアー…'s curve (its points dragged on a line from the
// top to the bottom of the base box) and 置き換え…'s map (a layer of the page, its merged picture, or a PNG or PSD file —
// as Photoshop asks for its 置き換えマップ). ぶれ, ファセット, 輪郭検出 and ソラリゼーション have no window.

#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "app/ask.hpp"
#include "app/filter_forms.hpp"
#include "core/filter_kinds.hpp"
#include "core/pyops.hpp"
#include "service/wording.hpp"

namespace genko::app::filter_forms {

using core::Json;

namespace {

// (lengths are the working 200 dpi's pixels: design 2.2, 9 章)
const QString kLengths = QStringLiteral("長さは 200 dpi の画素で数えます（600 dpi のページでは 3 倍の画素に掛かり、紙の上の大きさは同じです）。");
// (the distortions' base box)
const QString kBaseBox = QStringLiteral("選択範囲（無ければレイヤー）の範囲を中心にかけます。");

std::vector<std::pair<QString, Json>> undefined_areas(bool wrap_first) {
    std::vector<std::pair<QString, Json>> out{{QStringLiteral("折り返す"), Json("wrap")}, {QStringLiteral("端のピクセルを繰り返して埋める"), Json("repeat")}};
    if (!wrap_first) std::swap(out[0], out[1]);
    return out;
}

// --- シアー…'s curve ---------------------------------------------------------------------------------------------------

// The curve's value at t (Catmull–Rom through the points, as the kernel draws it: render/fx_distort.cpp).
double curve_at(const std::vector<std::array<double, 2>>& pts, double t) {
    if (pts.empty()) return 0;
    if (pts.size() == 1 || t <= pts.front()[0]) return pts.front()[1];
    if (t >= pts.back()[0]) return pts.back()[1];
    std::size_t i = 0;
    while (i + 2 < pts.size() && t > pts[i + 1][0]) ++i;
    const auto& p1 = pts[i];
    const auto& p2 = pts[i + 1];
    const auto& p0 = i > 0 ? pts[i - 1] : p1;
    const auto& p3 = i + 2 < pts.size() ? pts[i + 2] : p2;
    const double span = std::max(p2[0] - p1[0], 1e-6);
    const double s = (t - p1[0]) / span;
    const double m1 = i > 0 ? (p2[1] - p0[1]) / std::max(p2[0] - p0[0], 1e-6) * span : p2[1] - p1[1];
    const double m2 = i + 2 < pts.size() ? (p3[1] - p1[1]) / std::max(p3[0] - p1[0], 1e-6) * span : p2[1] - p1[1];
    const double s2 = s * s, s3 = s2 * s;
    return (2 * s3 - 3 * s2 + 1) * p1[1] + (s3 - 2 * s2 + s) * m1 + (-2 * s3 + 3 * s2) * p2[1] + (s3 - s2) * m2;
}

// A line from the top to the bottom of the base box, its points dragged across (a click adds one, a double click takes
// one away; the top and the bottom stay at their places, moved across only).
class ShearCurve : public QWidget {
public:
    explicit ShearCurve(std::function<void()> changed) : changed_(std::move(changed)) {
        setObjectName(QStringLiteral("shear_curve"));
        setFixedSize(160, 200);
        setCursor(Qt::CrossCursor);
    }
    const std::vector<std::array<double, 2>>& points() const { return points_; }
    void set_points(std::vector<std::array<double, 2>> points) {
        std::stable_sort(points.begin(), points.end(), [](const auto& a, const auto& b) { return a[0] < b[0]; });
        if (points.size() < 2) points = {{0, 0}, {1, 0}};
        points_ = std::move(points);
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), palette().base());
        p.setPen(QPen(palette().mid().color(), 1));
        for (int k = 1; k < 4; ++k) {
            p.drawLine(QPointF(width() * k / 4.0, 0), QPointF(width() * k / 4.0, height()));
            p.drawLine(QPointF(0, height() * k / 4.0), QPointF(width(), height() * k / 4.0));
        }
        QPainterPath path;
        for (int y = 0; y <= height(); ++y) {
            const QPointF at = view(static_cast<double>(y) / std::max(1, height()), curve_at(points_, static_cast<double>(y) / std::max(1, height())));
            if (y == 0) path.moveTo(at);
            else path.lineTo(at);
        }
        p.setPen(QPen(palette().text().color(), 1.5));
        p.drawPath(path);
        p.setBrush(palette().highlight());
        for (const auto& pt : points_) p.drawEllipse(view(pt[0], pt[1]), 3.5, 3.5);
    }
    void mousePressEvent(QMouseEvent* event) override {
        const QPointF pos = event->position();
        drag_ = near(pos);
        if (!drag_) {
            const auto [t, offset] = value(pos);
            if (t <= 0.0 || t >= 1.0) return;
            points_.push_back({t, offset});
            std::stable_sort(points_.begin(), points_.end(), [](const auto& a, const auto& b) { return a[0] < b[0]; });
            if (points_.size() > core::kShearMostPoints) {
                points_.erase(std::find(points_.begin(), points_.end(), std::array<double, 2>{t, offset}));
                return;
            }
            drag_ = near(pos);
            changed_();
        }
        update();
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (!drag_) return;
        auto& pt = points_[*drag_];
        const auto [t, offset] = value(event->position());
        pt[1] = offset;
        if (*drag_ > 0 && *drag_ + 1 < points_.size()) pt[0] = std::clamp(t, points_[*drag_ - 1][0] + 0.01, points_[*drag_ + 1][0] - 0.01);
        update();
        changed_();
    }
    void mouseReleaseEvent(QMouseEvent*) override { drag_.reset(); }
    void mouseDoubleClickEvent(QMouseEvent* event) override {
        const auto at = near(event->position());
        if (!at || *at == 0 || *at + 1 == points_.size()) return;
        points_.erase(points_.begin() + static_cast<std::ptrdiff_t>(*at));
        drag_.reset();
        update();
        changed_();
    }

private:
    QPointF view(double t, double offset) const { return QPointF(width() / 2.0 + offset * (width() / 2.0 - 6.0), t * height()); }
    std::pair<double, double> value(QPointF pos) const {
        return {std::clamp(pos.y() / std::max(1, height()), 0.0, 1.0), std::clamp((pos.x() - width() / 2.0) / (width() / 2.0 - 6.0), -1.0, 1.0)};
    }
    std::optional<std::size_t> near(QPointF pos) const {
        for (std::size_t i = 0; i < points_.size(); ++i) {
            const QPointF at = view(points_[i][0], points_[i][1]);
            if (std::hypot(at.x() - pos.x(), at.y() - pos.y()) <= 6.0) return i;
        }
        return std::nullopt;
    }
    std::vector<std::array<double, 2>> points_{{0, 0}, {1, 0}};
    std::optional<std::size_t> drag_;
    std::function<void()> changed_;
};

void shear_curve(adjust_forms::Form& form, const adjust_forms::FormContext&) {
    auto* curve = new ShearCurve([&form] { emit form.changed(); });
    const Json fallback = Json::array({Json::array({0, 0}), Json::array({1, 0})});
    form.custom(
        "points", QStringLiteral("曲線"), curve,
        [curve] {
            Json out = Json::array();
            for (const auto& pt : curve->points()) out.push_back(Json::array({std::round(pt[0] * 1000.0) / 1000.0, std::round(pt[1] * 1000.0) / 1000.0}));
            return out;
        },
        [curve](const Json& v) {
            std::vector<std::array<double, 2>> pts;
            for (std::size_t i = 0; v.is_array() && i < v.size(); ++i) {
                const Json& p = v[i];
                if (p.is_array() && p.size() == 2 && p[0].is_number() && p[1].is_number()) pts.push_back({p[0].get<double>(), p[1].get<double>()});
            }
            curve->set_points(std::move(pts));
        },
        fallback);
}

// --- 置き換え…'s map ----------------------------------------------------------------------------------------------------

// The map: a layer of the page or its merged picture (map), or a PNG or PSD file read here (map_data, base64); a smart
// filter's map the book holds kept as it is until another is chosen.
void displacement_map(adjust_forms::Form& form, const adjust_forms::FormContext& context) {
    auto* box = new QWidget;
    auto* row = new QHBoxLayout(box);
    row->setContentsMargins(0, 0, 0, 0);
    auto* choose = new QComboBox;
    choose->setObjectName(QStringLiteral("displace_map"));
    auto* file = new QPushButton(QStringLiteral("ファイル…"));
    file->setObjectName(QStringLiteral("displace_map_file"));
    row->addWidget(choose, 1);
    row->addWidget(file);
    struct State {
        Json held;       // (a smart filter's map the book holds: "sha256:…")
        QString name;    // (the file chosen)
        std::string data;  // (its bytes, base64)
    };
    auto state = std::make_shared<State>();
    const auto fill = [choose, state, context] {
        const QSignalBlocker quiet(choose);
        choose->clear();
        if (state->held.is_string()) choose->addItem(QStringLiteral("今の置き換えマップ"), QStringLiteral("held"));
        if (!state->data.empty()) choose->addItem(QStringLiteral("ファイル（%1）").arg(state->name), QStringLiteral("file"));
        choose->addItem(QStringLiteral("統合した絵"), QStringLiteral("merged"));
        if (context.page != nullptr) {
            for (auto it = context.page->layers.rbegin(); it != context.page->layers.rend(); ++it)
                if (it->kind != core::LayerKind::Folder && it->kind != core::LayerKind::Adjust)  // (no picture of their own)
                    choose->addItem(QStringLiteral("レイヤー「%1」").arg(wording::layer_label(*it)), QString::fromStdString("layer:" + it->id));
        }
    };
    const auto select = [choose](const QString& data) {
        const int at = choose->findData(data);
        if (at >= 0) choose->setCurrentIndex(at);
    };
    fill();
    QObject::connect(choose, &QComboBox::currentIndexChanged, &form, [&form] { emit form.changed(); });
    QObject::connect(file, &QPushButton::clicked, &form, [&form, choose, state, fill, select] {
        const QString path = QFileDialog::getOpenFileName(&form, QStringLiteral("置き換えマップを選んでください"), QString(),
                                                          QStringLiteral("置き換えマップ (*.psd *.png);;すべてのファイル (*)"));
        if (path.isEmpty()) return;
        QFile in(path);
        const QString name = QFileInfo(path).fileName();
        if (!in.open(QIODevice::ReadOnly) || in.size() > static_cast<qint64>(core::kDisplacementMapMaxBytes)) {
            ask::warning(&form, QStringLiteral("置き換え"), QStringLiteral("「%1」を読めませんでした（PNG か PSD、32 MB まで）").arg(name));
            return;
        }
        const QByteArray bytes = in.readAll();
        if (!bytes.startsWith(QByteArray("\x89PNG\r\n\x1a\n", 8)) && !bytes.startsWith("8BPS")) {
            ask::warning(&form, QStringLiteral("置き換え"), QStringLiteral("「%1」は PNG でも PSD でもありません").arg(name));
            return;
        }
        state->name = name;
        state->data = bytes.toBase64().toStdString();
        fill();
        select(QStringLiteral("file"));
        emit form.changed();
    });
    // (two settings: the map — a layer or the book's file — and the file's data; the one chosen given, the other none)
    form.custom(
        "map", QStringLiteral("置き換えマップ"), box,
        [choose, state]() -> Json {
            const QString data = choose->currentData().toString();
            if (data == QLatin1String("held")) return state->held;
            if (data == QLatin1String("merged")) return Json{{"layer", "merged"}};
            if (data.startsWith(QLatin1String("layer:"))) return Json{{"layer", data.mid(6).toStdString()}};
            return Json();  // (the file: its data)
        },
        [state, fill, select](const Json& v) {
            if (v.is_string()) state->held = v;
            fill();
            if (v.is_string()) select(QStringLiteral("held"));
            else if (v.is_object() && core::get(v, "layer") != nullptr && v["layer"] == Json("merged")) select(QStringLiteral("merged"));
            else if (v.is_object() && core::get(v, "layer") != nullptr && v["layer"].is_string()) select(QString::fromStdString("layer:" + v["layer"].get<std::string>()));
        },
        Json{{"layer", "merged"}});
    auto* data_label = new QLabel;
    data_label->setVisible(false);
    form.custom(
        "map_data", QString(), data_label,
        [choose, state]() -> Json { return choose->currentData().toString() == QLatin1String("file") ? Json(state->data) : Json(); }, [](const Json&) {}, Json());
}

}  // namespace

std::vector<FilterForm> distort_forms() {
    std::vector<FilterForm> out;
    {  // 置き換え… (水平比率・垂直比率 −999–999 %, 置き換えマップ 伸縮して合わせる・並べて表示, 未定義領域)
        FilterForm f;
        f.kind = "distort_displace";
        f.labels = {{"horizontal", QStringLiteral("水平比率（%）")}, {"vertical", QStringLiteral("垂直比率（%）")}, {"fit", QStringLiteral("置き換えマップの合わせ方")},
                    {"undefined_areas", QStringLiteral("未定義領域")}};
        f.choices = {{"fit", {{QStringLiteral("伸縮して合わせる"), Json("stretch")}, {QStringLiteral("並べて表示"), Json("tile")}}},
                     {"undefined_areas", undefined_areas(false)}};
        f.decimals = {{"horizontal", 0}, {"vertical", 0}};
        f.more = displacement_map;
        f.note = QStringLiteral("置き換えマップの赤が横に、緑が縦に動かします（50% のグレーは動かさず、透明な所も動かしません）。100% で 200 dpi の 128 画素まで動きます。"
                                "マップはこの原稿のレイヤー・統合した絵か、PNG・PSD のファイル（32 MB まで）です。並べて表示では、マップの 1 画素を 200 dpi の 1 画素として並べます。");
        f.smart_note = QStringLiteral("スマートフィルターでは、選んだレイヤーやファイルの絵を原稿の中に写して持ちます（後からレイヤーを変えても変わりません）。");
        out.push_back(std::move(f));
    }
    {  // つまむ… (量 −100–100 %)
        FilterForm f;
        f.kind = "distort_pinch";
        f.labels = {{"amount", QStringLiteral("量（%）")}};
        f.decimals = {{"amount", 0}};
        f.note = kBaseBox;
        out.push_back(std::move(f));
    }
    {  // 極座標… (直交座標を極座標に・極座標を直交座標に)
        FilterForm f;
        f.kind = "distort_polar_coordinates";
        f.labels = {{"mode", QStringLiteral("変換")}};
        f.choices = {{"mode", {{QStringLiteral("直交座標を極座標に"), Json("rectangular_to_polar")}, {QStringLiteral("極座標を直交座標に"), Json("polar_to_rectangular")}}}};
        f.note = kBaseBox;
        out.push_back(std::move(f));
    }
    {  // 波紋… (量 −999–999 %, サイズ 小・中・大)
        FilterForm f;
        f.kind = "distort_ripple";
        f.labels = {{"amount", QStringLiteral("量（%）")}, {"size", QStringLiteral("サイズ")}};
        f.choices = {{"size", {{QStringLiteral("小"), Json("small")}, {QStringLiteral("中"), Json("medium")}, {QStringLiteral("大"), Json("large")}}}};
        f.decimals = {{"amount", 0}};
        f.note = kLengths;
        out.push_back(std::move(f));
    }
    {  // シアー… (its curve, 未定義領域)
        FilterForm f;
        f.kind = "distort_shear";
        f.labels = {{"undefined_areas", QStringLiteral("未定義領域")}};
        f.choices = {{"undefined_areas", undefined_areas(true)}};
        f.more = shear_curve;
        f.note = QStringLiteral("線をクリックして点を足し、ドラッグで横にずらします（ダブルクリックで点を消します）。上から下までの各行を、線の位置だけ横にずらします。");
        out.push_back(std::move(f));
    }
    {  // 球面… (量 −100–100 %, モード 標準・水平方向のみ・垂直方向のみ)
        FilterForm f;
        f.kind = "distort_spherize";
        f.labels = {{"amount", QStringLiteral("量（%）")}, {"mode", QStringLiteral("モード")}};
        f.choices = {{"mode", {{QStringLiteral("標準"), Json("normal")}, {QStringLiteral("水平方向のみ"), Json("horizontal_only")},
                               {QStringLiteral("垂直方向のみ"), Json("vertical_only")}}}};
        f.decimals = {{"amount", 0}};
        f.note = kBaseBox;
        out.push_back(std::move(f));
    }
    {  // 渦巻き… (角度 −999–999°)
        FilterForm f;
        f.kind = "distort_twirl";
        f.labels = {{"angle", QStringLiteral("角度（°）")}};
        f.decimals = {{"angle", 0}};
        f.note = kBaseBox;
        out.push_back(std::move(f));
    }
    {  // 波形… (生成機能の数, 波長・振幅の最小と最大, 種類, 未定義領域)
        FilterForm f;
        f.kind = "distort_wave";
        f.labels = {{"generators", QStringLiteral("生成機能の数")},     {"wavelength_min", QStringLiteral("波長 最小（px）")},
                    {"wavelength_max", QStringLiteral("波長 最大（px）")}, {"amplitude_min", QStringLiteral("振幅 最小（px）")},
                    {"amplitude_max", QStringLiteral("振幅 最大（px）")},  {"type", QStringLiteral("種類")},
                    {"undefined_areas", QStringLiteral("未定義領域")}};
        f.choices = {{"type", {{QStringLiteral("正弦波"), Json("sine")}, {QStringLiteral("三角波"), Json("triangle")}, {QStringLiteral("矩形波"), Json("square")}}},
                     {"undefined_areas", undefined_areas(true)}};
        f.decimals = {{"wavelength_min", 0}, {"wavelength_max", 0}, {"amplitude_min", 0}, {"amplitude_max", 0}};
        f.note = kLengths + QStringLiteral("\n生成機能は 64 まで重ねます（それより多くても 64 と同じです）。");
        out.push_back(std::move(f));
    }
    {  // ジグザグ… (量 −100–100, 折り返し 0–20, スタイル)
        FilterForm f;
        f.kind = "distort_zigzag";
        f.labels = {{"amount", QStringLiteral("量")}, {"ridges", QStringLiteral("折り返し")}, {"style", QStringLiteral("スタイル")}};
        f.choices = {{"style", {{QStringLiteral("水面の波紋"), Json("pond_ripples")}, {QStringLiteral("中心から外へ"), Json("out_from_center")},
                                {QStringLiteral("中心の周囲"), Json("around_center")}}}};
        f.decimals = {{"amount", 0}, {"ridges", 0}};
        f.note = kBaseBox;
        out.push_back(std::move(f));
    }
    return out;
}

std::vector<FilterForm> pixelate_forms() {
    std::vector<FilterForm> out;
    {  // カラーハーフトーン… (最大半径 4–127 pixel, スクリーン角度 チャンネル 1–4)
        FilterForm f;
        f.kind = "pixelate_color_halftone";
        f.labels = {{"max_radius", QStringLiteral("最大半径（px）")}, {"channel1", QStringLiteral("スクリーン角度 チャンネル 1（°）")},
                    {"channel2", QStringLiteral("チャンネル 2（°）")},  {"channel3", QStringLiteral("チャンネル 3（°）")},
                    {"channel4", QStringLiteral("チャンネル 4（°）")}};
        f.decimals = {{"max_radius", 0}, {"channel1", 0}, {"channel2", 0}, {"channel3", 0}, {"channel4", 0}};
        f.note = kLengths + QStringLiteral("\nCMYK のページでは 4 つの版にチャンネル 1〜4 の角度で網をかけます（RGB は 1〜3、グレーは 1）。");
        out.push_back(std::move(f));
    }
    {  // 水晶… (セルの大きさ 3–300)
        FilterForm f;
        f.kind = "pixelate_crystallize";
        f.labels = {{"cell_size", QStringLiteral("セルの大きさ（px）")}};
        f.decimals = {{"cell_size", 0}};
        f.note = kLengths;
        out.push_back(std::move(f));
    }
    {  // メゾティント… (種類)
        FilterForm f;
        f.kind = "pixelate_mezzotint";
        f.labels = {{"type", QStringLiteral("種類")}};
        f.choices = {{"type",
                      {{QStringLiteral("細かいドット"), Json("fine_dots")},     {QStringLiteral("中間のドット"), Json("medium_dots")},
                       {QStringLiteral("粒状ドット"), Json("grainy_dots")},     {QStringLiteral("粗いドット"), Json("coarse_dots")},
                       {QStringLiteral("短い線"), Json("short_lines")},         {QStringLiteral("中間の線"), Json("medium_lines")},
                       {QStringLiteral("長い線"), Json("long_lines")},          {QStringLiteral("短いストローク"), Json("short_strokes")},
                       {QStringLiteral("中間のストローク"), Json("medium_strokes")}, {QStringLiteral("長いストローク"), Json("long_strokes")}}}};
        f.note = QStringLiteral("模様は 200 dpi の画素で決まります（600 dpi のページでも紙の上で同じ大きさです）。");
        out.push_back(std::move(f));
    }
    {  // モザイク… (セルの大きさ 2–200 平方)
        FilterForm f;
        f.kind = "pixelate_mosaic";
        f.labels = {{"cell_size", QStringLiteral("セルの大きさ（px）")}};
        f.decimals = {{"cell_size", 0}};
        f.note = kLengths + QStringLiteral("\nセルは選択範囲（無ければレイヤー）の左上からそろえます。");
        out.push_back(std::move(f));
    }
    {  // 点描… (セルの大きさ 3–300, 背景色)
        FilterForm f;
        f.kind = "pixelate_pointillize";
        f.labels = {{"cell_size", QStringLiteral("セルの大きさ（px）")}, {"background", QStringLiteral("背景色")}};
        f.decimals = {{"cell_size", 0}};
        f.note = kLengths;
        out.push_back(std::move(f));
    }
    return out;
}

std::vector<FilterForm> stylize_forms() {
    std::vector<FilterForm> out;
    {  // 拡散… (モード)
        FilterForm f;
        f.kind = "stylize_diffuse";
        f.labels = {{"mode", QStringLiteral("モード")}};
        f.choices = {{"mode", {{QStringLiteral("標準"), Json("normal")}, {QStringLiteral("比較（暗）"), Json("darken_only")},
                               {QStringLiteral("比較（明）"), Json("lighten_only")}, {QStringLiteral("異方性"), Json("anisotropic")}}}};
        out.push_back(std::move(f));
    }
    {  // エンボス… (角度 −180–180°, 高さ 1–100 pixel, 量 1–500 %)
        FilterForm f;
        f.kind = "stylize_emboss";
        f.labels = {{"angle", QStringLiteral("角度（°）")}, {"height", QStringLiteral("高さ（px）")}, {"amount", QStringLiteral("量（%）")}};
        f.decimals = {{"angle", 0}, {"height", 0}, {"amount", 0}};
        f.note = kLengths;
        out.push_back(std::move(f));
    }
    {  // 押し出し… (種類 ブロック・ピラミッド, サイズ 2–255 pixel, 奥行き 1–255, ランダム・レベルに基づく, 2 つのチェック)
        FilterForm f;
        f.kind = "stylize_extrude";
        f.labels = {{"type", QStringLiteral("種類")},
                    {"size", QStringLiteral("サイズ（px）")},
                    {"depth", QStringLiteral("奥行き")},
                    {"depth_mode", QStringLiteral("奥行きの決め方")},
                    {"solid_front_faces", QStringLiteral("前面を立方体にする")},
                    {"mask_incomplete_blocks", QStringLiteral("不完全なブロックをマスク")}};
        f.choices = {{"type", {{QStringLiteral("ブロック"), Json("blocks")}, {QStringLiteral("ピラミッド"), Json("pyramids")}}},
                     {"depth_mode", {{QStringLiteral("ランダム"), Json("random")}, {QStringLiteral("レベルに基づく"), Json("level_based")}}}};
        f.decimals = {{"size", 0}, {"depth", 0}};
        f.note = kLengths;
        out.push_back(std::move(f));
    }
    {  // 油彩… (ブラシ: 様式化・清潔さ・拡大・縮小・密度, 照明: 角度・光沢)
        FilterForm f;
        f.kind = "stylize_oil_paint";
        f.labels = {{"stylization", QStringLiteral("様式化")},  {"cleanliness", QStringLiteral("清潔さ")}, {"scale", QStringLiteral("拡大・縮小")},
                    {"bristle_detail", QStringLiteral("密度")}, {"lighting", QStringLiteral("照明")},     {"angle", QStringLiteral("角度（°）")},
                    {"shine", QStringLiteral("光沢")}};
        f.decimals = {{"stylization", 1}, {"cleanliness", 1}, {"scale", 1}, {"bristle_detail", 1}, {"angle", 0}, {"shine", 1}};
        f.note = QStringLiteral("筆の長さと模様は 200 dpi の画素で決まります（600 dpi のページでも紙の上で同じ大きさです）。");
        out.push_back(std::move(f));
    }
    {  // タイル… (タイルの数 1–99, 最大移動距離 1–99 %, 隙間)
        FilterForm f;
        f.kind = "stylize_tiles";
        f.labels = {{"count", QStringLiteral("タイルの数")}, {"max_offset", QStringLiteral("最大移動距離（%）")}, {"fill", QStringLiteral("隙間")},
                    {"foreground", QStringLiteral("描画色")}, {"background", QStringLiteral("背景色")}};
        f.choices = {{"fill", {{QStringLiteral("背景色"), Json("background")}, {QStringLiteral("描画色"), Json("foreground")},
                               {QStringLiteral("反転イメージ"), Json("inverse")}, {QStringLiteral("元のイメージ"), Json("unaltered")}}}};
        f.decimals = {{"max_offset", 0}};
        f.note = QStringLiteral("タイルの数は、選択範囲（無ければレイヤー）の短い辺に並ぶ数です。");
        out.push_back(std::move(f));
    }
    {  // 輪郭のトレース… (レベル 0–255, エッジ 下・上)
        FilterForm f;
        f.kind = "stylize_trace_contour";
        f.labels = {{"level", QStringLiteral("レベル")}, {"edge", QStringLiteral("エッジ")}};
        f.choices = {{"edge", {{QStringLiteral("下"), Json("lower")}, {QStringLiteral("上"), Json("upper")}}}};
        f.decimals = {{"level", 0}};
        out.push_back(std::move(f));
    }
    {  // 風… (方式 標準・強く・ずらし, 方向 右から・左から)
        FilterForm f;
        f.kind = "stylize_wind";
        f.labels = {{"method", QStringLiteral("方式")}, {"direction", QStringLiteral("方向")}};
        f.choices = {{"method", {{QStringLiteral("標準"), Json("wind")}, {QStringLiteral("強く"), Json("blast")}, {QStringLiteral("ずらし"), Json("stagger")}}},
                     {"direction", {{QStringLiteral("右から"), Json("from_right")}, {QStringLiteral("左から"), Json("from_left")}}}};
        f.note = kLengths;
        out.push_back(std::move(f));
    }
    return out;
}

}  // namespace genko::app::filter_forms
