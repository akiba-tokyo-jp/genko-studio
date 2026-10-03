#include "app/pen.hpp"

#include <QSettings>
#include <QStringList>
#include <QTabletEvent>

#include <cmath>

#include "app/config.hpp"
#include "core/pynum.hpp"
#include "core/stroke_geom.hpp"
#include "render/brushes.hpp"

namespace genko::app {

PenSample tablet_sample(double x_mm, double y_mm, const QTabletEvent& event) {
    const double tilt = std::abs(event.xTilt()) / 60.0;
    const core::PenPoint packed = core::pack_point(x_mm, y_mm, event.pressure(), tilt);
    PenSample sample;
    sample.x_mm = packed.x;
    sample.y_mm = packed.y;
    sample.pressure = packed.p.value_or(0.7);
    sample.rotation = event.rotation();
    return sample;
}

PenSample mouse_sample(double x_mm, double y_mm) {
    const core::PenPoint packed = core::pack_point(x_mm, y_mm, std::nullopt);
    PenSample sample;
    sample.x_mm = packed.x;
    sample.y_mm = packed.y;
    sample.pressure = packed.p.value_or(0.7);
    return sample;
}

PenSettings PenSettings::for_kind(const std::string& kind) {
    const core::Brush b = render::brushes::brush(kind);
    PenSettings pen;
    pen.kind = kind;
    pen.width_mm = b.width_mm;
    pen.opacity = static_cast<double>(static_cast<int>(b.opacity * 100)) / 100.0;  // (the slider holds whole percents)
    pen.stabilize = b.stabilize;
    pen.taper = b.taper;
    return pen;
}

PenSettings PenSettings::load() {
    const auto store = settings();
    const std::string kind = store->value(QStringLiteral("brush/kind"), QString::fromLatin1(core::kDefaultBrush.data())).toString().toStdString();
    PenSettings pen = for_kind(kind);
    const QString prefix = QStringLiteral("brush/%1/").arg(QString::fromStdString(kind));
    pen.width_mm = store->value(prefix + QStringLiteral("size"), pen.width_mm).toDouble();
    pen.opacity = static_cast<int>(store->value(prefix + QStringLiteral("opacity"), pen.opacity).toDouble() * 100) / 100.0;
    pen.stabilize = store->value(prefix + QStringLiteral("steady"), static_cast<qlonglong>(pen.stabilize)).toLongLong();
    pen.taper = store->value(prefix + QStringLiteral("taper"), pen.taper).toString().toLower() == QLatin1String("true");
    pen.pressure_gamma = store->value(prefix + QStringLiteral("pressure"), 1.0).toDouble();
    pen.taper_in_mm = store->value(prefix + QStringLiteral("taper_in"), -1.0).toDouble();
    pen.taper_out_mm = store->value(prefix + QStringLiteral("taper_out"), -1.0).toDouble();
    pen.ink_pressure = store->value(prefix + QStringLiteral("ink_pressure"), 0).toDouble();
    pen.speed_steady = store->value(prefix + QStringLiteral("speed_steady"), false).toString().toLower() == QLatin1String("true");
    pen.post_fit = store->value(prefix + QStringLiteral("post_fit"), 0.0).toDouble();
    pen.snap_lines = store->value(prefix + QStringLiteral("snap_lines"), 0.0).toDouble();
    const QStringList rgb = store->value(QStringLiteral("brush/rgb"), QStringLiteral("20,20,20")).toString().split(QLatin1Char(','));
    if (rgb.size() >= 3) {
        bool ok = true;
        std::vector<std::int64_t> values;
        for (int i = 0; i < 3 && ok; ++i) values.push_back(rgb[i].trimmed().toLongLong(&ok));
        if (ok) pen.rgb = values;
    }
    return pen;
}

void PenSettings::save() const {
    const auto store = settings();
    const QString prefix = QStringLiteral("brush/%1/").arg(QString::fromStdString(kind));
    store->setValue(QStringLiteral("brush/kind"), QString::fromStdString(kind));
    store->setValue(prefix + QStringLiteral("size"), width_mm);
    store->setValue(prefix + QStringLiteral("opacity"), opacity);
    store->setValue(prefix + QStringLiteral("steady"), static_cast<qlonglong>(stabilize));
    store->setValue(prefix + QStringLiteral("taper"), taper ? QStringLiteral("true") : QStringLiteral("false"));
    store->setValue(prefix + QStringLiteral("pressure"), pressure_gamma);
    store->setValue(QStringLiteral("brush/rgb"), QStringLiteral("%1,%2,%3").arg(rgb[0]).arg(rgb[1]).arg(rgb[2]));
}

core::Json PenSettings::stroke_fields() const {
    const core::Brush b = render::brushes::brush(kind);
    core::Json out = core::Json::object();
    out["kind"] = kind;
    out["width_mm"] = core::py_round(width_mm, 3);
    out["stabilize"] = stabilize;
    out["taper"] = taper;
    if (!b.rgb) out["rgb"] = core::Json(rgb);
    if (std::abs(opacity - b.opacity) > 1e-6) out["opacity"] = core::py_round(opacity / std::max(b.opacity, 0.01), 3);
    if (pressure_gamma != 1.0) out["pressure_gamma"] = pressure_gamma;
    if (taper && (taper_in_mm >= 0 || taper_out_mm >= 0)) {
        if (taper_in_mm >= 0) out["taper_in_mm"] = core::py_round(taper_in_mm, 2);
        if (taper_out_mm >= 0) out["taper_out_mm"] = core::py_round(taper_out_mm, 2);
    }
    if (ink_pressure != 0) out["pressure_opacity"] = core::py_round(ink_pressure / 100.0, 2);
    if (speed_steady && stabilize != 0) out["stabilize_speed"] = true;
    if (post_fit > 0) out["post_fit"] = core::py_round(post_fit, 2);
    if (snap_lines > 0) out["snap_lines_mm"] = core::py_round(snap_lines, 2);
    return out;
}

}  // namespace genko::app
