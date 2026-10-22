#include "app/layer_panel.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "app/adjust_forms.hpp"
#include "app/ask.hpp"
#include "app/canvas.hpp"
#include "app/config.hpp"
#include "app/icons.hpp"
#include "app/layer_dialogs.hpp"
#include "app/main_window.hpp"
#include "app/preferences.hpp"
#include "app/theme.hpp"
#include "app/comfort.hpp"
#include "service/wording.hpp"
#include "core/adjust.hpp"
#include "core/ids.hpp"
#include "core/pyconv.hpp"
#include "core/smart.hpp"
#include "render/ink.hpp"
#include "render/plugins.hpp"
#include "render/page.hpp"

namespace genko::app {

using core::Json;

namespace {

QString kind_name(core::LayerKind kind) {
    switch (kind) {
    case core::LayerKind::Strokes: return QStringLiteral("ペンのレイヤー");
    case core::LayerKind::Raster: return QStringLiteral("ペイントのレイヤー");
    case core::LayerKind::Folder: return QStringLiteral("フォルダ");
    case core::LayerKind::Placed: return QStringLiteral("画像");
    case core::LayerKind::Tone: return QStringLiteral("トーン");
    case core::LayerKind::Fill: return QStringLiteral("塗り");
    case core::LayerKind::Adjust: return QStringLiteral("色調補正");
    case core::LayerKind::Smart: return QStringLiteral("スマートオブジェクト");
    default: return {};
    }
}

const std::vector<std::pair<QString, Json>>& tints() {
    static const std::vector<std::pair<QString, Json>> all{
        {QStringLiteral("表示色: そのまま"), Json()}, {QStringLiteral("表示色: 青"), Json::array({40, 110, 230})},
        {QStringLiteral("表示色: 赤"), Json::array({220, 50, 50})}, {QStringLiteral("表示色: 緑"), Json::array({40, 150, 70})},
        {QStringLiteral("表示色: 灰"), Json::array({150, 150, 150})}};
    return all;
}

// What a layer's small picture shows besides its lines and pixels (each kind's settings, its patches' places).
std::string thumb_state(const core::Layer& layer) {
    const auto dump = [](const std::optional<Json>& value) { return value ? value->dump() : std::string("-"); };
    std::string out = std::to_string(static_cast<int>(layer.kind)) + "|" + dump(layer.fill) + "|" + dump(layer.tone) + "|" + dump(layer.screen) +
                      "|" + dump(layer.effect) + "|" + dump(layer.adjust) + "|" + layer.asset.value_or("") + "|" + layer.material_id.value_or("") +
                      "|" + (layer.mask && layer.mask->enabled ? "mask" : "") + "|" + std::to_string(layer.opacity);
    for (const core::Patch& patch : layer.patches) out += "|" + patch.attrs.dump();
    out += "|" + dump(layer.smart);  // (smart-objects@1: its settings)
    return out;
}

QPushButton* button(const QString& text, const QString& tip, const QString& name) {
    auto* b = new QPushButton(text);
    b->setToolTip(tip);
    b->setObjectName(name);
    return b;
}

// A layer's row (Python's _LayerRow): the eye, a framed picture of what it holds, its name, and on the right its marks,
// opacity and blend (only when not the usual); the chosen row has the accent bar on its left edge.
class LayerRow : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

protected:
    void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override {
        QStyledItemDelegate::initStyleOption(option, index);
        const QString name = index.data(Qt::UserRole + 3).toString();
        if (!name.isEmpty()) option->text = name;
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        const QString meta = index.data(Qt::UserRole + 2).toString();
        const theme::Tokens t = theme::tokens();
        const int width = meta.isEmpty() ? 0 : opt.fontMetrics.horizontalAdvance(meta) + 12;
        if (!meta.isEmpty()) opt.rect = option.rect.adjusted(0, 0, -width, 0);
        QStyle* style = opt.widget != nullptr ? opt.widget->style() : QApplication::style();
        painter->save();
        const bool chosen = (option.state & QStyle::State_Selected) != 0;
        if (chosen) painter->fillRect(option.rect.adjusted(0, 1, 0, -1), QColor(t.selected));  // (the whole row lit, then the bar)
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);
        const QRect picture = style->subElementRect(QStyle::SE_ItemViewItemDecoration, &opt, opt.widget);
        if (index.data(Qt::UserRole + 1).toString() == QLatin1String("picture") && picture.isValid()) {
            painter->setPen(QColor(t.divider));
            painter->drawRect(picture.adjusted(0, 0, -1, -1));
        }
        if (chosen) painter->fillRect(QRect(option.rect.left(), option.rect.top() + 6, 3, option.rect.height() - 12), QColor(t.accent));
        if (!meta.isEmpty()) {
            painter->setPen(QColor(t.muted));
            painter->drawText(option.rect.adjusted(0, 0, -8, 0), Qt::AlignRight | Qt::AlignVCenter, meta);
        }
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        const QSize hint = QStyledItemDelegate::sizeHint(option, index);
        return QSize(hint.width(), std::max(hint.height(), 44));
    }
};

}  // namespace

