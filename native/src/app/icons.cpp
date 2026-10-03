#include "app/icons.hpp"

#include <QFile>
#include <QFont>
#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QSvgRenderer>

#include <cmath>
#include <map>
#include <tuple>

#include "app/theme.hpp"

// (Q_INIT_RESOURCE must be used outside of any namespace)
static void genko_load_app_resources() {
    Q_INIT_RESOURCE(genko_app);
    Q_INIT_RESOURCE(genko_fonts);
}

namespace genko::app::icons {

namespace {

const QHash<QString, QString>& lucide() {
    static const QHash<QString, QString> table = {
        {"select", "mouse-pointer-2"}, {"pen", "pen-tool"}, {"eraser", "eraser"}, {"frame", "layout-dashboard"},
        {"fill", "paint-bucket"}, {"lassofill", "lasso"}, {"lasso", "lasso-select"}, {"picker", "pipette"},
        {"blend", "blend"}, {"shape", "shapes"}, {"rect", "square-dashed"}, {"wand", "wand-sparkles"},
        {"reshape", "spline"}, {"ruler", "ruler"}, {"3d", "box"}, {"stamp", "stamp"}, {"undo", "undo-2"},
        {"redo", "redo-2"}, {"zoom_in", "zoom-in"}, {"zoom_out", "zoom-out"}, {"fit", "scan"}, {"prev", "chevron-left"},
        {"next", "chevron-right"}, {"move", "move"}, {"export", "share"}, {"add", "plus"}, {"pen_layer", "pen-line"},
        {"paint_layer", "paintbrush"}, {"folder", "folder-plus"}, {"delete", "trash-2"}, {"up", "arrow-up"},
        {"down", "arrow-down"}, {"approve", "circle-check"}, {"back", "corner-up-left"}, {"expand", "maximize-2"},
        {"search", "search"}, {"settings", "settings"}, {"story", "file-text"}, {"check", "clipboard-check"},
        {"page", "file"}, {"open", "folder-open"}, {"book", "book-open"}, {"duplicate", "copy"},
        {"merge", "arrow-down-to-line"}, {"more", "ellipsis"}, {"info", "info"}, {"eye", "eye"}, {"eye_off", "eye-off"},
        {"close", "x"}, {"caret_up", "chevron-up"}, {"caret_down", "chevron-down"}, {"kind_strokes", "pen-line"},
        {"kind_raster", "paintbrush"}, {"kind_folder", "folder"}, {"kind_placed", "image"}, {"kind_tone", "grid-3x3"},
        {"kind_fill", "square"}, {"kind_adjust", "contrast"}, {"kind_other", "layers"},
    };
    return table;
}

// A Lucide picture in this colour (its lines are drawn in currentColor).
QPixmap lucide_picture(const QString& file, const QString& colour, int size = 64) {
    QFile source(QStringLiteral(":/genko/lucide/%1.svg").arg(file));
    if (!source.open(QIODevice::ReadOnly)) return {};
    QByteArray svg = source.readAll();
    svg.replace("currentColor", colour.toUtf8());
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    QSvgRenderer(svg).render(&painter, QRectF(4, 4, size - 8, size - 8));
    painter.end();
    return pixmap;
}

QPen line(const QColor& colour, double width = 2.4, Qt::PenStyle style = Qt::SolidLine) {
    return QPen(colour, width, style, Qt::RoundCap, Qt::RoundJoin);
}

// The few pictures Genko draws itself (on a 32 × 32 grid).
void draw(const QString& name, QPainter& p, const QColor& ink, const QColor& accent) {
    p.setPen(line(ink));
    p.setBrush(Qt::NoBrush);
    if (name == QLatin1String("text")) {
        QFont font;
        font.setPixelSize(21);
        font.setWeight(QFont::DemiBold);
        p.setFont(font);
        p.setPen(ink);
        p.drawText(QRectF(0, 0, 32, 32), Qt::AlignCenter, QStringLiteral("あ"));
    } else if (name == QLatin1String("effect")) {
        for (int a = 0; a < 360; a += 30) {
            const double r = a * 3.14159265358979323846 / 180.0;
            p.drawLine(QPointF(16 + 6 * std::cos(r), 16 + 6 * std::sin(r)), QPointF(16 + 14 * std::cos(r), 16 + 14 * std::sin(r)));
        }
    } else if (name == QLatin1String("gradient")) {
        QLinearGradient shade(4, 16, 28, 16);
        shade.setColorAt(0, ink);
        shade.setColorAt(1, QColor(255, 255, 255, 0));
        p.setBrush(shade);
        p.drawRect(QRectF(4, 8, 24, 16));
    } else if (name == QLatin1String("zoom")) {
        p.drawEllipse(QRectF(4, 4, 18, 18));
        p.drawLine(QPointF(19, 19), QPointF(28, 28));
        p.setPen(line(accent, 1.8));
        p.drawLine(QPointF(9, 13), QPointF(17, 13));
        p.drawLine(QPointF(13, 9), QPointF(13, 17));
    } else {
        p.drawRect(QRectF(6, 6, 20, 20));
    }
}

QPixmap picture(const QString& name, const QString& ink, const QString& accent) {
    if (const auto it = lucide().find(name); it != lucide().end()) {
        const QPixmap pixmap = lucide_picture(it.value(), ink);
        if (!pixmap.isNull()) return pixmap;
    }
    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.scale(2, 2);
    draw(name, painter, QColor(ink), QColor(accent));
    painter.end();
    return pixmap;
}

}  // namespace

void init_resources() {
    static const bool loaded = [] {
        genko_load_app_resources();
        return true;
    }();
    (void)loaded;
}

QIcon icon(const char* name, const QString& ink, const QString& accent) {
    init_resources();
    static std::map<std::tuple<QString, QString, QString>, QIcon> made;
    const auto key = std::make_tuple(QString::fromLatin1(name), ink, accent);
    if (const auto it = made.find(key); it != made.end()) return it->second;
    QIcon out(picture(QString::fromLatin1(name), ink, ink));
    const QPixmap chosen = picture(QString::fromLatin1(name), accent, accent);
    for (const auto mode : {QIcon::Normal, QIcon::Active, QIcon::Selected}) out.addPixmap(chosen, mode, QIcon::On);
    made.emplace(key, out);
    return out;
}

QIcon icon(const char* name) {
    const theme::Tokens t = theme::tokens();
    return icon(name, t.text, t.accent);
}

}  // namespace genko::app::icons
