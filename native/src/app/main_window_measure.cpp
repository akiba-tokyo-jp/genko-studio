// PC5 J-d1 (measure@1, SPEC.md PROD-01「計測」): Photoshop's イメージ → 解析 → 計測スケールを設定 ▸ (初期設定・カスタム…), the
// ものさしツール and the カウントツール (イメージ → 解析 ▸ and the スポイト group; their options: the line's readout, レイヤーの
// 角度補正 — Alt: the page turned and cut —, 消去; the count groups, their colour, sizes and 表示, クリア) and 表示 → 表示・非表示
// → カウント (the extra "count": app/canvas_measure.cpp) — each change of the book an op through apply_ops
// (render/ops_measure), as the person. The ものさしツール's readout also in the 情報 panel.
// PC5 J-d2: データポイントを選択 ▸ カスタム…, 計測値を記録 (the app's log: app/measurement_log), 目盛りマーカーを配置… (its
// window, then the op place_scale_marker) and ウィンドウ → 計測ログ (the panel, after レイヤーカンプ).

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFutureWatcher>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPixmap>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <memory>

#include "app/ask.hpp"
#include "app/canvas.hpp"
#include "app/config.hpp"
#include "app/info_panels.hpp"
#include "app/main_window.hpp"
#include "app/measurement_log.hpp"
#include "app/theme.hpp"
#include "app/tool_settings.hpp"
#include "core/measure.hpp"
#include "render/measure_features.hpp"
#include "service/wording.hpp"

