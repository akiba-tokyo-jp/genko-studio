// The 3D ops against the Python baseline's apply_ops: fixed batches for each op (8 or more that succeed, 4 or more that
// fail, page locks and strict_gates among them) on the books of geom3d_harness.py fixtures, compared for
//  - the reply: ok, applied, the snapshot, job_id, warnings — or the same error text;
//  - the full snapshot and the 3D state after it: every page's prims, rulers and extra keys (camera, light) as JSON, each
//    layer's strokes (the bytes of its .strokes.json), its pixels and its screen;
//  - the book as saved: project.json as Python's writer writes it (the strokes blobs by their sha256, so byte for byte;
//    a picture made by render_prims by its pixels, PNG files being encoded differently).
// The cases where the C++ build refuses what Python takes in (and would keep a broken book) are in safeSideRefusals.
// Skipped without the Python reference.

#include <QtTest>

#include <QDir>
#include <QTemporaryDir>

#include <limits>
#include <map>
#include <string>
#include <vector>

#include "core/command_bus.hpp"
#include "core/ids.hpp"
#include "core/strokes.hpp"
#include "render/ops_registry.hpp"
#include "render/png.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"
#include "storage/reader.hpp"
#include "storage/snapshot.hpp"
#include "storage/writer.hpp"
#include "test3d.hpp"

namespace fs = std::filesystem;
using genko::core::Json;

