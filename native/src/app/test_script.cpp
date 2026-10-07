#include "app/test_script.hpp"

#if GENKO_FAULT_INJECTION

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QPushButton>
#include <QTimer>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "app/ask.hpp"
#include "app/canvas.hpp"
#include "app/dialogs.hpp"
#include "app/documents.hpp"
#include "app/inject.hpp"
#include "app/main_window.hpp"
#include "app/pages_panel.hpp"
#include "app/save_status.hpp"
#include "core/json.hpp"
#include "core/paths.hpp"
#include "storage/fault.hpp"

namespace genko::app::test_script {

namespace {

namespace fs = std::filesystem;
using core::Json;

const char* kind_name(SaveKind kind) {
    switch (kind) {
    case SaveKind::Saved:
        return "saved";
    case SaveKind::Dirty:
        return "dirty";
    case SaveKind::Saving:
        return "saving";
    case SaveKind::Failed:
        return "failed";
    case SaveKind::RecoveryOnly:
        return "recovery_only";
    }
    return "?";
}

MainWindow* front_window() {
    if (auto* active = qobject_cast<MainWindow*>(QApplication::activeWindow()); active != nullptr && !active->closed()) return active;
    const auto list = documents::windows();
    return list.empty() ? nullptr : list.front();
}

bool same_place(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    return fs::weakly_canonical(fs::absolute(a, ec), ec) == fs::weakly_canonical(fs::absolute(b, ec), ec);
}

Json state_of(MainWindow* w) {
    Json r = Json::object();
    r["windows"] = documents::windows().size();
    if (w == nullptr) return r;
    const Session& s = w->session();
    const SaveStatus status = s.status();
    const core::Document& book = s.document();
    r["path"] = s.path() ? Json(core::path_to_utf8(*s.path())) : Json(nullptr);
    r["title"] = book.title;
    r["book_id"] = book.book_id;
    r["revision"] = s.base_revision();
    r["generation"] = s.generation();
    r["status"] = kind_name(status.kind);
    r["status_words"] = state_words(status).toStdString();
    r["unsaved"] = s.unsaved();
    r["can_undo"] = s.can_undo();
    r["can_redo"] = s.can_redo();
    r["loading"] = s.loading();
    r["pages"] = book.pages.size();
    r["page_index"] = w->page_index();
    r["read_only"] = s.read_only_reason();
    r["documents"] = w->documents().size();
    r["last_notice"] = w->last_notice().toStdString();
    r["last_error"] = w->last_error().toStdString();
    Json layers = Json::array();
    Json lines = Json::array();
    Json frames = Json::array();
    if (const core::Page* page = w->current_page()) {
        r["page_id"] = page->id;
        for (const core::Layer& layer : page->layers) {
            Json ids = Json::array();
            for (const auto& stroke : layer.strokes->items) {
                ids.push_back(stroke->id);
                if (layer.role != core::LayerRole::Ink) continue;
                Json points = Json::array();
                for (const core::PointF& p : stroke->points) points.push_back(Json::array({p.x, p.y}));
                lines.push_back(Json::object({{"id", stroke->id}, {"points", points}, {"pressure", stroke->pressure}, {"kind", stroke->kind},
                                              {"width_mm", stroke->width_mm}}));
            }
            layers.push_back(Json::object({{"id", layer.id}, {"role", std::string(core::to_string(layer.role))}, {"strokes", ids}}));
        }
        for (const core::Frame* frame : page->leaf_frames()) frames.push_back(Json::object({{"id", frame->id}, {"rect", core::rect_to_json(frame->rect)}}));
    }
    r["layers"] = layers;
    r["lines"] = lines;    // the ink's lines on the page shown (points in mm, pressures)
    r["frames"] = frames;  // its panels (rect: x, y, width, height in mm)
    return r;
}

class Runner : public QObject {
public:
    Runner(Json steps, const QString& log) : QObject(qApp), steps_(std::move(steps)), log_(log) {
        if (!log.isEmpty() && !log_.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            qWarning("GENKO_TEST_SCRIPT: cannot write the log %s", qPrintable(log));
        }
        answers(Json::object());  // (no real dialog ever waits for a person: everything is answered "no" until told)
    }

    void begin() { QTimer::singleShot(0, this, [this] { step(); }); }

private:
    enum class State { Done, Wait, Fail };
    struct Outcome {
        State state = State::Done;
        Json data = Json::object();
        std::string error;
    };

    void log(const Json& line) {
        if (!log_.isOpen()) return;
        log_.write(QByteArray::fromStdString(line.dump() + "\n"));
        log_.flush();
    }

