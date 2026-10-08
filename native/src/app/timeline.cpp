#include "app/timeline.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QFileInfo>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <set>

#include "app/ask.hpp"
#include "app/layer_panel.hpp"
#include "app/main_window.hpp"
#include "app/theme.hpp"
#include "app/wording.hpp"
#include "core/anim.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "render/anim.hpp"
#include "render/page.hpp"

namespace genko::app {

namespace {

using core::Json;

QImage to_qimage(const render::Image& image) {
    const render::Image rgb = image.mode() == "RGB" ? image : image.convert("RGB");
    const std::string bytes = rgb.tobytes();
    return QImage(reinterpret_cast<const uchar*>(bytes.data()), rgb.width(), rgb.height(), rgb.width() * 3, QImage::Format_RGB888).copy();
}

QString title_of(const core::Page& page, const Json& id) {
    if (!id.is_string()) return QStringLiteral("?");
    for (const core::Layer& layer : page.layers) {
        if (layer.id == id.get<std::string>()) return QString::fromStdString(layer.title);
    }
    return QStringLiteral("?");
}

}  // namespace

TimelinePanel::TimelinePanel(MainWindow* window) : window_(window) {
    setObjectName(QStringLiteral("timeline"));
    onion = new QCheckBox(QStringLiteral("前後を透かす"));
    onion->setToolTip(QStringLiteral("オニオンスキン: 前のフレームを赤、次を青で薄く出します"));
    onion->setChecked(true);
    connect(onion, &QCheckBox::toggled, this, [this](bool) { set_frame(frame, false); });
    start = new QPushButton(QStringLiteral("このページをアニメーションにする"));
    connect(start, &QPushButton::clicked, this, [this] { start_animation(); });
    play_button = new QPushButton(QStringLiteral("▶ 再生"));
    play_button->setCheckable(true);
    connect(play_button, &QPushButton::toggled, this, [this](bool on) { play(on); });
    fps = new QSpinBox;
    fps->setRange(1, 60);
    fps->setSuffix(QStringLiteral(" fps"));
    fps->setToolTip(QStringLiteral("1 秒に何フレーム進むか"));
    connect(fps, &QSpinBox::editingFinished, this, [this] { set(Json{{"fps", fps->value()}}); });
    frames = new QSpinBox;
    frames->setRange(1, static_cast<int>(core::anim::kMaxFrames));
    frames->setSuffix(QStringLiteral(" フレーム"));
    connect(frames, &QSpinBox::editingFinished, this, [this] { set(Json{{"frames", frames->value()}}); });
    loop = new QCheckBox(QStringLiteral("くり返す"));
    connect(loop, &QCheckBox::toggled, this, [this](bool on) { set(Json{{"loop", on}}); });
    where = new QLabel;
    table = new QTableWidget;
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->horizontalHeader()->setMinimumSectionSize(kCellWidth);
    table->horizontalHeader()->setDefaultSectionSize(kCellWidth);
    table->verticalHeader()->setDefaultSectionSize(22);
    connect(table, &QTableWidget::cellClicked, this, [this](int, int column) { set_frame(column + 1); });
    table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(table, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        const QTableWidgetItem* item = table->itemAt(pos);
        if (item == nullptr) return;
        if (QMenu* menu = cell_menu(item->row(), item->column())) {
            menu->exec(table->viewport()->mapToGlobal(pos));
            menu->deleteLater();
        }
    });
    connect(table, &QTableWidget::cellDoubleClicked, this, [this](int row, int column) {
        if (QMenu* menu = cell_menu(row, column)) {
            menu->exec(table->viewport()->mapToGlobal(table->visualItemRect(table->item(row, column)).center()));
            menu->deleteLater();
        }
    });
    add_folder_button = new QPushButton(QStringLiteral("＋フォルダー"));
    add_folder_button->setToolTip(QStringLiteral("アニメーションフォルダー: セルを入れ、タイムラインの 1 行になります"));
    connect(add_folder_button, &QPushButton::clicked, this, [this] { add_folder(); });
    add_cel_button = new QPushButton(QStringLiteral("＋セル"));
    add_cel_button->setToolTip(QStringLiteral("選んだ行のフォルダーに新しいセルを作り、このフレームから出します"));
    connect(add_cel_button, &QPushButton::clicked, this, [this] { add_cel(); });
    camera = new QPushButton(QStringLiteral("カメラ"));
    camera->setToolTip(QStringLiteral("カメラワーク: 今見えている範囲を、このフレームのカメラにします（フレームの間は動きます）"));
    connect(camera, &QPushButton::clicked, this, [this] { camera_here(); });
    export_button = new QPushButton(QStringLiteral("書き出し…"));
    connect(export_button, &QPushButton::clicked, this, [this] { export_animation(); });
    timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this] { tick(); });

    auto* top = new QGridLayout;
    top->addWidget(play_button, 0, 0);
    top->addWidget(fps, 0, 1);
    top->addWidget(frames, 1, 0);
    top->addWidget(loop, 1, 1);
    top->addWidget(onion, 2, 0);
    top->addWidget(where, 2, 1);
    auto* buttons = new QGridLayout;
    buttons->addWidget(add_folder_button, 0, 0);
    buttons->addWidget(add_cel_button, 0, 1);
    buttons->addWidget(camera, 1, 0);
    buttons->addWidget(export_button, 1, 1);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(start);
    layout->addLayout(top);
    layout->addWidget(table, 1);
    layout->addLayout(buttons);
    refresh();
}

