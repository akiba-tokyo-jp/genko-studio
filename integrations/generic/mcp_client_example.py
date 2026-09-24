"""Drive Genko from any MCP client: a short run with the official Python SDK over stdio.

    uv run --extra mcp python integrations/generic/mcp_client_example.py --root /tmp/manga

It starts `genko mcp` as a subprocess (the same way Hermes Agent, Claude Code or Claude
Desktop do), creates a project, writes a tiny bible and script, submits one name page,
and prints what `next` asks for. Replace the fixed JSON with your own agent's output.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import anyio
from mcp import Client, StdioServerParameters

BIBLE = {
    "title": "試し", "logline": "屋上で待つ", "plot": "少女が屋上で友だちを待つ。", "themes": ["約束"],
    "characters": [{"id": "hina", "name": "ひな", "role": "heroine", "age": 16,
                    "look": {"hair": "肩までの黒髪", "hair_value": "beta", "eyes": "大きい", "build": "細身", "height_cm": 156,
                             "outfits": [{"id": "default", "desc": "セーラー服"}], "marks": [], "silhouette": "前髪"},
                    "speech": {"first_person": "わたし", "tone": "丁寧"}, "tokens_en": "a slim schoolgirl with shoulder-length black hair"}],
    "locations": [{"id": "roof", "name": "屋上", "desc": "フェンスのある屋上", "times": ["sunset"]}],
    "style": {"notes": []}, "constraints": [],
}
SCRIPT = {"scenes": [{"id": "s1", "summary": "待つ", "location_id": "roof", "time": "sunset", "beats": [
    {"id": "b1", "kind": "action", "speaker_id": None, "text": "屋上で待つひな", "emotion": "不安", "page": 1, "reveal": False},
    {"id": "b2", "kind": "monologue", "speaker_id": "hina", "text": "来るかな", "emotion": "不安", "page": 1, "reveal": False}]}]}
PLAN = {"page": 1, "turn_role": "normal", "template": None,
        "tiers": [{"h": 1.0, "cols": [{"slot": "p1", "w": 1.0, "rows": None}]}],
        "panels": [{"slot": "p1", "shot": "MS", "angle": "eye", "location_id": "roof", "time": "sunset",
                    "characters": [{"id": "hina", "pose": "立つ", "expression": "不安", "facing": "left", "pos": "center", "scale": 0.8}],
                    "action": "屋上で待つ", "emotion": "不安", "fx": [], "emphasis": 0.5, "cross": False, "beat_ids": ["b1", "b2"],
                    "lines": [{"beat_id": "b2", "balloon": "thought", "breaks": ["来るかな"]}]}]}


async def run(root: Path, agent: str) -> dict:
    params = StdioServerParameters(command=sys.executable, args=["-m", "genko", "mcp", "--root", str(root), "--agent", agent])
    report: dict = {}
    async with Client(params) as client:
        tools = sorted(t.name for t in (await client.list_tools()).tools)
        report["tools"] = len(tools)

        async def call(tool_name: str, **arguments) -> dict:
            result = await client.call_tool(tool_name, arguments)
            return json.loads(result.content[0].text)

        report["create"] = (await call("create_project", name="try.genko", title="試し", pages=1))["ok"]
        report["bible"] = (await call("set_bible", project="try.genko", bible=BIBLE, commit=True))["committed"]
        report["script"] = (await call("set_script", project="try.genko", script=SCRIPT, commit=True))["committed"]
        report["name"] = (await call("submit_name", project="try.genko", plan=PLAN, commit=True))["committed"]
        report["next"] = [item["kind"] for item in (await call("next", project="try.genko", limit=5))["items"]]
        skill = await client.read_resource("genko://guide/skill")
        report["skill_chars"] = len(skill.contents[0].text)
    return report


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--agent", default="ai:example")
    args = parser.parse_args()
    args.root.mkdir(parents=True, exist_ok=True)
    print(json.dumps(anyio.run(run, args.root, args.agent), ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
