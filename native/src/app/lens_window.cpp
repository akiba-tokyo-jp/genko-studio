// PC4 節 D6 (SPEC.md COLOR-08「フィルター」, design 5.2): フィルター → レンズ補正… and 広角補正…'s windows
// (app/lens_window.hpp). Their settings: core/filter_kinds (lens_correction, adaptive_wide_angle); their formulas:
// render/fx_lens.cpp and core/lens (PhotoCraft v0.2.0's lens.rs and wideangle.rs). The words are Photoshop's (日本語版).

#include "app/lens_window.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <stop_token>
#include <vector>

#include "app/ask.hpp"
#include "app/colours.hpp"
#include "app/filter_gallery_window.hpp"
#include "app/main_window.hpp"
#include "core/adjust.hpp"
#include "core/filter_kinds.hpp"
#include "core/lens.hpp"
#include "core/pyops.hpp"
#include "render/fx.hpp"
#include "render/ops_filters.hpp"
#include "render/segment/segment.hpp"
#include "service/wording.hpp"

namespace genko::app::lens_window {

using core::Json;

// --- the picture -----------------------------------------------------------------------------------------------------------

PictureView::PictureView(QWidget* parent) : QWidget(parent) {
    setMinimumSize(360, 300);
    setMouseTracking(false);
}

void PictureView::set_image(const QImage& image, const std::array<double, 4>& box_mm) {
    image_ = image;
    box_ = box_mm;
    update();
}

QRectF PictureView::target() const {
    const double w = std::max(box_[2] - box_[0], 1e-6), h = std::max(box_[3] - box_[1], 1e-6);
    const double k = std::min((width() - 16) / w, (height() - 16) / h);
    const double tw = w * k, th = h * k;
    return QRectF((width() - tw) / 2.0, (height() - th) / 2.0, tw, th);
}

double PictureView::scale() const { return target().width() / std::max(box_[2] - box_[0], 1e-6); }

QPointF PictureView::to_widget(double x_mm, double y_mm) const {
    const QRectF t = target();
    return QPointF(t.left() + (x_mm - box_[0]) * scale(), t.top() + (y_mm - box_[1]) * scale());
}

std::array<double, 2> PictureView::to_mm(const QPointF& at) const {
    const QRectF t = target();
    return {box_[0] + (at.x() - t.left()) / scale(), box_[1] + (at.y() - t.top()) / scale()};
}

void PictureView::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(96, 96, 96));
    const QRectF t = target();
    // (a clear picture over a checker, as Photoshop shows clear pixels)
    const int cell = 8;
    for (int y = static_cast<int>(t.top()); y < t.bottom(); y += cell)
        for (int x = static_cast<int>(t.left()); x < t.right(); x += cell)
            painter.fillRect(QRectF(x, y, std::min<double>(cell, t.right() - x), std::min<double>(cell, t.bottom() - y)),
                             ((x / cell + y / cell) % 2) != 0 ? QColor(204, 204, 204) : QColor(255, 255, 255));
    if (!image_.isNull()) {
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawImage(t, image_);
    }
    if (overlay) {
        painter.setRenderHint(QPainter::Antialiasing, true);
        overlay(painter);
    }
}

void PictureView::mousePressEvent(QMouseEvent* event) {
    down_ = true;
    if (press) press(to_mm(event->position()), event->modifiers(), event->button());
}

void PictureView::mouseMoveEvent(QMouseEvent* event) {
    if (down_ && drag) drag(to_mm(event->position()), event->modifiers());
}

void PictureView::mouseReleaseEvent(QMouseEvent* event) {
    if (!down_) return;
    down_ = false;
    if (release) release(to_mm(event->position()), event->modifiers());
}

