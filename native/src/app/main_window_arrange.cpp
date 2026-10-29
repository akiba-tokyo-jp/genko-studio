// ウィンドウ (PC5 H-0, 節 H 5.2; SPEC VIEW-01): アレンジ ▸ and ワークスペース ▸ at the top as Photoshop's, then the books
// open, the bars made one's own and the panels. H-0 made the places; H-c (窓の区画・浮動・同じ表示にする, 節 H 5.3, the user's
// decision 7) makes アレンジ ▸'s commands: the books side by side in cells of this window (app/doc_area, app/pane_view —
// PhotoCraft's view_cmds.rs arrange and cells, MIT), floated into windows of their own and taken back into tabs, their
// views made the same; and ツール's switch and 修飾キー among the panels. H-d fills ワークスペース ▸ (fill_workspaces).

#include <QDir>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QInputEvent>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QTabBar>
#include <QToolBar>
#include <QToolButton>

#include <algorithm>
#include <chrono>
#include <cmath>

#include "app/doc_area.hpp"
#include "app/ask.hpp"
#include "app/documents.hpp"
#include "app/ime.hpp"
#include "app/main_window.hpp"
#include "app/pane_view.hpp"
#include "app/session.hpp"
#include "app/theme.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"

namespace genko::app {

namespace {

// (the menu's arrangements: attribute, words, layout — Photoshop's order and its Japanese names)
struct Arrangement {
    const char* attribute;
    const char* words;
    const char* layout;
    const char* tip;
};

const std::vector<Arrangement>& arrangements() {
    static const std::vector<Arrangement> all{
        {"act_arrange_tile_vertical", "すべてを左右に並べる", "tileAllVertically", "開いている原稿をすべて、このウィンドウの中に左右に並べます"},
        {"act_arrange_tile_horizontal", "すべてを上下に並べる", "tileAllHorizontally", "開いている原稿をすべて、このウィンドウの中に上下に並べます"},
        {"act_arrange_2up_vertical", "2 アップ（縦）", "twoUpVertical", "原稿を 2 つ、縦長の区画に左右に並べます"},
        {"act_arrange_2up_horizontal", "2 アップ（横）", "twoUpHorizontal", "原稿を 2 つ、横長の区画に上下に並べます"},
        {"act_arrange_3up_vertical", "3 アップ（縦）", "threeUpVertical", "原稿を 3 つ、縦長の区画に左右に並べます"},
        {"act_arrange_3up_horizontal", "3 アップ（横）", "threeUpHorizontal", "原稿を 3 つ、横長の区画に上下に並べます"},
        {"act_arrange_3up_stacked", "3 アップ（スタック）", "threeUpStacked", "原稿を 3 つ、左に 1 つ・右に 2 つ重ねて並べます"},
        {"act_arrange_4up", "4 アップ", "fourUp", "原稿を 4 つ、2 × 2 の区画に並べます"},
        {"act_arrange_6up", "6 アップ", "sixUp", "原稿を 6 つ、3 × 2 の区画に並べます"},
    };
    return all;
}

// 修飾キー: the keys kept down added to the canvas's pointer and pen events (the tools read them there), and the tool a key
// holds for a moment (環境設定's Alt / Ctrl tools) held again at a press, if a click elsewhere let it go.
class Latch : public QObject {
public:
    explicit Latch(MainWindow* window) : QObject(window), window_(window) {}

    bool eventFilter(QObject* watched, QEvent* event) override {
        const Qt::KeyboardModifiers kept = window_->latched_keys();
        if (kept == Qt::NoModifier) return false;
        switch (event->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::TabletPress:
            for (const auto& [key, name] : keys()) {
                if ((kept & key) && !window_->canvas()->tool_held()) window_->canvas()->hold_modifier(name, true);
            }
            [[fallthrough]];
        case QEvent::MouseMove:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick:
        case QEvent::TabletMove:
        case QEvent::TabletRelease:
        case QEvent::Wheel: {
            auto* input = static_cast<QInputEvent*>(event);
            input->setModifiers(input->modifiers() | kept);
            break;
        }
        default:
            break;
        }
        (void)watched;
        return false;
    }

