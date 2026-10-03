#include "app/pages_panel.hpp"

#include <QCoreApplication>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QMenu>
#include <QPixmap>
#include <QScrollBar>
#include <QTimer>

#include <algorithm>

#include "app/ask.hpp"
#include "core/covers.hpp"
#include "core/pyconv.hpp"

namespace genko::app {

namespace {

QString number(const core::Num& n) { return QString::fromStdString(n.repr()); }

}  // namespace

QString page_text(const core::Page& page) {
    static const std::map<std::string, QString> kLabels = {
        {"front", QStringLiteral("表紙")}, {"back", QStringLiteral("裏表紙")},
        {"jacket", QStringLiteral("カバー（表紙・背・裏表紙・袖）")}, {"obi", QStringLiteral("帯（表・背・裏・袖、低い紙）")}};
    QString who;
    if (const auto it = page.extra.find("assignee"); it != page.extra.end() && it->is_string()) who = QString::fromStdString(it->get<std::string>());
    if (const core::Json* cover = core::cover_of(page)) {
        const auto label = kLabels.find((*cover)["kind"].get<std::string>());
        return (label != kLabels.end() ? label->second : QString()) + (who.isEmpty() ? QString() : QStringLiteral("\n担当 %1").arg(who));
    }
    QString name;
    if (page.name_ok) {
        name = QStringLiteral("ネーム ✓");
    } else if (page.plan && page.plan->is_object() && page.plan->contains("name") && core::py_truthy((*page.plan)["name"])) {
        name = QStringLiteral("ネーム 承認待ち");
    } else {
        name = QStringLiteral("ネーム");
    }
    const QString art = page.art_ok ? QStringLiteral(" / 作画 ✓") : (page.name_ok ? QStringLiteral(" / 作画中") : QString());
    const QString done = page.stage == "finish" ? QStringLiteral(" / 仕上げ ✓") : QString();
    QString extra;
    if (page.spread_with && page.spread_with->truthy()) {
        const bool first = page.index < *page.spread_with;
        extra += QStringLiteral("\n見開き %1–%2").arg(number(first ? page.index : *page.spread_with), number(first ? *page.spread_with : page.index));
    }
    if (!page.numero) extra += QStringLiteral("\nノンブルなし");
    if (!who.isEmpty()) extra += QStringLiteral("\n担当 %1").arg(who);
    return QStringLiteral("%1 ページ\n%2%3%4%5").arg(number(page.index), name, art, done, extra);
}

PageList::PageList(QWidget* parent) : QListWidget(parent), maker_(std::make_unique<ThumbMaker>()) {
    setIconSize(QSize(static_cast<int>(std::nearbyint(kThumbHeight * 0.72)), kThumbHeight));
    setDragDropMode(QAbstractItemView::InternalMove);
    setDefaultDropAction(Qt::MoveAction);
    setSpacing(2);
    setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this, &QWidget::customContextMenuRequested, this, &PageList::show_menu);
    connect(maker_.get(), &ThumbMaker::done, this, [this](const QString& id, int, const QImage& image, bool) {
        const std::string key = id.toStdString();
        const auto asked = asked_.find(key);
        if (asked == asked_.end()) return;
        const core::Page* page = asked->second;
        asked_.erase(asked);
        // (a picture of a page that has changed since it was asked for is not shown)
        bool current = false;
        if (doc_) {
            for (const auto& p : doc_->pages) {
                if (p->id == key && p.get() == page) current = true;
            }
        }
        if (!current) {
            request_visible();
            return;
        }
        QIcon icon;
        const QPixmap pixmap = QPixmap::fromImage(image);
        for (const auto mode : {QIcon::Normal, QIcon::Selected, QIcon::Active}) icon.addPixmap(pixmap, mode);  // (never tinted)
        pictures_[key] = {page, icon};
        for (int row = 0; row < count(); ++row) {
            if (item(row)->data(Qt::UserRole + 1).toString() == id) item(row)->setIcon(icon);
        }
        request_visible();
    });
}

QIcon PageList::blank() const {
    QPixmap pixmap(iconSize());
    pixmap.fill(QColor(QStringLiteral("#f4f4f4")));
    return QIcon(pixmap);
}

