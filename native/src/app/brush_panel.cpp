#include "app/brush_panel.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "app/ask.hpp"
#include "app/config.hpp"
#include "app/fields.hpp"
#include "app/theme.hpp"
#include "core/pynum.hpp"
#include "core/stroke_geom.hpp"
#include "render/abr.hpp"
#include "render/brushes.hpp"

namespace genko::app {

using core::Json;

namespace {

constexpr double kPi = 3.14159265358979323846;
const std::vector<double> kSizes{0.2, 0.3, 0.5, 0.8, 1.2, 2.0, 3.0, 5.0, 10.0};
const std::vector<std::array<int, 3>> kMono{{20, 20, 20}, {64, 64, 64}, {128, 128, 128}, {192, 192, 192}, {255, 255, 255}};
// a few quiet colours for colour work and blue pencil (the chosen colour is always one click away)
const std::vector<std::array<int, 3>> kColours{{196, 72, 60}, {214, 140, 72}, {206, 178, 92}, {98, 142, 96}, {72, 118, 164},
                                               {86, 96, 150}, {130, 96, 140}, {222, 178, 172}, {150, 116, 88}, {238, 222, 204}};
const std::vector<std::pair<QString, double>> kPressure{{QStringLiteral("やわらかい"), 0.7}, {QStringLiteral("ふつう"), 1.0}, {QStringLiteral("かたい"), 1.6}};

const std::vector<std::pair<QString, QString>> kTextures{{QStringLiteral("なし（なめらか）"), QString()},
                                                         {QStringLiteral("鉛筆のざらつき"), QStringLiteral("grain")},
                                                         {QStringLiteral("筆のかすれ"), QStringLiteral("dry")},
                                                         {QStringLiteral("エアブラシ（ぼかし）"), QStringLiteral("soft")},
                                                         {QStringLiteral("水彩（縁に色がたまる）"), QStringLiteral("water")}};
const std::vector<std::pair<QString, QString>> kTips{{QStringLiteral("丸"), QStringLiteral("round")},
                                                     {QStringLiteral("平たい（カリグラフィ）"), QStringLiteral("flat")},
                                                     {QStringLiteral("画像"), QStringLiteral("image")}};
const std::vector<std::pair<QString, QString>> kPatterns{{QStringLiteral("なし"), QString()},          {QStringLiteral("点"), QStringLiteral("dots")},
                                                         {QStringLiteral("破線"), QStringLiteral("dash")},   {QStringLiteral("レース"), QStringLiteral("lace")},
                                                         {QStringLiteral("草"), QStringLiteral("grass")},    {QStringLiteral("ハート"), QStringLiteral("hearts")},
                                                         {QStringLiteral("星"), QStringLiteral("stars")},    {QStringLiteral("葉"), QStringLiteral("leaves")}};
const std::vector<std::pair<QString, QString>> kAa{{QStringLiteral("なし"), QStringLiteral("none")}, {QStringLiteral("弱"), QStringLiteral("weak")},
                                                   {QStringLiteral("中"), QStringLiteral("normal")}, {QStringLiteral("強"), QStringLiteral("strong")}};

QString key_of(const std::string& kind) { return QString::fromStdString(kind); }

QString number_text(double v) {  // (Python's f"{v:g}")
    return QString::number(v, 'g', 6);
}

QLabel* section(const QString& title, const QString& tip = {}) {
    auto* label = new QLabel(title);
    theme::role(label, "section");
    if (!tip.isEmpty()) label->setToolTip(tip);
    return label;
}

QDoubleSpinBox* number_box(double lo, double hi, double step, int decimals, const QString& suffix) {
    auto* box = new QDoubleSpinBox;
    box->setMaximumWidth(110);
    box->setRange(lo, hi);
    box->setSingleStep(step);
    box->setDecimals(decimals);
    box->setSuffix(suffix);
    return box;
}

QSpinBox* whole_box(int lo, int hi, const QString& suffix = {}) {
    auto* box = new QSpinBox;
    box->setMaximumWidth(110);
    box->setRange(lo, hi);
    box->setSuffix(suffix);
    return box;
}

std::map<std::string, QPixmap>& previews() {
    static std::map<std::string, QPixmap> cache;
    return cache;
}

// The S of a sample: points lightly, hard, lightly (n + 1 of them) over w × h px at `dpi`.
core::PenPoints s_curve(int n, double x0, double dx, double y0, double amplitude, double period, int dpi) {
    const double mm = 25.4 / dpi;
    core::PenPoints points;
    for (int i = 0; i <= n; ++i)
        points.push_back(core::PenPoint{(x0 + i * dx) * mm, (y0 + amplitude * std::sin(i / period)) * mm, std::sin(kPi * i / n)});
    return points;
}

// a coverage put on a picture in one colour (its strength × opacity)
void lay(QImage& image, const render::brushes::Coverage& drawn, const std::array<int, 3>& rgb, double opacity, bool over_paper) {
    const std::string cover = drawn.mask.tobytes();
    const int cw = drawn.mask.width(), ch = drawn.mask.height();
    for (int y = 0; y < ch; ++y) {
        const int iy = drawn.origin.y + y;
        if (iy < 0 || iy >= image.height()) continue;
        uchar* row = image.scanLine(iy);
        for (int x = 0; x < cw; ++x) {
            const int ix = drawn.origin.x + x;
            if (ix < 0 || ix >= image.width()) continue;
            const int a = static_cast<int>(static_cast<unsigned char>(cover[static_cast<std::size_t>(y) * cw + x]) * opacity);
            if (a == 0) continue;
            uchar* px = row + ix * 4;
            if (over_paper) {  // (Image.paste with the coverage as its mask: the paper shows through the rest)
                for (int c = 0; c < 3; ++c) px[c] = static_cast<uchar>((rgb[static_cast<std::size_t>(c)] * a + px[c] * (255 - a) + 127) / 255);
            } else {
                for (int c = 0; c < 3; ++c) px[c] = static_cast<uchar>(rgb[static_cast<std::size_t>(c)]);
                px[3] = static_cast<uchar>(a);
            }
        }
    }
}

}  // namespace

QPixmap stroke_preview(const std::string& kind, const std::array<int, 3>& ink, QSize size) {
    const std::string key = kind + "/" + std::to_string(ink[0]) + "," + std::to_string(ink[1]) + "," + std::to_string(ink[2]) + "/" +
                            std::to_string(size.width()) + "x" + std::to_string(size.height());
    if (const auto found = previews().find(key); found != previews().end()) return found->second;
    const int w = size.width() * 2, h = size.height() * 2;
    constexpr int dpi = 96;
    const double mm = 25.4 / dpi;
    const core::Brush brush = render::brushes::brush(kind);
    core::PenPoints points = s_curve(60, 6, (w - 12) / 60.0, h / 2.0, h / 4.0, 9.5, dpi);
    if (brush.taper) points = core::taper_points(points);
    const double width = std::max(0.4, std::min(brush.width_mm * 1.6, h * 0.4 * mm));
    QImage image(w, h, QImage::Format_RGBA8888);
    image.fill(Qt::transparent);
    std::optional<render::brushes::Coverage> drawn;
    try {
        drawn = render::brushes::draw(render::Size{w, h}, points, dpi, width, kind, "preview");
    } catch (const std::exception&) {
        drawn.reset();  // (a broken brush of one's own: no picture rather than no list)
    }
    if (drawn) {
        std::array<int, 3> rgb = ink;
        if (brush.rgb && *brush.rgb != std::vector<std::int64_t>{20, 20, 20})
            rgb = {static_cast<int>((*brush.rgb)[0]), static_cast<int>((*brush.rgb)[1]), static_cast<int>((*brush.rgb)[2])};
        lay(image, *drawn, rgb, std::max(0.35, brush.opacity), false);
    }
    QPixmap pixmap = QPixmap::fromImage(image);
    pixmap.setDevicePixelRatio(2);
    previews()[key] = pixmap;
    return pixmap;
}

std::vector<double> size_presets() {
    const QString raw = settings()->value(QStringLiteral("brush/sizes"), QString()).toString();
    std::set<double> values;
    bool ok = true;
    for (const QString& part : raw.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        if (part.trimmed().isEmpty()) continue;
        const double v = part.trimmed().toDouble(&ok);
        if (!ok) {
            values.clear();
            break;
        }
        values.insert(core::py_round(v, 2));
    }
    std::vector<double> out;
    for (const double v : values)
        if (v >= 0.05 && v <= 50) out.push_back(v);
    return out.empty() ? kSizes : out;
}

// --- the panel ---------------------------------------------------------------------------------------------------------

BrushPanel::BrushPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("brush_panel"));
    try {
        render::brushes::register_brushes(render::brushes::load_library(config_dir()));  // the person's own brushes
    } catch (const std::exception&) {
        // (a library that cannot be read: the built-in brushes)
    }
    kinds = new QListWidget;
    kinds->setObjectName(QStringLiteral("brush_kinds"));
    fill_kinds();
    kinds->setMaximumHeight(170);
    make = new QPushButton(QStringLiteral("複製して調整…"));
    make->setToolTip(QStringLiteral("選んでいるペンをもとに、入り抜き・筆圧・質感などを変えた自分のブラシを作ります"));
    forget = new QPushButton(QStringLiteral("自作のブラシを消す"));
    edit = new QPushButton(QStringLiteral("詳細を直す…"));
    edit->setToolTip(QStringLiteral("選んでいる自分のブラシ（★）の、入り抜き・筆圧・先端・質感などを全部開いて直します"));
    forget->setToolTip(QStringLiteral("自分のブラシ一覧から消します（そのブラシで描いた原稿の線はそのまま）"));
    connect(kinds, &QListWidget::currentRowChanged, this, [this](int) { kind_changed(); });
    size = number_box(0.05, 50, 0.1, 2, QStringLiteral(" mm"));
    size->setObjectName(QStringLiteral("brush_size"));
    connect(size, &QDoubleSpinBox::valueChanged, this, [this] { save(); });
    size_grid_ = new QGridLayout;
    size_grid_->setSpacing(2);
    fill_sizes();
    opacity = new QSlider(Qt::Horizontal);
    opacity->setRange(5, 100);
    connect(opacity, &QSlider::valueChanged, this, [this] { save(); });
    steady = whole_box(0, 15);
    steady->setToolTip(QStringLiteral("大きいほど手ぶれを抑える（線が少し遅れて付いてくる）"));
    connect(steady, &QSpinBox::valueChanged, this, [this] { save(); });
    taper = new QCheckBox(QStringLiteral("入り抜き"));
    taper->setToolTip(QStringLiteral("線の両端を細くします"));
    connect(taper, &QCheckBox::toggled, this, [this] { save(); });
    taper_in = number_box(-1, 30, 0.5, 1, QStringLiteral(" mm"));
    taper_out = number_box(-1, 30, 0.5, 1, QStringLiteral(" mm"));
    for (const auto& [box, what] : {std::pair{taper_in, QStringLiteral("入り（描き始め）")}, {taper_out, QStringLiteral("抜き（描き終わり）")}}) {
        box->setSpecialValueText(QStringLiteral("自動"));  // (-1: 自動, the line's length decides)
        box->setToolTip(what + QStringLiteral("が細くなる長さ。0 でその端は細くしない。自動: 線の長さの 1/4（両方とも自動のとき）"));
        connect(box, &QDoubleSpinBox::valueChanged, this, [this] { save(); });
    }
    ink_pressure = whole_box(0, 100, QStringLiteral(" %"));
    ink_pressure->setToolTip(QStringLiteral("弱い筆圧で線が薄くなる割合（鉛筆の下描き・影に）。0 % で筆圧は太さにだけ効く"));
    connect(ink_pressure, &QSpinBox::valueChanged, this, [this] { save(); });
    speed_steady = new QCheckBox(QStringLiteral("速い線ほど補正を強く"));
    speed_steady->setToolTip(QStringLiteral("速度による手ブレ補正: すばやく引いた所ほど手ぶれを強く抑え、ゆっくり描いた所は細かい形を残す"));
    connect(speed_steady, &QCheckBox::toggled, this, [this] { save(); });
    snap_lines = number_box(0, 5, 0.5, 1, QStringLiteral(" mm"));
    snap_lines->setSpecialValueText(QStringLiteral("しない"));
    snap_lines->setToolTip(QStringLiteral("ベクター吸着: 線の端がこの距離まで近い線（同じレイヤー）に、くっついて止まる。形が閉じ、線がつながる"));
    connect(snap_lines, &QDoubleSpinBox::valueChanged, this, [this] { save(); });
    post_fit = number_box(0, 2, 0.1, 1, QStringLiteral(" mm"));
    post_fit->setSpecialValueText(QStringLiteral("しない"));
    post_fit->setToolTip(QStringLiteral("後補正: 描き終えた線のゆれ（この幅まで）を除き、なめらかな曲線に置き換える"));
    connect(post_fit, &QDoubleSpinBox::valueChanged, this, [this] { save(); });
    pressure = new QComboBox;
    pressure->setToolTip(QStringLiteral("やわらかい: 弱い力でも太く。かたい: 強く押したときだけ太く"));
    for (const auto& [label, gamma] : kPressure) pressure->addItem(label, gamma);
    connect(pressure, &QComboBox::currentIndexChanged, this, [this] { save(); });
    // colour
    swatch = new QPushButton;
    swatch->setFixedSize(40, 40);
    connect(swatch, &QPushButton::clicked, this, [this] { pick(); });
    auto* palette = new QGridLayout;
    std::vector<std::array<int, 3>> all = kMono;
    all.insert(all.end(), kColours.begin(), kColours.end());
    for (std::size_t i = 0; i < all.size(); ++i) {
        const auto rgb = all[i];
        auto* button = new QPushButton;
        button->setFixedSize(22, 22);  // (22 px, 2 px apart or more: a 24 px target pitch)
        button->setStyleSheet(QStringLiteral("QPushButton { background: rgb(%1, %2, %3); border: 1px solid rgba(128,128,128,0.45); border-radius: 11px; }"
                                             "QPushButton:hover { border: 2px solid palette(highlight); }")
                                  .arg(rgb[0])
                                  .arg(rgb[1])
                                  .arg(rgb[2]));
        button->setToolTip(rgb == std::array<int, 3>{255, 255, 255} ? QStringLiteral("白")
                           : rgb == std::array<int, 3>{20, 20, 20}  ? QStringLiteral("黒")
                                                                     : QStringLiteral("色（RGB %1, %2, %3）").arg(rgb[0]).arg(rgb[1]).arg(rgb[2]));
        button->setAccessibleName(button->toolTip());
        connect(button, &QPushButton::clicked, this, [this, rgb] { set_colour(rgb); });
        palette->addWidget(button, static_cast<int>(i / 5), static_cast<int>(i % 5));
    }
    // fill
    gap = number_box(0, 5, 0.1, 2, QStringLiteral(" mm"));
    gap->setToolTip(QStringLiteral("線の切れ目がこの幅までなら、閉じているとみなして塗る"));
    connect(gap, &QDoubleSpinBox::valueChanged, this, [this] { save(); });
    reference = new QComboBox;
    reference->addItem(QStringLiteral("見えている全部"), QStringLiteral("page"));
    reference->addItem(QStringLiteral("このレイヤーだけ"), QStringLiteral("layer"));
    reference->addItem(QStringLiteral("参照レイヤー"), QStringLiteral("reference"));
    reference->setToolTip(QStringLiteral("参照レイヤー: レイヤー パネルで「参照にする」にしたレイヤーの線を見て塗ります"));
    connect(reference, &QComboBox::currentIndexChanged, this, [this] { save(); });
    tolerance = whole_box(0, 100, QStringLiteral(" %"));
    tolerance->setToolTip(QStringLiteral("色の誤差: 大きいほど、灰色やうすい線も越えて塗る。小さいほど、うすい線でも止まる"));
    connect(tolerance, &QSpinBox::valueChanged, this, [this] { save(); });
    expand = number_box(0, 1.5, 0.05, 2, QStringLiteral(" mm"));
    expand->setToolTip(QStringLiteral("領域拡縮: 塗りを線の下へ広げる幅（塗り残しの白いすき間を防ぐ）"));
    connect(expand, &QDoubleSpinBox::valueChanged, this, [this] { save(); });
    skip_draft = new QCheckBox(QStringLiteral("下描き・ネームは見ない"));
    skip_draft->setToolTip(QStringLiteral("下描きやネームの線を壁にせず、ペン入れの線だけで区切って塗る"));
    connect(skip_draft, &QCheckBox::toggled, this, [this] { save(); });
    skip_text = new QCheckBox(QStringLiteral("台詞・フキダシは見ない"));
    skip_text->setToolTip(QStringLiteral("台詞とフキダシの線を壁にしない"));
    connect(skip_text, &QCheckBox::toggled, this, [this] { save(); });
    lasso_mode = new QComboBox;
    for (const auto& [label, key] : {std::pair{QStringLiteral("囲んだ形を塗る"), QStringLiteral("shape")},
                                     {QStringLiteral("囲んだ中の閉じた所だけ"), QStringLiteral("enclosed")},
                                     {QStringLiteral("なぞった所の塗り残し"), QStringLiteral("gaps")}})
        lasso_mode->addItem(label, key);
    lasso_mode->setToolTip(QStringLiteral("囲って塗る（Shift+G）の塗り方。閉じた所だけ: 囲んだ中で、線に囲まれた所だけを塗る。"
                                          "塗り残し: なぞった所の、塗った色の間に残ったすき間だけを塗る"));
    connect(lasso_mode, &QComboBox::currentIndexChanged, this, [this] { save(); });
    gap_size = number_box(0.2, 6, 0.1, 2, QStringLiteral(" mm"));
    gap_size->setToolTip(QStringLiteral("塗り残しとみなす、すき間の大きさ（これより大きい白は残す）"));
    connect(gap_size, &QDoubleSpinBox::valueChanged, this, [this] { save(); });
    crossing = new QCheckBox(QStringLiteral("消しゴムで交点まで消す"));
    crossing->setToolTip(QStringLiteral("線の交わる所までを一度に消します（はみ出しの掃除）"));
    connect(crossing, &QCheckBox::toggled, this, [this] { save(); });

    // the pen as set now, drawn (a change is seen before the page is touched)
    sample = new LineSample([this] {
        Json f = stroke_fields();
        f["opacity"] = opacity->value() / 100.0;
        f["rgb"] = Json::array({rgb_[0], rgb_[1], rgb_[2]});
        return f;
    });
    auto* form = new QFormLayout;
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->addRow(sample);
    form->addRow(QStringLiteral("太さ"), slider_for(size, true));
    auto* sizes_box = new QWidget;
    sizes_box->setLayout(size_grid_);
    size_grid_->setContentsMargins(0, 0, 0, 0);
    form->addRow(sizes_box);
    form->addRow(QStringLiteral("不透明度"), with_value(opacity));
    form->addRow(QStringLiteral("手ぶれ補正"), slider_for(steady));
    form->addRow(speed_steady);
    form->addRow(QStringLiteral("線の端をくっつける"), slider_for(snap_lines));
    form->addRow(QStringLiteral("後補正"), slider_for(post_fit));
    form->addRow(taper);
    form->addRow(QStringLiteral("入り"), slider_for(taper_in));
    form->addRow(QStringLiteral("抜き"), slider_for(taper_out));
    form->addRow(QStringLiteral("筆圧で濃さ"), slider_for(ink_pressure));
    form->addRow(QStringLiteral("筆圧"), pressure);
    palette->setSpacing(3);
    auto* colour_row = new QHBoxLayout;
    colour_row->addWidget(swatch, 0, Qt::AlignTop);
    colour_row->addLayout(palette);
    colour_row->addStretch(1);
    auto* fill_form = new QFormLayout;
    fill_form->setRowWrapPolicy(QFormLayout::WrapAllRows);
    fill_form->addRow(QStringLiteral("隙間を閉じる"), slider_for(gap));
    fill_form->addRow(QStringLiteral("見る範囲"), reference);
    fill_form->addRow(skip_draft);
    fill_form->addRow(skip_text);
    fill_form->addRow(QStringLiteral("色の誤差"), slider_for(tolerance));
    fill_form->addRow(QStringLiteral("線の下へ広げる"), slider_for(expand));
    fill_form->addRow(QStringLiteral("囲って塗る"), lasso_mode);
    fill_form->addRow(QStringLiteral("塗り残しの大きさ"), slider_for(gap_size));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(section(QStringLiteral("ペンの種類")));
    layout->addWidget(kinds);
    files = new QPushButton(QStringLiteral("読み込み・書き出し ▾"));
    files->setToolTip(QStringLiteral("ブラシをファイルに書き出す・読み込む（.genkobrush、Photoshop の .abr）"));
    auto* make_row = new QVBoxLayout;
    make_row->setSpacing(1);
    for (QPushButton* button : {make, edit, forget, files}) {
        button->setProperty("row", true);
        make_row->addWidget(button);
    }
    layout->addLayout(make_row);
    layout->addWidget(section(QStringLiteral("描き味")));
    layout->addLayout(form);
    layout->addWidget(section(QStringLiteral("色"), QStringLiteral("スポイト（I）で原稿から拾えます")));
    layout->addLayout(colour_row);
    layout->addWidget(section(QStringLiteral("塗りつぶし（G）")));
    layout->addLayout(fill_form);
    layout->addWidget(crossing);
    layout->addStretch(1);
    loading_ = true;
    load();
    loading_ = false;
    forget->setEnabled(kind().starts_with("my_"));
}

