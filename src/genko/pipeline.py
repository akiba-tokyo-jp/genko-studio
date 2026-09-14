from __future__ import annotations

from genko.models import Page


class InkBlockedError(RuntimeError):
    """Ink starts only after the name gate (G2) is marked OK."""


def advance(page: Page, to: str) -> None:
    if to == "ink" and not page.name_ok:
        raise InkBlockedError("name is not OK")
    page.stage = to
