#pragma once

#include <functional>
class QString;
class QWidget;

namespace genko::app {

// Browsing never edits a book. Activation is a callback into the window's existing Session/CommandBus.
QWidget* make_builtin_material_panel(QWidget* parent = nullptr, std::function<void(const QString&, const QString&)> activate = {});

}  // namespace genko::app
