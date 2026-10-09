// The lines in the window (M4①c, Python's main.py): テキスト (act_text) and 台詞を入れる, フキダシを手で描く, the chosen
// line typed over in place, deleted, turned vertical or across, its balloon's shape; ストーリーエディター and 台詞の検索・
// 置換; the 台詞 panel (story_panel.cpp) and the text tool's ツールの設定; what the canvas's lines become — a line typed
// where the text tool clicked (_type_new_line), a balloon drawn by hand (_balloon_drawn), a balloon moved, resized,
// turned or its tails dragged (lineGeometry, _on_text_moved), typed over (_edit_line_inline) — and a balloon's own
// right-click menu (_line_menu). Every change is an op through apply_ops.

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDockWidget>
#include <QFile>
#include <QMenu>
#include <QScrollArea>
#include <QVBoxLayout>

#include <algorithm>

#include "app/ask.hpp"
#include "app/canvas.hpp"
#include "app/ime.hpp"
#include "app/lettering.hpp"
#include "app/main_window.hpp"
#include "app/story_editor.hpp"
#include "app/story_panel.hpp"
#include "app/tool_settings.hpp"
#include "app/wording.hpp"
#include "core/base64.hpp"
#include "core/error.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "render/image.hpp"
#include "render/png.hpp"
#include "render/text/fonts.hpp"
#include "render/text/lettering.hpp"

