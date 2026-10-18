// PC3 B1 (SPEC.md COLOR-06「画面」, the user's decision 5: Photoshop's names): the selection's windows — 選択範囲 →
// 色域指定… (Color Range: the sampled colours picked with the eyedroppers on its picture of the page, the colour families,
// the tones, the colours out of the proof's gamut; its picture shows what it selects), 選択範囲を保存… (into a new alpha
// channel, or into one the page has: replaced, added to, taken from, intersected) and 選択範囲を読み込む… (from an alpha
// or spot channel, a colour plate, the drawing layer's transparency or mask; inverted; as the new selection or joined to
// the one there). The selection is the window's (the user's decision 4): reading makes it, saving is a change to the book.

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPixmap>
#include <QRadioButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

#include "app/ask.hpp"
#include "app/canvas.hpp"
#include "app/main_window.hpp"
#include "core/channels.hpp"
#include "core/masks.hpp"
#include "core/pyconv.hpp"
#include "render/page.hpp"
#include "render/proof.hpp"
#include "render/raster_ops.hpp"
#include "render/select_color.hpp"
#include "render/selection.hpp"
#include "service/wording.hpp"

namespace genko::app {

using core::Json;

namespace {

// Color Range's picture of the page: a click on it is a point of the page (mm) for the eyedroppers.
class PickedPicture : public QLabel {
public:
    std::function<void(QPointF)> picked;
    QSizeF page_mm;

protected:
    void mousePressEvent(QMouseEvent* event) override {
        const QPixmap shown = pixmap();
        if (!picked || shown.isNull() || page_mm.isEmpty()) return;
        // (the picture is drawn at the label's top left, at its own size)
        const QPointF at = event->position();
        if (at.x() < 0 || at.y() < 0 || at.x() >= shown.width() || at.y() >= shown.height()) return;
        picked(QPointF(at.x() / shown.width() * page_mm.width(), at.y() / shown.height() * page_mm.height()));
    }
};

QImage grey_picture(const render::Image& mask) {
    QImage out(mask.width(), mask.height(), QImage::Format_Grayscale8);
    const std::string bytes = mask.convert("L").tobytes();
    for (int y = 0; y < mask.height(); ++y) std::copy_n(bytes.data() + std::size_t(y) * mask.width(), mask.width(), out.scanLine(y));
    return out;
}

QImage rgb_picture(const render::Image& picture) {
    const render::Image rgb = picture.convert("RGB");
    QImage out(rgb.width(), rgb.height(), QImage::Format_RGB888);
    const std::string bytes = rgb.tobytes();
    for (int y = 0; y < rgb.height(); ++y) std::copy_n(bytes.data() + std::size_t(y) * rgb.width() * 3, rgb.width() * 3, out.scanLine(y));
    return out;
}

// The joining ways of a selection read or made (Photoshop's Operation): their words and the window's names.
const std::vector<std::pair<QString, QString>>& joining() {
    static const std::vector<std::pair<QString, QString>> ways{{QStringLiteral("新しい選択範囲"), QStringLiteral("replace")},
                                                               {QStringLiteral("選択範囲に追加"), QStringLiteral("add")},
                                                               {QStringLiteral("選択範囲から一部削除"), QStringLiteral("subtract")},
                                                               {QStringLiteral("選択範囲との共通範囲"), QStringLiteral("intersect")}};
    return ways;
}

QDialogButtonBox* ok_cancel(QDialog& dialog) {
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    return buttons;
}

}  // namespace

// --- 色域指定… ------------------------------------------------------------------------------------------------------

void MainWindow::color_range_dialog() {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    const double page_w = page->spec.width_mm.value(), page_h = page->spec.height_mm.value();
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("color_range_dialog"));
    dialog.setWindowTitle(QStringLiteral("色域指定"));
    auto* outer = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    outer->addLayout(form);
    auto* select = new QComboBox;
    select->setObjectName(QStringLiteral("range_select"));
    for (const auto& [label, key] : {std::pair{"指定色域", "sampled"}, {"レッド系", "reds"}, {"イエロー系", "yellows"}, {"グリーン系", "greens"},
                                     {"シアン系", "cyans"}, {"ブルー系", "blues"}, {"マゼンタ系", "magentas"}, {"ハイライト", "highlights"},
                                     {"中間調", "midtones"}, {"シャドウ", "shadows"}, {"色域外", "out_of_gamut"}})
        select->addItem(QString::fromUtf8(label), QString::fromLatin1(key));
    form->addRow(QStringLiteral("選択"), select);
    auto* fuzziness = new QSpinBox;
    fuzziness->setObjectName(QStringLiteral("range_fuzziness"));
    fuzziness->setRange(0, 200);
    fuzziness->setValue(40);
    form->addRow(QStringLiteral("許容量"), fuzziness);
    auto* localized = new QCheckBox(QStringLiteral("ローカライズされたカラークラスタ"));
    localized->setObjectName(QStringLiteral("range_localized"));
    form->addRow(localized);
    auto* range = new QSpinBox;
    range->setObjectName(QStringLiteral("range_range"));
    range->setRange(0, 100);
    range->setValue(100);
    range->setSuffix(QStringLiteral(" %"));
    range->setEnabled(false);
    form->addRow(QStringLiteral("範囲"), range);
    auto* invert = new QCheckBox(QStringLiteral("反転"));
    invert->setObjectName(QStringLiteral("range_invert"));
    form->addRow(invert);
    // the eyedroppers (a click on the picture): the colour there, one more, one taken away
    auto* droppers = new QHBoxLayout;
    auto* pick = new QRadioButton(QStringLiteral("スポイト"));
    auto* pick_add = new QRadioButton(QStringLiteral("サンプルに追加"));
    auto* pick_subtract = new QRadioButton(QStringLiteral("サンプルから削除"));
    pick->setObjectName(QStringLiteral("range_pick"));
    pick_add->setObjectName(QStringLiteral("range_pick_add"));
    pick_subtract->setObjectName(QStringLiteral("range_pick_subtract"));
    pick->setChecked(true);
    for (QRadioButton* b : {pick, pick_add, pick_subtract}) droppers->addWidget(b);
    form->addRow(droppers);
    auto* show_image = new QCheckBox(QStringLiteral("画像を表示（スポイトで色を拾う）"));
    show_image->setObjectName(QStringLiteral("range_show_image"));
    form->addRow(show_image);
    auto* picture = new PickedPicture;
    picture->setObjectName(QStringLiteral("range_preview"));
    picture->page_mm = QSizeF(page_w, page_h);
    picture->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    outer->addWidget(picture);
    auto* said = new QLabel;
    said->setObjectName(QStringLiteral("range_said"));
    said->setWordWrap(true);
    outer->addWidget(said);
    outer->addWidget(ok_cancel(dialog));

