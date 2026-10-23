// PC4 E2 (window.panel.brushes, brush.presets.list・.save・.delete・.importAbr; SPEC.md COLOR-09「ブラシのプリセット」): the
// Brushes panel — the library by its groups (render/brush_presets), a click choosing the brush in hand.

#include "app/brushes_panel.hpp"

#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QImage>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>
#include <mutex>

#include "app/ask.hpp"
#include "app/brush_panel.hpp"
#include "app/config.hpp"
#include "app/main_window.hpp"
#include "app/off_ui.hpp"
#include "app/theme.hpp"
#include "core/brush_dabs.hpp"
#include "core/brushes.hpp"
#include "core/error.hpp"
#include "core/paths.hpp"
#include "render/abr_map.hpp"
#include "render/brush_presets.hpp"
#include "render/brushes.hpp"
#include "render/dabs/lines.hpp"
#include "service/wording.hpp"
#include "storage/fsutil.hpp"

namespace genko::app {

using core::Json;
namespace presets = render::brush_presets;

namespace {

constexpr int kKeyRole = Qt::UserRole + 1;    // (a brush's key)
constexpr int kGroupRole = Qt::UserRole + 2;  // (a group's name, and whether it is built in)

// (made once, by what draws them: small, kept for the panel's next refresh)
std::mutex& made_mutex() {
    static std::mutex m;
    return m;
}
std::map<std::string, QImage>& made_pictures() {
    static std::map<std::string, QImage> m;
    return m;
}

std::string picture_key(const std::string& ref, double hardness, double roundness, int side) {
    return ref.empty() ? "round/" + std::to_string(std::lround(hardness * 100)) + "/" + std::to_string(std::lround(roundness * 100)) + "/" + std::to_string(side)
                       : ref + "/" + std::to_string(side);
}

// The picture of a tip (black paint, its alpha the paint), off the window's thread.
QImage tip_picture(const std::filesystem::path& config, const std::string& ref, double hardness, double roundness, int side) {
    QImage out(side * 2, side * 2, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    if (ref.empty()) {
        // a round tip: its hardness as a soft edge, its roundness as an ellipse
        const double r = side * 0.85;
        for (int y = 0; y < out.height(); ++y) {
            for (int x = 0; x < out.width(); ++x) {
                const double dx = (x + 0.5 - side) / r, dy = (y + 0.5 - side) / (r * std::max(0.05, roundness));
                const double d = std::sqrt(dx * dx + dy * dy);
                const double inner = std::clamp(hardness, 0.0, 0.98);
                const double a = d >= 1.0 ? 0.0 : d <= inner ? 1.0 : 1.0 - (d - inner) / (1.0 - inner);
                out.setPixel(x, y, qRgba(0, 0, 0, static_cast<int>(std::lround(a * 255))));
            }
        }
        return out;
    }
    core::Bytes png = render::dabs::holds_picture(ref) ? render::dabs::picture(ref) : render::brushes::library_tip(config, ref);
    if (!png) return out;
    QImage grey;
    if (!grey.loadFromData(reinterpret_cast<const uchar*>(png->data()), static_cast<int>(png->size()), "PNG")) return out;
    grey = grey.convertToFormat(QImage::Format_Grayscale8).scaled(side * 2, side * 2, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    const int ox = (out.width() - grey.width()) / 2, oy = (out.height() - grey.height()) / 2;
    for (int y = 0; y < grey.height(); ++y) {
        const uchar* row = grey.constScanLine(y);
        for (int x = 0; x < grey.width(); ++x) out.setPixel(ox + x, oy + y, qRgba(0, 0, 0, row[x]));
    }
    return out;
}

QIcon icon_of(const QImage& image) {
    QPixmap pixmap = QPixmap::fromImage(image);
    pixmap.setDevicePixelRatio(2);
    // (the theme's text colour, so a dark panel shows it)
    QPixmap tinted(pixmap.size());
    tinted.setDevicePixelRatio(2);
    tinted.fill(Qt::transparent);
    QPainter p(&tinted);
    p.drawPixmap(0, 0, pixmap);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(tinted.rect(), QColor(theme::tokens().text));
    p.end();
    return QIcon(tinted);
}

// What draws a brush's picture: its sampled tip's ref ("" round) and its hardness and roundness.
struct TipOf {
    std::string ref;
    double hardness = 1.0, roundness = 1.0;
    bool dab = false;
};
TipOf tip_of(const Json& settings) {
    TipOf out;
    if (core::is_dab_brush(settings)) {
        out.dab = true;
        try {
            const Json given = settings.value("dabs", Json::object());
            const core::DabSettings d = core::dabs_from_json(given.is_null() ? Json::object() : given);
            out.ref = d.tip;
            out.hardness = d.hardness;
            out.roundness = d.roundness;
        } catch (const core::Error&) {
        }
    }
    return out;
}

}  // namespace

QIcon brush_tip_icon(const std::string& ref, double hardness, double roundness, int side) {
    const std::string key = picture_key(ref, hardness, roundness, side);
    {
        std::lock_guard lock(made_mutex());
        if (const auto it = made_pictures().find(key); it != made_pictures().end()) return icon_of(it->second);
    }
    QImage image = tip_picture(config_dir(), ref, hardness, roundness, side);
    std::lock_guard lock(made_mutex());
    made_pictures()[key] = image;
    return icon_of(image);
}

BrushesPanel::BrushesPanel(MainWindow* window) : window_(window) {
    setObjectName(QStringLiteral("brushes_panel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    tree_ = new QTreeWidget;
    tree_->setObjectName(QStringLiteral("brushes_tree"));
    tree_->setHeaderHidden(true);
    tree_->setIconSize(QSize(28, 28));
    tree_->setRootIsDecorated(true);
    layout->addWidget(tree_, 1);
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) { chosen(item); });
    auto* row = new QHBoxLayout;
    const auto add = [&](const char* name, const QString& text, const QString& tip, const std::function<void()>& work) {
        auto* b = new QPushButton(text);
        b->setObjectName(QString::fromLatin1(name));
        b->setToolTip(tip);
        connect(b, &QPushButton::clicked, this, work);
        row->addWidget(b);
        return b;
    };
    add("brush_preset_new", QStringLiteral("＋"), QStringLiteral("新規ブラシプリセット…: 今のブラシ（ツールの設定の）を自分のブラシとして加えます"), [this] { new_preset(); });
    add("brush_preset_rename", QStringLiteral("名前…"), QStringLiteral("選んだ自分のブラシの名前を変えます"), [this] { rename(); });
    add("brush_preset_delete", QStringLiteral("削除"), QStringLiteral("選んだ自分のブラシをライブラリから消します（描いた線はそのまま）"), [this] { remove(); });
    auto* more = new QToolButton;
    more->setObjectName(QStringLiteral("brush_preset_more"));
    more->setText(QStringLiteral("…"));
    more->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(more);
    menu->setObjectName(QStringLiteral("brush_preset_menu"));
    menu->addAction(QStringLiteral("ブラシを読み込み…"), this, [this] {
        const QString path = ask::open_path(this, QStringLiteral("ブラシを読み込み"), QStringLiteral("ブラシ (*.abr *.genkobrush)"));
        if (!path.isEmpty()) import_file(path);
    });
    menu->addAction(QStringLiteral("ブラシを書き出し…"), this, [this] {
        const QString path = ask::save_path(this, QStringLiteral("ブラシを書き出し"), QStringLiteral("ブラシ.genkobrush"), QStringLiteral("Genko のブラシ (*.genkobrush)"));
        if (!path.isEmpty()) export_file(path);
    });
    menu->addSeparator();
    menu->addAction(QStringLiteral("新規グループ…"), this, [this] { new_group(); });
    menu->addAction(QStringLiteral("グループ名を変更…"), this, [this] { rename_group(); });
    menu->addAction(QStringLiteral("グループを削除"), this, [this] { remove_group(); });
    QMenu* move = menu->addMenu(QStringLiteral("グループに移動"));
    move->setObjectName(QStringLiteral("brush_preset_move"));
    connect(move, &QMenu::aboutToShow, this, [this, move] {
        move->clear();
        for (const presets::Group& g : presets::load(config_dir()).groups) {
            if (g.builtin) continue;
            const QString name = QString::fromStdString(g.name);
            move->addAction(name, this, [this, name] { move_to_group(name); });
        }
        move->addAction(QStringLiteral("新規グループ…"), this, [this] {
            const auto name = ask::get_text(this, QStringLiteral("グループに移動"), QStringLiteral("新しいグループ名"), QStringLiteral("グループ 1"));
            if (name && !name->trimmed().isEmpty()) move_to_group(name->trimmed());
        });
    });
    more->setMenu(menu);
    row->addWidget(more);
    layout->addLayout(row);
    picture_timer_.setSingleShot(true);
    picture_timer_.setInterval(0);
    connect(&picture_timer_, &QTimer::timeout, this, [this] { make_pictures(); });
}

QPushButton* BrushesPanel::button(const QString& name) const { return findChild<QPushButton*>(name); }

std::string BrushesPanel::chosen_key() const {
    const QTreeWidgetItem* item = tree_->currentItem();
    return item != nullptr ? item->data(0, kKeyRole).toString().toStdString() : std::string();
}

QString BrushesPanel::group_of_choice() const {
    const QTreeWidgetItem* item = tree_->currentItem();
    if (item == nullptr) return {};
    if (item->parent() != nullptr) item = item->parent();
    return item->data(0, kGroupRole).toString();
}

void BrushesPanel::refresh() {
    loading_ = true;
    const std::string current = window_->brush_panel() != nullptr ? window_->brush_panel()->kind() : std::string();
    const std::string chosen_now = chosen_key();
    // (the brush in hand chosen — a built-in dab brush by its library key, whatever key the book took for it —, else
    // the row chosen before)
    std::string in_hand = current;
    if (const presets::Preset* builtin = presets::builtin_dab_of(current)) in_hand = builtin->key;
    tree_->clear();
    pending_.clear();
    QTreeWidgetItem* chosen_item = nullptr;
    QTreeWidgetItem* chosen_before = nullptr;
    const presets::Library lib = presets::load(config_dir());  // (the file only: no picture is read here)
    for (const presets::Group& g : lib.groups) {
        auto* group = new QTreeWidgetItem(tree_, {QString::fromStdString(g.name)});
        group->setFlags(Qt::ItemIsEnabled);
        group->setData(0, kGroupRole, QString::fromStdString(g.name));
        group->setToolTip(0, g.builtin ? QStringLiteral("入っているブラシ（名前・中身は変えられません。複製して自分のブラシに）") : QStringLiteral("自分のブラシ"));
        for (const presets::Preset& p : g.items) {
            auto* item = new QTreeWidgetItem(group, {QString::fromStdString(p.name)});
            item->setData(0, kKeyRole, QString::fromStdString(p.key));
            const TipOf tip = tip_of(p.settings);
            if (!tip.dab) {
                item->setIcon(0, QIcon(stroke_preview(p.key, {20, 20, 20}, QSize(28, 14))));  // (Genko's: its line, cached)
                item->setToolTip(0, QStringLiteral("Genko のブラシ"));
            } else {
                const std::string key = picture_key(tip.ref, tip.hardness, tip.roundness, 28);
                bool made = false;
                {
                    std::lock_guard lock(made_mutex());
                    if (const auto it = made_pictures().find(key); it != made_pictures().end()) {
                        item->setIcon(0, icon_of(it->second));
                        made = true;
                    }
                }
                if (!made) pending_.push_back(Pending{p.key, tip.ref, tip.hardness, tip.roundness});
                item->setToolTip(0, tip.ref.empty() ? QStringLiteral("円の先端") : QStringLiteral("サンプルの先端"));
            }
            if (p.key == in_hand) chosen_item = item;
            if (p.key == chosen_now) chosen_before = item;
        }
        group->setExpanded(true);
    }
    if (!lib.problem.empty()) {
        auto* note = new QTreeWidgetItem(tree_, {QStringLiteral("（ブラシのライブラリのファイルを読めないので、入っているブラシだけを見せています）")});
        note->setFlags(Qt::ItemIsEnabled);
    }
    if (chosen_item == nullptr) chosen_item = chosen_before;
    if (chosen_item != nullptr) tree_->setCurrentItem(chosen_item);
    loading_ = false;
    if (!pending_.empty()) picture_timer_.start();
}

void BrushesPanel::make_pictures() {
    if (making_ || pending_.empty()) return;
    making_ = true;
    // (a few at a time, off the window's thread: a library of large tips is read once, never on every refresh)
    std::vector<Pending> batch(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(16, pending_.size())));
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(batch.size()));
    const std::filesystem::path config = config_dir();
    auto* watcher = new QFutureWatcher<std::vector<std::pair<std::string, QImage>>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, batch] {
        const auto made = watcher->result();
        watcher->deleteLater();
        std::map<std::string, QImage> by_key;
        {
            std::lock_guard lock(made_mutex());
            for (std::size_t i = 0; i < made.size(); ++i) {
                made_pictures()[made[i].first] = made[i].second;
                by_key[batch[i].key] = made[i].second;
            }
        }
        for (QTreeWidgetItemIterator it(tree_); *it != nullptr; ++it) {
            const std::string key = (*it)->data(0, kKeyRole).toString().toStdString();
            if (const auto found = by_key.find(key); found != by_key.end()) (*it)->setIcon(0, icon_of(found->second));
        }
        making_ = false;
        if (!pending_.empty()) picture_timer_.start();
    });
    watcher->setFuture(QtConcurrent::run([batch, config] {
        std::vector<std::pair<std::string, QImage>> out;
        for (const Pending& p : batch) {
            out.emplace_back(picture_key(p.ref, p.hardness, p.roundness, 28), tip_picture(config, p.ref, p.hardness, p.roundness, 28));
        }
        return out;
    }));
}

