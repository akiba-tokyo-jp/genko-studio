// The window's brushes and tool settings (Python's genko/app/main.py: the ツールの設定 dock and its pages,
// _brush_changed, _make_brush, _edit_brush, _forget_brush, _export_brush, import_brushes, _import_brushes_dialog,
// _eraser_size): the brush panel drives the pen; one's own brushes are made, opened again, forgotten, given to a file
// and taken from one (.genkobrush, Photoshop's .abr), and kept in the config folder.

#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>

#include <filesystem>
#include <fstream>
#include <iterator>

#include "app/ask.hpp"
#include "app/brush_panel.hpp"
#include "app/canvas.hpp"
#include "app/config.hpp"
#include "app/fields.hpp"
#include "app/main_window.hpp"
#include "app/material_tabs.hpp"
#include "app/theme.hpp"
#include "app/tool_settings.hpp"
#include "app/wording.hpp"
#include "core/brushes.hpp"
#include "core/ids.hpp"
#include "core/paths.hpp"
#include "render/abr.hpp"
#include "render/brushes.hpp"
#include "storage/fsutil.hpp"

namespace genko::app {

using core::Json;

namespace {

QLabel* note(const QString& text) {
    auto* label = new QLabel(text);
    label->setWordWrap(true);
    theme::hint(label);
    return label;
}

std::filesystem::path path_of(const QString& text) { return core::path_from_utf8(text.toStdString()); }

}  // namespace

void MainWindow::build_tool_settings() {
    brush_ = new BrushPanel;
    connect(brush_, &BrushPanel::changed, this, [this] { brush_changed(); });
    connect(brush_->make, &QPushButton::clicked, this, [this] { make_brush(); });
    connect(brush_->edit, &QPushButton::clicked, this, [this] { edit_brush(); });
    connect(brush_->forget, &QPushButton::clicked, this, [this] { forget_brush(); });
    auto* brush_files = new QMenu(brush_->files);
    brush_files->addAction(QStringLiteral("選んでいるブラシをファイルに書き出す…"), this, [this] { export_brush(); });
    brush_files->addAction(QStringLiteral("ブラシを読み込む（.genkobrush・.abr）…"), this, [this] { import_brushes_dialog(); });
    brush_->files->setMenu(brush_files);
    pen_ = brush_->pen();

    tool_settings_ = new ToolSettings;
    ToolSettings* ts = tool_settings_;
    ts->add({QStringLiteral("pen"), QStringLiteral("fill"), QStringLiteral("lassofill"), QStringLiteral("picker")}, brush_);
    // the eraser
    eraser_size_ = new QDoubleSpinBox;
    eraser_size_->setObjectName(QStringLiteral("eraser_size"));
    eraser_size_->setRange(0.2, 50);
    eraser_size_->setSingleStep(0.5);
    eraser_size_->setSuffix(QStringLiteral(" mm"));
    eraser_size_->setValue(eraser_mm_);
    connect(eraser_size_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        eraser_mm_ = value;
        canvas_->eraser_mm = eraser_mm_;
        canvas_->update();
    });
    auto* eraser_page = new QWidget;
    auto* eraser_form = new QFormLayout(eraser_page);
    eraser_form->setRowWrapPolicy(QFormLayout::WrapAllRows);
    eraser_form->setContentsMargins(0, 0, 0, 0);
    eraser_form->addRow(QStringLiteral("消しゴムの太さ（[ ] でも変わる）"), slider_for(eraser_size_, true));
    eraser_form->addRow(brush_->crossing);
    eraser_mode_ = new QComboBox;
    eraser_mode_->setObjectName(QStringLiteral("eraser_mode"));
    for (const auto& [label, key] : {std::pair{QStringLiteral("触れた所で切る"), QString()}, {QStringLiteral("交点まで"), QStringLiteral("to_crossing")},
                                     {QStringLiteral("線全体"), QStringLiteral("whole")}})
        eraser_mode_->addItem(label, key);
    eraser_mode_->setToolTip(QStringLiteral("線全体: 触れた線を丸ごと消す（ベクター）"));
    eraser_form->addRow(QStringLiteral("消し方"), eraser_mode_);
    eraser_texture_ = new QComboBox;
    eraser_texture_->setObjectName(QStringLiteral("eraser_texture"));
    for (const auto& [label, key] : {std::pair{QStringLiteral("硬め"), QStringLiteral("hard")}, {QStringLiteral("軟らかめ（縁がぼける）"), QStringLiteral("soft")},
                                     {QStringLiteral("粗め（ざらつく）"), QStringLiteral("rough")}})
        eraser_texture_->addItem(label, key);
    eraser_texture_->setToolTip(QStringLiteral("ペイントのレイヤーの消え方。ペンの線（ベクター）は、どれでもその所で切れる"));
    eraser_form->addRow(QStringLiteral("消しゴムの質"), eraser_texture_);
    eraser_balloons_ = new QCheckBox(QStringLiteral("フキダシを削る"));
    eraser_balloons_->setObjectName(QStringLiteral("eraser_balloons"));
    eraser_balloons_->setToolTip(QStringLiteral("消しゴムでなぞった所の、台詞のフキダシ（中と線）を削ります。絵は消しません"));
    eraser_form->addRow(eraser_balloons_);
    eraser_form->addRow(note(QStringLiteral("定規への吸着（表示メニュー）がオンなら、消しゴムも定規に沿って消します")));
    eraser_form->addRow(note(QStringLiteral("トーンのレイヤーでは削ります（ぼかすかは素材パネルのトーンの欄で）")));
    ts->add({QStringLiteral("eraser")}, eraser_page);
    // the panel tool
    frame_mode_ = new QComboBox;
    frame_mode_->setObjectName(QStringLiteral("frame_mode"));
    for (const auto& [label, key] : {std::pair{QStringLiteral("ドラッグで割る"), QStringLiteral("cut")}, {QStringLiteral("長方形を描く"), QStringLiteral("rect")},
                                     {QStringLiteral("折れ線で描く"), QStringLiteral("poly")}, {QStringLiteral("フリーハンドで描く"), QStringLiteral("free")}})
        frame_mode_->addItem(label, key);
    frame_mode_->setToolTip(QStringLiteral("描く: 空いた所に新しいコマを描きます（折れ線は角をクリック、最初の角のクリック・Enter・"
                                           "ダブルクリックで閉じる）。最初に描いたコマは基本枠と入れ替わります"));
    connect(frame_mode_, &QComboBox::activated, this, [this](int) { canvas_->frame_mode = frame_mode_->currentData().toString(); });
    frame_mode_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    frame_mode_->setMinimumContentsLength(6);
    std::vector<QAction*> kinds = border_kind_actions_;
    ts->add({QStringLiteral("frame")},
            action_page({QStringLiteral("作り方"), static_cast<QWidget*>(frame_mode_), QStringLiteral("割る"), action("act_split_h"), action("act_split_v"),
                         action("act_merge"), action("act_delete_frame"), action("act_frame_selection"), action("act_template"),
                         action("act_save_template"), action("act_gutters"), QStringLiteral("枠線"), action("act_border"), action("act_no_border"),
                         static_cast<QWidget*>(menu_button(QStringLiteral("枠線の種類・色"), {kinds, {action("act_border_colour")}})), action("act_corner"),
                         QStringLiteral("形"), action("act_bleed"), action("act_reset_shape"), action("act_frame_numbers"),
                         QStringLiteral("原稿"), action("act_paper"),
                         static_cast<QWidget*>(note(QStringLiteral("コマを選ぶと、辺の中ほどの ◇ をドラッグで辺を曲げられます（外へふくらむ・内へへこむ）。")))}));
    // the selection
    marquee_mode_ = new QComboBox;
    marquee_mode_->setObjectName(QStringLiteral("marquee_mode"));
    for (const auto& [label, key] : {std::pair{QStringLiteral("長方形"), QStringLiteral("rect")}, {QStringLiteral("楕円"), QStringLiteral("ellipse")},
                                     {QStringLiteral("投げ縄"), QStringLiteral("lasso")}, {QStringLiteral("折れ線"), QStringLiteral("polyline")},
                                     {QStringLiteral("自動選択"), QStringLiteral("wand")}, {QStringLiteral("色域選択"), QStringLiteral("colour")},
                                     {QStringLiteral("選択ペン"), QStringLiteral("selpen")}, {QStringLiteral("選択消し"), QStringLiteral("selerase")}})
        marquee_mode_->addItem(label, key);
    connect(marquee_mode_, &QComboBox::activated, this, [this](int) { choose_tool(marquee_mode_->currentData().toString()); });
    selection_pen_ = new QDoubleSpinBox;
    selection_pen_->setObjectName(QStringLiteral("selection_pen"));
    selection_pen_->setRange(0.2, 60);
    selection_pen_->setSuffix(QStringLiteral(" mm"));
    selection_pen_->setValue(canvas_->selection_pen_mm);
    connect(selection_pen_, &QDoubleSpinBox::valueChanged, this, [this](double v) { canvas_->selection_pen_mm = v; });
    colour_tolerance_box_ = new QSpinBox;
    colour_tolerance_box_->setObjectName(QStringLiteral("colour_tolerance"));
    colour_tolerance_box_->setRange(0, 255);
    colour_tolerance_box_->setValue(static_cast<int>(colour_tolerance));
    colour_tolerance_box_->setToolTip(QStringLiteral("色域選択: どれだけ違う色まで同じとみなすか"));
    connect(colour_tolerance_box_, &QSpinBox::valueChanged, this, [this](int v) { colour_tolerance = v; });
    colour_contiguous_box_ = new QCheckBox(QStringLiteral("隣り合う所だけ"));
    colour_contiguous_box_->setObjectName(QStringLiteral("colour_contiguous"));
    connect(colour_contiguous_box_, &QCheckBox::toggled, this, [this](bool on) { colour_contiguous = on; });
    auto* sel_form = new QWidget;
    auto* sfl = new QFormLayout(sel_form);
    sfl->setContentsMargins(0, 0, 0, 0);
    sfl->setRowWrapPolicy(QFormLayout::WrapLongRows);
    sfl->addRow(QStringLiteral("選び方"), marquee_mode_);
    sfl->addRow(QStringLiteral("選択ペンの太さ"), selection_pen_);
    sfl->addRow(QStringLiteral("色域の幅"), colour_tolerance_box_);
    sfl->addRow(QString(), colour_contiguous_box_);
    ts->add({QStringLiteral("marquee")},
            action_page({sel_form, static_cast<QWidget*>(note(QStringLiteral("Shift で足す・Alt で引く・両方で重なりだけ"))), QStringLiteral("選択範囲と中身"),
                         static_cast<QWidget*>(menu_button(QStringLiteral("選択範囲"),
                                                           {{action("act_select_all"), action("act_deselect"), action("act_sel_invert")},
                                                            {action("act_sel_grow"), action("act_sel_shrink"), action("act_sel_feather"), action("act_sel_layer")},
                                                            {action("act_sel_keep"), action("act_quick_mask")}})),
                         static_cast<QWidget*>(menu_button(QStringLiteral("中身"),
                                                           {{action("act_copy"), action("act_cut"), action("act_paste"), action("act_delete_area")},
                                                            {action("act_flip_h"), action("act_flip_v"), action("act_warp_perspective"), action("act_warp_mesh"),
                                                             action("act_warp_apply")},
                                                            {action("act_fill_selection"), action("act_line_width"), action("act_tone_here")}}))}));
    ts->add({QStringLiteral("text")}, text_settings_);  // (the text tool: main_window_lines.cpp)
    ts->add({QStringLiteral("select")}, line_select_page());  // (the chosen line's lettering, then the view)
    ts->add({QStringLiteral("move")}, action_page({QStringLiteral("レイヤー"), action("act_layer_dup"), action("act_select_all")}));
    build_paint_pages(ts);  // (図形・色混ぜ・ゆがみ・グラデーション: main_window_paint.cpp)
    build_vector_pages(ts);  // (線の編集・線の修正: main_window_vector.cpp)
    build_guide_pages(ts);   // (定規・3D: main_window_guides.cpp)
    build_material_pages(ts);  // (効果線・素材を置く: main_window_materials.cpp)

