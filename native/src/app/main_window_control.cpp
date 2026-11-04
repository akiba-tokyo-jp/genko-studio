// What the control channel does in a window (PC5 J-g, SPEC.md PROD-01「制御チャンネル」; app/control_channel.hpp serves it,
// service/control.hpp holds its rules): look (ui.inspect, ui.screenshot), the view only (ui.set, ui.menu.invoke of the view's
// commands), the book's ops (engine.execute) and the pen (ui.pointer) as the token's actor with an agent's rights — the
// ops MCP's apply_ops takes, its refusals, nothing asked of the person (ask::Closed).

#include <QBuffer>
#include <QDockWidget>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QPixmap>
#include <QStatusBar>
#include <QTabBar>

#include <cmath>

#include "app/ask.hpp"
#include "app/canvas.hpp"
#include "app/main_window.hpp"
#include "app/timeline.hpp"
#include "core/error.hpp"
#include "core/video.hpp"
#include "render/ops_measure.hpp"
#include "service/control.hpp"

namespace genko::app {

namespace rules = service::control;
using core::Json;

namespace {

[[noreturn]] void bad(const std::string& message) { throw core::Error("bad_params", message); }

// A menu's commands as a tree: {menu, items: […]} and {name (attribute), words, on, checked?, invoke}
Json menu_tree(const QMenu* menu) {
    Json items = Json::array();
    for (QAction* act : menu->actions()) {
        if (act->isSeparator() || !act->isVisible()) continue;
        if (act->menu() != nullptr) {
            Json sub = menu_tree(act->menu());
            sub["menu"] = act->menu()->title().remove(QLatin1Char('&')).toStdString();
            items.push_back(std::move(sub));
            continue;
        }
        const QString name = act->property("genko_attribute").toString();
        Json item{{"words", act->text().remove(QLatin1Char('&')).toStdString()}, {"on", act->isEnabled()}};
        if (!name.isEmpty()) item["name"] = name.toStdString();
        if (act->isCheckable()) item["checked"] = act->isChecked();
        item["invoke"] = !name.isEmpty() && rules::view_command(name.toStdString());
        items.push_back(std::move(item));
    }
    return Json{{"items", std::move(items)}};
}

}  // namespace

Json MainWindow::control_inspect() const {
    Json out = Json::object();
    out["tool"] = canvas_->tool().toStdString();
    out["pen_tools"] = Json::array({"pen", "pencil", "eraser"});
    const core::Page* page = current_page();
    const core::Document& b = book();
    out["page"] = page != nullptr ? Json{{"page", page->index.json()}, {"id", page->id}, {"position", page_index_}, {"pages", b.pages.size()}} : Json();
    out["frame"] = current_frame(page);
    out["zoom"] = Json{{"percent", std::round(canvas_->zoom_percent_exact() * 100.0) / 100.0}, {"rotation", canvas_->rotation()}};
    out["read_only"] = session_->read_only_reason();
    out["book"] = Json{{"title", b.title}, {"revision", b.revision}};
    Json panels = Json::array();
    for (QDockWidget* dock : findChildren<QDockWidget*>()) {
        if (!rules::view_panel(dock->objectName().toStdString())) continue;
        panels.push_back(Json{{"name", dock->objectName().toStdString()}, {"shown", dock->isVisible()}});
    }
    out["panels"] = std::move(panels);
    Json open = Json::array();  // (the books open in this window: their titles alone, never a path)
    for (std::size_t i = 0; i < documents_.size(); ++i) {
        open.push_back(Json{{"title", documents_[i].title().toStdString()}, {"front", static_cast<int>(i) == doc_}});
    }
    out["books"] = std::move(open);
    out["window"] = windowTitle().toStdString();
    Json menus = Json::array();
    for (QAction* top : menuBar()->actions()) {
        if (top->menu() == nullptr || !top->isVisible()) continue;
        Json m = menu_tree(top->menu());
        m["menu"] = top->menu()->title().remove(QLatin1Char('&')).toStdString();
        menus.push_back(std::move(m));
    }
    out["menus"] = std::move(menus);
    if (page != nullptr) {
        try {
            out["measure"] = render::measure::listing(*page);
        } catch (const std::exception&) {
            out["measure"] = Json();
        }
    }
    return out;
}

Json MainWindow::control_set(const Json& params) {
    static const std::vector<std::string> kKeys{"page", "frame", "zoom", "center", "fit", "tool", "panels"};
    for (const auto& [key, value] : params.items()) {
        if (std::find(kKeys.begin(), kKeys.end(), key) == kKeys.end()) bad("ui.set が変えるのは page・frame・zoom・center・fit・tool・panels だけ: " + key);
    }
    // (every value checked before anything changes)
    std::optional<int> page;
    if (params.contains("page")) {
        const Json& v = params["page"];
        if (!v.is_number_integer()) bad("page はページの番号（op の page と同じ）");
        bool found = false;
        for (const auto& p : book().pages) found = found || p->index.json() == v;
        if (!found) bad("このページはありません: " + v.dump());
        page = static_cast<int>(v.get<std::int64_t>());
    }
    if (params.contains("frame") && (!params["frame"].is_number_integer() || params["frame"].get<std::int64_t>() < 1 || params["frame"].get<std::int64_t>() > 3000)) {
        bad("frame は 1〜3000 の整数");
    }
    if (params.contains("zoom") && (!params["zoom"].is_number() || !(params["zoom"].get<double>() >= 1.0 && params["zoom"].get<double>() <= 12800.0))) {
        bad("zoom は 1〜12800（%、100 でページの画素が等倍）");
    }
    if (params.contains("center")) {
        const Json& c = params["center"];
        if (!c.is_array() || c.size() != 2 || !c[0].is_number() || !c[1].is_number() || !std::isfinite(c[0].get<double>()) || !std::isfinite(c[1].get<double>())) {
            bad("center は [x, y]（mm）");
        }
    }
    if (params.contains("fit") && params["fit"] != Json("page")) bad("fit は \"page\"（ページを画面に合わせる）");
    QString tool;
    if (params.contains("tool")) {
        if (!params["tool"].is_string()) bad("tool は道具の名前（ui.inspect の tool の形）");
        tool = QString::fromStdString(params["tool"].get<std::string>());
        if (tool_actions().find(tool) == tool_actions().end()) bad("この道具はありません: " + tool.toStdString());
    }
    std::vector<std::pair<QDockWidget*, bool>> panels;
    if (params.contains("panels")) {
        if (!params["panels"].is_object()) bad("panels は {名前: true | false}");
        for (const auto& [name, shown] : params["panels"].items()) {
            if (!shown.is_boolean()) bad("panels の値は true か false: " + name);
            QDockWidget* dock = nullptr;
            if (rules::view_panel(name)) {
                for (QDockWidget* d : findChildren<QDockWidget*>()) {
                    if (d->objectName().toStdString() == name) dock = d;
                }
            }
            if (dock == nullptr) bad("このパネルは制御チャンネルから出し入れできません: " + name);
            panels.emplace_back(dock, shown.get<bool>());
        }
    }
    const ask::Closed closed;  // (a view's change never asks)
    if (page) go_to_page(*page);
    if (params.contains("frame") && timeline_ != nullptr) timeline_->set_frame(params["frame"].get<std::int64_t>());
    if (params.contains("fit")) canvas_->fit_page();
    if (params.contains("zoom")) canvas_->set_zoom_percent(params["zoom"].get<double>());
    if (params.contains("center")) canvas_->center_on(params["center"][0].get<double>(), params["center"][1].get<double>());
    if (!tool.isEmpty()) choose_tool(tool);
    for (const auto& [dock, shown] : panels) {
        dock->setVisible(shown);
        if (shown) dock->raise();
    }
    const Json now = control_inspect();
    return Json{{"tool", now["tool"]}, {"page", now["page"]}, {"frame", now["frame"]}, {"zoom", now["zoom"]}, {"panels", now["panels"]}};
}

Json MainWindow::control_menu(const QString& attribute) {
    if (!rules::view_command(attribute.toStdString())) {
        throw core::Error("not_allowed", "制御チャンネルから選べるのは表示の命令だけ（ui.inspect の menus で invoke が true のもの）: " + attribute.toStdString());
    }
    QAction* act = action(attribute);
    if (act == nullptr) throw core::Error("not_allowed", "この窓にこの命令はありません: " + attribute.toStdString());
    if (!act->isEnabled()) throw core::Error("not_enabled", "この命令は今は使えません: " + act->text().toStdString());
    {
        const ask::Closed closed;
        act->trigger();
    }
    Json out{{"name", attribute.toStdString()}, {"words", act->text().toStdString()}};
    if (act->isCheckable()) out["checked"] = act->isChecked();
    return out;
}

QByteArray MainWindow::control_screenshot(bool whole_window, int max_side) const {
    QPixmap shot = whole_window ? const_cast<MainWindow*>(this)->grab() : canvas_->grab();
    if (shot.isNull()) return {};
    if (max_side > 0 && std::max(shot.width(), shot.height()) > max_side) shot = shot.scaled(max_side, max_side, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    if (!shot.save(&buffer, "PNG")) return {};
    return bytes;
}

Json MainWindow::control_execute(const Json& ops, const std::string& actor) {
    if (const auto refused = rules::refused_ops(ops)) throw core::Error(refused->code, refused->message);
    if (canvas_->control_drawing()) throw core::Error("busy", "制御チャンネルのペンが描いている間は当てられません");
    const ask::Closed closed;
    const core::PublicAnimationOps public_ops;  // (an agent's set_animation as MCP's apply_ops has it: J-e1)
    const core::ApplyResult result = session_->apply_as(ops, rules::actor_of(actor));
    Json out{{"applied", result.applied}, {"warnings", result.has_warnings ? result.warnings : Json::array()}, {"revision", book().revision}};
    if (!result.results.empty()) out["results"] = result.results;
    return out;
}

Json MainWindow::control_pen(const Json& events, const std::string& actor, bool& holding) {
    holding = canvas_->control_drawing();
    Json out = Json::object();
    std::size_t done = 0;
    try {
        for (const Json& e : events) {
            if (!e.is_object() || !e.value("kind", Json()).is_string()) bad("events の各項目は {kind: down | move | up, x, y, pressure?}");
            const std::string kind = e["kind"].get<std::string>();
            const Json x = e.value("x", Json()), y = e.value("y", Json());
            const Json pressure = e.value("pressure", Json(0.7));
            const bool placed = x.is_number() && y.is_number() && std::isfinite(x.get<double>()) && std::isfinite(y.get<double>());
            if (!pressure.is_number() || !(pressure.get<double>() >= 0.0 && pressure.get<double>() <= 1.0)) bad("pressure は 0〜1");
            const QPointF mm = placed ? QPointF(x.get<double>(), y.get<double>()) : QPointF();
            if (kind == "down") {
                if (!placed) bad("down には x・y（前のページの mm）");
                if (canvas_->control_drawing()) throw core::Error("busy", "線を描いている途中です（up で離す）");
                if (current_page() == nullptr) throw core::Error("refused", "前にページがありません");
                if (!session_->read_only_reason().empty()) throw core::Error("read_only", "この原稿は読み取り専用です");
                if (!rules::pen_tool(canvas_->tool().toStdString())) {
                    throw core::Error("not_allowed", "制御チャンネルのペンで使える道具はペン・鉛筆・消しゴムだけ（ui.set {tool} で持ち替える）: 今は " +
                                                         canvas_->tool().toStdString());
                }
                if (effect_shape_for_ || text_path_for_) throw core::Error("busy", "人が始めた操作の途中です（効果線の形・文字のパス）");
                if (!canvas_->control_press(mm, pressure.get<double>())) throw core::Error("busy", "キャンバスが線を受けられません（人が描いている途中など）");
            } else if (kind == "move") {
                if (!placed) bad("move には x・y");
                if (!canvas_->control_drawing()) bad("move は down の後に");
                canvas_->control_move(mm, pressure.get<double>());
            } else if (kind == "up") {
                if (!canvas_->control_drawing()) bad("up は down の後に");
                if (placed) canvas_->control_move(mm, pressure.get<double>());
                control_actor_ = rules::actor_of(actor);
                control_refused_.clear();
                control_applied_ = false;
                {
                    const ask::Closed closed;  // (the pen's line never brings a dialog: one it would ask is refused)
                    canvas_->control_release();
                }
                control_actor_.reset();
                if (!control_applied_) {
                    holding = false;
                    if (!control_refused_.isEmpty()) throw core::Error("refused", control_refused_.toStdString());
                    throw core::Error("refused", "線は原稿に入りませんでした（描くレイヤーが無い・人に聞くことがあった など）");
                }
                out["applied"] = true;
            } else {
                bad("kind は down・move・up");
            }
            ++done;
        }
    } catch (...) {
        control_actor_.reset();
        holding = canvas_->control_drawing();
        throw;
    }
    holding = canvas_->control_drawing();
    out["events"] = done;
    out["drawing"] = holding;
    return out;
}

void MainWindow::control_pen_cancel() { canvas_->control_cancel(); }

void MainWindow::control_state(const QString& words, bool drawing) {
    if (control_label_ == nullptr) {
        if (words.isEmpty()) return;
        control_label_ = new QLabel(this);
        control_label_->setObjectName(QStringLiteral("controlChannelState"));
        statusBar()->addPermanentWidget(control_label_);
    }
    control_label_->setText(drawing ? QStringLiteral("<b>%1</b>").arg(words.toHtmlEscaped()) : words.toHtmlEscaped());
    control_label_->setToolTip(QStringLiteral("制御チャンネル（環境設定 → 連携）。描いている間は、この窓の操作を止めています（Esc で止める）"));
    control_label_->setVisible(!words.isEmpty());
}

}  // namespace genko::app
