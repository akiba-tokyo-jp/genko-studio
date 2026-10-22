// PC4 E3 (edit.definePattern, layer.newFillLayer.pattern, pattern.presets.select・apply・new, view.patternPreview,
// window.panel.patterns; SPEC.md COLOR-09「パターン」): 編集 → パターンを定義…, the chosen pattern (Photoshop's 今のパターン,
// the window's state), a new pattern fill layer and the ops that put a library pattern into the book first (the ops
// never read the library), 表示 → パターンプレビュー (the page shown repeated around itself), and the Patterns panel's dock.

#include <QDockWidget>
#include <QMenu>
#include <QSettings>

#include "app/ask.hpp"
#include "app/canvas.hpp"
#include "app/config.hpp"
#include "app/main_window.hpp"
#include "app/patterns_panel.hpp"
#include "core/ids.hpp"
#include "core/patterns.hpp"
#include "render/pattern_library.hpp"
#include "render/patterns.hpp"

namespace genko::app {

using core::Json;
namespace library = render::pattern_library;
namespace store = render::preset_store;

QString MainWindow::current_pattern() const {
    const QString kept = settings()->value(QStringLiteral("patterns/current")).toString();
    if (!kept.isEmpty() && pattern_chosen(book(), kept)) return kept;
    // (none chosen, or one gone: the library's first, as Photoshop's)
    const store::Library lib = library::load(config_dir());
    for (const store::Group& g : lib.groups) {
        if (!g.items.empty()) return QStringLiteral("library:") + QString::fromStdString(g.items.front().value("name", std::string()));
    }
    return {};
}

void MainWindow::set_current_pattern(const QString& choice) { settings()->setValue(QStringLiteral("patterns/current"), choice); }

std::optional<Json> MainWindow::pattern_fill_of(const QString& choice, double scale, double angle, bool link, Json& before) {
    before = Json::array();
    const auto chosen = pattern_chosen(book(), choice);
    if (!chosen) return std::nullopt;
    std::string id = chosen->first.id;
    if (id.empty()) {  // (a library pattern: the book's copy of the same picture, else one put in first)
        for (const core::Pattern& kept : book().patterns) {
            if (kept.asset == chosen->first.asset && kept.mode == chosen->first.mode && kept.depth == chosen->first.depth && kept.dpi == chosen->first.dpi) {
                id = kept.id;
                break;
            }
        }
        if (id.empty()) {
            id = core::next_pattern_id(book());
            Json op = library::add_op(chosen->first, chosen->second);
            op["id"] = id;
            before.push_back(std::move(op));
        }
    }
    try {
        return core::pattern_fill_spec(Json{{"id", id}, {"scale", scale}, {"angle", angle}, {"link", link}});
    } catch (const core::Error&) {
        return std::nullopt;
    }
}

void MainWindow::define_pattern(std::optional<QString> name) {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    const store::Library lib_now = library::load(config_dir());
    if (!name) {
        const QString fallback = QString::fromStdString(store::unique_name(lib_now.groups, "パターン 1"));
        const auto asked = ask::get_text(this, QStringLiteral("パターンを定義"), QStringLiteral("パターン名"), fallback);
        if (!asked) return;
        name = asked->trimmed().isEmpty() ? fallback : asked->trimmed();
    }
    const std::optional<Json> area = selection_area();
    try {
        const render::patterns::FromPage made = render::patterns::from_page(book(), *page, area ? &*area : nullptr, name->toStdString());
        store::Library lib = library::load(config_dir());
        const Json out = library::add(config_dir(), lib.groups, made.pattern, made.bytes, std::nullopt);
        library::save(config_dir(), lib.groups);
        set_current_pattern(QStringLiteral("library:") + QString::fromStdString(out.value("name", made.pattern.name)));
        flash(QStringLiteral("パターン「%1」を定義しました（%2 × %3 画素）")
                  .arg(QString::fromStdString(out.value("name", made.pattern.name)))
                  .arg(made.pattern.width)
                  .arg(made.pattern.height),
              4000);
    } catch (const std::exception& error) {
        ask::warning(this, QStringLiteral("パターンを定義"), QStringLiteral("パターンを定義できませんでした: %1").arg(QString::fromUtf8(error.what())));
        return;
    }
    if (patterns_panel_ != nullptr) patterns_panel_->refresh();
}

void MainWindow::new_pattern_layer(const QString& choice) {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    const auto asked = asking();
    PatternFillSettings start;
    start.choice = choice.isEmpty() ? current_pattern() : choice;
    const auto picked = pattern_fill_dialog(this, book(), start, QStringLiteral("パターンで塗りつぶし"));
    if (!picked || !still(asked)) return;
    Json ops;
    const auto spec = pattern_fill_of(picked->choice, picked->scale, picked->angle, picked->link, ops);
    if (!spec) return;
    set_current_pattern(picked->choice);
    const std::string id = core::new_id();
    Json op{{"op", "add_layer"}, {"page", page->index.json()}, {"kind", "fill"}, {"pattern", *spec}, {"name", "パターン"}, {"id", id}};
    if (const core::Layer* layer = target_layer()) op["after"] = layer->id;
    ops.push_back(std::move(op));
    if (apply_ops(ops)) set_target_layer(id);
    if (patterns_panel_ != nullptr) patterns_panel_->refresh();
}

void MainWindow::build_pattern_actions() {
    make("act_define_pattern", QStringLiteral("パターンを定義…"), [this] { define_pattern(); }, {},
         QStringLiteral("選択範囲（無ければページ全体）を、見えているとおりにパターンのライブラリに加えます"));
    make("act_layer_pattern", QStringLiteral("パターンのレイヤー…"), [this] { new_pattern_layer(); }, {},
         QStringLiteral("パターンを敷きつめた塗りつぶしレイヤーを作ります（レイヤー → 新規塗りつぶしレイヤー → パターン…）"));
    QAction* preview = make("act_pattern_preview", QStringLiteral("パターンプレビュー"), [] {}, {},
                            QStringLiteral("ページを上下左右に並べて見せます（パターンにしたときのつながりを見る。原稿は変わりません）"), true);
    connect(preview, &QAction::toggled, this, [this](bool on) {
        canvas_->pattern_preview = on;
        canvas_->update();
    });
}

void MainWindow::build_pattern_menus(QMenu* edit, QMenu* view) {
    // 編集: before カスタムシェイプを定義… (Photoshop's ブラシを定義… · パターンを定義… · カスタムシェイプを定義…)
    QAction* shape = action("act_define_custom_shape");
    const QList<QAction*> in_edit = edit->actions();
    const int at = shape != nullptr ? static_cast<int>(in_edit.indexOf(shape)) : -1;
    if (at >= 0) {
        edit->insertAction(shape, action("act_define_pattern"));
    } else {
        edit->addAction(action("act_define_pattern"));
    }
    // 表示: after ブラシプレビュー
    QAction* brush = action("act_brush_preview");
    const QList<QAction*> in_view = view->actions();
    const int after = brush != nullptr ? static_cast<int>(in_view.indexOf(brush)) : -1;
    if (after >= 0 && after + 1 < in_view.size()) {
        view->insertAction(in_view[after + 1], action("act_pattern_preview"));
    } else {
        view->addAction(action("act_pattern_preview"));
    }
}

}  // namespace genko::app
