#include "app/timeline.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QProgressDialog>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QToolButton>
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
#include "service/wording.hpp"
#include "core/anim.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "core/video.hpp"
#include "formats/exporting.hpp"
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

// The frame shown, of how many, and the time it starts at (PC5 J-e1, timeline.info's time): "5 / 24（0.33 秒）".
QString frame_text(std::int64_t frame, std::int64_t count, double fps) {
    QString text = QStringLiteral("%1 / %2").arg(frame).arg(count);
    if (fps > 0) text += QStringLiteral("（%1 秒）").arg(QString::number(static_cast<double>(frame - 1) / fps, 'f', 2));
    return text;
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
    onion = new QCheckBox(QStringLiteral("オニオンスキン"));  // (PC5 J-0, 節 J 1.2: Photoshop's name)
    onion->setToolTip(QStringLiteral("オニオンスキン: 前のフレームを赤、次を青で薄く出します"));
    onion->setChecked(true);
    connect(onion, &QCheckBox::toggled, this, [this](bool) { set_frame(frame, false); });
    start = new QPushButton(QStringLiteral("ビデオタイムラインを作成"));  // (PC5 J-0: Photoshop's name)
    start->setToolTip(QStringLiteral("このページをアニメーション（ビデオタイムライン）にします"));
    connect(start, &QPushButton::clicked, this, [this] { start_animation(); });
    play_button = new QPushButton(QStringLiteral("▶ 再生"));
    play_button->setCheckable(true);
    connect(play_button, &QPushButton::toggled, this, [this](bool on) { play(on); });
    fps = new QDoubleSpinBox;  // (PC5 J-e1, timeline@1: 0.5..120, 23.976 and 29.97 too)
    fps->setRange(core::kTimelineFpsMin, core::kTimelineFpsMax);
    fps->setDecimals(3);
    fps->setSuffix(QStringLiteral(" fps"));
    fps->setToolTip(QStringLiteral("1 秒に何フレーム進むか（0.5〜120。1〜60 の外の速さは、この版より前の Genko では読み取り専用で開きます）"));
    connect(fps, &QDoubleSpinBox::editingFinished, this, [this] {
        const core::Page* p = page();
        // (as shown — the page's, or a value past 0.5..120 an anim before it held, put in the box's range: never written for that)
        if (p == nullptr || !core::anim::is_animation(*p) || fps->value() == fps_shown_) return;
        set(Json{{"fps", fps->value()}});
    });
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
    export_button = new QPushButton(QStringLiteral("ビデオをレンダリング…"));  // (PC5 J-0: Photoshop's name)
    connect(export_button, &QPushButton::clicked, this, [this] { export_animation(); });
    timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this] { tick(); });
    // (PC5 J-e1) the frame before and after — the window's commands, put in by build_menu —, the work area, the menu
    previous_button = new QToolButton;
    next_button = new QToolButton;
    work = new QCheckBox(QStringLiteral("作業範囲"));
    work->setToolTip(QStringLiteral("再生とビデオのレンダリングを、このフレームの範囲だけにします"));
    work_first = new QSpinBox;
    work_last = new QSpinBox;
    for (QSpinBox* box : {work_first, work_last}) {
        box->setRange(1, static_cast<int>(core::anim::kMaxFrames));
        connect(box, &QSpinBox::editingFinished, this, [this] {
            if (!work->isChecked()) return;
            const int a = work_first->value(), b = work_last->value();
            set_work(std::array<std::int64_t, 2>{std::min(a, b), std::max(a, b)});
        });
    }
    work_first->setToolTip(QStringLiteral("作業範囲の開始点"));
    work_last->setToolTip(QStringLiteral("作業範囲の終了点"));
    connect(work, &QCheckBox::toggled, this, [this](bool on) {
        if (!on) {
            set_work(std::nullopt);
            return;
        }
        const int a = work_first->value(), b = work_last->value();
        set_work(std::array<std::int64_t, 2>{std::min(a, b), std::max(a, b)});
    });
    panel_menu = new QMenu(this);
    menu_button = new QToolButton;
    menu_button->setText(QStringLiteral("≡"));
    menu_button->setToolTip(QStringLiteral("タイムラインのメニュー"));
    menu_button->setPopupMode(QToolButton::InstantPopup);
    menu_button->setMenu(panel_menu);

    auto* steps = new QHBoxLayout;
    steps->addWidget(previous_button);
    steps->addWidget(next_button);
    steps->addWidget(where, 1);
    steps->addWidget(menu_button);
    auto* work_row = new QHBoxLayout;
    work_row->addWidget(work);
    work_row->addWidget(work_first);
    work_row->addWidget(new QLabel(QStringLiteral("〜")));
    work_row->addWidget(work_last);
    auto* top = new QGridLayout;
    top->addWidget(play_button, 0, 0);
    top->addWidget(fps, 0, 1);
    top->addWidget(frames, 1, 0);
    top->addWidget(loop, 1, 1);
    top->addWidget(onion, 2, 0);
    top->addLayout(steps, 2, 1);
    top->addLayout(work_row, 3, 0, 1, 2);
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
                                                          add_cel_button, camera, export_button, previous_button, next_button, work}) {
        widget->setEnabled(on);
    }
    work_first->setEnabled(on && work->isChecked());
    work_last->setEnabled(on && work->isChecked());
    if (!on) {
        table->clear();
        table->setRowCount(0);
        table->setColumnCount(0);
        where->setText(QString());
        return;
    }
    std::int64_t count = 1;
    std::optional<std::array<std::int64_t, 2>> work_now;
    double speed = 12;
    try {
        count = core::anim::frames_of(*p);
        frame = std::clamp<std::int64_t>(frame, 1, count);
        speed = core::anim::fps_of(*p);
        {
            const QSignalBlocker quiet(fps);
            fps->setValue(speed);
            fps_shown_ = fps->value();
        }
        {
            const QSignalBlocker quiet(frames);
            frames->setValue(static_cast<int>(count));
        }
        loop->blockSignals(true);
        loop->setChecked(core::anim::loops(*p));
        loop->blockSignals(false);
        work_now = core::work_area(*p);
    } catch (const std::exception&) {
    }
    {  // (PC5 J-e1: the work area — its frames kept in the boxes while it is off, to turn it on again)
        const QSignalBlocker quiet_work(work), quiet_first(work_first), quiet_last(work_last);
        work->setChecked(work_now.has_value());
        for (QSpinBox* box : {work_first, work_last}) box->setMaximum(static_cast<int>(count));
        if (work_now) {
            work_first->setValue(static_cast<int>((*work_now)[0]));
            work_last->setValue(static_cast<int>((*work_now)[1]));
        } else if (work_last->value() <= 1 || work_last->value() > count) {
            work_first->setValue(1);
            work_last->setValue(static_cast<int>(count));
        }
        work_first->setEnabled(work_now.has_value());
        work_last->setEnabled(work_now.has_value());
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
    const theme::Tokens& tones = theme::tokens();
    if (work_now) {  // (PC5 J-e1: the frames outside the work area faint)
        for (std::int64_t f = 1; f <= count; ++f) {
            if (f >= (*work_now)[0] && f <= (*work_now)[1]) continue;
            if (QTableWidgetItem* head = table->horizontalHeaderItem(static_cast<int>(f - 1))) head->setForeground(QBrush(QColor(tones.faint)));
        }
    }
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
    where->setText(frame_text(frame, count, speed));
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
    // (PC5 J-e1: from the work area's first frame when the frame shown is outside it or at its end — Photoshop's)
    try {
        const auto [first, last] = core::played_frames(*p);
        if (frame < first || frame >= last) set_frame(first, false);
    } catch (const std::exception&) {
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
    const auto [first, last] = core::played_frames(*p);  // (PC5 J-e1: the work area's frames, else all of them)
    std::int64_t next = frame + 1;
    if (next > last || next < first) {
        if (!core::anim::loops(*p) && next > last) {
            play_button->setChecked(false);
            return;
        }
        next = first;
    }
    frame = next;
    window_->anim_frames[p->id] = next;
    window_->canvas()->show_frame(frame_picture(*p, next));
    where->setText(frame_text(next, count, core::anim::fps_of(*p)));
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
    if (!core::anim::is_animation(*p)) {
        window_->flash(QStringLiteral("このページはアニメーションではありません（タイムラインの「ビデオタイムラインを作成」から）"), 5000);
        return;
    }
    // (PC5 J-e1: ビデオをレンダリング's window first — the frames, the size, the speed —, then where and in which format)
    RenderVideoDialog asked(this, *p);
    if (ask::exec(&asked) != QDialog::Accepted) return;
    const auto range = asked.frames();
    const auto width = asked.width_px();
    const auto speed = asked.speed();
    const auto& path = window_->session().path();
    const QString folder = path ? QString::fromStdString(core::path_to_utf8(path->parent_path()))
                                : QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    char name[48];
    std::snprintf(name, sizeof(name), "p%03lld_animation.gif", static_cast<long long>(core::py_int(p->index.json())));
    QString chosen;
    const QString target = ask::save_path_filtered(
        this, QStringLiteral("ビデオをレンダリング"), folder + QLatin1Char('/') + QString::fromLatin1(name),
        QStringLiteral("GIF (*.gif);;WebP (*.webp);;PNG（APNG） (*.png);;MP4 (*.mp4);;連番 PNG（フォルダー） (*)"), &chosen);
    if (target.isEmpty()) return;
    const QString suffix = QFileInfo(target).suffix();
    const std::string fmt = chosen.contains(QStringLiteral("連番")) ? std::string("frames") : (suffix.isEmpty() ? std::string("gif") : suffix.toStdString());
    // (a width asked for: drawn at the resolution that gives it — 100 to 600 dpi — then made that wide, not a 100 dpi
    // picture blown up; the book and the page as they are now, whatever comes in while the frames are drawn)
    const core::Document book = window_->book();
    const core::Page shown = *p;
    int dpi = 100;
    if (width) {
        double across_mm = shown.trim_rect_mm().width.value();
        try {
            if (const auto rect = core::anim::camera_at(shown, range[0]); rect && rect->size() >= 4 && (*rect)[2] > 0) across_mm = (*rect)[2];
        } catch (const std::exception&) {
        }
        const double at_100 = across_mm / 25.4 * 100.0;
        if (at_100 > 0) dpi = static_cast<int>(std::clamp(std::ceil(100.0 * *width / at_100), 100.0, 600.0));
    }
    QProgressDialog progress(QStringLiteral("ビデオをレンダリングしています…"), QStringLiteral("キャンセル"), 0,
                             static_cast<int>(range[1] - range[0] + 1), this);
    progress.setWindowTitle(QStringLiteral("ビデオをレンダリング"));
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(500);
    const auto told = [&progress](std::int64_t done, std::int64_t) {
        progress.setValue(static_cast<int>(done));
        QCoreApplication::processEvents();
        return !progress.wasCanceled();
    };
    QApplication::setOverrideCursor(Qt::WaitCursor);
    try {
        render::anim::export_animation(shown, core::path_from_utf8(target.toStdString()), &book, dpi, fmt, true, "trim", width, range, speed,
                                       told);
    } catch (const core::Error& error) {
        QApplication::restoreOverrideCursor();
        progress.reset();
        if (std::string_view(error.code()) == "cancelled") {
            window_->flash(QStringLiteral("ビデオのレンダリングをやめました（何も書き出していません）"), 5000);
            return;
        }
        ask::warning(this, QStringLiteral("Genko"), wording::error(QString::fromUtf8(error.what())));
        return;
    } catch (const std::exception& error) {
        QApplication::restoreOverrideCursor();
        progress.reset();
        ask::warning(this, QStringLiteral("Genko"), wording::error(QString::fromUtf8(error.what())));
        return;
    }
    QApplication::restoreOverrideCursor();
    progress.reset();
    QString said = QStringLiteral("アニメーションを書き出しました: %1").arg(target);
    if (const auto note = formats::ink_note(shown)) said += QStringLiteral("（") + QString::fromStdString(*note) + QStringLiteral("）");  // (color-mode@1)
    window_->flash(said, 5000);
}

// --- PC5 J-e1: moving the frame, the work area, the panel's menu ----------------------------------------------------

void TimelinePanel::go(const QString& where_to) {
    const core::Page* p = page();
    if (p == nullptr || !core::anim::is_animation(*p)) {
        window_->flash(QStringLiteral("このページはアニメーションではありません（タイムラインの「ビデオタイムラインを作成」から）"), 4000);
        return;
    }
    if (play_button->isChecked()) play_button->setChecked(false);  // (stepping stops the playing)
    std::int64_t count = 1;
    std::array<std::int64_t, 2> played{1, 1};
    try {
        count = core::anim::frames_of(*p);
        played = core::played_frames(*p);
    } catch (const std::exception&) {
    }
    const std::int64_t now = std::clamp<std::int64_t>(window_->current_frame(p), 1, count);
    std::int64_t to = now;
    if (where_to == QLatin1String("next")) {
        to = now + 1;
    } else if (where_to == QLatin1String("previous")) {
        to = now - 1;
    } else if (where_to == QLatin1String("first")) {
        to = 1;
    } else if (where_to == QLatin1String("last")) {
        to = count;
    } else if (where_to == QLatin1String("work_start")) {
        to = played[0];
    } else if (where_to == QLatin1String("work_end")) {
        to = played[1];
    }
    set_frame(std::clamp<std::int64_t>(to, 1, count));
}

void TimelinePanel::ask_frame() {
    const core::Page* p = page();
    if (p == nullptr || !core::anim::is_animation(*p)) {
        go(QString());  // (said so)
        return;
    }
    const auto count = static_cast<int>(core::anim::frames_of(*p));
    const auto chosen = ask::get_int(this, QStringLiteral("フレームへ移動"), QStringLiteral("フレーム（1〜%1）").arg(count),
                                     static_cast<int>(std::clamp<std::int64_t>(window_->current_frame(p), 1, count)), 1, count);
    if (!chosen) return;
    if (play_button->isChecked()) play_button->setChecked(false);
    set_frame(*chosen);
}

void TimelinePanel::set_work(std::optional<std::array<std::int64_t, 2>> span) {
    const core::Page* p = page();
    if (p == nullptr || !core::anim::is_animation(*p)) return;
    try {
        if (core::work_area(*p) == span) {
            refresh();
            return;
        }
    } catch (const std::exception& error) {  // (an anim's values no op makes — frames of words —: said, nothing changed)
        window_->flash(wording::error(QString::fromUtf8(error.what())), 5000);
        return;
    }
    set(Json{{"work", span ? Json::array({(*span)[0], (*span)[1]}) : Json()}});
}

void TimelinePanel::set_work_end(bool start) {
    const core::Page* p = page();
    if (p == nullptr || !core::anim::is_animation(*p)) return;
    std::array<std::int64_t, 2> played{1, 1};
    try {
        played = core::played_frames(*p);
    } catch (const std::exception& error) {
        window_->flash(wording::error(QString::fromUtf8(error.what())), 5000);
        return;
    }
    const std::int64_t now = window_->current_frame(p);
    set_work(start ? std::array<std::int64_t, 2>{now, std::max(now, played[1])} : std::array<std::int64_t, 2>{std::min(now, played[0]), now});
}

void TimelinePanel::delete_timeline() {
    const core::Page* p = page();
    if (p == nullptr || !core::anim::is_animation(*p)) return;
    play_button->setChecked(false);
    ops(Json::array({Json{{"op", "set_animation"}, {"page", p->index.json()}, {"off", true}}}));
}

void TimelinePanel::build_menu() {
    previous_button->setDefaultAction(window_->action(QStringLiteral("act_previous_frame")));
    next_button->setDefaultAction(window_->action(QStringLiteral("act_next_frame")));
    previous_button->setText(QStringLiteral("◀"));
    next_button->setText(QStringLiteral("▶"));
    // (made again from nothing each time it is called — J-e2 adds its video layers' items to it —: the submenus and the
    // panel's own items before gone, the window's commands only taken out)
    disconnect(menu_shown_);
    for (QMenu* sub : panel_menu->findChildren<QMenu*>(QString(), Qt::FindDirectChildrenOnly)) delete sub;
    panel_menu->clear();
    panel_menu->addAction(window_->action(QStringLiteral("act_timeline")));  // (ビデオタイムラインを作成)
    QAction* gone = panel_menu->addAction(QStringLiteral("タイムラインを削除"), this, [this] { delete_timeline(); });
    gone->setObjectName(QStringLiteral("timeline_delete"));
    gone->setToolTip(QStringLiteral("このページをふつうのページに戻します（セルはレイヤーのまま残ります）"));
    panel_menu->addSeparator();
    QMenu* moves = panel_menu->addMenu(QStringLiteral("移動"));
    for (const char* name : {"act_next_frame", "act_previous_frame", "act_first_frame", "act_last_frame", "act_work_start", "act_work_end"}) {
        moves->addAction(window_->action(QString::fromLatin1(name)));
    }
    moves->addSeparator();
    moves->addAction(window_->action(QStringLiteral("act_go_to_frame")));
    QMenu* area = panel_menu->addMenu(QStringLiteral("作業範囲"));
    area->setObjectName(QStringLiteral("timeline_work_menu"));
    area->addAction(QStringLiteral("作業範囲の開始点を設定"), this, [this] { set_work_end(true); })->setObjectName(QStringLiteral("timeline_work_start"));
    area->addAction(QStringLiteral("作業範囲の終了点を設定"), this, [this] { set_work_end(false); })->setObjectName(QStringLiteral("timeline_work_end"));
    area->addAction(QStringLiteral("作業範囲をクリア"), this, [this] { set_work(std::nullopt); })->setObjectName(QStringLiteral("timeline_work_clear"));
    panel_menu->addSeparator();
    panel_menu->addAction(window_->action(QStringLiteral("act_render_video")));
    menu_shown_ = connect(panel_menu, &QMenu::aboutToShow, this, [this] {
        const core::Page* p = page();
        const bool on = p != nullptr && core::anim::is_animation(*p);
        if (auto* gone_now = panel_menu->findChild<QAction*>(QStringLiteral("timeline_delete"))) gone_now->setEnabled(on);
        if (auto* area_now = panel_menu->findChild<QMenu*>(QStringLiteral("timeline_work_menu"))) area_now->setEnabled(on);
    });
}

// --- ビデオをレンダリング ----------------------------------------------------------------------------------------------

RenderVideoDialog::RenderVideoDialog(QWidget* parent, const core::Page& page) : QDialog(parent) {
    setWindowTitle(QStringLiteral("ビデオをレンダリング"));
    bool has_work = false;
    try {
        count_ = core::anim::frames_of(page);
        work_ = core::played_frames(page);
        page_fps_ = core::anim::fps_of(page);
        has_work = core::work_area(page).has_value();
    } catch (const std::exception&) {  // (an anim's values no op makes: all of its frames, at 12 fps, as the panel shows it)
        work_ = {1, count_};
    }
    range = new QComboBox;
    range->addItem(QStringLiteral("すべてのフレーム（1〜%1）").arg(count_), QStringLiteral("all"));
    if (has_work) range->addItem(QStringLiteral("作業範囲（%1〜%2）").arg(work_[0]).arg(work_[1]), QStringLiteral("work"));
    range->addItem(QStringLiteral("フレームを指定"), QStringLiteral("custom"));
    if (has_work) range->setCurrentIndex(1);  // (Photoshop's: the work area when there is one)
    first = new QSpinBox;
    last = new QSpinBox;
    for (QSpinBox* box : {first, last}) box->setRange(1, static_cast<int>(count_));
    first->setValue(static_cast<int>(work_[0]));
    last->setValue(static_cast<int>(work_[1]));
    const auto follow = [this] {
        const bool custom = range->currentData().toString() == QLatin1String("custom");
        first->setEnabled(custom);
        last->setEnabled(custom);
    };
    connect(range, qOverload<int>(&QComboBox::currentIndexChanged), this, [follow](int) { follow(); });
    follow();
    size = new QSpinBox;
    size->setRange(0, 8192);
    size->setSpecialValueText(QStringLiteral("ページのまま（100 dpi）"));
    size->setSuffix(QStringLiteral(" px（幅）"));
    size->setValue(0);
    fps = new QDoubleSpinBox;
    fps->setRange(core::kTimelineFpsMin, core::kTimelineFpsMax);
    fps->setDecimals(3);
    fps->setSuffix(QStringLiteral(" fps"));
    fps->setValue(page_fps_);
    auto* frames_row = new QHBoxLayout;
    frames_row->addWidget(first);
    frames_row->addWidget(new QLabel(QStringLiteral("〜")));
    frames_row->addWidget(last);
    auto* form = new QFormLayout;
    form->addRow(QStringLiteral("範囲"), range);
    form->addRow(QStringLiteral("フレーム"), frames_row);
    form->addRow(QStringLiteral("大きさ"), size);
    form->addRow(QStringLiteral("フレームレート"), fps);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("レンダリング…"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("キャンセル"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(buttons);
}

std::array<std::int64_t, 2> RenderVideoDialog::frames() const {
    const QString which = range->currentData().toString();
    if (which == QLatin1String("work")) return work_;
    if (which == QLatin1String("custom")) {
        const std::int64_t a = first->value(), b = last->value();
        return {std::min(a, b), std::max(a, b)};
    }
    return {1, count_};
}

std::optional<int> RenderVideoDialog::width_px() const {
    if (size->value() == 0) return std::nullopt;
    return size->value();
}

std::optional<double> RenderVideoDialog::speed() const {
    if (std::abs(fps->value() - page_fps_) < 0.0005) return std::nullopt;  // (the page's own, as it is)
    return fps->value();
}

}  // namespace genko::app