namespace {

QString words(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

// The layer's picture the window works: what shows of it (its frame, the box the filter works) at a resolution that fits
// the window, made once.
struct Shown {
    render::Image picture;  // "RGBA"
    std::array<double, 4> box_mm{0, 0, 1, 1};
    int dpi = 72;
    render::Box area;       // (the picture on the page's grid at dpi)
    render::fx::Rect frame;  // (what shows of it, on that grid)
};

Shown shown_of(const MainWindow& window, const FilterWindowCall& call) {
    Shown out;
    const filter_gallery::Source source = filter_gallery::source_of(window, *call.page, *call.layer, call.filter);
    const std::array<double, 4> box = filter_gallery::shown_box(source);
    const double long_mm = std::max({box[2] - box[0], box[3] - box[1], 1.0});
    out.dpi = static_cast<int>(std::clamp(std::floor(640.0 / long_mm * 25.4), 4.0, 400.0));
    out.area = render::Box{static_cast<int>(std::floor(box[0] / 25.4 * out.dpi)), static_cast<int>(std::floor(box[1] / 25.4 * out.dpi)),
                           static_cast<int>(std::ceil(box[2] / 25.4 * out.dpi)), static_cast<int>(std::ceil(box[3] / 25.4 * out.dpi))};
    out.box_mm = {out.area.x0 * 25.4 / out.dpi, out.area.y0 * 25.4 / out.dpi, out.area.x1 * 25.4 / out.dpi, out.area.y1 * 25.4 / out.dpi};
    try {
        out.picture = source.picture(out.dpi, out.area).convert("RGBA");
    } catch (const std::exception&) {
        out.picture = render::Image::create("RGBA", render::Size{std::max(1, out.area.width()), std::max(1, out.area.height())}, render::Ink{0, 0, 0, 0});
    }
    // (what shows of it: the frame, on the grid)
    const std::string bytes = out.picture.tobytes();
    int x0 = out.picture.width(), y0 = out.picture.height(), x1 = 0, y1 = 0;
    for (int y = 0; y < out.picture.height(); ++y)
        for (int x = 0; x < out.picture.width(); ++x) {
            if (bytes[(std::size_t(y) * std::size_t(out.picture.width()) + std::size_t(x)) * 4 + 3] == 0) continue;
            x0 = std::min(x0, x);
            y0 = std::min(y0, y);
            x1 = std::max(x1, x + 1);
            y1 = std::max(y1, y + 1);
        }
    if (x1 <= x0 || y1 <= y0) x0 = 0, y0 = 0, x1 = out.picture.width(), y1 = out.picture.height();
    out.frame = render::fx::Rect{out.area.x0 + x0, out.area.y0 + y0, out.area.x0 + x1, out.area.y0 + y1};
    return out;
}

QImage qimage_of(const render::Image& rgba) {
    const std::string bytes = rgba.tobytes();
    return QImage(reinterpret_cast<const uchar*>(bytes.data()), rgba.width(), rgba.height(), rgba.width() * 4, QImage::Format_RGBA8888).copy();
}

// The kind's kernel on the shown picture with its settings (their normal form): the picture the window shows of it.
QImage worked(const Shown& shown, const std::string& kind, const Json& settings) {
    const render::fx::Kernel* kernel = render::fx::kernel(kind);
    const core::FilterInfo* info = core::filter_info(kind);
    if (kernel == nullptr || info == nullptr) return QImage();
    const render::ImageAllocationBudget budget{1ULL << 30};
    const auto surface = render::fx::eight_surface(shown.picture);
    const render::fx::PlaceScope place(shown.area.x0, shown.area.y0, shown.frame);
    render::fx::apply(*surface, *kernel, settings, info->inks, render::fx::Rect{0, 0, shown.picture.width(), shown.picture.height()}, nullptr,
                      shown.dpi / 200.0);
    return qimage_of(render::fx::take_image(*surface));
}

QDoubleSpinBox* spin(QWidget* parent, const char* name, double lo, double hi, double value, int decimals = 0, double step = 1) {
    auto* box = new QDoubleSpinBox(parent);
    box->setObjectName(QString::fromLatin1(name));
    box->setRange(lo, hi);
    box->setDecimals(decimals);
    box->setSingleStep(step);
    box->setValue(value);
    return box;
}

double number(const Json& v, const char* key, double fallback) {
    const Json* x = core::get(v, key);
    return x != nullptr && x->is_number() && !x->is_boolean() ? x->get<double>() : fallback;
}

bool flag(const Json& v, const char* key, bool fallback) {
    const Json* x = core::get(v, key);
    return x != nullptr && x->is_boolean() ? x->get<bool>() : fallback;
}

std::string text(const Json& v, const char* key, const char* fallback) {
    const Json* x = core::get(v, key);
    return x != nullptr && x->is_string() ? x->get<std::string>() : std::string(fallback);
}

// The window's frame shared by both: the picture with the result on it (on a thread, a moment after a change), the
// settings on the right, OK and キャンセル.
class LensDialog : public QDialog {
public:
    LensDialog(QWidget* parent, const FilterWindowCall& call, std::string kind, QString title)
        : QDialog(parent), call_(call), kind_(std::move(kind)) {
        setObjectName(QStringLiteral("filter_dialog"));
        setProperty("kind", QString::fromStdString(kind_));
        setWindowTitle(title);
        shown_ = shown_of(*call.window, call);
        auto* outer = new QHBoxLayout(this);
        auto* left = new QVBoxLayout;
        view_ = new PictureView(this);
        view_->setObjectName(QStringLiteral("lens_view"));
        left->addWidget(view_, 1);
        status_ = new QLabel(this);
        status_->setObjectName(QStringLiteral("lens_status"));
        status_->setWordWrap(true);
        left->addWidget(status_);
        outer->addLayout(left, 1);
        right_ = new QVBoxLayout;
        outer->addLayout(right_);
        timer_ = new QTimer(this);
        timer_->setSingleShot(true);
        timer_->setInterval(150);
        connect(timer_, &QTimer::timeout, this, [this] { run_preview(); });
        view_->set_image(qimage_of(shown_.picture), shown_.box_mm);
    }
    ~LensDialog() override {
        if (stop_) stop_->request_stop();
    }

    // The settings chosen (the op's: lens_correction's or adaptive_wide_angle's).
    virtual Json settings() const = 0;

protected:
    void finish() {
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("OK"));
        buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("キャンセル"));
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        right_->addStretch(1);
        right_->addWidget(buttons);
        schedule();
    }
    void schedule() { timer_->start(); }