    static const std::vector<std::pair<Qt::KeyboardModifier, QString>>& keys() {
        static const std::vector<std::pair<Qt::KeyboardModifier, QString>> all{
            {Qt::ShiftModifier, QStringLiteral("shift")}, {Qt::ControlModifier, QStringLiteral("ctrl")}, {Qt::AltModifier, QStringLiteral("alt")}};
        return all;
    }

private:
    MainWindow* window_;
};

// A tab's mark of its cell: the arrangement's cells, its own filled.
QIcon cell_mark(const QString& layout, int count, int cell) {
    const std::vector<QRectF> cells = DocArea::cells(layout, count);
    QPixmap picture(18, 14);
    picture.fill(Qt::transparent);
    QPainter painter(&picture);
    const theme::Tokens t = theme::tokens();
    const QRectF room(1, 1, 16, 12);
    for (int i = 0; i < static_cast<int>(cells.size()); ++i) {
        const QRectF& c = cells[static_cast<std::size_t>(i)];
        const QRectF r(room.x() + c.x() * room.width(), room.y() + c.y() * room.height(), c.width() * room.width(), c.height() * room.height());
        painter.setPen(QColor(t.muted));
        painter.setBrush(i == cell ? QColor(t.accent) : QColor(Qt::transparent));
        painter.drawRect(r.adjusted(0.5, 0.5, -0.5, -0.5));
    }
    painter.end();
    return QIcon(picture);
}

}  // namespace

void MainWindow::build_arrange_actions() {
    // (window.arrange.*: the books side by side in this window's cells — the tab in front first —, each fitted to its cell)
    for (const Arrangement& a : arrangements()) {
        const QString layout = QString::fromLatin1(a.layout);
        make(QString::fromLatin1(a.attribute), QString::fromUtf8(a.words), [this, layout] { arrange(layout); }, {}, QString::fromUtf8(a.tip));
    }
    make("act_arrange_consolidate", QStringLiteral("すべてをタブに統合"), [this] { consolidate_all(); }, {},
         QStringLiteral("並べた区画と、ほかのウィンドウに分離した原稿を、このウィンドウのタブに戻します"));
    make("act_arrange_cascade", QStringLiteral("重ねて表示"), [this] { float_all(true); }, {},
         QStringLiteral("前の原稿のほかを、それぞれのウィンドウに分離して少しずつずらして重ねます"));
    make("act_arrange_tile", QStringLiteral("並べて表示"), [this] { arrange(QStringLiteral("tile")); }, {},
         QStringLiteral("開いている原稿をすべて、このウィンドウの中にできるだけ正方形に近い区画で並べます"));
    make("act_float_window", QStringLiteral("ウィンドウを分離"), [this] { float_in_window(); }, {},
         QStringLiteral("前の原稿のタブを、このウィンドウから出して自分のウィンドウにします（ページ・表示はそのまま）"));
    make("act_float_all", QStringLiteral("すべてのウィンドウを分離"), [this] { float_all(false); }, {},
         QStringLiteral("前の原稿のほかのタブを、それぞれのウィンドウにします"));
    make("act_match_zoom", QStringLiteral("同じズーム倍率で表示"), [this] { match_views(true, false, false); }, {},
         QStringLiteral("ほかの区画とウィンドウの原稿を、前の原稿と同じ表示倍率（%）にします"));
    make("act_match_location", QStringLiteral("同じ位置に表示"), [this] { match_views(false, true, false); }, {},
         QStringLiteral("ほかの区画とウィンドウの原稿を、前の原稿と同じ位置（ページの中の同じ所）を見る表示にします"));
    make("act_match_rotation", QStringLiteral("同じ回転で表示"), [this] { match_views(false, false, true); }, {},
         QStringLiteral("ほかの区画とウィンドウの原稿を、前の原稿と同じ表示の回転・反転にします"));
    make("act_match_all", QStringLiteral("すべてを同じ表示にする"), [this] { match_views(true, true, true); }, {},
         QStringLiteral("ほかの区画とウィンドウの原稿を、前の原稿と同じ倍率・位置・回転にします"));
    // 「<名前>」の新規ウィンドウ (window.arrange.newWindowForDocument: this book in one more window — its id kept, the old name
    // in preferences::renamed()). Its words stay the same for every book: a command is kept — its keys, its place in the
    // quick access — by its words.
    make("act_new_window", QStringLiteral("新規ウィンドウ"), [this] { new_window(); }, {},
         QStringLiteral("この原稿をもう 1 つのウィンドウで開きます。拡大して描きながら、別の窓で全体を見る"));
    // 手のひらツール's すべてのウィンドウをスクロール and ズームツール's すべてのウィンドウをズーム (their options: build_view_pages)
    QAction* scroll = make("act_scroll_all", QStringLiteral("すべてのウィンドウをスクロール"), [] {}, {},
                           QStringLiteral("手のひらツールのドラッグで、並べたほかの区画とウィンドウの原稿も同じだけ動かします（Shift を押している間は区画だけ）"), true);
    connect(scroll, &QAction::toggled, this, [this](bool on) { canvas_->scroll_all = on; });
    QAction* zoom = make("act_zoom_all", QStringLiteral("すべてのウィンドウをズーム"), [] {}, {},
                         QStringLiteral("ズームツールで、並べたほかの区画とウィンドウの原稿も同じだけ拡大・縮小します（Shift を押している間は区画だけ）"), true);
    connect(zoom, &QAction::toggled, this, [this](bool on) { canvas_->zoom_all = on; });
}

void MainWindow::build_arrange_menus(QMenu* arrange) {
    arrange->setObjectName(QStringLiteral("menu_arrange"));
    // (Photoshop's order, 節 H 5.2: the cells | すべてをタブに統合, 重ねて表示, 並べて表示 | ウィンドウを分離, すべてのウィンドウを分離 |
    // 同じ… | 新規ウィンドウ)
    for (const Arrangement& a : arrangements()) arrange->addAction(action(QString::fromLatin1(a.attribute)));
    arrange->addSeparator();
    for (const char* name : {"act_arrange_consolidate", "act_arrange_cascade", "act_arrange_tile"}) arrange->addAction(action(QString::fromLatin1(name)));
    arrange->addSeparator();
    for (const char* name : {"act_float_window", "act_float_all"}) arrange->addAction(action(QString::fromLatin1(name)));
    arrange->addSeparator();
    for (const char* name : {"act_match_zoom", "act_match_location", "act_match_rotation", "act_match_all"}) arrange->addAction(action(QString::fromLatin1(name)));
    arrange->addSeparator();
    arrange->addAction(action("act_new_window"));
}

void MainWindow::name_panels() {
    // (節 H 1.2: Photoshop's names — the panels' switches in ウィンドウ follow; renamed() reads the old ones)
    for (const auto& [object, title] : {std::pair{"全体図", "ナビゲーター"}, {"ツールの設定", "オプション"}, {"履歴", "ヒストリー"}}) {
        for (QDockWidget* dock : findChildren<QDockWidget*>(QString::fromUtf8(object))) dock->setWindowTitle(QString::fromUtf8(title));
    }
}

void MainWindow::build_arrange_docks() {
    // ツール (window.panel.tools): the tool palette shown or hidden
    view_menu_->addAction(palette_->toggleViewAction());
    // 修飾キー (window.panel.modifierKeys, PhotoCraft's workspace_ui.rs Modifier Keys): Shift, Ctrl and Alt kept down for the
    // canvas while their buttons are in
    auto* keys = new QWidget;
    auto* row = new QHBoxLayout(keys);
    row->setContentsMargins(6, 6, 6, 6);
    for (const auto& [key, name] : Latch::keys()) {
        auto* button = new QToolButton;
        button->setObjectName(QStringLiteral("mod_") + name);
        button->setText(key == Qt::ShiftModifier ? QStringLiteral("Shift") : key == Qt::ControlModifier ? QStringLiteral("Ctrl") : QStringLiteral("Alt"));
        button->setCheckable(true);
        button->setFocusPolicy(Qt::NoFocus);  // (the canvas keeps the keys' focus)
        button->setMinimumSize(48, 32);
        button->setToolTip(QStringLiteral("押している間と同じに、キャンバスの道具に %1 を効かせます（もう一度押すと離します）").arg(button->text()));
        const Qt::KeyboardModifier modifier = key;
        connect(button, &QToolButton::toggled, this, [this, modifier](bool on) { latch_key(modifier, on); });
        row->addWidget(button);
    }
    row->addStretch(1);
    modifier_dock_ = new QDockWidget(QStringLiteral("修飾キー"), this);
    modifier_dock_->setObjectName(QStringLiteral("修飾キー"));
    modifier_dock_->setWidget(keys);
    modifier_dock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::LeftDockWidgetArea, modifier_dock_);
    modifier_dock_->hide();  // (for a touch screen or a pen alone: ウィンドウ brings it)
    // (its switch turned off — ウィンドウ → 修飾キー —: the keys kept down let go; hidden behind another panel's tab or by
    // 原稿だけを表示, they stay down — review P3-8)
    connect(modifier_dock_->toggleViewAction(), &QAction::triggered, this, [keys](bool shown) {
        if (shown) return;
        for (QToolButton* button : keys->findChildren<QToolButton*>()) button->setChecked(false);
    });
    view_menu_->addAction(modifier_dock_->toggleViewAction());
    canvas_->installEventFilter(new Latch(this));
}