    auto* dock = new QDockWidget(QStringLiteral("ツールの設定"), this);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(ts);
    scroll->setMinimumHeight(24);  // (on a short screen it scrolls, and the window stays on the screen)
    dock->setWidget(scroll);
    dock->setObjectName(QStringLiteral("ツールの設定"));
    dock->setTitleBarWidget(new QWidget);  // (the tool's own name heads the panel; no second title above it)
    dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    addDockWidget(Qt::LeftDockWidgetArea, dock);
    view_menu_->addAction(dock->toggleViewAction());
    tool_settings_dock_ = dock;
    ts->show_tool(canvas_->tool());
}

void MainWindow::brush_changed() {
    pen_ = brush_->pen();
    pen_changed();
    canvas_->update();
}

void MainWindow::make_brush() {
    BrushDialog dialog(this, brush_->kind());
    if (ask::exec(&dialog) != QDialog::Accepted) return;
    const std::string key = "my_" + core::new_id();
    const Json data = dialog.data();
    try {
        render::brushes::define_brush(key, data);
        render::brushes::save_to_library(config_dir(), key, core::brush_to_dict(render::brushes::brush(key)));
    } catch (const std::exception& error) {
        flash(wording::error(QString::fromUtf8(error.what())), 6000, true);
        return;
    }
    brush_->reload_kinds(key);
    flash(QStringLiteral("ブラシ「%1」を作りました（ブラシの一覧の ★）").arg(QString::fromStdString(data["label"].get<std::string>())), 4000);
}