LayerPanel::LayerPanel(MainWindow* window) : window_(window) {
    setObjectName(QStringLiteral("layer_panel"));
    target_ = new QLabel;
    target_->setWordWrap(true);
    list_ = new QListWidget;
    list_->setObjectName(QStringLiteral("layerList"));
    list_->setUniformItemSizes(true);
    list_->setMinimumHeight(96);
    list_->setSelectionMode(QAbstractItemView::ExtendedSelection);  // Ctrl / Shift+click: several
    list_->setIconSize(QSize(30, 38));
    list_->setItemDelegate(new LayerRow(list_));
    connect(list_, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) { visibility(item); });
    connect(list_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        const int kind = row_kind(list_->row(item));  // (PC3 A2: a smart filter's row — its window; the header: nothing)
        if (kind >= 0) edit_smart_filter(kind);
        else if (kind == kLayerRow) edit_special();
    });
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(list_, &QListWidget::customContextMenuRequested, this, [this](const QPoint& at) { smart_filter_menu(at); });
    connect(list_, &QListWidget::currentRowChanged, this, [this](int) { selected(); });
    search_ = new QLineEdit;
    search_->setPlaceholderText(QStringLiteral("レイヤーを探す（名前）"));
    search_->setClearButtonEnabled(true);
    connect(search_, &QLineEdit::textChanged, this, [this](const QString& text) { search(text); });
    connect(search_, &QLineEdit::returnPressed, this, [this] { choose_found(); });  // (PC3 B2: レイヤーを検索)
    name_ = new QLineEdit;
    name_->setObjectName(QStringLiteral("layer_name"));
    name_->setPlaceholderText(QStringLiteral("レイヤーの名前"));
    connect(name_, &QLineEdit::editingFinished, this, [this] {
        const auto [page, layer] = this->layer();
        const QString name = name_->text().trimmed();
        if (layer != nullptr && !name.isEmpty() && name != wording::layer_label(*layer))
            window_->apply_ops(Json::array({Json{{"op", "set_layer"}, {"page", page->index.json()}, {"id", layer->id}, {"name", name.toStdString()}}}));
    });
    opacity_ = new QSlider(Qt::Horizontal);
    opacity_->setObjectName(QStringLiteral("layer_opacity"));
    opacity_->setRange(0, 100);
    connect(opacity_, &QSlider::sliderReleased, this, [this] { set("opacity", opacity_->value() / 100.0); });
    blend_ = new QComboBox;
    blend_->setObjectName(QStringLiteral("layer_blend"));
    for (const auto& [key, label] : blend_modes()) blend_->addItem(label, QString::fromStdString(key));
    connect(blend_, &QComboBox::activated, this, [this](int) {
        const auto [page, layer] = this->layer();
        const std::string blend = blend_->currentData().toString().toStdString();
        // (a correction layer is drawn by its native.blend — the user's decision 2 —; its own blend is not looked at)
        if (layer != nullptr && !loading_ && layer->kind == core::LayerKind::Adjust && layer->adjust && layer->adjust->is_object()) {
            if (blend != core::native_of(*layer->adjust).blend)
                window_->apply_ops(Json::array({Json{{"op", "set_adjustment"}, {"page", page->index.json()}, {"id", layer->id},
                                                     {"native", Json{{"blend", blend == "normal" ? Json() : Json(blend)}}}}}));
            return;
        }
        set("blend", blend);
    });
    const auto box = [this](const QString& text, const char* name, std::function<void(bool)> on, const QString& tip = {}) {
        auto* b = new QCheckBox(text);
        b->setObjectName(QString::fromLatin1(name));
        if (!tip.isEmpty()) b->setToolTip(tip);
        connect(b, &QCheckBox::clicked, this, std::move(on));
        return b;
    };
    clip_ = box(QStringLiteral("下のレイヤーでクリップ"), "layer_clip", [this](bool on) { set("clip", on); });
    protect_ = box(QStringLiteral("透明部分を保護"), "layer_lock_alpha", [this](bool on) { set("lock_alpha", on); });
    locked_ = box(QStringLiteral("ロック（描けなくする）"), "layer_locked", [this](bool on) { set("locked", on); });
    overhang_ = box(QStringLiteral("コマの外にもはみ出す"), "layer_overhang", [this](bool on) { set("panel_clip", !on); },
                    QStringLiteral("このレイヤーの線を、コマの枠で切らずに間の白や外まで描きます"));
    each_panel_ = box(QStringLiteral("描き始めたコマの中だけに描く"), "layer_each_panel", [this](bool on) { set("panel_each", on); },
                      QStringLiteral("線を、描き始めたコマの枠で切ります（となりのコマにはみ出さない）。外すと、どのコマの中にも描けます"));
    draft_ = box(QStringLiteral("下描き（書き出さない）"), "layer_draft", [this](bool on) { set("exportable", !on); },
                 QStringLiteral("画面には見えますが、書き出し・印刷には出ません"));
    reference_ = box(QStringLiteral("参照にする"), "layer_reference", [this](bool on) { set("reference", on); },
                     QStringLiteral("塗りつぶしの「見る範囲: 参照レイヤー」で、このレイヤーの線を見て塗ります。線画を参照にすれば、塗りは別のレイヤーに入れられます"));
    tint_ = new QComboBox;
    tint_->setObjectName(QStringLiteral("layer_tint"));
    for (std::size_t i = 0; i < tints().size(); ++i) tint_->addItem(tints()[i].first, static_cast<int>(i));
    tint_->setToolTip(QStringLiteral("画面でだけ、このレイヤーをこの色で見ます（印刷は元の色）"));
    connect(tint_, &QComboBox::activated, this, [this](int i) { set("color", tints()[static_cast<std::size_t>(std::max(0, i))].second); });

    // adding, taking away, moving, combining
    auto* add_pen = button(QStringLiteral("＋ペン"), QStringLiteral("線を描くレイヤー（線はあとから消しゴムで切れる）"), QStringLiteral("layer_add_pen"));
    connect(add_pen, &QPushButton::clicked, this, [this] { add("pen", QStringLiteral("ペン")); });
    auto* add_paint = button(QStringLiteral("＋ペイント"), QStringLiteral("塗りや画像のレイヤー"), QStringLiteral("layer_add_paint"));
    connect(add_paint, &QPushButton::clicked, this, [this] { add("paint", QStringLiteral("ペイント")); });
    auto* add_folder = button(QStringLiteral("＋フォルダ"), QStringLiteral("フォルダを足す（レイヤーをまとめる）"), QStringLiteral("layer_add_folder"));
    connect(add_folder, &QPushButton::clicked, this, [this] { add("folder", QStringLiteral("フォルダ")); });
    auto* add_special_button = button(QStringLiteral("＋塗り・補正 ▾"),
                                      QStringLiteral("ベタ塗り・グラデーション・色調補正のレイヤー（あとから何度でも直せます。ダブルクリックで直す）"),
                                      QStringLiteral("layer_add_special"));
    special_ = new QMenu(add_special_button);
    special_->addAction(QStringLiteral("ベタ塗りのレイヤー…"), this, [this] { add_fill(); });
    special_->addAction(QStringLiteral("グラデーションのレイヤー…"), this, [this] { add_gradient(); });
    QMenu* adjust = special_->addMenu(QStringLiteral("色調補正のレイヤー"));
    for (const auto& [key, label] : adjustment_kinds()) {
        // (PC2b, the user's decision 3: a new 色相・彩度 is the upstream form, hue_saturation, in its place; a layer of
        // Python's hue is still mended in its own window. The user's decision 8: each named as レイヤー → 色調補正 names
        // the same correction, Photoshop's name — 色相・彩度…, 階調の反転)
        const std::string made = key == "hue" && adjust_forms::form_kind("hue_saturation") != nullptr ? std::string("hue_saturation") : key;
        const QString title = adjust_forms::command_title(made);
        adjust->addAction(title.isEmpty() ? label + QStringLiteral("…") : title, this, [this, made] { add_adjust(made); });
    }
    // (PC2: the C++ build's own kinds that have a form — app/adjust_forms, each group's)
    for (const adjust_forms::FormKind& form : adjust_forms::form_kinds()) {
        const core::AdjustKind* kind = core::adjust_kind(form.kind);
        if (kind == nullptr || !kind->layer || kind->group == "Python" || form.kind == "hue_saturation") continue;
        adjust->addAction(adjust_forms::command_title(form.kind), this, [this, key = std::string(form.kind)] { add_adjust(key); });
    }
    special_->addSeparator();
    special_->addAction(QStringLiteral("塗り・補正を直す…"), this, [this] { edit_special(); });
    add_special_button->setMenu(special_);
    auto* several_button = button(QStringLiteral("まとめて ▾"),
                                  QStringLiteral("Ctrl・Shift+クリックで選んだレイヤーをまとめて扱う。表示レイヤーの結合・変換・下描きの一括など"),
                                  QStringLiteral("layer_many"));
    many_ = new QMenu(several_button);
    many_->addAction(QStringLiteral("選んだレイヤーを結合"), this, [this] { merge_selected(); });
    QAction* group = many_->addAction(QStringLiteral("選んだレイヤーをフォルダにまとめる"), this, [this] { group_selected(); });
    group->setShortcut(QKeySequence(QStringLiteral("Ctrl+G")));
    many_->addAction(QStringLiteral("選んだレイヤーを見せる"), this, [this] { set_selected(Json{{"visible", true}}); });
    many_->addAction(QStringLiteral("選んだレイヤーを隠す"), this, [this] { set_selected(Json{{"visible", false}}); });
    many_->addAction(QStringLiteral("選んだレイヤーをロック"), this, [this] { set_selected(Json{{"locked", true}}); });
    many_->addAction(QStringLiteral("選んだレイヤーのロックを外す"), this, [this] { set_selected(Json{{"locked", false}}); });
    many_->addSeparator();
    many_->addAction(QStringLiteral("表示レイヤーを結合"), this, [this] { merge_visible(false); });
    QAction* visible_copy = many_->addAction(QStringLiteral("表示レイヤーのコピーを結合"), this, [this] { merge_visible(true); });
    visible_copy->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+Alt+E")));
    many_->addSeparator();
    many_->addAction(QStringLiteral("ペイントのレイヤーに変換（線を画像に）"), this, [this] { convert("paint"); });
    many_->addAction(QStringLiteral("ペンのレイヤーに変換（画像を線に）"), this, [this] { convert("pen"); });
    many_->addSeparator();
    many_->addAction(QStringLiteral("下描きを全部隠す"), this, [this] { drafts(Json{{"visible", false}}); });
    many_->addAction(QStringLiteral("下描きを全部見せる"), this, [this] { drafts(Json{{"visible", true}}); });
    many_->addAction(QStringLiteral("用紙の色…"), this, [this] { paper(); });
    several_button->setMenu(many_);
    effect_button_ = button(QStringLiteral("効果 ▾"), QStringLiteral("境界効果（フチ・水彩境界）と、表示色を印刷にも出すか"), QStringLiteral("layer_effects"));
    effects_ = new QMenu(effect_button_);
    effects_->addAction(QStringLiteral("フチをつける…"), this, [this] { border(); });
    effects_->addAction(QStringLiteral("水彩境界…"), this, [this] { water_edge(); });
    effects_->addAction(QStringLiteral("境界効果を外す"), this, [this] { set("effect", Json()); });
    effects_->addSeparator();
    effects_->addAction(QStringLiteral("トーン化（グレーを網点で印刷）…"), this, [this] { screen(); });
    effects_->addAction(QStringLiteral("トーン化を外す"), this, [this] { set("screen", Json()); });
    effects_->addSeparator();
    color_prints_ = effects_->addAction(QStringLiteral("表示色で印刷する"));
    color_prints_->setCheckable(true);
    color_prints_->setToolTip(QStringLiteral("「表示色」を画面だけでなく書き出し・印刷にも出します（青い線の原稿など）"));
    connect(color_prints_, &QAction::toggled, this, [this](bool on) { set("color_prints", on); });
    effect_button_->setMenu(effects_);
    auto* up = button(QStringLiteral("↑"), QStringLiteral("選んだレイヤーを上へ"), QStringLiteral("layer_up"));
    connect(up, &QPushButton::clicked, this, [this] { move(1); });
    auto* down = button(QStringLiteral("↓"), QStringLiteral("選んだレイヤーを下へ"), QStringLiteral("layer_down"));
    connect(down, &QPushButton::clicked, this, [this] { move(-1); });
    auto* delete_button = button(QStringLiteral("消す"), QStringLiteral("選んだレイヤーを消す"), QStringLiteral("layer_delete"));
    connect(delete_button, &QPushButton::clicked, this, [this] { remove(); });
    auto* duplicate_button = button(QStringLiteral("複製"), QStringLiteral("選んだレイヤーの写しを、すぐ上に作ります"), QStringLiteral("layer_duplicate"));
    connect(duplicate_button, &QPushButton::clicked, this, [this] { duplicate(); });
    auto* merge = button(QStringLiteral("下と結合"), QStringLiteral("選んだレイヤーを、すぐ下のレイヤーに合わせます（ペン同士は線のまま）"), QStringLiteral("layer_merge_down"));
    connect(merge, &QPushButton::clicked, this, [this] { merge_down(); });
    mask_button_ = button(QStringLiteral("マスク ▾"), QStringLiteral("レイヤーの一部を隠す。選択範囲から作り、ブラシで見せる所を足し、消しゴムで隠す"),
                          QStringLiteral("layer_mask"));
    mask_menu_ = new QMenu(mask_button_);
    // (PC3 B2, the user's decision 5: レイヤー → レイヤーマスク by Photoshop's names; マスクを編集する and マスクを反転 Genko's own)
    mask_menu_->addAction(QStringLiteral("すべての領域を表示"), this, [this] { mask(Json{{"fill", "show"}}); });
    mask_menu_->addAction(QStringLiteral("すべての領域を隠す"), this, [this] { mask(Json{{"fill", "hide"}}); });
    mask_menu_->addAction(QStringLiteral("選択範囲外をマスク"), this, [this] { mask_from_selection(); });
    mask_menu_->addAction(QStringLiteral("選択範囲をマスク"), this, [this] { hide_selection(); });
    mask_menu_->addAction(QStringLiteral("透明部分から"), this, [this] { mask_from_transparency(); });
    mask_menu_->addSeparator();
    // (PC4 E-0, the user's decision 2: the brush tool's name — preferences::renamed())
    mask_edit_ = mask_menu_->addAction(QStringLiteral("マスクを編集する（ブラシで見せる・消しゴムで隠す）"));
    mask_edit_->setCheckable(true);
    mask_edit_->setProperty("genko_attribute", QStringLiteral("act_mask_edit"));  // (named, as the window's commands: the help finds it by it)
    connect(mask_edit_, &QAction::toggled, this, [this](bool on) { edit_mask(on); });
    mask_menu_->addAction(QStringLiteral("マスクを反転"), this, [this] { mask(Json{{"invert", true}}); });
    mask_menu_->addSeparator();
    mask_menu_->addAction(QStringLiteral("削除"), this, [this] { mask(Json{{"delete", true}}); });
    mask_menu_->addAction(QStringLiteral("適用"), this, [this] { apply_mask(); });
    mask_off_ = mask_menu_->addAction(QStringLiteral("使用しない"));
    mask_off_->setCheckable(true);
    connect(mask_off_, &QAction::toggled, this, [this](bool on) { if (!loading_) mask(Json{{"enabled", !on}}); });
    mask_link_ = mask_menu_->addAction(QStringLiteral("リンク"));
    mask_link_->setCheckable(true);
    mask_link_->setToolTip(QStringLiteral("マスクをレイヤーと一緒に動かします（外すと、マスクはページに固定）"));
    connect(mask_link_, &QAction::toggled, this, [this](bool on) {
        const auto [page, layer] = this->layer();
        if (!loading_ && layer != nullptr && layer->mask && layer->mask->linked != on) mask(Json{{"linked", on}});
    });
    mask_button_->setMenu(mask_menu_);
    // (PC4 節 D, the user's decision 1: かける… opens the フィルター menu — Photoshop's filters; the corrections are レイヤー →
    // 色調補正's)
    filter_apply_ = button(QStringLiteral("かける…"), QStringLiteral("フィルター メニューを開きます（選んだフィルターをこのレイヤーにかけます。選択範囲があればその中だけ）"),
                           QStringLiteral("layer_filter_apply"));
    QPushButton* apply = filter_apply_;
    connect(apply, &QPushButton::clicked, this, [this] { apply_filter(); });

    const auto row = [](std::initializer_list<QWidget*> widgets) {
        auto* line = new QHBoxLayout;
        line->setSpacing(2);
        for (QWidget* w : widgets) {
            if (w == nullptr) line->addStretch(1);
            else line->addWidget(w);
        }
        return line;
    };
    // small picture buttons under the list, in groups (Python's): what adds a layer (and, apart on the right, what takes one
    // away), then what moves and combines them; their words in the tooltips
    for (const auto& [b, picture, tip] :
         std::vector<std::tuple<QPushButton*, const char*, QString>>{
             {add_pen, "pen_layer", QStringLiteral("ペンのレイヤーを足す（線を描く。線はあとから消しゴムで切れる）")},
             {add_paint, "paint_layer", QStringLiteral("ペイントのレイヤーを足す（塗りや筆のにじみ）")},
             {add_folder, "folder", QStringLiteral("フォルダを足す（レイヤーをまとめる）")},
             {add_special_button, "add", QStringLiteral("塗り・グラデーション・色調補正のレイヤーを足す")},
             {duplicate_button, "duplicate", QStringLiteral("選んだレイヤーを複製")},
             {merge, "merge", QStringLiteral("下のレイヤーと結合")},
             {several_button, "more", QStringLiteral("まとめて: 選んだレイヤーの結合・フォルダにまとめる・変換など")},
             {up, "up", QStringLiteral("選んだレイヤーを上へ")},
             {down, "down", QStringLiteral("選んだレイヤーを下へ")},
             {delete_button, "delete", QStringLiteral("選んだレイヤーを消す")}}) {
        b->setText(QString());
        b->setToolTip(tip);
        b->setAccessibleName(tip);
        theme::iconed(b, picture);
        b->setFixedSize(30, 28);
        b->setIconSize(QSize(18, 18));
        b->setProperty("iconbtn", true);  // (flat pictures, lit on hover: theme.cpp)
    }
    auto* adds = new QVBoxLayout;
    adds->setSpacing(4);
    adds->addLayout(row({add_pen, add_paint, add_folder, add_special_button, nullptr, delete_button}));
    adds->addLayout(row({up, down, nullptr, duplicate_button, merge, several_button}));
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(target_);
    layout->addWidget(search_);
    layout->addWidget(list_, 1);
    layout->addLayout(adds);
    // the chosen layer's settings fold away: the list and its buttons are what is used all the time
    details_toggle_ = new QPushButton;
    details_toggle_->setObjectName(QStringLiteral("layer_details_toggle"));
    details_toggle_->setCheckable(true);
    details_ = new QWidget;
    auto* dl = new QVBoxLayout(details_);
    dl->setContentsMargins(0, 0, 0, 0);
    dl->setSpacing(3);
    dl->addWidget(name_);
    auto* props = new QFormLayout;
    props->addRow(QStringLiteral("不透明度"), opacity_);
    props->addRow(QStringLiteral("合成"), blend_);
    dl->addLayout(props);
    for (QCheckBox* b : {clip_, protect_, locked_, each_panel_, overhang_, draft_, reference_}) dl->addWidget(b);
    dl->addWidget(tint_);
    dl->addLayout(row({mask_button_, effect_button_}));
    auto* frow = new QHBoxLayout;
    frow->addWidget(new QLabel(QStringLiteral("フィルター")), 1);
    frow->addWidget(apply);
    dl->addLayout(frow);
    connect(details_toggle_, &QPushButton::toggled, this, [this](bool on) { show_details(on); });
    details_toggle_->setChecked(settings()->value(QStringLiteral("ui/layer_details"), QStringLiteral("false")).toString() == QStringLiteral("true"));
    show_details(details_toggle_->isChecked(), false);
    layout->addWidget(details_toggle_);
    layout->addWidget(details_);
}

