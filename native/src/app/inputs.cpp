// Number fields and lists (Python's genko/app/inputs.py).

#include "app/inputs.hpp"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QPointer>
#include <QSlider>
#include <QTimer>
#include <QWidget>

namespace genko::app::inputs {

namespace {

bool field_kind(QObject* object) {
    return qobject_cast<QAbstractSpinBox*>(object) != nullptr || qobject_cast<QComboBox*>(object) != nullptr ||
           qobject_cast<QSlider*>(object) != nullptr;
}

class Inputs : public QObject {
public:
    using QObject::QObject;

    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::FocusIn) {
            if (auto* spin = qobject_cast<QAbstractSpinBox*>(watched)) {
                QPointer<QAbstractSpinBox> alive(spin);
                QTimer::singleShot(0, spin, [alive] {
                    if (alive) alive->selectAll();
                });
            }
        } else if (event->type() == QEvent::Wheel) {
            auto* widget = qobject_cast<QWidget*>(watched);
            if (widget != nullptr && !widget->hasFocus()) {
                event->ignore();  // (the wheel goes on to what holds the field: the dialog scrolls)
                return true;
            }
        }
        return false;
    }
};

Inputs* filter = nullptr;
bool pending = false;

void scan() {
    pending = false;
    QWidget* focus = QApplication::focusWidget();
    if (focus != nullptr) watch(focus->window());
    QWidget* active = QApplication::activeWindow();
    if (active != nullptr && !(focus != nullptr && active == focus->window())) watch(active);
}

void scan_soon() {
    // (after the event that moved the focus has finished: a window may be closing, half taken apart, as it moves)
    if (!pending) {
        pending = true;
        QTimer::singleShot(0, filter, &scan);
    }
}

}  // namespace

void watch(QWidget* widget) {
    if (filter == nullptr || widget == nullptr) return;
    QList<QWidget*> found;
    if (field_kind(widget)) found << widget;
    for (QWidget* child : widget->findChildren<QWidget*>()) {
        if (field_kind(child)) found << child;
    }
    for (QWidget* field : found) {
        if (!field->property("genko_inputs").toBool()) {
            field->setProperty("genko_inputs", true);
            field->installEventFilter(filter);
        }
    }
}

void install() {
    auto* app = qobject_cast<QApplication*>(QCoreApplication::instance());
    if (app == nullptr || filter != nullptr) return;
    filter = new Inputs(app);
    QObject::connect(app, &QApplication::focusChanged, filter, [](QWidget*, QWidget*) { scan_soon(); });
    QObject::connect(app, &QGuiApplication::focusWindowChanged, filter, [](QWindow*) { scan_soon(); });
    for (QWidget* top : QApplication::topLevelWidgets()) watch(top);
}

}  // namespace genko::app::inputs
