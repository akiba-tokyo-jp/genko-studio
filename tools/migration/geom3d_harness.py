#!/usr/bin/env python3
"""The Python reference for the C++ 3D tests (native/tests/*3d*): runs the baseline (src/genko, numpy on OpenBLAS) and
writes what the C++ build must match. Development only; nothing here goes into the product.

    PYTHONPATH=src python3 tools/migration/geom3d_harness.py <command> ...

Commands (JSON is UTF-8 without escapes):
  blas                          whether numpy's matmul and dot on this machine round as native/src/core/linalg3 does:
                                {"ok": bool, "failed": [...]} on stdout
  geometry OUT --seed N --count K
                                K random prims (figures with random builds, joints and hands; heads, hands, models,
                                boxes, guides, props, scenes, mannequins) seen through random cameras, and what mesh3d,
                                prim3d, persp3d, mannequin and threeops make of them: the skeleton, the mesh, its
                                projection, normals, surfaces, pen lines, edges, boxes, vanishing points, the camera for
                                a ruler, drags and IK (exact bits for small values, sha256 of the bits for arrays)
  models OUT --seed N --count K K random OBJ and glTF files (good and broken) and what read_obj / read_gltf make of them
  fixtures OUT                  the books of the fixed op cases (OUT/a.genko: every kind of 3D, panels, layers, a camera,
                                a light, rulers; OUT/strict.genko: strict_gates; OUT/locked.genko: a page locked)
  poses CONFIG                  the pose library (genko.poses) through fixed steps with GENKO_CONFIG_DIR=CONFIG: each
                                step's result and the poses on stdout, CONFIG/poses.json as written
  make-books OUT --seed N --count K [--render]
                                K random books with 3D on their pages (made with apply_ops); --render: nothing on them
                                this C++ step does not draw (no lines, nombres, tones, effects)
  apply JOBS                    for each job {"book", "steps": [[op…]…], "agent", "dry_run", "ids", "out", "dest"?,
                                "probe"?}: the steps applied one after the other to the book read once (apply_ops), each
                                step's reply ({"ok": false, "error"} for an ApplyError; "crash": the exception's type for
                                anything else), the full snapshot and the 3D state after it (with "probe": whether the book
                                can still be drawn and read back); saved into dest at the end
  random-ops OUT --seed N --count K --books DIR
                                K random op sequences over the books of DIR (the 3D ops and the book ops of M1)
"""
from __future__ import annotations

import argparse
import base64
import copy
import hashlib
import io
import json
import math
import random
import struct
import sys
from fractions import Fraction
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT / "src") not in sys.path:
    sys.path.insert(0, str(ROOT / "src"))
sys.path.insert(0, str(Path(__file__).resolve().parent))


def bits(x) -> str:
    return "%016x" % struct.unpack("<Q", struct.pack("<d", float(x)))[0]


def dumps(value) -> str:
    return json.dumps(value, ensure_ascii=False)


def sha_f64(values) -> str:
    import numpy as np

    return hashlib.sha256(np.ascontiguousarray(np.asarray(values, dtype="<f8")).tobytes()).hexdigest()


def sha_f32(values) -> str:
    import numpy as np

    return hashlib.sha256(np.ascontiguousarray(np.asarray(values, dtype="<f4")).tobytes()).hexdigest()


def sha_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def lines_text(lines) -> str:
    """Polylines as one text: each point's x and y bits, "," between points, ";" between lines."""
    return ";".join(",".join(bits(p[0]) + bits(p[1]) for p in line) for line in lines)


def lines_digest(lines) -> dict:
    return {"count": len(lines), "points": sum(len(line) for line in lines), "sha": sha_text(lines_text(lines))}


def edges_digest(edges) -> dict:
    text = ";".join(bits(a[0]) + bits(a[1]) + bits(b[0]) + bits(b[1]) + ("1" if seen else "0") for a, b, seen in edges)
    return {"count": len(edges), "sha": sha_text(text)}


# --- blas: numpy's rounding on this machine ---------------------------------------------------------------------------


def _fma(a, b, c):
    a, b, c = float(a), float(b), float(c)
    p = Fraction(a) * Fraction(b)
    r = p + Fraction(c)
    if r == 0:
        if p == 0 and c == 0:
            neg = (math.copysign(1, a) * math.copysign(1, b)) < 0
            return -0.0 if (neg and math.copysign(1, c) < 0) else 0.0
        return 0.0
    return float(r)


def blas_check(rounds: int = 400) -> dict:
    import numpy as np

    rng = random.Random(5)

    def rnd():
        r = rng.random()
        if r < 0.3:
            t = rng.uniform(-4, 4)
            return rng.choice([math.cos(t), math.sin(t), -math.sin(t), 0.0, 1.0, -0.0])
        if r < 0.6:
            return rng.uniform(-1, 1)
        return rng.uniform(-300, 300) * rng.choice([1.0, 1e-3, 37.25])

    def chain(row, col):
        s = 0.0
        for a, b in zip(row, col):
            s = _fma(a, b, s)
        return s

    def gemv3(row, x):
        return 0.0 + _fma(row[2], x[2], _fma(row[0], x[0], float(row[1]) * float(x[1])))

    def gemv4(row, x):
        p = [float(a) * float(b) for a, b in zip(row, x)]
        return 0.0 + ((p[0] + p[2]) + (p[1] + p[3]))

    def dot3(a, b):
        p = [float(x) * float(y) for x, y in zip(a, b)]
        return 0.0 + ((p[0] + p[1]) + p[2])

    failed = set()
    for _ in range(rounds):
        a = np.array([[rnd() for _ in range(3)] for _ in range(3)])
        b = np.array([[rnd() for _ in range(3)] for _ in range(3)])
        v = np.array([rnd() for _ in range(3)])
        c = a @ b
        if any(bits(c[i, j]) != bits(chain(a[i], b[:, j])) for i in range(3) for j in range(3)):
            failed.add("dgemm 3x3")
        y = a @ v
        if any(bits(y[i]) != bits(gemv3(a[i], v)) for i in range(3)):
            failed.add("dgemv 3x3")
        n = rng.choice([1, 2, 5, 40])
        rows = np.array([[rnd() for _ in range(3)] for _ in range(n)])
        out = rows @ a.T
        want = (lambda i, j: gemv3(a[j], rows[i])) if n == 1 else (lambda i, j: chain(rows[i], a[j]))
        if any(bits(out[i, j]) != bits(want(i, j)) for i in range(n) for j in range(3)):
            failed.add(f"rows @ M.T ({n} rows)")
        if bits(v @ b[0]) != bits(dot3(v, b[0])) or bits(np.linalg.norm(v)) != bits(math.sqrt(dot3(v, v))):
            failed.add("ddot / norm")
        m4 = np.array([[rnd() for _ in range(4)] for _ in range(4)])
        n4 = np.array([[rnd() for _ in range(4)] for _ in range(4)])
        c4 = m4 @ n4
        if any(bits(c4[i, j]) != bits(chain(m4[i], n4[:, j])) for i in range(4) for j in range(4)):
            failed.add("dgemm 4x4")
        r4 = np.array([[rnd() for _ in range(4)]])
        o4 = r4 @ m4.T
        if any(bits(o4[0, j]) != bits(gemv4(m4[j], r4[0])) for j in range(4)):
            failed.add("dgemv 4x4")
        w = np.array([rnd() for _ in range(3)])
        s = float(np.sum(w))
        if bits(s) != bits(((0.0 + float(w[0])) + float(w[1])) + float(w[2])):
            failed.add("np.sum")
    return {"ok": not failed, "failed": sorted(failed)}


# --- random prims and cameras ---------------------------------------------------------------------------------------

CUBE = "\n".join(["v 0 0 0", "v 1 0 0", "v 1 1 0", "v 0 1 0", "v 0 0 1", "v 1 0 1", "v 1 1 1", "v 0 1 1",
                  "f 1 4 3 2", "f 5 6 7 8", "f 1 2 6 5", "f 2 3 7 6", "f 3 4 8 7", "f 4 1 5 8"])


