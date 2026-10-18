// 編集 → コンテンツに応じた塗りつぶし… (PC3 B4, edit.contentAwareFill; SPEC.md COLOR-06「修復」): Photoshop's window over the
// op content_aware_fill (render/ops_retouch) — サンプリング領域 (自動・長方形 and its margin・カスタム: an alpha channel's white
// as the area copied from), カラー適応, 回転適応, 拡大・縮小, 左右に反転, 出力先 (現在のレイヤー・新規レイヤー・レイヤーを複製) —
// the result shown on the page while it changes (the window's preview, 250 ms after the last change), then 塗りつぶす.

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QPushButton>
#include <QTimer>

#include "app/adjust_forms.hpp"
#include "app/ask.hpp"
#include "app/main_window.hpp"
#include "core/ids.hpp"

namespace genko::app {

using core::Json;

void MainWindow::content_aware_fill_dialog() {
    const auto asked = asking();
    const core::Page* page = current_page();
    const core::Layer* layer = target_layer();
    if (page == nullptr || layer == nullptr) {
        flash(QStringLiteral("描く先のレイヤーを選びます"), 3000);
        return;
    }
    if (layer->kind != core::LayerKind::Raster && layer->kind != core::LayerKind::Strokes) {
        flash(QStringLiteral("描画のレイヤー（ペン・ペイント）を選びます"), 3000);
        return;
    }
    const std::optional<Json> area = selection_area();
    if (!area) {
        flash(QStringLiteral("先に、埋める所を選択範囲で囲みます"), 4000);
        return;
    }
    const Json op{{"op", "content_aware_fill"}, {"page", page->index.json()}, {"layer_id", layer->id}, {"area", *area}};
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("content_aware_dialog"));
    dialog.setWindowTitle(QStringLiteral("コンテンツに応じた塗りつぶし"));
    auto* layout = new QFormLayout(&dialog);
    auto* form = new adjust_forms::Form;
    form->choice("sampling", QStringLiteral("サンプリング領域"),
                 {{QStringLiteral("自動"), Json("auto")}, {QStringLiteral("長方形"), Json("rectangular")}, {QStringLiteral("カスタム"), Json("custom")}},
                 Json("auto"));
    form->number("margin_mm", QStringLiteral("長方形の余白（mm）"), 0, 500, 10, 1);
    std::vector<std::pair<QString, Json>> sources{{QStringLiteral("（なし）"), Json()}};
    for (const core::Channel& channel : page->channels) sources.emplace_back(QString::fromStdString(channel.name), Json{{"channel", channel.id}});
    form->choice("source_area", QStringLiteral("カスタムのサンプリング領域"), sources, Json());
    form->choice("color_adaptation", QStringLiteral("カラー適応"),
                 {{QStringLiteral("なし"), Json("none")}, {QStringLiteral("初期設定"), Json("default")}, {QStringLiteral("高"), Json("high")},
                  {QStringLiteral("非常に高い"), Json("very_high")}},
                 Json("default"));
    form->choice("rotation_adaptation", QStringLiteral("回転適応"),
                 {{QStringLiteral("なし"), Json("none")}, {QStringLiteral("低"), Json("low")}, {QStringLiteral("中"), Json("medium")},
                  {QStringLiteral("高"), Json("high")}, {QStringLiteral("フル"), Json("full")}},
                 Json("none"));
    form->check("scale", QStringLiteral("拡大・縮小"), false);
    form->check("mirror", QStringLiteral("左右に反転"), false);
    form->choice("output", QStringLiteral("出力先"),
                 {{QStringLiteral("現在のレイヤー"), Json("current")}, {QStringLiteral("新規レイヤー"), Json("new")},
                  {QStringLiteral("レイヤーを複製"), Json("duplicate")}},
                 Json("current"));
    form->note(QStringLiteral("選択範囲を、まわりの絵（サンプリング領域）から作って埋めます。長方形は選択範囲のまわりの余白から、"
                              "カスタムはアルファチャンネルの白い所から写します。"));
    form->set_values(Json::object());
    layout->addRow(form);
    // (the settings laid into the op; a null left out)
    const auto with = [&op](const Json& values) {
        Json out = op;
        for (const auto& [key, value] : values.items()) {
            if (!value.is_null()) out[key] = value;
        }
        return out;
    };
    auto* check = new QCheckBox(QStringLiteral("プレビュー（ページで見る）"));
    check->setObjectName(QStringLiteral("content_aware_preview"));
    check->setChecked(true);
    layout->addRow(QString(), check);
    auto* timer = new QTimer(&dialog);
    timer->setSingleShot(true);
    timer->setInterval(250);
    connect(form, &adjust_forms::Form::changed, &dialog, [timer] { timer->start(); });
    connect(check, &QCheckBox::toggled, &dialog, [timer] { timer->start(); });
    connect(timer, &QTimer::timeout, &dialog, [&] {
        try {
            preview_ops(check->isChecked() ? std::optional<Json>(Json::array({with(form->values())})) : std::nullopt);
        } catch (const std::exception&) {
            preview_ops(std::nullopt);  // (a preview that cannot be made must not stop the window)
        }
    });
    timer->start(0);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("塗りつぶす"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("やめる"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addRow(buttons);
    const bool ok = ask::exec(&dialog) == QDialog::Accepted;
    timer->stop();
    const Json values = form->values();
    preview_ops(std::nullopt);
    if (!ok || !still(asked)) return;
    Json made = with(values);
    if (made.value("output", std::string("current")) != "current") made["new_id"] = core::new_id();  // (the new layer's id, chosen here)
    apply_ops(Json::array({made}));
}

}  // namespace genko::app
