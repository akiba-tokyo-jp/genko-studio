#pragma once

#include <QObject>
#include <QWidget>

#include <cstdint>
#include <string>
#include <vector>

#include "core/json.hpp"
#include "render/measure_features.hpp"

class QLabel;
class QTableWidget;
class QToolButton;

// PC5 J-d2 (SPEC.md PROD-01「計測」): Photoshop's 計測ログ — the rows イメージ → 解析 → 計測値を記録 adds, for the app's session
// only (PhotoCraft's AnalysisState: not in the book, not saved; every window's records in the one log, as Photoshop's for
// every document), each row kept to データポイントを選択's points of its source (this machine's setting measure/points), and
// the ウィンドウ → 計測ログ panel: the table (Photoshop's columns: those any row has), 計測値を記録, 選んだ行を削除 and
// 書き出し… (the CSV of the rows chosen, or of all).

namespace genko::app {

class MainWindow;

class MeasurementLog : public QObject {
    Q_OBJECT
public:
    struct Row {
        std::uint64_t id = 0;
        core::Json values;  // {key: value} (render/measure_features' columns)
    };
    // The app's log (one for every window).
    static MeasurementLog& instance();

    const std::vector<Row>& rows() const { return rows_; }
    // The rows recorded, each kept to the data points of their source, the summary labelled with the log's next number
    // (計測 n: counted through the app's session, a refused record taking none); their ids. Nothing put in when memory
    // fails (std::bad_alloc thrown).
    std::vector<std::uint64_t> add(const render::measure::Recorded& made);
    // 選んだ行を削除 (the ids given; none: all of them). How many went.
    std::size_t remove(const std::vector<std::uint64_t>& ids);
    void clear();
    // The columns any row has (Photoshop's order) and the CSV of the rows given (none: all).
    std::vector<std::string> columns() const;
    std::string csv(const std::vector<std::uint64_t>& ids = {}) const;
    // The grid the last selection was counted on (0: none yet).
    int last_grid_dpi() const { return last_grid_dpi_; }

    // データポイントを選択 (this machine's setting measure/points).
    static render::measure::DataPoints data_points();
    static void set_data_points(const render::measure::DataPoints& points);

signals:
    void changed();

private:
    std::vector<Row> rows_;
    std::uint64_t next_row_ = 0;
    std::size_t measurements_ = 0;
    int last_grid_dpi_ = 0;
};

class MeasurementLogPanel : public QWidget {
    Q_OBJECT
public:
    explicit MeasurementLogPanel(MainWindow* window);
    // The log again (it changed).
    void refresh();
    // The ids of the rows chosen in the table.
    std::vector<std::uint64_t> chosen() const;
    // 選んだ行を削除 (none chosen: nothing), 書き出し… (the rows chosen, else all; asked where).
    void remove_chosen();
    void export_csv();
    // Tests: the table, a button (log_record, log_delete, log_export), the line under it.
    QTableWidget* table() const { return table_; }
    QToolButton* button(const QString& name) const;
    QLabel* status() const { return status_; }

private:
    void fill();

    MainWindow* window_;
    QTableWidget* table_ = nullptr;
    QLabel* status_ = nullptr;
};

}  // namespace genko::app