    // The result over the picture, on a thread (the last one asked for wins); what the settings cannot be under it.
    void run_preview() {
        if (stop_) stop_->request_stop();
        Json params;
        try {
            const Json given = settings();
            const Json filled = render::resolve_filter(call_.window->book(), *call_.page, call_.layer, kind_, given);
            params = core::normalize_filter(kind_, filled, call_.page);
            status_->setText(QString());
        } catch (const std::exception& error) {
            status_->setText(wording::error(QString::fromUtf8(error.what())));
            view_->set_image(qimage_of(shown_.picture), shown_.box_mm);
            return;
        }
        auto stop = std::make_shared<std::stop_source>();
        stop_ = stop;
        setProperty("previewing", true);
        auto* watcher = new QFutureWatcher<QImage>(this);
        connect(watcher, &QFutureWatcher<QImage>::finished, this, [this, watcher, stop] {
            const QImage image = watcher->result();
            watcher->deleteLater();
            if (stop_ != stop || stop->stop_requested()) return;
            setProperty("previewing", false);
            ++previews_;
            setProperty("previews", previews_);
            if (!image.isNull()) view_->set_image(image, shown_.box_mm);
        });
        watcher->setFuture(QtConcurrent::run([shown = shown_, kind = kind_, params, token = stop->get_token()]() -> QImage {
            const render::segment::StopScope scope(token);
            try {
                return worked(shown, kind, params);
            } catch (...) {  // (stopped, refused, past the budget: none)
                return QImage();
            }
        }));
    }

    FilterWindowCall call_;
    std::string kind_;
    Shown shown_;
    PictureView* view_ = nullptr;
    QLabel* status_ = nullptr;
    QVBoxLayout* right_ = nullptr;
    QTimer* timer_ = nullptr;
    std::shared_ptr<std::stop_source> stop_;
    int previews_ = 0;
};

// --- レンズ補正 -----------------------------------------------------------------------------------------------------------