def glb(positions, indices, translation=(5, 0, 0), extra_nodes=False, rotation=None, scale=None, patch=None) -> bytes:
    """A binary glTF (what VRM files are) of one mesh (patch(doc) may change its JSON before it is packed)."""
    import numpy as np

    pos = np.array(positions, dtype="<f4").tobytes()
    idx = np.array(indices, dtype="<u2").tobytes()
    blob = pos + idx
    blob += b"\0" * (-len(blob) % 4)
    node = {"mesh": 0, "translation": list(translation)}
    if rotation is not None:
        node["rotation"] = list(rotation)
    if scale is not None:
        node["scale"] = list(scale)
    nodes = [node]
    if extra_nodes:
        nodes = [{"children": [1], "scale": [2, 1, 0.5]}, node]
    doc = {"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}], "nodes": nodes,
           "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}],
           "buffers": [{"byteLength": len(blob)}],
           "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": len(pos)},
                           {"buffer": 0, "byteOffset": len(pos), "byteLength": len(idx)}],
           "accessors": [{"bufferView": 0, "componentType": 5126, "count": len(positions), "type": "VEC3"},
                         {"bufferView": 1, "componentType": 5123, "count": len(indices), "type": "SCALAR"}]}
    if patch is not None:
        patch(doc)
    text = json.dumps(doc).encode("utf-8")
    text += b" " * (-len(text) % 4)
    body = struct.pack("<II", len(text), 0x4E4F534A) + text + struct.pack("<II", len(blob), 0x004E4942) + blob
    return struct.pack("<III", 0x46546C67, 2, 12 + len(body)) + body


TETRA = ([[0, 0, 0], [1, 0, 0], [0, 1, 0], [0, 0, 1]], [0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3])


def rand_obj(rng) -> str:
    """A small OBJ: a box, a pyramid or random triangles; sometimes with normals, textures and slashes."""
    kind = rng.choice(["cube", "pyramid", "soup", "quads"])
    if kind == "cube":
        return CUBE
    lines = ["# made by the test", "o thing"]
    if kind == "pyramid":
        pts = [(0, 0, 0), (2, 0, 0), (2, 0, 2), (0, 0, 2), (1, rng.uniform(1, 3), 1)]
        for p in pts:
            lines.append("v %r %r %r" % p)
        lines += ["vn 0 1 0", "vt 0 0", "f 1/1/1 2/1/1 3/1/1 4/1/1", "f 1//1 2//1 5//1", "f 2 3 5", "f 3 4 5", "f 4 1 5"]
        return "\n".join(lines)
    n = rng.randrange(4, 30)
    for _ in range(n):
        lines.append("v %r %r %r" % (rng.uniform(-5, 5), rng.uniform(-5, 5), rng.uniform(-5, 5)))
    for _ in range(rng.randrange(1, 40)):
        size = 3 if kind == "soup" else 4
        idx = [str(rng.randrange(1, n + 1)) if rng.random() < 0.8 else str(-rng.randrange(1, n + 1)) for _ in range(size)]
        lines.append("f " + " ".join(idx))
    return "\r\n".join(lines) if rng.random() < 0.2 else "\n".join(lines)


def rand_body(rng) -> dict:
    body = {}
    if rng.random() < 0.4:
        body["sex"] = rng.choice(["male", "female", ""])
    for key, (lo, hi) in (("heads", (4.0, 10.0)), ("shoulders", (0.6, 1.5)), ("hips", (0.6, 1.6)), ("build", (0.5, 1.8)),
                          ("legs", (0.6, 1.5))):
        if rng.random() < 0.4:
            body[key] = round(rng.uniform(lo, hi), rng.choice([1, 2, 4]))
    return body


def rand_joints(rng, names, axes) -> dict:
    out = {}
    for name in names:
        if rng.random() < 0.45:
            out[name] = {a: round(rng.uniform(-2.6, 2.6), 4) for a in axes if rng.random() < 0.6}
    return out


def rand_hands(rng) -> dict:
    from genko import mesh3d

    out = {}
    for side in ("l", "r"):
        r = rng.random()
        if r < 0.3:
            out[side] = rng.choice(mesh3d.HAND_POSES)
        elif r < 0.5:
            out[side] = {"pose": rng.choice(mesh3d.HAND_POSES), "curls": [round(rng.random(), 3) for _ in range(5)]}
        elif r < 0.6:
            out[side] = {"pose": rng.choice(mesh3d.HAND_POSES), "curls": {f: round(rng.random(), 3) for f in mesh3d.FINGERS if rng.random() < 0.5}}
    return out


def rand_camera(rng):
    if rng.random() < 0.35:
        return None
    cam = {}
    for key, span in (("turn", 1.6), ("tip", 0.9), ("roll", 0.6)):
        if rng.random() < 0.85:
            cam[key] = round(rng.uniform(-span, span), 4)
    if rng.random() < 0.7:
        cam["focal_mm"] = rng.choice([20.0, 60.0, 400.0, 1500.0, 5000.0, round(rng.uniform(20, 2000), 1)])
    if rng.random() < 0.85:
        cam["target"] = [round(rng.uniform(0, 250), 3), round(rng.uniform(0, 350), 3)]
    return cam or {"turn": 0.3}


def rand_rot(rng):
    r = rng.random()
    if r < 0.15:
        return None
    if r < 0.25:
        return [0, 0, 0]
    return [round(rng.uniform(-3.2, 3.2), 4) for _ in range(3)]


def rand_prim(rng) -> dict:
    from genko import mannequin, mesh3d, prim3d

    kind = rng.choice(["figure"] * 6 + ["head", "head", "hand", "hand", "mesh", "mesh", "box", "box", "cube", "cylinder",
                                        "stairs", "floor", "sphere", "cone", "prop", "scene", "mannequin", "mannequin"])
    pos = [round(rng.uniform(10, 240), 3), round(rng.uniform(20, 340), 3), rng.choice([0, 0.0, round(rng.uniform(-150, 400), 2)])]
    prim = {"id": "p%04d" % rng.randrange(10000), "kind": kind}
    if rng.random() < 0.9:
        prim["pos"] = pos
    rot = rand_rot(rng)
    if rot is not None:
        prim["rot"] = rot
    if rng.random() < 0.3:
        prim["focal_mm"] = rng.choice([20.0, 120.0, 400.0, 900.0])
    if kind == "figure":
        h = rng.choice([30.0, 60.0, 90.0, 120.0, 180.0, round(rng.uniform(20, 250), 2)])
        prim["size"] = [h / 2, h, h / 4]
        prim["body"] = rand_body(rng)
        prim["joints"] = rand_joints(rng, mesh3d.FIGURE_JOINTS, ("x", "y", "z"))
        prim["hands"] = rand_hands(rng)
    elif kind in ("head", "hand"):
        s = rng.choice([12.0, 25.0, 30.0, round(rng.uniform(5, 80), 2)])
        prim["size"] = [s, s, s]
        if kind == "hand":
            prim["side"] = rng.choice(["l", "r", "r"])
            prim["pose"] = rng.choice(list(mesh3d.HAND_POSES))
            if rng.random() < 0.4:
                prim["curls"] = [round(rng.random(), 3) for _ in range(5)]
    elif kind == "mesh":
        if rng.random() < 0.7:
            mesh = mesh3d.read_obj(rand_obj(rng))
        else:
            pts, idx = TETRA
            mesh = mesh3d.read_gltf(glb(pts, idx, extra_nodes=rng.random() < 0.5))
        ratio = mesh.pop("ratio")
        s = rng.choice([40.0, 60.0, round(rng.uniform(10, 120), 2)])
        prim.update(size=[s, s, s] if rng.random() < 0.8 else [s, s * 0.5], mesh=mesh, title="モデル", ratio=ratio)
    elif kind == "mannequin":
        h = rng.choice([60.0, 80.0, 120.0, round(rng.uniform(20, 200), 2)])
        prim["size"] = [h / 2, h, h / 4]
        prim["joints"] = mannequin.default_joints()
        if rng.random() < 0.5:
            mannequin.apply_preset(prim, rng.choice(list(mannequin.PRESETS)))
        for name, values in rand_joints(rng, mannequin.JOINTS, ("yaw", "pitch")).items():
            prim["joints"][name] = {**prim["joints"][name], **values}
    elif kind == "scene":
        scene = rng.choice(list(prim3d.SCENES))
        prim["scene"] = scene
        prim["size"] = [float(v) * rng.choice([0.5, 1.0, 1.3]) for v in prim3d.SCENE_SIZES[scene]]
        prim["focal_mm"] = rng.choice([120.0, 220.0, 400.0])
    else:
        r = rng.random()
        if r < 0.5:
            prim["size"] = [round(rng.uniform(5, 120), 2) for _ in range(3)]
        elif r < 0.7:
            prim["size"] = round(rng.uniform(5, 120), 2)
        elif r < 0.8:
            prim["size"] = [round(rng.uniform(5, 120), 2)]
        if kind == "prop":
            prim["prop"] = rng.choice(list(prim3d.PROPS) + ["spaceship"])
        if kind == "stairs":
            prim["steps"] = rng.choice([2, 6, 11, 30])
        if kind == "floor":
            prim["lines"] = rng.choice([2, 8, 15, 40])
    return prim


