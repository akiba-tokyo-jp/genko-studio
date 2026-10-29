// PC5 I-d (variables@1, SPEC.md AUTO-01「変数とデータセット」): ページ (Photoshop's イメージ) → 変数 ▸ 定義…・データセット…,
// データセットを適用…, ファイル → 書き出し → データセットからファイル… and ファイル → 読み込み → 変数データセット… — each change of
// the book an op through apply_ops (render/ops_variables), the files read and written by formats/datasets off the window's
// thread (app/variables_dialog: the windows).

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QLineEdit>
#include <QMenu>
#include <QSpinBox>

#include <algorithm>

#include "app/ask.hpp"
#include "app/main_window.hpp"
#include "app/off_ui.hpp"
#include "app/session.hpp"
#include "app/variables_dialog.hpp"
#include "core/error.hpp"
#include "core/paths.hpp"
#include "core/variables.hpp"
#include "formats/copy.hpp"
#include "formats/datasets.hpp"
#include "service/wording.hpp"

namespace genko::app {

using core::Json;
namespace fs = std::filesystem;

namespace {

QString qpath(const fs::path& path) { return QString::fromStdString(core::path_to_utf8(path)); }
fs::path fpath(const QString& path) { return core::path_from_utf8(path.toStdString()); }

}  // namespace

void MainWindow::build_variable_actions() {
    make(QStringLiteral("act_variables_define"), QStringLiteral("定義…"), [this] { define_variables(); }, {},
         QStringLiteral("レイヤー・台詞に変数（表示・テキストの置き換え・ピクセルの置き換え）を定義します"));
    make(QStringLiteral("act_variables_data_sets"), QStringLiteral("データセット…"), [this] { edit_data_sets(); }, {},
         QStringLiteral("変数の値の組（データセット）を作り、直します"));
    make(QStringLiteral("act_apply_data_set"), QStringLiteral("データセットを適用…"), [this] { apply_data_set_dialog(); }, {},
         QStringLiteral("データセットの値で、レイヤーの表示・文字・画像を置き換えます（取り消せます）"));
    make(QStringLiteral("act_data_sets_to_files"), QStringLiteral("データセットからファイル…"), [this] { data_sets_to_files(); }, {},
         QStringLiteral("データセットを 1 つずつ適用した画像を、1 枚ずつ書き出します（原稿はそのまま）"));
    make(QStringLiteral("act_import_data_sets"), QStringLiteral("変数データセット…"), [this] { import_data_sets(); }, {},
         QStringLiteral("テキストファイル（CSV）からデータセットを読み込みます"));
}

void MainWindow::build_variable_menus(QMenu* file, QMenu* pages) {
    // ページ (Photoshop's イメージ) → 変数 ▸ and データセットを適用…: after 複製…, before 解析 ▸ (Photoshop's order)
    auto* variables = new QMenu(QStringLiteral("変数"), pages);
    variables->setObjectName(QStringLiteral("menu_variables"));
    for (const char* name : {"act_variables_define", "act_variables_data_sets"}) variables->addAction(action(QString::fromLatin1(name)));
    QAction* before = nullptr;
    const QList<QAction*> in_pages = pages->actions();
    for (QAction* item : in_pages) {
        if (item->menu() != nullptr && item->menu()->objectName() == QLatin1String("menu_analysis")) before = item;
    }
    if (before == nullptr) {
        const auto duplicate = std::find(in_pages.begin(), in_pages.end(), action(QStringLiteral("act_duplicate_image")));
        if (duplicate != in_pages.end() && std::next(duplicate) != in_pages.end()) before = *std::next(duplicate);
    }
    if (before != nullptr) {
        pages->insertMenu(before, variables);
        pages->insertAction(before, action(QStringLiteral("act_apply_data_set")));
    } else {
        pages->addMenu(variables);
        pages->addAction(action(QStringLiteral("act_apply_data_set")));
    }
    // ファイル → 書き出し → データセットからファイル…: after カラールックアップテーブル… (Photoshop's order)
    if (export_menu_ != nullptr) export_menu_->addAction(action(QStringLiteral("act_data_sets_to_files")));
    // ファイル → 読み込み ▸ 変数データセット… (Photoshop's first of 読み込み): the submenu found ("menu_import", after PSD をレイヤーの
    // まま読み込む…), or made there when it is not yet
    QMenu* imports = nullptr;
    for (QAction* item : file->actions()) {
        if (item->menu() != nullptr && item->menu()->objectName() == QLatin1String("menu_import")) imports = item->menu();
    }
    if (imports == nullptr) {
        imports = new QMenu(QStringLiteral("読み込み"), file);
        imports->setObjectName(QStringLiteral("menu_import"));
        const QList<QAction*> items = file->actions();
        const auto psd = std::find(items.begin(), items.end(), action(QStringLiteral("act_import_psd")));
        if (psd != items.end() && std::next(psd) != items.end()) {
            file->insertMenu(*std::next(psd), imports);
        } else {
            file->addMenu(imports);
        }
    }
    const QList<QAction*> imported = imports->actions();
    if (imported.isEmpty()) {
        imports->addAction(action(QStringLiteral("act_import_data_sets")));
    } else {
        imports->insertAction(imported.front(), action(QStringLiteral("act_import_data_sets")));
    }
}

bool MainWindow::variable_op(Json op) { return apply_ops(Json::array({std::move(op)})); }

void MainWindow::define_variables() {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    VariablesDialog dialog(this, book(), *page);
    if (ask::exec(&dialog) != QDialog::Accepted) return;
    variable_op(Json{{"op", "set_variables"}, {"defs", dialog.defs()}});
}

void MainWindow::edit_data_sets() {
    if (current_page() == nullptr) return;
    if (core::variables_of(book()).defs.empty()) {
        flash(QStringLiteral("原稿に変数がありません（ページ → 変数 → 定義… で定義します）"), 5000, true);
        return;
    }
    DataSetsDialog dialog(this, book());
    if (ask::exec(&dialog) != QDialog::Accepted) return;
    variable_op(Json{{"op", "set_data_sets"}, {"data_sets", dialog.data_sets()}});
}

void MainWindow::apply_data_set_dialog() {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    const std::shared_ptr<const core::Document> doc = session_->snapshot();
    if (!doc || core::variables_of(*doc).data_sets.empty()) {
        flash(QStringLiteral("原稿にデータセットがありません（ページ → 変数 → データセット… で作ります）"), 5000, true);
        return;
    }
    std::size_t at = 0;
    for (std::size_t i = 0; i < doc->pages.size(); ++i) {
        if (doc->pages[i]->id == page->id) at = i;
    }
    ApplyDataSetDialog dialog(this, doc, at, session_->actor());
    if (ask::exec(&dialog) != QDialog::Accepted || dialog.chosen() < 0) return;
    if (variable_op(Json{{"op", "apply_data_set"}, {"index", dialog.chosen()}})) {
        flash(QStringLiteral("データセット「%1」を適用しました").arg(QString::fromStdString(core::variables_of(book()).data_sets.at(std::size_t(dialog.chosen())).name)));
    }
}

void MainWindow::data_sets_to_files() {
    const std::shared_ptr<const core::Document> doc = session_->snapshot();
    if (!doc || core::variables_of(*doc).data_sets.empty()) {
        flash(QStringLiteral("原稿にデータセットがありません（ページ → 変数 → データセット… で作ります）"), 5000, true);
        return;
    }
    const std::optional<fs::path>& path = session_->path();
    const QString folder = path ? qpath(path->parent_path() / core::path_from_utf8(core::path_to_utf8(path->stem()) + "_書き出し")) : QDir::homePath() + QStringLiteral("/genko_書き出し");
    DataSetsToFilesDialog dialog(this, folder, *doc);
    if (ask::exec(&dialog) != QDialog::Accepted) return;
    const fs::path where = fpath(QDir::fromNativeSeparators(dialog.folder->text().trimmed()));
    if (where.empty()) return;
    if (writes_over_a_source(where)) return;
    formats::DataSetFiles files;
    files.extension = dialog.format->currentData().toString().toStdString();
    files.naming = dialog.naming->text().trimmed().isEmpty() ? std::string("{name}") : dialog.naming->text().trimmed().toStdString();
    if (dialog.which->currentIndex() > 0) files.data_sets = {dialog.which->currentData().toString().toStdString()};
    if (dialog.quality->isEnabled()) files.options.quality = dialog.quality->value();
    files.actor = core::Actor(session_->actor());
    files.check = [guard = export_guard()](const fs::path& file) { formats::check_target(guard, file); };
    try {
        const auto written = off_ui([doc, where, files] {
            fs::create_directories(where);
            return formats::export_data_sets(*doc, where, files);
        });
        flash(QStringLiteral("データセットから %1 枚書き出しました: %2").arg(written.size()).arg(QDir::toNativeSeparators(qpath(where))), 6000);
    } catch (const std::exception& error) {
        flash(QStringLiteral("書き出せませんでした: %1").arg(wording::error(QString::fromUtf8(error.what()))), 6000, true);
    }
}

void MainWindow::import_data_sets() {
    if (current_page() == nullptr) return;
    const core::Variables variables = core::variables_of(book());
    if (variables.defs.empty()) {
        flash(QStringLiteral("原稿に変数がありません（ページ → 変数 → 定義… で定義してから読み込みます）"), 5000, true);
        return;
    }
    ImportDataSetsDialog dialog(this);
    if (ask::exec(&dialog) != QDialog::Accepted) return;
    const fs::path file = fpath(QDir::fromNativeSeparators(dialog.file->text().trimmed()));
    if (file.empty()) return;
    formats::CsvOptions options;
    options.delimiter = static_cast<char>(dialog.delimiter->currentData().toInt());
    options.folder = file.parent_path();
    formats::CsvRead read;
    try {
        read = off_ui([file, variables, options] {
            QFile in(qpath(file));
            if (!in.open(QIODevice::ReadOnly)) throw core::Error("value", "the text file cannot be read");
            if (in.size() > (64ll << 20)) throw core::Error("value", "the text file is larger than 64 MiB");
            const QByteArray bytes = in.readAll();
            return formats::read_data_sets_csv(std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())), variables, options);
        });
    } catch (const std::exception& error) {
        flash(QStringLiteral("読み込めませんでした: %1").arg(wording::error(QString::fromUtf8(error.what()))), 6000, true);
        return;
    }
    if (!variable_op(Json{{"op", "set_data_sets"}, {"data_sets", read.data_sets}, {"append", !dialog.replace->isChecked()}})) return;
    QString said = QStringLiteral("データセットを %1 個読み込みました").arg(read.data_sets.size());
    for (const std::string& note : read.notes) said += QStringLiteral("（%1）").arg(QString::fromStdString(note));
    flash(said, 6000);
}

}  // namespace genko::app
