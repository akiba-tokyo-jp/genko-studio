// PC4 E2 (brush.presets.*, brush.defineFromSelection, edit.defineBrushPreset, window.panel.brushes・.brushSettings・
// .toolPresets, tool.presets.*, edit.presets.*; SPEC.md COLOR-09「ブラシのプリセット」「ツールプリセット」「プリセットマネージャー」):
// the brush in hand chosen from the library (a built-in dab brush made known under the key this book takes for it),
// 編集 → ブラシを定義… and プリセット ▸ (プリセットマネージャー…, プリセットを書き出し／読み込み…, プリセットを移行…), the
// ブラシ・ブラシ設定 (F5)・ツールプリセット panels in the docks E-0 made, and a tool preset taken and set.

#include <QAbstractButton>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

#include <filesystem>

#include "app/ask.hpp"
#include "app/brush_panel.hpp"
#include "app/brush_settings_panel.hpp"
#include "app/brushes_panel.hpp"
#include "app/canvas.hpp"
#include "app/colours.hpp"
#include "app/config.hpp"
#include "app/gradients_panel.hpp"
#include "app/main_window.hpp"
#include "app/off_ui.hpp"
#include "app/patterns_panel.hpp"
#include "app/preset_manager.hpp"
#include "app/shapes_panel.hpp"
#include "app/tool_presets_panel.hpp"
#include "app/tool_settings.hpp"
#include "core/brush_dabs.hpp"
#include "core/brushes.hpp"
#include "core/error.hpp"
#include "core/paths.hpp"
#include "render/brush_presets.hpp"
#include "render/brushes.hpp"
#include "render/preset_files.hpp"
#include "service/wording.hpp"
#include "storage/fsutil.hpp"

