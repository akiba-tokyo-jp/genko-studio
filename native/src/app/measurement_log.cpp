// PC5 J-d2 (app/measurement_log.hpp): the app's measurement log — PhotoCraft v0.2.0's AnalysisState, record's rows kept to
// the data points, measurementLog.delete and .export (crates/engine/src/analysis_cmds.rs, MIT:
// native/third_party/photocraft/NOTICE.md).

#include "app/measurement_log.hpp"

#include <QSettings>
#include <QString>

#include <algorithm>

#include "app/config.hpp"

namespace genko::app {

using core::Json;

MeasurementLog& MeasurementLog::instance() {
    static MeasurementLog* log = new MeasurementLog;  // (the app's, for its session: never written anywhere)
    return *log;
}

std::vector<std::uint64_t> MeasurementLog::add(const render::measure::Recorded& made) {
    const render::measure::DataPoints points = data_points();
    std::vector<std::uint64_t> ids;
    for (const Json& row : made.rows) {
        rows_.push_back(Row{++next_row_, render::measure::kept(row, points.of(made.source))});
        ids.push_back(next_row_);
    }
    if (made.grid_dpi > 0) last_grid_dpi_ = made.grid_dpi;
    emit changed();
    return ids;
}

std::size_t MeasurementLog::remove(const std::vector<std::uint64_t>& ids) {
    const std::size_t before = rows_.size();
    if (ids.empty()) {
        rows_.clear();
    } else {
        std::erase_if(rows_, [&](const Row& r) { return std::find(ids.begin(), ids.end(), r.id) != ids.end(); });
    }
    if (rows_.size() != before) emit changed();
    return before - rows_.size();
}

void MeasurementLog::clear() { (void)remove({}); }

std::vector<std::string> MeasurementLog::columns() const {
    std::vector<Json> values;
    values.reserve(rows_.size());
    for (const Row& r : rows_) values.push_back(r.values);
    return render::measure::log_columns(values);
}

std::string MeasurementLog::csv(const std::vector<std::uint64_t>& ids) const {
    std::vector<Json> values;
    for (const Row& r : rows_)
        if (ids.empty() || std::find(ids.begin(), ids.end(), r.id) != ids.end()) values.push_back(r.values);
    return render::measure::csv(values);
}

render::measure::DataPoints MeasurementLog::data_points() {
    const QString text = settings()->value(QStringLiteral("measure/points")).toString();
    if (text.isEmpty()) return {};
    try {
        return render::measure::data_points_from_json(Json::parse(text.toStdString()));
    } catch (const Json::exception&) {  // (a setting that is not JSON: every point, as at first)
        return {};
    }
}

void MeasurementLog::set_data_points(const render::measure::DataPoints& points) {
    settings()->setValue(QStringLiteral("measure/points"), QString::fromStdString(render::measure::to_json(points).dump()));
}

}  // namespace genko::app