void LayerPanel::show_details(bool on, bool save) {
    details_->setVisible(on);
    if (on && save) fade_in(details_, 160);  // (opened by the person: its fields come in softly)
    details_toggle_->setText((on ? QStringLiteral("▾ ") : QStringLiteral("▸ ")) + QStringLiteral("レイヤーの設定"));
    details_toggle_->setToolTip(QStringLiteral("不透明度・合成・ロック・下描き・マスク・フィルターなど"));
    if (save) settings()->setValue(QStringLiteral("ui/layer_details"), on ? QStringLiteral("true") : QStringLiteral("false"));
}

void LayerPanel::edit_mask(bool on) {
    const auto [page, layer] = this->layer();
    if (on && layer != nullptr && !layer->mask) mask(Json{{"fill", "show"}});
    window_->mask_edit = on;
    window_->flash(on ? QStringLiteral("マスクの編集中: ブラシで見せる所を足し、消しゴムで隠します") : QStringLiteral("マスクの編集を終えました"), 4000);
}

namespace {

// What a layer's small picture is drawn over (環境設定's 透明部分): its checker's size and colours.
std::string checker_key(const preferences::Checker& c) {
    return QStringLiteral("%1 %2 %3").arg(c.square).arg(c.light.name(QColor::HexArgb), c.dark.name(QColor::HexArgb)).toStdString();
}

}  // namespace

