// PC3 B3 (SPEC.md COLOR-06「選択とマスク」, the user's decision 5: Photoshop's names): 選択範囲 → 選択とマスク… — the
// selection's edge refined (render/select_smart's {"refine"}: 半径・スマート半径 by the guided filter, then 滑らかに・ぼかし・
// コントラスト・エッジをシフト), its picture shown as 表示モード says (オニオンスキン・点線・オーバーレイ・黒地・白地・白黒・
// レイヤー上) — worked small on a worker thread a moment after a setting changes —, and put out to 選択範囲, レイヤーマスク,
// 新規レイヤー or 新規レイヤー（レイヤーマスクあり） (不要なカラーの除去 needs a new layer: then the one with a mask, as Photoshop).
// The layer outputs are one apply (one Undo): a new layer of the selection's pixels (add_layer + paste) or a duplicate with
// the refined mask (duplicate_layer + set_layer_mask, + matting decontaminate), the source layer hidden; a mask is linked.

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QImage>
#include <QLabel>
#include <QPixmap>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <cmath>
#include <memory>

#include "app/ask.hpp"
#include "app/canvas.hpp"
#include "app/layer_panel.hpp"
#include "app/main_window.hpp"
#include "core/ids.hpp"
#include "core/pyconv.hpp"
#include "render/page.hpp"
#include "render/raster_ops.hpp"
#include "render/selection.hpp"
#include "service/wording.hpp"

