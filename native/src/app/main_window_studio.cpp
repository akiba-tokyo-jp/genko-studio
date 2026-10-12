// The studio in the window (M5④, Python's main.py with studio_widgets.py and ai_link.py): the approval box over the
// panels while something waits for the person, the panel view (コマの詳細) and the library (資料) as tabs — books made
// with agents only —, the process bar and the AI button in the status bar (AI と作る), チャットでの承認…, the note and
// the box in front when the AI's requests arrive, and genko:// links (the style catalog's 「この絵柄を使う」).

#include <QApplication>
#include <QDockWidget>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QStatusBar>

#include <algorithm>

#include "app/ai_dialog.hpp"
#include "app/ai_link.hpp"
#include "app/ask.hpp"
#include "app/config.hpp"
#include "app/links.hpp"
#include "app/main_window.hpp"
#include "app/reply_box.hpp"
#include "app/review_model.hpp"
#include "app/studio_widgets.hpp"
#include "app/style_picker.hpp"
#include "app/theme.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "service/briefs.hpp"
#include "service/stylecat.hpp"
#include "service/studio_service.hpp"
#include "service/wording.hpp"
#include "storage/asset_store.hpp"

namespace genko::app {

using core::Json;

namespace {

QDockWidget* studio_dock(QMainWindow* window, const QString& title, QWidget* panel, bool closable) {
    auto* dock = new QDockWidget(title, window);
    dock->setObjectName(title);
    auto* scroll = new QScrollArea;  // (a tall panel scrolls on a small screen instead of making the window taller)
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(panel);
    dock->setWidget(scroll);
    dock->setMinimumWidth(200);
    QDockWidget::DockWidgetFeatures features = QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable;
    if (closable) features |= QDockWidget::DockWidgetClosable;  // (Python's OCCASIONAL_PANELS: 資料)
    dock->setFeatures(features);
    dock->hide();  // (shown for a book made with agents only: never stacked in a plain book's window, even while it is made)
    return dock;
}

// The panel is in front (a panel behind another tab is moved out of the window).
bool in_front(const QMainWindow& window, const QDockWidget* dock) {
    return dock != nullptr && dock->isVisible() && (dock->isFloating() || window.rect().intersects(dock->geometry()));
}

bool raise_requests() {
    const QString value = settings()->value(QStringLiteral("ui/raise_requests"), QStringLiteral("true")).toString().toLower();
    return value == QLatin1String("1") || value == QLatin1String("true") || value == QLatin1String("yes");
}

}  // namespace

void MainWindow::build_studio_actions() {
    make("act_style", QStringLiteral("絵柄を選ぶ…"), [this] { pick_style(); }, {},
         QStringLiteral("マンガの絵柄カタログから、この原稿の絵柄を選びます（絵の依頼文と見本の参照画像になります）"));
    make("act_chat_approval", QStringLiteral("チャットでの承認…"), [this] { chat_approval(); }, {},
         QStringLiteral("Telegram などで AI に「承認します」と返すと、AI があなたの名前で承認を記録します（既定）。この原稿だけ止めることもできます"));
}

void MainWindow::build_studio_bar() {
    ai_button_ = new QPushButton(QStringLiteral("AI と作る"));
    ai_button_->setObjectName(QStringLiteral("aiButton"));
    theme::role_prop(ai_button_, "chip", true);  // (the same rounded look as the counts beside it)
    ai_button_->setToolTip(QStringLiteral("AI（Claude など）をつなぐ・この原稿で動いた AI を見る"));
    connect(ai_button_, &QPushButton::clicked, this, &MainWindow::open_ai_dialog);
    statusBar()->addPermanentWidget(ai_button_);
    ai_timer_.setInterval(30000);  // (the minutes since the AI last wrote)
    connect(&ai_timer_, &QTimer::timeout, this, &MainWindow::refresh_ai);
    ai_timer_.start();
    process_ = new ProcessBar;
    connect(process_, &ProcessBar::openBox, this, [this] { show_dock(QStringLiteral("承認箱")); });
    // (a reply draft of a book here that was broken, too long to keep or could not be written)
    connect(&Drafts::instance(), &Drafts::problem, this, [this](const QString& book, const QString& message) {
        if (closed_) return;
        const auto key_of = [](const Session& session) {
            return session.path() ? QString::fromStdString(core::path_to_utf8(*session.path()))
                                  : QStringLiteral("book:") + QString::fromStdString(session.document().book_id);
        };
        bool mine = session_ && key_of(*session_) == book;
        for (const Document& doc : documents_) mine = mine || (doc.session && key_of(*doc.session) == book);
        if (mine) flash(message, 8000, true);
    });
    statusBar()->addPermanentWidget(process_);
    studio_timer_.setSingleShot(true);
    studio_timer_.setInterval(250);  // (Python's _dock_timer: the side panels a little after an edit)
    connect(&studio_timer_, &QTimer::timeout, this, [this] { studio_follow(true); });
}

void MainWindow::build_studio_docks() {
    approvals_ = new ApprovalBox(this);
    panel_view_ = new PanelView(this);
    library_ = new Library(this);
    for (QWidget* widget : std::initializer_list<QWidget*>{approvals_, panel_view_}) {
        // (Python's changed → _reload_pages: the pages and the box follow the decision)
        if (auto* box = qobject_cast<ApprovalBox*>(widget)) connect(box, &ApprovalBox::changed, this, [this] { reload_pages(); });
        if (auto* view = qobject_cast<PanelView*>(widget)) connect(view, &PanelView::changed, this, [this] { reload_pages(); });
    }
    // the approval box on its own over the row of tabs (only while something waits for the person)
    box_dock_ = studio_dock(this, QStringLiteral("承認箱"), approvals_, false);
    addDockWidget(Qt::RightDockWidgetArea, box_dock_);
    splitDockWidget(box_dock_, pages_dock_, Qt::Vertical);
    view_menu_->addAction(box_dock_->toggleViewAction());
    panel_dock_ = studio_dock(this, QStringLiteral("コマの詳細"), panel_view_, false);
    addDockWidget(Qt::RightDockWidgetArea, panel_dock_);
    library_dock_ = studio_dock(this, QStringLiteral("資料"), library_, true);
    addDockWidget(Qt::RightDockWidgetArea, library_dock_);
    agent_docks_ = {box_dock_, panel_dock_, library_dock_};
    for (QDockWidget* dock : agent_docks_) {
        connect(dock, &QDockWidget::visibilityChanged, this, [this, dock](bool shown) {
            if (!shown || stale_docks_.erase(dock) == 0) return;
            if (dock == box_dock_) approvals_->refresh();
            if (dock == panel_dock_) refresh_studio_docks();
            if (dock == library_dock_) library_->refresh();
        });
    }
}

std::set<std::string> MainWindow::request_ids() const {
    std::set<std::string> ids;
    for (const auto& item : review_model::inbox(book())) ids.insert(item.ticket_id);
    return ids;
}

void MainWindow::agent_view(bool on) {
    if (agent_mode_ == on || box_dock_ == nullptr) return;
    agent_mode_ = on;
    for (QDockWidget* dock : agent_docks_) {
        dock->toggleViewAction()->setVisible(on);
        if (!on) {
            dock->hide();
        } else if (dock == panel_dock_) {
            dock->show();
        }
    }
    box_waiting_.reset();
    sync_box();
    if (process_ != nullptr) process_->setVisible(on);
}

void MainWindow::sync_box() {
    if (box_dock_ == nullptr) return;
    const bool waiting = agent_mode_.value_or(false) && !review_model::inbox(book()).empty();
    if (box_waiting_ == waiting) return;
    box_waiting_ = waiting;
    box_dock_->setVisible(waiting);
    if (waiting) {
        approvals_->refresh();
        if (isVisible()) QTimer::singleShot(0, this, &MainWindow::fit_box);
    }
}

void MainWindow::fit_box() {
    if (box_dock_ == nullptr || !box_dock_->isVisible() || box_dock_->isFloating()) return;
    const int most = height() / 2;
    const bool pictured = approvals_->preview->has_image() || approvals_->choices->isVisibleTo(approvals_);
    const int want = pictured ? std::max(320, most)
                              : std::min(std::max(approvals_->sizeHint().height(), approvals_->minimumSizeHint().height()) + 24, most);
    resizeDocks({box_dock_, pages_dock_}, {want, std::max(200, height() - want)}, Qt::Vertical);
}

void MainWindow::studio_follow(bool now) {
    if (process_ == nullptr || box_dock_ == nullptr) return;
    if (!now) {  // (an edit: the studio follows a moment later, as Python's _dock_timer — the pen is not kept waiting)
        studio_timer_.start();
        return;
    }
    studio_timer_.stop();
    agent_view(agent_book());  // (an agent may have started working on this book)
    process_->refresh(book());
    sync_box();
    known_requests_ = request_ids();
    refresh_studio_docks();
}

void MainWindow::refresh_studio_docks() {
    if (approvals_ == nullptr) return;
    // the panel shown: the one chosen on this page (Python's _sync_panel_view)
    const core::Page* page = current_page();
    if (panel_view_->frame_id && (page == nullptr || page->find_frame(*panel_view_->frame_id) == nullptr)) panel_view_->frame_id.reset();
    if (!panel_view_->frame_id && page != nullptr && page->selected_frame_id.is_string() &&
        page->find_frame(page->selected_frame_id.get<std::string>()) != nullptr) {
        panel_view_->frame_id = page->selected_frame_id.get<std::string>();
    }
    const std::pair<QDockWidget*, std::function<void()>> panels[] = {
        {box_dock_, [this] { approvals_->refresh(); }},
        {panel_dock_, [this] { panel_view_->refresh(); }},
        {library_dock_, [this] { library_->refresh(); }},
    };
    for (const auto& [dock, refresh] : panels) {
        // (the box hidden because nothing waits: emptied too, cheaply)
        if (in_front(*this, dock) || (dock == box_dock_ && !box_waiting_.value_or(false))) {
            stale_docks_.erase(dock);
            refresh();
        } else {
            stale_docks_.insert(dock);
        }
    }
}

bool MainWindow::apply_and_commit(const Json& ops) {
    if (!apply_ops(ops)) return false;
    session_->save_now();  // (Python's commit_now: written at once, so the agent sees the decision immediately)
    return true;
}

void MainWindow::open_ai_dialog() {
    AiDialog dialog(this, session_->path(), &ai_target_);
    ask::exec(&dialog);
    refresh_ai();
}

void MainWindow::refresh_ai() {
    if (ai_button_ == nullptr) return;
    const auto& path = session_->path();
    const auto [words, working] = ai_link::status_words(path);
    QString text = (working ? QStringLiteral("● ") : QString()) + words;
    QString tip = QStringLiteral("AI（Claude など）をつなぐ・この原稿で動いた AI を見る");
    // (C++, SPEC AI-01: the AI's last call in this book failed — said here too, its reason in the tooltip)
    bool failed = false;
    if (path) {
        if (const auto call = ai_link::last_call(*path)) {
            const auto& probe = ai_link::last_probe();
            const double tested = probe ? static_cast<double>(probe->at.toMSecsSinceEpoch()) / 1000.0 : -1;
            failed = !call->ok && call->at >= tested;
            if (failed) {
                tip += QStringLiteral("\n最後の呼び出しが失敗しました: %1 の %2 — %3").arg(ai_link::ai_name(call->actor), call->tool, call->error);
            }
        }
    }
    if (failed) text += QStringLiteral("・失敗");
    ai_button_->setText(text);
    ai_button_->setToolTip(tip);
    theme::role_prop(ai_button_, "quiet", !working && !failed);
    theme::role_prop(ai_button_, "danger", failed);
}

void MainWindow::chat_approval() {
    const bool on = service::chat_approval_open(book());
    const QString text = on ? QStringLiteral("いまは、チャットで AI に承認を伝えると、AI があなたの名前で承認を記録します（記録には AI の名前と、"
                                             "あなたが送った言葉が残ります）。\n\nこの原稿では止めますか？ 止めると、承認は承認箱でだけ行えます。")
                            : QStringLiteral("この原稿では、チャットでの承認を止めています。\n\nTelegram などのチャットで AI に「承認します」と返したとき、"
                                             "AI があなたの名前で記録できるように戻しますか？ 正式な書き出しはこれまでどおり Genko で行います。");
    if (!ask::question(this, QStringLiteral("チャットでの承認"), text)) return;
    apply_ops(Json::array({Json{{"op", "allow_chat_approval"}, {"on", !on}}}));
    flash(on ? QStringLiteral("この原稿ではチャットでの承認を止めました") : QStringLiteral("チャットでの承認を AI が記録できるようにしました"), 4000);
}

void MainWindow::notice_requests(const std::set<std::string>& before) {
    std::vector<review_model::InboxItem> fresh;
    for (auto& item : review_model::inbox(book())) {
        if (before.count(item.ticket_id) == 0) fresh.push_back(std::move(item));
    }
    if (fresh.empty()) return;
    const QString title = QString::fromStdString(fresh.back().title());
    const std::string newest = fresh.back().ticket_id;
    const QString more = fresh.size() > 1 ? QStringLiteral(" ほか %1 件").arg(fresh.size() - 1) : QString();
    const std::shared_ptr<int> tries = std::make_shared<int>(0);
    const std::shared_ptr<std::function<void()>> show = std::make_shared<std::function<void()>>();
    *show = [this, title, newest, more, tries, show] {
        // nothing moves under a pen that is still down: it waits for the pen to lift
        if (QApplication::mouseButtons() != Qt::NoButton && *tries < 40) {
            ++*tries;
            QTimer::singleShot(250, this, *show);
            return;
        }
        toast(this, QStringLiteral("承認の依頼が届きました: %1%2").arg(title, more), 6000);
        QApplication::alert(this);
        if (!raise_requests()) return;
        show_dock(QStringLiteral("承認箱"));
        if (!box_dock_->isFloating() && box_dock_->height() < 360) {  // (room enough to see the request)
            resizeDocks({box_dock_, pages_dock_}, {static_cast<int>(height() * 0.62), 80}, Qt::Vertical);
        }
        approvals_->refresh();
        // (C++, I13: a reply being written stays in front — the newest is chosen only when nothing is being written)
        if (!approvals_->reason->toPlainText().trimmed().isEmpty()) return;
        const auto& items = approvals_->items();
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (items[i].ticket_id == newest) approvals_->list->setCurrentRow(static_cast<int>(i));
        }
    };
    (*show)();
}

