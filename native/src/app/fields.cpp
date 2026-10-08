#include "app/fields.hpp"

#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QImage>
#include <QPixmap>
#include <QSlider>
#include <QSpinBox>

#include <cmath>
#include <memory>

#include "app/theme.hpp"
#include "core/stroke_geom.hpp"
#include "render/brushes.hpp"

namespace genko::app {

namespace {

constexpr int kSteps = 1000;
constexpr double kPi = 3.14159265358979323846;

// The slider and its number joined both ways (the number's own minimum and maximum, evenly or on a log scale).
template <class Spin>
QWidget* joined(Spin* spin, bool log) {
    auto* row = new QWidget;
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    auto* slider = new QSlider(Qt::Horizontal);
    slider->setRange(0, kSteps);
    slider->setToolTip(spin->toolTip());
    double low = static_cast<double>(spin->minimum());
    const double high = static_cast<double>(spin->maximum());
    if (log) low = std::max(low, 1e-3);
    const auto to_slider = [=](double value) {
        if (log) return static_cast<int>(std::lround(kSteps * std::log(std::max(value, low) / low) / std::log(high / low)));
        const double span = high - low;
        return static_cast<int>(std::lround(kSteps * (value - low) / (span != 0 ? span : 1)));
    };
    const auto from_slider = [=](int pos) {
        const double t = static_cast<double>(pos) / kSteps;
        return log ? low * std::pow(high / low, t) : low + (high - low) * t;
    };
    auto following = std::make_shared<bool>(false);
    QObject::connect(slider, &QSlider::valueChanged, spin, [=](int pos) {
        if (*following) return;
        const double value = from_slider(pos);
        if constexpr (std::is_same_v<Spin, QDoubleSpinBox>) {
            const double scale = std::pow(10.0, spin->decimals());
            spin->setValue(std::round(value * scale) / scale);
        } else {
            spin->setValue(static_cast<int>(std::lround(value)));
        }
    });
    const auto follow = [=](double value) {
        *following = true;
        slider->setValue(to_slider(value));
        *following = false;
    };
    if constexpr (std::is_same_v<Spin, QDoubleSpinBox>) QObject::connect(spin, &QDoubleSpinBox::valueChanged, slider, follow);
    else QObject::connect(spin, &QSpinBox::valueChanged, slider, [follow](int v) { follow(v); });
    follow(static_cast<double>(spin->value()));
    spin->setMaximumWidth(96);
    layout->addWidget(slider, 1);
    layout->addWidget(spin);
    row->setProperty("slider", QVariant::fromValue(static_cast<QObject*>(slider)));
    return row;
}

}  // namespace

QWidget* slider_for(QDoubleSpinBox* spin, bool log) { return joined(spin, log); }
QWidget* slider_for(QSpinBox* spin, bool log) { return joined(spin, log); }

QWidget* with_value(QSlider* slider, const QString& suffix) {
    auto* row = new QWidget;
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    auto* value = new QLabel;
    value->setMinimumWidth(value->fontMetrics().horizontalAdvance(QStringLiteral("100%")) + 4);
    value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    QObject::connect(slider, &QSlider::valueChanged, value, [value, suffix](int v) { value->setText(QString::number(v) + suffix); });
    value->setText(QString::number(slider->value()) + suffix);
    layout->addWidget(slider, 1);
    layout->addWidget(value);
    return row;
}

LineSample::LineSample(std::function<core::Json()> fields, QWidget* parent) : QLabel(parent), fields_(std::move(fields)) {
    setMinimumHeight(56);
    setMinimumWidth(80);
    // (as wide as the panel gives it: its picture follows the width, never the other way round)
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    setObjectName(QStringLiteral("lineSample"));
    setAlignment(Qt::AlignCenter);
    setToolTip(QStringLiteral("今の設定で引いた線の見本（弱く → 強く → 弱く）"));
    timer_.setSingleShot(true);
    timer_.setInterval(60);
    connect(&timer_, &QTimer::timeout, this, &LineSample::draw);
}

void LineSample::resizeEvent(QResizeEvent* event) {
    QLabel::resizeEvent(event);
    refresh();
}

void LineSample::draw() {
    const core::Json f = fields_();
    constexpr int scale = 2;
    const int w = std::max(80, width() - 10) * scale, h = 50 * scale;
    const int dpi = 96 * scale;
    const double mm = 25.4 / dpi;
    const double gamma = f.value("pressure_gamma", 1.0);
    core::PenPoints points;
    for (int i = 0; i <= 80; ++i)
        points.push_back(core::PenPoint{(10 + i * (w - 20) / 80.0) * mm, (h / 2.0 + h * 0.22 * std::sin(i / 12.7)) * mm,
                                        std::pow(std::max(0.02, std::sin(kPi * i / 80)), gamma)});
    if (f.value("taper", false)) {
        std::optional<double> in, out;
        if (f.contains("taper_in_mm")) in = f["taper_in_mm"].get<double>();
        if (f.contains("taper_out_mm")) out = f["taper_out_mm"].get<double>();
        points = core::taper_points(points, in, out);
    }
    const double width_mm = std::max(0.1, std::min(f.value("width_mm", 1.0), h * 0.3 * mm));
    QImage image(w, h, QImage::Format_RGBA8888);
    image.fill(Qt::transparent);
    std::optional<render::brushes::Coverage> drawn;
    try {
        drawn = render::brushes::draw(render::Size{w, h}, points, dpi, width_mm, f.value("kind", std::string("gpen")), "sample", {},
                                      f.value("pressure_opacity", 0.0));
    } catch (const std::exception&) {
        drawn.reset();  // (a broken brush of one's own: no sample rather than an error)
    }
    if (drawn) {
        std::array<int, 3> rgb{20, 20, 20};
        if (f.contains("rgb") && f["rgb"].is_array() && f["rgb"].size() >= 3)
            for (int c = 0; c < 3; ++c) rgb[static_cast<std::size_t>(c)] = f["rgb"][static_cast<std::size_t>(c)].get<int>();
        if (rgb == std::array<int, 3>{20, 20, 20} && theme::tokens().dark) rgb = {225, 225, 228};  // (black ink on a dark panel: shown light)
        const double opacity = std::max(0.08, f.value("opacity", 1.0));
        const std::string cover = drawn->mask.tobytes();
        const int cw = drawn->mask.width(), ch = drawn->mask.height();
        for (int y = 0; y < ch; ++y) {
            const int iy = drawn->origin.y + y;
            if (iy < 0 || iy >= h) continue;
            uchar* row = image.scanLine(iy);
            for (int x = 0; x < cw; ++x) {
                const int ix = drawn->origin.x + x;
                if (ix < 0 || ix >= w) continue;
                const int a = static_cast<int>(static_cast<unsigned char>(cover[static_cast<std::size_t>(y) * cw + x]) * opacity);
                if (a == 0) continue;
                row[ix * 4] = static_cast<uchar>(rgb[0]);
                row[ix * 4 + 1] = static_cast<uchar>(rgb[1]);
                row[ix * 4 + 2] = static_cast<uchar>(rgb[2]);
                row[ix * 4 + 3] = static_cast<uchar>(a);
            }
        }
    }
    QPixmap pixmap = QPixmap::fromImage(image);
    pixmap.setDevicePixelRatio(scale);
    setPixmap(pixmap);
}

}  // namespace genko::app