namespace {

struct Case {
    const char* op;    // the op under test
    const char* book;  // a, strict or locked
    const char* agent;
    bool dry_run;
    const char* ops;
};

// $F1, $F2, $F3: page 1's panels in reading order; $NAME: a model file of geom3d_harness.py fixture_models (as a JSON string)
const Case kCases[] = {
    // add_figure
    {"add_figure", "a", "genko", false, R"([{"op": "add_figure", "page": 1, "id": "f1"}])"},
    {"add_figure", "a", "genko", false, R"([{"op": "add_figure", "page": 3, "id": "f2", "pos": [50, 60], "height_mm": "150", "rot": [0.1, 0.2], "focal_mm": 200}])"},
    {"add_figure", "a", "genko", false, R"([{"op": "add_figure", "page": 3, "id": "f3", "preset": "think", "joints": {"l_knee": {"x": -0.5}}, "hands": {"l": "fist", "r": [0, 0.5, 1, 0.2, 0.1]}}])"},
    {"add_figure", "a", "genko", false, R"([{"op": "add_figure", "page": 3, "id": "f4", "body": {"sex": "female", "heads": 6, "legs": 1.2}, "height_mm": 200}])"},
    {"add_figure", "a", "human:作者", false, R"([{"op": "add_figure", "page": 1, "id": "f5", "frame_id": "$F2", "pos": [40, 200, 0]}])"},
    {"add_figure", "a", "genko", false, R"([{"op": "add_figure", "page": 2, "pos": [120, 100, 0]}])"},
    {"add_figure", "a", "genko", false, R"([{"op": "add_figure", "page": 3, "id": "f7", "hands": {"r": {"pose": "open", "curls": {"index": 1, "thumb": 0.5}}}}])"},
    {"add_figure", "a", "genko", false, R"([{"op": "add_figure", "page": 3, "id": "f8", "focal_mm": 5, "preset": "sit"}])"},
    {"add_figure", "a", "genko", false, R"([{"op": "add_figure", "page": 3, "id": "f9", "body": {"sex": null, "build": 1.8}, "colour": "red"}])"},
    {"add_figure", "a", "genko", true, R"([{"op": "add_figure", "page": 3, "id": "f10", "preset": "kneel"}])"},
    {"add_figure", "a", "genko", false, R"([{"op": "add_figure", "page": 1, "height_mm": 9}])"},
    {"add_figure", "a", "genko", false, R"([{"op": "add_figure", "page": 1, "id": "fig"}])"},
    {"add_figure", "a", "genko", false, R"([{"op": "add_figure", "page": 1, "id": "x", "body": {"heads": 20}}])"},
    {"add_figure", "a", "genko", false, R"([{"op": "add_figure", "page": 1, "id": "x", "joints": {"tail": {"x": 1}}}])"},
    {"add_figure", "a", "genko", false, R"([{"op": "add_figure", "page": 1, "id": "x", "hands": {"r": "wave"}}])"},
    {"add_figure", "a", "genko", false, R"([{"op": "add_figure", "page": 1, "id": "x", "preset": "dance"}])"},
    {"add_figure", "a", "genko", false, R"([{"op": "add_figure", "page": 1, "id": "x", "pos": "ab"}])"},
    {"add_figure", "a", "genko", false, R"([{"op": "add_figure", "page": 9}])"},
    {"add_figure", "a", "genko", false, R"([{"op": "add_figure", "page": 1, "id": "x", "frame_id": "nope"}])"},
    {"add_figure", "a", "genko", false, R"([{"op": "add_figure", "page": 1, "id": "x", "body": [1]}])"},
    {"add_figure", "a", "genko", false, R"([{"op": "add_figure", "page": 1, "id": "x", "body": {"sex": "robot"}}])"},
    {"add_figure", "a", "genko", false, R"([{"op": "add_figure", "page": 1, "id": "x", "joints": {"l_arm": {"w": 1}}}])"},
    {"add_figure", "a", "genko", false, R"([{"op": "add_figure", "page": 1, "id": "x", "hands": {"x": "fist"}}])"},
    {"add_figure", "a", "genko", false, R"([{"op": "add_figure", "page": 1, "id": "x", "hands": {"r": {"pose": "open", "curls": {"sixth": 1}}}}])"},
    {"add_figure", "locked", "ai:hermes", false, R"([{"op": "add_figure", "page": 1, "id": "x"}])"},
    // pose_figure
    {"pose_figure", "a", "genko", false, R"([{"op": "pose_figure", "page": 1, "id": "fig", "joints": {"r_arm": {"z": -1.5}}}])"},
    {"pose_figure", "a", "genko", false, R"([{"op": "pose_figure", "page": 1, "id": "fig", "preset": "peace"}])"},
    {"pose_figure", "a", "genko", false, R"([{"op": "pose_figure", "page": 1, "id": "fig", "set_joints": {"head": {"y": 0.4}}}])"},
    {"pose_figure", "a", "genko", false, R"([{"op": "pose_figure", "page": 1, "id": "fig", "body": {"sex": "male", "hips": 1.3}}])"},
    {"pose_figure", "a", "genko", false, R"([{"op": "pose_figure", "page": 1, "id": "fig", "hands": {"l": {"pose": "open", "curls": {"index": 1.0}}}}])"},
    {"pose_figure", "a", "genko", false, R"([{"op": "pose_figure", "page": 1, "id": "fig", "rot": [0, 0.5, 0], "pos": [70, 130], "height_mm": 110}])"},
    {"pose_figure", "a", "genko", false, R"([{"op": "pose_figure", "page": 1, "id": "fig", "drag": {"handle": "r_elbow", "to": [30, 60]}}])"},
    {"pose_figure", "a", "genko", false, R"([{"op": "pose_figure", "page": 1, "id": "fig", "drag": {"handle": "pelvis", "to": [60, 150]}}])"},
    {"pose_figure", "a", "genko", false, R"([{"op": "pose_figure", "page": 1, "id": "fig", "drag": {"handle": "l_wrist", "to": [80, 90], "ik": true}}])"},
    {"pose_figure", "a", "genko", false, R"([{"op": "pose_figure", "page": 2, "id": "fig2", "drag": {"handle": "r_ankle", "to": [95, 240], "ik": true}}])"},
    {"pose_figure", "a", "genko", false, R"([{"op": "pose_figure", "page": 2, "id": "fig2", "drag": {"handle": "head", "to": [100, 90]}}])"},
    {"pose_figure", "a", "genko", false, R"([{"op": "pose_figure", "page": 1, "id": "hand", "pose": "fist"}])"},
    {"pose_figure", "a", "genko", false, R"([{"op": "pose_figure", "page": 1, "id": "hand", "curls": [0, 0, 1, 1, 1]}])"},
    {"pose_figure", "a", "genko", false, R"([{"op": "pose_figure", "page": 1, "id": "hand", "curls": {"thumb": 1}, "rot": [0.3, 0, 0.2]}])"},
    {"pose_figure", "a", "genko", false, R"([{"op": "pose_figure", "page": 1, "id": "box"}])"},
    {"pose_figure", "a", "genko", false, R"([{"op": "pose_figure", "page": 1, "id": "nothing"}])"},
    {"pose_figure", "a", "genko", false, R"([{"op": "pose_figure", "page": 1, "id": "fig", "body": {"heads": 20}}])"},
    {"pose_figure", "a", "genko", false, R"([{"op": "pose_figure", "page": 1, "id": "fig", "hands": {"r": "wave"}}])"},
    {"pose_figure", "a", "genko", false, R"([{"op": "pose_figure", "page": 1, "id": "fig", "drag": {"handle": "tail", "to": [1, 2]}}])"},
    {"pose_figure", "a", "genko", false, R"([{"op": "pose_figure", "page": 1, "id": "fig", "drag": {"handle": "head", "to": [1, 2], "ik": true}}])"},
    {"pose_figure", "a", "genko", false, R"([{"op": "pose_figure", "page": 1, "id": "hand", "pose": "wave"}])"},
    {"pose_figure", "a", "genko", false, R"([{"op": "pose_figure", "page": 1, "id": "hand", "curls": [1, 2, 3]}])"},
    {"pose_figure", "a", "genko", false, R"([{"op": "pose_figure", "page": 1, "id": "fig", "drag": {"handle": "l_knee", "to": "x"}}])"},
    {"pose_figure", "locked", "ai:hermes", false, R"([{"op": "pose_figure", "page": 1, "id": "fig", "preset": "run"}])"},
    // add_head
    {"add_head", "a", "genko", false, R"([{"op": "add_head", "page": 3}])"},
    {"add_head", "a", "genko", false, R"([{"op": "add_head", "page": 3, "id": "h1", "pos": [60, 60, 0], "size_mm": 30, "rot": [0.2, 0.6, 0]}])"},
    {"add_head", "a", "genko", false, R"([{"op": "add_head", "page": 1, "id": "h2", "pos": [40, 40], "frame_id": "$F1"}])"},
    {"add_head", "a", "genko", false, R"([{"op": "add_head", "page": 3, "id": "h3", "size_mm": "45"}])"},
    {"add_head", "a", "genko", false, R"([{"op": "add_head", "page": 3, "id": "h4", "rot": [1, 2]}])"},
    {"add_head", "a", "genko", false, R"([{"op": "add_head", "page": 1, "id": "head"}])"},
    {"add_head", "a", "genko", true, R"([{"op": "add_head", "page": 2, "id": "h6"}])"},
    {"add_head", "a", "genko", false, R"([{"op": "add_head", "page": 3, "id": "h7"}, {"op": "add_head", "page": 3, "id": "h8", "size_mm": 12}])"},
    {"add_head", "a", "genko", false, R"([{"op": "add_head", "page": 9}])"},
    {"add_head", "a", "genko", false, R"([{"op": "add_head", "page": 3, "pos": "x"}])"},
    {"add_head", "a", "genko", false, R"([{"op": "add_head", "page": 3, "rot": [1]}])"},
    {"add_head", "a", "genko", false, R"([{"op": "add_head", "page": 1, "frame_id": "nope"}])"},
    {"add_head", "a", "genko", false, R"([{"op": "add_head", "page": 3, "size_mm": "abc"}])"},
    {"add_head", "locked", "ai:hermes", false, R"([{"op": "add_head", "page": 1}])"},
    // add_hand
    {"add_hand", "a", "genko", false, R"([{"op": "add_hand", "page": 3}])"},
    {"add_hand", "a", "genko", false, R"([{"op": "add_hand", "page": 3, "id": "k1", "pos": [120, 60, 0], "size_mm": 30, "pose": "peace", "side": "l"}])"},
    {"add_hand", "a", "genko", false, R"([{"op": "add_hand", "page": 3, "id": "k2", "pose": "grip", "rot": [0.5, 1, 0]}])"},
    {"add_hand", "a", "genko", false, R"([{"op": "add_hand", "page": 1, "id": "k3", "frame_id": "$F3", "pos": [120, 200, 0]}])"},
    {"add_hand", "a", "genko", false, R"([{"op": "add_hand", "page": 3, "id": "k4", "side": "r", "pose": "point", "size_mm": "18"}])"},
    {"add_hand", "a", "genko", false, R"([{"op": "add_hand", "page": 3, "id": "k5", "side": "", "pose": ""}])"},
    {"add_hand", "a", "genko", true, R"([{"op": "add_hand", "page": 3, "id": "k6"}])"},
    {"add_hand", "a", "genko", false, R"([{"op": "add_hand", "page": 2, "id": "k7", "pose": "open"}])"},
    {"add_hand", "a", "genko", false, R"([{"op": "add_hand", "page": 3, "side": "x"}])"},
    {"add_hand", "a", "genko", false, R"([{"op": "add_hand", "page": 3, "pose": "wave"}])"},
    {"add_hand", "a", "genko", false, R"([{"op": "add_hand", "page": 3, "pos": [1]}])"},
    {"add_hand", "a", "genko", false, R"([{"op": "add_hand", "page": 9}])"},
    {"add_hand", "a", "genko", false, R"([{"op": "add_hand", "page": 3, "size_mm": {"a": 1}}])"},
    // import_model
    {"import_model", "a", "genko", false, R"([{"op": "import_model", "page": 3, "id": "m1", "obj": $CUBE}])"},
    {"import_model", "a", "genko", false, R"([{"op": "import_model", "page": 3, "id": "m2", "obj": $PYRAMID, "size_mm": 45}])"},
    {"import_model", "a", "genko", false, R"([{"op": "import_model", "page": 3, "id": "m3", "glb": $GLB}])"},
    {"import_model", "a", "genko", false, R"([{"op": "import_model", "page": 3, "id": "m4", "gltf": $GLTF, "name": "四面体"}])"},
    {"import_model", "a", "genko", false, R"([{"op": "import_model", "page": 1, "id": "m5", "obj": $CUBE, "size_mm": 25, "pos": [40, 40, 10], "rot": [0, 0, 0], "frame_id": "$F1"}])"},
    {"import_model", "a", "genko", false, R"([{"op": "import_model", "page": 3, "id": "m6", "obj": $NEGATIVE}])"},
    {"import_model", "a", "genko", false, R"([{"op": "import_model", "page": 3, "obj": $CUBE}])"},
    {"import_model", "a", "genko", false, R"([{"op": "import_model", "page": 2, "id": "m8", "glb": $GLB_NODES, "size_mm": 50}])"},
    {"import_model", "a", "genko", false, R"([{"op": "import_model", "page": 3, "id": "x"}])"},
    {"import_model", "a", "genko", false, R"([{"op": "import_model", "page": 3, "obj": "  \n "}])"},
    {"import_model", "a", "genko", false, R"([{"op": "import_model", "page": 3, "obj": "v 0 0 0\nv 1 0 0"}])"},
    {"import_model", "a", "genko", false, R"([{"op": "import_model", "page": 3, "obj": "v 0 0 0\nf 1 2 3"}])"},
    {"import_model", "a", "genko", false, R"([{"op": "import_model", "page": 3, "glb": $GLB_SHORT}])"},
    {"import_model", "a", "genko", false, R"([{"op": "import_model", "page": 3, "gltf": $GLTF_EXTERNAL}])"},
    {"import_model", "a", "genko", false, R"([{"op": "import_model", "page": 3, "glb": "!!!"}])"},
    {"import_model", "a", "genko", false, R"([{"op": "import_model", "page": 3, "obj": "v a b c\nf 1 2 3"}])"},
    {"import_model", "a", "genko", false, R"([{"op": "import_model", "page": 3, "obj": $MANY_FACES}])"},
    {"import_model", "a", "genko", false, R"([{"op": "import_model", "page": 3, "obj": "v 0 0 0\nv 1 1 1\nv 2 0 0\nf 1 2 x"}])"},
    {"import_model", "a", "genko", false, R"([{"op": "import_model", "page": 3, "obj": 5}])"},
    {"import_model", "a", "genko", false, R"([{"op": "import_model", "page": 3, "obj": $CUBE, "size_mm": "big"}])"},
    // set_camera
    {"set_camera", "a", "genko", false, R"([{"op": "set_camera", "page": 1, "turn": 0.8, "tip": 0.3, "focal_mm": 250}])"},
    {"set_camera", "a", "genko", false, R"([{"op": "set_camera", "page": 2, "off": true}])"},
    {"set_camera", "a", "genko", false, R"([{"op": "set_camera", "page": 2, "roll": 0.12345678}])"},
    {"set_camera", "a", "genko", false, R"([{"op": "set_camera", "page": 3, "target": [10, 20]}])"},
    {"set_camera", "a", "genko", false, R"([{"op": "set_camera", "page": 2, "target": null}])"},
    {"set_camera", "a", "genko", false, R"([{"op": "set_camera", "page": 3, "target": [1, 2, 3], "tip": -0.25}])"},
    {"set_camera", "a", "genko", false, R"([{"op": "set_camera", "page": 3, "turn": "0.5", "focal_mm": "20"}])"},
    {"set_camera", "a", "genko", false, R"([{"op": "set_camera", "page": 3}])"},
    {"set_camera", "a", "genko", false, R"([{"op": "set_camera", "page": 1, "off": 1}])"},
    {"set_camera", "a", "genko", false, R"([{"op": "set_camera", "page": 1, "focal_mm": 10}])"},
    {"set_camera", "a", "genko", false, R"([{"op": "set_camera", "page": 1, "focal_mm": 6000}])"},
    {"set_camera", "a", "genko", false, R"([{"op": "set_camera", "page": 1, "target": [5]}])"},
    {"set_camera", "a", "genko", false, R"([{"op": "set_camera", "page": 1, "turn": "x"}])"},
    {"set_camera", "a", "genko", false, R"([{"op": "set_camera", "page": 9}])"},
    {"set_camera", "locked", "ai:hermes", false, R"([{"op": "set_camera", "page": 1, "turn": 1}])"},
    // set_light
    {"set_light", "a", "genko", false, R"([{"op": "set_light", "page": 1, "dir": [1, 0, -0.2], "ambient": 0.2}])"},
    {"set_light", "a", "genko", false, R"([{"op": "set_light", "page": 1, "dir": [1, 1]}])"},
    {"set_light", "a", "genko", false, R"([{"op": "set_light", "page": 1, "ambient": 1.5}])"},
    {"set_light", "a", "genko", false, R"([{"op": "set_light", "page": 1, "ambient": -1}])"},
    {"set_light", "a", "genko", false, R"([{"op": "set_light", "page": 3}])"},
    {"set_light", "a", "genko", false, R"([{"op": "set_light", "page": 2, "ambient": 0.7}])"},
    {"set_light", "a", "genko", false, R"([{"op": "set_light", "page": 3, "dir": ["1", "0.5", "-1"]}])"},
    {"set_light", "a", "genko", false, R"([{"op": "set_light", "page": 3, "ambient": "0.4", "dir": [0, 0, 1, 7]}])"},
    {"set_light", "a", "genko", false, R"([{"op": "set_light", "page": 1, "dir": [0, 0, 0]}])"},
    {"set_light", "a", "genko", false, R"([{"op": "set_light", "page": 1, "dir": [1]}])"},
    {"set_light", "a", "genko", false, R"([{"op": "set_light", "page": 1, "ambient": "x"}])"},
    {"set_light", "a", "genko", false, R"([{"op": "set_light", "page": 9}])"},
    {"set_light", "a", "genko", false, R"([{"op": "set_light", "page": 1, "dir": "x"}])"},
    // render_prims
    {"render_prims", "a", "genko", false, R"([{"op": "render_prims", "page": 1, "layer_id": "paint", "ids": ["fig"]}])"},
    {"render_prims", "a", "genko", false, R"([{"op": "render_prims", "page": 1, "layer_id": "pen", "surfaces": false}])"},
    {"render_prims", "a", "genko", false, R"([{"op": "render_prims", "page": 2, "layer_id": "paint2", "tone": {"lpi": 50}}])"},
    {"render_prims", "a", "genko", false, R"([{"op": "render_prims", "page": 1, "layer_id": "paint", "ids": ["box", "head", "hand"]}])"},
    {"render_prims", "a", "genko", false, R"([{"op": "render_prims", "page": 1, "layer_id": "paint", "ids": ["head"], "light": [1, 0, 0], "ambient": 0.6, "rgb": [200, 0, 0], "width_mm": 0.5, "kind": "gpen"}])"},
    {"render_prims", "a", "genko", false, R"([{"op": "render_prims", "page": 2, "ids": ["box2"], "surfaces": false}])"},
    {"render_prims", "a", "genko", false, R"([{"op": "render_prims", "page": 1, "layer_id": "paint", "ids": ["room", "stairs"], "lines": false}])"},
    {"render_prims", "a", "genko", false, R"([{"op": "render_prims", "page": 1, "layer_id": "paint", "ids": ["model", "chair"], "tone": {"pattern": "line", "angle": 30, "offset_mm": [1, 2]}}])"},
    {"render_prims", "a", "genko", false, R"([{"op": "render_prims", "page": 1, "layer_id": "paint"}])"},
    {"render_prims", "strict", "human:作者", false, R"([{"op": "render_prims", "page": 2, "layer_id": "paint2", "ids": ["fig2"]}])"},
    {"render_prims", "strict", "human:作者", false, R"([{"op": "render_prims", "page": 1, "layer_id": "draft", "ids": ["head"], "surfaces": false}])"},
    {"render_prims", "a", "genko", false, R"([{"op": "render_prims", "page": 3}])"},
    {"render_prims", "a", "genko", false, R"([{"op": "render_prims", "page": 1, "ids": ["man"]}])"},
    {"render_prims", "a", "genko", false, R"([{"op": "render_prims", "page": 1, "layer_id": "frozen"}])"},
    {"render_prims", "a", "genko", false, R"([{"op": "render_prims", "page": 1, "layer_id": "folder"}])"},
    {"render_prims", "a", "genko", false, R"([{"op": "render_prims", "page": 1, "layer_id": "nope"}])"},
    {"render_prims", "a", "genko", false, R"([{"op": "render_prims", "page": 1, "layer_id": "pen", "surfaces": false, "kind": "laser"}])"},
    {"render_prims", "a", "genko", false, R"([{"op": "render_prims", "page": 1, "layer_id": "paint", "ids": ["head"], "tone": {"lpi": 200}}])"},
    {"render_prims", "a", "genko", false, R"([{"op": "render_prims", "page": 1, "layer": "nope"}])"},
    {"render_prims", "strict", "human:作者", false, R"([{"op": "render_prims", "page": 1, "layer_id": "paint", "ids": ["fig"]}])"},
    {"render_prims", "strict", "human:作者", false, R"([{"op": "render_prims", "page": 1, "ids": ["fig"]}])"},
    {"render_prims", "locked", "ai:hermes", false, R"([{"op": "render_prims", "page": 1, "layer_id": "paint"}])"},
    // add_mannequin
    {"add_mannequin", "a", "genko", false, R"([{"op": "add_mannequin", "page": 3}])"},
    {"add_mannequin", "a", "genko", false, R"([{"op": "add_mannequin", "page": 3, "id": "m1", "preset": "look_back"}])"},
    {"add_mannequin", "a", "genko", false, R"([{"op": "add_mannequin", "page": 3, "id": "m2", "pos": [60, 200, 5], "rot": [0.3, 2.5, 0.2], "height_mm": 120}])"},
    {"add_mannequin", "a", "genko", false, R"([{"op": "add_mannequin", "page": 3, "id": "m3", "pos": [60, 200]}])"},
    {"add_mannequin", "a", "genko", false, R"([{"op": "add_mannequin", "page": 3, "id": 7}])"},
    {"add_mannequin", "a", "genko", false, R"([{"op": "add_mannequin", "page": 3, "id": "m5", "rot": [1, 1, 1], "preset": "walk"}])"},
    {"add_mannequin", "a", "genko", false, R"([{"op": "add_mannequin", "page": 3, "id": "m6", "pos": ["100", "160"]}])"},
    {"add_mannequin", "a", "genko", false, R"([{"op": "add_mannequin", "page": 3, "id": "m7", "height_mm": "120", "preset": "arms_up"}])"},
    {"add_mannequin", "a", "genko", true, R"([{"op": "add_mannequin", "page": 1, "id": "m8"}])"},
    {"add_mannequin", "a", "genko", false, R"([{"op": "add_mannequin", "page": 3, "preset": "dance"}])"},
    {"add_mannequin", "a", "genko", false, R"([{"op": "add_mannequin", "page": 9}])"},
    {"add_mannequin", "a", "genko", false, R"([{"op": "add_mannequin", "page": 3, "pos": 5}])"},
    {"add_mannequin", "a", "genko", false, R"([{"op": "add_mannequin", "page": 3, "height_mm": "x"}])"},
    {"add_mannequin", "a", "genko", false, R"([{"op": "add_mannequin", "page": 3, "rot": 5}])"},
    {"add_mannequin", "locked", "ai:hermes", false, R"([{"op": "add_mannequin", "page": 1}])"},
    // pose_mannequin
    {"pose_mannequin", "a", "genko", false, R"([{"op": "pose_mannequin", "page": 1, "id": "man", "joints": {"l_arm": {"yaw": 0.8}}}])"},
    {"pose_mannequin", "a", "genko", false, R"([{"op": "pose_mannequin", "page": 1, "id": "man", "preset": "arms_up", "height_mm": 120, "joints": {"head": {"yaw": 0.3}}}])"},
    {"pose_mannequin", "a", "genko", false, R"([{"op": "pose_mannequin", "page": 1, "id": "man", "drag": {"handle": "chest", "to": [140, 100]}}])"},
    {"pose_mannequin", "a", "genko", false, R"([{"op": "pose_mannequin", "page": 1, "id": "man", "drag": {"handle": "l_hand", "to": [100, 120]}}])"},
    {"pose_mannequin", "a", "genko", false, R"([{"op": "pose_mannequin", "page": 1, "id": "man", "drag": {"handle": "r_toe", "to": [170, 230]}}])"},
    {"pose_mannequin", "a", "genko", false, R"([{"op": "pose_mannequin", "page": 1, "id": "man", "drag": {"handle": "pelvis", "to": [60, 150]}}])"},
    {"pose_mannequin", "a", "genko", false, R"([{"op": "pose_mannequin", "page": 1, "id": "man", "rot": [0.3, 2.5, 0.2], "pos": [100, 160, 0]}])"},
    {"pose_mannequin", "a", "genko", false, R"([{"op": "pose_mannequin", "page": 1, "id": "fig", "height_mm": 100}])"},
    {"pose_mannequin", "a", "genko", false, R"([{"op": "pose_mannequin", "page": 1, "id": "man", "joints": {"tail": {"yaw": 1}}}])"},
    {"pose_mannequin", "a", "genko", false, R"([{"op": "pose_mannequin", "page": 1, "id": "man", "preset": "sit", "drag": {"handle": "head", "to": [150, 100]}}])"},
    {"pose_mannequin", "a", "genko", false, R"([{"op": "pose_mannequin", "page": 1, "id": "nothing"}])"},
    {"pose_mannequin", "a", "genko", false, R"([{"op": "pose_mannequin", "page": 1, "id": "man", "preset": "dance"}])"},
    {"pose_mannequin", "a", "genko", false, R"([{"op": "pose_mannequin", "page": 1, "id": "man", "drag": {"handle": "tail", "to": [1, 2]}}])"},
    {"pose_mannequin", "a", "genko", false, R"([{"op": "pose_mannequin", "page": 1, "id": "man", "joints": 5}])"},
    {"pose_mannequin", "a", "genko", false, R"([{"op": "pose_mannequin", "page": 1, "id": "man", "drag": {"handle": "pelvis", "to": "ab"}}])"},
    {"pose_mannequin", "locked", "ai:hermes", false, R"([{"op": "pose_mannequin", "page": 1, "id": "man", "preset": "walk"}])"},
    // add_prim3d
    {"add_prim3d", "a", "genko", false, R"([{"op": "add_prim3d", "page": 3, "id": "b1"}])"},
    {"add_prim3d", "a", "genko", false, R"([{"op": "add_prim3d", "page": 3, "id": "b2", "kind": "cylinder", "size": [30, 50, 30], "rot": [0.2, 0.4, 0]}])"},
    {"add_prim3d", "a", "genko", false, R"([{"op": "add_prim3d", "page": 3, "id": "b3", "kind": "stairs", "steps": 40}])"},
    {"add_prim3d", "a", "genko", false, R"([{"op": "add_prim3d", "page": 3, "id": "b4", "kind": "floor"}])"},
    {"add_prim3d", "a", "genko", false, R"([{"op": "add_prim3d", "page": 3, "id": "b5", "kind": "sphere", "size": 35}])"},
    {"add_prim3d", "a", "genko", false, R"([{"op": "add_prim3d", "page": 3, "id": "b6", "kind": "cone", "focal_mm": 120}])"},
    {"add_prim3d", "a", "genko", false, R"([{"op": "add_prim3d", "page": 3, "id": "b7", "kind": "prop", "prop": "desk", "size": [60]}])"},
    {"add_prim3d", "a", "genko", false, R"([{"op": "add_prim3d", "page": 1, "id": "b8", "frame_id": "$F3", "pos": [120, 200, 0], "kind": "floor", "lines": 3, "rot": [0.5, 0, 0]}])"},
    {"add_prim3d", "a", "genko", false, R"([{"op": "add_prim3d", "page": 3, "kind": "prop", "steps": 3}])"},
    {"add_prim3d", "a", "genko", false, R"([{"op": "add_prim3d", "page": 3, "kind": "torus"}])"},
    {"add_prim3d", "a", "genko", false, R"([{"op": "add_prim3d", "page": 3, "kind": "prop", "prop": "spaceship"}])"},
    {"add_prim3d", "a", "genko", false, R"([{"op": "add_prim3d", "page": 3, "size": "x"}])"},
    {"add_prim3d", "a", "genko", false, R"([{"op": "add_prim3d", "page": 3, "pos": 5}])"},
    {"add_prim3d", "a", "genko", false, R"([{"op": "add_prim3d", "page": 1, "frame_id": "nope"}])"},
    {"add_prim3d", "a", "genko", false, R"([{"op": "add_prim3d", "page": 9}])"},
    // add_scene
    {"add_scene", "a", "genko", false, R"([{"op": "add_scene", "page": 3, "id": "s1"}])"},
    {"add_scene", "a", "genko", false, R"([{"op": "add_scene", "page": 3, "id": "s2", "kind": "classroom", "size": 100}])"},
    {"add_scene", "a", "genko", false, R"([{"op": "add_scene", "page": 3, "id": "s3", "kind": "corridor", "size": [60, 70, 300], "focal_mm": 300}])"},
    {"add_scene", "a", "genko", false, R"([{"op": "add_scene", "page": 3, "id": "s4", "kind": "street", "rot": [0, 0.2, 0]}])"},
    {"add_scene", "a", "genko", false, R"([{"op": "add_scene", "page": 1, "id": "s5", "kind": "room", "frame_id": false}])"},
    {"add_scene", "a", "genko", false, R"([{"op": "add_scene", "page": 1, "id": "s6", "kind": "corridor", "frame_id": "$F1", "pos": [100, 80, 0]}])"},
    {"add_scene", "a", "genko", false, R"([{"op": "add_scene", "page": 1, "id": "s7", "pos": [60, 200, 50]}])"},
    {"add_scene", "a", "genko", false, R"([{"op": "add_scene", "page": 2, "id": "s8", "kind": "street", "focal_mm": 10}])"},
    {"add_scene", "a", "genko", false, R"([{"op": "add_scene", "page": 3, "kind": "castle"}])"},
    {"add_scene", "a", "genko", false, R"([{"op": "add_scene", "page": 3, "size": "x"}])"},
    {"add_scene", "a", "genko", false, R"([{"op": "add_scene", "page": 1, "frame_id": "nope"}])"},
    {"add_scene", "a", "genko", false, R"([{"op": "add_scene", "page": 9}])"},
    {"add_scene", "locked", "ai:hermes", false, R"([{"op": "add_scene", "page": 1}])"},
    // edit_prim
    {"edit_prim", "a", "genko", false, R"([{"op": "edit_prim", "page": 1, "id": "box", "pos": [100, 150, 0], "rot": [0, 0.3, 0]}])"},
    {"edit_prim", "a", "genko", false, R"([{"op": "edit_prim", "page": 1, "id": "box", "size": [10, 20]}])"},
    {"edit_prim", "a", "genko", false, R"([{"op": "edit_prim", "page": 1, "id": "stairs", "focal_mm": 5}])"},
    {"edit_prim", "a", "genko", false, R"([{"op": "edit_prim", "page": 1, "id": "man", "size": [1, 110, 1]}])"},
    {"edit_prim", "a", "genko", false, R"([{"op": "edit_prim", "page": 1, "id": "room", "pos": [150, 200, 120]}])"},
    {"edit_prim", "a", "genko", false, R"([{"op": "edit_prim", "page": 1, "id": "fig", "size": [50, 100, 25], "focal_mm": 300}])"},
    {"edit_prim", "a", "genko", false, R"([{"op": "edit_prim", "page": 2, "id": "box2", "rot": "12"}])"},
    {"edit_prim", "a", "genko", true, R"([{"op": "edit_prim", "page": 1, "id": "chair", "pos": [1, 2, 3]}])"},
    {"edit_prim", "a", "genko", false, R"([{"op": "edit_prim", "page": 1, "id": "head", "pos": null, "rot": [0, 1]}])"},
    {"edit_prim", "a", "genko", false, R"([{"op": "edit_prim", "page": 1, "id": "nothing"}])"},
    {"edit_prim", "a", "genko", false, R"([{"op": "edit_prim", "page": 1, "id": "box", "pos": 5}])"},
    {"edit_prim", "a", "genko", false, R"([{"op": "edit_prim", "page": 1, "id": "box", "focal_mm": "x"}])"},
    {"edit_prim", "a", "genko", false, R"([{"op": "edit_prim", "page": 9, "id": "box"}])"},
    {"edit_prim", "locked", "ai:hermes", false, R"([{"op": "edit_prim", "page": 1, "id": "box", "pos": [1, 2]}])"},
    // delete_prim
    {"delete_prim", "a", "genko", false, R"([{"op": "delete_prim", "page": 1, "id": "box"}])"},
    {"delete_prim", "a", "genko", false, R"([{"op": "delete_prim", "page": 1, "id": "fig"}])"},
    {"delete_prim", "a", "genko", false, R"([{"op": "delete_prim", "page": 1, "id": "man"}])"},
    {"delete_prim", "a", "genko", false, R"([{"op": "delete_prim", "page": 1, "id": "room"}, {"op": "add_scene", "page": 1, "id": "room"}])"},
    {"delete_prim", "a", "genko", true, R"([{"op": "delete_prim", "page": 1, "id": "model"}])"},
    {"delete_prim", "a", "genko", false, R"([{"op": "delete_prim", "page": 2, "id": "box2"}])"},
    {"delete_prim", "a", "genko", false, R"([{"op": "add_head", "page": 1, "id": "head"}, {"op": "delete_prim", "page": 1, "id": "head"}])"},
    {"delete_prim", "a", "genko", false, R"([{"op": "delete_prim", "page": 1, "id": "chair"}, {"op": "delete_prim", "page": 1, "id": "stairs"}])"},
    {"delete_prim", "a", "genko", false, R"([{"op": "delete_prim", "page": 1, "id": "nothing"}])"},
    {"delete_prim", "a", "genko", false, R"([{"op": "delete_prim", "page": 1, "id": "box"}, {"op": "delete_prim", "page": 1, "id": "box"}])"},
    {"delete_prim", "a", "genko", false, R"([{"op": "delete_prim", "page": 9, "id": "box"}])"},
    {"delete_prim", "a", "genko", false, R"([{"op": "delete_prim", "page": 1}])"},
    {"delete_prim", "locked", "ai:hermes", false, R"([{"op": "delete_prim", "page": 1, "id": "box"}])"},
    // trace_prims
    {"trace_prims", "a", "genko", false, R"([{"op": "trace_prims", "page": 1}])"},
    {"trace_prims", "a", "genko", false, R"([{"op": "trace_prims", "page": 1, "layer_id": "pen", "ids": ["box"]}])"},
    {"trace_prims", "a", "genko", false, R"([{"op": "trace_prims", "page": 1, "layer_id": "pen", "ids": ["fig", "man"], "kind": "gpen"}])"},
    {"trace_prims", "a", "genko", false, R"([{"op": "trace_prims", "page": 1, "layer_id": "paint", "ids": ["head"], "width_mm": 0.6, "rgb": [10, 20, 200]}])"},
    {"trace_prims", "a", "genko", false, R"([{"op": "trace_prims", "page": 2, "layer_id": "paint2"}])"},
    {"trace_prims", "a", "genko", false, R"([{"op": "trace_prims", "page": 1, "ids": ["room"], "layer": "ink"}])"},
    {"trace_prims", "a", "genko", false, R"([{"op": "trace_prims", "page": 1, "ids": ["model", "chair", "stairs"], "layer": "name", "kind": "oil"}])"},
    {"trace_prims", "a", "genko", false, R"([{"op": "trace_prims", "page": 1, "ids": ["hand"], "width_mm": "0.4"}])"},
    {"trace_prims", "strict", "human:作者", false, R"([{"op": "trace_prims", "page": 2, "ids": ["fig2"]}])"},
    {"trace_prims", "a", "genko", false, R"([{"op": "trace_prims", "page": 3}])"},
    {"trace_prims", "a", "genko", false, R"([{"op": "trace_prims", "page": 1, "layer_id": "frozen"}])"},
    {"trace_prims", "a", "genko", false, R"([{"op": "trace_prims", "page": 1, "layer_id": "folder"}])"},
    {"trace_prims", "a", "genko", false, R"([{"op": "trace_prims", "page": 1, "kind": "laser"}])"},
    {"trace_prims", "strict", "human:作者", false, R"([{"op": "trace_prims", "page": 1, "layer_id": "pen"}])"},
    {"trace_prims", "a", "genko", false, R"([{"op": "trace_prims", "page": 1, "ids": ["box"], "width_mm": "wide"}])"},
    {"trace_prims", "locked", "ai:hermes", false, R"([{"op": "trace_prims", "page": 1}])"},
    // ruler_from_3d
    {"ruler_from_3d", "a", "genko", false, R"([{"op": "ruler_from_3d", "page": 1}])"},
    {"ruler_from_3d", "a", "genko", false, R"([{"op": "ruler_from_3d", "page": 1, "prim_id": "box", "id": "rb"}])"},
    {"ruler_from_3d", "a", "genko", false, R"([{"op": "ruler_from_3d", "page": 1, "prim_id": "head", "grid": 10}])"},
    {"ruler_from_3d", "a", "genko", false, R"([{"op": "ruler_from_3d", "page": 2, "id": "r2"}])"},
    {"ruler_from_3d", "a", "genko", false, R"([{"op": "ruler_from_3d", "page": 2, "prim_id": "box2", "grid": 60}])"},
    {"ruler_from_3d", "a", "genko", false, R"([{"op": "ruler_from_3d", "page": 1, "prim_id": "room", "grid": 0}])"},
    {"ruler_from_3d", "a", "genko", false, R"([{"op": "ruler_from_3d", "page": 1, "prim_id": "model"}])"},
    {"ruler_from_3d", "a", "genko", false, R"([{"op": "set_camera", "page": 1, "turn": 1.2}, {"op": "ruler_from_3d", "page": 1, "prim_id": "stairs"}])"},
    {"ruler_from_3d", "a", "genko", false, R"([{"op": "ruler_from_3d", "page": 3}])"},
    {"ruler_from_3d", "a", "genko", false, R"([{"op": "ruler_from_3d", "page": 1, "prim_id": "nope"}])"},
    {"ruler_from_3d", "a", "genko", false, R"([{"op": "ruler_from_3d", "page": 1, "prim_id": "man"}])"},
    {"ruler_from_3d", "a", "genko", false, R"([{"op": "ruler_from_3d", "page": 1, "id": "pr"}])"},
    {"ruler_from_3d", "a", "genko", false, R"([{"op": "add_prim3d", "page": 3, "id": "far", "focal_mm": 10000000}, {"op": "ruler_from_3d", "page": 3, "prim_id": "far"}])"},
    {"ruler_from_3d", "locked", "ai:hermes", false, R"([{"op": "ruler_from_3d", "page": 1}])"},
    // camera_from_ruler
    {"camera_from_ruler", "a", "genko", false, R"([{"op": "camera_from_ruler", "page": 1, "id": "pr"}])"},
    {"camera_from_ruler", "a", "genko", false, R"([{"op": "camera_from_ruler", "page": 1, "id": "pr1"}])"},
    {"camera_from_ruler", "a", "genko", false, R"([{"op": "camera_from_ruler", "page": 1, "id": "pr3", "prim_id": "box"}])"},
    {"camera_from_ruler", "a", "genko", false, R"([{"op": "set_camera", "page": 1, "focal_mm": 600, "target": [80, 120]}, {"op": "camera_from_ruler", "page": 1, "id": "pr"}])"},
    {"camera_from_ruler", "a", "genko", true, R"([{"op": "camera_from_ruler", "page": 1, "id": "pr1", "prim_id": "head"}])"},
    {"camera_from_ruler", "a", "genko", false, R"([{"op": "camera_from_ruler", "page": 1, "id": "pr3", "prim_id": "stairs"}])"},
    {"camera_from_ruler", "a", "genko", false, R"([{"op": "ruler_from_3d", "page": 1, "prim_id": "box", "id": "rb"}, {"op": "camera_from_ruler", "page": 1, "id": "rb", "prim_id": "model"}])"},
    {"camera_from_ruler", "a", "genko", false, R"([{"op": "camera_from_ruler", "page": 1, "id": "pr", "prim_id": "room"}])"},
    {"camera_from_ruler", "a", "genko", false, R"([{"op": "camera_from_ruler", "page": 1, "id": "ln"}])"},
    {"camera_from_ruler", "a", "genko", false, R"([{"op": "camera_from_ruler", "page": 1, "id": "nope"}])"},
    {"camera_from_ruler", "a", "genko", false, R"([{"op": "camera_from_ruler", "page": 3, "id": "pr"}])"},
    {"camera_from_ruler", "a", "genko", false, R"([{"op": "camera_from_ruler", "page": 1, "id": "pr", "prim_id": "nothing"}])"},
    {"camera_from_ruler", "locked", "ai:hermes", false, R"([{"op": "camera_from_ruler", "page": 1, "id": "pr"}])"},
    // the book ops with the 3D ones
    {"set_note", "a", "genko", false, R"([{"op": "add_figure", "page": 3, "id": "z"}, {"op": "set_note", "page": 3, "note": "3D"}, {"op": "delete_page", "page": 1}])"},
};

// What the C++ build refuses where Python takes it in and leaves a book that is broken (docs/cpp-migration/SPEC.md §2):
// each is run through Python too, to show what it does there. 1e999 is an infinity (Python's json reads it so; the
// C++ build is given one too).
struct Refusal {
    const char* what;
    const char* ops;
};

const Refusal kRefusals[] = {
    // numbers that are not finite, kept in the book (Python's json writes them as Infinity and NaN)
    {"a figure at infinity", R"([{"op": "add_figure", "page": 1, "id": "x", "pos": [1e999, 0, 0]}])"},
    {"a figure of infinite height", R"([{"op": "pose_figure", "page": 1, "id": "fig", "height_mm": 1e999}])"},
    {"a head of infinite size", R"([{"op": "add_head", "page": 1, "id": "x", "size_mm": 1e999}])"},
    {"a hand turned by infinity", R"([{"op": "add_hand", "page": 1, "id": "x", "rot": [0, 1e999, 0]}])"},
    {"a model of infinite size", R"([{"op": "import_model", "page": 1, "id": "x", "obj": $CUBE, "size_mm": 1e999}])"},
    {"a model with a corner at infinity", R"([{"op": "import_model", "page": 1, "id": "x", "obj": $OBJ_INFINITE}])"},
    {"a camera turned by infinity", R"([{"op": "set_camera", "page": 1, "turn": 1e999}])"},
    {"a camera aimed at infinity", R"([{"op": "set_camera", "page": 1, "target": [1e999, 5]}])"},
    {"a light from infinity", R"([{"op": "set_light", "page": 1, "dir": [1e999, 0, 0]}])"},
    {"a screen at an infinite angle", R"([{"op": "render_prims", "page": 1, "layer_id": "paint", "tone": {"angle": 1e999}}])"},
    {"lines of infinite width", R"([{"op": "trace_prims", "page": 1, "layer_id": "pen", "ids": ["box"], "width_mm": 1e999}])"},
    {"a box moved to infinity", R"([{"op": "edit_prim", "page": 1, "id": "box", "pos": [1e999, 1, 1]}])"},
    {"a guide of infinite focal length", R"([{"op": "add_prim3d", "page": 1, "id": "x", "focal_mm": 1e999}])"},
    {"a box of infinite size", R"([{"op": "add_prim3d", "page": 1, "id": "x", "size": [1e999, 1, 1]}])"},
    {"a scene of infinite size", R"([{"op": "add_scene", "page": 1, "id": "x", "size": 1e999}])"},
    {"a mannequin of infinite height", R"([{"op": "add_mannequin", "page": 1, "id": "x", "height_mm": 1e999}])"},
    {"a mannequin dragged to infinity", R"([{"op": "pose_mannequin", "page": 1, "id": "man", "drag": {"handle": "pelvis", "to": [1e999, 0]}}])"},
    // mannequins that cannot be drawn
    {"a mannequin of negative height", R"([{"op": "add_mannequin", "page": 1, "id": "x", "height_mm": -50}])"},
    {"a mannequin's joint that is not a number", R"([{"op": "pose_mannequin", "page": 1, "id": "man", "joints": {"l_arm": {"yaw": "x"}}}])"},
    {"a mannequin at a place that is not a number", R"([{"op": "add_mannequin", "page": 1, "id": "x", "pos": ["a", "b"]}])"},
    {"a mannequin turned by text", R"([{"op": "pose_mannequin", "page": 1, "id": "man", "rot": ["x", 0, 0]}])"},
    // perspective grids that rulers do not take
    {"a grid of 100 lines", R"([{"op": "ruler_from_3d", "page": 1, "prim_id": "box", "grid": 100}])"},
    {"a grid of -1 lines", R"([{"op": "ruler_from_3d", "page": 1, "prim_id": "box", "grid": -1}])"},
    // models that cannot be drawn, or that would make the book enormous
    {"a glTF face at a corner that is not there", R"([{"op": "import_model", "page": 1, "id": "x", "glb": $GLB_BAD_INDEX}])"},
    {"glTF nodes inside themselves", R"([{"op": "import_model", "page": 1, "id": "x", "gltf": $GLTF_LOOP}])"},
    {"a model of 240001 corners", R"([{"op": "import_model", "page": 1, "id": "x", "obj": $OBJ_HUGE}])"},
    {"faces of 510000 corners in all", R"([{"op": "import_model", "page": 1, "id": "x", "obj": $OBJ_WIDE}])"},
};

// Infinities and odd model files that Python works through without harm: the same reply, book and 3D state.
const Refusal kInfinities[] = {
    {"a figure's hand dragged to infinity", R"([{"op": "pose_figure", "page": 1, "id": "fig", "drag": {"handle": "r_hand", "to": [1e999, 0]}}])"},
    {"a figure's foot dragged to infinity", R"([{"op": "pose_figure", "page": 1, "id": "fig", "drag": {"handle": "l_foot", "to": [50, 1e999]}}])"},
    {"a figure's hand reaching for infinity", R"([{"op": "pose_figure", "page": 1, "id": "fig", "drag": {"handle": "r_hand", "ik": true, "to": [1e999, 50]}}])"},
    {"a figure's elbow dragged to infinity", R"([{"op": "pose_figure", "page": 1, "id": "fig", "drag": {"handle": "l_elbow", "to": [-1e999, -1e999]}}])"},
    {"a mannequin's hand dragged to infinity", R"([{"op": "pose_mannequin", "page": 1, "id": "man", "drag": {"handle": "l_hand", "to": [1e999, 0]}}])"},
    {"surfaces lit from infinity", R"([{"op": "render_prims", "page": 1, "layer_id": "paint", "light": [1e999, 0, 0]}])"},
    {"surfaces in infinite ambient light", R"([{"op": "render_prims", "page": 2, "layer_id": "paint2", "ambient": 1e999}])"},
    {"an infinite ambient light", R"([{"op": "set_light", "page": 1, "ambient": 1e999}])"},
    {"a figure of infinite height", R"([{"op": "add_figure", "page": 1, "id": "x", "height_mm": 1e999}])"},
    {"a camera of infinite focal length", R"([{"op": "set_camera", "page": 1, "focal_mm": 1e999}])"},
    {"a glTF accessor of a negative count", R"([{"op": "import_model", "page": 1, "id": "x", "glb": $GLB_NEGATIVE_COUNT}])"},
    {"glTF corners of a negative count", R"([{"op": "import_model", "page": 1, "id": "x", "glb": $GLB_NEGATIVE_POSITIONS}])"},
    {"a glTF accessor of a negative count, half a value off", R"([{"op": "import_model", "page": 1, "id": "x", "glb": $GLB_NEGATIVE_ODD}])"},
};

// The ops of a case for the C++ build: as Python reads them (1e999 an infinity, -1e999 its negative).
void put_infinities(Json& value) {
    if (value.is_string() && (value == Json("<inf>") || value == Json("<-inf>"))) {
        value = (value == Json("<inf>") ? 1.0 : -1.0) * std::numeric_limits<double>::infinity();
    } else if (value.is_array() || value.is_object()) {
        for (auto& item : value) put_infinities(item);
    }
}

Json with_infinities(const std::string& text) {
    std::string marked;  // (1e999 and -1e999 outside JSON strings as marks; a model's text keeps its own)
    bool in_string = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (in_string) {
            marked += c;
            if (c == '\\' && i + 1 < text.size()) {
                marked += text[++i];
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') in_string = true;
        if (text.compare(i, 6, "-1e999") == 0) {
            marked += "\"<-inf>\"";
            i += 5;
        } else if (text.compare(i, 5, "1e999") == 0) {
            marked += "\"<inf>\"";
            i += 4;
        } else {
            marked += c;
        }
    }
    Json ops = genko::core::parse_python_json(marked);
    put_infinities(ops);
    return ops;
}

using genko::test::replace_all;
using genko::test::state_of;

}  // namespace

class TestContract3dOps : public QObject {
    Q_OBJECT

