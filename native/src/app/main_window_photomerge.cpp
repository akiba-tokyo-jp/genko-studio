// 編集 → レイヤーを自動整列…・レイヤーを自動合成…, ファイル → 自動処理 → Photomerge…, スクリプト → ファイルをスタックとして読み込み…・
// 統計… (PC5 I-g, SPEC.md AUTO-01): the windows of app/auto_align_dialog.hpp and app/photomerge_dialog.hpp, the ops sent on a
// worker (send_filter: 中止 leaves the book as it was, one Undo) and the new books made on a worker with 中止.

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFutureWatcher>
#include <QMenu>
#include <QPointer>
#include <QProgressDialog>
#include <QTimer>
#include <QtConcurrent>

#include <atomic>
#include <memory>
#include <stop_token>

#include "app/ask.hpp"
#include "app/auto_align_dialog.hpp"
#include "app/layer_panel.hpp"
#include "app/main_window.hpp"
#include "app/open_dialog.hpp"
#include "app/photomerge_dialog.hpp"
#include "app/session.hpp"
#include "app/stoppable.hpp"
#include "core/actor.hpp"
#include "core/paths.hpp"
#include "render/fx.hpp"
#include "render/ops_align.hpp"
#include "render/page.hpp"
#include "render/segment/segment.hpp"
#include "service/automate.hpp"
#include "service/wording.hpp"

