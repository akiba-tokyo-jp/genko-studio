#pragma once

// Contact sheets of the 3D figure (SPEC FIGURE-01: the pictures a person looks at, not a test): the nine presets, male
// and female, from the front and three quarters, shaded and as the traced lines; the hands' six poses, both sides, from
// the back, the palm and the side; the head. Written by test_figure_mesh when GENKO_FIGURE_OUT names a folder.

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/json.hpp"
#include "core/mesh3d.hpp"
#include "core/pyops.hpp"
#include "render/image.hpp"
#include "render/png.hpp"

namespace genko::test::figures {

using core::Json;
namespace mesh3d = core::mesh3d;

inline constexpr const char* kPresetNames[] = {"stand", "walk", "run", "sit", "point", "arms_up", "think", "kneel", "peace"};

// A figure prim as add_figure makes it (pos [0, 0, 0]: its pelvis at the page's origin).
inline Json figure_prim(const std::string& preset, const std::string& sex, double turn, double height = 90.0) {
    Json prim = Json::object();
    prim["id"] = "f";
    prim["kind"] = "figure";
    prim["pos"] = Json::array({0.0, 0.0, 0.0});
    prim["size"] = Json::array({height / 2, height, height / 4});
    prim["rot"] = Json::array({0.0, turn, 0.0});
    prim["body"] = sex.empty() ? Json::object() : Json::object({{"sex", sex}});
    Json joints = Json::object();
    Json hands = Json::object();
    if (const Json* data = core::get(mesh3d::figure_presets(), preset)) {
        for (const auto& [k, v] : data->items()) {
            if (k == "hands") {
                hands = v;
            } else {
                joints[k] = v;
            }
        }
    }
    prim["joints"] = joints;
    prim["hands"] = hands;
    return prim;
}

struct Cell {
    std::vector<Json> prims;
    double x = 0;  // the box shown (mm)
    double y = 0;
    double w = 0;
    double h = 0;
    double dpi = 50;
    bool lines = false;
};

struct Sheet {
    int width = 0;
    int height = 0;
    std::string rgb;

