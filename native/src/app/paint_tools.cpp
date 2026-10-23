// 画素を変える筆 (PC4 E5, SPEC.md COLOR-09「画素を変える筆」): ぼかしツール・シャープツール・指先ツール (the ぼかし group, beside
// Genko's 色混ぜ), 覆い焼きツール・焼き込みツール・スポンジツール (覆い焼き), 混合ブラシツール・色の置き換えツール (ブラシ) and
// 背景消しゴムツール (消しゴム) in the window and on the canvas — the tools by Photoshop's names in their groups
// (app/paint_panels.cpp), their settings (ツールの設定: Photoshop's options bar), a stroke made their op
// (render/ops_paint_tools), asked first when the layer's lines and pictures would become its pixels, and the Mixer Brush's
// paint kept between strokes (補充・クリーン). As the retouching tools (app/retouch_tools.cpp): the stroke is drawn as a line
// while it is made and the op sent when it ends.

#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

#include "app/ask.hpp"
#include "app/canvas.hpp"
#include "app/colours.hpp"
#include "app/fields.hpp"
#include "app/main_window.hpp"
#include "app/theme.hpp"
#include "app/tool_settings.hpp"
#include "render/color_canvas.hpp"
#include "render/shape_parts.hpp"

namespace genko::app {

using core::Json;

namespace {

// Each tool: its command's id, the canvas's tool name, its name (Photoshop's), what it does, its op.
struct PixelTool {
    const char* attribute;
    const char* name;
    const char* title;
    const char* tip;
    const char* op;
};

const std::vector<PixelTool>& pixel_tools() {
    static const std::vector<PixelTool> tools{
        {"act_blur_tool", "blurtool", "ぼかしツール", "なぞった所の画素をぼかします（描く先のペイントのレイヤー。選択範囲があればその中だけ）", "blur_brush"},
        {"act_sharpen_tool", "sharpentool", "シャープツール", "なぞった所の画素をくっきりさせます", "sharpen_brush"},
        {"act_smudge_tool", "smudgetool", "指先ツール", "なぞった向きに色をのばします（フィンガーペイントでは描画色から）", "smudge_brush"},
        {"act_dodge_tool", "dodge", "覆い焼きツール", "なぞった所を明るくします（範囲・露光量）", "dodge"},
        {"act_burn_tool", "burn", "焼き込みツール", "なぞった所を暗くします（範囲・露光量）", "burn"},
        {"act_sponge_tool", "sponge", "スポンジツール", "なぞった所の彩度を下げる・上げる", "sponge"},
        {"act_mixer_brush", "mixer", "混合ブラシツール", "描画色の絵の具とカンバスの色を混ぜながら描きます（ウェット・補充量・ミックス）", "mixer_brush"},
        {"act_background_eraser", "bgeraser", "背景消しゴムツール", "ブラシの中心の色に近い所を透明にします（サンプル・制限・許容値）", "background_eraser"},
        {"act_color_replacement", "colorreplace", "色の置き換えツール", "ブラシの中心の色に近い所の色を、描画色に置き換えます（モード・サンプル・制限・許容値）",
         "color_replacement"},
    };
    return tools;
}

const PixelTool* tool_of(const QString& name) {
    for (const PixelTool& t : pixel_tools()) {
        if (name == QLatin1String(t.name)) return &t;
    }
    return nullptr;
}

QLabel* hint(const QString& text) {
    auto* label = new QLabel(text);
    label->setWordWrap(true);
    theme::hint(label);
    return label;
}

QComboBox* choices(const char* name, const std::vector<std::pair<QString, QString>>& items, const std::string& value) {
    auto* box = new QComboBox;
    box->setObjectName(QString::fromLatin1(name));
    for (const auto& [label, key] : items) box->addItem(label, key);
    box->setCurrentIndex(std::max(0, box->findData(QString::fromStdString(value))));
    return box;
}

Json rgb_json(const Rgb& c) { return Json::array({c[0], c[1], c[2]}); }

}  // namespace

bool PageCanvas::is_pixel_tool(const QString& tool) { return tool_of(tool) != nullptr; }

void MainWindow::build_paint_tool_actions() {
    // (Photoshop's tools, without keys of their own: their groups' keys are the tools already there — B, E, G — and O is
    // the 長方形ツール; chosen from ツール, or a group's button held on the palette)
    for (const PixelTool& t : pixel_tools()) {
        const QString name = QString::fromLatin1(t.name);
        QAction* act = make(QString::fromLatin1(t.attribute), QString::fromUtf8(t.title), [this, name] { choose_tool(name); }, {}, QString::fromUtf8(t.tip), true);
        act->setAutoRepeat(false);  // (a held key chooses the tool once)
        tools_->addAction(act);
        tool_actions_[name] = act;
    }
}

void MainWindow::build_paint_tool_pages(ToolSettings* ts) {
    PixelToolSettings& s = pixel_tool_settings;
    auto* size = new QDoubleSpinBox;
    size->setObjectName(QStringLiteral("pixel_size"));
    size->setRange(0.2, 300);
    size->setSingleStep(0.5);
    size->setSuffix(QStringLiteral(" mm"));
    size->setValue(s.size_mm);
    connect(size, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        pixel_tool_settings.size_mm = v;
        canvas_->pixel_tool_mm = v;
        canvas_->update();
    });
    canvas_->pixel_tool_mm = s.size_mm;
    const auto percent = [this](const char* name, int lo, int hi, double value, double PixelToolSettings::*field) {
        auto* box = new QSpinBox;
        box->setObjectName(QString::fromLatin1(name));
        box->setRange(lo, hi);
        box->setSuffix(QStringLiteral(" %"));
        box->setValue(static_cast<int>(value));
        connect(box, &QSpinBox::valueChanged, this, [this, field](int v) { pixel_tool_settings.*field = v; });
        return box;
    };
    const auto check = [this](const char* name, const QString& label, bool value, bool PixelToolSettings::*field, const QString& tip = {}) {
        auto* box = new QCheckBox(label);
        box->setObjectName(QString::fromLatin1(name));
        box->setChecked(value);
        if (!tip.isEmpty()) box->setToolTip(tip);
        connect(box, &QCheckBox::toggled, this, [this, field](bool on) { pixel_tool_settings.*field = on; });
        return box;
    };
    const auto chosen = [this](QComboBox* box, std::string PixelToolSettings::*field) {
        connect(box, &QComboBox::currentIndexChanged, this, [this, box, field] { pixel_tool_settings.*field = box->currentData().toString().toStdString(); });
        return box;
    };
    QSpinBox* hardness = percent("pixel_hardness", 0, 100, s.hardness, &PixelToolSettings::hardness);
    QSpinBox* spacing = percent("pixel_spacing", 1, 1000, s.spacing, &PixelToolSettings::spacing);
    QSpinBox* strength = percent("pixel_strength", 1, 100, s.strength, &PixelToolSettings::strength);
    QCheckBox* detail = check("pixel_protect_detail", QStringLiteral("ディテールを保持"), s.protect_detail, &PixelToolSettings::protect_detail,
                              QStringLiteral("まわりより明るく・暗くはしない（輪郭のにじみ・ノイズを強めない）"));
    QCheckBox* finger = check("pixel_finger", QStringLiteral("フィンガーペイント"), s.finger_painting, &PixelToolSettings::finger_painting,
                              QStringLiteral("描画色をつけた指でなぞるように、描画色からのばし始める"));
    QComboBox* range = chosen(choices("pixel_range", {{QStringLiteral("シャドウ"), QStringLiteral("shadows")}, {QStringLiteral("中間調"), QStringLiteral("midtones")},
                                                      {QStringLiteral("ハイライト"), QStringLiteral("highlights")}},
                                      s.range),
                              &PixelToolSettings::range);
    QSpinBox* exposure = percent("pixel_exposure", 1, 100, s.exposure, &PixelToolSettings::exposure);
    QCheckBox* tones = check("pixel_protect_tones", QStringLiteral("トーンを保護"), s.protect_tones, &PixelToolSettings::protect_tones,
                             QStringLiteral("明るさだけを変え、色合いを保つ（白飛び・黒つぶれを抑える）"));
    QComboBox* sponge = chosen(choices("pixel_sponge_mode", {{QStringLiteral("彩度を下げる"), QStringLiteral("desaturate")}, {QStringLiteral("彩度を上げる"), QStringLiteral("saturate")}},
                                       s.sponge_mode),
                               &PixelToolSettings::sponge_mode);
    QSpinBox* sponge_flow = percent("pixel_sponge_flow", 1, 100, s.sponge_flow, &PixelToolSettings::sponge_flow);
    QCheckBox* vibrance = check("pixel_vibrance", QStringLiteral("自然な彩度"), s.vibrance, &PixelToolSettings::vibrance,
                                QStringLiteral("色があふれる所（もう鮮やかな色・ほとんど灰色の色）ほど弱く効かせる"));
    // the Mixer Brush: its paint, the buttons and the mixing
    mixer_paint_ = new QLabel;
    mixer_paint_->setObjectName(QStringLiteral("pixel_mixer_paint"));
    mixer_paint_->setMinimumHeight(18);
    mixer_paint_->setWordWrap(true);
    auto* load = new QPushButton(QStringLiteral("現在のブラシに補充"));
    load->setObjectName(QStringLiteral("pixel_mixer_load"));
    load->setToolTip(QStringLiteral("描画色の絵の具をいっぱいに補充します"));
    connect(load, &QPushButton::clicked, this, [this] { mixer_load(); });
    auto* clean = new QPushButton(QStringLiteral("ブラシをクリーン"));
    clean->setObjectName(QStringLiteral("pixel_mixer_clean"));
    clean->setToolTip(QStringLiteral("ブラシの絵の具を洗い落とします"));
    connect(clean, &QPushButton::clicked, this, [this] { mixer_clean(); });
    auto* buttons = new QWidget;
    auto* row = new QVBoxLayout(buttons);  // (one above the other: side by side they are wider than the narrow dock)
    row->setContentsMargins(0, 0, 0, 0);
    row->addWidget(load);
    row->addWidget(clean);
    QCheckBox* load_after = check("pixel_load_after", QStringLiteral("ストローク後にブラシを補充"), s.load_after, &PixelToolSettings::load_after);
    QCheckBox* clean_after = check("pixel_clean_after", QStringLiteral("ストローク後にブラシをクリーン"), s.clean_after, &PixelToolSettings::clean_after);
    QSpinBox* wet = percent("pixel_wet", 0, 100, s.wet, &PixelToolSettings::wet);
    QSpinBox* loaded = percent("pixel_load", 0, 100, s.load, &PixelToolSettings::load);
    QSpinBox* mix = percent("pixel_mix", 0, 100, s.mix, &PixelToolSettings::mix);
    QSpinBox* mixer_flow = percent("pixel_mixer_flow", 0, 100, s.mixer_flow, &PixelToolSettings::mixer_flow);
    wet->setToolTip(QStringLiteral("カンバスから絵の具を取り込む量（0 % で乾いたブラシ: 補充した色だけで描く）"));
    loaded->setToolTip(QStringLiteral("補充した絵の具の量（少ないと、なぞるうちに乾いてかすれる）"));
    mix->setToolTip(QStringLiteral("補充した絵の具とカンバスの絵の具の割合（100 % でカンバスの色だけ）"));
    QCheckBox* sample_all = check("pixel_sample_all", QStringLiteral("全レイヤーを対象"), s.sample_all, &PixelToolSettings::sample_all,
                                  QStringLiteral("見えているとおりのページから絵の具を取り込む（オフ: 描く先のレイヤーから）"));
    // the Background Eraser and Color Replacement
    QComboBox* sampling = chosen(choices("pixel_sampling", {{QStringLiteral("連続"), QStringLiteral("continuous")}, {QStringLiteral("一度"), QStringLiteral("once")},
                                                            {QStringLiteral("背景のスウォッチ"), QStringLiteral("swatch")}},
                                         s.sampling),
                                 &PixelToolSettings::sampling);
    sampling->setToolTip(QStringLiteral("連続: ブラシの中心の色を拾い続ける・一度: 描き始めの色だけ・背景のスウォッチ: 背景色"));
    QComboBox* limits = chosen(choices("pixel_limits", {{QStringLiteral("隣接されていない"), QStringLiteral("discontiguous")}, {QStringLiteral("隣接"), QStringLiteral("contiguous")},
                                                        {QStringLiteral("輪郭検出"), QStringLiteral("find_edges")}},
                                       s.limits),
                               &PixelToolSettings::limits);
    QSpinBox* eraser_tolerance = percent("pixel_eraser_tolerance", 0, 100, s.eraser_tolerance, &PixelToolSettings::eraser_tolerance);
    QCheckBox* protect_fg = check("pixel_protect_foreground", QStringLiteral("描画色を保護"), s.protect_foreground, &PixelToolSettings::protect_foreground,
                                  QStringLiteral("描画色に近い所は消さない"));
    QComboBox* replace = chosen(choices("pixel_replace_mode", {{QStringLiteral("色相"), QStringLiteral("hue")}, {QStringLiteral("彩度"), QStringLiteral("saturation")},
                                                               {QStringLiteral("カラー"), QStringLiteral("color")}, {QStringLiteral("輝度"), QStringLiteral("luminosity")}},
                                        s.replace_mode),
                                &PixelToolSettings::replace_mode);
    QSpinBox* replace_tolerance = percent("pixel_replace_tolerance", 0, 100, s.replace_tolerance, &PixelToolSettings::replace_tolerance);
    QCheckBox* antialias = check("pixel_antialias", QStringLiteral("アンチエイリアス"), s.antialias, &PixelToolSettings::antialias,
                                 QStringLiteral("許容値の境目をなめらかにする"));
    // (the fields are shared by the nine tools' page: each tool shows its own rows — ToolSettings shows the tool in hand's)
    auto* widget = new QWidget;
    widget->setObjectName(QStringLiteral("pixel_tools_page"));
    auto* form = new QFormLayout(widget);
    form->setContentsMargins(0, 0, 0, 0);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->addRow(QStringLiteral("直径"), slider_for(size, true));
    form->addRow(QStringLiteral("硬さ"), hardness);
    form->addRow(QStringLiteral("間隔"), spacing);
    form->addRow(QStringLiteral("強さ"), strength);
    form->addRow(QString(), detail);
    form->addRow(QString(), finger);
    form->addRow(QStringLiteral("範囲"), range);
    form->addRow(QStringLiteral("露光量"), exposure);
    form->addRow(QString(), tones);
    form->addRow(QStringLiteral("モード"), sponge);
    form->addRow(QStringLiteral("流量"), sponge_flow);
    form->addRow(QString(), vibrance);
    form->addRow(QStringLiteral("絵の具"), mixer_paint_);
    form->addRow(QString(), buttons);
    form->addRow(QString(), load_after);
    form->addRow(QString(), clean_after);
    form->addRow(QStringLiteral("ウェット"), wet);
    form->addRow(QStringLiteral("補充量"), loaded);
    form->addRow(QStringLiteral("ミックス"), mix);
    form->addRow(QStringLiteral("流量"), mixer_flow);
    form->addRow(QString(), sample_all);
    form->addRow(QStringLiteral("モード"), replace);
    form->addRow(QStringLiteral("サンプル"), sampling);
    form->addRow(QStringLiteral("制限"), limits);
    form->addRow(QStringLiteral("許容値"), eraser_tolerance);
    form->addRow(QStringLiteral("許容値"), replace_tolerance);
    form->addRow(QString(), protect_fg);
    form->addRow(QString(), antialias);
    QLabel* note = hint(QString());
    form->addRow(note);
    QLabel* paint_label = mixer_paint_;
    const auto show_for = [=](const QString& tool) {
        const bool focus = tool == QLatin1String("blurtool") || tool == QLatin1String("sharpentool");
        const bool toning = tool == QLatin1String("dodge") || tool == QLatin1String("burn");
        const bool mixer = tool == QLatin1String("mixer"), eraser = tool == QLatin1String("bgeraser"), replacing = tool == QLatin1String("colorreplace");
        form->setRowVisible(strength, focus || tool == QLatin1String("smudgetool"));
        form->setRowVisible(detail, tool == QLatin1String("sharpentool"));
        form->setRowVisible(finger, tool == QLatin1String("smudgetool"));
        form->setRowVisible(range, toning);
        form->setRowVisible(exposure, toning);
        form->setRowVisible(tones, toning);
        form->setRowVisible(sponge, tool == QLatin1String("sponge"));
        form->setRowVisible(sponge_flow, tool == QLatin1String("sponge"));
        form->setRowVisible(vibrance, tool == QLatin1String("sponge"));
        for (QWidget* w : std::initializer_list<QWidget*>{paint_label, buttons, load_after, clean_after, wet, loaded, mix, mixer_flow, sample_all})
            form->setRowVisible(w, mixer);
        form->setRowVisible(replace, replacing);
        form->setRowVisible(sampling, eraser || replacing);
        form->setRowVisible(limits, eraser || replacing);
        form->setRowVisible(eraser_tolerance, eraser);
        form->setRowVisible(replace_tolerance, replacing);
        form->setRowVisible(protect_fg, eraser);
        form->setRowVisible(antialias, replacing);
        note->setText(mixer    ? QStringLiteral("補充した描画色の絵の具に、カンバスの色を取り込んで混ぜながら描きます。選択範囲があればその中だけ")
                      : eraser ? QStringLiteral("ブラシの中心（＋）の色に近い所を透明にします。透明部分の保護をしたレイヤーには使えません")
                      : replacing ? QStringLiteral("ブラシの中心の色に近い所の色を、描画色の色相・彩度・カラー・輝度に置き換えます")
                                  : QStringLiteral("描く先のペイントのレイヤーの画素を変えます（線や絵は、はじめに画素になります）。選択範囲があればその中だけ"));
    };
    for (const PixelTool& t : pixel_tools()) {
        const QString name = QString::fromLatin1(t.name);
        connect(tool_actions_.at(name), &QAction::toggled, widget, [show_for, name](bool on) {
            if (on) show_for(name);
        });
    }
    show_for(QStringLiteral("blurtool"));
    std::vector<QString> names;
    for (const PixelTool& t : pixel_tools()) names.push_back(QString::fromLatin1(t.name));
    ts->add(names, widget);
    mixer_shown();
}

