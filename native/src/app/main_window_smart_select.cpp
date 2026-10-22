// PC3 B3 (SPEC.md COLOR-06「賢い選択」「マッティング」, the user's decisions 4, 5 and 6): the smart selection in the window —
// クイック選択ツール (a stroke grows over what is like it, up to its edges; Alt takes away) and オブジェクト選択ツール (a rectangle
// dragged round a thing), 選択範囲 → フォーカス領域…・被写体を選択・空を選択, レイヤー → すべてのオブジェクトをマスク and
// マッティング ▸ カラーの除去…・フリンジ削除…・黒マット削除・白マット削除. The selection is the window's (decision 4): each finds
// an area of the area language (render/select_smart: classical algorithms, decision 6) on a worker thread from the book as
// it is — a progress with やめる (Esc) while it works — and joins it to the selection; the masks and the matting are ops
// (one Undo). 選択とマスク… is select_mask_window.cpp.

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QMenu>
#include <QProgressDialog>
#include <QSpinBox>
#include <QtConcurrent>

#include <cmath>
#include <stop_token>

#include "app/ask.hpp"
#include "app/canvas.hpp"
#include "app/main_window.hpp"
#include "app/theme.hpp"
#include "core/pyconv.hpp"
#include "render/page.hpp"
#include "render/raster_ops.hpp"
#include "render/segment/segment.hpp"
#include "render/selection.hpp"
#include "service/wording.hpp"

