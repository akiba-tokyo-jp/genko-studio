#include "app/material_tabs.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QCursor>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QIcon>
#include <QPixmap>
#include <QImage>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <tuple>
#include <vector>

#include "app/ask.hpp"
#include "app/canvas.hpp"
#include "app/config.hpp"
#include "app/main_window.hpp"
#include "app/material_panel.hpp"
#include "app/wording.hpp"
#include "core/base64.hpp"
#include "core/model.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "render/effects.hpp"
#include "render/materials.hpp"
#include "render/png.hpp"
#include "render/selection.hpp"
#include "render/tones.hpp"

namespace genko::app {

using core::Json;

namespace {

// (label, key)
const std::vector<std::pair<const char*, const char*>> kPatterns{
    {"網点", "dot"},          {"線", "line"},       {"カケアミ風（交差）", "cross"}, {"砂目", "noise"},   {"ベタのグレー", "flat"},
    {"柄: 市松", "check"},    {"柄: レンガ", "brick"}, {"柄: 波", "wave"},             {"柄: 格子", "grid"}, {"柄: 斜線", "hatch"},
    {"柄: 星", "star"},       {"柄: 砂", "sand"},   {"柄: 画像から…", "image"}};
const std::vector<std::pair<const char*, const char*>> kScreenShapes{{"丸", "round"}, {"四角", "square"}, {"ひし形", "diamond"}, {"楕円", "ellipse"}};

// The settings people change per effect kind: (key, label, lo, hi, step, default; NaN: none).
struct Field {
    const char* key;
    const char* label;
    double lo, hi, step;
    std::optional<double> fallback;
};
const std::map<std::string, std::vector<Field>>& effect_fields_of() {
    static const std::map<std::string, std::vector<Field>> fields{
        {"focus", {{"count", "本数", 10, 600, 10, 90}, {"inner_r", "中心の空き（mm）", 1, 200, 1, std::nullopt},
                   {"length_mm", "線の長さ（mm、0 で端まで）", 0, 400, 1, 0}, {"jitter", "ばらつき", 0, 1, 0.05, 0.25},
                   {"width_mm", "太さ（mm）", 0.05, 5, 0.05, 0.8}, {"twist", "渦（°）", -180, 180, 5, 0}}},
        {"speed", {{"count", "本数", 5, 400, 5, 40}, {"spacing_mm", "間隔（mm、0 で本数から）", 0, 50, 0.5, 0},
                   {"angle", "向き（°）", -180, 180, 5, 0}, {"length", "長さ", 0.05, 1, 0.05, 0.7}, {"curve", "曲がり（mm）", -80, 80, 1, 0},
                   {"jitter", "ばらつき", 0, 1, 0.05, 0.25}, {"width_mm", "太さ（mm）", 0.05, 5, 0.05, 0.5},
                   {"spread_mm", "沿わせた時の幅（mm）", 2, 300, 1, 40}}},
        {"uni_flash", {{"count", "本数", 20, 800, 10, 140}, {"inner_r", "中心の空き（mm）", 1, 200, 1, std::nullopt},
                       {"length_mm", "線の長さ（mm）", 2, 150, 1, std::nullopt}, {"jitter", "ばらつき", 0, 1, 0.05, 0.25},
                       {"width_mm", "太さ（mm）", 0.05, 3, 0.05, 0.35}}},
        {"beta_flash", {{"spikes", "トゲの数", 10, 400, 5, 70}, {"inner_r", "中心の空き（mm）", 1, 200, 1, std::nullopt},
                        {"depth", "トゲの長さ", 0.05, 1, 0.05, 0.45}, {"jitter", "ばらつき", 0, 1, 0.05, 0.25}}},
        {"white", {}},
    };
    return fields;
}
// 集中線と流線の、まとまり・乱れ
const std::vector<Field> kEffectMore{{"bundle", "まとまり（1 束の本数）", 1, 50, 1, 1}, {"bundle_gap", "束の間のすき間", 0, 0.95, 0.05, 0.5},
                                     {"jitter_length", "乱れ: 長さ", 0, 1, 0.05, std::nullopt},
                                     {"jitter_position", "乱れ: 位置", 0, 1, 0.05, std::nullopt},
                                     {"jitter_width", "乱れ: 太さ", 0, 1, 0.05, std::nullopt}};
const std::vector<std::pair<const char*, const char*>> kTapers{
    {"中心側・終わりを細く（入り）", "in"}, {"外側・始めを細く（抜き）", "out"}, {"両端を細く", "both"}, {"なし", "none"}};
// (流線 has no centre: its ends are where the lines flow to and where they come from)
const std::vector<std::pair<const char*, const char*>> kSpeedTapers{
    {"流れる先を細く", "in"}, {"流れてくる元を細く", "out"}, {"両端を細く", "both"}, {"なし", "none"}};

QString q(const std::string& text) { return QString::fromStdString(text); }

// float(x) for a value shown in a field (a value that is not a number: the fallback).
double number_or(const Json& value, double fallback) {
    try {
        return value.is_null() ? fallback : core::py_float(value);
    } catch (const core::Error&) {
        return fallback;
    }
}

std::filesystem::path library_config() { return config_dir(); }

QString error_text(const std::exception& error) { return wording::error(QString::fromUtf8(error.what())); }

}  // namespace

QString effect_label(const std::string& kind) {
    static const std::map<std::string, const char*> labels{
        {"focus", "集中線"}, {"speed", "流線"}, {"uni_flash", "ウニフラッシュ"}, {"beta_flash", "ベタフラッシュ"}, {"white", "白で塗る"}};
    const auto found = labels.find(kind);
    return found == labels.end() ? q(kind) : QString::fromUtf8(found->second);
}

QPixmap effect_picture(const std::string& kind) {
    static std::map<std::string, QPixmap> pictures;
    if (const auto found = pictures.find(kind); found != pictures.end()) return found->second;
    const QSize size(60, 44);
    const core::Document doc = core::new_episode("t", core::Num(1), 1, core::PageSpec::custom(72, 52, 72, 52, 0, 0, 0, 0, 0, 600));
    const core::Page& page = doc.page(0);
    const int dpi = static_cast<int>(core::py_round_int(2 * 25.4 * size.width() / page.spec.width_mm.value()));
    const core::Rect b = page.bleed_rect_mm();
    const double bx = b.x.value(), by = b.y.value(), bw = b.width.value(), bh = b.height.value();
    const Json params{{"center", Json::array({bx + bw / 2, by + bh / 2})}, {"count", kind != "speed" ? 60 : 26}, {"inner", Json::array({bw * 0.16, bh * 0.16})}};
    const int w = static_cast<int>(core::py_round_int(bw * dpi / 25.4)) + 2, h = static_cast<int>(core::py_round_int(bh * dpi / 25.4)) + 2;
    render::Image image = render::Image::create("RGBA", render::Size{w, h}, render::Ink{255, 255, 255, 255});
    image = render::effects::draw(std::move(image), Json{{"id", "sample-" + kind}, {"kind", kind}, {"params", params}}, page, dpi, render::Size{w, h},
                                  render::Box{0, 0, w, h});
    const render::Size twice = std::abs(static_cast<double>(w) / h - static_cast<double>(size.width()) / size.height()) < 0.05
                                   ? render::Size{size.width() * 2, size.height() * 2}
                                   : render::Size{size.width() * 2, static_cast<int>(core::py_round_int(size.width() * 2.0 * h / w))};
    image = image.resize(twice, render::Resample::Lanczos).crop(render::Box{0, 0, size.width() * 2, size.height() * 2}).convert("RGBA");
    const std::string raw = image.tobytes();
    QPixmap pixmap = QPixmap::fromImage(QImage(reinterpret_cast<const uchar*>(raw.data()), image.width(), image.height(), image.width() * 4,
                                               QImage::Format_RGBA8888).copy());
    pixmap.setDevicePixelRatio(2);
    pictures[kind] = pixmap;
    return pixmap;
}

void set_effect_pictures(const std::vector<QAction*>& actions, const std::vector<std::string>& kinds) {
    for (std::size_t i = 0; i < actions.size() && i < kinds.size(); ++i) {
        if (!actions[i]->icon().isNull()) continue;
        try {
            actions[i]->setIcon(QIcon(effect_picture(kinds[i])));
        } catch (const std::exception&) {  // (no picture: the name alone still works)
        }
    }
}

QWidget* effect_tiles(const std::vector<QAction*>& actions) {
    auto* box = new QWidget;
    auto* grid = new QGridLayout(box);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(6);
    for (std::size_t i = 0; i < actions.size(); ++i) {
        auto* tile = new QToolButton;
        tile->setObjectName(QStringLiteral("effectTile"));
        tile->setDefaultAction(actions[i]);
        tile->setIconSize(QSize(60, 44));
        tile->setMinimumWidth(40);
        tile->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        tile->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        grid->addWidget(tile, static_cast<int>(i / 2), static_cast<int>(i % 2));
    }
    return box;
}

MaterialPanel::MaterialPanel(MainWindow* window) : window_(window) {
    // --- materials
    browser = new MaterialBrowser(nullptr, [window](const QString& id, const QString& kind) { window->material_activated(id, kind); });
    auto* use_button = new QPushButton(QStringLiteral("貼る"));
    use_button->setToolTip(QStringLiteral("トーン: 選択範囲か選んだコマに（なければクリックした所に）。効果線: 選んだコマに。画像・パーツ: クリックした所に"));
    connect(use_button, &QPushButton::clicked, this, [this] { use(); });
    auto* more = new QGridLayout;
    const std::vector<std::tuple<QString, std::function<void()>, QString>> buttons{
        {QStringLiteral("画像を追加…"), [this] { import_image(); }, QStringLiteral("画像ファイルを素材にします")},
        {QStringLiteral("範囲を登録…"), [this] { register_selection(); }, QStringLiteral("選んだ範囲の線と塗りを素材（パーツ）にします")},
        {QStringLiteral("名前…"), [this] { rename(); }, QStringLiteral("名前とフォルダを変えます")},
        {QStringLiteral("消す"), [this] { remove(); }, QStringLiteral("自分で登録した素材を消します")},
        {QStringLiteral("タグ…"), [this] { edit_tags(); }, QStringLiteral("探すときの言葉（タグ）を付けます")},
        {QStringLiteral("素材パック…"), [this] { pack_menu(); }, QStringLiteral("素材パック（フォルダ・zip）を読み込む／選んだ素材を書き出す")}};
    int i = 0;
    for (const auto& [title, slot, tip] : buttons) {
        auto* button = new QPushButton(title);
        button->setObjectName(QStringLiteral("material:%1").arg(title));
        button->setToolTip(tip);
        connect(button, &QPushButton::clicked, this, slot);
        more->addWidget(button, i / 2, i % 2);
        ++i;
    }
    auto* new_folder_button = new QPushButton(QStringLiteral("＋フォルダ…"));
    new_folder_button->setToolTip(QStringLiteral("素材を分けるフォルダを作ります"));
    connect(new_folder_button, &QPushButton::clicked, this, [this] { new_folder(); });
    auto* mat_box = new QGroupBox(QStringLiteral("素材（ダブルクリックで貼る）"));
    auto* ml = new QVBoxLayout(mat_box);
    browser->add_beside_folder(new_folder_button);
    ml->addWidget(browser, 1);
    ml->addWidget(use_button);
    ml->addLayout(more);
    // --- the tone being worked on
    tone_label = new QLabel;
    tone_label->setWordWrap(true);
    pattern = new QComboBox;
    for (const auto& [label, key] : kPatterns) pattern->addItem(QString::fromUtf8(label), QString::fromLatin1(key));
    connect(pattern, &QComboBox::activated, this, [this](int) { pattern_chosen(); });
    scale = new QDoubleSpinBox;
    scale->setRange(0.3, 50);
    scale->setSuffix(QStringLiteral(" mm"));
    scale->setValue(3.0);
    scale->setToolTip(QStringLiteral("柄トーンの模様の大きさ（繰り返しの間隔）"));
    connect(scale, &QDoubleSpinBox::editingFinished, this, [this] { tone(Json{{"scale_mm", scale->value()}}); });
    lpi = new QDoubleSpinBox;
    lpi->setRange(5, 300);
    lpi->setSuffix(QStringLiteral(" 線"));
    connect(lpi, &QDoubleSpinBox::editingFinished, this, [this] { tone(Json{{"lpi", lpi->value()}}); });
    density = new QSpinBox;
    density->setRange(0, 100);
    density->setSuffix(QStringLiteral(" %"));
    connect(density, &QSpinBox::editingFinished, this, [this] { tone(Json{{"density", density->value() / 100.0}}); });
    angle = new QDoubleSpinBox;
    angle->setRange(-180, 180);
    angle->setSuffix(QStringLiteral("°"));
    connect(angle, &QDoubleSpinBox::editingFinished, this, [this] { tone(Json{{"angle", angle->value()}}); });
    dot_shape = new QComboBox;
    for (const auto& [label, key] : kScreenShapes) dot_shape->addItem(QString::fromUtf8(label), QString::fromLatin1(key));
    dot_shape->setToolTip(QStringLiteral("網点の形（網の種類）。四角は 50% で角どうしがつながる"));
    connect(dot_shape, &QComboBox::activated, this, [this](int) { tone(Json{{"dot_shape", dot_shape->currentData().toString().toStdString()}}); });
    off_x = new QDoubleSpinBox;
    off_y = new QDoubleSpinBox;
    offset_timer_ = new QTimer(this);
    offset_timer_->setSingleShot(true);
    offset_timer_->setInterval(700);
    connect(offset_timer_, &QTimer::timeout, this, [this] { offset_now(); });
    for (const auto& [spin, tip] : {std::pair{off_x, QStringLiteral("網を右へずらす（mm）")}, std::pair{off_y, QStringLiteral("網を下へずらす（mm）")}}) {
        spin->setRange(-20, 20);
        spin->setSingleStep(0.1);
        spin->setDecimals(2);
        spin->setSuffix(QStringLiteral(" mm"));
        spin->setToolTip(tip + QStringLiteral("。貼る場所はそのままで、網点の並びだけが動く（隣のトーンと網をそろえる・モアレを避ける）"));
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this](double) { offset_typed(); });  // (kept without Enter)
        connect(spin, &QDoubleSpinBox::editingFinished, this, [this] { offset_now(); });
    }
    auto* offset_row = new QHBoxLayout;
    offset_row->addWidget(off_x);
    offset_row->addWidget(off_y);
    gradient = new QComboBox;
    gradient->addItem(QStringLiteral("なし"), QString());
    gradient->addItem(QStringLiteral("直線"), QStringLiteral("linear"));
    gradient->addItem(QStringLiteral("円"), QStringLiteral("radial"));
    connect(gradient, &QComboBox::activated, this, [this](int) { gradient_changed(); });
    g_angle = new QDoubleSpinBox;
    g_angle->setRange(-180, 180);
    g_angle->setSuffix(QStringLiteral("°"));
    g_angle->setToolTip(QStringLiteral("90 で上から下へ"));
    g_start = new QSpinBox;
    g_end = new QSpinBox;
    for (QSpinBox* spin : {g_start, g_end}) {
        spin->setRange(0, 100);
        spin->setSuffix(QStringLiteral(" %"));
        connect(spin, &QSpinBox::editingFinished, this, [this] { gradient_changed(); });
    }
    connect(g_angle, &QDoubleSpinBox::editingFinished, this, [this] { gradient_changed(); });
    soft = new QCheckBox(QStringLiteral("消しゴムでぼかして削る"));
    auto* tone_form = new QFormLayout;
    tone_form->addRow(QString(), tone_label);
    tone_form->addRow(QStringLiteral("模様"), pattern);
    tone_form->addRow(QStringLiteral("柄の大きさ"), scale);
    tone_form->addRow(QStringLiteral("線数"), lpi);
    tone_form->addRow(QStringLiteral("濃さ"), density);
    tone_form->addRow(QStringLiteral("角度"), angle);
    tone_form->addRow(QStringLiteral("網の形"), dot_shape);
    tone_form->addRow(QStringLiteral("網のずれ"), offset_row);
    tone_form->addRow(QStringLiteral("グラデーション"), gradient);
    tone_form->addRow(QStringLiteral("　向き"), g_angle);
    tone_form->addRow(QStringLiteral("　始まり"), g_start);
    tone_form->addRow(QStringLiteral("　終わり"), g_end);
    tone_form->addRow(QString(), soft);
    tone_box = new QGroupBox(QStringLiteral("トーン（ペンで足す・消しゴムで削る）"));
    tone_box->setLayout(tone_form);
    // --- effect lines on the page
    effects = new QListWidget;
    effects->setObjectName(QStringLiteral("effectList"));
    effects->setMaximumHeight(96);
    connect(effects, &QListWidget::currentRowChanged, this, [this](int) { effect_picked(); });
    effect_form = new QFormLayout;
    auto* effect_buttons = new QHBoxLayout;
    for (const auto& [title, slot] : {std::pair<QString, std::function<void()>>{QStringLiteral("線にする"), [this] { effect_to_layer(); }},
                                      std::pair<QString, std::function<void()>>{QStringLiteral("消す"), [this] { delete_effect(); }}}) {
        auto* button = new QPushButton(title);
        button->setObjectName(QStringLiteral("effect:%1").arg(title));
        connect(button, &QPushButton::clicked, this, slot);
        effect_buttons->addWidget(button);
    }
    effect_box = new QGroupBox(QStringLiteral("このページの効果線（K）"));
    effect_box->setToolTip(QStringLiteral("効果線ツール（K）でコマの中をクリックすると入ります。中心の＋をドラッグで動かします"));
    auto* el = new QVBoxLayout(effect_box);
    el->addWidget(effects);
    el->addLayout(effect_form);
    el->addLayout(effect_buttons);
    // three pages in the panel, so none of them needs a long scroll
    tabs = new QTabWidget;
    for (const auto& [box, title] : {std::pair{mat_box, QStringLiteral("素材")}, std::pair{tone_box, QStringLiteral("トーン")},
                                     std::pair{effect_box, QStringLiteral("効果線")}}) {
        auto* page = new QWidget;
        auto* pl = new QVBoxLayout(page);
        pl->setContentsMargins(0, 0, 0, 0);
        box->setFlat(true);  // (the tab already frames it)
        box->layout()->setContentsMargins(2, 4, 2, 2);
        pl->addWidget(box, box == mat_box ? 1 : 0);
        if (box != mat_box) pl->addStretch(1);
        tabs->addTab(page, title);
    }
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(tabs);
}

