// The system's frosted glass (Python's genko/app/glass.py).

#include "app/glass.hpp"

#include <QApplication>
#include <QEvent>
#include <QMenu>
#include <QOperatingSystemVersion>
#include <QSettings>
#include <QStyleFactory>
#include <QWidget>

#ifdef _WIN32
#include <windows.h>
#endif

#include "app/theme.hpp"

namespace genko::app::glass {

namespace {

std::optional<QString>& forced_kind() {
    static std::optional<QString> kind;
    return kind;
}

std::function<bool(QWidget*)>& forced_apply() {
    static std::function<bool(QWidget*)> apply;
    return apply;
}

#ifdef _WIN32
bool windows_glass(QWidget* widget, bool dark) {
    // (dwmapi is asked for by name: nothing to link, and a Windows without it simply has no glass)
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (dwm == nullptr) return false;
    using SetAttribute = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    struct Margins {
        int left, right, top, bottom;
    };
    using ExtendFrame = HRESULT(WINAPI*)(HWND, const Margins*);
    const auto set = reinterpret_cast<SetAttribute>(GetProcAddress(dwm, "DwmSetWindowAttribute"));
    const auto extend = reinterpret_cast<ExtendFrame>(GetProcAddress(dwm, "DwmExtendFrameIntoClientArea"));
    if (set == nullptr || extend == nullptr) return false;
    const HWND hwnd = reinterpret_cast<HWND>(widget->winId());
    const int dark_mode = dark ? 1 : 0;
    set(hwnd, 20, &dark_mode, sizeof(dark_mode));  // DWMWA_USE_IMMERSIVE_DARK_MODE
    const int corner = 2;
    set(hwnd, 33, &corner, sizeof(corner));  // DWMWA_WINDOW_CORNER_PREFERENCE: round
    const Margins whole{-1, -1, -1, -1};
    extend(hwnd, &whole);
    const int backdrop = 3;  // DWMSBT_TRANSIENTWINDOW: Acrylic
    return set(hwnd, 38, &backdrop, sizeof(backdrop)) == S_OK;  // DWMWA_SYSTEMBACKDROP_TYPE
}
#endif

// On one see-through window: the glass asked for when it is first shown.
class Shown : public QObject {
public:
    using QObject::QObject;
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::Show) {
            if (auto* widget = qobject_cast<QWidget*>(watched)) apply(widget);
            watched->removeEventFilter(this);
        }
        return false;
    }
};

QStyle* made = nullptr;

}  // namespace

QString available() {
    if (forced_kind()) return *forced_kind();
#ifdef _WIN32
    static const QString kind = [] {
        if (QOperatingSystemVersion::current() < QOperatingSystemVersion(QOperatingSystemVersion::Windows, 10, 0, 22621)) {
            return QString();  // (Windows 11 22H2: the system backdrop for any window)
        }
        const QSettings personalize(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"),
                                    QSettings::NativeFormat);
        if (personalize.contains(QStringLiteral("EnableTransparency")) && personalize.value(QStringLiteral("EnableTransparency")).toInt() == 0) {
            return QString();
        }
        return QStringLiteral("windows");
    }();
    return kind;
#else
    return {};
#endif
}

bool wants(QWidget* widget) {
    if (qobject_cast<QMenu*>(widget) != nullptr) return true;
    if (widget->windowType() == Qt::ToolTip) return true;
    return widget->property("glass_wanted").toBool();
}

bool apply(QWidget* widget) {
    const QString kind = available();
    if (kind.isEmpty() || widget->property("glass").isValid()) return widget->property("glass").toBool();
    bool done = false;
    if (forced_apply()) {
        done = forced_apply()(widget);
    } else {
#ifdef _WIN32
        done = windows_glass(widget, theme::tokens().dark);
#endif
    }
    theme::role_prop(widget, "glass", done);
    return done;
}

GlassStyle::GlassStyle() : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion"))) {
    setObjectName(QStringLiteral("fusion"));  // (the theme keeps it: it is the Fusion style)
    shown_ = new Shown(this);
}

void GlassStyle::polish(QWidget* widget) {
    QProxyStyle::polish(widget);
    if (widget->isWindow() && !widget->property("glass").isValid() && !widget->property("glass_readied").toBool() && wants(widget)) {
        widget->setProperty("glass_readied", true);  // (once: a style set again readies every widget again)
        widget->setAttribute(Qt::WA_TranslucentBackground);  // (before its window is made)
        widget->installEventFilter(shown_);
    }
}

QStyle* style() {
    if (available().isEmpty()) return QStyleFactory::create(QStringLiteral("Fusion"));
    made = new GlassStyle;
    return made;
}

bool install(QApplication* app) {
    if (app == nullptr) app = qobject_cast<QApplication*>(QCoreApplication::instance());
    if (app == nullptr || available().isEmpty()) return false;
    if (made == nullptr) QApplication::setStyle(style());  // (the theme sets it; an app styled some other way gets it here)
    return true;
}

void set_available_for_tests(std::optional<QString> kind) { forced_kind() = std::move(kind); }
void set_apply_for_tests(std::function<bool(QWidget*)> apply) { forced_apply() = std::move(apply); }

}  // namespace genko::app::glass