void LayerPanel::refresh_look() {
    marks_.clear();
    // (環境設定's 透明部分: the pictures drawn over another checker again — only those: the look is refreshed for other
    // reasons too, the computer's light or dark, and drawing every layer's picture again is a stall)
    const std::string checker = checker_key(preferences::checker());
    std::erase_if(thumbs_, [&](const auto& thumb) { return thumb.second.checker != checker; });
    refresh();
}

void LayerPanel::refresh() {
    const core::Page* page = window_->current_page();
    // (PC3 C-d: the layers chosen together stay chosen after a change of the same page — aligned, linked or moved again)
    const std::vector<std::string> chosen = page != nullptr && page->id == listed_page_ ? selected_ids() : std::vector<std::string>{};
    listed_page_ = page != nullptr ? page->id : std::string();
    loading_ = true;
    list_->clear();
    ids_.clear();
    rows_.clear();
    if (page != nullptr) {
        for (auto it = page->layers.rbegin(); it != page->layers.rend(); ++it) {  // front first
            const core::Layer& layer = *it;
            const QString indent = core::py_truthy(layer.parent_id) ? QStringLiteral("　") : QString();
            QStringList marks;
            if (!layer.exportable && layer.role != core::LayerRole::Name && layer.role != core::LayerRole::Draft) marks << QStringLiteral("下描き");
            if (layer.reference) marks << QStringLiteral("参照");
            if (layer.mask) marks << (layer.mask->linked ? QStringLiteral("マスク（リンク）") : QStringLiteral("マスク"));  // (PC3 B2: masks@1)
            if (layer.shapes && !layer.shapes->items.empty()) marks << QStringLiteral("形 %1").arg(layer.shapes->items.size());  // (pen-shapes@1)
            if (layer.locked) marks << QStringLiteral("ロック");
            if (layer.lock_position) marks << QStringLiteral("位置ロック");  // (transform@1, PC3 C-d)
            if (layer.link) {  // (transform@1, PC3 C-d: its link group, by its number when it has one)
                const QString group = QString::fromStdString(*layer.link);
                marks << (group.startsWith(QStringLiteral("link-")) ? QStringLiteral("リンク %1").arg(group.mid(5)) : QStringLiteral("レイヤーリンク"));
            }
            if (layer.kind == core::LayerKind::Smart) {  // (smart-objects@1: its contents in the book, or a file linked)
                const core::SmartContents* contents = core::smart::contents_of(window_->book(), layer);
                marks << (contents != nullptr && contents->link ? QStringLiteral("リンク") : QStringLiteral("埋め込み"));
                if (contents != nullptr && contents->link) {  // (PC3 A1: the linked file gone, or changed)
                    const std::string status = window_->link_status(*core::smart::contents_id(layer));
                    if (status == "missing") marks << QStringLiteral("リンク切れ");
                    if (status == "modified") marks << QStringLiteral("更新あり");
                }
            }
            QString blend;
            const bool correction = layer.kind == core::LayerKind::Adjust && layer.adjust && layer.adjust->is_object();
            const std::string drawn = correction ? core::native_of(*layer.adjust).blend : layer.blend;  // (a correction's: its native's)
            for (const auto& [key, label] : blend_modes()) if (key == drawn && key != "normal") blend = label;
            QStringList meta = marks;
            if (!blend.isEmpty()) meta << blend;
            if (layer.opacity < 0.995) meta << QStringLiteral("%1%").arg(std::lround(layer.opacity * 100));
            auto* item = new QListWidgetItem(indent + wording::layer_label(layer) + (marks.isEmpty() ? QString() : QStringLiteral("　· ") + marks.join(QStringLiteral(" · "))));
            item->setData(Qt::UserRole + 2, meta.join(QStringLiteral(" · ")));
            item->setData(Qt::UserRole + 3, indent + wording::layer_label(layer));  // (the name alone: the marks go right)
            item->setToolTip(kind_name(layer.kind));
            // its picture, or (nothing drawn yet) a quiet mark of its kind; every row the same height
            const QIcon picture = thumbnail(*page, layer);
            item->setIcon(picture.isNull() ? kind_mark(layer.kind) : picture);
            item->setData(Qt::UserRole + 1, picture.isNull() ? QStringLiteral("mark") : QStringLiteral("picture"));
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(layer.visible ? Qt::Checked : Qt::Unchecked);
            list_->addItem(item);
            ids_.push_back(layer.id);
            rows_.push_back(kLayerRow);
            if (layer.kind == core::LayerKind::Smart) add_smart_filter_rows(layer, indent);  // (PC3 A2)
        }
    }
    if (const core::Layer* target = window_->target_layer()) {
        const auto at = std::find(ids_.begin(), ids_.end(), target->id);
        if (at != ids_.end()) list_->setCurrentRow(static_cast<int>(at - ids_.begin()));
    }
    if (chosen.size() > 1) {
        for (int row = 0; row < list_->count() && row < static_cast<int>(ids_.size()); ++row)
            if (row_kind(row) == kLayerRow && std::find(chosen.begin(), chosen.end(), ids_[static_cast<std::size_t>(row)]) != chosen.end())
                list_->item(row)->setSelected(true);
    }
    loading_ = false;
    search_->setVisible(ids_.size() > 8 || !search_->text().isEmpty());  // (finding a layer by name: a thick page only)
    search(search_->text());
    selected(false);
}

