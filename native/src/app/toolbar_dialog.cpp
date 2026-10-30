// 編集 → ツールバー… (PC5 H-d; PhotoCraft v0.2.0's prefs.rs toolbar, MIT).

#include "app/toolbar_dialog.hpp"

#include <QAction>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

#include "app/config.hpp"
#include "app/documents.hpp"
#include "app/look.hpp"
#include "app/main_window.hpp"
#include "app/theme.hpp"

namespace genko::app {

namespace toolbar {

namespace {

const QString kOrder = QStringLiteral("toolbar/order");
const QString kHidden = QStringLiteral("toolbar/hidden");

QStringList split(const QString& text) { return text.split(QLatin1Char('\t'), Qt::SkipEmptyParts); }

}  // namespace

const QStringList& default_order() {
    // (MainWindow::build_toolbars' palette: Python's — select, move, pen, eraser, blend, shape, fill, lassofill, gradient,
    // picker | text, frame | the selection | the rulers, 3D, effect lines — and 手のひらツール last, PC5 H-a)
    static const QStringList all{QStringLiteral("act_select"),  QStringLiteral("act_move"),     QStringLiteral("act_pen"),     QStringLiteral("act_eraser"),
                                 QStringLiteral("act_blend"),   QStringLiteral("act_shape"),    QStringLiteral("act_fill"),    QStringLiteral("act_lassofill"),
                                 QStringLiteral("act_gradient"), QStringLiteral("act_picker"),  QStringLiteral("|"),           QStringLiteral("act_text"),
                                 QStringLiteral("act_frame"),   QStringLiteral("|"),            QStringLiteral("act_marquee"), QStringLiteral("act_lasso"),
                                 QStringLiteral("act_wand"),    QStringLiteral("act_reshape"),
                                 // (PC5 J-b: スライスツール・スライス選択ツール — Photoshop's 切り抜き group, its menu on each)
                                 QStringLiteral("act_slice_tool"), QStringLiteral("act_slice_select_tool"), QStringLiteral("|"),           QStringLiteral("act_ruler"),
                                 QStringLiteral("act_3d"),      QStringLiteral("act_effect"),   QStringLiteral("act_hand_tool")};
    return all;
}

QStringList hidden() { return split(settings()->value(kHidden).toString()); }

QStringList order() {
    QStringList out = split(settings()->value(kOrder).toString());
    if (out.isEmpty()) return default_order();
    const QStringList away = hidden();
    for (const QString& tool : default_order()) {  // (a tool made since the order was kept)
        if (tool != QLatin1String("|") && !out.contains(tool) && !away.contains(tool)) out << tool;
    }
    return out;
}

void keep(const QStringList& order, const QStringList& hidden) {
    auto store = settings();
    if (order == default_order() && hidden.isEmpty()) {  // (as Genko makes it: nothing kept)
        store->remove(kOrder);
        store->remove(kHidden);
        return;
    }
    store->setValue(kOrder, order.join(QLatin1Char('\t')));
    if (hidden.isEmpty()) {
        store->remove(kHidden);
    } else {
        store->setValue(kHidden, hidden.join(QLatin1Char('\t')));
    }
}

}  // namespace toolbar

namespace {

const QString kLineWords = QStringLiteral("――（区切り）");

}  // namespace

ToolbarDialog::ToolbarDialog(MainWindow* window) : QDialog(window), window_(window) {
    setWindowTitle(QStringLiteral("ツールバーをカスタマイズ"));
    setObjectName(QStringLiteral("toolbarDialog"));
    shown = new QListWidget;
    shown->setObjectName(QStringLiteral("toolbar_shown"));
    extra = new QListWidget;
    extra->setObjectName(QStringLiteral("toolbar_extra"));
    for (QListWidget* list : {shown, extra}) {
        list->setIconSize(QSize(20, 20));
        list->setMinimumWidth(220);
    }
    up = new QPushButton(QStringLiteral("上へ"));
    down = new QPushButton(QStringLiteral("下へ"));
    to_extra = new QPushButton(QStringLiteral("余分なツールへ →"));
    to_bar = new QPushButton(QStringLiteral("← ツールバーへ"));
    line = new QPushButton(QStringLiteral("区切りを足す"));
    restore = new QPushButton(QStringLiteral("初期設定に戻す"));
    connect(up, &QPushButton::clicked, this, [this] { move(-1); });
    connect(down, &QPushButton::clicked, this, [this] { move(1); });
    connect(to_extra, &QPushButton::clicked, this, [this] { hide_tool(); });
    connect(to_bar, &QPushButton::clicked, this, [this] { show_tool(); });
    connect(line, &QPushButton::clicked, this, &ToolbarDialog::add_line);
    connect(restore, &QPushButton::clicked, this, &ToolbarDialog::restore_defaults);
    auto* grid = new QGridLayout;
    grid->addWidget(new QLabel(QStringLiteral("ツールバー")), 0, 0);
    grid->addWidget(new QLabel(QStringLiteral("余分なツール")), 0, 2);
    grid->addWidget(shown, 1, 0);
    auto* middle = new QVBoxLayout;
    for (QPushButton* button : {up, down, to_extra, to_bar, line}) middle->addWidget(button);
    middle->addStretch(1);
    middle->addWidget(restore);
    grid->addLayout(middle, 1, 1);
    grid->addWidget(extra, 1, 2);
    auto* hint = new QLabel(QStringLiteral("ツールの並びを変え、使わないツールを「余分なツール」へ移します。余分なツールは、ツールバーの最後の「…」から選べます。"
                                           "グループのツール（長押しで出るもの）は、そのボタンについて移ります。"));
    hint->setWordWrap(true);
    theme::role(hint, "hint");
    grid->addWidget(hint, 2, 0, 1, 3);
    fill(toolbar::order(), toolbar::hidden());
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("決める"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("やめる"));
    connect(buttons, &QDialogButtonBox::accepted, this, &ToolbarDialog::save);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    look::frame(this, look::header(QStringLiteral("ツールバー"), QStringLiteral("このパソコンでのツールの並びです。原稿には残りません。")), grid, nullptr,
                look::footer(buttons));
    resize(620, 560);
}

void ToolbarDialog::fill(const QStringList& order, const QStringList& away) {
    shown->clear();
    extra->clear();
    const auto add = [this](QListWidget* list, const QString& entry) {
        if (entry == QLatin1String("|")) {
            auto* item = new QListWidgetItem(kLineWords, list);
            item->setData(Qt::UserRole, entry);
            return;
        }
        QAction* tool = window_->action(entry);
        if (tool == nullptr) return;  // (a tool this build does not have)
        auto* item = new QListWidgetItem(tool->icon(), QString(tool->text()).remove(QLatin1Char('&')), list);
        item->setData(Qt::UserRole, entry);
    };
    for (const QString& entry : order) {
        if (!away.contains(entry)) add(shown, entry);
    }
    for (const QString& entry : away) add(extra, entry);
}

QStringList ToolbarDialog::order() const {
    QStringList out;
    for (int i = 0; i < shown->count(); ++i) out << shown->item(i)->data(Qt::UserRole).toString();
    return out;
}

QStringList ToolbarDialog::hidden() const {
    QStringList out;
    for (int i = 0; i < extra->count(); ++i) out << extra->item(i)->data(Qt::UserRole).toString();
    return out;
}

void ToolbarDialog::move(int delta) {
    const int row = shown->currentRow();
    const int to = row + delta;
    if (row < 0 || to < 0 || to >= shown->count()) return;
    QListWidgetItem* item = shown->takeItem(row);
    shown->insertItem(to, item);
    shown->setCurrentRow(to);
}

void ToolbarDialog::hide_tool(const QString& attribute) {
    int row = shown->currentRow();
    if (!attribute.isEmpty()) {
        row = -1;
        for (int i = 0; i < shown->count(); ++i) {
            if (shown->item(i)->data(Qt::UserRole).toString() == attribute) row = i;
        }
    }
    if (row < 0) return;
    QListWidgetItem* item = shown->takeItem(row);
    if (item->data(Qt::UserRole).toString() == QLatin1String("|")) {  // (a line goes, not to the extra tools)
        delete item;
        return;
    }
    extra->addItem(item);
}

void ToolbarDialog::show_tool(const QString& attribute) {
    int row = extra->currentRow();
    if (!attribute.isEmpty()) {
        row = -1;
        for (int i = 0; i < extra->count(); ++i) {
            if (extra->item(i)->data(Qt::UserRole).toString() == attribute) row = i;
        }
    }
    if (row < 0) return;
    shown->addItem(extra->takeItem(row));
    shown->setCurrentRow(shown->count() - 1);
}

void ToolbarDialog::add_line() {
    auto* item = new QListWidgetItem(kLineWords);
    item->setData(Qt::UserRole, QStringLiteral("|"));
    shown->insertItem(shown->currentRow() < 0 ? shown->count() : shown->currentRow() + 1, item);
}

void ToolbarDialog::restore_defaults() { fill(toolbar::default_order(), {}); }

void ToolbarDialog::save() {
    toolbar::keep(order(), hidden());
    for (MainWindow* window : documents::windows()) window->fill_palette();
    accept();
}

}  // namespace genko::app
