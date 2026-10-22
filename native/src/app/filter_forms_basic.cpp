// PC4 節 D1 (SPEC.md COLOR-08「フィルター」, design 5.2): the windows of フィルター → シャープ・ノイズ・その他・ビデオ (app/filter_forms.hpp)
// — each kind's fields in Photoshop's words, and カスタム…'s 5 × 5 grid of its kernel (a field of the group's own). ぼかし's are
// in app/filter_forms.cpp; NTSC カラー and 平均 have no window.

#include <QGridLayout>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QWidget>

#include <cmath>
#include <vector>

#include "app/filter_forms.hpp"

namespace genko::app::filter_forms {

using core::Json;

namespace {

// (lengths are the working 200 dpi's pixels: design 2.2, 9 章)
const QString kLengths = QStringLiteral("長さは 200 dpi の画素で数えます（600 dpi のページでは 3 倍の画素に掛かり、紙の上の大きさは同じです）。");

// カスタム…'s kernel: 5 × 5 whole numbers −999..999, row by row (its centre the 13th); empty boxes are 0.
void kernel_grid(adjust_forms::Form& form, const adjust_forms::FormContext&) {
    auto* grid = new QWidget;
    auto* layout = new QGridLayout(grid);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    std::vector<QSpinBox*> boxes;
    for (int i = 0; i < 25; ++i) {
        auto* box = new QSpinBox;
        box->setRange(-999, 999);
        box->setValue(i == 12 ? 1 : 0);
        box->setObjectName(QStringLiteral("kernel_%1").arg(i));
        box->setAlignment(Qt::AlignRight);
        layout->addWidget(box, i / 5, i % 5);
        QObject::connect(box, &QSpinBox::valueChanged, &form, [&form] { emit form.changed(); });
        boxes.push_back(box);
    }
    Json fallback = Json::array();
    for (int i = 0; i < 25; ++i) fallback.push_back(i == 12 ? 1 : 0);
    form.custom(
        "kernel", QStringLiteral("カーネル"), grid,
        [boxes] {
            Json out = Json::array();
            for (QSpinBox* box : boxes) out.push_back(box->value());
            return out;
        },
        [boxes](const Json& v) {
            for (std::size_t i = 0; i < boxes.size(); ++i) {
                const QSignalBlocker quiet(boxes[i]);
                const Json* at = v.is_array() && i < v.size() ? &v[i] : nullptr;
                boxes[i]->setValue(at != nullptr && at->is_number() ? static_cast<int>(std::lround(at->get<double>())) : (i == 12 ? 1 : 0));
            }
        },
        fallback);
}

}  // namespace

std::vector<FilterForm> sharpen_forms() {
    std::vector<FilterForm> out;
    {  // アンシャープマスク… (量 1–500 %, 半径 0.1–1000 pixel, しきい値 0–255 レベル)
        FilterForm f;
        f.kind = "sharpen_unsharp_mask";
        f.labels = {{"amount", QStringLiteral("量（%）")}, {"radius", QStringLiteral("半径（px）")}, {"threshold", QStringLiteral("しきい値（レベル）")}};
        f.decimals = {{"amount", 0}, {"radius", 1}, {"threshold", 0}};
        f.note = kLengths;
        out.push_back(std::move(f));
    }
    {  // スマートシャープ… (量, 半径 0.1–64 pixel, ノイズを軽減 %)
        FilterForm f;
        f.kind = "sharpen_smart";
        f.labels = {{"amount", QStringLiteral("量（%）")}, {"radius", QStringLiteral("半径（px）")}, {"reduce_noise", QStringLiteral("ノイズを軽減（%）")}};
        f.decimals = {{"amount", 0}, {"radius", 1}, {"reduce_noise", 0}};
        f.note = kLengths;
        out.push_back(std::move(f));
    }
    return out;
}

std::vector<FilterForm> noise_forms() {
    std::vector<FilterForm> out;
    {  // ダスト＆スクラッチ… (半径 1–500 pixel, しきい値 0–255 レベル)
        FilterForm f;
        f.kind = "noise_dust_and_scratches";
        f.labels = {{"radius", QStringLiteral("半径（px）")}, {"threshold", QStringLiteral("しきい値（レベル）")}};
        f.decimals = {{"radius", 0}, {"threshold", 0}};
        f.note = kLengths;
        out.push_back(std::move(f));
    }
    {  // ノイズを加える… (量 %, 分布, グレースケールノイズ)
        FilterForm f;
        f.kind = "noise_add";
        f.labels = {{"amount", QStringLiteral("量（%）")}, {"distribution", QStringLiteral("分布")}, {"monochromatic", QStringLiteral("グレースケールノイズ")}};
        f.choices = {{"distribution", {{QStringLiteral("均等に分布"), Json("uniform")}, {QStringLiteral("ガウス分布"), Json("gaussian")}}}};
        f.decimals = {{"amount", 1}};
        f.note = QStringLiteral("粒はレイヤーの画素ごとです（解像度の高いページほど細かくなります）。");
        out.push_back(std::move(f));
    }
    {  // ノイズを軽減… (強さ 0–10, ディテールを保持・カラーノイズを軽減・ディテールをシャープに %, JPEG の斑点を削除)
        FilterForm f;
        f.kind = "noise_reduce";
        f.labels = {{"strength", QStringLiteral("強さ")},
                    {"preserve_details", QStringLiteral("ディテールを保持（%）")},
                    {"reduce_color_noise", QStringLiteral("カラーノイズを軽減（%）")},
                    {"sharpen_details", QStringLiteral("ディテールをシャープに（%）")},
                    {"remove_jpeg_artifact", QStringLiteral("JPEG の斑点を削除")}};
        f.decimals = {{"strength", 0}, {"preserve_details", 0}, {"reduce_color_noise", 0}, {"sharpen_details", 0}};
        f.note = QStringLiteral("CMYK・マルチチャンネルのページでは版ごとになめらかにします（カラーノイズの軽減は RGB・Lab のページだけです）。");
        out.push_back(std::move(f));
    }
    {  // 明るさの中間値… (半径 1–500 pixel)
        FilterForm f;
        f.kind = "noise_median";
        f.labels = {{"radius", QStringLiteral("半径（px）")}};
        f.decimals = {{"radius", 0}};
        f.note = kLengths;
        out.push_back(std::move(f));
    }
    return out;
}

std::vector<FilterForm> other_forms() {
    std::vector<FilterForm> out;
    {  // HSB/HSL (入力モード, 行の順序)
        FilterForm f;
        f.kind = "other_hsb_hsl";
        f.labels = {{"input_mode", QStringLiteral("入力モード")}, {"row_order", QStringLiteral("行の順序")}};
        const std::vector<std::pair<QString, Json>> models{{QStringLiteral("RGB"), Json("rgb")}, {QStringLiteral("HSB"), Json("hsb")}, {QStringLiteral("HSL"), Json("hsl")}};
        f.choices = {{"input_mode", models}, {"row_order", models}};
        out.push_back(std::move(f));
    }
    {  // カスタム… (the 5 × 5 grid, 拡大・縮小 1–9999, オフセット −9999–9999)
        FilterForm f;
        f.kind = "other_custom";
        f.labels = {{"scale", QStringLiteral("拡大・縮小")}, {"offset", QStringLiteral("オフセット")}};
        f.decimals = {{"scale", 0}, {"offset", 0}};
        f.more = kernel_grid;
        f.note = QStringLiteral("各画素の 5 × 5 の近くの値にカーネルの数を掛けて足し、拡大・縮小で割ってオフセットを足します（一目盛りは 200 dpi の 1 画素）。");
        out.push_back(std::move(f));
    }
    {  // ハイパス… (半径 0.1–1000 pixel)
        FilterForm f;
        f.kind = "other_high_pass";
        f.labels = {{"radius", QStringLiteral("半径（px）")}};
        f.decimals = {{"radius", 1}};
        f.note = kLengths;
        out.push_back(std::move(f));
    }
    for (const char* kind : {"other_minimum", "other_maximum"}) {  // 明るさの最小値…・最大値… (半径, 保持: 直角度・真円率)
        FilterForm f;
        f.kind = kind;
        f.labels = {{"radius", QStringLiteral("半径（px）")}, {"preserve", QStringLiteral("保持")}};
        f.choices = {{"preserve", {{QStringLiteral("直角度"), Json("squareness")}, {QStringLiteral("真円率"), Json("roundness")}}}};
        f.decimals = {{"radius", 1}};
        f.note = kLengths;
        out.push_back(std::move(f));
    }
    {  // スクロール… (水平方向・垂直方向 pixel, 未定義領域)
        FilterForm f;
        f.kind = "other_offset";
        f.labels = {{"horizontal", QStringLiteral("水平方向（px 右へ）")}, {"vertical", QStringLiteral("垂直方向（px 下へ）")},
                    {"undefined_areas", QStringLiteral("未定義領域")}};
        f.choices = {{"undefined_areas", {{QStringLiteral("折り返す"), Json("wrap")}, {QStringLiteral("端のピクセルを繰り返して埋める"), Json("repeat")},
                                          {QStringLiteral("透明にする"), Json("transparent")}}}};
        f.decimals = {{"horizontal", 0}, {"vertical", 0}};
        f.note = kLengths;
        out.push_back(std::move(f));
    }
    return out;
}

std::vector<FilterForm> video_forms() {
    std::vector<FilterForm> out;
    {  // インターレース解除… (消去: 奇数・偶数フィールド, 新規フィールドの作成: 複製・補間)
        FilterForm f;
        f.kind = "video_de_interlace";
        f.labels = {{"eliminate", QStringLiteral("消去")}, {"create_by", QStringLiteral("新規フィールドの作成")}};
        f.choices = {{"eliminate", {{QStringLiteral("奇数フィールド"), Json("odd_fields")}, {QStringLiteral("偶数フィールド"), Json("even_fields")}}},
                     {"create_by", {{QStringLiteral("補間"), Json("interpolation")}, {QStringLiteral("複製"), Json("duplication")}}}};
        f.note = QStringLiteral("1 行は 200 dpi の 1 画素の高さです。");
        out.push_back(std::move(f));
    }
    return out;
}

}  // namespace genko::app::filter_forms
