"""Built-in materials made from their shapes when first asked for, so they cost nothing to ship:

- 漫符 (manga marks): sweat, anger mark, surprise lines, heart, star, sparkle, note, spiral, exclamation,
  question, light bulb, flower, steam, motion marks, shine — pen lines ready to put on a page;
- 小物 (props): window, door, chair, desk, bookshelf, tree, cloud, moon, sun, house, mountains, grass, street
  lamp, cup;
- 背景（線画）: the 3D scenes (room, classroom, corridor, street) traced into lines;
- 3D: figures, boxes, stairs, floors and scenes to set up as guides;
- ブラシ: brush presets (rain, snow, falling leaves, star sky, lace trim, hatching).

Every entry has "tags" (words a search finds). Lines are in mm around (0, 0); materials put them where
clicked.
"""

from __future__ import annotations

import math
from functools import lru_cache

Point = tuple[float, float]


def _circle(cx: float, cy: float, r: float, a0: float = 0.0, a1: float = 360.0, n: int = 32) -> list[Point]:
    return [(cx + r * math.cos(math.radians(a0 + (a1 - a0) * k / n)), cy + r * math.sin(math.radians(a0 + (a1 - a0) * k / n)))
            for k in range(n + 1)]


def _ellipse(cx, cy, rx, ry, n=36) -> list[Point]:
    return [(cx + rx * math.cos(2 * math.pi * k / n), cy + ry * math.sin(2 * math.pi * k / n)) for k in range(n + 1)]


def _rect(x, y, w, h) -> list[Point]:
    return [(x, y), (x + w, y), (x + w, y + h), (x, y + h), (x, y)]


def _heart(cx, cy, s) -> list[Point]:
    pts = []
    for k in range(49):
        t = 2 * math.pi * k / 48
        x = 16 * math.sin(t) ** 3
        y = -(13 * math.cos(t) - 5 * math.cos(2 * t) - 2 * math.cos(3 * t) - math.cos(4 * t))
        pts.append((cx + x * s / 32, cy + y * s / 32))
    return pts


def _star(cx, cy, r, points=5, inner=0.45) -> list[Point]:
    out = []
    for k in range(points * 2 + 1):
        a = -math.pi / 2 + math.pi * k / points
        rr = r if k % 2 == 0 else r * inner
        out.append((cx + rr * math.cos(a), cy + rr * math.sin(a)))
    return out


def _spiral(cx, cy, r, turns=3) -> list[Point]:
    n = int(turns * 36)
    return [(cx + r * k / n * math.cos(2 * math.pi * turns * k / n), cy + r * k / n * math.sin(2 * math.pi * turns * k / n))
            for k in range(n + 1)]


def _drop(cx, cy, s) -> list[Point]:
    """A drop of sweat: a point on top, round below."""
    pts = [(cx, cy - s)]
    pts += _circle(cx, cy + s * 0.35, s * 0.45, -20, 200, 20)
    pts.append((cx, cy - s))
    return pts


MARKS = {
    "汗": (["汗", "あせ", "焦り", "しずく"], lambda: [_drop(0, 0, 4), _drop(6, 3, 2.6)]),
    "怒りマーク": (["怒り", "いかり", "青筋", "怒"], lambda: [_circle(-2.5, -2.5, 3, 0, 90, 8), _circle(2.5, -2.5, 3, 90, 180, 8),
                                                           _circle(2.5, 2.5, 3, 180, 270, 8), _circle(-2.5, 2.5, 3, 270, 360, 8)]),
    "驚き線": (["驚き", "びっくり", "おどろき"], lambda: [[(-6, -2), (-8, -8)], [(0, -3), (0, -10)], [(6, -2), (8, -8)]]),
    "ハート": (["ハート", "恋", "好き", "愛"], lambda: [_heart(0, 0, 10)]),
    "星": (["星", "ほし", "スター"], lambda: [_star(0, 0, 5)]),
    "キラキラ": (["キラキラ", "輝き", "光", "きらめき"], lambda: [_star(0, 0, 5, 4, 0.18), _star(8, -5, 2.5, 4, 0.18), _star(-6, 6, 2, 4, 0.18)]),
    "音符": (["音符", "音楽", "歌", "鼻歌"], lambda: [_ellipse(-2, 6, 2.2, 1.5), [(0.2, 6), (0.2, -6), (4, -4)]]),
    "ぐるぐる": (["渦", "混乱", "目が回る", "ぐるぐる"], lambda: [_spiral(0, 0, 6)]),
    "びっくりマーク": (["感嘆符", "びっくり", "！"], lambda: [[(0, -8), (0, 3)], _circle(0, 7, 0.9, n=10)]),
    "はてなマーク": (["疑問", "はてな", "？"], lambda: [_circle(0, -4, 4, 180, 400, 20) + [(0, 1), (0, 3)], _circle(0, 7, 0.9, n=10)]),
    "ひらめき": (["ひらめき", "電球", "アイデア"], lambda: [_circle(0, -2, 5, 130, 410, 28), _rect(-2.5, 3, 5, 3),
                                                         [(-7, -9), (-9, -11)], [(0, -9), (0, -12)], [(7, -9), (9, -11)]]),
    "花": (["花", "はな", "フラワー", "喜び"], lambda: [_circle(0, 0, 1.5, n=12)] + [_circle(3.2 * math.cos(math.radians(a)),
                                                                                    3.2 * math.sin(math.radians(a)), 1.9, n=14)
                                                                            for a in range(0, 360, 72)]),
    "湯気": (["湯気", "煙", "ゆげ", "怒り"], lambda: [[(x + 2 * math.sin(t / 3), -t) for t in range(0, 13)] for x in (-4, 0, 4)]),
    "動きの線": (["動き", "揺れ", "震え", "モーション"], lambda: [_circle(0, 0, 8, 200, 250, 8), _circle(0, 0, 11, 200, 250, 8),
                                                              _circle(0, 0, 8, -70, -20, 8), _circle(0, 0, 11, -70, -20, 8)]),
    "光の線": (["光", "輝き", "後光"], lambda: [[(3 * math.cos(math.radians(a)), 3 * math.sin(math.radians(a))),
                                                 (9 * math.cos(math.radians(a)), 9 * math.sin(math.radians(a)))]
                                                for a in range(0, 360, 30)]),
}

