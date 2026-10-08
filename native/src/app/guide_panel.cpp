#include "app/guide_panel.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

#include <cmath>
#include <map>

#include "app/ask.hpp"
#include "app/canvas.hpp"
#include "app/config.hpp"
#include "app/main_window.hpp"
#include "app/theme.hpp"
#include "core/mesh3d.hpp"
#include "core/poses.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "core/pynum.hpp"

namespace genko::app {

using core::Json;

namespace {

constexpr double kPi = 3.14159265358979323846;

using Labels = std::vector<std::pair<const char*, const char*>>;

const Labels& ruler_names() {
    static const Labels l = {{"line", "直線定規"}, {"curve", "曲線定規"}, {"parallel", "平行線定規"}, {"concentric", "同心円定規"},
                             {"radial", "放射線定規"}, {"perspective", "パース定規"}, {"symmetry", "対称定規"}};
    return l;
}
// the stick figure's poses (mannequin.PRESETS), the 3D figure's (mesh3d.FIGURE_PRESETS), and the hands'
const Labels& stick_poses() {
    static const Labels l = {{"stand", "立つ"}, {"walk", "歩く"}, {"run", "走る"}, {"sit", "座る"}, {"point", "指さす"}, {"look_back", "振り返る"},
                             {"arms_up", "両手を上げる"}};
    return l;
}
const Labels& figure_poses() {
    static const Labels l = {{"stand", "立つ"}, {"walk", "歩く"}, {"run", "走る"}, {"sit", "座る"}, {"point", "指さす"}, {"arms_up", "両手を上げる"},
                             {"think", "考える"}, {"kneel", "片ひざ"}, {"peace", "ピース"}};
    return l;
}
const Labels& hand_poses() {
    static const Labels l = {{"open", "開く"}, {"relaxed", "力を抜く"}, {"fist", "握る"}, {"point", "指さす"}, {"peace", "ピース"}, {"grip", "つかむ"}};
    return l;
}
const Labels& kind_labels() {
    static const Labels l = {{"box", "箱"},     {"cylinder", "円柱"}, {"sphere", "球"},    {"cone", "円錐"}, {"prop", "小物"},
                             {"stairs", "階段"}, {"floor", "床"},      {"scene", "背景"},  {"figure", "デッサン人形（3D）"},
                             {"head", "頭部"},   {"hand", "手"},       {"mesh", "モデル"}, {"mannequin", "デッサン人形（棒）"}};
    return l;
}

QString label_of(const Labels& labels, const std::string& key) {
    for (const auto& [k, v] : labels)
        if (key == k) return QString::fromUtf8(v);
    return QString::fromStdString(key);
}

bool has(const Labels& labels, const std::string& key) {
    for (const auto& [k, v] : labels)
        if (key == k) return true;
    return false;
}

std::string str_at(const Json& j, const char* key) {
    const auto found = j.is_object() ? j.find(key) : j.end();
    return j.is_object() && found != j.end() && found->is_string() ? found->get<std::string>() : std::string();
}

double num_at(const Json& j, const char* key, double fallback) {
    if (!j.is_object()) return fallback;
    const auto found = j.find(key);
    if (found == j.end() || !core::py_truthy(*found)) return fallback;
    try {
        return core::to_float(*found);
    } catch (const std::exception&) {
        return fallback;
    }
}

std::array<double, 3> rot_of(const Json& prim) {
    std::array<double, 3> out{0, 0, 0};
    const auto found = prim.find("rot");
    if (found != prim.end() && found->is_array()) {
        for (std::size_t i = 0; i < 3 && i < found->size(); ++i) {
            try {
                out[i] = core::to_float((*found)[i]);
            } catch (const std::exception&) {
            }
        }
    }
    return out;
}

}  // namespace

QString ruler_label(const Json& ruler) {
    const std::string kind = str_at(ruler, "kind");
    QString label = label_of(ruler_names(), kind);
    if (kind == "perspective") {
        const auto points = ruler.find("points");
        label += QStringLiteral("（%1 点）").arg(points != ruler.end() && points->is_array() ? static_cast<int>(points->size()) : 0);
    }
    if (kind == "symmetry" && num_at(ruler, "copies", 2) > 2) label += QStringLiteral("（%1 方向）").arg(static_cast<int>(num_at(ruler, "copies", 2)));
    if (ruler.contains("frame_id") && core::py_truthy(ruler["frame_id"])) label += QStringLiteral(" ・ コマの中だけ");
    return label;
}

GuidePanel::GuidePanel(MainWindow* window) : window_(window) {
    // rulers
    rulers = new QListWidget;
    rulers->setObjectName(QStringLiteral("guide_rulers"));
    rulers->setMaximumHeight(130);
    connect(rulers, &QListWidget::currentRowChanged, this, [this](int) { ruler_picked(); });
    connect(rulers, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) { ruler_toggled(item); });
    angle = new QDoubleSpinBox;
    angle->setRange(-360, 360);
    angle->setSuffix(QStringLiteral("°"));
    connect(angle, &QDoubleSpinBox::editingFinished, this, [this] { ruler_set(Json{{"angle", angle->value()}}); });
    ratio = new QDoubleSpinBox;
    ratio->setRange(0.05, 20);
    ratio->setSingleStep(0.05);
    ratio->setToolTip(QStringLiteral("1 で円。小さいほど横長の楕円"));
    connect(ratio, &QDoubleSpinBox::editingFinished, this, [this] { ruler_set(Json{{"ratio", ratio->value()}}); });
    copies = new QSpinBox;
    copies->setRange(2, 32);
    copies->setToolTip(QStringLiteral("2 で左右対称。3 以上は中心の周りに回した写し"));
    connect(copies, &QSpinBox::editingFinished, this, [this] { ruler_set(Json{{"copies", copies->value()}}); });
    mirror = new QCheckBox(QStringLiteral("鏡写しも"));
    connect(mirror, &QCheckBox::toggled, this, [this](bool on) {
        if (!loading_) ruler_set(Json{{"mirror", on}});
    });
    in_panel = new QCheckBox(QStringLiteral("選んだコマの中だけ"));
    in_panel->setToolTip(QStringLiteral("この定規を、選んだコマの中だけで効かせます"));
    connect(in_panel, &QCheckBox::toggled, this, [this](bool on) { panel_only(on); });
    auto* remove_ruler = new QPushButton(QStringLiteral("この定規を消す"));
    connect(remove_ruler, &QPushButton::clicked, this, [this] { delete_ruler(); });
    auto* ruler_form = new QFormLayout;
    ruler_form->addRow(QStringLiteral("角度"), angle);
    ruler_form->addRow(QStringLiteral("縦横の比"), ratio);
    ruler_form->addRow(QStringLiteral("写しの数"), copies);
    ruler_form->addRow(QString(), mirror);
    auto* ruler_box = new QGroupBox(QStringLiteral("定規（R で置く・点をドラッグで動かす）"));
    auto* rl = new QVBoxLayout(ruler_box);
    rl->addWidget(rulers);
    rl->addLayout(ruler_form);
    rl->addWidget(in_panel);
    rl->addWidget(remove_ruler);
    ruler_hint = new QLabel(QStringLiteral("このページに定規はありません。「定規」メニューから置きます。"));
    ruler_hint->setWordWrap(true);
    rl->addWidget(ruler_hint);
    // 3D
    prims = new QListWidget;
    prims->setObjectName(QStringLiteral("guide_prims"));
    prims->setMaximumHeight(110);
    connect(prims, &QListWidget::currentRowChanged, this, [this](int) { prim_picked(); });
    preset = new QComboBox;
    preset->addItem(QStringLiteral("（ポーズを選ぶ）"), QString());
    for (const auto& [key, label] : stick_poses()) preset->addItem(QString::fromUtf8(label), QString::fromLatin1(key));
    connect(preset, &QComboBox::activated, this, [this](int) { choose_preset(); });
    turn = slider(-180, 180, 1);
    tip = slider(-90, 90, 0);
    lean = slider(-180, 180, 2);
    size = new QDoubleSpinBox;
    size->setRange(5, 400);
    size->setSuffix(QStringLiteral(" mm"));
    connect(size, &QDoubleSpinBox::editingFinished, this, [this] { resize_prim(); });
    focal = new QSlider(Qt::Horizontal);
    focal->setRange(60, 1500);
    focal->setToolTip(QStringLiteral("左ほど遠近が強い（広角）"));
    connect(focal, &QSlider::sliderReleased, this, [this] { prim_set(Json{{"focal_mm", focal->value()}}); });
    auto* buttons = new QHBoxLayout;
    auto* trace = new QPushButton(QStringLiteral("線にする"));
    trace->setToolTip(QStringLiteral("選んだ 3D を、描く先のレイヤーに鉛筆の線で写します（下描きに）"));
    connect(trace, &QPushButton::clicked, this, [this] { window_->trace_prims(true); });
    auto* remove = new QPushButton(QStringLiteral("消す"));
    connect(remove, &QPushButton::clicked, this, [this] { delete_prim(); });
    buttons->addWidget(trace);
    buttons->addWidget(remove);
    auto* more = new QGridLayout;
    body_button = new QPushButton(QStringLiteral("体型・手…"));
    body_button->setToolTip(QStringLiteral("デッサン人形の等身・肩幅・腰幅・体格・脚の長さと、手のポーズ（手のモデルはその形）"));
    connect(body_button, &QPushButton::clicked, this, [this] { body_dialog(); });
    auto* camera = new QPushButton(QStringLiteral("カメラ・光…"));
    camera->setToolTip(QStringLiteral("このページの 3D をまとめて見る向き・画角と、光の向き"));
    connect(camera, &QPushButton::clicked, this, [this] { camera_dialog(); });
    auto* surfaces = new QPushButton(QStringLiteral("線と面に…"));
    surfaces->setToolTip(QStringLiteral("3D を描く先のレイヤーに、線（見えない所は描かない）と陰の面（トーン化もできる）で写します"));
    connect(surfaces, &QPushButton::clicked, this, [this] { render_dialog(); });
    save_pose = new QPushButton(QStringLiteral("ポーズを保存…"));
    save_pose->setToolTip(QStringLiteral("今のポーズ（関節と手）を名前を付けて残します。どの原稿でも「ポーズ」の一覧に出ます"));
    connect(save_pose, &QPushButton::clicked, this, [this] { keep_pose(); });
    ik = new QCheckBox(QStringLiteral("手先・足先を引くと腕・脚ごと動く（IK）"));
    ik->setChecked(settings()->value(QStringLiteral("3d/ik"), true).toString().toLower() == QLatin1String("true"));
    connect(ik, &QCheckBox::toggled, this, [](bool on) { settings()->setValue(QStringLiteral("3d/ik"), on); });
    more->addWidget(body_button, 0, 0);
    more->addWidget(camera, 0, 1);
    more->addWidget(surfaces, 1, 0);
    more->addWidget(save_pose, 1, 1);
    more->addWidget(ik, 2, 0, 1, 2);
    auto* prim_form = new QFormLayout;
    prim_form->addRow(QStringLiteral("ポーズ"), preset);
    prim_form->addRow(QStringLiteral("向き"), turn);
    prim_form->addRow(QStringLiteral("傾き（前後）"), tip);
    prim_form->addRow(QStringLiteral("傾き（左右）"), lean);
    prim_form->addRow(QStringLiteral("大きさ"), size);
    prim_form->addRow(QStringLiteral("パース"), focal);
    auto* prim_box = new QGroupBox(QStringLiteral("3D（J で関節や箱をドラッグ）"));
    auto* pl = new QVBoxLayout(prim_box);
    pl->addWidget(prims);
    pl->addLayout(prim_form);
    pl->addLayout(buttons);
    pl->addLayout(more);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(ruler_box);
    layout->addWidget(prim_box);
    layout->addStretch(1);
    refresh();
}

