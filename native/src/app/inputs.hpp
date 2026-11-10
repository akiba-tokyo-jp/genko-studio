#pragma once

class QWidget;

// How number fields and choice lists take input everywhere in the app (Python's genko/app/inputs.py): a number field
// entered is all selected (typing replaces it, so "-10" or "4" goes in as typed), and the mouse wheel changes a field or
// a list only when it is the one being used (scrolling a dialog past it leaves it alone).
//
// The watcher sits on those widgets only, not on the whole app (an app-wide filter runs for every event of every widget:
// paints, mouse moves). Widgets are found when a window comes forward or the focus moves (watch() adds one by hand).

namespace genko::app::inputs {

// Once per app.
void install();
// The fields in `widget` (and itself) take input the app's way; each is set up once.
void watch(QWidget* widget);

}  // namespace genko::app::inputs