    QTemporaryDir scratch_;
    QString books_;
    Json models_;
    std::map<std::string, std::vector<std::string>> frames_;  // book → page 1's panels in reading order

    QString path(const QString& relative) const { return scratch_.path() + QLatin1Char('/') + relative; }

    std::string expand(const Case& c) { return expand(c.ops, c.book); }

    std::string expand(const char* ops, const char* book) {
        std::string text = ops;
        std::vector<std::string> names;  // (the longest first: $GLB_NODES before $GLB)
        for (const auto& [name, value] : models_.items()) names.push_back(name);
        std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
        for (const std::string& name : names) text = replace_all(text, "$" + name, genko::core::dump_python(models_[name]));
        const auto& frames = frames_[book];
        for (std::size_t i = 0; i < frames.size(); ++i) text = replace_all(text, "$F" + std::to_string(i + 1), frames[i]);
        return text;
    }

    void expect_same(const Json& got, const Json& want, const std::string& what, int& failures) {
        std::string where;
        if (!genko::test::strict_equal(got, want, &where)) {
            qWarning("%s: %s", what.c_str(), where.c_str());
            ++failures;
        }
    }

    // The two project.json payloads with each picture made anew compared by its pixels (the PNG bytes differ).
    void same_project(const Json& mine, const Json& theirs, const QString& py_book, const QString& cpp_assets, const std::string& what,
                      int& failures) {
        const std::string difference = genko::test::project_difference(mine, theirs, py_book, cpp_assets);
        if (!difference.empty()) {
            qWarning("%s: %s", what.c_str(), difference.c_str());
            ++failures;
        }
    }

private slots:
    void initTestCase() {
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
        QVERIFY(scratch_.isValid());
        books_ = path("books");
        const auto r = genko::test::geom3d_harness({"fixtures", books_}, path("py"));
        QVERIFY2(r.finished && r.exit_code == 0, r.err.right(3000).constData());
        models_ = genko::test::read_json(books_ + "/models.json");
        for (const char* book : {"a", "strict", "locked"}) {
            const auto loaded = genko::storage::load_document(genko::storage::path_from_utf8((books_ + "/" + book + ".genko").toStdString()));
            for (const genko::core::Frame* f : loaded.document.page(0).leaf_frames()) frames_[book].push_back(f->id);
        }
    }