namespace genko::app {

using core::Json;
using core::Num;

namespace {

Json num_point(const Num& x, const Num& y) { return Json::array({core::py_round(x, 2).json(), core::py_round(y, 2).json()}); }

std::vector<std::string> story_ids(const core::Document& book) {
    std::vector<std::string> ids;
    for (const core::StoryLine& line : book.story) ids.push_back(line.id);
    return ids;
}

// the id of a line the book has now that it did not have before
std::optional<std::string> added_line(const core::Document& book, const std::vector<std::string>& before) {
    for (const core::StoryLine& line : book.story) {
        if (std::find(before.begin(), before.end(), line.id) == before.end()) return line.id;
    }
    return std::nullopt;
}

}  // namespace

// --- the commands ----------------------------------------------------------------------------------------------------

void MainWindow::build_line_actions() {
    make("act_text", QStringLiteral("テキスト"), [this] { choose_tool(QStringLiteral("text")); }, {QKeySequence(QStringLiteral("T"))},
         QStringLiteral("クリックした所に台詞を入力します（縦書き）"), true);
    QAction* text = action("act_text");
    tools_->addAction(text);
    text->setAutoRepeat(false);  // (a held key chooses the tool once)
    tool_actions_[QStringLiteral("text")] = text;
    make("act_story_editor", QStringLiteral("ストーリーエディター…"), [this] { open_story_editor(); }, {QKeySequence(QStringLiteral("Ctrl+Shift+L"))},
         QStringLiteral("全ページの台詞をまとめて直す・台本を流し込む"));
    make("act_replace", QStringLiteral("台詞の検索・置換…"), [this] { replace_dialog(); }, {QKeySequence(QStringLiteral("Ctrl+Alt+F"))},
         QStringLiteral("全ページの台詞から言葉を探して置き換えます"));
    make("act_line_type", QStringLiteral("台詞を入れる（テキストの道具）"), [this] { choose_tool(QStringLiteral("text")); });
    QAction* pen = make("act_balloon_pen", QStringLiteral("フキダシを手で描く"), [] {}, {}, QStringLiteral("ドラッグで囲んだ形のフキダシに台詞を入れます"), true);
    connect(pen, &QAction::triggered, this, [this](bool on) {
        choose_tool(QStringLiteral("text"));
        text_settings_->draw_balloon->setChecked(on);
    });
    make("act_line_edit", QStringLiteral("選んだ台詞をその場で直す"), [this] { edit_selected_line(); }, {QKeySequence(QStringLiteral("F2"))});
    make("act_line_delete", QStringLiteral("選んだ台詞を消す"), [this] { delete_selected_line(); });
    make("act_line_wrap", QStringLiteral("縦書き・横書きを切り替える"), [this] { toggle_selected_wrap(); });

    // the text tool's settings, and the lines panel (its style box goes beside the select tool: build_tool_settings)
    text_settings_ = new TextToolSettings;
    connect(text_settings_->draw_balloon, &QCheckBox::toggled, this, [this](bool on) {
        canvas_->balloon_pen = on;
        action("act_balloon_pen")->setChecked(on);
    });
    story_ = new StoryPanel(this);
    lines_timer_.setSingleShot(true);
    lines_timer_.setInterval(250);  // (Python's _dock_timer: the side panels follow an edit a moment later)
    connect(&lines_timer_, &QTimer::timeout, this, [this] { refresh_lines(); });

    // what the canvas's lines become
    connect(canvas_, &PageCanvas::textRequested, this, &MainWindow::type_new_line);
    connect(canvas_, &PageCanvas::balloonDrawn, this, &MainWindow::balloon_drawn);
    connect(canvas_, &PageCanvas::lineSelected, this, [this](const QString& id, bool open_panel) { on_line_selected(id.toStdString(), open_panel); });
    connect(canvas_, &PageCanvas::lineGeometry, this, [this](const QString& id, const Json& change) {
        Json op{{"op", change.contains("style") ? "edit_line" : "move_line"}, {"id", id.toStdString()}};
        for (const auto& [key, value] : change.items()) op[key] = value;
        apply_ops(Json::array({op}));
    });
    connect(canvas_, &PageCanvas::lineEditRequested, this, [this](const QString& id) { edit_line_inline(id.toStdString()); });
    connect(canvas_, &PageCanvas::lineContextMenu, this, [this](const QString& id, const QPoint& global) {
        if (auto menu = line_menu(id.toStdString())) menu->exec(global);
    });
    connect(canvas_, &PageCanvas::textMoved, this, [this](const QString& id, double x_mm, double y_mm) {
        apply_ops(Json::array({Json{{"op", "move_line"}, {"id", id.toStdString()}, {"x_mm", x_mm}, {"y_mm", y_mm}}}));
    });
}

void MainWindow::build_line_menus(QMenu* pages) {
    // ページ → 台詞, then the book's lines (Python's menu: the shapes of the chosen balloon between)
    QMenu* lines = pages->addMenu(QStringLiteral("台詞"));
    lines->addAction(action("act_line_type"));
    lines->addAction(action("act_balloon_pen"));
    lines->addSeparator();
    lines->addAction(action("act_line_edit"));
    lines->addAction(action("act_line_wrap"));
    QMenu* shapes = lines->addMenu(QStringLiteral("フキダシの形"));
    for (const auto& [key, label] : lettering::kinds()) shapes->addAction(label, this, [this, key = key] { set_selected_balloon(key); });
    lines->addAction(action("act_line_delete"));
    pages->addSeparator();
    pages->addAction(action("act_story_editor"));
    pages->addAction(action("act_replace"));
}

void MainWindow::build_line_dock() {
    auto* dock = new QDockWidget(QStringLiteral("台詞"), this);
    dock->setObjectName(QStringLiteral("台詞"));
    auto* scroll = new QScrollArea;  // (tall: it scrolls on a small screen instead of making the window taller)
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(story_);
    dock->setWidget(scroll);
    dock->setMinimumWidth(200);
    dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);  // (a tab of the row, as Python's)
    addDockWidget(Qt::RightDockWidgetArea, dock);
    tabifyDockWidget(pages_dock_, dock);
    view_menu_->addAction(dock->toggleViewAction());
    lines_dock_ = dock;
}

QWidget* MainWindow::line_select_page() {
    // the select tool's page: the chosen line's lettering and balloon first, then the view and the book's lines
    QWidget* page = action_page({QStringLiteral("表示"), action("act_fit"), action("act_actual"), QStringLiteral("原稿"), action("act_story_editor")});
    static_cast<QVBoxLayout*>(page->layout())->insertWidget(0, story_->style_box);
    return page;
}

void MainWindow::panel_for_tool(const QString& tool) {
    // the tab of the side row follows the work: the lines for the text tool, the layers for the drawing tools (only
    // between those two, so a panel the person opened stays in front)
    QDockWidget* layers = findChild<QDockWidget*>(QStringLiteral("レイヤー"));
    if (lines_dock_ == nullptr || layers == nullptr) return;
    static const QStringList drawing = {QStringLiteral("pen"),   QStringLiteral("eraser"), QStringLiteral("fill"),
                                        QStringLiteral("lassofill"), QStringLiteral("vector"), QStringLiteral("blend"),
                                        QStringLiteral("liquify"), QStringLiteral("gradient"), QStringLiteral("shape")};
    QDockWidget* want = tool == QLatin1String("text") ? lines_dock_ : drawing.contains(tool) ? layers : nullptr;
    QDockWidget* other = want == lines_dock_ ? layers : lines_dock_;
    const auto seen = [this](QDockWidget* dock) { return dock->isVisible() && (dock->isFloating() || rect().intersects(dock->geometry())); };
    if (want != nullptr && !seen(want) && seen(other)) want->raise();
}

