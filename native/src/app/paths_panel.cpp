#include "app/paths_panel.hpp"

#include <QGridLayout>
#include <QImage>
#include <QListWidget>
#include <QPixmap>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "app/ask.hpp"
#include "app/canvas.hpp"
#include "app/main_window.hpp"
#include "render/path_raster.hpp"

namespace genko::app {

using core::Json;
namespace vec = core::vector;

namespace {

constexpr int kKey = Qt::UserRole + 10;

QPushButton* make_button(const QString& text, const QString& tip, const QString& name) {
    auto* b = new QPushButton(text);
    b->setToolTip(tip);
    b->setObjectName(name);
    return b;
}

}  // namespace

PathsPanel::PathsPanel(MainWindow* window) : QWidget(window), window_(window) {
    setObjectName(QStringLiteral("paths_panel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    list_ = new QListWidget;
    list_->setObjectName(QStringLiteral("paths_list"));
    list_->setIconSize(QSize(28, 36));
    list_->setToolTip(QStringLiteral("クリックで、ペンツール・パス選択ツールで直すパスにします。ダブルクリックで名前を変えます（作業用パスは名前を付けて保存）"));
    layout->addWidget(list_, 1);
    auto* grid = new QGridLayout;
    grid->setContentsMargins(0, 0, 0, 0);
    int at = 0;
    const auto add_button = [&](const QString& text, const QString& tip, const QString& name, auto slot) {
        QPushButton* b = make_button(text, tip, name);
        connect(b, &QPushButton::clicked, this, slot);
        grid->addWidget(b, at / 2, at % 2);
        ++at;
    };
    add_button(QStringLiteral("塗りつぶす"), QStringLiteral("パスを塗りつぶす: パスの中を描く先のレイヤーに描画色で塗ります"), QStringLiteral("path_fill"),
               [this] { window_->path_fill(); });
    add_button(QStringLiteral("境界線を描く"), QStringLiteral("ブラシでパスの境界線を描く: パスに沿って、今のペンで線を引きます"), QStringLiteral("path_stroke"),
               [this] { window_->path_stroke(); });
    add_button(QStringLiteral("選択範囲に"), QStringLiteral("パスを選択範囲として読み込む（Shift で足す、Alt で引く）"), QStringLiteral("path_selection"),
               [this] { window_->path_to_selection(); });
    add_button(QStringLiteral("作業用パスを作成"), QStringLiteral("選択範囲から作業用パスを作成: 選択範囲の輪郭を曲線のパスにします"),
               QStringLiteral("path_from_selection"), [this] { window_->work_path_from_selection(); });
    add_button(QStringLiteral("マスクを追加"), QStringLiteral("描く先のレイヤーに、このパスでベクトルマスクを付けます"), QStringLiteral("path_mask"),
               [this] { window_->vector_mask_command("current_path"); });
    add_button(QStringLiteral("新規パス"), QStringLiteral("新規パスを作成: 名前の付いた空のパスを作り、ペンツールで描く先にします"), QStringLiteral("path_new"),
               [this] { add_new(); });
    add_button(QStringLiteral("削除"), QStringLiteral("パスを削除"), QStringLiteral("path_delete"), [this] { remove(); });
    layout->addLayout(grid);
    connect(list_, &QListWidget::itemClicked, this, &PathsPanel::clicked);
    connect(list_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item, QListWidgetItem*) {
        if (item != nullptr && !loading_) clicked(item);
    });
    connect(list_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*) { rename(); });
}

QPushButton* PathsPanel::button(const QString& name) const { return findChild<QPushButton*>(name); }

QIcon PathsPanel::thumbnail(const vec::Path& path, double width_mm, double height_mm) {
    const int h = 36;
    const int w = std::max(8, static_cast<int>(std::lround(h * width_mm / std::max(1.0, height_mm))));
    const double dpi = h / std::max(1.0, height_mm) * 25.4;
    const render::Image cover = render::path_raster::path_mask(path, dpi, render::Size{w, h});
    const std::string grey = cover.tobytes();
    QImage icon(w, h, QImage::Format_RGB32);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int v = 255 - static_cast<unsigned char>(grey[static_cast<std::size_t>(y) * w + x]) * 3 / 5;
            icon.setPixel(x, y, qRgb(v, v, v));
        }
    }
    return QIcon(QPixmap::fromImage(icon));
}

void PathsPanel::refresh() {
    loading_ = true;
    list_->clear();
    keys_.clear();
    const core::Page* page = window_->current_page();
    if (page == nullptr) {
        loading_ = false;
        return;
    }
    const double w = page->spec.width_mm.value(), h = page->spec.height_mm.value();
    const QString target = window_->canvas()->target_path;
    const auto add_row = [&](const QString& label, const QString& key, const vec::Path& path) {
        auto* item = new QListWidgetItem(thumbnail(path, w, h), label);
        item->setData(kKey, key);
        list_->addItem(item);
        keys_.push_back(key);
        if (key == target) list_->setCurrentItem(item);
    };
    if (page->work_path) add_row(QStringLiteral("作業用パス"), QStringLiteral("work"), *page->work_path);
    for (const vec::NamedPath& named : page->paths) add_row(QString::fromStdString(named.name), QStringLiteral("saved:") + QString::fromStdString(named.name), named.path);
    if (const core::Layer* layer = window_->target_layer(); layer != nullptr && layer->vector_mask) {
        const QString title = QString::fromStdString(layer->title.empty() ? layer->id : layer->title);
        add_row(QStringLiteral("%1 のベクトルマスク").arg(title), QStringLiteral("mask:") + QString::fromStdString(layer->id), layer->vector_mask->path);
    }
    // (PC3 C-b: the shape parts of the pen layer drawn on — and of the layer of the part chosen on the page)
    std::vector<const core::Layer*> holders;
    if (const core::Layer* layer = window_->target_layer(); layer != nullptr) holders.push_back(layer);
    if (target.startsWith(QLatin1String("shape:"))) {
        const std::string id = target.mid(6).section(QLatin1Char('/'), 0, 0).toStdString();
        for (const core::Layer& layer : page->layers)
            if (layer.id == id && std::find(holders.begin(), holders.end(), &layer) == holders.end()) holders.push_back(&layer);
    }
    for (const core::Layer* layer : holders) {
        if (!layer->shapes) continue;
        const QString title = QString::fromStdString(layer->title.empty() ? layer->id : layer->title);
        int n = 0;
        for (const vec::ShapePart& part : layer->shapes->items) {
            ++n;
            const QString name = part.name ? QString::fromStdString(*part.name) : QStringLiteral("形 %1").arg(n);
            add_row(QStringLiteral("%1 の %2").arg(title, name),
                    QStringLiteral("shape:%1/%2").arg(QString::fromStdString(layer->id), QString::fromStdString(part.id)), part.path);
        }
    }
    loading_ = false;
}