void MainWindow::latch_key(Qt::KeyboardModifier key, bool on) {
    if (on == latched_.testFlag(key)) return;
    latched_.setFlag(key, on);
    for (const auto& [modifier, name] : Latch::keys()) {
        if (modifier == key) canvas_->hold_modifier(name, on);  // (環境設定's Alt / Ctrl tool, as the key held)
    }
}

// --- the cells ---------------------------------------------------------------------------------------------------------

int MainWindow::document_of(std::uint64_t tab) const {
    for (std::size_t i = 0; i < documents_.size(); ++i) {
        if (documents_[i].tab == tab) return static_cast<int>(i);
    }
    return -1;
}

void MainWindow::arrange(const QString& layout) {
    if (doc_area_ == nullptr) return;
    const int count = static_cast<int>(documents_.size());
    if (layout != QLatin1String("tabs") && count < 2) {
        flash(QStringLiteral("並べるには、原稿を 2 つ以上開いてください"), 4000);
        return;
    }
    store_document();
    // (the tab in front first, then the next ones in the tabs' order, as PhotoCraft's: as many as there are cells)
    const int cells = DocArea::capacity(layout, count);
    std::vector<std::uint64_t> tabs;
    for (int k = 0; k < cells; ++k) tabs.push_back(documents_[static_cast<std::size_t>((doc_ + k) % count)].tab);
    doc_area_->arrange(layout, tabs, 0);
    // (each fits its new cell: PhotoCraft's fit_pending)
    for (int k = 1; k < cells; ++k) documents_[static_cast<std::size_t>((doc_ + k) % count)].view.reset();
    for (int i = 0; i < static_cast<int>(doc_area_->tabs().size()); ++i) {
        if (PaneView* pane = doc_area_->pane(i)) fill_pane(pane, doc_area_->tabs()[static_cast<std::size_t>(i)]);
    }
    if (cells > 0) canvas_->fit_page();
    mark_tabs();
    if (cells > 0 && count > cells) {
        flash(QStringLiteral("区画は %1 までです。ほかの原稿はタブで選ぶと、前の区画に出ます").arg(cells), 5000);
    }
}

