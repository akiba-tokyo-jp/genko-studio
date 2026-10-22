// PC4 節 D (SPEC.md COLOR-08「フィルター」, design 5.2): the windows of フィルター's kinds (app/filter_forms.hpp).

#include "app/filter_forms.hpp"

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>

#include <algorithm>

#include "app/ask.hpp"
#include "core/adjust_kinds.hpp"
#include "core/filter_kinds.hpp"

namespace genko::app::filter_forms {

using core::Json;

namespace {

QString words(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

// (lengths are the working 200 dpi's pixels: design 2.2, 9 章)
const QString kLengths = QStringLiteral("長さは 200 dpi の画素で数えます（600 dpi のページでは 3 倍の画素に掛かり、紙の上の大きさは同じです）。");

// (review P2-2: a page-wide layer at 600 dpi — B4's 6071 × 8598 — holds a window of the imaging budget only so far; the
// lengths past it are refused as too large, SPEC.md COLOR-08)
QString large(const QString& what, int rgb, int cmyk) {
    return kLengths + QStringLiteral("\n600 dpi の B4 ほどのページいっぱいのレイヤーでは、%1が約 %2（CMYK は約 %3）を超えると大きすぎて掛けられません"
                                     "（原稿は変わりません。スマートフィルターは印刷・書き出しでそう知らせます）。")
                          .arg(what)
                          .arg(rgb)
                          .arg(cmyk);
}

// The table's fields of a kind (those with a word), then the group's own.
void build(adjust_forms::Form& form, const core::AdjustKind& kind, const core::FilterInfo* info, const FilterForm* own,
           const adjust_forms::FormContext& context) {
    using Type = core::AdjustParam::Type;
    std::size_t colour = 0;  // (the colours a kind reads: the main colour, then the sub colour)
    for (const core::AdjustParam& p : kind.params) {
        const std::string key(p.key);
        QString label;
        if (own != nullptr) {
            const auto found = own->labels.find(key);
            if (found == own->labels.end()) continue;
            label = found->second;
        } else {
            label = words(p.key);
        }
        int places = -1;
        if (own != nullptr) {
            if (const auto found = own->decimals.find(key); found != own->decimals.end()) places = found->second;
        }
        switch (p.type) {
            case Type::Number: form.number(key, label, p.lo, p.hi, p.fallback, places); break;
            case Type::Whole: form.number(key, label, p.lo, p.hi, p.fallback, 0); break;
            case Type::Bool: form.check(key, label, p.fallback != 0); break;
            case Type::Choice: {
                std::vector<std::pair<QString, Json>> options;
                if (own != nullptr && own->choices.contains(key)) {
                    options = own->choices.at(key);
                } else {
                    for (const std::string_view c : p.choices) options.emplace_back(words(c), Json(std::string(c)));
                }
                if (context.smart && own != nullptr && own->not_smart.contains(key)) {  // (not on a smart filter: its note says why)
                    const std::vector<Json>& left_out = own->not_smart.at(key);
                    std::erase_if(options, [&left_out](const auto& o) { return std::find(left_out.begin(), left_out.end(), o.second) != left_out.end(); });
                }
                form.choice(key, label, std::move(options), Json(std::string(p.text)));
                break;
            }
            case Type::Rgb: {
                const bool painted = info != nullptr && std::find(info->colours.begin(), info->colours.end(), p.key) != info->colours.end();
                std::array<int, 3> fallback{0, 0, 0};
                if (painted) {
                    fallback = colour++ == 0 ? context.pen : context.sub;
                } else if (p.numbers.size() == 3) {
                    fallback = {static_cast<int>(p.numbers[0]), static_cast<int>(p.numbers[1]), static_cast<int>(p.numbers[2])};
                }
                form.colour(key, label, fallback);
                break;
            }
            default: break;  // (lists, groups and a group's own: its more())
        }
    }
    if (own != nullptr && own->more) own->more(form, context);
    if (own != nullptr && !own->note.isEmpty()) form.note(own->note);
    if (context.smart && own != nullptr && !own->smart_note.isEmpty()) form.note(own->smart_note);
}

}  // namespace

// --- each group's forms -----------------------------------------------------------------------------------------------

std::vector<FilterForm> blur_forms() {
    std::vector<FilterForm> out;
    {  // ぼかし（ガウス）… (Photoshop: 半径 0.1–1000 pixel, a tenth at a time)
        FilterForm f;
        f.kind = "blur_gaussian";
        f.labels = {{"radius", QStringLiteral("半径（px）")}};
        f.decimals = {{"radius", 1}};
        f.note = large(QStringLiteral("半径"), 280, 250);
        out.push_back(std::move(f));
    }
    {  // ぼかし（ボックス）… (Photoshop: 半径 1–2000 pixel)
        FilterForm f;
        f.kind = "blur_box";
        f.labels = {{"radius", QStringLiteral("半径（px）")}};
        f.decimals = {{"radius", 0}};
        f.note = kLengths;
        out.push_back(std::move(f));
    }
    // D1: the rest of ぼかし (平均: no window)
    {  // ぼかし（移動）… (Photoshop: 角度 −360–360°, 距離 1–2000 pixel)
        FilterForm f;
        f.kind = "blur_motion";
        f.labels = {{"angle", QStringLiteral("角度（°）")}, {"distance", QStringLiteral("距離（px）")}};
        f.decimals = {{"angle", 0}, {"distance", 0}};
        f.note = large(QStringLiteral("距離"), 1700, 1500);
        out.push_back(std::move(f));
    }
    {  // ぼかし（放射状）… (量 1–100, 方法 回転・ズーム, 中心)
        FilterForm f;
        f.kind = "blur_radial";
        f.labels = {{"amount", QStringLiteral("量")}, {"method", QStringLiteral("方法")}, {"center_x", QStringLiteral("中心（横）")},
                    {"center_y", QStringLiteral("中心（縦）")}};
        f.choices = {{"method", {{QStringLiteral("回転"), Json("spin")}, {QStringLiteral("ズーム"), Json("zoom")}}}};
        f.decimals = {{"amount", 0}, {"center_x", 2}, {"center_y", 2}};
        f.note = QStringLiteral("中心は、選択範囲（無ければレイヤー）の幅と高さに対する割合（0〜1）です。");
        out.push_back(std::move(f));
    }
    {  // ぼかし（表面）… (半径 1–100 pixel, しきい値 2–255 レベル)
        FilterForm f;
        f.kind = "blur_surface";
        f.labels = {{"radius", QStringLiteral("半径（px）")}, {"threshold", QStringLiteral("しきい値（レベル）")}};
        f.decimals = {{"radius", 0}, {"threshold", 0}};
        f.note = kLengths;
        out.push_back(std::move(f));
    }
    {  // ぼかし（詳細）… (半径, しきい値, 画質, モード)
        FilterForm f;
        f.kind = "blur_smart";
        f.labels = {{"radius", QStringLiteral("半径（px）")}, {"threshold", QStringLiteral("しきい値")}, {"quality", QStringLiteral("画質")},
                    {"mode", QStringLiteral("モード")}};
        f.choices = {{"quality", {{QStringLiteral("高"), Json("high")}, {QStringLiteral("中"), Json("medium")}, {QStringLiteral("低"), Json("low")}}},
                     {"mode", {{QStringLiteral("標準"), Json("normal")}, {QStringLiteral("エッジのみ"), Json("edge_only")},
                               {QStringLiteral("エッジオーバーレイ"), Json("overlay_edge")}}}};
        f.decimals = {{"radius", 1}, {"threshold", 1}};
        f.note = kLengths;
        out.push_back(std::move(f));
    }
    {  // ぼかし（シェイプ）… (半径 5–1000 pixel, the shape: PhotoCraft's built-in ones)
        FilterForm f;
        f.kind = "blur_shape";
        f.labels = {{"radius", QStringLiteral("半径（px）")}, {"shape", QStringLiteral("シェイプ")}};
        f.choices = {{"shape",
                      {{QStringLiteral("円"), Json("circle")}, {QStringLiteral("リング"), Json("ring")}, {QStringLiteral("正方形"), Json("square")},
                       {QStringLiteral("ひし形"), Json("diamond")}, {QStringLiteral("三角形"), Json("triangle")}, {QStringLiteral("六角形"), Json("hexagon")},
                       {QStringLiteral("星"), Json("star")}, {QStringLiteral("ハート"), Json("heart")}, {QStringLiteral("十字"), Json("cross")}}}};
        f.decimals = {{"radius", 0}};
        f.note = large(QStringLiteral("半径"), 850, 760);
        out.push_back(std::move(f));
    }
    {  // ぼかし（レンズ）… (深度情報, 虹彩, 反射光, ノイズ)
        FilterForm f;
        f.kind = "blur_lens";
        f.labels = {{"depth_map", QStringLiteral("深度情報のソース")},
                    {"focal_distance", QStringLiteral("ぼかしの焦点距離")},
                    {"invert", QStringLiteral("反転")},
                    {"shape", QStringLiteral("虹彩の形状")},
                    {"radius", QStringLiteral("虹彩の半径（px）")},
                    {"blade_curvature", QStringLiteral("羽根の湾曲")},
                    {"rotation", QStringLiteral("回転（°）")},
                    {"brightness", QStringLiteral("反射光の明るさ")},
                    {"threshold", QStringLiteral("反射光のしきい値")},
                    {"noise", QStringLiteral("ノイズの量")},
                    {"distribution", QStringLiteral("ノイズの分布")},
                    {"monochromatic", QStringLiteral("グレースケールノイズ")}};
        f.choices = {{"depth_map", {{QStringLiteral("なし"), Json("none")}, {QStringLiteral("透明部分"), Json("transparency")},
                                    {QStringLiteral("レイヤーマスク"), Json("layer_mask")}}},
                     {"shape", {{QStringLiteral("六角形（6）"), Json("hexagon")}, {QStringLiteral("三角形（3）"), Json("triangle")},
                                {QStringLiteral("正方形（4）"), Json("square")}, {QStringLiteral("五角形（5）"), Json("pentagon")},
                                {QStringLiteral("七角形（7）"), Json("heptagon")}, {QStringLiteral("八角形（8）"), Json("octagon")}}},
                     {"distribution", {{QStringLiteral("均等に分布"), Json("uniform")}, {QStringLiteral("ガウス分布"), Json("gaussian")}}}};
        f.decimals = {{"focal_distance", 0}, {"radius", 0}, {"blade_curvature", 0}, {"rotation", 0}, {"brightness", 0}, {"threshold", 0}, {"noise", 0}};
        f.note = kLengths;
        // (a smart object has no layer mask of its own pixels to read as the depth: the filter would see none)
        f.not_smart = {{"depth_map", {Json("layer_mask")}}};
        f.smart_note = QStringLiteral("スマートフィルターでは、深度情報のソースにレイヤーマスクは選べません（スマートオブジェクトのマスクは深度に読みません）。");
        out.push_back(std::move(f));
    }
    return out;
}
std::vector<FilterForm> distort_forms() { return {}; }       // (D2)
std::vector<FilterForm> pixelate_forms() { return {}; }      // (D2)
std::vector<FilterForm> stylize_forms() { return {}; }       // (D2)
std::vector<FilterForm> render_forms() { return {}; }        // (D4)
std::vector<FilterForm> blur_gallery_forms() { return {}; }  // (D4)

const std::vector<FilterForm>& filter_forms() {
    static const std::vector<FilterForm> all = [] {
        std::vector<FilterForm> out;
        for (auto group : {blur_forms, sharpen_forms, noise_forms, other_forms, video_forms, distort_forms, pixelate_forms, stylize_forms,
                           render_forms, blur_gallery_forms}) {
            for (FilterForm& each : group()) out.push_back(std::move(each));
        }
        return out;
    }();
    return all;
}

const FilterForm* filter_form(std::string_view kind) {
    for (const FilterForm& each : filter_forms()) {
        if (each.kind == kind) return &each;
    }
    return nullptr;
}

QString title(std::string_view kind) {
    const core::FilterInfo* info = core::filter_info(kind);
    return info != nullptr ? words(info->name) : words(kind);
}

std::optional<Json> dialog(QWidget* parent, std::string_view kind, const Json& now, const adjust_forms::FormContext& context, const Preview& preview,
                           Json* opened) {
    const core::AdjustKind* found = core::adjust_kind(kind);
    if (found == nullptr || !found->filter || !core::pc4_filter(kind)) return std::nullopt;
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("filter_dialog"));
    dialog.setProperty("kind", words(kind));
    dialog.setWindowTitle(title(kind));
    auto* layout = new QFormLayout(&dialog);
    auto* form = new adjust_forms::Form;
    build(*form, *found, core::filter_info(kind), filter_form(kind), context);
    form->set_values(now.is_object() ? now : Json::object());
    if (opened != nullptr) *opened = form->values();
    layout->addRow(form);
    auto* row = new QDialogButtonBox;
    QPushButton* reset = row->addButton(QStringLiteral("既定に戻す"), QDialogButtonBox::ResetRole);
    reset->setObjectName(QStringLiteral("filter_reset"));
    QObject::connect(reset, &QPushButton::clicked, form, &adjust_forms::Form::reset);
    layout->addRow(row);
    auto* timer = new QTimer(&dialog);
    timer->setSingleShot(true);
    timer->setInterval(250);
    QCheckBox* check = nullptr;
    if (preview.show) {
        check = new QCheckBox(QStringLiteral("プレビュー（ページで見る）"));
        check->setObjectName(QStringLiteral("filter_preview"));
        check->setChecked(true);
        layout->addRow(QString(), check);
        QObject::connect(form, &adjust_forms::Form::changed, &dialog, [timer] { timer->start(); });
        QObject::connect(check, &QCheckBox::toggled, &dialog, [timer] { timer->start(); });
        QObject::connect(timer, &QTimer::timeout, &dialog, [&] {
            try {
                preview.show(check->isChecked() ? std::optional<Json>(form->values()) : std::nullopt);
            } catch (const std::exception&) {
                preview.show(std::nullopt);  // (a preview that cannot be made must not stop the window)
            }
        });
        timer->start(0);
    }
    if (preview.busy) {  // (a heavy kind's preview on a thread: said while it is worked out)
        auto* busy = new QLabel(QStringLiteral("プレビューを作っています…"));
        busy->setObjectName(QStringLiteral("filter_preview_busy"));
        busy->setProperty("role", "hint");
        busy->setVisible(false);
        layout->addRow(QString(), busy);
        auto* poll = new QTimer(&dialog);
        poll->setInterval(100);
        QObject::connect(poll, &QTimer::timeout, &dialog, [busy, &preview] { busy->setVisible(preview.busy()); });
        poll->start();
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("かける"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("やめる"));
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addRow(buttons);
    const bool ok = ask::exec(&dialog) == QDialog::Accepted;
    timer->stop();
    const Json result = form->values();
    if (preview.show) preview.show(std::nullopt);
    if (!ok) return std::nullopt;
    return result;
}

}  // namespace genko::app::filter_forms
