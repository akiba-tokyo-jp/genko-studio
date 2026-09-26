"""Making adopted art big enough to print (M2): Genko's own enlargement (Lanczos, then the line edges firmed up), or
an upscaler program the person registered on this computer. Neither adds detail that was not drawn; the result is
marked as enlarged so the pre-press check can say so.

Upscalers (<config>/upscalers.json, written only by a person with `genko studio upscaler`):
  {"<name>": {"command": ["realesrgan-ncnn-vulkan", "-i", "{in}", "-o", "{out}", "-s", "{scale}"], "scales": [2, 4],
              "label": "…"}}
{in} and {out} are PNG files Genko makes and reads; {scale} is the enlargement asked for.
"""

from __future__ import annotations

import json
import shlex
import subprocess
import tempfile
from pathlib import Path

from PIL import Image, ImageFilter

from genko.tokens import config_dir

BUILTIN = "genko"
MAX_SCALE = 4.0
TIMEOUT_S = 900


def _file() -> Path:
    return config_dir() / "upscalers.json"


def registered() -> dict[str, dict]:
    try:
        data = json.loads(_file().read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    return {k: v for k, v in data.items() if isinstance(v, dict) and isinstance(v.get("command"), list)} if isinstance(data, dict) else {}


def available() -> list[dict]:
    """The enlargers an agent can name: Genko's own and the person's."""
    out = [{"name": BUILTIN, "label": "Genko の拡大（Lanczos と線の輪郭の整え。描き込みは増えない）", "scales": "1〜4"}]
    for name, spec in sorted(registered().items()):
        out.append({"name": name, "label": spec.get("label") or name, "scales": spec.get("scales") or "any"})
    return out


def register(name: str, command: str | list[str], scales: list[float] | None = None, label: str = "") -> dict:
    """(A person's command) Remember an upscaler program. The command needs {in} and {out}."""
    if not name or name == BUILTIN or not all(ch.isalnum() or ch in "-_." for ch in name):
        raise ValueError(f"名前は英数字と - _ .（{BUILTIN} 以外）")
    parts = shlex.split(command) if isinstance(command, str) else [str(p) for p in command]
    if not parts or not any("{in}" in p for p in parts) or not any("{out}" in p for p in parts):
        raise ValueError("コマンドには {in}（元の画像）と {out}（拡大した画像）を入れる")
    data = registered()
    data[name] = {"command": parts, **({"scales": [float(s) for s in scales]} if scales else {}), **({"label": label} if label else {})}
    _file().parent.mkdir(parents=True, exist_ok=True)
    _file().write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return data[name]


def unregister(name: str) -> bool:
    data = registered()
    if name not in data:
        return False
    del data[name]
    _file().write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return True


def builtin(image: Image.Image, scale: float) -> Image.Image:
    """Lanczos enlargement, then an unsharp mask sized to the enlargement: soft line edges become firm again."""
    mode = "RGBA" if image.mode in ("RGBA", "LA", "P") else ("L" if image.mode in ("L", "1") else "RGB")
    source = image.convert(mode)
    size = (max(1, round(source.width * scale)), max(1, round(source.height * scale)))
    big = source.resize(size, Image.LANCZOS)
    if scale > 1.05:
        sharpen = ImageFilter.UnsharpMask(radius=max(1.0, scale * 0.7), percent=90, threshold=2)
        if mode == "RGBA":
            rgb, alpha = big.convert("RGB").filter(sharpen), big.getchannel("A")
            big = rgb.convert("RGBA")
            big.putalpha(alpha)
        else:
            big = big.filter(sharpen)
    return big


def external(name: str, image: Image.Image, scale: float) -> Image.Image:
    """Run the person's upscaler on the picture. Its output is resized to exactly `scale` (programs round)."""
    spec = registered().get(name)
    if spec is None:
        raise ValueError(f"高解像度化の道具 {name} は登録されていない（{', '.join(a['name'] for a in available())}）")
    with tempfile.TemporaryDirectory(prefix="genko-up-") as tmp:
        src, dst = Path(tmp) / "in.png", Path(tmp) / "out.png"
        image.save(src)
        scale_word = str(int(scale)) if float(scale).is_integer() else f"{scale:g}"
        cmd = [part.replace("{in}", str(src)).replace("{out}", str(dst)).replace("{scale}", scale_word) for part in spec["command"]]
        try:
            done = subprocess.run(cmd, capture_output=True, text=True, timeout=TIMEOUT_S)
        except (OSError, subprocess.TimeoutExpired) as exc:
            raise ValueError(f"{name} を動かせなかった（{exc}）") from exc
        if done.returncode != 0 or not dst.is_file():
            raise ValueError(f"{name} が失敗した: {(done.stderr or done.stdout).strip()[:300]}")
        with Image.open(dst) as out:
            result = out.convert(image.mode if image.mode in ("RGB", "RGBA", "L") else "RGB")
    wanted = (max(1, round(image.width * scale)), max(1, round(image.height * scale)))
    return result if result.size == wanted else result.resize(wanted, Image.LANCZOS)


def enlarge(image: Image.Image, scale: float, method: str = BUILTIN) -> Image.Image:
    if not 1.0 <= scale <= MAX_SCALE:
        raise ValueError(f"scale は 1〜{MAX_SCALE:g}")
    return builtin(image, scale) if method == BUILTIN else external(method, image, scale)
