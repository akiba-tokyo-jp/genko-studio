#include "app/channel_panel.hpp"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPixmap>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "app/ask.hpp"
#include "app/main_window.hpp"
#include "core/channels.hpp"
#include "render/channels.hpp"

namespace genko::app {

using core::Json;

namespace {

constexpr int kKey = Qt::UserRole + 10;

QString composite_label(const core::Page& page) {
    if (page.color) {
        switch (page.color->space) {
            case core::InkSpace::Cmyk: return QStringLiteral("CMYK");
            case core::InkSpace::Lab: return QStringLiteral("Lab");
            case core::InkSpace::Gray: return QStringLiteral("グレー");
            case core::InkSpace::Multi: return QStringLiteral("マルチチャンネル");
            default: break;
        }
    }
    return QStringLiteral("RGB");
}

QPushButton* make_button(const QString& text, const QString& tip, const QString& name) {
    auto* b = new QPushButton(text);
    b->setToolTip(tip);
    b->setObjectName(name);
    return b;
}

}  // namespace

ChannelPanel::ChannelPanel(MainWindow* window) : QWidget(window), window_(window) {
    setObjectName(QStringLiteral("channel_panel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    list_ = new QListWidget;
    list_->setObjectName(QStringLiteral("channel_list"));
    list_->setIconSize(QSize(28, 36));
    list_->setToolTip(QStringLiteral("クリックで描く先に（ペン・消しゴム・塗り・フィルター）。目のチェックで見せる・隠す。チャンネルはダブルクリックでオプション"));
    layout->addWidget(list_, 1);
    auto* row = new QHBoxLayout;
    const auto add_button = [&](const QString& text, const QString& tip, const QString& name, auto slot) {
        QPushButton* b = make_button(text, tip, name);
        connect(b, &QPushButton::clicked, this, slot);
        row->addWidget(b);
    };
    add_button(QStringLiteral("新規"), QStringLiteral("アルファチャンネルを新しく作る（黒: 何も選ばれていない）"), QStringLiteral("channel_new"), [this] { add(false); });
    add_button(QStringLiteral("特色"), QStringLiteral("特色チャンネルを新しく作る（選択範囲があればその所に）"), QStringLiteral("channel_spot"), [this] { add(true); });
    add_button(QStringLiteral("複製"), QStringLiteral("選んだチャンネル（色の版・合成も）を複製する"), QStringLiteral("channel_duplicate"), [this] { duplicate(); });
    add_button(QStringLiteral("削除"), QStringLiteral("選んだチャンネルを削除する（色の版を削除するとマルチチャンネルになります）"), QStringLiteral("channel_delete"), [this] { remove(); });
    layout->addLayout(row);
    auto* row2 = new QHBoxLayout;
    const auto add_button2 = [&](const QString& text, const QString& tip, const QString& name, auto slot) {
        QPushButton* b = make_button(text, tip, name);
        connect(b, &QPushButton::clicked, this, slot);
        row2->addWidget(b);
    };
    add_button2(QStringLiteral("選択範囲として読み込む"), QStringLiteral("選んだチャンネルの白い所を選択範囲にする"), QStringLiteral("channel_load"),
                [this] { load_selection(); });
    add_button2(QStringLiteral("選択範囲を保存"), QStringLiteral("選択範囲を新しいアルファチャンネルとして残す"), QStringLiteral("channel_save"),
                [this] { save_selection(); });
    menu_ = new QMenu(this);
    menu_->setObjectName(QStringLiteral("channel_menu"));
    menu_->addAction(QStringLiteral("チャンネルのオプション…"), this, [this] { options(); });
    menu_->addSeparator();
    menu_->addAction(QStringLiteral("チャンネルを分割"), this, [this] { split(); });
    menu_->addAction(QStringLiteral("チャンネルを統合…"), this, [this] { merge(); });
    menu_->addAction(QStringLiteral("特色チャンネルを合成"), this, [this] { merge_spot(); });
    QPushButton* more = make_button(QStringLiteral("…"), QStringLiteral("オプション・分割・統合・特色を合成"), QStringLiteral("channel_more"));
    more->setMenu(menu_);
    row2->addWidget(more);
    layout->addLayout(row2);
    connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) { clicked(item); });
    connect(list_, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) { eye(item); });
    connect(list_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        const std::string key = item->data(kKey).toString().toStdString();
        if (key.starts_with("stock:")) return;  // (a click already used it)
        if (core::is_channel_id(key)) options();
    });
}

