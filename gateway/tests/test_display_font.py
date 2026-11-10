"""Offline validation of the font embedded by the device, not a cloud/UI test."""

import gzip
from pathlib import Path


FONT_DIR = Path(__file__).resolve().parents[2] / "firmware" / "fonts"


def test_display_font_binary_matches_editable_source_and_covers_hint_text():
    source = gzip.decompress((FONT_DIR / "icebreaker_text16.hex.gz").read_bytes()).decode("ascii")
    glyphs = {}
    packed = bytearray()
    for line in source.splitlines():
        code, bitmap = line.split(":")
        data = bytes.fromhex(bitmap)
        glyphs[int(code, 16)] = data
        if len(data) == 16:
            data = b"".join(bytes([row, 0]) for row in data)
        assert len(data) == 32
        packed.extend(data)
    assert bytes(packed) == (FONT_DIR / "icebreaker_text16.bin").read_bytes()
    assert len(packed) == 688160
    for character in "最近有没有一件让你觉得有趣的小事？新话题·仅文字提示轻提示已开启收起聆听中语音上传云端":
        assert ord(character) in glyphs
        assert any(glyphs[ord(character)])
    assert list(glyphs) == sorted(glyphs)