void MainWindow::refresh_lines() {
    lines_timer_.stop();
    if (story_ == nullptr) return;
    story_->refresh();
    story_->select(canvas_->selected_line_id);  // (the chosen line's settings beside the tool stay current)
}

// --- the lines typed on the page ------------------------------------------------------------------------------------

const core::StoryLine* MainWindow::line_by_id(const std::string& line_id) const {
    for (const core::StoryLine& line : book().story) {
        if (line.id == line_id) return &line;
    }
    return nullptr;
}

const core::Frame* MainWindow::frame_by_id(const std::optional<std::string>& frame_id) const {
    const core::Page* page = current_page();
    if (page == nullptr || !frame_id || frame_id->empty()) return nullptr;
    return page->find_frame(*frame_id);
}

void MainWindow::type_new_line(double x_mm, double y_mm) {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    const auto origin = session_;
    const std::string page_id = page->id;
    canvas_->open_editor(x_mm, y_mm, QString(), [this, origin, page_id, x_mm, y_mm](std::optional<QString> typed) {
        if (!typed || typed->isEmpty() || closed_) return;
        // (the book as it is now: the page may have gone while the words were typed)
        const core::Page* target = nullptr;
        for (const auto& p : book().pages) {
            if (p->id == page_id) target = p.get();
        }
        if (session_ != origin || target == nullptr) {
            flash(QStringLiteral("確認中に対象の原稿・ページが変更されたため、操作を中止しました。やり直してください。"), 6000, true);
            return;
        }
        const lettering::Marks marks = lettering::parse_marks(*typed);
        const core::Frame* frame = target->frame_at(Num(x_mm), Num(y_mm));
        const TextToolSettings::LineFields fields = text_settings_->line_fields();
        const Json box = lettering::place_at(x_mm, y_mm, marks.text, fields.balloon, fields.vertical, frame);
        Json op{{"op", "add_line"}, {"page", target->index.json()}, {"text", marks.text}, {"balloon", fields.balloon}};
        for (const auto& [key, value] : box.items()) op[key] = value;
        if (!fields.style.empty()) op["style"] = fields.style;
        if (frame != nullptr) op["frame_id"] = frame->id;
        if (!marks.ruby_runs.empty()) op["ruby_runs"] = marks.ruby_runs;
        if (!marks.emphasis_runs.empty()) op["emphasis_runs"] = marks.emphasis_runs;
        if (!marks.style_runs.empty()) op["style_runs"] = marks.style_runs;
        const auto before = story_ids(book());
        if (apply_ops(Json::array({op}))) {
            const auto added = added_line(book(), before);
            canvas_->selected_line_id = added;
            choose_tool(QStringLiteral("select"));
            on_line_selected(added.value_or(std::string()), false);
        }
    });
}

void MainWindow::balloon_drawn(const Json& outline) {
    // the text tool's balloon pen: the drawn outline becomes the balloon, then the words are typed
    const core::Page* page = current_page();
    if (page == nullptr || !outline.is_array() || outline.empty()) return;
    double x0 = outline[0][0].get<double>(), x1 = x0, y0 = outline[0][1].get<double>(), y1 = y0;
    for (const Json& p : outline) {
        x0 = core::py_min(x0, p[0].get<double>());
        x1 = core::py_max(x1, p[0].get<double>());
        y0 = core::py_min(y0, p[1].get<double>());
        y1 = core::py_max(y1, p[1].get<double>());
    }
    const double cx = (x0 + x1) / 2;
    const double cy = (y0 + y1) / 2;
    const auto origin = session_;
    const std::string page_id = page->id;
    canvas_->open_editor(x0, y0, QString(), [this, origin, page_id, outline, cx, cy](std::optional<QString> typed) {
        if (!typed || typed->isEmpty() || closed_) return;
        const core::Page* target = nullptr;
        for (const auto& p : book().pages) {
            if (p->id == page_id) target = p.get();
        }
        if (session_ != origin || target == nullptr) {
            flash(QStringLiteral("確認中に対象の原稿・ページが変更されたため、操作を中止しました。やり直してください。"), 6000, true);
            return;
        }
        const lettering::Marks marks = lettering::parse_marks(*typed);
        const TextToolSettings::LineFields fields = text_settings_->line_fields();
        const std::string kind = fields.balloon != "sfx" && fields.balloon != "none" && fields.balloon != "narration" ? fields.balloon : "speech";
        Json op{{"op", "add_line"}, {"page", target->index.json()}, {"text", marks.text}, {"balloon", kind}, {"path", outline},
                {"wrap", fields.vertical ? "vertical" : "horizontal"}};
        if (const core::Frame* frame = target->frame_at(Num(cx), Num(cy))) op["frame_id"] = frame->id;
        if (!fields.style.empty()) op["style"] = fields.style;
        if (!marks.ruby_runs.empty()) op["ruby_runs"] = marks.ruby_runs;
        if (!marks.emphasis_runs.empty()) op["emphasis_runs"] = marks.emphasis_runs;
        if (!marks.style_runs.empty()) op["style_runs"] = marks.style_runs;
        const auto before = story_ids(book());
        if (apply_ops(Json::array({op}))) {
            const auto added = added_line(book(), before);
            canvas_->selected_line_id = added;
            on_line_selected(added.value_or(std::string()), false);
        }
    });
}