void BrushesPanel::chosen(QTreeWidgetItem* item) {
    if (loading_ || item == nullptr) return;
    const std::string key = item->data(0, kKeyRole).toString().toStdString();
    if (!key.empty()) window_->choose_brush(key);  // (the brush in hand: ツールの設定's list follows)
}

void BrushesPanel::new_preset(std::optional<QString> name) {
    BrushPanel* panel = window_->brush_panel();
    if (panel == nullptr) return;
    const std::string kind = panel->kind();
    const core::Brush now = render::brushes::brush(kind);
    if (!name) {
        const auto asked = ask::get_text(this, QStringLiteral("新規ブラシプリセット"), QStringLiteral("ブラシ名"), QString::fromStdString(now.label) + QStringLiteral(" 2"));
        if (!asked) return;
        name = asked->trimmed();
    }
    Json settings = core::brush_to_dict(now);
    settings["width_mm"] = std::clamp(panel->size->value(), 0.05, 50.0);  // (Photoshop's: the size in hand)
    try {
        settings = render::brushes::with_pictures(settings);  // (its pictures, so the library holds them)
        const QString group = group_of_choice();
        const bool own_group = !group.isEmpty() && std::none_of(presets::builtin_groups().begin(), presets::builtin_groups().end(),
                                                                [&](const presets::Group& g) { return QString::fromStdString(g.name) == group; });
        const Json out = presets::save(config_dir(), std::nullopt, settings, name->isEmpty() ? std::nullopt : std::optional<std::string>(name->toStdString()),
                                       own_group ? std::optional<std::string>(group.toStdString()) : std::nullopt);
        window_->brushes_changed(out.value("key", std::string()));
        window_->flash(QStringLiteral("ブラシ「%1」を加えました").arg(QString::fromStdString(out.value("name", std::string()))), 3000);
    } catch (const std::exception& error) {
        ask::warning(this, QStringLiteral("新規ブラシプリセット"), QStringLiteral("加えられませんでした: %1").arg(wording::error(QString::fromUtf8(error.what()))));
    }
}

