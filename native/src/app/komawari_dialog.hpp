#pragma once

#include <QDialog>
#include <QString>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "app/templates.hpp"
#include "core/json.hpp"
#include "core/model.hpp"
#include "service/layout_ideas.hpp"

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSlider;
class QSpinBox;
class QThreadPool;
class QKeyEvent;
class QTimer;

// コマ割りの案を出す… (コマ割り K2, docs/cpp-migration/SPEC.md LAYOUT-01): a batch of layout ideas for the page in
// front (service::layout_ideas — 12 by default, 5 or 24 when asked), each shown as the page cut by it, drawn small on a
// worker thread; the settings (コマ数, 段数, プリセット, つまみ, 見せ場のコマ, 束の数), the seed to copy and paste (the same
// settings and seed give the same batch), a new batch, variations of the idea chosen or of the page as it is (変異度),
// back to the batch before the variations (ガチャに戻る), and the idea laid on the page as one change (one Undo takes
// it back). The window changes no book: `plan` is the change chosen, applied by the main window as the template
// window's is.

namespace genko::app {

// The ops that lay `skeleton` on the page (its lines deleted and its panels merged back first when it is not blank:
// layout::clear_page, then layout_ideas::apply_idea — the apply_layout road), as one batch with the ids it gives, so
// that one Undo takes it back. Throws core::Error or layout::LayoutError (in Japanese) when it cannot.
templates::Plan idea_plan(const core::Document& doc, std::size_t page_index, const core::Json& skeleton, const std::string& actor);

class KomawariDialog : public QDialog {
    Q_OBJECT

public:
    KomawariDialog(QWidget* parent, std::shared_ptr<const core::Document> doc, std::size_t page_index, std::string actor);
    ~KomawariDialog() override;

    std::optional<templates::Plan> plan;  // the change chosen (このページに当てる)
    QString chosen;                       // the idea chosen, in words (its summary)
    bool needs_clearing = false;          // the page has panels or lines: the main window asks first

    // the settings
    QComboBox* preset = nullptr;
    QComboBox* panels = nullptr;   // 指定なし, 1…8
    QSpinBox* min_tiers = nullptr;  // 0: 指定なし
    QSpinBox* max_tiers = nullptr;
    QSpinBox* focus = nullptr;     // 0: なし
    QComboBox* count = nullptr;    // 5, 12, 24
    std::vector<std::pair<std::string, QSlider*>> knobs;  // slant, bleed, split, big, overlap, overflow, frameless (0…100)
    QLineEdit* seed = nullptr;
    QPushButton* copy_seed = nullptr;
    QPushButton* paste_seed = nullptr;
    QPushButton* new_batch = nullptr;
    // the variations
    QComboBox* base = nullptr;     // 選んだ案, 今のページのコマ割り
    QSlider* variation = nullptr;  // 0…100
    QPushButton* vary = nullptr;
    QPushButton* back = nullptr;
    QPushButton* apply_button = nullptr;
    QLabel* state = nullptr;       // what the batch is
    QLabel* summary = nullptr;     // the idea chosen, in words

    QListWidget* list() const { return list_; }
    const service::layout_ideas::Batch& batch() const { return batch_; }
    bool varying() const { return varying_; }
    // The request the settings make (no seed, no base).
    service::layout_ideas::Request request() const;
    // A batch from the settings: with `seed` (empty: a new one).
    void make_batch(const std::string& with_seed);
    // The batch again from the seed in its field (what was pasted or typed).
    void from_seed_field();
    // Variations of the base chosen (the idea chosen, or the page as it is) by the 変異度.
    void make_variations();
    // The batch before the variations, as it was.
    void back_to_gacha();
    // このページに当てる: the idea chosen as the change (`plan`), the window accepted.
    void choose();
    // Wait while the pictures of this batch are drawn (false: not all in `ms`).
    bool wait_pictures(int ms);
    // The page as it is can be a base (its panels are tiers of cuts).
    bool page_can_be_base() const { return page_skeleton_.has_value(); }

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    void show_batch();
    void vary_from(const core::Json& from, const QString& words, const std::string& with_seed);
    void settings_changed();
    void preset_changed();
    void follow();
    void bound();

    std::shared_ptr<const core::Document> doc_;
    std::size_t page_index_ = 0;
    std::string actor_;
    core::PageSpec spec_;
    std::optional<core::Json> page_skeleton_;
    service::layout_ideas::Batch batch_;
    bool varying_ = false;
    std::optional<service::layout_ideas::Batch> gacha_;  // the batch before the variations
    int gacha_row_ = -1;
    std::optional<core::Json> base_skeleton_;            // the base of the variations shown
    QString base_words_;
    QListWidget* list_ = nullptr;
    QTimer* again_ = nullptr;  // the settings changed: the batch again a moment later
    std::unique_ptr<QThreadPool> pool_;
    std::shared_ptr<KomawariDialog*> self_;
    int generation_ = 0;
    int pending_ = 0;
};

}  // namespace genko::app