void MainWindow::edit_line_inline(const std::string& line_id) {
    const core::StoryLine* line = line_by_id(line_id);
    if (line == nullptr) return;
    const auto origin = session_;
    canvas_->open_editor(line->x_mm.value(), line->y_mm.value(), lettering::with_marks(*line), [this, origin, line_id](std::optional<QString> typed) {
        if (!typed || typed->isEmpty() || closed_) return;
        // (another book in the window now — its tab chosen, Ctrl+Tab — may have a line of the same id: not that one)
        if (session_ != origin) {
            flash(QStringLiteral("確認中に対象の原稿・ページが変更されたため、操作を中止しました。やり直してください。"), 6000, true);
            return;
        }
        const core::StoryLine* current = line_by_id(line_id);  // (the line as it is now)
        if (current == nullptr) return;
        const lettering::Marks marks = lettering::parse_marks(*typed);
        if (lettering::same_marks(marks, *current)) return;
        Json ops = Json::array({Json{{"op", "edit_line"},
                                     {"id", line_id},
                                     {"text", marks.text},
                                     {"ruby_runs", marks.ruby_runs},
                                     {"emphasis_runs", marks.emphasis_runs},
                                     {"style_runs", marks.style_runs}}});
        const Json size = lettering::refit(*current, frame_by_id(current->frame_id), marks.text, current->balloon, current->wrap == "vertical");
        // keep the balloon's centre where it was
        const double cx = (current->x_mm + current->w_mm / Num(2)).value();
        const double cy = (current->y_mm + current->h_mm / Num(2)).value();
        const double w = size["w_mm"].get<double>();
        const double h = size["h_mm"].get<double>();
        ops.push_back(Json{{"op", "move_line"}, {"id", line_id}, {"x_mm", core::py_round(cx - w / 2, 2)}, {"y_mm", core::py_round(cy - h / 2, 2)},
                           {"w_mm", w}, {"h_mm", h}});
        apply_ops(ops);
    });
}

void MainWindow::on_line_selected(const std::string& line_id, bool open_panel) {
    story_->refresh();
    story_->select(line_id);
    if (open_panel) show_dock(QStringLiteral("台詞"));
}

const core::StoryLine* MainWindow::selected_line_or_say() {
    const core::StoryLine* line = canvas_->selected_line_id ? line_by_id(*canvas_->selected_line_id) : nullptr;
    if (line == nullptr) flash(QStringLiteral("先に選択ツール（V）で台詞（フキダシ）をクリックして選びます"), 4000);
    return line;
}

void MainWindow::edit_selected_line() {
    if (const core::StoryLine* line = selected_line_or_say()) edit_line_inline(line->id);
}

void MainWindow::delete_selected_line() {
    const core::StoryLine* line = selected_line_or_say();
    if (line != nullptr && apply_ops(Json::array({Json{{"op", "delete_line"}, {"id", line->id}}}))) canvas_->selected_line_id.reset();
}