void MainWindow::fill_pane(PaneView* pane, std::uint64_t tab) {
    const int i = document_of(tab);
    if (pane == nullptr || i < 0) return;
    const Document& d = documents_[static_cast<std::size_t>(i)];
    pane_look(pane);
    (void)proof_renderer(pane->renderer(), d.proof);  // (its own 色の校正 and 32 bit preview; a profile unread: none)
    pane->show_tab(
        tab, d.session,
        [this, tab]() -> std::optional<PaneView::Shown> {
            const int at = document_of(tab);
            if (at < 0) return std::nullopt;
            const Document& doc = documents_[static_cast<std::size_t>(at)];
            const DocPtr book = doc.session->snapshot();
            if (!book) return std::nullopt;
            if (doc.contents) {  // (smart-objects@1: a contents' tab — its contents' page alone, as shown_book)
                const auto found = book->smart_contents.find(*doc.contents);
                if (found == book->smart_contents.end() || !found->second || !found->second->page) return std::nullopt;
                auto view = std::make_shared<core::Document>(*book);
                view->pages = {found->second->page};
                view->deferred.clear();
                view->story.clear();
                return PaneView::Shown{std::move(view), 0, "proof"};
            }
            if (book->pages.empty()) return std::nullopt;
            const std::size_t index = static_cast<std::size_t>(std::clamp(doc.page_index, 0, static_cast<int>(book->pages.size()) - 1));
            const core::Page& page = *book->pages[index];
            // (as show_page: a book made with agents shows its name stage's pages as the name)
            const bool agents = book->strict_gates || core::py_truthy(book->studio);
            return PaneView::Shown{book, index, !page.name_ok && agents ? "name" : "proof"};
        },
        d.view);
}

