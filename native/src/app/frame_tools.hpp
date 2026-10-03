#pragma once

#include <QPointF>
#include <QString>

#include <string>
#include <vector>

#include "core/model.hpp"

// The panel geometry the canvas's panel tool needs (Python's genko/frames.py gutters, edge_normal and the reading
// order of ops._leaves_in_reading_order), on top of core's frames.

namespace genko::app {

// A gutter a person can drag: the split it belongs to, which one, its middle line (page mm) and width.
struct Gutter {
    std::string node;
    int index = 0;
    QPointF p0;
    QPointF p1;
    double width = 0.0;
    bool horizontal = false;
};

std::vector<Gutter> gutters(const core::Frame& root);

// Edge i (corner i → i+1): its middle, its outward normal and its end.
struct EdgeNormal {
    QPointF mid;
    QPointF normal;
    QPointF end;
};
EdgeNormal edge_normal(const std::vector<QPointF>& points, std::size_t i);

// The panel's corners (frames.shape) and outline (frames.outline: bowed edges walked, rounded corners) as floats.
std::vector<QPointF> shape_of(const core::Frame& frame);
std::vector<QPointF> outline_of(const core::Frame& frame);
// The panel's bows when they fit its corners (frames.curves_of).
std::vector<double> curves_of(const core::Frame& frame, std::size_t corners);

// The panels in reading order for a binding (right: right to left).
std::vector<const core::Frame*> leaves_in_reading_order(const core::Frame& root, core::Binding binding);

// The distance from p to the segment a–b.
double distance_to_segment(const QPointF& p, const QPointF& a, const QPointF& b);

// frame_tree (studio/layout.py): a panel tree as set_layout takes it, with each panel's look.
core::Json frame_tree(const core::Frame& frame);

}  // namespace genko::app