void MainWindow::open_link(const QString& link) {
    std::optional<std::string> style_id;
    try {
        style_id = service::stylecat::style_from_link(Json(link.toStdString()));
    } catch (const service::stylecat::CatalogError& error) {
        flash(QString::fromUtf8(error.what()), 6000, true);
        return;
    }
    if (!style_id) {
        flash(QStringLiteral("Genko の知らないリンクです: %1").arg(link), 6000, true);
        return;
    }
    links::come_forward(this);
    use_style(*style_id, true);
}

void MainWindow::open_link_when_ready(const QString& link) {
    if (session_->path()) {
        open_link(link);
    } else {
        pending_links_ << link;  // (the book is being read: show_document takes it)
    }
}

void MainWindow::pick_style() {
    for (;;) {  // (a "no" to the question goes back to the picker, not out of it)
        StylePicker dialog(this, service::briefs::catalog(book()), book().spec.expression);
        if (ask::exec(&dialog) != QDialog::Accepted) return;
        if (dialog.back_to_genko) {
            if (apply_ops(Json::array({Json{{"op", "set_style_catalog"}, {"catalog", nullptr}}}))) {
                flash(QStringLiteral("絵柄を Genko の言葉に戻しました"), 4000);
            }
            return;
        }
        if (!dialog.chosen) return;
        style_declined_ = false;
        if (use_style(*dialog.chosen, false) || !style_declined_) return;
    }
}

