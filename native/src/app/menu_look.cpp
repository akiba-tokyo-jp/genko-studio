// 編集 → メニュー…'s hidden and coloured items in the window's menus (PC5 H-d; PhotoCraft v0.2.0's prefs.rs menus, MIT).

#include "app/menu_look.hpp"

#include <QAction>
#include <QEvent>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QPointer>
#include <QTimer>
#include <QVariant>
#include <QWidget>

#include <algorithm>
#include <set>

#include "app/config.hpp"
#include "app/main_window.hpp"
#include "app/preferences.hpp"
#include "app/workspace.hpp"

namespace genko::app::menu_look {

const char* const kShowAll = "すべてのメニュー項目を表示";

namespace {

const QString kHidden = QStringLiteral("menus/hidden");
const QString kColours = QStringLiteral("menus/colors");

class Look;

// The colours over a menu's items: a band at the item's left and a light tint over it (a child of the menu, painted after
// it, whatever the style draws; the mouse goes through it).
class Overlay : public QWidget {
public:
    Overlay(QMenu* menu, Look* look) : QWidget(menu), menu_(menu), look_(look) {
        setObjectName(QStringLiteral("menu_colours"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setFocusPolicy(Qt::NoFocus);
    }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QMenu* menu_;
    Look* look_;
};

// (by its name: the classes here have no meta-object of their own, which qobject_cast and findChild<T> need)
Overlay* overlay_of(QMenu* menu) {
    for (QObject* child : menu->children()) {
        if (child->objectName() == QLatin1String("menu_colours")) return dynamic_cast<Overlay*>(child);
    }
    return nullptr;
}

// One window's: the commands hidden and coloured now, and what each open menu has taken out.
class Look : public QObject {
public:
    explicit Look(MainWindow* window) : QObject(window) { setObjectName(QStringLiteral("menu_look")); }

    std::set<const QAction*> hidden;
    std::map<const QAction*, QColor> colours;

    void watch(QMenu* menu) {
        if (menu == nullptr || menu->property("genko_menu_look").toBool()) return;
        menu->setProperty("genko_menu_look", true);
        menu->setProperty("genko_menu_look_owner", QVariant::fromValue(static_cast<QObject*>(this)));
        // (as it shows, not as it is about to: a menu filled to be read — the help's fill emits aboutToShow — never loses an
        // item; the menu shown resizes to what it holds)
        connect(menu, &QObject::destroyed, this, [this, menu] { opened_.erase(menu); });
        menu->installEventFilter(this);
        for (QAction* item : menu->actions()) watch(item->menu());
    }

    std::vector<std::pair<QRect, QColor>> painted(QMenu* menu) const {
        std::vector<std::pair<QRect, QColor>> out;
        for (QAction* item : menu->actions()) {
            const auto found = colours.find(item);
            if (found != colours.end() && item->isVisible()) out.emplace_back(menu->actionGeometry(item), found->second);
        }
        return out;
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        auto* menu = qobject_cast<QMenu*>(watched);
        if (menu != nullptr && event->type() == QEvent::Show) opening(menu);
        if (menu != nullptr && event->type() == QEvent::Hide) closing(menu);
        if (event->type() == QEvent::Show || event->type() == QEvent::Resize) {
            if (menu != nullptr) {
                if (Overlay* overlay = overlay_of(menu)) {
                    overlay->setGeometry(menu->rect());
                    overlay->raise();
                }
            }
        }
        return false;
    }

private:
    struct Out {
        QPointer<QAction> action;
        QPointer<QAction> next;  // (the item it was before: put back there)
    };
    struct Opened {
        std::vector<Out> out;
        QPointer<QAction> line;
        QPointer<QAction> all;
        bool show_all = false;
        QPoint at;
    };
    std::map<QMenu*, Opened> opened_;

    void opening(QMenu* menu) {
        Opened& now = opened_[menu];
        put_back(menu, now);  // (an opening not closed: as it was first)
        for (QAction* item : menu->actions()) watch(item->menu());  // (a menu made since)
        if (now.show_all) {
            now.show_all = false;  // (すべてのメニュー項目を表示: this once, every item)
        } else {
            const QList<QAction*> items = menu->actions();
            for (qsizetype i = 0; i < items.size(); ++i) {
                if (hidden.count(items[i]) != 0 && !items[i]->isSeparator() && items[i]->menu() == nullptr) {
                    now.out.push_back({items[i], i + 1 < items.size() ? items[i + 1] : nullptr});
                }
            }
            for (const Out& out : now.out) menu->removeAction(out.action);
            if (!now.out.empty()) {
                now.line = menu->addSeparator();
                now.all = menu->addAction(QString::fromUtf8(kShowAll));
                now.all->setObjectName(QStringLiteral("menu_show_all"));  // (not a command: never named, never given keys)
                connect(now.all, &QAction::triggered, this, [this, menu] {
                    Opened& again = opened_[menu];
                    again.show_all = true;
                    const QPoint at = again.at;
                    QTimer::singleShot(0, menu, [menu, at] { menu->popup(at); });
                });
            }
        }
        // the colours: painted over the items when some are coloured
        bool any = false;
        for (QAction* item : menu->actions()) any = any || colours.count(item) != 0;
        Overlay* overlay = overlay_of(menu);
        if (any && overlay == nullptr) overlay = new Overlay(menu, this);
        if (overlay != nullptr) {
            overlay->setVisible(any);
            overlay->setGeometry(menu->rect());
            overlay->raise();
            overlay->update();
        }
    }

    void closing(QMenu* menu) {
        const auto found = opened_.find(menu);
        if (found == opened_.end()) return;
        found->second.at = menu->pos();
        put_back(menu, found->second);
    }

    static void put_back(QMenu* menu, Opened& now) {
        for (QPointer<QAction>* extra : {&now.all, &now.line}) {
            if (*extra) {
                menu->removeAction(*extra);
                (*extra)->deleteLater();  // (later: its click is handled after the menu hides)
            }
            *extra = nullptr;
        }
        for (auto out = now.out.rbegin(); out != now.out.rend(); ++out) {
            if (out->action) menu->insertAction(out->next, out->action);
        }
        now.out.clear();
    }
};

void Overlay::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    for (const auto& [rect, colour] : look_->painted(menu_)) {
        QColor tint = colour;
        tint.setAlpha(64);
        painter.fillRect(rect, tint);
        painter.fillRect(QRect(rect.left(), rect.top() + 1, 4, std::max(1, rect.height() - 2)), colour);
    }
}

Look* look_of(MainWindow* window) {
    for (QObject* child : window->children()) {
        if (child->objectName() == QLatin1String("menu_look")) {
            if (auto* look = dynamic_cast<Look*>(child)) return look;
        }
    }
    return new Look(window);
}

QStringList split_names(const QString& text) { return text.split(QLatin1Char('\t'), Qt::SkipEmptyParts); }

}  // namespace

const std::vector<Colour>& colours() {
    // (Photoshop's メニュー… colours: なし・赤・オレンジ・黄・緑・青・紫・グレー)
    static const std::vector<Colour> all{{QStringLiteral("none"), QStringLiteral("なし"), QColor()},
                                         {QStringLiteral("red"), QStringLiteral("赤"), QColor(0xe5, 0x53, 0x4b)},
                                         {QStringLiteral("orange"), QStringLiteral("オレンジ"), QColor(0xf0, 0x88, 0x3e)},
                                         {QStringLiteral("yellow"), QStringLiteral("黄"), QColor(0xe3, 0xb3, 0x41)},
                                         {QStringLiteral("green"), QStringLiteral("緑"), QColor(0x57, 0xab, 0x5a)},
                                         {QStringLiteral("blue"), QStringLiteral("青"), QColor(0x53, 0x9b, 0xf5)},
                                         {QStringLiteral("violet"), QStringLiteral("紫"), QColor(0xb0, 0x83, 0xf0)},
                                         {QStringLiteral("gray"), QStringLiteral("グレー"), QColor(0x90, 0x9d, 0xab)}};
    return all;
}

QColor colour(const QString& id) {
    for (const Colour& one : colours()) {
        if (one.id == id) return one.colour;
    }
    return {};
}

QStringList hidden() { return split_names(settings()->value(kHidden).toString()); }

void set_hidden(const QStringList& names) {
    if (names.isEmpty()) {
        settings()->remove(kHidden);
    } else {
        settings()->setValue(kHidden, names.join(QLatin1Char('\t')));
    }
}

std::map<QString, QString> coloured() {
    auto store = settings();
    std::map<QString, QString> out;
    store->beginGroup(kColours);
    for (const QString& name : store->allKeys()) {  // (all: a name with "/" in it is a key inside groups)
        const QString id = store->value(name).toString();
        if (colour(id).isValid()) out[name] = id;
    }
    store->endGroup();
    return out;
}

void set_coloured(const std::map<QString, QString>& chosen) {
    auto store = settings();
    store->remove(kColours);
    for (const auto& [name, id] : chosen) {
        if (colour(id).isValid()) store->setValue(kColours + QLatin1Char('/') + name, id);
    }
}

void apply(MainWindow* window) {
    Look* look = look_of(window);
    const QStringList hid = hidden();
    const auto colour_of = coloured();
    look->hidden.clear();
    look->colours.clear();
    if (!hid.isEmpty() || !colour_of.empty()) {
        const auto names = command_names(window);
        for (QAction* action : commands(window)) {
            const QString name = command_name(names, action);
            // (kept under its name, or an old name of it: renamed() — the review's P3-10)
            const QStringList kept_as = QStringList{name} + preferences::old_names(name);
            for (const QString& one : kept_as) {
                if (hid.contains(one)) look->hidden.insert(action);
            }
            for (auto one = kept_as.rbegin(); one != kept_as.rend(); ++one) {
                if (const auto found = colour_of.find(*one); found != colour_of.end()) look->colours[action] = colour(found->second);
            }
        }
    }
    for (QAction* top : window->menuBar()->actions()) look->watch(top->menu());
}

std::vector<std::pair<QRect, QColor>> painted(QMenu* menu) {
    auto* look = dynamic_cast<Look*>(menu->property("genko_menu_look_owner").value<QObject*>());
    return look != nullptr ? look->painted(menu) : std::vector<std::pair<QRect, QColor>>{};
}

}  // namespace genko::app::menu_look
