// PC2b (色の補正): the forms of the colour corrections (hue_saturation — its ranges and their band —, vibrance,
// color_balance — its tones —, black_white, photo_filter — its presets —, channel_mixer — its outputs —,
// selective_color — its ranges) and their commands in レイヤー → 色調補正 (app/adjust_forms.hpp): each a destructive
// correction (filter_raster) on the layer drawn on, inside the selection, the page showing the result while its
// window is open; 彩度を下げる straight on.

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "app/adjust_forms.hpp"
#include "app/main_window.hpp"
#include "core/adjust_color.hpp"

namespace genko::app::adjust_forms {

using core::Json;
namespace color = core::color_adjust;

namespace {

Json whole_or(double v) {
    if (v == std::floor(v) && std::fabs(v) < 1e15) return Json(static_cast<std::int64_t>(v));
    return Json(v);
}

Json numbers_json(const std::vector<double>& values) {
    Json out = Json::array();
    for (const double v : values) out.push_back(whole_or(v));
    return out;
}

// --- records edited one at a time ------------------------------------------------------------------------------------

struct Slot {
    QString label;
    double lo, hi;
};

// Several records of the same numbers, one shown at a time (Color Balance's tones, Selective Color's ranges, Channel
// Mixer's outputs, Hue/Saturation's ranges): a chooser of which, a field for each number.
class Records : public QWidget {
public:
    Records(const QString& chooser, const std::vector<QString>& names, std::vector<Slot> fields, std::vector<std::vector<double>> fallbacks, int first = 0)
        : values(fallbacks), fallbacks_(std::move(fallbacks)) {
        auto* grid = new QGridLayout(this);
        grid->setContentsMargins(0, 0, 0, 0);
        chooser_ = new QComboBox;
        chooser_->setObjectName(QStringLiteral("record_chooser"));
        for (const QString& name : names) chooser_->addItem(name);
        chooser_->setCurrentIndex(first);
        grid->addWidget(new QLabel(chooser), 0, 0);
        grid->addWidget(chooser_, 0, 1);
        for (std::size_t k = 0; k < fields.size(); ++k) {
            auto* spin = new QDoubleSpinBox;
            spin->setObjectName(QStringLiteral("record_%1").arg(k));
            spin->setRange(fields[k].lo, fields[k].hi);
            spin->setDecimals(0);
            grid->addWidget(new QLabel(fields[k].label), static_cast<int>(k) + 1, 0);
            grid->addWidget(spin, static_cast<int>(k) + 1, 1);
            spins_.push_back(spin);
            connect(spin, &QDoubleSpinBox::valueChanged, this, [this, k](double v) {
                values[static_cast<std::size_t>(current())][k] = v;
                touched();
            });
        }
        connect(chooser_, &QComboBox::currentIndexChanged, this, [this] { show_current(); });
        show_current();
    }

    int current() const { return std::max(0, chooser_->currentIndex()); }
    void choose(int i) { chooser_->setCurrentIndex(i); }
    // The fields from the record chosen (no change said).
    void show_current() {
        const std::vector<double>& now = values[static_cast<std::size_t>(current())];
        for (std::size_t k = 0; k < spins_.size(); ++k) {
            const QSignalBlocker quiet(spins_[k]);
            spins_[k]->setValue(now[k]);
        }
        if (redraw) redraw();
    }
    // A record set from outside (the form's set_values): none said changed.
    void set(std::size_t i, std::vector<double> v) {
        values[i] = std::move(v);
        if (static_cast<int>(i) == current()) show_current();
    }
    const std::vector<double>& fallback(std::size_t i) const { return fallbacks_[i]; }
    // A number of the record chosen changed by a widget of the caller's (the band).
    void change(std::size_t k, double v) {
        values[static_cast<std::size_t>(current())][k] = v;
        const QSignalBlocker quiet(spins_[k]);
        spins_[k]->setValue(v);
        touched();
    }