namespace genko::app {

using core::Json;

namespace {

void paint_swatch(QToolButton* button, const QColor& colour) {
    QPixmap swatch(28, 16);
    swatch.fill(colour);
    button->setIcon(QIcon(swatch));
    button->setIconSize(swatch.size());
}

Json point_json(const QPointF& p) { return Json::array({std::round(p.x() * 10000.0) / 10000.0, std::round(p.y() * 10000.0) / 10000.0}); }

core::RulerLine line_of(QPointF start, QPointF end, bool protractor, QPointF arm) {
    core::RulerLine line;
    line.start = {start.x(), start.y()};
    line.end = {end.x(), end.y()};
    if (protractor) line.protractor = std::array<double, 2>{arm.x(), arm.y()};
    return line;
}

}  // namespace

void MainWindow::build_measure_actions() {
    // the ものさしツール and the カウントツール (Photoshop's, in the スポイト group: app/paint_panels.cpp, and イメージ → 解析 ▸);
    // no key of their own (I is the group's, which Genko does not go round)
    QAction* ruler = make(QStringLiteral("act_measure_ruler_tool"), QStringLiteral("ものさしツール"), [this] { choose_tool(QStringLiteral("measure")); }, {},
                          QStringLiteral("2 点の距離と角度を計ります（端をドラッグで動かす、Alt＋端からドラッグで分度器）"), true);
    tools_->addAction(ruler);
    ruler->setAutoRepeat(false);
    tool_actions_[QStringLiteral("measure")] = ruler;
    QAction* count = make(QStringLiteral("act_measure_count_tool"), QStringLiteral("カウントツール"), [this] { choose_tool(QStringLiteral("count")); }, {},
                          QStringLiteral("クリックした所に番号の印を付けて数えます（Alt＋クリックで外す、ドラッグで動かす）"), true);
    tools_->addAction(count);
    count->setAutoRepeat(false);
    tool_actions_[QStringLiteral("count")] = count;
    // イメージ → 解析 → 計測スケールを設定 ▸ 初期設定・カスタム… (image.analysis.setMeasurementScale)
    make(QStringLiteral("act_measure_scale_default"), QStringLiteral("初期設定"),
         [this] {
             if (measure_op(Json{{"op", "set_measurement_scale"}, {"preset", "default"}}))
                 flash(QStringLiteral("計測スケールを初期設定（1 pixel = 1 pixel）に戻しました"), 4000);
         },
         {}, QStringLiteral("計測スケールを 1 pixel = 1 pixel に戻します"));
    make(QStringLiteral("act_measure_scale_custom"), QStringLiteral("カスタム…"), [this] { measurement_scale_dialog(); }, {},
         QStringLiteral("計測スケール（ピクセルの長さ・論理的な長さ・単位）を決めます"));
    // (PC5 J-d2) イメージ → 解析 → データポイントを選択 ▸ カスタム… (image.analysis.selectDataPoints), 計測値を記録
    // (image.analysis.recordMeasurements: Photoshop's Shift+Ctrl+M in its key set — key_sets::photoshop_keys, the user's
    // decision 13 —, none in Genko's, where Ctrl+Shift+M is コマを結合's), 目盛りマーカーを配置… (image.analysis.placeScaleMarker)
    make(QStringLiteral("act_measure_data_points"), QStringLiteral("カスタム…"), [this] { data_points_dialog(); }, {},
         QStringLiteral("計測値を記録するときに出す項目（データポイント）を選びます"));
    make(QStringLiteral("act_measure_record"), QStringLiteral("計測値を記録"), [this] { (void)record_measurements(); }, {},
         QStringLiteral("選択範囲・ものさし・カウントの計測値を計測ログに記録します"));
    make(QStringLiteral("act_measure_scale_marker"), QStringLiteral("目盛りマーカーを配置…"), [this] { scale_marker_dialog(); }, {},
         QStringLiteral("計測スケールの長さの目盛りと文字を、ページの左下に置きます"));
    // ウィンドウ → 計測ログ (window.panel.measurementLog)
    make(QStringLiteral("act_panel_measurement_log"), QStringLiteral("計測ログ"),
         [this] {
             if (measurement_log_dock_ == nullptr) return;
             const bool show = !measurement_log_dock_->isVisible();
             measurement_log_dock_->setVisible(show);
             if (show) measurement_log_dock_->raise();
         },
         {}, QStringLiteral("記録した計測値の表（このアプリを閉じるまで）"), true);
    // 表示 → 表示・非表示 → カウント (view.show.count): the extra "count" on the canvas
    QAction* shown = make(QStringLiteral("act_show_count"), QStringLiteral("カウント"), [] {}, {}, QStringLiteral("カウントツールの印と番号を表示します"), true);
    shown->setChecked(settings()->value(QStringLiteral("view/show_count"), true).toBool());
    connect(shown, &QAction::toggled, this, [this](bool on) {
        settings()->setValue(QStringLiteral("view/show_count"), on);
        share_view_switch(QStringLiteral("act_show_count"), on);  // (H-b's: every open window the same)
        canvas_->show_extra(QStringLiteral("count"), on);
        canvas_->update();
    });
}

void MainWindow::build_measure_menus(QMenu* view) {
    // 表示 → 表示・非表示 ▸ (節 H's: show_menu, main_window_view.cpp — its place among the items show_rows())
    show_menu(view)->addAction(action(QStringLiteral("act_show_count")));
}

void MainWindow::build_measure_tools() {
    // the canvas: the count marks and the ものさしツール's line drawn over the page, the tools' presses and drags
    canvas_->add_extra({QStringLiteral("count"), [this](QPainter& painter, const core::Page& page, const PageCanvas::ExtraView& view) {
                            canvas_->paint_count(painter, page, view);
                        }});
    canvas_->show_extra(QStringLiteral("count"), action(QStringLiteral("act_show_count"))->isChecked());
    canvas_->add_extra({QStringLiteral("ruler"), [this](QPainter& painter, const core::Page& page, const PageCanvas::ExtraView& view) {
                            canvas_->paint_ruler(painter, page, view);
                        }});
    connect(canvas_, &PageCanvas::rulerMoving, this,
            [this](QPointF start, QPointF end, bool protractor, QPointF arm) { show_ruler_readout(line_of(start, end, protractor, arm)); });
    connect(canvas_, &PageCanvas::rulerDrawn, this, [this](QPointF start, QPointF end, bool protractor, QPointF arm) {
        Json op{{"op", "set_ruler_line"}, {"start", point_json(start)}, {"end", point_json(end)}};
        op["protractor"] = protractor ? point_json(arm) : Json(nullptr);
        // (the coordinator's decision after the review's P3-1: measuring never needs the book — a line the op cannot put on
        // the page (it is locked, the book read-only) stays in this window, its readout shown)
        const core::RulerLine line = line_of(start, end, protractor, arm);
        if (measure_op(std::move(op))) {
            canvas_->ruler_here.reset();
        } else if (const core::Page* page = current_page()) {
            canvas_->ruler_here = line;
            canvas_->ruler_here_page = page->id;
            canvas_->update();
            show_ruler_readout(line);
            flash(QStringLiteral("このページは変えられないため、ものさしの線はこの窓の中だけに引きました（計った値は出します）"), 5000);
        }
    });
    connect(canvas_, &PageCanvas::countRequested, this, [this](double x, double y) {
        measure_op(Json{{"op", "add_count"}, {"at_mm", point_json(QPointF(x, y))}});
    });
    connect(canvas_, &PageCanvas::countRemoved, this, [this](int group, int index) {
        measure_op(Json{{"op", "remove_count"}, {"group", group}, {"index", index}});
    });
    connect(canvas_, &PageCanvas::countMoved, this, [this](int group, int index, double x, double y) {
        measure_op(Json{{"op", "move_count"}, {"group", group}, {"index", index}, {"to", point_json(QPointF(x, y))}});
    });
    if (tool_settings_ == nullptr) return;
    // the ものさしツール's options: X・Y・W・H・A・L1・L2, レイヤーの角度補正 (Alt: the page turned and cut), 消去 (Photoshop's)
    {
        auto* page = new QWidget;
        page->setObjectName(QStringLiteral("measure_settings"));
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(0, 0, 0, 0);
        ruler_readout_ = new QLabel(QStringLiteral("—"));
        ruler_readout_->setObjectName(QStringLiteral("ruler_readout"));
        ruler_readout_->setWordWrap(true);
        ruler_readout_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(ruler_readout_);
        auto* straighten = new QPushButton(QStringLiteral("レイヤーの角度補正"));
        straighten->setObjectName(QStringLiteral("ruler_straighten"));
        straighten->setToolTip(QStringLiteral("線が水平か垂直になるように、ページを回して切り抜きます。Alt＋クリックで、切り抜かずに選んだレイヤーだけを回します"));
        connect(straighten, &QPushButton::clicked, this, [this] { straighten_by_ruler((QApplication::keyboardModifiers() & Qt::AltModifier) == 0); });
        layout->addWidget(straighten);
        auto* clear = new QPushButton(QStringLiteral("消去"));
        clear->setObjectName(QStringLiteral("ruler_clear"));
        clear->setToolTip(QStringLiteral("ものさしの線を消します"));
        connect(clear, &QPushButton::clicked, this, [this] {
            if (canvas_->ruler_here) {  // (this window's line: gone, the book untouched)
                canvas_->ruler_here.reset();
                canvas_->update();
                show_ruler_readout();
                return;
            }
            const core::Page* p = current_page();
            if (p == nullptr || !p->measure || !p->measure->ruler) return;
            measure_op(Json{{"op", "set_ruler_line"}, {"clear", true}});
        });
        layout->addWidget(clear);
        auto* hint = new QLabel(QStringLiteral("長さは mm とページの解像度の画素で出します。計測スケールを決めると、その単位でも出します"));
        hint->setWordWrap(true);
        theme::role(hint, "hint");
        layout->addWidget(hint);
        layout->addStretch(1);
        tool_settings_->add({QStringLiteral("measure")}, page);
    }
    // the カウントツール's options: the group (new, delete), its colour, 印のサイズ, ラベルのサイズ, 表示, クリア (Photoshop's)
    {
        auto* page = new QWidget;
        page->setObjectName(QStringLiteral("count_settings"));
        auto* form = new QFormLayout(page);
        form->setContentsMargins(0, 0, 0, 0);
        auto* row = new QHBoxLayout;
        count_groups_ = new QComboBox;
        count_groups_->setObjectName(QStringLiteral("count_groups"));
        count_groups_->setToolTip(QStringLiteral("印を付けるカウントグループ"));
        connect(count_groups_, &QComboBox::activated, this, [this](int i) {
            const core::Page* p = current_page();
            if (p == nullptr || !p->measure || i < 0 || static_cast<std::size_t>(i) == p->measure->active_group) return;
            measure_op(Json{{"op", "set_count_group"}, {"group", i}, {"active", true}});
        });
        row->addWidget(count_groups_, 1);
        auto* add = new QToolButton;
        add->setObjectName(QStringLiteral("count_new_group"));
        add->setText(QStringLiteral("＋"));
        add->setToolTip(QStringLiteral("新しいカウントグループを作ります"));
        connect(add, &QToolButton::clicked, this, [this] { measure_op(Json{{"op", "new_count_group"}}); });
        row->addWidget(add);
        auto* remove = new QToolButton;
        remove->setObjectName(QStringLiteral("count_delete_group"));
        remove->setText(QStringLiteral("－"));
        remove->setToolTip(QStringLiteral("選んだカウントグループを削除します（取り消せます）"));
        connect(remove, &QToolButton::clicked, this, [this] {
            const core::Page* p = current_page();
            if (p == nullptr || !p->measure || p->measure->count_groups.empty()) return;
            measure_op(Json{{"op", "delete_count_group"}});
        });
        row->addWidget(remove);
        form->addRow(QStringLiteral("カウントグループ"), row);
        count_colour_ = new QToolButton;
        count_colour_->setObjectName(QStringLiteral("count_color"));
        count_colour_->setToolTip(QStringLiteral("グループの印と番号の色"));
        paint_swatch(count_colour_, QColor(255, 0, 0));
        connect(count_colour_, &QToolButton::clicked, this, [this] {
            const core::Page* p = current_page();
            if (p == nullptr || !p->measure || p->measure->count_groups.empty()) return;
            const auto& c = p->measure->count_groups[p->measure->active_group].color;
            const std::optional<QColor> chosen = ask::colour(this, QColor(c[0], c[1], c[2]), QStringLiteral("カウントグループのカラー"));
            if (!chosen || !chosen->isValid()) return;
            measure_op(Json{{"op", "set_count_group"}, {"color", Json::array({chosen->red(), chosen->green(), chosen->blue()})}});
        });
        form->addRow(QStringLiteral("カラー"), count_colour_);
        count_marker_ = new QSpinBox;
        count_marker_->setObjectName(QStringLiteral("count_marker_size"));
        count_marker_->setRange(1, 10);
        count_label_ = new QSpinBox;
        count_label_->setObjectName(QStringLiteral("count_label_size"));
        count_label_->setRange(8, 72);
        for (QSpinBox* box : {count_marker_, count_label_}) {
            box->setKeyboardTracking(false);
            connect(box, &QSpinBox::valueChanged, this, [this, box](int value) {
                const core::Page* p = current_page();
                if (p == nullptr || !p->measure || p->measure->count_groups.empty()) return;
                const core::CountGroup& g = p->measure->count_groups[p->measure->active_group];
                const bool marker = box == count_marker_;
                if ((marker ? g.marker_size : g.label_size) == value) return;
                measure_op(Json{{"op", "set_count_group"}, {marker ? "marker_size" : "label_size", value}});
            });
        }
        form->addRow(QStringLiteral("マーカーのサイズ"), count_marker_);
        form->addRow(QStringLiteral("ラベルのサイズ"), count_label_);
        count_visible_ = new QCheckBox(QStringLiteral("表示"));
        count_visible_->setObjectName(QStringLiteral("count_visible"));
        count_visible_->setToolTip(QStringLiteral("このグループの印を表示します"));
        connect(count_visible_, &QCheckBox::toggled, this, [this](bool on) {
            const core::Page* p = current_page();
            if (p == nullptr || !p->measure || p->measure->count_groups.empty()) return;
            if (p->measure->count_groups[p->measure->active_group].visible == on) return;
            measure_op(Json{{"op", "set_count_group"}, {"visible", on}});
        });
        form->addRow(count_visible_);
        auto* clear = new QPushButton(QStringLiteral("クリア"));
        clear->setObjectName(QStringLiteral("count_clear"));
        clear->setToolTip(QStringLiteral("このグループの印をすべて外します（取り消せます）"));
        connect(clear, &QPushButton::clicked, this, [this] {
            const core::Page* p = current_page();
            if (p == nullptr || !p->measure || p->measure->count_groups.empty()) {
                flash(QStringLiteral("このページにはカウントがありません"), 3000);
                return;
            }
            measure_op(Json{{"op", "clear_count"}});
        });
        form->addRow(clear);
        tool_settings_->add({QStringLiteral("count")}, page);
    }
    refresh_measure();
}

bool MainWindow::measure_op(Json op) {
    const core::Page* page = current_page();
    if (page == nullptr) return false;
    op["page"] = page->index.json();
    const bool done = apply_ops(Json::array({std::move(op)}));
    refresh_measure();
    canvas_->update();
    return done;
}

void MainWindow::refresh_measure() {
    if (canvas_->ruler_here && (current_page() == nullptr || current_page()->id != canvas_->ruler_here_page)) canvas_->ruler_here.reset();
    show_ruler_readout();
    if (count_groups_ == nullptr) return;
    const core::Page* page = current_page();
    const core::Measure none;
    const core::Measure& m = page != nullptr && page->measure ? *page->measure : none;
    const QSignalBlocker quiet_groups(count_groups_), quiet_marker(count_marker_), quiet_label(count_label_), quiet_visible(count_visible_);
    count_groups_->clear();
    for (const core::CountGroup& g : m.count_groups) count_groups_->addItem(QStringLiteral("%1（%2）").arg(QString::fromStdString(g.name)).arg(g.points.size()));
    const bool any = !m.count_groups.empty();
    if (any) count_groups_->setCurrentIndex(static_cast<int>(m.active_group));
    const core::CountGroup shown = any ? m.count_groups[m.active_group] : core::new_count_group(0);
    paint_swatch(count_colour_, QColor(shown.color[0], shown.color[1], shown.color[2]));
    count_marker_->setValue(shown.marker_size);
    count_label_->setValue(shown.label_size);
    count_visible_->setChecked(shown.visible);
    for (QWidget* w : {static_cast<QWidget*>(count_colour_), static_cast<QWidget*>(count_marker_), static_cast<QWidget*>(count_label_),
                       static_cast<QWidget*>(count_visible_)})
        w->setEnabled(any);
}

void MainWindow::show_ruler_readout(const std::optional<core::RulerLine>& given) {
    const core::Page* page = current_page();
    std::optional<core::RulerLine> line = given;
    if (!line) line = canvas_->ruler_here;  // (a line the book could not take: this window's)
    if (!line && page != nullptr && page->measure) line = page->measure->ruler;
    std::optional<Json> readout;
    if (line && page != nullptr) {
        readout = core::to_json(core::ruler_readout(*line, page->measure ? page->measure->scale : std::nullopt, core::page_px_per_mm(*page)));
    }
    if (ruler_readout_ != nullptr) {
        if (!readout) {
            ruler_readout_->setText(QStringLiteral("—"));
        } else {
            const auto num = [&](const char* key) { return readout->at(key).get<double>(); };
            QString text = QStringLiteral("X %1  Y %2  W %3  H %4 mm\nA %5°  L1 %6 mm")
                               .arg(num("x_mm"), 0, 'f', 2)
                               .arg(num("y_mm"), 0, 'f', 2)
                               .arg(num("w_mm"), 0, 'f', 2)
                               .arg(num("h_mm"), 0, 'f', 2)
                               .arg(num("angle"), 0, 'f', 1)
                               .arg(num("l1_mm"), 0, 'f', 2);
            if (readout->at("protractor").get<bool>()) text += QStringLiteral("  L2 %1 mm").arg(num("l2_mm"), 0, 'f', 2);
            ruler_readout_->setText(text);
        }
    }
    if (info_panel_ != nullptr) info_panel_->show_ruler(readout);
}

void MainWindow::straighten_by_ruler(bool crop) {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    if (!page->measure || !page->measure->ruler) {
        flash(canvas_->ruler_here ? QStringLiteral("このページは変えられないため、角度補正できません（線はこの窓の中だけのものです）")
                                  : QStringLiteral("先にものさしツールで、水平か垂直にしたい線に沿ってドラッグします"),
              4000);
        return;
    }
    if (crop) {
        page_size_op(Json{{"op", "straighten_layer"}, {"page", page->index.json()}, {"crop", true}}, QStringLiteral("角度補正して切り抜きました"));
        refresh_measure();
        return;
    }
    const std::vector<std::string> ids = transform_targets();
    if (ids.empty()) {
        flash(QStringLiteral("先に角度補正するレイヤーを選びます（Alt＋クリックで、選んだレイヤーだけを回します）"), 4000);
        return;
    }
    Json list = Json::array();
    for (const std::string& id : ids) list.push_back(id);
    if (measure_op(Json{{"op", "straighten_layer"}, {"ids", std::move(list)}})) flash(QStringLiteral("レイヤーを角度補正しました"), 3000);
}

void MainWindow::measurement_scale_dialog() {
    const Asked asked = asking();
    const core::Page* page = current_page();
    if (page == nullptr) return;
    const core::MeasurementScale now = page->measure && page->measure->scale ? *page->measure->scale : core::MeasurementScale{};
    const double paper = core::page_px_per_mm(*page);
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("計測スケール"));
    dialog.setObjectName(QStringLiteral("measurement_scale"));
    auto* form = new QFormLayout(&dialog);
    auto* pixels = new QDoubleSpinBox;
    pixels->setObjectName(QStringLiteral("scale_pixel_length"));
    pixels->setRange(0.001, 1e9);
    pixels->setDecimals(6);
    pixels->setValue(now.pixel_length);
    auto* logical = new QDoubleSpinBox;
    logical->setObjectName(QStringLiteral("scale_logical_length"));
    logical->setRange(0.0001, 1e9);
    logical->setDecimals(6);
    logical->setValue(now.logical_length);
    auto* units = new QLineEdit(QString::fromStdString(now.units));
    units->setObjectName(QStringLiteral("scale_units"));
    form->addRow(QStringLiteral("ピクセルの長さ"), pixels);
    form->addRow(QStringLiteral("論理的な長さ"), logical);
    form->addRow(QStringLiteral("論理的な単位"), units);
    auto* by_paper = new QPushButton(QStringLiteral("用紙の mm にする"));
    by_paper->setObjectName(QStringLiteral("scale_paper_mm"));
    by_paper->setToolTip(QStringLiteral("ページの解像度（%1 dpi）で 1 mm になる画素の数を 1 mm にします").arg(std::round(paper * 25.4)));
    auto paper_pressed = std::make_shared<bool>(false);
    connect(by_paper, &QPushButton::clicked, &dialog, [=] {
        *paper_pressed = true;
        pixels->setValue(paper);
        logical->setValue(1.0);
        units->setText(QStringLiteral("mm"));
    });
    form->addRow(by_paper);
    auto* note = new QLabel(QStringLiteral("ピクセルの長さはページの解像度の画素で数えます。初期設定は 1 pixel = 1 pixel です"));
    note->setWordWrap(true);
    theme::role(note, "hint");
    form->addRow(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);
    if (ask::exec(&dialog) != QDialog::Accepted || !still(asked)) return;
    if (units->text().trimmed().isEmpty()) {
        flash(QStringLiteral("論理的な単位を入れます"), 3000, true);
        return;
    }
    // (the review's P3-6: 用紙の mm pressed and left as it put them — the preset, the page's pixels a mm as the op has them)
    const bool by_preset = *paper_pressed && std::fabs(pixels->value() - pixels->valueFromText(pixels->textFromValue(paper))) < 1e-9 &&
                           logical->value() == 1.0 && units->text().trimmed() == QStringLiteral("mm");
    Json op = by_preset ? Json{{"op", "set_measurement_scale"}, {"preset", "paper_mm"}}
                        : Json{{"op", "set_measurement_scale"},
                               {"pixel_length", pixels->value()},
                               {"logical_length", logical->value()},
                               {"units", units->text().trimmed().toStdString()}};
    if (measure_op(std::move(op))) {
        flash(QStringLiteral("計測スケールを決めました"), 3000);
    }
}

