// PC4 節 D6 (SPEC.md COLOR-08「フィルター」, design 5.2): フィルター → 消点…'s window (app/vanishing_window.hpp). The planes and
// the op: core/vanishing, render/vanishing (PhotoCraft v0.2.0's vanishing.rs and vp_cmds.rs). The words are Photoshop's
// (日本語版).

#include "app/vanishing_window.hpp"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QRadioButton>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <vector>

#include "app/ask.hpp"
#include "app/lens_window.hpp"
#include "app/main_window.hpp"
#include "core/pyops.hpp"
#include "core/vanishing.hpp"
#include "render/image_ops.hpp"
#include "render/page.hpp"
#include "render/resample.hpp"
#include "render/retouch_dabs.hpp"
#include "render/vanishing.hpp"
#include "service/wording.hpp"

namespace genko::app::vanishing_window {

using core::Json;
using XY = std::array<double, 2>;

namespace {

QImage qimage_of(const render::Image& picture) {
    const render::Image rgba = picture.mode() == "RGBA" ? picture : picture.convert("RGBA");
    const std::string bytes = rgba.tobytes();
    return QImage(reinterpret_cast<const uchar*>(bytes.data()), rgba.width(), rgba.height(), rgba.width() * 4, QImage::Format_RGBA8888).copy();
}

QDoubleSpinBox* spin(QWidget* parent, const char* name, double lo, double hi, double value, int decimals = 0) {
    auto* box = new QDoubleSpinBox(parent);
    box->setObjectName(QString::fromLatin1(name));
    box->setRange(lo, hi);
    box->setDecimals(decimals);
    box->setValue(value);
    return box;
}

class VanishingDialog : public QDialog {
public:
    VanishingDialog(QWidget* parent, const FilterWindowCall& call) : QDialog(parent), call_(call) {
        setObjectName(QStringLiteral("filter_dialog"));
        setProperty("kind", QStringLiteral("vanishing_point"));
        setWindowTitle(QStringLiteral("消点"));
        const core::Page& page = *call.page;
        width_mm_ = page.spec.width_mm.value();
        height_mm_ = page.spec.height_mm.value();
        dpi_ = static_cast<int>(std::clamp(std::floor(720.0 / std::max({width_mm_, height_mm_, 1.0}) * 25.4), 4.0, 200.0));
        try {
            base_ = render::render_page(page, dpi_, render::proof_options(), &call.window->book()).image.convert("RGBA");
        } catch (const std::exception&) {
            base_ = render::Image::create("RGBA", render::Size{render::mm_to_px(width_mm_, dpi_), render::mm_to_px(height_mm_, dpi_)}, render::Ink{255, 255, 255, 255});
        }
        // (the planes the page keeps)
        if (const Json* kept = core::page_vanishing(page)) {
            try {
                const Json normal = core::read_vanishing(*kept);
                planes_ = normal["planes"];
                if (normal.contains("focal_mm")) focal_mm_ = normal["focal_mm"].get<double>();
            } catch (const std::exception&) {
            }
        }
        auto* outer = new QHBoxLayout(this);
        auto* left = new QVBoxLayout;
        view_ = new lens_window::PictureView(this);
        view_->setObjectName(QStringLiteral("vanishing_view"));
        left->addWidget(view_, 1);
        status_ = new QLabel(this);
        status_->setObjectName(QStringLiteral("vanishing_status"));
        status_->setWordWrap(true);
        left->addWidget(status_);
        outer->addLayout(left, 1);
        auto* right = new QVBoxLayout;
        outer->addLayout(right);
        // the tools
        tools_ = new QButtonGroup(this);
        const char* names[] = {"vanishing_create", "vanishing_edit", "vanishing_stamp", "vanishing_paste"};
        const QString words[] = {QStringLiteral("面作成ツール"), QStringLiteral("面修正ツール"), QStringLiteral("スタンプツール"), QStringLiteral("貼り付けた絵を動かす")};
        for (int i = 0; i < 4; ++i) {
            auto* button = new QRadioButton(words[i], this);
            button->setObjectName(QString::fromLatin1(names[i]));
            tools_->addButton(button, i);
            right->addWidget(button);
        }
        tools_->button(planes_.empty() ? 0 : 1)->setChecked(true);
        // 面: tear one off the chosen plane's edge
        auto* tear = new QFormLayout;
        edge_ = new QComboBox(this);
        edge_->setObjectName(QStringLiteral("vanishing_edge"));
        edge_->addItem(QStringLiteral("上"), QStringLiteral("top"));
        edge_->addItem(QStringLiteral("右"), QStringLiteral("right"));
        edge_->addItem(QStringLiteral("下"), QStringLiteral("bottom"));
        edge_->addItem(QStringLiteral("左"), QStringLiteral("left"));
        tear->addRow(QStringLiteral("継ぎ足す辺"), edge_);
        angle_ = spin(this, "vanishing_angle", -360, 360, 90, 1);
        angle_->setSuffix(QStringLiteral("°"));
        tear->addRow(QStringLiteral("角度"), angle_);
        depth_ = spin(this, "vanishing_depth", 0.05, 20, 1, 2);
        tear->addRow(QStringLiteral("奥行き"), depth_);
        right->addLayout(tear);
        auto* tear_button = new QPushButton(QStringLiteral("面を継ぎ足す"), this);
        tear_button->setObjectName(QStringLiteral("vanishing_tear"));
        right->addWidget(tear_button);
        auto* remove = new QPushButton(QStringLiteral("面を削除"), this);
        remove->setObjectName(QStringLiteral("vanishing_remove"));
        right->addWidget(remove);
        // スタンプ
        auto* stamp = new QFormLayout;
        size_ = spin(this, "vanishing_size", 1, 2000, 40);
        size_->setSuffix(QStringLiteral(" px"));
        stamp->addRow(QStringLiteral("直径"), size_);
        hardness_ = spin(this, "vanishing_hardness", 0, 100, 50);
        stamp->addRow(QStringLiteral("硬さ"), hardness_);
        opacity_ = spin(this, "vanishing_opacity", 0, 100, 100);
        opacity_->setSuffix(QStringLiteral(" %"));
        stamp->addRow(QStringLiteral("不透明度"), opacity_);
        right->addLayout(stamp);
        // 貼り付け
        source_ = new QComboBox(this);
        source_->setObjectName(QStringLiteral("vanishing_source"));
        for (auto it = page.layers.rbegin(); it != page.layers.rend(); ++it) {
            if (it->kind == core::LayerKind::Folder || it->kind == core::LayerKind::Adjust) continue;
            source_->addItem(wording::layer_label(*it), QString::fromStdString(it->id));
        }
        right->addWidget(new QLabel(QStringLiteral("貼り付けるレイヤー"), this));
        right->addWidget(source_);
        auto* paste = new QPushButton(QStringLiteral("面に貼り付け"), this);
        paste->setObjectName(QStringLiteral("vanishing_paste_button"));
        right->addWidget(paste);
        new_layer_ = new QCheckBox(QStringLiteral("新しいレイヤーに描く"), this);
        new_layer_->setObjectName(QStringLiteral("vanishing_new_layer"));
        right->addWidget(new_layer_);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("OK"));
        buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("キャンセル"));
        ok_ = buttons->button(QDialogButtonBox::Ok);
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        right->addStretch(1);
        right->addWidget(buttons);
        timer_ = new QTimer(this);
        timer_->setSingleShot(true);
        timer_->setInterval(120);
        connect(timer_, &QTimer::timeout, this, [this] { preview(); });
        connect(tear_button, &QPushButton::clicked, this, [this] { tear_off(); });
        connect(remove, &QPushButton::clicked, this, [this] {
            if (planes_.empty()) return;
            const std::size_t last = planes_.size() - 1;
            planes_.erase(planes_.begin() + static_cast<std::ptrdiff_t>(last));
            Json kept = Json::array();
            for (const Json& p : pastes_)
                if (p["plane"].get<std::size_t>() < last) kept.push_back(p);
            pastes_ = std::move(kept);
            chosen_ = planes_.empty() ? -1 : static_cast<int>(planes_.size()) - 1;
            changed();
        });
        connect(paste, &QPushButton::clicked, this, [this] {
            if (planes_.empty() || source_->currentIndex() < 0) return;
            const int plane = chosen_ >= 0 ? chosen_ : 0;
            pastes_.push_back(Json{{"plane", plane}, {"layer", source_->currentData().toString().toStdString()}, {"at", Json::array({0.25, 0.25})}, {"width", 0.5}});
            tools_->button(3)->setChecked(true);
            changed();
        });
        view_->press = [this](XY at, Qt::KeyboardModifiers keys, Qt::MouseButton) { pressed(at, keys); };
        view_->drag = [this](XY at, Qt::KeyboardModifiers) { dragged(at); };
        view_->release = [this](XY at, Qt::KeyboardModifiers) { released(at); };
        view_->overlay = [this](QPainter& painter) { draw(painter); };
        chosen_ = planes_.empty() ? -1 : 0;
        changed();
    }