bool MainWindow::pixel_tool_bakes_ok(const core::Page& page, const core::Layer& layer, const QString& tool) {
    const std::size_t lines = layer.stroke_count();
    const std::size_t pictures = layer.patches.size();
    const bool parts = render::shape_parts::any(layer);
    if (lines == 0 && pictures == 0 && !parts) return true;
    const std::string key = page.id + "/" + layer.id;
    if (blend_baked_.contains(key)) return true;  // (asked once a layer while the window is open, as a blend's)
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("pixel_tool_bakes"));
    dialog.setWindowTitle(tool);
    auto* layout = new QVBoxLayout(&dialog);
    QStringList what;
    if (lines > 0) what << QStringLiteral("線 %1 本").arg(lines);
    if (pictures > 0) what << QStringLiteral("絵 %1 枚").arg(pictures);
    if (parts) what << QStringLiteral("シェイプ");
    const QString which = layer.kind == core::LayerKind::Strokes ? QStringLiteral("このペンのレイヤーの") : QStringLiteral("このレイヤーの");
    auto* text = new QLabel(QStringLiteral("%1%2はペイントのレイヤーの画素になり、線としては直せなくなります（取り消しで戻せます）。\n「%3」で描きますか？")
                                .arg(which, what.join(QStringLiteral("と")), tool));
    text->setWordWrap(true);
    layout->addWidget(text);
    auto* buttons = new QDialogButtonBox;
    buttons->addButton(QStringLiteral("描く"), QDialogButtonBox::AcceptRole);
    buttons->addButton(QStringLiteral("やめる"), QDialogButtonBox::RejectRole);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (ask::exec(&dialog) != QDialog::Accepted) return false;
    blend_baked_.insert(key);
    return true;
}