void BrushesPanel::rename(std::optional<QString> name) {
    const std::string key = chosen_key();
    if (key.empty()) return;
    if (!presets::is_own(key)) {
        window_->flash(QStringLiteral("名前を変えられるのは自分のブラシです（入っているブラシは＋で自分のブラシにしてから）"), 4000);
        return;
    }
    if (!name) {
        const auto asked = ask::get_text(this, QStringLiteral("名前を変更"), QStringLiteral("ブラシ名"), tree_->currentItem()->text(0));
        if (!asked) return;
        name = asked->trimmed();
    }
    try {
        presets::edit(config_dir(), Json{{"action", "rename"}, {"preset", key}, {"name", name->toStdString()}});
        window_->brushes_changed(key);
    } catch (const std::exception& error) {
        ask::warning(this, QStringLiteral("名前を変更"), QStringLiteral("変えられませんでした: %1").arg(wording::error(QString::fromUtf8(error.what()))));
    }
}

void BrushesPanel::remove() {
    const std::string key = chosen_key();
    if (key.empty()) return;
    if (!presets::is_own(key)) {
        window_->flash(QStringLiteral("消せるのは自分のブラシです"), 3000);
        return;
    }
    try {
        presets::remove(config_dir(), {key});
        window_->brushes_changed(std::nullopt, key);
        window_->flash(QStringLiteral("ブラシをライブラリから消しました（描いた線はそのまま）"), 3000);
    } catch (const std::exception& error) {
        ask::warning(this, QStringLiteral("削除"), QStringLiteral("消せませんでした: %1").arg(wording::error(QString::fromUtf8(error.what()))));
    }
}