void BrushPanel::fill_kinds() {
    Json mine = Json::object();
    try {
        mine = render::brushes::load_library(config_dir());
    } catch (const std::exception&) {
    }
    const QColor text(theme::tokens().text);
    kinds->setIconSize(QSize(72, 20));
    for (const core::Brush& brush : render::brushes::everything()) {
        auto* item = new QListWidgetItem((brush.key.starts_with("my_") ? QStringLiteral("★ ") : QString()) + QString::fromStdString(brush.label));
        item->setIcon(QIcon(stroke_preview(brush.key, {text.red(), text.green(), text.blue()}, QSize(72, 20))));
        item->setData(Qt::UserRole, key_of(brush.key));
        if (brush.key.starts_with("my_") && !mine.contains(brush.key)) item->setToolTip(QStringLiteral("この原稿に入っていたブラシ"));
        kinds->addItem(item);
    }
}

void BrushPanel::reload_kinds(const std::optional<std::string>& select) {
    const std::string current = select.value_or(kind());
    loading_ = true;
    kinds->clear();
    fill_kinds();
    int row = 0;
    for (int i = 0; i < kinds->count(); ++i)
        if (kinds->item(i)->data(Qt::UserRole).toString().toStdString() == current) row = i;
    kinds->setCurrentRow(row);
    loading_ = false;
    forget->setEnabled(kind().starts_with("my_"));
    if (select) kind_changed();
}