QSlider* GuidePanel::slider(int lo, int hi, int index) {
    auto* s = new QSlider(Qt::Horizontal);
    s->setRange(lo, hi);
    connect(s, &QSlider::sliderReleased, this, [this, s, index] { rot(index, s->value()); });
    connect(s, &QSlider::valueChanged, s, [s](int v) { s->setToolTip(QStringLiteral("%1°").arg(v)); });
    return s;
}

// --- state ----------------------------------------------------------------------------------------------------------

void GuidePanel::refresh() {
    const core::Page* page = window_->current_page();
    loading_ = true;
    const auto keep_ruler = window_->canvas()->selected_ruler_id;
    rulers->clear();
    if (page != nullptr && page->rulers.is_array()) {
        for (const Json& r : page->rulers) {
            auto* item = new QListWidgetItem(ruler_label(r));
            const std::string id = str_at(r, "id");
            item->setData(Qt::UserRole, QString::fromStdString(id));
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(core::py_truthy(r.value("active", Json(true))) ? Qt::Checked : Qt::Unchecked);
            item->setToolTip(QStringLiteral("チェックを外すと効かなくなります（線は吸い付かない）"));
            rulers->addItem(item);
            if (keep_ruler && *keep_ruler == id) rulers->setCurrentItem(item);
        }
    }
    ruler_hint->setVisible(rulers->count() == 0);
    const auto keep_prim = window_->canvas()->selected_prim_id;
    prims->clear();
    std::map<std::string, int> counts;
    if (page != nullptr && page->prims.is_array()) {
        for (const Json& p : page->prims) {
            std::string kind = str_at(p, "kind");
            if (kind.empty()) kind = "box";
            const int n = ++counts[kind];
            QString label = QStringLiteral("%1 %2").arg(label_of(kind_labels(), kind)).arg(n);
            if (kind == "mesh" && !str_at(p, "title").empty()) label = QStringLiteral("モデル「%1」").arg(QString::fromStdString(str_at(p, "title")));
            const std::string pose = str_at(p, "preset");
            if (!pose.empty()) {
                label += QStringLiteral("（%1）").arg(has(stick_poses(), pose) ? label_of(stick_poses(), pose) : label_of(figure_poses(), pose));
            }
            auto* item = new QListWidgetItem(label);
            const std::string id = str_at(p, "id");
            item->setData(Qt::UserRole, QString::fromStdString(id));
            prims->addItem(item);
            if (keep_prim && *keep_prim == id) prims->setCurrentItem(item);
        }
    }
    loading_ = false;
    show_ruler();
    show_prim();
}

