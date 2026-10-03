#pragma once

#include <QDialog>
#include <QListWidget>
#include <QWidget>

#include <memory>

#include "app/thumbs.hpp"

// The navigator (全体図) and the page overview (Python's genko/app/navigator.py). The navigator shows the whole page
// small, with a frame around the part seen in the canvas; clicking or dragging in it moves the view there. The page
// overview lays all pages out large enough to recognise, and opens the one double-clicked.

namespace genko::app {

class PageCanvas;

class Navigator : public QWidget {
    Q_OBJECT

public:
    explicit Navigator(PageCanvas* canvas, QWidget* parent = nullptr);
    // Where the page is drawn in the navigator (empty: no page).
    QRectF page_rect() const;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;

private:
    void go(const QPointF& pos);

    PageCanvas* canvas_ = nullptr;
};

class PageOverview : public QDialog {
    Q_OBJECT

public:
    // doc: the book; current: the page shown now (its number).
    PageOverview(QWidget* parent, DocPtr doc, int current);
    QListWidget* list() const { return list_; }
    ThumbMaker& maker() { return *maker_; }

signals:
    void pageChosen(int index);

private:
    QListWidget* list_ = nullptr;
    std::unique_ptr<ThumbMaker> maker_;
    DocPtr doc_;
};

}  // namespace genko::app
