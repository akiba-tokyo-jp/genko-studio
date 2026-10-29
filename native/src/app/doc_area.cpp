// The middle of the window (PC5 H-c): see doc_area.hpp.

#include "app/doc_area.hpp"

#include <QPainter>
#include <QPaintEvent>
#include <QSplitter>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <utility>

#include "app/canvas.hpp"
#include "app/pane_view.hpp"
#include "app/theme.hpp"

namespace genko::app {

namespace {

// A cell: a thin frame (the accent around the one in front, as Photoshop marks the active window) around its page.
class Cell : public QWidget {
public:
    Cell() {
        setObjectName(QStringLiteral("paneCell"));
        auto* box = new QVBoxLayout(this);
        box->setContentsMargins(2, 2, 2, 2);
        box->setSpacing(0);
        setMinimumSize(28, 28);
    }
    bool front = false;

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        const theme::Tokens t = theme::tokens();
        painter.fillRect(rect(), front ? QColor(t.accent) : QColor(t.border));
    }
};

// (rows of slots, each row a line of cells: a layout's places, row by row; threeUpStacked is its own)
std::vector<int> rows_of(const QString& layout, int books) {
    const int n = std::clamp(books, 0, DocArea::kMostPanes);
    if (layout == QLatin1String("tileAllVertically")) return {n};
    if (layout == QLatin1String("tileAllHorizontally")) return std::vector<int>(static_cast<std::size_t>(n), 1);
    if (layout == QLatin1String("twoUpVertical")) return {2};
    if (layout == QLatin1String("twoUpHorizontal")) return {1, 1};
    if (layout == QLatin1String("threeUpVertical")) return {3};
    if (layout == QLatin1String("threeUpHorizontal")) return {1, 1, 1};
    if (layout == QLatin1String("fourUp")) return {2, 2};
    if (layout == QLatin1String("sixUp")) return {3, 3};
    if (layout == QLatin1String("tile")) {  // (as square as can be: PhotoCraft's ceil(√n) columns)
        const int columns = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n))));
        const int rows = (n + columns - 1) / columns;
        return std::vector<int>(static_cast<std::size_t>(rows), columns);
    }
    return {};
}

int slots_of(const QString& layout, int books) {
    if (layout == QLatin1String("threeUpStacked")) return 3;
    int total = 0;
    for (int row : rows_of(layout, books)) total += row;
    return total;
}

QSplitter* splitter(Qt::Orientation way) {
    auto* s = new QSplitter(way);
    s->setChildrenCollapsible(false);
    s->setHandleWidth(4);
    return s;
}

void even(QSplitter* s) {
    for (int i = 0; i < s->count(); ++i) s->setStretchFactor(i, 1);
    s->setSizes(QList<int>(s->count(), 1000));
}

}  // namespace

DocArea::DocArea(PageCanvas* front, QWidget* parent) : QWidget(parent), canvas_(front), canvas_least_(front->minimumSize()) {
    setObjectName(QStringLiteral("docArea"));
    panes_pool_.setMaxThreadCount(kPaneThreads);
    root_ = new QVBoxLayout(this);
    root_->setContentsMargins(0, 0, 0, 0);
    root_->setSpacing(0);
    root_->addWidget(canvas_, 1);  // (the tabs' way: the canvas alone, as the window always had it)
}

const QStringList& DocArea::layouts() {
    static const QStringList all{QStringLiteral("tileAllVertically"), QStringLiteral("tileAllHorizontally"), QStringLiteral("twoUpVertical"),
                                 QStringLiteral("twoUpHorizontal"),   QStringLiteral("threeUpVertical"),     QStringLiteral("threeUpHorizontal"),
                                 QStringLiteral("threeUpStacked"),    QStringLiteral("fourUp"),              QStringLiteral("sixUp"),
                                 QStringLiteral("tile")};
    return all;
}

int DocArea::capacity(const QString& layout, int books) {
    if (books < 2 || !layouts().contains(layout)) return 0;
    return std::min({slots_of(layout, books), books, kMostPanes});
}