const Json* GuidePanel::ruler() const {
    const core::Page* page = window_->current_page();
    QListWidgetItem* item = rulers->currentItem();
    if (page == nullptr || item == nullptr || !page->rulers.is_array()) return nullptr;
    const std::string id = item->data(Qt::UserRole).toString().toStdString();
    for (const Json& r : page->rulers)
        if (str_at(r, "id") == id) return &r;
    return nullptr;
}

const Json* GuidePanel::prim() const {
    const core::Page* page = window_->current_page();
    QListWidgetItem* item = prims->currentItem();
    if (page == nullptr || item == nullptr || !page->prims.is_array()) return nullptr;
    const std::string id = item->data(Qt::UserRole).toString().toStdString();
    for (const Json& p : page->prims)
        if (str_at(p, "id") == id) return &p;
    return nullptr;
}

void GuidePanel::show_ruler() {
    const Json* r = ruler();
    const std::string kind = r != nullptr ? str_at(*r, "kind") : std::string();
    loading_ = true;
    angle->setEnabled(kind == "parallel" || kind == "concentric");
    ratio->setEnabled(kind == "concentric");
    copies->setEnabled(kind == "symmetry");
    mirror->setEnabled(kind == "symmetry");
    in_panel->setEnabled(r != nullptr);
    if (r != nullptr) {
        angle->setValue(num_at(*r, "angle", 0));
        ratio->setValue(num_at(*r, "ratio", 1));
        copies->setValue(static_cast<int>(num_at(*r, "copies", 2)));
        mirror->setChecked(r->contains("mirror") && core::py_truthy((*r)["mirror"]));
        in_panel->setChecked(r->contains("frame_id") && core::py_truthy((*r)["frame_id"]));
    }
    loading_ = false;
}