class LensCorrectionDialog : public LensDialog {
public:
    LensCorrectionDialog(QWidget* parent, const FilterWindowCall& call) : LensDialog(parent, call, "lens_correction", QStringLiteral("レンズ補正")) {
        const Json now = call.now.is_object() ? call.now : Json::object();
        auto* tabs = new QTabWidget(this);
        tabs->setObjectName(QStringLiteral("lens_tabs"));
        // 自動補正
        auto* automatic = new QWidget(tabs);
        auto* form = new QFormLayout(automatic);
        const auto camera = render::picture_camera(call.window->book(), *call.page, call.layer);
        QString said = QStringLiteral("この画像の EXIF には、カメラとレンズの情報がありません");
        if (camera) {
            QStringList parts;
            if (camera->make || camera->model) parts << words((camera->make.value_or("") + " " + camera->model.value_or("")));
            if (camera->lens) parts << words(*camera->lens);
            if (camera->focal_length) parts << QStringLiteral("焦点距離 %1 mm").arg(*camera->focal_length, 0, 'f', 1);
            if (camera->focal_length_35mm) parts << QStringLiteral("35 mm 換算 %1 mm").arg(*camera->focal_length_35mm, 0, 'f', 0);
            if (!parts.isEmpty()) said = parts.join(QStringLiteral("・"));
        }
        auto* camera_label = new QLabel(said, automatic);
        camera_label->setObjectName(QStringLiteral("lens_camera"));
        camera_label->setWordWrap(true);
        form->addRow(QStringLiteral("カメラとレンズ"), camera_label);
        profile_ = new QComboBox(automatic);
        profile_->setObjectName(QStringLiteral("lens_profile"));
        profile_->addItem(QStringLiteral("なし"), QStringLiteral("none"));
        profile_->addItem(QStringLiteral("一般的なレンズ（焦点距離から）"), QStringLiteral("generic"));
        profile_->addItem(QStringLiteral("自動（画像の EXIF から）"), QStringLiteral("auto"));
        const int found = profile_->findData(QString::fromStdString(text(now, "profile", camera ? "auto" : "none")));
        profile_->setCurrentIndex(std::max(0, found));
        form->addRow(QStringLiteral("レンズプロファイル"), profile_);
        focal_ = spin(automatic, "lens_focal_length", 0, 2000, number(now, "focal_length", 0), 1, 1);
        focal_->setSpecialValueText(QStringLiteral("（EXIF から）"));
        focal_->setSuffix(QStringLiteral(" mm"));
        form->addRow(QStringLiteral("焦点距離（35 mm 換算）"), focal_);
        distortion_on_ = new QCheckBox(QStringLiteral("ゆがみ収差"), automatic);
        distortion_on_->setObjectName(QStringLiteral("lens_correct_distortion"));
        distortion_on_->setChecked(flag(now, "correct_distortion", true));
        ca_on_ = new QCheckBox(QStringLiteral("色収差"), automatic);
        ca_on_->setObjectName(QStringLiteral("lens_correct_ca"));
        ca_on_->setChecked(flag(now, "correct_ca", true));
        vignette_on_ = new QCheckBox(QStringLiteral("周辺光量補正"), automatic);
        vignette_on_->setObjectName(QStringLiteral("lens_correct_vignette"));
        vignette_on_->setChecked(flag(now, "correct_vignette", true));
        form->addRow(QStringLiteral("補正"), distortion_on_);
        form->addRow(QString(), ca_on_);
        form->addRow(QString(), vignette_on_);
        auto_scale_ = new QCheckBox(QStringLiteral("画像を自動で拡大・縮小"), automatic);
        auto_scale_->setObjectName(QStringLiteral("lens_auto_scale"));
        auto_scale_->setChecked(flag(now, "auto_scale", false));
        form->addRow(QString(), auto_scale_);
        edge_ = new QComboBox(automatic);
        edge_->setObjectName(QStringLiteral("lens_edge"));
        edge_->addItem(QStringLiteral("透明"), QStringLiteral("transparency"));
        edge_->addItem(QStringLiteral("エッジを拡張"), QStringLiteral("extension"));
        edge_->addItem(QStringLiteral("黒"), QStringLiteral("black"));
        edge_->addItem(QStringLiteral("白"), QStringLiteral("white"));
        edge_->addItem(QStringLiteral("描画色"), QStringLiteral("color"));
        edge_->setCurrentIndex(std::max(0, edge_->findData(QString::fromStdString(text(now, "edge", "transparency")))));
        form->addRow(QStringLiteral("エッジ"), edge_);
        tabs->addTab(automatic, QStringLiteral("自動補正"));
        // カスタム
        auto* custom = new QWidget(tabs);
        auto* rows = new QFormLayout(custom);
        distortion_ = spin(custom, "lens_distortion", -100, 100, number(now, "distortion", 0), 2, 1);
        rows->addRow(QStringLiteral("ゆがみを補正"), distortion_);
        red_cyan_ = spin(custom, "lens_red_cyan", -100, 100, number(now, "red_cyan", 0), 2, 1);
        rows->addRow(QStringLiteral("赤／シアンのフリンジを補正"), red_cyan_);
        blue_yellow_ = spin(custom, "lens_blue_yellow", -100, 100, number(now, "blue_yellow", 0), 2, 1);
        rows->addRow(QStringLiteral("青／イエローのフリンジを補正"), blue_yellow_);
        vignette_ = spin(custom, "lens_vignette_amount", -100, 100, number(now, "vignette_amount", 0), 0, 1);
        rows->addRow(QStringLiteral("周辺光量補正：適用量"), vignette_);
        midpoint_ = spin(custom, "lens_vignette_midpoint", 0, 100, number(now, "vignette_midpoint", 50), 0, 1);
        rows->addRow(QStringLiteral("周辺光量補正：中心点"), midpoint_);
        vertical_ = spin(custom, "lens_vertical", -100, 100, number(now, "vertical", 0), 0, 1);
        rows->addRow(QStringLiteral("垂直方向の遠近補正"), vertical_);
        horizontal_ = spin(custom, "lens_horizontal", -100, 100, number(now, "horizontal", 0), 0, 1);
        rows->addRow(QStringLiteral("水平方向の遠近補正"), horizontal_);
        angle_ = spin(custom, "lens_angle", -180, 180, number(now, "angle", 0), 2, 0.1);
        angle_->setSuffix(QStringLiteral("°"));
        rows->addRow(QStringLiteral("角度"), angle_);
        scale_ = spin(custom, "lens_scale", 50, 150, number(now, "scale", 100), 0, 1);
        scale_->setSuffix(QStringLiteral(" %"));
        rows->addRow(QStringLiteral("拡大・縮小"), scale_);
        tabs->addTab(custom, QStringLiteral("カスタム"));
        right_->addWidget(tabs);
        grid_ = new QCheckBox(QStringLiteral("グリッドを表示"), this);
        grid_->setObjectName(QStringLiteral("lens_grid"));
        grid_->setChecked(true);
        right_->addWidget(grid_);
        straighten_ = new QPushButton(QStringLiteral("角度補正ツール"), this);
        straighten_->setObjectName(QStringLiteral("lens_straighten"));
        straighten_->setCheckable(true);
        straighten_->setToolTip(QStringLiteral("画像の上で、水平か垂直にしたい線に沿ってドラッグします"));
        right_->addWidget(straighten_);
        for (QComboBox* box : {profile_, edge_}) connect(box, &QComboBox::currentIndexChanged, this, [this] { schedule(); });
        for (QCheckBox* box : {distortion_on_, ca_on_, vignette_on_, auto_scale_}) connect(box, &QCheckBox::toggled, this, [this] { schedule(); });
        for (QDoubleSpinBox* box : {focal_, distortion_, red_cyan_, blue_yellow_, vignette_, midpoint_, vertical_, horizontal_, angle_, scale_})
            connect(box, &QDoubleSpinBox::valueChanged, this, [this] { schedule(); });
        connect(grid_, &QCheckBox::toggled, view_, [this] { view_->update(); });
        // 角度補正ツール: a line dragged on the picture makes the angle that levels it (core/lens straighten_angle)
        view_->press = [this](std::array<double, 2> at, Qt::KeyboardModifiers, Qt::MouseButton) {
            if (!straighten_->isChecked()) return;
            line_ = {at, at};
            view_->update();
        };
        view_->drag = [this](std::array<double, 2> at, Qt::KeyboardModifiers) {
            if (!line_) return;
            (*line_)[1] = at;
            view_->update();
        };
        view_->release = [this](std::array<double, 2> at, Qt::KeyboardModifiers) {
            if (!line_) return;
            (*line_)[1] = at;
            straighten(*line_);
            line_.reset();
            view_->update();
        };
        view_->overlay = [this](QPainter& painter) {
            if (grid_->isChecked()) {
                painter.setPen(QPen(QColor(120, 160, 255, 140), 1));
                const auto& b = view_->box_mm();
                const QPointF a = view_->to_widget(b[0], b[1]), z = view_->to_widget(b[2], b[3]);
                const double step = std::max(16.0, (z.x() - a.x()) / 16.0);
                for (double x = a.x(); x <= z.x() + 0.5; x += step) painter.drawLine(QPointF(x, a.y()), QPointF(x, z.y()));
                for (double y = a.y(); y <= z.y() + 0.5; y += step) painter.drawLine(QPointF(a.x(), y), QPointF(z.x(), y));
            }
            if (line_) {
                painter.setPen(QPen(QColor(255, 80, 40), 2));
                painter.drawLine(view_->to_widget((*line_)[0][0], (*line_)[0][1]), view_->to_widget((*line_)[1][0], (*line_)[1][1]));
            }
        };
        finish();
    }

