#!/opt/pyref/bin/python
"""Hermes の独立確認（M1）: 作業者の試験を使わず、genko 実行ファイルを直接動かして保存の安全性を確かめる。

コンテナ内で実行: /src/build/linux-debug/src/api/genko（故障注入あり）と linux-release を使う。
"""
import hashlib, json, os, shutil, subprocess, sys, tempfile, time
from pathlib import Path

DBG = "/src/build/linux-debug/src/api/genko"
REL = "/src/build/linux-release/src/api/genko"
PY = "/opt/pyref/bin/python"
results = []


def run(binary, *args, env=None, inp=None):
    e = dict(os.environ)
    e.pop("GENKO_FAULT", None)
    if env:
        e.update(env)
    p = subprocess.run([binary, *args], capture_output=True, text=True, env=e, input=inp, timeout=120)
    try:
        out = json.loads(p.stdout) if p.stdout.strip() else None
    except ValueError:
        out = {"raw": p.stdout[:300]}
    return p.returncode, out, p.stderr[-300:]


def check(name, ok, detail=""):
    results.append({"check": name, "ok": bool(ok), "detail": detail})


def tree_hash(root: Path):
    h = {}
    for p in sorted(root.rglob("*")):
        if p.is_file() and p.name != "project.lock":
            h[p.relative_to(root).as_posix()] = hashlib.sha256(p.read_bytes()).hexdigest()
    return h


def journal_lines(book: Path):
    j = book / "studio" / "journal.jsonl"
    out = []
    if j.exists():
        for line in j.read_text(encoding="utf-8").split("\n"):
            if line.strip():
                out.append(json.loads(line))
    return out


def audit_lines(book: Path):
    a = book / "studio" / "audit.jsonl"
    return [json.loads(l) for l in a.read_text(encoding="utf-8").split("\n") if l.strip()] if a.exists() else []


work = Path(tempfile.mkdtemp(prefix="hermes-m1-"))
base = work / "基準 原稿.genko"
code, out, err = run(REL, "new", str(base), "--title", "確認", "--pages", "2")
check("new", code == 0 and out and out.get("ok"), f"{code} {out} {err}")
ops = json.dumps([{"op": "name_ok", "page": 1}, {"op": "set_note", "page": 2, "note": "メモ🙂"}], ensure_ascii=False)

# 1) 各段階 × 各故障: 旧か新の整合した状態に戻り、次の保存が一度だけ確定する
for stage in ("assets", "prepare", "project", "audit", "commit"):
    for action in ("crash", "fail", "late", "torn"):
        book = work / f"b-{stage}-{action}.genko"
        shutil.copytree(base, book)
        before = json.loads((book / "project.json").read_text(encoding="utf-8"))
        c1, o1, e1 = run(DBG, "apply", str(book), "-", "--agent", "human:作者", env={"GENKO_FAULT": f"{stage}:{action}"}, inp=ops)
        # 故障なしの別プロセスで開く
        c2, o2, e2 = run(REL, "inspect", str(book), "--full")
        c3, o3, e3 = run(REL, "doctor", str(book))
        state = json.loads((book / "project.json").read_text(encoding="utf-8"))
        page1_ok = state["pages"][0]["name_ok"]
        note2 = state["pages"][1]["note"]
        consistent = (page1_ok, note2) in ((False, ""), (True, "メモ🙂"))
        # 次の保存（故障なし）
        c4, o4, e4 = run(REL, "apply", str(book), "-", "--agent", "human:作者",
                         inp=json.dumps([{"op": "set_note", "page": 1, "note": "次"}], ensure_ascii=False))
        after = json.loads((book / "project.json").read_text(encoding="utf-8"))
        lines = journal_lines(book)
        commits = [l["txn"] for l in lines if l.get("kind") == "commit"]
        dup_commit = len(commits) != len(set(commits))
        prep = {l["txn"] for l in lines if l.get("kind") == "prepare"}
        closed = {l["txn"] for l in lines if l.get("kind") in ("commit", "abort")}
        dangling = prep - closed
        auds = [a.get("txn") for a in audit_lines(book) if a.get("txn")]
        dup_audit = len(auds) != len(set(auds))
        revs = [l.get("rev") for l in lines if l.get("kind") == "commit"]
        mono = all(b > a for a, b in zip(revs, revs[1:]))
        approved_once = sum(1 for a in audit_lines(book) for ch in (a.get("changes") or []) if "name_ok" in json.dumps(ch)) <= 1
        ok = (c2 == 0 and consistent and c4 == 0 and after["pages"][0]["note"] == "次" and not dup_commit and not dangling
              and not dup_audit and mono and after["revision"] > before["revision"] and approved_once)
        check(f"fault {stage}:{action}", ok,
              f"apply1={c1} inspect={c2} doctor={c3} state={(page1_ok, note2)} next={c4} rev {before['revision']}→{after['revision']} "
              f"dupc={dup_commit} dangling={len(dangling)} dupa={dup_audit} mono={mono} once={approved_once}")

# 2) Undo → 別の編集 → 古い expect-revision を拒否（ABA）
book = work / "aba.genko"
shutil.copytree(base, book)
r0 = json.loads((book / "project.json").read_text(encoding="utf-8"))["revision"]
run(REL, "apply", str(book), "-", "--agent", "ai:a", inp=json.dumps([{"op": "set_note", "page": 1, "note": "x"}]))
run(REL, "undo", str(book), "--as", "ai:a")
c, o, e = run(REL, "apply", str(book), "-", "--agent", "ai:b", "--expect-revision", str(r0),
              inp=json.dumps([{"op": "set_note", "page": 1, "note": "y"}]))