void LayerPanel::show_target() {
    const core::Layer* target = window_->target_layer();
    const auto at = target != nullptr ? std::find(ids_.begin(), ids_.end(), target->id) : ids_.end();
    if (at == ids_.end()) {  // (a layer the list does not have yet)
        refresh();
        return;
    }
    const int row = static_cast<int>(at - ids_.begin());
    if (list_->currentRow() != row) {
        loading_ = true;
        list_->setCurrentRow(row);
        loading_ = false;
    }
    selected(false);
}

QIcon LayerPanel::thumbnail(const core::Page& page, const core::Layer& layer) {
    // a small picture of the layer alone (kept until the layer changes); an empty layer or a folder: none
    if (layer.kind == core::LayerKind::Folder) return {};
    const bool drawn = layer.stroke_count() > 0 || !layer.patches.empty() || (layer.raster_png && !layer.raster_png->empty()) || layer.color_raster ||
                       layer.ink_raster ||
                       layer.kind == core::LayerKind::Placed || layer.kind == core::LayerKind::Tone || (layer.fill && core::py_truthy(*layer.fill)) ||
                       layer.kind == core::LayerKind::Smart || (layer.shapes && !layer.shapes->items.empty());
    if (!drawn) return {};
    const std::string key = page.id + "/" + layer.id;
    Thumb made{layer.strokes, layer.raster_png, layer.color_raster, layer.ink_raster, layer.mask ? layer.mask->png : nullptr, {}, thumb_state(layer), {}, {}};
    for (const core::Patch& patch : layer.patches) made.patches.push_back(patch.png);
    if (layer.shapes && !layer.shapes->items.empty()) {  // (pen-shapes@1: its parts, a list made anew when they change)
        made.state += "|shapes " + std::to_string(reinterpret_cast<std::uintptr_t>(layer.shapes.get())) + " " +
                      std::to_string(core::vector::shapes_blob(*layer.shapes).size());
    }
    if (const core::SmartContents* contents = core::smart::contents_of(window_->book(), layer)) {  // (its contents as they are now)
        made.state += "|" + std::to_string(reinterpret_cast<std::uintptr_t>(contents));
    }
    if (const auto found = thumbs_.find(key); found != thumbs_.end()) {
        const Thumb& was = found->second;
        if (was.strokes == made.strokes && was.raster == made.raster && was.color == made.color && was.ink == made.ink && was.mask == made.mask &&
            was.patches == made.patches && was.state == made.state)
            return was.icon;
    }
    QIcon icon;
    const preferences::Checker checker = preferences::checker();
    made.checker = checker_key(checker);
    try {
        const render::Image image = render::layer_image(page, layer, 10, &window_->book(), true);
        if (checker.square <= 0) {  // (no checker: on white)
            render::Image paper = render::Image::create("RGBA", image.size(), render::Ink{255, 255, 255, 255});
            paper = render::alpha_composite(paper, image.convert("RGBA"));
            const std::string bytes = paper.convert("RGB").tobytes();
            const QImage picture(reinterpret_cast<const uchar*>(bytes.data()), paper.width(), paper.height(), paper.width() * 3, QImage::Format_RGB888);
            icon = QIcon(QPixmap::fromImage(picture.copy()));
        } else {  // (環境設定 → 透明部分・色域: the transparent parts over the checker)
            const render::Image rgba = image.convert("RGBA");
            const std::string bytes = rgba.tobytes();
            const QImage layer_picture(reinterpret_cast<const uchar*>(bytes.data()), rgba.width(), rgba.height(), rgba.width() * 4, QImage::Format_RGBA8888);
            QImage picture(rgba.width(), rgba.height(), QImage::Format_RGB32);
            preferences::paint_checker(picture, checker);
            QPainter painter(&picture);
            painter.drawImage(0, 0, layer_picture);
            painter.end();
            icon = QIcon(QPixmap::fromImage(picture));
        }
    } catch (const std::exception&) {
        return {};
    }
    if (thumbs_.size() > 400) thumbs_.clear();
    made.icon = icon;
    thumbs_[key] = std::move(made);
    return icon;
}

std::pair<const core::Page*, const core::Layer*> LayerPanel::layer() const {
    const core::Page* page = window_->current_page();
    const int row = list_->currentRow();
    if (page == nullptr || row < 0 || row >= static_cast<int>(ids_.size())) return {page, nullptr};
    for (const core::Layer& l : page->layers)
        if (l.id == ids_[static_cast<std::size_t>(row)]) return {page, &l};
    return {page, nullptr};
}

void LayerPanel::selected(bool from_list) {
    const auto [page, layer] = this->layer();
    if (layer == nullptr) {
        target_->clear();
        target_->hide();
        return;
    }
    if (from_list && !loading_) {
        if (window_->target_layer() != layer && mask_edit_->isChecked()) mask_edit_->setChecked(false);  // (the mask being edited was the other layer's)
        window_->set_target_layer(layer->id);
    }
    loading_ = true;
    name_->setText(wording::layer_label(*layer));
    opacity_->setValue(static_cast<int>(std::lround(100 * layer->opacity)));
    const bool correction = layer->kind == core::LayerKind::Adjust && layer->adjust && layer->adjust->is_object();
    const std::string blend = correction ? core::native_of(*layer->adjust).blend : layer->blend;  // (a correction's: its native's)
    blend_->setCurrentIndex(std::max(0, blend_->findData(QString::fromStdString(blend.empty() ? "normal" : blend))));
    clip_->setChecked(layer->clip);
    protect_->setChecked(layer->lock_alpha);
    locked_->setChecked(layer->locked);
    overhang_->setChecked(!layer->panel_clip);
    each_panel_->setChecked(layer->panel_each);
    each_panel_->setEnabled(layer->panel_clip);
    draft_->setChecked(!layer->exportable);
    reference_->setChecked(layer->reference);
    draft_->setEnabled(layer->role != core::LayerRole::Name && layer->role != core::LayerRole::Draft);  // (those never print)
    int tint = 0;
    if (layer->color && !layer->color->empty())
        for (std::size_t i = 1; i < tints().size(); ++i)
            if (tints()[i].second == Json(*layer->color)) tint = static_cast<int>(i);
    tint_->setCurrentIndex(tint);
    mask_off_->setChecked(layer->mask && !layer->mask->enabled);
    mask_link_->setChecked(layer->mask && layer->mask->linked);  // (PC3 B2: masks@1)
    mask_link_->setEnabled(layer->mask.has_value());
    mask_button_->setText(layer->mask ? QStringLiteral("マスク ◐ ▾") : QStringLiteral("マスク ▾"));
    color_prints_->setChecked(layer->color_prints);
    effect_button_->setText(layer->effect && core::py_truthy(*layer->effect) ? QStringLiteral("効果 ✓ ▾") : QStringLiteral("効果 ▾"));
    loading_ = false;
    const bool drawable = MainWindow::drawable(*layer);
    const bool prints = layer->exportable && layer->role != core::LayerRole::Name && layer->role != core::LayerRole::Draft;
    const QString note = prints ? QString() : QStringLiteral(" <span style='color:#c0392b'>（印刷されません）</span>");
    target_->setText(drawable ? QStringLiteral("描く先: <b>%1</b>%2").arg(wording::layer_label(*layer).toHtmlEscaped(), note)
                              : QStringLiteral("<span style='color:#c0392b'>「%1」には描けません。ペンかペイントのレイヤーを選びます</span>")
                                    .arg(wording::layer_label(*layer).toHtmlEscaped()));
    target_->setVisible(!(drawable && prints));  // (only a warning needs words)
}

void LayerPanel::set(const std::string& key, const Json& value) {
    const auto [page, layer] = this->layer();
    if (layer != nullptr && !loading_)
        window_->apply_ops(Json::array({Json{{"op", "set_layer"}, {"page", page->index.json()}, {"id", layer->id}, {key, value}}}));
}

