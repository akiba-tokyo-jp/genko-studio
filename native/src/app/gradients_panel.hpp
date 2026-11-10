#pragma once

#include <QDialog>
#include <QObject>
#include <QPixmap>
#include <QWidget>

#include <array>
#include <functional>
#include <string>
#include <vector>

#include "core/json.hpp"
#include "render/preset_store.hpp"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGridLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

// グラデーション (PC4 E4, window.panel.gradients and gradient.presets.*, SPEC.md COLOR-09「グラデーションのプリセット」):
// the gradients of this machine (render/gradient_presets: the settings folder's gradients.json) in Photoshop's グラデーション
// panel — groups of presets, a new one from the current gradient, renamed, deleted, moved into a new group, .grd files read,
// the built-in groups again — and the current gradient the グラデーションツール paints with (the window's state, kept in the
// settings: gradient.presets.select); the グラデーションエディター edits a preset's colour stops (a colour of its own, the
// 描画色 or the 背景色) and its transparency stops.

namespace genko::app {

// The library and the current gradient.
class GradientLibrary : public QObject {
    Q_OBJECT
public:
    explicit GradientLibrary(QObject* parent = nullptr);
    const std::vector<render::preset_store::Group>& groups() const { return groups_; }
    // (the file there cannot be read: why — it is not written over, saving refused)
    const std::string& problem() const { return problem_; }
    // The groups changed and written (false: refused, `error` says why; the groups as they were).
    bool change(const std::vector<render::preset_store::Group>& groups, QString* error = nullptr);
    // (review P3-4) `work` done on the library as the settings folder holds it now (read again first: what another window
    // or `genko gradients` wrote since is kept) and written; false: refused (`error` says why — a core::Error `work`
    // throws too —; the groups as they were).
    bool update(const std::function<void(std::vector<render::preset_store::Group>&)>& work, QString* error = nullptr);
    // The library read again from the settings folder.
    void reload();
    // The preset named (the first of its name), or null.
    const core::Json* find(const std::string& name) const;
    // The current gradient (a preset item: {"name", "stops", "transparency"?}); 描画色から背景色へ at first.
    const core::Json& current() const { return current_; }
    void set_current(const core::Json& item);

signals:
    void changed();
    void current_changed();

private:
    std::vector<render::preset_store::Group> groups_;
    std::string problem_;
    core::Json current_;
};

// A strip of a preset's colours over a chequer (its transparency seen), for the panel and the tool's list.
QPixmap gradient_strip(const core::Json& item, QSize size, const std::array<int, 3>& foreground, const std::array<int, 3>& background);

// グラデーションエディター: a preset's name, its colour stops (位置 %, 種類 色・描画色・背景色, the colour) and its transparency
// stops (位置 %, 不透明度 %), 2 to 16 of each, a strip of it above.
class GradientEditor : public QDialog {
    Q_OBJECT
public:
    GradientEditor(const core::Json& item, const std::array<int, 3>& foreground, const std::array<int, 3>& background, QWidget* parent = nullptr);
    // The preset as edited (render/gradient_presets::item_of's form).
    core::Json item() const;
    QLineEdit* name = nullptr;
    QPushButton* add_colour = nullptr;
    QPushButton* add_opacity = nullptr;

private:
    struct ColourStop {
        double at = 0;
        QString kind = QStringLiteral("rgb");  // rgb | foreground | background
        std::array<int, 3> rgb{0, 0, 0};
    };
    struct OpacityStop {
        double at = 0;
        double opacity = 1;
    };
    void fill();
    std::vector<ColourStop> colours_;
    std::vector<OpacityStop> opacity_;
    std::array<int, 3> fg_, bg_;
    QLabel* strip_ = nullptr;
    QGridLayout* colour_rows_ = nullptr;
    QGridLayout* opacity_rows_ = nullptr;
};

// The panel.
class GradientsPanel : public QWidget {
    Q_OBJECT
public:
    GradientsPanel(GradientLibrary* library, QWidget* parent = nullptr);
    // The 描画色 and 背景色 the strips and the editor show.
    void set_colours(const std::array<int, 3>& foreground, const std::array<int, 3>& background);
    // The preset the list has chosen (none: a group or nothing).
    const core::Json* chosen() const;
    // The panel's commands (the tests use them; the buttons and the menu call them).
    void new_preset(const QString& name = {});
    void rename_chosen(const QString& name = {});
    void delete_chosen();
    void new_group(const QString& name = {});
    // (review P1-1, P3-3: the file read in the background — import_done() when it is in, or refused —; a group of its
    // name there already replaced only when the person says so, else the presets added to it)
    void import_grd(const QString& path = {});
    bool importing() const { return importing_; }
    void reset(bool append);
    void edit_chosen();

    QTreeWidget* tree = nullptr;
    QPushButton* apply = nullptr;   // (新規グラデーション塗りつぶしレイヤー)
    QPushButton* more = nullptr;    // (the panel's menu)

signals:
    // A preset clicked: the current gradient (gradient.presets.select; a gradient fill layer chosen takes it too).
    void preset_chosen(const core::Json& item);
    // 新規グラデーション塗りつぶしレイヤー (gradient.presets.apply).
    void apply_requested(const core::Json& item);
    void said(const QString& words);
    void import_done();

private:
    void fill();
    // (review P3-4: on the library as the folder holds it now) a warning when it cannot be written
    bool keep(const std::function<void(std::vector<render::preset_store::Group>&)>& work);
    void imported(const QString& path, const core::Json& items, const std::vector<std::string>& warnings, const std::string& error);
    bool importing_ = false;
    GradientLibrary* library_;
    std::array<int, 3> fg_{20, 20, 20}, bg_{255, 255, 255};
};

}  // namespace genko::app