void MainWindow::pane_look(PaneView* pane) {
    // (what the canvas in front shows over the page, the pane too: the guides, the grid, the extras and their switches)
    pane->show_guides = canvas_->show_guides;
    pane->show_frame_numbers = canvas_->show_frame_numbers;
    pane->rulers_visible = canvas_->rulers_visible;
    pane->grid_visible = canvas_->grid_visible;
    pane->grid_mm = canvas_->grid_mm;
    pane->extras_shown = canvas_->extras_shown;
    pane->show_guide_lines = canvas_->show_guide_lines;
    pane->show_canvas_guides = canvas_->show_canvas_guides;
    pane->guide_colour = canvas_->guide_colour;
    pane->grid_colour = canvas_->grid_colour;
    pane->print_dpi = canvas_->print_dpi;
    // (the production's extras — PC5 J-c 注釈, J-d1 カウント・ものさしツール's line — drawn by the pane's own painters, each
    // shown as the canvas shows it; J-b's スライス adds its own here the same way: review P3-3)
    using View = PageCanvas::ExtraView;
    pane->add_extra({QStringLiteral("notes"), [pane](QPainter& painter, const core::Page& page, const View& view) { pane->paint_notes(painter, page, view); }});
    pane->add_extra({QStringLiteral("count"), [pane](QPainter& painter, const core::Page& page, const View& view) { pane->paint_count(painter, page, view); }});
    pane->add_extra({QStringLiteral("ruler"), [pane](QPainter& painter, const core::Page& page, const View& view) { pane->paint_ruler(painter, page, view); }});
    for (const QString& key : pane->extra_keys()) pane->show_extra(key, canvas_->extra_shown(key));
}