    void fail(const std::string& error) {
        log(Json::object({{"step", index_}, {"do", index_ < steps_.size() ? steps_[index_].value("do", "") : ""}, {"ok", false}, {"error", error}}));
        failed_ = true;
        QCoreApplication::exit(70);
    }

    void finish() {
        if (finished_) return;
        finished_ = true;
        log(Json::object({{"done", true}}));
    }

    void step() {
        if (failed_ || finished_) return;
        if (index_ >= steps_.size()) {
            finish();
            return;
        }
        const Json& s = steps_[index_];
        if (!started_) {
            clock_.start();
            started_ = true;
        }
        Outcome outcome;
        try {
            outcome = run(s);
        } catch (const std::exception& error) {
            fail(error.what());
            return;
        }
        if (outcome.state == State::Fail) {
            fail(outcome.error);
            return;
        }
        if (outcome.state == State::Wait) {
            if (clock_.elapsed() > s.value("ms", 30000)) {
                fail("timed out");
                return;
            }
            QTimer::singleShot(20, this, [this] { step(); });
            return;
        }
        Json line = Json::object({{"step", index_}, {"do", s.value("do", "")}, {"ok", true}});
        for (const auto& [key, value] : outcome.data.items()) line[key] = value;
        log(line);
        ++index_;
        started_ = false;
        if (s.value("do", "") == "quit" && documents::windows().empty()) {
            finish();  // (the app ends now)
            return;
        }
        QTimer::singleShot(0, this, [this] { step(); });
    }

    void answers(const Json& s) {
        auto r = std::make_shared<ask::Responder>();
        const std::string close = s.value("close", "cancel");
        const bool yes = s.value("question", false);
        const Json questions = s.value("questions", Json::object());
        const QString save_path = QString::fromStdString(s.value("save_path", ""));
        r->question = [this, yes, questions](const QString& title, const QString& text) {
            const bool answer = questions.contains(title.toStdString()) ? questions[title.toStdString()].get<bool>() : yes;
            log(Json::object({{"asked", title.toStdString()}, {"text", text.toStdString()}, {"answer", answer}}));
            return answer;
        };
        r->warning = [this](const QString& title, const QString& text) {
            log(Json::object({{"warned", title.toStdString()}, {"text", text.toStdString()}}));
        };
        r->save_path = [this, save_path](const QString& caption, const QString&) {
            log(Json::object({{"asked", caption.toStdString()}, {"answer", save_path.toStdString()}}));
            return save_path;
        };
        r->existing_dir = [](const QString&, const QString&) { return QString(); };
        r->get_double = [](const QString&, const QString&, double, double, double, int) { return std::optional<double>(); };
        r->get_int = [](const QString&, const QString&, int, int, int) { return std::optional<int>(); };
        r->get_text = [](const QString&, const QString&, const QString&) { return std::optional<QString>(); };
        r->colour = [](const QColor&, const QString&) { return std::optional<QColor>(); };
        r->exec = [this, close](QDialog* dialog) -> int {
            if (auto* guard = qobject_cast<CloseGuard*>(dialog)) {
                log(Json::object({{"asked", "close"}, {"buttons", Json::array({guard->save_button()->text().toStdString(),
                                                                             guard->save_as_button()->text().toStdString(),
                                                                             guard->discard_button()->text().toStdString(),
                                                                             guard->cancel_button()->text().toStdString()})},
                                  {"default", guard->cancel_button()->isDefault() ? "cancel" : "other"},
                                  {"answer", close}}));
                QPushButton* button = close == "save"      ? guard->save_button()
                                      : close == "save_as" ? guard->save_as_button()
                                      : close == "discard" ? guard->discard_button()
                                                           : guard->cancel_button();
                button->click();
                return guard->result();
            }
            log(Json::object({{"asked", dialog->metaObject()->className()}, {"answer", "reject"}}));
            return QDialog::Rejected;
        };
        ask::set_responder(std::move(r));
    }