def geometry_case(rng) -> dict:
    """One prim and camera, and every value the C++ geometry must give the same bits for."""
    import numpy as np

    from genko import mannequin, mesh3d, persp3d, prim3d, threeops

    prim = rand_prim(rng)
    camera = rand_camera(rng)
    out = {"prim": prim, "camera": camera}
    kind = prim["kind"]
    seen_with = {**prim, "camera": camera} if camera else prim

    def attempt(name, fn):
        try:
            out[name] = fn()
        except Exception as exc:  # (the C++ build must refuse the same: any error)
            out[name] = {"error": type(exc).__name__, "message": str(exc)}

    if kind == "figure":
        def skeleton():
            sk = mesh3d.figure_skeleton(prim)
            return {"points": [[k, [bits(c) for c in v]] for k, v in sk["points"].items()],
                    "turns": [[k, [bits(c) for c in np.asarray(v).flatten()]] for k, v in sk["turns"].items()],
                    "unit": bits(sk["unit"])}
        attempt("skeleton", skeleton)
        attempt("handles", lambda: [[n, bits(x), bits(y)] for n, (x, y) in threeops.figure_handles(prim, camera)])
        handle = rng.choice(["pelvis", *threeops.HANDLES])
        target = [round(rng.uniform(0, 250), 3), round(rng.uniform(0, 350), 3)]
        attempt("drag", lambda: {"handle": handle, "to": target, "change": threeops.drag_joint(prim, handle, target, camera)})
        if rng.random() < 0.4:
            ik = rng.choice(list(threeops.IK_CHAINS))
            attempt("reach", lambda: {"handle": ik, "to": target, "change": threeops.reach(prim, ik, target, camera)})
    if kind in mesh3d.MESH_KINDS or kind in prim3d.KINDS:
        def mesh():
            (verts, faces, parts), marks = mesh3d._mesh_of(prim)
            return {"n": len(verts), "v": sha_f64(np.asarray(verts).reshape(-1)), "f": sha_text(json.dumps([list(f) for f in faces])),
                    "parts": sha_text(json.dumps(list(parts))), "marks": sha_f64([c for line in marks for p in line for c in p]),
                    "faces": len(faces)}
        attempt("mesh", mesh)

        def seen():
            pts, depth, faces, parts, guides = mesh3d.seen(prim, camera)
            return {"pts": sha_f64(np.asarray(pts).reshape(-1)), "depth": sha_f64(depth),
                    "guides": sha_f64([c for p, dz in guides for c in list(np.asarray(p).reshape(-1)) + list(dz)])}
        attempt("seen", seen)

        def normals():
            (verts, faces, _p), _m = mesh3d._mesh_of(prim)
            if not len(verts):
                return {"n": 0}
            n = mesh3d._normals(mesh3d._turned(prim, verts, camera), faces)
            return {"n": len(n), "sha": sha_f64(np.asarray(n).reshape(-1))}
        attempt("normals", normals)

        def raster():
            x, y, w, h = prim3d.prim_bbox(seen_with) if kind in mesh3d.MESH_KINDS else (0.0, 0.0, 120.0, 160.0)
            dpi = rng.choice([12, 25, 40])
            size = (max(1, min(400, int(w / 25.4 * dpi) + 3)), max(1, min(400, int(h / 25.4 * dpi) + 3)))
            light = rng.choice([None, [1, 0, -0.2], [-0.3, 0.8, 0.5], [0, 0, 0]])
            ambient = rng.choice([0.35, 0.0, 0.8])
            shade, zbuf, alpha = mesh3d.raster([prim], size, dpi, camera, light, ambient, box=(float(x), float(y)))
            return {"size": list(size), "dpi": dpi, "light": light, "ambient": ambient, "box": [bits(x), bits(y)],
                    "zbuf": sha_f32(zbuf.reshape(-1)), "shade": sha_f32(shade.reshape(-1)), "alpha": int(alpha.sum())}
        attempt("raster", raster)
    if kind in mesh3d.MESH_KINDS:
        dpi = rng.choice([16.0, 40.0])
        attempt("lines", lambda: {"dpi": dpi, **lines_digest(mesh3d.lines([prim], camera, dpi))})
    attempt("edges", lambda: edges_digest(prim3d.edges(seen_with)))
    attempt("trace", lambda: lines_digest(prim3d.trace(seen_with)))
    attempt("joined", lambda: lines_digest(prim3d.join_lines(prim3d.trace(seen_with))))
    attempt("bbox", lambda: [bits(v) for v in prim3d.prim_bbox(seen_with)])
    if kind not in mesh3d.MESH_KINDS and kind != "mannequin":
        attempt("box", lambda: [bits(v) for v in prim3d.bbox(seen_with)])
    if kind == "mannequin":
        def bone():
            b = mannequin.skeleton(prim)
            return {"segments": [[bits(a[0]), bits(a[1]), bits(c[0]), bits(c[1]), part] for a, c, part in b["segments"]],
                    "head": [bits(b["head"][0][0]), bits(b["head"][0][1]), bits(b["head"][1])], "facing": b["facing"],
                    "bbox": [bits(v) for v in b["bbox"]]}
        attempt("bone", bone)
        handle = rng.choice(["pelvis", *mannequin.HANDLES])
        target = [round(rng.uniform(0, 250), 3), round(rng.uniform(0, 350), 3)]
        attempt("pose_to", lambda: {"handle": handle, "to": target, "change": mannequin.pose_to(prim, handle, target)})
    else:
        attempt("vanishing", lambda: persp3d.vanishing_points(prim, camera))
        attempt("ruler", lambda: persp3d.ruler_from(prim, camera))
        if rng.random() < 0.15:
            points = [[round(rng.uniform(-300, 500), 2), round(rng.uniform(-200, 600), 2)] for _ in range(rng.choice([1, 2, 3]))]
            base = camera or {"target": [91.0, 128.5]}
            attempt("camera_for", lambda: {"points": points, "camera": persp3d.camera_for({"points": points}, prim, base)})
    return out


def geometry(out: str, seed: int, count: int) -> None:
    from genko import mesh3d

    rng = random.Random(seed)
    cases = []
    for _ in range(count):
        mesh3d._figure_cached.cache_clear()
        mesh3d._lines_cached.cache_clear()
        cases.append(geometry_case(rng))
    Path(out).write_text(dumps(cases), encoding="utf-8")


# --- model files ----------------------------------------------------------------------------------------------------