PROPS = {
    "窓": (["窓", "まど", "部屋", "建物"], lambda: [_rect(-15, -20, 30, 40), [(0, -20), (0, 20)], [(-15, 0), (15, 0)]]),
    "ドア": (["ドア", "扉", "とびら", "部屋"], lambda: [_rect(-10, -22, 20, 44), _circle(6, 2, 1, n=10)]),
    "椅子": (["椅子", "いす", "家具"], lambda: [[(-8, -16), (-8, 12)], [(-8, 0), (8, 0), (8, 12)], [(-8, -16), (-6, -16), (-6, 0)]]),
    "机": (["机", "つくえ", "テーブル", "家具"], lambda: [[(-20, -4), (20, -4), (22, -1), (-18, -1), (-20, -4)],
                                                       [(-17, -1), (-17, 14)], [(19, -1), (19, 14)]]),
    "本棚": (["本棚", "本", "家具", "部屋"], lambda: [_rect(-14, -24, 28, 48), [(-14, -8), (14, -8)], [(-14, 8), (14, 8)]]
             + [[(x, -22), (x, -8)] for x in range(-12, 13, 3)] + [[(x, -6), (x, 8)] for x in range(-12, 8, 3)]),
    "木": (["木", "き", "自然", "森"], lambda: [[(-2, 20), (-2, 2)], [(2, 20), (2, 2)], _ellipse(0, -8, 12, 11)]),
    "雲": (["雲", "くも", "空", "天気"], lambda: [_circle(-8, 0, 5, 90, 270, 12) + _circle(-2, -4, 6, 180, 360, 14)
                                                 + _circle(7, -1, 5, 230, 450, 12) + [(-8, 5)]]),
    "月": (["月", "つき", "夜"], lambda: [_circle(0, 0, 8, 60, 300, 28) + _circle(4, 0, 6.5, 250, 110, 20)]),
    "太陽": (["太陽", "たいよう", "晴れ", "空"], lambda: [_circle(0, 0, 6, n=30)] + [[(8 * math.cos(math.radians(a)), 8 * math.sin(math.radians(a))),
                                                                                      (12 * math.cos(math.radians(a)), 12 * math.sin(math.radians(a)))]
                                                                                     for a in range(0, 360, 45)]),
    "家": (["家", "いえ", "建物", "街"], lambda: [[(-15, -2), (0, -16), (15, -2)], _rect(-12, -2, 24, 20), _rect(-3, 8, 6, 10)]),
    "山": (["山", "やま", "自然", "遠景"], lambda: [[(-30, 10), (-12, -12), (-2, 0), (10, -16), (30, 10)]]),
    "草": (["草", "くさ", "地面", "自然"], lambda: [[(x, 0), (x + 1.5, -5 - (x % 3))] for x in range(-12, 13, 3)]),
    "街灯": (["街灯", "電柱", "外", "街", "夜"], lambda: [[(0, 30), (0, -20), (6, -22)], _rect(4, -22, 5, 3)]),
    "カップ": (["カップ", "コップ", "飲み物", "小物"], lambda: [[(-5, -6), (-4, 6), (4, 6), (5, -6)], _circle(6, 0, 3, -90, 90, 10)]),
}

