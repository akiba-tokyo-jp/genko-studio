#pragma once

#include <QString>

#include <utility>
#include <vector>

class QMenu;
class QPlainTextEdit;

// Styling the characters chosen in a line being typed (Python's genko/app/text_style.py: 選んだ文字を大きく・太く・色を
// 変える) without typing the notation: the chosen part is wrapped in it ({太|…}, 《《…》》, ｜…《…》), and the page shows
// it styled.

namespace genko::app::text_style {

// CHOICES: (label, the mark put around the chosen part; "" = 傍点, "ruby" = asks for the reading)
const std::vector<std::pair<QString, QString>>& choices();
// The chosen characters wrapped in the notation for `mark` (false: nothing is chosen, or no reading was given).
bool wrap(QPlainTextEdit* edit, const QString& mark);
// 「選んだ文字を」: each style, enabled while characters are chosen.
QMenu* menu_for(QPlainTextEdit* edit, QMenu* parent_menu = nullptr);
// The right-click menu gets 「選んだ文字を」, and Ctrl+B and the others work while typing. While that menu is up the edit's
// property "menuOpen" is true (an in-place editor stays open).
void install(QPlainTextEdit* edit);

}  // namespace genko::app::text_style