    // the samples: the colour picked at points (mm), or the drawing colour until one is picked
    auto points = std::make_shared<Json>(Json::array());
    auto minus = std::make_shared<Json>(Json::array());
    const Json drawing_colour = Json::array({pen_.rgb.at(0), pen_.rgb.at(1), pen_.rgb.at(2)});
    const render::proof::Setup proof_setup = canvas_->renderer().proof_setup();
    const double threshold = canvas_->renderer().gamut() ? canvas_->renderer().gamut()->threshold : render::proof::kDefaultThreshold;
    const std::string page_id = page->id;
    const auto spec = [=]() {
        const QString key = select->currentData().toString();
        Json out{{"select", key.toStdString()}};
        const bool tones = key == QLatin1String("highlights") || key == QLatin1String("midtones") || key == QLatin1String("shadows");
        if (key == QLatin1String("sampled")) {
            if (points->empty()) out["colors"] = Json::array({drawing_colour});
            else out["points_mm"] = *points;
            if (!minus->empty()) out["subtract_points_mm"] = *minus;
            out["fuzziness"] = fuzziness->value();
            if (localized->isChecked() && !points->empty()) {
                out["localized"] = true;
                out["range"] = range->value();
            }
        } else if (tones) {
            out["fuzziness"] = std::min(fuzziness->value(), 100);
        }
        if (invert->isChecked()) out["invert"] = true;
        return out;
    };
    // the area: Out of Gamut against this window's proof setup (表示 → 校正の設定), the others as the ops take them
    const auto area_of = [this, spec, proof_setup, threshold](const core::Page& shown, int dpi) -> std::optional<Json> {
        const Json given = spec();
        if (given["select"] == Json("out_of_gamut")) {
            const render::Size size{render::mm_to_px(shown.spec.width_mm.value(), dpi), render::mm_to_px(shown.spec.height_mm.value(), dpi)};
            render::Image mask = render::select_color::out_of_gamut(shown, book(), proof_setup, threshold, size);
            if (given.value("invert", false)) mask = mask.point([](int v) { return 255 - v; });
            return render::selection::from_mask(mask, dpi);
        }
        return render::selection::from_mask(render::selection::to_mask(Json{{"range", given}}, shown, &book(), dpi), dpi);
    };
    auto* timer = new QTimer(&dialog);
    timer->setSingleShot(true);
    timer->setInterval(250);
    const auto refresh = [&, this] {
        const core::Page* now = current_page();
        if (now == nullptr || now->id != page_id) return;
        constexpr int kPreviewDpi = 30;
        try {
            render::use_brushes_of(book());
            if (show_image->isChecked()) {
                render::RenderOptions options = render::proof_options();
                picture->setPixmap(QPixmap::fromImage(rgb_picture(render::render_page(*now, kPreviewDpi, options, &book()).image)));
            } else {
                const render::Size size{render::mm_to_px(page_w, kPreviewDpi), render::mm_to_px(page_h, kPreviewDpi)};
                const auto area = area_of(*now, kPreviewDpi);
                const render::Image mask =
                    area ? render::selection::to_mask(*area, *now, &book(), kPreviewDpi) : render::Image::create("L", size, render::Ink(0));
                picture->setPixmap(QPixmap::fromImage(grey_picture(mask)));
            }
            said->clear();
        } catch (const std::exception& error) {
            said->setText(wording::error(QString::fromUtf8(error.what())));
        }
    };
    connect(timer, &QTimer::timeout, &dialog, refresh);
    const auto later = [timer] { timer->start(); };
    connect(select, &QComboBox::currentIndexChanged, &dialog, [=](int) {
        const QString key = select->currentData().toString();
        const bool sampled = key == QLatin1String("sampled");
        const bool tones = key == QLatin1String("highlights") || key == QLatin1String("midtones") || key == QLatin1String("shadows");
        fuzziness->setEnabled(sampled || tones);
        fuzziness->setMaximum(tones ? 100 : 200);
        localized->setEnabled(sampled);
        for (QRadioButton* b : {pick, pick_add, pick_subtract}) b->setEnabled(sampled);
        later();
    });
    connect(fuzziness, &QSpinBox::valueChanged, &dialog, [=](int) { later(); });
    connect(range, &QSpinBox::valueChanged, &dialog, [=](int) { later(); });
    connect(localized, &QCheckBox::toggled, &dialog, [=](bool on) {
        range->setEnabled(on);
        later();
    });
    connect(invert, &QCheckBox::toggled, &dialog, [=](bool) { later(); });
    connect(show_image, &QCheckBox::toggled, &dialog, [=](bool) { later(); });
    picture->picked = [=](QPointF mm) {
        const Json point = Json::array({core::py_round(mm.x(), 3), core::py_round(mm.y(), 3)});
        if (pick_add->isChecked()) {
            points->push_back(point);
        } else if (pick_subtract->isChecked()) {
            minus->push_back(point);
        } else {
            *points = Json::array({point});
            *minus = Json::array();
        }
        if (select->currentData().toString() != QLatin1String("sampled")) select->setCurrentIndex(0);
        later();
    };
    refresh();
    const Asked asked = asking();
    if (ask::exec(&dialog) != QDialog::Accepted || !still(asked)) return;
    const core::Page* now = current_page();
    if (now == nullptr || now->id != page_id) return;
    std::optional<Json> area;
    try {
        render::use_brushes_of(book());
        area = area_of(*now, render::selection::kSelDpi);
    } catch (const std::exception& error) {
        flash(wording::error(QString::fromUtf8(error.what())), 4000, true);
        return;
    }
    if (!area) {
        flash(QStringLiteral("色域指定で選ばれる所がありません"), 3000);
        return;
    }
    if (canvas_->tool() != QLatin1String("marquee")) choose_tool(QStringLiteral("rect"));
    join_selection(area, QStringLiteral("replace"));
}

