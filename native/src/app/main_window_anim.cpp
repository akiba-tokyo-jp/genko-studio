// Animation and the timelapse in the window (Python's main.py: act_timeline and the タイムライン panel, act_timelapse,
// act_timelapse_export with its TimelapseDialog from dialogs.py; the panel itself is app/timeline.cpp), and the filter
// plugins' commands (act_plugins; act_plugin_settings, COMP-04's choice: app/plugins_dialog.cpp).

#include <QApplication>
#include <QComboBox>
#include <QDesktopServices>
#include <QUrl>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QVBoxLayout>

#include <algorithm>

#include "app/ask.hpp"
#include "app/layer_panel.hpp"
#include "app/plugins_dialog.hpp"
#include "app/main_window.hpp"
#include "app/theme.hpp"
#include "app/stoppable.hpp"
#include "app/timeline.hpp"
#include "service/wording.hpp"
#include "core/anim.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "render/movie.hpp"
#include "render/plugins.hpp"
#include "render/timelapse.hpp"

namespace genko::app {

using core::Json;

void MainWindow::build_anim_actions() {
    // (PC5 J-e1, 節 J 1.2: Photoshop's ビデオタイムラインを作成 — its id kept, the old name in preferences::renamed(); the panel's
    // switch is ウィンドウ → タイムライン): the panel shown, and the page made an animation when it is not one yet
    make("act_timeline", QStringLiteral("ビデオタイムラインを作成"), [this] {
        show_dock(QStringLiteral("タイムライン"));
        const core::Page* page = current_page();
        if (timeline_ != nullptr && page != nullptr && !core::anim::is_animation(*page)) timeline_->start_animation();
    }, {}, QStringLiteral("このページを短いアニメーションにします: セル・タイムライン・オニオンスキン・カメラワーク・書き出し"));
    // (PC5 J-e1, timeline.nextFrame・previousFrame・setFrame: the frame shown moved — the window's, not the book's. Their keys
    // work while the タイムライン panel has the focus: ← →, Home End, Shift with ← → to the work area's ends — the same in
    // both key sets, never the canvas's arrows)
    const auto go = [this](const char* where) {
        return [this, where] {
            if (timeline_ != nullptr) timeline_->go(QString::fromLatin1(where));
        };
    };
    make("act_next_frame", QStringLiteral("次のフレーム"), go("next"), QList<QKeySequence>{QKeySequence(Qt::Key_Right)},
         QStringLiteral("タイムラインの次のフレームを見ます"));
    make("act_previous_frame", QStringLiteral("前のフレーム"), go("previous"), QList<QKeySequence>{QKeySequence(Qt::Key_Left)},
         QStringLiteral("タイムラインの前のフレームを見ます"));
    make("act_first_frame", QStringLiteral("最初のフレーム"), go("first"), QList<QKeySequence>{QKeySequence(Qt::Key_Home)},
         QStringLiteral("タイムラインの最初のフレームを見ます"));
    make("act_last_frame", QStringLiteral("最後のフレーム"), go("last"), QList<QKeySequence>{QKeySequence(Qt::Key_End)},
         QStringLiteral("タイムラインの最後のフレームを見ます"));
    make("act_work_start", QStringLiteral("作業範囲の開始点へ移動"), go("work_start"), QList<QKeySequence>{QKeySequence(Qt::SHIFT | Qt::Key_Left)},
         QStringLiteral("作業範囲の最初のフレームを見ます（作業範囲が無ければ最初のフレーム）"));
    make("act_work_end", QStringLiteral("作業範囲の終了点へ移動"), go("work_end"), QList<QKeySequence>{QKeySequence(Qt::SHIFT | Qt::Key_Right)},
         QStringLiteral("作業範囲の最後のフレームを見ます（作業範囲が無ければ最後のフレーム）"));
    make("act_go_to_frame", QStringLiteral("フレームへ移動…"), [this] {
        if (timeline_ != nullptr) timeline_->ask_frame();
    }, {}, QStringLiteral("番号を決めて、そのフレームを見ます"));
    // (PC5 J-e1, file.export.renderVideo: ファイル → 書き出し ▸ and the timeline's menu)
    make("act_render_video", QStringLiteral("ビデオをレンダリング…"), [this] {
        if (timeline_ != nullptr) timeline_->export_animation();
    }, {}, QStringLiteral("アニメーションのページを、動く画像（GIF・WebP・APNG・MP4）か連番 PNG に書き出します（範囲・大きさ・フレームレート）"));
    make("act_timelapse", QStringLiteral("タイムラプスを記録する"), [this] { toggle_timelapse(action("act_timelapse")->isChecked()); }, {},
         QStringLiteral("保存のたびに、変わったページの小さな絵を残します（制作過程の動画にできます）"), true);
    make("act_timelapse_export", QStringLiteral("タイムラプスを書き出す…"), [this] { export_timelapse(); }, {},
         QStringLiteral("記録した制作過程を動く画像（WebP・GIF・PNG・MP4）に"));
    // フィルターのプラグイン (render/plugins.hpp: off until chosen, each in its own process)
    make("act_plugins", QStringLiteral("プラグインのフォルダーを開く"), [] {
        std::error_code ec;
        std::filesystem::create_directories(render::plugins::folder(), ec);
        QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(core::path_to_utf8(render::plugins::folder()))));
    }, {}, QStringLiteral("フィルターのプラグイン（.wasm・.py・プログラムのフォルダー）を入れるフォルダー"));
    // (PC5 J-f, edit.preferences.plugIns: Photoshop's 環境設定 ▸ プラグイン… — the old プラグインの設定…, preferences::renamed())
    make("act_plugin_settings", QStringLiteral("プラグイン…"), [this] {
        PluginsDialog dialog(this);
        if (ask::exec(&dialog) == QDialog::Accepted) follow_filter_menu();  // (フィルター → プラグイン: the plugins chosen)
    }, {}, QStringLiteral("プラグインを使うか、追加のプラグインフォルダー、どの Python で、どれを動かしてよいか"));
    // (PC5 J-f, plugin.install: フィルター → プラグイン ▸ プラグインをインストール… — copied into the plugins' folder, not chosen)
    make("act_plugin_install", QStringLiteral("プラグインをインストール…"), [this] {
        const QString path = ask::open_path(this, QStringLiteral("プラグインをインストール"), QStringLiteral("プラグイン (*.wasm *.py)"));
        if (path.isEmpty()) return;
        try {
            const std::string key = render::plugins::install(core::path_from_utf8(path.toStdString()));
            flash(QStringLiteral("プラグイン「%1」を入れました。動かすには 編集 → 環境設定 → プラグイン… で選びます").arg(QString::fromStdString(key)), 6000);
        } catch (const std::exception& error) {
            ask::warning(this, QStringLiteral("プラグインをインストール"), wording::error(QString::fromUtf8(error.what())));
        }
    }, {},
         QStringLiteral("WebAssembly のプラグイン（.wasm）か Python のプラグイン（.py）をプラグインのフォルダーに入れます（動かすには 環境設定 → プラグイン… で選びます）"));
}