namespace genko::app {

using core::Json;

namespace {

constexpr int kPreviewDpi = 50;

double r3(double v) { return core::py_round(v, 3); }

// The page's picture (RGB) and the refined mask (L) of the same size, shown as the view mode says.
QImage shown_picture(const render::Image& page, const render::Image& mask, const QString& view) {
    const render::Image rgb = page.convert("RGB");
    const int w = rgb.width(), h = rgb.height();
    const std::string px = rgb.tobytes();
    const std::string m = (mask.size() == rgb.size() ? mask : mask.resize(rgb.size(), render::Resample::Bilinear)).convert("L").tobytes();
    QImage out(w, h, QImage::Format_RGB888);
    const auto at = [&](int x, int y) { return static_cast<unsigned char>(m[static_cast<std::size_t>(y) * w + x]); };
    for (int y = 0; y < h; ++y) {
        uchar* row = out.scanLine(y);
        for (int x = 0; x < w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * w + x;
            const double k = at(x, y) / 255.0;
            for (int c = 0; c < 3; ++c) {
                const double v = static_cast<unsigned char>(px[i * 3 + c]);
                double o = v;
                if (view == QLatin1String("overlay")) {  // (the masked part under red at half)
                    const double red = c == 0 ? 255.0 : 0.0;
                    o = v * k + (v * 0.5 + red * 0.5) * (1 - k);
                } else if (view == QLatin1String("black")) {
                    o = v * k;
                } else if (view == QLatin1String("white")) {
                    o = v * k + 255.0 * (1 - k);
                } else if (view == QLatin1String("bw")) {
                    o = 255.0 * k;
                } else if (view == QLatin1String("onion")) {  // (the masked part faded to half)
                    o = v * k + (v * 0.5 + 127.5) * (1 - k);
                } else if (view == QLatin1String("layers")) {  // (over a checkerboard: what the layer shows alone)
                    const double cell = ((x / 6 + y / 6) % 2) == 0 ? 204.0 : 255.0;
                    o = v * k + cell * (1 - k);
                }
                row[x * 3 + c] = static_cast<uchar>(std::clamp(std::lround(o), 0L, 255L));
            }
            // 点線: the edge where the mask crosses half, dashed black and white
            if (view == QLatin1String("ants") && x + 1 < w && y + 1 < h) {
                const bool in = at(x, y) >= 128;
                if (in != (at(x + 1, y) >= 128) || in != (at(x, y + 1) >= 128)) {
                    const uchar v = ((x + y) / 3) % 2 == 0 ? 0 : 255;
                    row[x * 3] = row[x * 3 + 1] = row[x * 3 + 2] = v;
                }
            }
        }
    }
    return out;
}

}  // namespace

void MainWindow::select_and_mask() {
    const auto current = need_area();
    const core::Page* page = current_page();
    if (!current || page == nullptr) return;
    const std::string page_id = page->id;
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("select_mask_dialog"));
    dialog.setWindowTitle(QStringLiteral("選択とマスク"));
    auto* outer = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    outer->addLayout(form);
    auto* view = new QComboBox;
    view->setObjectName(QStringLiteral("sm_view"));
    for (const auto& [label, key] : {std::pair{"オニオンスキン", "onion"}, {"点線", "ants"}, {"オーバーレイ", "overlay"}, {"黒地", "black"},
                                     {"白地", "white"}, {"白黒", "bw"}, {"レイヤー上", "layers"}})
        view->addItem(QString::fromUtf8(label), QString::fromLatin1(key));
    view->setCurrentIndex(view->findData(QStringLiteral("overlay")));
    form->addRow(QStringLiteral("表示モード"), view);
    auto* all_layers = new QCheckBox(QStringLiteral("全レイヤーを対象"));
    all_layers->setObjectName(QStringLiteral("sm_all_layers"));
    all_layers->setChecked(smart_select_settings.all_layers);
    all_layers->setToolTip(QStringLiteral("見えているページの絵で縁を探す（外すと描く先のレイヤーの絵だけ）"));
    form->addRow(all_layers);
    // エッジの検出
    auto* radius = new QDoubleSpinBox;
    radius->setObjectName(QStringLiteral("sm_radius"));
    radius->setRange(0, 30);
    radius->setDecimals(1);
    radius->setSingleStep(0.5);
    radius->setSuffix(QStringLiteral(" mm"));
    radius->setToolTip(QStringLiteral("縁から、この幅の中で絵の縁（髪の毛・ぼけ）を探します（0: 探さない）"));
    form->addRow(QStringLiteral("半径"), radius);
    auto* smart_radius = new QCheckBox(QStringLiteral("スマート半径"));
    smart_radius->setObjectName(QStringLiteral("sm_smart_radius"));
    smart_radius->setToolTip(QStringLiteral("くっきりした縁は狭く、ぼけた縁は広く探します"));
    form->addRow(smart_radius);
    // グローバル調整
    auto* smooth = new QSpinBox;
    smooth->setObjectName(QStringLiteral("sm_smooth"));
    smooth->setRange(0, 100);
    form->addRow(QStringLiteral("滑らかに"), smooth);
    auto* feather = new QDoubleSpinBox;
    feather->setObjectName(QStringLiteral("sm_feather"));
    feather->setRange(0, 50);
    feather->setDecimals(1);
    feather->setSuffix(QStringLiteral(" mm"));
    form->addRow(QStringLiteral("ぼかし"), feather);
    auto* contrast = new QSpinBox;
    contrast->setObjectName(QStringLiteral("sm_contrast"));
    contrast->setRange(0, 100);
    contrast->setSuffix(QStringLiteral(" %"));
    form->addRow(QStringLiteral("コントラスト"), contrast);
    auto* shift = new QSpinBox;
    shift->setObjectName(QStringLiteral("sm_shift"));
    shift->setRange(-100, 100);
    shift->setSuffix(QStringLiteral(" %"));
    form->addRow(QStringLiteral("エッジをシフト"), shift);
    // 出力設定
    auto* decontaminate = new QCheckBox(QStringLiteral("不要なカラーの除去"));
    decontaminate->setObjectName(QStringLiteral("sm_decontaminate"));
    form->addRow(decontaminate);
    auto* amount = new QSpinBox;
    amount->setObjectName(QStringLiteral("sm_amount"));
    amount->setRange(0, 100);
    amount->setValue(100);
    amount->setSuffix(QStringLiteral(" %"));
    amount->setEnabled(false);
    form->addRow(QStringLiteral("適用量"), amount);
    auto* output = new QComboBox;
    output->setObjectName(QStringLiteral("sm_output"));
    for (const auto& [label, key] : {std::pair{"選択範囲", "selection"}, {"レイヤーマスク", "layer_mask"}, {"新規レイヤー", "new_layer"},
                                     {"新規レイヤー（レイヤーマスクあり）", "new_layer_mask"}})
        output->addItem(QString::fromUtf8(label), QString::fromLatin1(key));
    form->addRow(QStringLiteral("出力先"), output);
    auto* picture = new QLabel;
    picture->setObjectName(QStringLiteral("sm_preview"));
    picture->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    outer->addWidget(picture);
    auto* said = new QLabel;
    said->setObjectName(QStringLiteral("sm_said"));
    said->setWordWrap(true);
    outer->addWidget(said);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    outer->addWidget(buttons);
    connect(decontaminate, &QCheckBox::toggled, amount, &QWidget::setEnabled);