void MainWindow::panes_follow() {
    // (review P3-4: エクストラ, 表示・非表示 ▸, the grid, the guides' colours, the monitor's profile changed — every pane at once)
    if (doc_area_ == nullptr) return;
    for (PaneView* pane : doc_area_->panes()) {
        pane_look(pane);
        const int i = document_of(pane->tab());
        if (i >= 0) (void)proof_renderer(pane->renderer(), documents_[static_cast<std::size_t>(i)].proof);
        pane->update();
    }
}

QString MainWindow::tab_tip(int index) const {
    if (index < 0 || index >= static_cast<int>(documents_.size())) return {};
    const Document& d = documents_[static_cast<std::size_t>(index)];
    const auto path = d.session ? d.session->path() : std::nullopt;
    QString tip = path ? QDir::toNativeSeparators(QString::fromStdString(core::path_to_utf8(*path))) : QStringLiteral("未保存");
    // (PC5 H-c review P3-7: its cell besides its path)
    const int cell = doc_area_ != nullptr && doc_area_->front() >= 0 ? doc_area_->cell_of(d.tab) : -1;
    if (cell >= 0) tip += QStringLiteral("（区画 %1）").arg(cell + 1);
    return tip;
}

void MainWindow::keep_pane_views() {
    if (doc_area_ == nullptr) return;
    for (PaneView* pane : doc_area_->panes()) {
        const int i = document_of(pane->tab());
        if (i >= 0 && pane->page() != nullptr) documents_[static_cast<std::size_t>(i)].view = pane->view_state();
    }
}

void MainWindow::arrange_follow() {
    if (doc_area_ == nullptr || doc_ < 0 || doc_ >= static_cast<int>(documents_.size())) return;
    if (doc_area_->front() < 0) {
        mark_tabs();
        return;
    }
    const std::uint64_t now = documents_[static_cast<std::size_t>(doc_)].tab;
    const std::vector<std::uint64_t> tabs = doc_area_->tabs();
    const int front = doc_area_->front();
    const bool gone = std::any_of(tabs.begin(), tabs.end(), [this](std::uint64_t tab) { return document_of(tab) < 0; });
    if (!gone) {
        const int cell = doc_area_->cell_of(now);
        if (cell < 0) {
            doc_area_->set_tab(front, now);  // (a tab in no cell: shown in the cell in front, as Photoshop's n-up)
        } else if (cell != front) {
            // (the canvas to the cell of the tab now in front; the pane there to the cell it left, showing the tab that left
            // the front — its view as store_document kept it)
            if (PaneView* moved = doc_area_->bring_front(cell)) fill_pane(moved, tabs[static_cast<std::size_t>(front)]);
        }
        mark_tabs();
        return;
    }
    // (tabs closed or floated leave their cells; the tab now in front, in none, takes the cell in front's place)
    const bool placed = std::find(tabs.begin(), tabs.end(), now) != tabs.end();
    std::vector<std::uint64_t> kept;
    for (int i = 0; i < static_cast<int>(tabs.size()); ++i) {
        const std::uint64_t tab = tabs[static_cast<std::size_t>(i)];
        if (!placed && i == front) {
            kept.push_back(now);
        } else if (document_of(tab) >= 0) {
            kept.push_back(tab);
        }
    }
    const int at = static_cast<int>(std::find(kept.begin(), kept.end(), now) - kept.begin());
    doc_area_->arrange(kept.size() < 2 ? QStringLiteral("tabs") : doc_area_->layout(), kept, at);
    for (int i = 0; i < static_cast<int>(doc_area_->tabs().size()); ++i) {
        if (PaneView* pane = doc_area_->pane(i)) fill_pane(pane, doc_area_->tabs()[static_cast<std::size_t>(i)]);
    }
    mark_tabs();
}

void MainWindow::activate_pane(int cell) {
    PaneView* pane = doc_area_ != nullptr ? doc_area_->pane(cell) : nullptr;
    if (pane == nullptr) return;
    const int i = document_of(pane->tab());
    if (i < 0 || i == doc_) return;
    if (pane->page() != nullptr) documents_[static_cast<std::size_t>(i)].view = pane->view_state();  // (as it was seen in the pane)
    switch_document(i);  // (what is half done in front settled as for a tab chosen; refused: nothing moves)
}