    // The angle that levels (or uprights) a line drawn on the picture, added to the angle now (as Photoshop's tool sets it).
    void straighten(const std::array<std::array<double, 2>, 2>& line) {
        if (std::hypot(line[1][0] - line[0][0], line[1][1] - line[0][1]) < 1e-6) return;
        double angle = angle_->value() + core::lens::straighten_angle(line[0], line[1]);
        while (angle > 180.0) angle -= 360.0;
        while (angle < -180.0) angle += 360.0;
        angle_->setValue(angle);
        straighten_->setChecked(false);
    }

    Json settings() const override {
        Json out{{"profile", profile_->currentData().toString().toStdString()},
                 {"correct_distortion", distortion_on_->isChecked()},
                 {"correct_ca", ca_on_->isChecked()},
                 {"correct_vignette", vignette_on_->isChecked()},
                 {"auto_scale", auto_scale_->isChecked()},
                 {"edge", edge_->currentData().toString().toStdString()},
                 {"distortion", distortion_->value()},
                 {"red_cyan", red_cyan_->value()},
                 {"blue_yellow", blue_yellow_->value()},
                 {"vignette_amount", vignette_->value()},
                 {"vignette_midpoint", midpoint_->value()},
                 {"vertical", vertical_->value()},
                 {"horizontal", horizontal_->value()},
                 {"angle", angle_->value()},
                 {"scale", scale_->value()}};
        if (focal_->value() > 0) out["focal_length"] = focal_->value();
        if (out["edge"] == "color") {
            const auto& rgb = call_.window->pen().rgb;
            out["edge_color"] = Json::array({static_cast<int>(rgb[0]), static_cast<int>(rgb[1]), static_cast<int>(rgb[2])});
        }
        return out;
    }

private:
    QComboBox* profile_ = nullptr;
    QDoubleSpinBox* focal_ = nullptr;
    QCheckBox *distortion_on_ = nullptr, *ca_on_ = nullptr, *vignette_on_ = nullptr, *auto_scale_ = nullptr, *grid_ = nullptr;
    QComboBox* edge_ = nullptr;
    QDoubleSpinBox *distortion_ = nullptr, *red_cyan_ = nullptr, *blue_yellow_ = nullptr, *vignette_ = nullptr, *midpoint_ = nullptr;
    QDoubleSpinBox *vertical_ = nullptr, *horizontal_ = nullptr, *angle_ = nullptr, *scale_ = nullptr;
    QPushButton* straighten_ = nullptr;
    std::optional<std::array<std::array<double, 2>, 2>> line_;
};

