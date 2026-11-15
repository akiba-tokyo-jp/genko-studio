#pragma once

// Contact sheets of the pose library (SPEC FIGURE-02: the pictures a person reviews, not a test): for each category, its
// poses from the front, three quarters and the side, each with its id and Japanese name, the floor's line (and a chair's
// seat), a dot at each contact (green on what it rests on, red off it) and a red frame when a check fails. Written by
// test_figure_poses when GENKO_POSE_SHEETS names a folder (with checks.json: what each pose measured).

#include <QColor>
#include <QFont>
#include <QImage>
#include <QPainter>
#include <QString>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "core/json.hpp"
#include "core/mesh3d.hpp"
#include "core/pose_checks.hpp"
#include "render/figure_thumbs.hpp"

namespace genko::test::pose_sheets {

using core::Json;
namespace mesh3d = core::mesh3d;
namespace checks = core::pose_checks;

inline constexpr int kCellW = 176;
inline constexpr int kCellH = 250;
inline constexpr int kLabel = 38;
inline constexpr int kColumns = 3;  // (poses in a row)
inline constexpr int kRows = 4;     // (rows on a sheet)
inline constexpr double kViews[] = {0.0, 0.785, 1.571};

struct Entry {
    Json pose;
    checks::Report report;
};

// One pose's three views and its label, at (x, y) of the painter.
inline void draw_pose(QPainter& painter, int x, int y, const Entry& e) {
    const Json prim = checks::figure_of(e.pose);
    const double u = e.report.measured.unit;
    for (int v = 0; v < 3; ++v) {
        const double turn = kViews[v];
        render::figure_thumbs::Frame f;
        const std::string grey = render::figure_thumbs::grey(prim, turn, kCellW, kCellH, 2, &f);
        const double k = f.px_per_mm;
        QImage cell(kCellW, kCellH, QImage::Format_RGB888);
        for (int py = 0; py < kCellH; ++py) {
            for (int px = 0; px < kCellW; ++px) {
                const int g = static_cast<unsigned char>(grey[static_cast<std::size_t>(py) * kCellW + px]);
                cell.setPixelColor(px, py, g == 255 ? QColor(255, 255, 255) : QColor(g, g, std::min(255, g + 18)));
            }
        }
        const int ox = x + v * kCellW;
        const int oy = y + kLabel;
        painter.drawImage(ox, oy, cell);
        Json seen = prim;
        seen["rot"] = Json::array({0.0, turn, 0.0});
        const double floor = f.floor;
        const double box_y = f.top;
        const auto to_px = [&](const core::la::Vec3& p) {
            const mesh3d::OnPage at = mesh3d::to_page(seen, std::vector<core::la::Vec3>{p}, nullptr);
            return QPointF(ox + (at.pts[0][0] - f.left) * k, oy + (at.pts[0][1] - box_y) * k);
        };
        painter.setPen(QPen(QColor(150, 120, 90), 1));
        const double fy = oy + (floor - box_y) * k;
        painter.drawLine(QPointF(ox + 4, fy), QPointF(ox + kCellW - 4, fy));
        if (e.pose.value("support", std::string()) == "chair") {  // (the seat: a short line under the pelvis)
            const double sy = fy - e.report.measured.seat * u * k;
            painter.setPen(QPen(QColor(60, 90, 200), 2));
            const QPointF c = to_px(core::la::Vec3{0.0, 0.0, 0.0});
            painter.drawLine(QPointF(c.x() - 0.9 * u * k, sy), QPointF(c.x() + 0.9 * u * k, sy));
        }
        for (const Json& c : e.pose.value("contact", Json::array())) {
            std::string name = c.get<std::string>();
            if (name == "back" || name == "front" || name == "side") {  // (lying on it: the ribcage's point; a lean on a wall: none)
                if (e.report.measured.gap.at("torso") > checks::kContactGap) continue;
                name = "torso";
            }
            const auto at = e.report.measured.lowest.find(name);
            if (at == e.report.measured.lowest.end()) continue;
            const double gap = e.report.measured.gap.at(name);
            const bool seat = e.pose.value("support", std::string()) == "chair" && name == "seat";
            const bool good = seat || gap <= checks::kContactGap;
            painter.setPen(Qt::NoPen);
            painter.setBrush(good ? QColor(30, 170, 60) : QColor(220, 30, 30));
            painter.drawEllipse(to_px(at->second), 3.0, 3.0);
        }
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(215, 215, 225), 1));
        painter.drawRect(ox, oy, kCellW - 1, kCellH - 1);
    }
    painter.setPen(QColor(30, 30, 30));
    QFont small(QStringLiteral("WenQuanYi Zen Hei"));
    small.setPixelSize(12);
    painter.setFont(small);
    painter.drawText(x + 4, y + 14, QString::fromStdString(e.pose.value("id", std::string())));
    QFont big(QStringLiteral("WenQuanYi Zen Hei"));
    big.setPixelSize(15);
    painter.setFont(big);
    painter.drawText(x + 4, y + 33, QString::fromStdString(e.pose.value("ja", std::string())));
    if (!e.report.ok()) {
        painter.setFont(small);
        painter.setPen(QColor(220, 30, 30));
        painter.drawText(x + 3 * kCellW - 4 - 200, y + 14, 200, 16, Qt::AlignRight, QString::fromStdString(e.report.issues[0].check));
        painter.setPen(QPen(QColor(220, 30, 30), 3));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(x + 1, y + 1, 3 * kCellW - 3, kLabel + kCellH - 3);
    }
}