void MainWindow::mark_tabs() {
    if (doc_tabs_ == nullptr || doc_area_ == nullptr) return;
    const bool cells = doc_area_->front() >= 0;
    if (!cells && !tabs_marked_) return;  // (the tabs' way, as it always was: no marks)
    tabs_marked_ = cells;
    const int count = static_cast<int>(doc_area_->tabs().size());
    for (int i = 0; i < doc_tabs_->count() && i < static_cast<int>(documents_.size()); ++i) {
        const int cell = cells ? doc_area_->cell_of(documents_[static_cast<std::size_t>(i)].tab) : -1;
        doc_tabs_->setTabIcon(i, cell >= 0 ? cell_mark(doc_area_->layout(), count, cell) : QIcon());
        doc_tabs_->setTabToolTip(i, tab_tip(i));
    }
}

void MainWindow::hand_used(double factor, const QPointF& moved, bool every_window) {
    std::vector<PageCanvas*> others;
    if (doc_area_ != nullptr) {
        for (PaneView* pane : doc_area_->panes()) others.push_back(pane);
    }
    if (every_window) {
        for (MainWindow* window : documents::windows()) {
            if (window != this) others.push_back(window->canvas());
        }
    }
    for (PageCanvas* c : others) {
        if (c->page() == nullptr) continue;
        if (factor > 0 && std::abs(factor - 1.0) > 1e-9) c->zoom_by(factor);
        if (!moved.isNull()) c->pan_by(moved);
    }
}

void MainWindow::match_views(bool zoom, bool location, bool rotation) {
    const core::Page* page = canvas_->page();
    if (page == nullptr) return;
    const double percent = canvas_->zoom_percent_exact();
    // (the place: where the middle of the view is on its page, as a part of the page — books of other sizes the same place)
    const QPointF middle = canvas_->to_mm(QPointF(canvas_->width() / 2.0, canvas_->height() / 2.0));
    const double fx = middle.x() / page->spec.width_mm.value();
    const double fy = middle.y() / page->spec.height_mm.value();
    std::vector<PageCanvas*> others;
    if (doc_area_ != nullptr) {
        for (PaneView* pane : doc_area_->panes()) others.push_back(pane);
    }
    for (MainWindow* window : documents::windows()) {
        if (window != this) others.push_back(window->canvas());
    }
    for (PageCanvas* c : others) {
        const core::Page* there = c->page();
        if (there == nullptr) continue;
        if (rotation) {
            c->set_rotation(canvas_->rotation());
            c->flip_view(canvas_->flipped());
            c->flip_view_vertical(canvas_->flipped_vertical());
        }
        if (zoom) c->set_zoom_percent(percent);
        if (location) c->center_on(fx * there->spec.width_mm.value(), fy * there->spec.height_mm.value());
    }
}

// --- floating and taking back ---------------------------------------------------------------------------------------

void MainWindow::take_document(const Document& doc) {
    if (doc.session) {
        doc.session->set_idle(std::chrono::milliseconds(save_after_ms_));
        connect(doc.session.get(), &Session::statusChanged, this, &MainWindow::maybe_backup, Qt::UniqueConnection);
    }
    documents_.push_back(doc);
    doc_tabs_->blockSignals(true);
    doc_tabs_->addTab(doc.title());
    doc_tabs_->blockSignals(false);
}

void MainWindow::adopt_first(const Document& doc) {
    if (documents_.empty()) return;
    documents_[0] = doc;
    doc_ = -1;
    show_document(0);
}

void MainWindow::finish_typing() {
    // (review P3-12: words being typed go into their own book before its tab moves — a menu took the focus, not the page)
    if (InlineEditor* typing = canvas_->editor()) typing->finish(true);
}

