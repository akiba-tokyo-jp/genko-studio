import struct
from pathlib import Path

from genko.export import export_psd
from genko.models import PageSpec, new_episode
from genko.ops import apply_ops


def _psd_layer_count(data: bytes) -> int:
    offset = 26
    color_len = struct.unpack(">I", data[offset : offset + 4])[0]
    offset += 4 + color_len
    res_len = struct.unpack(">I", data[offset : offset + 4])[0]
    offset += 4 + res_len
    layer_section_len = struct.unpack(">I", data[offset : offset + 4])[0]
    if layer_section_len == 0:
        return 0
    offset += 4
    _info_len = struct.unpack(">I", data[offset : offset + 4])[0]
    offset += 4
    return abs(struct.unpack(">h", data[offset : offset + 2])[0])


def test_psd_layer_count_matches_text_runs_plus_composite(tmp_path: Path):
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    apply_ops(
        ep,
        [
            {"op": "add_line", "page": 1, "text": "今だ", "speaker": "A"},
            {"op": "add_line", "page": 1, "text": "待て", "speaker": "B"},
        ],
    )
    path = export_psd(ep, tmp_path / "out.psd", dpi=36)
    count = _psd_layer_count(path.read_bytes())
    assert count == 1 + 2
