#!/opt/pyref/bin/python
"""Hermes の独立確認（M2 基本 op）: 作業者の試験とは別に、乱数の op 列を C++ の `genko apply` と
Python の `python -m genko apply` に同じ順で与え、各段の応答（ok・error 文言・applied・warnings）と、最後の原稿
（`inspect --full`）を比べる。

コンテナ内で実行（/src = 作業ツリー）: GENKO_BIN（既定 /src/build/linux-release/src/api/genko）を使う。
原稿は `pyref_harness.py make-opsbook` の v3 原稿。Python 側はそのまま、C++ 側は `genko migrate` で v4 にしてから使う。

op は docs/ops.schema.json の引数名で作り、対象（ページ・コマ id・層 id）は各段の前に Python 側の原稿を
`inspect --full` で読んで実在するものから選ぶ（一部はわざと実在しないもの）。
比べないもの: id の文字列（両側で別の乱数。出てきた順の番号に置き換えて比べる）、revision・txn・job_id・code（v4 だけ）。
C++ が "not_yet_ported" で断った段が出た op 列は、そこで比較をやめて数える（M2 の範囲外の部分）。
"""
import json, os, random, re, shutil, subprocess, sys, tempfile
from pathlib import Path

os.environ["PYTHONPATH"] = "/src/src"
os.environ["PYTHONDONTWRITEBYTECODE"] = "1"
os.environ["PYTHONHASHSEED"] = "0"
GENKO = os.environ.get("GENKO_BIN", "/src/build/linux-release/src/api/genko")
PY = "/opt/pyref/bin/python"
ID_RE = re.compile(r"^(pg_)?[0-9a-f]{12}$")
SKIP_KEYS = {"revision", "txn", "job_id", "code"}


def norm(value, ids):
    if isinstance(value, dict):
        return {k: norm(v, ids) for k, v in value.items() if k not in SKIP_KEYS}
    if isinstance(value, list):
        return [norm(v, ids) for v in value]
    if isinstance(value, str) and ID_RE.match(value):
        return ids.setdefault(value, f"<id{len(ids)}>")
    return value


def run(argv):
    r = subprocess.run(argv, capture_output=True, text=True, timeout=300, cwd="/tmp")
    lines = r.stdout.strip().splitlines()
    out = None
    if lines:
        try:
            out = json.loads(lines[-1])
        except json.JSONDecodeError:
            out = {"raw": r.stdout[-300:]}
    return r.returncode, out, r.stderr


def inspect(book, cpp):
    argv = [GENKO, "inspect", book, "--full"] if cpp else [PY, "-m", "genko", "inspect", book, "--full"]
    return run(argv)[1]


def pair_ids(a, b, mapping):
    """Python 側と C++ 側の snapshot を同じ形として並べて読み、Python の id → C++ の id の対応を作る
    （実行中に作られた層・コマの id は両側で別の乱数のため）。"""
    if isinstance(a, dict) and isinstance(b, dict):
        for k in a:
            if k in b:
                pair_ids(a[k], b[k], mapping)
    elif isinstance(a, list) and isinstance(b, list):
        for x, y in zip(a, b):
            pair_ids(x, y, mapping)
    elif isinstance(a, str) and isinstance(b, str) and ID_RE.match(a) and ID_RE.match(b):
        mapping.setdefault(a, b)


def translate(value, mapping):
    if isinstance(value, dict):
        return {k: translate(v, mapping) for k, v in value.items()}
    if isinstance(value, list):
        return [translate(v, mapping) for v in value]
    if isinstance(value, str):
        return mapping.get(value, value)
    return value


def pick(rng, real, fake, p_fake=0.15):
    if not real or rng.random() < p_fake:
        return fake
    return rng.choice(real)


