// 編集 → コンテンツに応じた塗りつぶし… (PC3 B4, edit.contentAwareFill; SPEC.md COLOR-06「修復」): Photoshop's window over the
// op content_aware_fill (render/ops_retouch) — サンプリング領域 (自動・長方形 and its margin・カスタム: an alpha channel's white
// as the area copied from), カラー適応, 回転適応, 拡大・縮小, 左右に反転, 出力先 (現在のレイヤー・新規レイヤー・レイヤーを複製) —
// the result shown on the page while it changes (the window's preview, 250 ms after the last change, worked out on a
// worker thread from the book as it is: a change of the settings, turning the preview off or closing the window stops
// the one in hand), then 塗りつぶす — the preview's result itself when it is of these settings and done.

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QPushButton>
#include <QTimer>
#include <QtConcurrent>

#include <memory>
#include <stop_token>

#include "app/adjust_forms.hpp"
#include "app/ask.hpp"
#include "app/main_window.hpp"
#include "core/command_bus.hpp"
#include "core/ids.hpp"
#include "render/ops_registry.hpp"
#include "render/raster_ops.hpp"
#include "render/segment/segment.hpp"

namespace genko::app {

using core::Json;

namespace {

// A preview being worked out: the ops as applied (their targets laid in) and the book they are applied to; when it is
// done, its result and the ids new_id() gave meanwhile.
struct PreviewJob {
    std::stop_source stop;
    Json targeted;
    DocPtr base;
    bool done = false;
    std::optional<core::ApplyResult> result;
    std::vector<std::string> taken;
};

struct Worked {
    std::optional<core::ApplyResult> result;
    std::vector<std::string> taken;
};

}  // namespace

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
    const std::string made_id = core::new_id();  // (a new layer's or copy's id, chosen now: the preview's is the fill's)
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
    const auto with = [&op, &made_id](const Json& values) {
        Json out = op;
        for (const auto& [key, value] : values.items()) {
            if (!value.is_null()) out[key] = value;
        }
        if (out.value("output", std::string("current")) != "current") out["new_id"] = made_id;
        return out;
    };
    auto* check = new QCheckBox(QStringLiteral("プレビュー（ページで見る）"));
    check->setObjectName(QStringLiteral("content_aware_preview"));
    check->setChecked(true);
    layout->addRow(QString(), check);
    auto* timer = new QTimer(&dialog);
    timer->setSingleShot(true);
    timer->setInterval(250);
    // (the preview in hand: stopped as soon as the settings change, the preview is turned off or the window goes)
    const auto job = std::make_shared<std::shared_ptr<PreviewJob>>();
    const auto stop_job = [job] {
        if (*job) (*job)->stop.request_stop();
        job->reset();
    };
    connect(form, &adjust_forms::Form::changed, &dialog, [timer, stop_job] {
        stop_job();
        timer->start();
    });
    connect(check, &QCheckBox::toggled, &dialog, [timer, stop_job] {
        stop_job();
        timer->start();
    });
    connect(timer, &QTimer::timeout, &dialog, [this, &dialog, check, form, with, job, stop_job] {
        stop_job();
        if (!check->isChecked()) {
            show_preview(std::nullopt);
            return;
        }
        auto next = std::make_shared<PreviewJob>();
        try {
            render::use_brushes_of(book());
            next->targeted = with_contents_target(with_channel_target(Json::array({with(form->values())})));
        } catch (const std::exception&) {
            show_preview(std::nullopt);  // (a preview that cannot be made must not stop the window)
            return;
        }
        next->base = session_->snapshot();
        *job = next;
        auto* watcher = new QFutureWatcher<Worked>(&dialog);
        connect(watcher, &QFutureWatcher<Worked>::finished, &dialog, [this, watcher, next, job] {
            Worked worked = watcher->result();
            watcher->deleteLater();
            if (*job != next || next->stop.stop_requested()) return;  // (not wanted any more)
            next->done = true;
            next->taken = std::move(worked.taken);
            if (worked.result) {
                next->result = std::move(worked.result);
                show_preview(next->result->doc);
            } else {
                show_preview(std::nullopt);  // (one that cannot be made: the page as it is)
            }
        });
        watcher->setFuture(QtConcurrent::run([base = next->base, ops = next->targeted, actor = session_->actor(), token = next->stop.get_token()]() -> Worked {
            const render::segment::StopScope scope(token);
            core::ScopedIdScript script;
            try {
                core::ApplyResult result = core::CommandBus(render::ops_registry()).apply(*base, ops, core::Actor(actor));
                return Worked{std::move(result), script.taken()};
            } catch (const std::exception&) {  // (stopped — render::Cancelled —, or refused: no preview)
                return Worked{};
            }
        }));
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
    const std::shared_ptr<PreviewJob> worked = *job;
    stop_job();
    show_preview(std::nullopt);
    if (!ok || !still(asked)) return;
    const Json ops = Json::array({with(values)});
    // (the preview of these very settings, done on the book as it is: its result taken, not worked out again)
    if (worked && worked->done && worked->result && check->isChecked()) {
        try {
            if (worked->targeted == with_contents_target(with_channel_target(ops))) {
                apply_worked(ops, worked->targeted, worked->base, std::move(*worked->result), worked->taken);
                return;
            }
        } catch (const std::exception&) {  // (worked out again below)
        }
    }
    apply_ops(ops);
}

}  // namespace genko::app
