"""Remember which stroke lists were saved as (or loaded from) which asset.

A thick book has tens of thousands of strokes. Writing them all out again on every save, and reading
them all back on every undo, made one change on a 32-page book take seconds. Stroke objects are never
changed in place once a batch of ops is done (ops work on copies), so a list of the same stroke objects
is the same blob: saving reuses the ref, and loading a ref hands back the strokes read last time.
"""

from __future__ import annotations

from collections import OrderedDict

MAX_BLOBS = 4096

_by_ref: OrderedDict[str, tuple] = OrderedDict()
_by_ids: dict[tuple[int, ...], str] = {}


def _key(strokes) -> tuple[int, ...]:
    return tuple(map(id, strokes))


def ref_for(strokes) -> str | None:
    """The asset these exact stroke objects were saved as, if known."""
    ref = _by_ids.get(_key(strokes))
    if ref is None:
        return None
    kept = _by_ref.get(ref)
    if kept is None or len(kept) != len(strokes) or any(a is not b for a, b in zip(kept, strokes)):
        return None
    _by_ref.move_to_end(ref)
    return ref


def strokes_for(ref: str) -> list | None:
    """A new list of the strokes last saved as or loaded from ref."""
    kept = _by_ref.get(ref)
    if kept is None:
        return None
    _by_ref.move_to_end(ref)
    return list(kept)


def remember(ref: str, strokes) -> None:
    old = _by_ref.pop(ref, None)
    if old is not None:
        _by_ids.pop(_key(old), None)
    kept = tuple(strokes)
    _by_ref[ref] = kept  # holds the strokes, so their ids stay theirs
    _by_ids[_key(kept)] = ref
    while len(_by_ref) > MAX_BLOBS:
        _, dropped = _by_ref.popitem(last=False)
        _by_ids.pop(_key(dropped), None)


def clear() -> None:
    _by_ref.clear()
    _by_ids.clear()