void GuidePanel::show_prim() {
    const Json* p = prim();
    for (QWidget* w : std::initializer_list<QWidget*>{preset, turn, tip, lean, size, focal}) w->setEnabled(p != nullptr);
    if (p == nullptr) return;
    loading_ = true;
    const std::string kind = str_at(*p, "kind");
    const bool figure = kind == "mannequin" || kind == "figure";
    preset->clear();
    preset->addItem(QStringLiteral("（ポーズを選ぶ）"), QString());
    for (const auto& [key, label] : kind == "figure" ? figure_poses() : kind == "hand" ? hand_poses() : stick_poses())
        preset->addItem(QString::fromUtf8(label), QString::fromLatin1(key));
    if (kind == "figure") {
        try {
            const Json own = core::poses::user_poses();  // (the person's own, kept with the app's settings)
            for (const Json& pose : core::iterate(own)) {
                const std::string name = str_at(pose, "name");
                preset->addItem(QStringLiteral("自分: %1").arg(QString::fromStdString(name)), QStringLiteral("own:") + QString::fromStdString(name));
            }
        } catch (const std::exception&) {
        }
    }
    preset->setEnabled(figure || kind == "hand");
    body_button->setEnabled(kind == "figure" || kind == "hand");
    save_pose->setEnabled(kind == "figure");
    focal->setEnabled(kind != "mannequin");
    const auto r = rot_of(*p);
    tip->setValue(static_cast<int>(core::py_round_int(r[0] * 180.0 / kPi)));
    turn->setValue(static_cast<int>(core::py_round_int(r[1] * 180.0 / kPi)));
    lean->setValue(static_cast<int>(core::py_round_int(r[2] * 180.0 / kPi)));
    std::vector<double> sides{40, 80, 20};
    const auto given = p->find("size");
    if (given != p->end() && given->is_array() && !given->empty()) {
        sides.clear();
        for (const Json& v : *given) {
            try {
                sides.push_back(core::to_float(v));
            } catch (const std::exception&) {
                sides.push_back(0);
            }
        }
    }
    size->setValue(figure ? (sides.size() > 1 ? sides[1] : sides[0]) : *std::max_element(sides.begin(), sides.end()));
    size->setToolTip(figure ? QStringLiteral("身長") : QStringLiteral("いちばん長い辺（形はそのまま）"));
    focal->setValue(static_cast<int>(num_at(*p, "focal_mm", 400)));
    loading_ = false;
}

// --- edits ----------------------------------------------------------------------------------------------------------

void GuidePanel::ruler_picked() {
    if (loading_) return;
    const Json* r = ruler();
    window_->canvas()->selected_ruler_id = r != nullptr ? std::optional<std::string>(str_at(*r, "id")) : std::nullopt;
    window_->canvas()->update();
    show_ruler();
}

