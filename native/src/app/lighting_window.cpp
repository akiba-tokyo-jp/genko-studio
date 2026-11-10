// PC4 節 D4 (SPEC.md COLOR-08「フィルター」, design 5.2): フィルター → 描画 → 照明効果…'s window (app/lighting_window.hpp). Its
// settings: core/filter_kinds (render_lighting_effects); its formula: render/fx_render.cpp (PhotoCraft v0.2.0's render.rs
// lighting). The words are Photoshop's (日本語版).

#include "app/lighting_window.hpp"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <stop_token>

#include "app/ask.hpp"
#include "app/canvas.hpp"
#include "app/filter_gallery_window.hpp"
#include "app/lens_window.hpp"
#include "app/main_window.hpp"
#include "app/theme.hpp"
#include "core/adjust.hpp"
#include "core/filter_kinds.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "render/channels.hpp"
#include "render/fx.hpp"
#include "render/image_ops.hpp"
#include "render/page.hpp"
#include "render/segment/segment.hpp"
#include "service/wording.hpp"

namespace genko::app::lighting_window {

namespace {

using core::Json;

constexpr double kPi = std::numbers::pi;
constexpr double kGrab = 9.0;

double num(const Json& v, const char* key, double fallback) {
    const Json* x = core::get(v, key);
    return x != nullptr && x->is_number() && !x->is_boolean() ? x->get<double>() : fallback;
}

QImage qimage_of(const render::Image& rgba) {
    const std::string bytes = rgba.tobytes();
    return QImage(reinterpret_cast<const uchar*>(bytes.data()), rgba.width(), rgba.height(), rgba.width() * 4, QImage::Format_RGBA8888).copy();
}

// What the window works on: the layer's picture over the base box at a resolution that fits the window, the base box on
// that grid, and a channel's density over the page there (a texture of the page's).
struct Shown {
    render::Image picture;  // "RGBA" over `area`
    std::array<double, 4> box_mm{0, 0, 1, 1};
    int dpi = 72;
    render::Box area;
    std::uint32_t page_w = 1, page_h = 1;  // (the page's grid at dpi)
};

// A channel of the page over the shown area at the window's resolution (its pixel (0, 0) the area's), as the kernel reads
// a texture: made once on a thread, inside the budget, and kept by the window.
std::shared_ptr<const render::fx::Picture> channel_over(const core::Channel& channel, const Shown& shown) {
    const render::Image mask =
        render::channels::mask_of(channel, render::Size{static_cast<int>(shown.page_w), static_cast<int>(shown.page_h)}).crop(shown.area);
    const render::image_ops::Held held(std::uint64_t(mask.width()) * std::uint64_t(mask.height()) * 5);  // (its bytes and the picture of four)
    const std::string bytes = mask.tobytes();
    auto out = std::make_shared<render::fx::Picture>();
    out->width = mask.width();
    out->height = mask.height();
    out->rgba.resize(bytes.size() * 4);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        out->rgba[i * 4] = out->rgba[i * 4 + 1] = out->rgba[i * 4 + 2] = bytes[i];
        out->rgba[i * 4 + 3] = static_cast<char>(255);
    }
    return out;
}

struct Worked {
    QImage image;
    std::shared_ptr<const render::fx::Picture> texture;  // (the channel's, when one was read)
};

// The result of the settings over the shown picture (on a thread, inside a budget): the op's own kernel over the area alone
// — its base box, so the lights are shares of it as the op's on the grid are —, a channel of the page as its texture made
// first when the window has not got it yet.
Worked worked(const Shown& shown, const Json& settings, std::shared_ptr<const render::fx::Picture> texture, const std::optional<core::Channel>& channel) {
    Worked out;
    const render::fx::Kernel* kernel = render::fx::kernel("render_lighting_effects");
    if (kernel == nullptr) return out;
    const render::ImageAllocationBudget budget{1ULL << 30};
    if (!texture && channel) texture = channel_over(*channel, shown);
    out.texture = texture;
    const auto surface = render::fx::eight_surface(shown.picture);
    render::fx::Reads reads;
    reads.map = texture.get();
    render::fx::apply(*surface, *kernel, settings, core::FilterInks::Meaning, render::fx::Rect{0, 0, shown.picture.width(), shown.picture.height()}, nullptr,
                      shown.dpi / 200.0, nullptr, {}, reads);
    out.image = qimage_of(render::fx::take_image(*surface));
    return out;
}

QDoubleSpinBox* spin(QWidget* parent, const char* name, double lo, double hi, int decimals, double step) {
    auto* box = new QDoubleSpinBox(parent);
    box->setObjectName(QString::fromLatin1(name));
    box->setRange(lo, hi);
    box->setDecimals(decimals);
    box->setSingleStep(step);
    box->setKeyboardTracking(false);
    return box;
}

QString kind_words(const std::string& kind) {
    return kind == "point" ? QStringLiteral("点光源") : kind == "infinite" ? QStringLiteral("無限光") : QStringLiteral("スポットライト");
}

enum class Grip { None, Place, Target, Direction };

class LightingDialog : public QDialog {
public:
    LightingDialog(QWidget* parent, const FilterWindowCall& call) : QDialog(parent), call_(call) {
        setObjectName(QStringLiteral("filter_dialog"));
        setProperty("kind", QStringLiteral("render_lighting_effects"));
        setWindowTitle(QStringLiteral("照明効果"));
        smart_ = call.layer != nullptr && call.layer->kind == core::LayerKind::Smart;
        try {
            params_ = core::normalize_filter("render_lighting_effects", call.now.is_object() ? call.now : Json::object(), call.page);
        } catch (const std::exception&) {
            params_ = core::normalize_filter("render_lighting_effects", Json::object(), call.page);
        }
        make_shown();
        auto* outer = new QHBoxLayout(this);
        auto* left = new QVBoxLayout;
        view_ = new lens_window::PictureView(this);
        view_->setObjectName(QStringLiteral("lighting_view"));
        view_->set_image(qimage_of(shown_.picture), shown_.box_mm);
        view_->overlay = [this](QPainter& p) { draw(p); };
        view_->press = [this](std::array<double, 2> mm, Qt::KeyboardModifiers, Qt::MouseButton) { press({mm[0], mm[1]}); };
        view_->drag = [this](std::array<double, 2> mm, Qt::KeyboardModifiers) { drag({mm[0], mm[1]}); };
        view_->release = [this](std::array<double, 2>, Qt::KeyboardModifiers) { grip_ = Grip::None; };
        left->addWidget(view_, 1);
        status_ = new QLabel(this);
        status_->setObjectName(QStringLiteral("lighting_status"));
        status_->setWordWrap(true);
        left->addWidget(status_);
        outer->addLayout(left, 1);
        auto* right = new QVBoxLayout;
        outer->addLayout(right);
        // the lights
        list_ = new QListWidget(this);
        list_->setObjectName(QStringLiteral("lighting_lights"));
        list_->setMaximumHeight(120);
        right->addWidget(new QLabel(QStringLiteral("ライト"), this));
        right->addWidget(list_);
        auto* adds = new QHBoxLayout;
        for (const auto& [kind, name] : {std::pair<const char*, const char*>{"spot", "lighting_add_spot"}, {"point", "lighting_add_point"}, {"infinite", "lighting_add_infinite"}}) {
            auto* add = new QPushButton(QStringLiteral("＋%1").arg(kind_words(kind)), this);
            add->setObjectName(QString::fromLatin1(name));
            const std::string k = kind;
            connect(add, &QPushButton::clicked, this, [this, k] { add_light(k); });
            adds->addWidget(add);
        }
        right->addLayout(adds);
        auto* remove = new QPushButton(QStringLiteral("ライトを削除"), this);
        remove->setObjectName(QStringLiteral("lighting_delete"));
        connect(remove, &QPushButton::clicked, this, [this] { delete_light(); });
        right->addWidget(remove);
        auto* light = new QFormLayout;
        right->addLayout(light);
        kind_ = new QComboBox(this);
        kind_->setObjectName(QStringLiteral("lighting_kind"));
        for (const char* k : {"spot", "point", "infinite"}) kind_->addItem(kind_words(k), QString::fromLatin1(k));
        light->addRow(QStringLiteral("種類"), kind_);
        colour_ = new QPushButton(this);
        colour_->setObjectName(QStringLiteral("lighting_color"));
        light->addRow(QStringLiteral("カラー"), colour_);
        intensity_ = spin(this, "lighting_intensity", -100, 100, 0, 1);
        light->addRow(QStringLiteral("強度"), intensity_);
        hotspot_ = spin(this, "lighting_hotspot", 0, 100, 0, 1);
        light->addRow(QStringLiteral("ホットスポット（%）"), hotspot_);
        cone_ = spin(this, "lighting_cone", 1, 89, 0, 1);
        light->addRow(QStringLiteral("円錐（°）"), cone_);
        z_ = spin(this, "lighting_z", 0, 10, 2, 0.05);
        light->addRow(QStringLiteral("高さ"), z_);
        radius_ = spin(this, "lighting_radius", 0.01, 10, 2, 0.05);
        light->addRow(QStringLiteral("半径"), radius_);
        angle_ = spin(this, "lighting_angle", -360, 360, 0, 5);
        light->addRow(QStringLiteral("角度（°）"), angle_);
        elevation_ = spin(this, "lighting_elevation", 0, 90, 0, 1);
        light->addRow(QStringLiteral("仰角（°）"), elevation_);
        // the surface
        auto* surface = new QFormLayout;
        right->addWidget(new QLabel(QStringLiteral("プロパティ"), this));
        right->addLayout(surface);
        gloss_ = spin(this, "lighting_gloss", -100, 100, 0, 1);
        surface->addRow(QStringLiteral("光沢"), gloss_);
        metallic_ = spin(this, "lighting_metallic", -100, 100, 0, 1);
        surface->addRow(QStringLiteral("メタリック"), metallic_);
        exposure_ = spin(this, "lighting_exposure", -100, 100, 0, 1);
        surface->addRow(QStringLiteral("露光量"), exposure_);
        ambience_ = spin(this, "lighting_ambience", -100, 100, 0, 1);
        surface->addRow(QStringLiteral("環境光"), ambience_);
        texture_ = new QComboBox(this);
        texture_->setObjectName(QStringLiteral("lighting_texture"));
        for (const auto& [words, value] : {std::pair<const char*, const char*>{"なし", "none"}, {"レッド", "red"}, {"グリーン", "green"}, {"ブルー", "blue"},
                                           {"透明部分", "transparency"}, {"輝度", "luminance"}})
            texture_->addItem(QString::fromUtf8(words), QString::fromLatin1(value));
        if (!smart_ && call.page != nullptr) {  // (an alpha channel of the page: not a smart filter's — its page has none)
            for (const core::Channel& channel : call.page->channels)
                if (channel.kind == core::ChannelKind::Alpha)
                    texture_->addItem(QString::fromStdString(channel.name), QStringLiteral("channel:%1").arg(QString::fromStdString(channel.id)));
        }
        surface->addRow(QStringLiteral("テクスチャ"), texture_);
        height_ = spin(this, "lighting_height", 0, 100, 0, 1);
        surface->addRow(QStringLiteral("高さ"), height_);
        white_ = new QCheckBox(QStringLiteral("白い部分を高く"), this);
        white_->setObjectName(QStringLiteral("lighting_white_is_high"));
        surface->addRow(QString(), white_);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("OK"));
        buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("キャンセル"));
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        right->addStretch(1);
        right->addWidget(buttons);
        timer_ = new QTimer(this);
        timer_->setSingleShot(true);
        timer_->setInterval(150);
        connect(timer_, &QTimer::timeout, this, [this] { run_preview(); });
        fill_list();
        show_light();
        show_surface();
        wire();
        timer_->start();
    }
    ~LightingDialog() override {
        if (stop_) stop_->request_stop();
    }

    // The settings chosen (render_lighting_effects' normal form).
    Json settings() const { return core::normalize_filter("render_lighting_effects", params_, call_.page); }

