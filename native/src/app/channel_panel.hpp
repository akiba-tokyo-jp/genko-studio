#pragma once

#include <QWidget>

#include <map>
#include <string>
#include <vector>

#include "core/model.hpp"

class QListWidget;
class QListWidgetItem;
class QMenu;
class QPushButton;

// The channel panel (channels@1, SPEC.md COLOR-03; PhotoCraft's Channels panel): the page's composite, its colour plates
// (RGB: レッド・グリーン・ブルー; CMYK: シアン・マゼンタ・イエロー・ブラック; Lab: 明度・a・b), its alpha and spot channels
// (each its eye, a small picture and its name), then the areas kept on it (ストック). A click on a row makes it what the
// pixel tools work on (the window's channel target: Ctrl+Alt+2 the composite, Ctrl+Alt+3… the plates then the channels);
// the eye shows or hides it. New, spot, duplicate, delete, load as the selection, save the selection; the menu:
// options, split, merge, merge spot. Every change of the book is an op through the window (apply_ops): Undo, saving and
// the journal as for any edit. What is targeted and shown is the window's, never saved.

namespace genko::app {

class MainWindow;

class ChannelPanel : public QWidget {
    Q_OBJECT
public:
    explicit ChannelPanel(MainWindow* window);
    // The page's rows again (after a change, another page, another view).
    void refresh();
    // Tests and the window: the list, the buttons, the menu; each row's key ("composite", a plate's name, a channel's
    // id, "stock:<name>").
    QListWidget* list() const { return list_; }
    QMenu* menu() const { return menu_; }
    QPushButton* button(const QString& name) const;
    const std::vector<std::string>& keys() const { return keys_; }
    // The buttons' and the menu's work (tests call them as the buttons do).
    void add(bool spot);
    void duplicate();
    void remove();
    void load_selection();
    void save_selection();
    void options();
    void split();
    void merge();
    void merge_spot();

private:
    void clicked(QListWidgetItem* item);
    void eye(QListWidgetItem* item);
    // The alpha or spot channel the row chosen names (its id), or "".
    std::string chosen_channel() const;
    QIcon thumbnail(const core::Page& page, const core::Channel& channel);

    MainWindow* window_;
    QListWidget* list_ = nullptr;
    QMenu* menu_ = nullptr;
    std::vector<std::string> keys_;
    bool loading_ = false;
    // channel id → its small picture and the raster it was made of
    struct Thumb {
        core::Bytes raster;
        QIcon icon;
    };
    std::map<std::string, Thumb> thumbs_;
};

}  // namespace genko::app