// --- materials -----------------------------------------------------------------------------------------------------

void MaterialPanel::reload_library(const std::optional<QString>& select) {
    browser->reload();
    if (select) browser->select(*select);
}

std::optional<Json> MaterialPanel::current_material() const {
    const QString id = browser->current_id();
    if (id.isEmpty()) return std::nullopt;
    try {
        return render::materials::get_material(library_config(), id.toStdString());
    } catch (const core::Error&) {
        return std::nullopt;
    }
}

void MaterialPanel::select_material(const QString& material_id) { browser->select(material_id); }

void MaterialPanel::use() {
    const auto item = current_material();
    if (!item) {
        window_->flash(QStringLiteral("先に素材を選びます"), 2500);
        return;
    }
    window_->use_material(*item);
}

void MaterialPanel::import_image() {
    const QString path = ask::open_path(this, QStringLiteral("画像を素材に取り込む"),
                                        QStringLiteral("画像 (*.png *.jpg *.jpeg *.webp *.bmp *.tif *.tiff)"));
    if (path.isEmpty()) return;
    const QString folder = browser->folder().isEmpty() ? QStringLiteral("画像") : browser->folder();
    Json item;
    try {
        item = render::materials::import_image(library_config(), core::path_from_utf8(path.toStdString()), Json(), Json(folder.toStdString()));
    } catch (const std::exception& error) {
        window_->flash(QStringLiteral("取り込めませんでした（%1）").arg(error_text(error)), 6000, true);
        return;
    }
    reload_library(q(item["id"].get<std::string>()));
    window_->flash(QStringLiteral("「%1」を素材にしました").arg(q(core::py_str(item["name"]))), 2500);
}