bool MainWindow::pixel_tool_stroke(const StrokeInput& stroke, const core::Page& page, const core::Layer& layer) {
    const PixelTool* tool = tool_of(stroke.tool);
    if (tool == nullptr || stroke.points.empty()) return false;
    canvas_->stroke_dropped();
    const PixelToolSettings& s = pixel_tool_settings;
    // (a mask or a channel painted is lent to the op as a picture of its own: the layer's lines stay as they are)
    const bool on_channel = !channel_view().target.empty();
    const bool on_mask = mask_edit && !on_channel;
    if (!on_mask && !on_channel && !pixel_tool_bakes_ok(page, layer, QString::fromUtf8(tool->title))) return true;
    // (the pressure does not change the brush, as Photoshop's retouching tools by default: each point at full size)
    Json points = Json::array();
    for (const core::PenPoint& p : stroke.points) points.push_back(Json::array({p.x, p.y}));
    const std::string op_name = tool->op;
    Json op{{"op", op_name}, {"page", page.index.json()}, {"layer_id", layer.id}, {"points", points},
            {"width_mm", s.size_mm}, {"hardness", s.hardness}, {"spacing", s.spacing}};
    if (on_mask) op["layer_mask"] = true;
    if (const auto area = selection_area()) op["area"] = *area;
    const Rgb fore = colours_ != nullptr ? colours_->rgb() : Rgb{20, 20, 20};
    const Rgb back = colours_ != nullptr ? colours_->sub_rgb() : Rgb{255, 255, 255};
    const bool deep = !page.color && page.rgb && page.rgb->depth != 8;
    // the 描画色 (with its precise values on a page of 16 or 32 bits)
    const auto foreground = [&](const char* key) {
        op[key] = rgb_json(fore);
        if (colours_ != nullptr && deep && colours_->is_precise()) op["color_rgb"] = colours_->foreground_json(page.rgb->depth == 32 ? "f32" : "u16");
    };
    if (op_name == "blur_brush" || op_name == "sharpen_brush" || op_name == "smudge_brush") op["strength"] = s.strength;
    if (op_name == "sharpen_brush") op["protect_detail"] = s.protect_detail;
    if (op_name == "smudge_brush") {
        op["finger_painting"] = s.finger_painting;
        if (s.finger_painting) foreground("rgb");
    }
    if (op_name == "dodge" || op_name == "burn") {
        op["range"] = s.range;
        op["exposure"] = s.exposure;
        op["protect_tones"] = s.protect_tones;
    }
    if (op_name == "sponge") {
        op["mode"] = s.sponge_mode;
        op["flow"] = s.sponge_flow;
        op["vibrance"] = s.vibrance;
    }
    if (op_name == "mixer_brush") {
        op["wet"] = s.wet;
        op["load"] = s.load;
        op["mix"] = s.mix;
        op["flow"] = s.mixer_flow;
        op["sample_all"] = s.sample_all;
        op["load_after"] = s.load_after;
        op["clean_after"] = s.clean_after;
        foreground("rgb");
        if (!mixer_reservoir.is_null()) op["reservoir"] = mixer_reservoir;
    }
    if (op_name == "background_eraser") {
        op["sampling"] = s.sampling;
        op["limits"] = s.limits;
        op["tolerance"] = s.eraser_tolerance;
        op["protect_foreground"] = s.protect_foreground;
        op["foreground_rgb"] = rgb_json(fore);
        op["swatch_rgb"] = rgb_json(back);
    }
    if (op_name == "color_replacement") {
        op["mode"] = s.replace_mode;
        op["sampling"] = s.sampling;
        op["limits"] = s.limits;
        op["tolerance"] = s.replace_tolerance;
        op["antialias"] = s.antialias;
        op["swatch_rgb"] = rgb_json(back);
        foreground("rgb");
    }
    const bool done = apply_ops(Json::array({op}));
    if (done && op_name == "mixer_brush") {
        // (the paint the stroke left, for the next: what the op reported)
        for (const Json& item : last_results_) {
            if (item.is_object() && item.value("op", std::string()) == "mixer_brush" && item.contains("reservoir")) mixer_reservoir = item["reservoir"];
        }
        mixer_shown();
    }
    return true;
}