// --- PC5 J-d2: 計測値を記録, the 計測ログ panel, データポイントを選択, 目盛りマーカーを配置 ---------------------------------------

void MainWindow::build_measurement_log_dock() {
    measurement_log_panel_ = new MeasurementLogPanel(this);
    measurement_log_dock_ = new QDockWidget(QStringLiteral("計測ログ"), this);
    measurement_log_dock_->setObjectName(QStringLiteral("計測ログ"));
    measurement_log_dock_->setWidget(measurement_log_panel_);
    measurement_log_dock_->setMinimumWidth(200);
    measurement_log_dock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::BottomDockWidgetArea, measurement_log_dock_);  // (Photoshop's: a wide table under the canvas)
    measurement_log_dock_->hide();  // (ウィンドウ → 計測ログ brings it, and 計測値を記録)
    QAction* toggle = action(QStringLiteral("act_panel_measurement_log"));
    view_menu_->addAction(toggle);
    connect(measurement_log_dock_, &QDockWidget::visibilityChanged, this, [this, toggle](bool) {
        const QSignalBlocker quiet(toggle);
        toggle->setChecked(measurement_log_dock_->isVisible());
    });
}

bool MainWindow::record_measurements() {
    if (measuring_) {  // (one at a time: the one under way finishes first)
        flash(QStringLiteral("計測しています…（終わるまでお待ちください）"), 3000);
        return false;
    }
    const core::Page* page = current_page();
    if (page == nullptr) return false;
    const std::optional<Json> area = selection_area();
    // (a ruler line the book could not take — this window's — measured as the page's: measuring never needs the book)
    auto measured = std::make_shared<core::Page>(*page);
    if (!area && canvas_->ruler_here && (!page->measure || !page->measure->ruler)) {
        if (!measured->measure) measured->measure = core::Measure{};
        measured->measure->ruler = *canvas_->ruler_here;
    }
    // (the review's P2-1: measured on a worker — the book and the page as they are now, shared, never changed —, the rows
    // put in the log here when it is done; the window goes on meanwhile, 計測値を記録 waits)
    struct Measured {
        render::measure::Recorded made;
        QString error;
    };
    const auto doc = std::make_shared<const core::Document>(book());
    measuring_ = true;
    QAction* record = action(QStringLiteral("act_measure_record"));
    record->setEnabled(false);
    QApplication::setOverrideCursor(Qt::BusyCursor);
    flash(QStringLiteral("計測しています…"), 2000);
    auto* watcher = new QFutureWatcher<Measured>(this);
    connect(watcher, &QFutureWatcher<Measured>::finished, this, [this, watcher, record, had_area = area.has_value()] {
        watcher->deleteLater();
        QApplication::restoreOverrideCursor();
        measuring_ = false;
        record->setEnabled(true);
        Measured result = watcher->result();
        if (!result.error.isEmpty()) {
            flash(QStringLiteral("計測値を記録できませんでした: %1").arg(wording::error(result.error)), 5000, true);
            return;
        }
        const render::measure::Recorded& made = result.made;
        try {  // (the review's P1-1: the log and its table made again — memory they cannot have said, not a crash)
            (void)MeasurementLog::instance().add(made);
        } catch (const std::exception&) {
            flash(QStringLiteral("計測値を記録できませんでした: 計測ログに入れる記憶が足りません（計測ログの行を削除してください）"), 6000, true);
            return;
        }
        if (measurement_log_dock_ != nullptr && !measurement_log_dock_->isVisible()) {
            measurement_log_dock_->show();
            measurement_log_dock_->raise();
        }
        const QString source = made.source == "selection" ? (had_area ? QStringLiteral("選択範囲") : QStringLiteral("ページ全体"))
                               : made.source == "ruler"    ? QStringLiteral("ものさし")
                                                           : QStringLiteral("カウント");
        if (made.features_left_out > 0) {
            flash(QStringLiteral("%1の計測値を記録しました（塊が %2 個と多いため、塊ごとの行は先頭の %3 個だけ。まとめの行は全部を数えています）")
                      .arg(source)
                      .arg(made.features_left_out + render::measure::kFeatureRowsMax)
                      .arg(render::measure::kFeatureRowsMax),
                  6000);
        } else {
            flash(QStringLiteral("%1の計測値を記録しました（%2 行）").arg(source).arg(made.rows.size()), 4000);
        }
    });
    watcher->setFuture(QtConcurrent::run([doc, measured, area]() -> Measured {
        Measured out;
        try {
            // (its number given when it goes into the log: a refused one takes none — the review's P3-2)
            out.made = render::measure::record(*doc, *measured, "auto", area, 0, render::measure::now_text());
        } catch (const std::exception& error) {
            out.error = QString::fromUtf8(error.what());
        }
        return out;
    }));
    return true;
}

