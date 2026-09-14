from pathlib import Path

from genko.io import load_episode
from genko.models import LayerRole


def test_v1_file_loads_as_layers(tmp_path: Path):
    src = Path("examples/deadline.genko")
    if not src.exists():
        src = tmp_path / "v1.genko"
        src.mkdir()
        (src / "project.json").write_text(
            '{"title":"v1","episode":1,"binding":"right",'
            '"spec":{"width_mm":210,"height_mm":297,"dpi":600,"bleed_mm":3,"inner_margin_mm":10,"expression":"mono"},'
            '"pages":[{"index":1,"note":"","name_ok":false,"stage":"name",'
            '"frames":[{"id":"root","rect":{"x":13,"y":13,"width":184,"height":271},"split_axis":null,"children":[]}],'
            '"fills":{},"name_strokes":[[[10,10],[20,20]]],"ink_strokes":[]}],'
            '"story":[{"id":"a","page_index":1,"text":"hi","speaker":"A","frame_id":null}]}',
            encoding="utf-8",
        )
    ep = load_episode(src)
    page = ep.pages[0]
    roles = [layer.role for layer in page.layers]
    assert LayerRole.NAME in roles
    name = next(layer for layer in page.layers if layer.role == LayerRole.NAME)
    assert name.exportable is False
    assert ep.story_for_page(1)