// --- 広角補正 -------------------------------------------------------------------------------------------------------------

class WideAngleDialog : public LensDialog {
public:
    WideAngleDialog(QWidget* parent, const FilterWindowCall& call) : LensDialog(parent, call, "adaptive_wide_angle", QStringLiteral("広角補正")) {
        const Json now = call.now.is_object() ? call.now : Json::object();
        auto* form = new QFormLayout;
        model_ = new QComboBox(this);
        model_->setObjectName(QStringLiteral("wide_model"));
        model_->addItem(QStringLiteral("自動"), QStringLiteral("auto"));
        model_->addItem(QStringLiteral("魚眼"), QStringLiteral("fisheye"));
        model_->addItem(QStringLiteral("遠近法"), QStringLiteral("perspective"));
        model_->addItem(QStringLiteral("完全な球面"), QStringLiteral("full_spherical"));
        model_->setCurrentIndex(std::max(0, model_->findData(QString::fromStdString(text(now, "model", "auto")))));
        form->addRow(QStringLiteral("補正"), model_);
        scale_ = spin(this, "wide_scale", 50, 150, number(now, "scale", 100), 0, 1);
        scale_->setSuffix(QStringLiteral(" %"));
        form->addRow(QStringLiteral("拡大・縮小"), scale_);
        focal_ = spin(this, "wide_focal_length", 0, 200, number(now, "focal_length", 0), 2, 0.5);
        focal_->setSpecialValueText(QStringLiteral("（撮影時の設定）"));
        focal_->setSuffix(QStringLiteral(" mm"));
        form->addRow(QStringLiteral("焦点距離"), focal_);
        crop_ = spin(this, "wide_crop_factor", 0.1, 10, number(now, "crop_factor", 1), 2, 0.1);
        form->addRow(QStringLiteral("切り抜き係数"), crop_);
        right_->addLayout(form);
        auto* help = new QLabel(QStringLiteral("拘束ツール：画像の上で、まっすぐにしたい線の端から端へドラッグします（Shift で水平・垂直）"), this);
        help->setWordWrap(true);
        right_->addWidget(help);
        list_ = new QListWidget(this);
        list_->setObjectName(QStringLiteral("wide_constraints"));
        right_->addWidget(list_);
        orientation_ = new QComboBox(this);
        orientation_->setObjectName(QStringLiteral("wide_orientation"));
        orientation_->addItem(QStringLiteral("任意の方向"), QStringLiteral("free"));
        orientation_->addItem(QStringLiteral("水平方向"), QStringLiteral("horizontal"));
        orientation_->addItem(QStringLiteral("垂直方向"), QStringLiteral("vertical"));
        right_->addWidget(orientation_);
        auto* remove = new QPushButton(QStringLiteral("拘束を削除"), this);
        remove->setObjectName(QStringLiteral("wide_remove"));
        right_->addWidget(remove);
        if (const Json* given = core::get(now, "constraints"); given != nullptr && given->is_array()) {
            for (const Json& c : *given) {
                const Json* a = core::get(c, "a");
                const Json* b = core::get(c, "b");
                if (a == nullptr || b == nullptr || !a->is_array() || !b->is_array() || a->size() != 2 || b->size() != 2) continue;
                lines_.push_back({{(*a)[0].get<double>(), (*a)[1].get<double>()}, {(*b)[0].get<double>(), (*b)[1].get<double>()}, text(c, "orientation", "free")});
            }
        }
        refill();
        connect(model_, &QComboBox::currentIndexChanged, this, [this] { schedule(); });
        for (QDoubleSpinBox* box : {scale_, focal_, crop_}) connect(box, &QDoubleSpinBox::valueChanged, this, [this] { schedule(); });
        connect(list_, &QListWidget::currentRowChanged, this, [this](int row) {
            if (row < 0 || row >= static_cast<int>(lines_.size())) return;
            const QSignalBlocker quiet(orientation_);
            orientation_->setCurrentIndex(std::max(0, orientation_->findData(QString::fromStdString(lines_[std::size_t(row)].orientation))));
            view_->update();
        });
        connect(orientation_, &QComboBox::currentIndexChanged, this, [this] {
            const int row = list_->currentRow();
            if (row < 0 || row >= static_cast<int>(lines_.size())) return;
            lines_[std::size_t(row)].orientation = orientation_->currentData().toString().toStdString();
            refill(row);
            schedule();
        });
        connect(remove, &QPushButton::clicked, this, [this] {
            const int row = list_->currentRow();
            if (row < 0 || row >= static_cast<int>(lines_.size())) return;
            lines_.erase(lines_.begin() + row);
            refill();
            schedule();
        });
        // 拘束ツール: a line from where it is pressed to where it is let go (Shift: level or upright, whichever is nearer)
        view_->press = [this](std::array<double, 2> at, Qt::KeyboardModifiers, Qt::MouseButton button) {
            if (button != Qt::LeftButton) return;
            drawing_ = Line{at, at, "free"};
            view_->update();
        };
        view_->drag = [this](std::array<double, 2> at, Qt::KeyboardModifiers) {
            if (!drawing_) return;
            drawing_->b = at;
            view_->update();
        };
        view_->release = [this](std::array<double, 2> at, Qt::KeyboardModifiers keys) {
            if (!drawing_) return;
            drawing_->b = at;
            Line made = *drawing_;
            drawing_.reset();
            if (std::hypot(made.b[0] - made.a[0], made.b[1] - made.a[1]) * view_->scale() < 4.0) {  // (a click: none)
                view_->update();
                return;
            }
            if (keys.testFlag(Qt::ShiftModifier))
                made.orientation = std::fabs(made.b[0] - made.a[0]) >= std::fabs(made.b[1] - made.a[1]) ? "horizontal" : "vertical";
            if (lines_.size() >= core::kWideAngleMostConstraints) return;
            lines_.push_back(made);
            refill(static_cast<int>(lines_.size()) - 1);
            schedule();
        };
        view_->overlay = [this](QPainter& painter) {
            for (std::size_t i = 0; i < lines_.size(); ++i) {
                const bool chosen = static_cast<int>(i) == list_->currentRow();
                const QColor colour = lines_[i].orientation == "horizontal" ? QColor(255, 200, 0) : lines_[i].orientation == "vertical" ? QColor(255, 0, 200) : QColor(0, 200, 255);
                painter.setPen(QPen(colour, chosen ? 3 : 2));
                painter.drawPath(curve_of(lines_[i]));
            }
            if (drawing_) {
                painter.setPen(QPen(QColor(0, 200, 255), 1, Qt::DashLine));
                painter.drawLine(view_->to_widget(drawing_->a[0], drawing_->a[1]), view_->to_widget(drawing_->b[0], drawing_->b[1]));
            }
        };
        finish();
    }