const core::Page* TimelinePanel::page() const { return window_->current_page(); }

std::vector<std::string> TimelinePanel::folder_ids() const {
    std::vector<std::string> out;
    const core::Page* p = page();
    if (p == nullptr) return out;
    try {
        for (const Json& id : core::anim::folders(*p)) out.push_back(id.is_string() ? id.get<std::string>() : core::py_str(id));
    } catch (const std::exception&) {
    }
    return out;
}

std::optional<std::string> TimelinePanel::current_folder() const {
    const auto ids = folder_ids();
    const int row = table->currentRow();
    if (row >= 0 && row < static_cast<int>(ids.size())) return ids[static_cast<std::size_t>(row)];
    if (!ids.empty()) return ids.front();
    return std::nullopt;
}

void TimelinePanel::refresh() {
    const core::Page* p = page();
    const bool on = p != nullptr && core::anim::is_animation(*p);
    start->setVisible(p != nullptr && !on);
    for (QWidget* widget : std::initializer_list<QWidget*>{play_button, fps, frames, loop, onion, where, table, add_folder_button,
                                                          add_cel_button, camera, export_button}) {
        widget->setEnabled(on);
    }
    if (!on) {
        table->clear();
        table->setRowCount(0);
        table->setColumnCount(0);
        where->setText(QString());
        return;
    }
    std::int64_t count = 1;
    try {
        count = core::anim::frames_of(*p);
        frame = std::clamp<std::int64_t>(frame, 1, count);
        for (auto [box, value] : {std::pair{fps, static_cast<int>(core::anim::fps_of(*p))}, std::pair{frames, static_cast<int>(count)}}) {
            box->blockSignals(true);
            box->setValue(value);
            box->blockSignals(false);
        }
        loop->blockSignals(true);
        loop->setChecked(core::anim::loops(*p));
        loop->blockSignals(false);
    } catch (const std::exception&) {
    }
    const std::vector<std::string> ids = folder_ids();
    const int row_now = std::max(0, table->currentRow());
    table->blockSignals(true);
    table->clear();
    table->setRowCount(static_cast<int>(ids.size()));
    table->setColumnCount(static_cast<int>(count));
    QStringList columns;
    for (std::int64_t f = 1; f <= count; ++f) columns << QString::number(f);
    table->setHorizontalHeaderLabels(columns);
    QStringList rows;
    for (const std::string& id : ids) rows << title_of(*p, Json(id));
    table->setVerticalHeaderLabels(rows);
    std::set<std::int64_t> keys;
    const Json* data = core::anim::spec(*p);
    if (data != nullptr) {
        try {
            for (const Json& key : core::iterate(core::py_or(core::py_get(*data, "camera"), Json::array()))) keys.insert(core::to_int(core::py_get(key, "frame")));
        } catch (const std::exception&) {
        }
    }
    const theme::Tokens& colours = theme::tokens();
    for (std::size_t r = 0; r < ids.size(); ++r) {
        const Json folder(ids[r]);
        std::map<std::int64_t, Json> starts;
        try {
            if (const Json* track = core::anim::track(*p, folder)) {
                for (const Json& item : core::iterate(core::py_get(*track, "cels", Json::array()))) {
                    const auto pair = core::unpack_values(item, 2);
                    starts[core::to_int(pair[0])] = pair[1];
                }
            }
        } catch (const std::exception&) {
        }
        for (std::int64_t column = 0; column < count; ++column) {
            const std::int64_t f = column + 1;
            Json cel;
            try {
                cel = core::anim::cel_at(*p, folder, f);
            } catch (const std::exception&) {
            }
            QString text;
            if (starts.contains(f)) {
                text = core::py_truthy(cel) ? title_of(*p, cel) : QStringLiteral("×");
                if (text == QLatin1String("?")) text = QStringLiteral("×");
            } else {
                text = core::py_truthy(cel) ? QStringLiteral("│") : QString();
            }
            auto* item = new QTableWidgetItem(text);
            item->setTextAlignment(Qt::AlignCenter);
            if (f == frame) {
                item->setBackground(QBrush(QColor(colours.accent_soft)));
            } else if (keys.contains(f)) {
                item->setBackground(QBrush(QColor(colours.selected)));
            }
            table->setItem(static_cast<int>(r), static_cast<int>(column), item);
        }
    }
    if (!ids.empty()) table->setCurrentCell(std::min(row_now, static_cast<int>(ids.size()) - 1), static_cast<int>(frame - 1));
    table->blockSignals(false);
    where->setText(QStringLiteral("%1 / %2").arg(frame).arg(count));
}

