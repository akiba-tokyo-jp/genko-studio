#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

// PC3 C-c (SPEC.md COLOR-07, edit.puppetWarp): Edit › Puppet Warp — pins on a triangle mesh over what a layer shows,
// deformed as rigidly as possible. PhotoCraft v0.2.0's algo/src/puppet.rs (MIT, native/third_party/photocraft/NOTICE.md):
//
// * The mesh: what is covered (an alpha above 0.05, grown or shrunk by the expansion) on a coarse block grid; a square
//   grid at the density's spacing keeps every cell touching covered blocks, split into two triangles (alternating
//   diagonals).
// * The deformation: the per-triangle ARAP energy (Liu et al. 2008), Σ_t Σ_(i,j)∈t w_ij |(v_i − v_j) − R_t (x_i − x_j)|²
//   with cotangent weights, minimised by turns of a local step (each triangle's best rotation from its 2×2 covariance —
//   rigid; × √scale normal; × scale distort) and a global step (one sparse solve per axis: a banded Cholesky factor, or
//   Jacobi-preconditioned conjugate gradients). Pins are stiff soft constraints on the barycentric point under them; a
//   pin's rotation fixes R on the triangles around it; a part of the mesh without a pin stays where it is. The first
//   global step is harmonic (R = I), or with turned pins each triangle turned by their inverse-distance weighted angle.
// * The drawing order: back to front by the depth of each triangle's nearest pin.
//
// Coordinates are any one unit the caller chooses (the transform op uses the page's working 200 dpi pixels): the
// coverage's cells are one unit square each.

namespace genko::render::puppet {

enum class Mode { Rigid, Normal, Distort };
enum class Density { Fewer, Normal, More };
std::optional<Mode> mode_from(std::string_view text);        // (rigid, normal, distort)
std::optional<Density> density_from(std::string_view text);  // (fewer, normal, more)
std::string_view to_string(Mode mode);
std::string_view to_string(Density density);

using XY = std::array<double, 2>;

// A pin: where it was put (src) and where it is now (dst); a fixed turn of the mesh about it (degrees, none:
// automatic); its depth (where the mesh folds over itself, the triangles nearest a higher pin are drawn on top).
struct Pin {
    XY src{0, 0};
    XY dst{0, 0};
    std::optional<double> rotate;
    int depth = 0;
};

struct Warp {
    std::vector<Pin> pins;
    Mode mode = Mode::Normal;
    Density density = Density::Normal;
    double expansion = 2.0;  // (units: the mesh grown past what is covered; below 0 shrunk; −200..200)
    // No pin moved or turned.
    bool is_identity() const;
};

// A triangle mesh over a regular grid: vertex (i, j) of the grid is at (ox + i·spacing, oy + j·spacing).
struct Mesh {
    std::vector<XY> verts;
    std::vector<std::array<std::size_t, 3>> tris;
    double spacing = 0;
    double ox = 0, oy = 0;
    std::size_t cols = 0, rows = 0;               // (the grid's cells)
    std::vector<std::array<std::size_t, 2>> cell;  // (each cell's two triangles, or none: SIZE_MAX)
    bool empty() const { return tris.empty(); }
    // The triangle under p and p's barycentric weights on its corners; none outside the mesh.
    std::optional<std::pair<std::size_t, std::array<double, 3>>> locate(double x, double y) const;
};

// The coverage: `alpha` (w × h values, rows from the top) over the unit squares from (x0, y0).
struct Coverage {
    std::vector<float> alpha;
    int width = 0, height = 0;
    int x0 = 0, y0 = 0;
};
Mesh build_mesh(const Coverage& coverage, Density density, double expansion);

// The mesh deformed by the pins: its vertices where they went and the triangles' drawing order (back to front).
struct Deformed {
    Mesh mesh;
    std::vector<XY> verts;
    std::vector<std::size_t> order;
    // Where a point of the source goes: through its triangle's barycentric weights; off the mesh, moved as the nearest
    // vertex moved.
    XY map(double x, double y) const;
};

// The number of local and global rounds the op runs.
inline constexpr int kIterations = 40;

// The ARAP solver prepared for one mesh and its pins' places (the triangles a turned pin fixes: `fixed_rotation`).
class Solver {
public:
    Solver(Mesh mesh, const std::vector<XY>& pin_src, const std::vector<bool>& fixed_rotation);
    ~Solver();
    Solver(const Solver&) = delete;
    Solver& operator=(const Solver&) = delete;
    const Mesh& mesh() const;
    // The mesh deformed: the pins' destinations, their fixed turns, `iterations` rounds; and the rigid ARAP energy after
    // each round.
    std::pair<std::vector<XY>, std::vector<double>> solve(const std::vector<XY>& pin_dst, const std::vector<std::optional<double>>& rotations,
                                                          Mode mode, int iterations) const;
    // The energy of `v` with the rigid best fit.
    double energy(const std::vector<XY>& v, const std::vector<XY>& pin_dst, const std::vector<std::optional<double>>& rotations) const;
    // The pins' points on the deformed mesh (their barycentric places).
    std::vector<XY> pinned(const std::vector<XY>& v) const;
    // The triangles back to front by the depth of their nearest pin.
    std::vector<std::size_t> draw_order(const std::vector<int>& depths, const std::vector<XY>& pin_src) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The whole warp: the mesh over the coverage, solved with the pins (kIterations rounds), and its drawing order.
Deformed deform(const Coverage& coverage, const Warp& warp, int iterations = kIterations);

}  // namespace genko::render::puppet
