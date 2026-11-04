// 表示 → ピクセル縦横比 ▸ and ピクセル縦横比を補正 (PC5 H-f, the user's decision 10; SPEC.md VIEW-01「ピクセル縦横比」): the
// page's pixel aspect ratio (pixel-aspect@1, core/pixel_aspect.hpp) — Photoshop's ratios, and ratios of the person's own kept
// by their names on this computer (カスタムピクセル縦横比…, PhotoCraft v0.2.0's workspace_ui.rs custom_par: MIT) — and the
// view widened by it (PageCanvas::set_pixel_aspect_correction). For the view only: what is drawn and every file written
// keep the page's square pixels.

#include <QActionGroup>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

#include <cmath>
#include <utility>
#include <vector>

#include "app/ask.hpp"
#include "app/canvas.hpp"
#include "app/config.hpp"
#include "app/main_window.hpp"
#include "core/pixel_aspect.hpp"

namespace genko::app {

namespace {

const QString kCustomKey = QStringLiteral("view/custom_pixel_aspects");  // (a JSON list of {name, ratio})

// The commands of the ratios, by the presets' ids (the ledger's view.pixelAspectRatio.<id>).
QString attribute_of(std::string_view id) {
    static const std::vector<std::pair<std::string_view, const char*>> names{
        {"square", "act_pixel_aspect_square"},
        {"d1DvNtsc", "act_pixel_aspect_d1_ntsc"},
        {"d1DvPal", "act_pixel_aspect_d1_pal"},
        {"d1DvNtscWidescreen", "act_pixel_aspect_d1_ntsc_wide"},
        {"hdv1080", "act_pixel_aspect_hdv1080"},
        {"d1DvPalWidescreen", "act_pixel_aspect_d1_pal_wide"},
        {"dvcproHd1080", "act_pixel_aspect_dvcpro_hd1080"},
        {"anamorphic2To1", "act_pixel_aspect_anamorphic"}};
    for (const auto& [preset, name] : names) {
        if (preset == id) return QString::fromLatin1(name);
    }
    return {};
}

// The person's own ratios, as kept (the latest kept last).
std::vector<std::pair<QString, double>> custom_ratios() {
    std::vector<std::pair<QString, double>> out;
    const QJsonArray kept = QJsonDocument::fromJson(settings()->value(kCustomKey).toString().toUtf8()).array();
    for (const QJsonValue& v : kept) {
        const QJsonObject o = v.toObject();
        const QString name = o.value(QStringLiteral("name")).toString().trimmed();
        const double ratio = o.value(QStringLiteral("ratio")).toDouble();
        if (!name.isEmpty() && core::pixel_aspect_allowed(ratio)) out.emplace_back(name, ratio);
    }
    return out;
}

void keep_custom_ratios(const std::vector<std::pair<QString, double>>& ratios) {
    QJsonArray list;
    for (const auto& [name, ratio] : ratios) list.append(QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("ratio"), ratio}});
    settings()->setValue(kCustomKey, QString::fromUtf8(QJsonDocument(list).toJson(QJsonDocument::Compact)));
}

}  // namespace

void MainWindow::build_pixel_aspect_actions() {
    auto* ratios = new QActionGroup(this);
    ratios->setExclusive(true);
    QAction* custom = make("act_pixel_aspect_custom", QStringLiteral("カスタムピクセル縦横比…"), [this] { ask_custom_pixel_aspect(); }, {},
                           QStringLiteral("画素の縦横比を数で決めます（名前を付けると、このパソコンに残してまた選べます）"), true);
    ratios->addAction(custom);
    for (const core::PixelAspectPreset& preset : core::pixel_aspect_presets()) {
        const double ratio = preset.ratio;
        QAction* act = make(attribute_of(preset.id), QString::fromUtf8(preset.name.data(), static_cast<qsizetype>(preset.name.size())),
                            [this, ratio] { set_pixel_aspect(ratio); }, {},
                            preset.id == "square" ? QStringLiteral("ページの画素を正方形として表示します")
                                                  : QStringLiteral("ページの画素をこの縦横比（幅 ÷ 高さ）として表示します（表示だけ。描く画素・書き出しは正方形のまま）"),
                            true);
        ratios->addAction(act);
    }
    QAction* correction = make("act_pixel_aspect_correction", QStringLiteral("ピクセル縦横比を補正"), [] {}, {},
                               QStringLiteral("画素が正方形でないページを、その縦横比で横に伸ばして（縮めて）表示します（表示だけ）"), true);
    correction->setChecked(true);
    connect(correction, &QAction::toggled, this, [this](bool on) {
        for (PageCanvas* c : findChildren<PageCanvas*>()) c->set_pixel_aspect_correction(on);
    });
    refresh_pixel_aspect();
}

void MainWindow::build_pixel_aspect_menu(QMenu* view) {
    QMenu* ratios = view->addMenu(QStringLiteral("ピクセル縦横比"));
    ratios->setObjectName(QStringLiteral("menu_pixel_aspect"));
    ratios->addAction(action(QStringLiteral("act_pixel_aspect_custom")));
    ratios->addSeparator();
    for (const core::PixelAspectPreset& preset : core::pixel_aspect_presets()) ratios->addAction(action(attribute_of(preset.id)));
    connect(ratios, &QMenu::aboutToShow, this, [this] { refresh_pixel_aspect(); });
    view->addAction(action(QStringLiteral("act_pixel_aspect_correction")));
}