def rand_ops(rng, snap):
    pages = snap.get("pages") or []
    indexes = [p["index"] for p in pages]
    page = pick(rng, indexes, rng.choice([0, len(indexes) + 1, 99]), 0.1)
    cur = next((p for p in pages if p["index"] == page), None)
    leaves = [l["id"] for l in (cur or {}).get("leaves") or []]
    layers = [l["id"] for l in (cur or {}).get("layers") or []]
    pen_layers = [l["id"] for l in (cur or {}).get("layers") or [] if l.get("kind") == "strokes"]
    n_ink = (cur or {}).get("ink_stroke_count") or 0
    pt = lambda: [round(rng.uniform(20, 170), 2), round(rng.uniform(20, 250), 2)]
    name = rng.choice(["split_frame", "cut_frame", "move_gutter", "merge_frame", "resize_frame", "set_frame", "add_frame",
                       "delete_frame", "select_frame", "add_page", "delete_page", "duplicate_page", "reorder", "advance",
                       "name_ok", "lock_page", "unlock_page", "set_note", "set_meta", "set_autosave", "add_stroke",
                       "add_stroke", "delete_stroke", "edit_stroke", "simplify_stroke", "erase", "add_layer",
                       "delete_layer", "duplicate_layer", "set_layer", "set_layers", "reorder_layers", "for_pages"])
    op = {"op": name}
    fid = pick(rng, leaves, "nosuchframe")
    lid = pick(rng, layers, "nosuchlayer")
    if name == "split_frame":
        op.update(page=page, axis=rng.choice(["horizontal", "vertical"]), ratio=rng.choice([0.3, 0.5, 0.7]),
                  gutter_mm=rng.choice([2.0, 5.0]), frame_id=fid)
        if rng.random() < 0.3:
            op["tilt_mm"] = rng.choice([3.0, -6.0])
    elif name == "cut_frame":
        op.update(page=page, frame_id=fid, p0=pt(), p1=pt())
    elif name == "move_gutter":
        op.update(page=page, frame_id=fid, delta_mm=rng.choice([-5.0, 3.0]), index=rng.choice([0, 1]))
    elif name in ("merge_frame", "delete_frame", "select_frame"):
        op.update(page=page, frame_id=fid)
        if name != "select_frame" and rng.random() < 0.5:
            op["force"] = True
    elif name == "resize_frame":
        op.update(page=page, frame_id=fid, rect={"x": 30, "y": 30, "width": rng.choice([50, 80]), "height": 60})
    elif name == "set_frame":
        op.update(page=page, frame_id=fid)
        op.update(rng.choice([{"bleed": True}, {"clip": False}, {"border_mm": rng.choice([0, 0.8])},
                              {"corner_mm": 3}, {"curves": [2, 0, -2, 0]}]))
    elif name == "add_frame":
        op.update(page=page)
        op.update(rng.choice([{"rect": [20, 20, 60, 40]}, {"points": [pt(), pt(), pt()]}]))
    elif name == "add_page":
        op.update(count=rng.choice([1, 2]))
        if rng.random() < 0.5:
            op["after"] = pick(rng, indexes, 0)
    elif name in ("delete_page", "duplicate_page", "name_ok", "unlock_page"):
        op.update(page=page)
    elif name == "reorder":
        order = indexes[:]
        rng.shuffle(order)
        op.update(order=order if rng.random() < 0.8 else order[:-1])
    elif name == "advance":
        op.update(page=page, to=rng.choice(["name", "ink", "finish"]))
    elif name == "lock_page":
        op.update(page=page, agent=rng.choice(["ai:x", "human:a"]))
    elif name == "set_note":
        op.update(page=page, note=rng.choice(["メモ", "", "二行\n目"]))
    elif name == "set_meta":
        op.update(rng.choice([{"title": "題"}, {"binding": rng.choice(["left", "right"])}, {"episode": rng.choice([3, "4"])},
                              {"start_side": rng.choice(["left", "right", None])}]))
    elif name == "set_autosave":
        op.update(enabled=rng.choice([True, False]))
    elif name == "add_stroke":
        n = rng.randint(2, 7)
        pts = [pt() + ([round(rng.uniform(0.1, 1), 3)] if rng.random() < 0.5 else []) for _ in range(n)]
        op.update(page=page, layer=rng.choice(["ink", "name"]), points=pts, width_mm=rng.choice([0.3, 0.8]),
                  kind=rng.choice(["gpen", "maru", "kabura", "mili", "pencil", "fude", "marker"]))
        if rng.random() < 0.3:
            op["taper"] = True
        if rng.random() < 0.3:
            op["stabilize"] = rng.choice([2, 5])
        if rng.random() < 0.2 and pen_layers:
            op.pop("layer")
            op["layer_id"] = rng.choice(pen_layers)
    elif name in ("delete_stroke", "edit_stroke", "simplify_stroke"):
        op.update(page=page, layer="ink", index=rng.randint(-1, max(0, n_ink)))
        if name == "edit_stroke":
            op["points"] = [pt(), pt(), pt()]
        if name == "simplify_stroke":
            op["epsilon_mm"] = rng.choice([0.2, 1.0])
    elif name == "erase":
        op.update(page=page, layer="ink", points=[pt(), pt()], width_mm=rng.choice([1.0, 4.0]),
                  mode=rng.choice(["cut", "to_crossing", "whole"]))
    elif name == "add_layer":
        op.update(page=page, name=rng.choice(["下描き", "ペン2"]), kind=rng.choice(["pen", "folder", "fill"]))
    elif name in ("delete_layer", "duplicate_layer"):
        op.update(page=page, id=lid)
    elif name == "set_layer":
        op.update(page=page, layer=lid)
        op.update(rng.choice([{"visible": False}, {"opacity": rng.choice([0.5, 1.0])}, {"locked": True},
                              {"blend": rng.choice(["multiply", "screen"])}]))
    elif name == "set_layers":
        op.update(page=page, ids=[l for l in layers if rng.random() < 0.5], visible=rng.choice([True, False]))
    elif name == "reorder_layers":
        order = layers[:]
        rng.shuffle(order)
        if rng.random() < 0.3 and order:
            order = order[:-1]  # 指定し忘れ（Python は層を消すため、C++ は断る: COMP-01a）
        op.update(page=page, order=order)
    elif name == "for_pages":
        op.update(pages=rng.choice(["body", "all", indexes[:2]]), ops=[{"op": "set_note", "note": "一括"}])
    return [op]


