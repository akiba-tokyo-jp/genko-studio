#pragma once

#include <QProxyStyle>
#include <QString>

#include <functional>
#include <optional>

class QApplication;
class QWidget;

// Frosted glass for what floats over the page (Python's genko/app/glass.py: menus, tooltips, the round menu, the
// command search), made by the system itself: Acrylic on Windows 11. Nothing is imitated: elsewhere, on an older
// Windows, or when the person has turned transparency off in the system, the floating parts keep their plain face.
// (Python also asked a Mac for the vibrancy of its menus; this build has no Mac.)
//
// The parts ask for the glass by class; the app's style (a Fusion that also does this, only where the glass exists)
// makes them see-through as they are readied, before their window is made, and a watcher on each such window asks the
// system for the glass when it is first shown (not an application event filter: that would run for every event of
// every widget and slow drawing).

namespace genko::app::glass {

// "windows" or "" (no system glass here, or turned off by the person).
QString available();
// The floating parts that get the glass: menus, tooltips, and widgets with the property glass_wanted.
bool wants(QWidget* widget);
// Ask the system for the glass behind this (already see-through) window; its face then lets it through (property
// "glass"). False where there is none.
bool apply(QWidget* widget);

// Fusion, with the floating parts made see-through as each is readied (polished).
class GlassStyle : public QProxyStyle {
    Q_OBJECT

public:
    GlassStyle();
    void polish(QWidget* widget) override;
    using QProxyStyle::polish;

private:
    QObject* shown_ = nullptr;
};

// The app's style: Fusion, which also readies the glass where the system has it.
QStyle* style();
// Once, and only where the system has the glass (elsewhere the style is plain Fusion).
bool install(QApplication* app = nullptr);

// (tests) what available() says and what apply() does, instead of asking the system
void set_available_for_tests(std::optional<QString> kind);
void set_apply_for_tests(std::function<bool(QWidget*)> apply);

}  // namespace genko::app::glass