void BrushPanel::set_sizes(const std::vector<double>& values) {
    std::set<double> kept;
    for (const double v : values)
        if (v >= 0.05 && v <= 50) kept.insert(core::py_round(v, 2));
    QStringList parts;
    for (const double v : kept) parts << number_text(v);
    settings()->setValue(QStringLiteral("brush/sizes"), parts.join(QLatin1Char(',')));
    fill_sizes();
}

void BrushPanel::fill_sizes() {
    // the size chips: click to use; right-click to set it to the size in use or take it away; ＋ keeps the size in use
    while (QLayoutItem* item = size_grid_->takeAt(0)) {
        if (QWidget* w = item->widget()) w->deleteLater();
        delete item;
    }
    const std::vector<double> presets = sizes();
    for (std::size_t i = 0; i <= presets.size(); ++i) {
        const bool plus = i == presets.size();
        auto* button = new QPushButton(plus ? QStringLiteral("＋") : number_text(presets[i]));
        button->setFixedWidth(34);
        button->setProperty("chip", true);  // (small flat choices: theme)
        if (plus) {
            button->setToolTip(QStringLiteral("今の太さをプリセットに足す"));
            connect(button, &QPushButton::clicked, this, [this] {
                auto values = sizes();
                values.push_back(size->value());
                set_sizes(values);
            });
        } else {
            const double value = presets[i];
            button->setToolTip(QStringLiteral("%1 mm（右クリックで今の太さにする・消す）").arg(number_text(value)));
            connect(button, &QPushButton::clicked, this, [this, value] { size->setValue(value); });
            button->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(button, &QPushButton::customContextMenuRequested, this, [this, value, button](const QPoint&) { size_menu(value, button); });
        }
        size_grid_->addWidget(button, static_cast<int>(i / 5), static_cast<int>(i % 5));
    }
}