void MaterialPanel::register_selection() {
    const auto items = window_->copy_selection_items();
    if (!items) return;
    const auto name = ask::get_text(this, QStringLiteral("素材に登録"), QStringLiteral("名前"));
    if (!name || name->trimmed().isEmpty()) return;
    const QString folder = browser->folder().isEmpty() ? QStringLiteral("マイ素材") : browser->folder();
    Json item;
    try {
        item = render::materials::add_material(library_config(), Json(name->toStdString()), "lines", Json(folder.toStdString()),
                                               Json{{"items", *items}});
    } catch (const std::exception& error) {
        window_->flash(QStringLiteral("登録できませんでした（%1）").arg(error_text(error)), 6000, true);
        return;
    }
    reload_library(q(item["id"].get<std::string>()));
    window_->flash(QStringLiteral("「%1」を素材にしました（素材 → 貼る）").arg(q(item["name"].get<std::string>())), 3000);
}

void MaterialPanel::rename() {
    const auto item = current_material();
    if (!item || core::py_truthy(core::py_get(*item, "builtin"))) {
        window_->flash(QStringLiteral("名前を変えられるのは自分で登録した素材です"), 2500);
        return;
    }
    const auto name = ask::get_text(this, QStringLiteral("素材の名前"), QStringLiteral("名前"), q(core::py_str(core::py_get(*item, "name", ""))));
    if (!name) return;
    QStringList folders;
    for (const std::string& f : render::materials::folders(library_config())) folders << q(f);
    const QString now = q(core::py_str(core::py_get(*item, "folder", "")));
    const auto folder = ask::get_item(this, QStringLiteral("素材のフォルダ"), QStringLiteral("フォルダ"), folders,
                                      std::max(0, static_cast<int>(folders.indexOf(now))), true);
    if (!folder) return;
    try {
        const Json fallback_folder = item->contains("folder") ? (*item)["folder"] : Json("マイ素材");
        render::materials::update_material(library_config(), (*item)["id"].get<std::string>(),
                                           Json{{"name", name->isEmpty() ? (*item)["name"] : Json(name->toStdString())},
                                                {"folder", folder->isEmpty() ? fallback_folder : Json(folder->toStdString())}});
    } catch (const std::exception& error) {
        window_->flash(QStringLiteral("変えられませんでした（%1）").arg(error_text(error)), 6000, true);
        return;
    }
    reload_library(q((*item)["id"].get<std::string>()));
}