    // the area the settings say, of the selection there is
    const Json chosen = *current;
    const std::string target = target_layer() != nullptr ? target_layer()->id : std::string();
    const auto refine_of = [=]() {
        Json spec{{"area", chosen}};
        if (radius->value() > 0) spec["radius_mm"] = r3(radius->value());
        if (smart_radius->isChecked()) spec["smart_radius"] = true;
        if (smooth->value() != 0) spec["smooth"] = smooth->value();
        if (feather->value() > 0) spec["feather_mm"] = r3(feather->value());
        if (contrast->value() != 0) spec["contrast"] = contrast->value();
        if (shift->value() != 0) spec["shift_edge"] = shift->value();
        if (!all_layers->isChecked() && !target.empty()) spec["layer"] = target;
        return Json{{"refine", spec}};
    };
    // the preview: the page small, the refined mask on a worker thread, shown as the view mode says
    render::use_brushes_of(book());
    const DocPtr doc = session_->snapshot();
    const std::size_t index = static_cast<std::size_t>(page_index_);
    auto seen = std::make_shared<render::Image>();
    auto mask = std::make_shared<render::Image>();
    try {
        *seen = render::render_page(*page, kPreviewDpi, render::proof_options(), &book()).image;
    } catch (const std::exception& error) {
        said->setText(wording::error(QString::fromUtf8(error.what())));
    }
    const auto show = [=] {
        if (seen->empty() || mask->empty()) return;
        picture->setPixmap(QPixmap::fromImage(shown_picture(*seen, *mask, view->currentData().toString())));
    };
    auto ticket = std::make_shared<std::uint64_t>(0);
    auto* timer = new QTimer(&dialog);
    timer->setSingleShot(true);
    timer->setInterval(250);
    connect(timer, &QTimer::timeout, &dialog, [=, &dialog] {
        const std::uint64_t mine = ++*ticket;
        auto* watcher = new QFutureWatcher<std::pair<render::Image, QString>>(&dialog);
        connect(watcher, &QFutureWatcher<std::pair<render::Image, QString>>::finished, &dialog, [=] {
            const auto [made, error] = watcher->result();
            watcher->deleteLater();
            if (mine != *ticket) return;  // (a newer one is on its way)
            said->setText(error.isEmpty() ? QString() : wording::error(error));
            if (!made.empty()) {
                *mask = made;
                show();
            }
        });
        watcher->setFuture(QtConcurrent::run([doc, index, area = refine_of()]() -> std::pair<render::Image, QString> {
            try {
                return {render::selection::to_mask(area, doc->page(index), doc.get(), kPreviewDpi), QString()};
            } catch (const std::exception& error) {
                return {render::Image(), QString::fromUtf8(error.what())};
            }
        }));
    });
    const auto later = [timer] { timer->start(); };
    connect(view, &QComboBox::currentIndexChanged, &dialog, [=](int) { show(); });
    for (QDoubleSpinBox* box : {radius, feather}) connect(box, &QDoubleSpinBox::valueChanged, &dialog, [=](double) { later(); });
    for (QSpinBox* box : {smooth, contrast, shift}) connect(box, &QSpinBox::valueChanged, &dialog, [=](int) { later(); });
    for (QCheckBox* box : {smart_radius, all_layers}) connect(box, &QCheckBox::toggled, &dialog, [=](bool) { later(); });
    later();
    const Asked asked = asking();
    const int answer = ask::exec(&dialog);
    ++*ticket;  // (a preview still on its way is not wanted)
    if (answer != QDialog::Accepted || !still(asked)) return;
    const core::Page* now = current_page();
    if (now == nullptr || now->id != page_id) return;
    smart_select_settings.all_layers = all_layers->isChecked();
    Json settings{{"output", output->currentData().toString().toStdString()}, {"layer", target}};
    if (decontaminate->isChecked()) {
        settings["decontaminate"] = true;
        settings["amount"] = amount->value();
        if (radius->value() > 0) settings["radius_mm"] = r3(radius->value());
    }
    solve_smart(refine_of(), QStringLiteral("選択とマスク"), [this, settings](const std::optional<Json>& area) {
        if (!area) {
            flash(QStringLiteral("選択範囲が残りませんでした"), 3000);
            return;
        }
        select_mask_output(*area, settings);
    });
}