private:
    // the picture: the base box (a smart object's page; a layer's selection's box, else the page) at a resolution that fits
    void make_shown() {
        const core::Page& page = *call_.page;
        const double pw = page.spec.width_mm.value(), ph = page.spec.height_mm.value();
        std::array<double, 4> box{0, 0, pw, ph};
        if (!smart_ && call_.window->selection_area()) {
            const auto b = call_.window->canvas()->selection_box();
            if (b[2] > b[0] && b[3] > b[1]) box = b;
        }
        const double long_mm = std::max({box[2] - box[0], box[3] - box[1], 1.0});
        shown_.dpi = static_cast<int>(std::clamp(std::floor(560.0 / long_mm * 25.4), 4.0, 300.0));
        shown_.area = render::Box{static_cast<int>(std::floor(box[0] / 25.4 * shown_.dpi)), static_cast<int>(std::floor(box[1] / 25.4 * shown_.dpi)),
                                  static_cast<int>(std::ceil(box[2] / 25.4 * shown_.dpi)), static_cast<int>(std::ceil(box[3] / 25.4 * shown_.dpi))};
        shown_.box_mm = {shown_.area.x0 * 25.4 / shown_.dpi, shown_.area.y0 * 25.4 / shown_.dpi, shown_.area.x1 * 25.4 / shown_.dpi,
                         shown_.area.y1 * 25.4 / shown_.dpi};
        shown_.page_w = static_cast<std::uint32_t>(std::max(1, render::mm_to_px(pw, shown_.dpi)));
        shown_.page_h = static_cast<std::uint32_t>(std::max(1, render::mm_to_px(ph, shown_.dpi)));
        try {
            const filter_gallery::Source source = filter_gallery::source_of(*call_.window, page, *call_.layer, call_.filter);
            shown_.picture = source.picture(shown_.dpi, shown_.area).convert("RGBA");
        } catch (const std::exception&) {
            shown_.picture = render::Image::create("RGBA", render::Size{std::max(1, shown_.area.width()), std::max(1, shown_.area.height())},
                                                   render::Ink{128, 128, 128, 255});
        }
    }

    double bw() const { return std::max(shown_.box_mm[2] - shown_.box_mm[0], 1e-6); }
    double bh() const { return std::max(shown_.box_mm[3] - shown_.box_mm[1], 1e-6); }
    double shortest() const { return std::min(bw(), bh()); }
    QPointF place(double x, double y) const { return {shown_.box_mm[0] + x * bw(), shown_.box_mm[1] + y * bh()}; }
    std::array<double, 2> share(QPointF mm) const { return {(mm.x() - shown_.box_mm[0]) / bw(), (mm.y() - shown_.box_mm[1]) / bh()}; }
    Json& lights() { return params_["lights"]; }
    Json* chosen() {
        Json& all = lights();
        return chosen_ >= 0 && chosen_ < static_cast<int>(all.size()) ? &all[static_cast<std::size_t>(chosen_)] : nullptr;
    }
    // An infinite light's direction's handle (from its place toward where the light comes from).
    QPointF direction_tip(const Json& l) const {
        const double a = num(l, "angle", 135) * kPi / 180.0;
        const double reach = 36.0 / std::max(view_->scale(), 1e-6);
        return place(num(l, "x", 0.25), num(l, "y", 0.2)) + QPointF(std::cos(a), -std::sin(a)) * reach;
    }

    void draw(QPainter& p) {
        const QColor ink = theme::accent();
        const Json& all = params_["lights"];
        for (std::size_t i = 0; i < all.size(); ++i) {
            const Json& l = all[i];
            const bool chosen = static_cast<int>(i) == chosen_;
            const std::string kind = core::py_str(core::py_get(l, "kind", Json("spot")));
            const QPointF at = place(num(l, "x", 0.25), num(l, "y", 0.2));
            p.setPen(QPen(ink, chosen ? 2.0 : 1.2));
            p.setBrush(Qt::NoBrush);
            if (kind == "point") {
                const double r = num(l, "radius", 0.8) * shortest() * view_->scale();
                p.drawEllipse(view_->to_widget(at.x(), at.y()), r, r);
            } else if (kind == "spot") {
                const QPointF target = place(num(l, "target_x", 0.5), num(l, "target_y", 0.55));
                const double height = num(l, "z", 0.6) * shortest();
                const double far = std::hypot(std::hypot(target.x() - at.x(), target.y() - at.y()), height);
                const double r = far * std::tan(std::clamp(num(l, "cone", 45), 1.0, 89.0) * kPi / 180.0) * view_->scale();
                p.drawLine(view_->to_widget(at.x(), at.y()), view_->to_widget(target.x(), target.y()));
                p.setPen(QPen(ink, 1.0, Qt::DashLine));
                p.drawEllipse(view_->to_widget(target.x(), target.y()), r, r);
                p.setPen(QPen(ink, 1.5));
                p.setBrush(chosen && grip_ == Grip::Target ? ink : QColor(255, 255, 255, 230));
                const QPointF t = view_->to_widget(target.x(), target.y());
                p.drawRect(QRectF(t.x() - 4.5, t.y() - 4.5, 9, 9));
            } else {
                const QPointF tip = direction_tip(l);
                p.drawLine(view_->to_widget(at.x(), at.y()), view_->to_widget(tip.x(), tip.y()));
                p.setBrush(QColor(255, 255, 255, 230));
                const QPointF t = view_->to_widget(tip.x(), tip.y());
                p.drawRect(QRectF(t.x() - 4.5, t.y() - 4.5, 9, 9));
            }
            const QColor colour = colour_of(l);
            p.setPen(QPen(ink, 2.0));
            p.setBrush(colour);
            p.drawEllipse(view_->to_widget(at.x(), at.y()), chosen ? 9 : 7, chosen ? 9 : 7);
        }
    }

    static QColor colour_of(const Json& l) {
        const Json* c = core::get(l, "color");
        if (c == nullptr || !c->is_array() || c->size() != 3) return QColor(255, 255, 255);
        return QColor((*c)[0].get<int>(), (*c)[1].get<int>(), (*c)[2].get<int>());
    }

    bool near(QPointF a, QPointF b) const { return std::hypot(a.x() - b.x(), a.y() - b.y()) * view_->scale() <= kGrab; }

    void press(QPointF mm) {
        grip_ = Grip::None;
        Json& all = lights();
        if (Json* l = chosen()) {
            const std::string kind = core::py_str(core::py_get(*l, "kind", Json("spot")));
            if (kind == "spot" && near(mm, place(num(*l, "target_x", 0.5), num(*l, "target_y", 0.55)))) grip_ = Grip::Target;
            else if (kind == "infinite" && near(mm, direction_tip(*l))) grip_ = Grip::Direction;
            if (grip_ != Grip::None) return;
        }
        for (std::size_t i = all.size(); i-- > 0;) {
            if (!near(mm, place(num(all[i], "x", 0.25), num(all[i], "y", 0.2)))) continue;
            chosen_ = static_cast<int>(i);
            grip_ = Grip::Place;
            const QSignalBlocker quiet(list_);
            list_->setCurrentRow(chosen_);
            show_light();
            view_->update();
            return;
        }
    }

    void drag(QPointF mm) {
        Json* l = chosen();
        if (l == nullptr || grip_ == Grip::None) return;
        const auto [x, y] = share(mm);
        if (grip_ == Grip::Place) {
            (*l)["x"] = std::clamp(x, -10.0, 10.0);
            (*l)["y"] = std::clamp(y, -10.0, 10.0);
        } else if (grip_ == Grip::Target) {
            (*l)["target_x"] = std::clamp(x, -10.0, 10.0);
            (*l)["target_y"] = std::clamp(y, -10.0, 10.0);
        } else {
            const QPointF at = place(num(*l, "x", 0.25), num(*l, "y", 0.2));
            const QPointF d = mm - at;
            if (std::hypot(d.x(), d.y()) > 1e-9) (*l)["angle"] = std::remainder(std::atan2(-d.y(), d.x()) * 180.0 / kPi, 360.0);
            const QSignalBlocker quiet(angle_);
            angle_->setValue(num(*l, "angle", 135));
        }
        view_->update();
        timer_->start();
    }

    void fill_list() {
        const QSignalBlocker quiet(list_);
        list_->clear();
        for (const Json& l : params_["lights"]) list_->addItem(kind_words(core::py_str(core::py_get(l, "kind", Json("spot")))));
        if (chosen_ >= list_->count()) chosen_ = list_->count() - 1;
        if (chosen_ < 0 && list_->count() > 0) chosen_ = 0;
        list_->setCurrentRow(chosen_);
    }

    void add_light(const std::string& kind) {
        Json& all = lights();
        if (all.size() >= core::kLightingMostLights) {
            status_->setText(QStringLiteral("ライトは %1 個までです").arg(core::kLightingMostLights));
            return;
        }
        all.push_back(Json{{"kind", kind}, {"x", 0.5}, {"y", 0.4}, {"z", 0.6}, {"target_x", 0.5}, {"target_y", 0.6}, {"angle", 135}, {"elevation", 45},
                           {"color", Json::array({255, 255, 255})}, {"intensity", 75}, {"cone", 45}, {"hotspot", 50}, {"radius", 0.8}});
        chosen_ = static_cast<int>(all.size()) - 1;
        fill_list();
        show_light();
        view_->update();
        timer_->start();
    }

    void delete_light() {
        Json& all = lights();
        if (chosen_ < 0 || chosen_ >= static_cast<int>(all.size())) return;
        all.erase(all.begin() + chosen_);
        chosen_ = std::min(chosen_, static_cast<int>(all.size()) - 1);
        fill_list();
        show_light();
        view_->update();
        timer_->start();
    }

    // the chosen light's fields (none chosen: off)
    void show_light() {
        const Json* l = chosen();
        const std::string kind = l != nullptr ? core::py_str(core::py_get(*l, "kind", Json("spot"))) : std::string("spot");
        for (QWidget* w : std::initializer_list<QWidget*>{kind_, colour_, intensity_, hotspot_, cone_, z_, radius_, angle_, elevation_}) {
            const QSignalBlocker quiet(w);
            w->setEnabled(l != nullptr);
        }
        if (l == nullptr) return;
        const QSignalBlocker q1(kind_), q2(intensity_), q3(hotspot_), q4(cone_), q5(z_), q6(radius_), q7(angle_), q8(elevation_);
        kind_->setCurrentIndex(kind == "point" ? 1 : kind == "infinite" ? 2 : 0);
        const QColor c = colour_of(*l);
        colour_->setText(c.name());
        colour_->setStyleSheet(QStringLiteral("background-color: %1; color: %2").arg(c.name(), c.lightness() > 128 ? QStringLiteral("black") : QStringLiteral("white")));
        intensity_->setValue(num(*l, "intensity", 75));
        hotspot_->setValue(num(*l, "hotspot", 50));
        cone_->setValue(num(*l, "cone", 45));
        z_->setValue(num(*l, "z", 0.6));
        radius_->setValue(num(*l, "radius", 0.8));
        angle_->setValue(num(*l, "angle", 135));
        elevation_->setValue(num(*l, "elevation", 45));
        hotspot_->setEnabled(kind == "spot");
        cone_->setEnabled(kind == "spot");
        radius_->setEnabled(kind == "point");
        angle_->setEnabled(kind == "infinite");
        elevation_->setEnabled(kind == "infinite");
        z_->setEnabled(kind != "infinite");
    }

    void show_surface() {
        const QSignalBlocker q1(gloss_), q2(metallic_), q3(exposure_), q4(ambience_), q5(texture_), q6(height_), q7(white_);
        gloss_->setValue(num(params_, "gloss", 0));
        metallic_->setValue(num(params_, "metallic", 0));
        exposure_->setValue(num(params_, "exposure", 0));
        ambience_->setValue(num(params_, "ambience", 8));
        height_->setValue(num(params_, "height", 50));
        white_->setChecked(params_.value("white_is_high", true));
        const Json texture = params_.value("texture", Json("none"));
        QString key = texture.is_string() ? QString::fromStdString(texture.get<std::string>()) : QStringLiteral("none");
        if (key == QLatin1String("alpha")) key = QStringLiteral("transparency");
        if (texture.is_object()) {  // (a channel named by its id or name: its row)
            const std::string named = core::py_str(core::py_get(texture, "channel", Json("")));
            for (const core::Channel& channel : call_.page->channels)
                if (channel.id == named || channel.name == named) key = QStringLiteral("channel:%1").arg(QString::fromStdString(channel.id));
        }
        const int row = texture_->findData(key);
        texture_->setCurrentIndex(row >= 0 ? row : 0);
    }

    // The fields into the settings (each change a moment before the preview).
    void wire() {
        connect(list_, &QListWidget::currentRowChanged, this, [this](int row) {
            chosen_ = row;
            show_light();
            view_->update();
        });
        const auto light_number = [this](QDoubleSpinBox* box, const char* key) {
            connect(box, &QDoubleSpinBox::valueChanged, this, [this, key](double v) {
                if (Json* l = chosen()) {
                    (*l)[key] = v;
                    view_->update();
                    timer_->start();
                }
            });
        };
        light_number(intensity_, "intensity");
        light_number(hotspot_, "hotspot");
        light_number(cone_, "cone");
        light_number(z_, "z");
        light_number(radius_, "radius");
        light_number(angle_, "angle");
        light_number(elevation_, "elevation");
        connect(kind_, &QComboBox::currentIndexChanged, this, [this](int) {
            if (Json* l = chosen()) {
                (*l)["kind"] = kind_->currentData().toString().toStdString();
                fill_list();
                show_light();
                view_->update();
                timer_->start();
            }
        });
        connect(colour_, &QPushButton::clicked, this, [this] {
            Json* l = chosen();
            if (l == nullptr) return;
            const QColor now = QColorDialog::getColor(colour_of(*l), this, QStringLiteral("ライトのカラー"));
            if (!now.isValid()) return;
            (*l)["color"] = Json::array({now.red(), now.green(), now.blue()});
            show_light();
            view_->update();
            timer_->start();
        });
        const auto surface_number = [this](QDoubleSpinBox* box, const char* key) {
            connect(box, &QDoubleSpinBox::valueChanged, this, [this, key](double v) {
                params_[key] = v;
                timer_->start();
            });
        };
        surface_number(gloss_, "gloss");
        surface_number(metallic_, "metallic");
        surface_number(exposure_, "exposure");
        surface_number(ambience_, "ambience");
        surface_number(height_, "height");
        connect(white_, &QCheckBox::toggled, this, [this](bool on) {
            params_["white_is_high"] = on;
            timer_->start();
        });
        connect(texture_, &QComboBox::currentIndexChanged, this, [this](int) {
            const QString key = texture_->currentData().toString();
            if (key.startsWith(QStringLiteral("channel:"))) params_["texture"] = Json{{"channel", key.mid(8).toStdString()}};
            else params_["texture"] = key == QLatin1String("transparency") ? std::string("alpha") : key.toStdString();
            timer_->start();
        });
    }

    // The result over the picture, on a thread (the last one asked for wins).
    void run_preview() {
        if (stop_) stop_->request_stop();
        Json params;
        try {
            params = settings();
            status_->setText(QString());
        } catch (const std::exception& error) {
            status_->setText(wording::error(QString::fromUtf8(error.what())));
            return;
        }
        // (a channel of the page as the texture: the one the window keeps, else made with the result on the thread)
        std::string texture_id;
        std::shared_ptr<const render::fx::Picture> texture;
        std::optional<core::Channel> channel;
        if (const Json* given = core::get(params, "texture"); given != nullptr && given->is_object()) {
            texture_id = core::py_str(core::py_get(*given, "channel", Json("")));
            if (const auto kept = textures_.find(texture_id); kept != textures_.end()) {
                texture = kept->second;
            } else {
                for (const core::Channel& c : call_.page->channels)
                    if (c.id == texture_id || c.name == texture_id) channel = c;
            }
        }
        auto stop = std::make_shared<std::stop_source>();
        stop_ = stop;
        setProperty("previewing", true);
        auto* watcher = new QFutureWatcher<Worked>(this);
        connect(watcher, &QFutureWatcher<Worked>::finished, this, [this, watcher, stop, texture_id] {
            const Worked done = watcher->result();
            watcher->deleteLater();
            if (!texture_id.empty() && done.texture) textures_[texture_id] = done.texture;
            if (stop_ != stop || stop->stop_requested()) return;
            setProperty("previewing", false);
            ++previews_;
            setProperty("previews", previews_);
            if (!done.image.isNull()) view_->set_image(done.image, shown_.box_mm);
        });
        watcher->setFuture(QtConcurrent::run([shown = shown_, params, texture, channel, token = stop->get_token()]() -> Worked {
            const render::segment::StopScope scope(token);
            try {
                return worked(shown, params, texture, channel);
            } catch (...) {  // (stopped, refused, past the budget: none)
                return Worked{};
            }
        }));
    }

    FilterWindowCall call_;
    bool smart_ = false;
    Json params_;
    Shown shown_;
    int chosen_ = 0;
    Grip grip_ = Grip::None;
    lens_window::PictureView* view_ = nullptr;
    QLabel* status_ = nullptr;
    QListWidget* list_ = nullptr;
    QComboBox* kind_ = nullptr;
    QPushButton* colour_ = nullptr;
    QDoubleSpinBox *intensity_ = nullptr, *hotspot_ = nullptr, *cone_ = nullptr, *z_ = nullptr, *radius_ = nullptr, *angle_ = nullptr, *elevation_ = nullptr;
    QDoubleSpinBox *gloss_ = nullptr, *metallic_ = nullptr, *exposure_ = nullptr, *ambience_ = nullptr, *height_ = nullptr;
    QComboBox* texture_ = nullptr;
    QCheckBox* white_ = nullptr;
    QTimer* timer_ = nullptr;
    std::shared_ptr<std::stop_source> stop_;
    int previews_ = 0;
    std::map<std::string, std::shared_ptr<const render::fx::Picture>> textures_;  // (the page's channels read as textures)
};

}  // namespace

void open(const FilterWindowCall& call) {
    if (call.window == nullptr || call.page == nullptr || call.layer == nullptr) return;
    LightingDialog dialog(call.window, call);
    if (ask::exec(&dialog) != QDialog::Accepted) return;
    Json params;
    try {
        params = dialog.settings();
    } catch (const std::exception& error) {
        call.window->flash(wording::error(QString::fromUtf8(error.what())), 5000, true);
        return;
    }
    if (call.send) call.send(params);
}

}  // namespace genko::app::lighting_window