void TimelinePanel::set_frame(std::int64_t to, bool follow) {
    const core::Page* p = page();
    if (p == nullptr || !core::anim::is_animation(*p)) return;
    frame = std::clamp<std::int64_t>(to, 1, core::anim::frames_of(*p));
    window_->anim_frames[p->id] = frame;
    if (const auto folder = current_folder(); follow && folder) {
        Json cel;
        try {
            cel = core::anim::cel_at(*p, Json(*folder), frame);
        } catch (const std::exception&) {
        }
        const core::Layer* target = window_->target_layer();
        if (cel.is_string() && (target == nullptr || target->id != cel.get<std::string>()) &&
            std::any_of(p->layers.begin(), p->layers.end(), [&](const core::Layer& l) { return l.id == cel.get<std::string>(); })) {
            window_->set_target_layer(cel.get<std::string>());
            if (window_->layer_panel() != nullptr) window_->layer_panel()->refresh();
        }
    }
    refresh();
    window_->canvas()->show_frame(QImage());
    window_->canvas()->renderer().set_anim(frame, onion->isChecked());
}

bool TimelinePanel::ops(const Json& batch) {
    const bool ok = window_->apply_ops(batch);
    refresh();
    if (window_->layer_panel() != nullptr) window_->layer_panel()->refresh();
    return ok;
}

void TimelinePanel::set(const Json& change) {
    const core::Page* p = page();
    if (p == nullptr || !core::anim::is_animation(*p)) return;
    Json op{{"op", "set_animation"}, {"page", p->index.json()}};
    for (const auto& [key, value] : change.items()) op[key] = value;
    ops(Json::array({op}));
}

void TimelinePanel::start_animation() {
    const core::Page* p = page();
    if (p == nullptr) return;
    const std::string folder = core::new_id(), cel = core::new_id();
    const Json index = p->index.json();
    if (ops(Json::array({Json{{"op", "set_animation"}, {"page", index}, {"fps", 12}, {"frames", 24}},
                         Json{{"op", "add_anim_folder"}, {"page", index}, {"id", folder}},
                         Json{{"op", "add_cel"}, {"page", index}, {"folder", folder}, {"id", cel}, {"name", "1"}}}))) {
        window_->set_target_layer(cel);
        set_frame(1);
    }
}

