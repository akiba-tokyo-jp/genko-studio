#pragma once

#include <QWidget>

#include <optional>
#include <string>

#include "core/json.hpp"

class QComboBox;
class QDoubleSpinBox;
class QGridLayout;
class QLabel;
class QTimer;
class QVBoxLayout;

// PC2a (window.panel.adjustments・window.panel.properties, design 5): the 色調補正 panel — a button for each kind of
// correction layer: pressed, a correction layer of that kind over the layer drawn on, in PhotoCraft's form (the user's
// decision 3), and the 属性 panel brought up — and the 属性 panel — the chosen layer's settings: a correction layer's
// own (its kind's form, its blend and opacity; what is moved shown on the page at once, what is let go one
// set_adjustment: one Undo), a fill's (its window), any layer's opacity, blend and lock.

namespace genko::app {

class MainWindow;

namespace adjust_forms {

class Form;

class AdjustmentsPanel : public QWidget {
    Q_OBJECT
public:
    explicit AdjustmentsPanel(MainWindow* window);
    // A correction layer of the kind over the layer drawn on (as its button does); false: not made.
    bool add(const std::string& kind);

private:
    MainWindow* window_;
};

class PropertiesPanel : public QWidget {
    Q_OBJECT
public:
    explicit PropertiesPanel(MainWindow* window);
    // The chosen layer's settings shown again (the book or the layer changed).
    void refresh();
    // What is being changed applied now (the panel applies it itself a moment after the last change).
    void commit();
    // Tests: the correction's form (none for another layer), the opacity, the blend.
    Form* form() const { return form_; }
    QDoubleSpinBox* opacity() const { return opacity_; }
    QComboBox* blend() const { return blend_; }

private:
    void build();
    void changed();
    std::optional<core::Json> pending() const;

    MainWindow* window_;
    QVBoxLayout* body_ = nullptr;
    QWidget* content_ = nullptr;
    QLabel* title_ = nullptr;
    Form* form_ = nullptr;
    QDoubleSpinBox* opacity_ = nullptr;
    QComboBox* blend_ = nullptr;
    QTimer* preview_ = nullptr;
    QTimer* settle_ = nullptr;
    QTimer* watch_ = nullptr;
    std::string page_id_, layer_id_, kind_;
    core::Json opened_;  // (the form's settings as shown: only what is changed from them is sent)
    const void* seen_ = nullptr;  // (the book as it was shown: a change from anywhere shows it again)
    bool editing_ = false;
    bool loading_ = false;
};

}  // namespace adjust_forms

}  // namespace genko::app
