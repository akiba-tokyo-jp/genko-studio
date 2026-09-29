"""B: balloons and lettering as a letterer sets them: the balloon hugs the words (a fuller oval around their own
shape), the columns break between phrases, a spoken line drops its closing 。, the outline swells and thins, tails
are short and broad (none where the speaker is plain), balloons may run over the panel's border, and one speaker's
lines in a row run together."""

from genko.balloons import block_points, ellipse_around, text_layout
from genko.models import PageSpec, StoryLine, new_episode
from genko.ops import apply_ops
from genko.render import render_page
from genko.studio.blocking import Figure
from genko.studio.letter import EM_MM, Room, measure, obvious, tail_to
from genko.tategaki import phrase_columns, phrases, without_periods


def test_columns_break_between_phrases():
    assert phrase_columns("まんが作りのモヤモヤをズバッと解決するこのコーナー！", 8) == \
        ["まんが作りの", "モヤモヤを", "ズバッと解決する", "このコーナー！"]
    assert phrase_columns("いつも読んでくださってありがとうございます！", 8) == ["いつも", "読んでくださって", "ありがとう", "ございます！"]
    assert phrase_columns("テンポとは読んでるときの心地よさのこと", 7) == ["テンポとは", "読んでるときの", "心地よさのこと"]
    assert phrase_columns("まだ決着はついてない!!", 8) == ["まだ決着はつい", "てない!!"]  # (a long phrase in even parts)
    assert all(not col.startswith(("、", "。", "っ")) for col in phrase_columns("そっち、ちょっと、待ってってば。", 3))
    assert phrases("そっち、混ぜて") == ["そっち、", "混ぜて"]


def test_a_spoken_line_drops_its_closing_period():
    assert without_periods("そっち、混ぜて。") == "そっち、混ぜて"
    assert without_periods("はい。そうです。") == "はい\nそうです"
    assert without_periods("「はい。」") == "「はい」"
    assert without_periods("えっ？") == "えっ？"


def test_the_balloon_hugs_the_words():
    cols = ["まんが作りの", "モヤモヤを", "ズバッと解決する", "このコーナー！"]
    w, h = measure(cols, "speech")
    block_w, block_h = EM_MM * 4 + EM_MM * 0.4 * 3, EM_MM * 8
    old = (block_w * 2 ** 0.5 + EM_MM / 2) * (block_h * 2 ** 0.5 + EM_MM / 2)  # (the ellipse around the box's corners)
    assert w > block_w and h > block_h and w * h < old * 0.85
    ragged = [(0, 0, 10, 12), (14, 0, 24, 40), (28, 0, 38, 12)]  # (a long column between two short ones, from the top)
    rw, rh, _dx, _dy = ellipse_around(block_points(ragged, 0), 2.6)
    fw, fh, _dx, _dy = ellipse_around(block_points([(0, 0, 38, 40)], 0), 2.6)
    assert rw * rh < fw * fh * 0.97  # (the short columns' empty feet are not wrapped)
    ew, eh, _dx, _dy = ellipse_around(block_points([(0, 0, 38, 40)], 0), 2.0)
    assert fw * fh < ew * eh * 0.9  # (a letterer's fuller oval needs less room than an ellipse)


def _line(text, **style):
    return StoryLine(id="l1", page_index=1, speaker="", text=text, balloon="speech", wrap="vertical",
                     x_mm=20, y_mm=20, w_mm=0, h_mm=0, style=style)


def test_the_letters_keep_their_size_in_a_measured_balloon_and_sit_inside():
    cols = ["まんが作りの", "モヤモヤを", "ズバッと解決する", "このコーナー！"]
    w, h = measure(cols, "speech")
    line = _line("\n".join(cols))
    line.w_mm, line.h_mm = w, h
    image, em, corner = text_layout(line, 150)
    assert em == round(EM_MM / 25.4 * 150)  # (not shrunk)
    assert -w / 25.4 * 150 / 2 < corner[0] and corner[0] + image.width < w / 25.4 * 150 / 2 + 1


