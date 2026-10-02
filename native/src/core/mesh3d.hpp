#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/error.hpp"
#include "core/json.hpp"
#include "core/linalg3.hpp"

// 3D with surfaces (Python's genko/mesh3d.py, and the geometry of genko/threeops.py): meshes for the posable figure
// (体型と関節), a head for faces from any angle, hands with finger poses, imported models (OBJ, glTF/GLB/VRM), the
// surfaces of boxes and scenes; seen through the page's camera, lit by one light, drawn as shaded surfaces (面) and as
// the lines a pen would draw (seen edges and outlines, the hidden ones left out).
//
// Page space is mm: x to the right, y down the page, z away from the viewer. A prim is a JSON object as the book keeps
// it ({"kind", "pos", "size", "rot", …}); every number comes out as the Python baseline computes it, to the last bit
// (core/linalg3.hpp: numpy's rounding). A camera is the page's {turn, tip, roll, focal_mm, target}, passed on its own
// (null: each prim is seen about its own centre); a value that is not truthy counts as none.

namespace genko::core::mesh3d {

using la::Mat3;
using la::Vec3;
using Point2 = std::array<double, 2>;
using Line2 = std::vector<Point2>;

inline constexpr std::int64_t kMaxFaces = 30000;  // mesh3d.MAX_FACES
inline constexpr int kSmooth = 1000;               // added to a face's part when it belongs to a rounded piece
// (the most vertices and face corners a model may bring, the C++ build's alone: core/limits.hpp)

extern const std::array<std::string_view, 17> kFigureJoints;  // FIGURE_JOINTS
extern const std::array<std::string_view, 6> kHandPoses;      // HAND_POSES
extern const std::array<std::string_view, 5> kFingers;        // FINGERS

// MESH_KINDS: figure, head, hand, mesh.
bool is_mesh_kind(const Json& kind);

// FIGURE_PRESETS ({name: {joint: {axis: radians}, "hands": {side: pose}}}) and SEX_BODY, in Python's order.
const Json& figure_presets();
const Json& sex_body();

// 指ごとの曲がり (thumb, index, middle, ring, little: 0 straight .. 1 closed) from a pose name, a list of five, or
// {"pose", "curls": [five] | {finger: curl}}.
std::array<double, 5> hand_curls(const Json& value);

Mat3 rx(double t);  // swings +y toward −z (the viewer) for t > 0
Mat3 ry(double t);
Mat3 rz(double t);  // counter-clockwise on the page (y points down)
// prim3d's turn as one matrix: about the upright axis, then tip, then lean. `rot` is the value (null: [0, 0, 0]).
Mat3 prim_rotation(const Json& rot);
Mat3 joint_matrix(const Json& joint);

// The figure's joints in its own space (mm, before its turn) and each bone's turn, in Python's order.
struct Skeleton {
    std::vector<std::pair<std::string, Vec3>> points;
    std::vector<std::pair<std::string, Mat3>> turns;
    double unit = 0.0;
    Json body;