void LayerPanel::visibility(QListWidgetItem* item) {
    if (loading_) return;
    const core::Page* page = window_->current_page();
    const int row = list_->row(item);
    if (page == nullptr || row < 0 || row >= static_cast<int>(ids_.size())) return;
    const std::string id = ids_[static_cast<std::size_t>(row)];
    const auto it = std::find_if(page->layers.begin(), page->layers.end(), [&](const core::Layer& l) { return l.id == id; });
    if (it == page->layers.end()) return;
    const bool visible = item->checkState() == Qt::Checked;
    if (smart_filter_visibility(row, visible)) return;  // (PC3 A2: a smart filter's eye, or all of them)
    if (QApplication::keyboardModifiers() & Qt::AltModifier) {
        solo(id);  // (Alt+クリック: only this one shown; again: all back)
        return;
    }
    if (it->visible != visible)
        window_->apply_ops(Json::array({Json{{"op", "set_layer"}, {"page", page->index.json()}, {"id", id}, {"visible", visible}}}));
}

void LayerPanel::solo(const std::string& layer_id) {
    const core::Page* page = window_->current_page();
    if (page == nullptr) return;
    std::map<std::string, const core::Layer*> by_id;
    for (const core::Layer& l : page->layers) by_id[l.id] = &l;
    std::vector<std::string> keep{layer_id};
    for (auto cur = by_id.find(layer_id); cur != by_id.end() && core::py_truthy(cur->second->parent_id);) {
        const std::string parent = core::py_str(cur->second->parent_id);
        keep.push_back(parent);
        cur = by_id.find(parent);
    }
    const auto kept = [&](const std::string& id) { return std::find(keep.begin(), keep.end(), id) != keep.end(); };
    const bool alone = std::all_of(page->layers.begin(), page->layers.end(), [&](const core::Layer& l) { return kept(l.id) || !l.visible; });
    auto& hidden = solo_hidden_[page->id];
    Json ops = Json::array();
    if (alone && !hidden.empty()) {
        for (const std::string& id : hidden)
            if (by_id.contains(id)) ops.push_back(Json{{"op", "set_layer"}, {"page", page->index.json()}, {"id", id}, {"visible", true}});
        solo_hidden_.erase(page->id);
    } else {
        std::vector<std::string> hiding;
        for (const core::Layer& l : page->layers)
            if (!kept(l.id) && l.visible) hiding.push_back(l.id);
        for (const std::string& id : hiding) ops.push_back(Json{{"op", "set_layer"}, {"page", page->index.json()}, {"id", id}, {"visible", false}});
        for (const std::string& id : keep)
            if (by_id.contains(id) && !by_id[id]->visible) ops.push_back(Json{{"op", "set_layer"}, {"page", page->index.json()}, {"id", id}, {"visible", true}});
        hidden = hiding;
    }
    if (!ops.empty()) window_->apply_ops(ops);
    refresh();
}

void LayerPanel::add(const std::string& kind, const QString& title) {
    const auto [page, layer] = this->layer();
    if (page == nullptr) return;
    const std::string id = core::new_id();
    Json op{{"op", "add_layer"}, {"page", page->index.json()}, {"kind", kind}, {"name", title.toStdString()}, {"id", id}};
    if (layer != nullptr) op["after"] = layer->id;  // just in front of the chosen layer
    if (window_->apply_ops(Json::array({op})) && kind != "folder") {
        window_->set_target_layer(id);
        refresh();
    }
}

void LayerPanel::duplicate() {
    const auto [page, layer] = this->layer();
    if (layer == nullptr) return;
    const std::string id = core::new_id();
    if (window_->apply_ops(Json::array({Json{{"op", "duplicate_layer"}, {"page", page->index.json()}, {"id", layer->id}, {"new_id", id}}}))) {
        window_->set_target_layer(id);
        refresh();
    }
}

void LayerPanel::merge_down() {
    const auto [page, layer] = this->layer();
    if (layer == nullptr) return;
    const auto at = static_cast<std::ptrdiff_t>(layer - page->layers.data());
    const auto index = page->index.json();
    if (window_->apply_ops(Json::array({Json{{"op", "merge_down"}, {"page", index}, {"id", layer->id}}})) && at >= 1) {
        if (const core::Page* now = window_->current_page(); now != nullptr && at - 1 < static_cast<std::ptrdiff_t>(now->layers.size()))
            window_->set_target_layer(now->layers[static_cast<std::size_t>(at - 1)].id);
        refresh();
    }
}

void LayerPanel::mask(const Json& change) {
    const auto [page, layer] = this->layer();
    if (layer == nullptr || loading_) return;
    Json op{{"op", "set_layer_mask"}, {"page", page->index.json()}, {"id", layer->id}};
    op.update(change);
    // (masks@1, the user's decision: a new mask moves with its layer, as Photoshop's; the masks books have keep their place)
    if (!layer->mask && !change.contains("delete") && !change.contains("linked")) op["linked"] = true;
    window_->apply_ops(Json::array({op}));
    selected(false);
}

void LayerPanel::mask_from_selection() {
    const auto area = window_->selection_area();
    if (!area) {
        window_->flash(QStringLiteral("先に選択範囲ツール（M・L・W）で見せたい所を選びます"), 5000);
        return;
    }
    mask(Json{{"area", *area}});
    window_->canvas()->set_selection(std::nullopt);  // (PC3 B2: made, the selection let go — as Photoshop does)
}

void LayerPanel::move(int delta) {
    // (PC3 C-d, decision 5: 前面へ・背面へ among its siblings — a folder with its layers —, the layers chosen together)
    if (delta != 0) window_->arrange_layers(delta > 0 ? "forward" : "backward");
}

void LayerPanel::select_ids(const std::vector<std::string>& ids) {
    loading_ = true;
    list_->clearSelection();
    for (int row = 0; row < list_->count() && row < static_cast<int>(ids_.size()); ++row) {
        if (row_kind(row) != kLayerRow) continue;
        if (std::find(ids.begin(), ids.end(), ids_[static_cast<std::size_t>(row)]) != ids.end()) list_->item(row)->setSelected(true);
    }
    loading_ = false;
}

void LayerPanel::remove() {
    const auto [page, layer] = this->layer();
    if (layer == nullptr) return;
    const auto asked = window_->asking();  // (the page and the layer stay as they are while asked, or nothing is done)
    if (!ask::question(this, QStringLiteral("Genko"), QStringLiteral("レイヤー「%1」を消しますか？\n（元に戻す で取り消せます）").arg(wording::layer_label(*layer)))) return;
    if (!window_->still(asked)) return;
    window_->apply_ops(Json::array({Json{{"op", "delete_layer"}, {"page", page->index.json()}, {"id", layer->id}}}));
}

std::vector<std::string> LayerPanel::selected_ids() const {
    std::vector<int> rows;
    for (const QListWidgetItem* item : list_->selectedItems()) rows.push_back(list_->row(item));
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    std::vector<std::string> out;
    for (const int row : rows)  // (a smart filter's row is its smart object's: once)
        if (row >= 0 && row < static_cast<int>(ids_.size()) && std::find(out.begin(), out.end(), ids_[static_cast<std::size_t>(row)]) == out.end())
            out.push_back(ids_[static_cast<std::size_t>(row)]);
    return out;
}

void LayerPanel::search(const QString& text) {
    const core::Page* page = window_->current_page();
    const QString words = text.trimmed().toLower();
    // (PC3 B2, レイヤーを分離: on its page, only the layers isolated are listed)
    const bool alone = page != nullptr && !isolated_.empty() && isolated_page_ == page->id;
    for (std::size_t row = 0; row < ids_.size(); ++row) {
        QString label;
        if (page != nullptr)
            for (const core::Layer& l : page->layers)
                if (l.id == ids_[row]) label = wording::layer_label(l).toLower();
        const bool apart = alone && std::find(isolated_.begin(), isolated_.end(), ids_[row]) == isolated_.end();
        if (QListWidgetItem* item = list_->item(static_cast<int>(row))) item->setHidden(apart || (!words.isEmpty() && !label.contains(words)));
    }
}

