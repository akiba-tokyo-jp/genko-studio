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
#include <QPushButton>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QVBoxLayout>

#include "app/ask.hpp"
#include "app/layer_panel.hpp"
#include "app/plugins_dialog.hpp"
#include "app/main_window.hpp"
#include "app/theme.hpp"
#include "app/timeline.hpp"
#include "app/wording.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "render/movie.hpp"
#include "render/plugins.hpp"
#include "render/timelapse.hpp"

namespace genko::app {

using core::Json;

void MainWindow::build_anim_actions() {
    make("act_timeline", QStringLiteral("アニメーション（タイムライン）"), [this] { show_dock(QStringLiteral("タイムライン")); }, {},
         QStringLiteral("このページを短いアニメーションにします: セル・タイムライン・オニオンスキン・カメラワーク・書き出し"));
    make("act_timelapse", QStringLiteral("タイムラプスを記録する"), [this] { toggle_timelapse(action("act_timelapse")->isChecked()); }, {},
         QStringLiteral("保存のたびに、変わったページの小さな絵を残します（制作過程の動画にできます）"), true);
    make("act_timelapse_export", QStringLiteral("タイムラプスを書き出す…"), [this] { export_timelapse(); }, {},
         QStringLiteral("記録した制作過程を動く画像（WebP・GIF・PNG・MP4）に"));
    // フィルターのプラグイン (render/plugins.hpp: off until chosen, each in its own process)
    make("act_plugins", QStringLiteral("プラグインのフォルダーを開く"), [] {
        std::error_code ec;
        std::filesystem::create_directories(render::plugins::folder(), ec);
        QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(core::path_to_utf8(render::plugins::folder()))));
    }, {}, QStringLiteral("フィルターのプラグイン（.py）を入れるフォルダー"));
    make("act_plugin_settings", QStringLiteral("プラグインの設定…"), [this] {
        PluginsDialog dialog(this);
        if (ask::exec(&dialog) == QDialog::Accepted && layer_panel_ != nullptr) layer_panel_->reload_filters();
    }, {}, QStringLiteral("プラグインを使うか、どの Python で、どれを動かしてよいか"));
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
}

std::int64_t MainWindow::current_frame(const core::Page* page) const {
    if (page == nullptr) page = current_page();
    if (page == nullptr) return 1;
    const auto found = anim_frames.find(page->id);
    return found != anim_frames.end() ? found->second : 1;
}

void MainWindow::show_dock(const QString& title) {
    for (QDockWidget* dock : findChildren<QDockWidget*>()) {
        if (dock->windowTitle() == title) {
            dock->show();
            dock->raise();
        }
    }
    if (title == QLatin1String("タイムライン") && timeline_ != nullptr) timeline_->refresh();
}

void MainWindow::toggle_timelapse(bool on) {
    QAction* act = action("act_timelapse");
    if (on && !session_->path()) {
        flash(QStringLiteral("タイムラプスの前に、原稿を保存します（ファイル → 別の場所に保存）"), 6000);
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
    session_->save_now();
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
    QApplication::setOverrideCursor(Qt::WaitCursor);
    try {
        written = write(core::path_from_utf8(path.toStdString()));
    } catch (const std::exception& error) {
        QApplication::restoreOverrideCursor();
        ask::warning(this, QStringLiteral("Genko"), wording::error(QString::fromUtf8(error.what())));
        return;
    }
    QApplication::restoreOverrideCursor();
    accept();
}

}  // namespace genko::app