void MainWindow::select_mask_output(const Json& area, const Json& settings) {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    std::string out = settings.value("output", std::string("selection"));
    const bool decontaminate = settings.value("decontaminate", false);
    // (不要なカラーの除去 needs a new layer: the selection's and the mask's outputs become the new layer with a mask)
    if (decontaminate && (out == "selection" || out == "layer_mask")) out = "new_layer_mask";
    if (out == "selection") {
        if (canvas_->tool() != QLatin1String("marquee")) choose_tool(QStringLiteral("rect"));
        canvas_->set_selection(area);
        return;
    }
    const std::string source = settings.value("layer", std::string());
    const core::Layer* layer = nullptr;
    for (const core::Layer& l : page->layers)
        if (l.id == source) layer = &l;
    if (layer == nullptr) {
        flash(QStringLiteral("描く先のレイヤーを選んでから出力します"), 3000);
        return;
    }
    const Json index = page->index.json();
    Json ops = Json::array();
    std::string made;
    if (out == "layer_mask") {
        ops.push_back(Json{{"op", "set_layer_mask"}, {"page", index}, {"id", source}, {"area", area}, {"linked", true}});
    } else if (out == "new_layer") {
        const auto items = copied_items(area);  // (the source layer's pixels there, each by the area's coverage)
        if (!items) return;
        made = core::new_id();
        const bool pixels = !(*items).value("patches", Json::array()).empty();
        ops.push_back(Json{{"op", "add_layer"}, {"page", index}, {"name", layer->title.empty() ? std::string("レイヤー のコピー") : layer->title + " のコピー"},
                           {"kind", pixels ? "paint" : "pen"}, {"id", made}, {"after", source}});
        ops.push_back(Json{{"op", "paste"}, {"page", index}, {"layer_id", made}, {"items", *items}});
        if (decontaminate) {
            Json clean{{"op", "matting"}, {"page", index}, {"id", made}, {"kind", "decontaminate"}, {"amount", settings.value("amount", 100)}};
            if (settings.contains("radius_mm")) clean["radius_mm"] = settings["radius_mm"];
            ops.push_back(clean);
        }
        ops.push_back(Json{{"op", "set_layer"}, {"page", index}, {"id", source}, {"visible", false}});
    } else {  // (new_layer_mask)
        made = core::new_id();
        ops.push_back(Json{{"op", "duplicate_layer"}, {"page", index}, {"id", source}, {"new_id", made}});
        ops.push_back(Json{{"op", "set_layer_mask"}, {"page", index}, {"id", made}, {"area", area}, {"linked", true}});
        if (decontaminate) {
            Json clean{{"op", "matting"}, {"page", index}, {"id", made}, {"kind", "decontaminate"}, {"amount", settings.value("amount", 100)}, {"area", area}};
            if (settings.contains("radius_mm")) clean["radius_mm"] = settings["radius_mm"];
            ops.push_back(clean);
        }
        ops.push_back(Json{{"op", "set_layer"}, {"page", index}, {"id", source}, {"visible", false}});
    }
    if (!apply_ops(ops)) return;
    canvas_->set_selection(std::nullopt);  // (Photoshop's: the selection let go after a mask or a layer is made)
    if (!made.empty()) {
        set_target_layer(made);
        if (layer_panel_ != nullptr) layer_panel_->refresh();
    }
}

}  // namespace genko::app