    Sheet(int w, int h) : width(w), height(h), rgb(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3, static_cast<char>(255)) {}
    void put(int x, int y, int r, int g, int b) {
        if (x < 0 || y < 0 || x >= width || y >= height) return;
        const std::size_t at = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 3;
        rgb[at] = static_cast<char>(r);
        rgb[at + 1] = static_cast<char>(g);
        rgb[at + 2] = static_cast<char>(b);
    }
    void write(const std::filesystem::path& file) const {
        const render::Image image = render::Image::frombytes("RGB", render::Size{width, height}, rgb);
        const std::string png = render::write_png(image);
        std::ofstream out(file, std::ios::binary);
        out.write(png.data(), static_cast<std::streamsize>(png.size()));
    }
};

inline int px_of(double mm, double dpi) { return static_cast<int>(std::lround(mm / 25.4 * dpi)); }

inline void draw_cell(Sheet& sheet, int ox, int oy, const Cell& cell) {
    const int cw = px_of(cell.w, cell.dpi);
    const int ch = px_of(cell.h, cell.dpi);
    if (!cell.lines) {
        const mesh3d::Raster r = mesh3d::raster(cell.prims, cw, ch, cell.dpi, nullptr, nullptr, 0.35, cell.x, cell.y);
        for (int y = 0; y < ch; ++y) {
            for (int x = 0; x < cw; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(cw) + static_cast<std::size_t>(x);
                if (!r.alpha(i)) continue;
                const int g = static_cast<int>(std::clamp(r.shade[i], 0.0f, 1.0f) * 255.0f);
                sheet.put(ox + x, oy + y, g, g, g);
            }
        }
    } else {
        const double k = cell.dpi / 25.4;
        for (const Json& prim : cell.prims) {
            const std::vector<Json> one{prim};
            for (const auto& line : mesh3d::lines(one, nullptr, 40.0)) {
                for (std::size_t i = 0; i + 1 < line.size(); ++i) {
                    const double ax = (line[i][0] - cell.x) * k;
                    const double ay = (line[i][1] - cell.y) * k;
                    const double bx = (line[i + 1][0] - cell.x) * k;
                    const double by = (line[i + 1][1] - cell.y) * k;
                    const int n = std::max(1, static_cast<int>(std::ceil(std::hypot(bx - ax, by - ay) * 3)));
                    for (int s = 0; s <= n; ++s) {
                        const double t = static_cast<double>(s) / n;
                        const int x = static_cast<int>(std::floor(ax + (bx - ax) * t));
                        const int y = static_cast<int>(std::floor(ay + (by - ay) * t));
                        if (x < 0 || y < 0 || x >= cw || y >= ch) continue;
                        sheet.put(ox + x, oy + y, 40, 40, 90);
                    }
                }
            }
        }
    }
    for (int x = 0; x < cw; ++x) sheet.put(ox + x, oy + ch - 1, 200, 200, 220);  // (the cells' borders)
    for (int y = 0; y < ch; ++y) sheet.put(ox + cw - 1, oy + y, 200, 200, 220);
}

// rows of cells (each row's cells the same size)
inline void write_sheet(const std::filesystem::path& file, const std::vector<std::vector<Cell>>& rows) {
    int width = 0;
    int height = 0;
    for (const auto& row : rows) {
        int w = 0;
        int h = 0;
        for (const Cell& c : row) {
            w += px_of(c.w, c.dpi);
            h = std::max(h, px_of(c.h, c.dpi));
        }
        width = std::max(width, w);
        height += h;
    }
    Sheet sheet(width, height);
    int oy = 0;
    for (const auto& row : rows) {
        int ox = 0;
        int h = 0;
        for (const Cell& c : row) {
            draw_cell(sheet, ox, oy, c);
            ox += px_of(c.w, c.dpi);
            h = std::max(h, px_of(c.h, c.dpi));
        }
        oy += h;
    }
    sheet.write(file);
}

inline void write_all(const std::filesystem::path& dir) {
    std::filesystem::create_directories(dir);
    for (const char* sex : {"male", "female"}) {  // the presets: front, 3/4, each shaded and as lines
        std::vector<std::vector<Cell>> rows;
        for (const char* preset : kPresetNames) {
            std::vector<Cell> row;
            for (const double turn : {0.0, 0.785}) {
                for (const bool lines : {false, true}) {
                    row.push_back(Cell{{figure_prim(preset, sex, turn)}, -60.0, -84.0, 120.0, 144.0, 50.0, lines});
                }
            }
            rows.push_back(row);
        }
        write_sheet(dir / (std::string("presets_") + sex + ".png"), rows);
    }
    {  // close: stand, sit, run, think from the front, 3/4 and the side (female, male), shaded and lines
        for (const char* sex : {"female", "male"}) {
            std::vector<std::vector<Cell>> rows;
            for (const char* preset : {"stand", "sit", "run", "think"}) {
                std::vector<Cell> row;
                for (const double turn : {0.0, 0.785, 1.571}) {
                    for (const bool lines : {false, true}) {
                        row.push_back(Cell{{figure_prim(preset, sex, turn, 160.0)}, -50.0, -100.0, 100.0, 200.0, 60.0, lines});
                    }
                }
                rows.push_back(row);
            }
            write_sheet(dir / (std::string("close_") + sex + ".png"), rows);
        }
    }
    {  // the parts up close: head and shoulders, a hand at the side, the feet (stand, 3/4 and side)
        std::vector<std::vector<Cell>> rows;
        for (const double turn : {0.0, 0.785, 1.571, 3.14}) {
            const Json prim = figure_prim("stand", "female", turn, 200.0);
            const Json run = figure_prim("run", "male", turn, 200.0);
            std::vector<Cell> row;
            for (const bool lines : {false, true}) row.push_back(Cell{{prim}, -40.0, -125.0, 80.0, 60.0, 150.0, lines});   // head
            for (const bool lines : {false, true}) row.push_back(Cell{{prim}, -50.0, -5.0, 100.0, 40.0, 150.0, lines});    // hands
            for (const bool lines : {false, true}) row.push_back(Cell{{prim}, -40.0, 90.0, 80.0, 40.0, 150.0, lines});    // feet
            for (const bool lines : {false, true}) row.push_back(Cell{{run}, -60.0, -95.0, 120.0, 90.0, 75.0, lines});     // fists
            rows.push_back(row);
        }
        write_sheet(dir / "parts.png", rows);
    }
    {  // the hands on their own: six poses, left and right, from the back, the palm, the side and the thumb's side
        std::vector<std::vector<Cell>> rows;
        for (const char* pose : {"open", "relaxed", "fist", "point", "peace", "grip"}) {
            for (const char* side : {"l", "r"}) {
                std::vector<Cell> row;
                for (const double turn : {0.0, 3.14159, 1.5708, -1.5708}) {
                    Json hand = Json::object({{"kind", "hand"}, {"id", "h"}, {"pos", Json::array({0.0, 0.0, 0.0})}, {"size", Json::array({40.0, 40.0, 40.0})},
                                              {"rot", Json::array({0.0, turn, 0.0})}, {"side", side}, {"pose", pose}});
                    for (const bool lines : {false, true}) row.push_back(Cell{{hand}, -26.0, -28.0, 52.0, 56.0, 110.0, lines});
                }
                rows.push_back(row);
            }
        }
        write_sheet(dir / "hands.png", rows);
    }
    {  // the head on its own
        std::vector<Cell> row;
        for (const double turn : {0.0, 0.785, 1.571, 3.14}) {
            Json head = Json::object({{"kind", "head"}, {"id", "h"}, {"pos", Json::array({0.0, 0.0, 0.0})}, {"size", Json::array({30.0, 30.0, 30.0})},
                                      {"rot", Json::array({0.0, turn, 0.0})}});
            for (const bool lines : {false, true}) row.push_back(Cell{{head}, -20.0, -20.0, 40.0, 40.0, 120.0, lines});
        }
        write_sheet(dir / "heads.png", {row});
    }
}

}  // namespace genko::test::figures