void PageList::fill(DocPtr doc, int current) {
    doc_ = std::move(doc);
    blockSignals(true);
    clear();
    if (doc_) {
        for (const auto& page : doc_->pages) {
            auto* item = new QListWidgetItem(page_text(*page));
            item->setData(Qt::UserRole, QVariant::fromValue(static_cast<qlonglong>(core::py_int(page->index.json()))));
            item->setData(Qt::UserRole + 1, QString::fromStdString(page->id));
            const auto found = pictures_.find(page->id);
            item->setIcon(found != pictures_.end() ? found->second.second : blank());
            addItem(item);
        }
        setCurrentRow(std::min(std::max(current, 0), count() - 1));
    }
    blockSignals(false);
    QTimer::singleShot(0, this, &PageList::request_visible);
}

bool PageList::has_picture(int row) const {
    if (!doc_ || row < 0 || row >= static_cast<int>(doc_->pages.size())) return false;
    const auto& page = doc_->pages[static_cast<std::size_t>(row)];
    const auto found = pictures_.find(page->id);
    return found != pictures_.end() && found->second.first == page.get();
}

void PageList::request_visible() {
    if (!doc_) return;
    const QRect seen = viewport()->rect();
    for (int row = 0; row < count() && row < static_cast<int>(doc_->pages.size()); ++row) {
        if (!visualItemRect(item(row)).intersects(seen)) continue;
        const auto& page = doc_->pages[static_cast<std::size_t>(row)];
        if (has_picture(row)) continue;
        if (const auto asked = asked_.find(page->id); asked != asked_.end() && asked->second == page.get()) continue;
        asked_[page->id] = page.get();
        maker_->request(doc_, static_cast<std::size_t>(row), kThumbHeight, page->name_ok ? "proof" : "name");
    }
}

bool PageList::wait_pictures(int ms) {
    QElapsedTimer clock;
    clock.start();
    request_visible();
    for (;;) {
        bool all = true;
        const QRect seen = viewport()->rect();
        for (int row = 0; row < count(); ++row) {
            if (visualItemRect(item(row)).intersects(seen) && !has_picture(row)) all = false;
        }
        if (all) return true;
        if (clock.elapsed() > ms) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
}

void PageList::resizeEvent(QResizeEvent* event) {
    QListWidget::resizeEvent(event);
    QTimer::singleShot(0, this, &PageList::request_visible);
}

void PageList::scrollContentsBy(int dx, int dy) {
    QListWidget::scrollContentsBy(dx, dy);
    request_visible();
}

void PageList::dropEvent(QDropEvent* event) {
    std::vector<int> before;
    for (int row = 0; row < count(); ++row) before.push_back(item(row)->data(Qt::UserRole).toInt());
    const int moving = currentItem() != nullptr ? currentItem()->data(Qt::UserRole).toInt() : -1;
    QListWidget::dropEvent(event);
    std::vector<int> order;
    for (int row = 0; row < count(); ++row) order.push_back(item(row)->data(Qt::UserRole).toInt());
    std::vector<int> a = before;
    std::vector<int> b = order;
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    if (order != before && a == b) {
        emit reorderRequested(order, moving);
    } else {
        fill(doc_, currentRow());
    }
}

void PageList::show_menu(const QPoint& pos) {
    QListWidgetItem* at = itemAt(pos);
    if (at == nullptr) return;
    const int index = at->data(Qt::UserRole).toInt();
    QMenu menu(this);
    menu.addAction(QStringLiteral("この後ろにページを追加"), this, [this, index] { emit addAfterRequested(index); });
    menu.addAction(QStringLiteral("このページを複製"), this, [this, index] { emit duplicateRequested(index); });
    menu.addSeparator();
    const auto move = [this](int page, int delta) {
        std::vector<int> order;
        for (const auto& p : doc_->pages) order.push_back(static_cast<int>(core::py_int(p->index.json())));
        const auto it = std::find(order.begin(), order.end(), page);
        if (it == order.end()) return;
        const int i = static_cast<int>(it - order.begin());
        const int j = std::max(0, std::min(static_cast<int>(order.size()) - 1, i + delta));
        if (i == j) return;
        order.erase(order.begin() + i);
        order.insert(order.begin() + j, page);
        emit reorderRequested(order, page);
    };
    menu.addAction(QStringLiteral("前へ移す"), this, [move, index] { move(index, -1); });
    menu.addAction(QStringLiteral("後ろへ移す"), this, [move, index] { move(index, 1); });
    menu.addSeparator();
    menu.addAction(QStringLiteral("このページを消す…"), this, [this, index] {
        if (ask::question(this, QStringLiteral("Genko"), QStringLiteral("%1 ページを消しますか？（元に戻す で取り消せます）").arg(index))) {
            emit deleteRequested(index);
        }
    });
    menu.exec(mapToGlobal(pos));
}

}  // namespace genko::app