void MainWindow::data_points_dialog() {
    namespace m = render::measure;
    const m::DataPoints now = MeasurementLog::data_points();
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("データポイントを選択"));
    dialog.setObjectName(QStringLiteral("data_points"));
    auto* layout = new QVBoxLayout(&dialog);
    // (Photoshop's groups: 共通 — every source's —, 選択範囲, ものさしツール, カウントツール)
    struct Box {
        QCheckBox* box;
        std::string key;
        std::string source;  // ("": 共通)
    };
    std::vector<Box> boxes;
    const auto group = [&](const QString& title, const std::string& source, const std::vector<std::string>& keys) {
        auto* frame = new QGroupBox(title);
        frame->setObjectName(QStringLiteral("points_%1").arg(source.empty() ? QStringLiteral("common") : QString::fromStdString(source)));
        auto* grid = new QGridLayout(frame);
        for (std::size_t i = 0; i < keys.size(); ++i) {
            auto* box = new QCheckBox(QString::fromStdString(m::column_name(keys[i])));
            box->setObjectName(QStringLiteral("point_%1_%2").arg(source.empty() ? QStringLiteral("common") : QString::fromStdString(source),
                                                                  QString::fromStdString(keys[i])));
            const std::vector<std::string>& list = now.of(source.empty() ? "selection" : source);
            box->setChecked(std::find(list.begin(), list.end(), keys[i]) != list.end());
            grid->addWidget(box, static_cast<int>(i / 3), static_cast<int>(i % 3));
            boxes.push_back({box, keys[i], source});
        }
        layout->addWidget(frame);
    };
    const auto own = [](std::string_view source) {
        std::vector<std::string> keys = m::available(source);
        keys.erase(keys.begin(), keys.begin() + 7);  // (the common ones first: label … scaleFactor)
        return keys;
    };
    std::vector<std::string> common = m::available("count");
    common.pop_back();
    group(QStringLiteral("共通"), "", common);
    group(QStringLiteral("選択範囲"), "selection", own("selection"));
    group(QStringLiteral("ものさしツール"), "ruler", own("ruler"));
    group(QStringLiteral("カウントツール"), "count", own("count"));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    buttons->button(QDialogButtonBox::RestoreDefaults)->setText(QStringLiteral("初期設定に戻す"));
    buttons->button(QDialogButtonBox::RestoreDefaults)->setObjectName(QStringLiteral("points_defaults"));
    connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, &dialog, [&boxes] {
        for (const Box& b : boxes) b.box->setChecked(true);
    });
    layout->addWidget(buttons);
    if (ask::exec(&dialog) != QDialog::Accepted) return;
    Json changes = Json::object();
    for (const Box& b : boxes) {
        for (const std::string_view source : m::kSources) {
            if (!b.source.empty() && b.source != source) continue;
            changes[std::string(source)][b.key] = b.box->isChecked();
        }
    }
    MeasurementLog::set_data_points(m::choose_data_points(now, changes));
    flash(QStringLiteral("計測値を記録するときの項目を決めました"), 3000);
}

