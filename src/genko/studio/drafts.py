"""M0 sidecar files under <project>/studio/drafts/. Since M3 they are only read by
`genko studio adopt-drafts`, which moves them into project.json.

drafts/bible.json, drafts/script.json, drafts/name/pNNN.json hold what the
agent wrote; drafts/reviews.json, drafts/requests.json and drafts/state.json
hold the agent's self-checks, approval requests, human comments and a counter.
"""

from __future__ import annotations

from pathlib import Path
from typing import Any

from genko.studio.jsonutil import read_json, write_json


class Drafts:
    def __init__(self, project: Path) -> None:
        self.root = Path(project) / "studio" / "drafts"

    def _path(self, name: str) -> Path:
        return self.root / name

    def next_seq(self) -> int:
        state = read_json(self._path("state.json"), {"seq": 0})
        state["seq"] = int(state.get("seq", 0)) + 1
        write_json(self._path("state.json"), state)
        return state["seq"]

    def bible(self) -> dict | None:
        return read_json(self._path("bible.json"))

    def set_bible(self, bible: dict) -> None:
        write_json(self._path("bible.json"), bible)

    def script(self) -> dict | None:
        return read_json(self._path("script.json"))

    def set_script(self, script: dict) -> None:
        write_json(self._path("script.json"), script)

    def name(self, page: int) -> dict | None:
        return read_json(self._path(f"name/p{page:03d}.json"))

    def set_name(self, page: int, record: dict) -> None:
        write_json(self._path(f"name/p{page:03d}.json"), record)

    def delete_name(self, page: int) -> None:
        path = self._path(f"name/p{page:03d}.json")
        if path.exists():
            path.unlink()

    def reviews(self) -> dict[str, Any]:
        return read_json(self._path("reviews.json"), {})

    def set_review(self, page: int, review: dict) -> None:
        reviews = self.reviews()
        reviews[str(page)] = review
        write_json(self._path("reviews.json"), reviews)

    def requests(self) -> list[dict]:
        return read_json(self._path("requests.json"), [])

    def set_requests(self, items: list[dict]) -> None:
        write_json(self._path("requests.json"), items)
