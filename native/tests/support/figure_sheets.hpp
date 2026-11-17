#pragma once

// Contact sheets of the 3D figure (SPEC FIGURE-01: the pictures a person looks at, not a test): the nine presets, male
// and female, from the front and three quarters, shaded and as the traced lines; the hands' six poses, both sides, from
// the back, the palm and the side; the head. Written by test_figure_mesh when GENKO_FIGURE_OUT names a folder.
// The anatomy sheets (the body pass of 2026-11-16) are the larger looks a person judges the forms by: the torso all
// round, the back, the shoulder with the arm raised, the hip seated, squatting and with a leg out, the leg and the
// foot, the head at a larger size. GENKO_FIGURE_ONLY=torso,hip writes only the sheets named.

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

// A figure standing with the joints given (JSON text) and its hands, turned about the upright axis and tipped.
inline Json posed(const char* joints, const std::string& sex, double turn, double height, double tip = 0.0, const char* hands = "{}") {
    Json prim = figure_prim("stand", sex, turn, height);
    prim["joints"] = core::parse_python_json(joints);
    prim["hands"] = core::parse_python_json(hands);
    prim["rot"] = Json::array({tip, turn, 0.0});
    return prim;
}

// A cell of `w` × `h` mm about a joint of the prim as the page shows it (moved by dx, dy).
inline Cell around(const Json& prim, const char* joint, double w, double h, double dpi, bool lines = false, double dx = 0.0, double dy = 0.0) {
    const mesh3d::Point2 at = mesh3d::page_of_joint(prim, joint, nullptr);
    return Cell{{prim}, at[0] + dx - w / 2, at[1] + dy - h / 2, w, h, dpi, lines};
}

