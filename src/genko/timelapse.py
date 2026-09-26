"""The timelapse (タイムラプス): while it is on (set_timelapse), every save leaves a small picture of each page
it changed in studio/timelapse/; the export plays them back as a moving picture (animated WebP, GIF or PNG, or
MP4 when ffmpeg is on the computer), for one page or the whole book in the order the work was done.
"""

from __future__ import annotations

import json
import shutil
import subprocess
import tempfile
import time
from pathlib import Path

from PIL import Image

FOLDER = Path("studio") / "timelapse"
LONG_SIDE = 720  # px: the recorded pictures
MAX_FRAMES = 50_000
FORMATS = ("webp", "gif", "png", "mp4")
MOVIES = FORMATS


def is_on(episode) -> bool:
    return bool((episode.extra.get("timelapse") or {}).get("on"))


def _index(project: Path) -> Path:
    return Path(project) / FOLDER / "index.jsonl"


def frames(project: Path, page: int | None = None) -> list[dict]:
    """The recorded pictures in order: [{n, page, at, file}]."""
    target = _index(project)
    if not target.is_file():
        return []
    out = []
    for line in target.read_text(encoding="utf-8").splitlines():
        try:
            item = json.loads(line)
        except ValueError:
            continue
        if page is None or item.get("page") == page:
            out.append(item)
    return out


def changed_pages(before: dict | None, after: dict) -> list[int]:
    """The page numbers whose content (or words on them) differ between two saves of project.json."""
    old = {p.get("index"): p for p in (before or {}).get("pages", [])}
    lines_old: dict = {}
    lines_new: dict = {}
    for payload, into in ((before or {}, lines_old), (after, lines_new)):
        for line in payload.get("story", []) or []:
            into.setdefault(line.get("page_index"), []).append(line)
    out = []
    for page in after.get("pages", []):
        index = page.get("index")
        if old.get(index) != page or lines_old.get(index) != lines_new.get(index):
            out.append(index)
    return out


def record(project: Path, episode, pages: list[int]) -> list[Path]:
    """A picture of each of these pages as they are now."""
    from genko.render import render_page

    if not pages:
        return []
    folder = Path(project) / FOLDER
    folder.mkdir(parents=True, exist_ok=True)
    known = frames(project)
    if len(known) >= MAX_FRAMES:
        return []
    n = (known[-1]["n"] + 1) if known else 1
    written = []
    with _index(project).open("a", encoding="utf-8") as handle:
        for index in pages:
            page = next((p for p in episode.pages if p.index == index), None)
            if page is None:
                continue
            dpi = max(10, round(LONG_SIDE / (max(page.spec.width_mm, page.spec.height_mm) / 25.4)))
            try:
                image = render_page(page, dpi, mode="proof", episode=episode).convert("RGB")
            except Exception:  # (a picture that cannot be made now must never stop the save)
                continue
            name = f"{n:06d}_p{index:03d}.jpg"
            image.save(folder / name, quality=85)
            handle.write(json.dumps({"n": n, "page": index, "at": time.time(), "file": name}) + "\n")
            written.append(folder / name)
            n += 1
    return written


def clear(project: Path) -> None:
    shutil.rmtree(Path(project) / FOLDER, ignore_errors=True)


def _canvas(images: list[Image.Image]) -> tuple[int, int]:
    return max(i.width for i in images), max(i.height for i in images)


