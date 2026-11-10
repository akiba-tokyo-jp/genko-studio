#include "app/text_style.hpp"

#include <QAction>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPointer>
#include <QTextCursor>

#include <map>
#include <memory>

#include "app/ask.hpp"
#include "app/lettering.hpp"

namespace genko::app::text_style {

namespace {

// SHORTCUTS
const std::map<QString, QString>& shortcuts() {
    static const std::map<QString, QString> keys = {
        {QStringLiteral("太"), QStringLiteral("Ctrl+B")}, {QStringLiteral("大"), QStringLiteral("Ctrl+Shift+.")}, {QStringLiteral("小"), QStringLiteral("Ctrl+Shift+,")}};
    return keys;
}

}  // namespace

const std::vector<std::pair<QString, QString>>& choices() {
    static const std::vector<std::pair<QString, QString>> list = {
        {QStringLiteral("大きく"), QStringLiteral("大")},
        {QStringLiteral("とても大きく"), QStringLiteral("特大")},
        {QStringLiteral("小さく"), QStringLiteral("小")},
        {QStringLiteral("太く"), QStringLiteral("太")},
        {QStringLiteral("極太に"), QStringLiteral("極太")},
        {QStringLiteral("赤に"), QStringLiteral("赤")},
        {QStringLiteral("青に"), QStringLiteral("青")},
        {QStringLiteral("白に"), QStringLiteral("白")},
        {QStringLiteral("縦中横（数字などを横に並べる）"), QStringLiteral("縦中横")},
        {QStringLiteral("傍点を付ける"), QString()},
        {QStringLiteral("ルビを付ける…"), QStringLiteral("ruby")},
    };
    return list;
}

bool wrap(QPlainTextEdit* edit, const QString& mark) {
    QTextCursor cursor = edit->textCursor();
    const QString chosen = cursor.selectedText().replace(QChar(0x2029), QLatin1Char('\n'));
    if (chosen.isEmpty()) return false;
    QString text;
    if (mark == QLatin1String("ruby")) {
        const QPointer<QPlainTextEdit> alive(edit);
        const auto reading = ask::get_text(edit, QStringLiteral("ルビ"), QStringLiteral("「%1」の読み").arg(chosen));
        if (alive.isNull() || !reading || lettering::strip(*reading).isEmpty()) return false;
        text = QStringLiteral("｜%1《%2》").arg(chosen, lettering::strip(*reading));
    } else if (mark.isEmpty()) {
        text = QStringLiteral("《《%1》》").arg(chosen);
    } else {
        text = QStringLiteral("{%1|%2}").arg(mark, chosen);
    }
    cursor.insertText(text);
    edit->setTextCursor(cursor);
    return true;
}

QMenu* menu_for(QPlainTextEdit* edit, QMenu* parent_menu) {
    auto* menu = new QMenu(QStringLiteral("選んだ文字を"), parent_menu != nullptr ? static_cast<QWidget*>(parent_menu) : edit);
    const bool chosen = edit->textCursor().hasSelection();
    for (const auto& [label, mark] : choices()) {
        QAction* action = menu->addAction(label, edit, [edit, mark = mark] { wrap(edit, mark); });
        action->setEnabled(chosen);
        if (const auto it = shortcuts().find(mark); it != shortcuts().end()) action->setShortcut(QKeySequence(it->second));
    }
    if (!chosen) menu->setToolTip(QStringLiteral("先に文字をドラッグして選びます"));
    return menu;
}

void install(QPlainTextEdit* edit) {
    edit->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(edit, &QPlainTextEdit::customContextMenuRequested, edit, [edit](const QPoint& pos) {
        std::unique_ptr<QMenu> menu(edit->createStandardContextMenu());
        menu->addSeparator();
        menu->addMenu(menu_for(edit, menu.get()));
        const QPointer<QPlainTextEdit> alive(edit);
        edit->setProperty("menuOpen", true);  // (an in-place editor keeps the line open while its menu is up)
        menu->exec(edit->mapToGlobal(pos));
        if (!alive.isNull()) edit->setProperty("menuOpen", false);
    });
    for (const auto& [mark, keys] : shortcuts()) {
        auto* action = new QAction(edit);
        action->setShortcut(QKeySequence(keys));
        action->setShortcutContext(Qt::WidgetShortcut);
        QObject::connect(action, &QAction::triggered, edit, [edit, mark = mark] { wrap(edit, mark); });
        edit->addAction(action);
    }
}

}  // namespace genko::app::text_style
