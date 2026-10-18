// コピーソース (PC3 B4, window.panel.cloneSource): PhotoCraft v0.2.0's engine/src/presets/clone_source.rs (cloneSource.set,
// .select, .resetTransform, .overlay; MIT: native/third_party/photocraft/NOTICE.md) as a Qt panel over the window's
// sources.

#include "app/clone_source_panel.hpp"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFont>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

#include "app/main_window.hpp"
#include "app/theme.hpp"

namespace genko::app {

namespace {

QDoubleSpinBox* number(const char* name, double lo, double hi, double step, const QString& suffix, int decimals = 1) {
    auto* box = new QDoubleSpinBox;
    box->setObjectName(QString::fromLatin1(name));
    box->setRange(lo, hi);
    box->setSingleStep(step);
    box->setDecimals(decimals);
    box->setSuffix(suffix);
    return box;
}

QLabel* section(const QString& text) {
    auto* label = new QLabel(text);
    theme::role(label, "section");
    return label;
}

}  // namespace

CloneSourcePanel::CloneSourcePanel(MainWindow* window) : window_(window) {
    setObjectName(QStringLiteral("clone_source_panel"));
    auto* layout = new QVBoxLayout(this);
    // the five sources
    auto* row = new QHBoxLayout;
    auto* group = new QButtonGroup(this);
    group->setExclusive(true);
    for (int i = 0; i < 5; ++i) {
        auto* button = new QToolButton;
        button->setObjectName(QStringLiteral("clone_slot_%1").arg(i));
        button->setText(QString::number(i + 1));
        button->setCheckable(true);
        button->setToolTip(QStringLiteral("コピーソース %1 を使う（Alt+クリックで、使っているソースの場所を決める）").arg(i + 1));
        group->addButton(button, i);
        row->addWidget(button);
        slots_[static_cast<std::size_t>(i)] = button;
    }
    row->addStretch(1);
    connect(group, &QButtonGroup::idClicked, this, [this](int id) {
        if (quiet_) return;
        window_->clone_sources.active = id;
        window_->clone_sources_follow();
    });
    layout->addLayout(row);
    where_ = new QLabel;
    where_->setObjectName(QStringLiteral("clone_where"));
    where_->setWordWrap(true);
    theme::hint(where_);
    layout->addWidget(where_);
    // the source's offset and transform
    auto* form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    offset_x_ = number("clone_offset_x", -10000, 10000, 1, QStringLiteral(" mm"), 2);
    offset_y_ = number("clone_offset_y", -10000, 10000, 1, QStringLiteral(" mm"), 2);
    auto* offsets = new QHBoxLayout;
    offsets->addWidget(new QLabel(QStringLiteral("X")));
    offsets->addWidget(offset_x_);
    offsets->addWidget(new QLabel(QStringLiteral("Y")));
    offsets->addWidget(offset_y_);
    form->addRow(QStringLiteral("オフセット"), offsets);
    width_ = number("clone_width", 1, 1000, 1, QStringLiteral(" %"));
    height_ = number("clone_height", 1, 1000, 1, QStringLiteral(" %"));
    auto* sizes = new QHBoxLayout;
    sizes->addWidget(new QLabel(QStringLiteral("W")));
    sizes->addWidget(width_);
    sizes->addWidget(new QLabel(QStringLiteral("H")));
    sizes->addWidget(height_);
    form->addRow(QStringLiteral("拡大・縮小"), sizes);
    rotation_ = number("clone_rotation", -180, 180, 1, QStringLiteral(" °"));
    form->addRow(QStringLiteral("回転"), rotation_);
    flip_h_ = new QToolButton;
    flip_h_->setObjectName(QStringLiteral("clone_flip_h"));
    flip_h_->setText(QStringLiteral("左右に反転"));
    flip_h_->setCheckable(true);
    flip_v_ = new QToolButton;
    flip_v_->setObjectName(QStringLiteral("clone_flip_v"));
    flip_v_->setText(QStringLiteral("上下に反転"));
    flip_v_->setCheckable(true);
    reset_ = new QPushButton(QStringLiteral("変形をリセット"));
    reset_->setObjectName(QStringLiteral("clone_reset"));
    auto* flips = new QHBoxLayout;
    flips->addWidget(flip_h_);
    flips->addWidget(flip_v_);
    form->addRow(QString(), flips);
    form->addRow(QString(), reset_);
    layout->addLayout(form);
    // the overlay
    layout->addWidget(section(QStringLiteral("オーバーレイ")));
    auto* overlay = new QFormLayout;
    overlay->setContentsMargins(0, 0, 0, 0);
    show_ = new QCheckBox(QStringLiteral("オーバーレイを表示"));
    show_->setObjectName(QStringLiteral("clone_show"));
    opacity_ = new QSpinBox;
    opacity_->setObjectName(QStringLiteral("clone_opacity"));
    opacity_->setRange(0, 100);
    opacity_->setSuffix(QStringLiteral(" %"));
    clipped_ = new QCheckBox(QStringLiteral("クリップ"));
    clipped_->setObjectName(QStringLiteral("clone_clipped"));
    clipped_->setToolTip(QStringLiteral("ブラシの中だけに重ねて見せる"));
    auto_hide_ = new QCheckBox(QStringLiteral("自動的に隠す"));
    auto_hide_->setObjectName(QStringLiteral("clone_auto_hide"));
    auto_hide_->setToolTip(QStringLiteral("描いている間は隠す"));
    invert_ = new QCheckBox(QStringLiteral("反転"));
    invert_->setObjectName(QStringLiteral("clone_invert"));
    blend_ = new QComboBox;
    blend_->setObjectName(QStringLiteral("clone_blend"));
    for (const auto& [label, key] : {std::pair{QStringLiteral("通常"), QStringLiteral("normal")}, {QStringLiteral("比較（暗）"), QStringLiteral("darken")},
                                     {QStringLiteral("比較（明）"), QStringLiteral("lighten")}, {QStringLiteral("差の絶対値"), QStringLiteral("difference")}})
        blend_->addItem(label, key);
    overlay->addRow(QString(), show_);
    overlay->addRow(QStringLiteral("不透明度"), opacity_);
    overlay->addRow(QString(), clipped_);
    overlay->addRow(QString(), auto_hide_);
    overlay->addRow(QString(), invert_);
    overlay->addRow(QStringLiteral("描画モード"), blend_);
    layout->addLayout(overlay);
    layout->addStretch(1);
    for (QDoubleSpinBox* box : {offset_x_, offset_y_, width_, height_, rotation_}) connect(box, &QDoubleSpinBox::valueChanged, this, [this] { changed(); });
    for (QToolButton* button : {flip_h_, flip_v_}) connect(button, &QToolButton::toggled, this, [this] { changed(); });
    for (QCheckBox* box : {show_, clipped_, auto_hide_, invert_}) connect(box, &QCheckBox::toggled, this, [this] { changed(); });
    connect(opacity_, &QSpinBox::valueChanged, this, [this] { changed(); });
    connect(blend_, &QComboBox::currentIndexChanged, this, [this] { changed(); });
    connect(reset_, &QPushButton::clicked, this, [this] {
        MainWindow::CloneSource& slot = window_->clone_sources.now();
        slot.width = slot.height = 100;
        slot.rotation = 0;
        slot.flip_h = slot.flip_v = false;
        window_->clone_sources_follow();
    });
    refresh();
}

void CloneSourcePanel::refresh() {
    quiet_ = true;
    const MainWindow::CloneSources& all = window_->clone_sources;
    const MainWindow::CloneSource& slot = all.items[static_cast<std::size_t>(std::clamp(all.active, 0, 4))];
    for (int i = 0; i < 5; ++i) {
        QToolButton* button = slots_[static_cast<std::size_t>(i)];
        const QSignalBlocker hold(button);
        button->setChecked(i == all.active);
        // (a source that is set shows it)
        QFont font = button->font();
        font.setBold(all.items[static_cast<std::size_t>(i)].source.has_value());
        button->setFont(font);
    }
    if (slot.source) {
        where_->setText(QStringLiteral("ソース %1: (%2, %3) mm").arg(all.active + 1).arg(slot.source->x(), 0, 'f', 1).arg(slot.source->y(), 0, 'f', 1));
    } else {
        where_->setText(QStringLiteral("ソース %1: まだありません（Alt キーを押しながらクリックして決めます）").arg(all.active + 1));
    }
    const bool paired = slot.source && slot.anchor;
    offset_x_->setEnabled(paired);
    offset_y_->setEnabled(paired);
    {
        const QSignalBlocker a(offset_x_), b(offset_y_), c(width_), d(height_), e(rotation_), f(flip_h_), g(flip_v_);
        offset_x_->setValue(paired ? slot.source->x() - slot.anchor->x() : 0.0);
        offset_y_->setValue(paired ? slot.source->y() - slot.anchor->y() : 0.0);
        width_->setValue(slot.width);
        height_->setValue(slot.height);
        rotation_->setValue(slot.rotation);
        flip_h_->setChecked(slot.flip_h);
        flip_v_->setChecked(slot.flip_v);
    }
    {
        const QSignalBlocker a(show_), b(opacity_), c(clipped_), d(auto_hide_), e(invert_), f(blend_);
        show_->setChecked(all.overlay);
        opacity_->setValue(static_cast<int>(std::lround(all.opacity)));
        clipped_->setChecked(all.clipped);
        auto_hide_->setChecked(all.auto_hide);
        invert_->setChecked(all.invert);
        blend_->setCurrentIndex(std::max(0, blend_->findData(QString::fromStdString(all.blend))));
    }
    quiet_ = false;
}

void CloneSourcePanel::changed() {
    if (quiet_) return;
    MainWindow::CloneSources& all = window_->clone_sources;
    MainWindow::CloneSource& slot = all.now();
    if (slot.source && slot.anchor) {  // (the offset moves the source, the destination it is paired with kept)
        slot.source = QPointF(slot.anchor->x() + offset_x_->value(), slot.anchor->y() + offset_y_->value());
    }
    slot.width = width_->value();
    slot.height = height_->value();
    slot.rotation = rotation_->value();
    slot.flip_h = flip_h_->isChecked();
    slot.flip_v = flip_v_->isChecked();
    all.overlay = show_->isChecked();
    all.opacity = opacity_->value();
    all.clipped = clipped_->isChecked();
    all.auto_hide = auto_hide_->isChecked();
    all.invert = invert_->isChecked();
    all.blend = blend_->currentData().toString().toStdString();
    window_->clone_sources_follow();
}

}  // namespace genko::app