    const Vec3& point(std::string_view name) const;
    const Mat3& turn(std::string_view name) const;
};
Skeleton figure_skeleton(const Json& prim);

struct Mesh {
    std::vector<Vec3> v;
    std::vector<std::vector<std::int64_t>> f;
    std::vector<int> parts;  // which part each face belongs to (for the lines); + kSmooth for rounded pieces
};

// A prim's mesh in its own space (before its turn and place) and the guide lines drawn on it (a face's centre line,
// eye line and nose). Figures are remembered while the prim is unchanged (Python's lru_cache).
struct Model {
    Mesh mesh;
    std::vector<std::vector<Vec3>> marks;
};
std::shared_ptr<const Model> model_of(const Json& prim);

// Points of a prim's own space on the page (mm) and their depth (larger = further).
struct OnPage {
    std::vector<Point2> pts;
    std::vector<double> depth;
};
OnPage to_page(const Json& prim, std::span<const Vec3> local, const Json* camera);

// The prim's mesh on the page: points, depth, faces, parts and the guide lines on the page.
struct Seen {
    OnPage at;
    std::shared_ptr<const Model> model;
    std::vector<OnPage> guides;
};
Seen seen(const Json& prim, const Json* camera);

// The camera a prim is seen with: the page's (page.extra["camera"]) when it is truthy, else the prim's own "camera".
const Json* camera_of(const Json& prim, const Json* page_camera);

// Each face's unit normal as the prim is seen (its own turn, then the camera's): _normals(_turned(…)).
std::vector<Vec3> face_normals(const Json& prim, const Mesh& mesh, const Json* camera);

// Shaded surfaces (1 = lit white; float32 as numpy has them) and the depth buffer (inf where there is no surface),
// for the prims with meshes, drawn at dpi over a picture of width × height px whose top-left is (box_x, box_y) mm.
// `light` is the light's direction (null: the usual one). With a window only that part of the picture is made (its
// pixels are the same as in the whole picture's); x0, y0 say where it starts.
struct Window {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;  // (outside)
    int y1 = 0;
};

struct Raster {
    int x0 = 0;
    int y0 = 0;
    int width = 0;
    int height = 0;
    std::vector<float> zbuf;
    std::vector<float> shade;

    bool alpha(std::size_t i) const;
    bool any() const;
};
// A picture of surfaces of more than limits::kSurfacePixels is core::Error("image_too_large"), where numpy would run out
// of memory.
Raster raster(std::span<const Json> prims, int width, int height, double dpi, const Json* camera, const Json* light,
              double ambient, double box_x = 0.0, double box_y = 0.0, const Window* window = nullptr);

// The lines a pen would draw (page mm): outlines and creases of the meshes, the face guides, hidden parts left out.
std::vector<Line2> lines(std::span<const Json> prims, const Json* camera, double dpi = 40.0, double crease_deg = 40.0);
// A mesh prim's pen lines (remembered while the prim and its camera are unchanged).
std::shared_ptr<const std::vector<Line2>> prim_lines(const Json& prim, const Json* camera, double dpi = 40.0);

// The pen lines of the prims again, without what is remembered (the tests use this).
void clear_caches();

// --- OBJ and glTF ---------------------------------------------------------------------------------------------------

// A model file that cannot be used (Python's ObjError): the op reports its message as it is.
class ObjError : public Error {
public:
    explicit ObjError(const std::string& message);
};

// A mesh from OBJ text ("v" and "f" lines), fitted into a unit box centred on 0 with y down:
// {"v": [x, y, z, …] (rounded to 5 places), "f": [[i, j, k, …], …], "ratio": [w, h, d]}.
Json read_obj(std::string_view text);
// A mesh from a .glb / .vrm (binary glTF) or a .gltf with its buffers inside (data: URIs), the nodes' places applied.
Json read_gltf(std::string_view data);

// --- the figure's handles (threeops) -------------------------------------------------------------------------------

// The joint change that points the dragged bone at `target` (page mm): {"pos": …} for the pelvis, else
// {"joints": {joint: {axis: radians}}}. core::OpError for an unknown handle.
Json drag_joint(const Json& prim, std::string_view handle, Point2 target, const Json* camera);
// IK: the hand (or foot) pulled to `target` and the limb following ({"joints": {upper: {x, z}, lower: {x}}}).
Json reach(const Json& prim, std::string_view handle, Point2 target, const Json* camera);
// The points people drag, on the page: pelvis, then threeops.HANDLES in order.
std::vector<std::pair<std::string, Point2>> figure_handles(const Json& prim, const Json* camera);
// The page position of one of the skeleton's points (threeops._page_of_joint).
Point2 page_of_joint(const Json& prim, std::string_view name, const Json* camera);

}  // namespace genko::core::mesh3d