bool BrushesPanel::import_file(const QString& path, std::optional<bool> replace) {
    last_warnings.clear();
    if (QFileInfo(path).suffix().toLower() != QLatin1String("abr")) {
        try {
            const std::vector<std::string> keys = window_->import_brushes(path);  // (.genkobrush: as before)
            window_->brushes_changed(keys.empty() ? std::nullopt : std::optional<std::string>(keys.front()));
            window_->flash(keys.empty() ? QStringLiteral("読み込めるブラシがありませんでした") : QStringLiteral("ブラシを %1 本読み込みました").arg(keys.size()), 4000);
            return !keys.empty();
        } catch (const std::exception& error) {
            ask::warning(this, QStringLiteral("ブラシを読み込み"), QStringLiteral("読み込めませんでした: %1").arg(wording::error(QString::fromUtf8(error.what()))));
            return false;
        }
    }
    const std::string group = QFileInfo(path).completeBaseName().toStdString();
    // (a group of the file's name there already: put in its place, or added to it — asked, as Photoshop adds)
    if (!replace) {
        bool there = false;
        for (const presets::Group& g : presets::load(config_dir()).groups) there = there || (!g.builtin && g.name == group);
        replace = there && ask::question(this, QStringLiteral("ブラシを読み込み"),
                                         QStringLiteral("グループ「%1」はもうあります。中のブラシを読み込むブラシで置き換えますか？\n（いいえ: グループに足します）").arg(QString::fromStdString(group)));
    }
    Json out;
    try {
        // (read, mapped and written off the window's thread: a large file does not stop the window)
        const std::filesystem::path file = core::path_from_utf8(path.toStdString());
        const std::filesystem::path config = config_dir();
        const bool put_in_place = *replace;
        out = off_ui([file, config, group, put_in_place] {
            const std::string bytes = storage::read_file_bounded(file, render::abr::kMaxFileBytes);
            return presets::import_abr(config, bytes, group, put_in_place);
        });
    } catch (const std::exception& error) {
        ask::warning(this, QStringLiteral("ブラシを読み込み"), QStringLiteral("読み込めませんでした: %1").arg(wording::error(QString::fromUtf8(error.what()))));
        return false;
    }
    for (const Json& w : out.value("warnings", Json::array())) last_warnings.push_back(w.get<std::string>());
    const Json& made = out["brushes"];
    window_->brushes_changed(made.empty() ? std::nullopt : std::optional<std::string>(made.front().value("key", std::string())));
    QString words = QStringLiteral("ブラシを %1 本、グループ「%2」に読み込みました").arg(made.size()).arg(QString::fromStdString(out.value("group", group)));
    if (!last_warnings.empty()) {
        words += QStringLiteral("\n\nそのままでは読み込めなかったもの:");
        for (std::size_t i = 0; i < last_warnings.size() && i < 12; ++i) words += QStringLiteral("\n・") + QString::fromStdString(last_warnings[i]);
        if (last_warnings.size() > 12) words += QStringLiteral("\n・ほか %1 件").arg(last_warnings.size() - 12);
        ask::warning(this, QStringLiteral("ブラシを読み込み"), words);
    } else {
        window_->flash(words, 4000);
    }
    return !made.empty();
}

