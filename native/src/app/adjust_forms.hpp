#pragma once

#include <QKeySequence>
#include <QList>
#include <QString>
#include <QWidget>

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/layer_dialogs.hpp"
#include "core/adjust_kinds.hpp"
#include "core/json.hpp"
#include "core/model.hpp"

class QAction;
class QFormLayout;
class QMenu;

// PC2 (補正・色管理, design 5): the windows of the corrections — one form of fields for each kind (the window of a
// destructive correction, a correction layer's window, later the properties panel's), and the menus' frame. Each
// group's own file adds its kinds' forms and commands (app/adjust_forms_tone.cpp: PC2a, app/adjust_forms_color.cpp:
// PC2b, app/image_dialogs.cpp: PC2c); a kind with none has no new command (the old windows of Python's kinds,
// layer_dialogs' filter_params, stay as they are).

namespace genko::app {

class MainWindow;
struct BookChange;

namespace adjust_forms {

// What a form knows of where it is asked.
struct FormContext {
    const core::Page* page = nullptr;     // (its mode: which channels a channel switch offers)
    std::array<int, 3> pen{20, 20, 20};   // (the main colour: a colour's default where the kind has none)
    std::array<int, 3> sub{255, 255, 255};  // (PC4: the sub colour — 背景色 — of a filter that reads both)
    std::optional<std::vector<int>> histogram;  // (the layer's lightness: levels and a tone curve)
    // 「自動」(design 3.3): the settings it finds for the layer; none: no such button.
    std::function<std::optional<core::Json>()> automatic;
};

// The fields of one correction's settings, laid out as a form. A field's key may be a path into its native
// ("native.blend": {"native": {"blend": …}}). Each field has its default (「既定に戻す」 sets every one back) and
// says when it changes.
class Form : public QWidget {
    Q_OBJECT
public:
    explicit Form(QWidget* parent = nullptr);

    // A number lo..hi (`decimals` places; -1: by its range), a choice among labelled values, a check, a colour
    // (QColorDialog), and any widget of a group's own (its value got and set as JSON).
    void number(const std::string& key, const QString& label, double lo, double hi, double fallback, int decimals = -1);
    void choice(const std::string& key, const QString& label, std::vector<std::pair<QString, core::Json>> options, const core::Json& fallback);
    void check(const std::string& key, const QString& label, bool fallback);
    void colour(const std::string& key, const QString& label, std::array<int, 3> fallback);
    void custom(const std::string& key, const QString& label, QWidget* widget, std::function<core::Json()> get,
                std::function<void(const core::Json&)> set, const core::Json& fallback);
    // A field for each setting of the kind's table (core/adjust_kinds.hpp) that has a label here, in the table's order:
    // numbers, whole numbers, checks, choices (`choices`: the words of a choice's values; none: the values) and colours
    // with their ranges and defaults. (Lists, groups and a group's own: its custom fields.)
    void from_table(const core::AdjustKind& kind, const std::map<std::string, QString>& labels,
                    const std::map<std::string, std::vector<std::pair<QString, core::Json>>>& choices = {});
    // A note under the fields (what the numbers mean).
    void note(const QString& text);