// The category's sheets: <dir>/<category>_<n>.png, kColumns × kRows poses each.
inline std::vector<std::filesystem::path> write(const std::filesystem::path& dir, const std::string& category, const std::vector<Entry>& entries) {
    std::filesystem::create_directories(dir);
    std::vector<std::filesystem::path> out;
    const std::size_t per = static_cast<std::size_t>(kColumns * kRows);
    for (std::size_t first = 0, n = 1; first < entries.size(); first += per, ++n) {
        const std::size_t count = std::min(per, entries.size() - first);
        const int rows = static_cast<int>((count + kColumns - 1) / kColumns);
        QImage sheet(kColumns * 3 * kCellW + (kColumns - 1) * 8, rows * (kLabel + kCellH + 8), QImage::Format_RGB888);
        sheet.fill(QColor(246, 246, 246));
        QPainter painter(&sheet);
        painter.setRenderHint(QPainter::Antialiasing);
        for (std::size_t i = 0; i < count; ++i) {
            const int col = static_cast<int>(i % kColumns);
            const int row = static_cast<int>(i / kColumns);
            draw_pose(painter, col * (3 * kCellW + 8), row * (kLabel + kCellH + 8), entries[first + i]);
        }
        painter.end();
        const std::filesystem::path file = dir / (category + "_" + std::to_string(n) + ".png");
        sheet.save(QString::fromStdString(file.string()));
        out.push_back(file);
    }
    return out;
}

// What a pose measured, for checks.json.
inline Json measured(const Entry& e) {
    const checks::Measure& m = e.report.measured;
    Json gaps = Json::object();
    for (const auto& [k, v] : m.gap) gaps[k] = std::isfinite(v) ? Json(std::round(v * 1000.0) / 1000.0) : Json(99.0);  // (no point of that part)
    Json lowest = Json::object();
    for (const auto& [k, p] : m.lowest) lowest[k] = Json::array({p[0] / m.unit, p[1] / m.unit, p[2] / m.unit});
    Json issues = Json::array();
    for (const auto& i : e.report.issues) issues.push_back(i.check + ": " + i.what);
    return Json::object({{"id", e.report.id},
                         {"ok", e.report.ok()},
                         {"issues", issues},
                         {"floor", m.floor / m.unit},
                         {"gap", gaps},
                         {"lowest", lowest},
                         {"seat", std::round(m.seat * 1000.0) / 1000.0},
                         {"down", m.down},
                         {"balance", std::round(m.balance * 1000.0) / 1000.0},
                         {"mass", Json::array({m.mass[0] / m.unit, m.mass[1] / m.unit, m.mass[2] / m.unit})},
                         {"inside", Json::object({{"torso", e.report.inside.torso}, {"head", e.report.inside.head}, {"overlap", e.report.inside.overlap},
                                                  {"torso_limb", e.report.inside.torso_limb}, {"head_limb", e.report.inside.head_limb}, {"overlap_limbs", e.report.inside.overlap_limbs}})},
                         {"triangles", m.triangles},
                         {"reach", m.reach}});
}

}  // namespace genko::test::pose_sheets
