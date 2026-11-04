// 編集's history and memory (PC5 H-f, 節 H 6 章; SPEC.md VIEW-01「取り消しとメモリをクリア」): 最後の状態を切り替え after やり直し,
// and メモリをクリア ▸ (取り消し・クリップボード・ヒストリー・すべて・ビデオキャッシュ, Photoshop's) before 検索. Genko's history is
// the book's journal (storage/journal): the coordinator's decision — what is cleared is memory only, what the app keeps to be
// quick (the states the history holds in memory, the pictures the drawing remembers, the copy); every change stays in the
// journal, and Undo still reaches it. PhotoCraft v0.2.0's engine/src/edit_menu_cmds.rs purge (MIT) for its items and its
// answer (what was let go, the bytes).

#include <QLocale>
#include <QMenu>

#include <set>

#include "app/canvas.hpp"
#include "app/documents.hpp"
#include "app/main_window.hpp"
#include "app/memory.hpp"
#include "app/tiles.hpp"
#include "app/timeline.hpp"
#include "core/mesh3d.hpp"
#include "render/footage.hpp"
#include "render/page.hpp"

namespace genko::app {

void MainWindow::build_purge_actions() {
    // (Photoshop's Ctrl+Alt+Z in both key sets: free in Genko's)
    make("act_toggle_last_state", QStringLiteral("最後の状態を切り替え"), [this] { toggle_last_state(); }, {QKeySequence(QStringLiteral("Ctrl+Alt+Z"))},
         QStringLiteral("直前の状態と今の状態を行き来します（取り消しとやり直しを交互に。ほかの人の変更は戻しません）"));
    make("act_purge_undo", QStringLiteral("取り消し"), [this] { purge(QStringLiteral("undo")); }, {},
         QStringLiteral("この原稿の取り消しのために記憶している状態を空けます。取り消しの記録は原稿に残ります（取り消しはそのまま使えます）"));
    make("act_purge_clipboard", QStringLiteral("クリップボード"), [this] { purge(QStringLiteral("clipboard")); }, {},
         QStringLiteral("Genko でコピーしたもの（パソコンのクリップボードにある Genko の物も）を消して、記憶を空けます"));
    make("act_purge_histories", QStringLiteral("ヒストリー"), [this] { purge(QStringLiteral("histories")); }, {},
         QStringLiteral("開いているすべての原稿の、ヒストリーのために記憶している状態を空けます。取り消しの記録は原稿に残ります"));
    make("act_purge_all", QStringLiteral("すべて"), [this] { purge(QStringLiteral("all")); }, {},
         QStringLiteral("取り消し・クリップボード・ヒストリー・ビデオキャッシュと、描いた絵の覚え（レイヤー・配置画像・文字・3D・スマートオブジェクト・"
                        "見えていない細かいタイル）を空けます。取り消しの記録は原稿に残ります"));
    make("act_purge_video_cache", QStringLiteral("ビデオキャッシュ"), [this] { purge(QStringLiteral("video")); }, {},
         QStringLiteral("ビデオレイヤーの素材のフレームと、タイムラインで再生したフレームの覚えを空けます"));
}

void MainWindow::build_purge_menu(QMenu* edit) {
    QMenu* purge_menu = edit->addMenu(QStringLiteral("メモリをクリア"));
    purge_menu->setObjectName(QStringLiteral("menu_purge"));
    purge_menu->setToolTipsVisible(true);
    for (const char* name : {"act_purge_undo", "act_purge_clipboard", "act_purge_histories", "act_purge_all"}) purge_menu->addAction(action(QString::fromLatin1(name)));
    purge_menu->addSeparator();
    purge_menu->addAction(action(QStringLiteral("act_purge_video_cache")));
}

void MainWindow::toggle_last_state() {
    if (session_->stepped_back() && session_->can_redo()) {
        redo();
    } else {
        undo();
    }
}

MainWindow::Purged MainWindow::purge(const QString& what) {
    Purged out;
    const bool all = what == QLatin1String("all");
    const std::vector<MainWindow*> windows = [this] {
        std::vector<MainWindow*> list = documents::windows();
        if (std::find(list.begin(), list.end(), this) == list.end()) list.push_back(this);
        return list;
    }();
    // (the books' histories: this one's, or every open book's — a book open in two windows once)
    const auto histories = [&](bool every) {
        std::set<Session*> books;
        if (session_) books.insert(session_.get());
        if (every) {
            for (MainWindow* w : windows) {
                if (w->session_) books.insert(w->session_.get());
                for (const Document& d : w->documents_) {
                    if (d.session) books.insert(d.session.get());
                }
            }
        }
        bool any = false;
        for (Session* book : books) {
            const Session::Purged p = book->purge_history();
            any = any || p.changes > 0 || p.states > 0;
            out.bytes += static_cast<std::int64_t>(p.bytes);
        }
        return any;
    };
    if (what == QLatin1String("undo") || all) {
        if (histories(false)) out.items << QStringLiteral("取り消し");
    }
    if (what == QLatin1String("clipboard") || all) {
        std::int64_t bytes = 0;
        for (MainWindow* w : windows) bytes += w->purge_clipboard();
        if (bytes > 0) {
            out.items << QStringLiteral("クリップボード");
            out.bytes += bytes;
        }
    }
    if (what == QLatin1String("histories") || all) {
        if (histories(true)) out.items << QStringLiteral("ヒストリー");
    }
    if (what == QLatin1String("video") || all) {
        std::int64_t bytes = render::footage::cache_bytes();
        render::footage::clear_cache();
        for (MainWindow* w : windows) {
            if (w->timeline_ != nullptr) bytes += w->timeline_->purge_played();
        }
        if (bytes > 0) {
            out.items << QStringLiteral("ビデオキャッシュ");
            out.bytes += bytes;
        }
    }
    if (all) {
        // (what the drawing remembers between tiles and pages — drawn again when it is wanted, the same pictures —, and
        // the finer tiles out of sight of every canvas: the ones on the screen stay)
        std::int64_t bytes = render::render_cache_bytes();
        render::clear_render_caches();
        core::mesh3d::clear_caches();
        for (MainWindow* w : windows) {
            for (PageCanvas* c : w->findChildren<PageCanvas*>()) bytes += static_cast<std::int64_t>(c->renderer().purge());
        }
        if (bytes > 0) {
            out.items << QStringLiteral("描いた絵の覚え");
            out.bytes += bytes;
        }
    }
    memory::give_back();  // (the room let go, back to the system now)
    if (out.items.isEmpty()) {
        flash(QStringLiteral("クリアするものはありませんでした"), 3000);
    } else {
        const QString size = out.bytes > 0 ? QStringLiteral("（約 %1）").arg(QLocale::system().formattedDataSize(out.bytes)) : QString();
        const bool history = what == QLatin1String("undo") || what == QLatin1String("histories") || all;
        flash(QStringLiteral("メモリをクリアしました：%1%2。%3").arg(out.items.join(QStringLiteral("・")), size,
                                                                history ? QStringLiteral("取り消しの記録は原稿に残っています") : QString()),
              4000);
    }
    return out;
}

}  // namespace genko::app