    // The settings as the fields hold them ({key: value}, the paths nested), each field from `now` (one it does not
    // hold: its default), and every field back to its default.
    core::Json values() const;
    void set_values(const core::Json& now);
    void reset();
    QFormLayout* layout_form() const { return form_; }

signals:
    void changed();

private:
    struct Field {
        std::string key;
        core::Json fallback;
        std::function<core::Json()> get;
        std::function<void(const core::Json&)> set;
    };
    void add(const QString& label, QWidget* widget, Field field);
    QFormLayout* form_ = nullptr;
    std::vector<Field> fields_;
    bool quiet_ = false;  // (setting the fields: one changed signal at the end)
};

// One kind's form: its window's title and how its fields are made (the window then sets them from the settings it starts
// from: Form::set_values).
using Builder = std::function<void(Form& form, const FormContext& context)>;
struct FormKind {
    std::string_view kind;
    std::string_view group;
    QString title;
    Builder build;
    // (PC2a, the user's decision 3) the settings a correction made new on the screen starts from — PhotoCraft's form:
    // levels and a tone curve upstream, brightness_contrast modern, posterize floor —; none: the form's defaults.
    core::Json fresh = core::Json::object();
    // An existing correction's settings as the form holds them (a gradient map's colours as its stops); none: as they are.
    std::function<core::Json(const core::Json&)> prepare;
};
// Every kind with a form, by group (each group's file: tone_forms, color_forms).
const std::vector<FormKind>& form_kinds();
const FormKind* form_kind(std::string_view kind);
// The name of a correction's commands (the user's decision 8: Photoshop's, the same for レイヤー → 色調補正's command and
// the layer panel's 色調補正のレイヤー item — コマンドを探す tells them apart by their place): its form's title and "…"
// (階調の反転: done at once, no window); none for a kind with no form.
QString command_title(std::string_view kind);
std::vector<FormKind> tone_forms();   // PC2a: app/adjust_forms_tone.cpp
std::vector<FormKind> color_forms();  // PC2b: app/adjust_forms_color.cpp

// A correction's window (design 5): its form from `now` (its defaults where it has none), 「既定に戻す」, 「自動」 when the
// context has it, the result shown on the page while it changes (`preview`, 250 ms after the last change, as
// filter_params), かける / やめる. The settings, or none when the person stopped (or the kind has no form). `opened`: the
// settings as the form showed them when it opened (changes(): what the person changed).
std::optional<core::Json> adjust_dialog(QWidget* parent, const std::string& kind, const core::Json& now, const FormContext& context,
                                        FilterPreview preview = {}, core::Json* opened = nullptr);

// (the user's decision 3: an existing correction is set again in its own form) What a form's settings changed from
// those it opened with (`before`; both Form::values()): each setting changed, an object's by its own keys (a native's, a
// channel record's), one taken out (a record set back to doing nothing) as null; {} for none. lay_changes lays them over
// a correction as it is (null: the key taken out; a native left empty: taken out) — what the window did not change stays
// as it was (Python's form kept: a gamma curve's absent points, a levels' gamma of three places, a gradient map's
// colours).
core::Json changes(const core::Json& before, const core::Json& after);
void lay_changes(core::Json& adjust, const core::Json& changes);
// A form's settings without what it leaves out (a null: Python's default, no native setting), for a correction made new.
core::Json without_nulls(const core::Json& values);

// The menus' frame (MainWindow::build_adjust_menus, app/main_window_adjust.cpp): each group adds its commands to レイヤー →
// 色調補正 (`adjust`) and to 編集 (`edit`) with the window's `make` (a command registered by its name).
using MakeAction = std::function<QAction*(const QString& name, const QString& title, std::function<void()> run, const QList<QKeySequence>& keys)>;
struct MenuContext {
    MainWindow* window = nullptr;
    MakeAction make;
};
void build_tone_menu(QMenu* adjust, const MenuContext& context);   // PC2a: app/adjust_forms_tone.cpp
void build_color_menu(QMenu* adjust, const MenuContext& context);  // PC2b: app/adjust_forms_color.cpp
void build_image_menu(QMenu* adjust, QMenu* edit, const MenuContext& context);  // PC2c: app/image_dialogs.cpp
// PC2c (フェード, app/image_dialogs.cpp): after each change of the window's book (MainWindow::on_book_changed), what 編集
// → フェード would take back — the pixels a filter, a correction or a fill changed — and the command enabled by it.
void follow_change(MainWindow& window, const BookChange& change);
// PC2a (window.panel.adjustments・window.panel.properties): the 色調補正 and 属性 panels, docked and listed in ウィンドウ
// (`window_menu`): app/adjust_panels.cpp.
void build_tone_docks(MainWindow* window, QMenu* window_menu);

}  // namespace adjust_forms

}  // namespace genko::app
