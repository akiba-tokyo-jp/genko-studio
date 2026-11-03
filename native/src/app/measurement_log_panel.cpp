// PC5 J-d2 (app/measurement_log.hpp): ウィンドウ → 計測ログ — the log's table and its buttons (PhotoCraft v0.2.0's
// crates/ui-egui/src/analysis_ui.rs, in Qt).

#include <QAbstractItemView>
#include <QAction>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <set>

#include "app/ask.hpp"
#include "app/main_window.hpp"
#include "app/measurement_log.hpp"
#include "app/theme.hpp"
#include "core/paths.hpp"
#include "storage/fsutil.hpp"

namespace genko::app {

using core::Json;

namespace {

constexpr int kRowId = Qt::UserRole + 21;  // (a row's id in the log)

QString cell_text(const Json& value) {
    if (value.is_string()) return QString::fromStdString(value.get<std::string>());
    if (value.is_array()) return QString::fromStdString(render::measure::csv_field(value));
    if (value.is_null()) return {};
    return QString::fromStdString(value.dump());
}

}  // namespace

MeasurementLogPanel::MeasurementLogPanel(MainWindow* window) : QWidget(window), window_(window) {
    setObjectName(QStringLiteral("measurement_log_panel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    table_ = new QTableWidget;
    table_->setObjectName(QStringLiteral("measurement_log_table"));
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->verticalHeader()->setVisible(false);
    table_->setToolTip(QStringLiteral("記録した計測値（このアプリを閉じるまで。原稿には入りません）"));
    layout->addWidget(table_, 1);
    status_ = new QLabel;
    status_->setObjectName(QStringLiteral("measurement_log_status"));
    status_->setWordWrap(true);
    theme::role(status_, "hint");
    layout->addWidget(status_);
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
    add(QStringLiteral("計測値を記録"), QStringLiteral("選択範囲・ものさし・カウントの計測値を記録します"), QStringLiteral("log_record"),
        [this] { window_->action(QStringLiteral("act_measure_record"))->trigger(); });
    row->addStretch(1);
    add(QStringLiteral("削除"), QStringLiteral("選んだ行を削除します"), QStringLiteral("log_delete"), [this] { remove_chosen(); });
    add(QStringLiteral("書き出し…"), QStringLiteral("選んだ行（選んでいなければすべて）を CSV に書き出します"), QStringLiteral("log_export"),
        [this] { export_csv(); });
    layout->addLayout(row);
    connect(&MeasurementLog::instance(), &MeasurementLog::changed, this, [this] { refresh(); });
    refresh();
}

QToolButton* MeasurementLogPanel::button(const QString& name) const { return findChild<QToolButton*>(name); }

void MeasurementLogPanel::refresh() {
    try {  // (the review's P1-1: a table the memory cannot hold said, never a crash)
        fill();
    } catch (const std::exception&) {
        table_->clear();
        table_->setRowCount(0);
        table_->setColumnCount(0);
        status_->setText(QStringLiteral("計測ログが大きすぎて表に出せません（行を削除するか、書き出し… で CSV にしてください）"));
    }
}

void MeasurementLogPanel::fill() {
    const MeasurementLog& log = MeasurementLog::instance();
    const std::vector<std::string> keys = log.columns();
    table_->clear();
    table_->setColumnCount(static_cast<int>(keys.size()));
    QStringList heads;
    for (const std::string& key : keys) heads << QString::fromStdString(render::measure::column_name(key));
    table_->setHorizontalHeaderLabels(heads);
    table_->setRowCount(static_cast<int>(log.rows().size()));
    for (std::size_t r = 0; r < log.rows().size(); ++r) {
        const MeasurementLog::Row& row = log.rows()[r];
        for (std::size_t c = 0; c < keys.size(); ++c) {
            auto* item = new QTableWidgetItem(row.values.contains(keys[c]) ? cell_text(row.values[keys[c]]) : QString());
            item->setData(kRowId, QVariant::fromValue<qulonglong>(row.id));
            table_->setItem(static_cast<int>(r), static_cast<int>(c), item);
        }
    }
    if (!keys.empty()) table_->resizeColumnsToContents();
    if (log.rows().empty()) {
        status_->setText(QStringLiteral("イメージ → 解析 → 計測値を記録 で、選択範囲・ものさし・カウントの計測値がここに並びます"));
    } else {
        QString text = QStringLiteral("%1 行").arg(log.rows().size());
        if (log.last_grid_dpi() > 0)
            text += QStringLiteral("。選択範囲は %1 dpi の格子で数え、ページの解像度の画素に直しています").arg(log.last_grid_dpi());
        status_->setText(text);
    }
}

std::vector<std::uint64_t> MeasurementLogPanel::chosen() const {
    std::set<std::uint64_t> ids;
    for (const QTableWidgetItem* item : table_->selectedItems()) ids.insert(item->data(kRowId).toULongLong());
    return {ids.begin(), ids.end()};
}

void MeasurementLogPanel::remove_chosen() {
    const std::vector<std::uint64_t> ids = chosen();
    if (ids.empty()) {
        window_->flash(QStringLiteral("削除する行を選びます"), 3000);
        return;
    }
    const std::size_t gone = MeasurementLog::instance().remove(ids);
    window_->flash(QStringLiteral("計測ログから %1 行を削除しました").arg(gone), 3000);
}

void MeasurementLogPanel::export_csv() {
    const MeasurementLog& log = MeasurementLog::instance();
    if (log.rows().empty()) {
        window_->flash(QStringLiteral("計測ログに行がありません"), 3000);
        return;
    }
    const std::vector<std::uint64_t> ids = chosen();
    const QString path = ask::save_path(this, QStringLiteral("計測値を書き出し"), QStringLiteral("計測ログ.csv"), QStringLiteral("CSV (*.csv)"));
    if (path.isEmpty()) return;
    // (the review's P2-4: never over this book, its folder, a linked file or a picture a book was opened from — said)
    if (window_->refuses_to_write(core::path_from_utf8(path.toStdString()))) return;
    // (UTF-8 with its BOM, as a spreadsheet opens a CSV of Japanese words)
    try {
        storage::write_atomic(core::path_from_utf8(path.toStdString()), "\xEF\xBB\xBF" + log.csv(ids));
    } catch (const std::exception& error) {
        window_->flash(QStringLiteral("書き出せませんでした: %1").arg(QString::fromUtf8(error.what())), 5000, true);
        return;
    }
    const std::size_t count = ids.empty() ? log.rows().size() : ids.size();
    window_->flash(QStringLiteral("計測ログの %1 行を書き出しました").arg(count), 4000);
}

}  // namespace genko::app