    // The op's settings (filter_menu adds the page, the layer and the selection).
    Json settings() const {
        Json out{{"planes", planes_}, {"keep_planes", true}, {"new_layer", new_layer_->isChecked()}};
        if (focal_mm_) out["focal_mm"] = *focal_mm_;
        if (!pastes_.empty()) out["paste"] = pastes_;
        if (!strokes_.empty()) out["clone"] = strokes_;
        return out;
    }

private:
    // The scene of the planes now (none: they cannot be — said under the picture).
    std::optional<render::vanishing::Scene> scene() const {
        if (planes_.empty()) return std::nullopt;
        try {
            Json given{{"planes", planes_}};
            if (focal_mm_) given["focal_mm"] = *focal_mm_;
            return render::vanishing::scene_of(core::read_vanishing(given), width_mm_, height_mm_);
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }

    void changed() {
        scene_ = scene();
        if (!planes_.empty() && !scene_) {
            try {
                Json given{{"planes", planes_}};
                (void)render::vanishing::scene_of(core::read_vanishing(given), width_mm_, height_mm_);
            } catch (const std::exception& error) {
                status_->setText(wording::error(QString::fromUtf8(error.what())));
            }
        } else if (planes_.empty()) {
            status_->setText(QStringLiteral("面作成ツールで、床や壁の四隅を順にクリックして面を作ります"));
        } else {
            status_->setText(QStringLiteral("面 %1 つ・貼り付け %2・スタンプ %3").arg(planes_.size()).arg(pastes_.size()).arg(strokes_.size()));
        }
        ok_->setEnabled(scene_.has_value());
        setProperty("planes", static_cast<int>(planes_.size()));
        timer_->start();
        view_->update();
    }

    void tear_off() {
        if (planes_.empty() || chosen_ < 0) return;
        if (planes_.size() >= core::kVanishingMostPlanes) return;
        planes_.push_back(Json{{"from", chosen_}, {"edge", edge_->currentData().toString().toStdString()}, {"angle", angle_->value()}, {"depth", depth_->value()}});
        chosen_ = static_cast<int>(planes_.size()) - 1;
        changed();
    }

    // The plane's corners now (a torn-off plane's from the scene).
    std::optional<render::vanishing::Quad> corners(std::size_t i) const {
        if (!scene_ || i >= scene_->planes.size()) return std::nullopt;
        return scene_->planes[i];
    }

    double near_mm() const { return 8.0 / std::max(view_->scale(), 1e-6); }

    void pressed(const XY& at, Qt::KeyboardModifiers keys) {
        const int tool = tools_->checkedId();
        if (tool == 0) {  // 面作成ツール: the corners clicked, four make a plane
            if (planes_.size() >= core::kVanishingMostPlanes) return;
            clicks_.push_back(at);
            if (clicks_.size() == 4) {
                Json quad = Json::array();
                for (const XY& p : clicks_) quad.push_back(Json::array({p[0], p[1]}));
                clicks_.clear();
                planes_.push_back(Json{{"corners_mm", quad}});
                chosen_ = static_cast<int>(planes_.size()) - 1;
                tools_->button(1)->setChecked(true);
                changed();
            }
            view_->update();
            return;
        }
        if (tool == 1) {  // 面修正ツール: a corner of a plane of its own corners dragged, else a plane chosen
            for (std::size_t i = 0; i < planes_.size(); ++i) {
                if (!planes_[i].contains("corners_mm")) continue;
                for (std::size_t k = 0; k < 4; ++k) {
                    const Json& p = planes_[i]["corners_mm"][k];
                    if (std::hypot(p[0].get<double>() - at[0], p[1].get<double>() - at[1]) <= near_mm()) {
                        moving_ = std::make_pair(i, k);
                        chosen_ = static_cast<int>(i);
                        return;
                    }
                }
            }
            if (scene_) {
                if (const auto i = scene_->plane_at(at)) chosen_ = static_cast<int>(*i);
            }
            view_->update();
            return;
        }
        if (tool == 2) {  // スタンプツール: Alt-click where it copies from, a drag a stroke
            if (keys.testFlag(Qt::AltModifier)) {
                stamp_source_ = at;
                view_->update();
                return;
            }
            if (!stamp_source_) {
                status_->setText(QStringLiteral("Alt キーを押しながらクリックして、コピーする元を決めます"));
                return;
            }
            stroke_ = std::vector<XY>{at};
            return;
        }
        if (tool == 3 && !pastes_.empty() && scene_) {  // the last paste moved by the drag (its place on its plane)
            moving_paste_ = at;
        }
    }

    void dragged(const XY& at) {
        if (moving_) {
            planes_[moving_->first]["corners_mm"][moving_->second] = Json::array({at[0], at[1]});
            scene_ = scene();
            view_->update();
            return;
        }
        if (stroke_) {
            stroke_->push_back(at);
            view_->update();
            return;
        }
        if (moving_paste_ && scene_) {
            Json& p = pastes_.back();
            const std::size_t plane = p["plane"].get<std::size_t>();
            const auto h = render::vanishing::homography(scene_->planes[plane]);
            const auto inv = h ? h->inverse() : std::nullopt;
            if (!inv) return;
            const auto [u0, v0] = inv->apply((*moving_paste_)[0], (*moving_paste_)[1]);
            const auto [u1, v1] = inv->apply(at[0], at[1]);
            p["at"] = Json::array({std::clamp(p["at"][0].get<double>() + u1 - u0, -10.0, 10.0), std::clamp(p["at"][1].get<double>() + v1 - v0, -10.0, 10.0)});
            moving_paste_ = at;
            timer_->start();
        }
    }

    void released(const XY& at) {
        if (moving_) {
            moving_.reset();
            changed();
            return;
        }
        if (stroke_ && stamp_source_) {
            stroke_->push_back(at);
            Json points = Json::array();
            for (const XY& p : *stroke_) points.push_back(Json::array({p[0], p[1]}));
            strokes_.push_back(Json{{"source", Json::array({(*stamp_source_)[0], (*stamp_source_)[1]})},
                                    {"points", points},
                                    {"size_px", size_->value()},
                                    {"hardness", hardness_->value()},
                                    {"opacity", opacity_->value()}});
            // (the next stroke copies on from where this one's end copied, as the aligned stamp does)
            const XY first = stroke_->front();
            stamp_source_ = XY{(*stamp_source_)[0] + at[0] - first[0], (*stamp_source_)[1] + at[1] - first[1]};
            stroke_.reset();
            changed();
            return;
        }
        stroke_.reset();
        if (moving_paste_) {
            moving_paste_.reset();
            changed();
        }
    }

    void draw(QPainter& painter) {
        // the planes and their grids
        if (scene_) {
            for (std::size_t i = 0; i < scene_->planes.size(); ++i) {
                const auto h = render::vanishing::homography(scene_->planes[i]);
                if (!h) continue;
                const bool chosen = static_cast<int>(i) == chosen_;
                painter.setPen(QPen(chosen ? QColor(40, 120, 255) : QColor(40, 200, 120), 1));
                const int n = 8;
                for (int k = 0; k <= n; ++k) {
                    const double t = static_cast<double>(k) / n;
                    const auto a = h->apply(t, 0.0), b = h->apply(t, 1.0), c = h->apply(0.0, t), d = h->apply(1.0, t);
                    painter.drawLine(view_->to_widget(a.first, a.second), view_->to_widget(b.first, b.second));
                    painter.drawLine(view_->to_widget(c.first, c.second), view_->to_widget(d.first, d.second));
                }
                painter.setPen(QPen(chosen ? QColor(40, 120, 255) : QColor(40, 200, 120), 2));
                for (const XY& p : scene_->planes[i]) painter.drawRect(QRectF(view_->to_widget(p[0], p[1]) - QPointF(3, 3), QSizeF(6, 6)));
            }
        }
        // the corners clicked so far
        painter.setPen(QPen(QColor(255, 120, 0), 2));
        for (std::size_t i = 0; i < clicks_.size(); ++i) {
            const QPointF p = view_->to_widget(clicks_[i][0], clicks_[i][1]);
            painter.drawEllipse(p, 4, 4);
            if (i > 0) painter.drawLine(view_->to_widget(clicks_[i - 1][0], clicks_[i - 1][1]), p);
        }
        if (stamp_source_) {
            const QPointF p = view_->to_widget((*stamp_source_)[0], (*stamp_source_)[1]);
            painter.setPen(QPen(QColor(255, 0, 0), 1));
            painter.drawLine(p - QPointF(6, 0), p + QPointF(6, 0));
            painter.drawLine(p - QPointF(0, 6), p + QPointF(0, 6));
        }
        if (stroke_) {
            painter.setPen(QPen(QColor(255, 0, 0, 160), 2));
            for (std::size_t i = 1; i < stroke_->size(); ++i)
                painter.drawLine(view_->to_widget((*stroke_)[i - 1][0], (*stroke_)[i - 1][1]), view_->to_widget((*stroke_)[i][0], (*stroke_)[i][1]));
        }
    }

    // What the pastes and strokes make, on the page's picture (render/vanishing on it).
    void preview() {
        const std::array<double, 4> box{0, 0, width_mm_, height_mm_};
        if (!scene_ || (pastes_.empty() && strokes_.empty())) {
            view_->set_image(qimage_of(base_), box);
            return;
        }
        try {
            const render::ImageAllocationBudget budget{1ULL << 30};
            render::image_ops::Pixels shown = render::image_ops::from_image(base_);
            const double per_mm = dpi_ / 25.4;
            const render::Box grid{0, 0, base_.width(), base_.height()};
            const core::Document& doc = call_.window->book();
            for (const Json& p : pastes_) {
                const std::string id = p["layer"].get<std::string>();
                const core::Layer* layer = nullptr;
                for (const core::Layer& l : call_.page->layers)
                    if (l.id == id) layer = &l;
                if (layer == nullptr) continue;
                const render::image_ops::Pixels source = render::image_ops::shown_layer_pixels(doc, *call_.page, *layer);
                const std::vector<render::image_ops::Rgba> rgba = render::image_ops::rgba_of(source);
                int x0 = static_cast<int>(source.width()), y0 = static_cast<int>(source.height()), x1 = 0, y1 = 0;
                for (std::uint32_t y = 0; y < source.height(); ++y)
                    for (std::uint32_t x = 0; x < source.width(); ++x) {
                        if (!(rgba[std::size_t(y) * source.width() + x][3] > 0.0f)) continue;
                        x0 = std::min(x0, int(x));
                        y0 = std::min(y0, int(y));
                        x1 = std::max(x1, int(x) + 1);
                        y1 = std::max(y1, int(y) + 1);
                    }
                if (x1 <= x0 || y1 <= y0) continue;
                render::resample::Plane picture(x1 - x0, y1 - y0, 4, true);
                for (int y = y0; y < y1; ++y)
                    for (int x = x0; x < x1; ++x) {
                        const auto& q = rgba[std::size_t(y) * source.width() + std::size_t(x)];
                        std::copy(q.begin(), q.end(), picture.at(x - x0, y - y0));
                    }
                const std::size_t plane = p["plane"].get<std::size_t>();
                const render::Box out = render::vanishing::paste_box(*scene_, plane, per_mm, grid);
                const auto made = render::vanishing::pasted(*scene_, plane, picture, {p["at"][0].get<double>(), p["at"][1].get<double>()},
                                                            p["width"].get<double>(), per_mm, out);
                if (!made) continue;
                for (int y = out.y0; y < out.y1; ++y)
                    for (int x = out.x0; x < out.x1; ++x)
                        render::retouch::over(shown.at(std::size_t(y) * shown.width() + std::size_t(x)), made->at(x - out.x0, y - out.y0), 3, 1.0f, false);
            }
            for (const Json& s : strokes_) {
                std::vector<XY> points;
                for (const Json& q : s["points"]) points.push_back({q[0].get<double>(), q[1].get<double>()});
                const XY source{s["source"][0].get<double>(), s["source"][1].get<double>()};
                const render::vanishing::Stamp st = render::vanishing::stamp_coverage(*scene_, source, points, s["size_px"].get<double>(),
                                                                                      s["hardness"].get<double>() / 100.0, per_mm, grid);
                if (st.dabs == 0) continue;
                const render::image_ops::Pixels before = shown;
                render::image_ops::Pixels paint(render::image_ops::Pixels::Kind::Eight, shown.width(), shown.height(), 3);
                for (int y = st.box.y0; y < st.box.y1; ++y)
                    for (int x = st.box.x0; x < st.box.x1; ++x) {
                        if (!(st.cov[std::size_t(y - st.box.y0) * std::size_t(st.box.width()) + std::size_t(x - st.box.x0)] > 0.0f)) continue;
                        const auto q = render::vanishing::stamp_source(*scene_, st, {x + 0.5, y + 0.5}, per_mm);
                        if (!q) continue;
                        const int sx = static_cast<int>(std::floor((*q)[0])), sy = static_cast<int>(std::floor((*q)[1]));
                        if (sx < 0 || sy < 0 || sx >= grid.x1 || sy >= grid.y1) continue;
                        std::copy_n(before.at(std::size_t(sy) * before.width() + std::size_t(sx)), 4, paint.at(std::size_t(y) * paint.width() + std::size_t(x)));
                    }
                render::retouch::Coverage cov;
                cov.box = st.box;
                cov.cov = st.cov;
                render::retouch::apply_coverage(shown, grid, cov, static_cast<float>(s["opacity"].get<double>() / 100.0), nullptr, false, paint, "normal");
            }
            view_->set_image(qimage_of(render::image_ops::to_image(shown)), box);
        } catch (const std::exception&) {
            view_->set_image(qimage_of(base_), box);
        }
        ++previews_;
        setProperty("previews", previews_);
    }

    FilterWindowCall call_;
    double width_mm_ = 1, height_mm_ = 1;
    int dpi_ = 72;
    render::Image base_;
    Json planes_ = Json::array();
    Json pastes_ = Json::array();
    Json strokes_ = Json::array();
    std::optional<double> focal_mm_;
    std::optional<render::vanishing::Scene> scene_;
    int chosen_ = -1;
    std::vector<XY> clicks_;
    std::optional<std::pair<std::size_t, std::size_t>> moving_;
    std::optional<XY> moving_paste_;
    std::optional<XY> stamp_source_;
    std::optional<std::vector<XY>> stroke_;
    lens_window::PictureView* view_ = nullptr;
    QLabel* status_ = nullptr;
    QButtonGroup* tools_ = nullptr;
    QComboBox *edge_ = nullptr, *source_ = nullptr;
    QDoubleSpinBox *angle_ = nullptr, *depth_ = nullptr, *size_ = nullptr, *hardness_ = nullptr, *opacity_ = nullptr;
    QCheckBox* new_layer_ = nullptr;
    QPushButton* ok_ = nullptr;
    QTimer* timer_ = nullptr;
    int previews_ = 0;
};

}  // namespace

void open(const FilterWindowCall& call) {
    if (call.window == nullptr || call.page == nullptr || call.layer == nullptr) return;
    VanishingDialog dialog(call.window, call);
    if (ask::exec(&dialog) != QDialog::Accepted) return;
    if (call.send) call.send(dialog.settings());
}

}  // namespace genko::app::vanishing_window
