"""Input schemas for agents (bible@1, script@1, name_plan@1).

Kept conservative so any LLM can emit them as tool arguments: no recursion,
no numeric or length limits (Genko checks ranges in lint), every property
required and optional values nullable.
"""

from __future__ import annotations


def obj(**props: dict) -> dict:
    return {"type": "object", "properties": props, "required": list(props), "additionalProperties": False}


def arr(items: dict) -> dict:
    return {"type": "array", "items": items}


def nullable(schema: dict) -> dict:
    return {"anyOf": [schema, {"type": "null"}]}


def enum(*values: str) -> dict:
    return {"type": "string", "enum": list(values)}


STR = {"type": "string"}
NUM = {"type": "number"}
INT = {"type": "integer"}
BOOL = {"type": "boolean"}

SHOTS = ("ELS", "LS", "FS", "MS", "MCU", "CU", "ECU", "INSERT")
ANGLES = ("eye", "high", "low", "bird", "worm", "dutch")
POSITIONS = ("left", "left_third", "center", "right_third", "right")
FACINGS = ("left", "right", "front", "back")
BALLOONS = ("speech", "thought", "shout", "whisper", "narration")
BEAT_KINDS = ("action", "dialogue", "monologue", "narration", "sfx")
TEXT_BEATS = ("dialogue", "monologue", "narration")

BIBLE_V1 = obj(
    title=STR,
    logline=STR,
    plot=STR,
    themes=arr(STR),
    characters=arr(
        obj(
            id=STR,
            name=STR,
            role=STR,
            age=nullable(INT),
            look=obj(
                hair=STR,
                hair_value=enum("beta", "tone", "white"),
                eyes=STR,
                build=STR,
                height_cm=nullable(NUM),
                outfits=arr(obj(id=STR, desc=STR)),
                marks=arr(STR),
                silhouette=STR,
            ),
            speech=obj(first_person=STR, tone=STR),
            tokens_en=nullable(STR),
        )
    ),
    locations=arr(obj(id=STR, name=STR, desc=STR, times=arr(STR))),
    style=obj(notes=arr(STR)),
    constraints=arr(STR),
)

SCRIPT_V1 = obj(
    scenes=arr(
        obj(
            id=STR,
            summary=STR,
            location_id=nullable(STR),
            time=nullable(STR),
            beats=arr(
                obj(
                    id=STR,
                    kind=enum(*BEAT_KINDS),
                    speaker_id=nullable(STR),
                    text=STR,
                    emotion=STR,
                    page=INT,
                    reveal=BOOL,
                )
            ),
        )
    )
)

NAME_PLAN_V1 = obj(
    page=INT,
    turn_role=enum("normal", "reveal", "hook"),
    template=nullable(STR),
    tiers=nullable(arr(obj(h=NUM, cols=arr(obj(slot=STR, w=NUM, rows=nullable(arr(obj(slot=STR, h=NUM)))))))),
    panels=arr(
        obj(
            slot=STR,
            shot=enum(*SHOTS),
            angle=enum(*ANGLES),
            location_id=nullable(STR),
            time=nullable(STR),
            characters=arr(
                obj(
                    id=STR,
                    pose=STR,
                    expression=STR,
                    facing=enum(*FACINGS),
                    pos=enum(*POSITIONS),
                    scale=NUM,
                )
            ),
            action=STR,
            emotion=STR,
            fx=arr(STR),
            emphasis=NUM,
            cross=BOOL,
            beat_ids=arr(STR),
            lines=arr(obj(beat_id=STR, balloon=enum(*BALLOONS), breaks=arr(STR))),
        )
    ),
)

SCHEMAS = {"bible@1": BIBLE_V1, "script@1": SCRIPT_V1, "name_plan@1": NAME_PLAN_V1}