void MainWindow::toggle_selected_wrap() {
    const core::StoryLine* line = selected_line_or_say();
    if (line == nullptr) return;
    const bool vertical = line->wrap != "vertical";
    Json move{{"op", "move_line"}, {"id", line->id}};
    const Json size = lettering::refit(*line, frame_by_id(line->frame_id), line->text, line->balloon, vertical);
    for (const auto& [key, value] : size.items()) move[key] = value;
    apply_ops(Json::array({Json{{"op", "edit_line"}, {"id", line->id}, {"wrap", vertical ? "vertical" : "horizontal"}}, move}));
}

void MainWindow::set_selected_balloon(const QString& kind) {
    if (const core::StoryLine* line = selected_line_or_say()) apply_ops(Json::array({Json{{"op", "edit_line"}, {"id", line->id}, {"balloon", kind.toStdString()}}}));
}

// --- a balloon's own menu -------------------------------------------------------------------------------------------

namespace {

// _with_bend(line, tail): a tail with one more corner, half way between its last bend (or the balloon) and its tip
Json with_bend(const core::StoryLine& line, const Json& tail) {
    const double cx = (line.x_mm + (line.w_mm.truthy() ? line.w_mm : Num(40)) / Num(2)).value();
    const double cy = (line.y_mm + (line.h_mm.truthy() ? line.h_mm : Num(20)) / Num(2)).value();
    Json bends = Json::array();
    if (core::truthy_at(tail, "vias")) {
        for (const Json& v : core::iterate(tail["vias"])) bends.push_back(v);
    } else if (core::truthy_at(tail, "via")) {
        bends.push_back(tail["via"]);
    }
    const std::vector<Json> to = core::iterate(tail["to"]);
    const double sx = bends.empty() ? cx : core::to_float(bends.back()[0]);
    const double sy = bends.empty() ? cy : core::to_float(bends.back()[1]);
    bends.push_back(Json::array({core::py_round((sx + core::to_float(to[0])) / 2 + 3, 2), core::py_round((sy + core::to_float(to[1])) / 2, 2)}));
    Json out = Json::object();
    for (const auto& [key, value] : tail.items()) {
        if (key != "via") out[key] = value;
    }
    out["vias"] = bends;
    return out;
}

}  // namespace

std::optional<QString> MainWindow::panel_reference(const std::string& frame_id) const {
    // the words that point an AI at one panel: the page and reading order a person sees, and the names the AI's tools use
    const core::Page* page = current_page();
    const core::Frame* frame = page != nullptr ? page->find_frame(frame_id) : nullptr;
    if (frame == nullptr) return std::nullopt;
    const auto leaves = page->leaf_frames();
    QString order = QStringLiteral("None");
    for (std::size_t i = 0; i < leaves.size(); ++i) {
        if (leaves[i]->id == frame_id) {
            order = QString::number(i + 1);
            break;
        }
    }
    QString slot;
    if (frame->panel && frame->panel->is_object() && frame->panel->contains("slot") && core::py_truthy((*frame->panel)["slot"]))
        slot = QString::fromStdString(core::py_str((*frame->panel)["slot"]));
    const QString index = QString::fromStdString(page->index.repr());
    const QString names = QStringLiteral("page %1, frame_id \"%2\"").arg(index, QString::fromStdString(frame_id)) +
                          (slot.isEmpty() ? QString() : QStringLiteral(", slot \"%1\"").arg(slot));
    return QStringLiteral("%1 ページ目の %2 コマ目（読み順）［%3］").arg(index, order, names);
}