QPushButton* ChannelPanel::button(const QString& name) const { return findChild<QPushButton*>(name); }

void ChannelPanel::refresh() {
    loading_ = true;
    list_->clear();
    keys_.clear();
    const core::Page* page = window_->current_page();
    if (page == nullptr) {
        loading_ = false;
        return;
    }
    const MainWindow::ChannelView view = window_->channel_view();
    const auto add_row = [&](const std::string& key, const QString& text, std::optional<bool> eye, const QIcon& icon, const QString& tip) {
        auto* item = new QListWidgetItem(icon, text, list_);
        item->setData(kKey, QString::fromStdString(key));
        if (eye) {
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(*eye ? Qt::Checked : Qt::Unchecked);
        }
        if (!tip.isEmpty()) item->setToolTip(tip);
        keys_.push_back(key);
        return item;
    };
    const bool all = std::none_of(view.hidden.begin(), view.hidden.end(), [](bool h) { return h; });
    const std::vector<std::string> plates = core::plate_names(*page);
    const std::vector<std::string> labels = core::plate_labels(*page);
    QListWidgetItem* chosen = add_row("composite", composite_label(*page), all, QIcon(), QStringLiteral("合成（Ctrl+Alt+2）"));
    int slot = 3;
    if (plates.size() > 1) {
        for (std::size_t k = 0; k < plates.size(); ++k) {
            QListWidgetItem* item = add_row(plates[k], QString::fromStdString(labels[k]), !view.hidden[k], QIcon(),
                                            QStringLiteral("色の版（Ctrl+Alt+%1）").arg(slot++));
            if (view.target == plates[k]) chosen = item;
        }
    }
    std::map<std::string, Thumb> kept;
    for (const core::Channel& channel : page->channels) {
        const bool spot = channel.kind == core::ChannelKind::Spot;
        const bool shown = spot ? !view.hidden_spots.contains(channel.id) : view.shown.contains(channel.id);
        QString tip = spot ? QStringLiteral("特色チャンネル") : QStringLiteral("アルファチャンネル");
        if (slot <= 9) tip += QStringLiteral("（Ctrl+Alt+%1）").arg(slot);
        ++slot;
        QListWidgetItem* item = add_row(channel.id, QString::fromStdString(channel.name) + (spot ? QStringLiteral("（特色）") : QString()), shown,
                                        thumbnail(*page, channel), tip);
        if (view.target == channel.id) chosen = item;
        if (const auto it = thumbs_.find(channel.id); it != thumbs_.end()) kept.emplace(channel.id, it->second);
    }
    thumbs_ = std::move(kept);
    // (the areas kept on the page, Python's ストック: their own rows)
    if (page->extra.is_object() && page->extra.contains("saved_areas") && page->extra["saved_areas"].is_object()) {
        for (const auto& [name, area] : page->extra["saved_areas"].items()) {
            add_row("stock:" + name, QStringLiteral("ストック: ") + QString::fromStdString(name), std::nullopt, QIcon(),
                    QStringLiteral("クリックで選択範囲に（選択 → ストックから選ぶ と同じ）"));
        }
    }
    list_->setCurrentItem(chosen);
    loading_ = false;
}

QIcon ChannelPanel::thumbnail(const core::Page& page, const core::Channel& channel) {
    const auto it = thumbs_.find(channel.id);
    if (it != thumbs_.end() && it->second.raster == channel.raster) return it->second.icon;
    const double w = page.spec.width_mm.value(), h = page.spec.height_mm.value();
    const int tw = w >= h ? 36 : std::max(1, static_cast<int>(std::lround(36 * w / h)));
    const int th = w >= h ? std::max(1, static_cast<int>(std::lround(36 * h / w))) : 36;
    QIcon icon;
    try {
        const render::Image mask = render::channels::thumbnail(channel, render::Size{tw, th});  // (read row by row, not kept)
        const std::string bytes = mask.tobytes();
        QImage image(reinterpret_cast<const uchar*>(bytes.data()), tw, th, tw, QImage::Format_Grayscale8);
        icon = QIcon(QPixmap::fromImage(image.copy()));
    } catch (const std::exception&) {
    }
    thumbs_[channel.id] = Thumb{channel.raster, icon};
    return icon;
}

void ChannelPanel::clicked(QListWidgetItem* item) {
    if (loading_ || item == nullptr) return;
    const std::string key = item->data(kKey).toString().toStdString();
    if (key.starts_with("stock:")) {  // (a kept area: the selection, as ストックから選ぶ)
        window_->select_stock(key.substr(6));
        return;
    }
    window_->target_channel(key == "composite" ? std::string() : key);
}

