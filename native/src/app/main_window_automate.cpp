// ファイル → 自動処理 ▸・スクリプト ▸ and the steps run in the window (PC5 I-0, SPEC.md AUTO-01: the frame the later groups
// fill — I-e バッチ・ドロップレット・画像プロセッサー・スクリプトイベント・参照…, I-a/I-f/I-g/I-h their commands; the rules of
// service/automate.hpp and service/script_events.hpp).

#include <QAction>
#include <QMenu>

#include "app/actions.hpp"
#include "app/main_window.hpp"
#include "core/actor.hpp"
#include "service/automate.hpp"
#include "service/script_events.hpp"
#include "service/wording.hpp"

namespace genko::app {

using core::Json;

void MainWindow::build_automate_actions() {
    // (the later groups make their commands here: make("act_batch", …), make("act_script_browse", …) …)
}

void MainWindow::build_automate_menus() {
    // (the later groups put their commands in automate_menu_ and scripts_menu_ here, in Photoshop's order)
    automate_menu_->menuAction()->setVisible(!automate_menu_->actions().isEmpty());  // (shown once it holds one)
}

bool MainWindow::steps_allowed(const Json& ops, const QString& label, const QString& hint) {
    try {
        service::automate::check_steps(ops, core::Actor(session_->actor()));
    } catch (const core::Error& error) {
        flash(QStringLiteral("「%1」: %2%3").arg(label, wording::error(std::string(error.what())), hint), 8000, true);
        return false;
    }
    return true;
}

bool MainWindow::run_steps(const Json& steps, bool recorded, const QString& label) {
    const core::Page* page = current_page();
    if (page == nullptr) {
        flash(QStringLiteral("原稿を開いていないので、「%1」を流しませんでした").arg(label), 6000, true);
        return false;
    }
    Json ops = steps;
    if (recorded) {  // (an オートアクション: on the page in front, its layer drawn on and its chosen panel, as 実行 plays it)
        const core::Layer* layer = target_layer();
        const core::Frame* frame = selected_frame();
        ops = actions::replay(steps, page->index.json(), layer != nullptr ? std::optional<std::string>(layer->id) : std::nullopt,
                              frame != nullptr ? std::optional<std::string>(frame->id) : std::nullopt);
    }
    const core::Actor actor(session_->actor());
    if (!steps_allowed(ops, label)) return false;
    // (the journal names each change by the steps; an オートアクション being recorded does not take them in; both set
    // back whatever happens)
    struct Named {
        Session& session;
        std::optional<Json>& recording;
        std::optional<Json> kept;
        ~Named() {
            session.set_script({});
            recording = std::move(kept);
        }
    } named{*session_, recording_, std::move(recording_)};
    recording_.reset();
    session_->set_script(label.toStdString());
    Json pending = Json::array();
    const auto flush = [&]() {
        if (pending.empty()) return true;
        const bool ok = apply_ops(pending);
        pending = Json::array();
        return ok;
    };
    for (const Json& op : ops) {
        const auto name = op.find("op");
        if (name == op.end() || !name->is_string() || !service::automate::pseudo_op(name->get<std::string>())) {
            pending.push_back(op);
            continue;
        }
        if (!flush()) return false;
        Json made;
        try {
            made = service::automate::expand_pseudo(book(), op);
            service::automate::check_steps(made, actor);
        } catch (const std::exception& error) {
            flash(QStringLiteral("「%1」: %2").arg(label, wording::error(std::string(error.what()))), 6000, true);
            return false;
        }
        pending = std::move(made);
        if (!flush()) return false;
    }
    return flush();
}

void MainWindow::script_event(service::script_events::Event event) {
    namespace events = service::script_events;
    if (events::Firing::active()) return;  // (the steps an event runs raise none)
    const events::Loaded loaded = events::load();
    if (loaded.problem && !script_events_told_) {
        script_events_told_ = true;
        flash(QString::fromStdString(*loaded.problem), 8000, true);
    }
    const std::vector<events::Binding> bound = events::bound_to(loaded.table, event);
    if (bound.empty()) return;
    const events::Firing firing;
    for (const events::Binding& binding : bound) {
        try {
            const events::Steps steps = events::steps_of(binding, event);
            if (!run_steps(steps.steps, steps.recorded, QString::fromStdString(steps.label))) break;
        } catch (const std::exception& error) {
            flash(QStringLiteral("スクリプトイベント「%1」: %2").arg(QString::fromStdString(binding.name), wording::error(std::string(error.what()))), 6000, true);
            break;
        }
    }
}

}  // namespace genko::app