inline void write_anatomy(const std::filesystem::path& dir, const std::string& only) {
    const auto wanted = [&](const char* name) { return only.empty() || ("," + only + ",").find("," + std::string(name) + ",") != std::string::npos; };
    const double pi = 3.14159265358979;
    const char* const kSexes[] = {"male", "female", ""};
    if (wanted("torso")) {  // the torso all round, arms a little out: male, female, neither; and the traced lines of the front and the back
        std::vector<std::vector<Cell>> rows;
        for (const char* sex : kSexes) {
            std::vector<Cell> row;
            for (const double turn : {0.0, pi / 4, pi / 2, 3 * pi / 4, pi}) {
                row.push_back(Cell{{posed(R"({"l_arm": {"z": 0.3}, "r_arm": {"z": -0.3}})", sex, turn, 240.0)}, -52.0, -112.0, 104.0, 156.0, 60.0, false});
            }
            rows.push_back(row);
        }
        write_sheet(dir / "torso.png", rows);
    }
    if (wanted("back")) {  // the whole body from the back, three quarters back and the side, shaded and as lines
        std::vector<std::vector<Cell>> rows;
        for (const char* sex : kSexes) {
            std::vector<Cell> row;
            for (const double turn : {pi, 3 * pi / 4, pi / 2}) {
                for (const bool lines : {false, true}) row.push_back(Cell{{figure_prim("stand", sex, turn, 160.0)}, -50.0, -100.0, 100.0, 200.0, 50.0, lines});
            }
            rows.push_back(row);
        }
        write_sheet(dir / "back.png", rows);
    }
    if (wanted("shoulder")) {  // the shoulder: arms out to the side, overhead, forward, back, on the hips; front, 3/4 and back
        std::vector<std::vector<Cell>> rows;
        const char* const poses[] = {
            R"({"l_arm": {"z": 1.45}, "r_arm": {"z": -1.45}})",
            R"({"l_arm": {"z": 2.9}, "r_arm": {"z": -2.9}})",
            R"({"l_arm": {"x": 1.5}, "r_arm": {"x": 1.5}})",
            R"({"l_arm": {"x": -0.9}, "r_arm": {"x": 2.6}})",
            R"({"l_arm": {"z": 0.75, "x": -0.25}, "l_elbow": {"x": 0.5, "z": -1.5}, "r_arm": {"z": -0.75, "x": -0.25}, "r_elbow": {"x": 0.5, "z": 1.5}})",
        };
        for (const char* sex : {"male", "female"}) {
            for (const char* pose : poses) {
                std::vector<Cell> row;
                for (const double turn : {0.0, pi / 4, pi / 2, pi}) {
                    row.push_back(Cell{{posed(pose, sex, turn, 200.0)}, -80.0, -150.0, 160.0, 150.0, 45.0, false});
                }
                rows.push_back(row);
            }
        }
        write_sheet(dir / "shoulder.png", rows);
    }
    if (wanted("hip")) {  // the hip: seated, a deep squat, a leg out to the side, a leg back; front, 3/4, side, back and from below
        std::vector<std::vector<Cell>> rows;
        const char* const poses[] = {
            R"({"l_leg": {"x": 1.5708}, "l_knee": {"x": -1.5708}, "r_leg": {"x": 1.5708}, "r_knee": {"x": -1.5708}})",
            R"({"spine": {"x": 0.3}, "l_leg": {"x": 2.2, "z": 0.25}, "l_knee": {"x": -2.5}, "l_ankle": {"x": 0.45}, "r_leg": {"x": 2.2, "z": -0.25}, "r_knee": {"x": -2.5}, "r_ankle": {"x": 0.45}})",
            R"({"l_leg": {"z": 1.2}, "l_arm": {"z": 0.4}, "r_arm": {"z": -0.4}})",
            R"({"l_leg": {"x": -0.7}, "l_knee": {"x": -0.3}, "r_leg": {"x": 0.9}, "r_knee": {"x": -0.9}})",
        };
        for (const char* sex : {"male", "female"}) {
            for (const char* pose : poses) {
                std::vector<Cell> row;
                for (const double turn : {0.0, pi / 4, pi / 2, 3 * pi / 4, pi}) {
                    row.push_back(Cell{{posed(pose, sex, turn, 200.0)}, -80.0, -60.0, 160.0, 160.0, 45.0, false});
                }
                row.push_back(Cell{{posed(pose, sex, 0.0, 200.0, -1.2)}, -80.0, -80.0, 160.0, 160.0, 45.0, false});  // (from below)
                rows.push_back(row);
            }
        }
        write_sheet(dir / "hip.png", rows);
    }
    if (wanted("leg")) {  // the legs: straight and one knee bent 120°, all round, with the traced lines of the side; the feet up close
        std::vector<std::vector<Cell>> rows;
        for (const char* sex : {"male", "female"}) {
            std::vector<Cell> row;
            for (const double turn : {0.0, pi / 4, pi / 2, pi, -pi / 2}) {
                row.push_back(Cell{{posed(R"({"l_leg": {"x": 1.0}, "l_knee": {"x": -2.1}})", sex, turn, 200.0)}, -50.0, -10.0, 100.0, 135.0, 60.0, false});
            }
            row.push_back(Cell{{posed(R"({"l_leg": {"x": 1.0}, "l_knee": {"x": -2.1}})", sex, pi / 2, 200.0)}, -50.0, -10.0, 100.0, 135.0, 60.0, true});
            rows.push_back(row);
        }
        write_sheet(dir / "leg.png", rows);
        std::vector<std::vector<Cell>> feet;
        for (const char* sex : {"male", "female"}) {
            std::vector<Cell> row;
            for (const double turn : {0.0, pi / 4, pi / 2, pi, -pi / 2}) row.push_back(around(posed("{}", sex, turn, 200.0), "l_ankle", 60.0, 40.0, 150.0, false, 0.0, -2.0));
            row.push_back(around(posed("{}", sex, 0.0, 200.0, 1.2), "l_ankle", 60.0, 40.0, 150.0));   // (from above)
            row.push_back(around(posed("{}", sex, 0.0, 200.0, -1.2), "l_ankle", 60.0, 40.0, 150.0));  // (the sole)
            row.push_back(around(posed(R"({"l_ankle": {"x": -0.9}, "l_leg": {"x": 0.4}})", sex, pi / 2, 200.0), "l_ankle", 60.0, 40.0, 150.0));
            feet.push_back(row);
        }
        write_sheet(dir / "foot.png", feet);
    }
    if (wanted("arm")) {  // the arm: straight and bent, the wrist and the hand on the arm, all round
        std::vector<std::vector<Cell>> rows;
        for (const char* sex : {"male", "female"}) {
            for (const char* pose : {R"({"l_arm": {"z": 1.45}, "r_arm": {"z": -1.45}, "r_elbow": {"x": 1.9}})",
                                     R"({"l_arm": {"x": 0.5, "z": 0.2}, "l_elbow": {"x": 1.6}, "r_arm": {"z": -0.5}, "l_wrist": {"z": -0.8}, "r_wrist": {"x": 0.5}})"}) {
                std::vector<Cell> row;
                for (const double turn : {0.0, pi / 2, pi, -pi / 2}) {
                    for (const bool lines : {false, true}) {
                        row.push_back(Cell{{posed(pose, sex, turn, 200.0, 0.0, R"({"l": "open", "r": "relaxed"})")}, -95.0, -85.0, 190.0, 100.0, 40.0, lines});
                    }
                }
                rows.push_back(row);
            }
        }
        write_sheet(dir / "arm.png", rows);
    }
    if (wanted("head")) {  // the head larger: front, 3/4, profile, back, from above and below, shaded and as lines; then on the body
        std::vector<std::vector<Cell>> rows;
        for (const bool lines : {false, true}) {
            std::vector<Cell> row;
            for (const double turn : {0.0, pi / 4, pi / 2, 3 * pi / 4, pi}) {
                Json head = Json::object({{"kind", "head"}, {"id", "h"}, {"pos", Json::array({0.0, 0.0, 0.0})}, {"size", Json::array({40.0, 40.0, 40.0})},
                                          {"rot", Json::array({0.0, turn, 0.0})}});
                row.push_back(Cell{{head}, -26.0, -26.0, 52.0, 52.0, 150.0, lines});
            }
            for (const double tip : {0.6, -0.6}) {
                Json head = Json::object({{"kind", "head"}, {"id", "h"}, {"pos", Json::array({0.0, 0.0, 0.0})}, {"size", Json::array({40.0, 40.0, 40.0})},
                                          {"rot", Json::array({tip, 0.5, 0.0})}});
                row.push_back(Cell{{head}, -26.0, -26.0, 52.0, 52.0, 150.0, lines});
            }
            rows.push_back(row);
        }
        for (const char* sex : kSexes) {
            std::vector<Cell> row;
            for (const double turn : {0.0, pi / 4, pi / 2, pi}) {
                for (const bool lines : {false, true}) {
                    if (lines && turn != 0.0 && turn != pi / 2) continue;
                    row.push_back(Cell{{figure_prim("stand", sex, turn, 240.0)}, -32.0, -150.0, 64.0, 80.0, 120.0, lines});
                }
            }
            rows.push_back(row);
        }
        write_sheet(dir / "head.png", rows);
    }
    if (wanted("fists")) {  // the hands on the arm, up close: the six poses, from the back of the hand, the thumb's side, the palm and the little finger's
        std::vector<std::vector<Cell>> rows;
        for (const char* hands : {R"({"l": "open"})", R"({"l": "relaxed"})", R"({"l": "fist"})", R"({"l": "point"})", R"({"l": "peace"})", R"({"l": "grip"})"}) {
            std::vector<Cell> row;
            for (const double turn : {-pi / 2, 0.0, pi / 2, pi}) {
                for (const bool lines : {false, true}) {
                    row.push_back(around(posed(R"({"l_arm": {"z": 0.5}})", "male", turn, 200.0, 0.0, hands), "l_wrist", 44.0, 50.0, 130.0, lines, 0.0, 10.0));
                }
            }
            rows.push_back(row);
        }
        write_sheet(dir / "fists.png", rows);
    }
}