void MainWindow::edit_brush() {
    // サブツール詳細: one of the person's own brushes opened again, every setting, and kept under its name
    const std::string key = brush_->kind();
    if (!key.starts_with("my_")) {
        flash(QStringLiteral("直せるのは自分のブラシ（★）です。元のブラシは「複製して調整…」で自分のブラシにしてから"), 5000);
        return;
    }
    Json kept;
    try {
        kept = render::brushes::load_library(config_dir()).value(key, Json());
    } catch (const std::exception&) {
    }
    if (!kept.is_object()) kept = core::brush_to_dict(render::brushes::brush(key));
    BrushDialog dialog(this, key, true);
    dialog.name->setText(QString::fromStdString(kept.contains("label") && kept["label"].is_string() && !kept["label"].get<std::string>().empty()
                                                    ? kept["label"].get<std::string>()
                                                    : render::brushes::brush(key).label));
    if (ask::exec(&dialog) != QDialog::Accepted) return;
    Json data = dialog.data();
    if (kept.contains("base") && kept["base"].is_string() && !kept["base"].get<std::string>().empty()) data["base"] = kept["base"];
    try {
        render::brushes::define_brush(key, data);
        render::brushes::save_to_library(config_dir(), key, core::brush_to_dict(render::brushes::brush(key)));
    } catch (const std::exception& error) {
        flash(wording::error(QString::fromUtf8(error.what())), 6000, true);
        return;
    }
    brush_->reload_kinds(key);
    flash(QStringLiteral("ブラシ「%1」を直しました（これから描く線に効きます）").arg(QString::fromStdString(data["label"].get<std::string>())), 4000);
}