std::unique_ptr<QMenu> MainWindow::line_menu(const std::string& line_id) {
    const core::StoryLine* line = line_by_id(line_id);
    if (line == nullptr) return nullptr;
    on_line_selected(line_id, false);
    auto menu = std::make_unique<QMenu>(this);
    // (each item reads the line as it is when chosen: the book may change while the menu is up)
    const auto now = [this, line_id] { return line_by_id(line_id); };
    const auto apply_now = [this, now](const std::function<Json(const core::StoryLine&)>& ops_of) {
        if (const core::StoryLine* l = now()) apply_ops(ops_of(*l));
    };
    menu->addAction(QStringLiteral("打ち直す（ダブルクリック）"), this, [this, line_id] { edit_line_inline(line_id); });
    QMenu* shapes = menu->addMenu(QStringLiteral("フキダシの形"));
    for (const auto& [key, label] : lettering::kinds()) {
        QAction* act = shapes->addAction(label);
        act->setCheckable(true);
        act->setChecked(QString::fromStdString(line->balloon) == key);
        const std::string kind = key.toStdString();
        connect(act, &QAction::triggered, this, [apply_now, kind] {
            apply_now([kind](const core::StoryLine& l) { return Json::array({Json{{"op", "edit_line"}, {"id", l.id}, {"balloon", kind}}}); });
        });
    }
    const std::string turned = line->wrap == "vertical" ? "horizontal" : "vertical";
    menu->addAction(line->wrap == "vertical" ? QStringLiteral("横書きにする") : QStringLiteral("縦書きにする"), this, [apply_now, turned] {
        apply_now([turned](const core::StoryLine& l) { return Json::array({Json{{"op", "edit_line"}, {"id", l.id}, {"wrap", turned}}}); });
    });
    menu->addSeparator();
    const auto tails_op = [](const core::StoryLine& l, const Json& tails) { return Json::array({Json{{"op", "move_line"}, {"id", l.id}, {"tails", tails}}}); };
    menu->addAction(QStringLiteral("しっぽを足す"), this, [apply_now, tails_op] {
        apply_now([tails_op](const core::StoryLine& l) {
            Json tails = PageCanvas::tails_of(l);
            tails.push_back(Json{{"to", num_point(l.x_mm - l.w_mm * Num(0.2), l.y_mm + l.h_mm * Num(1.25))}});
            return tails_op(l, tails);
        });
    });
    const Json tails = PageCanvas::tails_of(*line);
    if (!tails.empty()) {
        menu->addAction(QStringLiteral("しっぽを 1 本消す"), this, [apply_now, tails_op] {
            apply_now([tails_op](const core::StoryLine& l) {
                Json all = PageCanvas::tails_of(l);
                if (!all.empty()) all.erase(all.end() - 1);
                return tails_op(l, all);
            });
        });
        QAction* bend = menu->addAction(QStringLiteral("しっぽに曲がり角を足す（折れ線）"), this, [apply_now, tails_op] {
            apply_now([tails_op](const core::StoryLine& l) {
                Json bent = Json::array();
                for (const Json& t : PageCanvas::tails_of(l)) bent.push_back(with_bend(l, t));
                return tails_op(l, bent);
            });
        });
        bend->setToolTip(QStringLiteral("しっぽの途中に角を足します。□をドラッグして折れ線のしっぽにします"));
        menu->addAction(QStringLiteral("しっぽをまっすぐにする"), this, [apply_now, tails_op] {
            apply_now([tails_op](const core::StoryLine& l) {
                Json straight = Json::array();
                for (const Json& t : PageCanvas::tails_of(l)) straight.push_back(Json{{"to", t["to"]}});
                return tails_op(l, straight);
            });
        });
        QMenu* kinds = menu->addMenu(QStringLiteral("しっぽの形"));
        for (const auto& [key, label] : {std::pair{"wedge", "くさび"}, {"zigzag", "ギザギザ"}, {"fade", "消える"}, {"bubbles", "泡（心の声）"}}) {
            QAction* act = kinds->addAction(QString::fromUtf8(label));
            act->setCheckable(true);
            act->setChecked(std::all_of(tails.begin(), tails.end(), [key = key](const Json& t) {
                const Json kind = core::get_or(t, "kind", Json());
                return (core::py_truthy(kind) ? core::py_str(kind) : std::string("wedge")) == key;
            }));
            connect(act, &QAction::triggered, this, [apply_now, tails_op, kind = std::string(key)] {
                apply_now([tails_op, kind](const core::StoryLine& l) {
                    Json kinded = Json::array();
                    for (Json t : PageCanvas::tails_of(l)) {
                        t["kind"] = kind;
                        kinded.push_back(t);
                    }
                    return tails_op(l, kinded);
                });
            });
        }
    }
    menu->addSeparator();
    const Json group = render::text::style_of(*line)["group"];
    const auto lines = book().story_for_page(line->page_index);
    const auto at = std::find_if(lines.begin(), lines.end(), [&](const core::StoryLine* l) { return l->id == line_id; });
    if (at != lines.end() && at + 1 != lines.end()) {
        const std::string next = (*(at + 1))->id;
        // (f"g_{line_id[:8]}": the id's first eight characters, code points as Python's str has them)
        const Json key = core::py_truthy(group) ? group : Json("g_" + render::text::utf8(render::text::u32(line_id).substr(0, 8)));
        menu->addAction(QStringLiteral("次の台詞のフキダシとつなげる"), this, [this, line_id, next, key] {
            apply_ops(Json::array({Json{{"op", "edit_line"}, {"id", line_id}, {"style", Json{{"group", key}}}},
                                   Json{{"op", "edit_line"}, {"id", next}, {"style", Json{{"group", key}}}}}));
        });
    }
    if (core::py_truthy(group)) {
        const Num page_index = line->page_index;
        menu->addAction(QStringLiteral("つなげたフキダシを離す"), this, [this, page_index, group] {
            Json ops = Json::array();
            for (const core::StoryLine* l : book().story_for_page(page_index)) {
                if (core::py_equals(render::text::style_of(*l)["group"], group)) ops.push_back(Json{{"op", "edit_line"}, {"id", l->id}, {"style", Json{{"group", nullptr}}}});
            }
            apply_ops(ops);
        });
    }
    menu->addAction(QStringLiteral("画像のフキダシにする…"), this, [this, line_id] { picture_balloon(line_id); });
    QMenu* paths = menu->addMenu(QStringLiteral("文字をパスに沿わせる"));
    const Num w = line->w_mm.truthy() ? line->w_mm : Num(40);
    const Num h = line->h_mm.truthy() ? line->h_mm : Num(20);
    const auto at_ = [](const Num& x, const Num& y) { return Json::array({core::py_round(x, 2).json(), core::py_round(y, 2).json()}); };
    const std::vector<std::pair<QString, Json>> presets = {
        {QStringLiteral("弧（上にふくらむ）"), Json::array({at_(Num(0), h), at_(w * Num(0.25), h * Num(0.3)), at_(w * Num(0.5), Num(0)),
                                                       at_(w * Num(0.75), h * Num(0.3)), at_(w, h)})},
        {QStringLiteral("弧（下にふくらむ）"), Json::array({at_(Num(0), Num(0)), at_(w * Num(0.25), h * Num(0.7)), at_(w * Num(0.5), h),
                                                       at_(w * Num(0.75), h * Num(0.7)), at_(w, Num(0))})},
        {QStringLiteral("波"), Json::array({at_(Num(0), h / Num(2)), at_(w * Num(0.25), Num(0)), at_(w * Num(0.5), h / Num(2)), at_(w * Num(0.75), h),
                                         at_(w, h / Num(2))})},
        {QStringLiteral("斜めに上がる"), Json::array({at_(Num(0), h), at_(w, Num(0))})},
    };
    for (const auto& [label, points] : presets) {
        paths->addAction(label, this, [this, line_id, points = points] {
            apply_ops(Json::array({Json{{"op", "edit_line"}, {"id", line_id}, {"balloon", "none"}, {"style", Json{{"text_path", points}}}}}));
        });
    }
    paths->addAction(QStringLiteral("描いて決める（次に引く線に沿わせる）"), this, [this, line_id] { draw_text_path(line_id); });
    if (core::py_truthy(render::text::style_of(*line)["text_path"])) {
        paths->addAction(QStringLiteral("パスから外す"), this, [this, line_id] {
            apply_ops(Json::array({Json{{"op", "edit_line"}, {"id", line_id}, {"style", Json{{"text_path", nullptr}}}}}));
        });
    }
    if (line->frame_id && !line->frame_id->empty() && panel_reference(*line->frame_id)) {  // (the panel this line is in, for an AI)
        menu->addSeparator();
        QAction* ref = menu->addAction(QStringLiteral("AI 用の参照をコピー（この台詞のコマ）"));
        ref->setToolTip(QStringLiteral("この台詞があるコマを AI に伝える言葉（ページ・読み順・AI の使う名前）をコピーします"));
        const std::string frame_id = *line->frame_id;
        connect(ref, &QAction::triggered, this, [this, frame_id] {
            if (const auto words = panel_reference(frame_id)) {
                QApplication::clipboard()->setText(*words);
                flash(QStringLiteral("コピーしました: %1").arg(*words), 4000);
            }
        });
    }
    menu->addSeparator();
    menu->addAction(QStringLiteral("消す"), this, [this, line_id] { apply_ops(Json::array({Json{{"op", "delete_line"}, {"id", line_id}}})); });
    return menu;
}