// --- 選択範囲を保存… -----------------------------------------------------------------------------------------------

bool MainWindow::save_selection_dialog() {
    const auto area = need_area();
    const core::Page* page = current_page();
    if (!area || page == nullptr) return false;
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("save_selection_dialog"));
    dialog.setWindowTitle(QStringLiteral("選択範囲を保存"));
    auto* form = new QFormLayout(&dialog);
    auto* channel = new QComboBox;
    channel->setObjectName(QStringLiteral("save_channel"));
    channel->addItem(QStringLiteral("新規"), QString());
    for (const core::Channel& each : page->channels) {
        if (each.kind == core::ChannelKind::Alpha && !each.quick_mask) channel->addItem(QString::fromStdString(each.name), QString::fromStdString(each.id));
    }
    form->addRow(QStringLiteral("チャンネル"), channel);
    auto* name = new QLineEdit;
    name->setObjectName(QStringLiteral("save_name"));
    name->setPlaceholderText(QString::fromStdString(core::next_channel_name(*page, core::ChannelKind::Alpha)));
    form->addRow(QStringLiteral("名前"), name);
    auto* operation = new QComboBox;
    operation->setObjectName(QStringLiteral("save_operation"));
    const auto ways = [operation](bool fresh) {
        operation->clear();
        if (fresh) {
            operation->addItem(QStringLiteral("新規チャンネル"), QStringLiteral("new"));
            return;
        }
        for (const auto& [label, key] : {std::pair{"チャンネルを置き換え", "replace"}, {"チャンネルに追加", "add"}, {"チャンネルから削除", "subtract"},
                                         {"チャンネルと交差", "intersect"}})
            operation->addItem(QString::fromUtf8(label), QString::fromLatin1(key));
    };
    ways(true);
    form->addRow(QStringLiteral("操作"), operation);
    connect(channel, &QComboBox::currentIndexChanged, &dialog, [=](int) {
        const bool fresh = channel->currentData().toString().isEmpty();
        name->setEnabled(fresh);
        ways(fresh);
    });
    form->addRow(ok_cancel(dialog));
    const Asked asked = asking();
    if (ask::exec(&dialog) != QDialog::Accepted || !still(asked)) return false;
    const std::string id = channel->currentData().toString().toStdString();
    if (id.empty()) return save_selection_to_channel(name->text());
    return apply_ops(Json::array({Json{{"op", "set_channel"}, {"page", page->index.json()}, {"channel", id}, {"area", *area},
                                       {"operation", operation->currentData().toString().toStdString()}}}));
}