void TimelinePanel::add_folder() {
    const core::Page* p = page();
    if (p != nullptr) ops(Json::array({Json{{"op", "add_anim_folder"}, {"page", p->index.json()}}}));
}

void TimelinePanel::add_cel() {
    const core::Page* p = page();
    const auto folder = current_folder();
    if (p == nullptr || !folder) return;
    const std::string cel = core::new_id();
    if (ops(Json::array({Json{{"op", "add_cel"}, {"page", p->index.json()}, {"folder", *folder}, {"id", cel}, {"at", frame}}}))) {
        window_->set_target_layer(cel);
        set_frame(frame);
    }
}

QMenu* TimelinePanel::cell_menu(int row, int column) {
    const core::Page* p = page();
    const auto ids = folder_ids();
    if (p == nullptr || row < 0 || row >= static_cast<int>(ids.size())) return nullptr;
    const std::string folder = ids[static_cast<std::size_t>(row)];
    const std::int64_t at = column + 1;
    const Json index = p->index.json();
    auto* menu = new QMenu(this);
    for (const core::Layer* layer : core::anim::cels_of(*p, Json(folder))) {
        const std::string id = layer->id;
        menu->addAction(QStringLiteral("セル「%1」を出す").arg(QString::fromStdString(layer->title)), this, [this, index, folder, at, id] {
            ops(Json::array({Json{{"op", "set_exposure"}, {"page", index}, {"folder", folder}, {"frame", at}, {"cel", id}}}));
        });
    }
    menu->addSeparator();
    menu->addAction(QStringLiteral("何も出さない（空セル）"), this, [this, index, folder, at] {
        ops(Json::array({Json{{"op", "set_exposure"}, {"page", index}, {"folder", folder}, {"frame", at}, {"cel", nullptr}}}));
    });
    menu->addAction(QStringLiteral("指定を消す（前のセルを続ける）"), this, [this, index, folder, at] {
        ops(Json::array({Json{{"op", "set_exposure"}, {"page", index}, {"folder", folder}, {"frame", at}, {"clear", true}}}));
    });
    menu->addSeparator();
    menu->addAction(QStringLiteral("このフレームのカメラを消す"), this, [this, index, at] {
        ops(Json::array({Json{{"op", "set_camera_key"}, {"page", index}, {"frame", at}, {"rect", nullptr}}}));
    });
    Json cel;
    try {
        cel = core::anim::cel_at(*p, Json(folder), at);
    } catch (const std::exception&) {
    }
    if (core::py_truthy(cel)) {
        Json table_now = Json::array();
        try {
            if (const Json* data = core::anim::spec(*p)) {
                for (const Json& c : core::iterate(core::py_or(core::py_get(*data, "light_table"), Json::array()))) table_now.push_back(c);
            }
        } catch (const std::exception&) {
        }
        const bool on = std::any_of(table_now.begin(), table_now.end(), [&](const Json& c) { return core::py_equals(c, cel); });
        Json next = Json::array();
        for (const Json& c : table_now) {
            if (!on || !core::py_equals(c, cel)) next.push_back(c);
        }
        if (!on) next.push_back(cel);
        menu->addAction(on ? QStringLiteral("ライトテーブルから外す") : QStringLiteral("ライトテーブルに置く（いつも薄く見る）"), this,
                        [this, index, next] { ops(Json::array({Json{{"op", "set_light_table"}, {"page", index}, {"cels", next}}})); });
    }
    return menu;
}

