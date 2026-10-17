// PC2 (補正・色管理, design 5): the menus' frame of the corrections — レイヤー → 色調補正 (the destructive corrections on
// the layer drawn on, inside the selection) and their commands in 編集 —, each group's commands added by its own file
// (app/adjust_forms.hpp: build_tone_menu PC2a, build_color_menu PC2b, build_image_menu PC2c).

#include <QMenu>

#include "app/adjust_forms.hpp"
#include "app/main_window.hpp"

namespace genko::app {

void MainWindow::build_adjust_menus(QMenu* layers, QMenu* edit) {
    auto* adjust = new QMenu(QStringLiteral("色調補正"), layers);
    adjust->setObjectName(QStringLiteral("menu_adjust"));
    adjust_forms::MenuContext context;
    context.window = this;
    context.make = [this](const QString& name, const QString& title, std::function<void()> run, const QList<QKeySequence>& keys) {
        return make(name, title, std::move(run), keys);
    };
    adjust_forms::build_tone_menu(adjust, context);
    adjust_forms::build_color_menu(adjust, context);
    adjust_forms::build_image_menu(adjust, edit, context);
    if (adjust->isEmpty()) {  // (no command yet: the menus as they were)
        delete adjust;
        return;
    }
    layers->addSeparator();
    layers->addMenu(adjust);
}

}  // namespace genko::app