void ChannelPanel::eye(QListWidgetItem* item) {
    if (loading_ || item == nullptr || !(item->flags() & Qt::ItemIsUserCheckable)) return;
    window_->show_channel(item->data(kKey).toString().toStdString(), item->checkState() == Qt::Checked);
}

std::string ChannelPanel::chosen_channel() const {
    const QListWidgetItem* item = list_->currentItem();
    if (item == nullptr) return {};
    const std::string key = item->data(kKey).toString().toStdString();
    return core::is_channel_id(key) ? key : std::string();
}

void ChannelPanel::add(bool spot) {
    const core::Page* page = window_->current_page();
    if (page == nullptr) return;
    Json op{{"op", "add_channel"}, {"page", page->index.json()}, {"kind", spot ? "spot" : "alpha"}};
    if (spot) {
        if (const auto area = window_->selection_area()) op["area"] = *area;  // (from the selection, as PhotoCraft's)
    }
    if (window_->apply_ops(Json::array({op}))) {
        const core::Page* now = window_->current_page();
        if (now != nullptr && !now->channels.empty() && !spot) window_->target_channel(now->channels.back().id);  // (PhotoCraft targets a new one)
        refresh();
    }
}

void ChannelPanel::duplicate() {
    const core::Page* page = window_->current_page();
    const QListWidgetItem* item = list_->currentItem();
    if (page == nullptr || item == nullptr) return;
    const std::string key = item->data(kKey).toString().toStdString();
    if (key.starts_with("stock:")) return;
    window_->apply_ops(Json::array({Json{{"op", "duplicate_channel"}, {"page", page->index.json()}, {"channel", key}}}));
    refresh();
}

void ChannelPanel::remove() {
    const core::Page* page = window_->current_page();
    const std::string id = chosen_channel();
    const QListWidgetItem* item = list_->currentItem();
    const std::string key = item != nullptr ? item->data(kKey).toString().toStdString() : std::string();
    if (page != nullptr && id.empty() && core::plate_index(*page, key)) {
        // (color-model@1, PC1d: a colour plate deleted makes the page multichannel — Photoshop's Delete Channel —, asked
        // first: its pixel layers flattened; a multichannel page's plate goes as it is)
        const bool multi = page->color && page->color->space == core::InkSpace::Multi;
        if (core::plate_names(*page).size() < 2) {
            window_->flash(QStringLiteral("色の版が 1 つしかないので削除できません"), 3000);
            return;
        }
        if (!multi && !ask::question(this, QStringLiteral("色の版を削除"),
                                     QStringLiteral("色の版を削除すると、このページはマルチチャンネルになります（画素レイヤーは 1 枚に統合され、"
                                                    "残りの版はそれぞれインキの版になります）。削除しますか？\n編集 → 取り消し で元に戻せます。"))) {
            return;
        }
        Json op{{"op", "delete_channel"}, {"page", page->index.json()}, {"channel", key}};
        if (!multi) op["to_multichannel"] = true;
        window_->apply_ops(Json::array({op}));
        refresh();
        return;
    }
    if (page == nullptr || id.empty()) {
        window_->flash(QStringLiteral("削除するチャンネルを選びます"), 3000);
        return;
    }
    window_->apply_ops(Json::array({Json{{"op", "delete_channel"}, {"page", page->index.json()}, {"channel", id}}}));
    refresh();
}

void ChannelPanel::load_selection() {
    const std::string id = chosen_channel();
    if (id.empty()) {
        window_->flash(QStringLiteral("選択範囲にするアルファ・特色チャンネルを選びます"), 3000);
        return;
    }
    window_->load_selection_from_channel(id);
}

void ChannelPanel::save_selection() {
    if (window_->save_selection_to_channel(QString())) refresh();
}