r1 = json.loads((book / "project.json").read_text(encoding="utf-8"))["revision"]
check("ABA: old expect-revision refused after undo", c != 0 and r1 == r0 + 2, f"code={c} out={o} rev {r0}→{r1}")

# 3) 承認を変える Undo は AI では拒否
book = work / "gate.genko"
shutil.copytree(base, book)
run(REL, "apply", str(book), "-", "--agent", "human:作者", inp=json.dumps([{"op": "name_ok", "page": 1}]))
c, o, e = run(REL, "undo", str(book), "--as", "ai:x")
c2, o2, e2 = run(REL, "undo", str(book), "--as", "ai:x", "--force")
st = json.loads((book / "project.json").read_text(encoding="utf-8"))
check("AI cannot undo an approval (even with --force)", c != 0 and c2 != 0 and st["pages"][0]["name_ok"] is True,
      f"undo={c} {o} force={c2} {o2}")

# 4) Python 版が project.lock を持っている間は C++ が書かない
book = work / "lock.genko"
shutil.copytree(base, book)
holder = subprocess.Popen([PY, "-c", (
    "import sys,time; sys.path.insert(0,'/src/src'); from pathlib import Path; from genko.lock import ProjectLock\n"
    f"l=ProjectLock(Path({str(book)!r}), agent='human:py'); l.__enter__(); print('held', flush=True); time.sleep(20)")],
    stdout=subprocess.PIPE, text=True, env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"})
holder.stdout.readline()
rb = json.loads((book / "project.json").read_text(encoding="utf-8"))["revision"]
c, o, e = run(REL, "apply", str(book), "-", "--agent", "human:a", inp=json.dumps([{"op": "set_note", "page": 1, "note": "z"}]))
ra = json.loads((book / "project.json").read_text(encoding="utf-8"))["revision"]
holder.kill(); holder.wait()
check("Python lock blocks the C++ writer", c != 0 and ra == rb, f"code={c} out={o}")

# 5) 旧原稿の変換: 原本は無変更、変換後に旧履歴まで Undo できる、version 4・世代は増える
for v in ("v1", "v2", "v3"):
    src = work / f"src-{v}.genko"
    shutil.copytree(f"/src/native/tests/data/legacy/book-{v}.genko", src)
    h0 = tree_hash(src)
    dst = work / f"変換後 {v}.genko"
    c, o, e = run(REL, "migrate", str(src), str(dst), "--as", "human:作者")
    h1 = tree_hash(src)
    pj = json.loads((dst / "project.json").read_text(encoding="utf-8")) if dst.exists() else {}
    ok = c == 0 and h0 == h1 and pj.get("version") == 4 and (dst / "legacy").is_dir()
    detail = f"code={c} same_source={h0 == h1} version={pj.get('version')}"
    if v == "v3" and ok:
        # v3 の旧履歴: 保存4回+Undo+Redo。Undo を繰り返して旧履歴へ入れることを確認
        revs = [pj["revision"]]
        undone = 0
        for _ in range(4):
            cu, ou, eu = run(REL, "undo", str(dst), "--as", "human:作者", "--force")
            if cu != 0:
                break
            undone += 1
            revs.append(json.loads((dst / "project.json").read_text(encoding="utf-8"))["revision"])
        cr, orr, er = run(REL, "redo", str(dst), "--as", "human:作者", "--force")
        revs.append(json.loads((dst / "project.json").read_text(encoding="utf-8"))["revision"])
        ok = ok and undone >= 2 and cr == 0 and all(b > a for a, b in zip(revs, revs[1:]))
        detail += f" undone={undone} redo={cr} revs={revs}"
    check(f"migrate {v}", ok, detail)

# 6) v3 原稿に直接書こうとすると、書き換えずに変換を案内する
src = work / "direct-v3.genko"
shutil.copytree("/src/native/tests/data/legacy/book-v3.genko", src)
h0 = tree_hash(src)
c, o, e = run(REL, "apply", str(src), "-", "--agent", "human:a", inp=json.dumps([{"op": "set_note", "page": 1, "note": "z"}]))
check("v3 book is not written (needs_migration)", c == 3 and tree_hash(src) == h0 and not (src / "project.lock").exists(),
      f"code={c} out={o}")

# 7) Release 版は GENKO_FAULT を無視する
book = work / "release-fault.genko"
shutil.copytree(base, book)
c, o, e = run(REL, "apply", str(book), "-", "--agent", "human:a", env={"GENKO_FAULT": "project:crash"},
              inp=json.dumps([{"op": "set_note", "page": 1, "note": "r"}]))
check("release ignores GENKO_FAULT", c == 0, f"code={c}")

shutil.rmtree(work, ignore_errors=True)
bad = [r for r in results if not r["ok"]]
print(json.dumps({"checks": len(results), "failed": len(bad), "failures": bad}, ensure_ascii=False, indent=1))
for r in results:
    print(("OK  " if r["ok"] else "NG  ") + r["check"] + "  | " + r["detail"][:200])
sys.exit(1 if bad else 0)
