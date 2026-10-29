#include "app/layer_comps_panel.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include "app/ask.hpp"
#include "app/look.hpp"
#include "app/main_window.hpp"
#include "formats/copy.hpp"
#include "render/ops_layer_comps.hpp"

namespace genko::app {

using core::Json;

namespace {

constexpr int kId = Qt::UserRole + 10;  // (a comp's id; 0: 最後のドキュメントの状態)

}  // namespace

LayerCompsPanel::LayerCompsPanel(MainWindow* window) : QWidget(window), window_(window) {
    setObjectName(QStringLiteral("layer_comps_panel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    list_ = new QListWidget;
    list_->setObjectName(QStringLiteral("layer_comps_list"));
    list_->setToolTip(QStringLiteral("ダブルクリックでそのレイヤーカンプを適用します（最後のドキュメントの状態: 元に戻します）"));
    layout->addWidget(list_, 1);
    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    const auto add = [&](const QString& text, const QString& tip, const QString& name, auto slot) {
        auto* b = new QToolButton;
        b->setText(text);
        b->setToolTip(tip);
        b->setObjectName(name);
        connect(b, &QToolButton::clicked, this, slot);
        row->addWidget(b);
        return b;
    };
    add(QStringLiteral("◀"), QStringLiteral("前のレイヤーカンプを適用"), QStringLiteral("comp_previous"), [this] { step(false); });
    add(QStringLiteral("▶"), QStringLiteral("次のレイヤーカンプを適用"), QStringLiteral("comp_next"), [this] { step(true); });
    row->addStretch(1);
    add(QStringLiteral("更新"), QStringLiteral("レイヤーカンプを更新: 選んだカンプに、今のレイヤーの表示・位置・アピアランスを記録し直します"),
        QStringLiteral("comp_update"), [this] { update_chosen(); });
    add(QStringLiteral("新規"), QStringLiteral("新規レイヤーカンプを作成: 今のレイヤーの表示・位置・アピアランスを記録します"), QStringLiteral("comp_new"),
        [this] { add_new(); });
    add(QStringLiteral("削除"), QStringLiteral("レイヤーカンプを削除"), QStringLiteral("comp_delete"), [this] { remove_chosen(); });
    // the panel's menu (Photoshop's レイヤーカンプ パネルのメニュー)
    menu_ = new QMenu(this);
    menu_->setObjectName(QStringLiteral("layer_comps_menu"));
    const auto item = [this](const QString& text, auto slot) {
        QAction* a = menu_->addAction(text);
        connect(a, &QAction::triggered, this, slot);
        return a;
    };
    item(QStringLiteral("新規レイヤーカンプ…"), [this] { window_->layer_comp_options(); });
    item(QStringLiteral("レイヤーカンプを複製"), [this] { duplicate_chosen(); });
    item(QStringLiteral("レイヤーカンプを削除"), [this] { remove_chosen(); });
    item(QStringLiteral("レイヤーカンプを更新"), [this] { update_chosen(); });
    menu_->addSeparator();
    item(QStringLiteral("レイヤーカンプを適用"), [this] { apply_chosen(); });
    item(QStringLiteral("次のレイヤーカンプを適用"), [this] { step(true); });
    item(QStringLiteral("前のレイヤーカンプを適用"), [this] { step(false); });
    item(QStringLiteral("最後のドキュメントの状態を復元"), [this] { restore_last(); });
    menu_->addSeparator();
    item(QStringLiteral("レイヤーカンプオプション…"), [this] { options_of_chosen(); });
    item(QStringLiteral("レイヤーカンプの警告をクリア"), [this] { clear_warnings(); });
    menu_->addSeparator();
    item(QStringLiteral("レイヤーカンプからファイル…"), [this] { window_->layer_comps_to_files(); });
    auto* more = new QToolButton;
    more->setText(QStringLiteral("≡"));
    more->setToolTip(QStringLiteral("レイヤーカンプ パネルのメニュー"));
    more->setObjectName(QStringLiteral("comp_menu"));
    more->setMenu(menu_);
    more->setPopupMode(QToolButton::InstantPopup);
    row->addWidget(more);
    layout->addLayout(row);
    connect(list_, &QListWidget::itemDoubleClicked, this, &LayerCompsPanel::double_clicked);
}

QToolButton* LayerCompsPanel::button(const QString& name) const { return findChild<QToolButton*>(name); }

void LayerCompsPanel::refresh() {
    loading_ = true;
    const std::optional<std::int64_t> was = list_->currentItem() != nullptr ? std::optional<std::int64_t>(list_->currentItem()->data(kId).toLongLong()) : std::nullopt;
    list_->clear();
    const core::Page* page = window_->current_page();
    const core::LayerComps none;
    const core::LayerComps& comps = page != nullptr && page->layer_comps ? *page->layer_comps : none;
    const bool own_state = !comps.last_applied;
    auto* last = new QListWidgetItem((own_state ? QStringLiteral("✓ ") : QStringLiteral("　 ")) + QStringLiteral("最後のドキュメントの状態"));
    last->setData(kId, QVariant::fromValue<qlonglong>(0));
    last->setToolTip(comps.last_state ? QStringLiteral("レイヤーカンプを適用する前の状態です。ダブルクリックで元に戻します")
                                      : QStringLiteral("レイヤーカンプを適用すると、その前の状態をここに残します"));
    list_->addItem(last);
    for (const core::LayerComp& c : comps.comps) {
        const std::size_t gone = page != nullptr ? render::layer_comps::missing(*page, c.layers).size() : 0;
        QString text = (comps.last_applied == c.id ? QStringLiteral("✓ ") : QStringLiteral("　 ")) + QString::fromStdString(c.name);
        if (gone > 0) text += QStringLiteral("  ⚠");
        auto* row = new QListWidgetItem(text);
        row->setData(kId, QVariant::fromValue<qlonglong>(c.id));
        QStringList what;
        if (c.apply_visibility) what << QStringLiteral("表示/非表示");
        if (c.apply_position) what << QStringLiteral("位置");
        if (c.apply_appearance) what << QStringLiteral("アピアランス");
        QString tip = QStringLiteral("適用するもの: %1").arg(what.isEmpty() ? QStringLiteral("なし") : what.join(QStringLiteral("・")));
        if (!c.comment.empty()) tip = QString::fromStdString(c.comment) + QStringLiteral("\n") + tip;
        if (gone > 0) tip += QStringLiteral("\n⚠ 記録したレイヤーのうち %1 枚がもうありません（更新するか、警告をクリアします）").arg(gone);
        row->setToolTip(tip);
        list_->addItem(row);
        if (was && *was == c.id) list_->setCurrentItem(row);
    }
    const bool any = !comps.comps.empty();
    for (const char* name : {"comp_previous", "comp_next", "comp_update", "comp_delete"}) {
        if (QToolButton* b = button(QString::fromLatin1(name))) b->setEnabled(any);
    }
    if (QToolButton* b = button(QStringLiteral("comp_new"))) b->setEnabled(page != nullptr);
    loading_ = false;
}

std::optional<std::int64_t> LayerCompsPanel::chosen() const {
    const core::Page* page = window_->current_page();
    if (page == nullptr || !page->layer_comps || page->layer_comps->comps.empty()) return std::nullopt;
    if (const QListWidgetItem* item = list_->currentItem()) {
        const std::int64_t id = item->data(kId).toLongLong();
        for (const core::LayerComp& c : page->layer_comps->comps)
            if (c.id == id) return id;
    }
    if (page->layer_comps->last_applied) return page->layer_comps->last_applied;
    return std::nullopt;
}

void LayerCompsPanel::double_clicked(QListWidgetItem* item) {
    if (item == nullptr) return;
    const std::int64_t id = item->data(kId).toLongLong();
    if (id == 0) {
        restore_last();
        return;
    }
    window_->layer_comp_op(Json{{"op", "apply_layer_comp"}, {"comp", id}});
}

void LayerCompsPanel::apply_chosen() {
    if (const auto id = chosen()) window_->layer_comp_op(Json{{"op", "apply_layer_comp"}, {"comp", *id}});
}

void LayerCompsPanel::step(bool next) { window_->layer_comp_op(Json{{"op", "apply_layer_comp"}, {"comp", next ? "next" : "previous"}}); }

void LayerCompsPanel::update_chosen() {
    if (const auto id = chosen()) window_->layer_comp_op(Json{{"op", "update_layer_comp"}, {"comp", *id}});
}

void LayerCompsPanel::add_new() { window_->layer_comp_op(Json{{"op", "new_layer_comp"}}); }

void LayerCompsPanel::remove_chosen() {
    if (const auto id = chosen()) window_->layer_comp_op(Json{{"op", "delete_layer_comp"}, {"comp", *id}});
}

void LayerCompsPanel::duplicate_chosen() {
    if (const auto id = chosen()) window_->layer_comp_op(Json{{"op", "duplicate_layer_comp"}, {"comp", *id}});
}

void LayerCompsPanel::options_of_chosen() {
    if (const auto id = chosen()) window_->layer_comp_options(*id);
}

void LayerCompsPanel::clear_warnings() { window_->layer_comp_op(Json{{"op", "clear_layer_comp_warnings"}}); }

void LayerCompsPanel::restore_last() { window_->layer_comp_op(Json{{"op", "apply_layer_comp"}, {"comp", "last_state"}}); }

// --- the windows ------------------------------------------------------------------------------------------------------

LayerCompOptionsDialog::LayerCompOptionsDialog(QWidget* parent, const QString& title, const QString& name_now, bool visible, bool place,
                                               bool look, const QString& comment_now)
    : QDialog(parent) {
    setObjectName(QStringLiteral("layerCompOptionsDialog"));
    setWindowTitle(title);
    name = new QLineEdit(name_now);
    name->setObjectName(QStringLiteral("comp_name"));
    name->setMaxLength(255);
    visibility = new QCheckBox(QStringLiteral("表示/非表示"));
    visibility->setObjectName(QStringLiteral("comp_visibility"));
    visibility->setChecked(visible);
    position = new QCheckBox(QStringLiteral("位置"));
    position->setObjectName(QStringLiteral("comp_position"));
    position->setChecked(place);
    appearance = new QCheckBox(QStringLiteral("アピアランス（レイヤースタイル）"));
    appearance->setObjectName(QStringLiteral("comp_appearance"));
    appearance->setChecked(look);
    comment = new QPlainTextEdit(comment_now);
    comment->setObjectName(QStringLiteral("comp_comment"));
    comment->setFixedHeight(72);
    buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("キャンセル"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(name, &QLineEdit::textChanged, this, [this](const QString& text) { buttons->button(QDialogButtonBox::Ok)->setEnabled(!text.trimmed().isEmpty()); });
    buttons->button(QDialogButtonBox::Ok)->setEnabled(!name_now.trimmed().isEmpty());
    auto* applies = new QVBoxLayout;
    applies->setContentsMargins(0, 0, 0, 0);
    for (QCheckBox* box : {visibility, position, appearance}) applies->addWidget(box);
    auto* applies_box = new QWidget;
    applies_box->setLayout(applies);
    QFormLayout* rows = look::form();
    rows->addRow(QStringLiteral("名前"), name);
    rows->addRow(QStringLiteral("適用先"), applies_box);
    rows->addRow(QStringLiteral("コメント"), comment);
    look::quiet_labels(rows);
    look::frame(this, look::header(title, QStringLiteral("レイヤーの表示・位置・アピアランスのうち、適用するものを選びます")), rows, nullptr, look::footer(buttons));
}

LayerCompsToFilesDialog::LayerCompsToFilesDialog(QWidget* parent, const QString& where, const QString& name, bool one_chosen) : QDialog(parent) {
    setObjectName(QStringLiteral("layerCompsToFilesDialog"));
    setWindowTitle(QStringLiteral("レイヤーカンプからファイル"));
    folder = new QLineEdit(QDir::toNativeSeparators(where));
    folder->setObjectName(QStringLiteral("comps_folder"));
    auto* pick = new QPushButton(QStringLiteral("選ぶ…"));
    connect(pick, &QPushButton::clicked, this, [this] {
        const QString got = ask::existing_dir(this, QStringLiteral("書き出し先"), folder->text());
        if (!got.isEmpty()) folder->setText(QDir::toNativeSeparators(got));
    });
    auto* line = new QHBoxLayout;
    line->setContentsMargins(0, 0, 0, 0);
    line->addWidget(folder, 1);
    line->addWidget(pick);
    auto* where_row = new QWidget;
    where_row->setLayout(line);
    prefix = new QLineEdit(name);
    prefix->setObjectName(QStringLiteral("comps_prefix"));
    chosen_only = new QCheckBox(QStringLiteral("選択したレイヤーカンプのみ"));
    chosen_only->setObjectName(QStringLiteral("comps_chosen_only"));
    chosen_only->setEnabled(one_chosen);
    format = new QComboBox;
    format->setObjectName(QStringLiteral("comps_format"));
    for (const std::string_view ext : formats::copy_extensions()) format->addItem(QString::fromLatin1(ext.data(), static_cast<qsizetype>(ext.size())).toUpper(), QString::fromLatin1(ext.data(), static_cast<qsizetype>(ext.size())));
    format->setCurrentIndex(std::max(0, format->findData(QStringLiteral("png"))));
    quality = new QSpinBox;
    quality->setObjectName(QStringLiteral("comps_quality"));
    quality->setRange(0, 12);
    quality->setValue(10);
    quality->setToolTip(QStringLiteral("JPEG の画質（0〜12。12 が最高）"));
    const auto jpeg = [this] {
        const QString f = format->currentData().toString();
        return f == QLatin1String("jpg") || f == QLatin1String("jpeg");
    };
    connect(format, &QComboBox::currentIndexChanged, this, [this, jpeg] { quality->setEnabled(jpeg()); });
    quality->setEnabled(jpeg());
    buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("書き出す"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("やめる"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    QFormLayout* rows = look::form();
    rows->addRow(QStringLiteral("書き出し先"), where_row);
    rows->addRow(QStringLiteral("ファイル名の接頭辞"), prefix);
    rows->addRow(QString(), chosen_only);
    rows->addRow(QStringLiteral("ファイル形式"), format);
    rows->addRow(QStringLiteral("JPEG の画質"), quality);
    look::quiet_labels(rows);
    look::frame(this, look::header(QStringLiteral("レイヤーカンプからファイル"), QStringLiteral("このページのレイヤーカンプを、1 つずつ適用したページの画像にして書き出します")),
                rows, nullptr, look::footer(buttons));
}

}  // namespace genko::app