void MainWindow::build_anim_dock() {
    timeline_ = new TimelinePanel(this);
    timeline_dock_ = new QDockWidget(QStringLiteral("タイムライン"), this);
    timeline_dock_->setObjectName(QStringLiteral("タイムライン"));
    timeline_dock_->setWidget(timeline_);
    timeline_dock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::LeftDockWidgetArea, timeline_dock_);
    tabifyDockWidget(navigator_dock_, timeline_dock_);
    navigator_dock_->raise();
    view_menu_->addAction(timeline_dock_->toggleViewAction());
    timeline_dock_->hide();  // (for some work only: the menu brings it)
    // (PC5 J-e1) the moving commands' keys while the panel has the focus only (the canvas keeps its arrows), the panel's menu
    // and buttons, and ビデオをレンダリング… in ファイル → 書き出し ▸ right after パスを Illustrator へ… (Photoshop's order)
    for (const char* name : {"act_next_frame", "act_previous_frame", "act_first_frame", "act_last_frame", "act_work_start", "act_work_end"}) {
        QAction* act = action(QString::fromLatin1(name));
        act->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        removeAction(act);
        timeline_->addAction(act);
    }
    timeline_->build_menu();
    if (export_menu_ != nullptr) {
        const QList<QAction*> exports = export_menu_->actions();
        const auto paths = std::find(exports.begin(), exports.end(), action(QStringLiteral("act_export_paths_ai")));
        if (paths != exports.end() && std::next(paths) != exports.end()) {
            export_menu_->insertAction(*std::next(paths), action(QStringLiteral("act_render_video")));
        } else {
            export_menu_->addAction(action(QStringLiteral("act_render_video")));
        }
    }
}