// --- 選択範囲を読み込む… ----------------------------------------------------------------------------------------------

void MainWindow::load_selection_dialog() {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    const core::Layer* layer = target_layer();
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("load_selection_dialog"));
    dialog.setWindowTitle(QStringLiteral("選択範囲を読み込む"));
    auto* form = new QFormLayout(&dialog);
    auto* source = new QComboBox;
    source->setObjectName(QStringLiteral("load_source"));
    // (the page's channels — the Quick Mask's too —, its colour plates and their composite, the drawing layer's transparency
    // and mask: each as the area the ops take)
    for (const core::Channel& each : page->channels) {
        source->addItem(QString::fromStdString(each.name), QString::fromStdString(Json{{"channel", each.id}}.dump()));
    }
    const std::vector<std::string> plates = core::plate_names(*page);
    if (plates.size() > 1) {
        const std::vector<std::string> labels = core::plate_labels(*page);
        source->addItem(QStringLiteral("合成（明るさ）"), QString::fromStdString(Json{{"channel", "composite"}}.dump()));
        for (std::size_t i = 0; i < plates.size(); ++i)
            source->addItem(QString::fromStdString(labels[i]), QString::fromStdString(Json{{"channel", plates[i]}}.dump()));
    } else {
        source->addItem(QStringLiteral("グレー（明るさ）"), QString::fromStdString(Json{{"channel", "composite"}}.dump()));
    }
    if (layer != nullptr) {
        const QString title = wording::layer_label(*layer);
        source->addItem(QStringLiteral("%1 の透明部分").arg(title), QString::fromStdString(Json{{"transparency", layer->id}}.dump()));
        if (layer->mask && layer->mask->png && !layer->mask->png->empty())
            source->addItem(QStringLiteral("%1 のマスク").arg(title), QString::fromStdString(Json{{"layer_mask", layer->id}}.dump()));
    }
    form->addRow(QStringLiteral("チャンネル"), source);
    auto* invert = new QCheckBox(QStringLiteral("反転"));
    invert->setObjectName(QStringLiteral("load_invert"));
    form->addRow(invert);
    auto* operation = new QComboBox;
    operation->setObjectName(QStringLiteral("load_operation"));
    for (const auto& [label, key] : joining()) operation->addItem(label, key);
    operation->setEnabled(selection_area().has_value());
    form->addRow(QStringLiteral("操作"), operation);
    form->addRow(ok_cancel(dialog));
    const Asked asked = asking();
    if (ask::exec(&dialog) != QDialog::Accepted || !still(asked)) return;
    const core::Page* now = current_page();
    if (now == nullptr || source->currentIndex() < 0) return;
    Json area = Json::parse(source->currentData().toString().toStdString());
    if (invert->isChecked()) area["invert"] = true;
    std::optional<Json> resolved;
    try {
        render::use_brushes_of(book());
        resolved = render::selection::resolve(area, *now, &book());
    } catch (const core::OpError&) {
        flash(QStringLiteral("「%1」には、選択する所がありません").arg(source->currentText()), 3000);
        return;
    } catch (const std::exception& error) {
        flash(wording::error(QString::fromUtf8(error.what())), 4000, true);
        return;
    }
    if (canvas_->tool() != QLatin1String("marquee")) choose_tool(QStringLiteral("rect"));
    join_selection(resolved, operation->isEnabled() ? operation->currentData().toString() : QStringLiteral("replace"));
}

}  // namespace genko::app
