#pragma once

#include <QDialog>
#include <QString>

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "core/json.hpp"

class QCheckBox;
class QComboBox;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QThreadPool;

// ポーズ集… (人形のポーズ集 G4, docs/cpp-migration/SPEC.md FIGURE-02): the pose library for the selected figure — the
// categories with their counts (すべて first), a search in Japanese or English, the view (正面・斜め・横), 左右反転, 足もとを
// 保つ (on by default), ランダム (24 poses drawn from what is shown, by a seed), and the poses as a grid of small
// pictures of the figure's own body, drawn on worker threads (render::figure_thumbs, remembered). A double-click or
// Enter puts the pose on the figure: pose_figure {page, id, pose_id, mirror, keep: "ground" | "pelvis"} through the
// window's ops — one change, one Undo. The window changes no book itself and stays open to try another.

namespace genko::app {

class PoseGallery : public QDialog {
    Q_OBJECT

public:
    // The figure the poses go on: its page (as the ops take it), its id and body; nothing when there is none.
    struct Target {
        core::Json page;
        std::string id;
        core::Json body;
    };
    using TargetOf = std::function<std::optional<Target>()>;
    using Apply = std::function<bool(const core::Json& op)>;

    PoseGallery(QWidget* parent, TargetOf target, Apply apply);
    ~PoseGallery() override;

    QListWidget* categories = nullptr;  // (row 0: すべて; then each category, its id as the item's data)
    QLineEdit* search = nullptr;
    QComboBox* view = nullptr;          // 正面, 斜め, 横 (the turn as the item's data)
    QCheckBox* mirror = nullptr;        // 左右反転
    QCheckBox* keep = nullptr;          // 足もとを保つ
    QPushButton* random_button = nullptr;
    QPushButton* all_button = nullptr;  // (back from a random pick to the list)
    QPushButton* apply_button = nullptr;
    QLabel* state = nullptr;

    QListWidget* grid() const { return grid_; }
    // The poses shown (ids, in order).
    std::vector<std::string> shown() const;
    // The figure's body the pictures are drawn with (re-read from the target; the pictures again when it changed).
    void follow_target();
    // ランダム: `n` poses of those the category and search give, from `seed` ("fp-…"; empty: a new one). The seed used.
    std::string pick_random(const std::string& seed = {}, std::size_t n = 24);
    // The pose chosen in the grid onto the figure (false: nothing chosen, no figure, or the op refused).
    bool apply_chosen();
    // Wait while the pictures shown are drawn (false: not all in `ms`).
    bool wait_pictures(int ms);
    // The grey picture shown for row `row` (empty until drawn).
    std::string picture_of(int row) const;

    static constexpr int kWidth = 120;
    static constexpr int kHeight = 180;

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    void list_poses();
    void show_ids(const std::vector<std::string>& ids, const QString& words);
    void draw_pictures();

    TargetOf target_;
    Apply apply_;
    core::Json body_ = core::Json::object();
    QListWidget* grid_ = nullptr;
    std::vector<std::string> ids_;
    std::vector<std::string> pictures_;
    std::unique_ptr<QThreadPool> pool_;
    std::shared_ptr<PoseGallery*> self_;
    int generation_ = 0;
    int pending_ = 0;
};

}  // namespace genko::app
