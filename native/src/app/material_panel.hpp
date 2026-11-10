#pragma once

#include <QWidget>

#include <functional>

class QComboBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QTimer;

namespace genko::app {

// The materials, built-in and the person's own, by folder and found by name, tag or kind (Python's material panel's
// list): browsing never edits a book; a double click is a callback (the material's id and kind) into the window.
// The built-in ones show their packaged pictures; the person's pictures, parts and tones are drawn only when they
// come into view, on a worker, and kept in the preview cache (never in the library). The person's library is read
// without changing it: only entries with their own ids ("u-…") and a known kind, pictures by a plain name inside it.
class MaterialBrowser : public QWidget {
    Q_OBJECT
public:
    MaterialBrowser(QWidget* parent, std::function<void(const QString&, const QString&)> activate);

    // The library and its folders read again (the chosen material and folder kept).
    void reload();
    // The material chosen ("" for none); choose one by id (in the list as it is).
    QString current_id() const;
    void select(const QString& material_id);
    // The folder chosen ("" for すべて); choose one by name.
    QString folder() const;
    void choose_folder(const QString& name);
    // A button beside the folder's list (＋フォルダ…).
    void add_beside_folder(QWidget* widget);

    QComboBox* folder_box() const { return folder_; }
    QLineEdit* search_box() const { return search_; }
    QListWidget* list() const { return list_; }

private:
    void fill_folders();
    void fill();

    QLabel* label_ = nullptr;
    QHBoxLayout* folder_row_ = nullptr;
    QComboBox* folder_ = nullptr;
    QLineEdit* search_ = nullptr;
    QListWidget* list_ = nullptr;
    QTimer* timer_ = nullptr;
    QString library_;
    QString cache_root_;
    QList<QVariant> entries_;  // (QJsonObject each: the built-in ones, then the person's)
};

// A browser standing alone (tests; the window's panel holds its own).
QWidget* make_builtin_material_panel(QWidget* parent = nullptr, std::function<void(const QString&, const QString&)> activate = {});

}  // namespace genko::app
