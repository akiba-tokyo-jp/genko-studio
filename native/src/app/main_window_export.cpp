// PC4 F-c (SPEC.md FILE-01; 節 F 5.2・5.4): ファイル → 書き出し ▸ — PNG としてクイック書き出し・書き出し形式…・書き出しの環境設定…・
// Web 用に保存（従来）…・レイヤーからファイル… — and レイヤー's PNG としてクイック書き出し・書き出し形式… (the layer panel's menu too).
// The pictures are made and written on a worker (off_ui: the window goes on drawing); what a file will not keep is said
// in the dialog before it is written, or — クイック書き出し, which asks nothing — after.

#include <QDir>
#include <QFileInfo>
#include <QLineEdit>

#include <algorithm>
#include <cmath>

#include "app/ask.hpp"
#include "app/documents.hpp"
#include "app/export_as_dialog.hpp"
#include "app/main_window.hpp"
#include "app/off_ui.hpp"
#include "app/save_for_web_dialog.hpp"
#include "core/covers.hpp"
#include "core/paths.hpp"
#include "formats/copy.hpp"
#include "formats/export.hpp"
#include "formats/web_export.hpp"
#include "service/wording.hpp"

namespace genko::app {

namespace fs = std::filesystem;

namespace {

QString qpath(const fs::path& path) { return QString::fromStdString(core::path_to_utf8(path)); }
fs::path fpath(const QString& path) { return core::path_from_utf8(path.toStdString()); }

QString filter_of(const QString& ext) {
    if (ext == QLatin1String("jpg")) return QStringLiteral("JPEG (*.jpg *.jpeg)");
    if (ext == QLatin1String("tif")) return QStringLiteral("TIFF (*.tif *.tiff)");
    return QStringLiteral("%1 (*.%2)").arg(ext.toUpper(), ext);
}

QString notes_text(const std::vector<std::string>& notes) {
    QStringList said;
    for (const std::string& note : notes) said << QString::fromStdString(note);
    return said.join(QStringLiteral("。"));
}

}  // namespace

void MainWindow::build_export_actions() {
    const auto key = [](const char* text) { return QList<QKeySequence>{QKeySequence(QString::fromLatin1(text))}; };
    make(QStringLiteral("act_quick_export_png"), QStringLiteral("PNG としてクイック書き出し"), [this] { quick_export(); }, {},
         QStringLiteral("このページを、書き出しの環境設定の形式（既定は PNG）・書き出し先のとおり、たずねずに書き出します"));
    make(QStringLiteral("act_export_as"), QStringLiteral("書き出し形式…"), [this] { export_as(); }, key("Ctrl+Alt+Shift+W"),
         QStringLiteral("このページを、形式・大きさ・画質を選んで 1 枚の画像に書き出します"));
    make(QStringLiteral("act_export_prefs"), QStringLiteral("書き出しの環境設定…"), [this] { export_preferences_dialog(); }, {},
         QStringLiteral("クイック書き出しの形式・書き出し先・画質・メタデータ"));
    make(QStringLiteral("act_save_for_web"), QStringLiteral("Web 用に保存（従来）…"), [this] { save_for_web(); }, key("Ctrl+Alt+Shift+S"),
         QStringLiteral("色数や画質を見比べて、Web 用に小さく書き出します"));
    make(QStringLiteral("act_layers_to_files"), QStringLiteral("レイヤーからファイル…"), [this] { layers_to_files(); }, {},
         QStringLiteral("ページごとのフォルダーに、レイヤーを 1 枚ずつ書き出します（形式・接頭辞・表示中のものだけ）"));
    make(QStringLiteral("act_layer_quick_export_png"), QStringLiteral("PNG としてクイック書き出し"), [this] {
            if (const core::Layer* layer = target_layer()) quick_export(layer->id);
            else flash(QStringLiteral("書き出すレイヤーを選んでください"), 4000, true);
        },
         {}, QStringLiteral("選んだレイヤーだけを、中身の範囲で切って、書き出しの環境設定のとおり書き出します"));
    make(QStringLiteral("act_layer_export_as"), QStringLiteral("書き出し形式…"), [this] {
            if (const core::Layer* layer = target_layer()) export_as(layer->id);
            else flash(QStringLiteral("書き出すレイヤーを選んでください"), 4000, true);
        },
         {}, QStringLiteral("選んだレイヤーだけを、中身の範囲で切って、形式・大きさ・画質を選んで書き出します"));
}

std::optional<std::size_t> MainWindow::export_page(std::shared_ptr<const core::Document>& doc) {
    // (the page shown, by its id: the book's, or in コンテンツを編集's tab its contents' page; a page not read yet is said)
    doc = contents_ ? shown_book() : session_->snapshot();
    return shown_page_at(doc, current_page());
}

// Not over a picture a book here was opened from (the user's decision 4: its copy goes to another file).
formats::WriteGuard MainWindow::export_guard() const {
    std::vector<fs::path> sources;
    std::vector<fs::path> books;
    for (MainWindow* window : documents::windows()) {
        for (const Document& d : window->documents()) {
            if (d.source) sources.push_back(*d.source);
            if (d.session && d.session->path()) books.push_back(*d.session->path());
        }
    }
    formats::WriteGuard guard = formats::write_guard(book(), session_->path(), std::move(sources));
    for (fs::path& b : books) guard.books.push_back(std::move(b));
    return guard;
}

bool MainWindow::writes_over_a_source(const fs::path& path) {
    try {
        formats::check_target(export_guard(), path);
        return false;
    } catch (const core::Error& error) {
        ask::warning(this, QStringLiteral("書き出し"), wording::error(QString::fromUtf8(error.what())));
        return true;
    }
}

bool MainWindow::quick_export(const std::optional<std::string>& layer) {
    std::shared_ptr<const core::Document> doc;
    const std::optional<std::size_t> at = export_page(doc);
    if (!at) return false;
    const formats::ExportPreferences prefs = export_preferences();
    const core::Page& page = doc->page(*at);
    const core::Layer* chosen = nullptr;
    if (layer) {
        for (const core::Layer& l : page.layers) {
            if (l.id == *layer) chosen = &l;
        }
    }
    // (a place the preferences name for a saved book; else asked — a book never saved, or 毎回たずねる)
    std::optional<fs::path> place = formats::quick_export_place(prefs, contents_ ? std::nullopt : session_->path(), page, chosen);
    if (!place) {
        const QString ext = QString::fromStdString(prefs.format);
        std::string name = doc->title.empty() ? std::string("無題") : doc->title;
        if (chosen != nullptr) name += "_" + chosen->title;
        // (a name every OS takes — a / or a : in the title never a folder —, cut by characters as the exports' are)
        const QString stem = QString::fromStdString(formats::safe_name(name, "genko"));
        const QString path = ask::save_path(this, QStringLiteral("クイック書き出し"), stem + QStringLiteral(".") + ext, filter_of(ext));
        if (path.isEmpty()) return false;
        place = fpath(QFileInfo(path).suffix().isEmpty() ? path + QStringLiteral(".") + ext : path);
    }
    if (writes_over_a_source(*place)) return false;
    try {
        const fs::path dest = *place;
        const formats::CopyResult written = off_ui([doc, page = *at, dest, prefs, layer] { return formats::quick_export(*doc, page, dest, prefs, layer); });
        QString words = QStringLiteral("書き出しました（%1）: %2").arg(human_bytes(written.bytes), QDir::toNativeSeparators(qpath(dest)));
        if (!written.notes.empty()) words += QStringLiteral("。") + notes_text(written.notes);
        flash(words, 6000);
        return true;
    } catch (const std::exception& error) {
        flash(QStringLiteral("書き出せませんでした: %1").arg(wording::error(QString::fromUtf8(error.what()))), 6000, true);
        return false;
    }
}

void MainWindow::export_as(const std::optional<std::string>& layer) {
    std::shared_ptr<const core::Document> doc;
    const std::optional<std::size_t> at = export_page(doc);
    if (!at) return;
    ExportAsDialog dialog(this, doc, *at, layer);
    if (ask::exec(&dialog) != QDialog::Accepted) return;
    const formats::CopyOptions options = dialog.options();
    const QString ext = dialog.extension();
    try {
        if (dialog.all_pages()) {
            const QString folder = ask::existing_dir(this, QStringLiteral("書き出し先のフォルダー"));
            if (folder.isEmpty()) return;
            if (!doc->deferred.empty()) {  // (never some pages written and the rest not: every one read first)
                flash(wording::error(QStringLiteral("the book's pages are still being read: try again in a moment")), 6000, true);
                return;
            }
            std::vector<std::size_t> pages;
            for (std::size_t i = 0; i < doc->pages.size(); ++i) pages.push_back(i);
            const fs::path where = fpath(folder);
            if (writes_over_a_source(where)) return;
            int there = 0;
            for (const std::size_t i : pages) {
                const fs::path file = where / core::path_from_utf8(core::file_stem(doc->page(i)) + "." + ext.toStdString());
                if (writes_over_a_source(file)) return;
                std::error_code ec;
                if (fs::exists(file, ec)) ++there;
            }
            // (the coordinator's P3-8: files of those names there — asked once, before anything is written)
            if (there > 0 && !ask::question(this, QStringLiteral("書き出し形式"),
                                            QStringLiteral("書き出し先に同じ名前のファイルが %1 個あります。置き換えますか？\n%2")
                                                .arg(there)
                                                .arg(QDir::toNativeSeparators(folder)))) {
                return;
            }
            const std::vector<formats::CopyResult> written =
                off_ui([doc, pages, where, ext, options] { return formats::copy_pages(*doc, pages, where, ext.toStdString(), options); });
            std::vector<std::string> notes;
            for (const formats::CopyResult& r : written) {
                for (const std::string& n : r.notes) {
                    if (std::find(notes.begin(), notes.end(), n) == notes.end()) notes.push_back(n);
                }
            }
            QString words = QStringLiteral("%1 ページを書き出しました: %2").arg(written.size()).arg(QDir::toNativeSeparators(folder));
            if (!notes.empty()) words += QStringLiteral("。") + notes_text(notes);
            flash(words, 6000);
            return;
        }
        std::string name = doc->title.empty() ? std::string("無題") : doc->title;
        if (layer) {
            for (const core::Layer& l : doc->page(*at).layers) {
                if (l.id == *layer) name += "_" + l.title;
            }
        }
        const QString stem = QString::fromStdString(formats::safe_name(name, "genko"));
        const QString path = ask::save_path(this, QStringLiteral("書き出し形式"), stem + QStringLiteral(".") + ext, filter_of(ext));
        if (path.isEmpty()) return;
        const fs::path dest = fpath(QFileInfo(path).suffix().isEmpty() ? path + QStringLiteral(".") + ext : path);
        if (writes_over_a_source(dest)) return;
        const formats::CopyResult written = off_ui([doc, page = *at, dest, options] { return formats::write_copy(*doc, page, dest, options); });
        QString words = QStringLiteral("書き出しました（%1・%2 × %3 px）: %4")
                            .arg(human_bytes(written.bytes))
                            .arg(written.width)
                            .arg(written.height)
                            .arg(QDir::toNativeSeparators(qpath(dest)));
        if (!written.notes.empty()) words += QStringLiteral("。") + notes_text(written.notes);
        flash(words, 6000);
    } catch (const std::exception& error) {
        flash(QStringLiteral("書き出せませんでした: %1").arg(wording::error(QString::fromUtf8(error.what()))), 6000, true);
    }
}

void MainWindow::export_preferences_dialog() {
    ExportPreferencesDialog dialog(this);
    if (ask::exec(&dialog) == QDialog::Accepted) flash(QStringLiteral("書き出しの環境設定を変えました"), 3000);
}

void MainWindow::save_for_web() {
    std::shared_ptr<const core::Document> doc;
    const std::optional<std::size_t> at = export_page(doc);
    if (!at) return;
    SaveForWebDialog dialog(this, doc, *at);
    if (ask::exec(&dialog) != QDialog::Accepted) return;
    try {
        const formats::WebSettings settings = dialog.settings();
        const formats::WebSize size = dialog.size();
        const QString ext = dialog.extension();
        const QString stem = QString::fromStdString(formats::safe_name(doc->title.empty() ? std::string("無題") : doc->title, "genko"));
        const QString path = ask::save_path(this, QStringLiteral("Web 用に保存"), stem + QStringLiteral(".") + ext, filter_of(ext));
        if (path.isEmpty()) return;
        const fs::path dest = fpath(QFileInfo(path).suffix().isEmpty() ? path + QStringLiteral(".") + ext : path);
        if (writes_over_a_source(dest)) return;
        const formats::WebResult written =
            off_ui([doc, page = *at, settings, size, dest] { return formats::save_for_web(*doc, page, settings, size, dest); });
        QString words = QStringLiteral("Web 用に保存しました（%1・%2 × %3 px）: %4")
                            .arg(human_bytes(written.bytes))
                            .arg(written.width)
                            .arg(written.height)
                            .arg(QDir::toNativeSeparators(qpath(dest)));
        if (!written.notes.empty()) words += QStringLiteral("。") + notes_text(written.notes);
        flash(words, 6000);
    } catch (const std::exception& error) {
        flash(QStringLiteral("Web 用に保存できませんでした: %1").arg(wording::error(QString::fromUtf8(error.what()))), 6000, true);
    }
}

void MainWindow::layers_to_files() {
    const std::shared_ptr<const core::Document> doc = contents_ ? shown_book() : session_->snapshot();
    if (!doc || doc->pages.empty()) return;
    if (!doc->deferred.empty()) {
        flash(wording::error(QStringLiteral("the book's pages are still being read: try again in a moment")), 6000, true);
        return;
    }
    const std::optional<fs::path>& book = session_->path();
    const QString folder = book ? qpath(book->parent_path() / core::path_from_utf8(core::path_to_utf8(book->stem()) + "_書き出し")) : QDir::homePath() + QStringLiteral("/genko_書き出し");
    LayersToFilesDialog dialog(this, folder, QString::fromStdString(doc->title));
    if (ask::exec(&dialog) != QDialog::Accepted) return;
    const fs::path where = fpath(QDir::fromNativeSeparators(dialog.folder->text().trimmed()));
    if (where.empty()) return;
    if (writes_over_a_source(where)) return;
    formats::LayerFiles files = dialog.files();
    files.check = [guard = export_guard()](const fs::path& file) { formats::check_target(guard, file); };
    try {
        const int dpi = std::max(1, static_cast<int>(std::lround(doc->spec.dpi.value())));
        const auto [written, notes] = off_ui([doc, where, dpi, files] {
            std::vector<std::string> said;
            std::vector<fs::path> paths = formats::export_layers(*doc, where, dpi, "paper", files, &said);
            return std::make_pair(std::move(paths), std::move(said));
        });
        QString words = QStringLiteral("レイヤーを %1 枚書き出しました: %2").arg(written.size()).arg(QDir::toNativeSeparators(qpath(where)));
        if (!notes.empty()) words += QStringLiteral("。") + notes_text(notes);
        flash(words, 6000);
    } catch (const std::exception& error) {
        flash(QStringLiteral("書き出せませんでした: %1").arg(wording::error(QString::fromUtf8(error.what()))), 6000, true);
    }
}

}  // namespace genko::app
