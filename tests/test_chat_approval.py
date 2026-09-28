"""チャットでの承認: a person who approves in a chat (Telegram…) has the AI record it, in the person's name, once the
person let this book take chat approvals; never without that, never for the final export."""

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


def test_a_chat_approval_is_recorded_only_when_the_person_allowed_it(tmp_path):
    agent, path = _book(tmp_path)
    off = agent.record_chat_approval("demo.genko", "name", "承認します 続けてください", pages=[1])
    assert not off.ok and off.issues[0].code == "chat_approval_off"
    assert not load_episode(path).pages[0].name_ok
    # the AI cannot turn it on for itself
    refused = agent.apply_ops("demo.genko", [{"op": "allow_chat_approval", "on": True}], commit=True)
    assert not refused.ok
    HumanService(path, "human:mimi").chat_approval(True)
    done = agent.record_chat_approval("demo.genko", "name", "承認します 続けてください", pages=[1])
    assert done.ok and done.data["by"] == "human:mimi" and done.data["via"].startswith("ai:hermes")
    episode = load_episode(path)
    assert episode.pages[0].name_ok
    record = episode.studio["approvals"][-1]
    assert record["by"] == "human:mimi" and record["via"].startswith("ai:hermes") and record["message"] == "承認します 続けてください"
    # the person's words are needed, and the final export stays with the person in Genko
    assert agent.record_chat_approval("demo.genko", "name", "  ", pages=[1]).issues[0].code == "message_required"
    assert agent.record_chat_approval("demo.genko", "export", "OK").issues[0].code == "gate_not_available"
    HumanService(path, "human:mimi").chat_approval(False)
    assert agent.record_chat_approval("demo.genko", "name", "OK", pages=[1]).issues[0].code == "chat_approval_off"