namespace genko::app {

using core::Json;
namespace presets = render::brush_presets;

// --- the brush in hand ----------------------------------------------------------------------------------------------

std::string MainWindow::book_key_for(const std::string& key) const {
    const presets::Preset* preset = presets::builtin_dab_of(key);
    if (preset == nullptr) return key;
    // (the key this book takes for the built-in brush: its own, or _2, _3 … when the book or this process holds a brush of
    // other settings under it — never the other brush written over: review E1 P3-2's way)
    for (int n = 1; n < 1000; ++n) {
        const std::string k = preset->key + (n == 1 ? std::string() : "_" + std::to_string(n));
        const auto held = book().brush_custom.find(k);
        if (held != book().brush_custom.end() && !presets::same_settings(*held, preset->settings)) continue;
        const render::brushes::Brush known = render::brushes::brush(k);
        if (known.key == k && !presets::same_settings(core::brush_to_dict(known), preset->settings)) continue;
        if (known.key != k) {
            try {
                render::brushes::define_brush(k, preset->settings);
            } catch (const std::exception&) {
            }
        }
        return k;
    }
    return preset->key;
}

void MainWindow::choose_brush(const std::string& key, bool keep_tool) {
    if (brush_ == nullptr) return;
    // (E5's promise: with a pixel tool in hand — ぼかし, 覆い焼き, 混合ブラシ… — a dab brush chosen is that tool's ブラシ, as
    // Photoshop's Brushes panel sets the tip of the tool in hand)
    if (!keep_tool && canvas_ != nullptr && pixel_brush_ != nullptr && PageCanvas::is_pixel_tool(canvas_->tool())) {
        const presets::Library lib = presets::load(config_dir());
        if (const presets::Preset* p = presets::find(lib, key); p != nullptr && core::is_dab_brush(p->settings)) {
            pixel_tool_settings.brush = p->key;
            fill_pixel_brushes();
            flash(QStringLiteral("%1 のブラシ: %2").arg(tool_title(canvas_->tool()), QString::fromStdString(p->name)), 2000);
            return;
        }
    }
    std::string kind = key;
    if (presets::builtin_dab_of(key) != nullptr) {
        kind = book_key_for(key);  // (made known to this process under it)
    } else if (presets::is_own(key) && render::brushes::brush(key).key != key) {
        try {  // (one of the library's this process does not know yet)
            render::brushes::register_brushes(render::brushes::load_own_brushes(config_dir()));
        } catch (const std::exception&) {
        }
    }
    if (brush_->kind() != kind) brush_->reload_kinds(kind);
    // (the brush tools take it in hand; the eraser draws with the brush in hand too, so it keeps its tool)
    if (!keep_tool && tool_settings_ != nullptr && canvas_ != nullptr && !PageCanvas::draws_line(canvas_->tool()) &&
        canvas_->tool() != QLatin1String("eraser")) {
        choose_tool(QStringLiteral("pen"));
    }
}

void MainWindow::brushes_changed(std::optional<std::string> select, std::optional<std::string> forgotten) {
    try {
        render::brushes::register_brushes(render::brushes::load_own_brushes(config_dir()));
    } catch (const std::exception&) {
    }
    if (forgotten && !forgotten->empty() && !book().brush_custom.contains(*forgotten)) render::brushes::forget_brush(*forgotten);
    forget_stroke_previews();
    if (brush_ != nullptr) {
        if (select && !select->empty()) {
            brush_->reload_kinds(*select);
        } else {
            brush_->reload_kinds(forgotten && brush_->kind() == *forgotten ? std::optional<std::string>(std::string(core::kDefaultBrush)) : std::nullopt);
        }
    }
    if (brushes_panel_ != nullptr) brushes_panel_->refresh();
    if (brush_settings_panel_ != nullptr) brush_settings_panel_->refresh();
    fill_pixel_brushes();
}

void MainWindow::edit_brush_settings() { edit_brush(); }
void MainWindow::make_brush_settings() { make_brush(); }

BrushSettingsPanel* MainWindow::brush_settings_panel() {
    if (brush_settings_panel_ == nullptr && brush_settings_dock_ != nullptr) {
        // (made when first wanted: the window opens without its many fields)
        QWidget* frame = brush_settings_dock_->widget();
        brush_settings_panel_ = new BrushSettingsPanel(this);
        brush_settings_dock_->setWidget(brush_settings_panel_);
        delete frame;
        brush_settings_panel_->refresh();
    }
    return brush_settings_panel_;
}

void MainWindow::build_brush_preset_docks(QDockWidget* settings_dock, QDockWidget* brushes_dock, QDockWidget* tools_dock) {
    brush_settings_dock_ = settings_dock;
    connect(settings_dock, &QDockWidget::visibilityChanged, this, [this](bool shown) {
        if (shown) brush_settings_panel()->refresh();
    });
    brushes_dock_ = brushes_dock;
    QWidget* frame = brushes_dock->widget();
    brushes_panel_ = new BrushesPanel(this);
    brushes_dock->setWidget(brushes_panel_);
    delete frame;
    connect(brushes_dock, &QDockWidget::visibilityChanged, this, [this](bool shown) {
        if (shown) brushes_panel_->refresh();
    });
    tool_presets_dock_ = tools_dock;
    frame = tools_dock->widget();
    tool_presets_panel_ = new ToolPresetsPanel(this);
    tools_dock->setWidget(tool_presets_panel_);
    delete frame;
    connect(tools_dock, &QDockWidget::visibilityChanged, this, [this](bool shown) {
        if (shown) tool_presets_panel_->refresh();
    });
    // (the library's dab pictures read when a brush needs one: never all at once)
    presets::use_library_pictures(config_dir());
}

// --- 編集 → ブラシを定義… ------------------------------------------------------------------------------------------------

void MainWindow::define_brush_preset(std::optional<QString> name) {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    const presets::Library lib = presets::load(config_dir());
    if (!name) {
        const QString fallback = QString::fromStdString(presets::next_sample_name(lib));
        const auto asked = ask::get_text(this, QStringLiteral("ブラシを定義"), QStringLiteral("ブラシ名"), fallback);
        if (!asked) return;
        name = asked->trimmed().isEmpty() ? fallback : asked->trimmed();
    }
    const std::optional<Json> area = selection_area();
    // (the layer drawn on alone, as Photoshop's: a paint layer's pixels; else the page as it shows)
    const core::Layer* layer = target_layer();
    const bool alone = layer != nullptr && layer->kind == core::LayerKind::Raster;
    try {
        const std::shared_ptr<const core::Document> shown = session_->snapshot();
        const core::Page seen = *page;
        const std::string named = name->toStdString();
        const std::string layer_id = alone ? layer->id : std::string();
        const presets::Defined made = off_ui([shown, seen, area, named, layer_id] {
            return presets::define_from(*shown, seen, area ? &*area : nullptr, layer_id.empty() ? nullptr : &layer_id, named);
        });
        const Json out = presets::save(config_dir(), std::nullopt, made.settings, named, std::nullopt, made.pictures);
        brushes_changed(out.value("key", std::string()));
        flash(QStringLiteral("ブラシ「%1」を定義しました（%2 × %3 画素）").arg(QString::fromStdString(out.value("name", named))).arg(made.width).arg(made.height), 4000);
    } catch (const std::exception& error) {
        ask::warning(this, QStringLiteral("ブラシを定義"), QStringLiteral("ブラシを定義できませんでした: %1").arg(wording::error(QString::fromUtf8(error.what()))));
    }
}

// --- the tools' presets ---------------------------------------------------------------------------------------------

QString MainWindow::tool_in_hand() const { return canvas_ != nullptr ? canvas_->tool() : QString(); }

QString MainWindow::tool_title(const QString& tool) const {
    if (const auto it = tool_actions_.find(tool); it != tool_actions_.end()) return it->second->text().remove(QLatin1Char('&'));
    return tool;
}

Json MainWindow::tool_fields(const QString& tool) const {
    Json out = Json::object();
    QWidget* page = tool_settings_ != nullptr ? tool_settings_->page_for(tool) : nullptr;
    if (page == nullptr) return out;
    for (QWidget* w : page->findChildren<QWidget*>()) {
        const QString name = w->objectName();
        if (name.isEmpty() || name.startsWith(QLatin1String("qt_"))) continue;
        const std::string key = name.toStdString();
        if (auto* box = qobject_cast<QDoubleSpinBox*>(w)) {
            out[key] = box->value();
        } else if (auto* spin = qobject_cast<QSpinBox*>(w)) {
            out[key] = spin->value();
        } else if (auto* slider = qobject_cast<QSlider*>(w)) {
            out[key] = slider->value();
        } else if (auto* check = qobject_cast<QCheckBox*>(w)) {
            out[key] = check->isChecked();
        } else if (auto* combo = qobject_cast<QComboBox*>(w)) {
            const QVariant data = combo->currentData();
            if (data.isValid() && data.canConvert<QString>() && !data.toString().isEmpty()) out[key] = data.toString().toStdString();
        }
    }
    return out;
}

void MainWindow::apply_tool_fields(const QString& tool, const Json& fields) {
    QWidget* page = tool_settings_ != nullptr ? tool_settings_->page_for(tool) : nullptr;
    if (page == nullptr || !fields.is_object()) return;
    for (const auto& [key, value] : fields.items()) {
        QWidget* w = page->findChild<QWidget*>(QString::fromStdString(key));
        if (w == nullptr) continue;
        if (auto* box = qobject_cast<QDoubleSpinBox*>(w); box != nullptr && value.is_number()) {
            box->setValue(value.get<double>());
        } else if (auto* spin = qobject_cast<QSpinBox*>(w); spin != nullptr && value.is_number()) {
            spin->setValue(static_cast<int>(value.get<double>()));
        } else if (auto* slider = qobject_cast<QSlider*>(w); slider != nullptr && value.is_number()) {
            slider->setValue(static_cast<int>(value.get<double>()));
        } else if (auto* check = qobject_cast<QCheckBox*>(w); check != nullptr && value.is_boolean()) {
            check->setChecked(value.get<bool>());
        } else if (auto* combo = qobject_cast<QComboBox*>(w); combo != nullptr && value.is_string()) {
            const int at = combo->findData(QString::fromStdString(value.get<std::string>()));
            if (at >= 0 && at != combo->currentIndex()) {
                combo->setCurrentIndex(at);
                emit combo->activated(at);  // (the fields kept in the settings follow a choice made: as the person's)
            }
        }
    }
}

Json MainWindow::tool_preset_options(const QString& tool, bool include_colour) const {
    Json options{{"fields", tool_fields(tool)}};
    // (a tool that draws with a brush keeps the brush in hand: its key, as Photoshop's tool preset keeps the brush)
    if (brush_ != nullptr && (PageCanvas::draws_line(tool) || tool == QLatin1String("eraser"))) {
        std::string kind = brush_->kind();
        if (const presets::Preset* builtin = presets::builtin_dab_of(kind)) kind = builtin->key;  // (the library's key)
        options["brush"] = kind;
    }
    if (include_colour && brush_ != nullptr) {
        const auto rgb = brush_->rgb();
        options["foreground"] = Json::array({rgb[0], rgb[1], rgb[2]});
    }
    return options;
}

void MainWindow::use_tool_preset(const Json& item) {
    const QString tool = QString::fromStdString(item.value("tool", std::string()));
    if (tool.isEmpty()) return;
    const Json options = item.value("options", Json::object());
    if (tool_actions_.contains(tool) || tool == QLatin1String("marquee") || tool == QLatin1String("shape")) choose_tool(tool);
    if (options.contains("brush") && options["brush"].is_string() && brush_ != nullptr) {
        const std::string key = options["brush"].get<std::string>();
        if (core::find_builtin(key) != nullptr || presets::builtin(key) != nullptr || render::brushes::brush(key).key == key ||
            presets::find(presets::load(config_dir()), key) != nullptr) {
            choose_brush(key, true);  // (the preset's tool kept in hand)
        } else {
            flash(QStringLiteral("このツールプリセットのブラシ（%1）が見つかりません。ほかの設定だけ使います").arg(QString::fromStdString(key)), 4000);
        }
    }
    if (options.contains("fields")) apply_tool_fields(canvas_->tool(), options["fields"]);
    if (options.contains("foreground") && options["foreground"].is_array() && options["foreground"].size() == 3 && brush_ != nullptr) {
        const Json& c = options["foreground"];
        brush_->set_colour({c[0].get<int>(), c[1].get<int>(), c[2].get<int>()});
    }
    flash(QStringLiteral("ツールプリセット「%1」").arg(QString::fromStdString(item.value("name", std::string()))), 2000);
}

// --- 編集 → プリセット ▸ ---------------------------------------------------------------------------------------------

void MainWindow::presets_changed() {
    brushes_changed(std::nullopt);
    if (gradients_ != nullptr) gradients_->reload();
    if (patterns_panel_ != nullptr) patterns_panel_->refresh();
    if (shapes_panel() != nullptr) shapes_panel()->refresh();
    if (tool_presets_panel_ != nullptr) tool_presets_panel_->refresh();
}

void MainWindow::preset_manager() {
    PresetManager dialog(this);
    ask::exec(&dialog);
    presets_changed();
}

Json MainWindow::export_presets(const QString& path, const std::vector<std::string>& kinds, bool builtins) {
    const std::filesystem::path file = core::path_from_utf8(path.toStdString());
    const std::filesystem::path config = config_dir();
    return off_ui([file, config, kinds, builtins] {
        Json counts;
        const std::string text = render::preset_files::export_text(config, kinds, builtins, &counts);
        storage::write_atomic(file, text);
        return counts;
    });
}

Json MainWindow::import_presets(const QString& path, const std::vector<std::string>& kinds) {
    const std::filesystem::path file = core::path_from_utf8(path.toStdString());
    const std::filesystem::path config = config_dir();
    const Json out = off_ui([file, config, kinds] {
        return render::preset_files::import_text(config, storage::read_file_bounded(file, render::preset_files::kMaxFileBytes), kinds);
    });
    presets_changed();
    return out;
}

Json MainWindow::migrate_presets(const QString& from) {
    const std::filesystem::path source = core::path_from_utf8(from.toStdString());
    const std::filesystem::path config = config_dir();
    const Json out = off_ui([source, config] { return render::preset_files::migrate(config, source); });
    presets_changed();
    return out;
}

namespace {

QString counts_text(const Json& counts) {
    static const std::vector<std::pair<std::string, QString>> kinds{{"brushes", QStringLiteral("ブラシ")},
                                                                    {"gradients", QStringLiteral("グラデーション")},
                                                                    {"patterns", QStringLiteral("パターン")},
                                                                    {"shapes", QStringLiteral("シェイプ")},
                                                                    {"tool_presets", QStringLiteral("ツールプリセット")}};
    QStringList parts;
    for (const auto& [k, label] : kinds) {
        if (counts.contains(k)) parts << QStringLiteral("%1 %2").arg(label).arg(counts[k].get<std::int64_t>());
    }
    return parts.join(QStringLiteral("・"));
}

}  // namespace

void MainWindow::export_import_presets_dialog() {
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("export_import_presets"));
    dialog.setWindowTitle(QStringLiteral("プリセットを書き出し／読み込み"));
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(QStringLiteral("書き出す・読み込むプリセット")));
    std::vector<std::pair<std::string, QCheckBox*>> boxes;
    for (const auto& [k, label] : std::vector<std::pair<std::string, QString>>{{"brushes", QStringLiteral("ブラシ")},
                                                                               {"gradients", QStringLiteral("グラデーション")},
                                                                               {"patterns", QStringLiteral("パターン")},
                                                                               {"shapes", QStringLiteral("シェイプ")},
                                                                               {"tool_presets", QStringLiteral("ツールプリセット")}}) {
        auto* box = new QCheckBox(label);
        box->setObjectName(QStringLiteral("preset_kind_") + QString::fromStdString(k));
        box->setChecked(true);
        layout->addWidget(box);
        boxes.emplace_back(k, box);
    }
    auto* builtins = new QCheckBox(QStringLiteral("入っているプリセットも書き出す"));
    builtins->setObjectName(QStringLiteral("preset_builtins"));
    layout->addWidget(builtins);
    auto* buttons = new QDialogButtonBox;
    QPushButton* out_button = buttons->addButton(QStringLiteral("書き出し…"), QDialogButtonBox::ActionRole);
    QPushButton* in_button = buttons->addButton(QStringLiteral("読み込み…"), QDialogButtonBox::ActionRole);
    buttons->addButton(QStringLiteral("閉じる"), QDialogButtonBox::RejectRole);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    const auto kinds = [&boxes] {
        std::vector<std::string> out;
        for (const auto& [k, box] : boxes) {
            if (box->isChecked()) out.push_back(k);
        }
        return out;
    };
    connect(out_button, &QPushButton::clicked, &dialog, [&] {
        if (kinds().empty()) return;
        const QString path = ask::save_path(&dialog, QStringLiteral("プリセットを書き出し"), QStringLiteral("プリセット.genkopresets"),
                                            QStringLiteral("Genko のプリセット (*.genkopresets)"));
        if (path.isEmpty()) return;
        try {
            const Json counts = export_presets(path, kinds(), builtins->isChecked());
            flash(QStringLiteral("プリセットを書き出しました（%1）").arg(counts_text(counts)), 4000);
            dialog.accept();
        } catch (const std::exception& error) {
            ask::warning(&dialog, QStringLiteral("プリセットを書き出し"), QStringLiteral("書き出せませんでした: %1").arg(wording::error(QString::fromUtf8(error.what()))));
        }
    });
    connect(in_button, &QPushButton::clicked, &dialog, [&] {
        if (kinds().empty()) return;
        const QString path = ask::open_path(&dialog, QStringLiteral("プリセットを読み込み"), QStringLiteral("Genko のプリセット (*.genkopresets)"));
        if (path.isEmpty()) return;
        try {
            const Json out = import_presets(path, kinds());
            flash(QStringLiteral("プリセットを読み込みました（%1）").arg(counts_text(out.value("added", Json::object()))), 4000);
            dialog.accept();
        } catch (const std::exception& error) {
            ask::warning(&dialog, QStringLiteral("プリセットを読み込み"), QStringLiteral("読み込めませんでした: %1").arg(wording::error(QString::fromUtf8(error.what()))));
        }
    });
    ask::exec(&dialog);
}