bool MainWindow::use_style(const std::string& style_id, bool asked) {
    namespace stylecat = service::stylecat;
    const auto& path = session_->path();
    if (!path) {
        flash(QStringLiteral("絵柄を選ぶ前に、原稿を保存します（ファイル → 別の場所に保存）"), 6000);
        return false;
    }
    const Json now = service::briefs::catalog(book());
    if (now.is_object() && now.value("id", Json()) == Json(style_id)) {  // (the style it has already: nothing to ask or change)
        const Json title = now.value("title", Json());
        flash(QStringLiteral("今の絵柄（%1）のままです").arg(QString::fromStdString(core::py_truthy(title) ? core::py_str(title) : style_id)), 4000);
        return true;
    }
    Json data;
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    try {
        data = stylecat::style(Json(style_id));
    } catch (const stylecat::CatalogError& error) {
        QGuiApplication::restoreOverrideCursor();
        flash(QString::fromUtf8(error.what()), 6000, true);
        return false;
    }
    QGuiApplication::restoreOverrideCursor();
    const auto joined = [](const Json& parts, const char* sep) {
        QStringList out;
        for (const Json& part : core::iterate(parts)) out << QString::fromStdString(core::py_str(part));
        return out.join(QString::fromUtf8(sep));
    };
    QString where = joined(stylecat::full_path(data), " › ");
    if (where.isEmpty()) where = QString::fromStdString(core::py_str(data.value("title", Json(style_id))));
    QString warn;
    if (data.value("expression", Json()) == Json("mono") && book().spec.expression == "color") {
        warn = QStringLiteral("\n\nこの絵柄は白黒用です。カラーの原稿では、絵の依頼に使われません。");
    }
    Json same = Json::array();
    try {
        same = stylecat::namesakes(core::py_truthy(data.value("id", Json())) ? data["id"] : Json(style_id), Json(core::py_str(data.value("title", Json("")))));
    } catch (const stylecat::CatalogError&) {
        same = Json::array();
    }
    bool ask_now = asked;
    if (core::py_truthy(same)) {
        ask_now = true;
        QStringList others;
        for (const Json& other : core::iterate(same)) others << joined(other.value("path", Json::array()), " › ");
        warn += QStringLiteral("\n\n同じ名前の絵柄がほかの分類にもあります: ") + others.join(QStringLiteral("、"));
    }
    const Json style = book().studio.is_object() ? book().studio.value("style", Json()) : Json();
    const Json locked = style.is_object() ? style.value("locked", Json()) : Json();
    if (core::py_truthy(locked)) {
        warn += QStringLiteral("\n\n絵柄は %1 ページの試しで固定されています。変えると、この後の絵の依頼が新しい絵柄になります。")
                    .arg(QString::fromStdString(core::py_str(locked.is_object() ? locked.value("page", Json()) : Json())));
    }
    if (ask_now || core::py_truthy(locked) || !warn.isEmpty()) {
        const QString title = book().title.empty() ? QStringLiteral("無題") : QString::fromStdString(book().title);
        if (!ask::question(this, QStringLiteral("絵柄を選ぶ"), QStringLiteral("「%1」の絵柄を「%2」にしますか？%3").arg(title, where, warn))) {
            style_declined_ = true;
            return false;
        }
    }
    Json kept;
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    try {
        session_->save_now();  // (Python's commit_now)
        const auto png = stylecat::sample_png(data);
        std::optional<std::string> asset;
        if (png) asset = storage::AssetStore(*path).put_bytes(*png, ".png");
        kept = stylecat::saved(data, asset);
    } catch (const std::exception& error) {
        QGuiApplication::restoreOverrideCursor();
        flash(QString::fromUtf8(error.what()), 6000, true);
        return false;
    }
    QGuiApplication::restoreOverrideCursor();
    if (!apply_ops(Json::array({Json{{"op", "set_style_catalog"}, {"catalog", kept}}}))) return false;
    QStringList sheets;
    for (const Json& c : core::iterate(book().bible.characters)) {
        if (!c.is_object() || !core::py_truthy(c.value("locked", Json()))) continue;
        const Json name = core::py_truthy(c.value("name", Json())) ? c["name"] : c.value("id", Json());
        sheets << QString::fromStdString(core::py_str(name));
    }
    const QString after = core::py_truthy(locked) && !sheets.isEmpty()
                              ? QStringLiteral("。承認済みの設定画（%1%2）は前の絵柄のままです").arg(sheets.mid(0, 3).join(QStringLiteral("、")), sheets.size() > 3 ? QStringLiteral(" ほか") : QString())
                              : QString();
    flash(QStringLiteral("絵柄を「%1」にしました（版 %2）。この後の絵の依頼に入ります%3").arg(where, QString::fromStdString(core::py_str(kept.value("version", Json()))), after), 8000);
    return true;
}

}  // namespace genko::app
