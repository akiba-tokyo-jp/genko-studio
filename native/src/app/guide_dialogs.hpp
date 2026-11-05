#pragma once

#include <QDialog>
#include <QString>

#include <functional>
#include <optional>

#include "core/guide_layout.hpp"
#include "core/json.hpp"
#include "core/model.hpp"
#include "core/units.hpp"

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QRadioButton;
class QSpinBox;

// The guides' windows (PC5 H-b, 節 H 5.1, SPEC.md VIEW-01「ガイド」): 表示 → 新規ガイド… (Photoshop's: its way and its place,
// in 環境設定 → 単位・定規's unit — a guide double-clicked opens it to move it) and 新規ガイドレイアウトを作成… (columns, rows,
// margins, centred columns, the guides there taken away first, what it is laid on — the paper, the finished size, the 基本枠
// — shown on the page as it is set; presets kept on this computer: settings guide_layouts/<name>). Each gives what the
// window turns into its op (add_ruler / edit_ruler, add_guide_layout): the lengths in mm.

namespace genko::app {

class NewGuideDialog : public QDialog {
    Q_OBJECT

public:
    // `axis`, `at_mm`: the guide moved (none: a new one, across at the page's middle as Photoshop's 0 would be off it)
    NewGuideDialog(QWidget* parent, const core::Page& page, core::units::Unit unit, const core::units::Basis& across_basis,
                   const core::units::Basis& down_basis, const QString& axis = QString(), std::optional<double> at_mm = std::nullopt);
    QString axis() const;  // "h" | "v"
    double at_mm() const;

    QRadioButton* across = nullptr;   // 水平方向 (a guide across: its y)
    QRadioButton* upright = nullptr;  // 垂直方向 (an upright guide: its x)
    QDoubleSpinBox* position = nullptr;
    QDialogButtonBox* buttons = nullptr;

private:
    void follow();
    core::units::Unit unit_;
    core::units::Basis across_basis_;  // (a length along the width)
    core::units::Basis down_basis_;    // (along the height)
    bool was_across_ = true;
};

class GuideLayoutDialog : public QDialog {
    Q_OBJECT

public:
    // `preview`: called with the guides the layout makes as it is set (none: nothing to show — or the preview turned off)
    GuideLayoutDialog(QWidget* parent, const core::Page& page, core::units::Unit unit, const core::units::Basis& across_basis,
                      const core::units::Basis& down_basis, std::function<void(std::optional<core::GuideLayout>)> preview = {});
    // The add_guide_layout op (with the page), and the layout it makes now (none: it would be refused — `why` says why).
    core::Json op() const;
    std::optional<core::GuideLayout> layout(QString* why = nullptr) const;
    // The settings of the window (an op's fields without the page) put in, and the presets kept.
    void put(const core::Json& settings);
    void save_preset(const QString& name);
    void delete_preset(const QString& name);

    QComboBox* preset = nullptr;
    QPushButton* save = nullptr;
    QPushButton* remove = nullptr;
    QComboBox* from = nullptr;  // paper | trim | frame
    QCheckBox* columns_on = nullptr;
    QSpinBox* columns = nullptr;
    QDoubleSpinBox* width = nullptr;  // (its least: 自動)
    QDoubleSpinBox* gutter = nullptr;
    QCheckBox* rows_on = nullptr;
    QSpinBox* rows = nullptr;
    QDoubleSpinBox* height = nullptr;
    QDoubleSpinBox* row_gutter = nullptr;
    QCheckBox* margin_on = nullptr;
    QDoubleSpinBox* top = nullptr;
    QDoubleSpinBox* left = nullptr;
    QDoubleSpinBox* bottom = nullptr;
    QDoubleSpinBox* right = nullptr;
    QCheckBox* center = nullptr;
    QCheckBox* clear = nullptr;
    QCheckBox* show_preview = nullptr;
    QLabel* note = nullptr;
    QDialogButtonBox* buttons = nullptr;

private:
    core::Json fields() const;  // (the op's fields, mm, without the page)
    void changed();
    void fill_presets(const QString& chosen);
    const core::Page& page_;
    core::units::Unit unit_;
    core::units::Basis across_basis_;
    core::units::Basis down_basis_;
    std::function<void(std::optional<core::GuideLayout>)> preview_;
    bool putting_ = false;
};

}  // namespace genko::app