void MainWindow::migrate_presets_dialog() {
    QString from = ask::existing_dir(this, QStringLiteral("プリセットを移行: ほかの Genko の設定フォルダー（キャンセルで書き出したファイルを選ぶ）"));
    if (from.isEmpty()) from = ask::open_path(this, QStringLiteral("プリセットを移行"), QStringLiteral("Genko のプリセット (*.genkopresets)"));
    if (from.isEmpty()) return;
    try {
        const Json out = migrate_presets(from);
        flash(out.value("migrated", std::int64_t{0}) > 0 ? QStringLiteral("プリセットを移行しました（%1）").arg(counts_text(out.value("added", Json::object())))
                                                         : QStringLiteral("移行するプリセット（まだ無いグループ）はありませんでした"),
              4000);
    } catch (const std::exception& error) {
        ask::warning(this, QStringLiteral("プリセットを移行"), QStringLiteral("移行できませんでした: %1").arg(wording::error(QString::fromUtf8(error.what()))));
    }
}

void MainWindow::build_brush_preset_actions() {
    make("act_define_brush", QStringLiteral("ブラシを定義…"), [this] { define_brush_preset(); }, {},
         QStringLiteral("選択範囲（無ければページ全体）の描いた所を、ブラシの先端にしてブラシのライブラリに加えます（暗い所ほど濃く）"));
    make("act_preset_manager", QStringLiteral("プリセットマネージャー…"), [this] { preset_manager(); }, {},
         QStringLiteral("ブラシ・グラデーション・パターン・シェイプ・ツールプリセットの名前・並び・グループを直します"));
    make("act_export_import_presets", QStringLiteral("プリセットを書き出し／読み込み…"), [this] { export_import_presets_dialog(); }, {},
         QStringLiteral("プリセットを 1 つのファイル（.genkopresets）に書き出す・読み込みます（ほかのパソコンへ持っていく）"));
    make("act_migrate_presets", QStringLiteral("プリセットを移行"), [this] { migrate_presets_dialog(); }, {},
         QStringLiteral("ほかの Genko の設定フォルダーか書き出したファイルから、まだ無いグループのプリセットを取り込みます"));
}

void MainWindow::build_brush_preset_menus(QMenu* edit) {
    // 編集: ブラシを定義… before パターンを定義… (Photoshop's ブラシを定義… · パターンを定義… · カスタムシェイプを定義…), and
    // プリセット ▸ right after カスタムシェイプを定義…
    if (QAction* pattern = action("act_define_pattern"); pattern != nullptr && edit->actions().contains(pattern)) {
        edit->insertAction(pattern, action("act_define_brush"));
    } else {
        edit->addAction(action("act_define_brush"));
    }
    auto* presets_menu = new QMenu(QStringLiteral("プリセット"), edit);
    presets_menu->setObjectName(QStringLiteral("menu_presets"));
    for (const char* name : {"act_preset_manager", "act_migrate_presets", "act_export_import_presets"}) presets_menu->addAction(action(QString::fromLatin1(name)));
    const QList<QAction*> in_edit = edit->actions();
    QAction* shape = action("act_define_custom_shape");
    const qsizetype at = shape != nullptr ? in_edit.indexOf(shape) : -1;
    if (at >= 0 && at + 1 < in_edit.size()) {
        edit->insertMenu(in_edit[at + 1], presets_menu);
    } else {
        edit->addMenu(presets_menu);
    }
}

}  // namespace genko::app