void GuidePanel::ruler_toggled(QListWidgetItem* item) {
    if (loading_ || window_->current_page() == nullptr) return;
    window_->apply_ops(Json::array({Json{{"op", "edit_ruler"}, {"page", window_->current_page()->index.json()},
                                         {"id", item->data(Qt::UserRole).toString().toStdString()}, {"active", item->checkState() == Qt::Checked}}}));
}

void GuidePanel::ruler_set(const Json& change) {
    const Json* r = ruler();
    if (loading_ || r == nullptr) return;
    Json op{{"op", "edit_ruler"}, {"page", window_->current_page()->index.json()}, {"id", str_at(*r, "id")}};
    for (const auto& [key, value] : change.items()) op[key] = value;
    window_->apply_ops(Json::array({op}));
}

void GuidePanel::panel_only(bool on) {
    if (loading_ || ruler() == nullptr) return;
    const core::Frame* frame = window_->selected_frame();
    if (on && frame == nullptr) {
        window_->flash(QStringLiteral("先にコマを選びます（選択ツールでコマをクリック）"), 3000);
        loading_ = true;
        in_panel->setChecked(false);
        loading_ = false;
        return;
    }
    ruler_set(Json{{"frame_id", on ? Json(frame->id) : Json()}});
}

void GuidePanel::delete_ruler() {
    const Json* r = ruler();
    const std::optional<std::string> id = r != nullptr ? std::optional<std::string>(str_at(*r, "id")) : window_->canvas()->selected_ruler_id;
    if (id && window_->current_page() != nullptr &&
        window_->apply_ops(Json::array({Json{{"op", "delete_ruler"}, {"page", window_->current_page()->index.json()}, {"id", *id}}}))) {
        window_->canvas()->selected_ruler_id.reset();
    }
}

void GuidePanel::prim_picked() {
    if (loading_) return;
    const Json* p = prim();
    window_->canvas()->selected_prim_id = p != nullptr ? std::optional<std::string>(str_at(*p, "id")) : std::nullopt;
    window_->canvas()->update();
    show_prim();
}

void GuidePanel::select_prim(const std::string& prim_id) {
    for (int row = 0; row < prims->count(); ++row) {
        if (prims->item(row)->data(Qt::UserRole).toString().toStdString() == prim_id) {
            prims->setCurrentRow(row);
            return;
        }
    }
    prims->setCurrentRow(-1);
}

void GuidePanel::prim_set(const Json& change) {
    const Json* p = prim();
    if (loading_ || p == nullptr) return;
    Json op{{"op", "edit_prim"}, {"page", window_->current_page()->index.json()}, {"id", str_at(*p, "id")}};
    for (const auto& [key, value] : change.items()) op[key] = value;
    window_->apply_ops(Json::array({op}));
}

void GuidePanel::rot(int index, int degrees) {
    const Json* p = prim();
    if (loading_ || p == nullptr) return;
    auto r = rot_of(*p);
    r[static_cast<std::size_t>(index)] = core::py_round(degrees * kPi / 180.0, 4);
    prim_set(Json{{"rot", Json::array({r[0], r[1], r[2]})}});
}

void GuidePanel::resize_prim() {
    const Json* p = prim();
    if (loading_ || p == nullptr) return;
    const double value = size->value();
    const std::string kind = str_at(*p, "kind");
    if (kind == "mannequin" || kind == "figure") {
        prim_set(Json{{"size", Json::array({value / 2, value, value / 4})}});
        return;
    }
    std::vector<double> sides{40, 40, 40};
    const auto given = p->find("size");
    if (given != p->end() && given->is_array() && !given->empty()) {
        sides.clear();
        for (const Json& v : *given) sides.push_back(core::to_float(v));
    }
    const double k = value / *std::max_element(sides.begin(), sides.end());
    Json out = Json::array();
    for (const double v : sides) out.push_back(core::py_round(v * k, 3));
    prim_set(Json{{"size", out}});
}

void GuidePanel::choose_preset() {
    const Json* p = prim();
    const QString key = preset->currentData().toString();
    if (p == nullptr || key.isEmpty()) return;
    const std::string kind = str_at(*p, "kind");
    const Json page = window_->current_page()->index.json();
    const std::string id = str_at(*p, "id");
    if (kind == "mannequin") {
        window_->apply_ops(Json::array({Json{{"op", "pose_mannequin"}, {"page", page}, {"id", id}, {"preset", key.toStdString()}}}));
    } else if (kind == "figure" && key.startsWith(QStringLiteral("own:"))) {
        if (const auto pose = core::poses::find(key.mid(4).toStdString())) {
            window_->apply_ops(Json::array({Json{{"op", "pose_figure"}, {"page", page}, {"id", id},
                                                 {"set_joints", core::py_or(core::py_get(*pose, "joints"), Json::object())},
                                                 {"hands", core::py_or(core::py_get(*pose, "hands"), Json::object())}}}));
        }
    } else if (kind == "figure") {
        window_->apply_ops(Json::array({Json{{"op", "pose_figure"}, {"page", page}, {"id", id}, {"preset", key.toStdString()}}}));
    } else if (kind == "hand") {
        window_->apply_ops(Json::array({Json{{"op", "pose_figure"}, {"page", page}, {"id", id}, {"pose", key.toStdString()}}}));
    }
    preset->setCurrentIndex(0);
}