namespace genko::app {

using core::Json;

namespace {

double r3(double v) { return core::py_round(v, 3); }

// What the worker found: the area (none: nothing), or why not.
struct Found {
    std::optional<Json> area;
    bool stopped = false;
    QString error;
};

}  // namespace

// The work in hand: stopped by its progress's やめる (or Esc), by a newer one, or when the window goes.
struct MainWindow::SmartJob {
    std::stop_source stop;
    QPointer<QProgressDialog> progress;
};

void MainWindow::build_smart_select_actions() {
    const auto tool = [this](const char* attribute, const QString& title, const QString& name, const QString& tip) {
        QAction* act = make(QString::fromLatin1(attribute), title, [this, name] { choose_tool(name); }, {}, tip, true);
        act->setAutoRepeat(false);
        tools_->addAction(act);
        tool_actions_[name] = act;
    };
    tool("act_object_select", QStringLiteral("オブジェクト選択ツール"), QStringLiteral("object"),
         QStringLiteral("ドラッグで囲んだ中の物を選びます（Shift で追加・Alt で一部削除）"));
    tool("act_quick_select", QStringLiteral("クイック選択ツール"), QStringLiteral("quick"),
         QStringLiteral("なぞった所から、似た色の所を縁まで選びます（続けてなぞると追加、Alt で一部削除）"));
    make("act_focus_area", QStringLiteral("フォーカス領域…"), [this] { focus_area_dialog(); }, {},
         QStringLiteral("ピントの合った（くっきりした）所を選びます"));
    make("act_select_subject", QStringLiteral("被写体を選択"), [this] { select_subject(); }, {},
         QStringLiteral("ページでいちばん目立つ物を選びます（写真向け。線画では外れることがあります）"));
    make("act_select_sky", QStringLiteral("空を選択"), [this] { select_sky(); }, {}, QStringLiteral("写真の空を選びます"));
    make("act_select_mask", QStringLiteral("選択とマスク…"), [this] { select_and_mask(); }, {QKeySequence(QStringLiteral("Ctrl+Alt+R"))},
         QStringLiteral("選択範囲の縁を、絵の縁（髪の毛・ぼけ）に合わせて整え、選択範囲・マスク・新しいレイヤーに出します"));
    make("act_mask_all_objects", QStringLiteral("すべてのオブジェクトをマスク"), [this] { mask_all_objects(); }, {},
         QStringLiteral("描く先のレイヤーで目立つ物を見つけ、それ以外を隠すマスクを付けます"));
    make("act_matting_decontaminate", QStringLiteral("カラーの除去…"), [this] { matting(QStringLiteral("decontaminate")); }, {},
         QStringLiteral("半透明の縁に残った背景の色を、中の色に置き換えます"));
    make("act_matting_defringe", QStringLiteral("フリンジ削除…"), [this] { matting(QStringLiteral("defringe")); }, {},
         QStringLiteral("縁から決めた幅の色を、内側の色に置き換えます"));
    make("act_matting_black", QStringLiteral("黒マット削除"), [this] { matting(QStringLiteral("remove_black")); }, {},
         QStringLiteral("黒の上で切り抜いた縁の暗さを取ります"));
    make("act_matting_white", QStringLiteral("白マット削除"), [this] { matting(QStringLiteral("remove_white")); }, {},
         QStringLiteral("白の上で切り抜いた縁の白さを取ります"));
    connect(canvas_, &PageCanvas::quickSelected, this, &MainWindow::quick_selected);
    connect(canvas_, &PageCanvas::objectSelected, this, &MainWindow::object_selected);
}

void MainWindow::build_matting_menu(QMenu* layers) {
    QMenu* menu = layers->addMenu(QStringLiteral("マッティング"));
    for (const char* name : {"act_matting_decontaminate", "act_matting_defringe", "act_matting_black", "act_matting_white"})
        menu->addAction(action(QString::fromLatin1(name)));
}

QWidget* MainWindow::smart_select_settings_widget() {
    // (ツールの設定 選択範囲ツール: クイック選択ツール's 直径 and エッジを強調, and what the smart selection sees)
    auto* box = new QWidget;
    box->setObjectName(QStringLiteral("smart_select_settings"));
    auto* form = new QFormLayout(box);
    form->setContentsMargins(0, 0, 0, 0);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    auto* size = new QDoubleSpinBox;
    size->setObjectName(QStringLiteral("quick_size"));
    size->setRange(0.2, 100);
    size->setDecimals(1);
    size->setSuffix(QStringLiteral(" mm"));
    size->setValue(canvas_->quick_size_mm);
    size->setToolTip(QStringLiteral("クイック選択ツールのブラシの直径"));
    connect(size, &QDoubleSpinBox::valueChanged, this, [this](double v) { canvas_->quick_size_mm = v; });
    auto* enhance = new QCheckBox(QStringLiteral("エッジを強調"));
    enhance->setObjectName(QStringLiteral("quick_enhance_edge"));
    enhance->setToolTip(QStringLiteral("クイック選択ツールの選択範囲の縁を、絵の縁に沿ってなめらかにする"));
    enhance->setChecked(smart_select_settings.enhance_edge);
    connect(enhance, &QCheckBox::toggled, this, [this](bool on) { smart_select_settings.enhance_edge = on; });
    auto* all = new QCheckBox(QStringLiteral("全レイヤーを対象"));
    all->setObjectName(QStringLiteral("smart_all_layers"));
    all->setToolTip(QStringLiteral("クイック選択・オブジェクト選択・被写体・空・フォーカス領域で、見えているページの絵で選ぶ（コマの枠線なども拾います。外すと描く先のレイヤーの絵だけ）"));
    all->setChecked(smart_select_settings.all_layers);
    connect(all, &QCheckBox::toggled, this, [this](bool on) { smart_select_settings.all_layers = on; });
    form->addRow(QStringLiteral("クイック選択の直径"), size);
    form->addRow(QString(), enhance);
    form->addRow(QStringLiteral("賢い選択"), all);
    return box;
}

// --- the worker ------------------------------------------------------------------------------------------------------

bool MainWindow::smart_select_busy() const { return smart_job_ != nullptr; }

Json MainWindow::seen_spec(Json spec) const {
    // (全レイヤーを対象 off: the drawing layer's own picture)
    if (!smart_select_settings.all_layers) {
        if (const core::Layer* layer = target_layer()) spec["layer"] = layer->id;
    }
    return spec;
}

void MainWindow::cancel_smart_select() {
    if (smart_job_) smart_job_->stop.request_stop();
}

void MainWindow::solve_smart(const Json& area, const QString& title, std::function<void(const std::optional<Json>&)> done) {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    if (smart_job_) smart_job_->stop.request_stop();  // (a newer one: the one before is not wanted)
    auto job = std::make_shared<SmartJob>();
    smart_job_ = job;
    const DocPtr doc = session_->snapshot();
    const std::size_t index = static_cast<std::size_t>(page_index_);
    const std::string page_id = page->id;
    render::use_brushes_of(book());
    auto* progress = new QProgressDialog(title + QStringLiteral("…"), QStringLiteral("やめる"), 0, 0, this);
    progress->setObjectName(QStringLiteral("smart_select_progress"));
    progress->setWindowTitle(title);
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(400);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    job->progress = progress;
    connect(progress, &QProgressDialog::canceled, this, [job] { job->stop.request_stop(); });
    auto* watcher = new QFutureWatcher<Found>(this);
    connect(watcher, &QFutureWatcher<Found>::finished, this, [this, watcher, job, page_id, title, done = std::move(done)] {
        const Found found = watcher->result();
        watcher->deleteLater();
        if (job->progress) job->progress->deleteLater();
        if (smart_job_ == job) smart_job_.reset();
        if (found.stopped || job->stop.stop_requested()) {
            flash(title + QStringLiteral("をやめました"), 2500);
            return;
        }
        if (!found.error.isEmpty()) {
            flash(wording::error(found.error), 4000, true);
            return;
        }
        const core::Page* now = current_page();
        if (now == nullptr || now->id != page_id) return;  // (another page in front by now: not this one's)
        done(found.area);
    });
    watcher->setFuture(QtConcurrent::run([doc, index, area, token = job->stop.get_token()]() -> Found {
        const render::segment::StopScope scope(token);
        try {
            const core::Page& p = doc->page(index);
            const int dpi = render::selection::grid_dpi(p);  // (a precise or ink page's finest grid)
            return Found{render::selection::from_mask(render::selection::to_mask(area, p, doc.get(), dpi), dpi), false, {}};
        } catch (const render::Cancelled&) {
            return Found{std::nullopt, true, {}};
        } catch (const std::exception& error) {
            return Found{std::nullopt, false, QString::fromUtf8(error.what())};
        }
    }));
}

// --- the tools and the menu's commands -------------------------------------------------------------------------------

void MainWindow::quick_selected(const QVector<QPointF>& points, const QString& how) {
    if (points.isEmpty()) return;
    Json stroke = Json::array();
    for (const QPointF& p : points) stroke.push_back(Json::array({r3(p.x()), r3(p.y())}));
    Json spec{{"points_mm", stroke}, {"size_mm", r3(canvas_->quick_size_mm)}};
    if (smart_select_settings.enhance_edge) spec["enhance_edge"] = true;
    solve_smart(Json{{"quick", seen_spec(spec)}}, QStringLiteral("クイック選択"), [this, how](const std::optional<Json>& area) {
        if (!area) {
            if (how == QLatin1String("replace")) flash(QStringLiteral("なぞった所から選べる所が見つかりませんでした"), 3000);
            return;
        }
        join_selection(area, how);
    });
}

void MainWindow::object_selected(const QRectF& box, const QString& how) {
    const Json spec{{"rect_mm", Json::array({r3(box.x()), r3(box.y()), r3(box.width()), r3(box.height())})}};
    solve_smart(Json{{"object", seen_spec(spec)}}, QStringLiteral("オブジェクト選択"), [this, how](const std::optional<Json>& area) {
        if (!area) {
            flash(QStringLiteral("囲んだ中に、選べる物が見つかりませんでした"), 3000);
            return;
        }
        join_selection(area, how);
    });
}

void MainWindow::select_subject() {
    solve_smart(Json{{"subject", seen_spec(Json::object())}}, QStringLiteral("被写体を選択"), [this](const std::optional<Json>& area) {
        if (!area) {
            flash(QStringLiteral("被写体が見つかりませんでした"), 3000);
            return;
        }
        if (canvas_->tool() != QLatin1String("marquee")) choose_tool(QStringLiteral("rect"));
        join_selection(area, QStringLiteral("replace"));
    });
}

void MainWindow::select_sky() {
    solve_smart(Json{{"sky", seen_spec(Json::object())}}, QStringLiteral("空を選択"), [this](const std::optional<Json>& area) {
        if (!area) {
            flash(QStringLiteral("空が見つかりませんでした"), 3000);  // (Photoshop's: the selection left alone)
            return;
        }
        if (canvas_->tool() != QLatin1String("marquee")) choose_tool(QStringLiteral("rect"));
        join_selection(area, QStringLiteral("replace"));
    });
}

void MainWindow::focus_area_dialog() {
    if (current_page() == nullptr) return;
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("focus_area_dialog"));
    dialog.setWindowTitle(QStringLiteral("フォーカス領域"));
    auto* form = new QFormLayout(&dialog);
    auto* range = new QSpinBox;
    range->setObjectName(QStringLiteral("focus_range"));
    range->setRange(0, 100);
    range->setValue(50);
    range->setSuffix(QStringLiteral(" %"));
    range->setToolTip(QStringLiteral("大きいほど、ピントの甘い所まで選びます"));
    auto* noise = new QSpinBox;
    noise->setObjectName(QStringLiteral("focus_noise"));
    noise->setRange(0, 100);
    noise->setSuffix(QStringLiteral(" %"));
    noise->setToolTip(QStringLiteral("ざらつき（ノイズ）の多い絵では上げます"));
    form->addRow(QStringLiteral("焦点が合った範囲"), range);
    form->addRow(QStringLiteral("画像ノイズのレベル"), noise);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);
    const Asked asked = asking();
    if (ask::exec(&dialog) != QDialog::Accepted || !still(asked)) return;
    const Json spec = seen_spec(Json{{"range", r3(range->value() / 100.0)}, {"noise", r3(noise->value() / 100.0)}});
    solve_smart(Json{{"focus", spec}}, QStringLiteral("フォーカス領域"), [this](const std::optional<Json>& area) {
        if (!area) {
            flash(QStringLiteral("ピントの合った所が見つかりませんでした"), 3000);
            return;
        }
        if (canvas_->tool() != QLatin1String("marquee")) choose_tool(QStringLiteral("rect"));
        join_selection(area, QStringLiteral("replace"));
    });
}