void MainWindow::scale_marker_dialog() {
    const Asked asked = asking();
    const core::Page* page = current_page();
    if (page == nullptr) return;
    const std::optional<core::MeasurementScale> scale = page->measure ? page->measure->scale : std::nullopt;
    const QString units = QString::fromStdString(scale ? scale->units : core::MeasurementScale{}.units);
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("目盛りマーカー"));
    dialog.setObjectName(QStringLiteral("scale_marker"));
    auto* form = new QFormLayout(&dialog);
    auto* length = new QDoubleSpinBox;
    length->setObjectName(QStringLiteral("marker_length"));
    // (the review's P3-3: any scale's lengths — 6 decimals over a wide range —; the nice length, left as it is shown, is
    // the op's own: no length sent)
    length->setRange(1e-6, 1e12);
    length->setDecimals(6);
    length->setValue(render::measure::nice_marker_length(*page));
    const double shown_length = length->value();
    length->setSuffix(QStringLiteral(" ") + units);
    form->addRow(QStringLiteral("長さ"), length);
    auto* font = new QLineEdit;
    font->setObjectName(QStringLiteral("marker_font"));
    font->setPlaceholderText(QStringLiteral("（既定のフォント）"));
    form->addRow(QStringLiteral("フォント"), font);
    auto* size = new QDoubleSpinBox;
    size->setObjectName(QStringLiteral("marker_size"));
    size->setRange(1, 1000);
    size->setDecimals(1);
    size->setValue(12);
    size->setSuffix(QStringLiteral(" pt"));
    form->addRow(QStringLiteral("サイズ"), size);
    auto* text = new QCheckBox(QStringLiteral("テキストを表示"));
    text->setObjectName(QStringLiteral("marker_text"));
    text->setChecked(true);
    form->addRow(text);
    auto* position = new QComboBox;
    position->setObjectName(QStringLiteral("marker_position"));
    position->addItem(QStringLiteral("下"), QStringLiteral("bottom"));
    position->addItem(QStringLiteral("上"), QStringLiteral("top"));
    form->addRow(QStringLiteral("テキストの位置"), position);
    auto* colour = new QComboBox;
    colour->setObjectName(QStringLiteral("marker_color"));
    colour->addItem(QStringLiteral("黒"), QStringLiteral("black"));
    colour->addItem(QStringLiteral("白"), QStringLiteral("white"));
    form->addRow(QStringLiteral("カラー"), colour);
    auto* note = new QLabel(QStringLiteral("長さは計測スケールの単位で決めます（%1）。目盛りと文字は「目盛りマーカー」のグループになります")
                                .arg(QString::fromStdString(core::describe(scale.value_or(core::MeasurementScale{})))));
    note->setWordWrap(true);
    theme::role(note, "hint");
    form->addRow(note);
    connect(text, &QCheckBox::toggled, &dialog, [=](bool on) {
        font->setEnabled(on);
        size->setEnabled(on);
        position->setEnabled(on);
    });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);
    if (ask::exec(&dialog) != QDialog::Accepted || !still(asked)) return;
    Json op{{"op", "place_scale_marker"},
            {"size_pt", size->value()},
            {"text", text->isChecked()},
            {"position", position->currentData().toString().toStdString()},
            {"color", colour->currentData().toString().toStdString()}};
    if (length->value() != shown_length) op["length"] = length->value();
    if (!font->text().trimmed().isEmpty()) op["font"] = font->text().trimmed().toStdString();
    if (measure_op(std::move(op))) flash(QStringLiteral("目盛りマーカーを配置しました"), 3000);
}

}  // namespace genko::app
