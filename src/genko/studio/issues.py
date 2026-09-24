from __future__ import annotations

from dataclasses import asdict, dataclass


@dataclass(frozen=True)
class Issue:
    """One finding about agent input. `path` is a JSON pointer into that input."""

    code: str
    severity: str  # "error" blocks a commit, "warning" is reported only
    path: str
    message: str
    hint: str = ""

    def to_dict(self) -> dict:
        return asdict(self)


def error(code: str, path: str, message: str, hint: str = "") -> Issue:
    return Issue(code, "error", path, message, hint)


def warning(code: str, path: str, message: str, hint: str = "") -> Issue:
    return Issue(code, "warning", path, message, hint)


def has_errors(issues: list[Issue]) -> bool:
    return any(issue.severity == "error" for issue in issues)
