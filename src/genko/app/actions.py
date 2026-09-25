"""オートアクション: record what was done (the ops) under a name, and do it again on another page.

The recording keeps the ops as they were applied. Played back, they go to the page on screen and the
layer being drawn on: "page" becomes the current page, "layer_id" the current layer, a panel the chosen
panel (or none), and things the recording made (a layer, a tone, a line…) get new ids each time.
Actions are kept in the settings folder (actions.json), so every book can use them.
"""

from __future__ import annotations

import copy
import json
import time
from pathlib import Path

# ops that make something with an "id" of its own
MAKERS = ("add_layer", "add_tone", "add_line", "add_effect", "add_prim3d", "add_scene", "add_mannequin", "add_ruler",
          "stamp_material", "duplicate_layer")
# ops whose "id" names the layer they change: played back, the layer being drawn on
LAYER_OPS = ("set_layer", "set_layer_mask", "paint_mask", "merge_down", "delete_layer", "duplicate_layer", "filter_raster")
# ops that pick an existing line by its place in a layer: only right on the page they were recorded on
BOUND_OPS = ("delete_stroke", "reshape_stroke", "edit_stroke", "set_stroke_width", "simplify_stroke", "edit_line", "move_line",
             "delete_line", "set_balloon_path", "reorder_lines")
NOT_RECORDED = ("undo", "lock_page", "unlock_page", "name_ok", "advance", "set_meta", "set_bible", "add_page",
                "delete_page", "duplicate_page", "reorder", "set_page_spec")


def path() -> Path:
    from genko.tokens import config_dir

    return config_dir() / "actions.json"


def load() -> dict:
    try:
        data = json.loads(path().read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    return {str(k): v for k, v in data.items() if isinstance(v, dict) and isinstance(v.get("ops"), list)}


def save(actions: dict) -> None:
    target = path()
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(actions, ensure_ascii=False, indent=1), encoding="utf-8")


def store(name: str, ops: list[dict]) -> None:
    actions = load()
    actions[name] = {"ops": ops, "made": time.strftime("%Y-%m-%d %H:%M")}
    save(actions)


def remove(name: str) -> None:
    actions = load()
    actions.pop(name, None)
    save(actions)


def recordable(ops: list[dict]) -> list[dict]:
    return [copy.deepcopy(op) for op in ops if isinstance(op, dict) and op.get("op") not in NOT_RECORDED]


def _made(ops: list[dict]) -> set[str]:
    out = set()
    for op in ops:
        if op.get("op") in MAKERS and op.get("id") and op.get("op") != "duplicate_layer":
            out.add(str(op["id"]))
        if op.get("op") == "duplicate_layer" and op.get("new_id"):
            out.add(str(op["new_id"]))
    return out


def page_bound(ops: list[dict]) -> list[bool]:
    """For each step: does it change one particular thing that was on the recorded page (a line, a stroke,
    an effect…)? Such steps cannot be done on another page and are left out when played."""
    made = _made(ops)
    out = []
    for op in ops:
        name = op.get("op")
        bound = False
        if name not in MAKERS and name not in LAYER_OPS and op.get("id") and str(op["id"]) not in made:
            bound = True
        if any(op.get(key) for key in ("ids", "stroke_id", "stroke_ids", "line_id")) or name in BOUND_OPS:
            bound = True
        out.append(bound)
    return out


def playable(ops: list[dict]) -> list[dict]:
    return [op for op, bound in zip(ops, page_bound(ops)) if not bound]


def replay(ops: list[dict], page: int, layer_id: str | None, frame_id: str | None) -> list[dict]:
    """The recorded ops, aimed at this page, layer and panel, with fresh ids for what they make (steps
    bound to one thing on the recorded page are left out: see page_bound)."""
    from genko.models import new_id

    ops = playable(ops)
    made: dict[str, str] = {}
    for op in ops:
        if op.get("op") in MAKERS and op.get("id") and op.get("op") != "duplicate_layer":
            made.setdefault(str(op["id"]), new_id())
        if op.get("op") == "duplicate_layer" and op.get("new_id"):
            made.setdefault(str(op["new_id"]), new_id())
    out = []
    for op in ops:
        op = copy.deepcopy(op)
        if "page" in op:
            op["page"] = page
        for key in ("id", "new_id", "after", "parent", "parent_id", "clip_to"):
            if isinstance(op.get(key), str) and op[key] in made:
                op[key] = made[op[key]]
            elif key == "id" and op.get("op") in LAYER_OPS and op.get("id") and layer_id:
                op["id"] = layer_id  # (the layer it changed then: the one drawn on now)
        if "layer_id" in op:
            recorded = str(op["layer_id"])
            op["layer_id"] = made.get(recorded) or layer_id or recorded
        if "frame_id" in op:
            if frame_id:
                op["frame_id"] = frame_id
            else:
                op.pop("frame_id")
        out.append(op)
    return out


