"""A small JSON Schema checker for the subset our input schemas use.

Supported: type (incl. integer vs number, bool is not a number), properties,
required, additionalProperties false, items, enum, const, anyOf.
"""

from __future__ import annotations

from typing import Any

from genko.studio.issues import Issue, error

_TYPES = {
    "object": lambda v: isinstance(v, dict),
    "array": lambda v: isinstance(v, list),
    "string": lambda v: isinstance(v, str),
    "boolean": lambda v: isinstance(v, bool),
    "null": lambda v: v is None,
    "integer": lambda v: isinstance(v, int) and not isinstance(v, bool),
    "number": lambda v: isinstance(v, (int, float)) and not isinstance(v, bool),
}

_ALLOWED_KEYS = {
    "type", "properties", "required", "additionalProperties", "items", "enum", "const",
    "anyOf", "description", "title", "$schema", "$id",
}


def _escape(key: str) -> str:
    return key.replace("~", "~0").replace("/", "~1")


def validate(data: Any, schema: dict, path: str = "") -> list[Issue]:
    issues: list[Issue] = []
    _check(data, schema, path, issues)
    return issues


def _check(value: Any, schema: dict, path: str, issues: list[Issue]) -> None:
    where = path or "/"
    if "anyOf" in schema:
        for option in schema["anyOf"]:
            if not validate(value, option, path):
                return
        issues.append(error("schema_any_of", where, "どの型にも合わない値です", _describe(schema)))
        return
    if "const" in schema and value != schema["const"]:
        issues.append(error("schema_const", where, f"値は {schema['const']!r} でなければならない"))
        return
    if "enum" in schema and value not in schema["enum"]:
        issues.append(error("schema_enum", where, f"{value!r} は使えない値です", "使える値: " + ", ".join(map(str, schema["enum"]))))
        return
    expected = schema.get("type")
    if expected and not _TYPES[expected](value):
        issues.append(error("schema_type", where, f"型が違う（{expected} が必要）"))
        return
    if expected == "object":
        props = schema.get("properties", {})
        for key in schema.get("required", []):
            if key not in value:
                issues.append(error("schema_required", f"{path}/{_escape(key)}", f"{key} がない"))
        if schema.get("additionalProperties") is False:
            for key in value:
                if key not in props:
                    issues.append(error("schema_unknown_key", f"{path}/{_escape(key)}", f"{key} は定義されていない項目です"))
        for key, sub in props.items():
            if key in value:
                _check(value[key], sub, f"{path}/{_escape(key)}", issues)
    elif expected == "array" and "items" in schema:
        for index, item in enumerate(value):
            _check(item, schema["items"], f"{path}/{index}", issues)


def _describe(schema: dict) -> str:
    parts = []
    for option in schema.get("anyOf", []):
        parts.append(str(option.get("type", option.get("const", "?"))))
    return "使える型: " + " / ".join(parts)


def conservative_problems(schema: Any, path: str = "") -> list[str]:
    """Return why a schema is not usable as tool input by every LLM (empty if fine)."""
    problems: list[str] = []
    if isinstance(schema, dict):
        for key in schema:
            if key not in _ALLOWED_KEYS:
                problems.append(f"{path or '/'}: {key} は使わない")
        if schema.get("type") == "object":
            props = schema.get("properties", {})
            if schema.get("additionalProperties") is not False:
                problems.append(f"{path or '/'}: additionalProperties は false にする")
            if sorted(schema.get("required", [])) != sorted(props):
                problems.append(f"{path or '/'}: required は全項目にする（任意項目は null を許す）")
            for key, sub in props.items():
                problems.extend(conservative_problems(sub, f"{path}/properties/{key}"))
        if "items" in schema:
            problems.extend(conservative_problems(schema["items"], f"{path}/items"))
        for index, option in enumerate(schema.get("anyOf", [])):
            problems.extend(conservative_problems(option, f"{path}/anyOf/{index}"))
    return problems