def test_the_outline_swells_and_the_oval_is_uneven_unless_asked_for_an_even_one():
    ep = new_episode("t", 1, 1, PageSpec.b5_doujin())
    apply_ops(ep, [{"op": "add_line", "page": 1, "text": "こんにちは", "balloon": "speech", "wrap": "vertical", "x_mm": 60,
                    "y_mm": 60, "w_mm": 24, "h_mm": 40, "id": "a"}])
    hand = render_page(ep.pages[0], 100, mode="print", episode=ep).convert("L")
    apply_ops(ep, [{"op": "edit_line", "id": "a", "style": {"hand": False}}])
    even = render_page(ep.pages[0], 100, mode="print", episode=ep).convert("L")
    assert hand.tobytes() != even.tobytes()
    apply_ops(ep, [{"op": "edit_line", "id": "a", "text": "はい。", "style": {"periods": True}}])
    kept = render_page(ep.pages[0], 100, mode="print", episode=ep).convert("L")
    apply_ops(ep, [{"op": "edit_line", "id": "a", "style": {"periods": None}}])
    dropped = render_page(ep.pages[0], 100, mode="print", episode=ep).convert("L")
    assert kept.tobytes() != dropped.tobytes()


def test_tails_are_short_and_left_off_where_the_speaker_is_plain():
    box = (100.0, 20.0, 20.0, 40.0)
    head = (40.0, 90.0, 20.0, 20.0)  # (far down to the left)
    tip = tail_to(box, head)
    cx, cy = 110.0, 40.0
    reach = ((tip[0] - cx) ** 2 + (tip[1] - cy) ** 2) ** 0.5
    assert reach < 20 + 8.5  # (it points at the mouth and stops short, not a long needle to it)
    alone = Figure("a", (60.0, 60.0, 20.0, 20.0), (55.0, 80.0, 30.0, 60.0))
    other = Figure("b", (150.0, 60.0, 20.0, 20.0), (145.0, 80.0, 30.0, 60.0))
    assert obvious((85.0, 40.0, 20.0, 30.0), alone, [alone])
    assert not obvious((110.0, 40.0, 20.0, 30.0), alone, [alone, other])


def test_a_balloon_may_run_over_the_border_but_never_into_another_panel():
    room = Room((0.0, 0.0, 200.0, 280.0), [(102.0, 2.0, 96.0, 96.0)], [])
    assert room.allows((70.0, -3.0, 25.0, 40.0))  # (over the top border, into the margin)
    assert not room.allows((95.0, 10.0, 25.0, 40.0))  # (into the panel beside it)


def test_one_speakers_lines_in_a_row_run_together():
    from genko.studio import layout
    from genko.studio.letter import place_page

    ep = new_episode("t", 1, 1, PageSpec.b5_doujin())
    plan = {"page": 1, "tiers": [{"h": 1.0, "cols": [{"slot": "p1", "w": 1.0, "rows": None}]}],
            "panels": [{"slot": "p1", "shot": "MS", "characters": [{"id": "a", "pos": "left"}, {"id": "b", "pos": "right"}],
                        "lines": [{"beat_id": "b1", "balloon": "speech", "breaks": ["ねえ", "聞いてる？"]},
                                  {"beat_id": "b2", "balloon": "speech", "breaks": ["ちょっと", "待ってよ"]}]}]}
    compiled = layout.apply_layout(ep, 1, plan, agent="ai:test")
    placements, issues = place_page(plan, compiled, {"characters": [{"id": "a", "name": "A"}, {"id": "b", "name": "B"}]},
                                    {"b1": "a", "b2": "a"})
    assert len(placements) == 2
    first, second = placements
    assert first.style and second.style and first.style["group"] == second.style["group"]
    assert second.tail is None
    assert second.x_mm < first.x_mm and second.x_mm + second.w_mm > first.x_mm  # (to its left, the two overlapping)
