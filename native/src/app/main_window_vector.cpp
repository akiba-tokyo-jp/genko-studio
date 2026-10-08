// Editing lines in the window (M3④-3, Python's main.py: act_reshape, act_vector and the commands on the chosen lines —
// join, cut where clicked, the pen's colour, delete, fewer points; the tool settings of 線の編集 (なぞって直す) and
// 線の修正（つまむ）; and what they become: _vector_edit, _vector_traced, _vector_simplify, _vector_selected, _reshape).

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QSpinBox>

#include "app/brush_panel.hpp"
#include "app/fields.hpp"
#include "app/main_window.hpp"
#include "app/theme.hpp"
#include "app/tool_settings.hpp"
#include "core/pynum.hpp"

namespace genko::app {

using core::Json;

void MainWindow::build_vector_actions() {
    const auto tool = [this](const char* attribute, const QString& title, const char* name, const QString& key, const QString& tip) {
        const QString which = QString::fromLatin1(name);
        QAction* act = make(QString::fromLatin1(attribute), title, [this, which] { choose_tool(which); }, {QKeySequence(key)}, tip, true);
        tools_->addAction(act);
        act->setAutoRepeat(false);  // (a held key chooses the tool once)
        tool_actions_[which] = act;
    };
    tool("act_reshape", QStringLiteral("線の修正（つまむ）"), "reshape", QStringLiteral("Y"),
         QStringLiteral("描いた線をつまんでドラッグすると、その辺りが滑らかに動きます"));
    tool("act_vector", QStringLiteral("線の編集（制御点）"), "vector", QStringLiteral("Shift+Y"),
         QStringLiteral("線を選んで制御点を動かす（Alt+クリックで点を足す、Delete で消す、Shift+クリックで 2 本目）"));
    make("act_vector_join", QStringLiteral("選んだ 2 本の線をつなぐ"), [this] { vector_selected("connect"); });
    QAction* cut = make("act_vector_cut", QStringLiteral("クリックした所で線を切る"), [] {}, {},
                        QStringLiteral("オンの間、線をクリックするとそこで 2 本に分かれます"), true);
    connect(cut, &QAction::toggled, this, [this](bool on) { canvas_->vector_cut = on; });
    make("act_vector_colour", QStringLiteral("選んだ線をペンの色にする"), [this] { vector_selected("recolor"); });
    make("act_vector_delete", QStringLiteral("選んだ線を消す"), [this] { vector_selected("delete"); });
    make("act_vector_simplify", QStringLiteral("選んだ線の点を減らす"), [this] { vector_simplify(); }, {},
         QStringLiteral("形を保ったまま、制御点を減らします（単純化）"));
}

void MainWindow::build_vector_pages(ToolSettings* ts) {
    auto* vector_note = new QLabel(QStringLiteral("線をクリックで選び、□（制御点）をドラッグ。Alt+クリックで点を足し、Delete で点（または線）を消す。"
                                                  "Shift+クリックで 2 本目を選ぶ。"));
    vector_note->setWordWrap(true);
    theme::role(vector_note, "hint");
    vector_mode_ = new QComboBox;
    vector_mode_->setObjectName(QStringLiteral("vector_mode"));
    for (const auto& [label, key] : {std::pair{QStringLiteral("点を直す・選ぶ"), QStringLiteral("edit")}, {QStringLiteral("なぞって太らせる"), QStringLiteral("widen")},
                                     {QStringLiteral("なぞって細らせる"), QStringLiteral("narrow")}, {QStringLiteral("なぞって描き直す（形）"), QStringLiteral("redraw")},
                                     {QStringLiteral("なぞって太さを描き直す"), QStringLiteral("redraw_width")},
                                     {QStringLiteral("なぞって端をつなぐ"), QStringLiteral("join")}, {QStringLiteral("なぞって点を減らす"), QStringLiteral("simplify")}})
        vector_mode_->addItem(label, key);
    vector_mode_->setToolTip(QStringLiteral("なぞって直す: 線の上をなぞった所だけを直します。描き直す（形）: 線の上から描き始めて"
                                            "同じ線の上で終えると、その間がなぞった形になる。太さを描き直す: ペンの筆圧が線の太さになる"));
    vector_mode_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    vector_mode_->setMinimumContentsLength(6);
    connect(vector_mode_, &QComboBox::currentIndexChanged, this, [this](int) { canvas_->vector_mode = vector_mode_->currentData().toString(); });
    auto* reach = new QDoubleSpinBox;
    reach->setObjectName(QStringLiteral("vector_reach"));
    reach->setRange(0.3, 20);
    reach->setSingleStep(0.5);
    reach->setSuffix(QStringLiteral(" mm"));
    reach->setValue(canvas_->vector_radius_mm);
    reach->setToolTip(QStringLiteral("なぞった所からこの幅の中の線を直します"));
    connect(reach, &QDoubleSpinBox::valueChanged, this, [this](double v) { canvas_->vector_radius_mm = v; });
    vector_amount_ = new QSpinBox;
    vector_amount_->setObjectName(QStringLiteral("vector_amount"));
    vector_amount_->setRange(5, 100);
    vector_amount_->setSuffix(QStringLiteral(" %"));
    vector_amount_->setValue(30);
    vector_amount_->setToolTip(QStringLiteral("太らせる・細らせるの 1 回の強さ"));
    vector_join_ = new QDoubleSpinBox;
    vector_join_->setObjectName(QStringLiteral("vector_join"));
    vector_join_->setRange(0.5, 30);
    vector_join_->setSuffix(QStringLiteral(" mm"));
    vector_join_->setValue(5.0);
    vector_join_->setToolTip(QStringLiteral("端をつなぐ: この距離までの端どうしをつなぐ"));
    auto* vector_form = new QWidget;
    auto* vfl = new QFormLayout(vector_form);
    vfl->setRowWrapPolicy(QFormLayout::WrapAllRows);
    vfl->setContentsMargins(0, 0, 0, 0);
    vfl->addRow(QStringLiteral("直し方"), vector_mode_);
    vfl->addRow(QStringLiteral("なぞる幅"), slider_for(reach, true));
    vfl->addRow(QStringLiteral("太らせ・細らせの強さ"), slider_for(vector_amount_));
    vfl->addRow(QStringLiteral("つなぐ距離"), slider_for(vector_join_));
    ts->add({QStringLiteral("vector")},
            action_page({static_cast<QWidget*>(vector_note), QStringLiteral("なぞって直す"), vector_form, QStringLiteral("線"), action("act_vector_cut"),
                         action("act_vector_join"), action("act_vector_colour"), action("act_vector_simplify"), action("act_vector_delete"),
                         QStringLiteral("選んだ点"), action("act_point_wider"), action("act_point_thinner")}));
    // 線の修正（つまむ）
    auto* radius = new QDoubleSpinBox;
    radius->setObjectName(QStringLiteral("reshape_radius"));
    radius->setRange(1, 60);
    radius->setSuffix(QStringLiteral(" mm"));
    radius->setValue(canvas_->reshape_radius_mm);
    connect(radius, &QDoubleSpinBox::valueChanged, this, [this](double v) { canvas_->reshape_radius_mm = v; });
    auto* radius_page = new QWidget;
    auto* radius_form = new QFormLayout(radius_page);
    radius_form->setRowWrapPolicy(QFormLayout::WrapAllRows);
    radius_form->setContentsMargins(0, 0, 0, 0);
    radius_form->addRow(QStringLiteral("つまんだ所から動く範囲"), radius);
    auto* pin = new QCheckBox(QStringLiteral("線の端は動かさない"));
    pin->setObjectName(QStringLiteral("reshape_pin"));
    pin->setToolTip(QStringLiteral("つまんでも、線の両端は元の場所にとどまります（端に近いほど動きが小さい）"));
    connect(pin, &QCheckBox::toggled, this, [this](bool on) { canvas_->reshape_pin_ends = on; });
    radius_form->addRow(pin);
    ts->add({QStringLiteral("reshape")}, radius_page);
}

// --- what the edits become ----------------------------------------------------------------------------------------

void MainWindow::vector_edit(const Json& change) {
    const core::Layer* layer = paint_layer();
    const core::Page* page = current_page();
    if (layer == nullptr || page == nullptr) return;
    Json op{{"op", "vector_edit"}, {"page", page->index.json()}, {"layer_id", layer->id}};
    for (const auto& [key, value] : change.items()) op[key] = value;
    apply_ops(Json::array({op}));
    canvas_->update();
}

void MainWindow::vector_traced(const Json& points, const QString& mode) {
    // a trace with the vector tool: the lines near it are mended as the tool's 直し方 says
    const core::Layer* layer = paint_layer();
    const core::Page* page = current_page();
    if (layer == nullptr || page == nullptr) return;
    Json op{{"op", "trace_edit"}, {"page", page->index.json()}, {"layer_id", layer->id}, {"action", mode.toStdString()}, {"points", points},
            {"radius_mm", core::py_round(canvas_->vector_radius_mm, 2)}};
    if (mode == QLatin1String("widen") || mode == QLatin1String("narrow")) op["amount"] = vector_amount_->value() / 100.0;
    if (mode == QLatin1String("join")) op["join_mm"] = vector_join_->value();
    apply_ops(Json::array({op}));
    canvas_->update();
}

void MainWindow::vector_simplify() {
    const std::vector<std::string> ids = canvas_->vector_ids;
    if (ids.empty()) {
        flash(QStringLiteral("先に「線の編集」（Shift+Y）で線を選びます"), 3000);
        return;
    }
    for (const std::string& id : ids) vector_edit(Json{{"action", "simplify"}, {"stroke_id", id}});
}

void MainWindow::vector_selected(const std::string& what) {
    const std::vector<std::string> ids = canvas_->vector_ids;
    if (ids.empty()) {
        flash(QStringLiteral("先に「線の編集」（Shift+Y）で線を選びます"), 3000);
        return;
    }
    if (what == "connect" && ids.size() != 2) {
        flash(QStringLiteral("つなぐ線を 2 本選びます（2 本目は Shift+クリック）"), 3000);
        return;
    }
    Json list = Json::array();
    for (const std::string& id : ids) list.push_back(id);
    Json change{{"action", what}, {"ids", list}};
    if (what == "recolor") change["rgb"] = pen_.rgb;
    vector_edit(change);
    if (what == "connect") canvas_->vector_ids = {ids.front()};
    if (what == "delete") canvas_->vector_ids.clear();
}

void MainWindow::reshaped(const QString& stroke_id, const Json& points) {
    const core::Layer* layer = paint_layer();
    if (layer == nullptr) return;
    apply_ops(Json::array({Json{{"op", "reshape_stroke"}, {"page", current_page()->index.json()}, {"layer_id", layer->id},
                                {"stroke_id", stroke_id.toStdString()}, {"points", points}}}));
}

}  // namespace genko::app
