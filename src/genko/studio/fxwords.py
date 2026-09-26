"""The effect words a name plan's panels may carry (fx), and what each becomes.

- "effect": lines Genko draws over the art at the finish (集中線, 流線, フラッシュ…)
- "mark": a manga mark (漫符) Genko stamps next to the panel's first reported face (汗, 怒りマーク…)
- "rain": rain streaks Genko draws over the panel at the finish (and the art request asks for rainy weather)
- "art": drawn by the image tool: the words go into the art request (水しぶき, 桜…)

Every word also reaches the art request, so the picture is made for it. A word not in the list is kept, sent to
the art request as it is, and reported as a warning (the agent can use a known word or draw it with apply_ops).
"""

from __future__ import annotations

# word (as written, lower-cased) → (what it becomes, its key, English for the art request)
_TABLE: dict[str, tuple[str, str, str]] = {}


def _add(kind: str, key: str, en: str, *words: str) -> None:
    for word in words:
        _TABLE[word.lower()] = (kind, key, en)


_add("effect", "focus", "focus lines closing in (drawn later by Genko: leave them out)", "集中線", "focus", "focus_lines", "集中")
_add("effect", "speed", "motion (speed lines drawn later by Genko: leave them out)", "流線", "speed", "speed_lines", "スピード線", "効果線")
_add("effect", "white", "a bright flash", "フラッシュ", "white", "flash", "白フラッシュ")
_add("effect", "uni_flash", "a sudden shock (a spiky flash drawn later by Genko)", "ウニフラッシュ", "uni_flash", "ウニフラ")
_add("effect", "beta_flash", "a dark shock (a black flash drawn later by Genko)", "ベタフラッシュ", "beta_flash", "ベタフラ")
_add("rain", "rain", "rainy weather, wet surfaces", "雨", "rain", "rainy", "雨粒", "大雨", "小雨")
_add("mark", "汗", "a nervous sweat drop", "汗", "sweat", "冷や汗", "焦り")
_add("mark", "怒りマーク", "an angry face", "怒り", "anger", "angry", "怒りマーク", "青筋")
_add("mark", "驚き線", "surprise", "驚き", "surprise", "shock", "びっくり")
_add("mark", "ハート", "affection", "ハート", "heart", "love", "恋")
_add("mark", "キラキラ", "sparkles", "キラキラ", "sparkle", "sparkles", "shine", "glitter", "輝き")
_add("mark", "音符", "humming happily", "音符", "music", "humming", "鼻歌")
_add("mark", "ぐるぐる", "dizzy confusion", "ぐるぐる", "dizzy", "confused", "混乱", "目が回る")
_add("mark", "はてなマーク", "puzzlement", "はてな", "question", "疑問", "？", "?")
_add("mark", "ひらめき", "a sudden idea", "ひらめき", "idea", "閃き")
_add("mark", "湯気", "fuming", "湯気", "steam", "fuming", "ぷんぷん")
_add("mark", "動きの線", "trembling", "震え", "trembling", "shake", "shaking", "揺れ")
_add("art", "splash", "water splashing up, flying droplets", "水しぶき", "splash", "しぶき", "水はね")
_add("art", "snow", "falling snow", "雪", "snow", "snowfall")
_add("art", "wind", "strong wind, hair and clothes blown", "風", "wind", "突風")
_add("art", "petals", "cherry blossom petals drifting", "桜", "桜吹雪", "petals", "花びら")
_add("art", "leaves", "falling leaves", "落ち葉", "leaves", "木の葉")
_add("art", "smoke", "smoke", "煙", "smoke")
_add("art", "fire", "fire and flames", "炎", "火", "fire", "flames")
_add("art", "explosion", "an explosion", "爆発", "explosion")
_add("art", "lightning", "lightning", "稲妻", "雷", "lightning")
_add("art", "fog", "fog, mist", "霧", "もや", "fog", "mist")
_add("art", "tears", "tears", "涙", "tears", "泣き")
_add("art", "light", "light rays, backlight", "光", "逆光", "光線", "light rays", "backlight")
_add("art", "stars", "a starry sky", "星空", "starry sky")
_add("art", "dust", "a cloud of dust", "土煙", "砂煙", "dust")
_add("art", "impact", "an impact", "衝撃", "impact")

KNOWN = sorted({word for word in _TABLE if not word.isascii()})  # (the Japanese words, for the agent)


def resolve(word: str) -> dict | None:
    """{kind, key, en} for a known word, None for an unknown one."""
    found = _TABLE.get(str(word).strip().lower())
    if found is None:
        return None
    kind, key, en = found
    return {"kind": kind, "key": key, "en": en}


def art_words(words: list) -> tuple[list[str], list[str]]:
    """(Japanese, English) for the art request: every word of the panel, known or not."""
    ja: list[str] = []
    en: list[str] = []
    for word in words or []:
        text = str(word).strip()
        if not text:
            continue
        found = resolve(text)
        ja.append(text)
        en.append(found["en"] if found else f"{text} (in Japanese)")
    return ja, en


def emphasis_words(emphasis) -> tuple[str, str] | None:
    """The panel's weight in the story (0..1) as words for the art request; None for an ordinary panel."""
    try:
        value = float(emphasis)
    except (TypeError, ValueError):
        return None
    if value >= 0.8:
        return ("この話の山場。迫力のある大胆な構図にする。", "A climax of the story: a bold, dramatic, striking composition.")
    if value >= 0.6:
        return ("強調するコマ。印象に残る構図にする。", "An emphasised panel: a memorable composition.")
    if value <= 0.2:
        return ("静かな間のコマ。控えめな構図にする。", "A quiet pause: an understated composition.")
    return None
