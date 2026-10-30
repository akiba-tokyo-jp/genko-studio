// A pane beside the one in front (PC5 H-c): see pane_view.hpp.

#include "app/pane_view.hpp"

#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTabletEvent>

#include "app/session.hpp"

namespace genko::app {

PaneView::PaneView(QWidget* parent) : PageCanvas(parent) {
    setObjectName(QStringLiteral("paneView"));
    set_view_only(true);
    setMinimumSize(24, 24);  // (six or sixteen of them share the middle of the window)
    setFocusPolicy(Qt::ClickFocus);
    show_guides = true;
    later_.setSingleShot(true);
    later_.setInterval(kLaterMs);
    connect(&later_, &QTimer::timeout, this, &PaneView::refresh);
}

void PaneView::show_tab(std::uint64_t tab, std::shared_ptr<Session> session, Source source, const std::optional<ViewState>& view) {
    tab_ = tab;
    if (follows_) disconnect(follows_);
    session_ = std::move(session);
    source_ = std::move(source);
    later_.stop();
    // (its book changed — by the pane in front, by an agent —: drawn again once the changes stop coming, 節 H 4.5)
    if (session_) follows_ = connect(session_.get(), &Session::changed, this, [this](const BookChange&) { later_.start(); });
    refresh();
    if (view) {
        set_view_state(*view);
    } else {
        fit_page();
    }
}

void PaneView::refresh() {
    later_.stop();
    const std::optional<Shown> shown = source_ ? source_() : std::nullopt;
    if (!shown || !shown->doc) {
        clear_page();
        return;
    }
    set_render_mode(shown->mode);
    set_page(shown->doc, shown->index);
}

// --- input: no tool; a click brings it to the front ----------------------------------------------------------------

void PaneView::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::MiddleButton) {  // (the middle button drags its view, as the canvas's)
        middle_ = true;
        PageCanvas::mousePressEvent(event);
        return;
    }
    if (event->button() == Qt::LeftButton && (space_ || (space_down && space_down()))) {
        // (Space+drag: its view, as the canvas's hand — 節 H 5.3; the canvas pans on the middle button's way)
        middle_ = true;
        QMouseEvent pan(event->type(), event->position(), event->scenePosition(), event->globalPosition(), Qt::MiddleButton, Qt::MiddleButton,
                        event->modifiers() & ~Qt::ShiftModifier, event->pointingDevice());
        PageCanvas::mousePressEvent(&pan);
        event->accept();
        return;
    }
    event->accept();
    if (event->button() == Qt::LeftButton) emit activated();
}

void PaneView::mouseMoveEvent(QMouseEvent* event) {
    if (middle_) PageCanvas::mouseMoveEvent(event);
    event->accept();
}

void PaneView::mouseReleaseEvent(QMouseEvent* event) {
    if (middle_ && (event->button() == Qt::MiddleButton || event->button() == Qt::LeftButton)) {
        middle_ = false;
        QMouseEvent pan(event->type(), event->position(), event->scenePosition(), event->globalPosition(), Qt::MiddleButton, Qt::NoButton,
                        event->modifiers(), event->pointingDevice());
        PageCanvas::mouseReleaseEvent(&pan);  // (the pan let go: the canvas's middle button's way)
    }
    event->accept();
}

void PaneView::mouseDoubleClickEvent(QMouseEvent* event) { event->accept(); }

void PaneView::tabletEvent(QTabletEvent* event) {
    event->accept();  // (the pen's lines never land here; its touch brings the pane to the front)
    if (event->type() == QEvent::TabletPress && event->button() == Qt::LeftButton) emit activated();
}

void PaneView::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {  // (Space alone: no tool's key here)
        space_ = true;
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void PaneView::keyReleaseEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        space_ = false;
        event->accept();
        return;
    }
    QWidget::keyReleaseEvent(event);
}

void PaneView::contextMenuEvent(QContextMenuEvent* event) { event->accept(); }

}  // namespace genko::app