std::vector<QRectF> DocArea::cells(const QString& layout, int books) {
    const int n = capacity(layout, books);
    std::vector<QRectF> out;
    if (n == 0) return out;
    if (layout == QLatin1String("threeUpStacked")) {
        out = {QRectF(0, 0, 0.5, 1), QRectF(0.5, 0, 0.5, 0.5), QRectF(0.5, 0.5, 0.5, 0.5)};
    } else {
        const std::vector<int> rows = rows_of(layout, books);
        const double h = 1.0 / static_cast<double>(rows.size());
        for (std::size_t r = 0; r < rows.size(); ++r) {
            // (every row as many columns as the widest: a row short of cells leaves the rest empty, as PhotoCraft's grid)
            const double w = 1.0 / *std::max_element(rows.begin(), rows.end());
            for (int c = 0; c < rows[r]; ++c) out.emplace_back(c * w, static_cast<double>(r) * h, w, h);
        }
    }
    out.resize(static_cast<std::size_t>(n));
    return out;
}

int DocArea::cell_of(std::uint64_t tab) const {
    const auto it = std::find(tabs_.begin(), tabs_.end(), tab);
    return it == tabs_.end() ? -1 : static_cast<int>(it - tabs_.begin());
}

PaneView* DocArea::pane(int cell) const {
    if (cell < 0 || cell >= static_cast<int>(contents_.size())) return nullptr;
    return qobject_cast<PaneView*>(contents_[static_cast<std::size_t>(cell)]);
}

std::vector<PaneView*> DocArea::panes() const {
    std::vector<PaneView*> out;
    for (QWidget* w : contents_) {
        if (auto* p = qobject_cast<PaneView*>(w)) out.push_back(p);
    }
    return out;
}

QWidget* DocArea::cell(int index) const {
    return index >= 0 && index < static_cast<int>(cells_.size()) ? cells_[static_cast<std::size_t>(index)] : nullptr;
}

QWidget* DocArea::take(int cell) {
    QWidget* w = contents_[static_cast<std::size_t>(cell)];
    cells_[static_cast<std::size_t>(cell)]->layout()->removeWidget(w);
    contents_[static_cast<std::size_t>(cell)] = nullptr;
    return w;
}

void DocArea::put(int cell, QWidget* content) {
    QWidget* frame = cells_[static_cast<std::size_t>(cell)];
    frame->layout()->addWidget(content);  // (taken in: a canvas moved within the window, its card's context kept — canvas_gpu)
    content->show();
    contents_[static_cast<std::size_t>(cell)] = content;
    frame->layout()->activate();  // (its size now: the view the window gives it fits this cell)
}

void DocArea::clear() {
    canvas_->hide();
    canvas_->setParent(this);
    for (QWidget* w : contents_) {
        if (auto* p = qobject_cast<PaneView*>(w)) {
            p->hide();
            p->setParent(this);
            spare_.push_back(p);
        }
    }
    contents_.clear();
    cells_.clear();
    tabs_.clear();
    if (tree_ != nullptr) {
        root_->removeWidget(tree_);
        delete tree_;
        tree_ = nullptr;
    }
    root_->removeWidget(canvas_);
}