void MainWindow::refresh_pixel_aspect() {
    const core::Page* page = canvas_ != nullptr ? canvas_->page() : nullptr;
    const double ratio = page != nullptr ? page->pixel_aspect : 1.0;
    const auto preset = core::pixel_aspect_preset_of(ratio);
    QAction* chosen = action(preset ? attribute_of(preset->id) : QStringLiteral("act_pixel_aspect_custom"));
    if (chosen != nullptr) chosen->setChecked(true);
    for (const core::PixelAspectPreset& each : core::pixel_aspect_presets()) {
        if (QAction* act = action(attribute_of(each.id))) act->setEnabled(page != nullptr);
    }
    if (QAction* custom = action(QStringLiteral("act_pixel_aspect_custom"))) custom->setEnabled(page != nullptr);
}

bool MainWindow::set_pixel_aspect(double ratio) {
    const core::Page* page = current_page();
    if (page == nullptr) return false;
    const bool done = std::abs(page->pixel_aspect - ratio) < 1e-12 ||
                      apply_ops(core::Json::array({core::Json{{"op", "set_pixel_aspect"}, {"page", page->index.json()}, {"ratio", ratio}}}));
    // (as Photoshop's: a ratio chosen is shown — the correction on)
    if (done && !core::square_pixels(ratio)) {
        if (QAction* correction = action(QStringLiteral("act_pixel_aspect_correction")); correction != nullptr && !correction->isChecked()) correction->setChecked(true);
    }
    refresh_pixel_aspect();
    return done;
}

void MainWindow::ask_custom_pixel_aspect() {
    const core::Page* page = current_page();
    if (page == nullptr) {
        refresh_pixel_aspect();
        return;
    }
    std::vector<std::pair<QString, double>> kept = custom_ratios();
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("custom_pixel_aspect"));
    dialog.setWindowTitle(QStringLiteral("カスタムピクセル縦横比"));
    auto* saved = new QComboBox;
    saved->setObjectName(QStringLiteral("pixel_aspect_saved"));
    saved->addItem(QStringLiteral("（新しい比）"), QString());
    for (const auto& [name, ratio] : kept) saved->addItem(QStringLiteral("%1（%2）").arg(name).arg(ratio, 0, 'f', 3), name);
    auto* name = new QLineEdit;
    name->setObjectName(QStringLiteral("pixel_aspect_name"));
    name->setPlaceholderText(QStringLiteral("名前（空のときは残しません）"));
    auto* ratio = new QDoubleSpinBox;
    ratio->setObjectName(QStringLiteral("pixel_aspect_ratio"));
    ratio->setRange(core::kPixelAspectMin, core::kPixelAspectMax);
    ratio->setDecimals(3);
    ratio->setSingleStep(0.01);
    ratio->setValue(page->pixel_aspect);
    auto* remove = new QPushButton(QStringLiteral("削除"));
    remove->setObjectName(QStringLiteral("pixel_aspect_delete"));
    remove->setEnabled(false);
    connect(saved, &QComboBox::currentIndexChanged, &dialog, [&kept, saved, name, ratio, remove](int i) {
        const QString chosen = saved->itemData(i).toString();
        remove->setEnabled(!chosen.isEmpty());
        for (const auto& [n, r] : kept) {
            if (n != chosen) continue;
            name->setText(n);
            ratio->setValue(r);
        }
    });
    connect(remove, &QPushButton::clicked, &dialog, [&kept, saved] {
        const QString chosen = saved->currentData().toString();
        std::erase_if(kept, [&chosen](const auto& k) { return k.first == chosen; });
        keep_custom_ratios(kept);
        saved->removeItem(saved->currentIndex());
        saved->setCurrentIndex(0);
    });
    auto* form = new QFormLayout;
    form->addRow(QStringLiteral("保存した比"), saved);
    form->addRow(QStringLiteral("名前"), name);
    form->addRow(QStringLiteral("縦横比"), ratio);
    auto* note = new QLabel(QStringLiteral("画素の幅 ÷ 高さ（0.100〜10.000）。表示だけを変えます（描く画素・書き出しは正方形のまま）"));
    note->setWordWrap(true);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addLayout(form);
    layout->addWidget(remove, 0, Qt::AlignLeft);
    layout->addWidget(note);
    layout->addWidget(buttons);
    if (ask::exec(&dialog) != QDialog::Accepted) {
        refresh_pixel_aspect();
        return;
    }
    const double chosen = ratio->value();
    if (const QString given = name->text().trimmed(); !given.isEmpty()) {  // (kept by its name: a name kept before takes the new ratio)
        std::erase_if(kept, [&given](const auto& k) { return k.first == given; });
        kept.emplace_back(given, chosen);
        keep_custom_ratios(kept);
    }
    set_pixel_aspect(chosen);
}

}  // namespace genko::app