void MainWindow::picture_balloon(const std::string& line_id) {
    // 画像のフキダシ: a picture as the balloon
    const QString path = ask::open_path(this, QStringLiteral("フキダシにする画像"), QStringLiteral("画像 (*.png *.webp *.jpg *.jpeg)"));
    if (path.isEmpty() || closed_) return;
    std::string png;
    try {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) throw core::Error("io", file.errorString().toStdString());
        const QByteArray bytes = file.readAll();
        render::Image image = render::open_image(std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size()))).convert("RGBA");
        image.thumbnail(render::Size{1200, 1200});
        png = render::write_png(image, 9);
    } catch (const std::exception& error) {
        flash(QStringLiteral("読み込めない画像です:\n%1").arg(QString::fromUtf8(error.what())), 6000, true);
        return;
    }
    apply_ops(Json::array({Json{{"op", "edit_line"}, {"id", line_id}, {"balloon", "picture"}, {"style", Json{{"picture", core::b64encode(png)}}}}}));
}

void MainWindow::draw_text_path(const std::string& line_id) {
    // the next line drawn with the pen becomes the path the words follow
    text_path_for_ = line_id;
    choose_tool(QStringLiteral("pen"));
    flash(QStringLiteral("文字を沿わせる線を、ペンで 1 本引きます"), 6000);
}

