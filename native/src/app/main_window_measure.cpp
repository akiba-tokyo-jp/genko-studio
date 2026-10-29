// PC5 J-d1 (measure@1, SPEC.md PROD-01「計測」): Photoshop's イメージ → 解析 → 計測スケールを設定 ▸ (初期設定・カスタム…), the
// ものさしツール and the カウントツール (イメージ → 解析 ▸ and the スポイト group; their options: the line's readout, レイヤーの
// 角度補正 — Alt: the page turned and cut —, 消去; the count groups, their colour, sizes and 表示, クリア) and 表示 → 表示・非表示
// → カウント (the extra "count": app/canvas_measure.cpp) — each change of the book an op through apply_ops
// (render/ops_measure), as the person. The ものさしツール's readout also in the 情報 panel.

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
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

#include <cmath>
#include <memory>

#include "app/ask.hpp"
#include "app/canvas.hpp"
#include "app/config.hpp"
#include "app/info_panels.hpp"
#include "app/main_window.hpp"
#include "app/theme.hpp"
#include "app/tool_settings.hpp"
#include "core/measure.hpp"

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

}  // namespace genko::app