void BrushPanel::size_menu(double value, QPushButton* button) {
    auto* menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->addAction(QStringLiteral("今の太さ（%1 mm）にする").arg(number_text(size->value())), this, [this, value] {
        std::vector<double> values;
        for (const double v : sizes()) values.push_back(v == value ? size->value() : v);
        set_sizes(values);
    });
    QAction* remove = menu->addAction(QStringLiteral("このプリセットを消す"), this, [this, value] {
        std::vector<double> values;
        for (const double v : sizes())
            if (v != value) values.push_back(v);
        set_sizes(values);
    });
    remove->setEnabled(sizes().size() > 1);
    menu->addAction(QStringLiteral("元の 9 つに戻す"), this, [this] { set_sizes({}); });
    menu->popup(button->mapToGlobal(button->rect().bottomLeft()));
}

std::string BrushPanel::kind() const {
    const QListWidgetItem* item = kinds->currentItem();
    return item != nullptr ? item->data(Qt::UserRole).toString().toStdString() : std::string(core::kDefaultBrush);
}

void BrushPanel::load() {
    const auto store = settings();
    const std::string chosen = store->value(QStringLiteral("brush/kind"), QString::fromLatin1(core::kDefaultBrush.data())).toString().toStdString();
    int row = 0;
    for (int i = 0; i < kinds->count(); ++i)
        if (kinds->item(i)->data(Qt::UserRole).toString().toStdString() == chosen) row = i;
    kinds->setCurrentRow(row);
    apply_kind_defaults(chosen, true);
    const QStringList rgb = store->value(QStringLiteral("brush/rgb"), QStringLiteral("20,20,20")).toString().split(QLatin1Char(','));
    std::array<int, 3> colour{20, 20, 20};
    bool ok = rgb.size() >= 3;
    for (int i = 0; i < 3 && ok; ++i) colour[static_cast<std::size_t>(i)] = rgb[i].trimmed().toInt(&ok);
    set_colour(ok ? colour : std::array<int, 3>{20, 20, 20});
    gap->setValue(store->value(QStringLiteral("fill/gap"), 0.3).toDouble());
    reference->setCurrentIndex(std::max(0, reference->findData(store->value(QStringLiteral("fill/reference"), QStringLiteral("page")).toString())));
    crossing->setChecked(store->value(QStringLiteral("eraser/crossing"), QStringLiteral("false")).toString().toLower() == QLatin1String("true"));
    tolerance->setValue(static_cast<int>(store->value(QStringLiteral("fill/tolerance"), 37).toDouble()));
    expand->setValue(store->value(QStringLiteral("fill/expand"), 0.15).toDouble());
    skip_draft->setChecked(store->value(QStringLiteral("fill/skip_draft"), QStringLiteral("false")).toString().toLower() == QLatin1String("true"));
    skip_text->setChecked(store->value(QStringLiteral("fill/skip_text"), QStringLiteral("false")).toString().toLower() == QLatin1String("true"));
    lasso_mode->setCurrentIndex(std::max(0, lasso_mode->findData(store->value(QStringLiteral("fill/lasso"), QStringLiteral("shape")).toString())));
    gap_size->setValue(store->value(QStringLiteral("fill/gap_size"), 1.5).toDouble());
}