def describe(ops: list[dict]) -> str:
    from genko.app.history import describe as said

    return said(ops)


def step_label(op: dict) -> str:
    from genko.app.history import describe as said

    return said([op])


class ActionsDialog:
    """オートアクションの管理: each action's steps, to look at, reorder, leave out, rename, play or delete."""

    def __new__(cls, window):
        from PySide6.QtCore import Qt
        from PySide6.QtWidgets import (
            QDialog,
            QDialogButtonBox,
            QHBoxLayout,
            QInputDialog,
            QLabel,
            QListWidget,
            QListWidgetItem,
            QMessageBox,
            QPushButton,
            QVBoxLayout,
        )

        dialog = QDialog(window)
        dialog.setWindowTitle("オートアクションの管理")
        dialog.resize(560, 420)
        names = QListWidget()
        steps = QListWidget()
        note = QLabel("灰色の手順は、記録したページの決まった物（台詞・線など）を変えるもので、ほかのページでは飛ばします。")
        note.setWordWrap(True)
        note.setStyleSheet("color:#666")
        state = {"data": load()}

        def fill_names(select: str | None = None) -> None:
            names.clear()
            for name in state["data"]:
                names.addItem(name)
            if names.count():
                items = names.findItems(select, Qt.MatchFlag.MatchExactly) if select else []
                names.setCurrentItem(items[0] if items else names.item(0))
            fill_steps()

        def current() -> str | None:
            item = names.currentItem()
            return item.text() if item else None

        def fill_steps() -> None:
            steps.clear()
            name = current()
            if name is None:
                return
            ops = state["data"][name]["ops"]
            for n, (op, bound) in enumerate(zip(ops, page_bound(ops)), 1):
                item = QListWidgetItem(f"{n}. {step_label(op)}" + ("（このページだけ）" if bound else ""))
                if bound:
                    item.setForeground(Qt.GlobalColor.gray)
                steps.addItem(item)

        def keep() -> None:
            save(state["data"])
            fill_steps()

        def move(delta: int) -> None:
            name, row = current(), steps.currentRow()
            ops = state["data"][name]["ops"] if name else []
            if not name or not 0 <= row < len(ops) or not 0 <= row + delta < len(ops):
                return
            ops[row], ops[row + delta] = ops[row + delta], ops[row]
            keep()
            steps.setCurrentRow(row + delta)

        def drop_step() -> None:
            name, row = current(), steps.currentRow()
            if name and 0 <= row < len(state["data"][name]["ops"]):
                del state["data"][name]["ops"][row]
                keep()

        def rename() -> None:
            name = current()
            if not name:
                return
            new, ok = QInputDialog.getText(dialog, "名前を変える", "新しい名前", text=name)
            if ok and new.strip() and new.strip() != name:
                state["data"] = {(new.strip() if k == name else k): v for k, v in state["data"].items()}
                save(state["data"])
                fill_names(new.strip())

        def delete() -> None:
            name = current()
            if name and QMessageBox.question(dialog, "オートアクション", f"「{name}」を消しますか？") == QMessageBox.StandardButton.Yes:
                state["data"].pop(name, None)
                save(state["data"])
                fill_names()

        def play() -> None:
            name = current()
            if name:
                window.play_action(name)

        names.currentRowChanged.connect(lambda _: fill_steps())
        buttons = QHBoxLayout()
        for label, slot in (("↑", lambda: move(-1)), ("↓", lambda: move(1)), ("手順を外す", drop_step), ("名前を変える", rename),
                            ("消す", delete), ("このページで実行", play)):
            button = QPushButton(label)
            button.clicked.connect(slot)
            buttons.addWidget(button)
        close = QDialogButtonBox(QDialogButtonBox.StandardButton.Close)
        close.button(QDialogButtonBox.StandardButton.Close).setText("閉じる")
        close.rejected.connect(dialog.reject)
        row = QHBoxLayout()
        row.addWidget(names, 1)
        row.addWidget(steps, 2)
        layout = QVBoxLayout(dialog)
        layout.addLayout(row, 1)
        layout.addWidget(note)
        layout.addLayout(buttons)
        layout.addWidget(close)
        dialog.names, dialog.steps, dialog.fill_names = names, steps, fill_names  # type: ignore[attr-defined]
        dialog.move_step, dialog.drop_step = move, drop_step  # type: ignore[attr-defined]
        fill_names()
        return dialog