bool BrushesPanel::export_file(const QString& path) {
    const std::string key = chosen_key();
    if (!key.empty() && window_->brush_panel() != nullptr && window_->brush_panel()->kind() != key) window_->choose_brush(key);
    return window_->export_brush(path);
}

void BrushesPanel::new_group(std::optional<QString> name) {
    if (!name) {
        const auto asked = ask::get_text(this, QStringLiteral("新規グループ"), QStringLiteral("グループ名"), QStringLiteral("グループ 1"));
        if (!asked || asked->trimmed().isEmpty()) return;
        name = asked->trimmed();
    }
    try {
        presets::edit(config_dir(), Json{{"action", "new_group"}, {"name", name->toStdString()}});
        refresh();
    } catch (const std::exception& error) {
        ask::warning(this, QStringLiteral("新規グループ"), QStringLiteral("作れませんでした: %1").arg(wording::error(QString::fromUtf8(error.what()))));
    }
}

void BrushesPanel::rename_group(std::optional<QString> name) {
    const QString group = group_of_choice();
    if (group.isEmpty()) return;
    if (!name) {
        const auto asked = ask::get_text(this, QStringLiteral("グループ名を変更"), QStringLiteral("グループ名"), group);
        if (!asked || asked->trimmed().isEmpty()) return;
        name = asked->trimmed();
    }
    try {
        presets::edit(config_dir(), Json{{"action", "rename_group"}, {"group", group.toStdString()}, {"name", name->toStdString()}});
        refresh();
    } catch (const std::exception& error) {
        ask::warning(this, QStringLiteral("グループ名を変更"), QStringLiteral("変えられませんでした: %1").arg(wording::error(QString::fromUtf8(error.what()))));
    }
}