std::optional<std::vector<std::string>> LayerPanel::several(std::size_t least) {
    auto ids = selected_ids();
    if (ids.size() < least) {
        window_->flash(QStringLiteral("Ctrl（または Shift）+クリックで、レイヤーを %1 枚以上選びます").arg(least), 5000);
        return std::nullopt;
    }
    return ids;
}

void LayerPanel::merge_selected() {
    const auto ids = several();
    const core::Page* page = window_->current_page();
    if (!ids || page == nullptr) return;
    const auto index = page->index.json();
    std::string lowest;
    for (const core::Layer& l : page->layers)
        if (lowest.empty() && std::find(ids->begin(), ids->end(), l.id) != ids->end()) lowest = l.id;
    if (window_->apply_ops(Json::array({Json{{"op", "merge_layers"}, {"page", index}, {"ids", *ids}}}))) {
        window_->set_target_layer(lowest);
        refresh();
    }
}

void LayerPanel::group_selected() {
    const auto ids = several(1);
    const core::Page* page = window_->current_page();
    if (ids && page != nullptr)
        window_->apply_ops(Json::array({Json{{"op", "group_layers"}, {"page", page->index.json()}, {"ids", *ids}, {"id", core::new_id()}, {"name", "フォルダ"}}}));
}

void LayerPanel::set_selected(const Json& fields) {
    const auto ids = several(1);
    const core::Page* page = window_->current_page();
    if (!ids || page == nullptr) return;
    Json op{{"op", "set_layers"}, {"page", page->index.json()}, {"ids", *ids}};
    op.update(fields);
    window_->apply_ops(Json::array({op}));
}

void LayerPanel::merge_visible(bool copy) {
    const core::Page* page = window_->current_page();
    if (page == nullptr) return;
    const auto asked = window_->asking();
    if (!copy && !ask::question(this, QStringLiteral("Genko"), QStringLiteral("見えているレイヤーを 1 枚にまとめます。\n（元に戻す で取り消せます）"))) return;
    if (!window_->still(asked)) return;
    const std::string id = core::new_id();
    Json op{{"op", "merge_visible"}, {"page", page->index.json()}, {"copy", copy}};
    if (copy) op["id"] = id;
    if (window_->apply_ops(Json::array({op})) && copy) {
        window_->set_target_layer(id);
        refresh();
    }
}

void LayerPanel::convert(const std::string& to) {
    const auto [page, layer] = this->layer();
    if (layer == nullptr) return;
    Json op{{"op", "convert_layer"}, {"page", page->index.json()}, {"id", layer->id}, {"to", to}};
    if (to == "pen" && layer->color_raster) op["preserve_precision"] = true;  // (precise colour: the lines keep it)
    window_->apply_ops(Json::array({op}));
}

void LayerPanel::drafts(const Json& fields) {
    const core::Page* page = window_->current_page();
    std::vector<std::string> ids;
    if (page != nullptr)
        for (const core::Layer& l : page->layers)
            if (l.role == core::LayerRole::Draft || (!l.exportable && l.role != core::LayerRole::Name)) ids.push_back(l.id);
    if (ids.empty()) {
        window_->flash(QStringLiteral("このページに下描きのレイヤーはありません"), 4000);
        return;
    }
    Json op{{"op", "set_layers"}, {"page", page->index.json()}, {"ids", ids}};
    op.update(fields);
    window_->apply_ops(Json::array({op}));
}

void LayerPanel::paper() {
    const core::Page* page = window_->current_page();
    std::vector<int> now{255, 255, 255};
    if (page != nullptr && page->extra.is_object() && page->extra.contains("paper_rgb") && page->extra["paper_rgb"].is_array())
        now = page->extra["paper_rgb"].get<std::vector<int>>();
    const auto asked = window_->asking();
    const auto colour = ask::colour(this, QColor(now[0], now[1], now[2]), QStringLiteral("用紙の色（全ページ）"));
    if (!colour || !window_->still(asked)) return;
    const Json rgb = Json::array({colour->red(), colour->green(), colour->blue()});
    window_->apply_ops(Json::array({Json{{"op", "set_paper"}, {"rgb", rgb == Json::array({255, 255, 255}) ? Json() : rgb}}}));
}

void LayerPanel::add_special(const std::string& kind, const QString& title, const Json& fields) {
    const auto [page, layer] = this->layer();
    if (page == nullptr) return;
    const std::string id = core::new_id();
    Json op{{"op", "add_layer"}, {"page", page->index.json()}, {"kind", kind}, {"name", title.toStdString()}, {"id", id}};
    op.update(fields);
    if (layer != nullptr) op["after"] = layer->id;
    if (window_->apply_ops(Json::array({op}))) {
        window_->set_target_layer(id);
        refresh();
    }
}

void LayerPanel::add_fill() {
    const auto& rgb = window_->pen().rgb;
    const auto asked = window_->asking();
    const auto colour = ask::colour(this, QColor(static_cast<int>(rgb[0]), static_cast<int>(rgb[1]), static_cast<int>(rgb[2])), QStringLiteral("ベタ塗りの色"));
    if (colour && window_->still(asked)) add_special("fill", QStringLiteral("ベタ塗り"), Json{{"rgb", Json::array({colour->red(), colour->green(), colour->blue()})}});
}

void LayerPanel::add_gradient() {
    const auto& rgb = window_->pen().rgb;
    const auto asked = window_->asking();
    const auto spec = gradient_dialog(this, window_->current_page(),
                                      Json{{"rgb_from", Json::array({rgb[0], rgb[1], rgb[2]})}, {"rgb_to", Json::array({255, 255, 255})}, {"opacity_to", 0.0}});
    if (spec && window_->still(asked)) add_special("gradient", QStringLiteral("グラデーション"), Json{{"gradient", *spec}});
}

std::optional<Json> LayerPanel::adjust_fields(const std::string& kind, const Json& now, Json* opened) {
    if (adjust_forms::form_kind(kind) != nullptr) {  // (PC2: a kind with a form of its own, app/adjust_forms)
        adjust_forms::FormContext context;
        context.page = window_->current_page();
        const auto& rgb = window_->pen().rgb;
        context.pen = {static_cast<int>(rgb[0]), static_cast<int>(rgb[1]), static_cast<int>(rgb[2])};
        return adjust_forms::adjust_dialog(this, kind, now, context, {}, opened);
    }
    if (kind == "gradient_map") {
        const auto& rgb = window_->pen().rgb;
        return gradient_map_dialog(this, now, {{static_cast<int>(rgb[0]), static_cast<int>(rgb[1]), static_cast<int>(rgb[2])}, {255, 255, 255}});
    }
    return filter_params(this, kind, now);
}

void LayerPanel::add_adjust(const std::string& kind) {
    const auto asked = window_->asking();
    // (PC2a, the user's decision 3: a correction made new starts from PhotoCraft's form, when its kind has a window. 階調の
    // 反転 has nothing to ask: made at once, as レイヤー → 色調補正 → 階調の反転 does its work — the user's decision 8)
    const adjust_forms::FormKind* form_of = adjust_forms::form_kind(kind);
    const auto params = kind == "invert" ? std::optional<Json>(Json::object())
                                         : adjust_fields(kind, form_of != nullptr ? form_of->fresh : Json::object());
    if (!params || !window_->still(asked)) return;
    QString title = QStringLiteral("色調補正");
    if (const adjust_forms::FormKind* form = adjust_forms::form_kind(kind)) title = form->title;
    for (const auto& [key, label] : adjustment_kinds()) if (key == kind) title = label;
    Json spec{{"kind", kind}};
    spec.update(adjust_forms::without_nulls(*params));
    add_special("adjust", title, Json{{"adjust", spec}});
}