    Json settings() const override {
        Json lines = Json::array();
        for (const Line& l : lines_)
            lines.push_back(Json{{"a", Json::array({l.a[0], l.a[1]})}, {"b", Json::array({l.b[0], l.b[1]})}, {"orientation", l.orientation}});
        Json out{{"model", model_->currentData().toString().toStdString()}, {"scale", scale_->value()}, {"crop_factor", crop_->value()}, {"constraints", lines}};
        if (focal_->value() > 0) out["focal_length"] = focal_->value();
        return out;
    }

private:
    struct Line {
        std::array<double, 2> a, b;
        std::string orientation;
    };

    void refill(int chosen = -1) {
        const QSignalBlocker quiet(list_);
        list_->clear();
        for (std::size_t i = 0; i < lines_.size(); ++i) {
            const QString way = lines_[i].orientation == "horizontal" ? QStringLiteral("水平方向") : lines_[i].orientation == "vertical" ? QStringLiteral("垂直方向") : QStringLiteral("任意の方向");
            list_->addItem(QStringLiteral("拘束 %1（%2）").arg(i + 1).arg(way));
        }
        list_->setCurrentRow(chosen);
        view_->update();
    }

    // The constraint as the lens draws its straight line: the great circle through its ends (core/lens Camera::arc), on the
    // picture's frame at the working 200 dpi.
    QPainterPath curve_of(const Line& line) const {
        QPainterPath path;
        core::lens::WideAngle wa;
        const std::string model = model_->currentData().toString().toStdString();
        wa.model = model == "fisheye" ? core::lens::Model::Fisheye : model == "perspective" ? core::lens::Model::Perspective
                   : model == "full_spherical" ? core::lens::Model::FullSpherical : core::lens::Model::Auto;
        wa.focal_length = focal_->value();
        wa.crop_factor = crop_->value();
        try {
            const Json filled = render::resolve_filter(call_.window->book(), *call_.page, call_.layer, kind_, settings());
            wa.focal_length = number(filled, "focal_length", wa.focal_length);
            wa.crop_factor = number(filled, "crop_factor", wa.crop_factor);
        } catch (const std::exception&) {
        }
        const double k = 200.0 / 25.4;  // (mm → working pixels)
        const double s = shown_.dpi / 200.0;
        const auto& f = shown_.frame;
        const core::lens::Camera cam = core::lens::Camera::of(wa, f.x0 / s, f.y0 / s, f.x1 / s, f.y1 / s);
        const std::vector<core::lens::XY> arc = cam.arc({line.a[0] * k, line.a[1] * k}, {line.b[0] * k, line.b[1] * k}, 24);
        for (std::size_t i = 0; i < arc.size(); ++i) {
            const QPointF p = view_->to_widget(arc[i][0] / k, arc[i][1] / k);
            if (i == 0) path.moveTo(p);
            else path.lineTo(p);
        }
        return path;
    }

    QComboBox* model_ = nullptr;
    QDoubleSpinBox *scale_ = nullptr, *focal_ = nullptr, *crop_ = nullptr;
    QListWidget* list_ = nullptr;
    QComboBox* orientation_ = nullptr;
    std::vector<Line> lines_;
    std::optional<Line> drawing_;
};

template <typename Window>
void open_window(const FilterWindowCall& call) {
    if (call.window == nullptr || call.page == nullptr || call.layer == nullptr) return;
    Window dialog(call.window, call);
    if (ask::exec(&dialog) != QDialog::Accepted) return;
    Json chosen;
    try {
        chosen = dialog.settings();
    } catch (const std::exception&) {
        return;
    }
    if (call.send) call.send(chosen);
}

}  // namespace

void open_lens(const FilterWindowCall& call) { open_window<LensCorrectionDialog>(call); }
void open_wide_angle(const FilterWindowCall& call) { open_window<WideAngleDialog>(call); }

}  // namespace genko::app::lens_window