namespace genko::app {

using core::Json;
namespace fs = std::filesystem;
namespace automate = service::automate;

void MainWindow::build_photomerge_actions() {
    make(QStringLiteral("act_auto_align_layers"), QStringLiteral("レイヤーを自動整列…"), [this] { auto_align_dialog(); }, {},
         QStringLiteral("選んだ 2 枚以上の画素のレイヤーの、同じ所を見つけて重ねます（遠近法・円筒法・球面法・コラージュ・位置の変更）"));
    make(QStringLiteral("act_auto_blend_layers"), QStringLiteral("レイヤーを自動合成…"), [this] { auto_blend_dialog(); }, {},
         QStringLiteral("選んだ 2 枚以上の画素のレイヤーにマスクを付けて、継ぎ目の見えない 1 枚に見せます（パノラマ・画像をスタック）"));
    make(QStringLiteral("act_photomerge"), QStringLiteral("Photomerge…"), [this] { photomerge_dialog(); }, {},
         QStringLiteral("重なりのある写真をつないで、パノラマの新しい画像を作ります（元のファイルは変えません）"));
    make(QStringLiteral("act_load_files_into_stack"), QStringLiteral("ファイルをスタックとして読み込み…"), [this] { load_stack_dialog(); }, {},
         QStringLiteral("画像を 1 枚 1 レイヤーにして、新しい画像に重ねて読み込みます"));
    make(QStringLiteral("act_statistics"), QStringLiteral("統計…"), [this] { statistics_dialog(); }, {},
         QStringLiteral("画像を重ねて、画素ごとの中央値・平均などで 1 枚にします（中央値で通りがかりの人を消すなど）"));
}

void MainWindow::build_photomerge_menus() {
    // (自動処理 ▸: Photomerge… last, right after HDR Pro に統合… — one group, Photoshop's order: the review's P3-8)
    automate_menu_->addAction(action(QStringLiteral("act_photomerge")));
    automate_menu_->menuAction()->setVisible(true);
    // (スクリプト ▸: ファイルをスタックとして読み込み…・統計… after スクリプトイベントマネージャー…, before 参照…)
    QAction* browse = action(QStringLiteral("act_script_browse"));
    const QList<QAction*> scripts = scripts_menu_->actions();
    const qsizetype at = scripts.indexOf(browse);
    QAction* before = at > 0 && scripts[at - 1]->isSeparator() ? scripts[at - 1] : browse;
    QAction* separator = new QAction(scripts_menu_);
    separator->setSeparator(true);
    scripts_menu_->insertAction(before, separator);
    scripts_menu_->insertAction(before, action(QStringLiteral("act_load_files_into_stack")));
    scripts_menu_->insertAction(before, action(QStringLiteral("act_statistics")));
}

void MainWindow::build_auto_align_items(QMenu* edit) {
    // (after 変形 ▸, Photoshop's: on with two or more paint layers chosen)
    for (const char* name : {"act_auto_align_layers", "act_auto_blend_layers"}) edit->addAction(action(QString::fromLatin1(name)));
    connect(edit, &QMenu::aboutToShow, this, [this] {
        const bool two = chosen_paint_layers().size() >= 2;
        for (const char* name : {"act_auto_align_layers", "act_auto_blend_layers"}) action(QString::fromLatin1(name))->setEnabled(two);
    });
}

std::vector<std::string> MainWindow::chosen_paint_layers() const {
    std::vector<std::string> out;
    const core::Page* page = current_page();
    if (page == nullptr || layer_panel_ == nullptr) return out;
    const std::vector<std::string> ids = layer_panel_->selected_ids();
    for (const core::Layer& layer : page->layers) {
        if (layer.kind == core::LayerKind::Raster && std::find(ids.begin(), ids.end(), layer.id) != ids.end()) out.push_back(layer.id);
    }
    return out;
}

void MainWindow::auto_align_dialog() {
    const core::Page* page = current_page();
    const std::vector<std::string> ids = chosen_paint_layers();
    if (page == nullptr || ids.size() < 2) {
        flash(QStringLiteral("レイヤー パネルで、画素のレイヤーを 2 枚以上選んでください"), 5000, true);
        return;
    }
    const Json at = page->index.json();
    AutoAlignDialog dialog(this);
    if (ask::exec(&dialog) != QDialog::Accepted) return;
    send_filter(Json::array({dialog.op(at, ids)}), QStringLiteral("レイヤーを自動整列"), true);
}

void MainWindow::auto_blend_dialog() {
    const core::Page* page = current_page();
    const std::vector<std::string> ids = chosen_paint_layers();
    if (page == nullptr || ids.size() < 2) {
        flash(QStringLiteral("レイヤー パネルで、画素のレイヤーを 2 枚以上選んでください"), 5000, true);
        return;
    }
    const Json at = page->index.json();
    AutoBlendDialog dialog(this);
    if (ask::exec(&dialog) != QDialog::Accepted) return;
    send_filter(Json::array({dialog.op(at, ids)}), QStringLiteral("レイヤーを自動合成"), true);
}

namespace {

// The open books' titles (開いているファイルを追加), in the window's order.
QStringList open_titles(const std::vector<Document>& docs) {
    QStringList out;
    for (const Document& d : docs) out << d.title();
    return out;
}

}  // namespace

void MainWindow::made_book(const QString& title, const std::function<formats::Opened()>& make_it) {
    // (the review's P2-6, and the coordinator's: I-h's run_stoppable — the progress window on the heap, one owner, its own
    // cancel taken off, 中止 asking the work to stop, a wait left early stopping it first —, its progress the render work's)
    const QPointer<MainWindow> alive(this);
    std::optional<formats::Opened> made;
    try {
        made = run_stoppable(this, title, QStringLiteral("%1を作っています…").arg(title), [make_it](const std::function<bool()>&) { return make_it(); });
    } catch (const std::exception& error) {
        if (alive) flash(QStringLiteral("%1: %2").arg(title, wording::error(QString::fromUtf8(error.what()))), 8000, true);
        return;
    }
    if (!alive) return;
    if (!made) {
        flash(QStringLiteral("%1を中止しました").arg(title), 4000);
        return;
    }
    add_picture_book(std::move(made->doc), made->notes);
}

namespace {

// What a source list names: files (a folder's pictures: their headers read on the worker, picture_files; each opened in its turn), and the open books' pages chosen
// (their snapshots).
struct Sources {
    std::vector<fs::path> files;
    std::vector<automate::Picture> open;
    // The pictures: the files opened (on the worker), then the open books'.
    // core::Error("value") for more pictures than `most`.
    void check(std::size_t most) const {
        if (files.size() + open.size() > most) throw core::Error("value", "more than " + std::to_string(most) + " pictures: choose fewer");
    }
    std::vector<automate::Picture> pictures(const formats::OpenOptions& options) const {
        std::vector<automate::Picture> out = automate::picture_files(files, options);
        out.insert(out.end(), open.begin(), open.end());
        return out;
    }
};

Sources pictures_of(const SourceFiles& source, const std::vector<Document>& docs, int current, const core::Page* shown) {
    std::vector<fs::path> files;
    if (source.use->currentData().toString() == QLatin1String("folder")) {
        const automate::Inputs inputs = automate::list_inputs(source.files());
        for (const automate::Input& input : inputs.items) {
            if (!input.book) files.push_back(input.path);
        }
    } else {
        files = source.files();
    }
    Sources out;
    out.files = std::move(files);
    for (const int i : source.open_chosen()) {
        if (i < 0 || i >= static_cast<int>(docs.size()) || !docs[static_cast<std::size_t>(i)].session) continue;
        const Document& d = docs[static_cast<std::size_t>(i)];
        const std::shared_ptr<const core::Document> snap = d.session->snapshot();
        std::size_t page = 0;
        if (i == current && shown != nullptr) {
            for (std::size_t k = 0; k < snap->pages.size(); ++k) {
                if (snap->pages[k].get() == shown) page = k;
            }
        } else if (d.page_index >= 0 && static_cast<std::size_t>(d.page_index) < snap->pages.size()) {
            page = static_cast<std::size_t>(d.page_index);
        }
        if (snap->is_deferred(page)) throw core::Error("page_not_loaded", "the page is not read yet");
        out.open.push_back(automate::Picture::of(d.title().toStdString(), snap, page));
    }
    return out;
}

}  // namespace

void MainWindow::photomerge_dialog() {
    PhotomergeDialog dialog(this, open_titles(documents_));
    if (ask::exec(&dialog) != QDialog::Accepted) return;
    automate::PhotomergeOptions options = dialog.options();
    options.open = file_open_options();
    try {
        const Sources sources = pictures_of(dialog.source, documents_, doc_, current_page());
        sources.check(render::align::kMaxLayers);  // (the review's P1-1, P3-1: more than it takes said before any file is read)
        made_book(QStringLiteral("Photomerge"), [sources, options] { return automate::photomerge(sources.pictures(options.open), options).opened; });
    } catch (const std::exception& error) {
        flash(QStringLiteral("Photomerge: %1").arg(wording::error(QString::fromUtf8(error.what()))), 8000, true);
    }
}

void MainWindow::load_stack_dialog() {
    StackDialog dialog(this, open_titles(documents_), false);
    if (ask::exec(&dialog) != QDialog::Accepted) return;
    automate::StackOptions options;
    options.actor = core::Actor(session_ ? session_->actor() : std::string("human:作者"));
    options.open = file_open_options();
    options.align = dialog.align->isChecked();
    options.smart = dialog.smart != nullptr && dialog.smart->isChecked();
    try {
        const Sources sources = pictures_of(dialog.source, documents_, doc_, current_page());
        if (options.align) sources.check(render::align::kMaxLayers);
        made_book(QStringLiteral("ファイルをスタックとして読み込み"), [sources, options] { return automate::load_files_into_stack(sources.pictures(options.open), options); });
    } catch (const std::exception& error) {
        flash(QStringLiteral("ファイルをスタックとして読み込み: %1").arg(wording::error(QString::fromUtf8(error.what()))), 8000, true);
    }
}

void MainWindow::statistics_dialog() {
    StackDialog dialog(this, open_titles(documents_), true);
    if (ask::exec(&dialog) != QDialog::Accepted) return;
    automate::StackOptions options;
    options.actor = core::Actor(session_ ? session_->actor() : std::string("human:作者"));
    options.open = file_open_options();
    options.align = dialog.align->isChecked();
    const std::string mode = dialog.mode->currentData().toString().toStdString();
    try {
        const Sources sources = pictures_of(dialog.source, documents_, doc_, current_page());
        if (options.align) sources.check(render::align::kMaxLayers);
        made_book(QStringLiteral("統計"), [sources, options, mode] { return automate::statistics(sources.pictures(options.open), mode, options); });
    } catch (const std::exception& error) {
        flash(QStringLiteral("統計: %1").arg(wording::error(QString::fromUtf8(error.what()))), 8000, true);
    }
}

}  // namespace genko::app