    void fixedCasesMatchPython() {
        Json jobs = Json::array();
        int n = 0;
        for (const Case& c : kCases) {
            Json job = Json::object();
            job["book"] = (books_ + "/" + c.book + ".genko").toStdString();
            job["steps"] = Json::array({genko::core::parse_python_json(expand(c))});
            job["agent"] = c.agent;
            job["dry_run"] = c.dry_run;
            job["ids"] = true;
            job["out"] = path(QStringLiteral("out/%1.json").arg(n)).toStdString();
            job["dest"] = path(QStringLiteral("out/%1.genko").arg(n)).toStdString();
            jobs.push_back(job);
            ++n;
        }
        QDir().mkpath(path("out"));
        genko::test::write_bytes(path("out/jobs.json"), genko::core::dump_python(jobs));
        const auto ran = genko::test::geom3d_harness({"apply", path("out/jobs.json")}, path("py"));
        QVERIFY2(ran.finished && ran.exit_code == 0, ran.err.right(3000).constData());

        int failures = 0;
        std::map<std::string, std::pair<int, int>> tally;  // op → (succeeded, failed)
        n = 0;
        for (const Case& c : kCases) {
            const std::string name = std::string(c.op) + " #" + std::to_string(n) + " " + std::string(c.ops).substr(0, 90);
            const QString out = path(QStringLiteral("out/%1").arg(n));
            const Json want = genko::test::read_json(out + ".json")["steps"][0];
            ++n;
            if (want["reply"].contains("crash")) {
                qWarning("%s: Python crashed (%s: %s)", name.c_str(), want["reply"]["crash"].get<std::string>().c_str(),
                         want["reply"]["error"].get<std::string>().c_str());
                ++failures;
                continue;
            }
            if (want["nonfinite"].get<bool>()) {
                qWarning("%s: Python kept a number that is not finite", name.c_str());
                ++failures;
                continue;
            }
            Json got;
            genko::core::Document doc;
            genko::core::Document applied;
            bool ok = false;
            {
                const genko::core::ScopedIdSource ids(genko::core::counting_ids());
                doc = genko::storage::load_document(genko::storage::path_from_utf8((books_ + "/" + c.book + ".genko").toStdString())).document;
                try {
                    const auto result = genko::core::CommandBus(genko::render::ops_registry())
                                            .apply(doc, genko::core::parse_python_json(expand(c)), genko::core::Actor(c.agent), c.dry_run);
                    got = Json::object();
                    got["ok"] = true;
                    got["applied"] = result.applied;
                    got["snapshot"] = genko::storage::snapshot(result.doc);
                    got["job_id"] = genko::core::new_id();
                    if (result.has_warnings) got["warnings"] = result.warnings;
                    applied = c.dry_run ? doc : result.doc;
                    ok = true;
                } catch (const genko::core::ApplyError& error) {
                    got = Json::object({{"ok", false}, {"error", error.what()}});
                    applied = doc;
                }
            }
            auto& t = tally[c.op];
            (ok ? t.first : t.second) += 1;
            expect_same(got, want["reply"], name + " reply", failures);
            expect_same(genko::storage::snapshot(applied, true), want["full"], name + " full snapshot", failures);
            expect_same(state_of(applied), want["state"], name + " 3D state", failures);
            if (ok && !c.dry_run) {
                const QString assets = out + ".cpp-assets";
                genko::storage::AssetStore store(genko::storage::path_from_utf8(assets.toStdString()));
                same_project(genko::storage::project_payload_v4(applied, store), genko::test::read_json(out + ".genko/project.json"), out + ".genko",
                             assets, name, failures);
            }
        }
        for (const auto& [op, t] : tally) {
            qInfo("%s: %d succeeded, %d failed (the same as Python)", op.c_str(), t.first, t.second);
            if (op == "set_note") continue;
            QVERIFY2(t.first >= 8 && t.second >= 4, (op + ": too few cases").c_str());
        }
        QCOMPARE(tally.size(), std::size_t{18});
        QCOMPARE(failures, 0);
    }

