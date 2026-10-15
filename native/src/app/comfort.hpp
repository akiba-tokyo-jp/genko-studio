#pragma once

#include <QObject>
#include <QPointer>
#include <QSize>
#include <QTimer>
#include <QWidget>

#include <functional>
#include <utility>
#include <vector>

class QAction;
class QApplication;
class QMainWindow;
class QVariantAnimation;

// Comfort over long sessions (Python's genko/app/comfort.py, UI-B): the page alone with one key (the panels come back
// at the window's edge), a round menu at the pen, a reminder to rest the eyes (off unless chosen), fewer moving things
// on the screen, and the letters' size changed at once. (The layouts of the stages of the work are the window's:
// main_window_actions.cpp; the quiet note over the canvas is studio_widgets' toast.)

namespace genko::app {

class MainWindow;

// --- the page alone (Tab) ------------------------------------------------------------------------------------------

// Hide every panel and bar but the menus and the status line; bring a side back while the cursor is at it.
class CanvasOnly : public QObject {
    Q_OBJECT

public:
    static constexpr int kEdgePx = 8;  // (the cursor this close to the window's side brings that side's panels back)

    explicit CanvasOnly(MainWindow* window);

    bool on() const { return on_; }
    bool toggle();
    void enter();
    void leave();
    // The side shown for a moment (left | right; empty: none), and the cursor's place (window px) looked at (tests: as
    // the watch does every 120 ms).
    const QString& peek() const { return peek_; }
    void watch_at(const QPoint& pos);

private:
    std::vector<QWidget*> pieces() const;
    QString side_of(QWidget* widget) const;
    void watch();
    void show_side(const QString& side);

    MainWindow* window_;
    bool on_ = false;
    std::vector<std::pair<QPointer<QWidget>, bool>> saved_;
    QString peek_;
    QTimer timer_;
};

// --- the round menu at the pen ---------------------------------------------------------------------------------------

// The tools whose right click opens the round menu (Shift+right click: the usual menu).
bool radial_tool(const QString& tool);
// 描く道具の右クリックで円形のメニュー (環境設定 ui/radial; on unless turned off).
bool radial_on();
// The round menu's commands: the drawing tools and the few moves made most often.
std::vector<QAction*> radial_actions(MainWindow* window);

// Up to eight commands around the cursor: point and click (or let go of the right button) to choose.
class RadialMenu : public QWidget {
    Q_OBJECT

public:
    static constexpr int kRadius = 78;
    static constexpr int kButton = 26;

    RadialMenu(QWidget* window, const std::vector<QAction*>& actions, const QPoint& centre);

    const std::vector<QAction*>& commands() const { return actions_; }
    // How far it has opened (0 … 1: it opens from the pen outward unless motion is reduced), and its opening.
    double grow() const { return grow_; }
    QVariantAnimation* opening() const { return opening_; }
    int hover() const { return hover_; }
    // The command at a point of the menu (-1: the middle, which closes it).
    int at(const QPointF& pos) const;
    void choose(int index);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    QPointF centre() const;
    QPointF spot(int i) const;

    std::vector<QAction*> actions_;
    int hover_ = -1;
    double grow_ = 1.0;
    QVariantAnimation* opening_ = nullptr;
    double opened_s_ = 0.0;
};

// --- resting the eyes ------------------------------------------------------------------------------------------------

// When the person last pressed, drew or typed (seconds on a steady clock). One watcher for the whole application, on
// only while the rest reminder is (an app-wide watcher runs for every event of every widget, which slows drawing).
double last_input();
void stop_input();
bool input_watched();
// (tests: as if the person did something now)
void touch_input();
double monotonic_seconds();

// Counts the time actually spent working (input within the last two minutes) and, after the chosen minutes, says so
// quietly. Off (0) unless the person chooses a length in the preferences (ui/rest_minutes).
class RestReminder : public QObject {
    Q_OBJECT

public:
    static constexpr double kIdleS = 120;
    static constexpr double kResetIdleS = 300;

    explicit RestReminder(MainWindow* window);

    int minutes() const;
    double worked = 0.0;
    // One tick of the count (every 15 s).
    void count();
    void remind();
    // (tests) what remind() would show: replaced, nothing is shown
    std::function<void()> on_remind;

private:
    MainWindow* window_;
    QTimer tick_;
};

// --- fewer moving things; the letters' size at once -------------------------------------------------------------------

// The computer's own "fewer animations" setting (Windows: animations off; elsewhere Qt does not tell it: no).
bool system_reduces_motion();
// Menus, lists and tooltips open without sliding or fading when the person asks for fewer moving things.
void apply_motion(QApplication* app = nullptr);
// The screen's letters at the size chosen in 環境設定 (ui/font_pt; 0: the system's), now (every panel follows).
void apply_font(QApplication* app = nullptr);
// A panel coming into sight fades in (and rises a few pixels when asked) instead of popping; nothing moves when the
// person asks for fewer moving things. The effect is taken off when done, so drawing stays as fast.
void fade_in(QWidget* widget, int ms = 160, int rise = 0);
QSize icon_size();

}  // namespace genko::app
