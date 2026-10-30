// PC5 I-c (artboards@1, SPEC.md AUTO-01「アートボード」; the user's decision 3: アートボード＝ページ): レイヤー → 新規 → アートボード…・
// グループからのアートボード…・レイヤーからのアートボード…, イメージ（ページ）→ アートボードの設定…, 表示 → アートボードを画面サイズに
// 合わせる・選択したアートボードのガイドを消去 (and 表示・非表示 ▸ アートボードガイド・アートボード名: main_window_guide_view.cpp),
// ファイル → 書き出し → アートボードからファイル…・アートボードから PDF…. Each change of the book an op through apply_ops
// (render/ops_artboards); the files written by formats/artboards off the window's thread, never over the book or a file it
// links (export_guard).

#include <QAction>
#include <QDir>
#include <QMenu>

#include <algorithm>
#include <filesystem>

#include "app/artboard_dialog.hpp"
#include "app/ask.hpp"
#include "app/canvas.hpp"
#include "app/layer_panel.hpp"
#include "app/main_window.hpp"
#include "app/off_ui.hpp"
#include "core/artboards.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "formats/artboards.hpp"
#include "render/page.hpp"
#include "service/wording.hpp"

namespace genko::app {

using core::Json;
namespace fs = std::filesystem;

namespace {

QString qpath(const fs::path& path) { return QString::fromStdString(core::path_to_utf8(path)); }
fs::path fpath(const QString& path) { return core::path_from_utf8(path.toStdString()); }

void put_before(QMenu* menu, QAction* anchor, QAction* command) { menu->insertAction(menu->actions().contains(anchor) ? anchor : nullptr, command); }

int pixels(double mm, double dpi) { return render::mm_to_px(mm, static_cast<int>(std::llround(dpi))); }

}  // namespace

void MainWindow::build_artboard_actions() {
    make(QStringLiteral("act_layer_artboard"), QStringLiteral("アートボード…"), [this] { new_artboard(); }, {},
         QStringLiteral("アートボード（大きさを pixel で決めたページ）を、このページの後ろに足します"));
    make(QStringLiteral("act_layer_artboard_from_group"), QStringLiteral("グループからのアートボード…"), [this] { artboard_from(true); }, {},
         QStringLiteral("選んだグループの描かれた範囲を大きさにしたアートボードを作り、グループをそこへ移します"));
    make(QStringLiteral("act_layer_artboard_from_layers"), QStringLiteral("レイヤーからのアートボード…"), [this] { artboard_from(false); }, {},
         QStringLiteral("選んだレイヤーの描かれた範囲を大きさにしたアートボードを作り、レイヤーをそこへ移します"));
    make(QStringLiteral("act_artboard_settings"), QStringLiteral("アートボードの設定…"), [this] { artboard_settings(); }, {},
         QStringLiteral("このアートボードの名前・大きさ・背景色を変えます"));
    make(QStringLiteral("act_fit_artboard"), QStringLiteral("アートボードを画面サイズに合わせる"), [this] { fit_artboard(); }, {},
         QStringLiteral("このアートボード（ページ）だけを画面いっぱいに表示します"));
    make(QStringLiteral("act_clear_artboard_guides"), QStringLiteral("選択したアートボードのガイドを消去"), [this] { clear_artboard_guides(); }, {},
         QStringLiteral("このアートボードのガイドを消します（描画定規はそのまま）"));
    make(QStringLiteral("act_artboards_to_files"), QStringLiteral("アートボードからファイル…"), [this] { artboards_to_files(false); }, {},
         QStringLiteral("アートボードを 1 つずつファイルに書き出します（1 つの PSD にまとめることもできます）"));
    make(QStringLiteral("act_artboards_to_pdf"), QStringLiteral("アートボードから PDF…"), [this] { artboards_to_files(true); }, {},
         QStringLiteral("アートボードを 1 ページずつ JPEG にした PDF に書き出します"));
}

void MainWindow::build_artboard_menus(QMenu* layers, QMenu* pages) {
    // レイヤー → 新規 ▸: アートボード…・グループからのアートボード…・レイヤーからのアートボード… before レイヤーからのフレーム… (Photoshop's
    // order, after I-a's レイヤーからのグループ…)
    QMenu* made = nullptr;
    for (QAction* a : layers->actions())
        if (a->menu() != nullptr && a->text() == QStringLiteral("新規")) made = a->menu();
    if (made != nullptr) {
        for (const char* name : {"act_layer_artboard", "act_layer_artboard_from_group", "act_layer_artboard_from_layers"})
            put_before(made, action(QStringLiteral("act_layer_frame")), action(QString::fromLatin1(name)));
    }
    // イメージ（ページ）: アートボードの設定… after カンバスサイズ…
    if (pages != nullptr) {
        const QList<QAction*> items = pages->actions();
        const auto canvas = std::find(items.begin(), items.end(), action(QStringLiteral("act_canvas_size")));
        if (canvas != items.end() && std::next(canvas) != items.end()) pages->insertAction(*std::next(canvas), action(QStringLiteral("act_artboard_settings")));
        else pages->addAction(action(QStringLiteral("act_artboard_settings")));
    }
    // ファイル → 書き出し ▸: アートボードからファイル…・アートボードから PDF… first of the files a part each (Photoshop's order: before
    // レイヤーからファイル…)
    if (export_menu_ != nullptr) {
        for (const char* name : {"act_artboards_to_files", "act_artboards_to_pdf"})
            put_before(export_menu_, action(QStringLiteral("act_layers_to_files")), action(QString::fromLatin1(name)));
    }
}

void MainWindow::build_artboard_view_menus(QMenu* view) {
    // (view_rows places them: アートボードを画面サイズに合わせる after 画面サイズに合わせる, 選択したアートボードのガイドを消去 after
    // ガイドを消去; アートボードガイド and アートボード名 are 表示・非表示 ▸'s, main_window_guide_view.cpp)
    for (const char* name : {"act_fit_artboard", "act_clear_artboard_guides"}) view->addAction(action(QString::fromLatin1(name)));
}

void MainWindow::new_artboard() {
    const core::Document& doc = book();
    const double dpi = doc.spec.dpi.value();
    ArtboardDialog dialog(this, std::nullopt, QString::fromStdString(core::next_artboard_name(doc)), pixels(doc.spec.width_mm.value(), dpi),
                          pixels(doc.spec.height_mm.value(), dpi), dpi);
    const auto asked = asking();
    if (ask::exec(&dialog) != QDialog::Accepted || !still(asked)) return;
    Json op = dialog.settings();
    op["op"] = "add_artboard";
    const core::Page* page = current_page();
    if (page != nullptr && !core::is_cover(*page)) op["after"] = core::py_int(page->index.json());
    std::vector<std::string> before;
    for (const auto& p : book().pages) before.push_back(p->id);
    if (apply_ops(Json::array({std::move(op)}))) {
        // (the new page: after this one, or the last before the covers)
        for (int i = 0; i < static_cast<int>(book().pages.size()); ++i) {
            if (std::find(before.begin(), before.end(), book().pages[static_cast<std::size_t>(i)]->id) == before.end()) page_index_ = i;
        }
        reload_pages();
    }
}

void MainWindow::artboard_settings() {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    if (!page->artboard) {
        flash(QStringLiteral("このページはアートボードではありません（レイヤー → 新規 → アートボード… で作ります）"), 5000, true);
        return;
    }
    const double dpi = page->spec.dpi.value();
    ArtboardDialog dialog(this, page->artboard, {}, pixels(page->spec.width_mm.value(), dpi), pixels(page->spec.height_mm.value(), dpi), dpi);
    const auto asked = asking();
    if (ask::exec(&dialog) != QDialog::Accepted || !still(asked)) return;
    page = current_page();
    if (page == nullptr) return;
    Json op = dialog.settings();
    op["op"] = "set_artboard";
    op["page"] = page->index.json();
    apply_ops(Json::array({std::move(op)}));
}

void MainWindow::artboard_from(bool group) {
    const core::Page* page = current_page();
    const core::Layer* layer = target_layer();
    if (page == nullptr || layer == nullptr) return;
    std::vector<std::string> ids = layer_panel_ != nullptr ? layer_panel_->selected_ids() : std::vector<std::string>{};
    if (ids.empty()) ids = {layer->id};
    if (group) {
        const core::Layer* chosen = nullptr;
        for (const core::Layer& l : page->layers)
            if (ids.size() == 1 && l.id == ids.front()) chosen = &l;
        if (chosen == nullptr || chosen->kind != core::LayerKind::Folder) {
            flash(QStringLiteral("グループを 1 つ選んでください"), 4000, true);
            return;
        }
    }
    const QString fallback = group ? QString::fromStdString(layer->title) : QString::fromStdString(core::next_artboard_name(book()));
    const auto asked = asking();
    const auto name = ask::get_text(this, group ? QStringLiteral("グループからのアートボード") : QStringLiteral("レイヤーからのアートボード"),
                                    QStringLiteral("名前:"), fallback);
    if (!name || !still(asked)) return;
    const QString given = name->trimmed().isEmpty() ? fallback : name->trimmed();
    const int at = static_cast<int>(core::py_int(page->index.json()));
    if (apply_ops(Json::array({Json{{"op", "artboard_from_layers"}, {"page", page->index.json()}, {"ids", ids}, {"name", given.toStdString()}}}))) {
        page_index_ = at;  // (the new page, right after this one)
        reload_pages();
    }
}

void MainWindow::fit_artboard() {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    if (!page->artboard) {
        flash(QStringLiteral("このページはアートボードではありません"), 3000);
        return;
    }
    const double w = page->spec.width_mm.value(), h = page->spec.height_mm.value();
    PageCanvas* c = canvas_;
    c->glide([c, w, h] { c->fit_box(QRectF(0, 0, w, h)); });
}

void MainWindow::clear_artboard_guides() {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    if (!page->artboard) {
        flash(QStringLiteral("このページはアートボードではありません（カンバスガイドを消去 で消せます）"), 4000);
        return;
    }
    const bool any = page->rulers.is_array() && std::any_of(page->rulers.begin(), page->rulers.end(), [](const Json& r) {
                         return r.is_object() && r.value("kind", Json()) == Json("guide");
                     });
    if (!any) {
        flash(QStringLiteral("このアートボードにガイドはありません"), 3000);
        return;
    }
    if (apply_ops(Json::array({Json{{"op", "delete_ruler"}, {"page", page->index.json()}, {"kind", "guide"}}})))
        flash(QStringLiteral("このアートボードのガイドを消しました"), 3000);
}

void MainWindow::artboards_to_files(bool pdf) {
    const std::shared_ptr<const core::Document> doc = contents_ ? shown_book() : session_->snapshot();
    if (!doc) return;
    std::vector<std::string> names;
    for (const auto& page : doc->pages)
        if (page->artboard) names.push_back(page->artboard->name);
    if (names.empty()) {
        flash(QStringLiteral("この原稿にはアートボードがありません（レイヤー → 新規 → アートボード… で作ります）"), 5000, true);
        return;
    }
    const std::optional<fs::path>& book_path = session_->path();
    const std::string stem = book_path ? core::path_to_utf8(book_path->stem()) : std::string("genko");
    const fs::path folder = book_path ? book_path->parent_path() / core::path_from_utf8(stem + "_書き出し") : fpath(QDir::homePath() + QStringLiteral("/genko_書き出し"));
    const QString start = pdf ? qpath(folder.parent_path() / core::path_from_utf8(stem + "_アートボード.pdf")) : qpath(folder);
    ArtboardExportDialog dialog(this, pdf, start, QString::fromStdString(doc->title), names);
    if (ask::exec(&dialog) != QDialog::Accepted) return;
    const fs::path where = fpath(QDir::fromNativeSeparators(dialog.where->text().trimmed()));
    if (where.empty()) return;
    if (writes_over_a_source(where)) return;
    const std::vector<std::string> chosen = dialog.chosen();
    if (chosen.empty()) {
        flash(QStringLiteral("書き出すアートボードを選んでください"), 4000, true);
        return;
    }
    const auto check = [guard = export_guard()](const fs::path& file) { formats::check_target(guard, file); };
    try {
        if (pdf) {
            formats::ArtboardPdf options;
            options.artboards = chosen;
            options.quality = dialog.quality->value();
            options.check = check;
            const auto made = off_ui([doc, where, options] { return formats::export_artboards_pdf(*doc, where, options); });
            flash(QStringLiteral("アートボード %1 枚を PDF に書き出しました: %2").arg(made.pages).arg(QDir::toNativeSeparators(qpath(where))), 6000);
            return;
        }
        const std::string extension = dialog.format->currentData().toString().toStdString();
        if (dialog.one_psd->isChecked()) {
            formats::ArtboardPsd options;
            options.artboards = chosen;
            options.psb = extension == "psb";
            options.check = check;
            fs::path file = where;
            if (fs::is_directory(where) || !where.has_extension()) file = where / core::path_from_utf8(stem + "." + extension);
            const auto made = off_ui([doc, file, options] { return formats::export_artboards_psd(*doc, file, options); });
            QString said = QStringLiteral("アートボード %1 枚を 1 つの %2 に書き出しました: %3")
                               .arg(chosen.size())
                               .arg(QString::fromStdString(extension).toUpper(), QDir::toNativeSeparators(qpath(made.file)));
            if (!made.notes.empty()) said += QStringLiteral("（%1）").arg(QString::fromStdString(made.notes.front()));
            flash(said, 8000);
            return;
        }
        formats::ArtboardFiles files;
        files.extension = extension;
        files.prefix = dialog.prefix->text().trimmed().toStdString();
        files.artboards = chosen;
        if (dialog.quality->isEnabled()) files.options.quality = dialog.quality->value();
        files.check = check;
        const auto written = off_ui([doc, where, files] {
            fs::create_directories(where);
            return formats::export_artboards(*doc, where, files);
        });
        flash(QStringLiteral("アートボードを %1 枚書き出しました: %2").arg(written.size()).arg(QDir::toNativeSeparators(qpath(where))), 6000);
    } catch (const std::exception& error) {
        flash(QStringLiteral("書き出せませんでした: %1").arg(wording::error(QString::fromUtf8(error.what()))), 6000, true);
    }
}

}  // namespace genko::app