    void safeSideRefusals() {
        // (the jobs file is written by hand: the C++ Json holds no infinity to write, Python reads 1e999 as one)
        std::vector<const Refusal*> all;
        for (const Refusal& r : kRefusals) all.push_back(&r);
        for (const Refusal& r : kInfinities) all.push_back(&r);
        std::string jobs = "[";
        for (std::size_t n = 0; n < all.size(); ++n) {
            const Json job = Json::object({{"book", (books_ + "/a.genko").toStdString()},
                                           {"agent", "genko"},
                                           {"ids", true},
                                           {"probe", true},
                                           {"out", path(QStringLiteral("refused/%1.json").arg(n)).toStdString()}});
            std::string text = genko::core::dump_python(job);
            text.pop_back();
            jobs += (n > 0 ? ", " : "") + text + ", \"steps\": [" + expand(all[n]->ops, "a") + "]}";
        }
        jobs += "]";
        QDir().mkpath(path("refused"));
        genko::test::write_bytes(path("refused/jobs.json"), jobs);
        const auto ran = genko::test::geom3d_harness({"apply", path("refused/jobs.json")}, path("py"));
        QVERIFY2(ran.finished && ran.exit_code == 0, ran.err.right(3000).constData());

        const genko::core::Document doc =
            genko::storage::load_document(genko::storage::path_from_utf8((books_ + "/a.genko").toStdString())).document;
        int failures = 0;
        // the infinities Python works through: the same as Python
        for (std::size_t n = std::size(kRefusals); n < all.size(); ++n) {
            const Refusal& r = *all[n];
            const Json want = genko::test::read_json(path(QStringLiteral("refused/%1.json").arg(n)))["steps"][0];
            if (want["reply"].contains("crash") || want["nonfinite"] == Json(true) || !want["probe"]["render"].is_null()) {
                qWarning("%s: Python breaks the book (%s)", r.what, genko::core::dump_python(want["reply"]).substr(0, 300).c_str());
                ++failures;
                continue;
            }
            const genko::core::ScopedIdSource ids(genko::core::counting_ids());  // (the ids as Python's: the book read first)
            const genko::core::Document book =
                genko::storage::load_document(genko::storage::path_from_utf8((books_ + "/a.genko").toStdString())).document;
            Json got;
            genko::core::Document applied = book;
            try {
                const auto result = genko::core::CommandBus(genko::render::ops_registry())
                                        .apply(book, with_infinities(expand(r.ops, "a")), genko::core::Actor("genko"));
                got = Json::object({{"ok", true}, {"applied", result.applied}, {"snapshot", genko::storage::snapshot(result.doc)}});
                got["job_id"] = genko::core::new_id();
                if (result.has_warnings) got["warnings"] = result.warnings;
                applied = result.doc;
            } catch (const genko::core::ApplyError& error) {
                got = Json::object({{"ok", false}, {"error", error.what()}});
            }
            expect_same(got, want["reply"], std::string(r.what) + " reply", failures);
            expect_same(genko::storage::snapshot(applied, true), want["full"], std::string(r.what) + " full snapshot", failures);
            expect_same(state_of(applied), want["state"], std::string(r.what) + " 3D state", failures);
            qInfo("%s: the same as Python (%s)", r.what, got["ok"] == Json(true) ? "applied" : "refused by both");
        }
        // what the C++ build refuses
        for (std::size_t n = 0; n < std::size(kRefusals); ++n) {
            const Refusal& r = kRefusals[n];
            const Json step = genko::test::read_json(path(QStringLiteral("refused/%1.json").arg(n)))["steps"][0];
            const Json& reply = step["reply"];
            const Json& probe = step["probe"];
            const auto text = [](const Json& v) { return v.is_string() ? v.get<std::string>() : genko::core::dump_python(v); };
            std::string python;  // how Python breaks the book (nothing: it does not)
            if (reply.contains("crash")) {
                python = "stops with " + text(reply["crash"]) + " (" + text(reply["error"]).substr(0, 100) + ")";
            } else if (reply["ok"] == Json(false)) {
                python = "";
            } else if (step["nonfinite"] == Json(true)) {
                python = "keeps a number that is not finite";
            } else if (probe["strokes_nonfinite"] == Json(true)) {
                python = "keeps lines of numbers that are not finite";
            } else if (!probe["render"].is_null()) {
                python = "keeps it, and the page can no longer be drawn (" + text(probe["render"]) + ")";
            } else if (!probe["reload"].is_null()) {
                python = "keeps it, and the book can no longer be read (" + text(probe["reload"]) + ")";
            } else if (!probe["rulers"].is_null()) {
                python = "keeps a ruler that add_ruler and edit_ruler refuse (" + text(probe["rulers"]) + ")";
            } else if (probe["mesh_vertices"].get<std::int64_t>() > genko::core::mesh3d::kMaxVertices ||
                       probe["mesh_corners"].get<std::int64_t>() > genko::core::mesh3d::kMaxCorners) {
                python = "keeps a model of " + text(probe["mesh_vertices"]) + " corners and faces of " + text(probe["mesh_corners"]) +
                         " corners in all";
            }
            std::string cpp;
            try {
                const genko::core::ScopedIdSource ids(genko::core::counting_ids());
                (void)genko::core::CommandBus(genko::render::ops_registry())
                    .apply(doc, with_infinities(expand(r.ops, "a")), genko::core::Actor("genko"));
            } catch (const genko::core::ApplyError& error) {
                cpp = error.what();
            }
            if (python.empty()) {
                qWarning("%s: Python does not break the book (%s)", r.what, genko::core::dump_python(reply).substr(0, 300).c_str());
                ++failures;
            }
            if (cpp.empty()) {
                qWarning("%s: the C++ build takes it in", r.what);
                ++failures;
            }
            qInfo("%s: Python %s; C++ refuses: %s", r.what, python.c_str(), cpp.c_str());
        }
        QCOMPARE(failures, 0);
    }
};

QTEST_GUILESS_MAIN(TestContract3dOps)
#include "test_contract_3d_ops.moc"