void MainWindow::mask_all_objects() {
    // (the drawing layer's own picture; the things found shown, the rest hidden — a new mask, linked)
    const core::Page* page = current_page();
    const core::Layer* layer = target_layer();
    if (page == nullptr || layer == nullptr) return;
    if (layer->kind == core::LayerKind::Folder) {
        flash(QStringLiteral("フォルダにはマスクを付けられません（中のレイヤーに付けます）"), 3000);
        return;
    }
    const std::string id = layer->id;
    solve_smart(Json{{"subject", Json{{"layer", id}}}}, QStringLiteral("すべてのオブジェクトをマスク"), [this, id](const std::optional<Json>& area) {
        const core::Page* now = current_page();
        if (now == nullptr) return;
        if (!area) {
            flash(QStringLiteral("オブジェクトが見つかりませんでした"), 3000);
            return;
        }
        if (apply_ops(Json::array({Json{{"op", "set_layer_mask"}, {"page", now->index.json()}, {"id", id}, {"area", *area}, {"linked", true}}})))
            flash(QStringLiteral("見つかった物のほかを隠すマスクを付けました"), 3000);
    });
}

void MainWindow::matting(const QString& kind) {
    const core::Page* page = current_page();
    const core::Layer* layer = page != nullptr ? paint_layer() : nullptr;
    if (layer == nullptr) return;
    Json op{{"op", "matting"}, {"page", page->index.json()}, {"id", layer->id}, {"kind", kind.toStdString()}};
    const Asked asked = asking();
    if (kind == QLatin1String("decontaminate")) {
        const auto amount = ask::get_double(this, QStringLiteral("カラーの除去"), QStringLiteral("適用量（%）"), 100, 0, 100, 0);
        if (!amount || !still(asked)) return;
        op["amount"] = *amount;
    } else if (kind == QLatin1String("defringe")) {
        const auto width = ask::get_double(this, QStringLiteral("フリンジ削除"), QStringLiteral("幅（mm）"), 0.3, 0.05, 20, 2);
        if (!width || !still(asked)) return;
        op["width_mm"] = *width;
    }
    if (const auto area = selection_area()) op["area"] = *area;
    apply_ops(Json::array({op}));
}

}  // namespace genko::app