// One look asked for by hand (GENKO_FIGURE_VIEW, JSON): {"joints": {…}, "hands": {…}, "body": {…}, "height": 200,
// "turns": [0, 0.8], "tip": 0, "box": [x, y, w, h] (mm about the pelvis), "dpi": 100, "lines": false} → view.png.
inline void write_view(const std::filesystem::path& dir, const std::string& text) {
    const Json spec = core::parse_python_json(text);
    const double height = spec.value("height", 200.0);
    const Json box = spec.value("box", Json::array({-60.0, -120.0, 120.0, 240.0}));
    std::vector<std::vector<Cell>> rows;
    for (const bool lines : {false, true}) {
        if (lines && !spec.value("lines", false)) continue;
        std::vector<Cell> row;
        for (const Json& turn : spec.value("turns", Json::array({0.0}))) {
            Json prim = figure_prim("stand", "", turn.get<double>(), height);
            prim["joints"] = spec.value("joints", Json::object());
            prim["hands"] = spec.value("hands", Json::object());
            prim["body"] = spec.value("body", Json::object());
            prim["rot"] = Json::array({spec.value("tip", 0.0), turn.get<double>(), 0.0});
            row.push_back(Cell{{prim}, box[0].get<double>(), box[1].get<double>(), box[2].get<double>(), box[3].get<double>(), spec.value("dpi", 100.0), lines});
        }
        rows.push_back(row);
    }
    write_sheet(dir / "view.png", rows);
}

inline void write_all(const std::filesystem::path& dir, const std::string& only = std::string(), const std::string& view = std::string()) {
    std::filesystem::create_directories(dir);
    if (!view.empty()) {
        write_view(dir, view);
        return;
    }
    write_anatomy(dir, only);
    if (!only.empty() && ("," + only + ",").find(",all,") == std::string::npos) return;
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