void TimelinePanel::camera_here() {
    // the part of the page the screen shows now becomes the camera at this frame
    const core::Page* p = page();
    if (p == nullptr) return;
    PageCanvas* canvas = window_->canvas();
    const QPointF a = canvas->to_mm(QPointF(canvas->rect().topLeft()));
    const QPointF b = canvas->to_mm(QPointF(canvas->rect().bottomRight()));
    double x0 = std::max(0.0, a.x()), x1 = std::min(p->spec.width_mm.value(), b.x());
    double y0 = std::max(0.0, a.y()), y1 = std::min(p->spec.height_mm.value(), b.y());
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    ops(Json::array({Json{{"op", "set_camera_key"}, {"page", p->index.json()}, {"frame", frame},
                          {"rect", Json::array({x0, y0, x1 - x0, y1 - y0})}}}));
}

void TimelinePanel::play(bool on) {
    const core::Page* p = page();
    if (!on || p == nullptr || !core::anim::is_animation(*p)) {
        timer->stop();
        play_button->setText(QStringLiteral("▶ 再生"));
        window_->canvas()->show_frame(QImage());
        if (p != nullptr && core::anim::is_animation(*p)) window_->canvas()->renderer().set_anim(frame, onion->isChecked());
        return;
    }
    play_button->setText(QStringLiteral("■ 止める"));
    played_.clear();
    timer->start(static_cast<int>(std::max<std::int64_t>(15, core::py_round_int(1000.0 / core::anim::fps_of(*p)))));
}

void TimelinePanel::tick() {
    const core::Page* p = page();
    if (p == nullptr || !core::anim::is_animation(*p)) {
        play_button->setChecked(false);
        return;
    }
    const std::int64_t count = core::anim::frames_of(*p);
    std::int64_t next = frame + 1;
    if (next > count) {
        if (!core::anim::loops(*p)) {
            play_button->setChecked(false);
            return;
        }
        next = 1;
    }
    frame = next;
    window_->anim_frames[p->id] = next;
    window_->canvas()->show_frame(frame_picture(*p, next));
    where->setText(QStringLiteral("%1 / %2").arg(next).arg(count));
}

QImage TimelinePanel::frame_picture(const core::Page& p, std::int64_t at) {
    if (const auto found = played_.find(at); found != played_.end()) return found->second;
    const int dpi = std::min(100, window_->canvas()->wanted_dpi());
    QImage picture;
    try {
        render::RenderOptions options;
        options.mode = "proof";
        options.at_frame = true;
        options.skip_unported = true;  // (a preview on screen)
        picture = to_qimage(render::render_page(core::anim::at_frame(p, at), dpi, options, &window_->book()).image);
    } catch (const std::exception&) {
    }
    played_[at] = picture;
    return picture;
}

void TimelinePanel::export_animation() {
    const core::Page* p = page();
    if (p == nullptr) return;
    const auto& path = window_->session().path();
    const QString folder = path ? QString::fromStdString(core::path_to_utf8(path->parent_path()))
                                : QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    char name[48];
    std::snprintf(name, sizeof(name), "p%03lld_animation.gif", static_cast<long long>(core::py_int(p->index.json())));
    QString chosen;
    const QString target = ask::save_path_filtered(
        this, QStringLiteral("アニメーションを書き出す"), folder + QLatin1Char('/') + QString::fromLatin1(name),
        QStringLiteral("GIF (*.gif);;WebP (*.webp);;PNG（APNG） (*.png);;MP4 (*.mp4);;連番 PNG（フォルダー） (*)"), &chosen);
    if (target.isEmpty()) return;
    const QString suffix = QFileInfo(target).suffix();
    const std::string fmt = chosen.contains(QStringLiteral("連番")) ? std::string("frames") : (suffix.isEmpty() ? std::string("gif") : suffix.toStdString());
    QApplication::setOverrideCursor(Qt::WaitCursor);
    try {
        render::anim::export_animation(*p, core::path_from_utf8(target.toStdString()), &window_->book(), 100, fmt);
    } catch (const std::exception& error) {
        QApplication::restoreOverrideCursor();
        ask::warning(this, QStringLiteral("Genko"), wording::error(QString::fromUtf8(error.what())));
        return;
    }
    QApplication::restoreOverrideCursor();
    window_->flash(QStringLiteral("アニメーションを書き出しました: %1").arg(target), 5000);
}

}  // namespace genko::app