void MainWindow::float_in_window() {
    finish_typing();
    if (documents_.size() < 2) {
        flash(QStringLiteral("原稿が 1 つなので、このウィンドウのままです"), 4000);
        return;
    }
    store_document();
    const Document moving = documents_[static_cast<std::size_t>(doc_)];
    moving.session->save_now();
    auto* window = new MainWindow(moving.session);
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->resize(size());
    window->adopt_first(moving);  // (its page, its view, its layer and its proof as they were here)
    window->show();
    window->move(pos() + QPoint(40, 40));
    close_document(doc_);  // (the book is open there now: nothing asked)
}

void MainWindow::float_all(bool cascade) {
    finish_typing();
    if (documents_.size() < 2) {
        flash(QStringLiteral("原稿が 1 つなので、このウィンドウのままです"), 4000);
        return;
    }
    store_document();
    const std::uint64_t front = documents_[static_cast<std::size_t>(doc_)].tab;
    // (review P3-6: as many windows as cells at most — this one with them —; the rest stay in their tabs, said)
    std::vector<Document> going;
    std::size_t left = 0;
    for (const Document& d : documents_) {
        if (d.tab == front) continue;
        if (static_cast<int>(going.size()) + 1 < DocArea::kMostPanes) {
            going.push_back(d);
        } else {
            ++left;
        }
    }
    std::vector<MainWindow*> made;
    int step = 0;
    for (const Document& d : going) {
        d.session->save_now();
        auto* window = new MainWindow(d.session);
        window->setAttribute(Qt::WA_DeleteOnClose);
        window->resize(size());
        window->adopt_first(d);
        window->show();
        window->move(pos() + QPoint(40, 40) * ++step);
        made.push_back(window);
    }
    // (their tabs here go: their books are open there — nothing asked)
    for (int i = static_cast<int>(documents_.size()) - 1; i >= 0; --i) {
        const std::uint64_t tab = documents_[static_cast<std::size_t>(i)].tab;
        if (std::any_of(going.begin(), going.end(), [tab](const Document& d) { return d.tab == tab; })) close_document(i);
    }
    if (left > 0) flash(QStringLiteral("ウィンドウは %1 までです。残りの %2 つの原稿はタブのままです").arg(DocArea::kMostPanes).arg(left), 5000);
    if (cascade) {
        // 重ねて表示: the windows one over another 40 px apart, this one first and on top
        for (MainWindow* window : made) window->raise();
        raise();
        activateWindow();
    }
}

void MainWindow::consolidate_all() {
    finish_typing();
    // (review P3-9: what is half done in another window — a transform, the crop box, words being typed — is never let go
    // without a word: finished there first, as Enter would, or nothing is taken in)
    for (MainWindow* other : documents::windows()) {
        if (other == this || !other->canvas_->half_done()) continue;
        if (!ask::question(this, QStringLiteral("すべてをタブに統合"),
                           QStringLiteral("「%1」のウィンドウに、確定していない作業（変形・切り抜きの枠・パス・文字の入力など）があります。"
                                          "確定してからタブに統合しますか？（いいえ: 統合をやめます）")
                               .arg(other->documents_[static_cast<std::size_t>(std::max(0, other->doc_))].title()))) {
            return;
        }
        other->canvas_->finish_half_done();
    }
    store_document();
    for (MainWindow* other : documents::windows()) {
        if (other == this) continue;
        other->store_document();
        for (const Document& d : other->documents_) {
            // (the same book in the same way here already — one book opened in two windows —: not twice)
            const bool here = std::any_of(documents_.begin(), documents_.end(),
                                          [&d](const Document& h) { return h.session == d.session && h.contents == d.contents; });
            if (!here) take_document(d);
        }
        other->close();  // (its books are open here: closed without a question)
    }
    if (doc_area_ != nullptr && doc_area_->front() >= 0) arrange(QStringLiteral("tabs"));
    mark_tabs();
}

}  // namespace genko::app