void MainWindow::forget_brush() {
    const std::string key = brush_->kind();
    if (!key.starts_with("my_")) return;
    try {
        render::brushes::save_to_library(config_dir(), key, std::nullopt);
    } catch (const std::exception& error) {
        flash(wording::error(QString::fromUtf8(error.what())), 6000, true);
        return;
    }
    if (!book().brush_custom.contains(key)) render::brushes::forget_brush(key);
    brush_->reload_kinds(std::string(core::kDefaultBrush));
    flash(QStringLiteral("自作のブラシを一覧から消しました（描いた線はそのまま）"), 4000);
}

bool MainWindow::export_brush(std::optional<QString> path) {
    // the brush in a file (.genkobrush) to give to someone else or keep
    const std::string key = brush_->kind();
    const core::Brush brush = render::brushes::brush(key);
    if (!path) {
        path = ask::save_path(this, QStringLiteral("ブラシを書き出す"), QString::fromStdString(brush.label) + QStringLiteral(".genkobrush"),
                              QStringLiteral("Genko のブラシ (*.genkobrush)"));
        if (path->isEmpty()) return false;
    }
    Json definition = core::brush_to_dict(brush);
    definition["base"] = core::find_builtin(key) != nullptr ? key : std::string(core::kDefaultBrush);
    try {
        definition = render::brushes::with_paper_picture(definition);  // (a paper goes with its picture: the file is enough)
    } catch (const std::exception&) {
        flash(QStringLiteral("書き出せませんでした: このブラシの紙質の画像が見つかりません"), 5000, true);
        return false;
    }
    core::DumpOptions options;
    options.indent = 1;
    options.item_separator = ",";
    const std::string text = core::dump(Json{{"genko_brush", 1}, {"brushes", Json{{key, definition}}}}, options);
    try {
        storage::write_atomic(path_of(*path), text);  // (the whole file or none: never a cut-off one reported written)
    } catch (const std::exception&) {
        flash(QStringLiteral("書き出せませんでした: %1").arg(*path), 5000, true);
        return false;
    }
    flash(QStringLiteral("ブラシを書き出しました: %1").arg(QFileInfo(*path).fileName()), 3000);
    return true;
}