    Outcome run(const Json& s) {
        const std::string what = s.value("do", "");
        Outcome out;
        if (what == "answers") {
            answers(s);
            return out;
        }
        if (what == "fault") {
            storage::fault::set_for_testing(s.value("set", ""));
            return out;
        }
        if (what == "sleep") {
            if (clock_.elapsed() < s.value("ms", 0)) out.state = State::Wait;
            return out;
        }
        if (what == "start_dialog") {
            auto* start = qobject_cast<StartDialog*>(QApplication::activeModalWidget());
            if (start == nullptr) {
                out.state = State::Wait;
                return out;
            }
            out.data["cards"] = start->cards().size();
            out.data["size"] = Json::array({start->width(), start->height()});
            if (s.contains("choose")) {
                start->chosen = core::path_from_utf8(s["choose"].get<std::string>());
                start->accept();
            } else {
                start->close_button()->click();
            }
            return out;
        }
        MainWindow* w = front_window();
        if (what == "quit") {
            QApplication::closeAllWindows();
            out.data["windows_left"] = documents::windows().size();
            return out;
        }
        if (w == nullptr) {
            out.state = State::Wait;
            return out;
        }
        if (what == "wait_book") {
            const auto& path = w->session().path();
            const bool there = path && (!s.contains("path") || same_place(*path, core::path_from_utf8(s["path"].get<std::string>())));
            if (!w->isVisible() || !there || w->canvas()->page() == nullptr) out.state = State::Wait;
            return out;
        }
        if (what == "wait_read") {
            // (the book's pages all read: a window shows the first page first and reads the others meanwhile)
            if (w->session().loading()) out.state = State::Wait;
            return out;
        }
        if (what == "report") {
            out.data = state_of(w);
            out.data["tag"] = s.value("tag", "");
            return out;
        }
        if (what == "tool") {
            w->choose_tool(QString::fromStdString(s.value("name", "pen")));
            return out;
        }
        if (what == "action") {
            QAction* act = w->action(QString::fromStdString(s.value("name", "")));
            if (act == nullptr) {
                out.state = State::Fail;
                out.error = "no such action: " + s.value("name", "");
                return out;
            }
            act->trigger();
            return out;
        }
        if (what == "page") {
            w->pages()->setCurrentRow(s.value("row", 0));
            return out;
        }
        if (what == "stroke") {
            const Json& points = s.at("points");
            if (s.value("device", "mouse") == "tablet") {
                std::vector<inject::PenInput> pen;
                const Json tilt = s.value("tilt", Json::array({0, 0}));
                for (const Json& p : points) {
                    inject::PenInput input;
                    input.mm = QPointF(p[0].get<double>(), p[1].get<double>());
                    input.pressure = p.size() > 2 ? p[2].get<double>() : 1.0;
                    input.x_tilt = tilt[0].get<double>();
                    input.y_tilt = tilt[1].get<double>();
                    input.rotation = s.value("rotation", 0.0);
                    pen.push_back(input);
                }
                inject::tablet_stroke(w->canvas(), pen, s.value("eraser_end", false), true);
            } else {
                std::vector<QPointF> mm;
                for (const Json& p : points) mm.emplace_back(p[0].get<double>(), p[1].get<double>());
                inject::mouse_stroke(w->canvas(), mm, Qt::NoModifier, true);
            }
            return out;
        }
        if (what == "wait_saved") {
            if (w->session().status().kind != SaveKind::Saved) out.state = State::Wait;
            return out;
        }
        if (what == "wait_status") {
            if (kind_name(w->session().status().kind) != s.value("kind", "saved")) out.state = State::Wait;
            return out;
        }
        if (what == "ops") {
            // (through the window, as its commands do: the session, the CommandBus, the Saver)
            if (!w->apply_ops(s.at("ops"))) {
                out.state = State::Fail;
                out.error = "the ops were refused: " + w->last_error().toStdString();
            }
            return out;
        }
        if (what == "exit_now") {
            // the process ends here, as when it is killed: nothing more is saved, no question asked
            log(Json::object({{"step", index_}, {"do", "exit_now"}, {"ok", true}}));
            log_.close();
            std::_Exit(s.value("code", 0));
        }
        if (what == "wait_rendered") {
            if (!w->canvas()->renderer().settled()) out.state = State::Wait;
            return out;
        }
        out.state = State::Fail;
        out.error = "unknown step: " + what;
        return out;
    }

    Json steps_;
    std::size_t index_ = 0;
    QFile log_;
    QElapsedTimer clock_;
    bool started_ = false;
    bool failed_ = false;
    bool finished_ = false;
};

}  // namespace

bool available() { return true; }

bool start() {
    const QString file = qEnvironmentVariable("GENKO_TEST_SCRIPT");
    if (file.isEmpty()) return false;
    QFile in(file);
    if (!in.open(QIODevice::ReadOnly)) {
        qWarning("GENKO_TEST_SCRIPT: cannot read %s", qPrintable(file));
        return false;
    }
    Json script = Json::parse(in.readAll().toStdString(), nullptr, false);
    if (!script.is_object() || !script.contains("steps") || !script["steps"].is_array()) {
        qWarning("GENKO_TEST_SCRIPT: %s is not {\"steps\": [...]}", qPrintable(file));
        return false;
    }
    auto* runner = new Runner(script["steps"], QString::fromStdString(script.value("log", "")));
    runner->begin();
    return true;
}

}  // namespace genko::app::test_script

#else

namespace genko::app::test_script {

bool available() { return false; }
bool start() { return false; }  // (a release build never runs test scripts)

}  // namespace genko::app::test_script

#endif
