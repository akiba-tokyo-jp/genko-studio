// PC5 J-a (layer-comps@1, SPEC.md PROD-01「レイヤーカンプ」): ウィンドウ → レイヤーカンプ (the dock and its panel, app/
// layer_comps_panel) and ファイル → 書き出し → レイヤーカンプからファイル… — each change of the book an op through apply_ops
// (render/ops_layer_comps), the files written by formats::export_layer_comps off the window's thread.

#include <QAction>
#include <QDir>
#include <QDockWidget>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QSignalBlocker>
#include <QCheckBox>
#include <QComboBox>
#include <QSpinBox>

#include <algorithm>
#include <cmath>

#include "app/ask.hpp"
#include "app/layer_comps_panel.hpp"
#include "app/main_window.hpp"
#include "app/off_ui.hpp"
#include "core/paths.hpp"
#include "formats/copy.hpp"
#include "service/wording.hpp"

namespace genko::app {

using core::Json;
namespace fs = std::filesystem;

namespace {

QString qpath(const fs::path& path) { return QString::fromStdString(core::path_to_utf8(path)); }
fs::path fpath(const QString& path) { return core::path_from_utf8(path.toStdString()); }

}  // namespace

void MainWindow::build_layer_comp_actions() {
    make(QStringLiteral("act_comps_to_files"), QStringLiteral("レイヤーカンプからファイル…"), [this] { layer_comps_to_files(); }, {},
         QStringLiteral("このページのレイヤーカンプを 1 つずつ適用した画像を、1 枚ずつ書き出します"));
    make(QStringLiteral("act_panel_layer_comps"), QStringLiteral("レイヤーカンプ"),
         [this] {
             if (layer_comps_dock_ == nullptr) return;
             const bool show = !layer_comps_dock_->isVisible();
             layer_comps_dock_->setVisible(show);
             if (show) layer_comps_dock_->raise();
         },
         {}, QStringLiteral("レイヤーの表示・位置・アピアランスの組を残して切り替えます"), true);
}

void MainWindow::build_layer_comps_dock() {
    layer_comps_panel_ = new LayerCompsPanel(this);
    layer_comps_dock_ = new QDockWidget(QStringLiteral("レイヤーカンプ"), this);
    layer_comps_dock_->setObjectName(QStringLiteral("レイヤーカンプ"));
    layer_comps_dock_->setWidget(layer_comps_panel_);
    layer_comps_dock_->setMinimumWidth(200);
    layer_comps_dock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::RightDockWidgetArea, layer_comps_dock_);
    if (layers_dock_ != nullptr) tabifyDockWidget(layers_dock_, layer_comps_dock_);  // (beside the layers, as the paths)
    layer_comps_dock_->hide();  // (ウィンドウ → レイヤーカンプ brings it)
    QAction* toggle = action(QStringLiteral("act_panel_layer_comps"));
    view_menu_->addAction(toggle);
    connect(layer_comps_dock_, &QDockWidget::visibilityChanged, this, [this, toggle](bool) {
        const QSignalBlocker quiet(toggle);
        toggle->setChecked(layer_comps_dock_->isVisible());
        if (layer_comps_dock_->isVisible() && layer_comps_panel_ != nullptr) layer_comps_panel_->refresh();
    });
}

bool MainWindow::layer_comp_op(Json op) {
    const core::Page* page = current_page();
    if (page == nullptr) return false;
    op["page"] = page->index.json();
    const bool done = apply_ops(Json::array({std::move(op)}));
    if (layer_comps_panel_ != nullptr) layer_comps_panel_->refresh();
    return done;
}

