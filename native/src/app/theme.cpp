#include "app/theme.hpp"

#include <QAbstractButton>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QLabel>
#include <QLibraryInfo>
#include <QPainter>
#include <QPalette>
#include <QPointer>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>
#include <QTranslator>

#include <algorithm>
#include <vector>

#include "app/config.hpp"
#include "app/icons.hpp"

namespace genko::app::theme {

namespace {

QString shift(const QString& hex, int step) {
    QColor colour(hex);
    int h = 0, s = 0, l = 0, a = 0;
    colour.getHsl(&h, &s, &l, &a);
    colour.setHsl(h, s, std::clamp(l + step * 7, 0, 255), a);
    return colour.name();
}

QString rgba(const QString& hex, double alpha) {
    const QColor c(hex);
    return QStringLiteral("rgba(%1, %2, %3, %4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(alpha);
}

QString setting(const char* key, const char* fallback) {
    return settings()->value(QString::fromLatin1(key), QString::fromLatin1(fallback)).toString();
}

QString mode() {
    const QString value = setting("ui/theme", "system");
    return value == QLatin1String("light") || value == QLatin1String("dark") ? value : QStringLiteral("system");
}

int brightness() {
    bool ok = false;
    const int value = setting("ui/brightness", "0").toInt(&ok);
    return ok ? std::clamp(value, -2, 2) : 0;
}

bool system_dark() {
    if (auto* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
        return app->styleHints()->colorScheme() == Qt::ColorScheme::Dark;
    }
    return false;
}

// A small picture for the style sheet (the spin boxes' arrows, a tab's close button), kept as a file it can point to.
QString indicator(const char* name, const QString& colour) {
    const QString folder = QDir::tempPath() + QStringLiteral("/genko-ui");
    const QString path = folder + QStringLiteral("/%1_%2.png").arg(QString::fromLatin1(name), colour.mid(1));
    if (!QFile::exists(path)) {
        QDir().mkpath(folder);
        icons::icon(name, colour, colour).pixmap(32, 32).save(path);
    }
    return path;
}

std::vector<QPointer<QLabel>>& hints() {
    static std::vector<QPointer<QLabel>> list;
    return list;
}

void show_hint(QLabel* label, bool on) {
    label->setVisible(on);
    QWidget* parent = label->parentWidget();
    if (parent != nullptr && !on && !label->text().isEmpty() && parent->toolTip().isEmpty()) parent->setToolTip(label->text());
}

QString applied_key;  // (the look put on the application: re-styling every widget again is slow)

}  // namespace

Tokens light() {
    Tokens t;
    t.dark = false;
    t.window = "#d7d9dd", t.panel = "#f1f2f4", t.base = "#fafafb", t.raised = "#f7f8f9", t.hover = "#e5e7eb";
    t.border = "#c6c9cf", t.divider = "#dcdee2", t.text = "#1f2124", t.muted = "#62666d", t.faint = "#a3a7ad";
    t.accent = "#d9601f", t.accent_text = "#ffffff", t.accent_soft = "#f6dccb", t.selected = "#d9dde4";
    t.danger = "#c0392b", t.ok = "#2f7d4f", t.surround = "#9ea0a4";
    return t;
}

Tokens dark() {
    Tokens t;
    t.dark = true;
    t.window = "#18191b", t.panel = "#2a2b2f", t.base = "#212225", t.raised = "#34363a", t.hover = "#3b3d42";
    t.border = "#44464c", t.divider = "#393b40", t.text = "#dcdde0", t.muted = "#9a9ea6", t.faint = "#62656b";
    t.accent = "#e07a45", t.accent_text = "#1b1b1d", t.accent_soft = "#4a3429", t.selected = "#41444b";
    t.danger = "#e0685a", t.ok = "#6cc18f", t.surround = "#3a3b3e";
    return t;
}

Tokens tokens() {
    const QString which = mode();
    Tokens t = (which == QLatin1String("dark") || (which == QLatin1String("system") && system_dark())) ? dark() : light();
    if (const int step = brightness(); step != 0) {
        for (QString* grey : {&t.window, &t.panel, &t.base, &t.raised, &t.hover, &t.border, &t.divider, &t.surround, &t.selected}) {
            *grey = shift(*grey, step);
        }
    }
    const QString chosen = setting("ui/surround", "auto");
    bool ok = false;
    const int level = chosen.toInt(&ok);
    if (chosen != QLatin1String("auto") && ok) {
        const int v = std::clamp(level, 0, 255);
        t.surround = QColor(v, v, v).name();
    }
    return t;
}

bool show_hints() {
    const QString value = setting("ui/hints", "false").toLower();
    return value == QLatin1String("1") || value == QLatin1String("true") || value == QLatin1String("yes");
}

void set_hints(bool on) {
    settings()->setValue(QStringLiteral("ui/hints"), on ? QStringLiteral("true") : QStringLiteral("false"));
    auto& list = hints();
    std::erase_if(list, [](const QPointer<QLabel>& p) { return p.isNull(); });
    for (const auto& label : list) show_hint(label.data(), on);
}

QColor accent() { return QColor(tokens().accent); }

QColor surround() { return QColor(tokens().surround); }

QString style_sheet(const Tokens& t, double pt) {
    const QString big = QString::number(pt + 5) + QStringLiteral("pt");
    const QString head = QString::number(pt + 2) + QStringLiteral("pt");
    const QString small = QString::number(std::max(9.0, pt - 1)) + QStringLiteral("pt");
    QString s;
    s += QStringLiteral("QMainWindow, QDialog { background: %1; }\n").arg(t.window);
    s += QStringLiteral("QMainWindow::separator { background: %1; width: 6px; height: 6px; }\n").arg(t.window);
    s += QStringLiteral("QMainWindow::separator:hover { background: %1; }\n").arg(t.accent_soft);
    s += QStringLiteral("QDockWidget { color: %1; titlebar-close-icon: none; }\n").arg(t.text);
    s += QStringLiteral("QDockWidget::title { background: %1; padding: 5px 8px; border-bottom: 1px solid %2; text-align: left; font-weight: 500; }\n")
             .arg(t.panel, t.divider);
    s += QStringLiteral("QDockWidget > QWidget { background: %1; }\n").arg(t.panel);
    s += QStringLiteral("QMainWindow > QTabBar { background: %1; }\n").arg(t.panel);
    s += QStringLiteral("QWidget#panelBody { background: %1; }\n").arg(t.panel);
    s += QStringLiteral("QTabWidget::pane { border: none; border-top: 1px solid %1; background: %2; }\n").arg(t.divider, t.panel);
    s += QStringLiteral("QTabBar { qproperty-drawBase: 0; }\n");
    s += QStringLiteral("QTabBar::tab { background: transparent; color: %1; padding: 6px 6px; border: none; border-bottom: 2px solid transparent; }\n")
             .arg(t.muted);
    s += QStringLiteral("QTabBar::tab:hover { color: %1; }\n").arg(t.text);
    s += QStringLiteral("QTabBar::tab:selected { color: %1; border-bottom: 2px solid %2; font-weight: 500; }\n").arg(t.text, t.accent);
    s += QStringLiteral("QPushButton { background: %1; color: %2; border: 1px solid %3; border-radius: 6px; padding: 4px 8px; }\n")
             .arg(t.raised, t.text, t.border);
    s += QStringLiteral("QPushButton:hover { background: %1; }\n").arg(t.hover);
    s += QStringLiteral("QPushButton:checked { background: %1; border-color: %2; }\n").arg(t.selected, t.accent);
    s += QStringLiteral("QPushButton:pressed { background: %1; border-color: %2; }\n").arg(t.accent_soft, t.accent);
    s += QStringLiteral("QPushButton:disabled { color: %1; background: %2; border-color: %3; }\n").arg(t.faint, t.panel, t.divider);
    s += QStringLiteral("QPushButton[primary=\"true\"] { background: %1; color: %2; border-color: %1; font-weight: 600; }\n")
             .arg(t.accent, t.accent_text);
    s += QStringLiteral("QPushButton[primary=\"true\"]:hover { background: %1; }\n").arg(shift(t.accent, 1));
    s += QStringLiteral("QPushButton[primary=\"true\"]:disabled { background: %1; color: %2; border-color: %3; }\n")
             .arg(t.panel, t.faint, t.divider);
    s += QStringLiteral("QPushButton[chip=\"true\"] { background: transparent; border: 1px solid %1; border-radius: 9px; padding: 1px 4px; color: %2; }\n")
             .arg(t.divider, t.muted);
    s += QStringLiteral("QPushButton[chip=\"true\"]:hover { background: %1; color: %2; }\n").arg(t.hover, t.text);
    s += QStringLiteral("QPushButton[quiet=\"true\"] { color: %1; }\n").arg(t.muted);
    s += QStringLiteral("QToolButton { background: transparent; color: %1; border: 1px solid transparent; border-radius: 5px; padding: 3px; }\n")
             .arg(t.text);
    s += QStringLiteral("QToolButton:hover { background: %1; }\n").arg(t.hover);
    s += QStringLiteral("QToolButton:checked { background: %1; border-color: %1; }\n").arg(t.selected);
    s += QStringLiteral("QToolButton:pressed { background: %1; border-color: %2; }\n").arg(t.accent_soft, t.accent);
    s += QStringLiteral("QToolBar { background: %1; border: none; spacing: 1px; padding: 1px; }\n").arg(t.window);
    s += QStringLiteral("QToolBar#commands { padding: 2px 4px; spacing: 2px; }\n");
    s += QStringLiteral("QToolBar#commands QToolButton { padding: 4px; }\n");
    s += QStringLiteral("QToolBar::separator { background: %1; width: 1px; height: 1px; margin: 4px 6px; }\n").arg(t.divider);
    s += QStringLiteral("QLineEdit, QPlainTextEdit, QTextEdit, QSpinBox, QDoubleSpinBox, QComboBox { background: %1; color: %2; border: 1px solid %3; "
                        "border-radius: 6px; padding: 2px 3px; selection-background-color: %4; selection-color: %5; }\n")
             .arg(t.base, t.text, t.border, t.accent, t.accent_text);
    s += QStringLiteral("QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus { border-color: %1; }\n")
             .arg(t.accent);
    s += QStringLiteral("QComboBox::drop-down { border: none; width: 18px; }\n");
    s += QStringLiteral("QSpinBox::up-button, QDoubleSpinBox::up-button { subcontrol-origin: border; subcontrol-position: top right; width: 16px; "
                        "border: none; border-left: 1px solid %1; border-top-right-radius: 4px; background: transparent; }\n")
             .arg(t.divider);
    s += QStringLiteral("QSpinBox::down-button, QDoubleSpinBox::down-button { subcontrol-origin: border; subcontrol-position: bottom right; width: 16px; "
                        "border: none; border-left: 1px solid %1; border-bottom-right-radius: 4px; background: transparent; }\n")
             .arg(t.divider);
    s += QStringLiteral("QSpinBox::up-arrow, QDoubleSpinBox::up-arrow { image: url(%1); width: 10px; height: 10px; }\n")
             .arg(indicator("caret_up", t.muted));
    s += QStringLiteral("QSpinBox::down-arrow, QDoubleSpinBox::down-arrow { image: url(%1); width: 10px; height: 10px; }\n")
             .arg(indicator("caret_down", t.muted));
    s += QStringLiteral("QComboBox QAbstractItemView { background: %1; border: 1px solid %2; selection-background-color: %3; selection-color: %4; }\n")
             .arg(t.base, t.border, t.selected, t.text);
    s += QStringLiteral("QListWidget, QListView, QTreeWidget, QTreeView { background: %1; color: %2; border: 1px solid %3; border-radius: 6px; outline: none; }\n")
             .arg(t.base, t.text, t.divider);
    s += QStringLiteral("QListWidget::item { padding: 4px 8px; border-radius: 4px; }\n");
    s += QStringLiteral("QListWidget::item:hover { background: %1; }\n").arg(t.hover);
    s += QStringLiteral("QListWidget::item:selected { background: %1; color: %2; }\n").arg(t.selected, t.text);
    s += QStringLiteral("QScrollBar:vertical { background: transparent; width: 10px; margin: 0; }\n");
    s += QStringLiteral("QScrollBar:horizontal { background: transparent; height: 10px; margin: 0; }\n");
    s += QStringLiteral("QScrollBar::handle { background: %1; border-radius: 4px; min-height: 24px; min-width: 24px; margin: 1px; }\n").arg(t.border);
    s += QStringLiteral("QScrollBar::handle:hover { background: %1; }\n").arg(t.muted);
    s += QStringLiteral("QScrollBar::add-line, QScrollBar::sub-line, QScrollBar::add-page, QScrollBar::sub-page { background: none; width: 0; height: 0; }\n");
    s += QStringLiteral("QMenuBar { background: %1; color: %2; }\n").arg(t.window, t.text);
    s += QStringLiteral("QMenuBar::item:selected { background: %1; border-radius: 4px; }\n").arg(t.hover);
    s += QStringLiteral("QMenu { background: %1; color: %2; border: 1px solid %3; border-radius: 6px; padding: 4px; }\n").arg(t.panel, t.text, t.border);
    s += QStringLiteral("QMenu::item { padding: 4px 22px 4px 18px; border-radius: 4px; }\n");
    s += QStringLiteral("QMenu::item:selected { background: %1; color: %2; }\n").arg(t.selected, t.text);
    s += QStringLiteral("QMenu::item:disabled { color: %1; }\n").arg(t.faint);
    s += QStringLiteral("QMenu::separator { height: 1px; background: %1; margin: 4px 8px; }\n").arg(t.divider);
    s += QStringLiteral("QToolTip { background: %1; color: %2; border: 1px solid %3; border-radius: 4px; padding: 4px 6px; }\n")
             .arg(t.raised, t.text, t.border);
    s += QStringLiteral("QStatusBar { background: %1; color: %2; border-top: 1px solid %3; padding: 0 8px; }\n").arg(t.window, t.muted, t.divider);
    s += QStringLiteral("QStatusBar::item { border: none; }\n");
    s += QStringLiteral("QStatusBar QLabel { color: %1; }\n").arg(t.muted);
    s += QStringLiteral("QLabel[role=\"hint\"] { color: %1; }\n").arg(t.muted);
    s += QStringLiteral("QLabel[role=\"empty\"] { color: %1; padding: 16px; }\n").arg(t.muted);
    s += QStringLiteral("QLabel[role=\"error\"] { color: %1; }\n").arg(t.danger);
    s += QStringLiteral("QLabel[role=\"title\"] { font-weight: 700; font-size: %1; }\n").arg(big);
    s += QStringLiteral("QLabel[role=\"heading\"] { font-weight: 700; font-size: %1; }\n").arg(head);
    s += QStringLiteral("QLabel[role=\"section\"] { color: %1; font-weight: 500; font-size: %2; padding-top: 8px; padding-bottom: 2px; }\n")
             .arg(t.muted, small);
    s += QStringLiteral("QLabel[role=\"label\"] { color: %1; }\n").arg(t.muted);
    s += QStringLiteral("QLabel[role=\"caption\"] { color: %1; font-size: %2; }\n").arg(t.muted, small);
    s += QStringLiteral("QLabel[role=\"badge\"] { border-radius: 8px; padding: 1px 8px; background: %1; color: %2; }\n").arg(t.hover, t.text);
    s += QStringLiteral("QLabel[role=\"badge-warn\"] { border-radius: 8px; padding: 1px 8px; background: %1; color: %2; }\n").arg(t.accent_soft, t.text);
    s += QStringLiteral("QLabel[role=\"badge-ok\"] { border-radius: 8px; padding: 1px 8px; background: %1; color: %2; }\n").arg(t.hover, t.ok);
    s += QStringLiteral("QTabBar::close-button { image: url(%1); subcontrol-position: right; border-radius: 4px; padding: 1px; margin: 2px; }\n")
             .arg(indicator("close", t.muted));
    s += QStringLiteral("QTabBar::close-button:hover { image: url(%1); background: %2; }\n").arg(indicator("close", t.text), t.hover);
    s += QStringLiteral("QFrame#dialogCard { background: %1; border: 1px solid %2; border-radius: 8px; }\n").arg(t.panel, t.divider);
    s += QStringLiteral("QWidget#dialogFooter { border-top: 1px solid %1; }\n").arg(t.divider);
    s += QStringLiteral("QLabel#startNotice { background: %1; color: %2; border-radius: 6px; padding: 8px; }\n").arg(t.accent_soft, t.text);
    s += QStringLiteral("QWidget#saveFailure { background: %1; border: 1px solid %2; border-radius: 6px; }\n").arg(t.accent_soft, t.danger);
    s += QStringLiteral("QLabel#unportedBand { background: %1; color: %2; border-bottom: 1px solid %3; padding: 4px 8px; }\n")
             .arg(t.accent_soft, t.text, t.border);
    s += QStringLiteral("QPushButton#actionCard { background: %1; border: 1px solid %2; border-radius: 12px; padding: 0; text-align: left; }\n")
             .arg(t.panel, t.divider);
    s += QStringLiteral("QPushButton#actionCard:hover { background: %1; border-color: %2; }\n").arg(t.hover, t.border);
    s += QStringLiteral("QPushButton#actionCard:pressed { background: %1; }\n").arg(t.selected);
    s += QStringLiteral("QPushButton#actionCard[main=\"true\"] { border: 2px solid %1; }\n").arg(t.accent);
    s += QStringLiteral("QListWidget#recentBooks { background: transparent; border: none; }\n");
    s += QStringLiteral("QMenu[glass=\"true\"] { background: %1; border: 1px solid %2; }\n").arg(rgba(t.panel, 0.62), rgba(t.border, 0.7));
    return s;
}

void apply(QApplication* app) {
    if (app == nullptr) return;
    // the screen's letters: the bundled family (the same on every computer) or the computer's own
    if (setting("ui/ui_font", "genko") != QLatin1String("system")) {
        static QString family = [] {
            QString found;
            for (const char* face : {":/genko/fonts/IBMPlexSansJP-Regular.ttf", ":/genko/fonts/IBMPlexSansJP-Medium.ttf",
                                     ":/genko/fonts/IBMPlexSansJP-Bold.ttf"}) {
                const int id = QFontDatabase::addApplicationFont(QString::fromLatin1(face));
                const QStringList families = id >= 0 ? QFontDatabase::applicationFontFamilies(id) : QStringList();
                if (found.isEmpty() && !families.isEmpty()) found = families.front();
            }
            return found;
        }();
        if (!family.isEmpty() && app->font().family() != family) {
            QFont font = app->font();
            font.setFamily(family);
            font.setHintingPreference(QFont::PreferNoHinting);
            app->setFont(font);
        }
    }
    const Tokens t = tokens();
    const double pt = app->font().pointSizeF() > 0 ? app->font().pointSizeF() : 9.0;
    const QString key = t.window + t.accent + t.surround + QString::number(pt) + app->font().family();
    if (key == applied_key) return;
    applied_key = key;
    if (app->style()->name().toLower() != QLatin1String("fusion")) app->setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QPalette p;
    const auto set = [&p](QPalette::ColorRole role, const QString& colour) { p.setColor(role, QColor(colour)); };
    set(QPalette::Window, t.window), set(QPalette::WindowText, t.text), set(QPalette::Base, t.base);
    set(QPalette::AlternateBase, t.panel), set(QPalette::ToolTipBase, t.raised), set(QPalette::ToolTipText, t.text);
    set(QPalette::PlaceholderText, t.faint), set(QPalette::Text, t.text), set(QPalette::Button, t.raised);
    set(QPalette::ButtonText, t.text), set(QPalette::BrightText, t.danger), set(QPalette::Highlight, t.accent);
    set(QPalette::HighlightedText, t.accent_text), set(QPalette::Link, t.accent), set(QPalette::Mid, t.border);
    set(QPalette::Midlight, t.hover), set(QPalette::Dark, t.divider), set(QPalette::Light, t.base);
    for (const auto role : {QPalette::Text, QPalette::ButtonText, QPalette::WindowText}) p.setColor(QPalette::Disabled, role, QColor(t.faint));
    app->setPalette(p);
    app->setStyleSheet(style_sheet(t, pt));
    // Qt's own words in Japanese (はい・いいえ, the file and colour dialogs), whatever language the computer is set to
    static QTranslator* translator = nullptr;
    if (translator == nullptr) {
        translator = new QTranslator(app);
        if (translator->load(QStringLiteral("qtbase_ja"), QLibraryInfo::path(QLibraryInfo::TranslationsPath))) {
            app->installTranslator(translator);
        }
    }
}

QWidget* role(QWidget* widget, const char* name) {
    widget->setProperty("role", QString::fromLatin1(name));
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    return widget;
}

QLabel* hint(QLabel* label) {
    role(label, "hint");
    hints().emplace_back(label);
    show_hint(label, show_hints());
    return label;
}

void role_prop(QWidget* widget, const char* key, const QVariant& value) {
    widget->setProperty(key, value);
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}

QAbstractButton* primary(QAbstractButton* button) {
    role_prop(button, "primary", true);
    return button;
}

void name_buttons(QWidget* root) {
    for (QAbstractButton* button : root->findChildren<QAbstractButton*>()) {
        if (button->text().isEmpty() && button->accessibleName().isEmpty() && !button->toolTip().isEmpty()) {
            button->setAccessibleName(button->toolTip().section(QLatin1Char('\n'), 0, 0).trimmed());
        }
    }
}

}  // namespace genko::app::theme