// --- the figure's body and hands, the camera and light, 3D into drawing ----------------------------------------------

void GuidePanel::keep_pose(const std::optional<QString>& asked) {
    // ポーズを保存: the selected figure's pose under a name (the same name: replaced)
    const Json* p = prim();
    if (p == nullptr || str_at(*p, "kind") != "figure") {
        window_->flash(QStringLiteral("先にデッサン人形（3D）を選びます"), 3000);
        return;
    }
    QString name;
    if (asked) {
        name = *asked;
    } else {
        const auto typed = ask::get_text(this, QStringLiteral("ポーズを保存"), QStringLiteral("ポーズの名前"));
        if (!typed) return;
        name = *typed;
    }
    if (name.trimmed().isEmpty()) return;
    try {
        core::poses::save_pose(name.toStdString(), *p);
    } catch (const std::exception& error) {
        window_->flash(QString::fromUtf8(error.what()), 5000, true);
        return;
    }
    show_prim();
    window_->flash(QStringLiteral("ポーズ「%1」を残しました（ポーズの一覧の「自分: …」）").arg(name.trimmed()), 4000);
}

void GuidePanel::body_dialog() {
    const Json* found = prim();
    if (found == nullptr || (str_at(*found, "kind") != "figure" && str_at(*found, "kind") != "hand")) {
        window_->flash(QStringLiteral("先にデッサン人形（3D）か手を選びます"), 3000);
        return;
    }
    const Json p = *found;  // (the book may change while the dialog is open)
    const bool figure = str_at(p, "kind") == "figure";
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("body_dialog"));
    dialog.setWindowTitle(QStringLiteral("体型・手"));
    auto* form = new QFormLayout(&dialog);
    std::vector<std::pair<std::string, QDoubleSpinBox*>> fields;
    QComboBox* sex = nullptr;
    const Json body = p.contains("body") && p["body"].is_object() ? p["body"] : Json::object();
    if (figure) {
        sex = new QComboBox;
        sex->setObjectName(QStringLiteral("sex"));
        for (const auto& [label, key] : {std::pair{QStringLiteral("決めない（数だけ）"), QString()}, {QStringLiteral("男性"), QStringLiteral("male")},
                                         {QStringLiteral("女性"), QStringLiteral("female")}})
            sex->addItem(label, key);
        sex->setCurrentIndex(std::max(0, sex->findData(QString::fromStdString(str_at(body, "sex")))));
        sex->setToolTip(QStringLiteral("男女の体型（肩幅・腰幅・胸）。下の数はそのうえで効きます"));
        form->addRow(QStringLiteral("体型"), sex);
        for (const auto& [key, label, lo, hi, fallback] :
             {std::tuple{"heads", "等身", 4.0, 10.0, 7.5}, {"shoulders", "肩幅", 0.6, 1.5, 1.0}, {"hips", "腰幅", 0.6, 1.6, 1.0},
              {"build", "体格（太さ）", 0.5, 1.8, 1.0}, {"legs", "脚の長さ", 0.6, 1.5, 1.0}}) {
            auto* box = new QDoubleSpinBox;
            box->setObjectName(QString::fromLatin1(key));
            box->setRange(lo, hi);
            box->setSingleStep(hi <= 2 ? 0.1 : 0.5);
            box->setValue(num_at(body, key, fallback));
            form->addRow(QString::fromUtf8(label), box);
            fields.emplace_back(key, box);
        }
    }
    std::vector<std::tuple<std::string, QComboBox*, std::vector<QDoubleSpinBox*>>> hands;
    const std::vector<std::pair<std::string, QString>> sides = figure ? std::vector<std::pair<std::string, QString>>{{"l", QStringLiteral("左手")}, {"r", QStringLiteral("右手")}}
                                                                      : std::vector<std::pair<std::string, QString>>{{"pose", QStringLiteral("手の形")}};
    for (const auto& [side, label] : sides) {
        auto* combo = new QComboBox;
        combo->setObjectName(QStringLiteral("hand_") + QString::fromStdString(side));
        for (const auto& [key, name] : hand_poses()) combo->addItem(QString::fromUtf8(name), QString::fromLatin1(key));
        Json now;
        if (figure) {
            now = p.contains("hands") && p["hands"].is_object() ? p["hands"].value(side, Json()) : Json();
        } else if (p.contains("curls") && core::py_truthy(p["curls"])) {
            now = Json{{"pose", core::py_or(core::py_get(p, "pose"), "relaxed")}, {"curls", p["curls"]}};
        } else {
            now = core::py_get(p, "pose");
        }
        const std::string pose_now = now.is_object() ? str_at(now, "pose") : now.is_string() ? now.get<std::string>() : std::string();
        combo->setCurrentIndex(std::max(0, combo->findData(QString::fromStdString(pose_now.empty() ? "relaxed" : pose_now))));
        form->addRow(label, combo);
        auto* row = new QHBoxLayout;
        std::vector<QDoubleSpinBox*> spins;
        const auto curls = core::mesh3d::hand_curls(core::py_truthy(now) ? now : Json("relaxed"));
        for (const double curl : curls) {  // (親指・人差し指・中指・薬指・小指: 0 まっすぐ〜1 曲げきる)
            auto* spin = new QDoubleSpinBox;
            spin->setRange(0, 1);
            spin->setSingleStep(0.1);
            spin->setDecimals(1);
            spin->setValue(core::py_round(curl, 1));
            spin->setToolTip(QStringLiteral("指の曲がり（左から親指・人差し指・中指・薬指・小指。0 まっすぐ、1 曲げきる）"));
            row->addWidget(spin);
            spins.push_back(spin);
        }
        form->addRow(QStringLiteral("　指ごと"), row);
        connect(combo, &QComboBox::currentIndexChanged, combo, [combo, spins](int) {
            const auto c = core::mesh3d::hand_curls(Json(combo->currentData().toString().toStdString()));
            for (std::size_t i = 0; i < spins.size() && i < c.size(); ++i) spins[i]->setValue(core::py_round(c[i], 1));
        });
        hands.emplace_back(side, combo, spins);
    }
    auto* ok = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(ok, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(ok, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(ok);
    const MainWindow::Asked asked = window_->asking();
    if (ask::exec(&dialog) != QDialog::Accepted || !window_->still(asked)) return;
    const auto chosen = [](QComboBox* combo, const std::vector<QDoubleSpinBox*>& spins) -> Json {
        const std::string pose = combo->currentData().toString().toStdString();
        Json curls = Json::array();
        const auto base = core::mesh3d::hand_curls(Json(pose));
        bool same = true;
        for (std::size_t i = 0; i < spins.size(); ++i) {
            const double v = core::py_round(spins[i]->value(), 2);
            curls.push_back(v);
            if (i < base.size() && std::abs(v - base[i]) >= 0.05) same = false;
        }
        return same ? Json(pose) : Json{{"pose", pose}, {"curls", curls}};
    };
    Json op{{"op", "pose_figure"}, {"page", window_->current_page()->index.json()}, {"id", str_at(p, "id")}};
    if (figure) {
        Json out = Json::object();
        for (const auto& [key, box] : fields) out[key] = core::py_round(box->value(), 2);
        if (sex != nullptr) out["sex"] = sex->currentData().toString().toStdString();
        op["body"] = out;
        Json chosen_hands = Json::object();
        for (const auto& [side, combo, spins] : hands) chosen_hands[side] = chosen(combo, spins);
        op["hands"] = chosen_hands;
    } else {
        const auto& [side, combo, spins] = hands.front();
        const Json picked = chosen(combo, spins);
        op["pose"] = combo->currentData().toString().toStdString();
        if (picked.is_object()) op["curls"] = picked["curls"];
    }
    window_->apply_ops(Json::array({op}));
}

void GuidePanel::camera_dialog() {
    const core::Page* page = window_->current_page();
    if (page == nullptr) return;
    const Json camera = page->extra.is_object() && page->extra.contains("camera") && page->extra["camera"].is_object() ? page->extra["camera"] : Json::object();
    const Json light = page->extra.is_object() && page->extra.contains("light") && page->extra["light"].is_object() ? page->extra["light"] : Json::object();
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("camera_dialog"));
    dialog.setWindowTitle(QStringLiteral("カメラ・光（このページの 3D）"));
    auto* form = new QFormLayout(&dialog);
    auto* use = new QCheckBox(QStringLiteral("カメラを使う（3D をまとめて同じ向きから見る）"));
    use->setObjectName(QStringLiteral("use"));
    use->setChecked(!camera.empty());
    std::array<QDoubleSpinBox*, 3> angles{};
    const char* keys[] = {"turn", "tip", "roll"};
    const char* labels[] = {"回り込み（°）", "見下ろし（°）", "傾き（°）"};
    for (std::size_t i = 0; i < 3; ++i) {
        angles[i] = new QDoubleSpinBox;
        angles[i]->setObjectName(QString::fromLatin1(keys[i]));
        angles[i]->setRange(-180, 180);
        angles[i]->setValue(num_at(camera, keys[i], 0) * 180.0 / kPi);
        form->addRow(QString::fromUtf8(labels[i]), angles[i]);
    }
    auto* focal_box = new QDoubleSpinBox;
    focal_box->setObjectName(QStringLiteral("focal"));
    focal_box->setRange(20, 5000);
    focal_box->setSuffix(QStringLiteral(" mm"));
    focal_box->setValue(num_at(camera, "focal_mm", 400));
    focal_box->setToolTip(QStringLiteral("小さいほど広角（遠近が強い）"));
    form->insertRow(0, use);
    form->addRow(QStringLiteral("画角（焦点距離）"), focal_box);
    std::array<double, 3> direction{-0.5, -0.7, -0.6};
    if (light.contains("dir") && light["dir"].is_array()) {
        for (std::size_t i = 0; i < 3 && i < light["dir"].size(); ++i) direction[i] = core::to_float(light["dir"][i]);
    }
    std::array<QDoubleSpinBox*, 3> dir{};
    const char* dir_labels[] = {"光: 右へ", "光: 下へ", "光: 奥へ"};
    for (std::size_t i = 0; i < 3; ++i) {
        dir[i] = new QDoubleSpinBox;
        dir[i]->setRange(-1, 1);
        dir[i]->setSingleStep(0.1);
        dir[i]->setValue(direction[i]);
        form->addRow(QString::fromUtf8(dir_labels[i]), dir[i]);
    }
    auto* ambient = new QDoubleSpinBox;
    ambient->setObjectName(QStringLiteral("ambient"));
    ambient->setRange(0, 1);
    ambient->setSingleStep(0.05);
    ambient->setValue(num_at(light, "ambient", 0.35));
    form->addRow(QStringLiteral("明るさの底上げ"), ambient);
    auto* ok = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(ok, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(ok, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(ok);
    const MainWindow::Asked asked = window_->asking();
    if (ask::exec(&dialog) != QDialog::Accepted || !window_->still(asked)) return;
    const Json index = window_->current_page()->index.json();
    Json ops = Json::array({Json{{"op", "set_light"}, {"page", index}, {"dir", Json::array({dir[0]->value(), dir[1]->value(), dir[2]->value()})},
                                 {"ambient", ambient->value()}}});
    if (use->isChecked()) {
        ops.push_back(Json{{"op", "set_camera"}, {"page", index}, {"turn", angles[0]->value() * kPi / 180.0}, {"tip", angles[1]->value() * kPi / 180.0},
                           {"roll", angles[2]->value() * kPi / 180.0}, {"focal_mm", focal_box->value()}});
    } else {
        ops.push_back(Json{{"op", "set_camera"}, {"page", index}, {"off", true}});
    }
    window_->apply_ops(ops);
}

void GuidePanel::render_dialog() {
    const core::Page* page = window_->current_page();
    const core::Layer* layer = window_->paint_layer();
    if (page == nullptr || layer == nullptr || !page->prims.is_array() || page->prims.empty()) {
        window_->flash(QStringLiteral("3D を置き、描く先のレイヤーを選びます"), 3000);
        return;
    }
    const Json* p = prim();
    const std::optional<std::string> chosen = p != nullptr ? std::optional<std::string>(str_at(*p, "id")) : std::nullopt;
    const std::string layer_id = layer->id;
    QMessageBox box(QMessageBox::Question, QStringLiteral("3D を線と面に"),
                    QStringLiteral("陰の面も写しますか？（はい: 線と面・面はトーン化して網点で印刷 ／ いいえ: 線だけ）"),
                    QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, this);
    const MainWindow::Asked asked = window_->asking();
    const int answer = ask::exec(&box);
    if (answer != QMessageBox::Yes && answer != QMessageBox::No) return;
    if (!window_->still(asked)) return;
    Json op{{"op", "render_prims"}, {"page", window_->current_page()->index.json()}, {"layer_id", layer_id}, {"surfaces", answer == QMessageBox::Yes}};
    if (chosen) op["ids"] = Json::array({*chosen});
    if (answer == QMessageBox::Yes) op["tone"] = Json{{"lpi", 60}};
    window_->apply_ops(Json::array({op}));
}

void GuidePanel::delete_prim() {
    const Json* p = prim();
    const std::optional<std::string> id = p != nullptr ? std::optional<std::string>(str_at(*p, "id")) : window_->canvas()->selected_prim_id;
    if (id && window_->current_page() != nullptr &&
        window_->apply_ops(Json::array({Json{{"op", "delete_prim"}, {"page", window_->current_page()->index.json()}, {"id", *id}}}))) {
        window_->canvas()->selected_prim_id.reset();
    }
}

}  // namespace genko::app