bool MainWindow::text_path_stroke(const StrokeInput& stroke) {
    if (!text_path_for_) return false;
    const std::string line_id = *text_path_for_;
    text_path_for_.reset();
    canvas_->stroke_dropped();  // (this stroke is a path for words, not ink)
    const core::StoryLine* line = line_by_id(line_id);
    const std::size_t n = stroke.points.size();
    if (line != nullptr && n >= 2) {
        const std::size_t step = std::max<std::size_t>(1, n / 40);
        Json path = Json::array();
        for (std::size_t i = 0; i < n; i += step) {
            path.push_back(Json::array({core::py_round((Num(stroke.points[i].x) - line->x_mm).value(), 2),
                                        core::py_round((Num(stroke.points[i].y) - line->y_mm).value(), 2)}));
        }
        apply_ops(Json::array({Json{{"op", "edit_line"}, {"id", line_id}, {"balloon", "none"}, {"style", Json{{"text_path", path}}}}}));
    }
    return true;
}

bool MainWindow::balloon_eraser_stroke(const StrokeInput& stroke, const core::Page& page) {
    // フキダシ消しゴム: the eraser with フキダシを削る cuts the balloons it went over (cut_balloon), and draws on nothing
    if (stroke.tool != QLatin1String("eraser") || eraser_balloons_ == nullptr || !eraser_balloons_->isChecked() || stroke.points.empty()) return false;
    canvas_->stroke_dropped();
    const double reach = eraser_mm_ / 2;
    double x0 = stroke.points[0].x, x1 = x0, y0 = stroke.points[0].y, y1 = y0;
    for (const core::PenPoint& p : stroke.points) {
        x0 = core::py_min(x0, p.x);
        x1 = core::py_max(x1, p.x);
        y0 = core::py_min(y0, p.y);
        y1 = core::py_max(y1, p.y);
    }
    Json points = Json::array();
    for (const core::PenPoint& p : stroke.points) points.push_back(Json::array({core::py_round(p.x, 2), core::py_round(p.y, 2)}));
    Json ops = Json::array();
    for (const core::StoryLine* line : book().story_for_page(page.index)) {
        const std::string kind = line->balloon.empty() ? std::string("speech") : line->balloon;
        const double w = line->w_mm.truthy() ? line->w_mm.value() : 40.0;
        const double h = line->h_mm.truthy() ? line->h_mm.value() : 20.0;
        if (kind == "sfx" || kind == "none") continue;
        if (x0 - reach < line->x_mm.value() + w && x1 + reach > line->x_mm.value() && y0 - reach < line->y_mm.value() + h && y1 + reach > line->y_mm.value())
            ops.push_back(Json{{"op", "cut_balloon"}, {"id", line->id}, {"points", points}, {"width_mm", eraser_mm_}});
    }
    if (ops.empty()) {
        flash(QStringLiteral("なぞった所にフキダシがありません"), 3000);
        return true;
    }
    apply_ops(ops);
    return true;
}

// --- the book's lines -------------------------------------------------------------------------------------------------

StoryEditor* MainWindow::story_editor() const { return story_editor_.data(); }

void MainWindow::open_story_editor() {
    if (!story_editor_.isNull()) story_editor_->close();
    auto* editor = new StoryEditor(this);
    editor->setAttribute(Qt::WA_DeleteOnClose);
    story_editor_ = editor;
    editor->show();
}

void MainWindow::replace_dialog() {
    ReplaceDialog dialog(this);
    ask::exec(&dialog);
}

}  // namespace genko::app
