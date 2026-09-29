"""チャットでの承認: a person who approves in a chat (Telegram…) has the AI record it, in the person's name, with no
setting to turn on first; a person can stop it for a book (the AI cannot turn it back on), never for the final export."""

from __future__ import annotations

import json
from pathlib import Path

from genko.io import load_episode
from genko.studio.service import HumanService, StudioService

FIXTURES = Path(__file__).parent / "fixtures" / "studio" / "demo4"


def _book(tmp_path: Path) -> tuple[StudioService, Path]:
    agent = StudioService(tmp_path, "ai:hermes")
    assert agent.create_project("demo.genko", "demo", 4).ok
    load = lambda name: json.loads((FIXTURES / name).read_text(encoding="utf-8"))  # noqa: E731
    assert agent.set_bible("demo.genko", load("bible.json"), commit=True).ok
    assert agent.set_script("demo.genko", load("script.json"), commit=True).ok
    assert agent.submit_name("demo.genko", load("p001.json"), commit=True).ok
    return agent, tmp_path / "demo.genko"


def test_a_chat_approval_is_recorded_without_a_setting_and_stops_where_the_person_stopped_it(tmp_path):
    agent, path = _book(tmp_path)
    done = agent.record_chat_approval("demo.genko", "name", "承認します 続けてください", pages=[1], person="mi mi")
    assert done.ok and done.data["by"] == "human:mi_mi" and done.data["via"].startswith("ai:hermes")
    episode = load_episode(path)
    assert episode.pages[0].name_ok
    record = episode.studio["approvals"][-1]
    assert record["by"] == "human:mi_mi" and record["via"].startswith("ai:hermes") and record["message"] == "承認します 続けてください"
    # no name given: the person who last approved in this book
    again = agent.record_chat_approval("demo.genko", "name", "OK", pages=[1])
    assert again.ok and again.data["by"] == "human:mi_mi"
    # the person's words are needed, and the final export stays with the person in Genko
    assert agent.record_chat_approval("demo.genko", "name", "  ", pages=[1]).issues[0].code == "message_required"
    assert agent.record_chat_approval("demo.genko", "export", "OK").issues[0].code == "gate_not_available"
    # a person can stop it for this book; the AI cannot turn it back on
    HumanService(path, "human:mimi").chat_approval(False)
    assert agent.record_chat_approval("demo.genko", "name", "OK", pages=[1]).issues[0].code == "chat_approval_off"
    assert not agent.apply_ops("demo.genko", [{"op": "allow_chat_approval", "on": True}], commit=True).ok
    HumanService(path, "human:mimi").chat_approval(True)
    back = agent.record_chat_approval("demo.genko", "name", "OK", pages=[1])
    assert back.ok and back.data["by"] == "human:mimi"  # (open again: no name given, the person who opened it)