void MainWindow::mixer_load() {
    const Rgb fore = colours_ != nullptr ? colours_->rgb() : Rgb{20, 20, 20};
    // (as the op loads it: the 描画色 full, as the colour formulas see the page — an RGB page of a profile's values)
    double rgb[3] = {fore[0] / 255.0, fore[1] / 255.0, fore[2] / 255.0};
    if (const core::Page* page = current_page(); page != nullptr && !page->color) render::RgbSpace::of(*page).from_srgb(rgb, 1);
    const Json pickup = mixer_reservoir.is_object() && mixer_reservoir.contains("pickup") ? mixer_reservoir["pickup"] : Json(nullptr);
    mixer_reservoir = Json{{"paint", Json::array({rgb[0], rgb[1], rgb[2], 1.0})}, {"amount", 1.0}, {"pickup", pickup}};
    mixer_shown();
}

void MainWindow::mixer_clean() {
    mixer_reservoir = nullptr;
    mixer_shown();
}

void MainWindow::mixer_shown() {
    if (mixer_paint_ == nullptr) return;
    const auto colour_of = [](const Json& v) -> std::optional<QColor> {
        if (!v.is_array() || v.size() != 4) return std::nullopt;
        const auto c = [&](std::size_t k) { return std::clamp(static_cast<int>(std::lround(v[k].get<double>() * 255.0)), 0, 255); };
        return QColor(c(0), c(1), c(2));
    };
    const std::optional<QColor> paint = mixer_reservoir.is_object() ? colour_of(mixer_reservoir.value("paint", Json())) : std::nullopt;
    const std::optional<QColor> pickup = mixer_reservoir.is_object() ? colour_of(mixer_reservoir.value("pickup", Json())) : std::nullopt;
    if (!paint && !pickup) {
        mixer_paint_->setText(QStringLiteral("（きれい: 絵の具なし）"));
        mixer_paint_->setStyleSheet(QString());
        return;
    }
    const double amount = mixer_reservoir.value("amount", 0.0);
    QString text;
    if (paint) text += QStringLiteral("<span style='background:%1'>&nbsp;&nbsp;&nbsp;&nbsp;</span> 補充 %2 %").arg(paint->name()).arg(std::lround(amount * 100));
    if (pickup) text += QStringLiteral("　<span style='background:%1'>&nbsp;&nbsp;&nbsp;&nbsp;</span> 取り込んだ色").arg(pickup->name());
    mixer_paint_->setText(text);
}

}  // namespace genko::app
