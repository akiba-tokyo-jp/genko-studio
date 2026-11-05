// PC4 E2 (window.panel.brushSettings, F5; SPEC.md COLOR-09「ブラシ設定パネル」): the Brush Settings panel — the brush in
// hand's dab settings in Photoshop's sections, a line drawn with them, and a brush of Genko's own way of drawing made a
// dab brush.

#include "app/brush_settings_panel.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>

#include "app/ask.hpp"
#include "app/brush_panel.hpp"
#include "app/config.hpp"
#include "app/fields.hpp"
#include "app/main_window.hpp"
#include "app/off_ui.hpp"
#include "app/patterns_panel.hpp"
#include "app/theme.hpp"
#include "core/brushes.hpp"
#include "core/error.hpp"
#include "core/paths.hpp"
#include "render/brush_presets.hpp"
#include "render/brushes.hpp"
#include "render/dabs/lines.hpp"
#include "render/patterns.hpp"
#include "render/png.hpp"
#include "service/wording.hpp"
#include "storage/fsutil.hpp"

namespace genko::app {

using core::Json;
namespace presets = render::brush_presets;

namespace {

const std::vector<std::pair<QString, core::DabControl>>& controls() {
    using C = core::DabControl;
    static const std::vector<std::pair<QString, C>> c{{QStringLiteral("オフ"), C::Off},
                                                      {QStringLiteral("フェード"), C::Fade},
                                                      {QStringLiteral("筆圧"), C::Pressure},
                                                      {QStringLiteral("ペンの傾き"), C::Tilt},
                                                      {QStringLiteral("スタイラスホイール"), C::Wheel},
                                                      {QStringLiteral("回転"), C::Rotation},
                                                      {QStringLiteral("初期方向"), C::InitialDirection},
                                                      {QStringLiteral("方向"), C::Direction}};
    return c;
}

const std::vector<std::pair<QString, core::DabMaskMode>>& mask_modes() {
    using M = core::DabMaskMode;
    static const std::vector<std::pair<QString, M>> m{{QStringLiteral("乗算"), M::Multiply},          {QStringLiteral("減算"), M::Subtract},
                                                      {QStringLiteral("比較（暗）"), M::Darken},      {QStringLiteral("オーバーレイ"), M::Overlay},
                                                      {QStringLiteral("覆い焼きカラー"), M::ColorDodge}, {QStringLiteral("焼き込みカラー"), M::ColorBurn},
                                                      {QStringLiteral("焼き込み（リニア）"), M::LinearBurn}, {QStringLiteral("ハードミックス"), M::HardMix},
                                                      {QStringLiteral("リニアの高さ"), M::LinearHeight}, {QStringLiteral("高さ"), M::Height}};
    return m;
}

const std::vector<std::pair<QString, QString>>& procedurals() {
    static const std::vector<std::pair<QString, QString>> p{{QStringLiteral("紙"), QStringLiteral("paper")},
                                                            {QStringLiteral("キャンバス"), QStringLiteral("canvas")},
                                                            {QStringLiteral("ノイズ"), QStringLiteral("noise")},
                                                            {QStringLiteral("点"), QStringLiteral("dots")}};
    return p;
}

QLabel* hint(const QString& text) {
    auto* label = new QLabel(text);
    label->setWordWrap(true);
    theme::hint(label);
    return label;
}

}  // namespace

BrushSettingsPanel::BrushSettingsPanel(MainWindow* window) : window_(window) {
    setObjectName(QStringLiteral("brush_settings_panel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    about = new QLabel;
    about->setObjectName(QStringLiteral("bs_about"));
    about->setWordWrap(true);
    layout->addWidget(about);
    pages = new QStackedWidget;
    auto* scroll = new QScrollArea;  // (the dock first, then the panel in the scroll area: P0's order)
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(scroll, 1);
    auto* dab_page = new QWidget;
    dab_page->setObjectName(QStringLiteral("bs_dab_page"));
    build_dab_page(dab_page);
    pages->addWidget(dab_page);
    auto* genko_page = new QWidget;
    genko_page->setObjectName(QStringLiteral("bs_genko_page"));
    auto* genko = new QVBoxLayout(genko_page);
    genko->setContentsMargins(0, 0, 0, 0);
    genko->addWidget(hint(QStringLiteral("このブラシは Genko の描き方のブラシです（先端・模様・質感・紙質は「ブラシの詳細」で）。"
                                         "Photoshop のブラシ設定（シェイプ・散布・テクスチャなど）で直すには、ダブのブラシにします。")));
    details = new QPushButton(QStringLiteral("ブラシの詳細…"));
    details->setObjectName(QStringLiteral("bs_details"));
    details->setToolTip(QStringLiteral("先端・模様・質感・紙質などを開きます（入っているブラシは複製して自分のブラシに）"));
    connect(details, &QPushButton::clicked, this, [this] {
        if (presets::is_own(key_)) {
            window_->edit_brush_settings();
        } else {
            window_->make_brush_settings();
        }
    });
    to_dabs = new QPushButton(QStringLiteral("Photoshop 型（ダブ）にする"));
    to_dabs->setObjectName(QStringLiteral("bs_to_dabs"));
    to_dabs->setToolTip(QStringLiteral("このブラシの大きさ・硬さで、Photoshop のブラシ設定で直せる自分のブラシ（ダブのブラシ）を作ります"));
    connect(to_dabs, &QPushButton::clicked, this, [this] { make_dab_copy(); });
    genko->addWidget(details);
    genko->addWidget(to_dabs);
    genko->addStretch(1);
    pages->addWidget(genko_page);
    scroll->setWidget(pages);
    sample = new LineSample([this] {
        Json f{{"kind", key_.empty() ? std::string(core::kDefaultBrush) : key_}, {"width_mm", 2.0}, {"taper", false}};
        if (window_->brush_panel() != nullptr) {
            const auto rgb = window_->brush_panel()->rgb();
            f["rgb"] = Json::array({rgb[0], rgb[1], rgb[2]});
            f["width_mm"] = std::clamp(window_->brush_panel()->size->value(), 0.3, 6.0);
        }
        return f;
    });
    sample->setObjectName(QStringLiteral("bs_sample"));
    layout->addWidget(sample);
    keep_timer_.setSingleShot(true);
    keep_timer_.setInterval(500);
    connect(&keep_timer_, &QTimer::timeout, this, [this] { keep_now(); });
}

void BrushSettingsPanel::build_dab_page(QWidget* page) {
    auto* outer = new QVBoxLayout(page);
    outer->setContentsMargins(0, 0, 0, 0);
    QFormLayout* form = nullptr;
    const auto section = [&](const QString& title, const char* check, bool core::DabSettings::* /*unused*/ = nullptr) -> QCheckBox* {
        auto* box = new QGroupBox(title);
        box->setObjectName(QStringLiteral("bs_section_") + QString::fromLatin1(check != nullptr ? check : "tip"));
        form = new QFormLayout(box);
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        outer->addWidget(box);
        if (check == nullptr) return nullptr;
        auto* on = new QCheckBox(QStringLiteral("使う"));
        on->setObjectName(QStringLiteral("bs_") + QString::fromLatin1(check));
        connect(on, &QCheckBox::toggled, this, [this] { changed(); });
        form->addRow(on);
        return on;
    };
    // the field makers: each registers how it is read into the settings and set from them
    const auto number = [&](const char* name, const QString& label, double lo, double hi, double step, int decimals, const QString& suffix,
                            std::function<double(const core::DabSettings&)> get, std::function<void(core::DabSettings&, double)> set) {
        auto* box = new QDoubleSpinBox;
        box->setObjectName(QStringLiteral("bs_") + QString::fromLatin1(name));
        box->setRange(lo, hi);
        box->setSingleStep(step);
        box->setDecimals(decimals);
        box->setSuffix(suffix);
        connect(box, &QDoubleSpinBox::valueChanged, this, [this] { changed(); });
        form->addRow(label, box);
        readers_[box->objectName()] = [box, set](core::DabSettings& s) { set(s, box->value()); };
        writers_[box->objectName()] = [box, get](const core::DabSettings& s) { box->setValue(get(s)); };
        return box;
    };
    const auto percent = [&](const char* name, const QString& label, double lo, double hi, std::function<double(const core::DabSettings&)> get,
                             std::function<void(core::DabSettings&, double)> set) {
        return number(name, label, lo, hi, 1, 0, QStringLiteral(" %"), [get](const core::DabSettings& s) { return get(s) * 100.0; },
                      [set](core::DabSettings& s, double v) { set(s, v / 100.0); });
    };
    const auto flag = [&](const char* name, const QString& label, std::function<bool(const core::DabSettings&)> get, std::function<void(core::DabSettings&, bool)> set) {
        auto* box = new QCheckBox(label);
        box->setObjectName(QStringLiteral("bs_") + QString::fromLatin1(name));
        connect(box, &QCheckBox::toggled, this, [this] { changed(); });
        form->addRow(box);
        readers_[box->objectName()] = [box, set](core::DabSettings& s) { set(s, box->isChecked()); };
        writers_[box->objectName()] = [box, get](const core::DabSettings& s) { box->setChecked(get(s)); };
        return box;
    };
    const auto control = [&](const char* name, const QString& label, std::function<core::DabDynamic&(core::DabSettings&)> dyn) {
        auto* box = new QComboBox;
        box->setObjectName(QStringLiteral("bs_") + QString::fromLatin1(name));
        for (const auto& [text, c] : controls()) box->addItem(text, static_cast<int>(c));
        connect(box, &QComboBox::currentIndexChanged, this, [this] { changed(); });
        form->addRow(label, box);
        readers_[box->objectName()] = [box, dyn](core::DabSettings& s) { dyn(s).control = static_cast<core::DabControl>(box->currentData().toInt()); };
        writers_[box->objectName()] = [box, dyn](const core::DabSettings& s) {
            core::DabSettings copy = s;
            box->setCurrentIndex(std::max(0, box->findData(static_cast<int>(dyn(copy).control))));
        };
        return box;
    };
    const auto mode = [&](const char* name, const QString& label, std::function<core::DabMaskMode&(core::DabSettings&)> m) {
        auto* box = new QComboBox;
        box->setObjectName(QStringLiteral("bs_") + QString::fromLatin1(name));
        for (const auto& [text, v] : mask_modes()) box->addItem(text, static_cast<int>(v));
        connect(box, &QComboBox::currentIndexChanged, this, [this] { changed(); });
        form->addRow(label, box);
        readers_[box->objectName()] = [box, m](core::DabSettings& s) { m(s) = static_cast<core::DabMaskMode>(box->currentData().toInt()); };
        writers_[box->objectName()] = [box, m](const core::DabSettings& s) {
            core::DabSettings copy = s;
            box->setCurrentIndex(std::max(0, box->findData(static_cast<int>(m(copy)))));
        };
        return box;
    };
    using S = core::DabSettings;
    // ブラシ先端のシェイプ
    section(QStringLiteral("ブラシ先端のシェイプ"), nullptr);
    {
        auto* tip_row = new QWidget;
        auto* row = new QHBoxLayout(tip_row);
        row->setContentsMargins(0, 0, 0, 0);
        auto* tip_label = new QLabel;
        tip_label->setObjectName(QStringLiteral("bs_tip"));
        auto* pick = new QPushButton(QStringLiteral("画像…"));
        pick->setObjectName(QStringLiteral("bs_tip_picture"));
        pick->setToolTip(QStringLiteral("画像ファイルを先端にします（白い紙に暗い描画、または透明に描いたもの）"));
        connect(pick, &QPushButton::clicked, this, [this] { pick_tip(); });
        auto* round = new QPushButton(QStringLiteral("円"));
        round->setObjectName(QStringLiteral("bs_tip_round"));
        round->setToolTip(QStringLiteral("先端を円にします"));
        connect(round, &QPushButton::clicked, this, [this] {
            tip_ = std::string();
            changed();
        });
        row->addWidget(tip_label, 1);
        row->addWidget(pick);
        row->addWidget(round);
        form->addRow(QStringLiteral("先端"), tip_row);
        writers_[QStringLiteral("bs_tip")] = [this, tip_label](const S&) { tip_label->setText(tip_.empty() ? QStringLiteral("円") : QStringLiteral("サンプル")); };
        readers_[QStringLiteral("bs_tip")] = [this](S& s) { s.tip = tip_; };
    }
    number("size", QStringLiteral("直径"), 0.01, 500, 0.1, 2, QStringLiteral(" mm"), [](const S& s) { return s.size_mm; }, [](S& s, double v) { s.size_mm = v; });
    number("angle", QStringLiteral("角度"), -360, 360, 1, 0, QStringLiteral("°"), [](const S& s) { return s.angle; }, [](S& s, double v) { s.angle = v; });
    percent("roundness", QStringLiteral("真円率"), 1, 100, [](const S& s) { return s.roundness; }, [](S& s, double v) { s.roundness = v; });
    percent("hardness", QStringLiteral("硬さ"), 0, 100, [](const S& s) { return s.hardness; }, [](S& s, double v) { s.hardness = v; });
    percent("spacing", QStringLiteral("間隔"), 1, 1000, [](const S& s) { return s.spacing; }, [](S& s, double v) { s.spacing = v; });
    flag("flip_x", QStringLiteral("X 軸で反転"), [](const S& s) { return s.flip_x; }, [](S& s, bool v) { s.flip_x = v; });
    flag("flip_y", QStringLiteral("Y 軸で反転"), [](const S& s) { return s.flip_y; }, [](S& s, bool v) { s.flip_y = v; });
    // シェイプ
    QCheckBox* on = section(QStringLiteral("シェイプ"), "shape");
    readers_[on->objectName()] = [on](S& s) { s.shape.enabled = on->isChecked(); };
    writers_[on->objectName()] = [on](const S& s) { on->setChecked(s.shape.enabled); };
    percent("size_jitter", QStringLiteral("サイズのジッター"), 0, 100, [](const S& s) { return s.shape.size.jitter; }, [](S& s, double v) { s.shape.size.jitter = v; });
    control("size_control", QStringLiteral("コントロール"), [](S& s) -> core::DabDynamic& { return s.shape.size; });
    percent("size_minimum", QStringLiteral("最小の直径"), 0, 100, [](const S& s) { return s.shape.size.minimum; }, [](S& s, double v) { s.shape.size.minimum = v; });
    percent("angle_jitter", QStringLiteral("角度のジッター"), 0, 100, [](const S& s) { return s.shape.angle.jitter; }, [](S& s, double v) { s.shape.angle.jitter = v; });
    control("angle_control", QStringLiteral("コントロール"), [](S& s) -> core::DabDynamic& { return s.shape.angle; });
    percent("roundness_jitter", QStringLiteral("真円率のジッター"), 0, 100, [](const S& s) { return s.shape.roundness.jitter; },
            [](S& s, double v) { s.shape.roundness.jitter = v; });
    control("roundness_control", QStringLiteral("コントロール"), [](S& s) -> core::DabDynamic& { return s.shape.roundness; });
    percent("roundness_minimum", QStringLiteral("最小の真円率"), 0, 100, [](const S& s) { return s.shape.roundness.minimum; },
            [](S& s, double v) { s.shape.roundness.minimum = v; });
    flag("flip_x_jitter", QStringLiteral("X 軸の反転のジッター"), [](const S& s) { return s.shape.flip_x_jitter; }, [](S& s, bool v) { s.shape.flip_x_jitter = v; });
    flag("flip_y_jitter", QStringLiteral("Y 軸の反転のジッター"), [](const S& s) { return s.shape.flip_y_jitter; }, [](S& s, bool v) { s.shape.flip_y_jitter = v; });
    // 散布
    on = section(QStringLiteral("散布"), "scatter");
    readers_[on->objectName()] = [on](S& s) { s.scatter.enabled = on->isChecked(); };
    writers_[on->objectName()] = [on](const S& s) { on->setChecked(s.scatter.enabled); };
    percent("scatter_amount", QStringLiteral("散布"), 0, 1000, [](const S& s) { return s.scatter.scatter.jitter; }, [](S& s, double v) { s.scatter.scatter.jitter = v; });
    flag("scatter_both", QStringLiteral("両軸"), [](const S& s) { return s.scatter.both_axes; }, [](S& s, bool v) { s.scatter.both_axes = v; });
    control("scatter_control", QStringLiteral("コントロール"), [](S& s) -> core::DabDynamic& { return s.scatter.scatter; });
    number("count", QStringLiteral("数"), 1, 16, 1, 0, QString(), [](const S& s) { return static_cast<double>(s.scatter.count); },
           [](S& s, double v) { s.scatter.count = static_cast<std::int64_t>(std::lround(v)); });
    percent("count_jitter", QStringLiteral("数のジッター"), 0, 100, [](const S& s) { return s.scatter.count_jitter.jitter; },
            [](S& s, double v) { s.scatter.count_jitter.jitter = v; });
    // テクスチャ
    on = section(QStringLiteral("テクスチャ"), "texture");
    readers_[on->objectName()] = [on](S& s) { s.texture.enabled = on->isChecked(); };
    writers_[on->objectName()] = [on](const S& s) { on->setChecked(s.texture.enabled); };
    {
        auto* pattern_row = new QWidget;
        auto* row = new QHBoxLayout(pattern_row);
        row->setContentsMargins(0, 0, 0, 0);
        auto* kind = new QComboBox;
        kind->setObjectName(QStringLiteral("bs_texture_pattern"));
        for (const auto& [text, style] : procedurals()) kind->addItem(text, style);
        kind->addItem(QStringLiteral("パターンから"), QStringLiteral("asset"));
        auto* from = new QPushButton(QStringLiteral("パターン…"));
        from->setObjectName(QStringLiteral("bs_texture_from"));
        from->setToolTip(QStringLiteral("パターンのライブラリか原稿のパターンの明るさを、テクスチャにします"));
        connect(kind, &QComboBox::currentIndexChanged, this, [this] { changed(); });
        connect(from, &QPushButton::clicked, this, [this, kind] {
            QDialog pick(this);
            pick.setObjectName(QStringLiteral("bs_texture_dialog"));
            pick.setWindowTitle(QStringLiteral("テクスチャのパターン"));
            auto* l = new QVBoxLayout(&pick);
            QComboBox* choose = pattern_combo(window_->book(), window_->current_pattern());
            l->addWidget(choose);
            auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
            connect(buttons, &QDialogButtonBox::accepted, &pick, &QDialog::accept);
            connect(buttons, &QDialogButtonBox::rejected, &pick, &QDialog::reject);
            l->addWidget(buttons);
            if (ask::exec(&pick) != QDialog::Accepted) return;
            const auto chosen = pattern_chosen(window_->book(), choose->currentData().toString());
            if (!chosen) return;
            try {
                // (its lightness, at most 1024 pixels a side: a texture's — made off the window's thread, review P3-17)
                const auto pattern = chosen->first;
                const auto bytes = chosen->second;
                const auto made = off_ui([pattern, bytes] {
                    const std::string grey = render::patterns::grey(pattern, *bytes);
                    render::Image image = render::Image::frombytes("L", render::Size{static_cast<int>(pattern.width), static_cast<int>(pattern.height)}, grey);
                    if (image.width() > static_cast<int>(core::kMaxPatternEdge) || image.height() > static_cast<int>(core::kMaxPatternEdge)) {
                        image.thumbnail(render::Size{static_cast<int>(core::kMaxPatternEdge), static_cast<int>(core::kMaxPatternEdge)});
                    }
                    return render::dabs::take_in_picture(render::write_png(image, 6), false, core::kMaxPatternEdge);
                });
                render::dabs::keep_picture(made.first, made.second);
                texture_asset_ = made.first;
                kind->setCurrentIndex(kind->findData(QStringLiteral("asset")));
                changed();
            } catch (const std::exception& error) {
                window_->flash(wording::error(QString::fromUtf8(error.what())), 4000, true);
            }
        });
        row->addWidget(kind, 1);
        row->addWidget(from);
        form->addRow(QStringLiteral("パターン"), pattern_row);
        readers_[kind->objectName()] = [this, kind](S& s) {
            const QString style = kind->currentData().toString();
            if (style == QLatin1String("asset") && !texture_asset_.empty()) {
                s.texture.pattern.asset = texture_asset_;
            } else {
                s.texture.pattern.asset.clear();
                s.texture.pattern.procedural = (style == QLatin1String("asset") ? QStringLiteral("paper") : style).toStdString();
            }
        };
        writers_[kind->objectName()] = [this, kind](const S& s) {
            texture_asset_ = s.texture.pattern.asset;
            kind->setCurrentIndex(std::max(0, kind->findData(s.texture.pattern.asset.empty() ? QString::fromStdString(s.texture.pattern.procedural) : QStringLiteral("asset"))));
        };
    }
    flag("texture_invert", QStringLiteral("反転"), [](const S& s) { return s.texture.invert; }, [](S& s, bool v) { s.texture.invert = v; });
    percent("texture_scale", QStringLiteral("比率"), 1, 1000, [](const S& s) { return s.texture.scale; }, [](S& s, double v) { s.texture.scale = v; });
    number("texture_brightness", QStringLiteral("明るさ"), -150, 150, 1, 0, QString(), [](const S& s) { return s.texture.brightness * 150.0; },
           [](S& s, double v) { s.texture.brightness = v / 150.0; });
    number("texture_contrast", QStringLiteral("コントラスト"), -50, 50, 1, 0, QString(), [](const S& s) { return s.texture.contrast * 50.0; },
           [](S& s, double v) { s.texture.contrast = v / 50.0; });
    flag("texture_each_tip", QStringLiteral("各描点にテクスチャを適用"), [](const S& s) { return s.texture.each_tip; }, [](S& s, bool v) { s.texture.each_tip = v; });
    mode("texture_mode", QStringLiteral("描画モード"), [](S& s) -> core::DabMaskMode& { return s.texture.mode; });
    percent("texture_depth", QStringLiteral("深さ"), 0, 100, [](const S& s) { return s.texture.depth; }, [](S& s, double v) { s.texture.depth = v; });
    percent("texture_depth_jitter", QStringLiteral("深さのジッター"), 0, 100, [](const S& s) { return s.texture.depth_jitter.jitter; },
            [](S& s, double v) { s.texture.depth_jitter.jitter = v; });
    // デュアルブラシ
    on = section(QStringLiteral("デュアルブラシ"), "dual");
    readers_[on->objectName()] = [on](S& s) { s.dual.enabled = on->isChecked(); };
    writers_[on->objectName()] = [on](const S& s) { on->setChecked(s.dual.enabled); };
    mode("dual_mode", QStringLiteral("モード"), [](S& s) -> core::DabMaskMode& { return s.dual.mode; });
    number("dual_size", QStringLiteral("直径"), 0.01, 500, 0.1, 2, QStringLiteral(" mm"), [](const S& s) { return s.dual.size_mm; }, [](S& s, double v) { s.dual.size_mm = v; });
    percent("dual_spacing", QStringLiteral("間隔"), 1, 1000, [](const S& s) { return s.dual.spacing; }, [](S& s, double v) { s.dual.spacing = v; });
    percent("dual_scatter", QStringLiteral("散布"), 0, 1000, [](const S& s) { return s.dual.scatter; }, [](S& s, double v) { s.dual.scatter = v; });
    flag("dual_both", QStringLiteral("両軸"), [](const S& s) { return s.dual.both_axes; }, [](S& s, bool v) { s.dual.both_axes = v; });
    number("dual_count", QStringLiteral("数"), 1, 16, 1, 0, QString(), [](const S& s) { return static_cast<double>(s.dual.count); },
           [](S& s, double v) { s.dual.count = static_cast<std::int64_t>(std::lround(v)); });
    flag("dual_flip", QStringLiteral("反転"), [](const S& s) { return s.dual.flip; }, [](S& s, bool v) { s.dual.flip = v; });
    // カラー
    on = section(QStringLiteral("カラー"), "color");
    readers_[on->objectName()] = [on](S& s) { s.color.enabled = on->isChecked(); };
    writers_[on->objectName()] = [on](const S& s) { on->setChecked(s.color.enabled); };
    flag("color_per_tip", QStringLiteral("描点ごとに適用"), [](const S& s) { return s.color.per_tip; }, [](S& s, bool v) { s.color.per_tip = v; });
    percent("color_fg_bg", QStringLiteral("描画色・背景色のジッター"), 0, 100, [](const S& s) { return s.color.fg_bg.jitter; }, [](S& s, double v) { s.color.fg_bg.jitter = v; });
    control("color_fg_bg_control", QStringLiteral("コントロール"), [](S& s) -> core::DabDynamic& { return s.color.fg_bg; });
    percent("color_hue", QStringLiteral("色相のジッター"), 0, 100, [](const S& s) { return s.color.hue_jitter; }, [](S& s, double v) { s.color.hue_jitter = v; });
    percent("color_saturation", QStringLiteral("彩度のジッター"), 0, 100, [](const S& s) { return s.color.saturation_jitter; },
            [](S& s, double v) { s.color.saturation_jitter = v; });
    percent("color_brightness", QStringLiteral("明るさのジッター"), 0, 100, [](const S& s) { return s.color.brightness_jitter; },
            [](S& s, double v) { s.color.brightness_jitter = v; });
    percent("color_purity", QStringLiteral("純度"), -100, 100, [](const S& s) { return s.color.purity; }, [](S& s, double v) { s.color.purity = v; });
    // 伝達
    on = section(QStringLiteral("伝達"), "transfer");
    readers_[on->objectName()] = [on](S& s) { s.transfer.enabled = on->isChecked(); };
    writers_[on->objectName()] = [on](const S& s) { on->setChecked(s.transfer.enabled); };
    percent("opacity_jitter", QStringLiteral("不透明度のジッター"), 0, 100, [](const S& s) { return s.transfer.opacity.jitter; },
            [](S& s, double v) { s.transfer.opacity.jitter = v; });
    control("opacity_control", QStringLiteral("コントロール"), [](S& s) -> core::DabDynamic& { return s.transfer.opacity; });
    percent("flow_jitter", QStringLiteral("流量のジッター"), 0, 100, [](const S& s) { return s.transfer.flow.jitter; }, [](S& s, double v) { s.transfer.flow.jitter = v; });
    control("flow_control", QStringLiteral("コントロール"), [](S& s) -> core::DabDynamic& { return s.transfer.flow; });
    // ブラシポーズ
    on = section(QStringLiteral("ブラシポーズ"), "pose");
    readers_[on->objectName()] = [on](S& s) { s.pose.enabled = on->isChecked(); };
    writers_[on->objectName()] = [on](const S& s) { on->setChecked(s.pose.enabled); };
    number("tilt_x", QStringLiteral("傾き X"), -90, 90, 1, 0, QStringLiteral("°"), [](const S& s) { return s.pose.tilt_x; }, [](S& s, double v) { s.pose.tilt_x = v; });
    number("tilt_y", QStringLiteral("傾き Y"), -90, 90, 1, 0, QStringLiteral("°"), [](const S& s) { return s.pose.tilt_y; }, [](S& s, double v) { s.pose.tilt_y = v; });
    number("pose_rotation", QStringLiteral("回転"), 0, 359, 1, 0, QStringLiteral("°"), [](const S& s) { return std::min(359.0, s.pose.rotation); },
           [](S& s, double v) { s.pose.rotation = v; });
    percent("pose_pressure", QStringLiteral("筆圧"), 0, 100, [](const S& s) { return s.pose.pressure; }, [](S& s, double v) { s.pose.pressure = v; });
    flag("override_tilt", QStringLiteral("傾きを上書き"), [](const S& s) { return s.pose.override_tilt; }, [](S& s, bool v) { s.pose.override_tilt = v; });
    flag("override_rotation", QStringLiteral("回転を上書き"), [](const S& s) { return s.pose.override_rotation; }, [](S& s, bool v) { s.pose.override_rotation = v; });
    flag("override_pressure", QStringLiteral("筆圧を上書き"), [](const S& s) { return s.pose.override_pressure; }, [](S& s, bool v) { s.pose.override_pressure = v; });
    // ノイズ・ウェットエッジ・ビルドアップ・テクスチャを保護
    section(QStringLiteral("そのほか"), nullptr);
    flag("noise", QStringLiteral("ノイズ"), [](const S& s) { return s.noise; }, [](S& s, bool v) { s.noise = v; });
    flag("wet_edges", QStringLiteral("ウェットエッジ"), [](const S& s) { return s.wet_edges; }, [](S& s, bool v) { s.wet_edges = v; });
    flag("build_up", QStringLiteral("ビルドアップ"), [](const S& s) { return s.build_up; }, [](S& s, bool v) { s.build_up = v; });
    number("build_up_rate", QStringLiteral("ビルドアップの速さ"), 1, 1000, 1, 0, QStringLiteral(" 点/秒"), [](const S& s) { return s.build_up_rate; },
           [](S& s, double v) { s.build_up_rate = v; });
    flag("protect_texture", QStringLiteral("テクスチャを保護"), [](const S& s) { return s.protect_texture; }, [](S& s, bool v) { s.protect_texture = v; });
    // (review P3-20, E1 review P3-13: Photoshop's オプションバーの筆圧の 2 つ, kept with the brush as a preset keeps them)
    flag("pressure_size", QStringLiteral("筆圧でサイズを変える"), [](const S& s) { return s.pressure_size; }, [](S& s, bool v) { s.pressure_size = v; });
    flag("pressure_opacity", QStringLiteral("筆圧で不透明度を変える"), [](const S& s) { return s.pressure_opacity; }, [](S& s, bool v) { s.pressure_opacity = v; });
    outer->addWidget(hint(QStringLiteral("滑らかさ・流量・描画モードはオプションで（ブラシごとに覚えます）。")));
    outer->addStretch(1);
}

QWidget* BrushSettingsPanel::field(const QString& name) const { return findChild<QWidget*>(name); }

core::DabSettings BrushSettingsPanel::settings() const {
    core::DabSettings s = shown_;
    for (const auto& [name, read] : readers_) read(s);
    return s;
}

void BrushSettingsPanel::show_settings(const core::DabSettings& s) {
    loading_ = true;
    for (const auto& [name, write] : writers_) write(s);
    loading_ = false;
}

void BrushSettingsPanel::refresh() {
    if (keep_waiting_) keep_now();  // (a change waiting: kept before another brush is shown)
    BrushPanel* panel = window_->brush_panel();
    key_ = panel != nullptr ? panel->kind() : std::string(core::kDefaultBrush);
    const core::Brush brush = render::brushes::brush(key_);
    const QString name = QString::fromStdString(brush.label);
    if (brush.dabs) {
        shown_ = *brush.dabs;
        tip_ = shown_.tip;
        show_settings(shown_);
        pages->setCurrentIndex(0);
        about->setText(presets::is_own(key_) ? QStringLiteral("自分のブラシ「%1」").arg(name)
                                             : QStringLiteral("「%1」（入っているブラシ: 直すと複製して自分のブラシにします）").arg(name));
    } else {
        pages->setCurrentIndex(1);
        about->setText(QStringLiteral("「%1」（Genko の描き方のブラシ）").arg(name));
    }
    if (sample != nullptr) sample->refresh();
}

void BrushSettingsPanel::changed() {
    if (loading_) return;
    const core::DabSettings now = settings();
    try {
        (void)core::dabs_from_json(core::dabs_to_json(now));  // (what a book would take)
    } catch (const core::Error& error) {
        window_->flash(wording::error(QString::fromUtf8(error.what())), 3000, true);
        return;
    }
    const core::Brush brush = render::brushes::brush(key_);
    Json settings_of = core::brush_to_dict(brush);
    settings_of["engine"] = std::string(core::kDabEngine);
    settings_of["dabs"] = core::dabs_to_json(now);
    // (a built-in brush, or one an open book draws lines with — any tab's, review P3-8: copied as one's own first — no line
    // drawn already changes)
    if (!presets::is_own(key_) || window_->any_book_holds_brush(key_)) {
        try {
            const Json made = presets::save(config_dir(), std::nullopt, settings_of, brush.label + "のコピー", std::nullopt);
            key_ = made.value("key", std::string());
            settings_of["label"] = made.value("name", brush.label);
            render::brushes::define_brush(key_, settings_of);
            window_->brushes_changed(key_);
            window_->flash(QStringLiteral("「%1」を自分のブラシにして直しています").arg(QString::fromStdString(made.value("name", std::string()))), 3000);
        } catch (const std::exception& error) {
            window_->flash(wording::error(QString::fromUtf8(error.what())), 4000, true);
        }
        return;
    }
    try {
        render::brushes::define_brush(key_, settings_of);  // (this process at once: the sample and the next line)
    } catch (const std::exception& error) {
        window_->flash(wording::error(QString::fromUtf8(error.what())), 3000, true);
        return;
    }
    shown_ = now;
    forget_stroke_previews({key_});  // (its line only — review P2-3)
    if (sample != nullptr) sample->refresh();
    keep_waiting_ = true;
    keep_timer_.start();  // (the library a moment after the last change)
}

void BrushSettingsPanel::keep_now() {
    keep_timer_.stop();
    if (!keep_waiting_) return;
    keep_waiting_ = false;
    if (!presets::is_own(key_)) return;
    const core::Brush brush = render::brushes::brush(key_);
    if (brush.key != key_) return;
    try {
        presets::save(config_dir(), key_, render::brushes::with_paper_picture(core::brush_to_dict(brush)), brush.label, std::nullopt);
    } catch (const std::exception& error) {
        window_->flash(QStringLiteral("ブラシをライブラリに残せませんでした: %1").arg(wording::error(QString::fromUtf8(error.what()))), 5000, true);
    }
}

void BrushSettingsPanel::make_dab_copy() {
    const core::Brush brush = render::brushes::brush(key_);
    core::DabSettings d;
    d.size_mm = std::clamp(brush.width_mm, 0.01, 500.0);
    d.hardness = brush.texture == "soft" ? 0.0 : 1.0;
    d.opacity = std::clamp(brush.opacity, 0.0, 1.0);
    Json settings_of = core::brush_to_dict(brush);
    settings_of["engine"] = std::string(core::kDabEngine);
    settings_of["dabs"] = core::dabs_to_json(d);
    try {
        const Json made = presets::save(config_dir(), std::nullopt, render::brushes::with_paper_picture(settings_of), brush.label + "（ダブ）", std::nullopt);
        const std::string key = made.value("key", std::string());
        settings_of["label"] = made.value("name", brush.label);
        render::brushes::define_brush(key, settings_of);
        window_->brushes_changed(key);
        window_->flash(QStringLiteral("ダブのブラシ「%1」を作りました").arg(QString::fromStdString(made.value("name", std::string()))), 3000);
    } catch (const std::exception& error) {
        window_->flash(wording::error(QString::fromUtf8(error.what())), 4000, true);
    }
}

void BrushSettingsPanel::pick_tip() {
    const QString path = ask::open_path(this, QStringLiteral("先端の画像"), QStringLiteral("画像 (*.png *.jpg *.jpeg *.bmp *.webp *.tif *.tiff)"));
    if (path.isEmpty()) return;
    try {
        // (review P3-17: read within a bound and made a tip off the window's thread)
        const std::filesystem::path file = core::path_from_utf8(path.toStdString());
        const auto made = off_ui([file] {
            return render::dabs::take_in_picture(storage::read_file_bounded(file, std::size_t(256) << 20), true, core::kMaxTipEdge);
        });
        render::dabs::keep_picture(made.first, made.second);
        tip_ = made.first;
        changed();
    } catch (const std::exception& error) {
        window_->flash(QStringLiteral("先端にできませんでした: %1").arg(wording::error(QString::fromUtf8(error.what()))), 5000, true);
    }
}

}  // namespace genko::app
