"""The manga style catalog (マンガの絵柄カタログ, https://manga.akiba.tokyo.jp): read a style and keep it in the book.

The catalog is a tree of three levels (genre → sub-genre → finished style). Each branch has a prompt (ja, en, tags),
things never to draw (avoid) and a sample picture. A book keeps a copy of the chosen branch — its words, its version
and its sample (as an asset) — so the book's style does not move when the site changes; `newer` only tells that the
site has a newer version.

Only reading, over HTTPS GET. GENKO_STYLE_CATALOG changes the address, GENKO_STYLE_CATALOG_TOKEN is sent as a bearer
token when the catalog is not public.
"""

from __future__ import annotations

import io
import json
import os
import re
import urllib.error
import urllib.request
from datetime import datetime, timezone
from urllib.parse import parse_qs, quote, urlparse

BASE = "https://manga.akiba.tokyo.jp"
TIMEOUT_S = 15
ID_RE = re.compile(r"^[a-z0-9][a-z0-9-]{0,120}$")
LINK_SCHEME = "genko"


class CatalogError(ValueError):
    pass


def base() -> str:
    return (os.environ.get("GENKO_STYLE_CATALOG") or BASE).rstrip("/")


def page_url(style_id: str) -> str:
    """The branch's page on the site (for a person to look at)."""
    return f"{base()}/n/{quote(style_id)}?lang=ja"


def _get(url: str) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": "GenkoStudio", "Accept": "application/json, image/*"})
    token = os.environ.get("GENKO_STYLE_CATALOG_TOKEN")
    if token:
        request.add_header("Authorization", f"Bearer {token}")
    try:
        with urllib.request.urlopen(request, timeout=TIMEOUT_S) as reply:  # noqa: S310 (https, a fixed site)
            return reply.read()
    except urllib.error.HTTPError as exc:
        if exc.code == 404:
            raise CatalogError("絵柄カタログにその絵柄がない（id を確かめる）") from exc
        if exc.code == 401:
            raise CatalogError("絵柄カタログが限定公開になっている（GENKO_STYLE_CATALOG_TOKEN が要る）") from exc
        raise CatalogError(f"絵柄カタログが答えなかった（HTTP {exc.code}）") from exc
    except (urllib.error.URLError, TimeoutError, OSError) as exc:
        raise CatalogError("絵柄カタログにつながらない（インターネットの接続を確かめる）") from exc


def _json(path: str) -> dict:
    try:
        data = json.loads(_get(base() + path).decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise CatalogError("絵柄カタログの返事が読めない") from exc
    if not isinstance(data, dict):
        raise CatalogError("絵柄カタログの返事が読めない")
    return data


def check_id(style_id: str) -> str:
    style_id = str(style_id or "").strip()
    if not ID_RE.match(style_id):
        raise CatalogError(f"絵柄の id「{style_id}」は使えない（英小文字・数字・ハイフン）")
    return style_id


def tree() -> list[dict]:
    """Every published branch: {id, parent, level, title, summary, thumbnail_url}."""
    nodes = _json("/v1/tree").get("nodes")
    if not isinstance(nodes, list):
        raise CatalogError("絵柄カタログの一覧が読めない")
    return [n for n in nodes if isinstance(n, dict) and n.get("id")]


def style(style_id: str) -> dict:
    """One branch as the catalog has it now (with its children)."""
    data = _json(f"/v1/styles/{check_id(style_id)}")
    if not isinstance(data.get("prompt"), dict) or not data["prompt"].get("ja"):
        raise CatalogError("この絵柄には言葉（prompt）が無い")
    return data


def sample_png(data: dict, fetch=None) -> bytes | None:
    """The branch's best sample (samples[reference]) as PNG, or None when it has none."""
    samples = data.get("samples") or []
    index = data.get("reference")
    index = index if isinstance(index, int) and 0 <= index < len(samples) else 0
    if not samples or not isinstance(samples[index], dict) or not samples[index].get("url"):
        return None
    raw = (fetch or _get)(str(samples[index]["url"]))
    from PIL import Image

    try:
        picture = Image.open(io.BytesIO(raw))
        picture.load()
    except Exception as exc:  # noqa: BLE001 (any unreadable picture)
        raise CatalogError("絵柄の見本の絵が読めない") from exc
    out = io.BytesIO()
    picture.convert("L" if data.get("expression") == "mono" else "RGB").save(out, format="PNG", optimize=True)
    return out.getvalue()


def saved(data: dict, sample_asset: str | None) -> dict:
    """What the book keeps of a branch (its words, what it never draws, where it came from, its version)."""
    prompt = data.get("prompt") or {}
    samples = data.get("samples") or []
    index = data.get("reference") if isinstance(data.get("reference"), int) else 0
    sample_url = samples[index].get("url") if 0 <= index < len(samples) and isinstance(samples[index], dict) else None
    return {
        "id": str(data["id"]),
        "title": str(data.get("title") or data["id"]),
        "path": [str(p.get("title") or p.get("id")) for p in data.get("path") or [] if isinstance(p, dict)],
        "summary": str(data.get("summary") or ""),
        "level": data.get("level"),
        "expression": str(data.get("expression") or "mono"),
        "prompt": {"ja": str(prompt.get("ja") or ""), "en": str(prompt.get("en") or ""), "tags": str(prompt.get("tags") or "")},
        "avoid": [str(a) for a in data.get("avoid") or []],
        "sample": sample_asset,
        "sample_url": sample_url,
        "version": data.get("version"),
        "updated_at": data.get("updated_at"),
        "terms": str((data.get("license") or {}).get("terms") or ""),
        "source": f"{base()}/v1/styles/{data['id']}",
        "taken_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    }


def take(style_id: str, store) -> dict:
    """Read a branch and its sample now, put the sample in the book's assets: the `catalog` for set_style_catalog."""
    data = style(style_id)
    png = sample_png(data)
    return saved(data, store.put_bytes(png, ".png") if png else None)


def newer(kept: dict) -> dict | None:
    """The catalog's version of the book's style when it is newer than the book's copy (None when the same)."""
    now = style(kept["id"])
    if now.get("version") == kept.get("version") and now.get("updated_at") == kept.get("updated_at"):
        return None
    return {"id": now["id"], "version": now.get("version"), "updated_at": now.get("updated_at"),
            "kept_version": kept.get("version")}


def style_from_link(link: str) -> str | None:
    """genko://use-style?id=… → the id (None for other links)."""
    parts = urlparse(str(link or ""))
    if parts.scheme != LINK_SCHEME or (parts.netloc or parts.path.strip("/")) != "use-style":
        return None
    found = (parse_qs(parts.query).get("id") or [""])[0]
    return check_id(found)


def validate(catalog: dict) -> dict:
    """The kept branch as an op carries it (checked, nothing else)."""
    if not isinstance(catalog, dict):
        raise CatalogError("catalog はオブジェクト")
    check_id(catalog.get("id", ""))
    prompt = catalog.get("prompt")
    if not isinstance(prompt, dict) or not str(prompt.get("ja") or "").strip():
        raise CatalogError("catalog.prompt.ja が要る")
    if catalog.get("sample") is not None and not str(catalog["sample"]).startswith("sha256:"):
        raise CatalogError("catalog.sample は sha256:… の asset")
    keys = ("id", "title", "path", "summary", "level", "expression", "prompt", "avoid", "sample", "sample_url", "version",
            "updated_at", "terms", "source", "taken_at")
    return {k: catalog[k] for k in keys if k in catalog}