def model_case(rng) -> dict:
    """A model file (OBJ text, GLB bytes or glTF text) and what read_obj / read_gltf make of it (or the error)."""
    from genko import mesh3d

    r = rng.random()
    if r < 0.45:
        kind = "obj"
        text = rand_obj(rng)
        if rng.random() < 0.25:  # (broken ones)
            text = rng.choice(["v 0 0 0\nv 1 0 0", "v 0 0 0\nf 1 2 3", "v 1 2\nf 1 1 1", "v a b c\nf 1 2 3", "f 1 2 3",
                               "v 0 0 0\nv 1 1 1\nv 2 0 0\nf 1 2 x", "v 0 0 0\nv 1 1 1\nv 2 0 0\nf 1 2 0",
                               "v 0 0 0　\nv 1 1 1\nv 2 0 0\nf\t1 2 3", "v 1_0 2 3\nv 1 1 1\nv 2 0 0\nf 1 2 3 f 3 2 1"])
        data = text
    elif r < 0.85:
        kind = "glb"
        pts, idx = TETRA
        if rng.random() < 0.5:
            pts = [[round(rng.uniform(-3, 3), 3) for _ in range(3)] for _ in range(rng.randrange(3, 12))]
            idx = [rng.randrange(len(pts)) for _ in range(3 * rng.randrange(1, 10))]
        raw = glb(pts, idx, translation=[rng.uniform(-5, 5) for _ in range(3)], extra_nodes=rng.random() < 0.4,
                  rotation=[0, 0.7071068, 0, 0.7071068] if rng.random() < 0.3 else None,
                  scale=[1, 2, rng.choice([1, 0.5])] if rng.random() < 0.3 else None)
        if rng.random() < 0.2:
            raw = rng.choice([b"glTF" + b"\0" * 8, raw[:30], raw[:-8], b"glTF" + raw[4:12] + b"\0" * 20])
        data = base64.b64encode(raw).decode("ascii")
    else:
        kind = "gltf"
        pts, idx = TETRA
        raw = glb(pts, idx)
        # the same model as a .gltf with its buffer inside
        length = struct.unpack_from("<I", raw, 12)[0]
        doc = json.loads(raw[20:20 + length].decode("utf-8"))
        blob = raw[20 + length + 8:]
        doc["buffers"] = [{"byteLength": len(blob), "uri": "data:application/octet-stream;base64," + base64.b64encode(blob).decode("ascii")}]
        text = json.dumps(doc)
        if rng.random() < 0.3:
            text = rng.choice(['{"buffers": [{"uri": "model.bin"}]}', "not json", "[1, 2]", '{"buffers": [{"uri": "data:nocomma"}]}',
                               '{"nodes": [], "buffers": []}'])
        data = text
    try:
        if kind == "obj":
            result = {"mesh": mesh3d.read_obj(data)}
        elif kind == "glb":
            result = {"mesh": mesh3d.read_gltf(base64.b64decode(data))}
        else:
            result = {"mesh": mesh3d.read_gltf(data.encode("utf-8"))}
    except mesh3d.ObjError as exc:
        result = {"error": "ObjError", "message": str(exc)}
    except (ValueError, IndexError, KeyError, TypeError) as exc:
        result = {"error": type(exc).__name__, "message": str(exc)}
    except Exception as exc:  # (Python fails harder: the C++ build must refuse it)
        result = {"error": type(exc).__name__, "message": str(exc), "crash": True}
    return {"kind": kind, "data": data, **result}


def models(out: str, seed: int, count: int) -> None:
    rng = random.Random(seed)
    Path(out).write_text(dumps([model_case(rng) for _ in range(count)]), encoding="utf-8")


# --- books ----------------------------------------------------------------------------------------------------------


def fresh(ids: bool = True) -> None:
    import pyref_harness

    pyref_harness.fresh_process_state(ids)
    from genko import mesh3d

    mesh3d._figure_cached.cache_clear()
    mesh3d._lines_cached.cache_clear()


def _apply(episode, ops, agent="human:作者"):
    from genko.ops import apply_ops

    return apply_ops(episode, ops, agent=agent)


def fixture_book(dest: Path, strict: bool = False, locked: bool = False) -> None:
    """Page 1: three panels (one slanted), every kind of 3D, a perspective ruler, a paint layer and a locked layer;
    page 2: a camera and a light, a figure and a box; page 3: nothing yet."""
    from genko.io import save_episode
    from genko.models import Layer, LayerKind, LayerRole, PageSpec, new_episode

    fresh(True)
    ep = new_episode("3D の試験", 1, 3, PageSpec.b5_doujin())
    for page in ep.pages:
        page.numero = False
    p1 = ep.pages[0]
    root = p1.leaf_frames()[0]
    a, b = p1.split_frame(root.id, "horizontal", 0.5, 4)
    c, d = p1.split_frame(b.id, "vertical", 0.4, 3)
    from genko import frames as framemod

    pts = framemod.corners(d.rect)
    framemod.set_shape(d, [(pts[0][0] + 6, pts[0][1]), pts[1], pts[2], (pts[3][0] - 6, pts[3][1])])
    paint = Layer(id="paint", role=LayerRole.USER, kind=LayerKind.RASTER)
    pen = Layer(id="pen", role=LayerRole.USER, kind=LayerKind.STROKES)
    frozen = Layer(id="frozen", role=LayerRole.USER, kind=LayerKind.STROKES)
    frozen.locked = True
    folder = Layer(id="folder", role=LayerRole.USER, kind=LayerKind.FOLDER)
    draft = Layer(id="draft", role=LayerRole.DRAFT, kind=LayerKind.STROKES, exportable=False)
    p1.layers += [paint, pen, frozen, folder, draft]
    ep.pages[1].layers.append(Layer(id="paint2", role=LayerRole.USER, kind=LayerKind.RASTER))
    _apply(ep, [
        {"op": "add_figure", "page": 1, "id": "fig", "pos": [60, 120, 0], "height_mm": 90, "preset": "walk", "frame_id": a.id},
        {"op": "add_head", "page": 1, "id": "head", "pos": [140, 60, 0], "rot": [0.2, 0.6, 0]},
        {"op": "add_hand", "page": 1, "id": "hand", "pos": [120, 90, 0], "side": "l", "pose": "peace"},
        {"op": "import_model", "page": 1, "id": "model", "obj": CUBE, "size_mm": 30, "pos": [40, 200, 0]},
        {"op": "add_prim3d", "page": 1, "id": "box", "kind": "box", "pos": [100, 150, 0], "size": [30, 20, 25]},
        {"op": "add_prim3d", "page": 1, "id": "stairs", "kind": "stairs", "pos": [150, 200, 0], "steps": 5},
        {"op": "add_prim3d", "page": 1, "id": "chair", "kind": "prop", "prop": "chair", "pos": [60, 220, 0]},
        {"op": "add_scene", "page": 1, "id": "room", "kind": "room", "frame_id": c.id},
        {"op": "add_mannequin", "page": 1, "id": "man", "pos": [150, 160, 0], "height_mm": 70, "preset": "run"},
        {"op": "add_ruler", "page": 1, "kind": "perspective", "id": "pr", "points": [[-40, 80], [260, 90]]},
        {"op": "add_ruler", "page": 1, "kind": "perspective", "id": "pr1", "points": [[90, -150]]},
        {"op": "add_ruler", "page": 1, "kind": "perspective", "id": "pr3", "points": [[-60, 70], [250, 80], [95, 900]]},
        {"op": "add_ruler", "page": 1, "kind": "line", "id": "ln", "points": [[10, 10], [50, 50]]},
        {"op": "add_figure", "page": 2, "id": "fig2", "pos": [90, 150, 0], "height_mm": 120, "body": {"sex": "female"}},
        {"op": "add_prim3d", "page": 2, "id": "box2", "kind": "box", "pos": [60, 80, 0], "size": 40, "rot": [0, 0, 0]},
        {"op": "set_camera", "page": 2, "turn": 0.5, "tip": 0.2, "focal_mm": 350},
        {"op": "set_light", "page": 2, "dir": [1, 0.2, -0.4], "ambient": 0.25},
    ])
    if strict:
        ep.strict_gates = True
        ep.pages[1].name_ok = True
    if locked:
        ep.page_locks[ep.pages[0].id] = "ai:other"
    save_episode(ep, dest, actor="human:作者")