void ChannelPanel::options() {
    const core::Page* page = window_->current_page();
    const std::string id = chosen_channel();
    if (page == nullptr || id.empty()) return;
    const core::Channel channel = *core::find_channel(*page, id);
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("channel_options"));
    dialog.setWindowTitle(QStringLiteral("チャンネルのオプション"));
    auto* form = new QFormLayout(&dialog);
    auto* name = new QLineEdit(QString::fromStdString(channel.name));
    name->setObjectName(QStringLiteral("channel_name"));
    auto* kind = new QComboBox;
    kind->setObjectName(QStringLiteral("channel_kind"));
    kind->addItem(QStringLiteral("マスク範囲に色を付ける"), QStringLiteral("masked"));
    kind->addItem(QStringLiteral("選択範囲に色を付ける"), QStringLiteral("selected"));
    kind->addItem(QStringLiteral("特色"), QStringLiteral("spot"));
    kind->setCurrentIndex(channel.kind == core::ChannelKind::Spot ? 2 : channel.indicates == "selected" ? 1 : 0);
    QColor colour(channel.color[0], channel.color[1], channel.color[2]);
    auto* swatch = new QPushButton;
    swatch->setObjectName(QStringLiteral("channel_colour"));
    const auto paint = [swatch](const QColor& c) { swatch->setStyleSheet(QStringLiteral("background:%1; min-width:48px").arg(c.name())); };
    paint(colour);
    connect(swatch, &QPushButton::clicked, &dialog, [&] {
        if (const auto picked = ask::colour(&dialog, colour, QStringLiteral("チャンネルの色"))) {
            colour = *picked;
            paint(colour);
        }
    });
    auto* amount = new QSpinBox;
    amount->setObjectName(QStringLiteral("channel_amount"));
    amount->setRange(0, 100);
    amount->setSuffix(QStringLiteral(" %"));
    amount->setValue(static_cast<int>(std::lround((channel.kind == core::ChannelKind::Spot ? channel.solidity : channel.opacity) * 100)));
    form->addRow(QStringLiteral("名前"), name);
    form->addRow(QStringLiteral("色の付け方"), kind);
    form->addRow(QStringLiteral("色"), swatch);
    form->addRow(QStringLiteral("不透明度・濃度"), amount);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);
    const MainWindow::Asked asked = window_->asking();
    if (ask::exec(&dialog) != QDialog::Accepted || !window_->still(asked)) return;
    const QString chosen = kind->currentData().toString();
    Json op{{"op", "set_channel"}, {"page", page->index.json()}, {"channel", id}, {"name", name->text().trimmed().toStdString()},
            {"color", Json::array({colour.red(), colour.green(), colour.blue()})}};
    if (chosen == QLatin1String("spot")) {
        op["kind"] = "spot";
        op["solidity"] = amount->value() / 100.0;
    } else {
        op["kind"] = "alpha";
        op["indicates"] = chosen.toStdString();
        op["opacity"] = amount->value() / 100.0;
    }
    window_->apply_ops(Json::array({op}));
    refresh();
}

void ChannelPanel::split() {
    const core::Page* page = window_->current_page();
    if (page == nullptr) return;
    const MainWindow::Asked asked = window_->asking();
    if (!ask::question(this, QStringLiteral("チャンネルを分割"),
                       QStringLiteral("色の版とチャンネルを 1 枚ずつグレーのページにして、このページと置き換えます（元に戻すで戻せます）。続けますか？")) ||
        !window_->still(asked)) {
        return;
    }
    window_->apply_ops(Json::array({Json{{"op", "split_channels"}, {"page", page->index.json()}}}));
}

void ChannelPanel::merge() {
    const core::Page* page = window_->current_page();
    if (page == nullptr) return;
    const MainWindow::Asked asked = window_->asking();
    const auto mode = ask::get_item(this, QStringLiteral("チャンネルを統合"),
                                    QStringLiteral("このページから続くグレーのページを、どの色モードの版にしますか（RGB・Lab は 3 ページ、CMYK は 4 ページ）"),
                                    {QStringLiteral("RGB"), QStringLiteral("CMYK"), QStringLiteral("Lab")});
    if (!mode || !window_->still(asked)) return;
    const std::string key = mode->toLower().toStdString();
    const std::size_t wanted = key == "cmyk" ? 4 : 3;
    const core::Document& book = window_->book();
    Json pages = Json::array();
    for (int i = window_->page_index(); i < static_cast<int>(book.pages.size()) && pages.size() < wanted; ++i) {
        pages.push_back(book.page(static_cast<std::size_t>(i)).index.json());
    }
    window_->apply_ops(Json::array({Json{{"op", "merge_channels"}, {"pages", pages}, {"mode", key}}}));
}

void ChannelPanel::merge_spot() {
    const core::Page* page = window_->current_page();
    if (page == nullptr) return;
    Json op{{"op", "merge_spot_channel"}, {"page", page->index.json()}};
    if (const std::string id = chosen_channel(); !id.empty()) op["channel"] = id;
    window_->apply_ops(Json::array({op}));
}

}  // namespace genko::app