void MaterialPanel::edit_tags() {
    const auto item = current_material();
    if (!item || core::py_truthy(core::py_get(*item, "builtin"))) {
        window_->flash(QStringLiteral("タグを付けられるのは自分で登録した素材です（入っている素材にはタグが付いています）"), 4000);
        return;
    }
    QStringList now;
    const Json tags = core::py_get(*item, "tags");
    if (core::py_truthy(tags) && tags.is_array()) {
        for (const Json& tag : tags) now << q(core::py_str(tag));
    }
    const auto text = ask::get_text(this, QStringLiteral("素材のタグ"), QStringLiteral("タグ（、で区切る）"), now.join(QStringLiteral("、")));
    if (!text) return;
    Json words = Json::array();
    for (const QString& word : text->split(QRegularExpression(QStringLiteral("[、,，\\s]+")), Qt::SkipEmptyParts)) words.push_back(word.toStdString());
    try {
        render::materials::update_material(library_config(), (*item)["id"].get<std::string>(), Json{{"tags", words}});
    } catch (const std::exception& error) {
        window_->flash(QStringLiteral("タグを付けられませんでした（%1）").arg(error_text(error)), 6000, true);
        return;
    }
    reload_library(q((*item)["id"].get<std::string>()));
}

void MaterialPanel::pack_menu() {
    QMenu menu(this);
    menu.addAction(QStringLiteral("素材パック（zip）を読み込む…"), this, [this] { import_pack(true); });
    menu.addAction(QStringLiteral("フォルダを素材パックとして読み込む…"), this, [this] { import_pack(false); });
    menu.addSeparator();
    menu.addAction(QStringLiteral("このフォルダの自分の素材を書き出す…"), this, [this] { export_pack(); });
    menu.exec(QCursor::pos());
}