void PathsPanel::clicked(QListWidgetItem* item) {
    if (item == nullptr) return;
    window_->canvas()->target_path = item->data(kKey).toString();
    window_->canvas()->path_subpath.reset();
    window_->canvas()->update();
}

void PathsPanel::add_new(std::optional<QString> name) {
    const core::Page* page = window_->current_page();
    if (page == nullptr) return;
    QString chosen;
    if (name) {
        chosen = *name;
    } else {
        // (パス 1, パス 2, …: the first not taken)
        int n = 1;
        const auto taken = [&](const QString& candidate) {
            return std::any_of(page->paths.begin(), page->paths.end(), [&](const vec::NamedPath& p) { return QString::fromStdString(p.name) == candidate; });
        };
        while (taken(QStringLiteral("パス %1").arg(n))) ++n;
        chosen = QStringLiteral("パス %1").arg(n);
    }
    if (chosen.trimmed().isEmpty()) return;
    if (!window_->apply_ops(Json::array({Json{{"op", "set_path"}, {"page", page->index.json()}, {"name", chosen.toStdString()},
                                              {"path", Json{{"subpaths", Json::array()}}}}})))
        return;
    window_->canvas()->target_path = QStringLiteral("saved:") + chosen.trimmed();
    refresh();
}

void PathsPanel::remove() {
    const core::Page* page = window_->current_page();
    if (page == nullptr) return;
    const QString key = window_->canvas()->target_path;
    if (key.startsWith(QLatin1String("mask:"))) {
        window_->vector_mask_command("delete");
    } else if (key.startsWith(QLatin1String("shape:"))) {  // (PC3 C-b: the shape part goes, as Photoshop deletes the shape layer's path)
        const QString rest = key.mid(6);
        if (!window_->apply_ops(Json::array({Json{{"op", "vector_edit"}, {"page", page->index.json()}, {"layer_id", rest.section(QLatin1Char('/'), 0, 0).toStdString()},
                                                  {"action", "delete"}, {"stroke_id", rest.section(QLatin1Char('/'), 1).toStdString()}}})))
            return;
        window_->canvas()->target_path = QStringLiteral("work");
    } else {
        Json op{{"op", "delete_path"}, {"page", page->index.json()}};
        if (key.startsWith(QLatin1String("saved:"))) op["name"] = key.mid(6).toStdString();
        if (!window_->apply_ops(Json::array({op}))) return;
        window_->canvas()->target_path = QStringLiteral("work");
    }
    refresh();
}

void PathsPanel::rename(std::optional<QString> name) {
    const core::Page* page = window_->current_page();
    if (page == nullptr) return;
    const QString key = window_->canvas()->target_path;
    if (key.startsWith(QLatin1String("mask:"))) return;
    if (key.startsWith(QLatin1String("shape:"))) {  // (PC3 C-b: the shape part's name)
        const QString rest = key.mid(6);
        QString chosen = name.value_or(QString());
        if (!name) {
            const auto asked = ask::get_text(this, QStringLiteral("シェイプの名前を変更"), QStringLiteral("シェイプの名前"));
            if (!asked) return;
            chosen = *asked;
        }
        if (chosen.trimmed().isEmpty()) return;
        window_->apply_ops(Json::array({Json{{"op", "edit_shape_part"}, {"page", page->index.json()}, {"layer_id", rest.section(QLatin1Char('/'), 0, 0).toStdString()},
                                             {"id", rest.section(QLatin1Char('/'), 1).toStdString()}, {"name", chosen.trimmed().toStdString()}}}));
        refresh();
        return;
    }
    const bool work = key == QLatin1String("work");
    if (work && !page->work_path) return;
    QString chosen;
    if (name) {
        chosen = *name;
    } else {
        const auto asked = ask::get_text(this, work ? QStringLiteral("パスを保存") : QStringLiteral("パスの名前を変更"), QStringLiteral("パスの名前"),
                                         work ? QStringLiteral("パス %1").arg(page->paths.size() + 1) : key.mid(6));
        if (!asked) return;
        chosen = *asked;
    }
    Json op{{"op", "rename_path"}, {"page", page->index.json()}, {"to", chosen.toStdString()}};
    if (!work) op["name"] = key.mid(6).toStdString();
    if (!window_->apply_ops(Json::array({op}))) return;
    window_->canvas()->target_path = QStringLiteral("saved:") + chosen.trimmed();
    refresh();
}

}  // namespace genko::app