    std::vector<std::vector<double>> values;
    std::function<void()> changed;  // (the form's changed)
    std::function<void()> redraw;   // (a band of the caller's)

private:
    void touched() {
        if (redraw) redraw();
        if (changed) changed();
    }
    QComboBox* chooser_ = nullptr;
    std::vector<QDoubleSpinBox*> spins_;
    std::vector<std::vector<double>> fallbacks_;
};

// The records as the form's fields `keys` (one widget edits them all: the rows after the first hidden), each read and
// written as JSON.
void add_records(Form& form, Records* records, const QString& label, const std::vector<std::string>& keys,
                 std::function<Json(const std::vector<double>&)> to_json, std::function<std::vector<double>(const Json&, std::size_t)> from_json) {
    records->changed = [&form] { emit form.changed(); };
    for (std::size_t i = 0; i < keys.size(); ++i) {
        QWidget* holder = i == 0 ? static_cast<QWidget*>(records) : new QWidget;
        form.custom(
            keys[i], i == 0 ? label : QString(), holder, [records, i, to_json] { return to_json(records->values[i]); },
            [records, i, from_json](const Json& v) { records->set(i, from_json(v, i)); }, to_json(records->fallback(i)));
        if (i > 0) form.layout_form()->setRowVisible(holder, false);
    }
}

// A list of numbers from JSON (a value missing or not a number: the fallback's).
std::vector<double> list_or(const Json& v, const std::vector<double>& fallback) {
    std::vector<double> out = fallback;
    if (!v.is_array()) return out;
    for (std::size_t k = 0; k < out.size() && k < v.size(); ++k) {
        if (v[k].is_number()) out[k] = v[k].get<double>();
    }
    return out;
}

// --- Hue/Saturation's band ---------------------------------------------------------------------------------------------

// The hue round a range (its centre ±180°) with its four sliders: where it fades in, is fully on, fades out and ends —
// each dragged between its neighbours.
class HueBand : public QWidget {
public:
    explicit HueBand(Records* records) : records_(records) {
        setObjectName(QStringLiteral("hue_band"));
        setMinimumSize(260, 34);
        setToolTip(QStringLiteral("色域の範囲（つまみをドラッグ）: 内側の 2 つの間が全部、外側の 2 つまでで薄れる"));
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        const QRectF bar(kMargin, 4, width() - 2 * kMargin, height() - 16);
        const double from = left();
        QLinearGradient hues(bar.topLeft(), bar.topRight());
        for (int k = 0; k <= 12; ++k) {
            const double deg = from + k * 30.0;
            hues.setColorAt(k / 12.0, QColor::fromHsvF(static_cast<float>(std::fmod(std::fmod(deg, 360) + 360, 360) / 360), 1.0F, 1.0F));
        }
        p.fillRect(bar, hues);
        const auto b = bounds();
        // how much each hue belongs to the range
        QPainterPath shape;
        shape.moveTo(x_of(b[0]), bar.bottom());
        shape.lineTo(x_of(b[1]), bar.top());
        shape.lineTo(x_of(b[2]), bar.top());
        shape.lineTo(x_of(b[3]), bar.bottom());
        p.setPen(QPen(palette().color(QPalette::WindowText), 1.5));
        p.drawPath(shape);
        for (std::size_t k = 0; k < 4; ++k) {
            const double x = x_of(b[k]);
            QPolygonF mark;
            mark << QPointF(x, bar.bottom() + 1) << QPointF(x - 5, height() - 1.0) << QPointF(x + 5, height() - 1.0);
            p.setBrush(k == 1 || k == 2 ? palette().color(QPalette::WindowText) : palette().color(QPalette::Base));
            p.drawPolygon(mark);
        }
    }
    void mousePressEvent(QMouseEvent* event) override {
        const auto b = bounds();
        dragging_ = -1;
        double nearest = 9;
        for (std::size_t k = 0; k < 4; ++k) {
            const double d = std::fabs(x_of(b[k]) - event->position().x());
            if (d < nearest) {
                nearest = d;
                dragging_ = static_cast<int>(k);
            }
        }
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (dragging_ < 0) return;
        const auto b = bounds();
        const auto k = static_cast<std::size_t>(dragging_);
        double deg = std::round(left() + (event->position().x() - kMargin) / std::max(1.0, width() - 2.0 * kMargin) * 360);
        if (k > 0) deg = std::max(deg, b[k - 1]);
        if (k < 3) deg = std::min(deg, b[k + 1]);
        if (deg != b[k]) records_->change(3 + k, deg);
    }
    void mouseReleaseEvent(QMouseEvent*) override { dragging_ = -1; }

private:
    static constexpr double kMargin = 6;
    std::array<double, 4> bounds() const {
        const std::vector<double>& r = records_->values[static_cast<std::size_t>(records_->current())];
        return {r[3], r[4], r[5], r[6]};
    }
    double left() const {
        const auto b = bounds();
        return (b[1] + b[2]) / 2 - 180;
    }
    double x_of(double deg) const { return kMargin + (deg - left()) / 360 * (width() - 2 * kMargin); }
    Records* records_;
    int dragging_ = -1;
};

const std::vector<QString> kHueNames{QStringLiteral("レッド系"), QStringLiteral("イエロー系"), QStringLiteral("グリーン系"),
                                     QStringLiteral("シアン系"), QStringLiteral("ブルー系"),   QStringLiteral("マゼンタ系")};

void hue_saturation_form(Form& form, const FormContext&) {
    // (colorize first: its hue 0..360 and saturation 0..100 set before the numbers are)
    form.check("colorize", QStringLiteral("色彩の統一"), false);
    form.number("hue", QStringLiteral("色相"), -180, 180, 0, 0);
    form.number("saturation", QStringLiteral("彩度"), -100, 100, 0, 0);
    form.number("lightness", QStringLiteral("明度"), -100, 100, 0, 0);
    auto* colorize = form.findChild<QCheckBox*>(QStringLiteral("field_colorize"));
    auto* hue = form.findChild<QDoubleSpinBox*>(QStringLiteral("field_hue"));
    auto* saturation = form.findChild<QDoubleSpinBox*>(QStringLiteral("field_saturation"));
    std::vector<std::vector<double>> fallbacks;
    for (int i = 0; i < 6; ++i) {
        const color::HueRange r = color::neutral_range(i);
        fallbacks.push_back({0, 0, 0, r.bounds[0], r.bounds[1], r.bounds[2], r.bounds[3]});
    }
    auto* records = new Records(QStringLiteral("色域"), kHueNames,
                                {{QStringLiteral("色相"), -180, 180}, {QStringLiteral("彩度"), -100, 100}, {QStringLiteral("明度"), -100, 100},
                                 {QStringLiteral("薄れ始め"), -720, 720}, {QStringLiteral("全部（左）"), -720, 720}, {QStringLiteral("全部（右）"), -720, 720},
                                 {QStringLiteral("薄れ終わり"), -720, 720}},
                                fallbacks);
    auto* band = new HueBand(records);
    records->redraw = [band] { band->update(); };
    auto* box = new QWidget;
    auto* column = new QVBoxLayout(box);
    column->setContentsMargins(0, 0, 0, 0);
    column->addWidget(records);
    column->addWidget(band);
    std::vector<std::string> keys;
    for (const std::string_view key : color::kHueRanges) keys.emplace_back(key);
    // (the records' widget is the first row's: its box holds the band too)
    records->changed = [&form] { emit form.changed(); };
    for (std::size_t i = 0; i < keys.size(); ++i) {
        QWidget* holder = i == 0 ? box : new QWidget;
        const auto to_json = [](const std::vector<double>& v) {
            return Json{{"hue", whole_or(v[0])}, {"saturation", whole_or(v[1])}, {"lightness", whole_or(v[2])}, {"range", numbers_json({v[3], v[4], v[5], v[6]})}};
        };
        form.custom(
            keys[i], i == 0 ? QStringLiteral("色域ごと") : QString(), holder, [records, i, to_json] { return to_json(records->values[i]); },
            [records, i](const Json& v) {
                std::vector<double> out = records->fallback(i);
                if (v.is_object()) {
                    const char* names[] = {"hue", "saturation", "lightness"};
                    for (std::size_t k = 0; k < 3; ++k) {
                        if (v.contains(names[k]) && v[names[k]].is_number()) out[k] = v[names[k]].get<double>();
                    }
                    if (v.contains("range")) {
                        const std::vector<double> range = list_or(v["range"], {out[3], out[4], out[5], out[6]});
                        std::copy(range.begin(), range.end(), out.begin() + 3);
                    }
                }
                records->set(i, std::move(out));
            },
            to_json(records->fallback(i)));
        if (i > 0) form.layout_form()->setRowVisible(holder, false);
    }
    form.note(QStringLiteral("色域ごとの指定は、その色相の色にだけ掛かります（灰色に近い色ほど弱く）。色彩の統一では、色相（0〜360）と彩度（0〜100）で全体を 1 色に染めます。"));
    QObject::connect(colorize, &QCheckBox::toggled, &form, [hue, saturation, records](bool on) {
        hue->setRange(on ? 0 : -180, on ? 360 : 180);
        saturation->setRange(on ? 0 : -100, 100);
        records->setEnabled(!on);  // (a colorize leaves the ranges out)
    });
}

void vibrance_form(Form& form, const FormContext&) {
    form.from_table(*core::adjust_kind("vibrance"), {{"vibrance", QStringLiteral("自然な彩度")}, {"saturation", QStringLiteral("彩度")}});
    form.note(QStringLiteral("自然な彩度は、彩度の低い色ほど強く動かします（灰色は動きません）。"));
}

void color_balance_form(Form& form, const FormContext&) {
    std::vector<std::vector<double>> fallbacks(3, std::vector<double>{0, 0, 0});
    auto* records = new Records(QStringLiteral("階調"), {QStringLiteral("シャドウ"), QStringLiteral("中間調"), QStringLiteral("ハイライト")},
                                {{QStringLiteral("シアン ⇔ レッド"), -100, 100}, {QStringLiteral("マゼンタ ⇔ グリーン"), -100, 100},
                                 {QStringLiteral("イエロー ⇔ ブルー"), -100, 100}},
                                fallbacks, 1);
    add_records(form, records, QStringLiteral("カラーバランス"), {"shadows", "midtones", "highlights"}, numbers_json,
                [records](const Json& v, std::size_t i) { return list_or(v, records->fallback(i)); });
    form.check("preserve_luminosity", QStringLiteral("輝度を保持"), true);
}

void black_white_form(Form& form, const FormContext&) {
    form.from_table(*core::adjust_kind("black_white"),
                    {{"reds", QStringLiteral("レッド系")}, {"yellows", QStringLiteral("イエロー系")}, {"greens", QStringLiteral("グリーン系")},
                     {"cyans", QStringLiteral("シアン系")}, {"blues", QStringLiteral("ブルー系")}, {"magentas", QStringLiteral("マゼンタ系")},
                     {"tint", QStringLiteral("着色")}, {"tint_color", QStringLiteral("着色の色")}});
    form.note(QStringLiteral("色ごとの数（％）で、その色をどれだけ明るい灰色にするかを決めます。"));
}

QString preset_label(std::string_view id) {
    static const std::map<std::string_view, QString> labels{
        {"warming85", QStringLiteral("暖色系フィルター (85)")},   {"warmingLBA", QStringLiteral("暖色系フィルター (LBA)")},
        {"warming81", QStringLiteral("暖色系フィルター (81)")},   {"cooling80", QStringLiteral("寒色系フィルター (80)")},
        {"coolingLBB", QStringLiteral("寒色系フィルター (LBB)")}, {"cooling82", QStringLiteral("寒色系フィルター (82)")},
        {"red", QStringLiteral("レッド")},                        {"orange", QStringLiteral("オレンジ")},
        {"yellow", QStringLiteral("イエロー")},                   {"green", QStringLiteral("グリーン")},
        {"cyan", QStringLiteral("シアン")},                       {"blue", QStringLiteral("ブルー")},
        {"violet", QStringLiteral("バイオレット")},               {"magenta", QStringLiteral("マゼンタ")},
        {"sepia", QStringLiteral("セピア")},                      {"deepRed", QStringLiteral("ディープレッド")},
        {"deepBlue", QStringLiteral("ディープブルー")},           {"deepEmerald", QStringLiteral("ディープエメラルド")},
        {"deepYellow", QStringLiteral("ディープイエロー")},       {"underwater", QStringLiteral("水中")}};
    const auto it = labels.find(id);
    return it != labels.end() ? it->second : QString::fromUtf8(id.data(), static_cast<qsizetype>(id.size()));
}

void photo_filter_form(Form& form, const FormContext&) {
    // (a preset sets the colour; the colour is the setting — a colour of none of them shows カスタム)
    auto* presets = new QComboBox;
    presets->setObjectName(QStringLiteral("photo_filter_preset"));
    for (const color::PhotoFilterPreset& each : color::photo_filters()) presets->addItem(preset_label(each.id), QString::fromUtf8(each.id.data(), static_cast<qsizetype>(each.id.size())));
    presets->addItem(QStringLiteral("カスタム（色を選ぶ）"), QString());
    form.layout_form()->addRow(QStringLiteral("フィルター"), presets);
    form.from_table(*core::adjust_kind("photo_filter"),
                    {{"color", QStringLiteral("色")}, {"density", QStringLiteral("適用量（％）")}, {"preserve_luminosity", QStringLiteral("輝度を保持")}});
    QObject::connect(presets, &QComboBox::activated, &form, [&form, presets](int i) {
        const color::PhotoFilterPreset* preset = color::photo_filter(presets->itemData(i).toString().toStdString());
        if (preset == nullptr) return;
        Json now = form.values();
        now["color"] = Json::array({preset->rgb[0], preset->rgb[1], preset->rgb[2]});
        form.set_values(now);
    });
    const auto show = [&form, presets] {
        const Json colour = form.values().value("color", Json());
        int at = presets->count() - 1;
        for (int i = 0; i < presets->count() - 1; ++i) {
            const color::PhotoFilterPreset* preset = color::photo_filter(presets->itemData(i).toString().toStdString());
            if (preset != nullptr && colour == Json::array({preset->rgb[0], preset->rgb[1], preset->rgb[2]})) at = i;
        }
        const QSignalBlocker quiet(presets);
        presets->setCurrentIndex(at);
    };
    QObject::connect(&form, &Form::changed, presets, show);
    show();
}

void channel_mixer_form(Form& form, const FormContext&) {
    auto* records = new Records(QStringLiteral("出力先"),
                                {QStringLiteral("レッド"), QStringLiteral("グリーン"), QStringLiteral("ブルー"), QStringLiteral("グレー（モノクロ）")},
                                {{QStringLiteral("レッド（％）"), -200, 200}, {QStringLiteral("グリーン（％）"), -200, 200},
                                 {QStringLiteral("ブルー（％）"), -200, 200}, {QStringLiteral("定数（％）"), -200, 200}},
                                {{100, 0, 0, 0}, {0, 100, 0, 0}, {0, 0, 100, 0}, {40, 40, 20, 0}});
    add_records(form, records, QStringLiteral("チャンネル"), {"red", "green", "blue", "gray"}, numbers_json,
                [records](const Json& v, std::size_t i) { return list_or(v, records->fallback(i)); });
    form.check("monochrome", QStringLiteral("モノクロ"), false);
    auto* monochrome = form.findChild<QCheckBox*>(QStringLiteral("field_monochrome"));
    QObject::connect(monochrome, &QCheckBox::toggled, &form, [records](bool on) { records->choose(on ? 3 : 0); });
    form.note(QStringLiteral("出力の各チャンネルを、元のレッド・グリーン・ブルーの割合と定数で作ります。モノクロではグレーの行を 3 つのチャンネルに使います。"));
}

void selective_color_form(Form& form, const FormContext&) {
    form.choice("method", QStringLiteral("方法"), {{QStringLiteral("相対値"), Json("relative")}, {QStringLiteral("絶対値"), Json("absolute")}}, Json("relative"));
    auto* records = new Records(QStringLiteral("色域"),
                                {QStringLiteral("レッド系"), QStringLiteral("イエロー系"), QStringLiteral("グリーン系"), QStringLiteral("シアン系"),
                                 QStringLiteral("ブルー系"), QStringLiteral("マゼンタ系"), QStringLiteral("ホワイト系"), QStringLiteral("ニュートラル系"),
                                 QStringLiteral("ブラック系")},
                                {{QStringLiteral("シアン（％）"), -100, 100}, {QStringLiteral("マゼンタ（％）"), -100, 100},
                                 {QStringLiteral("イエロー（％）"), -100, 100}, {QStringLiteral("ブラック（％）"), -100, 100}},
                                std::vector<std::vector<double>>(9, std::vector<double>{0, 0, 0, 0}));
    std::vector<std::string> keys;
    for (const std::string_view key : color::kSelectiveRanges) keys.emplace_back(key);
    add_records(form, records, QStringLiteral("インキ"), keys, numbers_json, [records](const Json& v, std::size_t i) { return list_or(v, records->fallback(i)); });
    form.note(QStringLiteral("色域ごとに、その色のインキ（シアン・マゼンタ・イエロー・ブラック）を増やし減らします。相対値は今あるインキに比例します（白は染まりません）。"));
}

// --- the commands -------------------------------------------------------------------------------------------------------

// The destructive correction `kind` on the layer drawn on (inside the selection): its window with the page showing the
// result (none for 彩度を下げる), then filter_raster.
void correct(MainWindow* window, const std::string& kind, const QString& title) {
    const core::Page* page = window->current_page();
    const core::Layer* layer = window->target_layer();
    if (page == nullptr || layer == nullptr || !(layer->kind == core::LayerKind::Raster || layer->kind == core::LayerKind::Strokes)) {
        window->flash(QStringLiteral("%1は、ペンかペイントのレイヤーを選んでから使います").arg(title), 4000, true);
        return;
    }
    if (layer->locked) {
        window->flash(QStringLiteral("レイヤーがロックされています"), 4000, true);
        return;
    }
    const auto asked = window->asking();
    const std::string layer_id = layer->id;
    const Json index = page->index.json();
    const auto area = window->selection_area();
    const auto op_of = [kind, layer_id, index, area](const Json& params) {
        Json op{{"op", "filter_raster"}, {"page", index}, {"id", layer_id}, {"kind", kind}};
        op.update(params);
        if (area) op["area"] = *area;
        return op;
    };
    if (form_kind(kind) == nullptr) {  // (彩度を下げる: no settings)
        window->apply_ops(Json::array({op_of(Json::object())}));
        return;
    }
    FormContext context;
    context.page = page;
    const auto& rgb = window->pen().rgb;
    context.pen = {static_cast<int>(rgb[0]), static_cast<int>(rgb[1]), static_cast<int>(rgb[2])};
    const auto params = adjust_dialog(window, kind, Json::object(), context, [window, op_of](const std::optional<Json>& values) {
        window->preview_ops(values ? std::optional<Json>(Json::array({op_of(*values)})) : std::nullopt);
    });
    if (!params || !window->still(asked)) return;
    window->apply_ops(Json::array({op_of(*params)}));
}

}  // namespace

std::vector<FormKind> color_forms() {
    return {{"hue_saturation", "PC2b", QStringLiteral("色相・彩度"), hue_saturation_form, core::Json::object(), {}},
            {"vibrance", "PC2b", QStringLiteral("自然な彩度"), vibrance_form, core::Json::object(), {}},
            {"color_balance", "PC2b", QStringLiteral("カラーバランス"), color_balance_form, core::Json::object(), {}},
            {"black_white", "PC2b", QStringLiteral("白黒"), black_white_form, core::Json::object(), {}},
            {"photo_filter", "PC2b", QStringLiteral("レンズフィルター"), photo_filter_form, core::Json::object(), {}},
            {"channel_mixer", "PC2b", QStringLiteral("チャンネルミキサー"), channel_mixer_form, core::Json::object(), {}},
            {"selective_color", "PC2b", QStringLiteral("特定色域の選択"), selective_color_form, core::Json::object(), {}}};
}

void build_color_menu(QMenu* adjust, const MenuContext& context) {
    struct Command {
        const char* name;
        QString title;
        const char* kind;
        QList<QKeySequence> keys;
    };
    // (Photoshop's names, the layer panel's 色調補正のレイヤー's the same: the user's decision 8, command_title)
    const std::vector<Command> commands{
        {"act_adjust_vibrance", command_title("vibrance"), "vibrance", {}},
        {"act_adjust_hue_saturation", command_title("hue_saturation"), "hue_saturation", {QKeySequence(QStringLiteral("Ctrl+U"))}},
        {"act_adjust_color_balance", command_title("color_balance"), "color_balance", {QKeySequence(QStringLiteral("Ctrl+B"))}},
        {"act_adjust_black_white", command_title("black_white"), "black_white", {}},
        {"act_adjust_photo_filter", command_title("photo_filter"), "photo_filter", {}},
        {"act_adjust_channel_mixer", command_title("channel_mixer"), "channel_mixer", {}},
        {"act_adjust_selective_color", command_title("selective_color"), "selective_color", {}},
        {"act_adjust_desaturate", QStringLiteral("彩度を下げる"), "desaturate", {QKeySequence(QStringLiteral("Ctrl+Shift+U"))}},
    };
    if (!adjust->isEmpty()) adjust->addSeparator();
    for (const Command& each : commands) {
        MainWindow* window = context.window;
        const std::string kind = each.kind;
        QString title = each.title;
        title.remove(QStringLiteral("…"));
        adjust->addAction(context.make(QString::fromLatin1(each.name), each.title, [window, kind, title] { correct(window, kind, title); }, each.keys));
    }
}

}  // namespace genko::app::adjust_forms