void MaterialPanel::import_pack(bool zip_file) {
    const QString path = zip_file ? ask::open_path(this, QStringLiteral("素材パックを読み込む"), QStringLiteral("素材パック (*.zip)"))
                                  : ask::existing_dir(this, QStringLiteral("素材パックのフォルダ"));
    if (path.isEmpty()) return;
    Json added;
    try {
        added = render::materials::import_pack(library_config(), core::path_from_utf8(path.toStdString()));
    } catch (const std::exception& error) {
        reload_library();  // (what came in before the error stays, as in Python)
        window_->flash(QStringLiteral("読み込めませんでした（%1）").arg(error_text(error)), 6000, true);
        return;
    }
    reload_library();
    window_->flash(QStringLiteral("素材を %1 個読み込みました").arg(added.size()), 4000);
}

void MaterialPanel::export_pack() {
    const QString folder = browser->folder();
    std::vector<std::string> mine;
    for (const Json& item : render::materials::user_materials(library_config())) {
        if (folder.isEmpty() || core::py_get(item, "folder") == folder.toStdString()) mine.push_back(core::py_str(item["id"]));
    }
    if (mine.empty()) {
        window_->flash(QStringLiteral("書き出せる自分の素材がありません（フォルダを選び直します）"), 4000);
        return;
    }
    const QString path = ask::save_path(this, QStringLiteral("素材パックを書き出す"),
                                        (folder.isEmpty() ? QStringLiteral("素材") : folder) + QStringLiteral(".zip"),
                                        QStringLiteral("素材パック (*.zip)"));
    if (path.isEmpty()) return;
    try {
        render::materials::export_pack(library_config(), mine, core::path_from_utf8(path.toStdString()));
    } catch (const std::exception& error) {
        window_->flash(QStringLiteral("書き出せませんでした（%1）").arg(error_text(error)), 6000, true);
        return;
    }
    window_->flash(QStringLiteral("素材 %1 個を書き出しました").arg(mine.size()), 4000);
}

void MaterialPanel::remove() {
    const auto item = current_material();
    if (!item) return;
    if (core::py_truthy(core::py_get(*item, "builtin"))) {
        window_->flash(QStringLiteral("最初から入っている素材は消せません"), 2500);
        return;
    }
    if (!ask::question(this, QStringLiteral("Genko"),
                       QStringLiteral("素材「%1」を消しますか？（原稿に貼ったものは残ります）").arg(q(core::py_str(core::py_get(*item, "name", "")))))) {
        return;
    }
    try {
        render::materials::delete_material(library_config(), (*item)["id"].get<std::string>());
    } catch (const std::exception& error) {
        window_->flash(QStringLiteral("消せませんでした（%1）").arg(error_text(error)), 6000, true);
        return;
    }
    reload_library();
}

void MaterialPanel::new_folder() {
    const auto name = ask::get_text(this, QStringLiteral("フォルダを作る"), QStringLiteral("フォルダの名前"));
    if (!name || name->trimmed().isEmpty()) return;
    const std::string clean = core::py_strip(name->toStdString());
    try {
        render::materials::add_folder(library_config(), name->toStdString());
    } catch (const std::exception& error) {
        window_->flash(QStringLiteral("フォルダを作れませんでした（%1）").arg(error_text(error)), 6000, true);
        return;
    }
    reload_library();
    browser->choose_folder(q(clean));
}

