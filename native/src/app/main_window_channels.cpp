// channels@1 (SPEC.md COLOR-03): the window's side of a page's channels — the channel panel's dock beside the layers, what
// the pixel tools work on (the target: the ops sent from here are given it as their "channel", as PhotoCraft's session
// fills a pixel command's target in), what is shown on screen (the plates, the alpha channels laid over, the spots), the
// target keys Ctrl+Alt+2…9 (Ctrl+2 is 定規にスナップ), and the selection saved to a channel or loaded from one.

#include <QAction>
#include <QDockWidget>
#include <QMenu>

#include <algorithm>
#include <set>
#include <string>

#include "app/ask.hpp"
#include "app/canvas.hpp"
#include "app/channel_panel.hpp"
#include "app/main_window.hpp"
#include "core/channels.hpp"
#include "core/error.hpp"
#include "core/pyconv.hpp"
#include "render/selection.hpp"

namespace genko::app {

using core::Json;

MainWindow::ChannelView MainWindow::channel_view() const {
    const core::Page* page = current_page();
    if (page == nullptr) return {};
    const auto it = channel_views_.find(page->id);
    ChannelView view = it == channel_views_.end() ? ChannelView{} : it->second;
    // (what the page no longer has is forgotten: a channel deleted, undone, another mode)
    const std::size_t plates = core::plate_names(*page).size();
    view.hidden.resize(plates, false);
    if (!view.target.empty() && core::find_channel(*page, view.target) == nullptr &&
        !(core::plate_index(*page, view.target) && plates > 1)) {
        view.target.clear();
        view.hidden.assign(plates, false);
    }
    std::erase_if(view.shown, [&](const std::string& id) { return core::find_channel(*page, id) == nullptr; });
    std::erase_if(view.hidden_spots, [&](const std::string& id) { return core::find_channel(*page, id) == nullptr; });
    // (never nothing at all: the plates back when none is shown and no channel is laid over)
    if (std::all_of(view.hidden.begin(), view.hidden.end(), [](bool h) { return h; }) && view.shown.empty() &&
        (view.target.empty() || core::plate_index(*page, view.target))) {
        view.hidden.assign(plates, false);
    }
    return view;
}

void MainWindow::channel_view_changed() {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    const ChannelView view = channel_view();
    PageRenderer::ChannelView shown;
    if (std::find(view.hidden.begin(), view.hidden.end(), true) != view.hidden.end()) {
        std::vector<bool> planes;
        for (const bool hidden : view.hidden) planes.push_back(!hidden);
        shown.planes = std::move(planes);
    }
    for (const core::Channel& channel : page->channels) {
        if (channel.kind == core::ChannelKind::Alpha && view.shown.contains(channel.id)) shown.overlays.push_back(channel.id);
    }
    if (!view.hidden_spots.empty()) {
        std::vector<std::string> spots;
        for (const core::Channel& channel : page->channels) {
            if (channel.kind == core::ChannelKind::Spot && !view.hidden_spots.contains(channel.id)) spots.push_back(channel.id);
        }
        shown.spots = std::move(spots);
    }
    canvas_->renderer().set_channel_view(std::move(shown));
    if (channel_panel_ != nullptr) channel_panel_->refresh();
}

void MainWindow::target_channel(const std::string& target) {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    ChannelView view = channel_view();
    const std::size_t plates = core::plate_names(*page).size();
    view.target = target;
    if (target.empty() || target == "composite") {  // (the composite: every plate shown again)
        view.target.clear();
        view.hidden.assign(plates, false);
    } else if (const auto plate = core::plate_index(*page, target); plate && core::find_channel(*page, target) == nullptr) {
        view.hidden.assign(plates, true);  // (a plate alone, in grey)
        view.hidden[*plate] = false;
        view.shown.clear();
    } else if (const core::Channel* channel = core::find_channel(*page, target); channel != nullptr) {
        view.target = channel->id;
        view.hidden.assign(plates, true);  // (the channel alone, in grey)
        view.shown = {channel->id};
        view.hidden_spots.erase(channel->id);
    } else {
        return;
    }
    channel_views_[page->id] = view;
    channel_view_changed();
}

void MainWindow::target_slot(int slot) {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    if (slot == 2) {
        target_channel("");
        return;
    }
    const std::vector<std::string> plates = core::plate_names(*page);
    const std::size_t shown = plates.size() > 1 ? plates.size() : 0;  // (PhotoCraft's ⌘3…: the plates, then the channels)
    const std::size_t at = static_cast<std::size_t>(slot - 3);
    if (at < shown) {
        target_channel(plates[at]);
    } else if (at - shown < page->channels.size()) {
        target_channel(page->channels[at - shown].id);
    } else {
        flash(QStringLiteral("このページには %1 番目のチャンネルがありません").arg(slot), 3000);
    }
}

void MainWindow::show_channel(const std::string& key, bool on) {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    ChannelView view = channel_view();
    if (key == "composite") {
        view.hidden.assign(view.hidden.size(), !on);
    } else if (const core::Channel* channel = core::find_channel(*page, key); channel != nullptr) {
        if (channel->kind == core::ChannelKind::Alpha) {
            if (on) view.shown.insert(channel->id);
            else view.shown.erase(channel->id);
        } else if (on) {
            view.hidden_spots.erase(channel->id);
        } else {
            view.hidden_spots.insert(channel->id);
        }
    } else if (const auto plate = core::plate_index(*page, key)) {
        view.hidden[*plate] = !on;
    } else {
        return;
    }
    channel_views_[page->id] = view;
    channel_view_changed();
}

Json MainWindow::with_channel_target(const Json& ops) const {
    const core::Page* page = current_page();
    if (page == nullptr || !ops.is_array()) return ops;
    const ChannelView view = channel_view();
    if (view.target.empty()) return ops;
    const bool plate = core::find_channel(*page, view.target) == nullptr;
    // (PC3: the pixel ops that change a layer's pixels and cannot paint a channel — the retouching brushes, コンテンツに応じた
    // 塗りつぶし, the Magic Eraser, 境界線を描く, the matting —: refused while a channel, the Quick Mask too, is drawn on, rather
    // than changing the layer under it)
    static const std::set<std::string, std::less<>> kLayerOnly{"clone_stamp", "healing_brush", "spot_healing", "history_brush",
                                                               "content_aware_fill", "magic_erase", "outline_area", "matting",
                                                               "vanishing_point",  // (PC4 D6: 消点's pastes and strokes)
                                                               "render_picture"};  // (PC4 D4: 炎・額縁・木 draw on a layer)
    const auto on_this_page = [&](const Json& op) {
        try {
            return core::Num(core::py_int(op.value("page", Json()))) == page->index;
        } catch (const std::exception&) {
            return false;
        }
    };
    Json out = ops;
    for (Json& op : out) {
        if (!op.is_object() || !op.contains("op") || !op["op"].is_string()) continue;
        if (!core::channel_target_op(op["op"].get<std::string>())) {
            if (!plate && kLayerOnly.contains(op["op"].get<std::string>()) && on_this_page(op)) {
                throw core::Error("value", "チャンネル（クイックマスクも）に描いている間は、この操作はできません（レイヤーの画素だけを変える操作です）。"
                                    "描く先をレイヤーに戻してから使います");
            }
            continue;
        }
        if (!on_this_page(op)) continue;  // (another page's op: as it is)
        if (core::python_channel_kind(op)) {
            // (levels, curve and brightness_contrast take Python's own channel — r, g or b, else all three —, which the
            // target fills in when the op names none (all three): an RGB page's plate as r, g or b; a CMYK or Lab page's
            // plate by its name and a channel by its id, this build's values there)
            if (op.contains("channel") && op["channel"] != Json("rgb")) continue;
            if (!plate) {
                op["channel"] = view.target;
            } else if (page->color && (page->color->space == core::InkSpace::Cmyk || page->color->space == core::InkSpace::Lab)) {
                op["channel"] = core::plate_names(*page).at(*core::plate_index(*page, view.target));
            } else if (!page->color) {
                static const std::map<std::string, std::string> kLetters{{"red", "r"}, {"green", "g"}, {"blue", "b"}};
                if (const auto letter = kLetters.find(view.target); letter != kLetters.end()) op["channel"] = letter->second;
            }
            continue;
        }
        if (!op.contains("channel")) op["channel"] = view.target;
    }
    return out;
}

bool MainWindow::save_selection_to_channel(std::optional<QString> name) {
    const auto area = need_area();
    const core::Page* page = current_page();
    if (!area || page == nullptr) return false;
    if (!name) {
        const Asked asked = asking();
        name = ask::get_text(this, QStringLiteral("選択範囲を保存"), QStringLiteral("チャンネルの名前（空のままなら「Alpha n」）"));
        if (!name || !still(asked)) return false;
    }
    Json op{{"op", "add_channel"}, {"page", page->index.json()}, {"fill", "area"}, {"area", *area}};
    if (!name->trimmed().isEmpty()) op["name"] = name->trimmed().toStdString();
    return apply_ops(Json::array({op}));
}

void MainWindow::load_selection_from_channel(const std::string& id) {
    const core::Page* page = current_page();
    if (page == nullptr || core::find_channel(*page, id) == nullptr) return;
    Json area;
    try {
        area = render::selection::resolve(Json{{"channel", id}}, *page, &book());
    } catch (const core::OpError&) {
        flash(QStringLiteral("このチャンネルには、選択する所がありません"), 3000);
        return;
    }
    if (canvas_->tool() != QLatin1String("marquee")) choose_tool(QStringLiteral("rect"));
    join_selection(area, QStringLiteral("replace"));
}

void MainWindow::build_channel_actions() {
    const QString tip = QStringLiteral("ブラシツール・消しゴムツール・塗り・フィルターの描く先を、ページのすべての色（合成）に戻す");
    make("act_channel_composite", QStringLiteral("合成チャンネルを対象にする"), [this] { target_slot(2); }, {QKeySequence(QStringLiteral("Ctrl+Alt+2"))}, tip);
    for (int slot = 3; slot <= 9; ++slot) {
        make(QStringLiteral("act_channel_%1").arg(slot), QStringLiteral("チャンネル %1 を対象にする").arg(slot), [this, slot] { target_slot(slot); },
             {QKeySequence(QStringLiteral("Ctrl+Alt+%1").arg(slot))},
             QStringLiteral("チャンネル パネルの %1 番目（合成の次から: 色の版、アルファ・特色の順）を描く先にする").arg(slot - 1));
    }
    make("act_sel_to_channel", QStringLiteral("選択範囲を保存…"), [this] { save_selection_dialog(); }, {},
         QStringLiteral("選択範囲をアルファチャンネルとしてページに残す（新規か、あるチャンネルに置き換え・追加・削除・交差。白が選ばれた所）"));
}

void MainWindow::build_channel_dock() {
    channel_panel_ = new ChannelPanel(this);
    channels_dock_ = new QDockWidget(QStringLiteral("チャンネル"), this);
    channels_dock_->setObjectName(QStringLiteral("チャンネル"));
    channels_dock_->setWidget(channel_panel_);
    channels_dock_->setMinimumWidth(200);
    channels_dock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::RightDockWidgetArea, channels_dock_);
    tabifyDockWidget(layers_dock_, channels_dock_);  // (beside the layers)
    channels_dock_->hide();  // (for colour work only: 表示 → チャンネル brings it, as the サブビュー)
    view_menu_->addAction(channels_dock_->toggleViewAction());
    connect(channels_dock_, &QDockWidget::visibilityChanged, this, [this](bool shown) {
        if (shown && channel_panel_ != nullptr) channel_panel_->refresh();
    });
}

}  // namespace genko::app