void BrushPanel::apply_kind_defaults(const std::string& kind_key, bool stored) {
    const core::Brush brush = render::brushes::brush(kind_key);
    const auto store = settings();
    const QString prefix = QStringLiteral("brush/%1/").arg(key_of(kind_key));
    const auto kept = [&](const char* key, const QVariant& fallback) {
        return stored ? store->value(prefix + QString::fromLatin1(key), fallback) : fallback;
    };
    size->setValue(kept("size", brush.width_mm).toDouble());
    opacity->setValue(static_cast<int>(kept("opacity", brush.opacity).toDouble() * 100));
    steady->setValue(static_cast<int>(kept("steady", static_cast<qlonglong>(brush.stabilize)).toLongLong()));
    taper->setChecked(stored ? kept("taper", brush.taper).toString().toLower() == QLatin1String("true") : brush.taper);
    const double gamma = kept("pressure", 1.0).toDouble();
    pressure->setCurrentIndex(std::max(0, pressure->findData(gamma)));
    taper_in->setValue(kept("taper_in", -1).toDouble());
    taper_out->setValue(kept("taper_out", -1).toDouble());
    ink_pressure->setValue(static_cast<int>(kept("ink_pressure", 0).toDouble()));
    speed_steady->setChecked(kept("speed_steady", false).toString().toLower() == QLatin1String("true"));
    post_fit->setValue(kept("post_fit", 0).toDouble());
    snap_lines->setValue(kept("snap_lines", 0).toDouble());
    follow_taper();
}

void BrushPanel::kind_changed() {
    if (loading_) return;
    loading_ = true;
    apply_kind_defaults(kind(), true);
    loading_ = false;
    forget->setEnabled(kind().starts_with("my_"));
    save();
}