void BrushesPanel::remove_group() {
    const QString group = group_of_choice();
    if (group.isEmpty()) return;
    if (!ask::question(this, QStringLiteral("グループを削除"), QStringLiteral("グループ「%1」とその中の自分のブラシを消しますか？（描いた線はそのまま）").arg(group))) return;
    try {
        const Json out = presets::edit(config_dir(), Json{{"action", "delete_group"}, {"group", group.toStdString()}});
        std::vector<std::string> gone;
        for (const Json& k : out.value("deleted", Json::array())) gone.push_back(k.get<std::string>());
        window_->brushes_changed(std::nullopt, gone.empty() ? std::string() : gone.front());
        for (std::size_t i = 1; i < gone.size(); ++i) window_->brushes_changed(std::nullopt, gone[i]);
    } catch (const std::exception& error) {
        ask::warning(this, QStringLiteral("グループを削除"), QStringLiteral("消せませんでした: %1").arg(wording::error(QString::fromUtf8(error.what()))));
    }
}

void BrushesPanel::move_to_group(const QString& group) {
    const std::string key = chosen_key();
    if (key.empty() || !presets::is_own(key)) {
        window_->flash(QStringLiteral("グループに移動できるのは自分のブラシです"), 3000);
        return;
    }
    try {
        presets::edit(config_dir(), Json{{"action", "move"}, {"preset", key}, {"group", group.toStdString()}});
        refresh();
    } catch (const std::exception& error) {
        ask::warning(this, QStringLiteral("グループに移動"), QStringLiteral("移せませんでした: %1").arg(wording::error(QString::fromUtf8(error.what()))));
    }
}

}  // namespace genko::app