// --- the tone ------------------------------------------------------------------------------------------------------

const core::Layer* MaterialPanel::tone_layer() const {
    const core::Layer* layer = window_->target_layer();
    return layer != nullptr && layer->kind == core::LayerKind::Tone ? layer : nullptr;
}

void MaterialPanel::refresh() {
    if (offset_timer_->isActive()) offset_now();  // (a shift typed a moment ago is kept before the fields are reloaded)
    loading_ = true;
    const core::Layer* layer = tone_layer();
    tone_box->setEnabled(layer != nullptr);
    if (layer == nullptr) {
        tone_label->setText(QStringLiteral("レイヤー パネルでトーンのレイヤーを選ぶと、ここで変えられます"));
    } else {
        const render::tones::Settings tone = render::tones::settings(*layer);
        tone_label->setText(QStringLiteral("「%1」").arg(layer->title.empty() ? QStringLiteral("トーン") : q(layer->title)));
        const QString key = tone.pattern.is_string() ? q(tone.pattern.get<std::string>()) : QString();
        pattern->setCurrentIndex(std::max(0, pattern->findData(key)));
        scale->setValue(tone.scale_mm && core::py_truthy(*tone.scale_mm) ? number_or(*tone.scale_mm, 3.0) : 3.0);
        const auto& motifs = render::tones::motifs();
        scale->setEnabled(std::find(motifs.begin(), motifs.end(), key.toStdString()) != motifs.end());
        lpi->setValue(tone.lpi);
        density->setValue(static_cast<int>(core::py_round_int(tone.density * 100)));
        angle->setValue(tone.angle);
        const QString shape = tone.dot_shape && core::py_truthy(*tone.dot_shape) ? q(core::py_str(*tone.dot_shape)) : QStringLiteral("round");
        dot_shape->setCurrentIndex(std::max(0, dot_shape->findData(shape)));
        dot_shape->setEnabled(key == QLatin1String("dot"));
        double ox = 0, oy = 0;
        if (tone.offset_mm && core::py_truthy(*tone.offset_mm) && tone.offset_mm->is_array() && tone.offset_mm->size() >= 2) {
            ox = number_or((*tone.offset_mm)[0], 0);
            oy = number_or((*tone.offset_mm)[1], 0);
        }
        off_x->setValue(ox);
        off_y->setValue(oy);
        const Json g = core::py_truthy(tone.gradient) && tone.gradient.is_object() ? tone.gradient : Json::object();
        gradient->setCurrentIndex(std::max(0, gradient->findData(q(core::py_str(core::py_get(g, "shape", ""))))));
        g_angle->setValue(number_or(core::py_get(g, "angle", 90), 90));
        g_start->setValue(static_cast<int>(core::py_round_int(number_or(core::py_get(g, "start", tone.density), tone.density) * 100)));
        g_end->setValue(static_cast<int>(core::py_round_int(number_or(core::py_get(g, "end", 0), 0) * 100)));
    }
    const bool shaped = !gradient->currentData().toString().isEmpty();
    for (QWidget* widget : std::initializer_list<QWidget*>{g_angle, g_start, g_end}) widget->setEnabled(shaped);
    loading_ = false;
    fill_effects();
}

