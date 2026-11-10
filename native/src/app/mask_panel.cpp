// PC3 B2 (masks@1): the 属性 panel's part for the chosen layer's mask — 濃度 (%) and ぼかし (mm), each applied as one
// set_layer_mask when it is let go (the page shows the mask as it will be), and リンク.

#include "app/mask_panel.hpp"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QSpinBox>

#include <cmath>

#include "app/main_window.hpp"
#include "core/pynum.hpp"

namespace genko::app {

using core::Json;

QWidget* mask_properties(MainWindow* window, const core::Page& page, const core::Layer& layer) {
    auto* box = new QGroupBox(QStringLiteral("レイヤーマスク"));
    box->setObjectName(QStringLiteral("properties_mask"));
    auto* form = new QFormLayout(box);
    const core::Mask& mask = *layer.mask;
    auto* density = new QSpinBox;
    density->setObjectName(QStringLiteral("properties_mask_density"));
    density->setRange(0, 100);
    density->setSuffix(QStringLiteral(" %"));
    density->setValue(static_cast<int>(std::lround(mask.density * 100)));
    auto* feather = new QDoubleSpinBox;
    feather->setObjectName(QStringLiteral("properties_mask_feather"));
    feather->setRange(0, 100);
    feather->setDecimals(2);
    feather->setSingleStep(0.1);
    feather->setSuffix(QStringLiteral(" mm"));
    feather->setValue(mask.feather_mm);
    auto* linked = new QCheckBox(QStringLiteral("リンク（レイヤーと一緒に動かす）"));
    linked->setObjectName(QStringLiteral("properties_mask_linked"));
    linked->setChecked(mask.linked);
    form->addRow(QStringLiteral("濃度"), density);
    form->addRow(QStringLiteral("ぼかし"), feather);
    form->addRow(linked);
    const Json index = page.index.json();
    const std::string id = layer.id;
    const auto set = [window, index, id](const char* key, const Json& value) {
        window->apply_ops(Json::array({Json{{"op", "set_layer_mask"}, {"page", index}, {"id", id}, {key, value}}}));
    };
    QObject::connect(density, &QSpinBox::editingFinished, box, [density, set, was = density->value()]() mutable {
        if (density->value() == was) return;
        was = density->value();
        set("density", core::py_round(density->value() / 100.0, 4));
    });
    QObject::connect(feather, &QDoubleSpinBox::editingFinished, box, [feather, set, was = feather->value()]() mutable {
        if (std::abs(feather->value() - was) < 1e-9) return;
        was = feather->value();
        set("feather_mm", core::py_round(feather->value(), 4));
    });
    QObject::connect(linked, &QCheckBox::toggled, box, [set](bool on) { set("linked", on); });
    return box;
}

}  // namespace genko::app
