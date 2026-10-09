// The book and its pages in the window (M4②c, Python's main.py): 次のページと見開きにする／解除 (_toggle_spread and
// set_spread, which the page list's menu uses too), ノンブルの設定… (pages_panel.nombre_dialog), 原稿用紙の設定…
// (_paper_settings), このページのノンブルを隠す／出す (_toggle_page_nombre), 表紙・カバーを足す… (_add_cover_dialog),
// このページの担当… (_assignee_dialog), ほかの原稿のページを取り込む… (_merge_book) and PSD をレイヤーのまま読み込む…
// (_import_psd). Every change is the op Python's window sends, through apply_ops; a book or page that changed while a
// question was open is left alone (asking / still).

#include <QFileInfo>

#include <filesystem>
#include <system_error>

#include "app/ask.hpp"
#include "app/book_dialogs.hpp"
#include "app/canvas.hpp"
#include "app/dialogs.hpp"
#include "app/layer_panel.hpp"
#include "app/main_window.hpp"
#include "app/pages_panel.hpp"
#include "app/save_status.hpp"
#include "app/wording.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/paths.hpp"
#include "storage/asset_store.hpp"
#include "storage/reader.hpp"

namespace genko::app {

namespace fs = std::filesystem;
using core::Json;
using core::Num;

namespace {

QString number(const Num& n) { return QString::fromStdString(n.repr()); }

// The page with this number ({p.index: p for p in pages}: the last one so numbered), or none.
const core::Page* numbered(const core::Document& book, const Num& index) {
    const core::Page* found = nullptr;
    for (const auto& page : book.pages) {
        if (page->index == index) found = page.get();
    }
    return found;
}

// The other book's folder and this one, the same (Path.resolve() of each, compared).
bool same_folder(const fs::path& a, const fs::path& b) {
    std::error_code ea, eb;
    const fs::path x = fs::weakly_canonical(a, ea);
    const fs::path y = fs::weakly_canonical(b, eb);
    return !ea && !eb && x == y;
}

// Why the other book's asset files could not be copied, in the person's words: one too large (wording's own words), or
// this book's folder that cannot be written (as a save that fails says it).
QString copy_refusal(const core::Error& error) {
    if (error.code() == "memory") return wording::error(QString::fromUtf8(error.what()));
    SaveStatus status;
    status.kind = SaveKind::Failed;
    status.code = QString::fromStdString(error.code());
    status.reason = QString::fromUtf8(error.what());
    return QStringLiteral("取り込む原稿の素材ファイルを、この原稿に写せませんでした: %1").arg(failure_reason(status));
}

}  // namespace

void MainWindow::build_book_actions() {
    make("act_spread", QStringLiteral("次のページと見開きにする／解除"), [this] { toggle_spread(); });
    make("act_nombre", QStringLiteral("ノンブルの設定…"), [this] { nombre_dialog(); }, {}, QStringLiteral("位置・書体・大きさ・始まりの番号・隠しノンブル"));
    make("act_paper", QStringLiteral("原稿用紙の設定…"), [this] { paper_settings(); }, {},
         QStringLiteral("用紙・仕上がり・裁ち落とし・基本枠。変えるとコマや台詞も新しい枠に合わせて動きます"));
    make("act_page_nombre", QStringLiteral("このページのノンブルを隠す／出す"), [this] { toggle_page_nombre(); });
    make("act_add_cover", QStringLiteral("表紙・カバーを足す…"), [this] { add_cover_dialog(); }, {}, QStringLiteral("表紙・裏表紙、または背と袖のあるカバー 1 枚"));
    make("act_assignee", QStringLiteral("このページの担当…"), [this] { assignee_dialog(); }, {},
         QStringLiteral("ページを誰が描くかを決めます（ページ一覧に出ます）"));
    make("act_merge_book", QStringLiteral("ほかの原稿のページを取り込む…"), [this] { merge_book(); }, {},
         QStringLiteral("別の原稿（.genko）のページを、台詞や絵ごとこの原稿の後ろに足します（作品の結合）"));
    make("act_import_psd", QStringLiteral("PSD をレイヤーのまま読み込む…"), [this] { import_psd(); }, {},
         QStringLiteral("Photoshop・CLIP STUDIO PAINT などの PSD／PSB を、レイヤー・フォルダー・マスク・合成モードのままこのページに"));
    // the page list's menu: a spread with the page after or before it, or undone; the page's nombre hidden or shown
    connect(pages_, &PageList::spreadRequested, this, [this](int index, int other) { set_spread(Num(index), Num(other)); });
    connect(pages_, &PageList::spreadUndone, this, [this](int index) { set_spread(Num(index), std::nullopt); });
    connect(pages_, &PageList::numeroRequested, this, [this](int index, bool numero) {
        apply_ops(Json::array({Json::object({{"op", "set_nombre"}, {"page", index}, {"numero", numero}})}));
    });
}

// set_spread(index, other): both pages say whom they face; none: the page and the one it faced, neither.
void MainWindow::set_spread(const Num& index, const std::optional<Num>& other) {
    const core::Page* page = nullptr;  // (next(p for p in pages if p.index == index): the first so numbered)
    for (const auto& p : book().pages) {
        if (p->index == index) {
            page = p.get();
            break;
        }
    }
    Json ops = Json::array();
    if (!other && page != nullptr && page->spread_with && page->spread_with->truthy()) {
        ops.push_back(Json::object({{"op", "set_spread"}, {"page", index.json()}, {"with", nullptr}}));
        ops.push_back(Json::object({{"op", "set_spread"}, {"page", page->spread_with->json()}, {"with", nullptr}}));
    } else if (other) {
        ops.push_back(Json::object({{"op", "set_spread"}, {"page", index.json()}, {"with", other->json()}}));
        ops.push_back(Json::object({{"op", "set_spread"}, {"page", other->json()}, {"with", index.json()}}));
    }
    if (!ops.empty() && apply_ops(ops)) reload_pages();
}

// 次のページと見開きにする／解除: undone when the page is in a spread; else with the next page when they face each other;
// else, asked first, with the page before when that one faces it (the next is the other side of its leaf); else said.
void MainWindow::toggle_spread() {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    if (page->spread_with && page->spread_with->truthy()) {
        set_spread(page->index, std::nullopt);
        return;
    }
    const core::Page* next = numbered(book(), page->index + Num(1));
    const core::Page* before = numbered(book(), page->index - Num(1));
    if (next != nullptr && !core::facing_problem(book(), *page, *next)) {
        set_spread(page->index, next->index);
        return;
    }
    // (with the next page it would be the two sides of one sheet: the page it faces is the one before)
    if (before != nullptr && !core::facing_problem(book(), *before, *page)) {
        const Asked asked = asking();
        const Num index = page->index;
        const Num facing = before->index;
        const QString text = QStringLiteral("%1 ページと %2 ページは 1 枚の紙の表と裏なので、見開きになりません。\n"
                                            "%1 ページと向かい合うのは %3 ページです。%3・%1 ページを見開きにしますか？")
                                 .arg(number(index), number(index + Num(1)), number(facing));
        if (ask::question(this, QStringLiteral("見開き"), text) && still(asked)) set_spread(facing, index);
        return;
    }
    flash(QStringLiteral("%1 ページには向かい合うページがありません").arg(number(page->index)), 4000);
}

void MainWindow::toggle_page_nombre() {
    if (const core::Page* page = current_page()) {
        apply_ops(Json::array({Json::object({{"op", "set_nombre"}, {"page", page->index.json()}, {"numero", !page->numero}})}));
    }
}

// ノンブルの設定… (pages_panel.nombre_dialog): every setting sent as the dialog has it.
void MainWindow::nombre_dialog() {
    const Asked asked = asking();
    std::unique_ptr<NombreDialog> dialog;
    try {
        dialog = std::make_unique<NombreDialog>(this, *asked.snapshot);
    } catch (const core::Error&) {
        // (settings written into the book that are not numbers: Python's dialog fails before it opens)
        flash(QStringLiteral("ノンブルの値が不正です。原稿は変更していません。"), 6000, true);
        return;
    }
    if (ask::exec(dialog.get()) == QDialog::Accepted && still(asked)) apply_ops(Json::array({dialog->op()}));
}

// 原稿用紙の設定… (_paper_settings): the paper dialog for a change; the book on its new paper, fitted to the view.
void MainWindow::paper_settings() {
    const Asked asked = asking();
    PaperDialog dialog(this, asked.snapshot->spec, true);
    if (ask::exec(&dialog) != QDialog::Accepted || !still(asked)) return;
    if (!apply_ops(Json::array({dialog.op()}))) return;
    reload_pages();
    canvas_->fit_page();
    flash(QStringLiteral("原稿用紙を変えました: %1").arg(QString::fromStdString(book().spec.describe())), 5000);
}

// 表紙・カバーを足す… (_add_cover_dialog): the cover at the end of the book, in front.
void MainWindow::add_cover_dialog() {
    const Asked asked = asking();
    CoverDialog dialog(this, *asked.snapshot);
    if (dialog.all_there()) {
        flash(QStringLiteral("表紙・裏表紙・カバー・帯は、もう全部あります"), 4000);
        return;
    }
    if (ask::exec(&dialog) != QDialog::Accepted || !still(asked)) return;
    if (!apply_ops(Json::array({dialog.op()}))) return;
    // _select_page(len(pages) - 1), the page list's row with it
    const int last = static_cast<int>(book().pages.size()) - 1;
    {
        const QSignalBlocker quiet(pages_);
        pages_->setCurrentRow(last);
    }
    select_page(last);
}

// このページの担当… (_assignee_dialog): the name for the page in front (empty: none).
void MainWindow::assignee_dialog() {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    const Asked asked = asking();
    const Json index = page->index.json();
    QString now;  // ((page.extra or {}).get("assignee", ""))
    if (const auto it = page->extra.find("assignee"); it != page->extra.end() && it->is_string()) now = QString::fromStdString(it->get<std::string>());
    const auto who = ask::get_text(this, QStringLiteral("このページの担当"), QStringLiteral("名前（空にすると担当なし）"), now);
    if (!who || !still(asked)) return;
    apply_ops(Json::array({Json::object({{"op", "set_assignee"}, {"pages", Json::array({index})}, {"who", who->toStdString()}})}));
}

// ほかの原稿のページを取り込む… (_merge_book, 作品の結合): another book's pages (all, or some) after this one's. Its asset
// files are copied into this book first (merge.copy_assets: storage::copy_assets, which takes only files laid out as
// assets that hold what their names say, and follows no link), then the pages go in by import_pages.
void MainWindow::merge_book() {
    if (!session_->path()) {
        flash(QStringLiteral("取り込む前に、この原稿を保存します（ファイル → 別の場所に保存）"), 6000);
        return;
    }
    const Asked asked = asking();
    const QString folder = ask::existing_dir(this, QStringLiteral("ページを取り込む原稿（.genko のフォルダ）"));
    if (folder.isEmpty()) return;
    const std::string from = python_path_text(folder);  // (str(Path(folder)))
    const fs::path src = core::path_from_utf8(from);
    std::error_code ec;
    if (!fs::is_regular_file(src / "project.json", ec)) {
        ask::warning(this, QStringLiteral("Genko"), QStringLiteral("Genko の原稿ではありません（.genko のフォルダを選びます）"));
        return;
    }
    if (same_folder(src, *session_->path())) {
        flash(QStringLiteral("同じ原稿は取り込めません（ページの複製を使います）"), 5000);
        return;
    }
    std::int64_t count = 0;
    try {
        const storage::LoadResult loaded = storage::load_document(src);
        // (a book this build can only open read-only cannot be taken in either: import_pages refuses it)
        if (!loaded.document.read_only_reason.empty()) throw core::Error("read_only", loaded.document.read_only_reason);
        count = static_cast<std::int64_t>(loaded.document.pages.size());
    } catch (const std::exception& error) {
        // (Python says the reader's own words; in the person's words where there are some)
        const QString why = QString::fromUtf8(error.what());
        const QString said = wording::error(why);
        flash(QStringLiteral("その原稿を読めませんでした:\n%1").arg(said.startsWith(QStringLiteral("この操作はできませんでした")) ? why : said), 6000, true);
        return;
    }
    const auto text = ask::get_text(this, QStringLiteral("作品の結合"), QStringLiteral("取り込むページ（1〜%1。例: 1-4, 7。空ならすべて）").arg(count));
    if (!text || !still(asked)) return;
    std::optional<std::vector<std::int64_t>> pages;
    try {
        pages = page_list(*text, count);
    } catch (const core::Error&) {
        flash(QStringLiteral("ページの書き方が読めません（例: 1-4, 7）"), 5000, true);
        return;
    }
    const Json ops = Json::array({import_op(from, pages)});
    if (!session_->read_only_reason().empty()) {  // (nothing is copied into a book open read-only: only the refusal)
        apply_ops(ops);
        return;
    }
    session_->save_now();  // (Python's commit_now)
    try {
        storage::copy_assets(src, *session_->path());
    } catch (const core::Error& error) {
        flash(copy_refusal(error), 6000, true);
        return;
    }
    const std::size_t before = book().pages.size();
    if (apply_ops(ops)) {
        flash(QStringLiteral("「%1」の %2 ページを後ろに足しました")
                  .arg(QString::fromStdString(core::path_to_utf8(src.stem())), QString::number(book().pages.size() - before)),
              5000);
    }
}

// PSD をレイヤーのまま読み込む… (_import_psd): the file's layers on the page in front, just above the layer chosen in the
// layer panel, fitted to the bleed. (Python connects the action to _import_psd(path=None) itself, so QAction.triggered's
// `checked` comes in as the path, False, and the command returns before asking; the file is asked for here, as
// _import_psd does when it is given none.)
void MainWindow::import_psd() {
    if (current_page() == nullptr) return;
    const Asked asked = asking();
    const QString path = ask::open_path(this, QStringLiteral("PSD を読み込む"), QStringLiteral("PSD (*.psd *.psb)"));
    if (path.isEmpty() || !still(asked)) return;
    const core::Page* page = current_page();
    const std::vector<std::string> chosen = layer_panel_->selected_ids();
    Json op = Json::object({{"op", "import_psd"}, {"page", page->index.json()}, {"path", path.toStdString()}, {"fit", "bleed"}});
    if (!chosen.empty() && !chosen.front().empty()) op["after"] = chosen.front();  // (just above the chosen layer)
    setCursor(Qt::WaitCursor);
    const bool ok = apply_ops(Json::array({op}));
    unsetCursor();
    if (ok) flash(QStringLiteral("「%1」をレイヤーのまま読み込みました").arg(QFileInfo(path).fileName()), 5000);
}

}  // namespace genko::app
