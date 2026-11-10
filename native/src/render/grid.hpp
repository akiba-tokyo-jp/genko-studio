#pragma once

#include <cstdint>
#include <vector>

#include "render/image.hpp"

// A grid of booleans, row by row: the numpy bool arrays of Python's fills, filters and tracing (genko/fill.py,
// filters.py, vectorize.py).

namespace genko::render {

struct BoolGrid {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> cells;  // 0 or 1

    BoolGrid() = default;
    BoolGrid(int w, int h, bool value = false)
        : width(w), height(h), cells(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), value ? 1 : 0) {}

    unsigned char& at(int x, int y) { return cells[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)]; }
    unsigned char at(int x, int y) const {
        return cells[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)];
    }
    // a.any()
    bool any() const;
};

// (grid * 255).astype("uint8") as an "L" picture.
Image grid_image(const BoolGrid& grid);

// np.asarray(picture) < threshold (or > when `above`), for an "L" picture.
BoolGrid compare(const Image& picture, int threshold, bool above = false);

}  // namespace genko::render