void DocArea::arrange(const QString& layout, const std::vector<std::uint64_t>& tabs, int front) {
    const bool had_focus = canvas_->hasFocus();
    clear();
    const int n = std::min(static_cast<int>(tabs.size()), capacity(layout, static_cast<int>(tabs.size())));
    if (n < 2 || layout == QLatin1String("tabs")) {
        layout_ = QStringLiteral("tabs");
        front_ = -1;
        for (PaneView* p : std::exchange(spare_, {})) delete p;
        canvas_->setMinimumSize(canvas_least_);
        root_->addWidget(canvas_, 1);
        canvas_->show();
        if (had_focus) canvas_->setFocus();
        return;
    }
    layout_ = layout;
    tabs_.assign(tabs.begin(), tabs.begin() + n);
    front_ = std::clamp(front, 0, n - 1);
    canvas_->setMinimumSize(24, 24);  // (the cells share the room; the canvas alone again: its own least size)
    const int room = std::max(n, slots_of(layout, n));
    std::vector<QWidget*> places;
    for (int i = 0; i < room; ++i) {
        if (i < n) {
            auto* frame = new Cell;
            cells_.push_back(frame);
            places.push_back(frame);
        } else {
            places.push_back(new QWidget);  // (an empty place: fewer books than the arrangement's cells)
        }
    }
    if (layout == QLatin1String("threeUpStacked")) {
        QSplitter* across = splitter(Qt::Horizontal);
        QSplitter* down = splitter(Qt::Vertical);
        across->addWidget(places[0]);
        down->addWidget(places[1]);
        down->addWidget(places[2]);
        across->addWidget(down);
        even(down);
        even(across);
        tree_ = across;
    } else {
        const std::vector<int> rows = rows_of(layout, n);
        std::vector<QWidget*> lines;
        std::size_t next = 0;
        const int widest = *std::max_element(rows.begin(), rows.end());
        for (int row : rows) {
            if (row == 1 && rows.size() > 1) {
                lines.push_back(places[next++]);
                continue;
            }
            QSplitter* line = splitter(Qt::Horizontal);
            for (int c = 0; c < widest; ++c) line->addWidget(c < row ? places[next++] : new QWidget);
            even(line);
            lines.push_back(line);
        }
        if (lines.size() == 1) {
            tree_ = lines.front();
        } else {
            QSplitter* down = splitter(Qt::Vertical);
            for (QWidget* line : lines) down->addWidget(line);
            even(down);
            tree_ = down;
        }
    }
    // (the middle asks the window for no more room than the canvas alone did: on a small screen the window stays within it —
    // SPEC UX-02 —, the cells sharing what there is)
    tree_->setMinimumSize(canvas_least_);
    root_->addWidget(tree_, 1);
    tree_->show();  // (now: its cells' sizes are there for the views the window gives them)
    contents_.assign(static_cast<std::size_t>(n), nullptr);
    for (int i = 0; i < n; ++i) {
        QWidget* content = canvas_;
        if (i != front_) {
            PaneView* p = nullptr;
            if (!spare_.empty()) {
                p = spare_.back();
                spare_.pop_back();
            } else {
                p = new PaneView;
                p->renderer().share_pool(&panes_pool_, 1);
                p->space_down = [canvas = canvas_] { return canvas->space_held(); };
                connect(p, &PaneView::activated, this, [this, p] {
                    const auto it = std::find(contents_.begin(), contents_.end(), static_cast<QWidget*>(p));
                    if (it != contents_.end()) emit paneActivated(static_cast<int>(it - contents_.begin()));
                });
            }
            content = p;
        }
        put(i, content);
    }
    for (PaneView* p : std::exchange(spare_, {})) delete p;
    root_->activate();
    mark();
    if (had_focus) canvas_->setFocus();
}

PaneView* DocArea::bring_front(int to) {
    if (front_ < 0 || to < 0 || to >= static_cast<int>(contents_.size()) || to == front_) return nullptr;
    const bool had_focus = canvas_->hasFocus();
    QWidget* moving = take(to);
    QWidget* canvas = take(front_);
    put(to, canvas);
    put(front_, moving);
    front_ = to;
    mark();
    canvas_->update();  // (drawn again in its new place — on the card too: its context may have been made again)
    if (had_focus || moving->hasFocus()) canvas_->setFocus();
    return qobject_cast<PaneView*>(moving);
}

void DocArea::set_tab(int cell, std::uint64_t tab) {
    if (cell >= 0 && cell < static_cast<int>(tabs_.size())) tabs_[static_cast<std::size_t>(cell)] = tab;
}

void DocArea::mark() {
    for (std::size_t i = 0; i < cells_.size(); ++i) {
        auto* frame = static_cast<Cell*>(cells_[i]);
        frame->front = static_cast<int>(i) == front_;
        frame->update();
    }
}

}  // namespace genko::app