def main():
    n_lists = int(sys.argv[1]) if len(sys.argv) > 1 else 30
    steps = int(sys.argv[2]) if len(sys.argv) > 2 else 10
    work = Path(tempfile.mkdtemp(prefix="hermes-ops-"))
    base = work / "base.genko"
    subprocess.run([PY, "/src/tools/migration/pyref_harness.py", "make-opsbook", str(base)], check=True, capture_output=True)
    stats = {"lists": 0, "steps": 0, "same": 0, "stopped_not_yet_ported": 0, "comp01a_refusals": 0, "diff": 0, "books_same": 0}
    diffs = []
    for seed in range(n_lists):
        rng = random.Random(7000 + seed)
        py_book, cpp_book = str(work / f"py{seed}.genko"), str(work / f"cpp{seed}.genko")
        shutil.copytree(base, py_book)
        code, out, err = run([GENKO, "migrate", str(base), cpp_book])
        if code != 0:
            print("migrate failed", out, err[-300:])
            return 2
        stats["lists"] += 1
        stopped = False
        for step in range(steps):
            snap = inspect(py_book, False) or {}
            mapping = {}
            pair_ids(snap, inspect(cpp_book, True) or {}, mapping)
            ops = rand_ops(rng, snap)
            f = work / f"ops{seed}_{step}.json"
            fc = work / f"ops{seed}_{step}.cpp.json"
            f.write_text(json.dumps(ops, ensure_ascii=False), encoding="utf-8")
            fc.write_text(json.dumps(translate(ops, mapping), ensure_ascii=False), encoding="utf-8")
            pc, pout, perr = run([PY, "-m", "genko", "apply", py_book, str(f)])
            cc, cout, cerr = run([GENKO, "apply", cpp_book, str(fc)])
            stats["steps"] += 1
            if isinstance(cout, dict) and cout.get("code") == "not_yet_ported":
                stats["stopped_not_yet_ported"] += 1
                stopped = True
                break
            if isinstance(cout, dict) and cout.get("code") in ("refused_python_bug", "comp01a") or (
                    isinstance(cout, dict) and "COMP-01a" in str(cout.get("error", ""))):
                stats["comp01a_refusals"] += 1
                stopped = True
                break
            if pout is None and pc != 0:
                last = perr.strip().splitlines()[-1] if perr.strip() else ""
                pout = {"ok": False, "error": last}
            pn, cn = norm(pout, {}), norm(cout, {})
            keys = ("ok", "error", "applied", "warnings")
            pk = {k: pn.get(k) for k in keys} if isinstance(pn, dict) else pn
            ck = {k: cn.get(k) for k in keys} if isinstance(cn, dict) else cn
            if pc != cc or pk != ck:
                stats["diff"] += 1
                diffs.append({"seed": seed, "step": step, "ops": ops, "py": [pc, pk], "cpp": [cc, ck]})
                stopped = True
                break
            stats["same"] += 1
        if not stopped:
            ps, cs = norm(inspect(py_book, False), {}), norm(inspect(cpp_book, True), {})
            if ps != cs:
                stats["diff"] += 1
                diffs.append({"seed": seed, "final_book_differs": True,
                              "py": json.dumps(ps, ensure_ascii=False)[:400], "cpp": json.dumps(cs, ensure_ascii=False)[:400]})
            else:
                stats["books_same"] += 1
    print(json.dumps(stats, ensure_ascii=False))
    for d in diffs[:10]:
        print(json.dumps(d, ensure_ascii=False)[:1500])
    shutil.rmtree(work, ignore_errors=True)
    return 0 if stats["diff"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