void BrushPanel::save() {
    if (sample != nullptr) sample->refresh();
    if (loading_) return;
    const std::string key = kind();
    const auto store = settings();
    const QString prefix = QStringLiteral("brush/%1/").arg(key_of(key));
    store->setValue(QStringLiteral("brush/kind"), key_of(key));
    store->setValue(prefix + QStringLiteral("size"), size->value());
    store->setValue(prefix + QStringLiteral("opacity"), opacity->value() / 100.0);
    store->setValue(prefix + QStringLiteral("steady"), steady->value());
    store->setValue(prefix + QStringLiteral("taper"), taper->isChecked() ? QStringLiteral("true") : QStringLiteral("false"));
    store->setValue(prefix + QStringLiteral("pressure"), pressure->currentData());
    store->setValue(prefix + QStringLiteral("taper_in"), taper_in->value());
    store->setValue(prefix + QStringLiteral("taper_out"), taper_out->value());
    store->setValue(prefix + QStringLiteral("ink_pressure"), ink_pressure->value());
    store->setValue(prefix + QStringLiteral("speed_steady"), speed_steady->isChecked() ? QStringLiteral("true") : QStringLiteral("false"));
    store->setValue(prefix + QStringLiteral("post_fit"), post_fit->value());
    store->setValue(prefix + QStringLiteral("snap_lines"), snap_lines->value());
    follow_taper();
    store->setValue(QStringLiteral("fill/gap"), gap->value());
    store->setValue(QStringLiteral("fill/reference"), reference->currentData());
    store->setValue(QStringLiteral("eraser/crossing"), crossing->isChecked() ? QStringLiteral("true") : QStringLiteral("false"));
    store->setValue(QStringLiteral("fill/tolerance"), tolerance->value());
    store->setValue(QStringLiteral("fill/expand"), expand->value());
    store->setValue(QStringLiteral("fill/skip_draft"), skip_draft->isChecked() ? QStringLiteral("true") : QStringLiteral("false"));
    store->setValue(QStringLiteral("fill/skip_text"), skip_text->isChecked() ? QStringLiteral("true") : QStringLiteral("false"));
    store->setValue(QStringLiteral("fill/lasso"), lasso_mode->currentData());
    store->setValue(QStringLiteral("fill/gap_size"), gap_size->value());
    emit changed();
}

Json BrushPanel::fill_fields() const {
    Json out{{"gap_mm", gap->value()}, {"reference", reference->currentData().toString().toStdString()}, {"tolerance", tolerance->value()},
             {"expand_mm", core::py_round(expand->value(), 2)}};
    Json ignore = Json::array();
    if (skip_draft->isChecked()) ignore.push_back("draft");
    if (skip_text->isChecked()) ignore.push_back("text");
    if (!ignore.empty() && out["reference"] == "page") out["ignore"] = ignore;
    return out;
}

void BrushPanel::follow_taper() {
    // (the lengths only mean something while the line tapers)
    taper_in->setEnabled(taper->isChecked());
    taper_out->setEnabled(taper->isChecked());
}

void BrushPanel::set_colour(const std::array<int, 3>& rgb) {
    rgb_ = rgb;
    swatch->setStyleSheet(QStringLiteral("QPushButton { background: rgb(%1, %2, %3); border: 2px solid rgba(128,128,128,0.6); border-radius: 20px; }")
                              .arg(rgb[0])
                              .arg(rgb[1])
                              .arg(rgb[2]));
    swatch->setToolTip(QStringLiteral("今の色 (%1, %2, %3)").arg(rgb[0]).arg(rgb[1]).arg(rgb[2]));
    settings()->setValue(QStringLiteral("brush/rgb"), QStringLiteral("%1,%2,%3").arg(rgb[0]).arg(rgb[1]).arg(rgb[2]));
    if (sample != nullptr) sample->refresh();
    emit changed();
}

void BrushPanel::pick() {
    if (const auto colour = ask::colour(this, QColor(rgb_[0], rgb_[1], rgb_[2]), QStringLiteral("色")))
        set_colour({colour->red(), colour->green(), colour->blue()});
}

double BrushPanel::nudge_size(int step) {
    const double value = size->value();
    const auto presets = sizes();
    std::size_t nearest = 0;
    for (std::size_t k = 1; k < presets.size(); ++k)
        if (std::abs(presets[k] - value) < std::abs(presets[nearest] - value)) nearest = k;
    const auto at = std::clamp<std::ptrdiff_t>(static_cast<std::ptrdiff_t>(nearest) + step, 0, static_cast<std::ptrdiff_t>(presets.size()) - 1);
    size->setValue(presets[static_cast<std::size_t>(at)]);
    return size->value();
}

PenSettings BrushPanel::pen() const {
    PenSettings p;
    p.kind = kind();
    p.width_mm = size->value();
    p.opacity = opacity->value() / 100.0;
    p.stabilize = steady->value();
    p.taper = taper->isChecked();
    p.taper_in_mm = taper_in->value();
    p.taper_out_mm = taper_out->value();
    p.pressure_gamma = pressure->currentData().toDouble();
    p.rgb = {rgb_[0], rgb_[1], rgb_[2]};
    p.ink_pressure = ink_pressure->value();
    p.speed_steady = speed_steady->isChecked();
    p.post_fit = post_fit->value();
    p.snap_lines = snap_lines->value();
    return p;
}

// --- the dialog ----------------------------------------------------------------------------------------------------------