def fixture_models() -> dict:
    """The model files the fixed op cases import (by name: the C++ test puts them into the ops)."""
    pts, idx = TETRA
    raw = glb(pts, idx)
    length = struct.unpack_from("<I", raw, 12)[0]
    doc = json.loads(raw[20:20 + length].decode("utf-8"))
    blob = raw[20 + length + 8:]
    doc["buffers"] = [{"byteLength": len(blob), "uri": "data:application/octet-stream;base64," + base64.b64encode(blob).decode("ascii")}]
    pyramid = "\n".join(["# pyramid", "v 0 0 0", "v 2 0 0", "v 2 0 2", "v 0 0 2", "v 1 2.5 1", "vn 0 1 0", "vt 0 0",
                         "f 1/1/1 2/1/1 3/1/1 4/1/1", "f 1//1 2//1 5//1", "f 2 3 5", "f 3 4 5", "f 4 1 5"])
    negative = "\n".join(["v 0 0 0", "v 1 0 0", "v 0 1 0", "v 0 0 1", "f -4 -3 -2", "f -4 -2 -1", "f -4 -1 -3", "f -3 -1 -2"])
    many = "\n".join(["v 0 0 0", "v 1 0 0", "v 0 1 0"] + ["f 1 2 3"] * 30001)
    # what the C++ build refuses (Python takes them in): too many corners, faces of too many corners, a corner at
    # infinity, glTF faces at corners that are not there, nodes inside themselves, an accessor of a negative count
    huge = "\n".join(["v %d %d 0" % (i % 1000, i // 1000) for i in range(240001)] + ["f 1 2 3"])
    wide = "\n".join(["v %d %d 0" % (i, i * i % 7) for i in range(17)] + ["f " + " ".join(str(i) for i in range(1, 18))] * 30000)
    infinite = "\n".join(["v 1e999 0 0", "v 0 1 0", "v 0 0 1", "f 1 2 3"])
    loop = json.dumps({"asset": {"version": "2.0"}, "scenes": [{"nodes": [0]}], "nodes": [{"children": [1]}, {"children": [0]}]})

    def negative_count(doc):  # (numpy reads to the end of the buffer: what Python and the C++ build both take)
        doc["accessors"][1]["count"] = -3

    def negative_positions(doc):
        doc["accessors"][0]["count"] = -1

    def negative_odd(doc):
        doc["accessors"][1].update(count=-3, byteOffset=1)

    models = {"OBJ_HUGE": huge, "OBJ_WIDE": wide, "OBJ_INFINITE": infinite, "GLTF_LOOP": loop,
              "GLB_BAD_INDEX": base64.b64encode(glb(pts, [0, 2, 1, 0, 1, 99])).decode("ascii"),
              "GLB_NEGATIVE_COUNT": base64.b64encode(glb(pts, idx, patch=negative_count)).decode("ascii"),
              "GLB_NEGATIVE_POSITIONS": base64.b64encode(glb(pts, idx, patch=negative_positions)).decode("ascii"),
              "GLB_NEGATIVE_ODD": base64.b64encode(glb(pts, idx, patch=negative_odd)).decode("ascii")}
    return {"CUBE": CUBE, "PYRAMID": pyramid, "NEGATIVE": negative, "MANY_FACES": many, **models,
            "GLB": base64.b64encode(raw).decode("ascii"),
            "GLB_NODES": base64.b64encode(glb(pts, idx, translation=(1, 2, 3), extra_nodes=True, rotation=[0, 0.7071068, 0, 0.7071068],
                                              scale=[1, 2, 0.5])).decode("ascii"),
            "GLB_SHORT": base64.b64encode(b"glTF" + b"\0" * 8).decode("ascii"),
            "GLTF": json.dumps(doc), "GLTF_EXTERNAL": json.dumps({"buffers": [{"uri": "model.bin"}]})}


def fixtures(out: str) -> None:
    root = Path(out)
    root.mkdir(parents=True, exist_ok=True)
    fixture_book(root / "a.genko")
    fixture_book(root / "strict.genko", strict=True)
    fixture_book(root / "locked.genko", locked=True)
    (root / "models.json").write_text(dumps(fixture_models()), encoding="utf-8")


def rand_ops_for_book(rng, page_count: int, render: bool) -> list:
    """add_* ops that put random 3D on the pages (with explicit ids)."""
    from genko import mannequin, mesh3d, prim3d

    ops = []
    n = 0
    for page in range(1, page_count + 1):
        for _ in range(rng.randrange(0, 5)):
            n += 1
            pid = f"q{page}{n:02d}"
            pos = [round(rng.uniform(5, 170), 2), round(rng.uniform(10, 250), 2), rng.choice([0, 0, round(rng.uniform(-60, 200), 1)])]
            kind = rng.choice(["figure", "figure", "head", "hand", "model", "prim", "prim", "scene", "mannequin"])
            if kind == "figure":
                op = {"op": "add_figure", "page": page, "id": pid, "pos": pos, "height_mm": rng.choice([40, 90, 150]),
                      "rot": [round(rng.uniform(-1, 1), 3) for _ in range(3)] if rng.random() < 0.6 else None}
                if rng.random() < 0.5:
                    op["preset"] = rng.choice(list(mesh3d.FIGURE_PRESETS))
                if rng.random() < 0.5:
                    op["body"] = rand_body(rng)
                if rng.random() < 0.4:
                    op["joints"] = rand_joints(rng, mesh3d.FIGURE_JOINTS, ("x", "y", "z"))
                if rng.random() < 0.4:
                    op["hands"] = rand_hands(rng)
                op = {k: v for k, v in op.items() if v is not None}
            elif kind in ("head", "hand"):
                op = {"op": f"add_{kind}", "page": page, "id": pid, "pos": pos, "size_mm": rng.choice([15, 30, 45]),
                      "rot": [round(rng.uniform(-2, 2), 3) for _ in range(3)]}
                if kind == "hand":
                    op.update(side=rng.choice(["l", "r"]), pose=rng.choice(list(mesh3d.HAND_POSES)))
            elif kind == "model":
                op = {"op": "import_model", "page": page, "id": pid, "pos": pos, "size_mm": rng.choice([20, 45])}
                if rng.random() < 0.6:
                    op["obj"] = rand_obj(rng)
                else:
                    op["glb"] = base64.b64encode(glb(*TETRA, extra_nodes=rng.random() < 0.5)).decode("ascii")
            elif kind == "prim":
                op = {"op": "add_prim3d", "page": page, "id": pid, "pos": pos,
                      "kind": rng.choice(["box", "cylinder", "stairs", "floor", "sphere", "cone", "prop"]),
                      "size": [round(rng.uniform(10, 60), 1) for _ in range(3)] if rng.random() < 0.7 else rng.choice([25, 40.5])}
                if op["kind"] == "prop":
                    op["prop"] = rng.choice(list(prim3d.PROPS))
                if rng.random() < 0.5:
                    op["rot"] = [round(rng.uniform(-1.5, 1.5), 3) for _ in range(3)]
                if rng.random() < 0.3:
                    op["focal_mm"] = rng.choice([150, 400, 1000])
            elif kind == "scene":
                op = {"op": "add_scene", "page": page, "id": pid, "kind": rng.choice(list(prim3d.SCENES)),
                      "size": rng.choice([None, 120, [150, 80, 200]])}
                if rng.random() < 0.5:
                    op["frame_id"] = False
                op = {k: v for k, v in op.items() if v is not None}
            else:
                op = {"op": "add_mannequin", "page": page, "id": pid, "pos": pos, "height_mm": rng.choice([60, 80, 110])}
                if rng.random() < 0.6:
                    op["preset"] = rng.choice(list(mannequin.PRESETS))
                if rng.random() < 0.4:
                    op["rot"] = [round(rng.uniform(-1, 1), 3), round(rng.uniform(-3, 3), 3), round(rng.uniform(-0.5, 0.5), 3)]
            ops.append(op)
        if rng.random() < 0.4:
            ops.append({"op": "set_camera", "page": page, "turn": round(rng.uniform(-1, 1), 3), "tip": round(rng.uniform(-0.5, 0.5), 3),
                        "focal_mm": rng.choice([60, 250, 900])})
        if rng.random() < 0.3:
            ops.append({"op": "set_light", "page": page, "dir": [round(rng.uniform(-1, 1), 2) for _ in range(3)],
                        "ambient": round(rng.uniform(0, 0.8), 2)})
        if rng.random() < 0.3:
            ops.append({"op": "add_ruler", "page": page, "kind": "perspective", "id": f"r{page}",
                        "points": [[round(rng.uniform(-200, 300), 1), round(rng.uniform(-100, 300), 1)] for _ in range(rng.choice([1, 2, 3]))]})
    return ops


def make_book(rng, dest: Path, render: bool) -> None:
    from genko import frames as framemod
    from genko.io import save_episode
    from genko.models import Layer, LayerKind, LayerRole, PageSpec, new_episode

    fresh(True)
    spec = rng.choice([PageSpec.b5_doujin, PageSpec.a5_doujin,
                       lambda: PageSpec.custom(120, 170, 110, 160, 3, 8, 8, 7, 6)])()
    ep = new_episode("3D の本", 1, rng.randint(1, 3), spec)
    for page in ep.pages:
        page.numero = False
        root = page.leaf_frames()[0]
        if rng.random() < 0.7:
            a, b = page.split_frame(root.id, rng.choice(["horizontal", "vertical"]), rng.choice([0.4, 0.5]), 4)
            if rng.random() < 0.4:
                pts = framemod.corners(b.rect)
                framemod.set_shape(b, [(pts[0][0] + 5, pts[0][1]), pts[1], pts[2], (pts[3][0] - 5, pts[3][1])])
            if rng.random() < 0.3:
                a.corner_mm = 3.0
        if rng.random() < 0.6:
            page.layers.append(Layer(id=f"paint{page.index}", role=LayerRole.USER, kind=LayerKind.RASTER))
        if rng.random() < 0.4:
            page.layers.append(Layer(id=f"pen{page.index}", role=LayerRole.USER, kind=LayerKind.STROKES))
        page.name_ok = rng.random() < 0.5
    ops = rand_ops_for_book(rng, len(ep.pages), render)
    if ops:
        _apply(ep, ops)
    # frames for some prims: the panel under them
    for page in ep.pages:
        leaves = page.leaf_frames()
        for prim in page.prims:
            if leaves and rng.random() < 0.25:
                prim["frame_id"] = rng.choice(leaves).id
    ep.strict_gates = rng.random() < 0.2
    if rng.random() < 0.15 and len(ep.pages) > 1:
        ep.page_locks[ep.pages[-1].id] = rng.choice(["ai:other", "human:作者"])
    save_episode(ep, dest, actor="human:作者")


def make_books(out: str, seed: int, count: int, render: bool) -> None:
    root = Path(out)
    root.mkdir(parents=True, exist_ok=True)
    for i in range(count):
        make_book(random.Random(seed * 1000 + i), root / f"book-{i:02d}.genko", render)


# --- applying ops -----------------------------------------------------------------------------------------------------


def _png_pixels_sha(data: bytes | None) -> dict | None:
    if not data:
        return None
    from PIL import Image

    im = Image.open(io.BytesIO(data))
    im.load()
    return {"mode": im.mode, "size": list(im.size), "sha": hashlib.sha256(im.convert("RGBA").tobytes()).hexdigest()}


def state_of(episode) -> list:
    """What the 3D ops change on each page: its prims, rulers and extra keys (the camera, the light), and each layer's
    strokes (the canonical bytes the book would keep), raster (its pixels) and screen."""
    from genko.models import stroke_to_packed

    pages = []
    for page in episode.pages:
        layers = []
        for layer in page.layers:
            blob = json.dumps([stroke_to_packed(s) for s in layer.strokes], sort_keys=True, separators=(",", ":"),
                              ensure_ascii=False) if layer.strokes else None
            layers.append({"id": layer.id, "role": layer.role.value, "kind": layer.kind.value,
                           "strokes": hashlib.sha256(blob.encode("utf-8")).hexdigest() if blob else None,
                           "stroke_count": len(layer.strokes), "raster": _png_pixels_sha(layer.raster_png),
                           "screen": layer.screen})
        pages.append({"index": page.index, "prims": page.prims, "rulers": page.rulers, "extra": page.extra, "layers": layers})
    return pages


def probe(episode) -> dict:
    """What a book Python has changed can still do (for the inputs the C++ build refuses): be drawn (every page in
    proof at 30 dpi), be saved and read back; and whether its lines hold numbers that are not finite, how big its
    models are."""
    import tempfile

    from genko import render
    from genko.io import load_episode, save_episode

    from genko import rulers

    out = {"render": None, "reload": None, "rulers": None, "strokes_nonfinite": False, "mesh_vertices": 0, "mesh_corners": 0}
    for page in episode.pages:
        for ruler in page.rulers:
            try:
                rulers.validate(ruler)  # (what add_ruler and edit_ruler hold every ruler to)
            except Exception as exc:
                out["rulers"] = out["rulers"] or f"{type(exc).__name__}: {exc}"
    try:
        for page in episode.pages:
            render._STROKE_CACHE.clear()
            render.render_page(page, 30, mode="proof", episode=episode)
    except Exception as exc:  # (what the drawing stops with)
        out["render"] = f"{type(exc).__name__}: {str(exc)[:160]}"
    try:
        with tempfile.TemporaryDirectory() as tmp:
            save_episode(copy.deepcopy(episode), Path(tmp) / "b.genko")
            load_episode(Path(tmp) / "b.genko")
    except Exception as exc:
        out["reload"] = f"{type(exc).__name__}: {str(exc)[:160]}"
    for page in episode.pages:
        for layer in page.layers:
            for stroke in layer.strokes:
                if any(not math.isfinite(float(c)) for p in stroke.points for c in p) or not math.isfinite(float(stroke.width_mm)):
                    out["strokes_nonfinite"] = True
        for prim in page.prims:
            mesh = prim.get("mesh") if isinstance(prim, dict) else None
            if isinstance(mesh, dict):
                out["mesh_vertices"] = max(out["mesh_vertices"], len(mesh.get("v") or []) // 3)
                out["mesh_corners"] = max(out["mesh_corners"], sum(len(f) for f in mesh.get("f") or []))
    return out


def apply_jobs(jobs_path: str) -> None:
    from genko.headless import snapshot
    from genko.io import load_episode, save_episode
    from genko.ops import ApplyError, apply_ops

    jobs = json.loads(Path(jobs_path).read_text(encoding="utf-8"))
    for job in jobs:
        fresh(bool(job.get("ids", True)))
        episode = load_episode(Path(job["book"]))
        steps = []
        ok_all = True
        for ops in job["steps"]:
            entry = {}
            try:
                entry["reply"] = apply_ops(episode, ops, dry_run=bool(job.get("dry_run")), agent=job.get("agent", "genko"))
            except ApplyError as exc:
                entry["reply"] = {"ok": False, "error": str(exc)}
                ok_all = False
            except Exception as exc:  # (Python fails harder: the C++ build must refuse the op)
                entry["reply"] = {"ok": False, "error": str(exc), "crash": type(exc).__name__}
                ok_all = False
            before = [len(p.layers) for p in episode.pages]
            entry["full"] = snapshot(episode, full=True)
            assert before == [len(p.layers) for p in episode.pages], "snapshot(full=True) made a layer"
            state = state_of(episode)
            text = json.dumps(state, ensure_ascii=False)
            entry["nonfinite"] = "NaN" in text or "Infinity" in text
            entry["state"] = json.loads(text.replace("-Infinity", "null").replace("Infinity", "null").replace("NaN", "null"))
            if job.get("probe"):
                entry["probe"] = probe(episode)
            steps.append(entry)
        Path(job["out"]).write_text(dumps({"steps": steps}), encoding="utf-8")
        if job.get("dest") and not job.get("dry_run"):
            save_episode(episode, Path(job["dest"]), actor=job.get("agent", "genko"))


# --- the pose library -------------------------------------------------------------------------------------------------


POSE_STEPS = [
    ["save", "立ち", {"joints": {"l_arm": {"z": 0.5}, "head": {"y": -0.25, "x": 0.1}}, "hands": {"l": "fist"}}],
    ["save", " 走り　", {"joints": {"r_knee": {"x": 1.2}}, "hands": {"r": {"pose": "open", "curls": [0, 0.5, 1, 1, 0.25]}}}],
    ["save", "立ち", {"joints": {"spine": {"x": 0.3}}, "body": {"heads": 6}, "size": [45, 90, 22.5]}],
    ["save", "  ", {"joints": {}}],
    ["save", "空", {}],
    ["delete", "走り"],
    ["delete", "ない"],
    ["save", "座り", {"joints": {"l_hip": {"x": -1.4}, "r_hip": {"x": -1.4}}, "hands": {}}],
    ["find", "座り"],
    ["find", "走り"],
]


def pose_library(config: str) -> None:
    """POSE_STEPS through genko.poses with GENKO_CONFIG_DIR=config: what each returns (or raises), the poses at the end;
    CONFIG/poses.json is left as Python wrote it."""
    import os

    os.environ["GENKO_CONFIG_DIR"] = config
    from genko import poses

    results = []
    for step in POSE_STEPS:
        try:
            if step[0] == "save":
                results.append(poses.save_pose(step[1], step[2]))
            elif step[0] == "delete":
                results.append(poses.delete_pose(step[1]))
            else:
                results.append(poses.find(step[1]))
        except ValueError as exc:
            results.append({"error": str(exc)})
    sys.stdout.write(dumps({"steps": POSE_STEPS, "results": results, "poses": poses.user_poses()}) + "\n")


# --- random op sequences ------------------------------------------------------------------------------------------


def random_op(rng, book: dict) -> dict:
    """One random op (mostly the 3D ops, a few of M1's book ops) for a book described by its pages' prim ids, layer ids
    and ruler ids. Some are meant to fail (unknown ids, bad values)."""
    from genko import mannequin, mesh3d, prim3d

    pages = book["pages"]
    page = rng.choice(pages)
    index = page["index"] if rng.random() < 0.95 else 9
    prims = page["prims"] or ["nothing"]
    figures = page["figures"] or prims
    mannequins = page["mannequins"] or prims
    layers = page["layers"] or ["nothing"]
    pos = [round(rng.uniform(0, 180), 2), round(rng.uniform(0, 260), 2), rng.choice([0, round(rng.uniform(-50, 150), 1)])]
    new = "n%04d" % rng.randrange(10000)
    r = rng.random()
    name = rng.choice(["add_figure", "pose_figure", "pose_figure", "add_head", "add_hand", "import_model", "set_camera",
                       "set_light", "render_prims", "add_mannequin", "pose_mannequin", "pose_mannequin", "add_prim3d",
                       "add_scene", "edit_prim", "delete_prim", "trace_prims", "ruler_from_3d", "camera_from_ruler",
                       "set_note", "name_ok", "set_meta", "lock_page", "add_page"])
    if name == "add_figure":
        op = {"op": name, "page": index, "pos": pos, "height_mm": rng.choice([90, 50, 300, 9, 401, "120"])}
        if rng.random() < 0.7:
            op["id"] = new if rng.random() < 0.9 else prims[0]
        for key, make in (("preset", lambda: rng.choice(list(mesh3d.FIGURE_PRESETS) + ["dance"])),
                          ("body", lambda: rand_body(rng) if rng.random() < 0.9 else {"heads": 20}),
                          ("joints", lambda: rand_joints(rng, mesh3d.FIGURE_JOINTS, ("x", "y", "z")) if rng.random() < 0.9 else {"tail": {"x": 1}}),
                          ("hands", lambda: rand_hands(rng) if rng.random() < 0.9 else {"r": "wave"}),
                          ("rot", lambda: [round(rng.uniform(-2, 2), 3) for _ in range(3)])):
            if rng.random() < 0.35:
                op[key] = make()
        return op
    if name == "pose_figure":
        op = {"op": name, "page": index, "id": rng.choice(figures)}
        choice = rng.random()
        if choice < 0.2:
            op["joints"] = rand_joints(rng, mesh3d.FIGURE_JOINTS, ("x", "y", "z"))
        elif choice < 0.3:
            op["set_joints"] = rand_joints(rng, mesh3d.FIGURE_JOINTS, ("x", "y", "z"))
        elif choice < 0.4:
            op["preset"] = rng.choice(list(mesh3d.FIGURE_PRESETS))
        elif choice < 0.5:
            op["body"] = rand_body(rng) or {"sex": "female"}
        elif choice < 0.6:
            op["hands"] = rand_hands(rng) or {"l": "fist"}
        elif choice < 0.7:
            op["pose"] = rng.choice(list(mesh3d.HAND_POSES) + ["wave"])
            op["curls"] = [round(rng.random(), 2) for _ in range(5)] if rng.random() < 0.5 else None
            op = {k: v for k, v in op.items() if v is not None}
        elif choice < 0.85:
            from genko import threeops

            handles = ["pelvis", *threeops.HANDLES]
            op["drag"] = {"handle": rng.choice(handles + ["tail"]), "to": [round(rng.uniform(0, 180), 2), round(rng.uniform(0, 250), 2)]}
        else:
            from genko import threeops

            op["drag"] = {"handle": rng.choice(list(threeops.IK_CHAINS) + ["head"]), "ik": True,
                          "to": [round(rng.uniform(20, 160), 2), round(rng.uniform(30, 230), 2)]}
        if rng.random() < 0.2:
            op["height_mm"] = rng.choice([60, 140])
        if rng.random() < 0.2:
            op["rot"] = [round(rng.uniform(-1, 1), 3) for _ in range(3)]
        return op
    if name in ("add_head", "add_hand"):
        op = {"op": name, "page": index, "pos": pos if rng.random() < 0.95 else "x", "size_mm": rng.choice([20, 35, None])}
        if name == "add_hand":
            op["side"] = rng.choice(["l", "r", "x"]) if rng.random() < 0.5 else None
            op["pose"] = rng.choice(list(mesh3d.HAND_POSES) + ["wave"]) if rng.random() < 0.6 else None
        if rng.random() < 0.6:
            op["id"] = new
        return {k: v for k, v in op.items() if v is not None}
    if name == "import_model":
        op = {"op": name, "page": index, "pos": pos, "id": new}
        k = rng.random()
        if k < 0.6:
            op["obj"] = rand_obj(rng) if rng.random() < 0.85 else rng.choice(["v 0 0 0", "   ", 5])
        elif k < 0.9:
            op["glb"] = base64.b64encode(glb(*TETRA, extra_nodes=rng.random() < 0.5)).decode("ascii") if rng.random() < 0.85 else "!!!"
        else:
            op["gltf"] = '{"buffers": [{"uri": "model.bin"}]}'
        return op
    if name == "set_camera":
        op = {"op": name, "page": index}
        if rng.random() < 0.15:
            op["off"] = True
        for key in ("turn", "tip", "roll"):
            if rng.random() < 0.6:
                op[key] = round(rng.uniform(-1.2, 1.2), 5)
        if rng.random() < 0.5:
            op["focal_mm"] = rng.choice([20, 300, 5000, 10, 9000, 333.3])
        if rng.random() < 0.3:
            op["target"] = rng.choice([[90, 120], None, [5], [10.5, 20, 30]])
        return op
    if name == "set_light":
        op = {"op": name, "page": index}
        if rng.random() < 0.7:
            op["dir"] = rng.choice([[round(rng.uniform(-1, 1), 3) for _ in range(3)], [0, 0, 0], [1, 1]])
        if rng.random() < 0.6:
            op["ambient"] = rng.choice([0.2, 0.0, 1.5, -1, 0.65])
        return op
    if name in ("render_prims", "trace_prims"):
        op = {"op": name, "page": index}
        roll = rng.random()
        if roll < 0.7:
            op["layer_id"] = rng.choice(layers)
        elif roll < 0.85:
            op["layer"] = rng.choice(["ink", "name", "draft", "bg", "nope"])
        if rng.random() < 0.4:
            op["ids"] = rng.sample(prims, min(len(prims), rng.randrange(1, 3)))
        if rng.random() < 0.3:
            op["kind"] = rng.choice(["mili", "pencil", "gpen", "oil", "laser"])
        if rng.random() < 0.3:
            op["width_mm"] = rng.choice([0.2, 1, "0.5"])
        if rng.random() < 0.2:
            op["rgb"] = [10, 20, 200]
        if name == "render_prims":
            if rng.random() < 0.3:
                op["surfaces"] = False
            if rng.random() < 0.2:
                op["lines"] = False
            if rng.random() < 0.2:
                op["tone"] = rng.choice([{"lpi": 50}, {"lpi": 200}, {"pattern": "line", "angle": 30}])
            if rng.random() < 0.2:
                op["light"] = [0.5, -1, -0.3]
            if rng.random() < 0.2:
                op["ambient"] = 0.5
        return op
    if name == "add_mannequin":
        op = {"op": name, "page": index, "pos": pos, "height_mm": rng.choice([80, 100, None])}
        if rng.random() < 0.4:
            op["preset"] = rng.choice(list(mannequin.PRESETS) + ["dance"])
        if rng.random() < 0.3:
            op["rot"] = [round(rng.uniform(-1, 1), 3) for _ in range(3)]
        if rng.random() < 0.7:
            op["id"] = new
        return {k: v for k, v in op.items() if v is not None}
    if name == "pose_mannequin":
        op = {"op": name, "page": index, "id": rng.choice(mannequins)}
        c = rng.random()
        if c < 0.3:
            op["joints"] = rand_joints(rng, mannequin.JOINTS, ("yaw", "pitch"))
        elif c < 0.45:
            op["preset"] = rng.choice(list(mannequin.PRESETS))
        elif c < 0.8:
            op["drag"] = {"handle": rng.choice(["pelvis", *mannequin.HANDLES, "tail"]),
                          "to": [round(rng.uniform(0, 180), 2), round(rng.uniform(0, 250), 2)]}
        else:
            op["height_mm"] = rng.choice([50, 120])
        if rng.random() < 0.2:
            op["rot"] = [round(rng.uniform(-0.5, 0.5), 3), round(rng.uniform(-3, 3), 3), 0]
        if rng.random() < 0.2:
            op["pos"] = pos
        return op
    if name == "add_prim3d":
        op = {"op": name, "page": index, "kind": rng.choice(["box", "cylinder", "stairs", "floor", "sphere", "cone", "prop", "torus"]),
              "pos": pos}
        if rng.random() < 0.6:
            op["size"] = rng.choice([[20, 30, 40], 25, "x", [10]])
        if op["kind"] == "prop":
            op["prop"] = rng.choice(list(prim3d.PROPS) + ["spaceship"])
        if rng.random() < 0.7:
            op["id"] = new
        if rng.random() < 0.2:
            op["frame_id"] = rng.choice(page["frames"] + ["nope"])
        return op
    if name == "add_scene":
        op = {"op": name, "page": index, "kind": rng.choice(list(prim3d.SCENES) + ["castle"])}
        if rng.random() < 0.4:
            op["size"] = rng.choice([100, [150, 80, 200], "x"])
        if rng.random() < 0.3:
            op["frame_id"] = rng.choice(page["frames"] + [False, "nope"])
        if rng.random() < 0.7:
            op["id"] = new
        return op
    if name == "edit_prim":
        op = {"op": name, "page": index, "id": rng.choice(prims)}
        for key in ("pos", "size", "rot"):
            if rng.random() < 0.4:
                op[key] = [round(rng.uniform(-50, 200), 2) for _ in range(rng.choice([2, 3]))]
        if rng.random() < 0.3:
            op["focal_mm"] = rng.choice([10, 300])
        return op
    if name == "delete_prim":
        return {"op": name, "page": index, "id": rng.choice(prims + ["nothing"])}
    if name in ("ruler_from_3d", "camera_from_ruler"):
        op = {"op": name, "page": index}
        if rng.random() < 0.6:
            op["prim_id"] = rng.choice(prims)
        if name == "ruler_from_3d":
            if rng.random() < 0.5:
                op["id"] = new if rng.random() < 0.9 else (page["rulers"] or ["x"])[0]
            if rng.random() < 0.4:
                op["grid"] = rng.choice([0, 10, 60])
        else:
            op["id"] = rng.choice(page["rulers"] + ["nope"])
        return op
    if name == "set_note":
        return {"op": name, "page": index, "note": "メモ %d" % rng.randrange(100)}
    if name == "name_ok":
        return {"op": name, "page": index}
    if name == "set_meta":
        return {"op": name, "strict_gates": rng.random() < 0.5}
    if name == "lock_page":
        return {"op": name, "page": index, "agent": rng.choice(["ai:other", "human:作者"])}
    return {"op": "add_page", "count": 1}


def describe_book(path: Path) -> dict:
    from genko.io import load_episode

    fresh(True)
    ep = load_episode(path)
    pages = []
    for page in ep.pages:
        pages.append({"index": page.index,
                      "prims": [p.get("id") for p in page.prims],
                      "figures": [p.get("id") for p in page.prims if p.get("kind") in ("figure", "hand")],
                      "mannequins": [p.get("id") for p in page.prims if p.get("kind") == "mannequin"],
                      "layers": [layer.id for layer in page.layers],
                      "rulers": [r.get("id") for r in page.rulers],
                      "frames": [f.id for f in page.leaf_frames()]})
    return {"pages": pages}


def random_ops(out: str, seed: int, count: int, books_dir: str) -> None:
    rng = random.Random(seed)
    books = sorted(str(p) for p in Path(books_dir).glob("book-*.genko"))
    described = {b: describe_book(Path(b)) for b in books}
    sequences = []
    for _ in range(count):
        book = rng.choice(books)
        steps = [[random_op(rng, described[book])] for _ in range(rng.randrange(3, 9))]
        sequences.append({"book": book, "steps": steps, "agent": rng.choice(["human:作者", "ai:hermes", "genko"])})
    Path(out).write_text(dumps(sequences), encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="cmd", required=True)
    sub.add_parser("blas")
    for name in ("geometry", "models", "make-books"):
        p = sub.add_parser(name)
        p.add_argument("out")
        p.add_argument("--seed", type=int, default=1)
        p.add_argument("--count", type=int, default=50)
        p.add_argument("--render", action="store_true")
    p = sub.add_parser("fixtures")
    p.add_argument("out")
    p = sub.add_parser("poses")
    p.add_argument("config")
    p = sub.add_parser("apply")
    p.add_argument("jobs")
    p = sub.add_parser("random-ops")
    p.add_argument("out")
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--count", type=int, default=150)
    p.add_argument("--books", required=True)
    args = parser.parse_args(argv)
    if args.cmd == "blas":
        sys.stdout.write(dumps(blas_check()) + "\n")
    elif args.cmd == "geometry":
        geometry(args.out, args.seed, args.count)
    elif args.cmd == "models":
        models(args.out, args.seed, args.count)
    elif args.cmd == "make-books":
        make_books(args.out, args.seed, args.count, args.render)
    elif args.cmd == "fixtures":
        fixtures(args.out)
    elif args.cmd == "poses":
        pose_library(args.config)
    elif args.cmd == "apply":
        apply_jobs(args.jobs)
    elif args.cmd == "random-ops":
        random_ops(args.out, args.seed, args.count, args.books)
    return 0


if __name__ == "__main__":
    sys.exit(main())