std::int64_t MainWindow::current_frame(const core::Page* page) const {
    if (page == nullptr) page = current_page();
    if (page == nullptr) return 1;
    const auto found = anim_frames.find(page->id);
    return found != anim_frames.end() ? found->second : 1;
}

void MainWindow::show_dock(const QString& title) {
    for (QDockWidget* dock : findChildren<QDockWidget*>()) {
        if (dock->windowTitle() == title || dock->objectName() == title) {  // (PC5 H-0: or its name before Photoshop's)
            dock->show();
            dock->raise();
        }
    }
    if (title == QLatin1String("タイムライン") && timeline_ != nullptr) timeline_->refresh();
}

void MainWindow::toggle_timelapse(bool on) {
    QAction* act = action("act_timelapse");
    if (on && !session_->path()) {
        flash(QStringLiteral("タイムラプスの前に、原稿を保存します（ファイル → 別名で保存…）"), 6000);
        const QSignalBlocker quiet(act);
        act->setChecked(false);
        return;
    }
    if (apply_ops(Json::array({Json{{"op", "set_timelapse"}, {"on", on}}}))) {
        session_->save_now();
        flash(on ? QStringLiteral("タイムラプスを記録しています（保存のたびに 1 コマ）") : QStringLiteral("タイムラプスの記録を止めました"), 4000);
    } else {
        const QSignalBlocker quiet(act);
        act->setChecked(render::timelapse::is_on(book()));
    }
}

void MainWindow::export_timelapse() {
    if (!session_->path()) {
        flash(QStringLiteral("原稿を保存してから使えます"), 5000);
        return;
    }
    // (the frames of the save made now are counted and written too: the save is waited for, as Python's commit_now)
    session_->save_now();
    if (!session_->wait_saved(std::chrono::seconds(30))) {
        flash(QStringLiteral("保存がまだ終わっていません。最後の変更のコマは、次に書き出すときに入ります"), 5000);
    }
    if (!session_->path()) return;
    const core::Page* page = current_page();
    TimelapseDialog dialog(this, *session_->path(), page != nullptr ? page->index.json() : Json(1));
    ask::exec(&dialog);
}

// --- タイムラプスを書き出す ----------------------------------------------------------------------------------------