BRUSH_PRESETS = {
    "雨ブラシ": (["雨", "あめ", "天気", "ブラシ"], {"base": "dashline", "label": "雨", "width_mm": 3.0, "spacing": 1.3, "scatter": 1.2,
                                                  "count": 2, "size_jitter": 0.4}),
    "雪ブラシ": (["雪", "ゆき", "天気", "冬", "ブラシ"], {"base": "stipple", "label": "雪", "width_mm": 6.0, "spacing": 0.6, "scatter": 0.9,
                                                         "size_jitter": 0.7, "rgb": [255, 255, 255]}),
    "落ち葉ブラシ": (["葉", "落ち葉", "秋", "自然", "ブラシ"], {"base": "leaves", "label": "落ち葉", "width_mm": 6.0, "scatter": 1.0,
                                                               "spacing": 0.9}),
    "星空ブラシ": (["星", "夜空", "星空", "ブラシ"], {"base": "stars", "label": "星空", "width_mm": 2.0, "spacing": 2.5, "scatter": 1.0,
                                                   "size_jitter": 0.8}),
    "レース飾りブラシ": (["レース", "飾り", "服", "ブラシ"], {"base": "lace", "label": "レース飾り", "width_mm": 4.0}),
    "カケアミ風ブラシ": (["カケアミ", "影", "斜線", "ブラシ"], {"base": "mili", "label": "カケアミ風", "width_mm": 0.2, "post_smooth": 0}),
}

PRIMS = {
    "デッサン人形": (["人物", "人形", "ポーズ", "3D"], {"prim": "mannequin"}),
    "箱": (["箱", "建物", "3D"], {"prim": "box"}),
    "円柱": (["円柱", "柱", "3D"], {"prim": "cylinder"}),
    "階段": (["階段", "建物", "3D"], {"prim": "stairs"}),
    "床の格子": (["床", "地面", "パース", "3D"], {"prim": "floor"}),
    "部屋（3D）": (["部屋", "室内", "背景", "3D"], {"scene": "room"}),
    "教室（3D）": (["教室", "学校", "背景", "3D"], {"scene": "classroom"}),
    "廊下（3D）": (["廊下", "学校", "背景", "3D"], {"scene": "corridor"}),
    "街並み（3D）": (["街", "外", "背景", "3D"], {"scene": "street"}),
}


def _lines_item(key: str, name: str, folder: str, tags: list[str], paths: list, width: float) -> dict:
    strokes = [{"points": [[round(x, 3), round(y, 3)] for x, y in path], "width_mm": width, "kind": "mili"} for path in paths if len(path) >= 2]
    return {"id": key, "kind": "lines", "name": name, "folder": folder, "tags": tags, "items": {"strokes": strokes, "patches": []}}


def _scene_lines(kind: str) -> list:
    """A 3D scene traced into lines, about 180 mm across."""
    from genko import prim3d

    size = list(prim3d.SCENE_SIZES[kind])
    rot, near = prim3d.SCENE_VIEWS[kind]
    focal = 220.0
    prim = {"id": "s", "kind": "scene", "scene": kind, "pos": [0.0, 0.0, size[2] / 2 + near * focal], "size": size,
            "rot": list(rot), "focal_mm": focal}
    paths = prim3d.trace(prim)
    xs = [p[0] for path in paths for p in path]
    ys = [p[1] for path in paths for p in path]
    if not xs:
        return []
    k = 180.0 / max(1e-6, max(xs) - min(xs))
    cx, cy = (max(xs) + min(xs)) / 2, (max(ys) + min(ys)) / 2
    return [[((x - cx) * k, (y - cy) * k) for x, y in path] for path in paths]


@lru_cache(maxsize=1)
def generated() -> tuple[dict, ...]:
    out: list[dict] = []
    for name, (tags, make) in MARKS.items():
        out.append(_lines_item(f"mark-{name}", name, "漫符", tags + ["漫符"], make(), 0.35))
    for name, (tags, make) in PROPS.items():
        out.append(_lines_item(f"prop-{name}", name, "小物", tags + ["小物"], make(), 0.3))
    from genko import prim3d

    for kind, label in prim3d.SCENE_LABELS.items():
        try:
            paths = _scene_lines(kind)
        except Exception:  # (a scene that cannot be traced here is left out)
            continue
        out.append(_lines_item(f"bg-{kind}", f"{label}（線画）", "背景（線画）", [label, "背景", "線画"], paths, 0.25))
    for name, (tags, data) in BRUSH_PRESETS.items():
        out.append({"id": f"brush-{name}", "kind": "brush", "name": name, "folder": "ブラシ", "tags": tags, "brush": data})
    for name, (tags, data) in PRIMS.items():
        out.append({"id": f"3d-{name}", "kind": "prim", "name": name, "folder": "3D", "tags": tags, **data})
    return tuple(out)