BrushDialog::BrushDialog(QWidget* parent, const std::string& base, bool editing) : QDialog(parent), base_(base) {
    setObjectName(QStringLiteral("brush_dialog"));
    setWindowTitle(editing ? QStringLiteral("ブラシの詳細") : QStringLiteral("ブラシを複製して調整"));
    const core::Brush b = render::brushes::brush(base);
    const auto combo = [](const std::vector<std::pair<QString, QString>>& items, const std::string& now) {
        auto* box = new QComboBox;
        for (const auto& [label, key] : items) box->addItem(label, key);
        box->setCurrentIndex(std::max(0, box->findData(QString::fromStdString(now))));
        return box;
    };
    const auto spin = [](int lo, int hi, int value, const QString& suffix = {}) {
        auto* box = new QSpinBox;
        box->setRange(lo, hi);
        box->setSuffix(suffix);
        box->setValue(value);
        return box;
    };
    const auto percent = [](double v) { return static_cast<int>(core::py_round_whole(v * 100)); };
    name = new QLineEdit(QString::fromStdString(b.label) + QStringLiteral(" のコピー"));
    width = new QDoubleSpinBox;
    width->setRange(0.05, 50);
    width->setSingleStep(0.1);
    width->setSuffix(QStringLiteral(" mm"));
    width->setValue(b.width_mm);
    thin = spin(0, 100, percent(b.min_pressure), QStringLiteral(" %"));
    thin->setToolTip(QStringLiteral("いちばん弱く描いたときの太さ（太さに対する割合）。0 で針のように細くなる"));
    curve = new QDoubleSpinBox;
    curve->setRange(0.2, 5);
    curve->setSingleStep(0.1);
    curve->setValue(b.gamma);
    curve->setToolTip(QStringLiteral("1 より大きいと、強く押したときだけ太くなる（かたい）。小さいと弱い力でも太い（やわらかい）"));
    opacity = spin(5, 100, percent(b.opacity), QStringLiteral(" %"));
    steady = spin(0, 15, static_cast<int>(b.stabilize));
    taper = new QCheckBox(QStringLiteral("入り抜き（線の両端を細く）"));
    taper->setChecked(b.taper);
    texture = combo(kTextures, b.texture);
    fixed = new QCheckBox(QStringLiteral("筆圧で太さを変えない"));
    fixed->setChecked(b.fixed_width);
    white = new QCheckBox(QStringLiteral("白で描く（修正用）"));
    white->setChecked(b.rgb && *b.rgb == std::vector<std::int64_t>{255, 255, 255});
    sample = new QLabel;
    sample->setObjectName(QStringLiteral("brush_sample"));
    // the tip and how it is laid down
    tip = combo(kTips, b.tip);
    tip_png = b.tip_png;
    tip_angle = spin(-180, 180, static_cast<int>(core::py_round_whole(b.tip_angle)), QStringLiteral(" °"));
    tip_ratio = spin(2, 100, percent(b.tip_ratio), QStringLiteral(" %"));
    tip_follow = new QCheckBox(QStringLiteral("先端を線の向きに合わせて回す"));
    tip_follow->setChecked(b.tip_follow);
    tip_rotation = new QCheckBox(QStringLiteral("ペンの軸を回すと先端も回る（アートペン）"));
    tip_rotation->setChecked(b.tip_rotation);
    tip_picture = new QPushButton(QStringLiteral("画像から先端を作る…"));
    connect(tip_picture, &QPushButton::clicked, this, [this] { pick_tip(); });
    pattern = combo(kPatterns, b.pattern);
    spacing = spin(0, 500, percent(b.spacing), QStringLiteral(" %"));
    spacing->setToolTip(QStringLiteral("先端を置く間隔（太さに対する割合）。0 で続いた線"));
    scatter = spin(0, 500, percent(b.scatter), QStringLiteral(" %"));
    scatter->setToolTip(QStringLiteral("先端を線から散らす広さ（スプレー・点描）"));
    stamp = spin(2, 300, percent(b.stamp_size), QStringLiteral(" %"));
    jitter = spin(0, 100, percent(b.size_jitter), QStringLiteral(" %"));
    turn = new QCheckBox(QStringLiteral("ランダムに回す"));
    turn->setChecked(b.turn_jitter);
    count = spin(1, 12, static_cast<int>(b.count));
    speed = spin(0, 100, percent(b.speed), QStringLiteral(" %"));
    speed->setToolTip(QStringLiteral("速く描くほど細くなる強さ"));
    post = spin(0, 10, static_cast<int>(b.post_smooth));
    post->setToolTip(QStringLiteral("描き終えた後に線をなめらかに整える強さ（後補正）"));
    mix = spin(0, 100, percent(b.mix), QStringLiteral(" %"));
    mix->setToolTip(QStringLiteral("下地混色: 同じレイヤーにもう塗ってある色を、どれだけ混ぜて描くか（0 % で混ぜない）"));
    stretch = spin(0, 100, percent(b.stretch), QStringLiteral(" %"));
    stretch->setToolTip(QStringLiteral("色延び: 拾った色を線の先までどれだけ引きずるか（混色が 0 % のときは効きません）"));
    aa = combo(kAa, b.aa);

    auto* form = new QFormLayout;
    form->addRow(QStringLiteral("名前"), name);
    form->addRow(section(QStringLiteral("線の太さと筆圧")));
    form->addRow(QStringLiteral("太さ（はじめの値）"), width);
    form->addRow(QStringLiteral("弱い筆圧での太さ"), thin);
    form->addRow(QStringLiteral("筆圧の効き方"), curve);
    form->addRow(QString(), taper);
    form->addRow(QStringLiteral("速さで細く"), speed);
    form->addRow(section(QStringLiteral("なめらかさ")));
    form->addRow(QStringLiteral("手ぶれ補正"), steady);
    form->addRow(QStringLiteral("後補正"), post);
    form->addRow(QStringLiteral("アンチエイリアス"), aa);
    form->addRow(section(QStringLiteral("色と質感")));
    form->addRow(QStringLiteral("不透明度"), opacity);
    form->addRow(QStringLiteral("質感"), texture);
    form->addRow(QStringLiteral("下地混色"), mix);
    form->addRow(QStringLiteral("色延び"), stretch);
    form->addRow(QString(), fixed);
    form->addRow(QString(), white);
    auto* tips = new QFormLayout;
    tips->addRow(section(QStringLiteral("先端")));
    tips->addRow(QStringLiteral("先端の形"), tip);
    tips->addRow(QString(), tip_picture);
    tips->addRow(QStringLiteral("先端の角度"), tip_angle);
    tips->addRow(QStringLiteral("平たさ"), tip_ratio);
    tips->addRow(QString(), tip_follow);
    tips->addRow(QString(), tip_rotation);
    tips->addRow(section(QStringLiteral("模様（点・レース・草などを並べる）")));
    tips->addRow(QStringLiteral("模様"), pattern);
    tips->addRow(QStringLiteral("間隔"), spacing);
    tips->addRow(QStringLiteral("散らばり"), scatter);
    tips->addRow(QStringLiteral("1 つの大きさ"), stamp);
    tips->addRow(QStringLiteral("大きさの乱れ"), jitter);
    tips->addRow(QString(), turn);
    tips->addRow(QStringLiteral("一度に置く数"), count);
    auto* tabs = new QTabWidget;
    auto* basic = new QWidget;
    auto* shape = new QWidget;
    basic->setLayout(form);
    shape->setLayout(tips);
    tabs->addTab(basic, QStringLiteral("描き味"));
    tabs->addTab(shape, QStringLiteral("先端・模様"));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(editing ? QStringLiteral("直す") : QStringLiteral("作る"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("やめる"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    sample->setMinimumSize(260, 160);
    sample->setAlignment(Qt::AlignCenter);
    sample->setStyleSheet(QStringLiteral("background: white; border-radius: 6px"));
    auto* header = new QLabel(editing ? QStringLiteral("「%1」の設定を直します。これから描く線に効きます。").arg(QString::fromStdString(b.label))
                                      : QStringLiteral("「%1」をもとに、自分のブラシを作ります。元のブラシは変わりません。").arg(QString::fromStdString(b.label)));
    header->setWordWrap(true);
    theme::role(header, "hint");
    auto* change = new QLabel(QStringLiteral("値を変えると、すぐにこの線が描き直されます（弱く・強く・弱くと押した線）。"));
    change->setWordWrap(true);
    theme::role(change, "hint");
    auto* side = new QVBoxLayout;
    side->addWidget(sample, 1);
    side->addWidget(change);
    auto* body = new QHBoxLayout;
    body->addWidget(tabs, 3);
    body->addLayout(side, 2);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(header);
    layout->addLayout(body, 1);
    layout->addWidget(buttons);
    resize(900, 600);
    for (QSpinBox* w : {thin, opacity, steady, tip_angle, tip_ratio, spacing, scatter, stamp, jitter, count, speed, post})
        connect(w, &QSpinBox::valueChanged, this, [this] { draw_sample(); });
    for (QDoubleSpinBox* w : {width, curve}) connect(w, &QDoubleSpinBox::valueChanged, this, [this] { draw_sample(); });
    for (QCheckBox* w : {taper, fixed, white, tip_follow, tip_rotation, turn}) connect(w, &QCheckBox::toggled, this, [this] { draw_sample(); });
    for (QComboBox* w : {texture, tip, pattern, aa}) connect(w, &QComboBox::currentIndexChanged, this, [this] { draw_sample(); });
    draw_sample();
}

Json BrushDialog::data() const {
    const QString label = name->text().trimmed();
    const std::string tip_kind = tip->currentData().toString().toStdString();
    Json out = Json::object();
    out["label"] = label.isEmpty() ? std::string("自分のブラシ") : label.toStdString();
    out["base"] = base_;
    out["width_mm"] = width->value();
    out["min_pressure"] = thin->value() / 100.0;
    out["gamma"] = core::py_round(curve->value(), 2);
    out["opacity"] = opacity->value() / 100.0;
    out["stabilize"] = steady->value();
    out["taper"] = taper->isChecked();
    out["texture"] = texture->currentData().toString().toStdString();
    out["fixed_width"] = fixed->isChecked();
    out["rgb"] = white->isChecked() ? Json::array({255, 255, 255}) : Json(nullptr);
    out["tip"] = tip_kind != "image" || !tip_png.empty() ? tip_kind : std::string("round");
    out["tip_angle"] = static_cast<double>(tip_angle->value());
    out["tip_ratio"] = tip_ratio->value() / 100.0;
    out["tip_follow"] = tip_follow->isChecked();
    out["tip_rotation"] = tip_rotation->isChecked();
    out["tip_png"] = tip_png;
    out["pattern"] = pattern->currentData().toString().toStdString();
    out["spacing"] = spacing->value() / 100.0;
    out["scatter"] = scatter->value() / 100.0;
    out["stamp_size"] = stamp->value() / 100.0;
    out["size_jitter"] = jitter->value() / 100.0;
    out["turn_jitter"] = turn->isChecked();
    out["count"] = count->value();
    out["speed"] = speed->value() / 100.0;
    out["post_smooth"] = post->value();
    out["aa"] = aa->currentData().toString().toStdString();
    out["mix"] = mix->value() / 100.0;
    out["stretch"] = stretch->value() / 100.0;
    return out;
}

void BrushDialog::pick_tip() {
    const QString path = ask::open_path(this, QStringLiteral("先端にする画像"), QStringLiteral("画像 (*.png *.jpg *.jpeg *.webp *.bmp)"));
    if (path.isEmpty()) return;
    try {
        tip_png = render::abr::tip_from_picture(std::filesystem::path(path.toStdU16String()));
    } catch (const std::exception&) {
        return;  // (a picture that cannot be read: nothing changes)
    }
    tip->setCurrentIndex(tip->findData(QStringLiteral("image")));
    if (spacing->value() == 0) spacing->setValue(25);
    draw_sample();
}

void BrushDialog::draw_sample() {
    // an S-curve pressed lightly, then hard, then lightly, as this brush draws it
    constexpr const char* kSample = "my_sample";
    core::Brush made;
    try {
        render::brushes::define_brush(kSample, data());
        made = render::brushes::brush(kSample);
    } catch (const std::exception&) {
        render::brushes::forget_brush(kSample);
        return;
    }
    constexpr int w = 300, h = 140, dpi = 96;
    core::PenPoints points = s_curve(100, 22, 2.56, 70, 34, 16, dpi);
    if (made.taper) points = core::taper_points(points);
    std::optional<render::brushes::Coverage> drawn;
    try {
        drawn = render::brushes::draw(render::Size{w, h}, points, dpi, std::max(0.3, std::min(made.width_mm, 4.0)), kSample, "sample");
    } catch (const std::exception&) {
        drawn.reset();
    }
    render::brushes::forget_brush(kSample);
    const bool white_ink = made.rgb && *made.rgb == std::vector<std::int64_t>{255, 255, 255};
    QImage paper(w, h, QImage::Format_RGBA8888);
    paper.fill(white_ink ? QColor(235, 235, 235) : QColor(255, 255, 255));
    if (drawn) {
        std::array<int, 3> rgb{20, 20, 20};
        if (made.rgb) rgb = {static_cast<int>((*made.rgb)[0]), static_cast<int>((*made.rgb)[1]), static_cast<int>((*made.rgb)[2])};
        lay(paper, *drawn, rgb, made.opacity, true);
    }
    sample->setPixmap(QPixmap::fromImage(paper));
}

}  // namespace genko::app