void MaterialPanel::pattern_chosen() {
    const QString key = pattern->currentData().toString();
    if (key != QLatin1String("image")) {
        tone(Json{{"pattern", key.toStdString()}});
        return;
    }
    const QString path = ask::open_path(this, QStringLiteral("柄にする画像（白地に黒い模様）"), QStringLiteral("画像 (*.png *.jpg *.jpeg *.bmp *.webp)"));
    if (path.isEmpty()) {
        refresh();
        return;
    }
    std::string png;
    try {
        std::ifstream file(core::path_from_utf8(path.toStdString()), std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (!file.good() && !file.eof()) throw core::Error("io", "the picture cannot be read");
        render::Image image = render::selection::open_picture(bytes).convert("LA");
        image.thumbnail(render::Size{512, 512});
        png = render::write_png(image);
    } catch (const std::exception& error) {
        window_->flash(QStringLiteral("読み込めない画像です:\n%1").arg(error_text(error)), 6000, true);
        refresh();
        return;
    }
    tone(Json{{"pattern", "image"}, {"tile_png", core::b64encode(png)}});
}

void MaterialPanel::tone(const Json& change) {
    const core::Layer* layer = tone_layer();
    if (loading_ || layer == nullptr || window_->current_page() == nullptr) return;
    Json op{{"op", "set_tone"}, {"page", window_->current_page()->index.json()}, {"id", layer->id}};
    for (const auto& [key, value] : change.items()) op[key] = value;
    window_->apply_ops(Json::array({op}));
}

void MaterialPanel::offset_typed() {
    if (loading_) return;
    const core::Layer* layer = tone_layer();
    offset_typed_ = true;
    offset_layer_ = layer != nullptr ? std::optional<std::string>(layer->id) : std::nullopt;
    offset_timer_->start();
}

void MaterialPanel::offset_now() {
    offset_timer_->stop();
    const core::Layer* layer = tone_layer();
    if (layer == nullptr || (offset_typed_ && offset_layer_ != layer->id)) return;
    const double x = core::py_round(off_x->value(), 2), y = core::py_round(off_y->value(), 2);
    const render::tones::Settings now = render::tones::settings(*layer);
    double ox = 0, oy = 0;
    if (now.offset_mm && core::py_truthy(*now.offset_mm) && now.offset_mm->is_array() && now.offset_mm->size() >= 2) {
        ox = core::py_round(number_or((*now.offset_mm)[0], 0), 2);
        oy = core::py_round(number_or((*now.offset_mm)[1], 0), 2);
    }
    if (ox != x || oy != y) tone(Json{{"offset_mm", Json::array({x, y})}});
}

void MaterialPanel::gradient_changed() {
    const QString shape = gradient->currentData().toString();
    for (QWidget* widget : std::initializer_list<QWidget*>{g_angle, g_start, g_end}) widget->setEnabled(!shape.isEmpty());
    if (shape.isEmpty()) {
        tone(Json{{"gradient", nullptr}});
        return;
    }
    if (g_start->value() == g_end->value()) {
        loading_ = true;
        g_start->setValue(density->value());
        g_end->setValue(0);
        loading_ = false;
    }
    tone(Json{{"gradient", Json{{"shape", shape.toStdString()}, {"angle", g_angle->value()}, {"start", g_start->value() / 100.0},
                                {"end", g_end->value() / 100.0}}}});
}

// --- effect lines --------------------------------------------------------------------------------------------------

void MaterialPanel::fill_effects() {
    const core::Page* page = window_->current_page();
    const std::optional<std::string> keep = window_->canvas()->selected_effect_id;
    loading_ = true;
    effects->clear();
    if (page != nullptr) {
        for (const Json& effect : page->effects) {
            if (!effect.is_object()) continue;
            QString label = effect_label(core::py_str(core::py_get(effect, "kind", "")));
            if (core::py_get(effect, "visible") == Json(false)) label += QStringLiteral("（隠す）");
            auto* entry = new QListWidgetItem(label);
            const std::string id = core::py_str(core::py_get(effect, "id", ""));
            entry->setData(Qt::UserRole, q(id));
            effects->addItem(entry);
            if (keep && *keep == id) effects->setCurrentItem(entry);
        }
    }
    loading_ = false;
    show_effect();
}

const Json* MaterialPanel::effect() const {
    const core::Page* page = window_->current_page();
    const QListWidgetItem* entry = effects->currentItem();
    if (page == nullptr || entry == nullptr) return nullptr;
    const std::string id = entry->data(Qt::UserRole).toString().toStdString();
    for (const Json& effect : page->effects) {
        if (effect.is_object() && core::py_str(core::py_get(effect, "id", "")) == id) return &effect;
    }
    return nullptr;
}

void MaterialPanel::select_effect(const std::string& effect_id) {
    for (int row = 0; row < effects->count(); ++row) {
        if (effects->item(row)->data(Qt::UserRole).toString().toStdString() == effect_id) {
            effects->setCurrentRow(row);
            return;
        }
    }
}

void MaterialPanel::effect_picked() {
    if (loading_) return;
    const Json* chosen = effect();
    window_->canvas()->selected_effect_id = chosen != nullptr ? std::optional<std::string>(core::py_str(core::py_get(*chosen, "id", ""))) : std::nullopt;
    window_->canvas()->update();
    show_effect();
}

void MaterialPanel::show_effect() {
    // (the fields go later, not at once: the one being edited may be the one whose signal brought us here)
    while (effect_form->rowCount() > 0) {
        const QFormLayout::TakeRowResult row = effect_form->takeRow(0);
        for (QLayoutItem* item : {row.labelItem, row.fieldItem}) {
            if (item == nullptr) continue;
            if (QWidget* widget = item->widget()) {
                widget->hide();
                widget->setParent(nullptr);  // (out of the panel at once: never found again)
                widget->deleteLater();
            }
            delete item;
        }
    }
    effect_fields.clear();
    const Json* found = effect();
    const core::Page* page = window_->current_page();
    if (found == nullptr || page == nullptr) return;
    const Json effect_now = *found;  // (the fields keep their own copy: the book may change under them)
    const Json params = core::py_truthy(core::py_get(effect_now, "params")) && effect_now["params"].is_object() ? effect_now["params"] : Json::object();
    std::array<double, 4> box{};
    try {
        box = render::effects::panel_area(effect_now, *page).second;
    } catch (const std::exception&) {
        return;
    }
    const std::string kind = core::py_str(core::py_get(effect_now, "kind", ""));
    std::vector<Field> fields;
    if (const auto it = effect_fields_of().find(kind); it != effect_fields_of().end()) fields = it->second;
    if (kind == "focus" || kind == "speed") fields.insert(fields.end(), kEffectMore.begin(), kEffectMore.end());
    for (const Field& field : fields) {
        auto* spin = new QDoubleSpinBox;
        spin->setRange(field.lo, field.hi);
        spin->setSingleStep(field.step);
        spin->setDecimals(field.step >= 1 ? 0 : 2);
        const std::string key = field.key;
        double value = 0;
        if (key == "inner_r") {
            try {
                value = render::effects::inner_size(params, box).x;
            } catch (const std::exception&) {
                value = field.lo;
            }
        } else if (key == "length_mm" && kind == "focus") {
            value = number_or(core::py_truthy(core::py_get(params, key)) ? params[key] : Json(0), 0);
        } else if (key == "length_mm") {
            const double fallback = std::max(8.0, std::min(box[2], box[3]) * 0.18);
            value = number_or(core::py_get(params, key, fallback), fallback);
        } else if (key.starts_with("jitter_")) {
            const double jitter = number_or(core::py_get(params, "jitter", 0.25), 0.25);
            value = number_or(core::py_get(params, key, jitter), jitter);
        } else {
            const double fallback = field.fallback.value_or(field.lo);
            value = number_or(core::py_get(params, key, fallback), fallback);
        }
        spin->setValue(value);
        connect(spin, &QDoubleSpinBox::editingFinished, this, [this, key, spin] { effect_set(key, spin->value()); });
        effect_form->addRow(QString::fromUtf8(field.label), spin);
        effect_fields[key] = spin;
    }
    if (kind == "speed" || kind == "focus") {
        auto* taper = new QComboBox;
        for (const auto& [text, value] : kind == "speed" ? kSpeedTapers : kTapers) taper->addItem(QString::fromUtf8(text), QString::fromLatin1(value));
        const Json now = core::py_get(params, "taper", true);
        QString chosen;
        if (now.is_null() || now == Json(true) || now == Json(1) || now == Json("True")) chosen = kind == "focus" ? QStringLiteral("in") : QStringLiteral("both");
        else if (now == Json(false) || now == Json(0) || now == Json("") || now == Json("none")) chosen = QStringLiteral("none");
        else chosen = q(core::py_str(now));
        taper->setCurrentIndex(std::max(0, taper->findData(chosen)));
        connect(taper, &QComboBox::activated, this, [this, taper](int) { effect_set("taper", taper->currentData().toString().toStdString()); });
        effect_form->addRow(QStringLiteral("入り抜き"), taper);
        effect_fields["taper"] = taper;
        const std::string key = kind == "speed" ? "path" : "inner_path";
        const std::string id = core::py_str(core::py_get(effect_now, "id", ""));
        auto* draw = new QPushButton(kind == "speed" ? QStringLiteral("描いた線に沿わせる") : QStringLiteral("中心の空きを描いた形にする"));
        draw->setObjectName(QStringLiteral("effect:draw"));
        draw->setToolTip(QStringLiteral("ボタンを押してから、ペンで 1 本引きます（流線はその線に沿い、集中線はその形の外から入る）"));
        connect(draw, &QPushButton::clicked, this, [this, id, key] { window_->draw_effect_shape(id, key); });
        effect_form->addRow(draw);
        if (core::py_truthy(core::py_get(params, key))) {
            auto* undo = new QPushButton(kind == "speed" ? QStringLiteral("沿わせるのをやめる") : QStringLiteral("中心の空きを楕円に戻す"));
            undo->setObjectName(QStringLiteral("effect:undraw"));
            connect(undo, &QPushButton::clicked, this, [this, id, key] {
                const core::Page* shown = window_->current_page();
                if (shown == nullptr) return;
                window_->apply_ops(Json::array({Json{{"op", "edit_effect"}, {"page", shown->index.json()}, {"id", id}, {"params", Json{{key, nullptr}}}}}));
            });
            effect_form->addRow(undo);
        }
    }
}

void MaterialPanel::effect_set(const std::string& key, const Json& value) {
    const Json* found = effect();
    const core::Page* page = window_->current_page();
    if (found == nullptr || page == nullptr) return;
    const Json effect_now = *found;
    const std::string kind = core::py_str(core::py_get(effect_now, "kind", ""));
    Json change;
    if (key == "inner_r") {
        const Json params = core::py_truthy(core::py_get(effect_now, "params")) && effect_now["params"].is_object() ? effect_now["params"] : Json::object();
        try {
            const auto box = render::effects::panel_area(effect_now, *page).second;
            const render::effects::XY r = render::effects::inner_size(params, box);
            const double v = value.get<double>();
            change = Json{{"inner", Json::array({core::py_round(v, 2), core::py_round(v * r.y / std::max(r.x, 1e-6), 2)})}};
        } catch (const std::exception& error) {
            window_->flash(error_text(error), 4000, true);
            return;
        }
    } else if (key == "count" || key == "spikes" || key == "bundle") {
        change = Json{{key, static_cast<std::int64_t>(value.get<double>())}};
    } else if (key == "taper") {
        change = Json{{key, value == Json("none") ? Json(false) : value}};
    } else if ((key == "length_mm" || key == "spacing_mm") && value.is_number() && value.get<double>() == 0 && (kind == "focus" || kind == "speed")) {
        change = Json{{key, nullptr}};  // (0: to the edge / from the count)
    } else {
        change = Json{{key, core::py_round(value.get<double>(), 3)}};
    }
    window_->apply_ops(Json::array({Json{{"op", "edit_effect"}, {"page", page->index.json()}, {"id", core::py_get(effect_now, "id")}, {"params", change}}}));
}

void MaterialPanel::effect_to_layer() {
    const Json* found = effect();
    const core::Layer* layer = window_->paint_layer();
    const core::Page* page = window_->current_page();
    if (found == nullptr || layer == nullptr || page == nullptr) return;
    if (window_->apply_ops(Json::array({Json{{"op", "effect_to_layer"}, {"page", page->index.json()}, {"id", core::py_get(*found, "id")},
                                             {"layer_id", layer->id}}}))) {
        window_->flash(QStringLiteral("効果線を線にしました。消しゴムやペンで手を入れられます"), 3500);
    }
}

void MaterialPanel::delete_effect() {
    const Json* found = effect();
    const core::Page* page = window_->current_page();
    if (found == nullptr || page == nullptr) return;
    const Json id = core::py_get(*found, "id");
    window_->apply_ops(Json::array({Json{{"op", "delete_effect"}, {"page", page->index.json()}, {"id", id}}}));
    window_->canvas()->selected_effect_id.reset();
}

}  // namespace genko::app
