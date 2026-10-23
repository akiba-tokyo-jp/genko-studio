// PC4 節 D5 (SPEC.md COLOR-08「フィルター」, design 5.2): フィルター → Camera Raw フィルター…'s window and its panel
// (app/camera_raw_window.hpp). Its settings: core/filter_kinds (camera_raw); its formula: render/fx_camera_raw.cpp
// (PhotoCraft v0.2.0's camera_raw.rs; the tabs and their order are its ui-egui/camera_raw_ui.rs's). The words are Adobe
// Camera Raw's (日本語版).

#include "app/camera_raw_window.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSlider>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <vector>

#include "app/adjust_forms.hpp"
#include "app/ask.hpp"
#include "app/filter_gallery_window.hpp"
#include "app/layer_dialogs.hpp"
#include "app/lens_window.hpp"
#include "app/main_window.hpp"
#include "core/adjust.hpp"
#include "core/color_raster.hpp"
#include "core/pyops.hpp"
#include "render/fx.hpp"
#include "render/fx_camera_raw.hpp"
#include "render/image.hpp"
#include "render/segment/segment.hpp"
#include "service/wording.hpp"

namespace genko::app::camera_raw_window {

using core::Json;

// --- the picture -----------------------------------------------------------------------------------------------------------

bool well_formed(const Picture& p) {
    return p.width > 0 && p.height > 0 && p.rgba.size() == std::size_t(p.width) * std::size_t(p.height) * 4 && std::isfinite(p.s) && p.s > 0;
}

namespace {

// The box of the picture's pixels that are not clear (none: the whole of it).
render::fx::Rect frame_of(const Picture& p) {
    int x0 = p.width, y0 = p.height, x1 = 0, y1 = 0;
    for (int y = 0; y < p.height; ++y)
        for (int x = 0; x < p.width; ++x) {
            if (!(p.rgba[(std::size_t(y) * std::size_t(p.width) + std::size_t(x)) * 4 + 3] > 0)) continue;
            x0 = std::min(x0, x);
            y0 = std::min(y0, y);
            x1 = std::max(x1, x + 1);
            y1 = std::max(y1, y + 1);
        }
    if (x1 <= x0 || y1 <= y0) return render::fx::Rect{0, 0, p.width, p.height};
    return render::fx::Rect{x0, y0, x1, y1};
}

std::string bytes_of(const std::vector<float>& rgba) {
    std::string out(rgba.size(), '\0');
    for (std::size_t i = 0; i < rgba.size(); ++i)
        out[i] = static_cast<char>(std::lround(std::clamp(std::isfinite(rgba[i]) ? rgba[i] : 0.0f, 0.0f, 1.0f) * 255.0f));
    return out;
}

// The picture's values as the screen shows them (its own conversion, else as they are: sRGB's).
QImage screen_of(const Picture& p, std::vector<float> rgba) {
    if (p.to_screen) p.to_screen(rgba.data(), rgba.size() / 4);
    const std::string bytes = bytes_of(rgba);
    return QImage(reinterpret_cast<const uchar*>(bytes.data()), p.width, p.height, p.width * 4, QImage::Format_RGBA8888).copy();
}

}  // namespace

QImage shown(const Picture& picture) {
    if (!well_formed(picture)) return QImage();
    return screen_of(picture, picture.rgba);
}

QImage developed(const Picture& picture, const Json& settings) {
    const render::fx::Kernel* kernel = render::fx::kernel("camera_raw");
    if (kernel == nullptr || !well_formed(picture)) return QImage();
    const render::ImageAllocationBudget budget{1ULL << 30};
    const render::fx::Rect frame = frame_of(picture);
    // (the op's run on a part: the picture's place on its grid — 粒子 —, its frame what shows)
    const render::fx::PlaceScope place(picture.x0, picture.y0,
                                       render::fx::Rect{picture.x0 + frame.x0, picture.y0 + frame.y0, picture.x0 + frame.x1, picture.y0 + frame.y1});
    const render::fx::Rect all{0, 0, picture.width, picture.height};
    std::vector<float> out(picture.rgba.size());
    if (!picture.floats) {  // (worked as an 8-bit layer: its result within 0..1)
        const render::Image rgba = render::Image::frombytes("RGBA", render::Size{picture.width, picture.height}, bytes_of(picture.rgba));
        const auto surface = render::fx::eight_surface(rgba);
        render::fx::apply(*surface, *kernel, settings, core::FilterInks::Meaning, all, nullptr, picture.s);
        const std::string bytes = render::fx::take_image(*surface).tobytes();
        for (std::size_t i = 0; i < out.size(); ++i) out[i] = static_cast<unsigned char>(bytes[i]) / 255.0f;
        return screen_of(picture, std::move(out));
    }
    const std::string raster = core::encode_color_pixels(std::uint32_t(picture.width), std::uint32_t(picture.height), "f32", [&picture](std::size_t i) {
        return std::array<double, 4>{picture.rgba[i * 4], picture.rgba[i * 4 + 1], picture.rgba[i * 4 + 2], std::clamp(picture.rgba[i * 4 + 3], 0.0f, 1.0f)};
    });
    const auto surface = render::fx::precise_surface(raster);
    render::fx::apply(*surface, *kernel, settings, core::FilterInks::Meaning, all, nullptr, picture.s);
    const core::ColorRasterView view(render::fx::take_bytes(*surface));
    for (std::size_t i = 0; i < out.size() / 4; ++i) {
        const auto p = view.pixel(i);
        for (std::size_t c = 0; c < 4; ++c) out[i * 4 + c] = static_cast<float>(p[c]);
    }
    return screen_of(picture, std::move(out));
}

// --- the panel ---------------------------------------------------------------------------------------------------------------

namespace {

// One of Camera Raw's sliders: a slider and its number, its key's value in a form (Form::custom).
struct Slider {
    QSlider* slider = nullptr;
    QDoubleSpinBox* spin = nullptr;
};

// The tabs' words: Adobe Camera Raw's (日本語版).
QString words(const char* text) { return QString::fromUtf8(text); }

const std::array<const char*, 8> kBands{"レッド", "オレンジ", "イエロー", "グリーン", "アクア", "ブルー", "パープル", "マゼンタ"};

constexpr std::array<const char*, 4> kCurveKeys{"point_curve", "point_curve_red", "point_curve_green", "point_curve_blue"};
// (core/filter_kinds read_point_curve: at most 19 points)
constexpr std::size_t kMostCurvePoints = 19;

std::vector<std::array<double, 2>> straight() { return {{0, 0}, {255, 255}}; }

std::vector<std::array<double, 2>> curve_of(const Json& v) {
    std::vector<std::array<double, 2>> out;
    if (v.is_array())
        for (const Json& p : v)
            if (p.is_array() && p.size() == 2 && p[0].is_number() && p[1].is_number()) out.push_back({p[0].get<double>(), p[1].get<double>()});
    return out.size() < 2 ? straight() : out;
}

Json curve_json(const std::vector<std::array<double, 2>>& points) {
    bool linear = true;
    for (const auto& p : points) linear = linear && std::fabs(p[0] - p[1]) < 1e-3;
    Json out = Json::array();
    if (linear) return out;  // (the straight line: [], upstream's empty curve)
    for (const auto& p : points) out.push_back(Json::array({p[0], p[1]}));
    return out;
}

// A key's value in settings ("grade_shadows.hue": the wheel's hue), and its place to be set.
const Json* at_path(const Json& v, const std::string& path) {
    const std::size_t dot = path.find('.');
    const Json* here = core::get(v, dot == std::string::npos ? path : path.substr(0, dot));
    if (here == nullptr || dot == std::string::npos) return here;
    return core::get(*here, path.substr(dot + 1));
}

void put_at(Json& v, const std::string& path, const Json& value) {
    const std::size_t dot = path.find('.');
    if (dot == std::string::npos) {
        v[path] = value;
        return;
    }
    Json& group = v[path.substr(0, dot)];
    if (!group.is_object()) group = Json::object();
    group[path.substr(dot + 1)] = value;
}

}  // namespace

struct CameraRawPanel::Parts {
    std::vector<adjust_forms::Form*> forms;
    adjust_forms::Form* basic = nullptr;
    std::map<std::string, std::pair<adjust_forms::Form*, QWidget*>> rows;  // (each key's row: to hide it)
    lens_window::PictureView* view = nullptr;
    QLabel* status = nullptr;
    QCheckBox* before = nullptr;
    QTabWidget* tabs = nullptr;
    // トーンカーブ's point curves: RGB, レッド, グリーン, ブルー (shown one at a time)
    std::array<std::vector<std::array<double, 2>>, 4> curves;
    int curve = 0;
    CurveEditor* editor = nullptr;
    QComboBox* channel = nullptr;
    // the settings it started from (their normal form) and the keys the person touched since (D5's review P3-2)
    Json base;
    std::set<std::string> touched;
    std::shared_ptr<const Picture> picture;
    QImage original, result;
    QTimer* timer = nullptr;
    std::shared_ptr<std::stop_source> stop;
    int previews = 0;
    bool quiet = false;
};

CameraRawPanel::CameraRawPanel(QWidget* parent) : QWidget(parent), parts_(std::make_unique<Parts>()) {
    setObjectName(QStringLiteral("camera_raw_panel"));
    Parts& parts = *parts_;
    parts.base = core::normalize_filter("camera_raw", Json::object(), nullptr);
    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    // (the picture, its status and 補正前)
    auto* left = new QVBoxLayout;
    parts.view = new lens_window::PictureView(this);
    parts.view->setObjectName(QStringLiteral("camera_raw_view"));
    parts.view->setMinimumSize(480, 360);
    left->addWidget(parts.view, 1);
    auto* foot = new QHBoxLayout;
    parts.status = new QLabel(QStringLiteral("画像を読み込んでいます…"), this);
    parts.status->setObjectName(QStringLiteral("camera_raw_status"));
    parts.status->setWordWrap(true);
    foot->addWidget(parts.status, 1);
    parts.before = new QCheckBox(QStringLiteral("補正前（Y）"), this);
    parts.before->setObjectName(QStringLiteral("camera_raw_before"));
    parts.before->setToolTip(QStringLiteral("補正前の画像を表示します"));
    foot->addWidget(parts.before);
    left->addLayout(foot);
    outer->addLayout(left, 1);
    // (the tabs, 初期化)
    auto* right = new QVBoxLayout;
    parts.tabs = new QTabWidget(this);
    parts.tabs->setObjectName(QStringLiteral("camera_raw_tabs"));
    parts.tabs->setMinimumWidth(380);
    right->addWidget(parts.tabs, 1);
    auto* reset_button = new QPushButton(QStringLiteral("初期化"), this);
    reset_button->setObjectName(QStringLiteral("camera_raw_reset"));
    reset_button->setToolTip(QStringLiteral("すべての設定を初期設定に戻します"));
    right->addWidget(reset_button, 0, Qt::AlignRight);
    outer->addLayout(right);
    parts.timer = new QTimer(this);
    parts.timer->setSingleShot(true);
    parts.timer->setInterval(150);

    // (a key the person changed: its field's value from now on, the preview a moment later)
    const auto touched = [this](const std::string& key) {
        if (parts_->quiet) return;
        parts_->touched.insert(key);
        parts_->timer->start();
        if (changed) changed();
    };
    const auto tab = [&parts](const QString& title) {
        auto* scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        parts.tabs->addTab(scroll, title);
        auto* form = new adjust_forms::Form;
        scroll->setWidget(form);
        parts.forms.push_back(form);
        return form;
    };
    // A slider (Camera Raw's: lo..hi, `decimals` places) with its number for `key` (a path into a group: "grade_shadows.hue").
    const auto slider = [&parts, touched](adjust_forms::Form* form, const std::string& key, const QString& label, double lo, double hi, double fallback,
                                          int decimals = 0) {
        const double scale = std::pow(10.0, decimals);
        auto* row = new QWidget;
        auto* line = new QHBoxLayout(row);
        line->setContentsMargins(0, 0, 0, 0);
        Slider s;
        s.slider = new QSlider(Qt::Horizontal, row);
        s.slider->setRange(static_cast<int>(std::lround(lo * scale)), static_cast<int>(std::lround(hi * scale)));
        s.spin = new QDoubleSpinBox(row);
        std::string name = "camera_raw_" + key;
        std::replace(name.begin(), name.end(), '.', '_');
        s.spin->setObjectName(QString::fromStdString(name));
        s.spin->setRange(lo, hi);
        s.spin->setDecimals(decimals);
        s.spin->setSingleStep(decimals > 0 ? 1.0 / scale * (decimals >= 2 ? 5 : 1) : 1.0);
        s.spin->setMinimumWidth(72);
        line->addWidget(s.slider, 1);
        line->addWidget(s.spin);
        const auto set = [s, scale](double v) {
            const QSignalBlocker a(s.slider), b(s.spin);
            s.spin->setValue(v);
            s.slider->setValue(static_cast<int>(std::lround(v * scale)));
        };
        set(fallback);
        QObject::connect(s.slider, &QSlider::valueChanged, row, [s, scale, touched, key](int v) {
            const QSignalBlocker b(s.spin);
            s.spin->setValue(v / scale);
            touched(key);
        });
        QObject::connect(s.spin, &QDoubleSpinBox::valueChanged, row, [s, scale, touched, key](double v) {
            const QSignalBlocker a(s.slider);
            s.slider->setValue(static_cast<int>(std::lround(v * scale)));
            touched(key);
        });
        form->custom(
            key, label, row,
            [s, decimals] {
                const double v = s.spin->value();
                return decimals == 0 ? Json(static_cast<std::int64_t>(std::lround(v))) : Json(std::lround(v * std::pow(10.0, decimals)) / std::pow(10.0, decimals));
            },
            [set, fallback](const Json& v) { set(v.is_number() ? v.get<double>() : fallback); }, Json(fallback));
        parts.rows[key] = {form, row};
    };
    const auto heading = [](adjust_forms::Form* form, const QString& text) {
        auto* label = new QLabel(QStringLiteral("<b>%1</b>").arg(text));
        form->layout_form()->addRow(label);
    };

    // 基本補正
    {
        adjust_forms::Form* form = tab(QStringLiteral("基本補正"));
        parts.basic = form;
        heading(form, QStringLiteral("ホワイトバランス: 撮影時の設定"));
        slider(form, "temperature", words("色温度"), -100, 100, 0);
        slider(form, "tint", words("色かぶり補正"), -100, 100, 0);
        heading(form, QStringLiteral("トーン"));
        slider(form, "exposure", words("露光量"), -5, 5, 0, 2);
        slider(form, "contrast", words("コントラスト"), -100, 100, 0);
        slider(form, "highlights", words("ハイライト"), -100, 100, 0);
        slider(form, "shadows", words("シャドウ"), -100, 100, 0);
        slider(form, "whites", words("白レベル"), -100, 100, 0);
        slider(form, "blacks", words("黒レベル"), -100, 100, 0);
        heading(form, QStringLiteral("外観"));
        slider(form, "texture", words("テクスチャ"), -100, 100, 0);
        slider(form, "clarity", words("明瞭度"), -100, 100, 0);
        slider(form, "dehaze", words("かすみの除去"), -100, 100, 0);
        slider(form, "vibrance", words("自然な彩度"), -100, 100, 0);
        slider(form, "saturation", words("彩度"), -100, 100, 0);
    }
    // トーンカーブ: the parametric curve's regions (and their splits) and the point curve of each channel
    {
        adjust_forms::Form* form = tab(QStringLiteral("トーンカーブ"));
        heading(form, QStringLiteral("パラメトリックカーブ"));
        slider(form, "curve_highlights", words("ハイライト"), -100, 100, 0);
        slider(form, "curve_lights", words("ライト"), -100, 100, 0);
        slider(form, "curve_darks", words("ダーク"), -100, 100, 0);
        slider(form, "curve_shadows", words("シャドウ"), -100, 100, 0);
        auto* splits = new QWidget;
        auto* line = new QHBoxLayout(splits);
        line->setContentsMargins(0, 0, 0, 0);
        std::array<QDoubleSpinBox*, 3> boxes{};
        const std::array<double, 3> fallback{25, 50, 75};
        for (std::size_t i = 0; i < 3; ++i) {
            boxes[i] = new QDoubleSpinBox(splits);
            boxes[i]->setObjectName(QStringLiteral("camera_raw_curve_split_%1").arg(i));
            boxes[i]->setRange(5, 95);
            boxes[i]->setDecimals(0);
            boxes[i]->setValue(fallback[i]);
            line->addWidget(boxes[i]);
            QObject::connect(boxes[i], &QDoubleSpinBox::valueChanged, splits, [touched] { touched("curve_splits"); });
        }
        form->custom(
            "curve_splits", words("領域の区切り"), splits,
            [boxes] { return Json::array({std::lround(boxes[0]->value()), std::lround(boxes[1]->value()), std::lround(boxes[2]->value())}); },
            [boxes, fallback](const Json& v) {
                for (std::size_t i = 0; i < 3; ++i) {
                    const QSignalBlocker hold(boxes[i]);
                    boxes[i]->setValue(v.is_array() && v.size() == 3 && v[i].is_number() ? v[i].get<double>() : fallback[i]);
                }
            },
            Json::array({25, 50, 75}));
        parts.rows["curve_splits"] = {form, splits};
        heading(form, QStringLiteral("ポイントカーブ"));
        parts.channel = new QComboBox;
        parts.channel->setObjectName(QStringLiteral("camera_raw_curve_channel"));
        for (const char* name : {"RGB", "レッド", "グリーン", "ブルー"}) parts.channel->addItem(words(name));
        form->layout_form()->addRow(words("チャンネル"), parts.channel);
        parts.editor = new CurveEditor(straight());
        parts.editor->setObjectName(QStringLiteral("camera_raw_curve"));
        // (at most 19 points: one more is not added, and the window says so — D5's review P3-1)
        parts.editor->set_most_points(kMostCurvePoints);
        parts.editor->full = [this] { parts_->status->setText(QStringLiteral("ポイントカーブの点は %1 個までです").arg(kMostCurvePoints)); };
        form->layout_form()->addRow(parts.editor);
        for (auto& c : parts.curves) c = straight();
        QObject::connect(parts.channel, &QComboBox::currentIndexChanged, parts.editor, [this](int index) {
            Parts& p = *parts_;
            p.curve = std::clamp(index, 0, 3);
            const QSignalBlocker hold(p.editor);
            p.editor->set_points(p.curves[static_cast<std::size_t>(p.curve)]);
        });
        QObject::connect(parts.editor, &CurveEditor::changed, parts.editor, [this, touched] {
            Parts& p = *parts_;
            p.curves[static_cast<std::size_t>(p.curve)] = p.editor->points();
            touched(kCurveKeys[static_cast<std::size_t>(p.curve)]);
        });
    }
    // ディテール
    {
        adjust_forms::Form* form = tab(QStringLiteral("ディテール"));
        heading(form, QStringLiteral("シャープ"));
        slider(form, "sharpen_amount", words("適用量"), 0, 150, 0);
        slider(form, "sharpen_radius", words("半径"), 0.5, 3, 1, 1);
        slider(form, "sharpen_detail", words("ディテール"), 0, 100, 25);
        slider(form, "sharpen_masking", words("マスク"), 0, 100, 0);
        heading(form, QStringLiteral("ノイズ軽減"));
        slider(form, "noise_luminance", words("輝度"), 0, 100, 0);
        slider(form, "noise_luminance_detail", words("輝度のディテール"), 0, 100, 50);
        slider(form, "noise_color", words("カラー"), 0, 100, 0);
        slider(form, "noise_color_detail", words("カラーのディテール"), 0, 100, 50);
        form->note(QStringLiteral("半径は 200 dpi の作業の画素です（600 dpi のページでは 3 倍の画素に掛かります）"));
    }
    // カラーミキサー: 色相・彩度・輝度 of the eight colours
    {
        adjust_forms::Form* form = tab(QStringLiteral("カラーミキサー"));
        const std::array<std::pair<const char*, const char*>, 3> parts_of{{{"hsl_hue", "色相"}, {"hsl_sat", "彩度"}, {"hsl_lum", "輝度"}}};
        for (const auto& [key, title] : parts_of) {
            heading(form, words(title));
            // (the eight sliders of one key: a list of 8 numbers)
            auto* group = new QWidget;
            auto* rows = new QFormLayout(group);
            rows->setContentsMargins(0, 0, 0, 0);
            auto values = std::make_shared<std::array<QDoubleSpinBox*, 8>>();
            auto sliders = std::make_shared<std::array<QSlider*, 8>>();
            const std::string name(key);
            for (std::size_t i = 0; i < 8; ++i) {
                auto* row = new QWidget(group);
                auto* line = new QHBoxLayout(row);
                line->setContentsMargins(0, 0, 0, 0);
                auto* sl = new QSlider(Qt::Horizontal, row);
                sl->setRange(-100, 100);
                auto* spin = new QDoubleSpinBox(row);
                spin->setObjectName(QStringLiteral("camera_raw_%1_%2").arg(QString::fromStdString(name)).arg(i));
                spin->setRange(-100, 100);
                spin->setDecimals(0);
                spin->setMinimumWidth(72);
                line->addWidget(sl, 1);
                line->addWidget(spin);
                rows->addRow(words(kBands[i]), row);
                (*values)[i] = spin;
                (*sliders)[i] = sl;
                QObject::connect(sl, &QSlider::valueChanged, row, [spin, touched, name](int v) {
                    const QSignalBlocker b(spin);
                    spin->setValue(v);
                    touched(name);
                });
                QObject::connect(spin, &QDoubleSpinBox::valueChanged, row, [sl, touched, name](double v) {
                    const QSignalBlocker a(sl);
                    sl->setValue(static_cast<int>(std::lround(v)));
                    touched(name);
                });
            }
            form->custom(
                name, QString(), group,
                [values] {
                    Json out = Json::array();
                    for (QDoubleSpinBox* spin : *values) out.push_back(std::lround(spin->value()));
                    return out;
                },
                [values, sliders](const Json& v) {
                    for (std::size_t i = 0; i < 8; ++i) {
                        const double x = v.is_array() && v.size() == 8 && v[i].is_number() ? v[i].get<double>() : 0.0;
                        const QSignalBlocker a((*sliders)[i]), b((*values)[i]);
                        (*values)[i]->setValue(x);
                        (*sliders)[i]->setValue(static_cast<int>(std::lround(x)));
                    }
                },
                Json::array({0, 0, 0, 0, 0, 0, 0, 0}));
            parts.rows[name] = {form, group};
        }
    }
    // カラーグレーディング
    {
        adjust_forms::Form* form = tab(QStringLiteral("カラーグレーディング"));
        for (const auto& [key, title] : std::array<std::pair<const char*, const char*>, 4>{
                 {{"grade_shadows", "シャドウ"}, {"grade_midtones", "中間調"}, {"grade_highlights", "ハイライト"}, {"grade_global", "グローバル"}}}) {
            heading(form, words(title));
            slider(form, std::string(key) + ".hue", words("色相"), 0, 360, 0);
            slider(form, std::string(key) + ".sat", words("彩度"), 0, 100, 0);
            slider(form, std::string(key) + ".lum", words("輝度"), -100, 100, 0);
        }
        slider(form, "grade_blending", words("ブレンド"), 0, 100, 50);
        slider(form, "grade_balance", words("バランス"), -100, 100, 0);
    }
    // 効果
    {
        adjust_forms::Form* form = tab(QStringLiteral("効果"));
        heading(form, QStringLiteral("粒子"));
        slider(form, "grain_amount", words("適用量"), 0, 100, 0);
        slider(form, "grain_size", words("サイズ"), 0, 100, 25);
        slider(form, "grain_roughness", words("粗さ"), 0, 100, 50);
        heading(form, QStringLiteral("切り抜き後の周辺光量補正"));
        form->choice("vignette_style", words("スタイル"),
                     {{words("ハイライト優先"), Json("highlight_priority")}, {words("カラー優先"), Json("color_priority")}, {words("ペイントオーバーレイ"), Json("paint_overlay")}},
                     Json("highlight_priority"));
        if (auto* style = form->findChild<QComboBox*>(QStringLiteral("field_vignette_style"))) {
            QObject::connect(style, &QComboBox::currentIndexChanged, style, [touched] { touched("vignette_style"); });
            parts.rows["vignette_style"] = {form, style};
        }
        slider(form, "vignette_amount", words("適用量"), -100, 100, 0);
        slider(form, "vignette_midpoint", words("中心点"), 0, 100, 50);
        slider(form, "vignette_roundness", words("丸み"), -100, 100, 0);
        slider(form, "vignette_feather", words("ぼかし"), 0, 100, 50);
        slider(form, "vignette_highlights", words("ハイライト"), 0, 100, 0);
    }

    QObject::connect(reset_button, &QPushButton::clicked, this, [this] { reset(); });
    QObject::connect(parts.before, &QCheckBox::toggled, this, [this](bool on) {
        Parts& p = *parts_;
        if (p.picture) p.view->set_image(on || p.result.isNull() ? p.original : p.result, p.picture->box_mm);
    });
    auto* key = new QShortcut(QKeySequence(Qt::Key_Y), this);
    key->setContext(Qt::WidgetWithChildrenShortcut);
    QObject::connect(key, &QShortcut::activated, this, [this] { parts_->before->toggle(); });
    // (the result on a thread a moment after the last change; the last one asked for wins)
    QObject::connect(parts.timer, &QTimer::timeout, this, [this] {
        Parts& p = *parts_;
        if (p.stop) p.stop->request_stop();
        if (!p.picture) return;
        Json params;
        try {
            params = settings();
        } catch (const std::exception& error) {
            setProperty("previewing", false);
            p.status->setText(wording::error(QString::fromUtf8(error.what())));
            return;
        }
        auto stop = std::make_shared<std::stop_source>();
        p.stop = stop;
        setProperty("previewing", true);
        p.status->setText(QStringLiteral("プレビューを作っています…"));
        auto* watcher = new QFutureWatcher<QImage>(this);
        QObject::connect(watcher, &QFutureWatcher<QImage>::finished, this, [this, watcher, stop] {
            const QImage image = watcher->result();
            watcher->deleteLater();
            Parts& q = *parts_;
            if (q.stop != stop || stop->stop_requested()) return;
            setProperty("previewing", false);
            q.status->setText(image.isNull() ? QStringLiteral("この設定ではプレビューを作れませんでした") : QString());
            if (image.isNull()) return;
            q.result = image;
            ++q.previews;
            setProperty("previews", q.previews);
            if (!q.before->isChecked()) q.view->set_image(q.result, q.picture->box_mm);
        });
        watcher->setFuture(QtConcurrent::run([picture = p.picture, params, token = stop->get_token()]() -> QImage {
            const render::segment::StopScope scope(token);
            try {
                return developed(*picture, params);
            } catch (...) {  // (stopped, past the budget: none)
                return QImage();
            }
        }));
    });
    setProperty("previews", 0);
    setProperty("previewing", false);
}

CameraRawPanel::~CameraRawPanel() {
    if (parts_->stop) parts_->stop->request_stop();
}

void CameraRawPanel::set_picture(std::shared_ptr<const Picture> picture) {
    Parts& p = *parts_;
    if (picture && !well_formed(*picture)) {  // (its values not its size's, or no scale: none worked — D5's review P3-5)
        p.picture.reset();
        p.status->setText(QStringLiteral("画像の形が正しくないので、プレビューを作れません"));
        return;
    }
    p.picture = std::move(picture);
    if (!p.picture) {
        p.status->setText(QStringLiteral("画像を読み込んでいます…"));
        return;
    }
    p.original = shown(*p.picture);
    p.result = QImage();
    p.status->setText(QString());
    p.view->set_image(p.original, p.picture->box_mm);
    p.timer->start();
}

void CameraRawPanel::set_settings(const Json& settings) {
    Parts& p = *parts_;
    Json now = settings.is_object() ? settings : Json::object();
    try {
        p.base = core::normalize_filter("camera_raw", now, nullptr);
    } catch (const std::exception&) {  // (what the table refuses: the defaults, the seed kept)
        Json kept = Json::object();
        if (now.contains("seed") && now["seed"].is_number_integer()) kept["seed"] = now["seed"];
        p.base = core::normalize_filter("camera_raw", kept, nullptr);
    }
    p.touched.clear();
    p.quiet = true;
    for (adjust_forms::Form* form : p.forms) form->set_values(p.base);
    for (std::size_t i = 0; i < 4; ++i) p.curves[i] = curve_of(core::py_get(p.base, kCurveKeys[i], Json::array()));
    {
        const QSignalBlocker hold(p.editor);
        p.editor->set_points(p.curves[static_cast<std::size_t>(p.curve)]);
    }
    p.quiet = false;
    p.timer->start();
}

Json CameraRawPanel::settings() const {
    const Parts& p = *parts_;
    // (what it started from, each key the person touched as its field shows it)
    Json values = p.base;
    Json shown = Json::object();
    for (const adjust_forms::Form* form : p.forms) shown.update(form->values());
    for (const std::string& key : p.touched) {
        bool curve = false;
        for (std::size_t i = 0; i < 4; ++i) {
            if (key != kCurveKeys[i]) continue;
            values[key] = curve_json(p.curves[i]);
            curve = true;
        }
        if (curve) continue;
        if (const Json* field = at_path(shown, key)) put_at(values, key, *field);
    }
    return core::normalize_filter("camera_raw", values, nullptr);
}

void CameraRawPanel::reset() {
    Json kept = Json::object();
    kept["seed"] = core::py_get(parts_->base, "seed", Json(0));
    set_settings(kept);
    if (changed) changed();
}

void CameraRawPanel::add_to_basic(QWidget* widget) {
    if (widget == nullptr || parts_->basic == nullptr) return;
    parts_->basic->layout_form()->insertRow(0, widget);
}

void CameraRawPanel::set_hidden(const std::vector<std::string>& keys, bool hidden) {
    for (const std::string& key : keys) {
        const auto found = parts_->rows.find(key);
        if (found == parts_->rows.end()) continue;
        found->second.first->layout_form()->setRowVisible(found->second.second, !hidden);
    }
}

// --- the window ----------------------------------------------------------------------------------------------------------------

namespace {

// The layer's picture the window works: what shows of it (the frame) at a resolution that fits the window (at most its own
// grid's), made on a thread.
std::shared_ptr<const Picture> picture_of(const filter_gallery::Source& source) {
    const std::array<double, 4> box = filter_gallery::shown_box(source);
    const double long_mm = std::max({box[2] - box[0], box[3] - box[1], 1.0});
    const int dpi = static_cast<int>(std::clamp(std::floor(900.0 / long_mm * 25.4), 4.0, std::max(4.0, std::min(400.0, source.dpi))));
    const render::Box area{static_cast<int>(std::floor(box[0] / 25.4 * dpi)), static_cast<int>(std::floor(box[1] / 25.4 * dpi)),
                           static_cast<int>(std::ceil(box[2] / 25.4 * dpi)), static_cast<int>(std::ceil(box[3] / 25.4 * dpi))};
    auto out = std::make_shared<Picture>();
    render::Image image;
    try {
        image = source.picture(dpi, area).convert("RGBA");
    } catch (const std::exception&) {
        image = render::Image::create("RGBA", render::Size{std::max(1, area.width()), std::max(1, area.height())}, render::Ink{0, 0, 0, 0});
    }
    out->width = image.width();
    out->height = image.height();
    const std::string bytes = image.tobytes();
    out->rgba.resize(bytes.size());
    for (std::size_t i = 0; i < bytes.size(); ++i) out->rgba[i] = static_cast<unsigned char>(bytes[i]) / 255.0f;
    out->s = dpi / 200.0;
    out->x0 = area.x0;
    out->y0 = area.y0;
    out->box_mm = {area.x0 * 25.4 / dpi, area.y0 * 25.4 / dpi, (area.x0 + out->width) * 25.4 / dpi, (area.y0 + out->height) * 25.4 / dpi};
    return out;
}

class CameraRawDialog : public QDialog {
public:
    CameraRawDialog(QWidget* parent, const FilterWindowCall& call) : QDialog(parent) {
        setObjectName(QStringLiteral("filter_dialog"));
        setProperty("kind", QStringLiteral("camera_raw"));
        setWindowTitle(QStringLiteral("Camera Raw フィルター"));
        resize(1180, 760);
        auto* layout = new QVBoxLayout(this);
        panel_ = new CameraRawPanel(this);
        layout->addWidget(panel_, 1);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("OK"));
        buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("キャンセル"));
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        layout->addWidget(buttons);
        // (from the command: the command's seed — a new grain each time —, its preview's the same; a smart filter set again:
        // its own)
        Json now = call.now.is_object() ? call.now : Json::object();
        command_seed_ = call.filter < 0 && call.seed.has_value() && !now.contains("seed");
        if (command_seed_) now["seed"] = *call.seed;
        panel_->set_settings(now);
        // (the picture made on a thread: the window answers meanwhile)
        const filter_gallery::Source source = filter_gallery::source_of(*call.window, *call.page, *call.layer, call.filter);
        auto* watcher = new QFutureWatcher<std::shared_ptr<const Picture>>(this);
        connect(watcher, &QFutureWatcher<std::shared_ptr<const Picture>>::finished, this, [this, watcher] {
            panel_->set_picture(watcher->result());
            watcher->deleteLater();
        });
        watcher->setFuture(QtConcurrent::run([source]() { return picture_of(source); }));
    }
    // The settings chosen; from the command without its seed (the command gives its own, the same: 前回のフィルター then
    // a new one).
    Json settings() const {
        Json chosen = panel_->settings();
        if (command_seed_) chosen.erase("seed");
        return chosen;
    }

protected:
    // (OK with settings the table refuses: said, the window kept with them — D5's review P3-1)
    void accept() override {
        try {
            (void)panel_->settings();
        } catch (const std::exception& error) {
            ask::warning(this, windowTitle(), wording::error(QString::fromUtf8(error.what())));
            return;
        }
        QDialog::accept();
    }

private:
    CameraRawPanel* panel_ = nullptr;
    bool command_seed_ = false;
};

}  // namespace

void open(const FilterWindowCall& call) {
    if (call.window == nullptr || call.page == nullptr || call.layer == nullptr) return;
    CameraRawDialog dialog(call.window, call);
    if (ask::exec(&dialog) != QDialog::Accepted) return;
    Json chosen;
    try {
        chosen = dialog.settings();
    } catch (const std::exception& error) {  // (said, nothing sent)
        ask::warning(call.window, dialog.windowTitle(), wording::error(QString::fromUtf8(error.what())));
        return;
    }
    if (call.send) call.send(chosen);
}

}  // namespace genko::app::camera_raw_window
