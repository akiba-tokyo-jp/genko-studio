#pragma once

#include <QString>

#include <memory>
#include <optional>
#include <string>

#include "core/json.hpp"

class QWidget;

// PC4 F-e (SPEC.md FILE-01「カメラ RAW」, design 5.3 and 8-3 (a)): Camera Raw's window for a camera RAW file — opening one
// (開く…: the RAW's development chosen before it opens, as Photoshop) and a RAW smart object's コンテンツを編集. 節 D's
// panel (app/camera_raw_window: CameraRawPanel — 基本補正 … 効果, worked on the preview) with the RAW stage put at the top of
// 基本補正 (ホワイトバランス 撮影時の設定・自動・カスタム, 色温度 (K), 色かぶり補正 and 露光量 sent to the RAW stage — the panel's
// relative ones hidden —, 自動, and 詳細's デモザイク and ハイライト), the ワークフロー under it (色空間, ビット数, 解像度,
// 出力のシャープ, スマートオブジェクトとして開く) and the buttons 画像を開く (Shift: オブジェクトを開く), キャンセル and 完了 (the
// settings remembered for the file, nothing opened) — or OK and キャンセル for a smart object. The preview is the RAW stage
// developed by LibRaw on a thread at half its size and at most 900 pixels (render/raw preview: a change of the RAW stage
// 150 ms after the last, the last asked for wins), the panel's adjustments worked on it as the op works them.

namespace genko::app::camera_raw_open {

// 環境設定 → Camera Raw (edit.preferences.rawDefaults; PhotoCraft's prefs.rs RawDefaults, MIT): the ワークフロー a RAW file
// opens with — 色空間 (Adobe RGB), ビット数 (16), 解像度 (300 ppi), 出力のシャープ (なし), スマートオブジェクトとして開く (no),
// 自動補正 (no: 自動's exposure applied when the window opens) — kept in the machine's settings (raw/*).
struct Workflow {
    std::string space = "adobe-rgb-compat";
    int depth = 16;
    double dpi = 300;
    std::string sharpen = "none";
    bool smart = false;
    bool auto_tone = false;
};
Workflow workflow();
void set_workflow(const Workflow& workflow);

// What 完了 remembered for a RAW file (by its bytes' asset ref: {"develop", "adjust"?, "resolution"}), the latest 100.
std::optional<core::Json> remembered(const std::string& ref);
void remember(const std::string& ref, const core::Json& settings);

enum class Ending { Cancel, Open, OpenObject, Done };
struct Chosen {
    Ending ending = Ending::Cancel;
    core::Json raw = core::Json::object();  // {"develop", "adjust"?}
    double dpi = 300;
};

// The window for the RAW file of these bytes named `name`, starting from `start` ({"develop"?, "adjust"?}: a smart object's
// raw, or what 完了 remembered; none: the ワークフロー's) and the resolution `dpi`: opening it, or (`editing`) a RAW smart
// object's development (OK: Ending::Open). Run through ask::exec (tests answer it).
Chosen run(QWidget* parent, std::shared_ptr<const std::string> bytes, const QString& name, const core::Json& start, double dpi, bool editing);

}  // namespace genko::app::camera_raw_open