void LayerPanel::edit_special() {
    const auto [page, layer] = this->layer();
    if (layer == nullptr) return;
    if (layer->kind == core::LayerKind::Smart) {  // (smart-objects@1: レイヤーの内容オプション… — its contents)
        window_->content_options();
        return;
    }
    const auto asked = window_->asking();  // (each dialog's answer applied only to the book it was asked about)
    if (layer->kind == core::LayerKind::Adjust && layer->adjust && layer->adjust->is_object()) {
        const std::string kind = layer->adjust->value("kind", std::string("levels"));
        if (kind == "exposure") {
            window_->action(QStringLiteral("act_exposure"))->trigger();  // (its own dialog)
            return;
        }
        Json opened;
        const auto params = adjust_fields(kind, *layer->adjust, &opened);
        if (!params || !window_->still(asked)) return;
        Json spec;
        if (opened.is_object()) {
            // (PC2, the user's decision 3: a window of PC2's forms — what the person changed laid over the correction as it
            // is: what the window does not hold or did not change stays, Python's `channel` and form among it)
            spec = *layer->adjust;
            adjust_forms::lay_changes(spec, adjust_forms::changes(opened, *params));
            if (spec == *layer->adjust) return;
        } else {
            spec = Json{{"kind", kind}};
            spec.update(*params);
            // (PC2: its native settings — the blend, the C++ build's own formulas — kept where the window does not hold them)
            Json native = layer->adjust->contains("native") && (*layer->adjust)["native"].is_object() ? (*layer->adjust)["native"] : Json::object();
            if (params->contains("native") && (*params)["native"].is_object()) native.update((*params)["native"]);
            spec.erase("native");
            if (!native.empty()) spec["native"] = native;
        }
        set("adjust", spec);
    } else if (layer->kind == core::LayerKind::Fill && layer->fill && layer->fill->is_object() && layer->fill->contains("gradient")) {
        if (const auto spec = gradient_dialog(this, page, (*layer->fill)["gradient"]); spec && window_->still(asked)) set("fill", Json{{"gradient", *spec}});
    } else if (layer->kind == core::LayerKind::Fill && layer->fill && layer->fill->is_object()) {
        const Json rgb = layer->fill->value("rgb", Json::array({255, 255, 255}));
        const auto colour = ask::colour(this, QColor(rgb[0].get<int>(), rgb[1].get<int>(), rgb[2].get<int>()), QStringLiteral("ベタ塗りの色"));
        if (colour && window_->still(asked)) set("fill", Json{{"rgb", Json::array({colour->red(), colour->green(), colour->blue()})}});
    }
}

void LayerPanel::border() {
    const auto [page, layer] = this->layer();
    if (layer == nullptr) return;
    const auto asked = window_->asking();
    const auto width = ask::get_double(this, QStringLiteral("フチ"), QStringLiteral("フチの太さ（mm）"), 0.6, 0.05, 10, 2);
    if (!width) return;
    const auto colour = ask::colour(this, QColor(255, 255, 255), QStringLiteral("フチの色"));
    if (!colour || !window_->still(asked)) return;
    Json effect = layer->effect && layer->effect->is_object() ? *layer->effect : Json::object();
    effect["border"] = Json{{"width_mm", *width}, {"rgb", Json::array({colour->red(), colour->green(), colour->blue()})}};
    set("effect", effect);
}

void LayerPanel::water_edge() {
    const auto [page, layer] = this->layer();
    if (layer == nullptr) return;
    const auto asked = window_->asking();
    const auto width = ask::get_double(this, QStringLiteral("水彩境界"), QStringLiteral("にじむ幅（mm）"), 0.8, 0.05, 10, 2);
    if (!width || !window_->still(asked)) return;
    Json effect = layer->effect && layer->effect->is_object() ? *layer->effect : Json::object();
    effect["water_edge"] = Json{{"width_mm", *width}, {"strength", 0.7}};
    set("effect", effect);
}

void LayerPanel::screen() {
    const auto [page, layer] = this->layer();
    if (layer == nullptr) return;
    const auto asked = window_->asking();
    const auto spec = screen_dialog(this, layer->screen && layer->screen->is_object() ? *layer->screen : Json::object());
    if (!spec || !window_->still(asked)) return;
    set("screen", *spec);
    window_->flash(QStringLiteral("このレイヤーのグレーは、印刷と書き出しで網点になります（画面はグレーのまま）"), 5000);
}

std::optional<std::vector<int>> LayerPanel::histogram(const core::Page& page, const core::Layer& layer) const {
    // how many of the layer's drawn pixels have each lightness (for レベル補正 and トーンカーブ)
    try {
        if (page.color && layer.ink_raster) {  // (color-mode@1: the lightness of the colours its inks mean, which the
                                               // corrections work: a line on K 100% alone is black; every other pixel)
            const core::InkPixels p = core::decode_ink_raster(*layer.ink_raster);
            const unsigned n = p.channels() - 1;
            std::vector<double> inks;
            for (std::uint32_t y = 0; y < p.height; y += 2) {
                for (std::uint32_t x = 0; x < p.width; x += 2) {
                    const std::uint16_t* s = p.samples.data() + (std::size_t(y) * p.width + x) * p.channels();
                    if (s[n] == 0) continue;
                    for (unsigned c = 0; c < n; ++c) inks.push_back(render::ink::decode_sample(p.space, c, s[c], p.depth));
                }
            }
            const std::size_t count = inks.size() / n;
            std::vector<double> rgb(count * 3);
            render::ink::meaning_of(*page.color, inks.data(), rgb.data(), count);
            std::vector<int> counts(256, 0);
            for (std::size_t i = 0; i < count; ++i) {  // (as Image.convert("L"): 299, 587, 114)
                const double l = (rgb[i * 3] * 299 + rgb[i * 3 + 1] * 587 + rgb[i * 3 + 2] * 114) / 1000 * 255;
                ++counts[static_cast<std::size_t>(std::clamp(std::lround(l), 0L, 255L))];
            }
            return counts;
        }
        const render::Image image = render::layer_image(page, layer, 100, &window_->book(), true);
        const std::string grey = image.convert("L").tobytes(), alpha = image.getchannel(3).tobytes();
        std::vector<int> counts(256, 0);
        for (std::size_t i = 0; i < grey.size(); ++i)
            if (alpha[i] != 0) ++counts[static_cast<unsigned char>(grey[i])];
        return counts;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

void LayerPanel::apply_filter() {
    // (PC4 節 D, the user's decision 1: the フィルター menu under the button — its commands work the layer drawn on)
    QMenu* menu = window_->filter_menu();
    if (menu == nullptr) return;
    QWidget* from = filter_apply_ != nullptr && filter_apply_->isVisible() ? static_cast<QWidget*>(filter_apply_) : static_cast<QWidget*>(this);
    menu->popup(from->mapToGlobal(QPoint(0, from->height())));
}

QIcon LayerPanel::kind_mark(core::LayerKind kind) {
    // a small grey picture of the layer's kind, in the room a thumbnail takes (kept per kind and colour)
    static const std::map<core::LayerKind, const char*> names{
        {core::LayerKind::Strokes, "kind_strokes"}, {core::LayerKind::Raster, "kind_raster"}, {core::LayerKind::Folder, "kind_folder"},
        {core::LayerKind::Placed, "kind_placed"},   {core::LayerKind::Tone, "kind_tone"},     {core::LayerKind::Fill, "kind_fill"},
        {core::LayerKind::Adjust, "kind_adjust"},   {core::LayerKind::Smart, "kind_smart"}};
    const theme::Tokens t = theme::tokens();
    const auto found = names.find(kind);
    const QString key = QString::fromLatin1(found != names.end() ? found->second : "kind_other") + t.muted;
    if (const auto kept = marks_.find(key); kept != marks_.end()) return kept->second;
    const QSize size = list_->iconSize();
    QPixmap pixmap(size);
    pixmap.fill(Qt::transparent);
    const int side = std::min({18, size.width(), size.height()});  // (a small quiet mark in the picture's room)
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawPixmap((size.width() - side) / 2, (size.height() - side) / 2,
                       icons::icon(found != names.end() ? found->second : "kind_other", t.muted, t.muted).pixmap(side, side));
    painter.end();
    QIcon mark;
    mark.addPixmap(pixmap, QIcon::Normal);
    mark.addPixmap(pixmap, QIcon::Selected);  // (still: not tinted when its row is chosen)
    marks_[key] = mark;
    return mark;
}

}  // namespace genko::app
