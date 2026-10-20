#include "app/shapes_panel.hpp"

#include <QComboBox>
#include <QHBoxLayout>
#include <QImage>
#include <QListWidget>
#include <QPixmap>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

#include "app/ask.hpp"
#include "app/main_window.hpp"
#include "core/config_dir.hpp"
#include "core/error.hpp"
#include "service/wording.hpp"

namespace genko::app {

namespace presets = render::shape_presets;

namespace {

constexpr int kName = Qt::UserRole + 11;
constexpr int kSwatch = 40;

QIcon swatch(const core::vector::Path& path) {
    const render::Image grey = presets::thumbnail(path, kSwatch);
    const std::string bytes = grey.tobytes();
    QImage icon(kSwatch, kSwatch, QImage::Format_RGB32);
    for (int y = 0; y < kSwatch; ++y) {
        for (int x = 0; x < kSwatch; ++x) {
            const int v = 255 - static_cast<unsigned char>(bytes[static_cast<std::size_t>(y) * kSwatch + x]) * 3 / 4;
            icon.setPixel(x, y, qRgb(v, v, v));
        }
    }
    return QIcon(QPixmap::fromImage(icon));
}

QPushButton* make_button(const QString& text, const QString& tip, const QString& name) {
    auto* b = new QPushButton(text);
    b->setToolTip(tip);
    b->setObjectName(name);
    return b;
}

}  // namespace

ShapesPanel::ShapesPanel(MainWindow* window) : QWidget(window), window_(window) {
    setObjectName(QStringLiteral("shapes_panel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    groups_ = new QComboBox;
    groups_->setObjectName(QStringLiteral("shapes_groups"));
    layout->addWidget(groups_);
    list_ = new QListWidget;
    list_->setObjectName(QStringLiteral("shapes_list"));
    list_->setViewMode(QListView::IconMode);
    list_->setIconSize(QSize(kSwatch, kSwatch));
    list_->setResizeMode(QListView::Adjust);
    list_->setMovement(QListView::Static);
    list_->setSpacing(4);
    list_->setToolTip(QStringLiteral("クリックでカスタムシェイプツールの形にします。ダブルクリックでページに置きます"));
    layout->addWidget(list_, 1);
    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    QPushButton* put = make_button(QStringLiteral("配置"), QStringLiteral("選んだ形を、描く先のペンのレイヤーにシェイプとして置きます"), QStringLiteral("shapes_place"));
    QPushButton* drop = make_button(QStringLiteral("削除"), QStringLiteral("選んだ形をプリセットから削除します"), QStringLiteral("shapes_delete"));
    QPushButton* again = make_button(QStringLiteral("初期設定に戻す"), QStringLiteral("プリセットを最初の形の組に戻します"), QStringLiteral("shapes_reset"));
    for (QPushButton* b : {put, drop, again}) row->addWidget(b);
    layout->addLayout(row);
    connect(put, &QPushButton::clicked, this, [this] { place(); });
    connect(drop, &QPushButton::clicked, this, [this] { remove(); });
    connect(again, &QPushButton::clicked, this, [this] { reset(); });
    connect(groups_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (!loading_) show_group();
    });
    connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem*) {
        const auto [group, name] = chosen();
        if (!name.isEmpty()) window_->choose_custom_shape(group, name);
    });
    connect(list_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*) { place(); });
    refresh();
}

QPushButton* ShapesPanel::button(const QString& name) const { return findChild<QPushButton*>(name); }

void ShapesPanel::refresh() {
    std::string problem;
    presets_ = presets::load(core::config_dir(), &problem);
    loading_ = true;
    const QString was = groups_->currentText();
    groups_->clear();
    for (const presets::Group& g : presets_) groups_->addItem(QString::fromStdString(g.name));
    const int at = groups_->findText(was);
    groups_->setCurrentIndex(std::max(0, at));
    loading_ = false;
    show_group();
}

void ShapesPanel::show_group() {
    list_->clear();
    const int at = groups_->currentIndex();
    if (at < 0 || static_cast<std::size_t>(at) >= presets_.size()) return;
    for (const presets::Preset& p : presets_[static_cast<std::size_t>(at)].shapes) {
        auto* item = new QListWidgetItem(swatch(p.path), QString::fromStdString(p.name));
        item->setData(kName, QString::fromStdString(p.name));
        item->setToolTip(QString::fromStdString(p.name));
        list_->addItem(item);
    }
}

std::pair<QString, QString> ShapesPanel::chosen() const {
    const QListWidgetItem* item = list_->currentItem();
    if (item == nullptr) return {groups_->currentText(), QString()};
    return {groups_->currentText(), item->data(kName).toString()};
}

bool ShapesPanel::place() {
    const auto [group, name] = chosen();
    if (name.isEmpty()) {
        window_->flash(QStringLiteral("置く形をシェイプ パネルで選びます"), 3000);
        return false;
    }
    return window_->place_shape_preset(group, name);
}

bool ShapesPanel::remove() {
    const auto [group, name] = chosen();
    if (name.isEmpty()) return false;
    std::vector<presets::Group> groups = presets_;
    try {
        presets::edit(groups, core::Json{{"action", "delete"}, {"preset", name.toStdString()}, {"group", group.toStdString()}});
        presets::save(core::config_dir(), groups);
    } catch (const std::exception& error) {
        window_->flash(wording::error(QString::fromUtf8(error.what())), 4000, true);
        return false;
    }
    refresh();
    return true;
}

void ShapesPanel::reset() {
    try {
        presets::save(core::config_dir(), presets::reset(presets_, false));
    } catch (const std::exception& error) {
        window_->flash(wording::error(QString::fromUtf8(error.what())), 4000, true);
        return;
    }
    refresh();
}

}  // namespace genko::app
