#pragma once

#include <QFileDialog>
#include <QStringList>

#include <filesystem>
#include <optional>
#include <vector>

#include "formats/open_file.hpp"
#include "render/codecs/format.hpp"

class QComboBox;

// 開く… and 指定形式で開く… (PC4 F-b, file.open / file.openAs; 節 F 5.3): Genko's own open window (Qt's: an OS's own cannot
// choose a .genko folder and files in one) — a .genko folder chosen (or double-clicked) is a book and is not gone into,
// a file is a picture opened as a book of one page (formats::open_file), several chosen open in tabs of their own. Its
// kinds of files listed by Photoshop's names; 指定形式で開く… adds 形式 (the bytes read as that format).

namespace genko::app {

// How a file opened in a window is read: this computer's カラー設定 (the working spaces, a picture's own profile kept —
// the policies are the window's after it is open: color_policy_on_open) and 環境設定 → ファイル管理 (EXIF's profile tag).
formats::OpenOptions file_open_options();
// PC4 F-e: the camera RAW files' patterns ("*.dng *.DNG …") and their filter ("Camera Raw (…)").
QString raw_patterns();
QString raw_filter();

// The kinds of files 開く… lists ("すべての形式 (…);;Genko の原稿 (*.genko);;…").
QString open_filters();

class OpenDialog : public QFileDialog {
    Q_OBJECT

public:
    // `as`: 指定形式で開く… (its 形式 shown).
    OpenDialog(QWidget* parent, bool as);
    // What was chosen: books (their folders) and files.
    const std::vector<std::filesystem::path>& chosen() const { return chosen_; }
    // 指定形式で開く…'s 形式 (none: by what the bytes are).
    std::optional<render::codecs::Format> format() const;
    QComboBox* format_box() const { return format_; }
    // (tests, and accept) these paths chosen, with this 形式
    void choose(const QStringList& paths, std::optional<render::codecs::Format> as = std::nullopt);

protected:
    void accept() override;

private:
    std::vector<std::filesystem::path> chosen_;
    QComboBox* format_ = nullptr;
};

}  // namespace genko::app
