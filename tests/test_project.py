from genko.models import Binding, LayerRole, PageSpec, new_episode


def test_new_episode_creates_requested_page_count():
    ep = new_episode(title="試作", episode=1, page_count=8, spec=PageSpec.a4_mono())
    assert ep.title == "試作"
    assert ep.episode == 1
    assert len(ep.pages) == 8
    assert [p.index for p in ep.pages] == list(range(1, 9))


def test_a4_page_has_bleed_trim_and_inner_frame():
    spec = PageSpec.a4_mono()
    assert spec.width_mm == 210
    assert spec.height_mm == 297
    assert spec.dpi == 600
    assert spec.bleed_mm == 3
    page = new_episode("t", 1, 1, spec).pages[0]
    inner = page.inner_rect_mm()
    assert inner.x == spec.bleed_mm + spec.inner_margin_mm
    assert inner.y == spec.bleed_mm + spec.inner_margin_mm
    assert inner.width < spec.width_mm
    assert inner.height < spec.height_mm


def test_reorder_pages_renumbers():
    ep = new_episode("t", 1, 3, PageSpec.a4_mono())
    ep.pages[0].note = "was-first"
    ep.reorder([3, 1, 2])
    assert [p.note for p in ep.pages] == ["", "was-first", ""]
    assert [p.index for p in ep.pages] == [1, 2, 3]


def test_binding_right_makes_odd_pages_recto():
    ep = new_episode("t", 1, 4, PageSpec.a4_mono(), binding=Binding.RIGHT)
    assert ep.pages[0].is_recto() is True
    assert ep.pages[1].is_recto() is False
