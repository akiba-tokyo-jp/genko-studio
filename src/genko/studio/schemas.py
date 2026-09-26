"""Input schemas for agents (bible@1, script@1, name_plan@1).

Kept conservative so any LLM can emit them as tool arguments: no recursion,
no numeric or length limits (Genko checks ranges in lint), every property
required and optional values nullable.
"""

from __future__ import annotations


def obj(**props: dict) -> dict:
    return {"type": "object", "properties": props, "required": list(props), "additionalProperties": False}


def more(schema: dict, **later: dict) -> dict:
    """An object schema with properties added later. They are required and nullable like the rest (strict tool
    schemas want that); inputs written before them get null filled in (fill_nulls) before they are checked."""
    return {**schema, "properties": {**schema["properties"], **later}, "required": [*schema["required"], *later]}


def fill_nulls(data, schema: dict):
    """A copy of `data` with every missing nullable property set to null (so an input from before a field was
    added still fits the schema)."""
    if isinstance(data, dict) and schema.get("type") == "object":
        out = dict(data)
        for key, sub in schema.get("properties", {}).items():
            if key not in out:
                if any(option.get("type") == "null" for option in sub.get("anyOf", [])):
                    out[key] = None
            else:
                out[key] = fill_nulls(out[key], sub)
        return out
    if isinstance(data, list) and schema.get("type") == "array":
        return [fill_nulls(item, schema.get("items", {})) for item in data]
    if "anyOf" in schema and data is not None:
        for option in schema["anyOf"]:
            if option.get("type") != "null":
                return fill_nulls(data, option)
    return data


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
BALLOONS = ("speech", "thought", "shout", "whisper", "narration", "sfx")
BEAT_KINDS = ("action", "dialogue", "monologue", "narration", "sfx")
TEXT_BEATS = ("dialogue", "monologue", "narration")

LETTER_KINDS = ("speech", "thought", "shout", "whisper", "narration", "sfx", "title")
# the look of one kind of text: a font (a bundled key from inspect fonts, or a font file's path), a size (times the
# usual) and a weight. Null keeps the default.
LOOK = obj(font=nullable(STR), scale=nullable(NUM), weight=nullable(enum("normal", "bold", "heavy")))

BIBLE_V1 = more(obj(
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
),
    author=nullable(STR),  # (the author's name: the title page carries it)
    lettering=nullable(obj(**{k: nullable(LOOK) for k in LETTER_KINDS})),  # (the book's lettering; null: the defaults)
    props=nullable(arr(obj(id=STR, name=STR, desc=STR, tokens_en=nullable(STR)))),  # (things that look the same every time)
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

NAME_PLAN_V1 = more(obj(
    page=INT,
    turn_role=enum("normal", "reveal", "hook"),
    template=nullable(STR),
    tiers=nullable(arr(obj(h=NUM, cols=arr(obj(slot=STR, w=NUM, rows=nullable(arr(obj(slot=STR, h=NUM)))))))),
    panels=arr(
        more(obj(
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
        ),
            props=nullable(arr(STR)),  # (ids of the bible's props seen in the panel: their references go with the art request)
            bleed=nullable(BOOL),  # (断ち切り: the panel's edges on the page's outer frame run to the paper's edge)
            slant=nullable(NUM),  # (斜め: the border to the next panel in reading order, its two ends apart by this many mm)
            sfx_at=nullable(arr(NUM)),  # (where the sound comes from, [x, y] 0..1 in the panel: the sound effect goes there)
        )
    ),
),
    spread=nullable(BOOL),  # (見開き: this page and the next are drawn as one picture)
    title=nullable(BOOL),  # (the page's first tier makes room for the title and the author's name)
)

SCHEMAS = {"bible@1": BIBLE_V1, "script@1": SCRIPT_V1, "name_plan@1": NAME_PLAN_V1}