std::vector<std::string> MainWindow::import_brushes(const QString& path) {
    // brushes from a .genkobrush or a Photoshop .abr into one's own list (core::Error and OSError-like failures throw)
    const std::filesystem::path file_path = path_of(path);
    std::ifstream file(file_path, std::ios::binary);
    if (!file) throw core::PyUncaught("FileNotFoundError", "[Errno 2] No such file or directory: '" + path.toStdString() + "'");
    const std::string raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::vector<Json> definitions;
    if (QFileInfo(path).suffix().toLower() == QLatin1String("abr")) {
        definitions = render::abr::brushes_from(raw, QFileInfo(path).completeBaseName().toStdString() + " ");
    } else {
        if (const auto bad = core::utf8_error(raw)) throw core::PyValueError(*bad);
        const Json data = core::parse_python_json(raw);
        if (!data.is_object()) throw core::PyValueError("a brush file is an object with \"brushes\"");
        const Json brushes = data.value("brushes", Json::object());
        if (brushes.is_object())
            for (const auto& [name, definition] : brushes.items()) definitions.push_back(definition);
    }
    std::vector<std::string> keys;
    for (const Json& definition : definitions) {
        const std::string key = "my_" + core::new_id();
        try {
            render::brushes::define_brush(key, render::brushes::with_paper_taken_in(definition));
        } catch (const core::PyUncaught&) {
            throw;
        } catch (const core::Error&) {
            continue;  // (ValueError, TypeError: one that does not make sense is left out)
        }
        render::brushes::save_to_library(config_dir(), key, core::brush_to_dict(render::brushes::brush(key)));
        keys.push_back(key);
    }
    if (!keys.empty()) brush_->reload_kinds(keys.front());
    return keys;
}

void MainWindow::import_brushes_dialog() {
    const QString path = ask::open_path(this, QStringLiteral("ブラシを読み込む"), QStringLiteral("ブラシ (*.genkobrush *.abr)"));
    if (path.isEmpty()) return;
    std::vector<std::string> keys;
    try {
        keys = import_brushes(path);
    } catch (const std::exception& error) {
        flash(QStringLiteral("読み込めませんでした: %1").arg(wording::error(QString::fromUtf8(error.what()))), 5000, true);
        return;
    }
    flash(keys.empty() ? QStringLiteral("読み込めるブラシがありませんでした") : QStringLiteral("ブラシを %1 本読み込みました（一覧の ★）").arg(keys.size()), 4000);
}

Json MainWindow::eraser_fields(const core::Layer& layer) const {
    Json out = Json::object();
    if (layer.kind == core::LayerKind::Tone) {
        // (a tone layer is scraped: how soft is the materials panel's)
        if (materials_ != nullptr && materials_->soft->isChecked()) out["soft"] = true;
    } else if (eraser_mode_ != nullptr && !eraser_mode_->currentData().toString().isEmpty()) {
        out["mode"] = eraser_mode_->currentData().toString().toStdString();
    } else if (brush_ != nullptr && brush_->crossing->isChecked()) {
        out["mode"] = "to_crossing";
    }
    if (eraser_texture_ != nullptr && eraser_texture_->currentData().toString() != QLatin1String("hard"))
        out["texture"] = eraser_texture_->currentData().toString().toStdString();
    return out;
}

}  // namespace genko::app