void MainWindow::layer_comp_options(std::optional<std::int64_t> comp) {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    const core::LayerComp* now = nullptr;
    if (comp && page->layer_comps) {
        for (const core::LayerComp& c : page->layer_comps->comps)
            if (c.id == *comp) now = &c;
        if (now == nullptr) return;
    }
    QString name;
    if (now != nullptr) {
        name = QString::fromStdString(now->name);
    } else {  // (レイヤーカンプ 1, 2 …: the first free, as the op names one)
        for (int n = 1;; ++n) {
            const std::string candidate = "レイヤーカンプ " + std::to_string(n);
            const bool taken = page->layer_comps && std::any_of(page->layer_comps->comps.begin(), page->layer_comps->comps.end(),
                                                                [&](const core::LayerComp& c) { return c.name == candidate; });
            if (!taken) {
                name = QString::fromStdString(candidate);
                break;
            }
        }
    }
    LayerCompOptionsDialog dialog(this, now != nullptr ? QStringLiteral("レイヤーカンプオプション") : QStringLiteral("新規レイヤーカンプ"), name,
                                  now == nullptr || now->apply_visibility, now == nullptr || now->apply_position, now == nullptr || now->apply_appearance,
                                  now != nullptr ? QString::fromStdString(now->comment) : QString());
    if (ask::exec(&dialog) != QDialog::Accepted) return;
    Json op{{"op", now != nullptr ? "set_layer_comp" : "new_layer_comp"},
            {"name", dialog.name->text().trimmed().toStdString()},
            {"comment", dialog.comment->toPlainText().toStdString()},
            {"visibility", dialog.visibility->isChecked()},
            {"position", dialog.position->isChecked()},
            {"appearance", dialog.appearance->isChecked()}};
    if (now != nullptr) op["comp"] = now->id;
    layer_comp_op(std::move(op));
}

void MainWindow::layer_comps_to_files() {
    const std::shared_ptr<const core::Document> doc = contents_ ? shown_book() : session_->snapshot();
    const core::Page* page = current_page();
    if (!doc || page == nullptr) return;
    if (!page->layer_comps || page->layer_comps->comps.empty()) {
        flash(QStringLiteral("このページにはレイヤーカンプがありません（ウィンドウ → レイヤーカンプ で作ります）"), 5000, true);
        return;
    }
    std::optional<std::size_t> at;
    for (std::size_t i = 0; i < doc->pages.size(); ++i) {
        if (doc->pages[i]->id == page->id) at = i;
    }
    if (!at || doc->is_deferred(*at)) return;
    const std::optional<std::int64_t> one = layer_comps_panel_ != nullptr ? layer_comps_panel_->chosen() : page->layer_comps->last_applied;
    const std::optional<fs::path>& book = session_->path();
    const QString folder = book ? qpath(book->parent_path() / core::path_from_utf8(core::path_to_utf8(book->stem()) + "_書き出し")) : QDir::homePath() + QStringLiteral("/genko_書き出し");
    LayerCompsToFilesDialog dialog(this, folder, QString::fromStdString(doc->title), one.has_value());
    if (ask::exec(&dialog) != QDialog::Accepted) return;
    const fs::path where = fpath(QDir::fromNativeSeparators(dialog.folder->text().trimmed()));
    if (where.empty()) return;
    if (writes_over_a_source(where)) return;
    formats::CompFiles files;
    files.extension = dialog.format->currentData().toString().toStdString();
    files.prefix = dialog.prefix->text().trimmed().toStdString();
    if (dialog.chosen_only->isChecked() && one) files.comps = {*one};
    if (dialog.quality->isEnabled()) files.options.quality = dialog.quality->value();
    files.check = [guard = export_guard()](const fs::path& file) { formats::check_target(guard, file); };
    try {
        const std::size_t index = *at;
        const auto written = off_ui([doc, where, index, files] {
            fs::create_directories(where);
            return formats::export_layer_comps(*doc, index, where, files);
        });
        flash(QStringLiteral("レイヤーカンプを %1 枚書き出しました: %2").arg(written.size()).arg(QDir::toNativeSeparators(qpath(where))), 6000);
    } catch (const std::exception& error) {
        flash(QStringLiteral("書き出せませんでした: %1").arg(wording::error(QString::fromUtf8(error.what()))), 6000, true);
    }
}

}  // namespace genko::app