def export(project: Path, dest: Path, *, page: int | None = None, fps: float = 12, seconds: float | None = None,
           fmt: str | None = None, hold: float = 2.0, report: dict | None = None) -> Path:
    """The recorded pictures as a moving picture at `dest` (its suffix, or fmt: webp | gif | png | mp4).
    `seconds` fits the whole recording into that time (frames are dropped evenly); `hold` keeps the finished
    picture on screen a little at the end. `report` gets "frames": the pictures in the file."""
    items = frames(project, page)
    if not items:
        raise ValueError("nothing has been recorded yet (turn the timelapse on and work for a while)")
    dest = Path(dest)
    fmt = (fmt or dest.suffix.lstrip(".") or "webp").lower()
    if fmt == "apng":
        fmt = "png"
    if fmt not in FORMATS:
        raise ValueError(f"format must be one of {', '.join(FORMATS)}")
    if not 1 <= fps <= 60:
        raise ValueError("fps is 1 to 60")
    if seconds:
        wanted = max(2, int(seconds * fps))
        if len(items) > wanted:
            step = len(items) / wanted
            items = [items[int(i * step)] for i in range(wanted - 1)] + [items[-1]]
    folder = Path(project) / FOLDER
    images = []
    for item in items:
        try:
            with Image.open(folder / item["file"]) as img:
                images.append(img.convert("RGB"))
        except OSError:
            continue
    if not images:
        raise ValueError("the recorded pictures are missing")
    size = _canvas(images)
    frames_out = []
    for image in images:
        board = Image.new("RGB", size, (128, 128, 128))
        board.paste(image, ((size[0] - image.width) // 2, (size[1] - image.height) // 2))
        frames_out.append(board)
    return write_movie(frames_out, dest.with_suffix("." + fmt), fps, fmt, hold=hold, report=report)


def write_movie(pictures: list[Image.Image], dest: Path, fps: float, fmt: str, *, hold: float = 0.0,
                loop: bool = True, report: dict | None = None) -> Path:
    """Pictures of one size as a moving picture: animated WebP, GIF or PNG, or MP4 through ffmpeg. `hold` keeps
    the last picture on screen that many seconds more. The same picture twice in a row is one picture shown
    twice as long (the file formats merge them anyway); `report` gets "frames", the pictures in the file."""
    if fmt not in MOVIES:
        raise ValueError(f"format must be one of {', '.join(MOVIES)}")
    dest = Path(dest)
    dest.parent.mkdir(parents=True, exist_ok=True)
    pictures = [p.convert("RGB") for p in pictures]
    duration = max(20, round(1000 / fps))
    if fmt == "mp4":
        pictures = pictures + [pictures[-1]] * max(0, round(hold * fps))
        if report is not None:
            report["frames"] = len(pictures)
        return _mp4(pictures, dest, fps)
    kept: list[Image.Image] = []
    durations: list[int] = []
    for picture in pictures:
        if kept and picture.tobytes() == kept[-1].tobytes():
            durations[-1] += duration
        else:
            kept.append(picture)
            durations.append(duration)
    pictures = kept
    durations[-1] += round(hold * 1000)  # (the finished picture stays a little)
    if report is not None:
        report["frames"] = len(pictures)
    if fmt == "gif":
        pictures = [p.quantize(colors=128, dither=Image.Dither.NONE) for p in pictures]
    extra = {"lossless": False, "quality": 80} if fmt == "webp" else {}
    loops = {"loop": 0} if loop else ({} if fmt == "gif" else {"loop": 1})
    if len(pictures) == 1:
        pictures[0].save(dest, **({"duration": durations[0]} if fmt != "png" else {}))
        return dest
    pictures[0].save(dest, save_all=True, append_images=pictures[1:], duration=durations, **loops, **extra)
    return dest


def ffmpeg() -> str | None:
    return shutil.which("ffmpeg")


def _mp4(images: list[Image.Image], dest: Path, fps: float) -> Path:
    tool = ffmpeg()
    if tool is None:
        raise ValueError("MP4 needs ffmpeg on this computer; WebP, GIF and PNG need nothing")
    with tempfile.TemporaryDirectory() as tmp:
        for i, image in enumerate(images):
            even = image.crop((0, 0, image.width - image.width % 2, image.height - image.height % 2))
            even.save(Path(tmp) / f"{i:06d}.png")
        cmd = [tool, "-y", "-loglevel", "error", "-framerate", str(fps), "-i", str(Path(tmp) / "%06d.png"),
               "-c:v", "libx264", "-pix_fmt", "yuv420p", str(dest)]
        done = subprocess.run(cmd, capture_output=True, text=True)
        if done.returncode != 0:
            raise ValueError(f"ffmpeg failed: {done.stderr.strip()[:300]}")
    return dest