TimelapseDialog::TimelapseDialog(QWidget* parent, std::filesystem::path project, const Json& current_page)
    : QDialog(parent), project_(std::move(project)), current_page_(current_page) {
    setWindowTitle(QStringLiteral("タイムラプスを書き出す"));
    which = new QComboBox;
    which->addItem(QStringLiteral("全ページ（描いた順）"), QStringLiteral("all"));
    which->addItem(QStringLiteral("このページだけ（%1 ページ）").arg(QString::fromStdString(core::py_str(current_page_))), QStringLiteral("page"));
    movie = new QComboBox;
    for (const auto& [label, key] : {std::pair{QStringLiteral("WebP（動く画像・軽い）"), QStringLiteral("webp")}, {QStringLiteral("GIF"), QStringLiteral("gif")},
                                     {QStringLiteral("PNG（APNG）"), QStringLiteral("png")}, {QStringLiteral("MP4（動画）"), QStringLiteral("mp4")}}) {
        movie->addItem(label, key);
    }
    if (!render::movie::ffmpeg()) {  // (MP4 needs ffmpeg; the others need nothing)
        if (auto* model = qobject_cast<QStandardItemModel*>(movie->model())) {
            QStandardItem* item = model->item(movie->findData(QStringLiteral("mp4")));
            item->setEnabled(false);
            item->setToolTip(QStringLiteral("ffmpeg が入っていないので使えません"));
        }
    }
    fps = new QDoubleSpinBox;
    fps->setRange(1, 60);
    fps->setDecimals(0);
    fps->setValue(12);
    fps->setSuffix(QStringLiteral(" コマ／秒"));
    seconds = new QDoubleSpinBox;
    seconds->setRange(0, 600);
    seconds->setDecimals(0);
    seconds->setValue(0);
    seconds->setSpecialValueText(QStringLiteral("すべてのコマ"));
    seconds->setSuffix(QStringLiteral(" 秒に収める"));
    counted = new QLabel;
    theme::role(counted, "hint");
    auto* form = new QFormLayout;
    form->addRow(QStringLiteral("ページ"), which);
    form->addRow(QStringLiteral("形式"), movie);
    form->addRow(QStringLiteral("速さ"), fps);
    form->addRow(QStringLiteral("長さ"), seconds);
    form->addRow(QString(), counted);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("書き出す…"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("やめる"));
    connect(buttons, &QDialogButtonBox::accepted, this, [this] { run(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(buttons);
    connect(which, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { count(); });
    count();
}

std::optional<Json> TimelapseDialog::page() const {
    if (which->currentData().toString() == QLatin1String("page")) return current_page_;
    return std::nullopt;
}

int TimelapseDialog::count() {
    const auto n = static_cast<int>(render::timelapse::frames(project_, page()).size());
    counted->setText(n != 0 ? QStringLiteral("記録したコマ: %1").arg(n) : QStringLiteral("まだ記録がありません（ファイル → タイムラプスを記録する）"));
    return n;
}

std::filesystem::path TimelapseDialog::write(const std::filesystem::path& dest) {
    std::optional<double> fit;
    if (seconds->value() != 0) fit = seconds->value();
    return render::timelapse::export_timelapse(project_, dest, page(), fps->value(), fit, movie->currentData().toString().toStdString());
}

void TimelapseDialog::run() {
    if (count() == 0) return;
    const QString ext = movie->currentData().toString();
    const std::filesystem::path start =
        project_.parent_path() / core::path_from_utf8(core::path_to_utf8(project_.stem()) + "_timelapse." + ext.toStdString());
    const QString path = ask::save_path(this, QStringLiteral("タイムラプスの保存先"), QString::fromStdString(core::path_to_utf8(start)),
                                        QStringLiteral("*.%1").arg(ext));
    if (path.isEmpty()) return;
    // (PC6, the review's P2-4: written on a worker with its progress window and 中止 — app/stoppable.hpp —, as write() writes
    // it: the recorded pictures read and the movie made there, MP4's ffmpeg waited for there and stopped by 中止 too)
    std::optional<double> fit;
    if (seconds->value() != 0) fit = seconds->value();
    const QPointer<TimelapseDialog> alive(this);
    const QString title = QStringLiteral("タイムラプスを書き出し");
    // (the file put in place: kept here even when 中止 came after it — the review's P3-3 —, as ビデオをレンダリング does)
    auto whole = std::make_shared<std::optional<std::filesystem::path>>();
    try {
        (void)run_stoppable(this, title, QStringLiteral("タイムラプスを書き出しています…"),
                            [project = project_, dest = core::path_from_utf8(path.toStdString()), asked = page(), rate = fps->value(), fit,
                             format = movie->currentData().toString().toStdString(), whole](const std::function<bool()>&) {
                                *whole = render::timelapse::export_timelapse(project, dest, asked, rate, fit, format);
                                return true;
                            });
    } catch (const std::exception& error) {
        if (alive) ask::warning(this, QStringLiteral("Genko"), wording::error(QString::fromUtf8(error.what())));
        return;
    }
    if (!alive) return;
    if (!*whole) {  // (stopped: nothing written — the movie is written beside its name and moved there whole —, said here)
        counted->setText(QStringLiteral("タイムラプスの書き出しを中止しました（何も書き出していません）"));
        return;
    }
    written = std::move(*whole);
    accept();
}

}  // namespace genko::app
